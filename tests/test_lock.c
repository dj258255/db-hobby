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

    /* ---- 교착 탐지: wait-for 그래프 순환 ---- */

    /* 고전 교착: T1이 A 쥐고 B 기다림, T2가 B 쥐고 A 기다림 (T1<->T2) */
    lock_init(&lm);
    lock_acquire(&lm, 1, "A", 0, LOCK_X);
    lock_acquire(&lm, 2, "B", 0, LOCK_X);
    lock_wait_add(&lm, 1, "B", 0, LOCK_X); /* T1 -> T2 */
    lock_wait_add(&lm, 2, "A", 0, LOCK_X); /* T2 -> T1 */
    CHECK(lock_deadlock_victim(&lm) != 0, "교착 탐지(T1<->T2 순환) -> victim 반환");

    /* 단방향 대기는 교착 아님: T2가 T1을 기다리지만 T1은 안 기다림 */
    lock_init(&lm);
    lock_acquire(&lm, 1, "A", 0, LOCK_X);
    lock_wait_add(&lm, 2, "A", 0, LOCK_X); /* T2 -> T1, T1은 대기 없음 */
    CHECK(lock_deadlock_victim(&lm) == 0, "단방향 대기(T2->T1)는 교착 아님");

    /* 3중 순환: T1->T2->T3->T1 */
    lock_init(&lm);
    lock_acquire(&lm, 1, "A", 0, LOCK_X);
    lock_acquire(&lm, 2, "B", 0, LOCK_X);
    lock_acquire(&lm, 3, "C", 0, LOCK_X);
    lock_wait_add(&lm, 1, "B", 0, LOCK_X); /* T1 -> T2 */
    lock_wait_add(&lm, 2, "C", 0, LOCK_X); /* T2 -> T3 */
    lock_wait_add(&lm, 3, "A", 0, LOCK_X); /* T3 -> T1 */
    CHECK(lock_deadlock_victim(&lm) != 0, "3중 순환(T1->T2->T3->T1) 교착 탐지");

    /* 한 대기를 해소하면(victim abort) 순환이 끊겨 교착이 사라진다 */
    lock_wait_clear(&lm, 3);
    CHECK(lock_deadlock_victim(&lm) == 0, "한 대기 해소되면 교착 사라짐");

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
