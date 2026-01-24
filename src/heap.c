#include "heap.h"
#include "page.h"

#include <string.h>

void heap_init(Heap *h, BufferPool *bp, Pager *pager, page_id_t first_page) {
    h->bp = bp;
    h->pager = pager;
    h->first_page = first_page;
}

int heap_insert(Heap *h, const void *rec, uint16_t len, RID *rid_out) {
    page_id_t target;
    void *page;

    /* 1) 데이터 페이지가 하나라도 있으면 마지막 페이지에 먼저 시도한다. */
    if (h->pager->num_pages > h->first_page) {
        target = h->pager->num_pages - 1;
        page = bufpool_fetch(h->bp, target);
        if (!page) {
            return -1;
        }
        int slot = slotpage_insert(page, rec, len);
        if (slot >= 0) {
            bufpool_unpin(h->bp, target, 1); /* 수정함 */
            rid_out->page_id = target;
            rid_out->slot = (uint16_t)slot;
            return 0;
        }
        /* 마지막 페이지가 꽉 찼다 — 손 떼고 새 페이지로 */
        bufpool_unpin(h->bp, target, 0);
    }

    /* 2) 새 페이지를 할당해 거기에 넣는다. */
    page = bufpool_new_page(h->bp, &target);
    if (!page) {
        return -1;
    }
    slotpage_init(page); /* 갓 할당된 페이지를 빈 슬롯 페이지로 초기화 */
    int slot = slotpage_insert(page, rec, len);
    bufpool_unpin(h->bp, target, 1);
    if (slot < 0) {
        return -1; /* 빈 페이지에도 안 들어가는 큰 행 */
    }
    rid_out->page_id = target;
    rid_out->slot = (uint16_t)slot;
    return 0;
}

int heap_get(Heap *h, RID rid, void *buf, uint16_t *len_out) {
    void *page = bufpool_fetch(h->bp, rid.page_id);
    if (!page) {
        return -1;
    }
    const void *rec;
    uint16_t len;
    int rc = slotpage_get(page, rid.slot, &rec, &len);
    if (rc == 0) {
        memcpy(buf, rec, len);
        if (len_out) {
            *len_out = len;
        }
    }
    bufpool_unpin(h->bp, rid.page_id, 0);
    return rc;
}

int heap_delete(Heap *h, RID rid) {
    void *page = bufpool_fetch(h->bp, rid.page_id);
    if (!page) {
        return -1;
    }
    int rc = slotpage_delete(page, rid.slot);
    bufpool_unpin(h->bp, rid.page_id, rc == 0 ? 1 : 0);
    return rc;
}

int heap_scan(Heap *h, heap_visit_fn visit, void *ctx) {
    uint64_t np = h->pager->num_pages;
    for (page_id_t pid = h->first_page; pid < np; pid++) {
        void *page = bufpool_fetch(h->bp, pid);
        if (!page) {
            return -1;
        }
        uint16_t n = slotpage_num_slots(page);
        for (uint16_t s = 0; s < n; s++) {
            const void *rec;
            uint16_t len;
            if (slotpage_get(page, s, &rec, &len) == 0) { /* 삭제된 슬롯은 건너뜀 */
                RID rid = {pid, s};
                int r = visit(rid, rec, len, ctx);
                if (r != 0) {
                    bufpool_unpin(h->bp, pid, 0);
                    return r; /* early stop */
                }
            }
        }
        bufpool_unpin(h->bp, pid, 0);
    }
    return 0;
}
