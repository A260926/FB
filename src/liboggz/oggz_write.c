/*
   Copyright (C) 2003 Commonwealth Scientific and Industrial Research
   Organisation (CSIRO) Australia

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   - Neither the name of CSIRO Australia nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE ORGANISATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "config.h"

#if OGGZ_CONFIG_WRITE

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#include <ogg/ogg.h>

#include "oggz_private.h"
#include "oggz_vector.h"

/* #define DEBUG */

/* Define to 0 or 1 */
#define ALWAYS_FLUSH 0

#define OGGZ_WRITE_EMPTY (-707)

/* #define ZPACKET_CMP */

#ifdef ZPACKET_CMP
static int
oggz_zpacket_cmp (oggz_writer_packet_t * a, oggz_writer_packet_t * b,
		  void * user_data)
{
  OGGZ * oggz = (OGGZ *)user_data;
  long serialno_a, serialno_b;
  ogg_int64_t unit_a, unit_b;

  serialno_a = a->stream->ogg_stream.serialno;
  serialno_b = b->stream->ogg_stream.serialno;

  unit_a = oggz_get_unit (oggz, serialno_a, a->op.granulepos);
  unit_b = oggz_get_unit (oggz, serialno_b, b->op.granulepos);

  if (unit_a < unit_b) return -1;
  else return (unit_a > unit_b);
}
#endif

OGGZ *
oggz_write_init (OGGZ * oggz)
{
  OggzWriter * writer = &oggz->x.writer;

  writer->next_zpacket = NULL;

  writer->packet_queue = oggz_vector_new ();
  if (writer->packet_queue == NULL) return NULL;

#ifdef ZPACKET_CMP
  /* XXX: comparison function should only kick in when a metric is set */
  oggz_vector_set_cmp (writer->packet_queue,
		       (OggzCmpFunc)oggz_zpacket_cmp, oggz);
#endif

  writer->hungry = NULL;
  writer->hungry_user_data = NULL;
  writer->hungry_only_when_empty = 0;

  writer->writing = 0;
  writer->no_more_packets = 0;
  writer->state = OGGZ_MAKING_PACKETS;

  writer->flushing = 0;
#if 0
  writer->eog = 1;
  writer->eop = 1; /* init ready to start next packet */
#endif
  writer->eos = 0;

  writer->current_zpacket = NULL;

  writer->packet_offset = 0;
  writer->page_offset = 0;

  writer->current_stream = NULL;

  writer->pending_bytes = 0;
  writer->pending_limit = 0; /* 0 means unlimited, the default */
  writer->page_accounted = 0;

  writer->io_pending_error = 0;

  return oggz;
}

/******** Back-pressure accounting ********/

/*
 * Pending bytes count the original (unframed) bytes of every packet that has
 * been accepted via oggz_write_feed() but whose ending Ogg page has not yet
 * been delivered in full. Page headers are never counted.
 *
 * Each logical stream keeps its own ordered chain of ledger entries, one per
 * admitted packet, so that interleaved streams cannot disturb each other's
 * accounting. A packet spanning multiple pages stays on the chain until the
 * last page it appears on completes; the packets ending on a page are popped
 * together when that page is delivered in full.
 */

/* Record an admitted packet on its stream's ledger. */
static int
oggz_pending_admit (OggzWriter * writer, oggz_stream_t * stream,
                    ogg_int64_t bytes)
{
  oggz_pending_packet_t * node;

  node = (oggz_pending_packet_t *) oggz_malloc (sizeof (*node));
  if (node == NULL) return -1;

  node->bytes = bytes;
  node->next = NULL;

  if (stream->pending_tail != NULL)
    stream->pending_tail->next = node;
  else
    stream->pending_head = node;
  stream->pending_tail = node;

  writer->pending_bytes += bytes;

  return 0;
}

/* Release every ledger entry still held by a stream (used on close). */
static void
oggz_pending_drain_stream (OggzWriter * writer, oggz_stream_t * stream)
{
  oggz_pending_packet_t * node = stream->pending_head;

  while (node != NULL) {
    oggz_pending_packet_t * next = node->next;
    writer->pending_bytes -= node->bytes;
    oggz_free (node);
    node = next;
  }

  stream->pending_head = NULL;
  stream->pending_tail = NULL;
}

/*
 * A page has just been delivered in full. ogg_page_packets() reports how
 * many packets end on it; pop that many entries from the page's stream and
 * subtract their original byte counts. Continuation pages that finish no
 * packet report 0 and release nothing.
 */
