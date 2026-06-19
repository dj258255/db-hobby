/*
 * bench_parallel.c — 병렬 실행(36~39편)이 진짜 빨라졌나 실측.
 *
 * 같은 집계 쿼리를 워커 수 1·2·4·8로 돌려 wall-clock을 재고, 1워커(직렬 기준선)
 * 대비 speedup을 낸다. 두 체제를 대조한다:
 *   ① 워밍(warm): 테이블이 버퍼 풀(64프레임)에 다 올라온 CPU-bound 워크로드.
 *      -> 페이지 fetch가 싸고, 가시성+WHERE+누적이 코어 수만큼 병렬 -> speedup 기대.
 *   ② 콜드(cold): 테이블이 풀보다 커(>64페이지) 스캔마다 thrash.
 *      -> 41편 전엔 pager_read가 풀 latch 안에서 직렬화돼 이득이 작았다(8워커 0.65x).
 *         41편에서 읽기 I/O를 latch 밖으로 빼자 워커들이 병렬로 읽어 이득이 살아났다.
 *
 * 빌드/실행:  make bench-parallel   (자동 -O2)
 */
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *NULLOUT;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static void ex(Database *db, const char *sql) { db_exec(db, sql, NULLOUT); }

static void wipe(const char *path) {
    char buf[600];
    unlink(path);
    const char *suf[] = {".big.tbl", ".big.idx", ".big.wal", ".big.idx.wal"};
    for (int i = 0; i < 4; i++) {
        snprintf(buf, sizeof buf, "%s%s", path, suf[i]);
        unlink(buf);
    }
}

/* big(id INT, v INT)에 1..n 적재. 50행씩 묶어 커밋(스테이징 한계 64 이내). */
static void load(Database *db, int n) {
    ex(db, "CREATE TABLE big (id INT, v INT)");
    char sql[96];
    int in_txn = 0, batch = 0;
    for (int i = 1; i <= n; i++) {
        if (!in_txn) { ex(db, "BEGIN"); in_txn = 1; batch = 0; }
        snprintf(sql, sizeof sql, "INSERT INTO big VALUES (%d, %d)", i, i);
        ex(db, sql);
        if (++batch >= 50) { ex(db, "COMMIT"); in_txn = 0; }
    }
    if (in_txn) ex(db, "COMMIT");
}

/* 한 쿼리를 iters번 돌려 최소 시간(노이즈 최소화)을 초로 반환. */
static double timed(Database *db, const char *sql, int iters) {
    double best = 1e30;
    for (int i = 0; i < iters; i++) {
        double t0 = now_sec();
        db_exec(db, sql, NULLOUT);
        double dt = now_sec() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

/* 워커 1·2·4·8로 재고 speedup 표를 찍는다. */
static void sweep(Database *db, const char *label, const char *sql, int iters) {
    printf("  %s   (iters=%d, 최소시간)\n", label, iters);
    int workers[] = {1, 2, 4, 8};
    double base = 0;
    for (int w = 0; w < 4; w++) {
        db_set_parallel_workers(workers[w]);
        double s = timed(db, sql, iters);
        if (w == 0) base = s;
        printf("    workers=%-2d  %8.3f ms   speedup %4.2fx\n",
               workers[w], s * 1e3, base / s);
    }
    printf("\n");
}

int main(void) {
    NULLOUT = fopen("/dev/null", "w");
    const char *path = "build/bench_parallel.db";

    /* ── ① 워밍 체제: 테이블이 풀(64프레임)에 다 올라온다 (~45페이지) ── */
    wipe(path);
    Database wdb;
    db_open(&wdb, path);
    int WARM_N = 15000; /* ~45페이지 < 64 -> warm */
    printf("적재(warm): %d행...\n", WARM_N);
    load(&wdb, WARM_N);
    /* 캐시 워밍(한 번 훑어 페이지를 다 올린다) */
    ex(&wdb, "SELECT COUNT(*) FROM big");
    printf("\n[워밍 체제 — CPU-bound, 페이지가 풀에 상주]\n\n");
    sweep(&wdb, "SUM(v) 전체 스캔(가벼운 per-row)", "SELECT SUM(v) FROM big", 500);
    sweep(&wdb, "COUNT(*) WHERE v>7500(가시성+WHERE)", "SELECT COUNT(*) FROM big WHERE v > 7500", 500);
    sweep(&wdb, "COUNT,SUM,MIN,MAX WHERE(무거운 per-row)",
          "SELECT COUNT(*), SUM(v), MIN(v), MAX(v) FROM big WHERE v > 5000", 500);
    db_close(&wdb);

    /* ── ② 콜드 체제: 테이블이 풀보다 크다 (>64페이지) -> thrash ── */
    wipe(path);
    Database cdb;
    db_open(&cdb, path);
    int COLD_N = 120000; /* ~360페이지 > 64 -> 스캔마다 재적재 */
    printf("적재(cold): %d행...\n", COLD_N);
    load(&cdb, COLD_N);
    printf("\n[콜드 체제 — 테이블>풀. 41편 A/B: 읽기 I/O를 latch 안 vs 밖]\n\n");
    const char *cold_q = "SELECT SUM(v) FROM big";
    bufpool_set_io_in_latch(1); /* 41편 이전: I/O를 latch 안에서(직렬) */
    sweep(&cdb, "before — pager_read를 latch 안에서(직렬 I/O)", cold_q, 20);
    bufpool_set_io_in_latch(0); /* 41편: I/O를 latch 밖에서(병렬) */
    sweep(&cdb, "after  — pager_read를 latch 밖에서(병렬 I/O)", cold_q, 20);
    db_close(&cdb);

    db_set_parallel_workers(4); /* 원복 */
    fclose(NULLOUT);
    printf("끝. (해석: 워밍=CPU-bound 병렬 이득. 콜드=41편에서 pager_read를 풀 latch 밖으로\n");
    printf("      빼 병렬 읽기가 가능해져 이득이 살아남. 남은 천장: 스레드 오버헤드·작은 풀·\n");
    printf("      dirty 쓰기 latch·engine_mtx -> 더 큰/세밀한 풀과 engine_mtx 제거가 프론티어)\n");
    return 0;
}
