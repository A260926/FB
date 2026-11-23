/*
 * Pending-byte back-pressure write contract.
 *
 * Registered with the task driver through TASK_CHECK / task_cases /
 * task_case_count in four groups: admission, pages, resume, streams.
 *
 * The test independently models the pending count: every admitted packet
 * length is recorded in per-serial queues, and every fully delivered Ogg
 * page (parsed directly with the in-tree libogg) releases the packets that
 * end on that page, as reported by ogg_page_packets(). The model is
 * reconciled against oggz_write_get_pending_bytes() after every feed and
 * every output chunk.
 */

#include "task_support/check.h"

#include <oggz/oggz.h>
#include <ogg/ogg.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define N_SERIALS 2
#define MAX_QUEUE 128
#define MAX_DESCS 128

/* ---------------- test packet descriptions ---------------- */

struct pkt_desc {
  long serial;
  long len;
  int flush;
  long b_o_s;
  long e_o_s;
  ogg_int64_t granule;
  ogg_int64_t packetno;
};

static void
fill_packet (unsigned char * buf, long serial, ogg_int64_t packetno, long len)
{
  long i;
  for (i = 0; i < len; i++)
    buf[i] = (unsigned char) (i * 7 + serial * 3 + packetno * 5);
}

static void
desc_to_ogg (ogg_packet * op, unsigned char * buf, const struct pkt_desc * d)
{
  fill_packet (buf, d->serial, d->packetno, d->len);
  op->packet = buf;
  op->bytes = d->len;
  op->b_o_s = d->b_o_s;
  op->e_o_s = d->e_o_s;
  op->granulepos = d->granule;
  op->packetno = d->packetno;
}

/* ---------------- output byte buffer ---------------- */

struct obuf {
  unsigned char * data;
  size_t len;
  size_t cap;
};

static int
obuf_append (struct obuf * b, const void * p, size_t n)
{
  if (b->len + n > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 4096;
    while (cap < b->len + n) cap *= 2;
    unsigned char * nd = (unsigned char *) realloc (b->data, cap);
    if (nd == NULL) return -1;
    b->data = nd;
    b->cap = cap;
  }
  memcpy (b->data + b->len, p, n);
  b->len += n;
  return 0;
}

