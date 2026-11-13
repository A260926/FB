// MQTT5 property decode contract tests.
//
// This file registers cases only; task_support/driver.c provides main() and
// runs the values/boundaries/framing/fuzz groups.
#include "../mongoose.h"

#include <stdint.h>

#include "../task_support/check.h"
#include "mqtt_property_core.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static struct mg_mqtt_message msg_wrap(const void *buf, size_t total,
                                       size_t props_start, size_t props_size) {
  struct mg_mqtt_message m;
  memset(&m, 0, sizeof(m));
  m.dgram.buf = (char *) buf;
  m.dgram.len = total;
  m.props_start = props_start;
  m.props_size = props_size;
  return m;
}

static int prop_has_no_views(const struct mg_mqtt_prop *p) {
  return p->key.buf == NULL && p->key.len == 0 && p->val.buf == NULL &&
         p->val.len == 0;
}

// A decoded property that neither convicts nor is stale from a previous call.
static int prop_views_zeroed_except(const struct mg_mqtt_prop *p,
                                    int key_used, int val_used) {
  if (!key_used && (p->key.buf != NULL || p->key.len != 0)) return 0;
  if (!val_used && (p->val.buf != NULL || p->val.len != 0)) return 0;
  return 1;
}

// ---------------------------------------------------------------------------
// values: hand-written bytes and expected fields
// ---------------------------------------------------------------------------

// One property of each of the seven representations, plus a repeated user
// property, in one mixed list:
//   byte 0x01 = 0x7f
//   short 0x13 = 0x1234
//   int 0x02 = 0x11223344
//   varint 0x0b = 128 (0x80 0x01, deliberately not the shortest form)
//   string 0x03 = "abc"
//   binary 0x09 = 00 01 00 ff
//   user 0x26 = ("", "")
//   user 0x26 = ("k1", "v")
static const uint8_t s_mixed[] = {
    0x01, 0x7f,                                     //  0..1
    0x13, 0x12, 0x34,                               //  2..4
    0x02, 0x11, 0x22, 0x33, 0x44,                   //  5..9
    0x0b, 0x80, 0x01,                               // 10..12
    0x03, 0x00, 0x03, 'a', 'b', 'c',                // 13..18
    0x09, 0x00, 0x04, 0x00, 0x01, 0x00, 0xff,       // 19..25
    0x26, 0x00, 0x00, 0x00, 0x00,                   // 26..30
    0x26, 0x00, 0x02, 'k', '1', 0x00, 0x01, 'v'     // 31..38
};

static int case_values_seven_types(void) {
  const uint8_t *s = s_mixed;
  struct mg_mqtt_message m = msg_wrap(s, sizeof(s_mixed), 0, sizeof(s_mixed));
  struct mg_mqtt_prop p;
  size_t off = 0;

  memset(&p, 0xee, sizeof(p));
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
  TASK_CHECK(off == 2);
  TASK_CHECK(p.id == MQTT_PROP_PAYLOAD_FORMAT_INDICATOR);
  TASK_CHECK(p.iv == 0x7f);
  TASK_CHECK(prop_has_no_views(&p));

  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
  TASK_CHECK(off == 5);
  TASK_CHECK(p.id == MQTT_PROP_SERVER_KEEP_ALIVE);
  TASK_CHECK(p.iv == 0x1234);
  TASK_CHECK(prop_has_no_views(&p));

  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
  TASK_CHECK(off == 10);
  TASK_CHECK(p.id == MQTT_PROP_MESSAGE_EXPIRY_INTERVAL);
  TASK_CHECK(p.iv == 0x11223344u);
  TASK_CHECK(prop_has_no_views(&p));

  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
  TASK_CHECK(off == 13);
  TASK_CHECK(p.id == MQTT_PROP_SUBSCRIPTION_IDENTIFIER);
  TASK_CHECK(p.iv == 128);
  TASK_CHECK(prop_has_no_views(&p));

  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
  TASK_CHECK(off == 19);
  TASK_CHECK(p.id == MQTT_PROP_CONTENT_TYPE);
  TASK_CHECK(p.iv == 0);
  TASK_CHECK(p.key.buf == NULL && p.key.len == 0);
  TASK_CHECK(p.val.len == 3);
  TASK_CHECK(p.val.buf == (const char *) s + 16);
  TASK_CHECK(memcmp(p.val.buf, "abc", 3) == 0);

  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
  TASK_CHECK(off == 26);
  TASK_CHECK(p.id == MQTT_PROP_CORRELATION_DATA);
  TASK_CHECK(p.iv == 0);
  TASK_CHECK(p.key.buf == NULL && p.key.len == 0);
  TASK_CHECK(p.val.len == 4);
  TASK_CHECK(p.val.buf == (const char *) s + 22);
  TASK_CHECK(p.val.buf[0] == 0 && p.val.buf[1] == 1 && p.val.buf[2] == 0 &&
             (uint8_t) p.val.buf[3] == 0xff);

  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
  TASK_CHECK(off == 31);
  TASK_CHECK(p.id == MQTT_PROP_USER_PROPERTY);
  TASK_CHECK(p.iv == 0);
  TASK_CHECK(p.key.len == 0 && p.val.len == 0);
  TASK_CHECK(p.key.buf == (const char *) s + 29);
  TASK_CHECK(p.val.buf == (const char *) s + 31);

  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
  TASK_CHECK(off == 39);
  TASK_CHECK(p.id == MQTT_PROP_USER_PROPERTY);
  TASK_CHECK(p.key.len == 2 && memcmp(p.key.buf, "k1", 2) == 0);
  TASK_CHECK(p.key.buf == (const char *) s + 34);
  TASK_CHECK(p.val.len == 1 && memcmp(p.val.buf, "v", 1) == 0);
  TASK_CHECK(p.val.buf == (const char *) s + 38);

  memset(&p, 0xee, sizeof(p));
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 0);
  TASK_CHECK(off == sizeof(s_mixed));
  TASK_CHECK(p.id == 0xee);  // end of section leaves the property untouched
  return 0;
}

