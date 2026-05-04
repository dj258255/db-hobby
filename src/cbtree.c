#include "cbtree.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define ORDER 4   /* 노드당 최대 키 수(작게 잡아 분할이 자주 나게) */
#define MAXH 40   /* 최대 트리 높이(래치 스택 크기) */

typedef struct Node {
    pthread_rwlock_t latch;
    int is_leaf;
    int n;                          /* 키 개수 */
    int64_t key[ORDER + 1];         /* 분할 직전 +1 여유 */
    struct Node *child[ORDER + 2];  /* 내부 노드의 자식 */
    int64_t val[ORDER + 1];         /* 리프의 값 */
    struct Node *next;              /* 리프 형제 체인 */
} Node;

struct CBTree {
    /* header->child[0] 이 진짜 루트. 루트를 읽거나 바꾸려면 header latch를 먼저 잡는다
     * — 루트 분할(높이 증가)도 header latch 안에서 포인터만 갈아끼우면 돼 우아하다. */
    Node *header;
};

static Node *node_new(int is_leaf) {
    Node *n = calloc(1, sizeof(Node));
    pthread_rwlock_init(&n->latch, NULL);
    n->is_leaf = is_leaf;
    return n;
}
static void rlock(Node *n) { pthread_rwlock_rdlock(&n->latch); }
static void wlock(Node *n) { pthread_rwlock_wrlock(&n->latch); }
static void unlock(Node *n) { pthread_rwlock_unlock(&n->latch); }

/* 내부 노드에서 key로 내려갈 자식 인덱스 */
static int child_idx(const Node *n, int64_t key) {
    int i = 0;
    while (i < n->n && key >= n->key[i]) i++;
    return i;
}

CBTree *cbtree_create(void) {
    CBTree *t = calloc(1, sizeof(CBTree));
    t->header = node_new(0);
    t->header->child[0] = node_new(1); /* 빈 리프 루트 */
    return t;
}

static void node_free(Node *n) {
    if (!n) return;
    if (!n->is_leaf) {
        for (int i = 0; i <= n->n; i++) node_free(n->child[i]);
    }
    pthread_rwlock_destroy(&n->latch);
    free(n);
}
void cbtree_destroy(CBTree *t) {
    if (!t) return;
    node_free(t->header->child[0]);
    pthread_rwlock_destroy(&t->header->latch);
    free(t->header);
    free(t);
}

/* ---- 탐색: 읽기 crabbing (자식 잡고 부모 놓기) ---- */
int cbtree_search(CBTree *t, int64_t key, int64_t *out) {
    rlock(t->header);
    Node *cur = t->header->child[0];
    rlock(cur);
    unlock(t->header);
    while (!cur->is_leaf) {
        Node *c = cur->child[child_idx(cur, key)];
        rlock(c);       /* 자식을 먼저 잡고 */
        unlock(cur);    /* 부모를 놓는다 (lock coupling) */
        cur = c;
    }
    int rc = -1;
    for (int i = 0; i < cur->n; i++) {
        if (cur->key[i] == key) { if (out) *out = cur->val[i]; rc = 0; break; }
    }
    unlock(cur);
    return rc;
}

