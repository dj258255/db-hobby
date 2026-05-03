#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * VACUUM (A2) — 16편(DELETE=xmax)이 쌓아 놓은 죽은 버전을 치우는 청소부.
 *   ① 죽은 버전의 힙 슬롯 회수 + 페이지 compaction (RID 불변)
 *   ② 죽은 버전을 가리키던 인덱스 항목 제거 (B+Tree lazy 삭제)
 *      — 단, PK 인덱스가 살아있는 새 버전(UPDATE/재삽입)을 가리키면 보존
 *   ③ 꼬리의 빈 페이지는 파일 truncate (PG의 조건부 truncate)
 *   ④ 트랜잭션 안에서는 거부 (PostgreSQL과 동일)
 */

static int failures = 0;

#define CHECK(cond, msg)                       \
    do {                                       \
        if (cond) {                            \
            printf("  ok   %s\n", msg);        \
        } else {                               \
            printf("  FAIL %s\n", msg);        \
            failures++;                        \
        }                                      \
    } while (0)

static char *run(Database *db, const char *sql) {
    char *buf = NULL;
    size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    db_exec(db, sql, f);
    fclose(f);
    return buf;
}

static void cleanup(const char *base) {
    char p[700];
    const char *ext[] = {"t.tbl", "t.idx", "t.wal", "t.idx.wal", "t.tn.idx", "t.tn.idx.wal"};
    unlink(base);
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        snprintf(p, sizeof(p), "%s.%s", base, ext[i]);
        unlink(p);
    }
}

static Table *table_of(Database *db, const char *tbl) {
    for (int i = 0; i < db->num_tables; i++) {
        if (strcmp(db->tables[i].schema.table, tbl) == 0) {
            return &db->tables[i];
        }
    }
    return NULL;
}

static int phys_count_visit(RID rid, const void *rec, uint16_t len, void *ctx) {
    (void)rid; (void)rec; (void)len;
    (*(int *)ctx)++;
    return 0;
}
static int phys_rows(Database *db, const char *tbl) {
    Table *t = table_of(db, tbl);
    if (!t) return -1;
    int n = 0;
    heap_scan(&t->heap, phys_count_visit, &n);
    return n;
}

#define PAYLOAD "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" /* 120자 — 페이지를 빨리 채운다 */

