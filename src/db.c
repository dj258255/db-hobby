#include "db.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------- tuple codec: 값 <-> 바이트 -------------
 *   INT  : 4바이트 (int32)
 *   TEXT : 2바이트 길이 + 바이트열
 */

static int encode_row(const CreateStmt *schema, const Value *vals, int nvals,
                      uint8_t *buf, uint16_t *out_len) {
    if (nvals != schema->num_columns) {
        return -1;
    }
    uint16_t off = 0;
    for (int i = 0; i < schema->num_columns; i++) {
        const Value *v = &vals[i];
        if (schema->columns[i].type == COL_INT) {
            if (v->type != VAL_INT) {
                return -1;
            }
            int32_t x = (int32_t)v->int_val;
            memcpy(buf + off, &x, 4);
            off += 4;
        } else {
            if (v->type != VAL_TEXT) {
                return -1;
            }
            uint16_t len = (uint16_t)strlen(v->text_val);
            memcpy(buf + off, &len, 2);
            off += 2;
            memcpy(buf + off, v->text_val, len);
            off += len;
        }
    }
    *out_len = off;
    return 0;
}

static void decode_row(const CreateStmt *schema, const uint8_t *rec, Value *out) {
    uint16_t off = 0;
    for (int i = 0; i < schema->num_columns; i++) {
        if (schema->columns[i].type == COL_INT) {
            int32_t x;
            memcpy(&x, rec + off, 4);
            off += 4;
            out[i].type = VAL_INT;
            out[i].int_val = x;
        } else {
            uint16_t len;
            memcpy(&len, rec + off, 2);
            off += 2;
            out[i].type = VAL_TEXT;
            memcpy(out[i].text_val, rec + off, len);
            out[i].text_val[len] = '\0';
            off += len;
        }
    }
}

/* RID <-> int64 (인덱스 값으로 저장하려고). slot < 65536 이므로 안전. */
static int64_t rid_encode(RID r) {
    return (int64_t)r.page_id * 65536 + r.slot;
}
static RID rid_decode(int64_t v) {
    RID r;
    r.page_id = (page_id_t)(v / 65536);
    r.slot = (uint16_t)(v % 65536);
    return r;
}

static void print_value(FILE *out, const Value *v) {
    if (v->type == VAL_INT) {
        fprintf(out, "%ld", v->int_val);
    } else {
        fprintf(out, "%s", v->text_val);
    }
}

static void print_row(FILE *out, const CreateStmt *schema, const Value *row) {
    for (int i = 0; i < schema->num_columns; i++) {
        if (i) {
            fprintf(out, " | ");
        }
        print_value(out, &row[i]);
    }
    fprintf(out, "\n");
}

/* ------------- 카탈로그 (테이블 목록 + 스키마) -------------
 * PostgreSQL의 pg_class에 해당. 어떤 테이블이 있고 컬럼이 뭔지를 <path> 파일에
 * 그대로 직렬화한다(작은 메타데이터라 페이지 없이 단순 바이너리로 충분).
 */

static void catalog_write(Database *db) {
    FILE *f = fopen(db->path, "wb");
    if (!f) {
        return;
    }
    int32_t n = db->num_tables;
    fwrite(&n, sizeof(n), 1, f);
    for (int i = 0; i < db->num_tables; i++) {
        fwrite(&db->tables[i].schema, sizeof(CreateStmt), 1, f);
    }
    fclose(f);
}

/* 테이블 파일(.tbl, .idx)을 열어 Heap/B+Tree를 준비한다. schema는 미리 채워둘 것. */
static int table_open_files(Table *t, const char *dbpath) {
    char p[700];
    snprintf(p, sizeof(p), "%s.%s.tbl", dbpath, t->schema.table);
    if (pager_open(&t->pager, p) != 0) {
        return -1;
    }
    t->bp = bufpool_create(&t->pager, 16);
    if (!t->bp) {
        pager_close(&t->pager);
        return -1;
    }
    heap_init(&t->heap, t->bp, &t->pager, 0); /* 테이블 파일은 순수 힙: page 0부터 */
    t->has_index = 0;
    if (t->schema.num_columns > 0 && t->schema.columns[0].type == COL_INT) {
        char ip[700];
        snprintf(ip, sizeof(ip), "%s.%s.idx", dbpath, t->schema.table);
        if (btree_open(&t->index, ip) == 0) {
            t->has_index = 1;
        }
    }
    return 0;
}

static void table_close_files(Table *t) {
    if (t->has_index) {
        btree_close(&t->index);
        t->has_index = 0;
    }
    if (t->bp) {
        bufpool_flush_all(t->bp);
        bufpool_destroy(t->bp);
        t->bp = NULL;
    }
    pager_close(&t->pager);
}

