/* raft.c — 합의 핵심. 설계·경계는 raft.h 참고.
 * Raft 논문(Ongaro & Ousterhout 2014) §5의 규칙을 학습용으로 옮긴다. */
#include "raft.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── 로그 헬퍼 ──
 * 스냅샷(§7) 이후 로그는 base 오프셋을 가진다: 논리 인덱스 i의 엔트리는 물리
 * 위치 log[i - log_base]에 산다. log[0]은 논리 인덱스 log_base를 대표하는 마커
 * (term=lastIncludedTerm). log_base=0이면 변환은 무연산이라 예전과 동일하다. */

int64_t raft_last_log_index(const Raft *r) {
    return r->log_base + r->log_len - 1; /* 마지막 논리 인덱스 */
}
uint64_t raft_last_log_term(const Raft *r) {
    return r->log[r->log_len - 1].term; /* 물리 마지막 = 논리 마지막 */
}
/* 논리 인덱스 i(>= log_base)의 term. */
static uint64_t term_at(const Raft *r, int64_t i) {
    return r->log[i - r->log_base].term;
}
/* 논리 인덱스 i(>= log_base)의 엔트리 포인터. */
static RaftEntry *ent(Raft *r, int64_t i) {
    return &r->log[i - r->log_base];
}

static void maybe_adopt_config(Raft *r, const RaftEntry *e); /* 아래 정의 */

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
    maybe_adopt_config(r, &e); /* config 엔트리면 즉시 새 멤버십 채택(§6) */
    return 0;
}

/* ── 멤버십(§6): 과반·투표·복제 대상을 member_mask로 계산 ── */
int raft_is_member(const Raft *r, int id) {
    return (r->member_mask >> id) & 1u;
}

int64_t raft_read_confirmed(const Raft *r) {
    return r->read_confirmed_index;
}
static int member_count(const Raft *r) {
    int c = 0;
    for (uint32_t m = r->member_mask; m; m &= m - 1) c++;
    return c;
}
static int majority(const Raft *r) {
    return member_count(r) / 2 + 1;
}
/* config 엔트리가 로그에 들어오면 그 즉시 새 멤버 마스크를 채택한다(§6 규칙:
 * 커밋을 기다리지 않는다). append 경로 어디서든 이걸 부른다. */
static void maybe_adopt_config(Raft *r, const RaftEntry *e) {
    if (e->is_config) {
        r->member_mask = (uint32_t)e->command;
    }
}

/* member_mask를 '권위 있게' 재계산한다: base(log_base 시점 멤버십)에서 시작해
 * 남아있는 로그의 config 엔트리를 순서대로 적용. adopt-on-append의 대칭 짝 —
 * 로그 꼬리가 truncate돼 config 엔트리가 롤백되면 이걸로 member_mask도 되돌린다. */
static void recompute_member_mask(Raft *r) {
    uint32_t m = r->base_member_mask;
    for (int64_t i = r->log_base + 1; i <= r->log_base + r->log_len - 1; i++) {
        RaftEntry *e = &r->log[i - r->log_base];
        if (e->is_config) {
            m = (uint32_t)e->command;
        }
    }
    r->member_mask = m;
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
    /* 리더가 아니게 됐으니 진행 중이던 읽기 확인은 무효 — 고립된 옛 리더의 읽기가
     * 뒤늦게 확인되는 걸 막는다(선형화 안전). */
    r->read_barrier_index = -1;
    r->read_confirmed_index = -1;
}

/* 어떤 메시지든 자기보다 높은 term을 보면 즉시 팔로워로 물러난다(§5.1). */
static void step_down_if_higher(Raft *r, uint64_t term) {
    if (term > r->current_term) {
        become_follower(r, term);
    }
}

static void push_msg(RaftOutbox *out, const RaftMsg *m) {
    if (out->n < (int)(sizeof(out->m) / sizeof(out->m[0]))) {
        out->m[out->n++] = *m;
    }
}