static void
obuf_free (struct obuf * b)
{
  free (b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

/* ---------------- independent pending model ---------------- */

struct model {
  ogg_sync_state sync;
  ogg_int64_t pending;
  long serials[N_SERIALS];
  long queue[N_SERIALS][MAX_QUEUE];
  int qh[N_SERIALS];
  int qt[N_SERIALS];
};

static int
model_serial_index (struct model * m, long serial)
{
  int i;
  for (i = 0; i < N_SERIALS; i++) {
    if (m->serials[i] == serial) return i;
  }
  for (i = 0; i < N_SERIALS; i++) {
    if (m->qt[i] == m->qh[i]) {
      m->serials[i] = serial;
      m->qt[i] = m->qh[i] = 0;
      return i;
    }
  }
  return -1;
}

static int
model_init (struct model * m)
{
  int i;
  memset (m, 0, sizeof (*m));
  if (ogg_sync_init (&m->sync) != 0) return -1;
  for (i = 0; i < N_SERIALS; i++) m->serials[i] = 0;
  return 0;
}

static void
model_clear (struct model * m)
{
  ogg_sync_clear (&m->sync);
}

static int
model_admit (struct model * m, long serial, long len)
{
  int idx = model_serial_index (m, serial);
  TASK_CHECK (idx >= 0);
  TASK_CHECK (m->qt[idx] < MAX_QUEUE);
  m->queue[idx][m->qt[idx]++] = len;
  m->pending += len;
  return 0;
}

/* Feed freshly produced bytes; release original lengths for every packet
 * that ends on each complete page found. */
static int
model_pump (struct model * m, const unsigned char * data, long n)
{
  char * sbuf;
  ogg_page og;
  long r;

  if (n == 0) return 0;

  sbuf = ogg_sync_buffer (&m->sync, n);
  TASK_CHECK (sbuf != NULL);
  memcpy (sbuf, data, n);
  TASK_CHECK (ogg_sync_wrote (&m->sync, n) == 0);

  while ((r = ogg_sync_pageseek (&m->sync, &og)) != 0) {
    int idx, npackets, i;
    if (r < 0) continue; /* resync; must not happen on valid output */

    idx = model_serial_index (m, ogg_page_serialno (&og));
    TASK_CHECK (idx >= 0);

    npackets = ogg_page_packets (&og);
    for (i = 0; i < npackets; i++) {
      TASK_CHECK (m->qh[idx] < m->qt[idx]);
      m->pending -= m->queue[idx][m->qh[idx]++];
    }
  }

  TASK_CHECK (m->pending >= 0);
  return 0;
}

static int
model_check (OGGZ * z, struct model * m)
{
  TASK_CHECK (oggz_write_get_pending_bytes (z) == m->pending);
  return 0;
}

/* ---------------- feeding ---------------- */

static int
feed_desc (OGGZ * z, struct model * m, const struct pkt_desc * d,
           unsigned char * buf, int * guard)
{
  ogg_packet op;
  int r;

  desc_to_ogg (&op, buf, d);
  r = oggz_write_feed (z, &op, d->serial, d->flush, guard);
  if (r == 0) {
    TASK_CHECK (model_admit (m, d->serial, d->len) == 0);
    TASK_CHECK (model_check (z, m) == 0);
  }
  return r;
}

/* Drain everything through oggz_write_output(), reconciling each chunk. */
static int
drain_output (OGGZ * z, struct obuf * out, struct model * m, long chunk)
{
  unsigned char tmp[4096];
  long n;
  long iterations = 0;

  TASK_CHECK (chunk > 0 && chunk <= (long) sizeof (tmp));

  for (;;) {
    size_t before = out->len;
    n = oggz_write_output (z, tmp, chunk);
    TASK_CHECK (++iterations < 2000000);
    if (n > 0) {
      TASK_CHECK (obuf_append (out, tmp, (size_t) n) == 0);
      TASK_CHECK (model_pump (m, out->data + before, n) == 0);
      TASK_CHECK (model_check (z, m) == 0);
    } else if (n == 0) {
      /* oggz_write_output needs a second call to force the final flush of
       * an unflushed tail; it is truly done only once nothing is pending. */
      if (oggz_write_get_pending_bytes (z) == 0) break;
    } else {
      TASK_CHECK (n >= 0);
    }
  }

  TASK_CHECK (oggz_write_get_pending_bytes (z) == 0);
  TASK_CHECK (m->pending == 0);
  return 0;
}

/* Build a reference output for a descriptor list using oggz_write_output
 * with an unlimited handle, so resume scenarios can byte-compare. */
static int
reference_output (const struct pkt_desc * d, int nd, struct obuf * out)
{
  OGGZ * z;
  struct model m;
  unsigned char * big;
  long maxlen = 0;
  int i;

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);

  for (i = 0; i < nd; i++) if (d[i].len > maxlen) maxlen = d[i].len;
  big = (unsigned char *) malloc ((size_t) (maxlen ? maxlen : 1));
  TASK_CHECK (big != NULL);

  for (i = 0; i < nd; i++) {
    TASK_CHECK (feed_desc (z, &m, &d[i], big, NULL) == 0);
  }

  TASK_CHECK (drain_output (z, out, &m, 777) == 0);

  free (big);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

/* ---------------- real reader roundtrip check ---------------- */

struct reader_ctx {
  long serials[N_SERIALS];
  const struct pkt_desc * by_serial[N_SERIALS][MAX_DESCS];
  int counts[N_SERIALS];
  int pos[N_SERIALS];
  int seen;
};

static int
reader_index (struct reader_ctx * rc, long serial)
{
  int i;
  for (i = 0; i < N_SERIALS; i++) {
    if (rc->serials[i] == serial) return i;
  }
  for (i = 0; i < N_SERIALS; i++) {
    if (rc->counts[i] == 0) {
      rc->serials[i] = serial;
      return i;
    }
  }
  return -1;
}

static int
read_packet_cb (OGGZ * z, oggz_packet * zp, long serial, void * user_data)
{
  struct reader_ctx * rc = (struct reader_ctx *) user_data;
  ogg_packet * op = &zp->op;
  int idx = reader_index (rc, serial);
  const struct pkt_desc * d;
  unsigned char * expected;
  long i;

  (void) z;

  TASK_CHECK (idx >= 0);
  TASK_CHECK (rc->pos[idx] < rc->counts[idx]);
  d = rc->by_serial[idx][rc->pos[idx]++];

  TASK_CHECK (op->bytes == d->len);
  /* libogg reports BOS/EOS as raw flag bits (0x100/0x200); compare truth. */
  TASK_CHECK (!!op->b_o_s == !!d->b_o_s);
  TASK_CHECK (!!op->e_o_s == !!d->e_o_s);
  /* Only the last packet ending on a page records a granule position; other
   * packets on that page decode as -1. Whenever it is recorded it must match. */
  TASK_CHECK (op->granulepos == -1 || op->granulepos == d->granule);
  TASK_CHECK (op->packetno == d->packetno);

  expected = (unsigned char *) malloc ((size_t) (d->len ? d->len : 1));
  TASK_CHECK (expected != NULL);
  fill_packet (expected, d->serial, d->packetno, d->len);
  for (i = 0; i < d->len; i++) {
    TASK_CHECK (op->packet[i] == expected[i]);
  }
  free (expected);

  rc->seen++;
  return 0;
}

static int
run_reader (const struct obuf * out, const struct pkt_desc * d, int nd)
{
  OGGZ * r;
  struct reader_ctx rc;
  int i, idx, total;

  memset (&rc, 0, sizeof (rc));
  for (i = 0; i < nd; i++) {
    idx = reader_index (&rc, d[i].serial);
    TASK_CHECK (idx >= 0);
    TASK_CHECK (rc.counts[idx] < MAX_DESCS);
    rc.by_serial[idx][rc.counts[idx]++] = &d[i];
  }

  r = oggz_new (OGGZ_READ);
  TASK_CHECK (r != NULL);
  TASK_CHECK (oggz_set_read_callback (r, -1, read_packet_cb, &rc) == 0);
  TASK_CHECK (oggz_read_input (r, out->data, (long) out->len) == (long) out->len);
  TASK_CHECK (oggz_close (r) == 0);

  total = 0;
  for (i = 0; i < N_SERIALS; i++) {
    TASK_CHECK (rc.pos[i] == rc.counts[i]);
    total += rc.counts[i];
  }
  TASK_CHECK (rc.seen == nd);
  TASK_CHECK (total == nd);
  return 0;
}

/* ---------------- controllable write callback ---------------- */

enum sink_mode {
  SINK_FULL = 0,
  SINK_ZERO,
  SINK_EAGAIN,
  SINK_SHORT,
  SINK_HARD,
  SINK_FAIL_SECOND
};

struct sink {
  struct obuf * out;
  enum sink_mode mode;
  size_t short_n;
  int hard_fail_left;
  int invocations;
  int invocations_in_call;
};

static size_t
sink_cb (void * user_handle, void * buf, size_t n)
{
  struct sink * s = (struct sink *) user_handle;

  s->invocations++;
  s->invocations_in_call++;

  switch (s->mode) {
  case SINK_FULL:
    if (obuf_append (s->out, buf, n) != 0) return (size_t) -1;
    return n;
  case SINK_ZERO:
    return 0;
  case SINK_EAGAIN:
    errno = EAGAIN;
    return (size_t) -1;
  case SINK_SHORT: {
    size_t k = n < s->short_n ? n : s->short_n;
    if (obuf_append (s->out, buf, k) != 0) return (size_t) -1;
    return k;
  }
  case SINK_HARD:
    if (s->hard_fail_left > 0) {
      s->hard_fail_left--;
      errno = EIO;
      return (size_t) -1;
    }
    if (obuf_append (s->out, buf, n) != 0) return (size_t) -1;
    return n;
  case SINK_FAIL_SECOND:
    if (s->invocations_in_call == 1) {
      if (obuf_append (s->out, buf, n) != 0) return (size_t) -1;
      return n;
    }
    errno = EIO;
    return (size_t) -1;
  }

  return 0;
}

/* Drive oggz_write() against the sink, pumping the model after each call. */
static int
drain_write (OGGZ * z, struct sink * sink, struct model * m)
{
  long n;
  long iterations = 0;

  for (;;) {
    size_t before = sink->out->len;
    sink->invocations_in_call = 0;
    n = oggz_write (z, 1024);
    TASK_CHECK (++iterations < 2000000);
    if (n == 0) break;
    if (n < 0) return (int) n;
    TASK_CHECK (sink->out->len == before + (size_t) n);
    TASK_CHECK (model_pump (m, sink->out->data + before, n) == 0);
    TASK_CHECK (model_check (z, m) == 0);
  }

  return 0;
}

/* =====================================================================
 * admission group
 * ===================================================================== */

static int
case_admission_validation (void)
{
  OGGZ * w, * r;

  w = oggz_new (OGGZ_WRITE);
  TASK_CHECK (w != NULL);

  /* Default: unlimited, nothing pending. */
  TASK_CHECK (oggz_write_get_pending_bytes (w) == 0);
  TASK_CHECK (oggz_write_set_pending_limit (w, 0) == 0);

  /* Negative limits are rejected. */
  TASK_CHECK (oggz_write_set_pending_limit (w, -1) == OGGZ_ERR_INVALID);
  TASK_CHECK (oggz_write_set_pending_limit (w, -100000) == OGGZ_ERR_INVALID);

  /* Read handles reject both calls. */
  r = oggz_new (OGGZ_READ);
  TASK_CHECK (r != NULL);
  TASK_CHECK (oggz_write_set_pending_limit (r, 10) == OGGZ_ERR_INVALID);
  TASK_CHECK (oggz_write_get_pending_bytes (r) < 0);

  /* NULL handles. */
  TASK_CHECK (oggz_write_set_pending_limit (NULL, 10) == OGGZ_ERR_BAD_OGGZ);
  TASK_CHECK (oggz_write_get_pending_bytes (NULL) == OGGZ_ERR_BAD_OGGZ);

  TASK_CHECK (oggz_close (r) == 0);
  TASK_CHECK (oggz_close (w) == 0);
  return 0;
}

static int
case_admission_capacity_retry (void)
{
  OGGZ * z;
  struct model m;
  struct obuf out = {0};
  unsigned char buf[16];
  ogg_packet op;
  struct pkt_desc d = { 17, 11, OGGZ_FLUSH_AFTER, 1, 1, 0, 0 };
  int guard = 0;

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);

  TASK_CHECK (oggz_write_set_pending_limit (z, 10) == 0);

  desc_to_ogg (&op, buf, &d);

  /* Over capacity: rejected, nothing enqueued, no stream created. */
  TASK_CHECK (oggz_write_feed (z, &op, 17, OGGZ_FLUSH_AFTER, &guard)
              == OGGZ_ERR_IO_AGAIN);
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 0);
  TASK_CHECK (m.pending == 0);
  TASK_CHECK (guard == 0);
  TASK_CHECK (oggz_get_numtracks (z) == 0);

  /* Raising the limit lets the very same packet through unchanged. */
  TASK_CHECK (oggz_write_set_pending_limit (z, 11) == 0);
  TASK_CHECK (feed_desc (z, &m, &d, buf, &guard) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 11);

  TASK_CHECK (drain_output (z, &out, &m, 3) == 0);
  TASK_CHECK (run_reader (&out, &d, 1) == 0);

  obuf_free (&out);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

static int
case_admission_boundary_and_empty (void)
{
  OGGZ * z;
  struct model m;
  unsigned char buf[8];
  struct pkt_desc full = { 5, 5, 0, 1, 0, 0, 0 };
  struct pkt_desc over = { 5, 1, 0, 0, 0, 1, 1 };
  struct pkt_desc empty = { 5, 0, 0, 0, 0, 2, 2 };
  ogg_packet op;

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);

  TASK_CHECK (oggz_write_set_pending_limit (z, 5) == 0);

  /* Exactly at the limit is admitted. */
  TASK_CHECK (feed_desc (z, &m, &full, buf, NULL) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 5);

  /* One more byte would overflow. */
  desc_to_ogg (&op, buf, &over);
  TASK_CHECK (oggz_write_feed (z, &op, 5, 0, NULL) == OGGZ_ERR_IO_AGAIN);
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 5);

  /* Zero-byte packets still join the sequence at a full quota. */
  TASK_CHECK (feed_desc (z, &m, &empty, buf, NULL) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 5);

  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

