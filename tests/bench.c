/*
 * bench.c — db-hobby 실측 벤치마크.
 *
 * 두 가지를 잰다:
 *   ① 같은 한 행을 찾을 때 인덱스 점 조회(O(log n)) vs 풀 스캔(O(n)) 지연.
 *      테이블 크기 N을 1천 -> 1만 -> 10만으로 키우며 둘의 발산을 본다.
 *   ② 커밋 묶기(fsync 분할 상환)의 효과 = WAL 내구성 비용.
 *      행마다 커밋(행당 fsync) vs 50행씩 묶어 커밋.
 *
 * 빌드/실행:  make bench   (자동으로 -O2로 컴파일해 실행)
 */
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *NULLOUT; /* SELECT 출력은 버린다 — 엔진 시간만 잰다 */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 재현 가능한 난수(xorshift64) — time/rand 시드 없이 고정 */
static unsigned long rng = 88172645463325252UL;
static unsigned long xorshift(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void ex(Database *db, const char *sql) { db_exec(db, sql, NULLOUT); }

/* 테이블 파일들을 싹 지운다(<path> + <path>.t.*) */
static void wipe(const char *path) {
    char buf[600];
    unlink(path);
    const char *suf[] = {".t.tbl", ".t.idx", ".t.wal", ".t.idx.wal"};
    for (int i = 0; i < 4; i++) {
        snprintf(buf, sizeof buf, "%s%s", path, suf[i]);
        unlink(buf);
    }
}

/* t(id INT, name TEXT)에 1..n 적재. 50행마다 커밋(스테이징 한계 64 이내). */
static void load(Database *db, int n) {
    ex(db, "CREATE TABLE t (id INT, name TEXT)");
    char sql[128];
    const int BATCH = 50;
    for (int i = 1; i <= n; i++) {
        if (i % BATCH == 1) ex(db, "BEGIN");
        snprintf(sql, sizeof sql, "INSERT INTO t VALUES (%d, 'u%d')", i, i);
        ex(db, sql);
        if (i % BATCH == 0 || i == n) ex(db, "COMMIT");
    }
}

/* N행 테이블에서 점 조회 vs 풀 스캔 평균 지연(마이크로초). */
static void measure_lookup(int n, int iter_idx, int iter_scan, int verify) {
    char path[64];
    snprintf(path, sizeof path, "build/bench_%d.db", n);
    wipe(path);

    Database db;
    db_open(&db, path);
    load(&db, n);

    char sql[128];

    if (verify) {
        /* 정확성 확인: PK 조건은 인덱스, 비PK 조건은 풀 스캔이어야 한다 */
        ex(&db, "SELECT * FROM t WHERE id = 1");
        int used_idx = db.used_index;
        ex(&db, "SELECT * FROM t WHERE name = 'u1'");
        int used_scan = db.used_index;
        printf("  (확인: id=? -> used_index=%d, name=? -> used_index=%d)\n",
               used_idx, used_scan);
    }

    /* ① 인덱스 점 조회: WHERE id = <랜덤 PK> */
    double t0 = now_sec();
    for (int k = 0; k < iter_idx; k++) {
        int id = 1 + (int)(xorshift() % (unsigned long)n);
        snprintf(sql, sizeof sql, "SELECT * FROM t WHERE id = %d", id);
        ex(&db, sql);
    }
    double idx_us = (now_sec() - t0) / iter_idx * 1e6;

    /* ② 풀 스캔: WHERE name = 'u<랜덤>' (비PK라 전 행 훑음) */
    t0 = now_sec();
    for (int k = 0; k < iter_scan; k++) {
        int id = 1 + (int)(xorshift() % (unsigned long)n);
        snprintf(sql, sizeof sql, "SELECT * FROM t WHERE name = 'u%d'", id);
        ex(&db, sql);
    }
    double scan_us = (now_sec() - t0) / iter_scan * 1e6;

    printf("  N=%-7d  인덱스 점조회 %8.2f us   풀스캔 %10.2f us   배율 %7.0fx\n",
           n, idx_us, scan_us, scan_us / idx_us);

    db_close(&db);
    wipe(path);
}

/* 커밋 묶기 효과: 행당 커밋 vs 50행 묶음 커밋. 적재 처리량(rows/sec). */
static void measure_commit(int m) {
    char path[64];
    char sql[128];

    /* (a) 행당 autocommit — 행마다 WAL fsync */
    snprintf(path, sizeof path, "build/bench_auto.db");
    wipe(path);
    Database a;
    db_open(&a, path);
    ex(&a, "CREATE TABLE t (id INT, name TEXT)");
    double t0 = now_sec();
    for (int i = 1; i <= m; i++) {
        snprintf(sql, sizeof sql, "INSERT INTO t VALUES (%d, 'u%d')", i, i);
        ex(&a, sql); /* BEGIN/COMMIT 없음 -> 문장별 autocommit -> 행당 fsync */
    }
    double auto_s = now_sec() - t0;
    db_close(&a);
    wipe(path);

    /* (b) 50행 묶음 커밋 — fsync 1/50 */
    snprintf(path, sizeof path, "build/bench_batch.db");
    wipe(path);
    Database b;
    db_open(&b, path);
    db_close(&b); /* load()가 CREATE부터 하니 다시 연다 */
    db_open(&b, path);
    t0 = now_sec();
    load(&b, m);
    double batch_s = now_sec() - t0;
    db_close(&b);
    wipe(path);

    printf("  행당 커밋   %6d행  %7.3f s   %10.0f rows/s   (fsync %d회)\n",
           m, auto_s, m / auto_s, m);
    printf("  50행 묶음   %6d행  %7.3f s   %10.0f rows/s   (fsync ~%d회)\n",
           m, batch_s, m / batch_s, (m + 49) / 50);
    printf("  -> 묶음이 %.1f배 빠름 (차이는 대부분 fsync 횟수)\n", auto_s / batch_s);
}

int main(void) {
    NULLOUT = fopen("/dev/null", "w");
    if (!NULLOUT) {
        perror("open /dev/null");
        return 1;
    }

    printf("=== db-hobby 벤치마크 ===\n\n");

    printf("[1] 한 행 찾기: 인덱스 점 조회(O(log n)) vs 풀 스캔(O(n))\n");
    measure_lookup(1000, 5000, 3000, 1);
    measure_lookup(10000, 5000, 1000, 0);
    measure_lookup(100000, 5000, 300, 0);
    printf("\n");

    printf("[2] WAL 내구성 비용: 행당 커밋 vs 묶음 커밋 (적재 처리량)\n");
    measure_commit(5000);

    fclose(NULLOUT);
    return 0;
}
