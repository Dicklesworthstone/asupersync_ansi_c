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
	src/core/cleanup.c \
	src/core/ghost.c \
	src/core/affinity.c \
	src/core/adaptive.c

RUNTIME_SRC := \
	src/runtime/hooks.c \
	src/runtime/lifecycle.c \
	src/runtime/scheduler.c \
	src/runtime/cancellation.c \
	src/runtime/quiescence.c \
	src/runtime/resource.c \
	src/runtime/trace.c \
	src/runtime/hindsight.c \
	src/runtime/telemetry.c \
	src/runtime/profile_compat.c \
	src/runtime/hft_instrument.c \
	src/runtime/automotive_instrument.c \
	src/runtime/overload_catalog.c \
	src/runtime/parallel.c

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
TEST_CFLAGS := $(ALL_CFLAGS) -I$(CURDIR)/tests -I$(CURDIR)/src

# ---------------------------------------------------------------------------
# E2E scripts
# ---------------------------------------------------------------------------
E2E_SCRIPT_DIR := tests/e2e
E2E_ALL_SCRIPTS := \
	$(E2E_SCRIPT_DIR)/core_lifecycle.sh \
	$(E2E_SCRIPT_DIR)/codec_parity.sh \
	$(E2E_SCRIPT_DIR)/robustness.sh \
	$(E2E_SCRIPT_DIR)/robustness_fault.sh \
	$(E2E_SCRIPT_DIR)/robustness_endian.sh \
	$(E2E_SCRIPT_DIR)/robustness_exhaustion.sh \
	$(E2E_SCRIPT_DIR)/hft_microburst.sh \
	$(E2E_SCRIPT_DIR)/automotive_watchdog.sh \
	$(E2E_SCRIPT_DIR)/continuity.sh \
	$(E2E_SCRIPT_DIR)/continuity_restart.sh

E2E_VERTICAL_SCRIPTS := \
	$(E2E_SCRIPT_DIR)/hft_microburst.sh \
	$(E2E_SCRIPT_DIR)/automotive_watchdog.sh \
	$(E2E_SCRIPT_DIR)/continuity.sh \
	$(E2E_SCRIPT_DIR)/continuity_restart.sh

# ===================================================================
# PRIMARY TARGETS — map 1:1 to quality gate commands
# ===================================================================

.PHONY: all build clean install uninstall
.PHONY: format-check lint lint-docs lint-checkpoint
.PHONY: test test-unit test-invariants test-e2e test-e2e-vertical
.PHONY: conformance codec-equivalence profile-parity
.PHONY: fuzz-smoke ci-embedded-matrix
.PHONY: release bench
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
# lint-docs — public API documentation coverage gate (bd-hwb.16)
# ---------------------------------------------------------------------------
lint-docs:
	@echo "[asx] lint-docs: checking public API documentation coverage..."
	@./tools/ci/check_api_docs.sh

# ---------------------------------------------------------------------------
# lint-checkpoint — checkpoint-coverage gate for kernel loops (bd-66l.6)
# ---------------------------------------------------------------------------
lint-checkpoint:
	@echo "[asx] lint-checkpoint: checking kernel loop checkpoint coverage..."
	@./tools/ci/check_checkpoint_coverage.sh

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

# Profile compat test needs extra source (profile_compat.c not yet in LIB_A)
$(TEST_DIR)/unit/runtime/test_profile_compat: tests/unit/runtime/test_profile_compat.c src/runtime/profile_compat.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/profile_compat.c $(LIB_A) $(ALL_LDFLAGS)

# HFT instrumentation test needs extra source (bd-j4m.3)
$(TEST_DIR)/unit/runtime/test_hft_instrument: tests/unit/runtime/test_hft_instrument.c src/runtime/hft_instrument.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/hft_instrument.c $(LIB_A) $(ALL_LDFLAGS)

# Automotive instrumentation test needs extra source (bd-j4m.4)
$(TEST_DIR)/unit/runtime/test_automotive_instrument: tests/unit/runtime/test_automotive_instrument.c src/runtime/automotive_instrument.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/automotive_instrument.c $(LIB_A) $(ALL_LDFLAGS)

