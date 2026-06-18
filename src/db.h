#ifndef MINIDB_DB_H
#define MINIDB_DB_H

#include <stdio.h>
#include "pager.h"
#include "bufpool.h"
#include "heap.h"
#include "btree.h"
#include "lsm.h"
#include "wal.h"
#include "sql.h"
#include "lock.h"
#include "mvcc.h"

/*
 * Database — 모든 계층을 하나로 묶어 SQL을 실행한다.
 *
 *   db_exec(db, "INSERT INTO users VALUES (1, 'kim')")
 *      -> 파서: SQL -> AST
 *      -> 실행기: AST를 보고 저장 계층을 부린다
 *          CREATE  : 스키마(카탈로그) 기록 + (첫 컬럼이 INT면) B+Tree 인덱스 생성
 *          INSERT  : 값을 바이트로 인코딩 -> heap_insert -> 인덱스에 (PK -> RID) 등록
 *          SELECT  : WHERE가 PK면 인덱스로 O(log n) 조회, 아니면 풀 스캔
 *                    JOIN이면 중첩 루프 조인
 *
 * 저장 구조(PostgreSQL의 relfilenode를 본뜸): 테이블마다 파일을 따로 둔다.
 *   <path>            카탈로그 — 어떤 테이블이 있고 스키마가 뭔지 (pg_class 격)
 *   <path>.<tbl>.tbl  그 테이블의 행 (Heap)
 *   <path>.<tbl>.idx  그 테이블의 PK 인덱스 (B+Tree)
 *   <path>.<tbl>.wal  그 테이블 데이터의 쓰기 선행 로그 (커밋 원자성·크래시 복구)
 *
 * 학습용 단순화: 첫 컬럼(INT)을 유일 PK로 보고 인덱싱한다. 단일 INNER JOIN까지.
 */

#define DB_MAX_TABLES 16
#define DB_MAX_SEC_IDX 4 /* 테이블당 보조 인덱스 최대 개수 */
#define DB_MAX_SESSIONS 8 /* 동시에 열 수 있는 트랜잭션 핸들(세션) 수 */

typedef struct Database Database; /* Table이 소유 DB를 역참조(가시성 판정용) */

/* 한 세션 = 하나의 (열려 있을 수 있는) 트랜잭션 핸들. 단일 스레드지만 `SESSION n`으로
 * 세션을 갈아타며 여러 트랜잭션을 인터리브한다 — reader가 writer를 안 막는 걸
 * 진짜 두 트랜잭션으로 시연하기 위한 장치. */
typedef struct {
    int in_txn; /* BEGIN 중인가 */
    int txn;    /* 이 세션의 트랜잭션 id (in_txn일 때) */
    /* 트랜잭션 시작 스냅샷(스냅샷 격리): snap_next 이후에 발급됐거나 시작 시점에
     * 진행 중이던 트랜잭션은, 나중에 커밋해도 이 트랜잭션에겐 안 보인다. */
    int snap_next;
    int snap_inprog[DB_MAX_SESSIONS];
    int n_snap_inprog;
} DbSession;

/* 보조 인덱스(CREATE INDEX) — PK가 아닌 INT 컬럼에 거는 비유니크 B+Tree.
 * 자기 파일 <db>.<tbl>.<name>.idx(+.wal)를 쓰고, 키=컬럼값, 값=RID. */
typedef struct {
    char name[SQL_NAME_LEN]; /* 인덱스 이름 */
    int col;                 /* 인덱싱하는 컬럼 위치 */
    BTree tree;
    uint64_t txn_pages;      /* BEGIN 시점 인덱스 파일 페이지 수(롤백 복원용) */
} SecIndex;

