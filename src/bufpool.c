#include "bufpool.h"

#include <pthread.h>
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
    /* 트랙 D: 프레임 테이블(메타데이터)을 보호하는 latch. 반환된 페이지 데이터 자체는
     * pin이 지킨다(pin>0이면 축출 안 되므로 mutex 밖에서 써도 안전) — 이게 pin 프로토콜.
     * I/O(pager_read/write)도 이 latch 안에서 한다: 직렬화되지만 단순하고 정확하다.
     * (진짜 DB는 "read-in-progress" 상태로 I/O를 latch 밖으로 뺀다.) */
    pthread_mutex_t latch;
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
    pthread_mutex_init(&bp->latch, NULL);
    bp->pager = pager;
    bp->num_frames = num_frames;
    return bp;
}

/* page_id를 담은 프레임을 찾는다. 없으면 NULL. (latch 보유 가정. 학습용이라 선형 탐색 —
 * 진짜 DB는 page_id -> frame 해시 테이블을 둔다.) */
static Frame *find_frame(BufferPool *bp, page_id_t page_id) {
    for (size_t i = 0; i < bp->num_frames; i++) {
        if (bp->frames[i].valid && bp->frames[i].page_id == page_id) {
            return &bp->frames[i];
        }
    }
    return NULL;
}

/* 새 페이지를 올릴 프레임을 고른다: 빈 프레임 우선, 없으면 LRU victim. (latch 보유 가정)
 * victim이 dirty면 디스크로 flush한다. 모두 pin되어 있으면 NULL. */
static Frame *pick_frame(BufferPool *bp) {
    /* 1) 빈 프레임 */
    for (size_t i = 0; i < bp->num_frames; i++) {
        if (!bp->frames[i].valid) {
            return &bp->frames[i];
        }
    }
    /* 2) LRU victim: pin 안 된 것 중 last_used가 가장 작은 것. */
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

/* latch 보유 상태에서 page_id 프레임을 확보한다(hit면 pin, miss면 load). */
static Frame *fetch_locked(BufferPool *bp, page_id_t page_id) {
    Frame *f = find_frame(bp, page_id);
    if (f) {
        bp->hits++;
        f->pin_count++;
        f->last_used = ++bp->clock;
        return f;
    }
    bp->misses++;
    f = pick_frame(bp);
    if (!f) {
        return NULL;
    }
    if (pager_read(bp->pager, page_id, f->data) != 0) {
        return NULL; /* f는 이미 valid=0(빈 프레임) 상태 */
    }
    f->page_id = page_id;
    f->valid = 1;
    f->dirty = 0;
    f->pin_count = 1;
    f->last_used = ++bp->clock;
    return f;
}

void *bufpool_fetch(BufferPool *bp, page_id_t page_id) {
    pthread_mutex_lock(&bp->latch);
    Frame *f = fetch_locked(bp, page_id);
    void *ret = f ? f->data : NULL;
    pthread_mutex_unlock(&bp->latch);
    return ret;
}

void *bufpool_new_page(BufferPool *bp, page_id_t *page_id_out) {
    pthread_mutex_lock(&bp->latch);
    void *ret = NULL;
    page_id_t id = pager_allocate(bp->pager); /* latch가 num_pages 경쟁도 막는다 */
    if (id != PAGE_ID_INVALID) {
        Frame *f = fetch_locked(bp, id);
        if (f) {
            ret = f->data;
            if (page_id_out) *page_id_out = id;
        }
    }
    pthread_mutex_unlock(&bp->latch);
    return ret;
}

void bufpool_unpin(BufferPool *bp, page_id_t page_id, int is_dirty) {
    pthread_mutex_lock(&bp->latch);
    Frame *f = find_frame(bp, page_id);
    if (f) {
        if (is_dirty) {
            f->dirty = 1;
        }
        if (f->pin_count > 0) {
            f->pin_count--;
        }
    }
    pthread_mutex_unlock(&bp->latch);
}

int bufpool_flush_all(BufferPool *bp) {
    pthread_mutex_lock(&bp->latch);
    int rc = 0;
    for (size_t i = 0; i < bp->num_frames; i++) {
        Frame *f = &bp->frames[i];
        if (f->valid && f->dirty) {
            if (pager_write(bp->pager, f->page_id, f->data) != 0) { rc = -1; break; }
            f->dirty = 0;
        }
    }
    pthread_mutex_unlock(&bp->latch);
    return rc;
}

int bufpool_flush_cb(BufferPool *bp, bufpool_sink_fn sink, void *ctx) {
    pthread_mutex_lock(&bp->latch);
    int n = 0;
    for (size_t i = 0; i < bp->num_frames; i++) {
        Frame *f = &bp->frames[i];
        if (f->valid && f->dirty) {
            if (sink(f->page_id, f->data, ctx) != 0) { n = -1; break; }
            f->dirty = 0; /* WAL이 받아갔으니 풀에선 clean. 적용은 WAL이 한다. */
            n++;
        }
    }
    pthread_mutex_unlock(&bp->latch);
    return n;
}

void bufpool_set_no_steal(BufferPool *bp, int on) {
    pthread_mutex_lock(&bp->latch);
    bp->no_steal = on;
    pthread_mutex_unlock(&bp->latch);
}

void bufpool_set_steal_handler(BufferPool *bp, bufpool_sink_fn fn, void *ctx) {
    pthread_mutex_lock(&bp->latch);
    bp->steal_fn = fn;
    bp->steal_ctx = ctx;
    pthread_mutex_unlock(&bp->latch);
}

void bufpool_discard_dirty(BufferPool *bp) {
    pthread_mutex_lock(&bp->latch);
    for (size_t i = 0; i < bp->num_frames; i++) {
        Frame *f = &bp->frames[i];
        if (f->valid && f->dirty) {
            f->valid = 0;
            f->dirty = 0;
        }
    }
    pthread_mutex_unlock(&bp->latch);
}

void bufpool_invalidate_all(BufferPool *bp) {
    pthread_mutex_lock(&bp->latch);
    for (size_t i = 0; i < bp->num_frames; i++) {
        bp->frames[i].valid = 0;
        bp->frames[i].dirty = 0;
        bp->frames[i].pin_count = 0;
    }
    pthread_mutex_unlock(&bp->latch);
}

void bufpool_invalidate_from(BufferPool *bp, page_id_t first) {
    pthread_mutex_lock(&bp->latch);
    for (size_t i = 0; i < bp->num_frames; i++) {
        Frame *f = &bp->frames[i];
        if (f->valid && f->page_id >= first) {
            f->valid = 0;
            f->dirty = 0;
            f->pin_count = 0;
        }
    }
    pthread_mutex_unlock(&bp->latch);
}

void bufpool_destroy(BufferPool *bp) {
    if (!bp) {
        return;
    }
    bufpool_flush_all(bp);
    pthread_mutex_destroy(&bp->latch);
    free(bp->frames);
    free(bp);
}

size_t bufpool_hits(const BufferPool *bp) {
    return bp->hits; /* 통계 — 경합 없는 관찰용이라 latch 생략 */
}

size_t bufpool_misses(const BufferPool *bp) {
    return bp->misses;
}