static Table *find_table(Database *db, const char *name) {
    for (int i = 0; i < db->num_tables; i++) {
        if (strcmp(db->tables[i].schema.table, name) == 0) {
            return &db->tables[i];
        }
    }
    return NULL;
}

/* ------------- WHERE 평가 -------------
 * 한정자(tbl)가 있으면 그 테이블의 컬럼만, 없으면 이름으로 찾는다.
 */

/* 비교 결과 sign(<0,0,>0)에 연산자를 적용해 참/거짓을 낸다. */
static int cmp_apply(CmpOp op, long sign) {
    switch (op) {
        case CMP_EQ: return sign == 0;
        case CMP_NE: return sign != 0;
        case CMP_LT: return sign < 0;
        case CMP_GT: return sign > 0;
        case CMP_LE: return sign <= 0;
        case CMP_GE: return sign >= 0;
    }
    return 0;
}

/* 한 스키마에서 [qtbl.]col 에 해당하는 셀을 찾는다. 없으면 NULL.
 * qtbl이 있고 이 스키마의 테이블명과 다르면 이 테이블 소속이 아니다. */
static const Value *cell_in(const CreateStmt *s, const char *qtbl, const char *col,
                            const Value *row) {
    if (qtbl[0] && strcmp(qtbl, s->table) != 0) {
        return NULL;
    }
    for (int i = 0; i < s->num_columns; i++) {
        if (strcmp(s->columns[i].name, col) == 0) {
            return &row[i];
        }
    }
    return NULL;
}

/* 셀 하나에 <op> <val>을 적용. 셀이 없거나 타입이 안 맞으면 거짓. */
static int cond_eval(const Value *cell, const Condition *cond) {
    if (!cell) {
        return 0;
    }
    const Value *wv = &cond->val;
    if (cell->type == VAL_INT && wv->type == VAL_INT) {
        long sign = (cell->int_val < wv->int_val) ? -1 : (cell->int_val > wv->int_val ? 1 : 0);
        return cmp_apply(cond->op, sign);
    }
    if (cell->type == VAL_TEXT && wv->type == VAL_TEXT) {
        return cmp_apply(cond->op, (long)strcmp(cell->text_val, wv->text_val));
    }
    return 0;
}

static int values_equal(const Value *a, const Value *b) {
    if (a->type != b->type) {
        return 0;
    }
    if (a->type == VAL_INT) {
        return a->int_val == b->int_val;
    }
    return strcmp(a->text_val, b->text_val) == 0;
}

/* --- 단일 테이블 WHERE --- */
static int cond_matches(const CreateStmt *schema, const Condition *cond, const Value *row) {
    return cond_eval(cell_in(schema, cond->tbl, cond->col, row), cond);
}

static int group_matches(const CreateStmt *schema, const AndGroup *g, const Value *row) {
    for (int i = 0; i < g->count; i++) {
        if (!cond_matches(schema, &g->conds[i], row)) {
            return 0;
        }
    }
    return 1;
}

/* WHERE 절(DNF): 어느 한 AND 묶음이라도 참이면 참. count==0 이면 항상 참. */
static int where_matches(const CreateStmt *schema, const Where *w, const Value *row) {
    if (w->count == 0) {
        return 1;
    }
    for (int i = 0; i < w->count; i++) {
        if (group_matches(schema, &w->groups[i], row)) {
            return 1;
        }
    }
    return 0;
}

/* --- 조인된 두 행에 대한 WHERE (왼쪽 먼저, 없으면 오른쪽에서 컬럼을 찾는다) --- */
static const Value *cell_join(const CreateStmt *ls, const Value *lrow, const CreateStmt *rs,
                              const Value *rrow, const char *qtbl, const char *col) {
    const Value *v = cell_in(ls, qtbl, col, lrow);
    if (v) {
        return v;
    }
    return cell_in(rs, qtbl, col, rrow);
}

static int where_matches_join(const CreateStmt *ls, const Value *lrow, const CreateStmt *rs,
                              const Value *rrow, const Where *w) {
    if (w->count == 0) {
        return 1;
    }
    for (int gi = 0; gi < w->count; gi++) {
        const AndGroup *g = &w->groups[gi];
        int all = 1;
        for (int i = 0; i < g->count; i++) {
            const Condition *c = &g->conds[i];
            if (!cond_eval(cell_join(ls, lrow, rs, rrow, c->tbl, c->col), c)) {
                all = 0;
                break;
            }
        }
        if (all) {
            return 1;
        }
    }
    return 0;
}

/* ------------- 실행기: CREATE / INSERT ------------- */

