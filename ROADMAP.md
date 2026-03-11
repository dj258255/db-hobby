# minidb 로드맵 (트랙별)

진행 상황 한눈에. 블로그 시리즈(1~12편)와 그 뒤 추가분을 추적한다.
세부 한계는 `README.md`의 "Scope", 구조는 `DESIGN.md` 참고.

현재: **테스트 316개 / 19스위트 통과.**

> 저장 철학에 따라 갈리는 일을 두 트랙으로 나눈다 — **A: PostgreSQL식**(minidb의 현재 정체성),
> **B: MySQL/InnoDB 대조**. 저장과 무관한 SQL 마무리는 **C**. 공통 핵심은 이미 다 만들었다(Done).

## Done — 공통 핵심 (PG·MySQL이 똑같이 쓰는 것)

- [x] **저장** — 페이저(`pread`/`pwrite`, 고정 4KB), 슬롯 페이지(가변 행), 버퍼 풀(pin/dirty/LRU), 힙 파일
- [x] **SQL 프런트엔드** — 손으로 쓴 렉서 + 재귀 하강 파서 -> AST, 튜플 코덱(INT/TEXT, null 비트맵)
- [x] **인덱스** — 디스크 B+Tree(노드 분할, 점 조회·범위 스캔·중복 키), 플래너(점/범위/풀스캔)
- [x] **WAL** — redo-only + no-steal, 커밋 시 force, 크래시 복구(redo/discard). 힙·인덱스 둘 다 보호
- [x] **트랜잭션** — `BEGIN`/`COMMIT`/`ROLLBACK` (원자성 A · 내구성 D)
- [x] **조인** — N-way 중첩 루프, 인덱스 NLJ, 해시 조인, `INNER`/`LEFT`, 별칭·self-join
- [x] **집계** — `COUNT`/`SUM`/`MIN`/`MAX`/`AVG`, `GROUP BY`, `HAVING`, 정렬 기반 GroupAggregate
- [x] **WHERE** — 비교 6종, `AND`/`OR`(DNF), `IN(값목록)`, `IN(SELECT)`/`NOT IN`, 스칼라 서브쿼리, `BETWEEN`, `LIKE`, `IS [NOT] NULL`
- [x] **출력** — 다중 키 `ORDER BY`, `LIMIT`, `OFFSET`, `DISTINCT`
- [x] **NULL** — 저장(null 비트맵), `NOT NULL` 제약, 3값 논리, NULLS LAST
- [x] **EXPLAIN** — 실행기와 동일 로직으로 플랜 트리 출력 · **벤치마크**(`make bench`)
- [x] **보조 인덱스** — `CREATE INDEX`(INT 비유니크), 빌드·카탈로그 영속화·DML 유지보수·플래너+EXPLAIN (4단계, 10편)
- [x] **격리(2PL)** — 테이블 S/X 락 · strict 2PL · 교착 탐지. dirty read/lost update 방지 시연 (3단계, 11편). *잠금 기반 — 진짜 MVCC는 트랙 A*

---

## 트랙 A — PostgreSQL식 (현재 정체성, 한 줄기로 완성)

minidb는 이미 PG식이다 — 힙 + 별도 인덱스(RID), relfilenode, dead tuple, UPDATE=새 버전, EXPLAIN 용어.
남은 건 **격리를 잠금(2PL)에서 버전(MVCC)으로** 끌어올리고, 그 부산물인 죽은 공간을 **VACUUM**으로 청소하는 것.
이 둘을 하면 "진짜 미니 PostgreSQL"이 완성된다. (자세한 2PL vs MVCC 대조는 12편.)

### A1. MVCC (스냅샷 격리) — 진행 중
- [x] **1. 트랜잭션 상태 로그 + 가시성 규칙** (mvcc.c, standalone) — "xmin 커밋 AND xmax 미커밋이면 보임". abort된 INSERT/DELETE/UPDATE가 상태만으로 롤백되는 것까지 test_mvcc로 검증
- [ ] 2. 행에 `xmin`/`xmax` 저장(codec 헤더, tombstone을 xmax로 승격) + 읽기 경로가 가시성으로 dead row 판정 + 트랜잭션 시작 스냅샷
- [ ] 3. 다중 트랜잭션 핸들 + 인터리브 데모 (reader가 writer를 안 막는 걸 진짜로 시연)
- [ ] 4. 쓰기-쓰기 충돌(first-updater-wins)

### A2. VACUUM (죽은 공간 회수) — MVCC의 짝
MVCC는 일부러 옛 버전(dead tuple)을 쌓으니, 안 치우면 테이블·인덱스가 부푼다(bloat). 그걸 청소.
- [ ] 죽은 튜플(tombstone/안 보이는 옛 버전)의 힙 공간 회수
- [ ] 죽은 인덱스 항목 제거 = **B+Tree 키 삭제**(삽입의 반대: 노드 병합·재분배)
  - ※ B+Tree 자체와 범위 스캔은 **이미 있음**. 이건 "키를 지우는 연산"으로, 오직 공간 회수용이다.

---

## 트랙 B — MySQL/InnoDB 대조 (트랙 A 완성 뒤 별도 챕터)

별도 프로젝트를 또 만들지 않는다. minidb 안에 InnoDB식 선택지를 **다른 모드**로 더해,
한 코드에서 PG식 vs MySQL식을 나란히 비교한다(블로그 1편 힙 vs 클러스터드, 4편 append-only vs undo의 코드판).
- [ ] **B1. 클러스터드 테이블 모드** (index-organized) — 데이터를 PK B+Tree에 PK 순서로 저장, 보조 인덱스는 RID가 아니라 PK 값을 듦("보조 -> PK -> 데이터" 2단 조회). 힙(PG) vs 클러스터드(InnoDB)를 `make bench`로 비교(PK 조회·보조 조회·INSERT 비용)
- [ ] **B2. undo 기반 MVCC** — in-place 수정 + undo log (PG의 append-only 새 버전과 대조). dead tuple 대신 undo·purge
- [ ] **B3. (선택) InnoDB 잠금** — next-key 락(갭 락) 등

---

## 트랙 C — SQL 완성도 (저장 철학과 무관, 머리 식힐 때 하나씩)

- [ ] 다중 컬럼 `GROUP BY`
- [ ] 상관(correlated) 서브쿼리
- [ ] 복합·커버링 인덱스 / 인덱스 온리 스캔
- [ ] 괄호로 묶은 WHERE, 다중 조건 `ON`
- [ ] `DEFAULT` 값, `UPDATE ... SET col = NULL`
- [ ] 버퍼 풀을 넘는 큰 트랜잭션 (no-steal + `WAL_MAX_STAGED` 한계 — 진짜로 풀려면 steal/ARIES)

---

## 추천 순서

1. **트랙 A 한 줄기**: A1(MVCC) -> A2(VACUUM). "진짜 미니 PostgreSQL" 완성.
2. 그다음 **트랙 B**(클러스터드 모드)로 MySQL 대조 — 힙 vs 클러스터드를 직접 벤치.
3. **트랙 C**는 사이사이 하나씩.
