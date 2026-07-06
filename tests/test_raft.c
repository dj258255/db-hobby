#include "raft.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * 트랙 H2 — Raft 합의 단위 테스트 (결정적 시뮬레이션 네트워크).
 *
 * 합의의 정확성은 '적대적 스케줄링'에서 증명해야 한다 — 분단·크래시·재정렬.
 * 실제 소켓+벽시계로는 그걸 결정적으로 재현할 수 없으므로(26편 소켓과 대조),
 * 여기선 하버스가 논리 시계·메시지 라우터·분단 행렬을 단일 스레드로 소유한다.
 * 각 노드는 순수 로직(raft_tick + raft_recv)일 뿐이다.
 *
 * 검증하는 안전성(§5):
 *   - Election Safety : 한 term에 리더는 최대 하나.
 *   - Leader Append   : 리더는 자기 로그를 덮지 않는다(항상 append).
 *   - Log Matching    : prev 검사로 갈라진 꼬리를 리더 것으로 덮어쓴다.
 *   - Leader Completeness(§5.4.1) : 자기 로그가 최신인 후보만 당선.
 *   - State Machine Safety : 커밋된 엔트리는 모든 노드에서 같은 순서로 적용.
 */

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } } while (0)

#define MAXLOG 128
#define QCAP 8192

typedef struct {
    Raft nodes[RAFT_MAX_NODES];
    int n;
    int connected[RAFT_MAX_NODES][RAFT_MAX_NODES];
    int alive[RAFT_MAX_NODES];
    int64_t applied[RAFT_MAX_NODES][MAXLOG]; /* applied[node][index] = command */
    int64_t applied_hi[RAFT_MAX_NODES];      /* 적용한 마지막 인덱스 */
    int64_t sm[RAFT_MAX_NODES];              /* 레지스터 상태기계 = 마지막 적용 커맨드 값 */
    RaftMsg q[QCAP];
    int qhead, qtail;
} Cluster;

static Cluster g; /* 단일 스레드 테스트 — 전역이면 apply 콜백이 접근하기 쉽다 */

static void on_apply(int id, int64_t index, int64_t command, void *ctx) {
    (void)ctx;
    g.sm[id] = command; /* 레지스터 SM: 마지막 적용 커맨드가 곧 상태 */
    if (index < MAXLOG) {
        g.applied[id][index] = command;
        if (index > g.applied_hi[id]) g.applied_hi[id] = index;
    }
}

/* InstallSnapshot 수신 시 SM에 스냅샷을 설치(§7): 레지스터 값을 통째로 얹고
 * 적용 인덱스를 스냅샷 지점으로 점프한다(중간 인덱스를 하나씩 적용하지 않음). */
static void on_snap_install(int id, int64_t last_index, int64_t value, void *ctx) {
    (void)ctx;
    g.sm[id] = value;
    if (last_index > g.applied_hi[id]) g.applied_hi[id] = last_index;
}

static void qpush(RaftMsg m) {
    int next = (g.qtail + 1) % QCAP;
    if (next == g.qhead) return; /* 가득 참(학습용: 조용히 버림 — QCAP 넉넉) */
    g.q[g.qtail] = m;
    g.qtail = next;
}
static int qpop(RaftMsg *m) {
    if (g.qhead == g.qtail) return 0;
    *m = g.q[g.qhead];
    g.qhead = (g.qhead + 1) % QCAP;
    return 1;
}

static void cluster_init(int n) {
    memset(&g, 0, sizeof(g));
    g.n = n;
    for (int i = 0; i < n; i++) {
        /* 서로 다른 선거 타임아웃 -> '누가 먼저 타임아웃하나'가 결정적.
         * 노드 i = 6 + 4i (6,10,14,...), 하트비트 2(<최소 선거) -> 리더가 권위 유지. */
        raft_init(&g.nodes[i], i, n, 6 + 4 * i, 2, on_apply, &g);
        g.nodes[i].snap_install = on_snap_install; /* §7 스냅샷 설치 콜백 */
        g.alive[i] = 1;
        g.applied_hi[i] = 0;
        for (int j = 0; j < n; j++) g.connected[i][j] = 1;
    }
}

static void deliver(RaftMsg *m) {
    if (!g.alive[m->from] || !g.alive[m->to]) return;
    if (!g.connected[m->from][m->to]) return;
    RaftMsg reply;
    int has_reply = 0;
    RaftOutbox out;
    out.n = 0;
    raft_recv(&g.nodes[m->to], m, &reply, &has_reply, &out);
    if (has_reply) qpush(reply);
    for (int i = 0; i < out.n; i++) qpush(out.m[i]);
}

/* 한 논리 스텝: 살아있는 노드를 모두 tick -> 메시지 큐를 다 비운다(응답 cascade 포함). */
static void cluster_step(void) {
    for (int i = 0; i < g.n; i++) {
        if (!g.alive[i]) continue;
        RaftOutbox out;
        out.n = 0;
        raft_tick(&g.nodes[i], &out);
        for (int k = 0; k < out.n; k++) qpush(out.m[k]);
    }
    RaftMsg m;
    int guard = 0;
    while (qpop(&m) && guard++ < QCAP) {
        deliver(&m);
    }
}
static void cluster_run(int steps) {
    for (int s = 0; s < steps; s++) cluster_step();
}

