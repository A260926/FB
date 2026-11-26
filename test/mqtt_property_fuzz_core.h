// Shared checking core for the MQTT5 property decoder tests.
//
// This header is included by both test/mqtt_property_contract.c (the `fuzz`
// group) and test/mqtt_property_fuzz.c (the standalone libFuzzer entry point).
// It contains an independent model of a legal MQTT5 property list: property
// values are generated here, encoded here with this file's own minimal
// big-endian encoder, and the expected field values/ranges/offsets are kept
// here. Nothing in this file calls the library's property encoder or decoder
// to build expectations.
//
// Every case is turned into a bounded local byte buffer (well under 4096
// bytes): a leading sentinel, the encoded property zone, a short logical
// payload tail, and a trailing sentinel. No network access and no file access
// happens here.

#ifndef MP_FUZZ_CORE_H
#define MP_FUZZ_CORE_H

#include "mongoose.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MP_MAX_PROPS 6
#define MP_MAX_VAL 256
#define MP_ZONE_MAX 3200
#define MP_TAIL_MAX 32
#define MP_TAIL_LEN 24
#define MP_GUARD 16

struct mp_spec {
  int type;            // MQTT_PROP_TYPE_* value
  uint8_t id;          // property identifier byte
  uint32_t iv;         // integer payload for numeric types
  uint8_t key[MP_MAX_VAL];  // key bytes for user properties
  uint8_t val[MP_MAX_VAL];  // value bytes for string/binary/pair
  size_t key_len;
  size_t val_len;
  size_t start;    // offset of the identifier byte inside the zone
  size_t end;      // offset just past the encoded property
  size_t key_off;  // offset of the first key byte (pair only)
  size_t val_off;  // offset of the first value byte (string/binary/pair)
};

struct mp_case {
  struct mp_spec props[MP_MAX_PROPS];
  size_t nprops;
  uint32_t seed;
  uint8_t zone[MP_ZONE_MAX];
  size_t zone_len;
  uint8_t tail[MP_TAIL_MAX];
  size_t tail_len;
};

struct mp_fail {
  int ok;          // 1 while the case is still good
  int line;        // line of the first failed expectation
  unsigned seed;   // seed (or hash) of the case
  size_t at;       // property offset where the failure happened
  char msg[200];
  char snippet[80];  // hex of the relevant encoded property bytes
};

static uint32_t mp_rng_next(uint32_t *s) {
  uint32_t x = *s ? *s : 1u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *s = x;
  return x;
}

static void mp_put_u16(uint8_t *p, size_t v) {
  p[0] = (uint8_t) ((v >> 8) & 0xFF);
  p[1] = (uint8_t) (v & 0xFF);
}

static void mp_put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t) ((v >> 24) & 0xFF);
  p[1] = (uint8_t) ((v >> 16) & 0xFF);
  p[2] = (uint8_t) ((v >> 8) & 0xFF);
  p[3] = (uint8_t) (v & 0xFF);
}

// Minimal base-128 encoding, independent of the library implementation.
static size_t mp_enc_varint(uint8_t *out, uint32_t v) {
  size_t n = 0;
  do {
    uint8_t b = (uint8_t) (v & 0x7F);
    v >>= 7;
    if (v != 0) b |= 0x80;
    out[n++] = b;
  } while (v != 0 && n < 4);
  return n;
}

static void mp_fill(uint8_t *dst, size_t len, uint32_t *rs, int binary) {
  size_t i;
  for (i = 0; i < len; i++) {
    uint32_t r = mp_rng_next(rs);
    if (binary && (i % 4) == 0)
      dst[i] = 0;  // Binary payloads must preserve embedded zero bytes
    else if (binary)
      dst[i] = (uint8_t) (r & 0xFF);
    else
      dst[i] = (uint8_t) ('a' + (r % 26));
  }
}

