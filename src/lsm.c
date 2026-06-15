/* lsm.c — Log-Structured Merge 저장 엔진 (독립 모듈)
 *
 * ── 정직한 경계 ─────────────────────────────────────────────────────
 * db.c의 저장 계층(heap.c / btree.c / clustered)에 배선돼 있지 않다. SQL 계층
 * 뒤에 LSM을 "대체 테이블 저장 엔진"으로 꽂으려면 다음이 더 필요하고, 그게
 * 프론티어다:
 *   1) 스키마·다중 컬럼 값: 지금은 (int64 key -> int64 val) 한 쌍뿐.
 *   2) memtable WAL: 지금은 memtable이 volatile(close/crash 시 flush 안 된 건
 *      날아감). 진짜 LSM은 memtable 쓰기를 먼저 WAL에 append해 복구한다
 *      — 이 프로젝트가 트랙 E(WAL/no-force)에서 이미 만든 그 기법을 재사용하면 됨.
 *   3) MVCC/스냅샷 연동: 지금 read path는 "최신이 이긴다"만 안다. 18편의 스냅샷
 *      가시성(txid 기준)과 합치려면 버전마다 xmin/xmax가 붙어야 한다.
 * 그래서 독립 모듈 + 자체 테스트로 서고, 블로그가 'B+Tree vs LSM'을 설명한다.
 * ────────────────────────────────────────────────────────────────────
 *
 * ── 무엇을 단순화했나 ───────────────────────────────────────────────
 *   - memtable: skiplist 대신 "정렬 동적 배열 + 이진탐색". 삽입이 O(n) shift라
 *     느리지만 정확성이 자명하고 flush가 이미 정렬돼 공짜다.
 *   - compaction: leveled(L0/L1/...) 티어 없이 "전체를 하나로" full compaction만.
 *   - Bloom filter 없음: 없는 키도 매 SSTable을 이진탐색한다(read amp 그대로 노출).
 *     대신 SSTable마다 [min_key,max_key]로 범위 밖이면 통째로 건너뛴다(RocksDB의
 *     per-file key range 프루닝의 축소판).
 *   - 단일 스레드: compaction이 background 스레드가 아니라 동기 호출.
 *   - 값은 int64 고정, 파일은 호스트 바이트오더로 그냥 fwrite(이식성 미고려).
 * ────────────────────────────────────────────────────────────────────
 */
#include "lsm.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── 인메모리 엔트리 ─────────────────────────────────────────────── */
typedef struct {
    int64_t key;
    int64_t val;
    int     tomb; /* 1이면 tombstone(삭제 마커) */
} Entry;

/* ── SSTable 온디스크 포맷 ───────────────────────────────────────────
 * [헤더 16B] uint64 magic, uint64 count
 * [레코드 count개] 각 24B: int64 key, int64 val, int64 tomb  (키 오름차순)
 * 고정 크기 레코드라 오프셋 = 16 + i*24. 이진탐색을 fseek로 바로 한다. */
#define SST_MAGIC     0x314D534C6D696E64ULL /* 아무 magic */
#define SST_HDR_BYTES 16
#define SST_REC_BYTES 24

typedef struct {
    uint64_t seq;         /* 생성 순번. 클수록 최신(옛것을 가린다). */
    char     path[1024];
    FILE    *fp;          /* 열린 채 유지(get마다 fseek) */
    uint64_t count;
    int64_t  min_key, max_key; /* 범위 프루닝용 */
} SSTable;

struct LSM {
    char    dir[900];
    size_t  threshold;
    int     multi;        /* 0=유니크(dedup 단위 key), 1=멀티값(dedup 단위 (key,val)) */

    Entry  *mem;          /* 정렬 동적 배열 memtable. 유니크는 key순, 멀티는 (key,val)순 */
    size_t  mem_n, mem_cap;

