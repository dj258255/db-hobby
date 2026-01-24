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
    const char *path = "build/test_txn.db";
    const char *idx = "build/test_txn.db.idx";
    unlink(path);
    unlink(idx);

    Database db;
    db_open(&db, path);

    char *o;
    o = run(&db, "CREATE TABLE users (id INT, name TEXT)");
    free(o);
    o = run(&db, "INSERT INTO users VALUES (1, 'kim')"); /* autocommit */
    free(o);

    o = run(&db, "SELECT * FROM users");
    CHECK(strstr(o, "kim") && strstr(o, "(1행"), "초기: kim 1행");
    free(o);

    /* -- 롤백 -- */
    o = run(&db, "BEGIN");
    CHECK(strstr(o, "트랜잭션 시작") != NULL, "BEGIN");
    free(o);
    o = run(&db, "INSERT INTO users VALUES (2, 'lee')");
    free(o);
    o = run(&db, "SELECT * FROM users");
    CHECK(strstr(o, "kim") && strstr(o, "lee") && strstr(o, "(2행"),
          "트랜잭션 안에서는 자기 변경이 보인다 (2행)");
    free(o);
    o = run(&db, "ROLLBACK");
    CHECK(strstr(o, "롤백됨") != NULL, "ROLLBACK");
    free(o);
    o = run(&db, "SELECT * FROM users");
    CHECK(strstr(o, "kim") && !strstr(o, "lee") && strstr(o, "(1행"),
          "롤백 후 lee 사라짐 (힙 되돌림)");
    free(o);
    o = run(&db, "SELECT * FROM users WHERE id = 2");
    CHECK(strstr(o, "0행") && !strstr(o, "lee"), "롤백 후 인덱스도 id=2 못 찾음");
    free(o);

    /* -- 커밋 -- */
    o = run(&db, "BEGIN");
    free(o);
    o = run(&db, "INSERT INTO users VALUES (3, 'park')");
    free(o);
    o = run(&db, "COMMIT");
    CHECK(strstr(o, "커밋됨") != NULL, "COMMIT");
    free(o);
    o = run(&db, "SELECT * FROM users");
    CHECK(strstr(o, "kim") && strstr(o, "park") && strstr(o, "(2행"), "커밋 후 kim, park (2행)");
    free(o);
    o = run(&db, "SELECT * FROM users WHERE id = 3");
    CHECK(strstr(o, "park") && strstr(o, "1행"), "커밋된 park는 인덱스로 찾힌다");
    free(o);

    db_close(&db);

    /* -- 재시작 영속성 -- */
    Database db2;
    db_open(&db2, path);
    /* 스키마가 카탈로그에 영속되므로 재선언 없이 바로 질의 */
    o = run(&db2, "SELECT * FROM users");
    CHECK(strstr(o, "kim") && strstr(o, "park") && strstr(o, "(2행"),
          "재시작 후에도 커밋된 행만 남음 (2행)");
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
