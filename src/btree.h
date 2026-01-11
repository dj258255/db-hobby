#ifndef MINIDB_BTREE_H
#define MINIDB_BTREE_H

#include <stdint.h>
#include "pager.h"
#include "bufpool.h"

/*
 * B+Tree 인덱스 — 키 → 값을 O(log n)에 찾는다. (InnoDB clustered index,
 * PostgreSQL btree와 같은 자료구조.) 풀 스캔(O(n))을 피하는 장치.
 *
 * 노드는 페이지에 저장된다(버퍼 풀 경유). 리프가 꽉 차면 반으로 쪼개고
 * 가운데 키를 부모로 올린다(split). 부모도 꽉 차면 또 쪼개지며, 루트까지
 * 올라가면 트리 높이가 1 자란다 — 그래서 항상 균형이 유지된다.
 *
 * 테이블 데이터와 섞이지 않도록 인덱스는 자기 파일을 따로 쓴다.
 * page 0 = 메타(루트 페이지 id 저장), page 1+ = 노드들.
 *
 * 학습용으로 키·값은 int64다. minidb에서는 키=INT 컬럼 값, 값=RID를 int64로 인코딩.
 */

typedef int64_t bkey_t;
typedef int64_t bval_t;

typedef struct {
    Pager pager;
    BufferPool *bp;
    page_id_t root;
} BTree;

/* 인덱스 파일을 연다(없으면 빈 트리 생성). 0 성공, -1 실패. */
int btree_open(BTree *bt, const char *path);

/* flush하고 닫는다. */
void btree_close(BTree *bt);

/* 키에 값을 넣는다(있으면 갱신). 0 성공, -1 실패. */
int btree_insert(BTree *bt, bkey_t key, bval_t val);

/* 키를 찾는다. 있으면 *out에 값을 넣고 0, 없으면 -1. */
int btree_search(BTree *bt, bkey_t key, bval_t *out);

/* 모든 키를 오름차순으로 visit에 넘긴다(리프 체인 따라). visit가 0 아니면 멈춤. */
typedef int (*btree_visit_fn)(bkey_t key, bval_t val, void *ctx);
int btree_scan(BTree *bt, btree_visit_fn visit, void *ctx);

#endif /* MINIDB_BTREE_H */