// A bounded length table that deliberately crosses both sides of 127 and 128.
static size_t mp_pick_len(uint32_t r) {
  static const size_t tab[] = {0, 1, 2, 3, 8, 17, 126, 127, 128, 129, 200, 255};
  return tab[r % (sizeof(tab) / sizeof(tab[0]))];
}

static uint8_t mp_id_for(int type, uint32_t r) {
  switch (type) {
    case MQTT_PROP_TYPE_BYTE: {
      static const uint8_t a[] = {0x01, 0x17, 0x19, 0x24, 0x25, 0x28, 0x29, 0x2A};
      return a[r % (sizeof(a) / sizeof(a[0]))];
    }
    case MQTT_PROP_TYPE_SHORT: {
      static const uint8_t a[] = {0x13, 0x21, 0x22, 0x23};
      return a[r % (sizeof(a) / sizeof(a[0]))];
    }
    case MQTT_PROP_TYPE_INT: {
      static const uint8_t a[] = {0x02, 0x11, 0x18, 0x27};
      return a[r % (sizeof(a) / sizeof(a[0]))];
    }
    case MQTT_PROP_TYPE_VARIABLE_INT:
      return 0x0B;
    case MQTT_PROP_TYPE_STRING: {
      static const uint8_t a[] = {0x03, 0x08, 0x12, 0x15, 0x1A, 0x1C, 0x1F};
      return a[r % (sizeof(a) / sizeof(a[0]))];
    }
    case MQTT_PROP_TYPE_BINARY_DATA: {
      static const uint8_t a[] = {0x09, 0x16};
      return a[r % (sizeof(a) / sizeof(a[0]))];
    }
    case MQTT_PROP_TYPE_STRING_PAIR:
      return 0x26;
    default:
      return 0x01;
  }
}

// Independent encoder: filled-in specs in, canonical zone bytes and the
// expected offsets/ranges out.
static void mp_encode(struct mp_case *c) {
  size_t z = 0, i;
  for (i = 0; i < c->nprops; i++) {
    struct mp_spec *s = &c->props[i];
    s->start = z;
    s->key_off = 0;
    s->val_off = 0;
    c->zone[z++] = s->id;
    switch (s->type) {
      case MQTT_PROP_TYPE_BYTE:
        c->zone[z++] = (uint8_t) (s->iv & 0xFF);
        break;
      case MQTT_PROP_TYPE_SHORT:
        mp_put_u16(c->zone + z, (size_t) s->iv);
        z += 2;
        break;
      case MQTT_PROP_TYPE_INT:
        mp_put_u32(c->zone + z, s->iv);
        z += 4;
        break;
      case MQTT_PROP_TYPE_VARIABLE_INT:
        z += mp_enc_varint(c->zone + z, s->iv);
        break;
      case MQTT_PROP_TYPE_STRING:
      case MQTT_PROP_TYPE_BINARY_DATA:
        mp_put_u16(c->zone + z, s->val_len);
        z += 2;
        s->val_off = z;
        memcpy(c->zone + z, s->val, s->val_len);
        z += s->val_len;
        break;
      case MQTT_PROP_TYPE_STRING_PAIR:
        mp_put_u16(c->zone + z, s->key_len);
        z += 2;
        s->key_off = z;
        memcpy(c->zone + z, s->key, s->key_len);
        z += s->key_len;
        mp_put_u16(c->zone + z, s->val_len);
        z += 2;
        s->val_off = z;
        memcpy(c->zone + z, s->val, s->val_len);
        z += s->val_len;
        break;
      default:
        break;
    }
    s->end = z;
  }
  c->zone_len = z;
  for (i = 0; i < MP_TAIL_MAX; i++) c->tail[i] = (uint8_t) ('A' + (i % 26));
  c->tail_len = MP_TAIL_LEN;
}