static void
oggz_pending_page_delivered (OGGZ * oggz)
{
  OggzWriter * writer = &oggz->x.writer;
  ogg_page * og = &oggz->current_page;
  oggz_stream_t * stream;
  int npackets, i;

  stream = oggz_get_stream (oggz, (long) ogg_page_serialno (og));
  if (stream == NULL) return;

  npackets = ogg_page_packets (og);

  for (i = 0; i < npackets; i++) {
    oggz_pending_packet_t * node = stream->pending_head;

    if (node == NULL) break;

    stream->pending_head = node->next;
    if (stream->pending_head == NULL) stream->pending_tail = NULL;

    writer->pending_bytes -= node->bytes;
    oggz_free (node);
  }

  if (writer->pending_bytes < 0) writer->pending_bytes = 0;
}

/*
 * Called whenever an output operation reaches the end of the current page.
 * The transition guard makes sure the page is accounted exactly once even
 * if an extra zero-length output of the same page occurs before the next
 * page is initialised.
 */
static void
oggz_pending_note_page_complete (OGGZ * oggz)
{
  OggzWriter * writer = &oggz->x.writer;

  if (writer->page_accounted) return;
  writer->page_accounted = 1;
  oggz_pending_page_delivered (oggz);
}

static int
oggz_writer_packet_free (oggz_writer_packet_t * zpacket)
{
  if (!zpacket) return 0;

  if (zpacket->guard) {
    /* managed by user; flag guard */
    *zpacket->guard = 1;
  } else {
    /* managed by oggz; free copied data */
    oggz_free (zpacket->op.packet);
  }
  oggz_free (zpacket);

  return 0;
}

int
oggz_write_flush (OGGZ * oggz)
{
  OggzWriter * writer = &oggz->x.writer;
  ogg_stream_state * os;
  ogg_page * og;
  int ret = 0;

  os = writer->current_stream;
  og = &oggz->current_page;

  if (os != NULL)
    ret = ogg_stream_flush (os, og);

  if (ret) {
    /* A fresh page now occupies current_page; its bytes have not been
     * delivered yet, so its packets stay pending and it must be accounted
     * exactly once when its final byte is written. */
    writer->page_offset = 0;
    writer->page_accounted = 0;
  }

  return ret;
}

OGGZ *
oggz_write_close (OGGZ * oggz)
{
  OggzWriter * writer = &oggz->x.writer;
  int i, size;

  oggz_write_flush (oggz);

  oggz_writer_packet_free (writer->current_zpacket);
  oggz_writer_packet_free (writer->next_zpacket);

  oggz_vector_foreach (writer->packet_queue,
		       (OggzFunc)oggz_writer_packet_free);
  oggz_vector_delete (writer->packet_queue);

  /* Release any packets still on the per-stream ledgers. */
  size = oggz_vector_size (oggz->streams);
  for (i = 0; i < size; i++) {
    oggz_stream_t * stream =
      (oggz_stream_t *) oggz_vector_nth_p (oggz->streams, i);
    oggz_pending_drain_stream (writer, stream);
  }

  writer->pending_bytes = 0;
  writer->io_pending_error = 0;

  return oggz;
}

/******** Back-pressure limits ********/

int
oggz_write_set_pending_limit (OGGZ * oggz, ogg_int64_t limit)
{
  OggzWriter * writer;

  if (oggz == NULL) return OGGZ_ERR_BAD_OGGZ;

  if (!(oggz->flags & OGGZ_WRITE)) {
    return OGGZ_ERR_INVALID;
  }

  if (limit < 0) return OGGZ_ERR_INVALID;

  writer = &oggz->x.writer;

  /* Refuse to lower the limit below the bytes currently in flight; the
   * previously configured limit is left untouched. */
  if (limit != 0 && limit < writer->pending_bytes) {
    return OGGZ_ERR_INVALID;
  }

  writer->pending_limit = limit;

  return 0;
}

ogg_int64_t
oggz_write_get_pending_bytes (OGGZ * oggz)
{
  OggzWriter * writer;

  if (oggz == NULL) return OGGZ_ERR_BAD_OGGZ;

  if (!(oggz->flags & OGGZ_WRITE)) {
    return OGGZ_ERR_INVALID;
  }

  writer = &oggz->x.writer;

  return writer->pending_bytes;
}

/******** Packet queueing ********/

int
oggz_write_set_hungry_callback (OGGZ * oggz, OggzWriteHungry hungry,
				int only_when_empty, void * user_data)
{
  OggzWriter * writer;

  if (oggz == NULL) return OGGZ_ERR_BAD_OGGZ;

  if (!(oggz->flags & OGGZ_WRITE)) {
    return OGGZ_ERR_INVALID;
  }

  writer = &oggz->x.writer;

  writer->hungry = hungry;
  writer->hungry_user_data = user_data;
  writer->hungry_only_when_empty = only_when_empty;

  return 0;
}

