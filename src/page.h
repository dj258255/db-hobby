#ifndef MINIDB_PAGE_H
#define MINIDB_PAGE_H

#include <stdint.h>
#include "pager.h" /* PAGE_SIZE */

/*
 * 슬롯 페이지 — 고정 크기 페이지(4KB) 안에 가변 길이 레코드를 패킹한다.
 * PostgreSQL heap 페이지, InnoDB 페이지와 같은 구조다.
 *
 *   +-----------------------------------------------+
 *   | Header (num_slots, free_end)                    |
 *   | Slot[0] Slot[1] ... ->  (위에서 아래로 자람)      |
 *   |            <- 빈 공간 ->                            |
 *   |              ... Record[1] Record[0]            |  (끝에서 위로 자람)
 *   +-----------------------------------------------+
 *
 * 슬롯은 (offset, length)로 레코드의 페이지 내 위치를 가리킨다. 슬롯 번호는
 * 레코드가 옮겨져도 안 바뀌는 안정적인 주소다 -> 상위 계층의 tuple pointer 기반.
 */

/* 빈 슬롯 페이지로 초기화한다. */
void slotpage_init(void *page);

/* 레코드를 삽입하고 슬롯 번호를 반환한다. 공간이 부족하면 -1. */
int slotpage_insert(void *page, const void *rec, uint16_t len);

/* 슬롯의 레코드 포인터와 길이를 돌려준다. 0 성공, -1이면 없는/삭제된 슬롯. */
int slotpage_get(const void *page, uint16_t slot, const void **rec_out, uint16_t *len_out);

/* 슬롯을 삭제(빈 칸으로 표시)한다. 0 성공, -1 잘못된 슬롯. */
int slotpage_delete(void *page, uint16_t slot);

/* 현재 슬롯 개수(삭제된 것 포함). */
uint16_t slotpage_num_slots(const void *page);

/* 남은 빈 공간(바이트). */
uint16_t slotpage_free_space(const void *page);

#endif /* MINIDB_PAGE_H */
