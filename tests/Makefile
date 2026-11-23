CC         ?= gcc
BUILD_DIR  ?= $(abspath ../build)
INC_DIR    ?= $(abspath ../include)
SRC_DIR    ?= $(abspath ../src)
STATIC_LIB ?= $(BUILD_DIR)/libmcp.a

TEST_BUILD_DIR := $(BUILD_DIR)/test

CPPFLAGS := -D_POSIX_C_SOURCE=200809L -I$(INC_DIR) -I$(SRC_DIR)
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -Werror
DEPFLAGS  = -MMD -MP
LDFLAGS  ?=
LDLIBS   ?= -lm -lpthread

ASAN_FLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer -g

MCP_TEST_SRC  := test_lib_mcp.c
MCP_TEST_OBJ  := $(TEST_BUILD_DIR)/test_lib_mcp.o
MCP_TEST_BIN  := $(TEST_BUILD_DIR)/test_lib_mcp
MCP_ASAN_BIN  := $(TEST_BUILD_DIR)/test_lib_mcp_asan

IO_TEST_SRC   := test_lib_io.c
IO_TEST_OBJ   := $(TEST_BUILD_DIR)/test_lib_io.o
IO_TEST_BIN   := $(TEST_BUILD_DIR)/test_lib_io
IO_ASAN_BIN   := $(TEST_BUILD_DIR)/test_lib_io_asan

LIB_SRCS := $(wildcard $(SRC_DIR)/*.c)
DEPS     := $(MCP_TEST_OBJ:.o=.d) $(IO_TEST_OBJ:.o=.d)

.PHONY: build run asan clean

build: $(MCP_TEST_BIN) $(IO_TEST_BIN)

$(MCP_TEST_OBJ): $(MCP_TEST_SRC) | $(TEST_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(MCP_TEST_BIN): $(MCP_TEST_OBJ) $(STATIC_LIB) | $(TEST_BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(STATIC_LIB) $(LDLIBS) -o $@

$(IO_TEST_OBJ): $(IO_TEST_SRC) | $(TEST_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(IO_TEST_BIN): $(IO_TEST_OBJ) $(STATIC_LIB) | $(TEST_BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(STATIC_LIB) $(LDLIBS) -o $@

run: build
	./$(MCP_TEST_BIN)
	./$(IO_TEST_BIN)

asan: | $(TEST_BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -Wall -Wextra -Werror $(ASAN_FLAGS) \
		$(LIB_SRCS) $(MCP_TEST_SRC) $(LDFLAGS) $(LDLIBS) -o $(MCP_ASAN_BIN)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -Wall -Wextra -Werror $(ASAN_FLAGS) \
		$(LIB_SRCS) $(IO_TEST_SRC) $(LDFLAGS) $(LDLIBS) -o $(IO_ASAN_BIN)
	./$(MCP_ASAN_BIN)
	./$(IO_ASAN_BIN)

$(TEST_BUILD_DIR):
	mkdir -p $@

-include $(DEPS)

clean:
	rm -rf $(TEST_BUILD_DIR)