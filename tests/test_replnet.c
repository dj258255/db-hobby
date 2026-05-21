#include "replnet.h"
#include "replica.h"
#include "wal.h"
#include "pager.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * 트랙 H1 후속 — WAL 로그 시핑을 '소켓'으로 실어 나르는 단위 테스트.
 *
 * 25편은 primary가 로컬에 십해 둔 WAL 파일을 replica가 tail했다(파일 공유 흉내).
 * 여기선 진짜로 바이트를 소켓으로 스트리밍한다:
 *   SENDER 스레드   : primary를 구동(커밋)하고 walsender_send로 새 WAL을 소켓에 흘림.
 *   RECEIVER 스레드 : walreceiver_recv로 로컬 로그에 append한 뒤 replica_apply.
 * replica.c는 한 줄도 안 고쳤다 — 네트워크 계층은 바이트만 옮기고, redo의 정확성은
 * 기존 replica_apply(lsn 게이트 + 페이지 전체 물리 로깅)가 그대로 책임진다.
 *
 * 전송로: socketpair(AF_UNIX, SOCK_STREAM). 프로세스 내 양방향 신뢰 스트림이라
 * 포트 flakiness가 없다. 실제 TCP(AF_INET localhost) listener/accept로 바꿔도
 * send/recv가 흘리는 '바이트'는 완전히 동일하다 — 소켓은 그저 파이프다.
 *
 * 결정성: 라운드마다 sender가 1프레임 보내고, receiver가 받아 apply한 뒤 1바이트
 * ack를 되돌린다. sender는 ack를 받고서야 다음 라운드를 커밋한다(락스텝). 그래서
 * "이번 라운드에 몇 커밋이 적용됐나"가 라운드 경계로 딱 갈려 검증이 확정적이다.
 * (이 ack는 테스트 동기화용일 뿐, walsender_send 자체는 ack를 안 기다린다 = 비동기.)
 * 락스텝 결과(라운드별 적용 수/복제 위치)는 공유 구조체에 적어두고, 스레드 join
 * 후 MAIN이 단일 스레드로 CHECK한다 — failures 카운터에 경쟁이 없다.
 */

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } } while (0)

#define PRIMARY_DB "build/test_replnet_primary.db"
#define PRIMARY_WAL "build/test_replnet_primary.wal"
#define REPLICA_DB "build/test_replnet_replica.db"
#define REPLICA_WAL "build/test_replnet_replica.wal" /* receiver의 로컬 로그 */

static void cleanup(void) {
    unlink(PRIMARY_DB);
    unlink(PRIMARY_WAL);
    unlink(REPLICA_DB);
    unlink(REPLICA_WAL);
}

#define NROUNDS 3

/* 스레드가 공유하는 상태. sender/receiver는 서로 겹치지 않는 필드만 쓰고,
 * ack 락스텝이 라운드 순서를 강제하므로 데이터 경쟁이 없다. */
typedef struct {
    int sender_sock;   /* sv[0] */
    int receiver_sock; /* sv[1] */

    Wal *primary;      /* sender가 구동 */
    WalReplica *rep;   /* receiver가 구동 */
    int recv_log_fd;   /* receiver가 로컬 로그에 append할 쓰기 fd. rep->log_fd는
                        * replica_open이 O_RDONLY로 연 '읽기' fd라 apply 전용이고,
                        * 받은 바이트 쓰기는 이 별도 쓰기 fd로 한다(같은 파일).
                        * 실제 walreceiver(쓰기)/startup replay(읽기) 분리와 같다. */

    /* 라운드별 결과(receiver가 기록, join 후 main이 읽음) */
    int applied[NROUNDS];      /* 그 라운드에서 replica_apply가 적용한 커밋 수 */
    uint64_t position[NROUNDS];/* 라운드 직후 복제 위치(applied_lsn) */
    int recv_bytes[NROUNDS];   /* 그 라운드에서 소켓으로 받은 바이트 수 */
    int saw_extra_eof;         /* 마지막 라운드 뒤 recv가 0(EOF)을 줬는가 */
    int recv_error;            /* receiver 경로에서 오류가 났는가 */
} Shared;

