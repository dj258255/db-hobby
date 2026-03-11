#include "mvcc.h"

#include <string.h>

void txnlog_init(TxnLog *log) {
    memset(log->status, 0, sizeof(log->status)); /* 전부 TXN_INPROGRESS */
}

void txnlog_begin(TxnLog *log, int txn) {
    if (txn > 0 && txn < TXN_MAX) {
        log->status[txn] = TXN_INPROGRESS;
    }
}

void txnlog_commit(TxnLog *log, int txn) {
    if (txn > 0 && txn < TXN_MAX) {
        log->status[txn] = TXN_COMMITTED;
    }
}

void txnlog_abort(TxnLog *log, int txn) {
    if (txn > 0 && txn < TXN_MAX) {
        log->status[txn] = TXN_ABORTED;
    }
}

TxnStatus txnlog_status(const TxnLog *log, int txn) {
    if (txn > 0 && txn < TXN_MAX) {
        return (TxnStatus)log->status[txn];
    }
    return TXN_INPROGRESS; /* 범위 밖/0 = 아직 진행 중 취급(안 커밋됨) */
}

int mvcc_visible(const TxnLog *log, int xmin, int xmax) {
    /* 생성자가 커밋 안 했으면(진행 중이거나 abort) 이 버전은 없는 것이다. */
    if (txnlog_status(log, xmin) != TXN_COMMITTED) {
        return 0;
    }
    /* 커밋된 트랜잭션이 지웠으면 안 보인다. (xmax==0 = 안 지워짐, abort된 삭제 = 무효라 보임) */
    if (xmax != 0 && txnlog_status(log, xmax) == TXN_COMMITTED) {
        return 0;
    }
    return 1;
}
