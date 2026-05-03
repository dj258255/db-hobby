#include "bufpool.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    page_id_t page_id;     /* 이 프레임이 담은 페이지 */
    int valid;             /* 사용 중인 프레임인가 */
    int dirty;             /* 메모리에서 수정됐나 */
    int pin_count;         /* 지금 쓰는 중인 곳 수 (>0이면 쫓아낼 수 없음) */
    uint64_t last_used;    /* LRU 스탬프 (클수록 최근) */
    uint8_t data[PAGE_SIZE];
} Frame;

struct BufferPool {
    Pager *pager;
    Frame *frames;
    size_t num_frames;
    uint64_t clock;        /* 단조 증가 LRU 스탬프 발급기 */
    size_t hits;
    size_t misses;
    int no_steal;          /* 트랜잭션 중: dirty 페이지를 교체 대상에서 제외 */
    bufpool_sink_fn steal_fn; /* no-steal 중 dirty 축출 시 부를 핸들러(WAL undo). NULL이면 옛 동작 */
    void *steal_ctx;
};

BufferPool *bufpool_create(Pager *pager, size_t num_frames) {
    if (num_frames == 0) {
        return NULL;
    }
    BufferPool *bp = calloc(1, sizeof(BufferPool));
    if (!bp) {
        return NULL;
    }
    bp->frames = calloc(num_frames, sizeof(Frame));
    if (!bp->frames) {
        free(bp);
        return NULL;
    }
    bp->pager = pager;
    bp->num_frames = num_frames;
    return bp;
}

/* page_id를 담은 프레임을 찾는다. 없으면 NULL. (학습용이라 선형 탐색 —
 * 진짜 DB는 page_id -> frame 해시 테이블을 둔다.) */
static Frame *find_frame(BufferPool *bp, page_id_t page_id) {
    for (size_t i = 0; i < bp->num_frames; i++) {
        if (bp->frames[i].valid && bp->frames[i].page_id == page_id) {
            return &bp->frames[i];
        }
    }
    return NULL;
}

/* 새 페이지를 올릴 프레임을 고른다: 빈 프레임 우선, 없으면 LRU victim.
 * victim이 dirty면 디스크로 flush한다. 모두 pin되어 있으면 NULL. */
static Frame *pick_frame(BufferPool *bp) {
    /* 1) 빈 프레임 */
    for (size_t i = 0; i < bp->num_frames; i++) {
        if (!bp->frames[i].valid) {
            return &bp->frames[i];
        }
    }
    /* 2) LRU victim: pin 안 된 것 중 last_used가 가장 작은 것.
     * no-steal이고 steal 핸들러가 없으면 dirty는 제외(옛 동작). 핸들러가 있으면
     * dirty도 후보 — 축출할 때 핸들러가 undo 로깅 후 디스크로 내보낸다(steal). */
    int can_steal_dirty = !bp->no_steal || bp->steal_fn != NULL;
    Frame *victim = NULL;
    for (size_t i = 0; i < bp->num_frames; i++) {
        Frame *f = &bp->frames[i];
        if (f->pin_count != 0) {
            continue;
        }
        if (f->dirty && !can_steal_dirty) {
            continue; /* 옛 no-steal: 커밋 안 된 페이지를 쫓아내지 않는다 */
        }
        if (!victim || f->last_used < victim->last_used) {
            victim = f;
        }
    }
    if (!victim) {
        return NULL; /* 전부 pin됨(또는 no-steal+핸들러없음+전부 dirty) — 자리 없음 */
    }
    if (victim->dirty) {
        if (bp->no_steal && bp->steal_fn) {
            /* STEAL: 핸들러가 WAL undo 로깅 + 디스크 쓰기까지 책임진다. */
            if (bp->steal_fn(victim->page_id, victim->data, bp->steal_ctx) != 0) {
                return NULL;
            }
        } else if (pager_write(bp->pager, victim->page_id, victim->data) != 0) {
            return NULL;
        }
        victim->dirty = 0;
    }
    victim->valid = 0; /* 재사용 위해 비움 */
    return victim;
}

