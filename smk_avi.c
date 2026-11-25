#include "smk_avi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVI_MAX_STREAMS 8u
#define AVIIF_KEYFRAME 0x00000010u
#define AVIF_HASINDEX 0x00000010u
#define AVIF_ISINTERLEAVED 0x00000100u

struct avi_index_entry {
    unsigned char id[4];
    uint32_t flags;
    uint32_t offset;
    uint32_t length;
};

struct avi_stream_patch {
    long scale_pos;
    long rate_pos;
    long length_pos;
    long suggested_pos;
    long sample_size_pos;
    uint32_t scale;
    uint32_t rate;
    uint32_t sample_size;
    uint32_t block_align;
    uint32_t max_chunk;
    uint64_t data_bytes;
    uint32_t chunks;
    int is_audio;
};

struct smk_avi_mux {
    FILE *fp;
    char *path;

    uint64_t riff_size;
    uint64_t movi_offset;
    uint32_t width;
    uint32_t height;
    uint32_t image_size;

    long riff_size_pos;
    long hdrl_size_pos;
    uint32_t hdrl_size;
    long avih_frames_pos;
    long avih_streams_pos;
    long avih_suggested_pos;
    long movi_size_pos;

    unsigned stream_count;
    struct avi_stream_patch streams[AVI_MAX_STREAMS];

    struct avi_index_entry *index;
    size_t index_count;
    size_t index_capacity;

    int active;
    unsigned active_stream;
    uint32_t active_declared;
    uint32_t active_written;
    uint32_t active_offset;
};

void smk_avi_abort(smk_avi_mux **mp)
{
    smk_avi_mux *mux;
    if (!mp || !*mp)
        return;
    mux = *mp;
    if (mux->fp)
        fclose(mux->fp);
    if (mux->path) {
        remove(mux->path);
        free(mux->path);
    }
    free(mux->index);
    free(mux);
    *mp = NULL;
}

static int append_bytes(smk_avi_mux *mux, const void *data, size_t size)
{
    if (size > UINT32_MAX || mux->riff_size > UINT32_MAX - (uint64_t)size)
        return -1;
    if (size && fwrite(data, 1, size, mux->fp) != size)
        return -1;
    mux->riff_size += (uint64_t)size;
    return 0;
}

static int put_u16(smk_avi_mux *mux, uint16_t value)
{
    unsigned char b[2] = {
        (unsigned char)(value & 0xffu),
        (unsigned char)((value >> 8) & 0xffu)
    };
    return append_bytes(mux, b, sizeof b);
}

static int put_u32(smk_avi_mux *mux, uint32_t value)
{
    unsigned char b[4] = {
        (unsigned char)(value & 0xffu),
        (unsigned char)((value >> 8) & 0xffu),
        (unsigned char)((value >> 16) & 0xffu),
        (unsigned char)((value >> 24) & 0xffu)
    };
    return append_bytes(mux, b, sizeof b);
}

static int put_fourcc(smk_avi_mux *mux, const char fourcc[4])
{
    return append_bytes(mux, fourcc, 4);
}

static long current_pos(smk_avi_mux *mux)
{
    long pos = ftell(mux->fp);
    if (pos < 0)
        return -1;
    return pos;
}

static int patch_u32(smk_avi_mux *mux, long pos, uint32_t value)
{
    unsigned char b[4];
    if (pos < 0 || fseek(mux->fp, pos, SEEK_SET) != 0)
        return -1;
    b[0] = (unsigned char)(value & 0xffu);
    b[1] = (unsigned char)((value >> 8) & 0xffu);
    b[2] = (unsigned char)((value >> 16) & 0xffu);
    b[3] = (unsigned char)((value >> 24) & 0xffu);
    if (fwrite(b, 1, 4, mux->fp) != 4)
        return -1;
    return 0;
}

uint32_t smk_avi_bgr_stride(uint32_t width)
{
    uint64_t row = (uint64_t)width * 3u;
    if (row > UINT32_MAX)
        return 0;
    return (uint32_t)((row + 3u) & ~(uint64_t)3u);
}

