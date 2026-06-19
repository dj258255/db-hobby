#include "bufpool.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    page_id_t page_id;     /* 이 프레임이 담은 페이지 */
    int valid;             /* 사용 중인 프레임인가 */
    int dirty;             /* 메모리에서 수정됐나 */
    int pin_count;         /* 지금 쓰는 중인 곳 수 (>0이면 쫓아낼 수 없음) */
    int io_pending;        /* 이 프레임을 latch 밖에서 로딩 중(read-in-progress). 41편 */
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
     * 41편: 읽기 I/O(pager_read)는 이제 latch 밖에서 한다 — miss 시 victim 프레임을
     * io_pending으로 예약(pin해서 축출 방지)하고 latch를 놓은 뒤 읽는다. 같은 페이지를
     * 동시에 miss한 스레드는 io_cond에서 기다렸다 깨어나 hit로 잡는다(중복 로드 없음).
     * 그래서 콜드 스캔에서 여러 워커의 디스크 읽기가 병렬로 돈다. (dirty victim 쓰기는
     * 아직 latch 안 — 스테이징 경로라 스캔에선 드물다.) */
    pthread_mutex_t latch;
    pthread_cond_t  io_cond; /* io_pending 프레임이 로딩을 마치면 broadcast */
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
    pthread_cond_init(&bp->io_cond, NULL);
    bp->pager = pager;
    bp->num_frames = num_frames;
    return bp;
}

/* 벤치 A/B 스위치: 1이면 41편 이전처럼 읽기 I/O를 latch 안에서 한다(직렬). 기본 0.
 * 프로세스 전역(단순화) — 벤치가 같은 프로세스에서 before/after를 재려고 토글한다. */
static int g_io_in_latch = 0;
void bufpool_set_io_in_latch(int on) { g_io_in_latch = on; }

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

/* latch 보유 상태로 진입/반환한다(hit면 pin, miss면 load). miss의 pager_read는
 * 41편부터 latch를 놓고 수행한다 — 그동안 프레임은 io_pending+pin으로 보호된다. */
static Frame *fetch_locked(BufferPool *bp, page_id_t page_id) {
    for (;;) {
        Frame *f = find_frame(bp, page_id);
        if (f) {
            if (f->io_pending) {
                /* 다른 스레드가 바로 이 페이지를 로딩 중 — 끝날 때까지 기다렸다 재시도.
                 * cond_wait이 latch를 원자적으로 놓고 잤다 다시 잡는다. */
                pthread_cond_wait(&bp->io_cond, &bp->latch);
                continue;
            }
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
        if (g_io_in_latch) {
            /* 41편 이전 동작(벤치 A/B용): 읽기 I/O를 latch 안에서 — 직렬화된다. */
            if (pager_read(bp->pager, page_id, f->data) != 0) {
                f->valid = 0;
                return NULL;
            }
            f->page_id = page_id;
            f->valid = 1;
            f->dirty = 0;
            f->pin_count = 1;
            f->last_used = ++bp->clock;
            return f;
        }
        /* 이 프레임을 page_id로 '예약': io_pending으로 표시하고 pin해서 로딩 중
         * 축출되지 않게 한다. 이제 latch를 놓아도 다른 스레드는 이 프레임을 찾으면
         * io_pending을 보고 위에서 기다린다(중복 로드 방지). */
        f->page_id = page_id;
        f->valid = 1;
        f->dirty = 0;
        f->io_pending = 1;
        f->pin_count = 1;

        pthread_mutex_unlock(&bp->latch);
        int rc = pager_read(bp->pager, page_id, f->data); /* ← latch 밖: 병렬 I/O */
        pthread_mutex_lock(&bp->latch);

        f->io_pending = 0;
        pthread_cond_broadcast(&bp->io_cond); /* 기다리던 스레드 깨움 */
        if (rc != 0) {
            f->pin_count = 0;
            f->valid = 0; /* 로드 실패 — 프레임 반납 */
            return NULL;
        }
        f->last_used = ++bp->clock;
        return f;
    }
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
    pthread_cond_destroy(&bp->io_cond);
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
