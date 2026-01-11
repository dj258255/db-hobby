#ifndef MINIDB_WAL_H
#define MINIDB_WAL_H

#include "pager.h"

/*
 * WAL (Write-Ahead Log) — 내구성과 원자성을 주는 장치.
 *
 * 규칙: 데이터 파일을 고치기 전에 로그에 먼저 적고 fsync 한다(write-ahead).
 * 트랜잭션 동안 바뀐 페이지는 메모리에 모아두고(stage), 커밋할 때:
 *   1) 모은 페이지들을 로그에 쓴다
 *   2) 커밋 마커를 쓰고 fsync  ← 여기를 지나면 "내구"하다
 *   3) 데이터 파일에 실제로 적용한다
 *   4) 로그를 비운다(체크포인트)
 *
 * 크래시 복구(wal_open 시): 로그를 읽어
 *   - 커밋 마커가 있으면  → 데이터에 재적용(redo). 내구성.
 *   - 커밋 마커가 없으면  → 버린다. 원자성(롤백).
 *
 * 학습용: 단일 트랜잭션, 물리적 페이지 로깅(force-at-commit). 동시성·부분 로깅은 없음.
 */

#define WAL_MAX_STAGED 64

typedef struct {
    Pager data;   /* 데이터 파일 */
    int log_fd;   /* 로그 파일 */
    void *staged; /* 이번 트랜잭션에서 바뀐 페이지들(메모리) */
    int num_staged;
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

/* 커밋: 로그에 쓰고 fsync → 데이터에 적용 → 로그 비움. 0 성공, -1 실패. */
int wal_commit(Wal *w);

/* ── 테스트용 크래시 주입 ── */
/* 1이면 커밋이 로그 fsync 직후(데이터 적용 전)에 멈춘다 = "내구성 분기점 직후 크래시". */
extern int wal_test_crash_after_log;
/* 1이면 커밋이 커밋 마커 없이 페이지 로그만 쓰고 멈춘다 = "커밋 전 크래시". */
extern int wal_test_crash_before_commit;

#endif /* MINIDB_WAL_H */