static int exec_create(Database *db, const CreateStmt *c, FILE *out) {
    if (find_table(db, c->table)) {
        fprintf(out, "ERROR: 이미 테이블 '%s' 가 있습니다\n", c->table);
        return -1;
    }
    if (db->num_tables >= DB_MAX_TABLES) {
        fprintf(out, "ERROR: 테이블이 너무 많습니다 (최대 %d개)\n", DB_MAX_TABLES);
        return -1;
    }
    /* 카탈로그엔 없지만 디스크에 옛 파일이 남아 있을 수 있으니 깨끗이 지우고 시작한다. */
    char tp[700], ip[700];
    snprintf(tp, sizeof(tp), "%s.%s.tbl", db->path, c->table);
    snprintf(ip, sizeof(ip), "%s.%s.idx", db->path, c->table);
    unlink(tp);
    unlink(ip);

    Table *t = &db->tables[db->num_tables];
    t->schema = *c;
    if (table_open_files(t, db->path) != 0) {
        fprintf(out, "ERROR: 테이블 파일을 열 수 없습니다\n");
        return -1;
    }
    db->num_tables++;
    catalog_write(db);

    fprintf(out, "테이블 '%s' 생성됨 (컬럼 %d개)\n", c->table, c->num_columns);
    if (t->has_index) {
        fprintf(out, "  (인덱스: %s 컬럼)\n", t->schema.columns[0].name);
    }
    return 0;
}

static int exec_insert(Database *db, const InsertStmt *in, FILE *out) {
    Table *t = find_table(db, in->table);
    if (!t) {
        fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", in->table);
        return -1;
    }
    uint8_t buf[PAGE_SIZE];
    uint16_t len;
    if (encode_row(&t->schema, in->values, in->num_values, buf, &len) != 0) {
        fprintf(out, "ERROR: 값의 개수나 타입이 스키마와 맞지 않습니다\n");
        return -1;
    }
    RID rid;
    if (heap_insert(&t->heap, buf, len, &rid) != 0) {
        fprintf(out, "ERROR: 삽입 실패 (행이 너무 큼?)\n");
        return -1;
    }
    if (t->has_index && in->values[0].type == VAL_INT) {
        btree_insert(&t->index, in->values[0].int_val, rid_encode(rid));
    }
    fprintf(out, "1개 행 삽입됨\n");
    return 0;
}

/* ------------- 실행기: SELECT (단일 테이블) ------------- */

typedef struct {
    const CreateStmt *schema;
    const Where *where;
    FILE *out;
    int count;
} SelectCtx;

static int select_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)rid;
    (void)len;
    SelectCtx *ctx = ctx_;
    Value row[SQL_MAX_COLS];
    decode_row(ctx->schema, rec, row);
    if (!where_matches(ctx->schema, ctx->where, row)) {
        return 0;
    }
    print_row(ctx->out, ctx->schema, row);
    ctx->count++;
    return 0;
}

/* 인덱스 범위 스캔: B+Tree가 준 (키, RID)마다 힙 행을 읽어 출력한다.
 * tombstone(삭제된 슬롯)이면 heap_get이 -1이라 자동으로 빠진다. */
typedef struct {
    Table *t;
    bkey_t bound;
    CmpOp op;
    FILE *out;
    int count;
} RangeCtx;

static int range_visit(bkey_t key, bval_t val, void *ctx_) {
    RangeCtx *r = ctx_;
    if (r->op == CMP_GT && key == r->bound) return 0;  /* seek는 ==도 주니 건너뜀 */
    if (r->op == CMP_LT && key >= r->bound) return 1;  /* 상한 도달 -> 멈춤 */
    if (r->op == CMP_LE && key > r->bound) return 1;
    uint8_t recbuf[PAGE_SIZE];
    uint16_t len;
    if (heap_get(&r->t->heap, rid_decode(val), recbuf, &len) == 0) {
        Value row[SQL_MAX_COLS];
        decode_row(&r->t->schema, recbuf, row);
        print_row(r->out, &r->t->schema, row);
        r->count++;
    }
    return 0;
}

/* ORDER BY / LIMIT: materialize 경로. 스트리밍으론 정렬을 못 한다(마지막 행까지 봐야
 * 첫 출력 순서가 정해짐). WHERE에 맞는 행을 버퍼에 모은 뒤(= PostgreSQL의 Sort 노드)
 * 정렬하고 LIMIT만큼 자른다. 단순함을 위해 이 경로는 인덱스 대신 풀 스캔으로 모은다. */
#define SELECT_MAX_ROWS 4096

typedef struct {
    const CreateStmt *schema;
    const Where *where;
    Value *rows; /* count * ncols 짜리 평면 배열. 행 i는 rows + i*ncols */
    int ncols;
    int cap;
    int count;
} MatCtx;