static void mp_gen(uint32_t seed, int forced_type, struct mp_case *c) {
  uint32_t rs = seed ? seed : 1u;
  size_t i;
  memset(c, 0, sizeof(*c));
  c->seed = seed;
  c->nprops = forced_type >= 0 ? 3 : (size_t) (2 + (mp_rng_next(&rs) % 4));
  for (i = 0; i < c->nprops; i++) {
    struct mp_spec *s = &c->props[i];
    int type = (i == 0 && forced_type >= 0) ? forced_type
                                            : (int) (mp_rng_next(&rs) % 7);
    s->type = type;
    s->id = mp_id_for(type, mp_rng_next(&rs));
    switch (type) {
      case MQTT_PROP_TYPE_BYTE:
        s->iv = mp_rng_next(&rs) % 256;
        break;
      case MQTT_PROP_TYPE_SHORT:
        s->iv = mp_rng_next(&rs) % 65536;
        break;
      case MQTT_PROP_TYPE_INT:
        s->iv = mp_rng_next(&rs);
        break;
      case MQTT_PROP_TYPE_VARIABLE_INT: {
        static const uint32_t v[] = {0,     1,      127,     128,
                                     16383, 16384,  2097151, 2097152,
                                     0x0FFFFFFF};
        s->iv = v[mp_rng_next(&rs) % (sizeof(v) / sizeof(v[0]))];
        break;
      }
      case MQTT_PROP_TYPE_STRING:
      case MQTT_PROP_TYPE_BINARY_DATA: {
        size_t len;
        if (forced_type >= 0) {
          static const size_t l[] = {0, 127, 128};
          len = l[i % (sizeof(l) / sizeof(l[0]))];
        } else {
          len = mp_pick_len(mp_rng_next(&rs));
        }
        s->val_len = len;
        mp_fill(s->val, len, &rs, type == MQTT_PROP_TYPE_BINARY_DATA);
        break;
      }
      case MQTT_PROP_TYPE_STRING_PAIR: {
        size_t kl, vl;
        if (forced_type >= 0) {
          static const size_t l[] = {0, 127, 128};
          kl = l[i % (sizeof(l) / sizeof(l[0]))];
          vl = l[(i + 1) % (sizeof(l) / sizeof(l[0]))];
        } else {
          kl = mp_pick_len(mp_rng_next(&rs));
          vl = mp_pick_len(mp_rng_next(&rs));
        }
        s->key_len = kl;
        s->val_len = vl;
        mp_fill(s->key, kl, &rs, 0);
        mp_fill(s->val, vl, &rs, 0);
        break;
      }
      default:
        break;
    }
  }
  mp_encode(c);
}

// Seeds 1..7 force one representative of each property type (and use lengths
// 0/127/128 in a multi-property list); every other seed mixes types at random.
static void mp_case_from_seed(uint32_t seed, struct mp_case *c) {
  int forced = (seed >= 1 && seed <= 7) ? (int) (seed - 1) : -1;
  mp_gen(seed, forced, c);
}

// A single-property case of a given type and value/key/value lengths, with
// deterministic contents derived from `tag`. Used to enumerate every
// truncation position for each representation independently.
static void mp_case_one(int type, size_t key_len, size_t val_len, uint32_t tag,
                        struct mp_case *c) {
  uint32_t rs = tag ? tag : 1u;
  struct mp_spec *s;
  memset(c, 0, sizeof(*c));
  c->seed = tag;
  c->nprops = 1;
  s = &c->props[0];
  s->type = type;
  s->id = mp_id_for(type, tag);
  switch (type) {
    case MQTT_PROP_TYPE_BYTE:
      s->iv = tag & 0xFFu;
      break;
    case MQTT_PROP_TYPE_SHORT:
      s->iv = tag & 0xFFFFu;
      break;
    case MQTT_PROP_TYPE_INT:
      s->iv = tag;
      break;
    case MQTT_PROP_TYPE_VARIABLE_INT:
      s->iv = tag & 0x0FFFFFFFu;
      break;
    case MQTT_PROP_TYPE_STRING:
    case MQTT_PROP_TYPE_BINARY_DATA:
      s->val_len = val_len;
      mp_fill(s->val, val_len, &rs, type == MQTT_PROP_TYPE_BINARY_DATA);
      break;
    case MQTT_PROP_TYPE_STRING_PAIR:
      s->key_len = key_len;
      s->val_len = val_len;
      mp_fill(s->key, key_len, &rs, 0);
      mp_fill(s->val, val_len, &rs, 0);
      break;
    default:
      break;
  }
  mp_encode(c);
}

