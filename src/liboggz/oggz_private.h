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

#ifndef __OGGZ_PRIVATE_H__
#define __OGGZ_PRIVATE_H__

#include <stdio.h>
#include <sys/types.h>

#include <ogg/ogg.h>
#include <oggz/oggz_constants.h>
#include <oggz/oggz_off_t.h>

#include "oggz/oggz_packet.h"

#include "oggz_macros.h"
#include "oggz_vector.h"
#include "oggz_dlist.h"

#define OGGZ_AUTO_MULT 1000Ull

typedef struct _OGGZ OGGZ;
typedef struct _OggzComment OggzComment;
typedef struct _OggzIO OggzIO;
typedef struct _OggzReader OggzReader;
typedef struct _OggzWriter OggzWriter;


typedef int (*OggzReadPacket) (OGGZ * oggz, oggz_packet * op, long serialno,
			       void * user_data);
typedef int (*OggzReadPage) (OGGZ * oggz, const ogg_page * og, long serialno,
			     void * user_data);

/* oggz_stream */
#include "oggz_stream_private.h"

typedef ogg_int64_t (*OggzMetric) (OGGZ * oggz, long serialno,
				   ogg_int64_t granulepos,
				   void * user_data);

typedef int (*OggzOrder) (OGGZ * oggz, ogg_packet * op, void * target,
			  void * user_data);

typedef int (*OggzWriteHungry) (OGGZ * oggz, int empty, void * user_data);

/* oggz_io */
typedef size_t (*OggzIORead) (void * user_handle, void * buf, size_t n);
typedef size_t (*OggzIOWrite) (void * user_handle, void * buf, size_t n);
typedef int (*OggzIOSeek) (void * user_handle, long offset, int whence);
typedef long (*OggzIOTell) (void * user_handle);
typedef int (*OggzIOFlush) (void * user_handle);

struct _oggz_stream_t {
  ogg_stream_state ogg_stream;

  /** STATIC INFO */
  int content;
  int numheaders;
  int preroll;
  ogg_int64_t granulerate_n;
  ogg_int64_t granulerate_d;
  ogg_int64_t first_granule;
  ogg_int64_t basegranule;
  int granuleshift;

  /* The comments */
  char * vendor;
  OggzVector * comments;

  /** CURRENT STATE **/
  /* non b_o_s packet has been written (not just queued) */
  int delivered_non_b_o_s;

  int b_o_s; /* beginning of stream */
  int e_o_s; /* end of stream */
  ogg_int64_t granulepos;
  ogg_int64_t packetno;

  /** CALLBACKS **/
  OggzMetric metric;
  void * metric_user_data;
  int metric_internal;

  OggzOrder order;
  void * order_user_data;

  OggzReadPacket read_packet;
  void * read_user_data;

  OggzReadPage read_page;
  void * read_page_user_data;

  /* calculated granulepos values, not extracted values */
  ogg_int64_t last_granulepos;
  ogg_int64_t page_granulepos;
  void * calculate_data;
  ogg_packet * last_packet;
};

struct _OggzReader {
  ogg_sync_state ogg_sync;

  /* XXX: these two can prolly be removed again :) */
  ogg_stream_state ogg_stream;
  long current_serialno;

  OggzReadPacket read_packet;
  void * read_user_data;

  OggzReadPage read_page;
  void * read_page_user_data;

  ogg_int64_t current_unit;
  ogg_int64_t current_granulepos;

  /* Read positioning */
  long current_page_bytes;

  /* Calculation of position */
  oggz_off_t current_packet_begin_page_offset;
  int current_packet_pages;
  int current_packet_begin_segment_index;

#if 0
  oggz_off_t offset_page_end; /* offset of end of current page */
#endif
};

/**
 * Bundle a packet with the stream it is being queued for; used in
 * the packet_queue vector
 */
typedef struct {
  ogg_packet op;
  oggz_stream_t * stream;
  int flush;
  int * guard;
} oggz_writer_packet_t;

/*
 * One record per physical Ogg page, kept in file order. Only framing
 * metadata and offsets are retained; page and packet payloads are never
 * stored in the index.
 */
typedef struct {
  oggz_off_t offset;       /* byte offset of the page capture pattern */
  long page_bytes;         /* total page size (header + body) */
  long serialno;           /* logical bitstream this page belongs to */
  int continued;           /* page header continuation flag */
  int nsegs;               /* number of lacing values */
} OggzPktIndexPage;