static int
case_admission_lower_limit (void)
{
  OGGZ * z;
  struct model m;
  unsigned char buf[32];
  struct pkt_desc d1 = { 7, 20, 0, 1, 0, 0, 0 };
  struct pkt_desc d2 = { 7, 5, 0, 0, 0, 1, 1 };
  struct pkt_desc d3 = { 7, 1, 0, 0, 0, 2, 2 };
  ogg_packet op3;

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);

  TASK_CHECK (oggz_write_set_pending_limit (z, 25) == 0);
  TASK_CHECK (feed_desc (z, &m, &d1, buf, NULL) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 20);

  /* Cannot lower below the current pending count; old limit retained. */
  TASK_CHECK (oggz_write_set_pending_limit (z, 19) == OGGZ_ERR_INVALID);
  TASK_CHECK (oggz_write_set_pending_limit (z, 0) == 0); /* unlimited ok */
  TASK_CHECK (oggz_write_set_pending_limit (z, 25) == 0); /* back to 25 */

  /* 5 more reaches exactly the limit; a further 1 would not. */
  TASK_CHECK (feed_desc (z, &m, &d2, buf, NULL) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 25);

  desc_to_ogg (&op3, buf, &d3);
  TASK_CHECK (oggz_write_feed (z, &op3, 7, 0, NULL) == OGGZ_ERR_IO_AGAIN);
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 25);

  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

