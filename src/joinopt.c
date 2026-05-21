/* joinopt.c — 조인 순서 최적화 (Selinger DP)
 *
 * ── 정직한 경계 ─────────────────────────────────────────────────────
 * 이 파일은 '최적 조인 순서와 비용'을 계산하는 순수 계획기다. db.c의
 * exec_select_join 은 여전히 쿼리에 적힌 순서대로 실행한다. 여기 결과를
 * 실행기에 배선하려면 세 가지가 더 필요하고, 그게 프론티어다:
 *   1) 실행 순서 재정렬: tabs[]/off[]/comb 재배치.
 *   2) ON 술어 재매핑: on_at/on_bt 등 인덱스가 새 순서를 가리키게.
 *   3) LEFT/OUTER JOIN 순서 제약 존중: 외부 조인은 자유롭게 못 바꾼다
 *      (A LEFT JOIN B 를 B 먼저로 돌리면 의미가 바뀐다). 현재 모델은
 *      inner equi-join 만 가정한다.
 * 그래서 이 모듈은 22·23편처럼 독립 모듈 + 자체 테스트로 서고, 블로그가
 * '왜 여기까지'를 설명한다.
 * ────────────────────────────────────────────────────────────────────
 *
 * DP 요지:
 *   dp[S] = 관계 집합 S 를 조인하는 최소 비용 left-deep 계획.
 *   기저:   dp[{i}] = card[i]              (i 를 seq scan)
 *   전이:   dp[S ∪ {r}] = min over r∉S, r가 S에 연결됨:
 *               dp[S].cost + join_cost(dp[S].rows, r)
 *   답:     dp[전체집합]
 * 관계가 n개면 2^n 부분집합만 채운다(순열 n! 대신). */
#include "joinopt.h"

#define JOPT_INF (1e300)

const char *joinopt_method_str(JoinMethod m) {
    switch (m) {
        case JMETH_INDEX: return "Index NLJ";
        case JMETH_HASH:  return "Hash Join";
        default:          return "Nested Loop";
    }
}

/* r 를 집합 mask(이미 조인된 관계들)에 붙일 때, 그 사이 조인 술어들의 결합
 * 선택도. 연결 간선이 하나도 없으면 1.0(=교차곱)을 돌려주고 *connected=0. */
static double combined_sel(const JoinGraph *g, int mask, int r, int *connected) {
    double s = 1.0;
    int conn = 0;
    for (int x = 0; x < g->nrel; x++) {
        if (!(mask & (1 << x))) continue;
        if (g->sel[r][x] < 1.0) { /* r·x 사이 조인 간선 존재 */
            s *= g->sel[r][x];
            conn = 1;
        }
    }
    if (connected) *connected = conn;
    return s;
}

/* prefix 계획(행 수 prev_rows)에 관계 r 을 붙이는 최소 비용과 방법.
 * connected=0(교차곱)이면 중첩 루프만 가능. */
static double join_step_cost(const JoinGraph *g, int mask, int r, double prev_rows,
                             int connected, JoinMethod *meth) {
    double cr = g->card[r];
    (void)mask;
    /* 중첩 루프: 외부 prefix 행마다 r 전체를 훑는다. */
    double nested = prev_rows * cr;
    if (!connected) { /* 교차곱: 인덱스/해시는 등식 키가 없어 못 쓴다 */
        *meth = JMETH_SCAN;
        return nested;
    }
    /* 해시 조인: r 로 해시를 한 번 짓고(cr), prefix 행마다 프로브(prev_rows). */
    double hash = cr + prev_rows;
    /* 인덱스 NLJ: r 의 PK가 조인 키면 prefix 행마다 점 조회 1회(≈prev_rows). */
    double best = hash;
    *meth = JMETH_HASH;
    if (g->indexed[r]) {
        double idx = prev_rows * 1.0;
        if (idx < best) { best = idx; *meth = JMETH_INDEX; }
    }
    if (nested < best) { best = nested; *meth = JMETH_SCAN; }
    return best;
}

static int valid_graph(const JoinGraph *g) {
    if (!g || g->nrel < 1 || g->nrel > JOPT_MAX_REL) return 0;
    for (int i = 0; i < g->nrel; i++) {
        if (g->card[i] < 0) return 0;
    }
    return 1;
}

