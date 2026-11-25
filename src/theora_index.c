/* Theora/Ogg stream indexing and validation for ogg_theora_plan.
 *
 * The source is read as a byte stream starting at offset zero.  Pages are
 * parsed directly (capture pattern, version, CRC, page sequence, BOS/EOS,
 * continuation state) instead of going through ogg_sync_pageout(), because
 * the latter silently skips bytes when it loses sync.  Every page of every
 * logical stream is validated here, including those that are never selected
 * by a cut.
 *
 * Page bodies are streamed in lacing segments of at most 255 bytes, so no
 * page (and therefore no video payload) is ever held in memory here; only
 * the three header packets are accumulated.
 */

#include "theora_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ogg/ogg.h>

#define THEORA_IDENT_MIN 42u
#define THEORA_SEG_MAX 255u

/* ------------------------------------------------------------------ */
/* Ogg CRC (polynomial 0x04c11db7, init/final 0, unreflected).         */

static uint32_t crc_byte(uint32_t crc, unsigned char b) {
  int bit;
  crc ^= (uint32_t)b << 24;
  for (bit = 0; bit < 8; ++bit) {
    if (crc & 0x80000000u)
      crc = (crc << 1) ^ 0x04c11db7u;
    else
      crc <<= 1;
  }
  return crc;
}

static uint32_t crc_bytes(uint32_t crc, const unsigned char *p, size_t len) {
  size_t i;
  for (i = 0; i < len; ++i) crc = crc_byte(crc, p[i]);
  return crc;
}

/** Compute the CRC of a page header whose checksum field is treated as
 * four zero bytes. */
static uint32_t header_crc(const unsigned char *header, size_t header_len) {
  uint32_t crc = 0;
  size_t i;
  for (i = 0; i < header_len; ++i) {
    unsigned char b = (i >= 22 && i < 26) ? 0 : header[i];
    crc = crc_byte(crc, b);
  }
  return crc;
}

/* ------------------------------------------------------------------ */
/* Small helpers.                                                      */

static uint32_t read_le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint32_t read_be32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int theora_calc_granule(uint32_t shift, uint64_t key_frame, uint64_t frame,
                        ogg_int64_t *out) {
  uint64_t kp1, hi, lo, val;
  if (frame < key_frame) return 1;
  kp1 = key_frame + 1;
  if (kp1 == 0) return 1;
  if (shift >= 64) return 1;
  if (kp1 > (UINT64_MAX >> shift)) return 1;
  hi = kp1 << shift;
  lo = frame - key_frame;
  if (hi > (uint64_t)INT64_MAX - lo) return 1;
  val = hi + lo;
  if (val > (uint64_t)INT64_MAX) return 1;
  *out = (ogg_int64_t)val;
  return 0;
}

