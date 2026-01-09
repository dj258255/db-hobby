#include "pager.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>

int pager_open(Pager *pager, const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }
    pager->fd = fd;
    /* 파일 크기를 페이지 크기로 나누면 페이지 수. (학습용이라 끝의 잔여 바이트는
     * 무시한다. 진짜 DB는 헤더 페이지에 메타데이터를 둔다.) */
    pager->num_pages = (uint64_t)st.st_size / PAGE_SIZE;
    return 0;
}

void pager_close(Pager *pager) {
    if (pager->fd >= 0) {
        close(pager->fd);
        pager->fd = -1;
    }
}

page_id_t pager_allocate(Pager *pager) {
    page_id_t id = pager->num_pages;
    /* 0으로 채운 빈 페이지를 끝에 써서 파일을 한 페이지만큼 확장한다. */
    uint8_t zero[PAGE_SIZE];
    memset(zero, 0, PAGE_SIZE);
    if (pager_write(pager, id, zero) != 0) {
        return PAGE_ID_INVALID;
    }
    pager->num_pages++;
    return id;
}

/*
 * page_id 페이지는 파일에서 offset (page_id * PAGE_SIZE)에 PAGE_SIZE 바이트로 산다.
 * pread/pwrite로 seek 없이 그 offset을 직접 읽고 쓴다. 반환값이 PAGE_SIZE와 다르면
 * (부분 I/O이거나 오류) 실패로 본다 — 학습용 단순화. 실무에선 루프로 마저 처리한다.
 */

int pager_read(Pager *pager, page_id_t page_id, void *buf) {
    off_t offset = (off_t)page_id * PAGE_SIZE; /* 이 페이지가 사는 디스크 위치 */
    ssize_t n = pread(pager->fd, buf, PAGE_SIZE, offset);
    return (n == (ssize_t)PAGE_SIZE) ? 0 : -1;
}

int pager_write(Pager *pager, page_id_t page_id, const void *buf) {
    off_t offset = (off_t)page_id * PAGE_SIZE;
    ssize_t n = pwrite(pager->fd, buf, PAGE_SIZE, offset);
    return (n == (ssize_t)PAGE_SIZE) ? 0 : -1;
}
