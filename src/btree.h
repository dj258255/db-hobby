#ifndef MINIDB_BTREE_H
#define MINIDB_BTREE_H

#include <stdint.h>
#include "pager.h"
#include "bufpool.h"
#include "wal.h"

/*
 * B+Tree 인덱스 — 키 -> 값을 O(log n)에 찾는다. (InnoDB clustered index,
 * PostgreSQL btree와 같은 자료구조.) 풀 스캔(O(n))을 피하는 장치.
 *
 * 노드는 페이지에 저장된다(버퍼 풀 경유). 리프가 꽉 차면 반으로 쪼개고
 * 가운데 키를 부모로 올린다(split). 부모도 꽉 차면 또 쪼개지며, 루트까지
 * 올라가면 트리 높이가 1 자란다 — 그래서 항상 균형이 유지된다.
 *
 * 테이블 데이터와 섞이지 않도록 인덱스는 자기 파일(.idx)을 따로 쓰고, 데이터 파일처럼
 * WAL(.idx.wal)로 감싼다 — 인덱스 페이저는 wal.data 안에 있다.
 * page 0 = 메타(루트 페이지 id 저장), page 1+ = 노드들.
 *
 * 학습용으로 키·값은 int64다. db-hobby에서는 키=INT 컬럼 값, 값=RID를 int64로 인코딩.
 */

typedef int64_t bkey_t;
typedef int64_t bval_t;

typedef struct {
    Wal wal; /* 인덱스 파일(.idx)을 WAL로 감싼다. 페이저는 wal.data. */
    BufferPool *bp;
    page_id_t root;
} BTree;

/* 인덱스 파일을 연다(없으면 빈 트리 생성). 로그 파일은 <path>.wal. 0 성공, -1 실패. */
int btree_open(BTree *bt, const char *path);

/* flush하고 닫는다. */
void btree_close(BTree *bt);

/* 키에 값을 넣는다(있으면 갱신 = 유니크). 0 성공, -1 실패. PK 인덱스용. */
int btree_insert(BTree *bt, bkey_t key, bval_t val);

/* 키에 값을 넣되 같은 키를 덮어쓰지 않고 추가한다(비유니크). 보조 인덱스용. 0/-1. */
int btree_insert_dup(BTree *bt, bkey_t key, bval_t val);

/* (key, val) 항목 하나를 리프에서 제거한다 — VACUUM이 죽은 버전의 인덱스 항목을
 * 지울 때 쓴다. lazy 삭제: 노드 병합·재분배는 안 한다(PostgreSQL nbtree도 리프
 * 항목 삭제 + 빈 페이지 재활용만 하지 재분배는 안 한다). 내부 노드의 분리키가
 * stale해질 수 있지만 라우팅 안내판일 뿐이라 탐색은 정확하다. 0 성공, -1 없음. */
int btree_delete_val(BTree *bt, bkey_t key, bval_t val);

/* 키를 찾는다. 있으면 *out에 값을 넣고 0, 없으면 -1. (같은 키가 여럿이면 그중 하나.) */
int btree_search(BTree *bt, bkey_t key, bval_t *out);

/* 메타 페이지에서 루트를 다시 읽는다(롤백 후 in-memory 루트를 디스크와 동기화). */
void btree_reload_root(BTree *bt);

/* 모든 키를 오름차순으로 visit에 넘긴다(리프 체인 따라). visit가 0 아니면 멈춤. */
typedef int (*btree_visit_fn)(bkey_t key, bval_t val, void *ctx);
int btree_scan(BTree *bt, btree_visit_fn visit, void *ctx);

/* key와 같은 키의 값을 전부 visit에 넘긴다(중복 키 조회). 하한 탐색 후 리프 체인을 훑는다.
 * btree_insert_dup으로 넣은 비유니크 인덱스에서 한 키의 모든 RID를 모을 때 쓴다. */
int btree_find_all(BTree *bt, bkey_t key, btree_visit_fn visit, void *ctx);

/* start 이상(key >= start)인 키부터 오름차순으로 visit한다(범위 스캔의 lower bound).
 * start가 있는 리프로 바로 내려간 뒤 리프 체인을 따라 옆으로 읽는다. */
int btree_seek_scan(BTree *bt, bkey_t start, btree_visit_fn visit, void *ctx);

#endif /* MINIDB_BTREE_H */
