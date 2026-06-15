/* raftdb.c — Raft로 복제되는 db-hobby. 설계·경계는 raftdb.h 참고.
 * raft.c(합의)와 db.c(엔진)를 잇는 상태기계 복제 계층. */
#include "raftdb.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* 커밋된 명령을 이 노드의 엔진에 적용(§SMR): command는 cmds[]의 seq. 모든 노드가
 * 같은 순서로 이 콜백을 밟으므로 엔진들이 수렴한다. 리더도 여기서(커밋 후) 적용한다
 * — 클라이언트 write는 제안만 하지 실행하지 않는다. */
static void raftdb_apply(int node_id, int64_t index, int64_t command, void *ctx) {
    RaftDb *rd = ctx;
    (void)index;
    if (command >= 0 && command < rd->n_cmds) {
        /* F1: db_exec 실패를 삼키지 않는다. 실패하면 이 노드의 엔진은 안 바뀌었는데
         * last_applied만 전진해 영구 발산한다 — 그걸 세어 드러낸다. */
        if (db_exec(&rd->db[node_id], rd->cmds[command], rd->devnull) != 0) {
            rd->apply_errors++;
        }
    }
}

int raftdb_apply_errors(const RaftDb *rd) {
    return rd->apply_errors;
}

/* ── 시뮬 네트워크 큐 ── */
static void qpush(RaftDb *rd, const RaftMsg *m) {
    int next = (rd->qtail + 1) % RAFTDB_QCAP;
    if (next == rd->qhead) return; /* 가득 참(넉넉한 상한) */
    rd->q[rd->qtail] = *m;
    rd->qtail = next;
}
static int qpop(RaftDb *rd, RaftMsg *m) {
    if (rd->qhead == rd->qtail) return 0;
    *m = rd->q[rd->qhead];
    rd->qhead = (rd->qhead + 1) % RAFTDB_QCAP;
    return 1;
}

static void deliver(RaftDb *rd, RaftMsg *m) {
    if (!rd->alive[m->from] || !rd->alive[m->to]) return;
    if (!rd->connected[m->from][m->to]) return;
    RaftMsg reply;
    int has_reply = 0;
    RaftOutbox out;
    out.n = 0;
    raft_recv(&rd->raft[m->to], m, &reply, &has_reply, &out); /* 커밋되면 여기서 apply */
    if (has_reply) qpush(rd, &reply);
    for (int i = 0; i < out.n; i++) qpush(rd, &out.m[i]);
}

void raftdb_step(RaftDb *rd) {
    for (int i = 0; i < rd->n; i++) {
        if (!rd->alive[i]) continue;
        RaftOutbox out;
        out.n = 0;
        raft_tick(&rd->raft[i], &out);
        for (int k = 0; k < out.n; k++) qpush(rd, &out.m[k]);
    }
    RaftMsg m;
    int guard = 0;
    while (qpop(rd, &m) && guard++ < RAFTDB_QCAP) {
        deliver(rd, &m);
    }
}

void raftdb_run(RaftDb *rd, int steps) {
    for (int s = 0; s < steps; s++) raftdb_step(rd);
}

int raftdb_leader(const RaftDb *rd) {
    /* L1 완화: 어떤 살아있는 노드가 더 높은 term에 있으면, 그보다 낮은 term의
     * 리더는 '고립된 stale 리더'다 — 거기 제안하면 커밋 못 되고 유실된다. 그래서
     * 살아있는 노드의 최대 term과 같은 term의 리더만 유효로 본다(분단 창에서
     * stale 리더로의 제안을 줄인다. 완전 제거는 커밋 확인 = 프론티어). */
    uint64_t max_term = 0;
    for (int i = 0; i < rd->n; i++) {
        if (rd->alive[i] && rd->raft[i].current_term > max_term) {
            max_term = rd->raft[i].current_term;
        }
    }
    for (int i = 0; i < rd->n; i++) {
        if (rd->alive[i] && rd->raft[i].role == RAFT_LEADER &&
            rd->raft[i].current_term == max_term) {
            return i;
        }
    }
    return -1;
}

int raftdb_write(RaftDb *rd, const char *sql) {
    int L = raftdb_leader(rd);
    if (L < 0) return -1;
    if (rd->n_cmds >= RAFTDB_MAX_CMDS) return -1;
    int seq = rd->n_cmds++;
    strncpy(rd->cmds[seq], sql, RAFTDB_CMD_LEN - 1);
    rd->cmds[seq][RAFTDB_CMD_LEN - 1] = '\0';
    raft_submit(&rd->raft[L], (int64_t)seq); /* 제안만 — 커밋 후 모든 노드가 적용 */
    return 0;
}

