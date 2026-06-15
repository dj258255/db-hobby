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

/* 로그 엔트리 = (term, 커맨드). is_config=1이면 멤버십 변경 엔트리(§6)로,
 * command에 '변경 후 멤버 비트마스크'가 들어간다(상태기계엔 적용 안 함). */
typedef struct {
    uint64_t term;
    int64_t command;
    int is_config;
} RaftEntry;

typedef enum {
    MSG_REQUEST_VOTE,
    MSG_REQUEST_VOTE_REPLY,
    MSG_APPEND_ENTRIES,
    MSG_APPEND_ENTRIES_REPLY,
    MSG_INSTALL_SNAPSHOT,       /* 리더가 압축돼 사라진 prefix를 스냅샷으로 보냄(§7) */
    MSG_INSTALL_SNAPSHOT_REPLY
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

    /* InstallSnapshot (§7) */
    int64_t last_included_index; /* 스냅샷이 담은 마지막 인덱스 */
    uint64_t last_included_term; /* 그 인덱스의 term */
    int64_t snapshot_value;      /* 상태기계 스냅샷(학습용: 레지스터 SM의 값 하나) */
    uint32_t member_mask;        /* 스냅샷 시점의 멤버십(§6): 스냅샷으로 합류한 노드가
                                  * 압축돼 사라진 config 엔트리 대신 이걸로 구성원이 됨 */
} RaftMsg;

/* 노드가 이번 tick/수신에서 '보내고 싶은' 메시지들을 담는 봉투. 하버스가 라우팅한다. */
typedef struct {
    RaftMsg m[RAFT_MAX_NODES * 2];
    int n;
} RaftOutbox;

/* 커밋된 엔트리를 상태기계에 적용할 때 불리는 콜백. 테스트가 노드별 적용 이력을
 * 기록해 '모든 노드가 같은 순서로 같은 커맨드를 적용했나'(합의)를 검증한다. */
typedef void (*RaftApplyFn)(int node_id, int64_t index, int64_t command, void *ctx);

/* InstallSnapshot을 받았을 때, 상태기계에 스냅샷을 설치하라고 부르는 콜백(§7).
 * 학습용 레지스터 SM에선 값 하나(sm_state)를 통째로 얹고 인덱스를 점프한다. */
typedef void (*RaftSnapInstallFn)(int node_id, int64_t last_index, int64_t sm_state, void *ctx);

typedef struct {
    int id;
    int n_nodes;                /* 초기 클러스터 크기(member_mask 초기화용) */
    /* 멤버십(§6): 비트 i가 켜져 있으면 노드 i가 현재 클러스터 구성원. 과반·투표·
     * 복제 대상이 모두 이 마스크로 계산된다. config 엔트리를 '로그에 보자마자'
     * 갱신한다(커밋 때가 아니라 — §6 안전 규칙). 지속 상태. */
    uint32_t member_mask;
    uint32_t base_member_mask;  /* log_base 시점의 멤버십(스냅샷/truncation 복구 기준). 지속 */
    int64_t pending_config_index; /* 리더가 낸 미커밋 config 엔트리 인덱스(-1=없음).
                                   * §6: 직전 변경이 커밋되기 전엔 다음 변경 금지(commit-wait). 휘발 */

    /* ---- 지속 상태(persistent): 크래시에도 살아남아야 한다 ---- */
    uint64_t current_term;
    int voted_for;              /* 이번 term에 표를 준 노드 id, 없으면 -1 */
    RaftEntry *log;             /* log[0]은 base 마커. 논리 인덱스 i = log[i - log_base] */
    int64_t log_len;            /* 마커 포함 물리 길이 */
    int64_t log_cap;
    /* 스냅샷(§7): log[0]은 논리 인덱스 log_base를 대표하고 그 term=lastIncludedTerm.
     * log_base 이전 엔트리는 압축돼 사라졌고 snapshot_value가 그 상태를 대신한다. */
    int64_t log_base;           /* log[0]이 대표하는 논리 인덱스(=마지막 스냅샷 인덱스) */
    int64_t snapshot_value;     /* log_base 시점의 상태기계 스냅샷(레지스터 SM 값) */

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
    RaftSnapInstallFn snap_install; /* InstallSnapshot 수신 시 SM에 스냅샷 설치 */
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

/* 지속 상태(current_term, voted_for, log)를 디스크에 저장하고 fsync한다(§5.1).
 * Raft 규칙: 이 셋은 RPC에 응답하기 '전에' 내구화돼야 안전하다 — votedFor가
 * 안 남으면 크래시 후 같은 term에 두 번 투표해 리더가 둘 생길 수 있다. 0/-1.
 * (15편 WAL의 no-force와 같은 뿌리: fsync가 내구성의 지점.) */
int raft_save(const Raft *r, const char *path);

/* raft_init으로 초기화된 노드에 디스크의 지속 상태를 되읽는다. 휘발 상태는
 * init 기본값 그대로(§5.1: 크래시 시 volatile은 잃어도 됨). 0 성공, -1 실패. */
int raft_load(Raft *r, const char *path);

/* 멤버십 변경(§6, 단일 서버 방식): 리더가 '변경 후 멤버 마스크'를 config 로그
 * 엔트리로 추가하고 복제한다. 한 번에 한 노드만 add/remove해야 옛/새 과반이 겹쳐
 * 안전하다(joint consensus 불필요). 리더 아니면 -1, 성공 시 엔트리 인덱스. */
int64_t raft_change_config(Raft *r, uint32_t new_member_mask);

/* 노드 i가 현재 클러스터 구성원인가(관측용). */
int raft_is_member(const Raft *r, int id);

/* 로그 압축(§7): index까지의 로그를 버리고 스냅샷으로 대체한다. index는 이미
 * 커밋·적용된 지점이어야 한다(index <= commit_index, index > log_base). sm_state는
 * 그 시점의 상태기계 스냅샷(레지스터 SM 값). 이후 log[0]이 index를 대표하고,
 * index+1.. 엔트리만 남는다. 성공 0, 조건 불만족 -1. */
int raft_snapshot(Raft *r, int64_t index, int64_t sm_state);

/* 관측용 헬퍼. */
int64_t raft_last_log_index(const Raft *r);
uint64_t raft_last_log_term(const Raft *r);

#endif /* MINIDB_RAFT_H */
