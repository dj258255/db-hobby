#include "btree.h"
#include "bufpool.h"
#include "heap.h"
#include "pager.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * bench_clustered — 같은 데이터를 두 방식으로 저장해 접근 경로 비용을 대조한다.
 *
 *   힙(PostgreSQL식)   : 힙 파일 + PK 인덱스(key->RID) + 보조 인덱스(sk->RID).
 *                        조회 = 인덱스로 RID를 얻고 힙에서 행을 가져온다(간접).
 *   클러스터드(InnoDB식): 데이터를 PK B+Tree 리프에 PK 순서로 저장(key->row).
 *                        보조 인덱스는 RID가 아니라 PK를 든다(sk->PK). 조회 = 보조 -> PK -> 데이터.
 *
 * 핵심 대조(구조가 곧 성능):
 *   - PK 점 조회 : 클러스터드는 트리 한 번(행이 리프에 있음). 힙은 트리 + 힙 페치.  -> 클러스터드 유리
 *   - 보조 점 조회: 클러스터드는 트리 두 번(보조->PK->데이터). 힙은 트리 + 힙 페치.  -> 힙이 대등/유리
 *   - PK 범위    : 클러스터드는 리프 체인에 행이 붙어 있어 지역성 최고. 힙은 RID가 흩어진
 *                  페이지를 가리켜 랜덤 페치.                                   -> 클러스터드 압승
 * (행을 섞어 넣어 힙의 물리 순서가 PK 순서와 어긋나게 한다 — 실제 테이블처럼.)
 */

