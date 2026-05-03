#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ------------- 바이트 조립 (빅엔디안 = 네트워크 바이트 순서) ------------- */

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    int oom;
} Buf;

static void buf_need(Buf *b, size_t extra) {
    if (b->oom) return;
    if (b->len + extra > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + extra) nc *= 2;
        uint8_t *p = realloc(b->buf, nc);
        if (!p) { b->oom = 1; return; }
        b->buf = p; b->cap = nc;
    }
}
static void buf_u8(Buf *b, uint8_t v) { buf_need(b, 1); if (!b->oom) b->buf[b->len++] = v; }
static void buf_u16(Buf *b, uint16_t v) { buf_u8(b, v >> 8); buf_u8(b, v & 0xff); }
static void buf_u32(Buf *b, uint32_t v) {
    buf_u8(b, v >> 24); buf_u8(b, (v >> 16) & 0xff); buf_u8(b, (v >> 8) & 0xff); buf_u8(b, v & 0xff);
}
static void buf_bytes(Buf *b, const void *p, size_t n) { buf_need(b, n); if (!b->oom) { memcpy(b->buf + b->len, p, n); b->len += n; } }
static void buf_cstr(Buf *b, const char *s) { buf_bytes(b, s, strlen(s) + 1); }
static void buf_free(Buf *b) { free(b->buf); b->buf = NULL; b->len = b->cap = 0; }

/* 타입 바이트 + 길이(자기 포함, 타입 제외) 프레임으로 메시지를 소켓에 쓴다. */
static int send_msg(int fd, uint8_t type, Buf *payload) {
    if (payload->oom) return -1;
    uint8_t hdr[5];
    hdr[0] = type;
    uint32_t len = (uint32_t)(payload->len + 4);
    hdr[1] = len >> 24; hdr[2] = (len >> 16) & 0xff; hdr[3] = (len >> 8) & 0xff; hdr[4] = len & 0xff;
    if (write(fd, hdr, 5) != 5) return -1;
    if (payload->len && write(fd, payload->buf, payload->len) != (ssize_t)payload->len) return -1;
    return 0;
}

/* ------------- 소켓에서 정확히 n바이트 읽기 ------------- */
static int recv_all(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return -1;               /* EOF */
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}
static int recv_u32(int fd, uint32_t *out) {
    uint8_t b[4];
    if (recv_all(fd, b, 4) != 0) return -1;
    *out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    return 0;
}

/* ------------- 표준 응답 메시지들 ------------- */

static int send_ready(int fd, char status) { /* 'I' idle, 'T' in-txn, 'E' failed */
    Buf b = {0}; buf_u8(&b, (uint8_t)status);
    int rc = send_msg(fd, 'Z', &b); buf_free(&b); return rc;
}
static int send_error(int fd, const char *msg) {
    Buf b = {0};
    buf_u8(&b, 'S'); buf_cstr(&b, "ERROR");
    buf_u8(&b, 'C'); buf_cstr(&b, "XX000");
    buf_u8(&b, 'M'); buf_cstr(&b, msg);
    buf_u8(&b, 0);
    int rc = send_msg(fd, 'E', &b); buf_free(&b); return rc;
}
static int send_cmd_complete(int fd, const char *tag) {
    Buf b = {0}; buf_cstr(&b, tag);
    int rc = send_msg(fd, 'C', &b); buf_free(&b); return rc;
}

/* RowDescription: 모든 컬럼을 text(typeOID 25)로 기술한다. */
static int send_row_desc(int fd, char **cols, int ncols) {
    Buf b = {0};
    buf_u16(&b, (uint16_t)ncols);
    for (int i = 0; i < ncols; i++) {
        buf_cstr(&b, cols[i]);
        buf_u32(&b, 0);       /* table OID */
        buf_u16(&b, 0);       /* column attr */
        buf_u32(&b, 25);      /* type OID = text */
        buf_u16(&b, 0xffff);  /* type len -1 (가변) */
        buf_u32(&b, 0xffffffff); /* typmod -1 */
        buf_u16(&b, 0);       /* format text */
    }
    int rc = send_msg(fd, 'T', &b); buf_free(&b); return rc;
}
static int send_data_row(int fd, char **vals, int ncols) {
    Buf b = {0};
    buf_u16(&b, (uint16_t)ncols);
    for (int i = 0; i < ncols; i++) {
        size_t n = strlen(vals[i]);
        buf_u32(&b, (uint32_t)n);
        buf_bytes(&b, vals[i], n);
    }
    int rc = send_msg(fd, 'D', &b); buf_free(&b); return rc;
}

/* ------------- 실행기 텍스트 출력 -> wire 응답 ------------- */

/* " | "로 나눠 최대 max개 컬럼 포인터를 in-place로 만든다. 반환 = 컬럼 수. */
static int split_pipe(char *line, char **out, int max) {
    int n = 0;
    char *p = line;
    out[n++] = p;
    while (n < max && (p = strstr(p, " | ")) != NULL) {
        *p = '\0';
        p += 3;
        out[n++] = p;
    }
    return n;
}

