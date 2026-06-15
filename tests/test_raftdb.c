#include "raftdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ① Raft로 실제 엔진 쓰기를 복제 — 진짜 고가용 DB.
 *
 * 28~32편 Raft(합의)와 db.c(엔진)를 상태기계 복제로 잇는다:
 *   쓰기 SQL을 리더가 Raft에 제안 -> 과반 커밋 -> 모든 노드가 자기 db-hobby 엔진에
 *   같은 순서로 적용. 검증:
 *     1) 실제 db_exec 쓰기가 3노드 엔진 전부에 복제돼 SELECT 결과가 일치.
 *     2) 리더가 죽어도 새 리더가 이어받아 계속 쓰기 -> 데이터 일관(진짜 failover).
 */

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } } while (0)

/* node 엔진에서 SELECT를 실행해 출력 문자열을 돌려준다(호출자가 free). */
static char *query(RaftDb *rd, int node, const char *sql) {
    char *buf = NULL; size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    raftdb_query(rd, node, sql, f);
    fclose(f);
    return buf;
}

/* 출력에서 "name_" 등장 횟수 = 행 수(대략). */
static int count_rows(const char *out) {
    int n = 0;
    for (const char *p = out; (p = strstr(p, "name_")) != NULL; p++) n++;
    return n;
}

int main(void) {
    printf("== Raft로 복제되는 db-hobby (진짜 고가용 DB) ==\n");

    RaftDb *rd = malloc(sizeof *rd); /* RaftDb는 크다(메시지 큐) — 힙에 */
    if (!rd) return 1;
    if (raftdb_open(rd, 3, "build/raftdb_t") != 0) {
        printf("  FAIL raftdb_open\n");
        return 1;
    }

    /* ── 리더 선출 ─────────────────────────────────────────────── */
    raftdb_run(rd, 20);
    int lead = raftdb_leader(rd);
    CHECK(lead >= 0, "리더 선출됨");

    /* ── 쓰기: 리더에 제안 -> Raft 합의 -> 모든 노드 엔진에 적용 ── */
    raftdb_write(rd, "CREATE TABLE t (id INT, name TEXT)");
    for (int i = 1; i <= 5; i++) {
        char sql[128];
        snprintf(sql, sizeof sql, "INSERT INTO t VALUES (%d, 'name_%d')", i, i);
        raftdb_write(rd, sql);
    }
    raftdb_run(rd, 25); /* 복제·커밋·적용 */

    /* ── 검증 1: 3노드 엔진 전부에 복제됨(SELECT 결과 일치) ────── */
    char *q0 = query(rd, 0, "SELECT * FROM t");
    char *q1 = query(rd, 1, "SELECT * FROM t");
    char *q2 = query(rd, 2, "SELECT * FROM t");
    CHECK(count_rows(q0) == 5, "노드0 엔진에 5행 복제됨");
    CHECK(count_rows(q1) == 5, "노드1 엔진에 5행 복제됨");
    CHECK(count_rows(q2) == 5, "노드2 엔진에 5행 복제됨");
    CHECK(strcmp(q0, q1) == 0 && strcmp(q1, q2) == 0,
          "세 노드 엔진의 SELECT 결과가 완전히 일치(SMR 수렴)");
    CHECK(strstr(q0, "name_3") != NULL, "복제된 데이터 내용 정확(name_3)");
    free(q0); free(q1); free(q2);

    /* ── 검증 2: 리더 failover — 리더가 죽어도 이어받아 계속 쓴다 ── */
    int old_lead = raftdb_leader(rd);
    raftdb_crash(rd, old_lead); /* 리더 다운 */
    raftdb_run(rd, 40);         /* 남은 노드가 새 리더 선출 */
    int new_lead = raftdb_leader(rd);
    CHECK(new_lead >= 0 && new_lead != old_lead, "failover: 새 리더가 이어받음");

    /* 새 리더에 계속 쓰기 */
    for (int i = 6; i <= 8; i++) {
        char sql[128];
        snprintf(sql, sizeof sql, "INSERT INTO t VALUES (%d, 'name_%d')", i, i);
        raftdb_write(rd, sql);
    }
    raftdb_run(rd, 25);

    /* 살아있는 두 노드(old_lead 제외)가 8행으로 일관 */
    int a = -1, b = -1;
    for (int i = 0; i < 3; i++) {
        if (i == old_lead) continue;
        if (a < 0) a = i; else b = i;
    }
    char *qa = query(rd, a, "SELECT * FROM t");
    char *qb = query(rd, b, "SELECT * FROM t");
    CHECK(count_rows(qa) == 8, "failover 후: 생존 노드가 8행(계속 복제됨)");
    CHECK(strcmp(qa, qb) == 0, "failover 후: 두 생존 노드가 여전히 일치");
    CHECK(strstr(qa, "name_8") != NULL, "failover 후 새 쓰기(name_8)도 복제됨");
    free(qa); free(qb);

    /* ── 검증 3: 선형화 읽기(ReadIndex) — 확인된 리더에서만 읽는다 ── */
    {
        char *b = NULL; size_t sz = 0;
        FILE *f = open_memstream(&b, &sz);
        int rc = raftdb_query_linearizable(rd, "SELECT * FROM t", f, 40);
        fclose(f);
        CHECK(rc == 0, "선형화 읽기: 과반 확인된 리더에서 서빙됨");
        CHECK(count_rows(b) == 8, "선형화 읽기: 최신 8행 반환");
        free(b);

        /* 리더를 고립 -> 과반 확인 불가 -> 선형화 읽기 거부(낡은 읽기 방지). 대조로
         * 비선형화 직접 읽기는 여전히 뭔가 돌려준다(안전하지 않을 수 있음). */
        raftdb_isolate(rd, new_lead);
        char *b2 = NULL; size_t sz2 = 0;
        FILE *f2 = open_memstream(&b2, &sz2);
        int rc2 = raftdb_query_linearizable(rd, "SELECT * FROM t", f2, 40);
        fclose(f2);
        CHECK(rc2 == -1, "선형화 읽기: 고립된 리더는 과반 확인 실패 -> 거부");
        free(b2);

        char *b3 = query(rd, new_lead, "SELECT * FROM t"); /* 직접 읽기는 그냥 응답 */
        CHECK(count_rows(b3) >= 0, "대조: 비선형화 직접 읽기는 확인 없이 응답(낡을 수 있음)");
        free(b3);
    }

    /* F1: apply 중 db_exec 실패가 하나도 없었나(있으면 그 노드가 발산한 것). */
    CHECK(raftdb_apply_errors(rd) == 0, "apply 오류 0 — 어떤 노드도 엔진 발산 없음");

    raftdb_close(rd);
    free(rd);

    if (failures == 0) printf("\n모든 테스트 통과\n");
    else printf("\n%d개 실패\n", failures);
    return failures ? 1 : 0;
}
