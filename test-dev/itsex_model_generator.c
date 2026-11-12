/*
 * IT 2.14/2.15 compressed sample model generator.
 *
 * The format combines three independent pieces of state, and the
 * generator keeps them separate so a regression in one is visible
 * without looking like a failure of another:
 *
 *   1. Bit width control ("left"): the current code width starts at 9
 *      (8-bit) or 17 (16-bit) on every new compression block and is
 *      changed only by explicit control symbols. The three control
 *      representations are the low-width sentinel plus a following
 *      width field, the middle-width reserved interval, and the
 *      high-bit control at the initial maximum width. A control symbol
 *      changes the read width but never produces a sample.
 *   2. Byte/block framing: each block begins with a uint16 little-endian
 *      payload length; the bitstream is LSB-first inside the payload.
 *      Zero padding bits in the final payload byte are never read as
 *      code words of the next block, and every new block starts its own
 *      width and accumulator state regardless of the previous block.
 *   3. Two layers of unsigned modular differential accumulators:
 *      temp  = (temp + delta) mod M          (first difference)
 *      temp2 = (temp2 + temp) mod M          (IT 2.15 second difference)
 *      with M = 256 for 8-bit and 65536 for 16-bit. Every block resets
 *      both accumulators to zero. All arithmetic below is unsigned and
 *      explicitly modulo M, so expected wraparound is visible directly
 *      in the expected arrays; no signed overflow or negative shift is
 *      used.
 */
#include <stdlib.h>
#include <string.h>

/*
 * Shared model interface, kept in the two delivered translation units
 * rather than a third header. test_itsex_model.c carries the same
 * declaration block.
 */
#define ITSEX_NWIDTH 18		/* Widths 1..17 plus the unused slot 0. */

struct itsex_model_stats {
    size_t width_data[ITSEX_NWIDTH];  /* Data symbols per code width. */
    size_t switches[3];               /* Low sentinel / middle / high. */
    size_t control_byte_crossings;    /* Control field straddles a byte. */
    size_t control_cache_crossings;   /* Control field straddles a 32-bit
                                         cache refill boundary. */
};

struct itsex_stream_model {
    int bits;             /* 8 or 16. */
    int it215;            /* 0 = IT 2.14, 1 = IT 2.15. */
    int nblocks;
    size_t length;        /* Total output samples. */
    uint32_t modulus;     /* 256 or 65536, expressed explicitly. */

    /* Packed stream: sequence of blocks, each a uint16 LE length prefix
     * followed by that many payload bytes. */
    uint8_t *packed;
    size_t packed_len;

    /* Expected decoder output, modular bit patterns (0..modulus-1). */
    uint32_t *expected;

    /* Accumulator state at block end; used to prove that no state
     * leaks across independent blocks. */
    uint32_t block_end_temp;     /* Final block end (both accumulators). */
    uint32_t block_end_temp2;
    uint32_t block1_end_temp;    /* First block end, for block count > 1. */
    uint32_t block1_end_temp2;
    size_t block1_end;    /* Sample index of the first block boundary. */

    struct itsex_model_stats stats;
};

struct itsex_stereo_model {
    int bits;
    int it215;
    size_t channel_length;   /* Samples per channel. */
    struct itsex_stream_model left;
    struct itsex_stream_model right;
    uint8_t *packed;         /* Left stream followed by right stream. */
    size_t packed_len;
};

/* Length classes. SHORT is 61 samples, FULL is exactly one compression
 * block (32768 for 8-bit, 16384 for 16-bit), TAIL is a full block plus
 * 17 samples (two blocks, the second partial). */
#define ITSEX_LEN_SHORT 61
#define ITSEX_LEN_TAIL_EXTRA 17

int itsex_model_build(int bits, int it215, size_t length,
                      struct itsex_stream_model *out);
int itsex_model_build_stereo(int bits, int it215, size_t channel_length,
                             struct itsex_stereo_model *out);
size_t itsex_model_build_it_module(const struct itsex_stereo_model *sm,
                                   uint8_t **out_buf);
void itsex_model_free(struct itsex_stream_model *m);
void itsex_model_free_stereo(struct itsex_stereo_model *m);


#define ITSEX_BLOCK_8 32768u
#define ITSEX_BLOCK_16 16384u

/* IT sample header flags (values from the IT format, kept local so this
 * translation unit never depends on production decoder headers). */
#define IT_F_SAMPLE 0x01u
#define IT_F_16BIT  0x02u
#define IT_F_STEREO 0x04u
#define IT_F_COMP   0x08u
#define IT_CVT_DIFF 0x04u

