// Shared checking core for the MQTT5 property decoder contract.
//
// Included by test/mqtt_property_contract.c (driven by
// task_support/driver.c) and by test/mqtt_property_fuzz.c
// (LLVMFuzzerTestOneInput). It contains an independent model of the MQTT5
// property wire format: expected values are generated and encoded here
// without calling any of the library's encode or decode routines.
#ifndef MQTT_PROPERTY_CORE_H
#define MQTT_PROPERTY_CORE_H

#include <stdio.h>
#include <string.h>

#include "../task_support/check.h"

#ifndef MQTT_PROP_CORE_MAX_INPUT
#define MQTT_PROP_CORE_MAX_INPUT 4096
#endif
#define CORE_MAX_INPUT MQTT_PROP_CORE_MAX_INPUT
#define CORE_MAX_PROPS 8
#define CORE_MAX_KEY 64
#define CORE_MAX_VAL 200
#define CORE_SEC_MAX 4096
#define CORE_PAD 32
#define CORE_SENTINEL 0xA5A5A5A5u

static unsigned core_checks;
// Execution accounting: a sanitizer-clean run alone does not prove that any
// property field was actually compared, so the drivers print these counters.
static unsigned core_runs;
static unsigned core_decodes;
static unsigned core_multiprop;
static unsigned core_wide;
// Set while shrinking a failing input: assertions still return early but do
// not call task_check() (no diagnostics and no counter noise).
static int core_quiet;

