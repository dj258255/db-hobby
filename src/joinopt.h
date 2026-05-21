/* joinopt — 조인 순서 최적화 (Selinger 스타일 동적 계획법)
 *
 * 21편(비용 옵티마이저)은 '한 테이블을 어떻게 읽나(seq vs index)'만 골랐다.
 * 이 모듈은 그 위층 질문 — '여러 테이블을 어떤 순서로 조인하나' — 를 푼다.
 *
 * 관계 n개를 왼쪽부터 하나씩 붙이는 left-deep 트리를, 부분집합 비트마스크
 * DP로 최적화한다. n! 순열 대신 2^n 부분집합만 채운다.
 *
 * 정직한 경계: 이 모듈은 '최적 순서와 그 비용'을 계산하는 계획기(planner)일
 * 뿐, db.c의 실행기(exec_select_join)에 배선돼 있지 않다. 실행기는 여전히
 * 쿼리에 적힌 순서대로 돈다. 배선(순서 재정렬 + ON 술어 재매핑 + LEFT JOIN
 * 순서 제약 존중)은 명시적 프론티어다 — joinopt.c 상단 주석 참고. */
#ifndef JOINOPT_H
#define JOINOPT_H

#define JOPT_MAX_REL 12 /* DP는 2^n. 현실적 상한(=4096 부분집합)으로 못박는다. */

/* 조인할 관계들의 그래프. 카디널리티와 관계쌍 사이 술어 선택도로 기술한다. */
typedef struct {
    int nrel;                                  /* 관계 개수 (<= JOPT_MAX_REL) */
    double card[JOPT_MAX_REL];                 /* 지역 술어 적용 후 기본 행 수 */
    /* sel[i][j] = i·j 사이 조인 술어들의 결합 선택도. 1.0 = 술어 없음(=간선 없음,
     * 붙이면 교차곱). 0<sel<1 = 조인 간선. 대칭이어야 한다(sel[i][j]==sel[j][i]). */
    double sel[JOPT_MAX_REL][JOPT_MAX_REL];
    /* indexed[j]=1 이면 j를 앞선 집합에 붙일 때 j의 PK 점 조회(인덱스 NLJ)를
     * 쓸 수 있다(21편의 Index Point Lookup, 프로브당 비용≈1). */
    int indexed[JOPT_MAX_REL];
} JoinGraph;

typedef enum { JMETH_SCAN, JMETH_HASH, JMETH_INDEX } JoinMethod;

typedef struct {
    int n;                          /* == g->nrel */
    int order[JOPT_MAX_REL];        /* 고른 left-deep 순서(관계 id 나열) */
    JoinMethod method[JOPT_MAX_REL];/* order[k]를 붙일 때 고른 조인 방법(k>=1) */
    double cost;                    /* 추정 총 비용 */
    double rows;                    /* 추정 결과 카디널리티 */
    int had_cross;                  /* 교차곱을 피할 수 없어 한 번이라도 썼나 */
} JoinPlan;

/* 최적 left-deep 순서를 DP로 찾는다. 성공 0, 잘못된 입력 -1.
 * 연결된 확장을 우선하고, 연결 불가일 때만 교차곱을 허용(had_cross=1). */
int joinopt_optimize(const JoinGraph *g, JoinPlan *out);

/* 순진한 좌->우 순서(0,1,2,...,n-1)의 비용. 같은 비용 모델로 계산해
 * DP 결과와 대조하는 기준선. out에 그 순서의 계획을 채운다(NULL이면 생략). */
double joinopt_naive_cost(const JoinGraph *g, JoinPlan *out);

/* 방법 이름(디버그/EXPLAIN용). */
const char *joinopt_method_str(JoinMethod m);

#endif
