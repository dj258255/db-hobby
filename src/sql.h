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
 *   SELECT * FROM <name> [WHERE <col> = <int|'text'>]
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

/* WHERE 한 조건: <col> <op> <val> */
typedef struct {
    char col[SQL_NAME_LEN];
    CmpOp op;
    Value val;
} Condition;

/* WHERE 절: 여러 조건을 AND로 묶는다. count == 0 이면 WHERE 없음. */
#define SQL_MAX_CONDS 8
typedef struct {
    int count;
    Condition conds[SQL_MAX_CONDS];
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

typedef struct {
    char table[SQL_NAME_LEN];
    Where where;
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
