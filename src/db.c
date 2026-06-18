#include "db.h"
#include "page.h" /* VACUUM의 슬롯 검사·compaction */
#include "parscan.h" /* 병렬 풀 스캔(36편) — 스트리밍 SELECT 풀스캔 배선 */

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
        /* 옵티마이저 통계도 영속화 (ANALYZE 결과) */
        Table *tt = &db->tables[i];
        fwrite(&tt->stat_valid, sizeof(int32_t), 1, f);
        fwrite(&tt->stat_rows, sizeof(int64_t), 1, f);
        fwrite(&tt->stat_pages, sizeof(int64_t), 1, f);
        fwrite(&tt->stat_pk_min, sizeof(int64_t), 1, f);
        fwrite(&tt->stat_pk_max, sizeof(int64_t), 1, f);
    }
    fclose(f);
}

/* ── PK 인덱스 추상화(Table Access Method): B+Tree 또는 LSM ───────────────
 * 실행기는 PK 인덱스를 '키(PK) -> RID' 매핑으로만 쓴다. 그 매핑을 제자리 갱신
 * B+Tree로 담을지, append-only LSM으로 담을지는 테이블마다 `USING`으로 고른다.
 * 둘 다 '한 PK -> 여러 RID'(MVCC 다중버전)라 비유니크(dup)여야 한다 — B+Tree는
 * btree_insert_dup, LSM은 멀티값 모드(lsm_put_dup)가 그 역할을 한다.
 *
 * ── 정직한 경계 ──────────────────────────────────────────────────────
 * LSM PK 인덱스는 B+Tree 인덱스와 달리 자체 WAL/트랜잭션 롤백에 참여하지 않는다.
 * 대신 WAL-backed heap을 진실의 원천으로 삼는 '파생 가속기'다: 재오픈·롤백 시
 * heap에서 통째로 재구축하고(lsm_pk_rebuild), 중단된 트랜잭션이 남긴 dangling
 * 항목은 읽기 경로의 heap 가시성 게이트(point_visit/range_visit의 rec_visible)가
 * 걸러낸다. 그래서 인덱스가 잠깐 부정확해도 결과는 항상 옳다.
 * ──────────────────────────────────────────────────────────────────── */
#define LSM_PK_THRESHOLD 256 /* memtable 임계치(넘으면 SSTable로 flush) */

/* btree_visit_fn(반환값=중단)과 lsm 콜백(void) 사이 어댑터. */
typedef struct { btree_visit_fn visit; void *ctx; } PidxCb;
static void pidx_cb_adapter(int64_t key, int64_t val, void *ctx_) {
    PidxCb *a = ctx_;
    a->visit((bkey_t)key, (bval_t)val, a->ctx); /* LSM 스캔은 조기중단을 안 쓰므로 반환 무시 */
}

/* PK 인덱스에 (키 -> RID) 추가(비유니크: 같은 PK의 여러 버전 공존). */
static void pidx_insert(Table *t, int64_t key, RID rid) {
    if (t->index_kind == 1) lsm_put_dup(t->lindex, key, rid_encode(rid));
    else btree_insert_dup(&t->index, key, rid_encode(rid));
}

/* PK 인덱스에서 특정 (키, RID) 짝만 제거(VACUUM이 죽은 버전 정리 시). */
static void pidx_delete(Table *t, int64_t key, RID rid) {
    if (t->index_kind == 1) lsm_delete_val(t->lindex, key, rid_encode(rid));
    else btree_delete_val(&t->index, key, rid_encode(rid));
}

/* PK '=' 점 조회: 그 키의 모든 버전 RID를 visit로 넘긴다(가시성 판정은 콜백이). */
static void pidx_find_all(Table *t, int64_t key, btree_visit_fn visit, void *ctx) {
    if (t->index_kind == 1) {
        PidxCb a = {visit, ctx};
        lsm_find_all(t->lindex, key, pidx_cb_adapter, &a);
    } else {
        btree_find_all(&t->index, key, visit, ctx);
    }
}

/* PK 범위 조회(<,>,<=,>=): 콜백(range_visit)이 op·가시성으로 최종 필터링한다.
 * LSM은 lsm_scan에 [lo,hi]를 주고, B+Tree는 seek/full-scan을 그대로 쓴다. */
static void pidx_range(Table *t, int op, int64_t bound, btree_visit_fn visit, void *ctx) {
    if (t->index_kind == 1) {
        int64_t lo = INT64_MIN, hi = INT64_MAX;
        if (op == CMP_GT || op == CMP_GE) lo = bound; /* 하한만 */
        else hi = bound;                              /* LT/LE: 상한만 */
        PidxCb a = {visit, ctx};
        lsm_scan(t->lindex, lo, hi, pidx_cb_adapter, &a);
    } else {
        if (op == CMP_GT || op == CMP_GE) btree_seek_scan(&t->index, bound, visit, ctx);
        else btree_scan(&t->index, visit, ctx);
    }
}

/* heap의 모든 튜플(라이브·죽은 버전 무관)에 대해 (PK -> RID)를 LSM 인덱스에 심는다.
 * B+Tree 인덱스가 VACUUM 전까지 모든 버전 항목을 갖는 것과 같은 multiset을 만든다. */
static int lsm_pk_build_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)len;
    Table *t = ctx_;
    Value row[SQL_MAX_COLS];
    decode_row(&t->schema, (const uint8_t *)rec, row);
    if (row[0].type == VAL_INT) lsm_put_dup(t->lindex, row[0].int_val, rid_encode(rid));
    return 0;
}

