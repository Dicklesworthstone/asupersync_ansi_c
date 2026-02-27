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
)

set(ASX_RUNTIME_SRC)
set(ASX_CHANNEL_SRC)
set(ASX_TIME_SRC)
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
    file(GLOB_RECURSE UNIT_TESTS tests/unit/**/*_test.c)
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
    file(GLOB_RECURSE INV_TESTS tests/invariant/**/*_test.c)
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