/* 리더가 압축된 prefix를 스냅샷으로 통째로 보낸다(§7). */
static void send_install_snapshot(Raft *r, int peer, RaftOutbox *out) {
    RaftMsg m;
    memset(&m, 0, sizeof(m));
    m.type = MSG_INSTALL_SNAPSHOT;
    m.from = r->id;
    m.to = peer;
    m.term = r->current_term;
    m.last_included_index = r->log_base;
    m.last_included_term = r->log[0].term;
    m.snapshot_value = r->snapshot_value;
    m.member_mask = r->base_member_mask; /* 스냅샷 시점 멤버십 — 합류자가 이걸로 구성원이 됨 */
    push_msg(out, &m);
}

static void send_append_entries(Raft *r, int peer, RaftOutbox *out) {
    int64_t prev = r->next_index[peer] - 1;
    /* 팔로워가 필요로 하는 prev가 우리 로그에서 이미 압축돼 사라졌다면(<log_base),
     * AppendEntries로는 보낼 수 없다 -> 스냅샷을 통째로 보낸다. */
    if (prev < r->log_base) {
        send_install_snapshot(r, peer, out);
        return;
    }
    RaftMsg m;
    memset(&m, 0, sizeof(m));
    m.type = MSG_APPEND_ENTRIES;
    m.from = r->id;
    m.to = peer;
    m.term = r->current_term;
    m.prev_log_index = prev;
    m.prev_log_term = term_at(r, prev);
    m.leader_commit = r->commit_index;
    m.read_epoch = r->read_epoch; /* ReadIndex: 팔로워가 되돌려줄 배리어 식별자 */
    int n = 0;
    for (int64_t idx = r->next_index[peer]; idx <= raft_last_log_index(r) && n < RAFT_MAX_BATCH;
         idx++) {
        m.entries[n++] = *ent(r, idx);
    }
    m.n_entries = n;
    push_msg(out, &m);
}

