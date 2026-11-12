// ============================================================================
// tests/coefficients.c
//
// Offline numerical regression for the MPEG-1 coefficient reconstruction in
// pl_mpeg.  The test maintains its own model of 8x8 DCT coefficients in
// *natural* (row/column) coordinates, builds legal MPEG-1 elementary streams
// from that model, feeds them through the public decode chain
// (plm_buffer_create_with_memory -> plm_video_create_with_buffer ->
//  plm_video_decode) and checks the resulting Y/Cb/Cr planes against a fully
// independent reference implementation.
//
// The reference never touches pl_mpeg internals:
//   - the zigzag order is generated from the diagonal scan rule;
//   - dequantization follows the MPEG-1 intra/inter distinction, the
//     even->odd adjustment and the coefficient clipping;
//   - the inverse transform is a direct double precision 2-D cosine sum.
// pl_mpeg's premultiplier table, butterfly IDCT and VLC walk tables are not
// used to compute any expected value.
//
// On assertion failure: prints "COEFF_ASSERT scene=<name> ..." and exits 42.
// On full success: prints "MPEG_COEFF_OK cases=<n>".
//
// Build: cc -std=c99 -O0 -I. tests/coefficients.c -lm -o coefficients
// ============================================================================

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

// ----------------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------------

static int clamp_u8(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static int iabs(int v) { return v < 0 ? -v : v; }

// round-half-away-from-zero, used only for the +/-1 reference comparisons
static int round_near(double v) {
    return (int)lround(v);
}

// ============================================================================
// Reference side: zigzag generated from the diagonal scan rule
//
// MPEG-1 zigzag walks the anti-diagonals of the 8x8 natural grid; even
// diagonals (u+v even) are traversed bottom-up, odd diagonals top-down.
// inv_zigzag[scan] = natural index, zigzag[natural] = scan index.
// ============================================================================

static uint8_t g_zigzag[64];     // natural -> scan
static uint8_t g_inv_zigzag[64]; // scan    -> natural

static void reference_init_zigzag(void) {
    int scan = 0;
    for (int s = 0; s <= 14; s++) {
        if ((s & 1) == 0) {
            // even diagonal: from (min(s,7), s-min(s,7)) up to ...
            int u = s < 8 ? s : 7;
            int v = s - u;
            while (u >= 0 && v < 8) {
                g_inv_zigzag[scan] = (uint8_t)(u * 8 + v);
                u--;
                v++;
                scan++;
            }
        } else {
            // odd diagonal: top-down
            int v = s < 8 ? s : 7;
            int u = s - v;
            while (v >= 0 && u < 8) {
                g_inv_zigzag[scan] = (uint8_t)(u * 8 + v);
                v--;
                u++;
                scan++;
            }
        }
    }
    for (int i = 0; i < 64; i++) {
        g_zigzag[g_inv_zigzag[i]] = (uint8_t)i;
    }
}

// ----------------------------------------------------------------------------
// Quantization matrices in NATURAL coordinates.  The defaults are the MPEG-1
// spec tables; the custom ones are deliberately asymmetric so that a missing
// zigzag remap moves energy to a visible, wrong frequency position.
// ----------------------------------------------------------------------------

static const int REF_INTRA_DEFAULT[64] = {
     8, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};

static const int REF_NONINTRA_DEFAULT[64] = {
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16
};

// m[u][v] = 8 + 4u + 2v, plus a diagonal bump: rows and columns always
// differ, so horizontal basis (0,v) and vertical basis (u,0) get different
// weights.  All entries stay in [8,53].
static int REF_INTRA_CUSTOM[64];
// m[u][v] = 16 + 2u + v, entries in [16,37]; m[0][0] stays 16.
static int REF_NONINTRA_CUSTOM[64];

static void reference_init_matrices(void) {
    for (int u = 0; u < 8; u++) {
        for (int v = 0; v < 8; v++) {
            REF_INTRA_CUSTOM[u * 8 + v] = 8 + 4 * u + 2 * v + (u == v ? 3 : 0);
            REF_NONINTRA_CUSTOM[u * 8 + v] = 16 + 2 * u + v;
        }
    }
}

// ----------------------------------------------------------------------------
// Reference dequantization (MPEG-1).  Inputs are the encoded levels; output is
// the reconstructed transform coefficient F[u][v] in natural coordinates,
// i.e. exactly what an orthonormal-style 2-D IDCT expects.
//
// intra DC:      F00 = 8 * dc_coeff
// intra AC:      t = (2*level)*q*w >> 4, then even->odd, clip [-2048,2047]
// non-intra:     t = (2*level + sign(level))*q*w >> 4, then even->odd, clip
// ----------------------------------------------------------------------------

static void ref_dequant_intra(int dc_coeff, const int ac_level[64],
                              int qscale, const int matrix[64], int F[64]) {
    memset(F, 0, 64 * sizeof(int));
    F[0] = dc_coeff * 8;
    for (int idx = 1; idx < 64; idx++) {
        int level = ac_level[idx];
        if (level == 0) { continue; }
        int t = (2 * level * qscale * matrix[idx]) >> 4;
        if ((t & 1) == 0) { t += (t > 0 ? -1 : 1); }
        if (t > 2047) { t = 2047; }
        if (t < -2048) { t = -2048; }
        F[idx] = t;
    }
}

static void ref_dequant_inter(const int level_natural[64],
                              int qscale, const int matrix[64], int F[64]) {
    memset(F, 0, 64 * sizeof(int));
    for (int idx = 0; idx < 64; idx++) {
        int level = level_natural[idx];
        if (level == 0) { continue; }
        int widened = 2 * level + (level < 0 ? -1 : 1);
        int t = (widened * qscale * matrix[idx]) >> 4;
        if ((t & 1) == 0) { t += (t > 0 ? -1 : 1); }
        if (t > 2047) { t = 2047; }
        if (t < -2048) { t = -2048; }
        F[idx] = t;
    }
}

// Direct double precision 2-D inverse DCT cosine sum:
//   f[row][col] = 1/4 sum_u sum_v C(u)C(v) F[u][v]
//             cos((2row+1)u pi/16) cos((2col+1)v pi/16), C(0)=1/sqrt(2).
// Output is row-major: out[row*8+col].  For F00 only it collapses to F00/8.
static void ref_idct_2d(const int F[64], double out[64]) {
    double cu[8], cv[8];
    for (int k = 0; k < 8; k++) {
        cu[k] = (k == 0) ? 1.0 / sqrt(2.0) : 1.0;
        cv[k] = cu[k];
    }
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            double sum = 0.0;
            for (int u = 0; u < 8; u++) {
                double cosu = cos((double)(2 * row + 1) * u * M_PI / 16.0);
                for (int v = 0; v < 8; v++) {
                    if (F[u * 8 + v] == 0) { continue; }
                    double cosv = cos((double)(2 * col + 1) * v * M_PI / 16.0);
                    sum += cu[u] * cv[v] * (double)F[u * 8 + v] * cosu * cosv;
                }
            }
            out[row * 8 + col] = 0.25 * sum;
        }
    }
}


// ============================================================================
// MPEG-1 elementary stream writer.  Everything is constructed from fixed
// coefficient models; no decoded output is ever fed back in.
// ============================================================================