# Overload catalog test needs extra sources (bd-j4m.8)
$(TEST_DIR)/unit/runtime/test_overload_catalog: tests/unit/runtime/test_overload_catalog.c src/runtime/overload_catalog.c src/runtime/hft_instrument.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/overload_catalog.c src/runtime/hft_instrument.c $(LIB_A) $(ALL_LDFLAGS)

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

# ---------------------------------------------------------------------------
# test-e2e — run all canonical e2e scenario lanes
# ---------------------------------------------------------------------------
test-e2e:
	@echo "[asx] test-e2e: running $(words $(E2E_ALL_SCRIPTS)) script(s)..."
	@pass=0; fail=0; \
	for s in $(E2E_ALL_SCRIPTS); do \
		echo "  RUN  $$(basename $$s)"; \
		if $$s; then \
			echo "  PASS $$(basename $$s)"; \
			pass=$$((pass + 1)); \
		else \
			echo "  FAIL $$(basename $$s)"; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo "[asx] test-e2e: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ] || exit 1

# ---------------------------------------------------------------------------
# test-e2e-vertical — run HFT/automotive/continuity e2e lanes
# ---------------------------------------------------------------------------
test-e2e-vertical:
	@echo "[asx] test-e2e-vertical: running $(words $(E2E_VERTICAL_SCRIPTS)) script(s)..."
	@pass=0; fail=0; \
	for s in $(E2E_VERTICAL_SCRIPTS); do \
		echo "  RUN  $$(basename $$s)"; \
		if $$s; then \
			echo "  PASS $$(basename $$s)"; \
			pass=$$((pass + 1)); \
		else \
			echo "  FAIL $$(basename $$s)"; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo "[asx] test-e2e-vertical: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ] || exit 1

# ---------------------------------------------------------------------------
# test-e2e-suite — run ALL e2e families via canonical aggregation script
#
# Emits unified run manifest with git rev, compiler/target, seed,
# profile/codec matrix, per-family results, and first-failure triage.
# Maps to hard gates: GATE-E2E-LIFECYCLE, GATE-E2E-CODEC,
# GATE-E2E-ROBUSTNESS, GATE-E2E-VERTICAL-{HFT,AUTO}, GATE-E2E-CONTINUITY.
# ---------------------------------------------------------------------------
test-e2e-suite: $(LIB_A)
	@chmod +x $(E2E_SCRIPT_DIR)/run_all.sh $(E2E_SCRIPT_DIR)/harness.sh $(E2E_ALL_SCRIPTS) 2>/dev/null || true
	@$(E2E_SCRIPT_DIR)/run_all.sh

$(TEST_DIR)/invariant/%: tests/invariant/%.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

test-dirs:
	@mkdir -p $(TEST_DIR)/unit/core $(TEST_DIR)/unit/runtime \
	          $(TEST_DIR)/unit/channel $(TEST_DIR)/unit/time \
	          $(TEST_DIR)/invariant/lifecycle $(TEST_DIR)/invariant/quiescence

# ---------------------------------------------------------------------------
# bench — performance benchmark suite (bd-1md.6)
#
# Compiles with -O2 for realistic performance measurements.
# Outputs JSON with p50/p95/p99/p99.9/p99.99 metrics.
# Usage:
#   make bench                    # Build and run (human-friendly)
#   make bench-json               # Build and run (JSON-only to stdout)
#   make bench-build              # Build only
# ---------------------------------------------------------------------------
BENCH_DIR  := $(BUILD_DIR)/bench
BENCH_SRC  := tests/bench/bench_runtime.c
BENCH_BIN  := $(BENCH_DIR)/bench_runtime

BENCH_CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -Werror \
                -Wno-unused-parameter -Wno-unused-result \
                -Wno-conversion -Wno-sign-conversion \
                -O2 -DNDEBUG \
                $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) \
                -I$(CURDIR)/tests -I$(CURDIR)/src

.PHONY: bench bench-json bench-build

bench-build: $(BENCH_BIN)

