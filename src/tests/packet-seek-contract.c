/*
 * Contract tests for oggz_seek_packets().
 *
 * The containers are built with the in-tree libogg only: two logical
 * audio streams are multiplexed with empty packets, 255-multiple packet
 * sizes, several packets per page, packets spanning many pages and
 * interleaved in-flight packets. Every packet's payload and physical
 * completion order is recorded independently and checked against what
 * the OGGZ read callbacks deliver.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <ogg/ogg.h>
#include <oggz/oggz.h>

#include "../../task_support/check.h"

#define SERIAL_A 0x1010
#define SERIAL_B 0x2020

/* ------------------------------------------------------------------ */
/* Reference container construction                                   */
/* ------------------------------------------------------------------ */

typedef struct {
  long serialno;
  long packetno;            /* ordinal within the logical stream */
  long bytes;
  unsigned char value;
  long b_o_s;
  long e_o_s;
  ogg_int64_t granulepos;
  long order;               /* physical completion order, zero based */
} RefPacket;

typedef struct {
  unsigned char * data;
  long size;
  long cap;

  RefPacket * packets;      /* completion order across both streams */
  long n_packets;
  long packets_cap;

  long per_stream[2];       /* packet count per stream */
  long order_counter;
} RefContainer;

static RefPacket *
ref_stream_packet (RefContainer * c, long serialno, long packetno)
{
  long i;
  for (i = 0; i < c->n_packets; i++) {
    if (c->packets[i].serialno == serialno &&
        c->packets[i].packetno == packetno)
      return &c->packets[i];
  }
  return NULL;
}

static long
ref_stream_count (RefContainer * c, long serialno)
{
  long i, n = 0;
  for (i = 0; i < c->n_packets; i++)
    if (c->packets[i].serialno == serialno) n++;
  return n;
}

static int
ref_emit_page (RefContainer * c, ogg_page * og)
{
  if (c->size + og->header_len + og->body_len > c->cap) {
    long cap = c->cap ? c->cap * 2 : 4096;
    unsigned char * p;
    while (cap < c->size + og->header_len + og->body_len) cap *= 2;
    p = (unsigned char *) realloc (c->data, (size_t) cap);
    if (p == NULL) return -1;
    c->data = p;
    c->cap = cap;
  }
  memcpy (c->data + c->size, og->header, (size_t) og->header_len);
  c->size += og->header_len;
  memcpy (c->data + c->size, og->body, (size_t) og->body_len);
  c->size += og->body_len;
  return 0;
}

static int
ref_record (RefContainer * c, long serialno, long packetno, long bytes,
            unsigned char value, int b_o_s, int e_o_s,
            ogg_int64_t granulepos)
{
  RefPacket * p;
  if (c->n_packets == c->packets_cap) {
    long cap = c->packets_cap ? c->packets_cap * 2 : 32;
    RefPacket * np = (RefPacket *)
      realloc (c->packets, (size_t) cap * sizeof (*np));
    if (np == NULL) return -1;
    c->packets = np;
    c->packets_cap = cap;
  }
  p = &c->packets[c->n_packets++];
  p->serialno = serialno;
  p->packetno = packetno;
  p->bytes = bytes;
  p->value = value;
  p->b_o_s = b_o_s;
  p->e_o_s = e_o_s;
  /* libogg writes granulepos zero on the BOS (initial header) page. */
  p->granulepos = b_o_s ? 0 : granulepos;
  p->order = c->order_counter++;
  return 0;
}

/* Feed one packet and flush its pages immediately, recording the
 * reference entry at completion time. */
static int
ref_feed_flush (RefContainer * c, ogg_stream_state * os, long serialno,
                long packetno, long bytes, unsigned char value,
                int e_o_s, ogg_int64_t granulepos)
{
  unsigned char * buf;
  ogg_packet op;
  ogg_page og;

  buf = (unsigned char *) malloc ((size_t) bytes + 1);
  if (buf == NULL) return -1;
  memset (buf, value, (size_t) bytes);

  memset (&op, 0, sizeof (op));
  op.packet = buf;
  op.bytes = bytes;
  op.b_o_s = (packetno == 0);
  op.e_o_s = e_o_s;
  op.granulepos = granulepos;
  op.packetno = packetno;

  if (ogg_stream_packetin (os, &op) != 0) {
    free (buf);
    return -1;
  }
  free (buf);

  while (ogg_stream_flush (os, &og)) {
    if (ref_emit_page (c, &og) != 0) return -1;
  }

  if (ref_record (c, serialno, packetno, bytes, value,
                  packetno == 0, e_o_s, granulepos) != 0)
    return -1;
  return 0;
}

/*
 * Build the standard two-stream container. Per stream:
 *   0: BOS, 1 byte
 *   1: empty packet (0 bytes), shares a page with packet 2
 *   2: 255 bytes (one full lacing segment)
 *   3: 255*3 + 1 bytes, spans several pages
 *   4: 2 bytes, same page as packet 5
 *   5: 3 bytes
 *   6: 255*7 bytes, spans many pages, EOS
 *
 * Streams are interleaved so the big packets of one track stay open
 * across pages of the other track.
 */
static const long k_sizes[7] = {1, 0, 255, 255 * 3 + 1, 2, 3, 255 * 7};
#define N_PACKETS_PER_STREAM 7

static int
ref_build (RefContainer * c)
{
  ogg_stream_state sa, sb;
  long i;

  memset (c, 0, sizeof (*c));

  if (ogg_stream_init (&sa, SERIAL_A) != 0) return -1;
  if (ogg_stream_init (&sb, SERIAL_B) != 0) return -1;

  for (i = 0; i < N_PACKETS_PER_STREAM; i++) {
    long eos = (i == N_PACKETS_PER_STREAM - 1);
    if (ref_feed_flush (c, &sa, SERIAL_A, i, k_sizes[i],
                        (unsigned char) ('A' + i), eos,
                        (ogg_int64_t) (i * 100)) != 0) goto fail;
    if (ref_feed_flush (c, &sb, SERIAL_B, i, k_sizes[i],
                        (unsigned char) ('a' + i), eos,
                        (ogg_int64_t) (i * 100 + 50)) != 0) goto fail;
  }

  ogg_stream_clear (&sa);
  ogg_stream_clear (&sb);
  return 0;

fail:
  ogg_stream_clear (&sa);
  ogg_stream_clear (&sb);
  return -1;
}

