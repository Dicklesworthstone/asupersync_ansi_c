/*
 * test_db.c — unit tests for database client abstraction
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/net/db.h>
#include <string.h>

TEST(db_pool_init) {
    asx_db_pool pool;
    asx_db_pool_init(&pool);
    ASSERT_EQ(pool.next_id, 1u);
}

TEST(db_connect_and_close) {
    asx_db_pool pool;
    asx_db_conn *conn;

    asx_db_pool_init(&pool);
    ASSERT_EQ(asx_db_connect(&pool, "mem://test", &conn), ASX_OK);
    ASSERT_TRUE(conn != NULL);
    ASSERT_TRUE(conn->alive);
    ASSERT_EQ(conn->state, ASX_DB_CONN_ACTIVE);
    ASSERT_STR_EQ(conn->dsn, "mem://test");

    ASSERT_EQ(asx_db_close(conn), ASX_OK);
    ASSERT_FALSE(conn->alive);
    ASSERT_EQ(conn->state, ASX_DB_CONN_CLOSED);
}

TEST(db_connect_rejects_oversized_dsn) {
    asx_db_pool pool;
    asx_db_conn *conn = NULL;
    asx_db_conn *ok_conn = NULL;
    char dsn[ASX_DB_DSN_MAX + 8u];

    memset(dsn, 'a', sizeof(dsn) - 1u);
    dsn[sizeof(dsn) - 1u] = '\0';

    asx_db_pool_init(&pool);
    ASSERT_EQ(asx_db_connect(&pool, dsn, &conn), ASX_E_BUFFER_TOO_SMALL);
    ASSERT_TRUE(conn == NULL);
    ASSERT_EQ(asx_db_connect(&pool, "mem://ok", &ok_conn), ASX_OK);
    ASSERT_TRUE(ok_conn != NULL);
    ASSERT_EQ(asx_db_close(ok_conn), ASX_OK);
}

TEST(db_connect_exhaustion) {
    asx_db_pool pool;
    asx_db_conn *conns[ASX_DB_MAX_CONNECTIONS + 1];
    uint32_t i;

    asx_db_pool_init(&pool);
    for (i = 0; i < ASX_DB_MAX_CONNECTIONS; i++) {
        ASSERT_EQ(asx_db_connect(&pool, "mem://db", &conns[i]), ASX_OK);
    }
    ASSERT_EQ(asx_db_connect(&pool, "mem://db", &conns[ASX_DB_MAX_CONNECTIONS]),
              ASX_E_RESOURCE_EXHAUSTED);

    for (i = 0; i < ASX_DB_MAX_CONNECTIONS; i++) {
        asx_db_close(conns[i]);
    }
}

TEST(db_execute_query) {
    asx_db_pool pool;
    asx_db_conn *conn;
    asx_db_result result;

    asx_db_pool_init(&pool);
    asx_db_connect(&pool, "mem://test", &conn);
    ASSERT_EQ(asx_db_execute(conn, "SELECT 1", &result), ASX_OK);
    ASSERT_EQ(conn->queries_executed, 1u);
    ASSERT_EQ(result.row_count, 0u); /* deterministic: empty result */
    asx_db_close(conn);
}

TEST(db_transaction_lifecycle) {
    asx_db_pool pool;
    asx_db_conn *conn;

    asx_db_pool_init(&pool);
    asx_db_connect(&pool, "mem://test", &conn);

    ASSERT_EQ(asx_db_begin(conn), ASX_OK);
    ASSERT_EQ(conn->state, ASX_DB_CONN_IN_TX);

    /* Can't begin again while in TX */
    ASSERT_EQ(asx_db_begin(conn), ASX_E_INVALID_STATE);

    ASSERT_EQ(asx_db_commit(conn), ASX_OK);
    ASSERT_EQ(conn->state, ASX_DB_CONN_ACTIVE);

    /* Rollback test */
    ASSERT_EQ(asx_db_begin(conn), ASX_OK);
    ASSERT_EQ(asx_db_rollback(conn), ASX_OK);
    ASSERT_EQ(conn->state, ASX_DB_CONN_ACTIVE);

    /* Can't commit when not in TX */
    ASSERT_EQ(asx_db_commit(conn), ASX_E_INVALID_STATE);

    asx_db_close(conn);
}

