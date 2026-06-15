#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * LSM을 실제 엔진의 PK 인덱스 저장 엔진으로 배선한 것(35편)을 검증한다.
 * `CREATE TABLE ... USING lsm`으로 만든 테이블이 기본 B+Tree 테이블과
 *   - 점 조회(=), 범위 조회(>,>=,<,<=)
 *   - MVCC UPDATE 가시성(옛 버전 가림), DELETE, VACUUM, ROLLBACK
 *   - 재오픈(heap에서 인덱스 재구축), 카탈로그 영속(index_kind)
 * 에서 완전히 동일하게 동작하는지, 그리고 LSM 특유의 재구축/롤백 경로가
 * 결과를 훼손하지 않는지를 확인한다.
 */

static int failures = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) printf("  ok   %s\n", msg);                                  \
        else { printf("  FAIL %s\n", msg); failures++; }                       \
    } while (0)

static char *run(Database *db, const char *sql) {
    char *buf = NULL;
    size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    db_exec(db, sql, f);
    fclose(f);
    return buf;
}

/* 출력에서 "(N행" 의 N을 뽑는다(-1이면 못 찾음). */
static int rows_of(const char *out) {
    const char *p = strrchr(out, '(');
    if (!p) return -1;
    return atoi(p + 1);
}

/* 두 테이블(같은 데이터, 다른 저장 엔진)에 같은 SELECT을 던져 결과가 같은지. */
static int same_result(Database *db, const char *sql_a, const char *sql_b) {
    char *a = run(db, sql_a);
    char *b = run(db, sql_b);
    int eq = strcmp(a, b) == 0;
    free(a);
    free(b);
    return eq;
}