static int case_values_varint_edges(void) {
  // 0, 127, 128, 268435455, and a non-minimal zero (0x80 0x00).
  static const uint8_t s[] = {0x0b, 0x00,       0x0b, 0x7f, 0x0b, 0x80,
                              0x01, 0x0b, 0xff, 0xff, 0xff, 0x7f, 0x0b,
                              0x80, 0x00};
  static const uint32_t want[] = {0, 127, 128, 268435455u, 0};
  struct mg_mqtt_message m = msg_wrap(s, sizeof(s), 0, sizeof(s));
  struct mg_mqtt_prop p;
  size_t off = 0, i;
  for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
    TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
    TASK_CHECK(p.id == MQTT_PROP_SUBSCRIPTION_IDENTIFIER);
    TASK_CHECK(p.iv == want[i]);
    TASK_CHECK(prop_has_no_views(&p));
  }
  TASK_CHECK(off == sizeof(s));
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 0);
  return 0;
}

static int case_values_stale_fields_reset(void) {
  // Reading a byte after a string must not leave the old string view behind,
  // and reading a string after a user property must clear the old key.
  static const uint8_t s[] = {0x01, 0x2a, 0x03, 0x00, 0x02, 'h', 'i',
                              0x26, 0x00, 0x01, 'k', 0x00, 0x01, 'v'};
  struct mg_mqtt_message m = msg_wrap(s, sizeof(s), 0, sizeof(s));
  struct mg_mqtt_prop p;
  size_t off = 0;

  memset(&p, 0xff, sizeof(p));  // junk in every field before the first call
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
  TASK_CHECK(p.id == MQTT_PROP_PAYLOAD_FORMAT_INDICATOR && p.iv == 0x2a);
  TASK_CHECK(prop_views_zeroed_except(&p, 0, 0));

  memset(&p, 0xff, sizeof(p));
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
  TASK_CHECK(p.id == MQTT_PROP_CONTENT_TYPE && p.iv == 0);
  TASK_CHECK(p.val.len == 2 && memcmp(p.val.buf, "hi", 2) == 0);
  TASK_CHECK(prop_views_zeroed_except(&p, 0, 1));

  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
  TASK_CHECK(p.id == MQTT_PROP_USER_PROPERTY && p.iv == 0);
  TASK_CHECK(p.key.len == 1 && p.val.len == 1);
  TASK_CHECK(prop_views_zeroed_except(&p, 1, 1));
  TASK_CHECK(off == sizeof(s));
  return 0;
}

static int case_values_empty_and_binary_zeros(void) {
  static const uint8_t s[] = {
      0x03, 0x00, 0x00,              // empty string
      0x09, 0x00, 0x00,              // empty binary
      0x26, 0x00, 0x00, 0x00, 0x00,  // empty user property
      0x09, 0x00, 0x03, 0x00, 0x00, 0x00  // all-zero binary, length preserved
  };
  static const size_t offs[] = {0, 3, 6, 11};
  static const size_t lens[] = {0, 0, 0, 3};
  struct mg_mqtt_message m = msg_wrap(s, sizeof(s), 0, sizeof(s));
  struct mg_mqtt_prop p;
  size_t off = 0, i;
  for (i = 0; i < 4; i++) {
    TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
    TASK_CHECK(off == (i + 1 < 4 ? offs[i + 1] : sizeof(s)));
    if (i == 2) {
      TASK_CHECK(p.key.len == 0 && p.val.len == 0);
      TASK_CHECK(p.key.buf != NULL && p.val.buf != NULL);
    } else {
      TASK_CHECK(p.val.len == lens[i]);
      TASK_CHECK(p.val.buf != NULL);
      if (lens[i] > 0)
        TASK_CHECK(p.val.buf[0] == 0 && p.val.buf[1] == 0 &&
                   p.val.buf[2] == 0);
    }
  }
  return 0;
}

