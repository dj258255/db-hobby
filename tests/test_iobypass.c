#include "db.h"
#include "bufpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 읽기 I/O를 풀 latch 밖으로 뺀 것(41편, read-in-progress)을 검증한다.
 * 콜드 테이블(>버퍼 풀 64프레임)을 여러 워커로 동시에 훑으면, 워커들이 서로 다른/
 * 같은 페이지를 동시에 miss한다 -> io_pending 예약 + 대기 경로를 강하게 exercise한다.
 *   ① 8워커 콜드 집계가 수학적 정답과 일치(동시 로딩이 정확).
 *   ② io_in_latch 0(밖) vs 1(안) 두 경로가 바이트 동일한 결과(둘 다 정확).
 * data race 부재는 test_iobypass_tsan(ThreadSanitizer 빌드)이 확인한다.
 */

static int failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (cond) printf("  ok   %s\n", msg);                                   \
        else { printf("  FAIL %s\n", msg); failures++; }                        \
    } while (0)

static char *run(Database *db, const char *sql) {
    char *buf = NULL;
    size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    db_exec(db, sql, f);
    fclose(f);
    return buf;
}
static long scalar_val(const char *out) {
    const char *nl = strchr(out, '\n');
    return nl ? atol(nl + 1) : -999999;
}

int main(void) {
    const char *path = "build/test_iobypass.db";
    unlink(path);
    Database db;
    db_open(&db, path);
    char *o;

    /* 콜드: 70000행(~210페이지) > 풀 64프레임 -> 스캔마다 재적재 -> 동시 miss 다발 */
    o = run(&db, "CREATE TABLE big (id INT, v INT)"); free(o);
    const int N = 70000;
    int in_txn = 0, batch = 0;
    for (int i = 1; i <= N; i++) {
        char sql[96];
        if (!in_txn) { o = run(&db, "BEGIN"); free(o); in_txn = 1; batch = 0; }
        snprintf(sql, sizeof(sql), "INSERT INTO big VALUES (%d, %d)", i, i);
        o = run(&db, sql); free(o);
        if (++batch >= 50) { o = run(&db, "COMMIT"); free(o); in_txn = 0; }
    }
    if (in_txn) { o = run(&db, "COMMIT"); free(o); }

    /* oracle: SUM(1..70000) = 70000*70001/2 = 2450035000 */
    const long SUM_ORACLE = 2450035000L;

    /* ── ① 8워커 콜드 집계가 정확(동시 페이지 로딩) ── */
    db_set_parallel_workers(8);
    o = run(&db, "SELECT SUM(v) FROM big");
    CHECK(scalar_val(o) == SUM_ORACLE, "8워커 콜드 SUM = 2450035000 (동시 로딩 정확)");
    free(o);
    o = run(&db, "SELECT COUNT(*), MIN(v), MAX(v) FROM big");
    CHECK(strstr(o, "70000 | 1 | 70000") != NULL, "8워커 콜드 COUNT/MIN/MAX 정확");
    free(o);

    /* ── ② io_in_latch 0(밖) vs 1(안): 두 경로 결과 바이트 동일 ── */
    const char *q = "SELECT SUM(v), COUNT(*), MAX(v) FROM big WHERE v > 35000";
    bufpool_set_io_in_latch(0);
    char *out_off = run(&db, q);
    bufpool_set_io_in_latch(1);
    char *out_on = run(&db, q);
    bufpool_set_io_in_latch(0);
    CHECK(strcmp(out_off, out_on) == 0, "I/O latch 밖(41편) vs 안(이전): 결과 바이트 동일");
    free(out_off);
    free(out_on);

    /* ── ③ 스트리밍 SELECT도 콜드 동시 스캔에서 정확 ── */
    o = run(&db, "SELECT * FROM big WHERE v > 69995"); /* 69996..70000 = 5행 */
    CHECK(strstr(o, "69996 | 69996") && strstr(o, "70000 | 70000"),
          "콜드 병렬 스트리밍 SELECT: 정확한 행(69996..70000)");
    free(o);

    db_set_parallel_workers(4);
    db_close(&db);
    unlink(path);

    if (failures == 0) { printf("\n모든 테스트 통과\n"); return 0; }
    printf("\n%d개 실패\n", failures);
    return 1;
}