/* =====================================================================
 * pages group
 * ===================================================================== */

static int
case_pages_mixed_sizes_byte_at_a_time (void)
{
  OGGZ * z;
  struct model m;
  struct obuf out = {0};
  unsigned char * big;
  /* empty, 255-multiples, exact page fill, and cross-page packets */
  struct pkt_desc d[] = {
    { 101, 0,     0,                  1, 0, 0, 0 },
    { 101, 255,   0,                  0, 0, 1, 1 },
    { 101, 510,   0,                  0, 0, 2, 2 },
    { 101, 65025, 0,                  0, 0, 3, 3 },
    { 101, 70000, OGGZ_FLUSH_AFTER,   0, 1, 4, 4 }
  };
  int nd = (int) (sizeof (d) / sizeof (d[0]));
  int i;

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);

  big = (unsigned char *) malloc (70000);
  TASK_CHECK (big != NULL);
  for (i = 0; i < nd; i++)
    TASK_CHECK (feed_desc (z, &m, &d[i], big, NULL) == 0);

  /* One byte at a time: a partially written header/body or a continuation
   * page finishing no packet must never release pending bytes. */
  TASK_CHECK (drain_output (z, &out, &m, 1) == 0);
  TASK_CHECK (run_reader (&out, d, nd) == 0);

  free (big);
  obuf_free (&out);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

static int
case_pages_last_byte_boundary (void)
{
  OGGZ * z;
  struct model m;
  struct obuf out = {0};
  unsigned char buf[200];
  struct pkt_desc d1 = { 202, 100, OGGZ_FLUSH_AFTER, 1, 1, 0, 0 };
  unsigned char tmp[512];
  long guard_iters = 0;

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);
  TASK_CHECK (feed_desc (z, &m, &d1, buf, NULL) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 100);

  /* Drain one byte at a time until exactly the final page byte remains. */
  for (;;) {
    long remaining = oggz_write_get_next_page_size (z);
    size_t before;
    long n;

    if (remaining == 1) break;
    TASK_CHECK (++guard_iters < 100000);
    TASK_CHECK (remaining >= 0);

    before = out.len;
    n = oggz_write_output (z, tmp, 1);
    if (n == 0) continue; /* first call creates the page; retry for a byte */
    TASK_CHECK (n == 1);
    TASK_CHECK (obuf_append (&out, tmp, 1) == 0);
    TASK_CHECK (model_pump (&m, out.data + before, 1) == 0);

    /* One byte before full page delivery: nothing may have been released. */
    TASK_CHECK (oggz_write_get_pending_bytes (z) == 100);
    TASK_CHECK (m.pending == 100);
  }

  /* The final byte releases the packet. */
  {
    size_t before = out.len;
    long n = oggz_write_output (z, tmp, 1);
    TASK_CHECK (n == 1);
    TASK_CHECK (obuf_append (&out, tmp, 1) == 0);
    TASK_CHECK (model_pump (&m, out.data + before, 1) == 0);
    TASK_CHECK (oggz_write_get_pending_bytes (z) == 0);
    TASK_CHECK (m.pending == 0);
  }

  obuf_free (&out);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

static int
case_pages_packets_share_a_page (void)
{
  OGGZ * z;
  struct model m;
  struct obuf out = {0};
  unsigned char buf[16];
  struct pkt_desc d[5];
  ogg_sync_state sync;
  ogg_page og;
  int pages_with_packets = 0;
  int i;
  long total = 0;
  char * sb;

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);

  for (i = 0; i < 5; i++) {
    d[i].serial = 303;
    d[i].len = 10;
    d[i].flush = (i == 4) ? OGGZ_FLUSH_AFTER : 0;
    d[i].b_o_s = (i == 0);
    d[i].e_o_s = (i == 4);
    d[i].granule = i;
    d[i].packetno = i;
    TASK_CHECK (feed_desc (z, &m, &d[i], buf, NULL) == 0);
  }
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 50);

  TASK_CHECK (drain_output (z, &out, &m, 64) == 0);

  /* Independently confirm several packets end on the same page and were
   * released together rather than one at a time. */
  TASK_CHECK (ogg_sync_init (&sync) == 0);
  sb = ogg_sync_buffer (&sync, (long) out.len);
  memcpy (sb, out.data, out.len);
  TASK_CHECK (ogg_sync_wrote (&sync, (long) out.len) == 0);
  while (ogg_sync_pageseek (&sync, &og) > 0) {
    if (ogg_page_packets (&og) > 0) pages_with_packets++;
    total += ogg_page_packets (&og);
  }
  ogg_sync_clear (&sync);
  TASK_CHECK (total == 5);
  TASK_CHECK (pages_with_packets >= 1 && pages_with_packets < 5);

  TASK_CHECK (run_reader (&out, d, 5) == 0);

  obuf_free (&out);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

static int
case_pages_both_output_paths_agree (void)
{
  OGGZ * z;
  struct model m;
  struct obuf out = {0}, ref = {0};
  unsigned char * big;
  struct pkt_desc d[] = {
    { 404, 300,   0,                1, 0, 0, 0 },
    { 404, 70000, 0,                0, 0, 1, 1 },
    { 404, 17,    OGGZ_FLUSH_AFTER, 0, 1, 2, 2 }
  };
  int nd = (int) (sizeof (d) / sizeof (d[0]));
  int i;

  TASK_CHECK (reference_output (d, nd, &ref) == 0);

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);
  big = (unsigned char *) malloc (70000);
  TASK_CHECK (big != NULL);
  for (i = 0; i < nd; i++)
    TASK_CHECK (feed_desc (z, &m, &d[i], big, NULL) == 0);

  /* oggz_write_output and oggz_write keep the same ledger. */
  TASK_CHECK (drain_output (z, &out, &m, 4096) == 0);
  TASK_CHECK (out.len == ref.len);
  TASK_CHECK (memcmp (out.data, ref.data, out.len) == 0);
  TASK_CHECK (run_reader (&out, d, nd) == 0);

  free (big);
  obuf_free (&out);
  obuf_free (&ref);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

