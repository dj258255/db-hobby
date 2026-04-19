# 15편 기획서 — FORCE를 버리고 WAL을 진실의 원천으로 (no-force + 체크포인트 + 3-패스 ARIES)

> 상태: **15(a) 구현 완료.** 두 편 분할 확정 — 15편 = no-force + 상시 redo + 단순 체크포인트(코드 완료),
> 16편 = CLR + 퍼지 체크포인트 + 3-패스 정식화.
> 전제: 14편에서 steal + before-image undo + redo/undo 2-패스까지 완료(`docs/post-14-aries-plan.md`).

## ✅ 구현 확정 사항 (계획 대비 변경)

1. **pageLSN 생략 (페이지 포맷 변경 안 함).** 페이지 전체 물리 로깅에선 커밋 순서 redo가 idempotent라
   pageLSN이 정확성에 불필요. 14편 7절("pageLSN 없음 — redo가 idempotent")의 논리와 일관.
   pageLSN은 훗날 physiological 로깅 편에서 "진짜 필요해서" 등장시키는 게 정직한 서사.
   → 이로써 이 편의 최대 리스크(온디스크 포맷 변경·파일 비호환)가 사라짐.
2. **db-hobby식 no-force**: 커밋 = 로그 fsync 하나(유일한 내구성 지점). 페이지는 파일에 write-back하되
   **fsync하지 않고**(OS 캐시), 로그는 truncate하지 않음. "디스크(캐시) = 최신 커밋" 불변식이 유지돼
   steal의 capture-at-first-steal(디스크에서 before-image 읽기)이 그대로 정확. bufpool/트랜잭션
   choreography 무변경 — 최소 침습.
3. **롤백 스코프**: `txn_log_start`(wal_begin 시 로그 끝 오프셋) 이후만 undo하고 그 지점으로 로그
   truncate — 앞선 커밋 이력 보존.
4. **복구 = 다중 트랜잭션 2-패스**: pass1은 커밋 구간마다 after-image를 커밋 순서대로 redo(+마지막
   커밋 오프셋 추적), pass2는 그 뒤 꼬리(loser)만 스트리밍 undo + base truncate. 복구 끝 = 로그
   truncate = "여는 것 자체가 체크포인트".
5. **단순 체크포인트**: 커밋 끝에 로그 크기 > 4MB(`WAL_CHECKPOINT_BYTES`)면 데이터 fsync 후 로그
   truncate. 퍼지 아님(16편).
6. 테스트: test_wal에 다중 커밋 redo·롤백 이력 보존 시나리오 추가. **338개 / 21스위트 green.**

---

## 0. 메타 — frontmatter 초안 (14편 포맷 맞춤)

```yaml
title: 'FORCE를 버리다 — WAL을 진실의 원천으로 (no-force + 체크포인트 + 3-패스 ARIES)'
titleEn: 'Dropping FORCE — Making the WAL the Source of Truth (No-Force + Checkpoint + 3-Pass ARIES)'
date: 2026-07-XX
series: "db-hobby"
seriesOrder: 15
tags: [C, Database Internals, Recovery, WAL, ARIES, Checkpoint, PostgreSQL, Learning]
```

블로그 규칙: db-hobby는 여전히 **페이지 전체 물리 로깅 + 단일 트랜잭션**임을 명시. 진짜 ARIES(physiological·동시성)와의 경계를 14편 7절처럼 유지.

---

## 1. 장애/동기 — 지금은 커밋할 때마다 데이터 전체를 fsync한다 (FORCE)

14편 이후에도 커밋은 여전히 **FORCE**다(`wal_commit` 3단계: 로그+마커 → **데이터 페이지 적용 + `fsync(data)`** → 로그 truncate). 즉 커밋 때마다:
1. 로그를 fsync하고,
2. **모든 dirty 데이터 페이지를 디스크에 쓰고 또 fsync**한다.

두 번째가 비싸다. 커밋 지연이 dirty 페이지 수에 비례하고, 같은 페이지를 여러 트랜잭션이 자주 고쳐도 매번 디스크로 내려간다. 게다가 **커밋 시 로그를 truncate**하니, WAL이 "이미 일어난 일의 기록(source of truth)"이 아니라 그때그때 버려지는 임시 버퍼다 — 진짜 DB의 WAL 모델과 다르다.