    SSTable **ssts;       /* seq 오름차순(뒤가 최신). get은 뒤에서 앞으로 훑는다. */
    size_t  nsst, sst_cap;

    uint64_t next_seq;
};

/* ── memtable: 정렬 배열 + 이진탐색 ─────────────────────────────────── */

/* key가 있으면 그 인덱스, 없으면 삽입될 위치를 lo로 돌려준다. found=1/0. */
static size_t mem_bsearch(const LSM *l, int64_t key, int *found) {
    size_t lo = 0, hi = l->mem_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (l->mem[mid].key < key) lo = mid + 1;
        else if (l->mem[mid].key > key) hi = mid;
        else { *found = 1; return mid; }
    }
    *found = 0;
    return lo;
}

/* memtable에 최신 버전 기록(있으면 제자리 치환, 없으면 정렬 위치에 삽입). */
static int mem_upsert(LSM *l, int64_t key, int64_t val, int tomb) {
    int found;
    size_t pos = mem_bsearch(l, key, &found);
    if (found) {
        l->mem[pos].val = val;
        l->mem[pos].tomb = tomb;
        return 0;
    }
    if (l->mem_n == l->mem_cap) {
        size_t nc = l->mem_cap ? l->mem_cap * 2 : 16;
        Entry *ne = realloc(l->mem, nc * sizeof(Entry));
        if (!ne) return -1;
        l->mem = ne;
        l->mem_cap = nc;
    }
    memmove(&l->mem[pos + 1], &l->mem[pos], (l->mem_n - pos) * sizeof(Entry));
    l->mem[pos] = (Entry){key, val, tomb};
    l->mem_n++;
    return 0;
}

/* [멀티] (key,val) 짝으로 이진탐색. 있으면 그 인덱스, 없으면 삽입 위치를 lo로. */
static size_t mem_bsearch2(const LSM *l, int64_t key, int64_t val, int *found) {
    size_t lo = 0, hi = l->mem_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int64_t k = l->mem[mid].key, v = l->mem[mid].val;
        if (k < key || (k == key && v < val)) lo = mid + 1;
        else if (k > key || (k == key && v > val)) hi = mid;
        else { *found = 1; return mid; }
    }
    *found = 0;
    return lo;
}

/* [멀티] (key,val) 짝 기록. 같은 짝이 있으면 tombstone 비트만 갱신, 없으면 정렬
 * 위치((key,val)순)에 삽입. 유니크 mem_upsert와 달리 다른 val을 덮지 않는다. */
static int mem_upsert_dup(LSM *l, int64_t key, int64_t val, int tomb) {
    int found;
    size_t pos = mem_bsearch2(l, key, val, &found);
    if (found) {
        l->mem[pos].tomb = tomb;
        return 0;
    }
    if (l->mem_n == l->mem_cap) {
        size_t nc = l->mem_cap ? l->mem_cap * 2 : 16;
        Entry *ne = realloc(l->mem, nc * sizeof(Entry));
        if (!ne) return -1;
        l->mem = ne;
        l->mem_cap = nc;
    }
    memmove(&l->mem[pos + 1], &l->mem[pos], (l->mem_n - pos) * sizeof(Entry));
    l->mem[pos] = (Entry){key, val, tomb};
    l->mem_n++;
    return 0;
}

/* ── SSTable I/O ─────────────────────────────────────────────────── */

static void sst_path(const LSM *l, uint64_t seq, char *out, size_t cap) {
    snprintf(out, cap, "%s/%06llu.sst", l->dir, (unsigned long long)seq);
}

/* 정렬된 엔트리 배열을 새 SSTable 파일로 쓴다(순차 쓰기 한 방). */
static int sst_write_file(const char *path, const Entry *es, size_t n) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    uint64_t magic = SST_MAGIC, count = n;
    if (fwrite(&magic, 8, 1, fp) != 1 || fwrite(&count, 8, 1, fp) != 1) goto err;
    for (size_t i = 0; i < n; i++) {
        int64_t rec[3] = {es[i].key, es[i].val, es[i].tomb};
        if (fwrite(rec, 8, 3, fp) != 3) goto err;
    }
    if (fflush(fp) != 0) goto err;
    fclose(fp);
    return 0;
