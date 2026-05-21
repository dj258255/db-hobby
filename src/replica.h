/* replica — WAL 로그 시핑 복제 (트랙 H1)
 *
 * primary는 커밋할 때 WAL에 REC_PAGE(after-image)들과 REC_COMMIT을 남긴다(15편).
 * replica는 그 로그를 tail하며, 커밋된 구간의 after-image를 자기 데이터 파일에
 * 순서대로 재적용한다 — 즉 crash 복구의 redo Pass를 '파괴적 복구'가 아니라
 * '증분·연속'으로 돌리는 것이다. PostgreSQL 스트리밍 복제(walreceiver가 WAL을
 * replay)의 핵심과 같은 구조다.
 *
 * 정확성의 뿌리: 페이지 전체 물리 로깅이라 redo는 idempotent다. 그래서 같은
 * 커밋 구간을 두 번 적용해도 결과가 같고, lsn으로 '이미 적용한 커밋'만 거르면
 * 체크포인트로 로그가 truncate돼 오프셋이 되감겨도 안전하게 이어 붙는다.
 *
 * 일관성 모델: 비동기 물리 로그 시핑, 최종 일관성. replica는 primary보다 뒤처질
 * 수 있고(lag), 커밋된(=REC_COMMIT까지 도달한) 상태만 보인다 — 미완 꼬리는
 * 적용하지 않는다(읽기 일관성). 커밋 마커 앞에서 로그가 잘려 있으면(primary가
 * 쓰는 중) 그 구간은 다음 apply까지 미룬다.
 *
 * 정직한 경계 (프론티어):
 *   - 네트워크 전송(walsender/walreceiver 소켓)은 없다. 이 모듈은 '십된 로그
 *     파일'을 읽는다 — TCP로 바이트를 나르는 건 19편 서버 위에 얹을 다음 일.
 *   - 복제 슬롯(replication slot)이 없다. primary가 replica가 따라잡기 전에
 *     체크포인트로 로그를 truncate하면, 그 사이 커밋 구간을 놓칠 수 있다(아래
 *     replica_apply 주석). 진짜 시스템은 슬롯으로 로그를 붙잡아 둔다.
 *   - 자동 failover·합의는 트랙 H2(Raft)로 분리. 이건 read replica까지다.
 */
#ifndef MINIDB_REPLICA_H
#define MINIDB_REPLICA_H

#include "pager.h"
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    Pager data;               /* replica 자신의 데이터 파일(primary를 뒤따르는 사본) */
    int log_fd;               /* primary WAL 로그의 읽기 전용 fd(십된 로그) */
    off_t apply_off;          /* 커밋 구간을 적용 완료한 바이트 오프셋 */
    uint64_t applied_lsn;     /* 마지막으로 적용한 커밋의 lsn(복제 위치). 0=없음 */
    uint64_t commits_applied; /* 지금까지 적용한 커밋 트랜잭션 수(단조 증가) */
} WalReplica;

/* replica 데이터 파일과 primary 로그(읽기 전용)를 연다. 0 성공, -1 실패. */
int replica_open(WalReplica *r, const char *data_path, const char *log_path);

/* fd만 닫는다. */
void replica_close(WalReplica *r);

/* 지금 로그에 도달해 있는 '새' 커밋 구간들을 재적용한다(catch-up).
 * 이번 호출에서 적용한 커밋 수를 반환, 오류 시 -1.
 * 미완 꼬리(REC_COMMIT 미도달)는 적용하지 않고 다음 호출로 미룬다. */
int replica_apply(WalReplica *r);

/* 현재 복제 위치(마지막으로 적용한 커밋의 lsn). primary와 비교해 lag 측정. */
uint64_t replica_position(const WalReplica *r);

#endif /* MINIDB_REPLICA_H */