/* LSB-first bit writer. It grows a byte buffer and never shares any
 * logic with the production 32-bit cached read_bits() routine. */
struct bitwriter {
    uint8_t *buf;
    size_t cap;
    size_t nbits;
};

static int bw_init(struct bitwriter *bw)
{
    bw->cap = 256;
    bw->nbits = 0;
    bw->buf = (uint8_t *)calloc(bw->cap, 1);
    return bw->buf ? 0 : -1;
}

static int bw_reserve(struct bitwriter *bw, size_t bytes)
{
    if (bytes <= bw->cap)
        return 0;
    size_t cap = bw->cap;
    while (cap < bytes)
        cap <<= 1;
    uint8_t *nb = (uint8_t *)realloc(bw->buf, cap);
    if (nb == NULL)
        return -1;
    memset(nb + bw->cap, 0, cap - bw->cap);
    bw->buf = nb;
    bw->cap = cap;
    return 0;
}

static int bw_put(struct bitwriter *bw, uint32_t value, int n)
{
    size_t end = bw->nbits + (size_t)n;
    if (bw_reserve(bw, (end + 7) / 8) < 0)
        return -1;
    for (int k = 0; k < n; k++) {
        size_t bp = bw->nbits++;
        if ((value >> k) & 1u)
            bw->buf[bp >> 3] |= (uint8_t)(1u << (bp & 7u));
    }
    return 0;
}

/* A planned block: its bitstream payload plus the model outputs. */
struct block_plan {
    struct bitwriter bw;
    uint32_t *expected;
    size_t count;
    size_t cap;
    uint32_t temp;
    uint32_t temp2;
};

static int block_reserve_samples(struct block_plan *bp, size_t extra)
{
    if (bp->count + extra <= bp->cap)
        return 0;
    size_t cap = bp->cap ? bp->cap : 64;
    while (cap < bp->count + extra)
        cap <<= 1;
    uint32_t *ne = (uint32_t *)realloc(bp->expected,
                                        cap * sizeof(uint32_t));
    if (ne == NULL)
        return -1;
    bp->expected = ne;
    bp->cap = cap;
    return 0;
}

/* Candidate signed deltas, including large magnitudes so the modular
 * accumulators visibly wrap in both directions. */
static const int deltas8[] = {
    1, -1, 2, -2, 3, -3, 5, -5, 8, -8, 13, -13, 21, -21, 34, -34,
    55, -55, 89, -89, 127, -128, 200, -200, 100, -100, 7, -7, 0
};
static const int deltas16[] = {
    1, -1, 3, -3, 7, -7, 17, -17, 43, -43, 100, -100, 255, -256,
    1000, -1000, 4096, -4096, 16384, -16384, 32767, -32768,
    50000, -20000, 0
};

static int max_width(int bits)
{
    return bits + 1;
}

static int control_field_bits(int bits)
{
    return bits == 8 ? 3 : 4;
}

static uint32_t width_mask(int w)
{
    return (1u << w) - 1u;
}

/* True when a w-bit code word is a differential symbol at width w
 * rather than one of the reserved control representations. */
static int data_code_ok(int bits, int w, uint32_t code)
{
    int maxw = max_width(bits);

    if (w < 7) {
        /* Low widths: the single sentinel value 1 << (w-1) is reserved. */
        return code != (1u << (w - 1));
    }
    if (w < maxw) {
        /* Middle widths: the reserved switch interval. */
        if (bits == 8) {
            uint32_t hi = (0xffu >> (9 - w)) + 4u;
            uint32_t lo = hi - 8u;
            return code <= lo || code > hi;
        }
        uint32_t hi = (0xffffu >> (17 - w)) + 8u;
        uint32_t lo = (hi - 16u) & 0xffffu;
        return code <= lo || code > hi;
    }

    /* Initial maximum width: codes at/above the high-bit threshold are
     * high-bit width controls; everything below is a data symbol. */
    return code < (bits == 8 ? 0x100u : 0x10000u);
}

static uint32_t delta_code(int delta, int w)
{
    return (uint32_t)delta & width_mask(w);
}

/* Pick a legal data delta at width w, rotating through the candidate
 * list by step so successive symbols vary. At widths below the sample
 * bit depth the decoder sign-extends the field, so only the range
 * [-2^(w-1), 2^(w-1)-1] is representable; at the bit depth and the
 * initial maximum width the full unsigned field range is available.
 * Delta 0 always encodes to a legal code, so the search terminates. */