static int find_leader(void) {
    for (int i = 0; i < g.n; i++)
        if (g.alive[i] && g.nodes[i].role == RAFT_LEADER) return i;
    return -1;
}
static int count_leaders(void) {
    int c = 0;
    for (int i = 0; i < g.n; i++)
        if (g.alive[i] && g.nodes[i].role == RAFT_LEADER) c++;
    return c;
}
/* 가장 높은 term의 리더. 분단 중엔 낡은 리더(소수파)와 새 리더(다수파)가 공존할
 * 수 있으므로 — Raft 불변식은 'term당 하나'다 — 최신 term의 리더가 정당한 리더. */
static int find_leader_highest_term(void) {
    int best = -1;
    uint64_t bt = 0;
    for (int i = 0; i < g.n; i++) {
        if (g.alive[i] && g.nodes[i].role == RAFT_LEADER && g.nodes[i].current_term >= bt) {
            bt = g.nodes[i].current_term;
            best = i;
        }
    }
    return best;
}
/* 살아있는 노드들이 [1..hi] 적용 이력이 완전히 일치하는가(State Machine Safety). */
static int applied_agree(int64_t hi) {
    int base = -1;
    for (int i = 0; i < g.n; i++) {
        if (!g.alive[i]) continue;
        if (base < 0) base = i;
        for (int64_t k = 1; k <= hi; k++)
            if (g.applied[i][k] != g.applied[base][k]) return 0;
    }
    return 1;
}
/* 어느 스텝에서도 같은 term에 리더가 둘이면 안 된다(Election Safety). */
static int two_leaders_same_term(void) {
    for (int i = 0; i < g.n; i++) {
        if (!g.alive[i] || g.nodes[i].role != RAFT_LEADER) continue;
        for (int j = i + 1; j < g.n; j++) {
            if (!g.alive[j] || g.nodes[j].role != RAFT_LEADER) continue;
            if (g.nodes[i].current_term == g.nodes[j].current_term) return 1;
        }
    }
    return 0;
}