static int mat_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)rid;
    (void)len;
    MatCtx *m = ctx_;
    if (m->count >= m->cap) {
        return 0; /* 버퍼 가득 — 학습용 상한 */
    }
    Value row[SQL_MAX_COLS];
    decode_row(m->schema, rec, row);
    if (!where_matches(m->schema, m->where, row)) {
        return 0;
    }
    memcpy(m->rows + (size_t)m->count * m->ncols, row, (size_t)m->ncols * sizeof(Value));
    m->count++;
    return 0;
}

/* qsort 비교기는 컨텍스트를 못 받으니(이식성 위해 qsort_r 회피) 정렬 컬럼만
 * 파일 정적으로 둔다. 단일 스레드 학습용이라 안전. ASC로만 비교하고
 * DESC는 정렬 뒤 배열을 뒤집어 처리한다. */
static int g_sort_ci;

static int row_cmp(const void *a, const void *b) {
    const Value *x = (const Value *)a + g_sort_ci;
    const Value *y = (const Value *)b + g_sort_ci;
    if (x->type == VAL_INT) {
        if (x->int_val < y->int_val) return -1;
        if (x->int_val > y->int_val) return 1;
        return 0;
    }
    return strcmp(x->text_val, y->text_val);
}

static int exec_select_sorted(Table *t, const SelectStmt *sel, FILE *out) {
    int ncols = t->schema.num_columns;
    Value *rows = malloc((size_t)SELECT_MAX_ROWS * ncols * sizeof(Value));
    if (!rows) {
        fprintf(out, "ERROR: 메모리 부족\n");
        return -1;
    }
    MatCtx m = {&t->schema, &sel->where, rows, ncols, SELECT_MAX_ROWS, 0};
    heap_scan(&t->heap, mat_visit, &m);

    if (sel->order_col[0] != '\0') {
        int ci = -1;
        for (int i = 0; i < ncols; i++) {
            if (strcmp(t->schema.columns[i].name, sel->order_col) == 0) {
                ci = i;
            }
        }
        if (ci < 0) {
            fprintf(out, "ERROR: ORDER BY 컬럼이 없습니다 (%s)\n", sel->order_col);
            free(rows);
            return -1;
        }
        g_sort_ci = ci;
        qsort(rows, m.count, (size_t)ncols * sizeof(Value), row_cmp);
        if (sel->order_desc) {
            /* ASC 정렬 결과를 뒤집어 DESC로 만든다 */
            Value tmp[SQL_MAX_COLS];
            for (int i = 0, j = m.count - 1; i < j; i++, j--) {
                memcpy(tmp, rows + (size_t)i * ncols, (size_t)ncols * sizeof(Value));
                memcpy(rows + (size_t)i * ncols, rows + (size_t)j * ncols,
                       (size_t)ncols * sizeof(Value));
                memcpy(rows + (size_t)j * ncols, tmp, (size_t)ncols * sizeof(Value));
            }
        }
    }

    int count = 0;
    for (int i = 0; i < m.count; i++) {
        if (sel->limit >= 0 && count >= sel->limit) {
            break;
        }
        print_row(out, &t->schema, rows + (size_t)i * ncols);
        count++;
    }
    free(rows);
    fprintf(out, "(%d행)\n", count);
    return 0;
}

/* ------------- 실행기: JOIN (중첩 루프 조인) ------------- */

typedef struct {
    Table *L, *R;
    const SelectStmt *sel;
    /* ON 양변을 (side: 0=왼쪽테이블 1=오른쪽테이블, idx: 컬럼 위치)로 미리 해소 */
    int al_side, al_idx;
    int ar_side, ar_idx;
    /* 인덱스 중첩 루프 조인: 안쪽(R)이 PK 인덱스를 갖고 ON이 그 PK와 맞으면,
     * 안쪽 전체 스캔 대신 바깥 행의 키로 점 조회한다. */
    int use_index;     /* 1이면 인덱스 NLJ */
    int outer_key_idx; /* 바깥(L) 행에서 조인 키를 꺼낼 컬럼 위치 */
    FILE *out;
    int count;
    const Value *lrow; /* 바깥 루프의 현재 행 */
} JoinCtx;

/* 결합된 (lrow, rrow) 한 쌍을 ON·WHERE로 거른 뒤 출력한다.
 * LIMIT에 도달했으면 1을 돌려 바깥/안쪽 루프를 멈추게 한다. */