static uint8_t g_es[1u << 20];
static size_t g_bit_pos;

static void es_align(void) {
    g_bit_pos = (g_bit_pos + 7u) & ~(size_t)7u;
}

static void es_bits(unsigned value, unsigned n) {
    for (unsigned i = n; i > 0; i--) {
        unsigned bit = (value >> (i - 1)) & 1u;
        if (bit) {
            g_es[g_bit_pos >> 3] |= (uint8_t)(1u << (7 - (g_bit_pos & 7)));
        }
        g_bit_pos++;
    }
}

static void es_start_code(unsigned code) {
    es_align();
    es_bits(0, 8);
    es_bits(0, 8);
    es_bits(1, 8);
    es_bits(code, 8);
}

static void es_string(const char *bits) {
    for (const char *p = bits; *p; p++) {
        es_bits((unsigned)(*p - '0'), 1);
    }
}

// --- VLC fragments actually used (paths into the MPEG-1 VLC state tables) ---

static void vlc_mb_increment_one(void) { es_string("1"); }

static void vlc_mb_type_intra(void) { es_string("1"); } // 0x01

// Predictive picture macroblock types, chosen WITHOUT a forward motion vector
// so no motion VLCs have to be emitted (the decoder then leaves the vector at
// zero and predicts the macroblock in place from the reference frame).
static void vlc_mb_type_inter_cbp(void) { es_string("01"); }       // 0x02
static void vlc_mb_type_inter_q_cbp(void) { es_string("00001"); }  // 0x12

// Code block pattern codewords for the luma-only patterns the inter scenes
// use (verified by walking pl_mpeg's PLM_VIDEO_CODE_BLOCK_PATTERN table).
static void vlc_cbp(int cbp) {
    const char *code;
    switch (cbp) {
        case 4:  code = "1101";  break; // block 3
        case 8:  code = "1100";  break; // block 2
        case 16: code = "1011";  break; // block 1
        case 32: code = "1010";  break; // block 0
        case 48: code = "10010"; break; // blocks 0+1
        case 60: code = "111";   break; // blocks 0+1+2+3
        default:
            fprintf(stderr, "COEFF_ASSERT scene=internal cbp=%d unsupported\n",
                    cbp);
            exit(42);
    }
    es_string(code);
}

static void vlc_dct_size(int plane, int size) {
    // Paths traced through pl_mpeg's PLM_VIDEO_DCT_SIZE state tables.
    // plane 0 = luminance, 1/2 = chrominance.
    static const char *lum[9] = {
        "100", "00", "01", "101", "110", "1110", "11110",
        "111110", "1111110"
    };
    static const char *chr[9] = {
        "00", "01", "10", "110", "1110", "11110", "111110",
        "1111110", "11111110"
    };
    es_string(plane == 0 ? lum[size] : chr[size]);
}

// DCT coefficient VLCs used directly; everything else goes through escape.
static void vlc_dct_eob(void) { es_string("10"); } // only valid with n > 0

static void vlc_dct_escape(int run, int level) {
    es_string("000001"); // escape in pl_mpeg's DCT coefficient table
    es_bits((unsigned)run, 6);
    if (level > 0) {
        es_bits((unsigned)level, 8);          // 1..127
    } else {
        es_bits((unsigned)(256 + level), 8);  // -1..-127 -> 255..129
    }
}

// Emit one (run, level) token, choosing the direct VLC for the small table
// entries and the escape code otherwise, so both decode paths are exercised.
// n is the scan index the token lands on: at n == 0 the run-0/level-1
// codeword needs no EOB discriminator, while at n > 0 the decoder reads an
// extra bit to tell the coefficient apart from end_of_block.
static void emit_coeff(int run, int level, int n) {
    int abs_level = iabs(level);
    int direct =
        (run == 0 && (abs_level == 1 || abs_level == 2 || abs_level == 3)) ||
        (run == 1 && abs_level == 1) ||
        (run == 2 && abs_level == 1);
    if (!direct) {
        vlc_dct_escape(run, level);
        return;
    }
    if (run == 0 && abs_level == 1 && n > 0) {
        // "1" + 1 (not end_of_block) + sign
        es_string("11");
        es_bits(level < 0 ? 1u : 0u, 1);
        return;
    }
    const char *code;
    if (run == 0 && abs_level == 1) { code = "1"; }
    else if (run == 0 && abs_level == 2) { code = "0100"; }
    else if (run == 0 && abs_level == 3) { code = "00101"; }
    else if (run == 1 && abs_level == 1) { code = "011"; }
    else { code = "0101"; } // run == 2, level +/-1
    es_string(code);
    es_bits(level < 0 ? 1u : 0u, 1);
}

static int dc_diff_size(int diff) {
    if (diff == 0) { return 0; }
    int size = 1, mag = iabs(diff);
    while ((1 << size) <= mag) { size++; }
    return size;
}

// Signed DC differential in `size` bits (MPEG-1 mapping, see ISO 7.2):
// positive diffs are sent as-is, negative diffs as diff + 2^size - 1.
static void emit_dc_differential(int diff) {
    int size = dc_diff_size(diff);
    if (size == 0) { return; }
    unsigned enc = diff > 0
        ? (unsigned)diff
        : (unsigned)((1 << size) - 1 + diff);
    es_bits(enc, (unsigned)size);
}

// ----------------------------------------------------------------------------
// Stream-level builders
// ----------------------------------------------------------------------------

typedef struct {
    int dc;                  // reconstructed DC coefficient (intra blocks)
    int ac[64];              // AC levels indexed by NATURAL coordinate
} block_model_t;

typedef struct {
    block_model_t blocks[6]; // 4 luma, Cb, Cr
} mb_model_t;

static void es_sequence_header(int width, int height,
                               int custom_intra, int custom_nonintra) {
    es_start_code(0xB3);
    es_bits((unsigned)width, 12);
    es_bits((unsigned)height, 12);
    es_bits(1, 4);  // pixel aspect ratio code
    es_bits(3, 4);  // 25 fps
    es_bits(20, 18);
    es_bits(1, 1);  // marker
    es_bits(0, 10); // vbv buffer size
    es_bits(0, 1);  // constrained parameters
    es_bits(custom_intra ? 1u : 0u, 1);  // load intra matrix?
    if (custom_intra) {
        for (int scan = 0; scan < 64; scan++) {
            es_bits((unsigned)REF_INTRA_CUSTOM[g_inv_zigzag[scan]], 8);
        }
    }
    es_bits(custom_nonintra ? 1u : 0u, 1);
    if (custom_nonintra) {
        for (int scan = 0; scan < 64; scan++) {
            es_bits((unsigned)REF_NONINTRA_CUSTOM[g_inv_zigzag[scan]], 8);
        }
    }
}

static void es_picture_header(int picture_type) {
    es_start_code(0x00);
    es_bits(0, 10); // temporal reference
    es_bits((unsigned)picture_type, 3);
    es_bits(0, 16); // vbv delay
    if (picture_type == 2) {
        es_bits(1, 1); // forward full pixel
        es_bits(1, 3); // forward f_code = 1
    }
}