static int pick_delta(int bits, int w, size_t step)
{
    int cand[16];
    size_t n = 0;

    if (w < bits) {
        int lo = -(1 << (w - 1));
        int hi = (1 << (w - 1)) - 1;
        int t;

        cand[n++] = 0;
        cand[n++] = 1;
        cand[n++] = -1;
        cand[n++] = hi;
        cand[n++] = lo;
        for (t = 2; t <= 4; t++) {
            cand[n++] = t;
            cand[n++] = -t;
        }
        for (size_t k = 0; k < n; k++) {
            int d = cand[(step + k) % n];
            if (d < lo || d > hi)
                continue;
            if (data_code_ok(bits, w, delta_code(d, w)))
                return d;
        }
        return 0;
    }

    const int *list = bits == 8 ? deltas8 : deltas16;
    size_t ln = bits == 8 ? sizeof(deltas8) / sizeof(deltas8[0])
                          : sizeof(deltas16) / sizeof(deltas16[0]);
    for (size_t k = 0; k < ln; k++) {
        int d = list[(step + k) % ln];
        if (data_code_ok(bits, w, delta_code(d, w)))
            return d;
    }
    return 0;
}

/* Pick a legal delta at width w that leaves both accumulators nonzero.
 * Used for the final symbol of a block that has a successor. */
static int pick_closing_delta(int bits, int w, uint32_t temp,
                              uint32_t temp2)
{
    uint32_t mod = bits == 8 ? 0x100u : 0x10000u;

    if (w < bits) {
        int lo = -(1 << (w - 1));
        int hi = (1 << (w - 1)) - 1;
        for (int d = lo; d <= hi; d++) {
            if (!data_code_ok(bits, w, delta_code(d, w)))
                continue;
            uint32_t nt = (temp + ((uint32_t)d & (mod - 1u)))
                          & (mod - 1u);
            uint32_t nt2 = (temp2 + nt) & (mod - 1u);
            if (nt != 0 && nt2 != 0)
                return d;
        }
        return 0;
    }

    const int *list = bits == 8 ? deltas8 : deltas16;
    size_t n = bits == 8 ? sizeof(deltas8) / sizeof(deltas8[0])
                         : sizeof(deltas16) / sizeof(deltas16[0]);
    for (size_t k = 0; k < n; k++) {
        int d = list[k];
        if (!data_code_ok(bits, w, delta_code(d, w)))
            continue;
        uint32_t nt = (temp + ((uint32_t)d & (mod - 1u))) & (mod - 1u);
        uint32_t nt2 = (temp2 + nt) & (mod - 1u);
        if (nt != 0 && nt2 != 0)
            return d;
    }
    return 0;
}

/* Emit one data symbol whose raw w-bit code is exactly `code`, by
 * deriving the signed delta the decoder would recover from it. This
 * pins down the edges of the reserved control ranges. */
static int emit_data(struct block_plan *bp, int bits, int it215, int w,
                     int delta, struct itsex_model_stats *stats);

static int emit_code_as_data(struct block_plan *bp, int bits, int it215,
                             int w, uint32_t code,
                             struct itsex_model_stats *stats)
{
    int delta;

    if (!data_code_ok(bits, w, code))
        return -1;
    if (w < bits) {
        /* Decoder sign-extends the field before accumulating. */
        uint32_t sign = 1u << (w - 1);
        delta = (int)(code ^ sign) - (int)sign;
    } else {
        /* Full-width fields are used as unsigned modular deltas. */
        delta = (int)code;
    }
    if (delta_code(delta, w) != (code & width_mask(w)))
        return -1;
    return emit_data(bp, bits, it215, w, delta, stats);
}

/* Emit data symbols using the code values immediately adjacent to each
 * reserved control representation (sentinel +/- 1, both ends of the
 * middle reserved interval, and threshold - 1 at the maximum width),
 * proving those neighbors are still differential symbols. */
static int emit_boundary_data(struct block_plan *bp, int bits, int it215,
                              int w, size_t quota,
                              struct itsex_model_stats *stats)
{
    int maxw = max_width(bits);

