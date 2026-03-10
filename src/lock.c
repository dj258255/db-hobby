#include "lock.h"

#include <stdio.h>
#include <string.h>

void lock_init(LockManager *lm) {
    lm->n = 0;
    memset(lm->entries, 0, sizeof(lm->entries));
}

static int same_obj(const LockEntry *e, const char *table, long key) {
    return e->key == key && strcmp(e->table, table) == 0;
}

int lock_acquire(LockManager *lm, int txn, const char *table, long key, LockMode mode) {
    /* 1) 같은 txn이 이미 이 객체에 쥔 락을 찾고, 다른 txn의 락 모드를 모은다. */
    LockEntry *mine = NULL;
    int other_s = 0, other_x = 0;
    for (int i = 0; i < lm->n; i++) {
        LockEntry *e = &lm->entries[i];
        if (e->txn == 0 || !same_obj(e, table, key)) {
            continue;
        }
        if (e->txn == txn) {
            mine = e;
        } else if (e->mode == LOCK_X) {
            other_x = 1;
        } else {
            other_s = 1;
        }
    }

    if (mine) {
        /* 이미 보유: 요청이 S거나 이미 X면 충분하다. */
        if (mode == LOCK_S || mine->mode == LOCK_X) {
            return 0;
        }
        /* S -> X 업그레이드는 다른 보유자가 없을 때만 */
        if (other_s || other_x) {
            return -1;
        }
        mine->mode = LOCK_X;
        return 0;
    }

    /* 2) 새 락: 호환 행렬 검사. */
    if (mode == LOCK_X && (other_s || other_x)) {
        return -1; /* 쓰기는 누구와도 충돌 */
    }
    if (mode == LOCK_S && other_x) {
        return -1; /* 읽기는 X와 충돌 */
    }

    /* 3) 부여: 빈 칸에 기록(없으면 끝에 추가). */
    int slot = -1;
    for (int i = 0; i < lm->n; i++) {
        if (lm->entries[i].txn == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (lm->n >= LOCK_MAX) {
            return -1; /* 락 테이블이 꽉 참 */
        }
        slot = lm->n++;
    }
    lm->entries[slot].txn = txn;
    snprintf(lm->entries[slot].table, LOCK_NAME_LEN, "%s", table);
    lm->entries[slot].key = key;
    lm->entries[slot].mode = mode;
    return 0;
}

void lock_release_all(LockManager *lm, int txn) {
    for (int i = 0; i < lm->n; i++) {
        if (lm->entries[i].txn == txn) {
            lm->entries[i].txn = 0; /* 빈 칸으로 */
        }
    }
}

int lock_held(const LockManager *lm, int txn, const char *table, long key) {
    for (int i = 0; i < lm->n; i++) {
        const LockEntry *e = &lm->entries[i];
        if (e->txn == txn && e->key == key && strcmp(e->table, table) == 0) {
            return 1;
        }
    }
    return 0;
}
