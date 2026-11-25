#include "smk_export.h"
#include "smk_avi.h"
#include "smacker.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMK_HEADER_AUDIO_BINK_BITS UINT32_C(0x0c000000)
#define SMK_HEADER_AUDIO_EXISTS UINT32_C(0x40000000)

struct smk_raw_header {
    uint32_t width;
    uint32_t height;
    uint32_t frames;
    uint32_t rate_word;
    uint32_t flags;
    uint32_t audio_rate[7];
};

struct audio_cursor {
    int used;
    unsigned original_track;
    smk decoder;

    unsigned long current_frame;
    const unsigned char *pcm;
    unsigned long available;

    uint64_t phase;
    uint64_t numerator;
    uint32_t denominator;
    uint32_t block_align;
    unsigned bits;
};

static unsigned long debug_pcm_peak;
static unsigned long long debug_pcm_streamed;

unsigned long smk_export_debug_pcm_resident_peak(void)
{
    return debug_pcm_peak;
}

unsigned long long smk_export_debug_pcm_streamed_bytes(void)
{
    return debug_pcm_streamed;
}

static void debug_observe_record(unsigned long available)
{
    if (available > debug_pcm_peak)
        debug_pcm_peak = available;
}

static int read_u32(FILE *fp, uint32_t *value)
{
    unsigned char b[4];
    if (fread(b, 1, sizeof b, fp) != sizeof b)
        return -1;
    *value = (uint32_t)b[0] |
             ((uint32_t)b[1] << 8) |
             ((uint32_t)b[2] << 16) |
             ((uint32_t)b[3] << 24);
    return 0;
}

/*
 * The decoder owns all SMK frame/audio decoding.  This only inspects fixed
 * header fields that are not exposed by the public compatibility API.
 */
static int read_and_check_header(const char *path, struct smk_raw_header *h,
                                 uint64_t *frame_duration)
{
    FILE *fp;
    unsigned char magic[4];
    unsigned i;
    int32_t signed_rate;
    uint32_t magnitude;

    fp = fopen(path, "rb");
    if (!fp)
        return -1;

    if (fread(magic, 1, sizeof magic, fp) != sizeof magic ||
        (memcmp(magic, "SMK2", 4) != 0 && memcmp(magic, "SMK4", 4) != 0))
        goto error;

    if (read_u32(fp, &h->width) || read_u32(fp, &h->height) ||
        read_u32(fp, &h->frames) || read_u32(fp, &h->rate_word) ||
        read_u32(fp, &h->flags))
        goto error;

    /* The seven maximum-audio-buffer fields. */
    for (i = 0; i < 7; ++i) {
        uint32_t dummy;
        if (read_u32(fp, &dummy))
            goto error;
    }

    /* Hufftree chunk size and four unpacked tree sizes. */
    for (i = 0; i < 5; ++i) {
        uint32_t dummy;
        if (read_u32(fp, &dummy))
            goto error;
    }

    for (i = 0; i < 7; ++i) {
        if (read_u32(fp, &h->audio_rate[i]))
            goto error;
    }

    if (fclose(fp) != 0)
        return -1;
    fp = NULL;

    if (h->width == 0 || h->height == 0 || (h->flags & UINT32_C(0x06)))
        return -1;

    for (i = 0; i < 7; ++i) {
        if ((h->audio_rate[i] & SMK_HEADER_AUDIO_EXISTS) &&
            (h->audio_rate[i] & SMK_HEADER_AUDIO_BINK_BITS))
            return -1;
    }

    signed_rate = (int32_t)h->rate_word;
    if (signed_rate > 0) {
        *frame_duration = (uint64_t)signed_rate * UINT64_C(1000);
    } else if (signed_rate < 0) {
        magnitude = (uint32_t)(-(int64_t)signed_rate);
        *frame_duration = (uint64_t)magnitude * UINT64_C(10);
    } else {
        *frame_duration = 100000;
    }
    if (*frame_duration == 0 || *frame_duration > UINT32_MAX)
        return -1;

    return 0;

error:
    if (fp)
        fclose(fp);
    return -1;
}