static int write_avih(smk_avi_mux *mux, uint32_t frame_us)
{
    if (put_fourcc(mux, "avih") || put_u32(mux, 56))
        return -1;

    if (put_u32(mux, frame_us))          /* dwMicroSecPerFrame */
        return -1;
    if (put_u32(mux, 0))                 /* dwMaxBytesPerSec */
        return -1;
    if (put_u32(mux, 0))                 /* dwPaddingGranularity */
        return -1;
    if (put_u32(mux, AVIF_HASINDEX | AVIF_ISINTERLEAVED))
        return -1;

    mux->avih_frames_pos = current_pos(mux);
    if (mux->avih_frames_pos < 0 || put_u32(mux, 0))
        return -1;

    if (put_u32(mux, 0))                 /* dwInitialFrames */
        return -1;

    mux->avih_streams_pos = current_pos(mux);
    if (mux->avih_streams_pos < 0 || put_u32(mux, 0))
        return -1;

    mux->avih_suggested_pos = current_pos(mux);
    if (mux->avih_suggested_pos < 0 || put_u32(mux, 0))
        return -1;

    if (put_u32(mux, mux->width) || put_u32(mux, mux->height))
        return -1;
    if (put_u32(mux, 0) || put_u32(mux, 0) ||
        put_u32(mux, 0) || put_u32(mux, 0))
        return -1;
    return 0;
}

static int patch_strh_positions(smk_avi_mux *mux, unsigned stream)
{
    struct avi_stream_patch *sp = &mux->streams[stream];
    /* At this point the 56-byte strh data starts with fourcc/handler. */
    long base = current_pos(mux);
    if (base < 0)
        return -1;
    sp->scale_pos = base + 20;
    sp->rate_pos = base + 24;
    sp->length_pos = base + 32;
    sp->suggested_pos = base + 36;
    sp->sample_size_pos = base + 44;
    return 0;
}

static int write_video_strl(smk_avi_mux *mux,
                            uint32_t reduced_scale,
                            uint32_t reduced_rate)
{
    /* LIST size: 'strl' + strh(64) + strf(48). */
    if (put_fourcc(mux, "LIST") || put_u32(mux, 116) ||
        put_fourcc(mux, "strl"))
        return -1;

    if (put_fourcc(mux, "strh") || put_u32(mux, 56))
        return -1;
    if (patch_strh_positions(mux, 0))
        return -1;
    mux->streams[0].scale = reduced_scale;
    mux->streams[0].rate = reduced_rate;
    mux->streams[0].sample_size = 0;
    mux->streams[0].is_audio = 0;

    if (put_fourcc(mux, "vids"))
        return -1;
    if (put_fourcc(mux, "DIB "))
        return -1;
    if (put_u32(mux, 0) || put_u32(mux, 0) || put_u32(mux, 0))
        return -1;
    if (put_u32(mux, reduced_scale) || put_u32(mux, reduced_rate))
        return -1;
    if (put_u32(mux, 0) || put_u32(mux, 0) || put_u32(mux, 0))
        return -1;
    if (put_u32(mux, UINT32_C(0xffffffff))) /* quality */
        return -1;
    if (put_u32(mux, 0))                    /* dwSampleSize patched later */
        return -1;
    if (put_u16(mux, 0) || put_u16(mux, 0) ||
        put_u16(mux, (uint16_t)mux->width) ||
        put_u16(mux, (uint16_t)mux->height))
        return -1;

    if (put_fourcc(mux, "strf") || put_u32(mux, 40) || put_u32(mux, 40))
        return -1;
    if (put_u32(mux, mux->width) || put_u32(mux, mux->height))
        return -1;
    if (put_u16(mux, 1) || put_u16(mux, 24))
        return -1;
    if (put_u32(mux, 0))                    /* BI_RGB */
        return -1;
    if (put_u32(mux, mux->image_size))
        return -1;
    if (put_u32(mux, 0) || put_u32(mux, 0) ||
        put_u32(mux, 0) || put_u32(mux, 0))
        return -1;
    return 0;
}

