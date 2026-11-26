// MQTT5 property decoding contract tests.
//
// Registers cases under the values / boundaries / framing / fuzz groups via
// task_cases and task_case_count; driver.c supplies main() and task_check().
//
// The fuzz group shares its checking core with test/mqtt_property_fuzz.c
// through mqtt_property_fuzz_core.h. In that core an independent model
// generates valid property values, encodes them with its own minimal
// big-endian encoder, and supplies the expected field values, ranges and
// offsets; expectations never come from the library's encoder or decoder.

#include "../task_support/check.h"
#include "mqtt_property_fuzz_core.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int mp_report(const char *what, const struct mp_fail *f) {
  fprintf(stderr, "DETAIL %s seed=%u at=%lu rule_line=%d msg=%s snippet=%s\n",
          what, (unsigned) f->seed, (unsigned long) f->at, f->line, f->msg,
          f->snippet);
  return 0;
}

#define REQUIRE_OK(cond, f, what)              \
  do {                                         \
    if (!(cond)) {                             \
      mp_report((what), (f));                  \
      TASK_CHECK(0);                           \
    } else {                                   \
      TASK_CHECK(1);                           \
    }                                          \
  } while (0)

// ---------------------------------------------------------------- values ---

// A single hand-written zone packed with every representation, then empty
// values, then two repeated user properties (a mixed list overall).
static const uint8_t s_zone_all[] = {
    0x01, 0x2A,                                      // byte = 42
    0x13, 0x12, 0x34,                                // short = 0x1234
    0x02, 0x01, 0x02, 0x03, 0x04,                    // int = 0x01020304
    0x0B, 0x80, 0x01,                                // varint = 128
    0x03, 0x00, 0x03, 'a', 'b', 'c',                 // string "abc"
    0x09, 0x00, 0x03, 0x00, 0x01, 0x00,              // binary {0,1,0}
    0x26, 0x00, 0x01, 'k', 0x00, 0x01, 'v',          // pair k -> v
    0x03, 0x00, 0x00,                                // empty string
    0x09, 0x00, 0x00,                                // empty binary
    0x26, 0x00, 0x00, 0x00, 0x00,                    // empty pair
    0x26, 0x00, 0x02, 'k', '1', 0x00, 0x02, 'v', '1',// user property k1/v1
    0x26, 0x00, 0x02, 'k', '2', 0x00, 0x02, 'v', '2' // user property k2/v2
};

struct mp_expected {
  uint8_t id;
  uint32_t iv;
  size_t key_len;   // SIZE_MAX means "must be NULL/0"
  const char *key;
  size_t val_len;   // SIZE_MAX means "must be NULL/0"
  const char *val;
  size_t end;       // offset just past this property
};

static const struct mp_expected s_exp_all[] = {
    {0x01, 42, SIZE_MAX, NULL, SIZE_MAX, NULL, 2},
    {0x13, 0x1234, SIZE_MAX, NULL, SIZE_MAX, NULL, 5},
    {0x02, 0x01020304u, SIZE_MAX, NULL, SIZE_MAX, NULL, 10},
    {0x0B, 128, SIZE_MAX, NULL, SIZE_MAX, NULL, 13},
    {0x03, 0, SIZE_MAX, NULL, 3, "abc", 19},
    {0x09, 0, SIZE_MAX, NULL, 3, "\x00\x01\x00", 25},
    {0x26, 0, 1, "k", 1, "v", 32},
    {0x03, 0, SIZE_MAX, NULL, 0, "", 35},
    {0x09, 0, SIZE_MAX, NULL, 0, "", 38},
    {0x26, 0, 0, "", 0, "", 43},
    {0x26, 0, 2, "k1", 2, "v1", 52},
    {0x26, 0, 2, "k2", 2, "v2", 61},
};