static int case_values_legacy_wrapper(void) {
  // mg_mqtt_next_prop keeps its signature and relative-offset success value,
  // and reuses the same safe decoder.
  static const uint8_t s[] = {0x01, 0x05, 0x13, 0x12, 0x34, 0xff, 0x00};
  struct mg_mqtt_message m = msg_wrap(s, sizeof(s) - 1, 0, 5);
  struct mg_mqtt_prop p;
  struct mg_mqtt_prop saved;
  size_t pos;

  memset(&p, 0xee, sizeof(p));
  pos = mg_mqtt_next_prop(&m, &p, 0);
  TASK_CHECK(pos == 2 && p.id == 0x01 && p.iv == 5);

  pos = mg_mqtt_next_prop(&m, &p, pos);
  TASK_CHECK(pos == 5 && p.id == 0x13 && p.iv == 0x1234);

  saved = p;
  pos = mg_mqtt_next_prop(&m, &p, pos);
  TASK_CHECK(pos == 0);  // normal end and errors both report 0
  TASK_CHECK(core_same_prop(&p, &saved));

  // An unknown id reports 0 and does not modify the property.
  m.props_start = 5;
  m.props_size = 1;
  p.id = 0x26;
  p.iv = 7;
  p.key.len = 3;
  saved = p;
  pos = mg_mqtt_next_prop(&m, &p, 0);
  TASK_CHECK(pos == 0);
  TASK_CHECK(core_same_prop(&p, &saved));
  return 0;
}

// ---------------------------------------------------------------------------
// boundaries: truncation, section ends, and SIZE_MAX-near descriptions
// ---------------------------------------------------------------------------

struct boundary_fixture {
  const char *name;
  const uint8_t *bytes;
  size_t size;
};

static const uint8_t s_b_byte[] = {0x01, 0x2a};
static const uint8_t s_b_short[] = {0x13, 0x12, 0x34};
static const uint8_t s_b_int[] = {0x02, 0x11, 0x22, 0x33, 0x44};
static const uint8_t s_b_varint[] = {0x0b, 0xff, 0xff, 0xff, 0x7f};
static const uint8_t s_b_string[] = {0x03, 0x00, 0x03, 'a', 'b', 'c'};
static const uint8_t s_b_binary[] = {0x09, 0x00, 0x02, 0x00, 0xff};
static const uint8_t s_b_pair[] = {0x26, 0x00, 0x01, 'k', 0x00, 0x01, 'v'};

static const struct boundary_fixture s_fixtures[] = {
    {"byte", s_b_byte, sizeof(s_b_byte)},
    {"short", s_b_short, sizeof(s_b_short)},
    {"int", s_b_int, sizeof(s_b_int)},
    {"varint", s_b_varint, sizeof(s_b_varint)},
    {"string", s_b_string, sizeof(s_b_string)},
    {"binary", s_b_binary, sizeof(s_b_binary)},
    {"pair", s_b_pair, sizeof(s_b_pair)}};

// Guarded buffer: both the input bytes and the decoded property get sentinels.
struct guarded_bytes {
  uint32_t pre[4];
  uint8_t buf[64];
  uint32_t post[4];
};

struct guarded_prop {
  uint32_t pre[4];
  struct mg_mqtt_prop p;
  uint32_t post[4];
};

static void guard_bytes_init(struct guarded_bytes *g) {
  size_t i;
  for (i = 0; i < 4; i++) g->pre[i] = g->post[i] = 0xdeadbeefu;
  memset(g->buf, 0x5a, sizeof(g->buf));
}

static int guard_bytes_ok(const struct guarded_bytes *g) {
  size_t i;
  for (i = 0; i < 4; i++)
    if (g->pre[i] != 0xdeadbeefu || g->post[i] != 0xdeadbeefu) return 0;
  return 1;
}

static void guard_prop_init(struct guarded_prop *g, uint8_t fill) {
  size_t i;
  for (i = 0; i < 4; i++) g->pre[i] = g->post[i] = 0xa5a5a5a5u;
  memset(&g->p, fill, sizeof(g->p));
}

static int guard_prop_ok(const struct guarded_prop *g) {
  size_t i;
  for (i = 0; i < 4; i++)
    if (g->pre[i] != 0xa5a5a5a5u || g->post[i] != 0xa5a5a5a5u) return 0;
  return 1;
}

