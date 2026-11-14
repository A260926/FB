/* Equal-power crossfading data source. See ma_crossfade.h for the contract. */
#include "ma_crossfade.h"

#include <math.h>
#include <stdint.h>
#include <string.h>  /* memset, memmove */

#define MA_CROSSFADE_PI 3.14159265358979323846

static ma_data_source_vtable g_ma_crossfade_vtable;

ma_crossfade_config ma_crossfade_config_init(void)
{
    ma_crossfade_config config;
    memset(&config, 0, sizeof(config));
    config.channels   = 0;
    config.sampleRate = 0;
    return config;
}

static ma_bool32 ma_crossfade_add_checked(ma_uint64 a, ma_uint64 b, ma_uint64* pOut)
{
    if (a > UINT64_MAX - b) {
        return MA_FALSE;
    }

    *pOut = a + b;
    return MA_TRUE;
}

/* Seeks a borrowed source to an absolute frame inside the source. */
static ma_result ma_crossfade_seek_source(ma_data_source* pDataSource, ma_uint64 frameIndex)
{
    return ma_data_source_seek_to_pcm_frame(pDataSource, frameIndex);
}

/*
Reads from the active clip at its recorded clip position. The source is positioned lazily so a
source that just became active is found at the right material frame before its first read.
A NULL output buffer discards the frames, exercising the same consumption path.
*/
static ma_result ma_crossfade_read_active(ma_crossfade* pCrossfade, ma_uint64 frameCount, float* pFramesOut, ma_uint64* pFramesRead)
{
    ma_crossfade_clip* pClip = &pCrossfade->pClips[pCrossfade->currentClip];
    ma_uint64 materialFrame;
    ma_result result;

    if (!ma_crossfade_add_checked(pClip->clipStartInFrames, pCrossfade->clipPos, &materialFrame)) {
        return MA_INVALID_ARGS;
    }

    if (!pCrossfade->oldPositioned) {
        result = ma_crossfade_seek_source(pClip->pDataSource, materialFrame);
        if (result != MA_SUCCESS) {
            return result;
        }

        pCrossfade->oldPositioned = MA_TRUE;
    }

    return ma_data_source_read_pcm_frames(pClip->pDataSource, pFramesOut, frameCount, pFramesRead);
}

/* Reads from the incoming clip at its recorded partner position. */
static ma_result ma_crossfade_read_partner(ma_crossfade* pCrossfade, ma_uint64 frameCount, float* pFramesOut, ma_uint64* pFramesRead)
{
    ma_crossfade_clip* pClip = &pCrossfade->pClips[pCrossfade->nextClip];
    ma_uint64 materialFrame;
    ma_result result;

    if (!ma_crossfade_add_checked(pClip->clipStartInFrames, pCrossfade->nextClipPos, &materialFrame)) {
        return MA_INVALID_ARGS;
    }

    if (!pCrossfade->newPositioned) {
        result = ma_crossfade_seek_source(pClip->pDataSource, materialFrame);
        if (result != MA_SUCCESS) {
            return result;
        }

        pCrossfade->newPositioned = MA_TRUE;
    }

    return ma_data_source_read_pcm_frames(pClip->pDataSource, pFramesOut, frameCount, pFramesRead);
}

