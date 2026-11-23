/*
dr_mp3_stems - dual subband stem decoding sessions on top of dr_mp3.

A session decodes an in-memory MPEG-1 Layer III stream into two independent float
streams ("low" and "high") split by polyphase subband, keeps per-stem IMDCT/QMF
history, supports a time-varying cut schedule and PCM-frame seeking, and applies the
native gapless delay/padding trimming exactly once on the shared PCM clock.

Data is borrowed for the lifetime of the session; the cut schedule is copied.
*/
#ifndef dr_mp3_stems_h
#define dr_mp3_stems_h

#include "dr_mp3.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A schedule point: from absolute audio granule `granule` onwards, the low path owns
   subbands [0, low_subbands) and the high path owns [low_subbands, 32). Granules are
   counted from the start of audio (Xing/Info tag frames are not audio granules). */
typedef struct { drmp3_uint64 granule; unsigned low_subbands; } drmp3_stem_point;

typedef struct drmp3_stems drmp3_stems;

/* Opens a session. Returns NULL on any invalid argument. The schedule must be
   non-empty, strictly increasing on granule, start at granule 0, and every cut must
   be in 0..32. The input buffer is only borrowed and is never modified. */
drmp3_stems *drmp3_stems_open_memory(const void *data, size_t data_size,
                                     const drmp3_stem_point *points, size_t point_count);

void drmp3_stems_close(drmp3_stems *s);

/* Reads up to `frames` interleaved PCM frames into low and high (same frame count in
   each). Returns the number delivered; a zero-capacity read is allowed with NULL
   buffers and does not advance the session. Non-zero capacity requires both buffers. */
drmp3_uint64 drmp3_stems_read(drmp3_stems *s, drmp3_uint64 frames, float *low, float *high);

/* Seeks to a trimmed PCM frame index in 0..length(). Returns 1 on success, 0 on an
   out-of-range or invalid request, leaving the prior state untouched. Seeking to the
   end is legal and a later seek can leave EOF. */
int drmp3_stems_seek(drmp3_stems *s, drmp3_uint64 frame);

drmp3_uint64 drmp3_stems_tell(const drmp3_stems *s);
drmp3_uint64 drmp3_stems_length(const drmp3_stems *s);
unsigned drmp3_stems_channels(const drmp3_stems *s);
unsigned drmp3_stems_sample_rate(const drmp3_stems *s);

#ifdef __cplusplus
}
#endif

#endif /* dr_mp3_stems_h */