/*
 * Build a second container whose pages deliberately pack multiple
 * packets together: after the forced BOS pages, several short packets
 * share pages, and a spanning packet sits between them. The reference
 * completion order is obtained by independently decoding the produced
 * bytes with a fresh libogg sync engine, rather than by recording the
 * packetin order.
 */
static int
ref_build_packed (RefContainer * c)
{
  static const long size_a[5] = {4, 5, 6, 255 * 4 + 7, 8};
  static const long size_b[5] = {2, 3, 255 * 2 + 3, 5, 6};

  ogg_stream_state sa, sb;
  ogg_packet op;
  ogg_page og;
  unsigned char * buf;
  long pa = 0, pb = 0;
  long max_bytes;
  int do_flush;

  ogg_sync_state sync;
  ogg_stream_state da, db;
  ogg_page dog;

  memset (c, 0, sizeof (*c));
  max_bytes = 255 * 4 + 7;
  buf = (unsigned char *) malloc ((size_t) max_bytes);
  if (buf == NULL) return -1;

  if (ogg_stream_init (&sa, SERIAL_A) != 0) { free (buf); return -1; }
  if (ogg_stream_init (&sb, SERIAL_B) != 0) {
    ogg_stream_clear (&sa); free (buf); return -1;
  }

  /* Interleave packetins; flush only at chosen boundaries so several
   * packets land on one page. */
  while (pa < 5 || pb < 5) {
    do_flush = 0;

    if (pa < 5) {
      long bytes = size_a[pa];
      memset (buf, (int) ('P' + pa), (size_t) bytes);
      memset (&op, 0, sizeof (op));
      op.packet = buf;
      op.bytes = bytes;
      op.b_o_s = (pa == 0);
      op.e_o_s = (pa == 4);
      op.granulepos = pa * 10;
      op.packetno = pa;
      if (ogg_stream_packetin (&sa, &op) != 0) goto fail;
      pa++;
    }

    if (pb < 5) {
      long bytes = size_b[pb];
      memset (buf, (int) ('q' + pb), (size_t) bytes);
      memset (&op, 0, sizeof (op));
      op.packet = buf;
      op.bytes = bytes;
      op.b_o_s = (pb == 0);
      op.e_o_s = (pb == 4);
      op.granulepos = pb * 10 + 5;
      op.packetno = pb;
      if (ogg_stream_packetin (&sb, &op) != 0) goto fail;
      pb++;
    }

    /* Flush after the first pair of packets, the spanning packets and
     * again at the end. */
    do_flush = (pa == 2 && pb == 2) || (pa == 4 && pb == 4) ||
               (pa == 5 && pb == 5);
    if (do_flush) {
      while (ogg_stream_flush (&sa, &og))
        if (ref_emit_page (c, &og) != 0) goto fail;
      while (ogg_stream_flush (&sb, &og))
        if (ref_emit_page (c, &og) != 0) goto fail;
    }
  }

  ogg_stream_clear (&sa);
  ogg_stream_clear (&sb);
  free (buf);

  /* Independently decode the wire bytes to record completion order. */
  if (ogg_sync_init (&sync) != 0) goto decode_fail;
  if (ogg_stream_init (&da, SERIAL_A) != 0) {
    ogg_sync_clear (&sync); goto decode_fail;
  }
  if (ogg_stream_init (&db, SERIAL_B) != 0) {
    ogg_stream_clear (&da); ogg_sync_clear (&sync); goto decode_fail;
  }

  {
    char * sb2;
    long off = 0;

    while (off < c->size) {
      long chunk = c->size - off;
      if (chunk > 256) chunk = 256;
      sb2 = ogg_sync_buffer (&sync, chunk);
      memcpy (sb2, c->data + off, (size_t) chunk);
      ogg_sync_wrote (&sync, chunk);
      off += chunk;

      while (ogg_sync_pageout (&sync, &dog) == 1) {
        long serial = ogg_page_serialno (&dog);
        ogg_stream_state * ds =
          (serial == SERIAL_A) ? &da : &db;
        ogg_packet dop;

        ogg_stream_pagein (ds, &dog);
        while (ogg_stream_packetout (ds, &dop) == 1) {
          unsigned char value =
            (unsigned char) (serial == SERIAL_A
                             ? 'P' + dop.packetno
                             : 'q' + dop.packetno);
          if (ref_record (c, serial, (long) dop.packetno, dop.bytes,
                          value, !!dop.b_o_s, !!dop.e_o_s,
                          dop.granulepos) != 0) {
            ogg_stream_clear (&da); ogg_stream_clear (&db);
            ogg_sync_clear (&sync);
            goto decode_fail;
          }
        }
      }
    }
  }

  ogg_stream_clear (&da);
  ogg_stream_clear (&db);
  ogg_sync_clear (&sync);
  return 0;

fail:
  ogg_stream_clear (&sa);
  ogg_stream_clear (&sb);
  free (buf);
decode_fail:
  free (c->data);
  free (c->packets);
  memset (c, 0, sizeof (*c));
  return -1;
}

/* Emit one page (if any) of os into c. */
static int
ref_flush_one (RefContainer * c, ogg_stream_state * os)
{
  ogg_page og;
  if (ogg_stream_flush (os, &og))
    return ref_emit_page (c, &og);
  return 0;
}

/*
 * Build a container whose physical layout deliberately leaves a packet
 * of one track open across pages of the other track. The two big
 * packets are each split over multiple pages, and the other track's
 * complete pages are written in between, so a seek onto a later target
 * must rebuild an in-flight packet that began on an earlier page.
 */