### 측정으로 동기 만들기 (failure-first) — ⚠️ 현실 확인 완료
- 측정 결과(이 개발 하드웨어, macOS): **자동커밋 커밋당 ≈0.273 ms**(자동커밋 1건 = data.tbl + index.idx 각각 로그+파일 = 최대 4 fsync). 한 트랜잭션 500행 배치 = 총 0.015s.
- **결론: 성능 훅이 약하다.** no-force는 파일 fsync 2개를 없애 대략 절반으로 줄이는 정도지 극적이지 않다(macOS fsync는 F_FULLFSYNC가 아니라 저렴). 극적인 before/after 숫자를 기대하면 안 된다.

> **따라서 15편의 명분을 "속도"에서 "아키텍처"로 옮긴다.** 진짜 이유:
> ① **WAL을 source of truth로** — 커밋 시 로그만 내구화, 데이터는 게으르게. 진짜 DB의 durability 모델.
> ② **체크포인트로 복구 시간·로그 크기 bound** — 운영 가능한 복구.
> ③ **결정적으로, 13편이 남긴 "진짜 동시 MVCC"의 전제조건** — steal + abort-롤백 + 다중 트랜잭션 위에 no-force/redo가 얹혀야 스냅샷 격리가 성립. 15편은 그 프론티어로 가는 마지막 계단.
> 벤치 수치(≈0.27ms→절반)는 **보조 근거**로만 쓰고, 헤드라인은 아키텍처로.

## 2. no-force가 여는 문제들 (그래서 나머지가 필요하다)

no-force로 바꾸면 연쇄로 세 가지가 필요해진다.

1. **커밋된 변경이 디스크에 없을 수 있다** → 복구가 **로그에서 redo**로 앞으로 감아야 한다. (14편 redo는 "커밋 마커 뒤 크래시" 좁은 창만 커버했음. 이제 상시 필요.)
2. **로그를 커밋마다 못 자른다**(source of truth) → 로그가 무한정 커진다 → **체크포인트**로 "여기까지는 디스크에 반영됨"을 찍어 그 앞을 잘라야 한다.
3. **로그가 여러 트랜잭션분을 담는다** → redo가 "이 페이지에 이 로그가 이미 반영됐나"를 알아야 중복/역적용을 피한다 → **pageLSN** 필요.

## 3. 설계 — db-hobby 규모의 ARIES

| ARIES 요소 | 15편 구현 | 정직한 단순화 |
|---|---|---|
| **LSN** | 로그 파일 내 바이트 오프셋(또는 단조 카운터) | 단일 스레드라 단순 |
| **pageLSN** | 페이지 헤더에 8B — "이 페이지에 마지막으로 반영된 LSN" | 페이지 포맷 변경 |
| **WAL 규칙** | (이미 있음) 페이지 flush 전 그 pageLSN까지 로그 fsync | 14편 steal에서 절반 도입됨 |
| **no-force** | 커밋 = 로그+마커 fsync만. 데이터 적용·log truncate 안 함 | — |
| **REDO** | 체크포인트~로그끝을 훑어 `pageLSN < recLSN`인 페이지만 after-image 적용 | 페이지 물리 redo |
| **체크포인트** | dirty page table + (단일)active txn 스냅샷을 로그에 + dirty flush | fuzzy가 아니라 단순 체크포인트로 시작 가능 |
| **UNDO/CLR** | loser를 before-image로 되돌리며 CLR 기록(재크래시 안전) | 14편 undo에 CLR만 추가 |
| **3-패스** | Analysis(체크포인트에서 loser·dirty 파악) → Redo(역사 반복) → Undo(loser 되돌림) | 단일 트랜잭션이라 loser 집합이 작음 |

