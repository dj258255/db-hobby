/* raft.c — 합의 핵심. 설계·경계는 raft.h 참고.
 * Raft 논문(Ongaro & Ousterhout 2014) §5의 규칙을 학습용으로 옮긴다. */
#include "raft.h"

#include <stdlib.h>
#include <string.h>

/* ── 로그 헬퍼 (log[0]은 term 0 sentinel, 실제 엔트리는 1-indexed) ── */

int64_t raft_last_log_index(const Raft *r) {
    return r->log_len - 1;
}
uint64_t raft_last_log_term(const Raft *r) {
    return r->log[r->log_len - 1].term;
}

static int log_append(Raft *r, RaftEntry e) {
    if (r->log_len >= r->log_cap) {
        int64_t cap = r->log_cap * 2;
        RaftEntry *p = realloc(r->log, (size_t)cap * sizeof(RaftEntry));
        if (!p) {
            return -1;
        }
        r->log = p;
        r->log_cap = cap;
    }
    r->log[r->log_len++] = e;
    return 0;
}

static int majority(const Raft *r) {
    return r->n_nodes / 2 + 1;
}

/* 후보의 로그가 나(투표자)만큼 최신인가 (§5.4.1 선거 제약). */
static int candidate_log_up_to_date(const Raft *r, int64_t cand_idx, uint64_t cand_term) {
    uint64_t my_term = raft_last_log_term(r);
    int64_t my_idx = raft_last_log_index(r);
    if (cand_term != my_term) {
        return cand_term > my_term;
    }
    return cand_idx >= my_idx;
}

/* ── 상태 전이 ── */

static void become_follower(Raft *r, uint64_t term) {
    r->role = RAFT_FOLLOWER;
    r->current_term = term;
    r->voted_for = -1;
    r->election_elapsed = 0;
}

/* 어떤 메시지든 자기보다 높은 term을 보면 즉시 팔로워로 물러난다(§5.1). */
static void step_down_if_higher(Raft *r, uint64_t term) {
    if (term > r->current_term) {
        become_follower(r, term);
    }
}

static void send_append_entries(Raft *r, int peer, RaftOutbox *out) {
    int64_t prev = r->next_index[peer] - 1;
    RaftMsg m;
    memset(&m, 0, sizeof(m));
    m.type = MSG_APPEND_ENTRIES;
    m.from = r->id;
    m.to = peer;
    m.term = r->current_term;
    m.prev_log_index = prev;
    m.prev_log_term = r->log[prev].term;
    m.leader_commit = r->commit_index;
    int n = 0;
    for (int64_t idx = r->next_index[peer]; idx <= raft_last_log_index(r) && n < RAFT_MAX_BATCH;
         idx++) {
        m.entries[n++] = r->log[idx];
    }
    m.n_entries = n;
    if (out->n < (int)(sizeof(out->m) / sizeof(out->m[0]))) {
        out->m[out->n++] = m;
    }
}

static void become_leader(Raft *r, RaftOutbox *out) {
    r->role = RAFT_LEADER;
    for (int i = 0; i < r->n_nodes; i++) {
        r->next_index[i] = raft_last_log_index(r) + 1;
        r->match_index[i] = 0;
    }
    r->match_index[r->id] = raft_last_log_index(r);
    r->heartbeat_elapsed = 0;
    /* 즉시 하트비트(빈 AppendEntries)로 권위를 알린다 — 다른 후보의 선거를 막는다. */
    for (int i = 0; i < r->n_nodes; i++) {
        if (i != r->id) {
            send_append_entries(r, i, out);
        }
    }
}

static void become_candidate(Raft *r, RaftOutbox *out) {
    r->current_term++;
    r->role = RAFT_CANDIDATE;
    r->voted_for = r->id;
    r->votes_granted = 1; /* 자기 자신에게 한 표 */
    r->election_elapsed = 0;
    if (r->votes_granted >= majority(r)) { /* 단일 노드 클러스터면 즉시 당선 */
        become_leader(r, out);
        return;
    }
    for (int i = 0; i < r->n_nodes; i++) {
        if (i == r->id) {
            continue;
        }
        RaftMsg m;
        memset(&m, 0, sizeof(m));
        m.type = MSG_REQUEST_VOTE;
        m.from = r->id;
        m.to = i;
        m.term = r->current_term;
        m.last_log_index = raft_last_log_index(r);
        m.last_log_term = raft_last_log_term(r);
        if (out->n < (int)(sizeof(out->m) / sizeof(out->m[0]))) {
            out->m[out->n++] = m;
        }
    }
}

