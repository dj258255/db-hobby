#include "db.h"
#include "wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 트랙 E (ARIES) 회귀 테스트 — "버퍼 풀보다 큰 트랜잭션".
 *
 * 14편 장애 서사의 목표 상태를 표현한다:
 *   - no-steal 시절엔 트랜잭션의 dirty 페이지가 전부 버퍼 풀(WAL_MAX_STAGED=64프레임)에
 *     갇혀, 64페이지를 넘기면 INSERT가 자리를 못 얻어 실패했다(README Scope의 명시된 한계).
 *   - STEAL(dirty 축출 허용) + before-image UNDO를 심으면, 트랜잭션 크기가 버퍼 풀
 *     크기와 분리되어 큰 트랜잭션도 커밋되고, 크래시/롤백에도 안전해야 한다.
 *
 * 단계 1(현재): 이 테스트는 RED다 — 큰 트랜잭션이 아직 못 돈다. 단계 4~5에서 GREEN이 된다.
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
    const char *ext[] = {"t.tbl", "t.idx", "t.wal", "t.idx.wal"};
    unlink(base);
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        snprintf(p, sizeof(p), "%s.%s", base, ext[i]);
        unlink(p);
    }
}

/* 64페이지(버퍼 풀)를 확실히 넘기려고 payload를 크게, 행을 많이 넣는다. */
#define N_ROWS 3000
#define PAYLOAD "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" /* 120 chars */

static int count_rows(Database *db) {
    char *o = run(db, "SELECT COUNT(*) FROM t");
    int n = -1;
    /* 출력에서 첫 정수를 긁는다 */
    for (const char *p = o; *p; p++) {
        if (*p >= '0' && *p <= '9') { n = atoi(p); break; }
    }
    free(o);
    return n;
}

int main(void) {
    const char *path = "build/test_recovery.db";
    char sql[512];

    /* --- 시나리오 A: 버퍼 풀보다 큰 트랜잭션이 커밋되고 재오픈 후에도 남는다 (내구성) --- */
    cleanup(path);
    {
        Database db;
        db_open(&db, path);
        char *o;
        o = run(&db, "CREATE TABLE t (id INT, v TEXT)"); free(o);

        o = run(&db, "BEGIN"); free(o);
        int inserted_ok = 1;
        for (int i = 1; i <= N_ROWS; i++) {
            snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, '%s')", i, PAYLOAD);
            o = run(&db, sql);
            if (strstr(o, "ERROR") || strstr(o, "오류")) { inserted_ok = 0; free(o); break; }
            free(o);
        }
        CHECK(inserted_ok, "버퍼 풀보다 큰 트랜잭션의 모든 INSERT가 성공 (steal 필요)");

        o = run(&db, "COMMIT");
        CHECK(!strstr(o, "ERROR") && !strstr(o, "오류") && strstr(o, "커밋"),
              "큰 트랜잭션이 커밋됨");
        free(o);

        CHECK(count_rows(&db) == N_ROWS, "커밋 직후 전체 행이 조회됨");
        db_close(&db);

        db_open(&db, path);
        CHECK(count_rows(&db) == N_ROWS, "재오픈 후에도 전체 행 유지 (내구성)");
        db_close(&db);
    }

    /* --- 시나리오 B: 커밋된 데이터를 두고 큰 트랜잭션을 ROLLBACK -> 커밋분만 남는다 (원자성) ---
     * steal된 '기존' 페이지(50행이 든 마지막 힙 페이지)를 before-image로 정확히 되돌리는지,
     * steal된 '새' 페이지는 잘라내는지 함께 검증한다. */
    cleanup(path);
    {
        Database db;
        db_open(&db, path);
        char *o;
        o = run(&db, "CREATE TABLE t (id INT, v TEXT)"); free(o);
        for (int i = 1; i <= 50; i++) { /* 커밋된 소량 데이터 (autocommit) */
            snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, '%s')", i, PAYLOAD);
            o = run(&db, sql); free(o);
        }
        CHECK(count_rows(&db) == 50, "커밋된 50행 준비");

        o = run(&db, "BEGIN"); free(o);
        for (int i = 51; i <= 50 + N_ROWS; i++) { /* 버퍼 풀 초과 -> steal */
            snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, '%s')", i, PAYLOAD);
            o = run(&db, sql); free(o);
        }
        o = run(&db, "ROLLBACK"); free(o);

        CHECK(count_rows(&db) == 50, "ROLLBACK 후 커밋된 50행만 남음 (before-image 원복 + 새 페이지 truncate)");
        db_close(&db);

        db_open(&db, path);
        CHECK(count_rows(&db) == 50, "재오픈 후에도 50행 (롤백이 디스크에 반영됨)");
        db_close(&db);
    }

    /* --- 시나리오 C: 큰 트랜잭션이 steal 후 '커밋 전 크래시' -> 재오픈 시 undo (원자성) --- */
    cleanup(path);
    {
        Database db;
        db_open(&db, path);
        char *o;
        o = run(&db, "CREATE TABLE t (id INT, v TEXT)"); free(o);
        for (int i = 1; i <= 50; i++) {
            snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, '%s')", i, PAYLOAD);
            o = run(&db, sql); free(o);
        }
        o = run(&db, "BEGIN"); free(o);
        for (int i = 51; i <= 50 + N_ROWS; i++) { /* steal로 디스크에 미커밋 변경이 샌다 */
            snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, '%s')", i, PAYLOAD);
            o = run(&db, sql); free(o);
        }
        wal_test_crash_before_commit = 1; /* 커밋 마커 없이 멈춘다 */
        o = run(&db, "COMMIT"); free(o);
        wal_test_crash_before_commit = 0;
        db_close(&db); /* 크래시 */

        db_open(&db, path); /* 복구: 마커 없음 -> loser -> before-image로 undo + 새 페이지 truncate */
        CHECK(count_rows(&db) == 50, "크래시(커밋 전) 후 재오픈 -> 커밋된 50행만 (원자성)");
        db_close(&db);
    }

    cleanup(path);
    if (failures == 0) {
        printf("\n전체 통과\n");
        return 0;
    }
    printf("\n%d개 실패 (단계 4~5 전까지는 RED가 정상)\n", failures);
    return 1;
}
