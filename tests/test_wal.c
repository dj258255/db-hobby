#include "wal.h"

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

static void fill(uint8_t *p, uint8_t v) { memset(p, v, PAGE_SIZE); }

int main(void) {
    const char *dp = "build/test_wal.db";
    const char *lp = "build/test_wal.log";
    unlink(dp);
    unlink(lp);

    uint8_t page[PAGE_SIZE], buf[PAGE_SIZE];
    Wal w;
    wal_open(&w, dp, lp);

    /* 1) 정상 커밋 -> 내구성 */
    wal_begin(&w);
    fill(page, 0xA1);
    wal_stage(&w, 1, page);
    CHECK(wal_commit(&w) == 0, "정상 커밋");
    wal_read(&w, 1, buf);
    CHECK(buf[0] == 0xA1, "커밋된 페이지가 데이터에 반영됨");
    wal_close(&w);

    wal_open(&w, dp, lp); /* 재오픈 */
    wal_read(&w, 1, buf);
    CHECK(buf[0] == 0xA1, "재오픈 후에도 유지 (내구성)");

    /* 2) 로그 fsync 직후 크래시 -> 복구가 redo */
    wal_begin(&w);
    fill(page, 0xB2);
    wal_stage(&w, 1, page);
    wal_test_crash_after_log = 1;
    wal_commit(&w); /* 로그+커밋마커+fsync, 데이터엔 적용 안 함 */
    wal_test_crash_after_log = 0;

    wal_read(&w, 1, buf);
    CHECK(buf[0] == 0xA1, "적용 전이라 데이터 파일은 아직 옛값(0xA1)");
    wal_close(&w); /* 크래시 — 커밋된 로그가 남는다 */

    wal_open(&w, dp, lp); /* 복구가 커밋된 변경을 redo */
    wal_read(&w, 1, buf);
    CHECK(buf[0] == 0xB2, "복구가 커밋된 변경을 재적용 (내구성)");

    /* 3) 커밋 전 크래시 -> 버려짐 (원자성) */
    wal_begin(&w);
    fill(page, 0xC3);
    wal_stage(&w, 1, page);
    wal_test_crash_before_commit = 1;
    wal_commit(&w); /* 페이지 로그만, 커밋 마커 없음 */
    wal_test_crash_before_commit = 0;
    wal_close(&w); /* 크래시 */

    wal_open(&w, dp, lp); /* 복구: 커밋 마커 없음 -> 버림 */
    wal_read(&w, 1, buf);
    CHECK(buf[0] == 0xB2, "커밋 안 된 변경은 흔적 없이 버려짐 (원자성)");

    wal_close(&w);
    unlink(dp);
    unlink(lp);

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
