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
    TOK_BEGIN, TOK_COMMIT, TOK_ROLLBACK,
    TOK_DELETE, TOK_UPDATE, TOK_SET, TOK_AND, TOK_OR,
    TOK_ORDER, TOK_BY, TOK_ASC, TOK_DESC, TOK_LIMIT,
    TOK_JOIN, TOK_ON, TOK_GROUP,
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
    } else {
        p_fail(p, "값(정수 또는 '문자열')이 필요합니다");
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
        parse_where_op(p, &c->op);
        parse_value(p, &c->val);
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
        c->num_columns++;
        if (!p_accept(p, TOK_COMMA)) {
            break;
        }
    }
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

/* SELECT 목록: * 또는 콤마로 구분된 항목들. 항목 = 일반 컬럼 또는 FUNC(arg). */
static void parse_select_list(Parser *p, SelectStmt *s) {
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
        it->agg = AGG_NONE;
        it->star = 0;
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
                char qt[SQL_NAME_LEN];
                parse_colref(p, qt, it->col); /* 단일 테이블 집계라 한정자는 무시 */
            }
            p_expect(p, TOK_RPAREN, ") 가 필요합니다");
            s->has_aggregate = 1;
        } else if (p_accept(p, TOK_DOT)) { /* 한정 일반 컬럼 t.col */
            if (p->cur.type != TOK_IDENT) {
                p_fail(p, ". 다음에 컬럼 이름이 필요합니다");
                return;
            }
            snprintf(it->col, SQL_NAME_LEN, "%s", p->cur.text);
            p_advance(p);
        } else { /* 한정 없는 일반 컬럼 */
            snprintf(it->col, SQL_NAME_LEN, "%s", name);
        }
        s->num_items++;
    } while (p_accept(p, TOK_COMMA));
}

static void parse_select(Parser *p, Statement *st) {
    st->type = STMT_SELECT;
    SelectStmt *s = &st->select;
    s->limit = -1; /* memset이 0으로 둔 걸 "LIMIT 없음"으로 바로잡는다 */
    parse_select_list(p, s);
    p_expect(p, TOK_FROM, "FROM이 필요합니다");
    parse_name(p, s->table);
    if (p->cur.type == TOK_IDENT) { /* 테이블 뒤 식별자는 별칭 (키워드는 별도 토큰이라 안 걸림) */
        parse_name(p, s->alias);
    }
    while (p_accept(p, TOK_JOIN)) {
        if (s->num_joins >= SQL_MAX_JOINS) {
            p_fail(p, "JOIN이 너무 많습니다");
            break;
        }
        JoinClause *jc = &s->joins[s->num_joins];
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
        char qt[SQL_NAME_LEN];
        parse_colref(p, qt, s->group_col); /* 단일 테이블 집계라 한정자는 무시 */
    }
    if (p_accept(p, TOK_ORDER)) {
        p_expect(p, TOK_BY, "ORDER 다음에 BY가 필요합니다");
        parse_colref(p, s->order_tbl, s->order_col);
        if (p_accept(p, TOK_DESC)) {
            s->order_desc = 1;
        } else {
            p_accept(p, TOK_ASC); /* ASC는 기본값, 있어도 그만 */
        }
    }
    if (p_accept(p, TOK_LIMIT)) {
        if (p->cur.type != TOK_INT || p->cur.int_val < 0) {
            p_fail(p, "LIMIT 뒤에는 0 이상의 정수가 필요합니다");
        } else {
            s->limit = p->cur.int_val;
            p_advance(p);
        }
    }
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

int sql_parse(const char *sql, Statement *out, char *errbuf, size_t errlen) {
    Parser p;
    memset(&p, 0, sizeof(p));
    p.lex.src = sql;
    p.lex.pos = 0;
    p.ok = 1;
    p_advance(&p); /* 첫 토큰 준비 */

    memset(out, 0, sizeof(*out));
    switch (p.cur.type) {
        case TOK_CREATE: p_advance(&p); parse_create(&p, out); break;
        case TOK_INSERT: p_advance(&p); parse_insert(&p, out); break;
        case TOK_SELECT: p_advance(&p); parse_select(&p, out); break;
        case TOK_DELETE: p_advance(&p); parse_delete(&p, out); break;
        case TOK_UPDATE: p_advance(&p); parse_update(&p, out); break;
        case TOK_BEGIN: out->type = STMT_BEGIN; p_advance(&p); break;
        case TOK_COMMIT: out->type = STMT_COMMIT; p_advance(&p); break;
        case TOK_ROLLBACK: out->type = STMT_ROLLBACK; p_advance(&p); break;
        default: p_fail(&p, "CREATE / INSERT / SELECT / BEGIN / COMMIT / ROLLBACK 로 시작해야 합니다"); break;
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
