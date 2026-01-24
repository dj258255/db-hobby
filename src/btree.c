#include "btree.h"

#include <stdint.h>
#include <string.h>

/* 노드당 최대 키 수. 학습용으로 작게 잡아 분할/다단계가 잘 보이게 한다.
 * (진짜 DB는 페이지를 꽉 채워 수백 개. 알고리즘은 동일하다.) */
#define BT_MAX_KEYS 8

/* 페이지에 그대로 덮어 해석하는 노드. 삽입 후 분할을 위해 배열에 +1 여유칸을 둔다. */
typedef struct {
    uint8_t is_leaf;
    uint8_t _pad;
    uint16_t num_keys;
    uint32_t _pad2;
    uint64_t next_leaf; /* 리프 형제 페이지 id (0 = 없음). 내부 노드에선 미사용 */
    bkey_t keys[BT_MAX_KEYS + 1];
    union {
        bval_t values[BT_MAX_KEYS + 1];     /* 리프 */
        uint64_t children[BT_MAX_KEYS + 2]; /* 내부 노드의 자식 페이지 id들 */
    } u;
} BTNode;

static BTNode *fetch(BTree *bt, page_id_t pid) {
    return (BTNode *)bufpool_fetch(bt->bp, pid);
}

static void write_root(BTree *bt, page_id_t root) {
    uint64_t *meta = (uint64_t *)bufpool_fetch(bt->bp, 0);
    meta[0] = root;
    bufpool_unpin(bt->bp, 0, 1);
}

int btree_open(BTree *bt, const char *path) {
    if (pager_open(&bt->pager, path) != 0) {
        return -1;
    }
    bt->bp = bufpool_create(&bt->pager, 32);
    if (!bt->bp) {
        pager_close(&bt->pager);
        return -1;
    }
    if (bt->pager.num_pages == 0) {
        /* 새 인덱스: page 0 = 메타, page 1 = 빈 루트 리프 */
        page_id_t meta_pid, root_pid;
        bufpool_new_page(bt->bp, &meta_pid); /* page 0 */
        bufpool_unpin(bt->bp, meta_pid, 1);
        BTNode *root = (BTNode *)bufpool_new_page(bt->bp, &root_pid); /* page 1 */
        root->is_leaf = 1;
        root->num_keys = 0;
        root->next_leaf = 0;
        bufpool_unpin(bt->bp, root_pid, 1);
        bt->root = root_pid;
        write_root(bt, root_pid);
    } else {
        uint64_t *meta = (uint64_t *)bufpool_fetch(bt->bp, 0);
        bt->root = meta[0];
        bufpool_unpin(bt->bp, 0, 0);
    }
    return 0;
}

void btree_close(BTree *bt) {
    if (bt->bp) {
        bufpool_flush_all(bt->bp);
        bufpool_destroy(bt->bp);
    }
    pager_close(&bt->pager);
}

/* pid 서브트리에 (key,val)을 넣는다. 노드가 분할되면 1을 반환하고 올라갈 분리키
 * (*sep_out)와 새 오른쪽 페이지(*right_out)를 채운다. 분할 없으면 0, 오류 -1. */