    if (w < 7) {
        uint32_t sentinel = 1u << (w - 1);
        if (bp->count < quota &&
            emit_code_as_data(bp, bits, it215, w, sentinel - 1u, stats) < 0)
            return -1;
        if (sentinel + 1u < (1u << w) && bp->count < quota &&
            emit_code_as_data(bp, bits, it215, w, sentinel + 1u, stats) < 0)
            return -1;
    } else if (w < maxw) {
        if (bits == 8) {
            uint32_t hi = (0xffu >> (9 - w)) + 4u;
            uint32_t lo = hi - 8u;
            if (bp->count < quota &&
                emit_code_as_data(bp, bits, it215, w, lo, stats) < 0)
                return -1;
            if (bp->count < quota &&
                emit_code_as_data(bp, bits, it215, w, hi + 1u, stats) < 0)
                return -1;
        } else {
            uint32_t hi = (0xffffu >> (17 - w)) + 8u;
            uint32_t lo = (hi - 16u) & 0xffffu;
            if (bp->count < quota &&
                emit_code_as_data(bp, bits, it215, w, lo, stats) < 0)
                return -1;
            if (bp->count < quota &&
                emit_code_as_data(bp, bits, it215, w,
                                  (hi + 1u) & 0xffffu, stats) < 0)
                return -1;
        }
    } else {
        uint32_t threshold = bits == 8 ? 0x100u : 0x10000u;
        if (bp->count < quota &&
            emit_code_as_data(bp, bits, it215, w, threshold - 1u, stats) < 0)
            return -1;
    }
    return 0;
}

/* Emit one differential data symbol and advance both accumulators. */
static int emit_data(struct block_plan *bp, int bits, int it215, int w,
                     int delta, struct itsex_model_stats *stats)
{
    uint32_t mod = bits == 8 ? 0x100u : 0x10000u;
    uint32_t dmod = (uint32_t)delta & (mod - 1u);

    if (bw_put(&bp->bw, delta_code(delta, w), w) < 0)
        return -1;
    if (block_reserve_samples(bp, 1) < 0)
        return -1;

    bp->temp = (bp->temp + dmod) & (mod - 1u);
    bp->temp2 = (bp->temp2 + bp->temp) & (mod - 1u);
    bp->expected[bp->count++] = it215 ? bp->temp2 : bp->temp;
    stats->width_data[w]++;
    return 0;
}

/* Emit a width switch control using one of the three representations:
 * class 0 = low sentinel + following width field (w < maxw),
 * class 1 = middle reserved interval (7 <= w < maxw),
 * class 2 = high-bit control at the initial maximum width.
 * The encodings follow the decoder's width update rule:
 *   v = field + 1; new width = v < w ? v : v + 1. */
static int emit_switch(struct block_plan *bp, int bits, int w, int target,
                       struct itsex_model_stats *stats)
{
    int maxw = max_width(bits);
    int cf = control_field_bits(bits);
    size_t start = bp->bw.nbits;
    size_t field_len;
    int klass;

    if (w == maxw) {
        /* High-bit control: raw = threshold + (target - 1), maxw bits. */
        uint32_t threshold = bits == 8 ? 0x100u : 0x10000u;
        if (bw_put(&bp->bw, threshold + (uint32_t)(target - 1), maxw) < 0)
            return -1;
        klass = 2;
        field_len = (size_t)maxw;
    } else if (w >= 7) {
        /* Middle reserved interval: code = j + v where v = target or
         * target - 1 depending on whether v is below the current width. */
        uint32_t value, v;
        if (bits == 8) {
            uint32_t hi = (0xffu >> (9 - w)) + 4u;
            uint32_t lo = hi - 8u;
            v = (uint32_t)(target < w ? target : target - 1);
            value = lo + v;
        } else {
            uint32_t hi = (0xffffu >> (17 - w)) + 8u;
            uint32_t lo = (hi - 16u) & 0xffffu;
            v = (uint32_t)(target < w ? target : target - 1);
            value = lo + v;
        }
        if (bw_put(&bp->bw, value, w) < 0)
            return -1;
        klass = 1;
        field_len = (size_t)w;
    } else {
        /* Low sentinel followed by the cf-bit width field; the field
         * stores v - 1, with v = target or target - 1. */
        uint32_t sentinel = 1u << (w - 1);
        uint32_t v = (uint32_t)(target < w ? target : target - 1);
        if (bw_put(&bp->bw, sentinel, w) < 0)
            return -1;
        if (bw_put(&bp->bw, v - 1u, cf) < 0)
            return -1;
        klass = 0;
        field_len = (size_t)w + (size_t)cf;
    }

    stats->switches[klass]++;
    if (start / 8 != (start + field_len - 1) / 8)
        stats->control_byte_crossings++;
    if (start / 32 != (start + field_len - 1) / 32)
        stats->control_cache_crossings++;
    return 0;
}