// Maps arbitrary fuzzer bytes to a bounded valid case: a hash picks the seed,
// and a bounded perturbation of one string/binary/pair length uses the input
// size/bytes. No unbounded input reaches any buffer.
static void mp_case_from_data(const uint8_t *data, size_t size,
                              struct mp_case *c) {
  uint32_t h = 2166136261u;
  size_t i;
  for (i = 0; i < size; i++) h = (h ^ data[i]) * 16777619u;
  mp_gen(h | 1u, -1, c);
  if (size > 2 && c->nprops > 0) {
    struct mp_spec *s;
    size_t idx = (size_t) (data[0] % c->nprops);
    int delta;
    s = &c->props[idx];
    if (s->type != MQTT_PROP_TYPE_STRING && s->type != MQTT_PROP_TYPE_BINARY_DATA &&
        s->type != MQTT_PROP_TYPE_STRING_PAIR)
      return;
    delta = (int) (data[size - 1] % 5) - 2;  // bounded by +/-2
    if (s->type == MQTT_PROP_TYPE_STRING_PAIR && (data[1] & 1)) {
      int nl = (int) s->key_len + delta;
      if (nl < 0) nl = 0;
      if (nl > 255) nl = 255;
      s->key_len = (size_t) nl;
      mp_fill(s->key, s->key_len, &h, 0);
    } else {
      int nl = (int) s->val_len + delta;
      if (nl < 0) nl = 0;
      if (nl > 255) nl = 255;
      s->val_len = (size_t) nl;
      mp_fill(s->val, s->val_len, &h,
              s->type == MQTT_PROP_TYPE_BINARY_DATA);
    }
    mp_encode(c);
  }
}

static void mp_garbage(struct mg_mqtt_prop *p) {
  memset(p, 0xA5, sizeof(*p));
  p->id = 0xFE;
  p->iv = 0xDEADBEEFu;
  // NULL but non-zero lengths: any field the decoder fails to clear shows up
  // as a length mismatch, without ever creating a pointer that is dereferenced.
  p->key.buf = NULL;
  p->key.len = (size_t) 0x1111;
  p->val.buf = NULL;
  p->val.len = (size_t) 0x2222;
}

static void mp_failf(struct mp_fail *f, int line, size_t at, const char *fmt, ...) {
  char buf[160];
  va_list ap;
  if (!f->ok) return;
  f->ok = 0;
  f->line = line;
  f->at = at;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  snprintf(f->msg, sizeof(f->msg), "%s", buf);
}

static void mp_snippet(struct mp_fail *f, const uint8_t *b, size_t from,
                       size_t to) {
  size_t i, n = 0;
  f->snippet[0] = '\0';
  if (to > from + 12) to = from + 12;
  for (i = from; i < to && n + 4 < sizeof(f->snippet); i++) {
    int w = snprintf(f->snippet + n, sizeof(f->snippet) - n, "%02x ", b[i]);
    if (w <= 0) break;
    n += (size_t) w;
  }
}

