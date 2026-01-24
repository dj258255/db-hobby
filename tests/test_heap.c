#include "heap.h"
#include "bufpool.h"
#include "pager.h"

#include <stdio.h>
#include <string.h>
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

/* 스캔으로 행 개수를 센다 */
typedef struct { int count; } CountCtx;
static int count_visit(RID rid, const void *rec, uint16_t len, void *ctx) {
    (void)rid; (void)rec; (void)len;
    ((CountCtx *)ctx)->count++;
    return 0;
}

static int scan_count(Heap *h) {
    CountCtx c = {0};
    heap_scan(h, count_visit, &c);
    return c.count;
}

int main(void) {
    const char *path = "build/test_heap.db";
    unlink(path);

    Pager pgr;
    pager_open(&pgr, path);
    BufferPool *bp = bufpool_create(&pgr, 8);
    Heap heap;
    heap_init(&heap, bp, &pgr, 0);

    /* 기본 삽입/조회 */
    RID ra, rb, rg;
    CHECK(heap_insert(&heap, "alpha", 5, &ra) == 0, "insert alpha");
    CHECK(heap_insert(&heap, "beta", 4, &rb) == 0, "insert beta");
    CHECK(heap_insert(&heap, "gamma", 5, &rg) == 0, "insert gamma");

    char buf[128];
    uint16_t len;
    CHECK(heap_get(&heap, ra, buf, &len) == 0 && len == 5 && memcmp(buf, "alpha", 5) == 0,
          "get alpha by RID");
    CHECK(scan_count(&heap) == 3, "스캔 3행");

    /* 멀티 페이지: 64바이트 행 200개 -> 여러 페이지에 걸친다 */
    char row[64];
    memset(row, 'x', sizeof(row));
    for (int i = 0; i < 200; i++) {
        RID r;
        if (heap_insert(&heap, row, sizeof(row), &r) != 0) {
            failures++;
            break;
        }
    }
    CHECK(scan_count(&heap) == 203, "스캔 203행 (멀티 페이지)");
    CHECK(pgr.num_pages > 1, "여러 페이지에 걸쳐 저장됨");

    /* 삭제 -> 스캔에서 빠지고, 조회도 실패 */
    CHECK(heap_delete(&heap, rb) == 0, "delete beta");
    CHECK(scan_count(&heap) == 202, "삭제 후 스캔 202행");
    CHECK(heap_get(&heap, rb, buf, &len) == -1, "삭제된 beta 조회 실패");

    /* 영속성: flush + 완전 재오픈 후에도 행이 살아있어야 한다 */
    bufpool_flush_all(bp);
    bufpool_destroy(bp);
    pager_close(&pgr);

    Pager pgr2;
    pager_open(&pgr2, path);
    BufferPool *bp2 = bufpool_create(&pgr2, 8);
    Heap heap2;
    heap_init(&heap2, bp2, &pgr2, 0);
    CHECK(scan_count(&heap2) == 202, "재오픈 후에도 202행");
    CHECK(heap_get(&heap2, ra, buf, &len) == 0 && memcmp(buf, "alpha", 5) == 0,
          "재오픈 후 alpha 조회");
    bufpool_destroy(bp2);
    pager_close(&pgr2);

    unlink(path);

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
