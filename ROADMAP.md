# db-hobby 로드맵 (트랙별)

진행 상황 한눈에. 블로그 시리즈(1~12편)와 그 뒤 추가분을 추적한다.
세부 한계는 `README.md`의 "Scope", 구조는 `DESIGN.md` 참고.

현재: **테스트 338개 / 21스위트 통과.**

> 저장 철학에 따라 갈리는 일을 나눈다 — **A: PostgreSQL식**(현재 정체성), **B: MySQL/InnoDB 대조**,
> 저장과 무관한 SQL 마무리는 **C**. 공통 핵심은 이미 다 만들었다(Done).
> 그 위로 "새로 공부되는 축"을 더 얹었다 — **D: 진짜 멀티스레드 동시성**, **E: ARIES 복구**,
> **F: 비용 기반 옵티마이저**, **G: 클라이언트/서버(psql 접속)**, **H: 분산(복제·Raft·샤딩)**, **I: LSM 엔진**.
> A~C가 "지금 걸 완성", D~I가 "단일 노드 DB 마스터 이후의 새 지평". 상세 순서는 맨 아래 **추천 순서** 참고.

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

db-hobby는 이미 PG식이다 — 힙 + 별도 인덱스(RID), relfilenode, dead tuple, UPDATE=새 버전, EXPLAIN 용어.
남은 건 **격리를 잠금(2PL)에서 버전(MVCC)으로** 끌어올리고, 그 부산물인 죽은 공간을 **VACUUM**으로 청소하는 것.
이 둘을 하면 "진짜 미니 PostgreSQL"이 완성된다. (자세한 2PL vs MVCC 대조는 12편.)

### A1. MVCC (스냅샷 격리) — 마일스톤 도달 (2c~4는 코어 재작성 프론티어)
> 1~2b까지 안전하게 심었다(버전·가시성 게이트·영속화, 무회귀). 그 너머(2c DELETE->xmax, 3 진짜 동시성,
> 4 쓰기충돌)는 no-steal/WAL-truncate/tombstone/단일 트랜잭션 코어를 steal+abort롤백+다중 트랜잭션으로
> 갈아엎어야 해서 안정된 엔진을 흔드는 큰 재작성이다. 여기까지를 13편으로 정리.
- [x] **1. 트랜잭션 상태 로그 + 가시성 규칙** (mvcc.c, standalone) — "xmin 커밋 AND xmax 미커밋이면 보임". abort된 INSERT/DELETE/UPDATE가 상태만으로 롤백되는 것까지 test_mvcc로 검증
- [x] **2a. 행 codec에 xmin/xmax 헤더 + INSERT/UPDATE가 xmin 기록 + TxnLog를 트랜잭션 생명주기(BEGIN/COMMIT/ROLLBACK·autocommit)에 연결.** 실제 힙 행 가시성을 test_mvcc_store로 증명(txn 아보트하면 그 행이 안 보임). 헤더는 SELECT 출력에 투명(무회귀)
- [x] **2b. MVCC 가시성 게이트를 SELECT* 읽기 경로에 + next_txn 영속화(committed_below)** — select_visit이 row_visible(xmin/xmax, my_txn)로 거른다. db_close가 next_txn 저장 -> 재오픈 시 그 미만 txn=커밋으로 봐 옛 행이 보임(no-steal라 디스크엔 커밋분만). 닫고 다시 열어 SELECT가 옛 행을 보이는 것 test로 증명. 경합 없으면 무회귀
- [ ] 2c. DELETE를 tombstone -> xmax로 + 나머지 읽기 경로(인덱스·조인·집계·정렬)도 게이트 통일 + 트랜잭션 시작 스냅샷(지금은 read-committed식)
- [ ] 3. 다중 트랜잭션 핸들 + 인터리브 데모 (reader가 writer를 안 막는 걸 진짜로 시연)
- [ ] 4. 쓰기-쓰기 충돌(first-updater-wins)

### A2. VACUUM (죽은 공간 회수) — MVCC의 짝
MVCC는 일부러 옛 버전(dead tuple)을 쌓으니, 안 치우면 테이블·인덱스가 부푼다(bloat). 그걸 청소.
- [ ] 죽은 튜플(tombstone/안 보이는 옛 버전)의 힙 공간 회수
- [ ] 죽은 인덱스 항목 제거 = **B+Tree 키 삭제**(삽입의 반대: 노드 병합·재분배)
  - ※ B+Tree 자체와 범위 스캔은 **이미 있음**. 이건 "키를 지우는 연산"으로, 오직 공간 회수용이다.

---