// Compare one successfully decoded property against the independent model.
static int mp_check_fields(const struct mp_spec *s,
                           const struct mg_mqtt_prop *p,
                           const uint8_t *base, struct mp_fail *f) {
  if (p->id != s->id) {
    mp_failf(f, __LINE__, s->start, "id %02x != expected %02x",
             (unsigned) p->id, (unsigned) s->id);
    return 0;
  }
  switch (s->type) {
    case MQTT_PROP_TYPE_BYTE:
    case MQTT_PROP_TYPE_SHORT:
    case MQTT_PROP_TYPE_INT:
    case MQTT_PROP_TYPE_VARIABLE_INT:
      if (p->iv != s->iv) {
        mp_failf(f, __LINE__, s->start, "iv %u != expected %u",
                 (unsigned) p->iv, (unsigned) s->iv);
        return 0;
      }
      if (p->key.len != 0 || p->key.buf != NULL || p->val.len != 0 ||
          p->val.buf != NULL) {
        mp_failf(f, __LINE__, s->start, "numeric property leaked string view");
        return 0;
      }
      break;
    case MQTT_PROP_TYPE_STRING:
    case MQTT_PROP_TYPE_BINARY_DATA:
      if (p->iv != 0) {
        mp_failf(f, __LINE__, s->start, "string property leaked iv %u",
                 (unsigned) p->iv);
        return 0;
      }
      if (p->key.len != 0 || p->key.buf != NULL) {
        mp_failf(f, __LINE__, s->start, "string property leaked key view");
        return 0;
      }
      if (p->val.len != s->val_len) {
        mp_failf(f, __LINE__, s->start, "val.len %lu != expected %lu",
                 (unsigned long) p->val.len, (unsigned long) s->val_len);
        return 0;
      }
      if (p->val.buf != (char *) base + s->val_off) {
        mp_failf(f, __LINE__, s->start, "val.buf not inside the dgram zone");
        return 0;
      }
      if (s->val_len > 0 && memcmp(p->val.buf, s->val, s->val_len) != 0) {
        mp_failf(f, __LINE__, s->start, "val bytes differ from the model");
        return 0;
      }
      break;
    case MQTT_PROP_TYPE_STRING_PAIR:
      if (p->iv != 0) {
        mp_failf(f, __LINE__, s->start, "pair property leaked iv %u",
                 (unsigned) p->iv);
        return 0;
      }
      if (p->key.len != s->key_len) {
        mp_failf(f, __LINE__, s->start, "key.len %lu != expected %lu",
                 (unsigned long) p->key.len, (unsigned long) s->key_len);
        return 0;
      }
      if (p->key.buf != (char *) base + s->key_off) {
        mp_failf(f, __LINE__, s->start, "key.buf not inside the dgram zone");
        return 0;
      }
      if (s->key_len > 0 && memcmp(p->key.buf, s->key, s->key_len) != 0) {
        mp_failf(f, __LINE__, s->start, "key bytes differ from the model");
        return 0;
      }
      if (p->val.len != s->val_len) {
        mp_failf(f, __LINE__, s->start, "val.len %lu != expected %lu",
                 (unsigned long) p->val.len, (unsigned long) s->val_len);
        return 0;
      }
      if (p->val.buf != (char *) base + s->val_off) {
        mp_failf(f, __LINE__, s->start, "pair val.buf not inside the zone");
        return 0;
      }
      if (s->val_len > 0 && memcmp(p->val.buf, s->val, s->val_len) != 0) {
        mp_failf(f, __LINE__, s->start, "pair value bytes differ from model");
        return 0;
      }
      break;
    default:
      mp_failf(f, __LINE__, s->start, "unexpected model type %d", s->type);
      return 0;
  }
  return 1;
}

