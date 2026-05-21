# db-hobby 로드맵 (트랙별)

진행 상황 한눈에. 블로그 시리즈(1~12편)와 그 뒤 추가분을 추적한다.
세부 한계는 `README.md`의 "Scope", 구조는 `DESIGN.md` 참고.

현재: **테스트 499개 / 32스위트 통과.**

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
MVCC 토대(13편)·DELETE=xmax(16편)·**VACUUM(17편)까지 완성** — 단일 트랜잭션 기준 "미니 PostgreSQL"의
저장·복구·격리 축이 다 섰다. 남은 프론티어는 **진짜 다중 트랜잭션**(A1-3·4). (2PL vs MVCC 대조는 12편.)

### A1. MVCC (스냅샷 격리) — **완성(18편).** 13편이 남긴 프론티어를 전부 통과
> 1~2b(13편) -> 트랙 E(14·15편)가 전제(steal·abort롤백·로그 복구)를 깔고 -> 2c(16편) ->
> 3·4(18편). "reader가 writer를 안 막는다"가 시연이 아니라 **실행**이 됐다.
- [x] **1. 트랜잭션 상태 로그 + 가시성 규칙** (mvcc.c, standalone) — "xmin 커밋 AND xmax 미커밋이면 보임". abort된 INSERT/DELETE/UPDATE가 상태만으로 롤백되는 것까지 test_mvcc로 검증
- [x] **2a. 행 codec에 xmin/xmax 헤더 + INSERT/UPDATE가 xmin 기록 + TxnLog를 트랜잭션 생명주기(BEGIN/COMMIT/ROLLBACK·autocommit)에 연결.** 실제 힙 행 가시성을 test_mvcc_store로 증명(txn 아보트하면 그 행이 안 보임). 헤더는 SELECT 출력에 투명(무회귀)
- [x] **2b. MVCC 가시성 게이트를 SELECT* 읽기 경로에 + next_txn 영속화(committed_below)** — select_visit이 row_visible(xmin/xmax, my_txn)로 거른다. db_close가 next_txn 저장 -> 재오픈 시 그 미만 txn=커밋으로 봐 옛 행이 보임(no-steal라 디스크엔 커밋분만). 닫고 다시 열어 SELECT가 옛 행을 보이는 것 test로 증명. 경합 없으면 무회귀
- [x] **2c(대부분). DELETE를 tombstone -> xmax로 + 읽기 경로 게이트 통일 (16편)** — DELETE/UPDATE 옛 버전이 xmax를 받고(heap_overwrite 제자리 헤더 갱신), 힙을 읽는 모든 경로(풀스캔·PK 점/범위·보조 인덱스·조인 3방식·집계·정렬·서브쿼리·DML 수집·인덱스 빌드)가 rec_visible 게이트를 지난다. DELETE ROLLBACK -> 행이 되살아남(test_mvcc_dml). 죽은 버전 누적 = VACUUM(A2)의 동기. ※ '트랜잭션 시작 스냅샷'은 남음 — 동시성(3) 없인 관찰 불가라 그때 함께
- [x] **3. 다중 트랜잭션 핸들 + 인터리브 + 시작 스냅샷 (18편)** — 세션(`SESSION n`, 최대 8)마다 트랜잭션 핸들. **읽기는 락을 안 잡는다**(11편 S락 제거) — reader는 writer의 미커밋을 '가시성으로' 못 볼 뿐 막히지 않는다. BEGIN 시점 스냅샷(snap_next + 진행 중 목록)으로 REPEATABLE READ; 트랜잭션 밖은 read committed. PK 인덱스를 다중 버전화(버전마다 항목, 조회가 보이는 버전 선택). 테이블별 WAL은 X락 덕에 여전히 단일 writer — 14·15편 복구 무수정 성립. db_close는 열린 트랜잭션을 롤백(진짜 DB의 커넥션 끊김과 동일)
- [x] **4. 쓰기-쓰기 충돌 = first-updater-wins (18편)** — 테이블 X락(strict 2PL)이라 같은 테이블의 두 번째 writer는 즉시 거부. ※ PG는 행 단위지만 원리는 동일(테이블 granularity로 정직하게 명시)

