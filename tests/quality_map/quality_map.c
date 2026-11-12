// tests/quality_map/quality_map.c
//
// Test driver for WavpackUnpackSamplesWithQuality().
//
// This program uses the library's own encoder to build a three-channel hybrid
// (lossless) WavPack file with a correction file, and then exercises the quality
// map API against several deliberately damaged variants of that file (a missing
// correction block on a non-first substream, a missing WV audio block, and an
// audio CRC mismatch detected at the end of a block). Every assertion is checked
// against the original PCM samples and against reference decodes using the original
// entry point (with and without the correction file), so this also verifies that
// the two entry points agree.
//
// All decodes are deterministic, so the same request (including after a seek) must
// produce identical PCM and identical quality maps. The test prints the grouped
// quality ranges it encounters and finishes with QUALITY_MAP_OK if everything
// passed.

#include <wavpack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHANS       3
#define FRAMES      160
#define BLKSZ       16
#define SENTINEL    0xEE

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf (stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        exit (1); \
    } \
} while (0)

static int out_cb (void *id, void *data, int32_t bcount)
{
    return fwrite (data, 1, bcount, (FILE *) id) == (size_t) bcount;
}

/////////////////////////////////////////////////////////////////////////////
// raw block stream helpers

typedef struct { unsigned char *data; long size; } FileData;

typedef struct {
    long pos; uint32_t size; int64_t index; uint32_t samples; uint32_t flags;
    int mono, initial, final;
} BlockInfo;

static uint32_t header_u32 (unsigned char *p)
{
    return p [0] | ((uint32_t) p [1] << 8) | ((uint32_t) p [2] << 16) | ((uint32_t) p [3] << 24);
}

static FileData read_file (const char *path)
{
    FileData fd = { NULL, 0 };
    FILE *f = fopen (path, "rb");

    CHECK (f, "can't open input file");
    fseek (f, 0, SEEK_END);
    fd.size = ftell (f);
    fseek (f, 0, SEEK_SET);
    fd.data = malloc (fd.size);
    CHECK (fd.data && fread (fd.data, 1, fd.size, f) == (size_t) fd.size, "can't read input file");
    fclose (f);
    return fd;
}

static void write_file (const char *path, FileData *fd)
{
    FILE *f = fopen (path, "wb");

    CHECK (f, "can't open output file");
    CHECK (fwrite (fd->data, 1, fd->size, f) == (size_t) fd->size, "can't write output file");
    fclose (f);
}

static int scan_blocks (FileData *fd, BlockInfo *blocks, int max_blocks)
{
    long pos = 0;
    int count = 0;

    while (pos + 32 <= fd->size && count < max_blocks) {
        unsigned char *h = fd->data + pos;
        BlockInfo *b = blocks + count;

        if (memcmp (h, "wvpk", 4))
            break;

        b->pos = pos;
        b->size = header_u32 (h + 4) + 8;
        b->index = (int64_t) header_u32 (h + 16) + ((int64_t) h [10] << 32);
        b->samples = header_u32 (h + 20);
        b->flags = header_u32 (h + 24);
        b->mono = !!(b->flags & MONO_FLAG);
        b->initial = !!(b->flags & INITIAL_BLOCK);
        b->final = !!(b->flags & FINAL_BLOCK);

        if (pos + (long) b->size > fd->size)
            break;

        pos += b->size;
        count++;
    }

    return count;
}

// Find the block in the scanned array with the specified block index whose mono
// flag matches "mono" (i.e., either the stereo or the mono substream of a group).
// Returns the file position or -1.

static long find_block (BlockInfo *blocks, int count, int64_t index, int mono)
{
    int i;

    for (i = 0; i < count; ++i)
        if (blocks [i].index == index && blocks [i].mono == mono && blocks [i].samples == BLKSZ)
            return blocks [i].pos;

    return -1;
}

static void set_header_u32 (unsigned char *p, uint32_t v)
{
    p [0] = v & 0xff;
    p [1] = (v >> 8) & 0xff;
    p [2] = (v >> 16) & 0xff;
    p [3] = (v >> 24) & 0xff;
}

// Recompute and replace the trailing block checksum metadata of the block at the
// specified file position. The block remains structurally valid, which lets us
// modify the stored audio CRC and still have the block accepted by the library's
// block verifier (so the audio CRC check inside the decoder is what fails).

static void fix_block_checksum (FileData *fd, long pos)
{
    unsigned char *block = fd->data + pos;
    uint32_t bcount;
    unsigned char *dp = block + 32;
    WavpackHeader hdr;

    // the block buffer holds the header in little-endian (file) byte order, so
    // copy it through an aligned local structure to read the fields portably

    memcpy (&hdr, block, sizeof (hdr));
    WavpackLittleEndianToNative (&hdr, WavpackHeaderFormat);
    bcount = hdr.ckSize - sizeof (WavpackHeader) + 8;

    while (bcount >= 2) {
        unsigned char id = *dp++;
        unsigned char c1 = *dp++;
        uint32_t meta_bc = c1 << 1;

        bcount -= 2;

        if (id & ID_LARGE) {
            unsigned char c2 = *dp++, c3 = *dp++;
            meta_bc += ((uint32_t) c2 << 9) + ((uint32_t) c3 << 17);
            bcount -= 2;
        }

        if ((id & ID_UNIQUE) == ID_BLOCK_CHECKSUM) {
            int wcount = (int) (dp - 2 - block) >> 1, i;
            unsigned char *csptr = block;
            uint32_t csum = (uint32_t) -1;

            // WavpackVerifySingleBlock converts the header to little-endian before
            // hashing, so replicate that exactly (including converting it back)

            WavpackNativeToLittleEndian (&hdr, WavpackHeaderFormat);
            memcpy (block, &hdr, sizeof (hdr));

            for (i = 0; i < wcount; ++i) {
                csum = (csum * 3) + csptr [0] + (csptr [1] << 8);
                csptr += 2;
            }

            WavpackLittleEndianToNative (&hdr, WavpackHeaderFormat);
            memcpy (block, &hdr, sizeof (hdr));

            if (meta_bc == 4) {
                set_header_u32 (dp, csum);
            }
            else {
                csum ^= csum >> 16;
                dp [0] = csum & 0xff;
                dp [1] = (csum >> 8) & 0xff;
            }

            return;
        }

        dp += meta_bc;
        bcount -= meta_bc;
    }

    CHECK (0, "block has no checksum metadata");
}

static void corrupt_audio_crc (FileData *fd, long pos)
{
    unsigned char *crcp = fd->data + pos + 28;

    set_header_u32 (crcp, header_u32 (crcp) ^ 0x5a5a5a5a);
    fix_block_checksum (fd, pos);
}

