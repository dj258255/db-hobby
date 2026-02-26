#include "btree.h"

#include <stdio.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg)                       \
    do {                                       \
        if (cond) {                            \
            printf("  ok   %s\n", msg);        \
        } else {                               \
            printf("  FAIL %s\n", msg);        \
            failures++;                        \
        }                                      \
    } while (0)

#define N 1000

typedef struct {
    long prev;
    int ascending;
    int count;
} ScanCtx;

static int scan_visit(bkey_t k, bval_t v, void *ctx_) {
    (void)v;
    ScanCtx *c = ctx_;
    if (c->count > 0 && k <= c->prev) {
        c->ascending = 0;
    }
    c->prev = k;
    c->count++;
    return 0;
}

typedef struct {
    int count;
    long sum;
} CollectCtx;

static int collect_visit(bkey_t k, bval_t v, void *ctx_) {
    (void)k;
    CollectCtx *c = ctx_;
    c->count++;
    c->sum += (long)v;
    return 0;
}

int main(void) {
    const char *path = "build/test_btree.idx";
    unlink(path);

    BTree bt;
    CHECK(btree_open(&bt, path) == 0, "btree_open");

    /* 0..N-1 삽입 (값 = 키*10). 노드당 8키라 다단계 분할이 일어난다 */
    for (int i = 0; i < N; i++) {
        btree_insert(&bt, i, (bval_t)i * 10);
    }

    /* 전부 검색되고 값이 맞아야 한다 */
    int all = 1;
    for (int i = 0; i < N; i++) {
        bval_t v;
        if (btree_search(&bt, i, &v) != 0 || v != (bval_t)i * 10) {
            all = 0;
        }
    }
    CHECK(all, "1000개 키 전부 검색되고 값 일치");

    bval_t v;
    CHECK(btree_search(&bt, N, &v) == -1, "없는 키(1000)는 -1");
    CHECK(btree_search(&bt, -5, &v) == -1, "없는 키(-5)는 -1");

    /* 갱신 */
    btree_insert(&bt, 500, 999999);
    CHECK(btree_search(&bt, 500, &v) == 0 && v == 999999, "키 500 값 갱신됨");

    /* 오름차순 스캔, 개수 정확 */
    ScanCtx sc = {0, 1, 0};
    btree_scan(&bt, scan_visit, &sc);
    CHECK(sc.count == N, "스캔 1000개");
    CHECK(sc.ascending, "스캔이 오름차순 (트리 구조 정상)");

    /* 영속성: 닫고 다시 열어도 검색·스캔이 된다 */
    btree_close(&bt);
    BTree bt2;
    btree_open(&bt2, path);
    CHECK(btree_search(&bt2, 777, &v) == 0 && v == 7770, "재오픈 후 검색");
    ScanCtx sc2 = {0, 1, 0};
    btree_scan(&bt2, scan_visit, &sc2);
    CHECK(sc2.count == N, "재오픈 후 스캔 1000개");
    btree_close(&bt2);

    /* ---- 중복 키(비유니크): btree_insert_dup + btree_find_all ---- */
    const char *dpath = "build/test_btree_dup.idx";
    char dwal[64];
    snprintf(dwal, sizeof dwal, "%s.wal", dpath);
    unlink(dpath);
    unlink(dwal);
    BTree dbt;
    btree_open(&dbt, dpath);
    btree_insert_dup(&dbt, 5, 100);
    btree_insert_dup(&dbt, 5, 101);
    btree_insert_dup(&dbt, 5, 102);
    btree_insert_dup(&dbt, 3, 30);
    btree_insert_dup(&dbt, 7, 70);
    btree_insert_dup(&dbt, 7, 71);
    {
        CollectCtx c = {0, 0};
        btree_find_all(&dbt, 5, collect_visit, &c);
        CHECK(c.count == 3 && c.sum == 303, "find_all(5) -> 값 3개(100+101+102)");
    }
    {
        CollectCtx c = {0, 0};
        btree_find_all(&dbt, 7, collect_visit, &c);
        CHECK(c.count == 2 && c.sum == 141, "find_all(7) -> 값 2개");
    }
    {
        CollectCtx c = {0, 0};
        btree_find_all(&dbt, 3, collect_visit, &c);
        CHECK(c.count == 1, "find_all(3) -> 값 1개");
    }
    {
        CollectCtx c = {0, 0};
        btree_find_all(&dbt, 9, collect_visit, &c);
        CHECK(c.count == 0, "find_all(없는 키) -> 0개");
    }
    /* 분할을 가로지르는 중복: 같은 키 50개 + 주변 키. 노드당 8키라 42가 여러 리프로 쪼개진다. */
    for (int i = 0; i < 20; i++) {
        btree_insert_dup(&dbt, 40, i);
    }
    for (int i = 0; i < 50; i++) {
        btree_insert_dup(&dbt, 42, 1000 + i);
    }
    for (int i = 0; i < 20; i++) {
        btree_insert_dup(&dbt, 44, i);
    }
    {
        CollectCtx c = {0, 0};
        btree_find_all(&dbt, 42, collect_visit, &c);
        CHECK(c.count == 50, "find_all: 분할 가로지르는 중복 50개 다 찾음(하한 탐색)");
    }
    btree_close(&dbt);
    unlink(dpath);
    unlink(dwal);

    unlink(path);

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