/* primary에 pid 페이지를 val로 채워 한 트랜잭션으로 커밋한다. */
static void primary_commit_page(Wal *w, page_id_t pid, uint8_t val) {
    uint8_t page[PAGE_SIZE];
    memset(page, val, PAGE_SIZE);
    wal_begin(w);
    wal_stage(w, pid, page);
    wal_commit(w);
}

/* 라운드별로 이번에 커밋할 페이지들(pid,val). 두 스레드가 같은 대본을 공유. */
static const struct { int npages; page_id_t pid[2]; uint8_t val[2]; } script[NROUNDS] = {
    { 1, { 0 },    { 0xAA } },        /* R0: pid0=0xAA */
    { 2, { 1, 2 }, { 0xBB, 0xCC } },  /* R1: pid1=0xBB, pid2=0xCC */
    { 1, { 0 },    { 0xDD } },        /* R2: pid0 덮어쓰기 0xAA->0xDD */
};

static void *sender_thread(void *arg) {
    Shared *sh = arg;
    /* primary WAL을 읽기 전용으로 따로 연다 — primary의 쓰기 fd와 오프셋이 안 섞이게.
     * (primary는 커밋마다 이 파일에 append하고, 우리는 새로 붙은 꼬리만 읽는다.) */
    int plog = open(PRIMARY_WAL, O_RDONLY);
    off_t sent_off = 0;

    for (int r = 0; r < NROUNDS; r++) {
        for (int i = 0; i < script[r].npages; i++) {
            primary_commit_page(sh->primary, script[r].pid[i], script[r].val[i]);
        }
        /* 이 라운드에 새로 붙은 WAL 바이트를 한 프레임으로 흘려보낸다. */
        walsender_send(sh->sender_sock, plog, &sent_off);
        /* receiver가 apply를 끝낼 때까지 대기(테스트 락스텝). */
        uint8_t ack;
        if (read(sh->sender_sock, &ack, 1) != 1) {
            break;
        }
    }
    /* 더 보낼 게 없다 -> 쓰기 방향을 닫아 receiver에게 깨끗한 EOF를 알린다. */
    shutdown(sh->sender_sock, SHUT_WR);
    close(plog);
    return NULL;
}

static void *receiver_thread(void *arg) {
    Shared *sh = arg;
    for (int r = 0; r < NROUNDS; r++) {
        int nb = walreceiver_recv(sh->receiver_sock, sh->recv_log_fd);
        if (nb <= 0) {
            sh->recv_error = 1;
            break;
        }
        sh->recv_bytes[r] = nb;
        int applied = replica_apply(sh->rep);
        if (applied < 0) {
            sh->recv_error = 1;
            break;
        }
        sh->applied[r] = applied;
        sh->position[r] = replica_position(sh->rep);
        /* apply 완료 ack (sender 락스텝 해제). */
        uint8_t ack = 1;
        if (write(sh->receiver_sock, &ack, 1) != 1) {
            sh->recv_error = 1;
            break;
        }
    }
    /* 모든 라운드 후엔 sender가 소켓을 닫았으므로 다음 recv는 0(EOF)이어야 한다. */
    sh->saw_extra_eof = (walreceiver_recv(sh->receiver_sock, sh->recv_log_fd) == 0);
    return NULL;
}

/* primary와 replica의 pid 페이지가 바이트 동일한가. */
static int pages_equal(Pager *a, Pager *b, page_id_t pid) {
    uint8_t pa[PAGE_SIZE], pb[PAGE_SIZE];
    if (pager_read(a, pid, pa) != 0) return 0;
    if (pager_read(b, pid, pb) != 0) return 0;
    return memcmp(pa, pb, PAGE_SIZE) == 0;
}

