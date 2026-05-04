#include "btree.h"
#include "bufpool.h"
#include "heap.h"
#include "pager.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * 트랙 B — 클러스터드(InnoDB식) 접근 경로의 정확성.
 * 힙(PG식: 인덱스->RID->힙)과 클러스터드(InnoDB식: 데이터가 PK 리프에, 보조는 PK를 듦)를
 * 같은 데이터로 만들고, 세 경로가 같은 답을 주는지 확인한다. (성능 대조는 `make bench-clustered`)
 */

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } } while (0)

#define N 1000
static int64_t rid_enc(RID r) { return (int64_t)r.page_id * 65536 + r.slot; }
static RID rid_dec(int64_t v) { RID r; r.page_id = (page_id_t)(v / 65536); r.slot = (uint16_t)(v % 65536); return r; }
static int64_t sec_of(int64_t k) { return (int64_t)(((uint64_t)k * 2654435761u) % N); }

typedef struct { int64_t lo, hi, cnt, sum; Heap *h; } HR;
static int heap_rv(bkey_t key, bval_t val, void *c) {
    HR *r = c; if (key >= r->hi) return 1;
    uint8_t b[16]; uint16_t l; int64_t p = 0;
    if (heap_get(r->h, rid_dec(val), b, &l) == 0) memcpy(&p, b, 8);
    r->cnt++; r->sum += p; return 0;
}
typedef struct { int64_t hi, cnt, sum; } CR;
static int clust_rv(bkey_t key, bval_t val, void *c) {
    CR *r = c; if (key >= r->hi) return 1; r->cnt++; r->sum += val; return 0;
}

int main(void) {
    const char *hp="build/tc_h.db",*pk="build/tc_pk.idx",*sk="build/tc_sk.idx",*cl="build/tc_cl.idx",*cs="build/tc_cs.idx";
    const char *all[]={hp,pk,sk,cl,cs}; char wp[600];
    for (int i=0;i<5;i++){unlink(all[i]);snprintf(wp,sizeof wp,"%s.wal",all[i]);unlink(wp);}

    Pager pgr; pager_open(&pgr, hp);
    BufferPool *bp = bufpool_create(&pgr, 16);
    Heap heap; heap_init(&heap, bp, &pgr, 0);
    BTree pk_idx, sk_idx, clust, csec;
    btree_open(&pk_idx, pk); btree_open(&sk_idx, sk); btree_open(&clust, cl); btree_open(&csec, cs);

    for (int64_t k = 0; k < N; k++) {
        int64_t pay = k * 10 + 1, s = sec_of(k);
        uint8_t rec[8]; memcpy(rec, &pay, 8);
        RID rid; heap_insert(&heap, rec, 8, &rid);
        btree_insert(&pk_idx, k, rid_enc(rid));  /* 힙: key->RID */
        btree_insert(&sk_idx, s, rid_enc(rid));  /* 힙: sec->RID */
        btree_insert(&clust, k, pay);            /* 클러스터드: key->데이터 */
        btree_insert(&csec, s, k);               /* 클러스터드: sec->PK */
    }

    /* --- PK 점 조회: 힙(간접) vs 클러스터드(직접) 같은 값 --- */
    int pk_ok = 1;
    for (int64_t k = 0; k < N; k += 7) {
        int64_t e, hv = -1, cv = -2; uint8_t b[16]; uint16_t l;
        if (btree_search(&pk_idx, k, &e) == 0 && heap_get(&heap, rid_dec(e), b, &l) == 0) memcpy(&hv, b, 8);
        btree_search(&clust, k, &cv);
        if (hv != k * 10 + 1 || cv != k * 10 + 1) pk_ok = 0;
    }
    CHECK(pk_ok, "PK 점 조회: 힙(인덱스->RID->힙) = 클러스터드(리프 직접)");

    /* --- 보조 점 조회: 클러스터드는 보조->PK->데이터 2단 --- */
    int sk_ok = 1;
    for (int64_t k = 0; k < N; k += 7) {
        int64_t s = sec_of(k), e, hv = -1, pkv = -1, cv = -2; uint8_t b[16]; uint16_t l;
        if (btree_search(&sk_idx, s, &e) == 0 && heap_get(&heap, rid_dec(e), b, &l) == 0) memcpy(&hv, b, 8);
        if (btree_search(&csec, s, &pkv) == 0) btree_search(&clust, pkv, &cv); /* 2단: sec->PK->데이터 */
        if (hv != k * 10 + 1 || pkv != k || cv != k * 10 + 1) sk_ok = 0;
    }
    CHECK(sk_ok, "보조 점 조회: 클러스터드 보조->PK->데이터가 힙과 같은 답 (InnoDB 이중 조회)");

    /* --- PK 범위: 두 모델이 같은 개수·합 --- */
    HR hr = {100, 300, 0, 0, &heap};
    btree_seek_scan(&pk_idx, 100, heap_rv, &hr);
    CR cr = {300, 0, 0};
    btree_seek_scan(&clust, 100, clust_rv, &cr);
    CHECK(hr.cnt == 200 && cr.cnt == 200, "PK 범위 [100,300): 둘 다 200행");
    CHECK(hr.sum == cr.sum, "PK 범위 합계 일치 (클러스터드=힙, 경로만 다름)");

    /* --- 없는 키 --- */
    int64_t v;
    CHECK(btree_search(&clust, N + 5, &v) == -1, "클러스터드: 없는 PK -> 못 찾음");

    btree_close(&pk_idx); btree_close(&sk_idx); btree_close(&clust); btree_close(&csec);
    bufpool_destroy(bp); pager_close(&pgr);
    for (int i=0;i<5;i++){unlink(all[i]);snprintf(wp,sizeof wp,"%s.wal",all[i]);unlink(wp);}

    if (failures == 0) { printf("\n전체 통과\n"); return 0; }
    printf("\n%d개 실패\n", failures);
    return 1;
}
