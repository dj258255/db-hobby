#include "lock.h"

#include <stdio.h>
#include <string.h>

void lock_init(LockManager *lm) {
    lm->n = 0;
    memset(lm->entries, 0, sizeof(lm->entries));
    lm->nwaits = 0;
    memset(lm->waits, 0, sizeof(lm->waits));
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

/* ---- 교착 탐지 ---- */

void lock_wait_add(LockManager *lm, int txn, const char *table, long key, LockMode mode) {
    for (int i = 0; i < lm->nwaits; i++) { /* 같은 txn의 옛 대기는 교체(한 번에 하나만 기다린다) */
        if (lm->waits[i].txn == txn) {
            snprintf(lm->waits[i].table, LOCK_NAME_LEN, "%s", table);
            lm->waits[i].key = key;
            lm->waits[i].mode = mode;
            return;
        }
    }
    if (lm->nwaits >= LOCK_MAX_WAITS) {
        return;
    }
    LockWait *w = &lm->waits[lm->nwaits++];
    w->txn = txn;
    snprintf(w->table, LOCK_NAME_LEN, "%s", table);
    w->key = key;
    w->mode = mode;
}

void lock_wait_clear(LockManager *lm, int txn) {
    for (int i = 0; i < lm->nwaits; i++) {
        if (lm->waits[i].txn == txn) {
            lm->waits[i] = lm->waits[--lm->nwaits]; /* 끝 칸을 당겨와 빈틈 메움 */
            return;
        }
    }
}

/* txn이 기다리는 락의 충돌 보유자들(= txn이 기다리는 트랜잭션들)을 out에 모은다. */
static int wait_targets(const LockManager *lm, int txn, int *out, int max) {
    int n = 0;
    const LockWait *w = NULL;
    for (int i = 0; i < lm->nwaits; i++) {
        if (lm->waits[i].txn == txn) {
            w = &lm->waits[i];
            break;
        }
    }
    if (!w) {
        return 0;
    }
    for (int i = 0; i < lm->n; i++) {
        const LockEntry *e = &lm->entries[i];
        if (e->txn == 0 || e->txn == txn || e->key != w->key || strcmp(e->table, w->table) != 0) {
            continue;
        }
        int conflict = (w->mode == LOCK_X) || (e->mode == LOCK_X); /* 호환 행렬 */
        if (!conflict) {
            continue;
        }
        int dup = 0;
        for (int j = 0; j < n; j++) {
            if (out[j] == e->txn) {
                dup = 1;
            }
        }
        if (!dup && n < max) {
            out[n++] = e->txn;
        }
    }
    return n;
}

/* path를 따라 wait-for 그래프를 DFS. txn이 path에 다시 나오면 순환 -> 그 txn 반환. */
static int dfs_cycle(const LockManager *lm, int txn, int *path, int depth) {
    for (int i = 0; i < depth; i++) {
        if (path[i] == txn) {
            return txn; /* 이미 경로에 있음 = 순환 */
        }
    }
    if (depth >= LOCK_MAX_WAITS) {
        return 0;
    }
    path[depth] = txn;
    int targets[LOCK_MAX];
    int nt = wait_targets(lm, txn, targets, LOCK_MAX);
    for (int i = 0; i < nt; i++) {
        int v = dfs_cycle(lm, targets[i], path, depth + 1);
        if (v) {
            return v;
        }
    }
    return 0;
}

int lock_deadlock_victim(LockManager *lm) {
    int path[LOCK_MAX_WAITS];
    for (int i = 0; i < lm->nwaits; i++) {
        int v = dfs_cycle(lm, lm->waits[i].txn, path, 0);
        if (v) {
            return v;
        }
    }
    return 0;
}
