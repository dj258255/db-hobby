#ifndef MINIDB_SQL_H
#define MINIDB_SQL_H

#include <stddef.h>

/*
 * SQL 프런트엔드 — 텍스트를 AST로 바꾼다.
 *
 *   "SELECT * FROM users WHERE id = 1"
 *        |  토크나이저(lexer): 토큰으로 쪼갬
 *        v  SELECT  *  FROM  users  WHERE  id  =  1
 *        |  파서(recursive descent): 구조로 조립
 *        v  Select{ table: "users", where: id = 1 }
 *
 * 지원 문법(학습용 최소 부분집합):
 *   CREATE TABLE <name> (<col> INT|TEXT, ...)
 *   INSERT INTO <name> VALUES (<int|'text'>, ...)
 *   SELECT * FROM <name> [JOIN <name> ON <colref> = <colref>]
 *                        [WHERE <cond> [AND <cond>] [OR ...]]
 *                        [ORDER BY <col> [ASC|DESC]] [LIMIT <n>]
 *   <cond>   = <colref> <op> <val>,  <op> = =, !=, <, >, <=, >=
 *   <colref> = [<table>.]<col>
 */

#define SQL_MAX_COLS 32
#define SQL_NAME_LEN 64
#define SQL_TEXT_LEN 256

typedef enum { COL_INT, COL_TEXT } ColType;

/* WHERE 비교 연산자 */
typedef enum { CMP_EQ, CMP_NE, CMP_LT, CMP_GT, CMP_LE, CMP_GE } CmpOp;

typedef enum { VAL_INT, VAL_TEXT } ValueType;
typedef struct {
    ValueType type;
    long int_val;                /* VAL_INT */
    char text_val[SQL_TEXT_LEN]; /* VAL_TEXT */
} Value;

typedef struct {
    char name[SQL_NAME_LEN];
    ColType type;
} ColumnDef;

/* WHERE 한 조건: [<tbl>.]<col> <op> <val>. tbl[0]=='\0' 이면 한정 없음. */
typedef struct {
    char tbl[SQL_NAME_LEN];
    char col[SQL_NAME_LEN];
    CmpOp op;
    Value val;
} Condition;

/* AND로 묶인 조건 묶음(DNF 한 항). 전부 참이어야 이 묶음이 참. */
#define SQL_MAX_CONDS 8
typedef struct {
    int count;
    Condition conds[SQL_MAX_CONDS];
} AndGroup;

/* WHERE 절 = DNF. AND 묶음들을 OR로 잇는다(어느 한 묶음이라도 참이면 매칭).
 *   a AND b OR c  ->  groups = [ [a,b], [c] ]
 * count == 0 이면 WHERE 없음(항상 참). AND가 OR보다 강하게 묶인다. */
#define SQL_MAX_GROUPS 8
typedef struct {
    int count;
    AndGroup groups[SQL_MAX_GROUPS];
} Where;

typedef enum {
    STMT_CREATE,
    STMT_INSERT,
    STMT_SELECT,
    STMT_DELETE,
    STMT_UPDATE,
    STMT_BEGIN,    /* BEGIN — 트랜잭션 시작 */
    STMT_COMMIT,   /* COMMIT — 확정 */
    STMT_ROLLBACK, /* ROLLBACK — 되돌리기 */
} StmtType;

typedef struct {
    char table[SQL_NAME_LEN];
    ColumnDef columns[SQL_MAX_COLS];
    int num_columns;
} CreateStmt;

typedef struct {
    char table[SQL_NAME_LEN];
    Value values[SQL_MAX_COLS];
    int num_values;
} InsertStmt;

/* INNER JOIN ... ON <l> = <r>. has_join==0 이면 단일 테이블. */
typedef struct {
    int has_join;
    char table[SQL_NAME_LEN];                       /* JOIN 대상(오른쪽) 테이블 */
    char l_tbl[SQL_NAME_LEN], l_col[SQL_NAME_LEN];  /* ON 왼쪽 컬럼 참조 */
    char r_tbl[SQL_NAME_LEN], r_col[SQL_NAME_LEN];  /* ON 오른쪽 컬럼 참조 */
} JoinSpec;

typedef struct {
    char table[SQL_NAME_LEN];
    JoinSpec join;
    Where where;
    char order_col[SQL_NAME_LEN]; /* "" 이면 ORDER BY 없음 */
    int order_desc;               /* 1이면 DESC, 0이면 ASC */
    long limit;                   /* -1이면 LIMIT 없음 */
} SelectStmt;

typedef struct {
    char table[SQL_NAME_LEN];
    Where where;
} DeleteStmt;

typedef struct {
    char table[SQL_NAME_LEN];
    char set_col[SQL_NAME_LEN];
    Value set_val;
    Where where;
} UpdateStmt;

typedef struct {
    StmtType type;
    union {
        CreateStmt create;
        InsertStmt insert;
        SelectStmt select;
        DeleteStmt del;
        UpdateStmt upd;
    };
} Statement;

/* SQL 한 문장을 파싱한다. 0 성공, -1 실패(errbuf에 오류 메시지). */
int sql_parse(const char *sql, Statement *out, char *errbuf, size_t errlen);

#endif /* MINIDB_SQL_H */