err:
    fclose(fp);
    return -1;
}

/* 파일을 열어 SSTable 핸들을 만든다(count/min/max 채움). */
static SSTable *sst_open(uint64_t seq, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    uint64_t magic = 0, count = 0;
    if (fread(&magic, 8, 1, fp) != 1 || fread(&count, 8, 1, fp) != 1 ||
        magic != SST_MAGIC) {
        fclose(fp);
        return NULL;
    }
    SSTable *s = calloc(1, sizeof(SSTable));
    if (!s) { fclose(fp); return NULL; }
    s->seq = seq;
    s->fp = fp;
    s->count = count;
    snprintf(s->path, sizeof(s->path), "%s", path);
    if (count > 0) {
        int64_t first[3], last[3];
        fseek(fp, SST_HDR_BYTES, SEEK_SET);
        if (fread(first, 8, 3, fp) != 3) { fclose(fp); free(s); return NULL; }
        fseek(fp, SST_HDR_BYTES + (long)(count - 1) * SST_REC_BYTES, SEEK_SET);
        if (fread(last, 8, 3, fp) != 3) { fclose(fp); free(s); return NULL; }
        s->min_key = first[0];
        s->max_key = last[0];
    }
    return s;
}

/* i번째 레코드를 읽는다. */
static void sst_read_rec(SSTable *s, uint64_t i, int64_t *key, int64_t *val, int *tomb) {
    int64_t rec[3];
    fseek(s->fp, SST_HDR_BYTES + (long)i * SST_REC_BYTES, SEEK_SET);
    if (fread(rec, 8, 3, s->fp) != 3) { *key = 0; *val = 0; *tomb = 1; return; }
    *key = rec[0];
    *val = rec[1];
    *tomb = (int)rec[2];
}

/* 한 SSTable에서 key를 이진탐색. 찾으면 val/tomb를 채우고 1. */
static int sst_get(SSTable *s, int64_t key, int64_t *val, int *tomb) {
    if (s->count == 0 || key < s->min_key || key > s->max_key) return 0; /* 범위 프루닝 */
    long lo = 0, hi = (long)s->count - 1;
    while (lo <= hi) {
        long mid = lo + (hi - lo) / 2;
        int64_t k, v; int t;
        sst_read_rec(s, (uint64_t)mid, &k, &v, &t);
        if (k == key) { *val = v; *tomb = t; return 1; }
        if (k < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

static void sst_close(SSTable *s) {
    if (!s) return;
    if (s->fp) fclose(s->fp);
    free(s);
}

/* LSM의 SSTable 목록에 추가(seq 오름차순 유지). */
static int lsm_add_sst(LSM *l, SSTable *s) {
    if (l->nsst == l->sst_cap) {
        size_t nc = l->sst_cap ? l->sst_cap * 2 : 8;
        SSTable **ns = realloc(l->ssts, nc * sizeof(SSTable *));
        if (!ns) return -1;
        l->ssts = ns;
        l->sst_cap = nc;
    }
    /* seq는 항상 증가하므로 뒤에 붙이면 정렬 유지 */
    l->ssts[l->nsst++] = s;
    return 0;
}

/* ── 공개 API ────────────────────────────────────────────────────── */

LSM *lsm_open(const char *dir, size_t memtable_threshold) {
    return lsm_open_multi(dir, memtable_threshold, 0);
}

LSM *lsm_open_multi(const char *dir, size_t memtable_threshold, int multi) {
    if (!dir || memtable_threshold == 0) return NULL;
    LSM *l = calloc(1, sizeof(LSM));
    if (!l) return NULL;
    snprintf(l->dir, sizeof(l->dir), "%s", dir);
    l->threshold = memtable_threshold;
    l->multi = multi;
    l->next_seq = 1;

    mkdir(dir, 0777); /* 있으면 EEXIST 무시 */

    /* 기존 *.sst 파일 발견 -> reopen/persistence */
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            unsigned long long seq;
            char tail[8];
            /* "NNNNNN.sst" 만 매칭 */
            if (sscanf(de->d_name, "%llu.%3s", &seq, tail) == 2 &&
                strcmp(tail, "sst") == 0) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
                SSTable *s = sst_open((uint64_t)seq, path);
                if (s) {
                    lsm_add_sst(l, s);
                    if (seq >= l->next_seq) l->next_seq = seq + 1;
                }
            }
        }
        closedir(d);
    }

    /* 발견한 SSTable을 seq 오름차순으로 정렬(readdir 순서는 임의) */
    for (size_t i = 0; i + 1 < l->nsst; i++)
        for (size_t j = i + 1; j < l->nsst; j++)
            if (l->ssts[j]->seq < l->ssts[i]->seq) {
                SSTable *t = l->ssts[i]; l->ssts[i] = l->ssts[j]; l->ssts[j] = t;
            }
    return l;
}