// The decoder only hands back a picture once it has located the following
// picture start code (or exhausted the buffer).  A trailing, never-decoded
// picture start code makes the final frame return deterministically, the same
// technique baseline/start.c uses.
static void es_trailing_picture_start(void) {
    es_start_code(0x00);
}

// Emit the AC tokens of one block: walk scan positions, derive run lengths
// from the natural-coordinate model via the generated zigzag table.  Intra
// blocks always end with end_of_block: their AC read starts at n == 1, where
// the decoder treats the "10" codeword as EOB (at n == 0 that same codeword
// would instead be a run-0/level-1 coefficient).
static void emit_block_ac(const block_model_t *bm) {
    int last_scan = 0;
    for (int scan = 1; scan < 64; scan++) {
        int natural = g_inv_zigzag[scan];
        int level = bm->ac[natural];
        if (level == 0) { continue; }
        int run = scan - last_scan - 1;
        emit_coeff(run, level, scan);
        last_scan = scan;
    }
    vlc_dct_eob();
}

// Intra macroblock: address increment 1, intra type, 6 blocks with DC
// prediction.  The block model stores DC in coefficient domain (0 = neutral
// gray); the stream/decoder predictor works in the pixel domain where neutral
// is 128, so the encoded DC value is model_dc + 128.  dc_predictor mirrors the
// decoder's bookkeeping and is always initialized to 128 at a slice start.
static void es_intra_mb(const mb_model_t *mb, int dc_predictor[3]) {
    vlc_mb_increment_one();
    vlc_mb_type_intra();
    for (int b = 0; b < 6; b++) {
        int plane = b < 4 ? 0 : b - 3;
        int encoded_dc = mb->blocks[b].dc + 128;
        int diff = encoded_dc - dc_predictor[plane];
        int size = dc_diff_size(diff);
        vlc_dct_size(plane, size);
        emit_dc_differential(diff);
        dc_predictor[plane] = encoded_dc;
        emit_block_ac(&mb->blocks[b]);
    }
}

// Non-intra macroblock whose luma blocks carry natural-coordinate residual
// levels (chroma blocks are not coded and keep the prediction).  The code
// block pattern is derived from the blocks that actually contain a
// coefficient.  The macroblock type carries no motion vector, so the decoder
// predicts in place from the reference frame.  qscale may be changed per MB.
typedef struct {
    int levels[4][64]; // natural-coordinate levels for luma blocks 0..3
    int qscale;        // 0 keeps the current slice quantizer scale
} inter_mb_model_t;

static void es_inter_mb(const inter_mb_model_t *mb, int *qscale) {
    vlc_mb_increment_one();
    if (mb->qscale != 0 && mb->qscale != *qscale) {
        vlc_mb_type_inter_q_cbp();
        es_bits((unsigned)mb->qscale, 5);
        *qscale = mb->qscale;
    } else {
        vlc_mb_type_inter_cbp();
    }
    // mb types 0x02/0x12 carry no forward motion vector; the decoder leaves
    // the vector at zero and predicts the macroblock in place.  No motion
    // VLC is therefore emitted.
    // Derive the code block pattern from the blocks that actually carry a
    // coefficient.  Every flagged block therefore emits >= 1 real token, so
    // the parser never has to read an end_of_block at scan position zero.
    int cbp = 0;
    for (int b = 0; b < 4; b++) {
        for (int idx = 0; idx < 64; idx++) {
            if (mb->levels[b][idx] != 0) {
                cbp |= 1 << (5 - b); // block b -> bit (5-b)
                break;
            }
        }
    }
    vlc_cbp(cbp);
    for (int b = 0; b < 4; b++) {
        if ((cbp & (1 << (5 - b))) == 0) { continue; }
        int last_scan = 0;
        for (int scan = 0; scan < 64; scan++) {
            int natural = g_inv_zigzag[scan];
            int level = mb->levels[b][natural];
            if (level == 0) { continue; }
            int run = scan - last_scan;
            emit_coeff(run, level, scan);
            last_scan = scan + 1;
        }
        vlc_dct_eob(); // n > 0 guaranteed because the block has a coefficient
    }
}

static void es_slice_begin(int slice_id, int qscale) {
    es_start_code((unsigned)slice_id);
    es_bits((unsigned)qscale, 5);
    es_bits(0, 1); // extra bit slice = 0
}

// ============================================================================
// Decode helper and assertion framework
// ============================================================================

static int g_cases;
static int g_assertions;

static void coeff_fail(const char *scene, const char *fmt, ...) {
    printf("COEFF_ASSERT scene=%s ", scene);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    exit(42);
}

// The sequence header parser refuses streams shorter than 136 bytes, so every
// decode is fed the written bytes plus zero padding.  Decoding stops at the
// sequence end code (0xB7) before the padding is ever interpreted.
#define ES_PAD_BYTES 256

static size_t stream_length(void) {
    return g_bit_pos / 8 + ES_PAD_BYTES;
}

static plm_frame_t *decode_stream(size_t bytes, plm_video_t **out_video,
                                  plm_buffer_t **out_buffer) {
    plm_buffer_t *buffer = plm_buffer_create_with_memory(g_es, bytes, 0);
    plm_video_t *video = plm_video_create_with_buffer(buffer, 1);
    if (!plm_video_has_header(video)) {
        coeff_fail("internal", "sequence header not parsed");
    }
    plm_video_set_no_delay(video, 1);
    plm_frame_t *frame = plm_video_decode(video);
    if (!frame) { coeff_fail("internal", "first frame not decoded"); }
    *out_video = video;
    *out_buffer = buffer;
    return frame;
}

static void destroy_decoder(plm_video_t *video) {
    plm_video_destroy(video); // frees the buffer too (destroy_when_done=1)
}

static void scene_begin(const char *name) {
    g_cases++;
    printf("SCENE %s\n", name);
}

// Compare every pixel of an 8x8 block region of a plane against the expected
// pixel callback.  Tolerance: 0 = exact, 1 = per-pixel +/-1 (never averaged).
typedef int (*pixel_fn)(int x, int y);

static void check_block_region(const char *scene, const char *plane_name,
                               const uint8_t *data, int stride,
                               int origin_x, int origin_y,
                               pixel_fn expected, int tol) {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int actual = data[(origin_y + y) * stride + (origin_x + x)];
            int want = expected(x, y);
            g_assertions++;
            int d = actual - want;
            if (d < 0) { d = -d; }
            if (d > tol) {
                coeff_fail(scene,
                           "%s plane pixel (%d,%d) actual=%d expected=%d tol=%d",
                           plane_name, origin_x + x, origin_y + y,
                           actual, want, tol);
            }
        }
    }
}

static void check_uniform_plane(const char *scene, const char *plane_name,
                                const plm_plane_t *plane, int value, int tol) {
    for (unsigned int y = 0; y < plane->height; y++) {
        for (unsigned int x = 0; x < plane->width; x++) {
            int actual = plane->data[y * plane->width + x];
            g_assertions++;
            int d = actual - value;
            if (d < 0) { d = -d; }
            if (d > tol) {
                coeff_fail(scene, "%s plane (%u,%u) actual=%d expected=%d",
                           plane_name, x, y, actual, value);
            }
        }
    }
}