int
oggz_write_feed (OGGZ * oggz, ogg_packet * op, long serialno, int flush,
		 int * guard)
{
  OggzWriter * writer;
  oggz_stream_t * stream;
  oggz_writer_packet_t * packet;
  ogg_packet * new_op;
  unsigned char * new_buf = NULL;
  int b_o_s, e_o_s, bos_auto;
  int strict, prefix, suffix;

#ifdef DEBUG
  printf ("oggz_write_feed: IN\n");
#endif

  if (oggz == NULL) return OGGZ_ERR_BAD_OGGZ;

  if (!(oggz->flags & OGGZ_WRITE)) {
    return OGGZ_ERR_INVALID;
  }

  writer = &oggz->x.writer;

  if (guard && *guard != 0) return OGGZ_ERR_BAD_GUARD;

  /* Check that the serialno is in the valid range for an Ogg page header,
   * ie. that it fits within 32 bits and does not equal the special value -1 */
  if ((long)((ogg_int32_t)serialno) != serialno || serialno == -1) {
#ifdef DEBUG
    printf ("oggz_write_feed: serialno %010lu\n", serialno);
#endif
    return OGGZ_ERR_BAD_SERIALNO;
  }

  /* Back-pressure: refuse to admit the packet while it would exceed the
   * configured pending allowance. This is checked before any stream is
   * created and before any stream state (BOS/EOS, granulepos, packetno) or
   * guard is touched, so the very same packet can be retried unchanged once
   * pending bytes drain or the limit is raised. A limit of 0 means
   * unlimited. Zero-byte packets are counted as zero and so never block. */
  if (writer->pending_limit != 0) {
    ogg_int64_t packet_bytes = (ogg_int64_t) (op->bytes < 0 ? 0 : op->bytes);
    if (writer->pending_bytes + packet_bytes > writer->pending_limit) {
      return OGGZ_ERR_IO_AGAIN;
    }
  }

#ifdef DEBUG
  printf ("oggz_write_feed: (%010lu) FLUSH: %d\n", serialno, flush);
#endif

  /* Cache strict, prefix, suffix */
  strict = !(oggz->flags & OGGZ_NONSTRICT);
  prefix = (oggz->flags & OGGZ_PREFIX);
  suffix = (oggz->flags & OGGZ_SUFFIX);

  /* Set bos, eos to canonical values */
  bos_auto = (op->b_o_s == -1) ? 1 : 0;
  b_o_s = op->b_o_s ? 1 : 0;
  e_o_s = op->e_o_s ? 1 : 0;

  stream = oggz_get_stream (oggz, serialno);
  if (stream == NULL) {
    if (bos_auto) b_o_s = 1;

    if (strict && b_o_s && !oggz_get_bos (oggz, -1)) {
	return OGGZ_ERR_BOS;
    }

    if (b_o_s || !strict || suffix) {
      stream = oggz_add_stream (oggz, serialno);
      if (stream == NULL)
        return OGGZ_ERR_OUT_OF_MEMORY;
      oggz_auto_identify_packet (oggz, op, serialno);
    } else {
      return OGGZ_ERR_BAD_SERIALNO;
    }
  } else {
    if (bos_auto) b_o_s = 0;

    if (!suffix && strict && stream->e_o_s)
      return OGGZ_ERR_EOS;
  }

  /* Check packet against mapping restrictions */
  if (strict) {
    if (op->bytes < 0) return OGGZ_ERR_BAD_BYTES;
    if (!suffix && b_o_s != stream->b_o_s) return OGGZ_ERR_BAD_B_O_S;
    if (op->granulepos != -1 && op->granulepos < stream->granulepos &&
        /* Allow negative granulepos immediately after headers, for Dirac: */
        !(stream->granulepos == 0 && op->granulepos < 0))
      return OGGZ_ERR_BAD_GRANULEPOS;

    /* Allow packetno == -1 to indicate oggz should fill it in; otherwise:
     * if op is the first packet in the stream, initialize the stream's
     * packetno to the given one, otherwise check it for strictness.
     * For stream suffixes, believe the packetno value */
    if (op->packetno != -1) {
      if (b_o_s || suffix) {
	stream->packetno = op->packetno;
      } else if (op->packetno <= stream->packetno) {
	return OGGZ_ERR_BAD_PACKETNO;
      }
    }
  }

  /* OK -- Update stream's memory of packet details */

  if (!stream->metric && (oggz->flags & OGGZ_AUTO)) {
    oggz_auto_read_bos_packet (oggz, op, serialno, NULL);
  }

  stream->b_o_s = 0; /* The stream is henceforth no longer at bos */
  stream->e_o_s = e_o_s; /* We believe the eos value */
  stream->granulepos = op->granulepos; /* and the granulepos */

  /* If the user specified a packetno of -1, fill it in automatically;
   * otherwise, use the user-specified value */
  stream->packetno = (op->packetno != -1) ? op->packetno : stream->packetno+1;

  /* Now set up the packet and add it to the queue */
  if (guard == NULL) {
    new_buf = oggz_malloc ((size_t)op->bytes);
    if (new_buf == NULL) return OGGZ_ERR_OUT_OF_MEMORY;

    memcpy (new_buf, op->packet, (size_t)op->bytes);
  } else {
    new_buf = op->packet;
  }

  packet = oggz_malloc (sizeof (oggz_writer_packet_t));
  if (packet == NULL) {
    if (guard == NULL && new_buf != NULL) oggz_free (new_buf);
    return OGGZ_ERR_OUT_OF_MEMORY;
  }

  new_op = &packet->op;
  new_op->packet = new_buf;
  new_op->bytes = op->bytes;
  new_op->b_o_s = b_o_s;
  new_op->e_o_s = e_o_s;
  new_op->granulepos = op->granulepos;
  new_op->packetno = stream->packetno;

  packet->stream = stream;
  packet->flush = flush;
  packet->guard = guard;

#ifdef DEBUG
  printf ("oggz_write_feed: made packet bos %ld eos %ld (%ld bytes) FLUSH: %d\n",
	  new_op->b_o_s, new_op->e_o_s, new_op->bytes, packet->flush);
#endif

  if (oggz_vector_insert_p (writer->packet_queue, packet) == NULL) {
    oggz_free (packet);
    if (!guard) oggz_free (new_buf);
    return -1;
  }

  /* The packet is now accepted; record its original bytes as pending on the
   * stream's ledger until the page on which it ends is delivered in full. */
  if (oggz_pending_admit (writer, stream, (ogg_int64_t) op->bytes) != 0) {
    oggz_vector_remove_p (writer->packet_queue, packet);
    oggz_free (packet);
    if (!guard) oggz_free (new_buf);
    return OGGZ_ERR_OUT_OF_MEMORY;
  }

  writer->no_more_packets = 0;

#ifdef DEBUG
  printf ("oggz_write_feed: enqueued packet, queue size %d\n",
	  oggz_vector_size (writer->packet_queue));
#endif

  return 0;
}

