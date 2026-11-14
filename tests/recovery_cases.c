/* --------------------------------------------------------------------
 * tests/recovery_cases.c
 *
 * Receive-side recovery regression tests for libogg: after the sync
 * layer drops a corrupt page, the packet (unpack) layer must not glue
 * leftover fragments onto the audio that follows.
 *
 * The library already ships a self-test (src/framing.c, built as
 * test_framing) that round-trips encoder-produced pages and covers
 * several lost-page branches.  This suite does not replace it; it adds
 * an independent receive-side model and combination chains the old
 * test does not exercise end to end:
 *
 *  - Input pages are assembled by hand below: little-endian header
 *    fields, hand-written lacing values (255 runs, zero terminators,
 *    zero-length packets) and a CRC-32 computed bitwise from
 *    polynomial 0x04c11db7.  ogg_stream_packetin()/ogg_stream_flush()
 *    and ogg_page_checksum_set() are NOT used to build the reference
 *    input, and no second decode pass of this library acts as the
 *    oracle: expected page bytes and expected packet/hole event
 *    sequences are stored as standalone models next to the tests.
 *  - All input bytes flow through ogg_sync_buffer()/ogg_sync_wrote()/
 *    ogg_sync_pageout(); recovered pages are dispatched by serial
 *    number to real ogg_stream_pagein()/ogg_stream_packetpeek()/
 *    ogg_stream_packetout() calls.
 *  - Packet bodies contain fake "OggS" capture patterns: inert inside
 *    well-formed pages, but fatal to any implementation that rescans
 *    naively instead of trusting header length + CRC.
 *  - Borrowed page/packet data (pointers into library-owned buffers)
 *    is compared immediately, before any later call could move those
 *    buffers.
 *  - No network, no audio devices, no external material.
 * -------------------------------------------------------------------- */

#include <stdlib.h>
#include <string.h>

#include <ogg/ogg.h>

#include "task_support/check.h"

/* ========================= independent model ========================= */

#define MAX_BODY_LEN 65025L
#define MAX_RAW_LEN  (27 + 255 + MAX_BODY_LEN)

/* A fully serialized page plus its section lengths. */
typedef struct {
  unsigned char raw[MAX_RAW_LEN];
  long header_len;
  long body_len;
  long len; /* header_len + body_len */
} built_page;

/* Incremental description of a page before serialization. */
typedef struct {
  unsigned char flags; /* 0x01 continued, 0x02 bos, 0x04 eos */
  ogg_int64_t granule;
  long serial;
  long pageno;
  int nsegs;
  unsigned char lacing[255];
  long body_len;
  unsigned char body[MAX_BODY_LEN];
} page_spec;

/* Expected stream-layer event.  bytes < 0 means a hole (lost data)
 * event; otherwise a complete packet with these properties. */
typedef struct {
  long bytes;
  ogg_int64_t packetno;
  ogg_int64_t granule;
  int bos;
  int eos;
  unsigned int seed; /* content pattern seed */
} ev_expect;

/* CRC-32 from polynomial 0x04c11db7, computed bitwise, initial
 * register 0 and no final xor (the Ogg page CRC).  This is the
 * reference model; the library's own table-driven implementation is
 * never consulted to produce it. */
static ogg_uint32_t crc_model_update(ogg_uint32_t crc, const unsigned char *data, long n) {
  long i;
  int b;
  for (i = 0; i < n; i++) {
    crc ^= (ogg_uint32_t)data[i] << 24;
    for (b = 0; b < 8; b++)
      crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : (crc << 1);
  }
  return crc;
}

static void put_le32(unsigned char *d, unsigned long v) {
  d[0] = (unsigned char)(v & 0xffu);
  d[1] = (unsigned char)((v >> 8) & 0xffu);
  d[2] = (unsigned char)((v >> 16) & 0xffu);
  d[3] = (unsigned char)((v >> 24) & 0xffu);
}

static void put_le64(unsigned char *d, unsigned long long v) {
  int i;
  for (i = 0; i < 8; i++) {
    d[i] = (unsigned char)(v & 0xffu);
    v >>= 8;
  }
}

/* Deterministic packet body pattern.  Fake capture patterns are
 * embedded on purpose: inside a well-formed page they must be inert. */
