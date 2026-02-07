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

/* 테스트가 만드는 파일들을 모두 지운다(카탈로그 + 테이블별 .tbl/.idx). */
static void cleanup(const char *base) {
    char p[700];
    const char *tbls[] = {"users", "orders"};
    unlink(base);
    for (size_t i = 0; i < sizeof(tbls) / sizeof(tbls[0]); i++) {
        snprintf(p, sizeof(p), "%s.%s.tbl", base, tbls[i]);
        unlink(p);
        snprintf(p, sizeof(p), "%s.%s.idx", base, tbls[i]);
        unlink(p);
    }
}

int main(void) {
    const char *path = "build/test_join.db";
    cleanup(path);

    Database db;
    db_open(&db, path);

    char *o;
    /* 두 테이블 생성 */
    o = run(&db, "CREATE TABLE users (id INT, name TEXT)"); free(o);
    o = run(&db, "CREATE TABLE orders (oid INT, uid INT, item TEXT)"); free(o);

    /* 여러 테이블을 한 DB가 든다 */
    o = run(&db, "SELECT * FROM users");
    CHECK(strstr(o, "(0행") != NULL, "users 비어 있음");
    free(o);

    o = run(&db, "INSERT INTO users VALUES (1, 'kim')"); free(o);
    o = run(&db, "INSERT INTO users VALUES (2, 'lee')"); free(o);
    o = run(&db, "INSERT INTO users VALUES (3, 'park')"); free(o);
    o = run(&db, "INSERT INTO orders VALUES (10, 1, 'book')"); free(o);
    o = run(&db, "INSERT INTO orders VALUES (11, 1, 'pen')"); free(o);
    o = run(&db, "INSERT INTO orders VALUES (12, 2, 'desk')"); free(o);
    o = run(&db, "INSERT INTO orders VALUES (13, 9, 'ghost')"); free(o); /* 매칭되는 user 없음 */

    /* 두 테이블이 따로 산다(서로 안 섞임) */
    o = run(&db, "SELECT * FROM users");
    CHECK(strstr(o, "(3행") && !strstr(o, "book"), "users는 3행, orders 데이터 안 섞임");
    free(o);
    o = run(&db, "SELECT * FROM orders");
    CHECK(strstr(o, "(4행") && !strstr(o, "kim"), "orders는 4행, users 데이터 안 섞임");
    free(o);

    /* 기본 INNER JOIN: users.id = orders.uid */
    o = run(&db, "SELECT * FROM users JOIN orders ON users.id = orders.uid");
    CHECK(strstr(o, "users.id") && strstr(o, "orders.item"), "조인 헤더에 한정 컬럼명");
    CHECK(strstr(o, "(3행") != NULL, "조인 결과 3행 (kim x2, lee x1; park/ghost 제외)");
    CHECK(strstr(o, "1 | kim | 10 | 1 | book") != NULL, "kim - book이 한 행으로 결합");
    CHECK(!strstr(o, "park") && !strstr(o, "ghost"), "매칭 없는 park/ghost는 빠짐");
    free(o);

    /* 한정 없는 ON: id = uid (id->users, uid->orders 로 자동 해소) */
    o = run(&db, "SELECT * FROM users JOIN orders ON id = uid");
    CHECK(strstr(o, "(3행") != NULL, "한정 없는 ON도 동작");
    free(o);

    /* JOIN + WHERE (오른쪽 테이블 컬럼 한정) */
    o = run(&db, "SELECT * FROM users JOIN orders ON users.id = orders.uid WHERE orders.item = 'pen'");
    CHECK(strstr(o, "kim") && strstr(o, "pen") && !strstr(o, "desk") && strstr(o, "(1행"),
          "WHERE orders.item='pen' -> kim/pen 1행");
    free(o);

    /* JOIN + WHERE (왼쪽 테이블 컬럼 한정) */
    o = run(&db, "SELECT * FROM users JOIN orders ON users.id = orders.uid WHERE users.name = 'kim'");
    CHECK(strstr(o, "book") && strstr(o, "pen") && !strstr(o, "desk") && strstr(o, "(2행"),
          "WHERE users.name='kim' -> kim의 주문 2건");
    free(o);

    /* JOIN + LIMIT */
    o = run(&db, "SELECT * FROM users JOIN orders ON users.id = orders.uid LIMIT 1");
    CHECK(strstr(o, "(1행") != NULL, "JOIN LIMIT 1 -> 1행");
    free(o);

    /* 인덱스 중첩 루프 조인: 안쪽(users)의 PK(id)가 ON 컬럼이면 점 조회로 */
    o = run(&db, "SELECT * FROM orders JOIN users ON orders.uid = users.id");
    CHECK(strstr(o, "(3행") != NULL, "orders x users (uid=id) -> 3행 (uid 9는 매칭 없음)");
    CHECK(db.used_index == 1, "안쪽 PK가 ON 컬럼 -> 인덱스 조인");
    free(o);
    /* 안쪽 컬럼이 PK가 아니면(users x orders, uid는 PK 아님) 전체 스캔 */
    o = run(&db, "SELECT * FROM users JOIN orders ON users.id = orders.uid");
    CHECK(db.used_index == 0, "안쪽 비PK 조인 -> 전체 스캔");
    free(o);

    /* JOIN + ORDER BY: 결합 행을 정렬 (orders.uid 오름차순 -> 1,1,2) */
    o = run(&db, "SELECT * FROM users JOIN orders ON users.id = orders.uid ORDER BY orders.oid");
    {
        char *book = strstr(o, "book"), *pen = strstr(o, "pen"), *desk = strstr(o, "desk");
        CHECK(book && pen && desk && book < pen && pen < desk,
              "ORDER BY orders.oid -> book(10), pen(11), desk(12)");
    }
    free(o);
    /* JOIN + ORDER BY DESC + LIMIT */
    o = run(&db, "SELECT * FROM users JOIN orders ON users.id = orders.uid ORDER BY orders.oid DESC LIMIT 1");
    CHECK(strstr(o, "desk") && !strstr(o, "book") && strstr(o, "(1행"),
          "ORDER BY orders.oid DESC LIMIT 1 -> desk만");
    free(o);
    /* ORDER BY로 왼쪽 테이블 컬럼 정렬 (users.name DESC -> lee, kim, kim) */
    o = run(&db, "SELECT * FROM users JOIN orders ON users.id = orders.uid ORDER BY users.name DESC");
    {
        char *lee = strstr(o, "lee"), *kim = strstr(o, "kim");
        CHECK(lee && kim && lee < kim, "ORDER BY users.name DESC -> lee 먼저");
    }
    free(o);

    /* 재오픈해도 두 테이블 다 살아 있다(카탈로그 + 파일별 영속) */
    db_close(&db);
    db_open(&db, path);
    o = run(&db, "SELECT * FROM users JOIN orders ON users.id = orders.uid");
    CHECK(strstr(o, "(3행") != NULL, "재오픈 후에도 조인 결과 3행");
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
