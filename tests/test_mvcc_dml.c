#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * MVCC DML (A1-2c) — DELETE가 tombstone이 아니라 xmax를 새기고(논리 삭제),
 * '모든' 읽기 경로(풀스캔·PK 점조회·보조 인덱스·조인·집계·정렬)가 가시성 게이트로
 * 거른다는 것을 검증한다. UPDATE의 옛 버전도 xmax를 받는다.
 *
 * 핵심 성질:
 *   - DELETE 후에도 행은 힙에 물리적으로 남는다 (공간 회수는 VACUUM의 일)
 *   - DELETE를 ROLLBACK하면 행이 되살아난다 (xmax 무효화 = MVCC 롤백)
 *   - 재오픈 후에도 커밋된 DELETE는 숨겨진다 (committed_below 경유)
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
    const char *ext[] = {"t.tbl",  "t.idx",  "t.wal",  "t.idx.wal", "t.sn.idx", "t.sn.idx.wal",
                         "u.tbl",  "u.idx",  "u.wal",  "u.idx.wal"};
    unlink(base);
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        snprintf(p, sizeof(p), "%s.%s", base, ext[i]);
        unlink(p);
    }
}

/* 힙에 물리적으로 남은 레코드 수(가시성 무시) — "지워도 안 사라짐"의 증거 */
static int phys_count_visit(RID rid, const void *rec, uint16_t len, void *ctx) {
    (void)rid; (void)rec; (void)len;
    (*(int *)ctx)++;
    return 0;
}
static int phys_rows(Database *db, const char *tbl) {
    for (int i = 0; i < db->num_tables; i++) {
        if (strcmp(db->tables[i].schema.table, tbl) == 0) {
            int n = 0;
            heap_scan(&db->tables[i].heap, phys_count_visit, &n);
            return n;
        }
    }
    return -1;
}

int main(void) {
    const char *path = "build/test_mvcc_dml.db";
    cleanup(path);

    Database db;
    db_open(&db, path);
    char *o;

    o = run(&db, "CREATE TABLE t (id INT, n INT, v TEXT)"); free(o);
    o = run(&db, "CREATE INDEX t_n ON t(n)"); free(o); /* 보조 인덱스 경로용 */
    o = run(&db, "INSERT INTO t VALUES (1, 10, 'one')"); free(o);
    o = run(&db, "INSERT INTO t VALUES (2, 20, 'two')"); free(o);
    o = run(&db, "INSERT INTO t VALUES (3, 30, 'three')"); free(o);
    o = run(&db, "CREATE TABLE u (id INT, tid INT)"); free(o);
    o = run(&db, "INSERT INTO u VALUES (100, 2)"); free(o); /* 조인 상대 */

    /* --- A. DELETE는 모든 읽기 경로에서 숨는다 (그러나 물리적으론 남는다) --- */
    o = run(&db, "DELETE FROM t WHERE id = 2");
    CHECK(strstr(o, "1개 행 삭제됨") != NULL, "DELETE 1행");
    free(o);

    o = run(&db, "SELECT * FROM t");
    CHECK(!strstr(o, "two") && strstr(o, "one") && strstr(o, "three"),
          "풀스캔: 지운 행이 안 보임");
    free(o);

    o = run(&db, "SELECT * FROM t WHERE id = 2"); /* PK 점 조회 */
    CHECK(!strstr(o, "two") && strstr(o, "인덱스 사용"), "PK 점 조회: 안 보임 (게이트)");
    free(o);

    o = run(&db, "SELECT * FROM t WHERE id > 1"); /* PK 범위 스캔 */
    CHECK(!strstr(o, "two") && strstr(o, "three"), "PK 범위 스캔: 안 보임");
    free(o);

    o = run(&db, "SELECT * FROM t WHERE n = 20"); /* 보조 인덱스 */
    CHECK(!strstr(o, "two") && strstr(o, "인덱스 사용"), "보조 인덱스: 안 보임");
    free(o);

    o = run(&db, "SELECT COUNT(*) FROM t"); /* 집계(materialize) */
    CHECK(strstr(o, "2") != NULL, "COUNT(*) = 2");
    free(o);

    o = run(&db, "SELECT * FROM t ORDER BY id"); /* 정렬 경로 */
    CHECK(!strstr(o, "two"), "ORDER BY: 안 보임");
    free(o);

    o = run(&db, "SELECT * FROM u INNER JOIN t ON u.tid = t.id"); /* 조인(인덱스 NLJ) */
    CHECK(!strstr(o, "two"), "JOIN: 지운 행과 짝을 짓지 않음");
    free(o);

    CHECK(phys_rows(&db, "t") == 3, "힙엔 여전히 3행이 물리적으로 있다 (xmax 논리 삭제)");

    /* --- B. DELETE를 ROLLBACK하면 되살아난다 --- */
    o = run(&db, "BEGIN"); free(o);
    o = run(&db, "DELETE FROM t WHERE id = 1"); free(o);
    o = run(&db, "SELECT * FROM t");
    CHECK(!strstr(o, "one"), "트랜잭션 안: 내가 지운 행은 나에게도 안 보임");
    free(o);
    o = run(&db, "ROLLBACK"); free(o);
    o = run(&db, "SELECT * FROM t");
    CHECK(strstr(o, "one") != NULL, "ROLLBACK -> 지운 행이 되살아남 (MVCC 롤백)");
    free(o);

    /* --- C. UPDATE = 옛 버전 xmax + 새 버전. 롤백하면 옛 값으로 --- */
    int before_upd = phys_rows(&db, "t");
    o = run(&db, "UPDATE t SET v = 'ONE' WHERE id = 1"); free(o);
    o = run(&db, "SELECT * FROM t WHERE id = 1");
    CHECK(strstr(o, "ONE") && !strstr(o, "'one'"), "UPDATE 후 새 값만 보임");
    free(o);
    CHECK(phys_rows(&db, "t") == before_upd + 1,
          "UPDATE가 버전을 하나 더 쌓음 (옛 버전은 힙에 남음)");

    o = run(&db, "BEGIN"); free(o);
    o = run(&db, "UPDATE t SET v = 'X' WHERE id = 3"); free(o);
    o = run(&db, "ROLLBACK"); free(o);
    o = run(&db, "SELECT * FROM t WHERE id = 3");
    CHECK(strstr(o, "three") && !strstr(o, "X"), "UPDATE 롤백 -> 옛 값으로 복귀");
    free(o);

    /* --- D. 지운 PK를 다시 INSERT --- */
    o = run(&db, "INSERT INTO t VALUES (2, 21, 'two-again')"); free(o);
    o = run(&db, "SELECT * FROM t WHERE id = 2");
    CHECK(strstr(o, "two-again") && !strstr(o, "'two'"), "재삽입한 PK: 새 행만 보임");
    free(o);

    /* --- E. 커밋된 DELETE는 재오픈 후에도 숨는다 --- */
    o = run(&db, "DELETE FROM t WHERE id = 3"); free(o);
    db_close(&db);
    db_open(&db, path);
    o = run(&db, "SELECT * FROM t");
    CHECK(!strstr(o, "three"), "재오픈 후에도 커밋된 DELETE는 안 보임");
    free(o);
    o = run(&db, "SELECT * FROM t WHERE id = 3");
    CHECK(!strstr(o, "three"), "재오픈 후 PK 조회도 안 보임");
    free(o);
    CHECK(phys_rows(&db, "t") >= 4, "재오픈 후에도 죽은 버전들이 힙에 남아 있다 (VACUUM 전)");

    db_close(&db);
    cleanup(path);
    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
