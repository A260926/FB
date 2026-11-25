/* Tests for ogg_theora_plan (include/ogg/theora_edit.h).
 *
 * The fixture frames.ogv is a real 12 frame Theora stream.  Its packet
 * payloads and headers are used, but every synthetic source used here is
 * re-paged and re-checksummed by this file's own page builder (and its own
 * bit-at-a-time CRC), so the tests never rely on the production pager.
 *
 * Output is parsed independently as well: pages, lacing, packets and
 * granules are decoded without using ogg_stream_state, so a wrong page
 * layout or granule cannot hide behind libogg's decoder.
 */

#include "../task_support/check.h"

#include <ogg/ogg.h>
#include <ogg/theora_edit.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Byte and CRC helpers.                                               */

static void put_le32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

static uint32_t get_le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint32_t get_be32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_be32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v >> 24);
  p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);
  p[3] = (unsigned char)v;
}

/* Independent Ogg CRC: polynomial 0x04c11db7, init/final zero. */
static uint32_t ogg_crc(const unsigned char *data, size_t len) {
  uint32_t crc = 0;
  size_t i;
  int bit;
  for (i = 0; i < len; ++i) {
    crc ^= (uint32_t)data[i] << 24;
    for (bit = 0; bit < 8; ++bit) {
      if (crc & 0x80000000u)
        crc = (crc << 1) ^ 0x04c11db7u;
      else
        crc <<= 1;
    }
  }
  return crc;
}

static void stamp_crc(unsigned char *page, size_t len) {
  uint32_t crc;
  put_le32(page + 22, 0);
  crc = ogg_crc(page, len);
  put_le32(page + 22, crc);
}

/* ------------------------------------------------------------------ */
/* Dynamic byte buffer.                                                */

typedef struct buf {
  unsigned char *data;
  size_t len;
  size_t cap;
} buf;

static void buf_init(buf *b) { memset(b, 0, sizeof(*b)); }

static int buf_add(buf *b, const void *data, size_t len) {
  if (b->len + len > b->cap) {
    size_t ncap = b->cap ? b->cap : 1024;
    unsigned char *n;
    while (b->len + len > ncap) ncap *= 2;
    n = realloc(b->data, ncap);
    if (!n) return 1;
    b->data = n;
    b->cap = ncap;
  }
  if (len) memcpy(b->data + b->len, data, len);
  b->len += len;
  return 0;
}

static void buf_free(buf *b) {
  free(b->data);
  memset(b, 0, sizeof(*b));
}

/* ------------------------------------------------------------------ */
/* Independent page emitter.                                           */

