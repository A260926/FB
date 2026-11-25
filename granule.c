/*
 * libmad - MPEG audio decoder library
 *
 * Bounded granule pull decoder.
 *
 * A single instance owns a fixed-size compressed queue, one in-flight
 * physical frame's Layer III state (side information, main-data bit position,
 * remaining bit count, and reservoir), the overlap history, and the synthesis
 * filterbank.  No state grows with stream length.  mad_granule_next() advances
 * at most one granule; feed()/finish()/progress() never perform spectral or
 * synthesis work.
 */

# ifdef HAVE_CONFIG_H
#  include "config.h"
# endif

# include "global.h"

# include <stdlib.h>
# include <string.h>

# ifdef HAVE_LIMITS_H
#  include <limits.h>
# else
#  define CHAR_BIT  8
# endif

# include "bit.h"
# include "stream.h"
# include "frame.h"
# include "synth.h"
# include "layer3.h"
# include "granule.h"

/* Owned compressed queue.  Empty instances accept at least this many bytes. */
# define GRANULE_QUEUE_BYTES	8192

/* Bounded look-ahead needed to read the following frame's main_data_begin. */
# define GRANULE_LOOKAHEAD	MAD_BUFFER_GUARD

struct mad_granule_decoder {
  int options;

  unsigned char queue[GRANULE_QUEUE_BYTES + MAD_BUFFER_GUARD];
  size_t qlen;

  struct mad_stream stream;
  struct mad_frame  frame;
  struct mad_synth  synth;

  struct mad_iii_frame iii;
  int frame_ready;		/* iii holds a prepared physical frame */
  unsigned int next_gr;		/* next granule index within that frame */

  /* Per-stream fixed format, established by the first accepted header. */
  int have_format;
  int lsf;
  unsigned int samplerate;
  unsigned int nchannels;

  int finished;
  int ended;
  int error_sticky;
  enum mad_error error;

  uint64_t granules;
  uint64_t physical_frames;
  uint64_t sample_frames;
};

/* --- header length look-ahead -------------------------------------------- */

/*
 * The bitrate/sample-rate tables mirror those in frame.c.  Only what is needed
 * to bound one physical frame is replicated here; the authoritative header
 * parse still runs through mad_header_decode().
 */
static unsigned long const granule_bitrate_mpeg1[15] = {
  0, 32000, 40000, 48000, 56000, 64000, 80000, 96000,
  112000, 128000, 160000, 192000, 224000, 256000, 320000
};

static unsigned long const granule_bitrate_lsf[15] = {
  0, 8000, 16000, 24000, 32000, 40000, 48000, 56000,
  64000, 80000, 96000, 112000, 128000, 144000, 160000
};

static unsigned int const granule_samplerate[3] = { 44100, 48000, 32000 };

struct granule_header {
  int lsf;
  unsigned int samplerate;
  unsigned int nchannels;
  unsigned int frame_len;
};

/*
 * Parse just enough of a 4-byte Layer III MPEG-1/2 header to determine the
 * physical frame length and fixed stream format.  Free-format, MPEG-2.5,
 * other layers, and reserved field values are rejected.  Returns a mad_error
 * (MAD_ERROR_NONE on success).
 */
static enum mad_error granule_header_parse(unsigned char const *p,
					   struct granule_header *out)
{
  unsigned int version, layer_bits, layer, index, sridx, padding;
  unsigned int mode;
  unsigned long bitrate, samplerate, slots, len;
  int lsf;

  if (p[0] != 0xff || (p[1] & 0xe0) != 0xe0)
    return MAD_ERROR_LOSTSYNC;

  version = (p[1] >> 3) & 0x3;
  if (version == 0 || version == 1)
    return MAD_ERROR_LOSTSYNC;		/* MPEG-2.5 / reserved */

  lsf = (version == 2);			/* MPEG-2 (LSF) */

  layer_bits = (p[1] >> 1) & 0x3;
  layer = 4 - layer_bits;
  if (layer != MAD_LAYER_III)
    return MAD_ERROR_BADLAYER;

  index = (p[2] >> 4) & 0xf;
  if (index == 15)
    return MAD_ERROR_BADBITRATE;
  if (index == 0)
    return MAD_ERROR_BADBITRATE;	/* free-format not supported */

  sridx = (p[2] >> 2) & 0x3;
  if (sridx == 3)
    return MAD_ERROR_BADSAMPLERATE;

  padding = (p[2] >> 1) & 0x1;

  bitrate    = lsf ? granule_bitrate_lsf[index] : granule_bitrate_mpeg1[index];
  samplerate = granule_samplerate[sridx];
  if (lsf)
    samplerate /= 2;

  slots = lsf ? 72 : 144;
  len = slots * bitrate / samplerate + padding;

  mode = 3 - ((p[3] >> 6) & 0x3);

  out->lsf        = lsf;
  out->samplerate = samplerate;
  out->nchannels  = (mode == MAD_MODE_SINGLE_CHANNEL) ? 1 : 2;
  out->frame_len  = (unsigned int) len;

