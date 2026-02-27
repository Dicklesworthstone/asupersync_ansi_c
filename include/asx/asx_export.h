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
  #ifdef ASX_BUILDING_DLL
    #define ASX_API __declspec(dllexport)
  #else
    #define ASX_API __declspec(dllimport)
  #endif
#elif defined(__GNUC__) && __GNUC__ >= 4
  #define ASX_API __attribute__((visibility("default")))
#else
  #define ASX_API
#endif

#endif /* ASX_EXPORT_H */