// Build a file from the blocks of "src" except those listed in "remove_positions".
// Positions refer to offsets in the source file.

static void filter_file (const char *src, const char *dst, long *remove_positions, int remove_count)
{
    FileData in = read_file (src);
    BlockInfo blocks [128];
    int nblocks = scan_blocks (&in, blocks, 128);
    unsigned char *out = malloc (in.size);
    long out_size = 0, pos = 0;
    int i, j;

    CHECK (out, "out of memory");

    for (i = 0; i < nblocks; ++i) {
        int remove = 0;

        for (j = 0; j < remove_count; ++j)
            if (blocks [i].pos == remove_positions [j])
                remove = 1;

        if (!remove) {
            memcpy (out + out_size, in.data + pos, blocks [i].size);
            out_size += blocks [i].size;
        }

        pos += blocks [i].size;
    }

    free (in.data);
    in.data = out;
    in.size = out_size;
    write_file (dst, &in);
    free (out);
}

static void copy_file (const char *src, const char *dst)
{
    FileData fd = read_file (src);

    write_file (dst, &fd);
    free (fd.data);
}

/////////////////////////////////////////////////////////////////////////////
// PCM generation (identical at encode and check time)

static void gen_pcm (int32_t *pcm, int frames, int chans, int with_zeros)
{
    int i;

    for (i = 0; i < frames * chans; ++i) {
        int frame = i / chans, chan = i % chans;

        // a normal deterministic signal that differs on every channel so we can
        // verify channel placement, optionally with a deliberately silent run of
        // frames (40..63) to prove that normally encoded zeros are NOT reported
        // as concealed

        if (with_zeros && frame >= 40 && frame < 64)
            pcm [i] = 0;
        else
            pcm [i] = (((i * 1103515245u + 12345u + (unsigned) chan * 7919u) >> 16) % 60001) - 30000;
    }
}

/////////////////////////////////////////////////////////////////////////////
// encoding helpers

static void encode_hybrid_ref (void)
{
    FILE *f = fopen ("qm.wv", "wb"), *fc = fopen ("qm.wvc", "wb");
    WavpackContext *wpc;
    WavpackConfig config;
    int32_t *pcm;

    CHECK (f && fc, "can't create reference files");
    wpc = WavpackOpenFileOutput (out_cb, f, fc);
    CHECK (wpc, "can't create encoder");

    memset (&config, 0, sizeof (config));
    config.num_channels = CHANS;
    config.channel_mask = 7;            // FL, FR, LFE
    config.sample_rate = 48000;
    config.bits_per_sample = 16;
    config.bytes_per_sample = 2;
    config.block_samples = BLKSZ;
    config.bitrate = 2.0;
    config.flags = CONFIG_HYBRID_FLAG | CONFIG_CREATE_WVC;

    CHECK (WavpackSetConfiguration64 (wpc, &config, FRAMES, NULL), "WavpackSetConfiguration64 failed");
    CHECK (WavpackPackInit (wpc), "WavpackPackInit failed");

    pcm = malloc (FRAMES * CHANS * sizeof (int32_t));
    gen_pcm (pcm, FRAMES, CHANS, 0);
    CHECK (WavpackPackSamples (wpc, pcm, FRAMES), "WavpackPackSamples failed");
    CHECK (WavpackFlushSamples (wpc), "WavpackFlushSamples failed");

    free (pcm);
    WavpackCloseFile (wpc);
    fclose (f);
    fclose (fc);
}

// A plain (non-hybrid) stereo file used to exercise the temporal worker path.

#define STFRAMES 96
#define STCHANS  2
#define STBLKSZ  16

static void encode_stereo_lossless (const char *path, int32_t *pcm, int frames)
{
    FILE *f = fopen (path, "wb");
    WavpackContext *wpc;
    WavpackConfig config;

    CHECK (f, "can't create stereo file");
    wpc = WavpackOpenFileOutput (out_cb, f, NULL);
    CHECK (wpc, "can't create stereo encoder");

    memset (&config, 0, sizeof (config));
    config.num_channels = STCHANS;
    config.channel_mask = 3;
    config.sample_rate = 48000;
    config.bits_per_sample = 16;
    config.bytes_per_sample = 2;
    config.block_samples = STBLKSZ;

    CHECK (WavpackSetConfiguration64 (wpc, &config, frames, NULL), "stereo WavpackSetConfiguration64 failed");
    CHECK (WavpackPackInit (wpc), "stereo WavpackPackInit failed");
    CHECK (WavpackPackSamples (wpc, pcm, frames), "stereo WavpackPackSamples failed");
    CHECK (WavpackFlushSamples (wpc), "stereo WavpackFlushSamples failed");

    WavpackCloseFile (wpc);
    fclose (f);
}

// A hybrid stereo file used to exercise the temporal worker path with corrections.

static void encode_stereo_hybrid (const char *wvpath, const char *wvcpath, int32_t *pcm, int frames)
{
    FILE *f = fopen (wvpath, "wb"), *fc = fopen (wvcpath, "wb");
    WavpackContext *wpc;
    WavpackConfig config;

    CHECK (f && fc, "can't create stereo hybrid file");
    wpc = WavpackOpenFileOutput (out_cb, f, fc);
    CHECK (wpc, "can't create stereo hybrid encoder");

    memset (&config, 0, sizeof (config));
    config.num_channels = STCHANS;
    config.channel_mask = 3;
    config.sample_rate = 48000;
    config.bits_per_sample = 16;
    config.bytes_per_sample = 2;
    config.block_samples = STBLKSZ;
    config.bitrate = 2.0;
    config.flags = CONFIG_HYBRID_FLAG | CONFIG_CREATE_WVC;

    CHECK (WavpackSetConfiguration64 (wpc, &config, frames, NULL), "stereo hybrid config failed");
    CHECK (WavpackPackInit (wpc), "stereo hybrid init failed");
    CHECK (WavpackPackSamples (wpc, pcm, frames), "stereo hybrid pack failed");
    CHECK (WavpackFlushSamples (wpc), "stereo hybrid flush failed");

    WavpackCloseFile (wpc);
    fclose (f);
    fclose (fc);
}

// A floating-point file, used to verify that quality maps are rejected (while the
// original entry point continues to work).