/* LSM PK 인덱스를 비우고 heap에서 다시 채운다(재오픈·롤백 후 heap과 동기화). */
static void lsm_pk_rebuild(Table *t) {
    lsm_clear(t->lindex);
    heap_scan(&t->heap, lsm_pk_build_visit, t);
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
    t->index_kind = t->schema.index_kind; /* 카탈로그에 영속된 저장 엔진 선택 */
    t->lindex = NULL;
    if (t->schema.num_columns > 0 && t->schema.columns[0].type == COL_INT) {
        if (t->index_kind == 1) {
            /* LSM PK 인덱스: 디렉터리로 연 뒤 heap에서 재구축(파생 가속기). */
            char ld[720];
            snprintf(ld, sizeof(ld), "%s.%s.lsmidx", dbpath, t->schema.table);
            t->lindex = lsm_open_multi(ld, LSM_PK_THRESHOLD, 1);
            if (!t->lindex) return -1;
            t->has_index = 1;
            lsm_pk_rebuild(t); /* 옛 SSTable을 비우고 heap의 모든 버전으로 다시 채움 */
        } else {
            char ip[700];
            snprintf(ip, sizeof(ip), "%s.%s.idx", dbpath, t->schema.table);
            if (btree_open(&t->index, ip) == 0) {
                t->has_index = 1;
            }
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
        if (t->index_kind == 1) {
            lsm_close(t->lindex); /* memtable은 버려짐 — 재오픈 때 heap에서 재구축 */
            t->lindex = NULL;
        } else {
            btree_close(&t->index);
        }
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

/* txn이 "커밋된 것으로 보이나" — 이전 실행 id(committed_below 미만)는 전부 커밋,
 * 이번 실행 id는 TxnLog가 COMMITTED일 때만. 그리고 명시적 트랜잭션 안에서는
 * **시작 시점 스냅샷**으로 판정한다(스냅샷 격리): 내 시작 이후에 발급됐거나(>= snap_next)
 * 시작 시점에 진행 중이던 트랜잭션은, 지금 커밋돼 있어도 나에겐 '아직 커밋 안 됨'이다.
 * 트랜잭션 밖(autocommit 문장)은 지금 상태 그대로 본다(read committed). */
static int txn_committed_view(Database *db, int txn) {
    if (txn <= 0) {
        return 0;
    }
    if (txn < db->committed_below) {
        return 1;
    }
    if (txnlog_status(&db->txnlog, txn) != TXN_COMMITTED) {
        return 0;
    }
    const DbSession *s = &db->sessions[db->cur_session];
    if (s->in_txn) {
        if (txn >= s->snap_next) {
            return 0; /* 내 시작 이후에 태어난 트랜잭션 */
        }
        for (int i = 0; i < s->n_snap_inprog; i++) {
            if (s->snap_inprog[i] == txn) {
                return 0; /* 내 시작 시점에 진행 중이던 트랜잭션 */
            }
        }
    }
    return 1;
}

/* 트랜잭션 절(아래)에 정의 — 쓰기 실행기들이 첫 쓰기 전에 부른다. */
static void table_begin_write(Database *db, Table *t, int txn);

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

/* 스캔/조회가 받은 원시 레코드가 현재 문장(트랜잭션)에 보이는 버전인가.
 * DELETE가 tombstone이 아니라 xmax를 새기므로(논리 삭제), 힙을 읽는 '모든' 경로가
 * 이 게이트를 지나야 한다 — 풀스캔·인덱스 조회·조인·집계·서브쿼리·DML 수집까지. */
static int rec_visible(Database *db, const void *rec) {
    return row_visible(db, db_rec_xmin(rec), db_rec_xmax(rec), db->cur_txn);
}

/* DELETE/UPDATE의 옛 버전 처리: 행을 지우지 않고 xmax만 새긴다(PostgreSQL식 논리 삭제).
 * 길이가 같아 제자리 덮어쓰기 — 물리 공간 회수는 VACUUM(예정)의 일. */
static int stamp_xmax(Table *t, RID rid, int32_t xmax) {
    uint8_t rec[PAGE_SIZE];
    uint16_t len;
    if (heap_get(&t->heap, rid, rec, &len) != 0) {
        return -1;
    }
    memcpy(rec + 4, &xmax, 4); /* MVCC 헤더 = [xmin(4)][xmax(4)] */
    return heap_overwrite(&t->heap, rid, rec, len);
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
    t->owner = db;  /* 읽기 경로의 가시성 판정용 역참조 */
    t->writer_txn = 0;
    if (table_open_files(t, db->path) != 0) {
        fprintf(out, "ERROR: 테이블 파일을 열 수 없습니다\n");
        return -1;
    }
    db->num_tables++;
    table_begin_write(db, t, db->cur_txn); /* 초기 페이지(인덱스 메타 등)도 WAL로 커밋 */
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
    Database *db;
} SecBuildCtx;

static int secidx_build_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)len;
    SecBuildCtx *c = ctx_;
    if (!rec_visible(c->db, rec)) {
        return 0; /* 지워진(xmax) 옛 버전은 색인하지 않는다 */
    }
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
    if (t && t->writer_txn != 0) {
        /* 미커밋 쓰기가 있는 테이블 위에 인덱스를 지으면 (가시성 게이트가 그 행들을
         * 걸러) 커밋 후 인덱스에 구멍이 난다 — DDL이라 그냥 거부한다. */
        fprintf(out, "ERROR: 다른 트랜잭션이 '%s' 테이블을 쓰는 중입니다\n", ci->table);
        return -1;
    }
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
    SecBuildCtx bc = {si, col, &t->schema, 0, db};
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
    table_begin_write(db, t, db->cur_txn); /* 이 테이블의 WAL 트랜잭션을 (처음이면) 연다 */
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
        /* 버전마다 인덱스 항목을 단다(PostgreSQL식) — 다중 버전에선 같은 PK의 옛/새
         * 버전이 공존하므로, 유니크 덮어쓰기 대신 (키,RID) 짝을 추가하고 조회가
         * '보이는' 버전을 고른다. B+Tree든 LSM이든 pidx가 라우팅한다. */
        pidx_insert(t, in->values[0].int_val, rid);
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

/* ── 병렬 스트리밍 풀스캔(37편): 36편 parscan을 실제 SELECT에 배선 ──────────
 * 뜨거운 부분(가시성 판정 + WHERE 평가)을 워커들이 병렬로 돌려 매칭 RID를 페이지
 * 순서로 모으고, 유일하게 스레드 안전하지 않은 '출력'은 leader가 직렬로 한다.
 * 그래서 결과는 직렬 경로와 바이트 단위로 동일하다. engine_mtx가 실행을 직렬화하는
 * 동안(동시 writer 없음) 힙·스냅샷·스키마·WHERE는 모두 읽기 전용이라 안전하다. */
#define PARSCAN_MIN_PAGES 16 /* 이 미만이면 병렬 오버헤드가 이득보다 커 직렬로 */
/* 병렬 워커 수. 기본 4. 벤치/테스트가 워커 수별 speedup을 재려고 런타임에 바꾼다
 * (db_set_parallel_workers). 1이면 사실상 직렬 기준선(스레드 1개가 전 페이지 처리). */
static int g_parscan_workers = 4;
void db_set_parallel_workers(int n) { g_parscan_workers = (n < 1) ? 1 : n; }
int db_get_parallel_workers(void) { return g_parscan_workers; }

/* WHERE에 서브쿼리(scalar/IN-SELECT)가 있으면 술어가 실행기를 재진입 → 병렬 불가. */
static int where_has_subquery(const Where *w) {
    for (int g = 0; g < w->count; g++)
        for (int i = 0; i < w->groups[g].count; i++)
            if (w->groups[g].conds[i].sub != NULL) return 1;
    return 0;
}

typedef struct {
    const CreateStmt *schema;
    const char       *tname;
    const Where      *where;
    Database         *db;
    int               my_txn;
} ParSelCtx;

/* 워커 술어: 가시성 게이트 + WHERE 평가(둘 다 읽기 전용). 매칭이면 1. */
static int parsel_pred(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)rid; (void)len;
    ParSelCtx *c = ctx_;
    if (!row_visible(c->db, db_rec_xmin(rec), db_rec_xmax(rec), c->my_txn)) return 0;
    Value row[SQL_MAX_COLS];
    decode_row(c->schema, rec, row);
    return where_matches(c->schema, c->tname, c->where, row);
}

/* 자격이 되면 병렬로 풀스캔해 결과를 출력하고 1을 반환(호출자는 직렬 스킵).
 * 자격 미달(작은 테이블/서브쿼리/에러)이면 0을 반환 → 호출자가 직렬로 폴백. */
static int parallel_fullscan(Database *db, Table *t, const char *tname,
                             const Where *where, FILE *out, int *count_out) {
    uint64_t np = t->heap.pager->num_pages;
    uint64_t pages = (np > t->heap.first_page) ? (np - t->heap.first_page) : 0;
    if (pages < PARSCAN_MIN_PAGES) return 0;   /* 작은 테이블은 직렬이 낫다 */
    if (where_has_subquery(where)) return 0;   /* 서브쿼리는 실행기 재진입 → 직렬 */

    ParSelCtx pc = {&t->schema, tname, where, db, db->cur_txn};
    ParscanResult res;
    if (parscan_collect(&t->heap, g_parscan_workers, parsel_pred, &pc, &res) != 0)
        return 0; /* 병렬 실패 시 조용히 직렬 폴백 */

    /* leader가 페이지 순서(res는 이미 페이지 순서)로 직렬 출력 — 직렬과 바이트 동일. */
    int n = 0;
    uint8_t recbuf[PAGE_SIZE];
    uint16_t len;
    for (int64_t i = 0; i < res.n; i++) {
        if (heap_get(&t->heap, res.rids[i], recbuf, &len) == 0) {
            Value row[SQL_MAX_COLS];
            decode_row(&t->schema, recbuf, row);
            print_row(out, &t->schema, row);
            n++;
        }
    }
    parscan_result_free(&res);
    *count_out = n;
    return 1;
}

/* 인덱스 범위 스캔: B+Tree가 준 (키, RID)마다 힙 행을 읽어 출력한다.
 * 인덱스는 MVCC를 모른다 — 힙 행을 읽은 뒤 가시성 게이트로 거른다(지워진/미커밋 버전 제외). */
typedef struct {
    Table *t;
    bkey_t bound;
    CmpOp op;
    FILE *out;
    int count;
} RangeCtx;

/* PK 점 조회: 다중 버전 인덱스라 같은 키에 항목이 여러 개일 수 있다 — 후보마다
 * 힙에서 읽어 '보이는' 버전만 출력한다(인덱스는 MVCC를 모른다, 판정은 게이트가). */
typedef struct {
    Table *t;
    FILE *out;
    int count;
} PointCtx;

static int point_visit(bkey_t key, bval_t val, void *ctx_) {
    (void)key;
    PointCtx *p = ctx_;
    uint8_t recbuf[PAGE_SIZE];
    uint16_t len;
    if (heap_get(&p->t->heap, rid_decode(val), recbuf, &len) == 0 &&
        rec_visible(p->t->owner, recbuf)) {
        Value row[SQL_MAX_COLS];
        decode_row(&p->t->schema, recbuf, row);
        print_row(p->out, &p->t->schema, row);
        p->count++;
    }
    return 0;
}

static int range_visit(bkey_t key, bval_t val, void *ctx_) {
    RangeCtx *r = ctx_;
    if (r->op == CMP_GT && key == r->bound) return 0;  /* seek는 ==도 주니 건너뜀 */
    if (r->op == CMP_LT && key >= r->bound) return 1;  /* 상한 도달 -> 멈춤 */
    if (r->op == CMP_LE && key > r->bound) return 1;
    uint8_t recbuf[PAGE_SIZE];
    uint16_t len;
    if (heap_get(&r->t->heap, rid_decode(val), recbuf, &len) == 0 &&
        rec_visible(r->t->owner, recbuf)) {
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
    Database *db; /* MVCC 가시성 판정용 */
} MatCtx;

static int mat_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)rid;
    (void)len;
    MatCtx *m = ctx_;
    if (!rec_visible(m->db, rec)) {
        return 0; /* 안 보이는 버전(미커밋/아보트/지워짐)은 건너뜀 */
    }
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
                .count = 0,
                .db = t->owner};
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
/* ── 진짜 병렬 부분 집계(39편): 행을 안 모으고 누적만 — 메모리 O(워커수×항목수) ──
 * 38편은 매칭 행을 모두 모은 뒤 집계했다(메모리 O(매칭행수)). 39편은 워커가 자기
 * 페이지 범위를 훑으며 항목별 부분합(total/cnt/sum/min/max)만 '누적'하고, leader가
 * 부분합을 결합해 compute_cell과 같은 OutCell을 만든다 — 행을 하나도 안 쌓는다.
 * PostgreSQL의 Partial Aggregate -> Finalize Aggregate 두 단계와 같은 결.
 * 자격: 그룹/HAVING/DISTINCT 없는 순수 집계, 모든 항목이 INT 집계(또는 COUNT(*)),
 * 서브쿼리 없는 WHERE, 큰 테이블. 그 외는 38편(수집)·직렬 경로로 폴백. */