/*
Appends one side of a transition to its pending cache. A short read is only survivable while the
source reports MA_BUSY: the delivered prefix is kept and never pulled again. MA_AT_END, a
zero-frame read, or any other failure means the trimmed material ended early, which is a data
error rather than a normal end of stream.
*/
static ma_result ma_crossfade_fill_side(ma_crossfade* pCrossfade, ma_bool32 isOldSide, ma_uint32 frameCount, ma_uint32* pPending, float* pCache)
{
    ma_uint64 framesRead = 0;
    ma_result result;
    float* pDst;

    if (frameCount == 0) {
        return MA_SUCCESS;
    }

    pDst = pCache + (size_t)(*pPending) * pCrossfade->channels;

    if (isOldSide) {
        result = ma_crossfade_read_active(pCrossfade, frameCount, pDst, &framesRead);
    } else {
        result = ma_crossfade_read_partner(pCrossfade, frameCount, pDst, &framesRead);
    }

    if (framesRead > frameCount) {
        return MA_ERROR;
    }

    if (framesRead < frameCount) {
        if (result != MA_BUSY) {
            return MA_ERROR;   /* Trimmed material ended early, or the source failed. */
        }
        /* MA_BUSY: keep the delivered prefix, which may be empty; the retry asks for the rest. */
    }

    *pPending += (ma_uint32)framesRead;
    return MA_SUCCESS;
}

/* Emits the already paired overlap prefix, mixing with the absolute transition phase. */
static ma_uint32 ma_crossfade_emit_cached(ma_crossfade* pCrossfade, float* pOut, ma_uint64 frameCount, ma_uint64 delivered, ma_uint64 overlapOutputStart, ma_uint64 overlapLength)
{
    ma_uint32 take = (ma_uint32)(frameCount - delivered);
    ma_uint32 t;
    const ma_uint32 channels = pCrossfade->channels;

    if (take > pCrossfade->cachedFrames) {
        take = pCrossfade->cachedFrames;
    }

    if (pOut != NULL) {
        for (t = 0; t < take; ++t) {
            /* Phase is derived from the absolute output position, so chunked reads never restart it. */
            ma_uint64 k = (pCrossfade->cursor + t) - overlapOutputStart;
            double theta = MA_CROSSFADE_PI * ((double)k + 0.5) / (2.0 * (double)overlapLength);
            float gainOld = (float)cos(theta);
            float gainNew = (float)sin(theta);
            ma_uint32 c;

            for (c = 0; c < channels; ++c) {
                pOut[(delivered + t) * channels + c] =
                    pCrossfade->pOldCache[t * channels + c] * gainOld +
                    pCrossfade->pNewCache[t * channels + c] * gainNew;
            }
        }
    }

    /* Compact by the emitted count: unpaired frames of the faster side sit beyond the paired
       prefix and must move to the front so the next fill appends after them. */
    memmove(pCrossfade->pOldCache, pCrossfade->pOldCache + (size_t)take * channels,
        (pCrossfade->oldPendingFrames - take) * channels * sizeof(float));
    memmove(pCrossfade->pNewCache, pCrossfade->pNewCache + (size_t)take * channels,
        (pCrossfade->newPendingFrames - take) * channels * sizeof(float));

    pCrossfade->cachedFrames     -= take;
    pCrossfade->oldPendingFrames -= take;
    pCrossfade->newPendingFrames -= take;
    pCrossfade->clipPos          += take;
    pCrossfade->nextClipPos      += take;
    pCrossfade->cursor           += take;

    return take;
}

/* Promotes the fully consumed incoming clip to the active clip. */
static void ma_crossfade_promote_partner(ma_crossfade* pCrossfade)
{
    pCrossfade->currentClip      = pCrossfade->nextClip;
    pCrossfade->clipPos          = pCrossfade->nextClipPos;
    pCrossfade->oldPositioned    = pCrossfade->newPositioned;  /* Same source, already positioned. */
    pCrossfade->newPositioned    = MA_FALSE;
    pCrossfade->nextClipPos      = 0;
    pCrossfade->cachedFrames     = 0;
    pCrossfade->oldPendingFrames = 0;
    pCrossfade->newPendingFrames = 0;
    if (pCrossfade->currentClip + 1 < pCrossfade->clipCount) {
        pCrossfade->nextClip = pCrossfade->currentClip + 1;
    }
}