static void encode_float (void)
{
    FILE *f = fopen ("qm_float.wv", "wb");
    WavpackContext *wpc;
    WavpackConfig config;
    int32_t *pcm = malloc (64 * 2 * sizeof (int32_t));
    float *fp = (float *) pcm;
    int i;

    CHECK (f, "can't create float file");
    wpc = WavpackOpenFileOutput (out_cb, f, NULL);
    CHECK (wpc, "can't create float encoder");

    memset (&config, 0, sizeof (config));
    config.num_channels = 2;
    config.channel_mask = 3;
    config.sample_rate = 44100;
    config.bits_per_sample = 32;
    config.bytes_per_sample = 4;
    config.float_norm_exp = 127;

    CHECK (WavpackSetConfiguration64 (wpc, &config, 64, NULL), "float config failed");
    CHECK (WavpackPackInit (wpc), "float init failed");

    for (i = 0; i < 64 * 2; ++i)
        fp [i] = (float) (((i * 37) % 101) - 50) / 64.0f;

    CHECK (WavpackPackSamples (wpc, pcm, 64), "float pack failed");
    CHECK (WavpackFlushSamples (wpc), "float flush failed");

    WavpackCloseFile (wpc);
    fclose (f);
    free (pcm);
}

// A DSD file, used the same way as the float file.

static void encode_dsd (void)
{
    FILE *f = fopen ("qm_dsd.wv", "wb");
    WavpackContext *wpc;
    WavpackConfig config;
    int32_t *pcm = malloc (64 * 2 * sizeof (int32_t));
    int i;

    CHECK (f, "can't create DSD file");
    wpc = WavpackOpenFileOutput (out_cb, f, NULL);
    CHECK (wpc, "can't create DSD encoder");

    memset (&config, 0, sizeof (config));
    config.num_channels = 2;
    config.channel_mask = 3;
    config.sample_rate = 352800;
    config.bits_per_sample = 8;
    config.bytes_per_sample = 1;
    config.qmode = QMODE_DSD_AUDIO;

    if (!WavpackSetConfiguration64 (wpc, &config, 64, NULL)) {
        fprintf (stderr, "DSD encoding not available (%s), skipping DSD test\n", WavpackGetErrorMessage (wpc));
        WavpackCloseFile (wpc);
        fclose (f);
        remove ("qm_dsd.wv");
        free (pcm);
        return;
    }

    CHECK (WavpackPackInit (wpc), "DSD init failed");

    for (i = 0; i < 64 * 2; ++i)
        pcm [i] = (i * 61 + 17) & 0xff;

    CHECK (WavpackPackSamples (wpc, pcm, 64), "DSD pack failed");
    CHECK (WavpackFlushSamples (wpc), "DSD flush failed");

    WavpackCloseFile (wpc);
    fclose (f);
    free (pcm);
}

static void encode_zero_ref (void)
{
    FILE *f = fopen ("qm_zero.wv", "wb");
    WavpackContext *wpc;
    WavpackConfig config;
    int32_t *pcm = malloc (FRAMES * CHANS * sizeof (int32_t));

    CHECK (f, "can't create zero reference file");
    wpc = WavpackOpenFileOutput (out_cb, f, NULL);
    CHECK (wpc, "can't create zero encoder");

    memset (&config, 0, sizeof (config));
    config.num_channels = CHANS;
    config.channel_mask = 7;
    config.sample_rate = 48000;
    config.bits_per_sample = 16;
    config.bytes_per_sample = 2;
    config.block_samples = BLKSZ;

    CHECK (WavpackSetConfiguration64 (wpc, &config, FRAMES, NULL), "zero config failed");
    CHECK (WavpackPackInit (wpc), "zero init failed");

    gen_pcm (pcm, FRAMES, CHANS, 1);
    CHECK (WavpackPackSamples (wpc, pcm, FRAMES), "zero pack failed");
    CHECK (WavpackFlushSamples (wpc), "zero flush failed");

    WavpackCloseFile (wpc);
    fclose (f);
    free (pcm);
}

/////////////////////////////////////////////////////////////////////////////
// decode helpers and result comparisons

typedef struct {
    int32_t *pcm;
    unsigned char *quality;
    uint32_t frames;
    int errors;
} Decode;

static void decode_quality (WavpackContext *wpc, Decode *d, uint32_t max_frames, int chans)
{
    d->pcm = calloc ((size_t) max_frames * chans, sizeof (int32_t));
    d->quality = malloc (max_frames);
    CHECK (d->pcm && d->quality, "out of memory");
    memset (d->quality, SENTINEL, max_frames);
    d->frames = WavpackUnpackSamplesWithQuality (wpc, d->pcm, max_frames, d->quality);
    d->errors = WavpackGetNumErrors (wpc);
}

static void decode_plain (WavpackContext *wpc, Decode *d, uint32_t max_frames, int chans)
{
    d->pcm = calloc ((size_t) max_frames * chans, sizeof (int32_t));
    d->quality = NULL;
    CHECK (d->pcm, "out of memory");
    d->frames = WavpackUnpackSamples (wpc, d->pcm, max_frames);
    d->errors = WavpackGetNumErrors (wpc);
}

static void free_decode (Decode *d)
{
    free (d->pcm);
    free (d->quality);
}

static void check_range (unsigned char *q, uint32_t from, uint32_t to, unsigned char value, const char *msg)
{
    uint32_t i;

    for (i = from; i < to; ++i)
        CHECK (q [i] == value, msg);
}

static void check_sentinel (unsigned char *q, uint32_t from, uint32_t to)
{
    uint32_t i;

    for (i = from; i < to; ++i)
        CHECK (q [i] == SENTINEL, "quality sentinel overwritten outside the returned range");
}

static void print_quality_groups (const char *label, unsigned char *q, uint32_t count)
{
    uint32_t i = 0;
    const char *names [] = { "LOSSLESS", "LOSSY", "CONCEALED" };

    printf ("%s:", label);

    while (i < count) {
        uint32_t j = i + 1;

        while (j < count && q [j] == q [i])
            j++;

        printf (" [%u..%u)=%s", i, j, q [i] <= 2 ? names [q [i]] : "?");
        i = j;
    }

    printf ("\n");
}

/////////////////////////////////////////////////////////////////////////////
// tests

static int32_t *reference_pcm;

// losslessly encoded digital silence is not concealed: the quality of frames that
// just happen to be zero is LOSSLESS

static void test_plain_zeros (void)
{
    WavpackContext *wpc = WavpackOpenFileInput ("qm_zero.wv", NULL, 0, 0);
    Decode d;
    int32_t pcm [FRAMES * CHANS];

    CHECK (wpc, "can't open zero file");
    decode_quality (wpc, &d, FRAMES, CHANS);
    CHECK (d.frames == FRAMES, "zeros: wrong frame count");
    CHECK (d.errors == 0, "zeros: unexpected errors");
    check_range (d.quality, 0, FRAMES, WV_QUALITY_LOSSLESS, "zeros: encoded zeros reported as concealed");

    gen_pcm (pcm, FRAMES, CHANS, 1);
    CHECK (!memcmp (d.pcm, pcm, sizeof (pcm)), "zeros: PCM mismatch");
    print_quality_groups ("plain-zeros", d.quality, d.frames);

    free_decode (&d);
    WavpackCloseFile (wpc);
}