static int join_emit(JoinCtx *j, const Value *lrow, const Value *rrow) {
    const Value *a = (j->al_side == 0) ? &lrow[j->al_idx] : &rrow[j->al_idx];
    const Value *b = (j->ar_side == 0) ? &lrow[j->ar_idx] : &rrow[j->ar_idx];
    if (!values_equal(a, b)) {
        return 0;
    }
    if (!where_matches_join(&j->L->schema, lrow, &j->R->schema, rrow, &j->sel->where)) {
        return 0;
    }
    /* 결합 행 출력: 왼쪽 컬럼들 다음 오른쪽 컬럼들 */
    for (int i = 0; i < j->L->schema.num_columns; i++) {
        if (i) {
            fprintf(j->out, " | ");
        }
        print_value(j->out, &lrow[i]);
    }
    for (int i = 0; i < j->R->schema.num_columns; i++) {
        fprintf(j->out, " | ");
        print_value(j->out, &rrow[i]);
    }
    fprintf(j->out, "\n");
    j->count++;
    return (j->sel->limit >= 0 && j->count >= j->sel->limit); /* LIMIT 도달 -> 멈춤 */
}

/* [qtbl.]col 을 (side, idx)로 해소한다. qtbl 없으면 왼쪽부터 찾는다. 0 성공, -1 실패. */
static int resolve_ref(Table *L, Table *R, const char *qtbl, const char *col, int *side,
                       int *idx) {
    if (qtbl[0]) {
        Table *t = NULL;
        int s = -1;
        if (strcmp(qtbl, L->schema.table) == 0) {
            t = L;
            s = 0;
        } else if (strcmp(qtbl, R->schema.table) == 0) {
            t = R;
            s = 1;
        } else {
            return -1;
        }
        for (int i = 0; i < t->schema.num_columns; i++) {
            if (strcmp(t->schema.columns[i].name, col) == 0) {
                *side = s;
                *idx = i;
                return 0;
            }
        }
        return -1;
    }
    for (int i = 0; i < L->schema.num_columns; i++) {
        if (strcmp(L->schema.columns[i].name, col) == 0) {
            *side = 0;
            *idx = i;
            return 0;
        }
    }
    for (int i = 0; i < R->schema.num_columns; i++) {
        if (strcmp(R->schema.columns[i].name, col) == 0) {
            *side = 1;
            *idx = i;
            return 0;
        }
    }
    return -1;
}

static int join_inner_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)rid;
    (void)len;
    JoinCtx *j = ctx_;
    Value rrow[SQL_MAX_COLS];
    decode_row(&j->R->schema, rec, rrow);
    return join_emit(j, j->lrow, rrow);
}

static int join_outer_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)rid;
    (void)len;
    JoinCtx *j = ctx_;
    Value lrow[SQL_MAX_COLS];
    decode_row(&j->L->schema, rec, lrow);
    j->lrow = lrow;
    int r;
    if (j->use_index) {
        /* 인덱스 NLJ: 안쪽 전체 스캔 대신 바깥 행의 키로 R의 PK 인덱스를 점 조회. */
        r = 0;
        const Value *k = &lrow[j->outer_key_idx];
        if (k->type == VAL_INT) {
            bval_t encoded;
            if (btree_search(&j->R->index, k->int_val, &encoded) == 0) {
                uint8_t recbuf[PAGE_SIZE];
                uint16_t len2;
                if (heap_get(&j->R->heap, rid_decode(encoded), recbuf, &len2) == 0) {
                    Value rrow[SQL_MAX_COLS];
                    decode_row(&j->R->schema, recbuf, rrow);
                    r = join_emit(j, lrow, rrow);
                }
            }
        }
    } else {
        r = heap_scan(&j->R->heap, join_inner_visit, j); /* 바깥 한 행마다 안쪽 전체 스캔 */
    }
    j->lrow = NULL;
    return r; /* 안쪽이 LIMIT로 멈췄으면(!=0) 바깥도 멈춘다 */
}

