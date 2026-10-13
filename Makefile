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
# STRICT_GATES=1 turns every "tool missing -> SKIP" path into a failure.
# Agents running the AGENTS.md gates locally should pass STRICT_GATES=1.
STRICT_GATES ?= $(CI)
FAIL_ON_MISSING_FORMATTER ?= $(STRICT_GATES)
FAIL_ON_MISSING_LINTER ?= $(STRICT_GATES)
FAIL_ON_MISSING_RUNNERS ?= $(STRICT_GATES)
FAIL_ON_MISSING_CROSS_TOOLCHAINS ?= $(STRICT_GATES)
FAIL_ON_EMPTY_UNIT_TESTS ?= $(STRICT_GATES)
# Pinned formatter: the system clang-format is used only when it is this
# version; otherwise uvx fetches it, so local runs and CI agree.
CLANG_FORMAT_VERSION ?= 18.1.8
CLANG_FORMAT ?= $(shell if clang-format --version 2>/dev/null | grep -q 'version $(CLANG_FORMAT_VERSION)'; then \
	echo clang-format; elif command -v uvx >/dev/null 2>&1; then \
	echo "uvx --from clang-format==$(CLANG_FORMAT_VERSION) clang-format"; fi)
# Trees format-check covers (tests/gates point it at a scratch tree).
FORMAT_PATHS ?= include src tests
# Pinned analyzer: Cppcheck 2.22.0 (PyPI wheel cppcheck==1.5.3). Different
# cppcheck versions report different findings, so local runs and CI must use
# the same one; the system binary is used only when it is that version.
CPPCHECK_VERSION ?= 2.22.0
CPPCHECK_WHEEL ?= 1.5.3
CPPCHECK_JOBS ?= 8
CPPCHECK ?= $(shell if cppcheck --version 2>/dev/null | grep -q 'Cppcheck $(CPPCHECK_VERSION)'; then \
	echo cppcheck; elif command -v uvx >/dev/null 2>&1; then \
	echo "uvx --from cppcheck==$(CPPCHECK_WHEEL) cppcheck"; fi)
# Each ASX_MUTANT_<NAME> macro the sources test is a planted bug that only a
# gate's own build defines (tests/gates/test_*_mutants.sh). cppcheck explores
# every #ifdef configuration, so without -U it lints the planted code as if it
# shipped (src/net/net.c quad[qlen], arrayIndexOutOfBoundsCond). The macros
# are found by grep, so a new mutant is covered without an edit here;
# tools/ci/run_static_analysis.sh derives the same list.
CPPCHECK_UNDEF ?= $(addprefix -U,$(sort $(shell grep -rhoE 'ASX_MUTANT_[A-Za-z0-9_]+' src include 2>/dev/null)))
# Pinned clang-tidy for the section 10.7 static-analysis gate, resolved the
# same way as clang-format.
CLANG_TIDY_VERSION ?= 18.1.8
CLANG_TIDY ?= $(shell if clang-tidy --version 2>/dev/null | grep -q 'version $(CLANG_TIDY_VERSION)'; then \
	echo clang-tidy; elif command -v uvx >/dev/null 2>&1; then \
	echo "uvx --from clang-tidy==$(CLANG_TIDY_VERSION) clang-tidy"; fi)
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
# Build-time resource class (asx_config.h): RESOURCE_CLASS=1, 2 or 3 sizes
# the arenas from R1, R2 or R3. Empty: the unclassed defaults.
# ---------------------------------------------------------------------------
RESOURCE_CLASS ?=
CLASS_DEF := $(if $(RESOURCE_CLASS),-DASX_RESOURCE_CLASS=$(RESOURCE_CLASS),)

# ---------------------------------------------------------------------------
# Debug / Release mode
#
# Goals that run or size the benchmark binary measure the optimized
# library, so they default to release. The build-config stamp rebuilds
# every object when the flags change: without this, `make release` then
# `make bench` (the perf workflow) rebuilt the library at -O0 and timed
# and sized that.
# ---------------------------------------------------------------------------
BENCH_GOALS := bench bench-json bench-build parallel-bench-json parallel-bench-gate slo-gate \
               size-gate evidence-dashboard perf-compare perf-digest
ifneq ($(filter $(BENCH_GOALS),$(MAKECMDGOALS)),)
BUILD_TYPE ?= release
endif
BUILD_TYPE ?= debug
ifeq ($(BUILD_TYPE),release)
  # Each function and datum in a section of its own, so a link told to drop
  # what nothing references (GC_LDFLAGS below) can: an archive member is
  # otherwise all or nothing, and the kernel-only quickstart linked 316 KB
  # of .text it never calls (bd-dkfs). No unwind tables either: C code has no
  # exception to unwind (nothing here uses _Unwind, setjmp or pthread_exit),
  # and on an x86-64 or arm64 host they were 16 KB of the quickstart. A
  # release crash backtrace then needs a -g build, as a debug build has.
  OPT_FLAGS := -O2 -DNDEBUG -ffunction-sections -fdata-sections \
               -fno-asynchronous-unwind-tables -fno-unwind-tables
else
  OPT_FLAGS := -O0 -g -DASX_DEBUG=1
endif

# ---------------------------------------------------------------------------
# Release artifact packaging defaults
# ---------------------------------------------------------------------------
RELEASE_TARGET ?= linux-x86_64
RELEASE_KIND ?= binary
RELEASE_VERSION ?=
RELEASE_INSTALL_SMOKE_VERSION ?= 0.1.0
RELEASE_INSTALL_SMOKE_TARGET ?= $(RELEASE_TARGET)

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
              $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) $(CLASS_DEF) $(CFLAGS)

# The link half of the release sections: drop what nothing references. GNU
# ld, lld and gold (and the cross and MinGW toolchains) take --gc-sections;
# Apple's ld64, which a native Darwin build uses, spells it -dead_strip.
ifeq ($(BUILD_TYPE),release)
  ifeq ($(shell uname -s 2>/dev/null)$(TARGET),Darwin)
    GC_LDFLAGS := -Wl,-dead_strip
  else
    GC_LDFLAGS := -Wl,--gc-sections
  endif
endif
ALL_LDFLAGS := $(BITS_FLAGS) $(LDFLAGS) $(GC_LDFLAGS)

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
	src/core/det_hash.c

RUNTIME_SRC := \
	src/runtime/hooks.c \
	src/runtime/builder.c \
	src/runtime/rt.c \
	src/runtime/equivalence.c \
	src/runtime/lifecycle.c \
	src/runtime/scheduler.c \
	src/runtime/lab_dispatch.c \
	src/runtime/forced_schedule.c \
	src/runtime/task_group.c \
	src/runtime/cancellation.c \
	src/runtime/quiescence.c \
	src/runtime/resource.c \
	src/runtime/trace.c \
	src/runtime/hindsight.c \
	src/runtime/journal.c \
	src/runtime/journal_replay.c \
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
	src/runtime/replay.c src/runtime/oracle.c \
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
	src/sync/wait_queue.c \
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
	src/actor/supervisor.c

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
	src/security/crypto.c \
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

SESSION_LINK_SRC := \
	src/channel/session_link.c

EVIDENCE_SRC := \
	src/evidence/evidence.c

EVIDENCE_SINK_SRC := \
	src/evidence_sink/evidence_sink.c

HEALTH_SRC := \
	src/observability/health.c

MONITOR_SRC := \
	src/monitor/monitor.c

LINK_SRC := \
	src/link/link.c

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

# Rust-parity oracle (bridge W1.5): JSON DOM and canonical form for DSL v2
# scenarios and asx.fixture.v2 files. Private headers under src/conformance.
CONFORMANCE_SRC := \
	src/conformance/json.c \
	src/conformance/canon.c \
	src/conformance/interpreter.c

# Platform sources selected by profile
ifeq ($(PROFILE),POSIX)
  PLATFORM_SRC := src/platform/posix/hooks.c src/platform/posix/net_posix.c \
                  src/platform/posix/fs_posix.c src/platform/posix/process_posix.c \
                  src/platform/posix/signal_posix.c src/platform/posix/journal_posix.c
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

LIB_SRC := $(CORE_SRC) $(RUNTIME_SRC) $(CHANNEL_SRC) $(SYNC_SRC) $(ACTOR_SRC) $(NET_SRC) $(BYTES_SRC) $(ENCODING_SRC) $(DECODING_SRC) $(RAPTORQ_SRC) $(MIGRATION_SRC) $(TIME_SRC) $(SECURITY_SRC) $(STREAM_SRC) $(FS_SRC) $(PROCESS_SRC) $(SIGNAL_SRC) $(PLAN_SRC) $(CX_SRC) $(SESSION_LINK_SRC) $(EVIDENCE_SRC) $(EVIDENCE_SINK_SRC) $(HEALTH_SRC) $(MONITOR_SRC) $(LINK_SRC) $(OBSERVABILITY_SRC) $(APP_SRC) $(CONSOLE_SRC) $(TRACING_COMPAT_SRC) $(SERVICE_SRC) $(TRANSPORT_SRC) $(REMOTE_SRC) $(SPORK_SRC) $(CONFORMANCE_SRC) $(ABI_SRC) $(CLI_SRC) $(PLATFORM_SRC)

