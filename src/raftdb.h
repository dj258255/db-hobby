/* raftdb — Raft로 복제되는 db-hobby (트랙 H2 통합: 진짜 고가용 DB)
 *
 * 28~32편의 Raft는 시뮬레이션 네트워크에서만 도는 독립 모듈이었고, 31편의 복제는
 * primary가 고정이었다. 이 계층이 둘을 잇는다 — 실제 db.c 엔진의 쓰기를 Raft
 * 합의로 복제한다. 정석인 상태기계 복제(SMR):
 *   1) 클라이언트 쓰기(SQL)는 리더가 '바로 실행하지 않고' Raft 로그에 제안한다.
 *   2) Raft가 과반에 복제·커밋한다.
 *   3) 커밋된 엔트리를 각 노드가 순서대로 자기 db-hobby 엔진에 db_exec로 적용한다.
 * 모든 노드가 같은 명령을 같은 순서로 적용하므로 엔진들이 수렴한다. 리더가 죽으면
 * 남은 노드가 새 리더를 뽑아 이어받는다 — 진짜 failover.
 *
 * ── 정직한 단순화 ───────────────────────────────────────────────────
 * 인프로세스 시뮬레이션 클러스터다. Raft 로그 엔트리는 명령의 seq(정수)만 나르고,
 * SQL 바이트는 공유 배열(cmds[])에 둔다 — 노드들이 한 프로세스라 payload가 물리적
 * 으로 이동할 필요가 없다. **합의의 본질(전역 순서·과반 커밋·리더 선출·failover)은
 * 전부 진짜 Raft(raft.c)가 하고**, 오직 명령 바이트 전송만 생략했다(진짜 클러스터
 * 라면 seq 대신 SQL 바이트가 엔트리에 실려 26편 소켓으로 흐른다). 이 덕에 raft.c는
 * 한 줄도 안 고치고 기존 apply 콜백을 그대로 쓴다.
 *
 * 정직한 경계 (프론티어):
 *   - 읽기는 아무 노드 엔진에서 직접 — 낡을 수 있다(선형화 읽기 = 다음 항목 ④).
 *   - 크래시 노드의 재합류(rejoin)는 안 한다: 엔진이 '적용한 Raft 인덱스'를 durable
 *     하게 추적하지 않아, 재시작 시 커밋 로그를 재적용하면 INSERT가 중복된다(레지스터
 *     SM과 달리 SQL 적용은 idempotent가 아니다). 엔진에 applied-index 영속화를 넣는
 *     게 재합류의 전제 — 프론티어. 그래서 여기선 failover(리더 죽고 이어받기)까지.
 *   - 단일 프로세스 시뮬 네트워크(합의 검증용, 28편과 같은 이유). 소켓 배선은 26편 위.
 *   - apply 실패 처리(적대적 리뷰 F1 반영): SMR의 전제는 "apply는 결정적"인데
 *     db_exec는 파일 I/O 부작용이 있어 노드-로컬로 실패할 수 있다(디스크 풀 등).
 *     그런 실패를 삼키면 last_applied만 전진하고 엔진은 안 바뀌어 그 노드가 영구
 *     발산한다. 그래서 apply 실패를 세어(apply_errors) '드러낸다'. 진짜 시스템은
 *     apply 실패를 치명적 조건으로 보고 그 노드를 멈추거나 재동기화한다 — 여기선
 *     검출까지(재동기화는 rejoin 프론티어와 한 몸).
 *   - 스냅샷(§7)은 안 켠다: raftdb는 raft_snapshot을 호출하지 않아 로그가 자란다.
 *     엔진 상태기계의 스냅샷/설치(snap_install이 DB 스냅샷을 복원)는 별도 통합 —
 *     프론티어. (그래서 raft_init에 snap_install을 안 넘긴다.)
 *   - write는 '제안'이지 '커밋 ack'가 아니다: raftdb_write는 raft_submit 후 바로
 *     0을 반환한다. 분단으로 고립된 stale 리더에 제안되면 그 엔트리는 커밋 못 되고
 *     사라질 수 있다(호출자는 0을 받았어도). 커밋 확인(선형화 쓰기)은 프론티어.
 */
