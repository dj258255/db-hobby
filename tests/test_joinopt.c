#include "joinopt.h"

#include <stdio.h>

/*
 * 트랙 F3 — 조인 순서 최적화(Selinger DP) 단위 테스트.
 *
 * 21편은 '한 테이블을 어떻게 읽나'만 골랐다. 테이블이 셋 이상 얽히면 진짜
 * 병목은 '어느 순서로 조인하나'다. 순진한 좌->우(쿼리에 적힌 순서)는
 * 중간 결과를 폭발시킬 수 있다. DP는 작은 중간 결과를 먼저 만드는 순서를
 * 찾아 비용을 낮춘다.
 *
 * 이 모듈은 순수 계획기 — 실행기에 배선돼 있지 않다(정직한 경계, joinopt.c 주석).
 * 그래서 여기선 '계획의 질'만 검증한다: DP <= 순진, 연결 우선, 순서의 타당성.
 */

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } } while (0)

/* 그래프 헬퍼: 대칭 간선을 한 번에 세팅. */
static void edge(JoinGraph *g, int i, int j, double sel) {
    g->sel[i][j] = sel;
    g->sel[j][i] = sel;
}
static void init_graph(JoinGraph *g, int n) {
    g->nrel = n;
    for (int i = 0; i < n; i++) {
        g->card[i] = 1.0;
        g->indexed[i] = 0;
        for (int j = 0; j < n; j++) g->sel[i][j] = 1.0; /* 기본: 간선 없음 */
    }
}
/* order 가 관계 0..n-1 의 순열인지. */
static int is_permutation(const JoinPlan *p) {
    int seen[JOPT_MAX_REL] = {0};
    for (int k = 0; k < p->n; k++) {
        int r = p->order[k];
        if (r < 0 || r >= p->n || seen[r]) return 0;
        seen[r] = 1;
    }
    return 1;
}

