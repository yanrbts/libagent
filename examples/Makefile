CC         ?= gcc
BUILD_DIR  ?= $(abspath ../build)
INC_DIR    ?= $(abspath ../include)
SRC_DIR    ?= $(abspath ../src)
STATIC_LIB ?= $(BUILD_DIR)/libmcp.a

EX_BUILD_DIR := $(BUILD_DIR)/examples

CPPFLAGS := -D_POSIX_C_SOURCE=200809L -I$(INC_DIR) -I$(SRC_DIR)
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -Werror
DEPFLAGS  = -MMD -MP
LDFLAGS  ?=
LDLIBS   ?= -lm

ASAN_FLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer -g

DEMO_SRC  := mcp_client_demo.c
DEMO_OBJ  := $(EX_BUILD_DIR)/mcp_client_demo.o
DEMO_BIN  := $(EX_BUILD_DIR)/mcp_client_demo
DEMO_ASAN := $(EX_BUILD_DIR)/mcp_client_demo_asan

LIB_SRCS  := $(wildcard $(SRC_DIR)/*.c)
DEPS      := $(DEMO_OBJ:.o=.d)

.PHONY: build asan clean

build: $(DEMO_BIN)

$(DEMO_OBJ): $(DEMO_SRC) | $(EX_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(DEMO_BIN): $(DEMO_OBJ) $(STATIC_LIB) | $(EX_BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(STATIC_LIB) $(LDLIBS) -o $@

asan: | $(EX_BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -Wall -Wextra -Werror $(ASAN_FLAGS) \
		$(LIB_SRCS) $(DEMO_SRC) $(LDFLAGS) $(LDLIBS) -o $(DEMO_ASAN)

$(EX_BUILD_DIR):
	mkdir -p $@

-include $(DEPS)

clean:
	rm -rf $(EX_BUILD_DIR)