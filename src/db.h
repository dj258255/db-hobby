#ifndef MINIDB_DB_H
#define MINIDB_DB_H

#include <stdio.h>
#include "pager.h"
#include "bufpool.h"
#include "heap.h"
#include "sql.h"

/*
 * Database — 모든 계층을 하나로 묶어 SQL을 실행한다.
 *
 *   db_exec(db, "INSERT INTO users VALUES (1, 'kim')")
 *      → 파서: SQL → AST
 *      → 실행기: AST를 보고 저장 계층을 부린다
 *          CREATE  : 스키마(카탈로그)를 기록
 *          INSERT  : 값을 스키마대로 바이트로 인코딩 → heap_insert
 *          SELECT  : heap_scan으로 훑으며 디코딩 → WHERE 필터 → 출력
 *
 * 학습용 단순화: 한 데이터베이스 = 한 테이블. 스키마(카탈로그)는 메모리에만 둔다
 * (행은 디스크에 영속되지만, 스키마를 디스크에 저장하는 진짜 카탈로그는 다음 단계).
 */

typedef struct {
    Pager pager;
    BufferPool *bp;
    Heap heap;
    int has_table;      /* CREATE TABLE 됐나 */
    CreateStmt schema;  /* 그 한 테이블의 스키마 */
} Database;

/* 파일을 열어 DB를 준비한다. 0 성공, -1 실패. */
int db_open(Database *db, const char *path);

/* dirty 페이지를 flush하고 닫는다. */
void db_close(Database *db);

/* SQL 한 문장을 실행한다. 결과/메시지는 out으로 출력. 0 성공, -1 오류. */
int db_exec(Database *db, const char *sql, FILE *out);

#endif /* MINIDB_DB_H */