void lsm_close(LSM *l) {
    if (!l) return;
    for (size_t i = 0; i < l->nsst; i++) sst_close(l->ssts[i]);
    free(l->ssts);
    free(l->mem);
    free(l);
}

int lsm_flush(LSM *l) {
    if (!l) return -1;
    if (l->mem_n == 0) return 0; /* no-op */
    uint64_t seq = l->next_seq++;
    char path[1024];
    sst_path(l, seq, path, sizeof(path));
    if (sst_write_file(path, l->mem, l->mem_n) != 0) return -1;
    SSTable *s = sst_open(seq, path);
    if (!s) return -1;
    if (lsm_add_sst(l, s) != 0) { sst_close(s); return -1; }
    l->mem_n = 0; /* memtable 비움 */
    return 0;
}

static int maybe_flush(LSM *l) {
    if (l->mem_n >= l->threshold) return lsm_flush(l);
    return 0;
}

int lsm_put(LSM *l, int64_t key, int64_t val) {
    if (!l) return -1;
    if (mem_upsert(l, key, val, 0) != 0) return -1;
    return maybe_flush(l);
}

int lsm_delete(LSM *l, int64_t key) {
    if (!l) return -1;
    if (mem_upsert(l, key, 0, 1) != 0) return -1; /* tombstone */
    return maybe_flush(l);
}

int lsm_put_dup(LSM *l, int64_t key, int64_t val) {
    if (!l) return -1;
    if (mem_upsert_dup(l, key, val, 0) != 0) return -1;
    return maybe_flush(l);
}

int lsm_delete_val(LSM *l, int64_t key, int64_t val) {
    if (!l) return -1;
    if (mem_upsert_dup(l, key, val, 1) != 0) return -1; /* 그 (key,val) 짝만 tombstone */
    return maybe_flush(l);
}

void lsm_clear(LSM *l) {
    if (!l) return;
    for (size_t i = 0; i < l->nsst; i++) {
        unlink(l->ssts[i]->path);
        sst_close(l->ssts[i]);
    }
    l->nsst = 0;
    l->mem_n = 0;
}

int lsm_get(LSM *l, int64_t key, int64_t *out) {
    if (!l) return 0;
    /* 1) memtable(가장 최신) */
    int found;
    size_t pos = mem_bsearch(l, key, &found);
    if (found) {
        if (l->mem[pos].tomb) return 0; /* 삭제됨 */
        if (out) *out = l->mem[pos].val;
        return 1;
    }
    /* 2) SSTable 최신 -> 오래된. 처음 만난 버전이 이긴다(read amplification). */
    for (size_t i = l->nsst; i-- > 0;) {
        int64_t v; int tomb;
        if (sst_get(l->ssts[i], key, &v, &tomb)) {
            if (tomb) return 0; /* tombstone이 옛 값을 가린다 */
            if (out) *out = v;
            return 1;
        }
    }
    return 0;
}

