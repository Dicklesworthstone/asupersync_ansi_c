# =============================================================================
# asx — asupersync ANSI C runtime
# Optional CMake generator (IDE/toolchain integration)
#
# The primary build system is the Makefile. This CMakeLists.txt is provided
# for IDE integration, consumers who require CMake, and the MSVC lane (MSVC
# cannot run the Makefile's GCC-style rules). It builds the Makefile's
# library (LIB_SRC), profile/codec flags and warning policy.
#
# SPDX-License-Identifier: MIT
# =============================================================================

cmake_minimum_required(VERSION 3.10)
project(asx VERSION 0.1.0 LANGUAGES C)

# C99, as the Makefile builds it. MSVC has no C99 mode: its C11 mode is the
# nearest that accepts the code.
if(MSVC)
    set(CMAKE_C_STANDARD 11)
else()
    set(CMAKE_C_STANDARD 99)
endif()
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
    # /wd4100 (unreferenced parameter) mirrors GCC's -Wno-unused-parameter.
    add_compile_options(/W4 /WX /wd4100)
    # The CRT's "use the _s variant" deprecations (C4996) are not about the
    # code's correctness; every other /W4 warning stays an error.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
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
# Library sources: every C file under src/ except the platform adapters
# (selected by profile below), the *_spike.c evaluation harnesses (linked
# only into their own tests) and the CLI's entry point, src/cli/main.c.
# That is exactly the Makefile's LIB_SRC.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE ASX_LIB_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
list(FILTER ASX_LIB_SRC EXCLUDE REGEX "/src/platform/")
list(FILTER ASX_LIB_SRC EXCLUDE REGEX "_spike\\.c$")
list(FILTER ASX_LIB_SRC EXCLUDE REGEX "/src/cli/main\\.c$")

set(ASX_PLATFORM_SRC)

# Select platform sources by profile
if(ASX_PROFILE STREQUAL "POSIX")
    list(APPEND ASX_PLATFORM_SRC src/platform/posix/hooks.c src/platform/posix/net_posix.c
        src/platform/posix/fs_posix.c src/platform/posix/process_posix.c
        src/platform/posix/signal_posix.c)
elseif(ASX_PROFILE STREQUAL "WIN32")
    list(APPEND ASX_PLATFORM_SRC src/platform/win32/hooks.c)
elseif(ASX_PROFILE STREQUAL "FREESTANDING" OR ASX_PROFILE STREQUAL "EMBEDDED_ROUTER")
    list(APPEND ASX_PLATFORM_SRC src/platform/freestanding/hooks.c)
endif()

add_library(asx STATIC
    ${ASX_LIB_SRC}
    ${ASX_PLATFORM_SRC}
)
if(ASX_PROFILE STREQUAL "WIN32")
    target_link_libraries(asx PUBLIC bcrypt)
elseif(ASX_PROFILE STREQUAL "POSIX")
    # The live POSIX runtime runs its reactor and blocking pool on threads.
    find_package(Threads REQUIRED)
    target_link_libraries(asx PUBLIC Threads::Threads)
endif()

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
    # The spike harnesses each test links in (as the Makefile's rules do).
    set(ASX_TEST_EXTRA_test_arena_locality src/runtime/arena_locality_spike.c)
    set(ASX_TEST_EXTRA_test_barrier_cert src/runtime/barrier_cert_spike.c)
    set(ASX_TEST_EXTRA_test_seqlock_ebr src/runtime/seqlock_ebr_spike.c)
    foreach(TEST_SRC ${UNIT_TESTS})
        # Basenames repeat across directories (core/test_hooks.c,
        # runtime/test_hooks.c): the target is named after the path.
        get_filename_component(TEST_BASE ${TEST_SRC} NAME_WE)
        file(RELATIVE_PATH TEST_REL ${CMAKE_CURRENT_SOURCE_DIR}/tests ${TEST_SRC})
        string(REGEX REPLACE "\\.c$" "" TEST_NAME ${TEST_REL})
        string(REPLACE "/" "_" TEST_NAME ${TEST_NAME})
        add_executable(${TEST_NAME} ${TEST_SRC} ${ASX_TEST_EXTRA_${TEST_BASE}})
        target_link_libraries(${TEST_NAME} PRIVATE asx)
        target_include_directories(${TEST_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        # From the repository root, as `make test-unit` runs them (some
        # read fixtures by relative path).
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Invariant tests
    file(GLOB_RECURSE INV_TESTS CONFIGURE_DEPENDS
        tests/invariant/**/*_test.c
        tests/invariant/**/test_*.c
    )
    list(REMOVE_DUPLICATES INV_TESTS)
    foreach(TEST_SRC ${INV_TESTS})
        file(RELATIVE_PATH TEST_REL ${CMAKE_CURRENT_SOURCE_DIR}/tests ${TEST_SRC})
        string(REGEX REPLACE "\\.c$" "" TEST_NAME ${TEST_REL})
        string(REPLACE "/" "_" TEST_NAME ${TEST_NAME})
        add_executable(${TEST_NAME} ${TEST_SRC})
        target_link_libraries(${TEST_NAME} PRIVATE asx)
        target_include_directories(${TEST_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
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