static uint64_t gcd_u64(uint64_t a, uint64_t b)
{
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* Returns 0 when PCM is available, 1 at source end, -1 on decoder failure. */
static int audio_refill(struct audio_cursor *a, unsigned long frame_count)
{
    while (a->available == 0) {
        int status;

        if (a->current_frame + 1 >= frame_count)
            return 1;

        status = smk_next(a->decoder);
        if (status < 0)
            return -1;

        ++a->current_frame;
        a->available = smk_get_audio_size(a->decoder,
                                          (unsigned char)a->original_track);
        a->pcm = smk_get_audio(a->decoder,
                               (unsigned char)a->original_track);
        if (a->available > 0 && !a->pcm)
            return -1;
        debug_observe_record(a->available);
    }
    return 0;
}

static int write_silence(smk_avi_mux *mux, uint64_t bytes, unsigned bits)
{
    unsigned char buffer[256];
    size_t chunk;

    memset(buffer, bits == 8 ? 0x80 : 0, sizeof buffer);
    while (bytes > 0) {
        chunk = bytes > sizeof buffer ? sizeof buffer : (size_t)bytes;
        if (smk_avi_write(mux, buffer, chunk))
            return -1;
        bytes -= chunk;
    }
    return 0;
}

static int write_audio_interval(smk_avi_mux *mux,
                                unsigned output_index,
                                struct audio_cursor *audio,
                                unsigned long frame_count)
{
    uint64_t sample_frames;
    uint64_t byte_count;
    uint32_t chunk_bytes;

    audio->phase += audio->numerator;
    sample_frames = audio->phase / audio->denominator;
    audio->phase %= audio->denominator;

    if (sample_frames > UINT32_MAX / audio->block_align)
        return -1;
    byte_count = sample_frames * audio->block_align;
    chunk_bytes = (uint32_t)byte_count;
    if (chunk_bytes == 0)
        return 0;

    debug_observe_record(audio->available);
    if (smk_avi_audio_begin(mux, output_index, chunk_bytes))
        return -1;

    while (byte_count > 0) {
        uint64_t take;
        int source_status;

        if (audio->available == 0) {
            source_status = audio_refill(audio, frame_count);
            if (source_status < 0)
                return -1;
            if (source_status > 0) {
                if (write_silence(mux, byte_count, audio->bits))
                    return -1;
                byte_count = 0;
                break;
            }
        }

        take = byte_count < audio->available ? byte_count : audio->available;
        if (take > (uint64_t)~(size_t)0)
            return -1;
        if (smk_avi_write(mux, audio->pcm, (size_t)take))
            return -1;

        debug_pcm_streamed += take;
        audio->pcm += (size_t)take;
        audio->available -= (unsigned long)take;
        byte_count -= take;
    }

    return smk_avi_chunk_end(mux);
}

int smk_export_avi(const char *input, const char *output, unsigned audio_mask)
{
    struct smk_raw_header raw;
    uint64_t frame_duration;
    uint64_t reduction;

    smk probe = NULL;
    smk video_decoder = NULL;
    struct audio_cursor audio[7];
    smk_avi_audio_format formats[7];
    smk_avi_mux *mux = NULL;
    unsigned char *row = NULL;

    unsigned char decoder_mask = 0;
    unsigned char channels[7];
    unsigned char bitdepth[7];
    unsigned long rates[7];

    unsigned long frame_count = 0;
    unsigned long video_width = 0;
    unsigned long video_height = 0;
    unsigned char y_mode = 0;
    double usf = 0.0;

    uint32_t width32, height32, stride, image_size;
    uint32_t reduced_scale, reduced_rate;
    unsigned output_audio_count = 0;
    unsigned i, j;
    int result = 1;
    int output_touched = 0;
    FILE *touch;

    memset(audio, 0, sizeof audio);
    memset(formats, 0, sizeof formats);

    if (!input || !output || (audio_mask & ~0x7fu))
        return 1;
    if (strcmp(input, output) == 0)
        return 1;

    debug_pcm_peak = 0;
    debug_pcm_streamed = 0;

    /* Establish the output before input validation; every later failure removes it. */
    touch = fopen(output, "wb");
    if (!touch)
        return 1;
    if (fclose(touch) != 0) {
        remove(output);
        return 1;
    }
    output_touched = 1;

    if (read_and_check_header(input, &raw, &frame_duration))
        goto cleanup;

    probe = smk_open_file(input, SMK_MODE_DISK);
    if (!probe)
        goto cleanup;

    if (smk_info_all(probe, NULL, &frame_count, NULL, &usf) ||
        smk_info_video(probe, &video_width, &video_height, &y_mode) ||
        smk_info_audio(probe, &decoder_mask, channels, bitdepth, rates))
        goto cleanup;

    if (frame_count != raw.frames || y_mode != SMK_FLAG_Y_NONE)
        goto cleanup;
    if (video_width == 0 || video_height == 0 ||
        video_width > UINT32_MAX || video_height > UINT32_MAX)
        goto cleanup;

    width32 = (uint32_t)video_width;
    height32 = (uint32_t)video_height;
    stride = smk_avi_bgr_stride(width32);
    if (!stride || (uint64_t)stride * height32 > UINT32_MAX)
        goto cleanup;
    image_size = stride * height32;

    reduction = gcd_u64(frame_duration, 1000000);
    reduced_scale = (uint32_t)(frame_duration / reduction);
    reduced_rate = (uint32_t)(1000000 / reduction);

    for (i = 0; i < 7; ++i) {
        if (!(audio_mask & (1u << i)) || !(decoder_mask & (1u << i)))
            continue;

        if (channels[i] != 1 && channels[i] != 2)
            goto cleanup;
        if (bitdepth[i] != 8 && bitdepth[i] != 16)
            goto cleanup;
        if (rates[i] == 0 || rates[i] > UINT32_MAX)
            goto cleanup;

        j = output_audio_count++;
        formats[j].samples_per_second = (uint32_t)rates[i];
        formats[j].channels = channels[i];
        formats[j].bits_per_sample = bitdepth[i];

        audio[j].used = 1;
        audio[j].original_track = i;
        audio[j].block_align = (uint32_t)channels[i] * (bitdepth[i] / 8u);
        audio[j].bits = bitdepth[i];
        audio[j].denominator = reduced_rate;
        audio[j].numerator = (uint64_t)rates[i] * reduced_scale;
    }

    smk_close(probe);
    probe = NULL;

    video_decoder = smk_open_file(input, SMK_MODE_DISK);
    if (!video_decoder || smk_enable_video(video_decoder, 1))
        goto cleanup;

    for (j = 0; j < output_audio_count; ++j) {
        struct audio_cursor *a = &audio[j];
        a->decoder = smk_open_file(input, SMK_MODE_DISK);
        if (!a->decoder)
            goto cleanup;
        if (smk_enable_all(a->decoder,
                           (unsigned char)(1u << a->original_track)))
            goto cleanup;
    }

    row = (unsigned char *)calloc(1, stride);
    if (!row)
        goto cleanup;

    if (frame_count > 0) {
        if (smk_first(video_decoder) < 0)
            goto cleanup;
        for (j = 0; j < output_audio_count; ++j) {
            struct audio_cursor *a = &audio[j];
            int status = smk_first(a->decoder);
            if (status < 0)
                goto cleanup;
            a->current_frame = 0;
            a->available = smk_get_audio_size(
                a->decoder, (unsigned char)a->original_track);
            a->pcm = smk_get_audio(
                a->decoder, (unsigned char)a->original_track);
            if (a->available > 0 && !a->pcm)
                goto cleanup;
            debug_observe_record(a->available);
        }
    }

    if (smk_avi_open(output, width32, height32, (uint32_t)frame_duration,
                     reduced_scale, reduced_rate, formats,
                     output_audio_count, &mux)) {
        mux = NULL;
        goto cleanup;
    }

    for (unsigned long frame = 0; frame < frame_count; ++frame) {
        const unsigned char *indexed;
        const unsigned char *palette;
        unsigned long src_row;
        uint32_t y, x;

        if (frame > 0 && smk_next(video_decoder) < 0)
            goto cleanup;

        indexed = smk_get_video(video_decoder);
        palette = smk_get_palette(video_decoder);
        if (!indexed || !palette)
            goto cleanup;

        if (smk_avi_video_begin(mux, image_size))
            goto cleanup;

        for (y = 0; y < height32; ++y) {
            src_row = (unsigned long)(height32 - 1u - y) * video_width;
            for (x = 0; x < width32; ++x) {
                unsigned char color_index = indexed[src_row + x];
                row[x * 3u + 0u] = palette[(unsigned)color_index * 3u + 2u];
                row[x * 3u + 1u] = palette[(unsigned)color_index * 3u + 1u];
                row[x * 3u + 2u] = palette[(unsigned)color_index * 3u + 0u];
            }
            if (smk_avi_write(mux, row, stride))
                goto cleanup;
        }
        if (smk_avi_chunk_end(mux))
            goto cleanup;

        for (j = 0; j < output_audio_count; ++j) {
            if (write_audio_interval(mux, j, &audio[j], frame_count))
                goto cleanup;
        }
    }

    if (smk_avi_finish(&mux))
        goto cleanup;

    result = 0;

cleanup:
    if (result != 0) {
        if (mux)
            smk_avi_abort(&mux);
        else if (output_touched)
            remove(output);
    }
    free(row);
    for (j = 0; j < output_audio_count; ++j) {
        if (audio[j].decoder)
            smk_close(audio[j].decoder);
    }
    if (video_decoder)
        smk_close(video_decoder);
    if (probe)
        smk_close(probe);
    return result;
}