static void model_fill(unsigned int seed, unsigned char *dst, long n) {
  long i;
  for (i = 0; i < n; i++) {
    unsigned int x = seed ^ ((unsigned int)i * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    dst[i] = (unsigned char)(x & 0xffu);
  }
  if (n >= 8) memcpy(dst + 1, "OggS", 4);
  if (n >= 600) memcpy(dst + 500, "OggS", 4);
  if (n >= 66100) memcpy(dst + 66000, "OggS", 4);
}

static void spec_init(page_spec *ps, long serial, long pageno, int flags,
                      ogg_int64_t granule) {
  memset(ps, 0, sizeof(*ps));
  ps->flags = (unsigned char)flags;
  ps->granule = granule;
  ps->serial = serial;
  ps->pageno = pageno;
}

/* Append one packet fragment to the page model.  finishes != 0 means
 * the packet terminates on this page (a zero-length packet is a single
 * 0 lacing value; a packet whose length is a multiple of 255 ends with
 * a 0 terminator).  A non-finishing fragment must end on a 255
 * boundary, exactly like a real page that continues on the next one. */
static int spec_fragment(page_spec *ps, const unsigned char *data, long len,
                         int finishes) {
  long n = len;
  while (n >= 255) {
    TASK_CHECK(ps->nsegs < 255);
    ps->lacing[ps->nsegs++] = 255;
    n -= 255;
  }
  if (finishes) {
    TASK_CHECK(ps->nsegs < 255);
    ps->lacing[ps->nsegs++] = (unsigned char)n;
  } else {
    TASK_CHECK(n == 0);
  }
  TASK_CHECK(ps->body_len + len <= MAX_BODY_LEN);
  if (len > 0) memcpy(ps->body + ps->body_len, data, (size_t)len);
  ps->body_len += len;
  return 0;
}

/* Serialize the page model: hand-written little-endian fields, lacing
 * copied verbatim, CRC computed over header+body with the CRC field
 * zeroed and then stored little-endian. */
static int build_page(const page_spec *ps, built_page *bp) {
  unsigned char *h = bp->raw;
  ogg_uint32_t crc;
  TASK_CHECK(ps->nsegs > 0 && ps->nsegs <= 255);
  memcpy(h, "OggS", 4);
  h[4] = 0; /* stream structure version */
  h[5] = ps->flags;
  put_le64(h + 6, (unsigned long long)ps->granule);
  put_le32(h + 14, (unsigned long)ps->serial);
  put_le32(h + 18, (unsigned long)ps->pageno);
  h[22] = h[23] = h[24] = h[25] = 0;
  h[26] = (unsigned char)ps->nsegs;
  memcpy(h + 27, ps->lacing, (size_t)ps->nsegs);
  bp->header_len = 27 + ps->nsegs;
  memcpy(bp->raw + bp->header_len, ps->body, (size_t)ps->body_len);
  bp->body_len = ps->body_len;
  bp->len = bp->header_len + bp->body_len;
  crc = crc_model_update(0, bp->raw, bp->header_len);
  crc = crc_model_update(crc, bp->raw + bp->header_len, bp->body_len);
  put_le32(h + 22, (unsigned long)crc);
  return 0;
}

/* A complete "OggS"-looking page whose stored CRC is deliberately
 * wrong (zero): capture pattern, version 0, zeroed granule/serial/
 * pageno, one 10-byte segment, 0xA5 body.  Contains no 'O' past the
 * capture pattern, so resync after rejecting it lands exactly on the
 * next real page. */
static void build_bogus_page(built_page *bp) {
  memset(bp, 0, sizeof(*bp));
  memcpy(bp->raw, "OggS", 4);
  bp->raw[26] = 1;  /* one segment */
  bp->raw[27] = 10; /* lacing value 10 */
  memset(bp->raw + 28, 0xA5, 10);
  bp->header_len = 28;
  bp->body_len = 10;
  bp->len = 38;
}

/* Scan for a fake capture pattern inside a buffer; used to prove that
 * corrupt pages cannot anchor the resync on their own payload. */
static int has_capture_after(const built_page *bp, long start) {
  long i;
  for (i = start; i + 4 <= bp->len; i++)
    if (memcmp(bp->raw + i, "OggS", 4) == 0) return 1;
  return 0;
}

/* ========================= library drivers ========================= */

/* Push bytes through the real sync-layer entry points. */
static int sync_write(ogg_sync_state *oy, const unsigned char *data, long n) {
  char *buf;
  if (n <= 0) return 0;
  buf = ogg_sync_buffer(oy, n);
  TASK_CHECK(buf != NULL);
  memcpy(buf, data, (size_t)n);
  TASK_CHECK(ogg_sync_wrote(oy, n) == 0);
  return 0;
}

/* A recovered page must equal the original good page byte for byte:
 * header (including the stored CRC bytes) and body.  Called while the
 * ogg_page still points into sync-owned storage, before any further
 * sync call can move it. */
static int check_page_bytes(const ogg_page *og, const built_page *bp) {
  TASK_CHECK(og->header_len == bp->header_len);
  TASK_CHECK(og->body_len == bp->body_len);
  TASK_CHECK(memcmp(og->header, bp->raw, (size_t)bp->header_len) == 0);
  TASK_CHECK(memcmp(og->body, bp->raw + bp->header_len, (size_t)bp->body_len) == 0);
  return 0;
}

/* Compare a decoded packet against the independent model.  The packet
 * content is regenerated from the model seed and memcmp'd immediately,
 * while op->packet still points into stream-owned storage. */
static int check_packet(const ogg_packet *op, const ev_expect *e) {
  unsigned char *want;
  TASK_CHECK(e->bytes >= 0);
  TASK_CHECK(op->bytes == e->bytes);
  TASK_CHECK(op->packetno == e->packetno);
  TASK_CHECK(op->granulepos == e->granule);
  TASK_CHECK((op->b_o_s != 0) == (e->bos != 0));
  TASK_CHECK((op->e_o_s != 0) == (e->eos != 0));
  if (e->bytes > 0) {
    want = (unsigned char *)malloc((size_t)e->bytes);
    TASK_CHECK(want != NULL);
    model_fill(e->seed, want, e->bytes);
    TASK_CHECK(memcmp(op->packet, want, (size_t)e->bytes) == 0);
    free(want);
  }
  return 0;
}

/* Bounded drain of stream events against the model cursor.  At most
 * max_events are consumed; a single ogg_stream_packetpeek() call
 * decides what the next event is (no open-ended waiting on a possibly
 * broken peek).  A hole is consumed by the peek itself; a packet is
 * verified through both peek (non-consuming) and packetout. */
static int drain_events(ogg_stream_state *os, const ev_expect *exp, int nexp,
                        int *idx, int max_events) {
  int guard;
  for (guard = 0; guard < max_events; guard++) {
    ogg_packet peek_op, out_op;
    int r = ogg_stream_packetpeek(os, &peek_op);
    if (r == 0) return 0; /* nothing buffered right now */
    TASK_CHECK(*idx < nexp);
    if (exp[*idx].bytes < 0) {
      TASK_CHECK(r == -1); /* the peek consumed the hole event */
      (*idx)++;
      continue;
    }
    TASK_CHECK(r == 1);
    TASK_CHECK(check_packet(&peek_op, &exp[*idx]) == 0);
    TASK_CHECK(ogg_stream_packetout(os, &out_op) == 1);
    TASK_CHECK(check_packet(&out_op, &exp[*idx]) == 0);
    (*idx)++;
  }
  return 0;
}

/* Feed a whole buffer through sync and pagein every recovered page
 * into one stream; count desync notifications and pages. */
static int pump_pages(ogg_sync_state *oy, ogg_stream_state *os,
                      const unsigned char *input, long total,
                      int *desyncs, int *pages) {
  TASK_CHECK(sync_write(oy, input, total) == 0);
  for (;;) {
    ogg_page og;
    int r = ogg_sync_pageout(oy, &og);
    if (r == 0) break;
    if (r < 0) {
      (*desyncs)++;
      continue;
    }
    TASK_CHECK(ogg_stream_pagein(os, &og) == 0);
    (*pages)++;
  }
  return 0;
}

/* Chunked-feed rig for the sync group: feeds [from,to) of the input in
 * chunk-sized pieces (chunk == 0 means one piece), draining pageout
 * after every piece.  Delivered pages are byte-compared against the
 * expected sequence; when check_early is set, a page may only be
 * delivered once the feed position covers its final byte. */
typedef struct {
  const built_page **pages;
  int npages;
  int page_idx;
  const long *ends; /* absolute input offsets where each page completes */
  int *types;       /* event log: 1 = page, -1 = desync */
  int ntypes;
  int max_types;
  int check_early;
} feed_ctx;

static int feed_range(ogg_sync_state *oy, const unsigned char *input,
                      long from, long to, long chunk, feed_ctx *ctx) {
  long pos = from;
  while (pos < to) {
    long n = chunk > 0 ? chunk : to - from;
    ogg_page og;
    if (n > to - pos) n = to - pos;
    TASK_CHECK(sync_write(oy, input + pos, n) == 0);
    pos += n;
    for (;;) {
      int r = ogg_sync_pageout(oy, &og);
      if (r == 0) break;
      TASK_CHECK(ctx->ntypes < ctx->max_types);
      if (r < 0) {
        ctx->types[ctx->ntypes++] = -1;
        continue;
      }
      TASK_CHECK(ctx->page_idx < ctx->npages);
      TASK_CHECK(check_page_bytes(&og, ctx->pages[ctx->page_idx]) == 0);
      if (ctx->check_early) TASK_CHECK(pos >= ctx->ends[ctx->page_idx]);
      ctx->page_idx++;
      ctx->types[ctx->ntypes++] = 1;
    }
  }
  return 0;
}

static long append_page(unsigned char *dst, long off, const built_page *p) {
  memcpy(dst + off, p->raw, (size_t)p->len);
  return off + p->len;
}

/* ============================== sync group ============================== */

/* Truncated input must wait without delivering anything: a partial
 * capture marker, a complete header with missing body, and a trailing
 * 2-byte capture fragment are all buffered, never delivered early and
 * never reported as desync. */
static int sync_truncated_waits(void) {
  page_spec spec;
  built_page p0, p1;
  unsigned char body[200];
  ogg_sync_state oy;
  ogg_page og;
  long i;

  model_fill(0x10, body, 200);
  spec_init(&spec, 0xC0DE, 0, 0x02, 77); /* BOS */
  TASK_CHECK(spec_fragment(&spec, body, 200, 1) == 0);
  TASK_CHECK(build_page(&spec, &p0) == 0);

  spec_init(&spec, 0xC0DE, 1, 0x00, 88);
  TASK_CHECK(spec_fragment(&spec, body, 150, 1) == 0);
  TASK_CHECK(build_page(&spec, &p1) == 0);

  TASK_CHECK(ogg_sync_init(&oy) == 0);

  /* Lone truncated capture marker: wait, no page, no desync. */
  TASK_CHECK(sync_write(&oy, p0.raw, 3) == 0); /* "Ogg" */
  TASK_CHECK(ogg_sync_pageout(&oy, &og) == 0);
  TASK_CHECK(ogg_sync_pageseek(&oy, &og) == 0);

  /* Rest of the page one byte at a time: nothing may be delivered
   * before the final byte arrives. */
  for (i = 3; i < p0.len; i++) {
    TASK_CHECK(sync_write(&oy, p0.raw + i, 1) == 0);
    if (i < p0.len - 1) TASK_CHECK(ogg_sync_pageout(&oy, &og) == 0);
  }
  TASK_CHECK(ogg_sync_pageout(&oy, &og) == 1);
  TASK_CHECK(check_page_bytes(&og, &p0) == 0);
  TASK_CHECK(ogg_sync_pageout(&oy, &og) == 0);

  /* Complete header but missing body: keep waiting. */
  TASK_CHECK(sync_write(&oy, p1.raw, 26) == 0);
  TASK_CHECK(ogg_sync_pageout(&oy, &og) == 0);
  TASK_CHECK(sync_write(&oy, p1.raw + 26, p1.header_len - 26) == 0);
  TASK_CHECK(ogg_sync_pageout(&oy, &og) == 0); /* full header, no body */
  TASK_CHECK(sync_write(&oy, p1.raw + p1.header_len, 100) == 0);
  TASK_CHECK(ogg_sync_pageout(&oy, &og) == 0); /* half the body */
  TASK_CHECK(sync_write(&oy, p1.raw + p1.header_len + 100, p1.body_len - 100) == 0);
  TASK_CHECK(ogg_sync_pageout(&oy, &og) == 1);
  TASK_CHECK(check_page_bytes(&og, &p1) == 0);

  /* A trailing truncated capture keeps waiting and is not an error. */
  TASK_CHECK(sync_write(&oy, p0.raw, 2) == 0); /* "Og" */
  TASK_CHECK(ogg_sync_pageout(&oy, &og) == 0);
  TASK_CHECK(ogg_sync_pageseek(&oy, &og) == 0);

  ogg_sync_clear(&oy);
  return 0;
}

/* A CRC-bad page plus a bogus "OggS"-looking page between good pages:
 * the sync layer must reject both, report the desync exactly once for
 * the continuous bad run, and recover the following good pages with
 * bytes identical to the originals.  A second garbage run after a
 * successful page may (and must) report desync again. */
static int sync_bad_crc_resync(void) {
  page_spec spec;
  built_page p0, p1bad, fake, p2, p3, p4;
  unsigned char b0[120], bad[60], b2a[300], b2b[510], b4[45];
  unsigned char garbage[100];
  unsigned char *input;
  const built_page *pages[4];
  feed_ctx ctx;
  ogg_sync_state oy;
  int types[8];
  long off, a, total;
  int i;

  model_fill(0x20, b0, 120);
  spec_init(&spec, 0xC0DE, 0, 0x02, 10);
  TASK_CHECK(spec_fragment(&spec, b0, 120, 1) == 0);
  TASK_CHECK(build_page(&spec, &p0) == 0);

  /* Structurally valid page whose body no longer matches its CRC. */
  memset(bad, 0xA5, sizeof(bad));
  spec_init(&spec, 0xC0DE, 1, 0x00, 11);
  TASK_CHECK(spec_fragment(&spec, bad, 60, 1) == 0);
  TASK_CHECK(build_page(&spec, &p1bad) == 0);
  p1bad.raw[p1bad.header_len] ^= 0x01; /* corrupt one body byte */
  TASK_CHECK(!has_capture_after(&p1bad, 1));

  build_bogus_page(&fake);
  TASK_CHECK(crc_model_update(0, fake.raw, fake.len) != 0); /* stored CRC wrong */
  TASK_CHECK(!has_capture_after(&fake, 1));

  model_fill(0x32, b2a, 300);
  model_fill(0x33, b2b, 510);
  spec_init(&spec, 0xC0DE, 2, 0x00, 12);
  TASK_CHECK(spec_fragment(&spec, b2a, 300, 1) == 0); /* 255,45 */
  TASK_CHECK(spec_fragment(&spec, NULL, 0, 1) == 0);  /* zero-length packet */
  TASK_CHECK(spec_fragment(&spec, b2b, 510, 1) == 0); /* 255,255,0 terminator */
  TASK_CHECK(build_page(&spec, &p2) == 0);

  spec_init(&spec, 0xC0DE, 3, 0x00, 13);
  TASK_CHECK(spec_fragment(&spec, NULL, 0, 1) == 0); /* zero-length packet page */
  TASK_CHECK(build_page(&spec, &p3) == 0);

  model_fill(0x25, b4, 45);
  spec_init(&spec, 0xC0DE, 4, 0x00, 14);
  TASK_CHECK(spec_fragment(&spec, b4, 45, 1) == 0);
  TASK_CHECK(build_page(&spec, &p4) == 0);

  memset(garbage, 0x5A, sizeof(garbage)); /* no 'O' bytes */

  /* input = p0 p1bad fake p2 p3 "Og" garbage p4 */
  a = p0.len + p1bad.len + fake.len + p2.len + p3.len;
  total = a + 2 + (long)sizeof(garbage) + p4.len;
  input = (unsigned char *)malloc((size_t)total);
  TASK_CHECK(input != NULL);
  off = 0;
  off = append_page(input, off, &p0);
  off = append_page(input, off, &p1bad);
  off = append_page(input, off, &fake);
  off = append_page(input, off, &p2);
  off = append_page(input, off, &p3);
  input[off] = 'O';
  input[off + 1] = 'g';
  off += 2;
  memcpy(input + off, garbage, sizeof(garbage));
  off += (long)sizeof(garbage);
  off = append_page(input, off, &p4);
  TASK_CHECK(off == total);

  pages[0] = &p0;
  pages[1] = &p2;
  pages[2] = &p3;
  pages[3] = &p4;
  memset(&ctx, 0, sizeof(ctx));
  ctx.pages = pages;
  ctx.npages = 4;
  ctx.types = types;
  ctx.max_types = 8;

  TASK_CHECK(ogg_sync_init(&oy) == 0);

  /* Good page, then two consecutive bad pages: exactly one desync
   * notification for the continuous bad run, then the good pages. */
  TASK_CHECK(feed_range(&oy, input, 0, a, 0, &ctx) == 0);
  TASK_CHECK(ctx.page_idx == 3);
  TASK_CHECK(ctx.ntypes == 4);
  TASK_CHECK(types[0] == 1 && types[1] == -1 && types[2] == 1 && types[3] == 1);

  /* Truncated capture tail: still waiting, no further event. */
  TASK_CHECK(feed_range(&oy, input, a, a + 2, 0, &ctx) == 0);
  TASK_CHECK(ctx.ntypes == 4);

  /* Garbage after a successful page may report desync once more. */
  TASK_CHECK(feed_range(&oy, input, a + 2, total, 0, &ctx) == 0);
  TASK_CHECK(ctx.page_idx == 4);
  TASK_CHECK(ctx.ntypes == 6);
  TASK_CHECK(types[4] == -1 && types[5] == 1);

  ogg_sync_clear(&oy);
  free(input);
  for (i = 0; i < 6; i++) TASK_CHECK(types[i] == (i == 1 || i == 4 ? -1 : 1));
  return 0;
}

/* The same composite input (good page, CRC-bad page, good page with
 * zero-length and zero-terminated packets and fake captures in the
 * bodies, then a header-complete/body-missing tail) fed byte by byte,
 * in short chunks crossing the fixed-header boundary, and as one
 * block: the event stream must be identical, and no page may be
 * delivered before its final byte arrived. */
static int sync_chunked_equivalence(void) {
  page_spec spec;
  built_page p0, p1bad, p2, p3;
  unsigned char b0[100], bad[60], b2a[300], b2b[510], b3[200];
  unsigned char *input;
  const built_page *pages[3];
  long ends[3];
  long off, len012, split, total;
  int t1[8], t2[8], t3[8];
  int n1 = 0, n2 = 0, n3 = 0;
  static const long chunks[3] = {1, 13, 0};
  int ci;

  model_fill(0x30, b0, 100);
  spec_init(&spec, 0xC0DE, 0, 0x02, 10);
  TASK_CHECK(spec_fragment(&spec, b0, 100, 1) == 0);
  TASK_CHECK(build_page(&spec, &p0) == 0);

  memset(bad, 0xA5, sizeof(bad));
  spec_init(&spec, 0xC0DE, 1, 0x00, 11);
  TASK_CHECK(spec_fragment(&spec, bad, 60, 1) == 0);
  TASK_CHECK(build_page(&spec, &p1bad) == 0);
  p1bad.raw[p1bad.header_len] ^= 0x01;
  TASK_CHECK(!has_capture_after(&p1bad, 1));

  model_fill(0x32, b2a, 300);
  model_fill(0x33, b2b, 510);
  spec_init(&spec, 0xC0DE, 2, 0x00, 12);
  TASK_CHECK(spec_fragment(&spec, b2a, 300, 1) == 0);
  TASK_CHECK(spec_fragment(&spec, NULL, 0, 1) == 0);
  TASK_CHECK(spec_fragment(&spec, b2b, 510, 1) == 0);
  TASK_CHECK(build_page(&spec, &p2) == 0);

  model_fill(0x34, b3, 200);
  spec_init(&spec, 0xC0DE, 3, 0x00, 13);
  TASK_CHECK(spec_fragment(&spec, b3, 200, 1) == 0);
  TASK_CHECK(build_page(&spec, &p3) == 0);

  /* input = p0 p1bad p2 p3 "Og"; p3 is fed in two ranges so that the
   * feed stops with a complete header but only half of the body. */
  len012 = p0.len + p1bad.len + p2.len;
  split = p3.header_len + 100;
  total = len012 + p3.len + 2;
  input = (unsigned char *)malloc((size_t)total);
  TASK_CHECK(input != NULL);
  off = 0;
  off = append_page(input, off, &p0);
  off = append_page(input, off, &p1bad);
  off = append_page(input, off, &p2);
  off = append_page(input, off, &p3);
  input[off] = 'O';
  input[off + 1] = 'g';
  TASK_CHECK(off + 2 == total);

  ends[0] = p0.len;
  ends[1] = len012;          /* end of p2 */
  ends[2] = len012 + p3.len; /* end of p3 */
  pages[0] = &p0;
  pages[1] = &p2;
  pages[2] = &p3;

  for (ci = 0; ci < 3; ci++) {
    ogg_sync_state oy;
    ogg_page og;
    feed_ctx ctx;
    int *types = ci == 0 ? t1 : ci == 1 ? t2 : t3;
    int *nt = ci == 0 ? &n1 : ci == 1 ? &n2 : &n3;
    long chunk = chunks[ci];

    memset(&ctx, 0, sizeof(ctx));
    ctx.pages = pages;
    ctx.npages = 3;
    ctx.ends = ends;
    ctx.types = types;
    ctx.max_types = 8;
    ctx.check_early = (chunk == 1);

    TASK_CHECK(ogg_sync_init(&oy) == 0);
    TASK_CHECK(feed_range(&oy, input, 0, len012 + split, chunk, &ctx) == 0);
    /* Header complete, body missing: p3 must wait, undelivered. */
    TASK_CHECK(ctx.page_idx == 2);
    TASK_CHECK(ogg_sync_pageout(&oy, &og) == 0);
    TASK_CHECK(feed_range(&oy, input, len012 + split, len012 + p3.len, chunk, &ctx) == 0);
    TASK_CHECK(ctx.page_idx == 3);
    /* Trailing truncated capture: waits, no event. */
    TASK_CHECK(feed_range(&oy, input, len012 + p3.len, total, chunk, &ctx) == 0);
    TASK_CHECK(ctx.page_idx == 3);
    TASK_CHECK(ogg_sync_pageout(&oy, &og) == 0);
    *nt = ctx.ntypes;
    ogg_sync_clear(&oy);
  }

  /* Identical event streams for all three feed chunkings. */
  TASK_CHECK(n1 == 4 && n2 == 4 && n3 == 4);
  TASK_CHECK(memcmp(t1, t2, sizeof(t1)) == 0);
  TASK_CHECK(memcmp(t1, t3, sizeof(t1)) == 0);
  TASK_CHECK(t1[0] == 1 && t1[1] == -1 && t1[2] == 1 && t1[3] == 1);

  free(input);
  return 0;
}

/* ogg_sync_pageseek() reports the exact number of skipped bytes as a
 * negative return, distinct from ogg_sync_pageout()'s one-time desync
 * notification; a complete page is returned with its exact length. */
static int sync_pageseek_skips(void) {
  page_spec spec;
  built_page p0, p1, fake;
  unsigned char b0[150], b1[90];
  unsigned char garbage[37];
  ogg_sync_state oy;
  ogg_page og;
  long r;
  int i;

  for (i = 0; i < 37; i++) garbage[i] = (unsigned char)(i + 1); /* no 'O' */

  model_fill(0x40, b0, 150);
  spec_init(&spec, 0xC0DE, 0, 0x02, 55);
  TASK_CHECK(spec_fragment(&spec, b0, 150, 1) == 0);
  TASK_CHECK(build_page(&spec, &p0) == 0);

  model_fill(0x41, b1, 90);
  spec_init(&spec, 0xC0DE, 1, 0x00, 56);
  TASK_CHECK(spec_fragment(&spec, b1, 90, 1) == 0);
  TASK_CHECK(build_page(&spec, &p1) == 0);

  build_bogus_page(&fake);
  TASK_CHECK(crc_model_update(0, fake.raw, fake.len) != 0);
  TASK_CHECK(!has_capture_after(&fake, 1));

  TASK_CHECK(ogg_sync_init(&oy) == 0);

  /* Garbage without any capture byte: one exact skip to the page. */
  TASK_CHECK(sync_write(&oy, garbage, (long)sizeof(garbage)) == 0);
  TASK_CHECK(sync_write(&oy, p0.raw, p0.len) == 0);
  r = ogg_sync_pageseek(&oy, &og);
  TASK_CHECK(r == -37);
  r = ogg_sync_pageseek(&oy, &og);
  TASK_CHECK(r == p0.len);
  TASK_CHECK(check_page_bytes(&og, &p0) == 0);
  TASK_CHECK(ogg_sync_pageseek(&oy, &og) == 0);

  /* A complete but CRC-bad page is skipped exactly up to the next
   * real page. */
  TASK_CHECK(sync_write(&oy, fake.raw, fake.len) == 0);
  TASK_CHECK(sync_write(&oy, p1.raw, p1.len) == 0);
  r = ogg_sync_pageseek(&oy, &og);
  TASK_CHECK(r == -fake.len);
  r = ogg_sync_pageseek(&oy, &og);
  TASK_CHECK(r == p1.len);
  TASK_CHECK(check_page_bytes(&og, &p1) == 0);
  TASK_CHECK(ogg_sync_pageseek(&oy, &og) == 0);

  ogg_sync_clear(&oy);
  return 0;
}

/* ============================ recovery group ============================ */

/* Packet manifest for recovery_orphan_suffix (independent model):
 *   pktA 300 bytes   - complete on page 0, queued before the loss
 *   pktB 70000 bytes - page 1 (first 65025 bytes) is LOST; its
 *                      4975-byte tail arrives on page 2 as an orphan
 *                      suffix (19x255 + 130 lacing) and must be dropped
 *   pktC 0 bytes     - zero-length packet on the recovery page
 *   pktD 255 bytes   - completes the recovery page (255 + 0 lacing)
 *   pktE 10 bytes    - EOS packet on page 3
 * Only the last packet completed on a page carries that page's
 * granule; the hole consumes packet sequence number 1. */
static int recovery_orphan_suffix(void) {
  static const ev_expect model[5] = {
    {300, 0, 1000, 1, 0, 0xA0}, /* pktA: packetno 0, BOS */
    {-1, 0, 0, 0, 0, 0},        /* hole for the lost page */
    {0, 2, -1, 0, 0, 0},        /* pktC: zero-length, no granule */
    {255, 3, 3000, 0, 0, 0xD0}, /* pktD: page 2's granule */
    {10, 4, 4000, 0, 1, 0xE0}   /* pktE: EOS */
  };
  page_spec spec;
  built_page p0, p2, p3;
  unsigned char pktA[300], tail[4975], pktD[255], pktE[10];
  unsigned char *input;
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_packet op;
  long off, total;
  int idx = 0, desyncs = 0, pages = 0, r;

  model_fill(0xA0, pktA, 300);
  model_fill(0xB0, tail, 4975);
  model_fill(0xD0, pktD, 255);
  model_fill(0xE0, pktE, 10);

  spec_init(&spec, 0xA5A5, 0, 0x02, 1000);
  TASK_CHECK(spec_fragment(&spec, pktA, 300, 1) == 0);
  TASK_CHECK(build_page(&spec, &p0) == 0);

  spec_init(&spec, 0xA5A5, 2, 0x01, 3000); /* continued */
  TASK_CHECK(spec_fragment(&spec, tail, 4975, 1) == 0); /* orphan suffix */
  TASK_CHECK(spec_fragment(&spec, NULL, 0, 1) == 0);    /* zero-length packet */
  TASK_CHECK(spec_fragment(&spec, pktD, 255, 1) == 0);  /* 255 + 0 terminator */
  TASK_CHECK(build_page(&spec, &p2) == 0);

  spec_init(&spec, 0xA5A5, 3, 0x04, 4000); /* EOS */
  TASK_CHECK(spec_fragment(&spec, pktE, 10, 1) == 0);
  TASK_CHECK(build_page(&spec, &p3) == 0);

  total = p0.len + p2.len + p3.len;
  input = (unsigned char *)malloc((size_t)total);
  TASK_CHECK(input != NULL);
  off = 0;
  off = append_page(input, off, &p0);
  off = append_page(input, off, &p2);
  off = append_page(input, off, &p3);
  TASK_CHECK(off == total);

  TASK_CHECK(ogg_sync_init(&oy) == 0);
  TASK_CHECK(ogg_stream_init(&os, 0xA5A5) == 0);
  TASK_CHECK(pump_pages(&oy, &os, input, total, &desyncs, &pages) == 0);
  TASK_CHECK(desyncs == 0); /* consecutive valid pages: no desync */
  TASK_CHECK(pages == 3);

  /* Bounded look at the next event: a complete packet, not a hole. */
  r = ogg_stream_packetpeek(&os, &op);
  TASK_CHECK(r == 1);
  TASK_CHECK(op.bytes == 300 && op.packetno == 0);
  TASK_CHECK(drain_events(&os, model, 5, &idx, 16) == 0);
  TASK_CHECK(idx == 5);
  TASK_CHECK(ogg_stream_packetpeek(&os, &op) == 0);
  TASK_CHECK(ogg_stream_eos(&os) == 1);

  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  free(input);
  return 0;
}

/* Packet manifest for recovery_pending_prefix_dropped:
 *   pktA 60 bytes    - complete on page 0, queued before the bad page
 *   pktB 65635 bytes - 510-byte prefix queued from page 0; the middle
 *                      page (65025 bytes) is dropped by the sync layer
 *                      (bad CRC); the 100-byte tail on page 2 is an
 *                      orphan suffix.  Both fragments must be dropped.
 *   pktC 30 bytes    - first real packet after the gap
 * The corrupt middle page never reaches the stream layer; the queued
 * prefix must not be glued to the packets that follow. */
static int recovery_pending_prefix_dropped(void) {
  static const ev_expect model[3] = {
    {60, 0, 500, 1, 0, 0x60}, /* pktA */
    {-1, 0, 0, 0, 0, 0},      /* hole for the dropped page */
    {30, 2, 900, 0, 0, 0x63}  /* pktC */
  };
  page_spec spec;
  built_page p0, p1bad, p2;
  const built_page *expect_pages[2];
  unsigned char pktA[60], prefix[510], tail[100], pktC[30];
  unsigned char mid[65025];
  unsigned char *input;
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_page og;
  long off, total;
  int idx = 0, desyncs = 0, page_idx = 0, r;

  model_fill(0x60, pktA, 60);
  model_fill(0x61, prefix, 510);
  model_fill(0x62, tail, 100);
  model_fill(0x63, pktC, 30);
  memset(mid, 0xA5, sizeof(mid));

  spec_init(&spec, 0xB6B6, 0, 0x02, 500);
  TASK_CHECK(spec_fragment(&spec, pktA, 60, 1) == 0);
  TASK_CHECK(spec_fragment(&spec, prefix, 510, 0) == 0); /* continues */
  TASK_CHECK(build_page(&spec, &p0) == 0);

  spec_init(&spec, 0xB6B6, 1, 0x01, -1); /* continued */
  TASK_CHECK(spec_fragment(&spec, mid, 65025, 0) == 0); /* 255x255 */
  TASK_CHECK(build_page(&spec, &p1bad) == 0);
  p1bad.raw[p1bad.header_len] ^= 0x01; /* break the CRC */
  TASK_CHECK(!has_capture_after(&p1bad, 1));

  spec_init(&spec, 0xB6B6, 2, 0x01, 900); /* continued */
  TASK_CHECK(spec_fragment(&spec, tail, 100, 1) == 0); /* orphan suffix */
  TASK_CHECK(spec_fragment(&spec, pktC, 30, 1) == 0);
  TASK_CHECK(build_page(&spec, &p2) == 0);

  total = p0.len + p1bad.len + p2.len;
  input = (unsigned char *)malloc((size_t)total);
  TASK_CHECK(input != NULL);
  off = 0;
  off = append_page(input, off, &p0);
  off = append_page(input, off, &p1bad);
  off = append_page(input, off, &p2);
  TASK_CHECK(off == total);

  expect_pages[0] = &p0;
  expect_pages[1] = &p2;

  TASK_CHECK(ogg_sync_init(&oy) == 0);
  TASK_CHECK(ogg_stream_init(&os, 0xB6B6) == 0);
  TASK_CHECK(sync_write(&oy, input, total) == 0);
  for (;;) {
    r = ogg_sync_pageout(&oy, &og);
    if (r == 0) break;
    if (r < 0) {
      desyncs++;
      continue;
    }
    /* Only p0 and p2 may be recovered, byte-identical to the model. */
    TASK_CHECK(page_idx < 2);
    TASK_CHECK(check_page_bytes(&og, expect_pages[page_idx]) == 0);
    page_idx++;
    TASK_CHECK(ogg_stream_pagein(&os, &og) == 0);
  }
  TASK_CHECK(desyncs == 1);
  TASK_CHECK(page_idx == 2);

  TASK_CHECK(drain_events(&os, model, 3, &idx, 8) == 0);
  TASK_CHECK(idx == 3);
  TASK_CHECK(ogg_stream_packetpeek(&os, NULL) == 0);

  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  free(input);
  return 0;
}

/* packetpeek/packetout progress rules across a hole: a peek on a hole
 * consumes the event (exactly once); repeated peeks on a complete
 * packet do not consume it; NULL-output calls follow the same
 * progress.  All probes are single bounded calls. */
static int recovery_peek_hole_progress(void) {
  static const ev_expect expA = {20, 0, 100, 1, 0, 0x70};
  static const ev_expect expB = {25, 2, 300, 0, 0, 0x71};
  page_spec spec;
  built_page p0, p2;
  unsigned char pktA[20], pktB[25];
  unsigned char *input;
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_packet op;
  long off, total;
  int desyncs, pages, r;

  model_fill(0x70, pktA, 20);
  model_fill(0x71, pktB, 25);

  spec_init(&spec, 0xC1C1, 0, 0x02, 100);
  TASK_CHECK(spec_fragment(&spec, pktA, 20, 1) == 0);
  TASK_CHECK(build_page(&spec, &p0) == 0);

  /* Page 1 is lost; page 2 carries pktB. */
  spec_init(&spec, 0xC1C1, 2, 0x00, 300);
  TASK_CHECK(spec_fragment(&spec, pktB, 25, 1) == 0);
  TASK_CHECK(build_page(&spec, &p2) == 0);

  total = p0.len + p2.len;
  input = (unsigned char *)malloc((size_t)total);
  TASK_CHECK(input != NULL);
  off = 0;
  off = append_page(input, off, &p0);
  off = append_page(input, off, &p2);
  TASK_CHECK(off == total);

  /* Phase 1: peeks with an output packet. */
  desyncs = 0;
  pages = 0;
  TASK_CHECK(ogg_sync_init(&oy) == 0);
  TASK_CHECK(ogg_stream_init(&os, 0xC1C1) == 0);
  TASK_CHECK(pump_pages(&oy, &os, input, total, &desyncs, &pages) == 0);
  TASK_CHECK(desyncs == 0 && pages == 2);

  r = ogg_stream_packetpeek(&os, &op);
  TASK_CHECK(r == 1);
  TASK_CHECK(check_packet(&op, &expA) == 0);
  r = ogg_stream_packetpeek(&os, &op); /* repeated peek: not consumed */
  TASK_CHECK(r == 1);
  TASK_CHECK(check_packet(&op, &expA) == 0);
  TASK_CHECK(ogg_stream_packetpeek(&os, NULL) == 1); /* NULL output: same */
  TASK_CHECK(ogg_stream_packetout(&os, NULL) == 1);  /* consume pktA */
  r = ogg_stream_packetpeek(&os, &op);               /* hole: consumed by the peek */
  TASK_CHECK(r == -1);
  r = ogg_stream_packetpeek(&os, &op); /* bounded: next event is pktB */
  TASK_CHECK(r == 1);
  TASK_CHECK(check_packet(&op, &expB) == 0);
  TASK_CHECK(ogg_stream_packetout(&os, &op) == 1);
  TASK_CHECK(check_packet(&op, &expB) == 0);
  TASK_CHECK(ogg_stream_packetpeek(&os, &op) == 0);
  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);

  /* Phase 2: NULL-output calls must follow the same progress. */
  desyncs = 0;
  pages = 0;
  TASK_CHECK(ogg_sync_init(&oy) == 0);
  TASK_CHECK(ogg_stream_init(&os, 0xC1C1) == 0);
  TASK_CHECK(pump_pages(&oy, &os, input, total, &desyncs, &pages) == 0);
  TASK_CHECK(desyncs == 0 && pages == 2);

  TASK_CHECK(ogg_stream_packetpeek(&os, NULL) == 1);
  TASK_CHECK(ogg_stream_packetout(&os, NULL) == 1);  /* pktA consumed */
  TASK_CHECK(ogg_stream_packetpeek(&os, NULL) == -1); /* hole consumed */
  TASK_CHECK(ogg_stream_packetpeek(&os, NULL) == 1);  /* pktB next */
  TASK_CHECK(ogg_stream_packetout(&os, &op) == 1);
  TASK_CHECK(check_packet(&op, &expB) == 0);
  TASK_CHECK(ogg_stream_packetpeek(&os, NULL) == 0);
  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);

  free(input);
  return 0;
}

