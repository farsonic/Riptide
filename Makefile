# Riptide - host build (Linux) for development and tests.
#   make                  native build (+ tests)
#   make SAN=1            with AddressSanitizer/UBSan
#   make CROSS=m68k-linux-gnu-   big-endian 68020 build, run tests with qemu-m68k
# The AmigaOS build lives in Makefile.amiga.
CROSS   ?=
CC      := $(CROSS)gcc
AR      := $(CROSS)ar
ifeq ($(CROSS),)
  BUILD := build/host
  ARCHFLAGS :=
  LDFLAGS += -pthread
else
  BUILD := build/$(patsubst %-,%,$(CROSS))
  ARCHFLAGS := -m68020 -static
  LDFLAGS += -pthread -static
endif
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
           -Wshadow -Wpointer-arith -Wstrict-prototypes -Iinclude $(ARCHFLAGS)
ifeq ($(SAN),1)
  CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
  LDFLAGS += -fsanitize=address,undefined
endif

CORE_SRC := $(wildcard src/core/*.c)
HOST_SRC := $(wildcard src/host/*.c)
LIB_OBJ  := $(patsubst src/%.c,$(BUILD)/%.o,$(CORE_SRC) $(HOST_SRC))
TEST_SRC := $(wildcard tests/*.c)
TEST_BIN := $(patsubst tests/%.c,$(BUILD)/tests/%,$(TEST_SRC))

all: $(BUILD)/libriptide.a $(TEST_BIN)

$(BUILD)/%.o: src/%.c $(wildcard include/rt/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/libriptide.a: $(LIB_OBJ)
	@mkdir -p $(dir $@)
	rm -f $@ && $(AR) rcs $@ $^

$(BUILD)/tests/%: tests/%.c $(BUILD)/libriptide.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $< $(BUILD)/libriptide.a $(LDFLAGS) -lm

check: all
	./tests/run_tests.sh $(BUILD)

clean:
	rm -rf build

.PHONY: all check clean