typedef struct {
    long total;      /* 매칭 행 수 (COUNT(*)) */
    long cnt;        /* 비NULL 개수 (COUNT(col)/SUM/AVG) */
    long sum;        /* 합 (SUM/AVG) */
    long minv, maxv; /* 최소/최대 (INT) */
    int  seen;       /* 비NULL을 하나라도 봤나 */
} AggPart;

typedef struct {
    const CreateStmt *schema;
    const char       *tname;
    const Where      *where;
    Database         *db;
    int               my_txn;
    const SelectStmt *sel;
    const int        *item_ci;         /* 공유 읽기전용: 항목별 컬럼 인덱스(-1=COUNT(*)) */
    AggPart           part[SQL_MAX_COLS]; /* 워커 전용 누적(락 불필요) */
} AggWctx;

/* 워커: 가시성 게이트 + WHERE 통과 행에 대해 항목별 부분합을 누적(행은 안 모음). */
static void agg_worker_visit(RID rid, const void *rec, uint16_t len, void *c_) {
    (void)rid; (void)len;
    AggWctx *w = c_;
    if (!row_visible(w->db, db_rec_xmin(rec), db_rec_xmax(rec), w->my_txn)) return;
    Value row[SQL_MAX_COLS];
    decode_row(w->schema, rec, row);
    if (!where_matches(w->schema, w->tname, w->where, row)) return;
    for (int k = 0; k < w->sel->num_items; k++) {
        const SelectItem *it = &w->sel->items[k];
        AggPart *p = &w->part[k];
        p->total++;
        if (it->agg == AGG_COUNT && it->star) continue; /* COUNT(*): 행 수만 */
        const Value *v = &row[w->item_ci[k]];
        if (v->type == VAL_NULL) continue;              /* COUNT(col)/SUM/AVG/MIN/MAX는 NULL 무시 */
        long iv = v->int_val;
        p->cnt++;
        p->sum += iv;
        if (!p->seen) { p->minv = p->maxv = iv; p->seen = 1; }
        else { if (iv < p->minv) p->minv = iv; if (iv > p->maxv) p->maxv = iv; }
    }
}

static int try_parallel_partial_aggregate(Table *t, const char *tname,
                                          const SelectStmt *sel, FILE *out) {
    if (!sel->has_aggregate) return 0;
    if (sel->group_col[0] || sel->has_having || sel->distinct) return 0;
    if (where_has_subquery(&sel->where)) return 0;

    Database *db = t->owner;
    uint64_t np = t->heap.pager->num_pages;
    uint64_t pages = (np > t->heap.first_page) ? (np - t->heap.first_page) : 0;
    if (pages < PARSCAN_MIN_PAGES) return 0;

    int ncols = t->schema.num_columns;
    ColRef cols[SQL_MAX_COLS];
    for (int i = 0; i < ncols; i++) {
        cols[i].tbl = tname;
        cols[i].col = t->schema.columns[i].name;
        cols[i].is_int = (t->schema.columns[i].type == COL_INT);
    }
    /* 항목 해소 + INT 검증. 부적합(투영 섞임·없는 컬럼·비INT SUM/AVG/MIN/MAX)이면
     * 폴백 → 직렬/38 경로가 처리(에러 메시지도 거기서 일관되게). */
    int item_ci[SQL_MAX_COLS];
    for (int k = 0; k < sel->num_items; k++) {
        const SelectItem *it = &sel->items[k];
        if (it->agg == AGG_NONE) return 0;
        if (it->agg == AGG_COUNT && it->star) { item_ci[k] = -1; continue; }
        int ci = find_col(cols, ncols, it->tbl, it->col);
        if (ci < 0) return 0;
        if (!cols[ci].is_int) return 0; /* TEXT MIN/MAX 등은 직렬로 */
        item_ci[k] = ci;
    }

    int nw = g_parscan_workers;
    AggWctx *wctx = calloc((size_t)nw, sizeof(AggWctx));
    void **ctxs = calloc((size_t)nw, sizeof(void *));
    if (!wctx || !ctxs) { free(wctx); free(ctxs); return 0; }
    for (int i = 0; i < nw; i++) {
        wctx[i].schema = &t->schema;
        wctx[i].tname = tname;
        wctx[i].where = &sel->where;
        wctx[i].db = db;
        wctx[i].my_txn = db->cur_txn;
        wctx[i].sel = sel;
        wctx[i].item_ci = item_ci;
        ctxs[i] = &wctx[i];
    }
    if (parscan_foreach(&t->heap, nw, ctxs, agg_worker_visit) != 0) {
        free(wctx); free(ctxs); return 0; /* 폴백 */
    }

    /* Finalize: 워커 부분합을 결합해 compute_cell과 같은 OutCell을 만든다. */
    OutCell outbuf[SQL_MAX_COLS];
    for (int k = 0; k < sel->num_items; k++) {
        const SelectItem *it = &sel->items[k];
        long total = 0, cnt = 0, sum = 0, minv = 0, maxv = 0;
        int seen = 0;
        for (int i = 0; i < nw; i++) {
            AggPart *p = &wctx[i].part[k];
            total += p->total;
            cnt += p->cnt;
            sum += p->sum;
            if (p->seen) {
                if (!seen) { minv = p->minv; maxv = p->maxv; seen = 1; }
                else { if (p->minv < minv) minv = p->minv; if (p->maxv > maxv) maxv = p->maxv; }
            }
        }
        OutCell c = {0, 0, 0.0, {0}};
        if (it->agg == AGG_COUNT && it->star) {
            c.num = (double)total;
        } else if (it->agg == AGG_COUNT) {
            c.num = (double)cnt;
        } else if (it->agg == AGG_SUM || it->agg == AGG_AVG) {
            if (!seen) c.is_null = 1;
            else c.num = (it->agg == AGG_SUM) ? (double)sum : (double)sum / cnt;
        } else { /* MIN / MAX (INT) */
            if (!seen) c.is_null = 1;
            else c.num = (double)((it->agg == AGG_MIN) ? minv : maxv);
        }
        outbuf[k] = c;
    }
    free(wctx);
    free(ctxs);

    /* 헤더 + emit_out_rows(행 + 푸터) — 직렬 aggregate_rowset과 출력 형식 동일. */
    for (int k = 0; k < sel->num_items; k++) {
        if (k) fprintf(out, " | ");
        print_item_label(out, &sel->items[k]);
    }
    fprintf(out, "\n");
    emit_out_rows(sel, outbuf, 1, out);
    return 1;
}

