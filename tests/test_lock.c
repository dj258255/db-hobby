#include "lock.h"

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

/* txn 1, 2, 3을 인터리브하며 락 충돌을 검증한다. */
int main(void) {
    LockManager lm;
    lock_init(&lm);

    /* X 락은 다른 txn의 어떤 요청과도 충돌 */
    CHECK(lock_acquire(&lm, 1, "t", 5, LOCK_X) == 0, "T1: X(t,5) 획득");
    CHECK(lock_acquire(&lm, 2, "t", 5, LOCK_X) == -1, "T2: X(t,5) 충돌(T1이 X)");
    CHECK(lock_acquire(&lm, 2, "t", 5, LOCK_S) == -1, "T2: S(t,5) 충돌(T1이 X)");
    CHECK(lock_acquire(&lm, 2, "t", 6, LOCK_X) == 0, "T2: X(t,6) 다른 키라 OK");

    /* 같은 txn은 자기 락과 충돌하지 않음(재취득 idempotent) */
    CHECK(lock_acquire(&lm, 1, "t", 5, LOCK_X) == 0, "T1: X(t,5) 재취득 OK");
    CHECK(lock_acquire(&lm, 1, "t", 5, LOCK_S) == 0, "T1: 이미 X면 S 요청도 OK");

    /* 해제하면 다른 txn이 얻는다 */
    lock_release_all(&lm, 1);
    CHECK(!lock_held(&lm, 1, "t", 5), "T1 해제 후 (t,5) 안 쥠");
    CHECK(lock_acquire(&lm, 2, "t", 5, LOCK_X) == 0, "T1 해제 후 T2: X(t,5) 획득");

    /* S 락끼리는 호환(여러 reader) */
    lock_init(&lm);
    CHECK(lock_acquire(&lm, 1, "t", 7, LOCK_S) == 0, "T1: S(t,7)");
    CHECK(lock_acquire(&lm, 2, "t", 7, LOCK_S) == 0, "T2: S(t,7) 공유 OK");
    CHECK(lock_acquire(&lm, 3, "t", 7, LOCK_X) == -1, "T3: X(t,7) 충돌(S 보유자 있음)");
    lock_release_all(&lm, 1);
    CHECK(lock_acquire(&lm, 3, "t", 7, LOCK_X) == -1, "T2가 아직 S라 T3 X 여전히 충돌");
    lock_release_all(&lm, 2);
    CHECK(lock_acquire(&lm, 3, "t", 7, LOCK_X) == 0, "둘 다 해제되니 T3 X OK");

    /* S -> X 업그레이드: 단독이면 OK */
    lock_init(&lm);
    CHECK(lock_acquire(&lm, 1, "t", 9, LOCK_S) == 0, "T1: S(t,9)");
    CHECK(lock_acquire(&lm, 1, "t", 9, LOCK_X) == 0, "T1: S->X 업그레이드(단독) OK");
    CHECK(lock_acquire(&lm, 2, "t", 9, LOCK_S) == -1, "업그레이드 후 T2 S 충돌");

    /* S -> X 업그레이드: 다른 reader가 있으면 막힘 */
    lock_init(&lm);
    CHECK(lock_acquire(&lm, 1, "t", 11, LOCK_S) == 0, "T1: S(t,11)");
    CHECK(lock_acquire(&lm, 2, "t", 11, LOCK_S) == 0, "T2: S(t,11)");
    CHECK(lock_acquire(&lm, 1, "t", 11, LOCK_X) == -1, "T1: S->X 충돌(T2도 S 보유)");

    /* lost update 시나리오: 둘 다 같은 행을 쓰려 하면 한쪽이 막힌다 */
    lock_init(&lm);
    CHECK(lock_acquire(&lm, 1, "acct", 100, LOCK_X) == 0, "T1: 잔액 행 X 잠금");
    CHECK(lock_acquire(&lm, 2, "acct", 100, LOCK_X) == -1,
          "T2: 같은 잔액 행 X 충돌 -> lost update 방지");

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