static int values_seven_types(void) {
  // Put explicit sentinels around the input; the decoder borrows from it.
  uint8_t store[sizeof(s_zone_all) + 32];
  struct mg_mqtt_message m;
  struct mg_mqtt_prop p;
  const uint8_t *base;
  size_t i, off = 0;

  memset(store, 0xCC, sizeof(store));
  memcpy(store + 16, s_zone_all, sizeof(s_zone_all));
  memset(&m, 0, sizeof(m));
  m.cmd = MQTT_CMD_PUBLISH;
  m.dgram.buf = (char *) (store + 16);
  m.dgram.len = sizeof(s_zone_all);
  m.props_start = 0;
  m.props_size = sizeof(s_zone_all);
  base = (const uint8_t *) m.dgram.buf;

  for (i = 0; i < sizeof(s_exp_all) / sizeof(s_exp_all[0]); i++) {
    const struct mp_expected *e = &s_exp_all[i];
    mp_garbage(&p);
    TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1);
    TASK_CHECK(off == e->end);
    TASK_CHECK(p.id == e->id);
    TASK_CHECK(p.iv == e->iv);
    if (e->key_len == SIZE_MAX) {
      TASK_CHECK(p.key.buf == NULL && p.key.len == 0);
    } else {
      TASK_CHECK(p.key.buf == (char *) base + (e->end - e->key_len - 2 -
                                                e->val_len) &&
                 p.key.len == e->key_len);
      TASK_CHECK(e->key_len == 0 ||
                 memcmp(p.key.buf, e->key, e->key_len) == 0);
    }
    if (e->val_len == SIZE_MAX) {
      TASK_CHECK(p.val.buf == NULL && p.val.len == 0);
    } else {
      TASK_CHECK(p.val.buf == (char *) base + (e->end - e->val_len) &&
                 p.val.len == e->val_len);
      TASK_CHECK(e->val_len == 0 ||
                 memcmp(p.val.buf, e->val, e->val_len) == 0);
    }
  }
  // Exactly at the end: clean stop leaves everything alone.
  {
    struct mg_mqtt_prop q, before;
    size_t boff = off;
    mp_garbage(&q);
    before = q;
    TASK_CHECK(mg_mqtt_prop_next(&m, &q, &off) == 0);
    TASK_CHECK(off == boff && memcmp(&q, &before, sizeof(q)) == 0);
  }
  // Sentinels around the input are untouched.
  for (i = 0; i < 16; i++) {
    TASK_CHECK(store[i] == 0xCC);
    TASK_CHECK(store[16 + sizeof(s_zone_all) + i] == 0xCC);
  }
  // The legacy iterator agrees with the new one on this zone.
  {
    struct mg_mqtt_prop q;
    size_t pos = 0, k;
    for (k = 0; k < sizeof(s_exp_all) / sizeof(s_exp_all[0]); k++) {
      mp_garbage(&q);
      pos = mg_mqtt_next_prop(&m, &q, pos);
      TASK_CHECK(pos == s_exp_all[k].end);
      TASK_CHECK(q.id == s_exp_all[k].id);
      TASK_CHECK(q.iv == s_exp_all[k].iv);
    }
    mp_garbage(&q);
    TASK_CHECK(mg_mqtt_next_prop(&m, &q, pos) == 0);
  }
  return 0;
}

static int values_empty_and_repeats(void) {
  // Empty string/binary/pair and repeated user properties, decoded in order.
  static const uint8_t zone[] = {
      0x03, 0x00, 0x00,                                // empty string
      0x09, 0x00, 0x00,                                // empty binary
      0x26, 0x00, 0x00, 0x00, 0x00,                    // empty pair
      0x26, 0x00, 0x00, 0x00, 0x02, 'v', 'v',          // empty key, "vv"
      0x26, 0x00, 0x01, 'k', 0x00, 0x00,               // "k", empty value
      0x03, 0x00, 0x05, 'h', 'e', 'l', 'l', 'o'        // "hello"
  };
  struct mg_mqtt_message m;
  struct mg_mqtt_prop p;
  size_t off = 0;
  const uint8_t *base;
  memset(&m, 0, sizeof(m));
  m.cmd = MQTT_CMD_PUBLISH;
  m.dgram.buf = (char *) zone;
  m.dgram.len = sizeof(zone);
  m.props_start = 0;
  m.props_size = sizeof(zone);
  base = (const uint8_t *) m.dgram.buf;

  mp_garbage(&p);
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1 && off == 3);
  TASK_CHECK(p.id == 0x03 && p.val.len == 0 && p.val.buf != NULL &&
             p.val.buf == (char *) base + 3 && p.key.len == 0);
  mp_garbage(&p);
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1 && off == 6);
  TASK_CHECK(p.id == 0x09 && p.val.len == 0 && p.val.buf == (char *) base + 6);
  mp_garbage(&p);
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1 && off == 11);
  TASK_CHECK(p.id == 0x26 && p.key.len == 0 && p.val.len == 0 &&
             p.key.buf == (char *) base + 9 &&
             p.val.buf == (char *) base + 11);
  mp_garbage(&p);
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1 && off == 18);
  TASK_CHECK(p.id == 0x26 && p.key.len == 0 && p.val.len == 2 &&
             memcmp(p.val.buf, "vv", 2) == 0);
  mp_garbage(&p);
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1 && off == 24);
  TASK_CHECK(p.id == 0x26 && p.key.len == 1 && memcmp(p.key.buf, "k", 1) == 0 &&
             p.val.len == 0 && p.val.buf == (char *) base + 24);
  mp_garbage(&p);
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1 && off == sizeof(zone));
  TASK_CHECK(p.id == 0x03 && p.val.len == 5 &&
             memcmp(p.val.buf, "hello", 5) == 0);
  mp_garbage(&p);
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 0 && off == sizeof(zone));
  return 0;
}