// Run the decoder over a declared property zone of length props_size.
// `logical` selects whether the backing buffer also carries the rest of the
// real zone plus a payload tail (logical end) or ends exactly at props_size
// (physical end). Both must produce identical results.
static int mp_check_zone(const struct mp_case *c, size_t props_size, int logical,
                         struct mp_fail *f) {
  uint8_t raw[MP_GUARD * 2 + MP_ZONE_MAX + MP_TAIL_MAX];
  uint8_t pre[MP_GUARD], post[MP_GUARD];
  struct mg_mqtt_message m;
  struct mg_mqtt_prop p, before;
  const uint8_t *base;
  size_t dlen, i, off = 0, pi = 0;

  memset(raw, 0, sizeof(raw));
  for (i = 0; i < MP_GUARD; i++) raw[i] = (uint8_t) (0x11 + i);
  if (logical) {
    memcpy(raw + MP_GUARD, c->zone, c->zone_len);
    memcpy(raw + MP_GUARD + c->zone_len, c->tail, c->tail_len);
    dlen = c->zone_len + c->tail_len;
  } else {
    memcpy(raw + MP_GUARD, c->zone, props_size);
    dlen = props_size;
  }
  for (i = 0; i < MP_GUARD; i++)
    raw[MP_GUARD + dlen + i] = (uint8_t) (0x77 + i);
  memcpy(pre, raw, MP_GUARD);
  memcpy(post, raw + MP_GUARD + dlen, MP_GUARD);

  memset(&m, 0, sizeof(m));
  m.cmd = MQTT_CMD_PUBLISH;
  m.dgram.buf = (char *) (raw + MP_GUARD);
  m.dgram.len = dlen;
  m.props_start = 0;
  m.props_size = props_size;
  base = (const uint8_t *) m.dgram.buf;

  mp_garbage(&p);
  for (;;) {
    if (off == props_size) {
      int rc;
      size_t boff = off;
      before = p;
      rc = mg_mqtt_prop_next(&m, &p, &off);
      if (rc != 0) {
        mp_failf(f, __LINE__, boff, "end of zone returned %d, expected 0", rc);
        return 0;
      }
      if (off != boff || memcmp(&p, &before, sizeof(p)) != 0) {
        mp_failf(f, __LINE__, boff, "clean end mutated offset or property");
        return 0;
      }
      break;
    }
    if (pi >= c->nprops) {
      mp_failf(f, __LINE__, off, "trailing bytes not covered by model");
      return 0;
    }
    {
      const struct mp_spec *s = &c->props[pi];
      if (s->start != off) {
        mp_failf(f, __LINE__, off, "model start %lu != cursor %lu",
                 (unsigned long) s->start, (unsigned long) off);
        return 0;
      }
      mp_snippet(f, base, s->start,
                 props_size < s->end ? props_size : s->end);
      if (props_size >= s->end) {
        int rc;
        size_t boff = off;
        before = p;
        rc = mg_mqtt_prop_next(&m, &p, &off);
        if (rc != 1) {
          mp_failf(f, __LINE__, off, "property %lu returned %d, expected 1",
                   (unsigned long) pi, rc);
          return 0;
        }
        if (off != s->end) {
          mp_failf(f, __LINE__, s->start, "cursor %lu != expected end %lu",
                   (unsigned long) off, (unsigned long) s->end);
          return 0;
        }
        if (!mp_check_fields(s, &p, base, f)) return 0;
        pi++;
      } else {
        int rc;
        size_t boff = off;
        before = p;
        rc = mg_mqtt_prop_next(&m, &p, &off);
        if (rc != -1) {
          mp_failf(f, __LINE__, off,
                   "truncated property returned %d, expected -1", rc);
          return 0;
        }
        if (off != boff || memcmp(&p, &before, sizeof(p)) != 0) {
          mp_failf(f, __LINE__, boff,
                   "failed decode mutated offset or property");
          return 0;
        }
        break;
      }
    }
  }
  if (memcmp(raw, pre, MP_GUARD) != 0) {
    mp_failf(f, __LINE__, 0, "leading sentinel was modified");
    return 0;
  }
  if (memcmp(raw + MP_GUARD + dlen, post, MP_GUARD) != 0) {
    mp_failf(f, __LINE__, dlen, "trailing sentinel was modified");
    return 0;
  }
  return 1;
}

static int mp_expect_fail(struct mg_mqtt_message *m, struct mp_fail *f,
                          size_t expect_off, const char *what) {
  struct mg_mqtt_prop p, before;
  size_t off = expect_off;
  mp_garbage(&p);
  before = p;
  if (mg_mqtt_prop_next(m, &p, &off) != -1) {
    mp_failf(f, __LINE__, expect_off, "%s did not return -1", what);
    return 0;
  }
  if (off != expect_off || memcmp(&p, &before, sizeof(p)) != 0) {
    mp_failf(f, __LINE__, expect_off,
             "%s mutated offset or property on failure", what);
    return 0;
  }
  return 1;
}