### A2. VACUUM (죽은 공간 회수) — 도달(17편)
16편(DELETE=xmax)이 쌓은 죽은 버전을 치운다. SQL `VACUUM [<table>]`, 트랜잭션 안에서는 거부(PG 동일).
- [x] **죽은 튜플 힙 공간 회수** — 슬롯 비움 + 페이지 compaction(슬롯 번호=RID 불변) + 꼬리의 전부-빈 페이지 조건부 truncate(PG와 같은 결). 힙·인덱스 청소는 WAL로 원자 커밋
- [x] **죽은 인덱스 항목 제거 = B+Tree 키 삭제(lazy)** — btree_delete_val이 리프에서 (키,RID) 짝을 제거. 병합·재분배는 안 함 — PostgreSQL nbtree도 리프 항목 삭제 + 빈 페이지 재활용만 하지 재분배는 안 한다(정직한 선택). PK 항목은 살아있는 새 버전(UPDATE/재삽입)을 가리키면 보존

---

## 트랙 B — MySQL/InnoDB 대조 (트랙 A 완성 뒤 별도 챕터)

별도 프로젝트를 또 만들지 않는다. db-hobby 안에 InnoDB식 선택지를 **다른 모드**로 더해,
한 코드에서 PG식 vs MySQL식을 나란히 비교한다(블로그 1편 힙 vs 클러스터드, 4편 append-only vs undo의 코드판).
- [x] **B1. 클러스터드 vs 힙 접근 경로 대조 (23편)** — 같은 데이터를 힙(PG식: 인덱스->RID->힙)과 클러스터드(InnoDB식: 데이터가 PK 리프에, 보조는 PK를 듦)로 만들어 `make bench-clustered`로 비교. 실측: PK 점 조회 1.2배·PK 범위 3.8배 클러스터드 유리(지역성), 보조 점 조회는 2배 느림(InnoDB 이중 조회 sec->PK->데이터). test_clustered로 세 경로 정확성 검증. ※ 벤치·비교 모듈로 구현(엔진에 CLUSTERED 모드로 배선은 안 함 — 회귀 위험 회피, 정직한 선택)
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

19편(트랙 G)의 한계 — 단일 스레드 poll = 진짜 병렬 아님 — 를 여기서 뚫는다. CMU 15-445 P1/P2/P4 깊이.
- [x] **D1. 버퍼 풀 스레드 안전 (20편)** — 풀별 latch가 프레임 메타데이터를 보호, 반환된 페이지 데이터는 pin이 보호(pin>0이면 축출 안 됨 = pin 프로토콜). I/O도 latch 안(직렬화되나 정확). pthread 스트레스 테스트(축출 폭풍 속 교차 오염 0·pin 누수 0·디스크 무결성) + **ThreadSanitizer 클린**(`make test-tsan`)으로 증명
- [x] **D4(부분). 커넥션당 스레드 서버 (20편)** — 19편의 poll 루프를 커넥션당 OS 스레드로. 실행은 전역 엔진 latch로 직렬화(정확성 우선) — 버퍼 풀 latch가 이 굵은 latch를 계층별로 걷어낼 첫 발판. 실제 psql 두 개가 진짜 스레드로 인터리브
- [x] **D2. B+Tree latch crabbing (22편)** — 독립 모듈 cbtree(인메모리, 노드별 rwlock). 탐색은 읽기 crabbing(자식 잡고 부모 놓기), 삽입은 쓰기 crabbing(자식이 안전하면 조상 전부 해제, 분할은 붙든 조상으로 전파). header 노드 트릭으로 루트 분할 처리. 동시 삽입(분할 폭풍)+읽기/쓰기 혼합 스트레스 + **ThreadSanitizer 클린**. ※ 엔진 btree.c엔 아직 미배선 — 굵은 엔진 latch를 걷어낼 때 이 기법으로
- [ ] **D3. 진짜 블로킹 락 매니저** — 지금은 충돌 즉시 거부(first-updater-wins). 대기 큐 + 조건변수로 block, 해제 때 깨움. wait-for 교착 탐지는 **이미 있음**. ※ 전역 엔진 latch를 걷어낸 뒤에야 의미 있음(지금은 실행이 직렬이라 블로킹=데드락)

---

## 트랙 E — 복구를 제대로 (redo-only no-steal -> ARIES)