// Luma block origin in the (macroblock-rounded) luma plane.
static void luma_origin(int mb_col, int mb_row, int block, int lw,
                        int *ox, int *oy) {
    *ox = mb_col * 16 + (block & 1 ? 8 : 0);
    *oy = mb_row * 16 + (block & 2 ? 8 : 0);
    (void)lw;
}

static void chroma_origin(int mb_col, int mb_row, int cw,
                          int *ox, int *oy) {
    *ox = mb_col * 8;
    *oy = mb_row * 8;
    (void)cw;
}

// ============================================================================
// Scenarios
// ============================================================================

static block_model_t dc_block(int dc) {
    block_model_t bm;
    memset(&bm, 0, sizeof(bm));
    bm.dc = dc;
    return bm;
}

static block_model_t ac_block(int dc, const int natural_ac[64]) {
    block_model_t bm;
    memset(&bm, 0, sizeof(bm));
    bm.dc = dc;
    if (natural_ac) { memcpy(bm.ac, natural_ac, sizeof(bm.ac)); }
    return bm;
}

// --- Scene 1: DC only, default matrices, distinct chroma DC -----------------

static void scene_intra_dc_only_default(void) {
    const char *name = "intra_dc_only_default";
    scene_begin(name);
    g_bit_pos = 0;
    memset(g_es, 0, sizeof(g_es));

    es_sequence_header(16, 16, 0, 0);
    es_picture_header(1);
    es_slice_begin(1, 8);

    mb_model_t mb;
    memset(&mb, 0, sizeof(mb));
    for (int b = 0; b < 4; b++) { mb.blocks[b] = dc_block(4); }
    mb.blocks[4] = dc_block(8);   // Cb
    mb.blocks[5] = dc_block(-6);  // Cr
    int pred[3] = {128, 128, 128};
    es_intra_mb(&mb, pred);
    es_trailing_picture_start();
    es_start_code(0xB7); // sequence end

    plm_video_t *video;
    plm_buffer_t *buffer;
    plm_frame_t *frame = decode_stream(stream_length(), &video, &buffer);

    // Exact: DC only reconstructs to 128 + dc on every pixel.
    check_uniform_plane(name, "Y", &frame->y, 128 + 4, 0);
    check_uniform_plane(name, "Cb", &frame->cb, 128 + 8, 0);
    check_uniform_plane(name, "Cr", &frame->cr, 128 - 6, 0);

    destroy_decoder(video);
}

// --- Scene 2: multi-slice DC prediction and slice resets -------------------

static void scene_intra_multislice_dc(void) {
    const char *name = "intra_multislice_dc";
    scene_begin(name);
    g_bit_pos = 0;
    memset(g_es, 0, sizeof(g_es));

    // 32x32 = 4 macroblocks in a 2x2 grid; each macroblock row is one slice.
    es_sequence_header(32, 32, 0, 0);
    es_picture_header(1);

    // Slice 1 (macroblock row 0).  DC prediction carries across the two MBs.
    es_slice_begin(1, 8);
    mb_model_t mb0;
    memset(&mb0, 0, sizeof(mb0));
    for (int b = 0; b < 4; b++) { mb0.blocks[b] = dc_block(10); }
    mb0.blocks[4] = dc_block(5);   // Cb
    mb0.blocks[5] = dc_block(-5);  // Cr
    int pred[3] = {128, 128, 128};
    es_intra_mb(&mb0, pred);

    mb_model_t mb1;
    memset(&mb1, 0, sizeof(mb1));
    for (int b = 0; b < 4; b++) { mb1.blocks[b] = dc_block(2); }
    mb1.blocks[4] = dc_block(7);
    mb1.blocks[5] = dc_block(-3);
    es_intra_mb(&mb1, pred);

    // Slice 2 (macroblock row 1): predictors reset to 128 at the slice start.
    // With the reset, a +2 differential lands on encoded DC 130 (model dc 2);
    // without it the luma predictor would still be 130 from MB1 and the same
    // differential would produce a different block.
    es_slice_begin(2, 8);
    mb_model_t mb2;
    memset(&mb2, 0, sizeof(mb2));
    for (int b = 0; b < 4; b++) { mb2.blocks[b] = dc_block(2); }
    mb2.blocks[4] = dc_block(4);
    mb2.blocks[5] = dc_block(-4);
    int pred2[3] = {128, 128, 128};
    es_intra_mb(&mb2, pred2);

    mb_model_t mb3;
    memset(&mb3, 0, sizeof(mb3));
    for (int b = 0; b < 4; b++) { mb3.blocks[b] = dc_block(3); }
    mb3.blocks[4] = dc_block(5);
    mb3.blocks[5] = dc_block(-3);
    es_intra_mb(&mb3, pred2);
    es_trailing_picture_start();
    es_start_code(0xB7);

    plm_video_t *video;
    plm_buffer_t *buffer;
    plm_frame_t *frame = decode_stream(stream_length(), &video, &buffer);

    // Luma row 0: MB0 = 128+10, MB1 = 128+2.
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            g_assertions++;
            if (frame->y.data[y * frame->y.width + x] != 138) {
                coeff_fail(name, "Y MB0 (%d,%d) actual=%d expected=138",
                           x, y, frame->y.data[y * frame->y.width + x]);
            }
        }
        for (int x = 16; x < 32; x++) {
            g_assertions++;
            if (frame->y.data[y * frame->y.width + x] != 130) {
                coeff_fail(name, "Y MB1 (%d,%d) actual=%d expected=130",
                           x, y, frame->y.data[y * frame->y.width + x]);
            }
        }
    }
    // Luma row 1: slice reset proved by DC 130/131 -> pixels 130/131.
    for (int y = 16; y < 32; y++) {
        for (int x = 0; x < 16; x++) {
            g_assertions++;
            if (frame->y.data[y * frame->y.width + x] != 130) {
                coeff_fail(name, "Y MB2 slice reset (%d,%d) actual=%d",
                           x, y, frame->y.data[y * frame->y.width + x]);
            }
        }
        for (int x = 16; x < 32; x++) {
            g_assertions++;
            if (frame->y.data[y * frame->y.width + x] != 131) {
                coeff_fail(name, "Y MB3 (%d,%d) actual=%d expected=131",
                           x, y, frame->y.data[y * frame->y.width + x]);
            }
        }
    }
    // Chroma plane is 16x16: one 8x8 block per macroblock.
    struct { int mx, my, cb, cr; } expect[4] = {
        {0, 0, 133, 123}, {1, 0, 135, 125},
        {0, 1, 132, 124}, {1, 1, 133, 125}
    };
    for (int m = 0; m < 4; m++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int px = expect[m].mx * 8 + x;
                int py = expect[m].my * 8 + y;
                g_assertions++;
                if (frame->cb.data[py * 16 + px] != expect[m].cb) {
                    coeff_fail(name, "Cb MB%d (%d,%d) actual=%d expected=%d",
                               m, px, py, frame->cb.data[py * 16 + px],
                               expect[m].cb);
                }
                if (frame->cr.data[py * 16 + px] != expect[m].cr) {
                    coeff_fail(name, "Cr MB%d (%d,%d) actual=%d expected=%d",
                               m, px, py, frame->cr.data[py * 16 + px],
                               expect[m].cr);
                }
            }
        }
    }

    destroy_decoder(video);
}