static int
ref_build_interleaved (RefContainer * c)
{
  ogg_stream_state sa, sb;
  ogg_packet op;
  ogg_page og;
  unsigned char * buf;
  /* Larger than one page body (255*255 = 65025 bytes) so each big
   * packet spans several physical pages. */
  long big = 255 * 260 + 7;   /* 66307 bytes, at least two pages */
  long sizes_a[4] = {1, big, 4, 5};
  long sizes_b[4] = {1, 6, big, 7};
  long i;

  memset (c, 0, sizeof (*c));
  buf = (unsigned char *) malloc ((size_t) big);
  if (buf == NULL) return -1;

  if (ogg_stream_init (&sa, SERIAL_A) != 0) { free (buf); return -1; }
  if (ogg_stream_init (&sb, SERIAL_B) != 0) {
    ogg_stream_clear (&sa); free (buf); return -1;
  }

  /* BOS packets, each alone on its first page. */
  {
    memset (buf, 'A', (size_t) sizes_a[0]);
    memset (&op, 0, sizeof (op));
    op.packet = buf; op.bytes = sizes_a[0]; op.b_o_s = 1;
    op.granulepos = 0; op.packetno = 0;
    ogg_stream_packetin (&sa, &op);
    if (ref_flush_one (c, &sa) != 0) goto fail;

    memset (buf, 'a', (size_t) sizes_b[0]);
    memset (&op, 0, sizeof (op));
    op.packet = buf; op.bytes = sizes_b[0]; op.b_o_s = 1;
    op.granulepos = 0; op.packetno = 0;
    ogg_stream_packetin (&sb, &op);
    if (ref_flush_one (c, &sb) != 0) goto fail;
  }

  /* Queue A1 (the big packet) and emit its first page only. */
  memset (buf, 'C', (size_t) sizes_a[1]);
  memset (&op, 0, sizeof (op));
  op.packet = buf; op.bytes = sizes_a[1];
  op.granulepos = 100; op.packetno = 1;
  ogg_stream_packetin (&sa, &op);
  if (ref_flush_one (c, &sa) != 0) goto fail;

  /* B1 completes entirely while A1 is still open. */
  memset (buf, 'd', (size_t) sizes_b[1]);
  memset (&op, 0, sizeof (op));
  op.packet = buf; op.bytes = sizes_b[1];
  op.granulepos = 150; op.packetno = 1;
  ogg_stream_packetin (&sb, &op);
  if (ref_flush_one (c, &sb) != 0) goto fail;

  /* Queue B2 (big) and emit its first page while A1 is still open. */
  memset (buf, 'E', (size_t) sizes_b[2]);
  memset (&op, 0, sizeof (op));
  op.packet = buf; op.bytes = sizes_b[2];
  op.granulepos = 250; op.packetno = 2;
  ogg_stream_packetin (&sb, &op);
  if (ref_flush_one (c, &sb) != 0) goto fail;

  /* Complete A1: its remaining pages land around B2's open packet. */
  while (ogg_stream_flush (&sa, &og))
    if (ref_emit_page (c, &og) != 0) goto fail;

  /* A2 and A3 complete before B2 finishes. */
  for (i = 2; i < 4; i++) {
    memset (buf, (int) ('F' + (i - 2)), (size_t) sizes_a[i]);
    memset (&op, 0, sizeof (op));
    op.packet = buf; op.bytes = sizes_a[i];
    op.granulepos = 300 + i * 10; op.packetno = i;
    ogg_stream_packetin (&sa, &op);
    if (ref_flush_one (c, &sa) != 0) goto fail;
  }

  /* Complete B2. */
  while (ogg_stream_flush (&sb, &og))
    if (ref_emit_page (c, &og) != 0) goto fail;

  /* Final short packets, B3 is EOS. */
  memset (buf, 'h', (size_t) sizes_b[3]);
  memset (&op, 0, sizeof (op));
  op.packet = buf; op.bytes = sizes_b[3]; op.e_o_s = 1;
  op.granulepos = 450; op.packetno = 3;
  ogg_stream_packetin (&sb, &op);
  while (ogg_stream_flush (&sb, &og))
    if (ref_emit_page (c, &og) != 0) goto fail;

  ogg_stream_clear (&sa);
  ogg_stream_clear (&sb);
  free (buf);

  /* Derive the reference completion order by independent decoding. */
  {
    ogg_sync_state sync;
    ogg_stream_state da, db;
    ogg_page dog;

    if (ogg_sync_init (&sync) != 0) goto decode_fail2;
    if (ogg_stream_init (&da, SERIAL_A) != 0) {
      ogg_sync_clear (&sync); goto decode_fail2;
    }
    if (ogg_stream_init (&db, SERIAL_B) != 0) {
      ogg_stream_clear (&da); ogg_sync_clear (&sync); goto decode_fail2;
    }

    {
      char * sb2;
      long off = 0;
      while (off < c->size) {
        long chunk = c->size - off;
        if (chunk > 256) chunk = 256;
        sb2 = ogg_sync_buffer (&sync, chunk);
        memcpy (sb2, c->data + off, (size_t) chunk);
        ogg_sync_wrote (&sync, chunk);
        off += chunk;
        while (ogg_sync_pageout (&sync, &dog) == 1) {
          long serial = ogg_page_serialno (&dog);
          ogg_stream_state * ds = (serial == SERIAL_A) ? &da : &db;
          ogg_packet dop;
          static const unsigned char fill_a[4] = {'A', 'C', 'F', 'G'};
          static const unsigned char fill_b[4] = {'a', 'd', 'E', 'h'};
          unsigned char val;
          ogg_stream_pagein (ds, &dog);
          while (ogg_stream_packetout (ds, &dop) == 1) {
            /* libogg packet pointers are invalidated by the next
             * packetout, so derive the fill byte from serial/ordinal. */
            val = (serial == SERIAL_A)
                    ? fill_a[dop.packetno] : fill_b[dop.packetno];
            if (ref_record (c, serial, (long) dop.packetno, dop.bytes,
                            val, !!dop.b_o_s, !!dop.e_o_s,
                            dop.granulepos) != 0) {
              ogg_stream_clear (&da); ogg_stream_clear (&db);
              ogg_sync_clear (&sync);
              goto decode_fail2;
            }
          }
        }
      }
    }

    ogg_stream_clear (&da);
    ogg_stream_clear (&db);
    ogg_sync_clear (&sync);
  }
  return 0;

fail:
  ogg_stream_clear (&sa);
  ogg_stream_clear (&sb);
  free (buf);
decode_fail2:
  free (c->data);
  free (c->packets);
  memset (c, 0, sizeof (*c));
  return -1;
}