// Empty zone, exact end, past end, SIZE_MAX-near descriptions, NULL arguments,
// unknown ids, and variable-integer termination limits.
static int mp_check_args(const struct mp_case *c, struct mp_fail *f) {
  struct mg_mqtt_message m;
  uint8_t tiny[8];
  struct mg_mqtt_prop p, before;
  size_t off;

  memset(&m, 0, sizeof(m));
  m.cmd = MQTT_CMD_PUBLISH;
  m.dgram.buf = (char *) c->zone;
  m.dgram.len = c->zone_len;
  m.props_start = 0;
  m.props_size = c->zone_len;

  // Offset exactly at the end -> clean stop, prop and offset untouched.
  mp_garbage(&p);
  before = p;
  off = c->zone_len;
  if (mg_mqtt_prop_next(&m, &p, &off) != 0) {
    mp_failf(f, __LINE__, off, "exact end did not return 0");
    return 0;
  }
  if (off != c->zone_len || memcmp(&p, &before, sizeof(p)) != 0) {
    mp_failf(f, __LINE__, off, "exact end mutated output");
    return 0;
  }

  // Offset past the end -> -1, untouched.
  if (!mp_expect_fail(&m, f, c->zone_len + 1, "offset past end")) return 0;
  if (!mp_expect_fail(&m, f, (size_t) SIZE_MAX, "SIZE_MAX offset")) return 0;

  // Description fields near SIZE_MAX must be rejected without overflowing.
  m.props_start = (size_t) SIZE_MAX;
  m.props_size = (size_t) SIZE_MAX;
  if (!mp_expect_fail(&m, f, 0, "SIZE_MAX description")) return 0;
  m.props_start = c->zone_len;
  m.props_size = (size_t) SIZE_MAX;
  if (!mp_expect_fail(&m, f, 0, "SIZE_MAX size with valid start")) return 0;
  m.props_start = (size_t) SIZE_MAX - 1;
  m.props_size = 0;
  if (!mp_expect_fail(&m, f, 0, "start near SIZE_MAX")) return 0;

  // NULL arguments: -1 without dereferencing.
  mp_garbage(&p);
  before = p;
  off = 0;
  if (mg_mqtt_prop_next(NULL, &p, &off) != -1) {
    mp_failf(f, __LINE__, 0, "NULL msg did not return -1");
    return 0;
  }
  if (off != 0 || memcmp(&p, &before, sizeof(p)) != 0) {
    mp_failf(f, __LINE__, 0, "NULL msg mutated output");
    return 0;
  }
  if (mg_mqtt_prop_next(&m, NULL, &off) != -1) {
    mp_failf(f, __LINE__, 0, "NULL prop did not return -1");
    return 0;
  }
  if (mg_mqtt_prop_next(&m, &p, NULL) != -1) {
    mp_failf(f, __LINE__, 0, "NULL offset did not return -1");
    return 0;
  }

  // Empty zone: zero readable bytes may be a NULL buffer.
  memset(&m, 0, sizeof(m));
  m.cmd = MQTT_CMD_PUBLISH;
  m.dgram.buf = NULL;
  m.dgram.len = 0;
  m.props_start = 0;
  m.props_size = 0;
  mp_garbage(&p);
  before = p;
  off = 0;
  if (mg_mqtt_prop_next(&m, &p, &off) != 0) {
    mp_failf(f, __LINE__, 0, "empty NULL zone did not return 0");
    return 0;
  }
  if (off != 0 || memcmp(&p, &before, sizeof(p)) != 0) {
    mp_failf(f, __LINE__, 0, "empty zone mutated output");
    return 0;
  }

  // A declared zone of length 0 but with payload bytes after it.
  m.dgram.buf = (char *) c->zone;
  m.dgram.len = c->zone_len;
  m.props_start = 0;
  m.props_size = 0;
  mp_garbage(&p);
  before = p;
  off = 0;
  if (mg_mqtt_prop_next(&m, &p, &off) != 0) {
    mp_failf(f, __LINE__, 0, "zero-length zone did not return 0");
    return 0;
  }
  if (off != 0 || memcmp(&p, &before, sizeof(p)) != 0) {
    mp_failf(f, __LINE__, 0, "zero-length zone mutated output");
    return 0;
  }

  // Unknown property ID.
  tiny[0] = 0x7F;
  tiny[1] = 0x00;
  memset(&m, 0, sizeof(m));
  m.cmd = MQTT_CMD_PUBLISH;
  m.dgram.buf = (char *) tiny;
  m.dgram.len = 1;
  m.props_start = 0;
  m.props_size = 1;
  if (!mp_expect_fail(&m, f, 0, "unknown property id")) return 0;

  // Variable integer that never terminates within its declared zone.
  tiny[0] = MQTT_PROP_SUBSCRIPTION_IDENTIFIER;
  tiny[1] = 0x80;
  tiny[2] = 0x80;
  tiny[3] = 0x80;
  tiny[4] = 0x80;
  m.dgram.buf = (char *) tiny;
  m.dgram.len = 5;
  m.props_start = 0;
  m.props_size = 5;
  if (!mp_expect_fail(&m, f, 0, "unterminated variable integer")) return 0;
  m.dgram.len = 4;
  m.props_size = 4;
  if (!mp_expect_fail(&m, f, 0, "unterminated 3-byte variable integer"))
    return 0;

  // Four-byte variable integer maximum is accepted.
  tiny[0] = MQTT_PROP_SUBSCRIPTION_IDENTIFIER;
  tiny[1] = 0xFF;
  tiny[2] = 0xFF;
  tiny[3] = 0xFF;
  tiny[4] = 0x7F;
  m.dgram.buf = (char *) tiny;
  m.dgram.len = 5;
  m.props_start = 0;
  m.props_size = 5;
  mp_garbage(&p);
  off = 0;
  if (mg_mqtt_prop_next(&m, &p, &off) != 1) {
    mp_failf(f, __LINE__, 0, "4-byte variable integer rejected");
    return 0;
  }
  if (p.id != MQTT_PROP_SUBSCRIPTION_IDENTIFIER || p.iv != 0x0FFFFFFFu ||
      off != 5 || p.key.len != 0 || p.val.len != 0) {
    mp_failf(f, __LINE__, 0, "4-byte variable integer decoded incorrectly");
    return 0;
  }
  return 1;
}