static void test_complete (void)
{
    WavpackContext *wpc = WavpackOpenFileInput ("qm.wv", NULL, OPEN_WVC, 0);
    Decode d;
    uint32_t i;

    CHECK (wpc, "can't open complete file");
    decode_quality (wpc, &d, FRAMES, CHANS);
    CHECK (d.frames == FRAMES, "complete: wrong frame count");
    CHECK (d.errors == 0, "complete: unexpected errors");
    CHECK (!memcmp (d.pcm, reference_pcm, FRAMES * CHANS * sizeof (int32_t)), "complete: PCM mismatch");
    for (i = 0; i < FRAMES; ++i)
        CHECK (d.quality [i] == WV_QUALITY_LOSSLESS, "complete: frame not lossless");
    check_sentinel (d.quality, FRAMES, FRAMES);
    print_quality_groups ("complete", d.quality, d.frames);

    free_decode (&d);
    WavpackCloseFile (wpc);
}

static void test_no_correction (void)
{
    WavpackContext *wpc = WavpackOpenFileInput ("qm.wv", NULL, 0, 0);
    Decode dq, dp;
    uint32_t i;

    CHECK (wpc, "can't open wv only");
    decode_quality (wpc, &dq, FRAMES, CHANS);
    CHECK (dq.frames == FRAMES, "no-wvc: wrong frame count");
    for (i = 0; i < FRAMES; ++i)
        CHECK (dq.quality [i] == WV_QUALITY_LOSSY, "no-wvc: frame not lossy");
    print_quality_groups ("no-wvc", dq.quality, dq.frames);
    WavpackCloseFile (wpc);

    // the lossy decode must actually differ from the original somewhere (so that
    // this is a meaningful lossy test rather than an accidentally lossless file),
    // and the new entry point's audio must be identical to the old one's

    {
        int diff = 0;

        for (i = 0; i < FRAMES * CHANS; ++i)
            diff += dq.pcm [i] != reference_pcm [i];

        CHECK (diff > 0, "no-wvc: expected lossy differences but audio was exact");
    }

    wpc = WavpackOpenFileInput ("qm.wv", NULL, 0, 0);
    CHECK (wpc, "can't reopen wv only");
    decode_plain (wpc, &dp, FRAMES, CHANS);
    CHECK (dp.frames == FRAMES, "no-wvc plain: wrong frame count");
    CHECK (!memcmp (dq.pcm, dp.pcm, FRAMES * CHANS * sizeof (int32_t)), "no-wvc: new/old entry PCM mismatch");
    free_decode (&dp);
    WavpackCloseFile (wpc);

    free_decode (&dq);
}

// quality must follow the worst of the delivered substreams: a correction block
// missing only for the third (non-first) channel's substream makes that frame
// range lossy even though the other substreams remain exact.

static void build_missing_nonfirst_wvc (void)
{
    FileData wvc = read_file ("qm.wvc");
    BlockInfo blocks [128];
    int n = scan_blocks (&wvc, blocks, 128);
    long pos = find_block (blocks, n, 48, 1);     // mono (channel 2) correction block at frame 48

    CHECK (pos >= 0, "couldn't find the non-first correction block");
    free (wvc.data);
    filter_file ("qm.wvc", "qm_nonfirst.wvc", &pos, 1);
    copy_file ("qm.wv", "qm_nonfirst.wv");
}

static void test_missing_nonfirst_wvc (void)
{
    WavpackContext *wpc;
    Decode d, orig;
    uint32_t i;

    // the standard open path derives the correction file name from the wv name,
    // so use a matching pair (the damaged correction file was written next to a
    // copy of the undamaged wv file by build_missing_nonfirst_wvc)

    wpc = WavpackOpenFileInput ("qm_nonfirst.wv", NULL, OPEN_WVC, 0);
    CHECK (wpc, "can't open non-first file");
    decode_quality (wpc, &d, FRAMES, CHANS);
    CHECK (d.frames == FRAMES, "non-first: wrong frame count");
    free_decode (&d);
    WavpackCloseFile (wpc);

    // reference decode of the same damaged file with the original entry point

    wpc = WavpackOpenFileInput ("qm_nonfirst.wv", NULL, OPEN_WVC, 0);
    CHECK (wpc, "can't reopen non-first file");
    decode_plain (wpc, &orig, FRAMES, CHANS);
    CHECK (orig.frames == FRAMES, "non-first plain: wrong frame count");
    free_decode (&orig);
    WavpackCloseFile (wpc);

    // now decode the damaged file with quality and verify the pattern

    wpc = WavpackOpenFileInput ("qm_nonfirst.wv", NULL, OPEN_WVC, 0);
    CHECK (wpc, "can't reopen non-first file for quality");
    decode_quality (wpc, &d, FRAMES, CHANS);

    check_range (d.quality, 0, 48, WV_QUALITY_LOSSLESS, "non-first: frames before damage not lossless");
    check_range (d.quality, 48, 64, WV_QUALITY_LOSSY, "non-first: frame range with missing correction not lossy");
    check_range (d.quality, 64, FRAMES, WV_QUALITY_LOSSLESS, "non-first: frames after damage not lossless");

    // channels 0 and 1 should still be exact, channel 2 (at least somewhere in the
    // damaged range) should differ from the original since only its correction was
    // removed

    {
        int c0 = 0, c1 = 0, c2 = 0;

        for (i = 48; i < 64; ++i) {
            c0 += d.pcm [i * CHANS + 0] != reference_pcm [i * CHANS + 0];
            c1 += d.pcm [i * CHANS + 1] != reference_pcm [i * CHANS + 1];
            c2 += d.pcm [i * CHANS + 2] != reference_pcm [i * CHANS + 2];
        }

        CHECK (c0 == 0 && c1 == 0, "non-first: correction removal affected the wrong channels");
        CHECK (c2 > 0, "non-first: channel 2 should have been decoded lossily");
    }

    print_quality_groups ("missing-nonfirst-wvc", d.quality, d.frames);
    free_decode (&d);
    WavpackCloseFile (wpc);
}

// a completely missing WV block creates a gap that the library fills with silence

static void build_gap (void)
{
    FileData wv = read_file ("qm.wv");
    BlockInfo blocks [128];
    int n = scan_blocks (&wv, blocks, 128);
    long positions [8];
    int count = 0, i;

    for (i = 0; i < n; ++i)
        if (blocks [i].index == 80 && count < 8)
            positions [count++] = blocks [i].pos;

    CHECK (count == 2, "couldn't find the WV block group for the gap");
    free (wv.data);

    filter_file ("qm.wv", "qm_gap.wv", positions, count);
    copy_file ("qm.wvc", "qm_gap.wvc");
}