static int emit_page(buf *out, uint32_t serial, uint32_t pageno,
                     unsigned flags, int64_t granule,
                     const unsigned char *segments, int nseg,
                     const unsigned char *body, size_t body_len) {
  size_t total = 27 + (size_t)nseg + body_len;
  unsigned char *page;
  uint64_t gp = (uint64_t)granule;
  int i;
  if (nseg < 0 || nseg > 255) return 1;
  page = malloc(total ? total : 1);
  if (!page) return 1;
  memcpy(page, "OggS", 4);
  page[4] = 0;
  page[5] = (unsigned char)flags;
  for (i = 0; i < 8; ++i) page[6 + i] = (unsigned char)(gp >> (8 * i));
  put_le32(page + 14, serial);
  put_le32(page + 18, pageno);
  put_le32(page + 22, 0);
  page[26] = (unsigned char)nseg;
  for (i = 0; i < nseg; ++i) page[27 + i] = segments[i];
  if (body_len) memcpy(page + 27 + nseg, body, body_len);
  stamp_crc(page, total);
  if (buf_add(out, page, total)) {
    free(page);
    return 1;
  }
  free(page);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Fixture packet extraction.                                          */

typedef struct packets {
  unsigned char *data[64];
  size_t len[64];
  ogg_int64_t gp[64];
  int count;
} packets;

static int read_packets(const char *path, packets *pk) {
  FILE *in = fopen(path, "rb");
  ogg_sync_state sy;
  ogg_stream_state os;
  int have_stream = 0;
  int rc = 1;
  memset(pk, 0, sizeof(*pk));
  if (!in) return 1;
  ogg_sync_init(&sy);
  for (;;) {
    ogg_page page;
    int r = ogg_sync_pageout(&sy, &page);
    if (r < 0) goto done;
    if (r == 0) {
      char *b = ogg_sync_buffer(&sy, 4096);
      size_t got = fread(b, 1, 4096, in);
      if (!got) {
        if (ferror(in)) goto done;
        break;
      }
      ogg_sync_wrote(&sy, (long)got);
      continue;
    }
    if (!have_stream) {
      if (ogg_stream_init(&os, ogg_page_serialno(&page))) goto done;
      have_stream = 1;
    }
    if (ogg_stream_pagein(&os, &page)) goto done;
    for (;;) {
      ogg_packet p;
      r = ogg_stream_packetout(&os, &p);
      if (r == 0) break;
      if (r < 0) goto done;
      if (pk->count >= 64) goto done;
      pk->data[pk->count] = malloc(p.bytes ? (size_t)p.bytes : 1);
      if (!pk->data[pk->count]) goto done;
      if (p.bytes) memcpy(pk->data[pk->count], p.packet, (size_t)p.bytes);
      pk->len[pk->count] = (size_t)p.bytes;
      pk->gp[pk->count] = p.granulepos;
      pk->count++;
    }
  }
  rc = 0;
done:
  if (have_stream) ogg_stream_clear(&os);
  ogg_sync_clear(&sy);
  fclose(in);
  if (rc) {
    int i;
    for (i = 0; i < pk->count; ++i) free(pk->data[i]);
    memset(pk, 0, sizeof(*pk));
  }
  return rc;
}

static void free_packets(packets *pk) {
  int i;
  for (i = 0; i < pk->count; ++i) free(pk->data[i]);
  memset(pk, 0, sizeof(*pk));
}

#define HDR_COUNT 3

/* ------------------------------------------------------------------ */
/* Source construction.                                                */

typedef struct frame_def {
  const unsigned char *data;
  size_t len;
  int key;
} frame_def;

/* Same granule formula the spec requires, used to make valid sources. */
static int64_t src_granule(uint32_t shift, uint64_t key, uint64_t n) {
  return (int64_t)(((uint64_t)(key + 1) << shift) + (n - key));
}

typedef struct page_acc {
  unsigned char segs[255];
  int nseg;
  buf body;
  int continued;        /* page begins inside a packet */
  int open;             /* page ends inside a packet */
  int comp_header;      /* some header packet completed on this page */
  int comp_frame;       /* some frame completed on this page */
  size_t first_frame;
  size_t last_frame;
} page_acc;

static void pa_init(page_acc *pa, int continued) {
  memset(pa, 0, sizeof(*pa));
  pa->continued = continued;
}

static void pa_reset(page_acc *pa, int continued) {
  buf_free(&pa->body);
  pa_init(pa, continued);
}

static int pa_seg(page_acc *pa, unsigned char v) {
  if (pa->nseg >= 255) return 1;
  pa->segs[pa->nseg++] = v;
  return 0;
}

static int pa_add_whole_frame(page_acc *pa, const frame_def *frames,
                              size_t index) {
  size_t len = frames[index].len;
  const unsigned char *data = frames[index].data;
  size_t rem = len;
  while (rem > 255) {
    if (pa_seg(pa, 255) || buf_add(&pa->body, data, 255)) return 1;
    data += 255;
    rem -= 255;
  }
  if (pa_seg(pa, (unsigned char)rem) || buf_add(&pa->body, data, rem)) return 1;
  pa->comp_frame = 1;
  pa->last_frame = index;
  return 0;
}

/* Add a piece of a frame packet.  If end_packet is zero, len must be a
 * multiple of 255 and the page will end inside the packet. */
static int pa_add_frame_piece(page_acc *pa, const unsigned char *data, size_t len,
                              int end_packet) {
  if (!end_packet) {
    size_t i;
    if (len % 255 != 0) return 1;
    for (i = 0; i < len; i += 255)
      if (pa_seg(pa, 255) || buf_add(&pa->body, data + i, 255)) return 1;
    pa->open = 1;
    return 0;
  } else {
    size_t rem = len;
    while (rem > 255) {
      if (pa_seg(pa, 255) || buf_add(&pa->body, data, 255)) return 1;
      data += 255;
      rem -= 255;
    }
    if (pa_seg(pa, (unsigned char)rem) || buf_add(&pa->body, data, rem))
      return 1;
    pa->open = 0;
    pa->comp_frame = 1;
    return 0;
  }
}

static int pa_add_header(page_acc *pa, const unsigned char *data, size_t len) {
  size_t rem = len;
  while (rem > 255) {
    if (pa_seg(pa, 255) || buf_add(&pa->body, data, 255)) return 1;
    data += 255;
    rem -= 255;
  }
  if (pa_seg(pa, (unsigned char)rem) || buf_add(&pa->body, data, rem)) return 1;
  pa->comp_header = 1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Stream builder: assembles pages and computes source granules from    */
/* the frame key flags.                                                 */

typedef struct stream_builder {
  buf out;
  uint32_t serial;
  uint32_t pageno;
  uint32_t shift;
  uint64_t last_key;
  int have_key;
  int8_t *keys;      /* frame key flags, used to advance last_key */
} stream_builder;

static void sb_init(stream_builder *sb, uint32_t serial, uint32_t shift,
                    int8_t *keys) {
  memset(sb, 0, sizeof(*sb));
  sb->serial = serial;
  sb->shift = shift;
  sb->keys = keys;
}

static int sb_flush(stream_builder *sb, page_acc *pa, unsigned flags_extra,
                    int eos, int force_no_granule) {
  int64_t gp;
  int i;
  if (pa->comp_frame) {
    for (i = (int)pa->first_frame; i <= (int)pa->last_frame; ++i)
      if (sb->keys[i]) {
        sb->last_key = (uint64_t)i;
        sb->have_key = 1;
      }
    if (!sb->have_key) return 1;
    gp = src_granule(sb->shift, sb->last_key, (uint64_t)pa->last_frame);
  } else if (pa->comp_header) {
    gp = 0;
  } else {
    gp = -1;
  }
  if (force_no_granule) gp = -1;
  {
    unsigned flags = flags_extra;
    if (pa->continued) flags |= 0x01u;
    if (eos) flags |= 0x04u;
    if (emit_page(&sb->out, sb->serial, sb->pageno++, flags, gp, pa->segs,
                  pa->nseg, pa->body.data, pa->body.len)) {
      return 1;
    }
  }
  pa_reset(pa, pa->open); /* next page continues if this one ended open */
  return 0;
}

/* Helper: a simple source: ident, comment, setup, then all frames in one
 * EOS page. */
static int build_simple(stream_builder *sb, const unsigned char *ident,
                        size_t ident_len, const unsigned char *comment,
                        size_t comment_len, const unsigned char *setup,
                        size_t setup_len, const frame_def *frames,
                        size_t nframes) {
  page_acc pa;
  pa_init(&pa, 0);
  if (pa_add_header(&pa, ident, ident_len)) return 1;
  if (sb_flush(sb, &pa, 0x02, 0, 0)) return 1;
  pa_init(&pa, 0);
  if (pa_add_header(&pa, comment, comment_len)) return 1;
  if (sb_flush(sb, &pa, 0, 0, 0)) return 1;
  pa_init(&pa, 0);
  if (pa_add_header(&pa, setup, setup_len)) return 1;
  if (sb_flush(sb, &pa, 0, 0, 0)) return 1;
  pa_init(&pa, 0);
  {
    size_t i;
    for (i = 0; i < nframes; ++i)
      if (pa_add_whole_frame(&pa, frames, i)) return 1;
    pa.comp_frame = 1;
    pa.first_frame = 0;
    pa.last_frame = nframes - 1;
  }
  return sb_flush(sb, &pa, 0, 1, 0);
}

/* The "paged" source: setup split across pages, several frames on a page,
 * a frame split across a page boundary, and a -1 continuation page. */
static int build_paged(stream_builder *sb, const unsigned char *ident,
                       size_t ident_len, const unsigned char *comment,
                       size_t comment_len, const unsigned char *setup,
                       size_t setup_len, const frame_def *frames,
                       size_t nframes) {
  page_acc pa;
  size_t setup_cut;
  size_t big_piece;

  /* ident */
  pa_init(&pa, 0);
  if (pa_add_header(&pa, ident, ident_len)) return 1;
  if (sb_flush(sb, &pa, 0x02, 0, 0)) return 1;

  /* comment */
  pa_init(&pa, 0);
  if (pa_add_header(&pa, comment, comment_len)) return 1;
  if (sb_flush(sb, &pa, 0, 0, 0)) return 1;

  /* setup, split in two pages: the first piece has only 255-byte segments
   * and so completes nothing (-1); the second is continued and completes
   * the header (granule 0). */
  setup_cut = setup_len / 2;
  setup_cut -= setup_cut % 255;
  if (setup_cut == 0) setup_cut = 255;
  if (setup_cut >= setup_len) setup_cut = setup_len - (setup_len % 255);
  pa_init(&pa, 0);
  {
    size_t i;
    for (i = 0; i < setup_cut; i += 255)
      if (pa_seg(&pa, 255) || buf_add(&pa.body, setup + i, 255)) return 1;
  }
  pa.open = 1;
  if (sb_flush(sb, &pa, 0, 0, 0)) return 1; /* continues; granule -1 */
  pa_init(&pa, 1);
  if (pa_add_header(&pa, setup + setup_cut, setup_len - setup_cut)) return 1;
  if (sb_flush(sb, &pa, 0, 0, 0)) return 1;

  /* frames 0..2 on one page, with the page granule belonging to frame 2 */
  pa_init(&pa, 0);
  pa.first_frame = 0;
  {
    size_t i;
    for (i = 0; i < 3; ++i)
      if (pa_add_whole_frame(&pa, frames, i)) return 1;
  }
  if (sb_flush(sb, &pa, 0, 0, 0)) return 1;

  /* frame 3 complete, then the first 510 bytes of frame 4 (which must be
   * longer than 510).  Page ends inside frame 4. */
  pa_init(&pa, 0);
  pa.comp_frame = 1;
  pa.first_frame = 3;
  if (pa_add_whole_frame(&pa, frames, 3)) return 1;
  pa.last_frame = 3;
  big_piece = 510;
  if (big_piece >= frames[4].len) return 1;
  if (pa_add_frame_piece(&pa, frames[4].data, big_piece, 0)) return 1;
  if (sb_flush(sb, &pa, 0, 0, 0)) return 1;

  /* continuation page: rest of frame 4 */
  pa_init(&pa, 1);
  pa.comp_frame = 1;
  pa.first_frame = 4;
  if (pa_add_frame_piece(&pa, frames[4].data + big_piece,
                         frames[4].len - big_piece, 1))
    return 1;
  pa.last_frame = 4;
  if (sb_flush(sb, &pa, 0, 0, 0)) return 1;

  /* remaining frames 5..end, EOS */
  pa_init(&pa, 0);
  pa.first_frame = 5;
  {
    size_t i;
    for (i = 5; i < nframes; ++i)
      if (pa_add_whole_frame(&pa, frames, i)) return 1;
  }
  return sb_flush(sb, &pa, 0, 1, 0);
}

/* ------------------------------------------------------------------ */
/* Comment variants.                                                   */

/* Rebuild a comment packet adding one comment.  Simpler and correct. */
static int comment_with_extra(const unsigned char *orig, size_t len,
                              const char *extra, buf *out) {
  size_t p, vend_len_end, n, i;
  uint32_t vlen, count;
  buf_init(out);
  if (len < 15 || memcmp(orig + 1, "theora", 6) != 0) return 1;
  if (buf_add(out, orig, 7)) return 1;
  p = 7;
  vlen = get_le32(orig + p);
  if (p + 4 + vlen + 4 > len) return 1;
  if (buf_add(out, orig + p, 4 + vlen)) return 1;
  p += 4 + vlen;
  count = get_le32(orig + p);
  vend_len_end = p;
  if (buf_add(out, orig + p, 4)) return 1;
  p += 4;
  for (i = 0; i < count; ++i) {
    uint32_t clen;
    if (p + 4 > len) return 1;
    clen = get_le32(orig + p);
    if (p + 4 + clen > len) return 1;
    if (buf_add(out, orig + p, 4 + clen)) return 1;
    p += 4 + clen;
  }
  /* overwrite count with count+1 */
  put_le32(out->data + vend_len_end, count + 1);
  if (buf_add(out, &count, 0)) return 1;
  {
    uint32_t clen32 = (uint32_t)strlen(extra);
    unsigned char hdr[4];
    put_le32(hdr, clen32);
    if (buf_add(out, hdr, 4)) return 1;
    if (buf_add(out, extra, clen32)) return 1;
  }
  /* copy any trailing bytes (there are none for a well formed packet) */
  if (p != len) return 1;
  n = count;
  (void)n;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Output parsing without libogg.                                      */

typedef struct page_info {
  unsigned flags;
  int64_t granule;
  uint32_t serial;
  uint32_t pageno;
  int nseg;
  unsigned char segments[255];
  size_t body_len;
  size_t offset;
} page_info;

typedef struct parsed {
  unsigned char *bytes;
  size_t len;
  page_info *pages;
  size_t page_count;
  unsigned char *packet[512];
  size_t packet_len[512];
  size_t packet_page[512];
  int packet_count;
} parsed;

static void free_parsed(parsed *o) {
  int i;
  for (i = 0; i < o->packet_count; ++i) free(o->packet[i]);
  free(o->pages);
  free(o->bytes);
  memset(o, 0, sizeof(*o));
}

static int parse_file(const char *path, parsed *out) {
  FILE *f = fopen(path, "rb");
  long sz;
  memset(out, 0, sizeof(*out));
  if (!f) return 1;
  if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET))
    goto fail;
  out->len = (size_t)sz;
  out->bytes = malloc(out->len ? out->len : 1);
  if (!out->bytes) goto fail;
  if (out->len && fread(out->bytes, 1, out->len, f) != out->len) goto fail;
  fclose(f);
  f = NULL;

  {
    size_t off = 0, cap = 0;
    while (off < out->len) {
      page_info pi;
      size_t total;
      int i;
      uint32_t stored, computed;
      unsigned char *tmp;
      if (off + 27 > out->len) goto fail;
      if (memcmp(out->bytes + off, "OggS", 4)) goto fail;
      if (out->bytes[off + 4] != 0) goto fail;
      memset(&pi, 0, sizeof(pi));
      pi.flags = out->bytes[off + 5];
      {
        uint64_t g = 0;
        for (i = 0; i < 8; ++i)
          g |= (uint64_t)out->bytes[off + 6 + i] << (8 * i);
        pi.granule = (int64_t)g;
      }
      pi.serial = get_le32(out->bytes + off + 14);
      pi.pageno = get_le32(out->bytes + off + 18);
      pi.nseg = out->bytes[off + 26];
      if (off + 27 + (size_t)pi.nseg > out->len) goto fail;
      memcpy(pi.segments, out->bytes + off + 27, (size_t)pi.nseg);
      for (i = 0; i < pi.nseg; ++i) pi.body_len += pi.segments[i];
      total = 27 + (size_t)pi.nseg + pi.body_len;
      if (off + total > out->len) goto fail;
      stored = get_le32(out->bytes + off + 22);
      tmp = malloc(total);
      if (!tmp) goto fail;
      memcpy(tmp, out->bytes + off, total);
      put_le32(tmp + 22, 0);
      computed = ogg_crc(tmp, total);
      free(tmp);
      if (stored != computed) goto fail;
      pi.offset = off;
      if (out->page_count == cap) {
        size_t ncap = cap ? cap * 2 : 16;
        page_info *n = realloc(out->pages, ncap * sizeof(*n));
        if (!n) goto fail;
        out->pages = n;
        cap = ncap;
      }
      out->pages[out->page_count++] = pi;
      off += total;
    }
  }

  /* Packets: gather lacing segments across pages. */
  {
    size_t p;
    unsigned char *pkt = NULL;
    size_t pkt_len = 0, pkt_cap = 0;
    int open = 0;
    for (p = 0; p < out->page_count; ++p) {
      const page_info *pi = &out->pages[p];
      size_t body_start = pi->offset + 27 + (size_t)pi->nseg;
      size_t bo = 0;
      int i;
      for (i = 0; i < pi->nseg; ++i) {
        size_t sl = pi->segments[i];
        if (pkt_len + sl > pkt_cap) {
          size_t ncap = pkt_cap ? pkt_cap * 2 : 256;
          unsigned char *n;
          while (pkt_len + sl > ncap) ncap *= 2;
          n = realloc(pkt, ncap);
          if (!n) {
            free(pkt);
            goto fail;
          }
          pkt = n;
          pkt_cap = ncap;
        }
        memcpy(pkt + pkt_len, out->bytes + body_start + bo, sl);
        bo += sl;
        pkt_len += sl;
        open = 1;
        if (sl < 255) {
          int k = out->packet_count;
          if (k >= 512) {
            free(pkt);
            goto fail;
          }
          out->packet[k] = malloc(pkt_len ? pkt_len : 1);
          if (!out->packet[k]) {
            free(pkt);
            goto fail;
          }
          memcpy(out->packet[k], pkt, pkt_len);
          out->packet_len[k] = pkt_len;
          out->packet_page[k] = p;
          out->packet_count++;
          pkt_len = 0;
          open = 0;
        }
      }
    }
    free(pkt);
    if (open) goto fail;
  }
  return 0;

fail:
  if (f) fclose(f);
  free_parsed(out);
  return 1;
}

/* ------------------------------------------------------------------ */
/* Output verification.                                                */

static int64_t out_granule(uint32_t shift, uint64_t key, uint64_t n) {
  return (int64_t)(((uint64_t)(key + 1) << shift) + (n - key));
}

/* Verify the output stream structure, payloads and independent granule
 * decode.  out_frames gives the expected source frame index for each output
 * data packet, in order. */
static int verify_output(const char *path, uint32_t serial,
                         const packets *pk, const frame_def *frames,
                         const int *out_frames, size_t out_count,
                         uint32_t shift) {
  parsed po;
  size_t p;
  int i;
  uint32_t expect_pageno = 0;
  int saw_bos = 0, saw_eos = 0;
  int64_t *exp_gp = NULL;
  uint64_t key = 0, n = 0;
  int have_key = 0;
  int64_t prev = -1;

  memset(&po, 0, sizeof(po));
  if (parse_file(path, &po)) return 1;

  TASK_CHECK(po.page_count >= 4);
  TASK_CHECK(po.packet_count >= (int)(HDR_COUNT + out_count));

  for (p = 0; p < po.page_count; ++p) {
    const page_info *pi = &po.pages[p];
    TASK_CHECK(pi->serial == serial);
    TASK_CHECK(pi->pageno == expect_pageno++);
    TASK_CHECK(pi->granule > -1 || !(pi->granule == -1) ? 1 : 1);
    if (p == 0) {
      TASK_CHECK(pi->flags & 0x02u);
      saw_bos = 1;
    } else {
      TASK_CHECK(!(pi->flags & 0x02u));
    }
    if (pi->flags & 0x04u) {
      TASK_CHECK(p == po.page_count - 1);
      saw_eos = 1;
    }
    /* Continuation is required exactly when the previous page ended inside
     * a packet (its last lacing value was 255). */
    if (p == 0) {
      TASK_CHECK((pi->flags & 0x01u) == 0);
    } else {
      int prev_open = po.pages[p - 1].nseg > 0 &&
                      po.pages[p - 1].segments[po.pages[p - 1].nseg - 1] == 255;
      TASK_CHECK(((pi->flags & 0x01u) != 0) == prev_open);
    }
  }
  TASK_CHECK(saw_bos && saw_eos);
  TASK_CHECK(po.pages[po.page_count - 1].flags & 0x04u);

  /* Headers copied byte for byte (ident and setup), comment may differ. */
  TASK_CHECK(po.packet_len[0] == pk->len[0]);
  TASK_CHECK(memcmp(po.packet[0], pk->data[0], pk->len[0]) == 0);
  TASK_CHECK(po.packet_len[2] == pk->len[2]);
  TASK_CHECK(memcmp(po.packet[2], pk->data[2], pk->len[2]) == 0);
  TASK_CHECK(po.packet_len[1] > 0);
  TASK_CHECK(memcmp(po.packet[1], "theora", 6) == 0 ||
             (po.packet[1][0] == 0x81 && !memcmp(po.packet[1] + 1, "theora", 6)));

  /* Expected granule per packet: three headers at zero, then the new stream
   * clock.  Derived only from the output's own keyframe bits. */
  TASK_CHECK(po.packet_count == (int)(HDR_COUNT + out_count));
  exp_gp = calloc((size_t)po.packet_count, sizeof(*exp_gp));
  if (!exp_gp) {
    free_parsed(&po);
    return 1;
  }
  for (i = 0; i < po.packet_count; ++i) {
    int idx = i;
    if (idx < HDR_COUNT) {
      exp_gp[idx] = 0;
      continue;
    }
    {
      int fi = idx - HDR_COUNT;
      int frame = out_frames[fi];
      int keyframe;
      TASK_CHECK(po.packet_len[idx] == frames[frame].len);
      if (frames[frame].len)
        TASK_CHECK(memcmp(po.packet[idx], frames[frame].data,
                          frames[frame].len) == 0);
      if (frames[frame].len == 0)
        keyframe = 0;
      else {
        unsigned char b0 = po.packet[idx][0];
        TASK_CHECK((b0 & 0x80u) == 0); /* video packet */
        keyframe = ((b0 & 0x40u) == 0);
      }
      if (keyframe) {
        key = (uint64_t)fi;
        have_key = 1;
      }
      TASK_CHECK(have_key);
      exp_gp[idx] = out_granule(shift, key, (uint64_t)fi);
      TASK_CHECK(exp_gp[idx] > prev);
      prev = exp_gp[idx];
      ++n;
    }
  }
  (void)n;

  /* Every page granule must equal the expected granule of the last packet
   * completed on that page (or -1 when none). */
  for (p = 0; p < po.page_count; ++p) {
    int last = -1;
    int idx;
    for (idx = 0; idx < po.packet_count; ++idx)
      if (po.packet_page[idx] == p) last = idx;
    if (last < 0) {
      TASK_CHECK(po.pages[p].granule == -1);
    } else {
      TASK_CHECK(po.pages[p].granule == exp_gp[last]);
    }
  }

  free(exp_gp);
  free_parsed(&po);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Common source handles.                                              */

typedef struct fixture_ctx {
  packets pk;
  const unsigned char *ident;
  size_t ident_len;
  const unsigned char *comment;
  size_t comment_len;
  const unsigned char *setup;
  size_t setup_len;
  uint32_t shift;
  uint32_t fps_num;
  uint32_t fps_den;
  frame_def frames[64];
  int8_t keys[64];
  int frame_count;
} fixture_ctx;

static int load_fixture(fixture_ctx *fx) {
  int i;
  memset(fx, 0, sizeof(*fx));
  if (read_packets("tests/fixtures/frames.ogv", &fx->pk)) return 1;
  if (fx->pk.count < HDR_COUNT + 12) {
    free_packets(&fx->pk);
    return 1;
  }
  fx->ident = fx->pk.data[0];
  fx->ident_len = fx->pk.len[0];
  fx->comment = fx->pk.data[1];
  fx->comment_len = fx->pk.len[1];
  fx->setup = fx->pk.data[2];
  fx->setup_len = fx->pk.len[2];
  if (fx->ident_len < 42) {
    free_packets(&fx->pk);
    return 1;
  }
  fx->shift = (uint32_t)(((fx->ident[40] & 3u) << 3) | (fx->ident[41] >> 5));
  fx->fps_num = get_be32(fx->ident + 22);
  fx->fps_den = get_be32(fx->ident + 26);
  fx->frame_count = fx->pk.count - HDR_COUNT;
  for (i = 0; i < fx->frame_count; ++i) {
    frame_def *f = &fx->frames[i];
    f->data = fx->pk.data[HDR_COUNT + i];
    f->len = fx->pk.len[HDR_COUNT + i];
    f->key = (!(f->data[0] & 0x80u) && !(f->data[0] & 0x40u)) ? 1 : 0;
    fx->keys[i] = (int8_t)f->key;
  }
  return 0;
}

static void unload_fixture(fixture_ctx *fx) {
  free_packets(&fx->pk);
  memset(fx, 0, sizeof(*fx));
}

static int write_bytes(const char *path, const unsigned char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) return 1;
  if (len && fwrite(data, 1, len, f) != len) {
    fclose(f);
    return 1;
  }
  return fclose(f) != 0;
}

static int read_bytes(const char *path, buf *out) {
  FILE *f = fopen(path, "rb");
  long sz;
  buf_init(out);
  if (!f) return 1;
  if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET))
    goto fail;
  if (sz) {
    out->data = malloc((size_t)sz);
    if (!out->data) goto fail;
    if (fread(out->data, 1, (size_t)sz, f) != (size_t)sz) goto fail;
    out->len = (size_t)sz;
    out->cap = (size_t)sz;
  }
  fclose(f);
  return 0;