int main(void) {
    printf("== Raft 합의 (리더 선출 + 로그 복제 + 안전성) ==\n");

    /* ── 1. 리더 선출 (3노드) ───────────────────────────────────── */
    cluster_init(3);
    int saw_two = 0;
    for (int s = 0; s < 20; s++) { cluster_step(); if (two_leaders_same_term()) saw_two = 1; }
    int L = find_leader();
    CHECK(L >= 0, "선거: 리더가 선출됨");
    CHECK(count_leaders() == 1, "선거: 리더는 정확히 하나(Election Safety)");
    CHECK(!saw_two, "선거: 어느 순간에도 같은 term 리더 둘 없음");
    CHECK(L == 0, "선거: 가장 작은 선거 타임아웃 노드(0)가 리더(결정적)");
    CHECK(g.nodes[L].current_term >= 1, "선거: 리더의 term >= 1");

    /* ── 2. 로그 복제 + 커밋 + 상태기계 합의 ───────────────────── */
    raft_submit(&g.nodes[L], 100);
    raft_submit(&g.nodes[L], 200);
    raft_submit(&g.nodes[L], 300);
    cluster_run(15);
    CHECK(g.nodes[L].commit_index == 3, "복제: 리더가 3개 엔트리 커밋");
    CHECK(applied_agree(3), "복제: 모든 노드가 같은 순서로 100,200,300 적용(SM Safety)");
    CHECK(g.applied[0][1] == 100 && g.applied[1][2] == 200 && g.applied[2][3] == 300,
          "복제: 적용된 커맨드 값이 정확");
    int all_committed = 1;
    for (int i = 0; i < 3; i++) if (g.nodes[i].commit_index != 3) all_committed = 0;
    CHECK(all_committed, "복제: 팔로워들도 commit_index=3까지 따라옴");

    /* ── 3. 분단 → 새 리더 → 옛 리더 강등 → 로그 화해 (5노드) ──── */
    cluster_init(5);
    cluster_run(20);
    int L1 = find_leader();
    CHECK(L1 == 0, "분단: 초기 리더는 노드 0");
    raft_submit(&g.nodes[L1], 111);
    cluster_run(10);
    CHECK(g.nodes[L1].commit_index == 1, "분단 전: 엔트리 1 커밋됨");

    /* 리더(0)를 소수파로 고립: 0 <-> {1,2,3,4} 단절 */
    for (int j = 1; j < 5; j++) { g.connected[0][j] = g.connected[j][0] = 0; }
    /* 고립된 옛 리더에 커밋 못 될 커맨드를 넣는다(과반이 없어 영영 커밋 안 됨) */
    raft_submit(&g.nodes[0], 999);
    raft_submit(&g.nodes[0], 998);
    cluster_run(30); /* 다수파 {1,2,3,4}가 새 리더를 뽑는다(더 높은 term) */
    /* 옛 리더(0)는 고립돼도 자기가 리더인 줄 안다(정확한 동작) -> 최신 term 리더를 찾는다. */
    int L2 = find_leader_highest_term();
    CHECK(L2 >= 1, "분단: 다수파에서 새 리더 선출");
    CHECK(g.nodes[L2].current_term > g.nodes[0].current_term,
          "분단: 새 리더의 term이 옛 리더보다 높음");
    raft_submit(&g.nodes[L2], 222); /* 새 리더가 다수파에 복제·커밋 */
    cluster_run(20);
    CHECK(g.nodes[L2].role == RAFT_LEADER && g.nodes[L2].commit_index >= 2,
          "분단: 새 리더가 다수파에 새 엔트리 커밋");
    CHECK(g.applied[0][2] != 222, "분단: 고립된 옛 리더는 222를 커밋 못 함");

    /* 치유: 다시 연결 -> 옛 리더는 더 높은 term 보고 강등, 갈린 꼬리(999,998) 덮어써짐 */
    for (int j = 1; j < 5; j++) { g.connected[0][j] = g.connected[j][0] = 1; }
    cluster_run(40);
    CHECK(g.nodes[0].role == RAFT_FOLLOWER, "치유: 옛 리더가 팔로워로 강등");
    CHECK(count_leaders() == 1, "치유: 리더는 여전히 하나");
    CHECK(g.applied[0][2] == 222, "치유: 옛 리더의 갈린 꼬리가 리더 로그로 덮어써짐(Log Matching)");
    int64_t hi = g.nodes[L2].commit_index;
    CHECK(applied_agree(hi), "치유: 모든 노드가 동일 로그로 수렴(SM Safety)");

    /* ── 4. 선거 제약(§5.4.1): 낡은 로그의 후보는 표를 못 받는다 ── */
    {
        cluster_init(3);
        /* 투표자 V(노드1)에게 term1 엔트리 두 개를 직접 심는다(로그를 최신으로). */
        Raft *V = &g.nodes[1];
        V->current_term = 1;
        RaftMsg fake_ae;
        memset(&fake_ae, 0, sizeof(fake_ae));
        fake_ae.type = MSG_APPEND_ENTRIES;
        fake_ae.from = 0; fake_ae.to = 1; fake_ae.term = 1;
        fake_ae.prev_log_index = 0; fake_ae.prev_log_term = 0;
        fake_ae.entries[0].term = 1; fake_ae.entries[0].command = 7;
        fake_ae.entries[1].term = 1; fake_ae.entries[1].command = 8;
        fake_ae.n_entries = 2; fake_ae.leader_commit = 0;
        RaftMsg rep; int hr = 0; RaftOutbox out; out.n = 0;
        raft_recv(V, &fake_ae, &rep, &hr, &out);
        CHECK(raft_last_log_index(V) == 2, "제약: 투표자 로그가 최신(인덱스 2)");

        /* 낡은 후보 C(로그 비어 있음, index 0)가 표를 구한다 -> 거절돼야 한다. */
        RaftMsg rv;
        memset(&rv, 0, sizeof(rv));
        rv.type = MSG_REQUEST_VOTE; rv.from = 2; rv.to = 1; rv.term = 2;
        rv.last_log_index = 0; rv.last_log_term = 0;
        raft_recv(V, &rv, &rep, &hr, &out);
        CHECK(hr && rep.vote_granted == 0, "제약: 로그가 뒤진 후보에게 표를 거절(Leader Completeness)");

        /* 같은/더 최신 로그의 후보는 표를 받는다. */
        RaftMsg rv2;
        memset(&rv2, 0, sizeof(rv2));
        rv2.type = MSG_REQUEST_VOTE; rv2.from = 0; rv2.to = 1; rv2.term = 3;
        rv2.last_log_index = 2; rv2.last_log_term = 1;
        raft_recv(V, &rv2, &rep, &hr, &out);
        CHECK(hr && rep.vote_granted == 1, "제약: 로그가 최신인 후보에겐 표를 부여");
    }

    /* ── 5. 크래시 후 재시작: 커밋된 로그를 따라잡는다 ─────────── */
    {
        cluster_init(3);
        cluster_run(20);
        int lead = find_leader();
        raft_submit(&g.nodes[lead], 55);
        raft_submit(&g.nodes[lead], 66);
        cluster_run(15);
        int victim = (lead + 1) % 3;
        CHECK(g.nodes[victim].commit_index == 2, "크래시 전: 희생 노드도 2까지 커밋");
        /* 크래시: 다운 + 휘발 상태 리셋(지속 상태=term/votedFor/log는 보존) */
        g.alive[victim] = 0;
        raft_crash_restart(&g.nodes[victim]);
        raft_submit(&g.nodes[lead], 77); /* 다운 중 새 커맨드(나머지 둘로 과반 커밋) */
        cluster_run(15);
        CHECK(g.nodes[lead].commit_index == 3, "크래시 중: 남은 과반으로 계속 커밋");
        /* 재시작: 다시 살아나 AppendEntries로 커밋분을 재적용 */
        g.alive[victim] = 1;
        cluster_run(25);
        CHECK(g.nodes[victim].commit_index == 3, "재시작: 희생 노드가 커밋 로그를 따라잡음");
        CHECK(applied_agree(3), "재시작: 재적용 후 모든 노드 로그 일치(idempotent 재적용)");
    }

    /* ── 6. 지속성(§5.1): votedFor가 크래시를 살아남아 이중 투표를 막는다 ──
     * votedFor가 안 남으면 한 노드가 같은 term에 두 번 투표해 리더가 둘 생길 수
     * 있다(Election Safety 붕괴). 진짜 디스크 라운드트립으로 이를 검증한다. */
    {
        const char *path = "build/test_raft_persist.dat";
        unlink(path);
        cluster_init(3);
        Raft *V = &g.nodes[1];
        RaftMsg rv1;
        memset(&rv1, 0, sizeof(rv1));
        rv1.type = MSG_REQUEST_VOTE; rv1.from = 0; rv1.to = 1; rv1.term = 5;
        rv1.last_log_index = 0; rv1.last_log_term = 0;
        RaftMsg rep; int hr = 0; RaftOutbox out; out.n = 0;
        raft_recv(V, &rv1, &rep, &hr, &out);
        CHECK(hr && rep.vote_granted == 1, "지속성: term5에서 후보 0에게 투표");
        CHECK(V->voted_for == 0, "지속성: votedFor=0 기록");
        CHECK(raft_save(V, path) == 0, "지속성: 지속 상태 fsync 저장(응답 전 내구화)");

        /* 진짜 크래시: 구조체를 처음부터 다시(메모리 날림) 만들고 디스크에서 복구. */
        Raft V2;
        raft_init(&V2, 1, 3, 10, 2, on_apply, &g);
        CHECK(V2.voted_for == -1, "지속성: 새 노드는 votedFor 없음(-1)");
        CHECK(raft_load(&V2, path) == 0, "지속성: 디스크에서 지속 상태 복구");
        CHECK(V2.current_term == 5 && V2.voted_for == 0,
              "지속성: term5·votedFor=0가 크래시를 살아남음");

        /* 같은 term5에서 다른 후보 2가 표를 구한다 -> 거절(이중 투표 방지). */
        RaftMsg rv2;
        memset(&rv2, 0, sizeof(rv2));
        rv2.type = MSG_REQUEST_VOTE; rv2.from = 2; rv2.to = 1; rv2.term = 5;
        rv2.last_log_index = 0; rv2.last_log_term = 0;
        hr = 0; out.n = 0;
        raft_recv(&V2, &rv2, &rep, &hr, &out);
        CHECK(hr && rep.vote_granted == 0,
              "지속성: 복구 후 같은 term 다른 후보엔 표 거절(이중 투표 방지 → Election Safety)");
        raft_free(&V2);
        unlink(path);
    }

    /* ── 7. 지속성: 커밋된 로그가 디스크 라운드트립을 그대로 살아남는다 ── */
    {
        const char *path = "build/test_raft_log.dat";
        unlink(path);
        cluster_init(3);
        cluster_run(20);
        int lead = find_leader();
        raft_submit(&g.nodes[lead], 42);
        raft_submit(&g.nodes[lead], 43);
        cluster_run(15);
        CHECK(raft_save(&g.nodes[lead], path) == 0, "지속성: 리더 로그 저장");
        int64_t saved_len = g.nodes[lead].log_len;
        uint64_t saved_term = g.nodes[lead].current_term;
        Raft R;
        raft_init(&R, lead, 3, 10, 2, on_apply, &g);
        CHECK(raft_load(&R, path) == 0, "지속성: 로그 복구");
        CHECK(R.log_len == saved_len && R.current_term == saved_term,
              "지속성: 로그 길이·term 일치");
        CHECK(R.log[1].command == 42 && R.log[2].command == 43,
              "지속성: 엔트리 순서·값(42,43) 보존");
        raft_free(&R);
        unlink(path);
    }

    /* ── 8. 스냅샷(§7): 로그 압축 후에도 복제·커밋이 정확 (log_base>0 전 경로) ──
     * 압축은 인덱스에 base 오프셋을 도입한다. 압축 '후' 새 엔트리를 복제·커밋해
     * send_append_entries·handle_append_entries·leader_advance_commit·apply가
     * log_base>0에서 모두 옳게 도는지 관통 검증한다. */
    {
        cluster_init(3);
        cluster_run(20);
        int lead = find_leader();
        for (int64_t v = 1; v <= 5; v++) raft_submit(&g.nodes[lead], 100 + v);
        cluster_run(20);
        CHECK(g.nodes[lead].commit_index == 5, "스냅샷: 5개 커밋(압축 전)");
        int64_t before_len = g.nodes[lead].log_len;
        CHECK(raft_snapshot(&g.nodes[lead], 5, g.sm[lead]) == 0, "스냅샷: index5까지 압축 성공");
        CHECK(g.nodes[lead].log_base == 5, "스냅샷: log_base가 5로 전진");
        CHECK(g.nodes[lead].log_len < before_len, "스냅샷: 물리 로그 길이 축소");
        CHECK(raft_last_log_index(&g.nodes[lead]) == 5, "스냅샷: 논리 마지막 인덱스 보존(5)");
        CHECK(raft_snapshot(&g.nodes[lead], 5, g.sm[lead]) == -1,
              "스냅샷: 이미 압축된 지점 재압축은 거부");

        /* 압축 후 새 커맨드 -> log_base>0에서 복제·커밋 (전 경로 관통) */
        raft_submit(&g.nodes[lead], 999);
        cluster_run(20);
        CHECK(g.nodes[lead].commit_index == 6, "스냅샷: 압축 후 새 엔트리(6) 커밋");
        int sok = 1, cok = 1;
        for (int i = 0; i < 3; i++) {
            if (g.sm[i] != 999) sok = 0;
            if (g.nodes[i].commit_index != 6) cok = 0;
        }
        CHECK(sok, "스냅샷: 압축 후에도 모든 노드 SM 수렴(999)");
        CHECK(cok, "스냅샷: 팔로워들도 commit_index=6");
    }

    /* ── 9. 스냅샷(§7): 뒤처진 팔로워가 InstallSnapshot으로 따라잡는다 ──
     * 리더가 팔로워가 필요로 하는 엔트리를 이미 압축해 버렸으면 AppendEntries로는
     * 못 보낸다 -> 스냅샷을 통째로 전송(InstallSnapshot). */
    {
        cluster_init(3);
        cluster_run(20);
        int lead = find_leader();
        int lag = (lead + 1) % 3;
        /* 팔로워를 크래시(다운) — 이후 커밋을 통째로 놓친다. */
        g.alive[lag] = 0;
        raft_crash_restart(&g.nodes[lag]);
        for (int64_t v = 1; v <= 6; v++) raft_submit(&g.nodes[lead], 500 + v);
        cluster_run(25); /* 남은 과반(리더+1)으로 커밋 */
        CHECK(g.nodes[lead].commit_index == 6, "InstallSnapshot: 다수파로 6개 커밋(lag는 못 받음)");
        /* 리더가 압축 -> lag가 필요로 하는 엔트리(1..6)가 사라진다. */
        raft_snapshot(&g.nodes[lead], g.nodes[lead].commit_index, g.sm[lead]);
        CHECK(g.nodes[lead].log_base == 6, "InstallSnapshot: 리더 압축(log_base=6)");
        CHECK(g.sm[lag] != g.sm[lead], "InstallSnapshot: 크래시된 팔로워는 아직 뒤처짐");

        /* 재시작 -> next_index[lag]-1 < log_base 이므로 리더가 InstallSnapshot 전송 */
        g.alive[lag] = 1;
        cluster_run(40);
        CHECK(g.nodes[lag].log_base == 6, "InstallSnapshot: 팔로워가 스냅샷 설치(log_base 점프)");
        CHECK(g.sm[lag] == g.sm[lead], "InstallSnapshot: 팔로워 SM이 리더와 일치(catch-up)");
        CHECK(g.nodes[lag].commit_index == g.nodes[lead].commit_index,
              "InstallSnapshot: commit_index 일치");
    }

    /* ── 10. 스냅샷+지속성: 재기동 시 스냅샷 상태를 SM에 복원한다 ──
     * log_base>0 노드를 디스크에 저장 후 새 노드로 load하면, 1..log_base 엔트리는
     * 압축돼 사라졌으므로 재적용으론 SM을 못 채운다 -> raft_load가 snap_install로
     * 스냅샷 값을 SM에 직접 시드해야 한다(안 그러면 그 구간 상태가 영구히 빔). */
    {
        const char *path = "build/test_raft_snap.dat";
        unlink(path);
        cluster_init(3);
        cluster_run(20);
        int lead = find_leader();
        for (int64_t v = 1; v <= 4; v++) raft_submit(&g.nodes[lead], 700 + v);
        cluster_run(20);
        raft_snapshot(&g.nodes[lead], 4, g.sm[lead]); /* log_base=4, snapshot_value=704 */
        CHECK(g.nodes[lead].log_base == 4, "스냅샷+지속: 리더 압축(log_base=4)");
        CHECK(raft_save(&g.nodes[lead], path) == 0, "스냅샷+지속: 스냅샷 노드 저장");

        g.sm[0] = -1; /* SM을 오염시켜 load가 다시 시드하는지 확인 */
        Raft R;
        raft_init(&R, 0, 3, 10, 2, on_apply, &g);
        R.snap_install = on_snap_install;
        CHECK(raft_load(&R, path) == 0, "스냅샷+지속: load 성공");
        CHECK(R.log_base == 4 && R.snapshot_value == 704,
              "스냅샷+지속: log_base·snapshot_value 복원");
        CHECK(g.sm[0] == 704, "스냅샷+지속: raft_load가 SM에 스냅샷을 시드(704)");
        CHECK(R.commit_index == 4 && R.last_applied == 4,
              "스냅샷+지속: commit/applied 하한 = log_base");
        raft_free(&R);
        unlink(path);
    }

    /* ── 11. 멤버십 변경(§6, 단일 서버): 노드 추가·제거 ──────────
     * 시작은 3-멤버 {0,1,2}, 노드3은 passive(비구성원). 리더가 노드3을 추가하면
     * config 엔트리가 복제돼 노드3이 '로그에 보자마자' 구성원이 되고 따라잡는다.
     * 이어서 노드2를 제거하면 과반이 재계산돼 {0,1,3}로 계속 커밋한다. */
    {
        cluster_init(4);
        for (int i = 0; i < 3; i++) g.nodes[i].member_mask = 0x7; /* {0,1,2} */
        g.nodes[3].member_mask = 0x0;                             /* passive joiner */
        cluster_run(20);
        int lead = find_leader();
        CHECK(lead >= 0 && lead < 3, "멤버십: {0,1,2}에서 리더 선출(노드3 제외)");
        CHECK(!raft_is_member(&g.nodes[3], 3), "멤버십: 노드3은 아직 비구성원");
        raft_submit(&g.nodes[lead], 11);
        cluster_run(15);
        CHECK(g.nodes[lead].commit_index >= 1, "멤버십: 3-멤버로 커밋");

        /* 노드 3 추가 */
        CHECK(raft_change_config(&g.nodes[lead], 0xF) > 0, "멤버십: 노드3 추가(config 엔트리)");
        cluster_run(30);
        CHECK(raft_is_member(&g.nodes[3], 3), "멤버십: 노드3이 구성원으로 채택(로그에 보자마자)");
        CHECK(g.nodes[3].commit_index == g.nodes[lead].commit_index,
              "멤버십: 추가된 노드3이 로그를 따라잡음");
        raft_submit(&g.nodes[lead], 22);
        cluster_run(20);
        int all4 = 1;
        for (int i = 0; i < 4; i++)
            if (g.nodes[i].commit_index != g.nodes[lead].commit_index) all4 = 0;
        CHECK(all4, "멤버십: 4-멤버로 새 엔트리 전원 커밋");

        /* 노드 2 제거(단일 서버 delta 검증) + decommission */
        CHECK(raft_change_config(&g.nodes[lead], 0xB) > 0, "멤버십: 노드2 제거(config 엔트리)");
        CHECK(raft_change_config(&g.nodes[lead], 0x0) == -1,
              "멤버십: 한 번에 여러 노드 변경은 거부(단일 서버 안전)");
        g.alive[2] = 0; /* 제거된 노드는 decommission(live 제거 시 선거 disruption은 프론티어) */
        cluster_run(30);
        CHECK(!raft_is_member(&g.nodes[lead], 2), "멤버십: 노드2가 비구성원으로");

        int64_t before = g.nodes[lead].commit_index;
        raft_submit(&g.nodes[lead], 33);
        cluster_run(20);
        CHECK(g.nodes[lead].commit_index > before, "멤버십: 노드2 없이 {0,1,3}로 계속 커밋");
        int ok013 = g.nodes[0].commit_index == g.nodes[lead].commit_index &&
                    g.nodes[1].commit_index == g.nodes[lead].commit_index &&
                    g.nodes[3].commit_index == g.nodes[lead].commit_index;
        CHECK(ok013, "멤버십: 남은 {0,1,3}가 합의 유지");
    }

    /* ── 12. 멤버십 정확성 수정(적대적 리뷰 반영) ─────────────────
     * 리뷰어가 찾은 4건: C1(스냅샷 합류자 member_mask 누락)·C2(truncation 시 config
     * 미복구)·L1(commit-wait 부재)·L2(자기 제거 리더 미강등)를 각각 검증한다. */
    {
        /* C1: InstallSnapshot이 member_mask를 설치 -> 스냅샷으로 합류한 노드가 구성원 */
        cluster_init(3);
        Raft *N = &g.nodes[2];
        N->member_mask = 0x0;
        N->base_member_mask = 0x0; /* passive joiner */
        RaftMsg is;
        memset(&is, 0, sizeof is);
        is.type = MSG_INSTALL_SNAPSHOT; is.from = 0; is.to = 2; is.term = 1;
        is.last_included_index = 5; is.last_included_term = 1; is.snapshot_value = 100;
        is.member_mask = 0x7; /* 스냅샷이 담은 멤버십 */
        RaftMsg rep; int hr = 0; RaftOutbox out; out.n = 0;
        raft_recv(N, &is, &rep, &hr, &out);
        CHECK(N->member_mask == 0x7 && raft_is_member(N, 2),
              "C1: InstallSnapshot이 member_mask 설치(스냅샷 합류자가 구성원됨)");

        /* C2: adopt한 미커밋 config가 truncation으로 롤백되면 member_mask도 복구 */
        cluster_init(3); /* base_member_mask=0x7 */
        Raft *F = &g.nodes[1];
        F->current_term = 1;
        RaftMsg ae;
        memset(&ae, 0, sizeof ae);
        ae.type = MSG_APPEND_ENTRIES; ae.from = 0; ae.to = 1; ae.term = 1;
        ae.prev_log_index = 0; ae.prev_log_term = 0;
        ae.entries[0].term = 1; ae.entries[0].command = 0xF; ae.entries[0].is_config = 1;
        ae.n_entries = 1; ae.leader_commit = 0;
        raft_recv(F, &ae, &rep, &hr, &out);
        CHECK(F->member_mask == 0xF, "C2 전: 미커밋 config를 adopt-on-append로 채택(0xF)");
        RaftMsg ae2; /* 더 높은 term의 리더가 idx1을 비-config로 덮어씀(truncation) */
        memset(&ae2, 0, sizeof ae2);
        ae2.type = MSG_APPEND_ENTRIES; ae2.from = 2; ae2.to = 1; ae2.term = 2;
        ae2.prev_log_index = 0; ae2.prev_log_term = 0;
        ae2.entries[0].term = 2; ae2.entries[0].command = 999; ae2.entries[0].is_config = 0;
        ae2.n_entries = 1; ae2.leader_commit = 0;
        raft_recv(F, &ae2, &rep, &hr, &out);
        CHECK(F->member_mask == 0x7, "C2: 롤백된 config가 member_mask에서 복구됨(0x7)");

        /* L1: 직전 config 변경이 커밋되기 전엔 다음 변경 거부(commit-wait) */
        cluster_init(4);
        for (int i = 0; i < 3; i++) { g.nodes[i].member_mask = 0x7; g.nodes[i].base_member_mask = 0x7; }
        g.nodes[3].member_mask = 0x0; g.nodes[3].base_member_mask = 0x0;
        cluster_run(20);
        int lead = find_leader();
        CHECK(raft_change_config(&g.nodes[lead], 0xF) > 0, "L1: 첫 변경(노드3 추가) 수락");
        CHECK(raft_change_config(&g.nodes[lead], 0xD) == -1,
              "L1: 직전 변경 미커밋 중 두 번째 변경 거부(commit-wait)");
        cluster_run(30);
        CHECK(raft_change_config(&g.nodes[lead], 0xB) > 0, "L1: 커밋 후 다음 변경 수락");

        /* L2: 자기 자신을 제거한 리더는 그 config가 커밋된 뒤 물러난다 */
        cluster_init(3);
        cluster_run(20);
        int L = find_leader();
        uint32_t without_self = 0x7 & ~(1u << L);
        CHECK(raft_change_config(&g.nodes[L], without_self) > 0, "L2: 리더가 자기 제거 config 추가");
        cluster_run(40);
        CHECK(g.nodes[L].role != RAFT_LEADER, "L2: 자기 제거가 커밋된 뒤 리더 강등");
    }

    /* ── 13. ReadIndex epoch(F1 수정): pre-barrier 응답이 오확인 못 한다 ──
     * 배리어 시작 전에 보낸 하트비트의 응답(옛 epoch)이 재정렬로 뒤늦게 도착해도
     * 확인에 세지 않는다 — 고립된 옛 리더가 낡은 읽기를 서빙하는 걸 막는다. */
    {
        cluster_init(5); /* 과반 3 */
        cluster_run(20);
        int lead = find_leader();
        RaftOutbox out; out.n = 0;
        int64_t ri = raft_read_index(&g.nodes[lead], &out); /* 배리어 시작, epoch++ */
        CHECK(ri >= 0, "epoch: 리더가 read_index 시작");
        uint64_t ep = g.nodes[lead].read_epoch;

        /* pre-barrier(옛 epoch) 성공 응답 2개 주입 -> 오확인되면 안 됨 */
        for (int f = 1; f <= 2; f++) {
            RaftMsg rep; memset(&rep, 0, sizeof rep);
            rep.type = MSG_APPEND_ENTRIES_REPLY; rep.from = f; rep.to = lead;
            rep.term = g.nodes[lead].current_term; rep.success = 1;
            rep.match_index = raft_last_log_index(&g.nodes[lead]);
            rep.read_epoch = ep - 1; /* 옛 epoch */
            RaftMsg r2; int hr = 0; RaftOutbox o2; o2.n = 0;
            raft_recv(&g.nodes[lead], &rep, &r2, &hr, &o2);
        }
        CHECK(raft_read_confirmed(&g.nodes[lead]) < 0,
              "epoch: pre-barrier 응답(옛 epoch)은 확인에 안 셈(오확인 방지)");

        /* 현재 epoch 응답 2개 -> self+2 = 3 과반 -> 확인 */
        for (int f = 1; f <= 2; f++) {
            RaftMsg rep; memset(&rep, 0, sizeof rep);
            rep.type = MSG_APPEND_ENTRIES_REPLY; rep.from = f; rep.to = lead;
            rep.term = g.nodes[lead].current_term; rep.success = 1;
            rep.match_index = raft_last_log_index(&g.nodes[lead]);
            rep.read_epoch = ep; /* 현재 epoch */
            RaftMsg r2; int hr = 0; RaftOutbox o2; o2.n = 0;
            raft_recv(&g.nodes[lead], &rep, &r2, &hr, &o2);
        }
        CHECK(raft_read_confirmed(&g.nodes[lead]) == ri,
              "epoch: 현재 epoch 응답 과반 -> 확인됨");
    }

    /* ── 14. ReadIndex 게이트(§6.4 전제조건): no-op 커밋 전 읽기 보류 ──
     * 갓 선출된 리더의 commit_index는 전임 리더가 커밋한 엔트리를 아직 반영 못 했을
     * 수 있다(§5.4.2: 옛 term 엔트리는 직접 커밋 불가). 그 시점의 commit_index를
     * read index로 잡으면 커밋된 쓰기를 놓친 읽기가 된다 — 새 리더는 당선 직후
     * no-op을 append하고, 그것이 커밋되기 전의 ReadIndex 요청은 거부해야 한다. */
    {
        cluster_init(3);
        /* 전임 리더(term1)가 커밋까지 마친 엔트리(42)를 노드1에 복제하되,
         * leader_commit이 닿기 전에 죽었다고 치자: 노드1은 last=1, commit=0. */
        Raft *N = &g.nodes[1];
        RaftMsg ae;
        memset(&ae, 0, sizeof ae);
        ae.type = MSG_APPEND_ENTRIES; ae.from = 0; ae.to = 1; ae.term = 1;
        ae.prev_log_index = 0; ae.prev_log_term = 0;
        ae.entries[0].term = 1; ae.entries[0].command = 42;
        ae.n_entries = 1; ae.leader_commit = 0;
        RaftMsg rep; int hr = 0; RaftOutbox out; out.n = 0;
        raft_recv(N, &ae, &rep, &hr, &out);
        CHECK(raft_last_log_index(N) == 1 && N->commit_index == 0,
              "게이트: 전임 커밋 엔트리를 가졌지만 commit_index엔 미반영(창 재현)");

        /* 노드1만 tick해 후보로 만들고, 노드2의 표로 당선시킨다(하트비트는 아직 미전달
         * — 이 창이 no-op 커밋 전의 새 리더다). */
        out.n = 0;
        for (int t = 0; t < 20 && N->role != RAFT_CANDIDATE; t++) raft_tick(N, &out);
        for (int k = 0; k < out.n; k++) {
            if (out.m[k].to == 2) {
                RaftOutbox o2; o2.n = 0;
                raft_recv(&g.nodes[2], &out.m[k], &rep, &hr, &o2);
            }
        }
        RaftOutbox hb; hb.n = 0; /* 당선 직후 하트비트 — 일부러 버린다(재전송됨) */
        RaftMsg r2;
        raft_recv(N, &rep, &r2, &hr, &hb);
        CHECK(N->role == RAFT_LEADER, "게이트: 노드1이 새 리더로 당선");
        CHECK(raft_last_log_index(N) == 2 && N->log[2].is_noop && N->log[2].term == N->current_term,
              "게이트: 당선 직후 현재 term의 no-op을 append(인덱스 2)");
        /* 버그 시나리오: 여기서 read index를 commit_index(0)로 잡으면 전임이 커밋한
         * 엔트리 1(42)을 놓친 읽기다 -> 게이트가 거부해야 한다. */
        out.n = 0;
        CHECK(raft_read_index(N, &out) == -1,
              "게이트: no-op 커밋 전 ReadIndex는 거부(커밋된 쓰기를 놓칠 수 있음)");

        /* 복제 재개 -> no-op이 커밋되면서 전임 term 엔트리도 간접 커밋(§5.4.2). */
        cluster_run(10);
        CHECK(N->commit_index == 2, "게이트: no-op 커밋이 전임 엔트리(1)까지 확정");
        CHECK(g.applied[1][1] == 42 && g.sm[1] == 42,
              "게이트: 전임 엔트리는 적용, no-op은 상태기계에 미적용");

        /* 이제 게이트 통과: read index = 2 (전임 커밋분 포함한 올바른 배리어). */
        out.n = 0;
        int64_t ri = raft_read_index(N, &out);
        CHECK(ri == 2, "게이트: no-op 커밋 후 ReadIndex 서빙(배리어=2, 전임 커밋 포함)");
        for (int k = 0; k < out.n; k++) qpush(out.m[k]);
        cluster_run(3);
        CHECK(raft_read_confirmed(N) == ri, "게이트: 과반 하트비트 ack로 읽기 확인");
    }

    for (int i = 0; i < RAFT_MAX_NODES; i++) {
        if (g.nodes[i].log) raft_free(&g.nodes[i]);
    }

    if (failures == 0) printf("\n모든 테스트 통과\n");
    else printf("\n%d개 실패\n", failures);
    return failures ? 1 : 0;
}
