/*
 * db.c — deterministic database client abstraction
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/db.h>
#include <string.h>

static size_t db_bounded_strlen(const char *str, size_t max) {
    size_t len;
    if (str == NULL) return 0u;
    for (len = 0u; len < max; len++) {
        if (str[len] == '\0') return len;
    }
    return max;
}

static uint32_t db_next_gen(uint32_t g) {
    g++;
    return g == 0u ? 1u : g;
}

/* ------------------------------------------------------------------ */
/* Connection pool                                                     */
/* ------------------------------------------------------------------ */

void asx_db_pool_init(asx_db_pool *pool) {
    if (pool == NULL) return;
    memset(pool, 0, sizeof(*pool));
    pool->next_id = 1u;
}

asx_status asx_db_connect(asx_db_pool *pool, const char *dsn, asx_db_conn **out) {
    uint32_t i;
    size_t len;
    asx_db_conn *c;

    if (pool == NULL || dsn == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    *out = NULL;
    len = db_bounded_strlen(dsn, ASX_DB_DSN_MAX);
    if (len >= ASX_DB_DSN_MAX) return ASX_E_BUFFER_TOO_SMALL;

    for (i = 0u; i < ASX_DB_MAX_CONNECTIONS; i++) {
        if (!pool->connections[i].alive) break;
    }
    if (i >= ASX_DB_MAX_CONNECTIONS) return ASX_E_RESOURCE_EXHAUSTED;

    c = &pool->connections[i];
    memset(c, 0, sizeof(*c));
    c->id = pool->next_id++;
    c->generation = db_next_gen(c->generation);
    c->state = ASX_DB_CONN_ACTIVE;
    c->alive = 1u;

    memcpy(c->dsn, dsn, len + 1u);

    *out = c;
    return ASX_OK;
}

asx_status asx_db_close(asx_db_conn *conn) {
    if (conn == NULL || !conn->alive) return ASX_E_INVALID_ARGUMENT;
    conn->state = ASX_DB_CONN_CLOSED;
    conn->alive = 0u;
    return ASX_OK;
}

asx_status asx_db_execute(asx_db_conn *conn, const char *query, asx_db_result *result) {
    if (conn == NULL || !conn->alive || query == NULL || result == NULL)
        return ASX_E_INVALID_ARGUMENT;
    if (conn->state == ASX_DB_CONN_CLOSED) return ASX_E_INVALID_STATE;

    asx_db_result_init(result);
    conn->queries_executed++;
    /* Deterministic: return empty result set for any query */
    return ASX_OK;
}

asx_status asx_db_begin(asx_db_conn *conn) {
    if (conn == NULL || !conn->alive) return ASX_E_INVALID_ARGUMENT;
    if (conn->state != ASX_DB_CONN_ACTIVE) return ASX_E_INVALID_STATE;
    conn->state = ASX_DB_CONN_IN_TX;
    return ASX_OK;
}

asx_status asx_db_commit(asx_db_conn *conn) {
    if (conn == NULL || !conn->alive) return ASX_E_INVALID_ARGUMENT;
    if (conn->state != ASX_DB_CONN_IN_TX) return ASX_E_INVALID_STATE;
    conn->state = ASX_DB_CONN_ACTIVE;
    return ASX_OK;
}

asx_status asx_db_rollback(asx_db_conn *conn) {
    if (conn == NULL || !conn->alive) return ASX_E_INVALID_ARGUMENT;
    if (conn->state != ASX_DB_CONN_IN_TX) return ASX_E_INVALID_STATE;
    conn->state = ASX_DB_CONN_ACTIVE;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Result set                                                          */
/* ------------------------------------------------------------------ */

void asx_db_result_init(asx_db_result *result) {
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
}

asx_status asx_db_result_add_column(asx_db_result *result, const char *name) {
    size_t len;

    if (result == NULL || name == NULL) return ASX_E_INVALID_ARGUMENT;
    if (result->column_count >= ASX_DB_MAX_COLUMNS) return ASX_E_RESOURCE_EXHAUSTED;
    len = db_bounded_strlen(name, ASX_DB_COLUMN_NAME_MAX);
    if (len == 0u || len >= ASX_DB_COLUMN_NAME_MAX) return ASX_E_INVALID_ARGUMENT;
    memcpy(result->columns[result->column_count], name, len + 1u);
    result->column_count++;
    return ASX_OK;
}

asx_status asx_db_result_add_row(asx_db_result *result, const asx_db_row *row) {
    if (result == NULL || row == NULL) return ASX_E_INVALID_ARGUMENT;
    if (result->row_count >= ASX_DB_MAX_ROWS) return ASX_E_RESOURCE_EXHAUSTED;
    result->rows[result->row_count] = *row;
    result->row_count++;
    return ASX_OK;
}

const asx_db_row *asx_db_result_next(asx_db_result *result) {
    if (result == NULL || result->cursor >= result->row_count) return NULL;
    return &result->rows[result->cursor++];
}

void asx_db_result_reset_cursor(asx_db_result *result) {
    if (result == NULL) return;
    result->cursor = 0u;
}

int asx_db_result_column_index(const asx_db_result *result, const char *name) {
    uint32_t i;

    if (result == NULL || name == NULL) return -1;
    for (i = 0u; i < result->column_count; i++) {
        if (strcmp(result->columns[i], name) == 0) return (int)i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Value constructors                                                  */
/* ------------------------------------------------------------------ */

asx_db_value asx_db_value_null(void) {
    asx_db_value v;
    memset(&v, 0, sizeof(v));
    v.type = ASX_DB_TYPE_NULL;
    return v;
}

asx_db_value asx_db_value_int(int64_t val) {
    asx_db_value v;
    memset(&v, 0, sizeof(v));
    v.type = ASX_DB_TYPE_INT;
    v.int_val = val;
    return v;
}

asx_db_value asx_db_value_text(const char *text) {
    asx_db_value v;
    size_t len;

    memset(&v, 0, sizeof(v));
    v.type = ASX_DB_TYPE_TEXT;
    if (text != NULL) {
        len = db_bounded_strlen(text, ASX_DB_CELL_MAX);
        if (len >= ASX_DB_CELL_MAX) len = ASX_DB_CELL_MAX - 1u;
        memcpy(v.text_val, text, len);
        v.text_val[len] = '\0';
        v.text_len = (uint32_t)len;
    }
    return v;
}

asx_db_value asx_db_value_float(double val) {
    asx_db_value v;
    memset(&v, 0, sizeof(v));
    v.type = ASX_DB_TYPE_FLOAT;
    v.float_val = val;
    return v;
}