static int case_boundaries_truncation(void) {
  size_t f, t;
  for (f = 0; f < sizeof(s_fixtures) / sizeof(s_fixtures[0]); f++) {
    const struct boundary_fixture *fx = &s_fixtures[f];
    for (t = 1; t < fx->size; t++) {
      struct guarded_bytes gb;
      struct guarded_prop gp;
      struct mg_mqtt_message m;
      struct mg_mqtt_prop before;
      size_t off = 0;
      int rc;

      // (a) Logical area ends inside the property, payload bytes follow.
      guard_bytes_init(&gb);
      memcpy(gb.buf, fx->bytes, fx->size);
      m = msg_wrap(gb.buf, fx->size + 8, 0, t);
      guard_prop_init(&gp, 0x3c);
      before = gp.p;
      rc = mg_mqtt_prop_next(&m, &gp.p, &off);
      TASK_CHECK(rc == -1);
      TASK_CHECK(core_same_prop(&gp.p, &before));
      TASK_CHECK(off == 0);
      TASK_CHECK(guard_prop_ok(&gp));
      TASK_CHECK(guard_bytes_ok(&gb));

      // (b) The truncation is also the physical end of the buffer.
      guard_bytes_init(&gb);
      memcpy(gb.buf, fx->bytes, t);
      m = msg_wrap(gb.buf, t, 0, t);
      guard_prop_init(&gp, 0x3c);
      before = gp.p;
      rc = mg_mqtt_prop_next(&m, &gp.p, &off);
      TASK_CHECK(rc == -1);
      TASK_CHECK(core_same_prop(&gp.p, &before));
      TASK_CHECK(off == 0);
      TASK_CHECK(guard_prop_ok(&gp));
      TASK_CHECK(guard_bytes_ok(&gb));
    }
    // The complete property at the physical buffer end decodes fine.
    {
      struct guarded_bytes gb;
      struct guarded_prop gp;
      struct mg_mqtt_message m;
      size_t off = 0;
      guard_bytes_init(&gb);
      memcpy(gb.buf, fx->bytes, fx->size);
      m = msg_wrap(gb.buf, fx->size, 0, fx->size);
      guard_prop_init(&gp, 0x3c);
      TASK_CHECK(mg_mqtt_prop_next(&m, &gp.p, &off) == 1);
      TASK_CHECK(off == fx->size);
      TASK_CHECK(guard_prop_ok(&gp));
      TASK_CHECK(guard_bytes_ok(&gb));
    }
  }
  return 0;
}

static int case_boundaries_section_ends(void) {
  struct guarded_bytes gb;
  struct guarded_prop gp;
  struct mg_mqtt_message m;
  struct mg_mqtt_prop before;
  size_t off;
  int rc;

  // An unterminated variable integer and a five-byte one are both invalid,
  // even though the bytes after the section would let a loose reader finish.
  {
    static const uint8_t unterminated[] = {0x0b, 0x80, 0x80, 0x80, 0x80};
    static const uint8_t five_byte[] = {0x0b, 0x80, 0x80, 0x80, 0x80, 0x00};
    size_t n;
    guard_prop_init(&gp, 0x3c);
    before = gp.p;
    for (n = 0; n < 2; n++) {
      const uint8_t *b = n == 0 ? unterminated : five_byte;
      size_t bl = n == 0 ? sizeof(unterminated) : sizeof(five_byte);
      guard_bytes_init(&gb);
      memcpy(gb.buf, b, bl);
      m = msg_wrap(gb.buf, bl, 0, bl);
      off = 0;
      rc = mg_mqtt_prop_next(&m, &gp.p, &off);
      TASK_CHECK(rc == -1);
      TASK_CHECK(core_same_prop(&gp.p, &before));
      TASK_CHECK(off == 0);
      TASK_CHECK(guard_prop_ok(&gp));
      TASK_CHECK(guard_bytes_ok(&gb));
    }
    // A section cut before the terminator must not use the terminator that
    // sits just outside the declared section.
    {
      static const uint8_t b[] = {0x0b, 0x80, 0x80, 0x80, 0x80, 0x00};
      guard_bytes_init(&gb);
      memcpy(gb.buf, b, sizeof(b));
      m = msg_wrap(gb.buf, sizeof(b), 0, 5);  // terminator is byte 5
      off = 0;
      rc = mg_mqtt_prop_next(&m, &gp.p, &off);
      TASK_CHECK(rc == -1);
      TASK_CHECK(core_same_prop(&gp.p, &before));
      TASK_CHECK(off == 0);
      TASK_CHECK(guard_prop_ok(&gp));
      TASK_CHECK(guard_bytes_ok(&gb));
    }
  }

  // Empty section: 0, and nothing changes.
  guard_bytes_init(&gb);
  m = msg_wrap(gb.buf, sizeof(gb.buf), 0, 0);
  guard_prop_init(&gp, 0x3c);
  before = gp.p;
  off = 0;
  TASK_CHECK(mg_mqtt_prop_next(&m, &gp.p, &off) == 0);
  TASK_CHECK(core_same_prop(&gp.p, &before));
  TASK_CHECK(off == 0);
  TASK_CHECK(guard_prop_ok(&gp));

  // Exactly at the end of a non-empty section: 0, nothing changes.
  memcpy(gb.buf, s_b_pair, sizeof(s_b_pair));
  m = msg_wrap(gb.buf, sizeof(s_b_pair), 0, sizeof(s_b_pair));
  off = sizeof(s_b_pair);
  before = gp.p;
  TASK_CHECK(mg_mqtt_prop_next(&m, &gp.p, &off) == 0);
  TASK_CHECK(core_same_prop(&gp.p, &before));
  TASK_CHECK(off == sizeof(s_b_pair));

  // One past the end: -1, nothing changes.
  off = sizeof(s_b_pair) + 1;
  rc = mg_mqtt_prop_next(&m, &gp.p, &off);
  TASK_CHECK(rc == -1);
  TASK_CHECK(core_same_prop(&gp.p, &before));
  TASK_CHECK(off == sizeof(s_b_pair) + 1);
  TASK_CHECK(guard_prop_ok(&gp));

  // A section that claims more bytes than the buffer holds is invalid.
  m = msg_wrap(gb.buf, sizeof(s_b_pair), 0, sizeof(s_b_pair) + 1);
  off = 0;
  rc = mg_mqtt_prop_next(&m, &gp.p, &off);
  TASK_CHECK(rc == -1);
  TASK_CHECK(core_same_prop(&gp.p, &before));
  TASK_CHECK(off == 0);
  TASK_CHECK(guard_prop_ok(&gp));
  return 0;
}