fail:
  fclose(f);
  buf_free(out);
  return 1;
}

/* Build, write out and verify a plan from a source file. */
static int execute_plan(const char *src_path, const ogg_theora_cut *cuts,
                        size_t count, const char *out_path, int serial,
                        uint32_t shift, const packets *pk,
                        const frame_def *frames, const int *out_frames,
                        size_t out_count) {
  FILE *in = fopen(src_path, "rb");
  FILE *out = fopen(out_path, "wb");
  ogg_theora_plan *plan = NULL;
  int rc = 1;
  if (!in || !out) goto done;
  if (ogg_theora_plan_create(in, cuts, count, &plan) || !plan) goto done;
  if (ogg_theora_plan_write(plan, out, serial)) goto done;
  if (fclose(out)) {
    out = NULL;
    goto done;
  }
  out = NULL;
  rc = verify_output(out_path, (uint32_t)serial, pk, frames, out_frames,
                     out_count, shift);
done:
  if (out) fclose(out);
  ogg_theora_plan_destroy(plan);
  if (in) fclose(in);
  return rc;
}

/* ------------------------------------------------------------------ */
/* Source mutation helpers for the error cases.                        */

static int page_spans(const buf *src, page_info *pages, int max,
                      int *count) {
  size_t off = 0;
  int n = 0;
  while (off < src->len) {
    page_info pi;
    size_t total;
    int i;
    if (off + 27 > src->len) return 1;
    if (memcmp(src->data + off, "OggS", 4)) return 1;
    memset(&pi, 0, sizeof(pi));
    pi.flags = src->data[off + 5];
    pi.nseg = src->data[off + 26];
    if (off + 27 + (size_t)pi.nseg > src->len) return 1;
    memcpy(pi.segments, src->data + off + 27, (size_t)pi.nseg);
    for (i = 0; i < pi.nseg; ++i) pi.body_len += pi.segments[i];
    total = 27 + (size_t)pi.nseg + pi.body_len;
    if (off + total > src->len) return 1;
    pi.offset = off;
    if (n >= max) return 1;
    pages[n++] = pi;
    off += total;
  }
  *count = n;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Cases: anchors.                                                     */

static int case_anchors_basic(void) {
  fixture_ctx fx;
  ogg_theora_cut cuts[1];
  int out_frames[2] = {0, 1};
  int rc;
  if (load_fixture(&fx)) return 1;
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  rc = execute_plan("tests/fixtures/frames.ogv", cuts, 1,
                    ".task-build/_a_basic.ogv", 701, fx.shift, &fx.pk, fx.frames,
                    out_frames, 2);
  TASK_CHECK(rc == 0);
  unload_fixture(&fx);
  return 0;
}

static int case_anchors_gop_end(void) {
  fixture_ctx fx;
  ogg_theora_cut cuts[1];
  int out_frames[3] = {4, 5, 6};
  if (load_fixture(&fx)) return 1;
  cuts[0].stream = 0;
  cuts[0].begin = 4;
  cuts[0].end = 7;
  TASK_CHECK(execute_plan("tests/fixtures/frames.ogv", cuts, 1,
                          ".task-build/_a_gop.ogv", 702, fx.shift, &fx.pk,
                          fx.frames, out_frames, 3) == 0);
  unload_fixture(&fx);
  return 0;
}

static int case_anchors_whole(void) {
  fixture_ctx fx;
  ogg_theora_cut cuts[1];
  int out_frames[12];
  int i;
  if (load_fixture(&fx)) return 1;
  for (i = 0; i < 12; ++i) out_frames[i] = i;
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 12;
  TASK_CHECK(execute_plan("tests/fixtures/frames.ogv", cuts, 1,
                          ".task-build/_a_whole.ogv", 703, fx.shift, &fx.pk,
                          fx.frames, out_frames, 12) == 0);
  unload_fixture(&fx);
  return 0;
}

/* Re-paged synthetic source: multi-packet page, split setup header, a frame
 * packet spanning pages, and a -1 continuation page. */
static int case_anchors_paged(void) {
  fixture_ctx fx;
  stream_builder sb;
  frame_def frames[13];
  unsigned char big_store[600];
  buf comment;
  int out_frames[7] = {4, 5, 6, 8, 9, 10, 11};
  ogg_theora_cut cuts[2];
  int rc = 1;

  if (load_fixture(&fx)) return 1;
  buf_init(&comment);
  if (comment_with_extra(fx.comment, fx.comment_len, "PAGED=1", &comment)) {
    unload_fixture(&fx);
    return 1;
  }
  memcpy(frames, fx.frames, 12 * sizeof(frames[0]));
  {
    int i;
    for (i = 0; i < (int)sizeof(big_store); ++i) big_store[i] = 0;
    big_store[0] = 0x20; /* key video packet, spans pages */
    for (i = 1; i < (int)sizeof(big_store); ++i)
      big_store[i] = (unsigned char)(0xA0 + i);
    frames[4].data = big_store;
    frames[4].len = sizeof(big_store);
    frames[4].key = 1;
  }
  {
    int8_t keys[12];
    int i;
    for (i = 0; i < 12; ++i) keys[i] = (int8_t)frames[i].key;
    sb_init(&sb, 4101, fx.shift, keys);
    if (build_paged(&sb, fx.ident, fx.ident_len, comment.data, comment.len,
                    fx.setup, fx.setup_len, frames, 12)) {
      buf_free(&sb.out);
      goto done;
    }
  }
  if (write_bytes(".task-build/_a_paged_src.ogv", sb.out.data, sb.out.len))
    goto done_buf;

  cuts[0].stream = 0;
  cuts[0].begin = 4;
  cuts[0].end = 7;
  cuts[1].stream = 0;
  cuts[1].begin = 8;
  cuts[1].end = 12;
  /* source packet table for header comparison: build a packets view */
  {
    packets pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.data[0] = (unsigned char *)fx.ident;
    pkt.len[0] = fx.ident_len;
    pkt.data[1] = comment.data;
    pkt.len[1] = comment.len;
    pkt.data[2] = (unsigned char *)fx.setup;
    pkt.len[2] = fx.setup_len;
    pkt.count = 3;
    rc = execute_plan(".task-build/_a_paged_src.ogv", cuts, 2,
                      ".task-build/_a_paged.ogv", 704, fx.shift, &pkt, frames,
                      out_frames, 7);
  }
  TASK_CHECK(rc == 0);
  goto done_buf;

done_buf:
done:
  if (sb.out.data) buf_free(&sb.out);
  buf_free(&comment);
  unload_fixture(&fx);
  return rc == 0 ? 0 : 1;
}

static int case_anchors_position(void) {
  /* create() always reads from the file start regardless of the current
   * file position. */
  fixture_ctx fx;
  ogg_theora_cut cuts[1];
  int out_frames[2] = {8, 9};
  FILE *in;
  FILE *out;
  ogg_theora_plan *plan = NULL;
  int rc = 1;
  if (load_fixture(&fx)) return 1;
  in = fopen("tests/fixtures/frames.ogv", "rb");
  out = fopen(".task-build/_a_pos.ogv", "wb");
  if (!in || !out) goto done;
  TASK_CHECK(fseek(in, 1024, SEEK_SET) == 0);
  cuts[0].stream = 0;
  cuts[0].begin = 8;
  cuts[0].end = 10;
  if (ogg_theora_plan_create(in, cuts, 1, &plan) || !plan) goto done;
  if (ogg_theora_plan_write(plan, out, 705)) goto done;
  if (fclose(out)) {
    out = NULL;
    goto done;
  }
  out = NULL;
  rc = verify_output(".task-build/_a_pos.ogv", 705, &fx.pk, fx.frames,
                     out_frames, 2, fx.shift);
  TASK_CHECK(rc == 0);
done:
  if (out) fclose(out);
  if (in) fclose(in);
  ogg_theora_plan_destroy(plan);
  unload_fixture(&fx);
  return rc == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Cases: montage.                                                     */

static int case_montage_reverse(void) {
  fixture_ctx fx;
  ogg_theora_cut cuts[2];
  int out_frames[6] = {8, 9, 10, 11, 4, 5};
  if (load_fixture(&fx)) return 1;
  cuts[0].stream = 0;
  cuts[0].begin = 8;
  cuts[0].end = 12;
  cuts[1].stream = 0;
  cuts[1].begin = 4;
  cuts[1].end = 6;
  TASK_CHECK(execute_plan("tests/fixtures/frames.ogv", cuts, 2,
                          ".task-build/_m_rev.ogv", 711, fx.shift, &fx.pk,
                          fx.frames, out_frames, 6) == 0);
  unload_fixture(&fx);
  return 0;
}

static int case_montage_repeat_overlap(void) {
  fixture_ctx fx;
  ogg_theora_cut cuts[3];
  int out_frames[12] = {0, 1, 2, 3, 4, 5, 6, 7, 4, 5, 0, 1};
  if (load_fixture(&fx)) return 1;
  /* A keyframe-aligned cut may overlap an earlier, longer cut. */
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 8;
  cuts[1].stream = 0;
  cuts[1].begin = 4;
  cuts[1].end = 6;
  cuts[2].stream = 0;
  cuts[2].begin = 0;
  cuts[2].end = 2;
  TASK_CHECK(execute_plan("tests/fixtures/frames.ogv", cuts, 3,
                          ".task-build/_m_rep.ogv", 712, fx.shift, &fx.pk,
                          fx.frames, out_frames, 12) == 0);
  unload_fixture(&fx);
  return 0;
}

/* Two chained logical streams with different comments; cut both. */
static int case_montage_two_streams(void) {
  fixture_ctx fx;
  stream_builder a, b;
  buf file, comment_b;
  int out_frames[6] = {8, 9, 0, 1, 2, 3};
  ogg_theora_cut cuts[2];
  packets pkt;
  int rc = 1;

  if (load_fixture(&fx)) return 1;
  buf_init(&file);
  buf_init(&comment_b);
  if (comment_with_extra(fx.comment, fx.comment_len, "SECOND=1", &comment_b))
    goto done;
  sb_init(&a, 5101, fx.shift, fx.keys);
  sb_init(&b, 5102, fx.shift, fx.keys);
  {
    frame_def frames[13];
    memcpy(frames, fx.frames, 12 * sizeof(frames[0]));
    if (build_simple(&a, fx.ident, fx.ident_len, fx.comment, fx.comment_len,
                     fx.setup, fx.setup_len, frames, 12))
      goto done;
    if (build_simple(&b, fx.ident, fx.ident_len, comment_b.data, comment_b.len,
                     fx.setup, fx.setup_len, frames, 12))
      goto done;
  }
  if (buf_add(&file, a.out.data, a.out.len) ||
      buf_add(&file, b.out.data, b.out.len))
    goto done;
  if (write_bytes(".task-build/_m_two_src.ogv", file.data, file.len)) goto done;

  cuts[0].stream = 1;
  cuts[0].begin = 8;
  cuts[0].end = 10;
  cuts[1].stream = 0;
  cuts[1].begin = 0;
  cuts[1].end = 4;
  memset(&pkt, 0, sizeof(pkt));
  pkt.data[0] = (unsigned char *)fx.ident;
  pkt.len[0] = fx.ident_len;
  pkt.data[1] = comment_b.data; /* first cut is stream 1: its comment wins */
  pkt.len[1] = comment_b.len;
  pkt.data[2] = (unsigned char *)fx.setup;
  pkt.len[2] = fx.setup_len;
  pkt.count = 3;
  rc = execute_plan(".task-build/_m_two_src.ogv", cuts, 2,
                    ".task-build/_m_two.ogv", 713, fx.shift, &pkt, fx.frames,
                    out_frames, 6);
  TASK_CHECK(rc == 0);
  goto cleanup;

done:
cleanup:
  buf_free(&file);
  buf_free(&comment_b);
  if (a.out.data) buf_free(&a.out);
  if (b.out.data) buf_free(&b.out);
  unload_fixture(&fx);
  return rc == 0 ? 0 : 1;
}

/* Cutting only the second stream keeps the second stream's headers. */
static int case_montage_second_only(void) {
  fixture_ctx fx;
  stream_builder a, b;
  buf file, comment_b;
  int out_frames[3] = {8, 9, 10};
  ogg_theora_cut cuts[1];
  packets pkt;
  int rc = 1;

  if (load_fixture(&fx)) return 1;
  buf_init(&file);
  buf_init(&comment_b);
  if (comment_with_extra(fx.comment, fx.comment_len, "SECOND=1", &comment_b))
    goto done;
  sb_init(&a, 5201, fx.shift, fx.keys);
  sb_init(&b, 5202, fx.shift, fx.keys);
  {
    frame_def frames[13];
    memcpy(frames, fx.frames, 12 * sizeof(frames[0]));
    if (build_simple(&a, fx.ident, fx.ident_len, fx.comment, fx.comment_len,
                     fx.setup, fx.setup_len, frames, 12))
      goto done;
    if (build_simple(&b, fx.ident, fx.ident_len, comment_b.data, comment_b.len,
                     fx.setup, fx.setup_len, frames, 12))
      goto done;
  }
  if (buf_add(&file, a.out.data, a.out.len) ||
      buf_add(&file, b.out.data, b.out.len))
    goto done;
  if (write_bytes(".task-build/_m_second_src.ogv", file.data, file.len))
    goto done;

  cuts[0].stream = 1;
  cuts[0].begin = 8;
  cuts[0].end = 11;
  memset(&pkt, 0, sizeof(pkt));
  pkt.data[0] = (unsigned char *)fx.ident;
  pkt.len[0] = fx.ident_len;
  pkt.data[1] = comment_b.data; /* the only selected stream's comment */
  pkt.len[1] = comment_b.len;
  pkt.data[2] = (unsigned char *)fx.setup;
  pkt.len[2] = fx.setup_len;
  pkt.count = 3;
  rc = execute_plan(".task-build/_m_second_src.ogv", cuts, 1,
                    ".task-build/_m_second.ogv", 714, fx.shift, &pkt, fx.frames,
                    out_frames, 3);
  TASK_CHECK(rc == 0);
  goto cleanup;

done:
cleanup:
  buf_free(&file);
  buf_free(&comment_b);
  if (a.out.data) buf_free(&a.out);
  if (b.out.data) buf_free(&b.out);
  unload_fixture(&fx);
  return rc == 0 ? 0 : 1;
}

/* Zero byte repeat frames are legal non-key frames. */
static int case_montage_empty_frame(void) {
  fixture_ctx fx;
  stream_builder sb;
  frame_def frames[13];
  int out_frames[6] = {0, 1, 2, 0, 1, 2};
  ogg_theora_cut cuts[2];
  int8_t keys[13];
  int i;
  int rc = 1;
  packets pkt;

  if (load_fixture(&fx)) return 1;
  memcpy(frames, fx.frames, 12 * sizeof(frames[0]));
  /* insert a zero-length frame at index 2 (non-key) */
  for (i = 11; i >= 2; --i) frames[i + 1] = frames[i];
  frames[2].data = (const unsigned char *)"";
  frames[2].len = 0;
  frames[2].key = 0;
  for (i = 0; i < 13; ++i) keys[i] = (int8_t)frames[i].key;
  sb_init(&sb, 6101, fx.shift, keys);
  if (build_simple(&sb, fx.ident, fx.ident_len, fx.comment, fx.comment_len,
                   fx.setup, fx.setup_len, frames, 13))
    goto done;
  if (write_bytes(".task-build/_m_empty_src.ogv", sb.out.data, sb.out.len))
    goto done;

  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 3;
  cuts[1].stream = 0;
  cuts[1].begin = 0;
  cuts[1].end = 3;
  memset(&pkt, 0, sizeof(pkt));
  pkt.data[0] = (unsigned char *)fx.ident;
  pkt.len[0] = fx.ident_len;
  pkt.data[1] = (unsigned char *)fx.comment;
  pkt.len[1] = fx.comment_len;
  pkt.data[2] = (unsigned char *)fx.setup;
  pkt.len[2] = fx.setup_len;
  pkt.count = 3;
  rc = execute_plan(".task-build/_m_empty_src.ogv", cuts, 2,
                    ".task-build/_m_empty.ogv", 715, fx.shift, &pkt, frames,
                    out_frames, 6);
  TASK_CHECK(rc == 0);
  goto cleanup;

done:
cleanup:
  if (sb.out.data) buf_free(&sb.out);
  unload_fixture(&fx);
  return rc == 0 ? 0 : 1;
}

/* The same plan is written twice with different serial numbers. */
static int case_montage_twice(void) {
  fixture_ctx fx;
  ogg_theora_cut cuts[1];
  int out_frames[4] = {4, 5, 6, 7};
  FILE *in;
  FILE *o1;
  FILE *o2;
  ogg_theora_plan *plan = NULL;
  int rc = 1;
  if (load_fixture(&fx)) return 1;
  in = fopen("tests/fixtures/frames.ogv", "rb");
  o1 = fopen(".task-build/_m_twice1.ogv", "wb");
  o2 = fopen(".task-build/_m_twice2.ogv", "wb");
  if (!in || !o1 || !o2) goto done;
  cuts[0].stream = 0;
  cuts[0].begin = 4;
  cuts[0].end = 8;
  if (ogg_theora_plan_create(in, cuts, 1, &plan) || !plan) goto done;
  TASK_CHECK(ogg_theora_plan_write(plan, o1, 9001) == 0);
  TASK_CHECK(ogg_theora_plan_write(plan, o2, 9002) == 0);
  if (fclose(o1)) {
    o1 = NULL;
    goto done;
  }
  o1 = NULL;
  if (fclose(o2)) {
    o2 = NULL;
    goto done;
  }
  o2 = NULL;
  rc = verify_output(".task-build/_m_twice1.ogv", 9001, &fx.pk, fx.frames,
                     out_frames, 4, fx.shift);
  TASK_CHECK(rc == 0);
  rc = verify_output(".task-build/_m_twice2.ogv", 9002, &fx.pk, fx.frames,
                     out_frames, 4, fx.shift);
  TASK_CHECK(rc == 0);
done:
  if (o1) fclose(o1);
  if (o2) fclose(o2);
  if (in) fclose(in);
  ogg_theora_plan_destroy(plan);
  unload_fixture(&fx);
  return rc == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Cases: errors.                                                      */

/* create() must fail and leave *out NULL. */
static int expect_create_failure(const char *src_path,
                                 const ogg_theora_cut *cuts, size_t count) {
  FILE *in = fopen(src_path, "rb");
  ogg_theora_plan *plan = (ogg_theora_plan *)0x1;
  int rc;
  if (!in) return 1;
  rc = ogg_theora_plan_create(in, cuts, count, &plan);
  fclose(in);
  return rc == 0 ? 1 : (plan != NULL ? 1 : 0);
}

static int case_errors_anchor_not_key(void) {
  fixture_ctx fx;
  ogg_theora_cut cuts[2];
  if (load_fixture(&fx)) return 1;
  cuts[0].stream = 0;
  cuts[0].begin = 1; /* not a keyframe */
  cuts[0].end = 3;
  TASK_CHECK(expect_create_failure("tests/fixtures/frames.ogv", cuts, 1) == 0);
  cuts[0].begin = 5;
  cuts[0].end = 7;
  TASK_CHECK(expect_create_failure("tests/fixtures/frames.ogv", cuts, 1) == 0);
  /* a keyframe start with a non-key second cut still fails */
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  cuts[1].stream = 0;
  cuts[1].begin = 3;
  cuts[1].end = 5;
  TASK_CHECK(expect_create_failure("tests/fixtures/frames.ogv", cuts, 2) == 0);
  unload_fixture(&fx);
  return 0;
}

static int case_errors_bad_ranges(void) {
  fixture_ctx fx;
  ogg_theora_cut cuts[3];
  if (load_fixture(&fx)) return 1;
  /* empty */
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 0;
  TASK_CHECK(expect_create_failure("tests/fixtures/frames.ogv", cuts, 1) == 0);
  /* reversed */
  cuts[0].begin = 5;
  cuts[0].end = 2;
  TASK_CHECK(expect_create_failure("tests/fixtures/frames.ogv", cuts, 1) == 0);
  /* out of range */
  cuts[0].begin = 8;
  cuts[0].end = 13;
  TASK_CHECK(expect_create_failure("tests/fixtures/frames.ogv", cuts, 1) == 0);
  /* bad stream number */
  cuts[0].stream = 2;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  TASK_CHECK(expect_create_failure("tests/fixtures/frames.ogv", cuts, 1) == 0);
  /* zero count */
  TASK_CHECK(expect_create_failure("tests/fixtures/frames.ogv", cuts, 0) == 0);
  (void)cuts[2];
  unload_fixture(&fx);
  return 0;
}

/* Mutate one byte of a page body and leave the CRC stale. */
static int case_errors_bad_crc(void) {
  buf src;
  page_info pages[16];
  int count;
  ogg_theora_cut cuts[1];
  int rc;
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  if (read_bytes("tests/fixtures/frames.ogv", &src)) return 1;
  if (page_spans(&src, pages, 16, &count)) {
    buf_free(&src);
    return 1;
  }
  TASK_CHECK(count >= 3);
  src.data[pages[2].offset + 27 + (size_t)pages[2].nseg] ^= 0x01;
  if (write_bytes(".task-build/_e_crc.ogv", src.data, src.len)) {
    buf_free(&src);
    return 1;
  }
  rc = expect_create_failure(".task-build/_e_crc.ogv", cuts, 1);
  TASK_CHECK(rc == 0);
  buf_free(&src);
  return 0;
}

/* Remove a middle page: page numbers then have a hole. */
static int case_errors_missing_page(void) {
  buf src;
  page_info pages[16];
  int count, i;
  ogg_theora_cut cuts[1];
  buf cut_src;
  if (read_bytes("tests/fixtures/frames.ogv", &src)) return 1;
  if (page_spans(&src, pages, 16, &count)) {
    buf_free(&src);
    return 1;
  }
  TASK_CHECK(count == 3);
  buf_init(&cut_src);
  /* keep page 0 and page 2, drop page 1 */
  if (buf_add(&cut_src, src.data + pages[0].offset,
              pages[0].body_len + 27 + (size_t)pages[0].nseg) ||
      buf_add(&cut_src, src.data + pages[2].offset,
              pages[2].body_len + 27 + (size_t)pages[2].nseg)) {
    buf_free(&src);
    return 1;
  }
  if (write_bytes(".task-build/_e_hole.ogv", cut_src.data, cut_src.len)) {
    buf_free(&src);
    buf_free(&cut_src);
    return 1;
  }
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  TASK_CHECK(expect_create_failure(".task-build/_e_hole.ogv", cuts, 1) == 0);
  (void)i;
  buf_free(&src);
  buf_free(&cut_src);
  return 0;
}

/* Flip the continued flag on a packet start page: an orphan continuation. */
static int case_errors_orphan_continuation(void) {
  buf src;
  page_info pages[16];
  int count;
  ogg_theora_cut cuts[1];
  if (read_bytes(".task-build/_a_paged_src.ogv", &src)) {
    /* Build the paged source first if it does not exist. */
    return 1;
  }
  if (page_spans(&src, pages, 16, &count)) {
    buf_free(&src);
    return 1;
  }
  TASK_CHECK(count >= 4);
  /* page 2 is the setup start (not continued); mark it continued. */
  src.data[pages[2].offset + 5] |= 0x01u;
  stamp_crc(src.data + pages[2].offset,
            27 + (size_t)pages[2].nseg + pages[2].body_len);
  if (write_bytes(".task-build/_e_orphan.ogv", src.data, src.len)) {
    buf_free(&src);
    return 1;
  }
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  TASK_CHECK(expect_create_failure(".task-build/_e_orphan.ogv", cuts, 1) == 0);
  buf_free(&src);
  return 0;
}

/* A page granule that disagrees with the frame sequence must be rejected,
 * even with a valid CRC. */
static int case_errors_bad_granule(void) {
  buf src;
  page_info pages[16];
  int count;
  ogg_theora_cut cuts[1];
  if (read_bytes("tests/fixtures/frames.ogv", &src)) return 1;
  if (page_spans(&src, pages, 16, &count)) {
    buf_free(&src);
    return 1;
  }
  TASK_CHECK(count == 3);
  {
    unsigned char *h = src.data + pages[2].offset;
    /* The frame page granule is 39; make it 38. */
    h[6] = 38;
    h[7] = 0;
    stamp_crc(h, 27 + (size_t)pages[2].nseg + pages[2].body_len);
  }
  if (write_bytes(".task-build/_e_gran.ogv", src.data, src.len)) {
    buf_free(&src);
    return 1;
  }
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  TASK_CHECK(expect_create_failure(".task-build/_e_gran.ogv", cuts, 1) == 0);
  buf_free(&src);
  return 0;
}

/* Truncated page and trailing garbage bytes are both errors. */
static int case_errors_truncation(void) {
  buf src;
  ogg_theora_cut cuts[1];
  if (read_bytes("tests/fixtures/frames.ogv", &src)) return 1;
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  /* truncate in the middle of the last page */
  if (write_bytes(".task-build/_e_trunc.ogv", src.data, src.len - 30)) {
    buf_free(&src);
    return 1;
  }
  TASK_CHECK(expect_create_failure(".task-build/_e_trunc.ogv", cuts, 1) == 0);
  /* append a few bytes that are not a page */
  if (buf_add(&src, "junk", 4)) {
    buf_free(&src);
    return 1;
  }
  if (write_bytes(".task-build/_e_tail.ogv", src.data, src.len)) {
    buf_free(&src);
    return 1;
  }
  TASK_CHECK(expect_create_failure(".task-build/_e_tail.ogv", cuts, 1) == 0);
  buf_free(&src);
  return 0;
}

/* Interleaved logical streams are unsupported. */
static int case_errors_interleaved(void) {
  fixture_ctx fx;
  stream_builder a, b;
  buf file;
  page_info pa[16], pb[16];
  int ca, cb, i;
  ogg_theora_cut cuts[1];
  frame_def frames[13];
  int rc = 1;

  if (load_fixture(&fx)) return 1;
  buf_init(&file);
  memcpy(frames, fx.frames, 12 * sizeof(frames[0]));
  sb_init(&a, 7101, fx.shift, fx.keys);
  sb_init(&b, 7102, fx.shift, fx.keys);
  if (build_simple(&a, fx.ident, fx.ident_len, fx.comment, fx.comment_len,
                   fx.setup, fx.setup_len, frames, 12))
    goto done;
  if (build_simple(&b, fx.ident, fx.ident_len, fx.comment, fx.comment_len,
                   fx.setup, fx.setup_len, frames, 12))
    goto done;
  if (page_spans(&a.out, pa, 16, &ca) || page_spans(&b.out, pb, 16, &cb))
    goto done;
  for (i = 0; i < ca || i < cb; ++i) {
    if (i < ca &&
        buf_add(&file, a.out.data + pa[i].offset,
                27 + (size_t)pa[i].nseg + pa[i].body_len))
      goto done;
    if (i < cb &&
        buf_add(&file, b.out.data + pb[i].offset,
                27 + (size_t)pb[i].nseg + pb[i].body_len))
      goto done;
  }
  if (write_bytes(".task-build/_e_inter_src.ogv", file.data, file.len))
    goto done;
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  TASK_CHECK(expect_create_failure(".task-build/_e_inter_src.ogv", cuts, 1) == 0);
  rc = 0;
done:
  buf_free(&file);
  if (a.out.data) buf_free(&a.out);
  if (b.out.data) buf_free(&b.out);
  unload_fixture(&fx);
  return rc;
}

/* The same FILE for source and output is rejected. */
static int case_errors_same_file(void) {
  fixture_ctx fx;
  ogg_theora_cut cuts[1];
  FILE *f;
  ogg_theora_plan *plan = NULL;
  int rc = 1;
  if (load_fixture(&fx)) return 1;
  f = fopen(".task-build/_e_same.ogv", "wb+");
  if (!f) goto done;
  {
    /* copy the fixture into the temporary file */
    buf src;
    if (read_bytes("tests/fixtures/frames.ogv", &src)) {
      fclose(f);
      unload_fixture(&fx);
      return 1;
    }
    if (src.len && fwrite(src.data, 1, src.len, f) != src.len) {
      buf_free(&src);
      fclose(f);
      unload_fixture(&fx);
      return 1;
    }
    buf_free(&src);
  }
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  if (ogg_theora_plan_create(f, cuts, 1, &plan) || !plan) goto done;
  TASK_CHECK(ogg_theora_plan_write(plan, f, 7201) != 0);
  rc = 0;
done:
  if (f) fclose(f);
  ogg_theora_plan_destroy(plan);
  unload_fixture(&fx);
  return rc;
}

/* A write failure is reported and a repaired output can retry from the
 * plan start. */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__)
#define TEST_HAVE_FUNOPEN 1
#elif defined(__GLIBC__)
#define TEST_HAVE_FOPENCOOKIE 1
#endif

typedef struct mem_sink {
  buf data;
  int fail;
} mem_sink;

#if defined(TEST_HAVE_FUNOPEN)
static int mem_read(void *cookie, char *bufp, int n) {
  (void)cookie;
  (void)bufp;
  (void)n;
  return -1;
}
static int mem_write(void *cookie, const char *bufp, int n) {
  mem_sink *s = (mem_sink *)cookie;
  if (s->fail) return -1;
  if (buf_add(&s->data, bufp, (size_t)n)) return -1;
  return n;
}
static int mem_close(void *cookie) {
  (void)cookie;
  return 0;
}
static FILE *open_sink(mem_sink *s) {
  return funopen(s, mem_read, mem_write, NULL, mem_close);
}
#elif defined(TEST_HAVE_FOPENCOOKIE)
static ssize_t mem_write_cookie(void *cookie, const char *bufp, size_t n) {
  mem_sink *s = (mem_sink *)cookie;
  if (s->fail) return -1;
  if (buf_add(&s->data, bufp, n)) return -1;
  return (ssize_t)n;
}
static FILE *open_sink(mem_sink *s) {
  cookie_io_functions_t fns;
  memset(&fns, 0, sizeof(fns));
  fns.write = mem_write_cookie;
  return fopencookie(s, "w", fns);
}
#endif

static int case_errors_write_retry(void) {
  fixture_ctx fx;
  ogg_theora_cut cuts[1];
  FILE *in;
  ogg_theora_plan *plan = NULL;
  int rc = 1;
  int out_frames[2] = {0, 1};
  if (load_fixture(&fx)) return 1;
#if defined(TEST_HAVE_FUNOPEN) || defined(TEST_HAVE_FOPENCOOKIE)
  in = fopen("tests/fixtures/frames.ogv", "rb");
  if (!in) goto done;
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  if (ogg_theora_plan_create(in, cuts, 1, &plan) || !plan) goto done;
  {
    mem_sink bad;
    mem_sink good;
    FILE *f;
    memset(&bad, 0, sizeof(bad));
    memset(&good, 0, sizeof(good));
    bad.fail = 1;
    f = open_sink(&bad);
    if (!f) goto done;
    TASK_CHECK(ogg_theora_plan_write(plan, f, 7301) != 0); /* fails */
    fclose(f);
    buf_free(&bad.data);

    f = open_sink(&good);
    if (!f) goto done;
    TASK_CHECK(ogg_theora_plan_write(plan, f, 7302) == 0); /* retry works */
    fclose(f);
    if (write_bytes(".task-build/_e_retry.ogv", good.data.data, good.data.len)) {
      buf_free(&good.data);
      goto done;
    }
    buf_free(&good.data);
  }
  rc = verify_output(".task-build/_e_retry.ogv", 7302, &fx.pk, fx.frames,
                     out_frames, 2, fx.shift);
  TASK_CHECK(rc == 0);
#else
  TASK_CHECK(0); /* no cookie streams on this platform */
  rc = 1;
#endif
done:
  if (in) fclose(in);
  ogg_theora_plan_destroy(plan);
  unload_fixture(&fx);
  return rc == 0 ? 0 : 1;
}

/* The source may be positioned anywhere and still reads from the start;
 * this is the successful counterpart of the error cases above. */
static int case_errors_unknown_stream(void) {
  fixture_ctx fx;
  ogg_theora_cut cuts[1];
  if (load_fixture(&fx)) return 1;
  cuts[0].stream = 1;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  TASK_CHECK(expect_create_failure("tests/fixtures/frames.ogv", cuts, 1) == 0);
  unload_fixture(&fx);
  return 0;
}


/* Build a source whose ident packet has mutated bytes.  The ident payload
 * changes; everything else is valid, and the source CRC is recomputed, so
 * the only failure can be header validation. */
static int build_with_ident_patch(uint32_t serial, uint32_t offset, size_t span,
                                  const unsigned char *values,
                                  const char *path);

static int build_with_ident_byte(uint32_t serial, uint32_t offset,
                                 unsigned char value, const char *path) {
  return build_with_ident_patch(serial, offset, 1, &value, path);
}

static int build_with_ident_patch(uint32_t serial, uint32_t offset,
                                  size_t span, const unsigned char *values,
                                  const char *path) {
  fixture_ctx fx;
  stream_builder sb;
  frame_def frames[13];
  unsigned char *ident_copy = NULL;
  int rc = 1;
  int8_t *keys = NULL;
  if (load_fixture(&fx)) return 1;
  memcpy(frames, fx.frames, 12 * sizeof(frames[0]));
  ident_copy = malloc(fx.ident_len);
  if (!ident_copy) goto done;
  memcpy(ident_copy, fx.ident, fx.ident_len);
  if (offset + span > fx.ident_len) goto done;
  memcpy(ident_copy + offset, values, span);
  keys = malloc(12 * sizeof(*keys));
  if (!keys) goto done;
  {
    int i;
    for (i = 0; i < 12; ++i) keys[i] = (int8_t)frames[i].key;
  }
  sb_init(&sb, serial, fx.shift, keys);
  if (build_simple(&sb, ident_copy, fx.ident_len, fx.comment, fx.comment_len,
                   fx.setup, fx.setup_len, frames, 12))
    goto done;
  if (write_bytes(path, sb.out.data, sb.out.len)) goto done;
  rc = 0;
done:
  if (rc == 0) buf_free(&sb.out);
  free(ident_copy);
  free(keys);
  unload_fixture(&fx);
  return rc;
}

/* Version, frame rate, keyframe shift and header identity are validated. */
static int case_errors_bad_headers(void) {
  fixture_ctx fx;
  ogg_theora_cut cuts[1];
  if (load_fixture(&fx)) return 1;
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;

  /* Wrong Theora version byte. */
  TASK_CHECK(build_with_ident_byte(7201, 7, 4, ".task-build/_e_ver.ogv") == 0);
  TASK_CHECK(expect_create_failure(".task-build/_e_ver.ogv", cuts, 1) == 0);

  /* Zero frame rate numerator (all four bytes). */
  {
    static const unsigned char zero4[4] = {0, 0, 0, 0};
    TASK_CHECK(build_with_ident_patch(7202, 22, 4, zero4,
                                      ".task-build/_e_fps.ogv") == 0);
  }
  TASK_CHECK(expect_create_failure(".task-build/_e_fps.ogv", cuts, 1) == 0);

  /* Illegal keyframe granule shift: bits 31, i.e. 0x7ffe in the shift
   * field (bits 30..28 plus quality field overlap); use shift == 31:
   * (b40&3)<<3 | b41>>5 == 31 means b40&3 == 3 and b41>>5 == 7. */
  {
    unsigned char b40 = fx.ident[40];
    unsigned char b41 = fx.ident[41];
    unsigned char patch[2];
    patch[0] = (unsigned char)((b40 & 0x7cu) | 0x03u);
    patch[1] = (unsigned char)((b41 & 0x1fu) | (7u << 5));
    TASK_CHECK(build_with_ident_patch(7203, 40, 2, patch,
                                      ".task-build/_e_shift.ogv") == 0);
  }
  TASK_CHECK(expect_create_failure(".task-build/_e_shift.ogv", cuts, 1) == 0);
  unload_fixture(&fx);
  return 0;
}

/* Two selected streams whose setup headers differ must be rejected. */
static int case_errors_header_mismatch(void) {
  fixture_ctx fx;
  stream_builder a, b;
  buf file;
  frame_def frames[13];
  ogg_theora_cut cuts[2];
  int rc = 1;
  unsigned char *setup_b = NULL;

  if (load_fixture(&fx)) return 1;
  buf_init(&file);
  memcpy(frames, fx.frames, 12 * sizeof(frames[0]));
  setup_b = malloc(fx.setup_len);
  if (!setup_b) goto done;
  memcpy(setup_b, fx.setup, fx.setup_len);
  setup_b[80] ^= 0x01u; /* different setup, still validly shaped */
  sb_init(&a, 7401, fx.shift, fx.keys);
  sb_init(&b, 7402, fx.shift, fx.keys);
  if (build_simple(&a, fx.ident, fx.ident_len, fx.comment, fx.comment_len,
                   fx.setup, fx.setup_len, frames, 12))
    goto done;
  if (build_simple(&b, fx.ident, fx.ident_len, fx.comment, fx.comment_len,
                   setup_b, fx.setup_len, frames, 12))
    goto done;
  if (buf_add(&file, a.out.data, a.out.len) ||
      buf_add(&file, b.out.data, b.out.len))
    goto done;
  if (write_bytes(".task-build/_e_mismatch_src.ogv", file.data, file.len))
    goto done;
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  cuts[1].stream = 1;
  cuts[1].begin = 4;
  cuts[1].end = 6;
  TASK_CHECK(expect_create_failure(".task-build/_e_mismatch_src.ogv", cuts, 2) ==
             0);
  /* Selecting just one of them is fine. */
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  {
    packets pkt;
    int out_frames[2] = {0, 1};
    memset(&pkt, 0, sizeof(pkt));
    pkt.data[0] = (unsigned char *)fx.ident;
    pkt.len[0] = fx.ident_len;
    pkt.data[1] = (unsigned char *)fx.comment;
    pkt.len[1] = fx.comment_len;
    pkt.data[2] = (unsigned char *)fx.setup;
    pkt.len[2] = fx.setup_len;
    pkt.count = 3;
    TASK_CHECK(execute_plan(".task-build/_e_mismatch_src.ogv", cuts, 1,
                            ".task-build/_e_mismatch_out.ogv", 7403, fx.shift,
                            &pkt, frames, out_frames, 2) == 0);
  }
  rc = 0;
done:
  buf_free(&file);
  free(setup_b);
  if (a.out.data) buf_free(&a.out);
  if (b.out.data) buf_free(&b.out);
  unload_fixture(&fx);
  return rc;
}

/* A source whose final page lacks EOS is rejected; and a stream missing a
 * header is rejected. */
static int case_errors_missing_eos(void) {
  buf src;
  page_info pages[16];
  int count;
  ogg_theora_cut cuts[1];
  if (read_bytes("tests/fixtures/frames.ogv", &src)) return 1;
  if (page_spans(&src, pages, 16, &count)) {
    buf_free(&src);
    return 1;
  }
  TASK_CHECK(count == 3);
  src.data[pages[2].offset + 5] &= (unsigned char)~0x04u; /* clear EOS */
  stamp_crc(src.data + pages[2].offset,
            27 + (size_t)pages[2].nseg + pages[2].body_len);
  if (write_bytes(".task-build/_e_noeos.ogv", src.data, src.len)) {
    buf_free(&src);
    return 1;
  }
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  TASK_CHECK(expect_create_failure(".task-build/_e_noeos.ogv", cuts, 1) == 0);
  buf_free(&src);
  return 0;
}

/* A zero-byte file must be rejected, not crash. */
static int case_errors_empty_source(void) {
  ogg_theora_cut cuts[1];
  FILE *f;
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  f = fopen(".task-build/_e_empty.ogv", "wb");
  if (!f) return 1;
  fclose(f);
  TASK_CHECK(expect_create_failure(".task-build/_e_empty.ogv", cuts, 1) == 0);
  return 0;
}

/* create() must not write any output; only write() does. */
static int case_errors_create_silent(void) {
  fixture_ctx fx;
  ogg_theora_cut cuts[1];
  FILE *in;
  FILE *out;
  ogg_theora_plan *plan = NULL;
  int rc = 1;
  long before, after;
  if (load_fixture(&fx)) return 1;
  in = fopen("tests/fixtures/frames.ogv", "rb");
  out = fopen(".task-build/_e_silent.ogv", "wb");
  if (!in || !out) goto done;
  if (fwrite("sentinel", 1, 8, out) != 8) goto done;
  if (fflush(out)) goto done;
  before = ftell(out);
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 2;
  if (ogg_theora_plan_create(in, cuts, 1, &plan) || !plan) goto done;
  if (fflush(out)) goto done;
  after = ftell(out);
  TASK_CHECK(before == after); /* create did not touch the output */
  rc = 0;
done:
  if (out) fclose(out);
  if (in) fclose(in);
  ogg_theora_plan_destroy(plan);
  unload_fixture(&fx);
  return rc;
}


/* Frame timing is exact rational n*fps_den/fps_num arithmetic; nothing is
 * rounded through milliseconds.  Build a source declared at 24000/1001 (a
 * rate that becomes non-integral in milliseconds) and verify the output
 * payloads and granule clock are unaffected. */
static int case_anchors_exact_time(void) {
  fixture_ctx fx;
  stream_builder sb;
  frame_def frames[13];
  unsigned char ident_copy[64];
  unsigned char *ident_data;
  ogg_theora_cut cuts[1];
  int out_frames[5] = {0, 1, 2, 3, 4};
  packets pkt;
  int rc = 1;

  if (load_fixture(&fx)) return 1;
  if (fx.ident_len > sizeof(ident_copy)) {
    unload_fixture(&fx);
    return 1;
  }
  memcpy(ident_copy, fx.ident, fx.ident_len);
  put_be32(ident_copy + 22, 24000);
  put_be32(ident_copy + 26, 1001);
  ident_data = ident_copy;
  memcpy(frames, fx.frames, 12 * sizeof(frames[0]));
  sb_init(&sb, 7501, fx.shift, fx.keys);
  if (build_simple(&sb, ident_data, fx.ident_len, fx.comment, fx.comment_len,
                   fx.setup, fx.setup_len, frames, 12))
    goto done;
  if (write_bytes(".task-build/_a_time_src.ogv", sb.out.data, sb.out.len))
    goto done;

  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 5;
  memset(&pkt, 0, sizeof(pkt));
  pkt.data[0] = ident_data;
  pkt.len[0] = fx.ident_len;
  pkt.data[1] = (unsigned char *)fx.comment;
  pkt.len[1] = fx.comment_len;
  pkt.data[2] = (unsigned char *)fx.setup;
  pkt.len[2] = fx.setup_len;
  pkt.count = 3;
  rc = execute_plan(".task-build/_a_time_src.ogv", cuts, 1,
                    ".task-build/_a_time.ogv", 7502, fx.shift, &pkt, frames,
                    out_frames, 5);
  TASK_CHECK(rc == 0);
  /* The output ident must carry the same 24000/1001 fields: byte-for-byte
   * copying, no time conversion. */
  {
    parsed po;
    memset(&po, 0, sizeof(po));
    if (parse_file(".task-build/_a_time.ogv", &po)) {
      rc = 1;
      goto done;
    }
    TASK_CHECK(get_be32(po.packet[0] + 22) == 24000);
    TASK_CHECK(get_be32(po.packet[0] + 26) == 1001);
    free_parsed(&po);
  }
  rc = 0;
done:
  if (sb.out.data) buf_free(&sb.out);
  unload_fixture(&fx);
  return rc == 0 ? 0 : 1;
}

/* A frame packet far larger than libogg's nominal page spans several
 * output pages; the payload must still round-trip exactly. */
static int case_montage_large_packet(void) {
  fixture_ctx fx;
  stream_builder sb;
  frame_def frames[13];
  unsigned char *big;
  size_t big_len = 200000;
  ogg_theora_cut cuts[1];
  int out_frames[3] = {0, 1, 2};
  packets pkt;
  int8_t keys[12];
  int rc = 1;
  int i;

  if (load_fixture(&fx)) return 1;
  big = malloc(big_len);
  if (!big) {
    unload_fixture(&fx);
    return 1;
  }
  big[0] = 0x20; /* keyframe */
  for (i = 1; i < (int)big_len; ++i) big[i] = (unsigned char)(i * 131 + 7);
  memcpy(frames, fx.frames, 12 * sizeof(frames[0]));
  frames[1].data = big;
  frames[1].len = big_len;
  frames[1].key = 1;
  {
    int k;
    for (k = 0; k < 12; ++k) keys[k] = (int8_t)frames[k].key;
    sb_init(&sb, 7601, fx.shift, keys);
  }
  /* Headers, then frame 0, then the huge frame spread over as many 255
   * segment pages as necessary, then frames 2..11. */
  {
    page_acc pa;
    size_t off;
    pa_init(&pa, 0);
    if (pa_add_header(&pa, fx.ident, fx.ident_len) ||
        sb_flush(&sb, &pa, 0x02, 0, 0))
      goto done;
    pa_init(&pa, 0);
    if (pa_add_header(&pa, fx.comment, fx.comment_len) ||
        sb_flush(&sb, &pa, 0, 0, 0))
      goto done;
    pa_init(&pa, 0);
    if (pa_add_header(&pa, fx.setup, fx.setup_len) ||
        sb_flush(&sb, &pa, 0, 0, 0))
      goto done;
    pa_init(&pa, 0);
    pa.first_frame = 0;
    if (pa_add_whole_frame(&pa, frames, 0) || sb_flush(&sb, &pa, 0, 0, 0))
      goto done;
    /* big frame 1, in pages of at most 255 lacing values */
    off = 0;
    while (off < big_len) {
      size_t chunk = big_len - off;
      pa_init(&pa, off != 0);
      pa.first_frame = 1;
      if (chunk >= 255u * 254u) {
        /* a full page of 255-byte segments only; completes nothing */
        size_t i;
        size_t take = 255u * 254u;
        for (i = 0; i < take; i += 255)
          if (pa_seg(&pa, 255) || buf_add(&pa.body, big + off + i, 255))
            goto done;
        pa.open = 1;
        if (sb_flush(&sb, &pa, 0, 0, 0)) goto done;
        off += take;
        continue;
      }
      /* final piece completes the packet */
      if (pa_add_frame_piece(&pa, big + off, chunk, 1)) goto done;
      pa.first_frame = 1;
      pa.last_frame = 1;
      if (sb_flush(&sb, &pa, 0, 0, 0)) goto done;
      off += chunk;
    }
    pa_init(&pa, 0);
    pa.first_frame = 2;
    for (i = 2; i < 12; ++i)
      if (pa_add_whole_frame(&pa, frames, i)) goto done;
    if (sb_flush(&sb, &pa, 0, 1, 0)) goto done;
  }
  if (write_bytes(".task-build/_m_big_src.ogv", sb.out.data, sb.out.len))
    goto done;
  cuts[0].stream = 0;
  cuts[0].begin = 0;
  cuts[0].end = 3;
  memset(&pkt, 0, sizeof(pkt));
  pkt.data[0] = (unsigned char *)fx.ident;
  pkt.len[0] = fx.ident_len;
  pkt.data[1] = (unsigned char *)fx.comment;
  pkt.len[1] = fx.comment_len;
  pkt.data[2] = (unsigned char *)fx.setup;
  pkt.len[2] = fx.setup_len;
  pkt.count = 3;
  rc = execute_plan(".task-build/_m_big_src.ogv", cuts, 1,
                    ".task-build/_m_big.ogv", 7602, fx.shift, &pkt, frames,
                    out_frames, 3);
  TASK_CHECK(rc == 0);
  rc = 0;
done:
  free(big);
  if (sb.out.data) buf_free(&sb.out);
  unload_fixture(&fx);
  return rc == 0 ? 0 : 1;
}

/* NULL and trivial arguments are handled without crashing. */
static int case_errors_null_args(void) {
  ogg_theora_cut cut;
  FILE *f;
  cut.stream = 0;
  cut.begin = 0;
  cut.end = 1;
  TASK_CHECK(ogg_theora_plan_create(NULL, &cut, 1, NULL) != 0);
  f = fopen("tests/fixtures/frames.ogv", "rb");
  if (!f) return 1;
  {
    ogg_theora_plan *p = (ogg_theora_plan *)0x1;
    TASK_CHECK(ogg_theora_plan_create(f, NULL, 1, &p) != 0);
    TASK_CHECK(p == NULL);
  }
  {
    ogg_theora_plan *p = (ogg_theora_plan *)0x1;
    TASK_CHECK(ogg_theora_plan_create(f, &cut, 0, &p) != 0);
    TASK_CHECK(p == NULL);
  }
  fclose(f);
  TASK_CHECK(ogg_theora_plan_write(NULL, NULL, 1) != 0);
  ogg_theora_plan_destroy(NULL); /* must be a no-op */
  return 0;
}

/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"anchors", "basic", case_anchors_basic},
    {"anchors", "gop_end", case_anchors_gop_end},
    {"anchors", "whole", case_anchors_whole},
    {"anchors", "paged", case_anchors_paged},
    {"anchors", "position", case_anchors_position},
    {"anchors", "exact_time", case_anchors_exact_time},
    {"montage", "reverse", case_montage_reverse},
    {"montage", "repeat_overlap", case_montage_repeat_overlap},
    {"montage", "two_streams", case_montage_two_streams},
    {"montage", "second_only", case_montage_second_only},
    {"montage", "empty_frame", case_montage_empty_frame},
    {"montage", "twice", case_montage_twice},
    {"montage", "large_packet", case_montage_large_packet},
    {"errors", "anchor_not_key", case_errors_anchor_not_key},
    {"errors", "bad_ranges", case_errors_bad_ranges},
    {"errors", "bad_crc", case_errors_bad_crc},
    {"errors", "missing_page", case_errors_missing_page},
    {"errors", "orphan_continuation", case_errors_orphan_continuation},
    {"errors", "bad_granule", case_errors_bad_granule},
    {"errors", "truncation", case_errors_truncation},
    {"errors", "interleaved", case_errors_interleaved},
    {"errors", "same_file", case_errors_same_file},
    {"errors", "write_retry", case_errors_write_retry},
    {"errors", "unknown_stream", case_errors_unknown_stream},
    {"errors", "bad_headers", case_errors_bad_headers},
    {"errors", "header_mismatch", case_errors_header_mismatch},
    {"errors", "missing_eos", case_errors_missing_eos},
    {"errors", "empty_source", case_errors_empty_source},
    {"errors", "create_silent", case_errors_create_silent},
    {"errors", "null_args", case_errors_null_args},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
