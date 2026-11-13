// Copyright (c) 2024 Cesanta Software Limited
// All rights reserved
//
// Walk the MQTT5 properties of an in-memory PUBLISH packet.
//
// This example never opens a socket and never contacts a broker: the packet
// bytes are a local constant. It shows the three possible outcomes of
// mg_mqtt_prop_next() and, next to each decoded value, where that value lives
// inside the original datagram (the returned strings are borrowed views).
//
// Build (C99, no third-party dependencies):
//   cc -std=c99 -I. main.c mongoose.c -o example
// Run:
//   ./example

#include "mongoose.h"

#include <stdio.h>

// QoS0 PUBLISH, topic "sensors/temp", MQTT5 properties:
//   0x01 0x01                         payload format indicator = 1 (byte)
//   0x02 0x00 0x00 0x00 0x3c          message expiry interval = 60 (int)
//   0x03 0x00 0x0a "text/plain"       content type (string)
//   0x09 0x00 0x04 00 01 00 ff        correlation data (binary, keeps NULs)
//   0x26 0x00 0x04 "unit" 0x00 0x01 "C"  user property ("unit", "C")
// payload: "21.5"
static const unsigned char s_publish[] = {
    0x30, 0x38,                                            // fixed header
    0x00, 0x0c, 's', 'e',  'n',  's',  'o',  'r',  's',     // topic length+name
    '/',  't',  'e',  'm',  'p',                           //
    0x25,                                                  // property length
    0x01, 0x01,                                            //
    0x02, 0x00, 0x00, 0x00, 0x3c,                          //
    0x03, 0x00, 0x0a, 't',  'e',  'x',  't',  '/',  'p',    //
    'l',  'a',  'i',  'n',                                 //
    0x09, 0x00, 0x04, 0x00, 0x01, 0x00, 0xff,              //
    0x26, 0x00, 0x04, 'u',  'n',  'i',  't',  0x00, 0x01,  //
    'C',                                                   //
    '2',  '1',  '.',  '5'};                                // payload

static void show(const struct mg_mqtt_message *m, const struct mg_mqtt_prop *p,
                 size_t offset) {
  size_t id_at = m->props_start + offset;
  printf("  prop id=0x%02x next_offset=%lu value=", p->id,
         (unsigned long) offset);
  switch (p->id) {
    case MQTT_PROP_PAYLOAD_FORMAT_INDICATOR:
    case MQTT_PROP_MESSAGE_EXPIRY_INTERVAL:
    case MQTT_PROP_SUBSCRIPTION_IDENTIFIER:
      printf("%lu (integer at dgram[%lu])\n", (unsigned long) p->iv,
             (unsigned long) id_at);
      break;
    case MQTT_PROP_USER_PROPERTY:
      printf("(\"%.*s\", \"%.*s\") at dgram[%lu..%lu]\n", (int) p->key.len,
             p->key.buf, (int) p->val.len, p->val.buf,
             (unsigned long) (p->key.buf - m->dgram.buf),
             (unsigned long) (p->val.buf + p->val.len - m->dgram.buf));
      break;
    case MQTT_PROP_CORRELATION_DATA: {
      size_t i;
      printf("binary[%lu]=", (unsigned long) p->val.len);
      for (i = 0; i < p->val.len; i++)
        printf("%02x", (unsigned char) p->val.buf[i]);
      printf(" at dgram[%lu..%lu]\n",
             (unsigned long) (p->val.buf - m->dgram.buf),
             (unsigned long) (p->val.buf + p->val.len - m->dgram.buf));
      break;
    }
    default:  // strings
      printf("\"%.*s\" (string) at dgram[%lu..%lu]\n", (int) p->val.len,
             p->val.buf, (unsigned long) (p->val.buf - m->dgram.buf),
             (unsigned long) (p->val.buf + p->val.len - m->dgram.buf));
      break;
  }
}

int main(void) {
  struct mg_mqtt_message m;
  struct mg_mqtt_prop p;
  size_t offset = 0;
  int rc;

  // Parse a complete packet held in memory.
  rc = mg_mqtt_parse(s_publish, sizeof(s_publish), 5, &m);
  if (rc != MQTT_OK) {
    printf("parse failed: %d\n", rc);
    return 1;
  }
  printf("topic=\"%.*s\" props=[%lu,+%lu) payload=\"%.*s\"\n",
         (int) m.topic.len, m.topic.buf, (unsigned long) m.props_start,
         (unsigned long) m.props_size, (int) m.data.len, m.data.buf);

  // Walk the property section. 1 = decoded, 0 = clean end, -1 = bad section.
  while ((rc = mg_mqtt_prop_next(&m, &p, &offset)) == 1) show(&m, &p, offset);
  printf("walk ended rc=%d after %lu property bytes\n", rc,
         (unsigned long) offset);

  // A truncated property is reported as -1, and the output is unchanged.
  // Here the section ends at the correlation data id, with no length bytes.
  {
    struct mg_mqtt_message bad = m;
    struct mg_mqtt_prop keep;
    size_t at = 20;  // start of the 0x09 correlation data property
    memset(&keep, 0, sizeof(keep));
    keep.id = 0xaa;
    bad.props_size = 21;
    rc = mg_mqtt_prop_next(&bad, &keep, &at);
    printf("truncated section: rc=%d id=0x%02x offset=%lu (unchanged)\n", rc,
           keep.id, (unsigned long) at);
  }

  // Borrowed views: p.val.buf points into s_publish, so it is valid only
  // while that buffer is alive and unmodified. Do not free or reuse the
  // packet bytes while holding a decoded property.
  return 0;
}
