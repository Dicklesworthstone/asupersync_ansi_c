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
# Release artifact packaging defaults
# ---------------------------------------------------------------------------
RELEASE_TARGET ?= linux-x86_64
RELEASE_KIND ?= binary
RELEASE_VERSION ?=

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
DEP_FLAGS := -MMD -MP

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
	src/core/adaptive.c \
	src/core/combinator.c \
	src/core/combinator2.c \
	src/core/symbol.c \
	src/core/abi.c \
	src/core/epoch.c \
	src/core/circuit_breaker.c \
	src/core/join_set.c \
	src/core/config_types.c \
	src/core/epoch_combinators.c \
	src/core/det_hash.c

RUNTIME_SRC := \
	src/runtime/hooks.c \
	src/runtime/builder.c \
	src/runtime/rt.c \
	src/runtime/equivalence.c \
	src/runtime/lifecycle.c \
	src/runtime/scheduler.c \
	src/runtime/cancellation.c \
	src/runtime/quiescence.c \
	src/runtime/resource.c \
	src/runtime/trace.c \
	src/runtime/hindsight.c \
	src/runtime/telemetry.c \
	src/runtime/profile_compat.c \
	src/runtime/browser_boundary.c \
	src/runtime/browser_diagnostic.c \
	src/runtime/diagnostic.c \
	src/runtime/hft_instrument.c \
	src/runtime/automotive_instrument.c \
	src/runtime/overload_catalog.c \
	src/runtime/blocking.c \
	src/runtime/parallel.c \
	src/runtime/adapter.c \
	src/runtime/vertical_adapter.c \
	src/runtime/virtual_time.c \
	src/runtime/lab.c \
	src/runtime/local.c \
	src/runtime/io_driver.c \
	src/runtime/config_reload.c \
	src/runtime/regression_localize.c \
	src/runtime/replay.c \
	src/runtime/event_log.c \
	src/runtime/snapshot.c \
	src/runtime/waker.c \
	src/runtime/deadline_monitor.c

CHANNEL_SRC := \
	src/channel/mpsc.c \
	src/channel/session.c \
	src/channel/oneshot.c \
	src/channel/broadcast.c \
	src/channel/watch.c

SYNC_SRC := \
	src/sync/notify.c \
	src/sync/semaphore.c \
	src/sync/barrier.c \
	src/sync/once.c \
	src/sync/mutex.c \
	src/sync/rwlock.c \
	src/sync/contended_mutex.c \
	src/sync/pool.c

ACTOR_SRC := \
	src/actor/actor.c \
	src/actor/supervisor.c \
	src/actor/gen_server.c

NET_SRC := \
	src/net/net.c \
	src/net/tls.c \
	src/net/websocket.c \
	src/net/quic.c \
	src/net/server.c \
	src/net/http.c \
	src/net/web.c \
	src/net/grpc.c \
	src/net/db.c \
	src/net/messaging.c \
	src/net/distributed.c \
	src/net/pipe.c

BYTES_SRC := \
	src/bytes/buf.c \
	src/bytes/codec.c \
	src/bytes/io_adapter.c

ENCODING_SRC := \
	src/encoding/encoding.c

DECODING_SRC := \
	src/decoding/decoding.c

RAPTORQ_SRC := \
	src/raptorq/raptorq.c

MIGRATION_SRC := \
	src/migration/migration.c

TIME_SRC := \
	src/time/deadline.c \
	src/time/sleep.c \
	src/time/timer_wheel.c

SECURITY_SRC := \
	src/security/security.c \
	src/security/audit.c

STREAM_SRC := \
	src/stream/stream.c

FS_SRC := \
	src/fs/fs.c

PROCESS_SRC := \
	src/process/process.c

SIGNAL_SRC := \
	src/signal/signal.c

PLAN_SRC := \
	src/plan/plan.c

CX_SRC := \
	src/cx/cx.c \
	src/cx/scope.c

LINK_SRC := \
	src/link/link.c

EVIDENCE_SRC := \
	src/evidence/evidence.c

EVIDENCE_SINK_SRC := \
	src/evidence_sink/evidence_sink.c

MONITOR_SRC := \
	src/monitor/monitor.c

OBSERVABILITY_SRC := \
	src/observability/observability.c

APP_SRC := \
	src/app/app.c \
	src/app/doctor.c \
	src/app/report.c

CONSOLE_SRC := \
	src/console/console.c

TRACING_COMPAT_SRC := \
	src/tracing_compat/tracing_compat.c

SERVICE_SRC := \
	src/service/service.c

TRANSPORT_SRC := \
	src/transport/transport.c

REMOTE_SRC := \
	src/remote/remote.c

SPORK_SRC := \
	src/spork/spork.c

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

CLI_SRC := src/cli/cli.c

ABI_SRC := src/abi/wasm_abi.c

LIB_SRC := $(CORE_SRC) $(RUNTIME_SRC) $(CHANNEL_SRC) $(SYNC_SRC) $(ACTOR_SRC) $(NET_SRC) $(BYTES_SRC) $(ENCODING_SRC) $(DECODING_SRC) $(RAPTORQ_SRC) $(MIGRATION_SRC) $(TIME_SRC) $(SECURITY_SRC) $(STREAM_SRC) $(FS_SRC) $(PROCESS_SRC) $(SIGNAL_SRC) $(PLAN_SRC) $(CX_SRC) $(LINK_SRC) $(EVIDENCE_SRC) $(EVIDENCE_SINK_SRC) $(MONITOR_SRC) $(OBSERVABILITY_SRC) $(APP_SRC) $(CONSOLE_SRC) $(TRACING_COMPAT_SRC) $(SERVICE_SRC) $(TRANSPORT_SRC) $(REMOTE_SRC) $(SPORK_SRC) $(ABI_SRC) $(CLI_SRC) $(PLATFORM_SRC)

# ---------------------------------------------------------------------------
# Object files and output
# ---------------------------------------------------------------------------
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
LIB_DIR   := $(BUILD_DIR)/lib
BIN_DIR   := $(BUILD_DIR)/bin
TEST_DIR  := $(BUILD_DIR)/tests