static ma_result ma_crossfade_on_read(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead)
{
    ma_crossfade* pCrossfade = (ma_crossfade*)pDataSource;
    float* pOut = (float*)pFramesOut;
    ma_uint64 delivered = 0;
    ma_result result = MA_SUCCESS;

    if (pFramesRead != NULL) {
        *pFramesRead = 0;
    }

    if (frameCount == 0) {
        return MA_INVALID_ARGS;
    }

    if (pCrossfade->finished || pCrossfade->cursor >= pCrossfade->totalLengthInFrames) {
        pCrossfade->finished = MA_TRUE;
        return MA_AT_END;
    }

    while (delivered < frameCount && pCrossfade->cursor < pCrossfade->totalLengthInFrames) {
        ma_crossfade_clip* pClip = &pCrossfade->pClips[pCrossfade->currentClip];
        ma_uint64 tailBegin = pClip->lengthInFrames - pClip->overlapInFrames;

        /* Emit paired overlap frames that are already cached. */
        if (pCrossfade->cachedFrames > 0) {
            ma_uint64 overlapOutputStart = pCrossfade->pClipOutputStarts[pCrossfade->currentClip] + tailBegin;
            ma_uint32 take = ma_crossfade_emit_cached(pCrossfade, pOut, frameCount, delivered, overlapOutputStart, pClip->overlapInFrames);
            delivered += take;

            if (pCrossfade->clipPos == pClip->lengthInFrames) {
                ma_crossfade_promote_partner(pCrossfade);
            }

            continue;
        }

        if (pCrossfade->clipPos < tailBegin || pClip->overlapInFrames == 0) {
            /* Solo region: only the active clip sounds. Zero overlap means solo all the way. */
            ma_uint64 regionEnd = (pClip->overlapInFrames == 0) ? pClip->lengthInFrames : tailBegin;
            ma_uint64 want = regionEnd - pCrossfade->clipPos;
            ma_uint64 got = 0;
            float* pDst = NULL;

            if (want > frameCount - delivered) {
                want = frameCount - delivered;
            }

            if (pOut != NULL) {
                pDst = pOut + delivered * pCrossfade->channels;
            }

            result = ma_crossfade_read_active(pCrossfade, want, pDst, &got);
            if (got > want) {
                result = MA_ERROR;
                break;
            }

            if (got > 0) {
                pCrossfade->clipPos += got;
                pCrossfade->cursor  += got;
                delivered           += got;
            }

            if (got < want) {
                /* BUSY keeps the delivered prefix valid; anything else is a premature data end. */
                if (result != MA_BUSY) {
                    result = MA_ERROR;
                }
                break;
            }

            if (pCrossfade->clipPos == pClip->lengthInFrames && pClip->overlapInFrames == 0) {
                /* Hard cut: force the next source to seek on its first read. */
                if (pCrossfade->currentClip + 1 < pCrossfade->clipCount) {
                    pCrossfade->currentClip += 1;
                    pCrossfade->nextClip     = (pCrossfade->currentClip + 1 < pCrossfade->clipCount)
                                             ? pCrossfade->currentClip + 1
                                             : pCrossfade->currentClip;
                }
                pCrossfade->clipPos       = 0;
                pCrossfade->oldPositioned = MA_FALSE;
                pCrossfade->newPositioned = MA_FALSE;
                pCrossfade->nextClipPos   = 0;
            }
        } else {
            /* Overlap region with an empty cache: pull both sides, retaining unpaired prefixes. */
            ma_uint64 overlapConsumed = pCrossfade->clipPos - tailBegin;
            ma_uint64 overlapLeft = pClip->overlapInFrames - overlapConsumed;
            /* Frames not yet emitted minus frames already waiting in each side's cache. */
            ma_uint64 oldNeed = overlapLeft - pCrossfade->oldPendingFrames;
            ma_uint64 newNeed = overlapLeft - pCrossfade->newPendingFrames;
            ma_uint32 oldWant;
            ma_uint32 newWant;
            ma_uint32 paired;

            pCrossfade->nextClip = pCrossfade->currentClip + 1;

            if (oldNeed > MA_CROSSFADE_BLOCK_FRAMES - pCrossfade->oldPendingFrames) {
                oldNeed = MA_CROSSFADE_BLOCK_FRAMES - pCrossfade->oldPendingFrames;
            }
            if (newNeed > MA_CROSSFADE_BLOCK_FRAMES - pCrossfade->newPendingFrames) {
                newNeed = MA_CROSSFADE_BLOCK_FRAMES - pCrossfade->newPendingFrames;
            }

            oldWant = (ma_uint32)oldNeed;
            newWant = (ma_uint32)newNeed;

            result = ma_crossfade_fill_side(pCrossfade, MA_TRUE, oldWant, &pCrossfade->oldPendingFrames, pCrossfade->pOldCache);
            if (result != MA_SUCCESS) {
                break;
            }

            result = ma_crossfade_fill_side(pCrossfade, MA_FALSE, newWant, &pCrossfade->newPendingFrames, pCrossfade->pNewCache);
            if (result != MA_SUCCESS) {
                break;
            }

            paired = pCrossfade->oldPendingFrames;
            if (paired > pCrossfade->newPendingFrames) {
                paired = pCrossfade->newPendingFrames;
            }

            if (paired == 0) {
                /* Nothing submittable yet; return immediately instead of spinning. Prefixes stay cached. */
                result = MA_BUSY;
                break;
            }

            pCrossfade->cachedFrames = paired;
        }
    }

    if (pFramesRead != NULL) {
        *pFramesRead = delivered;
    }

    if (pCrossfade->cursor >= pCrossfade->totalLengthInFrames) {
        pCrossfade->finished = MA_TRUE;
        return MA_SUCCESS;   /* The next read returns MA_AT_END with zero frames. */
    }

    if (delivered > 0) {
        return result;      /* MA_BUSY here carries a usable prefix. */
    }

    return (result == MA_SUCCESS) ? MA_BUSY : result;
}

