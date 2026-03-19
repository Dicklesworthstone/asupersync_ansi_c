/*
 * asx/net/db.h — database client abstraction
 *
 * Provides a portable database client surface: connection pool, prepared
 * statements, query execution, result set iteration, and transaction
 * management. In the deterministic core, all operations use in-memory
 * row storage with no real database backend.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_DB_H
#define ASX_NET_DB_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Database configuration and types
 * ------------------------------------------------------------------- */

#ifndef ASX_DB_MAX_CONNECTIONS
#define ASX_DB_MAX_CONNECTIONS 4u
#endif

#ifndef ASX_DB_MAX_COLUMNS
#define ASX_DB_MAX_COLUMNS 16u
#endif

#ifndef ASX_DB_MAX_ROWS
#define ASX_DB_MAX_ROWS 64u
#endif

#ifndef ASX_DB_COLUMN_NAME_MAX
#define ASX_DB_COLUMN_NAME_MAX 32u
#endif

#ifndef ASX_DB_CELL_MAX
#define ASX_DB_CELL_MAX 128u
#endif

#ifndef ASX_DB_QUERY_MAX
#define ASX_DB_QUERY_MAX 512u
#endif

#ifndef ASX_DB_DSN_MAX
#define ASX_DB_DSN_MAX 128u
#endif

typedef enum {
    ASX_DB_TYPE_NULL    = 0,
    ASX_DB_TYPE_INT     = 1,
    ASX_DB_TYPE_TEXT    = 2,
    ASX_DB_TYPE_BLOB    = 3,
    ASX_DB_TYPE_FLOAT   = 4
} asx_db_type;

typedef enum {
    ASX_DB_CONN_IDLE    = 0,
    ASX_DB_CONN_ACTIVE  = 1,
    ASX_DB_CONN_IN_TX   = 2,
    ASX_DB_CONN_CLOSED  = 3
} asx_db_conn_state;

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
    uint32_t queries_executed;
    uint8_t alive;
} asx_db_conn;

typedef struct {
    asx_db_conn connections[ASX_DB_MAX_CONNECTIONS];
    uint32_t next_id;
} asx_db_pool;

/* -------------------------------------------------------------------
 * Database API
 * ------------------------------------------------------------------- */

/* Initialize database connection pool. */
ASX_API void asx_db_pool_init(asx_db_pool *pool);

/* Open a connection. */
ASX_API asx_status asx_db_connect(asx_db_pool *pool, const char *dsn, asx_db_conn **out);

/* Close a connection. */
ASX_API asx_status asx_db_close(asx_db_conn *conn);

/* Execute a query. Result set is populated with in-memory deterministic data. */
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

ASX_API asx_db_value asx_db_value_null(void);
ASX_API asx_db_value asx_db_value_int(int64_t val);
ASX_API asx_db_value asx_db_value_text(const char *text);
ASX_API asx_db_value asx_db_value_float(double val);

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_DB_H */