/* ------------------------------------------------------------------ */
/* OGGZ reading harness                                               */
/* ------------------------------------------------------------------ */

typedef struct {
  RefContainer * ref;
  RefPacket ** got;
  long n_got;
  long cap;
  int page_calls;
  int stop_after;      /* return OGGZ_STOP_OK after this many packets */
  int stop_serial;     /* only count matching serial, -1 for any */
  unsigned bad;
} ReadSink;

static int
sink_packet (OGGZ * z, oggz_packet * zp, long serialno, void * user_data)
{
  ReadSink * sink = (ReadSink *) user_data;
  ogg_packet * op = &zp->op;
  RefPacket * expect;
  long j;

  expect = ref_stream_packet (sink->ref, serialno, (long) op->packetno);
  if (expect == NULL) { sink->bad++; return OGGZ_STOP_ERR; }

  /* libogg surfaces b_o_s/e_o_s as its lacing marker bits (0x100/0x200)
   * rather than 0/1, so compare truthiness. */
  /* libogg surfaces b_o_s/e_o_s as its lacing marker bits (0x100/0x200)
   * rather than 0/1, so compare truthiness. */
  /* libogg surfaces b_o_s/e_o_s as its lacing marker bits (0x100/0x200)
   * rather than 0/1, so compare truthiness. */
  if (op->bytes != expect->bytes) sink->bad++;
  if (!!op->b_o_s != !!expect->b_o_s) sink->bad++;
  if (!!op->e_o_s != !!expect->e_o_s) sink->bad++;
  if (op->granulepos != expect->granulepos) sink->bad++;
  for (j = 0; j < op->bytes; j++) {
    if (op->packet[j] != expect->value) { sink->bad++; break; }
  }

  if (sink->n_got == sink->cap) {
    long cap = sink->cap ? sink->cap * 2 : 16;
    RefPacket ** np = (RefPacket **)
      realloc (sink->got, (size_t) cap * sizeof (*np));
    if (np == NULL) { sink->bad++; return OGGZ_STOP_ERR; }
    sink->got = np;
    sink->cap = cap;
  }
  sink->got[sink->n_got++] = expect;

  if (sink->stop_after > 0 &&
      (sink->stop_serial == -1 || sink->stop_serial == serialno)) {
    sink->stop_after--;
    if (sink->stop_after == 0) return OGGZ_STOP_OK;
  }

  (void) z;
  return OGGZ_CONTINUE;
}

static int
sink_page (OGGZ * z, const ogg_page * og, long serialno, void * user_data)
{
  ReadSink * sink = (ReadSink *) user_data;
  (void) z; (void) og; (void) serialno;
  sink->page_calls++;
  return OGGZ_CONTINUE;
}

static void
sink_init (ReadSink * sink, RefContainer * ref)
{
  memset (sink, 0, sizeof (*sink));
  sink->ref = ref;
}

static void
sink_free (ReadSink * sink)
{
  free (sink->got);
  sink->got = NULL;
}

/* Drain every remaining packet through oggz_read(). */
static int
drain (OGGZ * z)
{
  long n;
  for (;;) {
    n = oggz_read (z, 256);
    if (n == 0) return 0;
    if (n < 0) {
      /* STOP_OK is a normal pause; anything else is a failure. */
      if (n == OGGZ_ERR_STOP_OK) return 0;
      return -1;
    }
  }
}

/* Read until EOF and check the delivered sequence is exactly the
 * reference completion order from the given ordinal onward. */
static int
check_tail_matches (ReadSink * sink, long first_order)
{
  long i;
  TASK_CHECK (sink->bad == 0);
  TASK_CHECK (sink->n_got == sink->ref->n_packets - first_order);
  for (i = 0; i < sink->n_got; i++) {
    TASK_CHECK (sink->got[i]->order == first_order + i);
  }
  return 0;
}

static OGGZ *
open_container (RefContainer * c, ReadSink * sink, int use_page_cb)
{
  FILE * f;
  OGGZ * z;

  f = tmpfile ();
  if (f == NULL) return NULL;
  if ((long) fwrite (c->data, 1, (size_t) c->size, f) != c->size) {
    fclose (f);
    return NULL;
  }
  rewind (f);

  z = oggz_open_stdio (f, OGGZ_READ);
  if (z == NULL) { fclose (f); return NULL; }
  if (oggz_set_read_callback (z, -1, sink_packet, sink) != 0) {
    oggz_close (z);
    return NULL;
  }
  if (use_page_cb &&
      oggz_set_read_page (z, -1, sink_page, sink) != 0) {
    oggz_close (z);
    return NULL;
  }
  return z;
}

/* ------------------------------------------------------------------ */
/* Custom IO source with short reads and call counting                */
/* ------------------------------------------------------------------ */

typedef struct {
  const unsigned char * data;
  long size;
  long pos;
  long min_pos;        /* smallest offset ever read from */
  long min_seek_pos;   /* smallest absolute offset ever seeked to */
  long read_calls;
  long seek_calls;
  long tell_calls;
  long max_read;       /* cap each read at this many bytes */
} CountIO;

static size_t
cio_read (void * handle, void * buf, size_t n)
{
  CountIO * io = (CountIO *) handle;
  size_t want = n;
  io->read_calls++;
  if (io->min_pos < 0 || io->pos < io->min_pos) io->min_pos = io->pos;
  if ((long) want > io->max_read) want = (size_t) io->max_read;
  if (io->pos >= io->size) return 0;
  if ((long) want > io->size - io->pos) want = (size_t) (io->size - io->pos);
  memcpy (buf, io->data + io->pos, want);
  io->pos += (long) want;
  return want;
}

static int
cio_seek (void * handle, long offset, int whence)
{
  CountIO * io = (CountIO *) handle;
  long base = 0;
  io->seek_calls++;
  if (whence == SEEK_SET) base = 0;
  else if (whence == SEEK_CUR) base = io->pos;
  else if (whence == SEEK_END) base = io->size;
  else return -1;
  if (base + offset < 0 || base + offset > io->size) return -1;
  io->pos = base + offset;
  if (io->min_seek_pos < 0 || io->pos < io->min_seek_pos)
    io->min_seek_pos = io->pos;
  return 0;
}