## 트랙 B — MySQL/InnoDB 대조 (트랙 A 완성 뒤 별도 챕터)

별도 프로젝트를 또 만들지 않는다. db-hobby 안에 InnoDB식 선택지를 **다른 모드**로 더해,
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

## 트랙 D — 동시성을 진짜로 (단일 스레드 -> 멀티 스레드)

지금 엔진은 단일 스레드다 — 락은 "경쟁 락 주입"으로 시연할 뿐 진짜 스레드 경쟁은 없다(README Scope).
여기서 진짜 스레드를 켠다. CMU 15-445 Project 1/2/4가 적대적 멀티스레드로 채점하는 바로 그 깊이 —
**단일 노드 DB 내부에서 db-hobby에 없는 가장 큰 조각.**
- [ ] **D1. 버퍼 풀 스레드 안전** — 프레임 latch(페이지별) + pin count 원자화 + 교체(LRU) 임계구역. 여러 스레드가 같은 페이지를 동시에 잡아도 안 깨지게
- [ ] **D2. B+Tree latch crabbing(lock coupling)** — 루트->리프로 내려가며 자식 latch를 잡고 안전하면 부모 latch를 놓기, 분할이 조상까지 안 번지면 조상 latch 조기 해제. 읽기/쓰기 latch 구분
- [ ] **D3. 진짜 블로킹 락 매니저** — 지금은 충돌을 즉시 거부. 이걸 대기 큐 + 조건변수로 바꿔 conflict면 block, 락 해제 때 깨움. wait-for 그래프 교착 탐지는 **이미 있음** -> 진짜 대기 상황에 연결
- [ ] **D4. 다중 커넥션 동시 트랜잭션** — 스레드별 트랜잭션 핸들(트랙 A3와 만남), 인터리브를 주입이 아니라 진짜 스레드로 시연

---

## 트랙 E — 복구를 제대로 (redo-only no-steal -> ARIES)

**steal + undo(14편), no-force + 단순 체크포인트(15편) 도달.** 커밋 전 dirty page를 before-image
로깅 후 방출(steal)하고, 커밋은 로그 fsync 하나가 유일한 내구성 지점(no-force) — 로그가 진실의
원천이 됐다(커밋 시 truncate 안 함). 복구는 다중 트랜잭션 로그를 커밋 구간별 redo + 꼬리 loser
undo로 처리. 남은 건 CLR·퍼지 체크포인트·3-패스 정식화(16편).
- [x] **E1. WAL rule + steal(14편) + no-force(15편)** — 커밋 = after-image+마커 로그 fsync만. 페이지는 fsync 없이 write-back. LSN 인프라(next/flushed_lsn) 도입. ※ pageLSN은 도입 안 함 — 페이지 전체 물리 로깅이라 redo가 idempotent해 불필요(physiological 로깅으로 갈 때 필요해짐, 정직한 생략)
- [x] **E2(부분). UNDO 로깅** — steal한 미커밋 변경을 before-image로 되돌림(롤백·크래시 복구 공통). first-write-wins로 페이지당 undo 1회. 롤백은 로그의 자기 트랜잭션 구간(txn_log_start~)만 undo. CLR은 남음
- [x] **E3(부분). 단순 체크포인트** — 로그가 임계(4MB)를 넘으면 커밋 끝에 데이터 fsync 후 로그 truncate. 재오픈 복구의 끝도 체크포인트로 동작. 퍼지(dirty page table + active txn 스냅샷)는 아님
- [ ] **E 마무리(16편)** — CLR(보상 로그, undo 중 재크래시 안전), 퍼지 체크포인트, 3-패스(Analysis -> Redo -> Undo) 정식화
- [ ] **E3. 퍼지 체크포인트** — dirty page table + active txn table 스냅샷을 로그에 찍어 복구 시작점을 앞당김
- [ ] **E4. 3-패스 복구(Analysis -> Redo -> Undo)** — 크래시 후 정확히 ARIES로 복원. 지금의 redo/discard보다 훨씬 현실적
  - ※ 이걸 하면 트랙 A1의 MVCC 재작성(steal + abort 롤백)이 자연히 풀린다 — E와 A는 한 몸.

---

## 트랙 F — 옵티마이저 (규칙 기반 -> 비용 기반)