TEST(db_result_navigation) {
    asx_db_result result;
    asx_db_row row1, row2;
    const asx_db_row *r;

    asx_db_result_init(&result);
    ASSERT_EQ(asx_db_result_add_column(&result, "id"), ASX_OK);
    ASSERT_EQ(asx_db_result_add_column(&result, "name"), ASX_OK);
    ASSERT_EQ(result.column_count, 2u);

    memset(&row1, 0, sizeof(row1));
    row1.cells[0] = asx_db_value_int(1);
    row1.cells[1] = asx_db_value_text("alice");
    row1.cell_count = 2;
    ASSERT_EQ(asx_db_result_add_row(&result, &row1), ASX_OK);

    memset(&row2, 0, sizeof(row2));
    row2.cells[0] = asx_db_value_int(2);
    row2.cells[1] = asx_db_value_text("bob");
    row2.cell_count = 2;
    ASSERT_EQ(asx_db_result_add_row(&result, &row2), ASX_OK);

    ASSERT_EQ(result.row_count, 2u);

    r = asx_db_result_next(&result);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ(r->cells[0].int_val, 1);
    ASSERT_STR_EQ(r->cells[1].text_val, "alice");

    r = asx_db_result_next(&result);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ(r->cells[0].int_val, 2);

    r = asx_db_result_next(&result);
    ASSERT_TRUE(r == NULL);

    asx_db_result_reset_cursor(&result);
    r = asx_db_result_next(&result);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ(r->cells[0].int_val, 1);
}

TEST(db_column_index) {
    asx_db_result result;
    asx_db_result_init(&result);
    asx_db_result_add_column(&result, "id");
    asx_db_result_add_column(&result, "name");

    ASSERT_EQ(asx_db_result_column_index(&result, "id"), 0);
    ASSERT_EQ(asx_db_result_column_index(&result, "name"), 1);
    ASSERT_EQ(asx_db_result_column_index(&result, "missing"), -1);
}

TEST(db_value_types) {
    asx_db_value v;
    char long_text[ASX_DB_CELL_MAX + 8u];

    v = asx_db_value_null();
    ASSERT_EQ(v.type, ASX_DB_TYPE_NULL);

    v = asx_db_value_int(42);
    ASSERT_EQ(v.type, ASX_DB_TYPE_INT);
    ASSERT_EQ(v.int_val, 42);

    v = asx_db_value_text("hello");
    ASSERT_EQ(v.type, ASX_DB_TYPE_TEXT);
    ASSERT_STR_EQ(v.text_val, "hello");
    ASSERT_EQ(v.text_len, 5u);

    memset(long_text, 'z', sizeof(long_text) - 1u);
    long_text[sizeof(long_text) - 1u] = '\0';
    v = asx_db_value_text(long_text);
    ASSERT_EQ(v.type, ASX_DB_TYPE_TEXT);
    ASSERT_EQ(v.text_len, ASX_DB_CELL_MAX - 1u);
    ASSERT_EQ(v.text_val[ASX_DB_CELL_MAX - 1u], '\0');
    ASSERT_EQ(v.text_val[0], 'z');

    v = asx_db_value_float(3.14);
    ASSERT_EQ(v.type, ASX_DB_TYPE_FLOAT);
    ASSERT_TRUE(v.float_val > 3.13 && v.float_val < 3.15);
}

TEST(db_null_args) {
    asx_db_pool pool;
    asx_db_conn *conn;
    asx_db_result result;

    asx_db_pool_init(&pool);
    ASSERT_EQ(asx_db_connect(NULL, "dsn", &conn), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_db_connect(&pool, NULL, &conn), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_db_close(NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_db_execute(NULL, "q", &result), ASX_E_INVALID_ARGUMENT);
}

int main(void) {
    fprintf(stderr, "=== db tests ===\n");
    RUN_TEST(db_pool_init);
    RUN_TEST(db_connect_and_close);
    RUN_TEST(db_connect_rejects_oversized_dsn);
    RUN_TEST(db_connect_exhaustion);
    RUN_TEST(db_execute_query);
    RUN_TEST(db_transaction_lifecycle);
    RUN_TEST(db_result_navigation);
    RUN_TEST(db_column_index);
    RUN_TEST(db_value_types);
    RUN_TEST(db_null_args);
    TEST_REPORT();
    return test_failures;
}