/* Pad with legal data symbols at width w until a field of `field` bits
 * would be fully contained in the current 32-bit cache word, so the
 * following control symbol cannot straddle a cache refill. */
static int pad_control_safe(struct block_plan *bp, int bits, int it215,
                            int w, int field, size_t quota,
                            struct itsex_model_stats *stats)
{
    for (int guard = 0; guard < 64; guard++) {
        size_t r = bp->bw.nbits & 31u;
        if (r + (size_t)field <= 32u)
            return 0;
        if (bp->count >= quota)
            return -1;
        if (emit_data(bp, bits, it215, w,
                      pick_delta(bits, w, bp->count), stats) < 0)
            return -1;
    }
    return -1;
}

/* Pad with legal data symbols at width w until the next w-bit read
 * straddles a 32-bit cache refill boundary with nonzero low-bit offset,
 * then emit the straddling data symbol itself. */
static int emit_data_across_cache(struct block_plan *bp, int bits,
                                  int it215, int w, size_t quota,
                                  struct itsex_model_stats *stats)
{
    for (int guard = 0; guard < 64; guard++) {
        size_t r = bp->bw.nbits & 31u;
        if (r != 0 && r + (size_t)w > 32u)
            return emit_data(bp, bits, it215, w,
                             pick_delta(bits, w, bp->count), stats);
        if (bp->count >= quota)
            return -1;
        if (emit_data(bp, bits, it215, w,
                      pick_delta(bits, w, bp->count), stats) < 0)
            return -1;
    }
    return -1;
}

/* Pad until the next field straddles a byte boundary (cache-safe
 * placement is the caller's responsibility). */
static int pad_until_byte_cross(struct block_plan *bp, int bits,
                                int it215, int w, int field, size_t quota,
                                struct itsex_model_stats *stats)
{
    for (int guard = 0; guard < 64; guard++) {
        size_t p = bp->bw.nbits;
        if (p % 8 != 0 && p / 8 != (p + (size_t)field - 1) / 8)
            return 0;
        if (bp->count >= quota)
            return -1;
        if (emit_data(bp, bits, it215, w,
                      pick_delta(bits, w, bp->count), stats) < 0)
            return -1;
    }
    return -1;
}

/*
 * Plan one independent block. Every block:
 *   - starts at the initial maximum width with both accumulators zero;
 *   - uses a high-bit control (class 2) down to width 1;
 *   - produces legal positive, negative and wrapping data symbols at
 *     every width 1..maxw.
 * Blocks with enough slack additionally return to the maximum width
 * with a low sentinel (class 0) and read a real data symbol across a
 * 32-bit cache refill, with width-walk controls placed so they never
 * straddle a refill. Full blocks additionally force one control field
 * across a 32-bit boundary and one across a byte boundary. The tight
 * 17-sample tail block has no padding slack and uses natural placement.
 */
static int plan_block(int bits, int it215, size_t quota, int full,
                      int need_nonzero_end, struct block_plan *bp,
                      struct itsex_model_stats *stats)
{
    const int maxw = max_width(bits);
    const int cf = control_field_bits(bits);
    int w = maxw;

    memset(bp, 0, sizeof(*bp));
    if (bw_init(&bp->bw) < 0)
        return -1;

    /* Opening high-bit control: maxw -> 1. */
    if (emit_switch(bp, bits, w, 1, stats) < 0)
        goto fail;
    w = 1;

