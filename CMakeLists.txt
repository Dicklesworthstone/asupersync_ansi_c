# =============================================================================
# asx — asupersync ANSI C runtime
# Optional CMake generator (IDE/toolchain integration)
#
# The primary build system is the Makefile. This CMakeLists.txt is provided
# for IDE integration and consumers who require CMake. It mirrors the
# Makefile's source list, profile/codec flags, and warning policy.
#
# SPDX-License-Identifier: MIT
# =============================================================================

cmake_minimum_required(VERSION 3.10)
project(asx VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(ASX_PROFILE "CORE" CACHE STRING
    "Profile: CORE, POSIX, WIN32, FREESTANDING, EMBEDDED_ROUTER, HFT, AUTOMOTIVE")
set(ASX_CODEC "JSON" CACHE STRING "Codec: JSON, BIN")
option(ASX_DETERMINISTIC "Enable deterministic scheduling mode" ON)
option(ASX_BUILD_TESTS "Build test executables" ON)

message(STATUS "asx: Makefile is authoritative; CMake is an optional integration bridge.")

set(ASX_ALLOWED_PROFILES CORE POSIX WIN32 FREESTANDING EMBEDDED_ROUTER HFT AUTOMOTIVE)
list(FIND ASX_ALLOWED_PROFILES "${ASX_PROFILE}" ASX_PROFILE_INDEX)
if(ASX_PROFILE_INDEX EQUAL -1)
    message(FATAL_ERROR
        "Invalid ASX_PROFILE='${ASX_PROFILE}'. Allowed values: ${ASX_ALLOWED_PROFILES}")
endif()

set(ASX_ALLOWED_CODECS JSON BIN)
list(FIND ASX_ALLOWED_CODECS "${ASX_CODEC}" ASX_CODEC_INDEX)
if(ASX_CODEC_INDEX EQUAL -1)
    message(FATAL_ERROR
        "Invalid ASX_CODEC='${ASX_CODEC}'. Allowed values: ${ASX_ALLOWED_CODECS}")
endif()

# ---------------------------------------------------------------------------
# Compiler warnings (warnings-as-errors for core/kernel)
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic -Werror
        -Wconversion -Wsign-conversion -Wshadow
        -Wstrict-prototypes -Wmissing-prototypes
        -Wswitch-enum -Wformat=2
        -Wno-unused-parameter
    )
elseif(MSVC)
    add_compile_options(/W4 /WX)
endif()

# ---------------------------------------------------------------------------
# Profile and codec definitions
# ---------------------------------------------------------------------------
add_compile_definitions(
    ASX_PROFILE_${ASX_PROFILE}
    ASX_CODEC_${ASX_CODEC}
    ASX_DETERMINISTIC=$<BOOL:${ASX_DETERMINISTIC}>
)

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_definitions(ASX_DEBUG=1)
endif()

# ---------------------------------------------------------------------------
# Library sources
# ---------------------------------------------------------------------------
set(ASX_CORE_SRC
    src/core/status.c
    src/core/transition_tables.c
    src/core/outcome.c
    src/core/budget.c
    src/core/cancel.c
    src/core/cleanup.c
    src/core/ghost.c
    src/core/affinity.c
    src/core/adaptive.c
)

set(ASX_RUNTIME_SRC
    src/runtime/hooks.c
    src/runtime/lifecycle.c
    src/runtime/scheduler.c
    src/runtime/cancellation.c
    src/runtime/quiescence.c
    src/runtime/resource.c
    src/runtime/trace.c
    src/runtime/hindsight.c
    src/runtime/telemetry.c
    src/runtime/profile_compat.c
)

set(ASX_CHANNEL_SRC
    src/channel/mpsc.c
)

set(ASX_TIME_SRC
    src/time/timer_wheel.c
)

set(ASX_PLATFORM_SRC)

# Select platform sources by profile
if(ASX_PROFILE STREQUAL "POSIX")
    list(APPEND ASX_PLATFORM_SRC src/platform/posix/hooks.c)
elseif(ASX_PROFILE STREQUAL "WIN32")
    list(APPEND ASX_PLATFORM_SRC src/platform/win32/hooks.c)
elseif(ASX_PROFILE STREQUAL "FREESTANDING" OR ASX_PROFILE STREQUAL "EMBEDDED_ROUTER")
    list(APPEND ASX_PLATFORM_SRC src/platform/freestanding/hooks.c)
endif()

add_library(asx STATIC
    ${ASX_CORE_SRC}
    ${ASX_RUNTIME_SRC}
    ${ASX_CHANNEL_SRC}
    ${ASX_TIME_SRC}
    ${ASX_PLATFORM_SRC}
)

target_include_directories(asx
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(ASX_BUILD_TESTS)
    enable_testing()

    # Unit tests
    file(GLOB_RECURSE UNIT_TESTS CONFIGURE_DEPENDS
        tests/unit/**/*_test.c
        tests/unit/**/test_*.c
    )
    list(REMOVE_DUPLICATES UNIT_TESTS)
    foreach(TEST_SRC ${UNIT_TESTS})
        get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
        add_executable(${TEST_NAME} ${TEST_SRC})
        target_link_libraries(${TEST_NAME} PRIVATE asx)
        target_include_directories(${TEST_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
        )
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endforeach()

    # Invariant tests
    file(GLOB_RECURSE INV_TESTS CONFIGURE_DEPENDS
        tests/invariant/**/*_test.c
        tests/invariant/**/test_*.c
    )
    list(REMOVE_DUPLICATES INV_TESTS)
    foreach(TEST_SRC ${INV_TESTS})
        get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
        add_executable(${TEST_NAME} ${TEST_SRC})
        target_link_libraries(${TEST_NAME} PRIVATE asx)
        target_include_directories(${TEST_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
        )
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Delegate advanced quality gates to the authoritative Makefile.
# ---------------------------------------------------------------------------
find_program(ASX_MAKE_EXECUTABLE NAMES gmake make)
if(ASX_MAKE_EXECUTABLE)
    add_custom_target(gate-conformance
        COMMAND ${ASX_MAKE_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR} conformance
        USES_TERMINAL
        COMMENT "Delegating conformance gate to Makefile")

    add_custom_target(gate-codec-equivalence
        COMMAND ${ASX_MAKE_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR} codec-equivalence
        USES_TERMINAL
        COMMENT "Delegating codec-equivalence gate to Makefile")

    add_custom_target(gate-profile-parity
        COMMAND ${ASX_MAKE_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR} profile-parity
        USES_TERMINAL
        COMMENT "Delegating profile-parity gate to Makefile")

    add_custom_target(gate-fuzz-smoke
        COMMAND ${ASX_MAKE_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR} fuzz-smoke
        USES_TERMINAL
        COMMENT "Delegating fuzz-smoke gate to Makefile")

    add_custom_target(gate-ci-embedded-matrix
        COMMAND ${ASX_MAKE_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR} ci-embedded-matrix
        USES_TERMINAL
        COMMENT "Delegating embedded matrix gate to Makefile")
else()
    message(WARNING "asx: make executable not found; gate-* delegation targets disabled.")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS asx
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/asx
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)