static int exec_select_join(Database *db, const SelectStmt *sel, FILE *out) {
    Table *L = find_table(db, sel->table);
    Table *R = find_table(db, sel->join.table);
    if (!L || !R) {
        fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", !L ? sel->table : sel->join.table);
        return -1;
    }
    if (sel->order_col[0] != '\0') {
        fprintf(out, "ERROR: JOIN과 ORDER BY는 아직 함께 못 씁니다\n");
        return -1;
    }

    JoinCtx j = {L, R, sel, 0, 0, 0, 0, 0, 0, out, 0, NULL};
    if (resolve_ref(L, R, sel->join.l_tbl, sel->join.l_col, &j.al_side, &j.al_idx) != 0 ||
        resolve_ref(L, R, sel->join.r_tbl, sel->join.r_col, &j.ar_side, &j.ar_idx) != 0) {
        fprintf(out, "ERROR: ON 절의 컬럼을 찾을 수 없습니다\n");
        return -1;
    }

    /* 인덱스 NLJ 가능 여부: 한 변이 R의 PK(첫 컬럼)이고 R에 인덱스가 있으며,
     * 다른 변이 바깥(L)의 컬럼이면, 안쪽 전체 스캔을 점 조회로 바꾼다. */
    if (R->has_index) {
        if (j.ar_side == 1 && j.ar_idx == 0 && j.al_side == 0) {
            j.use_index = 1;
            j.outer_key_idx = j.al_idx;
        } else if (j.al_side == 1 && j.al_idx == 0 && j.ar_side == 0) {
            j.use_index = 1;
            j.outer_key_idx = j.ar_idx;
        }
    }

    /* 헤더: 양 테이블 컬럼을 table.col 로 한정해 출력 */
    for (int i = 0; i < L->schema.num_columns; i++) {
        if (i) {
            fprintf(out, " | ");
        }
        fprintf(out, "%s.%s", L->schema.table, L->schema.columns[i].name);
    }
    for (int i = 0; i < R->schema.num_columns; i++) {
        fprintf(out, " | %s.%s", R->schema.table, R->schema.columns[i].name);
    }
    fprintf(out, "\n");

    db->used_index = j.use_index;
    heap_scan(&L->heap, join_outer_visit, &j);
    fprintf(out, "(%d행%s)\n", j.count, j.use_index ? ", 인덱스 조인" : "");
    return 0;
}

static int exec_select(Database *db, const SelectStmt *sel, FILE *out) {
    if (sel->join.has_join) {
        return exec_select_join(db, sel, out);
    }

    Table *t = find_table(db, sel->table);
    if (!t) {
        fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", sel->table);
        return -1;
    }
    /* 헤더 */
    for (int i = 0; i < t->schema.num_columns; i++) {
        if (i) {
            fprintf(out, " | ");
        }
        fprintf(out, "%s", t->schema.columns[i].name);
    }
    fprintf(out, "\n");

    db->used_index = 0;

    /* ORDER BY나 LIMIT이 있으면 모았다가 정렬/자르는 경로로 간다. */
    if (sel->order_col[0] != '\0' || sel->limit >= 0) {
        return exec_select_sorted(t, sel, out);
    }

    int count = 0;

    /* WHERE가 "PK(첫 컬럼) 정수 비교" 단일 조건이면 인덱스를 쓴다.
     * (OR 묶음 하나, 그 안에 조건 하나일 때만.) */
    const Condition *c0 = (sel->where.count == 1 && sel->where.groups[0].count == 1)
                              ? &sel->where.groups[0].conds[0]
                              : NULL;
    int pk_cond = c0 && t->has_index &&
                  (c0->tbl[0] == '\0' || strcmp(c0->tbl, t->schema.table) == 0) &&
                  strcmp(c0->col, t->schema.columns[0].name) == 0 && c0->val.type == VAL_INT;

    if (pk_cond && c0->op == CMP_EQ) {
        /* = -> 점 조회 O(log n) */
        db->used_index = 1;
        bval_t encoded;
        if (btree_search(&t->index, c0->val.int_val, &encoded) == 0) {
            uint8_t recbuf[PAGE_SIZE];
            uint16_t len;
            if (heap_get(&t->heap, rid_decode(encoded), recbuf, &len) == 0) {
                Value row[SQL_MAX_COLS];
                decode_row(&t->schema, recbuf, row);
                print_row(out, &t->schema, row);
                count++;
            }
        }
    } else if (pk_cond && (c0->op == CMP_GT || c0->op == CMP_GE || c0->op == CMP_LT ||
                           c0->op == CMP_LE)) {
        /* <, >, <=, >= -> 인덱스 범위 스캔 (리프 체인) */
        db->used_index = 1;
        RangeCtx rc = {t, c0->val.int_val, c0->op, out, 0};
        if (c0->op == CMP_GT || c0->op == CMP_GE) {
            btree_seek_scan(&t->index, c0->val.int_val, range_visit, &rc);
        } else {
            btree_scan(&t->index, range_visit, &rc);
        }
        count = rc.count;
    } else {
        /* 그 외(WHERE 없음/복합/비PK/TEXT 비교) -> 풀 스캔 */
        SelectCtx ctx = {&t->schema, &sel->where, out, 0};
        heap_scan(&t->heap, select_visit, &ctx);
        count = ctx.count;
    }

    fprintf(out, "(%d행%s)\n", count, db->used_index ? ", 인덱스 사용" : "");
    return 0;
}

/* ------------- 실행기: DELETE / UPDATE -------------
 * WHERE에 맞는 RID를 먼저 모은 뒤 처리한다. 스캔하며 바로 고치면 새로 삽입한 행을
 * 다시 스캔하는 문제가 생긴다. */
