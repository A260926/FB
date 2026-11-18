/*
   Copyright (C) 2026 Annodex Association

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   - Neither the name of the Annodex Association nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE ASSOCIATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * oggz_packet_seek.c
 *
 * Packet-ordinal seeking for read handles. The first packet seek on a
 * handle scans the whole physical bitstream once, using a private sync
 * state so neither user callbacks nor the reader's buffered state are
 * disturbed, and records one metadata record per page and per packet
 * (payloads are never stored). Later seeks only re-read the page interval
 * needed to reconstruct the target packet and any packets of other
 * tracks that are in flight at that point, plus a small fixed preread.
 */

#include "config.h"

#if OGGZ_CONFIG_READ

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <ogg/ogg.h>

#include "oggz_compat.h"
#include "oggz_private.h"

/* Number of pages buffered ahead once the target completion page has
 * been re-read. The value is fixed and independent of file size. */
#define OGGZ_PKT_SEEK_PREREAD_PAGES 4

/* Read chunk used while scanning the container. */
#define OGGZ_PKT_SCAN_CHUNKSIZE 4096

#define OGGZ_PKT_PAGES_INIT 256
#define OGGZ_PKT_PACKETS_INIT 64

/* A packet boundary observed while walking one page's segment table. */
typedef struct {
  long begin_page;
  long end_page;
} OggzPktBoundary;

/* Per-stream scratch used only while scanning the container. */
typedef struct OggzPktScanStream OggzPktScanStream;
struct OggzPktScanStream {
  long serialno;
  ogg_stream_state os;

  int packet_open;          /* a packet is currently spanning pages */
  long open_begin_page;

  OggzPktBoundary * bounds; /* boundaries, in completion order */
  long n_bounds;
  long bounds_cap;

  OggzPktIndexStream * idx; /* persistent index record for this stream */
};

typedef struct {
  OggzPktScanStream * items;
  long n;
  long cap;
} OggzPktScanStreams;

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

/*
 * The reader pulls bytes with raw read(fileno()) for FILE handles, so
 * the kernel file offset can be ahead of stdio's ftell() position.
 * The index scan therefore records/restores the true kernel offset via
 * lseek() rather than oggz_io_tell().
 */
static oggz_off_t
oggz_pkt_raw_tell (OGGZ * oggz)
{
  if (oggz->file != NULL) {
#ifdef HAVE_UNISTD_H
    return (oggz_off_t) lseek (fileno (oggz->file), 0, SEEK_CUR);
#else
    return (oggz_off_t) oggz_io_tell (oggz);
#endif
  }
  return (oggz_off_t) oggz_io_tell (oggz);
}

static int
oggz_pkt_raw_seek (OGGZ * oggz, oggz_off_t offset)
{
  if (oggz->file != NULL) {
#ifdef HAVE_UNISTD_H
    if (lseek (fileno (oggz->file), (off_t) offset, SEEK_SET) == (off_t) -1)
      return -1;
    return 0;
#else
    return oggz_io_seek (oggz, (long) offset, SEEK_SET);
#endif
  }
  return oggz_io_seek (oggz, (long) offset, SEEK_SET);
}

static OggzPktIndexStream *
oggz_pkt_find_index_stream (OggzPacketIndex * idx, long serialno)
{
  long i;
  for (i = 0; i < idx->n_streams; i++) {
    if (idx->streams[i].serialno == serialno)
      return &idx->streams[i];
  }
  return NULL;
}

static OggzPktScanStream *
oggz_pkt_find_scan_stream (OggzPktScanStreams * ss, long serialno)
{
  long i;
  for (i = 0; i < ss->n; i++) {
    if (ss->items[i].serialno == serialno)
      return &ss->items[i];
  }
  return NULL;
}

