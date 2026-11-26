// MQTT5 property iteration example.
//
// This example never contacts a network service. It builds a complete MQTT5
// PUBLISH packet in a local byte buffer, parses it with mg_mqtt_parse(), then
// walks its property section with mg_mqtt_prop_next(), printing each decoded
// property and stopping cleanly at the end.
//
// Error semantics (see mg_mqtt_prop_next in mongoose.h):
//   * returns  1: one property decoded; `prop` is filled and `offset` advanced
//   * returns  0: `offset` is exactly at the end of the property section
//   * returns -1: invalid section description, offset past the end, unknown
//                 property ID, or a property truncated with respect to the
//                 declared section
// On 0 and -1 the caller's `prop` and `offset` are left untouched, so a failed
// decode never yields a half-string or a half user property.
//
// Borrowed lifetime: the `val.buf` / `key.buf` views in `prop` point into the
// packet buffer that was passed to mg_mqtt_parse(). They stay valid only while
// that buffer is valid and unchanged; once it is freed or reused, copy what you
// need (for example with mg_strdup()) instead of keeping the views.

#include "mongoose.h"

#include <stdio.h>
#include <string.h>

// A complete MQTT5 PUBLISH packet on topic "dev/1" with three properties
// (content type, user property, message expiry interval) and payload "hi".
// Remaining length is 37 bytes:
//   2 topic-len + 5 topic + 1 prop-len + 27 props + 2 payload = 37.
static const uint8_t s_packet[] = {
    0x30, 0x25,                              // PUBLISH, QoS 0, remaining = 37
    0x00, 0x05, 'd', 'e', 'v', '/', '1',     // topic "dev/1"
    0x1B,                                    // property section length = 27
    0x03, 0x00, 0x0A, 't', 'e', 'x', 't', '/', 'p', 'l', 'a', 'i', 'n',
    0x26, 0x00, 0x02, 'k', '1', 0x00, 0x02, 'v', '1',
    0x02, 0x00, 0x00, 0x00, 0x3C,            // message expiry = 60 s
    'h', 'i'                                 // payload
};

static const char *prop_name(uint8_t id) {
  switch (id) {
    case MQTT_PROP_PAYLOAD_FORMAT_INDICATOR:
      return "payload-format-indicator";
    case MQTT_PROP_MESSAGE_EXPIRY_INTERVAL:
      return "message-expiry-interval";
    case MQTT_PROP_CONTENT_TYPE:
      return "content-type";
    case MQTT_PROP_USER_PROPERTY:
      return "user-property";
    case MQTT_PROP_SUBSCRIPTION_IDENTIFIER:
      return "subscription-identifier";
    default:
      return "other";
  }
}

int main(void) {
  struct mg_mqtt_message m;
  struct mg_mqtt_prop prop;
  size_t offset = 0;
  int rc, n = 0;

  rc = mg_mqtt_parse(s_packet, sizeof(s_packet), 5, &m);
  if (rc != MQTT_OK) {
    printf("packet rejected: %s\n",
           rc == MQTT_INCOMPLETE ? "incomplete" : "malformed");
    return 1;
  }

  printf("topic=%.*s payload=%.*s props=%lu bytes\n", (int) m.topic.len,
         m.topic.buf, (int) m.data.len, m.data.buf,
         (unsigned long) m.props_size);

  while ((rc = mg_mqtt_prop_next(&m, &prop, &offset)) == 1) {
    n++;
    switch (prop.id) {
      case MQTT_PROP_CONTENT_TYPE:
      case MQTT_PROP_RESPONSE_TOPIC:
      case MQTT_PROP_REASON_STRING:
        printf("  [%d] %s len=%lu val=%.*s\n", n, prop_name(prop.id),
               (unsigned long) prop.val.len, (int) prop.val.len, prop.val.buf);
        break;
      case MQTT_PROP_USER_PROPERTY:
        printf("  [%d] %s key=%.*s val=%.*s\n", n, prop_name(prop.id),
               (int) prop.key.len, prop.key.buf, (int) prop.val.len,
               prop.val.buf);
        break;
      case MQTT_PROP_CORRELATION_DATA:
      case MQTT_PROP_AUTHENTICATION_DATA:
        printf("  [%d] %s binary len=%lu\n", n, prop_name(prop.id),
               (unsigned long) prop.val.len);
        break;
      default:
        printf("  [%d] %s iv=%lu\n", n, prop_name(prop.id),
               (unsigned long) prop.iv);
        break;
    }
  }

  if (rc == 0) {
    printf("end of properties after %d propert%s\n", n, n == 1 ? "y" : "ies");
    return 0;
  }

  // rc == -1: the property section is corrupt. Repeat the failing call just to
  // show that nothing was handed back: prop and offset keep their old values.
  {
    struct mg_mqtt_prop before = prop;
    size_t before_offset = offset;
    rc = mg_mqtt_prop_next(&m, &prop, &offset);
    printf("decode error; offset %lu -> %lu, prop untouched=%s\n",
           (unsigned long) before_offset, (unsigned long) offset,
           memcmp(&before, &prop, sizeof(prop)) == 0 ? "yes" : "no");
  }
  return 1;
}