/* =========================== streaming group =========================== */

/* Two interleaved serials: stream A carries a 70000-byte packet
 * spanning two pages and then loses the page right after it (bad CRC);
 * stream B is untouched and must keep flowing.  Feed chunk sizes and
 * drain timing vary to force sync buffer compaction and stream
 * body/lacing moves; borrowed pages/packets are checked before any
 * later call can move them. */
static int streaming_interleaved_gap(void) {
  static const ev_expect modelA[5] = {
    {50, 0, 100, 1, 0, 0xA00},    /* a0 */
    {70000, 1, -1, 0, 0, 0xA11},  /* a1: spans pages 1-2, no granule */
    {0, 2, 300, 0, 0, 0},         /* a2: zero-length, page 2 granule */
    {-1, 0, 0, 0, 0, 0},          /* hole for the corrupt page 3 */
    {60, 4, 500, 0, 1, 0xA44}     /* a4: EOS */
  };
  static const ev_expect modelB[3] = {
    {30, 0, 11, 1, 0, 0xB00},  /* b0 */
    {510, 1, 22, 0, 0, 0xB11}, /* b1: 255,255,0 terminator */
    {40, 2, 33, 0, 1, 0xB22}   /* b2: EOS */
  };
  static const long chunks[4] = {1, 13, 4096, 7};
  page_spec spec;
  built_page *pg;
  unsigned char a0[50], a3[80], a4[60], b0[30], b1[510], b2[40];
  unsigned char *a1, *input;
  ogg_sync_state oy;
  ogg_stream_state osa, osb;
  long off, total, pos;
  int idxA = 0, idxB = 0, desyncs = 0, npages = 0, k = 0;

  pg = (built_page *)malloc(8 * sizeof(built_page));
  a1 = (unsigned char *)malloc(70000);
  TASK_CHECK(pg != NULL && a1 != NULL);

  model_fill(0xA00, a0, 50);
  model_fill(0xA11, a1, 70000);
  memset(a3, 0xA5, sizeof(a3));
  model_fill(0xA44, a4, 60);
  model_fill(0xB00, b0, 30);
  model_fill(0xB11, b1, 510);
  model_fill(0xB22, b2, 40);

  spec_init(&spec, 0x0A5A, 0, 0x02, 100);
  TASK_CHECK(spec_fragment(&spec, a0, 50, 1) == 0);
  TASK_CHECK(build_page(&spec, &pg[0]) == 0);

  spec_init(&spec, 0x0B6B, 0, 0x02, 11);
  TASK_CHECK(spec_fragment(&spec, b0, 30, 1) == 0);
  TASK_CHECK(build_page(&spec, &pg[1]) == 0);

  spec_init(&spec, 0x0A5A, 1, 0x00, -1);
  TASK_CHECK(spec_fragment(&spec, a1, 65025, 0) == 0); /* a1 part 1 */
  TASK_CHECK(build_page(&spec, &pg[2]) == 0);

  spec_init(&spec, 0x0B6B, 1, 0x00, 22);
  TASK_CHECK(spec_fragment(&spec, b1, 510, 1) == 0);
  TASK_CHECK(build_page(&spec, &pg[3]) == 0);

  spec_init(&spec, 0x0A5A, 2, 0x01, 300); /* continued */
  TASK_CHECK(spec_fragment(&spec, a1 + 65025, 4975, 1) == 0); /* a1 part 2 */
  TASK_CHECK(spec_fragment(&spec, NULL, 0, 1) == 0);          /* a2 */
  TASK_CHECK(build_page(&spec, &pg[4]) == 0);

  spec_init(&spec, 0x0A5A, 3, 0x00, 400);
  TASK_CHECK(spec_fragment(&spec, a3, 80, 1) == 0);
  TASK_CHECK(build_page(&spec, &pg[5]) == 0);
  pg[5].raw[pg[5].header_len] ^= 0x01; /* corrupt: dropped by sync */
  TASK_CHECK(!has_capture_after(&pg[5], 1));

  spec_init(&spec, 0x0B6B, 2, 0x04, 33);
  TASK_CHECK(spec_fragment(&spec, b2, 40, 1) == 0);
  TASK_CHECK(build_page(&spec, &pg[6]) == 0);

  spec_init(&spec, 0x0A5A, 4, 0x04, 500);
  TASK_CHECK(spec_fragment(&spec, a4, 60, 1) == 0);
  TASK_CHECK(build_page(&spec, &pg[7]) == 0);

  total = 0;
  for (k = 0; k < 8; k++) total += pg[k].len;
  input = (unsigned char *)malloc((size_t)total);
  TASK_CHECK(input != NULL);
  off = 0;
  for (k = 0; k < 8; k++) off = append_page(input, off, &pg[k]);
  TASK_CHECK(off == total);

  TASK_CHECK(ogg_sync_init(&oy) == 0);
  TASK_CHECK(ogg_stream_init(&osa, 0x0A5A) == 0);
  TASK_CHECK(ogg_stream_init(&osb, 0x0B6B) == 0);

  pos = 0;
  k = 0;
  while (pos < total) {
    long n = chunks[k++ & 3];
    ogg_page og;
    if (n > total - pos) n = total - pos;
    TASK_CHECK(sync_write(&oy, input + pos, n) == 0);
    pos += n;
    for (;;) {
      int r = ogg_sync_pageout(&oy, &og);
      if (r == 0) break;
      if (r < 0) {
        desyncs++;
        continue;
      }
      npages++;
      if (ogg_page_serialno(&og) == 0x0A5A) {
        TASK_CHECK(ogg_stream_pagein(&osa, &og) == 0);
        if (npages % 2 == 0)
          TASK_CHECK(drain_events(&osa, modelA, 5, &idxA, 1) == 0);
      } else {
        TASK_CHECK(ogg_page_serialno(&og) == 0x0B6B);
        TASK_CHECK(ogg_stream_pagein(&osb, &og) == 0);
        if (npages % 2 == 0)
          TASK_CHECK(drain_events(&osb, modelB, 3, &idxB, 1) == 0);
      }
    }
  }
  TASK_CHECK(drain_events(&osa, modelA, 5, &idxA, 1000) == 0);
  TASK_CHECK(drain_events(&osb, modelB, 3, &idxB, 1000) == 0);
  TASK_CHECK(idxA == 5 && idxB == 3);
  TASK_CHECK(npages == 7);   /* the corrupt page was not delivered */
  TASK_CHECK(desyncs == 1);  /* and reported exactly once */
  TASK_CHECK(ogg_stream_eos(&osa) == 1);
  TASK_CHECK(ogg_stream_eos(&osb) == 1);

  ogg_stream_clear(&osa);
  ogg_stream_clear(&osb);
  ogg_sync_clear(&oy);
  free(input);
  free(a1);
  free(pg);
  return 0;
}

