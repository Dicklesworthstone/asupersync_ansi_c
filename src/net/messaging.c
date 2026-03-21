/*
 * messaging.c — deterministic messaging and broker client
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/messaging.h>
#include <string.h>

static size_t msg_bounded_strlen(const char *str, size_t max) {
    size_t len;
    if (str == NULL) return 0u;
    for (len = 0u; len < max; len++) {
        if (str[len] == '\0') return len;
    }
    return max;
}

/* ------------------------------------------------------------------ */
/* Message                                                             */
/* ------------------------------------------------------------------ */

void asx_message_init(asx_message *msg) {
    if (msg == NULL) return;
    memset(msg, 0, sizeof(*msg));
}

asx_status asx_message_set(asx_message *msg, const char *topic, const void *payload,
                            uint32_t len) {
    size_t tlen;

    if (msg == NULL || topic == NULL) return ASX_E_INVALID_ARGUMENT;
    if (len > ASX_MSG_MAX_PAYLOAD) return ASX_E_BUFFER_TOO_SMALL;
    if (len > 0u && payload == NULL) return ASX_E_INVALID_ARGUMENT;
    tlen = msg_bounded_strlen(topic, ASX_MSG_MAX_TOPIC_LEN);
    if (tlen == 0u || tlen >= ASX_MSG_MAX_TOPIC_LEN) return ASX_E_INVALID_ARGUMENT;

    memset(msg, 0, sizeof(*msg));
    memcpy(msg->topic, topic, tlen + 1u);
    msg->payload_len = len;
    if (len > 0u && payload != NULL) memcpy(msg->payload, payload, len);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Message queue                                                       */
/* ------------------------------------------------------------------ */

void asx_msg_queue_init(asx_msg_queue *q) {
    if (q == NULL) return;
    memset(q, 0, sizeof(*q));
    q->next_seq = 1u;
}

asx_status asx_msg_queue_push(asx_msg_queue *q, const asx_message *msg) {
    uint32_t tail;

    if (q == NULL || msg == NULL) return ASX_E_INVALID_ARGUMENT;
    if (q->count >= ASX_MSG_QUEUE_DEPTH) return ASX_E_WOULD_BLOCK;

    tail = (q->head + q->count) % ASX_MSG_QUEUE_DEPTH;
    q->messages[tail] = *msg;
    q->messages[tail].sequence = q->next_seq++;
    q->count++;
    q->total_enqueued++;
    return ASX_OK;
}

asx_status asx_msg_queue_poll(asx_msg_queue *q, asx_message *out) {
    if (q == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (q->count == 0u) return ASX_E_PENDING;

    *out = q->messages[q->head];
    q->head = (q->head + 1u) % ASX_MSG_QUEUE_DEPTH;
    q->count--;
    q->total_dequeued++;
    return ASX_OK;
}

uint32_t asx_msg_queue_len(const asx_msg_queue *q) {
    if (q == NULL) return 0u;
    return q->count;
}

int asx_msg_queue_is_empty(const asx_msg_queue *q) {
    if (q == NULL) return 1;
    return q->count == 0u;
}

/* ------------------------------------------------------------------ */
/* Broker                                                              */
/* ------------------------------------------------------------------ */

void asx_msg_broker_init(asx_msg_broker *broker) {
    if (broker == NULL) return;
    memset(broker, 0, sizeof(*broker));
}

asx_status asx_msg_broker_topic(asx_msg_broker *broker, const char *name, asx_msg_topic **out) {
    uint32_t i;
    size_t len;
    asx_msg_topic *t;

    if (broker == NULL || name == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    *out = NULL;

    len = msg_bounded_strlen(name, ASX_MSG_MAX_TOPIC_LEN);
    if (len == 0u || len >= ASX_MSG_MAX_TOPIC_LEN) return ASX_E_INVALID_ARGUMENT;

    /* Find existing topic */
    for (i = 0u; i < broker->topic_count; i++) {
        if (broker->topics[i].active && strcmp(broker->topics[i].name, name) == 0) {
            *out = &broker->topics[i];
            return ASX_OK;
        }
    }

    /* Create new topic */
    if (broker->topic_count >= ASX_MSG_MAX_TOPICS) return ASX_E_RESOURCE_EXHAUSTED;
    t = &broker->topics[broker->topic_count];
    memset(t, 0, sizeof(*t));
    memcpy(t->name, name, len + 1u);
    t->active = 1u;
    broker->topic_count++;
    *out = t;
    return ASX_OK;
}

asx_status asx_msg_topic_subscribe(asx_msg_topic *topic, asx_msg_queue *subscriber_queue) {
    if (topic == NULL || subscriber_queue == NULL) return ASX_E_INVALID_ARGUMENT;
    if (topic->subscriber_count >= ASX_MSG_MAX_SUBSCRIBERS) return ASX_E_RESOURCE_EXHAUSTED;
    topic->subscribers[topic->subscriber_count].queue = subscriber_queue;
    topic->subscribers[topic->subscriber_count].active = 1u;
    topic->subscriber_count++;
    return ASX_OK;
}

asx_status asx_msg_topic_publish(asx_msg_topic *topic, const asx_message *msg) {
    uint32_t i;
    asx_status last_err;

    if (topic == NULL || msg == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!topic->active || strcmp(topic->name, msg->topic) != 0) return ASX_E_INVALID_ARGUMENT;
    last_err = ASX_OK;

    for (i = 0u; i < topic->subscriber_count; i++) {
        if (topic->subscribers[i].active && topic->subscribers[i].queue != NULL) {
            asx_status st = asx_msg_queue_push(topic->subscribers[i].queue, msg);
            if (st != ASX_OK) last_err = st;
        }
    }
    topic->total_published++;
    return last_err;
}

uint32_t asx_msg_topic_subscriber_count(const asx_msg_topic *topic) {
    if (topic == NULL) return 0u;
    return topic->subscriber_count;
}

uint32_t asx_msg_topic_total_published(const asx_msg_topic *topic) {
    if (topic == NULL) return 0u;
    return topic->total_published;
}
