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

    /* 4) no-force: 로그에 커밋 구간이 여러 개 쌓인 채 크래시 -> 복구가 순서대로 redo */
    wal_begin(&w);
    fill(page, 0xD4);
    wal_stage(&w, 1, page);
    CHECK(wal_commit(&w) == 0, "커밋 1 (no-force — 로그가 이력으로 남는다)");
    wal_begin(&w);
    fill(page, 0xE5);
    wal_stage(&w, 1, page);
    wal_test_crash_after_log = 1;
    wal_commit(&w); /* 두 번째 커밋: 마커 fsync 후 크래시(데이터 미적용) */
    wal_test_crash_after_log = 0;
    wal_close(&w); /* 로그엔 [D4 커밋][E5 커밋] 두 구간이 있다 */

    wal_open(&w, dp, lp); /* 복구가 커밋 구간들을 커밋 순서대로 재적용 */
    wal_read(&w, 1, buf);
    CHECK(buf[0] == 0xE5, "다중 커밋 로그 redo -> 마지막 커밋 값 (no-force 내구성)");

    /* 5) no-force + steal 롤백: 내 기록만 잘리고 앞선 커밋 이력은 보존 */
    wal_begin(&w);
    fill(page, 0x66);
    CHECK(wal_steal(&w, 1, page) == 0, "steal: before-image 로깅 후 미커밋 값을 디스크로");
    wal_read(&w, 1, buf);
    CHECK(buf[0] == 0x66, "steal된 미커밋 값이 디스크에 보인다");
    CHECK(wal_undo(&w) == 0, "롤백(undo)");
    wal_read(&w, 1, buf);
    CHECK(buf[0] == 0xE5, "before-image로 원복 (트랜잭션 시작 오프셋부터만 undo)");
    wal_close(&w);
    wal_open(&w, dp, lp); /* 재오픈해도 커밋 값 그대로 */
    wal_read(&w, 1, buf);
    CHECK(buf[0] == 0xE5, "롤백 후 재오픈 -> 마지막 커밋 값 유지");

    /* 6) undo 도중 '재크래시' -> 다음 복구가 처음부터 다시 돌아도 수렴 (CLR 불필요 증명)
     * 페이지 2개를 커밋해 두고(A1,B1), 트랜잭션이 둘 다 steal(A2,B2)한 채 크래시.
     * 첫 복구는 undo를 하나만 적용하고 죽는다(주입) -> 반쯤 되돌린 위험한 상태.
     * 두 번째 복구가 before-image를 처음부터 재적용 -> 둘 다 A1,B1로 정확히 복원.
     * 이게 가능한 건 before-image 덮어쓰기가 idempotent라서다 — CLR이 지키려는
     * 성질을 페이지 전체 물리 로깅이 공짜로 준다. */
    wal_begin(&w);
    fill(page, 0xA1);
    wal_stage(&w, 1, page);
    fill(page, 0xB1);
    wal_stage(&w, 2, page);
    CHECK(wal_commit(&w) == 0, "두 페이지 커밋 (A1, B1)");
    wal_close(&w);
    wal_open(&w, dp, lp); /* 열기 = 체크포인트: 로그를 비워 둔다 */

    wal_begin(&w);
    fill(page, 0xA2);
    CHECK(wal_steal(&w, 1, page) == 0, "페이지1 steal (미커밋 A2)");
    fill(page, 0xB2);
    CHECK(wal_steal(&w, 2, page) == 0, "페이지2 steal (미커밋 B2)");
    wal_close(&w); /* 크래시 — 로그: [BEGIN][UNDO A1][UNDO B1], 디스크: A2,B2 */

    wal_test_crash_in_undo = 1;
    wal_open(&w, dp, lp); /* 복구가 undo 하나만 적용하고 '재크래시' */
    wal_test_crash_in_undo = 0;
    wal_read(&w, 1, buf);
    CHECK(buf[0] == 0xA1, "1차 복구(중단됨): 페이지1은 되돌아감");
    wal_read(&w, 2, buf);
    CHECK(buf[0] == 0xB2, "1차 복구(중단됨): 페이지2는 아직 미커밋 값 (반쯤 undo)");
    wal_close(&w);

    wal_open(&w, dp, lp); /* 2차 복구: 같은 undo를 처음부터 다시 -> 수렴 */
    wal_read(&w, 1, buf);
    CHECK(buf[0] == 0xA1, "2차 복구: 페이지1 정확 (idempotent 재적용)");
    wal_read(&w, 2, buf);
    CHECK(buf[0] == 0xB1, "2차 복구: 페이지2도 복원 — CLR 없이 수렴");

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