// Full case: the untruncated zone in both placements, a fixed set of bounded
// truncations in both placements, and the argument/limit checks.
static int mp_run_case(struct mp_case *c, struct mp_fail *f) {
  size_t cuts[8];
  size_t ncuts = 0, k;
  uint32_t rs;
  memset(f, 0, sizeof(*f));
  f->ok = 1;
  f->seed = c->seed;
  if (!mp_check_zone(c, c->zone_len, 1, f)) return 0;
  if (!mp_check_zone(c, c->zone_len, 0, f)) return 0;
  cuts[ncuts++] = 1;
  if (c->zone_len > 2) cuts[ncuts++] = 2;
  cuts[ncuts++] = c->zone_len - 1;
  cuts[ncuts++] = c->zone_len / 2;
  rs = c->seed ? c->seed : 1u;
  for (k = 0; k < 3; k++) {
    size_t cut = 1 + (mp_rng_next(&rs) % (c->zone_len > 1 ? c->zone_len - 1 : 1));
    cuts[ncuts++] = cut;
  }
  for (k = 0; k < ncuts; k++) {
    size_t cut = cuts[k] > c->zone_len ? c->zone_len : cuts[k];
    if (cut == 0) continue;
    if (!mp_check_zone(c, cut, 1, f)) return 0;
    if (!mp_check_zone(c, cut, 0, f)) return 0;
  }
  if (!mp_check_args(c, f)) return 0;
  return f->ok;
}

#endif  // MP_FUZZ_CORE_H