/* "(<숫자>행" 으로 시작하는 SELECT 결과 꼬리 줄인가? */
static int is_rowcount_footer(const char *line) {
    if (line[0] != '(') return 0;
    const char *p = line + 1;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    return strncmp(p, "\xed\x96\x89", 3) == 0; /* "행" (UTF-8) */
}

/* db_exec의 텍스트 출력을 psql이 이해하는 메시지로 번역한다. */
static int reply_from_output(int fd, const char *sql, char *out, char txn_status) {
    /* 1) 에러 */
    if (strncmp(out, "ERROR", 5) == 0) {
        char *nl = strchr(out, '\n'); if (nl) *nl = '\0';
        send_error(fd, out + (out[5] == ':' ? 7 : 5));
        return send_ready(fd, 'E');
    }

    /* 줄 배열로 쪼갠다 */
    char *lines[4200];
    int nlines = 0;
    for (char *p = out; *p && nlines < 4200;) {
        lines[nlines++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = '\0';
        p = nl + 1;
    }

    /* 2) EXPLAIN: 한 컬럼 "QUERY PLAN"의 여러 행으로 (진짜 PG처럼) */
    const char *s = sql;
    while (*s == ' ' || *s == '\t') s++;
    if (strncasecmp(s, "EXPLAIN", 7) == 0) {
        char *hdr[1] = {(char *)"QUERY PLAN"};
        send_row_desc(fd, hdr, 1);
        for (int i = 0; i < nlines; i++) {
            if (lines[i][0] == '\0') continue;
            char *v[1] = {lines[i]};
            send_data_row(fd, v, 1);
        }
        send_cmd_complete(fd, "EXPLAIN");
        return send_ready(fd, txn_status);
    }

    /* 3) SELECT 결과: 마지막에 "(N행..." 꼬리가 있으면 header + rows */
    int footer = -1;
    for (int i = nlines - 1; i >= 0; i--) {
        if (is_rowcount_footer(lines[i])) { footer = i; break; }
    }
    if (footer >= 1) {
        char *cols[64];
        int ncols = split_pipe(lines[0], cols, 64);
        send_row_desc(fd, cols, ncols);
        int rows = 0;
        for (int i = 1; i < footer; i++) {
            char *vals[64];
            int nv = split_pipe(lines[i], vals, 64);
            for (int k = nv; k < ncols; k++) vals[k] = (char *)""; /* 모자라면 빈칸 */
            send_data_row(fd, vals, ncols);
            rows++;
        }
        char tag[32]; snprintf(tag, sizeof(tag), "SELECT %d", rows);
        send_cmd_complete(fd, tag);
        return send_ready(fd, txn_status);
    }

    /* 4) 그 외(INSERT/UPDATE/COMMIT/VACUUM 등 안내 메시지): 첫 줄을 명령 태그로 */
    send_cmd_complete(fd, nlines ? lines[0] : "OK");
    return send_ready(fd, txn_status);
}

/* ------------- 커넥션 상태 ------------- */

typedef struct {
    int fd;
    int session;      /* 이 커넥션에 배정된 세션(= 트랜잭션 핸들) */
    int startup_done;
} Conn;

/* ------------- 서버 루프 ------------- */

int server_run(Database *db, int port) {
    signal(SIGPIPE, SIG_IGN); /* 끊긴 소켓에 write해도 죽지 않게 */

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return -1;
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 만 */
    addr.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(lfd); return -1; }
    if (listen(lfd, 16) != 0) { close(lfd); return -1; }

    fprintf(stderr, "db-hobby: 127.0.0.1:%d 에서 대기 중 — psql \"host=127.0.0.1 port=%d dbname=%s\"\n",
            port, port, "db-hobby");

    Conn conns[DB_MAX_SESSIONS];
    int nconn = 0;
    int sess_used[DB_MAX_SESSIONS] = {0};

    for (;;) {
        struct pollfd pfds[DB_MAX_SESSIONS + 1];
        pfds[0].fd = lfd; pfds[0].events = POLLIN;
        for (int i = 0; i < nconn; i++) { pfds[i + 1].fd = conns[i].fd; pfds[i + 1].events = POLLIN; }
        if (poll(pfds, (nfds_t)(nconn + 1), -1) < 0) { if (errno == EINTR) continue; break; }

        /* 새 커넥션 */
        if (pfds[0].revents & POLLIN) {
            int cfd = accept(lfd, NULL, NULL);
            if (cfd >= 0) {
                int sess = -1;
                for (int i = 0; i < DB_MAX_SESSIONS; i++) if (!sess_used[i]) { sess = i; break; }
                if (sess < 0 || nconn >= DB_MAX_SESSIONS) {
                    close(cfd); /* 세션 소진 */
                } else {
                    sess_used[sess] = 1;
                    conns[nconn].fd = cfd; conns[nconn].session = sess; conns[nconn].startup_done = 0;
                    nconn++;
                }
            }
        }

        /* 기존 커넥션들 */
        for (int i = 0; i < nconn;) {
            if (!(pfds[i + 1].revents & (POLLIN | POLLHUP | POLLERR))) { i++; continue; }
            Conn *c = &conns[i];
            int dead = 0;

            if (!c->startup_done) {
                /* startup/SSLRequest: 타입 바이트 없이 [len][code...] */
                uint32_t len, code;
                if (recv_u32(c->fd, &len) != 0 || len < 8 || recv_u32(c->fd, &code) != 0) {
                    dead = 1;
                } else if (code == 80877103 || code == 80877104) {
                    /* SSL/GSS 요청 -> 거절('N'). 본 startup은 다음에 온다. */
                    uint32_t rest = len - 8;
                    char junk[64];
                    while (rest && !dead) { uint32_t take = rest > 64 ? 64 : rest; if (recv_all(c->fd, junk, take) != 0) dead = 1; rest -= take; }
                    if (!dead && write(c->fd, "N", 1) != 1) dead = 1;
                } else {
                    /* 본 startup: 나머지 파라미터는 읽어서 버린다 */
                    uint32_t rest = len - 8;
                    char junk[256];
                    while (rest && !dead) { uint32_t take = rest > 256 ? 256 : rest; if (recv_all(c->fd, junk, take) != 0) dead = 1; rest -= take; }
                    if (!dead) {
                        Buf b = {0};
                        buf_u32(&b, 0); send_msg(c->fd, 'R', &b); buf_free(&b);       /* AuthenticationOk */
                        b = (Buf){0}; buf_cstr(&b, "client_encoding"); buf_cstr(&b, "UTF8");
                        send_msg(c->fd, 'S', &b); buf_free(&b);                       /* ParameterStatus */
                        b = (Buf){0}; buf_cstr(&b, "server_encoding"); buf_cstr(&b, "UTF8");
                        send_msg(c->fd, 'S', &b); buf_free(&b);
                        b = (Buf){0}; buf_u32(&b, 12345); buf_u32(&b, 67890);
                        send_msg(c->fd, 'K', &b); buf_free(&b);                        /* BackendKeyData */
                        send_ready(c->fd, 'I');
                        c->startup_done = 1;
                    }
                }
            } else {
                /* 정규 메시지: [type][len][payload] */
                uint8_t type;
                if (recv_all(c->fd, &type, 1) != 0) { dead = 1; }
                else {
                    uint32_t len;
                    if (recv_u32(c->fd, &len) != 0 || len < 4) { dead = 1; }
                    else {
                        uint32_t plen = len - 4;
                        char *payload = plen ? malloc(plen + 1) : calloc(1, 1);
                        if (!payload || (plen && recv_all(c->fd, payload, plen) != 0)) { dead = 1; free(payload); }
                        else {
                            if (plen) payload[plen] = '\0';
                            if (type == 'X') { dead = 1; free(payload); }        /* Terminate */
                            else if (type == 'Q') {                              /* Simple Query */
                                db->cur_session = c->session;                    /* 커넥션 = 세션 */
                                char *sqlbuf = payload; /* Q 페이로드 = 쿼리 문자열\0 */
                                size_t olen = 0; char *obuf = NULL;
                                FILE *of = open_memstream(&obuf, &olen);
                                db_exec(db, sqlbuf, of);
                                fclose(of);
                                char st = db->sessions[c->session].in_txn ? 'T' : 'I';
                                if (reply_from_output(c->fd, sqlbuf, obuf, st) != 0) dead = 1;
                                free(obuf);
                                free(payload);
                            } else {
                                /* 미지원 메시지(Parse/Bind 등): 에러 + ReadyForQuery */
                                send_error(c->fd, "이 서버는 simple query(Q)만 지원합니다");
                                send_ready(c->fd, db->sessions[c->session].in_txn ? 'T' : 'I');
                                free(payload);
                            }
                        }
                    }
                }
            }

            if (dead) {
                /* 커넥션 종료 = 열린 트랜잭션 롤백(진짜 DB의 커넥션 끊김과 동일) */
                if (db->sessions[c->session].in_txn) {
                    db->cur_session = c->session;
                    FILE *nf = fopen("/dev/null", "w");
                    if (nf) { db_exec(db, "ROLLBACK", nf); fclose(nf); }
                }
                close(c->fd);
                sess_used[c->session] = 0;
                conns[i] = conns[nconn - 1]; /* 마지막 것으로 메꿔 압축 */
                nconn--;
                /* i 그대로 두고 다음 루프 (교체된 것 검사) — pfds 인덱스는 다음 poll에서 갱신 */
                break; /* revents 인덱스가 어긋나므로 이번 라운드는 여기서 접고 재poll */
            } else {
                i++;
            }
        }
    }
    close(lfd);
    return 0;
}
