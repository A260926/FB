/*
 * pending-write-contract.c
 *
 * Contract tests for the write-side backpressure API:
 *   oggz_write_set_pending_limit()
 *   oggz_write_get_pending_bytes()
 *
 * Registered through task_support/check.h in four groups:
 * admission, pages, resume, streams.
 *
 * All test data is generated in C. An independent ledger records every
 * admitted packet's length, and a separate libogg decode pipeline
 * (ogg_sync + per-serial ogg_stream) counts packets completed on each
 * output page; its pending total is cross-checked against the liboggz
 * getter after every output step. A real liboggz reader finally verifies
 * packet bytes, per-stream ordering and granule positions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <ogg/ogg.h>
#include <oggz/oggz.h>

#include "task_support/check.h"

#define SERIAL1 101
#define SERIAL2 202

/* TASK_CHECK expands to "return 1", so it is only valid inside the
 * int-returning test cases. Helpers use CK_, which records the failure in
 * a shared counter checked by every test case instead. */
static int contract_failures;
#define CK_(x) do { \
  if (!(x)) { \
    fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    contract_failures++; \
  } \
} while (0)

/* ------------------------------------------------------------------ */
/* Independent admission ledger and expected-pending pipeline          */
/* ------------------------------------------------------------------ */

struct ledger_pkt {
  long serial;
  long bytes;
  ogg_int64_t granule;
  ogg_int64_t reader_granule; /* granulepos the decoder will report */
  int pattern;
  int consumed;
};

struct serial_fifo {
  long serial;
  ogg_stream_state os;
  ogg_int64_t *sizes;
  int *ledger_idx;
  int n, cap;
};

struct harness {
  OGGZ *w;
  struct ledger_pkt *ledger;
  int nledger, capledger;

  ogg_sync_state sync;
  struct serial_fifo *fifos;
  int nfifos;

  ogg_int64_t expected_pending;

  unsigned char *out;
  size_t outlen, outcap;
};

static struct serial_fifo *
harness_fifo (struct harness *h, long serial)
{
  int i;
  for (i = 0; i < h->nfifos; i++)
    if (h->fifos[i].serial == serial) return &h->fifos[i];

  h->fifos = realloc (h->fifos,
		      (size_t) (h->nfifos + 1) * sizeof (*h->fifos));
  h->fifos[h->nfifos].serial = serial;
  h->fifos[h->nfifos].sizes = NULL;
  h->fifos[h->nfifos].ledger_idx = NULL;
  h->fifos[h->nfifos].n = h->fifos[h->nfifos].cap = 0;
  ogg_stream_init (&h->fifos[h->nfifos].os, (int) serial);
  return &h->fifos[h->nfifos++];
}

static void
fifo_push (struct serial_fifo *f, ogg_int64_t size, int ledger_idx)
{
  if (f->n == f->cap) {
    f->cap = f->cap ? f->cap * 2 : 8;
    f->sizes = realloc (f->sizes, (size_t) f->cap * sizeof (ogg_int64_t));
    f->ledger_idx = realloc (f->ledger_idx,
			     (size_t) f->cap * sizeof (int));
    CK_ (f->sizes != NULL);
    CK_ (f->ledger_idx != NULL);
  }
  f->sizes[f->n] = size;
  f->ledger_idx[f->n] = ledger_idx;
  f->n++;
}

/* Feed freshly produced output bytes through the independent libogg
 * pipeline and reconcile the expected pending count from the packets that
 * actually complete on each fully parsed page. */
static void
harness_account_output (struct harness *h, const unsigned char *buf, long n)
{
  char *syncbuf;
  ogg_page og;
  long pos = 0;

  while (pos < n) {
    long chunk = n - pos;
    syncbuf = ogg_sync_buffer (&h->sync, chunk);
    memcpy (syncbuf, buf + pos, (size_t) chunk);
    ogg_sync_wrote (&h->sync, chunk);
    pos += chunk;

    while (ogg_sync_pageout (&h->sync, &og) == 1) {
      struct serial_fifo *f;
      int completed, i;
      long serial = ogg_page_serialno (&og);
      ogg_int64_t page_granule = ogg_page_granulepos (&og);

      f = harness_fifo (h, serial);
      ogg_stream_pagein (&f->os, &og);

      completed = ogg_page_packets (&og);
      CK_ (completed >= 0);
      CK_ (completed <= f->n);
      if (completed > f->n) completed = f->n;

      /* Only the last packet completed on a page carries its granulepos;
       * libogg reports -1 for the others. Record exactly what a real
       * reader will observe. */
      for (i = 0; i < completed; i++) {
	int idx = f->ledger_idx[i];
	h->ledger[idx].reader_granule =
	  (i == completed - 1) ? page_granule : -1;
	h->expected_pending -= f->sizes[i];
      }
      f->n -= completed;
      if (f->n > 0) {
	memmove (f->sizes, f->sizes + completed,
		 (size_t) f->n * sizeof (ogg_int64_t));
	memmove (f->ledger_idx, f->ledger_idx + completed,
		 (size_t) f->n * sizeof (int));
      }
      CK_ (h->expected_pending >= 0);
    }
  }
}

static void
harness_append_output (struct harness *h, const unsigned char *buf, long n)
{
  while (h->outlen + (size_t) n > h->outcap)
    h->outcap = h->outcap ? h->outcap * 2 : 65536;
  h->out = realloc (h->out, h->outcap);
  CK_ (h->out != NULL);
  memcpy (h->out + h->outlen, buf, (size_t) n);
  h->outlen += (size_t) n;
}

