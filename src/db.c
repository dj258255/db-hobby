#include "db.h"

#include <stdint.h>
#include <string.h>

/* ───────────── tuple codec: 값 ↔ 바이트 ─────────────
 * 한 행을 스키마 순서대로 인코딩한다.
 *   INT  : 4바이트 (int32)
 *   TEXT : 2바이트 길이 + 바이트열
 */

static int encode_row(const CreateStmt *schema, const Value *vals, int nvals,
                      uint8_t *buf, uint16_t *out_len) {
    if (nvals != schema->num_columns) {
        return -1; /* 값 개수가 컬럼 수와 다름 */
    }
    uint16_t off = 0;
    for (int i = 0; i < schema->num_columns; i++) {
        const Value *v = &vals[i];
        if (schema->columns[i].type == COL_INT) {
            if (v->type != VAL_INT) {
                return -1; /* 타입 불일치 */
            }
            int32_t x = (int32_t)v->int_val;
            memcpy(buf + off, &x, 4);
            off += 4;
        } else { /* COL_TEXT */
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

/* ───────────── 실행기 ───────────── */

static int exec_create(Database *db, const CreateStmt *c, FILE *out) {
    if (db->has_table) {
        fprintf(out, "ERROR: 이미 테이블 '%s' 가 있습니다\n", db->schema.table);
        return -1;
    }
    db->schema = *c;
    db->has_table = 1;
    fprintf(out, "테이블 '%s' 생성됨 (컬럼 %d개)\n", c->table, c->num_columns);
    return 0;
}

static int exec_insert(Database *db, const InsertStmt *in, FILE *out) {
    if (!db->has_table || strcmp(in->table, db->schema.table) != 0) {
        fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", in->table);
        return -1;
    }
    uint8_t buf[PAGE_SIZE];
    uint16_t len;
    if (encode_row(&db->schema, in->values, in->num_values, buf, &len) != 0) {
        fprintf(out, "ERROR: 값의 개수나 타입이 스키마와 맞지 않습니다\n");
        return -1;
    }
    RID rid;
    if (heap_insert(&db->heap, buf, len, &rid) != 0) {
        fprintf(out, "ERROR: 삽입 실패 (행이 너무 큼?)\n");
        return -1;
    }
    fprintf(out, "1개 행 삽입됨\n");
    return 0;
}

typedef struct {
    const CreateStmt *schema;
    const SelectStmt *sel;
    FILE *out;
    int count;
} SelectCtx;

/* WHERE 절을 만족하는 행인가 */
static int match_where(const CreateStmt *schema, const SelectStmt *sel, const Value *row) {
    if (!sel->has_where) {
        return 1;
    }
    int ci = -1;
    for (int i = 0; i < schema->num_columns; i++) {
        if (strcmp(schema->columns[i].name, sel->where_col) == 0) {
            ci = i;
        }
    }
    if (ci < 0) {
        return 0; /* 없는 컬럼 → 매치 없음 */
    }
    const Value *cell = &row[ci];
    const Value *wv = &sel->where_val;
    if (cell->type == VAL_INT && wv->type == VAL_INT) {
        return cell->int_val == wv->int_val;
    }
    if (cell->type == VAL_TEXT && wv->type == VAL_TEXT) {
        return strcmp(cell->text_val, wv->text_val) == 0;
    }
    return 0;
}

static int select_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)rid;
    (void)len;
    SelectCtx *ctx = ctx_;
    Value row[SQL_MAX_COLS];
    decode_row(ctx->schema, rec, row);
    if (!match_where(ctx->schema, ctx->sel, row)) {
        return 0;
    }
    for (int i = 0; i < ctx->schema->num_columns; i++) {
        if (i) {
            fprintf(ctx->out, " | ");
        }
        if (row[i].type == VAL_INT) {
            fprintf(ctx->out, "%ld", row[i].int_val);
        } else {
            fprintf(ctx->out, "%s", row[i].text_val);
        }
    }
    fprintf(ctx->out, "\n");
    ctx->count++;
    return 0;
}

static int exec_select(Database *db, const SelectStmt *sel, FILE *out) {
    if (!db->has_table || strcmp(sel->table, db->schema.table) != 0) {
        fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", sel->table);
        return -1;
    }
    /* 헤더: 컬럼 이름 */
    for (int i = 0; i < db->schema.num_columns; i++) {
        if (i) {
            fprintf(out, " | ");
        }
        fprintf(out, "%s", db->schema.columns[i].name);
    }
    fprintf(out, "\n");

    SelectCtx ctx = {&db->schema, sel, out, 0};
    heap_scan(&db->heap, select_visit, &ctx);
    fprintf(out, "(%d행)\n", ctx.count);
    return 0;
}

/* ───────────── 공개 API ───────────── */

int db_open(Database *db, const char *path) {
    if (pager_open(&db->pager, path) != 0) {
        return -1;
    }
    db->bp = bufpool_create(&db->pager, 16);
    if (!db->bp) {
        pager_close(&db->pager);
        return -1;
    }
    heap_init(&db->heap, db->bp, &db->pager);
    db->has_table = 0;
    return 0;
}

void db_close(Database *db) {
    if (db->bp) {
        bufpool_flush_all(db->bp);
        bufpool_destroy(db->bp);
    }
    pager_close(&db->pager);
}

int db_exec(Database *db, const char *sql, FILE *out) {
    Statement st;
    char err[128];
    if (sql_parse(sql, &st, err, sizeof(err)) != 0) {
        fprintf(out, "ERROR: %s\n", err);
        return -1;
    }
    switch (st.type) {
        case STMT_CREATE: return exec_create(db, &st.create, out);
        case STMT_INSERT: return exec_insert(db, &st.insert, out);
        case STMT_SELECT: return exec_select(db, &st.select, out);
        default: return -1;
    }
}
