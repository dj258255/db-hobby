#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 트랙 F — 비용 기반 옵티마이저 (ANALYZE + 선택도 + 비용).
 * 규칙 기반 플래너는 "PK 조건이면 무조건 인덱스"였다. 넓은 범위엔 인덱스 범위 스캔이
 * 행마다 랜덤 힙 페치를 하느라 순차 스캔보다 비싸다. ANALYZE로 통계를 재고,
 * 선택도로 매칭 행 수를 추정해 비용으로 고른다.
 *   - 점 조회(=) / 좁은 범위 -> 인덱스
 *   - 넓은 범위(대부분 매칭) -> 순차 (비용 역전)
 * 그리고 어느 경로를 타든 결과는 같아야 한다(정확성 불변).
 */

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } } while (0)

static char *run(Database *db, const char *sql) {
    char *buf = NULL; size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    db_exec(db, sql, f);
    fclose(f);
    return buf;
}
static void cleanup(const char *base) {
    char p[700]; const char *ext[] = {"t.tbl","t.idx","t.wal","t.idx.wal"};
    unlink(base);
    for (size_t i=0;i<sizeof(ext)/sizeof(ext[0]);i++){snprintf(p,sizeof(p),"%s.%s",base,ext[i]);unlink(p);}
}
static int count_of(Database *db, const char *sql) {
    char *o = run(db, sql); int n = -1;
    for (const char *p = strstr(o, "("); p && *p; p++) if (*p >= '0' && *p <= '9') { n = atoi(p); break; }
    free(o); return n;
}

int main(void) {
    const char *path = "build/test_optimizer.db";
    char sql[256];
    cleanup(path);

    Database db; db_open(&db, path);
    char *o;
    o = run(&db, "CREATE TABLE t (id INT, v TEXT)"); free(o);
    for (int i = 1; i <= 1000; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, 'r%d')", i, i);
        o = run(&db, sql); free(o);
    }

    /* --- ANALYZE 전: 통계 없음 -> 규칙 기반(범위도 인덱스) --- */
    o = run(&db, "EXPLAIN SELECT * FROM t WHERE id > 100");
    CHECK(strstr(o, "Index Range Scan") && !strstr(o, "cost="),
          "ANALYZE 전: 넓은 범위도 규칙대로 인덱스 (비용 표기 없음)");
    free(o);

    /* --- ANALYZE --- */
    o = run(&db, "ANALYZE t");
    CHECK(strstr(o, "행 1000") && strstr(o, "[1, 1000]"), "ANALYZE: 행 1000 · PK 범위 [1,1000]");
    free(o);

    /* --- ANALYZE 후: 비용으로 갈린다 --- */
    o = run(&db, "EXPLAIN SELECT * FROM t WHERE id = 5");
    CHECK(strstr(o, "Index Point Lookup") && strstr(o, "rows=1"), "= -> 점 조회 (rows=1)");
    free(o);

    o = run(&db, "EXPLAIN SELECT * FROM t WHERE id > 999");
    CHECK(strstr(o, "Index Range Scan") && strstr(o, "rows=1"),
          "아주 좁은 범위(id>999, 1행) -> 인덱스 범위 스캔");
    free(o);

    o = run(&db, "EXPLAIN SELECT * FROM t WHERE id > 100");
    CHECK(strstr(o, "Seq Scan") && strstr(o, "비용 기반"),
          "넓은 범위(id>100, 901행) -> 순차 (비용 역전으로 인덱스 안 씀)");
    free(o);

    o = run(&db, "EXPLAIN SELECT * FROM t WHERE id < 500");
    CHECK(strstr(o, "Seq Scan") && strstr(o, "비용 기반"), "절반 범위(id<500) -> 순차");
    free(o);

    /* --- 정확성: 어느 경로를 타든 결과 행 수는 같다 --- */
    CHECK(count_of(&db, "SELECT * FROM t WHERE id > 999") == 1, "id>999 결과 1행 (인덱스 경로)");
    CHECK(count_of(&db, "SELECT * FROM t WHERE id > 100") == 900, "id>100 결과 900행 (순차 경로, 정확)");
    CHECK(count_of(&db, "SELECT * FROM t WHERE id = 5") == 1, "id=5 결과 1행");
    CHECK(count_of(&db, "SELECT COUNT(*) FROM t") == 1000, "전체 1000행");

    /* --- 통계는 재오픈 후에도 유지 (카탈로그 영속화) --- */
    db_close(&db);
    db_open(&db, path);
    o = run(&db, "EXPLAIN SELECT * FROM t WHERE id > 100");
    CHECK(strstr(o, "Seq Scan") && strstr(o, "비용 기반"), "재오픈 후에도 통계 유지 (비용 기반 유지)");
    free(o);

    /* --- 데이터가 바뀌면 재ANALYZE로 갱신 --- */
    for (int i = 1001; i <= 1000; i++) {} /* no-op */
    o = run(&db, "DELETE FROM t WHERE id > 10"); free(o);   /* 대부분 삭제 */
    o = run(&db, "ANALYZE t");
    CHECK(strstr(o, "행 10"), "DELETE 후 재ANALYZE -> 행 10으로 갱신 (죽은 버전 제외)");
    free(o);

    db_close(&db);
    cleanup(path);
    if (failures == 0) { printf("\n전체 통과\n"); return 0; }
    printf("\n%d개 실패\n", failures);
    return 1;
}