static int write_audio_strl(smk_avi_mux *mux,
                            unsigned output_index,
                            const smk_avi_audio_format *fmt)
{
    struct avi_stream_patch *sp;
    uint32_t block_align, byte_rate;

    if (!fmt || (fmt->channels != 1 && fmt->channels != 2) ||
        (fmt->bits_per_sample != 8 && fmt->bits_per_sample != 16) ||
        fmt->samples_per_second == 0)
        return -1;

    block_align = (uint32_t)fmt->channels * (fmt->bits_per_sample / 8u);
    if (block_align == 0 ||
        fmt->samples_per_second > UINT32_MAX / block_align)
        return -1;
    byte_rate = fmt->samples_per_second * block_align;

    /* LIST size: 'strl' + strh(64) + strf(26). */
    if (put_fourcc(mux, "LIST") || put_u32(mux, 94) ||
        put_fourcc(mux, "strl"))
        return -1;

    if (put_fourcc(mux, "strh") || put_u32(mux, 56))
        return -1;
    if (patch_strh_positions(mux, output_index + 1))
        return -1;

    sp = &mux->streams[output_index + 1];
    sp->is_audio = 1;
    sp->block_align = block_align;
    sp->scale = block_align;
    sp->rate = byte_rate;
    sp->sample_size = block_align;

    if (put_fourcc(mux, "auds"))
        return -1;
    if (put_u32(mux, 0))                    /* handler */
        return -1;
    if (put_u32(mux, 0) || put_u32(mux, 0) || put_u32(mux, 0))
        return -1;
    if (put_u32(mux, block_align) || put_u32(mux, byte_rate))
        return -1;
    if (put_u32(mux, 0) || put_u32(mux, 0) || put_u32(mux, 0))
        return -1;
    if (put_u32(mux, UINT32_C(0xffffffff)))
        return -1;
    if (put_u32(mux, block_align))
        return -1;
    if (put_u32(mux, 0) || put_u32(mux, 0))
        return -1;

    if (put_fourcc(mux, "strf") || put_u32(mux, 18))
        return -1;
    if (put_u16(mux, 1))                    /* WAVE_FORMAT_PCM */
        return -1;
    if (put_u16(mux, fmt->channels))
        return -1;
    if (put_u32(mux, fmt->samples_per_second) || put_u32(mux, byte_rate))
        return -1;
    if (put_u16(mux, (uint16_t)block_align) ||
        put_u16(mux, fmt->bits_per_sample) ||
        put_u16(mux, 0))
        return -1;
    return 0;
}

int smk_avi_open(const char *path,
                 uint32_t width,
                 uint32_t height,
                 uint32_t frame_us,
                 uint32_t reduced_scale,
                 uint32_t reduced_rate,
                 const smk_avi_audio_format *audio,
                 unsigned audio_count,
                 smk_avi_mux **out)
{
    smk_avi_mux *mux;
    unsigned i;
    uint32_t stride;

    if (!out || !path || audio_count > 7 || (audio_count && !audio))
        return -1;
    if (width == 0 || height == 0 || reduced_scale == 0 || reduced_rate == 0)
        return -1;
    /* AVI's rcFrame uses signed 16-bit coordinates. */
    if (width > 32767u || height > 32767u)
        return -1;

    stride = smk_avi_bgr_stride(width);
    if (stride == 0 || (uint64_t)stride * height > UINT32_MAX)
        return -1;

    mux = (smk_avi_mux *)calloc(1, sizeof *mux);
    if (!mux)
        return -1;
    mux->path = NULL;
    mux->fp = fopen(path, "wb");
    if (!mux->fp) {
        free(mux);
        return -1;
    }
    mux->path = (char *)malloc(strlen(path) + 1);
    if (!mux->path) {
        fclose(mux->fp);
        free(mux);
        return -1;
    }
    strcpy(mux->path, path);

    mux->width = width;
    mux->height = height;
    mux->image_size = stride * height;
    mux->stream_count = audio_count + 1;
    mux->riff_size = 0;
    mux->movi_offset = 4;

    /* RIFF header. */
    if (put_fourcc(mux, "RIFF"))
        goto error;
    mux->riff_size_pos = current_pos(mux);
    if (mux->riff_size_pos < 0 || put_u32(mux, 0))
        goto error;
    if (put_fourcc(mux, "AVI "))
        goto error;
    /* put_* counts the RIFF size field too; logical RIFF size begins at 4. */
    mux->riff_size = 4;

    /* hdrl LIST with back-patched size. */
    if (put_fourcc(mux, "LIST"))
        goto error;
    mux->hdrl_size_pos = current_pos(mux);
    if (mux->hdrl_size_pos < 0 || put_u32(mux, 0))
        goto error;
    if (put_fourcc(mux, "hdrl"))
        goto error;
    if (write_avih(mux, frame_us))
        goto error;
    if (write_video_strl(mux, reduced_scale, reduced_rate))
        goto error;
    for (i = 0; i < audio_count; ++i) {
        if (write_audio_strl(mux, i, &audio[i]))
            goto error;
    }

    mux->hdrl_size = (uint32_t)(mux->riff_size - 4u - 8u);
    /* The preceding calculation is clearer as the bytes after LIST size. */
    mux->hdrl_size = 4 + 64 + 124 + audio_count * 102;

    /* movi LIST. */
    if (put_fourcc(mux, "LIST"))
        goto error;
    mux->movi_size_pos = current_pos(mux);
    if (mux->movi_size_pos < 0 || put_u32(mux, 0))
        goto error;
    if (put_fourcc(mux, "movi"))
        goto error;

    *out = mux;
    return 0;

error:
    smk_avi_abort(&mux);
    return -1;
}