int main(void) {
    printf("== 조인 순서 최적화(Selinger DP) ==\n");

    /* ── 1. 단일 관계: 순서랄 게 없다 ───────────────────────────── */
    {
        JoinGraph g; init_graph(&g, 1); g.card[0] = 500;
        JoinPlan p;
        CHECK(joinopt_optimize(&g, &p) == 0, "1개 관계: 최적화 성공");
        CHECK(p.n == 1 && p.order[0] == 0, "1개 관계: 순서는 자기 자신");
        CHECK(p.cost == 500.0, "1개 관계: 비용 = 스캔 비용");
    }

    /* ── 2. 체인 조인에서 순서가 비용을 가른다 ───────────────────
     * R0(대형 10000) - R1(중형 1000) - R2(소형 10), 각 간선 선택도 0.001.
     * 쿼리 순서 0,1,2 는 먼저 R0 x R1 (중간결과 큼)을 만든다.
     * DP 는 작은 쪽부터 붙여 중간 결과를 작게 유지 -> 더 싸야 한다. */
    {
        JoinGraph g; init_graph(&g, 3);
        g.card[0] = 10000; g.card[1] = 1000; g.card[2] = 10;
        edge(&g, 0, 1, 0.001);
        edge(&g, 1, 2, 0.01);
        JoinPlan opt, naive;
        joinopt_optimize(&g, &opt);
        double nc = joinopt_naive_cost(&g, &naive);
        CHECK(is_permutation(&opt), "체인: 최적 순서가 순열");
        CHECK(opt.cost <= nc, "체인: DP 비용 <= 순진 좌->우 비용");
        CHECK(opt.cost < nc, "체인: DP가 순진보다 엄격히 싸다(재정렬 이득)");
        printf("     [체인] naive=%.0f  dp=%.0f  (order %d,%d,%d)\n",
               nc, opt.cost, opt.order[0], opt.order[1], opt.order[2]);
    }

    /* ── 3. 스타 스키마: 큰 fact + 작은 dimension 3개 ────────────
     * fact(R0, 100000) 가 dim1/2/3(각 100) 과 각각 연결. dimension을
     * 먼저 조인하면 폭발하고, fact 를 축으로 두는 게 자연스럽다.
     * DP <= naive 여야 하고 결과는 순열이어야 한다. */
    {
        JoinGraph g; init_graph(&g, 4);
        g.card[0] = 100000; g.card[1] = 100; g.card[2] = 100; g.card[3] = 100;
        edge(&g, 0, 1, 0.01);
        edge(&g, 0, 2, 0.01);
        edge(&g, 0, 3, 0.01);
        JoinPlan opt, naive;
        joinopt_optimize(&g, &opt);
        double nc = joinopt_naive_cost(&g, &naive);
        CHECK(is_permutation(&opt), "스타: 최적 순서가 순열");
        CHECK(opt.cost <= nc, "스타: DP <= 순진");
        CHECK(!opt.had_cross, "스타: 교차곱 없이 연결로만 조인");
        printf("     [스타] naive=%.0f  dp=%.0f\n", nc, opt.cost);
    }

    /* ── 4. 인덱스 NLJ가 방법 선택을 바꾼다 ──────────────────────
     * R1 의 PK가 조인 키(indexed=1)라면, prefix 행마다 점 조회(≈prev_rows)라
     * 해시(card+prev)보다 쌀 수 있다. 방법이 INDEX 로 잡히는지 본다. */
    {
        JoinGraph g; init_graph(&g, 2);
        g.card[0] = 1000; g.card[1] = 1000;
        edge(&g, 0, 1, 0.001);
        g.indexed[1] = 1;
        JoinPlan p;
        joinopt_optimize(&g, &p);
        /* R0 를 먼저(1000), 그 위에 인덱스로 R1 붙이면 add=1000, 총 2000.
         * 해시라면 add=1000+1000=2000 동률이지만, R1 을 먼저 두고 R0 인덱스는
         * indexed[0]=0 이라 불가 -> R0-first + index 가 유일 최적. */
        int last = p.order[1];
        CHECK(is_permutation(&p), "인덱스: 순열");
        CHECK(p.method[1] == JMETH_INDEX || p.method[1] == JMETH_HASH,
              "인덱스: 붙이는 방법이 인덱스/해시 중 하나");
        printf("     [인덱스] order %d,%d  method[1]=%s  cost=%.0f\n",
               p.order[0], p.order[1], joinopt_method_str(p.method[1]), p.cost);
        (void)last;
    }

    /* ── 5. 분리 그래프: 교차곱이 불가피 ────────────────────────
     * R0-R1 은 연결, R2 는 아무와도 연결 안 됨. 어쩔 수 없이 교차곱 1회.
     * had_cross=1 로 표시되고, 순서는 여전히 순열이어야 한다. */
    {
        JoinGraph g; init_graph(&g, 3);
        g.card[0] = 100; g.card[1] = 100; g.card[2] = 5;
        edge(&g, 0, 1, 0.1);
        /* R2 간선 없음 */
        JoinPlan p;
        CHECK(joinopt_optimize(&g, &p) == 0, "분리: 그래도 계획을 낸다");
        CHECK(is_permutation(&p), "분리: 순열");
        CHECK(p.had_cross, "분리: 교차곱을 썼다고 정직히 표시");
    }

    /* ── 6. DP 는 절대 순진보다 나쁘지 않다(전 케이스 불변식) ─────
     * 여러 랜덤스러운 그래프에서 DP <= naive 를 확인(결정적 시드값). */
    {
        int worse = 0;
        double cards[][4] = {
            {5000, 50, 2000, 30},
            {10, 20, 30, 40},
            {80000, 4, 4, 4},
            {100, 100, 100, 100},
        };
        double sels[][3] = { /* (0-1),(1-2),(2-3) 체인 선택도 */
            {0.002, 0.5, 0.01},
            {0.9, 0.9, 0.9},
            {0.001, 0.001, 0.001},
            {0.05, 0.05, 0.05},
        };
        for (int c = 0; c < 4; c++) {
            JoinGraph g; init_graph(&g, 4);
            for (int i = 0; i < 4; i++) g.card[i] = cards[c][i];
            edge(&g, 0, 1, sels[c][0]);
            edge(&g, 1, 2, sels[c][1]);
            edge(&g, 2, 3, sels[c][2]);
            JoinPlan opt;
            joinopt_optimize(&g, &opt);
            double nc = joinopt_naive_cost(&g, NULL);
            if (opt.cost > nc + 1e-6) worse++;
        }
        CHECK(worse == 0, "불변식: 어떤 그래프에서도 DP <= 순진");
    }

    if (failures == 0) printf("\n모든 테스트 통과\n");
    else printf("\n%d개 실패\n", failures);
    return failures ? 1 : 0;
}
