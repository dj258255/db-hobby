#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 병렬 집계(38편) 검증.
 * 큰 테이블(>= PARSCAN_MIN_PAGES)의 집계/GROUP BY는 워커가 가시성+WHERE를 병렬
 * 평가해 행을 모으고 aggregate_rowset(직렬과 같은 코드)로 계산한다. 이 경로는
 * 직렬 exec_select_project의 SELECT_MAX_ROWS materialize cap(큰 테이블 집계를
 * 조용히 절단하던 버그)까지 우회한다. 값은 수학적 정답(oracle)과 대조하고,
 * MVCC(UPDATE/DELETE) 반영·엔진 ThreadSanitizer 클린도 확인한다.
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

/* 스칼라 집계 출력에서 값 라인을 파싱: "HEADER\nVALUE\n(1행)\n" */
static long scalar_val(const char *out) {
    const char *nl = strchr(out, '\n');       /* 헤더 끝 */
    if (!nl) return -999999;
    return atol(nl + 1);                        /* 그다음 라인 = 값 */
}

int main(void) {
    const char *path = "build/test_paragg.db";
    unlink(path);

    Database db;
    db_open(&db, path);
    char *o;

    o = run(&db, "CREATE TABLE big (id INT, v INT)"); free(o);
    const int N = 6000; /* > SELECT_MAX_ROWS(4096) 로 잡아 cap 우회를 증명 */
    for (int i = 1; i <= N; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO big VALUES (%d, %d)", i, i);
        o = run(&db, sql); free(o);
    }

    /* ── 1) cap 우회: COUNT(*) = 6000 (직렬 경로였다면 4096으로 절단) ── */
    o = run(&db, "SELECT COUNT(*) FROM big");
    CHECK(scalar_val(o) == N, "COUNT(*) = 6000 — materialize cap 우회(직렬은 4096 절단)");
    free(o);

    /* ── 2) 수학 oracle 대조 ── */
    o = run(&db, "SELECT SUM(v) FROM big"); /* 1+..+6000 = 18003000 */
    CHECK(scalar_val(o) == 18003000L, "SUM(v) = 18003000 (n(n+1)/2)");
    free(o);
    o = run(&db, "SELECT MIN(v) FROM big");
    CHECK(scalar_val(o) == 1, "MIN(v) = 1");
    free(o);
    o = run(&db, "SELECT MAX(v) FROM big");
    CHECK(scalar_val(o) == N, "MAX(v) = 6000");
    free(o);
    o = run(&db, "SELECT AVG(v) FROM big"); /* 3000.5 */
    CHECK(strstr(o, "3000.5") != NULL, "AVG(v) = 3000.5");
    free(o);

    /* ── 3) WHERE와 함께 (워커 술어가 병렬 필터) ── */
    o = run(&db, "SELECT COUNT(*) FROM big WHERE v > 5990"); /* 5991..6000 = 10 */
    CHECK(scalar_val(o) == 10, "COUNT(*) WHERE v>5990 = 10");
    free(o);
    o = run(&db, "SELECT SUM(v) FROM big WHERE v <= 10"); /* 1+..+10 = 55 */
    CHECK(scalar_val(o) == 55, "SUM(v) WHERE v<=10 = 55");
    free(o);

    /* ── 4) 여러 집계 한 번에 ── */
    o = run(&db, "SELECT MIN(v), MAX(v) FROM big");
    CHECK(strstr(o, "1 | 6000") != NULL, "MIN(v), MAX(v) = 1 | 6000");
    free(o);

    /* ── 5) MVCC: UPDATE가 병렬 집계에도 반영 ── */
    o = run(&db, "UPDATE big SET v = 1000000 WHERE id = 1"); free(o); /* v: 1 -> 1000000 */
    o = run(&db, "SELECT SUM(v) FROM big"); /* 18003000 - 1 + 1000000 = 19002999 */
    CHECK(scalar_val(o) == 19002999L, "UPDATE 후 SUM 정확(가시성 게이트가 옛 버전 제외)");
    free(o);
    o = run(&db, "SELECT MAX(v) FROM big");
    CHECK(scalar_val(o) == 1000000L, "UPDATE 후 MAX = 1000000");
    free(o);

    /* ── 6) DELETE 반영 ── */
    o = run(&db, "DELETE FROM big WHERE id = 2"); free(o); /* v=2 제거 */
    o = run(&db, "SELECT COUNT(*) FROM big");
    CHECK(scalar_val(o) == N - 1, "DELETE 후 COUNT(*) = 5999");
    free(o);

    /* ── 7) GROUP BY도 같은 경로(수집 병렬 + aggregate_rowset) ── */
    o = run(&db, "CREATE TABLE grp (id INT, k INT, v INT)"); free(o);
    for (int i = 1; i <= N; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO grp VALUES (%d, %d, %d)", i, i % 3, i);
        o = run(&db, sql); free(o);
    }
    o = run(&db, "SELECT k, COUNT(*) FROM grp GROUP BY k"); /* 각 그룹 2000행 */
    /* 세 그룹(0,1,2) 각 2000 — 총합 6000이 cap 없이 집계됨 */
    CHECK(strstr(o, "0 | 2000") && strstr(o, "1 | 2000") && strstr(o, "2 | 2000"),
          "GROUP BY k: 세 그룹 각 2000 (cap 없이)");
    free(o);

    /* ── 8) 작은 테이블은 직렬 경로(무회귀) ── */
    o = run(&db, "CREATE TABLE small (id INT, v INT)"); free(o);
    for (int i = 1; i <= 5; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO small VALUES (%d, %d)", i, i * 10);
        o = run(&db, sql); free(o);
    }
    o = run(&db, "SELECT SUM(v) FROM small"); /* 10+20+30+40+50 = 150 */
    CHECK(scalar_val(o) == 150, "작은 테이블 SUM(직렬 경로)도 정확 — 무회귀");
    free(o);

    db_close(&db);
    unlink(path);

    if (failures == 0) { printf("\n모든 테스트 통과\n"); return 0; }
    printf("\n%d개 실패\n", failures);
    return 1;
}
