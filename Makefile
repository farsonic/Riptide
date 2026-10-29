# Riptide - host build (Linux) for development and tests.
#   make                  native build (+ tests)
#   make SAN=1            with AddressSanitizer/UBSan
#   make CROSS=m68k-linux-gnu-   big-endian 68020 build, run tests with qemu-m68k;
#                         links the 680x0 assembly routines (vasm; ASM=0: C only)
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
  ARCHFLAGS := -m68020 -mtune=68020-60 -static
  # static binaries: speedtest's POSIX shim takes numeric addresses only
  # (getaddrinfo would need glibc's shared NSS modules at run time)
  ARCHFLAGS += -DST_POSIX_STATIC
  LDFLAGS += -pthread -static
endif
VBCC ?= /opt/vbcc
VASM ?= $(VBCC)/bin/vasmm68k_mot
ifneq ($(findstring m68k,$(CROSS)),)
  ASM ?= 1
endif
ifeq ($(ASM),1)
  ARCHFLAGS += -DRT_ARCH_M68K_ASM
  ASM_OBJ := $(BUILD)/arch/m68k/rt_m68k.o
endif
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
           -Wshadow -Wpointer-arith -Wstrict-prototypes -Iinclude $(ARCHFLAGS)
# rebuild objects when a header they include changes
CFLAGS  += -MMD -MP
ifeq ($(SAN),1)
  CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
  LDFLAGS += -fsanitize=address,undefined
endif

CORE_SRC := $(wildcard src/core/*.c)
HOST_SRC := $(wildcard src/host/*.c)
LIB_OBJ  := $(patsubst src/%.c,$(BUILD)/%.o,$(CORE_SRC) $(HOST_SRC)) $(ASM_OBJ)
TEST_SRC := $(wildcard tests/*.c)
TEST_BIN := $(patsubst tests/%.c,$(BUILD)/tests/%,$(TEST_SRC))
# speedtest (src/apps/speedtest): portable code + the POSIX shim, for the
# host tests and build/host/speedtest
ST_SRC   := $(wildcard src/apps/speedtest/*.c)
ST_OBJ   := $(patsubst src/%.c,$(BUILD)/%.o,$(filter-out %/st_posix_main.c,$(ST_SRC)))
ST_HDR   := $(wildcard src/apps/speedtest/*.h)

all: $(BUILD)/libriptide.a $(BUILD)/libspeedtest.a $(BUILD)/speedtest $(TEST_BIN)

$(BUILD)/%.o: src/%.c $(wildcard include/rt/*.h) $(ST_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/arch/m68k/%.o: src/arch/m68k/%.s
	@mkdir -p $(dir $@)
	$(VASM) -quiet -m68020 -Felf -o $@ $<
	$(CROSS)objcopy --add-section .note.GNU-stack=/dev/null --set-section-flags .note.GNU-stack=contents,readonly $@

$(BUILD)/libriptide.a: $(LIB_OBJ)
	@mkdir -p $(dir $@)
	rm -f $@ && $(AR) rcs $@ $^

$(BUILD)/libspeedtest.a: $(ST_OBJ)
	@mkdir -p $(dir $@)
	rm -f $@ && $(AR) rcs $@ $^

$(BUILD)/speedtest: $(BUILD)/apps/speedtest/st_posix_main.o $(BUILD)/libspeedtest.a $(BUILD)/libriptide.a
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# the RiptidePrefs model test includes these sources
$(BUILD)/tests/t_prefs_conf: $(wildcard src/amiga/prefs/conf_edit.* src/amiga/prefs/prefs_fields.*)
$(BUILD)/tests/t_prefs_status: $(wildcard src/amiga/prefs/prefs_status.* src/amiga/prefs/prefs_counters.*)
$(BUILD)/tests/%: tests/%.c $(BUILD)/libriptide.a $(BUILD)/libspeedtest.a $(ST_HDR) $(wildcard tests/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isrc/apps/speedtest -o $@ $< $(BUILD)/libspeedtest.a $(BUILD)/libriptide.a $(LDFLAGS) -lm

check: all
	./tests/run_tests.sh $(BUILD)
	python3 tests/test_stackprobe_compare.py

# IPv6 interop peer (gVisor netstack) for tests/t_ip6_gvisor.c: needs Go
# and downloads gvisor.dev/gvisor, so it is not part of "all". A host
# binary also for the m68k build directory (qemu-m68k execs it natively).
GO ?= go
gvpeer: $(BUILD)/gvpeer
$(BUILD)/gvpeer: $(wildcard tests/interop/gvpeer/*.go) tests/interop/gvpeer/go.mod
	@mkdir -p $(dir $@)
	cd tests/interop/gvpeer && $(GO) build -o $(CURDIR)/$@ .
.PHONY: gvpeer

clean:
	rm -rf build

.PHONY: all check clean

-include $(LIB_OBJ:.o=.d) $(ST_OBJ:.o=.d)