**steal + undo(14편), no-force + 단순 체크포인트(15편) 도달.** 커밋 전 dirty page를 before-image
로깅 후 방출(steal)하고, 커밋은 로그 fsync 하나가 유일한 내구성 지점(no-force) — 로그가 진실의
원천이 됐다(커밋 시 truncate 안 함). 복구는 다중 트랜잭션 로그를 커밋 구간별 redo + 꼬리 loser
undo로 처리. 남은 건 CLR·퍼지 체크포인트·3-패스 정식화(16편).
- [x] **E1. WAL rule + steal(14편) + no-force(15편)** — 커밋 = after-image+마커 로그 fsync만. 페이지는 fsync 없이 write-back. LSN 인프라(next/flushed_lsn) 도입. ※ pageLSN은 도입 안 함 — 페이지 전체 물리 로깅이라 redo가 idempotent해 불필요(physiological 로깅으로 갈 때 필요해짐, 정직한 생략)
- [x] **E2(부분). UNDO 로깅** — steal한 미커밋 변경을 before-image로 되돌림(롤백·크래시 복구 공통). first-write-wins로 페이지당 undo 1회. 롤백은 로그의 자기 트랜잭션 구간(txn_log_start~)만 undo. CLR은 남음
- [x] **E3(부분). 단순 체크포인트** — 로그가 임계(4MB)를 넘으면 커밋 끝에 데이터 fsync 후 로그 truncate. 재오픈 복구의 끝도 체크포인트로 동작. 퍼지(dirty page table + active txn 스냅샷)는 아님
- [x] **E 매듭(16편) — CLR·퍼지·3-패스는 '이 엔진에선 불필요'를 증명하고 닫음.** CLR이 지키는 성질(undo 중 재크래시 수렴)은 페이지 전체 물리 로깅의 idempotent undo가 공짜로 준다 — 크래시 주입 테스트(test_wal)로 증명. 퍼지 체크포인트는 동시 운영이, 3-패스 Analysis는 다중 loser가 전제 — 각각 트랙 D/A1-3과 physiological 로깅이 등장할 때 '필요해서' 들어온다(화물숭배 금지 원칙)
- [ ] **E3. 퍼지 체크포인트** — dirty page table + active txn table 스냅샷을 로그에 찍어 복구 시작점을 앞당김
- [ ] **E4. 3-패스 복구(Analysis -> Redo -> Undo)** — 크래시 후 정확히 ARIES로 복원. 지금의 redo/discard보다 훨씬 현실적
  - ※ 이걸 하면 트랙 A1의 MVCC 재작성(steal + abort 롤백)이 자연히 풀린다 — E와 A는 한 몸.

---

## 트랙 F — 옵티마이저 (규칙 기반 -> 비용 기반) — **단일 테이블(21편) + 조인 순서 계획기(24편)**

규칙 기반은 "PK 조건이면 무조건 인덱스"였다 — 넓은 범위엔 행마다 랜덤 힙 페치라 순차보다 비싸다.
ANALYZE로 통계를 재고 선택도로 매칭 행 수를 추정해 비용으로 고른다. 조인 순서(F3)는 24편에서 Selinger DP 계획기로 구현(실행기 배선은 프론티어).
- [x] **F1. 통계 수집(ANALYZE)** — `ANALYZE [<table>]`가 보이는 행 수·힙 페이지 수·PK[min,max]를 재 카탈로그에 영속화(재오픈 유지). 죽은 버전 제외(rec_visible)
- [x] **F2. 카디널리티 추정 + 비용 선택** — PK 범위 선택도 = 범위가 [min,max]에서 차지하는 비율 -> 매칭 행 수. 비용: 순차=페이지 수, 인덱스 범위=1+매칭행수(랜덤 페치). 싼 쪽 선택("페치할 행 > 페이지 수면 순차가 이긴다" = random_page_cost 직관). exec_select·explain_single이 choose_pk_range로 결정 공유
- [x] **F4. EXPLAIN에 추정 비용/행 수** — `rows=.. cost=..` 표시. 비용상 순차를 고른 자리엔 `[비용 기반: 인덱스보다 쌈]`
- [~] **F3. 비용 기반 조인 순서(24편)** — System R식 부분집합 DP(`src/joinopt.c`)로 N-way 조인 순서+방법 조합 비용 최소화. `dp[S]`를 비트마스크로 bottom-up, 교차곱 회피(연결성)+독립 가정 카디널리티. **순수 계획기까지 완료**(test_joinopt). 실행기 배선(순서 재정렬·ON 재매핑·LEFT JOIN 순서 제약)과 히스토그램은 프론티어로 남김