# ---------------------------------------------------------------------------
# Object files and output
# ---------------------------------------------------------------------------
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
LIB_DIR   := $(BUILD_DIR)/lib
BIN_DIR   := $(BUILD_DIR)/bin
TEST_DIR  := $(BUILD_DIR)/tests
BUILD_CONFIG_FILE := $(BUILD_DIR)/.build-config

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
                 $(wildcard tests/unit/platform/*_test.c) \
                 $(wildcard tests/unit/platform/test_*.c) \
                 $(wildcard tests/unit/evidence/*_test.c) \
                 $(wildcard tests/unit/evidence/test_*.c) \
                 $(wildcard tests/unit/monitor/*_test.c) \
                 $(wildcard tests/unit/monitor/test_*.c) \
                 $(wildcard tests/unit/link/*_test.c) \
                 $(wildcard tests/unit/link/test_*.c) \
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
                 $(wildcard tests/unit/observability/test_*.c) \
                 $(wildcard tests/unit/conformance/test_*.c)
# The task-less GenServer's test (bd-t20v): its subject left the library; the
# file leaves the tree with its git rm, and until then it is not built.
UNIT_TEST_SRC := $(filter-out tests/unit/actor/test_gen_server.c,$(sort $(UNIT_TEST_SRC)))

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
                   $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) $(CLASS_DEF) $(CFLAGS)

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
	$(E2E_SCRIPT_DIR)/nested_regions.sh \
	$(E2E_SCRIPT_DIR)/codec_parity.sh \
	$(E2E_SCRIPT_DIR)/network_surface.sh \
	$(E2E_SCRIPT_DIR)/actor_supervision.sh \
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
	$(E2E_SCRIPT_DIR)/http_robustness.sh \
	$(E2E_SCRIPT_DIR)/parallel_swarm.sh \
	$(E2E_SCRIPT_DIR)/router_storm.sh \
	$(E2E_SCRIPT_DIR)/market_open_burst.sh \
	$(E2E_SCRIPT_DIR)/automotive_fault_burst.sh \
	$(E2E_SCRIPT_DIR)/openwrt_package.sh \
	$(E2E_SCRIPT_DIR)/foundational_contracts.sh \
	$(E2E_SCRIPT_DIR)/examples_smoke.sh \
	$(E2E_SCRIPT_DIR)/native_record_replay.sh \
	$(E2E_SCRIPT_DIR)/native_crash_continuity.sh \
	$(E2E_SCRIPT_DIR)/raptorq_erasure.sh \
	$(E2E_SCRIPT_DIR)/cli_smoke.sh \
	$(E2E_SCRIPT_DIR)/browser_smoke.sh
# This list is the families tests/e2e/run_all.sh runs (E2E_FAMILIES);
# tests/ci/test_e2e_suite_registry.sh holds the two together. Not a family:
# wave_c_acceptance_demo.sh, an aggregator that reruns other gates under its
# own target (wave-c-acceptance-demo); its chmod is in that target.

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

.PHONY: all build clean install uninstall FORCE conformance-runner
.PHONY: format-check lint lint-docs lint-checkpoint lint-anti-butchering lint-evidence lint-semantic-delta lint-static-analysis lint-schema-validation lint-scenarios-v2
.PHONY: model-check fixture-integrity fixtures-promote test-gates test-capacity-x4 test-resource-classes
.PHONY: print-capacity-macros capacity-table
.PHONY: test-asan test-asan-live test-tsan
.PHONY: test test-unit test-combinator-contract test-actor-supervision-harness test-browser-focused test-browser-minimal-focused test-invariants test-conformance-c test-vignettes examples test-examples test-e2e test-e2e-vertical test-e2e-parallel test-e2e-posix-adapter test-e2e-network-surface test-e2e-native-record-replay test-e2e-native-crash-continuity test-e2e-actor-supervision wave-c-acceptance-demo test-abi-shim abi-check
.PHONY: formal-cbmc formal-algebraic formal-tv formal-litmus formal-codegen formal-check
.PHONY: check-evidence-bundle
.PHONY: conformance codec-equivalence profile-parity parallel-parity crate-acceptance-gate resource-pressure-gate
.PHONY: fuzz-smoke ci-embedded-matrix embedded-portability-proof
.PHONY: release release-artifacts release-install-smoke bench
.PHONY: build-gcc build-clang build-msvc build-32 build-64
.PHONY: build-posix build-win32 build-parallel build-browser test-win32-hooks-build
.PHONY: build-wasm32 wasm32-module test-wasm32-replay
.PHONY: build-embedded-mipsel build-embedded-armv7 build-embedded-aarch64
.PHONY: cross-baremetal-arm-m4-free cross-baremetal-arm-m0-free
.PHONY: cross-baremetal-riscv32-free cross-baremetal-riscv64-free
.PHONY: cross-baremetal-arm-m4-router cross-baremetal-arm-m0-router
.PHONY: cross-baremetal-riscv32-router cross-baremetal-riscv64-router
.PHONY: cross-baremetal-all
.PHONY: qemu-smoke
.PHONY: cli

all: build

# ---------------------------------------------------------------------------
# build — compile library with strict warnings-as-errors
# ---------------------------------------------------------------------------
build: $(LIB_A)
	@echo "[asx] build complete (profile=$(PROFILE) codec=$(CODEC) det=$(DETERMINISTIC))"

build-posix:
	@$(MAKE) build PROFILE=POSIX DETERMINISTIC=0 CFLAGS="$(CFLAGS) -DASX_LOCKFREE_SINGLE_THREAD=0" LDFLAGS="-lpthread -lrt"

build-win32:
	@$(MAKE) build PROFILE=WIN32 CC="x86_64-w64-mingw32-gcc" CFLAGS="-DASX_BUILDING_DLL" LDFLAGS="-lbcrypt"

test-win32-hooks-build:
	@if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then \
		if [ "$(FAIL_ON_MISSING_CROSS_TOOLCHAINS)" = "1" ]; then \
			echo "[asx] test-win32-hooks-build: FAIL (x86_64-w64-mingw32-gcc missing)"; \
			exit 1; \
		fi; \
		echo "[asx] test-win32-hooks-build: SKIP (x86_64-w64-mingw32-gcc missing)"; \
	else \
		$(MAKE) PROFILE=WIN32 CC="x86_64-w64-mingw32-gcc" CFLAGS="-DASX_BUILDING_DLL" LDFLAGS="-lbcrypt" $(TEST_DIR)/unit/platform/test_win32_hooks; \
	fi

build-parallel:
	@$(MAKE) build PROFILE=PARALLEL CFLAGS="$(CFLAGS) -DASX_LOCKFREE_SINGLE_THREAD=0"

build-browser:
	@$(MAKE) build PROFILE=BROWSER

cli: $(LIB_A) src/cli/main.c
	@mkdir -p $(BUILD_DIR)/bin
	$(CC) $(ALL_CFLAGS) -o $(BUILD_DIR)/bin/asx src/cli/main.c $(LIB_A) $(LDFLAGS)
	@echo "[asx] cli binary: $(BUILD_DIR)/bin/asx"

# Rust-parity oracle runner (bridge W1.5/W1.6): executes DSL v2 scenarios
# through this runtime and compares them with Rust captures. It links a
# library built with the R3 arenas (64 regions, 256 tasks; asx_config.h) in
# its own build directory: Rust has no fixed capacity, so a scenario must
# never meet the unclassed build's 8-region arena, a footprint setting and
# not semantics (a supervisor opens a region per generation).
CONFORMANCE_RUNNER := $(BIN_DIR)/asx-conformance
CONFORMANCE_BUILD_DIR := $(BUILD_DIR)/conformance-r3
CONFORMANCE_LIB := $(CONFORMANCE_BUILD_DIR)/lib/libasx.a

conformance-runner: $(CONFORMANCE_RUNNER)

.PHONY: conformance-lib
conformance-lib:
	@$(MAKE) --no-print-directory BUILD_DIR=$(CONFORMANCE_BUILD_DIR) RESOURCE_CLASS=3 \
		$(CONFORMANCE_LIB)

$(CONFORMANCE_RUNNER): tools/conformance/runner.c conformance-lib | $(BIN_DIR)
	$(CC) $(filter-out -DASX_RESOURCE_CLASS=%,$(ALL_CFLAGS)) -DASX_RESOURCE_CLASS=3 \
		-I$(CURDIR)/src -o $@ $< $(CONFORMANCE_LIB) $(ALL_LDFLAGS)

$(LIB_A): $(LIB_OBJ) | $(LIB_DIR)
	@tmp="$@.$$$$.tmp"; \
	$(AR) rcs "$$tmp" $^ && \
	mv "$$tmp" "$@"

# Rebuild objects whenever the effective build configuration changes so
# profile/browser-mode/compiler-flag mixes cannot silently reuse stale objects.
$(BUILD_CONFIG_FILE): FORCE | obj-dirs
	@new_contents="$$(printf '%s\n%s\n%s\n' \
		'CC=$(CC)' \
		'ALL_CFLAGS=$(ALL_CFLAGS)' \
		'ALL_LDFLAGS=$(ALL_LDFLAGS)')"; \
	old_contents="$$(cat "$@" 2>/dev/null || true)"; \
	if [ "$$old_contents" != "$$new_contents" ]; then \
		printf '%s\n' "$$new_contents" > "$@"; \
	fi

$(OBJ_DIR)/%.o: src/%.c $(BUILD_CONFIG_FILE) | obj-dirs
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
	          $(OBJ_DIR)/plan $(OBJ_DIR)/cx $(OBJ_DIR)/abi \
	          $(OBJ_DIR)/service $(OBJ_DIR)/transport \
	          $(OBJ_DIR)/remote $(OBJ_DIR)/spork $(OBJ_DIR)/cli $(OBJ_DIR)/conformance \
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
	@if [ -n "$(CLANG_FORMAT)" ]; then \
		find $(FORMAT_PATHS) \( -name '*.c' -o -name '*.h' \) -print0 | \
		xargs -0 $(CLANG_FORMAT) --dry-run --Werror 2>&1 && \
		echo "[asx] format-check: PASS (clang-format $(CLANG_FORMAT_VERSION))" || \
		{ echo "[asx] format-check: FAIL — run $(CLANG_FORMAT) -i on the files above"; exit 1; }; \
	elif [ "$(FAIL_ON_MISSING_FORMATTER)" = "1" ]; then \
		echo "[asx] format-check: FAIL (clang-format $(CLANG_FORMAT_VERSION) not found and no uvx; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] format-check: SKIP (clang-format $(CLANG_FORMAT_VERSION) not found and no uvx; use STRICT_GATES=1 to fail)"; \
	fi

# ---------------------------------------------------------------------------
# lint — static analysis gate
# ---------------------------------------------------------------------------
lint:
	@echo "[asx] lint: ambient clock/entropy only in the cataloged providers..."
	@python3 tools/ci/check_ambient_authority.py
	@echo "[asx] lint: running static analysis..."
	@if [ -n "$(CPPCHECK)" ]; then \
		$(CPPCHECK) -j $(CPPCHECK_JOBS) --enable=warning,performance,portability --std=c99 \
		         --error-exitcode=1 \
		         --suppress=missingIncludeSystem \
		         --suppress=unusedFunction \
		         --suppress=normalCheckLevelMaxBranches \
		         --suppress=toomanyconfigs \
		         $(CPPCHECK_UNDEF) \
		         -I include src/ && \
		echo "[asx] lint: PASS (cppcheck $(CPPCHECK_VERSION))" || \
		{ echo "[asx] lint: FAIL"; exit 1; }; \
	elif [ "$(FAIL_ON_MISSING_LINTER)" = "1" ]; then \
		echo "[asx] lint: FAIL (cppcheck $(CPPCHECK_VERSION) not found and no uvx; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] lint: SKIP (cppcheck $(CPPCHECK_VERSION) not found and no uvx; use STRICT_GATES=1 to fail)"; \
	fi

# ---------------------------------------------------------------------------
# lint-docs — public API documentation coverage gate (bd-hwb.16)
# ---------------------------------------------------------------------------
lint-docs: $(LIB_A)
	@echo "[asx] lint-docs: checking public API documentation coverage..."
	@./tools/ci/check_api_docs.sh
	@echo "[asx] lint-docs: compiling the README's C samples..."
	@CC="$(CC)" ./tools/ci/check_readme_samples.sh README.md $(LIB_A) $(BUILD_DIR)/readme_samples $(OPT_FLAGS) $(BITS_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) $(CLASS_DEF) $(CFLAGS) $(ALL_LDFLAGS)
	@echo "[asx] lint-docs: checking the README's inventory numbers..."
	@./tools/ci/check_readme_facts.sh README.md
	@echo "[asx] lint-docs: checking the C refinement map against the v4 rule index..."
	@./tools/ci/check_refinement_map.sh
	@echo "[asx] lint-docs: checking the parity docs' citations against the tree..."
	@python3 tools/ci/check_doc_citations.py
	@echo "[asx] lint-docs: checking the README's capacity table against the headers..."
	@./tools/ci/capacity_table.sh --check README.md --out-dir $(BUILD_DIR)/capacity-table \
		-- $(CC) $(ALL_CFLAGS)

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
	@tools/ci/check_evidence_linkage.sh

# ---------------------------------------------------------------------------
# lint-static-analysis — section 10.7 static analysis gate (bd-66l.10)
# ---------------------------------------------------------------------------
lint-static-analysis:
	@echo "[asx] lint-static-analysis: section 10.7 gates..."
	@CPPCHECK="$(CPPCHECK)" CLANG_TIDY="$(CLANG_TIDY)" \
		FAIL_ON_MISSING_LINTER="$(FAIL_ON_MISSING_LINTER)" tools/ci/run_static_analysis.sh

# ---------------------------------------------------------------------------
# lint-gcc-analyzer — GCC -fanalyzer over src/ in CORE and POSIX live
# (bd-9kll.16.4, plan 10.7.2). Not part of `check`: its waivers were triaged
# with GCC 15.2 (docs/STATIC_ANALYSIS_TRIAGE.md), so CI runs it in the
# gcc:15.2.0 image (ci.yml, job gcc-analyzer); another GCC warns locally and
# fails under STRICT_GATES=1. Needs GCC with -fanalyzer (10+).
# ---------------------------------------------------------------------------
.PHONY: lint-gcc-analyzer
lint-gcc-analyzer:
	@STRICT_GATES="$(STRICT_GATES)" tools/ci/run_gcc_analyzer.sh

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
	@./tools/ci/validate_schemas.sh $(if $(filter 1,$(FAIL_ON_MISSING_LINTER)),--strict)

# lint-scenarios-v2 — the oracle's normative schemas (vocabulary v2, scenario
# DSL v2) are valid JSON Schema, and every DSL v2 scenario and the vocabulary
# example validate (bd-9kll.2.2, bd-9kll.2.3). Pinned check-jsonschema.
CHECK_JSONSCHEMA_VERSION ?= 0.38.2
CHECK_JSONSCHEMA ?= $(shell if check-jsonschema --version 2>/dev/null | grep -q 'version $(CHECK_JSONSCHEMA_VERSION)'; then \
	echo check-jsonschema; elif command -v uvx >/dev/null 2>&1; then \
	echo "uvx --from check-jsonschema==$(CHECK_JSONSCHEMA_VERSION) check-jsonschema"; fi)
lint-scenarios-v2:
	@echo "[asx] lint-scenarios-v2: vocabulary and scenario DSL v2 schemas..."
	@if [ -z "$(CHECK_JSONSCHEMA)" ]; then \
		if [ "$(FAIL_ON_MISSING_LINTER)" = "1" ]; then \
			echo "[asx] lint-scenarios-v2: FAIL (check-jsonschema $(CHECK_JSONSCHEMA_VERSION) not found and no uvx; strict mode)"; exit 1; \
		fi; \
		echo "[asx] lint-scenarios-v2: SKIP (check-jsonschema not found; use STRICT_GATES=1 to fail)"; exit 0; \
	fi
	@$(CHECK_JSONSCHEMA) --check-metaschema schemas/canonical_vocabulary_v2.json schemas/scenario_dsl_v2.json
	@$(CHECK_JSONSCHEMA) --schemafile schemas/scenario_dsl_v2.json tests/conformance/scenarios_v2/*.json
	@# A step on an obligation or send permit the task does not hold observes
	@# ASX_E_NOT_FOUND (DSL §3), so a misspelt name would pass silently: every
	@# name such a step uses must be acquired by reserve / reserve_send / handoff.
	@for f in tests/conformance/scenarios_v2/*.json; do \
		missing=$$(jq -r '([.. | objects | select(.op == "reserve" or .op == "reserve_send" or .op == "handoff") | .as]) as $$acq | ([.. | objects | select(.op == "commit" or .op == "abort" or .op == "leak" or .op == "handoff") | .obligation] + [.. | objects | select(.op == "permit_send" or .op == "permit_abort") | .permit]) | map(select(. as $$n | ($$acq | index($$n)) == null)) | unique | .[]' "$$f"); \
		if [ -n "$$missing" ]; then \
			echo "[asx] lint-scenarios-v2: FAIL $$f names never acquired: $$missing"; exit 1; \
		fi; \
	done
	@mkdir -p build/conformance
	@jq '.examples[0]' schemas/canonical_vocabulary_v2.json > build/conformance/vocab_v2_example.json
	@$(CHECK_JSONSCHEMA) --schemafile schemas/canonical_vocabulary_v2.json build/conformance/vocab_v2_example.json
	@echo "[asx] lint-scenarios-v2: PASS ($$(ls tests/conformance/scenarios_v2/*.json | wc -l | tr -d ' ') scenarios)"

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
test: test-unit test-invariants test-conformance-c test-vignettes test-examples fuzz-parsers-replay test-config-fingerprint test-cli-info
	@echo "[asx] test: all suites passed"

# ---------------------------------------------------------------------------
# test-unit — per-module correctness tests
#
# TEST_EXEC prefixes each test binary (and the conformance runner), e.g. an
# emulator for a cross build:
#   make test-unit TARGET=mips-linux-gnu PROFILE=EMBEDDED_ROUTER \
#     BUILD_DIR=build/qemu-mips LDFLAGS=-static TEST_EXEC=qemu-mips-static
# ---------------------------------------------------------------------------
TEST_EXEC ?=
test-unit: $(UNIT_TEST_BIN)
	@echo "[asx] test-unit: running $(words $(UNIT_TEST_BIN)) test(s)..."
	@# tests/test_log.h writes structured JSONL only into an existing directory.
	@mkdir -p build/test-logs
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
			if $(TEST_EXEC) $$t; then \
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

# metamorphic — the relations M1 (permute independent tasks) and M2 (add a
# no-op task) over tests/conformance/scenarios_v2 on the C interpreter
# (bd-9kll.12.1). The same binary runs in test-unit; this runs it alone.
# tests/gates/test_metamorphic_mutants.sh plants an order dependence and
# expects both relations to report it.
.PHONY: metamorphic
metamorphic: $(TEST_DIR)/unit/conformance/test_metamorphic
	@echo "[asx] metamorphic: M1 and M2 over tests/conformance/scenarios_v2..."
	@$(TEST_EXEC) $(TEST_DIR)/unit/conformance/test_metamorphic
	@echo "[asx] metamorphic: PASS"

# fuzz-metamorphic — M1 to M4 over scenarios generated now (bd-9kll.12.1):
# `twin_run generate --shape meta --jsonl` streams scenarios (half are M3
# cases, a nested-region budget triple; the rest are for M1, M2 and M4) into
# `test_metamorphic --fuzz`, which checks each until every relation has
# MM_FUZZ_CASES compared cases (10000 by default; MM_FUZZ_COUNT scenarios are
# offered, enough for them). A violation is reduced by ddmin (delete tasks,
# steps, regions, script entries, ... while the relation still fails) and
# written to $(MM_FUZZ_DIR)/mm-<relation>-min-NNN-001.json, a scenario ready
# to become a regression fixture once the drift is understood. Exit 1 on a
# violation, 2 when the stream ended short of the cases (a generator that did
# not run proves nothing). Needs cargo and the pinned asupersync like
# fuzz-differential; no Rust capture is involved, only the C interpreter.
MM_FUZZ_SEED ?= 12100
MM_FUZZ_CASES ?= 10000
MM_FUZZ_COUNT ?= $(shell expr $(MM_FUZZ_CASES) \* 3 + 200)
MM_FUZZ_DIR := $(BUILD_DIR)/metamorphic

.PHONY: fuzz-metamorphic
fuzz-metamorphic: $(TEST_DIR)/unit/conformance/test_metamorphic
	@mkdir -p $(MM_FUZZ_DIR)
	@echo "[asx] fuzz-metamorphic: seed=$(MM_FUZZ_SEED) cases=$(MM_FUZZ_CASES) per relation ($(MM_FUZZ_COUNT) scenarios offered)"
	@$(TWIN_RUN) generate --seed $(MM_FUZZ_SEED) --count $(MM_FUZZ_COUNT) --shape meta --jsonl | \
		ASX_METAMORPHIC_CASES=$(MM_FUZZ_CASES) ASX_METAMORPHIC_OUT=$(MM_FUZZ_DIR) \
		$(TEST_EXEC) $(TEST_DIR)/unit/conformance/test_metamorphic --fuzz -; rc=$$?; \
		if [ $$rc -eq 0 ]; then echo "[asx] fuzz-metamorphic: PASS"; \
		else echo "[asx] fuzz-metamorphic: FAIL (exit $$rc; minimized scenarios in $(MM_FUZZ_DIR))"; fi; \
		exit $$rc

COMBINATOR_CONTRACT_TEST_BIN := \
	$(TEST_DIR)/unit/core/test_combinator \
	$(TEST_DIR)/unit/core/test_combinator2 \
	$(TEST_DIR)/unit/core/test_combinator_parity \
	$(TEST_DIR)/unit/core/test_join_set \
	$(TEST_DIR)/unit/core/test_epoch_combinators \
	$(TEST_DIR)/unit/plan/test_plan \
	$(TEST_DIR)/unit/service/test_service \
	$(TEST_DIR)/unit/service/test_service_stack

test-combinator-contract: $(COMBINATOR_CONTRACT_TEST_BIN)
	@echo "[asx] test-combinator-contract: running $(words $(COMBINATOR_CONTRACT_TEST_BIN)) combinator/plan/service suite(s)..."
	@pass=0; fail=0; \
	for t in $(COMBINATOR_CONTRACT_TEST_BIN); do \
		echo "  RUN  $$(basename $$t)"; \
		if $$t; then \
			echo "  PASS $$(basename $$t)"; \
			pass=$$((pass + 1)); \
		else \
			echo "  FAIL $$(basename $$t)"; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo "[asx] test-combinator-contract: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ] || exit 1

ACTOR_SUPERVISION_HARNESS_TEST_BIN := \
	$(TEST_DIR)/unit/actor/test_actor \
	$(TEST_DIR)/unit/actor/test_supervisor

test-actor-supervision-harness: $(ACTOR_SUPERVISION_HARNESS_TEST_BIN)
	@echo "[asx] test-actor-supervision-harness: running $(words $(ACTOR_SUPERVISION_HARNESS_TEST_BIN)) actor/supervision suite(s)..."
	@pass=0; fail=0; \
	for t in $(ACTOR_SUPERVISION_HARNESS_TEST_BIN); do \
		echo "  RUN  $$(basename $$t)"; \
		if $$t; then \
			echo "  PASS $$(basename $$t)"; \
			pass=$$((pass + 1)); \
		else \
			echo "  FAIL $$(basename $$t)"; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo "[asx] test-actor-supervision-harness: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ] || exit 1

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
	$(TEST_DIR)/unit/actor/test_supervisor \
	$(TEST_DIR)/unit/sync/test_sync \
	$(TEST_DIR)/unit/bytes/test_buf \
	$(TEST_DIR)/unit/bytes/test_codec \
	$(TEST_DIR)/unit/bytes/test_io_adapter \
	$(TEST_DIR)/unit/encoding/test_encoding \
	$(TEST_DIR)/unit/decoding/test_decoding \
	$(TEST_DIR)/unit/stream/test_stream \
	$(TEST_DIR)/unit/security/test_crypto \
	$(TEST_DIR)/unit/security/test_security \
	$(TEST_DIR)/unit/security/test_security_audit \
	$(TEST_DIR)/unit/plan/test_plan \
	$(TEST_DIR)/unit/cx/test_cx \
	$(TEST_DIR)/unit/cx/test_scope \
	$(TEST_DIR)/unit/session/test_session_link \
	$(TEST_DIR)/unit/app/test_app \
	$(TEST_DIR)/unit/app/test_report \
	$(TEST_DIR)/unit/cli/test_cli \
	$(TEST_DIR)/unit/console/test_console \
	$(TEST_DIR)/unit/evidence/test_evidence \
	$(TEST_DIR)/unit/fs/test_fs \
	$(TEST_DIR)/unit/link/test_link \
	$(TEST_DIR)/unit/monitor/test_monitor \
	$(TEST_DIR)/unit/observability/test_health \
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

$(TEST_DIR)/unit/session/test_session_link: tests/unit/session/test_session_link.c $(PUBLIC_FAMILY_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
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

$(TEST_DIR)/unit/observability/test_health: tests/unit/observability/test_health.c $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(TEST_CFLAGS) -o $@ $< $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/unit/%: tests/unit/%.c $(LIB_A) | test-dirs
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

# ---------------------------------------------------------------------------
# test-invariants — lifecycle/quiescence invariant tests
# ---------------------------------------------------------------------------
test-invariants: $(INV_TEST_BIN)
	@echo "[asx] test-invariants: running $(words $(INV_TEST_BIN)) test(s)..."
	@mkdir -p build/test-logs
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
	@mkdir -p build/test-logs
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
	@mkdir -p build/test-logs
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
# examples / test-examples — every program in examples/ built under the
# warnings-as-errors policy against libasx, then run; each exits 0 only when
# all its scenarios pass. ex_browser_boundary checks the browser profile's
# fail-closed gating, so only PROFILE=BROWSER builds it; ex_quickstart uses
# the blocking pool, a native surface the browser profile removes.
# ---------------------------------------------------------------------------
EXAMPLE_SRC := $(sort $(wildcard examples/*.c))
ifeq ($(PROFILE),BROWSER)
EXAMPLE_SRC := $(filter-out examples/ex_quickstart.c,$(EXAMPLE_SRC))
# Query the real preprocessed public contract, including custom subprofile
# flags. These examples participate only where their APIs are public.
EXAMPLE_HAS_BROWSER_TRACE := $(shell printf '\043include <asx/asx_config.h>\nASX_HAS_BROWSER_TRACE\n' | $(CC) $(ALL_CFLAGS) -E -P -x c - | tail -n 1)
ifeq ($(EXAMPLE_HAS_BROWSER_TRACE),0)
EXAMPLE_SRC := $(filter-out examples/ex_browser_replay.c examples/ex_lab_replay.c,$(EXAMPLE_SRC))
endif
EXAMPLE_HAS_BROWSER_IO := $(shell printf '\043include <asx/asx_config.h>\nASX_HAS_BROWSER_IO\n' | $(CC) $(ALL_CFLAGS) -E -P -x c - | tail -n 1)
ifeq ($(EXAMPLE_HAS_BROWSER_IO),0)
EXAMPLE_SRC := $(filter-out examples/ex_encoding_decoding.c,$(EXAMPLE_SRC))
endif
else
EXAMPLE_SRC := $(filter-out examples/ex_browser_boundary.c,$(EXAMPLE_SRC))
endif
EXAMPLE_BIN := $(patsubst examples/%.c,$(BUILD_DIR)/examples/%,$(EXAMPLE_SRC))

examples: $(EXAMPLE_BIN)

$(BUILD_DIR)/examples/%: examples/%.c $(LIB_A)
	@mkdir -p $(@D)
	$(CC) $(VIGNETTE_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

test-examples: $(EXAMPLE_BIN)
	@echo "[asx] test-examples: running $(words $(EXAMPLE_BIN)) example(s)..."
	@pass=0; fail=0; \
	for t in $(EXAMPLE_BIN); do \
		if $(TEST_EXEC) $$t >$$t.log 2>&1; then \
			echo "  PASS $$(basename $$t)"; \
			pass=$$((pass + 1)); \
		else \
			echo "  FAIL $$(basename $$t) (log $$t.log)"; \
			tail -5 $$t.log; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo "[asx] test-examples: $$pass passed, $$fail failed"; \
	[ $$pass -gt 0 ] && [ $$fail -eq 0 ]

# ---------------------------------------------------------------------------
# test-abi-shim — consumer ABI/API stability verification (bd-56t.4)
# ---------------------------------------------------------------------------
ABI_SHIM_SRC := tests/abi/consumer_shim.c
ABI_SHIM_BIN := $(TEST_DIR)/abi/consumer_shim
ABI_HOST_OBJ := $(TEST_DIR)/abi/matched_host.o

$(ABI_HOST_OBJ): $(ABI_SHIM_SRC) $(LIB_A) | $(TEST_DIR)/abi
	$(CC) $(ALL_CFLAGS) -DASX_ABI_MATCHED_HOST -c -o $@ $<

$(ABI_SHIM_BIN): $(ABI_SHIM_SRC) $(ABI_HOST_OBJ) $(LIB_A) | $(TEST_DIR)/abi
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $< $(ABI_HOST_OBJ) $(LIB_A)

$(TEST_DIR)/abi:
	@mkdir -p $@

test-abi-shim: $(ABI_SHIM_BIN)
	@echo "[asx] test-abi-shim: running consumer ABI/API stability shim..."
	@$(ABI_SHIM_BIN)
	@echo "[asx] test-abi-shim: PASS"

# Compile external consumers separately from the unchanged library. Their
# ordinary config/hook/builder preparation and all four creation paths must
# refuse mismatched builds before touching any object or surrounding canary.
.PHONY: test-config-fingerprint
test-config-fingerprint: test-abi-shim
	@set -eu; \
	surface_flags="-DASX_ABI_LIBRARY_HAS_APP=$$($(ABI_SHIM_BIN) --app-available) -DASX_ABI_LIBRARY_HAS_LAB=$$($(ABI_SHIM_BIN) --lab-available)"; \
	for knob in ASX_MAX_REGION_CHILDREN ASX_MAX_REGIONS ASX_MAX_TASKS \
	    ASX_MAX_OBLIGATIONS ASX_MAX_TIMERS ASX_MAX_CHANNELS \
	    ASX_MAX_CANCEL_WITNESSES ASX_MAX_WORKERS ASX_MAX_IO_TOKENS \
	    ASX_MAX_DEADLINE_MONITORS ASX_MAX_WAKERS ASX_MAX_BLOCKING_TASKS \
	    ASX_MAX_SUSPECTS ASX_MAX_ONESHOTS ASX_MAX_BROADCASTS ASX_MAX_WATCHES \
	    ASX_MAX_SESSIONS ASX_MAX_EPOCHS ASX_MAX_EPOCH_OBSERVERS ASX_MAX_ACTORS \
	    ASX_MAX_SUPERVISORS ASX_MAX_SIGNAL_SUBSCRIPTIONS ASX_MAX_TCP_LISTENERS \
	    ASX_MAX_TCP_STREAMS ASX_MAX_UDP_SOCKETS ASX_MAX_UNIX_LISTENERS \
	    ASX_MAX_UNIX_STREAMS ASX_MAX_UNIX_DGRAM_SOCKETS ASX_MAX_PIPES \
	    ASX_MAX_TLS_STREAMS ASX_MAX_WS_CONNECTIONS ASX_MAX_QUIC_CONNECTIONS \
	    ASX_MAX_QUIC_STREAMS ASX_MAX_PROCESSES ASX_MAX_FS_ENTRIES \
	    ASX_MAX_OPEN_FILES ASX_MAX_OPEN_DIRS ASX_MAX_RUNTIMES; do \
	    bin="$(TEST_DIR)/abi/mismatch-$$knob"; \
	    echo "[asx] config mismatch control: $$knob"; \
	    other=999u; \
	    if [ "$$knob" = ASX_MAX_RUNTIMES ]; then other=2u; fi; \
	    $(CC) $(ALL_CFLAGS) $$surface_flags -U$$knob -D$$knob=$$other $(ALL_LDFLAGS) \
	        -o "$$bin" $(ABI_SHIM_SRC) $(ABI_HOST_OBJ) $(LIB_A); \
	    "$$bin" --expect-mismatch; \
	done; \
	for variant in profile codec codec-none codec-both determinism ghost buffer trace packing enums browser-minimal; do \
	    case "$$variant" in \
	        profile) if [ "$(PROFILE)" = CORE ]; then other=POSIX; else other=CORE; fi; \
	            flags="-UASX_PROFILE_$(PROFILE) -DASX_PROFILE_$$other" ;; \
	        codec) if [ "$(CODEC)" = JSON ]; then other=BIN; else other=JSON; fi; \
	            flags="-UASX_CODEC_$(CODEC) -DASX_CODEC_$$other" ;; \
	        codec-none) flags="-UASX_CODEC_JSON -UASX_CODEC_BIN" ;; \
	        codec-both) flags="-DASX_CODEC_JSON -DASX_CODEC_BIN" ;; \
	        determinism) if [ "$(DETERMINISTIC)" = 1 ]; then other=0; else other=1; fi; \
	            flags="-UASX_DETERMINISTIC -DASX_DETERMINISTIC=$$other" ;; \
	        ghost) if [ "$$($(ABI_SHIM_BIN) --ghost-enabled)" = 0 ]; then other=1; else other=0; fi; \
	            flags="-UASX_DEBUG_GHOST -DASX_DEBUG_GHOST=$$other" ;; \
	        buffer) flags="-UASX_BUF_CAPACITY -DASX_BUF_CAPACITY=8193u" ;; \
	        trace) flags="-UASX_TRACE_CAPACITY -DASX_TRACE_CAPACITY=2049u" ;; \
	        packing) flags="-fpack-struct=1" ;; \
	        enums) flags="-fshort-enums" ;; \
	        browser-minimal) \
	            if [ "$(PROFILE)" = BROWSER ]; then \
	                if [ "$$($(ABI_SHIM_BIN) --browser-io-enabled)" = 0 ]; then other=EXTENDED; else other=MINIMAL; fi; \
	                flags="-UASX_BROWSER_PROFILE_MINIMAL -UASX_BROWSER_PROFILE_EXTENDED -DASX_BROWSER_PROFILE_$$other"; \
	            else flags="-UASX_PROFILE_$(PROFILE) -DASX_PROFILE_BROWSER -DASX_BROWSER_PROFILE_MINIMAL"; fi ;; \
	    esac; \
	    bin="$(TEST_DIR)/abi/mismatch-$$variant"; \
	    echo "[asx] config mismatch control: $$variant"; \
	    $(CC) $(ALL_CFLAGS) $$surface_flags $$flags $(ALL_LDFLAGS) -o "$$bin" $(ABI_SHIM_SRC) $(ABI_HOST_OBJ) $(LIB_A); \
	    "$$bin" --expect-mismatch; \
	done; \
	echo "[asx] test-config-fingerprint: matching consumer and 49 mismatched builds PASS"

# Compare diagnostics with a consumer's independently compiled effective flag.
# In particular, -DASX_DEBUG_GHOST=0 must not be reported as enabled.
.PHONY: test-cli-info
test-cli-info: cli $(ABI_SHIM_BIN)
	@set -eu; \
	expected="$$($(ABI_SHIM_BIN) --ghost-enabled)"; \
	output="$$($(BIN_DIR)/asx info)"; \
	printf '%s\n' "$$output"; \
	if [ "$$expected" = 0 ]; then \
	    printf '%s\n' "$$output" | grep -Eq '^Ghost monitors: +no$$'; \
	    printf '%s\n' "$$output" | grep -q 'ASX_DEBUG_GHOST=0'; \
	else printf '%s\n' "$$output" | grep -Eq '^Ghost monitors: +yes$$'; fi; \
	echo "[asx] test-cli-info: effective ghost=$$expected PASS"

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

# Harnesses that need full library (cancel.c, budget.c + runtime deps)
$(TEST_DIR)/formal/cancel_witness_harness: $(FORMAL_CBMC_DIR)/cancel_witness_harness.c $(LIB_A) | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -o $@ $< $(LIB_A)

$(TEST_DIR)/formal/budget_lattice_harness: $(FORMAL_CBMC_DIR)/budget_lattice_harness.c $(LIB_A) | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -o $@ $< $(LIB_A)

$(TEST_DIR)/formal/child_region_harness: $(FORMAL_CBMC_DIR)/child_region_harness.c $(LIB_A) | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -o $@ $< $(LIB_A)

$(TEST_DIR)/formal/lane_selection_harness: $(FORMAL_CBMC_DIR)/lane_selection_harness.c $(LIB_A) | $(TEST_DIR)/formal
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

$(TEST_DIR)/formal/test_outcome_lattice: $(FORMAL_ALG_DIR)/test_outcome_lattice.c $(LIB_A) | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -o $@ $< $(LIB_A)

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
FORMAL_LITMUS_EXTRA_SRC := src/runtime/seqlock_ebr_spike.c
FORMAL_LITMUS_SINGLE_BIN := $(TEST_DIR)/formal/test_memory_model_litmus_single
FORMAL_LITMUS_MULTI_BIN := $(TEST_DIR)/formal/test_memory_model_litmus_multi
FORMAL_LITMUS_BIN := $(FORMAL_LITMUS_SINGLE_BIN) $(FORMAL_LITMUS_MULTI_BIN)

$(FORMAL_LITMUS_SINGLE_BIN): $(FORMAL_LITMUS_SRC) $(FORMAL_LITMUS_EXTRA_SRC) $(LIB_A) | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -DASX_LOCKFREE_SINGLE_THREAD=1 -o $@ $< $(FORMAL_LITMUS_EXTRA_SRC) $(LIB_A)

$(FORMAL_LITMUS_MULTI_BIN): $(FORMAL_LITMUS_SRC) $(FORMAL_LITMUS_EXTRA_SRC) $(LIB_A) | $(TEST_DIR)/formal
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) -DASX_LOCKFREE_SINGLE_THREAD=0 -o $@ $< $(FORMAL_LITMUS_EXTRA_SRC) $(LIB_A)

formal-litmus: $(FORMAL_LITMUS_BIN)
	@echo "[asx] formal-litmus: running memory-model litmus suite..."
	@$(FORMAL_LITMUS_SINGLE_BIN)
	@$(FORMAL_LITMUS_MULTI_BIN)
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
# GATE-E2E-PARALLEL-SWARM, GATE-E2E-PACKAGE.
# ---------------------------------------------------------------------------
test-e2e-suite: $(LIB_A)
	@chmod +x $(E2E_SCRIPT_DIR)/run_all.sh $(E2E_SCRIPT_DIR)/harness.sh $(E2E_ALL_SCRIPTS) 2>/dev/null || true
	@$(E2E_SCRIPT_DIR)/run_all.sh

test-e2e-parallel: $(LIB_A)
	@chmod +x $(E2E_SCRIPT_DIR)/harness.sh $(E2E_SCRIPT_DIR)/parallel_swarm.sh 2>/dev/null || true
	@$(E2E_SCRIPT_DIR)/parallel_swarm.sh

test-e2e-posix-adapter:
	@chmod +x $(E2E_SCRIPT_DIR)/harness.sh $(E2E_SCRIPT_DIR)/posix_adapter_smoke.sh 2>/dev/null || true
	@$(E2E_SCRIPT_DIR)/posix_adapter_smoke.sh

test-e2e-network-surface:
	@chmod +x $(E2E_SCRIPT_DIR)/harness.sh $(E2E_SCRIPT_DIR)/network_surface.sh 2>/dev/null || true
	@$(E2E_SCRIPT_DIR)/network_surface.sh

# test-e2e-foundational-evidence — tests/e2e/test_foundational_contracts.sh
# (bd-1eqo.15.1): the foundational type suites (algebraic and core unit tests)
# built and run against the library, with their JSONL evidence. It is not an
# e2e-suite family (that is foundational_contracts.sh); a failing suite fails it.
.PHONY: test-e2e-foundational-evidence
test-e2e-foundational-evidence: $(LIB_A)
	@bash $(E2E_SCRIPT_DIR)/test_foundational_contracts.sh

# test-e2e-install — scripts/install.sh and `make install` end to end
# (tests/e2e/install_smoke.sh, bd-0toq/bd-jxpg): install into a temporary
# prefix, compile and run the quickstart against the installed headers and
# library, install again. It builds a release library, so it is not an
# e2e-suite family (the suite fails a family that rewrites build/lib/libasx.a).
.PHONY: test-e2e-install
test-e2e-install:
	@chmod +x $(E2E_SCRIPT_DIR)/harness.sh $(E2E_SCRIPT_DIR)/install_smoke.sh 2>/dev/null || true
	@$(E2E_SCRIPT_DIR)/install_smoke.sh

# RP3: a real native HTTP server run (keep-alive, half request, connection
# reset, SIGTERM drain) recorded to a boundary journal, then replayed with no
# socket opened and required to reproduce the trace digest bit for bit.
# Builds its own POSIX library under build/e2e-http-live. Gate:
# GATE-E2E-RECORD-REPLAY. ASX_E2E_REPLAYS sets the repeat count (default 100).
test-e2e-native-record-replay:
	@chmod +x $(E2E_SCRIPT_DIR)/harness.sh $(E2E_SCRIPT_DIR)/native_record_replay.sh 2>/dev/null || true
	@$(E2E_SCRIPT_DIR)/native_record_replay.sh

# RP4: the same server killed (SIGKILL) at ASX_E2E_KILL_POINTS random offsets
# of its journal stream (default 50, seeded by ASX_E2E_SEED); the survivor of
# each is replayed as a recovery replay: every committed record reproduced,
# the trace checkpoints equal to the recorder's log, the run ending at the cut
# with its seq. Gate: GATE-E2E-CRASH-CONTINUITY.
test-e2e-native-crash-continuity:
	@chmod +x $(E2E_SCRIPT_DIR)/harness.sh $(E2E_SCRIPT_DIR)/native_crash_continuity.sh 2>/dev/null || true
	@$(E2E_SCRIPT_DIR)/native_crash_continuity.sh

test-e2e-actor-supervision:
	@chmod +x $(E2E_SCRIPT_DIR)/harness.sh $(E2E_SCRIPT_DIR)/actor_supervision.sh 2>/dev/null || true
	@$(E2E_SCRIPT_DIR)/actor_supervision.sh

wave-c-acceptance-demo: $(LIB_A)
	@chmod +x $(E2E_SCRIPT_DIR)/harness.sh $(E2E_SCRIPT_DIR)/parallel_swarm.sh $(E2E_SCRIPT_DIR)/network_surface.sh $(E2E_SCRIPT_DIR)/actor_supervision.sh $(E2E_SCRIPT_DIR)/wave_c_acceptance_demo.sh 2>/dev/null || true
	@$(E2E_SCRIPT_DIR)/wave_c_acceptance_demo.sh

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

$(TEST_DIR)/vignettes/vignette_session_link: tests/vignettes/vignette_session_link.c $(PUBLIC_FAMILY_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(VIGNETTE_CFLAGS) -o $@ $< $(PUBLIC_FAMILY_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/vignettes/vignette_console: tests/vignettes/vignette_console.c $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(VIGNETTE_CFLAGS) -o $@ $< $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

$(TEST_DIR)/vignettes/vignette_observability: tests/vignettes/vignette_observability.c $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) | test-dirs
	$(CC) $(VIGNETTE_CFLAGS) -o $@ $< $(OPERATOR_TEST_EXTRA_SRC) $(LIB_A) $(ALL_LDFLAGS)

test-dirs:
	@mkdir -p $(TEST_DIR)/unit/core $(TEST_DIR)/unit/runtime \
	          $(TEST_DIR)/unit/channel $(TEST_DIR)/unit/session \
	          $(TEST_DIR)/unit/record $(TEST_DIR)/unit/time \
	          $(TEST_DIR)/unit/bytes $(TEST_DIR)/unit/encoding \
	          $(TEST_DIR)/unit/decoding $(TEST_DIR)/unit/actor \
	          $(TEST_DIR)/unit/raptorq $(TEST_DIR)/unit/migration \
	          $(TEST_DIR)/unit/cx \
	          $(TEST_DIR)/unit/app $(TEST_DIR)/unit/console \
	          $(TEST_DIR)/unit/evidence $(TEST_DIR)/unit/observability \
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
#   make parallel-bench-json      # PARALLEL profile worker-count baseline JSON
#   make parallel-bench-gate      # Capture PARALLEL baselines and report warnings
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

.PHONY: bench bench-json bench-build parallel-bench-json parallel-bench-gate

bench-build: $(BENCH_BIN)

$(BENCH_BIN): $(BENCH_SRC) $(LIB_A) | $(BENCH_DIR)
	$(CC) $(BENCH_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

$(BENCH_DIR):
	@mkdir -p $@

bench: bench-build
	@echo "[asx] bench: running performance benchmarks..."
	@$(BENCH_BIN)
	@echo "[asx] bench: complete"

# ---------------------------------------------------------------------------
# perf-digest — what the rolling digests cost, before and after (bd-o5zh).
# tests/bench/bench_digest.c times the previous folds (copied into it) against
# src/runtime/fnv_fold.h in one process, checks that the digests are
# identical, and prints ns per spawn+run before and after (the library is
# built -O2: this is a bench goal). Not part of `make test`: timings are
# noisy on a shared machine. PERF_DIGEST_ARGS="--rounds N --runs N".
#   make perf-digest
#   rch exec -- make perf-digest
# ---------------------------------------------------------------------------
DIGEST_BENCH_SRC := tests/bench/bench_digest.c
DIGEST_BENCH_BIN := $(BENCH_DIR)/bench_digest

.PHONY: perf-digest

$(DIGEST_BENCH_BIN): $(DIGEST_BENCH_SRC) src/runtime/fnv_fold.h $(LIB_A) | $(BENCH_DIR)
	$(CC) $(BENCH_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

perf-digest: $(DIGEST_BENCH_BIN)
	@echo "[asx] perf-digest: timing the rolling digests, before and after..."
	@$(DIGEST_BENCH_BIN) $(PERF_DIGEST_ARGS)

# ---------------------------------------------------------------------------
# bench-journal-overhead — RP1's budget: an HTTP/1.1 keep-alive server with
# the boundary journal off and on, at most 10% throughput loss (bd-9kll.11.1).
# tests/bench/journal_overhead.sh builds its own live POSIX release library
# (build/bench-journal) and tests/bench/bench_journal_http.c, runs the same
# workload in every mode, interleaved, and prints requests/s, journal records
# and bytes per request and the loss against no journal. Not part of
# `make test`: timings are noisy on a shared machine.
#   make bench-journal-overhead
#   make bench-journal-overhead BENCH_JOURNAL_ARGS="--requests 50000 --rounds 9 --gate"
#   rch exec -- make bench-journal-overhead
# ---------------------------------------------------------------------------
.PHONY: bench-journal-overhead
bench-journal-overhead:
	@chmod +x tests/bench/journal_overhead.sh 2>/dev/null || true
	@BENCH_JOURNAL_ARGS="$(BENCH_JOURNAL_ARGS)" tests/bench/journal_overhead.sh

# ---------------------------------------------------------------------------
# bench-sched-ab — what the sweep's dispatch order costs (bd-9kll.4.6): the
# walk of before the bead (arena order), the priority and cancel lanes, and
# the shipped order with the timed lane, three release builds of the one
# tree (the planted-bug macros ASX_MUTANT_SWEEP_ARENA_ORDER and
# ASX_MUTANT_SWEEP_NO_TIMED_LANE select the first two), the same bench
# program against each, interleaved, the median of --runs runs.
# tests/bench/sched_lanes_ab.sh. Not part of `make test`: timings are noisy
# on a shared machine.
#   make bench-sched-ab
#   make bench-sched-ab SCHED_AB_ARGS="--runs 9 --gate"
#   rch exec -- make bench-sched-ab
# ---------------------------------------------------------------------------
.PHONY: bench-sched-ab
bench-sched-ab:
	@chmod +x tests/bench/sched_lanes_ab.sh 2>/dev/null || true
	@tests/bench/sched_lanes_ab.sh $(SCHED_AB_ARGS)

# ---------------------------------------------------------------------------
# test-ct-timing: dudect-style Welch t-test of asx_crypto_ct_equal and
# asx_crypto_ct_is_zero (bd-9kll.16.5). Not part of `make test`: timing
# statistics are noisy on shared machines. Exits 0 only if no leak is found
# and the harness flags its own deliberately leaky control; 1 on a leak; 2
# when the control is not flagged (inconclusive). CT_ARGS passes
# --samples N / --attempts N / --seed N.
# ---------------------------------------------------------------------------
CT_DIR := $(BUILD_DIR)/ct
CT_SRC := tests/security/ct_compare_dudect.c
CT_BIN := $(CT_DIR)/ct_compare_dudect

CT_CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -Werror -O2 -Wno-unused-parameter \
             $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF)

.PHONY: test-ct-timing

$(CT_BIN): $(CT_SRC) $(LIB_A) | $(CT_DIR)
	$(CC) $(CT_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS) -lm

$(CT_DIR):
	@mkdir -p $@

test-ct-timing: $(CT_BIN)
	@echo "[asx] test-ct-timing: dudect-style timing check of the constant-time helpers..."
	@$(CT_BIN) $(CT_ARGS)

bench-json:
	@$(MAKE) --no-print-directory bench-build >&2
	@$(BENCH_BIN) --json

parallel-bench-json:
	@$(MAKE) --no-print-directory bench-build PROFILE=PARALLEL CFLAGS="$(CFLAGS) -DASX_LOCKFREE_SINGLE_THREAD=0" >/dev/null
	@$(BENCH_BIN) --json

parallel-bench-gate:
	@echo "[asx] parallel-bench-gate: capturing PARALLEL worker-count baselines..."
	@mkdir -p build/perf
	@$(MAKE) --no-print-directory parallel-bench-json > build/perf/parallel-bench-results.json
	@jq -e '.profile == "PARALLEL" and (.parallel_worker_baselines | length > 0) and (.parallel_overload_slo_summary.schema == "asx.parallel_overload_slo_summary.v1") and (.parallel_resource_class_footprint_baselines | length > 0)' build/perf/parallel-bench-results.json >/dev/null
	@jq '{schema:"asx.parallel_bench_gate_summary.v1",generated_at:(now|todate),source:"build/perf/parallel-bench-results.json",profile:.profile,bench_metadata:.bench_metadata,overload_slo_summary:.parallel_overload_slo_summary,threshold_policy:.parallel_worker_baselines[0].overload_slo.thresholds,remediation:.parallel_worker_baselines[0].overload_slo.remediation,worker_rows:[.parallel_worker_baselines[] | {worker_count,resource_class,supported,threshold_status,p50_ns:.scheduler_stats.p50_ns,p95_ns:.scheduler_stats.p95_ns,p99_ns:.scheduler_stats.p99_ns,p99_9_ns:.scheduler_stats.p99_9_ns,admission_rejects_observed:.overload_slo.observed.admission.rejects_observed,steal_cost_proxy_ns_per_attempt:.overload_slo.observed.steal.cost_proxy_ns_per_attempt,peak_pressure_pct:.overload_slo.observed.queue_pressure.peak_pressure_pct,memory_footprint_bytes:.overload_slo.observed.memory_footprint_bytes}],resource_class_rows:.parallel_resource_class_footprint_baselines}' build/perf/parallel-bench-results.json > build/perf/parallel-bench-gate-summary.json
	@warns=$$(jq '[.parallel_worker_baselines[] | select(.threshold_status == "warn")] | length' build/perf/parallel-bench-results.json); \
	blocks=$$(jq '[.parallel_worker_baselines[] | select(.threshold_status == "block")] | length' build/perf/parallel-bench-results.json); \
	echo "[asx] parallel-bench-gate: observe-only warnings=$$warns blocks=$$blocks report=build/perf/parallel-bench-results.json summary=build/perf/parallel-bench-gate-summary.json"; \
	if [ "$$warns" -gt 0 ]; then \
		jq -r '.parallel_worker_baselines[] | select(.threshold_status == "warn") | "  - WARN worker_count=\(.worker_count) resource_class=\(.resource_class) p99_ns=\(.scheduler_stats.p99_ns) p99_9_ns=\(.scheduler_stats.p99_9_ns) pressure_pct=\(.overload_slo.observed.queue_pressure.peak_pressure_pct) memory_bytes=\(.overload_slo.observed.memory_footprint_bytes) remediation=\(.overload_slo.remediation)"' build/perf/parallel-bench-results.json; \
	fi; \
	if [ "$$blocks" -gt 0 ]; then \
		jq -r '.parallel_worker_baselines[] | select(.threshold_status == "block") | "  - BLOCK worker_count=\(.worker_count) resource_class=\(.resource_class) p99_ns=\(.scheduler_stats.p99_ns) p99_9_ns=\(.scheduler_stats.p99_9_ns) scheduler_ops_per_sec=\(.scheduler_throughput_ops_per_sec) mpsc_ops_per_sec=\(.mpsc_contention.ops_per_sec) memory_bytes=\(.overload_slo.observed.memory_footprint_bytes)"' build/perf/parallel-bench-results.json; \
		exit 1; \
	fi

.PHONY: slo-gate size-gate evidence-dashboard traceability-export release-evidence-manifest
slo-gate: bench-build
	@echo "[asx] slo-gate: capturing benchmark and evaluating SLO baselines..."
	@mkdir -p build/perf
	@tools/ci/run_bench_median.sh --runs 5 --output build/perf/bench-results.json -- $(BENCH_BIN) --json
	@tools/ci/evaluate_slo_gates.sh \
		--bench-json build/perf/bench-results.json \
		--command "make bench-json" \
		--output build/perf/slo_gate_report.json \
		--strict
	@echo "[asx] slo-gate: complete (report: build/perf/slo_gate_report.json)"

size-gate: release bench-build
	@echo "[asx] size-gate: measuring binary size and cold-start metrics..."
	@mkdir -p build/perf
	@tools/ci/run_bench_median.sh --runs 5 --output build/perf/bench-results.json -- $(BENCH_BIN) --json
	@tools/ci/evaluate_size_gates.sh \
		--lib $(LIB_DIR)/libasx.a \
		--bench-json build/perf/bench-results.json \
		--output build/perf/size_gate_report.json \
		--strict
	@echo "[asx] size-gate: complete (report: build/perf/size_gate_report.json)"

evidence-dashboard: bench-build
	@echo "[asx] evidence-dashboard: collecting metrics and computing trends..."
	@mkdir -p build/perf
	@tools/ci/run_bench_median.sh --runs 5 --output build/perf/bench-results.json -- $(BENCH_BIN) --json
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
# release-evidence-manifest — unified release evidence manifest (bd-dq94.2)
# ---------------------------------------------------------------------------
release-evidence-manifest:
	@echo "[asx] release-evidence-manifest: generating unified release evidence manifest..."
	@tools/ci/generate_release_evidence_manifest.sh --strict
	@echo "[asx] release-evidence-manifest: complete"

# ---------------------------------------------------------------------------
# resource-pressure-gate — deterministic failure-atomic pressure scenarios
# ---------------------------------------------------------------------------
resource-pressure-gate:
	@echo "[asx] resource-pressure-gate: replaying resource pressure scenario pack..."
	@ASX_GIT_COMMIT="$(ASX_GIT_COMMIT)" tools/ci/run_resource_pressure_gate.sh

# ---------------------------------------------------------------------------
# conformance — Rust parity: every Rust-captured DSL v2 fixture in
# fixtures/rust_reference_v2 (captured by tools/twin_run from asupersync's
# LabRuntime at the pinned rev) is executed through this C runtime by
# asx-conformance and compared canonical byte for byte: trace (Foata
# layers), snapshot and observations. The comparator's negative control
# (self-test: a corrupted expectation must FAIL) runs first. The target
# fails until every fixture matches; a FAIL is a C/Rust divergence, an
# ERROR an op or capability the C side cannot express yet (DSL §7). Every
# fixture matches, and CI's conformance job runs it as a blocking step (also
# under QEMU and -m32); it is not part of check-ci.
# A second pass (compare-sweep, bd-9kll.4.9) runs each fixture under the
# runtime's default sweep scheduler, which is what a live user gets, and
# compares snapshot and observations only. The fixtures listed with a reason
# in tests/conformance/sweep_known_divergences.txt may differ; any other
# difference fails, and a listed fixture that now matches fails as STALE.
# The kernel-state oracles of docs/LAB_ORACLE_CATALOG.md run over the
# arenas at the end of every fixture of the compare pass
# (CONFORMANCE_ORACLES, by Rust's registry names): a state a parity compare
# cannot see (a counter off, a closed region holding a task) fails the
# fixture and names the oracle. tests/gates/test_conformance_monitors.sh
# plants one. `make conformance CONFORMANCE_ORACLES=` runs without them and
# CONFORMANCE_ORACLES=all with every oracle the runner knows.
# CONFORMANCE_SWEEP_ORACLES is the same for the compare-sweep pass; it is
# empty until that pass has been measured with them.
# ---------------------------------------------------------------------------
CONFORMANCE_V2_FIXTURES := $(sort $(wildcard fixtures/rust_reference_v2/*.json))
CONFORMANCE_SELF_TEST_FIXTURE := fixtures/rust_reference_v2/obligation-reserve-commit-001.json
CONFORMANCE_SWEEP_KNOWN := tests/conformance/sweep_known_divergences.txt
CONFORMANCE_ORACLES ?= task_leak,obligation_leak,quiescence,finalizer,region_tree,deadline_monotone,cancellation_protocol,cancel_correctness,cancel_debt,task_counters,arena_counters
CONFORMANCE_SWEEP_ORACLES ?=

conformance: $(CONFORMANCE_RUNNER)
	@echo "[asx] conformance: executing $(words $(CONFORMANCE_V2_FIXTURES)) Rust-captured v2 fixture(s) through the C runtime..."
	@if [ -z "$(CONFORMANCE_V2_FIXTURES)" ]; then \
		echo "[asx] conformance: FAIL (no fixtures in fixtures/rust_reference_v2)"; \
		exit 1; \
	fi
	@$(TEST_EXEC) $(CONFORMANCE_RUNNER) self-test $(CONFORMANCE_SELF_TEST_FIXTURE)
	@ASX_ORACLES="$(CONFORMANCE_ORACLES)" $(TEST_EXEC) $(CONFORMANCE_RUNNER) compare $(CONFORMANCE_V2_FIXTURES)
	@ASX_ORACLES="$(CONFORMANCE_SWEEP_ORACLES)" $(TEST_EXEC) $(CONFORMANCE_RUNNER) compare-sweep --known $(CONFORMANCE_SWEEP_KNOWN) $(CONFORMANCE_V2_FIXTURES)

# ---------------------------------------------------------------------------
# conformance-ghost — the same corpus under the ghost monitors (bd-9kll.2.21).
# The runner is built against a library with ASX_DEBUG_GHOST on and with
# -DASX_CONFORMANCE_GHOST_CHECK, which FAILs a scenario whose run recorded any
# protocol or linearity violation and prints the first. `conformance` and its
# runner are unchanged. Own BUILD_DIR, so build/ is untouched. A clean run
# shows legal task/region/obligation transitions and single resolution of every
# obligation in these scenarios; it does not show the absence of memory
# errors (conformance-asan) or that the monitors cover every transition.
# tests/gates/test_conformance_monitors.sh plants a violation and expects this
# lane to fail where `conformance` passes.
# ---------------------------------------------------------------------------
CONFORMANCE_GHOST_DIR ?= $(BUILD_DIR)/conformance-ghost

.PHONY: conformance-ghost
conformance-ghost:
	@echo "[asx] conformance-ghost: fixture corpus under the ghost monitors..."
	@$(MAKE) --no-print-directory conformance BUILD_DIR=$(CONFORMANCE_GHOST_DIR) \
		CFLAGS='$(CFLAGS) -DASX_DEBUG_GHOST=1 -DASX_CONFORMANCE_GHOST_CHECK=1'
	@echo "[asx] conformance-ghost: PASS"

# ---------------------------------------------------------------------------
# twin-run-check — the Rust half of the oracle is code too (bd-9kll.2.23):
# tools/twin_run is formatted (cargo fmt --check), lint-clean with the tests
# included (clippy -D warnings), its unit tests pass (the trace projection of
# hand-built traces, canon, minimize) and the release binary that captures
# the fixtures builds. A twin_run that does not compile, or a projection that
# drifted, otherwise surfaces only in whichever batch first builds it. Needs
# cargo with rust-toolchain.toml's pinned nightly; CI runs each target as a
# step of the fuzz-rust-differential job, which has that toolchain.
# ---------------------------------------------------------------------------
TWIN_RUN_MANIFEST := tools/twin_run/Cargo.toml

.PHONY: twin-run-fmt twin-run-clippy twin-run-test twin-run-build twin-run-check
twin-run-fmt:
	cargo fmt --manifest-path $(TWIN_RUN_MANIFEST) --check

twin-run-clippy:
	cargo clippy --locked --manifest-path $(TWIN_RUN_MANIFEST) --all-targets -- -D warnings

twin-run-test:
	cargo test --locked --manifest-path $(TWIN_RUN_MANIFEST)

twin-run-build:
	cargo build --release --locked --manifest-path $(TWIN_RUN_MANIFEST) --bin twin_run

twin-run-check: twin-run-fmt twin-run-clippy twin-run-test twin-run-build

# ---------------------------------------------------------------------------
# fuzz-differential — Rust-vs-C differential fuzzing over DSL v2 (bd-ij9w):
# twin_run generates FUZZ_V2_COUNT scenarios for FUZZ_V2_SEED, runs them in
# asupersync's LabRuntime, and the C runtime runs every scenario Rust could
# capture (only this run's captures are compared). Every FAIL is a candidate
# drift to root-cause. CI runs it on every push with seed 9 (ci.yml) and on
# three new seeds nightly (nightly.yml). Rust refuses a few generated scenarios (a step
# naming a resource whose acquisition failed, ~2.5%). If the capture never
# finishes or under 90% is captured, the oracle itself broke; that fails
# here rather than letting a near-empty compare pass.
# ---------------------------------------------------------------------------
FUZZ_V2_SEED ?= 1
FUZZ_V2_COUNT ?= 100
# FUZZ_V2_SHAPE=timers draws every scenario from the timer-heavy shape of
# gen.rs (see fuzz-timers); empty is the default mix. The shape is part of the
# directory name, so the lanes that read $(FUZZ_V2_DIR) (fuzz-minimize,
# fuzz-ghost, fuzz-asan, fuzz-promote) take the same FUZZ_V2_SHAPE.
FUZZ_V2_SHAPE ?=
FUZZ_V2_DIR := $(BUILD_DIR)/fuzz_v2/seed-$(FUZZ_V2_SEED)$(if $(FUZZ_V2_SHAPE),-$(FUZZ_V2_SHAPE))
TWIN_RUN := $(if $(ASX_TWIN_BIN),$(ASX_TWIN_BIN),cargo run --release --locked --quiet --manifest-path tools/twin_run/Cargo.toml --)

.PHONY: fuzz-differential
fuzz-differential: $(CONFORMANCE_RUNNER)
	@echo "[asx] fuzz-differential: seed=$(FUZZ_V2_SEED) count=$(FUZZ_V2_COUNT)$(if $(FUZZ_V2_SHAPE), shape=$(FUZZ_V2_SHAPE))"
	@if [ -z "$(CHECK_JSONSCHEMA)" ]; then \
		echo "[asx] fuzz-differential: FAIL (check-jsonschema $(CHECK_JSONSCHEMA_VERSION) not found and no uvx)"; exit 1; \
	fi
	@mkdir -p $(FUZZ_V2_DIR)
	@$(TWIN_RUN) generate --seed $(FUZZ_V2_SEED) --count $(FUZZ_V2_COUNT) $(if $(FUZZ_V2_SHAPE),--shape $(FUZZ_V2_SHAPE)) --out $(FUZZ_V2_DIR)/scenarios
	@$(CHECK_JSONSCHEMA) --schemafile schemas/scenario_dsl_v2.json $(FUZZ_V2_DIR)/scenarios/*.json
	@$(TWIN_RUN) capture $(FUZZ_V2_DIR)/scenarios/*.json --out $(FUZZ_V2_DIR)/fixtures > $(FUZZ_V2_DIR)/capture.log || true
	@tail -1 $(FUZZ_V2_DIR)/capture.log
	@grep -q '^twin_run capture: ' $(FUZZ_V2_DIR)/capture.log || \
		{ echo "[asx] fuzz-differential: twin_run capture did not finish"; exit 1; }
	@captured=$$(grep -c '^PASS ' $(FUZZ_V2_DIR)/capture.log); \
		if [ $$((captured * 10)) -lt $$(($(FUZZ_V2_COUNT) * 9)) ]; then \
			echo "[asx] fuzz-differential: only $$captured of $(FUZZ_V2_COUNT) scenarios captured"; exit 1; \
		fi
	@$(CONFORMANCE_RUNNER) compare $$(sed -n 's|^PASS \([^ ]*\) .*|$(FUZZ_V2_DIR)/fixtures/\1.json|p' $(FUZZ_V2_DIR)/capture.log) \
		> $(FUZZ_V2_DIR)/compare.log; rc=$$?; cat $(FUZZ_V2_DIR)/compare.log; \
		if [ $$rc -ne 0 ]; then \
			echo "[asx] fuzz-differential: reduce with make fuzz-minimize FUZZ_V2_SEED=$(FUZZ_V2_SEED)$(if $(FUZZ_V2_SHAPE), FUZZ_V2_SHAPE=$(FUZZ_V2_SHAPE))"; \
		fi; exit $$rc

# ---------------------------------------------------------------------------
# fuzz-timers — the 1000-random-timers differential of the same-tick ordering
# port (bd-9kll.7.1): fuzz-differential over the timer-heavy shape of gen.rs
# at a fixed seed and count. A scenario holds 2 to 8 timers in one 1 ms tick
# of the wheel at a drawn level (under 256 ms up to more than a day, the
# overflow heap), registered in an order that is not deadline order, some of
# them late (a prelude sleep) so their deadline is reached by a cascade, with
# timeouts and lock deadlines among the sleeps, cancels and waker moves of
# regions and tasks, and advances past several deadlines at once. Every FAIL
# is a candidate drift in timer ordering; reduce it with
#   make fuzz-minimize FUZZ_V2_SEED=$(FUZZ_TIMERS_SEED) FUZZ_V2_SHAPE=timers
# The shape draws from a stream of its own: the default mix of
# fuzz-differential is byte-identical to what it was.
# ---------------------------------------------------------------------------
FUZZ_TIMERS_SEED ?= 7100
FUZZ_TIMERS_COUNT ?= 1000

.PHONY: fuzz-timers
fuzz-timers:
	@$(MAKE) --no-print-directory fuzz-differential FUZZ_V2_SHAPE=timers \
		FUZZ_V2_SEED=$(FUZZ_TIMERS_SEED) FUZZ_V2_COUNT=$(FUZZ_TIMERS_COUNT)

# ---------------------------------------------------------------------------
# fuzz-coverage — what the generated corpus leaves of the C runtime
# unexecuted (bd-9kll.2.10, G4). The conformance runner is built with gcc
# --coverage into $(FUZZ_COV_DIR) (build/ is not touched) and run over the
# committed fixtures and over the captured fixtures of every earlier
# `make fuzz-differential` run (build/fuzz_v2/seed-*/fixtures); the report
# lists, per file, the lines the fixtures execute and the generated corpus does
# not: the parts of the DSL the generator does not draw, or draws in the wrong
# shape. Data, not a gate. Needs gcov and at least one fuzz-differential run.
# ---------------------------------------------------------------------------
FUZZ_COV_DIR ?= $(BUILD_DIR)/fuzz_coverage

