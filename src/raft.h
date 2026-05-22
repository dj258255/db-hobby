/* raft — 합의(consensus) 핵심: 리더 선출 + 로그 복제 + 안전성 (트랙 H2)
 *
 * 25·26편의 복제는 primary가 '이미 정해진' 상태에서 WAL을 replica로 흘렸다.
 * 하지만 primary가 죽으면 누가 대신 서나? split-brain(두 노드가 서로 리더라
 * 우기는 것)은 어떻게 막나? 이건 복제가 아니라 '합의'의 문제다 — Raft가 푼다.
 *
 * Raft(Ongaro & Ousterhout, 2014)의 세 조각을 구현한다:
 *   1) 리더 선출  — 노드는 Follower/Candidate/Leader. 리더가 안 보이면(선거
 *      타임아웃) Candidate가 되어 term을 올리고 표를 구한다. 과반이면 Leader.
 *   2) 로그 복제  — Leader가 명령을 로그에 append하고 AppendEntries로 퍼뜨린다.
 *      과반이 복제하면 commit. 로그 일치성(prevLogIndex/Term 검사)으로 갈라진
 *      꼬리를 리더 것으로 덮어쓴다.
 *   3) 안전성     — 선거 제약(자기 로그가 최신인 후보만 당선) + 커밋 규칙(리더는
 *      '현재 term'의 엔트리가 과반에 닿을 때만 commit) = 커밋된 것은 안 뒤집힌다.
 *
 * ── 왜 소켓이 아니라 결정적 시뮬레이션인가 ─────────────────────────────
 * 26편은 복제를 진짜 소켓으로 날랐다. Raft는 일부러 시뮬레이션 네트워크 위에서
 * 돈다. 합의의 정확성은 '적대적 스케줄링'(분단·재정렬·지연·크래시)에서 증명해야
 * 하는데, 실제 소켓+벽시계로는 그 시나리오를 '결정적으로' 재현할 수 없다. 그래서
 * Raft 노드는 순수 로직(tick + RPC 핸들러)이고, 테스트 하버스가 논리 시계와
 * 메시지 라우터·분단 행렬을 단일 스레드로 소유한다(6.824 방식). 소켓(26편)은
 * 프로덕션 운반 수단, 이 시뮬레이터는 프로토콜이 옳음을 증명하는 도구다.
 *
 * ── 정직한 경계 (프론티어) ─────────────────────────────────────────────
 *   - 스냅샷/로그 압축 없음(논문 §7). 로그가 무한히 자란다.
 *   - 멤버십 변경 없음(§6, joint consensus). 클러스터 크기 고정.
 *   - 지속성(currentTerm/votedFor/log의 디스크 저장)은 구조체로 모델링하고
 *     crash_restart가 그걸 보존한다 — WAL에 실제로 fsync하는 배선은 프론티어
 *     (db-hobby 본체엔 [WAL이 있으니] 붙이는 건 자연스러운 다음 일).
 *   - 클라이언트 상호작용(선형화 읽기·리더 리스·중복 제거)은 없다. 상태기계는
 *     테스트가 apply 콜백으로 관찰하는 int64 커맨드 열이다.
 * ────────────────────────────────────────────────────────────────────
 */
#ifndef MINIDB_RAFT_H
#define MINIDB_RAFT_H

#include <stdint.h>

#define RAFT_MAX_NODES 7   /* 학습용 클러스터 상한 */
#define RAFT_MAX_BATCH 16  /* 한 AppendEntries에 싣는 엔트리 상한 */

typedef enum { RAFT_FOLLOWER, RAFT_CANDIDATE, RAFT_LEADER } RaftRole;

/* 로그 엔트리 = (그 엔트리가 만들어진 term, 상태기계 커맨드). */
typedef struct {
    uint64_t term;
    int64_t command;
} RaftEntry;

typedef enum {
    MSG_REQUEST_VOTE,
    MSG_REQUEST_VOTE_REPLY,
    MSG_APPEND_ENTRIES,
    MSG_APPEND_ENTRIES_REPLY
} RaftMsgType;

