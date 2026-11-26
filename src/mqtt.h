#pragma once

#include "net.h"
#include "str.h"

#define MQTT_CMD_CONNECT 1
#define MQTT_CMD_CONNACK 2
#define MQTT_CMD_PUBLISH 3
#define MQTT_CMD_PUBACK 4
#define MQTT_CMD_PUBREC 5
#define MQTT_CMD_PUBREL 6
#define MQTT_CMD_PUBCOMP 7
#define MQTT_CMD_SUBSCRIBE 8
#define MQTT_CMD_SUBACK 9
#define MQTT_CMD_UNSUBSCRIBE 10
#define MQTT_CMD_UNSUBACK 11
#define MQTT_CMD_PINGREQ 12
#define MQTT_CMD_PINGRESP 13
#define MQTT_CMD_DISCONNECT 14
#define MQTT_CMD_AUTH 15

#define MQTT_PROP_PAYLOAD_FORMAT_INDICATOR 0x01
#define MQTT_PROP_MESSAGE_EXPIRY_INTERVAL 0x02
#define MQTT_PROP_CONTENT_TYPE 0x03
#define MQTT_PROP_RESPONSE_TOPIC 0x08
#define MQTT_PROP_CORRELATION_DATA 0x09
#define MQTT_PROP_SUBSCRIPTION_IDENTIFIER 0x0B
#define MQTT_PROP_SESSION_EXPIRY_INTERVAL 0x11
#define MQTT_PROP_ASSIGNED_CLIENT_IDENTIFIER 0x12
#define MQTT_PROP_SERVER_KEEP_ALIVE 0x13
#define MQTT_PROP_AUTHENTICATION_METHOD 0x15
#define MQTT_PROP_AUTHENTICATION_DATA 0x16
#define MQTT_PROP_REQUEST_PROBLEM_INFORMATION 0x17
#define MQTT_PROP_WILL_DELAY_INTERVAL 0x18
#define MQTT_PROP_REQUEST_RESPONSE_INFORMATION 0x19
#define MQTT_PROP_RESPONSE_INFORMATION 0x1A
#define MQTT_PROP_SERVER_REFERENCE 0x1C
#define MQTT_PROP_REASON_STRING 0x1F
#define MQTT_PROP_RECEIVE_MAXIMUM 0x21
#define MQTT_PROP_TOPIC_ALIAS_MAXIMUM 0x22
#define MQTT_PROP_TOPIC_ALIAS 0x23
#define MQTT_PROP_MAXIMUM_QOS 0x24
#define MQTT_PROP_RETAIN_AVAILABLE 0x25
#define MQTT_PROP_USER_PROPERTY 0x26
#define MQTT_PROP_MAXIMUM_PACKET_SIZE 0x27
#define MQTT_PROP_WILDCARD_SUBSCRIPTION_AVAILABLE 0x28
#define MQTT_PROP_SUBSCRIPTION_IDENTIFIER_AVAILABLE 0x29
#define MQTT_PROP_SHARED_SUBSCRIPTION_AVAILABLE 0x2A

enum {
  MQTT_PROP_TYPE_BYTE,
  MQTT_PROP_TYPE_STRING,
  MQTT_PROP_TYPE_STRING_PAIR,
  MQTT_PROP_TYPE_BINARY_DATA,
  MQTT_PROP_TYPE_VARIABLE_INT,
  MQTT_PROP_TYPE_INT,
  MQTT_PROP_TYPE_SHORT
};

enum { MQTT_OK, MQTT_INCOMPLETE, MQTT_MALFORMED };

struct mg_mqtt_prop {
  uint8_t id;         // Enumerated at MQTT5 Reference
  uint32_t iv;        // Integer value for 8-, 16-, 32-bit integers types
  struct mg_str key;  // Non-NULL only for user property type
  struct mg_str val;  // Non-NULL only for UTF-8 types and user properties
};