static void
harness_init (struct harness *h, ogg_int64_t limit)
{
  memset (h, 0, sizeof *h);
  h->w = oggz_new (OGGZ_WRITE);
  CK_ (h->w != NULL);
  CK_ (oggz_write_set_pending_limit (h->w, limit) == 0);
  CK_ (oggz_write_get_pending_bytes (h->w) == 0);
  ogg_sync_init (&h->sync);
}

static void
harness_free (struct harness *h)
{
  int i;
  for (i = 0; i < h->nfifos; i++) {
    ogg_stream_clear (&h->fifos[i].os);
    free (h->fifos[i].sizes);
    free (h->fifos[i].ledger_idx);
  }
  free (h->fifos);
  ogg_sync_clear (&h->sync);
  free (h->ledger);
  free (h->out);
  CK_ (oggz_close (h->w) == 0);
}

static void
fill_pattern (unsigned char *buf, long n, int pattern)
{
  long i;
  for (i = 0; i < n; i++) buf[i] = (unsigned char) (i + pattern);
}

/* Feed one packet and, when it is accepted, record it in the independent
 * ledger and per-stream FIFO. Returns the oggz_write_feed() result. */
static int
harness_feed (struct harness *h, long serial, long bytes, int pattern,
	      int flush, int eos, int *guard, int expect)
{
  unsigned char *buf = malloc ((size_t) bytes + 1);
  ogg_packet op;
  int ret, bos, i;
  struct serial_fifo *f;

  fill_pattern (buf, bytes, pattern);
  memset (&op, 0, sizeof op);
  op.packet = buf;
  op.bytes = bytes;
  op.granulepos = (ogg_int64_t) h->nledger * 100 + (bytes == 0 ? 0 : 1);
  op.packetno = -1; /* let oggz number per stream */
  op.e_o_s = eos;

  /* BOS iff no packet of this serial has ever been admitted */
  bos = 1;
  for (i = 0; i < h->nledger; i++)
    if (h->ledger[i].serial == serial) { bos = 0; break; }
  op.b_o_s = bos;

  ret = oggz_write_feed (h->w, &op, serial, flush, guard);
  CK_ (ret == expect);

  if (ret == 0) {
    int idx = h->nledger;
    f = harness_fifo (h, serial);
    if (h->nledger == h->capledger) {
      h->capledger = h->capledger ? h->capledger * 2 : 16;
      h->ledger = realloc (h->ledger,
			   (size_t) h->capledger * sizeof (*h->ledger));
      CK_ (h->ledger != NULL);
    }
    h->ledger[idx].serial = serial;
    h->ledger[idx].bytes = bytes;
    h->ledger[idx].granule = op.granulepos;
    h->ledger[idx].reader_granule = -2; /* assigned when its page completes */
    h->ledger[idx].pattern = pattern;
    h->ledger[idx].consumed = 0;
    h->nledger++;

    fifo_push (f, (ogg_int64_t) bytes, idx);
    h->expected_pending += bytes;
  }

  free (buf);
  return ret;
}

/* Pull output via oggz_write_output(), verifying the pending getter against
 * the independent pipeline after every chunk. Returns total bytes. */
static long
harness_drain_output (struct harness *h, long chunk)
{
  unsigned char *buf = malloc ((size_t) chunk);
  long n, total = 0;

  CK_ (buf != NULL);
  for (;;) {
    n = oggz_write_output (h->w, buf, chunk);
    if (n <= 0) break;
    harness_append_output (h, buf, n);
    harness_account_output (h, buf, n);
    CK_ (oggz_write_get_pending_bytes (h->w) == h->expected_pending);
    total += n;
  }
  free (buf);
  return total;
}

/* Controllable sink for oggz_write(): full, short, pause and failure. */
struct sink {
  unsigned char *buf;
  size_t cap, len;
  int mode;                 /* 0 full 1 short 2 zero 3 eagain 4 error */
  int short_n;
  int calls;
  int hard_seen;
};

static size_t
sink_cb (void *user, void *b, size_t n)
{
  struct sink *s = user;
  s->calls++;
  switch (s->mode) {
  case 0: {
    size_t w = n;
    if (s->len + w > s->cap) w = s->cap - s->len;
    memcpy (s->buf + s->len, b, w);
    s->len += w;
    return w;
  }
  case 1: {
    size_t w = (size_t) s->short_n;
    if (w > n) w = n;
    memcpy (s->buf + s->len, b, w);
    s->len += w;
    return w;
  }
  case 2: return 0;
  case 3: errno = EAGAIN; return (size_t) -1;
  default: errno = EIO; s->hard_seen++; return (size_t) -1;
  }
}

/* Drive oggz_write() against a sink and reconcile after each call. */
static long
harness_drain_write (struct harness *h, struct sink *s, long chunk,
		     long *last_ret)
{
  long n, total = 0;
  size_t before;

  for (;;) {
    before = s->len;
    n = oggz_write (h->w, chunk);
    if (n < 0) { if (last_ret) *last_ret = n; break; }
    if (n == 0) { if (last_ret) *last_ret = 0; break; }
    CK_ ((size_t) n == s->len - before);
    harness_append_output (h, s->buf + before, n);
    harness_account_output (h, s->buf + before, n);
    CK_ (oggz_write_get_pending_bytes (h->w) == h->expected_pending);
    total += n;
  }
  return total;
}

