#include "task_engine.h"

#include <stdio.h>
#include <string.h>

/*
 * Scalar -> UTF encoders and independently constructed boundary tables.
 * The tables are rebuilt directly from the emitted code units (lead/
 * continuation byte structure), not from the scalar stream, so they can
 * catch a unit offset landing in the middle of a code point.
 */

int enc_utf8(const uint32_t *s, int n, uint8_t *out, int cap) {
  int p = 0;
  for (int i = 0; i < n; i++) {
    uint32_t c = s[i];
    if (c < 0x80u) {
      if (p < cap) out[p] = (uint8_t)c;
      p += 1;
    } else if (c < 0x800u) {
      if (p + 1 < cap) {
        out[p] = (uint8_t)(0xc0u | (c >> 6));
        out[p + 1] = (uint8_t)(0x80u | (c & 0x3fu));
      }
      p += 2;
    } else if (c < 0x10000u) {
      if (p + 2 < cap) {
        out[p] = (uint8_t)(0xe0u | (c >> 12));
        out[p + 1] = (uint8_t)(0x80u | ((c >> 6) & 0x3fu));
        out[p + 2] = (uint8_t)(0x80u | (c & 0x3fu));
      }
      p += 3;
    } else {
      if (p + 3 < cap) {
        out[p] = (uint8_t)(0xf0u | (c >> 18));
        out[p + 1] = (uint8_t)(0x80u | ((c >> 12) & 0x3fu));
        out[p + 2] = (uint8_t)(0x80u | ((c >> 6) & 0x3fu));
        out[p + 3] = (uint8_t)(0x80u | (c & 0x3fu));
      }
      p += 4;
    }
  }
  return p;
}

int enc_utf16(const uint32_t *s, int n, uint16_t *out, int cap) {
  int p = 0;
  for (int i = 0; i < n; i++) {
    uint32_t c = s[i];
    if (c < 0x10000u) {
      if (p < cap) out[p] = (uint16_t)c;
      p += 1;
    } else {
      uint32_t v = c - 0x10000u;
      if (p + 1 < cap) {
        out[p] = (uint16_t)(0xd800u + (v >> 10));
        out[p + 1] = (uint16_t)(0xdc00u + (v & 0x3ffu));
      }
      p += 2;
    }
  }
  return p;
}

int enc_utf32(const uint32_t *s, int n, uint32_t *out, int cap) {
  for (int i = 0; i < n; i++)
    if (i < cap) out[i] = s[i];
  return n;
}

void bound_utf8(const uint8_t *units, int n, uint8_t *table) {
  memset(table, 0, (size_t)(n + 1));
  table[0] = 1;
  for (int i = 0; i < n; i++)
    if ((units[i] & 0xc0u) != 0x80u) table[i] = 1; /* lead or ASCII */
  table[n] = 1;
}

void bound_utf16(const uint16_t *units, int n, uint8_t *table) {
  memset(table, 0, (size_t)(n + 1));
  table[0] = 1;
  for (int i = 0; i < n; i++) {
    uint16_t u = units[i];
    /* The high surrogate starts the scalar; the low surrogate is interior. */
    if (u < 0xdc00u || u > 0xdfffu) table[i] = 1;
  }
  table[n] = 1;
}

void bound_utf32(const uint32_t *units, int n, uint8_t *table) {
  (void)units;
  memset(table, 1, (size_t)(n + 1)); /* every unit is one scalar */
}

int map_offset(const uint8_t *table, int unit_count, size_t offset) {
  if (offset > (size_t)unit_count) return -1;
  if (!table[offset]) return -1; /* inside a multi-unit code point */
  int scalar = 0;
  for (int i = 0; i < (int)offset; i++)
    if (table[i]) scalar++;
  return scalar;
}

int render_scalars(const uint32_t *s, int n, char *out, int cap) {
  int pos = 0;
  for (int i = 0; i < n && pos < cap - 8; i++) {
    int m = s[i] < 0x80u
                ? snprintf(out + pos, (size_t)(cap - pos), "%c", (int)s[i])
                : snprintf(out + pos, (size_t)(cap - pos), "U+%x ", s[i]);
    if (m < 0) break;
    pos += m;
  }
  if (pos < cap) out[pos] = 0;
  return pos;
}
