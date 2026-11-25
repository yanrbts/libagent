CC         ?= gcc
BUILD_DIR  ?= $(abspath ../build)
INC_DIR    ?= $(abspath ../include)
SRC_DIR    ?= $(abspath ../src)
STATIC_LIB ?= $(BUILD_DIR)/libagent.a

EX_BUILD_DIR := $(BUILD_DIR)/examples

CPPFLAGS := -D_POSIX_C_SOURCE=200809L -I$(INC_DIR) -I$(SRC_DIR)
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -Werror
DEPFLAGS  = -MMD -MP
LDFLAGS  ?=
LDLIBS   ?= -lm

ASAN_FLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer -g

SRCS      := $(wildcard *.c)
TARGETS   := $(SRCS:.c=)
OBJS      := $(patsubst %.c,$(EX_BUILD_DIR)/%.o,$(SRCS))
BINS      := $(patsubst %,$(EX_BUILD_DIR)/%,$(TARGETS))
ASAN_BINS := $(patsubst %,$(EX_BUILD_DIR)/%_asan,$(TARGETS))
DEPS      := $(OBJS:.o=.d)

LIB_SRCS  := $(wildcard $(SRC_DIR)/*.c)

.PHONY: build asan clean

build: $(BINS)

$(EX_BUILD_DIR)/%.o: %.c | $(EX_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(EX_BUILD_DIR)/%: $(EX_BUILD_DIR)/%.o $(STATIC_LIB) | $(EX_BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(STATIC_LIB) $(LDLIBS) -o $@

asan: $(ASAN_BINS)

$(EX_BUILD_DIR)/%_asan: %.c $(LIB_SRCS) | $(EX_BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -Wall -Wextra -Werror $(ASAN_FLAGS) \
		$(LIB_SRCS) $< $(LDFLAGS) $(LDLIBS) -o $@

$(EX_BUILD_DIR):
	mkdir -p $@

-include $(DEPS)

clean:
	rm -rf $(EX_BUILD_DIR)