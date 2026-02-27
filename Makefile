# =============================================================================
# asx — asupersync ANSI C runtime
# Primary Makefile (bd-ix8.2)
#
# Targets cover all quality gates from AGENTS.md section 10.6 and
# PHASE2_SCAFFOLD_MANIFEST.md section 4.
#
# SPDX-License-Identifier: MIT
# =============================================================================

# ---------------------------------------------------------------------------
# Toolchain defaults (override via environment or command line)
# ---------------------------------------------------------------------------
CC       ?= gcc
AR       ?= ar
CFLAGS   ?=
LDFLAGS  ?=
PREFIX   ?= /usr/local

# ---------------------------------------------------------------------------
# Policy toggles (local defaults vs CI strictness)
# ---------------------------------------------------------------------------
CI ?= 0
FAIL_ON_MISSING_FORMATTER ?= $(CI)
FAIL_ON_MISSING_LINTER ?= $(CI)
FAIL_ON_MISSING_RUNNERS ?= $(CI)
FAIL_ON_MISSING_CROSS_TOOLCHAINS ?= $(CI)
FAIL_ON_EMPTY_UNIT_TESTS ?= $(CI)
FAIL_ON_EMPTY_INVARIANT_TESTS ?= 0
RUN_QEMU_IN_MATRIX ?= 0

# ---------------------------------------------------------------------------
# Profile selection (exactly one; default ASX_PROFILE_CORE)
# Usage: make build PROFILE=FREESTANDING
# ---------------------------------------------------------------------------
PROFILE  ?= CORE
PROFILE_DEF := -DASX_PROFILE_$(PROFILE)

# ---------------------------------------------------------------------------
# Codec selection (default JSON for bring-up)
# Usage: make build CODEC=BIN
# ---------------------------------------------------------------------------
CODEC    ?= JSON
CODEC_DEF := -DASX_CODEC_$(CODEC)

# ---------------------------------------------------------------------------
# Deterministic mode (on by default)
# ---------------------------------------------------------------------------
DETERMINISTIC ?= 1
DET_DEF := -DASX_DETERMINISTIC=$(DETERMINISTIC)

# ---------------------------------------------------------------------------
# Debug / Release mode
# ---------------------------------------------------------------------------
BUILD_TYPE ?= debug
ifeq ($(BUILD_TYPE),release)
  OPT_FLAGS := -O2 -DNDEBUG
else
  OPT_FLAGS := -O0 -g -DASX_DEBUG=1
endif

# ---------------------------------------------------------------------------
# Cross-compilation support
# Usage: make release TARGET=mipsel-openwrt-linux-musl
# ---------------------------------------------------------------------------
ifdef TARGET
  CROSS_PREFIX := $(TARGET)-
  CC := $(CROSS_PREFIX)gcc
  AR := $(CROSS_PREFIX)ar
endif

# ---------------------------------------------------------------------------
# Bitness override (for 32/64 matrix)
# Usage: make build BITS=32
# ---------------------------------------------------------------------------
ifdef BITS
  BITS_FLAGS := -m$(BITS)
else
  BITS_FLAGS :=
endif

# ---------------------------------------------------------------------------
# Warning policy — warnings-as-errors for core/kernel
# ---------------------------------------------------------------------------
WARN_FLAGS := -Wall -Wextra -Wpedantic -Werror \
              -Wconversion -Wsign-conversion -Wshadow \
              -Wstrict-prototypes -Wmissing-prototypes \
              -Wswitch-enum -Wformat=2 \
              -Wno-unused-parameter

# C standard
STD_FLAGS := -std=c99

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
INC_FLAGS := -I$(CURDIR)/include

# ---------------------------------------------------------------------------
# Combined compiler flags
# ---------------------------------------------------------------------------
ALL_CFLAGS := $(STD_FLAGS) $(WARN_FLAGS) $(OPT_FLAGS) $(BITS_FLAGS) \
              $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) $(CFLAGS)

ALL_LDFLAGS := $(BITS_FLAGS) $(LDFLAGS)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
CORE_SRC := \
	src/core/status.c \
	src/core/transition_tables.c \
	src/core/outcome.c \
	src/core/budget.c \
	src/core/cancel.c \
	src/core/cleanup.c

RUNTIME_SRC := \
	src/runtime/hooks.c \
	src/runtime/lifecycle.c \
	src/runtime/scheduler.c \
	src/runtime/cancellation.c \
	src/runtime/quiescence.c

CHANNEL_SRC := \
	src/channel/mpsc.c

TIME_SRC := \
	src/time/timer_wheel.c

