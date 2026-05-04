#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
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

static int recv_all(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return -1;
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

/* ------------- 표준 응답 ------------- */

static int send_ready(int fd, char status) {
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
static int send_row_desc(int fd, char **cols, int ncols) {
    Buf b = {0};
    buf_u16(&b, (uint16_t)ncols);
    for (int i = 0; i < ncols; i++) {
        buf_cstr(&b, cols[i]);
        buf_u32(&b, 0); buf_u16(&b, 0); buf_u32(&b, 25);
        buf_u16(&b, 0xffff); buf_u32(&b, 0xffffffff); buf_u16(&b, 0);
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

/* ------------- 실행기 텍스트 -> wire 번역 (19편과 동일) ------------- */

static int split_pipe(char *line, char **out, int max) {
    int n = 0; char *p = line;
    out[n++] = p;
    while (n < max && (p = strstr(p, " | ")) != NULL) { *p = '\0'; p += 3; out[n++] = p; }
    return n;
}
static int is_rowcount_footer(const char *line) {
    if (line[0] != '(') return 0;
    const char *p = line + 1;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    return strncmp(p, "\xed\x96\x89", 3) == 0; /* "행" */
}
static int reply_from_output(int fd, const char *sql, char *out, char txn_status) {
    if (strncmp(out, "ERROR", 5) == 0) {
        char *nl = strchr(out, '\n'); if (nl) *nl = '\0';
        send_error(fd, out + (out[5] == ':' ? 7 : 5));
        return send_ready(fd, 'E');
    }
    char *lines[4200]; int nlines = 0;
    for (char *p = out; *p && nlines < 4200;) {
        lines[nlines++] = p;
        char *nl = strchr(p, '\n'); if (!nl) break; *nl = '\0'; p = nl + 1;
    }
    const char *s = sql; while (*s == ' ' || *s == '\t') s++;
    if (strncasecmp(s, "EXPLAIN", 7) == 0) {
        char *hdr[1] = {(char *)"QUERY PLAN"};
        send_row_desc(fd, hdr, 1);
        for (int i = 0; i < nlines; i++) {
            if (lines[i][0] == '\0') continue;
            char *v[1] = {lines[i]}; send_data_row(fd, v, 1);
        }
        send_cmd_complete(fd, "EXPLAIN");
        return send_ready(fd, txn_status);
    }
    int footer = -1;
    for (int i = nlines - 1; i >= 0; i--) if (is_rowcount_footer(lines[i])) { footer = i; break; }
    if (footer >= 1) {
        char *cols[64];
        int ncols = split_pipe(lines[0], cols, 64);
        send_row_desc(fd, cols, ncols);
        int rows = 0;
        for (int i = 1; i < footer; i++) {
            char *vals[64];
            int nv = split_pipe(lines[i], vals, 64);
            for (int k = nv; k < ncols; k++) vals[k] = (char *)"";
            send_data_row(fd, vals, ncols); rows++;
        }
        char tag[32]; snprintf(tag, sizeof(tag), "SELECT %d", rows);
        send_cmd_complete(fd, tag);
        return send_ready(fd, txn_status);
    }
    send_cmd_complete(fd, nlines ? lines[0] : "OK");
    return send_ready(fd, txn_status);
}

/* ------------- 스레드 서버 -------------
 * 커넥션마다 OS 스레드를 하나 띄운다(진짜 병렬 accept/네트워크 I/O). 실행 자체는
 * 전역 엔진 latch(engine_mtx)로 직렬화한다 — 실행기 전 계층이 아직 스레드 안전하지
 * 않으므로(단일 스레드 가정), 우선 한 개의 굵은 latch로 정확성을 산다. 버퍼 풀은
 * 이미 자체 latch가 있어(트랙 D 1단계) 이 굵은 latch를 계층별로 걷어낼 첫 발판이다.
 */

typedef struct {
    Database *db;
    pthread_mutex_t *engine_mtx;  /* 실행 직렬화 */
    pthread_mutex_t *sess_mtx;    /* 세션 배정 테이블 보호 */
    int *sess_used;
    int fd;
    int session;
} ConnArg;

static void *conn_thread(void *argp) {
    ConnArg *a = argp;
    int fd = a->fd, sess = a->session;
    Database *db = a->db;

    /* startup/SSLRequest 핸드셰이크 (네트워크 I/O만 — 엔진 latch 불필요) */
    int ok = 1;
    for (;;) {
        uint32_t len, code;
        if (recv_u32(fd, &len) != 0 || len < 8 || recv_u32(fd, &code) != 0) { ok = 0; break; }
        uint32_t rest = len - 8; char junk[256];
        while (rest) { uint32_t t = rest > 256 ? 256 : rest; if (recv_all(fd, junk, t) != 0) { ok = 0; break; } rest -= t; }
        if (!ok) break;
        if (code == 80877103 || code == 80877104) { /* SSL/GSS 거절 후 진짜 startup 대기 */
            if (write(fd, "N", 1) != 1) { ok = 0; break; }
            continue;
        }
        /* 진짜 startup */
        Buf b = {0}; buf_u32(&b, 0); send_msg(fd, 'R', &b); buf_free(&b);
        b = (Buf){0}; buf_cstr(&b, "client_encoding"); buf_cstr(&b, "UTF8"); send_msg(fd, 'S', &b); buf_free(&b);
        b = (Buf){0}; buf_cstr(&b, "server_encoding"); buf_cstr(&b, "UTF8"); send_msg(fd, 'S', &b); buf_free(&b);
        b = (Buf){0}; buf_u32(&b, 12345); buf_u32(&b, 67890); send_msg(fd, 'K', &b); buf_free(&b);
        send_ready(fd, 'I');
        break;
    }

    /* 메시지 루프 */
    while (ok) {
        uint8_t type;
        if (recv_all(fd, &type, 1) != 0) break;
        uint32_t len;
        if (recv_u32(fd, &len) != 0 || len < 4) break;
        uint32_t plen = len - 4;
        char *payload = plen ? malloc(plen + 1) : calloc(1, 1);
        if (!payload || (plen && recv_all(fd, payload, plen) != 0)) { free(payload); break; }
        if (plen) payload[plen] = '\0';

        if (type == 'X') { free(payload); break; }           /* Terminate */
        if (type == 'Q') {                                    /* Simple Query */
            char *obuf = NULL; size_t olen = 0;
            FILE *of = open_memstream(&obuf, &olen);
            char st;
            /* --- 엔진 임계구역: 실행은 한 번에 하나 --- */
            pthread_mutex_lock(a->engine_mtx);
            db->cur_session = sess;                           /* 이 커넥션의 세션 */
            db_exec(db, payload, of);
            st = db->sessions[sess].in_txn ? 'T' : 'I';
            pthread_mutex_unlock(a->engine_mtx);
            fclose(of);
            if (reply_from_output(fd, payload, obuf, st) != 0) { free(obuf); free(payload); break; }
            free(obuf);
        } else {
            send_error(fd, "이 서버는 simple query(Q)만 지원합니다");
            pthread_mutex_lock(a->engine_mtx);
            char st = db->sessions[sess].in_txn ? 'T' : 'I';
            pthread_mutex_unlock(a->engine_mtx);
            send_ready(fd, st);
        }
        free(payload);
    }

    /* 커넥션 종료 = 열린 트랜잭션 롤백 (커넥션 끊김 = abort) */
    pthread_mutex_lock(a->engine_mtx);
    if (db->sessions[sess].in_txn) {
        db->cur_session = sess;
        FILE *nf = fopen("/dev/null", "w");
        if (nf) { db_exec(db, "ROLLBACK", nf); fclose(nf); }
    }
    pthread_mutex_unlock(a->engine_mtx);

    close(fd);
    pthread_mutex_lock(a->sess_mtx);
    a->sess_used[sess] = 0;
    pthread_mutex_unlock(a->sess_mtx);
    free(a);
    return NULL;
}

int server_run(Database *db, int port) {
    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return -1;
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(lfd); return -1; }
    if (listen(lfd, 16) != 0) { close(lfd); return -1; }

    fprintf(stderr, "db-hobby: 127.0.0.1:%d 에서 대기 중 (커넥션당 스레드) — "
                    "psql \"host=127.0.0.1 port=%d dbname=%s\"\n", port, port, "db-hobby");

    static pthread_mutex_t engine_mtx = PTHREAD_MUTEX_INITIALIZER;
    static pthread_mutex_t sess_mtx = PTHREAD_MUTEX_INITIALIZER;
    int sess_used[DB_MAX_SESSIONS] = {0};

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; break; }

        /* 세션 배정 */
        int sess = -1;
        pthread_mutex_lock(&sess_mtx);
        for (int i = 0; i < DB_MAX_SESSIONS; i++) if (!sess_used[i]) { sess = i; sess_used[i] = 1; break; }
        pthread_mutex_unlock(&sess_mtx);
        if (sess < 0) { close(cfd); continue; } /* 세션 소진 */

        ConnArg *a = malloc(sizeof(*a));
        a->db = db; a->engine_mtx = &engine_mtx; a->sess_mtx = &sess_mtx;
        a->sess_used = sess_used; a->fd = cfd; a->session = sess;
        pthread_t th;
        if (pthread_create(&th, NULL, conn_thread, a) != 0) {
            close(cfd);
            pthread_mutex_lock(&sess_mtx); sess_used[sess] = 0; pthread_mutex_unlock(&sess_mtx);
            free(a);
        } else {
            pthread_detach(th);
        }
    }
    close(lfd);
    return 0;
}