/* =====================================================================
 * resume group
 * ===================================================================== */

static struct pkt_desc resume_descs[] = {
  { 505, 100,  0,                1, 0, 0, 0 },
  { 505, 4096, 0,                0, 0, 1, 1 },
  { 505, 333,  0,                0, 0, 2, 2 },
  { 505, 200,  OGGZ_FLUSH_AFTER, 0, 1, 3, 3 }
};
#define RESUME_ND ((int)(sizeof(resume_descs)/sizeof(resume_descs[0])))

static int
feed_resume_set (OGGZ * z, struct model * m, unsigned char * big)
{
  int i;
  for (i = 0; i < RESUME_ND; i++)
    TASK_CHECK (feed_desc (z, m, &resume_descs[i], big, NULL) == 0);
  return 0;
}

static int
case_resume_pause_zero_and_eagain (void)
{
  OGGZ * z;
  struct model m;
  struct obuf out = {0}, ref = {0};
  struct sink sink;
  unsigned char * big;
  long n;

  TASK_CHECK (reference_output (resume_descs, RESUME_ND, &ref) == 0);

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);
  big = (unsigned char *) malloc (4096);
  TASK_CHECK (big != NULL);
  TASK_CHECK (feed_resume_set (z, &m, big) == 0);

  memset (&sink, 0, sizeof (sink));
  sink.out = &out;
  TASK_CHECK (oggz_io_set_write (z, sink_cb, &sink) == 0);

  /* A 0 return means pause: no progress, IO_AGAIN, pending untouched. */
  sink.mode = SINK_ZERO;
  n = oggz_write (z, 1024);
  TASK_CHECK (n == OGGZ_ERR_IO_AGAIN);
  TASK_CHECK (out.len == 0);
  TASK_CHECK (model_check (z, &m) == 0);

  /* (size_t)-1 + EAGAIN means exactly the same. */
  sink.mode = SINK_EAGAIN;
  n = oggz_write (z, 1024);
  TASK_CHECK (n == OGGZ_ERR_IO_AGAIN);
  TASK_CHECK (out.len == 0);
  TASK_CHECK (model_check (z, &m) == 0);

  /* Recovery resumes from the first unwritten byte. */
  sink.mode = SINK_FULL;
  TASK_CHECK (drain_write (z, &sink, &m) == 0);
  TASK_CHECK (out.len == ref.len);
  TASK_CHECK (memcmp (out.data, ref.data, out.len) == 0);
  TASK_CHECK (run_reader (&out, resume_descs, RESUME_ND) == 0);

  free (big);
  obuf_free (&out);
  obuf_free (&ref);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

static int
case_resume_short_write (void)
{
  OGGZ * z;
  struct model m;
  struct obuf out = {0}, ref = {0};
  struct sink sink;
  unsigned char * big;

  TASK_CHECK (reference_output (resume_descs, RESUME_ND, &ref) == 0);

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);
  big = (unsigned char *) malloc (4096);
  TASK_CHECK (big != NULL);
  TASK_CHECK (feed_resume_set (z, &m, big) == 0);

  memset (&sink, 0, sizeof (sink));
  sink.out = &out;
  sink.mode = SINK_SHORT;
  sink.short_n = 1;
  TASK_CHECK (oggz_io_set_write (z, sink_cb, &sink) == 0);

  /* The callback accepts only one byte per invocation. A busy loop would
   * re-invoke it hundreds of times within a single call; instead each short
   * write ends the call (the only extra invocation is a fresh page reached
   * because a one-byte page remainder was fully written). */
  while (out.len < ref.len) {
    size_t before = out.len;
    long n;
    sink.invocations_in_call = 0;
    n = oggz_write (z, 1024);
    TASK_CHECK (n > 0);
    TASK_CHECK (sink.invocations_in_call >= 1);
    TASK_CHECK (sink.invocations_in_call <= 2);
    TASK_CHECK (model_pump (&m, out.data + before, n) == 0);
    TASK_CHECK (model_check (z, &m) == 0);
  }
  TASK_CHECK (oggz_write (z, 1024) == 0);
  TASK_CHECK (out.len == ref.len);
  TASK_CHECK (memcmp (out.data, ref.data, out.len) == 0);
  TASK_CHECK (run_reader (&out, resume_descs, RESUME_ND) == 0);

  free (big);
  obuf_free (&out);
  obuf_free (&ref);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

static int
case_resume_hard_error (void)
{
  OGGZ * z;
  struct model m;
  struct obuf out = {0}, ref = {0};
  struct sink sink;
  unsigned char * big;
  long n;

  TASK_CHECK (reference_output (resume_descs, RESUME_ND, &ref) == 0);

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);
  big = (unsigned char *) malloc (4096);
  TASK_CHECK (big != NULL);
  TASK_CHECK (feed_resume_set (z, &m, big) == 0);

  memset (&sink, 0, sizeof (sink));
  sink.out = &out;
  sink.mode = SINK_HARD;
  sink.hard_fail_left = 1;
  TASK_CHECK (oggz_io_set_write (z, sink_cb, &sink) == 0);

  /* Failure before any progress: reported immediately, once. */
  n = oggz_write (z, 1024);
  TASK_CHECK (n == OGGZ_ERR_SYSTEM);
  TASK_CHECK (out.len == 0);

  /* Recover and complete. */
  sink.mode = SINK_FULL;
  TASK_CHECK (drain_write (z, &sink, &m) == 0);
  TASK_CHECK (out.len == ref.len);
  TASK_CHECK (memcmp (out.data, ref.data, out.len) == 0);

  /* Now progress first, failure second within one call: bytes return, the
   * error is deferred and reported exactly once on the next call. */
  obuf_free (&out);
  TASK_CHECK (oggz_close (z) == 0);

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);
  TASK_CHECK (feed_resume_set (z, &m, big) == 0);
  memset (&sink, 0, sizeof (sink));
  sink.out = &out;
  sink.mode = SINK_FAIL_SECOND;
  TASK_CHECK (oggz_io_set_write (z, sink_cb, &sink) == 0);

  n = oggz_write (z, 1024);
  TASK_CHECK (n > 0);
  TASK_CHECK (out.len == (size_t) n);
  TASK_CHECK (model_pump (&m, out.data, n) == 0);

  n = oggz_write (z, 1024);
  TASK_CHECK (n == OGGZ_ERR_SYSTEM);
  /* Reported once: a retry now gets a fresh chance once the sink recovers. */
  sink.mode = SINK_FULL;
  TASK_CHECK (drain_write (z, &sink, &m) == 0);
  TASK_CHECK (out.len == ref.len);
  TASK_CHECK (memcmp (out.data, ref.data, out.len) == 0);
  TASK_CHECK (run_reader (&out, resume_descs, RESUME_ND) == 0);

  free (big);
  obuf_free (&out);
  obuf_free (&ref);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

