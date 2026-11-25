#ifndef OGG_THEORA_INTERNAL_H
#define OGG_THEORA_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <ogg/ogg.h>
#include <ogg/theora_edit.h>

/* A contiguous run of packet bytes in the source file.  Page headers are
 * never included; a packet which crosses pages is split into several
 * ranges. */
typedef struct theora_range {
  uint64_t offset;
  uint64_t length;
} theora_range;

/* One source data packet (one video frame). */
typedef struct theora_frame {
  size_t range_first;   /* index into plan->ranges */
  size_t range_count;
  uint64_t bytes;       /* total packet bytes (may be zero) */
  unsigned char keyframe;
} theora_frame;

/* A single logical Theora stream found in the source. */
typedef struct theora_stream {
  uint32_t serial;
  uint32_t shift;       /* keyframe granule shift from the ident header */
  uint32_t fps_num;
  uint32_t fps_den;

  size_t frame_first;   /* index into plan->frames */
  uint64_t frame_count;
  unsigned char *ident;
  size_t ident_len;
  unsigned char *comment;
  size_t comment_len;
  unsigned char *setup;
  size_t setup_len;

  unsigned char selected;
} theora_stream;

struct ogg_theora_plan {
  FILE *source;

  theora_stream *streams;
  size_t stream_count;
  size_t stream_cap;

  theora_frame *frames;
  size_t frame_count;
  size_t frame_cap;

  theora_range *ranges;
  size_t range_count;
  size_t range_cap;

  unsigned char *ident;
  size_t ident_len;
  unsigned char *comment;
  size_t comment_len;
  unsigned char *setup;
  size_t setup_len;

  uint32_t shift;
  uint64_t total_frames;

  ogg_theora_cut *cuts;
  size_t cut_count;
};

/* Parse and validate plan->source from byte zero, filling in streams,
 * frames and ranges.  Does not look at plan->cuts. */
int theora_index_build(ogg_theora_plan *plan);

/* ((key_frame+1)<<shift) + (frame-key_frame), with overflow checking. */
int theora_calc_granule(uint32_t shift, uint64_t key_frame, uint64_t frame,
                        ogg_int64_t *out);

/* Read exactly expected_len bytes from the given source ranges into buffer. */
int theora_read_ranges(FILE *source, const theora_range *ranges, size_t count,
                       unsigned char *buffer, uint64_t expected_len,
                       size_t capacity);

#define THEORA_IDENT_MIN 42u

/* Free everything owned by the plan (not the plan itself and not source). */
void theora_plan_free_contents(ogg_theora_plan *plan);

#endif /* OGG_THEORA_INTERNAL_H */
