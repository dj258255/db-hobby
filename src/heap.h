#ifndef MINIDB_HEAP_H
#define MINIDB_HEAP_H

#include <stdint.h>
#include "pager.h"
#include "bufpool.h"

/*
 * Heap File — 순서 없는 페이지들의 모음으로 한 테이블의 행을 저장한다.
 * (PostgreSQL의 heap, 가장 기본적인 테이블 저장 방식.)
 *
 * 행을 넣으면 마지막 페이지의 슬롯에 들어가고, 공간이 없으면 새 페이지를 할당한다.
 * 행의 주소는 RID = (page_id, slot) — PostgreSQL의 TID와 같다. 인덱스가 나중에
 * "키 -> RID"로 이걸 가리킨다.
 *
 * 학습용 단순화: 한 파일 = 한 테이블. 빈 공간을 빨리 찾는 free space map은 없고,
 * 그냥 마지막 페이지를 먼저 시도한 뒤 안 되면 새 페이지를 붙인다.
 */

typedef struct {
    page_id_t page_id;
    uint16_t slot;
} RID;

typedef struct {
    BufferPool *bp;
    Pager *pager;
    page_id_t first_page; /* 데이터 페이지 시작. 테이블마다 파일이 따로라 보통 0. */
} Heap;

void heap_init(Heap *h, BufferPool *bp, Pager *pager, page_id_t first_page);

/* 행을 삽입하고 RID를 돌려준다. 0 성공, -1 실패(빈 페이지에도 안 들어갈 만큼 큰 행 등). */
int heap_insert(Heap *h, const void *rec, uint16_t len, RID *rid_out);

/* RID의 행을 buf로 복사한다. 0 성공, -1 없는/삭제된 행. */
int heap_get(Heap *h, RID rid, void *buf, uint16_t *len_out);

/* RID의 행을 삭제(tombstone)한다. 0 성공, -1 실패.
 * ※ db 실행기의 DELETE는 이제 이걸 안 쓴다 — MVCC답게 xmax를 새긴다(heap_overwrite).
 *   물리 슬롯 회수는 VACUUM(예정)의 일. */
int heap_delete(Heap *h, RID rid);

/* RID의 행을 '같은 길이'의 새 내용으로 제자리 덮어쓴다(MVCC 헤더 xmax 갱신용).
 * 길이가 다르거나 삭제된 슬롯이면 -1. 0 성공. */
int heap_overwrite(Heap *h, RID rid, const void *rec, uint16_t len);

/* 풀 스캔: 모든 살아있는 행을 차례로 visit에 넘긴다. visit가 0이 아닌 값을 반환하면
 * 스캔을 멈추고 그 값을 돌려준다(early stop). rec 포인터는 콜백 동안만 유효. */
typedef int (*heap_visit_fn)(RID rid, const void *rec, uint16_t len, void *ctx);
int heap_scan(Heap *h, heap_visit_fn visit, void *ctx);

#endif /* MINIDB_HEAP_H */