int joinopt_optimize(const JoinGraph *g, JoinPlan *out) {
    if (!valid_graph(g) || !out) return -1;
    int n = g->nrel;
    int full = (1 << n) - 1;

    /* mask 인덱스 DP 테이블. 2^n <= 4096. */
    static double dp_cost[1 << JOPT_MAX_REL];
    static double dp_rows[1 << JOPT_MAX_REL];
    static int dp_last[1 << JOPT_MAX_REL];   /* 이 계획에서 마지막에 붙인 관계 */
    static int dp_prev[1 << JOPT_MAX_REL];   /* 그 전 부분집합 mask */
    static JoinMethod dp_meth[1 << JOPT_MAX_REL]; /* 마지막 관계를 붙인 방법 */
    static int dp_cross[1 << JOPT_MAX_REL];  /* 이 계획에 교차곱이 있었나 */

    for (int m = 0; m <= full; m++) dp_cost[m] = JOPT_INF;

    /* 기저: 단일 관계는 seq scan. */
    for (int i = 0; i < n; i++) {
        int m = 1 << i;
        dp_cost[m] = g->card[i];
        dp_rows[m] = g->card[i];
        dp_last[m] = i;
        dp_prev[m] = 0;
        dp_meth[m] = JMETH_SCAN;
        dp_cross[m] = 0;
    }

    /* 전이: mask 를 증가 순으로 훑으면, 어떤 부분집합(비트 하나 뺀 것)도
     * 값이 더 작아 이미 확정돼 있다. */
    for (int mask = 1; mask <= full; mask++) {
        if (dp_cost[mask] >= JOPT_INF) continue;
        if (mask == full) continue;
        /* 연결 확장을 먼저 시도. 하나도 없으면(그래프 분리) 교차곱 허용. */
        for (int pass = 0; pass < 2; pass++) {
            int any_connected = 0;
            for (int r = 0; r < n; r++) {
                if (mask & (1 << r)) continue;
                int connected = 0;
                double csel = combined_sel(g, mask, r, &connected);
                if (pass == 0 && !connected) continue;      /* 1차: 연결만 */
                if (pass == 1 && connected) continue;        /* 2차: 교차곱만 */
                if (connected) any_connected = 1;

                JoinMethod meth;
                double add = join_step_cost(g, mask, r, dp_rows[mask], connected, &meth);
                double nrows = dp_rows[mask] * g->card[r] * csel;
                double ncost = dp_cost[mask] + add;
                int nmask = mask | (1 << r);
                if (ncost < dp_cost[nmask]) {
                    dp_cost[nmask] = ncost;
                    dp_rows[nmask] = nrows;
                    dp_last[nmask] = r;
                    dp_prev[nmask] = mask;
                    dp_meth[nmask] = meth;
                    dp_cross[nmask] = dp_cross[mask] || (!connected);
                }
            }
            if (pass == 0 && any_connected) break; /* 연결 확장이 있으면 교차곱 패스 생략 */
        }
    }

    if (dp_cost[full] >= JOPT_INF) return -1;

    /* 역추적: full 에서 prev 를 따라가며 순서를 뒤에서 앞으로 복원. */
    out->n = n;
    out->cost = dp_cost[full];
    out->rows = dp_rows[full];
    out->had_cross = dp_cross[full];
    int m = full;
    for (int pos = n - 1; pos >= 0; pos--) {
        out->order[pos] = dp_last[m];
        out->method[pos] = dp_meth[m];
        m = dp_prev[m];
    }
    out->method[0] = JMETH_SCAN; /* 첫 관계는 기저 스캔 */
    return 0;
}

double joinopt_naive_cost(const JoinGraph *g, JoinPlan *out) {
    if (!valid_graph(g)) return JOPT_INF;
    int n = g->nrel;
    double cost = g->card[0];
    double rows = g->card[0];
    int cross = 0;
    if (out) {
        out->n = n;
        out->order[0] = 0;
        out->method[0] = JMETH_SCAN;
    }
    int mask = 1; /* {0} */
    for (int k = 1; k < n; k++) {
        int r = k; /* 쿼리 순서 그대로 */
        int connected = 0;
        double csel = combined_sel(g, mask, r, &connected);
        JoinMethod meth;
        double add = join_step_cost(g, mask, r, rows, connected, &meth);
        cost += add;
        rows *= g->card[r] * csel;
        if (!connected) cross = 1;
        if (out) {
            out->order[k] = r;
            out->method[k] = meth;
        }
        mask |= (1 << r);
    }
    if (out) {
        out->cost = cost;
        out->rows = rows;
        out->had_cross = cross;
    }
    return cost;
}