  return MAD_ERROR_NONE;
}

/* --- state construction -------------------------------------------------- */

static void granule_state_clear(struct mad_granule_decoder *d, int options)
{
  memset(d->queue, 0, sizeof d->queue);
  d->qlen = 0;

  mad_stream_init(&d->stream);
  mad_frame_init(&d->frame);
  mad_synth_init(&d->synth);
  mad_stream_options(&d->stream, options);

  memset(&d->iii, 0, sizeof d->iii);
  d->frame_ready = 0;
  d->next_gr = 0;

  d->have_format = 0;
  d->lsf = 0;
  d->samplerate = 0;
  d->nchannels = 0;

  d->finished = 0;
  d->ended = 0;
  d->error_sticky = 0;
  d->error = MAD_ERROR_NONE;

  d->granules = 0;
  d->physical_frames = 0;
  d->sample_frames = 0;
}

static void granule_state_destroy(struct mad_granule_decoder *d)
{
  mad_synth_finish(&d->synth);
  mad_frame_finish(&d->frame);
  mad_stream_finish(&d->stream);
}

/* --- public API ---------------------------------------------------------- */

struct mad_granule_decoder *mad_granule_new(int options)
{
  struct mad_granule_decoder *d;
  int allowed = MAD_OPTION_IGNORECRC | MAD_OPTION_HALFSAMPLERATE;

  if (options & ~allowed)
    return 0;

  d = malloc(sizeof *d);
  if (d == 0)
    return 0;

  d->options = options;
  granule_state_clear(d, options);

  return d;
}

void mad_granule_free(struct mad_granule_decoder *d)
{
  if (d == 0)
    return;

  granule_state_destroy(d);
  free(d);
}

void mad_granule_reset(struct mad_granule_decoder *d)
{
  int options;

  if (d == 0)
    return;

  options = d->options;
  granule_state_destroy(d);
  granule_state_clear(d, options);
}

int mad_granule_feed(struct mad_granule_decoder *d,
		     unsigned char const *data, size_t length,
		     size_t *accepted)
{
  size_t room, take;

  if (d == 0 || accepted == 0)
    return -1;
  if (data == 0 && length != 0)
    return -1;

  *accepted = 0;

  if (d->finished || d->error_sticky)
    return -1;

  if (length == 0)
    return 0;

  room = GRANULE_QUEUE_BYTES - d->qlen;
  take = length < room ? length : room;

  if (take != 0) {
    memcpy(d->queue + d->qlen, data, take);
    d->qlen += take;
  }

  *accepted = take;
  return 0;
}

int mad_granule_finish(struct mad_granule_decoder *d)
{
  if (d == 0)
    return -1;

  d->finished = 1;
  return 0;
}

int mad_granule_progress(struct mad_granule_decoder const *d,
			 struct mad_granule_progress *progress)
{
  if (d == 0 || progress == 0)
    return -1;

  /*
   * The in-flight physical frame is retained in the owned queue until its
   * last granule is delivered, so the queue length already includes any
   * pending frame bytes.  Fixed-size codec workspace (reservoir, overlap,
   * filterbank) is constant and is not stream input.
   */
  progress->granules        = d->granules;
  progress->physical_frames = d->physical_frames;
  progress->sample_frames   = d->sample_frames;
  progress->buffered_bytes  = d->qlen;
  progress->error           = d->error_sticky ? d->error : MAD_ERROR_NONE;

  return 0;
}

/* --- decoding ------------------------------------------------------------ */

static int granule_latch(struct mad_granule_decoder *d, enum mad_error error)
{
  d->error_sticky = 1;
  d->error = error;
  d->frame_ready = 0;
  return MAD_GRANULE_ERROR;
}

static void granule_guard(struct mad_granule_decoder *d)
{
  memset(d->queue + d->qlen, 0, MAD_BUFFER_GUARD);
}

/*
 * Attempt to prepare the physical frame at the head of the queue.  Returns
 * MAD_GRANULE_NEED_INPUT when more bytes are required, or the status of the
 * first granule once a frame is ready (PCM or ERROR).
 */
