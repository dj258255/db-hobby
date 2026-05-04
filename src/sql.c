#include "sql.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

/* ------------------------- 토크나이저(lexer) ------------------------- */

typedef enum {
    TOK_EOF, TOK_IDENT, TOK_INT, TOK_STRING,
    TOK_LPAREN, TOK_RPAREN, TOK_COMMA, TOK_STAR, TOK_EQ, TOK_SEMI, TOK_DOT,
    TOK_LT, TOK_GT, TOK_LE, TOK_GE, TOK_NE,
    TOK_CREATE, TOK_TABLE, TOK_INSERT, TOK_INTO, TOK_VALUES,
    TOK_SELECT, TOK_FROM, TOK_WHERE, TOK_INT_TYPE, TOK_TEXT_TYPE,
    TOK_BEGIN, TOK_COMMIT, TOK_ROLLBACK, TOK_VACUUM, TOK_SESSION, TOK_ANALYZE,
    TOK_DELETE, TOK_UPDATE, TOK_SET, TOK_AND, TOK_OR,
    TOK_ORDER, TOK_BY, TOK_ASC, TOK_DESC, TOK_LIMIT,
    TOK_JOIN, TOK_ON, TOK_GROUP, TOK_HAVING, TOK_LEFT, TOK_OUTER,
    TOK_IS, TOK_NOT, TOK_NULL, TOK_DISTINCT, TOK_IN, TOK_OFFSET, TOK_BETWEEN, TOK_LIKE,
    TOK_EXPLAIN, TOK_INDEX,
    TOK_ERROR
} TokType;

typedef struct {
    TokType type;
    char text[SQL_TEXT_LEN];
    long int_val;
} Token;

typedef struct {
    const char *src;
    size_t pos;
} Lexer;

static TokType keyword_of(const char *s) {
    if (!strcasecmp(s, "CREATE")) return TOK_CREATE;
    if (!strcasecmp(s, "TABLE")) return TOK_TABLE;
    if (!strcasecmp(s, "INSERT")) return TOK_INSERT;
    if (!strcasecmp(s, "INTO")) return TOK_INTO;
    if (!strcasecmp(s, "VALUES")) return TOK_VALUES;
    if (!strcasecmp(s, "SELECT")) return TOK_SELECT;
    if (!strcasecmp(s, "FROM")) return TOK_FROM;
    if (!strcasecmp(s, "WHERE")) return TOK_WHERE;
    if (!strcasecmp(s, "INT")) return TOK_INT_TYPE;
    if (!strcasecmp(s, "TEXT")) return TOK_TEXT_TYPE;
    if (!strcasecmp(s, "BEGIN")) return TOK_BEGIN;
    if (!strcasecmp(s, "COMMIT")) return TOK_COMMIT;
    if (!strcasecmp(s, "ROLLBACK")) return TOK_ROLLBACK;
    if (!strcasecmp(s, "VACUUM")) return TOK_VACUUM;
    if (!strcasecmp(s, "SESSION")) return TOK_SESSION;
    if (!strcasecmp(s, "ANALYZE")) return TOK_ANALYZE;
    if (!strcasecmp(s, "DELETE")) return TOK_DELETE;
    if (!strcasecmp(s, "UPDATE")) return TOK_UPDATE;
    if (!strcasecmp(s, "SET")) return TOK_SET;
    if (!strcasecmp(s, "AND")) return TOK_AND;
    if (!strcasecmp(s, "OR")) return TOK_OR;
    if (!strcasecmp(s, "ORDER")) return TOK_ORDER;
    if (!strcasecmp(s, "BY")) return TOK_BY;
    if (!strcasecmp(s, "ASC")) return TOK_ASC;
    if (!strcasecmp(s, "DESC")) return TOK_DESC;
    if (!strcasecmp(s, "LIMIT")) return TOK_LIMIT;
    if (!strcasecmp(s, "JOIN")) return TOK_JOIN;
    if (!strcasecmp(s, "ON")) return TOK_ON;
    if (!strcasecmp(s, "GROUP")) return TOK_GROUP;
    if (!strcasecmp(s, "HAVING")) return TOK_HAVING;
    if (!strcasecmp(s, "LEFT")) return TOK_LEFT;
    if (!strcasecmp(s, "OUTER")) return TOK_OUTER;
    if (!strcasecmp(s, "IS")) return TOK_IS;
    if (!strcasecmp(s, "NOT")) return TOK_NOT;
    if (!strcasecmp(s, "NULL")) return TOK_NULL;
    if (!strcasecmp(s, "DISTINCT")) return TOK_DISTINCT;
    if (!strcasecmp(s, "IN")) return TOK_IN;
    if (!strcasecmp(s, "OFFSET")) return TOK_OFFSET;
    if (!strcasecmp(s, "BETWEEN")) return TOK_BETWEEN;
    if (!strcasecmp(s, "LIKE")) return TOK_LIKE;
    if (!strcasecmp(s, "EXPLAIN")) return TOK_EXPLAIN;
    if (!strcasecmp(s, "INDEX")) return TOK_INDEX;
    return TOK_IDENT;
}

