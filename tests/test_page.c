#include "page.h"
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

/* 슬롯에서 레코드를 꺼내 문자열로 비교하는 헬퍼 */
static int rec_equals(const void *page, uint16_t slot, const char *expect) {
    const void *rec;
    uint16_t len;
    if (slotpage_get(page, slot, &rec, &len) != 0) {
        return 0;
    }
    return len == (uint16_t)strlen(expect) && memcmp(rec, expect, len) == 0;
}

int main(void) {
    uint8_t page[PAGE_SIZE];
    slotpage_init(page);
    CHECK(slotpage_num_slots(page) == 0, "빈 페이지는 슬롯 0개");

    /* 삽입 -> 슬롯 번호 0, 1 */
    int s0 = slotpage_insert(page, "hello", 5);
    int s1 = slotpage_insert(page, "world!!", 7);
    CHECK(s0 == 0, "첫 레코드 슬롯 0");
    CHECK(s1 == 1, "둘째 레코드 슬롯 1");
    CHECK(slotpage_num_slots(page) == 2, "슬롯 2개");

    /* 조회 */
    CHECK(rec_equals(page, 0, "hello"), "슬롯 0 == hello");
    CHECK(rec_equals(page, 1, "world!!"), "슬롯 1 == world!!");

    /* 삭제 -> 슬롯 0은 사라지고 슬롯 1은 유지 */
    CHECK(slotpage_delete(page, 0) == 0, "슬롯 0 삭제");
    const void *r;
    uint16_t l;
    CHECK(slotpage_get(page, 0, &r, &l) == -1, "삭제된 슬롯 0 조회 실패");
    CHECK(rec_equals(page, 1, "world!!"), "슬롯 1은 그대로");

    /* 공간 소진: 꽉 찰 때까지 넣다가 -1이 나야 한다 */
    int inserted = 0;
    char buf[64];
    memset(buf, 'x', sizeof(buf));
    while (slotpage_insert(page, buf, sizeof(buf)) >= 0) {
        inserted++;
    }
    CHECK(inserted > 0, "공간이 남아있는 동안은 삽입 성공");
    CHECK(slotpage_free_space(page) < (uint16_t)(sizeof(buf) + 4), "꽉 차면 더 못 넣음");

    /* 페이저와 통합: 디스크에 쓰고 다시 읽어도 레코드가 살아있어야 한다 */
    const char *path = "build/test_page.db";
    unlink(path);
    Pager pgr;
    pager_open(&pgr, path);
    page_id_t pid = pager_allocate(&pgr);
    pager_write(&pgr, pid, page);
    pager_close(&pgr);

    Pager pgr2;
    pager_open(&pgr2, path);
    uint8_t reloaded[PAGE_SIZE];
    pager_read(&pgr2, pid, reloaded);
    pager_close(&pgr2);
    unlink(path);
    CHECK(rec_equals(reloaded, 1, "world!!"), "디스크 왕복 후에도 슬롯 1 유지");

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
