#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 병렬 스트리밍 풀스캔을 실제 SELECT에 배선한 것(37편)을 검증한다.
 * 큰 테이블(>= PARSCAN_MIN_PAGES)의 비인덱스 full-scan SELECT는 워커들이
 * 가시성+WHERE를 병렬로 평가하고 leader가 페이지 순서로 출력한다 — 결과가
 * 직렬 경로와 동일해야 하고, MVCC(UPDATE 가시성·DELETE·ROLLBACK)도 그대로여야 한다.
 * test_parexec_tsan(ThreadSanitizer 빌드)이 워커 측 row_visible/where_matches/
 * decode_row 호출에 data race가 없음을 확인한다.
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

/* "(N행..." 에서 N을 뽑는다 */
static int rows_of(const char *out) {
    const char *p = strrchr(out, '(');
    return p ? atoi(p + 1) : -1;
}

int main(void) {
    const char *path = "build/test_parexec.db";
    unlink(path);

    Database db;
    db_open(&db, path);
    char *o;

    /* PARSCAN_MIN_PAGES(16)를 넘기려면 넉넉히 — 8바이트*2 컬럼, 6000행이면 ~18페이지 */
    o = run(&db, "CREATE TABLE big (id INT, v INT)"); free(o);
    const int N = 6000;
    for (int i = 1; i <= N; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO big VALUES (%d, %d)", i, i * 2);
        o = run(&db, sql); free(o);
    }

    /* ── 1) 비인덱스 full-scan WHERE (병렬 경로) 정확성 ──────────────── */
    /* v 컬럼은 비PK라 인덱스 없음 → full scan → 큰 테이블이라 병렬 경로 */
    o = run(&db, "SELECT * FROM big WHERE v > 11990"); /* id 5996..6000 = 5행 */
    CHECK(rows_of(o) == 5, "병렬 full-scan: v>11990 -> 5행");
    CHECK(strstr(o, "5996 | 11992") && strstr(o, "6000 | 12000") && !strstr(o, "5995 |"),
          "병렬 full-scan: 정확한 행 + 페이지 순서(5996..6000)");
    /* 병렬 경로는 인덱스를 안 쓴다(full scan) */
    CHECK(!strstr(o, "인덱스 사용"), "병렬 full-scan은 인덱스 미사용(순차 풀스캔)");
    free(o);

    /* WHERE 없는 전체 스캔 = N행 */
    o = run(&db, "SELECT * FROM big");
    CHECK(rows_of(o) == N, "병렬 full-scan: WHERE 없음 -> 전체 6000행");
    free(o);

    /* 복합 WHERE (AND) — 여전히 비인덱스 full scan */
    o = run(&db, "SELECT * FROM big WHERE v > 10 AND v < 30"); /* id 6..14 -> v 12..28 = 9행 */
    CHECK(rows_of(o) == 9, "병렬 full-scan: 복합 WHERE(v>10 AND v<30) -> 9행");
    free(o);

    /* ── 2) MVCC UPDATE 가시성 (병렬 경로도 게이트를 지난다) ─────────── */
    o = run(&db, "UPDATE big SET v = 777777 WHERE id = 100"); free(o);
    o = run(&db, "SELECT * FROM big WHERE v = 777777");
    CHECK(rows_of(o) == 1 && strstr(o, "100 | 777777"),
          "병렬 full-scan: UPDATE된 새 버전만 보임(가시성 게이트)");
    free(o);
    o = run(&db, "SELECT * FROM big WHERE v = 200"); /* id=100의 옛 값 200은 안 보여야 */
    CHECK(rows_of(o) == 0, "병렬 full-scan: 옛 버전(v=200) 안 보임");
    free(o);

    /* ── 3) DELETE 반영 ── */
    o = run(&db, "DELETE FROM big WHERE id = 200"); free(o);
    o = run(&db, "SELECT * FROM big WHERE v = 400"); /* id=200 -> v=400 삭제됨 */
    CHECK(rows_of(o) == 0, "병렬 full-scan: DELETE된 행 안 보임");
    free(o);

    /* ── 4) ROLLBACK: 미커밋 삽입은 병렬 스캔에도 안 보인다 ── */
    o = run(&db, "BEGIN"); free(o);
    o = run(&db, "INSERT INTO big VALUES (99999, 88888)"); free(o);
    o = run(&db, "ROLLBACK"); free(o);
    o = run(&db, "SELECT * FROM big WHERE v = 88888");
    CHECK(rows_of(o) == 0, "병렬 full-scan: ROLLBACK된 삽입 안 보임");
    free(o);

    /* ── 5) 작은 테이블은 직렬 경로(회귀 없음 확인) ── */
    o = run(&db, "CREATE TABLE small (id INT, v INT)"); free(o);
    for (int i = 1; i <= 5; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO small VALUES (%d, %d)", i, i * 2);
        o = run(&db, sql); free(o);
    }
    o = run(&db, "SELECT * FROM small WHERE v > 4"); /* id 3,4,5 = 3행, 직렬 경로 */
    CHECK(rows_of(o) == 3 && strstr(o, "3 | 6") && strstr(o, "5 | 10"),
          "작은 테이블 full-scan(직렬)도 정확 — 회귀 없음");
    free(o);

    db_close(&db);
    unlink(path);

    if (failures == 0) { printf("\n모든 테스트 통과\n"); return 0; }
    printf("\n%d개 실패\n", failures);
    return 1;
}