/* commit_index까지 상태기계에 적용(§5.3). 재시작 후 재적용도 인덱스 기준이라 안전. */
static void apply_committed(Raft *r) {
    while (r->last_applied < r->commit_index) {
        r->last_applied++;
        if (r->apply) {
            r->apply(r->id, r->last_applied, r->log[r->last_applied].command, r->apply_ctx);
        }
    }
}

/* ── 초기화 ── */

int raft_init(Raft *r, int id, int n_nodes, int election_timeout, int heartbeat_timeout,
              RaftApplyFn apply, void *apply_ctx) {
    if (n_nodes < 1 || n_nodes > RAFT_MAX_NODES) {
        return -1;
    }
    memset(r, 0, sizeof(*r));
    r->id = id;
    r->n_nodes = n_nodes;
    r->current_term = 0;
    r->voted_for = -1;
    r->log_cap = 16;
    r->log = malloc((size_t)r->log_cap * sizeof(RaftEntry));
    if (!r->log) {
        return -1;
    }
    r->log[0].term = 0; /* sentinel */
    r->log[0].command = 0;
    r->log_len = 1;
    r->role = RAFT_FOLLOWER;
    r->commit_index = 0;
    r->last_applied = 0;
    r->election_timeout = election_timeout;
    r->heartbeat_timeout = heartbeat_timeout;
    r->apply = apply;
    r->apply_ctx = apply_ctx;
    return 0;
}

void raft_free(Raft *r) {
    free(r->log);
    r->log = NULL;
}

/* ── 논리 시계 ── */

void raft_tick(Raft *r, RaftOutbox *out) {
    if (r->role == RAFT_LEADER) {
        r->heartbeat_elapsed++;
        if (r->heartbeat_elapsed >= r->heartbeat_timeout) {
            r->heartbeat_elapsed = 0;
            for (int i = 0; i < r->n_nodes; i++) {
                if (i != r->id) {
                    send_append_entries(r, i, out);
                }
            }
        }
    } else {
        r->election_elapsed++;
        if (r->election_elapsed >= r->election_timeout) {
            become_candidate(r, out); /* 리더가 안 보인다 -> 선거 시작 */
        }
    }
}

int64_t raft_submit(Raft *r, int64_t command) {
    if (r->role != RAFT_LEADER) {
        return -1;
    }
    RaftEntry e = {.term = r->current_term, .command = command};
    if (log_append(r, e) != 0) {
        return -1;
    }
    r->match_index[r->id] = raft_last_log_index(r);
    return raft_last_log_index(r);
}

/* ── 수신 핸들러 ── */

static void handle_request_vote(Raft *r, const RaftMsg *msg, RaftMsg *reply) {
    step_down_if_higher(r, msg->term);
    memset(reply, 0, sizeof(*reply));
    reply->type = MSG_REQUEST_VOTE_REPLY;
    reply->from = r->id;
    reply->to = msg->from;
    reply->vote_granted = 0;

    if (msg->term < r->current_term) {
        reply->term = r->current_term; /* 낡은 term의 후보 -> 거절 */
        return;
    }
    int can_vote = (r->voted_for == -1 || r->voted_for == msg->from);
    if (can_vote && candidate_log_up_to_date(r, msg->last_log_index, msg->last_log_term)) {
        r->voted_for = msg->from;
        r->election_elapsed = 0; /* 표를 줬으니 선거 타이머 리셋 */
        reply->vote_granted = 1;
    }
    reply->term = r->current_term;
}

static void handle_request_vote_reply(Raft *r, const RaftMsg *msg, RaftOutbox *out) {
    if (msg->term > r->current_term) {
        become_follower(r, msg->term);
        return;
    }
    if (r->role != RAFT_CANDIDATE || msg->term != r->current_term) {
        return; /* 이미 리더가 됐거나 term이 지났다 -> 무시 */
    }
    if (msg->vote_granted) {
        r->votes_granted++;
        if (r->votes_granted >= majority(r)) {
            become_leader(r, out);
        }
    }
}