static int values_stale_fields(void) {
  // Decode repeatedly into one prop that always starts with stale, distinct
  // string views; a successful numeric decode must clear them, and a later
  // string decode must not resurrect an old key view.
  static const uint8_t zone[] = {
      0x13, 0x12, 0x34,              // short 0x1234
      0x03, 0x00, 0x03, 'a', 'b', 'c',  // string "abc"
      0x26, 0x00, 0x02, 'k', 'y', 0x00, 0x01, 'z',  // pair "ky"/"z"
      0x01, 0x11                    // byte 0x11
  };
  struct {
    uint8_t pre[16];
    struct mg_mqtt_prop p;
    uint8_t post[24];
  } g;
  char stale_key[] = "STALE-KEY";
  char stale_val[] = "STALE-VAL";
  struct mg_mqtt_message m;
  size_t i, off = 0;

  memset(&g, 0, sizeof(g));
  memset(g.pre, 0x5A, sizeof(g.pre));
  memset(g.post, 0xC3, sizeof(g.post));
  memset(&m, 0, sizeof(m));
  m.cmd = MQTT_CMD_PUBLISH;
  m.dgram.buf = (char *) zone;
  m.dgram.len = sizeof(zone);
  m.props_start = 0;
  m.props_size = sizeof(zone);

  g.p.id = 0x99;
  g.p.iv = 0xABCDEF01u;
  g.p.key.buf = stale_key;
  g.p.key.len = 9;
  g.p.val.buf = stale_val;
  g.p.val.len = 9;

  TASK_CHECK(mg_mqtt_prop_next(&m, &g.p, &off) == 1 && off == 3);
  TASK_CHECK(g.p.id == 0x13 && g.p.iv == 0x1234);
  TASK_CHECK(g.p.key.buf == NULL && g.p.key.len == 0);
  TASK_CHECK(g.p.val.buf == NULL && g.p.val.len == 0);
  TASK_CHECK(stale_key[0] == 'S' && stale_val[0] == 'S');

  TASK_CHECK(mg_mqtt_prop_next(&m, &g.p, &off) == 1 && off == 9);
  TASK_CHECK(g.p.id == 0x03 && g.p.iv == 0);
  TASK_CHECK(g.p.val.len == 3 && memcmp(g.p.val.buf, "abc", 3) == 0);
  TASK_CHECK(g.p.key.buf == NULL && g.p.key.len == 0);

  TASK_CHECK(mg_mqtt_prop_next(&m, &g.p, &off) == 1 && off == 17);
  TASK_CHECK(g.p.id == 0x26 && g.p.iv == 0);
  TASK_CHECK(g.p.key.len == 2 && memcmp(g.p.key.buf, "ky", 2) == 0);
  TASK_CHECK(g.p.val.len == 1 && memcmp(g.p.val.buf, "z", 1) == 0);

  TASK_CHECK(mg_mqtt_prop_next(&m, &g.p, &off) == 1 && off == 19);
  TASK_CHECK(g.p.id == 0x01 && g.p.iv == 0x11);
  TASK_CHECK(g.p.key.buf == NULL && g.p.key.len == 0);
  TASK_CHECK(g.p.val.buf == NULL && g.p.val.len == 0);

  for (i = 0; i < sizeof(g.pre); i++) TASK_CHECK(g.pre[i] == 0x5A);
  for (i = 0; i < sizeof(g.post); i++) TASK_CHECK(g.post[i] == 0xC3);
  return 0;
}

