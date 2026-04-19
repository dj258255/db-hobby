# 14편 기획서 — no-steal의 벽을 넘어 ARIES steal로

> 상태: 기획(draft). 트랙 E(ROADMAP)의 E1~E2를 "장애 서사 + 무회귀 커밋"으로 쪼갠 실행 계획.
> 대상 독자: 포트폴리오 리뷰어(제너럴리스트 포함) + DB 내부 관심자.

---

## 0. 메타 — frontmatter 초안 (13편 포맷 맞춤)

```yaml
title: '버퍼 풀보다 큰 트랜잭션은 왜 못 도는가 — no-steal에서 ARIES steal로'
titleEn: 'Why a Transaction Bigger Than the Buffer Pool Fails — From No-Steal to ARIES Steal'
description: "db-hobby는 트랜잭션이 버퍼 풀(64페이지)보다 커지면 커밋조차 못 하고 터진다. 왜? redo-only 복구를 지탱하려고 걸어둔 no-steal 때문이다. dirty 페이지를 커밋 전에 디스크로 못 내리니(steal 금지) 전부 메모리에 쌓이고, WAL stage 상한에서 막힌다. 이 벽을 ARIES의 steal로 넘는다 — 그런데 steal을 켜는 순간 '디스크에 미커밋 변경이 남는' 새 문제가 생기고, 그래서 UNDO 로깅이 필연이 된다. pageLSN·WAL 규칙·before-image UNDO를 db-hobby 코드에 심어, kill -9로 트랜잭션 한복판을 죽여도 데이터가 커밋 지점까지 정확히 복구되는 것을 보인다."
descriptionEn: "In db-hobby, a transaction larger than the buffer pool (64 pages) can't even commit — it errors out. Why? Because of no-steal, the very policy that lets redo-only recovery work. Dirty pages can't be flushed before commit (steal forbidden), so they pile up in memory and hit the WAL stage limit. We break that wall with ARIES steal — but the moment steal is on, uncommitted changes can reach disk, which forces UNDO logging. We graft pageLSN, the WAL rule, and before-image UNDO into db-hobby code, and show that killing the process mid-transaction with kill -9 recovers the data exactly up to the commit point."
date: 2026-07-XX
tags: [C, Database Internals, Recovery, WAL, ARIES, Transaction, Crash Recovery, Learning]
category: study/db-hobby
coverImage: /uploads/project/db-hobby/cover.svg
draft: false
series: "db-hobby"
seriesOrder: 14
```

블로그 규칙 준수: ARIES/steal/force는 **db-hobby가 어떻게 하는지**로 서술하고, "모든 DB가 이렇다"로 일반화하지 않는다. 특히 page-level 물리 로깅은 db-hobby의 선택임을 명시(실무는 physiological 로깅).

---

## 1. 장애 서사 (도입부) — 지금 당장 재현되는 실패

### 1-1. 눈에 보이는 재현
```sql
CREATE TABLE big (id INT, payload TEXT);
BEGIN;
-- 페이지 64개를 넘겨 채운다 (payload를 크게, 행을 많이)
INSERT INTO big VALUES (1, '....'); ...  -- 수천 행
COMMIT;   -- ERROR: 버퍼 풀/스테이지 한계 초과
```
→ 트랜잭션이 **커밋조차 못 하고** 실패한다. "작은 건 되는데 큰 건 안 되는" 비대칭이 도입부의 후크.

### 1-2. 포트폴리오용 30초 데모 (편 후반 클라이맥스)
```
$ db-hobby demo.db
db-hobby> BEGIN;
db-hobby> INSERT ... (많이)
# 커밋 직전, 다른 터미널에서
$ kill -9 <pid>          # 트랜잭션 한복판에서 강제 종료
$ db-hobby demo.db       # 재오픈 → 복구
db-hobby> SELECT ...     # 커밋 안 된 변경은 흔적 없음 (원자성), 커밋분은 그대로 (내구성)
```
"죽였는데 안 깨진다"가 각인 포인트.

---

## 2. 근본 원인 — 아키텍처 진단 (file:line)