void raftdb_query(RaftDb *rd, int node, const char *sql, FILE *out) {
    db_exec(&rd->db[node], sql, out); /* 낡을 수 있음(비선형화) */
}

void raftdb_isolate(RaftDb *rd, int node) {
    for (int j = 0; j < rd->n; j++) {
        if (j != node) {
            rd->connected[node][j] = 0;
            rd->connected[j][node] = 0;
        }
    }
}

int raftdb_query_linearizable(RaftDb *rd, const char *sql, FILE *out, int max_steps) {
    int L = raftdb_leader(rd);
    if (L < 0) {
        return -1;
    }
    RaftOutbox ob;
    ob.n = 0;
    int64_t ri = raft_read_index(&rd->raft[L], &ob); /* read index 잡고 하트비트 발사 */
    if (ri < 0) {
        return -1;
    }
    for (int k = 0; k < ob.n; k++) qpush(rd, &ob.m[k]); /* barrier 하트비트를 큐에 */

    for (int s = 0; s < max_steps; s++) {
        raftdb_step(rd);
        /* 리더십이 바뀌었으면(옛 리더가 강등) 이 읽기는 무효 -> 거부 */
        if (rd->raft[L].role != RAFT_LEADER) {
            return -1;
        }
        if (raft_read_confirmed(&rd->raft[L]) >= ri && rd->raft[L].last_applied >= ri) {
            db_exec(&rd->db[L], sql, out); /* 과반 확인됨 + 적용 완료 -> 선형화 읽기 */
            return 0;
        }
    }
    return -1; /* 과반 확인 실패(고립된 리더 등) -> 낡은 읽기를 서빙하지 않는다 */
}

void raftdb_crash(RaftDb *rd, int node) {
    rd->alive[node] = 0; /* 다운 — tick도 메시지 배달도 안 됨 */
}

/* 노드 i의 파일들을 지운다. L3: 데모가 테이블 "t"만 쓴다는 가정으로 접미사를
 * 하드코딩한다(다른 테이블을 만드는 시나리오면 그 파일 접미사도 추가해야 함). */
static void cleanup_node(const char *prefix, int i) {
    char p[300];
    const char *suf[] = {"", ".t.tbl", ".t.wal", ".t.idx", ".t.idx.wal"};
    for (size_t k = 0; k < sizeof(suf) / sizeof(suf[0]); k++) {
        snprintf(p, sizeof p, "%s.node%d.db%s", prefix, i, suf[k]);
        unlink(p);
    }
}

int raftdb_open(RaftDb *rd, int n, const char *path_prefix) {
    if (n < 1 || n > RAFTDB_MAX_NODES) return -1;
    memset(rd, 0, sizeof *rd);
    rd->n = n;
    strncpy(rd->prefix, path_prefix, sizeof(rd->prefix) - 1);
    rd->devnull = fopen("/dev/null", "w");
    if (!rd->devnull) return -1;
    for (int i = 0; i < n; i++) {
        cleanup_node(path_prefix, i);
        char p[256];
        snprintf(p, sizeof p, "%s.node%d.db", path_prefix, i);
        if (db_open(&rd->db[i], p) != 0) {
            /* L3: 오류 경로에서 이미 연 자원을 정리(누수 방지). */
            for (int j = 0; j < i; j++) {
                db_close(&rd->db[j]);
                raft_free(&rd->raft[j]);
            }
            fclose(rd->devnull);
            rd->devnull = NULL;
            return -1;
        }
        /* 서로 다른 선거 타임아웃으로 '누가 먼저'를 결정적으로. 하트비트 2. */
        raft_init(&rd->raft[i], i, n, 6 + 4 * i, 2, raftdb_apply, rd);
        rd->alive[i] = 1;
        for (int j = 0; j < n; j++) rd->connected[i][j] = 1;
    }
    return 0;
}

void raftdb_close(RaftDb *rd) {
    for (int i = 0; i < rd->n; i++) {
        db_close(&rd->db[i]);
        raft_free(&rd->raft[i]);
        cleanup_node(rd->prefix, i);
    }
    if (rd->devnull) {
        fclose(rd->devnull);
        rd->devnull = NULL;
    }
}