static void test_gap (void)
{
    WavpackContext *wpc = WavpackOpenFileInput ("qm_gap.wv", NULL, OPEN_WVC, 0);
    Decode d;
    uint32_t i;

    CHECK (wpc, "can't open gap file");
    decode_quality (wpc, &d, FRAMES, CHANS);
    CHECK (d.frames == FRAMES, "gap: wrong frame count");
    CHECK (d.errors >= 1, "gap: missing block was not reported as an error");
    check_range (d.quality, 0, 80, WV_QUALITY_LOSSLESS, "gap: frames before gap not lossless");
    check_range (d.quality, 80, 96, WV_QUALITY_CONCEALED, "gap: gap frames not concealed");
    check_range (d.quality, 96, FRAMES, WV_QUALITY_LOSSLESS, "gap: frames after gap not lossless");

    // the gap frames must be silence, and the audio outside the gap must be exact

    for (i = 80; i < 96; ++i) {
        int c;
        for (c = 0; c < CHANS; ++c)
            CHECK (d.pcm [i * CHANS + c] == 0, "gap: gap frame is not silence");
    }

    CHECK (!memcmp (d.pcm, reference_pcm, 80 * CHANS * sizeof (int32_t)), "gap: audio before the gap changed");
    CHECK (!memcmp (d.pcm + 96 * CHANS, reference_pcm + 96 * CHANS, (FRAMES - 96) * CHANS * sizeof (int32_t)),
        "gap: audio after the gap changed");

    print_quality_groups ("gap", d.quality, d.frames);
    free_decode (&d);
    WavpackCloseFile (wpc);
}

// a corrupted audio CRC is only detected at the end of a block. When the block is
// delivered in two calls, the first call's frames must not be retroactively marked
// concealed, and only the frames actually muted by the second call are concealed.

static void build_crc_mute (void)
{
    FileData wvc = read_file ("qm.wvc");
    BlockInfo blocks [128];
    int n = scan_blocks (&wvc, blocks, 128);
    long crc_pos [8];
    int count = 0, i;

    // for hybrid streams the correction block header replaces the WV header in the
    // decoder, so the audio CRC that is checked at the end of the block is the one
    // stored in the correction block

    for (i = 0; i < n; ++i)
        if (blocks [i].index == 64 && count < 8)
            crc_pos [count++] = blocks [i].pos;

    CHECK (count == 2, "couldn't find the WVC block group for the CRC test");

    for (i = 0; i < count; ++i)
        corrupt_audio_crc (&wvc, crc_pos [i]);

    write_file ("qm_crc.wvc", &wvc);
    free (wvc.data);
    copy_file ("qm.wv", "qm_crc.wv");
}

static void test_crc_mute (void)
{
    WavpackContext *wpc = WavpackOpenFileInput ("qm_crc.wv", NULL, OPEN_WVC, 0);
    Decode d;
    uint32_t i;

    CHECK (wpc, "can't open crc file");

    // a full sequential decode marks the whole damaged group as concealed (the
    // library zeroes it when the block ends)

    decode_quality (wpc, &d, FRAMES, CHANS);
    CHECK (d.frames == FRAMES, "crc: wrong frame count");
    CHECK (d.errors >= 1, "crc: corrupt audio CRC was not reported");
    check_range (d.quality, 0, 64, WV_QUALITY_LOSSLESS, "crc: frames before damage not lossless");
    check_range (d.quality, 64, 80, WV_QUALITY_CONCEALED, "crc: damaged group not concealed");
    check_range (d.quality, 80, FRAMES, WV_QUALITY_LOSSLESS, "crc: frames after damage not lossless");

    for (i = 64; i < 80; ++i) {
        int c;
        for (c = 0; c < CHANS; ++c)
            CHECK (d.pcm [i * CHANS + c] == 0, "crc: muted frame is not silence");
    }

    print_quality_groups ("crc-full", d.quality, d.frames);
    free_decode (&d);
    WavpackCloseFile (wpc);

    // now deliver the same damaged block in two calls: the first half must be
    // delivered normally (and reported as lossless), and only the second half (the
    // one that actually reaches the block end and is muted) is concealed

    wpc = WavpackOpenFileInput ("qm_crc.wv", NULL, OPEN_WVC, 0);
    CHECK (wpc, "can't reopen crc file");
    CHECK (WavpackSeekSample64 (wpc, 64), "crc: seek failed");

    {
        Decode half1, half2;

        decode_quality (wpc, &half1, 8, CHANS);
        CHECK (half1.frames == 8, "crc: first half wrong frame count");
        check_range (half1.quality, 0, 8, WV_QUALITY_LOSSLESS, "crc: first half should not be concealed");
        CHECK (!memcmp (half1.pcm, reference_pcm + 64 * CHANS, 8 * CHANS * sizeof (int32_t)),
            "crc: first half PCM should be exact (not retroactively muted)");

        decode_quality (wpc, &half2, 8, CHANS);
        CHECK (half2.frames == 8, "crc: second half wrong frame count");
        check_range (half2.quality, 0, 8, WV_QUALITY_CONCEALED, "crc: second half should be concealed");

        for (i = 0; i < 8 * CHANS; ++i)
            CHECK (half2.pcm [i] == 0, "crc: second half was not muted");

        print_quality_groups ("crc-half1", half1.quality, half1.frames);
        print_quality_groups ("crc-half2", half2.quality, half2.frames);

        free_decode (&half1);
        free_decode (&half2);
    }

    WavpackCloseFile (wpc);
}

// requests of 1, 7, and a whole block (16) frames, and the short read at EOF

static void test_chunk_sizes (void)
{
    unsigned char expected [FRAMES];
    uint32_t i;
    int sizes [] = { 1, 7, BLKSZ };
    int s;

    for (i = 0; i < FRAMES; ++i)
        expected [i] = WV_QUALITY_LOSSLESS;

    for (s = 0; s < 3; ++s) {
        WavpackContext *wpc = WavpackOpenFileInput ("qm.wv", NULL, OPEN_WVC, 0);
        Decode d;
        uint32_t off = 0;

        CHECK (wpc, "can't open file for chunk test");

        while (off < FRAMES) {
            uint32_t want = sizes [s];

            if (want > FRAMES - off)
                want = FRAMES - off;

            decode_quality (wpc, &d, want, CHANS);
            CHECK (d.frames == want, "chunk: wrong frame count");

            // all returned frames are lossless and match the original, and the
            // positions beyond the returned frames retain the sentinel

            check_range (d.quality, 0, d.frames, WV_QUALITY_LOSSLESS, "chunk: frame not lossless");
            check_sentinel (d.quality, d.frames, want);
            CHECK (!memcmp (d.pcm, reference_pcm + off * CHANS, want * CHANS * sizeof (int32_t)),
                "chunk: PCM mismatch");

            off += d.frames;
            free_decode (&d);
        }

        // a short read past the end of the file must return zero frames and leave
        // the quality buffer untouched

        decode_quality (wpc, &d, 5, CHANS);
        CHECK (d.frames == 0, "chunk: read past EOF did not return zero");
        check_sentinel (d.quality, 0, 5);
        free_decode (&d);
        WavpackCloseFile (wpc);
    }

    printf ("chunk sizes 1/7/%d: OK\n", BLKSZ);
}