/******** Page creation ********/

/*
 *

            ____           __________________\___           ____
           /    \         /    Page ready    /   \         /    \
          |      \ /=========\                /=========\ /      |
  Page    |       || Make    ||              ||  Write  ||       | Page
  !ready  V       || packet  ||              ||  page   ||       V ready
          |      / \=========/                \=========/ \      |
           \____/         \___/__________________/         \____/
                              \   Page !ready
 *
 */


/*
 * oggz_page_init (oggz)
 *
 * Initialises the next page of the current packet.
 *
 * If this returns 0, the page is not ready for writing.
 */
static long
oggz_page_init (OGGZ * oggz)
{
  OggzWriter * writer;
  ogg_stream_state * os;
  ogg_page * og;
  int ret;

  if (oggz == NULL) return -1;

  writer = &oggz->x.writer;
  os = writer->current_stream;
  og = &oggz->current_page;

  if (ALWAYS_FLUSH || writer->flushing) {
#ifdef DEBUG
    printf ("oggz_page_init: ATTEMPT FLUSH: ");
#endif
    ret = oggz_write_flush (oggz);
  } else {
#ifdef DEBUG
    printf ("oggz_page_init: ATTEMPT pageout: ");
#endif
    ret = ogg_stream_pageout (os, og);
  }

  if (ret) {
    writer->page_offset = 0;
    writer->page_accounted = 0;
  }

#ifdef DEBUG
  printf ("%s\n", ret ? "OK" : "NO");
#endif

  return ret;
}

/*
 * oggz_packet_init (oggz, buf, n)
 *
 * Initialises the next packet with data from buf, length n
 */
