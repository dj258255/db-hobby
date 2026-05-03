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

/* 격리 시연 — 16·18편 이후의 의미론:
 *   쓰기 vs 쓰기 = 테이블 X락으로 즉시 거부(first-updater-wins).
 *   읽기 = 락 없음. reader는 writer에게 절대 안 막힌다(MVCC 가시성이 격리를 맡는다).
 * 다른 트랜잭션(T99)의 락을 직접 주입해 충돌을 시연한다. */
int main(void) {
    const char *path = "build/test_isolation.db";
    unlink(path);

    Database db;
    db_open(&db, path);

    char *o;
    o = run(&db, "CREATE TABLE t (id INT, v INT)"); free(o);
    o = run(&db, "INSERT INTO t VALUES (1, 10)"); free(o);
    o = run(&db, "INSERT INTO t VALUES (2, 20)"); free(o);

    /* --- T99가 t를 X로 잠근 상황(다른 트랜잭션이 쓰는 중) --- */
    lock_acquire(&db.lm, 99, "t", 0, LOCK_X);

    o = run(&db, "SELECT * FROM t WHERE id = 1");
    CHECK(!strstr(o, "ERROR") && strstr(o, "(1행"),
          "X로 잠긴 t를 SELECT -> 정상 (reader는 writer에게 안 막힌다 — MVCC)");
    free(o);
    o = run(&db, "INSERT INTO t VALUES (3, 30)");
    CHECK(strstr(o, "ERROR") && strstr(o, "잠겨"),
          "X로 잠긴 t에 INSERT -> 거부 (write-write = first-updater-wins)");
    free(o);

    lock_release_all(&db.lm, 99);

    o = run(&db, "SELECT * FROM t");
    CHECK(strstr(o, "(2행"), "거부된 INSERT는 반영 안 됨(여전히 2행)");
    free(o);

    /* --- S락(레거시 호환 행렬)은 락 매니저 차원에서 여전히 X와 충돌한다 --- */
    lock_acquire(&db.lm, 99, "t", 0, LOCK_S);
    o = run(&db, "SELECT * FROM t WHERE id = 1");
    CHECK(!strstr(o, "ERROR"), "S로 잠긴 t를 SELECT -> 정상 (읽기는 락을 안 잡는다)");
    free(o);
    o = run(&db, "INSERT INTO t VALUES (3, 30)");
    CHECK(strstr(o, "ERROR"), "S로 잠긴 t에 INSERT(X) -> 충돌 (호환 행렬)");
    free(o);
    lock_release_all(&db.lm, 99);

    /* --- 역방향: 진짜 트랜잭션이 잠그면 끝(COMMIT)까지 다른 txn이 막힌다 (2PL) --- */
    o = run(&db, "BEGIN"); free(o);
    o = run(&db, "UPDATE t SET v = 99 WHERE id = 1");
    CHECK(!strstr(o, "ERROR"), "BEGIN 후 UPDATE -> t에 X 락 획득");
    free(o);
    CHECK(lock_acquire(&db.lm, 99, "t", 0, LOCK_X) == -1,
          "쓰는 트랜잭션의 t에 T99 X -> 충돌 (write-write)");
    CHECK(lock_acquire(&db.lm, 99, "t", 0, LOCK_S) == -1,
          "...T99 S도 충돌 (커밋 전 값을 못 읽음)");
    o = run(&db, "COMMIT"); free(o);
    CHECK(lock_acquire(&db.lm, 99, "t", 0, LOCK_X) == 0, "COMMIT 후 락 풀려 T99가 X 획득");
    lock_release_all(&db.lm, 99);

    db_close(&db);
    unlink(path);
    {
        const char *suf[] = {".t.tbl", ".t.idx", ".t.wal", ".t.idx.wal"};
        char buf[128];
        for (int i = 0; i < 4; i++) {
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
