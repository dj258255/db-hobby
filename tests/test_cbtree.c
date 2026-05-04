#include "cbtree.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * 트랙 D — latch crabbing 동시성 B+Tree.
 * 노드보다 훨씬 많은 키를 분할이 끊임없이 일어나게 넣으면서, 여러 스레드가 동시에
 * 삽입/탐색한다. crabbing이 틀리면(조상 래치를 너무 일찍/늦게 놓으면) 분할 도중
 * 트리가 깨져 조회가 실패하거나 크래시한다. `make test-tsan`으로 data race도 계측.
 */

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } } while (0)

#define N 8000
#define T 8

/* --- 동시 삽입: 스레드 t는 k%T==t 인 키를 넣는다(분할은 공유 노드에서 경쟁) --- */
typedef struct { CBTree *t; int tid; } InsArg;
static void *ins(void *ap) {
    InsArg *a = ap;
    for (int k = a->tid; k < N; k += T) cbtree_insert(a->t, k, (int64_t)k * 10 + 1);
    return NULL;
}

/* --- 혼합: writer는 N..2N를 넣고, reader는 이미 든 0..N을 계속 찾는다(항상 성공해야) --- */
typedef struct { CBTree *t; int tid; int bad; } MixArg;
static void *mix_writer(void *ap) {
    MixArg *a = ap;
    /* writer는 T/2개 -> stride도 T/2라야 N..2N를 빠짐없이 덮는다 */
    for (int k = N + a->tid; k < 2 * N; k += T / 2) cbtree_insert(a->t, k, (int64_t)k * 10 + 1);
    return NULL;
}
static void *mix_reader(void *ap) {
    MixArg *a = ap;
    unsigned seed = (unsigned)(a->tid * 40503u + 7);
    for (int i = 0; i < 60000; i++) {
        int k = rand_r(&seed) % N;                 /* 이미 커밋된 키 */
        int64_t v;
        if (cbtree_search(a->t, k, &v) != 0 || v != (int64_t)k * 10 + 1) a->bad++;
    }
    return NULL;
}

int main(void) {
    /* --- 단일 스레드 정확성 (분할·범위) --- */
    CBTree *t = cbtree_create();
    for (int k = 0; k < 1000; k++) cbtree_insert(t, k, k * 10 + 1);
    int ok = 1;
    for (int k = 0; k < 1000; k++) { int64_t v; if (cbtree_search(t, k, &v) != 0 || v != k * 10 + 1) ok = 0; }
    CHECK(ok, "단일 스레드: 1000키 삽입 후 전부 조회 성공");
    int64_t v;
    CHECK(cbtree_search(t, 100000, &v) == -1, "없는 키 조회 -> -1");
    cbtree_insert(t, 500, 999); cbtree_search(t, 500, &v);
    CHECK(v == 999, "기존 키 재삽입 -> 값 갱신");
    CHECK(cbtree_range_count(t, 200, 299) == 100, "범위 [200,299] = 100키");
    cbtree_destroy(t);

    /* --- 동시 삽입 (분할 폭풍) --- */
    t = cbtree_create();
    pthread_t th[T];
    InsArg ia[T];
    for (int i = 0; i < T; i++) { ia[i] = (InsArg){t, i}; pthread_create(&th[i], NULL, ins, &ia[i]); }
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);

    int miss = 0;
    for (int k = 0; k < N; k++) { int64_t vv; if (cbtree_search(t, k, &vv) != 0 || vv != (int64_t)k * 10 + 1) miss++; }
    CHECK(miss == 0, "동시 삽입 8스레드×1000키: 전부 정확히 조회됨 (crabbing 무결성)");
    CHECK(cbtree_range_count(t, 0, N - 1) == N, "범위 스캔 총합 = N (리프 체인 온전)");

    /* --- 읽기/쓰기 혼합 (읽기 crabbing vs 쓰기 분할) --- */
    MixArg ma[T];
    pthread_t wr[T / 2], rd[T / 2];
    for (int i = 0; i < T / 2; i++) { ma[i] = (MixArg){t, i, 0}; pthread_create(&wr[i], NULL, mix_writer, &ma[i]); }
    for (int i = 0; i < T / 2; i++) { ma[T / 2 + i] = (MixArg){t, i, 0}; pthread_create(&rd[i], NULL, mix_reader, &ma[T / 2 + i]); }
    for (int i = 0; i < T / 2; i++) pthread_join(wr[i], NULL);
    int rbad = 0;
    for (int i = 0; i < T / 2; i++) { pthread_join(rd[i], NULL); rbad += ma[T / 2 + i].bad; }
    CHECK(rbad == 0, "쓰기 도중 읽기: 커밋된 키는 항상 찾힌다 (읽기가 분할에 안 깨짐)");

    int miss2 = 0;
    for (int k = 0; k < 2 * N; k++) { int64_t vv; if (cbtree_search(t, k, &vv) != 0 || vv != (int64_t)k * 10 + 1) miss2++; }
    CHECK(miss2 == 0, "혼합 후 0..2N 전부 조회됨");
    cbtree_destroy(t);

    if (failures == 0) { printf("\n전체 통과\n"); return 0; }
    printf("\n%d개 실패\n", failures);
    return 1;
}
