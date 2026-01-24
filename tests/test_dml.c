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
    const char *path = "build/test_dml.db";
    const char *idx = "build/test_dml.db.idx";
    unlink(path);
    unlink(idx);

    Database db;
    db_open(&db, path);

    char *o;
    o = run(&db, "CREATE TABLE users (id INT, name TEXT)"); free(o);
    o = run(&db, "INSERT INTO users VALUES (1, 'kim')"); free(o);
    o = run(&db, "INSERT INTO users VALUES (2, 'lee')"); free(o);
    o = run(&db, "INSERT INTO users VALUES (3, 'park')"); free(o);

    /* DELETE WHERE id = 2 */
    o = run(&db, "DELETE FROM users WHERE id = 2");
    CHECK(strstr(o, "1개 행 삭제됨") != NULL, "DELETE WHERE id=2 → 1개 삭제");
    free(o);
    o = run(&db, "SELECT * FROM users");
    CHECK(strstr(o, "kim") && strstr(o, "park") && !strstr(o, "lee") && strstr(o, "(2행"),
          "삭제 후 kim, park만 (2행)");
    free(o);
    o = run(&db, "SELECT * FROM users WHERE id = 2");
    CHECK(strstr(o, "0행") && !strstr(o, "lee"), "삭제된 행은 인덱스로도 안 나옴");
    free(o);

    /* UPDATE WHERE id = 1 (name 변경) — RID가 바뀌어도 인덱스로 찾혀야 함 */
    o = run(&db, "UPDATE users SET name = 'KIM' WHERE id = 1");
    CHECK(strstr(o, "1개 행 수정됨") != NULL, "UPDATE WHERE id=1 → 1개 수정");
    free(o);
    o = run(&db, "SELECT * FROM users WHERE id = 1");
    CHECK(strstr(o, "KIM") && strstr(o, "1행"), "수정 후 인덱스로 새 행(KIM) 찾음");
    free(o);
    o = run(&db, "SELECT * FROM users");
    CHECK(strstr(o, "KIM") && strstr(o, "park") && !strstr(o, "kim"), "kim → KIM 반영");
    free(o);

    /* UPDATE 전체 (WHERE 없음) */
    o = run(&db, "UPDATE users SET name = 'Z'");
    CHECK(strstr(o, "2개 행 수정됨") != NULL, "WHERE 없는 UPDATE는 모든 행");
    free(o);
    o = run(&db, "SELECT * FROM users");
    CHECK(strstr(o, "Z") && !strstr(o, "KIM") && !strstr(o, "park"), "모든 name이 Z");
    free(o);

    /* DELETE 전체 */
    o = run(&db, "DELETE FROM users");
    CHECK(strstr(o, "2개 행 삭제됨") != NULL, "WHERE 없는 DELETE는 모든 행");
    free(o);
    o = run(&db, "SELECT * FROM users");
    CHECK(strstr(o, "(0행"), "전부 삭제 후 0행");
    free(o);

    /* 영속성 */
    db_close(&db);
    Database db2;
    db_open(&db2, path);
    /* 스키마가 카탈로그에 영속되므로 재선언 없이 바로 질의 */
    o = run(&db2, "SELECT * FROM users");
    CHECK(strstr(o, "(0행"), "재시작 후 재선언 없이 0행");
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