static void become_leader(Raft *r, RaftOutbox *out) {
    r->role = RAFT_LEADER;
    r->read_barrier_index = -1; /* 새 리더: 진행 중 읽기 없음 */
    r->read_confirmed_index = -1;
    for (int i = 0; i < RAFT_MAX_NODES; i++) {
        if (!raft_is_member(r, i)) continue;
        r->next_index[i] = raft_last_log_index(r) + 1;
        r->match_index[i] = 0;
    }
    r->match_index[r->id] = raft_last_log_index(r);
    r->heartbeat_elapsed = 0;
    /* 즉시 하트비트(빈 AppendEntries)로 권위를 알린다 — 다른 후보의 선거를 막는다. */
    for (int i = 0; i < RAFT_MAX_NODES; i++) {
        if (i != r->id && raft_is_member(r, i)) {
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
    for (int i = 0; i < RAFT_MAX_NODES; i++) {
        if (i == r->id || !raft_is_member(r, i)) {
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
            r->apply(r->id, r->last_applied, ent(r, r->last_applied)->command, r->apply_ctx);
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
    r->member_mask = (n_nodes >= 32) ? 0xFFFFFFFFu : ((1u << n_nodes) - 1u); /* 초기 0..n-1 구성원 */
    r->base_member_mask = r->member_mask;
    r->pending_config_index = -1;
    r->current_term = 0;
    r->voted_for = -1;
    r->log_cap = 16;
    r->log = malloc((size_t)r->log_cap * sizeof(RaftEntry));
    if (!r->log) {
        return -1;
    }
    r->log[0].term = 0; /* sentinel */
    r->log[0].command = 0;
    r->log[0].is_config = 0;
    r->log_len = 1;
    r->role = RAFT_FOLLOWER;
    r->commit_index = 0;
    r->last_applied = 0;
    r->election_timeout = election_timeout;
    r->heartbeat_timeout = heartbeat_timeout;
    r->read_barrier_index = -1;
    r->read_confirmed_index = -1;
    r->read_ack_mask = 0;
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
            for (int i = 0; i < RAFT_MAX_NODES; i++) {
                if (i != r->id && raft_is_member(r, i)) {
                    send_append_entries(r, i, out);
                }
            }
        }
    } else if (raft_is_member(r, r->id)) { /* 구성원이 아니면 선거를 시작하지 않는다 */
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
    RaftEntry e = {.term = r->current_term, .command = command, .is_config = 0};
    if (log_append(r, e) != 0) {
        return -1;
    }
    r->match_index[r->id] = raft_last_log_index(r);
    return raft_last_log_index(r);
}

/* 멤버십 변경(§6): 리더가 '변경 후 마스크'를 config 엔트리로 추가한다. log_append이
 * 즉시 member_mask를 채택하고(§6), 새로 들어온 노드에는 next/match를 초기화해 다음
 * 하트비트부터 복제한다. 안전을 위해 한 번에 한 노드 delta만 허용한다. */
int64_t raft_change_config(Raft *r, uint32_t new_mask) {
    if (r->role != RAFT_LEADER) {
        return -1;
    }
    uint32_t diff = r->member_mask ^ new_mask;
    if (diff == 0 || (diff & (diff - 1)) != 0) {
        return -1; /* 정확히 한 비트만 바뀌어야 옛/새 과반이 겹쳐 안전(단일 서버 변경) */
    }
    /* §6 commit-wait: 직전 config 변경이 커밋되기 전엔 다음 변경을 금지한다.
     * 겹치는 두 미커밋 변경이 옛/새 과반의 교집합 보장을 깨는 것을 막는다(단일-서버
     * 방식의 알려진 결함을 Ongaro가 정정한 규칙). */
    if (r->pending_config_index >= 0 && r->pending_config_index > r->commit_index) {
        return -1;
    }
    RaftEntry e = {.term = r->current_term, .command = (int64_t)new_mask, .is_config = 1};
    if (log_append(r, e) != 0) { /* log_append이 member_mask를 new_mask로 채택 */
        return -1;
    }
    r->match_index[r->id] = raft_last_log_index(r);
    r->pending_config_index = raft_last_log_index(r); /* 이 변경이 커밋될 때까지 다음 변경 금지 */
    /* 새로 추가된 노드(diff 비트가 new_mask에 켜져 있으면)에 복제 시작점 세팅 */
    for (int i = 0; i < RAFT_MAX_NODES; i++) {
        if ((new_mask & (1u << i)) && i != r->id && r->next_index[i] == 0) {
            r->next_index[i] = 1; /* 스냅샷/백트래킹으로 맞춰짐 */
            r->match_index[i] = 0;
        }
    }
    return raft_last_log_index(r);
}

/* 선형화 읽기 시작(ReadIndex): commit_index를 read index로 잡고, '지금도 리더'임을
 * 과반 하트비트 ack로 확인하러 하트비트를 뿌린다. handle_append_entries_reply가
 * 그 ack를 세어 과반이 되면 read_confirmed_index를 세운다. */
int64_t raft_read_index(Raft *r, RaftOutbox *out) {
    if (r->role != RAFT_LEADER) {
        return -1;
    }
    r->read_barrier_index = r->commit_index;
    r->read_confirmed_index = -1;
    r->read_ack_mask = (1u << r->id); /* 자기 자신은 당연히 ack */
    r->read_epoch++;                  /* 새 배리어 -> 이 이후 하트비트 응답만 센다(F1) */
    if (member_count(r) == 1) { /* 단일 노드면 즉시 확인 */
        r->read_confirmed_index = r->read_barrier_index;
        r->read_barrier_index = -1;
    }
    for (int i = 0; i < RAFT_MAX_NODES; i++) {
        if (i != r->id && raft_is_member(r, i)) {
            send_append_entries(r, i, out);
        }
    }
    return r->commit_index;
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
    reply->read_epoch = msg->read_epoch; /* ReadIndex: 리더의 배리어 식별자를 그대로 되돌린다 */

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
    if (msg->prev_log_index >= r->log_base) {
        if (term_at(r, msg->prev_log_index) != msg->prev_log_term) {
            return; /* prev에서 term 불일치 -> 실패 */
        }
    }
    /* prev < log_base면 그 지점은 내 스냅샷 안이라 term 검증 불가하지만, 스냅샷이
     * 그 prefix를 이미 보장하므로 통과시키고 아래에서 log_base 이하 엔트리는 건너뛴다. */

    /* 엔트리 이어붙이기: 충돌(term 다름)이 나는 첫 지점부터 꼬리를 덮어쓴다. */
    int truncated = 0;
    for (int i = 0; i < msg->n_entries; i++) {
        int64_t idx = msg->prev_log_index + 1 + i;
        if (idx <= r->log_base) {
            continue; /* 이미 스냅샷에 담긴 인덱스 -> 건너뜀 */
        }
        if (idx <= raft_last_log_index(r)) {
            if (term_at(r, idx) != msg->entries[i].term) {
                r->log_len = idx - r->log_base; /* 충돌 꼬리 절단(물리 길이) */
                truncated = 1;
                log_append(r, msg->entries[i]);
            }
            /* 같으면 이미 가진 엔트리 -> 유지(커밋된 걸 지우지 않기 위함) */
        } else {
            log_append(r, msg->entries[i]);
        }
    }
    reply->success = 1;
    reply->match_index = msg->prev_log_index + msg->n_entries;

    /* 꼬리를 truncate했다면 config 엔트리가 롤백됐을 수 있으므로, member_mask를 로그로
     * 부터 권위 있게 재계산한다(adopt-on-append의 대칭 짝 — C2 수정). truncation이
     * 없으면 log_append의 전방 채택으로 충분하다(정상 append 경로엔 손 안 댐). */
    if (truncated) {
        recompute_member_mask(r);
    }

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
        /* §5.4.2: 리더는 '현재 term'의 엔트리가 과반에 닿을 때만 commit한다.
         * N > commit_index >= log_base 이므로 term_at(N)은 항상 유효(N > log_base). */
        if (term_at(r, N) != r->current_term) {
            continue;
        }
        /* 구성원 중 match_index >= N인 수를 센다. 자기 match_index[self]는 항상
         * last_log_index로 유지되므로 자기도 이 루프에 포함된다(구성원일 때). */
        int count = 0;
        for (int i = 0; i < RAFT_MAX_NODES; i++) {
            if (raft_is_member(r, i) && r->match_index[i] >= N) {
                count++;
            }
        }
        if (count >= majority(r)) {
            r->commit_index = N;
            if (r->pending_config_index >= 0 && r->commit_index >= r->pending_config_index) {
                r->pending_config_index = -1; /* config 변경 커밋됨 -> 다음 변경 허용 */
            }
            apply_committed(r);
            /* §6(L2): 자기 자신을 제거하는 config가 '커밋된 뒤' 리더가 물러난다.
             * (커밋 전엔 계속 리더로 남아 그 제거 엔트리를 새 구성에 복제해야 한다.) */
            if (!raft_is_member(r, r->id)) {
                become_follower(r, r->current_term);
            }
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
        /* ReadIndex: 이번 배리어의 하트비트에 대한 응답만(epoch 일치) 센다. 과반이면
         * '지금도 리더' 확인 -> read_confirmed_index. 재정렬로 도착한 pre-barrier
         * 응답(옛 epoch)은 제외 — 고립된 옛 리더의 오확인을 막는다(F1 수정). */
        if (r->read_barrier_index >= 0 && msg->read_epoch == r->read_epoch) {
            r->read_ack_mask |= (1u << msg->from);
            int acks = 0;
            for (int i = 0; i < RAFT_MAX_NODES; i++) {
                if (raft_is_member(r, i) && (r->read_ack_mask & (1u << i))) acks++;
            }
            if (acks >= majority(r)) {
                r->read_confirmed_index = r->read_barrier_index;
                r->read_barrier_index = -1;
            }
        }
    } else if (r->next_index[msg->from] > 1) {
        r->next_index[msg->from]--; /* 로그가 갈렸다 -> 한 칸 뒤로 물러 재시도 */
    }
}

/* InstallSnapshot 수신(§7): 리더가 보낸 스냅샷으로 로그 prefix를 통째로 대체한다. */
static void handle_install_snapshot(Raft *r, const RaftMsg *msg, RaftMsg *reply) {
    step_down_if_higher(r, msg->term);
    memset(reply, 0, sizeof(*reply));
    reply->type = MSG_INSTALL_SNAPSHOT_REPLY;
    reply->from = r->id;
    reply->to = msg->from;
    reply->term = r->current_term;
    if (msg->term < r->current_term) {
        reply->match_index = 0; /* 낡은 리더 -> 무시 */
        return;
    }
    r->role = RAFT_FOLLOWER;
    r->election_elapsed = 0;
    reply->term = r->current_term;

    /* 이미 커밋된 지점까지의 스냅샷이면(오래됐거나 중복) 설치하지 않는다.
     * match_index는 스냅샷 인덱스로만 답한다 — raft_last_log_index를 답하면 팔로워의
     * 미커밋·갈린 꼬리까지 확정한 것처럼 보여, 리더가 그걸 과반에 넣어 divergent 인덱스를
     * 커밋할 수 있다(State Machine Safety 위반). 가드도 log_base가 아니라 commit_index
     * 기준으로 둬, last_included가 내 커밋보다 낮으면 commit을 되돌리지 않는다. */
    if (msg->last_included_index <= r->commit_index) {
        reply->match_index = msg->last_included_index;
        return;
    }
    /* 스냅샷 설치: 로그를 버리고 base를 스냅샷 지점으로 옮긴다(학습용: 팔로워가
     * 그 뒤 일부 엔트리를 갖고 있더라도 통째로 버린다 — 정확하되 약간 비효율). */
    r->log[0].term = msg->last_included_term;
    r->log[0].command = 0;
    r->log[0].is_config = 0;
    r->log_len = 1;
    r->log_base = msg->last_included_index;
    r->commit_index = msg->last_included_index;
    r->last_applied = msg->last_included_index;
    r->snapshot_value = msg->snapshot_value;
    /* 스냅샷에 담긴(압축된) config를 채택 — 스냅샷으로 합류한 노드가 구성원이 된다(C1 수정). */
    r->base_member_mask = msg->member_mask;
    r->member_mask = msg->member_mask;
    if (r->snap_install) {
        r->snap_install(r->id, msg->last_included_index, msg->snapshot_value, r->apply_ctx);
    }
    reply->match_index = msg->last_included_index;
}

static void handle_install_snapshot_reply(Raft *r, const RaftMsg *msg) {
    if (msg->term > r->current_term) {
        become_follower(r, msg->term);
        return;
    }
    if (r->role != RAFT_LEADER || msg->term != r->current_term) {
        return;
    }
    if (msg->match_index > r->match_index[msg->from]) {
        r->match_index[msg->from] = msg->match_index;
    }
    r->next_index[msg->from] = r->match_index[msg->from] + 1;
    leader_advance_commit(r);
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
        case MSG_INSTALL_SNAPSHOT:
            handle_install_snapshot(r, msg, reply);
            *has_reply = 1;
            break;
        case MSG_INSTALL_SNAPSHOT_REPLY:
            handle_install_snapshot_reply(r, msg);
            break;
    }
}

/* 로그 압축(§7): index까지 버리고 스냅샷으로 대체. index+1.. 엔트리만 남긴다. */
int raft_snapshot(Raft *r, int64_t index, int64_t sm_state) {
    if (index <= r->log_base || index > r->commit_index || index > raft_last_log_index(r)) {
        return -1; /* 이미 압축됐거나, 아직 커밋 안 됐거나, 범위 밖 */
    }
    uint64_t inc_term = term_at(r, index);
    /* index까지의 config 엔트리를 base_member_mask로 접어 넣는다(그것들이 압축돼
     * 사라지므로 — 이후 recompute/install의 기준이 된다). */
    for (int64_t i = r->log_base + 1; i <= index; i++) {
        if (ent(r, i)->is_config) {
            r->base_member_mask = (uint32_t)ent(r, i)->command;
        }
    }
    int64_t keep = raft_last_log_index(r) - index; /* index+1.. 남길 엔트리 수 */
    int64_t src = (index + 1) - r->log_base;       /* 남길 첫 엔트리의 물리 위치 */
    memmove(&r->log[1], &r->log[src], (size_t)keep * sizeof(RaftEntry));
    r->log[0].term = inc_term; /* 마커: 논리 인덱스 index를 대표 */
    r->log[0].command = 0;
    r->log[0].is_config = 0;
    r->log_len = keep + 1;
    r->log_base = index;
    r->snapshot_value = sm_state;
    return 0;
}

void raft_crash_restart(Raft *r) {
    /* 지속 상태(current_term, voted_for, log, 스냅샷)는 보존. 휘발만 초기화(§5.1).
     * commit/applied의 하한은 log_base다 — 스냅샷은 이미 적용된 상태를 대표하므로
     * 재시작 후에도 그 앞으로는 돌아가지 않는다(스냅샷 없으면 log_base=0 = 예전과 동일). */
    r->role = RAFT_FOLLOWER;
    r->commit_index = r->log_base;
    r->last_applied = r->log_base;
    r->votes_granted = 0;
    r->election_elapsed = 0;
    r->heartbeat_elapsed = 0;
    r->pending_config_index = -1; /* 휘발 */
    r->read_barrier_index = -1;
    r->read_confirmed_index = -1;
    memset(r->next_index, 0, sizeof(r->next_index));
    memset(r->match_index, 0, sizeof(r->match_index));
}

/* ── 지속성(§5.1): current_term/voted_for/log를 디스크에 내구화 ──
 * 포맷(단순 바이너리): [current_term 8B][voted_for 4B][log_len 8B][entries...].
 * 진짜 Raft는 이 저장을 RPC 응답 '전에' 부른다(그래야 표/로그가 내구화된 뒤
 * 응답이 나감). 여기선 하버스가 크래시 지점에서 호출해 디스크 라운드트립을
 * 검증한다 — 코어를 순수하게 유지하려는 학습용 타협. */

static int wr_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) {
            return -1;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}
static int rd_all(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) {
            return -1;
        }
        p += (size_t)r;
        n -= (size_t)r;
    }
    return 0;
}