.PHONY: fuzz-coverage
fuzz-coverage:
	@FUZZ_COV_DIR="$(FUZZ_COV_DIR)" bash tools/ci/fuzz_coverage.sh

# ---------------------------------------------------------------------------
# scheduler-certificate-parity — Rust's and C's scheduler agree on every
# dispatch and on the schedule certificate, and each replays the other's
# recorded schedule (bd-9kll.4.8, bd-9kll.9.3). tests/e2e/
# scheduler_certificate_parity.sh runs it over the committed Rust fixtures and
# SCHED_CERT_COUNT generated scenarios of SCHED_CERT_SEED, captured now:
# certificate and dispatch equality, Rust's schedule replayed in C, C's
# recorded schedule byte-identical to Rust's artifact, and C's schedule
# replayed by Rust's run_forced_schedule (twin_run forced-replay) with C's run
# equal to Rust's replay. One JSONL record per scenario and check in
# $(BUILD_DIR)/test-logs, the first divergence in each failing one. It needs
# cargo and the pinned asupersync like fuzz-differential.
# ---------------------------------------------------------------------------
SCHED_CERT_SEED ?= 1
SCHED_CERT_COUNT ?= 1000

.PHONY: scheduler-certificate-parity
scheduler-certificate-parity: $(CONFORMANCE_RUNNER)
	@echo "[asx] scheduler-certificate-parity: seed=$(SCHED_CERT_SEED) count=$(SCHED_CERT_COUNT)"
	@ASX_CONFORMANCE_RUNNER="$(abspath $(CONFORMANCE_RUNNER))" \
		ASX_TWIN_BIN="$(ASX_TWIN_BIN)" \
		ASX_E2E_SCHED_SEED="$(SCHED_CERT_SEED)" ASX_E2E_SCHED_COUNT="$(SCHED_CERT_COUNT)" \
		BUILD_DIR="$(abspath $(BUILD_DIR))" bash tests/e2e/scheduler_certificate_parity.sh

