CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -g
CFLAGS += -pthread   # 트랙 D: 버퍼 풀 latch·스레드 서버
BUILD := build

# 핵심 소스 (계층이 늘면 여기에 추가)
SRCS := src/pager.c src/page.c src/bufpool.c src/heap.c src/sql.c src/db.c src/btree.c src/wal.c src/lock.c src/mvcc.c

# REPL/서버 바이너리에만 링크되는 소스(테스트엔 불필요)
BINSRCS := src/server.c

# 테스트 (tests/test_<name>.c 를 추가하고 여기에 이름만 넣으면 된다)
TESTS := test_pager test_page test_bufpool test_heap test_sql test_exec test_btree test_wal test_txn test_dml test_where test_join test_agg test_waldml test_explain test_secindex test_lock test_isolation test_mvcc test_mvcc_store test_recovery test_mvcc_dml test_vacuum test_multitxn test_concurrency test_optimizer test_cbtree test_clustered

.PHONY: test repl serve clean bench test-tsan

# 동시성(버퍼 풀 latch·B+Tree crabbing)을 ThreadSanitizer로 검사 (data race 있으면 잡힘)
test-tsan: | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=thread -Isrc tests/test_concurrency.c $(SRCS) -o $(BUILD)/test_concurrency_tsan
	./$(BUILD)/test_concurrency_tsan
	$(CC) $(CFLAGS) -fsanitize=thread -Isrc tests/test_cbtree.c src/cbtree.c -o $(BUILD)/test_cbtree_tsan
	./$(BUILD)/test_cbtree_tsan

# cbtree는 엔진과 별개(독립 동시성 B+Tree). 엔진 SRCS 대신 cbtree.c만 링크.
$(BUILD)/test_cbtree: tests/test_cbtree.c src/cbtree.c | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -o $@

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