static ma_result ma_crossfade_on_seek(ma_data_source* pDataSource, ma_uint64 frameIndex)
{
    ma_crossfade* pCrossfade = (ma_crossfade*)pDataSource;
    ma_uint32 i;
    ma_uint32 clipIndex;
    ma_uint64 clipPos;
    ma_result result;

    /* Reject out-of-range seeks before touching any borrowed source. */
    if (frameIndex > pCrossfade->totalLengthInFrames) {
        return MA_INVALID_OPERATION;
    }

    /* Locate the clip owning the target frame. */
    clipIndex = pCrossfade->clipCount - 1;
    for (i = 0; i < pCrossfade->clipCount; ++i) {
        ma_uint64 start = pCrossfade->pClipOutputStarts[i];
        if (frameIndex >= start && frameIndex < start + pCrossfade->pClips[i].lengthInFrames) {
            clipIndex = i;
            break;
        }
    }

    clipPos = frameIndex - pCrossfade->pClipOutputStarts[clipIndex];

    /* Discard every unpaired sample and reset the rendering state. */
    pCrossfade->currentClip      = clipIndex;
    pCrossfade->clipPos          = clipPos;
    pCrossfade->cachedFrames     = 0;
    pCrossfade->oldPendingFrames = 0;
    pCrossfade->newPendingFrames = 0;
    pCrossfade->nextClipPos      = 0;
    pCrossfade->oldPositioned    = MA_FALSE;
    pCrossfade->newPositioned    = MA_FALSE;
    pCrossfade->finished         = (frameIndex == pCrossfade->totalLengthInFrames) ? MA_TRUE : MA_FALSE;
    pCrossfade->cursor           = frameIndex;
    pCrossfade->nextClip         = (clipIndex + 1 < pCrossfade->clipCount) ? clipIndex + 1 : clipIndex;

    /* Position the active source at the exact material frame. */
    {
        ma_uint64 materialFrame;
        if (!ma_crossfade_add_checked(pCrossfade->pClips[clipIndex].clipStartInFrames, clipPos, &materialFrame)) {
            return MA_INVALID_ARGS;
        }

        result = ma_crossfade_seek_source(pCrossfade->pClips[clipIndex].pDataSource, materialFrame);
        if (result != MA_SUCCESS) {
            return result;
        }

        pCrossfade->oldPositioned = MA_TRUE;
    }

    /* Inside an overlap the incoming source must be positioned too, at its own material frame,
       so rendering resumes mid-transition instead of re-fading from the transition start. */
    {
        ma_crossfade_clip* pClip = &pCrossfade->pClips[clipIndex];
        ma_uint64 tailBegin = pClip->lengthInFrames - pClip->overlapInFrames;

        if (pClip->overlapInFrames > 0 && clipPos >= tailBegin) {
            ma_uint64 partnerOffset = clipPos - tailBegin;
            ma_uint64 materialFrame;

            pCrossfade->nextClipPos = partnerOffset;

            if (!ma_crossfade_add_checked(pCrossfade->pClips[clipIndex + 1].clipStartInFrames, partnerOffset, &materialFrame)) {
                return MA_INVALID_ARGS;
            }

            result = ma_crossfade_seek_source(pCrossfade->pClips[clipIndex + 1].pDataSource, materialFrame);
            if (result != MA_SUCCESS) {
                return result;
            }

            pCrossfade->newPositioned = MA_TRUE;
        }
    }

    return MA_SUCCESS;
}