static Token lex_next(Lexer *lx) {
    Token t;
    memset(&t, 0, sizeof(t));

    while (lx->src[lx->pos] && isspace((unsigned char)lx->src[lx->pos])) {
        lx->pos++;
    }
    char c = lx->src[lx->pos];
    if (c == '\0') {
        t.type = TOK_EOF;
        return t;
    }

    /* 비교 연산자 (=, !=, <, >, <=, >=). 두 글자를 먼저 본다. */
    if (c == '=' || c == '<' || c == '>' || c == '!') {
        char nx = lx->src[lx->pos + 1];
        if (c == '<' && nx == '=') { lx->pos += 2; t.type = TOK_LE; return t; }
        if (c == '<' && nx == '>') { lx->pos += 2; t.type = TOK_NE; return t; }
        if (c == '>' && nx == '=') { lx->pos += 2; t.type = TOK_GE; return t; }
        if (c == '!' && nx == '=') { lx->pos += 2; t.type = TOK_NE; return t; }
        if (c == '=') { lx->pos++; t.type = TOK_EQ; return t; }
        if (c == '<') { lx->pos++; t.type = TOK_LT; return t; }
        if (c == '>') { lx->pos++; t.type = TOK_GT; return t; }
        t.type = TOK_ERROR; /* 외톨이 '!' */
        return t;
    }

    switch (c) {
        case '(': lx->pos++; t.type = TOK_LPAREN; return t;
        case ')': lx->pos++; t.type = TOK_RPAREN; return t;
        case ',': lx->pos++; t.type = TOK_COMMA; return t;
        case '*': lx->pos++; t.type = TOK_STAR; return t;
        case ';': lx->pos++; t.type = TOK_SEMI; return t;
        case '.': lx->pos++; t.type = TOK_DOT; return t;
        default: break;
    }

    /* 'string literal' */
    if (c == '\'') {
        lx->pos++;
        size_t n = 0;
        while (lx->src[lx->pos] && lx->src[lx->pos] != '\'') {
            if (n < SQL_TEXT_LEN - 1) {
                t.text[n++] = lx->src[lx->pos];
            }
            lx->pos++;
        }
        if (lx->src[lx->pos] != '\'') {
            t.type = TOK_ERROR; /* 닫는 따옴표 없음 */
            return t;
        }
        lx->pos++;
        t.text[n] = '\0';
        t.type = TOK_STRING;
        return t;
    }

    /* 정수 (앞에 - 가능) */
    if (isdigit((unsigned char)c) ||
        (c == '-' && isdigit((unsigned char)lx->src[lx->pos + 1]))) {
        size_t start = lx->pos;
        if (c == '-') {
            lx->pos++;
        }
        while (isdigit((unsigned char)lx->src[lx->pos])) {
            lx->pos++;
        }
        char buf[64];
        size_t len = lx->pos - start;
        if (len >= sizeof(buf)) {
            len = sizeof(buf) - 1;
        }
        memcpy(buf, lx->src + start, len);
        buf[len] = '\0';
        t.int_val = strtol(buf, NULL, 10);
        t.type = TOK_INT;
        return t;
    }

    /* 식별자 / 키워드 */
    if (isalpha((unsigned char)c) || c == '_') {
        size_t n = 0;
        while (isalnum((unsigned char)lx->src[lx->pos]) || lx->src[lx->pos] == '_') {
            if (n < SQL_NAME_LEN - 1) {
                t.text[n++] = lx->src[lx->pos];
            }
            lx->pos++;
        }
        t.text[n] = '\0';
        t.type = keyword_of(t.text);
        return t;
    }

    t.type = TOK_ERROR;
    return t;
}

