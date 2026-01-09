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

int main(void) {
    const char *path = "build/test_pager.db";
    unlink(path); /* 깨끗이 시작 */

    Pager p;
    CHECK(pager_open(&p, path) == 0, "pager_open");
    CHECK(p.num_pages == 0, "새 파일은 0 페이지");

    /* 페이지 할당 -> id 0 */
    page_id_t id0 = pager_allocate(&p);
    CHECK(id0 == 0, "첫 페이지 id == 0");
    CHECK(p.num_pages == 1, "할당 후 1 페이지");

    /* 패턴을 써서 다시 읽으면 같아야 한다 */
    uint8_t out[PAGE_SIZE];
    for (int i = 0; i < PAGE_SIZE; i++) {
        out[i] = (uint8_t)(i * 31 + 7);
    }
    CHECK(pager_write(&p, id0, out) == 0, "pager_write 페이지 0");

    uint8_t in[PAGE_SIZE];
    memset(in, 0, PAGE_SIZE);
    CHECK(pager_read(&p, id0, in) == 0, "pager_read 페이지 0");
    CHECK(memcmp(in, out, PAGE_SIZE) == 0, "쓴 내용 == 읽은 내용");

    /* 두 번째 페이지 */
    page_id_t id1 = pager_allocate(&p);
    CHECK(id1 == 1, "둘째 페이지 id == 1");

    pager_close(&p);

    /* 껐다 켜도 살아있어야 한다 (영속성) */
    Pager p2;
    CHECK(pager_open(&p2, path) == 0, "재오픈");
    CHECK(p2.num_pages == 2, "재오픈 시 2 페이지");
    uint8_t in2[PAGE_SIZE];
    CHECK(pager_read(&p2, id0, in2) == 0 && memcmp(in2, out, PAGE_SIZE) == 0,
          "재오픈 후 페이지 0 내용 유지");
    pager_close(&p2);

    unlink(path);

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