| 결속 고리 | 위치 | 내용 |
|---|---|---|
| 버퍼 풀 크기 = 스테이지 상한 | `src/db.c:163` | `bufpool_create(&t->wal.data, WAL_MAX_STAGED)` |
| 스테이지 상한 64 | `src/wal.h:23` | `#define WAL_MAX_STAGED 64` |
| 꽉 차면 실패 | `src/wal.c:149` | `if (w->num_staged >= WAL_MAX_STAGED) return -1;` |
| no-steal이 dirty 축출 금지 | `src/bufpool.c:70` | `if (bp->no_steal && f->dirty) { /* victim 제외 */ }` |
| 트랜잭션 중 no-steal ON | `src/db.c:2295` | `bufpool_set_no_steal(t->bp, 1)` (BEGIN) |
| B+Tree도 같은 이유로 상한 | `src/btree.c:41` | 주석: "no-steal이라 stage 상한 이하로" |
| redo-only가 성립하는 이유 | `src/wal.c:49` | "커밋된 페이지만 redo, 나머진 버림" — 디스크엔 커밋분만 있어서 |
| 명시된 한계 | `README.md:158` | "a transaction's dirty pages must fit in the buffer pool" |

**한 줄 인과:** redo-only 복구를 단순하게 유지하려고 no-steal을 걸었고 → no-steal이 dirty를 메모리에 가두니 → 트랜잭션이 버퍼 풀보다 크면 못 돈다.

---

## 3. 이번 편의 목표 & 범위 분할

### 14편 (이 문서)
- **STEAL 도입** + **before-image UNDO** + **pageLSN/WAL 규칙**으로 "버퍼 풀보다 큰 트랜잭션"을 돌리고, 크래시/롤백에 안전하게 만든다.
- 커밋 정책은 당분간 **force-at-commit 유지**(현재 `wal_commit` 3단계 그대로) — no-force는 15편.
  - 이유: STEAL만으로도 목표 장애(큰 트랜잭션)가 풀린다. NO-FORCE는 성능 최적화라 서사·리스크를 분리한다.

### 15편 (예고, 별도 기획)
- **NO-FORCE** + **퍼지 체크포인트** + **3-패스 복구(Analysis→Redo→Undo) 정식화**.
- 그리고 트랙 A1의 MVCC 재작성(steal + abort 롤백이 생기면 2c~4가 풀림)과 합류.

---

## 4. 설계 — ARIES 개념을 db-hobby에 매핑

db-hobby는 **페이지 전체를 물리 로깅**한다(REC_PAGE = 페이지 after-image). 이 성질을 살린 최소 ARIES:

| ARIES 개념 | db-hobby 구현(14편) | 정직한 단순화 |
|---|---|---|
| **LSN** | 로그 레코드 단조 증가 시퀀스 | 단일 스레드라 그냥 카운터 |
| **pageLSN** | 페이지 헤더에 마지막 변경 로그의 LSN 기록 | 페이지 예약 영역 8B 사용 |
| **WAL 규칙** | 페이지를 디스크에 쓰기 전, 그 pageLSN까지 로그를 fsync | steal 축출 직전에 강제 |
| **REDO** | after-image를 페이지에 재적용(이미 있음) | 물리 idempotent (pageLSN ≥ 레코드LSN이면 skip) |
| **UNDO** | 트랜잭션 최초 수정 시 잡아둔 **before-image**로 페이지 복원 | 물리 페이지 UNDO(=논리 아님) |
| **CLR(보상 로그)** | UNDO 적용을 로그에 남겨 복구 중 재크래시에도 진행 | 14편은 롤백/단일 크래시 위주, CLR은 최소 |
| **loser 트랜잭션** | 커밋 마커 없는 트랜잭션의 변경을 before-image로 되돌림 | 단일 트랜잭션이라 loser 집합이 단순 |

### 로그 레코드 포맷 변경 (`wal.c`)
현재: `REC_PAGE 'P'`(pid + after), `REC_COMMIT 'C'`.
추가:
- 모든 레코드에 `lsn`(uint64) 선두 필드.
- `REC_UPDATE`(대체 or 확장): `lsn, pid, before[PAGE_SIZE], after[PAGE_SIZE]` — undo/redo 양쪽 이미지.
  - 저장 2배가 부담이면: before-image는 트랜잭션당 **페이지별 첫 수정 때 한 번만** 별도 undo 로그에 기록(ARIES 방식). 14편은 단순함 우선으로 update 레코드에 둘 다 넣는 안을 기본으로, 최적화는 각주.