/* Final roundtrip: a real reader checks every packet against the ledger,
 * enforcing per-stream order, byte contents and granule positions. */
static int
read_packet_cb (OGGZ *z, oggz_packet *zp, long serial, void *user)
{
  struct harness *h = user;
  ogg_packet *op = &zp->op;
  int i, slot = -1, k;

  (void) z;
  for (i = 0; i < h->nledger; i++) {
    if (!h->ledger[i].consumed && h->ledger[i].serial == serial) {
      slot = i;
      break;
    }
  }
  if (slot < 0) { contract_failures++; return 1; }

  CK_ (h->ledger[slot].bytes == op->bytes);
  CK_ (h->ledger[slot].reader_granule == op->granulepos);
  for (k = 0; k < op->bytes; k++)
    CK_ (op->packet[k] ==
	 (unsigned char) (k + h->ledger[slot].pattern));

  h->ledger[slot].consumed = 1;
  return 0;
}

static void
harness_verify_roundtrip (struct harness *h)
{
  OGGZ *r = oggz_new (OGGZ_READ);
  long pos = 0, n;
  int i;

  CK_ (r != NULL);
  CK_ (oggz_set_read_callback (r, -1, read_packet_cb, h) == 0);

  while (pos < (long) h->outlen) {
    long chunk = (long) h->outlen - pos;
    if (chunk > 4096) chunk = 4096;
    n = oggz_read_input (r, h->out + pos, chunk);
    CK_ (n == chunk);
    pos += n;
  }
  for (i = 0; i < h->nledger; i++) CK_ (h->ledger[i].consumed);
  CK_ (oggz_close (r) == 0);
}

/* ------------------------------------------------------------------ */
/* admission group                                                     */
/* ------------------------------------------------------------------ */