    if (!full) {
        int safe = quota >= (size_t)(2 * maxw + 8);

        if (safe) {
            /* Cache-safe compact plan. Every control is placed so it is
             * fully inside one 32-bit cache word; therefore the first
             * read that straddles a refill is always a data symbol, so
             * a decoder that drops unconsumed low bits changes PCM
             * values without ever losing stream synchronization. */
            if (pad_control_safe(bp, bits, it215, w, w + cf, quota,
                                 stats) < 0)
                goto fail;
            if (emit_switch(bp, bits, w, maxw, stats) < 0)
                goto fail;
            w = maxw;
            if (emit_data_across_cache(bp, bits, it215, w, quota,
                                       stats) < 0)
                goto fail;
            if (pad_control_safe(bp, bits, it215, w, w, quota,
                                 stats) < 0)
                goto fail;
            if (emit_switch(bp, bits, w, 1, stats) < 0)
                goto fail;
            w = 1;
        }

        /* One data symbol at every legal width. In the safe variant
         * controls never straddle a cache refill; the tight variant
         * relies on natural placement (used only by the 17-sample tail
         * block, whose quota leaves no padding slack). */
        for (int t = 1; t <= maxw; t++) {
            if (t != w) {
                int field = (w == maxw) ? maxw :
                            (w < 7 ? w + cf : w);
                if (safe && pad_control_safe(bp, bits, it215, w, field,
                                             quota, stats) < 0)
                    goto fail;
                if (emit_switch(bp, bits, w, t, stats) < 0)
                    goto fail;
                w = t;
            }
            if (bp->count >= quota)
                goto fail;
            if (emit_data(bp, bits, it215, t,
                          pick_delta(bits, t, bp->count), stats) < 0)
                goto fail;
        }
        if (w != bits) {
            int field = (w < 7) ? w + cf : w;
            if (safe && pad_control_safe(bp, bits, it215, w, field,
                                         quota, stats) < 0)
                goto fail;
            if (emit_switch(bp, bits, w, bits, stats) < 0)
                goto fail;
            w = bits;
        }
        while (bp->count + 1 < quota) {
            if (emit_data(bp, bits, it215, w,
                          pick_delta(bits, w, bp->count), stats) < 0)
                goto fail;
        }
        if (bp->count < quota) {
            int delta = need_nonzero_end ?
                pick_closing_delta(bits, w, bp->temp, bp->temp2) :
                pick_delta(bits, w, bp->count);
            if (emit_data(bp, bits, it215, w, delta, stats) < 0)
                goto fail;
        }
        if (need_nonzero_end && (bp->temp == 0 || bp->temp2 == 0))
            goto fail;
        if (bp->count != quota)
            goto fail;
        return 0;
    }

    /* Low sentinel straight back to the maximum width. */
    if (pad_control_safe(bp, bits, it215, w, w + cf, quota, stats) < 0)
        goto fail;
    if (emit_switch(bp, bits, w, maxw, stats) < 0)
        goto fail;
    w = maxw;

    /* Read a real data symbol across a 32-bit cache refill. */
    if (emit_data_across_cache(bp, bits, it215, w, quota, stats) < 0)
        goto fail;

    /* Data symbols at every legal width, with the appropriate switch
     * representation connecting each width to the next. */
    for (int t = 1; t <= maxw; t++) {
        int perslot = full ? 2 : 1;

        if (t != w) {
            int field = (w == maxw) ? maxw :
                        (w < 7 ? w + cf : w);
            if (pad_control_safe(bp, bits, it215, w, field, quota,
                                 stats) < 0)
                goto fail;
            if (emit_switch(bp, bits, w, t, stats) < 0)
                goto fail;
            w = t;
        }

        for (int k = 0; k < perslot; k++) {
            if (bp->count >= quota)
                goto fail;
            if (emit_data(bp, bits, it215, t,
                          pick_delta(bits, t, bp->count + (size_t)k),
                          stats) < 0)
                goto fail;
        }
        if (full && emit_boundary_data(bp, bits, it215, t, quota,
                                       stats) < 0)
            goto fail;
    }

    if (full) {
        /* Force a control field across a 32-bit boundary: align the
         * stream so a maxw-bit high-bit control starts at a nonzero
         * offset inside a cache word and runs past its end. Since
         * gcd(maxw, 32) == 1 the target offset range is reachable. */
        if (pad_control_safe(bp, bits, it215, w, w, quota, stats) < 0)
            goto fail;
        for (int guard = 0; guard < 40; guard++) {
            size_t r = bp->bw.nbits & 31u;
            if (r != 0 && r + (size_t)w > 32u)
                break;
            if (bp->count >= quota)
                goto fail;
            if (emit_data(bp, bits, it215, w,
                          pick_delta(bits, w, bp->count), stats) < 0)
                goto fail;
        }
        {
            size_t r = bp->bw.nbits & 31u;
            if (r == 0 || r + (size_t)w <= 32u)
                goto fail;
        }
        if (emit_switch(bp, bits, w, 1, stats) < 0)
            goto fail;
        w = 1;

        /* Force a control field across a byte boundary. */
        if (pad_control_safe(bp, bits, it215, w, w + cf, quota, stats) < 0)
            goto fail;
        if (pad_until_byte_cross(bp, bits, it215, w, w + cf, quota,
                                 stats) < 0)
            goto fail;
        if (emit_switch(bp, bits, w, bits, stats) < 0)
            goto fail;
        w = bits;
    }