// --- Shared AC model used by the default/custom matrix control pair --------

// Natural-coordinate AC patterns.  Small amplitudes keep every reconstructed
// pixel within +/-1 of the double precision reference.
static int ac_horiz_neg[64];   // (0,1) = -2 : horizontal basis
static int ac_vert_pos[64];    // (1,0) = +2 : vertical basis
static int ac_run_escape[64];  // scan-run gap then an escape coded level
static int ac_chroma_cb[64];   // Cb block pattern
static int ac_chroma_cr[64];   // Cr block pattern
static int ac_matrix_probe_a[64]; // natural (u=1,v=2), small level
static int ac_matrix_probe_b[64]; // natural (u=3,v=0), small level

static void init_ac_models(void) {
    ac_horiz_neg[0 * 8 + 1] = -2;
    ac_vert_pos[1 * 8 + 0] = 2;

    // (2,0) is reached after two skipped scan positions relative to (0,1);
    // level 3 is outside... actually inside direct VLC, so force the escape
    // path explicitly by choosing level 5 with a run.
    ac_run_escape[0 * 8 + 1] = 1;
    ac_run_escape[2 * 8 + 0] = 5; // encoded via escape with a run

    ac_chroma_cb[0 * 8 + 2] = 2;
    ac_chroma_cr[1 * 8 + 1] = -2;

    ac_matrix_probe_a[1 * 8 + 2] = 3; // natural (u=1,v=2)
    ac_matrix_probe_b[3 * 8 + 0] = 3; // natural (u=3,v=0)
}

typedef struct {
    int F[64];
    double f[64];
    int base;       // 128 for intra, prediction value for inter
    int is_inter;
} ref_block_t;

static ref_block_t make_ref_intra(const block_model_t *bm, int qscale,
                                  const int matrix[64]) {
    ref_block_t r;
    memset(&r, 0, sizeof(r));
    ref_dequant_intra(bm->dc, bm->ac, qscale, matrix, r.F);
    ref_idct_2d(r.F, r.f);
    r.base = 128;
    r.is_inter = 0;
    return r;
}

static ref_block_t make_ref_inter(const int levels[64], int base, int qscale,
                                  const int matrix[64]) {
    ref_block_t r;
    memset(&r, 0, sizeof(r));
    ref_dequant_inter(levels, qscale, matrix, r.F);
    ref_idct_2d(r.F, r.f);
    r.base = base;
    r.is_inter = 1;
    return r;
}

// Expected pixel (x = column, y = row) of a reconstructed block, taking the
// prediction base (128 for intra; the known prediction for inter) and the
// final clamping into account.
static int ref_pixel_cb(const ref_block_t *r, int x, int y) {
    double v = r->is_inter ? (double)r->base + r->f[y * 8 + x]
                           : 128.0 + r->f[y * 8 + x];
    return clamp_u8(round_near(v));
}

static int pixel_callback_adapter(int x, int y); // fwd
static const ref_block_t *g_cb_ref;

static int pixel_callback_adapter(int x, int y) {
    return ref_pixel_cb(g_cb_ref, x, y);
}

static void check_ref_block(const char *scene, const char *plane_name,
                            const uint8_t *data, int stride,
                            int ox, int oy, const ref_block_t *r, int tol) {
    g_cb_ref = r;
    check_block_region(scene, plane_name, data, stride, ox, oy,
                       pixel_callback_adapter, tol);
}

// --- Scene 3: horizontal/vertical AC bases, default intra matrix ------------

static void scene_intra_ac_default(void) {
    const char *name = "intra_ac_default";
    scene_begin(name);
    g_bit_pos = 0;
    memset(g_es, 0, sizeof(g_es));

    es_sequence_header(16, 16, 0, 0);
    es_picture_header(1);
    es_slice_begin(1, 4);

    mb_model_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.blocks[0] = ac_block(0, ac_horiz_neg); // top-left:     horizontal
    mb.blocks[1] = ac_block(0, ac_vert_pos);  // top-right:    vertical
    mb.blocks[2] = dc_block(0);
    mb.blocks[3] = dc_block(0);
    mb.blocks[4] = dc_block(0);
    mb.blocks[5] = dc_block(0);
    int pred[3] = {128, 128, 128};
    es_intra_mb(&mb, pred);
    es_trailing_picture_start();
    es_start_code(0xB7);

    plm_video_t *video;
    plm_buffer_t *buffer;
    plm_frame_t *frame = decode_stream(stream_length(), &video, &buffer);

    ref_block_t r0 = make_ref_intra(&mb.blocks[0], 4, REF_INTRA_DEFAULT);
    ref_block_t r1 = make_ref_intra(&mb.blocks[1], 4, REF_INTRA_DEFAULT);
    int ox, oy;
    luma_origin(0, 0, 0, frame->y.width, &ox, &oy);
    check_ref_block(name, "Y", frame->y.data, (int)frame->y.width,
                    ox, oy, &r0, 1);
    luma_origin(0, 0, 1, frame->y.width, &ox, &oy);
    check_ref_block(name, "Y", frame->y.data, (int)frame->y.width,
                    ox, oy, &r1, 1);

    // untouched blocks stay exactly at 128
    g_assertions++;
    if (frame->y.data[8 * frame->y.width + 8] != 128) {
        coeff_fail(name, "untouched luma block drifted: %d",
                   frame->y.data[8 * frame->y.width + 8]);
    }

    destroy_decoder(video);
}

// --- Scene 4: run + escape coded coefficients ------------------------------

static void scene_intra_run_escape(void) {
    const char *name = "intra_run_escape_default";
    scene_begin(name);
    g_bit_pos = 0;
    memset(g_es, 0, sizeof(g_es));

    es_sequence_header(16, 16, 0, 0);
    es_picture_header(1);
    es_slice_begin(1, 4);

    mb_model_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.blocks[0] = ac_block(0, ac_run_escape);
    for (int b = 1; b < 6; b++) { mb.blocks[b] = dc_block(0); }
    int pred[3] = {128, 128, 128};
    es_intra_mb(&mb, pred);
    es_trailing_picture_start();
    es_start_code(0xB7);

    plm_video_t *video;
    plm_buffer_t *buffer;
    plm_frame_t *frame = decode_stream(stream_length(), &video, &buffer);

    ref_block_t r = make_ref_intra(&mb.blocks[0], 4, REF_INTRA_DEFAULT);
    int ox = 0, oy = 0;
    check_ref_block(name, "Y", frame->y.data, (int)frame->y.width,
                    ox, oy, &r, 1);

    destroy_decoder(video);
}

// --- Scene 5: same natural coefficients, asymmetric custom intra matrix ----

static plm_frame_t *build_custom_intra_stream(size_t *out_bytes,
                                               plm_video_t **out_video) {
    g_bit_pos = 0;
    memset(g_es, 0, sizeof(g_es));
    es_sequence_header(16, 16, 1, 0);
    es_picture_header(1);
    es_slice_begin(1, 4);

    mb_model_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.blocks[0] = ac_block(0, ac_horiz_neg);
    mb.blocks[1] = ac_block(0, ac_vert_pos);
    for (int b = 2; b < 6; b++) { mb.blocks[b] = dc_block(0); }
    int pred[3] = {128, 128, 128};
    es_intra_mb(&mb, pred);
    es_trailing_picture_start();
    es_start_code(0xB7);
    *out_bytes = g_bit_pos / 8;

    plm_video_t *video;
    plm_buffer_t *buffer;
    plm_frame_t *frame = decode_stream(stream_length(), &video, &buffer);
    *out_video = video;
    return frame;
}