struct mg_mqtt_opts {
  struct mg_str user;               // Username, can be empty
  struct mg_str pass;               // Password, can be empty
  struct mg_str client_id;          // Client ID
  struct mg_str topic;              // message/subscription topic
  struct mg_str message;            // message content
  uint8_t qos;                      // message quality of service
  uint8_t version;                  // Can be 4 (3.1.1), or 5. If 0, assume 4
  uint16_t keepalive;               // Keep-alive timer in seconds
  uint16_t retransmit_id;           // For PUBLISH, init to 0
  bool retain;                      // Retain flag
  bool clean;                       // Clean session flag
  struct mg_mqtt_prop *props;       // MQTT5 props array
  size_t num_props;                 // number of props
  struct mg_mqtt_prop *will_props;  // Valid only for CONNECT packet (MQTT5)
  size_t num_will_props;            // Number of will props
};

struct mg_mqtt_message {
  struct mg_str topic;  // Parsed topic for PUBLISH
  struct mg_str data;   // Parsed message for PUBLISH
  struct mg_str dgram;  // Whole MQTT packet, including headers
  uint16_t id;          // For PUBACK, PUBREC, PUBREL, PUBCOMP, SUBACK, PUBLISH
  uint8_t cmd;          // MQTT command, one of MQTT_CMD_*
  uint8_t qos;          // Quality of service
  uint8_t ack;          // CONNACK return code, 0 = success
  size_t props_start;   // Offset to the start of the properties (MQTT5)
  size_t props_size;    // Length of the properties
};

struct mg_connection *mg_mqtt_connect(struct mg_mgr *, const char *url,
                                      const struct mg_mqtt_opts *opts,
                                      mg_event_handler_t fn, void *fn_data);
struct mg_connection *mg_mqtt_listen(struct mg_mgr *mgr, const char *url,
                                     mg_event_handler_t fn, void *fn_data);
void mg_mqtt_login(struct mg_connection *c, const struct mg_mqtt_opts *opts);
uint16_t mg_mqtt_pub(struct mg_connection *c, const struct mg_mqtt_opts *opts);
void mg_mqtt_sub(struct mg_connection *, const struct mg_mqtt_opts *opts);
int mg_mqtt_parse(const uint8_t *, size_t, uint8_t, struct mg_mqtt_message *);
void mg_mqtt_send_header(struct mg_connection *, uint8_t cmd, uint8_t flags,
                         uint32_t len);
void mg_mqtt_ping(struct mg_connection *);
void mg_mqtt_pong(struct mg_connection *);
void mg_mqtt_disconnect(struct mg_connection *, const struct mg_mqtt_opts *);

// Decodes the next MQTT5 property at *offset, where *offset is a byte offset
// from the start of the declared property section (msg->props_start), i.e. the
// caller starts with *offset = 0 and feeds back the value updated by the
// previous successful call.
//
// Returns 1 on success, updating *offset to the offset just past the decoded
// property and filling *prop. Returns 0 when *offset is exactly at the end of
// the property section (clean end of iteration). Returns -1 when the property
// section description is invalid, *offset is beyond the end, the property ID is
// unknown, or the property is truncated relative to the declared section.
//
// On a 0 or -1 return, *offset and every field of *prop keep their prior
// values: no partial string or partial user property is ever handed out. The
// strings in *prop (id/iv/key/val) are zeroed when unused on success. key/val
// buffers are zero-length views for empty values, and are borrowed from
// msg->dgram.buf: they stay valid only while that backing storage is valid and
// unchanged (i.e. until the caller frees or reuses the dgram buffer).
//
// msg->dgram.buf must point to dgram.len readable bytes (NULL is accepted when
// dgram.len is 0). A NULL msg, prop, or offset yields -1 without dereferencing
// them. Boundary arithmetic is done without overflowing size_t or forming
// out-of-range pointers, even when props_start, props_size or offset are near
// SIZE_MAX.
int mg_mqtt_prop_next(const struct mg_mqtt_message *msg,
                      struct mg_mqtt_prop *prop, size_t *offset);

// Legacy iteration helper. Kept for signature compatibility: on success it
// returns the next offset relative to the property-section start (and fills
// *prop); at the clean end of the section or on any error it returns 0 and
// leaves *prop untouched. Implemented on top of mg_mqtt_prop_next.
size_t mg_mqtt_next_prop(struct mg_mqtt_message *, struct mg_mqtt_prop *,
                         size_t ofs);