int theora_read_ranges(FILE *source, const theora_range *ranges, size_t count,
                       unsigned char *buffer, uint64_t expected_len,
                       size_t capacity) {
  uint64_t total = 0, written = 0;
  size_t i;
  for (i = 0; i < count; ++i) {
    if (ranges[i].length > UINT64_MAX - total) return 1;
    total += ranges[i].length;
  }
  if (total != expected_len) return 1;
  if (total > (uint64_t)capacity) return 1;
  for (i = 0; i < count; ++i) {
    size_t len = (size_t)ranges[i].length;
    if (ranges[i].offset > (uint64_t)LONG_MAX) return 1;
    if (fseek(source, (long)ranges[i].offset, SEEK_SET) != 0) return 1;
    if (len && fread(buffer + (size_t)written, 1, len, source) != len)
      return 1;
    written += ranges[i].length;
  }
  return written == expected_len ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Growable arrays.                                                    */

static int grow_array(void **ptr, size_t *cap, size_t need, size_t elem) {
  size_t ncap;
  void *n;
  if (need <= *cap) return 0;
  ncap = *cap ? *cap : 16;
  while (ncap < need) {
    if (ncap > SIZE_MAX / 2) {
      ncap = need;
      break;
    }
    ncap *= 2;
  }
  if (ncap > SIZE_MAX / elem) return 1;
  n = realloc(*ptr, ncap * elem);
  if (!n) return 1;
  *ptr = n;
  *cap = ncap;
  return 0;
}

static int plan_add_range(ogg_theora_plan *plan, uint64_t offset,
                          uint64_t length) {
  theora_range *r;
  if (grow_array((void **)&plan->ranges, &plan->range_cap,
                 plan->range_count + 1, sizeof(*plan->ranges)))
    return 1;
  r = &plan->ranges[plan->range_count++];
  r->offset = offset;
  r->length = length;
  return 0;
}

static int plan_add_frame(ogg_theora_plan *plan, size_t *index) {
  theora_frame *f;
  if (grow_array((void **)&plan->frames, &plan->frame_cap,
                 plan->frame_count + 1, sizeof(*plan->frames)))
    return 1;
  f = &plan->frames[plan->frame_count];
  memset(f, 0, sizeof(*f));
  *index = plan->frame_count++;
  return 0;
}

static theora_stream *plan_add_stream(ogg_theora_plan *plan, uint32_t serial) {
  theora_stream *s;
  if (grow_array((void **)&plan->streams, &plan->stream_cap,
                 plan->stream_count + 1, sizeof(*plan->streams)))
    return NULL;
  s = &plan->streams[plan->stream_count++];
  memset(s, 0, sizeof(*s));
  s->serial = serial;
  s->frame_first = plan->frame_count;
  return s;
}

/* ------------------------------------------------------------------ */
/* Header parsing.                                                     */

static int check_magic(const unsigned char *buf, size_t len,
                       unsigned char type) {
  if (len < 7) return 1;
  if (buf[0] != type) return 1;
  if (memcmp(buf + 1, "theora", 6) != 0) return 1;
  return 0;
}

static uint32_t read_be24(const unsigned char *p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static int parse_ident(theora_stream *s, const unsigned char *buf,
                       size_t len) {
  uint32_t fps_num, fps_den;
  unsigned shift, pixel_fmt;
  if (len < THEORA_IDENT_MIN) return 1;
  if (check_magic(buf, len, 0x80)) return 1;
  if (buf[7] != 3 || buf[8] != 2 || buf[9] != 1) return 1; /* version 3.2.1 */
  if (read_be32(buf + 10) == 0 || read_be32(buf + 12) == 0) return 1;
  if (read_be24(buf + 14) == 0 || read_be24(buf + 17) == 0) return 1;
  fps_num = read_be32(buf + 22);
  fps_den = read_be32(buf + 26);
  if (fps_num == 0 || fps_den == 0) return 1; /* zero frame rate */
  shift = (unsigned)(((buf[40] & 0x03u) << 3) | (buf[41] >> 5));
  if (shift > 31) return 1; /* illegal keyframe granule shift */
  pixel_fmt = (unsigned)((buf[41] >> 3) & 0x03u);
  if (pixel_fmt == 1u) return 1; /* reserved pixel format */
  s->fps_num = fps_num;
  s->fps_den = fps_den;
  s->shift = shift;
  return 0;
}

static int parse_comment(const unsigned char *buf, size_t len) {
  size_t p;
  uint32_t vlen, n, i;
  if (check_magic(buf, len, 0x81)) return 1;
  if (len < 15) return 1;
  p = 7;
  vlen = read_le32(buf + p);
  p += 4;
  if ((uint64_t)vlen > (uint64_t)(len - p)) return 1;
  p += (size_t)vlen;
  if (p + 4 > len) return 1;
  n = read_le32(buf + p);
  p += 4;
  for (i = 0; i < n; ++i) {
    uint32_t clen;
    if (p + 4 > len) return 1;
    clen = read_le32(buf + p);
    p += 4;
    if ((uint64_t)clen > (uint64_t)(len - p)) return 1;
    p += (size_t)clen;
  }
  if (p != len) return 1;
  return 0;
}

static int parse_setup(const unsigned char *buf, size_t len) {
  if (len < 8) return 1;
  if (check_magic(buf, len, 0x82)) return 1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Page scanning (packet assembly).                                    */

typedef struct parse_state {
  ogg_theora_plan *plan;
  theora_stream *cur;
  int cur_eos;
  uint32_t expected_pageno;

  int open_packet;
  int open_kind;        /* 0 none, 1 header, 2 frame */
  unsigned header_index;
  size_t frame_index;

  unsigned char *tmp;   /* accumulated header bytes across pages */
  size_t tmp_len;
  size_t tmp_cap;

  uint64_t last_key;
  int have_key;

  int page_completed;
  int page_last_kind;   /* 1 header, 2 frame */
  size_t page_last_frame;
} parse_state;

static void free_tmp(parse_state *ps) {
  free(ps->tmp);
  ps->tmp = NULL;
  ps->tmp_len = 0;
  ps->tmp_cap = 0;
}

static int tmp_append(parse_state *ps, const unsigned char *data, size_t len) {
  if (len == 0) return 0;
  if (grow_array((void **)&ps->tmp, &ps->tmp_cap, ps->tmp_len + len, 1))
    return 1;
  memcpy(ps->tmp + ps->tmp_len, data, len);
  ps->tmp_len += len;
  return 0;
}

static int start_packet(parse_state *ps, unsigned char first, size_t len) {
  if (ps->header_index < 3) {
    ps->open_kind = 1;
    ps->tmp_len = 0;
    return 0;
  }
  {
    theora_frame *f;
    size_t index;
    if (plan_add_frame(ps->plan, &index)) return 1;
    ps->frame_index = index;
    f = &ps->plan->frames[index];
    if (len == 0) {
      f->keyframe = 0; /* zero-byte repeat frame is legal and non-key */
    } else {
      if (first & 0x80u) return 1; /* not a Theora video packet */
      f->keyframe = (unsigned char)((first & 0x40u) == 0);
    }
    ps->open_kind = 2;
  }
  return 0;
}

static int finish_packet(parse_state *ps) {
  theora_stream *s = ps->cur;
  if (ps->open_kind == 1) {
    switch (ps->header_index) {
      case 0:
        if (parse_ident(s, ps->tmp, ps->tmp_len)) return 1;
        s->ident = ps->tmp;
        s->ident_len = ps->tmp_len;
        break;
      case 1:
        if (parse_comment(ps->tmp, ps->tmp_len)) return 1;
        s->comment = ps->tmp;
        s->comment_len = ps->tmp_len;
        break;
      case 2:
        if (parse_setup(ps->tmp, ps->tmp_len)) return 1;
        s->setup = ps->tmp;
        s->setup_len = ps->tmp_len;
        break;
      default:
        return 1;
    }
    ps->tmp = NULL;
    ps->tmp_len = 0;
    ps->tmp_cap = 0;
    ++ps->header_index;
    ps->page_last_kind = 1;
  } else if (ps->open_kind == 2) {
    theora_frame *f = &ps->plan->frames[ps->frame_index];
    uint64_t local = (uint64_t)(ps->frame_index - s->frame_first);
    if (f->keyframe) {
      ps->last_key = local;
      ps->have_key = 1;
    }
    ps->page_last_kind = 2;
    ps->page_last_frame = ps->frame_index;
  } else {
    return 1;
  }
  ps->open_kind = 0;
  return 0;
}

/* Validate a page granule.  raw is the unsigned 64-bit header value; the
 * all-ones value is the -1 "no anchor" marker.  A page that completes no
 * packet must carry -1.  A non-negative granule belongs to the last packet
 * completed on the page and is checked against the frame sequence. */
static int check_page_granule(parse_state *ps, uint64_t raw) {
  theora_stream *s = ps->cur;
  if (!ps->page_completed) return raw == UINT64_MAX ? 0 : 1;
  if (raw == UINT64_MAX) {
    /* -1 means no usable anchor.  A page that completes a frame must carry
     * the anchor for its last packet; header-only pages are handled as 0. */
    return ps->page_last_kind == 2 ? 1 : 0;
  }
  if (raw & (UINT64_C(1) << 63)) return 1; /* negative but not -1 */
  if (ps->page_last_kind == 1) {
    /* Pages that only complete header packets carry no frame time; the
     * granule is treated as zero. */
    return 0;
  }
  if (ps->page_last_kind == 2) {
    ogg_int64_t expected;
    uint64_t frame = (uint64_t)(ps->page_last_frame - s->frame_first);
    if (!ps->have_key || frame < ps->last_key) return 1;
    if (theora_calc_granule(s->shift, ps->last_key, frame, &expected))
      return 1;
    return (uint64_t)expected == raw ? 0 : 1;
  }
  return 1;
}

/* Process one page body, reading the segments straight from f.  *crc is
 * updated with every body byte. */
static int process_page(parse_state *ps, FILE *f, const unsigned char *header,
                        size_t body_len, uint64_t body_pos, uint64_t raw_granule,
                        int continued, uint32_t *crc) {
  int nseg = (int)header[26];
  size_t off = 0;
  int i;
  unsigned char scratch[THEORA_SEG_MAX];

  if (continued) {
    if (!ps->open_packet) return 1; /* orphan continuation */
  } else {
    if (ps->open_packet) return 1; /* previous packet truncated */
  }

  ps->page_completed = 0;
  ps->page_last_kind = 0;
  ps->page_last_frame = 0;

  for (i = 0; i < nseg; ++i) {
    size_t len = (size_t)header[27 + i];
    if (len > body_len - off) return 1;
    if (len) {
      if (fread(scratch, 1, len, f) != len) return 1; /* truncated body */
      *crc = crc_bytes(*crc, scratch, len);
    }
    if (!ps->open_packet) {
      if (start_packet(ps, len ? scratch[0] : 0, len)) return 1;
      ps->open_packet = 1;
    }
    if (ps->open_kind == 1) {
      if (tmp_append(ps, scratch, len)) return 1;
    } else if (ps->open_kind == 2) {
      if (len) {
        theora_frame *fr = &ps->plan->frames[ps->frame_index];
        if (fr->range_count == 0) fr->range_first = ps->plan->range_count;
        if (plan_add_range(ps->plan, body_pos + off, len)) return 1;
        fr->range_count++;
        if (fr->bytes > UINT64_MAX - len) return 1;
        fr->bytes += len;
      }
    } else {
      return 1;
    }
    off += len;
    if (len < 255) {
      if (finish_packet(ps)) return 1;
      ps->open_packet = 0;
      ps->page_completed = 1;
    }
  }

  return check_page_granule(ps, raw_granule);
}

/* ------------------------------------------------------------------ */
/* Building the index.                                                 */

static int stream_serial_seen(const ogg_theora_plan *plan, uint32_t serial) {
  size_t i;
  for (i = 0; i < plan->stream_count; ++i)
    if (plan->streams[i].serial == serial) return 1;
  return 0;
}

static void finish_stream(parse_state *ps) {
  theora_stream *s = ps->cur;
  if (!s) return;
  s->frame_count = (uint64_t)(ps->plan->frame_count - s->frame_first);
}

static int begin_stream(parse_state *ps, uint32_t serial) {
  ps->cur = plan_add_stream(ps->plan, serial);
  if (!ps->cur) return 1;
  ps->cur_eos = 0;
  ps->expected_pageno = 0;
  ps->open_packet = 0;
  ps->open_kind = 0;
  ps->header_index = 0;
  ps->frame_index = 0;
  ps->last_key = 0;
  ps->have_key = 0;
  free_tmp(ps);
  return 0;
}

void theora_plan_free_contents(ogg_theora_plan *plan) {
  size_t i;
  if (!plan) return;
  for (i = 0; i < plan->stream_count; ++i) {
    free(plan->streams[i].ident);
    free(plan->streams[i].comment);
    free(plan->streams[i].setup);
  }
  free(plan->streams);
  free(plan->frames);
  free(plan->ranges);
  free(plan->ident);
  free(plan->comment);
  free(plan->setup);
  free(plan->cuts);
  memset(plan, 0, sizeof(*plan));
}

int theora_index_build(ogg_theora_plan *plan) {
  parse_state ps;
  uint64_t pos = 0;
  unsigned char *header = NULL;

  memset(&ps, 0, sizeof(ps));
  ps.plan = plan;

  if (fseek(plan->source, 0, SEEK_SET) != 0) return 1;

  for (;;) {
    unsigned char fixed[27];
    size_t hdr_len, body_len = 0, got;
    int nseg, i;
    uint32_t serial, pageno, stored_crc, computed_crc;
    unsigned flags;
    uint64_t raw_granule = 0;
    uint64_t body_pos;

    got = fread(fixed, 1, sizeof(fixed), plan->source);
    if (got == 0) break; /* clean end of file */
    if (got != sizeof(fixed)) goto fail; /* truncated page header */
    if (memcmp(fixed, "OggS", 4) != 0) goto fail;
    if (fixed[4] != 0) goto fail; /* stream structure version */

    nseg = (int)fixed[26];
    hdr_len = 27 + (size_t)nseg;
    header = malloc(hdr_len);
    if (!header) goto fail;
    memcpy(header, fixed, 27);
    if (nseg) {
      got = fread(header + 27, 1, (size_t)nseg, plan->source);
      if (got != (size_t)nseg) goto fail; /* truncated segment table */
    }
    pos += hdr_len;

    for (i = 0; i < nseg; ++i) {
      if (body_len > SIZE_MAX - (size_t)header[27 + i]) goto fail;
      body_len += (size_t)header[27 + i];
    }
    body_pos = pos;

    flags = header[5];
    serial = read_le32(header + 14);
    pageno = read_le32(header + 18);
    for (i = 0; i < 8; ++i)
      raw_granule |= (uint64_t)header[6 + i] << (8 * i);

    if (!ps.cur) {
      if (!(flags & 0x02u)) goto fail; /* first page must be BOS */
      if (pageno != 0) goto fail;
      if (stream_serial_seen(plan, serial)) goto fail;
      if (begin_stream(&ps, serial)) goto fail;
    } else if (serial == ps.cur->serial) {
      if (ps.cur_eos) goto fail; /* data after EOS */
      if (flags & 0x02u) goto fail; /* second BOS */
      if (pageno != ps.expected_pageno) goto fail; /* hole / reorder */
    } else {
      if (!ps.cur_eos) goto fail; /* interleaved streams unsupported */
      finish_stream(&ps);
      if (ps.header_index != 3) goto fail;
      if (!(flags & 0x02u)) goto fail;
      if (pageno != 0) goto fail;
      if (stream_serial_seen(plan, serial)) goto fail;
      if (begin_stream(&ps, serial)) goto fail;
    }

    computed_crc = header_crc(header, hdr_len);
    if (process_page(&ps, plan->source, header, body_len, body_pos,
                     raw_granule, (flags & 0x01u) != 0, &computed_crc) != 0)
      goto fail;
    stored_crc = read_le32(header + 22);
    if (stored_crc != computed_crc) goto fail; /* CRC error */

    if (flags & 0x04u) {
      if (ps.open_packet) goto fail; /* final packet truncated */
      if (!ps.page_completed) goto fail; /* EOS with no completed packet */
      ps.cur_eos = 1;
    }
    ps.expected_pageno++;
    free(header);
    header = NULL;
    pos += body_len;
  }

  if (!ps.cur) goto fail; /* no pages at all */
  finish_stream(&ps);
  if (!ps.cur_eos) goto fail; /* missing EOS */
  if (ps.header_index != 3) goto fail; /* missing header(s) */
  if (!ps.cur->ident || !ps.cur->comment || !ps.cur->setup) goto fail;

  free_tmp(&ps);
  return 0;

fail:
  free(header);
  free_tmp(&ps);
  return 1;
}