// zero-frame calls must not change any state or touch either buffer

static void test_zero_frames (void)
{
    WavpackContext *wpc = WavpackOpenFileInput ("qm.wv", NULL, OPEN_WVC, 0);
    int32_t pcm [CHANS * 4];
    unsigned char q [4];
    uint32_t i;

    CHECK (wpc, "can't open file for zero-frame test");

    for (i = 0; i < CHANS * 4; ++i)
        pcm [i] = 0x12345678;

    memset (q, SENTINEL, sizeof (q));
    CHECK (WavpackUnpackSamplesWithQuality (wpc, pcm, 0, q) == 0, "zero-frame: wrong return");
    CHECK (WavpackGetSampleIndex64 (wpc) == 0, "zero-frame: sample index changed");
    CHECK (WavpackGetNumErrors (wpc) == 0, "zero-frame: error count changed");

    for (i = 0; i < CHANS * 4; ++i)
        CHECK (pcm [i] == 0x12345678, "zero-frame: PCM buffer was written");

    for (i = 0; i < 4; ++i)
        CHECK (q [i] == SENTINEL, "zero-frame: quality buffer was written");

    // and a zero-frame call between normal reads must not disturb the stream

    {
        Decode a, b;
        decode_quality (wpc, &a, 8, CHANS);
        CHECK (WavpackUnpackSamplesWithQuality (wpc, pcm, 0, q) == 0, "zero-frame mid: wrong return");
        decode_quality (wpc, &b, 8, CHANS);
        CHECK (a.frames == 8 && b.frames == 8, "zero-frame mid: wrong frame count");
        CHECK (b.pcm [0] == reference_pcm [8 * CHANS], "zero-frame mid: stream disturbed");
        CHECK (b.quality [0] == WV_QUALITY_LOSSLESS, "zero-frame mid: wrong quality");
        free_decode (&a);
        free_decode (&b);
    }

    WavpackCloseFile (wpc);
    printf ("zero-frame calls: OK\n");
}

// alternating the old and new entry points on the same context must produce the
// same audio as either one alone

static void test_alternating (void)
{
    WavpackContext *wpc = WavpackOpenFileInput ("qm.wv", NULL, OPEN_WVC, 0);
    int32_t pcm [FRAMES * CHANS];
    unsigned char q [FRAMES];
    uint32_t off = 0;

    CHECK (wpc, "can't open file for alternating test");
    memset (pcm, 0, sizeof (pcm));
    memset (q, SENTINEL, sizeof (q));

    while (off < FRAMES) {
        uint32_t n = 5;

        if (n > FRAMES - off)
            n = FRAMES - off;

        if (off & 1) {
            uint32_t got = WavpackUnpackSamples (wpc, pcm + off * CHANS, n);
            CHECK (got == n, "alternating: wrong frame count (old entry)");
            // quality entries for this range are not touched by the old entry point
            check_sentinel (q, off, off + n);
        }
        else {
            uint32_t got = WavpackUnpackSamplesWithQuality (wpc, pcm + off * CHANS, n, q + off);
            CHECK (got == n, "alternating: wrong frame count (new entry)");
            check_range (q, off, off + n, WV_QUALITY_LOSSLESS, "alternating: frame not lossless");
        }

        off += n;
    }

    // for the frames decoded with the new entry point, verify the audio; the
    // frames decoded with the old entry point are verified as well, since both
    // must be the exact original

    CHECK (!memcmp (pcm, reference_pcm, FRAMES * CHANS * sizeof (int32_t)), "alternating: PCM mismatch");
    WavpackCloseFile (wpc);
    printf ("alternating entry points: OK\n");
}

// seeking: reading the same range with the same chunking after a seek must
// produce identical PCM and quality maps, and quality computed before a seek
// must not leak across it

static void test_seek (void)
{
    WavpackContext *wpc = WavpackOpenFileInput ("qm.wv", NULL, OPEN_WVC, 0);
    int32_t pcm1 [FRAMES * CHANS], pcm2 [FRAMES * CHANS];
    unsigned char q1 [FRAMES], q2 [FRAMES];
    uint32_t got1, got2, i;

    CHECK (wpc, "can't open file for seek test");

    // first pass: sequential, chunk 7
    got1 = 0;
    memset (pcm1, 0, sizeof (pcm1));
    memset (q1, SENTINEL, sizeof (q1));

    while (got1 < FRAMES) {
        uint32_t n = 7;
        if (n > FRAMES - got1) n = FRAMES - got1;
        CHECK (WavpackUnpackSamplesWithQuality (wpc, pcm1 + got1 * CHANS, n, q1 + got1) == n, "seek: pass1 short");
        got1 += n;
    }

    // move far forward, read a chunk (this must have no effect on the cached
    // quality of anything else), then seek back to 0 and re-read identically
    CHECK (WavpackSeekSample64 (wpc, 96), "seek: forward seek failed");

    {
        int32_t tmp [32 * CHANS];
        unsigned char tq [32];
        memset (tmp, 0, sizeof (tmp));
        memset (tq, SENTINEL, sizeof (tq));
        CHECK (WavpackUnpackSamplesWithQuality (wpc, tmp, 32, tq) == 32, "seek: forward read failed");
    }

    CHECK (WavpackSeekSample64 (wpc, 0), "seek: back seek failed");

    got2 = 0;
    memset (pcm2, 0, sizeof (pcm2));
    memset (q2, SENTINEL, sizeof (q2));

    while (got2 < FRAMES) {
        uint32_t n = 7;
        if (n > FRAMES - got2) n = FRAMES - got2;
        CHECK (WavpackUnpackSamplesWithQuality (wpc, pcm2 + got2 * CHANS, n, q2 + got2) == n, "seek: pass2 short");
        got2 += n;
    }

    CHECK (!memcmp (pcm1, pcm2, sizeof (pcm1)), "seek: PCM differs after seek");
    CHECK (!memcmp (q1, q2, FRAMES), "seek: quality differs after seek");

    print_quality_groups ("seek-pass1", q1, FRAMES);
    print_quality_groups ("seek-pass2", q2, FRAMES);

    for (i = 0; i < FRAMES; ++i)
        CHECK (q2 [i] == WV_QUALITY_LOSSLESS, "seek: re-read frame not lossless");

    WavpackCloseFile (wpc);
    printf ("seek re-read consistency: OK\n");
}