static ma_result ma_crossfade_on_get_data_format(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap)
{
    ma_crossfade* pCrossfade = (ma_crossfade*)pDataSource;

    if (pFormat != NULL) {
        *pFormat = pCrossfade->format;
    }
    if (pChannels != NULL) {
        *pChannels = pCrossfade->channels;
    }
    if (pSampleRate != NULL) {
        *pSampleRate = pCrossfade->sampleRate;
    }
    if (pChannelMap != NULL && channelMapCap > 0) {
        ma_channel_map_init_standard(ma_standard_channel_map_default, pChannelMap, channelMapCap, pCrossfade->channels);
    }

    return MA_SUCCESS;
}

static ma_result ma_crossfade_on_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor)
{
    ma_crossfade* pCrossfade = (ma_crossfade*)pDataSource;
    *pCursor = pCrossfade->cursor;
    return MA_SUCCESS;
}

static ma_result ma_crossfade_on_get_length(ma_data_source* pDataSource, ma_uint64* pLength)
{
    ma_crossfade* pCrossfade = (ma_crossfade*)pDataSource;
    *pLength = pCrossfade->totalLengthInFrames;
    return MA_SUCCESS;
}

static ma_data_source_vtable g_ma_crossfade_vtable =
{
    ma_crossfade_on_read,
    ma_crossfade_on_seek,
    ma_crossfade_on_get_data_format,
    ma_crossfade_on_get_cursor,
    ma_crossfade_on_get_length,
    NULL,   /* onSetLooping */
    0
};