static int node_insert(BTree *bt, page_id_t pid, bkey_t key, bval_t val,
                       bkey_t *sep_out, page_id_t *right_out) {
    BTNode *n = fetch(bt, pid);
    if (!n) {
        return -1;
    }

    if (n->is_leaf) {
        int i = 0;
        while (i < n->num_keys && n->keys[i] < key) {
            i++;
        }
        if (i < n->num_keys && n->keys[i] == key) {
            n->u.values[i] = val; /* 갱신 */
            bufpool_unpin(bt->bp, pid, 1);
            return 0;
        }
        for (int j = n->num_keys; j > i; j--) {
            n->keys[j] = n->keys[j - 1];
            n->u.values[j] = n->u.values[j - 1];
        }
        n->keys[i] = key;
        n->u.values[i] = val;
        n->num_keys++;

        if (n->num_keys <= BT_MAX_KEYS) {
            bufpool_unpin(bt->bp, pid, 1);
            return 0;
        }
        /* 리프 분할: 뒤 절반을 새 리프로 옮기고 첫 키를 위로 복사 */
        int total = n->num_keys;
        int left = total / 2;
        int right = total - left;
        page_id_t rpid;
        BTNode *r = (BTNode *)bufpool_new_page(bt->bp, &rpid);
        r->is_leaf = 1;
        r->num_keys = right;
        for (int j = 0; j < right; j++) {
            r->keys[j] = n->keys[left + j];
            r->u.values[j] = n->u.values[left + j];
        }
        r->next_leaf = n->next_leaf;
        n->num_keys = left;
        n->next_leaf = rpid;
        *sep_out = r->keys[0];
        *right_out = rpid;
        bufpool_unpin(bt->bp, rpid, 1);
        bufpool_unpin(bt->bp, pid, 1);
        return 1;
    }

    /* 내부 노드: 내려갈 자식을 고른다 */
    int i = 0;
    while (i < n->num_keys && key >= n->keys[i]) {
        i++;
    }
    page_id_t child = n->u.children[i];
    bkey_t sep;
    page_id_t cr;
    int sp = node_insert(bt, child, key, val, &sep, &cr);
    if (sp < 0) {
        bufpool_unpin(bt->bp, pid, 0);
        return -1;
    }
    if (sp == 0) {
        bufpool_unpin(bt->bp, pid, 0); /* 자식이 안 쪼개짐 → 이 노드 그대로 */
        return 0;
    }
    /* 자식이 쪼개짐 → (sep, cr)을 i 위치에 끼운다 */
    for (int j = n->num_keys; j > i; j--) {
        n->keys[j] = n->keys[j - 1];
    }
    for (int j = n->num_keys + 1; j > i + 1; j--) {
        n->u.children[j] = n->u.children[j - 1];
    }
    n->keys[i] = sep;
    n->u.children[i + 1] = cr;
    n->num_keys++;

    if (n->num_keys <= BT_MAX_KEYS) {
        bufpool_unpin(bt->bp, pid, 1);
        return 0;
    }
    /* 내부 노드 분할: 가운데 키는 위로 올린다(복사 아님) */
    int total = n->num_keys;
    int mid = total / 2;
    bkey_t push = n->keys[mid];
    int right_keys = total - mid - 1;
    page_id_t rpid;
    BTNode *r = (BTNode *)bufpool_new_page(bt->bp, &rpid);
    r->is_leaf = 0;
    r->num_keys = right_keys;
    for (int j = 0; j < right_keys; j++) {
        r->keys[j] = n->keys[mid + 1 + j];
    }
    for (int j = 0; j < right_keys + 1; j++) {
        r->u.children[j] = n->u.children[mid + 1 + j];
    }
    n->num_keys = mid;
    *sep_out = push;
    *right_out = rpid;
    bufpool_unpin(bt->bp, rpid, 1);
    bufpool_unpin(bt->bp, pid, 1);
    return 1;
}

int btree_insert(BTree *bt, bkey_t key, bval_t val) {
    bkey_t sep;
    page_id_t right;
    int sp = node_insert(bt, bt->root, key, val, &sep, &right);
    if (sp < 0) {
        return -1;
    }
    if (sp == 1) {
        /* 루트가 쪼개짐 → 새 루트(높이 +1) */
        page_id_t nr;
        BTNode *root = (BTNode *)bufpool_new_page(bt->bp, &nr);
        root->is_leaf = 0;
        root->num_keys = 1;
        root->keys[0] = sep;
        root->u.children[0] = bt->root;
        root->u.children[1] = right;
        bufpool_unpin(bt->bp, nr, 1);
        bt->root = nr;
        write_root(bt, nr);
    }
    return 0;
}

void btree_reload_root(BTree *bt) {
    uint64_t *meta = (uint64_t *)bufpool_fetch(bt->bp, 0);
    bt->root = meta[0];
    bufpool_unpin(bt->bp, 0, 0);
}

int btree_search(BTree *bt, bkey_t key, bval_t *out) {
    page_id_t pid = bt->root;
    for (;;) {
        BTNode *n = fetch(bt, pid);
        if (!n) {
            return -1;
        }
        if (n->is_leaf) {
            int rc = -1;
            for (int i = 0; i < n->num_keys; i++) {
                if (n->keys[i] == key) {
                    if (out) {
                        *out = n->u.values[i];
                    }
                    rc = 0;
                    break;
                }
            }
            bufpool_unpin(bt->bp, pid, 0);
            return rc;
        }
        int i = 0;
        while (i < n->num_keys && key >= n->keys[i]) {
            i++;
        }
        page_id_t child = n->u.children[i];
        bufpool_unpin(bt->bp, pid, 0);
        pid = child;
    }
}

int btree_scan(BTree *bt, btree_visit_fn visit, void *ctx) {
    /* 맨 왼쪽 리프로 내려간다 */
    page_id_t pid = bt->root;
    for (;;) {
        BTNode *n = fetch(bt, pid);
        if (n->is_leaf) {
            bufpool_unpin(bt->bp, pid, 0);
            break;
        }
        page_id_t c = n->u.children[0];
        bufpool_unpin(bt->bp, pid, 0);
        pid = c;
    }
    /* 리프 체인을 따라 오름차순으로 훑는다 */
    while (pid != 0) {
        BTNode *n = fetch(bt, pid);
        for (int i = 0; i < n->num_keys; i++) {
            int r = visit(n->keys[i], n->u.values[i], ctx);
            if (r != 0) {
                bufpool_unpin(bt->bp, pid, 0);
                return r;
            }
        }
        page_id_t nxt = n->next_leaf;
        bufpool_unpin(bt->bp, pid, 0);
        pid = nxt;
    }
    return 0;
}