LIB_OBJ := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(LIB_SRC))
LIB_DEP := $(LIB_OBJ:.o=.d)
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
                 $(wildcard tests/unit/session/*_test.c) \
                 $(wildcard tests/unit/session/test_*.c) \
                 $(wildcard tests/unit/link/*_test.c) \
                 $(wildcard tests/unit/link/test_*.c) \
                 $(wildcard tests/unit/record/*_test.c) \
                 $(wildcard tests/unit/record/test_*.c) \
                 $(wildcard tests/unit/time/*_test.c) \
                 $(wildcard tests/unit/time/test_*.c) \
                 $(wildcard tests/unit/cx/*_test.c) \
                 $(wildcard tests/unit/cx/test_*.c) \
                 $(wildcard tests/unit/security/*_test.c) \
                 $(wildcard tests/unit/security/test_*.c) \
                 $(wildcard tests/unit/fs/*_test.c) \
                 $(wildcard tests/unit/fs/test_*.c) \
                 $(wildcard tests/unit/process/*_test.c) \
                 $(wildcard tests/unit/process/test_*.c) \
                 $(wildcard tests/unit/signal/*_test.c) \
                 $(wildcard tests/unit/signal/test_*.c) \
                 $(wildcard tests/unit/stream/*_test.c) \
                 $(wildcard tests/unit/stream/test_*.c) \
                 $(wildcard tests/unit/sync/*_test.c) \
                 $(wildcard tests/unit/sync/test_*.c) \
                 $(wildcard tests/unit/plan/*_test.c) \
                 $(wildcard tests/unit/plan/test_*.c) \
                 $(wildcard tests/unit/app/*_test.c) \
                 $(wildcard tests/unit/app/test_*.c) \
                 $(wildcard tests/unit/console/*_test.c) \
                 $(wildcard tests/unit/console/test_*.c) \
                 $(wildcard tests/unit/evidence/*_test.c) \
                 $(wildcard tests/unit/evidence/test_*.c) \
                 $(wildcard tests/unit/monitor/*_test.c) \
                 $(wildcard tests/unit/monitor/test_*.c) \
                 $(wildcard tests/unit/tracing_compat/*_test.c) \
                 $(wildcard tests/unit/tracing_compat/test_*.c) \
                 $(wildcard tests/unit/bytes/*_test.c) \
                 $(wildcard tests/unit/bytes/test_*.c) \
                 $(wildcard tests/unit/encoding/*_test.c) \
                 $(wildcard tests/unit/encoding/test_*.c) \
                 $(wildcard tests/unit/decoding/*_test.c) \
                 $(wildcard tests/unit/decoding/test_*.c) \
                 $(wildcard tests/unit/actor/*_test.c) \
                 $(wildcard tests/unit/actor/test_*.c) \
                 $(wildcard tests/unit/raptorq/*_test.c) \
                 $(wildcard tests/unit/raptorq/test_*.c) \
                 $(wildcard tests/unit/migration/*_test.c) \
                 $(wildcard tests/unit/migration/test_*.c) \
                 $(wildcard tests/unit/service/*_test.c) \
                 $(wildcard tests/unit/service/test_*.c) \
                 $(wildcard tests/unit/transport/*_test.c) \
                 $(wildcard tests/unit/transport/test_*.c) \
                 $(wildcard tests/unit/remote/*_test.c) \
                 $(wildcard tests/unit/remote/test_*.c) \
                 $(wildcard tests/unit/spork/*_test.c) \
                 $(wildcard tests/unit/spork/test_*.c) \
                 $(wildcard tests/unit/net/*_test.c) \
                 $(wildcard tests/unit/net/test_*.c) \
                 $(wildcard tests/unit/abi/*_test.c) \
                 $(wildcard tests/unit/abi/test_*.c) \
                 $(wildcard tests/unit/cli/*_test.c) \
                 $(wildcard tests/unit/cli/test_*.c) \
                 $(wildcard tests/unit/evidence_sink/*_test.c) \
                 $(wildcard tests/unit/evidence_sink/test_*.c) \
                 $(wildcard tests/unit/fuzz/*_test.c) \
                 $(wildcard tests/unit/fuzz/test_*.c) \
                 $(wildcard tests/unit/observability/*_test.c) \
                 $(wildcard tests/unit/observability/test_*.c)
UNIT_TEST_SRC := $(sort $(UNIT_TEST_SRC))

INVARIANT_TEST_SRC := $(wildcard tests/invariant/lifecycle/*_test.c) \
                      $(wildcard tests/invariant/lifecycle/test_*.c) \
                      $(wildcard tests/invariant/quiescence/*_test.c) \
                      $(wildcard tests/invariant/quiescence/test_*.c)
INVARIANT_TEST_SRC := $(sort $(INVARIANT_TEST_SRC))

CONFORMANCE_TEST_SRC := $(wildcard tests/conformance/*_test.c)
CONFORMANCE_TEST_SRC := $(sort $(CONFORMANCE_TEST_SRC))

UNIT_TEST_BIN := $(patsubst tests/%.c,$(TEST_DIR)/%,$(UNIT_TEST_SRC))
INV_TEST_BIN  := $(patsubst tests/%.c,$(TEST_DIR)/%,$(INVARIANT_TEST_SRC))
CONFORMANCE_TEST_BIN := $(patsubst tests/%.c,$(TEST_DIR)/%,$(CONFORMANCE_TEST_SRC))
VIGNETTE_TEST_SRC := $(wildcard tests/vignettes/vignette_*.c)
VIGNETTE_TEST_SRC := $(sort $(VIGNETTE_TEST_SRC))
VIGNETTE_TEST_BIN := $(patsubst tests/%.c,$(TEST_DIR)/%,$(VIGNETTE_TEST_SRC))

# ---------------------------------------------------------------------------
# Test include path (adds tests/ for test_harness.h)
# ---------------------------------------------------------------------------
TEST_CFLAGS := $(ALL_CFLAGS) -I$(CURDIR)/tests -I$(CURDIR)/src
VIGNETTE_CFLAGS := $(STD_FLAGS) $(WARN_FLAGS) $(OPT_FLAGS) $(BITS_FLAGS) \
                   $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) $(CFLAGS)

CX_TEST_EXTRA_SRC := \
	src/bytes/buf.c \
	src/runtime/diagnostic.c \
	src/channel/oneshot.c \
	src/channel/broadcast.c \
	src/channel/watch.c \
	src/sync/notify.c \
	src/sync/semaphore.c \
	src/sync/barrier.c \
	src/sync/once.c \
	src/actor/actor.c \
	src/actor/supervisor.c \
	src/net/net.c

SECURITY_VIGNETTE_EXTRA_SRC := $(CX_TEST_EXTRA_SRC)
SECURITY_AUDIT_TEST_EXTRA_SRC := $(CX_TEST_EXTRA_SRC)
PUBLIC_FAMILY_TEST_EXTRA_SRC := $(CX_TEST_EXTRA_SRC)
APP_TEST_EXTRA_SRC := $(PUBLIC_FAMILY_TEST_EXTRA_SRC)
OPERATOR_TEST_EXTRA_SRC := $(PUBLIC_FAMILY_TEST_EXTRA_SRC)

# ---------------------------------------------------------------------------
# E2E scripts
# ---------------------------------------------------------------------------
E2E_SCRIPT_DIR := tests/e2e
E2E_ALL_SCRIPTS := \
	$(E2E_SCRIPT_DIR)/core_lifecycle.sh \
	$(E2E_SCRIPT_DIR)/codec_parity.sh \
	$(E2E_SCRIPT_DIR)/network_surface.sh \
	$(E2E_SCRIPT_DIR)/robustness.sh \
	$(E2E_SCRIPT_DIR)/robustness_fault.sh \
	$(E2E_SCRIPT_DIR)/robustness_endian.sh \
	$(E2E_SCRIPT_DIR)/robustness_exhaustion.sh \
	$(E2E_SCRIPT_DIR)/hft_microburst.sh \
	$(E2E_SCRIPT_DIR)/automotive_watchdog.sh \
	$(E2E_SCRIPT_DIR)/continuity.sh \
	$(E2E_SCRIPT_DIR)/continuity_restart.sh \
	$(E2E_SCRIPT_DIR)/native_host.sh \
	$(E2E_SCRIPT_DIR)/server_shutdown.sh \
	$(E2E_SCRIPT_DIR)/router_storm.sh \
	$(E2E_SCRIPT_DIR)/market_open_burst.sh \
	$(E2E_SCRIPT_DIR)/automotive_fault_burst.sh \
	$(E2E_SCRIPT_DIR)/openwrt_package.sh \
	$(E2E_SCRIPT_DIR)/foundational_contracts.sh \
	$(E2E_SCRIPT_DIR)/examples_smoke.sh \
	$(E2E_SCRIPT_DIR)/browser_smoke.sh

E2E_VERTICAL_SCRIPTS := \
	$(E2E_SCRIPT_DIR)/hft_microburst.sh \
	$(E2E_SCRIPT_DIR)/automotive_watchdog.sh \
	$(E2E_SCRIPT_DIR)/continuity.sh \
	$(E2E_SCRIPT_DIR)/continuity_restart.sh \
	$(E2E_SCRIPT_DIR)/router_storm.sh \
	$(E2E_SCRIPT_DIR)/market_open_burst.sh \
	$(E2E_SCRIPT_DIR)/automotive_fault_burst.sh

# ===================================================================
# PRIMARY TARGETS — map 1:1 to quality gate commands
# ===================================================================

.PHONY: all build clean install uninstall
.PHONY: format-check lint lint-docs lint-checkpoint lint-anti-butchering lint-evidence lint-semantic-delta lint-static-analysis lint-schema-validation
.PHONY: model-check
.PHONY: test test-unit test-browser-focused test-browser-minimal-focused test-invariants test-conformance-c test-vignettes test-e2e test-e2e-vertical test-abi-shim abi-check
.PHONY: formal-cbmc formal-algebraic formal-tv formal-litmus formal-codegen formal-check
.PHONY: check-evidence-bundle
.PHONY: conformance codec-equivalence profile-parity crate-acceptance-gate
.PHONY: fuzz-smoke ci-embedded-matrix
.PHONY: release release-artifacts bench
.PHONY: build-gcc build-clang build-msvc build-32 build-64
.PHONY: build-parallel build-browser
.PHONY: build-embedded-mipsel build-embedded-armv7 build-embedded-aarch64
.PHONY: cross-baremetal-arm-m4-free cross-baremetal-arm-m0-free
.PHONY: cross-baremetal-riscv32-free cross-baremetal-riscv64-free
.PHONY: cross-baremetal-arm-m4-router cross-baremetal-arm-m0-router
.PHONY: cross-baremetal-riscv32-router cross-baremetal-riscv64-router
.PHONY: cross-baremetal-all
.PHONY: qemu-smoke

all: build

# ---------------------------------------------------------------------------
# build — compile library with strict warnings-as-errors
# ---------------------------------------------------------------------------
build: $(LIB_A)
	@echo "[asx] build complete (profile=$(PROFILE) codec=$(CODEC) det=$(DETERMINISTIC))"

build-parallel:
	@$(MAKE) build PROFILE=PARALLEL

build-browser:
	@$(MAKE) build PROFILE=BROWSER

$(LIB_A): $(LIB_OBJ) | $(LIB_DIR)
	@tmp="$@.$$$$.tmp"; \
	$(AR) rcs "$$tmp" $^ && \
	mv "$$tmp" "$@"

$(OBJ_DIR)/%.o: src/%.c | obj-dirs
	$(CC) $(ALL_CFLAGS) $(DEP_FLAGS) -c -o $@ $<

-include $(LIB_DEP)

obj-dirs:
	@mkdir -p $(OBJ_DIR)/core $(OBJ_DIR)/runtime $(OBJ_DIR)/channel \
	          $(OBJ_DIR)/sync $(OBJ_DIR)/actor $(OBJ_DIR)/net \
	          $(OBJ_DIR)/bytes $(OBJ_DIR)/encoding $(OBJ_DIR)/decoding \
	          $(OBJ_DIR)/raptorq $(OBJ_DIR)/migration \
	          $(OBJ_DIR)/time $(OBJ_DIR)/security $(OBJ_DIR)/stream \
	          $(OBJ_DIR)/fs $(OBJ_DIR)/process $(OBJ_DIR)/signal \
	          $(OBJ_DIR)/link $(OBJ_DIR)/app $(OBJ_DIR)/console \
	          $(OBJ_DIR)/tracing_compat $(OBJ_DIR)/evidence \
	          $(OBJ_DIR)/evidence_sink $(OBJ_DIR)/monitor \
	          $(OBJ_DIR)/observability \
	          $(OBJ_DIR)/plan $(OBJ_DIR)/cx \
	          $(OBJ_DIR)/service $(OBJ_DIR)/transport \
	          $(OBJ_DIR)/remote $(OBJ_DIR)/spork $(OBJ_DIR)/cli \
	          $(OBJ_DIR)/platform/posix \
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
		         --suppress=normalCheckLevelMaxBranches \
		         --suppress=toomanyconfigs \
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
# lint-anti-butchering — semantic-sensitive proof-block gate (bd-66l.7)
# ---------------------------------------------------------------------------
lint-anti-butchering:
	@echo "[asx] lint-anti-butchering: checking guarantee impact proof block..."
	@./tools/ci/check_anti_butchering.sh

# ---------------------------------------------------------------------------
# lint-evidence — per-bead evidence linkage gate (bd-66l.9)
# ---------------------------------------------------------------------------
lint-evidence:
	@echo "[asx] lint-evidence: checking per-bead evidence linkage..."
	@if [ -x tools/ci/check_evidence_linkage.sh ]; then \
		tools/ci/check_evidence_linkage.sh; \
	else \
		echo "[asx] lint-evidence: SKIP (runner not found)"; \
	fi

# ---------------------------------------------------------------------------
# lint-static-analysis — section 10.7 static analysis gate (bd-66l.10)
# ---------------------------------------------------------------------------
lint-static-analysis:
	@echo "[asx] lint-static-analysis: section 10.7 gates..."
	@if [ -x tools/ci/run_static_analysis.sh ]; then \
		tools/ci/run_static_analysis.sh; \
	else \
		echo "[asx] lint-static-analysis: SKIP (runner not found)"; \
	fi

# ---------------------------------------------------------------------------
# lint-semantic-delta — semantic delta budget gate (bd-66l.3)
# ---------------------------------------------------------------------------
lint-semantic-delta:
	@echo "[asx] lint-semantic-delta: checking semantic delta budget..."
	@./tools/ci/check_semantic_delta_budget.sh

# ---------------------------------------------------------------------------
# lint-schema-validation — JSON schema validation gate (bd-16r)
# ---------------------------------------------------------------------------
lint-schema-validation:
	@echo "[asx] lint-schema-validation: validating fixture schemas..."
	@./tools/ci/validate_schemas.sh

# ---------------------------------------------------------------------------
# model-check — bounded model-check for state machine properties (bd-66l.10)
# ---------------------------------------------------------------------------
MODEL_CHECK_SRC := tests/invariant/model_check/test_bounded_model.c
MODEL_CHECK_BIN := $(BUILD_DIR)/test/invariant/model_check/test_bounded_model

$(MODEL_CHECK_BIN): $(MODEL_CHECK_SRC) $(LIB_A) | test-dirs
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

model-check: $(MODEL_CHECK_BIN)
	@echo "[asx] model-check: bounded state machine verification..."
	@$(MODEL_CHECK_BIN) && echo "  PASS test_bounded_model" || { echo "  FAIL test_bounded_model"; exit 1; }

# ---------------------------------------------------------------------------
# test — run all test suites
# ---------------------------------------------------------------------------
test: test-unit test-invariants test-conformance-c test-vignettes
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

# ---------------------------------------------------------------------------
# test-browser-focused — browser-profile focused shipped-surface suites
# ---------------------------------------------------------------------------
BROWSER_FOCUSED_TEST_BIN := \
	$(TEST_DIR)/unit/runtime/test_browser_boundary \
	$(TEST_DIR)/unit/runtime/test_browser_diagnostic \
	$(TEST_DIR)/unit/runtime/test_rt \
	$(TEST_DIR)/unit/runtime/test_blocking \
	$(TEST_DIR)/unit/runtime/test_io_driver \
	$(TEST_DIR)/unit/actor/test_actor \
	$(TEST_DIR)/unit/actor/test_gen_server \
	$(TEST_DIR)/unit/actor/test_supervisor \
	$(TEST_DIR)/unit/sync/test_sync \
	$(TEST_DIR)/unit/bytes/test_buf \
	$(TEST_DIR)/unit/bytes/test_codec \
	$(TEST_DIR)/unit/bytes/test_io_adapter \
	$(TEST_DIR)/unit/encoding/test_encoding \
	$(TEST_DIR)/unit/decoding/test_decoding \
	$(TEST_DIR)/unit/stream/test_stream \
	$(TEST_DIR)/unit/security/test_security \
	$(TEST_DIR)/unit/security/test_security_audit \
	$(TEST_DIR)/unit/plan/test_plan \
	$(TEST_DIR)/unit/cx/test_cx \
	$(TEST_DIR)/unit/cx/test_scope \
	$(TEST_DIR)/unit/link/test_link \
	$(TEST_DIR)/unit/app/test_app \
	$(TEST_DIR)/unit/app/test_report \
	$(TEST_DIR)/unit/cli/test_cli \
	$(TEST_DIR)/unit/console/test_console \
	$(TEST_DIR)/unit/evidence/test_evidence \
	$(TEST_DIR)/unit/fs/test_fs \
	$(TEST_DIR)/unit/monitor/test_monitor \
	$(TEST_DIR)/unit/process/test_process \
	$(TEST_DIR)/unit/record/test_record \
	$(TEST_DIR)/unit/migration/test_migration \
	$(TEST_DIR)/unit/raptorq/test_raptorq \
	$(TEST_DIR)/unit/signal/test_signal \
	$(TEST_DIR)/unit/spork/test_spork \
	$(TEST_DIR)/unit/tracing_compat/test_tracing_compat \
	$(TEST_DIR)/unit/net/test_net \
	$(TEST_DIR)/unit/net/test_server \
	$(TEST_DIR)/unit/net/test_http \
	$(TEST_DIR)/unit/net/test_web \
	$(TEST_DIR)/unit/net/test_grpc \
	$(TEST_DIR)/unit/net/test_messaging \
	$(TEST_DIR)/unit/net/test_websocket \
	$(TEST_DIR)/unit/net/test_quic \
	$(TEST_DIR)/unit/net/test_distributed \
	$(TEST_DIR)/unit/net/test_pipe \
	$(TEST_DIR)/unit/net/test_tls \
	$(TEST_DIR)/unit/net/test_db \
	$(TEST_DIR)/unit/service/test_service \
	$(TEST_DIR)/unit/service/test_service_stack \
	$(TEST_DIR)/unit/transport/test_transport \
	$(TEST_DIR)/unit/remote/test_remote

test-browser-focused:
	@echo "[asx] test-browser-focused: building and running browser-profile focused suites..."
	@$(MAKE) --no-print-directory PROFILE=BROWSER -B $(BROWSER_FOCUSED_TEST_BIN)
	@pass=0; fail=0; \
	for t in $(BROWSER_FOCUSED_TEST_BIN); do \
		echo "  RUN  $$(basename $$t)"; \
		if $$t; then \
			echo "  PASS $$(basename $$t)"; \
			pass=$$((pass + 1)); \
		else \
			echo "  FAIL $$(basename $$t)"; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo "[asx] test-browser-focused: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ] || exit 1

# ---------------------------------------------------------------------------
# test-browser-minimal-focused — minimal-browser hidden-contract suites
# ---------------------------------------------------------------------------
BROWSER_MINIMAL_FOCUSED_TEST_BIN := \
	$(TEST_DIR)/unit/runtime/test_browser_boundary \
	$(TEST_DIR)/unit/runtime/test_blocking \
	$(TEST_DIR)/unit/runtime/test_io_driver \
	$(TEST_DIR)/unit/cli/test_cli \
	$(TEST_DIR)/unit/fs/test_fs \
	$(TEST_DIR)/unit/process/test_process \
	$(TEST_DIR)/unit/signal/test_signal \
	$(TEST_DIR)/unit/net/test_server \
	$(TEST_DIR)/unit/net/test_grpc \
	$(TEST_DIR)/unit/net/test_messaging \
	$(TEST_DIR)/unit/net/test_tls \
	$(TEST_DIR)/unit/net/test_db \
	$(TEST_DIR)/unit/net/test_http \
	$(TEST_DIR)/unit/net/test_web \
	$(TEST_DIR)/unit/app/test_report \
	$(TEST_DIR)/unit/console/test_console \
	$(TEST_DIR)/unit/encoding/test_encoding \
	$(TEST_DIR)/unit/decoding/test_decoding \
	$(TEST_DIR)/unit/runtime/test_lab \
	$(TEST_DIR)/unit/runtime/test_telemetry \
	$(TEST_DIR)/unit/runtime/test_regression_localize \
	$(TEST_DIR)/unit/runtime/test_replay \
	$(TEST_DIR)/unit/tracing_compat/test_tracing_compat

test-browser-minimal-focused:
	@echo "[asx] test-browser-minimal-focused: building and running minimal-browser hidden-contract suites..."
	@$(MAKE) --no-print-directory PROFILE=BROWSER CFLAGS='-DASX_BROWSER_PROFILE_MINIMAL' -B $(BROWSER_MINIMAL_FOCUSED_TEST_BIN)
	@pass=0; fail=0; \
	for t in $(BROWSER_MINIMAL_FOCUSED_TEST_BIN); do \
		echo "  RUN  $$(basename $$t)"; \
		if $$t; then \
			echo "  PASS $$(basename $$t)"; \
			pass=$$((pass + 1)); \
		else \
			echo "  FAIL $$(basename $$t)"; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo "[asx] test-browser-minimal-focused: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ] || exit 1

# Profile compat test needs extra source (profile_compat.c not yet in LIB_A)
$(TEST_DIR)/unit/runtime/test_profile_compat: tests/unit/runtime/test_profile_compat.c src/runtime/profile_compat.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/profile_compat.c $(LIB_A) $(ALL_LDFLAGS)

# Diagnostic/replay tests still need explicit helper objects because
# lifecycle reset reaches non-shipped provider surfaces.
$(TEST_DIR)/unit/runtime/test_diagnostic: tests/unit/runtime/test_diagnostic.c $(CX_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(CX_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/runtime/test_replay: tests/unit/runtime/test_replay.c $(CX_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(CX_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

# Browser boundary test needs extra sources (bd-1eqo.16.1)
$(TEST_DIR)/unit/runtime/test_browser_boundary: tests/unit/runtime/test_browser_boundary.c src/runtime/browser_boundary.c src/runtime/profile_compat.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/browser_boundary.c src/runtime/profile_compat.c $(LIB_A) $(ALL_LDFLAGS)

# Browser diagnostic test needs extra sources (bd-1eqo.16.3)
$(TEST_DIR)/unit/runtime/test_browser_diagnostic: tests/unit/runtime/test_browser_diagnostic.c src/runtime/browser_diagnostic.c src/runtime/browser_boundary.c src/runtime/profile_compat.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/browser_diagnostic.c src/runtime/browser_boundary.c src/runtime/profile_compat.c $(LIB_A) $(ALL_LDFLAGS)

# HFT instrumentation test needs extra source (bd-j4m.3)
$(TEST_DIR)/unit/runtime/test_hft_instrument: tests/unit/runtime/test_hft_instrument.c src/runtime/hft_instrument.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/hft_instrument.c $(LIB_A) $(ALL_LDFLAGS)

# Automotive instrumentation test needs extra source (bd-j4m.4)
$(TEST_DIR)/unit/runtime/test_automotive_instrument: tests/unit/runtime/test_automotive_instrument.c src/runtime/automotive_instrument.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/automotive_instrument.c $(LIB_A) $(ALL_LDFLAGS)

# Overload catalog test needs extra sources (bd-j4m.8)
$(TEST_DIR)/unit/runtime/test_overload_catalog: tests/unit/runtime/test_overload_catalog.c src/runtime/overload_catalog.c src/runtime/hft_instrument.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/overload_catalog.c src/runtime/hft_instrument.c $(LIB_A) $(ALL_LDFLAGS)

# Adapter test needs extra sources (bd-j4m.5)
$(TEST_DIR)/unit/runtime/test_adapter: tests/unit/runtime/test_adapter.c src/runtime/adapter.c src/runtime/automotive_instrument.c src/runtime/hft_instrument.c src/runtime/overload_catalog.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/adapter.c src/runtime/automotive_instrument.c src/runtime/hft_instrument.c src/runtime/overload_catalog.c $(LIB_A) $(ALL_LDFLAGS)

# Vertical adapter test needs extra sources (bd-j4m.5)
$(TEST_DIR)/unit/runtime/test_vertical_adapter: tests/unit/runtime/test_vertical_adapter.c src/runtime/vertical_adapter.c src/runtime/automotive_instrument.c src/runtime/hft_instrument.c src/runtime/overload_catalog.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/vertical_adapter.c src/runtime/automotive_instrument.c src/runtime/hft_instrument.c src/runtime/overload_catalog.c $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/runtime/test_codec_equivalence: tests/unit/runtime/test_codec_equivalence.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/runtime/test_arena_locality: tests/unit/runtime/test_arena_locality.c src/runtime/arena_locality_spike.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/arena_locality_spike.c $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/runtime/test_barrier_cert: tests/unit/runtime/test_barrier_cert.c src/runtime/barrier_cert_spike.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/barrier_cert_spike.c $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/runtime/test_seqlock_ebr: tests/unit/runtime/test_seqlock_ebr.c src/runtime/seqlock_ebr_spike.c $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< src/runtime/seqlock_ebr_spike.c $(LIB_A) $(ALL_LDFLAGS)

# Link individual unit tests
$(TEST_DIR)/unit/cx/test_scope: tests/unit/cx/test_scope.c $(CX_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(CX_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/security/test_security_audit: tests/unit/security/test_security_audit.c $(SECURITY_AUDIT_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(SECURITY_AUDIT_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/channel/test_session: tests/unit/channel/test_session.c $(PUBLIC_FAMILY_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(PUBLIC_FAMILY_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/link/test_link: tests/unit/link/test_link.c $(PUBLIC_FAMILY_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(PUBLIC_FAMILY_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/record/test_record: tests/unit/record/test_record.c $(PUBLIC_FAMILY_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(PUBLIC_FAMILY_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/app/test_app: tests/unit/app/test_app.c $(APP_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(APP_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/app/test_report: tests/unit/app/test_report.c $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/console/test_console: tests/unit/console/test_console.c $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/tracing_compat/test_tracing_compat: tests/unit/tracing_compat/test_tracing_compat.c $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/evidence/test_evidence: tests/unit/evidence/test_evidence.c $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/monitor/test_monitor: tests/unit/monitor/test_monitor.c $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/%: tests/unit/%.c $(LIB_A) | test-dirs
	@mkdir -p $(@D)
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
# test-conformance-c — cross-codec / parity C conformance tests
# ---------------------------------------------------------------------------
test-conformance-c: $(CONFORMANCE_TEST_BIN)
	@echo "[asx] test-conformance-c: running $(words $(CONFORMANCE_TEST_BIN)) test(s)..."
	@if [ -z "$(strip $(CONFORMANCE_TEST_BIN))" ]; then \
		echo "[asx] test-conformance-c: no tests found (scaffold stage)"; \
	else \
		pass=0; fail=0; \
		for t in $(CONFORMANCE_TEST_BIN); do \
			echo "  RUN  $$(basename $$t)"; \
			if $$t; then \
				echo "  PASS $$(basename $$t)"; \
				pass=$$((pass + 1)); \
			else \
				echo "  FAIL $$(basename $$t)"; \
				fail=$$((fail + 1)); \
			fi; \
		done; \
		echo "[asx] test-conformance-c: $$pass passed, $$fail failed"; \
		[ $$fail -eq 0 ] || exit 1; \
	fi

# ---------------------------------------------------------------------------
# test-vignettes — compile and run API ergonomics usage vignettes
# ---------------------------------------------------------------------------
test-vignettes: $(VIGNETTE_TEST_BIN)
	@echo "[asx] test-vignettes: running $(words $(VIGNETTE_TEST_BIN)) vignette(s)..."
	@if [ -z "$(strip $(VIGNETTE_TEST_BIN))" ]; then \
		echo "[asx] test-vignettes: FAIL (no vignettes found)"; \
		exit 1; \
	else \
		pass=0; fail=0; \
		for t in $(VIGNETTE_TEST_BIN); do \
			echo "  RUN  $$(basename $$t)"; \
			if $$t; then \
				echo "  PASS $$(basename $$t)"; \
				pass=$$((pass + 1)); \
			else \
				echo "  FAIL $$(basename $$t)"; \
				fail=$$((fail + 1)); \
			fi; \
		done; \
		echo "[asx] test-vignettes: $$pass passed, $$fail failed"; \
		[ $$fail -eq 0 ] || exit 1; \
	fi

# ---------------------------------------------------------------------------
# test-abi-shim — consumer ABI/API stability verification (bd-56t.4)
# ---------------------------------------------------------------------------
ABI_SHIM_SRC := tests/abi/consumer_shim.c
ABI_SHIM_BIN := $(TEST_DIR)/abi/consumer_shim

$(ABI_SHIM_BIN): $(ABI_SHIM_SRC) $(LIB_A) | $(TEST_DIR)/abi
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -o $@ $< $(LIB_A)

$(TEST_DIR)/abi:
	@mkdir -p $@

test-abi-shim: $(ABI_SHIM_BIN)
	@echo "[asx] test-abi-shim: running consumer ABI/API stability shim..."
	@$(ABI_SHIM_BIN)
	@echo "[asx] test-abi-shim: PASS"

# ---------------------------------------------------------------------------
# abi-check — ABI break detection gate (bd-56t.4)
# ---------------------------------------------------------------------------
abi-check:
	@echo "[asx] abi-check: verifying ABI stability..."
	@tools/ci/check_abi_stability.sh --strict
	@echo "[asx] abi-check: PASS"

# ---------------------------------------------------------------------------
# formal-cbmc — CBMC-compatible transition harness verification (bd-3vt.1)
# ---------------------------------------------------------------------------
FORMAL_CBMC_DIR := tests/formal/cbmc
FORMAL_CBMC_SRCS := $(wildcard $(FORMAL_CBMC_DIR)/*_harness.c)
FORMAL_CBMC_BINS := $(patsubst $(FORMAL_CBMC_DIR)/%.c,$(TEST_DIR)/formal/%,$(FORMAL_CBMC_SRCS))

$(TEST_DIR)/formal:
	@mkdir -p $@

$(TEST_DIR)/formal/%: $(FORMAL_CBMC_DIR)/%.c src/core/transition_tables.c | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -o $@ $< src/core/transition_tables.c

# Cancel witness harness needs full library (cancel.c + runtime deps)
$(TEST_DIR)/formal/cancel_witness_harness: $(FORMAL_CBMC_DIR)/cancel_witness_harness.c $(LIB_A) | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -o $@ $< $(LIB_A)

formal-cbmc: $(FORMAL_CBMC_BINS)
	@echo "[asx] formal-cbmc: running $(words $(FORMAL_CBMC_BINS)) CBMC-compatible harness(es)..."
	@pass=0; fail=0; \
	for bin in $(FORMAL_CBMC_BINS); do \
		name=$$(basename $$bin); \
		if $$bin 2>&1; then \
			pass=$$((pass + 1)); \
		else \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo "[asx] formal-cbmc: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ] || exit 1
	@echo "[asx] formal-cbmc: PASS"

# ---------------------------------------------------------------------------
# formal-algebraic — algebraic property verification (bd-3vt.1)
# ---------------------------------------------------------------------------
FORMAL_ALG_DIR := tests/formal/algebraic
FORMAL_ALG_SRCS := $(wildcard $(FORMAL_ALG_DIR)/test_*.c)

$(TEST_DIR)/formal/test_outcome_lattice: $(FORMAL_ALG_DIR)/test_outcome_lattice.c src/core/outcome.c | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -o $@ $< src/core/outcome.c

$(TEST_DIR)/formal/test_cancel_monotone: $(FORMAL_ALG_DIR)/test_cancel_monotone.c $(LIB_A) | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -o $@ $< $(LIB_A)

$(TEST_DIR)/formal/test_budget_lattice: $(FORMAL_ALG_DIR)/test_budget_lattice.c src/core/budget.c | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -o $@ $< src/core/budget.c

$(TEST_DIR)/formal/test_foundational_parity: $(FORMAL_ALG_DIR)/test_foundational_parity.c $(LIB_A) | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -o $@ $< $(LIB_A)

$(TEST_DIR)/formal/test_resource_ownership_parity: $(FORMAL_ALG_DIR)/test_resource_ownership_parity.c $(LIB_A) | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -o $@ $< $(LIB_A)

$(TEST_DIR)/formal/test_circuit_breaker_sm: $(FORMAL_ALG_DIR)/test_circuit_breaker_sm.c $(LIB_A) | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -o $@ $< $(LIB_A)

FORMAL_ALG_BINS := $(TEST_DIR)/formal/test_outcome_lattice $(TEST_DIR)/formal/test_cancel_monotone $(TEST_DIR)/formal/test_budget_lattice $(TEST_DIR)/formal/test_foundational_parity $(TEST_DIR)/formal/test_resource_ownership_parity $(TEST_DIR)/formal/test_circuit_breaker_sm

formal-algebraic: $(FORMAL_ALG_BINS)
	@echo "[asx] formal-algebraic: running $(words $(FORMAL_ALG_BINS)) algebraic property suite(s)..."
	@pass=0; fail=0; \
	for bin in $(FORMAL_ALG_BINS); do \
		name=$$(basename $$bin); \
		if $$bin 2>&1; then \
			pass=$$((pass + 1)); \
		else \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo "[asx] formal-algebraic: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ] || exit 1
	@echo "[asx] formal-algebraic: PASS"

# ---------------------------------------------------------------------------
# formal-tv — translation validation (schema vs C code) (bd-3vt.1)
# ---------------------------------------------------------------------------
formal-tv:
	@echo "[asx] formal-tv: validating C code against invariant schema..."
	@tools/ci/check_translation_validation.sh --strict
	@echo "[asx] formal-tv: PASS"

# ---------------------------------------------------------------------------
# formal-litmus — memory-model litmus suite (bd-3vt.4)
# ---------------------------------------------------------------------------
FORMAL_LITMUS_SRC := tests/formal/litmus/test_memory_model_litmus.c
FORMAL_LITMUS_BIN := $(TEST_DIR)/formal/test_memory_model_litmus

$(FORMAL_LITMUS_BIN): $(FORMAL_LITMUS_SRC) $(LIB_A) | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -o $@ $< $(LIB_A)

formal-litmus: $(FORMAL_LITMUS_BIN)
	@echo "[asx] formal-litmus: running memory-model litmus suite..."
	@$(FORMAL_LITMUS_BIN)
	@echo "[asx] formal-litmus: PASS"

# ---------------------------------------------------------------------------
# formal-codegen — cross-optimization codegen stability (bd-3vt.4)
# ---------------------------------------------------------------------------
formal-codegen:
	@echo "[asx] formal-codegen: checking codegen stability across -O levels..."
	@tools/ci/check_codegen_stability.sh --strict
	@echo "[asx] formal-codegen: PASS"

# ---------------------------------------------------------------------------
# formal-check — all formal verification gates (bd-3vt.1, bd-3vt.4)
# ---------------------------------------------------------------------------
formal-check: formal-cbmc formal-algebraic formal-tv formal-litmus formal-codegen
	@echo "[asx] formal-check: all formal verification gates PASS"

# ---------------------------------------------------------------------------
# test-e2e — run all canonical e2e scenario lanes
# ---------------------------------------------------------------------------
test-e2e: test-e2e-suite
	@echo "[asx] test-e2e: canonical suite complete"

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
# Plus deployment/package gates: GATE-E2E-DEPLOY-{ROUTER,HFT,AUTO},
# GATE-E2E-PACKAGE.
# ---------------------------------------------------------------------------
test-e2e-suite: $(LIB_A)
	@chmod +x $(E2E_SCRIPT_DIR)/run_all.sh $(E2E_SCRIPT_DIR)/harness.sh $(E2E_ALL_SCRIPTS) 2>/dev/null || true
	@$(E2E_SCRIPT_DIR)/run_all.sh

$(TEST_DIR)/invariant/%: tests/invariant/%.c $(LIB_A) | test-dirs
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/conformance/%: tests/conformance/%.c $(LIB_A) | test-dirs
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/vignettes/%: tests/vignettes/%.c $(LIB_A) | test-dirs
	@mkdir -p $(@D)
	$(CC) $(VIGNETTE_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/vignettes/vignette_lifecycle: tests/vignettes/vignette_lifecycle.c $(CX_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(VIGNETTE_CFLAGS) -o $@ $< $(CX_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/vignettes/vignette_hooks: tests/vignettes/vignette_hooks.c $(CX_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(VIGNETTE_CFLAGS) -o $@ $< $(CX_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/vignettes/vignette_security: tests/vignettes/vignette_security.c $(SECURITY_VIGNETTE_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(VIGNETTE_CFLAGS) -o $@ $< $(SECURITY_VIGNETTE_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/vignettes/vignette_link: tests/vignettes/vignette_link.c $(PUBLIC_FAMILY_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(VIGNETTE_CFLAGS) -o $@ $< $(PUBLIC_FAMILY_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/vignettes/vignette_console: tests/vignettes/vignette_console.c $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(VIGNETTE_CFLAGS) -o $@ $< $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/vignettes/vignette_observability: tests/vignettes/vignette_observability.c $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(VIGNETTE_CFLAGS) -o $@ $< $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

test-dirs:
	@mkdir -p $(TEST_DIR)/unit/core $(TEST_DIR)/unit/runtime \
	          $(TEST_DIR)/unit/channel $(TEST_DIR)/unit/link \
	          $(TEST_DIR)/unit/record $(TEST_DIR)/unit/time \
	          $(TEST_DIR)/unit/bytes $(TEST_DIR)/unit/encoding \
	          $(TEST_DIR)/unit/decoding $(TEST_DIR)/unit/actor \
	          $(TEST_DIR)/unit/raptorq $(TEST_DIR)/unit/migration \
	          $(TEST_DIR)/unit/cx \
	          $(TEST_DIR)/unit/app $(TEST_DIR)/unit/console \
	          $(TEST_DIR)/unit/evidence $(TEST_DIR)/unit/monitor \
	          $(TEST_DIR)/unit/tracing_compat \
	          $(TEST_DIR)/unit/security $(TEST_DIR)/unit/fs \
	          $(TEST_DIR)/unit/process $(TEST_DIR)/unit/signal \
	          $(TEST_DIR)/unit/stream $(TEST_DIR)/unit/plan \
	          $(TEST_DIR)/invariant/lifecycle $(TEST_DIR)/invariant/quiescence \
	          $(TEST_DIR)/invariant/model_check \
	          $(TEST_DIR)/conformance \
	          $(TEST_DIR)/vignettes

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

.PHONY: slo-gate size-gate evidence-dashboard traceability-export
slo-gate: bench-build
	@echo "[asx] slo-gate: capturing benchmark and evaluating SLO baselines..."
	@mkdir -p build/perf
	@$(BENCH_BIN) --json > build/perf/bench-results.json
	@tools/ci/evaluate_slo_gates.sh \
		--bench-json build/perf/bench-results.json \
		--output build/perf/slo_gate_report.json \
		--strict
	@echo "[asx] slo-gate: complete (report: build/perf/slo_gate_report.json)"

size-gate: release bench-build
	@echo "[asx] size-gate: measuring binary size and cold-start metrics..."
	@mkdir -p build/perf
	@$(BENCH_BIN) --json > build/perf/bench-results.json
	@tools/ci/evaluate_size_gates.sh \
		--lib $(LIB_DIR)/libasx.a \
		--bench-json build/perf/bench-results.json \
		--output build/perf/size_gate_report.json \
		--strict
	@echo "[asx] size-gate: complete (report: build/perf/size_gate_report.json)"

evidence-dashboard: bench-build
	@echo "[asx] evidence-dashboard: collecting metrics and computing trends..."
	@mkdir -p build/perf
	@$(BENCH_BIN) --json > build/perf/bench-results.json
	@tools/ci/run_evidence_dashboard.sh \
		--bench-json build/perf/bench-results.json
	@echo "[asx] evidence-dashboard: complete"

traceability-export:
	@echo "[asx] traceability-export: generating machine-readable traceability index..."
	@tools/ci/generate_traceability_index.sh
	@echo "[asx] traceability-export: complete"

# ---------------------------------------------------------------------------
# check-evidence-bundle — certification evidence completeness
# ---------------------------------------------------------------------------
check-evidence-bundle:
	@echo "[asx] check-evidence-bundle: validating evidence completeness..."
	@tools/ci/check_evidence_bundle.sh --strict
	@echo "[asx] check-evidence-bundle: complete"

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
# crate-acceptance-gate — aggregate final crate-level parity evidence
# ---------------------------------------------------------------------------
crate-acceptance-gate:
	@echo "[asx] crate-acceptance-gate: aggregating crate-level parity evidence..."
	@python3 tools/ci/run_crate_acceptance_gate.py

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
#   make fuzz-smoke RUST_FUZZ_BINARY=path/to/rust_fuzz_target  # With Rust comparison
# ---------------------------------------------------------------------------
FUZZ_DIR := $(BUILD_DIR)/fuzz
FUZZ_SRC := tests/fuzz/fuzz_differential.c
FUZZ_BIN := $(FUZZ_DIR)/fuzz_differential
FUZZ_ARGS ?=
RUST_FUZZ_BINARY ?=
FUZZ_RUST_FLAG := $(if $(RUST_FUZZ_BINARY),--rust-binary $(RUST_FUZZ_BINARY),)

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
	@$(FUZZ_BIN) --smoke $(FUZZ_RUST_FLAG)

fuzz-nightly: fuzz-build
	@echo "[asx] fuzz-nightly: differential fuzzing nightly run..."
	@$(FUZZ_BIN) --nightly --verbose $(FUZZ_RUST_FLAG)

fuzz-run: fuzz-build
	@$(FUZZ_BIN) $(FUZZ_ARGS) $(FUZZ_RUST_FLAG)

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
# release-artifacts — deterministic tar.xz bundles + integrity metadata
# ---------------------------------------------------------------------------
release-artifacts:
	@version="$(RELEASE_VERSION)"; \
	if [ -z "$$version" ]; then \
		echo "[asx] release-artifacts: RELEASE_VERSION must be set (example: make release-artifacts RELEASE_VERSION=0.1.0 RELEASE_TARGET=linux-x86_64)"; \
		exit 1; \
	fi; \
	source_epoch="$(SOURCE_DATE_EPOCH)"; \
	if [ -z "$$source_epoch" ]; then \
		source_epoch="$$(git log -1 --format=%ct 2>/dev/null || date +%s)"; \
	fi; \
	tools/ci/run_release_artifacts.sh \
		--version "$$version" \
		--target "$(RELEASE_TARGET)" \
		--kind "$(RELEASE_KIND)" \
		--profile "$(PROFILE)" \
		--codec "$(CODEC)" \
		--deterministic "$(DETERMINISTIC)" \
		--source-date-epoch "$$source_epoch"

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
# Bare-metal cross-compilation targets (bd-aw69.4)
#
# These targets build static libraries for bare-metal ARM and RISC-V
# microcontrollers using arm-none-eabi-gcc and riscv64-unknown-elf-gcc.
# Each target gets a separate build directory under build/cross/<name>/.
# ---------------------------------------------------------------------------

# Generic bare-metal build recipe.
# Usage: $(call baremetal-build,<name>,<cc>,<ar>,<arch-flags>,<specs>,<profile>)
define baremetal-build
	@echo "[asx] cross-baremetal: building $(1) (profile=$(6))..."
	@if command -v $(2) >/dev/null 2>&1; then \
		bm_build="$(BUILD_DIR)/cross/$(1)"; \
		bm_obj="$$bm_build/obj"; \
		bm_lib="$$bm_build/lib"; \
		mkdir -p $$bm_obj/core $$bm_obj/runtime $$bm_obj/channel \
		         $$bm_obj/time $$bm_obj/security $$bm_obj/stream \
		         $$bm_obj/fs $$bm_obj/process $$bm_obj/signal \
		         $$bm_obj/plan \
		         $$bm_obj/platform/freestanding \
		         $$bm_lib; \
		for src in $(LIB_SRC); do \
			obj="$$bm_obj/$${src#src/}"; \
			obj="$${obj%.c}.o"; \
			$(2) $(STD_FLAGS) $(WARN_FLAGS) -Wno-type-limits -Wno-unused-value -Wno-format -O2 -DNDEBUG \
			     $(INC_FLAGS) -DASX_PROFILE_$(6) $(CODEC_DEF) $(DET_DEF) \
			     $(4) $(5) \
			     -c -o "$$obj" "$$src" || exit 1; \
		done; \
		$(3) rcs "$$bm_lib/libasx.a" $$(find "$$bm_obj" -name '*.o') && \
		echo "[asx] cross-baremetal: $(1) OK — $$bm_lib/libasx.a" && \
		size=$$(wc -c < "$$bm_lib/libasx.a") && \
		echo "[asx] cross-baremetal: $(1) size: $$size bytes"; \
	elif [ "$(FAIL_ON_MISSING_CROSS_TOOLCHAINS)" = "1" ]; then \
		echo "[asx] cross-baremetal: $(1) FAIL (toolchain not found; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] cross-baremetal: $(1) SKIP (toolchain not found)"; \
	fi
endef

# ARM Cortex-M4 — FREESTANDING
cross-baremetal-arm-m4-free:
	$(call baremetal-build,arm-m4-free,arm-none-eabi-gcc,arm-none-eabi-ar,-mcpu=cortex-m4 -mthumb,--specs=nosys.specs,FREESTANDING)

# ARM Cortex-M0 — FREESTANDING
cross-baremetal-arm-m0-free:
	$(call baremetal-build,arm-m0-free,arm-none-eabi-gcc,arm-none-eabi-ar,-mcpu=cortex-m0 -mthumb,--specs=nosys.specs,FREESTANDING)

# RISC-V 32-bit — FREESTANDING
cross-baremetal-riscv32-free:
	$(call baremetal-build,riscv32-free,riscv64-unknown-elf-gcc,riscv64-unknown-elf-ar,-march=rv32imac -mabi=ilp32,--specs=picolibc.specs,FREESTANDING)

# RISC-V 64-bit — FREESTANDING
cross-baremetal-riscv64-free:
	$(call baremetal-build,riscv64-free,riscv64-unknown-elf-gcc,riscv64-unknown-elf-ar,-march=rv64imac -mabi=lp64 -mcmodel=medany,--specs=picolibc.specs,FREESTANDING)

# ARM Cortex-M4 — EMBEDDED_ROUTER
cross-baremetal-arm-m4-router:
	$(call baremetal-build,arm-m4-router,arm-none-eabi-gcc,arm-none-eabi-ar,-mcpu=cortex-m4 -mthumb,--specs=nosys.specs,EMBEDDED_ROUTER)

# ARM Cortex-M0 — EMBEDDED_ROUTER
cross-baremetal-arm-m0-router:
	$(call baremetal-build,arm-m0-router,arm-none-eabi-gcc,arm-none-eabi-ar,-mcpu=cortex-m0 -mthumb,--specs=nosys.specs,EMBEDDED_ROUTER)

# RISC-V 32-bit — EMBEDDED_ROUTER
cross-baremetal-riscv32-router:
	$(call baremetal-build,riscv32-router,riscv64-unknown-elf-gcc,riscv64-unknown-elf-ar,-march=rv32imac -mabi=ilp32,--specs=picolibc.specs,EMBEDDED_ROUTER)

# RISC-V 64-bit — EMBEDDED_ROUTER
cross-baremetal-riscv64-router:
	$(call baremetal-build,riscv64-router,riscv64-unknown-elf-gcc,riscv64-unknown-elf-ar,-march=rv64imac -mabi=lp64 -mcmodel=medany,--specs=picolibc.specs,EMBEDDED_ROUTER)

# All 8 bare-metal targets
cross-baremetal-all: cross-baremetal-arm-m4-free cross-baremetal-arm-m0-free \
                     cross-baremetal-riscv32-free cross-baremetal-riscv64-free \
                     cross-baremetal-arm-m4-router cross-baremetal-arm-m0-router \
                     cross-baremetal-riscv32-router cross-baremetal-riscv64-router
	@echo "[asx] cross-baremetal-all: all 8 bare-metal targets complete"

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
.PHONY: check check-ci ci-embedded-baremetal
check: format-check lint lint-docs lint-checkpoint lint-anti-butchering lint-evidence lint-semantic-delta lint-static-analysis lint-schema-validation build test model-check abi-check test-abi-shim formal-check

check-ci: CI=1
check-ci: format-check lint lint-checkpoint lint-anti-butchering lint-evidence lint-semantic-delta lint-static-analysis lint-schema-validation build build-browser test-browser-focused test-browser-minimal-focused test model-check test-e2e-vertical conformance codec-equivalence profile-parity fuzz-smoke ci-embedded-matrix ci-embedded-baremetal

ci-embedded-baremetal:
	@echo "[asx] ci-embedded-baremetal: bare-metal gate..."
	@./tools/ci/gate_embedded_baremetal.sh

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
	@echo "  build-parallel     Build compile-only PARALLEL scaffold (deferred from Wave A gates)"
	@echo "  build-browser      Build library with PROFILE=BROWSER"
	@echo "  test-browser-focused Run browser-profile focused shipped-surface suites"
	@echo "  test-browser-minimal-focused Run minimal-browser hidden-contract suites"
	@echo "  format-check       Verify source formatting"
	@echo "  lint               Static analysis gate"
	@echo "  lint-docs          Public API documentation coverage gate"
	@echo "  lint-checkpoint    Checkpoint coverage gate for kernel loops"
	@echo "  lint-anti-butchering Anti-butchering proof-block gate"
	@echo "  lint-schema-validation JSON schema validation gate"
	@echo "  lint-evidence        Per-bead evidence linkage gate"
	@echo "  lint-semantic-delta  Semantic delta budget gate"
	@echo "  test               Run all tests (unit + invariant + vignettes)"
	@echo "  test-unit          Unit tests per module"
	@echo "  test-invariants    Lifecycle invariant tests"
	@echo "  test-vignettes     API ergonomics usage vignettes (public headers)"
	@echo "  test-e2e           Run all e2e scenario lanes"
	@echo "  test-e2e-vertical  Run HFT/automotive/continuity e2e lanes"
	@echo "  test-e2e-suite     Run ALL e2e families with unified manifest"
	@echo "  conformance        Rust fixture parity verification"
	@echo "  codec-equivalence  JSON vs BIN codec equivalence"
	@echo "  profile-parity     Cross-profile semantic digest parity"
	@echo "  crate-acceptance-gate Aggregate final crate-level parity evidence"
	@echo "  fuzz-smoke         Differential fuzzing smoke test"
	@echo "  minimize-selftest  Counterexample minimizer self-test"
	@echo "  ci-embedded-matrix Cross-target embedded builds (Linux-musl)"
	@echo "  cross-baremetal-all All 8 bare-metal ARM/RISC-V targets"
	@echo "  bench              Performance benchmarks (JSON output)"
	@echo "  bench-json         Benchmarks (JSON-only to stdout)"
	@echo "  release            Optimized production build"
	@echo "  release-artifacts  Build release tar.xz + checksum/signature/provenance bundles"
	@echo "  install            Install to PREFIX (default /usr/local)"
	@echo "  check              Combined gate (format+lint+build+test)"
	@echo "  clean              Remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  CC=gcc|clang       Compiler selection"
	@echo "  PROFILE=CORE|POSIX|WIN32|FREESTANDING|EMBEDDED_ROUTER|HFT|AUTOMOTIVE|PARALLEL|BROWSER"
	@echo "  CODEC=JSON|BIN     Codec selection"
	@echo "  BITS=32|64         Target bitness"
	@echo "  TARGET=<triplet>   Cross-compilation target"
	@echo "  BUILD_TYPE=debug|release"
	@echo "  DETERMINISTIC=0|1  Deterministic scheduling mode"
	@echo "  RELEASE_VERSION=x.y.z   Release version for release-artifacts target"
	@echo "  RELEASE_TARGET=<id>     Artifact id (default: linux-x86_64)"
	@echo "  RELEASE_KIND=binary|source"