#define N 40000
#define FRAMES 32          /* 각 구조의 버퍼 풀(작게 잡아 지역성 차이가 드러나게) */
#define PT_ITERS 40000
#define RANGE 200
#define RANGE_ITERS 3000

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static unsigned long rng = 88172645463325252UL;
static unsigned long xs(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static int64_t rid_enc(RID r) { return (int64_t)r.page_id * 65536 + r.slot; }
static RID rid_dec(int64_t v) { RID r; r.page_id = (page_id_t)(v / 65536); r.slot = (uint16_t)(v % 65536); return r; }

/* 보조 키 = PK의 전단사(섞기). 순서가 PK와 달라 보조 스캔이 흩어지게. */
static int64_t sec_of(int64_t k) { return (int64_t)(((uint64_t)k * 2654435761u) % N); }

static void wipe(const char *p) { unlink(p); char b[600]; const char *s[] = {".wal"};
    for (size_t i=0;i<1;i++){ snprintf(b,sizeof b,"%s%s",p,s[i]); unlink(b);} }

/* 범위 스캔 콜백들 */
typedef struct { int64_t stop; int64_t sum; Heap *h; } HeapRange;
static int heap_range_visit(bkey_t key, bval_t val, void *ctx) {
    HeapRange *r = ctx;
    if (key >= r->stop) return 1;
    uint8_t buf[16]; uint16_t len; int64_t pay = 0;
    if (heap_get(r->h, rid_dec(val), buf, &len) == 0) memcpy(&pay, buf, 8);
    r->sum += pay;
    return 0;
}
typedef struct { int64_t stop; int64_t sum; } ClustRange;
static int clust_range_visit(bkey_t key, bval_t val, void *ctx) {
    ClustRange *r = ctx;
    if (key >= r->stop) return 1;
    r->sum += val; /* 행(payload)이 리프에 바로 있다 */
    return 0;
}

int main(void) {
    const char *hp = "build/bc_heap.db", *hpk = "build/bc_pk.idx", *hsk = "build/bc_sk.idx";
    const char *cp = "build/bc_clust.idx", *cs = "build/bc_csec.idx";
    wipe(hp); wipe(hpk); wipe(hsk); wipe(cp); wipe(cs);

    /* --- 힙 모델(PG식) --- */
    Pager hpager; pager_open(&hpager, hp);
    BufferPool *hbp = bufpool_create(&hpager, FRAMES);
    Heap heap; heap_init(&heap, hbp, &hpager, 0);
    BTree pk_idx, sk_idx; btree_open(&pk_idx, hpk); btree_open(&sk_idx, hsk);

    /* --- 클러스터드 모델(InnoDB식) --- */
    BTree clust, csec; btree_open(&clust, cp); btree_open(&csec, cs);

    /* 적재: 키를 섞어 넣어 힙의 물리 순서를 흩뜨린다 */
    int64_t *order = malloc(sizeof(int64_t) * N);
    for (int i = 0; i < N; i++) order[i] = i;
    for (int i = N - 1; i > 0; i--) { int j = (int)(xs() % (unsigned)(i + 1)); int64_t t = order[i]; order[i] = order[j]; order[j] = t; }

    double t0 = now_sec();
    for (int i = 0; i < N; i++) {
        int64_t k = order[i], pay = k * 10 + 1, sk = sec_of(k);
        uint8_t rec[8]; memcpy(rec, &pay, 8);
        RID rid; heap_insert(&heap, rec, 8, &rid);
        btree_insert(&pk_idx, k, rid_enc(rid));
        btree_insert(&sk_idx, sk, rid_enc(rid));
    }
    double heap_build = now_sec() - t0;

    t0 = now_sec();
    for (int i = 0; i < N; i++) {
        int64_t k = order[i], pay = k * 10 + 1, sk = sec_of(k);
        btree_insert(&clust, k, pay);   /* 행을 PK 트리에 바로 */
        btree_insert(&csec, sk, k);     /* 보조는 PK를 든다 */
    }
    double clust_build = now_sec() - t0;

    /* 정확성: 두 모델이 같은 데이터를 준다 */
    int ok = 1;
    for (int s = 0; s < 500; s++) {
        int64_t k = (int64_t)(xs() % N);
        int64_t e; uint8_t buf[16]; uint16_t len; int64_t hp_v = -1, cl_v = -2;
        if (btree_search(&pk_idx, k, &e) == 0 && heap_get(&heap, rid_dec(e), buf, &len) == 0) memcpy(&hp_v, buf, 8);
        btree_search(&clust, k, &cl_v);
        if (hp_v != k * 10 + 1 || cl_v != k * 10 + 1) ok = 0;
    }
    printf("  %s 두 모델이 동일 데이터 반환\n", ok ? "ok  " : "FAIL");

    /* ① PK 점 조회 */
    volatile int64_t sink = 0;
    t0 = now_sec();
    for (int i = 0; i < PT_ITERS; i++) {
        int64_t k = (int64_t)(xs() % N), e; uint8_t buf[16]; uint16_t len;
        if (btree_search(&pk_idx, k, &e) == 0 && heap_get(&heap, rid_dec(e), buf, &len) == 0) { int64_t v; memcpy(&v,buf,8); sink += v; }
    }
    double h_pk = now_sec() - t0;
    t0 = now_sec();
    for (int i = 0; i < PT_ITERS; i++) { int64_t k=(int64_t)(xs()%N), v; if (btree_search(&clust,k,&v)==0) sink+=v; }
    double c_pk = now_sec() - t0;

    /* ② 보조 점 조회 */
    t0 = now_sec();
    for (int i = 0; i < PT_ITERS; i++) {
        int64_t sk = (int64_t)(xs()%N), e; uint8_t buf[16]; uint16_t len;
        if (btree_search(&sk_idx, sk, &e)==0 && heap_get(&heap, rid_dec(e), buf, &len)==0){int64_t v;memcpy(&v,buf,8);sink+=v;}
    }
    double h_sk = now_sec() - t0;
    t0 = now_sec();
    for (int i = 0; i < PT_ITERS; i++) {
        int64_t sk=(int64_t)(xs()%N), pk, v;
        if (btree_search(&csec, sk, &pk)==0 && btree_search(&clust, pk, &v)==0) sink+=v; /* 보조->PK->데이터 */
    }
    double c_sk = now_sec() - t0;

    /* ③ PK 범위 스캔 */
    t0 = now_sec();
    for (int i = 0; i < RANGE_ITERS; i++) {
        int64_t start = (int64_t)(xs() % (N - RANGE));
        HeapRange r = {start + RANGE, 0, &heap};
        btree_seek_scan(&pk_idx, start, heap_range_visit, &r); sink += r.sum;
    }
    double h_rng = now_sec() - t0;
    t0 = now_sec();
    for (int i = 0; i < RANGE_ITERS; i++) {
        int64_t start = (int64_t)(xs() % (N - RANGE));
        ClustRange r = {start + RANGE, 0};
        btree_seek_scan(&clust, start, clust_range_visit, &r); sink += r.sum;
    }
    double c_rng = now_sec() - t0;

    printf("\n  N=%d행, 버퍼 풀 %d프레임/구조, 섞어 적재\n\n", N, FRAMES);
    printf("  %-22s %10s %12s   %s\n", "작업", "힙(PG)", "클러스터드", "");
    printf("  %-22s %9.3fs %11.3fs\n", "적재(build)", heap_build, clust_build);
    printf("  %-22s %9.3fs %11.3fs   %.2f배\n", "PK 점 조회", h_pk, c_pk, h_pk / c_pk);
    printf("  %-22s %9.3fs %11.3fs   %.2f배\n", "보조 점 조회", h_sk, c_sk, h_sk / c_sk);
    printf("  %-22s %9.3fs %11.3fs   %.2f배\n", "PK 범위(200행)", h_rng, c_rng, h_rng / c_rng);
    printf("\n  (배수 = 힙/클러스터드. >1이면 클러스터드가 빠름)  [sink=%lld]\n", (long long)sink);

    free(order);
    btree_close(&pk_idx); btree_close(&sk_idx); btree_close(&clust); btree_close(&csec);
    bufpool_destroy(hbp); pager_close(&hpager);
    wipe(hp); wipe(hpk); wipe(hsk); wipe(cp); wipe(cs);
    unlink(hp);
    return 0;
}
