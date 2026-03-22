/*
 * test_messaging.c — unit tests for messaging and broker client
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/net/messaging.h>
#include <asx/runtime/browser_boundary.h>
#include <string.h>

static int messaging_surface_available(void) {
    return asx_surface_available_active(ASX_SURFACE_MESSAGING);
}

TEST(message_set) {
    asx_message msg;
    asx_message_init(&msg);
    ASSERT_EQ(asx_message_set(&msg, "events", "hello", 5), ASX_OK);
    ASSERT_STR_EQ(msg.topic, "events");
    ASSERT_EQ(msg.payload_len, 5u);
    ASSERT_TRUE(memcmp(msg.payload, "hello", 5) == 0);
}

TEST(message_null_args) {
    asx_message msg;
    ASSERT_EQ(asx_message_set(NULL, "t", "d", 1), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_message_set(&msg, NULL, "d", 1), ASX_E_INVALID_ARGUMENT);
}

TEST(message_null_payload_with_len_fails) {
    asx_message msg;
    asx_message_init(&msg);
    ASSERT_EQ(asx_message_set(&msg, "events", NULL, 3u), ASX_E_INVALID_ARGUMENT);
}

TEST(queue_push_poll) {
    asx_msg_queue q;
    asx_message in, out;

    asx_msg_queue_init(&q);
    ASSERT_TRUE(asx_msg_queue_is_empty(&q));

    asx_message_init(&in);
    asx_message_set(&in, "test", "data1", 5);
    if (!messaging_surface_available()) {
        ASSERT_EQ(asx_msg_queue_push(&q, &in), ASX_E_PERMISSION_DENIED);
        ASSERT_EQ(asx_msg_queue_len(&q), 0u);
        return;
    }
    ASSERT_EQ(asx_msg_queue_push(&q, &in), ASX_OK);
    ASSERT_EQ(asx_msg_queue_len(&q), 1u);
    ASSERT_FALSE(asx_msg_queue_is_empty(&q));

    ASSERT_EQ(asx_msg_queue_poll(&q, &out), ASX_OK);
    ASSERT_STR_EQ(out.topic, "test");
    ASSERT_EQ(out.payload_len, 5u);
    ASSERT_EQ(out.sequence, 1u);
    ASSERT_TRUE(asx_msg_queue_is_empty(&q));
}

TEST(queue_poll_empty) {
    asx_msg_queue q;
    asx_message out;

    asx_msg_queue_init(&q);
    if (!messaging_surface_available()) {
        ASSERT_EQ(asx_msg_queue_poll(&q, &out), ASX_E_PERMISSION_DENIED);
        return;
    }
    ASSERT_EQ(asx_msg_queue_poll(&q, &out), ASX_E_PENDING);
}

TEST(queue_fifo_order) {
    asx_msg_queue q;
    asx_message m1, m2, out;

    asx_msg_queue_init(&q);
    asx_message_set(&m1, "t", "first", 5);
    asx_message_set(&m2, "t", "second", 6);
    if (!messaging_surface_available()) {
        ASSERT_EQ(asx_msg_queue_push(&q, &m1), ASX_E_PERMISSION_DENIED);
        ASSERT_EQ(asx_msg_queue_push(&q, &m2), ASX_E_PERMISSION_DENIED);
        return;
    }
    asx_msg_queue_push(&q, &m1);
    asx_msg_queue_push(&q, &m2);

    asx_msg_queue_poll(&q, &out);
    ASSERT_TRUE(memcmp(out.payload, "first", 5) == 0);
    ASSERT_EQ(out.sequence, 1u);

    asx_msg_queue_poll(&q, &out);
    ASSERT_TRUE(memcmp(out.payload, "second", 6) == 0);
    ASSERT_EQ(out.sequence, 2u);
}

TEST(queue_full) {
    asx_msg_queue q;
    asx_message msg;
    uint32_t i;

    asx_msg_queue_init(&q);
    asx_message_set(&msg, "t", "x", 1);

    if (!messaging_surface_available()) {
        ASSERT_EQ(asx_msg_queue_push(&q, &msg), ASX_E_PERMISSION_DENIED);
        return;
    }

    for (i = 0; i < ASX_MSG_QUEUE_DEPTH; i++) { ASSERT_EQ(asx_msg_queue_push(&q, &msg), ASX_OK); }
    ASSERT_EQ(asx_msg_queue_push(&q, &msg), ASX_E_WOULD_BLOCK);
}

TEST(broker_create_topic) {
    asx_msg_broker broker;
    asx_msg_topic *topic;

    asx_msg_broker_init(&broker);
    if (!messaging_surface_available()) {
        ASSERT_EQ(asx_msg_broker_topic(&broker, "events", &topic), ASX_E_PERMISSION_DENIED);
        return;
    }
    ASSERT_EQ(asx_msg_broker_topic(&broker, "events", &topic), ASX_OK);
    ASSERT_TRUE(topic != NULL);
    ASSERT_STR_EQ(topic->name, "events");
    ASSERT_TRUE(topic->active);

    /* Getting same topic returns same pointer */
    {
        asx_msg_topic *topic2;
        ASSERT_EQ(asx_msg_broker_topic(&broker, "events", &topic2), ASX_OK);
        ASSERT_TRUE(topic == topic2);
    }
}