# ---------------------------------------------------------------------------
# fuzz-minimize — reduce every scenario the last fuzz-differential run of
# FUZZ_V2_SEED FAILed to a 1-minimal reproduction (twin_run minimize: delete
# steps, tasks, script entries and declarations while Rust still runs it and
# C still FAILs it the same way). Writes <id>-min.scenario.json and its Rust
# fixture to $(FUZZ_V2_DIR)/minimized.
# ---------------------------------------------------------------------------
.PHONY: fuzz-minimize
fuzz-minimize: $(CONFORMANCE_RUNNER)
	@test -f $(FUZZ_V2_DIR)/compare.log || \
		{ echo "[asx] fuzz-minimize: run make fuzz-differential FUZZ_V2_SEED=$(FUZZ_V2_SEED) first"; exit 1; }
	@for id in $$(sed -n 's/^FAIL \([^:]*\):.*/\1/p' $(FUZZ_V2_DIR)/compare.log); do \
		$(TWIN_RUN) minimize $(FUZZ_V2_DIR)/scenarios/$$id.json --runner $(CONFORMANCE_RUNNER) \
			--out $(FUZZ_V2_DIR)/minimized || exit 1; \
	done

# ---------------------------------------------------------------------------
# fuzz-ghost — the scenarios the last fuzz-differential run captured (same
# FUZZ_V2_SEED/FUZZ_V2_COUNT), compared by the ghost-checked runner of
# conformance-ghost (bd-9kll.2.21). It needs no Rust: it reads that run's
# capture.log and fixtures. A scenario whose run records a ghost violation
# FAILs and the first is printed; the generated corpus is where a protocol
# or linearity break that leaves the projection unchanged is most likely to
# show.
# ---------------------------------------------------------------------------
.PHONY: fuzz-ghost
fuzz-ghost:
	@test -f $(FUZZ_V2_DIR)/capture.log || \
		{ echo "[asx] fuzz-ghost: run make fuzz-differential FUZZ_V2_SEED=$(FUZZ_V2_SEED) FUZZ_V2_COUNT=$(FUZZ_V2_COUNT) first"; exit 1; }
	@$(MAKE) --no-print-directory $(CONFORMANCE_GHOST_DIR)/bin/asx-conformance \
		BUILD_DIR=$(CONFORMANCE_GHOST_DIR) \
		CFLAGS='$(CFLAGS) -DASX_DEBUG_GHOST=1 -DASX_CONFORMANCE_GHOST_CHECK=1'
	@fixtures=$$(sed -n 's|^PASS \([^ ]*\) .*|$(FUZZ_V2_DIR)/fixtures/\1.json|p' $(FUZZ_V2_DIR)/capture.log); \
		if [ -z "$$fixtures" ]; then \
			echo "[asx] fuzz-ghost: FAIL (no captured scenarios in $(FUZZ_V2_DIR)/capture.log)"; exit 1; \
		fi; \
		echo "[asx] fuzz-ghost: $$(printf '%s\n' $$fixtures | wc -l | tr -d ' ') generated scenario(s) under the ghost monitors..."; \
		$(TEST_EXEC) $(CONFORMANCE_GHOST_DIR)/bin/asx-conformance compare $$fixtures
	@echo "[asx] fuzz-ghost: PASS"

# ---------------------------------------------------------------------------
# fuzz-promote — a minimized fuzz counterexample becomes a per-push fixture
# (bd-9kll.2.24). Takes build/fuzz_v2/seed-<FUZZ_V2_SEED>/minimized/
# <PROMOTE_ID>-min.scenario.json (written by fuzz-minimize), renames it
# <PROMOTE_NAME> (default <PROMOTE_ID>-min-001, the schema's id pattern),
# validates it, captures its Rust fixture with twin_run now, requires the C
# runtime to PASS it, and installs the pair in tests/conformance/scenarios_v2
# and fixtures/rust_reference_v2; then lint-scenarios-v2 and fixture-integrity
# run over the corpus. Never overwrites a corpus file. PROMOTE_ALLOW_FAILING=1
# lands a divergence the C runtime still FAILs (make conformance is red until
# it is fixed). The step is exercised, with stub tools, by
# tests/gates/test_fuzz_promotion.sh.
#   make fuzz-minimize FUZZ_V2_SEED=9
#   make fuzz-promote FUZZ_V2_SEED=9 PROMOTE_ID=gen-9-017 [PROMOTE_NAME=...]
# Then update the README rust_fixtures fact (tools/count_inventory.sh
# --facts) and commit both files with the divergence's bead.
# ---------------------------------------------------------------------------
.PHONY: fuzz-promote
fuzz-promote: $(CONFORMANCE_RUNNER)
	@test -n "$(PROMOTE_ID)" || \
		{ echo "usage: make fuzz-promote FUZZ_V2_SEED=<n> PROMOTE_ID=<generated scenario id> [PROMOTE_NAME=<id>-NNN] [PROMOTE_ALLOW_FAILING=1]"; exit 2; }
	@if [ -z "$(CHECK_JSONSCHEMA)" ]; then \
		echo "[asx] fuzz-promote: FAIL (check-jsonschema $(CHECK_JSONSCHEMA_VERSION) not found and no uvx)"; exit 1; \
	fi
	@TWIN_RUN="$(TWIN_RUN)" CHECK_JSONSCHEMA="$(CHECK_JSONSCHEMA)" \
		./tools/ci/promote_fuzz_counterexample.sh \
		--minimized $(FUZZ_V2_DIR)/minimized --id "$(PROMOTE_ID)" \
		--runner $(CONFORMANCE_RUNNER) \
		--note "Minimized from fuzz seed $(FUZZ_V2_SEED), scenario $(PROMOTE_ID)." \
		$(if $(PROMOTE_NAME),--name "$(PROMOTE_NAME)") \
		$(if $(PROMOTE_ALLOW_FAILING),--allow-failing)
	@$(MAKE) --no-print-directory lint-scenarios-v2 fixture-integrity

# Reproduce the live oracle's positive and planted-negative controls. The
# mutation changes runtime budget priority; it is built in a separate tree.
# Both runners execute actual Rust captures, and a reduced counterexample
# must pass the clean runtime and fail the mutant. No digest-only oracle.
FUZZ_V2_MUTANT_DIR ?= $(BUILD_DIR)/fuzz-priority-mutant
.PHONY: test-fuzz-differential
test-fuzz-differential: $(CONFORMANCE_RUNNER)
	@$(MAKE) --no-print-directory conformance-runner BUILD_DIR="$(FUZZ_V2_MUTANT_DIR)" \
		CFLAGS='$(CFLAGS) -DASX_MUTANT_BUDGET_PRIORITY_MIN'
	@if [ -z "$(ASX_TWIN_BIN)" ]; then \
		cargo build --release --locked --manifest-path tools/twin_run/Cargo.toml --bin twin_run; \
	fi
	@ASX_TWIN_BIN="$(if $(ASX_TWIN_BIN),$(ASX_TWIN_BIN),$(CURDIR)/tools/twin_run/target/release/twin_run)" \
		ASX_C_RUNNER="$(abspath $(CONFORMANCE_RUNNER))" \
		ASX_MUTANT_C_RUNNER="$(abspath $(FUZZ_V2_MUTANT_DIR))/bin/asx-conformance" \
		BUILD_DIR="$(abspath $(BUILD_DIR))" bash tests/fuzz/test_differential_rust_mode.sh
	@ASX_TWIN_BIN="$(if $(ASX_TWIN_BIN),$(ASX_TWIN_BIN),$(CURDIR)/tools/twin_run/target/release/twin_run)" \
		BUILD_DIR="$(abspath $(BUILD_DIR))" bash tests/fuzz/test_cross_determinism.sh

# ---------------------------------------------------------------------------
# canon-differential — the two trace canonicalizers agree (bd-9kll.9.1):
# twin_run writes CANON_COUNT random vocabulary traces for CANON_SEED with
# their Foata canonical form and digest from Rust's canon.rs; asx-conformance
# canon recomputes both with C's canon.c and fails on any difference. CI runs
# it next to fuzz-differential.
# ---------------------------------------------------------------------------
CANON_SEED ?= 1
CANON_COUNT ?= 100000

.PHONY: canon-differential
canon-differential: $(CONFORMANCE_RUNNER)
	@echo "[asx] canon-differential: seed=$(CANON_SEED) count=$(CANON_COUNT)"
	@$(TWIN_RUN) canon-fuzz --seed $(CANON_SEED) --count $(CANON_COUNT) \
		--out $(BUILD_DIR)/canon/seed-$(CANON_SEED).jsonl
	@$(CONFORMANCE_RUNNER) canon $(BUILD_DIR)/canon/seed-$(CANON_SEED).jsonl