#ifndef MINIDB_RAFTDB_H
#define MINIDB_RAFTDB_H

#include "db.h"
#include "raft.h"

#include <stdio.h>

#define RAFTDB_MAX_NODES 7
#define RAFTDB_MAX_CMDS 512
#define RAFTDB_CMD_LEN 256
#define RAFTDB_QCAP 4096 /* 큰 RaftMsg × 이 수 = 큐 메모리. RaftDb는 힙에 할당 권장. */

typedef struct {
    int n;
    Raft raft[RAFTDB_MAX_NODES];      /* 노드별 Raft 인스턴스 */
    Database db[RAFTDB_MAX_NODES];     /* 노드별 진짜 db-hobby 엔진(각자 파일) */
    int alive[RAFTDB_MAX_NODES];       /* 0이면 다운(크래시) */
    int connected[RAFTDB_MAX_NODES][RAFTDB_MAX_NODES]; /* 분단 행렬 */

    char cmds[RAFTDB_MAX_CMDS][RAFTDB_CMD_LEN]; /* 복제되는 명령 payload(인프로세스 공유) */
    int n_cmds;

    RaftMsg q[RAFTDB_QCAP]; /* 시뮬 네트워크 메시지 큐 */
    int qhead, qtail;

    FILE *devnull;     /* 적용 시 db_exec 출력 버림 */
    int apply_errors;  /* apply 중 db_exec가 실패한 횟수(발산 검출용, F1) */
    char prefix[200];
} RaftDb;

/* n노드 클러스터를 연다. 각 노드마다 <prefix>.nodeI.db 엔진을 새로 만든다. 0/-1. */
int raftdb_open(RaftDb *rd, int n, const char *path_prefix);
void raftdb_close(RaftDb *rd);

/* 논리 시계 한 스텝: 살아있는 노드 tick + 메시지 라우팅. 커밋되면 apply 콜백이
 * 각 노드 엔진에 명령을 db_exec로 적용한다. */
void raftdb_step(RaftDb *rd);
void raftdb_run(RaftDb *rd, int steps);

/* 현재 리더(살아있는 노드 중 최신 term의 리더). 없으면 -1. */
int raftdb_leader(const RaftDb *rd);

/* 쓰기 SQL을 리더에 제안한다(바로 실행 X — 커밋 후 모든 노드가 적용). 리더 없으면 -1. */
int raftdb_write(RaftDb *rd, const char *sql);

/* 읽기 SQL을 특정 노드 엔진에서 직접 실행(낡을 수 있음 — 비선형화). */
void raftdb_query(RaftDb *rd, int node, const char *sql, FILE *out);

/* 선형화 읽기(ReadIndex): 리더가 '지금도 리더'임을 과반으로 확인한 뒤에야 읽기를
 * 서빙한다. 확인되면 리더 엔진에서 실행하고 0, 확인 안 되면(고립된 옛 리더 등)
 * max_steps 안에 확인 실패로 -1을 반환 — 낡은 읽기를 서빙하지 않는다. */
int raftdb_query_linearizable(RaftDb *rd, const char *sql, FILE *out, int max_steps);

/* 노드를 다른 모든 노드로부터 분단(고립). 선형화 읽기 테스트용. */
void raftdb_isolate(RaftDb *rd, int node);

/* 노드를 크래시(다운). */
void raftdb_crash(RaftDb *rd, int node);

/* apply 중 db_exec가 실패한 누적 횟수. 0이 아니면 어떤 노드가 발산했다는 신호(F1). */
int raftdb_apply_errors(const RaftDb *rd);

#endif /* MINIDB_RAFTDB_H */