static void scene_intra_ac_custom_matrix(void) {
    const char *name = "intra_ac_custom_asymmetric_matrix";
    scene_begin(name);

    // Decode the custom-matrix stream.
    size_t bytes_custom;
    plm_video_t *video_c;
    plm_frame_t *fc = build_custom_intra_stream(&bytes_custom, &video_c);
    (void)bytes_custom;

    mb_model_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.blocks[0] = ac_block(0, ac_horiz_neg);
    mb.blocks[1] = ac_block(0, ac_vert_pos);
    for (int b = 2; b < 6; b++) { mb.blocks[b] = dc_block(0); }

    ref_block_t r0 = make_ref_intra(&mb.blocks[0], 4, REF_INTRA_CUSTOM);
    ref_block_t r1 = make_ref_intra(&mb.blocks[1], 4, REF_INTRA_CUSTOM);
    int ox, oy;
    luma_origin(0, 0, 0, fc->y.width, &ox, &oy);
    check_ref_block(name, "Y", fc->y.data, (int)fc->y.width, ox, oy, &r0, 1);
    luma_origin(0, 0, 1, fc->y.width, &ox, &oy);
    check_ref_block(name, "Y", fc->y.data, (int)fc->y.width, ox, oy, &r1, 1);

    // Control: rebuild the IDENTICAL coefficient model with default matrices
    // and confirm the asymmetric matrix changed pixels in the targeted block,
    // while an untouched block (block 2) is byte-identical between the two.
    g_bit_pos = 0;
    memset(g_es, 0, sizeof(g_es));
    es_sequence_header(16, 16, 0, 0);
    es_picture_header(1);
    es_slice_begin(1, 4);
    int pred[3] = {128, 128, 128};
    es_intra_mb(&mb, pred);
    es_trailing_picture_start();
    es_start_code(0xB7);

    plm_video_t *video_d;
    plm_buffer_t *buffer_d;
    plm_frame_t *fd = decode_stream(stream_length(), &video_d, &buffer_d);

    int changed = 0;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            g_assertions++;
            int a = fc->y.data[y * fc->y.width + x];
            int b = fd->y.data[y * fc->y.width + x];
            if (a != b) { changed++; }
        }
    }
    if (changed == 0) {
        coeff_fail(name, "custom matrix produced no pixel change in block 0");
    }
    // untouched block 2 must be identical in both decodes
    for (int y = 8; y < 16; y++) {
        for (int x = 0; x < 8; x++) {
            g_assertions++;
            if (fc->y.data[y * fc->y.width + x] !=
                fd->y.data[y * fc->y.width + x]) {
                coeff_fail(name,
                           "untouched block 2 differs between matrix variants");
            }
        }
    }

    destroy_decoder(video_c);
    destroy_decoder(video_d);
}

// --- Scene 5b: custom-matrix zigzag remap at sensitive frequency positions --
//
// The custom matrix is asymmetric, so for these natural positions the weight
// the decoder must use (zigzag remapped) differs from the weight it would use
// if the remap were skipped.  The levels stay small, yet the permutation moves
// pixels by more than +/-1, which makes a missing zigzag remap fail the
// per-pixel tolerance instead of hiding inside it.
static void scene_intra_matrix_zigzag_remap(void) {
    const char *name = "intra_matrix_zigzag_remap";
    scene_begin(name);
    g_bit_pos = 0;
    memset(g_es, 0, sizeof(g_es));

    es_sequence_header(16, 16, 1, 0);
    es_picture_header(1);
    es_slice_begin(1, 4);

    mb_model_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.blocks[0] = ac_block(0, ac_matrix_probe_a); // (1,2) level 3
    mb.blocks[1] = ac_block(0, ac_matrix_probe_b); // (3,0) level 3
    for (int b = 2; b < 6; b++) { mb.blocks[b] = dc_block(0); }
    int pred[3] = {128, 128, 128};
    es_intra_mb(&mb, pred);
    es_trailing_picture_start();
    es_start_code(0xB7);

    plm_video_t *video;
    plm_buffer_t *buffer;
    plm_frame_t *frame = decode_stream(stream_length(), &video, &buffer);

    ref_block_t r0 = make_ref_intra(&mb.blocks[0], 4, REF_INTRA_CUSTOM);
    ref_block_t r1 = make_ref_intra(&mb.blocks[1], 4, REF_INTRA_CUSTOM);
    int ox, oy;
    luma_origin(0, 0, 0, frame->y.width, &ox, &oy);
    check_ref_block(name, "Y", frame->y.data, (int)frame->y.width,
                    ox, oy, &r0, 1);
    luma_origin(0, 0, 1, frame->y.width, &ox, &oy);
    check_ref_block(name, "Y", frame->y.data, (int)frame->y.width,
                    ox, oy, &r1, 1);

    destroy_decoder(video);
}

// --- Scene 6: workspace isolation between consecutive AC blocks -------------

static void scene_workspace_two_ac_blocks(void) {
    const char *name = "workspace_consecutive_ac_blocks";
    scene_begin(name);
    g_bit_pos = 0;
    memset(g_es, 0, sizeof(g_es));

    es_sequence_header(16, 16, 0, 0);
    es_picture_header(1);
    es_slice_begin(1, 4);

    mb_model_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.blocks[0] = ac_block(0, ac_horiz_neg); // (0,1) horizontal energy
    mb.blocks[1] = ac_block(0, ac_vert_pos);  // (1,0) vertical energy
    for (int b = 2; b < 6; b++) { mb.blocks[b] = dc_block(0); }
    int pred[3] = {128, 128, 128};
    es_intra_mb(&mb, pred);
    es_trailing_picture_start();
    es_start_code(0xB7);

    plm_video_t *video;
    plm_buffer_t *buffer;
    plm_frame_t *frame = decode_stream(stream_length(), &video, &buffer);

    ref_block_t r0 = make_ref_intra(&mb.blocks[0], 4, REF_INTRA_DEFAULT);
    ref_block_t r1 = make_ref_intra(&mb.blocks[1], 4, REF_INTRA_DEFAULT);
    int ox, oy;
    luma_origin(0, 0, 0, frame->y.width, &ox, &oy);
    check_ref_block(name, "Y", frame->y.data, (int)frame->y.width,
                    ox, oy, &r0, 1);
    luma_origin(0, 0, 1, frame->y.width, &ox, &oy);
    check_ref_block(name, "Y", frame->y.data, (int)frame->y.width,
                    ox, oy, &r1, 1);

    // Independent decoder that only contains block 1's pattern: block 1 in
    // the combined stream must match it pixel for pixel (no leakage from
    // block 0's transform workspace).
    g_bit_pos = 0;
    memset(g_es, 0, sizeof(g_es));
    es_sequence_header(16, 16, 0, 0);
    es_picture_header(1);
    es_slice_begin(1, 4);
    mb_model_t mb_only;
    memset(&mb_only, 0, sizeof(mb_only));
    mb_only.blocks[0] = dc_block(0);
    mb_only.blocks[1] = ac_block(0, ac_vert_pos);
    for (int b = 2; b < 6; b++) { mb_only.blocks[b] = dc_block(0); }
    int pred2[3] = {128, 128, 128};
    es_intra_mb(&mb_only, &pred2[0]);
    es_trailing_picture_start();
    es_start_code(0xB7);

    plm_video_t *video2;
    plm_buffer_t *buffer2;
    plm_frame_t *f2 = decode_stream(stream_length(), &video2, &buffer2);

    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            g_assertions++;
            int a = frame->y.data[y * frame->y.width + 8 + x];
            int b = f2->y.data[y * f2->y.width + 8 + x];
            if (a != b) {
                coeff_fail(name,
                           "block 1 contaminated by block 0 workspace at "
                           "(%d,%d) combined=%d isolated=%d",
                           x, y, a, b);
            }
        }
    }

    destroy_decoder(video);
    destroy_decoder(video2);
}