static int
case_resume_drain_and_retry_same_packet (void)
{
  OGGZ * z;
  struct model m;
  struct obuf out = {0};
  unsigned char buf[64];
  struct pkt_desc d1 = { 606, 60, OGGZ_FLUSH_AFTER, 1, 0, 0, 0 };
  struct pkt_desc d2 = { 606, 60, OGGZ_FLUSH_AFTER, 0, 0, 1, 1 };
  struct pkt_desc d3 = { 606, 60, OGGZ_FLUSH_AFTER, 0, 1, 2, 2 };
  unsigned char tmp[1];
  long guard_iters = 0;

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);
  TASK_CHECK (oggz_write_set_pending_limit (z, 100) == 0);

  TASK_CHECK (feed_desc (z, &m, &d1, buf, NULL) == 0);

  /* A second packet would overflow the quota; it is rejected and retried
   * later with the exact same ogg_packet fields. */
  {
    ogg_packet op2;
    desc_to_ogg (&op2, buf, &d2);
    TASK_CHECK (oggz_write_feed (z, &op2, 606, OGGZ_FLUSH_AFTER, NULL)
                == OGGZ_ERR_IO_AGAIN);
  }
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 60);

  /* Drain page one a byte at a time: no release until its final byte. */
  while (oggz_write_get_next_page_size (z) != 1) {
    size_t before = out.len;
    long n = oggz_write_output (z, tmp, 1);
    TASK_CHECK (++guard_iters < 100000);
    TASK_CHECK (n == 1);
    TASK_CHECK (obuf_append (&out, tmp, 1) == 0);
    TASK_CHECK (model_pump (&m, out.data + before, 1) == 0);
    TASK_CHECK (oggz_write_get_pending_bytes (z) == 60);
  }
  {
    size_t before = out.len;
    long n = oggz_write_output (z, tmp, 1);
    TASK_CHECK (n == 1);
    TASK_CHECK (obuf_append (&out, tmp, 1) == 0);
    TASK_CHECK (model_pump (&m, out.data + before, 1) == 0);
  }
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 0);

  /* The previously rejected packet now goes in; the third still overflows
   * until the second one drains. */
  TASK_CHECK (feed_desc (z, &m, &d2, buf, NULL) == 0);
  {
    ogg_packet op3;
    desc_to_ogg (&op3, buf, &d3);
    TASK_CHECK (oggz_write_feed (z, &op3, 606, OGGZ_FLUSH_AFTER, NULL)
                == OGGZ_ERR_IO_AGAIN);
  }
  TASK_CHECK (drain_output (z, &out, &m, 33) == 0);
  TASK_CHECK (feed_desc (z, &m, &d3, buf, NULL) == 0);
  TASK_CHECK (drain_output (z, &out, &m, 33) == 0);

  {
    struct pkt_desc all[3] = { d1, d2, d3 };
    TASK_CHECK (run_reader (&out, all, 3) == 0);
  }

  obuf_free (&out);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

/* hungry callback feeding guarded packets under a limit */
struct hungry_ctx {
  OGGZ * z;
  struct model * m;
  unsigned char * big;
  int guards[4];
  int calls;
  int stop_once;
};

static int
hungry_cb (OGGZ * z, int empty, void * user_data)
{
  struct hungry_ctx * hc = (struct hungry_ctx *) user_data;
  static const struct pkt_desc d[4] = {
    { 707, 140000, 0,                 1, 0, 0, 0 },
    { 707, 10,     0,                 0, 0, 1, 1 },
    { 707, 10,     0,                 0, 0, 2, 2 },
    { 707, 10,     OGGZ_FLUSH_AFTER,  0, 1, 3, 3 }
  };

  (void) empty;

  /* Querying the quota is allowed inside the hungry callback. */
  TASK_CHECK (oggz_write_get_pending_bytes (z) >= 0);

  /* Recursive writes remain forbidden. */
  {
    unsigned char scratch[8];
    TASK_CHECK (oggz_write (z, 8) == OGGZ_ERR_RECURSIVE_WRITE);
    TASK_CHECK (oggz_write_output (z, scratch, sizeof (scratch))
                == OGGZ_ERR_RECURSIVE_WRITE);
  }

  if (hc->stop_once) {
    hc->stop_once = 0;
    return OGGZ_STOP_OK;
  }

  if (hc->calls >= 4) return 0;

  TASK_CHECK (feed_desc (z, hc->m, &d[hc->calls], hc->big,
                         &hc->guards[hc->calls]) == 0);
  hc->calls++;
  return 0;
}