/* ------------------------- 파서(recursive descent) ------------------------- */

typedef struct {
    Lexer lex;
    Token cur;
    int ok;
    char err[128];
} Parser;

static void p_fail(Parser *p, const char *msg) {
    if (p->ok) {
        p->ok = 0;
        snprintf(p->err, sizeof(p->err), "%s", msg);
    }
}

static void p_advance(Parser *p) {
    p->cur = lex_next(&p->lex);
    if (p->cur.type == TOK_ERROR) {
        p_fail(p, "토큰을 해석할 수 없습니다");
    }
}

static int p_accept(Parser *p, TokType t) {
    if (p->cur.type == t) {
        p_advance(p);
        return 1;
    }
    return 0;
}

static void p_expect(Parser *p, TokType t, const char *what) {
    if (!p_accept(p, t)) {
        p_fail(p, what);
    }
}

static void parse_name(Parser *p, char *out) {
    if (p->cur.type == TOK_IDENT) {
        snprintf(out, SQL_NAME_LEN, "%s", p->cur.text);
        p_advance(p);
    } else {
        p_fail(p, "이름(식별자)이 필요합니다");
    }
}

/* 컬럼 참조: <ident> 또는 <ident>.<ident>. 앞쪽이 있으면 테이블 한정자. */
static void parse_colref(Parser *p, char *tbl_out, char *col_out) {
    tbl_out[0] = '\0';
    if (p->cur.type != TOK_IDENT) {
        p_fail(p, "컬럼 이름이 필요합니다");
        return;
    }
    char first[SQL_NAME_LEN];
    snprintf(first, SQL_NAME_LEN, "%s", p->cur.text);
    p_advance(p);
    if (p_accept(p, TOK_DOT)) {
        snprintf(tbl_out, SQL_NAME_LEN, "%s", first); /* first 는 테이블 */
        if (p->cur.type != TOK_IDENT) {
            p_fail(p, ". 다음에 컬럼 이름이 필요합니다");
            return;
        }
        snprintf(col_out, SQL_NAME_LEN, "%s", p->cur.text);
        p_advance(p);
    } else {
        snprintf(col_out, SQL_NAME_LEN, "%s", first); /* first 는 컬럼 */
    }
}

static void parse_value(Parser *p, Value *v) {
    if (p->cur.type == TOK_INT) {
        v->type = VAL_INT;
        v->int_val = p->cur.int_val;
        p_advance(p);
    } else if (p->cur.type == TOK_STRING) {
        v->type = VAL_TEXT;
        snprintf(v->text_val, SQL_TEXT_LEN, "%s", p->cur.text);
        p_advance(p);
    } else if (p->cur.type == TOK_NULL) {
        v->type = VAL_NULL;
        p_advance(p);
    } else {
        p_fail(p, "값(정수, '문자열', 또는 NULL)이 필요합니다");
    }
}

static void parse_where_op(Parser *p, CmpOp *out) {
    if (p_accept(p, TOK_EQ)) {
        *out = CMP_EQ;
    } else if (p_accept(p, TOK_NE)) {
        *out = CMP_NE;
    } else if (p_accept(p, TOK_LT)) {
        *out = CMP_LT;
    } else if (p_accept(p, TOK_GT)) {
        *out = CMP_GT;
    } else if (p_accept(p, TOK_LE)) {
        *out = CMP_LE;
    } else if (p_accept(p, TOK_GE)) {
        *out = CMP_GE;
    } else {
        p_fail(p, "비교 연산자(=, !=, <, >, <=, >=)가 필요합니다");
    }
}

static void parse_select_stmt(Parser *p, SelectStmt *s); /* 서브쿼리용 전방 선언 */