static int case_boundaries_size_max(void) {
  struct guarded_prop gp;
  struct mg_mqtt_message m;
  struct mg_mqtt_prop before;
  size_t off;
  int rc;

  guard_prop_init(&gp, 0x3c);
  before = gp.p;

  // props_start beyond the buffer.
  m = msg_wrap(NULL, 0, SIZE_MAX, 0);
  off = 0;
  rc = mg_mqtt_prop_next(&m, &gp.p, &off);
  TASK_CHECK(rc == -1);
  TASK_CHECK(core_same_prop(&gp.p, &before));

  m = msg_wrap(NULL, 0, SIZE_MAX - 2, SIZE_MAX - 1);
  off = 0;
  rc = mg_mqtt_prop_next(&m, &gp.p, &off);
  TASK_CHECK(rc == -1);
  TASK_CHECK(core_same_prop(&gp.p, &before));

  // props_start within the buffer but props_size near SIZE_MAX.
  m = msg_wrap(s_b_byte, 2, 0, SIZE_MAX);
  off = 0;
  rc = mg_mqtt_prop_next(&m, &gp.p, &off);
  TASK_CHECK(rc == -1);
  TASK_CHECK(core_same_prop(&gp.p, &before));

  m = msg_wrap(s_b_byte, 2, 1, SIZE_MAX - 1);
  off = 0;
  rc = mg_mqtt_prop_next(&m, &gp.p, &off);
  TASK_CHECK(rc == -1);
  TASK_CHECK(core_same_prop(&gp.p, &before));

  // A valid description with an offset near SIZE_MAX must not overflow.
  m = msg_wrap(s_b_pair, sizeof(s_b_pair), 0, sizeof(s_b_pair));
  off = SIZE_MAX;
  rc = mg_mqtt_prop_next(&m, &gp.p, &off);
  TASK_CHECK(rc == -1);
  TASK_CHECK(core_same_prop(&gp.p, &before));
  TASK_CHECK(off == SIZE_MAX);

  off = SIZE_MAX - 1;
  rc = mg_mqtt_prop_next(&m, &gp.p, &off);
  TASK_CHECK(rc == -1);
  TASK_CHECK(core_same_prop(&gp.p, &before));
  TASK_CHECK(off == SIZE_MAX - 1);

  // props_start + props_size would wrap; must be rejected before any pointer.
  m = msg_wrap(s_b_pair, sizeof(s_b_pair), SIZE_MAX - 1, 4);
  off = 0;
  rc = mg_mqtt_prop_next(&m, &gp.p, &off);
  TASK_CHECK(rc == -1);
  TASK_CHECK(core_same_prop(&gp.p, &before));
  TASK_CHECK(off == 0);
  TASK_CHECK(guard_prop_ok(&gp));
  return 0;
}

// ---------------------------------------------------------------------------
// framing: the real mg_mqtt_parse on MQTT5 PUBLISH
// ---------------------------------------------------------------------------

// QoS0, topic "t", zero properties, empty payload.
static const uint8_t s_p0_empty[] = {0x30, 0x04, 0x00, 0x01, 't', 0x00};
// QoS0, topic "t", zero properties, one payload byte.
static const uint8_t s_p0_one[] = {0x30, 0x05, 0x00, 0x01, 't', 0x00, 'x'};
// QoS1, topic "ab", id 0x1234, property length 4, content type "x", payload
// "hi".
static const uint8_t s_p1[] = {0x32, 0x0d, 0x00, 0x02, 'a', 'b', 0x12,
                               0x34, 0x04, 0x03, 0x00, 0x01, 'x', 'h',
                               'i'};