int main(void) {
    const char *path = "build/test_lsm_engine.db";
    unlink(path);
    /* 옛 LSM 인덱스 디렉터리도 정리 */
    system("rm -rf build/test_lsm_engine.db.*");

    Database db;
    db_open(&db, path);
    char *o;

    /* ── 1) USING lsm 파싱 + 테이블 생성 ──────────────────────────────── */
    o = run(&db, "CREATE TABLE l (id INT, v INT) USING lsm");
    CHECK(strstr(o, "생성됨"), "CREATE TABLE ... USING lsm 성공");
    free(o);
    /* 대조군: 기본 B+Tree 테이블(같은 스키마/데이터) */
    o = run(&db, "CREATE TABLE b (id INT, v INT)"); free(o);

    /* 같은 행들을 양쪽에 넣는다 */
    for (int i = 1; i <= 20; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO l VALUES (%d, %d)", i, i * 10);
        o = run(&db, sql); free(o);
        snprintf(sql, sizeof(sql), "INSERT INTO b VALUES (%d, %d)", i, i * 10);
        o = run(&db, sql); free(o);
    }

    /* ── 2) 점 조회(=): 인덱스 사용 + B+Tree와 동일 ──────────────────── */
    o = run(&db, "SELECT * FROM l WHERE id = 7");
    CHECK(strstr(o, "7 | 70") && strstr(o, "인덱스 사용"), "LSM 점 조회 id=7 -> 70 (인덱스 사용)");
    free(o);
    CHECK(same_result(&db, "SELECT * FROM l WHERE id = 7", "SELECT * FROM b WHERE id = 7"),
          "점 조회: LSM == B+Tree");

    /* ── 3) 범위 조회: 네 부등호 모두 B+Tree와 동일 ──────────────────── */
    CHECK(same_result(&db, "SELECT * FROM l WHERE id >= 15", "SELECT * FROM b WHERE id >= 15"),
          "범위 >=: LSM == B+Tree");
    CHECK(same_result(&db, "SELECT * FROM l WHERE id > 15", "SELECT * FROM b WHERE id > 15"),
          "범위 >: LSM == B+Tree");
    CHECK(same_result(&db, "SELECT * FROM l WHERE id <= 5", "SELECT * FROM b WHERE id <= 5"),
          "범위 <=: LSM == B+Tree");
    CHECK(same_result(&db, "SELECT * FROM l WHERE id < 5", "SELECT * FROM b WHERE id < 5"),
          "범위 <: LSM == B+Tree");

    /* ── 4) MVCC UPDATE: 새 버전이 옛 버전을 가린다(멀티값 인덱스) ────── */
    o = run(&db, "UPDATE l SET v = 7777 WHERE id = 7"); free(o);
    o = run(&db, "SELECT * FROM l WHERE id = 7");
    CHECK(strstr(o, "7 | 7777") && !strstr(o, "7 | 70") && rows_of(o) == 1,
          "UPDATE 후 점 조회: 새 버전만 보임(옛 70 가림) — 다중버전 LSM 인덱스");
    free(o);

    /* ── 5) DELETE + VACUUM: 삭제 반영, 죽은 인덱스 항목 청소 ─────────── */
    o = run(&db, "DELETE FROM l WHERE id = 3"); free(o);
    o = run(&db, "SELECT * FROM l WHERE id = 3");
    CHECK(rows_of(o) == 0, "DELETE 후 id=3 조회 0행");
    free(o);
    o = run(&db, "VACUUM l");
    CHECK(strstr(o, "VACUUM") || strstr(o, "청소") || strstr(o, "회수") || strstr(o, "완료"),
          "VACUUM l 실행(죽은 버전·인덱스 항목 청소)");
    free(o);
    o = run(&db, "SELECT * FROM l WHERE id = 3");
    CHECK(rows_of(o) == 0, "VACUUM 후에도 id=3 없음");
    free(o);
    o = run(&db, "SELECT * FROM l WHERE id = 7");
    CHECK(strstr(o, "7 | 7777") && rows_of(o) == 1, "VACUUM 후 살아있는 최신 버전 유지");
    free(o);

    /* ── 6) ROLLBACK: LSM 인덱스는 heap에서 재구축돼 dangling이 안 남는다 ── */
    o = run(&db, "BEGIN"); free(o);
    o = run(&db, "INSERT INTO l VALUES (99, 990)"); free(o);
    o = run(&db, "SELECT * FROM l WHERE id = 99");
    CHECK(rows_of(o) == 1, "롤백 전 미커밋 삽입은 보임");
    free(o);
    o = run(&db, "ROLLBACK"); free(o);
    o = run(&db, "SELECT * FROM l WHERE id = 99");
    CHECK(rows_of(o) == 0, "ROLLBACK 후 id=99 사라짐(인덱스 재구축으로 dangling 없음)");
    free(o);
    /* 롤백이 정상 데이터를 훼손하지 않았다: 삭제(3)·갱신(7)은 그대로, 나머지는 온전 */
    o = run(&db, "SELECT * FROM l WHERE id = 12");
    CHECK(strstr(o, "12 | 120") && rows_of(o) == 1, "ROLLBACK 후 무관한 행(id=12) 온전");
    free(o);
    o = run(&db, "SELECT * FROM l");
    CHECK(rows_of(o) == 19 && !strstr(o, "99 |"), /* 20행 - 삭제된 3 = 19, 롤백된 99 없음 */
          "ROLLBACK 후 전체 19행(3 삭제 반영, 99 롤백)");
    free(o);

    /* ── 7) 재오픈: index_kind 카탈로그 영속 + heap에서 재구축 ────────── */
    o = run(&db, "SELECT * FROM l WHERE id = 12");
    char *before = o; /* 닫기 전 결과 보관 */
    db_close(&db);
    db_open(&db, path);
    o = run(&db, "SELECT * FROM l WHERE id = 12");
    CHECK(strcmp(before, o) == 0 && strstr(o, "인덱스 사용"),
          "재오픈 후 LSM 인덱스 여전히 동작(카탈로그 index_kind 영속 + 재구축)");
    free(before);
    free(o);
    /* 재오픈 후에도 여전히 삭제·갱신이 반영돼 있다(heap이 진실의 원천) */
    o = run(&db, "SELECT * FROM l WHERE id = 3");
    CHECK(rows_of(o) == 0, "재오픈 후 id=3 여전히 없음");
    free(o);
    o = run(&db, "SELECT * FROM l WHERE id = 7");
    CHECK(strstr(o, "7 | 7777"), "재오픈 후 id=7 최신값 7777 유지");
    free(o);

    /* ── 8) 대량 삽입으로 SSTable flush 유발(임계치 넘김) 후에도 정확 ─── */
    o = run(&db, "CREATE TABLE big (id INT, v INT) USING lsm"); free(o);
    for (int i = 0; i < 600; i++) { /* LSM_PK_THRESHOLD(256) 초과 -> flush 여러 번 */
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO big VALUES (%d, %d)", i, i);
        o = run(&db, sql); free(o);
    }
    o = run(&db, "SELECT * FROM big WHERE id = 500");
    CHECK(strstr(o, "500 | 500") && rows_of(o) == 1,
          "SSTable flush 여러 번 후에도 점 조회 정확(id=500)");
    free(o);
    o = run(&db, "SELECT * FROM big WHERE id >= 597");
    CHECK(rows_of(o) == 3, "flush 후 범위 조회 정확(id>=597 -> 3행)");
    free(o);

    db_close(&db);
    unlink(path);
    system("rm -rf build/test_lsm_engine.db.*");

    if (failures == 0) { printf("\n모든 테스트 통과\n"); return 0; }
    printf("\n%d개 실패\n", failures);
    return 1;
}
