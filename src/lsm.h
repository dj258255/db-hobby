#ifndef MINIDB_LSM_H
#define MINIDB_LSM_H

#include <stdint.h>
#include <stddef.h>

/*
 * lsm — Log-Structured Merge 트리. B+Tree의 쓰기 최적화 대척점.
 *
 * 이 프로젝트의 btree.c/clustered는 "제자리 갱신(in-place)" 저장이다: 키가 갈
 * 페이지를 찾아가 그 페이지를 랜덤하게 읽고, 고치고, 다시 쓴다. 읽기엔 좋지만
 * 쓰기는 랜덤 I/O + 페이지 분할이라 비싸다(PostgreSQL/InnoDB 계열).
 *
 * LSM은 정반대 내기를 건다. "제자리에서 절대 안 고친다." 모든 쓰기는
 *   1) 인메모리 정렬 구조(memtable)에 append 되고,
 *   2) 임계치를 넘으면 통째로 정렬된 불변 파일(SSTable)로 "순차" flush 되고,
 *   3) 나중에 background로 여러 SSTable을 merge(compaction)해 정리된다.
 * 삭제조차 제자리 삭제가 아니라 tombstone(묘비) 마커를 새로 쓴다 — 이 프로젝트의
 * MVCC DELETE(xmax 논리 삭제)와 VACUUM(나중에 실제 청소)이 했던 그 지연 삭제와
 * 똑같은 발상이다. RocksDB/LevelDB/Cassandra/HBase가 이 계열이다.
 *
 * 대가(trade-off): 읽기가 비싸진다. 한 키를 찾으려면 memtable -> 최신 SSTable ->
 * ... -> 가장 오래된 SSTable 순으로 뒤져야 한다(read amplification). 그래서
 * 최신이 옛것을 "가린다(shadow)": 처음 만난 버전(tombstone 포함)이 이긴다.
 *
 * ── 정직한 경계 ─────────────────────────────────────────────────────
 * 이 모듈은 db.c의 저장 계층(힙/B+Tree)에 배선돼 있지 않다. cbtree.c/joinopt.c
 * 처럼 독립 모듈 + 자체 테스트로 서서, "쓰기 최적화 저장 엔진"의 뼈대만 순수하게
 * 증명한다. SQL 계층 뒤에 LSM을 "또 하나의 테이블 저장 엔진"으로 꽂는 것(MySQL의
 * MyRocks가 InnoDB 옆에 RocksDB를 꽂듯)이 프론티어다 — lsm.c 상단 주석 참고.
 * ────────────────────────────────────────────────────────────────────
 *
 * 키/값은 B+Tree처럼 int64_t 고정. 값 하나 + tombstone 비트가 전부다.
 */

typedef struct LSM LSM;

/* dir 디렉터리를 SSTable 저장소로 열거나 만든다(없으면 mkdir). 이미 있던 *.sst
 * 파일들을 발견해 read path에 편입한다(= reopen/persistence). memtable이
 * threshold개를 넘으면 put/delete가 자동으로 flush를 유발한다. 실패 시 NULL. */
LSM *lsm_open(const char *dir, size_t memtable_threshold);

/* lsm_open과 같되 multi=1이면 '멀티값(비유니크)' 모드로 연다. 유니크 모드는 한 key에
 * 값 하나(dedup 단위 = key)지만, 멀티 모드는 한 key에 여러 값을 담고 dedup 단위가
 * (key,val) 짝이 된다. DB의 다중버전 인덱스(한 PK -> 여러 행 버전 RID)가 요구하는
 * 모드다. 멀티 모드에서는 lsm_put_dup/lsm_delete_val/lsm_find_all/lsm_scan을 쓰고,
 * 유니크 전용인 lsm_put/lsm_delete/lsm_get은 쓰지 않는다(반대도 마찬가지). */
LSM *lsm_open_multi(const char *dir, size_t memtable_threshold, int multi);

/* [멀티] (key,val) 짝을 추가한다. 같은 key의 다른 val을 가리지 않는다(비유니크).
 * 같은 (key,val)을 다시 넣으면 tombstone만 해제된다. 성공 0, 실패 -1. */
int lsm_put_dup(LSM *l, int64_t key, int64_t val);

/* [멀티] 특정 (key,val) 짝만 tombstone 한다(그 짝만 삭제). 성공 0, 실패 -1. */
int lsm_delete_val(LSM *l, int64_t key, int64_t val);

/* [멀티] key의 살아있는 모든 val을 (val 오름차순) 콜백으로 넘긴다. 방문 개수 반환.
 * (key,val)마다 최신 버전이 이기고, tombstone된 짝은 건너뛴다. */
int64_t lsm_find_all(LSM *l, int64_t key,
                     void (*cb)(int64_t key, int64_t val, void *ctx), void *ctx);

/* 모든 SSTable을 unlink 하고 memtable을 비운다(빈 상태로 리셋). heap 같은 상위
 * 진실의 원천에서 인덱스를 재구축하기 직전에 부른다. */
void lsm_clear(LSM *l);

/* 닫는다. 주의: memtable(인메모리)은 flush 하지 않은 채 버려진다 — 우리는 WAL을
 * 두지 않았기 때문(단순화). 살아남기려면 close 전에 lsm_flush를 부르거나 임계치로
 * flush를 유발해야 한다. 이미 flush된 SSTable은 디스크에 남아 reopen 시 읽힌다. */
void lsm_close(LSM *l);

/* key -> val 쓰기. 제자리 갱신이 아니라 memtable에 최신 버전을 append/치환한다.
 * 임계치를 넘으면 flush가 일어날 수 있다. 성공 0, 실패 -1. */
int lsm_put(LSM *l, int64_t key, int64_t val);

/* key 삭제 = tombstone 쓰기(제자리 삭제 아님). 옛 값이 SSTable에 있어도 최신
 * tombstone이 그것을 가린다. 성공 0, 실패 -1. */
int lsm_delete(LSM *l, int64_t key);

/* key 조회. read path: memtable -> SSTable 최신->오래된 순. 처음 만난 버전이
 * 이긴다(tombstone이면 "삭제됨"). 찾으면 *out에 값 넣고 1, 없거나 삭제됐으면 0. */
int lsm_get(LSM *l, int64_t key, int64_t *out);

/* memtable을 새 SSTable로 강제 flush(비어 있으면 no-op). 순차 쓰기 한 번.
 * 성공 0, 실패 -1. */
int lsm_flush(LSM *l);

/* 모든 SSTable을 하나로 merge. 키마다 최신 버전만 남기고, 옛 데이터가 전부
 * 사라지므로 tombstone도 버린다. SSTable 개수가 줄어든다. 성공 0, 실패 -1. */
int lsm_compact(LSM *l);

/* 현재 디스크 SSTable 개수(read amplification/compaction 관측용). */
int lsm_sstable_count(const LSM *l);

/* [lo, hi] 범위의 살아있는 키를 오름차순으로 훑어 cb를 부른다(정렬된 run들을
 * merge). tombstone과 가려진 옛 버전은 건너뛴다. 방문한 키 개수를 반환. */
int64_t lsm_scan(LSM *l, int64_t lo, int64_t hi,
                 void (*cb)(int64_t key, int64_t val, void *ctx), void *ctx);

#endif /* MINIDB_LSM_H */
