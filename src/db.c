#include "db.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------- tuple codec: 값 <-> 바이트 -------------
 *   INT  : 4바이트 (int32)
 *   TEXT : 2바이트 길이 + 바이트열
 */

/* 행 맨 앞 MVCC 헤더: int32 xmin(만든 트랜잭션) + int32 xmax(지운 트랜잭션, 0=안 지움).
 * 그 뒤에 null 비트맵, 그 뒤에 값들. PostgreSQL 튜플 헤더의 xmin/xmax와 같은 발상. */
#define MVCC_HDR 8

int32_t db_rec_xmin(const void *rec) {
    int32_t x;
    memcpy(&x, rec, 4);
    return x;
}
int32_t db_rec_xmax(const void *rec) {
    int32_t x;
    memcpy(&x, (const char *)rec + 4, 4);
    return x;
}

static int encode_row(const CreateStmt *schema, const Value *vals, int nvals, int32_t xmin,
                      int32_t xmax, uint8_t *buf, uint16_t *out_len) {
    if (nvals != schema->num_columns) {
        return -1;
    }
    memcpy(buf, &xmin, 4); /* MVCC 헤더 */
    memcpy(buf + 4, &xmax, 4);
    /* null 비트맵: 컬럼당 1비트(1이면 NULL). 헤더 뒤에 둔다. */
    int nbits = (schema->num_columns + 7) / 8;
    memset(buf + MVCC_HDR, 0, (size_t)nbits);
    uint16_t off = (uint16_t)(MVCC_HDR + nbits);
    for (int i = 0; i < schema->num_columns; i++) {
        const Value *v = &vals[i];
        if (v->type == VAL_NULL) {
            buf[MVCC_HDR + i / 8] |= (uint8_t)(1 << (i % 8)); /* NULL 표시 */
            continue;
        }
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
    int nbits = (schema->num_columns + 7) / 8;
    uint16_t off = (uint16_t)(MVCC_HDR + nbits); /* MVCC 헤더 + null 비트맵 건너뜀 */
    for (int i = 0; i < schema->num_columns; i++) {
        if (rec[MVCC_HDR + i / 8] & (uint8_t)(1 << (i % 8))) { /* null 비트 */
            out[i].type = VAL_NULL;
            continue;
        }
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
    if (v->type == VAL_NULL) {
        fprintf(out, "NULL");
    } else if (v->type == VAL_INT) {
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
    int32_t nt = db->next_txn; /* MVCC: 다음 세션이 옛 행을 커밋으로 보게 next_txn 영속화 */
    fwrite(&nt, sizeof(nt), 1, f);
    for (int i = 0; i < db->num_tables; i++) {
        fwrite(&db->tables[i].schema, sizeof(CreateStmt), 1, f);
        /* 보조 인덱스 정의(개수 + 각 {이름, 컬럼 위치})도 같이 직렬화 */
        int32_t ns = db->tables[i].num_sec;
        fwrite(&ns, sizeof(ns), 1, f);
        for (int k = 0; k < db->tables[i].num_sec; k++) {
            fwrite(db->tables[i].sec[k].name, SQL_NAME_LEN, 1, f);
            int32_t col = db->tables[i].sec[k].col;
            fwrite(&col, sizeof(col), 1, f);
        }
    }
    fclose(f);
}

/* 테이블 파일(.tbl, .idx, .wal)을 열어 Heap/B+Tree를 준비한다. schema는 미리 채워둘 것.
 * wal_open이 .wal 로그를 읽어 크래시 복구(redo/discard)를 먼저 수행한다. */
static int table_open_files(Table *t, const char *dbpath) {
    char p[700], wp[710];
    snprintf(p, sizeof(p), "%s.%s.tbl", dbpath, t->schema.table);
    snprintf(wp, sizeof(wp), "%s.%s.wal", dbpath, t->schema.table);
    if (wal_open(&t->wal, p, wp) != 0) { /* 데이터 페이저(wal.data)를 열고 복구 */
        return -1;
    }
    /* 데이터 페이지를 캐시할 버퍼 풀. WAL을 거쳐 커밋하려면 dirty 페이지가 새지 않게
     * stage될 때까지 메모리에 머물러야 하므로, stage 상한(WAL_MAX_STAGED)만큼 프레임을 둔다. */
    t->bp = bufpool_create(&t->wal.data, WAL_MAX_STAGED);
    if (!t->bp) {
        wal_close(&t->wal);
        return -1;
    }
    heap_init(&t->heap, t->bp, &t->wal.data, 0); /* 테이블 파일은 순수 힙: page 0부터 */
    t->has_index = 0;
    if (t->schema.num_columns > 0 && t->schema.columns[0].type == COL_INT) {
        char ip[700];
        snprintf(ip, sizeof(ip), "%s.%s.idx", dbpath, t->schema.table);
        if (btree_open(&t->index, ip) == 0) {
            t->has_index = 1;
        }
    }
    /* 보조 인덱스들(재오픈 시 카탈로그가 채운 num_sec/sec[]대로). 새 테이블은 num_sec=0. */
    for (int k = 0; k < t->num_sec; k++) {
        char sp[780];
        snprintf(sp, sizeof(sp), "%s.%s.%s.idx", dbpath, t->schema.table, t->sec[k].name);
        if (btree_open(&t->sec[k].tree, sp) != 0) {
            return -1;
        }
    }
    return 0;
}

static void table_close_files(Table *t) {
    for (int k = 0; k < t->num_sec; k++) {
        btree_close(&t->sec[k].tree);
    }
    t->num_sec = 0;
    if (t->has_index) {
        btree_close(&t->index);
        t->has_index = 0;
    }
    if (t->bp) {
        bufpool_flush_all(t->bp);
        bufpool_destroy(t->bp);
        t->bp = NULL;
    }
    wal_close(&t->wal);
}

static Table *find_table(Database *db, const char *name) {
    for (int i = 0; i < db->num_tables; i++) {
        if (strcmp(db->tables[i].schema.table, name) == 0) {
            return &db->tables[i];
        }
    }
    return NULL;
}

/* ------------- MVCC 가시성 ------------- */

/* txn이 "커밋된 것으로 보이나" — 이전 세션 id(committed_below 미만)는 전부 커밋,
 * 이번 세션 id는 TxnLog가 COMMITTED일 때만. (no-steal+WAL이라 디스크엔 커밋분만 있음.) */
static int txn_committed_view(Database *db, int txn) {
    if (txn <= 0) {
        return 0;
    }
    if (txn < db->committed_below) {
        return 1;
    }
    return txnlog_status(&db->txnlog, txn) == TXN_COMMITTED;
}

/* 행 버전(xmin,xmax)이 my_txn 입장에서 보이나? 자기 트랜잭션의 미커밋 쓰기도 본다. */
static int row_visible(Database *db, int32_t xmin, int32_t xmax, int my_txn) {
    if (!(xmin == my_txn || txn_committed_view(db, xmin))) {
        return 0; /* 생성자가 내 것도 아니고 커밋도 아님 -> 없는 행 */
    }
    if (xmax != 0 && (xmax == my_txn || txn_committed_view(db, xmax))) {
        return 0; /* 내가/커밋된 누가 지움 -> 안 보임 */
    }
    return 1;
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
        case CMP_IS_NULL:     /* cond_eval에서 따로 처리 */
        case CMP_IS_NOT_NULL:
        case CMP_LIKE:        /* LIKE류는 like_match로 따로 처리 */
        case CMP_NOT_LIKE: return 0;
    }
    return 0;
}

/* SQL LIKE 패턴 매칭. '%' = 임의 길이(0+) 문자열, '_' = 정확히 한 글자.
 * 그 외 문자는 그대로 일치해야 한다(대소문자 구분 — db-hobby TEXT 비교가 strcmp라 일관).
 *
 * 백트래킹 two-pointer 방식: '%'를 만나면 그 위치(star)와 그때의 입력 위치(ss)를
 * 기억해 두고 일단 '%'가 0글자를 먹었다고 보고 전진한다. 뒤에서 막히면 star로
 * 되돌아가 '%'가 한 글자 더 먹은 셈 치고(ss++) 다시 시도한다. 재귀 없이 O(n*m).
 * (ESCAPE 절은 학습 범위 밖이라 '\%' 같은 이스케이프는 지원하지 않는다.) */
static int like_match(const char *s, const char *pat) {
    const char *star = NULL, *ss = NULL;
    while (*s) {
        if (*pat == '%') {
            star = pat++;   /* '%' 위치 기억, 일단 0글자 먹은 걸로 보고 패턴만 전진 */
            ss = s;
        } else if (*pat == '_' || *pat == *s) {
            pat++;
            s++;
        } else if (star) {
            pat = star + 1; /* 막혔다 -> 마지막 '%'가 한 글자 더 먹은 셈 치고 재시도 */
            s = ++ss;
        } else {
            return 0;
        }
    }
    while (*pat == '%') {
        pat++; /* 남은 패턴이 전부 '%'면 빈 문자열에 매칭되니 건너뛴다 */
    }
    return *pat == '\0';
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
    /* col IN (SELECT ...) — 미리 계산된 값 집합(in_set)에 멤버십 검사 */
    if (cond->in_sub) {
        if (!cell || cell->type == VAL_NULL) {
            return 0; /* NULL은 IN/NOT IN/스칼라 비교 모두 거짓(unknown) */
        }
        if (cond->scalar_sub) { /* col <op> (SELECT ...) — 한 값과 비교 */
            if (cond->in_set_n < 1) {
                return 0; /* 빈 서브쿼리 -> NULL -> 거짓 */
            }
            const Value *v = &cond->in_set[0];
            if (cell->type == VAL_INT && v->type == VAL_INT) {
                long sign = (cell->int_val < v->int_val) ? -1 : (cell->int_val > v->int_val);
                return cmp_apply(cond->op, sign);
            }
            if (cell->type == VAL_TEXT && v->type == VAL_TEXT) {
                return cmp_apply(cond->op, (long)strcmp(cell->text_val, v->text_val));
            }
            return 0;
        }
        int member = 0;
        for (int i = 0; i < cond->in_set_n; i++) {
            const Value *v = &cond->in_set[i];
            if (v->type != cell->type) continue;
            if (cell->type == VAL_INT ? cell->int_val == v->int_val
                                      : strcmp(cell->text_val, v->text_val) == 0) {
                member = 1;
                break;
            }
        }
        return cond->in_negate ? !member : member;
    }
    /* IS [NOT] NULL — NULL을 검사하는 유일한 방법(=는 NULL에 항상 거짓) */
    if (cond->op == CMP_IS_NULL) {
        return cell && cell->type == VAL_NULL;
    }
    if (cond->op == CMP_IS_NOT_NULL) {
        return cell && cell->type != VAL_NULL;
    }
    if (!cell) {
        return 0;
    }
    /* col [NOT] LIKE '패턴' — TEXT끼리만, 와일드카드 매칭 */
    if (cond->op == CMP_LIKE || cond->op == CMP_NOT_LIKE) {
        if (cell->type != VAL_TEXT || cond->val.type != VAL_TEXT) {
            return 0;
        }
        int m = like_match(cell->text_val, cond->val.text_val);
        return cond->op == CMP_NOT_LIKE ? !m : m;
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
    if (a->type == VAL_NULL || b->type == VAL_NULL) {
        return 0; /* NULL은 무엇과도(NULL과도) 같지 않다 */
    }
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
    char tp[700], ip[700], wp[710], iwp[720];
    snprintf(tp, sizeof(tp), "%s.%s.tbl", db->path, c->table);
    snprintf(ip, sizeof(ip), "%s.%s.idx", db->path, c->table);
    snprintf(wp, sizeof(wp), "%s.%s.wal", db->path, c->table);
    snprintf(iwp, sizeof(iwp), "%s.%s.idx.wal", db->path, c->table);
    unlink(tp);
    unlink(ip);
    unlink(wp);
    unlink(iwp);

    Table *t = &db->tables[db->num_tables];
    t->schema = *c;
    t->num_sec = 0; /* 새 테이블은 보조 인덱스 없음 */
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

/* CREATE INDEX: 기존 행을 훑어 보조 인덱스를 채우는 콜백 */
typedef struct {
    SecIndex *si;
    int col;
    const CreateStmt *schema;
    int count;
} SecBuildCtx;

static int secidx_build_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)len;
    SecBuildCtx *c = ctx_;
    Value row[SQL_MAX_COLS];
    decode_row(c->schema, (const uint8_t *)rec, row);
    if (row[c->col].type == VAL_INT) { /* NULL/비INT는 색인 안 함 */
        btree_insert_dup(&c->si->tree, row[c->col].int_val, rid_encode(rid));
        c->count++;
    }
    return 0;
}

/* CREATE INDEX <name> ON <table>(<col>) — INT 컬럼에 비유니크 보조 인덱스를 만든다.
 * 새 파일을 열어 기존 행으로 채우고, 직접 flush한 뒤 카탈로그에 영속화한다(DDL이라 즉시 반영). */
static int exec_create_index(Database *db, const CreateIndexStmt *ci, FILE *out) {
    Table *t = find_table(db, ci->table);
    if (!t) {
        fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", ci->table);
        return -1;
    }
    int col = -1;
    for (int i = 0; i < t->schema.num_columns; i++) {
        if (strcmp(t->schema.columns[i].name, ci->column) == 0) {
            col = i;
            break;
        }
    }
    if (col < 0) {
        fprintf(out, "ERROR: '%s' 컬럼이 없습니다\n", ci->column);
        return -1;
    }
    if (t->schema.columns[col].type != COL_INT) {
        fprintf(out, "ERROR: 보조 인덱스는 INT 컬럼에만 걸 수 있습니다 (%s)\n", ci->column);
        return -1;
    }
    if (col == 0 && t->has_index) {
        fprintf(out, "ERROR: 첫 컬럼은 이미 PK 인덱스가 있습니다\n");
        return -1;
    }
    if (t->num_sec >= DB_MAX_SEC_IDX) {
        fprintf(out, "ERROR: 보조 인덱스가 너무 많습니다 (최대 %d개)\n", DB_MAX_SEC_IDX);
        return -1;
    }
    for (int k = 0; k < t->num_sec; k++) {
        if (strcmp(t->sec[k].name, ci->name) == 0) {
            fprintf(out, "ERROR: 이미 인덱스 '%s' 가 있습니다\n", ci->name);
            return -1;
        }
    }

    SecIndex *si = &t->sec[t->num_sec];
    snprintf(si->name, SQL_NAME_LEN, "%s", ci->name);
    si->col = col;
    char sp[780], swp[800];
    snprintf(sp, sizeof(sp), "%s.%s.%s.idx", db->path, t->schema.table, ci->name);
    snprintf(swp, sizeof(swp), "%s.wal", sp);
    unlink(sp); /* 옛 파일 정리 */
    unlink(swp);
    if (btree_open(&si->tree, sp) != 0) {
        fprintf(out, "ERROR: 인덱스 파일을 열 수 없습니다\n");
        return -1;
    }
    /* 기존 행을 훑어 (컬럼값 -> RID) 등록 */
    SecBuildCtx bc = {si, col, &t->schema, 0};
    heap_scan(&t->heap, secidx_build_visit, &bc);
    bufpool_flush_all(si->tree.bp); /* WAL 없이 직접 영속화(한 번 만드는 DDL) */
    si->txn_pages = si->tree.wal.data.num_pages; /* 혹시 트랜잭션 중이면 롤백이 이 크기로(=no-op) */
    t->num_sec++;
    catalog_write(db);
    fprintf(out, "인덱스 '%s' 생성됨 (%s.%s, 행 %d개 색인)\n", ci->name, ci->table, ci->column,
            bc.count);
    return 0;
}

static int exec_insert(Database *db, const InsertStmt *in, FILE *out) {
    Table *t = find_table(db, in->table);
    if (!t) {
        fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", in->table);
        return -1;
    }
    /* NOT NULL 검증: 명시적 NOT NULL 컬럼, 그리고 첫 컬럼(PK=인덱스 키)은 NULL 금지.
     * 진짜 DB의 PK NOT NULL + 컬럼 제약과 같다. */
    for (int i = 0; i < in->num_values && i < t->schema.num_columns; i++) {
        int is_pk = (i == 0 && t->has_index);
        if ((is_pk || t->schema.columns[i].not_null) && in->values[i].type == VAL_NULL) {
            fprintf(out, "ERROR: '%s' 컬럼은 NULL일 수 없습니다%s\n",
                    t->schema.columns[i].name, is_pk ? " (기본 키)" : "");
            return -1;
        }
    }
    uint8_t buf[PAGE_SIZE];
    uint16_t len;
    if (encode_row(&t->schema, in->values, in->num_values, db->cur_txn, 0, buf, &len) != 0) {
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
    for (int k = 0; k < t->num_sec; k++) { /* 보조 인덱스도 (컬럼값 -> RID) 등록 */
        int col = t->sec[k].col;
        if (in->values[col].type == VAL_INT) { /* NULL은 색인 안 함 */
            btree_insert_dup(&t->sec[k].tree, in->values[col].int_val, rid_encode(rid));
        }
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
    Database *db; /* MVCC 가시성 판정용 */
    int my_txn;
} SelectCtx;

static int select_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)rid;
    (void)len;
    SelectCtx *ctx = ctx_;
    if (!row_visible(ctx->db, db_rec_xmin(rec), db_rec_xmax(rec), ctx->my_txn)) {
        return 0; /* 내 스냅샷에 안 보이는 버전(미커밋/아보트 생성, 커밋된 삭제)은 건너뜀 */
    }
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

/* qsort 비교기는 컨텍스트를 못 받으니(이식성 위해 qsort_r 회피) 정렬 키 목록을
 * 파일 정적으로 둔다. 단일 스레드 학습용이라 안전. 키마다 ASC/DESC를 적용한다. */
static int g_sort_keys[SQL_MAX_ORDER]; /* 행 안의 컬럼 위치들 */
static int g_sort_desc[SQL_MAX_ORDER];
static int g_sort_n;

/* 두 값을 비교해 sign(-1/0/1). NULL은 가장 크게 친다(ASC 정렬 시 끝 = PostgreSQL의 NULLS LAST). */
static int value_cmp(const Value *x, const Value *y) {
    if (x->type == VAL_NULL || y->type == VAL_NULL) {
        return (x->type == VAL_NULL) - (y->type == VAL_NULL);
    }
    if (x->type == VAL_INT) {
        return (x->int_val < y->int_val) ? -1 : (x->int_val > y->int_val);
    }
    int c = strcmp(x->text_val, y->text_val);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

static int row_cmp(const void *a, const void *b) {
    const Value *x = a;
    const Value *y = b;
    for (int k = 0; k < g_sort_n; k++) {
        int c = value_cmp(&x[g_sort_keys[k]], &y[g_sort_keys[k]]);
        if (c) return g_sort_desc[k] ? -c : c;
    }
    return 0;
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

    if (sel->num_order > 0) {
        g_sort_n = sel->num_order;
        for (int k = 0; k < sel->num_order; k++) {
            const OrderKey *ok = &sel->order_keys[k];
            int ci = -1;
            if (ok->pos > 0) {
                if (ok->pos > ncols) {
                    fprintf(out, "ERROR: ORDER BY 위치가 범위를 벗어났습니다\n");
                    free(rows);
                    return -1;
                }
                ci = ok->pos - 1;
            } else if (ok->tbl[0] == '\0' || strcmp(ok->tbl, tname) == 0) {
                for (int i = 0; i < ncols; i++) {
                    if (strcmp(t->schema.columns[i].name, ok->col) == 0) ci = i;
                }
            }
            if (ci < 0) {
                fprintf(out, "ERROR: ORDER BY 컬럼이 없습니다 (%s)\n", ok->col);
                free(rows);
                return -1;
            }
            g_sort_keys[k] = ci;
            g_sort_desc[k] = ok->desc;
        }
        qsort(rows, m.count, (size_t)ncols * sizeof(Value), row_cmp);
    }

    int count = 0;
    for (int i = 0; i < m.count; i++) {
        if (i < sel->offset) continue; /* OFFSET: 앞 N행 건너뜀 */
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
    if (a->type == VAL_NULL || b->type == VAL_NULL) {
        return a->type == VAL_NULL && b->type != VAL_NULL; /* NULL < 비NULL */
    }
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

/* 집계 출력 셀: NULL이거나 숫자(num)이거나 문자열(text).
 * COUNT/SUM/AVG와 INT MIN/MAX는 num, TEXT MIN/MAX와 투영 TEXT는 text. */
typedef struct {
    int is_null;
    int is_text;
    double num;
    char text[SQL_TEXT_LEN];
} OutCell;

/* rows의 [s,e) 구간(ncols 폭)에 대해 한 SELECT 항목의 값을 셀로 계산한다.
 * ci는 그 항목의 컬럼 위치(COUNT(*)는 무시). */
static OutCell compute_cell(const SelectItem *it, int ci, const Value *rows, int ncols, int s,
                            int e) {
    OutCell c = {0, 0, 0.0, {0}};
    if (it->agg == AGG_NONE) { /* 투영/그룹 키: 구간 첫 행의 대표값 */
        const Value *v = &rows[(size_t)s * ncols + ci];
        if (v->type == VAL_NULL) {
            c.is_null = 1;
        } else if (v->type == VAL_TEXT) {
            c.is_text = 1;
            snprintf(c.text, sizeof(c.text), "%s", v->text_val);
        } else {
            c.num = (double)v->int_val;
        }
        return c;
    }
    if (it->agg == AGG_COUNT && it->star) {
        c.num = e - s; /* COUNT(*): NULL 포함 전체 행 수 */
        return c;
    }
    /* COUNT(col)/SUM/AVG/MIN/MAX 는 NULL을 건너뛴다 */
    if (it->agg == AGG_COUNT) {
        int cnt = 0;
        for (int r = s; r < e; r++) {
            if (rows[(size_t)r * ncols + ci].type != VAL_NULL) cnt++;
        }
        c.num = cnt;
        return c;
    }
    if (it->agg == AGG_SUM || it->agg == AGG_AVG) {
        long sum = 0;
        int cnt = 0;
        for (int r = s; r < e; r++) {
            const Value *cell = &rows[(size_t)r * ncols + ci];
            if (cell->type == VAL_NULL) continue;
            sum += cell->int_val;
            cnt++;
        }
        if (cnt == 0) {
            c.is_null = 1; /* 전부 NULL(또는 빈 구간) -> NULL */
            return c;
        }
        c.num = (it->agg == AGG_SUM) ? (double)sum : (double)sum / cnt;
        return c;
    }
    /* MIN / MAX (INT 또는 TEXT), NULL 무시 */
    const Value *best = NULL;
    for (int r = s; r < e; r++) {
        const Value *cell = &rows[(size_t)r * ncols + ci];
        if (cell->type == VAL_NULL) continue;
        if (!best || (it->agg == AGG_MIN ? value_less(cell, best) : value_less(best, cell))) {
            best = cell;
        }
    }
    if (!best) {
        c.is_null = 1;
        return c;
    }
    if (best->type == VAL_TEXT) {
        c.is_text = 1;
        snprintf(c.text, sizeof(c.text), "%s", best->text_val);
    } else {
        c.num = (double)best->int_val;
    }
    return c;
}

static void print_cell(FILE *out, const SelectItem *it, const OutCell *c) {
    if (c->is_null) {
        fprintf(out, "NULL");
    } else if (c->is_text) {
        fprintf(out, "%s", c->text);
    } else if (it->agg == AGG_AVG) {
        fprintf(out, "%g", c->num);
    } else {
        fprintf(out, "%ld", (long)c->num); /* COUNT/SUM/MIN/MAX(int)/투영 INT */
    }
}

/* 출력 행을 정렬 키 목록으로 비교(파일 정적, 단일 스레드라 안전). NULL은 가장 작게. */
static int g_out_keys[SQL_MAX_ORDER];
static int g_out_desc[SQL_MAX_ORDER];
static int g_out_n;
static int outcell_cmp(const OutCell *x, const OutCell *y) {
    if (x->is_null || y->is_null) return x->is_null - y->is_null;
    if (x->is_text) {
        int c = strcmp(x->text, y->text);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    return (x->num < y->num) ? -1 : (x->num > y->num);
}
static int outrow_cmp(const void *a, const void *b) {
    const OutCell *x = a;
    const OutCell *y = b;
    for (int k = 0; k < g_out_n; k++) {
        int c = outcell_cmp(&x[g_out_keys[k]], &y[g_out_keys[k]]);
        if (c) return g_out_desc[k] ? -c : c;
    }
    return 0;
}

/* DISTINCT: 출력 행 전체(모든 컬럼)로 비교/동등 판정. */
static int g_out_ncols;
static int outrow_cmp_all(const void *a, const void *b) {
    const OutCell *x = a, *y = b;
    for (int i = 0; i < g_out_ncols; i++) {
        if (x[i].is_null || y[i].is_null) {
            int d = x[i].is_null - y[i].is_null;
            if (d) return d;
            continue;
        }
        if (x[i].is_text) {
            int d = strcmp(x[i].text, y[i].text);
            if (d) return d;
            continue;
        }
        if (x[i].num < y[i].num) return -1;
        if (x[i].num > y[i].num) return 1;
    }
    return 0;
}
static int outrows_equal(const OutCell *a, const OutCell *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i].is_null != b[i].is_null) return 0;
        if (a[i].is_null) continue;
        if (a[i].is_text != b[i].is_text) return 0;
        if (a[i].is_text) {
            if (strcmp(a[i].text, b[i].text) != 0) return 0;
        } else if (a[i].num != b[i].num) {
            return 0;
        }
    }
    return 1;
}

/* 모인 출력 행들을 ORDER BY(출력 컬럼 목록)로 정렬하고 LIMIT만큼 찍는다. outbuf는 호출자 소유. */
static int emit_out_rows(const SelectStmt *sel, OutCell *outbuf, int outcount, FILE *out) {
    if (sel->num_order > 0) {
        g_out_n = sel->num_order;
        for (int k = 0; k < sel->num_order; k++) {
            const OrderKey *ok = &sel->order_keys[k];
            int oc = -1;
            if (ok->pos > 0) {
                if (ok->pos > sel->num_items) {
                    fprintf(out, "ERROR: ORDER BY 위치가 범위를 벗어났습니다\n");
                    return -1;
                }
                oc = ok->pos - 1;
            } else {
                for (int j = 0; j < sel->num_items; j++) {
                    if (sel->items[j].agg == AGG_NONE && strcmp(sel->items[j].col, ok->col) == 0) {
                        oc = j;
                    }
                }
                if (oc < 0) {
                    fprintf(out, "ERROR: ORDER BY는 출력 컬럼(또는 위치)이어야 합니다 (%s)\n",
                            ok->col);
                    return -1;
                }
            }
            g_out_keys[k] = oc;
            g_out_desc[k] = ok->desc;
        }
        qsort(outbuf, outcount, (size_t)sel->num_items * sizeof(OutCell), outrow_cmp);
    }
    int printed = 0;
    for (int i = 0; i < outcount; i++) {
        if (i < sel->offset) continue; /* OFFSET */
        if (sel->limit >= 0 && printed >= sel->limit) break;
        OutCell *orow = outbuf + (size_t)i * sel->num_items;
        for (int k = 0; k < sel->num_items; k++) {
            if (k) fprintf(out, " | ");
            print_cell(out, &sel->items[k], &orow[k]);
        }
        fprintf(out, "\n");
        printed++;
    }
    fprintf(out, "(%d행)\n", printed);
    return 0;
}

/* 결합 행의 한 컬럼: 실효 테이블 이름 + 컬럼명 + INT 여부. 단일 테이블/조인 공통. */
typedef struct {
    const char *tbl;
    const char *col;
    int is_int;
} ColRef;

/* [qtbl.]col 을 cols에서 찾는다(없으면 -1). qtbl 없으면 이름으로 첫 매치. */
static int find_col(const ColRef *cols, int ncols, const char *qtbl, const char *col) {
    for (int i = 0; i < ncols; i++) {
        if (qtbl[0] && strcmp(qtbl, cols[i].tbl) != 0) continue;
        if (strcmp(cols[i].col, col) == 0) return i;
    }
    return -1;
}

static void print_item_label(FILE *out, const SelectItem *it) {
    if (it->agg == AGG_NONE) {
        if (it->tbl[0]) fprintf(out, "%s.%s", it->tbl, it->col);
        else fprintf(out, "%s", it->col);
    } else if (it->star) {
        fprintf(out, "%s(*)", agg_name(it->agg));
    } else if (it->tbl[0]) {
        fprintf(out, "%s(%s.%s)", agg_name(it->agg), it->tbl, it->col);
    } else {
        fprintf(out, "%s(%s)", agg_name(it->agg), it->col);
    }
}

/* 투영/집계/GROUP BY/HAVING/ORDER BY/LIMIT를 "이미 모은 행 집합"에 적용해 출력한다.
 * rows: n행, 각 행 ncols 폭. cols[i]: i번째 컬럼의 (실효테이블, 이름, INT여부).
 * 단일 테이블이면 한 테이블 컬럼들, 조인이면 결합 컬럼들을 넘기면 같은 코드로 동작한다.
 * rows는 호출자가 소유(여기서 free하지 않음). */
static int aggregate_rowset(const SelectStmt *sel, Value *rows, int n, int ncols,
                            const ColRef *cols, FILE *out) {
    /* 항목 컬럼 해소 + 타입 검증 */
    int item_ci[SQL_MAX_COLS];
    for (int k = 0; k < sel->num_items; k++) {
        const SelectItem *it = &sel->items[k];
        if (it->agg == AGG_COUNT && it->star) {
            item_ci[k] = -1;
            continue;
        }
        int ci = find_col(cols, ncols, it->tbl, it->col);
        if (ci < 0) {
            fprintf(out, "ERROR: 그런 컬럼이 없습니다 (%s)\n", it->col);
            return -1;
        }
        if ((it->agg == AGG_SUM || it->agg == AGG_AVG) && !cols[ci].is_int) {
            fprintf(out, "ERROR: %s 는 INT 컬럼에만 쓸 수 있습니다 (%s)\n", agg_name(it->agg),
                    it->col);
            return -1;
        }
        item_ci[k] = ci;
    }

    int grouped = (sel->group_col[0] != '\0');
    int gci = -1;
    if (grouped) {
        gci = find_col(cols, ncols, sel->group_tbl, sel->group_col);
        if (gci < 0) {
            fprintf(out, "ERROR: GROUP BY 컬럼이 없습니다 (%s)\n", sel->group_col);
            return -1;
        }
    }

    int hci = -1;
    if (sel->has_having && !(sel->having_agg.agg == AGG_COUNT && sel->having_agg.star)) {
        hci = find_col(cols, ncols, sel->having_agg.tbl, sel->having_agg.col);
        if (hci < 0) {
            fprintf(out, "ERROR: HAVING 컬럼이 없습니다 (%s)\n", sel->having_agg.col);
            return -1;
        }
    }

    /* 헤더 */
    for (int k = 0; k < sel->num_items; k++) {
        if (k) fprintf(out, " | ");
        print_item_label(out, &sel->items[k]);
    }
    fprintf(out, "\n");

    /* 순수 투영(그룹/집계 없음): 각 행을 그대로 투영. */
    if (!grouped && !sel->has_aggregate) {
        /* DISTINCT: 투영 출력 행을 모아 전체 컬럼으로 정렬·중복 제거한다
         * (= 모든 출력 컬럼으로 GROUP BY 한 것과 같다). */
        if (sel->distinct) {
            OutCell *outbuf = malloc((size_t)SELECT_MAX_ROWS * sel->num_items * sizeof(OutCell));
            if (!outbuf) {
                fprintf(out, "ERROR: 메모리 부족\n");
                return -1;
            }
            int outcount = 0;
            for (int r = 0; r < n && outcount < SELECT_MAX_ROWS; r++) {
                OutCell *orow = outbuf + (size_t)outcount * sel->num_items;
                for (int k = 0; k < sel->num_items; k++) {
                    orow[k] = compute_cell(&sel->items[k], item_ci[k], rows, ncols, r, r + 1);
                }
                outcount++;
            }
            g_out_ncols = sel->num_items;
            qsort(outbuf, outcount, (size_t)sel->num_items * sizeof(OutCell), outrow_cmp_all);
            int uniq = 0; /* 연속 중복 제거(정렬돼 있으니 인접 비교로 충분) */
            for (int i = 0; i < outcount; i++) {
                OutCell *cur = outbuf + (size_t)i * sel->num_items;
                if (uniq == 0 ||
                    !outrows_equal(outbuf + (size_t)(uniq - 1) * sel->num_items, cur,
                                   sel->num_items)) {
                    if (i != uniq) {
                        memcpy(outbuf + (size_t)uniq * sel->num_items, cur,
                               (size_t)sel->num_items * sizeof(OutCell));
                    }
                    uniq++;
                }
            }
            int rc = emit_out_rows(sel, outbuf, uniq, out);
            free(outbuf);
            return rc;
        }
        if (sel->num_order > 0) {
            g_sort_n = sel->num_order;
            for (int k = 0; k < sel->num_order; k++) {
                const OrderKey *ok = &sel->order_keys[k];
                int oc;
                if (ok->pos > 0) {
                    if (ok->pos > sel->num_items) {
                        fprintf(out, "ERROR: ORDER BY 위치가 범위를 벗어났습니다\n");
                        return -1;
                    }
                    oc = item_ci[ok->pos - 1];
                } else {
                    oc = find_col(cols, ncols, ok->tbl, ok->col);
                    if (oc < 0) {
                        fprintf(out, "ERROR: ORDER BY 컬럼이 없습니다 (%s)\n", ok->col);
                        return -1;
                    }
                }
                g_sort_keys[k] = oc;
                g_sort_desc[k] = ok->desc;
            }
            qsort(rows, n, (size_t)ncols * sizeof(Value), row_cmp);
        }
        int printed = 0;
        for (int r = 0; r < n; r++) {
            if (r < sel->offset) continue; /* OFFSET */
            if (sel->limit >= 0 && printed >= sel->limit) break;
            for (int k = 0; k < sel->num_items; k++) {
                if (k) fprintf(out, " | ");
                OutCell c = compute_cell(&sel->items[k], item_ci[k], rows, ncols, r, r + 1);
                print_cell(out, &sel->items[k], &c);
            }
            fprintf(out, "\n");
            printed++;
        }
        fprintf(out, "(%d행)\n", printed);
        return 0;
    }

    /* 그룹/집계: 출력 행(그룹마다 한 줄)을 버퍼에 모은다. */
    OutCell *outbuf = malloc((size_t)SELECT_MAX_ROWS * sel->num_items * sizeof(OutCell));
    if (!outbuf) {
        fprintf(out, "ERROR: 메모리 부족\n");
        return -1;
    }
    int outcount = 0;
    if (grouped) {
        g_sort_keys[0] = gci;
        g_sort_desc[0] = 0;
        g_sort_n = 1;
        qsort(rows, n, (size_t)ncols * sizeof(Value), row_cmp);
    }
    int s = 0;
    while (s < n || (s == 0 && !grouped)) { /* 집계만이면 빈 입력에도 그룹 1개(전체) */
        int e;
        if (grouped) {
            e = s + 1;
            while (e < n &&
                   values_equal(&rows[(size_t)s * ncols + gci], &rows[(size_t)e * ncols + gci])) {
                e++;
            }
        } else {
            e = n;
        }
        int pass = 1;
        if (sel->has_having) {
            OutCell hc = compute_cell(&sel->having_agg, hci, rows, ncols, s, e);
            const Value *hv = &sel->having_val;
            if (hc.is_text) {
                pass = (hv->type == VAL_TEXT) &&
                       cmp_apply(sel->having_op, (long)strcmp(hc.text, hv->text_val));
            } else {
                pass = (hv->type == VAL_INT) &&
                       cmp_apply(sel->having_op,
                                 (hc.num < hv->int_val) ? -1 : (hc.num > hv->int_val ? 1 : 0));
            }
        }
        if (pass && outcount < SELECT_MAX_ROWS) {
            OutCell *orow = outbuf + (size_t)outcount * sel->num_items;
            for (int k = 0; k < sel->num_items; k++) {
                orow[k] = compute_cell(&sel->items[k], item_ci[k], rows, ncols, s, e);
            }
            outcount++;
        }
        if (!grouped) break;
        s = e;
    }

    /* ORDER BY(출력 컬럼) + LIMIT + 출력 */
    int rc = emit_out_rows(sel, outbuf, outcount, out);
    free(outbuf);
    return rc;
}

/* 단일 테이블 투영/집계: WHERE에 맞는 행을 모아 aggregate_rowset에 넘긴다. */
static int exec_select_project(Table *t, const char *tname, const SelectStmt *sel, FILE *out) {
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

    ColRef cols[SQL_MAX_COLS];
    for (int i = 0; i < ncols; i++) {
        cols[i].tbl = tname;
        cols[i].col = t->schema.columns[i].name;
        cols[i].is_int = (t->schema.columns[i].type == COL_INT);
    }
    int rc = aggregate_rowset(sel, rows, m.count, ncols, cols, out);
    free(rows);
    return rc;
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
    int is_left[MJOIN_MAX_TABS];                /* 레벨 k가 LEFT JOIN이면 1 */
    int matched[MJOIN_MAX_TABS];                /* 레벨 k에서 이번 외부행이 매칭됐나(LEFT 판단용) */
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

/* ------------- EXPLAIN: 실행기와 같은 결정 로직으로 쿼리 플랜을 출력 ------------- */

static const char *xop_str(CmpOp op) {
    switch (op) {
        case CMP_EQ: return "=";
        case CMP_NE: return "!=";
        case CMP_LT: return "<";
        case CMP_GT: return ">";
        case CMP_LE: return "<=";
        case CMP_GE: return ">=";
        case CMP_IS_NULL: return "IS NULL";
        case CMP_IS_NOT_NULL: return "IS NOT NULL";
        case CMP_LIKE: return "LIKE";
        case CMP_NOT_LIKE: return "NOT LIKE";
    }
    return "?";
}

static void xfmt_val(char *buf, size_t n, const Value *v) {
    if (v->type == VAL_TEXT) {
        snprintf(buf, n, "'%s'", v->text_val);
    } else if (v->type == VAL_NULL) {
        snprintf(buf, n, "NULL");
    } else {
        snprintf(buf, n, "%ld", v->int_val);
    }
}

static void xfmt_cond(char *buf, size_t n, const Condition *c) {
    char col[160];
    if (c->tbl[0]) {
        snprintf(col, sizeof col, "%s.%s", c->tbl, c->col);
    } else {
        snprintf(col, sizeof col, "%s", c->col);
    }
    if (c->op == CMP_IS_NULL || c->op == CMP_IS_NOT_NULL) {
        snprintf(buf, n, "%s %s", col, xop_str(c->op));
    } else if (c->in_sub) {
        if (c->sub && c->scalar_sub) {
            snprintf(buf, n, "%s %s (subquery)", col, xop_str(c->op));
        } else if (c->sub) {
            snprintf(buf, n, "%s %sIN (subquery)", col, c->in_negate ? "NOT " : "");
        } else {
            snprintf(buf, n, "%s %sIN (%d values)", col, c->in_negate ? "NOT " : "", c->in_set_n);
        }
    } else {
        char v[300];
        xfmt_val(v, sizeof v, &c->val);
        snprintf(buf, n, "%s %s %s", col, xop_str(c->op), v);
    }
}

static void xrender_where(char *buf, size_t n, const Where *w) {
    size_t len = 0;
    buf[0] = '\0';
    for (int gi = 0; gi < w->count; gi++) {
        if (gi && len < n) {
            len += snprintf(buf + len, n - len, " OR ");
        }
        const AndGroup *g = &w->groups[gi];
        for (int i = 0; i < g->count && len < n; i++) {
            char c[400];
            xfmt_cond(c, sizeof c, &g->conds[i]);
            len += snprintf(buf + len, n - len, "%s%s", i ? " AND " : "", c);
        }
    }
}

static void xindent(FILE *out, int ind) {
    for (int i = 0; i < ind; i++) {
        fputc(' ', out);
    }
}

/* 후처리 노드(Limit/Unique/Sort/HAVING/Aggregate)를 바깥->안 순서로 찍고,
 * 접근 노드를 찍을 들여쓰기 깊이를 반환한다. */
static int xexplain_post(FILE *out, const SelectStmt *sel) {
    int ind = 0;
    if (sel->limit >= 0 || sel->offset > 0) {
        xindent(out, ind);
        if (sel->limit >= 0 && sel->offset > 0) {
            fprintf(out, "Limit  (limit=%ld, offset=%ld)\n", sel->limit, sel->offset);
        } else if (sel->limit >= 0) {
            fprintf(out, "Limit  (limit=%ld)\n", sel->limit);
        } else {
            fprintf(out, "Offset  (offset=%ld)\n", sel->offset);
        }
        ind += 2;
    }
    if (sel->distinct) {
        xindent(out, ind);
        fprintf(out, "Unique  (DISTINCT)\n");
        ind += 2;
    }
    if (sel->num_order > 0) {
        char keys[400];
        size_t len = 0;
        keys[0] = '\0';
        for (int k = 0; k < sel->num_order; k++) {
            const OrderKey *ok = &sel->order_keys[k];
            char key[160];
            if (ok->pos > 0) {
                snprintf(key, sizeof key, "col%d", ok->pos);
            } else if (ok->tbl[0]) {
                snprintf(key, sizeof key, "%s.%s", ok->tbl, ok->col);
            } else {
                snprintf(key, sizeof key, "%s", ok->col);
            }
            if (len < sizeof keys) {
                len += snprintf(keys + len, sizeof keys - len, "%s%s %s", k ? ", " : "", key,
                                ok->desc ? "DESC" : "ASC");
            }
        }
        xindent(out, ind);
        fprintf(out, "Sort  (keys: %s)\n", keys);
        ind += 2;
    }
    if (sel->has_having) {
        const SelectItem *h = &sel->having_agg;
        char fn[160], v[200];
        if (h->star) {
            snprintf(fn, sizeof fn, "%s(*)", agg_name(h->agg));
        } else {
            snprintf(fn, sizeof fn, "%s(%s)", agg_name(h->agg), h->col);
        }
        xfmt_val(v, sizeof v, &sel->having_val);
        xindent(out, ind);
        fprintf(out, "Filter  (HAVING: %s %s %s)\n", fn, xop_str(sel->having_op), v);
        ind += 2;
    }
    if (sel->has_aggregate || sel->group_col[0]) {
        char ag[400];
        size_t len = 0;
        ag[0] = '\0';
        for (int i = 0; i < sel->num_items; i++) {
            const SelectItem *it = &sel->items[i];
            if (it->agg == AGG_NONE) {
                continue;
            }
            char a[160];
            if (it->star) {
                snprintf(a, sizeof a, "%s(*)", agg_name(it->agg));
            } else {
                snprintf(a, sizeof a, "%s(%s)", agg_name(it->agg), it->col);
            }
            if (len < sizeof ag) {
                len += snprintf(ag + len, sizeof ag - len, "%s%s", len ? ", " : "", a);
            }
        }
        xindent(out, ind);
        if (sel->group_col[0]) {
            fprintf(out, "GroupAggregate  (group: %s; aggs: %s)\n", sel->group_col, ag);
        } else {
            fprintf(out, "Aggregate  (aggs: %s)\n", ag);
        }
        ind += 2;
    }
    return ind;
}

/* 단일 테이블 플랜. exec_select의 인덱스 선택 로직과 동일한 조건으로 접근 방법을 정한다. */
/* 단일 "= 정수" 조건이 어떤 보조 인덱스의 컬럼이면 그 인덱스 번호를, 아니면 -1.
 * pk_cond(PK 경로)면 -1(PK가 우선). exec_select와 explain_single이 공유한다. */
static int sec_index_for(const Table *t, const char *tname, const Condition *c0, int pk_cond) {
    if (!c0 || c0->in_sub || pk_cond || c0->op != CMP_EQ || c0->val.type != VAL_INT) {
        return -1;
    }
    if (c0->tbl[0] && strcmp(c0->tbl, tname) != 0) {
        return -1;
    }
    for (int k = 0; k < t->num_sec; k++) {
        if (strcmp(t->schema.columns[t->sec[k].col].name, c0->col) == 0) {
            return k;
        }
    }
    return -1;
}

static void explain_single(FILE *out, Table *t, const char *tname, const SelectStmt *sel) {
    fprintf(out, "EXPLAIN\n");
    int ind = xexplain_post(out, sel);

    /* 인덱스는 SELECT * + ORDER BY/LIMIT/OFFSET 없음 경로에서만 쓰인다(exec_select와 동일). */
    int can_index = sel->select_star && sel->num_order == 0 && sel->limit < 0 && sel->offset <= 0;
    const char *pkcol = t->schema.columns[0].name;
    const Condition *c0 = (sel->where.count == 1 && sel->where.groups[0].count == 1)
                              ? &sel->where.groups[0].conds[0]
                              : NULL;
    int pk_cond = c0 && !c0->in_sub && t->has_index &&
                  (c0->tbl[0] == '\0' || strcmp(c0->tbl, tname) == 0) &&
                  strcmp(c0->col, pkcol) == 0 && c0->val.type == VAL_INT;

    xindent(out, ind);
    if (can_index && pk_cond && c0->op == CMP_EQ) {
        fprintf(out, "Index Point Lookup on %s using %s  (%s = %ld)\n", tname, pkcol, pkcol,
                c0->val.int_val);
    } else if (can_index && pk_cond &&
               (c0->op == CMP_LT || c0->op == CMP_GT || c0->op == CMP_LE || c0->op == CMP_GE)) {
        fprintf(out, "Index Range Scan on %s using %s  (%s %s %ld)\n", tname, pkcol, pkcol,
                xop_str(c0->op), c0->val.int_val);
    } else if (can_index && sec_index_for(t, tname, c0, pk_cond) >= 0) {
        int sk = sec_index_for(t, tname, c0, pk_cond);
        fprintf(out, "Index Scan using %s on %s  (%s = %ld, recheck)\n", t->sec[sk].name, tname,
                c0->col, c0->val.int_val);
    } else if (sel->where.count > 0) {
        char w[800];
        xrender_where(w, sizeof w, &sel->where);
        fprintf(out, "Seq Scan on %s  (filter: %s)\n", tname, w);
    } else {
        fprintf(out, "Seq Scan on %s\n", tname);
    }
}

/* 조인 플랜. 레벨별 method[](실행기가 고른 값)를 그대로 출력한다. */
static void explain_join(FILE *out, const SelectStmt *sel, const MJoinCtx *m) {
    fprintf(out, "EXPLAIN\n");
    int ind = xexplain_post(out, sel);
    if (sel->where.count > 0) {
        char w[800];
        xrender_where(w, sizeof w, &sel->where);
        xindent(out, ind);
        fprintf(out, "Filter  (%s)\n", w);
        ind += 2;
    }
    xindent(out, ind);
    fprintf(out, "Nested-Loop Join  (%d tables)\n", m->ntabs);
    ind += 2;
    xindent(out, ind);
    fprintf(out, "Seq Scan on %s  (outer)\n", m->tname[0]);
    for (int k = 1; k < m->ntabs; k++) {
        xindent(out, ind);
        const char *lft = m->is_left[k] ? "Left " : "";
        if (m->method[k] == JM_INDEX) {
            fprintf(out, "%sIndex Nested Loop -> %s  (inner PK = join key)\n", lft, m->tname[k]);
        } else if (m->method[k] == JM_HASH) {
            fprintf(out, "%sHash Join -> %s  (build hash on join col)\n", lft, m->tname[k]);
        } else {
            fprintf(out, "%sNested Loop -> %s  (seq scan)\n", lft, m->tname[k]);
        }
    }
}

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
        m->matched[level] = 1; /* LEFT 판단용: 이 외부행이 매칭됐다 */
    }
    return mjoin_descend(m, level + 1);
}

/* 레벨 k 테이블의 컬럼들을 NULL로 채운다(LEFT JOIN 미매칭 시). */
static void mjoin_null_fill(MJoinCtx *m, int level) {
    int nc = m->tabs[level]->schema.num_columns;
    for (int i = 0; i < nc; i++) {
        m->rows[level][i].type = VAL_NULL;
    }
}

static int mjoin_descend(MJoinCtx *m, int level) {
    if (level == m->ntabs) {
        return mjoin_emit(m);
    }
    int is_left = (level >= 1 && m->is_left[level]);
    if (level >= 1) {
        m->matched[level] = 0; /* 이번 외부행에 대한 매칭 추적 리셋 */
    }
    int r = 0;
    if (level >= 1 && m->method[level] == JM_INDEX) {
        /* 인덱스 NLJ: 앞 테이블의 키로 Tk의 PK 인덱스를 점 조회 */
        const Value *k = &m->rows[m->key_t[level]][m->key_i[level]];
        if (k->type == VAL_INT) {
            bval_t encoded;
            if (btree_search(&m->tabs[level]->index, k->int_val, &encoded) == 0) {
                uint8_t recbuf[PAGE_SIZE];
                uint16_t len2;
                if (heap_get(&m->tabs[level]->heap, rid_decode(encoded), recbuf, &len2) == 0) {
                    decode_row(&m->tabs[level]->schema, recbuf, m->rows[level]);
                    m->matched[level] = 1;
                    r = mjoin_descend(m, level + 1);
                }
            }
        }
    } else if (level >= 1 && m->method[level] == JM_HASH) {
        /* 해시 조인: 앞 테이블의 키로 Tk 해시를 탐사. 같은 키의 행마다 내려간다. */
        const Value *k = &m->rows[m->key_t[level]][m->key_i[level]];
        for (HNode *n = hash_bucket(m->hash[level], k); n; n = n->next) {
            if (!values_equal(&n->key, k)) {
                continue; /* 버킷 충돌: 키가 진짜 같은 것만 */
            }
            memcpy(m->rows[level], n->row, (size_t)m->tabs[level]->schema.num_columns *
                                               sizeof(Value));
            m->matched[level] = 1;
            r = mjoin_descend(m, level + 1);
            if (r) {
                return r;
            }
        }
    } else {
        MJoinLevel lv = {m, level};
        r = heap_scan(&m->tabs[level]->heap, mjoin_visit, &lv);
    }
    if (r) {
        return r;
    }
    /* LEFT JOIN인데 이번 외부행에 매칭이 하나도 없었다 -> 오른쪽을 NULL로 채워 보존 */
    if (is_left && !m->matched[level]) {
        mjoin_null_fill(m, level);
        return mjoin_descend(m, level + 1);
    }
    return 0;
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
        m.is_left[k] = jc0->is_left;
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

    /* EXPLAIN이면 여기까지의 결정(레벨별 method)만 출력하고 끝낸다(해시 빌드 전). */
    if (sel->explain) {
        explain_join(out, sel, &m);
        return 0;
    }

    /* 해시 조인 레벨은 Tk를 조인 컬럼으로 미리 해시 빌드한다(한 번). 양 경로 공통. */
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

    /* SELECT * 가 아니면(투영/집계): 결합 행을 전부 모아 공통 집계기로 넘긴다.
     * 조인이 만든 결합 행이 곧 집계의 입력 — 두 실행기가 한 함수로 만난다. */
    if (!sel->select_star) {
        Value *matbuf = malloc((size_t)SELECT_MAX_ROWS * comb * sizeof(Value));
        if (!matbuf) {
            fprintf(out, "ERROR: 메모리 부족\n");
            for (int k = 1; k < m.ntabs; k++) hash_free(m.hash[k]);
            return -1;
        }
        m.materialize = 1;
        m.matbuf = matbuf;
        m.matcap = SELECT_MAX_ROWS;
        mjoin_descend(&m, 0);
        for (int k = 1; k < m.ntabs; k++) hash_free(m.hash[k]);

        ColRef cols[MJOIN_MAX_TABS * SQL_MAX_COLS];
        int ci = 0;
        for (int t = 0; t < m.ntabs; t++) {
            for (int i = 0; i < m.tabs[t]->schema.num_columns; i++) {
                cols[ci].tbl = m.tname[t];
                cols[ci].col = m.tabs[t]->schema.columns[i].name;
                cols[ci].is_int = (m.tabs[t]->schema.columns[i].type == COL_INT);
                ci++;
            }
        }
        int rc = aggregate_rowset(sel, matbuf, m.matcount, comb, cols, out);
        free(matbuf);
        return rc;
    }

    /* 헤더: 모든 테이블 컬럼을 table.col 로 한정해 출력 (SELECT *) */
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

    if (sel->num_order == 0 && sel->offset == 0) {
        /* ORDER BY/OFFSET 없음: 결합 행을 바로 출력하는 스트리밍 조인 */
        mjoin_descend(&m, 0);
        for (int k = 1; k < m.ntabs; k++) hash_free(m.hash[k]);
        fprintf(out, "(%d행%s)\n", m.count, note);
        return 0;
    }

    /* ORDER BY나 OFFSET이 있으면 결합 행을 모은다(조인 위의 Sort 노드).
     * ORDER BY 각 키를 결합 행에서의 위치로 해소한다(다중 컬럼 가능). */
    g_sort_n = sel->num_order;
    for (int k = 0; k < sel->num_order; k++) {
        const OrderKey *ok = &sel->order_keys[k];
        int oti, oci;
        if (ok->pos > 0) {
            if (ok->pos > comb) {
                fprintf(out, "ERROR: ORDER BY 위치가 범위를 벗어났습니다\n");
                for (int j = 1; j < m.ntabs; j++) hash_free(m.hash[j]);
                return -1;
            }
            g_sort_keys[k] = ok->pos - 1;
        } else if (resolve_chain_ref(m.tabs, m.tname, m.ntabs, ok->tbl, ok->col, &oti, &oci) == 0) {
            g_sort_keys[k] = m.off[oti] + oci; /* 결합 행에서의 위치 */
        } else {
            fprintf(out, "ERROR: ORDER BY 컬럼이 없습니다 (%s)\n", ok->col);
            for (int j = 1; j < m.ntabs; j++) hash_free(m.hash[j]);
            return -1;
        }
        g_sort_desc[k] = ok->desc;
    }

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

    if (sel->num_order > 0) {
        qsort(m.matbuf, m.matcount, (size_t)comb * sizeof(Value), row_cmp);
    }

    int printed = 0;
    for (int i = 0; i < m.matcount; i++) {
        if (i < sel->offset) continue; /* OFFSET */
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

/* ------------- 서브쿼리 prepare: col IN (SELECT ...) -------------
 * 상관 없는(uncorrelated) 서브쿼리라 바깥 스캔 전에 한 번만 돌려 값 집합을 만든다.
 * 안쪽은 단일 테이블·단일 컬럼 투영만 지원(IN 멤버십엔 그걸로 충분).
 */
static int prepare_where(Database *db, Where *w);

typedef struct {
    const CreateStmt *schema;
    const char *tname;
    const Where *where;
    int col;
    Value *set;
    int cap;
    int n;
} SubCtx;

static int sub_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)rid;
    (void)len;
    SubCtx *c = ctx_;
    Value row[SQL_MAX_COLS];
    decode_row(c->schema, rec, row);
    if (where_matches(c->schema, c->tname, c->where, row) && c->n < c->cap) {
        c->set[c->n++] = row[c->col];
    }
    return 0;
}

static int run_subquery(Database *db, SelectStmt *sub, Value **out_set, int *out_n) {
    /* 지원 형태: SELECT <컬럼> FROM <테이블> [WHERE ...] (조인·집계·* 없음) */
    if (sub->num_joins != 0 || sub->select_star || sub->num_items != 1 ||
        sub->items[0].agg != AGG_NONE || sub->group_col[0] != '\0') {
        return -1;
    }
    Table *t = find_table(db, sub->table);
    if (!t) {
        return -1;
    }
    const char *tname = sub->alias[0] ? sub->alias : t->schema.table;
    if (prepare_where(db, &sub->where) != 0) { /* 중첩 서브쿼리 먼저 */
        return -1;
    }
    int col = -1;
    for (int i = 0; i < t->schema.num_columns; i++) {
        if (strcmp(t->schema.columns[i].name, sub->items[0].col) == 0) col = i;
    }
    if (col < 0) {
        return -1;
    }
    Value *set = malloc((size_t)SELECT_MAX_ROWS * sizeof(Value));
    if (!set) {
        return -1;
    }
    SubCtx c = {&t->schema, tname, &sub->where, col, set, SELECT_MAX_ROWS, 0};
    heap_scan(&t->heap, sub_visit, &c);
    *out_set = set;
    *out_n = c.n;
    return 0;
}

/* WHERE 안의 모든 IN-서브쿼리를 한 번씩 실행해 in_set을 채운다(아직 안 채웠으면). */
static int prepare_where(Database *db, Where *w) {
    for (int gi = 0; gi < w->count; gi++) {
        AndGroup *g = &w->groups[gi];
        for (int i = 0; i < g->count; i++) {
            Condition *c = &g->conds[i];
            if (c->in_sub && !c->in_set) {
                if (run_subquery(db, c->sub, &c->in_set, &c->in_set_n) != 0) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

/* 보조 인덱스 스캔: find_all로 받은 후보 RID를 heap_get으로 읽고 WHERE를 재검사한다.
 * 재검사가 필요한 이유 — 삭제된 행(tombstone)은 heap_get이 거르고, UPDATE로 값이 바뀌어
 * 남은 stale 인덱스 항목이나 슬롯 재사용은 cond 재평가로 거른다. */
typedef struct {
    Table *t;
    const char *tname;
    const Where *where;
    FILE *out;
    int count;
} SecScanCtx;

static int sec_scan_visit(bkey_t key, bval_t val, void *ctx_) {
    (void)key;
    SecScanCtx *s = ctx_;
    uint8_t recbuf[PAGE_SIZE];
    uint16_t len;
    if (heap_get(&s->t->heap, rid_decode(val), recbuf, &len) != 0) {
        return 0; /* 삭제된(tombstone) 행 */
    }
    Value row[SQL_MAX_COLS];
    decode_row(&s->t->schema, recbuf, row);
    if (where_matches(&s->t->schema, s->tname, s->where, row)) { /* 재검사 */
        print_row(s->out, &s->t->schema, row);
        s->count++;
    }
    return 0;
}

static int exec_select(Database *db, const SelectStmt *sel, FILE *out) {
    if (sel->num_joins > 0) {
        /* 조인: SELECT * 는 스트리밍, 투영/집계는 결합 행을 모아 처리 (둘 다 안에서 분기) */
        return exec_select_join(db, sel, out);
    }

    Table *t = find_table(db, sel->table);
    if (!t) {
        fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", sel->table);
        return -1;
    }
    const char *tname = sel->alias[0] ? sel->alias : t->schema.table; /* 실효 이름 */

    if (sel->explain) {
        explain_single(out, t, tname, sel);
        return 0;
    }

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

    /* ORDER BY/LIMIT/OFFSET이 있으면 모았다가 정렬/자르는 경로로 간다. */
    if (sel->num_order > 0 || sel->limit >= 0 || sel->offset > 0) {
        return exec_select_sorted(t, tname, sel, out);
    }

    int count = 0;

    /* WHERE가 "PK(첫 컬럼) 정수 비교" 단일 조건이면 인덱스를 쓴다.
     * (OR 묶음 하나, 그 안에 조건 하나일 때만.) */
    const Condition *c0 = (sel->where.count == 1 && sel->where.groups[0].count == 1)
                              ? &sel->where.groups[0].conds[0]
                              : NULL;
    int pk_cond = c0 && !c0->in_sub && t->has_index &&
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
    } else if (sec_index_for(t, tname, c0, pk_cond) >= 0) {
        /* 비PK 컬럼 = 값 -> 보조 인덱스 find_all + heap_get + WHERE 재검사 */
        db->used_index = 1;
        int sk = sec_index_for(t, tname, c0, pk_cond);
        SecScanCtx sc = {t, tname, &sel->where, out, 0};
        btree_find_all(&t->sec[sk].tree, c0->val.int_val, sec_scan_visit, &sc);
        count = sc.count;
    } else {
        /* 그 외(WHERE 없음/복합/비PK/TEXT 비교) -> 풀 스캔 */
        SelectCtx ctx = {&t->schema, tname, &sel->where, out, 0, db, db->cur_txn};
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
        if (encode_row(&t->schema, row, t->schema.num_columns, db->cur_txn, 0, newbuf, &newlen) !=
            0) {
            continue;
        }
        heap_delete(&t->heap, ctx.rids[i]);
        RID newrid;
        if (heap_insert(&t->heap, newbuf, newlen, &newrid) != 0) {
            continue;
        }
        /* 새 RID로 인덱스 갱신 — 안 하면 인덱스가 삭제된 옛 위치를 가리켜 행이 사라진다.
         * RID가 통째로 바뀌므로 바뀐 컬럼과 무관하게 모든 인덱스에 새 RID를 다시 넣는다. */
        if (t->has_index && row[0].type == VAL_INT) {
            btree_insert(&t->index, row[0].int_val, rid_encode(newrid));
        }
        for (int k = 0; k < t->num_sec; k++) {
            int col = t->sec[k].col;
            if (row[col].type == VAL_INT) {
                btree_insert_dup(&t->sec[k].tree, row[col].int_val, rid_encode(newrid));
            }
        }
        n++;
    }
    fprintf(out, "%d개 행 수정됨\n", n);
    return 0;
}

/* ------------- 트랜잭션 -------------
 * 데이터(.tbl)는 이제 WAL을 통해 커밋한다(write-ahead): 트랜잭션 중 바뀐 페이지는
 * no-steal로 버퍼 풀에만 두고, COMMIT이면 그 dirty 페이지들을 WAL에 stage한 뒤
 * wal_commit(로그+마커+fsync -> 데이터 적용 -> 로그 비움)으로 원자적으로 확정한다.
 * 크래시가 커밋 도중 나도, 다음 wal_open이 마커 유무로 redo/discard를 결정한다.
 * ROLLBACK은 dirty를 버리고 할당분을 잘라 되돌린다(아무것도 로그에 안 적었다).
 *  데이터(.tbl)와 인덱스(.idx) 둘 다 각자의 WAL로 커밋한다.
 *  DDL인 CREATE는 즉시 반영되며 트랜잭션에 묶이지 않는다.) */

static int wal_stage_sink(page_id_t pid, const void *data, void *ctx) {
    return wal_stage((Wal *)ctx, pid, data);
}

/* STEAL 핸들러: 버퍼 풀이 커밋 전 dirty 페이지를 축출할 때 부른다.
 * wal_steal이 undo(before-image) 로깅 + 디스크 쓰기까지 원자적으로 처리한다. */
static int wal_steal_cb(page_id_t pid, const void *data, void *ctx) {
    return wal_steal((Wal *)ctx, pid, data);
}

/* 버퍼 풀의 dirty 페이지를 WAL로 보내 원자적으로 커밋한다(데이터·인덱스 공통). */
static int wal_flush_commit(BufferPool *bp, Wal *wal) {
    wal_begin(wal);
    int n = bufpool_flush_cb(bp, wal_stage_sink, wal);
    if (n < 0) {
        return -1;
    }
    if (n > 0) {
        return wal_commit(wal); /* 로그+마커+fsync -> 데이터 적용 -> 로그 비움 */
    }
    return 0; /* 바뀐 게 없으면 로그 쓸 것도 없다 */
}

static int exec_begin(Database *db, FILE *out) {
    if (db->in_txn) {
        fprintf(out, "ERROR: 이미 트랜잭션 중입니다\n");
        return -1;
    }
    db->in_txn = 1;
    db->cur_txn = db->next_txn++; /* 이 트랜잭션의 락 소유자 id · 행 xmin */
    txnlog_begin(&db->txnlog, db->cur_txn);
    for (int i = 0; i < db->num_tables; i++) {
        Table *t = &db->tables[i];
        bufpool_set_no_steal(t->bp, 1);
        bufpool_set_steal_handler(t->bp, wal_steal_cb, &t->wal);
        wal_begin(&t->wal);
        t->txn_data_pages = t->wal.data.num_pages;
        if (t->has_index) {
            bufpool_set_no_steal(t->index.bp, 1);
            bufpool_set_steal_handler(t->index.bp, wal_steal_cb, &t->index.wal);
            wal_begin(&t->index.wal);
            t->txn_index_pages = t->index.wal.data.num_pages;
        }
        for (int k = 0; k < t->num_sec; k++) {
            bufpool_set_no_steal(t->sec[k].tree.bp, 1);
            bufpool_set_steal_handler(t->sec[k].tree.bp, wal_steal_cb, &t->sec[k].tree.wal);
            wal_begin(&t->sec[k].tree.wal);
            t->sec[k].txn_pages = t->sec[k].tree.wal.data.num_pages;
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
        wal_flush_commit(t->bp, &t->wal); /* 데이터: WAL로 원자 커밋 */
        bufpool_set_no_steal(t->bp, 0);
        bufpool_set_steal_handler(t->bp, NULL, NULL);
        if (t->has_index) {
            wal_flush_commit(t->index.bp, &t->index.wal); /* 인덱스: 인덱스 WAL로 */
            bufpool_set_no_steal(t->index.bp, 0);
            bufpool_set_steal_handler(t->index.bp, NULL, NULL);
        }
        for (int k = 0; k < t->num_sec; k++) {
            wal_flush_commit(t->sec[k].tree.bp, &t->sec[k].tree.wal);
            bufpool_set_no_steal(t->sec[k].tree.bp, 0);
            bufpool_set_steal_handler(t->sec[k].tree.bp, NULL, NULL);
        }
    }
    txnlog_commit(&db->txnlog, db->cur_txn); /* MVCC: 이 트랜잭션을 커밋 표시 */
    lock_release_all(&db->lm, db->cur_txn);  /* 2PL: 끝에서 한꺼번에 푼다 */
    db->cur_txn = 0;
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
        /* steal이 있었으면 로그의 before-image로 디스크를 먼저 원복(+새 페이지 truncate).
         * 그다음 풀 전체를 무효화해 steal로 clean 처리된 미커밋 프레임까지 버린다. */
        wal_undo(&t->wal);
        bufpool_invalidate_all(t->bp);
        pager_truncate(&t->wal.data, t->txn_data_pages); /* non-steal 경로의 새 페이지 제거 */
        wal_begin(&t->wal);
        bufpool_set_no_steal(t->bp, 0);
        bufpool_set_steal_handler(t->bp, NULL, NULL);
        if (t->has_index) {
            wal_undo(&t->index.wal);
            bufpool_invalidate_all(t->index.bp);
            pager_truncate(&t->index.wal.data, t->txn_index_pages);
            wal_begin(&t->index.wal);
            btree_reload_root(&t->index); /* 루트가 분할로 바뀌었을 수 있으니 다시 읽는다 */
            bufpool_set_no_steal(t->index.bp, 0);
            bufpool_set_steal_handler(t->index.bp, NULL, NULL);
        }
        for (int k = 0; k < t->num_sec; k++) {
            wal_undo(&t->sec[k].tree.wal);
            bufpool_invalidate_all(t->sec[k].tree.bp);
            pager_truncate(&t->sec[k].tree.wal.data, t->sec[k].txn_pages);
            wal_begin(&t->sec[k].tree.wal);
            btree_reload_root(&t->sec[k].tree);
            bufpool_set_no_steal(t->sec[k].tree.bp, 0);
            bufpool_set_steal_handler(t->sec[k].tree.bp, NULL, NULL);
        }
    }
    txnlog_abort(&db->txnlog, db->cur_txn); /* MVCC: 이 트랜잭션을 아보트 표시 */
    lock_release_all(&db->lm, db->cur_txn); /* 2PL: 끝에서 한꺼번에 푼다 */
    db->cur_txn = 0;
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
    lock_init(&db->lm);
    txnlog_init(&db->txnlog);
    db->cur_txn = 0;
    db->next_txn = 1;
    db->committed_below = 1; /* 새 DB: 이전 트랜잭션 없음 */

    /* 카탈로그가 있으면 테이블 목록을 복원하고 각 테이블 파일을 연다. */
    FILE *f = fopen(path, "rb");
    if (f) {
        int32_t n = 0;
        if (fread(&n, sizeof(n), 1, f) == 1 && n >= 0 && n <= DB_MAX_TABLES) {
            int32_t nt = 1; /* MVCC: 저장된 next_txn -> 그 미만 id는 전부 커밋된 것으로 본다 */
            if (fread(&nt, sizeof(nt), 1, f) == 1 && nt > db->next_txn) {
                db->next_txn = nt;
            }
            db->committed_below = db->next_txn;
            for (int i = 0; i < n; i++) {
                CreateStmt s;
                if (fread(&s, sizeof(s), 1, f) != 1) {
                    break;
                }
                Table *t = &db->tables[db->num_tables];
                t->schema = s;
                /* 보조 인덱스 정의 복원(없거나 옛 포맷이면 0개로) */
                t->num_sec = 0;
                int32_t ns = 0;
                if (fread(&ns, sizeof(ns), 1, f) == 1 && ns >= 0 && ns <= DB_MAX_SEC_IDX) {
                    for (int k = 0; k < ns; k++) {
                        int32_t col = 0;
                        if (fread(t->sec[k].name, SQL_NAME_LEN, 1, f) != 1 ||
                            fread(&col, sizeof(col), 1, f) != 1) {
                            break;
                        }
                        t->sec[k].col = col;
                        t->num_sec++;
                    }
                }
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
    catalog_write(db); /* MVCC: 이번 세션의 최종 next_txn을 영속화(재오픈 시 옛 행 가시성) */
    for (int i = 0; i < db->num_tables; i++) {
        table_close_files(&db->tables[i]);
    }
    db->num_tables = 0;
}

/* 테이블 하나에 락을 건다(키 0 = 테이블 전체). 충돌이면 에러를 찍고 -1. */
static int lock_one(Database *db, const char *table, LockMode mode, int txn, FILE *out) {
    if (lock_acquire(&db->lm, txn, table, 0, mode) != 0) {
        fprintf(out, "ERROR: 테이블 '%s'가 다른 트랜잭션에 잠겨 있습니다 (%s 충돌)\n", table,
                mode == LOCK_X ? "쓰기" : "읽기");
        return -1;
    }
    return 0;
}

/* 문장이 건드리는 테이블에 락(쓰기 X / 읽기 S). 충돌이면 -1. CREATE/INDEX/트랜잭션 제어는 안 검. */
static int acquire_stmt_locks(Database *db, const Statement *st, int txn, FILE *out) {
    switch (st->type) {
        case STMT_INSERT:
            return lock_one(db, st->insert.table, LOCK_X, txn, out);
        case STMT_DELETE:
            return lock_one(db, st->del.table, LOCK_X, txn, out);
        case STMT_UPDATE:
            return lock_one(db, st->upd.table, LOCK_X, txn, out);
        case STMT_SELECT:
            if (st->select.explain) {
                return 0; /* EXPLAIN은 실행을 안 하니 락 불필요 */
            }
            if (lock_one(db, st->select.table, LOCK_S, txn, out) != 0) {
                return -1;
            }
            for (int k = 0; k < st->select.num_joins; k++) {
                if (lock_one(db, st->select.joins[k].table, LOCK_S, txn, out) != 0) {
                    return -1;
                }
            }
            return 0;
        default:
            return 0;
    }
}

int db_exec(Database *db, const char *sql, FILE *out) {
    Statement st;
    char err[128];
    if (sql_parse(sql, &st, err, sizeof(err)) != 0) {
        fprintf(out, "ERROR: %s\n", err);
        statement_free(&st); /* 실패 전에 만든 서브쿼리 노드도 해제 */
        return -1;
    }
    /* IN-서브쿼리가 있으면 바깥 스캔 전에 한 번씩 실행해 값 집합을 채운다. */
    Where *w = (st.type == STMT_SELECT)   ? &st.select.where
               : (st.type == STMT_DELETE) ? &st.del.where
               : (st.type == STMT_UPDATE) ? &st.upd.where
                                          : NULL;
    if (w && prepare_where(db, w) != 0) {
        fprintf(out, "ERROR: 서브쿼리를 실행할 수 없습니다 (지원 형태: SELECT <컬럼> FROM <테이블> [WHERE ...])\n");
        statement_free(&st);
        return -1;
    }
    /* 격리(2PL): 이 문장이 건드리는 테이블에 락을 건다. 명시적 트랜잭션이면 그 txn id로
     * 잡아 COMMIT/ROLLBACK까지 쥐고, autocommit이면 임시 id로 잡았다가 문장 끝에 푼다.
     * 다른 트랜잭션이 이미 충돌 락을 쥐고 있으면(단일 스레드라 "블록" 대신) 문장을 거부한다. */
    int lock_txn = 0, lock_autorelease = 0;
    if (st.type == STMT_INSERT || st.type == STMT_DELETE || st.type == STMT_UPDATE ||
        (st.type == STMT_SELECT && !st.select.explain)) {
        lock_txn = db->in_txn ? db->cur_txn : db->next_txn++;
        lock_autorelease = !db->in_txn;
        if (lock_autorelease) {
            db->cur_txn = lock_txn; /* autocommit: 이 문장이 곧 한 트랜잭션 (행 xmin에 쓰임) */
            txnlog_begin(&db->txnlog, lock_txn);
        }
        if (acquire_stmt_locks(db, &st, lock_txn, out) != 0) {
            if (lock_autorelease) {
                lock_release_all(&db->lm, lock_txn);
            }
            statement_free(&st);
            return -1;
        }
    }
    /* autocommit: 트랜잭션 밖 DML은 이 문장 하나가 곧 한 트랜잭션이다. WAL로 커밋될
     * 때까지 dirty 페이지(데이터·인덱스 둘 다)가 디스크로 새지 않게 no-steal을 켠다. */
    int autocommit = !db->in_txn &&
                     (st.type == STMT_CREATE || st.type == STMT_INSERT ||
                      st.type == STMT_DELETE || st.type == STMT_UPDATE);
    if (autocommit) {
        for (int i = 0; i < db->num_tables; i++) {
            Table *t = &db->tables[i];
            bufpool_set_no_steal(t->bp, 1);
            bufpool_set_steal_handler(t->bp, wal_steal_cb, &t->wal);
            wal_begin(&t->wal); /* base_pages 포착 — steal 시 undo truncate 기준 */
            if (t->has_index) {
                bufpool_set_no_steal(t->index.bp, 1);
                bufpool_set_steal_handler(t->index.bp, wal_steal_cb, &t->index.wal);
                wal_begin(&t->index.wal);
            }
            for (int k = 0; k < t->num_sec; k++) {
                bufpool_set_no_steal(t->sec[k].tree.bp, 1);
                bufpool_set_steal_handler(t->sec[k].tree.bp, wal_steal_cb, &t->sec[k].tree.wal);
                wal_begin(&t->sec[k].tree.wal);
            }
        }
    }
    int rc;
    switch (st.type) {
        case STMT_CREATE: rc = exec_create(db, &st.create, out); break;
        case STMT_CREATE_INDEX: rc = exec_create_index(db, &st.cidx, out); break;
        case STMT_INSERT: rc = exec_insert(db, &st.insert, out); break;
        case STMT_SELECT: rc = exec_select(db, &st.select, out); break;
        case STMT_DELETE: rc = exec_delete(db, &st.del, out); break;
        case STMT_UPDATE: rc = exec_update(db, &st.upd, out); break;
        case STMT_BEGIN: rc = exec_begin(db, out); break;
        case STMT_COMMIT: rc = exec_commit(db, out); break;
        case STMT_ROLLBACK: rc = exec_rollback(db, out); break;
        default: rc = -1;
    }
    if (autocommit) {
        for (int i = 0; i < db->num_tables; i++) {
            Table *t = &db->tables[i];
            if (rc == 0) {
                wal_flush_commit(t->bp, &t->wal); /* 데이터: WAL로 원자 커밋 */
                if (t->has_index) {
                    wal_flush_commit(t->index.bp, &t->index.wal); /* 인덱스: 인덱스 WAL로 */
                }
                for (int k = 0; k < t->num_sec; k++) {
                    wal_flush_commit(t->sec[k].tree.bp, &t->sec[k].tree.wal);
                }
            } else {
                /* 실패한 문장의 변경은 버린다. steal이 있었으면 before-image로 원복하고
                 * 새로 할당한 페이지를 잘라낸다. */
                uint64_t base = t->wal.base_pages;
                wal_undo(&t->wal);
                bufpool_invalidate_all(t->bp);
                pager_truncate(&t->wal.data, base);
                wal_begin(&t->wal);
                if (t->has_index) {
                    uint64_t ibase = t->index.wal.base_pages;
                    wal_undo(&t->index.wal);
                    bufpool_invalidate_all(t->index.bp);
                    pager_truncate(&t->index.wal.data, ibase);
                    wal_begin(&t->index.wal);
                    btree_reload_root(&t->index);
                }
                for (int k = 0; k < t->num_sec; k++) {
                    uint64_t sbase = t->sec[k].tree.wal.base_pages;
                    wal_undo(&t->sec[k].tree.wal);
                    bufpool_invalidate_all(t->sec[k].tree.bp);
                    pager_truncate(&t->sec[k].tree.wal.data, sbase);
                    wal_begin(&t->sec[k].tree.wal);
                    btree_reload_root(&t->sec[k].tree);
                }
            }
            bufpool_set_no_steal(t->bp, 0);
            bufpool_set_steal_handler(t->bp, NULL, NULL);
            if (t->has_index) {
                bufpool_set_no_steal(t->index.bp, 0);
                bufpool_set_steal_handler(t->index.bp, NULL, NULL);
            }
            for (int k = 0; k < t->num_sec; k++) {
                bufpool_set_no_steal(t->sec[k].tree.bp, 0);
                bufpool_set_steal_handler(t->sec[k].tree.bp, NULL, NULL);
            }
        }
    }
    if (lock_autorelease) {
        if (rc == 0) {
            txnlog_commit(&db->txnlog, lock_txn); /* 문장 성공 -> 이 트랜잭션 커밋 */
        } else {
            txnlog_abort(&db->txnlog, lock_txn);
        }
        lock_release_all(&db->lm, lock_txn); /* autocommit 문장: 잡았던 락을 푼다 */
    }
    statement_free(&st); /* 서브쿼리·IN 집합 해제 */
    return rc;
}
