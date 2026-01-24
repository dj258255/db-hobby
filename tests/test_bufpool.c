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

/* 페이지 첫 바이트에 마커를 쓰고/읽는 헬퍼 */
static void mark(void *page, uint8_t v) { ((uint8_t *)page)[0] = v; }
static uint8_t marker(const void *page) { return ((const uint8_t *)page)[0]; }

int main(void) {
    const char *path = "build/test_bufpool.db";
    unlink(path);

    Pager pgr;
    pager_open(&pgr, path);

    /* 프레임 2개짜리 작은 풀 — 교체를 강제로 일으키기 위해 */
    BufferPool *bp = bufpool_create(&pgr, 2);
    CHECK(bp != NULL, "bufpool_create");

    /* 페이지 3개를 만들고 각각 마커를 써서 dirty로 unpin */
    page_id_t p0, p1, p2;
    void *d0 = bufpool_new_page(bp, &p0);
    mark(d0, 0xA1);
    bufpool_unpin(bp, p0, 1);

    void *d1 = bufpool_new_page(bp, &p1);
    mark(d1, 0xB2);
    bufpool_unpin(bp, p1, 1);
    /* 이 시점 풀은 꽉 참: {p0, p1} */

    /* p0를 다시 가져오면 cache hit (아직 안 쫓겨남) */
    size_t hits_before = bufpool_hits(bp);
    void *d0b = bufpool_fetch(bp, p0);
    CHECK(bufpool_hits(bp) == hits_before + 1, "p0 재요청은 cache hit");
    CHECK(marker(d0b) == 0xA1, "p0 내용 유지");
    bufpool_unpin(bp, p0, 0);

    /* 세 번째 페이지 생성 -> 프레임 2개가 꽉 찼으니 LRU victim(p1)이 쫓겨난다.
     * p1은 dirty라 디스크로 flush되어야 한다. (p0는 방금 써서 더 최근) */
    void *d2 = bufpool_new_page(bp, &p2);
    mark(d2, 0xC3);
    bufpool_unpin(bp, p2, 1);

    /* p1을 다시 가져오면 miss(쫓겨났으니), 그런데 내용은 0xB2여야 한다
     * -> 쫓겨날 때 디스크에 제대로 flush됐다는 증거 */
    size_t miss_before = bufpool_misses(bp);
    void *d1b = bufpool_fetch(bp, p1);
    CHECK(bufpool_misses(bp) == miss_before + 1, "쫓겨난 p1 재요청은 miss");
    CHECK(marker(d1b) == 0xB2, "쫓겨난 dirty 페이지가 디스크에 flush됐다");
    bufpool_unpin(bp, p1, 0);

    /* flush_all 후 완전히 새 페이저로 읽어도 내용이 살아있어야 한다 */
    bufpool_flush_all(bp);
    bufpool_destroy(bp);
    pager_close(&pgr);

    Pager pgr2;
    pager_open(&pgr2, path);
    uint8_t buf[PAGE_SIZE];
    pager_read(&pgr2, p2, buf);
    CHECK(marker(buf) == 0xC3, "flush 후 새 페이저로 읽어도 p2 유지");
    pager_close(&pgr2);

    /* pin 보호: 두 프레임을 모두 pin하면 세 번째 요청은 자리 없음(NULL) */
    Pager pgr3;
    pager_open(&pgr3, path);
    BufferPool *bp2 = bufpool_create(&pgr3, 2);
    bufpool_fetch(bp2, p0); /* pin */
    bufpool_fetch(bp2, p1); /* pin — 둘 다 pin됨 */
    CHECK(bufpool_fetch(bp2, p2) == NULL, "모두 pin되면 자리 없음(NULL)");
    bufpool_unpin(bp2, p0, 0);
    bufpool_unpin(bp2, p1, 0);
    bufpool_destroy(bp2);
    pager_close(&pgr3);

    unlink(path);

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
