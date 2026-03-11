#include "mvcc.h"

#include <stdio.h>

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
    TxnLog log;
    txnlog_init(&log);

    /* 안 커밋된 INSERT는 안 보인다 */
    txnlog_begin(&log, 1);
    CHECK(mvcc_visible(&log, 1, 0) == 0, "진행 중 트랜잭션이 만든 행 -> 안 보임");

    /* 커밋되면 보인다 */
    txnlog_commit(&log, 1);
    CHECK(mvcc_visible(&log, 1, 0) == 1, "커밋된 행(xmin 커밋, xmax 없음) -> 보임");

    /* abort된 INSERT는 안 보인다 (= INSERT 롤백, truncate 없이) */
    txnlog_begin(&log, 2);
    txnlog_abort(&log, 2);
    CHECK(mvcc_visible(&log, 2, 0) == 0, "abort된 트랜잭션이 만든 행 -> 안 보임(INSERT 롤백)");

    /* 커밋된 DELETE: xmax가 커밋됐으면 안 보인다 */
    txnlog_commit(&log, 3);
    CHECK(mvcc_visible(&log, 1, 3) == 0, "커밋된 트랜잭션이 지운 행(xmax 커밋) -> 안 보임");

    /* 아직 안 커밋된 DELETE: xmax가 진행 중이면 여전히 보인다 */
    txnlog_begin(&log, 4);
    CHECK(mvcc_visible(&log, 1, 4) == 1, "진행 중 트랜잭션이 지우는 중(xmax 미커밋) -> 아직 보임");

    /* abort된 DELETE: 행이 다시 보인다 (= DELETE 롤백) */
    txnlog_abort(&log, 4);
    CHECK(mvcc_visible(&log, 1, 4) == 1, "abort된 삭제(xmax abort) -> 다시 보임(DELETE 롤백)");

    /* UPDATE 시나리오: 옛 버전(xmin=1, xmax=5)과 새 버전(xmin=5, xmax=0), txn 5 커밋 */
    txnlog_commit(&log, 5);
    CHECK(mvcc_visible(&log, 1, 5) == 0, "UPDATE: 옛 버전(xmax=5 커밋) -> 안 보임");
    CHECK(mvcc_visible(&log, 5, 0) == 1, "UPDATE: 새 버전(xmin=5 커밋) -> 보임");

    /* UPDATE를 abort하면: 옛 버전 다시 보이고 새 버전 사라짐 */
    txnlog_init(&log);
    txnlog_commit(&log, 1); /* 옛 버전 생성자 */
    txnlog_begin(&log, 6);
    txnlog_abort(&log, 6);  /* UPDATE 트랜잭션 abort */
    CHECK(mvcc_visible(&log, 1, 6) == 1, "UPDATE 롤백: 옛 버전(xmax=6 abort) -> 다시 보임");
    CHECK(mvcc_visible(&log, 6, 0) == 0, "UPDATE 롤백: 새 버전(xmin=6 abort) -> 사라짐");

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