# fixtures-fresh — every checked-in Rust fixture is what the pinned
# asupersync produces now through the current twin_run (bd-9kll.2.17): each
# fixture's scenario is captured again and the two are compared without
# their provenance (run id, toolchain, Cargo.lock digest). A twin_run change
# that alters a projection then fails here, instead of leaving the C runtime
# compared with an outdated capture. Needs cargo; CI runs it in the fuzz job.
FIXTURES_FRESH_DIR := $(BUILD_DIR)/fixtures-fresh
.PHONY: fixtures-fresh
fixtures-fresh:
	@echo "[asx] fixtures-fresh: capturing every v2 fixture's scenario again..."
	@mkdir -p $(FIXTURES_FRESH_DIR)/capture $(FIXTURES_FRESH_DIR)/compare
	@scenarios=""; \
	for f in fixtures/rust_reference_v2/*.json; do \
		id="$$(basename "$$f" .json)"; s="tests/conformance/scenarios_v2/$$id.json"; \
		[ -f "$$s" ] || { echo "[asx] fixtures-fresh: FAIL: fixture $$id has no scenario $$s"; exit 1; }; \
		scenarios="$$scenarios $$s"; \
	done; \
	$(TWIN_RUN) capture $$scenarios --out $(FIXTURES_FRESH_DIR)/capture >$(FIXTURES_FRESH_DIR)/capture.log 2>&1 || \
		{ tail -5 $(FIXTURES_FRESH_DIR)/capture.log; echo "[asx] fixtures-fresh: FAIL: capture failed"; exit 1; }; \
	stale=0; total=0; \
	for f in fixtures/rust_reference_v2/*.json; do \
		id="$$(basename "$$f" .json)"; total=$$((total + 1)); \
		jq -S 'del(.provenance)' "$$f" >$(FIXTURES_FRESH_DIR)/compare/$$id.checked-in.json; \
		jq -S 'del(.provenance)' $(FIXTURES_FRESH_DIR)/capture/$$id.json \
			>$(FIXTURES_FRESH_DIR)/compare/$$id.fresh.json; \
		if ! cmp -s $(FIXTURES_FRESH_DIR)/compare/$$id.checked-in.json \
			$(FIXTURES_FRESH_DIR)/compare/$$id.fresh.json; then \
			echo "[asx] fixtures-fresh: STALE $$id"; \
			diff $(FIXTURES_FRESH_DIR)/compare/$$id.checked-in.json \
				$(FIXTURES_FRESH_DIR)/compare/$$id.fresh.json | head -12; \
			stale=$$((stale + 1)); \
		fi; \
	done; \
	if [ "$$stale" -ne 0 ]; then echo "[asx] fixtures-fresh: FAIL ($$stale of $$total stale)"; exit 1; fi; \
	echo "[asx] fixtures-fresh: PASS ($$total fixtures reproduced)"

# check-rust-constants — the C runtime's kernel constants and defaults
# against Rust's (bd-9kll.2.13). schemas/rust_kernel_constants.json holds
# Rust's values, which `twin_run constants` reads from the pinned asupersync:
# its public API through the linked crate, its private constants from its
# source (syn). asx-conformance compares each with the C value. A constant
# without a C counterpart is declared N/A, and any other difference fails:
# there is no list of accepted differences. No cargo needed.
# rust-constants-fresh re-derives the document (needs cargo) and fails
# unless it equals the checked-in one. To refresh it after moving the pin,
# copy build/rust_kernel_constants.json over it unmodified.
RUST_CONSTANTS := schemas/rust_kernel_constants.json
.PHONY: check-rust-constants rust-constants-fresh
check-rust-constants: $(CONFORMANCE_RUNNER)
	@echo "[asx] check-rust-constants: C values vs $(RUST_CONSTANTS)"
	@$(CONFORMANCE_RUNNER) constants $(RUST_CONSTANTS)

rust-constants-fresh:
	@echo "[asx] rust-constants-fresh: re-deriving $(RUST_CONSTANTS) from the pinned asupersync"
	@mkdir -p $(BUILD_DIR)
	@host="$$(rustc -vV | sed -n 's/^host: //p')"; \
	manifest="$$(cargo metadata --locked --format-version 1 \
		--manifest-path tools/twin_run/Cargo.toml --filter-platform "$$host" | \
		jq -r '.packages[] | select(.name == "asupersync") | .manifest_path')"; \
	[ -n "$$manifest" ] || { echo "[asx] rust-constants-fresh: asupersync not found"; exit 1; }; \
	$(TWIN_RUN) constants --source "$$(dirname "$$manifest")" \
		--out $(BUILD_DIR)/rust_kernel_constants.json && \
	if cmp -s $(BUILD_DIR)/rust_kernel_constants.json $(RUST_CONSTANTS); then \
		echo "[asx] rust-constants-fresh: PASS ($(RUST_CONSTANTS) is current)"; \
	else \
		echo "[asx] rust-constants-fresh: FAIL: $(RUST_CONSTANTS) differs from the pinned asupersync"; \
		jq -S . $(RUST_CONSTANTS) >$(BUILD_DIR)/rust_kernel_constants.checked-in.json; \
		jq -S . $(BUILD_DIR)/rust_kernel_constants.json >$(BUILD_DIR)/rust_kernel_constants.derived.json; \
		diff $(BUILD_DIR)/rust_kernel_constants.checked-in.json \
			$(BUILD_DIR)/rust_kernel_constants.derived.json | head -60; \
		exit 1; \
	fi

# rust-rng-fresh — schemas/rust_det_rng.json holds the first 1000 outputs of
# Rust's DetRng for nine seeds (zero, the five degenerate patterns, three
# ordinary ones), which `twin_run rng` computes by running the pinned
# asupersync's own generator (bd-9kll.4.2). The C replica of the lab's pick is
# compared with it value for value by test_lab_dispatch
# (rng_replicates_rust_det_rng_for_every_seed_and_value_of_the_derived_document).
# This target (needs cargo) re-derives the document and fails unless it equals
# the checked-in one; to refresh it after moving the pin, copy
# build/rust_det_rng.json over it unmodified.
RUST_DET_RNG := schemas/rust_det_rng.json
.PHONY: rust-rng-fresh
rust-rng-fresh:
	@echo "[asx] rust-rng-fresh: re-deriving $(RUST_DET_RNG) from the pinned asupersync"
	@mkdir -p $(BUILD_DIR)
	@$(TWIN_RUN) rng --out $(BUILD_DIR)/rust_det_rng.json && \
	if cmp -s $(BUILD_DIR)/rust_det_rng.json $(RUST_DET_RNG); then \
		echo "[asx] rust-rng-fresh: PASS ($(RUST_DET_RNG) is current)"; \
	else \
		echo "[asx] rust-rng-fresh: FAIL: $(RUST_DET_RNG) differs from the pinned asupersync's DetRng"; \
		exit 1; \
	fi

# ---------------------------------------------------------------------------
# fixture-integrity — fixture schema, provenance, digest recomputation,
# capture_run_id format, unknown ops, and a codec round trip. It proves the
# fixtures are well-formed captures; it does NOT execute the C runtime.
# It and the two gates below depend on `build` so the build their script
# starts (tools/ci/run_conformance.sh) finds nothing to do when make -j runs
# them side by side (bd-4a9t).
# ---------------------------------------------------------------------------
fixture-integrity: build
	@echo "[asx] fixture-integrity: fixture schema/provenance/digest check..."
	@tools/ci/run_conformance.sh --mode fixture-integrity

# fixtures-promote — copy a staged capture (tools/ci/capture_rust_fixtures.sh)
# into fixtures/rust_reference; a baseline rebase needs owner sign-off in the
# rebase record (bd-9kll.2.1).
fixtures-promote:
	@[ -n "$(RUN_ID)" ] || { echo "usage: make fixtures-promote RUN_ID=<run_id>"; exit 2; }
	@tools/ci/promote_fixtures.sh --run-id "$(RUN_ID)"

# ---------------------------------------------------------------------------
# test-gates — negative controls: the gates themselves must fail on bad input
# ---------------------------------------------------------------------------
#
# Not run here: scripts in TEST_GATES_EXCLUDE have a CI job of their own.
# test_fuzz_parser_mutants.sh builds 12 sanitizer libraries (the planted-bug
# control of the parser fuzz harnesses, bd-9kll.16.7), too slow for every
# check; `make test-fuzz-parser-mutants` runs it in the fuzz-parity job.
TEST_GATES_EXCLUDE := tests/gates/test_fuzz_parser_mutants.sh

# Besides tests/gates/*.sh, the unit tests of CI and e2e scripts that build
# nothing (bd-grub: they ran in no job, and three of them had rotted):
# tests/ci/test_*.sh (the crate acceptance gate, the fixture pipeline e2e
# script, the e2e suite registry), the e2e harness contract and the checkpoint
# lint's own tests. Add a new such script here, or under tests/gates.
TEST_GATES_EXTRA := $(sort $(wildcard tests/ci/test_*.sh)) \
	tests/e2e/test_harness_smoke.sh tests/unit/core/test_checkpoint_lint.sh

test-gates:
	@echo "[asx] test-gates: running gate negative controls..."
	@for t in $(filter-out $(TEST_GATES_EXCLUDE),$(sort $(wildcard tests/gates/*.sh))) $(TEST_GATES_EXTRA); do \
		echo "  RUN  $$t"; \
		ASX_CONFORMANCE_RUNNER="$(abspath $(CONFORMANCE_RUNNER))" bash "$$t" || exit 1; \
	done
	@echo "[asx] test-gates: PASS"

# ---------------------------------------------------------------------------
# test-capacity-x4 — the unit suite with every capacity macro raised 4x
# (bd-9kll.10.4). Proves the overrides take effect and that no test or module
# hardcodes a default capacity. Separate BUILD_DIR, so build/ is untouched.
# ---------------------------------------------------------------------------
CAPACITY_X4_CFLAGS := \
	-DASX_ACTOR_MAILBOX_CAPACITY=64u -DASX_AFFINITY_TABLE_CAPACITY=1024u \
	-DASX_ACTOR_SYSTEM_LANE_CAPACITY=64u -DASX_NAME_REGISTRY_CAPACITY=64u \
	-DASX_BROADCAST_MAX_CAPACITY=128u -DASX_BUF_CAPACITY=16384u \
	-DASX_CHANNEL_MAX_CAPACITY=256u -DASX_CLEANUP_STACK_CAPACITY=128 \
	-DASX_ERROR_LEDGER_TASK_SLOTS=256u -DASX_EVENT_LOG_CAPACITY=2048u \
	-DASX_EVIDENCE_SINK_CAPACITY=256u -DASX_FS_FILE_CAPACITY=4096u \
	-DASX_GHOST_BORROW_TABLE_CAPACITY=512u -DASX_GHOST_DETERMINISM_CAPACITY=1024u \
	-DASX_GHOST_LINEARITY_CAPACITY=1024u -DASX_GHOST_RING_CAPACITY=256u \
	-DASX_HINDSIGHT_CAPACITY=1024u -DASX_IDEMPOTENCY_STORE_CAPACITY=64u \
	-DASX_LANE_TASK_CAPACITY=256u -DASX_LINK_CAPACITY=32u -DASX_MAX_ACTORS=64u \
	-DASX_MAX_BLOCKING_TASKS=64u \
	-DASX_MAX_BROADCASTS=32u -DASX_MAX_CANCEL_WITNESSES=256u -DASX_MAX_CHANNELS=64u \
	-DASX_MAX_DEADLINE_MONITORS=128u -DASX_MAX_EPOCH_OBSERVERS=32u -DASX_MAX_EPOCHS=64u \
	-DASX_MAX_FS_ENTRIES=64u -DASX_MAX_IO_TOKENS=256u \
	-DASX_MAX_OBLIGATIONS=512u -DASX_MAX_ONESHOTS=128u -DASX_MAX_OPEN_DIRS=32u \
	-DASX_MAX_OPEN_FILES=64u -DASX_MAX_PIPES=32u -DASX_MAX_PROCESSES=32u \
	-DASX_MAX_QUIC_CONNECTIONS=16u -DASX_MAX_QUIC_STREAMS=64u -DASX_MAX_REGION_CHILDREN=32u \
	-DASX_MAX_REGIONS=32u -DASX_MAX_RUNTIMES=4u -DASX_MAX_SESSIONS=32u \
	-DASX_MAX_SIGNAL_SUBSCRIPTIONS=64u \
	-DASX_MAX_SUPERVISORS=32u -DASX_MAX_SUSPECTS=16u -DASX_MAX_TASKS=256u \
	-DASX_MAX_TCP_LISTENERS=16u -DASX_MAX_TCP_STREAMS=64u -DASX_MAX_TIMERS=512u \
	-DASX_MAX_TLS_STREAMS=32u -DASX_MAX_UDP_SOCKETS=64u -DASX_MAX_UNIX_DGRAM_SOCKETS=32u \
	-DASX_MAX_UNIX_LISTENERS=16u -DASX_MAX_UNIX_STREAMS=64u -DASX_MAX_WAKERS=256u \
	-DASX_MAX_WATCHES=64u -DASX_MAX_WORKERS=256u -DASX_MAX_WS_CONNECTIONS=32u \
	-DASX_MONITOR_CAPACITY=32u -DASX_POSIX_BLOCKING_QUEUE_CAPACITY=32u \
	-DASX_POSIX_BLOCKING_WORKERS=16u -DASX_REGION_CAPTURE_ARENA_BYTES=65536u \
	-DASX_RESOLVER_CACHE_CAPACITY=32u \
	-DASX_RESOLVER_HOST_CAPACITY=1024u -DASX_SCHED_EVENT_LOG_CAPACITY=1024u \
	-DASX_SERVICE_BUFFER_CAPACITY=32u -DASX_SESSION_MAX_CAPACITY=64u \
	-DASX_SYMBOL_REGISTRY_CAPACITY=1024u -DASX_TRACE_CAPACITY=4096u \
	-DASX_UDP_PAYLOAD_CAPACITY=16384u -DASX_UDP_QUEUE_CAPACITY=16u \
	-DASX_UNIX_DGRAM_PAYLOAD_CAPACITY=16384u -DASX_UNIX_DGRAM_QUEUE_CAPACITY=16u \
	-DASX_WAIT_NODE_CAPACITY=1024u -DASX_WS_TX_CAPACITY=67584u

test-capacity-x4:
	@echo "[asx] test-capacity-x4: unit suite with every capacity macro raised 4x..."
	@$(MAKE) --no-print-directory test-unit BUILD_DIR=build/capacity-x4 \
		CFLAGS='$(CAPACITY_X4_CFLAGS)'
	@echo "[asx] test-capacity-x4: PASS"

# test-stress — the unit suite at stress scale under ASan+UBSan
# (bd-9kll.5.10): the capacity macros of test-capacity-x4 with room for 1024
# tasks. The crowd tests scale with ASX_MAX_TASKS, so here 1000 producers
# share a capacity-8 channel with ~30% cancelled, 1023 waiters line up on a
# mutex and on a notify, and 50 parties pass a barrier 20 times.
STRESS_CFLAGS := \
	$(filter-out -DASX_MAX_TASKS=% -DASX_MAX_OBLIGATIONS=% -DASX_WAIT_NODE_CAPACITY=%, \
		$(CAPACITY_X4_CFLAGS)) \
	-DASX_MAX_TASKS=1024u -DASX_MAX_OBLIGATIONS=2048u -DASX_WAIT_NODE_CAPACITY=4096u
.PHONY: test-stress
test-stress:
	@echo "[asx] test-stress: unit suite with room for 1024 tasks, under ASan+UBSan..."
	@$(SAN_ENV) $(MAKE) --no-print-directory test-unit BUILD_DIR=build/stress \
		CFLAGS='$(STRESS_CFLAGS) $(SAN_ASAN_FLAGS)' LDFLAGS='$(SAN_ASAN_FLAGS)'
	@echo "[asx] test-stress: PASS"

# The capacity macros: CAPACITY_X4_CFLAGS's names. The README's capacity
# table (make capacity-table) lists exactly these, and
# tools/ci/capacity_table.sh fails if a guarded capacity macro is missing.
print-capacity-macros:
	@echo $(foreach f,$(CAPACITY_X4_CFLAGS),$(firstword $(subst =, ,$(patsubst -D%,%,$(f)))))

capacity-table:
	@tools/ci/capacity_table.sh --out-dir $(BUILD_DIR)/capacity-table -- $(CC) $(ALL_CFLAGS)

# ---------------------------------------------------------------------------
# test-resource-classes — the unit suite built for each resource class
# (bd-9kll.13.1). RESOURCE_CLASS=1/2/3 sizes every arena from R1/R2/R3
# (asx_config.h), so test_boundary_exhaustion exhausts each class at its own
# limits and test_profile_compat checks the compiled capacities equal the
# class table. Each class builds in its own BUILD_DIR.
# ---------------------------------------------------------------------------
test-resource-classes:
	@for c in 1 2 3; do \
		echo "[asx] test-resource-classes: unit suite built for R$$c..."; \
		$(MAKE) --no-print-directory test-unit BUILD_DIR=build/class-r$$c \
			RESOURCE_CLASS=$$c || exit 1; \
	done
	@echo "[asx] test-resource-classes: PASS"

# ---------------------------------------------------------------------------
# test-asan / test-asan-live / test-tsan — the unit suite under sanitizers
# (bd-9kll.15.5), each in its own BUILD_DIR, so build/ is untouched:
#   test-asan       ASan + UBSan, deterministic CORE build;
#   test-asan-live  ASan + UBSan, live POSIX build (real threads, reactor);
#   test-tsan       TSan, live POSIX build (blocking pool, cross-thread
#                   wakers, signal self-pipe).
# Any finding fails the run: UBSan does not recover, ASan and TSan halt.
# ---------------------------------------------------------------------------
SAN_ASAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
# GCC refuses (-Werror=tsan) the atomic_thread_fence in platform/atomics.h
# because TSan does not model standalone fences. The fences stay in the
# binary; TSan sees the mpsc queue's publication through the acquire load
# and release store next to them. Clang has no -Wtsan and rejects the name.
SAN_TSAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer \
	$(if $(findstring clang,$(shell $(CC) --version 2>/dev/null)),,-Wno-tsan)
SAN_ENV := ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 TSAN_OPTIONS=halt_on_error=1

test-asan:
	@echo "[asx] test-asan: unit suite under ASan+UBSan (CORE, deterministic)..."
	@$(SAN_ENV) $(MAKE) --no-print-directory test-unit BUILD_DIR=build/asan \
		CFLAGS='$(SAN_ASAN_FLAGS)' LDFLAGS='$(SAN_ASAN_FLAGS)'
	@echo "[asx] test-asan: PASS"

test-asan-live:
	@echo "[asx] test-asan-live: unit suite under ASan+UBSan (POSIX, live)..."
	@$(SAN_ENV) $(MAKE) --no-print-directory test-unit BUILD_DIR=build/asan-live \
		PROFILE=POSIX DETERMINISTIC=0 CFLAGS='$(SAN_ASAN_FLAGS)' \
		LDFLAGS='$(SAN_ASAN_FLAGS) -lpthread -lrt'
	@echo "[asx] test-asan-live: PASS"

test-tsan:
	@echo "[asx] test-tsan: unit suite under TSan (POSIX, live)..."
	@$(SAN_ENV) $(MAKE) --no-print-directory test-unit BUILD_DIR=build/tsan \
		PROFILE=POSIX DETERMINISTIC=0 CFLAGS='$(SAN_TSAN_FLAGS)' \
		LDFLAGS='$(SAN_TSAN_FLAGS) -lpthread -lrt'
	@echo "[asx] test-tsan: PASS"

# ---------------------------------------------------------------------------
# conformance-asan / fuzz-asan — the executed corpus under ASan+UBSan
# (bd-9kll.2.22), in its own BUILD_DIR:
#   conformance-asan  every v2 fixture through a sanitized runner (the
#                     `conformance` recipe: self-test, then compare);
#   fuzz-asan         the scenarios the last `make fuzz-differential` run
#                     captured (same FUZZ_V2_SEED/FUZZ_V2_COUNT), compared
#                     by the same sanitized runner. It needs no Rust: it
#                     reads that run's capture.log and fixtures.
# Any sanitizer report fails the run (halt_on_error, no recovery), and so
# does a FAIL or ERROR. A green run covers the paths these scenarios reach
# in the R3-class CORE build; it is not a proof of memory safety, and the
# unit suite's sanitizer lanes (test-asan*) remain the other half.
# tests/gates/test_conformance_monitors.sh plants an out-of-bounds read
# (ASX_MUTANT_SANITIZER_OOB_READ) and expects this lane to report it.
# ---------------------------------------------------------------------------
CONFORMANCE_ASAN_DIR ?= build/conformance-asan
CONFORMANCE_ASAN_RUNNER := $(CONFORMANCE_ASAN_DIR)/bin/asx-conformance

.PHONY: conformance-asan fuzz-asan
conformance-asan:
	@echo "[asx] conformance-asan: fixture corpus under ASan+UBSan..."
	@$(SAN_ENV) $(MAKE) --no-print-directory conformance BUILD_DIR=$(CONFORMANCE_ASAN_DIR) \
		CFLAGS='$(CFLAGS) $(SAN_ASAN_FLAGS)' LDFLAGS='$(LDFLAGS) $(SAN_ASAN_FLAGS)'
	@echo "[asx] conformance-asan: PASS"

fuzz-asan:
	@test -f $(FUZZ_V2_DIR)/capture.log || \
		{ echo "[asx] fuzz-asan: run make fuzz-differential FUZZ_V2_SEED=$(FUZZ_V2_SEED) FUZZ_V2_COUNT=$(FUZZ_V2_COUNT) first"; exit 1; }
	@$(SAN_ENV) $(MAKE) --no-print-directory $(CONFORMANCE_ASAN_RUNNER) \
		BUILD_DIR=$(CONFORMANCE_ASAN_DIR) \
		CFLAGS='$(CFLAGS) $(SAN_ASAN_FLAGS)' LDFLAGS='$(LDFLAGS) $(SAN_ASAN_FLAGS)'
	@fixtures=$$(sed -n 's|^PASS \([^ ]*\) .*|$(FUZZ_V2_DIR)/fixtures/\1.json|p' $(FUZZ_V2_DIR)/capture.log); \
		if [ -z "$$fixtures" ]; then \
			echo "[asx] fuzz-asan: FAIL (no captured scenarios in $(FUZZ_V2_DIR)/capture.log)"; exit 1; \
		fi; \
		echo "[asx] fuzz-asan: $$(printf '%s\n' $$fixtures | wc -l | tr -d ' ') generated scenario(s) under ASan+UBSan..."; \
		$(SAN_ENV) $(TEST_EXEC) $(CONFORMANCE_ASAN_RUNNER) compare $$fixtures
	@echo "[asx] fuzz-asan: PASS"

# ---------------------------------------------------------------------------
# Executed parity (bd-9kll.2.9): every Rust-captured v2 fixture runs through
# the C runtime built for each profile (profile-parity-exec) and with the BIN
# codec (codec-equivalence-exec), each build in its own BUILD_DIR under
# build/parity/. Every build is compared with the same Rust captures, so all
# passing means the profiles and codecs produce the same canonical trace,
# snapshot, observations and dispatch order on the whole corpus. WIN32 is
# not in the list: it needs a Windows toolchain (the MSVC lane).
# The record checks after them (tools/ci/run_*.sh) still compare the digests
# stored in the fixture files and write the artifacts CI collects.
# ---------------------------------------------------------------------------
PARITY_PROFILES ?= CORE POSIX FREESTANDING EMBEDDED_ROUTER HFT AUTOMOTIVE PARALLEL BROWSER

.PHONY: profile-parity-exec codec-equivalence-exec
profile-parity-exec:
	@for p in $(PARITY_PROFILES); do \
		echo "[asx] profile-parity-exec: $$p"; \
		$(MAKE) --no-print-directory conformance PROFILE=$$p DETERMINISTIC=1 \
			BUILD_DIR=build/parity/$$p || { echo "[asx] profile-parity-exec: FAIL $$p"; exit 1; }; \
	done
	@echo "[asx] profile-parity-exec: PASS ($(words $(PARITY_PROFILES)) profiles x $(words $(CONFORMANCE_V2_FIXTURES)) fixtures)"

codec-equivalence-exec:
	@for c in JSON BIN; do \
		echo "[asx] codec-equivalence-exec: $$c"; \
		$(MAKE) --no-print-directory conformance CODEC=$$c DETERMINISTIC=1 \
			BUILD_DIR=build/parity/codec-$$c || { echo "[asx] codec-equivalence-exec: FAIL $$c"; exit 1; }; \
	done
	@echo "[asx] codec-equivalence-exec: PASS (JSON and BIN x $(words $(CONFORMANCE_V2_FIXTURES)) fixtures)"

# ---------------------------------------------------------------------------
# codec-equivalence — JSON vs BIN semantic digest parity
# ---------------------------------------------------------------------------
codec-equivalence: build codec-equivalence-exec
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
profile-parity: build profile-parity-exec
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
# parallel-parity — single-vs-multi-worker canonical digest parity
# ---------------------------------------------------------------------------
parallel-parity:
	@echo "[asx] parallel-parity: single-vs-multi-worker digest check..."
	@if [ -x tools/ci/run_parallel_parity.sh ]; then \
		tools/ci/run_parallel_parity.sh; \
	elif [ "$(FAIL_ON_MISSING_RUNNERS)" = "1" ]; then \
		echo "[asx] parallel-parity: FAIL (runner missing; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] parallel-parity: SKIP (runner not yet implemented)"; \
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
# Generates random operation mutations against the C runtime and verifies
# deterministic self-consistency. Rust parity is fuzz-differential (DSL v2).
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
ifneq ($(strip $(RUST_FUZZ_BINARY)),)
$(error RUST_FUZZ_BINARY is retired: use make fuzz-differential for live Rust/C parity)
endif
ASX_GIT_COMMIT ?= $(shell git -C $(CURDIR) show -s --format=%H HEAD 2>/dev/null || true)

FUZZ_CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -Werror \
               -Wno-unused-parameter -Wno-unused-result \
               -Wno-conversion -Wno-sign-conversion \
               -O2 -DNDEBUG \
               $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) \
               -I$(CURDIR)/tests -I$(CURDIR)/src $(CFLAGS)

.PHONY: fuzz-build fuzz-smoke fuzz-nightly fuzz-run fuzz-counterexample-replay

fuzz-build: $(FUZZ_BIN)

$(FUZZ_BIN): $(FUZZ_SRC) $(LIB_A) | $(FUZZ_DIR)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< $(LIB_A) $(ALL_LDFLAGS)

$(FUZZ_DIR):
	@mkdir -p $@

fuzz-smoke: fuzz-build
	@echo "[asx] fuzz-smoke: C determinism and mutation smoke test..."
	@$(FUZZ_BIN) --smoke

fuzz-nightly: fuzz-build
	@echo "[asx] fuzz-nightly: C determinism and mutation nightly run..."
	@$(FUZZ_BIN) --nightly --verbose

fuzz-run: fuzz-build
	@$(FUZZ_BIN) $(FUZZ_ARGS)

fuzz-counterexample-replay: fuzz-build
	@echo "[asx] fuzz-counterexample-replay: replaying the C determinism/crash corpus..."
	@ASX_GIT_COMMIT="$(ASX_GIT_COMMIT)" BUILD_DIR="$(BUILD_DIR)" bash tools/ci/replay_fuzz_counterexamples.sh

# test-fuzz-scripts — the integration tests of the fuzz gates (bd-grub): the
# smoke gate and the counterexample replay's negative controls, the minimizer
# pipeline, and the harnesses under ASan+UBSan (build_asan.sh builds those
# into build/asan through this Makefile). Run by the fuzz-parity job.
FUZZ_SCRIPT_TESTS := tests/fuzz/test_gate_fuzz.sh tests/fuzz/test_minimize_integration.sh

.PHONY: test-fuzz-scripts
test-fuzz-scripts: fuzz-build minimize-build
	@echo "[asx] test-fuzz-scripts: running the fuzz gates' integration tests..."
	@for t in $(FUZZ_SCRIPT_TESTS); do \
		echo "  RUN  $$t"; \
		bash "$$t" || exit 1; \
	done
	@echo "  RUN  tests/fuzz/test_harness_sanity.sh --quick"
	@bash tests/fuzz/test_harness_sanity.sh --quick
	@echo "[asx] test-fuzz-scripts: PASS"

# ---------------------------------------------------------------------------
# fuzz-parsers — libFuzzer harnesses for the parsers of untrusted input
# (bd-9kll.16.1): tests/fuzz/fuzz_parsers.c, one binary per target (its header
# lists them), built with clang against a library instrumented for the fuzzer
# and ASan+UBSan. fuzz-parsers-smoke runs each for FUZZ_PARSERS_SECONDS,
# starting from the committed seeds (tests/fuzz/parsers_seeds/<TARGET>); a
# crash, a sanitizer report, a timeout or a violated property fails it, and
# libFuzzer leaves the input (crash-*, timeout-*) in
# build/fuzz-parsers/run/<TARGET>. A found input is committed as a seed once
# fixed. fuzz-parsers-replay (in make test) builds the same harnesses with
# $(CC) and the strict flags, without libFuzzer, and runs every seed once.
# ---------------------------------------------------------------------------
FUZZ_PARSER_TARGETS := HTTP_REQUEST HTTP_RESPONSE WEBSOCKET BASE64 CODEC_JSON CODEC_BIN \
                       WEB_QUERY RAPTORQ_OTI RAPTORQ_DECODE FRAME_CODECS RESOLVE_HOST JSON_READER
FUZZ_PARSERS_CC ?= clang
FUZZ_PARSERS_DIR := build/fuzz-parsers
FUZZ_PARSERS_SECONDS ?= 60
FUZZ_PARSERS_SEEDS := tests/fuzz/parsers_seeds
# The grown corpus (bd-9kll.16.3): what soaks found, minimized with -merge=1.
# The smoke, the replay and the soak start from it as well as from the seeds;
# the mutant gate's discovery control starts from the seeds only.
FUZZ_PARSERS_CORPUS := tests/fuzz/parsers_corpus
FUZZ_PARSERS_SAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
# Extra compiler flags for the instrumented library only (not the harnesses).
# tests/gates/test_fuzz_parser_mutants.sh passes -DASX_MUTANT_<TARGET> here to
# build a parser with its planted bug (bd-9kll.16.7); nothing else sets it.
FUZZ_PARSERS_EXTRA_CFLAGS ?=

# test-fuzz-parser-mutants — the planted-bug control of the parser fuzz
# harnesses (bd-9kll.16.7): a replay control that gates and a discovery
# control that observes (tests/gates/test_fuzz_parser_mutants.sh, header). Run
# by the fuzz-parity CI job, not by test-gates. Needs clang with libFuzzer.
.PHONY: test-fuzz-parser-mutants
test-fuzz-parser-mutants:
	@bash tests/gates/test_fuzz_parser_mutants.sh

.PHONY: fuzz-parsers fuzz-parsers-smoke fuzz-parsers-replay
fuzz-parsers:
	@$(MAKE) --no-print-directory $(FUZZ_PARSERS_DIR)/lib/libasx.a BUILD_DIR=$(FUZZ_PARSERS_DIR) \
		CC=$(FUZZ_PARSERS_CC) \
		CFLAGS='-fsanitize=fuzzer-no-link $(FUZZ_PARSERS_SAN) $(FUZZ_PARSERS_EXTRA_CFLAGS)'
	@mkdir -p $(FUZZ_PARSERS_DIR)/bin
	@for t in $(FUZZ_PARSER_TARGETS); do \
		$(FUZZ_PARSERS_CC) $(STD_FLAGS) -g -O1 $(INC_FLAGS) $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) \
			$(CLASS_DEF) $(filter -D%,$(OPT_FLAGS)) -DASX_FUZZ_$$t -fsanitize=fuzzer $(FUZZ_PARSERS_SAN) \
			-o $(FUZZ_PARSERS_DIR)/bin/$$t tests/fuzz/fuzz_parsers.c \
			$(FUZZ_PARSERS_DIR)/lib/libasx.a || exit 1; \
	done
	@echo "[asx] fuzz-parsers: $(words $(FUZZ_PARSER_TARGETS)) harnesses in $(FUZZ_PARSERS_DIR)/bin"

fuzz-parsers-smoke: fuzz-parsers
	@for t in $(FUZZ_PARSER_TARGETS); do \
		run=$(FUZZ_PARSERS_DIR)/run/$$t; mkdir -p $$run/corpus; \
		cp $(FUZZ_PARSERS_SEEDS)/$$t/* $$run/corpus/ || exit 1; \
		if [ -d $(FUZZ_PARSERS_CORPUS)/$$t ]; then cp $(FUZZ_PARSERS_CORPUS)/$$t/* $$run/corpus/ || exit 1; fi; \
		echo "[asx] fuzz-parsers-smoke: $$t for $(FUZZ_PARSERS_SECONDS) s"; \
		( cd $$run && ../../bin/$$t -max_total_time=$(FUZZ_PARSERS_SECONDS) -timeout=10 \
			-rss_limit_mb=2048 -print_final_stats=1 corpus >>log.txt 2>&1 ) || \
			{ tail -40 $$run/log.txt; echo "[asx] fuzz-parsers-smoke: FAIL $$t (inputs in $$run)"; exit 1; }; \
		grep -E 'number_of_executed_units' $$run/log.txt | tail -1; \
	done
	@echo "[asx] fuzz-parsers-smoke: PASS ($(words $(FUZZ_PARSER_TARGETS)) parsers)"

fuzz-parsers-replay: $(LIB_A)
	@mkdir -p $(BUILD_DIR)/fuzz-parsers-replay
	@for t in $(FUZZ_PARSER_TARGETS); do \
		$(CC) $(ALL_CFLAGS) -DASX_FUZZ_STANDALONE -DASX_FUZZ_$$t \
			-o $(BUILD_DIR)/fuzz-parsers-replay/$$t tests/fuzz/fuzz_parsers.c $(LIB_A) \
			$(ALL_LDFLAGS) || exit 1; \
		$(TEST_EXEC) $(BUILD_DIR)/fuzz-parsers-replay/$$t $(FUZZ_PARSERS_SEEDS)/$$t/* \
			$$(find $(FUZZ_PARSERS_CORPUS)/$$t -type f 2>/dev/null | sort) || \
			{ echo "[asx] fuzz-parsers-replay: FAIL $$t"; exit 1; }; \
	done
	@echo "[asx] fuzz-parsers-replay: $(words $(FUZZ_PARSER_TARGETS)) harnesses ran their seeds"

# fuzz-parsers-soak — the harnesses for FUZZ_PARSERS_SOAK_SECONDS each from the
# committed seeds and corpus (bd-9kll.16.3, tools/ci/run_fuzz_soak.sh). A harness is
# started again after a crash so one finding does not end its time; the
# findings are the files of $(FUZZ_PARSERS_DIR)/soak/<TARGET>/crashes (it
# fails if there is one), the grown corpus is .../corpus, and the table of
# executions and coverage is .../summary.md. nightly.yml runs it per harness
# (FUZZ_PARSER_TARGETS=<one> selects one).
FUZZ_PARSERS_SOAK_SECONDS ?= 600
.PHONY: fuzz-parsers-soak
fuzz-parsers-soak: fuzz-parsers
	@bash tools/ci/run_fuzz_soak.sh --seconds $(FUZZ_PARSERS_SOAK_SECONDS) \
		--out $(FUZZ_PARSERS_DIR)/soak --bin-dir $(FUZZ_PARSERS_DIR)/bin \
		--seeds $(FUZZ_PARSERS_SEEDS) --seeds $(FUZZ_PARSERS_CORPUS) $(FUZZ_PARSER_TARGETS)

# parser-differential — the same bytes to asupersync's parser and to ours
# (bd-9kll.16.2). twin_run parse-fuzz writes a seeded corpus and the Rust
# outcomes (tools/twin_run/src/parsediff.rs), the ASX_DIFF_HTTP_REQUEST,
# ASX_DIFF_HTTP_RESPONSE, ASX_DIFF_WS_FRAME or ASX_DIFF_RAPTORQ mode of
# tests/fuzz/fuzz_parsers.c prints the C outcomes (PARSER_DIFF_TARGET=
# http-request, http-response, ws-frame or raptorq: the encoder of a block
# against asupersync's and the crates.io raptorq crate's), and
# tools/ci/parser_differential.py compares them, groups the differences by
# signature and request shape, and reduces the diverging inputs to classes of
# minimized input (report in $(PARSER_DIFF_OUT)). PARSER_DIFF_MODE=observe
# prints and passes; =gate fails on a divergence family not listed in
# tools/ci/parser_differential_known.txt. PARSER_DIFF_REQUIRE_HOST=0 runs the C
# parser without its Host requirement, so that policy does not mask other
# differences (CI runs both). TWIN_RUN is the twin_run command (ASX_TWIN_BIN,
# or cargo run); a prebuilt binary is faster per batch.
PARSER_DIFF_DIR := $(BUILD_DIR)/parser-differential
PARSER_DIFF_SEED ?= 1
PARSER_DIFF_COUNT ?= 2000
PARSER_DIFF_MODE ?= observe
PARSER_DIFF_REQUIRE_HOST ?=
PARSER_DIFF_OUT ?= $(PARSER_DIFF_DIR)/out
PARSER_DIFF_TARGET ?= http-request

.PHONY: parser-diff-build parser-differential parser-differential-selftest
parser-diff-build: $(LIB_A)
	@mkdir -p $(PARSER_DIFF_DIR)
	@$(CC) $(ALL_CFLAGS) -DASX_FUZZ_STANDALONE -DASX_DIFF_HTTP_REQUEST \
		-o $(PARSER_DIFF_DIR)/http_request tests/fuzz/fuzz_parsers.c $(LIB_A) $(ALL_LDFLAGS)
	@$(CC) $(ALL_CFLAGS) -DASX_FUZZ_STANDALONE -DASX_DIFF_HTTP_RESPONSE \
		-o $(PARSER_DIFF_DIR)/http_response tests/fuzz/fuzz_parsers.c $(LIB_A) $(ALL_LDFLAGS)
	@$(CC) $(ALL_CFLAGS) -DASX_FUZZ_STANDALONE -DASX_DIFF_WS_FRAME \
		-o $(PARSER_DIFF_DIR)/ws_frame tests/fuzz/fuzz_parsers.c $(LIB_A) $(ALL_LDFLAGS)
	@$(CC) $(ALL_CFLAGS) -DASX_FUZZ_STANDALONE -DASX_DIFF_RAPTORQ \
		-o $(PARSER_DIFF_DIR)/raptorq tests/fuzz/fuzz_parsers.c $(LIB_A) $(ALL_LDFLAGS)

parser-differential: parser-diff-build
	@python3 tools/ci/parser_differential.py run --twin-run "$(TWIN_RUN)" \
		--target $(PARSER_DIFF_TARGET) \
		--c-driver $(PARSER_DIFF_DIR)/$(subst -,_,$(PARSER_DIFF_TARGET)) --out $(PARSER_DIFF_OUT) \
		--seed $(PARSER_DIFF_SEED) --count $(PARSER_DIFF_COUNT) --mode $(PARSER_DIFF_MODE) \
		$(if $(PARSER_DIFF_REQUIRE_HOST),--c-require-host $(PARSER_DIFF_REQUIRE_HOST)) \
		--known tools/ci/parser_differential_known.txt

parser-differential-selftest:
	@python3 tools/ci/parser_differential.py --selftest
	@python3 tools/ci/check_raptorq_vectors.py --selftest

# raptorq-vectors-check — the golden vectors of tests/unit/raptorq/
# test_raptorq.c against the implementations that produced them (bd-9kll.16.2):
# the tables are read from the C source, twin_run computes each row with the
# crates.io raptorq 2.0.1 crate (and with asupersync's encoder where it has
# one), and every number is compared. A missing or empty table fails.
.PHONY: raptorq-vectors-check
raptorq-vectors-check:
	@python3 tools/ci/check_raptorq_vectors.py --twin-run "$(TWIN_RUN)"

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
ci-embedded-matrix:
	@# The canonical runner: OpenWrt musl or Debian glibc cross compiler per
	@# arch, JSONL rows with the compiler used, size and layout reports; a
	@# target with no compiler fails the run.
	@tools/ci/run_embedded_matrix.sh
	@if [ "$(RUN_QEMU_IN_MATRIX)" = "1" ]; then \
		$(MAKE) qemu-smoke FAIL_ON_MISSING_RUNNERS=$(FAIL_ON_MISSING_RUNNERS); \
	fi
	@echo "[asx] ci-embedded-matrix: all embedded targets built"

embedded-portability-proof:
	@echo "[asx] embedded-portability-proof: constrained profile + portability proof..."
	@bash tools/ci/run_embedded_portability_proof.sh

# ---------------------------------------------------------------------------
# release — optimized production build
# ---------------------------------------------------------------------------
release:
	$(MAKE) build BUILD_TYPE=release

# ---------------------------------------------------------------------------
# kernel-only — the ASX_KERNEL_ONLY build (bd-9kll.13.3; owner decision
# DEC-011/D6): the library without the conformance interpreter, the CLI and
# the protocol models (D5: QUIC, gRPC, DB, messaging, distributed, transport,
# remote). It is the artifact the plan section 11 footprint targets are
# measured on; size-report does the measuring. Its sources are the library's
# minus those, compiled in their own tree (KERNEL_BUILD_DIR) with
# -DASX_KERNEL_ONLY=1. kernel-only-check fails when a kernel object refers to
# a symbol only a left-out module defines (tools/ci/check_kernel_only.sh).
# ---------------------------------------------------------------------------
KERNEL_EXCLUDE_SRC := $(CONFORMANCE_SRC) $(CLI_SRC) $(TRANSPORT_SRC) $(REMOTE_SRC) \
	src/net/quic.c src/net/grpc.c src/net/db.c src/net/messaging.c src/net/distributed.c
KERNEL_SRC := $(filter-out $(KERNEL_EXCLUDE_SRC),$(LIB_SRC))
KERNEL_OBJ := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(KERNEL_SRC))
KERNEL_BUILD_DIR := $(BUILD_DIR)/kernel-only
KERNEL_LIB := $(KERNEL_BUILD_DIR)/lib/libasx_kernel.a
KERNEL_QUICKSTART := $(KERNEL_BUILD_DIR)/bin/ex_quickstart

.PHONY: kernel-only kernel-only-quickstart kernel-only-check

kernel-only:
	@$(MAKE) --no-print-directory BUILD_DIR=$(KERNEL_BUILD_DIR) \
		CFLAGS='$(CFLAGS) -DASX_KERNEL_ONLY=1' $(KERNEL_LIB)
	@echo "[asx] kernel-only: $(KERNEL_LIB) ($(words $(KERNEL_SRC)) of $(words $(LIB_SRC)) library sources)"

# The archive exists only in the kernel-only tree, whose make is given the
# macro: a plain `make build/lib/libasx_kernel.a` would archive objects built
# without it.
ifneq ($(filter -DASX_KERNEL_ONLY=1,$(CFLAGS)),)
$(LIB_DIR)/libasx_kernel.a: $(KERNEL_OBJ) | $(LIB_DIR)
	@tmp="$@.$$$$.tmp"; \
	$(AR) rcs "$$tmp" $^ && \
	mv "$$tmp" "$@"
endif

# The quickstart example linked against the kernel-only archive alone: what
# an application using the kernel pays, and the proof that the archive links.
kernel-only-quickstart: kernel-only
	@mkdir -p $(KERNEL_BUILD_DIR)/bin
	$(CC) $(VIGNETTE_CFLAGS) -DASX_KERNEL_ONLY=1 -o $(KERNEL_QUICKSTART) \
		examples/ex_quickstart.c $(KERNEL_LIB) $(ALL_LDFLAGS)
	@echo "[asx] kernel-only-quickstart: $(KERNEL_QUICKSTART)"

kernel-only-check: $(LIB_A) kernel-only
	@bash tools/ci/check_kernel_only.sh $(LIB_A) $(KERNEL_LIB)

# ---------------------------------------------------------------------------
# size-report — footprint of the kernel-only build per profile x resource
# class x build type, against the plan's CORE and EMBEDDED_ROUTER targets
# (bd-9kll.13.3): text/data/bss per object file, the archive totals and the
# linked quickstart. It builds in SIZE_REPORT_DIR, edits no baseline, and is
# observe-only unless SIZE_REPORT_STRICT=1 (then a build error or a figure
# over the hard threshold fails it). SIZE_REPORT_REQUIRE_BUILDS=1 gates the
# builds alone: a library build that fails, a quickstart that does not link or
# a kernel archive that fails its closure check exits nonzero, whatever the
# sizes; the CI lane size-report runs it so. Output: report.json and
# report.txt there.
#   make size-report SIZE_REPORT_PROFILES="CORE" SIZE_REPORT_CLASSES="1"
# ---------------------------------------------------------------------------
SIZE_REPORT_DIR ?= $(BUILD_DIR)/size-report
SIZE_REPORT_PROFILES ?= CORE POSIX FREESTANDING EMBEDDED_ROUTER
SIZE_REPORT_CLASSES ?= default 1 2 3
SIZE_REPORT_TYPES ?= release
SIZE_REPORT_STRICT ?= 0
SIZE_REPORT_REQUIRE_BUILDS ?= 0

.PHONY: size-report
size-report:
	@python3 tools/ci/size_report.py --out "$(SIZE_REPORT_DIR)" \
		--profiles "$(SIZE_REPORT_PROFILES)" --classes "$(SIZE_REPORT_CLASSES)" \
		--types "$(SIZE_REPORT_TYPES)" --cc "$(CC)" \
		$(if $(filter 1,$(SIZE_REPORT_REQUIRE_BUILDS)),--require-builds,) \
		$(if $(filter 1,$(SIZE_REPORT_STRICT)),--strict,)

# ---------------------------------------------------------------------------
# perf-compare — asx against asupersync on the same workloads, in one
# process, one sample of each engine in turn (bd-9kll.14.1,
# tools/perf_compare). The library and tools/perf_compare/c/asx_workloads.c
# are built here with the release flags (the workloads read arena-size
# macros, so they take the library's flags); the Rust program then links
# both and runs, with asupersync pinned by rev in its Cargo.toml. Output:
# report.json and report.txt in PERF_COMPARE_DIR.
#   make perf-compare                         # the full run
#   make perf-compare PERF_COMPARE_ARGS=--quick
#   make perf-compare PERF_COMPARE_ARGS="--history .bench-history --check-history"
# On a worker: rch exec -- make perf-compare
# ---------------------------------------------------------------------------
PERF_COMPARE_DIR ?= $(BUILD_DIR)/perf-compare
PERF_COMPARE_ARGS ?=
PERF_COMPARE_OBJ := $(PERF_COMPARE_DIR)/asx_workloads.o
PERF_COMPARE_LIB := $(PERF_COMPARE_DIR)/libasx_workloads.a

.PHONY: perf-compare

$(PERF_COMPARE_DIR):
	@mkdir -p $@

$(PERF_COMPARE_OBJ): tools/perf_compare/c/asx_workloads.c $(BUILD_CONFIG_FILE) | $(PERF_COMPARE_DIR)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(PERF_COMPARE_LIB): $(PERF_COMPARE_OBJ)
	$(AR) rcs $@ $<

perf-compare: $(LIB_A) $(PERF_COMPARE_LIB)
	ASX_LIB_DIR="$(abspath $(LIB_DIR))" ASX_WORKLOADS_DIR="$(abspath $(PERF_COMPARE_DIR))" \
		cargo run --release --manifest-path tools/perf_compare/Cargo.toml -- \
		--out "$(abspath $(PERF_COMPARE_DIR))" $(PERF_COMPARE_ARGS)

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
# release-install-smoke — install and run from packaged release artifact
# ---------------------------------------------------------------------------
release-install-smoke:
	@version="$(RELEASE_INSTALL_SMOKE_VERSION)"; \
	target="$(RELEASE_INSTALL_SMOKE_TARGET)"; \
	if [ -z "$$version" ] || [ -z "$$target" ]; then \
		echo "[asx] release-install-smoke: RELEASE_INSTALL_SMOKE_VERSION and RELEASE_INSTALL_SMOKE_TARGET must be set"; \
		exit 1; \
	fi; \
	$(MAKE) release-artifacts \
		RELEASE_VERSION="$$version" \
		RELEASE_TARGET="$$target" \
		RELEASE_KIND=binary \
		PROFILE=CORE \
		CODEC=BIN \
		DETERMINISTIC=1; \
	bash tools/ci/run_release_install_smoke.sh \
		--artifact "build/release/dist/asx-$$target.tar.xz" \
		--version "$$version" \
		--target "$$target"

# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# release gate (bd-9kll.18.5, bd-9kll.18.6)
#   release-gate-expected   the lanes a release waits for, expanded from the
#                           workflow files (what .github/workflows/release.yml
#                           requires at the tagged commit)
#   test-release-gate-e2e   the release-gate negative controls plus a real
#                           candidate: builds the library, verifies the
#                           archive, runs the installed consumer and doctor
#                           probe, then corrupts copies and requires rejection
# ---------------------------------------------------------------------------
.PHONY: release-gate-expected test-release-gate-e2e
release-gate-expected:
	@python3 tools/ci/release_gate.py expected --manifest tools/ci/release_required_gates.json

test-release-gate-e2e:
	@ASX_RELEASE_E2E=1 bash tests/gates/test_release_gate.sh

# install / uninstall
# ---------------------------------------------------------------------------
install: release
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include
	install -m 644 $(LIB_DIR)/libasx.a $(DESTDIR)$(PREFIX)/lib/
	cp -R include/asx $(DESTDIR)$(PREFIX)/include/
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

# build-msvc — what CI's msvc job runs: CMake's Visual Studio generator (x64,
# /W4 /WX), then the unit suite under CTest, for MSVC_PROFILE (CORE or
# WIN32). It needs Windows with Visual Studio and cmake; elsewhere it skips.
MSVC_PROFILE ?= WIN32
build-msvc:
	@case "$$(uname -s 2>/dev/null)" in \
		MINGW*|MSYS*|CYGWIN*|Windows_NT) on_windows=1 ;; *) on_windows=0 ;; esac; \
	if [ "$$on_windows" != 1 ] || ! command -v cmake >/dev/null 2>&1; then \
		echo "[asx] build-msvc: needs Windows with Visual Studio and cmake (CI: msvc job)"; \
		if [ "$(FAIL_ON_MISSING_CROSS_TOOLCHAINS)" = "1" ]; then \
			echo "[asx] build-msvc: FAIL (strict mode)"; \
			exit 1; \
		fi; \
		echo "[asx] build-msvc: SKIP (use STRICT_GATES=1 to fail)"; \
		exit 0; \
	fi; \
	cmake -S . -B build-msvc -A x64 -DASX_PROFILE=$(MSVC_PROFILE) && \
	cmake --build build-msvc --config Debug --parallel && \
	ctest --test-dir build-msvc -C Debug --output-on-failure --parallel 4

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
# build-wasm32 — the runtime as wasm32 modules (bd-9kll.15.4)
#
# clang --target=wasm32 -ffreestanding, linked with wasm-ld. The build box
# has no sysroot, no libc and no compiler-rt (worker B: clang and wasm-ld
# only), so the lane compiles the library against the declaration-only shim
# headers in tools/wasm/include and links exactly the C library symbols the
# library calls, defined in tools/wasm/wasm32_libc.c (the allocator is the
# static arena of src/runtime/hooks.c). wasm-ld runs without
# --allow-undefined: a new libc reference is a link error, not a stub.
# src/cli and src/console are the hosted stdio surfaces and stay out; the
# modules have no imports.
#
# One module per variant, build/wasm32/<variant>/asx.wasm:
#   core             PROFILE=CORE          deterministic single-thread kernel
#   freestanding     PROFILE=FREESTANDING  user hooks, static arena
#   browser          PROFILE=BROWSER       browser profile, extended
#   browser-minimal  PROFILE=BROWSER with ASX_BROWSER_PROFILE_MINIMAL
#
# Usage: make build-wasm32 [WASM32_CC=clang-21 WASM32_LD=wasm-ld-21]
# STRICT_GATES=1 fails (instead of SKIP) when the toolchain or node is
# missing. node only validates each module and instantiates it with no
# imports; the lane does not need it to build.
# ---------------------------------------------------------------------------
WASM32_CC ?= clang
WASM32_LD ?= wasm-ld
WASM32_NODE ?= node
WASM32_STACK_BYTES ?= 1048576
WASM32_DIR := $(BUILD_DIR)/wasm32

build-wasm32:
	@echo "[asx] build-wasm32: core, freestanding, browser, browser-minimal..."
	@if command -v $(WASM32_CC) >/dev/null 2>&1 && command -v $(WASM32_LD) >/dev/null 2>&1 && \
	    $(WASM32_CC) --print-targets 2>/dev/null | grep -q wasm32; then \
		$(MAKE) --no-print-directory wasm32-module PROFILE=CORE WASM32_VARIANT=core && \
		$(MAKE) --no-print-directory wasm32-module PROFILE=FREESTANDING WASM32_VARIANT=freestanding && \
		$(MAKE) --no-print-directory wasm32-module PROFILE=BROWSER WASM32_VARIANT=browser && \
		$(MAKE) --no-print-directory wasm32-module PROFILE=BROWSER WASM32_VARIANT=browser-minimal \
		        WASM32_VARIANT_DEFS=-DASX_BROWSER_PROFILE_MINIMAL; \
	elif [ "$(FAIL_ON_MISSING_CROSS_TOOLCHAINS)" = "1" ]; then \
		echo "[asx] build-wasm32: FAIL ($(WASM32_CC) with the wasm32 target and $(WASM32_LD) not found; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] build-wasm32: SKIP ($(WASM32_CC) with the wasm32 target and $(WASM32_LD) not found)"; \
	fi

# ---------------------------------------------------------------------------
# test-wasm32-replay — the v2 fixtures under a wasm runtime (bd-9kll.15.4)
#
# The replay modules are the library with R3 arenas (the conformance
# runner's) plus the entry tools/wasm/wasm32_replay.c, one per profile in
# WASM32_REPLAY_VARIANTS (name:PROFILE; BROWSER minimal has no lab to run a
# scenario). Each runs every fixtures/rust_reference_v2 fixture under node
# (V8's WebAssembly); the native asx-conformance `replay` command runs the
# same fixtures on this host; the outputs must be identical, so for every
# fixture the C run's semantic digest and projection digest are the x86_64
# CORE ones on every wasm32 profile, or the fixture fails the same way
# (an ERROR line). The lane needs node >= 20; STRICT_GATES=1 makes its
# absence a failure.
# ---------------------------------------------------------------------------
WASM32_REPLAY_DIR := $(WASM32_DIR)/replay
WASM32_REPLAY_VARIANTS := core:CORE freestanding:FREESTANDING browser:BROWSER

test-wasm32-replay: $(CONFORMANCE_RUNNER)
	@echo "[asx] test-wasm32-replay: $(words $(CONFORMANCE_V2_FIXTURES)) fixture(s) on wasm32 ($(WASM32_REPLAY_VARIANTS)) vs the native runner..."
	@if command -v $(WASM32_CC) >/dev/null 2>&1 && command -v $(WASM32_LD) >/dev/null 2>&1 && \
	    $(WASM32_CC) --print-targets 2>/dev/null | grep -q wasm32 && \
	    command -v $(WASM32_NODE) >/dev/null 2>&1; then \
		mkdir -p $(WASM32_REPLAY_DIR) && \
		$(TEST_EXEC) $(CONFORMANCE_RUNNER) replay $(CONFORMANCE_V2_FIXTURES) \
		        > $(WASM32_REPLAY_DIR)/native.txt; \
		rc=$$?; [ $$rc -le 1 ] || { echo "[asx] test-wasm32-replay: FAIL (native replay exit $$rc)"; exit 1; }; \
		runs=$$(grep -c ' sha256:.* sha256:' $(WASM32_REPLAY_DIR)/native.txt); \
		if [ "$$runs" -eq 0 ]; then \
			echo "[asx] test-wasm32-replay: FAIL (no fixture produced a digest line: nothing compared)"; \
			exit 1; \
		fi; \
		errs=$$(grep -c '^ERROR ' $(WASM32_REPLAY_DIR)/native.txt); \
		for v in $(WASM32_REPLAY_VARIANTS); do \
			name=$${v%%:*}; profile=$${v##*:}; \
			dir=$(WASM32_DIR)/replay-$$name; \
			$(MAKE) --no-print-directory wasm32-module PROFILE=$$profile RESOURCE_CLASS=3 \
			        WASM32_VARIANT=replay-$$name WASM32_ENTRY=tools/wasm/wasm32_replay.c || exit 1; \
			$(WASM32_NODE) tools/wasm/replay.js $$dir/asx.wasm $(CONFORMANCE_V2_FIXTURES) \
			        > $$dir/wasm.txt; \
			rc=$$?; [ $$rc -le 1 ] || { echo "[asx] test-wasm32-replay: FAIL ($$name: wasm replay exit $$rc)"; exit 1; }; \
			sed -n 's/^SKIP \([^ :]*\):.*/\1 sha256:/p' $$dir/wasm.txt > $$dir/skipped.txt; \
			grep -v -F -f $$dir/skipped.txt $(WASM32_REPLAY_DIR)/native.txt > $$dir/expected.txt; \
			grep -v '^SKIP ' $$dir/wasm.txt > $$dir/got.txt; \
			skips=$$(grep -c . $$dir/skipped.txt); \
			if cmp -s $$dir/expected.txt $$dir/got.txt; then \
				echo "[asx] test-wasm32-replay: $$name identical to x86_64 CORE ($$runs digest line(s) there, $$errs ERROR line(s), $$skips skipped as forbidden in the profile, DEC-008)"; \
			else \
				echo "[asx] test-wasm32-replay: FAIL ($$name differs from x86_64 CORE; first differences, < native, > wasm32):"; \
				diff $$dir/expected.txt $$dir/got.txt | head -20; \
				exit 1; \
			fi; \
		done; \
		echo "[asx] test-wasm32-replay: PASS ($(words $(WASM32_REPLAY_VARIANTS)) wasm32 profile(s) x $(words $(CONFORMANCE_V2_FIXTURES)) fixture(s))"; \
	elif [ "$(FAIL_ON_MISSING_RUNNERS)" = "1" ]; then \
		echo "[asx] test-wasm32-replay: FAIL (clang with wasm32, wasm-ld and $(WASM32_NODE) are required; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] test-wasm32-replay: SKIP (clang with wasm32, wasm-ld or $(WASM32_NODE) not found)"; \
	fi