/* ── 병렬 집계(38편): 집계/GROUP BY의 행 수집을 병렬로 + materialize cap 우회 ──
 * 직렬 exec_select_project는 SELECT_MAX_ROWS까지만 모아 큰 테이블 집계를 '조용히
 * 절단'한다(6000행 테이블의 COUNT(*)가 4096을 주는 버그). 여기선 워커가 가시성
 * 게이트+WHERE를 병렬 평가해 매칭 RID를 페이지 순서로 모으고(parscan_collect·
 * parsel_pred 재사용), leader가 매칭 수만큼 rows를 채워 aggregate_rowset(직렬과
 * '같은 코드')에 넘긴다 → cap 없이 정답을 내면서 병렬화, 출력 형식도 동일하다.
 * 자격 미달(작은 테이블/서브쿼리/집계·그룹 아님)이면 0 → 호출자가 직렬 폴백.
 * 정직한 경계: 매칭 행을 모두 담으므로(cap 제거의 대가) 매우 큰 결과엔 메모리를
 * 쓴다 — 진짜 partial aggregation(누적만, 행 안 모음)은 프론티어. */
static int try_parallel_aggregate(Table *t, const char *tname, const SelectStmt *sel, FILE *out) {
    if (!(sel->has_aggregate || sel->group_col[0])) return 0; /* 순수 투영은 직렬(형식 유지) */
    if (where_has_subquery(&sel->where)) return 0;

    Database *db = t->owner;
    uint64_t np = t->heap.pager->num_pages;
    uint64_t pages = (np > t->heap.first_page) ? (np - t->heap.first_page) : 0;
    if (pages < PARSCAN_MIN_PAGES) return 0;

    ParSelCtx pc = {&t->schema, tname, &sel->where, db, db->cur_txn};
    ParscanResult res;
    if (parscan_collect(&t->heap, g_parscan_workers, parsel_pred, &pc, &res) != 0) return 0;

    int ncols = t->schema.num_columns;
    Value *rows = malloc((size_t)(res.n > 0 ? res.n : 1) * ncols * sizeof(Value));
    if (!rows) { parscan_result_free(&res); return 0; } /* 폴백 */

    int nrows = 0;
    uint8_t recbuf[PAGE_SIZE];
    uint16_t len;
    for (int64_t i = 0; i < res.n; i++) {
        if (heap_get(&t->heap, res.rids[i], recbuf, &len) == 0) {
            Value row[SQL_MAX_COLS];
            decode_row(&t->schema, recbuf, row);
            memcpy(rows + (size_t)nrows * ncols, row, (size_t)ncols * sizeof(Value));
            nrows++;
        }
    }
    parscan_result_free(&res);

    ColRef cols[SQL_MAX_COLS];
    for (int i = 0; i < ncols; i++) {
        cols[i].tbl = tname;
        cols[i].col = t->schema.columns[i].name;
        cols[i].is_int = (t->schema.columns[i].type == COL_INT);
    }
    aggregate_rowset(sel, rows, nrows, ncols, cols, out);
    free(rows);
    return 1;
}

static int exec_select_project(Table *t, const char *tname, const SelectStmt *sel, FILE *out) {
    int ncols = t->schema.num_columns;
    /* 진짜 부분 집계(39편): 그룹 없는 순수 집계는 행을 안 모으고 병렬 누적만(메모리 O(1)). */
    if (try_parallel_partial_aggregate(t, tname, sel, out)) {
        return 0;
    }
    /* 병렬 집계(38편): 그 외 집계/GROUP BY는 병렬 수집 + cap 우회 경로로.
     * 자격 미달(작은 테이블/서브쿼리/순수 투영)이면 아래 직렬 경로로 폴백. */
    if (try_parallel_aggregate(t, tname, sel, out)) {
        return 0;
    }
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
                .count = 0,
                .db = t->owner};
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
    Database *db; /* MVCC 가시성 판정용 */
} HBuildCtx;

static int hbuild_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)rid;
    (void)len;
    HBuildCtx *c = ctx_;
    if (!rec_visible(c->db, rec)) {
        return 0; /* 빌드 단계에서 걸러야 탐사가 안 보이는 버전을 못 만난다 */
    }
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
    HBuildCtx c = {ht, &t->schema, col_idx, t->owner};
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

/* ------------- 옵티마이저: 통계(ANALYZE) + 비용 기반 접근 방법 선택 -------------
 * 지금까지 플래너는 규칙 기반이었다 — "PK 조건이면 무조건 인덱스". 그런데 넓은 범위
 * (테이블 대부분이 매칭)엔 인덱스 범위 스캔이 행마다 랜덤 힙 페치를 하느라 순차 스캔보다
 * 느리다. ANALYZE로 행 수·PK 범위를 재고, 선택도로 매칭 행 수를 추정해 비용으로 고른다. */

typedef struct {
    Database *db;
    const CreateStmt *schema;
    int64_t rows;
    int64_t pk_min, pk_max;
    int have;
} AnalyzeCtx;

static int analyze_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)rid; (void)len;
    AnalyzeCtx *c = ctx_;
    if (!rec_visible(c->db, rec)) {
        return 0; /* 보이는 행만 센다 (죽은 버전 제외) */
    }
    Value row[SQL_MAX_COLS];
    decode_row(c->schema, rec, row);
    c->rows++;
    if (c->schema->columns[0].type == COL_INT && row[0].type == VAL_INT) {
        long v = row[0].int_val;
        if (!c->have || v < c->pk_min) c->pk_min = v;
        if (!c->have || v > c->pk_max) c->pk_max = v;
        c->have = 1;
    }
    return 0;
}

static void analyze_table(Database *db, Table *t) {
    AnalyzeCtx c = {db, &t->schema, 0, 0, 0, 0};
    heap_scan(&t->heap, analyze_visit, &c);
    t->stat_rows = c.rows;
    int64_t pages = (int64_t)t->wal.data.num_pages - (int64_t)t->heap.first_page;
    t->stat_pages = pages < 1 ? 1 : pages;
    t->stat_pk_min = c.have ? c.pk_min : 0;
    t->stat_pk_max = c.have ? c.pk_max : 0;
    t->stat_valid = 1;
}

/* PK 범위 조건이 잡을 행 수 추정 (선택도 = 범위가 [min,max]에서 차지하는 비율) */
static int64_t est_pk_range_rows(const Table *t, CmpOp op, long v) {
    int64_t rows = t->stat_rows;
    if (rows <= 0) return 0;
    double lo = (double)t->stat_pk_min, hi = (double)t->stat_pk_max, span = hi - lo;
    double f;
    if (span <= 0) {
        f = 1.0;
    } else if (op == CMP_GT || op == CMP_GE) {
        f = (hi - (double)v) / span;
    } else { /* CMP_LT, CMP_LE */
        f = ((double)v - lo) / span;
    }
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    int64_t est = (int64_t)(rows * f + 0.5);
    if (est < 1) est = 1;
    if (est > rows) est = rows;
    return est;
}

/* 비용 모델(단순): 순차 스캔 = 페이지 수(순차 I/O), 인덱스 범위 = 1(트리 하강) +
 * 매칭 행수(행마다 랜덤 힙 페치 ≈ 페이지 1개). "페치할 행이 페이지 수보다 많으면
 * 순차가 이긴다"는 PostgreSQL random_page_cost 직관의 축소판. */