/*
 * One record per completed Ogg packet, per logical bitstream, in packet
 * order. Packet numbering starts at the BOS packet (packetno 0).
 */
typedef struct {
  long serialno;
  ogg_int64_t packetno;     /* ordinal within the logical bitstream */
  long bytes;               /* packet payload size in bytes */
  long b_o_s;
  long e_o_s;
  ogg_int64_t granulepos;   /* granulepos attached to the packet */
  long begin_page;          /* index of the page where the packet starts */
  long end_page;            /* index of the page where the packet completes */
} OggzPktIndexPacket;

/*
 * Per-stream packet index and the read cursor used by oggz_seek_packets().
 * The cursor ("next packet to deliver") starts at zero and only advances
 * when packets are actually delivered to user callbacks.
 */
typedef struct {
  long serialno;
  OggzPktIndexPacket * packets;  /* packet records, in completion order */
  long n_packets;
  long packets_cap;

  ogg_int64_t cursor;       /* ordinal of the next packet to deliver */

  long replay_index;        /* next record expected during replay */
} OggzPktIndexStream;

/*
 * Packet-seeking state for one OGGZ read handle. The first packet seek
 * scans the whole physical bitstream once and fills the page/packet
 * records; later seeks re-open only the page interval required to rebuild
 * the target and the in-flight packets of the other tracks, plus a small
 * fixed preread. Everything is destroyed with the handle; two handles
 * never share this state.
 */
typedef struct {
  OggzPktIndexPage * pages;       /* page records, in physical order */
  long n_pages;
  long pages_cap;

  OggzPktIndexStream * streams;   /* one record per logical bitstream */
  long n_streams;
  long streams_cap;

  int armed;              /* suppression window of a seek is active */
  int replay_active;      /* ordinal rewrite active until records exhaust */

  long resume_page;       /* first page to feed on the next read */
  long next_page_index;   /* index of the next page the reader will see */
  long disarm_after_page; /* last page fed under replay control */

  /* Identity of the armed target; packets completing before it (in
   * physical page order, then per-stream order on the target page) are
   * replayed without callbacks. */
  ogg_int64_t target_packetno;
  long target_end_page;

  oggz_off_t resume_offset;
  oggz_off_t saved_offset;

  /* saved reader state while the resume machinery is armed */
  long saved_current_serialno;
} OggzPacketIndex;

enum oggz_writer_state {
  OGGZ_MAKING_PACKETS = 0,
  OGGZ_WRITING_PAGES = 1
};

struct _OggzWriter {
  oggz_writer_packet_t * next_zpacket; /* stashed in case of FLUSH_BEFORE */
  OggzVector * packet_queue;

  OggzWriteHungry hungry;
  void * hungry_user_data;
  int hungry_only_when_empty;

  int writing; /* already mid-write; check for recursive writes */
  int state; /* OGGZ_MAKING_PACKETS or OGGZ_WRITING_PAGES */

  int flushing; /* whether current packet is being flushed or just paged out */

#if 0
  int eog; /* end of page */
  int eop; /* end of packet */
#endif
  int eos; /* end of stream */

  oggz_writer_packet_t * current_zpacket;

  int packet_offset; /* n bytes already copied out of current packet */
  int page_offset; /* n bytes already copied out of current page */

  ogg_stream_state * current_stream;

  int no_more_packets; /* used only in the local oggz_write loop to indicate
                          end of stream */

};

struct _OggzIO {
  OggzIORead read;
  void * read_user_handle;

  OggzIOWrite write;
  void * write_user_handle;

  OggzIOSeek seek;
  void * seek_user_handle;

  OggzIOTell tell;
  void * tell_user_handle;

  OggzIOFlush flush;
  void * flush_user_handle;
};

struct _OggzComment {
  /** The name of the comment, eg. "AUTHOR" */
  char * name;

  /** The value of the comment, as UTF-8 */
  char * value;
};

struct _OGGZ {
  int flags;
  FILE * file;
  OggzIO * io;

  ogg_packet current_packet;
  ogg_page current_page;

  oggz_off_t offset; /* offset of current page start */
  oggz_off_t offset_data_begin; /* offset of unit 0 page start */