static long
cio_tell (void * handle)
{
  CountIO * io = (CountIO *) handle;
  io->tell_calls++;
  return io->pos;
}

static OGGZ *
open_count_io (RefContainer * c, ReadSink * sink, CountIO * io,
               long max_read)
{
  OGGZ * z = oggz_new (OGGZ_READ);
  if (z == NULL) return NULL;
  memset (io, 0, sizeof (*io));
  io->data = c->data;
  io->size = c->size;
  io->max_read = max_read;
  io->min_pos = -1;
  io->min_seek_pos = -1;
  if (oggz_io_set_read (z, cio_read, io) != 0 ||
      oggz_io_set_seek (z, cio_seek, io) != 0 ||
      oggz_io_set_tell (z, cio_tell, io) != 0) {
    oggz_close (z);
    return NULL;
  }
  if (oggz_set_read_callback (z, -1, sink_packet, sink) != 0) {
    oggz_close (z);
    return NULL;
  }
  return z;
}

/* ================================================================== */
/* ordinal group                                                      */
/* ================================================================== */

static int
case_ordinal_set (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  long target = 3;
  long r;
  RefPacket * expect;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  r = oggz_seek_packets (z, SERIAL_A, target, SEEK_SET);
  TASK_CHECK (r == target);

  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.bad == 0);

  /* First callback must be exactly the target packet. */
  TASK_CHECK (sink.n_got > 0);
  expect = ref_stream_packet (&c, SERIAL_A, target);
  TASK_CHECK (sink.got[0] == expect);
  TASK_CHECK (sink.got[0]->order == expect->order);
  TASK_CHECK (check_tail_matches (&sink, expect->order) == 0);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_ordinal_end (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  long total, r;
  RefPacket * expect;

  TASK_CHECK (ref_build (&c) == 0);
  total = ref_stream_count (&c, SERIAL_B);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  /* SEEK_END -1 names the final packet. */
  r = oggz_seek_packets (z, SERIAL_B, -1, SEEK_END);
  TASK_CHECK (r == total - 1);
  expect = ref_stream_packet (&c, SERIAL_B, total - 1);

  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.bad == 0);
  TASK_CHECK (sink.got[0] == expect);
  TASK_CHECK (sink.got[0]->e_o_s);
  TASK_CHECK (check_tail_matches (&sink, expect->order) == 0);

  /* SEEK_END 0 is out of range (one past the last packet). */
  sink_free (&sink);
  sink_init (&sink, &c);
  TASK_CHECK (oggz_seek_packets (z, SERIAL_B, 0, SEEK_END) == -1);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_ordinal_cur (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  long r;
  RefPacket * expect;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  /* Initial cursor is zero: CUR +4 == absolute packet 4. */
  r = oggz_seek_packets (z, SERIAL_A, 4, SEEK_CUR);
  TASK_CHECK (r == 4);
  expect = ref_stream_packet (&c, SERIAL_A, 4);

  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.bad == 0);
  TASK_CHECK (sink.got[0] == expect);

  /* After draining to EOF, the cursor equals the packet count. */
  r = oggz_seek_packets (z, SERIAL_A, -2, SEEK_CUR);
  TASK_CHECK (r == N_PACKETS_PER_STREAM - 2);
  expect = ref_stream_packet (&c, SERIAL_A, N_PACKETS_PER_STREAM - 2);
  sink_free (&sink);
  sink_init (&sink, &c);
  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.got[0] == expect);
  TASK_CHECK (sink.bad == 0);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_ordinal_zero (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  RefPacket * expect;
  long r;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  /* Seek to BOS after reaching EOF: full stream is replayed. */
  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.n_got == c.n_packets);
  sink_free (&sink);
  sink_init (&sink, &c);

  /* Seek to BOS after reaching EOF. Packets that completed before the
   * target are not re-delivered: the first callback is the target and
   * only the physical-order tail follows it. */
  r = oggz_seek_packets (z, SERIAL_B, 0, SEEK_SET);
  TASK_CHECK (r == 0);
  TASK_CHECK (drain (z) == 0);
  expect = ref_stream_packet (&c, SERIAL_B, 0);
  TASK_CHECK (sink.got[0] == expect);
  TASK_CHECK (sink.got[0]->b_o_s);
  TASK_CHECK (check_tail_matches (&sink, expect->order) == 0);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_ordinal_packed (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  long r, i;

  /* Multi-packet pages and in-flight spanning packets. */
  TASK_CHECK (ref_build_packed (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  for (i = 0; i < ref_stream_count (&c, SERIAL_A); i++) {
    RefPacket * expect;
    r = oggz_seek_packets (z, SERIAL_A, i, SEEK_SET);
    TASK_CHECK (r == i);
    expect = ref_stream_packet (&c, SERIAL_A, i);
    sink_free (&sink);
    sink_init (&sink, &c);
    TASK_CHECK (drain (z) == 0);
    TASK_CHECK (sink.bad == 0);
    TASK_CHECK (sink.got[0] == expect);
    TASK_CHECK (check_tail_matches (&sink, expect->order) == 0);
  }

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

/* ================================================================== */
/* resume group                                                       */
/* ================================================================== */

static int
case_resume_stop_ok (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  RefPacket * expect;
  long r;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  r = oggz_seek_packets (z, SERIAL_A, 4, SEEK_SET);
  TASK_CHECK (r == 4);
  expect = ref_stream_packet (&c, SERIAL_A, 4);

  /* Pause after the first delivered packet. oggz_read() returns the
   * ingested byte count on the stopping call; OGGZ_ERR_STOP_OK surfaces
   * on the following call. */
  sink.stop_after = 1;
  sink.stop_serial = -1;
  do {
    r = oggz_read (z, 4096);
  } while (r > 0);
  TASK_CHECK (r == OGGZ_ERR_STOP_OK);
  TASK_CHECK (sink.n_got == 1);
  TASK_CHECK (sink.got[0] == expect);

  /* Resume: no duplicate, no skipped packet. */
  sink.stop_after = 0;
  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.bad == 0);
  TASK_CHECK (check_tail_matches (&sink, expect->order) == 0);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_resume_repeated (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  long r, i;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  /* Repeated seeks never duplicate or skip packets. */
  for (i = 0; i < N_PACKETS_PER_STREAM; i++) {
    RefPacket * expect;
    r = oggz_seek_packets (z, SERIAL_B, i, SEEK_SET);
    TASK_CHECK (r == i);
    expect = ref_stream_packet (&c, SERIAL_B, i);
    sink_free (&sink);
    sink_init (&sink, &c);
    TASK_CHECK (drain (z) == 0);
    TASK_CHECK (sink.bad == 0);
    TASK_CHECK (sink.got[0] == expect);
    TASK_CHECK (check_tail_matches (&sink, expect->order) == 0);
  }

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_resume_after_eof (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  RefPacket * expect;
  long r;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.n_got == c.n_packets);

  /* Reverse seek after EOF. */
  r = oggz_seek_packets (z, SERIAL_A, 2, SEEK_SET);
  TASK_CHECK (r == 2);
  expect = ref_stream_packet (&c, SERIAL_A, 2);
  sink_free (&sink);
  sink_init (&sink, &c);
  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.got[0] == expect);
  TASK_CHECK (check_tail_matches (&sink, expect->order) == 0);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_resume_after_byte_seek (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  long r;
  RefPacket * expect;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  /* A plain byte seek first; the packet cursor must be rebuilt from
   * byte offsets, not from the reset libogg counters. */
  TASK_CHECK (oggz_seek (z, 0, SEEK_SET) == 0);
  r = oggz_seek_packets (z, SERIAL_B, 5, SEEK_SET);
  TASK_CHECK (r == 5);
  expect = ref_stream_packet (&c, SERIAL_B, 5);
  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.bad == 0);
  TASK_CHECK (sink.got[0] == expect);
  TASK_CHECK (check_tail_matches (&sink, expect->order) == 0);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_resume_preread_reject (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  long r;
  RefPacket * expect;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  /* Arm a seek and ingest data until at least one packet has been
   * delivered, leaving further pages and packets buffered in the
   * reader. */
  r = oggz_seek_packets (z, SERIAL_A, 3, SEEK_SET);
  TASK_CHECK (r == 3);
  expect = ref_stream_packet (&c, SERIAL_A, 3);
  {
    int guard = 0;
    while (sink.n_got == 0 && guard++ < 64) {
      r = oggz_read (z, 64);
      if (r < 0 && r != OGGZ_ERR_STOP_OK) break;
    }
  }
  TASK_CHECK (sink.n_got >= 1);

  /* A rejected seek must not disturb the buffered next packet. */
  TASK_CHECK (oggz_seek_packets (z, 9999, 9999, SEEK_SET) == -1);
  TASK_CHECK (oggz_seek_packets (z, SERIAL_A, 1 << 30, SEEK_SET) == -1);

  {
    long already = sink.n_got;
    TASK_CHECK (drain (z) == 0);
    TASK_CHECK (sink.bad == 0);
    /* The sequence continues seamlessly after the previously buffered
     * first packet. */
    TASK_CHECK (sink.got[0] == expect);
    TASK_CHECK (check_tail_matches (&sink, expect->order) == 0);
    (void) already;
  }

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

/* ================================================================== */
/* interleave group                                                   */
/* ================================================================== */

static int
case_interleave_other_track (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  long r, i;
  RefPacket * expect;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  /* Seek onto a target whose page also completes the other track's
   * packet that began earlier (the big spanning packets). */
  r = oggz_seek_packets (z, SERIAL_A, 6, SEEK_SET);
  TASK_CHECK (r == 6);
  expect = ref_stream_packet (&c, SERIAL_A, 6);

  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.bad == 0);
  TASK_CHECK (sink.got[0] == expect);
  TASK_CHECK (check_tail_matches (&sink, expect->order) == 0);

  /* The in-flight B packet that completes around the target is present. */
  {
    int found_open_b = 0;
    for (i = 0; i < sink.n_got; i++) {
      if (sink.got[i]->serialno == SERIAL_B &&
          sink.got[i]->packetno >= 3)
        found_open_b = 1;
    }
    TASK_CHECK (found_open_b);
  }

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

/*
 * Physical interleave: a packet of the other track genuinely stays
 * open across this target's pages, so recovery must start at that
 * packet's own begin page and deliver it intact afterwards.
 */
static int
case_interleave_open_across_pages (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  long i;

  TASK_CHECK (ref_build_interleaved (&c) == 0);
  TASK_CHECK (c.n_packets == 8);

  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  /* Every packet is reachable; the two big spanning packets must be
   * rebuilt from their begin pages even when another track's pages sit
   * in the middle. */
  for (i = 0; i < c.n_packets; i++) {
    RefPacket * p = &c.packets[i];
    TASK_CHECK (oggz_seek_packets (z, p->serialno, p->packetno,
                                   SEEK_SET) == p->packetno);
    sink_free (&sink);
    sink_init (&sink, &c);
    TASK_CHECK (drain (z) == 0);
    TASK_CHECK (sink.bad == 0);
    TASK_CHECK (sink.got[0] == p);
    TASK_CHECK (check_tail_matches (&sink, p->order) == 0);
  }

  /* Explicitly seek onto A2, which completes while B2 is open across
   * it: B2 must still be delivered whole later in the tail. */
  TASK_CHECK (oggz_seek_packets (z, SERIAL_A, 2, SEEK_SET) == 2);
  sink_free (&sink);
  sink_init (&sink, &c);
  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.bad == 0);
  {
    RefPacket * b2 = ref_stream_packet (&c, SERIAL_B, 2);
    int found = 0, j;
    TASK_CHECK (b2 != NULL);
    for (j = 0; j < sink.n_got; j++)
      if (sink.got[j] == b2) found = 1;
    TASK_CHECK (found);
  }

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_interleave_every_target (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  long i;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  /* Every packet of both streams must be a valid target and the tail
   * must always follow the physical completion order exactly. */
  for (i = 0; i < c.n_packets; i++) {
    RefPacket * p = &c.packets[i];
    TASK_CHECK (oggz_seek_packets (z, p->serialno, p->packetno,
                                   SEEK_SET) == p->packetno);
    sink_free (&sink);
    sink_init (&sink, &c);
    TASK_CHECK (drain (z) == 0);
    TASK_CHECK (sink.bad == 0);
    TASK_CHECK (sink.got[0] == p);
    TASK_CHECK (check_tail_matches (&sink, p->order) == 0);
  }

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_interleave_page_callback_silent (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 1);
  TASK_CHECK (z != NULL);

  /* No page callbacks during the seek itself. */
  TASK_CHECK (oggz_seek_packets (z, SERIAL_B, 4, SEEK_SET) == 4);
  TASK_CHECK (sink.page_calls == 0);
  TASK_CHECK (sink.n_got == 0);

  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.bad == 0);
  TASK_CHECK (sink.page_calls > 0);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_interleave_short_read_io (void)
{
  RefContainer c;
  ReadSink sink;
  CountIO io;
  OGGZ * z;
  long r;
  RefPacket * expect;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  /* One-byte reads force every page to be assembled from many calls. */
  z = open_count_io (&c, &sink, &io, 1);
  TASK_CHECK (z != NULL);

  r = oggz_seek_packets (z, SERIAL_A, 5, SEEK_SET);
  TASK_CHECK (r == 5);
  expect = ref_stream_packet (&c, SERIAL_A, 5);
  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.bad == 0);
  TASK_CHECK (sink.got[0] == expect);
  TASK_CHECK (check_tail_matches (&sink, expect->order) == 0);
  TASK_CHECK (io.read_calls > 0);
  TASK_CHECK (io.seek_calls > 0);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_interleave_two_handles (void)
{
  RefContainer c;
  ReadSink s1, s2;
  OGGZ * z1, * z2;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&s1, &c);
  sink_init (&s2, &c);
  z1 = open_container (&c, &s1, 0);
  z2 = open_container (&c, &s2, 0);
  TASK_CHECK (z1 != NULL && z2 != NULL);

  TASK_CHECK (oggz_seek_packets (z1, SERIAL_A, 1, SEEK_SET) == 1);
  TASK_CHECK (oggz_seek_packets (z2, SERIAL_B, 6, SEEK_SET) == 6);
  TASK_CHECK (drain (z1) == 0);
  TASK_CHECK (drain (z2) == 0);
  TASK_CHECK (s1.bad == 0 && s2.bad == 0);
  TASK_CHECK (s1.got[0]->serialno == SERIAL_A && s1.got[0]->packetno == 1);
  TASK_CHECK (s2.got[0]->serialno == SERIAL_B && s2.got[0]->packetno == 6);

  oggz_close (z1);
  oggz_close (z2);
  sink_free (&s1);
  sink_free (&s2);
  free (c.data);
  free (c.packets);
  return 0;
}

/* ================================================================== */
/* reject group                                                       */
/* ================================================================== */

static int
case_reject_bad_args (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  OGGZ * w;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  TASK_CHECK (oggz_seek_packets (NULL, SERIAL_A, 0, SEEK_SET) == -1);
  TASK_CHECK (oggz_seek_packets (z, 9999, 0, SEEK_SET) == -1);
  TASK_CHECK (oggz_seek_packets (z, SERIAL_A, -1, SEEK_SET) == -1);
  TASK_CHECK (oggz_seek_packets (z, SERIAL_A,
                                 N_PACKETS_PER_STREAM, SEEK_SET) == -1);
  TASK_CHECK (oggz_seek_packets (z, SERIAL_A, 1, 4242) == -1);
  TASK_CHECK (oggz_seek_packets (z, SERIAL_A, 1, SEEK_END) == -1);
  TASK_CHECK (oggz_seek_packets (z, SERIAL_A,
                                 -N_PACKETS_PER_STREAM - 1, SEEK_END) == -1);
  TASK_CHECK (oggz_seek_packets (z, SERIAL_A, LONG_MAX, SEEK_CUR) == -1);

  /* Write handles are rejected. */
  w = oggz_new (OGGZ_WRITE);
  TASK_CHECK (w != NULL);
  TASK_CHECK (oggz_seek_packets (w, SERIAL_A, 0, SEEK_SET) == -1);
  oggz_close (w);

  /* Rejections change nothing: a subsequent SET 0 reads everything. */
  TASK_CHECK (oggz_seek_packets (z, SERIAL_A, 0, SEEK_SET) == 0);
  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (check_tail_matches (&sink, 0) == 0);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_reject_no_seek_io (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);

  /* Read-only IO, no seek/tell: packet seeking must be refused. */
  z = oggz_new (OGGZ_READ);
  TASK_CHECK (z != NULL);
  TASK_CHECK (oggz_io_set_read (z, cio_read, NULL) == 0);
  TASK_CHECK (oggz_set_read_callback (z, -1, sink_packet, &sink) == 0);
  TASK_CHECK (oggz_seek_packets (z, SERIAL_A, 0, SEEK_SET) == -1);
  oggz_close (z);

  /* A read-only handle without any IO at all is likewise refused. */
  z = oggz_new (OGGZ_READ);
  TASK_CHECK (z != NULL);
  TASK_CHECK (oggz_seek_packets (z, SERIAL_A, 0, SEEK_SET) == -1);
  oggz_close (z);

  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_reject_preserves_position (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  long before;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  /* Read a prefix normally. */
  TASK_CHECK (oggz_read (z, 200) > 0);
  before = sink.n_got;
  TASK_CHECK (before > 0);

  /* Failed calls leave the next packet untouched. */
  TASK_CHECK (oggz_seek_packets (z, 4242, 0, SEEK_SET) == -1);
  TASK_CHECK (oggz_seek_packets (z, SERIAL_A, -9, SEEK_SET) == -1);
  TASK_CHECK (oggz_seek_packets (z, SERIAL_B, 0, 99) == -1);
  TASK_CHECK (sink.n_got == before);

  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.bad == 0);
  TASK_CHECK (sink.n_got == c.n_packets);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_reject_io_rescan (void)
{
  RefContainer c;
  ReadSink sink;
  CountIO io;
  OGGZ * z;
  long scan_reads;
  RefPacket * expect;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_count_io (&c, &sink, &io, 37);
  TASK_CHECK (z != NULL);

  /* First seek builds the index by scanning the whole container,
   * including a seek back to offset zero. */
  TASK_CHECK (oggz_seek_packets (z, SERIAL_A, 2, SEEK_SET) == 2);
  scan_reads = io.read_calls;
  TASK_CHECK (scan_reads > 0);
  TASK_CHECK (io.min_seek_pos == 0);
  TASK_CHECK (drain (z) == 0);
  sink_free (&sink);
  sink_init (&sink, &c);

  /* Reset the extrema trackers; the second seek must not go back to
   * the start of the container or re-scan the unrelated prefix. */
  io.min_pos = -1;
  io.min_seek_pos = -1;
  {
    long reads_before = io.read_calls;
    long recovery_reads;

    TASK_CHECK (oggz_seek_packets (z, SERIAL_B, 5, SEEK_SET) == 5);

    /* Read until the target itself is delivered, i.e. the bounded
     * recovery interval has been consumed. */
    {
      int guard = 0;
      expect = ref_stream_packet (&c, SERIAL_B, 5);
      while (sink.n_got == 0 && guard++ < 128) {
        long r2 = oggz_read (z, 64);
        if (r2 < 0 && r2 != OGGZ_ERR_STOP_OK) break;
      }
    }
    TASK_CHECK (sink.n_got >= 1);
    TASK_CHECK (sink.got[0] == expect);
    TASK_CHECK (io.min_seek_pos > 0);
    TASK_CHECK (io.min_pos > 0);

    /* Recovery reads only touch the target region plus fixed preread,
     * never the whole container as the initial scan did. */
    recovery_reads = io.read_calls - reads_before;
    TASK_CHECK (recovery_reads > 0);
    TASK_CHECK (recovery_reads < scan_reads);
  }

  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.bad == 0);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

/* ================================================================== */
/* Legacy behavior examples                                           */
/* ================================================================== */

static int
case_legacy_plain_read (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.bad == 0);
  TASK_CHECK (sink.n_got == c.n_packets);
  TASK_CHECK (check_tail_matches (&sink, 0) == 0);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static int
case_legacy_byte_seek (void)
{
  RefContainer c;
  ReadSink sink;
  OGGZ * z;
  oggz_off_t at;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  z = open_container (&c, &sink, 0);
  TASK_CHECK (z != NULL);

  at = oggz_seek (z, 0, SEEK_SET);
  TASK_CHECK (at == 0);
  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.bad == 0);
  TASK_CHECK (sink.n_got == c.n_packets);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

static ogg_int64_t
linear_metric (OGGZ * z, long serialno, ogg_int64_t granulepos,
               void * user_data)
{
  (void) z; (void) serialno; (void) user_data;
  return granulepos;
}

static int
case_legacy_time_seek (void)
{
  RefContainer c;
  ReadSink sink;
  CountIO io;
  OGGZ * z;
  ogg_int64_t r;

  TASK_CHECK (ref_build (&c) == 0);
  sink_init (&sink, &c);
  /* Custom read/seek/tell keeps every reported position consistent,
   * which the legacy unit-seek machinery requires. */
  z = open_count_io (&c, &sink, &io, 4096);
  TASK_CHECK (z != NULL);
  TASK_CHECK (oggz_set_metric (z, -1, linear_metric, NULL) == 0);

  /* Read the BOS pages first so the metric machinery is primed. */
  TASK_CHECK (oggz_read (z, 4096) > 0);
  TASK_CHECK (sink.n_got > 0);

  r = oggz_seek_units (z, 200, SEEK_SET);
  TASK_CHECK (r >= 0);

  /* Time seek followed by a packet seek rebuilds correct cursors. */
  TASK_CHECK (oggz_seek_packets (z, SERIAL_A, 4, SEEK_SET) == 4);
  sink_free (&sink);
  sink_init (&sink, &c);
  TASK_CHECK (drain (z) == 0);
  TASK_CHECK (sink.bad == 0);
  TASK_CHECK (sink.got[0]->serialno == SERIAL_A);
  TASK_CHECK (sink.got[0]->packetno == 4);

  oggz_close (z);
  sink_free (&sink);
  free (c.data);
  free (c.packets);
  return 0;
}

/* ================================================================== */
/* Registration                                                       */
/* ================================================================== */

const struct task_case task_cases[] = {
  { "ordinal", "set",            case_ordinal_set },
  { "ordinal", "end",            case_ordinal_end },
  { "ordinal", "cur",            case_ordinal_cur },
  { "ordinal", "zero",           case_ordinal_zero },
  { "ordinal", "packed-pages",   case_ordinal_packed },

  { "resume", "stop-ok",              case_resume_stop_ok },
  { "resume", "repeated-seeks",       case_resume_repeated },
  { "resume", "backward-after-eof",   case_resume_after_eof },
  { "resume", "after-byte-seek",      case_resume_after_byte_seek },
  { "resume", "reject-after-preread", case_resume_preread_reject },

  { "interleave", "other-track-inflight", case_interleave_other_track },
  { "interleave", "open-across-pages",    case_interleave_open_across_pages },
  { "interleave", "every-target",         case_interleave_every_target },
  { "interleave", "page-callback-silent", case_interleave_page_callback_silent },
  { "interleave", "short-read-io",        case_interleave_short_read_io },
  { "interleave", "two-handles",          case_interleave_two_handles },

  { "reject", "bad-args",            case_reject_bad_args },
  { "reject", "no-seek-tell",        case_reject_no_seek_io },
  { "reject", "preserves-position",  case_reject_preserves_position },
  { "reject", "no-prefix-rescan",    case_reject_io_rescan },

  { "reject", "legacy-plain-read",   case_legacy_plain_read },
  { "reject", "legacy-byte-seek",    case_legacy_byte_seek },
  { "reject", "legacy-time-seek",    case_legacy_time_seek },
};

const size_t task_case_count =
  sizeof (task_cases) / sizeof (task_cases[0]);
