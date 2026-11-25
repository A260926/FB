#ifndef SPEEX_PLAYOUT_H
#define SPEEX_PLAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque bounded offline playout state. */
typedef struct SpeexPlayout_ SpeexPlayout;

#define SPEEX_PLAYOUT_OK            0
#define SPEEX_PLAYOUT_BAD_ARGUMENT -1
#define SPEEX_PLAYOUT_BAD_STATE    -2
#define SPEEX_PLAYOUT_NO_MEMORY    -3
#define SPEEX_PLAYOUT_REJECTED     -4

/** Snapshot of playout counters and current rate control. */
typedef struct SpeexPlayoutStats_ {
    uint64_t source_consumed;  /**< Source frames eaten by the FIR */
    uint64_t rendered;         /**< Device frames generated and cached */
    uint64_t delivered;        /**< Device frames copied to callers */
    uint64_t concealed;        /**< Consumed source frames supplied as gaps */
    uint64_t quanta;           /**< Control quanta started */
    uint32_t media_timestamp;  /**< Truncated start + source_consumed */
    int32_t  correction_ppm;   /**< Current quantum correction u */
} SpeexPlayoutStats;

/**
 * Create a bounded offline playout state.
 *
 * @param channels 1 or 2
 * @param target_frames 480, 960, ..., 9600
 * @return New state, or NULL for invalid arguments/allocation failure
 */
SpeexPlayout *speex_playout_init(unsigned channels, unsigned target_frames);

/** Start (or restart) the session media timeline at start. */
int speex_playout_reset(SpeexPlayout *st, uint32_t start);

/** Copy and accept one interleaved float PCM packet. */
int speex_playout_put(SpeexPlayout *st, uint32_t timestamp,
                      const float *pcm, uint32_t frames);

/** Fill frames device samples, generating cached 480-frame quanta as needed. */
int speex_playout_pull(SpeexPlayout *st, float *pcm, uint32_t frames);

/** Copy a consistent statistics snapshot. */
int speex_playout_get_stats(SpeexPlayout *st, SpeexPlayoutStats *out);

/** Release all state. Safe to call with NULL. */
void speex_playout_destroy(SpeexPlayout *st);

#ifdef __cplusplus
}
#endif

#endif