- `REC_COMMIT`: `lsn` + txn 종료.
- (15편) `REC_CHECKPOINT`, `REC_CLR`, `REC_END`.

---

## 5. 구현 단계 = 커밋 경계 (각 단계 = 초록불 유지)

> 규율: 단계마다 `make test`(현재 323개/20스위트) 초록불. 실패 테스트 단계(1)만 예외.

### 단계 0 — 안전망 고정
- `tests/test_recovery.c` 신설(또는 `test_wal.c` 확장). 기존 WAL 테스트가 회귀 감시로 남는지 확인.

### 단계 1 — 장애를 테스트로 박기 (실패가 정상)
- **파일:** `tests/test_wal.c` 또는 `tests/test_recovery.c`
- **내용:** 65페이지 이상을 건드리는 트랜잭션을 stage → `wal_stage`가 `-1` 반환하는 걸 **명시적으로** 검증(현재 동작 고정). 이게 도입부 재현.
- 커밋: `test: 버퍼 풀 초과 트랜잭션이 지금 실패함을 고정 (14편 장애 재현)`

### 단계 2 — pageLSN + WAL 규칙 (동작 불변, 인프라)
- **파일:** `src/page.h`(pageLSN 접근자), `src/wal.c`(lsn 카운터·레코드에 lsn), `src/wal.h`.
- **핵심:** 로그 레코드에 lsn 부여, 페이지 헤더에 pageLSN 기록, `pager_write` 경로에 "로그 먼저 flush" 훅. 여기까진 no-steal이라 실제 축출이 없어 **동작이 안 바뀜** → 무회귀 확인.
- 커밋: `wal: pageLSN + LSN 로그 시퀀스 + WAL 규칙 훅 (동작 불변)`

### 단계 3 — before-image 캡처 (UNDO 준비, 아직 steal OFF)
- **파일:** `src/wal.c`(stage 시 before-image 보관), `src/db.c`.
- **핵심:** `wal_stage`/커밋 경로가 페이지의 최초 수정 전 이미지를 잡아둔다. 아직 steal 안 하므로 사용처는 롤백뿐 → 기존 롤백(`bufpool_discard_dirty`)과 결과 동일해야 함(무회귀).
- 커밋: `wal: 트랜잭션 before-image 캡처 (undo 기반 마련)`

### 단계 4 — STEAL 켜기 (장애 해소 + 새 문제 노출)
- **파일:** `src/bufpool.c`(no-steal일 때도 dirty 축출 허용하되, 축출 전 `wal` 콜백으로 before/after 로그 + WAL 규칙 강제), `src/db.c:163`(버퍼 풀 크기와 스테이지 상한 분리), `src/wal.h:23`(상한 제거 또는 확대).
- **결과:** 단계 1의 큰 트랜잭션이 이제 **커밋 성공**. 단, 크래시 시 디스크에 미커밋 변경이 남을 수 있음 → 단계 5 없이는 크래시 복구가 깨짐(이 긴장을 글에서 드러냄).
- 테스트: 큰 트랜잭션 커밋 통과. + "미커밋 steal 후 크래시" 케이스는 **아직 FAIL 예상**으로 표시(다음 단계 예고).
- 커밋: `bufpool/wal: STEAL 허용 — 버퍼 풀보다 큰 트랜잭션 커밋 가능`

### 단계 5 — UNDO 복구 (긴장 해소)
- **파일:** `src/wal.c`(`wal_recover`를 redo-only → redo + undo로 확장), `tests/test_wal.c`.
- **핵심:** 복구 시 커밋 마커 없는 트랜잭션의 변경을 before-image로 되돌림. `wal_recover`(현 `wal.c:50`)를 "커밋된 것 redo + 미커밋 loser undo"로.
- **결과:** 단계 4에서 FAIL 표시했던 "미커밋 steal 후 크래시"가 통과. kill -9 데모 성립.
- 커밋: `wal: 크래시 복구에 UNDO 추가 — steal한 미커밋 변경을 before-image로 롤백`