---

## 트랙 G — 클라이언트/서버 (REPL -> 네트워크 DB) — **도달(19편)**

`./build/db-hobby db.db --serve 5433` -> **진짜 `psql`이 붙는다**(검증: psql 14.19). 단일 스레드 `poll()`
이벤트 루프, 커넥션 = 18편의 세션(커넥션당 트랜잭션 핸들). psql 두 개로 "reader가 writer를 안 막는다"를
네트워크 너머에서 시연. 그 한계(이벤트 루프 = 진짜 병렬 아님, 느린 클라가 서버를 막음)가 곧 트랙 D의 장애 서사다.
- [x] **G1. TCP 서버 + 커넥션당 세션** — `poll()` 루프, 커넥션↔세션 매핑, 끊김 시 열린 트랜잭션 롤백
- [x] **G2. PostgreSQL wire protocol(v3)** — startup·SSLRequest 거절·simple query(Q)·Terminate, RowDescription/DataRow(전 컬럼 text)·CommandComplete·ErrorResponse·ReadyForQuery(txn 상태 반영). EXPLAIN은 QUERY PLAN 컬럼으로
- [ ] **G3. (선택) extended query(Parse/Bind/Execute)** — 미지원(psql 기본 대화는 simple query라 불필요). SELECT는 실행기 텍스트 출력 파싱이라 TEXT에 " | " 들어가면 컬럼 갈림(알려진 한계)

---

## 트랙 H — 분산 (단일 노드 -> 다중 노드) — **read replica 도달(25편)**

db-hobby는 전부 단일 노드다. 여기서 축이 완전히 바뀐다(MIT 6.824의 영역).
**이미 WAL이 있으니 복제의 출발점이 자연스럽다 — 로그를 다른 노드로 보내면 그게 복제다.**
- [~] **H1. Primary-Replica 복제(log shipping)(25편) + TCP 전송(26편)** — primary의 WAL을 replica가 tail하며 커밋된 after-image를 redo로 재적용(`src/replica.c`). redo가 idempotent라 lsn 필터로 체크포인트 truncate에도 견딤(test_replica, 18). 26편: `src/replnet.c`가 그 WAL 바이트를 소켓으로 실어 나른다(walsender/walreceiver, [길이][바이트] 프레임, replica.c 미수정) — 25편의 '네트워크 없음' 프론티어를 닫음(test_replnet, 15). **비동기 네트워크 read replica까지 완료**. 복제 슬롯·동기 커밋(quorum ack)·재연결은 프론티어
- [ ] **H2. 합의(Raft)** — 리더 선출 + 로그 복제 + 안전성. 노드 장애/네트워크 분단에도 일관. (6.824 Raft를 네 엔진 위에서)
- [ ] **H3. 샤딩** — 키 범위/해시로 파티션 + 라우팅. (6.824 Sharded KV의 결)
  - ※ H는 사실상 별개 프로젝트 규모. 트랙 G(네트워크) 이후에나 현실적.

---

## 트랙 I — 대체 스토리지 엔진 (B-Tree <-> LSM) — **LSM 코어 도달(27편)**

트랙 B가 "PG 힙 vs InnoDB 클러스터드"를 한 코드에서 대조하듯, 여기선 "B-Tree vs LSM-Tree"를 대조한다. RocksDB/LevelDB 내부.
- [~] **I1. LSM 스토리지 모드(27편)** — `src/lsm.c`: memtable(인메모리 정렬) + 임계치 flush로 불변 SSTable 생성. 제자리 갱신 없음, 삭제는 tombstone. read path는 memtable->최신 SSTable 순(read amp). 독립 모듈(test_lsm, 24). db.c 저장계층 배선은 프론티어
- [~] **I2. Compaction(27편)** — 모든 SSTable을 하나로 merge, 키당 최신만 남기고 tombstone 청소, SSTable 개수 축소. leveled/tiered 계층화는 프론티어
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