/* 한 AND 묶음: cond (AND cond)* */
static void parse_and_group(Parser *p, AndGroup *g) {
    g->count = 0;
    do {
        if (g->count >= SQL_MAX_CONDS) {
            p_fail(p, "AND 조건이 너무 많습니다");
            return;
        }
        Condition *c = &g->conds[g->count];
        parse_colref(p, c->tbl, c->col);
        int negate = p_accept(p, TOK_NOT); /* NOT IN / NOT LIKE */
        if (negate) { /* NOT 다음엔 IN 또는 LIKE만 온다 */
            if (p->cur.type != TOK_IN && p->cur.type != TOK_LIKE) {
                p_fail(p, "NOT 다음에 IN 또는 LIKE가 필요합니다");
                return;
            }
        }
        if (p_accept(p, TOK_LIKE)) { /* col [NOT] LIKE '패턴' — %, _ 와일드카드 */
            c->op = negate ? CMP_NOT_LIKE : CMP_LIKE;
            parse_value(p, &c->val);
            if (c->val.type != VAL_TEXT) {
                p_fail(p, "LIKE 패턴은 문자열이어야 합니다");
                return;
            }
        } else if (p_accept(p, TOK_IN)) { /* col [NOT] IN (...) — 서브쿼리 또는 값 목록 */
            p_expect(p, TOK_LPAREN, "IN 다음에 ( 가 필요합니다");
            if (p->cur.type == TOK_SELECT) { /* col [NOT] IN (SELECT ...) 서브쿼리 */
                p_advance(p); /* SELECT */
                SelectStmt *sub = calloc(1, sizeof(SelectStmt));
                if (!sub) {
                    p_fail(p, "메모리 부족");
                    return;
                }
                parse_select_stmt(p, sub);
                p_expect(p, TOK_RPAREN, "서브쿼리 뒤에 ) 가 필요합니다");
                c->in_sub = 1;
                c->in_negate = negate;
                c->sub = sub;
            } else { /* col [NOT] IN (v1, v2, ...) — 값 목록을 파싱 때 바로 in_set에 채운다 */
                int cap = 8, n = 0;
                Value *set = malloc(sizeof(Value) * cap);
                if (!set) {
                    p_fail(p, "메모리 부족");
                    return;
                }
                do {
                    if (n >= cap) {
                        cap *= 2;
                        Value *bigger = realloc(set, sizeof(Value) * cap);
                        if (!bigger) {
                            free(set);
                            p_fail(p, "메모리 부족");
                            return;
                        }
                        set = bigger;
                    }
                    parse_value(p, &set[n]);
                    n++;
                } while (p_accept(p, TOK_COMMA));
                p_expect(p, TOK_RPAREN, "IN 값 목록 뒤에 ) 가 필요합니다");
                /* 멤버십 머신(in_sub) 재사용 — sub는 NULL이라 prepare_where가 건너뛴다 */
                c->in_sub = 1;
                c->in_negate = negate;
                c->in_set = set;
                c->in_set_n = n;
            }
        } else if (p_accept(p, TOK_BETWEEN)) {
            /* col BETWEEN a AND b 는 문법 설탕 — (col >= a) AND (col <= b)로 푼다.
             * 양끝 포함(inclusive). 두 번째 조건을 같은 AND 묶음에 직접 더한다. */
            c->op = CMP_GE;
            parse_value(p, &c->val);
            p_expect(p, TOK_AND, "BETWEEN <값> 다음에 AND가 필요합니다");
            g->count++;
            if (g->count >= SQL_MAX_CONDS) {
                p_fail(p, "AND 조건이 너무 많습니다");
                return;
            }
            Condition *c2 = &g->conds[g->count];
            snprintf(c2->tbl, SQL_NAME_LEN, "%s", c->tbl);
            snprintf(c2->col, SQL_NAME_LEN, "%s", c->col);
            c2->op = CMP_LE;
            parse_value(p, &c2->val);
        } else if (p_accept(p, TOK_IS)) { /* IS [NOT] NULL — 값 없는 조건 */
            c->op = p_accept(p, TOK_NOT) ? CMP_IS_NOT_NULL : CMP_IS_NULL;
            p_expect(p, TOK_NULL, "IS [NOT] 다음에 NULL이 필요합니다");
        } else {
            parse_where_op(p, &c->op);
            if (p->cur.type == TOK_LPAREN) { /* col <op> (SELECT ...) 스칼라 서브쿼리 */
                p_advance(p); /* ( */
                p_expect(p, TOK_SELECT, "( 다음에 SELECT 서브쿼리가 필요합니다");
                SelectStmt *sub = calloc(1, sizeof(SelectStmt));
                if (!sub) {
                    p_fail(p, "메모리 부족");
                    return;
                }
                parse_select_stmt(p, sub);
                p_expect(p, TOK_RPAREN, "서브쿼리 뒤에 ) 가 필요합니다");
                c->in_sub = 1;
                c->scalar_sub = 1;
                c->sub = sub;
            } else {
                parse_value(p, &c->val);
            }
        }
        g->count++;
    } while (p_accept(p, TOK_AND));
}

