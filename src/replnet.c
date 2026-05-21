/* replnet.c — WAL 로그 시핑의 네트워크 전송(walsender/walreceiver).
 * 설계·경계는 replnet.h 참고. replica.c는 건드리지 않고, 이 계층은 바이트만 옮긴다. */
#include "replnet.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

/* 부분 write를 다 밀어 넣는다. 0 성공, -1 오류. wal.c write_all과 같은 규약. */
static int write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) {
            return -1; /* peer가 닫았거나 오류 */
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

/* n바이트를 정확히 채워 읽는다. 0 = 다 읽음, 1 = 첫 바이트부터 EOF(peer 닫음),
 * -1 = 오류. wal.c/replica.c read_exact와 같은 규약. 소켓에서도 그대로 성립한다
 * (블로킹 소켓의 read는 상대가 닫으면 0을 준다). */
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
        p += (size_t)r;
        n -= (size_t)r;
    }
    return 0;
}

int walsender_send(int sock, int log_fd, off_t *sent_off) {
    off_t eof = lseek(log_fd, 0, SEEK_END);
    if (eof < 0) {
        return -1;
    }
    if (eof < *sent_off) {
        /* 로그가 *sent_off 아래로 줄었다 = primary가 체크포인트로 truncate했다.
         * 되감아 처음부터 다시 흘려보낸다 — receiver의 replica_apply가 lsn으로
         * 이미 적용한 커밋을 걸러내므로(idempotent) 중복 전송은 안전하다. */
        *sent_off = 0;
    }
    off_t avail = eof - *sent_off;
    if (avail <= 0) {
        return 0; /* 새 바이트 없음 */
    }
    size_t chunk = (avail > (off_t)REPLNET_CHUNK_MAX) ? REPLNET_CHUNK_MAX
                                                      : (size_t)avail;

    uint8_t *buf = malloc(chunk);
    if (!buf) {
        return -1;
    }
    if (lseek(log_fd, *sent_off, SEEK_SET) < 0) {
        free(buf);
        return -1;
    }
    /* 방금 EOF를 재어 chunk만큼은 반드시 파일에 있으므로 read_exact는 0이어야 한다. */
    if (read_exact(log_fd, buf, chunk) != 0) {
        free(buf);
        return -1;
    }

    /* 프레임 = [길이 8B][바이트]. 길이 먼저 보내 수신 측이 경계를 알게 한다. */
    uint64_t len = (uint64_t)chunk;
    if (write_all(sock, &len, sizeof(len)) != 0 ||
        write_all(sock, buf, chunk) != 0) {
        free(buf);
        return -1;
    }
    free(buf);
    *sent_off += (off_t)chunk;
    return (int)chunk;
}

int walreceiver_recv(int sock, int local_log_fd) {
    uint64_t len;
    int rc = read_exact(sock, &len, sizeof(len));
    if (rc == 1) {
        return 0; /* 프레임 경계에서 peer가 닫음 = 깨끗한 EOF */
    }
    if (rc < 0) {
        return -1;
    }
    if (len == 0) {
        return 0; /* 빈 프레임(keepalive) — 우리 sender는 안 보내지만 방어적으로 */
    }
    if (len > REPLNET_CHUNK_MAX) {
        return -1; /* 상한 초과 = 손상/불일치. 무한 malloc 방지. */
    }

    uint8_t *buf = malloc((size_t)len);
    if (!buf) {
        return -1;
    }
    /* 헤더는 왔는데 본문이 잘리면 = 프로토콜 오류(깨끗한 EOF가 아님) -> -1. */
    if (read_exact(sock, buf, (size_t)len) != 0) {
        free(buf);
        return -1;
    }

    /* 로컬 로그 끝에 이어 붙인다. 이 파일이 곧 replica_apply가 tail할 로그다. */
    if (lseek(local_log_fd, 0, SEEK_END) < 0 ||
        write_all(local_log_fd, buf, (size_t)len) != 0) {
        free(buf);
        return -1;
    }
    free(buf);
    /* 받은 WAL을 디스크에 내구화한 뒤 apply하도록 fsync(수신측 crash 견딤). */
    fsync(local_log_fd);
    return (int)len;
}
