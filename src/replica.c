/* replica.c — WAL 로그 시핑 복제 (트랙 H1). 설계·경계는 replica.h 참고. */
#include "replica.h"
#include "wal.h" /* REC_PAGE/COMMIT/BEGIN/UNDO — 로그 포맷 공유 */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 0 = 다 읽음, 1 = EOF(또는 잘린 레코드), -1 = 오류. wal.c와 같은 규약. */
static int read_exact(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r == 0) {
            return 1;
        }
        if (r < 0) {
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int replica_open(WalReplica *r, const char *data_path, const char *log_path) {
    if (pager_open(&r->data, data_path) != 0) {
        return -1;
    }
    /* primary가 쓰는 로그를 읽기 전용으로 연다(십된 로그). 아직 없으면 만들되
     * 읽기만 한다 — primary가 세그먼트를 붙이면 apply가 따라잡는다. */
    r->log_fd = open(log_path, O_RDONLY | O_CREAT, 0644);
    if (r->log_fd < 0) {
        pager_close(&r->data);
        return -1;
    }
    r->apply_off = 0;
    r->applied_lsn = 0;
    r->commits_applied = 0;
    return 0;
}

void replica_close(WalReplica *r) {
    if (r->log_fd >= 0) {
        close(r->log_fd);
        r->log_fd = -1;
    }
    pager_close(&r->data);
}

uint64_t replica_position(const WalReplica *r) {
    return r->applied_lsn;
}

/*
 * 로그를 apply_off부터 tail하며, 커밋된 구간의 after-image를 재적용한다.
 *
 * 구간 = REC_PAGE들 ... REC_COMMIT. REC_COMMIT을 만나야 그 구간을 '커밋됨'으로
 * 보고 버퍼링해 둔 after-image를 데이터에 쓴다(원자적으로 보이도록 커밋 경계에서만
 * 적용). REC_BEGIN/REC_UNDO(steal/undo 흔적)는 replay에 불필요하므로 건너뛴다.
 *
 * 오프셋 전진은 REC_COMMIT 경계에서만 한다 — 꼬리에 잘린 레코드가 있으면(primary가
 * 쓰는 중) 그 구간을 미완으로 두고 다음 호출에서 다시 읽는다(스트리밍 안전성).
 *
 * 체크포인트 견딤: primary가 로그를 truncate(체크포인트)하면 파일이 apply_off보다
 * 작아진다. 그 땐 오프셋을 0으로 되감고 처음부터 훑되, applied_lsn보다 작거나 같은
 * 커밋 구간은 '이미 적용됨'으로 건너뛴다(redo가 idempotent라 안전). 단 primary가
 * replica의 catch-up 전에 truncate하면 그 사이 구간은 사라질 수 있다 — 복제 슬롯이
 * 없어서다(replica.h의 프론티어).
 */
int replica_apply(WalReplica *r) {
    off_t eof = lseek(r->log_fd, 0, SEEK_END);
    if (eof < 0) {
        return -1;
    }
    if (eof < r->apply_off) {
        r->apply_off = 0; /* 로그가 truncate됨(체크포인트) -> 되감아 다시 훑는다 */
    }
    if (lseek(r->log_fd, r->apply_off, SEEK_SET) < 0) {
        return -1;
    }

    /* 커밋 마커를 만나기 전까지 after-image를 모아두는 버퍼. 커밋 구간의 REC_PAGE는
     * primary의 stage 상한(WAL_MAX_STAGED)만큼만 나온다. */
    static uint64_t pend_pid[WAL_MAX_STAGED];
    static uint8_t pend_img[WAL_MAX_STAGED][PAGE_SIZE];
    int npend = 0;
    int applied = 0;

    for (;;) {
        uint8_t type;
        int rc = read_exact(r->log_fd, &type, 1);
        if (rc != 0) {
            break; /* EOF/오류 -> 여기까지가 지금 도달한 로그. 다음 호출로 미룬다. */
        }
        uint64_t lsn;
        if (read_exact(r->log_fd, &lsn, sizeof(lsn)) != 0) {
            break; /* lsn 잘림 = 미완 꼬리 */
        }

        if (type == REC_PAGE) {
            uint64_t pid;
            if (read_exact(r->log_fd, &pid, sizeof(pid)) != 0) {
                break;
            }
            if (npend >= WAL_MAX_STAGED) {
                return -1; /* 구간이 stage 상한을 넘었다 = 로그 손상/불일치 */
            }
            if (read_exact(r->log_fd, pend_img[npend], PAGE_SIZE) != 0) {
                break;
            }
            pend_pid[npend] = pid;
            npend++;
        } else if (type == REC_BEGIN) {
            uint64_t base; /* replay엔 불필요 — 건너뛴다 */
            if (read_exact(r->log_fd, &base, sizeof(base)) != 0) {
                break;
            }
        } else if (type == REC_UNDO) {
            uint64_t pid;
            uint8_t before[PAGE_SIZE]; /* before-image는 replay에 안 쓴다 */
            if (read_exact(r->log_fd, &pid, sizeof(pid)) != 0 ||
                read_exact(r->log_fd, before, PAGE_SIZE) != 0) {
                break;
            }
        } else if (type == REC_COMMIT) {
            /* 구간 종료 = 커밋됨. lsn이 이미 적용한 것보다 클 때만 실제 적용
             * (truncate 후 되감아 다시 훑을 때 중복 방지). */
            if (lsn > r->applied_lsn) {
                for (int i = 0; i < npend; i++) {
                    if (pager_write(&r->data, pend_pid[i], pend_img[i]) != 0) {
                        return -1;
                    }
                }
                r->applied_lsn = lsn;
                r->commits_applied++;
                applied++;
            }
            npend = 0;
            r->apply_off = lseek(r->log_fd, 0, SEEK_CUR); /* 커밋 경계에서만 전진 */
        } else {
            break; /* 알 수 없는 타입 = 손상/미완 -> 멈춘다 */
        }
    }

    fsync(r->data.fd); /* 적용분을 replica 디스크에 내구화 */
    return applied;
}