### 단계 6 — kill -9 통합 데모 + 문서
- **파일:** `README.md`(Scope의 "dirty pages must fit in the buffer pool" 갱신), `ROADMAP.md`(E1/E2 체크), `tests/`(kill -9를 흉내내는 crash-injection 테스트).
- 커밋: `docs: Scope/ROADMAP 갱신 (E1 steal + E2 undo 도달) + 크래시 복구 데모`

---

## 6. 테스트 계획

| 테스트 | 검증 |
|---|---|
| 큰 트랜잭션 커밋 (65+ 페이지) | 단계 4 이후 통과 |
| steal 후 정상 커밋 → 재오픈 | 내구성 (redo) |
| steal 후 **커밋 전 크래시** → 재오픈 | 원자성 (undo) — 핵심 신규 |
| steal 후 **커밋 마커 직후 크래시** | redo가 마저 적용 |
| ROLLBACK (steal 발생분 포함) | before-image 복원 |
| 기존 323개 스위트 | 전부 무회귀 |
| B+Tree/보조 인덱스도 steal 경로 | 인덱스 WAL도 동일 보호 |

크래시 주입: 기존 `wal_test_crash_before_commit` / `wal_test_crash_after_log`(`wal.h:52`) 패턴을 재사용·확장.

---

## 7. 블로그 서사 구조 (문단 흐름)

1. **후크** — 작은 트랜잭션은 되는데 큰 트랜잭션은 커밋조차 못 하고 터진다(재현 로그).
2. **왜?** — no-steal 진단(코드 3중 결속 그림). "이건 버그가 아니라 redo-only를 지키려는 설계의 대가."
3. **교과서 축** — STEAL/FORCE 2×2 정책 표. db-hobby는 지금 (no-steal, force). ARIES는 (steal, no-force).
4. **1차 해결** — steal 켜기. 큰 트랜잭션 통과. "됐다!" ...그런데.
5. **새 장애** — steal하면 디스크에 미커밋이 샌다 → 크래시하면 오염. 왜 redo-only로는 못 고치나(되돌릴 원본이 없다).
6. **2차 해결** — before-image UNDO + WAL 규칙 + pageLSN. `wal_recover` 확장.
7. **클라이맥스** — kill -9 데모. 죽여도 커밋 지점까지 정확 복구.
8. **비교** — PostgreSQL/InnoDB의 steal+no-force, physiological 로깅과 db-hobby(page 물리 로깅)의 차이(정직한 단순화 명시).
9. **다음 편 예고** — no-force + 퍼지 체크포인트 + 3-패스 정식화, 그리고 MVCC 재작성 합류(A1).

SVG 후보(GitHub-dark 팔레트): ① STEAL/FORCE 2×2 정책 격자, ② no-steal 결속 3고리 다이어그램, ③ 복구 타임라인(로그·pageLSN·redo/undo 지점).

---

## 8. 리스크 & 무회귀 규율

- **가장 큰 리스크:** steal 경로가 B+Tree·보조 인덱스·힙 세 군데 WAL을 모두 건드림. → 단계 4를 힙만 먼저, 인덱스는 뒤에 나눌 수도.
- **저장 2배(before+after):** 14편은 명료성 우선. 각주로 "ARIES는 undo를 첫 수정 때 한 번만" 최적화 언급, 15편에서 개선.
- **force 유지:** no-force를 15편으로 미뤄 이번 편 리스크를 steal+undo로 한정.
- **각 단계 독립 커밋** — 언제 멈춰도 엔진이 초록불(프로젝트 정체성 "323개 통과" 유지).

---

## 9. 후속(15편) 예고 — no-force + 체크포인트 + 3-패스

- E1의 나머지(no-force), E3(퍼지 체크포인트), E4(Analysis→Redo→Undo 정식 3패스).
- 이게 완성되면 트랙 A1 MVCC의 "코어 재작성 프론티어"(steal + abort 롤백 필요)가 풀려 2c~4로 이어진다 — E와 A는 한 몸(ROADMAP:98).
