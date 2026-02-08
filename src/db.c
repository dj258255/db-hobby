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
 * tname은 이 테이블의 "실효 이름"(별칭이 있으면 별칭). qtbl이 있고 tname과
 * 다르면 이 테이블 소속이 아니다. */
static const Value *cell_in(const CreateStmt *s, const char *tname, const char *qtbl,
                            const char *col, const Value *row) {
    if (qtbl[0] && strcmp(qtbl, tname) != 0) {
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

/* --- 단일 테이블 WHERE (tname = 실효 테이블 이름) --- */
static int cond_matches(const CreateStmt *schema, const char *tname, const Condition *cond,
                        const Value *row) {
    return cond_eval(cell_in(schema, tname, cond->tbl, cond->col, row), cond);
}

static int group_matches(const CreateStmt *schema, const char *tname, const AndGroup *g,
                         const Value *row) {
    for (int i = 0; i < g->count; i++) {
        if (!cond_matches(schema, tname, &g->conds[i], row)) {
            return 0;
        }
    }
    return 1;
}

/* WHERE 절(DNF): 어느 한 AND 묶음이라도 참이면 참. count==0 이면 항상 참. */
static int where_matches(const CreateStmt *schema, const char *tname, const Where *w,
                         const Value *row) {
    if (w->count == 0) {
        return 1;
    }
    for (int i = 0; i < w->count; i++) {
        if (group_matches(schema, tname, &w->groups[i], row)) {
            return 1;
        }
    }
    return 0;
}

/* --- 체인 조인된 N개 행에 대한 WHERE: 체인 순서대로 컬럼을 찾는다.
 *     tname[t] = t번째 체인 테이블의 실효 이름(별칭 있으면 별칭). --- */
static const Value *cell_chain(Table **tabs, const char **tname, Value rows[][SQL_MAX_COLS],
                               int ntabs, const char *qtbl, const char *col) {
    for (int t = 0; t < ntabs; t++) {
        const Value *v = cell_in(&tabs[t]->schema, tname[t], qtbl, col, rows[t]);
        if (v) {
            return v;
        }
    }
    return NULL;
}

static int where_matches_chain(Table **tabs, const char **tname, Value rows[][SQL_MAX_COLS],
                               int ntabs, const Where *w) {
    if (w->count == 0) {
        return 1;
    }
    for (int gi = 0; gi < w->count; gi++) {
        const AndGroup *g = &w->groups[gi];
        int all = 1;
        for (int i = 0; i < g->count; i++) {
            const Condition *c = &g->conds[i];
            if (!cond_eval(cell_chain(tabs, tname, rows, ntabs, c->tbl, c->col), c)) {
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
    const char *tname;
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
    if (!where_matches(ctx->schema, ctx->tname, ctx->where, row)) {
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
    const char *tname;
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
    if (!where_matches(m->schema, m->tname, m->where, row)) {
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

static int exec_select_sorted(Table *t, const char *tname, const SelectStmt *sel, FILE *out) {
    int ncols = t->schema.num_columns;
    Value *rows = malloc((size_t)SELECT_MAX_ROWS * ncols * sizeof(Value));
    if (!rows) {
        fprintf(out, "ERROR: 메모리 부족\n");
        return -1;
    }
    MatCtx m = {.schema = &t->schema,
                .tname = tname,
                .where = &sel->where,
                .rows = rows,
                .ncols = ncols,
                .cap = SELECT_MAX_ROWS,
                .count = 0};
    heap_scan(&t->heap, mat_visit, &m);

    if (sel->order_col[0] != '\0') {
        int ci = -1;
        if (sel->order_tbl[0] == '\0' || strcmp(sel->order_tbl, tname) == 0) {
            for (int i = 0; i < ncols; i++) {
                if (strcmp(t->schema.columns[i].name, sel->order_col) == 0) {
                    ci = i;
                }
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

/* ------------- 실행기: 투영 / 집계 / GROUP BY (단일 테이블) -------------
 * SELECT * 가 아닌 목록을 처리한다. 행을 모은 뒤(materialize):
 *   - GROUP BY 있음/집계 있음 -> 그룹 키로 정렬해 연속 구간(run)마다 집계 (정렬 기반
 *     집계, PostgreSQL의 GroupAggregate)
 *   - 둘 다 없음 -> 단순 투영(각 행에서 고른 컬럼만 출력)
 */

static int value_less(const Value *a, const Value *b) {
    if (a->type == VAL_INT) {
        return a->int_val < b->int_val;
    }
    return strcmp(a->text_val, b->text_val) < 0;
}

static const char *agg_name(AggFunc a) {
    switch (a) {
        case AGG_COUNT: return "COUNT";
        case AGG_SUM: return "SUM";
        case AGG_MIN: return "MIN";
        case AGG_MAX: return "MAX";
        case AGG_AVG: return "AVG";
        default: return "";
    }
}

/* rows의 [s,e) 구간(ncols 폭)에 대해 한 SELECT 항목의 값을 출력한다.
 * ci는 그 항목의 컬럼 위치(COUNT(*)는 무시). */
static void print_item_value(FILE *out, const SelectItem *it, int ci, const Value *rows,
                             int ncols, int s, int e) {
    if (it->agg == AGG_NONE) { /* 투영/그룹 키: 구간 첫 행의 값(대표값) */
        print_value(out, &rows[(size_t)s * ncols + ci]);
        return;
    }
    if (it->agg == AGG_COUNT) {
        fprintf(out, "%d", e - s);
        return;
    }
    if (e == s) { /* 빈 구간 */
        fprintf(out, "0");
        return;
    }
    long sum = 0;
    Value best = rows[(size_t)s * ncols + ci];
    for (int r = s; r < e; r++) {
        const Value *cell = &rows[(size_t)r * ncols + ci];
        if (it->agg == AGG_SUM || it->agg == AGG_AVG) {
            sum += cell->int_val;
        } else if (it->agg == AGG_MIN) {
            if (value_less(cell, &best)) best = *cell;
        } else if (it->agg == AGG_MAX) {
            if (value_less(&best, cell)) best = *cell;
        }
    }
    if (it->agg == AGG_SUM) {
        fprintf(out, "%ld", sum);
    } else if (it->agg == AGG_AVG) {
        fprintf(out, "%g", (double)sum / (e - s));
    } else { /* MIN / MAX */
        print_value(out, &best);
    }
}

static int exec_select_project(Table *t, const char *tname, const SelectStmt *sel, FILE *out) {
    int ncols = t->schema.num_columns;

    /* 각 항목의 컬럼 위치를 해소하고 타입을 검증한다 */
    int item_ci[SQL_MAX_COLS];
    for (int k = 0; k < sel->num_items; k++) {
        const SelectItem *it = &sel->items[k];
        if (it->agg == AGG_COUNT && it->star) {
            item_ci[k] = -1;
            continue;
        }
        int ci = -1;
        for (int i = 0; i < ncols; i++) {
            if (strcmp(t->schema.columns[i].name, it->col) == 0) ci = i;
        }
        if (ci < 0) {
            fprintf(out, "ERROR: 그런 컬럼이 없습니다 (%s)\n", it->col);
            return -1;
        }
        if ((it->agg == AGG_SUM || it->agg == AGG_AVG) && t->schema.columns[ci].type != COL_INT) {
            fprintf(out, "ERROR: %s 는 INT 컬럼에만 쓸 수 있습니다 (%s)\n", agg_name(it->agg),
                    it->col);
            return -1;
        }
        item_ci[k] = ci;
    }

    int grouped = (sel->group_col[0] != '\0');
    int gci = -1;
    if (grouped) {
        for (int i = 0; i < ncols; i++) {
            if (strcmp(t->schema.columns[i].name, sel->group_col) == 0) gci = i;
        }
        if (gci < 0) {
            fprintf(out, "ERROR: GROUP BY 컬럼이 없습니다 (%s)\n", sel->group_col);
            return -1;
        }
    }
    if (sel->order_col[0] != '\0' && (grouped || sel->has_aggregate)) {
        fprintf(out, "ERROR: GROUP BY/집계와 ORDER BY는 아직 함께 못 씁니다\n");
        return -1;
    }

    /* 헤더 */
    for (int k = 0; k < sel->num_items; k++) {
        if (k) fprintf(out, " | ");
        const SelectItem *it = &sel->items[k];
        if (it->agg == AGG_NONE) {
            fprintf(out, "%s", it->col);
        } else if (it->star) {
            fprintf(out, "%s(*)", agg_name(it->agg));
        } else {
            fprintf(out, "%s(%s)", agg_name(it->agg), it->col);
        }
    }
    fprintf(out, "\n");

    /* WHERE에 맞는 행을 모은다 */
    Value *rows = malloc((size_t)SELECT_MAX_ROWS * ncols * sizeof(Value));
    if (!rows) {
        fprintf(out, "ERROR: 메모리 부족\n");
        return -1;
    }
    MatCtx m = {.schema = &t->schema,
                .tname = tname,
                .where = &sel->where,
                .rows = rows,
                .ncols = ncols,
                .cap = SELECT_MAX_ROWS,
                .count = 0};
    heap_scan(&t->heap, mat_visit, &m);
    int n = m.count;

    /* 순수 투영(그룹/집계 없음): 각 행을 한 그룹으로 본다. ORDER BY면 먼저 정렬. */
    if (!grouped && !sel->has_aggregate) {
        if (sel->order_col[0] != '\0') {
            int oc = -1;
            if (sel->order_tbl[0] == '\0' || strcmp(sel->order_tbl, tname) == 0) {
                for (int i = 0; i < ncols; i++) {
                    if (strcmp(t->schema.columns[i].name, sel->order_col) == 0) oc = i;
                }
            }
            if (oc < 0) {
                fprintf(out, "ERROR: ORDER BY 컬럼이 없습니다 (%s)\n", sel->order_col);
                free(rows);
                return -1;
            }
            g_sort_ci = oc;
            qsort(rows, n, (size_t)ncols * sizeof(Value), row_cmp);
            if (sel->order_desc) {
                Value tmp[SQL_MAX_COLS];
                for (int i = 0, j = n - 1; i < j; i++, j--) {
                    memcpy(tmp, rows + (size_t)i * ncols, (size_t)ncols * sizeof(Value));
                    memcpy(rows + (size_t)i * ncols, rows + (size_t)j * ncols,
                           (size_t)ncols * sizeof(Value));
                    memcpy(rows + (size_t)j * ncols, tmp, (size_t)ncols * sizeof(Value));
                }
            }
        }
        int printed = 0;
        for (int r = 0; r < n; r++) {
            if (sel->limit >= 0 && printed >= sel->limit) break;
            for (int k = 0; k < sel->num_items; k++) {
                if (k) fprintf(out, " | ");
                print_item_value(out, &sel->items[k], item_ci[k], rows, ncols, r, r + 1);
            }
            fprintf(out, "\n");
            printed++;
        }
        free(rows);
        fprintf(out, "(%d행)\n", printed);
        return 0;
    }

    /* 그룹/집계: 그룹 키로 정렬한 뒤 연속 구간마다 한 줄씩 집계 출력 */
    int printed = 0;
    if (grouped) {
        g_sort_ci = gci;
        qsort(rows, n, (size_t)ncols * sizeof(Value), row_cmp);
        int s = 0;
        while (s < n) {
            int e = s + 1;
            while (e < n &&
                   values_equal(&rows[(size_t)s * ncols + gci], &rows[(size_t)e * ncols + gci])) {
                e++;
            }
            if (sel->limit < 0 || printed < sel->limit) {
                for (int k = 0; k < sel->num_items; k++) {
                    if (k) fprintf(out, " | ");
                    print_item_value(out, &sel->items[k], item_ci[k], rows, ncols, s, e);
                }
                fprintf(out, "\n");
                printed++;
            }
            s = e;
        }
    } else {
        /* 집계만(GROUP BY 없음): 전체가 한 그룹 -> 한 줄 */
        for (int k = 0; k < sel->num_items; k++) {
            if (k) fprintf(out, " | ");
            print_item_value(out, &sel->items[k], item_ci[k], rows, ncols, 0, n);
        }
        fprintf(out, "\n");
        printed = 1;
    }
    free(rows);
    fprintf(out, "(%d행)\n", printed);
    return 0;
}

/* ------------- 해시 조인용 해시 테이블 -------------
 * 한 테이블을 조인 컬럼 값으로 색인한다(키 -> 그 키를 가진 행들의 사슬).
 * 빌드 한 번 뒤 O(1) 탐사 — 인덱스가 없을 때 중첩 루프 대신 쓴다.
 */
#define HJOIN_BUCKETS 1024

typedef struct HNode {
    struct HNode *next;
    Value key;
    Value row[]; /* 그 테이블 한 행 (ncols개) — 가변 길이 멤버 */
} HNode;

typedef struct {
    HNode *buckets[HJOIN_BUCKETS];
    int ncols;
} HashTab;

static unsigned val_hash(const Value *v) {
    if (v->type == VAL_INT) {
        return (unsigned)((uint64_t)v->int_val * 2654435761u);
    }
    unsigned h = 2166136261u; /* FNV-1a */
    for (const char *p = v->text_val; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    return h;
}

static void hash_insert(HashTab *ht, const Value *key, const Value *row) {
    HNode *n = malloc(sizeof(HNode) + (size_t)ht->ncols * sizeof(Value));
    if (!n) {
        return;
    }
    n->key = *key;
    memcpy(n->row, row, (size_t)ht->ncols * sizeof(Value));
    unsigned b = val_hash(key) % HJOIN_BUCKETS;
    n->next = ht->buckets[b];
    ht->buckets[b] = n;
}

static HNode *hash_bucket(HashTab *ht, const Value *key) {
    return ht->buckets[val_hash(key) % HJOIN_BUCKETS];
}

static void hash_free(HashTab *ht) {
    if (!ht) {
        return;
    }
    for (int b = 0; b < HJOIN_BUCKETS; b++) {
        HNode *n = ht->buckets[b];
        while (n) {
            HNode *nx = n->next;
            free(n);
            n = nx;
        }
    }
    free(ht);
}

/* 한 테이블을 col_idx 컬럼으로 해시 빌드한다 */
typedef struct {
    HashTab *ht;
    const CreateStmt *schema;
    int col_idx;
} HBuildCtx;

static int hbuild_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)rid;
    (void)len;
    HBuildCtx *c = ctx_;
    Value row[SQL_MAX_COLS];
    decode_row(c->schema, rec, row);
    hash_insert(c->ht, &row[c->col_idx], row);
    return 0;
}

static HashTab *hash_build(Table *t, int col_idx) {
    HashTab *ht = calloc(1, sizeof(HashTab));
    if (!ht) {
        return NULL;
    }
    ht->ncols = t->schema.num_columns;
    HBuildCtx c = {ht, &t->schema, col_idx};
    heap_scan(&t->heap, hbuild_visit, &c);
    return ht;
}

/* ------------- 실행기: JOIN (N-way 재귀 중첩 루프 조인) -------------
 * 테이블 체인 [T0(FROM), T1, T2, ...] 을 레벨별로 재귀한다. 레벨 k에서 Tk를
 * 스캔하며 행을 rows[k]에 담고, k번째 ON 조건(앞서 묶인 테이블을 참조)이 맞으면
 * 레벨 k+1로 내려간다. 모든 테이블이 묶이면 WHERE를 걸고 출력(또는 정렬용 수집).
 * 레벨마다 조인 방법을 따로 고른다(옵티마이저가 하는 일):
 *   INDEX  ON이 Tk의 PK를 앞 테이블과 맞추고 인덱스가 있으면 점 조회(인덱스 NLJ)
 *   HASH   그 외 ON이 Tk를 앞 테이블과 맞추면 Tk를 해시 빌드해 O(1) 탐사(해시 조인)
 *   SCAN   둘 다 아니면 전체 스캔(중첩 루프)
 */
#define MJOIN_MAX_TABS (1 + SQL_MAX_JOINS)
enum { JM_SCAN, JM_INDEX, JM_HASH };

typedef struct {
    Database *db;
    const SelectStmt *sel;
    Table *tabs[MJOIN_MAX_TABS];
    const char *tname[MJOIN_MAX_TABS]; /* 체인 테이블의 실효 이름(별칭 있으면 별칭) */
    int ntabs;
    /* 각 조인 레벨 k(1..ntabs-1)의 ON 양변을 (체인 테이블 idx, 컬럼 idx)로 해소 */
    int on_at[MJOIN_MAX_TABS], on_ai[MJOIN_MAX_TABS];
    int on_bt[MJOIN_MAX_TABS], on_bi[MJOIN_MAX_TABS];
    /* 레벨별 조인 방법과 부속 정보 */
    int method[MJOIN_MAX_TABS];                 /* JM_SCAN / JM_INDEX / JM_HASH */
    int key_t[MJOIN_MAX_TABS], key_i[MJOIN_MAX_TABS]; /* probe 키 출처(앞 테이블) */
    int hcol[MJOIN_MAX_TABS];                   /* HASH: Tk의 조인 컬럼 위치 */
    HashTab *hash[MJOIN_MAX_TABS];              /* HASH: Tk를 색인한 해시 테이블 */
    int off[MJOIN_MAX_TABS]; /* 결합 행에서 각 테이블 컬럼의 시작 위치 */
    int comb_ncols;
    Value rows[MJOIN_MAX_TABS][SQL_MAX_COLS]; /* 레벨별 현재 행 */
    /* ORDER BY면 출력 대신 결합 행을 모은다 */
    int materialize;
    Value *matbuf;
    int matcap;
    int matcount;
    FILE *out;
    int count;
} MJoinCtx;

/* [qtbl.]col 을 체인의 (테이블 idx, 컬럼 idx)로 해소한다. qtbl 없으면 체인 순서로
 * 첫 매치. 0 성공, -1 실패. */
static int resolve_chain_ref(Table **tabs, const char **tname, int ntabs, const char *qtbl,
                             const char *col, int *ti, int *ci) {
    for (int t = 0; t < ntabs; t++) {
        if (qtbl[0] && strcmp(qtbl, tname[t]) != 0) {
            continue;
        }
        for (int i = 0; i < tabs[t]->schema.num_columns; i++) {
            if (strcmp(tabs[t]->schema.columns[i].name, col) == 0) {
                *ti = t;
                *ci = i;
                return 0;
            }
        }
    }
    return -1;
}

/* 모든 테이블이 묶였을 때: WHERE 적용 후 결합 행을 출력하거나 수집한다.
 * LIMIT에 도달했으면 1을 돌려 위쪽 루프들을 멈추게 한다. */
static int mjoin_emit(MJoinCtx *m) {
    if (!where_matches_chain(m->tabs, m->tname, m->rows, m->ntabs, &m->sel->where)) {
        return 0;
    }
    if (m->materialize) {
        if (m->matcount < m->matcap) {
            Value *dst = m->matbuf + (size_t)m->matcount * m->comb_ncols;
            for (int t = 0; t < m->ntabs; t++) {
                int nc = m->tabs[t]->schema.num_columns;
                memcpy(dst + m->off[t], m->rows[t], (size_t)nc * sizeof(Value));
            }
            m->matcount++;
        }
        return 0;
    }
    int first = 1;
    for (int t = 0; t < m->ntabs; t++) {
        for (int i = 0; i < m->tabs[t]->schema.num_columns; i++) {
            if (!first) {
                fprintf(m->out, " | ");
            }
            first = 0;
            print_value(m->out, &m->rows[t][i]);
        }
    }
    fprintf(m->out, "\n");
    m->count++;
    return (m->sel->limit >= 0 && m->count >= m->sel->limit);
}

static int mjoin_descend(MJoinCtx *m, int level);

typedef struct {
    MJoinCtx *m;
    int level;
} MJoinLevel;

static int mjoin_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)rid;
    (void)len;
    MJoinLevel *lv = ctx_;
    MJoinCtx *m = lv->m;
    int level = lv->level;
    decode_row(&m->tabs[level]->schema, rec, m->rows[level]);
    if (level >= 1) {
        /* 이 레벨의 ON 등식을 확인한다 */
        const Value *a = &m->rows[m->on_at[level]][m->on_ai[level]];
        const Value *b = &m->rows[m->on_bt[level]][m->on_bi[level]];
        if (!values_equal(a, b)) {
            return 0;
        }
    }
    return mjoin_descend(m, level + 1);
}

static int mjoin_descend(MJoinCtx *m, int level) {
    if (level == m->ntabs) {
        return mjoin_emit(m);
    }
    if (level >= 1 && m->method[level] == JM_INDEX) {
        /* 인덱스 NLJ: 앞 테이블의 키로 Tk의 PK 인덱스를 점 조회 */
        const Value *k = &m->rows[m->key_t[level]][m->key_i[level]];
        if (k->type != VAL_INT) {
            return 0;
        }
        bval_t encoded;
        if (btree_search(&m->tabs[level]->index, k->int_val, &encoded) != 0) {
            return 0;
        }
        uint8_t recbuf[PAGE_SIZE];
        uint16_t len2;
        if (heap_get(&m->tabs[level]->heap, rid_decode(encoded), recbuf, &len2) != 0) {
            return 0;
        }
        decode_row(&m->tabs[level]->schema, recbuf, m->rows[level]);
        return mjoin_descend(m, level + 1);
    }
    if (level >= 1 && m->method[level] == JM_HASH) {
        /* 해시 조인: 앞 테이블의 키로 Tk 해시를 탐사. 같은 키의 행마다 내려간다. */
        const Value *k = &m->rows[m->key_t[level]][m->key_i[level]];
        for (HNode *n = hash_bucket(m->hash[level], k); n; n = n->next) {
            if (!values_equal(&n->key, k)) {
                continue; /* 버킷 충돌: 키가 진짜 같은 것만 */
            }
            memcpy(m->rows[level], n->row, (size_t)m->tabs[level]->schema.num_columns *
                                               sizeof(Value));
            int r = mjoin_descend(m, level + 1);
            if (r) {
                return r;
            }
        }
        return 0;
    }
    MJoinLevel lv = {m, level};
    return heap_scan(&m->tabs[level]->heap, mjoin_visit, &lv);
}

static int exec_select_join(Database *db, const SelectStmt *sel, FILE *out) {
    MJoinCtx m = {.db = db, .sel = sel, .out = out};
    m.ntabs = 1 + sel->num_joins;

    /* 체인 테이블들을 찾는다: tabs[0]=FROM, tabs[k]=k번째 JOIN 대상.
     * 실효 이름(tname)은 별칭이 있으면 별칭 — self-join은 별칭으로 두 인스턴스를 구별한다. */
    m.tabs[0] = find_table(db, sel->table);
    if (!m.tabs[0]) {
        fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", sel->table);
        return -1;
    }
    m.tname[0] = sel->alias[0] ? sel->alias : m.tabs[0]->schema.table;
    for (int k = 1; k < m.ntabs; k++) {
        const JoinClause *jc0 = &sel->joins[k - 1];
        m.tabs[k] = find_table(db, jc0->table);
        if (!m.tabs[k]) {
            fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", jc0->table);
            return -1;
        }
        m.tname[k] = jc0->alias[0] ? jc0->alias : m.tabs[k]->schema.table;
    }

    /* 각 조인 레벨의 ON을 해소하고 인덱스 NLJ 가능 여부를 정한다 */
    int used_index = 0;
    for (int k = 1; k < m.ntabs; k++) {
        const JoinClause *jc = &sel->joins[k - 1];
        if (resolve_chain_ref(m.tabs, m.tname, m.ntabs, jc->l_tbl, jc->l_col, &m.on_at[k],
                              &m.on_ai[k]) != 0 ||
            resolve_chain_ref(m.tabs, m.tname, m.ntabs, jc->r_tbl, jc->r_col, &m.on_bt[k],
                              &m.on_bi[k]) != 0) {
            fprintf(out, "ERROR: ON 절의 컬럼을 찾을 수 없습니다\n");
            return -1;
        }
        /* Tk 쪽 ON 변과 앞 테이블 쪽 ON 변을 가른다. */
        int kcol = -1;       /* Tk의 조인 컬럼 위치 */
        int pt = -1, pi = -1; /* 앞 테이블의 (idx, 컬럼) = probe 키 출처 */
        if (m.on_at[k] == k && m.on_bt[k] < k) {
            kcol = m.on_ai[k];
            pt = m.on_bt[k];
            pi = m.on_bi[k];
        } else if (m.on_bt[k] == k && m.on_at[k] < k) {
            kcol = m.on_bi[k];
            pt = m.on_at[k];
            pi = m.on_ai[k];
        }

        m.method[k] = JM_SCAN;
        if (kcol >= 0) {
            m.key_t[k] = pt;
            m.key_i[k] = pi;
            if (kcol == 0 && m.tabs[k]->has_index) {
                m.method[k] = JM_INDEX; /* Tk의 PK가 조인 키 -> 점 조회 */
            } else {
                m.method[k] = JM_HASH; /* 그 외 -> Tk를 조인 컬럼으로 해시 빌드 */
                m.hcol[k] = kcol;
            }
        }
        if (m.method[k] == JM_INDEX) {
            used_index = 1;
        }
    }

    /* 결합 행에서 각 테이블의 컬럼 시작 위치(off)와 총 컬럼 수 */
    int comb = 0;
    for (int t = 0; t < m.ntabs; t++) {
        m.off[t] = comb;
        comb += m.tabs[t]->schema.num_columns;
    }
    m.comb_ncols = comb;

    /* 헤더: 모든 테이블 컬럼을 table.col 로 한정해 출력 */
    {
        int first = 1;
        for (int t = 0; t < m.ntabs; t++) {
            for (int i = 0; i < m.tabs[t]->schema.num_columns; i++) {
                if (!first) {
                    fprintf(out, " | ");
                }
                first = 0;
                fprintf(out, "%s.%s", m.tname[t], m.tabs[t]->schema.columns[i].name);
            }
        }
        fprintf(out, "\n");
    }

    /* 해시 조인 레벨은 Tk를 조인 컬럼으로 미리 해시 빌드한다(한 번). */
    int used_hash = 0;
    for (int k = 1; k < m.ntabs; k++) {
        if (m.method[k] == JM_HASH) {
            m.hash[k] = hash_build(m.tabs[k], m.hcol[k]);
            if (!m.hash[k]) {
                fprintf(out, "ERROR: 해시 빌드 실패(메모리 부족)\n");
                for (int j = 1; j < k; j++) hash_free(m.hash[j]);
                return -1;
            }
            used_hash = 1;
        }
    }

    db->used_index = used_index;
    const char *note = "";
    if (used_index && used_hash) note = ", 인덱스+해시 조인";
    else if (used_index) note = ", 인덱스 조인";
    else if (used_hash) note = ", 해시 조인";

    if (sel->order_col[0] == '\0') {
        /* ORDER BY 없음: 결합 행을 바로 출력하는 스트리밍 조인 */
        mjoin_descend(&m, 0);
        for (int k = 1; k < m.ntabs; k++) hash_free(m.hash[k]);
        fprintf(out, "(%d행%s)\n", m.count, note);
        return 0;
    }

    /* ORDER BY: 결합 행을 모아 정렬한 뒤 LIMIT만큼 출력한다(조인 위의 Sort 노드). */
    int oti, oci;
    if (resolve_chain_ref(m.tabs, m.tname, m.ntabs, sel->order_tbl, sel->order_col, &oti, &oci) !=
        0) {
        fprintf(out, "ERROR: ORDER BY 컬럼이 없습니다 (%s)\n", sel->order_col);
        for (int k = 1; k < m.ntabs; k++) hash_free(m.hash[k]);
        return -1;
    }
    int ocomb = m.off[oti] + oci; /* 결합 행에서의 위치 */

    m.materialize = 1;
    m.matcap = SELECT_MAX_ROWS;
    m.matbuf = malloc((size_t)SELECT_MAX_ROWS * comb * sizeof(Value));
    if (!m.matbuf) {
        fprintf(out, "ERROR: 메모리 부족\n");
        for (int k = 1; k < m.ntabs; k++) hash_free(m.hash[k]);
        return -1;
    }
    mjoin_descend(&m, 0);
    for (int k = 1; k < m.ntabs; k++) hash_free(m.hash[k]);

    g_sort_ci = ocomb;
    qsort(m.matbuf, m.matcount, (size_t)comb * sizeof(Value), row_cmp);
    if (sel->order_desc) {
        Value tmp[MJOIN_MAX_TABS * SQL_MAX_COLS];
        for (int i = 0, k = m.matcount - 1; i < k; i++, k--) {
            memcpy(tmp, m.matbuf + (size_t)i * comb, (size_t)comb * sizeof(Value));
            memcpy(m.matbuf + (size_t)i * comb, m.matbuf + (size_t)k * comb,
                   (size_t)comb * sizeof(Value));
            memcpy(m.matbuf + (size_t)k * comb, tmp, (size_t)comb * sizeof(Value));
        }
    }

    int printed = 0;
    for (int i = 0; i < m.matcount; i++) {
        if (sel->limit >= 0 && printed >= sel->limit) {
            break;
        }
        Value *row = m.matbuf + (size_t)i * comb;
        for (int c = 0; c < comb; c++) {
            if (c) {
                fprintf(out, " | ");
            }
            print_value(out, &row[c]);
        }
        fprintf(out, "\n");
        printed++;
    }
    free(m.matbuf);
    fprintf(out, "(%d행%s)\n", printed, note);
    return 0;
}

static int exec_select(Database *db, const SelectStmt *sel, FILE *out) {
    /* 투영/집계(SELECT * 가 아님)는 단일 테이블만 지원한다. */
    if (!sel->select_star && sel->num_joins > 0) {
        fprintf(out, "ERROR: 투영/집계는 아직 단일 테이블만 됩니다\n");
        return -1;
    }
    if (sel->num_joins > 0) {
        return exec_select_join(db, sel, out);
    }

    Table *t = find_table(db, sel->table);
    if (!t) {
        fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", sel->table);
        return -1;
    }
    const char *tname = sel->alias[0] ? sel->alias : t->schema.table; /* 실효 이름 */

    if (!sel->select_star) {
        db->used_index = 0;
        return exec_select_project(t, tname, sel, out);
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
        return exec_select_sorted(t, tname, sel, out);
    }

    int count = 0;

    /* WHERE가 "PK(첫 컬럼) 정수 비교" 단일 조건이면 인덱스를 쓴다.
     * (OR 묶음 하나, 그 안에 조건 하나일 때만.) */
    const Condition *c0 = (sel->where.count == 1 && sel->where.groups[0].count == 1)
                              ? &sel->where.groups[0].conds[0]
                              : NULL;
    int pk_cond = c0 && t->has_index &&
                  (c0->tbl[0] == '\0' || strcmp(c0->tbl, tname) == 0) &&
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
        SelectCtx ctx = {&t->schema, tname, &sel->where, out, 0};
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
    /* DELETE/UPDATE는 별칭이 없으니 실효 이름 = 테이블명 */
    if (where_matches(c->schema, c->schema->table, c->where, row) && c->count < DML_MAX_ROWS) {
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
