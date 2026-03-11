#ifndef MINIDB_MVCC_H
#define MINIDB_MVCC_H

/*
 * MVCC(다중 버전 동시성 제어)의 두뇌 — 트랜잭션 상태 로그 + 가시성 규칙.
 *
 * PostgreSQL식: 행을 덮어쓰지 않고 버전을 쌓는다. 각 행 버전은 두 트랜잭션 id를 단다.
 *   xmin = 이 버전을 만든(INSERT한) 트랜잭션
 *   xmax = 이 버전을 지운(DELETE한) 트랜잭션 (0이면 아직 안 지워짐)
 * UPDATE = 옛 버전에 xmax 찍고 + 새 버전을 xmin으로 삽입.
 *
 * 가시성 규칙(이 모듈의 핵심):
 *   보인다  <=>  xmin이 커밋됐고  AND  (xmax==0 이거나 xmax가 아직 커밋 안 됨)
 *
 * 여기서 롤백이 공짜로 나온다 — 생성자가 abort면 그 버전은 안 보이고(INSERT 롤백),
 * 삭제자가 abort면 그 버전이 다시 보인다(DELETE 롤백). 상태만 바꾸면 끝.
 *
 * (이번 1단계는 스냅샷 없는 "read committed"식 — 지금 커밋된 것을 본다.
 *  트랜잭션 시작 시점 스냅샷은 2단계에서 더한다.)
 */

typedef enum {
    TXN_INPROGRESS = 0, /* memset 0 기본값 — 아직 진행 중(또는 안 생긴 id) */
    TXN_COMMITTED,
    TXN_ABORTED
} TxnStatus;

#define TXN_MAX 65536 /* 학습용 상한: txn id < TXN_MAX */

typedef struct {
    unsigned char status[TXN_MAX]; /* txn id -> TxnStatus */
} TxnLog;

void txnlog_init(TxnLog *log);
void txnlog_begin(TxnLog *log, int txn);  /* 진행 중으로 표시 */
void txnlog_commit(TxnLog *log, int txn);
void txnlog_abort(TxnLog *log, int txn);
TxnStatus txnlog_status(const TxnLog *log, int txn);

/* 행 버전(xmin,xmax)이 지금 보이나? 1 보임 / 0 안 보임. xmax==0 = 아직 안 지워짐. */
int mvcc_visible(const TxnLog *log, int xmin, int xmax);

#endif /* MINIDB_MVCC_H */
