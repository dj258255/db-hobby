#include "bufpool.h"
#include "pager.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 트랙 D 1단계 — 버퍼 풀 스레드 안전성.
 * 진짜 pthread로 버퍼 풀을 두들긴다: 프레임보다 페이지가 훨씬 많아 축출이 끊임없이
 * 일어나는 상황에서, 여러 스레드가 동시에 fetch/unpin 한다. latch가 없으면
 * 프레임 메타데이터가 깨져(엉뚱한 페이지 데이터·pin 언더플로) 무결성 검사가 실패한다.
 * (`make test-tsan` 으로 ThreadSanitizer 하에 돌리면 data race 자체가 잡힌다.)
 *
 * 핀 프로토콜의 계약을 검증한다:
 *   A. 축출 폭풍 속에서도 fetch가 '그 페이지의' 데이터를 정확히 돌려준다(교차 오염 없음).
 *   B. 스레드마다 서로 다른 페이지에 쓰면(디스크 flush 포함) 최종 디스크가 정확하다.
 * 버퍼 풀은 프레임 메타데이터만 지킨다 — 같은 페이지 내용에 대한 동시 쓰기 직렬화는
 * 상위 계층(래치)의 몫이므로, 여기선 페이지를 스레드별로 분리해 그 경계를 지킨다.
 */

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } } while (0)

#define NPAGES 300
#define NFRAMES 16   /* 스레드(8)보다 많아 pin 때문에 자리가 없을 일은 없다 */
#define NTHREADS 8
#define ITERS 40000

static uint64_t rd_u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr_u64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* --- A: 읽기 무결성 --- */
typedef struct { BufferPool *bp; int bad; unsigned seed; } ReadArg;
static void *reader(void *ap) {
    ReadArg *a = ap;
    for (int i = 0; i < ITERS; i++) {
        page_id_t pid = (page_id_t)(rand_r(&a->seed) % NPAGES);
        uint8_t *d = bufpool_fetch(a->bp, pid);
        if (!d) { a->bad++; continue; } /* NFRAMES>NTHREADS라 정상적으론 안 일어남 */
        if (rd_u64(d) != pid) a->bad++;  /* 교차 오염 = 축출 레이스 */
        bufpool_unpin(a->bp, pid, 0);
    }
    return NULL;
}

/* --- B: 스레드별 분리 페이지에 동시 쓰기 --- */
typedef struct { BufferPool *bp; int tid; } WriteArg;
static void *writer(void *ap) {
    WriteArg *a = ap;
    for (int i = 0; i < ITERS; i++) {
        page_id_t pid = (page_id_t)(a->tid + (size_t)(i % (NPAGES / NTHREADS)) * NTHREADS);
        if (pid >= NPAGES) continue;
        uint8_t *d = bufpool_fetch(a->bp, pid);
        if (!d) continue;
        wr_u64(d, pid);              /* 스탬프 유지 */
        wr_u64(d + 8, rd_u64(d + 8) + 1); /* 이 페이지는 이 스레드만 쓴다 → 안전 */
        bufpool_unpin(a->bp, pid, 1);
    }
    return NULL;
}

int main(void) {
    const char *path = "build/test_concurrency.db";
    unlink(path);

    Pager pager;
    pager_open(&pager, path);
    BufferPool *bp = bufpool_create(&pager, NFRAMES);

    /* 페이지 NPAGES개를 만들고 각자 page_id로 스탬프 */
    for (int p = 0; p < NPAGES; p++) {
        page_id_t id;
        uint8_t *d = bufpool_new_page(bp, &id);
        wr_u64(d, id);
        wr_u64(d + 8, 0);
        bufpool_unpin(bp, id, 1);
    }
    bufpool_flush_all(bp);

    /* --- A. 8스레드 동시 랜덤 읽기 (축출 폭풍) --- */
    pthread_t th[NTHREADS];
    ReadArg ra[NTHREADS];
    for (int t = 0; t < NTHREADS; t++) {
        ra[t] = (ReadArg){bp, 0, (unsigned)(t * 2654435761u + 1)};
        pthread_create(&th[t], NULL, reader, &ra[t]);
    }
    int read_bad = 0, read_null = 0;
    for (int t = 0; t < NTHREADS; t++) { pthread_join(th[t], NULL); read_bad += ra[t].bad; }
    CHECK(read_bad == 0, "동시 읽기 40000×8: 항상 그 페이지의 데이터 (교차 오염 0)");

    size_t total = bufpool_hits(bp) + bufpool_misses(bp);
    CHECK(total >= (size_t)NTHREADS * ITERS, "hit+miss 카운트가 최소 전체 접근 수 이상 (원자적 갱신)");

    /* pin 누수 검사: 모든 페이지를 한 번씩 다시 fetch할 수 있어야 한다(프레임이 안 잠김) */
    int all_fetch = 1;
    for (int p = 0; p < NPAGES; p++) {
        uint8_t *d = bufpool_fetch(bp, (page_id_t)p);
        if (!d || rd_u64(d) != (uint64_t)p) all_fetch = 0;
        if (d) bufpool_unpin(bp, (page_id_t)p, 0);
    }
    CHECK(all_fetch, "읽기 폭풍 뒤 모든 페이지 재조회 성공 (pin 누수 없음)");
    (void)read_null;

    /* --- B. 8스레드 동시 쓰기 (분리 페이지) → 디스크 무결성 --- */
    WriteArg wa[NTHREADS];
    for (int t = 0; t < NTHREADS; t++) {
        wa[t] = (WriteArg){bp, t};
        pthread_create(&th[t], NULL, writer, &wa[t]);
    }
    for (int t = 0; t < NTHREADS; t++) pthread_join(th[t], NULL);
    bufpool_flush_all(bp);

    /* 디스크에서 직접 읽어 스탬프 확인 (축출·flush가 페이지를 안 섞었나) */
    int disk_ok = 1;
    uint8_t buf[PAGE_SIZE];
    for (int p = 0; p < NPAGES; p++) {
        if (pager_read(&pager, (page_id_t)p, buf) != 0 || rd_u64(buf) != (uint64_t)p) disk_ok = 0;
    }
    CHECK(disk_ok, "동시 쓰기 뒤 디스크의 모든 페이지 스탬프 정확 (축출/flush 무결성)");

    bufpool_destroy(bp);
    pager_close(&pager);
    unlink(path);

    if (failures == 0) { printf("\n전체 통과\n"); return 0; }
    printf("\n%d개 실패\n", failures);
    return 1;
}
