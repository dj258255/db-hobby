#ifndef MINIDB_LOCK_H
#define MINIDB_LOCK_H

/*
 * 2PL 락 매니저 — (테이블, 키) 단위의 공유/배타 락.
 *
 * minidb는 단일 스레드라 OS 동시성은 없지만, 인터리브된 in-process 트랜잭션 사이의
 * 충돌을 탐지해 격리(직렬화)를 보인다. 충돌이면 acquire가 -1을 돌려준다("블록될 상황").
 *
 * 호환 행렬(같은 객체에 대해):
 *           보유 S   보유 X
 *   요청 S    OK      충돌
 *   요청 X   충돌     충돌
 * 같은 트랜잭션은 자기 자신과 충돌하지 않는다(S->X 업그레이드는 다른 보유자가 없을 때만).
 *
 * 2PL(strict): 트랜잭션은 끝까지 락을 쥐고, COMMIT/ROLLBACK에서 lock_release_all로 한꺼번에 푼다.
 */

#define LOCK_MAX 256
#define LOCK_NAME_LEN 64

typedef enum { LOCK_S, LOCK_X } LockMode; /* 공유(읽기) / 배타(쓰기) */

typedef struct {
    int txn; /* 락 소유 트랜잭션 id. 0이면 빈 칸 */
    char table[LOCK_NAME_LEN];
    long key;
    LockMode mode;
} LockEntry;

typedef struct {
    LockEntry entries[LOCK_MAX];
    int n;
} LockManager;

void lock_init(LockManager *lm);

/* txn이 (table,key)에 mode 락을 얻는다. 0 성공(또는 이미 충분히 보유), -1 충돌(다른 txn과). */
int lock_acquire(LockManager *lm, int txn, const char *table, long key, LockMode mode);

/* txn이 쥔 모든 락을 푼다(커밋/롤백 시). */
void lock_release_all(LockManager *lm, int txn);

/* txn이 (table,key)에 락을 쥐고 있나(테스트/디버그용). 1/0. */
int lock_held(const LockManager *lm, int txn, const char *table, long key);

#endif /* MINIDB_LOCK_H */