/* WHERE 절: AND 묶음들을 OR로 잇는다(DNF). (WHERE 키워드는 이미 소비됨)
 *   term (OR term)*,  term = cond (AND cond)*
 * AND가 OR보다 먼저 묶이므로 a AND b OR c 는 (a AND b) OR c 가 된다. */
static void parse_where(Parser *p, Where *w) {
    w->count = 0;
    do {
        if (w->count >= SQL_MAX_GROUPS) {
            p_fail(p, "OR 묶음이 너무 많습니다");
            return;
        }
        parse_and_group(p, &w->groups[w->count]);
        w->count++;
    } while (p_accept(p, TOK_OR));
}

static void parse_create(Parser *p, Statement *st) {
    st->type = STMT_CREATE;
    CreateStmt *c = &st->create;
    c->num_columns = 0;
    p_expect(p, TOK_TABLE, "CREATE 다음에 TABLE이 필요합니다");
    parse_name(p, c->table);
    p_expect(p, TOK_LPAREN, "( 가 필요합니다");
    while (p->ok && p->cur.type != TOK_RPAREN) {
        if (c->num_columns >= SQL_MAX_COLS) {
            p_fail(p, "컬럼이 너무 많습니다");
            break;
        }
        ColumnDef *col = &c->columns[c->num_columns];
        parse_name(p, col->name);
        if (p_accept(p, TOK_INT_TYPE)) {
            col->type = COL_INT;
        } else if (p_accept(p, TOK_TEXT_TYPE)) {
            col->type = COL_TEXT;
        } else {
            p_fail(p, "컬럼 타입(INT 또는 TEXT)이 필요합니다");
            break;
        }
        col->not_null = 0;
        if (p_accept(p, TOK_NOT)) { /* 선택적 NOT NULL 제약 */
            p_expect(p, TOK_NULL, "NOT 다음에 NULL이 필요합니다");
            col->not_null = 1;
        }
        c->num_columns++;
        if (!p_accept(p, TOK_COMMA)) {
            break;
        }
    }
    p_expect(p, TOK_RPAREN, ") 가 필요합니다");
}

/* CREATE INDEX <name> ON <table>(<col>) */
static void parse_create_index(Parser *p, Statement *st) {
    st->type = STMT_CREATE_INDEX;
    CreateIndexStmt *ci = &st->cidx;
    p_expect(p, TOK_INDEX, "CREATE 다음에 TABLE 또는 INDEX가 필요합니다");
    parse_name(p, ci->name);
    p_expect(p, TOK_ON, "INDEX <이름> 다음에 ON이 필요합니다");
    parse_name(p, ci->table);
    p_expect(p, TOK_LPAREN, "테이블명 다음에 ( 가 필요합니다");
    parse_name(p, ci->column);
    p_expect(p, TOK_RPAREN, ") 가 필요합니다");
}

static void parse_insert(Parser *p, Statement *st) {
    st->type = STMT_INSERT;
    InsertStmt *in = &st->insert;
    in->num_values = 0;
    p_expect(p, TOK_INTO, "INSERT 다음에 INTO가 필요합니다");
    parse_name(p, in->table);
    p_expect(p, TOK_VALUES, "VALUES가 필요합니다");
    p_expect(p, TOK_LPAREN, "( 가 필요합니다");
    while (p->ok && p->cur.type != TOK_RPAREN) {
        if (in->num_values >= SQL_MAX_COLS) {
            p_fail(p, "값이 너무 많습니다");
            break;
        }
        parse_value(p, &in->values[in->num_values]);
        in->num_values++;
        if (!p_accept(p, TOK_COMMA)) {
            break;
        }
    }
    p_expect(p, TOK_RPAREN, ") 가 필요합니다");
}

