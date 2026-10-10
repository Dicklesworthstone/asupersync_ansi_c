/*
 * asx_export.h — symbol visibility macros
 *
 * Include this in any public header that declares ASX_API functions.
 * The umbrella asx.h also includes this, so consumers who include
 * asx.h get it automatically.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_EXPORT_H
#define ASX_EXPORT_H

#if defined(_WIN32) || defined(__CYGWIN__)
/* The library builds static by default: no decoration. A DLL build
 * defines ASX_BUILDING_DLL, and its consumers ASX_USING_DLL. */
#if defined(ASX_BUILDING_DLL)
#define ASX_API __declspec(dllexport)
#elif defined(ASX_USING_DLL)
#define ASX_API __declspec(dllimport)
#else
#define ASX_API
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define ASX_API __attribute__((visibility("default")))
#else
#define ASX_API
#endif

/* Result-use annotation for status-bearing APIs.
 * C99/C11: use GCC/Clang __attribute__; C23+: use [[nodiscard]]. */
#ifndef ASX_MUST_USE
#if defined(__cplusplus) && __cplusplus >= 201703L
#define ASX_MUST_USE [[nodiscard]]
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L && !defined(__CPPCHECK__)
/* cppcheck explores this branch while parsing as C99 (--std=c99), where
 * [[nodiscard]] is a syntax error; it never applies to a C99 lint run. */
#define ASX_MUST_USE [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
#define ASX_MUST_USE __attribute__((warn_unused_result))
#elif defined(_MSC_VER) && defined(_Check_return_)
#define ASX_MUST_USE _Check_return_
#else
#define ASX_MUST_USE
#endif
#endif

#endif /* ASX_EXPORT_H */