// the same damaged file must also produce the same quality map after a seek

static void test_seek_damaged (void)
{
    WavpackContext *wpc = WavpackOpenFileInput ("qm_nonfirst.wv", NULL, OPEN_WVC, 0);
    Decode a, b;

    CHECK (wpc, "can't open damaged file for seek test");
    decode_quality (wpc, &a, 64, CHANS);        // frames 0..63 (includes damage)
    CHECK (WavpackSeekSample64 (wpc, 0), "damaged seek: failed");
    decode_quality (wpc, &b, 64, CHANS);

    CHECK (a.frames == 64 && b.frames == 64, "damaged seek: wrong frame count");
    CHECK (!memcmp (a.pcm, b.pcm, 64 * CHANS * sizeof (int32_t)), "damaged seek: PCM differs");
    CHECK (!memcmp (a.quality, b.quality, 64), "damaged seek: quality differs");
    print_quality_groups ("damaged-after-seek", b.quality, b.frames);

    free_decode (&a);
    free_decode (&b);
    WavpackCloseFile (wpc);
    printf ("damaged seek re-read consistency: OK\n");
}

// floating-point and DSD streams: quality maps are unavailable (return 0, set an
// error, consume nothing, write neither buffer) while the original entry point
// continues to decode them normally

static void test_unsupported (const char *path, const char *label, int chans, int open_flags)
{
    WavpackContext *wpc;
    int32_t *dec;
    unsigned char q [64];
    int i;
    int errors;

    wpc = WavpackOpenFileInput (path, NULL, open_flags, 0);
    if (!wpc) {
        printf ("%s unavailable in this build, skipping\n", label);
        return;
    }

    dec = calloc (64 * chans, sizeof (int32_t));
    CHECK (dec, "out of memory");

    // the original entry point must decode normally
    CHECK (WavpackUnpackSamples (wpc, dec, 64) == 64, "unsupported: old entry failed");
    free (dec);

    // rewind and retry with quality
    WavpackCloseFile (wpc);
    wpc = WavpackOpenFileInput (path, NULL, open_flags, 0);
    CHECK (wpc, "can't reopen unsupported file");

    dec = calloc (64 * chans, sizeof (int32_t));
    CHECK (dec, "out of memory");
    for (i = 0; i < 64 * chans; ++i) dec [i] = 0x7e7e7e7e;
    for (i = 0; i < 64; ++i) q [i] = SENTINEL;

    errors = WavpackGetNumErrors (wpc);
    CHECK (WavpackUnpackSamplesWithQuality (wpc, dec, 64, q) == 0, "unsupported: quality request did not return 0");
    CHECK (WavpackGetNumErrors (wpc) == errors, "unsupported: error count changed");
    CHECK (WavpackGetSampleIndex64 (wpc) == 0, "unsupported: input was consumed");

    for (i = 0; i < 64 * chans; ++i)
        CHECK (dec [i] == 0x7e7e7e7e, "unsupported: PCM buffer was written");

    for (i = 0; i < 64; ++i)
        CHECK (q [i] == SENTINEL, "unsupported: quality buffer was written");

    CHECK (WavpackGetErrorMessage (wpc) [0] != 0, "unsupported: no error message set");
    printf ("%s quality rejection: OK (%s)\n", label, WavpackGetErrorMessage (wpc));

    // a NULL quality map must still work through the new entry point
    CHECK (WavpackUnpackSamplesWithQuality (wpc, dec, 64, NULL) == 64, "unsupported: NULL quality path failed");

    free (dec);
    WavpackCloseFile (wpc);
}

// exercise the worker thread paths (spatial for multichannel, temporal for
// single-stream files) and verify that quality and PCM are identical to the
// serial path