### 페이지 포맷 변경 (가장 큰 리스크)
- `SlotPageHeader`(현재 `num_slots`,`free_end` = 4B)에 `uint64_t page_lsn` 추가 → `HEADER_SIZE` 4→12, 슬롯 배열 시작 이동. 힙 페이지 온디스크 레이아웃 변경.
- `BTNode`(현재 `_pad2` uint32 여유 있음)에 `page_lsn` 추가.
- **호환성**: 학습 프로젝트라 옛 `.tbl`/`.idx`는 비호환으로 두고(README에 명시), 새로 만들게 한다. 또는 magic/version 바이트로 감지.
- 이 변경이 힙·B+Tree·슬롯 페이지 계층을 광범위하게 건드리므로, **단독 단계 + 무회귀 커밋**으로 격리한다.

## 4. 구현 단계 = 커밋 경계 (각 단계 green 유지)

> 규율: 14편과 동일. 단계마다 `make test` 초록불(측정 단계 제외).

1. **커밋 비용 벤치** — `bench_commit` 신설. FORCE 반복 커밋 지연 측정("before"). (동작 불변)
2. **LSN 인프라** — 로그 레코드에 LSN 부여, WAL이 "마지막 flush LSN" 추적. (동작 불변)
3. **pageLSN 페이지 포맷** — 슬롯 헤더 + BTNode에 `page_lsn` 추가, 변경 시 스탬프. 아직 no-force 아님 → 스탬프만, 동작 불변(무회귀 핵심 관문).
4. **no-force 커밋** — `wal_commit`에서 데이터 적용·log truncate 제거. 커밋=로그+마커 fsync만. dirty는 풀에 남거나 steal로만 디스크행. (여기서 크래시 복구가 깨짐 → 다음 단계 예고)
5. **redo 상시화** — 복구가 로그를 훑어 `pageLSN < recLSN`인 커밋 페이지를 redo. no-force로 안 내려간 커밋분 복원.
6. **체크포인트** — 주기적으로 dirty flush + 체크포인트 레코드 기록 + 그 앞 로그 truncate. 로그 무한성장·복구시간 bound.
7. **3-패스 + CLR** — Analysis→Redo→Undo 정식화, undo에 CLR 추가(재크래시 안전).
8. **벤치 재측정 + 문서** — 커밋 지연 "after", README Scope/ROADMAP E 갱신.

## 5. 테스트 계획
- 기존 331 + `test_recovery` 전 시나리오 무회귀(no-force 후에도 내구성·원자성 유지).
- 신규: no-force 커밋 후 **데이터 미적용 상태에서 크래시 → redo로 복원**.
- 체크포인트 후 크래시 → 체크포인트부터 복구(앞 로그 없어도 정확).
- CLR: undo 도중 재크래시 → 다시 열어도 정확히 마무리.
- 커밋 지연 before/after 수치(bench).

## 6. 서사 구조 (블로그)
1. 후크 — "커밋이 왜 느린가": FORCE가 매번 데이터 fsync (bench 수치).
2. no-force로 한 칸 이동 → 커밋 빨라짐. ...그런데 커밋분이 디스크에 없다.
3. 그래서 redo 상시화(pageLSN 등장) → 로그 무한성장 → 체크포인트.
4. 3-패스 ARIES로 정식화 + CLR.
5. before/after 벤치. 진짜 ARIES와의 경계(physiological·동시성) 재확인.
6. 다음 — 13편이 남긴 **진짜 동시 MVCC**로 합류(steal+abort롤백+다중 트랜잭션이 이제 갖춰짐).

## 7. 스코프 판단 (사용자 결정 필요)
15편은 14편보다 훨씬 크다(페이지 포맷 변경 + 체크포인트 + 3-패스). **두 편으로 쪼개는 걸 추천**:
- **15편(a): no-force + pageLSN + 상시 redo** — "커밋을 로그 fsync만으로" 까지.
- **16편(b): 체크포인트 + 3-패스 ARIES + CLR** — 로그 bound + 교과서 복구.

한 편으로 몰면 분량·리스크가 과하고, 서사도 "빨라짐→새 문제→체크포인트→정식화"로 자연히 둘로 갈린다. 결정: **한 편 vs 두 편**, 그리고 **pageLSN 포맷 변경을 지금 감수할지**.