ifneq ($(WASM32_VARIANT),)
WASM32_VARIANT_DEFS ?=
WASM32_ENTRY ?=
WASM32_OBJ_DIR := $(WASM32_DIR)/$(WASM32_VARIANT)/obj
WASM32_MODULE := $(WASM32_DIR)/$(WASM32_VARIANT)/asx.wasm
WASM32_SRC := $(filter-out $(CLI_SRC) $(CONSOLE_SRC),$(LIB_SRC))
WASM32_OBJ := $(patsubst src/%.c,$(WASM32_OBJ_DIR)/%.o,$(WASM32_SRC)) $(WASM32_OBJ_DIR)/wasm32_libc.o \
              $(if $(WASM32_ENTRY),$(WASM32_OBJ_DIR)/entry.o)
WASM32_CFLAGS := --target=wasm32 -ffreestanding $(STD_FLAGS) $(WARN_FLAGS) -O2 -DNDEBUG \
                 -ffunction-sections -fdata-sections \
                 $(INC_FLAGS) -I$(CURDIR)/src -isystem $(CURDIR)/tools/wasm/include \
                 $(PROFILE_DEF) $(CODEC_DEF) $(DET_DEF) $(CLASS_DEF) -DASX_NO_TESTING_LOG=1 \
                 $(WASM32_VARIANT_DEFS)

