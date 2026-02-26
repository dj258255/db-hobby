# minidb 로드맵 (Done / Next)

진행 상황 한눈에. 블로그 시리즈(1~9편)와 그 뒤 추가분을 추적한다.
세부 한계는 `README.md`의 "Scope" 섹션, 구조는 `DESIGN.md` 참고.

현재: **테스트 251개 / 15스위트 통과.**

## Done

- [x] **저장** — 페이저(`pread`/`pwrite`, 고정 4KB), 슬롯 페이지(가변 행), 버퍼 풀(pin/dirty/LRU), 힙 파일
- [x] **SQL 프런트엔드** — 손으로 쓴 렉서 + 재귀 하강 파서 -> AST, 튜플 코덱(INT/TEXT, null 비트맵)
- [x] **인덱스** — 디스크 B+Tree(PK, 노드 분할), 플래너(점 조회 / 범위 스캔 / 풀 스캔)
- [x] **WAL** — redo-only + no-steal, 커밋 시 force, 크래시 복구(redo/discard). 힙(`.tbl`)과 인덱스(`.idx`) 둘 다 보호
- [x] **트랜잭션** — `BEGIN` / `COMMIT` / `ROLLBACK` (원자성 A · 내구성 D)
- [x] **조인** — N-way 중첩 루프, 인덱스 NLJ, 해시 조인, `INNER` / `LEFT`, 별칭·self-join
- [x] **집계** — `COUNT`/`SUM`/`MIN`/`MAX`/`AVG`, `GROUP BY`, `HAVING`, 정렬 기반 GroupAggregate
- [x] **WHERE** — 비교 6종, `AND`/`OR`(DNF), `IN (값목록)`, `IN (SELECT)`/`NOT IN`, 스칼라 서브쿼리, `BETWEEN`, `LIKE`/`NOT LIKE`, `IS [NOT] NULL`
- [x] **출력** — 다중 키 `ORDER BY`, `LIMIT`, `OFFSET`, `DISTINCT`
- [x] **NULL** — 저장(nullable 컬럼, null 비트맵), `NOT NULL` 제약, 3값 논리, NULLS LAST
- [x] **EXPLAIN** — 실행기와 동일 로직으로 플랜 트리 출력
- [x] **벤치마크** — `make bench` (인덱스 vs 풀스캔, fsync 비용)

## Next

### 보조 인덱스 (CREATE INDEX) — 진행 중
PK 외 임의 컬럼에 인덱스. 4단계로 쪼갠다.
- [x] **1. B+Tree 중복 키 지원** (btree_insert_dup + btree_find_all, 분할 가로지르는 중복도 하한 탐색으로 전부 조회) — 비유니크 인덱스(같은 값의 여러 행)를 위해, 키가 같아도 덮어쓰지 않고 (key,val) 여러 개를 담고 다 찾아오는 경로 추가 (현재 `btree_insert`는 "있으면 갱신")
- [ ] 2. `CREATE INDEX <name> ON <t>(<col>)` 파싱 + 인덱스 파일 생성 + 기존 행 채우기 + 카탈로그 영속화
- [ ] 3. DML 유지보수(INSERT/DELETE가 보조 인덱스도 갱신) + 인덱스별 WAL
- [ ] 4. 플래너가 보조 인덱스 선택 + EXPLAIN 표시

### 격리 / 동시성 (ACID의 I) — 가장 큼
- [ ] 동시 트랜잭션(스레드), MVCC(스냅샷) 또는 2PL 락, 격리 수준

### 그 밖에 (작은~중간)
- [ ] B+Tree 삭제 (현재 삭제 행은 힙에서 tombstone, 인덱스 항목은 방치)
- [ ] 버퍼 풀을 넘는 큰 트랜잭션 (no-steal + WAL_MAX_STAGED 한계)
- [ ] 복합·커버링 인덱스 / 인덱스 온리 스캔
- [ ] 다중 컬럼 `GROUP BY`
- [ ] 상관(correlated) 서브쿼리
- [ ] 괄호로 묶은 WHERE, 다중 조건 `ON`
- [ ] `DEFAULT` 값, `UPDATE ... SET col = NULL`
