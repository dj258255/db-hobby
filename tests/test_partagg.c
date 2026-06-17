#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 진짜 병렬 부분 집계(39편) 검증.
 * 38편은 매칭 행을 모두 모은 뒤 집계했지만(메모리 O(n)), 39편은 워커가 행을 안
 * 모으고 항목별 부분합만 누적해 leader가 결합한다(메모리 O(워커수×항목수)).
 * 여기선 그 경로의 구별되는 성질을 본다: NULL 처리(COUNT(*) vs COUNT(col)/SUM은
 * NULL 무시), 여러 집계 한 번에, 빈 결과(COUNT=0/SUM=NULL), TEXT MIN/MAX는 직렬
 * 폴백으로도 정확. 값은 수학적 정답(oracle)과 대조하고 엔진 ThreadSanitizer로도 검증.
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
static long scalar_val(const char *out) {
    const char *nl = strchr(out, '\n');
    return nl ? atol(nl + 1) : -999999;
}

int main(void) {
    const char *path = "build/test_partagg.db";
    unlink(path);
    Database db;
    db_open(&db, path);
    char *o;

    /* big(id, v): 6000행(>4096, >=16페이지). 10의 배수 행은 v = NULL. */
    o = run(&db, "CREATE TABLE big (id INT, v INT)"); free(o);
    const int N = 6000;
    for (int i = 1; i <= N; i++) {
        char sql[96];
        if (i % 10 == 0) snprintf(sql, sizeof(sql), "INSERT INTO big VALUES (%d, NULL)", i);
        else            snprintf(sql, sizeof(sql), "INSERT INTO big VALUES (%d, %d)", i, i);
        o = run(&db, sql); free(o);
    }
    /* oracle: NULL 아닌 행 = 5400개. 비NULL 합 = 18003000 - (10+20+..+6000)
     *        = 18003000 - 1803000 = 16200000. 비NULL MIN=1, MAX=5999. */

    /* ── 1) NULL 처리: COUNT(*)는 전체, COUNT(col)/SUM은 NULL 무시 ── */
    o = run(&db, "SELECT COUNT(*) FROM big");
    CHECK(scalar_val(o) == N, "COUNT(*) = 6000 (NULL 포함 전체 행)");
    free(o);
    o = run(&db, "SELECT COUNT(v) FROM big");
    CHECK(scalar_val(o) == 5400, "COUNT(v) = 5400 (NULL 600개 제외)");
    free(o);
    o = run(&db, "SELECT SUM(v) FROM big");
    CHECK(scalar_val(o) == 16200000L, "SUM(v) = 16200000 (NULL 무시)");
    free(o);
    o = run(&db, "SELECT MIN(v), MAX(v) FROM big");
    CHECK(strstr(o, "1 | 5999") != NULL, "MIN(v), MAX(v) = 1 | 5999 (NULL 무시)");
    free(o);

    /* ── 2) 여러 집계 한 번에 결합 ── */
    o = run(&db, "SELECT COUNT(*), COUNT(v), SUM(v) FROM big");
    CHECK(strstr(o, "6000 | 5400 | 16200000") != NULL,
          "다중 집계 한 쿼리: 6000 | 5400 | 16200000");
    free(o);

    /* ── 3) 빈 결과(전부 필터됨): COUNT(*)=0, SUM=NULL ── */
    o = run(&db, "SELECT COUNT(*), SUM(v) FROM big WHERE v > 999999");
    CHECK(strstr(o, "0 | NULL") != NULL, "빈 결과: COUNT(*)=0, SUM=NULL");
    free(o);

    /* ── 4) AVG는 비NULL 평균 ── */
    o = run(&db, "SELECT AVG(v) FROM big WHERE v <= 9"); /* 1..9 중 NULL 없음 -> 평균 5 */
    CHECK(strstr(o, "\n5\n") != NULL, "AVG(v) WHERE v<=9 = 5");
    free(o);

    /* ── 5) TEXT MIN/MAX: 부분 집계 자격 미달 -> 직렬/38 폴백으로도 정확 ── */
    o = run(&db, "CREATE TABLE t2 (id INT, name TEXT)"); free(o);
    for (int i = 1; i <= N; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO t2 VALUES (%d, 'n%05d')", i, i);
        o = run(&db, sql); free(o);
    }
    o = run(&db, "SELECT MIN(name), MAX(name) FROM t2"); /* n00001 .. n06000 */
    CHECK(strstr(o, "n00001 | n06000") != NULL,
          "TEXT MIN/MAX = n00001 | n06000 (부분집계 폴백 경로도 정확)");
    free(o);

    /* ── 6) 작은 테이블은 직렬 경로(무회귀) ── */
    o = run(&db, "CREATE TABLE small (id INT, v INT)"); free(o);
    for (int i = 1; i <= 5; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO small VALUES (%d, %d)", i, i * 10);
        o = run(&db, sql); free(o);
    }
    o = run(&db, "SELECT SUM(v), COUNT(*) FROM small");
    CHECK(strstr(o, "150 | 5") != NULL, "작은 테이블(직렬) SUM,COUNT = 150 | 5 — 무회귀");
    free(o);

    db_close(&db);
    unlink(path);

    if (failures == 0) { printf("\n모든 테스트 통과\n"); return 0; }
    printf("\n%d개 실패\n", failures);
    return 1;
}
