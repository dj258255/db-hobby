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

/* 보조 인덱스에서 한 키의 항목 수를 세는 콜백 */
static int count_visit(bkey_t k, bval_t v, void *ctx_) {
    (void)k;
    (void)v;
    (*(int *)ctx_)++;
    return 0;
}
static int find_count(BTree *bt, bkey_t key) {
    int c = 0;
    btree_find_all(bt, key, count_visit, &c);
    return c;
}

int main(void) {
    const char *path = "build/test_secindex.db";
    unlink(path);

    Database db;
    db_open(&db, path);

    char *o;
    o = run(&db, "CREATE TABLE t (id INT, age INT, name TEXT)"); free(o);
    o = run(&db, "INSERT INTO t VALUES (1, 20, 'a')"); free(o);
    o = run(&db, "INSERT INTO t VALUES (2, 30, 'b')"); free(o);
    o = run(&db, "INSERT INTO t VALUES (3, 20, 'c')"); free(o);
    o = run(&db, "INSERT INTO t VALUES (4, 20, 'd')"); free(o);
    o = run(&db, "INSERT INTO t VALUES (5, 30, 'e')"); free(o);

    /* 보조 인덱스 생성 */
    o = run(&db, "CREATE INDEX age_idx ON t(age)");
    CHECK(strstr(o, "생성됨") && strstr(o, "5개"), "CREATE INDEX -> 5개 행 색인");
    free(o);

    Table *t = &db.tables[0];
    CHECK(t->num_sec == 1, "보조 인덱스 1개 등록됨");
    CHECK(strcmp(t->sec[0].name, "age_idx") == 0 && t->sec[0].col == 1,
          "인덱스 메타: 이름=age_idx, 컬럼=age(1)");

    /* 비유니크: age=20 세 행, age=30 두 행, 없는 값 0행 */
    CHECK(find_count(&t->sec[0].tree, 20) == 3, "find_all(age=20) -> 3행");
    CHECK(find_count(&t->sec[0].tree, 30) == 2, "find_all(age=30) -> 2행");
    CHECK(find_count(&t->sec[0].tree, 99) == 0, "find_all(age=99) -> 0행");

    /* 오류 케이스 */
    o = run(&db, "CREATE INDEX bad ON t(name)");
    CHECK(strstr(o, "ERROR") && strstr(o, "INT"), "TEXT 컬럼 인덱스 -> 거부");
    free(o);
    o = run(&db, "CREATE INDEX bad ON t(nope)");
    CHECK(strstr(o, "ERROR") && strstr(o, "컬럼"), "없는 컬럼 -> 거부");
    free(o);
    o = run(&db, "CREATE INDEX bad ON nosuch(age)");
    CHECK(strstr(o, "ERROR") && strstr(o, "테이블"), "없는 테이블 -> 거부");
    free(o);
    o = run(&db, "CREATE INDEX age_idx ON t(id)");
    CHECK(strstr(o, "ERROR") && strstr(o, "이미"), "이름 중복 -> 거부");
    free(o);

    /* DML 유지보수: INSERT가 보조 인덱스도 갱신한다 */
    o = run(&db, "INSERT INTO t VALUES (6, 20, 'f')"); free(o);
    CHECK(find_count(&t->sec[0].tree, 20) == 4, "INSERT 후 find_all(age=20) -> 4행");

    /* 인덱스도 WAL로 묶이므로 롤백 시 함께 되돌아간다 */
    o = run(&db, "BEGIN"); free(o);
    o = run(&db, "INSERT INTO t VALUES (7, 20, 'g')"); free(o);
    CHECK(find_count(&t->sec[0].tree, 20) == 5, "트랜잭션 중 INSERT -> 인덱스 5행");
    o = run(&db, "ROLLBACK"); free(o);
    CHECK(find_count(&t->sec[0].tree, 20) == 4, "ROLLBACK -> 인덱스 4행으로 복원");

    /* 영속성: 닫고 다시 열어도 보조 인덱스가 살아 있다 (id=6 커밋분 포함, id=7 롤백분 제외) */
    db_close(&db);
    Database db2;
    db_open(&db2, path);
    Table *t2 = &db2.tables[0];
    CHECK(t2->num_sec == 1 && strcmp(t2->sec[0].name, "age_idx") == 0 && t2->sec[0].col == 1,
          "재오픈 후 보조 인덱스 메타 복원");
    CHECK(find_count(&t2->sec[0].tree, 20) == 4, "재오픈 후 find_all(age=20) -> 4행(커밋분 영속)");
    db_close(&db2);

    /* 정리 */
    unlink(path);
    {
        const char *suf[] = {".t.tbl",       ".t.idx",         ".t.wal",
                             ".t.idx.wal",   ".t.age_idx.idx", ".t.age_idx.idx.wal"};
        char buf[128];
        for (int i = 0; i < 6; i++) {
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
