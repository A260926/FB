/* Include ma_crossfade.h after miniaudio.h */
#ifndef miniaudio_ma_crossfade_h
#define miniaudio_ma_crossfade_h

#include "../../miniaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
ma_crossfade

A data source that stitches a clip table of trimmed, seekable f32 data sources into one
addressable stream. Adjacent clips can overlap with an equal-power crossfade. Borrowed data
sources are never owned: the crossfade reads and seeks them exclusively while it exists, and
uninitializing the crossfade never uninitializes them.

Timeline geometry (all quantities in PCM frames):

  - Clip i has length L[i] and overlap N[i] frames with the clip that follows it. The last
    overlap must be 0.
  - Clip 0 starts at output frame 0; clip i+1 starts at start[i] + L[i] - N[i].
  - Total length = sum(L) - sum(N), always non-zero.
  - The two overlaps bordering any one clip may not sum past that clip's length, so three
    clips never sound at once.
  - N == 0 is a hard cut. Inside an overlap, output frame k (0..N-1) is

        theta = pi * (k + 0.5) / (2 * N)
        out   = old * cos(theta) + new * sin(theta)

    evaluated independently per channel, with no clipping. Everywhere else the samples of
    the single active clip pass through unchanged.

Borrowed sources must all report ma_format_f32, the same channel count (1 or 2) and the same
sample rate, must be seekable, and any one source pointer may appear at most once.
*/

/* Maximum number of frames pulled from either side of a transition in one internal step.
   Unpaired sample storage is bounded by this block and the channel count, never by the
   duration of the timeline. */
#define MA_CROSSFADE_BLOCK_FRAMES 256

typedef struct
{
    ma_data_source* pDataSource;  /* Borrowed. Never freed by the crossfade. */
    ma_uint64 clipStartInFrames;  /* First frame used inside the borrowed source. */
    ma_uint64 lengthInFrames;     /* L[i], must be non-zero. */
    ma_uint64 overlapInFrames;    /* N[i], overlap with the next clip; 0 for the last clip. */
} ma_crossfade_clip;

typedef struct
{
    ma_uint32 channels;           /* 1 or 2; 0 means "take it from the sources". */
    ma_uint32 sampleRate;         /* 0 means "take it from the sources". */
} ma_crossfade_config;

ma_crossfade_config ma_crossfade_config_init(void);

typedef struct
{
    ma_data_source_base base;
    ma_format format;
    ma_uint32 channels;
    ma_uint32 sampleRate;
    ma_uint32 clipCount;
    ma_crossfade_clip* pClips;             /* Deep copy of the caller's clip table. */
    ma_uint64* pClipOutputStarts;          /* Output frame at which each clip begins. */
    ma_uint64 totalLengthInFrames;
    ma_uint64 cursor;                      /* Next output frame to produce. */
    ma_uint32 currentClip;                 /* Clip owning the next output frame. */
    ma_uint64 clipPos;                     /* Frames already consumed from currentClip. */
    ma_uint32 nextClip;                    /* Clip whose head is paired in the cache. */
    ma_uint64 nextClipPos;                 /* Frames already consumed from nextClip. */
    ma_bool32 oldPositioned;               /* Current source cursor matches its next material frame. */
    ma_bool32 newPositioned;               /* Partner source cursor matches its next material frame. */
    ma_uint32 oldPendingFrames;            /* Unemitted frames held in pOldCache. */
    ma_uint32 newPendingFrames;            /* Unemitted frames held in pNewCache. */
    ma_uint32 cachedFrames;                /* Paired prefix of the two caches, ready to emit. */
    ma_uint32 cacheCapacityInFrames;
    float* pOldCache;                      /* Tail samples of the outgoing clip. */
    float* pNewCache;                      /* Head samples of the incoming clip. */
    ma_bool32 finished;                    /* Cursor has reached the total length. */
} ma_crossfade;

/*
Initializes a crossfade from a copied clip table. Table validation and format queries run before
any borrowed source is seeked. On failure *ppCrossfade is left untouched, every internal
allocation is released, and no borrowed source is ever freed.
*/
ma_result ma_crossfade_init(const ma_crossfade_config* pConfig, const ma_crossfade_clip* pClips, ma_uint32 clipCount, ma_crossfade** ppCrossfade);

/* Uninitializes the crossfade and frees it. Borrowed data sources are left alone. */
void ma_crossfade_uninit(ma_crossfade* pCrossfade);

#ifdef __cplusplus
}
#endif
#endif  /* miniaudio_ma_crossfade_h */
