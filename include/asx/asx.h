/*
 * asx.h — asupersync ANSI C runtime: umbrella public header
 *
 * Usage:
 *   #include <asx/asx.h>
 *
 * This is the single public entry point for the asx runtime API.
 * Include this header to access all public types and functions.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_ASX_H
#define ASX_ASX_H

/* Version information */
#define ASX_API_VERSION_MAJOR 0
#define ASX_API_VERSION_MINOR 1
#define ASX_API_VERSION_PATCH 0

/* Symbol visibility (provides ASX_API) */
#include <asx/asx_export.h>

/* Configuration and profile selection */
#include <asx/asx_config.h>

/* Status and error codes */
#include <asx/asx_status.h>

/* Handle types and packing helpers */
#include <asx/asx_ids.h>

/* Core semantic types */
#include <asx/core/budget.h>
#include <asx/core/outcome.h>
#include <asx/core/cancel.h>
#include <asx/core/transition.h>

#endif /* ASX_ASX_H */
