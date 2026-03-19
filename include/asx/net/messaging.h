/*
 * asx/net/messaging.h — messaging and broker client abstraction
 *
 * Provides message queue, pub/sub topic, and broker client surfaces.
 * In the deterministic core, all messaging is in-memory with bounded
 * queues and topic-based dispatch.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_MESSAGING_H
#define ASX_NET_MESSAGING_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Messaging limits
 * ------------------------------------------------------------------- */

#ifndef ASX_MSG_MAX_PAYLOAD
#define ASX_MSG_MAX_PAYLOAD 1024u
#endif

#ifndef ASX_MSG_MAX_TOPIC_LEN
#define ASX_MSG_MAX_TOPIC_LEN 64u
#endif

#ifndef ASX_MSG_MAX_TOPICS
#define ASX_MSG_MAX_TOPICS 8u
#endif

#ifndef ASX_MSG_QUEUE_DEPTH
#define ASX_MSG_QUEUE_DEPTH 16u
#endif

#ifndef ASX_MSG_MAX_SUBSCRIBERS
#define ASX_MSG_MAX_SUBSCRIBERS 8u
#endif

/* -------------------------------------------------------------------
 * Message type
 * ------------------------------------------------------------------- */

typedef struct {
    char topic[ASX_MSG_MAX_TOPIC_LEN];
    uint8_t payload[ASX_MSG_MAX_PAYLOAD];
    uint32_t payload_len;
    uint64_t timestamp_ns;
    uint32_t sequence;
} asx_message;

/* Initialize a message. */
ASX_API void asx_message_init(asx_message *msg);

/* Set message topic and payload. */
ASX_API asx_status asx_message_set(asx_message *msg, const char *topic, const void *payload,
                                    uint32_t len);

/* -------------------------------------------------------------------
 * Message queue (point-to-point)
 * ------------------------------------------------------------------- */

typedef struct {
    asx_message messages[ASX_MSG_QUEUE_DEPTH];
    uint32_t head;
    uint32_t count;
    uint32_t next_seq;
    uint32_t total_enqueued;
    uint32_t total_dequeued;
} asx_msg_queue;

/* Initialize a message queue. */
ASX_API void asx_msg_queue_init(asx_msg_queue *q);

/* Enqueue a message. Returns ASX_E_WOULD_BLOCK if full. */
ASX_API asx_status asx_msg_queue_push(asx_msg_queue *q, const asx_message *msg);

/* Dequeue a message. Returns ASX_E_PENDING if empty. */
ASX_API asx_status asx_msg_queue_poll(asx_msg_queue *q, asx_message *out);

/* Get the current queue depth. */
ASX_API uint32_t asx_msg_queue_len(const asx_msg_queue *q);

/* Check if the queue is empty. */
ASX_API int asx_msg_queue_is_empty(const asx_msg_queue *q);

/* -------------------------------------------------------------------
 * Pub/sub topic
 * ------------------------------------------------------------------- */

typedef struct {
    asx_msg_queue *queue;
    uint8_t active;
} asx_msg_subscriber;

typedef struct {
    char name[ASX_MSG_MAX_TOPIC_LEN];
    asx_msg_subscriber subscribers[ASX_MSG_MAX_SUBSCRIBERS];
    uint32_t subscriber_count;
    uint32_t total_published;
    uint8_t active;
} asx_msg_topic;

/* -------------------------------------------------------------------
 * Broker
 * ------------------------------------------------------------------- */

typedef struct {
    asx_msg_topic topics[ASX_MSG_MAX_TOPICS];
    uint32_t topic_count;
} asx_msg_broker;

/* Initialize a broker. */
ASX_API void asx_msg_broker_init(asx_msg_broker *broker);

/* Create or get a topic by name. */
ASX_API asx_status asx_msg_broker_topic(asx_msg_broker *broker, const char *name,
                                         asx_msg_topic **out);

/* Subscribe a queue to a topic. */
ASX_API asx_status asx_msg_topic_subscribe(asx_msg_topic *topic, asx_msg_queue *subscriber_queue);

/* Publish a message to a topic (fans out to all subscribers). */
ASX_API asx_status asx_msg_topic_publish(asx_msg_topic *topic, const asx_message *msg);

/* Get the number of subscribers on a topic. */
ASX_API uint32_t asx_msg_topic_subscriber_count(const asx_msg_topic *topic);

/* Get total messages published to a topic. */
ASX_API uint32_t asx_msg_topic_total_published(const asx_msg_topic *topic);

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_MESSAGING_H */
