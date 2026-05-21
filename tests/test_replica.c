#include "replica.h"
#include "wal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * 트랙 H1 — WAL 로그 시핑 복제 단위 테스트.
 *
 * primary는 커밋마다 WAL에 after-image + 커밋 마커를 남긴다(15편). replica는 그
 * 로그를 tail하며 커밋된 구간을 자기 데이터 파일에 재적용한다 = redo를 증분으로.
 * 여기서 검증하는 것:
 *   1) 커밋된 페이지가 replica에 그대로 복제된다(primary 데이터 == replica 데이터).
 *   2) 미완 꼬리(커밋 마커 없음)는 적용되지 않는다 — 읽기 일관성.
 *   3) 증분 catch-up: 새 커밋만 추가로 적용하고 복제 위치(lsn)가 전진한다.
 *   4) idempotent: 새 커밋이 없으면 재적용은 0. 오프셋 되감아 다시 훑어도(체크포인트
 *      truncate 모사) 이미 적용한 커밋은 건너뛴다.
 */

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } } while (0)

static void cleanup(void) {
    unlink("build/test_replica_primary.db");
    unlink("build/test_replica.wal");
    unlink("build/test_replica_replica.db");
}

/* primary와 replica의 pid 페이지가 바이트 동일한가. */
static int pages_equal(Pager *a, Pager *b, page_id_t pid) {
    uint8_t pa[PAGE_SIZE], pb[PAGE_SIZE];
    if (pager_read(a, pid, pa) != 0) return 0;
    if (pager_read(b, pid, pb) != 0) return 0;
    return memcmp(pa, pb, PAGE_SIZE) == 0;
}

/* primary에 pid 페이지를 val로 채워 한 트랜잭션으로 커밋한다. */
static void primary_commit_page(Wal *w, page_id_t pid, uint8_t val) {
    uint8_t page[PAGE_SIZE];
    memset(page, val, PAGE_SIZE);
    wal_begin(w);
    wal_stage(w, pid, page);
    wal_commit(w);
}

int main(void) {
    printf("== WAL 로그 시핑 복제 (primary -> replica) ==\n");
    cleanup();

    Wal primary;
    if (wal_open(&primary, "build/test_replica_primary.db", "build/test_replica.wal") != 0) {
        printf("  FAIL primary 열기\n");
        return 1;
    }
    WalReplica rep;
    if (replica_open(&rep, "build/test_replica_replica.db", "build/test_replica.wal") != 0) {
        printf("  FAIL replica 열기\n");
        return 1;
    }

    /* ── 1. 기본 복제: 한 커밋이 replica로 흐른다 ─────────────── */
    primary_commit_page(&primary, 0, 0xAA);
    int n = replica_apply(&rep);
    CHECK(n == 1, "커밋 1건 적용됨");
    CHECK(pages_equal(&primary.data, &rep.data, 0), "pid0: replica == primary (0xAA)");
    CHECK(replica_position(&rep) > 0, "복제 위치(lsn)가 0보다 큼");
    uint64_t pos1 = replica_position(&rep);

    /* ── 2. 증분 catch-up: 새 커밋만 추가 적용 ────────────────── */
    primary_commit_page(&primary, 1, 0xBB);
    primary_commit_page(&primary, 2, 0xCC);
    n = replica_apply(&rep);
    CHECK(n == 2, "새 커밋 2건만 추가 적용(앞 건 재적용 안 함)");
    CHECK(pages_equal(&primary.data, &rep.data, 1), "pid1: replica == primary (0xBB)");
    CHECK(pages_equal(&primary.data, &rep.data, 2), "pid2: replica == primary (0xCC)");
    CHECK(replica_position(&rep) > pos1, "복제 위치가 전진함(lag 감소)");

    /* ── 3. idempotent: 새 커밋 없으면 재적용 0 ───────────────── */
    n = replica_apply(&rep);
    CHECK(n == 0, "새 커밋이 없으면 0건 적용");
    uint64_t pos3 = replica_position(&rep);

    /* ── 4. 덮어쓰기 복제: 같은 pid의 새 버전도 흐른다 ────────── */
    primary_commit_page(&primary, 0, 0xDD); /* pid0을 0xAA -> 0xDD */
    n = replica_apply(&rep);
    CHECK(n == 1, "pid0 갱신 커밋 적용");
    CHECK(pages_equal(&primary.data, &rep.data, 0), "pid0: replica가 새 값(0xDD)로 따라옴");
    CHECK(replica_position(&rep) > pos3, "복제 위치 전진");

    /* ── 5. 미완 꼬리는 적용 안 함(읽기 일관성) ───────────────
     * 커밋 마커 없이 페이지 로그만 남기고 멈춘다 = "커밋 전 크래시". replica는
     * 이 구간을 커밋으로 못 보므로 적용하지 않는다. */
    {
        uint8_t page[PAGE_SIZE];
        memset(page, 0xEE, PAGE_SIZE);
        wal_begin(&primary);
        wal_stage(&primary, 3, page);
        wal_test_crash_before_commit = 1; /* REC_PAGE만 쓰고 마커 없이 멈춤 */
        wal_commit(&primary);
        wal_test_crash_before_commit = 0;

        uint64_t pos_before = replica_position(&rep);
        n = replica_apply(&rep);
        CHECK(n == 0, "미완(커밋 마커 없는) 구간은 적용 안 됨");
        CHECK(replica_position(&rep) == pos_before, "복제 위치 그대로(꼬리 미적용)");
        /* pid3은 replica에 존재하지 않아야 한다(파일이 그만큼 안 자람). */
        uint8_t probe[PAGE_SIZE];
        CHECK(pager_read(&rep.data, 3, probe) != 0, "pid3: replica에 아직 없음(미커밋)");
    }

    /* ── 6. 오프셋 되감기(체크포인트 truncate 모사) → 중복 없이 이어감 ──
     * apply_off를 0으로 강제로 되감아 처음부터 다시 훑게 한다. lsn 덕에 이미
     * 적용한 커밋은 건너뛰고(0건 추가), 데이터는 그대로여야 한다. */
    {
        uint64_t pos_before = replica_position(&rep);
        rep.apply_off = 0; /* 체크포인트로 로그가 잘려 되감긴 상황을 모사 */
        n = replica_apply(&rep);
        CHECK(n == 0, "되감아 다시 훑어도 이미 적용한 커밋은 재적용 안 함(idempotent)");
        CHECK(replica_position(&rep) == pos_before, "되감기 후에도 복제 위치 불변");
        CHECK(pages_equal(&primary.data, &rep.data, 0), "되감기 후 데이터 무결(pid0)");
        CHECK(pages_equal(&primary.data, &rep.data, 2), "되감기 후 데이터 무결(pid2)");
    }

    replica_close(&rep);
    wal_close(&primary);
    cleanup();

    if (failures == 0) printf("\n모든 테스트 통과\n");
    else printf("\n%d개 실패\n", failures);
    return failures ? 1 : 0;
}