    /* Fill to the exact quota at a width that admits both signs. */
    if (w != bits) {
        int field = (w < 7) ? w + cf : w;
        if (pad_control_safe(bp, bits, it215, w, field, quota, stats) < 0)
            goto fail;
        if (emit_switch(bp, bits, w, bits, stats) < 0)
            goto fail;
        w = bits;
    }
    while (bp->count + 1 < quota) {
        if (emit_data(bp, bits, it215, w,
                      pick_delta(bits, w, bp->count), stats) < 0)
            goto fail;
    }
    if (bp->count < quota) {
        int delta;
        if (need_nonzero_end)
            delta = pick_closing_delta(bits, w, bp->temp, bp->temp2);
        else
            delta = pick_delta(bits, w, bp->count);
        if (emit_data(bp, bits, it215, w, delta, stats) < 0)
            goto fail;
    }

    if (need_nonzero_end && (bp->temp == 0 || bp->temp2 == 0))
        goto fail;
    if (bp->count != quota)
        goto fail;
    return 0;

fail:
    free(bp->bw.buf);
    free(bp->expected);
    memset(bp, 0, sizeof(*bp));
    return -1;
}

static size_t block_limit(int bits)
{
    return bits == 8 ? ITSEX_BLOCK_8 : ITSEX_BLOCK_16;
}

int itsex_model_build(int bits, int it215, size_t length,
                      struct itsex_stream_model *out)
{
    if ((bits != 8 && bits != 16) || (it215 & ~1) || length == 0 ||
        out == NULL)
        return -1;

    memset(out, 0, sizeof(*out));
    out->bits = bits;
    out->it215 = it215;
    out->length = length;
    out->modulus = bits == 8 ? 256u : 65536u;

    size_t blimit = block_limit(bits);
    size_t nblocks = (length + blimit - 1) / blimit;
    if (nblocks > 256)
        return -1;

    out->expected = (uint32_t *)malloc(length * sizeof(uint32_t));
    if (out->expected == NULL)
        return -1;

    /* Upper bound on packed size: every sample at maxw bits, plus block
     * headers and generous room for control symbols. */
    size_t cap = 2 + nblocks * (2 + length * (size_t)(bits + 1) / 8 + 64);
    out->packed = (uint8_t *)malloc(cap);
    if (out->packed == NULL) {
        free(out->expected);
        out->expected = NULL;
        return -1;
    }
    out->packed_len = 0;

    size_t done = 0;
    for (size_t b = 0; b < nblocks; b++) {
        size_t quota = length - done;
        if (quota > blimit)
            quota = blimit;
        /* Only exactly complete blocks carry the forced boundary
         * phases; compact blocks keep the width walk and the opening
         * high-bit/low-sentinel control pair. */
        int full = quota == blimit;
        struct block_plan bp;

        if (plan_block(bits, it215, quota, full, b + 1 < nblocks,
                       &bp, &out->stats) < 0)
            goto fail;

        size_t payload = (bp.bw.nbits + 7) / 8;
        if (payload > 0xffffu ||
            out->packed_len + 2 + payload > cap) {
            free(bp.bw.buf);
            free(bp.expected);
            goto fail;
        }

        /* Block header: uint16 LE payload length, then payload bytes.
         * Unused tail bits are zero because the buffer was calloc'd. */
        out->packed[out->packed_len++] = (uint8_t)(payload & 0xffu);
        out->packed[out->packed_len++] = (uint8_t)((payload >> 8) & 0xffu);
        memcpy(out->packed + out->packed_len, bp.bw.buf, payload);
        out->packed_len += payload;

        memcpy(out->expected + done, bp.expected, quota * sizeof(uint32_t));
        if (b == 0) {
            out->block1_end = quota;
            out->block1_end_temp = bp.temp;
            out->block1_end_temp2 = bp.temp2;
        }
        if (b + 1 == nblocks) {
            out->block_end_temp = bp.temp;
            out->block_end_temp2 = bp.temp2;
        }
        done += quota;
        free(bp.bw.buf);
        free(bp.expected);
    }

    out->nblocks = (int)nblocks;
    return 0;

fail:
    free(out->packed);
    free(out->expected);
    memset(out, 0, sizeof(*out));
    return -1;
}

int itsex_model_build_stereo(int bits, int it215, size_t channel_length,
                             struct itsex_stereo_model *out)
{
    if (out == NULL)
        return -1;
    memset(out, 0, sizeof(*out));
    out->bits = bits;
    out->it215 = it215;
    out->channel_length = channel_length;