#define CORE_CHECK(x)                                    \
  do {                                                   \
    if (core_quiet) {                                    \
      if (!(x)) return 1;                                \
    } else {                                             \
      ++core_checks;                                     \
      if (!task_check(!!(x), #x, __FILE__, __LINE__)) {  \
        return 1;                                        \
      }                                                  \
    }                                                    \
  } while (0)

struct core_prop {
  uint8_t type;
  uint8_t id;
  uint32_t iv;
  uint8_t key[CORE_MAX_KEY];
  size_t klen;
  uint8_t val[CORE_MAX_VAL];
  size_t vlen;
  size_t off;   // offset of the property id within the section
  size_t size;  // total encoded size including the id
};

static uint32_t core_rng(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x ? x : 0x12345678u;
  return *state;
}

static uint8_t core_id_for_type(uint8_t type) {
  switch (type) {
    case MQTT_PROP_TYPE_BYTE: return MQTT_PROP_PAYLOAD_FORMAT_INDICATOR;
    case MQTT_PROP_TYPE_INT: return MQTT_PROP_MESSAGE_EXPIRY_INTERVAL;
    case MQTT_PROP_TYPE_STRING: return MQTT_PROP_CONTENT_TYPE;
    case MQTT_PROP_TYPE_BINARY_DATA: return MQTT_PROP_CORRELATION_DATA;
    case MQTT_PROP_TYPE_VARIABLE_INT: return MQTT_PROP_SUBSCRIPTION_IDENTIFIER;
    case MQTT_PROP_TYPE_SHORT: return MQTT_PROP_SERVER_KEEP_ALIVE;
    default: return MQTT_PROP_USER_PROPERTY;
  }
}

// A length biased towards the one-byte/two-byte length-prefix boundary.
static size_t core_len(uint32_t *s, size_t cap) {
  static const size_t edge[] = {0, 1, 2, 3, 125, 126, 127, 128, 129, 130};
  uint32_t r = core_rng(s);
  size_t e = edge[(r >> 1) % (sizeof(edge) / sizeof(edge[0]))];
  if (r & 1) return e <= cap ? e : cap;
  return (size_t) ((r >> 1) % (uint32_t) (cap + 1));
}

static uint32_t core_varint(uint32_t *s) {
  static const uint32_t edge[] = {0,      1,       2,       126,
                                  127,    128,     129,     16383,
                                  16384,  2097151, 2097152, 268435455u};
  uint32_t r = core_rng(s);
  if (r & 1) return edge[(r >> 1) % (sizeof(edge) / sizeof(edge[0]))];
  return (r >> 1) % 268435456u;
}

static size_t core_put_varint(uint8_t *buf, size_t cap, size_t pos,
                              uint32_t value) {
  do {
    uint8_t b = (uint8_t) (value % 128);
    value /= 128;
    if (value > 0) b |= 0x80;
    if (pos >= cap) return 0;
    buf[pos++] = b;
  } while (value > 0);
  return pos;
}

static size_t core_put_u16(uint8_t *buf, size_t cap, size_t pos, size_t value) {
  if (pos + 2 > cap || value > 0xffffu) return 0;
  buf[pos] = (uint8_t) ((value >> 8) & 0xff);
  buf[pos + 1] = (uint8_t) (value & 0xff);
  return pos + 2;
}

static size_t core_put_blob(uint8_t *buf, size_t cap, size_t pos,
                            const uint8_t *data, size_t len) {
  if (pos + len > cap) return 0;
  if (len > 0) memcpy(buf + pos, data, len);
  return pos + len;
}

// Build a bounded, deterministic property model from raw bytes.
static int core_generate(const uint8_t *data, size_t size,
                         struct core_prop *props, size_t *count, uint8_t *sec,
                         size_t *sec_len, uint32_t *seed_out) {
  uint32_t seed = 2166136261u;
  size_t i, n, pos = 0, j;
  uint8_t start;

  for (j = 0; j < size; j++) seed = (seed ^ data[j]) * 16777619u;
  seed ^= (uint32_t) size * 2654435761u;
  if (seed == 0) seed = 1;
  *seed_out = seed;

  n = 1 + (size > 0 ? (data[0] % CORE_MAX_PROPS) : 0);
  if (n > CORE_MAX_PROPS) n = CORE_MAX_PROPS;
  start = (uint8_t) ((size > 1 ? data[1] : seed) % 7);

  for (i = 0; i < n; i++) {
    struct core_prop *p = &props[i];
    memset(p, 0, sizeof(*p));
    // Rotate through all seven representations so mixed lists appear.
    p->type = (uint8_t) ((start + i) % 7);
    p->id = core_id_for_type(p->type);
    p->off = pos;
    switch (p->type) {
      case MQTT_PROP_TYPE_BYTE:
        p->iv = core_rng(&seed) & 0xffu;
        if (pos + 2 > CORE_SEC_MAX) return 1;
        sec[pos++] = p->id, sec[pos++] = (uint8_t) p->iv;
        break;
      case MQTT_PROP_TYPE_SHORT:
        p->iv = core_rng(&seed) & 0xffffu;
        if (pos + 3 > CORE_SEC_MAX) return 1;
        sec[pos++] = p->id;
        pos = core_put_u16(sec, CORE_SEC_MAX, pos, p->iv);
        if (pos == 0) return 1;
        break;
      case MQTT_PROP_TYPE_INT:
        p->iv = core_rng(&seed);
        if (pos + 5 > CORE_SEC_MAX) return 1;
        sec[pos++] = p->id;
        sec[pos++] = (uint8_t) ((p->iv >> 24) & 0xff);
        sec[pos++] = (uint8_t) ((p->iv >> 16) & 0xff);
        sec[pos++] = (uint8_t) ((p->iv >> 8) & 0xff);
        sec[pos++] = (uint8_t) (p->iv & 0xff);
        break;
      case MQTT_PROP_TYPE_VARIABLE_INT:
        p->iv = core_varint(&seed);
        if (pos + 5 > CORE_SEC_MAX) return 1;
        sec[pos++] = p->id;
        pos = core_put_varint(sec, CORE_SEC_MAX, pos, p->iv);
        if (pos == 0) return 1;
        break;
      case MQTT_PROP_TYPE_STRING:
      case MQTT_PROP_TYPE_BINARY_DATA:
        p->vlen = core_len(&seed, CORE_MAX_VAL - 1);
        for (j = 0; j < p->vlen; j++) p->val[j] = (uint8_t) core_rng(&seed);
        // Binary data must preserve embedded zero bytes.
        if (p->type == MQTT_PROP_TYPE_BINARY_DATA && p->vlen > 2)
          p->val[p->vlen / 2] = 0;
        if (pos + 3 + p->vlen > CORE_SEC_MAX) return 1;
        sec[pos++] = p->id;
        pos = core_put_u16(sec, CORE_SEC_MAX, pos, p->vlen);
        if (pos == 0) return 1;
        pos = core_put_blob(sec, CORE_SEC_MAX, pos, p->val, p->vlen);
        if (pos == 0) return 1;
        break;
      case MQTT_PROP_TYPE_STRING_PAIR:
        p->klen = core_len(&seed, CORE_MAX_KEY - 1);
        p->vlen = core_len(&seed, CORE_MAX_VAL - 1);
        for (j = 0; j < p->klen; j++) p->key[j] = (uint8_t) core_rng(&seed);
        for (j = 0; j < p->vlen; j++) p->val[j] = (uint8_t) core_rng(&seed);
        if (pos + 5 + p->klen + p->vlen > CORE_SEC_MAX) return 1;
        sec[pos++] = p->id;
        pos = core_put_u16(sec, CORE_SEC_MAX, pos, p->klen);
        if (pos == 0) return 1;
        pos = core_put_blob(sec, CORE_SEC_MAX, pos, p->key, p->klen);
        if (pos == 0) return 1;
        pos = core_put_u16(sec, CORE_SEC_MAX, pos, p->vlen);
        if (pos == 0) return 1;
        pos = core_put_blob(sec, CORE_SEC_MAX, pos, p->val, p->vlen);
        if (pos == 0) return 1;
        break;
      default:
        return 1;
    }
    p->size = pos - p->off;
  }
  *count = n;
  *sec_len = pos;
  return 0;
}

struct core_guard {
  uint32_t pre[4];
  struct mg_mqtt_prop p;
  uint32_t post[4];
};

static void core_guard_init(struct core_guard *g, uint8_t fill) {
  size_t i;
  for (i = 0; i < 4; i++) g->pre[i] = g->post[i] = CORE_SENTINEL;
  memset(&g->p, fill, sizeof(g->p));
}

static int core_guard_ok(const struct core_guard *g) {
  size_t i;
  for (i = 0; i < 4; i++)
    if (g->pre[i] != CORE_SENTINEL || g->post[i] != CORE_SENTINEL) return 0;
  return 1;
}

static int core_same_prop(const struct mg_mqtt_prop *a,
                          const struct mg_mqtt_prop *b) {
  return a->id == b->id && a->iv == b->iv && a->key.buf == b->key.buf &&
         a->key.len == b->key.len && a->val.buf == b->val.buf &&
         a->val.len == b->val.len;
}

// Compare one decoded property against the independent model entry.
static int core_check_prop(const uint8_t *storage, size_t prefix,
                           const struct core_prop *m, size_t next_off,
                           const struct mg_mqtt_prop *p) {
  const uint8_t *at = storage + prefix + m->off;
  CORE_CHECK(p->id == m->id);
  switch (m->type) {
    case MQTT_PROP_TYPE_BYTE:
    case MQTT_PROP_TYPE_SHORT:
    case MQTT_PROP_TYPE_INT:
    case MQTT_PROP_TYPE_VARIABLE_INT:
      CORE_CHECK(p->iv == m->iv);
      CORE_CHECK(p->key.len == 0 && p->key.buf == NULL);
      CORE_CHECK(p->val.len == 0 && p->val.buf == NULL);
      break;
    case MQTT_PROP_TYPE_STRING:
    case MQTT_PROP_TYPE_BINARY_DATA:
      CORE_CHECK(p->iv == 0);
      CORE_CHECK(p->key.len == 0 && p->key.buf == NULL);
      CORE_CHECK(p->val.len == m->vlen);
      CORE_CHECK(p->val.buf == (const char *) at + 3);
      CORE_CHECK(m->vlen == 0 || memcmp(p->val.buf, m->val, m->vlen) == 0);
      break;
    case MQTT_PROP_TYPE_STRING_PAIR:
      CORE_CHECK(p->iv == 0);
      CORE_CHECK(p->key.len == m->klen);
      CORE_CHECK(p->key.buf == (const char *) at + 3);
      CORE_CHECK(m->klen == 0 || memcmp(p->key.buf, m->key, m->klen) == 0);
      CORE_CHECK(p->val.len == m->vlen);
      CORE_CHECK(p->val.buf == (const char *) at + 3 + m->klen + 2);
      CORE_CHECK(m->vlen == 0 || memcmp(p->val.buf, m->val, m->vlen) == 0);
      break;
    default:
      CORE_CHECK(0);
      break;
  }
  CORE_CHECK(next_off == m->off + m->size);
  return 0;
}

static int core_section_ok(const uint8_t *storage, size_t prefix, size_t sec_len,
                           size_t pad) {
  size_t i;
  for (i = 0; i < pad; i++)
    if (storage[prefix + sec_len + i] != (uint8_t) (0xc0 + i)) return 0;
  return 1;
}

// Exercise the decoder over one generated model. Returns 0 when every
// assertion held, 1 otherwise.
static int core_run(const uint8_t *input, size_t input_size) {
  struct core_prop model[CORE_MAX_PROPS];
  struct core_guard g;
  struct mg_mqtt_prop before;
  uint8_t in[CORE_MAX_INPUT];
  uint8_t sec[CORE_SEC_MAX];
  uint8_t storage[CORE_SEC_MAX + 2 * CORE_PAD];
  struct mg_mqtt_message msg;
  uint32_t seed = 0;
  size_t size, count, sec_len, prefix, i, j, pad = CORE_PAD;
  size_t input_copy;
  int rc;

  ++core_checks;
  input_copy = input_size < CORE_MAX_INPUT ? input_size : CORE_MAX_INPUT;
  size = input_copy;
  if (size > 0) memcpy(in, input, size);

  if (core_generate(in, size, model, &count, sec, &sec_len, &seed) != 0)
    return 0;  // bounded rejection: model too large to encode

  ++core_runs;
  if (count > 1) ++core_multiprop;
  for (i = 0; i < count; i++) {
    if (model[i].vlen >= 127 || model[i].klen >= 127 ||
        (model[i].type == MQTT_PROP_TYPE_VARIABLE_INT && model[i].iv >= 127) ||
        sec_len >= 127)
      core_wide = 1;
  }

  prefix = size > 2 ? (size_t) (in[2] % 8) : 0;
  if (prefix + sec_len + 2 * pad > sizeof(storage))
    pad = (sizeof(storage) - prefix - sec_len) / 2;
  memset(storage, 0x5a, sizeof(storage));
  memcpy(storage + prefix, sec, sec_len);
  for (i = 0; i < pad; i++) storage[prefix + sec_len + i] = (uint8_t) (0xc0 + i);

  memset(&msg, 0, sizeof(msg));
  msg.dgram.buf = (char *) storage;
  msg.dgram.len = prefix + sec_len + pad;
  msg.props_start = prefix;
  msg.props_size = sec_len;

  // 1. Walk the section, comparing values, ranges and progress.
  core_guard_init(&g, 0x3c);
  j = 0;
  for (i = 0; i < count; i++) {
    before = g.p;
    rc = mg_mqtt_prop_next(&msg, &g.p, &j);
    CORE_CHECK(rc == 1);
    if (rc != 1) return 1;
    ++core_decodes;
    CORE_CHECK(core_guard_ok(&g));
    if (core_check_prop(storage, prefix, &model[i], j, &g.p)) return 1;
  }
  before = g.p;
  rc = mg_mqtt_prop_next(&msg, &g.p, &j);
  CORE_CHECK(rc == 0);
  CORE_CHECK(core_same_prop(&g.p, &before));
  CORE_CHECK(core_guard_ok(&g));
  CORE_CHECK(j == sec_len);
  CORE_CHECK(core_section_ok(storage, prefix, sec_len, pad));
  CORE_CHECK(size == 0 || memcmp(in, input, size) == 0);  // input untouched

  // 2. Every truncation inside every property, with the logical section end
  //    followed by payload bytes (msg->dgram.len stays larger).
  for (i = 0; i < count; i++) {
    size_t t;
    for (t = 1; t < model[i].size; t++) {
      memset(&msg, 0, sizeof(msg));
      msg.dgram.buf = (char *) storage;
      msg.dgram.len = prefix + sec_len + pad;
      msg.props_start = prefix;
      msg.props_size = model[i].off + t;
      core_guard_init(&g, 0x3c);
      before = g.p;
      j = model[i].off;
      rc = mg_mqtt_prop_next(&msg, &g.p, &j);
      CORE_CHECK(rc == -1);
      CORE_CHECK(core_same_prop(&g.p, &before));
      CORE_CHECK(core_guard_ok(&g));
      CORE_CHECK(j == model[i].off);
      CORE_CHECK(core_section_ok(storage, prefix, sec_len, pad));
    }
  }

  // 3. Same truncations with the logical end also at the physical buffer end.
  for (i = 0; i < count; i++) {
    size_t t;
    for (t = 1; t < model[i].size; t += (model[i].size > 12 ? 5 : 1)) {
      memset(&msg, 0, sizeof(msg));
      msg.dgram.buf = (char *) storage;
      msg.dgram.len = prefix + model[i].off + t;
      msg.props_start = prefix;
      msg.props_size = model[i].off + t;
      core_guard_init(&g, 0x3c);
      before = g.p;
      j = model[i].off;
      rc = mg_mqtt_prop_next(&msg, &g.p, &j);
      CORE_CHECK(rc == -1);
      CORE_CHECK(core_same_prop(&g.p, &before));
      CORE_CHECK(core_guard_ok(&g));
      CORE_CHECK(j == model[i].off);
    }
  }

  // 4. Exactly at each property end: 0, fields and offset unchanged. Also
  //    decode the property in a section that ends right after it.
  for (i = 0; i < count; i++) {
    memset(&msg, 0, sizeof(msg));
    msg.dgram.buf = (char *) storage;
    msg.dgram.len = prefix + sec_len + pad;
    msg.props_start = prefix;
    msg.props_size = model[i].off + model[i].size;
    core_guard_init(&g, 0x3c);
    before = g.p;
    j = model[i].off;
    rc = mg_mqtt_prop_next(&msg, &g.p, &j);
    CORE_CHECK(rc == 1);
    if (rc == 1) {
      if (core_check_prop(storage, prefix, &model[i], j, &g.p)) return 1;
      CORE_CHECK(j == msg.props_size);
    }
    // Exactly at the end of the clipped section: 0, nothing changes.
    core_guard_init(&g, 0x3c);
    before = g.p;
    j = msg.props_size;  // exactly one past the last byte of this property
    rc = mg_mqtt_prop_next(&msg, &g.p, &j);
    CORE_CHECK(rc == 0);
    CORE_CHECK(core_same_prop(&g.p, &before));
    CORE_CHECK(core_guard_ok(&g));
    CORE_CHECK(j == msg.props_size);
    // One past the described area is out of range, not end-of-section.
    j = msg.props_size + 1;
    rc = mg_mqtt_prop_next(&msg, &g.p, &j);
    CORE_CHECK(rc == -1);
    CORE_CHECK(core_same_prop(&g.p, &before));
    CORE_CHECK(j == msg.props_size + 1);
  }

  // 5. Section claims more bytes than the dgram owns: invalid description.
  memset(&msg, 0, sizeof(msg));
  msg.dgram.buf = (char *) storage;
  msg.dgram.len = prefix + sec_len;
  msg.props_start = prefix;
  msg.props_size = sec_len + 1;
  core_guard_init(&g, 0x3c);
  before = g.p;
  j = 0;
  rc = mg_mqtt_prop_next(&msg, &g.p, &j);
  CORE_CHECK(rc == -1);
  CORE_CHECK(core_same_prop(&g.p, &before));
  CORE_CHECK(j == 0);
  CORE_CHECK(core_guard_ok(&g));

  // 6. Unknown property id (0xff) and NULL arguments.
  storage[prefix] = (uint8_t) 0xff;
  memset(&msg, 0, sizeof(msg));
  msg.dgram.buf = (char *) storage;
  msg.dgram.len = prefix + 1;
  msg.props_start = prefix;
  msg.props_size = 1;
  core_guard_init(&g, 0x3c);
  before = g.p;
  j = 0;
  rc = mg_mqtt_prop_next(&msg, &g.p, &j);
  CORE_CHECK(rc == -1);
  CORE_CHECK(core_same_prop(&g.p, &before));
  CORE_CHECK(core_guard_ok(&g));
  CORE_CHECK(mg_mqtt_prop_next(NULL, &g.p, &j) == -1);
  CORE_CHECK(mg_mqtt_prop_next(&msg, NULL, &j) == -1);
  CORE_CHECK(mg_mqtt_prop_next(&msg, &g.p, NULL) == -1);

  // 7. Empty section, with a NULL or present buffer.
  memset(&msg, 0, sizeof(msg));
  msg.dgram.buf = NULL;
  msg.dgram.len = 0;
  msg.props_start = 0;
  msg.props_size = 0;
  core_guard_init(&g, 0x3c);
  before = g.p;
  j = 0;
  CORE_CHECK(mg_mqtt_prop_next(&msg, &g.p, &j) == 0);
  CORE_CHECK(core_same_prop(&g.p, &before));
  CORE_CHECK(j == 0);

  return 0;
}

// Report a failing seed together with the relevant input bytes.
static void core_report_failure(uint32_t seed, const uint8_t *data, size_t size,
                                size_t max_bytes) {
  size_t i, n = size < max_bytes ? size : max_bytes;
  fprintf(stderr, "FUZZ_FAIL seed=%u size=%lu bytes=", (unsigned) seed,
          (unsigned long) size);
  for (i = 0; i < n; i++) fprintf(stderr, "%02x", data[i]);
  fprintf(stderr, "\n");
}

// Run the core without diagnostics; used to check a candidate input quickly.
#if defined(__GNUC__) || defined(__clang__)
#define CORE_MAYBE_UNUSED __attribute__((unused))
#else
#define CORE_MAYBE_UNUSED
#endif

CORE_MAYBE_UNUSED static int core_fails_quiet(const uint8_t *data,
                                              size_t size) {
  int rc;
  core_quiet = 1;
  rc = core_run(data, size);
  core_quiet = 0;
  return rc;
}

// Shrink a failing input to a minimal relevant byte fragment.
// Returns the reduced length for the bytes in `work`; 0 means the original
// input is already minimal.
CORE_MAYBE_UNUSED static size_t core_shrink(uint8_t *work, size_t size) {
  size_t step, len = size;
  for (step = (len + 1) / 2; step > 0; step /= 2) {
    size_t start = 0;
    while (start + step <= len) {
      memmove(work + start, work + start + step, len - start - step);
      if (core_fails_quiet(work, len - step)) {
        len -= step;
      } else {
        memmove(work + start + step, work + start, len - start - step);
        start += step;
      }
    }
  }
  return len == size ? 0 : len;
}

#endif
