#include "raft.h"

#include <stdio.h>
#include <string.h>

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
    RaftMsg q[QCAP];
    int qhead, qtail;
} Cluster;

static Cluster g; /* 단일 스레드 테스트 — 전역이면 apply 콜백이 접근하기 쉽다 */

static void on_apply(int id, int64_t index, int64_t command, void *ctx) {
    (void)ctx;
    if (index < MAXLOG) {
        g.applied[id][index] = command;
        if (index > g.applied_hi[id]) g.applied_hi[id] = index;
    }
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

    for (int i = 0; i < RAFT_MAX_NODES; i++) {
        if (g.nodes[i].log) raft_free(&g.nodes[i]);
    }

    if (failures == 0) printf("\n모든 테스트 통과\n");
    else printf("\n%d개 실패\n", failures);
    return failures ? 1 : 0;
}
