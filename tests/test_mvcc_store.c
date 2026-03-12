#include "db.h"
#include "heap.h"

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

/* 힙을 훑으며 (xmin 기록 여부, MVCC 가시성)으로 행을 센다. */
typedef struct {
    const TxnLog *log;
    int total;
    int visible;
    int has_xmin; /* 모든 행이 0이 아닌 xmin을 가졌나 */
} VisCtx;

static int vis_visit(RID rid, const void *rec, uint16_t len, void *c_) {
    (void)rid;
    (void)len;
    VisCtx *c = c_;
    c->total++;
    int32_t xmin = db_rec_xmin(rec);
    int32_t xmax = db_rec_xmax(rec);
    if (xmin > 0) {
        c->has_xmin++;
    }
    if (mvcc_visible(c->log, xmin, xmax)) {
        c->visible++;
    }
    return 0;
}

int main(void) {
    const char *path = "build/test_mvcc_store.db";
    unlink(path);

    Database db;
    db_open(&db, path);

    char *o;
    o = run(&db, "CREATE TABLE t (id INT)"); free(o);
    o = run(&db, "INSERT INTO t VALUES (1)"); free(o);
    int t1 = db.cur_txn; /* 방금 INSERT의 트랜잭션 id */
    o = run(&db, "INSERT INTO t VALUES (2)"); free(o);
    int t2 = db.cur_txn;
    o = run(&db, "INSERT INTO t VALUES (3)"); free(o);
    int t3 = db.cur_txn;

    /* 각 INSERT가 서로 다른 트랜잭션 id를 행 xmin에 박았다 */
    CHECK(t1 > 0 && t2 > 0 && t3 > 0 && t1 != t2 && t2 != t3,
          "각 INSERT가 행에 서로 다른 xmin을 기록");

    /* autocommit이라 셋 다 커밋됨 -> 가시성 규칙으로도 셋 다 보임 */
    VisCtx c1 = {&db.txnlog, 0, 0, 0};
    heap_scan(&db.tables[0].heap, vis_visit, &c1);
    CHECK(c1.total == 3 && c1.has_xmin == 3, "힙의 3행 모두 0 아닌 xmin을 가짐");
    CHECK(c1.visible == 3, "커밋된 트랜잭션이 만든 3행 -> 모두 보임");

    /* 두 번째 행을 만든 트랜잭션을 아보트시키면, 그 행만 안 보인다 (가시성 규칙) */
    txnlog_abort(&db.txnlog, t2);
    VisCtx c2 = {&db.txnlog, 0, 0, 0};
    heap_scan(&db.tables[0].heap, vis_visit, &c2);
    CHECK(c2.total == 3, "힙엔 여전히 3행이 물리적으로 있음(MVCC는 안 지움)");
    CHECK(c2.visible == 2, "txn2 아보트 -> 그 행만 안 보임(가시 2행)");

    /* SQL 레벨: 닫고 다시 열어도 옛 행이 보인다.
     * (db_close가 next_txn을 영속화 -> 재오픈 시 committed_below로 옛 txn=커밋. 게이트는 select_visit.) */
    db_close(&db);
    Database db2;
    db_open(&db2, path);
    o = run(&db2, "SELECT * FROM t");
    CHECK(strstr(o, "(3행"),
          "재오픈 후 SELECT * -> 3행 (MVCC 게이트 + next_txn 영속화로 옛 행 가시)");
    free(o);
    o = run(&db2, "SELECT * FROM t WHERE id = 2");
    CHECK(strstr(o, "(1행"), "재오픈 후 WHERE도 정상(가시성 게이트 통과)");
    free(o);
    db_close(&db2);

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