static void handle_append_entries(Raft *r, const RaftMsg *msg, RaftMsg *reply) {
    step_down_if_higher(r, msg->term);
    memset(reply, 0, sizeof(*reply));
    reply->type = MSG_APPEND_ENTRIES_REPLY;
    reply->from = r->id;
    reply->to = msg->from;
    reply->success = 0;
    reply->match_index = 0;

    if (msg->term < r->current_term) {
        reply->term = r->current_term; /* 낡은 리더 -> 거절(리더가 물러나게) */
        return;
    }
    /* 이 term의 정당한 리더가 있다 -> 후보였어도 팔로워로. 선거 타이머 리셋. */
    r->role = RAFT_FOLLOWER;
    r->election_elapsed = 0;
    reply->term = r->current_term;

    /* 로그 일치성 검사(§5.3): prev 위치의 엔트리가 term까지 일치해야 이어붙인다. */
    if (msg->prev_log_index > raft_last_log_index(r)) {
        return; /* 내 로그가 짧다 -> 실패(리더가 next_index를 낮춰 재시도) */
    }
    if (r->log[msg->prev_log_index].term != msg->prev_log_term) {
        return; /* prev에서 term 불일치 -> 실패 */
    }

    /* 엔트리 이어붙이기: 충돌(term 다름)이 나는 첫 지점부터 꼬리를 덮어쓴다. */
    for (int i = 0; i < msg->n_entries; i++) {
        int64_t idx = msg->prev_log_index + 1 + i;
        if (idx <= raft_last_log_index(r)) {
            if (r->log[idx].term != msg->entries[i].term) {
                r->log_len = idx; /* 충돌 꼬리 절단 */
                log_append(r, msg->entries[i]);
            }
            /* 같으면 이미 가진 엔트리 -> 유지(커밋된 걸 지우지 않기 위함) */
        } else {
            log_append(r, msg->entries[i]);
        }
    }
    reply->success = 1;
    reply->match_index = msg->prev_log_index + msg->n_entries;

    /* 리더의 commit을 따라간다(내가 실제로 가진 데까지). */
    if (msg->leader_commit > r->commit_index) {
        int64_t last = raft_last_log_index(r);
        r->commit_index = (msg->leader_commit < last) ? msg->leader_commit : last;
        apply_committed(r);
    }
}

/* 리더가 과반 복제를 확인해 commit_index를 올린다(§5.3, §5.4.2 현재-term 규칙). */
static void leader_advance_commit(Raft *r) {
    for (int64_t N = raft_last_log_index(r); N > r->commit_index; N--) {
        /* §5.4.2: 리더는 '현재 term'의 엔트리가 과반에 닿을 때만 commit한다. */
        if (r->log[N].term != r->current_term) {
            continue;
        }
        int count = 1; /* 자기 자신 */
        for (int i = 0; i < r->n_nodes; i++) {
            if (i != r->id && r->match_index[i] >= N) {
                count++;
            }
        }
        if (count >= majority(r)) {
            r->commit_index = N;
            apply_committed(r);
            break;
        }
    }
}

static void handle_append_entries_reply(Raft *r, const RaftMsg *msg) {
    if (msg->term > r->current_term) {
        become_follower(r, msg->term);
        return;
    }
    if (r->role != RAFT_LEADER || msg->term != r->current_term) {
        return;
    }
    if (msg->success) {
        if (msg->match_index > r->match_index[msg->from]) {
            r->match_index[msg->from] = msg->match_index;
        }
        r->next_index[msg->from] = r->match_index[msg->from] + 1;
        leader_advance_commit(r);
    } else if (r->next_index[msg->from] > 1) {
        r->next_index[msg->from]--; /* 로그가 갈렸다 -> 한 칸 뒤로 물러 재시도 */
    }
}

void raft_recv(Raft *r, const RaftMsg *msg, RaftMsg *reply, int *has_reply, RaftOutbox *out) {
    *has_reply = 0;
    switch (msg->type) {
        case MSG_REQUEST_VOTE:
            handle_request_vote(r, msg, reply);
            *has_reply = 1;
            break;
        case MSG_REQUEST_VOTE_REPLY:
            handle_request_vote_reply(r, msg, out);
            break;
        case MSG_APPEND_ENTRIES:
            handle_append_entries(r, msg, reply);
            *has_reply = 1;
            break;
        case MSG_APPEND_ENTRIES_REPLY:
            handle_append_entries_reply(r, msg);
            break;
    }
}

void raft_crash_restart(Raft *r) {
    /* 지속 상태(current_term, voted_for, log)는 보존. 휘발 상태만 초기화(§5.1). */
    r->role = RAFT_FOLLOWER;
    r->commit_index = 0;
    r->last_applied = 0;
    r->votes_granted = 0;
    r->election_elapsed = 0;
    r->heartbeat_elapsed = 0;
    memset(r->next_index, 0, sizeof(r->next_index));
    memset(r->match_index, 0, sizeof(r->match_index));
}