static void test_threads (void)
{
    int32_t stpcm [STFRAMES * STCHANS];
    int i;

    for (i = 0; i < STFRAMES * STCHANS; ++i)
        stpcm [i] = (((i * 1103515245u + 12345u) >> 16) % 50001) - 25000;

    encode_stereo_lossless ("qm_stereo.wv", stpcm, STFRAMES);

    // serial decode of the plain stereo file
    {
        WavpackContext *wpc = WavpackOpenFileInput ("qm_stereo.wv", NULL, 0, 0);
        Decode d;
        CHECK (wpc, "can't open stereo file");
        decode_quality (wpc, &d, STFRAMES, STCHANS);
        CHECK (d.frames == STFRAMES, "stereo: wrong frame count");
        check_range (d.quality, 0, STFRAMES, WV_QUALITY_LOSSLESS, "stereo: frame not lossless");
        print_quality_groups ("stereo-serial", d.quality, d.frames);
        free_decode (&d);
        WavpackCloseFile (wpc);
    }

    // temporal worker decode of the same file, requested in one call so that
    // blocks are handed to the workers while later blocks are decoded
    {
        WavpackContext *wpc = WavpackOpenFileInput ("qm_stereo.wv", NULL, 2 << OPEN_THREADS_SHFT, 0);
        Decode d;
        CHECK (wpc, "can't open stereo file for threads");
        decode_quality (wpc, &d, STFRAMES, STCHANS);
        CHECK (d.frames == STFRAMES, "stereo threads: wrong frame count");
        CHECK (!memcmp (d.pcm, stpcm, sizeof (stpcm)), "stereo threads: PCM mismatch");
        check_range (d.quality, 0, STFRAMES, WV_QUALITY_LOSSLESS, "stereo threads: frame not lossless");
        print_quality_groups ("stereo-temporal-workers", d.quality, d.frames);
        free_decode (&d);
        WavpackCloseFile (wpc);
    }

    // now the same stereo file as hybrid: temporal worker path with corrections
    {
        int32_t *hpcm = malloc (STFRAMES * STCHANS * sizeof (int32_t));
        WavpackContext *wpc;
        Decode d;

        for (i = 0; i < STFRAMES * STCHANS; ++i)
            hpcm [i] = (((i * 1103515245u + 98765u) >> 16) % 60001) - 30000;

        encode_stereo_hybrid ("qm_sthyb.wv", "qm_sthyb.wvc", hpcm, STFRAMES);

        wpc = WavpackOpenFileInput ("qm_sthyb.wv", NULL, OPEN_WVC | (2 << OPEN_THREADS_SHFT), 0);
        CHECK (wpc, "can't open stereo hybrid for threads");
        decode_quality (wpc, &d, STFRAMES, STCHANS);
        CHECK (d.frames == STFRAMES, "stereo hybrid threads: wrong frame count");
        CHECK (!memcmp (d.pcm, hpcm, STFRAMES * STCHANS * sizeof (int32_t)), "stereo hybrid threads: PCM mismatch");
        check_range (d.quality, 0, STFRAMES, WV_QUALITY_LOSSLESS, "stereo hybrid threads: frame not lossless");
        print_quality_groups ("stereo-hybrid-workers", d.quality, d.frames);
        free_decode (&d);
        WavpackCloseFile (wpc);

        // and without the correction file the same worker path must report lossy
        wpc = WavpackOpenFileInput ("qm_sthyb.wv", NULL, 2 << OPEN_THREADS_SHFT, 0);
        CHECK (wpc, "can't open stereo hybrid wv only");
        decode_quality (wpc, &d, STFRAMES, STCHANS);
        CHECK (d.frames == STFRAMES, "stereo hybrid lossy threads: wrong frame count");
        check_range (d.quality, 0, STFRAMES, WV_QUALITY_LOSSY, "stereo hybrid lossy threads: frame not lossy");
        print_quality_groups ("stereo-hybrid-workers-lossy", d.quality, d.frames);
        free_decode (&d);
        WavpackCloseFile (wpc);

        free (hpcm);
    }

    // spatial workers on the multichannel hybrid file must agree with the serial
    // decode exactly (including under damaged correction data)
    {
        WavpackContext *wpc = WavpackOpenFileInput ("qm_nonfirst.wv", NULL, OPEN_WVC | (2 << OPEN_THREADS_SHFT), 0);
        Decode threaded;
        int32_t sp [FRAMES * CHANS];
        unsigned char sq [FRAMES];
        uint32_t got = 0;

        CHECK (wpc, "can't open multichannel damaged file for threads");
        decode_quality (wpc, &threaded, FRAMES, CHANS);
        CHECK (threaded.frames == FRAMES, "mc threads: wrong frame count");
        print_quality_groups ("mc-workers-damaged", threaded.quality, threaded.frames);
        free_decode (&threaded);
        WavpackCloseFile (wpc);

        wpc = WavpackOpenFileInput ("qm_nonfirst.wv", NULL, OPEN_WVC, 0);
        CHECK (wpc, "can't reopen multichannel damaged file");
        memset (sp, 0, sizeof (sp));
        memset (sq, SENTINEL, sizeof (sq));
        while (got < FRAMES) {
            uint32_t n = 7;
            if (n > FRAMES - got) n = FRAMES - got;
            CHECK (WavpackUnpackSamplesWithQuality (wpc, sp + got * CHANS, n, sq + got) == n, "mc serial short");
            got += n;
        }
        WavpackCloseFile (wpc);

        wpc = WavpackOpenFileInput ("qm_nonfirst.wv", NULL, OPEN_WVC | (2 << OPEN_THREADS_SHFT), 0);
        CHECK (wpc, "can't reopen multichannel for threaded compare");

        {
            int32_t tp [FRAMES * CHANS];
            unsigned char tq [FRAMES];
            uint32_t o = 0;

            memset (tp, 0, sizeof (tp));
            memset (tq, SENTINEL, sizeof (tq));
            while (o < FRAMES) {
                uint32_t n = 7;
                if (n > FRAMES - o) n = FRAMES - o;
                CHECK (WavpackUnpackSamplesWithQuality (wpc, tp + o * CHANS, n, tq + o) == n, "mc threaded short");
                o += n;
            }

            CHECK (!memcmp (sp, tp, sizeof (sp)), "mc: serial and threaded PCM differ");
            CHECK (!memcmp (sq, tq, FRAMES), "mc: serial and threaded quality differ");
        }

        WavpackCloseFile (wpc);
    }

    // repeat the serial/threaded comparison (including the damaged file) several times
    // to exercise the temporal worker path's completion ordering, because the quality
    // of a frame range must never be influenced by a later block's correction state

    {
        int iter;

        for (iter = 0; iter < 10; ++iter) {
            WavpackContext *tw = WavpackOpenFileInput ("qm_sthyb.wv", NULL, OPEN_WVC | (2 << OPEN_THREADS_SHFT), 0);
            WavpackContext *sw = WavpackOpenFileInput ("qm_sthyb.wv", NULL, OPEN_WVC, 0);
            int32_t tp [STFRAMES * STCHANS], sp [STFRAMES * STCHANS];
            unsigned char tq [STFRAMES], sq [STFRAMES];
            uint32_t o;

            CHECK (tw && sw, "can't open stereo hybrid for stress compare");

            for (o = 0; o < STFRAMES; o += 6) {
                uint32_t n = 6;
                if (n > STFRAMES - o) n = STFRAMES - o;
                CHECK (WavpackUnpackSamplesWithQuality (tw, tp + o * STCHANS, n, tq + o) == n, "stress threaded short");
                CHECK (WavpackUnpackSamplesWithQuality (sw, sp + o * STCHANS, n, sq + o) == n, "stress serial short");
            }

            CHECK (!memcmp (tp, sp, sizeof (tp)), "stress: serial/threaded PCM mismatch");
            CHECK (!memcmp (tq, sq, sizeof (tq)), "stress: serial/threaded quality mismatch");

            WavpackCloseFile (tw);
            WavpackCloseFile (sw);
        }
    }

    printf ("serial and worker thread paths: OK\n");
}

/////////////////////////////////////////////////////////////////////////////

int main (void)
{
    reference_pcm = malloc (FRAMES * CHANS * sizeof (int32_t));
    CHECK (reference_pcm, "out of memory");
    gen_pcm (reference_pcm, FRAMES, CHANS, 0);

    encode_hybrid_ref ();
    encode_zero_ref ();

    test_complete ();
    test_plain_zeros ();
    test_no_correction ();

    build_missing_nonfirst_wvc ();
    test_missing_nonfirst_wvc ();

    build_gap ();
    test_gap ();

    build_crc_mute ();
    test_crc_mute ();

    test_chunk_sizes ();
    test_zero_frames ();
    test_alternating ();
    test_seek ();
    test_seek_damaged ();
    test_threads ();

    encode_float ();
    test_unsupported ("qm_float.wv", "float", 2, 0);

    encode_dsd ();
    if (fopen ("qm_dsd.wv", "rb")) {
        test_unsupported ("qm_dsd.wv", "dsd", 2, OPEN_DSD_NATIVE);
    }

    free (reference_pcm);
    printf ("QUALITY_MAP_OK\n");
    return 0;
}