// ------------------------------------------------------------ boundaries ---

static int boundaries_all_truncations(void) {
  // Enumerate every truncation point for every representation at several
  // meaningful value lengths (including 0, 1, 127, 128), both at the physical
  // end of the buffer and inside a logical zone that still has payload bytes
  // after the declared section.
  static const size_t lens[] = {0, 1, 2, 127, 128};
  int type;
  for (type = 0; type < 7; type++) {
    size_t li, cut;
    for (li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
      struct mp_case c;
      struct mp_fail f;
      mp_case_one(type, lens[li], lens[li], (uint32_t) (type * 31 + li + 1),
                  &c);
      for (cut = 1; cut < c.zone_len; cut++) {
        REQUIRE_OK(mp_check_zone(&c, cut, 1, &f), &f, "type truncation logical");
        REQUIRE_OK(mp_check_zone(&c, cut, 0, &f), &f, "type truncation physical");
      }
      // The full single-property zone must decode and borrow in place.
      REQUIRE_OK(mp_check_zone(&c, c.zone_len, 1, &f), &f, "single full logical");
      REQUIRE_OK(mp_check_zone(&c, c.zone_len, 0, &f), &f, "single full physical");
    }
  }

  // Multi-property mixed lists: every truncation point, both placements.
  {
    uint32_t seed;
    size_t cut;
    for (seed = 1; seed <= 7; seed++) {
      struct mp_case c;
      struct mp_fail f;
      mp_gen(seed, (int) (seed - 1), &c);
      for (cut = 1; cut < c.zone_len; cut++) {
        REQUIRE_OK(mp_check_zone(&c, cut, 1, &f), &f, "mixed truncation logical");
        REQUIRE_OK(mp_check_zone(&c, cut, 0, &f), &f, "mixed truncation physical");
      }
    }
  }
  return 0;
}

static int boundaries_full_and_args(void) {
  // Full zones in both placements (including the untouched sentinels), plus
  // empty / exact end / past end / SIZE_MAX description checks.
  uint32_t seed;
  for (seed = 1; seed <= 9; seed++) {
    struct mp_case c;
    struct mp_fail f;
    mp_gen(seed, seed <= 7 ? (int) (seed - 1) : -1, &c);
    REQUIRE_OK(mp_check_zone(&c, c.zone_len, 1, &f), &f, "full logical");
    REQUIRE_OK(mp_check_zone(&c, c.zone_len, 0, &f), &f, "full physical");
    REQUIRE_OK(mp_check_args(&c, &f), &f, "argument limits");
  }
  return 0;
}

// --------------------------------------------------------------- framing ---

static int framing_empty_and_payload(void) {
  // Zero-length property section, empty payload, and a one-byte payload.
  static const uint8_t zero_props_empty[] = {0x30, 4, 0, 1, 'a', 0};
  static const uint8_t zero_props_1byte[] = {0x30, 5, 0, 1, 'a', 0, 'x'};
  struct mg_mqtt_message m;
  struct mg_mqtt_prop p;
  size_t off;

  TASK_CHECK(mg_mqtt_parse(zero_props_empty, sizeof(zero_props_empty), 5, &m) ==
             MQTT_OK);
  TASK_CHECK(m.props_size == 0 && m.props_start == 6);
  TASK_CHECK(m.data.len == 0 && m.dgram.len == sizeof(zero_props_empty));
  TASK_CHECK(m.topic.len == 1 && m.topic.buf[0] == 'a');
  off = 0;
  mp_garbage(&p);
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 0 && off == 0);

  TASK_CHECK(mg_mqtt_parse(zero_props_1byte, sizeof(zero_props_1byte), 5, &m) ==
             MQTT_OK);
  TASK_CHECK(m.props_size == 0 && m.data.len == 1 && m.data.buf[0] == 'x');
  off = 0;
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 0 && off == 0);
  return 0;
}

