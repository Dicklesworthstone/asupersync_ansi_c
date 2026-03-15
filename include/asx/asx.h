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

/* ABI stability contract and versioning */
#include <asx/asx_abi.h>

/* Configuration and profile selection */
#include <asx/asx_config.h>

/* Status and error codes */
#include <asx/asx_status.h>

/* Handle types and packing helpers */
#include <asx/asx_ids.h>

/* Capability context and structured concurrency */
#include <asx/cx/cx.h>
#include <asx/cx/scope.h>

/* Core semantic types */
#include <asx/core/symbol.h>
#include <asx/core/budget.h>
#include <asx/core/cancel.h>
#include <asx/core/channel.h>
#include <asx/core/cleanup.h>
#include <asx/core/ghost.h>
#include <asx/core/outcome.h>
#include <asx/core/resource.h>
#include <asx/core/transition.h>

/* Codec abstraction and canonical fixture schema */
#include <asx/codec/codec.h>
#include <asx/codec/equivalence.h>
#include <asx/codec/schema.h>

/* Security and audit surface */
#include <asx/security/audit.h>
#include <asx/security/security.h>

/* Network surface */
#include <asx/net/net.h>

/* Obligation, session, record, and link public families */
#include <asx/link/link.h>
#include <asx/obligation/obligation.h>
#include <asx/record/record.h>
#include <asx/session/session.h>

/* Observability, evidence, and monitor families */
#include <asx/evidence/evidence.h>
#include <asx/evidence_sink/evidence_sink.h>
#include <asx/monitor/monitor.h>
#include <asx/observability/observability.h>

/* Operator and testing support */
#include <asx/app/app.h>
#include <asx/app/doctor.h>
#include <asx/app/report.h>
#include <asx/console/console.h>
#include <asx/tracing_compat/tracing_compat.h>

/* Runtime (walking skeleton — bd-ix8.8) */
#include <asx/runtime/adapter.h>
#include <asx/runtime/automotive_instrument.h>
#include <asx/runtime/blocking.h>
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/browser_diagnostic.h>
#include <asx/runtime/builder.h>
#include <asx/runtime/config_reload.h>
#include <asx/runtime/diagnostic.h>
#include <asx/runtime/event.h>
#include <asx/runtime/hft_instrument.h>
#include <asx/runtime/hindsight.h>
#include <asx/runtime/io_driver.h>
#include <asx/runtime/lab.h>
#include <asx/runtime/overload_catalog.h>
#include <asx/runtime/parallel.h>
#include <asx/runtime/profile_compat.h>
#include <asx/runtime/regression_localize.h>
#include <asx/runtime/replay.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/snapshot.h>
#include <asx/runtime/telemetry.h>
#include <asx/runtime/trace.h>
#include <asx/runtime/vertical_adapter.h>
#include <asx/runtime/virtual_time.h>
#include <asx/runtime/waker.h>

#endif /* ASX_ASX_H */
