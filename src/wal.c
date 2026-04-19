#include "wal.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int wal_test_crash_after_log = 0;
int wal_test_crash_before_commit = 0;

typedef struct {
    page_id_t page_id;
    uint8_t data[PAGE_SIZE];
} StagedPage;

/* 레코드 공통 머리: type(1B) + lsn(8B). 그 뒤에 타입별 payload. */
#define REC_PAGE 'P'   /* pid + after-image (redo) */
#define REC_COMMIT 'C' /* 커밋 마커 */
#define REC_BEGIN 'B'  /* base_pages (steal이 처음 일어날 때 한 번) */
#define REC_UNDO 'U'   /* pid + before-image (undo) */

/* no-force라 로그가 커밋 이력으로 자란다. 이 크기를 넘으면 커밋 끝에 체크포인트
 * (데이터 fsync -> 로그 truncate)로 자른다. 복구 시간과 로그 크기의 상한. */
#define WAL_CHECKPOINT_BYTES (4 << 20)

static int write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) {
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* 0 = 다 읽음, 1 = EOF(또는 잘린 레코드), -1 = 오류 */
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

/* 레코드 머리(type + lsn)를 쓴다. 부여한 lsn을 반환, 실패 시 0. */
static uint64_t write_hdr(Wal *w, uint8_t type) {
    uint64_t lsn = w->next_lsn++;
    if (write_all(w->log_fd, &type, 1) != 0 ||
        write_all(w->log_fd, &lsn, sizeof(lsn)) != 0) {
        return 0;
    }
    return lsn;
}

/* -- steal한 페이지 추적 (페이지당 undo는 최초 1회 = first-write-wins) -- */
static int spilled_contains(Wal *w, page_id_t pid) {
    for (int i = 0; i < w->num_spilled; i++) {
        if (w->spilled[i] == pid) {
            return 1;
        }
    }
    return 0;
}

static int spilled_add(Wal *w, page_id_t pid) {
    if (w->num_spilled >= w->cap_spilled) {
        int cap = w->cap_spilled ? w->cap_spilled * 2 : 64;
        uint64_t *p = realloc(w->spilled, sizeof(uint64_t) * (size_t)cap);
        if (!p) {
            return -1;
        }
        w->spilled = p;
        w->cap_spilled = cap;
    }
    w->spilled[w->num_spilled++] = pid;
    return 0;
}

/* 로그에서 n바이트를 건너뛴다(현재 오프셋 기준). 성공 0, 실패/EOF -1. */
static int skip_bytes(int fd, off_t n) {
    return lseek(fd, n, SEEK_CUR) < 0 ? -1 : 0;
}

/*
 * 크래시 복구. no-force라 로그엔 여러 트랜잭션의 이력이 순서대로 쌓여 있다.
 *   - Pass 1: 앞에서부터 훑으며, REC_COMMIT를 만날 때마다 그 구간의 REC_PAGE
 *     (after-image)들을 데이터에 재적용(redo). 커밋 순서 그대로 반복하므로 마지막
 *     상태가 정확히 복원된다(페이지 전체 물리 로깅 = idempotent, pageLSN 불필요).
 *     (구간당 REC_PAGE는 커밋 시 in-pool dirty만 stage하므로 <= WAL_MAX_STAGED.)
 *   - Pass 2: 마지막 커밋 마커 뒤에 남은 꼬리 = 미완(loser). REC_UNDO(before-image)로
 *     원복하고 새로 할당한 페이지(>= base)는 잘라낸다. 원자성.
 *     (loser의 REC_UNDO는 개수 제한이 없으므로 스트리밍으로 적용.)
 * 복구 끝 = 데이터 fsync + 로그 truncate — 여는 것 자체가 체크포인트다.
 */
static int wal_recover(Wal *w) {
    if (lseek(w->log_fd, 0, SEEK_SET) < 0) {
        return -1;
    }
    StagedPage *redo = malloc(sizeof(StagedPage) * WAL_MAX_STAGED);
    if (!redo) {
        return -1;
    }
    int nredo = 0;
    int rc = 0;
    off_t loser_start = 0; /* 마지막 커밋 마커 직후 오프셋 = loser 구간의 시작 */

    /* Pass 1: 커밋 구간마다 after-image를 커밋 순서대로 재적용(redo) */
    for (;;) {
        uint8_t type;
        if (read_exact(w->log_fd, &type, 1) != 0) {
            break; /* EOF 또는 오류 */
        }
        if (skip_bytes(w->log_fd, sizeof(uint64_t)) != 0) {
            break; /* lsn */
        }
        if (type == REC_BEGIN) {
            if (skip_bytes(w->log_fd, sizeof(uint64_t)) != 0) {
                break;
            }
        } else if (type == REC_UNDO) {
            if (skip_bytes(w->log_fd, sizeof(uint64_t) + PAGE_SIZE) != 0) {
                break; /* 커밋된 구간의 undo는 안 쓴다. loser 것은 pass 2에서 */
            }
        } else if (type == REC_PAGE) {
            uint64_t pid;
            if (read_exact(w->log_fd, &pid, sizeof(pid)) != 0) {
                break; /* 잘린 레코드 */
            }
            if (nredo < WAL_MAX_STAGED) {
                redo[nredo].page_id = pid;
                if (read_exact(w->log_fd, redo[nredo].data, PAGE_SIZE) != 0) {
                    break;
                }
                nredo++;
            } else if (skip_bytes(w->log_fd, PAGE_SIZE) != 0) {
                break;
            }
        } else if (type == REC_COMMIT) {
            /* 이 구간은 커밋됨 -> redo. 다음 구간을 이어서 훑는다. */
            for (int i = 0; i < nredo; i++) {
                if (pager_write(&w->data, redo[i].page_id, redo[i].data) != 0) {
                    rc = -1;
                }
            }
            nredo = 0;
            loser_start = lseek(w->log_fd, 0, SEEK_CUR);
        } else {
            break;
        }
    }

    /* Pass 2: 마지막 커밋 뒤 꼬리(loser) -> before-image로 원복(스트리밍) + 새 페이지 truncate. */
    {
        int have_base = 0;
        uint64_t base = 0;
        lseek(w->log_fd, loser_start, SEEK_SET);
        for (;;) {
            uint8_t type;
            if (read_exact(w->log_fd, &type, 1) != 0) {
                break;
            }
            if (skip_bytes(w->log_fd, sizeof(uint64_t)) != 0) {
                break; /* lsn */
            }
            if (type == REC_BEGIN) {
                uint64_t b;
                if (read_exact(w->log_fd, &b, sizeof(b)) != 0) {
                    break;
                }
                have_base = 1;
                base = b;
            } else if (type == REC_UNDO) {
                uint64_t pid;
                uint8_t before[PAGE_SIZE];
                if (read_exact(w->log_fd, &pid, sizeof(pid)) != 0 ||
                    read_exact(w->log_fd, before, PAGE_SIZE) != 0) {
                    break;
                }
                if (pager_write(&w->data, pid, before) != 0) {
                    rc = -1;
                }
            } else if (type == REC_PAGE) {
                /* loser의 after-image는 디스크에 적용된 적 없다(적용은 마커 뒤) -> 무시. */
                if (skip_bytes(w->log_fd, sizeof(uint64_t) + PAGE_SIZE) != 0) {
                    break;
                }
            } else {
                break;
            }
        }
        if (have_base && base < w->data.num_pages) {
            pager_truncate(&w->data, base); /* 새로 할당한 페이지 제거 */
        }
    }

    free(redo);
    if (ftruncate(w->log_fd, 0) < 0) {
        rc = -1;
    }
    lseek(w->log_fd, 0, SEEK_SET);
    fsync(w->data.fd);
    return rc;
}

int wal_open(Wal *w, const char *data_path, const char *log_path) {
    if (pager_open(&w->data, data_path) != 0) {
        return -1;
    }
    w->log_fd = open(log_path, O_RDWR | O_CREAT, 0644);
    if (w->log_fd < 0) {
        pager_close(&w->data);
        return -1;
    }
    w->staged = malloc(sizeof(StagedPage) * WAL_MAX_STAGED);
    if (!w->staged) {
        close(w->log_fd);
        pager_close(&w->data);
        return -1;
    }
    w->num_staged = 0;
    w->txn_active = 0;
    w->stole = 0;
    w->base_pages = 0;
    w->spilled = NULL;
    w->num_spilled = 0;
    w->cap_spilled = 0;
    w->next_lsn = 1;
    w->flushed_lsn = 0;
    w->txn_log_start = 0;
    wal_recover(w); /* 열면서 복구 (끝에 로그를 비우므로 = 체크포인트) */
    return 0;
}

void wal_close(Wal *w) {
    /* 깨끗한 종료든 크래시든 fd만 닫는다. 로그는 건드리지 않는다
     * (정상 커밋은 이미 로그를 비웠고, 크래시면 복구를 위해 로그를 남겨둬야 한다). */
    free(w->staged);
    w->staged = NULL;
    free(w->spilled);
    w->spilled = NULL;
    w->num_spilled = 0;
    w->cap_spilled = 0;
    if (w->log_fd >= 0) {
        close(w->log_fd);
        w->log_fd = -1;
    }
    pager_close(&w->data);
}

int wal_read(Wal *w, page_id_t page_id, void *buf) {
    return pager_read(&w->data, page_id, buf);
}

void wal_begin(Wal *w) {
    w->num_staged = 0;
    w->txn_active = 1;
    w->stole = 0;
    w->base_pages = w->data.num_pages; /* undo 시 여기로 truncate */
    w->num_spilled = 0;                /* spilled 목록 리셋 (버퍼는 재사용) */
    /* 로그엔 앞선 커밋들의 이력이 남아 있다(no-force). 내 트랜잭션의 기록은
     * 여기부터 시작한다 — 롤백(undo)이 이 오프셋부터만 훑는다. */
    w->txn_log_start = lseek(w->log_fd, 0, SEEK_END);
}

int wal_stage(Wal *w, page_id_t page_id, const void *buf) {
    StagedPage *s = w->staged;
    for (int i = 0; i < w->num_staged; i++) {
        if (s[i].page_id == page_id) {
            memcpy(s[i].data, buf, PAGE_SIZE);
            return 0;
        }
    }
    if (w->num_staged >= WAL_MAX_STAGED) {
        return -1;
    }
    s[w->num_staged].page_id = page_id;
    memcpy(s[w->num_staged].data, buf, PAGE_SIZE);
    w->num_staged++;
    return 0;
}

int wal_steal(Wal *w, page_id_t page_id, const void *buf) {
    lseek(w->log_fd, 0, SEEK_END);

    if (!w->stole) {
        /* 최초 steal: 트랜잭션 시작 페이지 수를 로그에 남긴다(undo 시 truncate 기준). */
        uint64_t base = w->base_pages;
        if (write_hdr(w, REC_BEGIN) == 0 ||
            write_all(w->log_fd, &base, sizeof(base)) != 0) {
            return -1;
        }
        w->stole = 1;
    }

    if (!spilled_contains(w, page_id)) {
        /* 최초로 이 페이지를 디스크로 내보낸다. base 미만이면 아직 내가 안 건드린
         * 디스크 = 커밋본이므로 before-image로 남긴다. base 이상이면 이번 트랜잭션이
         * 새로 할당한 페이지라 undo=truncate로 없앨 것 -> before-image 불필요. */
        if (page_id < w->base_pages) {
            uint8_t before[PAGE_SIZE];
            if (pager_read(&w->data, page_id, before) != 0) {
                return -1;
            }
            uint64_t pid = page_id;
            if (write_hdr(w, REC_UNDO) == 0 ||
                write_all(w->log_fd, &pid, sizeof(pid)) != 0 ||
                write_all(w->log_fd, before, PAGE_SIZE) != 0) {
                return -1;
            }
        }
        if (spilled_add(w, page_id) != 0) {
            return -1;
        }
    }

    fsync(w->log_fd); /* WAL 규칙: undo가 디스크에 내구된 뒤에야 데이터를 바꾼다 */
    w->flushed_lsn = w->next_lsn - 1;
    if (pager_write(&w->data, page_id, buf) != 0) {
        return -1;
    }
    fsync(w->data.fd); /* stolen 페이지도 내구화 — 커밋 후 크래시 시 redo가 없어도 안전 */
    return 0;
}

int wal_undo(Wal *w) {
    if (!w->stole) {
        return 0; /* steal이 없었으면 로그에 되돌릴 게 없다 */
    }
    /* 로그 앞부분은 앞선 트랜잭션들의 커밋 이력(no-force) — 내 것만 훑는다. */
    if (lseek(w->log_fd, w->txn_log_start, SEEK_SET) < 0) {
        return -1;
    }
    int rc = 0;
    uint64_t base = w->base_pages;
    int have_base = 0;
    for (;;) {
        uint8_t type;
        if (read_exact(w->log_fd, &type, 1) != 0) {
            break;
        }
        if (skip_bytes(w->log_fd, sizeof(uint64_t)) != 0) {
            break; /* lsn */
        }
        if (type == REC_BEGIN) {
            uint64_t b;
            if (read_exact(w->log_fd, &b, sizeof(b)) != 0) {
                break;
            }
            base = b;
            have_base = 1;
        } else if (type == REC_UNDO) {
            uint64_t pid;
            uint8_t before[PAGE_SIZE];
            if (read_exact(w->log_fd, &pid, sizeof(pid)) != 0 ||
                read_exact(w->log_fd, before, PAGE_SIZE) != 0) {
                break;
            }
            if (pager_write(&w->data, pid, before) != 0) {
                rc = -1;
            }
        } else {
            break; /* REC_PAGE/COMMIT 등은 롤백 경로에선 안 나온다 */
        }
    }
    (void)have_base;
    if (base < w->data.num_pages) {
        pager_truncate(&w->data, base); /* 새로 할당한 페이지 제거 */
    }
    fsync(w->data.fd);
    /* 아보트한 트랜잭션의 기록만 잘라낸다 — 앞선 커밋 이력은 보존(진실의 원천). */
    ftruncate(w->log_fd, w->txn_log_start);
    lseek(w->log_fd, 0, SEEK_END);
    w->stole = 0;
    w->num_spilled = 0;
    return rc;
}

int wal_commit(Wal *w) {
    StagedPage *s = w->staged;
    lseek(w->log_fd, 0, SEEK_END);

    /* 1) 바뀐 페이지들(after-image)을 로그에 쓴다 (write-ahead) */
    for (int i = 0; i < w->num_staged; i++) {
        uint64_t pid = s[i].page_id;
        if (write_hdr(w, REC_PAGE) == 0 ||
            write_all(w->log_fd, &pid, sizeof(pid)) != 0 ||
            write_all(w->log_fd, s[i].data, PAGE_SIZE) != 0) {
            return -1;
        }
    }
    if (wal_test_crash_before_commit) {
        fsync(w->log_fd);
        w->flushed_lsn = w->next_lsn - 1;
        return 0; /* 크래시: 커밋 마커 없음 -> 복구 시 undo로 버려짐 */
    }

    /* 2) 커밋 마커 + fsync — 이 줄을 지나면 "내구"하다 */
    if (write_hdr(w, REC_COMMIT) == 0) {
        return -1;
    }
    fsync(w->log_fd);
    w->flushed_lsn = w->next_lsn - 1;
    if (wal_test_crash_after_log) {
        return 0; /* 크래시: 데이터 적용 전 -> 복구가 redo */
    }

    /* 3) 데이터 파일에 반영은 하되 fsync하지 않는다(NO-FORCE) — 내구성은 이미
     *    2)의 로그 fsync가 책임진다. 크래시가 나도 복구의 redo가 로그에서 재적용.
     *    (stolen 페이지는 steal 시점에 이미 디스크에 내구화돼 있다.) */
    for (int i = 0; i < w->num_staged; i++) {
        if (pager_write(&w->data, s[i].page_id, s[i].data) != 0) {
            return -1;
        }
    }

    /* 4) 로그는 자르지 않는다 — 커밋 이력이 곧 진실의 원천. 대신 무한히 크지 않게,
     *    임계를 넘으면 체크포인트: 데이터를 fsync해 로그를 따라잡게 한 뒤 로그를 비운다. */
    if (lseek(w->log_fd, 0, SEEK_END) > (off_t)WAL_CHECKPOINT_BYTES) {
        fsync(w->data.fd);
        ftruncate(w->log_fd, 0);
        lseek(w->log_fd, 0, SEEK_SET);
    }
    w->num_staged = 0;
    w->stole = 0;
    w->num_spilled = 0;
    w->txn_active = 0;
    return 0;
}
