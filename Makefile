# gitfull — source/forge-based package manager
# Build: plain make + cc. No build-system dependencies beyond a C compiler and make.

CC       ?= cc
CSTD     ?= -std=c17
DEFS     := -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_DEFAULT_SOURCE
OPT      ?= -O2 -g
WARN     := -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 \
            -Wwrite-strings -Wpointer-arith -Wcast-qual -Wstrict-prototypes \
            -Wmissing-prototypes -Wswitch-enum -Wundef -Wold-style-definition \
            -Wvla -Werror
INC      := -Isrc
LDLIBS   := -lsqlite3 -lz
LDFLAGS  ?=

PREFIX   ?= /usr/local
BINDIR   := $(PREFIX)/bin

SRC_DIR  := src
BUILD    := build
BIN      := gitfull

SRCS := $(sort $(wildcard $(SRC_DIR)/*.c) $(wildcard $(SRC_DIR)/*/*.c))
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

TEST_SRCS := $(sort $(wildcard tests/unit/*.c))
TEST_BIN  := $(BUILD)/unit_tests

.PHONY: all clean install test unit asan fuzzcheck

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CSTD) $(OPT) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

$(BUILD)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(OPT) $(WARN) $(DEFS) $(INC) -c -o $@ $< -MMD -MP

# Unit tests (same strict flags; linked against the same objects except main.o)
unit: $(TEST_BIN)
$(TEST_BIN): $(filter-out $(BUILD)/main.o,$(OBJS)) $(TEST_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(OPT) $(WARN) $(DEFS) $(INC) -o $@ $(TEST_SRCS) \
		$(filter-out $(BUILD)/main.o,$(OBJS)) $(LDLIBS)

test: unit
	$(TEST_BIN)
	@bash tests/integration/run-all.sh

# Sanitizer build (ASan + UBSan) for development/testing
asan:
	@mkdir -p $(BUILD)
	$(MAKE) BUILD=$(BUILD)/asan CFLAGS_DUMMY=1 OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" unit
	ASAN_OPTIONS=detect_leaks=1 $(BUILD)/asan/unit_tests

clean:
	rm -rf $(BUILD) $(BIN)

install: $(BIN)
	install -Dm0755 $(BIN) $(DESTDIR)$(BINDIR)/gitfull

-include $(DEPS)
