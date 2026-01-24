#include "db.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* ───────────── tuple codec: 값 ↔ 바이트 ─────────────
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

/* RID ↔ int64 (인덱스 값으로 저장하려고). slot < 65536 이므로 안전. */
static int64_t rid_encode(RID r) {
    return (int64_t)r.page_id * 65536 + r.slot;
}
static RID rid_decode(int64_t v) {
    RID r;
    r.page_id = (page_id_t)(v / 65536);
    r.slot = (uint16_t)(v % 65536);
    return r;
}

static void print_row(FILE *out, const CreateStmt *schema, const Value *row) {
    for (int i = 0; i < schema->num_columns; i++) {
        if (i) {
            fprintf(out, " | ");
        }
        if (row[i].type == VAL_INT) {
            fprintf(out, "%ld", row[i].int_val);
        } else {
            fprintf(out, "%s", row[i].text_val);
        }
    }
    fprintf(out, "\n");
}

/* ───────────── 실행기 ───────────── */

static int exec_create(Database *db, const CreateStmt *c, FILE *out) {
    if (db->has_table) {
        fprintf(out, "ERROR: 이미 테이블 '%s' 가 있습니다\n", db->schema.table);
        return -1;
    }
    db->schema = *c;
    db->has_table = 1;

    /* 첫 컬럼이 INT면 그 컬럼을 PK로 보고 B+Tree 인덱스를 만든다 */
    if (c->num_columns > 0 && c->columns[0].type == COL_INT) {
        char idxpath[600];
        snprintf(idxpath, sizeof(idxpath), "%s.idx", db->path);
        if (btree_open(&db->index, idxpath) == 0) {
            db->has_index = 1;
        }
    }
    fprintf(out, "테이블 '%s' 생성됨 (컬럼 %d개)\n", c->table, c->num_columns);
    if (db->has_index) {
        fprintf(out, "  (인덱스: %s 컬럼)\n", db->schema.columns[0].name);
    }
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
    /* 인덱스에 (PK 값 → RID) 등록 */
    if (db->has_index && in->values[0].type == VAL_INT) {
        btree_insert(&db->index, in->values[0].int_val, rid_encode(rid));
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
        return 0;
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
    print_row(ctx->out, ctx->schema, row);
    ctx->count++;
    return 0;
}

static int exec_select(Database *db, const SelectStmt *sel, FILE *out) {
    if (!db->has_table || strcmp(sel->table, db->schema.table) != 0) {
        fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", sel->table);
        return -1;
    }
    /* 헤더 */
    for (int i = 0; i < db->schema.num_columns; i++) {
        if (i) {
            fprintf(out, " | ");
        }
        fprintf(out, "%s", db->schema.columns[i].name);
    }
    fprintf(out, "\n");

    db->used_index = 0;
    int count = 0;

    /* WHERE가 인덱스된 PK(첫 컬럼)에 대한 정수 비교면 → O(log n) 인덱스 조회 */
    if (sel->has_where && db->has_index &&
        strcmp(sel->where_col, db->schema.columns[0].name) == 0 &&
        sel->where_val.type == VAL_INT) {
        db->used_index = 1;
        bval_t encoded;
        if (btree_search(&db->index, sel->where_val.int_val, &encoded) == 0) {
            uint8_t recbuf[PAGE_SIZE];
            uint16_t len;
            if (heap_get(&db->heap, rid_decode(encoded), recbuf, &len) == 0) {
                Value row[SQL_MAX_COLS];
                decode_row(&db->schema, recbuf, row);
                print_row(out, &db->schema, row);
                count++;
            }
        }
    } else {
        /* 그 외 → 풀 스캔 */
        SelectCtx ctx = {&db->schema, sel, out, 0};
        heap_scan(&db->heap, select_visit, &ctx);
        count = ctx.count;
    }

    fprintf(out, "(%d행%s)\n", count, db->used_index ? ", 인덱스 사용" : "");
    return 0;
}

/* ───────────── 트랜잭션 ─────────────
 * no-steal + 커밋 시 force. 트랜잭션 중 바뀐 페이지는 버퍼 풀 메모리에만 두고,
 * COMMIT이면 flush+fsync(내구), ROLLBACK이면 dirty 프레임을 버리고 할당분을 잘라
 * 디스크 원본으로 되돌린다. 힙과 인덱스 둘 다 처리해 일관성을 지킨다.
 */

static int exec_begin(Database *db, FILE *out) {
    if (db->in_txn) {
        fprintf(out, "ERROR: 이미 트랜잭션 중입니다\n");
        return -1;
    }
    db->in_txn = 1;
    bufpool_set_no_steal(db->bp, 1);
    db->txn_data_pages = db->pager.num_pages;
    if (db->has_index) {
        bufpool_set_no_steal(db->index.bp, 1);
        db->txn_index_pages = db->index.pager.num_pages;
    }
    fprintf(out, "트랜잭션 시작\n");
    return 0;
}

static int exec_commit(Database *db, FILE *out) {
    if (!db->in_txn) {
        fprintf(out, "ERROR: 트랜잭션이 없습니다\n");
        return -1;
    }
    bufpool_flush_all(db->bp);
    fsync(db->pager.fd);
    if (db->has_index) {
        bufpool_flush_all(db->index.bp);
        fsync(db->index.pager.fd);
        bufpool_set_no_steal(db->index.bp, 0);
    }
    bufpool_set_no_steal(db->bp, 0);
    db->in_txn = 0;
    fprintf(out, "커밋됨\n");
    return 0;
}

static int exec_rollback(Database *db, FILE *out) {
    if (!db->in_txn) {
        fprintf(out, "ERROR: 트랜잭션이 없습니다\n");
        return -1;
    }
    bufpool_discard_dirty(db->bp);
    pager_truncate(&db->pager, db->txn_data_pages);
    bufpool_set_no_steal(db->bp, 0);
    if (db->has_index) {
        bufpool_discard_dirty(db->index.bp);
        pager_truncate(&db->index.pager, db->txn_index_pages);
        btree_reload_root(&db->index); /* 루트가 분할로 바뀌었을 수 있으니 다시 읽는다 */
        bufpool_set_no_steal(db->index.bp, 0);
    }
    db->in_txn = 0;
    fprintf(out, "롤백됨\n");
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
    db->has_index = 0;
    db->used_index = 0;
    db->in_txn = 0;
    snprintf(db->path, sizeof(db->path), "%s", path);
    return 0;
}

void db_close(Database *db) {
    if (db->has_index) {
        btree_close(&db->index);
        db->has_index = 0;
    }
    if (db->bp) {
        bufpool_flush_all(db->bp);
        bufpool_destroy(db->bp);
        db->bp = NULL;
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
    int rc;
    switch (st.type) {
        case STMT_CREATE: rc = exec_create(db, &st.create, out); break;
        case STMT_INSERT: rc = exec_insert(db, &st.insert, out); break;
        case STMT_SELECT: rc = exec_select(db, &st.select, out); break;
        case STMT_BEGIN: rc = exec_begin(db, out); break;
        case STMT_COMMIT: rc = exec_commit(db, out); break;
        case STMT_ROLLBACK: rc = exec_rollback(db, out); break;
        default: rc = -1;
    }
    /* autocommit: 트랜잭션 밖에서 성공한 변경은 즉시 디스크에 반영하고 클린으로 만든다.
     * 그래야 이후 ROLLBACK이 이 변경까지 되돌리지 않는다. */
    if (rc == 0 && !db->in_txn && (st.type == STMT_CREATE || st.type == STMT_INSERT)) {
        bufpool_flush_all(db->bp);
        if (db->has_index) {
            bufpool_flush_all(db->index.bp);
        }
    }
    return rc;
}