static int granule_prepare(struct mad_granule_decoder *d)
{
  struct granule_header hdr;
  enum mad_error error;
  size_t need;

  if (d->qlen == 0) {
    if (d->finished) {
      d->ended = 1;
      return MAD_GRANULE_END;
    }
    return MAD_GRANULE_NEED_INPUT;
  }

  if (d->qlen < 4) {
    if (d->finished)
      return granule_latch(d, MAD_ERROR_BUFLEN);
    return MAD_GRANULE_NEED_INPUT;
  }

  error = granule_header_parse(d->queue, &hdr);
  if (error != MAD_ERROR_NONE)
    return granule_latch(d, error);

  if (d->qlen < hdr.frame_len) {
    if (d->finished)
      return granule_latch(d, MAD_ERROR_BUFLEN);
    return MAD_GRANULE_NEED_INPUT;
  }

  /*
   * Before end-of-stream we need the following frame's header bytes to
   * reproduce the exact main_data_begin look-ahead.  Once finished, the
   * zero guard stands in for them exactly as in the full-frame API.
   */
  if (!d->finished && d->qlen < hdr.frame_len + GRANULE_LOOKAHEAD)
    return MAD_GRANULE_NEED_INPUT;

  /* Fixed per-stream format. */
  if (d->have_format) {
    if (hdr.lsf != d->lsf)
      return granule_latch(d, MAD_ERROR_BADLAYER);
    if (hdr.samplerate != d->samplerate)
      return granule_latch(d, MAD_ERROR_BADSAMPLERATE);
    if (hdr.nchannels != d->nchannels)
      return granule_latch(d, MAD_ERROR_BADMODE);
  }

  need = d->qlen + MAD_BUFFER_GUARD;
  granule_guard(d);

  mad_stream_buffer(&d->stream, d->queue, (unsigned long) need);
  d->frame.options = d->options;

  if (mad_header_decode(&d->frame.header, &d->stream) == -1)
    return granule_latch(d, d->stream.error);

  /* The authoritative header must agree with the bounded look-ahead. */
  if (((d->frame.header.flags & MAD_FLAG_LSF_EXT) != 0) != (hdr.lsf != 0) ||
      d->frame.header.samplerate != hdr.samplerate ||
      MAD_NCHANNELS(&d->frame.header) != hdr.nchannels)
    return granule_latch(d, MAD_ERROR_LOSTSYNC);

  if (!d->have_format) {
    d->have_format  = 1;
    d->lsf          = hdr.lsf;
    d->samplerate   = hdr.samplerate;
    d->nchannels    = hdr.nchannels;
  }

  d->frame.header.flags &= ~MAD_FLAG_INCOMPLETE;

  if (mad_iii_prepare(&d->stream, &d->frame, &d->iii) == -1)
    return granule_latch(d, d->stream.error);

  if (d->iii.result != 0)
    return granule_latch(d, d->stream.error);

  d->frame_ready = 1;
  d->next_gr = 0;

  return MAD_GRANULE_PCM;
}

/* Copy one granule's native fixed-point PCM out of synth->pcm. */
static void granule_deliver(struct mad_granule_decoder *d,
			    struct mad_pcm *pcm, unsigned int gr)
{
  unsigned int half = d->options & MAD_OPTION_HALFSAMPLERATE;
  unsigned int len  = half ? 288 : 576;
  unsigned int off  = (half ? 16 : 32) * 18 * gr;
  unsigned int ch, nch = d->synth.pcm.channels;

  pcm->samplerate = d->synth.pcm.samplerate;
  pcm->channels   = (unsigned short) nch;
  pcm->length     = (unsigned short) len;

  for (ch = 0; ch < nch && ch < 2; ++ch)
    memcpy(pcm->samples[ch], d->synth.pcm.samples[ch] + off,
	   len * sizeof(mad_fixed_t));
}

/* Decode and synthesize the next granule of a prepared frame. */
static int granule_advance(struct mad_granule_decoder *d, struct mad_pcm *pcm)
{
  unsigned int gr = d->next_gr;
  enum mad_error error;

  error = mad_iii_decode_granule(&d->iii.ptr, &d->frame, &d->iii.si,
				 d->iii.sfreqi, d->iii.nch, gr,
				 &d->iii.bits_left);
  if (error != MAD_ERROR_NONE)
    return granule_latch(d, error);

  /* Synthesize this granule's 18 subband steps into d->synth.pcm. */
  mad_synth_step(&d->synth, &d->frame, 18 * gr, 18);

  granule_deliver(d, pcm, gr);

  ++d->granules;
  d->sample_frames += pcm->length;
  ++d->next_gr;

  if (d->next_gr == d->iii.ngr) {
    /* Last granule: finalize the reservoir exactly once, then retire the
       physical frame's compressed bytes from the queue. */
    unsigned int consumed =
      (unsigned int) (d->stream.next_frame - d->stream.this_frame);

    mad_iii_preload(&d->stream, &d->iii, d->stream.next_frame);

    ++d->physical_frames;
    d->frame_ready = 0;
    d->next_gr = 0;

    if (consumed != 0) {
      memmove(d->queue, d->queue + consumed, d->qlen - consumed);
      d->qlen -= consumed;
    }
  }

  return MAD_GRANULE_PCM;
}

int mad_granule_next(struct mad_granule_decoder *d, struct mad_pcm *pcm)
{
  int status;

  if (d == 0 || pcm == 0)
    return -1;

  pcm->length = 0;

  if (d->error_sticky)
    return MAD_GRANULE_ERROR;

  if (d->ended)
    return MAD_GRANULE_END;

  if (!d->frame_ready) {
    status = granule_prepare(d);
    if (status != MAD_GRANULE_PCM)
      return status;
  }

  return granule_advance(d, pcm);
}