#define DML_MAX_ROWS 4096
typedef struct {
    RID rids[DML_MAX_ROWS];
    int count;
    const CreateStmt *schema;
    const Where *where;
} CollectCtx;

static int collect_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)len;
    CollectCtx *c = ctx_;
    Value row[SQL_MAX_COLS];
    decode_row(c->schema, rec, row);
    if (where_matches(c->schema, c->where, row) && c->count < DML_MAX_ROWS) {
        c->rids[c->count++] = rid;
    }
    return 0;
}

static int exec_delete(Database *db, const DeleteStmt *d, FILE *out) {
    Table *t = find_table(db, d->table);
    if (!t) {
        fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", d->table);
        return -1;
    }
    CollectCtx ctx = {.count = 0, .schema = &t->schema, .where = &d->where};
    heap_scan(&t->heap, collect_visit, &ctx);
    for (int i = 0; i < ctx.count; i++) {
        heap_delete(&t->heap, ctx.rids[i]);
    }
    /* 인덱스 항목은 남겨둬도 무해하다: 가리키는 슬롯이 tombstone이라 heap_get이 -1을
     * 돌려줘 결과에서 자동으로 빠진다. (B+Tree 삭제는 별도 주제로 미룬다.) */
    fprintf(out, "%d개 행 삭제됨\n", ctx.count);
    return 0;
}

static int exec_update(Database *db, const UpdateStmt *u, FILE *out) {
    Table *t = find_table(db, u->table);
    if (!t) {
        fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", u->table);
        return -1;
    }
    int sci = -1;
    for (int i = 0; i < t->schema.num_columns; i++) {
        if (strcmp(t->schema.columns[i].name, u->set_col) == 0) {
            sci = i;
        }
    }
    if (sci < 0) {
        fprintf(out, "ERROR: 그런 컬럼이 없습니다 (%s)\n", u->set_col);
        return -1;
    }
    ColType ct = t->schema.columns[sci].type;
    if ((ct == COL_INT && u->set_val.type != VAL_INT) ||
        (ct == COL_TEXT && u->set_val.type != VAL_TEXT)) {
        fprintf(out, "ERROR: SET 값 타입이 컬럼과 맞지 않습니다\n");
        return -1;
    }

    CollectCtx ctx = {.count = 0, .schema = &t->schema, .where = &u->where};
    heap_scan(&t->heap, collect_visit, &ctx);

    int n = 0;
    for (int i = 0; i < ctx.count; i++) {
        uint8_t recbuf[PAGE_SIZE];
        uint16_t len;
        if (heap_get(&t->heap, ctx.rids[i], recbuf, &len) != 0) {
            continue;
        }
        Value row[SQL_MAX_COLS];
        decode_row(&t->schema, recbuf, row);
        row[sci] = u->set_val; /* SET 적용 */

        /* 가변 길이라 제자리 수정이 안 된다 -> 옛 행 삭제 + 새 행 삽입 */
        uint8_t newbuf[PAGE_SIZE];
        uint16_t newlen;
        if (encode_row(&t->schema, row, t->schema.num_columns, newbuf, &newlen) != 0) {
            continue;
        }
        heap_delete(&t->heap, ctx.rids[i]);
        RID newrid;
        if (heap_insert(&t->heap, newbuf, newlen, &newrid) != 0) {
            continue;
        }
        /* 새 RID로 인덱스 갱신 — 안 하면 인덱스가 삭제된 옛 위치를 가리켜 행이 사라진다 */
        if (t->has_index && row[0].type == VAL_INT) {
            btree_insert(&t->index, row[0].int_val, rid_encode(newrid));
        }
        n++;
    }
    fprintf(out, "%d개 행 수정됨\n", n);
    return 0;
}

/* ------------- 트랜잭션 -------------
 * no-steal + 커밋 시 force. 모든 테이블에 걸쳐 적용한다. 트랜잭션 중 바뀐 페이지는
 * 버퍼 풀 메모리에만 두고, COMMIT이면 flush+fsync(내구), ROLLBACK이면 dirty 프레임을
 * 버리고 할당분을 잘라 디스크 원본으로 되돌린다. 힙과 인덱스 둘 다 처리한다.
 * (DDL인 CREATE는 즉시 반영되며 트랜잭션에 묶이지 않는다 — 많은 DB의 동작과 같다.) */

