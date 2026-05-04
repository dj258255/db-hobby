#ifndef MINIDB_CBTREE_H
#define MINIDB_CBTREE_H

#include <stdint.h>

/*
 * cbtree — latch crabbing(lock coupling)을 격리해 보이는 동시성 B+Tree.
 *
 * 트랙 D의 정수 알고리즘. 엔진의 btree.c(디스크·버퍼 풀)와 별개로, 저장 세부를 걷어내고
 * "여러 스레드가 같은 트리를 동시에 읽고 쓸 때 노드 래치를 어떻게 잡고 놓는가"만 순수하게
 * 담는다(인메모리, 노드마다 rwlock). CMU 15-445 Project 2의 그 crabbing이다.
 *
 * 규칙(루트->리프 한 방향이라 교착 없음):
 *   - 탐색(읽기): 자식 rlatch를 잡은 뒤 부모 rlatch를 놓으며 내려간다(lock coupling).
 *   - 삽입(쓰기): 자식 wlatch를 잡고, 그 자식이 "안전"(여유 있어 안 쪼개짐)하면 조상
 *     wlatch를 전부 놓는다. 안 안전하면 붙들고 있다가 분할이 위로 번지면 그 조상에 반영.
 *
 * 엔진에 아직 배선하진 않았다(엔진은 20편의 굵은 latch로 직렬 실행) — 이 모듈은 그 굵은
 * latch를 계층별로 걷어낼 때 필요한 "동시 B+Tree" 기법을 독립적으로 증명한다.
 */

typedef struct CBTree CBTree;

CBTree *cbtree_create(void);
void cbtree_destroy(CBTree *t);

/* key -> val 삽입(있으면 갱신). 0 성공. */
int cbtree_insert(CBTree *t, int64_t key, int64_t val);

/* key 조회. 있으면 *out에 값 넣고 0, 없으면 -1. */
int cbtree_search(CBTree *t, int64_t key, int64_t *out);

/* [lo, hi] 범위의 키 개수(리프 체인 crabbing으로 훑는다). */
int64_t cbtree_range_count(CBTree *t, int64_t lo, int64_t hi);

#endif /* MINIDB_CBTREE_H */
