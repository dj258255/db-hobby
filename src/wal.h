#ifndef MINIDB_WAL_H
#define MINIDB_WAL_H

#include "pager.h"

/*
 * WAL (Write-Ahead Log) — 내구성과 원자성을 주는 장치. 로그가 "진실의 원천"이다.
 *
 * 규칙: 데이터 파일을 고치기 전에 로그에 먼저 적고 fsync 한다(write-ahead).
 * 트랜잭션 동안 바뀐 페이지는 버퍼 풀에 모아두고, 커밋할 때(NO-FORCE):
 *   1) 모은 페이지들(after-image)을 로그에 쓴다
 *   2) 커밋 마커를 쓰고 fsync  <- 여기가 유일한 내구성 지점
 *   3) 데이터 파일에 반영은 하되 fsync하지 않는다 — 내구성은 로그가 책임진다
 *   4) 로그는 자르지 않는다. 커밋 이력이 쌓인다(진실의 원천).
 *      너무 커지면 체크포인트(데이터 fsync 후 로그 truncate)로 자른다.
 *
 * 크래시 복구(wal_open 시): 로그를 앞에서부터 읽어
 *   - 커밋 마커가 붙은 구간들 -> after-image를 순서대로 재적용(redo). 내구성.
 *   - 꼬리의 마커 없는 구간(loser) -> before-image로 되돌린다(undo) + 새 페이지는 잘라낸다. 원자성.
 *   복구 끝 = 데이터 fsync + 로그 truncate (여는 것 자체가 체크포인트다).
 *
 * STEAL (트랙 E): 트랜잭션의 dirty 페이지가 버퍼 풀보다 많아지면, 커밋 전이라도
 * 디스크로 내보낸다(steal). 그러면 디스크에 미커밋 변경이 생기므로 되돌릴 수단이 필요하다 —
 * steal 직전에 그 페이지의 before-image(=아직 내가 안 건드린 디스크의 커밋본)를 로그에
 * 먼저 남긴다(REC_BEGIN으로 트랜잭션 시작 페이지 수도 함께). first-write-wins: 페이지당 undo는
 * 최초 steal 때 한 번만.
 *
 * 학습용: 단일 트랜잭션, 페이지 전체 물리 로깅(그래서 redo가 idempotent — pageLSN 불필요).
 * 진짜 ARIES의 physiological 로깅·CLR·동시성은 없음.
 */

#define WAL_MAX_STAGED 64

typedef struct {
    Pager data;   /* 데이터 파일 */
    int log_fd;   /* 로그 파일 */
    void *staged; /* 이번 트랜잭션에서 바뀐 페이지들(메모리) */
    int num_staged;

    /* --- STEAL/UNDO (트랙 E) --- */
    int txn_active;      /* wal_begin 이후 커밋/롤백 전까지 1 */
    int stole;           /* 이 트랜잭션에서 steal이 한 번이라도 일어났나 */
    uint64_t base_pages; /* 트랜잭션 시작 시 페이지 수(undo 시 여기로 truncate) */
    uint64_t *spilled;   /* 이미 undo 로그를 남긴 page_id들 (first-write-wins) */
    int num_spilled;
    int cap_spilled;

    /* --- LSN (트랙 E: no-force로 가는 인프라) ---
     * 모든 로그 레코드는 단조 증가 LSN을 단다. flushed_lsn은 fsync로 내구화된
     * 마지막 LSN — "페이지를 디스크에 쓰기 전에 그 pageLSN까지 로그를 먼저"라는
     * WAL 규칙을 LSN 비교로 판정하기 위한 기반이다. (지금 로그는 커밋 시
     * truncate되는 임시 파일이라 LSN은 로그 수명 안에서만 유효.) */
    uint64_t next_lsn;
    uint64_t flushed_lsn;

    /* no-force: 로그에 여러 트랜잭션 이력이 쌓이므로, 롤백(undo)은 로그 처음이
     * 아니라 "내 트랜잭션이 시작된 오프셋"부터 훑어야 한다. wal_begin이 기록. */
    int64_t txn_log_start;
} Wal;

/* 데이터/로그 파일을 연다. 열 때 크래시 복구를 수행한다. 0 성공, -1 실패. */
int wal_open(Wal *w, const char *data_path, const char *log_path);

/* 닫는다(깨끗한 종료 — 로그는 비워진 상태). fd만 닫는다. */
void wal_close(Wal *w);

/* 페이지를 읽는다(데이터 파일에서). 0 성공, -1 실패. */
int wal_read(Wal *w, page_id_t page_id, void *buf);

/* 트랜잭션을 시작한다(stage 비움). */
void wal_begin(Wal *w);

/* 페이지 변경을 stage에 기록한다(아직 데이터 파일엔 안 씀). 0 성공, -1 가득참. */
int wal_stage(Wal *w, page_id_t page_id, const void *buf);

/* STEAL: 버퍼 풀이 커밋 전 dirty 페이지 P(내용 buf)를 축출해야 할 때 부른다.
 * WAL 규칙: 최초 steal이면 before-image를 로그에 먼저 남기고 fsync한 뒤, P를 데이터에 쓰고
 * fsync한다(FORCE). 이후 롤백/복구가 before-image로 원복. 0 성공, -1 실패. */
int wal_steal(Wal *w, page_id_t page_id, const void *buf);

/* 롤백: 이 트랜잭션에서 steal이 있었으면 로그의 before-image로 디스크를 원복하고,
 * 새로 할당한 페이지(>= base_pages)는 잘라낸다. 그리고 로그를 비운다. 0/-1.
 * steal이 없었으면 로그가 비어 있어 아무것도 안 한다(호출부가 truncate로 마무리). */
int wal_undo(Wal *w);

/* 커밋: 로그에 쓰고 fsync -> 데이터에 적용 -> 로그 비움. 0 성공, -1 실패. */
int wal_commit(Wal *w);

/* -- 테스트용 크래시 주입 -- */
/* 1이면 커밋이 로그 fsync 직후(데이터 적용 전)에 멈춘다 = "내구성 분기점 직후 크래시". */
extern int wal_test_crash_after_log;
/* 1이면 커밋이 커밋 마커 없이 페이지 로그만 쓰고 멈춘다 = "커밋 전 크래시". */
extern int wal_test_crash_before_commit;
/* 1이면 복구의 undo가 첫 before-image 하나만 적용하고 멈춘다(로그 안 자름)
 * = "undo 도중 재크래시". CLR 없이도 재복구가 수렴함을 증명하는 용도. */
extern int wal_test_crash_in_undo;

#endif /* MINIDB_WAL_H */
