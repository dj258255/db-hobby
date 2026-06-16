/* parscan.c — 병렬 풀 스캔 (독립 모듈). 헤더 주석에 설계·안전성·경계 설명. */
#include "parscan.h"

#include "bufpool.h"
#include "page.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ── 결과 버퍼(워커별 지역) ─────────────────────────────────────────── */
static int result_push(ParscanResult *r, RID rid) {
    if (r->n == r->cap) {
        int64_t nc = r->cap ? r->cap * 2 : 64;
        RID *nr = realloc(r->rids, (size_t)nc * sizeof(RID));
        if (!nr) return -1;
        r->rids = nr;
        r->cap = nc;
    }
    r->rids[r->n++] = rid;
    return 0;
}

void parscan_result_free(ParscanResult *r) {
    if (!r) return;
    free(r->rids);
    r->rids = NULL;
    r->n = r->cap = 0;
}

/* ── 워커 ────────────────────────────────────────────────────────────
 * 각 워커는 [lo,hi) 페이지 범위를 자기만의 지역 결과(res)에 모은다 — 워커 간
 * 공유 가변 상태가 없으므로 락이 필요 없다(버퍼 풀 자체 latch만으로 안전). */
typedef struct {
    Heap            *h;
    page_id_t        lo, hi; /* [lo, hi) */
    parscan_pred_fn  pred;
    void            *ctx;
    ParscanResult    res;    /* 지역 결과 */
    int              rc;     /* 0=성공, -1=실패 */
} Worker;

static void *worker_main(void *arg) {
    Worker *w = arg;
    w->rc = 0;
    for (page_id_t pid = w->lo; pid < w->hi; pid++) {
        void *page = bufpool_fetch(w->h->bp, pid); /* latch 아래 pin */
        if (!page) { w->rc = -1; return NULL; }
        uint16_t n = slotpage_num_slots(page);
        for (uint16_t s = 0; s < n; s++) {
            const void *rec;
            uint16_t len;
            if (slotpage_get(page, s, &rec, &len) == 0) { /* 삭제 슬롯 스킵 */
                RID rid = {pid, s};
                /* 페이지가 pin된 동안만 rec가 유효 — 술어를 인라인 평가한다. */
                if (!w->pred || w->pred(rid, rec, len, w->ctx)) {
                    if (result_push(&w->res, rid) != 0) {
                        bufpool_unpin(w->h->bp, pid, 0);
                        w->rc = -1;
                        return NULL;
                    }
                }
            }
        }
        bufpool_unpin(w->h->bp, pid, 0);
    }
    return NULL;
}

int parscan_collect(Heap *h, int nworkers, parscan_pred_fn pred, void *ctx,
                    ParscanResult *out) {
    if (!h || !out) return -1;
    out->rids = NULL;
    out->n = out->cap = 0;
    if (nworkers < 1) nworkers = 1;

    uint64_t np = h->pager->num_pages;
    page_id_t first = h->first_page;
    uint64_t total = (np > first) ? (np - first) : 0;
    if (total == 0) return 0; /* 빈 테이블 */

    /* 워커 수는 페이지 수를 넘지 않게(빈 워커 방지) */
    if ((uint64_t)nworkers > total) nworkers = (int)total;

    Worker *ws = calloc((size_t)nworkers, sizeof(Worker));
    pthread_t *th = calloc((size_t)nworkers, sizeof(pthread_t));
    if (!ws || !th) { free(ws); free(th); return -1; }

    /* 연속 블록 분배: 워커 순서 = 페이지 순서(merge가 직렬 스캔과 같은 순서). */
    uint64_t chunk = (total + (uint64_t)nworkers - 1) / (uint64_t)nworkers;
    for (int i = 0; i < nworkers; i++) {
        ws[i].h = h;
        ws[i].pred = pred;
        ws[i].ctx = ctx;
        ws[i].lo = (page_id_t)(first + (uint64_t)i * chunk);
        uint64_t hi = first + (uint64_t)(i + 1) * chunk;
        if (hi > np) hi = np;
        ws[i].hi = (page_id_t)hi;
        if (ws[i].lo > ws[i].hi) ws[i].lo = ws[i].hi;
    }

    /* 스레드 생성 실패 시, 그때까지 만든 것만 join하고 실패 처리. */
    int spawned = 0, rc = 0;
    for (int i = 0; i < nworkers; i++) {
        if (pthread_create(&th[i], NULL, worker_main, &ws[i]) != 0) { rc = -1; break; }
        spawned++;
    }
    for (int i = 0; i < spawned; i++) {
        pthread_join(th[i], NULL);
        if (ws[i].rc != 0) rc = -1;
    }

    if (rc == 0) {
        /* 워커 순서(=페이지 순서)로 병합 */
        for (int i = 0; i < nworkers; i++) {
            for (int64_t k = 0; k < ws[i].res.n; k++) {
                if (result_push(out, ws[i].res.rids[k]) != 0) { rc = -1; break; }
            }
            if (rc != 0) break;
        }
    }

    for (int i = 0; i < nworkers; i++) parscan_result_free(&ws[i].res);
    free(ws);
    free(th);
    if (rc != 0) parscan_result_free(out);
    return rc;
}