$(BENCH_BIN): $(BENCH_SRC) $(LIB_A) | $(BENCH_DIR)
	$(CC) $(BENCH_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

$(BENCH_DIR):
	@mkdir -p $@

bench: bench-build
	@echo "[asx] bench: running performance benchmarks..."
	@$(BENCH_BIN)
	@echo "[asx] bench: complete"

bench-json: bench-build
	@$(BENCH_BIN) --json

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
# fuzz — differential fuzzing harness (bd-1md.3)
#
# Generates random scenario DSL mutations, executes against C runtime,
# and verifies deterministic self-consistency. Compares against Rust
# reference fixtures when available.
#
# Usage:
#   make fuzz-build              # Build the fuzz harness
#   make fuzz-smoke              # CI smoke (100 iterations)
#   make fuzz-nightly            # Nightly (100000 iterations)
#   make fuzz-run FUZZ_ARGS="--seed 42 --iterations 5000"
# ---------------------------------------------------------------------------
FUZZ_DIR := $(BUILD_DIR)/fuzz
FUZZ_SRC := tests/fuzz/fuzz_differential.c
FUZZ_BIN := $(FUZZ_DIR)/fuzz_differential
FUZZ_ARGS ?=

FUZZ_CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -Werror \
               -Wno-unused-parameter -Wno-unused-result \
               -Wno-conversion -Wno-sign-conversion \
               -O2 -DNDEBUG \
               $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) \
               -I$(CURDIR)/tests -I$(CURDIR)/src

.PHONY: fuzz-build fuzz-smoke fuzz-nightly fuzz-run

fuzz-build: $(FUZZ_BIN)

$(FUZZ_BIN): $(FUZZ_SRC) $(LIB_A) | $(FUZZ_DIR)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

$(FUZZ_DIR):
	@mkdir -p $@

fuzz-smoke: fuzz-build
	@echo "[asx] fuzz-smoke: differential fuzzing smoke test..."
	@$(FUZZ_BIN) --smoke

fuzz-nightly: fuzz-build
	@echo "[asx] fuzz-nightly: differential fuzzing nightly run..."
	@$(FUZZ_BIN) --nightly --verbose

fuzz-run: fuzz-build
	@$(FUZZ_BIN) $(FUZZ_ARGS)

# ---------------------------------------------------------------------------
# minimize — deterministic counterexample minimizer (bd-1md.4)
#
# Reduces failing fuzz scenarios while preserving failure signatures.
# Uses delta debugging + single-op removal + argument simplification.
#
# Usage:
#   make minimize-build            # Build the minimizer
#   make minimize-selftest         # Run built-in self-test
#   make minimize-run MIN_ARGS="--failure-digest abc123 --verbose"
# ---------------------------------------------------------------------------
MIN_SRC  := tests/fuzz/fuzz_minimize.c
MIN_BIN  := $(FUZZ_DIR)/fuzz_minimize
MIN_ARGS ?=

.PHONY: minimize-build minimize-selftest minimize-run

minimize-build: $(MIN_BIN)

$(MIN_BIN): $(MIN_SRC) $(LIB_A) | $(FUZZ_DIR)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

minimize-selftest: minimize-build
	@echo "[asx] minimize-selftest: running minimizer self-test..."
	@$(MIN_BIN) --selftest --verbose

minimize-run: minimize-build
	@$(MIN_BIN) $(MIN_ARGS)

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
check: format-check lint lint-docs lint-checkpoint build test

check-ci: CI=1
check-ci: format-check lint lint-checkpoint build test test-e2e-vertical conformance codec-equivalence profile-parity fuzz-smoke ci-embedded-matrix

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
	@echo "  lint-docs          Public API documentation coverage gate"
	@echo "  lint-checkpoint    Checkpoint coverage gate for kernel loops"
	@echo "  test               Run all tests (unit + invariant)"
	@echo "  test-unit          Unit tests per module"
	@echo "  test-invariants    Lifecycle invariant tests"
	@echo "  test-e2e           Run all e2e scenario lanes"
	@echo "  test-e2e-vertical  Run HFT/automotive/continuity e2e lanes"
	@echo "  test-e2e-suite     Run ALL e2e families with unified manifest"
	@echo "  conformance        Rust fixture parity verification"
	@echo "  codec-equivalence  JSON vs BIN codec equivalence"
	@echo "  profile-parity     Cross-profile semantic digest parity"
	@echo "  fuzz-smoke         Differential fuzzing smoke test"
	@echo "  minimize-selftest  Counterexample minimizer self-test"
	@echo "  ci-embedded-matrix Cross-target embedded builds"
	@echo "  bench              Performance benchmarks (JSON output)"
	@echo "  bench-json         Benchmarks (JSON-only to stdout)"
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
