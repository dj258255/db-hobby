# minidb 설계 — 진짜 RDBMS를 밑바닥부터 해부하기

PostgreSQL / MySQL 같은 관계형 데이터베이스가 내부에서 어떻게 동작하는지를,
직접 한 겹씩 구현하며 이해하는 학습 프로젝트다. 새로운 걸 발명하려는 게 아니라,
이미 있는 것의 **기본 구조를 정확히 재현**하는 게 목표다.

## 계층 구조 (위 → 아래)

```
 SQL 텍스트
   │
   ▼ [1] Parser      토크나이저 + 파서 → AST
   ▼ [2] Planner     AST → 실행 계획 (풀스캔 vs 인덱스)
   ▼ [3] Executor    계획 실행 (Volcano/iterator 모델)
   │
   ▼ [4] Catalog     시스템 테이블 (스키마 메타데이터)        ← pg_catalog
   │
   ▼ [5] Access      Heap(테이블) + B-Tree(인덱스)
   ▼ [6] Buffer Pool 페이지 캐시 + 교체(LRU)                  ← InnoDB buffer pool
   ▼ [7] Page        슬롯 페이지: 행을 고정 크기 페이지에 패킹
   ▼ [8] Pager/Disk  페이지 ↔ 단일 파일 (고정 크기 I/O)
   │
   ▼ [9] WAL/Txn     쓰기 선행 로그 + MVCC/락 (고급)
```

핵심 사실: **모든 것은 고정 크기 페이지 위에 쌓인다.** (Postgres 8KB, MySQL InnoDB
16KB, SQLite 4KB.) 디스크는 페이지 단위로만 읽고 쓰며, 그 위에 슬롯 페이지 →
힙/B-Tree → 버퍼풀 → 실행기가 얹힌다. 그래서 맨 아래(Pager)부터 짓는다.

## 빌드 순서 (bottom-up)

| # | 단계 | 무엇을 배우나 | 실제 DB 대응 |
|---|---|---|---|
| 1 | **Pager / Disk Manager** | 페이지를 파일에 어떻게 읽고 쓰나 | SQLite pager, PG smgr |
| 2 | **Slotted Page** | 가변 길이 행을 고정 페이지에 패킹 | PG/InnoDB 페이지 레이아웃 |
| 3 | **Buffer Pool** | 페이지 캐시 + LRU 교체 | InnoDB buffer pool |
| 4 | **Heap File** | 테이블 = 페이지 모음, tuple 삽입/스캔 | PG heap |
| 5 | **Catalog** | 스키마를 어디에 저장하나 | pg_catalog, information_schema |
| 6 | **SQL Parser** | SQL 텍스트 → AST | 모든 DB의 프런트엔드 |
| 7 | **Executor** | CREATE/INSERT/SELECT/WHERE 실행 | Volcano 모델 |
| 8 | **B-Tree Index** | O(log n) 조회, 인덱스 스캔 | InnoDB clustered index |
| 9 | **WAL / 트랜잭션** | 내구성·복구·동시성(MVCC/락) | PG WAL, InnoDB redo log |

목표 최종 모습:
```sql
CREATE TABLE users (id INT, name TEXT);
INSERT INTO users VALUES (1, 'kim');
SELECT * FROM users WHERE id = 1;
```
→ 디스크 파일 하나에 저장되고, 프로그램을 껐다 켜도 데이터가 남는다.

## 설계 원칙

- **단순함 우선.** 동시성·네트워크·완전한 SQL은 뒤로 미룬다. 핵심 구조부터.
- **각 계층은 테스트로 검증.** "동작한다고 주장"이 아니라 테스트로 증명.
- **실제 DB와 비교 주석.** 우리가 단순화한 부분에 "PG/MySQL은 여기서 ~한다"를 남긴다.
