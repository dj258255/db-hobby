# minidb 로드맵 (Done / Next)

진행 상황 한눈에. 블로그 시리즈(1~10편)와 그 뒤 추가분을 추적한다.
세부 한계는 `README.md`의 "Scope" 섹션, 구조는 `DESIGN.md` 참고.

현재: **테스트 306개 / 18스위트 통과.**

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

### 보조 인덱스 (CREATE INDEX) — 완료 (4단계 전부)
PK 외 임의 컬럼에 인덱스. 4단계로 쪼갠다.
- [x] **1. B+Tree 중복 키 지원** (btree_insert_dup + btree_find_all, 분할 가로지르는 중복도 하한 탐색으로 전부 조회) — 비유니크 인덱스(같은 값의 여러 행)를 위해, 키가 같아도 덮어쓰지 않고 (key,val) 여러 개를 담고 다 찾아오는 경로 추가 (현재 `btree_insert`는 "있으면 갱신")
- [x] **2. `CREATE INDEX <name> ON <t>(<col>)`** — 파싱, 인덱스 파일 생성, 기존 행으로 채우기, 카탈로그 영속화(재오픈 복원). INT 컬럼만, 비유니크(btree_insert_dup). 아직 플래너가 안 쓰고 DML이 갱신 안 함(3~4단계)
- [x] **3. DML 유지보수 + 인덱스별 WAL** — INSERT가 보조 인덱스도 채우고, UPDATE는 RID가 바뀌므로 모든 보조 인덱스에 새 RID 재등록. DELETE는 tombstone이라 stale 항목 무해. 보조 인덱스도 자기 WAL로 묶여 롤백 시 함께 되돌아가고 커밋분은 영속.
  - [x] DELETE/UPDATE의 stale 인덱스 항목은 4단계의 heap_get + WHERE 재검사로 걸러짐
- [x] **4. 플래너가 보조 인덱스 선택 + EXPLAIN 표시** — WHERE 비PK컬럼=값이면 find_all로 후보 RID -> heap_get + WHERE 재검사(tombstone/stale 거름). EXPLAIN에 `Index Scan using <name>`.

### 격리 / 동시성 (ACID의 I) — 완료 (단일 스레드 모델 내)
단일 스레드라 OS 동시성은 없다. 대신 인터리브된 in-process 트랜잭션 + 2PL 락으로
격리의 핵심(충돌 직렬화, lost update/dirty read 방지)을 보인다. 단계로 쪼갠다.
- [x] **1. 락 매니저** — (테이블,키) 단위 S/X 락, 충돌 행렬, acquire(충돌이면 -1)/release, S->X 업그레이드
- [x] **2. DML에 락 통합 + 인터리브 데모** — INSERT/UPDATE/DELETE는 테이블 X, SELECT는 S 락을 cur_txn으로 잡아 2PL(COMMIT/ROLLBACK까지 유지). 충돌은 단일 스레드라 거부(ERROR). test_isolation이 다른 txn 락을 주입해 dirty read/lost update 방지·reader 호환을 진짜 엔진에서 시연.
- [x] **3. 교착 탐지** — wait-for 그래프(누가 누구를 기다리나) + DFS 순환 탐지. 거부 모델이라 실제 교착은 안 생기지만, "대기한다면" 생길 순환을 lock_deadlock_victim이 찾아 victim 반환(2중/3중 순환 데모).

### MVCC (스냅샷 격리) — 진짜 PostgreSQL식 격리 (다음 큰 주제)
현재 격리는 테이블 단위 2PL(잠금)이라 거칠고, 충돌을 거부하며, 실행기가 트랜잭션을 하나씩만 연다.
PostgreSQL식은 MVCC(다중 버전): **읽기가 쓰기를 막지 않고**, 각 트랜잭션이 자기 스냅샷을 본다.
minidb 저장 구조는 이미 MVCC에 맞다 — 힙, dead tuple, UPDATE=새 버전(=PG). 빠진 조각만 채운다.
- [ ] 1. 트랜잭션 id + 상태 로그(커밋/아보트) + 행에 `xmin`/`xmax` (지금의 tombstone 비트를 xmax로 승격)
- [ ] 2. 스냅샷 + 가시성 판정 (SELECT가 "내 스냅샷에서 보이는 버전"만 — xmin 커밋·보임 + xmax 안 보임)
- [ ] 3. 다중 트랜잭션 핸들 + 인터리브 데모 (reader가 writer를 안 막는 걸 진짜로 시연)
- [ ] 4. 쓰기-쓰기 충돌(first-updater-wins) + VACUUM(죽은 버전·인덱스 항목 회수)

### 그 밖에 (작은~중간)
- [ ] B+Tree 삭제 (현재 삭제 행은 힙에서 tombstone, 인덱스 항목은 방치)
- [ ] 버퍼 풀을 넘는 큰 트랜잭션 (no-steal + WAL_MAX_STAGED 한계)
- [ ] 복합·커버링 인덱스 / 인덱스 온리 스캔
- [ ] 다중 컬럼 `GROUP BY`
- [ ] 상관(correlated) 서브쿼리
- [ ] 괄호로 묶은 WHERE, 다중 조건 `ON`
- [ ] `DEFAULT` 값, `UPDATE ... SET col = NULL`
