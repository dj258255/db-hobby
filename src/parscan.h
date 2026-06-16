#ifndef MINIDB_PARSCAN_H
#define MINIDB_PARSCAN_H

#include "heap.h"

#include <stdint.h>

/*
 * parscan — 병렬 풀 스캔 (독립 모듈).
 *
 * 여러 워커 스레드가 한 heap의 disjoint 페이지 범위를 '동시에' 훑는다.
 * PostgreSQL의 parallel sequential scan(여러 워커가 페이지 블록을 나눠 스캔하고
 * leader가 모으는)의 축소판이다.
 *
 * ── 왜 이게 전역 실행기 latch 없이도 안전한가 ──────────────────────────
 * server.c는 실행을 굵은 engine_mtx 하나로 직렬화한다(실행기 계층이 아직 단일
 * 스레드 가정이라). 하지만 버퍼 풀은 트랙 D에서 이미 자체 latch로 스레드
 * 안전하다: bufpool_fetch/unpin이 latch 아래에서 pin_count를 관리하고, eviction
 * victim은 pin된 프레임을 건너뛴다. 그래서 '읽기 전용' 스캔은 여러 스레드가
 * 같은 풀에서 서로 다른(혹은 같은) 페이지를 fetch/unpin해도 안전하다 — 이것이
 * server.c 주석이 말한 "굵은 latch를 계층별로 걷어낼 첫 발판"이다.
 *
 * ── 정직한 경계 ──────────────────────────────────────────────────────
 * 이 모듈은 read-only 스캔만 병렬화한다. db.c 실행기에 배선돼 있지 않다(22편
 * cbtree, 27편 lsm처럼 독립 모듈 + 자체 테스트로 선다). engine_mtx를 통째로
 * 걷어내 '서로 다른 트랜잭션'을 동시에 돌리려면 카탈로그·테이블 WAL·MVCC txn
 * 상태까지 전부 스레드 안전해야 하고, 그게 프론티어다.
 *
 * 또한 버퍼 풀 latch는 페이지 fetch(캐시 미스 시 pager_read 포함)를 직렬화한다.
 * 그래서 콜드 캐시에선 디스크 I/O가 직렬화되고, 병렬화 이득은 페이지가 이미
 * 올라온 뒤의 CPU 작업(슬롯 스캔·술어 평가·가시성 판정)에서 난다. 워밍된 풀
 * (테이블이 다 올라온)에서 이득이 가장 크다. per-page latch로 I/O까지 병렬화하는
 * 건 더 세밀한 풀 설계의 몫(프론티어).
 * ────────────────────────────────────────────────────────────────────
 */

/* 술어: 이 행을 결과에 넣을지(!=0) 말지(0). ctx는 '읽기 전용 공유'여야 한다
 * (스냅샷·스키마·WHERE처럼 불변인 데이터). 워커 스레드들이 병렬로 호출한다 —
 * 공유 가변 상태를 건드리면 안 된다. rec는 호출 동안만 유효(페이지 pin 상태). */
typedef int (*parscan_pred_fn)(RID rid, const void *rec, uint16_t len, void *ctx);

typedef struct {
    RID    *rids; /* pred를 통과한 RID들(페이지 순서) */
    int64_t n, cap;
} ParscanResult;

/* nworkers 스레드로 [first_page, num_pages)를 연속 블록으로 나눠 스캔하고, pred를
 * 통과한 RID를 '페이지 순서'로 *out에 모은다(직렬 heap_scan과 같은 집합·순서).
 * nworkers<1이면 1로 클램프. 성공 0, 실패(스레드 생성/OOM/페이지 fetch 실패) -1.
 * 주의: 각 워커가 한 번에 페이지 하나를 pin하므로 풀 프레임 수 >= nworkers 필요. */
int parscan_collect(Heap *h, int nworkers, parscan_pred_fn pred, void *ctx,
                    ParscanResult *out);

void parscan_result_free(ParscanResult *r);

#endif /* MINIDB_PARSCAN_H */