// QoS2, same layout.
static const uint8_t s_p2[] = {0x34, 0x0d, 0x00, 0x02, 'a', 'b', 0x12,
                               0x34, 0x04, 0x03, 0x00, 0x01, 'x', 'h',
                               'i'};
// QoS1 but the property claims a 5-byte value inside a 4-byte property area.
static const uint8_t s_p_badprop[] = {0x32, 0x0d, 0x00, 0x02, 'a', 'b', 0x12,
                                      0x34, 0x04, 0x03, 0x00, 0x05, 'x',
                                      'h', 'i'};
// QoS1 but property length claims 9 bytes with only 4 present.
static const uint8_t s_p_badlen[] = {0x32, 0x0d, 0x00, 0x02, 'a', 'b',
                                     0x12, 0x34, 0x09, 0x03, 0x00, 0x01,
                                     'x',  'h',  'i'};
// QoS0 with an unterminated property length varint.
static const uint8_t s_p_badvarint[] = {0x30, 0x05, 0x00, 0x01,
                                        't',  0x80, 0x80};
// QoS0, topic "top", a 2-byte property area holding byte property 0x01 0x01,
// payload "data".
static const uint8_t s_p_legal[] = {0x30, 0x0c, 0x00, 0x03, 't', 'o', 'p',
                                    0x02, 0x01, 0x01, 'd', 'a', 't', 'a'};
// A second complete packet used to try to lend bytes to the first one.
static const uint8_t s_p_second[] = {0x30, 0x04, 0x00, 0x01, 'z', 0x00};

static int case_framing_zero_props(void) {
  struct mg_mqtt_message m;
  TASK_CHECK(mg_mqtt_parse(s_p0_empty, sizeof(s_p0_empty), 5, &m) == MQTT_OK);
  TASK_CHECK(m.dgram.len == sizeof(s_p0_empty));
  TASK_CHECK(m.topic.len == 1 && m.topic.buf[0] == 't');
  TASK_CHECK(m.props_start == 6 && m.props_size == 0);
  TASK_CHECK(m.data.len == 0);
  TASK_CHECK(m.data.buf == (const char *) s_p0_empty + 6);

  TASK_CHECK(mg_mqtt_parse(s_p0_one, sizeof(s_p0_one), 5, &m) == MQTT_OK);
  TASK_CHECK(m.props_start == 6 && m.props_size == 0);
  TASK_CHECK(m.data.len == 1 && m.data.buf[0] == 'x');
  TASK_CHECK(m.data.buf == (const char *) s_p0_one + 6);
  return 0;
}

static int case_framing_qos_and_props(void) {
  struct mg_mqtt_message m;
  struct mg_mqtt_prop p;
  size_t off = 0;

  TASK_CHECK(mg_mqtt_parse(s_p1, sizeof(s_p1), 5, &m) == MQTT_OK);
  TASK_CHECK(m.qos == 1 && m.id == 0x1234);
  TASK_CHECK(m.topic.len == 2 && memcmp(m.topic.buf, "ab", 2) == 0);
  TASK_CHECK(m.props_start == 9 && m.props_size == 4);
  TASK_CHECK(m.data.len == 2 && memcmp(m.data.buf, "hi", 2) == 0);
  TASK_CHECK(m.dgram.len == sizeof(s_p1));
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
  TASK_CHECK(off == 4);
  TASK_CHECK(p.id == MQTT_PROP_CONTENT_TYPE);
  TASK_CHECK(p.val.len == 1 && p.val.buf[0] == 'x');
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 0);

  TASK_CHECK(mg_mqtt_parse(s_p2, sizeof(s_p2), 5, &m) == MQTT_OK);
  TASK_CHECK(m.qos == 2 && m.id == 0x1234);
  TASK_CHECK(m.props_start == 9 && m.props_size == 4);
  TASK_CHECK(m.data.len == 2 && memcmp(m.data.buf, "hi", 2) == 0);
  return 0;
}

static int case_framing_malformed(void) {
  struct mg_mqtt_message m;

  // Complete packet, property value does not fit its declared area.
  TASK_CHECK(mg_mqtt_parse(s_p_badprop, sizeof(s_p_badprop), 5, &m) ==
             MQTT_MALFORMED);

  // Complete packet, property area extends past the packet.
  TASK_CHECK(mg_mqtt_parse(s_p_badlen, sizeof(s_p_badlen), 5, &m) ==
             MQTT_MALFORMED);

  // Complete packet, property length varint never terminates.
  TASK_CHECK(mg_mqtt_parse(s_p_badvarint, sizeof(s_p_badvarint), 5, &m) ==
             MQTT_MALFORMED);

  // Unterminated topic: complete fixed header, topic claims more bytes.
  {
    static const uint8_t bad[] = {0x30, 0x02, 0x00, 0x05, 'a'};
    TASK_CHECK(mg_mqtt_parse(bad, sizeof(bad), 5, &m) == MQTT_MALFORMED);
  }
  return 0;
}

