#ifndef MINIDB_DB_H
#define MINIDB_DB_H

#include <stdio.h>
#include "pager.h"
#include "bufpool.h"
#include "heap.h"
#include "btree.h"
#include "sql.h"

/*
 * Database — 모든 계층을 하나로 묶어 SQL을 실행한다.
 *
 *   db_exec(db, "INSERT INTO users VALUES (1, 'kim')")
 *      → 파서: SQL → AST
 *      → 실행기: AST를 보고 저장 계층을 부린다
 *          CREATE  : 스키마(카탈로그) 기록 + (첫 컬럼이 INT면) B+Tree 인덱스 생성
 *          INSERT  : 값을 바이트로 인코딩 → heap_insert → 인덱스에 (PK → RID) 등록
 *          SELECT  : WHERE가 PK면 인덱스로 O(log n) 조회, 아니면 풀 스캔
 *
 * 학습용 단순화: 한 데이터베이스 = 한 테이블. 첫 컬럼을 유일 PK로 보고 인덱싱한다.
 * 스키마는 메모리에만 둔다(행·인덱스는 디스크에 영속).
 */

typedef struct {
    Pager pager;
    BufferPool *bp;
    Heap heap;
    int has_table;
    CreateStmt schema;

    char path[512]; /* 데이터 파일 경로 (인덱스 파일 경로 유도용) */
    BTree index;    /* 첫 컬럼(INT PK) 인덱스 */
    int has_index;

    int used_index; /* 직전 SELECT가 인덱스를 썼나 (시연·테스트용) */
} Database;

/* 파일을 열어 DB를 준비한다. 0 성공, -1 실패. */
int db_open(Database *db, const char *path);

/* flush하고 닫는다. */
void db_close(Database *db);

/* SQL 한 문장을 실행한다. 결과/메시지는 out으로. 0 성공, -1 오류. */
int db_exec(Database *db, const char *sql, FILE *out);

#endif /* MINIDB_DB_H */
