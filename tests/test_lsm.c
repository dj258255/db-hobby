#include "lsm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * LSM 저장 엔진 — 독립 모듈 테스트.
 * B+Tree(제자리 갱신)의 쓰기 최적화 대척점. 여기서 검증하는 것:
 *   - 제자리 갱신이 아니라 memtable append + SSTable 순차 flush.
 *   - read path: memtable -> SSTable 최신->오래된(read amplification), 최신이 가림.
 *   - tombstone: 삭제도 새 마커를 쓴다(MVCC DELETE/VACUUM과 같은 지연 삭제).
 *   - compaction: 여러 SSTable을 하나로 merge, 옛 버전·tombstone 청소.
 *   - reopen: flush된 SSTable은 디스크에 남아 재오픈 시 읽힌다.
 */

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } } while (0)

#define DIR "build/test_lsm_data"

static void cleanup(void) { system("rm -rf " DIR); }

/* scan 콜백: 키를 순서대로 배열에 모으고 오름차순인지 검사 */
typedef struct { int64_t keys[256]; int64_t vals[256]; int n; int ordered; } ScanCtx;
static void scan_cb(int64_t key, int64_t val, void *ctx) {
    ScanCtx *c = ctx;
    if (c->n > 0 && key <= c->keys[c->n - 1]) c->ordered = 0;
    if (c->n < 256) { c->keys[c->n] = key; c->vals[c->n] = val; c->n++; }
}