static long
oggz_packet_init (OGGZ * oggz, oggz_writer_packet_t * next_zpacket)
{
  OggzWriter * writer;
  oggz_stream_t * stream;
  ogg_stream_state * os;
  ogg_packet * op;

  if (oggz == NULL) return -1L;

  writer = &oggz->x.writer;
  writer->current_zpacket = next_zpacket;
  op = &next_zpacket->op;

#ifdef DEBUG
  printf ("oggz_packet_init: %ld bytes\n", (long)op->bytes);
#endif

  stream = next_zpacket->stream;

  /* Mark this stream as having delivered a non b_o_s packet if so */
  if (!op->b_o_s) stream->delivered_non_b_o_s = 1;

  os = &stream->ogg_stream;
  ogg_stream_packetin (os, op);

  writer->flushing = (next_zpacket->flush & OGGZ_FLUSH_AFTER);
#ifdef DEBUG
  printf ("oggz_packet_init: set flush to %d\n", writer->flushing);
#endif
  writer->current_stream = os;
  writer->packet_offset = 0;

  return 0;
}

static long
oggz_page_copyout (OGGZ * oggz, unsigned char * buf, long n)
{
  OggzWriter * writer;
  long h, b;
  ogg_page * og;

  if (oggz == NULL) return -1L;

  writer = &oggz->x.writer;
  og = &oggz->current_page;

  h = MIN (n, og->header_len - writer->page_offset);
  if (h > 0) {
    memcpy (buf, og->header + writer->page_offset, h);
    writer->page_offset += h;
    n -= h;
    buf += h;
  } else {
    h = 0;
  }

  b = MIN (n, og->header_len + og->body_len - writer->page_offset);
  if (b > 0) {
#ifdef DEBUG
    {
      unsigned char * c = &og->body[writer->page_offset - og->header_len];
      printf ("oggz_page_copyout [%d] (@%ld): %c%c%c%c ...\n",
	      ogg_page_serialno (og), (long) ogg_page_granulepos (og),
	      c[0], c[1], c[2], c[3]);
    }
#endif
    memcpy (buf, og->body + (writer->page_offset - og->header_len), b);
    writer->page_offset += b;
    n -= b;
    buf += b;
  } else {
    b = 0;
  }

  if (writer->page_offset >= og->header_len + og->body_len) {
    /* The whole page, header and body, has now been copied out. */
    oggz_pending_note_page_complete (oggz);
  }

  return h + b;
}

/*
 * Write the next contiguous chunk (page header remainder or page body
 * remainder) of the current page through the IO callback.
 *
 * At most one callback invocation is made, so a short write never turns
 * into a busy loop. The page offset is advanced only by the bytes the
 * callback actually accepted; the next call resumes at the first unwritten
 * byte.
 *
 * \retval "> 0" the number of bytes accepted by the callback (possibly a
 *         short write)
 * \retval 0 the callback asked to pause: it returned 0, or (size_t)-1 with
 *         errno EAGAIN/EWOULDBLOCK
 * \retval -1 the callback failed: it returned (size_t)-1 with another errno
 */
static long
oggz_page_writeout (OGGZ * oggz, long n)
{
  OggzWriter * writer;
  ogg_page * og;
  unsigned char * ptr;
  long want, nwritten;
  size_t ret;

  writer = &oggz->x.writer;
  og = &oggz->current_page;

  if (writer->page_offset < og->header_len) {
    want = MIN (n, og->header_len - writer->page_offset);
    ptr = og->header + writer->page_offset;
  } else {
    want = MIN (n, og->header_len + og->body_len - writer->page_offset);
    ptr = og->body + (writer->page_offset - og->header_len);
  }

  if (want <= 0) return 0;

  ret = oggz_io_write (oggz, ptr, (size_t) want);

  if (ret == (size_t) 0) return 0;

  if (ret == (size_t) -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
  }

  /* Defensive: a misbehaving callback claiming more than requested must
   * not move the offset past the end of the page. */
  nwritten = (long) ret;
  if (nwritten > want) nwritten = want;

  writer->page_offset += nwritten;

  if (writer->page_offset >= og->header_len + og->body_len) {
    /* The whole page, header and body, has now been delivered. */
    oggz_pending_note_page_complete (oggz);
  }

  return nwritten;
}