int main(void) {
    const char *path = "build/test_vacuum.db";
    char sql[512];
    cleanup(path);

    Database db;
    db_open(&db, path);
    char *o;

    /* --- A. 기본: 죽은 버전 회수, 살아있는 행·인덱스는 그대로 --- */
    o = run(&db, "CREATE TABLE t (id INT, n INT, v TEXT)"); free(o);
    o = run(&db, "CREATE INDEX tn ON t(n)"); free(o);
    for (int i = 1; i <= 10; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, %d, 'r%d')", i, i * 10, i);
        o = run(&db, sql); free(o);
    }
    o = run(&db, "DELETE FROM t WHERE id > 5"); free(o);
    CHECK(phys_rows(&db, "t") == 10, "VACUUM 전: 죽은 버전 5개가 힙에 남아 있다 (10행)");

    o = run(&db, "VACUUM t");
    CHECK(strstr(o, "죽은 버전 5개") != NULL, "VACUUM이 죽은 버전 5개를 회수");
    free(o);
    CHECK(phys_rows(&db, "t") == 5, "VACUUM 후: 힙에 살아있는 5행만");

    o = run(&db, "SELECT COUNT(*) FROM t");
    CHECK(strstr(o, "5") != NULL, "가시 행 수는 그대로 5");
    free(o);
    o = run(&db, "SELECT * FROM t WHERE id = 3");
    CHECK(strstr(o, "r3") && strstr(o, "인덱스 사용"), "PK 조회 정상 (살아있는 인덱스 항목 보존)");
    free(o);
    o = run(&db, "SELECT * FROM t WHERE n = 30");
    CHECK(strstr(o, "r3") && strstr(o, "인덱스 사용"), "보조 인덱스 조회 정상");
    free(o);
    o = run(&db, "SELECT * FROM t WHERE id = 7");
    CHECK(!strstr(o, "r7"), "지운 행은 여전히 안 보임 (인덱스 항목도 제거됨)");
    free(o);

    o = run(&db, "VACUUM t");
    CHECK(strstr(o, "죽은 버전 0개") != NULL, "VACUUM은 멱등 — 두 번째엔 치울 게 없다");
    free(o);

    /* --- B. UPDATE bloat -> VACUUM. PK 인덱스는 새 버전을 가리키므로 보존돼야 한다 --- */
    for (int i = 0; i < 5; i++) { /* 같은 행을 5번 고쳐 옛 버전 5개를 쌓는다 */
        snprintf(sql, sizeof(sql), "UPDATE t SET v = 'v%d' WHERE id = 1", i);
        o = run(&db, sql); free(o);
    }
    CHECK(phys_rows(&db, "t") == 10, "UPDATE 5번 -> 옛 버전 5개 누적 (bloat)");
    o = run(&db, "VACUUM t");
    CHECK(strstr(o, "죽은 버전 5개") != NULL, "VACUUM이 UPDATE의 옛 버전들을 회수");
    free(o);
    o = run(&db, "SELECT * FROM t WHERE id = 1");
    CHECK(strstr(o, "v4") && strstr(o, "인덱스 사용"),
          "최신 버전은 PK로 정상 조회 (같은 키의 살아있는 항목은 안 지움)");
    free(o);

    /* --- C. 지운 PK를 VACUUM 후 재삽입 --- */
    o = run(&db, "INSERT INTO t VALUES (7, 70, 'r7-again')"); free(o);
    o = run(&db, "SELECT * FROM t WHERE id = 7");
    CHECK(strstr(o, "r7-again"), "VACUUM으로 항목이 지워진 PK를 재삽입 -> 정상 조회");
    free(o);

    /* --- D. 트랜잭션 안에서는 거부 --- */
    o = run(&db, "BEGIN"); free(o);
    o = run(&db, "VACUUM t");
    CHECK(strstr(o, "ERROR") != NULL, "BEGIN 안에서 VACUUM -> 거부 (PostgreSQL과 동일)");
    free(o);
    o = run(&db, "ROLLBACK"); free(o);

    /* --- E. 꼬리 페이지 truncate: 여러 페이지 채우고 전부 지우면 파일이 준다 --- */
    for (int i = 100; i < 160; i++) { /* 큰 payload로 페이지 여러 장을 만든다 */
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, %d, '%s')", i, i, PAYLOAD);
        o = run(&db, sql); free(o);
    }
    uint64_t np_full = table_of(&db, "t")->wal.data.num_pages;
    o = run(&db, "DELETE FROM t WHERE id >= 100"); free(o);
    o = run(&db, "VACUUM t"); free(o);
    uint64_t np_after = table_of(&db, "t")->wal.data.num_pages;
    CHECK(np_after < np_full, "꼬리의 빈 페이지가 잘려 파일이 줄었다 (조건부 truncate)");

    /* --- F. VACUUM 후 재오픈: 데이터·인덱스 온전 --- */
    db_close(&db);
    db_open(&db, path);
    o = run(&db, "SELECT COUNT(*) FROM t");
    CHECK(strstr(o, "6") != NULL, "재오픈 후 가시 행 6 (5 + 재삽입 1)");
    free(o);
    o = run(&db, "SELECT * FROM t WHERE n = 30");
    CHECK(strstr(o, "r3"), "재오픈 후 보조 인덱스 조회 정상");
    free(o);
    CHECK(phys_rows(&db, "t") == 6, "재오픈 후 힙도 살아있는 6행만");

    db_close(&db);
    cleanup(path);
    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