static int case_framing_incomplete(void) {
  struct mg_mqtt_message m;
  size_t n;
  // Every strict prefix of a complete packet is INCOMPLETE.
  for (n = 0; n < sizeof(s_p1); n++) {
    TASK_CHECK(mg_mqtt_parse(s_p1, n, 5, &m) == MQTT_INCOMPLETE);
  }
  for (n = 0; n < sizeof(s_p0_empty); n++) {
    TASK_CHECK(mg_mqtt_parse(s_p0_empty, n, 5, &m) == MQTT_INCOMPLETE);
  }
  return 0;
}

static int case_framing_concatenated(void) {
  uint8_t joined[sizeof(s_p_badprop) + sizeof(s_p_second)];
  struct mg_mqtt_message m;
  size_t n;
  int rc;

  memcpy(joined, s_p_badprop, sizeof(s_p_badprop));
  memcpy(joined + sizeof(s_p_badprop), s_p_second, sizeof(s_p_second));

  // The second packet's bytes are plentiful but the first property must not
  // borrow them: the first packet stays malformed.
  rc = mg_mqtt_parse(joined, sizeof(joined), 5, &m);
  TASK_CHECK(rc == MQTT_MALFORMED);

  // A legal first packet followed by another packet: only the first is
  // parsed and dgram.len is the first packet's true length.
  memcpy(joined, s_p_legal, sizeof(s_p_legal));
  memcpy(joined + sizeof(s_p_legal), s_p_second, sizeof(s_p_second));
  rc = mg_mqtt_parse(joined, sizeof(s_p_legal) + sizeof(s_p_second), 5, &m);
  TASK_CHECK(rc == MQTT_OK);
  TASK_CHECK(m.dgram.len == sizeof(s_p_legal));
  TASK_CHECK(m.topic.len == 3 && memcmp(m.topic.buf, "top", 3) == 0);
  TASK_CHECK(m.props_start == 8 && m.props_size == 2);
  TASK_CHECK(m.data.len == 4 && memcmp(m.data.buf, "data", 4) == 0);
  {
    struct mg_mqtt_prop p;
    size_t off = 0;
    TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
    TASK_CHECK(p.id == MQTT_PROP_PAYLOAD_FORMAT_INDICATOR && p.iv == 1);
    TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 0);
  }

  // Enumerate every prefix of the legal first packet.
  for (n = 0; n < sizeof(s_p_legal); n++) {
    TASK_CHECK(mg_mqtt_parse(s_p_legal, n, 5, &m) == MQTT_INCOMPLETE);
  }
  TASK_CHECK(mg_mqtt_parse(s_p_legal, sizeof(s_p_legal), 5, &m) == MQTT_OK);
  TASK_CHECK(m.dgram.len == sizeof(s_p_legal));
  TASK_CHECK(m.topic.len == 3 && memcmp(m.topic.buf, "top", 3) == 0);
  TASK_CHECK(m.props_start == 8 && m.props_size == 2);
  TASK_CHECK(m.data.len == 4 && memcmp(m.data.buf, "data", 4) == 0);
  return 0;
}

static int case_framing_mqtt4_compat(void) {
  static const uint8_t v4[] = {0x30, 4, 0, 1, 'a', 'x'};
  static const uint8_t v4q1[] = {0x32, 8, 0, 2, 'a', 'b',
                                 0x12, 0x34, 'h', 'i'};
  struct mg_mqtt_message m;
  TASK_CHECK(mg_mqtt_parse(v4, sizeof(v4), 4, &m) == MQTT_OK);
  TASK_CHECK(m.topic.len == 1 && m.topic.buf[0] == 'a');
  TASK_CHECK(m.data.len == 1 && m.data.buf[0] == 'x');
  TASK_CHECK(m.props_size == 0);
  TASK_CHECK(m.dgram.len == sizeof(v4));

  TASK_CHECK(mg_mqtt_parse(v4q1, sizeof(v4q1), 4, &m) == MQTT_OK);
  TASK_CHECK(m.qos == 1 && m.id == 0x1234);
  TASK_CHECK(m.topic.len == 2 && memcmp(m.topic.buf, "ab", 2) == 0);
  TASK_CHECK(m.data.len == 2 && memcmp(m.data.buf, "hi", 2) == 0);
  TASK_CHECK(m.props_size == 0);
  return 0;
}

// ---------------------------------------------------------------------------
// fuzz: shared core, fixed seeds, recorded execution count
// ---------------------------------------------------------------------------

