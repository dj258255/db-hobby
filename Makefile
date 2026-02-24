CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -g
BUILD := build

# 핵심 소스 (계층이 늘면 여기에 추가)
SRCS := src/pager.c src/page.c src/bufpool.c src/heap.c src/sql.c src/db.c src/btree.c src/wal.c

# 테스트 (tests/test_<name>.c 를 추가하고 여기에 이름만 넣으면 된다)
TESTS := test_pager test_page test_bufpool test_heap test_sql test_exec test_btree test_wal test_txn test_dml test_where test_join test_agg test_waldml

.PHONY: test repl clean

test: $(addprefix $(BUILD)/, $(TESTS))
	@for t in $(TESTS); do echo "=== $$t ==="; ./$(BUILD)/$$t || exit 1; echo; done

# 대화형 REPL 바이너리
repl: $(BUILD)/minidb
$(BUILD)/minidb: src/main.c $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $< $(SRCS) -o $@

# 각 테스트 = 그 테스트 소스 + 모든 핵심 소스
$(BUILD)/test_%: tests/test_%.c $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $< $(SRCS) -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