static AggFunc agg_of(const char *s) {
    if (!strcasecmp(s, "COUNT")) return AGG_COUNT;
    if (!strcasecmp(s, "SUM")) return AGG_SUM;
    if (!strcasecmp(s, "MIN")) return AGG_MIN;
    if (!strcasecmp(s, "MAX")) return AGG_MAX;
    if (!strcasecmp(s, "AVG")) return AGG_AVG;
    return AGG_NONE;
}

/* 한 항목: 일반 컬럼([t.]col) 또는 집계 FUNC(arg). is_agg가 채워지면 집계. */
static void parse_select_item(Parser *p, SelectItem *it) {
    it->agg = AGG_NONE;
    it->star = 0;
    it->tbl[0] = '\0';
    it->col[0] = '\0';
    if (p->cur.type != TOK_IDENT) {
        p_fail(p, "컬럼 또는 집계 함수가 필요합니다");
        return;
    }
    char name[SQL_NAME_LEN];
    snprintf(name, sizeof(name), "%s", p->cur.text);
    p_advance(p);
    if (p->cur.type == TOK_LPAREN) { /* 집계: FUNC(...) */
        AggFunc af = agg_of(name);
        if (af == AGG_NONE) {
            p_fail(p, "알 수 없는 집계 함수입니다");
            return;
        }
        it->agg = af;
        p_advance(p); /* ( */
        if (p_accept(p, TOK_STAR)) {
            it->star = 1;
            if (af != AGG_COUNT) {
                p_fail(p, "*는 COUNT에만 쓸 수 있습니다");
                return;
            }
        } else {
            parse_colref(p, it->tbl, it->col); /* [tbl.]col */
        }
        p_expect(p, TOK_RPAREN, ") 가 필요합니다");
    } else if (p_accept(p, TOK_DOT)) { /* 한정 일반 컬럼 t.col */
        if (p->cur.type != TOK_IDENT) {
            p_fail(p, ". 다음에 컬럼 이름이 필요합니다");
            return;
        }
        snprintf(it->tbl, SQL_NAME_LEN, "%s", name);
        snprintf(it->col, SQL_NAME_LEN, "%s", p->cur.text);
        p_advance(p);
    } else { /* 한정 없는 일반 컬럼 */
        snprintf(it->col, SQL_NAME_LEN, "%s", name);
    }
}

/* SELECT [DISTINCT] 목록: * 또는 콤마로 구분된 항목들. */
static void parse_select_list(Parser *p, SelectStmt *s) {
    if (p_accept(p, TOK_DISTINCT)) {
        s->distinct = 1;
    }
    if (p_accept(p, TOK_STAR)) {
        s->select_star = 1;
        return;
    }
    do {
        if (s->num_items >= SQL_MAX_COLS) {
            p_fail(p, "SELECT 항목이 너무 많습니다");
            return;
        }
        SelectItem *it = &s->items[s->num_items];
        parse_select_item(p, it);
        if (it->agg != AGG_NONE) {
            s->has_aggregate = 1;
        }
        s->num_items++;
    } while (p_accept(p, TOK_COMMA));
}