# Platform sources selected by profile
ifeq ($(PROFILE),POSIX)
  PLATFORM_SRC := src/platform/posix/hooks.c
else ifeq ($(PROFILE),WIN32)
  PLATFORM_SRC := src/platform/win32/hooks.c
else ifeq ($(PROFILE),FREESTANDING)
  PLATFORM_SRC := src/platform/freestanding/hooks.c
else ifeq ($(PROFILE),EMBEDDED_ROUTER)
  PLATFORM_SRC := src/platform/freestanding/hooks.c
else
  PLATFORM_SRC :=
endif

LIB_SRC := $(CORE_SRC) $(RUNTIME_SRC) $(CHANNEL_SRC) $(TIME_SRC) $(PLATFORM_SRC)

# ---------------------------------------------------------------------------
# Object files and output
# ---------------------------------------------------------------------------
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
LIB_DIR   := $(BUILD_DIR)/lib
BIN_DIR   := $(BUILD_DIR)/bin
TEST_DIR  := $(BUILD_DIR)/tests

LIB_OBJ := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(LIB_SRC))
LIB_A   := $(LIB_DIR)/libasx.a

# ---------------------------------------------------------------------------
# Test sources
# ---------------------------------------------------------------------------
UNIT_TEST_SRC := $(wildcard tests/unit/core/*_test.c) \
                 $(wildcard tests/unit/core/test_*.c) \
                 $(wildcard tests/unit/runtime/*_test.c) \
                 $(wildcard tests/unit/runtime/test_*.c) \
                 $(wildcard tests/unit/channel/*_test.c) \
                 $(wildcard tests/unit/channel/test_*.c) \
                 $(wildcard tests/unit/time/*_test.c) \
                 $(wildcard tests/unit/time/test_*.c)
UNIT_TEST_SRC := $(sort $(UNIT_TEST_SRC))

INVARIANT_TEST_SRC := $(wildcard tests/invariant/lifecycle/*_test.c) \
                      $(wildcard tests/invariant/lifecycle/test_*.c) \
                      $(wildcard tests/invariant/quiescence/*_test.c) \
                      $(wildcard tests/invariant/quiescence/test_*.c)
INVARIANT_TEST_SRC := $(sort $(INVARIANT_TEST_SRC))

UNIT_TEST_BIN := $(patsubst tests/%.c,$(TEST_DIR)/%,$(UNIT_TEST_SRC))
INV_TEST_BIN  := $(patsubst tests/%.c,$(TEST_DIR)/%,$(INVARIANT_TEST_SRC))

# ---------------------------------------------------------------------------
# Test include path (adds tests/ for test_harness.h)
# ---------------------------------------------------------------------------
TEST_CFLAGS := $(ALL_CFLAGS) -I$(CURDIR)/tests

# ===================================================================
# PRIMARY TARGETS — map 1:1 to quality gate commands
# ===================================================================

.PHONY: all build clean install uninstall
.PHONY: format-check lint
.PHONY: test test-unit test-invariants
.PHONY: conformance codec-equivalence profile-parity
.PHONY: fuzz-smoke ci-embedded-matrix
.PHONY: release
.PHONY: build-gcc build-clang build-msvc build-32 build-64
.PHONY: build-embedded-mipsel build-embedded-armv7 build-embedded-aarch64
.PHONY: qemu-smoke

all: build

# ---------------------------------------------------------------------------
# build — compile library with strict warnings-as-errors
# ---------------------------------------------------------------------------
build: $(LIB_A)
	@echo "[asx] build complete (profile=$(PROFILE) codec=$(CODEC) det=$(DETERMINISTIC))"

$(LIB_A): $(LIB_OBJ) | $(LIB_DIR)
	$(AR) rcs $@ $^

$(OBJ_DIR)/%.o: src/%.c | obj-dirs
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

obj-dirs:
	@mkdir -p $(OBJ_DIR)/core $(OBJ_DIR)/runtime $(OBJ_DIR)/channel \
	          $(OBJ_DIR)/time $(OBJ_DIR)/platform/posix \
	          $(OBJ_DIR)/platform/win32 $(OBJ_DIR)/platform/freestanding

$(LIB_DIR):
	@mkdir -p $@

$(BIN_DIR):
	@mkdir -p $@

# ---------------------------------------------------------------------------
# format-check — verify source formatting (clang-format)
# ---------------------------------------------------------------------------
format-check:
	@echo "[asx] format-check: verifying source formatting..."
	@if command -v clang-format >/dev/null 2>&1; then \
		find include src tests \( -name '*.c' -o -name '*.h' \) -print0 | \
		xargs -0 clang-format --dry-run --Werror 2>&1 && \
		echo "[asx] format-check: PASS" || \
		{ echo "[asx] format-check: FAIL — run clang-format"; exit 1; }; \
	elif [ "$(FAIL_ON_MISSING_FORMATTER)" = "1" ]; then \
		echo "[asx] format-check: FAIL (clang-format not found; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] format-check: SKIP (clang-format not found)"; \
	fi

# ---------------------------------------------------------------------------
# lint — static analysis gate
# ---------------------------------------------------------------------------
lint:
	@echo "[asx] lint: running static analysis..."
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=warning,performance,portability --std=c99 --error-exitcode=1 \
		         --suppress=missingIncludeSystem \
		         --suppress=unusedFunction \
		         -I include src/ && \
		echo "[asx] lint: PASS (cppcheck)" || \
		{ echo "[asx] lint: FAIL"; exit 1; }; \
	elif command -v clang-tidy >/dev/null 2>&1; then \
		find src -name '*.c' | xargs clang-tidy -- $(ALL_CFLAGS) && \
		echo "[asx] lint: PASS (clang-tidy)" || \
		{ echo "[asx] lint: FAIL"; exit 1; }; \
	elif [ "$(FAIL_ON_MISSING_LINTER)" = "1" ]; then \
		echo "[asx] lint: FAIL (no static analyzer found; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] lint: SKIP (no static analyzer found)"; \
	fi

# ---------------------------------------------------------------------------
# test — run all test suites
# ---------------------------------------------------------------------------
test: test-unit test-invariants
	@echo "[asx] test: all suites passed"

# ---------------------------------------------------------------------------
# test-unit — per-module correctness tests
# ---------------------------------------------------------------------------
test-unit: $(UNIT_TEST_BIN)
	@echo "[asx] test-unit: running $(words $(UNIT_TEST_BIN)) test(s)..."
	@if [ -z "$(strip $(UNIT_TEST_BIN))" ]; then \
		if [ "$(FAIL_ON_EMPTY_UNIT_TESTS)" = "1" ]; then \
			echo "[asx] test-unit: FAIL (no tests found; strict mode)"; \
			exit 1; \
		else \
			echo "[asx] test-unit: no tests found (scaffold stage)"; \
		fi; \
	else \
		pass=0; fail=0; \
		for t in $(UNIT_TEST_BIN); do \
			echo "  RUN  $$(basename $$t)"; \
			if $$t; then \
				echo "  PASS $$(basename $$t)"; \
				pass=$$((pass + 1)); \
			else \
				echo "  FAIL $$(basename $$t)"; \
				fail=$$((fail + 1)); \
			fi; \
		done; \
		echo "[asx] test-unit: $$pass passed, $$fail failed"; \
		[ $$fail -eq 0 ] || exit 1; \
	fi

# Link individual unit tests
$(TEST_DIR)/unit/%: tests/unit/%.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

# ---------------------------------------------------------------------------
# test-invariants — lifecycle/quiescence invariant tests
# ---------------------------------------------------------------------------
test-invariants: $(INV_TEST_BIN)
	@echo "[asx] test-invariants: running $(words $(INV_TEST_BIN)) test(s)..."
	@if [ -z "$(strip $(INV_TEST_BIN))" ]; then \
		if [ "$(FAIL_ON_EMPTY_INVARIANT_TESTS)" = "1" ]; then \
			echo "[asx] test-invariants: FAIL (no tests found; strict mode)"; \
			exit 1; \
		else \
			echo "[asx] test-invariants: no tests found (scaffold stage)"; \
		fi; \
	else \
		pass=0; fail=0; \
		for t in $(INV_TEST_BIN); do \
			echo "  RUN  $$(basename $$t)"; \
			if $$t; then \
				echo "  PASS $$(basename $$t)"; \
				pass=$$((pass + 1)); \
			else \
				echo "  FAIL $$(basename $$t)"; \
				fail=$$((fail + 1)); \
			fi; \
		done; \
		echo "[asx] test-invariants: $$pass passed, $$fail failed"; \
		[ $$fail -eq 0 ] || exit 1; \
	fi

$(TEST_DIR)/invariant/%: tests/invariant/%.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

test-dirs:
	@mkdir -p $(TEST_DIR)/unit/core $(TEST_DIR)/unit/runtime \
	          $(TEST_DIR)/unit/channel $(TEST_DIR)/unit/time \
	          $(TEST_DIR)/invariant/lifecycle $(TEST_DIR)/invariant/quiescence

# ---------------------------------------------------------------------------
# conformance — Rust fixture parity verification
# ---------------------------------------------------------------------------
conformance:
	@echo "[asx] conformance: Rust fixture parity check..."
	@if [ -x tools/ci/run_conformance.sh ]; then \
		tools/ci/run_conformance.sh; \
	elif [ "$(FAIL_ON_MISSING_RUNNERS)" = "1" ]; then \
		echo "[asx] conformance: FAIL (runner missing; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] conformance: SKIP (runner not yet implemented)"; \
	fi

# ---------------------------------------------------------------------------
# codec-equivalence — JSON vs BIN semantic digest parity
# ---------------------------------------------------------------------------
codec-equivalence:
	@echo "[asx] codec-equivalence: JSON vs BIN parity check..."
	@if [ -x tools/ci/run_codec_equivalence.sh ]; then \
		tools/ci/run_codec_equivalence.sh; \
	elif [ "$(FAIL_ON_MISSING_RUNNERS)" = "1" ]; then \
		echo "[asx] codec-equivalence: FAIL (runner missing; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] codec-equivalence: SKIP (runner not yet implemented)"; \
	fi

# ---------------------------------------------------------------------------
# profile-parity — cross-profile canonical digest parity
# ---------------------------------------------------------------------------
profile-parity:
	@echo "[asx] profile-parity: cross-profile digest check..."
	@if [ -x tools/ci/run_profile_parity.sh ]; then \
		tools/ci/run_profile_parity.sh; \
	elif [ "$(FAIL_ON_MISSING_RUNNERS)" = "1" ]; then \
		echo "[asx] profile-parity: FAIL (runner missing; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] profile-parity: SKIP (runner not yet implemented)"; \
	fi

# ---------------------------------------------------------------------------
# fuzz-smoke — differential fuzzing smoke test
# ---------------------------------------------------------------------------
fuzz-smoke:
	@echo "[asx] fuzz-smoke: differential fuzzing smoke test..."
	@if [ -x tools/fuzz/run_smoke.sh ]; then \
		tools/fuzz/run_smoke.sh; \
	elif [ "$(FAIL_ON_MISSING_RUNNERS)" = "1" ]; then \
		echo "[asx] fuzz-smoke: FAIL (runner missing; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] fuzz-smoke: SKIP (fuzzer not yet implemented)"; \
	fi

# ---------------------------------------------------------------------------
# ci-embedded-matrix — cross-target embedded builds + QEMU
# ---------------------------------------------------------------------------
ci-embedded-matrix: build-embedded-mipsel build-embedded-armv7 build-embedded-aarch64
	@if [ "$(RUN_QEMU_IN_MATRIX)" = "1" ]; then \
		$(MAKE) qemu-smoke FAIL_ON_MISSING_RUNNERS=$(FAIL_ON_MISSING_RUNNERS); \
	fi
	@echo "[asx] ci-embedded-matrix: all embedded targets built"

# ---------------------------------------------------------------------------
# release — optimized production build
# ---------------------------------------------------------------------------
release:
	$(MAKE) build BUILD_TYPE=release

# ---------------------------------------------------------------------------
# install / uninstall
# ---------------------------------------------------------------------------
install: release
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include/asx
	install -d $(DESTDIR)$(PREFIX)/include/asx/core
	install -m 644 $(LIB_DIR)/libasx.a $(DESTDIR)$(PREFIX)/lib/
	install -m 644 include/asx/*.h $(DESTDIR)$(PREFIX)/include/asx/
	install -m 644 include/asx/core/*.h $(DESTDIR)$(PREFIX)/include/asx/core/
	@echo "[asx] installed to $(DESTDIR)$(PREFIX)"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/libasx.a
	rm -rf $(DESTDIR)$(PREFIX)/include/asx
	@echo "[asx] uninstalled from $(DESTDIR)$(PREFIX)"

# ---------------------------------------------------------------------------
# Compiler matrix targets
# ---------------------------------------------------------------------------
build-gcc:
	$(MAKE) build CC=gcc

build-clang:
	$(MAKE) build CC=clang

build-msvc:
	@echo "[asx] build-msvc: MSVC cross-build not yet wired (requires cl.exe on PATH)"
	@echo "[asx] build-msvc: SKIP"

build-32:
	$(MAKE) build BITS=32

build-64:
	$(MAKE) build BITS=64

# ---------------------------------------------------------------------------
# Embedded cross-target builds
# ---------------------------------------------------------------------------
build-embedded-mipsel:
	@echo "[asx] build-embedded-mipsel: building for mipsel-openwrt-linux-musl..."
	@if command -v mipsel-openwrt-linux-musl-gcc >/dev/null 2>&1; then \
		$(MAKE) build TARGET=mipsel-openwrt-linux-musl PROFILE=EMBEDDED_ROUTER; \
	elif [ "$(FAIL_ON_MISSING_CROSS_TOOLCHAINS)" = "1" ]; then \
		echo "[asx] build-embedded-mipsel: FAIL (toolchain not found; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] build-embedded-mipsel: SKIP (toolchain not found)"; \
	fi

build-embedded-armv7:
	@echo "[asx] build-embedded-armv7: building for armv7-openwrt-linux-muslgnueabi..."
	@if command -v armv7-openwrt-linux-muslgnueabi-gcc >/dev/null 2>&1; then \
		$(MAKE) build TARGET=armv7-openwrt-linux-muslgnueabi PROFILE=EMBEDDED_ROUTER; \
	elif [ "$(FAIL_ON_MISSING_CROSS_TOOLCHAINS)" = "1" ]; then \
		echo "[asx] build-embedded-armv7: FAIL (toolchain not found; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] build-embedded-armv7: SKIP (toolchain not found)"; \
	fi

build-embedded-aarch64:
	@echo "[asx] build-embedded-aarch64: building for aarch64-openwrt-linux-musl..."
	@if command -v aarch64-openwrt-linux-musl-gcc >/dev/null 2>&1; then \
		$(MAKE) build TARGET=aarch64-openwrt-linux-musl PROFILE=EMBEDDED_ROUTER; \
	elif [ "$(FAIL_ON_MISSING_CROSS_TOOLCHAINS)" = "1" ]; then \
		echo "[asx] build-embedded-aarch64: FAIL (toolchain not found; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] build-embedded-aarch64: SKIP (toolchain not found)"; \
	fi

# ---------------------------------------------------------------------------
# QEMU smoke test
# ---------------------------------------------------------------------------
qemu-smoke:
	@echo "[asx] qemu-smoke: QEMU scenario execution..."
	@if [ -x tools/ci/run_qemu_smoke.sh ]; then \
		tools/ci/run_qemu_smoke.sh; \
	elif [ "$(FAIL_ON_MISSING_RUNNERS)" = "1" ]; then \
		echo "[asx] qemu-smoke: FAIL (runner missing; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] qemu-smoke: SKIP (QEMU harness not yet implemented)"; \
	fi

# ---------------------------------------------------------------------------
# check — combined gate for PR/push CI
# ---------------------------------------------------------------------------
.PHONY: check check-ci
check: format-check lint build test

check-ci: CI=1
check-ci: format-check lint build test conformance codec-equivalence profile-parity fuzz-smoke ci-embedded-matrix

# ---------------------------------------------------------------------------
# clean
# ---------------------------------------------------------------------------
clean:
	rm -rf $(BUILD_DIR)
	@echo "[asx] clean complete"

# ---------------------------------------------------------------------------
# Help
# ---------------------------------------------------------------------------
.PHONY: help
help:
	@echo "asx build system — primary targets:"
	@echo ""
	@echo "  build              Build library (warnings-as-errors)"
	@echo "  format-check       Verify source formatting"
	@echo "  lint               Static analysis gate"
	@echo "  test               Run all tests (unit + invariant)"
	@echo "  test-unit          Unit tests per module"
	@echo "  test-invariants    Lifecycle invariant tests"
	@echo "  conformance        Rust fixture parity verification"
	@echo "  codec-equivalence  JSON vs BIN codec equivalence"
	@echo "  profile-parity     Cross-profile semantic digest parity"
	@echo "  fuzz-smoke         Differential fuzzing smoke test"
	@echo "  ci-embedded-matrix Cross-target embedded builds"
	@echo "  release            Optimized production build"
	@echo "  install            Install to PREFIX (default /usr/local)"
	@echo "  check              Combined gate (format+lint+build+test)"
	@echo "  clean              Remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  CC=gcc|clang       Compiler selection"
	@echo "  PROFILE=CORE|POSIX|WIN32|FREESTANDING|EMBEDDED_ROUTER|HFT|AUTOMOTIVE"
	@echo "  CODEC=JSON|BIN     Codec selection"
	@echo "  BITS=32|64         Target bitness"
	@echo "  TARGET=<triplet>   Cross-compilation target"
	@echo "  BUILD_TYPE=debug|release"
	@echo "  DETERMINISTIC=0|1  Deterministic scheduling mode"
