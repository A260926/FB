/********************************************************************
 *                                                                  *
 * Rewind window for non-seekable Ogg/Vorbis streams.              *
 *                                                                  *
 * The cache stores complete compressed Vorbis audio packets,      *
 * their Ogg framing metadata (granule position, packet number,     *
 * flags) and a linear PCM-range index.  No decoded PCM, no DSP    *
 * snapshots and no copy of the whole input are kept; temporary    *
 * files and a second OggVorbis_File are never used.  A replay     *
 * rebuilds the decoder by re-synthesising only the packets needed *
 * to cover the requested position (the target packet plus its     *
 * overlap predecessor), then rejoins the original packet queue.   *
 *                                                                  *
 ********************************************************************/

#ifndef _V_REWIND_CACHE_H_
#define _V_REWIND_CACHE_H_

#include <ogg/ogg.h>
#include "vorbis/codec.h"

/* smallest/largest compressed-payload budget accepted by
   ov_rewind_enable() */
#define REWIND_MIN_BYTES 65536
#define REWIND_MAX_BYTES 8388608

typedef struct rewind_cache rewind_cache;

/* one indexed audio packet.  The payload storage belongs to the
   cache; the metadata mirrors the ogg_packet as delivered by the
   framing layer. */
typedef struct {
  unsigned char *packet;   /* compressed payload (cache owned) */
  long           bytes;
  ogg_int64_t    packetno; /* logical packet sequence number */
  ogg_int64_t    granulepos;
  int            e_o_s;
  ogg_int64_t    begin;    /* first PCM frame of this packet, link local */
  ogg_int64_t    end;      /* one past the last PCM frame, link local */
} rewind_entry;

/* allocate/destroy a window with the given payload budget. */
rewind_cache *rewind_cache_create(long max_bytes);
void          rewind_cache_destroy(rewind_cache *c);

/* forget every retained packet (link change, OV_HOLE) */
void          rewind_cache_clear(rewind_cache *c);
/* OV_HOLE variant: also drop the "link origin" status, since the lost
   predecessor makes the next retained packet non-reconstructible */
void          rewind_cache_discontinuity(rewind_cache *c);

/* retain one fully decoded audio packet.  begin/end are the link
   local PCM span ([begin,end)) actually delivered for the packet.
   Eviction always starts at the oldest packet.  A packet larger than
   the budget is accepted and immediately becomes the only occupant. */
int           rewind_cache_push(rewind_cache *c, const ogg_packet *op,
                                ogg_int64_t begin, ogg_int64_t end);

/* recoverable PCM window of the current logical link.  Returns 0 and
   fills the two outputs, or OV_FALSE (with both outputs zeroed) when
   nothing is recoverable.  Frames [begin,end) can be replayed
   exactly; begin is the first replayable frame (0 based at the link's
   first actual output frame) and end is one past the last, i.e. the
   count of delivered frames, matching the ov_pcm_tell() cursor. */
int           rewind_cache_range(rewind_cache *c,
                                 ogg_int64_t *begin, ogg_int64_t *end);

/* index of the packet whose [begin,end) span contains target, or
   -1 if target is not inside the recoverable window. */
int           rewind_cache_locate(rewind_cache *c, ogg_int64_t target);

/* direct indexed access for the replay machinery */
int                  rewind_cache_count(rewind_cache *c);
const rewind_entry  *rewind_cache_get(rewind_cache *c, int idx);

/* accounting/introspection used by the tests */
long          rewind_cache_payload_bytes(rewind_cache *c);
long          rewind_cache_budget(rewind_cache *c);
long          rewind_cache_evictions(rewind_cache *c);

/* Per-file rewind runtime, owned by vorbisfile and hung off
   OggVorbis_File.rewind_state.  The struct is visible here so the
   internal test driver can inspect the accounting. */
typedef struct rewind_runtime {
  rewind_cache *cache;
  int           armed;         /* ov_rewind_enable() succeeded */
  int           replay;        /* serving from the replay queue */
  ogg_int64_t   watermark;     /* frames made available in the link */
  ogg_int64_t   link_pos;      /* delivered frames since link start */
  long          synth_total;   /* all successful vorbis_synthesis calls */
  long          synth_replay;  /* synthesis calls serving a replay */

  /* replay packet queue (payloads are owned by the cache) */
  ogg_packet   *q;
  int           qn;            /* queued packets */
  int           qcap;
  int           qhead;         /* next packet to decode */
} rewind_runtime;

/* allocate/free the runtime wrapper around a budgeted cache */
rewind_runtime *rewind_runtime_create(long max_bytes);
void            rewind_runtime_destroy(rewind_runtime *rt);
void            rewind_runtime_reset_link(rewind_runtime *rt);

#endif
