# db-hobby 설계 — 진짜 RDBMS를 밑바닥부터 해부하기

PostgreSQL / MySQL 같은 관계형 데이터베이스가 내부에서 어떻게 동작하는지를,
직접 한 겹씩 구현하며 이해하는 학습 프로젝트다. 새로운 걸 발명하려는 게 아니라,
이미 있는 것의 **기본 구조를 정확히 재현**하는 게 목표다.

## 계층 구조 (위 -> 아래)

```
 SQL 텍스트
   |
   v [1] Parser      토크나이저 + 파서 -> AST
   v [2] Planner     AST -> 실행 계획 (풀스캔 vs 인덱스)
   v [3] Executor    계획 실행 (Volcano/iterator 모델)
   |
   v [4] Catalog     시스템 테이블 (스키마 메타데이터)        <- pg_catalog
   |
   v [5] Access      Heap(테이블) + B-Tree(인덱스)
   v [6] Buffer Pool 페이지 캐시 + 교체(LRU)                  <- InnoDB buffer pool
   v [7] Page        슬롯 페이지: 행을 고정 크기 페이지에 패킹
   v [8] Pager/Disk  페이지 <-> 단일 파일 (고정 크기 I/O)
   |
   v [9] WAL/Txn     쓰기 선행 로그 + MVCC/락 (고급)
```

핵심 사실: **모든 것은 고정 크기 페이지 위에 쌓인다.** (Postgres 8KB, MySQL InnoDB
16KB, SQLite 4KB.) 디스크는 페이지 단위로만 읽고 쓰며, 그 위에 슬롯 페이지 ->
힙/B-Tree -> 버퍼풀 -> 실행기가 얹힌다. 그래서 맨 아래(Pager)부터 짓는다.

## 빌드 순서 (bottom-up)

| # | 단계 | 무엇을 배우나 | 실제 DB 대응 |
|---|---|---|---|
| 1 | **Pager / Disk Manager** | 페이지를 파일에 어떻게 읽고 쓰나 | SQLite pager, PG smgr |
| 2 | **Slotted Page** | 가변 길이 행을 고정 페이지에 패킹 | PG/InnoDB 페이지 레이아웃 |
| 3 | **Buffer Pool** | 페이지 캐시 + LRU 교체 | InnoDB buffer pool |
| 4 | **Heap File** | 테이블 = 페이지 모음, tuple 삽입/스캔 | PG heap |
| 5 | **Catalog** | 스키마를 어디에 저장하나 | pg_catalog, information_schema |
| 6 | **SQL Parser** | SQL 텍스트 -> AST | 모든 DB의 프런트엔드 |
| 7 | **Executor** | CREATE/INSERT/SELECT/WHERE 실행 | Volcano 모델 |
| 8 | **B-Tree Index** | O(log n) 조회, 인덱스 스캔 | InnoDB clustered index |
| 9 | **WAL / 트랜잭션** | 내구성·복구·동시성(MVCC/락) | PG WAL, InnoDB redo log |

목표 최종 모습:
```sql
CREATE TABLE users (id INT, name TEXT);
INSERT INTO users VALUES (1, 'kim');
SELECT * FROM users WHERE id = 1;
```
-> 디스크 파일 하나에 저장되고, 프로그램을 껐다 켜도 데이터가 남는다.

## 2단계 — 단일 노드를 넘어서

위 9단계로 "미니 PostgreSQL"의 저장·실행 뼈대가 선 뒤, 진짜 DB의 어려운 축들을
하나씩 얹었다. 핵심 서사는 **각 편이 앞 편이 만든 문제를 푼다** — steal이 undo를
부르고, DELETE=xmax가 VACUUM을 부르고, 단일 스레드 서버가 스레드 안전을 부른다.

| 축 | 무엇을 | 실제 DB 대응 | 편 |
|---|---|---|---|
| **복구를 제대로** | STEAL + no-force WAL, 로그가 진실의 원천 | PG WAL, ARIES | 14·15 |
| **MVCC 완성** | DELETE=xmax, VACUUM, 다중 트랜잭션 스냅샷 격리 | PG MVCC/VACUUM | 16·17·18 |
| **네트워크** | PostgreSQL v3 wire protocol 서버(진짜 psql이 붙음) | PG backend | 19 |
| **동시성** | 스레드 안전 버퍼 풀, latch crabbing B+Tree | InnoDB latch | 20·22 |
| **비용 옵티마이저** | ANALYZE 통계·선택도, Selinger 조인 순서 DP | System R planner | 21·24 |
| **저장 대조** | 힙(PG) vs 클러스터드(InnoDB), LSM(RocksDB) | 세 저장 철학 | 23·27 |
| **복제** | WAL 로그 시핑(replica가 redo 재생), TCP 전송 | PG streaming replication | 25·26 |
| **합의(Raft)** | 리더 선출·로그 복제·안전성·지속성·스냅샷 | etcd/Consul Raft | 28·29·30 |

정직한 경계: 22·24·27·28~30편의 고급 축은 400+ green 테스트를 지키려 **독립
모듈**로 세우고, 각 편이 "여기까지, 그 너머는 이런 이유로"라고 통합 지점을 명시했다.
전체 지도는 블로그 시리즈 0편(전체 지도) 참고.

## 설계 원칙

- **핵심 구조부터, 그다음 어려운 축.** 1단계에선 동시성·네트워크·분산을 뒤로
  미뤄 저장·실행 뼈대를 세우고, 2단계에서 그것들을 정면으로 하나씩 뚫는다.
- **각 계층은 테스트로 검증.** "동작한다고 주장"이 아니라 테스트로 증명(현재
  557개 / 33스위트, 동시성은 ThreadSanitizer로).
- **실제 DB와 비교 주석.** 우리가 단순화한 부분에 "PG/MySQL은 여기서 ~한다"를 남긴다.
- **화물숭배 금지.** 기법은 그게 필요해지는 문제가 나타날 때 들여온다(예: CLR·퍼지
  체크포인트는 '이 엔진에선 불필요'를 증명하고 닫음). 무엇을 안 했는지도 명시한다.
