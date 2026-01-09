CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -g
BUILD := build

# 핵심 소스 (계층이 늘면 여기에 추가)
SRCS := src/pager.c

.PHONY: test clean

# 테스트 빌드 후 실행
test: $(BUILD)/test_pager
	./$(BUILD)/test_pager

$(BUILD)/test_pager: tests/test_pager.c $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $^ -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