    if (itsex_model_build(bits, it215, channel_length, &out->left) < 0)
        return -1;
    if (itsex_model_build(bits, it215, channel_length, &out->right) < 0) {
        itsex_model_free(&out->left);
        return -1;
    }

    out->packed_len = out->left.packed_len + out->right.packed_len;
    out->packed = (uint8_t *)malloc(out->packed_len);
    if (out->packed == NULL) {
        itsex_model_free(&out->left);
        itsex_model_free(&out->right);
        return -1;
    }
    memcpy(out->packed, out->left.packed, out->left.packed_len);
    memcpy(out->packed + out->left.packed_len, out->right.packed,
           out->right.packed_len);
    return 0;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

size_t itsex_model_build_it_module(const struct itsex_stereo_model *sm,
                                   uint8_t **out_buf)
{
    enum {
        FILE_HDR = 192,                  /* 0x40 + 64 pan + 64 vol */
        SMP_PTR_TABLE = 4,
        SMP_HDR = 80,
        SMP_DATA_OFS = FILE_HDR + SMP_PTR_TABLE + SMP_HDR
    };

    if (sm == NULL || out_buf == NULL)
        return 0;

    size_t size = SMP_DATA_OFS + sm->packed_len;
    uint8_t *buf = (uint8_t *)calloc(size, 1);
    if (buf == NULL)
        return 0;

    /* IT file header. Sample mode (no instruments), zero patterns. */
    memcpy(buf, "IMPM", 4);
    memcpy(buf + 4, "itsex model", 11);
    put16(buf + 32, 0);                                  /* ordnum */
    put16(buf + 34, 0);                                  /* insnum */
    put16(buf + 36, 1);                                  /* smpnum */
    put16(buf + 38, 0);                                  /* patnum */
    put16(buf + 40, 0x0214);                             /* cwt */
    put16(buf + 42, 0x0214);                             /* cmwt */
    put16(buf + 44, 0);                                  /* flags */
    put16(buf + 46, 0);                                  /* special */
    buf[48] = 64;                                        /* gv */
    buf[49] = 48;                                        /* mv */
    buf[50] = 6;                                         /* is */
    buf[51] = 120;                                       /* it */
    buf[52] = 128;                                       /* sep */
    buf[53] = 0;                                         /* pwd */
    put16(buf + 54, 0);                                  /* msglen */
    put32(buf + 56, 0);                                  /* msgofs */
    put32(buf + 60, 0);                                  /* rsvd */
    /* chpan/chvol stay zero. */

    /* Sample pointer table: one entry, file-relative. */
    put32(buf + FILE_HDR, FILE_HDR + SMP_PTR_TABLE);

    /* IMPS sample header. */
    uint8_t *sh = buf + FILE_HDR + SMP_PTR_TABLE;
    memcpy(sh, "IMPS", 4);
    sh[16] = 0;                                          /* zero */
    sh[17] = 64;                                         /* gvl */
    uint8_t sflags = (uint8_t)(IT_F_SAMPLE | IT_F_STEREO | IT_F_COMP);
    if (sm->bits == 16)
        sflags |= IT_F_16BIT;
    sh[18] = sflags;
    sh[19] = 64;                                         /* vol */
    sh[46] = sm->it215 ? (uint8_t)IT_CVT_DIFF : 0;      /* convert */
    sh[47] = 0x80;                                       /* dfp: no override */
    put32(sh + 48, (uint32_t)sm->channel_length);
    put32(sh + 52, 0);                                   /* loopbeg */
    put32(sh + 56, 0);                                   /* loopend */
    put32(sh + 60, 8363);                                /* c5spd */
    put32(sh + 64, 0);                                   /* sloopbeg */
    put32(sh + 68, 0);                                   /* sloopend */
    put32(sh + 72, SMP_DATA_OFS);                        /* sample_ptr */

    /* Two independently headed compressed channel streams, in order. */
    memcpy(buf + SMP_DATA_OFS, sm->packed, sm->packed_len);

    *out_buf = buf;
    return size;
}

void itsex_model_free(struct itsex_stream_model *m)
{
    if (m == NULL)
        return;
    free(m->packed);
    free(m->expected);
    memset(m, 0, sizeof(*m));
}

void itsex_model_free_stereo(struct itsex_stereo_model *m)
{
    if (m == NULL)
        return;
    itsex_model_free(&m->left);
    itsex_model_free(&m->right);
    free(m->packed);
    memset(m, 0, sizeof(*m));
}
