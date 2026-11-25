CC         ?= gcc
BUILD_DIR  ?= $(abspath ../build)
INC_DIR    ?= $(abspath ../include)
SRC_DIR    ?= $(abspath ../src)
STATIC_LIB ?= $(BUILD_DIR)/libagent.a

TEST_DIR       := .
UNITY_DIR      := $(TEST_DIR)
UNITY_SRC      := $(UNITY_DIR)/unity.c
TEST_BUILD_DIR := $(BUILD_DIR)/test

TLS ?= 0

CPPFLAGS += -D_GNU_SOURCE -I$(INC_DIR) -I$(SRC_DIR) -I$(UNITY_DIR)
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -Werror
DEPFLAGS  = -MMD -MP
LDFLAGS  ?=
LDLIBS   += -lm -lpthread

ifeq ($(TLS),1)
	CPPFLAGS += -DMG_TLS=MG_TLS_OPENSSL
	LDLIBS   += -lssl -lcrypto
endif

ASAN_FLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer -g

# Targets that define __wrap_malloc / __wrap_calloc
MOCK_WRAP_TESTS := test_lib_llm_mock

# Automatically collect all test_*.c files under tests/
TEST_SRCS  := $(sort $(wildcard $(TEST_DIR)/test_*.c))
TEST_NAMES := $(basename $(notdir $(TEST_SRCS)))

TEST_OBJS := $(patsubst %,$(TEST_BUILD_DIR)/%.o,$(TEST_NAMES))
TEST_BINS := $(patsubst %,$(TEST_BUILD_DIR)/%,$(TEST_NAMES))
ASAN_BINS := $(patsubst %,$(TEST_BUILD_DIR)/%_asan,$(TEST_NAMES))

UNITY_OBJ      := $(TEST_BUILD_DIR)/unity.o
UNITY_ASAN_OBJ := $(TEST_BUILD_DIR)/unity_asan.o

# Exclude standalone entrypoints like main.c if any exist in src/
LIB_SRCS      := $(filter-out %/main.c,$(sort $(wildcard $(SRC_DIR)/*.c)))
LIB_ASAN_OBJS := $(patsubst $(SRC_DIR)/%.c,$(TEST_BUILD_DIR)/asan_objs/%.o,$(LIB_SRCS))
ASAN_LIB      := $(TEST_BUILD_DIR)/libagent_asan.a

DEPS := $(TEST_OBJS:.o=.d) $(UNITY_OBJ:.o=.d)

# Target-specific linker wrap flags for mock OOM test binaries
$(foreach t,$(MOCK_WRAP_TESTS),$(TEST_BUILD_DIR)/$(t)): \
	LDFLAGS += -Wl,--wrap=malloc -Wl,--wrap=calloc

$(foreach t,$(MOCK_WRAP_TESTS),$(TEST_BUILD_DIR)/$(t)_asan): \
	LDFLAGS += -Wl,--wrap=malloc -Wl,--wrap=calloc

.DEFAULT_GOAL := build
.PHONY: build run build-asan asan clean

build: $(TEST_BINS)

# Compile Unity framework separately without -Werror
$(UNITY_OBJ): $(UNITY_SRC) | $(TEST_BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=c11 -O2 -Wall -Wextra $(DEPFLAGS) -c $< -o $@

$(TEST_BUILD_DIR)/%.o: $(TEST_DIR)/%.c | $(TEST_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_BUILD_DIR)/%: $(TEST_BUILD_DIR)/%.o $(UNITY_OBJ) $(STATIC_LIB) | $(TEST_BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(UNITY_OBJ) $(STATIC_LIB) $(LDLIBS) -o $@

run: build
	@set -e; for bin in $(TEST_BINS); do \
		"$$bin"; \
	done

# Build ASan/UBSan library and test binaries
$(UNITY_ASAN_OBJ): $(UNITY_SRC) | $(TEST_BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -Wall -Wextra $(ASAN_FLAGS) -c $< -o $@

$(TEST_BUILD_DIR)/asan_objs/%.o: $(SRC_DIR)/%.c | $(TEST_BUILD_DIR)/asan_objs
	$(CC) $(CPPFLAGS) -std=c11 -O1 -Wall -Wextra -Werror $(ASAN_FLAGS) -fPIC -c $< -o $@

$(ASAN_LIB): $(LIB_ASAN_OBJS)
	$(AR) rcs $@ $^

$(TEST_BUILD_DIR)/%_asan: $(TEST_DIR)/%.c $(ASAN_LIB) $(UNITY_ASAN_OBJ) | $(TEST_BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -Wall -Wextra -Werror $(ASAN_FLAGS) \
		$< $(ASAN_LIB) $(UNITY_ASAN_OBJ) $(LDFLAGS) $(LDLIBS) -o $@

build-asan: $(ASAN_BINS)

asan: build-asan
	@set -e; for bin in $(ASAN_BINS); do \
		"$$bin"; \
	done

$(TEST_BUILD_DIR) $(TEST_BUILD_DIR)/asan_objs:
	mkdir -p $@

-include $(DEPS)

clean:
	rm -rf $(TEST_BUILD_DIR)