int main(void) {
    printf("== WAL 로그 시핑을 소켓으로 (primary --tcp--> replica) ==\n");
    cleanup();

    Wal primary;
    if (wal_open(&primary, PRIMARY_DB, PRIMARY_WAL) != 0) {
        printf("  FAIL primary 열기\n");
        return 1;
    }
    /* replica는 자기 데이터 파일 + '로컬' 로그(소켓으로 받아 채울 파일)를 연다.
     * 25편과 달리 primary의 로그가 아니라 receiver가 append하는 로컬 파일을 가리킨다. */
    WalReplica rep;
    if (replica_open(&rep, REPLICA_DB, REPLICA_WAL) != 0) {
        printf("  FAIL replica 열기\n");
        wal_close(&primary);
        return 1;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("  FAIL socketpair\n");
        replica_close(&rep);
        wal_close(&primary);
        return 1;
    }

    /* receiver가 받은 WAL을 로컬 로그에 append할 쓰기 fd(rep.log_fd는 읽기 전용). */
    int recv_log_fd = open(REPLICA_WAL, O_WRONLY);
    if (recv_log_fd < 0) {
        printf("  FAIL 로컬 로그 쓰기 fd 열기\n");
        close(sv[0]); close(sv[1]);
        replica_close(&rep);
        wal_close(&primary);
        return 1;
    }

    Shared sh;
    memset(&sh, 0, sizeof(sh));
    sh.sender_sock = sv[0];
    sh.receiver_sock = sv[1];
    sh.primary = &primary;
    sh.rep = &rep;
    sh.recv_log_fd = recv_log_fd;

    pthread_t st, rt;
    pthread_create(&rt, NULL, receiver_thread, &sh); /* receiver 먼저(recv 대기) */
    pthread_create(&st, NULL, sender_thread, &sh);
    pthread_join(st, NULL);
    pthread_join(rt, NULL);

    close(sv[0]);
    close(sv[1]);
    close(recv_log_fd);

    /* ── 검증(단일 스레드) ─────────────────────────────────────── */
    CHECK(sh.recv_error == 0, "receiver 경로 오류 없음");

    /* 1) 커밋된 페이지가 소켓을 타고 replica로 복제됐다. */
    CHECK(sh.recv_bytes[0] > 0, "R0: 소켓으로 WAL 바이트 수신됨");
    CHECK(sh.applied[0] == 1, "R0: 커밋 1건 적용됨");
    CHECK(pages_equal(&primary.data, &rep.data, 0), "R0: pid0 replica==primary (0xAA)");
    CHECK(sh.position[0] > 0, "R0: 복제 위치(lsn) > 0");

    /* 2) 증분 catch-up: 새 커밋만 추가로 스트리밍되어 적용된다. */
    CHECK(sh.recv_bytes[1] > 0, "R1: 추가 WAL 바이트 수신됨");
    CHECK(sh.applied[1] == 2, "R1: 새 커밋 2건만 적용(앞 건 재적용 안 함)");
    CHECK(pages_equal(&primary.data, &rep.data, 1), "R1: pid1 replica==primary (0xBB)");
    CHECK(pages_equal(&primary.data, &rep.data, 2), "R1: pid2 replica==primary (0xCC)");
    CHECK(sh.position[1] > sh.position[0], "R1: 복제 위치 전진(lag 감소)");

    /* 3) 덮어쓰기도 스트리밍된다 + replica가 primary와 바이트 동일. */
    CHECK(sh.applied[2] == 1, "R2: pid0 갱신 커밋 적용");
    CHECK(pages_equal(&primary.data, &rep.data, 0), "R2: pid0 replica가 새 값(0xDD)로 따라옴");
    CHECK(sh.position[2] > sh.position[1], "R2: 복제 위치 전진");

    /* 4) 복제 위치가 primary의 마지막 커밋 lsn까지 도달(끝에서 완전히 따라잡음). */
    CHECK(replica_position(&rep) == primary.next_lsn - 1,
          "복제 위치 == primary 마지막 커밋 lsn (완전 catch-up)");

    /* 클린 셧다운: 마지막 라운드 뒤 소켓 닫힘을 receiver가 EOF로 감지. */
    CHECK(sh.saw_extra_eof == 1, "sender 종료를 receiver가 EOF(recv==0)로 감지");

    replica_close(&rep);
    wal_close(&primary);
    cleanup();

    if (failures == 0) printf("\n모든 테스트 통과\n");
    else printf("\n%d개 실패\n", failures);
    return failures ? 1 : 0;
}
