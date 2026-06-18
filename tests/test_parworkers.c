#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 병렬 실행의 '워커 수 무관 결과 동일성'을 못박는다(40편 벤치의 전제).
 * 같은 쿼리를 워커 1·2·4·8로 돌려 출력이 workers=1과 '바이트 단위로 동일'해야 한다.
 * 부분 집계(39)·병렬 집계 수집(38)·병렬 스트리밍 SELECT(37) 경로를 모두 덮는다.
 */

static int failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (cond) printf("  ok   %s\n", msg);                                   \
        else { printf("  FAIL %s\n", msg); failures++; }                        \
    } while (0)

static char *run(Database *db, const char *sql) {
    char *buf = NULL;
    size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    db_exec(db, sql, f);
    fclose(f);
    return buf;
}

/* sql을 workers=1 결과와 2·4·8 결과가 모두 같은지 검사. */
static void invariant(Database *db, const char *sql, const char *label) {
    db_set_parallel_workers(1);
    char *base = run(db, sql);
    int ok = 1;
    int ws[] = {2, 4, 8};
    for (int i = 0; i < 3; i++) {
        db_set_parallel_workers(ws[i]);
        char *o = run(db, sql);
        if (strcmp(base, o) != 0) ok = 0;
        free(o);
    }
    CHECK(ok, label);
    free(base);
    db_set_parallel_workers(4);
}

int main(void) {
    const char *path = "build/test_parworkers.db";
    unlink(path);
    Database db;
    db_open(&db, path);
    char *o;

    o = run(&db, "CREATE TABLE big (id INT, v INT)"); free(o);
    const int N = 6000; /* >=16페이지 -> 병렬 경로 */
    for (int i = 1; i <= N; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO big VALUES (%d, %d)", i, i);
        o = run(&db, sql); free(o);
    }

    CHECK(db_get_parallel_workers() == 4, "기본 워커 수 4");

    /* 부분 집계(39) — 워커 수 무관 동일 */
    invariant(&db, "SELECT COUNT(*), SUM(v), MIN(v), MAX(v) FROM big",
              "부분 집계 COUNT/SUM/MIN/MAX: 워커 1·2·4·8 결과 동일");
    invariant(&db, "SELECT SUM(v) FROM big WHERE v > 3000",
              "부분 집계 SUM+WHERE: 워커 무관 동일");
    /* GROUP BY(38 수집 경로) — 워커 수 무관 동일 */
    o = run(&db, "CREATE TABLE grp (id INT, k INT, v INT)"); free(o);
    for (int i = 1; i <= N; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO grp VALUES (%d, %d, %d)", i, i % 4, i);
        o = run(&db, sql); free(o);
    }
    invariant(&db, "SELECT k, COUNT(*), SUM(v) FROM grp GROUP BY k",
              "GROUP BY(수집 경로): 워커 무관 동일");
    /* 병렬 스트리밍 SELECT(37) — 워커 수 무관 동일(페이지 순서 보존) */
    invariant(&db, "SELECT * FROM big WHERE v > 5990",
              "병렬 스트리밍 SELECT: 워커 무관 동일(페이지 순서)");

    db_close(&db);
    unlink(path);

    if (failures == 0) { printf("\n모든 테스트 통과\n"); return 0; }
    printf("\n%d개 실패\n", failures);
    return 1;
}
