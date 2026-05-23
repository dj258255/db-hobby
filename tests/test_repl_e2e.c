#include "db.h"
#include "replica.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 캡스톤 — 복제를 '진짜 엔진'에 배선한 end-to-end 테스트.
 *
 * 25·26편의 replica.c는 합성 WAL로만 검증했다(독립 모듈). 이 테스트는 그 모듈이
 * 실제 db.c 엔진이 커밋한 WAL을 그대로 복제함을 증명한다 — "독립 모듈"을
 * "진짜 복제되는 DB"로 잇는다.
 *
 * 흐름(pg_basebackup + WAL 스트리밍 모델):
 *   1) primary: 진짜 db_exec로 CREATE TABLE + INSERT (실제 엔진이 .tbl/.wal에 씀)
 *   2) CREATE 직후 빈 힙을 base 스냅샷으로 복사(구조/page0 포함)
 *   3) primary가 열려 있는 동안(재오픈 금지 — LSN 리셋 landmine) INSERT들이
 *      .wal에 커밋 세그먼트로 쌓임
 *   4) replica.c가 primary의 .wal을 tail·replay -> replica 힙이 primary와 바이트 동일
 *   5) 카탈로그 복사 후 replica를 '진짜 Database'로 열어 SELECT -> 복제된 행을 본다
 *
 * 정직한 경계: primary를 재오픈하면 wal_open이 .wal을 truncate하고 LSN이 1로
 * 리셋되므로, 이 데모는 primary가 살아있는 한 세션 안에서 성립한다(연속 스트리밍
 * 모델). 복제 슬롯·다중 테이블·인덱스 스트리밍·live 소켓(--replica 모드)은 프론티어.
 */

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } } while (0)

#define PRIM "build/repl_primary.db"
#define REPL "build/repl_replica.db"

static char *run(Database *db, const char *sql) {
    char *buf = NULL; size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    db_exec(db, sql, f);
    fclose(f);
    return buf;
}

static int copy_file(const char *src, const char *dst) {
    FILE *s = fopen(src, "rb");
    if (!s) return -1;
    FILE *d = fopen(dst, "wb");
    if (!d) { fclose(s); return -1; }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, s)) > 0) {
        if (fwrite(buf, 1, n, d) != n) { rc = -1; break; }
    }
    fclose(s); fclose(d);
    return rc;
}

/* 두 파일이 바이트 동일한가. */
static int files_equal(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
    int eq = 1, ca, cb;
    do { ca = fgetc(fa); cb = fgetc(fb); if (ca != cb) { eq = 0; break; } } while (ca != EOF);
    fclose(fa); fclose(fb);
    return eq;
}

static void cleanup(const char *prefix) {
    const char *suf[] = {"", ".t.tbl", ".t.wal", ".t.idx", ".t.idx.wal"};
    char p[600];
    for (size_t i = 0; i < sizeof(suf) / sizeof(suf[0]); i++) {
        snprintf(p, sizeof p, "%s%s", prefix, suf[i]);
        unlink(p);
    }
}

int main(void) {
    printf("== 캡스톤: 복제를 진짜 엔진에 배선 (primary --WAL--> replica) ==\n");
    cleanup(PRIM);
    cleanup(REPL);

    /* ── 1. primary: 실제 엔진으로 스키마 + 데이터 ─────────────── */
    Database prim;
    if (db_open(&prim, PRIM) != 0) { printf("  FAIL primary 열기\n"); return 1; }
    free(run(&prim, "CREATE TABLE t (id INT, name TEXT)"));

    /* ── 2. base 스냅샷: CREATE 직후 빈 힙(구조/page0) 복사 ────── */
    int based = copy_file(PRIM ".t.tbl", REPL ".t.tbl");
    CHECK(based == 0, "base: CREATE 직후 빈 힙 스냅샷 복사");

    /* ── 3. primary가 열린 채로 INSERT(실제 커밋이 .wal에 쌓임) ── */
    for (int i = 1; i <= 10; i++) {
        char sql[128];
        snprintf(sql, sizeof sql, "INSERT INTO t VALUES (%d, 'name_%d')", i, i);
        free(run(&prim, sql));
    }
    /* primary를 '닫아' 카탈로그(next_txn)를 확정 — 닫기는 .wal을 안 지운다.
     * (재'오픈'만 truncate+LSN리셋. 우리는 다시 열지 않는다.) */
    db_close(&prim);

    /* ── 4. replica.c가 primary의 실제 .wal을 replay ──────────── */
    WalReplica r;
    if (replica_open(&r, REPL ".t.tbl", PRIM ".t.wal") != 0) {
        printf("  FAIL replica 열기\n"); return 1;
    }
    int applied = 0, n;
    while ((n = replica_apply(&r)) > 0) applied += n;
    CHECK(n == 0, "replica_apply 정상 종료(오류 없음)");
    CHECK(applied >= 10, "복제: primary가 커밋한 세그먼트들을 재생(>=10 커밋)");
    replica_close(&r);

    /* 힙이 primary와 바이트 동일 = 실제 엔진 WAL로 완전 복제됨 */
    CHECK(files_equal(PRIM ".t.tbl", REPL ".t.tbl"),
          "복제: replica 힙이 primary와 바이트 동일(실엔진 WAL 재생)");

    /* ── 5. replica를 '진짜 Database'로 열어 SELECT ──────────────
     * 카탈로그(정확한 next_txn)와 인덱스를 base로 복사 -> 완전한 복제본. */
    CHECK(copy_file(PRIM, REPL) == 0, "복제본 카탈로그 복사(next_txn 포함)");
    copy_file(PRIM ".t.idx", REPL ".t.idx");
    copy_file(PRIM ".t.idx.wal", REPL ".t.idx.wal");

    Database rep;
    if (db_open(&rep, REPL) != 0) { printf("  FAIL replica DB 열기\n"); return 1; }
    char *out = run(&rep, "SELECT * FROM t");
    CHECK(strstr(out, "name_1") != NULL && strstr(out, "name_10") != NULL,
          "복제본 SELECT가 복제된 행(name_1..name_10)을 본다");
    int rows = 0;
    for (const char *p = out; (p = strstr(p, "name_")) != NULL; p++) rows++;
    CHECK(rows == 10, "복제본 SELECT가 10개 행 전부 반환");
    free(out);

    /* PK 인덱스도 복사됐으니 인덱스 점 조회도 동작 */
    char *out2 = run(&rep, "SELECT * FROM t WHERE id = 7");
    CHECK(strstr(out2, "name_7") != NULL, "복제본에서 PK 인덱스 점 조회도 동작(id=7)");
    free(out2);
    db_close(&rep);

    cleanup(PRIM);
    cleanup(REPL);

    if (failures == 0) printf("\n모든 테스트 통과\n");
    else printf("\n%d개 실패\n", failures);
    return failures ? 1 : 0;
}
