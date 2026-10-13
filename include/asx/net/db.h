/*
 * asx/net/db.h — in-memory database client model for lab scenarios
 *
 * MODEL-ONLY (DEC-010). This is a deterministic in-memory model. It is not a
 * database client: no driver, no wire protocol and no storage. A connection
 * is a slot in a pool; a statement is refused (the model implements none); a
 * success status means the in-process model accepted the call, not that a
 * database did anything. The
 * SQLite, Postgres and MySQL backend flags are 0 and selecting those
 * backends is refused.
 *
 * It gives lab scenarios the shape of the API: connection pool, result-set
 * building and iteration, and a transaction state machine.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_DB_H
#define ASX_NET_DB_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if ASX_HAS_DATABASE_SURFACE
/* -------------------------------------------------------------------
 * Database configuration and types
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_DB_TYPE_NULL = 0,
    ASX_DB_TYPE_INT = 1,
    ASX_DB_TYPE_TEXT = 2,
    ASX_DB_TYPE_BLOB = 3,
    ASX_DB_TYPE_FLOAT = 4
} asx_db_type;

typedef enum {
    ASX_DB_CONN_IDLE = 0,
    ASX_DB_CONN_ACTIVE = 1,
    ASX_DB_CONN_IN_TX = 2,
    ASX_DB_CONN_CLOSED = 3
} asx_db_conn_state;

typedef enum {
    ASX_DB_BACKEND_MEMORY = 0,
    ASX_DB_BACKEND_SQLITE = 1,
    ASX_DB_BACKEND_POSTGRES = 2,
    ASX_DB_BACKEND_MYSQL = 3
} asx_db_backend;

/* -------------------------------------------------------------------
 * Database value (cell)
 * ------------------------------------------------------------------- */

typedef struct {
    asx_db_type type;
    int64_t int_val;
    double float_val;
    char text_val[ASX_DB_CELL_MAX];
    uint32_t text_len;
} asx_db_value;

/* -------------------------------------------------------------------
 * Result row and result set
 * ------------------------------------------------------------------- */

typedef struct {
    asx_db_value cells[ASX_DB_MAX_COLUMNS];
    uint32_t cell_count;
} asx_db_row;

typedef struct {
    char columns[ASX_DB_MAX_COLUMNS][ASX_DB_COLUMN_NAME_MAX];
    uint32_t column_count;
    asx_db_row rows[ASX_DB_MAX_ROWS];
    uint32_t row_count;
    uint32_t cursor;
    uint64_t rows_affected;
} asx_db_result;

/* -------------------------------------------------------------------
 * Database connection
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t id;
    uint32_t generation;
    asx_db_conn_state state;
    char dsn[ASX_DB_DSN_MAX];
    uint32_t queries_executed; /* statements run: always 0, the model runs none */
    uint8_t alive;
} asx_db_conn;

typedef struct {
    asx_db_backend backend;
} asx_db_config;

typedef struct {
    asx_db_config config;
    asx_db_conn connections[ASX_DB_MAX_CONNECTIONS];
    uint32_t next_id;
} asx_db_pool;

/* -------------------------------------------------------------------
 * Database API
 * ------------------------------------------------------------------- */

/* Initialize database connection pool. */
ASX_API void asx_db_pool_init(asx_db_pool *pool);

/* Initialize database configuration to deterministic defaults. */
ASX_API void asx_db_config_init(asx_db_config *cfg);

/* Get a human-readable backend name. */
ASX_API ASX_MUST_USE const char *asx_db_backend_str(asx_db_backend backend);

/* Select the backend to use for new pool connections. */
ASX_API asx_status asx_db_pool_set_backend(asx_db_pool *pool, asx_db_backend backend);

/* Open a model connection. Only "mem://..." DSNs are accepted: a DSN that
 * names a database (postgres://, mysql://, a file) would succeed and never
 * reach it, so it is refused with ASX_E_PERMISSION_DENIED, as selecting that
 * backend is. ASX_E_BUFFER_TOO_SMALL for a DSN of ASX_DB_DSN_MAX or more. */
ASX_API asx_status asx_db_connect(asx_db_pool *pool, const char *dsn, asx_db_conn **out);

/* Close a connection. */
ASX_API asx_status asx_db_close(asx_db_conn *conn);

/* Refuse the statement. The model implements none: it has no tables, no parser
 * and no storage, so no statement has a meaning here, and answering OK with an
 * empty result would read as "ran, found nothing" (an INSERT would look like
 * it inserted). Returns ASX_E_PERMISSION_DENIED for any non-empty statement,
 * as selecting a backend the model lacks does, with *result left empty and
 * nothing counted in queries_executed. ASX_E_INVALID_ARGUMENT for NULL
 * arguments, a closed connection or an empty statement. A lab scenario builds
 * the result sets it needs with asx_db_result_add_row. */
ASX_API asx_status asx_db_execute(asx_db_conn *conn, const char *query, asx_db_result *result);

/* Begin a transaction. */
ASX_API asx_status asx_db_begin(asx_db_conn *conn);

/* Commit a transaction. */
ASX_API asx_status asx_db_commit(asx_db_conn *conn);

/* Rollback a transaction. */
ASX_API asx_status asx_db_rollback(asx_db_conn *conn);

/* -------------------------------------------------------------------
 * Result set navigation
 * ------------------------------------------------------------------- */

/* Initialize an empty result. */
ASX_API void asx_db_result_init(asx_db_result *result);

/* Add a column to the result schema. */
ASX_API asx_status asx_db_result_add_column(asx_db_result *result, const char *name);

/* Add a row to the result. */
ASX_API asx_status asx_db_result_add_row(asx_db_result *result, const asx_db_row *row);

/* Get the next row. Returns NULL when exhausted. */
ASX_API const asx_db_row *asx_db_result_next(asx_db_result *result);

/* Reset the cursor to the beginning. */
ASX_API void asx_db_result_reset_cursor(asx_db_result *result);

/* Get column index by name. Returns -1 if not found. */
ASX_API int asx_db_result_column_index(const asx_db_result *result, const char *name);

/* -------------------------------------------------------------------
 * Value constructors
 * ------------------------------------------------------------------- */

/* Get a value as a 64-bit integer. */
ASX_API asx_db_value asx_db_value_null(void);
/* Get a value as a string pointer. */
ASX_API asx_db_value asx_db_value_int(int64_t val);
/* Get a value as a double. */
ASX_API asx_db_value asx_db_value_text(const char *text);
ASX_API asx_db_value asx_db_value_float(double val);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_DB_H */
