CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -g
BUILD := build

# 핵심 소스 (계층이 늘면 여기에 추가)
SRCS := src/pager.c src/page.c src/bufpool.c src/heap.c

# 테스트 (tests/test_<name>.c 를 추가하고 여기에 이름만 넣으면 된다)
TESTS := test_pager test_page test_bufpool test_heap

.PHONY: test clean

test: $(addprefix $(BUILD)/, $(TESTS))
	@for t in $(TESTS); do echo "=== $$t ==="; ./$(BUILD)/$$t || exit 1; echo; done

# 각 테스트 = 그 테스트 소스 + 모든 핵심 소스
$(BUILD)/test_%: tests/test_%.c $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $< $(SRCS) -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
