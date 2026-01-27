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

int main(void) {
    const char *path = "build/test_where.db";
    const char *idx = "build/test_where.db.idx";
    unlink(path);
    unlink(idx);

    Database db;
    db_open(&db, path);

    char *o;
    o = run(&db, "CREATE TABLE users (id INT, name TEXT)"); free(o);
    o = run(&db, "INSERT INTO users VALUES (1, 'kim')"); free(o);
    o = run(&db, "INSERT INTO users VALUES (2, 'lee')"); free(o);
    o = run(&db, "INSERT INTO users VALUES (3, 'park')"); free(o);
    o = run(&db, "INSERT INTO users VALUES (4, 'amy')"); free(o);

    /* > */
    o = run(&db, "SELECT * FROM users WHERE id > 2");
    CHECK(strstr(o, "park") && strstr(o, "amy") && !strstr(o, "kim") && !strstr(o, "lee") &&
              strstr(o, "(2행"),
          "id > 2  -> park, amy");
    CHECK(db.used_index == 1, "범위 조건(>)도 인덱스 (범위 스캔)");
    free(o);

    /* >= */
    o = run(&db, "SELECT * FROM users WHERE id >= 2");
    CHECK(strstr(o, "lee") && strstr(o, "park") && strstr(o, "amy") && !strstr(o, "kim") &&
              strstr(o, "(3행"),
          "id >= 2 -> lee, park, amy");
    free(o);

    /* < */
    o = run(&db, "SELECT * FROM users WHERE id < 2");
    CHECK(strstr(o, "kim") && !strstr(o, "lee") && strstr(o, "(1행"), "id < 2  -> kim");
    CHECK(db.used_index == 1, "범위 조건(<)도 인덱스");
    free(o);

    /* <= */
    o = run(&db, "SELECT * FROM users WHERE id <= 2");
    CHECK(strstr(o, "kim") && strstr(o, "lee") && !strstr(o, "park") && strstr(o, "(2행"),
          "id <= 2 -> kim, lee");
    free(o);

    /* != */
    o = run(&db, "SELECT * FROM users WHERE id != 2");
    CHECK(strstr(o, "kim") && !strstr(o, "lee") && strstr(o, "park") && strstr(o, "(3행"),
          "id != 2 -> kim, park, amy");
    free(o);

    /* = 은 여전히 인덱스 */
    o = run(&db, "SELECT * FROM users WHERE id = 2");
    CHECK(strstr(o, "lee") && strstr(o, "1행"), "id = 2  -> lee");
    CHECK(db.used_index == 1, "= 는 인덱스 사용");
    free(o);

    /* TEXT 비교 */
    o = run(&db, "SELECT * FROM users WHERE name != 'kim'");
    CHECK(strstr(o, "lee") && strstr(o, "park") && strstr(o, "amy") && !strstr(o, "kim") &&
              strstr(o, "(3행"),
          "name != 'kim' -> lee, park, amy");
    free(o);

    /* AND: 여러 조건 */
    o = run(&db, "SELECT * FROM users WHERE id > 1 AND id < 4");
    CHECK(strstr(o, "lee") && strstr(o, "park") && !strstr(o, "kim") && !strstr(o, "amy") &&
              strstr(o, "(2행"),
          "id > 1 AND id < 4 -> lee, park");
    free(o);
    o = run(&db, "SELECT * FROM users WHERE id >= 2 AND name != 'lee'");
    CHECK(strstr(o, "park") && strstr(o, "amy") && !strstr(o, "lee") && !strstr(o, "kim") &&
              strstr(o, "(2행"),
          "id >= 2 AND name != 'lee' -> park, amy");
    free(o);

    /* OR: 어느 한 묶음이라도 참이면 매칭 */
    o = run(&db, "SELECT * FROM users WHERE id = 1 OR id = 4");
    CHECK(strstr(o, "kim") && strstr(o, "amy") && !strstr(o, "lee") && !strstr(o, "park") &&
              strstr(o, "(2행"),
          "id = 1 OR id = 4 -> kim, amy");
    free(o);
    /* AND가 OR보다 강하게: (id=2 AND name='lee') OR id=4 -> lee, amy */
    o = run(&db, "SELECT * FROM users WHERE id = 2 AND name = 'lee' OR id = 4");
    CHECK(strstr(o, "lee") && strstr(o, "amy") && !strstr(o, "kim") && !strstr(o, "park") &&
              strstr(o, "(2행"),
          "id=2 AND name='lee' OR id=4 -> lee, amy");
    free(o);

    /* ORDER BY: 출력 순서를 위치로 검증 (id: 1=kim 2=lee 3=park 4=amy) */
    o = run(&db, "SELECT * FROM users ORDER BY id DESC");
    {
        char *p4 = strstr(o, "amy"), *p3 = strstr(o, "park"), *p1 = strstr(o, "kim");
        CHECK(p4 && p3 && p1 && p4 < p3 && p3 < p1, "ORDER BY id DESC -> amy, park, lee, kim");
    }
    free(o);
    o = run(&db, "SELECT * FROM users ORDER BY name");
    {
        char *amy = strstr(o, "amy"), *kim = strstr(o, "kim"), *park = strstr(o, "park");
        CHECK(amy && kim && park && amy < kim && kim < park,
              "ORDER BY name(ASC) -> amy, kim, lee, park");
    }
    free(o);

    /* LIMIT: ORDER BY와 함께 상위 N개만 */
    o = run(&db, "SELECT * FROM users ORDER BY id DESC LIMIT 2");
    CHECK(strstr(o, "amy") && strstr(o, "park") && !strstr(o, "lee") && !strstr(o, "kim") &&
              strstr(o, "(2행"),
          "ORDER BY id DESC LIMIT 2 -> amy, park");
    free(o);
    o = run(&db, "SELECT * FROM users LIMIT 1");
    CHECK(strstr(o, "(1행") != NULL, "LIMIT 1 -> 1행만");
    free(o);

    /* DELETE with range */
    o = run(&db, "DELETE FROM users WHERE id >= 3");
    CHECK(strstr(o, "2개 행 삭제됨") != NULL, "DELETE WHERE id >= 3 -> 2개 삭제");
    free(o);
    o = run(&db, "SELECT * FROM users");
    CHECK(strstr(o, "kim") && strstr(o, "lee") && !strstr(o, "park") && !strstr(o, "amy") &&
              strstr(o, "(2행"),
          "범위 삭제 후 kim, lee만");
    free(o);

    db_close(&db);
    unlink(path);
    unlink(idx);

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