static int begin_chunk(smk_avi_mux *mux,
                       unsigned stream,
                       const unsigned char id[4],
                       uint32_t declared_size)
{
    uint64_t added;
    uint32_t pad;

    if (!mux || mux->active || stream >= mux->stream_count)
        return -1;
    pad = declared_size & 1u;
    added = 8ull + declared_size + pad;

    if (mux->movi_offset > UINT32_MAX - added)
        return -1;
    if (mux->riff_size > UINT32_MAX - added)
        return -1;

    if (append_bytes(mux, id, 4) || put_u32(mux, declared_size))
        return -1;

    mux->active = 1;
    mux->active_stream = stream;
    mux->active_declared = declared_size;
    mux->active_written = 0;
    mux->active_offset = (uint32_t)mux->movi_offset;
    mux->movi_offset += added;
    return 0;
}

int smk_avi_video_begin(smk_avi_mux *mux, uint32_t declared_size)
{
    static const unsigned char id[4] = {'0', '0', 'd', 'b'};
    if (declared_size != mux->image_size)
        return -1;
    return begin_chunk(mux, 0, id, declared_size);
}

int smk_avi_audio_begin(smk_avi_mux *mux, unsigned audio_index, uint32_t declared_size)
{
    unsigned char id[4];
    if (audio_index >= mux->stream_count - 1)
        return -1;
    id[0] = (unsigned char)('0' + (audio_index + 1) / 10u);
    id[1] = (unsigned char)('0' + (audio_index + 1) % 10u);
    id[2] = 'w';
    id[3] = 'b';
    return begin_chunk(mux, audio_index + 1, id, declared_size);
}

int smk_avi_write(smk_avi_mux *mux, const void *data, size_t size)
{
    if (!mux || !mux->active)
        return -1;
    if ((uint64_t)mux->active_written + size > mux->active_declared)
        return -1;
    if (size && append_bytes(mux, data, size))
        return -1;
    mux->active_written += (uint32_t)size;
    return 0;
}

static int add_index_entry(smk_avi_mux *mux,
                           const unsigned char id[4],
                           uint32_t flags,
                           uint32_t offset,
                           uint32_t length)
{
    struct avi_index_entry *grown;
    size_t new_count = mux->index_count + 1;
    uint64_t index_bytes = new_count * 16ull;

    if (index_bytes > UINT32_MAX - 8u)
        return -1;
    /* Keep enough RIFF capacity for the idx1 chunk we will write later. */
    if (mux->riff_size > UINT32_MAX - (8ull + index_bytes))
        return -1;

    if (new_count > mux->index_capacity) {
        size_t new_capacity = mux->index_capacity ? mux->index_capacity * 2 : 16;
        while (new_capacity < new_count) {
            if (new_capacity > (size_t)-1 / 2)
                return -1;
            new_capacity *= 2;
        }
        grown = (struct avi_index_entry *)realloc(
            mux->index, new_capacity * sizeof *grown);
        if (!grown)
            return -1;
        mux->index = grown;
        mux->index_capacity = new_capacity;
    }

    memcpy(mux->index[mux->index_count].id, id, 4);
    mux->index[mux->index_count].flags = flags;
    mux->index[mux->index_count].offset = offset;
    mux->index[mux->index_count].length = length;
    mux->index_count = new_count;
    return 0;
}

