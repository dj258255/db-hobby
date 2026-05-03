#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 다중 트랜잭션 (A1-3·4) — 13편이 "코어 재작성 프론티어"로 남긴 그 시연.
 *   ① reader는 writer에게 안 막힌다: T1이 미커밋 UPDATE를 든 동안 T2의 SELECT는
 *      거부되지 않고 '옛 버전'을 읽는다 (MVCC의 존재 이유).
 *   ② 스냅샷 격리: BEGIN 이후에 커밋된 남의 변경은 트랜잭션이 끝날 때까지 안 보인다.
 *   ③ write-write는 테이블 X락으로 즉시 거부 = first-updater-wins.
 *   ④ 세션별 커밋/롤백은 자기가 쓴 테이블만 건드린다.
 *   ⑤ 두 트랜잭션이 열린 채 크래시 -> 재오픈 시 둘 다 원자적으로 사라진다.
 */

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
    const char *ext[] = {"t.tbl", "t.idx", "t.wal", "t.idx.wal",
                         "a.tbl", "a.idx", "a.wal", "a.idx.wal",
                         "b.tbl", "b.idx", "b.wal", "b.idx.wal"};
    unlink(base);
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        snprintf(p, sizeof(p), "%s.%s", base, ext[i]);
        unlink(p);
    }
}