static int
case_resume_hungry_guard_and_stop (void)
{
  OGGZ * z;
  struct model m;
  struct obuf out = {0};
  struct hungry_ctx hc;
  struct sink sink;

  /* Part A: a guard marks when libogg has absorbed the packet memory (the
   * packet struct is freed when the next packet is made), not when the
   * packet's final page is delivered. */
  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);
  {
    unsigned char * big = (unsigned char *) malloc (70000);
    unsigned char tailbuf[8];
    struct pkt_desc p1 = { 708, 70000, 0, 1, 0, 0, 0 };
    struct pkt_desc p2 = { 708, 5, OGGZ_FLUSH_AFTER, 0, 1, 1, 1 };
    int guard = 0;
    long guard_iters = 0;

    TASK_CHECK (big != NULL);
    fill_packet (big, p1.serial, p1.packetno, p1.len);
    fill_packet (tailbuf, p2.serial, p2.packetno, p2.len);
    TASK_CHECK (feed_desc (z, &m, &p1, big, &guard) == 0);
    {
      ogg_packet op2;
      desc_to_ogg (&op2, tailbuf, &p2);
      TASK_CHECK (oggz_write_feed (z, &op2, p2.serial, OGGZ_FLUSH_AFTER,
                                   NULL) == 0);
      TASK_CHECK (model_admit (&m, p2.serial, p2.len) == 0);
      TASK_CHECK (model_check (z, &m) == 0);
    }
    TASK_CHECK (guard == 0);
    TASK_CHECK (oggz_write_get_pending_bytes (z) == 70005);

    /* Drive output until p1's memory is absorbed by libogg and its struct
     * is freed (the guard marks). The 70000-byte packet spans many pages,
     * so when its guard marks its ending page is still incomplete: every
     * admitted byte is still pending. The guard is thus not a page-commit
     * notification. */
    while (guard == 0) {
      unsigned char chunk[257];
      size_t before = out.len;
      long n = oggz_write_output (z, chunk, sizeof (chunk));
      TASK_CHECK (++guard_iters < 100000);
      if (n == 0) continue;
      TASK_CHECK (obuf_append (&out, chunk, (size_t) n) == 0);
      TASK_CHECK (model_pump (&m, out.data + before, n) == 0);
    }
    TASK_CHECK (guard == 1);
    TASK_CHECK (oggz_write_get_pending_bytes (z) == 70005);
    TASK_CHECK (model_check (z, &m) == 0);

    TASK_CHECK (drain_output (z, &out, &m, 257) == 0);
    TASK_CHECK (oggz_write_get_pending_bytes (z) == 0);
    {
      struct pkt_desc both[2] = { p1, p2 };
      TASK_CHECK (run_reader (&out, both, 2) == 0);
    }
    free (big);
  }
  obuf_free (&out);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);

  /* Part B: hungry feeding under a limit, with STOP_OK and recursion guard. */
  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);
  TASK_CHECK (oggz_write_set_pending_limit (z, 200000) == 0);

  memset (&hc, 0, sizeof (hc));
  hc.z = z;
  hc.m = &m;
  hc.big = (unsigned char *) malloc (140000);
  TASK_CHECK (hc.big != NULL);
  hc.stop_once = 1;

  TASK_CHECK (oggz_write_set_hungry_callback (z, hungry_cb, 0, &hc) == 0);

  memset (&sink, 0, sizeof (sink));
  sink.out = &out;
  sink.mode = SINK_FULL;
  TASK_CHECK (oggz_io_set_write (z, sink_cb, &sink) == 0);

  /* First write is stopped by OGGZ_STOP_OK; no packet is consumed. */
  TASK_CHECK (oggz_write (z, 1024) == OGGZ_ERR_STOP_OK);
  TASK_CHECK (hc.calls == 0);

  /* Afterwards the hungry callback feeds the whole set to completion. */
  TASK_CHECK (drain_write (z, &sink, &m) == 0);
  TASK_CHECK (hc.calls == 4);
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 0);
  TASK_CHECK (hc.guards[0] == 1 && hc.guards[1] == 1 &&
              hc.guards[2] == 1 && hc.guards[3] == 1);

  {
    static const struct pkt_desc expect[4] = {
      { 707, 140000, 0,                 1, 0, 0, 0 },
      { 707, 10,     0,                 0, 0, 1, 1 },
      { 707, 10,     0,                 0, 0, 2, 2 },
      { 707, 10,     OGGZ_FLUSH_AFTER,  0, 1, 3, 3 }
    };
    TASK_CHECK (run_reader (&out, expect, 4) == 0);
  }

  free (hc.big);
  obuf_free (&out);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

/* =====================================================================
 * streams group
 * ===================================================================== */

static int
case_streams_dual_interleave (void)
{
  OGGZ * z;
  struct model m;
  struct obuf out = {0};
  unsigned char * big;
  struct pkt_desc d[8];
  int i;

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);
  TASK_CHECK (oggz_write_set_pending_limit (z, 300000) == 0);

  big = (unsigned char *) malloc (140000);
  TASK_CHECK (big != NULL);

  /* Both BOS packets first, before any non-bos page is produced. */
  d[0] = (struct pkt_desc){ 801, 50,     0, 1, 0, 0, 0 };
  d[1] = (struct pkt_desc){ 802, 60,     0, 1, 0, 0, 0 };
  d[2] = (struct pkt_desc){ 801, 140000, 0, 0, 0, 1, 1 };
  d[3] = (struct pkt_desc){ 802, 777,    0, 0, 0, 1, 1 };
  d[4] = (struct pkt_desc){ 801, 255,    0, 0, 0, 2, 2 };
  d[5] = (struct pkt_desc){ 802, 0,      0, 0, 0, 2, 2 };
  d[6] = (struct pkt_desc){ 801, 9, OGGZ_FLUSH_AFTER, 0, 1, 3, 3 };
  d[7] = (struct pkt_desc){ 802, 11, OGGZ_FLUSH_AFTER, 0, 1, 3, 3 };

  for (i = 0; i < 8; i++)
    TASK_CHECK (feed_desc (z, &m, &d[i], big, NULL) == 0);

  /* Interleaved chunking so pages of both streams alternate in flight. */
  TASK_CHECK (drain_output (z, &out, &m, 137) == 0);
  TASK_CHECK (run_reader (&out, d, 8) == 0);

  free (big);
  obuf_free (&out);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