int lsm_sstable_count(const LSM *l) {
    return l ? (int)l->nsst : 0;
}

/* ── merge 헬퍼: 여러 정렬 run을 (key asc, seq desc)로 훑어 최신만 뽑기 ──
 * memtable + 모든 SSTable의 엔트리를 (key, priority) 키로 정렬한다.
 * priority가 클수록 최신. 같은 key면 최신 하나만 채택. */
typedef struct {
    int64_t  key;
    int64_t  val;
    int      tomb;
    uint64_t prio; /* memtable=UINT64_MAX, SSTable=seq */
} MEntry;

static int mentry_cmp(const void *a, const void *b) {
    const MEntry *x = a, *y = b;
    if (x->key < y->key) return -1;
    if (x->key > y->key) return 1;
    /* 같은 key: 최신(prio 큰) 것이 앞으로 */
    if (x->prio > y->prio) return -1;
    if (x->prio < y->prio) return 1;
    return 0;
}

/* [멀티] (key asc, val asc, prio desc): 같은 (key,val) 짝의 최신이 그 짝의 맨 앞. */
static int mentry_cmp_multi(const void *a, const void *b) {
    const MEntry *x = a, *y = b;
    if (x->key < y->key) return -1;
    if (x->key > y->key) return 1;
    if (x->val < y->val) return -1;
    if (x->val > y->val) return 1;
    if (x->prio > y->prio) return -1;
    if (x->prio < y->prio) return 1;
    return 0;
}

/* dedup 그룹 경계: 유니크는 key, 멀티는 (key,val)가 같으면 같은 그룹(=옛 버전). */
static int same_group(const MEntry *a, const MEntry *b, int multi) {
    return a->key == b->key && (!multi || a->val == b->val);
}

/* memtable + SSTable 전부를 하나의 MEntry 배열로 수집(prio 부여). */
static MEntry *collect_all(LSM *l, size_t *out_n) {
    size_t cap = l->mem_n;
    for (size_t i = 0; i < l->nsst; i++) cap += l->ssts[i]->count;
    MEntry *arr = malloc((cap ? cap : 1) * sizeof(MEntry));
    if (!arr) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < l->mem_n; i++)
        arr[n++] = (MEntry){l->mem[i].key, l->mem[i].val, l->mem[i].tomb, UINT64_MAX};
    for (size_t i = 0; i < l->nsst; i++) {
        SSTable *s = l->ssts[i];
        for (uint64_t j = 0; j < s->count; j++) {
            int64_t k, v; int t;
            sst_read_rec(s, j, &k, &v, &t);
            arr[n++] = (MEntry){k, v, t, s->seq};
        }
    }
    qsort(arr, n, sizeof(MEntry), l->multi ? mentry_cmp_multi : mentry_cmp);
    *out_n = n;
    return arr;
}