/* After ogg_sync_reset()/ogg_stream_reset(), replaying input must
 * behave like a fresh start: no stale half packet, no stale desync
 * state, packet numbering restarts at zero. */
static int streaming_reset_replay(void) {
  static const ev_expect phase1[1] = {
    {40, 0, 1, 1, 0, 0xC00} /* c0 */
  };
  static const ev_expect phase2[4] = {
    {40, 0, 1, 1, 0, 0xC00},   /* c0 again, packetno back to 0 */
    {70000, 1, -1, 0, 0, 0xC11}, /* c1: spans pages 1-2 */
    {0, 2, 8, 0, 0, 0},        /* c2: zero-length, page 2 granule */
    {25, 3, 9, 0, 1, 0xC33}    /* c3: EOS */
  };
  page_spec spec;
  built_page *pg;
  unsigned char c0[40], c3[25];
  unsigned char garbage[50];
  unsigned char *c1, *input;
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_packet op;
  long off, total1, total2;
  int idx = 0, desyncs = 0, pages = 0;

  pg = (built_page *)malloc(4 * sizeof(built_page));
  c1 = (unsigned char *)malloc(70000);
  TASK_CHECK(pg != NULL && c1 != NULL);

  model_fill(0xC00, c0, 40);
  model_fill(0xC11, c1, 70000);
  model_fill(0xC33, c3, 25);
  memset(garbage, 0x5A, sizeof(garbage));

  spec_init(&spec, 0x0C0C, 0, 0x02, 1);
  TASK_CHECK(spec_fragment(&spec, c0, 40, 1) == 0);
  TASK_CHECK(build_page(&spec, &pg[0]) == 0);

  spec_init(&spec, 0x0C0C, 1, 0x00, -1);
  TASK_CHECK(spec_fragment(&spec, c1, 65025, 0) == 0); /* c1 part 1 */
  TASK_CHECK(build_page(&spec, &pg[1]) == 0);

  spec_init(&spec, 0x0C0C, 2, 0x01, 8); /* continued */
  TASK_CHECK(spec_fragment(&spec, c1 + 65025, 4975, 1) == 0); /* c1 part 2 */
  TASK_CHECK(spec_fragment(&spec, NULL, 0, 1) == 0);          /* c2 */
  TASK_CHECK(build_page(&spec, &pg[2]) == 0);

  spec_init(&spec, 0x0C0C, 3, 0x04, 9);
  TASK_CHECK(spec_fragment(&spec, c3, 25, 1) == 0);
  TASK_CHECK(build_page(&spec, &pg[3]) == 0);

  /* Phase 1: dirty the state machine - a delivered packet, a pending
   * half packet, a garbage desync, and a truncated page start. */
  total1 = pg[0].len + pg[1].len + (long)sizeof(garbage) + 10;
  input = (unsigned char *)malloc((size_t)total1);
  TASK_CHECK(input != NULL);
  off = 0;
  off = append_page(input, off, &pg[0]);
  off = append_page(input, off, &pg[1]);
  memcpy(input + off, garbage, sizeof(garbage));
  off += (long)sizeof(garbage);
  memcpy(input + off, pg[2].raw, 10); /* truncated page start */
  off += 10;
  TASK_CHECK(off == total1);

  TASK_CHECK(ogg_sync_init(&oy) == 0);
  TASK_CHECK(ogg_stream_init(&os, 0x0C0C) == 0);
  TASK_CHECK(pump_pages(&oy, &os, input, total1, &desyncs, &pages) == 0);
  TASK_CHECK(desyncs == 1);
  TASK_CHECK(pages == 2);
  TASK_CHECK(drain_events(&os, phase1, 1, &idx, 8) == 0);
  TASK_CHECK(idx == 1);
  TASK_CHECK(ogg_stream_packetpeek(&os, &op) == 0); /* c1 unfinished */

  /* Reset both layers; nothing from phase 1 may survive. */
  TASK_CHECK(ogg_sync_reset(&oy) == 0);
  TASK_CHECK(ogg_stream_reset(&os) == 0);

  /* Phase 2: clean replay of the same stream, now complete. */
  total2 = pg[0].len + pg[1].len + pg[2].len + pg[3].len;
  free(input);
  input = (unsigned char *)malloc((size_t)total2);
  TASK_CHECK(input != NULL);
  off = 0;
  off = append_page(input, off, &pg[0]);
  off = append_page(input, off, &pg[1]);
  off = append_page(input, off, &pg[2]);
  off = append_page(input, off, &pg[3]);
  TASK_CHECK(off == total2);

  idx = 0;
  desyncs = 0;
  pages = 0;
  TASK_CHECK(pump_pages(&oy, &os, input, total2, &desyncs, &pages) == 0);
  TASK_CHECK(desyncs == 0); /* no stale desync state */
  TASK_CHECK(pages == 4);
  TASK_CHECK(drain_events(&os, phase2, 4, &idx, 16) == 0);
  TASK_CHECK(idx == 4);
  TASK_CHECK(ogg_stream_eos(&os) == 1);

  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  free(input);
  free(c1);
  free(pg);
  return 0;
}

/* ============================== case table ============================== */

const struct task_case task_cases[] = {
    {"sync", "sync_truncated_waits", sync_truncated_waits},
    {"sync", "sync_bad_crc_resync", sync_bad_crc_resync},
    {"sync", "sync_chunked_equivalence", sync_chunked_equivalence},
    {"sync", "sync_pageseek_skips", sync_pageseek_skips},
    {"recovery", "recovery_orphan_suffix", recovery_orphan_suffix},
    {"recovery", "recovery_pending_prefix_dropped", recovery_pending_prefix_dropped},
    {"recovery", "recovery_peek_hole_progress", recovery_peek_hole_progress},
    {"streaming", "streaming_interleaved_gap", streaming_interleaved_gap},
    {"streaming", "streaming_reset_replay", streaming_reset_replay}};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