static int
oggz_dequeue_packet (OGGZ * oggz, oggz_writer_packet_t ** next_zpacket)
{
  OggzWriter * writer = &oggz->x.writer;
  int ret = 0;

  if (writer->next_zpacket != NULL) {
#ifdef DEBUG
    printf ("oggz_dequeue_packet: queue EMPTY\n");
#endif
    *next_zpacket = writer->next_zpacket;
    writer->next_zpacket = NULL;
  } else {
    *next_zpacket = oggz_vector_pop (writer->packet_queue);

    if (*next_zpacket == NULL) {
      if (writer->hungry) {
        ret = writer->hungry (oggz, 1, writer->hungry_user_data);
        *next_zpacket = oggz_vector_pop (writer->packet_queue);
#ifdef DEBUG
        printf ("oggz_dequeue_packet: called hungry and popped, new queue size %d\n",
  	        oggz_vector_size (writer->packet_queue));
#endif

#ifdef DEBUG
      } else {
        printf ("oggz_dequeue_packet: no packet, no hungry, queue size %d\n",
                oggz_vector_size (writer->packet_queue));
#endif
      }
#ifdef DEBUG
    } else {
    printf ("oggz_dequeue_packet: dequeued packet, queue size %d\n",
            oggz_vector_size (writer->packet_queue));
#endif
    }

  }

#ifdef DEBUG
  printf("oggz_dequeue_packeT: returning %d\n", ret);
#endif
  return ret;
}

static long
oggz_writer_make_packet (OGGZ * oggz)
{
  OggzWriter * writer = &oggz->x.writer;
  oggz_writer_packet_t * zpacket, * next_zpacket = NULL;
  int cb_ret = 0;

#ifdef DEBUG
  printf ("oggz_writer_make_packet: IN\n");
#endif

  /* finished with current packet; unguard */
  zpacket = writer->current_zpacket;
  oggz_writer_packet_free (zpacket);
  writer->current_zpacket = NULL;

  /* if the user wants the hungry callback after every packet, give
   * it to them, marking emptiness appropriately
   */
  if (writer->hungry && !writer->hungry_only_when_empty) {
    int empty = (oggz_vector_size (writer->packet_queue) == 0);
    cb_ret = writer->hungry (oggz, empty, writer->hungry_user_data);
  }

  if (cb_ret == 0) {
    /* dequeue and init the next packet */
    cb_ret = oggz_dequeue_packet (oggz, &next_zpacket);
    if (next_zpacket == NULL) {
#ifdef DEBUG
      printf ("oggz_writer_make_packet: packet queue empty\n");
#endif
      /*writer->eos = 1;*/
    } else {
      if ((writer->current_stream != NULL) &&
	  (next_zpacket->flush & OGGZ_FLUSH_BEFORE)) {
	writer->flushing = 1;
#ifdef DEBUG
	printf ("oggz_writer_make_packet: set flush to %d\n",
		writer->flushing);
#endif
	next_zpacket->flush &= OGGZ_FLUSH_AFTER;
	writer->next_zpacket = next_zpacket;
      } else {
	oggz_packet_init (oggz, next_zpacket);
      }
    }
  }

#ifdef DEBUG
  printf("oggz_writer_make_packet: cb_ret is %d\n", cb_ret);
#endif

  if (cb_ret == 0 && next_zpacket == NULL) return OGGZ_WRITE_EMPTY;

  return cb_ret;
}

