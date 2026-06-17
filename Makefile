CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -g
CFLAGS += -pthread   # 트랙 D: 버퍼 풀 latch·스레드 서버
BUILD := build

# 핵심 소스 (계층이 늘면 여기에 추가)
SRCS := src/pager.c src/page.c src/bufpool.c src/heap.c src/sql.c src/db.c src/btree.c src/wal.c src/lock.c src/mvcc.c src/replica.c src/replnet.c src/lsm.c src/parscan.c

# REPL/서버 바이너리에만 링크되는 소스(테스트엔 불필요)
BINSRCS := src/server.c

# 테스트 (tests/test_<name>.c 를 추가하고 여기에 이름만 넣으면 된다)
TESTS := test_pager test_page test_bufpool test_heap test_sql test_exec test_btree test_wal test_txn test_dml test_where test_join test_agg test_waldml test_explain test_secindex test_lock test_isolation test_mvcc test_mvcc_store test_recovery test_mvcc_dml test_vacuum test_multitxn test_concurrency test_optimizer test_cbtree test_clustered test_joinopt test_replica test_replnet test_lsm test_lsm_engine test_parscan test_parexec test_paragg test_raft test_repl_e2e test_raftdb

.PHONY: test repl serve clean bench test-tsan

# 동시성(버퍼 풀 latch·B+Tree crabbing)을 ThreadSanitizer로 검사 (data race 있으면 잡힘)
test-tsan: | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=thread -Isrc tests/test_concurrency.c $(SRCS) -o $(BUILD)/test_concurrency_tsan
	./$(BUILD)/test_concurrency_tsan
	$(CC) $(CFLAGS) -fsanitize=thread -Isrc tests/test_cbtree.c src/cbtree.c -o $(BUILD)/test_cbtree_tsan
	./$(BUILD)/test_cbtree_tsan
	$(CC) $(CFLAGS) -fsanitize=thread -Isrc tests/test_parscan.c src/parscan.c src/heap.c src/bufpool.c src/pager.c src/page.c -o $(BUILD)/test_parscan_tsan
	./$(BUILD)/test_parscan_tsan
	$(CC) $(CFLAGS) -fsanitize=thread -Isrc tests/test_parexec.c $(SRCS) -o $(BUILD)/test_parexec_tsan
	./$(BUILD)/test_parexec_tsan
	$(CC) $(CFLAGS) -fsanitize=thread -Isrc tests/test_paragg.c $(SRCS) -o $(BUILD)/test_paragg_tsan
	./$(BUILD)/test_paragg_tsan

# parscan은 엔진과 별개(병렬 풀 스캔, 실행기 미배선). 스토리지 스택만 링크.
$(BUILD)/test_parscan: tests/test_parscan.c src/parscan.c src/heap.c src/bufpool.c src/pager.c src/page.c | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -o $@

# cbtree는 엔진과 별개(독립 동시성 B+Tree). 엔진 SRCS 대신 cbtree.c만 링크.
$(BUILD)/test_cbtree: tests/test_cbtree.c src/cbtree.c | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -o $@

# joinopt도 엔진과 별개(순수 조인 순서 계획기, 실행기 미배선). joinopt.c만 링크.
$(BUILD)/test_joinopt: tests/test_joinopt.c src/joinopt.c | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -o $@

# test_lsm은 LSM을 단위(unit)로 검증하므로 lsm.c만 링크한다. (35편에서 lsm.c는
# SRCS에 편입돼 db.c의 PK 인덱스 저장 엔진으로도 배선됐다 — USING lsm.)
$(BUILD)/test_lsm: tests/test_lsm.c src/lsm.c | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -o $@

# raft도 엔진과 별개(합의 코어, 결정적 시뮬레이션으로 검증). raft.c만 링크.
$(BUILD)/test_raft: tests/test_raft.c src/raft.c | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -o $@

# raftdb는 Raft(raft.c)로 실제 엔진(SRCS)을 복제하는 통합 계층. 셋 다 링크.
$(BUILD)/test_raftdb: tests/test_raftdb.c src/raftdb.c src/raft.c $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $< src/raftdb.c src/raft.c $(SRCS) -o $@

test: $(addprefix $(BUILD)/, $(TESTS))
	@for t in $(TESTS); do echo "=== $$t ==="; ./$(BUILD)/$$t || exit 1; echo; done

# 실측 벤치마크 (최적화 빌드 -O2로 컴파일해 실행)
bench: $(BUILD)/bench
	./$(BUILD)/bench
$(BUILD)/bench: tests/bench.c $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -O2 -Isrc $< $(SRCS) -o $@

# 힙(PG) vs 클러스터드(InnoDB) 접근 경로 비용 대조
bench-clustered: $(BUILD)/bench_clustered
	./$(BUILD)/bench_clustered
$(BUILD)/bench_clustered: tests/bench_clustered.c $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -O2 -Isrc $< $(SRCS) -o $@

# 대화형 REPL / PostgreSQL wire 서버 바이너리 (한 바이너리, --serve로 서버)
repl: $(BUILD)/db-hobby
serve: $(BUILD)/db-hobby
	./$(BUILD)/db-hobby db-hobby.db --serve 5433
$(BUILD)/db-hobby: src/main.c $(SRCS) $(BINSRCS) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $< $(SRCS) $(BINSRCS) -o $@

# 각 테스트 = 그 테스트 소스 + 모든 핵심 소스
$(BUILD)/test_%: tests/test_%.c $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $< $(SRCS) -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
