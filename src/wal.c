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

#define REC_PAGE 'P'
#define REC_COMMIT 'C'

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

/* 크래시 복구: 커밋된 페이지는 데이터에 재적용(redo), 커밋 안 된 건 버림. 그리고 로그를 비운다. */
static int wal_recover(Wal *w) {
    if (lseek(w->log_fd, 0, SEEK_SET) < 0) {
        return -1;
    }
    StagedPage *pending = malloc(sizeof(StagedPage) * WAL_MAX_STAGED);
    if (!pending) {
        return -1;
    }
    int np = 0;
    int rc = 0;
    for (;;) {
        uint8_t type;
        int r = read_exact(w->log_fd, &type, 1);
        if (r != 0) {
            break; /* EOF 또는 오류 */
        }
        if (type == REC_PAGE) {
            if (np >= WAL_MAX_STAGED) {
                break;
            }
            uint64_t pid;
            if (read_exact(w->log_fd, &pid, sizeof(pid)) != 0) {
                break; /* 잘린 레코드 -> 이 트랜잭션은 미완 */
            }
            pending[np].page_id = pid;
            if (read_exact(w->log_fd, pending[np].data, PAGE_SIZE) != 0) {
                break;
            }
            np++;
        } else if (type == REC_COMMIT) {
            /* 여기까지가 커밋됨 -> 데이터에 재적용 */
            for (int i = 0; i < np; i++) {
                if (pager_write(&w->data, pending[i].page_id, pending[i].data) != 0) {
                    rc = -1;
                }
            }
            np = 0;
        } else {
            break;
        }
    }
    /* 남은 pending = 커밋 안 됨 -> 버린다 */
    free(pending);
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
    wal_recover(w); /* 열면서 복구 */
    return 0;
}

void wal_close(Wal *w) {
    /* 깨끗한 종료든 크래시든 fd만 닫는다. 로그는 건드리지 않는다
     * (정상 커밋은 이미 로그를 비웠고, 크래시면 복구를 위해 로그를 남겨둬야 한다). */
    free(w->staged);
    w->staged = NULL;
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

int wal_commit(Wal *w) {
    StagedPage *s = w->staged;
    lseek(w->log_fd, 0, SEEK_END);

    /* 1) 바뀐 페이지들을 로그에 쓴다 (write-ahead) */
    for (int i = 0; i < w->num_staged; i++) {
        uint8_t type = REC_PAGE;
        uint64_t pid = s[i].page_id;
        if (write_all(w->log_fd, &type, 1) != 0 ||
            write_all(w->log_fd, &pid, sizeof(pid)) != 0 ||
            write_all(w->log_fd, s[i].data, PAGE_SIZE) != 0) {
            return -1;
        }
    }
    if (wal_test_crash_before_commit) {
        fsync(w->log_fd);
        return 0; /* 크래시: 커밋 마커 없음 -> 복구 시 버려짐 */
    }

    /* 2) 커밋 마커 + fsync — 이 줄을 지나면 "내구"하다 */
    uint8_t c = REC_COMMIT;
    if (write_all(w->log_fd, &c, 1) != 0) {
        return -1;
    }
    fsync(w->log_fd);
    if (wal_test_crash_after_log) {
        return 0; /* 크래시: 데이터 적용 전 -> 복구가 redo */
    }

    /* 3) 데이터 파일에 실제로 적용 */
    for (int i = 0; i < w->num_staged; i++) {
        if (pager_write(&w->data, s[i].page_id, s[i].data) != 0) {
            return -1;
        }
    }
    fsync(w->data.fd);

    /* 4) 로그 비움(체크포인트) */
    ftruncate(w->log_fd, 0);
    lseek(w->log_fd, 0, SEEK_SET);
    w->num_staged = 0;
    return 0;
}