ma_result ma_crossfade_init(const ma_crossfade_config* pConfig, const ma_crossfade_clip* pClips, ma_uint32 clipCount, ma_crossfade** ppCrossfade)
{
    ma_crossfade* pCrossfade = NULL;
    ma_crossfade_config config;
    ma_result result;
    ma_uint32 i;
    ma_format firstFormat = ma_format_unknown;
    ma_uint32 firstChannels = 0;
    ma_uint32 firstSampleRate = 0;
    ma_uint64 runningStart = 0;
    size_t cacheBytes;
    ma_bool32 baseInitialized = MA_FALSE;

    if (pClips == NULL || ppCrossfade == NULL || clipCount == 0) {
        return MA_INVALID_ARGS;
    }

    config = (pConfig != NULL) ? *pConfig : ma_crossfade_config_init();

    pCrossfade = (ma_crossfade*)ma_malloc(sizeof(*pCrossfade), NULL);
    if (pCrossfade == NULL) {
        return MA_OUT_OF_MEMORY;
    }

    memset(pCrossfade, 0, sizeof(*pCrossfade));

    pCrossfade->pClips = (ma_crossfade_clip*)ma_malloc(sizeof(ma_crossfade_clip) * clipCount, NULL);
    if (pCrossfade->pClips == NULL) {
        ma_free(pCrossfade, NULL);
        return MA_OUT_OF_MEMORY;
    }

    pCrossfade->pClipOutputStarts = (ma_uint64*)ma_malloc(sizeof(ma_uint64) * clipCount, NULL);
    if (pCrossfade->pClipOutputStarts == NULL) {
        ma_free(pCrossfade->pClips, NULL);
        ma_free(pCrossfade, NULL);
        return MA_OUT_OF_MEMORY;
    }

    for (i = 0; i < clipCount; ++i) {
        pCrossfade->pClips[i] = pClips[i];   /* Deep copy of the table; sources stay borrowed. */
    }

    /* Table-level validation. */
    for (i = 0; i < clipCount; ++i) {
        ma_crossfade_clip* pClip = &pCrossfade->pClips[i];
        ma_uint32 j;
        ma_uint64 borderSum;
        ma_uint64 materialEnd;

        if (pClip->pDataSource == NULL || pClip->lengthInFrames == 0) {
            result = MA_INVALID_ARGS;
            goto failure;
        }

        if (i == clipCount - 1) {
            if (pClip->overlapInFrames != 0) {
                result = MA_INVALID_ARGS;  /* The last clip cannot overlap a successor. */
                goto failure;
            }
        }

        /* The overlaps bordering this clip may not exceed its own length. */
        borderSum = pClip->overlapInFrames;
        if (i > 0) {
            if (!ma_crossfade_add_checked(borderSum, pCrossfade->pClips[i - 1].overlapInFrames, &borderSum)) {
                result = MA_INVALID_ARGS;
                goto failure;
            }
        }
        if (borderSum > pClip->lengthInFrames) {
            result = MA_INVALID_ARGS;
            goto failure;
        }

        /* One borrowed source pointer may only appear once. */
        for (j = 0; j < i; ++j) {
            if (pCrossfade->pClips[j].pDataSource == pClip->pDataSource) {
                result = MA_INVALID_ARGS;
                goto failure;
            }
        }

        /* Material window must not overflow. */
        if (!ma_crossfade_add_checked(pClip->clipStartInFrames, pClip->lengthInFrames, &materialEnd)) {
            result = MA_INVALID_ARGS;
            goto failure;
        }
    }

    /* All sources must agree on f32, channel count (1 or 2) and sample rate. */
    for (i = 0; i < clipCount; ++i) {
        ma_format format;
        ma_uint32 channels = 0;
        ma_uint32 sampleRate = 0;

        result = ma_data_source_get_data_format(pCrossfade->pClips[i].pDataSource, &format, &channels, &sampleRate, NULL, 0);
        if (result != MA_SUCCESS) {
            goto failure;
        }

        if (format != ma_format_f32 || (channels != 1 && channels != 2)) {
            result = MA_INVALID_ARGS;
            goto failure;
        }

        if (i == 0) {
            firstFormat     = format;
            firstChannels   = channels;
            firstSampleRate = sampleRate;
        } else if (format != firstFormat || channels != firstChannels || sampleRate != firstSampleRate) {
            result = MA_INVALID_ARGS;
            goto failure;
        }
    }

    if (config.channels != 0 && config.channels != firstChannels) {
        result = MA_INVALID_ARGS;
        goto failure;
    }
    if (config.sampleRate != 0 && config.sampleRate != firstSampleRate) {
        result = MA_INVALID_ARGS;
        goto failure;
    }

    /* Timeline geometry with overflow checks. */
    pCrossfade->clipCount = clipCount;
    for (i = 0; i < clipCount; ++i) {
        ma_uint64 extended;

        pCrossfade->pClipOutputStarts[i] = runningStart;

        if (i + 1 < clipCount) {
            if (!ma_crossfade_add_checked(runningStart, pCrossfade->pClips[i].lengthInFrames, &extended)) {
                result = MA_INVALID_ARGS;
                goto failure;
            }
            /* Border validation above guarantees N[i] <= L[i], so this cannot underflow. */
            runningStart = extended - pCrossfade->pClips[i].overlapInFrames;
        }
    }

    if (!ma_crossfade_add_checked(runningStart, pCrossfade->pClips[clipCount - 1].lengthInFrames, &pCrossfade->totalLengthInFrames)) {
        result = MA_INVALID_ARGS;
        goto failure;
    }
    if (pCrossfade->totalLengthInFrames == 0) {
        result = MA_INVALID_ARGS;
        goto failure;
    }

    pCrossfade->format       = firstFormat;
    pCrossfade->channels     = firstChannels;
    pCrossfade->sampleRate   = firstSampleRate;
    pCrossfade->cursor       = 0;
    pCrossfade->currentClip  = 0;
    pCrossfade->clipPos      = 0;
    pCrossfade->nextClip     = (clipCount > 1) ? 1 : 0;
    pCrossfade->nextClipPos  = 0;
    pCrossfade->cacheCapacityInFrames = MA_CROSSFADE_BLOCK_FRAMES;

    cacheBytes = (size_t)MA_CROSSFADE_BLOCK_FRAMES * pCrossfade->channels * sizeof(float);
    pCrossfade->pOldCache = (float*)ma_malloc(cacheBytes, NULL);
    if (pCrossfade->pOldCache == NULL) {
        result = MA_OUT_OF_MEMORY;
        goto failure;
    }

    pCrossfade->pNewCache = (float*)ma_malloc(cacheBytes, NULL);
    if (pCrossfade->pNewCache == NULL) {
        result = MA_OUT_OF_MEMORY;
        goto failure;
    }

    {
        ma_data_source_config dataSourceConfig = ma_data_source_config_init();
        dataSourceConfig.vtable = &g_ma_crossfade_vtable;
        result = ma_data_source_init(&dataSourceConfig, &pCrossfade->base);
        if (result != MA_SUCCESS) {
            goto failure;
        }
        baseInitialized = MA_TRUE;
    }

    /* Start the first source at the first borrowed material frame. */
    result = ma_crossfade_seek_source(pCrossfade->pClips[0].pDataSource, pCrossfade->pClips[0].clipStartInFrames);
    if (result != MA_SUCCESS) {
        goto failure;
    }
    pCrossfade->oldPositioned = MA_TRUE;

    *ppCrossfade = pCrossfade;
    return MA_SUCCESS;

failure:
    if (baseInitialized) {
        ma_data_source_uninit(&pCrossfade->base);
    }
    ma_free(pCrossfade->pNewCache, NULL);
    ma_free(pCrossfade->pOldCache, NULL);
    ma_free(pCrossfade->pClipOutputStarts, NULL);
    ma_free(pCrossfade->pClips, NULL);
    ma_free(pCrossfade, NULL);
    return result;
}

void ma_crossfade_uninit(ma_crossfade* pCrossfade)
{
    if (pCrossfade == NULL) {
        return;
    }

    ma_data_source_uninit(&pCrossfade->base);
    ma_free(pCrossfade->pNewCache, NULL);
    ma_free(pCrossfade->pOldCache, NULL);
    ma_free(pCrossfade->pClipOutputStarts, NULL);
    ma_free(pCrossfade->pClips, NULL);
    ma_free(pCrossfade, NULL);
}