int main(void) {
    cleanup(); /* 시작 시 잔여 파일 제거 */

    int64_t v;

    /* ── 1) 기본 put/get + 없는 키 ─────────────────────────────── */
    LSM *l = lsm_open(DIR, 4); /* memtable 4개 넘으면 flush */
    CHECK(l != NULL, "lsm_open 성공");
    lsm_put(l, 10, 100);
    lsm_put(l, 20, 200);
    CHECK(lsm_get(l, 10, &v) == 1 && v == 100, "put/get: 10 -> 100");
    CHECK(lsm_get(l, 20, &v) == 1 && v == 200, "put/get: 20 -> 200");
    CHECK(lsm_get(l, 99, &v) == 0, "없는 키 조회 -> not-found");

    /* ── 2) 갱신: memtable 안 & flush 경계 넘어 최신이 이긴다 ────── */
    lsm_put(l, 10, 111);
    CHECK(lsm_get(l, 10, &v) == 1 && v == 111, "memtable 내 갱신: 최신 값 111");

    lsm_flush(l);                 /* 10=111 이 SSTable로 내려감 */
    CHECK(lsm_sstable_count(l) == 1, "flush 후 SSTable 1개");
    lsm_put(l, 10, 222);          /* 다시 memtable에 최신 기록 */
    CHECK(lsm_get(l, 10, &v) == 1 && v == 222,
          "flush 경계 넘어 갱신: memtable(222)이 SSTable(111)을 가림");

    /* ── 3) tombstone 삭제: 옛 값이 SSTable에 있어도 삭제로 보인다 ── */
    lsm_put(l, 30, 300);
    lsm_flush(l);                 /* 30=300 이 SSTable에 */
    CHECK(lsm_get(l, 30, &v) == 1 && v == 300, "삭제 전: 30 -> 300 (SSTable에 있음)");
    lsm_delete(l, 30);            /* memtable에 tombstone */
    CHECK(lsm_get(l, 30, &v) == 0, "delete 후: 30 -> not-found (tombstone이 옛 값 가림)");
    lsm_flush(l);                 /* tombstone도 SSTable로 flush */
    CHECK(lsm_get(l, 30, &v) == 0, "flush된 tombstone도 여전히 옛 SSTable 값을 가린다");

    lsm_close(l);

    /* ── 4) flush 폭풍: 임계치로 여러 SSTable 생성, 전부 읽힌다 ──── */
    cleanup();
    l = lsm_open(DIR, 4);
    for (int k = 0; k < 40; k++) lsm_put(l, k, (int64_t)k * 10 + 1);
    CHECK(lsm_sstable_count(l) >= 2, "40개 put(임계치 4) -> SSTable 여러 개 생성됨");
    int miss = 0;
    for (int k = 0; k < 40; k++)
        if (lsm_get(l, k, &v) != 1 || v != (int64_t)k * 10 + 1) miss++;
    CHECK(miss == 0, "read path(memtable + 여러 SSTable): 40키 전부 정확히 조회");

    /* ── 5) compaction: 여러 SSTable을 하나로, 정확성 보존 + 개수 축소 ── */
    /* 갱신·삭제를 섞어 옛 버전/tombstone이 여러 SSTable에 흩어지게 한다 */
    lsm_put(l, 5, 5555);          /* 5 갱신(옛 51은 이전 SSTable에) */
    lsm_delete(l, 7);             /* 7 삭제 */
    lsm_flush(l);
    int before = lsm_sstable_count(l);
    CHECK(before >= 2, "compaction 전 SSTable 여러 개");

    CHECK(lsm_compact(l) == 0, "compact 성공");
    int after = lsm_sstable_count(l);
    CHECK(after == 1, "compaction 후 SSTable 1개로 축소");
    CHECK(after < before, "SSTable 개수가 줄었다");

    /* 정확성: 살아있는 키 읽힘 / 갱신된 최신값 / 삭제된 키 사라짐 */
    CHECK(lsm_get(l, 5, &v) == 1 && v == 5555, "compaction 후 5 -> 최신값 5555");
    CHECK(lsm_get(l, 7, &v) == 0, "compaction 후 7 -> not-found (삭제 유지)");
    int miss2 = 0;
    for (int k = 0; k < 40; k++) {
        if (k == 7) { if (lsm_get(l, k, &v) != 0) miss2++; continue; }
        int64_t want = (k == 5) ? 5555 : (int64_t)k * 10 + 1;
        if (lsm_get(l, k, &v) != 1 || v != want) miss2++;
    }
    CHECK(miss2 == 0, "compaction 후 39개 라이브 키 정확 + 삭제키 gone (tombstone 청소)");

    /* ── 7) 정렬 스캔: 정렬된 run들을 merge, 삭제키 제외, 오름차순 ── */
    ScanCtx sc = {.n = 0, .ordered = 1};
    int64_t scanned = lsm_scan(l, 0, 39, scan_cb, &sc);
    CHECK(scanned == 39, "scan [0,39]: 라이브 키 39개(삭제된 7 제외)");
    CHECK(sc.ordered == 1, "scan 결과가 키 오름차순(sorted run merge)");
    CHECK(sc.n == 39 && sc.keys[0] == 0 && sc.keys[38] == 39, "scan 경계: 0..39, 7 빠짐");

    lsm_close(l);

    /* ── 6) reopen: 닫고 같은 파일 위에 다시 열어도 flush된 데이터 생존 ── */
    l = lsm_open(DIR, 4);
    CHECK(l != NULL, "reopen 성공");
    CHECK(lsm_sstable_count(l) == 1, "reopen: 디스크의 SSTable 발견(1개)");
    int miss3 = 0;
    for (int k = 0; k < 40; k++) {
        if (k == 7) { if (lsm_get(l, k, &v) != 0) miss3++; continue; }
        int64_t want = (k == 5) ? 5555 : (int64_t)k * 10 + 1;
        if (lsm_get(l, k, &v) != 1 || v != want) miss3++;
    }
    CHECK(miss3 == 0, "reopen 후 flush된 데이터 전부 생존(persistence)");
    /* reopen 후에도 새 쓰기가 가능하고 최신이 이긴다 */
    lsm_put(l, 5, 999999);
    CHECK(lsm_get(l, 5, &v) == 1 && v == 999999, "reopen 후 새 put이 옛 SSTable 값을 가림");
    lsm_close(l);

    /* ── 8) 멀티값(비유니크) 모드: 한 key에 여러 val ─────────────────────
     * DB의 다중버전 PK 인덱스(한 PK -> 여러 행 버전 RID)가 요구하는 모드.
     * dedup 단위가 key가 아니라 (key,val)이라, 같은 key의 서로 다른 val이 공존하고
     * delete_val이 특정 짝만 지운다. */
    system("rm -rf " DIR "_m");
    LSM *m = lsm_open_multi(DIR "_m", 2, 1); /* threshold 2: flush 자주 유발 */
    CHECK(m != NULL, "multi lsm_open_multi 성공");

    /* 한 key(1)에 세 개의 val을 append — 서로 가리지 않는다 */
    lsm_put_dup(m, 1, 100);
    lsm_put_dup(m, 1, 101);
    lsm_put_dup(m, 1, 102); /* threshold 2 넘겨 flush 유발 -> memtable+SSTable 혼재 */
    lsm_put_dup(m, 2, 200);

    ScanCtx f1 = {.n = 0, .ordered = 1};
    int64_t c1 = lsm_find_all(m, 1, scan_cb, &f1);
    CHECK(c1 == 3 && f1.n == 3, "find_all(1): 같은 key의 세 val 모두 반환(3개)");
    CHECK(f1.vals[0] == 100 && f1.vals[1] == 101 && f1.vals[2] == 102,
          "find_all(1): val 오름차순 {100,101,102} — memtable/SSTable 경계 넘어 dedup by (key,val)");

    /* 특정 (key,val) 짝만 tombstone */
    lsm_delete_val(m, 1, 101);
    ScanCtx f2 = {.n = 0, .ordered = 1};
    int64_t c2 = lsm_find_all(m, 1, scan_cb, &f2);
    CHECK(c2 == 2 && f2.vals[0] == 100 && f2.vals[1] == 102,
          "delete_val(1,101): 그 짝만 삭제 -> {100,102} (다른 짝 살아있음)");

    /* tombstone된 짝을 다시 put_dup -> 되살아난다(최신이 이긴다) */
    lsm_put_dup(m, 1, 101);
    ScanCtx f3 = {.n = 0, .ordered = 1};
    int64_t c3 = lsm_find_all(m, 1, scan_cb, &f3);
    CHECK(c3 == 3, "re-put_dup(1,101): tombstone 위에 최신 put -> 부활(3개)");

    /* 멀티 모드 range scan: 살아있는 모든 (key,val) 방문, key 오름차순 */
    ScanCtx rs = {.n = 0, .ordered = 1};
    int64_t rc = lsm_scan(m, 0, 10, scan_cb, &rs);
    CHECK(rc == 4 && rs.n == 4, "multi scan[0,10]: 살아있는 짝 4개 (1->3개 + 2->1개)");
    CHECK(rs.keys[0] == 1 && rs.keys[3] == 2, "multi scan: key 오름차순 유지");

    /* compaction 후에도 멀티값 dedup·tombstone 청소가 (key,val) 단위로 정확 */
    lsm_flush(m);
    lsm_compact(m);
    ScanCtx f4 = {.n = 0, .ordered = 1};
    int64_t c4 = lsm_find_all(m, 1, scan_cb, &f4);
    CHECK(c4 == 3 && f4.vals[0] == 100 && f4.vals[1] == 101 && f4.vals[2] == 102,
          "compaction 후에도 (key,val) 단위 라이브 유지: {100,101,102}");

    /* lsm_clear: 재구축 직전 전체 리셋 */
    lsm_clear(m);
    CHECK(lsm_sstable_count(m) == 0, "lsm_clear: SSTable 전부 제거");
    CHECK(lsm_find_all(m, 1, scan_cb, &f4) == 0, "lsm_clear 후 빈 상태");
    lsm_close(m);
    system("rm -rf " DIR "_m");

    cleanup(); /* 끝나면 임시 파일 제거 */

    if (failures == 0) { printf("\n모든 테스트 통과\n"); return 0; }
    printf("\n%d개 실패\n", failures);
    return failures ? 1 : 0;
}
