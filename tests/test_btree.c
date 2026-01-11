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

    unlink(path);

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
