#include "parscan.h"
#include "heap.h"
#include "bufpool.h"
#include "pager.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * parscan — 병렬 풀 스캔 독립 테스트.
 * 핵심 주장: 여러 워커가 disjoint 페이지 범위를 '동시에' 훑어도, 결과(RID 집합과
 * 순서)가 직렬 heap_scan과 '완전히 동일'하다. read-only + 스레드 안전 버퍼 풀이면
 * 전역 실행기 latch 없이도 안전하다는 것(= 굵은 latch를 계층별로 걷어낼 첫 발판).
 * ThreadSanitizer 빌드(test_parscan_tsan)로 데이터 레이스 부재도 검증한다.
 */

static int failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (cond) printf("  ok   %s\n", msg);                                   \
        else { printf("  FAIL %s\n", msg); failures++; }                        \
    } while (0)

/* 술어: key % 3 == 0 인 행만 (rec는 8바이트 int64 key) */
static int pred_div3(RID rid, const void *rec, uint16_t len, void *ctx) {
    (void)rid; (void)len; (void)ctx;
    int64_t k;
    memcpy(&k, rec, sizeof k);
    return (k % 3) == 0;
}

/* 직렬 기준선: heap_scan으로 같은 술어를 적용해 RID를 페이지 순서로 모은다 */
typedef struct { ParscanResult r; parscan_pred_fn pred; } SerialCtx;
static int serial_visit(RID rid, const void *rec, uint16_t len, void *ctx) {
    SerialCtx *c = ctx;
    if (!c->pred || c->pred(rid, rec, len, NULL)) {
        if (c->r.n == c->r.cap) {
            int64_t nc = c->r.cap ? c->r.cap * 2 : 64;
            c->r.rids = realloc(c->r.rids, (size_t)nc * sizeof(RID));
            c->r.cap = nc;
        }
        c->r.rids[c->r.n++] = rid;
    }
    return 0;
}

static int rid_eq(RID a, RID b) { return a.page_id == b.page_id && a.slot == b.slot; }

/* 두 결과가 RID 집합·순서까지 동일한지 */
static int same(const ParscanResult *a, const ParscanResult *b) {
    if (a->n != b->n) return 0;
    for (int64_t i = 0; i < a->n; i++)
        if (!rid_eq(a->rids[i], b->rids[i])) return 0;
    return 1;
}

int main(void) {
    const char *path = "build/test_parscan.db";
    unlink(path);

    Pager pgr;
    pager_open(&pgr, path);
    /* 풀을 넉넉히(테이블이 다 올라오게) — 워밍된 캐시에서 병렬 CPU 이득이 크고,
     * 각 워커가 페이지 하나를 pin하므로 프레임 >= nworkers 여야 한다. */
    BufferPool *bp = bufpool_create(&pgr, 4096);
    Heap heap;
    heap_init(&heap, bp, &pgr, 0);

    const int N = 6000;
    for (int i = 0; i < N; i++) {
        int64_t key = i;
        RID rid;
        if (heap_insert(&heap, &key, sizeof key, &rid) != 0) {
            printf("  FAIL insert %d\n", i);
            failures++;
            break;
        }
    }
    uint64_t npages = pgr.num_pages;
    printf("  info N=%d rows over %llu pages\n", N, (unsigned long long)npages);
    CHECK(npages >= 2, "여러 페이지에 걸침(병렬 분배가 의미 있음)");

    /* ── 직렬 기준선 ── */
    SerialCtx sc = {.pred = pred_div3};
    heap_scan(&heap, serial_visit, &sc);
    CHECK(sc.r.n == (N + 2) / 3, "직렬 스캔: key%3==0 행 수 정확");

    /* ── 병렬: 워커 수를 바꿔가며 직렬과 완전 동일한지 ── */
    int worker_counts[] = {1, 2, 3, 4, 8, 16};
    for (size_t wi = 0; wi < sizeof(worker_counts) / sizeof(worker_counts[0]); wi++) {
        int nw = worker_counts[wi];
        ParscanResult pr;
        int rc = parscan_collect(&heap, nw, pred_div3, NULL, &pr);
        char msg[96];
        snprintf(msg, sizeof msg, "병렬 스캔(%d워커): 직렬과 RID 집합·순서 완전 동일", nw);
        CHECK(rc == 0 && same(&pr, &sc.r), msg);
        parscan_result_free(&pr);
    }

    /* ── 술어 없이(NULL) 전체 스캔 = 모든 행 ── */
    ParscanResult full;
    int rcf = parscan_collect(&heap, 8, NULL, NULL, &full);
    CHECK(rcf == 0 && full.n == N, "병렬 전체 스캔(pred=NULL, 8워커): 전체 N행");
    /* 순서: 페이지 오름차순·슬롯 오름차순 → 첫 RID는 (0,0) */
    CHECK(full.n > 0 && full.rids[0].page_id == 0 && full.rids[0].slot == 0,
          "병렬 결과가 페이지 순서(첫 RID = (0,0))");
    parscan_result_free(&full);

    /* ── 빈 범위/경계: nworkers<1 클램프, 워커>페이지도 안전 ── */
    ParscanResult clamp;
    int rcc = parscan_collect(&heap, 0, pred_div3, NULL, &clamp); /* 0 -> 1로 클램프 */
    CHECK(rcc == 0 && same(&clamp, &sc.r), "nworkers=0 클램프(=1): 직렬과 동일");
    parscan_result_free(&clamp);

    free(sc.r.rids);
    bufpool_destroy(bp);
    pager_close(&pgr);
    unlink(path);

    if (failures == 0) { printf("\n모든 테스트 통과\n"); return 0; }
    printf("\n%d개 실패\n", failures);
    return 1;
}