int raft_save(const Raft *r, const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -1;
    }
    int rc = 0;
    if (wr_all(fd, &r->current_term, sizeof(r->current_term)) != 0 ||
        wr_all(fd, &r->voted_for, sizeof(r->voted_for)) != 0 ||
        wr_all(fd, &r->member_mask, sizeof(r->member_mask)) != 0 ||
        wr_all(fd, &r->base_member_mask, sizeof(r->base_member_mask)) != 0 ||
        wr_all(fd, &r->log_base, sizeof(r->log_base)) != 0 ||
        wr_all(fd, &r->snapshot_value, sizeof(r->snapshot_value)) != 0 ||
        wr_all(fd, &r->log_len, sizeof(r->log_len)) != 0 ||
        wr_all(fd, r->log, (size_t)r->log_len * sizeof(RaftEntry)) != 0) {
        rc = -1;
    }
    if (fsync(fd) != 0) { /* 내구성의 지점 — 여기를 지나야 '저장됨' */
        rc = -1;
    }
    close(fd);
    return rc;
}

int raft_load(Raft *r, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    uint64_t term;
    int voted;
    uint32_t mask, basemask;
    int64_t base, snap, len;
    if (rd_all(fd, &term, sizeof(term)) != 0 || rd_all(fd, &voted, sizeof(voted)) != 0 ||
        rd_all(fd, &mask, sizeof(mask)) != 0 || rd_all(fd, &basemask, sizeof(basemask)) != 0 ||
        rd_all(fd, &base, sizeof(base)) != 0 || rd_all(fd, &snap, sizeof(snap)) != 0 ||
        rd_all(fd, &len, sizeof(len)) != 0 || len < 1) {
        close(fd);
        return -1;
    }
    if (len > r->log_cap) {
        RaftEntry *p = realloc(r->log, (size_t)len * sizeof(RaftEntry));
        if (!p) {
            close(fd);
            return -1;
        }
        r->log = p;
        r->log_cap = len;
    }
    if (rd_all(fd, r->log, (size_t)len * sizeof(RaftEntry)) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    r->current_term = term;
    r->voted_for = voted;
    r->member_mask = mask;
    r->base_member_mask = basemask;
    r->pending_config_index = -1; /* 휘발: 재기동 리더는 리더가 아니다 */
    r->log_base = base;
    r->snapshot_value = snap;
    r->log_len = len;
    /* 스냅샷은 이미 적용된 상태를 대표하므로 commit/applied 하한을 log_base로. */
    r->commit_index = base;
    r->last_applied = base;
    /* 스냅샷이 있었다면(log_base>0), 1..log_base 엔트리는 압축돼 사라졌으므로 재적용
     * 경로로는 SM을 복원할 수 없다 -> 스냅샷 값을 SM에 직접 시드해야 한다.
     * (이걸 빠뜨리면 재기동 노드의 SM에 log_base까지의 상태가 영구히 빈다.) */
    if (r->log_base > 0 && r->snap_install) {
        r->snap_install(r->id, r->log_base, r->snapshot_value, r->apply_ctx);
    }
    return 0;
}