/* ---- 삽입: 쓰기 crabbing (pessimistic) ---- */
int cbtree_insert(CBTree *t, int64_t key, int64_t val) {
    Node *stack[MAXH];
    int sp = 0;

    wlock(t->header);
    stack[sp++] = t->header;
    Node *cur = t->header->child[0];
    wlock(cur);
    stack[sp++] = cur;

    /* 리프까지 내려가며, 자식이 '안전'(여유 있음)하면 조상 래치를 전부 놓는다. */
    while (!cur->is_leaf) {
        Node *c = cur->child[child_idx(cur, key)];
        wlock(c);
        if (c->n < ORDER) {                 /* 안전: 이 자식은 안 쪼개져 -> 위로 안 번진다 */
            for (int k = 0; k < sp; k++) unlock(stack[k]);
            sp = 0;                          /* 조상 전부 해제, 스택은 c부터 새로 */
        }
        stack[sp++] = c;
        cur = c;
    }

    /* cur = 리프(래치 보유). stack엔 cur + (해제 안 된) 불안전 조상들이 있다. */
    int i = 0;
    while (i < cur->n && cur->key[i] < key) i++;
    if (i < cur->n && cur->key[i] == key) {  /* 이미 있음 -> 갱신 */
        cur->val[i] = val;
        for (int k = 0; k < sp; k++) unlock(stack[k]);
        return 0;
    }
    for (int j = cur->n; j > i; j--) { cur->key[j] = cur->key[j - 1]; cur->val[j] = cur->val[j - 1]; }
    cur->key[i] = key; cur->val[i] = val; cur->n++;

    /* 분할 전파 */
    int64_t up_key = 0;
    Node *up_right = NULL;
    if (cur->n > ORDER) {
        int total = cur->n, left = total / 2, right = total - left;
        Node *r = node_new(1);
        r->n = right;
        for (int j = 0; j < right; j++) { r->key[j] = cur->key[left + j]; r->val[j] = cur->val[left + j]; }
        cur->n = left;
        r->next = cur->next; cur->next = r;
        up_key = r->key[0]; up_right = r;
    }

    int si = sp - 2; /* 리프의 부모부터 위로 */
    while (up_right && si >= 0) {
        Node *p = stack[si];
        if (p == t->header) {                /* 루트가 쪼개짐 -> 새 루트(높이 +1) */
            Node *nr = node_new(0);
            nr->n = 1; nr->key[0] = up_key;
            nr->child[0] = t->header->child[0];
            nr->child[1] = up_right;
            t->header->child[0] = nr;
            up_right = NULL;
            break;
        }
        /* (up_key, up_right)를 내부 노드 p에 끼운다 */
        int pi = 0;
        while (pi < p->n && p->key[pi] < up_key) pi++;
        for (int j = p->n; j > pi; j--) p->key[j] = p->key[j - 1];
        for (int j = p->n + 1; j > pi + 1; j--) p->child[j] = p->child[j - 1];
        p->key[pi] = up_key; p->child[pi + 1] = up_right; p->n++;
        if (p->n <= ORDER) { up_right = NULL; break; } /* 여기서 흡수 -> 전파 끝 */
        /* 내부 노드 분할: 가운데 키는 위로 올린다 */
        int total2 = p->n, mid = total2 / 2, rk = total2 - mid - 1;
        int64_t push = p->key[mid];
        Node *r2 = node_new(0);
        r2->n = rk;
        for (int j = 0; j < rk; j++) r2->key[j] = p->key[mid + 1 + j];
        for (int j = 0; j < rk + 1; j++) r2->child[j] = p->child[mid + 1 + j];
        p->n = mid;
        up_key = push; up_right = r2;
        si--;
    }

    for (int k = 0; k < sp; k++) unlock(stack[k]);
    return 0;
}

/* ---- 범위: lo가 든 리프로 내려간 뒤 리프 체인을 crabbing으로 훑는다 ---- */
int64_t cbtree_range_count(CBTree *t, int64_t lo, int64_t hi) {
    rlock(t->header);
    Node *cur = t->header->child[0];
    rlock(cur);
    unlock(t->header);
    while (!cur->is_leaf) {
        Node *c = cur->child[child_idx(cur, lo)];
        rlock(c); unlock(cur); cur = c;
    }
    int64_t cnt = 0;
    while (cur) {
        int past = 0;
        for (int i = 0; i < cur->n; i++) {
            if (cur->key[i] >= lo && cur->key[i] <= hi) cnt++;
            if (cur->key[i] > hi) past = 1;
        }
        if (past) { unlock(cur); break; }
        Node *nx = cur->next;
        if (nx) rlock(nx);   /* 다음 리프를 먼저 잡고 */
        unlock(cur);         /* 현재를 놓는다 */
        cur = nx;
    }
    return cnt;
}