/* 한 RPC(또는 그 응답). 학습용이라 union 대신 넉넉한 평면 구조체로 복사해 나른다. */
typedef struct {
    RaftMsgType type;
    int from, to;
    uint64_t term;              /* 모든 메시지에 실리는 보낸 이의 term */

    /* RequestVote */
    int64_t last_log_index;
    uint64_t last_log_term;

    /* RequestVoteReply */
    int vote_granted;

    /* AppendEntries */
    int64_t prev_log_index;
    uint64_t prev_log_term;
    int64_t leader_commit;
    RaftEntry entries[RAFT_MAX_BATCH];
    int n_entries;

    /* AppendEntriesReply */
    int success;
    int64_t match_index;        /* 성공 시 팔로워가 확정한 마지막 인덱스 */
} RaftMsg;

/* 노드가 이번 tick/수신에서 '보내고 싶은' 메시지들을 담는 봉투. 하버스가 라우팅한다. */
typedef struct {
    RaftMsg m[RAFT_MAX_NODES * 2];
    int n;
} RaftOutbox;

/* 커밋된 엔트리를 상태기계에 적용할 때 불리는 콜백. 테스트가 노드별 적용 이력을
 * 기록해 '모든 노드가 같은 순서로 같은 커맨드를 적용했나'(합의)를 검증한다. */
typedef void (*RaftApplyFn)(int node_id, int64_t index, int64_t command, void *ctx);

typedef struct {
    int id;
    int n_nodes;                /* 클러스터 크기(과반 계산용) */

    /* ---- 지속 상태(persistent): 크래시에도 살아남아야 한다 ---- */
    uint64_t current_term;
    int voted_for;              /* 이번 term에 표를 준 노드 id, 없으면 -1 */
    RaftEntry *log;             /* 1-indexed. log[0]은 sentinel(term 0) */
    int64_t log_len;            /* sentinel 포함 길이. 마지막 인덱스 = log_len-1 */
    int64_t log_cap;

    /* ---- 휘발 상태(volatile): 크래시 시 초기화 ---- */
    RaftRole role;
    int64_t commit_index;       /* 과반이 복제해 확정된 마지막 인덱스 */
    int64_t last_applied;       /* 상태기계에 적용한 마지막 인덱스 */
    int votes_granted;          /* Candidate일 때 모은 표 수 */

    /* ---- 리더 전용 휘발 상태 ---- */
    int64_t next_index[RAFT_MAX_NODES];  /* 각 팔로워에 보낼 다음 인덱스 */
    int64_t match_index[RAFT_MAX_NODES]; /* 각 팔로워가 확정한 마지막 인덱스 */

    /* ---- 논리 타이머(하버스가 raft_tick으로 구동) ---- */
    int election_elapsed;
    int election_timeout;       /* 이 값에 닿으면 선거 시작. 노드마다 달리 둬 결정적 */
    int heartbeat_elapsed;
    int heartbeat_timeout;      /* 리더가 이 주기로 AppendEntries(하트비트) */

    RaftApplyFn apply;
    void *apply_ctx;
} Raft;

/* 노드를 초기화한다(Follower, term 0, sentinel 로그 하나). election_timeout은
 * 하버스가 노드마다 다르게 세팅해 '누가 먼저 타임아웃하나'를 결정적으로 만든다. */
int raft_init(Raft *r, int id, int n_nodes, int election_timeout, int heartbeat_timeout,
              RaftApplyFn apply, void *apply_ctx);
void raft_free(Raft *r);

/* 논리 시계 1틱. 팔로워/후보면 election_elapsed++ 후 타임아웃 시 선거 시작.
 * 리더면 heartbeat_elapsed++ 후 주기마다 AppendEntries를 out에 채운다. */
void raft_tick(Raft *r, RaftOutbox *out);

/* 리더에게 클라이언트 커맨드를 제출한다. 리더면 자기 로그에 append하고 그 인덱스를
 * 반환(복제는 다음 tick/하트비트에), 리더가 아니면 -1. */
int64_t raft_submit(Raft *r, int64_t command);

/* 수신 처리. RequestVote/AppendEntries는 reply를 채우고(→ 하버스가 되돌림),
 * *_REPLY는 상태를 갱신하며 필요한 후속 메시지를 out에 채운다. */
void raft_recv(Raft *r, const RaftMsg *msg, RaftMsg *reply, int *has_reply, RaftOutbox *out);

/* 크래시 후 재시작: 지속 상태(term/votedFor/log)는 보존, 휘발 상태만 초기화. */
void raft_crash_restart(Raft *r);

/* 관측용 헬퍼. */
int64_t raft_last_log_index(const Raft *r);
uint64_t raft_last_log_term(const Raft *r);

#endif /* MINIDB_RAFT_H */