typedef struct {
    CreateStmt schema;
    Wal wal; /* 데이터 파일(.tbl)을 WAL로 감싼다. 데이터 페이저는 wal.data 안에 있다. */
    BufferPool *bp;
    Heap heap;
    BTree index; /* 첫 컬럼(INT PK) 인덱스 — index_kind==0(B+Tree)일 때 사용 */
    int has_index;
    int index_kind;  /* 0=B+Tree(제자리, 읽기 최적), 1=LSM(append, 쓰기 최적). schema.index_kind 사본 */
    LSM *lindex;     /* index_kind==1일 때 PK 인덱스(멀티값 LSM). heap에서 파생·재구축된다 */

    SecIndex sec[DB_MAX_SEC_IDX]; /* 보조 인덱스들 */
    int num_sec;

    uint64_t txn_data_pages;  /* 쓰기 시작 시점 데이터 파일 페이지 수(롤백 복원용) */
    uint64_t txn_index_pages; /* 쓰기 시작 시점 인덱스 파일 페이지 수 */

    /* 옵티마이저 통계 (ANALYZE가 채운다. PostgreSQL의 pg_statistic 축소판) */
    int      stat_valid;  /* ANALYZE를 돌린 적 있나 */
    int64_t  stat_rows;   /* 보이는 행 수 */
    int64_t  stat_pages;  /* 힙 페이지 수(순차 스캔 비용 단위) */
    int64_t  stat_pk_min; /* PK(첫 컬럼) 최소/최대 — 범위 선택도 추정용 */
    int64_t  stat_pk_max;

    int writer_txn; /* 이 테이블에 미커밋 변경을 가진 트랜잭션(0=없음). 테이블 X락이
                     * "테이블당 writer 하나"를 보장 — 그래서 테이블별 WAL은 여전히
                     * 단일 트랜잭션만 보고, 14·15편의 복구가 무수정으로 성립한다. */

    Database *owner; /* 이 테이블이 속한 DB — 읽기 경로의 MVCC 가시성 판정용 */
} Table;

struct Database {
    char path[512]; /* 카탈로그 파일 경로 (테이블 파일 경로 유도용) */
    Table tables[DB_MAX_TABLES];
    int num_tables;

    int used_index; /* 직전 SELECT가 인덱스를 썼나 (시연·테스트용) */

    DbSession sessions[DB_MAX_SESSIONS]; /* 다중 트랜잭션 핸들 */
    int cur_session;                     /* 현재 문장이 속한 세션 (SESSION n으로 전환) */

    LockManager lm; /* 쓰기 전용 2PL 테이블 X락 — write-write 충돌(first-updater-wins).
                     * 읽기는 락을 안 잡는다: reader의 격리는 MVCC 가시성이 맡는다. */
    int cur_txn;    /* 현재 문장의 트랜잭션 id (락 소유자·행 xmin·가시성 기준). 0이면 없음 */
    int next_txn;   /* 다음 트랜잭션 id 발급기 */
    int committed_below; /* 이 세션 시작 시점의 txn 번호 — 그 미만 id는 전부 커밋된 것으로 본다
                          * (no-steal+WAL이라 디스크엔 커밋분만 있음). 재오픈 시 옛 행 가시성. */
    TxnLog txnlog;  /* MVCC: 트랜잭션 상태(진행/커밋/아보트) — 행 가시성 판정용 */
};

/* 파일을 열어 DB를 준비한다. 0 성공, -1 실패. */
int db_open(Database *db, const char *path);

/* flush하고 닫는다. */
void db_close(Database *db);

/* 병렬 실행(36~39편)의 워커 수를 바꾼다(기본 4). 벤치가 워커 수별 speedup을 재거나
 * 테스트가 워커 수 무관 결과 동일성을 검증할 때 쓴다. 1이면 사실상 직렬 기준선. */
void db_set_parallel_workers(int n);
int  db_get_parallel_workers(void);

/* SQL 한 문장을 실행한다. 결과/메시지는 out으로. 0 성공, -1 오류. */
int db_exec(Database *db, const char *sql, FILE *out);

/* 힙 레코드의 MVCC 헤더(xmin/xmax) 접근. 가시성 판정·테스트용. */
int32_t db_rec_xmin(const void *rec);
int32_t db_rec_xmax(const void *rec);

#endif /* MINIDB_DB_H */
