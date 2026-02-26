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
    const char *path = "build/test_explain.db";
    unlink(path);

    Database db;
    db_open(&db, path);

    char *o;
    o = run(&db, "CREATE TABLE users (id INT, name TEXT)"); free(o);
    o = run(&db, "INSERT INTO users VALUES (1, 'kim')"); free(o);
    o = run(&db, "INSERT INTO users VALUES (2, 'lee')"); free(o);
    o = run(&db, "INSERT INTO users VALUES (3, 'park')"); free(o);
    o = run(&db, "INSERT INTO users VALUES (4, 'amy')"); free(o);
    o = run(&db, "CREATE TABLE o (oid INT, uid INT, item TEXT)"); free(o);
    o = run(&db, "INSERT INTO o VALUES (10, 1, 'book')"); free(o);
    o = run(&db, "INSERT INTO o VALUES (11, 2, 'pen')"); free(o);

    /* 단일 테이블 접근 방법 */
    o = run(&db, "EXPLAIN SELECT * FROM users WHERE id = 3");
    CHECK(strstr(o, "Index Point Lookup") && strstr(o, "id = 3"),
          "PK = -> Index Point Lookup");
    free(o);

    o = run(&db, "EXPLAIN SELECT * FROM users WHERE id > 3");
    CHECK(strstr(o, "Index Range Scan") && strstr(o, "id > 3"),
          "PK 범위 -> Index Range Scan");
    free(o);

    o = run(&db, "EXPLAIN SELECT * FROM users WHERE name = 'kim'");
    CHECK(strstr(o, "Seq Scan on users") && strstr(o, "filter: name = 'kim'"),
          "비PK 조건 -> Seq Scan + filter");
    free(o);

    o = run(&db, "EXPLAIN SELECT * FROM users");
    CHECK(strstr(o, "Seq Scan on users") && !strstr(o, "filter"),
          "조건 없음 -> Seq Scan (filter 없음)");
    free(o);

    /* ORDER BY / LIMIT가 인덱스를 끄는 것 (minidb의 정직한 한계) */
    o = run(&db, "EXPLAIN SELECT * FROM users ORDER BY name");
    CHECK(strstr(o, "Sort") && strstr(o, "Seq Scan on users"),
          "ORDER BY -> Sort 노드 + Seq Scan(인덱스 못 씀)");
    free(o);

    o = run(&db, "EXPLAIN SELECT * FROM users WHERE id = 2 LIMIT 1");
    CHECK(strstr(o, "Limit") && strstr(o, "Seq Scan") && strstr(o, "filter: id = 2"),
          "LIMIT가 있으면 PK여도 인덱스 안 씀 -> Seq Scan");
    free(o);

    /* 집계 */
    o = run(&db, "EXPLAIN SELECT name, COUNT(*) FROM users GROUP BY name");
    CHECK(strstr(o, "GroupAggregate") && strstr(o, "group: name") &&
              strstr(o, "Seq Scan on users"),
          "GROUP BY -> GroupAggregate + Seq Scan");
    free(o);

    /* 조인 방법 선택 */
    o = run(&db, "EXPLAIN SELECT * FROM users JOIN o ON users.id = o.uid");
    CHECK(strstr(o, "Nested-Loop Join") && strstr(o, "Hash Join -> o"),
          "조인 키가 비PK(o.uid) -> Hash Join");
    free(o);

    o = run(&db, "EXPLAIN SELECT * FROM o JOIN users ON o.uid = users.id");
    CHECK(strstr(o, "Index Nested Loop -> users"),
          "조인 키가 PK(users.id) -> Index Nested Loop");
    free(o);

    o = run(&db, "EXPLAIN SELECT * FROM users LEFT JOIN o ON users.id = o.uid");
    CHECK(strstr(o, "Left Hash Join -> o"),
          "LEFT JOIN -> Left 접두 + 방법 표시");
    free(o);

    db_close(&db);
    unlink(path);
    {
        const char *suf[] = {".users.tbl", ".users.idx", ".users.wal", ".users.idx.wal",
                             ".o.tbl", ".o.idx", ".o.wal", ".o.idx.wal"};
        char buf[128];
        for (int i = 0; i < 8; i++) {
            snprintf(buf, sizeof buf, "%s%s", path, suf[i]);
            unlink(buf);
        }
    }

    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패\n", failures);
    return 1;
}