지금 플래너는 규칙 기반(첫 컬럼 PK면 인덱스, 아니면 풀스캔; 조인 방법도 규칙 선택). 진짜 옵티마이저는 통계로 비용을 매겨 고른다.
- [ ] **F1. 통계 수집(ANALYZE)** — 테이블 행 수, 컬럼별 히스토그램/distinct 추정을 카탈로그에 영속화
- [ ] **F2. 카디널리티 추정** — WHERE 선택도(selectivity)로 결과 행 수 예측, 히스토그램 기반 range 추정
- [ ] **F3. 비용 기반 조인 순서** — System R식 DP로 N-way 조인의 순서 + 방법 조합 비용을 최소화. 지금 "선언 순서대로 왼쪽부터"를 진짜 탐색으로
- [ ] **F4. EXPLAIN에 추정 비용/행 수** — 진짜 PG처럼 `cost=.. rows=..` 표시

---

## 트랙 G — 클라이언트/서버 (REPL -> 네트워크 DB)

지금은 로컬 REPL 하나. 진짜 DB는 TCP로 여러 클라이언트를 받는다. **여기서 트랙 D(멀티스레드)가 진짜로 필요해진다** — 커넥션마다 세션.
- [ ] **G1. TCP 서버 + 커넥션당 세션** — accept 루프, 커넥션별 트랜잭션/락 컨텍스트
- [ ] **G2. PostgreSQL wire protocol(v3)** — startup, 쿼리(Q), RowDescription/DataRow, ReadyForQuery. **목표: 진짜 `psql`로 접속**. (프로토콜 스펙 정독 = CodeCrafters Redis의 RESP 정독과 같은 결)
- [ ] **G3. (선택) prepared statement / extended query** — Parse/Bind/Execute 흐름
  - ※ 포트폴리오 관점: "내가 C로 짠 DB에 `psql`이 그대로 붙는다"는 데모 하나가 면접에서 세다.

---

## 트랙 H — 분산 (단일 노드 -> 다중 노드)

db-hobby는 전부 단일 노드다. 여기서 축이 완전히 바뀐다(MIT 6.824의 영역).
**이미 WAL이 있으니 복제의 출발점이 자연스럽다 — 로그를 다른 노드로 보내면 그게 복제다.**
- [ ] **H1. Primary-Replica 복제(log shipping)** — primary의 WAL을 replica로 스트리밍, replica가 redo로 따라감. 동기/비동기 커밋 대조
- [ ] **H2. 합의(Raft)** — 리더 선출 + 로그 복제 + 안전성. 노드 장애/네트워크 분단에도 일관. (6.824 Raft를 네 엔진 위에서)
- [ ] **H3. 샤딩** — 키 범위/해시로 파티션 + 라우팅. (6.824 Sharded KV의 결)
  - ※ H는 사실상 별개 프로젝트 규모. 트랙 G(네트워크) 이후에나 현실적.

---

## 트랙 I — 대체 스토리지 엔진 (B-Tree <-> LSM)

트랙 B가 "PG 힙 vs InnoDB 클러스터드"를 한 코드에서 대조하듯, 여기선 "B-Tree vs LSM-Tree"를 대조한다. RocksDB/LevelDB 내부.
- [ ] **I1. LSM 스토리지 모드** — memtable(인메모리 정렬) + WAL + flush로 SSTable 생성
- [ ] **I2. Compaction** — leveled/tiered로 SSTable 병합, tombstone 청소
- [ ] **I3. Bloom filter** — SSTable별 존재 필터로 읽기 증폭 감소
- [ ] **I4. `make bench`로 B-Tree vs LSM** — 쓰기 많은/읽기 많은 워크로드에서 write/read amplification 비교

---

## 추천 순서

**(1) 지금 정체성 완성 — 최우선**
1. **트랙 A**: A1(MVCC) -> A2(VACUUM). "진짜 미니 PostgreSQL" 완성.
2. **트랙 B**(클러스터드 모드)로 MySQL 대조 — 힙 vs 클러스터드를 직접 벤치.

**(2) 깊이 — DB 코어를 교과서 수준으로**
3. **트랙 E**(ARIES) — 트랙 C의 "버퍼 풀 넘는 트랜잭션" 한계 + A1의 재작성 프론티어를 한 방에 푼다.
4. **트랙 D**(진짜 멀티스레드) — 단일 노드 DB 내부의 마지막 큰 조각. 15-445 P1/P4 깊이.
5. **트랙 F**(비용 기반 옵티마이저).

**(3) 새 축 — 이력서가 세지는 구간**
6. **트랙 G**(psql 붙는 서버) — 네트워크 축. 데모 임팩트 큼.
7. **트랙 H**(복제 -> Raft -> 샤딩) — 분산 축. 6.824를 네 엔진 위에서.

**(4) 대조 연구 — 사이사이**
8. **트랙 I**(LSM 엔진) — B-Tree vs LSM. · **트랙 C**(SQL 완성도)는 머리 식힐 때 하나씩.