/* SELECT 본문(SELECT 키워드는 이미 소비됨)을 SelectStmt에 채운다. 서브쿼리도 이걸 쓴다. */
static void parse_select_stmt(Parser *p, SelectStmt *s) {
    s->limit = -1; /* memset/calloc이 0으로 둔 걸 "LIMIT 없음"으로 바로잡는다 */
    parse_select_list(p, s);
    p_expect(p, TOK_FROM, "FROM이 필요합니다");
    parse_name(p, s->table);
    if (p->cur.type == TOK_IDENT) { /* 테이블 뒤 식별자는 별칭 (키워드는 별도 토큰이라 안 걸림) */
        parse_name(p, s->alias);
    }
    for (;;) {
        int is_left = 0;
        if (p_accept(p, TOK_LEFT)) {
            p_accept(p, TOK_OUTER); /* OUTER는 선택 */
            is_left = 1;
            p_expect(p, TOK_JOIN, "LEFT 다음에 JOIN이 필요합니다");
        } else if (!p_accept(p, TOK_JOIN)) {
            break;
        }
        if (s->num_joins >= SQL_MAX_JOINS) {
            p_fail(p, "JOIN이 너무 많습니다");
            break;
        }
        JoinClause *jc = &s->joins[s->num_joins];
        jc->is_left = is_left;
        parse_name(p, jc->table);
        if (p->cur.type == TOK_IDENT) {
            parse_name(p, jc->alias);
        }
        p_expect(p, TOK_ON, "JOIN 다음에 ON이 필요합니다");
        parse_colref(p, jc->l_tbl, jc->l_col);
        p_expect(p, TOK_EQ, "ON 조건은 <컬럼> = <컬럼> 형태여야 합니다");
        parse_colref(p, jc->r_tbl, jc->r_col);
        s->num_joins++;
    }
    if (p_accept(p, TOK_WHERE)) {
        parse_where(p, &s->where);
    }
    if (p_accept(p, TOK_GROUP)) {
        p_expect(p, TOK_BY, "GROUP 다음에 BY가 필요합니다");
        parse_colref(p, s->group_tbl, s->group_col);
    }
    if (p_accept(p, TOK_HAVING)) {
        s->has_having = 1;
        parse_select_item(p, &s->having_agg); /* 보통 집계 항목 */
        parse_where_op(p, &s->having_op);
        parse_value(p, &s->having_val);
    }
    if (p_accept(p, TOK_ORDER)) {
        p_expect(p, TOK_BY, "ORDER 다음에 BY가 필요합니다");
        do { /* 키 (, 키)* — 다중 컬럼 정렬 */
            if (s->num_order >= SQL_MAX_ORDER) {
                p_fail(p, "ORDER BY 키가 너무 많습니다");
                break;
            }
            OrderKey *ok = &s->order_keys[s->num_order];
            if (p->cur.type == TOK_INT) { /* ORDER BY <위치> — 출력 컬럼 번호(1-based) */
                ok->pos = (int)p->cur.int_val;
                p_advance(p);
            } else {
                parse_colref(p, ok->tbl, ok->col);
            }
            if (p_accept(p, TOK_DESC)) {
                ok->desc = 1;
            } else {
                p_accept(p, TOK_ASC); /* ASC는 기본값, 있어도 그만 */
            }
            s->num_order++;
        } while (p_accept(p, TOK_COMMA));
    }
    if (p_accept(p, TOK_LIMIT)) {
        if (p->cur.type != TOK_INT || p->cur.int_val < 0) {
            p_fail(p, "LIMIT 뒤에는 0 이상의 정수가 필요합니다");
        } else {
            s->limit = p->cur.int_val;
            p_advance(p);
        }
    }
    if (p_accept(p, TOK_OFFSET)) {
        if (p->cur.type != TOK_INT || p->cur.int_val < 0) {
            p_fail(p, "OFFSET 뒤에는 0 이상의 정수가 필요합니다");
        } else {
            s->offset = p->cur.int_val;
            p_advance(p);
        }
    }
}

static void parse_select(Parser *p, Statement *st) {
    st->type = STMT_SELECT;
    parse_select_stmt(p, &st->select);
}

static void parse_delete(Parser *p, Statement *st) {
    st->type = STMT_DELETE;
    DeleteStmt *d = &st->del;
    p_expect(p, TOK_FROM, "DELETE 다음에 FROM이 필요합니다");
    parse_name(p, d->table);
    if (p_accept(p, TOK_WHERE)) {
        parse_where(p, &d->where);
    }
}

static void parse_update(Parser *p, Statement *st) {
    st->type = STMT_UPDATE;
    UpdateStmt *u = &st->upd;
    parse_name(p, u->table);
    p_expect(p, TOK_SET, "SET이 필요합니다");
    parse_name(p, u->set_col);
    p_expect(p, TOK_EQ, "= 가 필요합니다");
    parse_value(p, &u->set_val);
    if (p_accept(p, TOK_WHERE)) {
        parse_where(p, &u->where);
    }
}