static double cost_seq(const Table *t) { return (double)t->stat_pages; }
static double cost_idx_range(int64_t est_rows) { return 1.0 + (double)est_rows; }

/* PK 범위에서 인덱스 vs 순차 선택. 통계 없으면 옛 규칙(인덱스) 유지.
 * est_rows/idx_cost/seq_cost를 채운다(EXPLAIN 표시용). 반환 1=인덱스, 0=순차. */
static int choose_pk_range(const Table *t, CmpOp op, long v,
                           int64_t *est_rows, double *idx_cost, double *seq_cost) {
    if (!t->stat_valid) {
        *est_rows = -1; *idx_cost = -1; *seq_cost = -1;
        return 1; /* 통계 없음 -> 옛 규칙대로 인덱스 */
    }
    int64_t er = est_pk_range_rows(t, op, v);
    *est_rows = er;
    *idx_cost = cost_idx_range(er);
    *seq_cost = cost_seq(t);
    return *idx_cost < *seq_cost ? 1 : 0;
}

static int exec_analyze(Database *db, const AnalyzeStmt *a, FILE *out) {
    if (a->table[0]) {
        Table *t = find_table(db, a->table);
        if (!t) {
            fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", a->table);
            return -1;
        }
        analyze_table(db, t);
        fprintf(out, "ANALYZE %s: 행 %lld · 페이지 %lld · PK 범위 [%lld, %lld]\n",
                t->schema.table, (long long)t->stat_rows, (long long)t->stat_pages,
                (long long)t->stat_pk_min, (long long)t->stat_pk_max);
    } else {
        for (int i = 0; i < db->num_tables; i++) analyze_table(db, &db->tables[i]);
        fprintf(out, "ANALYZE: %d개 테이블 통계 갱신\n", db->num_tables);
    }
    catalog_write(db); /* 통계 영속화 */
    return 0;
}

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

    /* PK 범위면 비용으로 인덱스/순차를 고른다(exec_select와 동일 결정). */
    int pk_range = can_index && pk_cond &&
                   (c0->op == CMP_LT || c0->op == CMP_GT || c0->op == CMP_LE || c0->op == CMP_GE);
    int64_t er = -1; double ic = -1, sc = -1;
    int range_use_idx = pk_range ? choose_pk_range(t, c0->op, c0->val.int_val, &er, &ic, &sc) : 0;

    xindent(out, ind);
    if (can_index && pk_cond && c0->op == CMP_EQ) {
        fprintf(out, "Index Point Lookup on %s using %s  (%s = %ld)%s\n", tname, pkcol, pkcol,
                c0->val.int_val, t->stat_valid ? "  rows=1 cost=1" : "");
    } else if (pk_range && range_use_idx) {
        fprintf(out, "Index Range Scan on %s using %s  (%s %s %ld)", tname, pkcol, pkcol,
                xop_str(c0->op), c0->val.int_val);
        if (t->stat_valid) fprintf(out, "  rows=%lld cost=%.0f", (long long)er, ic);
        fprintf(out, "\n");
    } else if (pk_range && !range_use_idx) {
        /* 비용상 순차가 더 싸다 — 규칙이라면 인덱스를 썼을 자리 */
        fprintf(out, "Seq Scan on %s  (filter: %s %s %ld)  rows=%lld cost=%.0f  [비용 기반: 인덱스보다 쌈]\n",
                tname, pkcol, xop_str(c0->op), c0->val.int_val, (long long)er, sc);
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

/* 인덱스 NLJ용: PK로 '보이는' 버전 하나를 찾아 rec에 담는다(다중 버전 후보를 훑음). */
typedef struct {
    Table *t;
    uint8_t *rec;
    int found;
} PkVisCtx;

static int pk_vis_visit(bkey_t key, bval_t val, void *ctx_) {
    (void)key;
    PkVisCtx *c = ctx_;
    uint16_t len;
    if (heap_get(&c->t->heap, rid_decode(val), c->rec, &len) == 0 &&
        rec_visible(c->t->owner, c->rec)) {
        c->found = 1;
        return 1; /* 첫 '보이는' 버전에서 멈춤 (논리적으로 PK당 하나) */
    }
    return 0;
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
    if (!rec_visible(m->tabs[level]->owner, rec)) {
        return 0; /* 조인도 보이는 버전만 짝을 짓는다 */
    }
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
            uint8_t recbuf[PAGE_SIZE];
            PkVisCtx pv = {m->tabs[level], recbuf, 0};
            btree_find_all(&m->tabs[level]->index, k->int_val, pk_vis_visit, &pv);
            if (pv.found) { /* 버전 후보들 중 '보이는' 것을 골랐다 */
                decode_row(&m->tabs[level]->schema, recbuf, m->rows[level]);
                m->matched[level] = 1;
                r = mjoin_descend(m, level + 1);
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
    Database *db; /* MVCC 가시성 판정용 */
} SubCtx;

static int sub_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)rid;
    (void)len;
    SubCtx *c = ctx_;
    if (!rec_visible(c->db, rec)) {
        return 0; /* 서브쿼리 결과 집합도 보이는 버전으로만 */
    }
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
    SubCtx c = {&t->schema, tname, &sub->where, col, set, SELECT_MAX_ROWS, 0, db};
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
 * 재검사가 필요한 이유 — 지워진/미커밋 버전은 가시성 게이트가 거르고, UPDATE로 값이 바뀌어
 * 남은 stale 인덱스 항목은 cond 재평가로 거른다(인덱스는 MVCC를 모른다). */
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
        return 0; /* 사라진 슬롯 */
    }
    if (!rec_visible(s->t->owner, recbuf)) {
        return 0; /* 지워진(xmax)/미커밋 버전 */
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

    /* PK 범위면 비용으로 인덱스/순차 결정 (explain_single과 동일). */
    int pk_range = pk_cond && (c0->op == CMP_GT || c0->op == CMP_GE || c0->op == CMP_LT ||
                               c0->op == CMP_LE);
    int64_t er_; double ic_, sc_;
    int range_use_idx = pk_range && choose_pk_range(t, c0->op, c0->val.int_val, &er_, &ic_, &sc_);

    if (pk_cond && c0->op == CMP_EQ) {
        /* = -> 점 조회 O(log n). 같은 키의 버전 후보들 중 '보이는' 것만. */
        db->used_index = 1;
        PointCtx pc = {t, out, 0};
        pidx_find_all(t, c0->val.int_val, point_visit, &pc);
        count = pc.count;
    } else if (pk_range && range_use_idx) {
        /* <, >, <=, >= 이고 비용상 인덱스가 쌈 -> 인덱스 범위 스캔 (리프 체인) */
        db->used_index = 1;
        RangeCtx rc = {t, c0->val.int_val, c0->op, out, 0};
        pidx_range(t, c0->op, c0->val.int_val, range_visit, &rc);
        count = rc.count;
    } else if (sec_index_for(t, tname, c0, pk_cond) >= 0) {
        /* 비PK 컬럼 = 값 -> 보조 인덱스 find_all + heap_get + WHERE 재검사 */
        db->used_index = 1;
        int sk = sec_index_for(t, tname, c0, pk_cond);
        SecScanCtx sc = {t, tname, &sel->where, out, 0};
        btree_find_all(&t->sec[sk].tree, c0->val.int_val, sec_scan_visit, &sc);
        count = sc.count;
    } else if (parallel_fullscan(db, t, tname, &sel->where, out, &count)) {
        /* 큰 테이블 + 서브쿼리 없는 WHERE -> 병렬 풀스캔(37편). count 채워짐. */
    } else {
        /* 그 외(WHERE 없음/복합/비PK/TEXT 비교) -> (직렬) 풀 스캔 */
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
    Database *db; /* MVCC 가시성 판정용 */
} CollectCtx;

static int collect_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)len;
    CollectCtx *c = ctx_;
    if (!rec_visible(c->db, rec)) {
        return 0; /* 이미 지워진(xmax)/안 보이는 버전은 DML 대상이 아니다 */
    }
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
    table_begin_write(db, t, db->cur_txn);
    CollectCtx ctx = {.count = 0, .schema = &t->schema, .where = &d->where, .db = db};
    heap_scan(&t->heap, collect_visit, &ctx);
    for (int i = 0; i < ctx.count; i++) {
        /* PostgreSQL식 논리 삭제: 행을 지우지 않고 xmax만 새긴다. 커밋되면 가시성
         * 게이트가 숨기고, 아보트하면 xmax가 무효라 되살아난다(MVCC 롤백). */
        stamp_xmax(t, ctx.rids[i], db->cur_txn);
    }
    /* 인덱스 항목은 남겨둬도 무해하다: 가리키는 행이 안 보이는 버전이면 가시성
     * 게이트가 걸러 결과에서 자동으로 빠진다. (B+Tree 삭제는 VACUUM에서.) */
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

    table_begin_write(db, t, db->cur_txn);
    CollectCtx ctx = {.count = 0, .schema = &t->schema, .where = &u->where, .db = db};
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

        /* MVCC식 UPDATE = 옛 버전에 xmax + 새 버전 추가(새 RID). 옛 버전은 힙에
         * 남아 롤백 시 되살아난다 — PostgreSQL이 UPDATE를 다루는 방식 그대로. */
        uint8_t newbuf[PAGE_SIZE];
        uint16_t newlen;
        if (encode_row(&t->schema, row, t->schema.num_columns, db->cur_txn, 0, newbuf, &newlen) !=
            0) {
            continue;
        }
        stamp_xmax(t, ctx.rids[i], db->cur_txn);
        RID newrid;
        if (heap_insert(&t->heap, newbuf, newlen, &newrid) != 0) {
            continue;
        }
        /* 새 버전의 인덱스 항목을 '추가'한다(덮어쓰기 아님) — 옛 버전의 항목은 남아서
         * 스냅샷 reader가 옛 버전을 인덱스로 찾을 수 있어야 한다(다중 버전 인덱스).
         * 죽은 버전의 항목은 나중에 VACUUM이 짝(키,RID)으로 지운다. */
        if (t->has_index && row[0].type == VAL_INT) {
            pidx_insert(t, row[0].int_val, newrid);
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

/* ------------- 실행기: VACUUM -------------
 * 16편이 만든 문제를 치운다: DELETE/UPDATE가 xmax만 새기니 죽은 버전이 힙에 쌓인다.
 * VACUUM은 ① 죽은 버전의 인덱스 항목을 지우고(드디어 필요해진 B+Tree 삭제 — lazy),
 * ② 힙 슬롯을 비워 페이지를 compaction하고, ③ 꼬리가 전부 빈 페이지면 파일을 자른다
 * (PostgreSQL의 조건부 truncate와 같은 결). PG처럼 트랜잭션 안에서는 못 돈다. */

/* 죽은 버전인가 — 커밋된 xmax가 찍혀 이제 아무 트랜잭션에게도 안 보일 옛 버전.
 * (단일 트랜잭션 + 물리 롤백이라 '아보트된 xmin' 행은 디스크에 없고, VACUUM은
 * 트랜잭션 밖에서만 돌므로 미커밋 xmax도 없다.) */
static int rec_dead(Database *db, const void *rec) {
    int32_t xmax = db_rec_xmax(rec);
    return xmax != 0 && txn_committed_view(db, xmax);
}

typedef struct {
    RID rid;
    bkey_t pk;
    int has_pk;
    bkey_t sk[DB_MAX_SEC_IDX];
    int has_sk[DB_MAX_SEC_IDX];
} DeadRef;

typedef struct {
    Database *db;
    Table *t;
    DeadRef *dead;
    int n, cap;
    int oom;
} VacCtx;

/* 죽은 버전을 모은다. 인덱스 항목을 지우려면 키 값이 필요하므로 여기서 미리 decode. */
static int vacuum_collect_visit(RID rid, const void *rec, uint16_t len, void *ctx_) {
    (void)len;
    VacCtx *c = ctx_;
    if (!rec_dead(c->db, rec)) {
        return 0;
    }
    if (c->n >= c->cap) {
        int cap = c->cap ? c->cap * 2 : 256;
        DeadRef *p = realloc(c->dead, sizeof(DeadRef) * (size_t)cap);
        if (!p) {
            c->oom = 1;
            return 1; /* 스캔 중단 */
        }
        c->dead = p;
        c->cap = cap;
    }
    Value row[SQL_MAX_COLS];
    decode_row(&c->t->schema, rec, row);
    DeadRef *e = &c->dead[c->n++];
    e->rid = rid;
    e->has_pk = (c->t->has_index && row[0].type == VAL_INT);
    e->pk = e->has_pk ? row[0].int_val : 0;
    for (int k = 0; k < c->t->num_sec; k++) {
        int col = c->t->sec[k].col;
        e->has_sk[k] = (row[col].type == VAL_INT);
        e->sk[k] = e->has_sk[k] ? row[col].int_val : 0;
    }
    return 0;
}

/* 페이지에 살아있는 슬롯이 하나도 없나(꼬리 truncate 판정). */
static int page_is_empty(const void *pg) {
    uint16_t n = slotpage_num_slots(pg);
    for (uint16_t s = 0; s < n; s++) {
        const void *r;
        uint16_t l;
        if (slotpage_get(pg, s, &r, &l) == 0) {
            return 0;
        }
    }
    return 1;
}

static int vacuum_table(Database *db, Table *t, FILE *out) {
    table_begin_write(db, t, db->cur_txn); /* 청소도 WAL로 원자 커밋 */
    /* 1) 죽은 버전 수집 */
    VacCtx c = {.db = db, .t = t, .dead = NULL, .n = 0, .cap = 0, .oom = 0};
    heap_scan(&t->heap, vacuum_collect_visit, &c);
    if (c.oom) {
        free(c.dead);
        fprintf(out, "ERROR: 메모리 부족\n");
        return -1;
    }

    /* 2) 죽은 버전을 가리키는 인덱스 항목 제거.
     * PK도 이제 버전마다 항목이 있으므로(다중 버전 인덱스), (키, 죽은 RID) 짝만
     * 정확히 지우면 된다 — 살아있는 새 버전의 항목은 자기 짝이 따로 있어 안전하다. */
    for (int i = 0; i < c.n; i++) {
        DeadRef *e = &c.dead[i];
        if (e->has_pk) {
            pidx_delete(t, e->pk, e->rid);
        }
        for (int k = 0; k < t->num_sec; k++) {
            if (e->has_sk[k]) {
                btree_delete_val(&t->sec[k].tree, e->sk[k], rid_encode(e->rid));
            }
        }
    }

    /* 3) 힙 슬롯 비우기 — 이제야 heap_delete(tombstone)가 제 일을 찾았다:
     * DELETE의 의미론이 아니라 VACUUM의 청소 도구로. 그리고 죽은 행이 있던
     * 페이지만 compaction해 레코드 공간을 회수한다(슬롯 번호 = RID는 불변). */
    for (int i = 0; i < c.n; i++) {
        heap_delete(&t->heap, c.dead[i].rid);
    }
    for (int i = 0; i < c.n; i++) {
        page_id_t pid = c.dead[i].rid.page_id;
        if (i > 0 && pid == c.dead[i - 1].rid.page_id) {
            continue; /* 같은 페이지는 한 번만 (수집이 페이지 순서라 인접해 있다) */
        }
        void *pg = bufpool_fetch(t->bp, pid);
        if (pg) {
            slotpage_compact(pg);
            bufpool_unpin(t->bp, pid, 1);
        }
    }

    /* 4) 꼬리의 빈 페이지 잘라내기 (PG의 조건부 truncate — 가운데 빈 페이지는 남는다).
     * 파일 축소는 WAL에 안 적히지만, 잘린 페이지엔 죽은 버전만 있었으므로 커밋 전에
     * 크래시해도 사용자 가시 상태는 동일하다(그 행들은 원래 아무에게도 안 보였다). */
    uint64_t np = t->wal.data.num_pages;
    uint64_t new_np = np;
    while (new_np > t->heap.first_page) {
        void *pg = bufpool_fetch(t->bp, new_np - 1);
        if (!pg) {
            break;
        }
        int empty = page_is_empty(pg);
        bufpool_unpin(t->bp, new_np - 1, 0);
        if (!empty) {
            break;
        }
        new_np--;
    }
    if (new_np < np) {
        bufpool_invalidate_from(t->bp, new_np); /* 잘린 페이지의 프레임이 파일을 되살리지 않게 */
        pager_truncate(&t->wal.data, new_np);
    }

    fprintf(out, "VACUUM %s: 죽은 버전 %d개 회수, 빈 페이지 %llu개 반환\n", t->schema.table,
            c.n, (unsigned long long)(np - new_np));
    free(c.dead);
    return 0;
}

static int exec_vacuum(Database *db, const VacuumStmt *v, FILE *out) {
    /* PostgreSQL과 동일: VACUUM cannot run inside a transaction block.
     * 다중 세션에선 더 강하게 — '어느 세션이든' 트랜잭션이 열려 있으면 거부한다.
     * (죽은 버전 판정 rec_dead가 "살아있는 스냅샷이 없다"를 전제하기 때문.) */
    for (int i = 0; i < DB_MAX_SESSIONS; i++) {
        if (db->sessions[i].in_txn) {
            fprintf(out, "ERROR: VACUUM은 트랜잭션 안(다른 세션 포함)에서 실행할 수 없습니다\n");
            return -1;
        }
    }
    if (v->table[0]) {
        Table *t = find_table(db, v->table);
        if (!t) {
            fprintf(out, "ERROR: 그런 테이블이 없습니다 (%s)\n", v->table);
            return -1;
        }
        return vacuum_table(db, t, out);
    }
    for (int i = 0; i < db->num_tables; i++) {
        if (vacuum_table(db, &db->tables[i], out) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ------------- 트랜잭션 (다중 세션) -------------
 * 세션마다 트랜잭션 핸들이 하나씩 있고, `SESSION n`으로 갈아타며 여러 트랜잭션을
 * 인터리브한다. 핵심 규칙:
 *   - 읽기는 락을 안 잡는다 — reader의 격리는 MVCC 가시성(스냅샷)이 맡는다.
 *     그래서 reader가 writer를 안 막고, writer도 reader를 안 막는다.
 *   - 쓰기는 테이블 X락(strict 2PL, 커밋까지 보유) — 같은 테이블의 두 번째 writer는
 *     즉시 거부된다(first-updater-wins, 테이블 단위).
 *   - 그 X락이 "테이블당 writer 하나"를 보장하므로, 테이블별 WAL은 여전히 한 번에
 *     한 트랜잭션만 본다 — 14·15편의 steal/undo/no-force 복구가 무수정으로 성립한다.
 * 테이블의 WAL 트랜잭션은 BEGIN이 아니라 "그 테이블에 처음 쓸 때" 열린다(lazy).
 * COMMIT/ROLLBACK은 자기가 쓴(writer_txn==나) 테이블만 확정/되돌린다.
 * DDL인 CREATE는 즉시 반영되며 트랜잭션에 묶이지 않는다. */

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
        return wal_commit(wal); /* 로그+마커+fsync(no-force) */
    }
    return 0; /* 바뀐 게 없으면 로그 쓸 것도 없다 */
}

/* 트랜잭션 txn이 테이블 t에 처음 쓰기 전에 부른다 — 이 테이블의 WAL 트랜잭션을 열고
 * no-steal(+steal 핸들러)을 켠다. 호출 전에 X락을 이미 쥐고 있어야 한다. */
static void table_begin_write(Database *db, Table *t, int txn) {
    (void)db;
    if (t->writer_txn == txn) {
        return; /* 이 트랜잭션이 이미 쓰는 중 */
    }
    t->writer_txn = txn;
    bufpool_set_no_steal(t->bp, 1);
    bufpool_set_steal_handler(t->bp, wal_steal_cb, &t->wal);
    wal_begin(&t->wal);
    t->txn_data_pages = t->wal.data.num_pages;
    if (t->has_index && t->index_kind == 0) { /* LSM 인덱스는 자체 WAL/롤백에 미참여 */
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

/* txn이 쓴 테이블들만 WAL로 커밋하고 쓰기 상태를 정리한다. */
static void txn_commit_tables(Database *db, int txn) {
    for (int i = 0; i < db->num_tables; i++) {
        Table *t = &db->tables[i];
        if (t->writer_txn != txn) {
            continue; /* 남의(또는 아무도 안 쓰는) 테이블은 건드리지 않는다 */
        }
        wal_flush_commit(t->bp, &t->wal); /* 데이터: WAL로 원자 커밋 */
        bufpool_set_no_steal(t->bp, 0);
        bufpool_set_steal_handler(t->bp, NULL, NULL);
        if (t->has_index && t->index_kind == 0) { /* LSM은 커밋할 인덱스 WAL이 없다(라이브 반영) */
            wal_flush_commit(t->index.bp, &t->index.wal); /* 인덱스: 인덱스 WAL로 */
            bufpool_set_no_steal(t->index.bp, 0);
            bufpool_set_steal_handler(t->index.bp, NULL, NULL);
        }
        for (int k = 0; k < t->num_sec; k++) {
            wal_flush_commit(t->sec[k].tree.bp, &t->sec[k].tree.wal);
            bufpool_set_no_steal(t->sec[k].tree.bp, 0);
            bufpool_set_steal_handler(t->sec[k].tree.bp, NULL, NULL);
        }
        t->writer_txn = 0;
    }
}

/* txn이 쓴 테이블들만 되돌린다(steal은 before-image로, 나머지는 discard+truncate). */
static void txn_abort_tables(Database *db, int txn) {
    for (int i = 0; i < db->num_tables; i++) {
        Table *t = &db->tables[i];
        if (t->writer_txn != txn) {
            continue;
        }
        /* steal이 있었으면 로그의 before-image로 디스크를 먼저 원복(+새 페이지 truncate).
         * 그다음 풀 전체를 무효화해 steal로 clean 처리된 미커밋 프레임까지 버린다. */
        wal_undo(&t->wal);
        bufpool_invalidate_all(t->bp);
        pager_truncate(&t->wal.data, t->txn_data_pages); /* non-steal 경로의 새 페이지 제거 */
        bufpool_set_no_steal(t->bp, 0);
        bufpool_set_steal_handler(t->bp, NULL, NULL);
        if (t->has_index && t->index_kind == 0) {
            wal_undo(&t->index.wal);
            bufpool_invalidate_all(t->index.bp);
            pager_truncate(&t->index.wal.data, t->txn_index_pages);
            btree_reload_root(&t->index); /* 루트가 분할로 바뀌었을 수 있으니 다시 읽는다 */
            bufpool_set_no_steal(t->index.bp, 0);
            bufpool_set_steal_handler(t->index.bp, NULL, NULL);
        } else if (t->has_index && t->index_kind == 1) {
            /* LSM 인덱스는 롤백되지 않으니, 이미 롤백된 heap에서 통째로 재구축해
             * 동기화한다(중단 txn이 남긴 dangling 항목 제거). */
            lsm_pk_rebuild(t);
        }
        for (int k = 0; k < t->num_sec; k++) {
            wal_undo(&t->sec[k].tree.wal);
            bufpool_invalidate_all(t->sec[k].tree.bp);
            pager_truncate(&t->sec[k].tree.wal.data, t->sec[k].txn_pages);
            btree_reload_root(&t->sec[k].tree);
            bufpool_set_no_steal(t->sec[k].tree.bp, 0);
            bufpool_set_steal_handler(t->sec[k].tree.bp, NULL, NULL);
        }
        t->writer_txn = 0;
    }
}

static int exec_begin(Database *db, FILE *out) {
    DbSession *s = &db->sessions[db->cur_session];
    if (s->in_txn) {
        fprintf(out, "ERROR: 이미 트랜잭션 중입니다\n");
        return -1;
    }
    s->in_txn = 1;
    s->txn = db->next_txn++; /* 이 트랜잭션의 락 소유자 id · 행 xmin */
    txnlog_begin(&db->txnlog, s->txn);
    db->cur_txn = s->txn;
    /* 트랜잭션 시작 스냅샷: 이후에 발급될 id(>= snap_next)와, 지금 진행 중인 남의
     * 트랜잭션은 — 나중에 커밋해도 — 이 트랜잭션에겐 안 보인다(스냅샷 격리). */
    s->snap_next = db->next_txn;
    s->n_snap_inprog = 0;
    for (int i = 0; i < DB_MAX_SESSIONS; i++) {
        if (i != db->cur_session && db->sessions[i].in_txn) {
            s->snap_inprog[s->n_snap_inprog++] = db->sessions[i].txn;
        }
    }
    fprintf(out, "트랜잭션 시작\n");
    return 0;
}

static int exec_commit(Database *db, FILE *out) {
    DbSession *s = &db->sessions[db->cur_session];
    if (!s->in_txn) {
        fprintf(out, "ERROR: 트랜잭션이 없습니다\n");
        return -1;
    }
    txn_commit_tables(db, s->txn);           /* 내가 쓴 테이블만 확정 */
    txnlog_commit(&db->txnlog, s->txn);      /* MVCC: 이 트랜잭션을 커밋 표시 */
    lock_release_all(&db->lm, s->txn);       /* 2PL: 끝에서 한꺼번에 푼다 */
    s->in_txn = 0;
    db->cur_txn = 0;
    fprintf(out, "커밋됨\n");
    return 0;
}

static int exec_rollback(Database *db, FILE *out) {
    DbSession *s = &db->sessions[db->cur_session];
    if (!s->in_txn) {
        fprintf(out, "ERROR: 트랜잭션이 없습니다\n");
        return -1;
    }
    txn_abort_tables(db, s->txn);           /* 내가 쓴 테이블만 되돌림 */
    txnlog_abort(&db->txnlog, s->txn);      /* MVCC: 이 트랜잭션을 아보트 표시 */
    lock_release_all(&db->lm, s->txn);      /* 2PL: 끝에서 한꺼번에 푼다 */
    s->in_txn = 0;
    db->cur_txn = 0;
    fprintf(out, "롤백됨\n");
    return 0;
}

/* ------------- 공개 API ------------- */

int db_open(Database *db, const char *path) {
    snprintf(db->path, sizeof(db->path), "%s", path);
    db->num_tables = 0;
    db->used_index = 0;
    memset(db->sessions, 0, sizeof(db->sessions));
    db->cur_session = 0;
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
                /* 옵티마이저 통계 복원 (없던 옛 카탈로그면 fread 실패 -> stat_valid=0 유지) */
                int32_t sv = 0;
                if (fread(&sv, sizeof(int32_t), 1, f) == 1) {
                    t->stat_valid = sv;
                    if (fread(&t->stat_rows, sizeof(int64_t), 1, f) != 1) t->stat_valid = 0;
                    if (fread(&t->stat_pages, sizeof(int64_t), 1, f) != 1) t->stat_valid = 0;
                    if (fread(&t->stat_pk_min, sizeof(int64_t), 1, f) != 1) t->stat_valid = 0;
                    if (fread(&t->stat_pk_max, sizeof(int64_t), 1, f) != 1) t->stat_valid = 0;
                }
                t->owner = db; /* 읽기 경로의 가시성 판정용 역참조 */
                t->writer_txn = 0;
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
    /* 열린 트랜잭션은 롤백하고 닫는다 — 커넥션이 끊기면 abort하는 진짜 DB와 같다.
     * (안 그러면 닫힘 flush가 미커밋 dirty 페이지를 디스크에 쓰고, next_txn 영속화가
     * 그 트랜잭션을 재오픈 시 '커밋'으로 승격시켜 미커밋이 유출된다.) */
    for (int i = 0; i < DB_MAX_SESSIONS; i++) {
        if (db->sessions[i].in_txn) {
            txn_abort_tables(db, db->sessions[i].txn);
            txnlog_abort(&db->txnlog, db->sessions[i].txn);
            lock_release_all(&db->lm, db->sessions[i].txn);
            db->sessions[i].in_txn = 0;
        }
    }
    catalog_write(db); /* MVCC: 이번 실행의 최종 next_txn을 영속화(재오픈 시 옛 행 가시성) */
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

/* 쓰기 문장이 건드리는 테이블에 X락. 충돌이면 -1 = first-updater-wins(테이블 단위).
 * SELECT는 락을 안 잡는다 — 11편의 S락은 MVCC 가시성(스냅샷)으로 대체됐다:
 * reader는 writer의 미커밋 버전을 '가시성으로' 못 볼 뿐, 막히지 않는다. */
static int acquire_stmt_locks(Database *db, const Statement *st, int txn, FILE *out) {
    switch (st->type) {
        case STMT_INSERT:
            return lock_one(db, st->insert.table, LOCK_X, txn, out);
        case STMT_DELETE:
            return lock_one(db, st->del.table, LOCK_X, txn, out);
        case STMT_UPDATE:
            return lock_one(db, st->upd.table, LOCK_X, txn, out);
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
    /* 쓰기 문장만 락을 건다(테이블 X, strict 2PL — 커밋까지 보유). 읽기는 락이 없다:
     * reader의 격리는 MVCC 가시성(스냅샷)이 맡는다. 충돌이면(같은 테이블의 두 번째
     * writer) 단일 스레드라 "블록" 대신 문장을 즉시 거부한다 = first-updater-wins.
     * DDL인 CREATE는 트랜잭션에 묶이지 않고 항상 즉시(autocommit으로) 반영한다. */
    DbSession *sess = &db->sessions[db->cur_session];
    int is_write = (st.type == STMT_INSERT || st.type == STMT_DELETE ||
                    st.type == STMT_UPDATE || st.type == STMT_VACUUM ||
                    st.type == STMT_CREATE);
    int autocommit = 0;
    int stmt_txn = 0;
    if (is_write) {
        autocommit = !sess->in_txn || st.type == STMT_CREATE;
        if (autocommit) {
            stmt_txn = db->next_txn++; /* 이 문장이 곧 한 트랜잭션 (행 xmin에 쓰임) */
            txnlog_begin(&db->txnlog, stmt_txn);
        } else {
            stmt_txn = sess->txn;
        }
        db->cur_txn = stmt_txn;
        if (acquire_stmt_locks(db, &st, stmt_txn, out) != 0) {
            if (autocommit) {
                txnlog_abort(&db->txnlog, stmt_txn);
                lock_release_all(&db->lm, stmt_txn);
            }
            db->cur_txn = sess->in_txn ? sess->txn : 0;
            statement_free(&st);
            return -1;
        }
    } else {
        /* 읽기(SELECT)·트랜잭션 제어: 락도, 새 txn id도 없다. 가시성 기준만 세운다. */
        db->cur_txn = sess->in_txn ? sess->txn : 0;
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
        case STMT_VACUUM: rc = exec_vacuum(db, &st.vac, out); break;
        case STMT_ANALYZE: rc = exec_analyze(db, &st.analyze, out); break;
        case STMT_SESSION: /* 세션 전환 — 다중 트랜잭션 인터리브의 스위치 */
            if (st.sess.n < 0 || st.sess.n >= DB_MAX_SESSIONS) {
                fprintf(out, "ERROR: 세션 번호는 0..%d 입니다\n", DB_MAX_SESSIONS - 1);
                rc = -1;
            } else {
                db->cur_session = st.sess.n;
                fprintf(out, "세션 %d\n", st.sess.n);
                rc = 0;
            }
            break;
        default: rc = -1;
    }
    if (autocommit) {
        /* 이 문장 하나가 곧 한 트랜잭션 — 자기가 쓴 테이블만 커밋/롤백한다. */
        if (rc == 0) {
            txn_commit_tables(db, stmt_txn);
            txnlog_commit(&db->txnlog, stmt_txn);
        } else {
            txn_abort_tables(db, stmt_txn);
            txnlog_abort(&db->txnlog, stmt_txn);
        }
        lock_release_all(&db->lm, stmt_txn);
        db->cur_txn = sess->in_txn ? sess->txn : 0;
    }
    statement_free(&st); /* 서브쿼리·IN 집합 해제 */
    return rc;
}
