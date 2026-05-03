#ifndef MINIDB_BUFPOOL_H
#define MINIDB_BUFPOOL_H

#include <stddef.h>
#include "pager.h"

/*
 * Buffer Pool — 페이지를 메모리에 캐시한다. (InnoDB buffer pool, PostgreSQL
 * shared buffers와 같은 역할.)
 *
 * 고정 개수의 "프레임"에 페이지를 담는다. 페이지를 요청하면:
 *   - 풀에 있으면(cache hit) 그대로 돌려준다.
 *   - 없으면(miss) 디스크에서 읽어 빈 프레임에 올린다. 빈 프레임이 없으면
 *     LRU victim을 골라 쫓아낸다(쫓겨나는 페이지가 dirty면 먼저 디스크로 flush).
 *
 * 세 가지 안전장치:
 *   - pin count: 쓰는 중인 페이지(pin>0)는 절대 쫓아내지 않는다.
 *   - dirty:     메모리에서 수정된 페이지는 쫓겨나기/flush 시 디스크에 반영된다.
 *   - LRU:       victim은 가장 오래 안 쓴 unpinned 프레임.
 */

typedef struct BufferPool BufferPool;

/* 페이저 위에 num_frames개 프레임을 가진 버퍼 풀을 만든다. 실패 시 NULL. */
BufferPool *bufpool_create(Pager *pager, size_t num_frames);

/* 모든 dirty 페이지를 flush하고 풀을 해제한다. */
void bufpool_destroy(BufferPool *bp);

/* 페이지를 가져온다(없으면 로드, 꽉 차면 victim 교체). 반환 포인터는 unpin 전까지
 * 유효하다. 모든 프레임이 pin되어 자리가 없으면 NULL. */
void *bufpool_fetch(BufferPool *bp, page_id_t page_id);

/* 새 페이지를 디스크에 할당하고 풀에 올려 돌려준다. page_id_out에 새 id. 실패 NULL. */
void *bufpool_new_page(BufferPool *bp, page_id_t *page_id_out);

/* 페이지 사용을 마쳤다고 표시한다(pin 감소). 수정했으면 is_dirty=1. */
void bufpool_unpin(BufferPool *bp, page_id_t page_id, int is_dirty);

/* 모든 dirty 페이지를 디스크에 쓴다. 성공 0, 실패 -1. */
int bufpool_flush_all(BufferPool *bp);

/* dirty 페이지마다 sink(page_id, data, ctx)를 부르고, sink가 0을 주면 그 프레임을
 * clean으로 표시한다(디스크엔 직접 안 쓴다 — WAL로 보내기 위함). 처리한 dirty 수를
 * 반환, sink 실패 시 -1. 페이지는 풀에 그대로 캐시된다. */
typedef int (*bufpool_sink_fn)(page_id_t page_id, const void *data, void *ctx);
int bufpool_flush_cb(BufferPool *bp, bufpool_sink_fn sink, void *ctx);

/* 트랜잭션용 no-steal 모드. 켜면 교체 시 dirty 프레임을 victim으로 안 고른다
 * (커밋 안 된 페이지가 디스크로 새는 걸 막는다) — 단, steal 핸들러가 등록돼 있으면
 * 그 핸들러를 통해(=undo 로그를 남기고) dirty 프레임을 축출할 수 있다(트랙 E). */
void bufpool_set_no_steal(BufferPool *bp, int on);

/* no-steal 중 dirty 프레임을 축출해야 할 때 부를 핸들러. 핸들러가 그 페이지를
 * 안전하게(=WAL로 undo 로깅 + 디스크 쓰기) 처리하면 0을 준다. NULL이면 옛 no-steal
 * (dirty를 축출하지 않고 자리가 없으면 실패). */
void bufpool_set_steal_handler(BufferPool *bp, bufpool_sink_fn fn, void *ctx);

/* dirty 프레임을 디스크에 쓰지 않고 전부 무효화한다(롤백용 — 다음 fetch가 디스크 원본을 읽음). */
void bufpool_discard_dirty(BufferPool *bp);

/* 모든 프레임(clean 포함)을 무효화한다. undo로 디스크를 되돌린 뒤, 풀에 남은 stale한
 * (steal로 clean 처리됐지만 미커밋 내용인) 프레임을 버려 다음 fetch가 디스크를 읽게 한다. */
void bufpool_invalidate_all(BufferPool *bp);

/* first 이후(포함) 페이지를 담은 프레임을 전부 무효화한다 — 파일 truncate 전에 불러,
 * 잘린 페이지의 dirty 프레임이 나중에 flush되며 파일을 되살리는 걸 막는다(VACUUM용). */
void bufpool_invalidate_from(BufferPool *bp, page_id_t first);

/* 통계 (시연·테스트용). */
size_t bufpool_hits(const BufferPool *bp);
size_t bufpool_misses(const BufferPool *bp);

#endif /* MINIDB_BUFPOOL_H */