$(WASM32_OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(WASM32_CC) $(WASM32_CFLAGS) $(DEP_FLAGS) -c $< -o $@

# -fno-builtin: the loops in the libc are the shape LLVM turns back into calls
# to memcpy, memset and memmove, which there would be calls to themselves.
$(WASM32_OBJ_DIR)/wasm32_libc.o: tools/wasm/wasm32_libc.c
	@mkdir -p $(@D)
	$(WASM32_CC) $(WASM32_CFLAGS) $(DEP_FLAGS) -fno-builtin -c $< -o $@

# The variant's own entry (the replay module's tools/wasm/wasm32_replay.c).
$(WASM32_OBJ_DIR)/entry.o: $(WASM32_ENTRY)
	@mkdir -p $(@D)
	$(WASM32_CC) $(WASM32_CFLAGS) $(DEP_FLAGS) -c $< -o $@

$(WASM32_MODULE): $(WASM32_OBJ)
	$(WASM32_LD) --no-entry --export-all --error-limit=0 -z stack-size=$(WASM32_STACK_BYTES) \
	             -o $@ $(WASM32_OBJ)

wasm32-module: $(WASM32_MODULE)
	@echo "[asx] wasm32 $(WASM32_VARIANT): $(WASM32_MODULE) $$(wc -c < $(WASM32_MODULE)) bytes, $(words $(WASM32_OBJ)) objects, no undefined symbols"
	@if command -v $(WASM32_NODE) >/dev/null 2>&1; then \
		$(WASM32_NODE) tools/wasm/validate.js $(WASM32_MODULE); \
	elif [ "$(FAIL_ON_MISSING_RUNNERS)" = "1" ]; then \
		echo "[asx] wasm32 $(WASM32_VARIANT): FAIL ($(WASM32_NODE) not found to validate the module; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] wasm32 $(WASM32_VARIANT): SKIP validation ($(WASM32_NODE) not found)"; \
	fi

-include $(WASM32_OBJ:.o=.d)
endif

# ---------------------------------------------------------------------------
# Bare-metal cross-compilation targets (bd-aw69.4)
#
# These targets build static libraries for bare-metal ARM and RISC-V
# microcontrollers using arm-none-eabi-gcc and riscv64-unknown-elf-gcc.
# Each target gets a separate build directory under build/cross/<name>/.
# ---------------------------------------------------------------------------

# Generic bare-metal build recipe. RESOURCE_CLASS selects the arenas (the
# default class otherwise); BAREMETAL_EXTRA_CFLAGS is for the proof images
# (test-baremetal-qemu), which add section flags and the JSON document caps.
# Usage: $(call baremetal-build,<name>,<cc>,<ar>,<arch-flags>,<specs>,<profile>)
BAREMETAL_EXTRA_CFLAGS ?=
define baremetal-build
	@echo "[asx] cross-baremetal: building $(1) (profile=$(6))..."
	@if command -v $(2) >/dev/null 2>&1; then \
		bm_build="$(BUILD_DIR)/cross/$(1)"; \
		bm_obj="$$bm_build/obj"; \
		bm_lib="$$bm_build/lib"; \
		mkdir -p $$bm_lib; \
		for src in $(LIB_SRC); do \
			obj="$$bm_obj/$${src#src/}"; \
			obj="$${obj%.c}.o"; \
			mkdir -p "$${obj%/*}"; \
			$(2) $(STD_FLAGS) $(WARN_FLAGS) -Wno-type-limits -Wno-unused-value -Wno-format -O2 -DNDEBUG \
			     $(INC_FLAGS) -DASX_PROFILE_$(6) $(CODEC_DEF) $(DET_DEF) $(CLASS_DEF) \
			     $(BAREMETAL_EXTRA_CFLAGS) $(4) $(5) \
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
# test-baremetal-qemu — fixtures on bare-metal Cortex-M4 and RISC-V 32 under
# qemu-system (bd-9kll.15.7)
#
# For each of the two cores and each resource class in BM_CLASSES the
# library is built by the cross-baremetal targets above (FREESTANDING on
# arm-none-eabi, EMBEDDED_ROUTER on riscv64-unknown-elf with rv32imac), then
# linked with tools/baremetal/bm_main.c, that core's start-up and the
# fixtures of tests/baremetal/subset.txt compiled in as const arrays
# (tools/baremetal/embed_fixtures.sh; no file system). The image runs under
# qemu-system-arm -M mps2-an386 or qemu-system-riscv32 -M virt, prints
# `asx_conformance_replay_line` for each fixture through semihosting, and
# tools/baremetal/compare_replay.sh compares that with the host's
# `asx-conformance replay` of the same files: the lines (scenario id,
# semantic digest, projection digest) must be identical. Resource class R1
# must differ from the host exactly on tests/baremetal/exceeds_r1.txt, the
# fixtures its arenas are too small for; a stale entry fails.
#
# The planted control builds the same image at BM_CONTROL_CLASS with one
# byte of BM_CONTROL_ID's scenario changed (a sleep's nanoseconds, one digit
# up): that fixture's line must differ and no other must.
#
# Footprint per image (text/data/bss from the linked ELF, and by origin:
# kernel, harness, libc) is in $(BM_DIR)/<core>-c<class>/size.txt and is
# printed. Needs arm-none-eabi-gcc (with newlib), riscv64-unknown-elf-gcc
# (with picolibc), qemu-system-arm and qemu-system-riscv32; absent, the
# target SKIPs unless STRICT_GATES=1 (FAIL_ON_MISSING_CROSS_TOOLCHAINS or
# FAIL_ON_MISSING_RUNNERS), which makes it fail. Debian/Ubuntu packages:
# gcc-arm-none-eabi libnewlib-arm-none-eabi gcc-riscv64-unknown-elf
# picolibc-riscv64-unknown-elf qemu-system-arm qemu-system-riscv (qemu-system-
# misc on releases that still carry riscv there).
# ---------------------------------------------------------------------------
BM_DIR := $(BUILD_DIR)/baremetal
BM_SUBSET_FILE := tests/baremetal/subset.txt
BM_EXCEEDS_R1 := tests/baremetal/exceeds_r1.txt
BM_IDS := $(shell sed -e '/^[[:space:]]*\#/d' -e '/^[[:space:]]*$$/d' $(BM_SUBSET_FILE) 2>/dev/null)
BM_FIXTURES := $(addprefix fixtures/rust_reference_v2/,$(addsuffix .json,$(BM_IDS)))
BM_CLASSES ?= 1 2 3
BM_JSON_NODES ?= 8192
BM_JSON_TEXT ?= 131072
BM_CONTROL_ID ?= mpsc-send-evict-oldest-001
BM_CONTROL_CLASS ?= 3
BM_QEMU_ARM ?= qemu-system-arm
BM_QEMU_RV32 ?= qemu-system-riscv32
BM_CFLAGS := -ffunction-sections -fdata-sections -DASX_JSON_MAX_NODES=$(BM_JSON_NODES)u -DASX_JSON_MAX_TEXT=$(BM_JSON_TEXT)u
# core:make target:library dir:profile:qemu
BM_CORES := arm-m4:cross-baremetal-arm-m4-free:arm-m4-free:FREESTANDING:$(BM_QEMU_ARM) \
            riscv32:cross-baremetal-riscv32-router:riscv32-router:EMBEDDED_ROUTER:$(BM_QEMU_RV32)

.PHONY: test-baremetal-qemu
test-baremetal-qemu: $(CONFORMANCE_RUNNER)
	@echo "[asx] test-baremetal-qemu: $(words $(BM_FIXTURES)) fixture(s) x classes $(BM_CLASSES) on Cortex-M4 and RISC-V 32 under qemu-system..."
	@if command -v arm-none-eabi-gcc >/dev/null 2>&1 && command -v riscv64-unknown-elf-gcc >/dev/null 2>&1 && \
	    command -v $(BM_QEMU_ARM) >/dev/null 2>&1 && command -v $(BM_QEMU_RV32) >/dev/null 2>&1; then \
		mkdir -p $(BM_DIR) && \
		$(TEST_EXEC) $(CONFORMANCE_RUNNER) replay $(BM_FIXTURES) > $(BM_DIR)/native.txt; \
		rc=$$?; [ $$rc -le 1 ] || { echo "[asx] test-baremetal-qemu: FAIL (native replay exit $$rc)"; exit 1; }; \
		digests=$$(grep -c ' sha256:.* sha256:' $(BM_DIR)/native.txt); \
		[ "$$digests" -gt 0 ] || { echo "[asx] test-baremetal-qemu: FAIL (no digest line: nothing compared)"; exit 1; }; \
		sh tools/baremetal/embed_fixtures.sh $(BM_DIR)/bm_fixtures.c $(BM_FIXTURES) || exit 1; \
		sh tools/baremetal/embed_fixtures.sh $(BM_DIR)/control/bm_fixtures.c --corrupt $(BM_CONTROL_ID) $(BM_FIXTURES) || exit 1; \
		: > $(BM_DIR)/footprint.txt; \
		for core in $(BM_CORES); do \
			arch=$${core%%:*}; rest=$${core#*:}; target=$${rest%%:*}; rest=$${rest#*:}; \
			lib=$${rest%%:*}; rest=$${rest#*:}; profile=$${rest%%:*}; qemu=$${rest#*:}; \
			for cls in $(BM_CLASSES); do \
				d=$(BM_DIR)/$$arch-c$$cls; \
				mkdir -p $$d; \
				$(MAKE) --no-print-directory $$target BUILD_DIR=$$d RESOURCE_CLASS=$$cls \
				        BAREMETAL_EXTRA_CFLAGS="$(BM_CFLAGS)" > $$d/build.log 2>&1 || \
				    { echo "[asx] test-baremetal-qemu: FAIL ($$arch class $$cls library build; tail of $$d/build.log:)"; tail -5 $$d/build.log; exit 1; }; \
				sh tools/baremetal/proof.sh --arch $$arch --lib $$d/cross/$$lib/lib/libasx.a \
				        --fixtures $(BM_DIR)/bm_fixtures.c --out $$d --qemu $$qemu \
				        --cflags "-DASX_PROFILE_$$profile $(CODEC_DEF) $(DET_DEF) -DASX_RESOURCE_CLASS=$$cls $(BM_CFLAGS)" || \
				    { echo "[asx] test-baremetal-qemu: FAIL ($$arch class $$cls image build)"; exit 1; }; \
				flag=; [ "$$cls" = "1" ] && flag="--expect-differ $(BM_EXCEEDS_R1)"; \
				if sh tools/baremetal/compare_replay.sh $(BM_DIR)/native.txt $$d/qemu.txt $$flag > $$d/compare.txt 2>&1; then \
					echo "[asx] test-baremetal-qemu: $$arch class $$cls: $$(tail -1 $$d/compare.txt)"; \
				else \
					echo "[asx] test-baremetal-qemu: FAIL ($$arch class $$cls, qemu exit $$(cat $$d/qemu.rc)):"; cat $$d/compare.txt; exit 1; \
				fi; \
				{ echo "$$arch class $$cls:"; grep -A1 '^ *text' $$d/size.txt | tail -1; grep -E '^(origin|kernel|harness|libc) ' $$d/size.txt; } >> $(BM_DIR)/footprint.txt; \
			done; \
			case " $(BM_CLASSES) " in *" $(BM_CONTROL_CLASS) "*) ;; \
				*) echo "[asx] test-baremetal-qemu: FAIL (BM_CONTROL_CLASS $(BM_CONTROL_CLASS) is not in BM_CLASSES)"; exit 1 ;; esac; \
			d=$(BM_DIR)/$$arch-c$(BM_CONTROL_CLASS); c=$(BM_DIR)/$$arch-control; \
			sh tools/baremetal/proof.sh --arch $$arch --lib $$d/cross/$$lib/lib/libasx.a \
			        --fixtures $(BM_DIR)/control/bm_fixtures.c --out $$c --qemu $$qemu \
			        --cflags "-DASX_PROFILE_$$profile $(CODEC_DEF) $(DET_DEF) -DASX_RESOURCE_CLASS=$(BM_CONTROL_CLASS) $(BM_CFLAGS)" || \
			    { echo "[asx] test-baremetal-qemu: FAIL ($$arch control image build)"; exit 1; }; \
			if sh tools/baremetal/compare_replay.sh $(BM_DIR)/native.txt $$c/qemu.txt --expect-differ $(BM_CONTROL_ID) > $$c/compare.txt 2>&1; then \
				echo "[asx] test-baremetal-qemu: $$arch planted control ($(BM_CONTROL_ID), one byte): $$(tail -1 $$c/compare.txt)"; \
			else \
				echo "[asx] test-baremetal-qemu: FAIL ($$arch planted control not detected):"; cat $$c/compare.txt; exit 1; \
			fi; \
		done; \
		echo "[asx] test-baremetal-qemu: footprint (bytes; flash = text + data; $(BM_DIR)/footprint.txt):"; cat $(BM_DIR)/footprint.txt; \
		echo "[asx] test-baremetal-qemu: PASS (2 cores x classes $(BM_CLASSES), $(words $(BM_FIXTURES)) fixture(s), $$digests digest line(s) on the host)"; \
	elif [ "$(FAIL_ON_MISSING_CROSS_TOOLCHAINS)" = "1" ] || [ "$(FAIL_ON_MISSING_RUNNERS)" = "1" ]; then \
		echo "[asx] test-baremetal-qemu: FAIL (arm-none-eabi-gcc, riscv64-unknown-elf-gcc, $(BM_QEMU_ARM) and $(BM_QEMU_RV32) are required; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] test-baremetal-qemu: SKIP (arm-none-eabi-gcc, riscv64-unknown-elf-gcc, $(BM_QEMU_ARM) or $(BM_QEMU_RV32) not found)"; \
	fi

# ---------------------------------------------------------------------------
# test-musl-qemu — the library on musl for OpenWrt-class targets, run under
# qemu-user (bd-9kll.15.8)
#
# The library (EMBEDDED_ROUTER profile, resource classes MUSL_CLASSES) and
# the conformance runner are cross-built statically against musl for a
# big-endian 32-bit MIPS (o32, soft float, mips32r2: OpenWrt's mips_24kc) and
# for aarch64 (OpenWrt's aarch64_generic), by `zig cc`, which carries clang
# and the musl sources and builds a real musl libc.a for each triple. The
# runner replays the fixtures of tests/baremetal/subset.txt under qemu-mips
# and qemu-aarch64; tools/baremetal/compare_replay.sh compares each line (id,
# semantic digest, projection digest) with the host runner's, with resource
# class 1 differing exactly on tests/baremetal/exceeds_r1.txt, as on the bare
# metal images. A planted one-byte corruption of one fixture must change that
# fixture's line and no other.
#
# The footprint a router application pays is the kernel-only static app
# tools/musl/kernel_min.c: one region, one task, one channel, linked against
# the kernel-only sources (-DASX_KERNEL_ONLY=1, no interpreter, CLI or
# protocol models) with --gc-sections. Its text/data/bss per target and class
# is printed to $(MUSL_DIR)/footprint.txt next to the runner binary's.
#
# Needs zig (MUSL_ZIG, `zig` or `python3 -m ziglang` from the PyPI package
# ziglang==0.14.1) and qemu-mips and qemu-aarch64 (qemu-user); absent, the
# target SKIPs unless STRICT_GATES=1 (FAIL_ON_MISSING_CROSS_TOOLCHAINS or
# FAIL_ON_MISSING_RUNNERS), which makes it fail. This is musl built from
# source by zig, not OpenWrt's own toolchain or a device (bd-9kll.15.8's
# device half stays open).
# ---------------------------------------------------------------------------
MUSL_ZIG ?= zig
# -Os: the router build is measured in bytes (bd-dkfs: mips R1 kernel-only app
# 151,524 B of flash at -O2, 122,792 B at -Os, behavior unchanged: the replay
# digests are the same). MUSL_LTO=-flto links the whole program at once: the
# same app is 50 KB, because the reset and lab tables nothing reaches fall
# away; it is opt-in until it has its own CI run.
MUSL_OPT ?= -Os
MUSL_LTO ?=
MUSL_DIR := $(BUILD_DIR)/musl
MUSL_CLASSES ?= 1 2
MUSL_CONTROL_CLASS ?= 2
MUSL_CONTROL_ID ?= $(BM_CONTROL_ID)
MUSL_KERNEL_MIN_LINE := kernel-min: region ok, task ok, channel ok (42), quiescent
# name:zig target:extra compile flags:qemu-user binary
MUSL_TARGETS := mips:mips-linux-musleabi:-mcpu=mips32r2:qemu-mips \
                aarch64:aarch64-linux-musl::qemu-aarch64

ifneq ($(MUSL_VARIANT),)
MUSL_VDIR := $(MUSL_DIR)/$(MUSL_VARIANT)
MUSL_CC := $(MUSL_ZIG) cc -target $(MUSL_TRIPLE) $(MUSL_ARCHFLAGS)
MUSL_CFLAGS := $(STD_FLAGS) $(WARN_FLAGS) $(MUSL_OPT) $(MUSL_LTO) -DNDEBUG -ffunction-sections -fdata-sections \
               $(INC_FLAGS) -I$(CURDIR)/src $(CODEC_DEF) $(DET_DEF) \
               -DASX_PROFILE_EMBEDDED_ROUTER -DASX_RESOURCE_CLASS=$(MUSL_CLASS_NUM)
MUSL_FULL_OBJ := $(patsubst src/%.c,$(MUSL_VDIR)/full/%.o,$(LIB_SRC))
MUSL_KERN_OBJ := $(patsubst src/%.c,$(MUSL_VDIR)/kernel/%.o,$(KERNEL_SRC))

$(MUSL_VDIR)/full/%.o: src/%.c
	@mkdir -p $(@D)
	@$(MUSL_CC) $(MUSL_CFLAGS) $(DEP_FLAGS) -c $< -o $@

$(MUSL_VDIR)/kernel/%.o: src/%.c
	@mkdir -p $(@D)
	@$(MUSL_CC) $(MUSL_CFLAGS) -DASX_KERNEL_ONLY=1 $(DEP_FLAGS) -c $< -o $@

$(MUSL_VDIR)/libasx_full.a: $(MUSL_FULL_OBJ)
	@tmp="$@.$$$$.tmp"; $(MUSL_ZIG) ar rcs "$$tmp" $^ && mv "$$tmp" "$@"

$(MUSL_VDIR)/libasx_kernel.a: $(MUSL_KERN_OBJ)
	@tmp="$@.$$$$.tmp"; $(MUSL_ZIG) ar rcs "$$tmp" $^ && mv "$$tmp" "$@"

$(MUSL_VDIR)/asx-conformance: tools/conformance/runner.c $(MUSL_VDIR)/libasx_full.a
	@$(MUSL_CC) $(MUSL_CFLAGS) -static -Wl,--gc-sections -o $@ $^

$(MUSL_VDIR)/kernel_min: tools/musl/kernel_min.c $(MUSL_VDIR)/libasx_kernel.a
	@$(MUSL_CC) $(MUSL_CFLAGS) -DASX_KERNEL_ONLY=1 -static -Wl,--gc-sections -o $@ $^

.PHONY: musl-variant
musl-variant: $(MUSL_VDIR)/asx-conformance $(MUSL_VDIR)/kernel_min

-include $(MUSL_FULL_OBJ:.o=.d) $(MUSL_KERN_OBJ:.o=.d)
endif

.PHONY: test-musl-qemu
test-musl-qemu: $(CONFORMANCE_RUNNER)
	@echo "[asx] test-musl-qemu: $(words $(BM_FIXTURES)) fixture(s) x classes $(MUSL_CLASSES) on musl mips (big-endian) and aarch64 under qemu-user..."
	@if $(MUSL_ZIG) version >/dev/null 2>&1 && command -v qemu-mips >/dev/null 2>&1 && \
	    command -v qemu-aarch64 >/dev/null 2>&1; then \
		mkdir -p $(MUSL_DIR) && \
		$(TEST_EXEC) $(CONFORMANCE_RUNNER) replay $(BM_FIXTURES) > $(MUSL_DIR)/native.txt; \
		rc=$$?; [ $$rc -le 1 ] || { echo "[asx] test-musl-qemu: FAIL (native replay exit $$rc)"; exit 1; }; \
		digests=$$(grep -c ' sha256:.* sha256:' $(MUSL_DIR)/native.txt); \
		[ "$$digests" -gt 0 ] || { echo "[asx] test-musl-qemu: FAIL (no digest line: nothing compared)"; exit 1; }; \
		sh tools/baremetal/embed_fixtures.sh $(MUSL_DIR)/control/control.c --corrupt $(MUSL_CONTROL_ID) \
		        fixtures/rust_reference_v2/$(MUSL_CONTROL_ID).json || exit 1; \
		: > $(MUSL_DIR)/footprint.txt; \
		for t in $(MUSL_TARGETS); do \
			name=$${t%%:*}; rest=$${t#*:}; triple=$${rest%%:*}; rest=$${rest#*:}; \
			flags=$${rest%%:*}; qemu=$${rest#*:}; \
			for cls in $(MUSL_CLASSES); do \
				v=$$name-c$$cls; d=$(MUSL_DIR)/$$v; \
				$(MAKE) --no-print-directory musl-variant MUSL_VARIANT=$$v MUSL_TRIPLE=$$triple \
				        MUSL_ARCHFLAGS="$$flags" MUSL_CLASS_NUM=$$cls > $(MUSL_DIR)/$$v.build.log 2>&1 || \
				    { echo "[asx] test-musl-qemu: FAIL ($$name class $$cls build; tail of $(MUSL_DIR)/$$v.build.log:)"; \
				      tail -8 $(MUSL_DIR)/$$v.build.log; exit 1; }; \
				$$qemu $$d/asx-conformance replay $(BM_FIXTURES) > $$d/qemu.txt 2> $$d/qemu.err; \
				flag=; [ "$$cls" = "1" ] && flag="--expect-differ $(BM_EXCEEDS_R1)"; \
				if sh tools/baremetal/compare_replay.sh $(MUSL_DIR)/native.txt $$d/qemu.txt $$flag > $$d/compare.txt 2>&1; then \
					echo "[asx] test-musl-qemu: $$name class $$cls: $$(tail -1 $$d/compare.txt)"; \
				else \
					echo "[asx] test-musl-qemu: FAIL ($$name class $$cls):"; cat $$d/compare.txt; exit 1; \
				fi; \
				got=$$($$qemu $$d/kernel_min 2>&1); krc=$$?; \
				if [ $$krc -eq 0 ] && [ "$$got" = "$(MUSL_KERNEL_MIN_LINE)" ]; then \
					echo "[asx] test-musl-qemu: $$name class $$cls kernel_min: $$got"; \
				else \
					echo "[asx] test-musl-qemu: FAIL ($$name class $$cls kernel_min exit $$krc: $$got)"; exit 1; \
				fi; \
				{ echo "$$name class $$cls (asx-conformance, then kernel_min):"; \
				  python3 -I tools/musl/elf_size.py $$d/asx-conformance $$d/kernel_min; } >> $(MUSL_DIR)/footprint.txt; \
			done; \
			case " $(MUSL_CLASSES) " in *" $(MUSL_CONTROL_CLASS) "*) ;; \
				*) echo "[asx] test-musl-qemu: FAIL (MUSL_CONTROL_CLASS $(MUSL_CONTROL_CLASS) is not in MUSL_CLASSES)"; exit 1 ;; esac; \
			d=$(MUSL_DIR)/$$name-c$(MUSL_CONTROL_CLASS); \
			$$qemu $$d/asx-conformance replay \
			        $(subst fixtures/rust_reference_v2/$(MUSL_CONTROL_ID).json,$(MUSL_DIR)/control/corrupted-$(MUSL_CONTROL_ID).json,$(BM_FIXTURES)) \
			        > $$d/control.txt 2> $$d/control.err; \
			if sh tools/baremetal/compare_replay.sh $(MUSL_DIR)/native.txt $$d/control.txt --expect-differ $(MUSL_CONTROL_ID) > $$d/control-compare.txt 2>&1; then \
				echo "[asx] test-musl-qemu: $$name planted control ($(MUSL_CONTROL_ID), one byte): $$(tail -1 $$d/control-compare.txt)"; \
			else \
				echo "[asx] test-musl-qemu: FAIL ($$name planted control not detected):"; cat $$d/control-compare.txt; exit 1; \
			fi; \
		done; \
		echo "[asx] test-musl-qemu: footprint (bytes; $(MUSL_DIR)/footprint.txt):"; cat $(MUSL_DIR)/footprint.txt; \
		echo "[asx] test-musl-qemu: PASS (musl mips + aarch64 x classes $(MUSL_CLASSES), $(words $(BM_FIXTURES)) fixture(s), $$digests digest line(s) on the host)"; \
	elif [ "$(FAIL_ON_MISSING_CROSS_TOOLCHAINS)" = "1" ] || [ "$(FAIL_ON_MISSING_RUNNERS)" = "1" ]; then \
		echo "[asx] test-musl-qemu: FAIL ($(MUSL_ZIG), qemu-mips and qemu-aarch64 are required; strict mode)"; \
		exit 1; \
	else \
		echo "[asx] test-musl-qemu: SKIP ($(MUSL_ZIG), qemu-mips or qemu-aarch64 not found)"; \
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
.PHONY: check check-ci ci-embedded-baremetal
check: format-check lint lint-docs lint-checkpoint lint-anti-butchering lint-evidence lint-semantic-delta lint-static-analysis lint-schema-validation build test model-check abi-check test-abi-shim formal-check

check-ci: CI=1
check-ci: format-check lint lint-checkpoint lint-anti-butchering lint-evidence lint-semantic-delta lint-static-analysis lint-schema-validation lint-scenarios-v2 build build-browser test-browser-focused test-browser-minimal-focused test test-capacity-x4 model-check test-e2e-vertical fixture-integrity test-gates codec-equivalence profile-parity parallel-parity fuzz-smoke ci-embedded-matrix ci-embedded-baremetal

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
	@echo "  build-parallel     Build PARALLEL profile with live atomic backend enabled"
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
	@echo "  test-combinator-contract Run combinator/plan/service contract suites"
	@echo "  test-actor-supervision-harness Run actor (GenServer) and supervision semantic harness suites"
	@echo "  test-invariants    Lifecycle invariant tests"
	@echo "  test-vignettes     API ergonomics usage vignettes (public headers)"
	@echo "  test-e2e           Run all e2e scenario lanes"
	@echo "  test-e2e-vertical  Run HFT/automotive/continuity e2e lanes"
	@echo "  test-e2e-suite     Run ALL e2e families with unified manifest"
	@echo "  test-e2e-posix-adapter Run POSIX reactor/readiness adapter smoke"
	@echo "  test-e2e-network-surface Run deterministic network primitive e2e smoke"
	@echo "  test-e2e-native-record-replay Record a native HTTP server run, replay it with no sockets, require a bit-exact digest"
	@echo "  test-e2e-native-crash-continuity SIGKILL a recorded server at random stream offsets, replay each survivor's committed prefix"
	@echo "  test-e2e-actor-supervision Run deterministic actor/supervision semantic e2e smoke"
	@echo "  wave-c-acceptance-demo Run Wave C user-facing acceptance evidence bundle"
	@echo "  conformance        Rust fixture parity verification"
	@echo "  codec-equivalence  JSON vs BIN codec equivalence"
	@echo "  profile-parity     Cross-profile semantic digest parity"
	@echo "  parallel-parity    Single-vs-multi-worker semantic digest parity"
	@echo "  crate-acceptance-gate Aggregate final crate-level parity evidence"
	@echo "  fuzz-smoke         C determinism and mutation smoke test"
	@echo "  fuzz-differential  Live Rust/C scenario comparison"
	@echo "  fuzz-timers        1000 timer-heavy scenarios (seed 7100) Rust vs C"
	@echo "  fuzz-metamorphic   M1-M4 over generated scenarios, 10000 cases each, ddmin on a violation"
	@echo "  test-fuzz-differential Detect and minimize a runtime mutation"
	@echo "  fuzz-counterexample-replay Replay C determinism/crash counterexamples"
	@echo "  minimize-selftest  Counterexample minimizer self-test"
	@echo "  ci-embedded-matrix Cross-target embedded builds (Linux-musl)"
	@echo "  embedded-portability-proof Compact EMBEDDED_ROUTER/R1 portability proof"
	@echo "  cross-baremetal-all All 8 bare-metal ARM/RISC-V targets"
	@echo "  test-baremetal-qemu Fixtures on bare-metal Cortex-M4 + RISC-V 32 under qemu-system, digests equal the host's"
	@echo "  test-musl-qemu     musl mips (BE) + aarch64 under qemu-user: digests equal the host's, kernel-only app footprint"
	@echo "  bench              Performance benchmarks (JSON output)"
	@echo "  bench-json         Benchmarks (JSON-only to stdout)"
	@echo "  parallel-bench-json PARALLEL worker-count baseline JSON"
	@echo "  parallel-bench-gate Capture PARALLEL baseline JSON and observe-only warnings"
	@echo "  release-evidence-manifest Generate unified release evidence manifest"
	@echo "  resource-pressure-gate Replay failure-atomic resource pressure scenarios"
	@echo "  release            Optimized production build"
	@echo "  kernel-only        ASX_KERNEL_ONLY archive: no conformance interpreter, CLI or protocol models"
	@echo "  kernel-only-check  Fail if a kernel-only object needs a left-out module"
	@echo "  size-report        Footprint per profile x class: objects, archive, linked quickstart"
	@echo "  perf-compare       asx vs asupersync, same workloads, one process (needs cargo)"
	@echo "  perf-digest        rolling-digest fold cost and ns per spawn+run, before and after"
	@echo "  bench-journal-overhead  HTTP keep-alive throughput with the journal off and on (RP1 budget 10%)"
	@echo "  release-artifacts  Build release tar.xz + checksum/signature/provenance bundles"
	@echo "  release-install-smoke Install release tarball into a prefix and run README quickstart"
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