int smk_avi_chunk_end(smk_avi_mux *mux)
{
    static const unsigned char pad_byte[1] = {0};
    unsigned char id[4];
    struct avi_stream_patch *sp;
    uint32_t pad;
    uint32_t flags = 0;

    if (!mux || !mux->active || mux->active_written != mux->active_declared)
        return -1;

    pad = mux->active_declared & 1u;
    if (pad && append_bytes(mux, pad_byte, 1))
        return -1;

    sp = &mux->streams[mux->active_stream];
    sp->chunks++;
    sp->data_bytes += mux->active_declared;
    if (mux->active_declared > sp->max_chunk)
        sp->max_chunk = mux->active_declared;

    if (mux->active_stream == 0) {
        memcpy(id, "00db", 4);
        flags = AVIIF_KEYFRAME;
    } else {
        unsigned n = mux->active_stream;
        id[0] = (unsigned char)('0' + n / 10u);
        id[1] = (unsigned char)('0' + n % 10u);
        memcpy(id + 2, "wb", 2);
    }

    if (add_index_entry(mux, id, flags, mux->active_offset,
                        mux->active_declared))
        return -1;

    mux->active = 0;
    return 0;
}

static int write_index(smk_avi_mux *mux)
{
    size_t i;
    uint32_t index_size = (uint32_t)(mux->index_count * 16u);

    if (mux->index_count * 16u > UINT32_MAX)
        return -1;
    if (put_fourcc(mux, "idx1") || put_u32(mux, index_size))
        return -1;

    for (i = 0; i < mux->index_count; ++i) {
        const struct avi_index_entry *e = &mux->index[i];
        if (put_fourcc(mux, (const char *)e->id) ||
            put_u32(mux, e->flags) ||
            put_u32(mux, e->offset) ||
            put_u32(mux, e->length))
            return -1;
    }
    return 0;
}

int smk_avi_finish(smk_avi_mux **mp)
{
    smk_avi_mux *mux;
    unsigned i;
    uint32_t global_buffer = 0;
    int rc = -1;

    if (!mp || !*mp)
        return -1;
    mux = *mp;
    if (mux->active)
        goto out;

    if (write_index(mux))
        goto out;

    if (mux->riff_size > UINT32_MAX ||
        patch_u32(mux, mux->riff_size_pos, (uint32_t)mux->riff_size))
        goto out;
    if (patch_u32(mux, mux->hdrl_size_pos, mux->hdrl_size))
        goto out;
    if (patch_u32(mux, mux->avih_frames_pos, mux->streams[0].chunks))
        goto out;
    if (patch_u32(mux, mux->avih_streams_pos, mux->stream_count))
        goto out;
    if (patch_u32(mux, mux->movi_size_pos, (uint32_t)mux->movi_offset))
        goto out;

    for (i = 0; i < mux->stream_count; ++i) {
        struct avi_stream_patch *sp = &mux->streams[i];
        uint32_t length;
        uint32_t sample_size;

        if (sp->max_chunk > global_buffer)
            global_buffer = sp->max_chunk;

        if (sp->is_audio) {
            if (sp->data_bytes % sp->block_align != 0 ||
                sp->data_bytes / sp->block_align > UINT32_MAX)
                goto out;
            length = (uint32_t)(sp->data_bytes / sp->block_align);
            sample_size = sp->block_align;
        } else {
            length = sp->chunks;
            sample_size = 0;
        }

        if (patch_u32(mux, sp->scale_pos, sp->scale) ||
            patch_u32(mux, sp->rate_pos, sp->rate) ||
            patch_u32(mux, sp->length_pos, length) ||
            patch_u32(mux, sp->suggested_pos, sp->max_chunk) ||
            patch_u32(mux, sp->sample_size_pos, sample_size))
            goto out;
    }

    if (patch_u32(mux, mux->avih_suggested_pos, global_buffer))
        goto out;

    if (fflush(mux->fp) != 0)
        goto out;
    if (fclose(mux->fp) != 0) {
        mux->fp = NULL;
        goto out;
    }
    mux->fp = NULL;
    free(mux->path);
    mux->path = NULL;
    free(mux->index);
    mux->index = NULL;
    free(mux);
    *mp = NULL;
    return 0;

out:
    if (mux->fp) {
        fclose(mux->fp);
        mux->fp = NULL;
    }
    return rc;
}