static int exec_begin(Database *db, FILE *out) {
    if (db->in_txn) {
        fprintf(out, "ERROR: 이미 트랜잭션 중입니다\n");
        return -1;
    }
    db->in_txn = 1;
    for (int i = 0; i < db->num_tables; i++) {
        Table *t = &db->tables[i];
        bufpool_set_no_steal(t->bp, 1);
        t->txn_data_pages = t->pager.num_pages;
        if (t->has_index) {
            bufpool_set_no_steal(t->index.bp, 1);
            t->txn_index_pages = t->index.pager.num_pages;
        }
    }
    fprintf(out, "트랜잭션 시작\n");
    return 0;
}

static int exec_commit(Database *db, FILE *out) {
    if (!db->in_txn) {
        fprintf(out, "ERROR: 트랜잭션이 없습니다\n");
        return -1;
    }
    for (int i = 0; i < db->num_tables; i++) {
        Table *t = &db->tables[i];
        bufpool_flush_all(t->bp);
        fsync(t->pager.fd);
        bufpool_set_no_steal(t->bp, 0);
        if (t->has_index) {
            bufpool_flush_all(t->index.bp);
            fsync(t->index.pager.fd);
            bufpool_set_no_steal(t->index.bp, 0);
        }
    }
    db->in_txn = 0;
    fprintf(out, "커밋됨\n");
    return 0;
}

static int exec_rollback(Database *db, FILE *out) {
    if (!db->in_txn) {
        fprintf(out, "ERROR: 트랜잭션이 없습니다\n");
        return -1;
    }
    for (int i = 0; i < db->num_tables; i++) {
        Table *t = &db->tables[i];
        bufpool_discard_dirty(t->bp);
        pager_truncate(&t->pager, t->txn_data_pages);
        bufpool_set_no_steal(t->bp, 0);
        if (t->has_index) {
            bufpool_discard_dirty(t->index.bp);
            pager_truncate(&t->index.pager, t->txn_index_pages);
            btree_reload_root(&t->index); /* 루트가 분할로 바뀌었을 수 있으니 다시 읽는다 */
            bufpool_set_no_steal(t->index.bp, 0);
        }
    }
    db->in_txn = 0;
    fprintf(out, "롤백됨\n");
    return 0;
}

/* ------------- 공개 API ------------- */

int db_open(Database *db, const char *path) {
    snprintf(db->path, sizeof(db->path), "%s", path);
    db->num_tables = 0;
    db->used_index = 0;
    db->in_txn = 0;

    /* 카탈로그가 있으면 테이블 목록을 복원하고 각 테이블 파일을 연다. */
    FILE *f = fopen(path, "rb");
    if (f) {
        int32_t n = 0;
        if (fread(&n, sizeof(n), 1, f) == 1 && n >= 0 && n <= DB_MAX_TABLES) {
            for (int i = 0; i < n; i++) {
                CreateStmt s;
                if (fread(&s, sizeof(s), 1, f) != 1) {
                    break;
                }
                Table *t = &db->tables[db->num_tables];
                t->schema = s;
                if (table_open_files(t, db->path) == 0) {
                    db->num_tables++;
                }
            }
        }
        fclose(f);
    }
    return 0;
}

void db_close(Database *db) {
    for (int i = 0; i < db->num_tables; i++) {
        table_close_files(&db->tables[i]);
    }
    db->num_tables = 0;
}

int db_exec(Database *db, const char *sql, FILE *out) {
    Statement st;
    char err[128];
    if (sql_parse(sql, &st, err, sizeof(err)) != 0) {
        fprintf(out, "ERROR: %s\n", err);
        return -1;
    }
    int rc;
    switch (st.type) {
        case STMT_CREATE: rc = exec_create(db, &st.create, out); break;
        case STMT_INSERT: rc = exec_insert(db, &st.insert, out); break;
        case STMT_SELECT: rc = exec_select(db, &st.select, out); break;
        case STMT_DELETE: rc = exec_delete(db, &st.del, out); break;
        case STMT_UPDATE: rc = exec_update(db, &st.upd, out); break;
        case STMT_BEGIN: rc = exec_begin(db, out); break;
        case STMT_COMMIT: rc = exec_commit(db, out); break;
        case STMT_ROLLBACK: rc = exec_rollback(db, out); break;
        default: rc = -1;
    }
    /* autocommit: 트랜잭션 밖에서 성공한 변경은 즉시 디스크에 반영해 클린으로 만든다.
     * 그래야 이후 ROLLBACK이 이 변경까지 되돌리지 않는다. */
    if (rc == 0 && !db->in_txn &&
        (st.type == STMT_CREATE || st.type == STMT_INSERT || st.type == STMT_DELETE ||
         st.type == STMT_UPDATE)) {
        for (int i = 0; i < db->num_tables; i++) {
            bufpool_flush_all(db->tables[i].bp);
            if (db->tables[i].has_index) {
                bufpool_flush_all(db->tables[i].index.bp);
            }
        }
    }
    return rc;
}