int lsm_compact(LSM *l) {
    if (!l) return -1;
    if (l->nsst < 1) return 0; /* 합칠 게 없다 */

    /* compaction은 SSTable만 대상으로 한다(memtable은 여전히 최신으로 가림).
     * 그래서 memtable을 제외하고 SSTable들만 수집한다. */
    size_t cap = 0;
    for (size_t i = 0; i < l->nsst; i++) cap += l->ssts[i]->count;
    if (cap == 0) return 0;
    MEntry *arr = malloc(cap * sizeof(MEntry));
    if (!arr) return -1;
    size_t n = 0;
    for (size_t i = 0; i < l->nsst; i++) {
        SSTable *s = l->ssts[i];
        for (uint64_t j = 0; j < s->count; j++) {
            int64_t k, v; int t;
            sst_read_rec(s, j, &k, &v, &t);
            arr[n++] = (MEntry){k, v, t, s->seq};
        }
    }
    qsort(arr, n, sizeof(MEntry), l->multi ? mentry_cmp_multi : mentry_cmp);

    /* 그룹(유니크=key, 멀티=(key,val))마다 최신(맨 앞) 하나만. 전체 compaction이라
     * 옛 데이터가 남지 않으므로 tombstone은 버린다(진짜 삭제 완료 = VACUUM 지점). */
    Entry *live = malloc((n ? n : 1) * sizeof(Entry));
    if (!live) { free(arr); return -1; }
    size_t ln = 0;
    for (size_t i = 0; i < n;) {
        MEntry cur = arr[i]; /* 이 그룹의 최신(prio 최대) */
        if (!cur.tomb) live[ln++] = (Entry){cur.key, cur.val, 0};
        i++;
        while (i < n && same_group(&arr[i], &cur, l->multi)) i++; /* 옛 버전 스킵 */
    }
    free(arr);

    /* 새 SSTable 하나로 쓴다(가장 큰 seq을 받아 최신이 되게). */
    uint64_t seq = l->next_seq++;
    char path[1024];
    sst_path(l, seq, path, sizeof(path));
    if (sst_write_file(path, live, ln) != 0) { free(live); return -1; }
    free(live);
    SSTable *merged = sst_open(seq, path);
    if (!merged) return -1;

    /* 옛 SSTable 파일들을 닫고 unlink, 목록을 merged 하나로 교체. */
    for (size_t i = 0; i < l->nsst; i++) {
        char old[1024];
        snprintf(old, sizeof(old), "%s", l->ssts[i]->path);
        sst_close(l->ssts[i]);
        unlink(old);
    }
    l->nsst = 0;
    if (lsm_add_sst(l, merged) != 0) { sst_close(merged); return -1; }
    return 0;
}

int64_t lsm_scan(LSM *l, int64_t lo, int64_t hi,
                 void (*cb)(int64_t key, int64_t val, void *ctx), void *ctx) {
    if (!l || lo > hi) return 0;
    size_t n = 0;
    MEntry *arr = collect_all(l, &n);
    if (!arr) return 0;
    /* 정렬 상태. 그룹(유니크=key, 멀티=(key,val))마다 최신 하나만, tombstone·범위밖 스킵.
     * 멀티 모드에선 같은 key의 살아있는 여러 val을 모두 방문한다(다중버전 인덱스). */
    int64_t cnt = 0;
    for (size_t i = 0; i < n;) {
        MEntry cur = arr[i];
        i++;
        while (i < n && same_group(&arr[i], &cur, l->multi)) i++;
        if (cur.key < lo || cur.key > hi) continue;
        if (cur.tomb) continue;
        if (cb) cb(cur.key, cur.val, ctx);
        cnt++;
    }
    free(arr);
    return cnt;
}

/* [멀티] key의 살아있는 모든 val을 (val 오름차순) 방문. (key,val)마다 최신이 이기고
 * tombstone된 짝은 스킵. 다중버전 PK 인덱스의 '= 점 조회'가 쓰는 경로. */
int64_t lsm_find_all(LSM *l, int64_t key,
                     void (*cb)(int64_t key, int64_t val, void *ctx), void *ctx) {
    if (!l) return 0;
    size_t n = 0;
    MEntry *arr = collect_all(l, &n);
    if (!arr) return 0;
    int64_t cnt = 0;
    for (size_t i = 0; i < n;) {
        MEntry cur = arr[i];
        i++;
        while (i < n && same_group(&arr[i], &cur, l->multi)) i++;
        if (cur.key != key) continue;
        if (cur.tomb) continue;
        if (cb) cb(cur.key, cur.val, ctx);
        cnt++;
    }
    free(arr);
    return cnt;
}