/* First index page whose byte offset is at or greater than offset. */
static long
oggz_pkt_page_at_or_after (OggzPacketIndex * idx, oggz_off_t offset)
{
  long lo = 0, hi = idx->n_pages;
  while (lo < hi) {
    long mid = lo + (hi - lo) / 2;
    if (idx->pages[mid].offset < offset)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

/* Number of packets of st completed before page index barrier. */
static long
oggz_pkt_packets_before_page (OggzPktIndexStream * st, long barrier)
{
  long lo = 0, hi = st->n_packets;
  while (lo < hi) {
    long mid = lo + (hi - lo) / 2;
    if (st->packets[mid].end_page < barrier)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

/* Index of the first packet of st completing on or after page index p. */
static long
oggz_pkt_first_completing_at_or_after (OggzPktIndexStream * st, long p)
{
  return oggz_pkt_packets_before_page (st, p);
}

/*
 * Find a packet of st that is open at page index p, i.e. whose begin
 * page is at or before p while its end page is at or after p. Returns
 * its index, or -1 when no packet is open there.
 */
static long
oggz_pkt_open_packet_at (OggzPktIndexStream * st, long p)
{
  long k = oggz_pkt_first_completing_at_or_after (st, p);
  if (k < st->n_packets && st->packets[k].begin_page <= p)
    return k;
  return -1;
}

static int
oggz_pkt_append_page (OggzPacketIndex * idx, const OggzPktIndexPage * rec)
{
  if (idx->n_pages == idx->pages_cap) {
    long cap = idx->pages_cap ? idx->pages_cap * 2 : OGGZ_PKT_PAGES_INIT;
    OggzPktIndexPage * p;
    if (cap <= idx->n_pages) return -1;
    p = (OggzPktIndexPage *)
      oggz_realloc (idx->pages, (size_t)cap * sizeof (*p));
    if (p == NULL) return -1;
    idx->pages = p;
    idx->pages_cap = cap;
  }
  idx->pages[idx->n_pages++] = *rec;
  return 0;
}

static int
oggz_pkt_append_packet (OggzPktIndexStream * st,
                        const OggzPktIndexPacket * rec)
{
  if (st->n_packets == st->packets_cap) {
    long cap = st->packets_cap ? st->packets_cap * 2 : OGGZ_PKT_PACKETS_INIT;
    OggzPktIndexPacket * p;
    if (cap <= st->n_packets) return -1;
    p = (OggzPktIndexPacket *)
      oggz_realloc (st->packets, (size_t)cap * sizeof (*p));
    if (p == NULL) return -1;
    st->packets = p;
    st->packets_cap = cap;
  }
  st->packets[st->n_packets++] = *rec;
  return 0;
}

static int
oggz_pkt_append_boundary (OggzPktScanStream * s,
                          const OggzPktBoundary * rec)
{
  if (s->n_bounds == s->bounds_cap) {
    long cap = s->bounds_cap ? s->bounds_cap * 2 : 16;
    OggzPktBoundary * p;
    if (cap <= s->n_bounds) return -1;
    p = (OggzPktBoundary *)
      oggz_realloc (s->bounds, (size_t)cap * sizeof (*p));
    if (p == NULL) return -1;
    s->bounds = p;
    s->bounds_cap = cap;
  }
  s->bounds[s->n_bounds++] = *rec;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Full container scan                                                */
/* ------------------------------------------------------------------ */

static OggzPktScanStream *
oggz_pkt_scan_lookup (OggzPktScanStreams * ss,
                      OggzPacketIndex * idx, long serialno)
{
  OggzPktScanStream * s = oggz_pkt_find_scan_stream (ss, serialno);
  OggzPktIndexStream * st;

  if (s != NULL) return s;

  if (ss->n == ss->cap) {
    long cap = ss->cap ? ss->cap * 2 : 8;
    OggzPktScanStream * p;
    if (cap <= ss->n) return NULL;
    p = (OggzPktScanStream *)
      oggz_realloc (ss->items, (size_t)cap * sizeof (*p));
    if (p == NULL) return NULL;
    ss->items = p;
    ss->cap = cap;
  }

  if (idx->n_streams == idx->streams_cap) {
    long cap = idx->streams_cap ? idx->streams_cap * 2 : 8;
    OggzPktIndexStream * p;
    if (cap <= idx->n_streams) return NULL;
    p = (OggzPktIndexStream *)
      oggz_realloc (idx->streams, (size_t)cap * sizeof (*p));
    if (p == NULL) return NULL;
    idx->streams = p;
    idx->streams_cap = cap;
  }

  st = &idx->streams[idx->n_streams];
  memset (st, 0, sizeof (*st));
  st->serialno = serialno;
  st->cursor = 0;
  st->replay_index = -1;
  idx->n_streams++;

  s = &ss->items[ss->n];
  memset (s, 0, sizeof (*s));
  s->serialno = serialno;
  s->idx = st;
  if (ogg_stream_init (&s->os, (int)serialno) != 0)
    return NULL;
  ss->n++;

  return s;
}

/*
 * Walk the segment table of one page and record packet boundaries.
 * A lacing value of 255 continues the current packet; any smaller value
 * (including 0, which ends an empty packet) completes it.
 */
static int
oggz_pkt_scan_segments (OggzPktScanStream * s, const ogg_page * og,
                        long page_index)
{
  int nsegs = og->header[26];
  int seg;

  for (seg = 0; seg < nsegs; seg++) {
    int val = og->header[27 + seg];

    if (!s->packet_open) {
      s->open_begin_page = page_index;
      s->packet_open = 1;
    }

    if (val < 255) {
      OggzPktBoundary b;
      b.begin_page = s->open_begin_page;
      b.end_page = page_index;
      if (oggz_pkt_append_boundary (s, &b) != 0) return -1;
      s->packet_open = 0;
    }
  }

  return 0;
}

static int
oggz_pkt_scan_drain_packets (OggzPktScanStream * s)
{
  ogg_packet op;
  int r;
  long drained = 0;

  while ((r = ogg_stream_packetout (&s->os, &op)) != 0) {
    OggzPktIndexPacket rec;
    OggzPktBoundary * b;

    if (r < 0) {
      /* Hole in page sequence numbers; out of scope for the supported
       * immutable, page-continuous containers. */
      return -1;
    }

    /* Each completed packet must have a matching boundary record. */
    if (drained >= s->n_bounds)
      return -1;

    b = &s->bounds[drained];

    rec.serialno = s->serialno;
    rec.packetno = op.packetno;
    rec.bytes = op.bytes;
    rec.b_o_s = op.b_o_s;
    rec.e_o_s = op.e_o_s;
    rec.granulepos = op.granulepos;
    rec.begin_page = b->begin_page;
    rec.end_page = b->end_page;

    if (oggz_pkt_append_packet (s->idx, &rec) != 0) return -1;

    drained++;
  }

  /* Boundary records are consumed one-for-one with packetouts; any
   * remaining ones describe packets still spanning later pages. */
  if (drained > 0) {
    memmove (s->bounds, s->bounds + drained,
             (size_t)(s->n_bounds - drained) * sizeof (*s->bounds));
    s->n_bounds -= drained;
  }

  return 0;
}

static void
oggz_pkt_scan_free (OggzPktScanStreams * ss)
{
  long i;
  for (i = 0; i < ss->n; i++) {
    ogg_stream_clear (&ss->items[i].os);
    oggz_free (ss->items[i].bounds);
  }
  oggz_free (ss->items);
  ss->items = NULL;
  ss->n = ss->cap = 0;
}

/*
 * Scan the entire physical bitstream. Called at most once per handle.
 * The reader's sync state, stream states and buffered packets are never
 * touched; only the underlying IO position moves, and it is restored.
 */
static int
oggz_pkt_index_build (OGGZ * oggz)
{
  OggzPacketIndex * idx;
  OggzPktScanStreams scan;
  ogg_sync_state scan_sync;
  ogg_page og;
  oggz_off_t running = 0;
  oggz_off_t saved_io_offset;
  oggz_off_t saved_oggz_offset;

  idx = (OggzPacketIndex *) oggz_malloc (sizeof (OggzPacketIndex));
  if (idx == NULL) return -1;
  memset (idx, 0, sizeof (*idx));
  idx->resume_offset = -1;

  memset (&scan, 0, sizeof (scan));

  saved_io_offset = oggz_pkt_raw_tell (oggz);
  saved_oggz_offset = oggz->offset;
  if (saved_io_offset < 0) goto fail;

  if (oggz_pkt_raw_seek (oggz, 0) != 0) goto fail;
  if (ogg_sync_init (&scan_sync) != 0) goto fail;

  for (;;) {
    char * buffer;
    long bytes_read;
    long more;

    buffer = ogg_sync_buffer (&scan_sync, OGGZ_PKT_SCAN_CHUNKSIZE);
    if (buffer == NULL) goto fail_clear_sync;

    bytes_read = (long) oggz_io_read (oggz, buffer, OGGZ_PKT_SCAN_CHUNKSIZE);
    if (bytes_read < 0) goto fail_clear_sync;
    if (bytes_read == 0) break;

    if (ogg_sync_wrote (&scan_sync, bytes_read) != 0)
      goto fail_clear_sync;

    while ((more = ogg_sync_pageseek (&scan_sync, &og)) != 0) {
      OggzPktIndexPage page_rec;
      OggzPktScanStream * s;
      long serialno;

      if (more < 0) {
        /* Gap or resync loss; not expected for supported containers. */
        goto fail_clear_sync;
      }

      serialno = ogg_page_serialno (&og);
      s = oggz_pkt_scan_lookup (&scan, idx, serialno);
      if (s == NULL) goto fail_clear_sync;

      memset (&page_rec, 0, sizeof (page_rec));
      page_rec.offset = running;
      page_rec.page_bytes = more;
      page_rec.serialno = serialno;
      page_rec.nsegs = og.header[26];
      page_rec.continued = ogg_page_continued (&og);

      if (oggz_pkt_scan_segments (s, &og, idx->n_pages) != 0)
        goto fail_clear_sync;

      if (ogg_stream_pagein (&s->os, &og) != 0)
        goto fail_clear_sync;

      if (oggz_pkt_scan_drain_packets (s) != 0)
        goto fail_clear_sync;

      if (oggz_pkt_append_page (idx, &page_rec) != 0)
        goto fail_clear_sync;

      running += more;
    }
  }

  ogg_sync_clear (&scan_sync);
  oggz_pkt_scan_free (&scan);

  /* Restore the underlying IO position and the reader's bookkeeping. */
  if (oggz_pkt_raw_seek (oggz, saved_io_offset) != 0) {
    long i;
    for (i = 0; i < idx->n_streams; i++)
      oggz_free (idx->streams[i].packets);
    oggz_free (idx->streams);
    oggz_free (idx->pages);
    oggz_free (idx);
    return -1;
  }
  oggz->offset = saved_oggz_offset;

  oggz->packet_index = idx;
  return 0;

fail_clear_sync:
  ogg_sync_clear (&scan_sync);
  oggz_pkt_scan_free (&scan);
fail:
  if (saved_io_offset >= 0)
    (void) oggz_pkt_raw_seek (oggz, saved_io_offset);
  oggz->offset = saved_oggz_offset;
  {
    long i;
    for (i = 0; i < idx->n_streams; i++)
      oggz_free (idx->streams[i].packets);
    oggz_free (idx->streams);
    oggz_free (idx->pages);
    oggz_free (idx);
  }
  return -1;
}

/* ------------------------------------------------------------------ */
/* Cursor initialization after the first scan                         */
/* ------------------------------------------------------------------ */

static int
oggz_pkt_reset_stream_cb (void * data)
{
  oggz_stream_t * stream = (oggz_stream_t *) data;
  if (stream->ogg_stream.serialno != -1)
    ogg_stream_reset (&stream->ogg_stream);
  return 0;
}

/*
 * Determine every stream's next-to-deliver ordinal from the live reader
 * when it has never been reset, or from page offsets after a byte or
 * time seek (which resets the libogg stream packet counters).
 */
static void
oggz_pkt_index_init_cursors (OGGZ * oggz)
{
  OggzPacketIndex * idx = oggz->packet_index;
  long i;

  for (i = 0; i < idx->n_streams; i++) {
    OggzPktIndexStream * st = &idx->streams[i];
    oggz_stream_t * live = oggz_get_stream (oggz, st->serialno);

    st->cursor = 0;
    st->replay_index = 0;

    if (oggz->pkt_seek_state_valid && live != NULL) {
      /* Pure sequential reading: packetno counts packets already
       * pulled out of libogg, i.e. delivered to the user. */
      st->cursor = live->packetno + 1;
      if (st->cursor < 0) st->cursor = 0;
      if (st->cursor > st->n_packets) st->cursor = st->n_packets;
    } else {
      long page_index =
        oggz_pkt_page_at_or_after (idx, oggz->offset);
      st->cursor = oggz_pkt_packets_before_page (st, page_index);
    }
  }
}

/* ------------------------------------------------------------------ */
/* Arming a seek                                                      */
/* ------------------------------------------------------------------ */

static int
oggz_pkt_seek_arm (OGGZ * oggz, OggzPktIndexStream * target_stream,
                   const OggzPktIndexPacket * target)
{
  OggzReader * reader = &oggz->x.reader;
  OggzPacketIndex * idx = oggz->packet_index;
  long resume_page = target->begin_page;
  long disarm_page;
  long i;

  /* The resume anchor must be no later than the begin page of every
   * packet that is open at the current anchor, otherwise libogg would
   * meet an orphaned continuation page and silently drop it. Moving the
   * anchor earlier can expose yet more open packets (and even earlier
   * begin pages), so iterate to a fixed point. The result starts at the
   * target's own begin page and only moves backwards. */
  for (;;) {
    int moved = 0;
    for (i = 0; i < idx->n_streams; i++) {
      OggzPktIndexStream * st = &idx->streams[i];
      long k = oggz_pkt_open_packet_at (st, resume_page);
      if (k >= 0 && st->packets[k].begin_page < resume_page) {
        resume_page = st->packets[k].begin_page;
        moved = 1;
      }
    }
    if (!moved) break;
  }

  idx->saved_offset = oggz->offset;
  idx->saved_current_serialno = reader->current_serialno;
  oggz->pkt_seek_state_valid = 0;
  idx->resume_page = resume_page;
  idx->resume_offset = idx->pages[resume_page].offset;
  idx->next_page_index = resume_page;
  idx->target_packetno = target->packetno;
  idx->target_end_page = target->end_page;

  disarm_page = target->end_page + OGGZ_PKT_SEEK_PREREAD_PAGES;
  if (disarm_page >= idx->n_pages) disarm_page = idx->n_pages - 1;
  idx->disarm_after_page = disarm_page;

  /* Pre-seed cursors: every packet strictly before the target in the
   * delivery order counts as consumed. The target stream stops at the
   * target ordinal; other streams stop at their packets completing
   * before the target's completion page. */
  for (i = 0; i < idx->n_streams; i++) {
    OggzPktIndexStream * st = &idx->streams[i];
    if (st->serialno == target_stream->serialno) {
      st->cursor = target->packetno;
    } else {
      st->cursor = oggz_pkt_packets_before_page (st, target->end_page);
    }
    st->replay_index =
      oggz_pkt_first_completing_at_or_after (st, resume_page);
  }

  /* Discard AUTO reverse-buffer entries; replay recreates them. */
  if (!oggz_dlist_is_empty (oggz->packet_buffer)) {
    oggz_dlist_deliter (oggz->packet_buffer, oggz_read_free_pbuffers);
  }

  /* Reset all libogg stream state and the sync engine, then position
   * the underlying IO at the resume anchor. */
  oggz_vector_foreach (oggz->streams, oggz_pkt_reset_stream_cb);
  ogg_sync_reset (&reader->ogg_sync);

  reader->current_serialno = -1;
  reader->current_page_bytes = 0;
  reader->current_packet_pages = 1;
  reader->current_packet_begin_page_offset = idx->resume_offset;
  reader->current_packet_begin_segment_index = 0;
  reader->current_unit = -1;
  reader->current_granulepos = -1;

  if (oggz_pkt_raw_seek (oggz, idx->resume_offset) != 0) {
    oggz->offset = idx->saved_offset;
    reader->current_serialno = idx->saved_current_serialno;
    return -1;
  }
  oggz->offset = idx->resume_offset;

  /* A stale STOP_OK latch must not suppress the first post-seek read. */
  oggz->cb_next = OGGZ_CONTINUE;

  idx->armed = 1;
  idx->replay_active = 1;
  oggz->pkt_seek_state_valid = 0;

  return 0;
}

/* ------------------------------------------------------------------ */
/* Hooks called from oggz_read.c                                      */
/* ------------------------------------------------------------------ */

/*
 * Called for every packet pulled out of a libogg stream state in
 * oggz_read_sync(). Returns 1 when the packet must be suppressed
 * because it is delivered before the seek target, 0 when it belongs to
 * the user-visible delivery sequence. The target itself is the first
 * packet delivered. Either way, the libogg packet number (which restarts
 * at zero whenever the stream state was reset mid-file) is rewritten to
 * the packet's true ordinal from the index.
 */
int
oggz_pkt_seek_suppress_packet (OGGZ * oggz, long serialno, ogg_packet * op)
{
  OggzPacketIndex * idx = oggz->packet_index;
  OggzPktIndexStream * st;
  OggzPktIndexPacket * rec;
  oggz_stream_t * stream;
  int suppress;

  if (idx == NULL) return 0;

  st = oggz_pkt_find_index_stream (idx, serialno);
  if (st == NULL) return 0;

  /* No successful seek has started a replay (e.g. the index was built
   * by a later-rejected call), or this stream has replayed every
   * indexed packet: ordinary sequential semantics apply and the libogg
   * packet number is left untouched. */
  if (!idx->replay_active ||
      st->replay_index < 0 || st->replay_index >= st->n_packets) {
    st->cursor = op->packetno + 1;
    return 0;
  }

  rec = &st->packets[st->replay_index];

  /* Suppress every packet strictly preceding the target in delivery
   * order. Pages have a single serialno, so packets on the target's
   * completion page are the target stream's ordinals in order: those
   * below the target are suppressed, the target and later packets are
   * delivered. */
  if (idx->armed) {
    if (rec->end_page < idx->target_end_page)
      suppress = 1;
    else if (rec->end_page == idx->target_end_page)
      suppress = (rec->packetno < idx->target_packetno);
    else
      suppress = 0;
  } else {
    suppress = 0;
  }

  /* Restore the original ordinal: libogg restarts its counter whenever
   * the stream state is reset for a mid-container resume. */
  op->packetno = rec->packetno;
  stream = oggz_get_stream (oggz, serialno);
  if (stream != NULL) stream->packetno = rec->packetno;

  if (!suppress) {
    /* Only packets actually delivered to the user advance the cursor. */
    st->cursor = rec->packetno + 1;
  }

  st->replay_index++;
  return suppress;
}

/*
 * Called whenever the reader creates a stream state for a page. During
 * a replay the stream can first appear mid-container; resetting it puts
 * libogg's expected page number at -1, so the first page is not
 * mistaken for a sequence gap and no synthetic hole marker is queued.
 */
void
oggz_pkt_seek_new_stream (OGGZ * oggz, long serialno)
{
  OggzPacketIndex * idx = oggz->packet_index;
  oggz_stream_t * stream;

  if (idx == NULL || !idx->replay_active) return;

  stream = oggz_get_stream (oggz, serialno);
  if (stream != NULL && stream->ogg_stream.serialno != -1)
    ogg_stream_reset (&stream->ogg_stream);
}

/*
 * Called for every page accepted by the reader's sync engine. While
 * armed, pages arrive in index order starting at the resume anchor;
 * once the target completion page plus the fixed preread has been
 * ingested, normal sequential reading takes over again.
 */
void
oggz_pkt_seek_note_page (OGGZ * oggz, const ogg_page * og)
{
  OggzPacketIndex * idx = oggz->packet_index;
  (void) og;

  if (idx == NULL || !idx->armed) return;

  if (idx->next_page_index >= idx->disarm_after_page) {
    /* End the suppression window. The ordinal rewrite stays active
     * until each stream has replayed all its indexed packets, since
     * libogg's own counter restarts from zero at the resume anchor. */
    idx->armed = 0;
  }

  idx->next_page_index++;
}

/*
 * Byte seeks, time seeks and purge invalidate any armed replay.
 */
void
oggz_pkt_seek_note_reset (OGGZ * oggz)
{
  OggzPacketIndex * idx = oggz->packet_index;
  if (idx == NULL) return;
  idx->armed = 0;
  idx->replay_active = 0;
}

/*
 * Whether a libogg "hole in data" notification should be swallowed.
 * Starting the replay mid-container means each stream's first page has
 * a sequence number newer than the reset stream state, so libogg emits
 * exactly one synthetic hole marker per stream. The supported containers
 * have continuous page sequences by contract, hence during a replay
 * every such marker is the synthetic one.
 */
int
oggz_pkt_seek_ignore_hole (OGGZ * oggz)
{
  OggzPacketIndex * idx = oggz->packet_index;
  return idx != NULL && idx->replay_active;
}

void
oggz_pkt_index_destroy (OGGZ * oggz)
{
  OggzPacketIndex * idx;
  long i;

  if (oggz == NULL) return;
  idx = oggz->packet_index;
  if (idx == NULL) return;

  for (i = 0; i < idx->n_streams; i++)
    oggz_free (idx->streams[i].packets);
  oggz_free (idx->streams);
  oggz_free (idx->pages);
  oggz_free (idx);
  oggz->packet_index = NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

static int
oggz_pkt_io_seekable (OGGZ * oggz)
{
  if (oggz->file != NULL) return 1;
  if (oggz->io != NULL && oggz->io->seek != NULL &&
      oggz->io->tell != NULL)
    return 1;
  return 0;
}

long
oggz_seek_packets (OGGZ * oggz, long serialno, long packets, int whence)
{
  OggzPacketIndex * idx;
  OggzPktIndexStream * st;
  ogg_int64_t target;

  if (oggz == NULL) return -1;
  if (oggz->flags & OGGZ_WRITE) return -1;
  if (!oggz_pkt_io_seekable (oggz)) return -1;

  switch (whence) {
  case SEEK_SET:
  case SEEK_CUR:
  case SEEK_END:
    break;
  default:
    return -1;
  }

  /* Build the index on first use. A failed build leaves the reader and
   * its buffered data untouched (the scan restores the IO position). */
  if (oggz->packet_index == NULL) {
    if (oggz_pkt_index_build (oggz) != 0) return -1;
    oggz_pkt_index_init_cursors (oggz);
  }

  idx = oggz->packet_index;
  st = oggz_pkt_find_index_stream (idx, serialno);
  if (st == NULL) return -1;
  if (st->n_packets == 0) return -1;

  switch (whence) {
  case SEEK_SET:
    if (packets < 0) return -1;
    target = (ogg_int64_t) packets;
    break;
  case SEEK_CUR:
    /* Guard the signed cursor/offset addition. */
    if (packets > 0 &&
        st->cursor > (ogg_int64_t) LONG_MAX - (ogg_int64_t) packets)
      return -1;
    if (packets < 0 &&
        st->cursor < (ogg_int64_t) LONG_MIN - (ogg_int64_t) packets)
      return -1;
    target = st->cursor + (ogg_int64_t) packets;
    break;
  default: /* SEEK_END */
    if (packets > 0) return -1;
    target = (ogg_int64_t) st->n_packets + (ogg_int64_t) packets;
    break;
  }

  /* The target must name an actual packet of this logical stream. */
  if (target < 0 || target >= st->n_packets) return -1;

  if (oggz_pkt_seek_arm (oggz, st, &st->packets[(long) target]) != 0)
    return -1;

  return (long) target;
}

#else /* !OGGZ_CONFIG_READ */

#include <ogg/ogg.h>
#include "oggz_private.h"

long
oggz_seek_packets (OGGZ * oggz, long serialno, long packets, int whence)
{
  (void) oggz;
  (void) serialno;
  (void) packets;
  (void) whence;
  return OGGZ_ERR_DISABLED;
}

#endif