// --- Scene 7: non-intra DC at the oddify/rounding critical point + clips ----

static void scene_inter_dc_critical_and_clipping(void) {
    const char *name = "inter_dc_oddify_rounding_and_clips";
    scene_begin(name);
    g_bit_pos = 0;
    memset(g_es, 0, sizeof(g_es));

    // 48x16 = 3 macroblocks.  I frame paints a uniform base of 100
    // (intra DC = -28, exact: pixel = 128 - 28 = 100).
    es_sequence_header(48, 16, 0, 1); // custom non-intra matrix also loaded
    es_picture_header(1);
    es_slice_begin(1, 8);
    int ipred[3] = {128, 128, 128};
    for (int m = 0; m < 3; m++) {
        mb_model_t mb;
        memset(&mb, 0, sizeof(mb));
        for (int b = 0; b < 6; b++) { mb.blocks[b] = dc_block(-28); }
        es_intra_mb(&mb, ipred);
    }

    // P picture, custom non-intra matrix has m[0][0] = 16 like the default.
    //  MB0: qscale 4, single non-intra DC level +1.  Hand calculation:
    //         widened = 2*1+1 = 3
    //         raw t   = 3*4*16 >> 4 = 12          (EVEN)
    //         oddify  -> 11                        (even->odd rule)
    //         bypass delta = (11*32 + 128) >> 8 = 1
    //       This is the rounding critical point: with the even->odd adjustment
    //       removed, t stays 12 and the delta becomes (12*32+128)>>8 = 2, so
    //       the output pixel would be 102 instead of the hand value 101.
    //  MB1: qscale 16, level +127 -> t clips to +2047 -> positive saturation.
    //  MB2: qscale 16, level -127 -> t clips to -2048 -> negative saturation.
    es_picture_header(2);
    es_slice_begin(1, 4);
    int qscale = 4;
    inter_mb_model_t p0;
    memset(&p0, 0, sizeof(p0));
    for (int b = 0; b < 4; b++) { p0.levels[b][0] = 1; }
    p0.qscale = 4;
    es_inter_mb(&p0, &qscale);

    inter_mb_model_t p1;
    memset(&p1, 0, sizeof(p1));
    for (int b = 0; b < 4; b++) { p1.levels[b][0] = 127; }
    p1.qscale = 16;
    es_inter_mb(&p1, &qscale);

    inter_mb_model_t p2;
    memset(&p2, 0, sizeof(p2));
    for (int b = 0; b < 4; b++) { p2.levels[b][0] = -127; }
    p2.qscale = 16;
    es_inter_mb(&p2, &qscale);
    es_trailing_picture_start();
    es_start_code(0xB7);

    plm_buffer_t *buffer = plm_buffer_create_with_memory(g_es, stream_length(), 0);
    plm_video_t *video = plm_video_create_with_buffer(buffer, 1);
    plm_video_set_no_delay(video, 1);
    plm_frame_t *fi = plm_video_decode(video);
    if (!fi) { coeff_fail(name, "I frame missing"); }
    check_uniform_plane(name, "I:Y", &fi->y, 100, 0);

    plm_frame_t *fp = plm_video_decode(video);
    if (!fp) { coeff_fail(name, "P frame missing"); }

    // MB0: exact hand-computed value 101 on all four luma blocks.
    for (int by = 0; by < 16; by++) {
        for (int bx = 0; bx < 16; bx++) {
            g_assertions++;
            int actual = fp->y.data[by * fp->y.width + bx];
            if (actual != 101) {
                coeff_fail(name, "critical MB0 (%d,%d) actual=%d expected=101",
                           bx, by, actual);
            }
        }
    }
    // MB1: positive clamp to 255.
    for (int by = 0; by < 16; by++) {
        for (int bx = 16; bx < 32; bx++) {
            g_assertions++;
            int actual = fp->y.data[by * fp->y.width + bx];
            if (actual != 255) {
                coeff_fail(name, "positive clip MB1 (%d,%d) actual=%d",
                           bx, by, actual);
            }
        }
    }
    // MB2: negative clamp to 0.
    for (int by = 0; by < 16; by++) {
        for (int bx = 32; bx < 48; bx++) {
            g_assertions++;
            int actual = fp->y.data[by * fp->y.width + bx];
            if (actual != 0) {
                coeff_fail(name, "negative clip MB2 (%d,%d) actual=%d",
                           bx, by, actual);
            }
        }
    }
    // Chroma was never coded in the P picture: prediction stays at the base.
    check_uniform_plane(name, "P:Cb", &fp->cb, 100, 0);
    check_uniform_plane(name, "P:Cr", &fp->cr, 100, 0);

    // Independently recompute the critical block with the double reference
    // (inter DC, custom non-intra matrix, qscale 4) and require exact match.
    int levels[64] = {0};
    levels[0] = 1;
    ref_block_t rc = make_ref_inter(levels, 100, 4, REF_NONINTRA_CUSTOM);
    g_assertions++;
    if (ref_pixel_cb(&rc, 0, 0) != 101) {
        coeff_fail(name, "reference hand calc mismatch: got %d",
                   ref_pixel_cb(&rc, 0, 0));
    }

    plm_video_destroy(video);
}

// --- Scene 8: inter AC residual added to a known prediction -----------------