  long run_blocksize; /* blocksize to use for oggz_run() */
  int cb_next;

  OggzVector * streams;
  int all_at_eos; /* all streams are at eos */

  OggzMetric metric;
  void * metric_user_data;
  int metric_internal;

  OggzOrder order;
  void * order_user_data;

  union {
    OggzReader reader;
    OggzWriter writer;
  } x;

  OggzDList * packet_buffer;

  /* Lazily built page/packet index for oggz_seek_packets(); NULL until
   * the first packet seek. Read handles only. */
  OggzPacketIndex * packet_index;

  /* Whether the live libogg packet counters still reflect sequential
   * reading from the start of the container; cleared by byte/time seeks
   * and purge, which reset the stream states. */
  int pkt_seek_state_valid;
};

OGGZ * oggz_read_init (OGGZ * oggz);
OGGZ * oggz_read_close (OGGZ * oggz);

OGGZ * oggz_write_init (OGGZ * oggz);
int oggz_write_flush (OGGZ * oggz);
OGGZ * oggz_write_close (OGGZ * oggz);

int oggz_map_return_value_to_error (int cb_ret);

int oggz_get_bos (OGGZ * oggz, long serialno);
ogg_int64_t oggz_get_unit (OGGZ * oggz, long serialno, ogg_int64_t granulepos);

int oggz_set_metric_internal (OGGZ * oggz, long serialno, OggzMetric metric,
			      void * user_data, int internal);
int oggz_has_metrics (OGGZ * oggz);

int oggz_purge (OGGZ * oggz);

/* metric_internal */

int
oggz_set_granulerate (OGGZ * oggz, long serialno, 
                                    ogg_int64_t granule_rate_numerator,
                                    ogg_int64_t granule_rate_denominator);

int
oggz_get_granulerate (OGGZ * oggz, long serialno,
                                    ogg_int64_t * granulerate_n,
                                    ogg_int64_t * granulerate_d);

int oggz_set_granuleshift (OGGZ * oggz, long serialno, int granuleshift);
int oggz_get_granuleshift (OGGZ * oggz, long serialno);

int oggz_set_first_granule (OGGZ * oggz, long serialno, ogg_int64_t first_granule);

int oggz_set_preroll (OGGZ * oggz, long serialno, int preroll);
int oggz_get_preroll (OGGZ * oggz, long serialno);

/* oggz_auto */
 
int
oggz_auto_read_bos_page (OGGZ * oggz, ogg_page * og, long serialno,
                         void * user_data);
int
oggz_auto_read_bos_packet (OGGZ * oggz, ogg_packet * op, long serialno, 
                           void * user_data);

int
oggz_auto_read_comments (OGGZ * oggz, oggz_stream_t * stream, long serialno,
                         ogg_packet * op);

int oggz_auto_identify_page (OGGZ *oggz, ogg_page *og, long serialno);
int oggz_auto_identify_packet (OGGZ * oggz, ogg_packet * op, long serialno);

/* comments */
int oggz_comments_init (oggz_stream_t * stream);
int oggz_comments_free (oggz_stream_t * stream);
int oggz_comments_decode (OGGZ * oggz, long serialno,
                          unsigned char * comments, long length);
long oggz_comments_encode (OGGZ * oggz, long serialno,
                           unsigned char * buf, long length);

/* oggz_io */
size_t oggz_io_read (OGGZ * oggz, void * buf, size_t n);
size_t oggz_io_write (OGGZ * oggz, void * buf, size_t n);
int oggz_io_seek (OGGZ * oggz, long offset, int whence);
long oggz_io_tell (OGGZ * oggz);
int oggz_io_flush (OGGZ * oggz);

/* oggz_read */
OggzDListIterResponse oggz_read_free_pbuffers(void *elem);

/* oggz_packet_seek */
void oggz_pkt_index_destroy (OGGZ * oggz);
void oggz_pkt_seek_note_reset (OGGZ * oggz);
void oggz_pkt_seek_note_page (OGGZ * oggz, const ogg_page * og);
int oggz_pkt_seek_suppress_packet (OGGZ * oggz, long serialno,
                                   ogg_packet * op);
int oggz_pkt_seek_ignore_hole (OGGZ * oggz);
void oggz_pkt_seek_new_stream (OGGZ * oggz, long serialno);

#endif /* __OGGZ_PRIVATE_H__ */