void *bufpool_fetch(BufferPool *bp, page_id_t page_id) {
    Frame *f = find_frame(bp, page_id);
    if (f) {
        /* cache hit */
        bp->hits++;
        f->pin_count++;
        f->last_used = ++bp->clock;
        return f->data;
    }
    /* cache miss */
    bp->misses++;
    f = pick_frame(bp);
    if (!f) {
        return NULL;
    }
    if (pager_read(bp->pager, page_id, f->data) != 0) {
        return NULL;
    }
    f->page_id = page_id;
    f->valid = 1;
    f->dirty = 0;
    f->pin_count = 1;
    f->last_used = ++bp->clock;
    return f->data;
}

void *bufpool_new_page(BufferPool *bp, page_id_t *page_id_out) {
    page_id_t id = pager_allocate(bp->pager); /* 디스크에 0으로 찬 페이지 확보 */
    if (id == PAGE_ID_INVALID) {
        return NULL;
    }
    void *data = bufpool_fetch(bp, id); /* 그 페이지를 풀에 올린다 */
    if (!data) {
        return NULL;
    }
    if (page_id_out) {
        *page_id_out = id;
    }
    return data;
}

void bufpool_unpin(BufferPool *bp, page_id_t page_id, int is_dirty) {
    Frame *f = find_frame(bp, page_id);
    if (!f) {
        return;
    }
    if (is_dirty) {
        f->dirty = 1;
    }
    if (f->pin_count > 0) {
        f->pin_count--;
    }
}

int bufpool_flush_all(BufferPool *bp) {
    for (size_t i = 0; i < bp->num_frames; i++) {
        Frame *f = &bp->frames[i];
        if (f->valid && f->dirty) {
            if (pager_write(bp->pager, f->page_id, f->data) != 0) {
                return -1;
            }
            f->dirty = 0;
        }
    }
    return 0;
}

int bufpool_flush_cb(BufferPool *bp, bufpool_sink_fn sink, void *ctx) {
    int n = 0;
    for (size_t i = 0; i < bp->num_frames; i++) {
        Frame *f = &bp->frames[i];
        if (f->valid && f->dirty) {
            if (sink(f->page_id, f->data, ctx) != 0) {
                return -1;
            }
            f->dirty = 0; /* WAL이 받아갔으니 풀에선 clean. 적용은 WAL이 한다. */
            n++;
        }
    }
    return n;
}

void bufpool_set_no_steal(BufferPool *bp, int on) {
    bp->no_steal = on;
}

void bufpool_set_steal_handler(BufferPool *bp, bufpool_sink_fn fn, void *ctx) {
    bp->steal_fn = fn;
    bp->steal_ctx = ctx;
}

void bufpool_discard_dirty(BufferPool *bp) {
    for (size_t i = 0; i < bp->num_frames; i++) {
        Frame *f = &bp->frames[i];
        if (f->valid && f->dirty) {
            f->valid = 0; /* 디스크에 안 쓰고 무효화 -> 다음 fetch가 원본을 읽는다 */
            f->dirty = 0;
        }
    }
}

void bufpool_invalidate_all(BufferPool *bp) {
    for (size_t i = 0; i < bp->num_frames; i++) {
        bp->frames[i].valid = 0;
        bp->frames[i].dirty = 0;
        bp->frames[i].pin_count = 0;
    }
}

void bufpool_invalidate_from(BufferPool *bp, page_id_t first) {
    for (size_t i = 0; i < bp->num_frames; i++) {
        Frame *f = &bp->frames[i];
        if (f->valid && f->page_id >= first) {
            f->valid = 0;
            f->dirty = 0;
            f->pin_count = 0;
        }
    }
}

void bufpool_destroy(BufferPool *bp) {
    if (!bp) {
        return;
    }
    bufpool_flush_all(bp);
    free(bp->frames);
    free(bp);
}

size_t bufpool_hits(const BufferPool *bp) {
    return bp->hits;
}

size_t bufpool_misses(const BufferPool *bp) {
    return bp->misses;
}