long
oggz_write_output (OGGZ * oggz, unsigned char * buf, long n)
{
  OggzWriter * writer;
  long bytes, bytes_written = 1, remaining = n, nwritten = 0;
  int active = 1, cb_ret = 0;

  if (oggz == NULL) return OGGZ_ERR_BAD_OGGZ;

  writer = &oggz->x.writer;

  if (!(oggz->flags & OGGZ_WRITE)) {
    return OGGZ_ERR_INVALID;
  }

  if (writer->writing) return OGGZ_ERR_RECURSIVE_WRITE;
  writer->writing = 1;

#ifdef DEBUG
  printf ("oggz_write_output: IN\n");
#endif

  /* A write-callback failure deferred from a previous oggz_write() call is
   * reported once before any further output. */
  if (writer->io_pending_error) {
    int err = writer->io_pending_error;
    writer->io_pending_error = 0;
    writer->writing = 0;
    return err;
  }

  if ((cb_ret = oggz->cb_next) != OGGZ_CONTINUE) {
    oggz->cb_next = 0;
    writer->writing = 0;
    writer->no_more_packets = 0;
    if (cb_ret == OGGZ_WRITE_EMPTY) cb_ret = 0;
    return oggz_map_return_value_to_error (cb_ret);
  }

  while (active && remaining > 0) {
    bytes = MIN (remaining, 1024);

#ifdef DEBUG
    printf ("oggz_write_output: write loop (%ld , %ld remain) ...\n", bytes,
	    remaining);
#endif

    while (writer->state == OGGZ_MAKING_PACKETS) {
#ifdef DEBUG
      	printf ("oggz_write_output: MAKING_PACKETS\n");
#endif
      if ((cb_ret = oggz_writer_make_packet (oggz)) != OGGZ_CONTINUE) {
#ifdef DEBUG
        printf ("oggz_write_output: no packets (cb_ret is %d)\n", cb_ret);
#endif
        if (cb_ret == OGGZ_WRITE_EMPTY) {
          writer->flushing = 1;
          writer->no_more_packets = 1;
        }
        /* At this point, in contrast to oggz_write(), we break out of this
         * loop unconditionally.
         */
        active = 0;
        break;
      }
      if (oggz_page_init (oggz)) {
        writer->state = OGGZ_WRITING_PAGES;
      } else {
#ifdef DEBUG
        printf ("oggz_write_output: unable to make page...\n");
#endif
        if (writer->no_more_packets) {
          active = 0;
          break;
        }
      }
    }

    if (writer->state == OGGZ_WRITING_PAGES) {
      bytes_written = oggz_page_copyout (oggz, buf, bytes);

      if (bytes_written == -1) {
        active = 0;
        cb_ret = OGGZ_ERR_SYSTEM; /* XXX: catch next */
      } else if (bytes_written == 0) {
        if (writer->no_more_packets) {
          active = 0;
          break;
        } else if (!oggz_page_init (oggz)) {
#ifdef DEBUG
          printf ("oggz_write_output: bytes_written == 0, DONE\n");
#endif
          writer->state = OGGZ_MAKING_PACKETS;
        }
      }

      buf += bytes_written;

      remaining -= bytes_written;
      nwritten += bytes_written;
    }
  }

#ifdef DEBUG
  printf ("oggz_write_output: OUT %ld\n", nwritten);
#endif

  writer->writing = 0;

  if (nwritten == 0) {
    if (cb_ret == OGGZ_WRITE_EMPTY) cb_ret = 0;
    return oggz_map_return_value_to_error (cb_ret);
  } else {
    oggz->cb_next = cb_ret;
  }

  return nwritten;
}

long
oggz_write (OGGZ * oggz, long n)
{
  OggzWriter * writer;
  long bytes, bytes_written = 1, remaining = n, nwritten = 0;
  int active = 1, cb_ret = 0;

  if (oggz == NULL) return OGGZ_ERR_BAD_OGGZ;

  writer = &oggz->x.writer;

  if (!(oggz->flags & OGGZ_WRITE)) {
    return OGGZ_ERR_INVALID;
  }

  if (writer->writing) return OGGZ_ERR_RECURSIVE_WRITE;
  writer->writing = 1;

#ifdef DEBUG
  printf ("oggz_write: IN\n");
#endif

  /* A write-callback failure deferred from a previous call is reported
   * once before any further output. */
  if (writer->io_pending_error) {
    int err = writer->io_pending_error;
    writer->io_pending_error = 0;
    writer->writing = 0;
    return err;
  }

  if ((cb_ret = oggz->cb_next) != OGGZ_CONTINUE) {
    oggz->cb_next = 0;
    writer->writing = 0;
    writer->no_more_packets = 0;
    if (cb_ret == OGGZ_WRITE_EMPTY) cb_ret = 0;
    return oggz_map_return_value_to_error (cb_ret);
  }

  while (active && remaining > 0) {
    bytes = MIN (remaining, 1024);

#ifdef DEBUG
    printf ("oggz_write: write loop (%ld , %ld remain) ...\n", bytes,
	    remaining);
#endif

    while (writer->state == OGGZ_MAKING_PACKETS) {
#ifdef DEBUG
      printf ("oggz_write: MAKING PACKETS\n");
#endif
      if ((cb_ret = oggz_writer_make_packet (oggz)) != OGGZ_CONTINUE) {
#ifdef DEBUG
        printf ("oggz_write: no packets (cb_ret is %d)\n", cb_ret);
#endif
        /*
         * if we're out of packets because we're at the end of the file,
         * we can't finish just yet.  Instead we need to force a page flush,
         * and write the page out.  So we set flushing and no_more_packets to
         * 1.  This causes oggz_page_init to flush the page, then we
         * will switch the state to OGGZ_WRITING_PAGES, which will trigger
         * the writing code below.
         */
        if (cb_ret == OGGZ_WRITE_EMPTY) {
#ifdef DEBUG
          printf ("oggz_write: Inferred end of data, forcing a page flush.\n");
#endif
          writer->flushing = 1;
          writer->no_more_packets = 1;
          cb_ret = OGGZ_CONTINUE;
        } else {
#ifdef DEBUG
          printf ("oggz_write: Stopped by user callback.\n");
#endif
          active = 0;
          break;
        }
      }
      if (oggz_page_init (oggz)) {
        writer->state = OGGZ_WRITING_PAGES;
      } else {
#ifdef DEBUG
        printf ("oggz_write: unable to make page...\n");
#endif
        if (writer->no_more_packets) {
          active = 0;
          break;
        }
      }
    }

    if (writer->state == OGGZ_WRITING_PAGES) {
      ogg_page * og = &oggz->current_page;
      long page_total = og->header_len + og->body_len;

      bytes_written = oggz_page_writeout (oggz, bytes);
#ifdef DEBUG
      printf ("oggz_write: MAKING PAGES; wrote %ld bytes\n", bytes_written);
#endif

      if (bytes_written > 0) {
        remaining -= bytes_written;
        nwritten += bytes_written;

        if (writer->page_offset < page_total) {
          /* The callback accepted fewer bytes than it was offered while the
           * page is still incomplete: a short write. Do not invoke the
           * callback again in this call; resume from the first unwritten
           * byte next time. */
          active = 0;
        }
        continue;
      }

      if (bytes_written == -1) {
        /* Hard callback failure. If this call already produced bytes, hand
         * them back now and report the error exactly once, on the next
         * call; otherwise report it immediately. */
        active = 0;
        if (nwritten > 0) {
          writer->io_pending_error = OGGZ_ERR_SYSTEM;
          break;
        }
        writer->writing = 0;
        return OGGZ_ERR_SYSTEM;
      }

      /* bytes_written == 0 */
      if (writer->page_offset < page_total) {
        /* The callback paused (returned 0, or -1/EAGAIN) with the page
         * still incomplete. Leave everything in place and resume from the
         * first unwritten byte on the next call. */
        active = 0;
        if (nwritten == 0) cb_ret = OGGZ_ERR_IO_AGAIN;
        break;
      }

      /* The current page is fully written; move on to the next one. */
      if (writer->no_more_packets) {
        active = 0;
        break;
      } else if (!oggz_page_init (oggz)) {
#ifdef DEBUG
        printf ("oggz_write: bytes_written == 0, DONE\n");
#endif
        writer->state = OGGZ_MAKING_PACKETS;
      }
    }
  }

#ifdef DEBUG
  printf ("oggz_write: OUT %ld\n", nwritten);
#endif

  writer->writing = 0;

  if (nwritten == 0) {
    if (cb_ret == OGGZ_WRITE_EMPTY) cb_ret = 0;
    return oggz_map_return_value_to_error (cb_ret);
  } else {
    oggz->cb_next = cb_ret;
  }

  return nwritten;
}

