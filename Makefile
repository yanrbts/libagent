CC          ?= cc
AR          ?= ar

TARGET_NAME ?= mcp
BUILD_DIR   ?= build
INC_DIR     ?= include
SRC_DIR     ?= src
TEST_DIR    := tests
EXAMPLE_DIR := examples

ABS_BUILD_DIR := $(abspath $(BUILD_DIR))
ABS_INC_DIR   := $(abspath $(INC_DIR))
ABS_SRC_DIR   := $(abspath $(SRC_DIR))

STATIC_LIB    := $(ABS_BUILD_DIR)/lib$(TARGET_NAME).a
SHARED_LIB    := $(ABS_BUILD_DIR)/lib$(TARGET_NAME).so

SRCS := \
	$(ABS_SRC_DIR)/util.c \
	$(ABS_SRC_DIR)/log.c \
	$(ABS_SRC_DIR)/lib_buf.c \
	$(ABS_SRC_DIR)/lib_io.c \
	$(ABS_SRC_DIR)/lib_mcp.c \
	$(ABS_SRC_DIR)/cJSON.c

STATIC_OBJS := $(patsubst $(ABS_SRC_DIR)/%.c,$(ABS_BUILD_DIR)/static/%.o,$(SRCS))
SHARED_OBJS := $(patsubst $(ABS_SRC_DIR)/%.c,$(ABS_BUILD_DIR)/shared/%.o,$(SRCS))

CPPFLAGS      := -D_POSIX_C_SOURCE=200809L -I$(ABS_INC_DIR) -I$(ABS_SRC_DIR)
CFLAGS        ?= -std=c11 -O2 -Wall -Wextra -Werror -fvisibility=hidden
DEPFLAGS       = -MMD -MP

STATIC_CFLAGS := $(CFLAGS)
SHARED_CFLAGS := $(CFLAGS) -fPIC
LDFLAGS       ?=
LDLIBS        ?= -lm -lpthread

ASAN_FLAGS    := -fsanitize=address,undefined -fno-omit-frame-pointer -g

SUBMAKE_ARGS := \
	CC="$(CC)" \
	AR="$(AR)" \
	BUILD_DIR="$(ABS_BUILD_DIR)" \
	INC_DIR="$(ABS_INC_DIR)" \
	SRC_DIR="$(ABS_SRC_DIR)" \
	STATIC_LIB="$(STATIC_LIB)" \
	SHARED_LIB="$(SHARED_LIB)"

.PHONY: all static shared \
        test-build test test-asan \
        examples-build examples-asan \
        clean distclean

all: static shared test-build examples-build

static: $(STATIC_LIB)

shared: $(SHARED_LIB)

$(STATIC_LIB): $(STATIC_OBJS) | $(ABS_BUILD_DIR)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(SHARED_OBJS) | $(ABS_BUILD_DIR)
	$(CC) -shared $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(ABS_BUILD_DIR)/static/%.o: $(ABS_SRC_DIR)/%.c | $(ABS_BUILD_DIR)/static
	$(CC) $(CPPFLAGS) $(STATIC_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(ABS_BUILD_DIR)/shared/%.o: $(ABS_SRC_DIR)/%.c | $(ABS_BUILD_DIR)/shared
	$(CC) $(CPPFLAGS) $(SHARED_CFLAGS) $(DEPFLAGS) -c $< -o $@

test-build: static
	$(MAKE) -C $(TEST_DIR) $(SUBMAKE_ARGS) build

test: static
	$(MAKE) -C $(TEST_DIR) $(SUBMAKE_ARGS) run

test-asan:
	$(MAKE) -C $(TEST_DIR) $(SUBMAKE_ARGS) asan

examples-build: static
	$(MAKE) -C $(EXAMPLE_DIR) $(SUBMAKE_ARGS) build

examples-asan:
	$(MAKE) -C $(EXAMPLE_DIR) $(SUBMAKE_ARGS) asan

$(ABS_BUILD_DIR) $(ABS_BUILD_DIR)/static $(ABS_BUILD_DIR)/shared:
	mkdir -p $@

-include $(STATIC_OBJS:.o=.d) $(SHARED_OBJS:.o=.d)

clean:
	$(MAKE) -C $(TEST_DIR) $(SUBMAKE_ARGS) clean
	$(MAKE) -C $(EXAMPLE_DIR) $(SUBMAKE_ARGS) clean
	rm -rf $(ABS_BUILD_DIR)/static $(ABS_BUILD_DIR)/shared

distclean:
	rm -rf $(ABS_BUILD_DIR)