int main(void) {
    const char *path = "build/test_multitxn.db";
    cleanup(path);

    Database db;
    db_open(&db, path);
    char *o;

    o = run(&db, "CREATE TABLE t (id INT, v INT)"); free(o);
    o = run(&db, "INSERT INTO t VALUES (1, 100)"); free(o);

    /* --- ① reader가 writer에게 안 막힌다 (그리고 dirty read도 없다) --- */
    o = run(&db, "SESSION 0"); free(o);
    o = run(&db, "BEGIN"); free(o);
    o = run(&db, "UPDATE t SET v = 999 WHERE id = 1"); free(o); /* 미커밋, X락 보유 */
    o = run(&db, "SELECT * FROM t WHERE id = 1");
    CHECK(strstr(o, "999") != NULL, "writer(세션0) 자신은 자기 미커밋 값을 본다");
    free(o);

    o = run(&db, "SESSION 1"); free(o);
    o = run(&db, "SELECT * FROM t WHERE id = 1");
    CHECK(!strstr(o, "ERROR"), "reader(세션1)는 writer에게 안 막힌다 — 거부 없음");
    CHECK(strstr(o, "100") && !strstr(o, "999"),
          "reader는 옛 버전(100)을 본다 — dirty read 없음 (락이 아니라 가시성)");
    free(o);

    /* --- ③ write-write = first-updater-wins --- */
    o = run(&db, "UPDATE t SET v = 555 WHERE id = 1");
    CHECK(strstr(o, "ERROR") && strstr(o, "잠겨"),
          "두 번째 writer(세션1)의 UPDATE -> 즉시 거부 (first-updater-wins)");
    free(o);
    o = run(&db, "SELECT * FROM t WHERE id = 1");
    CHECK(strstr(o, "100"), "거부당한 뒤에도 세션1은 여전히 읽을 수 있다");
    free(o);

    /* --- 세션0 커밋 -> 트랜잭션 밖 reader는 새 값을 본다 (read committed) --- */
    o = run(&db, "SESSION 0"); free(o);
    o = run(&db, "COMMIT"); free(o);
    o = run(&db, "SESSION 1"); free(o);
    o = run(&db, "SELECT * FROM t WHERE id = 1");
    CHECK(strstr(o, "999"), "커밋 후 트랜잭션 밖 SELECT -> 새 값(999)");
    free(o);

    /* --- ② 스냅샷 격리: BEGIN 이후에 커밋된 남의 변경은 안 보인다 --- */
    o = run(&db, "BEGIN"); free(o); /* 세션1 트랜잭션 시작(스냅샷 고정) */
    o = run(&db, "SESSION 0"); free(o);
    o = run(&db, "UPDATE t SET v = 777 WHERE id = 1"); free(o); /* autocommit 커밋됨 */
    o = run(&db, "SELECT * FROM t WHERE id = 1");
    CHECK(strstr(o, "777"), "세션0(트랜잭션 밖)은 방금 커밋한 777을 본다");
    free(o);
    o = run(&db, "SESSION 1"); free(o);
    o = run(&db, "SELECT * FROM t WHERE id = 1");
    CHECK(strstr(o, "999") && !strstr(o, "777"),
          "세션1(스냅샷)은 여전히 999 — 시작 이후의 커밋은 안 보인다 (REPEATABLE READ)");
    free(o);
    o = run(&db, "COMMIT"); free(o);
    o = run(&db, "SELECT * FROM t WHERE id = 1");
    CHECK(strstr(o, "777"), "트랜잭션을 끝내면 최신 커밋(777)이 보인다");
    free(o);

    /* --- 시작 시점에 '진행 중'이던 트랜잭션도 (나중에 커밋해도) 안 보인다 --- */
    o = run(&db, "SESSION 0"); free(o);
    o = run(&db, "BEGIN"); free(o);
    o = run(&db, "INSERT INTO t VALUES (2, 200)"); free(o); /* 미커밋 */
    o = run(&db, "SESSION 1"); free(o);
    o = run(&db, "BEGIN"); free(o); /* 이 시점: 세션0 진행 중 -> 스냅샷에서 제외 */
    o = run(&db, "SESSION 0"); free(o);
    o = run(&db, "COMMIT"); free(o); /* 세션1 시작 후에 커밋됨 */
    o = run(&db, "SESSION 1"); free(o);
    o = run(&db, "SELECT COUNT(*) FROM t");
    CHECK(strstr(o, "1") && !strstr(o, "2\n"),
          "시작 시점에 진행 중이던 트랜잭션의 INSERT는 커밋됐어도 안 보임 (스냅샷)");
    free(o);
    o = run(&db, "COMMIT"); free(o);
    o = run(&db, "SELECT COUNT(*) FROM t");
    CHECK(strstr(o, "2"), "트랜잭션 종료 후엔 보인다");
    free(o);

    /* --- ④ 서로 다른 테이블에 쓰는 두 트랜잭션: 커밋/롤백이 독립 --- */
    o = run(&db, "CREATE TABLE a (id INT, v INT)"); free(o);
    o = run(&db, "CREATE TABLE b (id INT, v INT)"); free(o);
    o = run(&db, "SESSION 0"); free(o);
    o = run(&db, "BEGIN"); free(o);
    o = run(&db, "INSERT INTO a VALUES (1, 1)"); free(o);
    o = run(&db, "SESSION 1"); free(o);
    o = run(&db, "BEGIN"); free(o);
    o = run(&db, "INSERT INTO b VALUES (1, 1)"); free(o);
    o = run(&db, "SESSION 0"); free(o);
    o = run(&db, "COMMIT"); free(o);
    o = run(&db, "SESSION 1"); free(o);
    o = run(&db, "ROLLBACK"); free(o);
    o = run(&db, "SELECT COUNT(*) FROM a");
    CHECK(strstr(o, "1"), "세션0이 커밋한 a의 행은 남는다");
    free(o);
    o = run(&db, "SELECT COUNT(*) FROM b");
    CHECK(strstr(o, "0"), "세션1이 롤백한 b의 행은 사라진다 (커밋/롤백이 테이블별로 독립)");
    free(o);

    /* --- SESSION 범위 오류 --- */
    o = run(&db, "SESSION 99");
    CHECK(strstr(o, "ERROR") != NULL, "범위 밖 세션 번호 -> 거부");
    free(o);

    /* --- ⑤ 두 트랜잭션이 열린 채 닫힘 -> 진짜 DB처럼 둘 다 롤백된다 --- */
    o = run(&db, "SESSION 0"); free(o);
    o = run(&db, "BEGIN"); free(o);
    o = run(&db, "INSERT INTO a VALUES (2, 2)"); free(o);
    o = run(&db, "SESSION 1"); free(o);
    o = run(&db, "BEGIN"); free(o);
    o = run(&db, "INSERT INTO b VALUES (2, 2)"); free(o);
    db_close(&db); /* 커넥션 끊김 — 열린 트랜잭션은 abort된다 (PG와 동일) */

    db_open(&db, path);
    o = run(&db, "SELECT COUNT(*) FROM a");
    CHECK(strstr(o, "1"), "재오픈 후 a: 커밋된 1행만 (닫힐 때 열린 트랜잭션은 롤백)");
    free(o);
    o = run(&db, "SELECT COUNT(*) FROM b");
    CHECK(strstr(o, "0"), "재오픈 후 b: 0행 (두 세션의 미커밋이 모두 롤백)");
    free(o);
    o = run(&db, "SELECT * FROM t WHERE id = 1");
    CHECK(strstr(o, "777"), "커밋된 데이터는 그대로 (내구성)");
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
