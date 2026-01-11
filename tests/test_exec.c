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

/* SQL 한 줄을 실행하고 출력을 문자열로 잡아 돌려준다(caller가 free). */
static char *run(Database *db, const char *sql) {
    char *buf = NULL;
    size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    db_exec(db, sql, f);
    fclose(f);
    return buf;
}

int main(void) {
    const char *path = "build/test_exec.db";
    const char *idx = "build/test_exec.db.idx";
    unlink(path);
    unlink(idx);

    Database db;
    db_open(&db, path);

    char *o;
    o = run(&db, "CREATE TABLE users (id INT, name TEXT)");
    CHECK(strstr(o, "생성됨") != NULL, "CREATE TABLE 실행");
    free(o);

    o = run(&db, "INSERT INTO users VALUES (1, 'kim')");
    CHECK(strstr(o, "삽입") != NULL, "INSERT 실행");
    free(o);
    o = run(&db, "INSERT INTO users VALUES (2, 'lee')");
    free(o);
    o = run(&db, "INSERT INTO users VALUES (3, 'park')");
    free(o);

    /* SELECT * — 모든 행 */
    o = run(&db, "SELECT * FROM users");
    CHECK(strstr(o, "kim") && strstr(o, "lee") && strstr(o, "park"), "SELECT * 가 모든 행 반환");
    CHECK(strstr(o, "(3행)") != NULL, "SELECT * 가 3행 보고");
    CHECK(db.used_index == 0, "WHERE 없으면 풀 스캔");
    free(o);

    /* WHERE 정수 (인덱스된 PK) → 인덱스 사용 */
    o = run(&db, "SELECT * FROM users WHERE id = 2");
    CHECK(strstr(o, "lee") && !strstr(o, "kim") && !strstr(o, "park"), "WHERE id=2 → lee만");
    CHECK(strstr(o, "1행") != NULL, "WHERE 결과 1행");
    CHECK(db.used_index == 1, "WHERE id=2 는 인덱스 사용 (O(log n))");
    free(o);

    /* WHERE 문자열 (인덱스 안 된 컬럼) → 풀 스캔 */
    o = run(&db, "SELECT * FROM users WHERE name = 'park'");
    CHECK(strstr(o, "park") && strstr(o, "(1행)"), "WHERE name='park' → park");
    CHECK(db.used_index == 0, "TEXT 컬럼 WHERE는 풀 스캔");
    free(o);

    /* 타입/개수 오류 */
    o = run(&db, "INSERT INTO users VALUES (9)");
    CHECK(strstr(o, "ERROR") != NULL, "값 개수 안 맞으면 ERROR");
    free(o);

    /* 영속성: 행은 디스크에 남는다(스키마는 메모리라 재선언 필요) */
    db_close(&db);
    Database db2;
    db_open(&db2, path);
    o = run(&db2, "CREATE TABLE users (id INT, name TEXT)");
    free(o);
    o = run(&db2, "SELECT * FROM users");
    CHECK(strstr(o, "kim") && strstr(o, "(3행)"), "재오픈 후에도 행 3개 유지");
    free(o);
    db_close(&db2);

    unlink(path);
    unlink(idx);

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
