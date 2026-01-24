#ifndef MINIDB_PAGER_H
#define MINIDB_PAGER_H

#include <stdint.h>
#include <stddef.h>

/*
 * Pager — 디스크 관리자. 진짜 DB의 가장 밑바닥 계층.
 *
 * DB의 모든 것은 "고정 크기 페이지" 위에 쌓인다. (PostgreSQL 8KB, MySQL InnoDB
 * 16KB, SQLite 4KB.) 디스크는 페이지 단위로만 읽고 쓴다. Pager는 "page_id로
 * 페이지를 읽고 쓴다"만 책임진다 — 그 위에 슬롯 페이지, 힙, B-Tree가 얹힌다.
 *
 * page_id N은 파일에서 byte offset (N * PAGE_SIZE)에 저장된다.
 */

#define PAGE_SIZE 4096

typedef uint64_t page_id_t;
#define PAGE_ID_INVALID ((page_id_t)-1)

typedef struct {
    int fd;             /* 열린 파일 디스크립터 */
    uint64_t num_pages; /* 현재 파일에 들어있는 페이지 수 */
} Pager;

/* 파일을 연다(없으면 생성). 성공 0, 실패 -1. */
int pager_open(Pager *pager, const char *path);

/* Pager를 닫는다. */
void pager_close(Pager *pager);

/* 새 빈(0으로 채운) 페이지를 파일 끝에 할당하고 그 page_id를 반환한다.
 * 실패 시 PAGE_ID_INVALID. */
page_id_t pager_allocate(Pager *pager);

/* page_id 페이지를 buf(최소 PAGE_SIZE 바이트)로 읽는다. 성공 0, 실패 -1. */
int pager_read(Pager *pager, page_id_t page_id, void *buf);

/* page_id 페이지에 buf(PAGE_SIZE 바이트)를 쓴다. 성공 0, 실패 -1. */
int pager_write(Pager *pager, page_id_t page_id, const void *buf);

/* 파일을 num_pages 페이지 크기로 줄인다(롤백 시 트랜잭션이 할당한 페이지 제거). 0/-1. */
int pager_truncate(Pager *pager, uint64_t num_pages);

#endif /* MINIDB_PAGER_H */