static int framing_qos_publish(void) {
  // QoS 1 and QoS 2 PUBLISH keep topic / packet id / properties / payload.
  static const uint8_t q1[] = {0x32, 9, 0, 1, 'a', 0, 1, 2, 1, 1, 'x'};
  static const uint8_t q2[] = {0x34, 9, 0, 1, 'a', 0, 2, 2, 1, 1, 'y'};
  struct mg_mqtt_message m;
  struct mg_mqtt_prop p;
  size_t off;

  TASK_CHECK(mg_mqtt_parse(q1, sizeof(q1), 5, &m) == MQTT_OK);
  TASK_CHECK(m.qos == 1 && m.id == 1 && m.props_size == 2 && m.data.len == 1);
  TASK_CHECK(m.props_start == 8 && m.data.buf[0] == 'x');
  off = 0;
  mp_garbage(&p);
  TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1 && off == 2);
  TASK_CHECK(p.id == 0x01 && p.iv == 1);

  TASK_CHECK(mg_mqtt_parse(q2, sizeof(q2), 5, &m) == MQTT_OK);
  TASK_CHECK(m.qos == 2 && m.id == 2 && m.props_size == 2 && m.data.len == 1);
  TASK_CHECK(m.data.buf[0] == 'y');
  return 0;
}

static int framing_bad_vs_incomplete(void) {
  // Complete packet with a bad property -> MALFORMED; the same packet
  // truncated below its declared length -> INCOMPLETE.
  static const uint8_t bad_id[] = {0x30, 6, 0, 1, 'a', 2, 0x7F, 0x00};
  static const uint8_t bad_string[] = {0x30, 7, 0, 1, 'a', 3, 0x03, 0x00, 0x05};
  static const uint8_t no_prop_len[] = {0x30, 3, 0, 1, 'a'};
  struct mg_mqtt_message m;

  TASK_CHECK(mg_mqtt_parse(bad_id, sizeof(bad_id), 5, &m) == MQTT_MALFORMED);
  TASK_CHECK(mg_mqtt_parse(bad_string, sizeof(bad_string), 5, &m) ==
             MQTT_MALFORMED);
  TASK_CHECK(mg_mqtt_parse(no_prop_len, sizeof(no_prop_len), 5, &m) ==
             MQTT_MALFORMED);

  TASK_CHECK(mg_mqtt_parse(bad_id, sizeof(bad_id) - 1, 5, &m) ==
             MQTT_INCOMPLETE);
  TASK_CHECK(mg_mqtt_parse(bad_id, 5, 5, &m) == MQTT_INCOMPLETE);
  TASK_CHECK(mg_mqtt_parse(bad_id, 1, 5, &m) == MQTT_INCOMPLETE);
  return 0;
}

static int framing_concatenated(void) {
  // A malformed first property must not become valid by borrowing bytes from
  // a second packet appended after it.
  uint8_t buf[64];
  static const uint8_t bad_first[] = {0x30, 7, 0, 1, 'a', 3, 0x03, 0x00, 0x05};
  static const uint8_t good_second[] = {0x30, 4, 0, 1, 'b', 0, 'h', 'e'};
  static const uint8_t good_first[] = {0x30, 7, 0, 1, 'a', 2, 1, 1, 'x'};
  struct mg_mqtt_message m;
  size_t n;

  n = 0;
  memcpy(buf + n, bad_first, sizeof(bad_first));
  n += sizeof(bad_first);
  memcpy(buf + n, good_second, sizeof(good_second));
  n += sizeof(good_second);
  TASK_CHECK(mg_mqtt_parse(buf, n, 5, &m) == MQTT_MALFORMED);

  // A valid first packet followed by arbitrary trailing bytes: only the first
  // packet is parsed and dgram.len is the first packet's true length.
  n = 0;
  memcpy(buf + n, good_first, sizeof(good_first));
  n += sizeof(good_first);
  memcpy(buf + n, good_second, sizeof(good_second));
  n += sizeof(good_second);
  TASK_CHECK(mg_mqtt_parse(buf, n, 5, &m) == MQTT_OK);
  TASK_CHECK(m.dgram.len == sizeof(good_first));
  TASK_CHECK(m.props_size == 2 && m.data.len == 1 && m.data.buf[0] == 'x');
  {
    struct mg_mqtt_prop p;
    size_t off = 0;
    TASK_CHECK(mg_mqtt_prop_next(&m, &p, &off) == 1 && p.id == 0x01 &&
               p.iv == 1 && off == 2);
  }

  // Enumerate every prefix of the valid first packet: incomplete until the
  // full length, then OK with accurate topic/property/data ranges.
  for (n = 0; n < sizeof(good_first); n++) {
    TASK_CHECK(mg_mqtt_parse(buf, n, 5, &m) == MQTT_INCOMPLETE);
  }
  TASK_CHECK(mg_mqtt_parse(buf, sizeof(good_first), 5, &m) == MQTT_OK);
  TASK_CHECK(m.dgram.len == sizeof(good_first));
  TASK_CHECK(m.topic.len == 1 && m.topic.buf[0] == 'a');
  TASK_CHECK(m.props_start == 6 && m.props_size == 2);
  TASK_CHECK(m.data.len == 1 && m.data.buf[0] == 'x');
  return 0;
}

