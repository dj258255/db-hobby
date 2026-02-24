#include "db.h"
#include "wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    const char *ext[] = {"t.tbl", "t.idx", "t.wal", "t.idx.wal"};
    unlink(base);
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        snprintf(p, sizeof(p), "%s.%s", base, ext[i]);
        unlink(p);
    }
}

/* WAL이 실제 INSERT 경로에 연결됐음을, 커밋 도중 크래시를 주입해 증명한다.
 * 크래시 후 닫고 다시 열면 wal_open이 로그를 보고 redo(내구성)/discard(원자성)한다. */
int main(void) {
    const char *path = "build/test_waldml.db";

    /* --- 시나리오 A: 커밋 마커 fsync 직후(데이터 적용 전) 크래시 -> 복구가 redo --- */
    cleanup(path);
    {
        Database db;
        db_open(&db, path);
        char *o;
        o = run(&db, "CREATE TABLE t (id INT, v TEXT)"); free(o);
        o = run(&db, "INSERT INTO t VALUES (1, 'alpha')"); free(o); /* 정상 커밋 */

        wal_test_crash_after_log = 1; /* 다음 커밋은 로그+마커 fsync 후 멈춘다(데이터 미적용) */
        o = run(&db, "INSERT INTO t VALUES (2, 'beta')"); free(o);
        wal_test_crash_after_log = 0;

        db_close(&db); /* 크래시 흉내: 데이터엔 beta 없음, 로그엔 커밋된 채 남음 */

        db_open(&db, path); /* wal_open이 데이터·인덱스 로그를 둘 다 redo */
        o = run(&db, "SELECT * FROM t");
        CHECK(strstr(o, "alpha") && strstr(o, "beta"),
              "크래시(커밋 후) -> 재시작 시 beta 데이터가 redo됨 (내구성)");
        free(o);
        /* WHERE id=2 는 인덱스 점 조회 -> 인덱스 항목까지 복구됐는지 확인 */
        o = run(&db, "SELECT * FROM t WHERE id = 2");
        CHECK(strstr(o, "beta") && strstr(o, "인덱스 사용"),
              "크래시 후 인덱스도 redo됨 (id=2 인덱스 조회로 beta)");
        free(o);
        db_close(&db);
    }

    /* --- 시나리오 B: 커밋 마커 전에 크래시 -> 복구가 discard --- */
    cleanup(path);
    {
        Database db;
        db_open(&db, path);
        char *o;
        o = run(&db, "CREATE TABLE t (id INT, v TEXT)"); free(o);
        o = run(&db, "INSERT INTO t VALUES (1, 'alpha')"); free(o); /* 정상 커밋 */

        wal_test_crash_before_commit = 1; /* 다음 커밋은 페이지 로그만 쓰고 마커 없이 멈춘다 */
        o = run(&db, "INSERT INTO t VALUES (2, 'beta')"); free(o);
        wal_test_crash_before_commit = 0;

        db_close(&db); /* 크래시 흉내: 마커 없는 로그 */

        db_open(&db, path); /* wal_open이 마커 없는 로그를 버린다 */
        o = run(&db, "SELECT * FROM t");
        CHECK(strstr(o, "alpha") && !strstr(o, "beta"),
              "크래시(커밋 전) -> 재시작 시 beta가 discard됨 (원자성)");
        free(o);
        db_close(&db);
    }

    cleanup(path);
    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