static int
case_streams_flush_and_eos (void)
{
  OGGZ * z;
  struct model m;
  struct obuf out = {0};
  unsigned char buf[32];
  struct pkt_desc d[] = {
    { 901, 5, OGGZ_FLUSH_AFTER,                          1, 0, 0, 0 },
    { 901, 10, 0,                                       0, 0, 1, 1 },
    { 901, 10, OGGZ_FLUSH_BEFORE,                       0, 0, 2, 2 },
    { 901, 7, OGGZ_FLUSH_BEFORE | OGGZ_FLUSH_AFTER,     0, 1, 3, 3 }
  };
  int nd = (int) (sizeof (d) / sizeof (d[0]));
  ogg_sync_state sync;
  ogg_page og;
  int page_packets[16];
  int npages = 0;
  long total_ends = 0;
  char * sb;
  int i;

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);
  for (i = 0; i < nd; i++)
    TASK_CHECK (feed_desc (z, &m, &d[i], buf, NULL) == 0);
  TASK_CHECK (drain_output (z, &out, &m, 251) == 0);

  TASK_CHECK (ogg_sync_init (&sync) == 0);
  sb = ogg_sync_buffer (&sync, (long) out.len);
  memcpy (sb, out.data, out.len);
  TASK_CHECK (ogg_sync_wrote (&sync, (long) out.len) == 0);
  {
    int last_eos = 0;
    while (ogg_sync_pageseek (&sync, &og) > 0) {
      TASK_CHECK (npages < 16);
      page_packets[npages] = ogg_page_packets (&og);
      total_ends += page_packets[npages];
      last_eos = ogg_page_eos (&og);
      npages++;
    }
    ogg_sync_clear (&sync);

    /* Four packets, each isolated by flushing, end on four distinct pages. */
    TASK_CHECK (total_ends == 4);
    for (i = 0; i < npages; i++) TASK_CHECK (page_packets[i] == 1);

    /* The final page carries EOS. */
    TASK_CHECK (last_eos != 0);
  }

  TASK_CHECK (run_reader (&out, d, nd) == 0);

  obuf_free (&out);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

static int
case_streams_close_with_pending (void)
{
  OGGZ * z;
  struct model m;
  unsigned char buf[16];
  struct pkt_desc d1 = { 909, 12, 0, 1, 0, 0, 0 };
  struct pkt_desc d2 = { 910, 13, 0, 1, 0, 0, 0 };

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);
  TASK_CHECK (oggz_write_set_pending_limit (z, 100) == 0);

  /* Admitted but never drained: close must release the ledgers itself. */
  TASK_CHECK (feed_desc (z, &m, &d1, buf, NULL) == 0);
  TASK_CHECK (feed_desc (z, &m, &d2, buf, NULL) == 0);
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 25);
  TASK_CHECK (oggz_close (z) == 0);

  model_clear (&m);
  return 0;
}

static int
case_streams_unlimited_roundtrip (void)
{
  OGGZ * z;
  struct model m;
  struct obuf out = {0};
  unsigned char * big;
  struct pkt_desc d[12];
  int i, k = 0;

  z = oggz_new (OGGZ_WRITE);
  TASK_CHECK (z != NULL);
  TASK_CHECK (model_init (&m) == 0);
  /* No limit is ever set: default unlimited behavior. */
  TASK_CHECK (oggz_write_get_pending_bytes (z) == 0);

  big = (unsigned char *) malloc (5000);
  TASK_CHECK (big != NULL);

  d[k++] = (struct pkt_desc){ 1001, 1, 0, 1, 0, 0, 0 };
  d[k++] = (struct pkt_desc){ 1002, 2, 0, 1, 0, 0, 0 };
  for (i = 0; i < 4; i++) {
    d[k++] = (struct pkt_desc){ 1001, 1000 + i, 0, 0, 0, i + 1, i + 1 };
    d[k++] = (struct pkt_desc){ 1002, 255 * (i + 1), 0, 0, 0, i + 1, i + 1 };
  }
  d[k++] = (struct pkt_desc){ 1001, 3, OGGZ_FLUSH_AFTER, 0, 1, 5, 5 };
  d[k++] = (struct pkt_desc){ 1002, 0, OGGZ_FLUSH_AFTER, 0, 1, 5, 5 };

  for (i = 0; i < k; i++)
    TASK_CHECK (feed_desc (z, &m, &d[i], big, NULL) == 0);

  TASK_CHECK (drain_output (z, &out, &m, 1024) == 0);
  TASK_CHECK (run_reader (&out, d, k) == 0);

  free (big);
  obuf_free (&out);
  model_clear (&m);
  TASK_CHECK (oggz_close (z) == 0);
  return 0;
}

/* ---------------- registration ---------------- */

const struct task_case task_cases[] = {
  { "admission", "validation",            case_admission_validation },
  { "admission", "capacity_retry",        case_admission_capacity_retry },
  { "admission", "boundary_and_empty",    case_admission_boundary_and_empty },
  { "admission", "lower_limit_rejected",  case_admission_lower_limit },

  { "pages", "mixed_sizes_byte_at_a_time", case_pages_mixed_sizes_byte_at_a_time },
  { "pages", "last_byte_boundary",          case_pages_last_byte_boundary },
  { "pages", "packets_share_a_page",        case_pages_packets_share_a_page },
  { "pages", "both_output_paths_agree",     case_pages_both_output_paths_agree },

  { "resume", "pause_zero_and_eagain",  case_resume_pause_zero_and_eagain },
  { "resume", "short_write_no_loop",    case_resume_short_write },
  { "resume", "hard_error_deferred",     case_resume_hard_error },
  { "resume", "drain_and_retry",         case_resume_drain_and_retry_same_packet },
  { "resume", "hungry_guard_stop",       case_resume_hungry_guard_and_stop },

  { "streams", "dual_interleave",      case_streams_dual_interleave },
  { "streams", "flush_before_after",   case_streams_flush_and_eos },
  { "streams", "close_with_pending",   case_streams_close_with_pending },
  { "streams", "unlimited_roundtrip",  case_streams_unlimited_roundtrip }
};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