static void scene_inter_ac_residual(void) {
    const char *name = "inter_ac_residual_add";
    scene_begin(name);
    g_bit_pos = 0;
    memset(g_es, 0, sizeof(g_es));

    // I frame: flat 128 base (DC 0).
    es_sequence_header(16, 16, 0, 0);
    es_picture_header(1);
    es_slice_begin(1, 8);
    mb_model_t imb;
    memset(&imb, 0, sizeof(imb));
    for (int b = 0; b < 6; b++) { imb.blocks[b] = dc_block(0); }
    int pred[3] = {128, 128, 128};
    es_intra_mb(&imb, pred);

    // P frame: horizontal and vertical AC residuals on luma blocks 0 and 1,
    // small amplitudes, cbp derived from the coded blocks.
    es_picture_header(2);
    es_slice_begin(1, 4);
    int qscale = 4;
    inter_mb_model_t pmb;
    memset(&pmb, 0, sizeof(pmb));
    memcpy(pmb.levels[0], ac_horiz_neg, sizeof(ac_horiz_neg));
    memcpy(pmb.levels[1], ac_vert_pos, sizeof(ac_vert_pos));
    pmb.qscale = 0;
    es_inter_mb(&pmb, &qscale);
    es_trailing_picture_start();
    es_start_code(0xB7);

    plm_buffer_t *buffer = plm_buffer_create_with_memory(g_es, stream_length(), 0);
    plm_video_t *video = plm_video_create_with_buffer(buffer, 1);
    plm_video_set_no_delay(video, 1);
    plm_frame_t *fi = plm_video_decode(video);
    if (!fi) { coeff_fail(name, "I frame missing"); }
    plm_frame_t *fp = plm_video_decode(video);
    if (!fp) { coeff_fail(name, "P frame missing"); }

    ref_block_t r0 = make_ref_inter(pmb.levels[0], 128, 4,
                                    REF_NONINTRA_DEFAULT);
    ref_block_t r1 = make_ref_inter(pmb.levels[1], 128, 4,
                                    REF_NONINTRA_DEFAULT);
    int ox, oy;
    luma_origin(0, 0, 0, fp->y.width, &ox, &oy);
    check_ref_block(name, "Y", fp->y.data, (int)fp->y.width, ox, oy, &r0, 1);
    luma_origin(0, 0, 1, fp->y.width, &ox, &oy);
    check_ref_block(name, "Y", fp->y.data, (int)fp->y.width, ox, oy, &r1, 1);

    // blocks 2/3 carry no residual energy: remain exactly at the prediction
    g_assertions++;
    if (fp->y.data[8 * fp->y.width + 8] != 128) {
        coeff_fail(name, "uncoded inter block drifted: %d",
                   fp->y.data[8 * fp->y.width + 8]);
    }

    plm_video_destroy(video);
}

// --- Scene 9: independent Cb/Cr AC checks ----------------------------------

static void scene_intra_chroma_ac(void) {
    const char *name = "intra_chroma_ac_independent";
    scene_begin(name);
    g_bit_pos = 0;
    memset(g_es, 0, sizeof(g_es));

    es_sequence_header(16, 16, 0, 0);
    es_picture_header(1);
    es_slice_begin(1, 4);

    mb_model_t mb;
    memset(&mb, 0, sizeof(mb));
    for (int b = 0; b < 4; b++) { mb.blocks[b] = dc_block(0); }
    mb.blocks[4] = ac_block(3, ac_chroma_cb);
    mb.blocks[5] = ac_block(-4, ac_chroma_cr);
    int pred[3] = {128, 128, 128};
    es_intra_mb(&mb, pred);
    es_trailing_picture_start();
    es_start_code(0xB7);

    plm_video_t *video;
    plm_buffer_t *buffer;
    plm_frame_t *frame = decode_stream(stream_length(), &video, &buffer);

    ref_block_t rcb = make_ref_intra(&mb.blocks[4], 4, REF_INTRA_DEFAULT);
    ref_block_t rcr = make_ref_intra(&mb.blocks[5], 4, REF_INTRA_DEFAULT);
    int ox, oy;
    chroma_origin(0, 0, (int)frame->cb.width, &ox, &oy);
    check_ref_block(name, "Cb", frame->cb.data, (int)frame->cb.width,
                    ox, oy, &rcb, 1);
    check_ref_block(name, "Cr", frame->cr.data, (int)frame->cr.width,
                    ox, oy, &rcr, 1);

    // Luma must stay exactly flat.
    check_uniform_plane(name, "Y", &frame->y, 128, 0);

    destroy_decoder(video);
}

// --- Scene 10: custom asymmetric non-intra matrix control -------------------

static void scene_inter_custom_nonintra_matrix(void) {
    const char *name = "inter_custom_nonintra_matrix";
    scene_begin(name);
    g_bit_pos = 0;
    memset(g_es, 0, sizeof(g_es));

    es_sequence_header(16, 16, 1, 1);
    es_picture_header(1);
    es_slice_begin(1, 8);
    mb_model_t imb;
    memset(&imb, 0, sizeof(imb));
    for (int b = 0; b < 6; b++) { imb.blocks[b] = dc_block(0); }
    int pred[3] = {128, 128, 128};
    es_intra_mb(&imb, pred);

    es_picture_header(2);
    es_slice_begin(1, 4);
    int qscale = 4;
    inter_mb_model_t pmb;
    memset(&pmb, 0, sizeof(pmb));
    memcpy(pmb.levels[0], ac_horiz_neg, sizeof(ac_horiz_neg));
    memcpy(pmb.levels[1], ac_vert_pos, sizeof(ac_vert_pos));
    pmb.qscale = 0;
    es_inter_mb(&pmb, &qscale);
    es_trailing_picture_start();
    es_start_code(0xB7);

    plm_buffer_t *buffer = plm_buffer_create_with_memory(g_es, stream_length(), 0);
    plm_video_t *video = plm_video_create_with_buffer(buffer, 1);
    plm_video_set_no_delay(video, 1);
    plm_frame_t *fi = plm_video_decode(video);
    if (!fi) { coeff_fail(name, "I frame missing"); }
    plm_frame_t *fp = plm_video_decode(video);
    if (!fp) { coeff_fail(name, "P frame missing"); }

    ref_block_t r0 = make_ref_inter(pmb.levels[0], 128, 4,
                                    REF_NONINTRA_CUSTOM);
    ref_block_t r1 = make_ref_inter(pmb.levels[1], 128, 4,
                                    REF_NONINTRA_CUSTOM);
    int ox, oy;
    luma_origin(0, 0, 0, fp->y.width, &ox, &oy);
    check_ref_block(name, "Y", fp->y.data, (int)fp->y.width, ox, oy, &r0, 1);
    luma_origin(0, 0, 1, fp->y.width, &ox, &oy);
    check_ref_block(name, "Y", fp->y.data, (int)fp->y.width, ox, oy, &r1, 1);

    plm_video_destroy(video);
}

// ============================================================================
// Entry point
// ============================================================================

int main(void) {
    reference_init_zigzag();
    reference_init_matrices();
    init_ac_models();

    // Sanity: the generated zigzag must match the well-known first/last
    // scan positions (checked against the rule, not the library table).
    if (g_inv_zigzag[0] != 0 || g_inv_zigzag[1] != 1 ||
        g_inv_zigzag[2] != 8 || g_inv_zigzag[63] != 63) {
        coeff_fail("zigzag_self_check", "diagonal scan generated wrongly");
    }

    scene_intra_dc_only_default();
    scene_intra_multislice_dc();
    scene_intra_ac_default();
    scene_intra_run_escape();
    scene_intra_ac_custom_matrix();
    scene_intra_matrix_zigzag_remap();
    scene_workspace_two_ac_blocks();
    scene_inter_dc_critical_and_clipping();
    scene_inter_ac_residual();
    scene_intra_chroma_ac();
    scene_inter_custom_nonintra_matrix();

    if (g_cases == 0) {
        coeff_fail("internal", "no scenarios ran");
    }
    printf("MPEG_COEFF_OK cases=%d assertions=%d\n", g_cases, g_assertions);
    return 0;
}
