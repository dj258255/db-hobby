#include "db.h"

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
    unlink(base);
    snprintf(p, sizeof(p), "%s.sales.tbl", base);
    unlink(p);
    snprintf(p, sizeof(p), "%s.sales.idx", base);
    unlink(p);
}

int main(void) {
    const char *path = "build/test_agg.db";
    cleanup(path);

    Database db;
    db_open(&db, path);

    char *o;
    o = run(&db, "CREATE TABLE sales (id INT, dept TEXT, amt INT)"); free(o);
    o = run(&db, "INSERT INTO sales VALUES (1, 'eng', 100)"); free(o);
    o = run(&db, "INSERT INTO sales VALUES (2, 'eng', 200)"); free(o);
    o = run(&db, "INSERT INTO sales VALUES (3, 'sales', 50)"); free(o);
    o = run(&db, "INSERT INTO sales VALUES (4, 'sales', 150)"); free(o);
    o = run(&db, "INSERT INTO sales VALUES (5, 'sales', 300)"); free(o);

    /* COUNT(*) */
    o = run(&db, "SELECT COUNT(*) FROM sales");
    CHECK(strstr(o, "COUNT(*)") && strstr(o, "5") && strstr(o, "(1행"), "COUNT(*) -> 5");
    free(o);

    /* SUM / MIN / MAX / AVG */
    o = run(&db, "SELECT SUM(amt) FROM sales");
    CHECK(strstr(o, "SUM(amt)") && strstr(o, "800"), "SUM(amt) -> 800");
    free(o);
    o = run(&db, "SELECT MIN(amt), MAX(amt) FROM sales");
    CHECK(strstr(o, "MIN(amt) | MAX(amt)") && strstr(o, "50 | 300"), "MIN/MAX -> 50, 300");
    free(o);
    o = run(&db, "SELECT AVG(amt) FROM sales");
    CHECK(strstr(o, "AVG(amt)") && strstr(o, "160"), "AVG(amt) -> 160");
    free(o);

    /* 집계 + WHERE */
    o = run(&db, "SELECT COUNT(*) FROM sales WHERE amt > 100");
    CHECK(strstr(o, "3") && strstr(o, "(1행"), "COUNT(*) WHERE amt>100 -> 3");
    free(o);

    /* GROUP BY: dept별 COUNT, SUM (dept 오름차순: eng, sales) */
    o = run(&db, "SELECT dept, COUNT(*), SUM(amt) FROM sales GROUP BY dept");
    CHECK(strstr(o, "dept | COUNT(*) | SUM(amt)"), "GROUP BY 헤더");
    CHECK(strstr(o, "eng | 2 | 300") != NULL, "eng 그룹 -> 2건, 합 300");
    CHECK(strstr(o, "sales | 3 | 500") != NULL, "sales 그룹 -> 3건, 합 500");
    CHECK(strstr(o, "(2행") != NULL, "그룹 2개");
    free(o);

    /* GROUP BY + AVG */
    o = run(&db, "SELECT dept, AVG(amt) FROM sales GROUP BY dept");
    CHECK(strstr(o, "eng | 150") && strstr(o, "sales") && strstr(o, "166"),
          "그룹별 AVG (eng 150, sales 166.667)");
    free(o);

    /* 투영(집계 아님): 고른 컬럼만 */
    o = run(&db, "SELECT dept, amt FROM sales WHERE id = 1");
    CHECK(strstr(o, "dept | amt") && strstr(o, "eng | 100") && strstr(o, "(1행"),
          "투영 dept, amt WHERE id=1 -> eng | 100");
    free(o);

    /* 투영 + ORDER BY + LIMIT */
    o = run(&db, "SELECT amt FROM sales ORDER BY amt DESC LIMIT 1");
    CHECK(strstr(o, "300") && !strstr(o, "100") && strstr(o, "(1행"),
          "투영 ORDER BY amt DESC LIMIT 1 -> 300");
    free(o);

    /* 재오픈 후에도 집계 동작 */
    db_close(&db);
    db_open(&db, path);
    o = run(&db, "SELECT COUNT(*) FROM sales");
    CHECK(strstr(o, "5") != NULL, "재오픈 후 COUNT(*) -> 5");
    free(o);

    db_close(&db);
    cleanup(path);

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