TEST(broker_pubsub) {
    asx_msg_broker broker;
    asx_msg_topic *topic;
    asx_msg_queue sub1, sub2;
    asx_message msg, out;

    asx_msg_broker_init(&broker);
    if (!messaging_surface_available()) {
        ASSERT_EQ(asx_msg_broker_topic(&broker, "news", &topic), ASX_E_PERMISSION_DENIED);
        return;
    }
    asx_msg_broker_topic(&broker, "news", &topic);

    asx_msg_queue_init(&sub1);
    asx_msg_queue_init(&sub2);
    ASSERT_EQ(asx_msg_topic_subscribe(topic, &sub1), ASX_OK);
    ASSERT_EQ(asx_msg_topic_subscribe(topic, &sub2), ASX_OK);
    ASSERT_EQ(asx_msg_topic_subscriber_count(topic), 2u);

    asx_message_set(&msg, "news", "breaking", 8);
    ASSERT_EQ(asx_msg_topic_publish(topic, &msg), ASX_OK);
    ASSERT_EQ(asx_msg_topic_total_published(topic), 1u);

    /* Both subscribers receive the message */
    ASSERT_EQ(asx_msg_queue_poll(&sub1, &out), ASX_OK);
    ASSERT_TRUE(memcmp(out.payload, "breaking", 8) == 0);

    ASSERT_EQ(asx_msg_queue_poll(&sub2, &out), ASX_OK);
    ASSERT_TRUE(memcmp(out.payload, "breaking", 8) == 0);
}

TEST(broker_publish_rejects_topic_mismatch) {
    asx_msg_broker broker;
    asx_msg_topic *topic;
    asx_msg_queue sub;
    asx_message msg;

    asx_msg_broker_init(&broker);
    if (!messaging_surface_available()) {
        ASSERT_EQ(asx_msg_broker_topic(&broker, "news", &topic), ASX_E_PERMISSION_DENIED);
        return;
    }
    ASSERT_EQ(asx_msg_broker_topic(&broker, "news", &topic), ASX_OK);
    asx_msg_queue_init(&sub);
    ASSERT_EQ(asx_msg_topic_subscribe(topic, &sub), ASX_OK);
    ASSERT_EQ(asx_message_set(&msg, "other", "payload", 7u), ASX_OK);

    ASSERT_EQ(asx_msg_topic_publish(topic, &msg), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_msg_queue_len(&sub), 0u);
    ASSERT_EQ(asx_msg_topic_total_published(topic), 0u);
}

TEST(broker_topic_exhaustion) {
    asx_msg_broker broker;
    asx_msg_topic *topic;
    uint32_t i;
    char name[16];

    asx_msg_broker_init(&broker);
    if (!messaging_surface_available()) {
        ASSERT_EQ(asx_msg_broker_topic(&broker, "a", &topic), ASX_E_PERMISSION_DENIED);
        return;
    }
    for (i = 0; i < ASX_MSG_MAX_TOPICS; i++) {
        name[0] = 'a' + (char)(i % 26);
        name[1] = '\0';
        ASSERT_EQ(asx_msg_broker_topic(&broker, name, &topic), ASX_OK);
    }
    ASSERT_EQ(asx_msg_broker_topic(&broker, "overflow", &topic), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(broker_null_args) {
    asx_msg_topic *topic;
    ASSERT_EQ(asx_msg_broker_topic(NULL, "t", &topic), ASX_E_INVALID_ARGUMENT);
}

int main(void) {
    fprintf(stderr, "=== messaging tests ===\n");
    RUN_TEST(message_set);
    RUN_TEST(message_null_args);
    RUN_TEST(message_null_payload_with_len_fails);
    RUN_TEST(queue_push_poll);
    RUN_TEST(queue_poll_empty);
    RUN_TEST(queue_fifo_order);
    RUN_TEST(queue_full);
    RUN_TEST(broker_create_topic);
    RUN_TEST(broker_pubsub);
    RUN_TEST(broker_publish_rejects_topic_mismatch);
    RUN_TEST(broker_topic_exhaustion);
    RUN_TEST(broker_null_args);
    TEST_REPORT();
    return test_failures;
}