static int framing_mqtt4_control(void) {
  // Hand-written MQTT 3.1.1 positive control: unchanged behaviour.
  static const uint8_t v4[] = {0x30, 4, 0, 1, 'a', 'x'};
  struct mg_mqtt_message m;
  TASK_CHECK(mg_mqtt_parse(v4, sizeof(v4), 4, &m) == MQTT_OK);
  TASK_CHECK(m.dgram.len == sizeof(v4));
  TASK_CHECK(m.topic.len == 1 && m.topic.buf[0] == 'a');
  TASK_CHECK(m.data.len == 1 && m.data.buf[0] == 'x');
  TASK_CHECK(m.props_size == 0 && m.props_start == 0);
  return 0;
}

// ------------------------------------------------------------------ fuzz ---

static int fuzz_fixed_seeds(void) {
  uint32_t seed, executed = 0;
  for (seed = 1; seed <= 300; seed++) {
    struct mp_case c;
    struct mp_fail f;
    mp_case_from_seed(seed, &c);
    executed++;
    if (!mp_run_case(&c, &f)) {
      fprintf(stderr, "FUZZ_SEED seed=%u at=%lu rule_line=%d msg=%s snippet=%s\n",
              (unsigned) seed, (unsigned long) f.at, f.line, f.msg, f.snippet);
      mp_report("fuzz_fixed_seeds", &f);
      TASK_CHECK(0);
    } else {
      TASK_CHECK(1);
    }
  }
  fprintf(stderr, "FUZZ_EXECUTED seeds=%u\n", (unsigned) executed);
  TASK_CHECK(executed >= 256);
  return 0;
}

static int fuzz_mapped_inputs(void) {
  // Deterministic byte inputs mapped through the bounded model adapter.
  static const uint8_t inputs[][8] = {
      {0x00}, {0x01}, {0x02}, {0x7F}, {0x80}, {0xFF}, {0x00, 0x01, 0x02},
      {0xFF, 0x00, 0x80}, {0x10, 0x20, 0x30, 0x40}, {0xAA, 0x55, 0xAA}};
  size_t i;
  unsigned executed = 0;
  for (i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
    struct mp_case c;
    struct mp_fail f;
    mp_case_from_data(inputs[i], sizeof(inputs[0]), &c);
    executed++;
    if (!mp_run_case(&c, &f)) {
      fprintf(stderr, "FUZZ_INPUT index=%lu msg=%s snippet=%s\n",
              (unsigned long) i, f.msg, f.snippet);
      mp_report("fuzz_mapped_inputs", &f);
      TASK_CHECK(0);
    } else {
      TASK_CHECK(1);
    }
  }
  fprintf(stderr, "FUZZ_MAPPED_EXECUTED inputs=%u\n", executed);
  TASK_CHECK(executed > 0);
  return 0;
}

// --------------------------------------------------------- registration ---

const struct task_case task_cases[] = {
    {"values", "seven_types", values_seven_types},
    {"values", "empty_and_repeats", values_empty_and_repeats},
    {"values", "stale_fields", values_stale_fields},
    {"boundaries", "all_truncations", boundaries_all_truncations},
    {"boundaries", "full_and_args", boundaries_full_and_args},
    {"framing", "empty_and_payload", framing_empty_and_payload},
    {"framing", "qos_publish", framing_qos_publish},
    {"framing", "bad_vs_incomplete", framing_bad_vs_incomplete},
    {"framing", "concatenated", framing_concatenated},
    {"framing", "mqtt4_control", framing_mqtt4_control},
    {"fuzz", "fixed_seeds", fuzz_fixed_seeds},
    {"fuzz", "mapped_inputs", fuzz_mapped_inputs},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