// Deterministic pseudo-random bytes for a fixed seed.
static size_t fill_seed_input(uint8_t *out, size_t cap, unsigned seed) {
  uint32_t s = 0x9e3779b9u ^ (seed * 2654435761u);
  size_t n = 3 + (seed % (cap - 2)), i;
  if (n > cap) n = cap;
  out[0] = (uint8_t) (seed % 8);  // property count - 1
  out[1] = (uint8_t) (seed % 7);  // starting representation
  out[2] = (uint8_t) (seed % 8);  // prefix before the property section
  for (i = 3; i < n; i++) {
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    out[i] = (uint8_t) s;
  }
  return n;
}

static int case_fuzz_fixed_seeds(void) {
  uint8_t buf[256];
  unsigned seed, executed = 0, decodes = 0, runs = 0, multiprop = 0;
  for (seed = 0; seed < 256; seed++) {
    size_t n = fill_seed_input(buf, sizeof(buf), seed);
    unsigned before = core_checks;
    unsigned d0 = core_decodes, r0 = core_runs, m0 = core_multiprop;
    if (core_run(buf, n) != 0) {
      core_report_failure(seed, buf, n, 16);
      return 1;
    }
    TASK_CHECK(core_checks > before);
    TASK_CHECK(core_decodes > d0);   // fields were really compared
    TASK_CHECK(core_runs > r0);
    multiprop += core_multiprop - m0;
    decodes += core_decodes - d0;
    runs += core_runs - r0;
    executed++;
  }
    TASK_CHECK(executed == 256);
    TASK_CHECK(decodes >= executed);
    TASK_CHECK(multiprop > 0);  // multi-property lists were exercised
    TASK_CHECK(core_wide);      // lengths crossed the 127/128 boundary
    printf("FUZZ_EXECUTED seeds=%u runs=%u decodes=%u multiprop=%u wide=%u\n",
           executed, runs, decodes, multiprop, core_wide);
  return 0;
}

// Structured seeds that provably cover all seven representations, multi-property
// lists, and value lengths crossing the 127/128 length-prefix boundary.
static int case_fuzz_coverage(void) {
  uint8_t buf[256];
  unsigned seed, tried = 0, executed = 0, decodes = 0;
  for (seed = 0; seed < 4096 && executed < 32; seed++) {
    struct core_prop model[CORE_MAX_PROPS];
    uint8_t sec[CORE_SEC_MAX];
    size_t count = 0, sec_len = 0, i;
    uint32_t actual_seed = 0;
    size_t n = fill_seed_input(buf, sizeof(buf), seed);
    int types = 0, wide = 0;
    unsigned d0 = core_decodes;
    buf[0] = 7;  // eight properties: the rotation covers all seven types
    buf[1] = 0;
    tried++;
    if (core_generate(buf, n, model, &count, sec, &sec_len, &actual_seed) != 0)
      continue;
    for (i = 0; i < count; i++) {
      types |= 1 << model[i].type;
      if (((model[i].type == MQTT_PROP_TYPE_STRING ||
            model[i].type == MQTT_PROP_TYPE_BINARY_DATA) &&
           model[i].vlen > 127) ||
          (model[i].type == MQTT_PROP_TYPE_STRING_PAIR &&
           (model[i].vlen > 127 || model[i].klen > 127)))
        wide = 1;
    }
    if (count < 8 || types != 0x7f || !wide) continue;
    if (core_run(buf, n) != 0) {
      core_report_failure(seed, buf, n, 16);
      return 1;
    }
    TASK_CHECK(core_decodes > d0);
    decodes += core_decodes - d0;
    executed++;
  }
  // Seeds carrying eight properties, all seven types, and at least one
  // string/binary value with a 128+ length prefix.
  TASK_CHECK(executed >= 32);
  TASK_CHECK(decodes >= executed * 8);
  TASK_CHECK(tried >= 1);
  printf("FUZZ_COVERAGE tried=%u executed=%u decodes=%u\n", tried, executed,
         decodes);
  return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    {"values", "seven_types", case_values_seven_types},
    {"values", "varint_edges", case_values_varint_edges},
    {"values", "stale_fields_reset", case_values_stale_fields_reset},
    {"values", "empty_and_binary_zeros", case_values_empty_and_binary_zeros},
    {"values", "legacy_wrapper", case_values_legacy_wrapper},

    {"boundaries", "truncation", case_boundaries_truncation},
    {"boundaries", "section_ends", case_boundaries_section_ends},
    {"boundaries", "size_max", case_boundaries_size_max},

    {"framing", "zero_props", case_framing_zero_props},
    {"framing", "qos_and_props", case_framing_qos_and_props},
    {"framing", "malformed", case_framing_malformed},
    {"framing", "incomplete", case_framing_incomplete},
    {"framing", "concatenated", case_framing_concatenated},
    {"framing", "mqtt4_compat", case_framing_mqtt4_compat},

    {"fuzz", "fixed_seeds", case_fuzz_fixed_seeds},
    {"fuzz", "coverage", case_fuzz_coverage},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
