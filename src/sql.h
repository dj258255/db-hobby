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
 *   SELECT <* | item, ...> FROM <name> [JOIN <name> ON <colref> = <colref>]...
 *                        [WHERE <cond> [AND <cond>] [OR ...]]
 *                        [GROUP BY <col>] [ORDER BY <colref> [ASC|DESC]] [LIMIT <n>]
 *   <item>   = <col> | COUNT(*) | COUNT|SUM|MIN|MAX|AVG(<col>)
 *   <cond>   = <colref> <op> <val>,  <op> = =, !=, <, >, <=, >=
 *   <colref> = [<table>.]<col>
 */

#define SQL_MAX_COLS 32
#define SQL_NAME_LEN 64
#define SQL_TEXT_LEN 256

typedef enum { COL_INT, COL_TEXT } ColType;

/* WHERE 비교 연산자. IS_NULL/IS_NOT_NULL은 값 피연산자가 없다. */
typedef enum {
    CMP_EQ, CMP_NE, CMP_LT, CMP_GT, CMP_LE, CMP_GE,
    CMP_IS_NULL, CMP_IS_NOT_NULL
} CmpOp;

/* VAL_NULL은 저장 행엔 안 생기고, LEFT JOIN의 미매칭 오른쪽 컬럼 등 결과에만 등장한다. */
typedef enum { VAL_INT, VAL_TEXT, VAL_NULL } ValueType;
typedef struct {
    ValueType type;
    long int_val;                /* VAL_INT */
    char text_val[SQL_TEXT_LEN]; /* VAL_TEXT */
} Value;

typedef struct {
    char name[SQL_NAME_LEN];
    ColType type;
} ColumnDef;

typedef struct SelectStmt SelectStmt; /* 전방 선언: 조건이 서브쿼리를 품을 수 있다 */

/* WHERE 한 조건: [<tbl>.]<col> <op> <val>, 또는 [<tbl>.]<col> IN (SELECT ...). */
typedef struct {
    char tbl[SQL_NAME_LEN];
    char col[SQL_NAME_LEN];
    CmpOp op;
    Value val;
    /* col IN (SELECT ...) 서브쿼리. in_sub면 sub가 (malloc된) 안쪽 쿼리.
     * 실행 직전 prepare 단계가 sub를 한 번 돌려 in_set(값 집합)을 채운다. */
    int in_sub;
    int in_negate; /* NOT IN 이면 1 (멤버십 부정) */
    SelectStmt *sub;
    Value *in_set;
    int in_set_n;
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

/* [LEFT] JOIN <table> [<alias>] ON <l> = <r>. 체인의 한 마디. */
typedef struct {
    int is_left;                                    /* 1이면 LEFT (OUTER) JOIN, 0이면 INNER */
    char table[SQL_NAME_LEN];                       /* JOIN 대상 테이블 */
    char alias[SQL_NAME_LEN];                       /* 별칭 ("" 이면 테이블명을 그대로 씀) */
    char l_tbl[SQL_NAME_LEN], l_col[SQL_NAME_LEN];  /* ON 왼쪽 컬럼 참조 */
    char r_tbl[SQL_NAME_LEN], r_col[SQL_NAME_LEN];  /* ON 오른쪽 컬럼 참조 */
} JoinClause;

#define SQL_MAX_JOINS 4 /* FROM 외에 최대 4개까지 JOIN으로 잇는다 */

/* 집계 함수 종류. AGG_NONE이면 일반 컬럼(투영). */
typedef enum { AGG_NONE, AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX, AGG_AVG } AggFunc;

/* SELECT 목록의 한 항목: 일반 컬럼이거나 집계 FUNC(arg). */
typedef struct {
    AggFunc agg;            /* AGG_NONE이면 일반 컬럼 */
    int star;               /* COUNT(*) 처럼 인자가 * 이면 1 */
    char tbl[SQL_NAME_LEN]; /* 테이블 한정자 ("" 이면 없음) — 조인 집계에서 모호성 해소 */
    char col[SQL_NAME_LEN]; /* 컬럼 이름 (star면 무시) */
} SelectItem;

struct SelectStmt {
    char table[SQL_NAME_LEN]; /* FROM 테이블 */
    char alias[SQL_NAME_LEN]; /* FROM 테이블 별칭 ("" 이면 테이블명을 그대로 씀) */
    int num_joins;            /* 0이면 단일 테이블 */
    JoinClause joins[SQL_MAX_JOINS];
    /* 투영/집계: select_star면 SELECT * (items 무시). 아니면 items가 출력 목록. */
    int distinct; /* SELECT DISTINCT 이면 1 (출력 행 중복 제거) */
    int select_star;
    int num_items;
    SelectItem items[SQL_MAX_COLS];
    int has_aggregate;            /* items에 집계가 하나라도 있으면 1 */
    char group_tbl[SQL_NAME_LEN]; /* GROUP BY 컬럼의 테이블 한정자 ("" 이면 없음) */
    char group_col[SQL_NAME_LEN]; /* GROUP BY 컬럼 ("" 이면 없음) */
    /* HAVING: 집계 조건으로 그룹을 거른다. has_having이면 having_agg <op> having_val. */
    int has_having;
    SelectItem having_agg;
    CmpOp having_op;
    Value having_val;
    Where where;
    char order_tbl[SQL_NAME_LEN]; /* ORDER BY 컬럼의 테이블 한정자 ("" 이면 없음) */
    char order_col[SQL_NAME_LEN]; /* "" 이면 ORDER BY 없음(또는 order_pos 사용) */
    int order_pos;                /* ORDER BY <위치> (1-based, 출력 컬럼). 0이면 미사용 */
    int order_desc;               /* 1이면 DESC, 0이면 ASC */
    long limit;                   /* -1이면 LIMIT 없음 */
};

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

/* 파싱된 문장이 malloc한 것(서브쿼리·IN 집합)을 해제한다. db_exec가 실행 뒤 부른다. */
void statement_free(Statement *st);

#endif /* MINIDB_SQL_H */