/* Where 안의 서브쿼리·IN 집합을 재귀적으로 해제한다. */
static void free_where(Where *w) {
    for (int gi = 0; gi < w->count; gi++) {
        AndGroup *g = &w->groups[gi];
        for (int i = 0; i < g->count; i++) {
            Condition *c = &g->conds[i];
            if (c->in_sub) {
                if (c->sub) {
                    free_where(&c->sub->where); /* 서브쿼리 안의 서브쿼리도 */
                    free(c->sub);
                    c->sub = NULL;
                }
                free(c->in_set);
                c->in_set = NULL;
            }
        }
    }
}

void statement_free(Statement *st) {
    switch (st->type) {
        case STMT_SELECT: free_where(&st->select.where); break;
        case STMT_DELETE: free_where(&st->del.where); break;
        case STMT_UPDATE: free_where(&st->upd.where); break;
        default: break;
    }
}

int sql_parse(const char *sql, Statement *out, char *errbuf, size_t errlen) {
    Parser p;
    memset(&p, 0, sizeof(p));
    p.lex.src = sql;
    p.lex.pos = 0;
    p.ok = 1;
    p_advance(&p); /* 첫 토큰 준비 */

    memset(out, 0, sizeof(*out));
    switch (p.cur.type) {
        case TOK_CREATE:
            p_advance(&p);
            if (p.cur.type == TOK_INDEX) { /* CREATE INDEX ... */
                parse_create_index(&p, out);
            } else { /* CREATE TABLE ... */
                parse_create(&p, out);
            }
            break;
        case TOK_INSERT: p_advance(&p); parse_insert(&p, out); break;
        case TOK_SELECT: p_advance(&p); parse_select(&p, out); break;
        case TOK_EXPLAIN: /* EXPLAIN <select> — 플랜만 출력 */
            p_advance(&p);
            p_expect(&p, TOK_SELECT, "EXPLAIN 다음에 SELECT가 필요합니다");
            parse_select(&p, out);
            out->select.explain = 1;
            break;
        case TOK_DELETE: p_advance(&p); parse_delete(&p, out); break;
        case TOK_UPDATE: p_advance(&p); parse_update(&p, out); break;
        case TOK_BEGIN: out->type = STMT_BEGIN; p_advance(&p); break;
        case TOK_COMMIT: out->type = STMT_COMMIT; p_advance(&p); break;
        case TOK_ROLLBACK: out->type = STMT_ROLLBACK; p_advance(&p); break;
        case TOK_SESSION: /* SESSION <n> — 세션 전환 (다중 트랜잭션 인터리브) */
            out->type = STMT_SESSION;
            p_advance(&p);
            if (p.cur.type == TOK_INT) {
                out->sess.n = (int)p.cur.int_val;
                p_advance(&p);
            } else {
                p_fail(&p, "SESSION 다음에 세션 번호가 필요합니다");
            }
            break;
        case TOK_VACUUM: /* VACUUM [<table>] — 테이블 생략 시 전체 */
            out->type = STMT_VACUUM;
            p_advance(&p);
            if (p.cur.type == TOK_IDENT) {
                snprintf(out->vac.table, sizeof(out->vac.table), "%s", p.cur.text);
                p_advance(&p);
            }
            break;
        case TOK_ANALYZE: /* ANALYZE [<table>] — 통계 수집 */
            out->type = STMT_ANALYZE;
            p_advance(&p);
            if (p.cur.type == TOK_IDENT) {
                snprintf(out->analyze.table, sizeof(out->analyze.table), "%s", p.cur.text);
                p_advance(&p);
            }
            break;
        default: p_fail(&p, "CREATE / INSERT / SELECT / BEGIN / COMMIT / ROLLBACK / VACUUM 로 시작해야 합니다"); break;
    }

    if (p.ok) {
        p_accept(&p, TOK_SEMI); /* 끝의 ; 는 선택 */
        if (p.cur.type != TOK_EOF) {
            p_fail(&p, "문장 끝에 예상치 못한 토큰이 있습니다");
        }
    }

    if (!p.ok) {
        if (errbuf && errlen) {
            snprintf(errbuf, errlen, "%s", p.err);
        }
        return -1;
    }
    return 0;
}