static int
test_defaults_and_validation (void)
{
  OGGZ *w = oggz_new (OGGZ_WRITE);
  OGGZ *r = oggz_new (OGGZ_READ);

  TASK_CHECK (w != NULL && r != NULL);
  TASK_CHECK (oggz_write_get_pending_bytes (w) == 0);

  TASK_CHECK (oggz_write_set_pending_limit (NULL, 5) == OGGZ_ERR_BAD_OGGZ);
  TASK_CHECK (oggz_write_get_pending_bytes (NULL) == OGGZ_ERR_BAD_OGGZ);
  TASK_CHECK (oggz_write_set_pending_limit (r, 5) == OGGZ_ERR_INVALID);
  TASK_CHECK (oggz_write_get_pending_bytes (r) == OGGZ_ERR_INVALID);

  TASK_CHECK (oggz_write_set_pending_limit (w, -1) == OGGZ_ERR_INVALID);
  TASK_CHECK (oggz_write_set_pending_limit (w, -1000000)
	      == OGGZ_ERR_INVALID);
  TASK_CHECK (oggz_write_set_pending_limit (w, 20) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (w) == 0);

  /* limit below current pending is refused and leaves the old limit */
  {
    unsigned char data[20];
    ogg_packet op;
    memset (data, 1, sizeof data);
    memset (&op, 0, sizeof op);
    op.packet = data; op.bytes = 20; op.b_o_s = 1;
    op.granulepos = 0; op.packetno = 0;
    TASK_CHECK (oggz_write_feed (w, &op, SERIAL1, OGGZ_FLUSH_AFTER, NULL)
		== 0);
    TASK_CHECK (oggz_write_get_pending_bytes (w) == 20);
    TASK_CHECK (oggz_write_set_pending_limit (w, 19)
		== OGGZ_ERR_INVALID);
    TASK_CHECK (oggz_write_set_pending_limit (w, 20) == 0); /* equal OK */
    /* the rejected 19-limit must not have replaced the working 20-limit:
     * a one-byte packet still cannot exceed it, but zero can */
    memset (&op, 0, sizeof op);
    op.packet = data; op.bytes = 1; op.granulepos = 1; op.packetno = 1;
    TASK_CHECK (oggz_write_feed (w, &op, SERIAL1, 0, NULL)
		== OGGZ_ERR_IO_AGAIN);
    TASK_CHECK (oggz_write_get_pending_bytes (w) == 20);
    TASK_CHECK (oggz_write_set_pending_limit (w, 0) == 0); /* unlimited */
  }

  TASK_CHECK (oggz_close (w) == 0);
  TASK_CHECK (oggz_close (r) == 0);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

static int
test_reject_preserves_state_and_guard (void)
{
  struct harness h;
  unsigned char big[64];
  ogg_packet op;
  int guard = 0;

  harness_init (&h, 16);
  fill_pattern (big, sizeof big, 7);

  /* Oversized guarded packet: rejected, guard and state untouched. */
  memset (&op, 0, sizeof op);
  op.packet = big; op.bytes = 64; op.b_o_s = 1;
  op.granulepos = 5; op.packetno = 0;
  TASK_CHECK (oggz_write_feed (h.w, &op, SERIAL1, 0, &guard)
	      == OGGZ_ERR_IO_AGAIN);
  TASK_CHECK (guard == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);

  /* Same packet retried verbatim after raising the limit is accepted:
   * proves BOS/packetno were not consumed by the rejected attempt. */
  TASK_CHECK (oggz_write_set_pending_limit (h.w, 64) == 0);
  TASK_CHECK (oggz_write_feed (h.w, &op, SERIAL1, OGGZ_FLUSH_AFTER, &guard)
	      == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 64);
  TASK_CHECK (guard == 0); /* owned by oggz until the packet is freed */

  /* A rejected EOS must not mark the stream at EOS: retrying it with a
   * raised limit is accepted as the stream's first packet. */
  {
    struct harness h2;
    unsigned char tail[40];
    ogg_packet p2;
    harness_init (&h2, 8);
    fill_pattern (tail, sizeof tail, 3);
    memset (&p2, 0, sizeof p2);
    p2.packet = tail; p2.bytes = 40; p2.b_o_s = 1; p2.e_o_s = 1;
    p2.granulepos = 0; p2.packetno = 0;
    TASK_CHECK (oggz_write_feed (h2.w, &p2, SERIAL2, 0, NULL)
		== OGGZ_ERR_IO_AGAIN);
    TASK_CHECK (oggz_write_set_pending_limit (h2.w, 40) == 0);
    TASK_CHECK (oggz_write_feed (h2.w, &p2, SERIAL2,
				 OGGZ_FLUSH_AFTER, NULL) == 0);
    harness_free (&h2);
  }

  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

static int
test_boundary_overflow_and_nil (void)
{
  struct harness h;
  unsigned char data[16];
  ogg_packet op;

  harness_init (&h, 10);
  fill_pattern (data, sizeof data, 1);

  /* exactly at the limit is admitted, one byte over is rejected */
  TASK_CHECK (harness_feed (&h, SERIAL1, 10, 1, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 10);

  memset (&op, 0, sizeof op);
  op.packet = data; op.bytes = 1; op.granulepos = 1; op.packetno = 1;
  TASK_CHECK (oggz_write_feed (h.w, &op, SERIAL1, 0, NULL)
	      == OGGZ_ERR_IO_AGAIN);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 10);

  /* nil packets always fit and keep their place in the sequence; flush it
   * so it reaches a page and can be read back */
  TASK_CHECK (harness_feed (&h, SERIAL1, 0, 2, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 10);
  TASK_CHECK (h.nledger == 2);

  harness_drain_output (&h, 64);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  harness_verify_roundtrip (&h);
  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

/* ------------------------------------------------------------------ */
/* pages group                                                         */
/* ------------------------------------------------------------------ */

static int
test_shared_page_released_together (void)
{
  struct harness h;
  long n, psize;
  unsigned char *buf;
  static const long lens[4] = { 10, 10, 10, 5 };
  int i;

  harness_init (&h, 0); /* unlimited: accounting is still active */

  /* establish the stream with a BOS packet on its own page and drain it */
  TASK_CHECK (harness_feed (&h, SERIAL1, 3, 0, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);
  harness_drain_output (&h, 64);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);

  /* four non-BOS packets share one page when the last one flushes */
  TASK_CHECK (harness_feed (&h, SERIAL1, 10, 1, 0, 0, NULL, 0) == 0);
  TASK_CHECK (harness_feed (&h, SERIAL1, 10, 2, 0, 0, NULL, 0) == 0);
  TASK_CHECK (harness_feed (&h, SERIAL1, 10, 3, 0, 0, NULL, 0) == 0);
  TASK_CHECK (harness_feed (&h, SERIAL1, 5, 4, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 35);

  /* Determine that page's size independently: a private libogg stream
   * whose BOS is already done, then the same four packets flushed. */
  {
    ogg_stream_state local;
    ogg_page og;
    unsigned char store[64];
    ogg_packet p;
    ogg_stream_init (&local, SERIAL1);
    memset (&p, 0, sizeof p);
    p.packet = store; p.bytes = 3; p.b_o_s = 1;
    ogg_stream_packetin (&local, &p);
    ogg_stream_flush (&local, &og); /* advances the stream past BOS */
    for (i = 0; i < 4; i++) {
      memset (&p, 0, sizeof p);
      p.packet = store; p.bytes = lens[i];
      ogg_stream_packetin (&local, &p);
    }
    TASK_CHECK (ogg_stream_flush (&local, &og) == 1);
    TASK_CHECK (ogg_page_packets (&og) == 4);
    psize = og.header_len + og.body_len;
    ogg_stream_clear (&local);
  }
  TASK_CHECK (psize > 35);
  buf = malloc ((size_t) psize);

  /* all but the final byte of this page: nothing may be released.
   * h.outlen already holds the earlier BOS page, so measure a delta. */
  {
    size_t base = h.outlen;
    while ((long) (h.outlen - base) < psize - 1) {
      long want = psize - 1 - (long) (h.outlen - base);
      n = oggz_write_output (h.w, buf, want);
      TASK_CHECK (n > 0);
      harness_append_output (&h, buf, n);
      harness_account_output (&h, buf, n);
      TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 35);
    }
    TASK_CHECK ((long) (h.outlen - base) == psize - 1);
  }
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 35);

  /* final byte: the whole page's packets are released together */
  n = oggz_write_output (h.w, buf, 1);
  TASK_CHECK (n == 1);
  harness_append_output (&h, buf, n);
  harness_account_output (&h, buf, n);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  TASK_CHECK (h.expected_pending == 0);

  free (buf);
  harness_drain_output (&h, 64);
  harness_verify_roundtrip (&h);
  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

static int
test_multipage_packet_counted_once (void)
{
  struct harness h;
  unsigned char *buf = malloc (256);
  long n;
  int pages = 0;

  harness_init (&h, 0);
  TASK_CHECK (harness_feed (&h, SERIAL1, 70000, 9, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 70000);

  while ((n = oggz_write_output (h.w, buf, 256)) > 0) {
    harness_append_output (&h, buf, n);
    harness_account_output (&h, buf, n);
    pages++;
    /* continuation pages that finish no packet release nothing: the
     * value can only be the full size or zero, never an intermediate */
    TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 70000 ||
		oggz_write_get_pending_bytes (h.w) == 0);
  }
  TASK_CHECK (n == 0);
  TASK_CHECK (pages >= 2);                 /* genuinely spanned pages */
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  TASK_CHECK (h.expected_pending == 0);

  harness_verify_roundtrip (&h);
  free (buf);
  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

static int
test_bytewise_partial_pages (void)
{
  struct harness h;
  int i;

  harness_init (&h, 0);
  for (i = 0; i < 6; i++)
    TASK_CHECK (harness_feed (&h, SERIAL1, 255, i,
			      i == 5 ? OGGZ_FLUSH_AFTER : 0, 0, NULL, 0)
		== 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 6 * 255);

  /* one byte at a time: partial headers/bodies never release early */
  harness_drain_output (&h, 1);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  TASK_CHECK (h.expected_pending == 0);
  harness_verify_roundtrip (&h);
  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

/* A run of nil packets keeps FIFO slots while contributing zero bytes;
 * their pages must still pop the slots so later packets stay aligned. */
static int
test_nil_packet_sequence (void)
{
  struct harness h;
  int i;

  harness_init (&h, 0);
  for (i = 0; i < 5; i++)
    TASK_CHECK (harness_feed (&h, SERIAL1, 0, i,
			      i == 4 ? OGGZ_FLUSH_AFTER : 0, 0, NULL, 0)
		== 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  TASK_CHECK (h.nledger == 5);

  /* deliver the nil pages while the running byte total is zero; their
   * FIFO slots must still be released */
  harness_drain_output (&h, 64);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  TASK_CHECK (h.expected_pending == 0);

  /* a real packet after the drained nil run must be accounted and released
   * correctly, proving the FIFO head was advanced past the nil slots */
  TASK_CHECK (harness_feed (&h, SERIAL1, 77, 9, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 77);
  harness_drain_output (&h, 64);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  TASK_CHECK (h.expected_pending == 0);
  harness_verify_roundtrip (&h);
  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

static int
test_flush_before_after (void)
{
  struct harness h;

  harness_init (&h, 0);
  TASK_CHECK (harness_feed (&h, SERIAL1, 300, 1, OGGZ_FLUSH_BEFORE, 0,
			    NULL, 0) == 0);
  TASK_CHECK (harness_feed (&h, SERIAL1, 300, 2, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);
  TASK_CHECK (harness_feed (&h, SERIAL1, 255, 3,
			    OGGZ_FLUSH_BEFORE | OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 855);

  harness_drain_output (&h, 128);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  harness_verify_roundtrip (&h);
  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

/* ------------------------------------------------------------------ */
/* resume group                                                        */
/* ------------------------------------------------------------------ */

static int
test_drain_unblocks_retry (void)
{
  struct harness h;

  harness_init (&h, 10);

  TASK_CHECK (harness_feed (&h, SERIAL1, 10, 4, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);

  /* a 12-byte packet is refused while 10 bytes are already in flight */
  TASK_CHECK (harness_feed (&h, SERIAL1, 12, 5, 0, 0, NULL,
			    OGGZ_ERR_IO_AGAIN) == OGGZ_ERR_IO_AGAIN);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 10);

  harness_drain_output (&h, 64);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);

  /* after draining, a packet that fits the allowance is admitted without
   * any limit change */
  TASK_CHECK (harness_feed (&h, SERIAL1, 5, 6, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 5);
  harness_drain_output (&h, 64);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);

  /* the original 12-byte packet still exceeds the allowance; raising the
   * limit (the drain-or-raise condition from the contract) admits it */
  TASK_CHECK (oggz_write_set_pending_limit (h.w, 12) == 0);
  TASK_CHECK (harness_feed (&h, SERIAL1, 12, 5, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 12);
  harness_drain_output (&h, 64);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  harness_verify_roundtrip (&h);
  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

static int
test_short_write_resume (void)
{
  struct harness h;
  struct sink s;
  long last = 0;

  memset (&s, 0, sizeof s);
  s.buf = malloc (1 << 16);
  s.cap = 1 << 16;
  s.mode = 1;
  s.short_n = 13;

  harness_init (&h, 0);
  TASK_CHECK (oggz_io_set_write (h.w, sink_cb, &s) == 0);
  TASK_CHECK (harness_feed (&h, SERIAL1, 5000, 5, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);
  harness_drain_write (&h, &s, 4096, &last);
  TASK_CHECK (last == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  TASK_CHECK (s.len == h.outlen);
  /* every downstream call made real progress: no busy loop */
  TASK_CHECK (s.calls > 0 && (long) s.calls * 13 >= (long) s.len);
  harness_verify_roundtrip (&h);

  free (s.buf);
  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

static int
test_pause_and_hard_error (void)
{
  struct harness h;
  struct sink s;
  long n, last = 0;

  memset (&s, 0, sizeof s);
  s.buf = malloc (1 << 16);
  s.cap = 1 << 16;

  harness_init (&h, 0);
  TASK_CHECK (oggz_io_set_write (h.w, sink_cb, &s) == 0);
  TASK_CHECK (harness_feed (&h, SERIAL1, 3000, 6, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);

  /* zero write pauses with IO_AGAIN: nothing delivered, pending held */
  s.mode = 2;
  n = oggz_write (h.w, 4096);
  TASK_CHECK (n == OGGZ_ERR_IO_AGAIN);
  TASK_CHECK (s.len == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 3000);

  /* EAGAIN via (size_t)-1 behaves the same */
  s.mode = 3;
  n = oggz_write (h.w, 4096);
  TASK_CHECK (n == OGGZ_ERR_IO_AGAIN);
  TASK_CHECK (s.len == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 3000);

  /* hard failure is reported, then a healthy sink drains the rest */
  s.mode = 4;
  n = oggz_write (h.w, 4096);
  TASK_CHECK (n == OGGZ_ERR_SYSTEM);
  TASK_CHECK (s.hard_seen == 1);

  s.mode = 0;
  harness_drain_write (&h, &s, 4096, &last);
  TASK_CHECK (last == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  harness_verify_roundtrip (&h);

  free (s.buf);
  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

static int
test_progress_then_error_reports_once (void)
{
  struct harness h;
  struct sink s;
  long n;
  size_t progress;

  memset (&s, 0, sizeof s);
  s.buf = malloc (1 << 16);
  s.cap = 1 << 16;
  s.mode = 1;
  s.short_n = 7;

  harness_init (&h, 0);
  TASK_CHECK (oggz_io_set_write (h.w, sink_cb, &s) == 0);
  TASK_CHECK (harness_feed (&h, SERIAL1, 4000, 8, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);

  /* make exactly seven bytes of progress, then stop for this call */
  n = oggz_write (h.w, 7);
  TASK_CHECK (n == 7);
  progress = s.len;
  TASK_CHECK (progress == 7);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 4000);
  /* keep the independent pipeline in step with the direct write */
  harness_append_output (&h, s.buf, 7);
  harness_account_output (&h, s.buf, 7);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == h.expected_pending);

  /* fail on the next call: the deferred SYSTEM is returned once, and no
   * bytes move */
  s.mode = 4;
  n = oggz_write (h.w, 4096);
  TASK_CHECK (n == OGGZ_ERR_SYSTEM);
  TASK_CHECK (s.len == progress);

  /* the error is one-shot: a healthy sink resumes from the same byte */
  s.mode = 0;
  {
    long last = 0;
    harness_drain_write (&h, &s, 4096, &last);
    TASK_CHECK (last == 0);
  }
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  harness_verify_roundtrip (&h);

  free (s.buf);
  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

static int
test_progress_then_pause_resumes (void)
{
  struct harness h;
  struct sink s;
  long n, total;

  memset (&s, 0, sizeof s);
  s.buf = malloc (1 << 16);
  s.cap = 1 << 16;
  s.mode = 1;
  s.short_n = 7;

  harness_init (&h, 0);
  TASK_CHECK (oggz_io_set_write (h.w, sink_cb, &s) == 0);
  TASK_CHECK (harness_feed (&h, SERIAL1, 4000, 9, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);

  /* first call makes exactly seven bytes of progress */
  n = oggz_write (h.w, 7);
  TASK_CHECK (n == 7);
  TASK_CHECK (s.len == 7);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 4000);

  /* a zero-write pause now returns IO_AGAIN without advancing, and the
   * unwritten bytes (and pending count) are left intact */
  s.mode = 2;
  n = oggz_write (h.w, 4096);
  TASK_CHECK (n == OGGZ_ERR_IO_AGAIN);
  TASK_CHECK (s.len == 7);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 4000);

  /* a healthy sink resumes from the first unwritten byte; roundtrip plus
   * the sink length confirm no byte was skipped or written twice */
  s.mode = 0;
  total = 7;
  while ((n = oggz_write (h.w, 4096)) > 0) total += n;
  TASK_CHECK (n == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  TASK_CHECK ((long) s.len == total);
  harness_append_output (&h, s.buf, (long) s.len);
  harness_account_output (&h, s.buf, (long) s.len);
  TASK_CHECK (h.expected_pending == 0);
  harness_verify_roundtrip (&h);

  free (s.buf);
  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

/* ------------------------------------------------------------------ */
/* streams group                                                       */
/* ------------------------------------------------------------------ */

static int
test_dual_serial_interleave (void)
{
  struct harness h;
  int i;

  harness_init (&h, 0);

  /* BOS packets of both logical streams, each on its own page */
  TASK_CHECK (harness_feed (&h, SERIAL1, 255, 1, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);
  TASK_CHECK (harness_feed (&h, SERIAL2, 255, 11, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);

  for (i = 0; i < 8; i++) {
    long serial = (i % 2) ? SERIAL2 : SERIAL1;
    long bytes = (i % 3 == 0) ? 2550 : (i % 3 == 1) ? 256 : 510;
    /* flush each packet so every interleaved page reaches the reader */
    int flush = OGGZ_FLUSH_AFTER;
    TASK_CHECK (harness_feed (&h, serial, bytes, 20 + i, flush, 0,
			      NULL, 0) == 0);
  }
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == h.expected_pending);

  harness_drain_output (&h, 97);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  TASK_CHECK (h.expected_pending == 0);
  TASK_CHECK (h.nledger == 10);
  harness_verify_roundtrip (&h);
  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

static int
test_eos_and_close (void)
{
  struct harness h;

  harness_init (&h, 100000);
  TASK_CHECK (harness_feed (&h, SERIAL1, 100, 1, 0, 0, NULL, 0) == 0);
  TASK_CHECK (harness_feed (&h, SERIAL1, 100, 2, 0, 0, NULL, 0) == 0);
  TASK_CHECK (harness_feed (&h, SERIAL1, 100, 3, OGGZ_FLUSH_AFTER, 1,
			    NULL, 0) == 0); /* EOS on the final packet */
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 300);
  TASK_CHECK (oggz_get_eos (h.w, SERIAL1) == 1);

  harness_drain_output (&h, 200);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  harness_verify_roundtrip (&h);
  harness_free (&h); /* close while a limit is still configured */
  TASK_CHECK (contract_failures == 0);
  return 0;
}

struct hungry_ctx {
  int fed;
  int limit_seen_ok;
};

static int
hungry_cb (OGGZ *z, int empty, void *user)
{
  struct hungry_ctx *c = user;
  unsigned char data[50];
  ogg_packet op;
  long pending;
  int ret;

  (void) empty;
  if (c->fed >= 6) return OGGZ_STOP_OK;

  pending = oggz_write_get_pending_bytes (z);
  if (pending < 0 || pending > 200) c->limit_seen_ok = 0;

  fill_pattern (data, sizeof data, c->fed);
  memset (&op, 0, sizeof op);
  op.packet = data; op.bytes = 50; op.b_o_s = (c->fed == 0);
  op.granulepos = c->fed; op.packetno = c->fed;
  ret = oggz_write_feed (z, &op, SERIAL1,
			 c->fed == 5 ? OGGZ_FLUSH_AFTER : 0, NULL);
  if (ret == OGGZ_ERR_IO_AGAIN) return OGGZ_STOP_OK; /* producer backs off */
  if (ret != 0) return OGGZ_STOP_ERR;
  c->fed++;
  return 0;
}

static int
test_hungry_feed_query_stop (void)
{
  OGGZ *w = oggz_new (OGGZ_WRITE);
  struct hungry_ctx c = { 0, 1 };
  unsigned char buf[512];
  long n;

  TASK_CHECK (oggz_write_set_pending_limit (w, 200) == 0);
  TASK_CHECK (oggz_write_set_hungry_callback (w, hungry_cb, 1, &c) == 0);

  while ((n = oggz_write_output (w, buf, sizeof buf)) > 0);
  TASK_CHECK (n == OGGZ_ERR_STOP_OK);
  TASK_CHECK (c.fed >= 1);
  TASK_CHECK (c.limit_seen_ok == 1);

  TASK_CHECK (oggz_close (w) == 0);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

static int
recursive_hungry (OGGZ *z, int empty, void *user)
{
  unsigned char buf[1];
  int *seen = user;
  long n;
  (void) empty;
  n = oggz_write_output (z, buf, 1);
  if (n == OGGZ_ERR_RECURSIVE_WRITE) *seen = 1;
  return OGGZ_STOP_OK;
}

static int
test_recursive_write_guard (void)
{
  OGGZ *w = oggz_new (OGGZ_WRITE);
  int seen = 0;
  unsigned char buf[1];
  long n;

  TASK_CHECK (oggz_write_set_hungry_callback (w, recursive_hungry, 1,
					      &seen) == 0);
  while ((n = oggz_write_output (w, buf, 1)) > 0);
  TASK_CHECK (n == OGGZ_ERR_STOP_OK);
  TASK_CHECK (seen == 1);
  TASK_CHECK (oggz_close (w) == 0);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

static int
test_hungry_guard_combo (void)
{
  OGGZ *w = oggz_new (OGGZ_WRITE);
  static unsigned char data[40];
  int guard_reject = 0, guard_accept = 0;
  unsigned char out[256];
  ogg_packet op;
  long psize, n, got = 0;

  TASK_CHECK (oggz_write_set_pending_limit (w, 20) == 0);
  fill_pattern (data, sizeof data, 2);

  /* rejected guarded feed: the guard must not be touched */
  memset (&op, 0, sizeof op);
  op.packet = data; op.bytes = 40; op.b_o_s = 1;
  op.granulepos = 0; op.packetno = 0;
  TASK_CHECK (oggz_write_feed (w, &op, SERIAL1, 0, &guard_reject)
	      == OGGZ_ERR_IO_AGAIN);
  TASK_CHECK (guard_reject == 0);

  /* accepted guarded packet: the guard is a lifetime marker, not a
   * page-commit notification */
  TASK_CHECK (oggz_write_set_pending_limit (w, 40) == 0);
  TASK_CHECK (oggz_write_feed (w, &op, SERIAL1, OGGZ_FLUSH_AFTER,
			       &guard_accept) == 0);
  TASK_CHECK (guard_accept == 0);

  /* deliver one byte at a time up to but not including the final page
   * byte; derive the page size with a private libogg stream rather than
   * assuming oggz internals */
  {
    ogg_stream_state local;
    ogg_page og;
    ogg_packet p;
    ogg_stream_init (&local, SERIAL1);
    memset (&p, 0, sizeof p);
    p.packet = data; p.bytes = 40; p.b_o_s = 1;
    ogg_stream_packetin (&local, &p);
    TASK_CHECK (ogg_stream_flush (&local, &og) == 1);
    psize = og.header_len + og.body_len;
    ogg_stream_clear (&local);
  }
  while (got < psize - 1) {
    n = oggz_write_output (w, out, 1);
    TASK_CHECK (n == 1);
    got += n;
  }
  TASK_CHECK (got == psize - 1);
  TASK_CHECK (oggz_write_get_pending_bytes (w) == 40);
  TASK_CHECK (guard_accept == 0);

  /* the final byte delivers the page and releases the bytes, but must not
   * itself mark the guard */
  n = oggz_write_output (w, out, 1);
  TASK_CHECK (n == 1);
  TASK_CHECK (oggz_write_get_pending_bytes (w) == 0);
  TASK_CHECK (guard_accept == 0);

  /* the packet structure is freed on the next make-packet cycle, marking
   * the guard under the existing memory-ownership rule */
  while ((n = oggz_write_output (w, out, sizeof out)) > 0);
  TASK_CHECK (n == 0);
  TASK_CHECK (guard_accept == 1);
  TASK_CHECK (guard_reject == 0);
  TASK_CHECK (oggz_close (w) == 0);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

static int
test_full_mix_roundtrip (void)
{
  struct harness h;
  /* nil, single byte, exact 255 multiples, a 255+1 boundary, a 2*255
   * boundary, and a packet spanning several pages */
  static const long sizes[] = { 0, 1, 255, 256, 510, 2550, 70000 };
  int i, k, ret;

  /* every packet fits the allowance on its own, so backpressure retries
   * always terminate */
  harness_init (&h, 80000);

  /* strict Ogg requires all BOS pages first: establish both streams */
  TASK_CHECK (harness_feed (&h, SERIAL1, sizes[0], 0, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);
  TASK_CHECK (harness_feed (&h, SERIAL2, sizes[0], 100, OGGZ_FLUSH_AFTER, 0,
			    NULL, 0) == 0);

  for (k = 0; k < 2; k++) {
    long serial = k ? SERIAL2 : SERIAL1;
    for (i = 1; i < (int) (sizeof sizes / sizeof sizes[0]); i++) {
      long bytes = sizes[i];
      int pattern = k * 100 + i;
      int last = (i == (int) (sizeof sizes / sizeof sizes[0]) - 1);
      int flush = (bytes >= 2550 || last) ? OGGZ_FLUSH_AFTER : 0;
      int eos = (k == 1 && last);
      while ((ret = harness_feed (&h, serial, bytes, pattern, flush, eos,
				  NULL, 0)) == OGGZ_ERR_IO_AGAIN) {
	harness_drain_output (&h, 512);
      }
      TASK_CHECK (ret == 0);
      if (bytes > 4096)
	TASK_CHECK (oggz_write_get_pending_bytes (h.w) >= bytes);
    }
    /* keep each stream's in-flight total under the allowance */
    harness_drain_output (&h, 512);
    TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  }

  harness_drain_output (&h, 333);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  TASK_CHECK (h.expected_pending == 0);
  TASK_CHECK (h.nledger == (int) (2 * (sizeof sizes / sizeof sizes[0])));
  harness_verify_roundtrip (&h);
  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

static int
test_unlimited_behaviour_unchanged (void)
{
  struct harness h;
  int i;

  harness_init (&h, 0); /* default: no limit */
  for (i = 0; i < 12; i++) {
    long bytes = (i % 4 == 0) ? 2550 : (i % 4 == 1) ? 0 : 128;
    TASK_CHECK (harness_feed (&h, SERIAL1, bytes, i,
			      i == 11 ? OGGZ_FLUSH_AFTER : 0, 0, NULL, 0)
		== 0);
  }
  harness_drain_output (&h, 4096);
  TASK_CHECK (oggz_write_get_pending_bytes (h.w) == 0);
  harness_verify_roundtrip (&h);
  harness_free (&h);
  TASK_CHECK (contract_failures == 0);
  return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
  { "admission", "defaults_and_validation", test_defaults_and_validation },
  { "admission", "reject_preserves_state_and_guard",
    test_reject_preserves_state_and_guard },
  { "admission", "boundary_overflow_and_nil", test_boundary_overflow_and_nil },

  { "pages", "shared_page_released_together",
    test_shared_page_released_together },
  { "pages", "multipage_packet_counted_once",
    test_multipage_packet_counted_once },
  { "pages", "bytewise_partial_pages", test_bytewise_partial_pages },
  { "pages", "nil_packet_sequence", test_nil_packet_sequence },
  { "pages", "flush_before_after", test_flush_before_after },

  { "resume", "drain_unblocks_retry", test_drain_unblocks_retry },
  { "resume", "short_write_resume", test_short_write_resume },
  { "resume", "pause_and_hard_error", test_pause_and_hard_error },
  { "resume", "progress_then_error_reports_once",
    test_progress_then_error_reports_once },
  { "resume", "progress_then_pause_resumes",
    test_progress_then_pause_resumes },

  { "streams", "dual_serial_interleave", test_dual_serial_interleave },
  { "streams", "eos_and_close", test_eos_and_close },
  { "streams", "hungry_feed_query_stop", test_hungry_feed_query_stop },
  { "streams", "recursive_write_guard", test_recursive_write_guard },
  { "streams", "hungry_guard_combo", test_hungry_guard_combo },
  { "streams", "full_mix_roundtrip", test_full_mix_roundtrip },
  { "streams", "unlimited_behaviour_unchanged",
    test_unlimited_behaviour_unchanged },
};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