long
oggz_write_get_next_page_size (OGGZ * oggz)
{
  OggzWriter * writer;
  ogg_page * og;

  if (oggz == NULL) return OGGZ_ERR_BAD_OGGZ;

  writer = &oggz->x.writer;

  if (!(oggz->flags & OGGZ_WRITE)) {
    return OGGZ_ERR_INVALID;
  }

  og = &oggz->current_page;

  return (og->header_len + og->body_len - (long)writer->page_offset);
}

#else /* OGGZ_CONFIG_WRITE */

#include <ogg/ogg.h>
#include "oggz_private.h"

OGGZ *
oggz_write_init (OGGZ * oggz)
{
  return NULL;
}

int
oggz_write_flush (OGGZ * oggz)
{
  return OGGZ_ERR_DISABLED;
}

OGGZ *
oggz_write_close (OGGZ * oggz)
{
  return NULL;
}

int
oggz_write_set_hungry_callback (OGGZ * oggz, OggzWriteHungry hungry,
				int only_when_empty, void * user_data)
{
  return OGGZ_ERR_DISABLED;
}

int
oggz_write_feed (OGGZ * oggz, ogg_packet * op, long serialno, int flush,
		 int * guard)
{
  return OGGZ_ERR_DISABLED;
}

long
oggz_write_output (OGGZ * oggz, unsigned char * buf, long n)
{
  return OGGZ_ERR_DISABLED;
}

long
oggz_write (OGGZ * oggz, long n)
{
  return OGGZ_ERR_DISABLED;
}

long
oggz_write_get_next_page_size (OGGZ * oggz)
{
  return OGGZ_ERR_DISABLED;
}

int
oggz_write_set_pending_limit (OGGZ * oggz, ogg_int64_t limit)
{
  return OGGZ_ERR_DISABLED;
}

ogg_int64_t
oggz_write_get_pending_bytes (OGGZ * oggz)
{
  return OGGZ_ERR_DISABLED;
}

#endif
