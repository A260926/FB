/*
 * check_predication.c
 *
 * C99 tests for glsl120 predicated SM3 instructions. All shaders are built as
 *  legal D3D9 token words in-process (no external assembler). Each numeric
 *  expectation is produced by the independent CPU register/texture model in
 *  predication_model.h, never by reading back the generated GLSL text. The
 *  GLSL itself is checked structurally (per-channel guards, one texture
 *  fetch, GLSL 1.20-only constructs, stable names).
 */

#include "mojoshader.h"
#include "predication_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_cases = 0;
static int g_assertions = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_assertions++;                                                        \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("  FAIL: " __VA_ARGS__);                                    \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static void count_case(void)
{
    g_cases++;
}

/* ----- small string helpers ------------------------------------------------ */

static int str_occurrences(const char *haystack, const char *needle)
{
    int count = 0;
    const char *p = haystack;
    size_t len = strlen(needle);
    if (p == NULL)
        return 0;
    while ((p = strstr(p, needle)) != NULL)
    {
        count++;
        p += len;
    } // while
    return count;
}

static int str_contains(const char *haystack, const char *needle)
{
    return (haystack != NULL) && (strstr(haystack, needle) != NULL);
}

/* ----- parser wrapper ------------------------------------------------------ */

static const MOJOSHADER_parseData *parse_words(const uint32_t *words,
                                               size_t count)
{
    return MOJOSHADER_parse("glsl120", "main", (const unsigned char *) words,
                            (unsigned)(count * sizeof (uint32_t)),
                            NULL, 0, NULL, 0, NULL, NULL, NULL);
}

/* ----- CPU model: one predicated arithmetic/move op ------------------------ */

typedef struct
{
    int regtype;
    int regnum;
    int swizzle;
    int mod;
} model_src;

typedef struct
{
    int op;
    int dst;                 /* temp register number */
    int mask;                /* static write mask */
    int sat;
    model_src s[3];
    int predicated;
    int pred_mod;            /* MS_MOD_NONE or MS_MOD_NOT */
    int pred_swiz;           /* 0xE4 or a replicate swizzle */
} math_case;

/* Reads every source first, so destination/source aliasing cannot let an
 *  early write influence a later channel. */
static void run_math_model(ms_state *st, const math_case *tc)
{
    float candidate[4];
    int enabled[4];

    /* DP3/DP4 collapse several source lanes into one scalar, broadcast into
     *  every enabled channel. Compute it once, before any write, matching the
     *  single GLSL dot() expression. */
    float dotvalue = 0.0f;
    if (tc->op == MS_OP_DP3 || tc->op == MS_OP_DP4)
    {
        const int lanes = (tc->op == MS_OP_DP3) ? 3 : 4;
        for (int k = 0; k < lanes; k++)
        {
            const float a = ms_read_src(st, tc->s[0].regtype, tc->s[0].regnum,
                                        tc->s[0].swizzle, tc->s[0].mod, k);
            const float b = ms_read_src(st, tc->s[1].regtype, tc->s[1].regnum,
                                        tc->s[1].swizzle, tc->s[1].mod, k);
            dotvalue += a * b;
        } // for
    } // if

    int c;
    for (c = 0; c < 4; c++)
    {
        enabled[c] = (tc->mask & (1 << c)) != 0;
        if (!enabled[c])
            continue;
        if (tc->op == MS_OP_DP3 || tc->op == MS_OP_DP4)
        {
            candidate[c] = dotvalue;
        } // if
        else
        {
            const float s0 = ms_read_src(st, tc->s[0].regtype, tc->s[0].regnum,
                                         tc->s[0].swizzle, tc->s[0].mod, c);
            const float s1 = ms_read_src(st, tc->s[1].regtype, tc->s[1].regnum,
                                         tc->s[1].swizzle, tc->s[1].mod, c);
            const float s2 = ms_read_src(st, tc->s[2].regtype, tc->s[2].regnum,
                                         tc->s[2].swizzle, tc->s[2].mod, c);
            candidate[c] = ms_apply_op(tc->op, c, s0, s1, s2);
        } // else
        if (tc->sat)
            candidate[c] = ms_sat(candidate[c]);
    } // for

    for (c = 0; c < 4; c++)
    {
        if (!enabled[c])
            continue;
        if (tc->predicated &&
            !ms_read_pred(st, tc->pred_swiz, tc->pred_mod, c))
            continue;  /* predicate closed: keep the old value. */
        float *dst = &st->r[tc->dst].x;
        dst[c] = candidate[c];
    } // for
}

/* Emit the matching token stream for run_math_model(). */
static size_t emit_math_words(const math_case *tc, uint32_t *out)
{
    size_t n = 0;
    const int src_count =
        (tc->op == MS_OP_MOV) ? 1 : (tc->op == MS_OP_MAD) ? 3 :
        (tc->op == MS_OP_CMP) ? 3 : 2;
    const int following = 1 + src_count + (tc->predicated ? 1 : 0);
    out[n++] = ms_inst(tc->op, following, tc->predicated, 0);
    out[n++] = ms_dst(MS_REG_TEMP, tc->dst, tc->mask, tc->sat);
    for (int i = 0; i < src_count; i++)
        out[n++] = ms_src(tc->s[i].regtype, tc->s[i].regnum,
                          tc->s[i].swizzle, tc->s[i].mod);
    if (tc->predicated)
        out[n++] = ms_src(MS_REG_PRED, 0, tc->pred_swiz, tc->pred_mod);
    return n;
}

static float vec_get(const ms_state *st, int reg, int c)
{
    const float *v = &st->r[reg].x;
    return v[c];
}

static int feq(float a, float b)
{
    return fabsf(a - b) <= 1e-5f * (1.0f + fabsf(b));
}

/* ----- full shader scaffolding --------------------------------------------- */

static size_t emit_vs_header(uint32_t *w)
{
    size_t n = 0;
    w[n++] = 0xfffe0300;                                  /* vs_3_0 */
    w[n++] = ms_inst(MS_OP_DCL, 2, 0, 0);
    w[n++] = MS_DCL_INPUT;
    w[n++] = ms_dst(MS_REG_INPUT, 0, 0xf, 0);             /* dcl v0 */
    w[n++] = ms_inst(MS_OP_DCL, 2, 0, 0);
    w[n++] = MS_DCL_INPUT;
    w[n++] = ms_dst(MS_REG_INPUT, 1, 0xf, 0);             /* dcl v1 */
    return n;
}

static size_t emit_ps_header(uint32_t *w, int sampler_dcl)
{
    size_t n = 0;
    w[n++] = 0xffff0300;                                  /* ps_3_0 */
    w[n++] = ms_inst(MS_OP_DCL, 2, 0, 0);
    w[n++] = MS_DCL_INPUT;
    w[n++] = ms_dst(MS_REG_INPUT, 0, 0xf, 0);             /* dcl v0 */
    w[n++] = ms_inst(MS_OP_DCL, 2, 0, 0);
    w[n++] = (uint32_t) sampler_dcl;
    w[n++] = ms_dst(MS_REG_SAMPLER, 0, 0xf, 0);           /* dcl s0 */
    return n;
}

/* ===== Test cases ========================================================= */

static void test_mixed_predicate_and_mask(void)
{
    count_case();
    /* p0 = (T, F, T, F); mask .xyzw; MOV r0, v0, gated by p0. */
    ms_state model;
    memset(&model, 0, sizeof (model));
    model.r[1] = ms_v4(10.0f, 20.0f, 30.0f, 40.0f);      /* v0 input slot */
    model.r[0] = ms_v4(1.0f, 2.0f, 3.0f, 4.0f);          /* prior r0 */
    model.p = (ms_bvec4) { 1, 0, 1, 0 };

    math_case tc = {
        MS_OP_MOV, 0, 0xf, 0,
        { { MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE } },
        1, MS_MOD_NONE, 0xE4
    };
    run_math_model(&model, &tc);
    CHECK(feq(vec_get(&model, 0, 0), 10.0f), "mixed: r0.x written");
    CHECK(feq(vec_get(&model, 0, 1), 2.0f),  "mixed: r0.y preserved");
    CHECK(feq(vec_get(&model, 0, 2), 30.0f), "mixed: r0.z written");
    CHECK(feq(vec_get(&model, 0, 3), 4.0f),  "mixed: r0.w preserved");

    uint32_t words[64];
    size_t n = emit_vs_header(words);
    /* establish p0 with setp_ge against a constant sign pattern. */
    words[n++] = ms_inst(MS_OP_SETP, 3, 0, 3);
    words[n++] = ms_dst(MS_REG_PRED, 0, 0xf, 0);
    words[n++] = ms_src(MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE);
    words[n++] = ms_src(MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE);
    n += emit_math_words(&tc, words + n);
    words[n++] = MS_OP_END;

    const MOJOSHADER_parseData *pd = parse_words(words, n);
    CHECK(pd->error_count == 0, "mixed: parses cleanly");
    CHECK(pd->output && str_contains(pd->output, "if (vs_p0.x)"),
          "mixed: x guard present");
    CHECK(pd->output && str_contains(pd->output, "if (vs_p0.z)"),
          "mixed: z guard present");
    CHECK(pd->output && !str_contains(pd->output, "any("),
          "mixed: no any() reduction");
    CHECK(pd->output && !str_contains(pd->output, "all("),
          "mixed: no all() reduction");
    MOJOSHADER_freeParseData(pd);
}

static void test_negated_replicate_predicate(void)
{
    count_case();
    /* (!p0.x) applied to every channel of a sparse .xz write. */
    ms_state model;
    memset(&model, 0, sizeof (model));
    model.r[1] = ms_v4(5.0f, 6.0f, 7.0f, 8.0f);
    model.r[0] = ms_v4(1.0f, 2.0f, 3.0f, 4.0f);
    model.p = (ms_bvec4) { 1, 0, 1, 0 };

    math_case tc = {
        MS_OP_ADD, 0, 0x5 /* .xz */, 0,
        { { MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE },
          { MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE } },
        1, MS_MOD_NOT, ms_replicate(0)
    };
    run_math_model(&model, &tc);
    /* p0.x is true, so !p0.x is false: no channel may change. */
    CHECK(feq(vec_get(&model, 0, 0), 1.0f), "replicate-neg: x preserved");
    CHECK(feq(vec_get(&model, 0, 1), 2.0f), "replicate-neg: y untouched");
    CHECK(feq(vec_get(&model, 0, 2), 3.0f), "replicate-neg: z preserved");
    CHECK(feq(vec_get(&model, 0, 3), 4.0f), "replicate-neg: w untouched");

    model.p = (ms_bvec4) { 0, 1, 0, 1 };
    run_math_model(&model, &tc);
    /* !p0.x now true for every lane: only masked .xz channels update. */
    CHECK(feq(vec_get(&model, 0, 0), 10.0f), "replicate-neg: x written");
    CHECK(feq(vec_get(&model, 0, 1), 2.0f),  "replicate-neg: y still masked");
    CHECK(feq(vec_get(&model, 0, 2), 14.0f), "replicate-neg: z written");
    CHECK(feq(vec_get(&model, 0, 3), 4.0f),  "replicate-neg: w still masked");

    uint32_t words[64];
    size_t n = emit_vs_header(words);
    n += emit_math_words(&tc, words + n);
    words[n++] = MS_OP_END;

    const MOJOSHADER_parseData *pd = parse_words(words, n);
    CHECK(pd->error_count == 0, "replicate-neg: parses cleanly");
    CHECK(pd->output && str_occurrences(pd->output, "if (!vs_p0.x)") == 2,
          "replicate-neg: both enabled lanes reuse !p0.x");
    CHECK(pd->output && !str_contains(pd->output, ".y ="),
          "replicate-neg: sparse mask omits y");
    CHECK(pd->output && !str_contains(pd->output, ".w ="),
          "replicate-neg: sparse mask omits w");
    MOJOSHADER_freeParseData(pd);
}

static void test_per_component_negate_no_swizzle(void)
{
    count_case();
    /* (!p0) with no swizzle negates each lane independently; the four guards
     *  must reference four different predicate components, never any/all. */
    ms_state model;
    memset(&model, 0, sizeof (model));
    model.r[1] = ms_v4(1.0f, 2.0f, 3.0f, 4.0f);
    model.r[0] = ms_v4(0.0f, 0.0f, 0.0f, 0.0f);
    model.p = (ms_bvec4) { 1, 0, 1, 0 };

    math_case tc = {
        MS_OP_MOV, 0, 0xf, 0,
        { { MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE } },
        1, MS_MOD_NOT, 0xE4
    };
    run_math_model(&model, &tc);
    /* Negated: open where p is false, i.e. lanes y and w. */
    CHECK(feq(model.r[0].x, 0.0f), "not: x stays (p0.x true -> gate false)");
    CHECK(feq(model.r[0].y, 2.0f),  "not: y written (p0.y false -> gate true)");
    CHECK(feq(model.r[0].z, 0.0f),  "not: z stays");
    CHECK(feq(model.r[0].w, 4.0f),  "not: w written");

    uint32_t words[32];
    size_t n = emit_vs_header(words);
    n += emit_math_words(&tc, words + n);
    words[n++] = MS_OP_END;

    const MOJOSHADER_parseData *pd = parse_words(words, n);
    CHECK(pd->error_count == 0, "not: parses cleanly");
    for (int c = 0; c < 4; c++)
    {
        char needle[32];
        snprintf(needle, sizeof (needle), "if (!vs_p0.%c)", "xyzw"[c]);
        CHECK(str_contains(pd->output, needle),
              "not: distinct negated guard for lane %c", "xyzw"[c]);
    }
    CHECK(!str_contains(pd->output, "any(") &&
          !str_contains(pd->output, "all("),
          "not: no vector reduction");
    MOJOSHADER_freeParseData(pd);
}

static void test_setp_partial_update(void)
{
    count_case();
    ms_state model;
    memset(&model, 0, sizeof (model));
    model.r[1] = ms_v4(1.0f, -1.0f, 0.0f, 2.0f);
    model.p = (ms_bvec4) { 7, 8, 9, 10 };  /* prior boolean components. */

    /* setp_ge p0.xz, v0, c0 with c0 all zero: only x,z are recomputed. */
    const int mask = 0x5;
    int expected[4] = { model.p.x, model.p.y, model.p.z, model.p.w };
    for (int c = 0; c < 4; c++)
        if (mask & (1 << c))
            expected[c] = (vec_get(&model, 1, c) >= 0.0f);

    uint32_t words[32];
    size_t n = emit_vs_header(words);
    words[n++] = ms_inst(MS_OP_SETP, 3, 0, 3);
    words[n++] = ms_dst(MS_REG_PRED, 0, mask, 0);
    words[n++] = ms_src(MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE);
    words[n++] = ms_src(MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE);
    words[n++] = MS_OP_END;

    const MOJOSHADER_parseData *pd = parse_words(words, n);
    CHECK(pd->error_count == 0, "setp: parses cleanly");
    /* Non-predicated partial SETP uses a masked vector write, which itself
     *  never touches the components outside the mask. */
    CHECK(pd->output && str_contains(pd->output, "vs_p0.xz ="),
          "setp: masked vector write to xz");
    CHECK(pd->output && !str_contains(pd->output, "vs_p0.y ="),
          "setp: leaves y");
    CHECK(pd->output && !str_contains(pd->output, "vs_p0.w ="),
          "setp: leaves w");
    /* The untouched model components must survive unchanged. */
    CHECK(expected[1] == 8 && expected[3] == 10,
          "setp: model keeps unwritten booleans");
    CHECK(expected[0] == 1 && expected[2] == 1,
          "setp: model computes GE on masked lanes");
    MOJOSHADER_freeParseData(pd);
}

static void test_source_destination_aliasing(void)
{
    count_case();
    /* r0 is both destination and a reordered source. The candidate vector
     *  must be fully computed before any channel of r0 is overwritten. */
    ms_state model;
    memset(&model, 0, sizeof (model));
    model.r[0] = ms_v4(1.0f, 2.0f, 3.0f, 4.0f);
    model.p = (ms_bvec4) { 1, 1, 1, 1 };

    math_case tc = {
        MS_OP_MOV, 0, 0xf, 0,
        { { MS_REG_TEMP, 0, 0x1B /* .wzyx reversal */, MS_MOD_NONE } },
        1, MS_MOD_NONE, 0xE4
    };
    /* swizzle 0x1B maps x<-w, y<-z, z<-y, w<-x. */
    run_math_model(&model, &tc);
    CHECK(feq(model.r[0].x, 4.0f), "alias: x gets old w");
    CHECK(feq(model.r[0].y, 3.0f), "alias: y gets old z");
    CHECK(feq(model.r[0].z, 2.0f), "alias: z gets old y");
    CHECK(feq(model.r[0].w, 1.0f), "alias: w gets old x");

    uint32_t words[32];
    size_t n = emit_vs_header(words);
    n += emit_math_words(&tc, words + n);
    words[n++] = MS_OP_END;

    const MOJOSHADER_parseData *pd = parse_words(words, n);
    CHECK(pd->error_count == 0, "alias: parses cleanly");
    CHECK(pd->output && str_contains(pd->output, "ms_pred_"),
          "alias: candidate snapshotted into a temporary");
    MOJOSHADER_freeParseData(pd);
}

static void test_saturate_only_on_writes(void)
{
    count_case();
    ms_state model;
    memset(&model, 0, sizeof (model));
    model.r[1] = ms_v4(2.0f, -2.0f, 0.5f, 9.0f);
    model.r[0] = ms_v4(0.25f, 0.25f, 0.25f, 0.25f);
    model.p = (ms_bvec4) { 1, 0, 1, 0 };

    math_case tc = {
        MS_OP_MOV, 0, 0xf, 1 /* saturate */,
        { { MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE } },
        1, MS_MOD_NONE, 0xE4
    };
    run_math_model(&model, &tc);
    CHECK(feq(model.r[0].x, 1.0f),  "sat: x clamped high");
    CHECK(feq(model.r[0].y, 0.25f), "sat: y predicate-off, not clamped");
    CHECK(feq(model.r[0].z, 0.5f),  "sat: z passes through");
    CHECK(feq(model.r[0].w, 0.25f), "sat: w predicate-off, not clamped");

    uint32_t words[32];
    size_t n = emit_vs_header(words);
    n += emit_math_words(&tc, words + n);
    words[n++] = MS_OP_END;

    const MOJOSHADER_parseData *pd = parse_words(words, n);
    CHECK(pd->error_count == 0, "sat: parses cleanly");
    /* Saturation is applied per enabled channel (mask .xyzw => four), each
     *  inside its own predicate guard. The numeric model above proves that
     *  predicate-closed lanes keep their unclamped old value. */
    CHECK(pd->output && str_contains(pd->output, "clamp("),
          "sat: clamp emitted");
    CHECK(pd->output &&
          str_occurrences(pd->output, "clamp(") == 4,
          "sat: one clamp per enabled channel, all behind guards");
    MOJOSHADER_freeParseData(pd);
}

static void test_texldl_single_fetch(void)
{
    count_case();
    ms_state model;
    memset(&model, 0, sizeof (model));
    model.r[0] = ms_v4(0.3f, 0.4f, 0.0f, 2.0f);  /* xy coord, w = lod */
    model.p = (ms_bvec4) { 1, 0, 1, 0 };

    /* One CPU sample, then channel-gated submission. */
    float coord[4] = { model.r[0].x, model.r[0].y, 0.0f, 0.0f };
    ms_vec4 fetched = ms_sample_2d(&model, 0, coord, model.r[0].w);
    const int mask = 0xf;
    ms_vec4 prior = ms_v4(0.01f, 0.02f, 0.03f, 0.04f);
    model.r[1] = prior;
    for (int c = 0; c < 4; c++)
        if ((mask & (1 << c)) && ms_read_pred(&model, 0xE4, MS_MOD_NONE, c))
            (&model.r[1].x)[c] = (&fetched.x)[c];
    CHECK(model.tex[0].calls == 1, "texldl: exactly one CPU sample");
    CHECK(model.tex[0].sampler == 0, "texldl: sampler id recorded");
    CHECK(feq(model.tex[0].lod, 2.0f), "texldl: lod passed through");
    CHECK(feq(model.tex[0].coord[0], 0.3f), "texldl: coord x");
    CHECK(feq(model.tex[0].coord[1], 0.4f), "texldl: coord y");
    /* Open lanes (x,z) carry the exact stub color; closed lanes (y,w) keep
     *  the prior register contents. This validates the color, not just text. */
    CHECK(feq(model.r[1].x, fetched.x), "texldl: open x takes sample color");
    CHECK(feq(model.r[1].z, fetched.z), "texldl: open z takes sample color");
    CHECK(feq(model.r[1].y, prior.y), "texldl: closed y keeps old value");
    CHECK(feq(model.r[1].w, prior.w), "texldl: closed w keeps old value");

    uint32_t words[32];
    size_t n = emit_vs_header(words);
    /* add a 2D sampler declaration. */
    words[n++] = ms_inst(MS_OP_DCL, 2, 0, 0);
    words[n++] = MS_DCL_SAMPLER2D;
    words[n++] = ms_dst(MS_REG_SAMPLER, 0, 0xf, 0);
    /* initialize the coordinate/lod temp r0 from v1 first. */
    words[n++] = ms_inst(MS_OP_MOV, 2, 0, 0);
    words[n++] = ms_dst(MS_REG_TEMP, 0, 0xf, 0);
    words[n++] = ms_src(MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE);
    words[n++] = ms_inst(MS_OP_TEXLDL, 4, 1, 0);
    words[n++] = ms_dst(MS_REG_TEMP, 1, 0xf, 0);
    words[n++] = ms_src(MS_REG_TEMP, 0, 0xE4, MS_MOD_NONE);
    words[n++] = ms_src(MS_REG_SAMPLER, 0, 0xE4, MS_MOD_NONE);
    words[n++] = ms_src(MS_REG_PRED, 0, 0xE4, MS_MOD_NONE);
    words[n++] = MS_OP_END;

    const MOJOSHADER_parseData *pd = parse_words(words, n);
    CHECK(pd->error_count == 0, "texldl: parses cleanly");
    CHECK(pd->output &&
          str_occurrences(pd->output, "= texture2DLod(") == 1,
          "texldl: one GLSL fetch for all channels");
    CHECK(pd->output && str_contains(pd->output, "vs_r0.xy, vs_r0.w"),
          "texldl: coord .xy and lod .w");
    MOJOSHADER_freeParseData(pd);
}

static void test_texldd_gradients_ps(void)
{
    count_case();
    ms_state model;
    memset(&model, 0, sizeof (model));
    model.r[0] = ms_v4(0.1f, 0.2f, 0.3f, 0.4f);  /* coordinate */
    model.p = (ms_bvec4) { 0, 1, 0, 1 };          /* mixed predicate lanes. */

    /* One explicit-gradient sample through the CPU stub, recording the
     *  coordinate and both gradient groups, then channel-gated submission. */
    float coord[4] = { model.r[0].x, model.r[0].y, model.r[0].z,
                       model.r[0].w };
    float gradx[4] = { model.r[0].x, model.r[0].y, 0.0f, 0.0f };
    float grady[4] = { model.r[0].x, model.r[0].y, 0.0f, 0.0f };
    ms_vec4 fetched = ms_sample_grad_2d(&model, 0, coord, gradx, grady);
    ms_vec4 prior = ms_v4(0.5f, 0.6f, 0.7f, 0.8f);
    model.r[1] = prior;
    for (int c = 0; c < 4; c++)
        if (ms_read_pred(&model, 0xE4, MS_MOD_NONE, c))
            (&model.r[1].x)[c] = (&fetched.x)[c];

    CHECK(model.tex[0].calls == 1, "texldd: exactly one CPU sample");
    CHECK(model.tex[0].sampler == 0, "texldd: sampler id recorded");
    CHECK(feq(model.tex[0].coord[0], 0.1f) &&
          feq(model.tex[0].coord[1], 0.2f), "texldd: coordinate captured");
    CHECK(feq(model.tex[0].gradx[0], 0.1f) &&
          feq(model.tex[0].gradx[1], 0.2f),
          "texldd: gradient group x captured");
    CHECK(feq(model.tex[0].grady[0], 0.1f) &&
          feq(model.tex[0].grady[1], 0.2f),
          "texldd: gradient group y captured");
    /* Open lanes are y,w; closed lanes x,z keep their prior values. */
    CHECK(feq(model.r[1].y, fetched.y), "texldd: open y takes sample color");
    CHECK(feq(model.r[1].w, fetched.w), "texldd: open w takes sample color");
    CHECK(feq(model.r[1].x, prior.x), "texldd: closed x keeps old value");
    CHECK(feq(model.r[1].z, prior.z), "texldd: closed z keeps old value");

    uint32_t words[40];
    size_t n = emit_ps_header(words, MS_DCL_SAMPLER2D);
    /* setp to give p0 real values, then predicated texldd, then output. */
    words[n++] = ms_inst(MS_OP_SETP, 3, 0, 3);
    words[n++] = ms_dst(MS_REG_PRED, 0, 0xf, 0);
    words[n++] = ms_src(MS_REG_INPUT, 0, 0xE4, MS_MOD_NONE);
    words[n++] = ms_src(MS_REG_INPUT, 0, 0xE4, MS_MOD_NONE);
    words[n++] = ms_inst(MS_OP_TEXLDD, 6, 1, 0);
    words[n++] = ms_dst(MS_REG_TEMP, 1, 0xf, 0);
    words[n++] = ms_src(MS_REG_INPUT, 0, 0xE4, MS_MOD_NONE);   /* coords */
    words[n++] = ms_src(MS_REG_SAMPLER, 0, 0xE4, MS_MOD_NONE);
    words[n++] = ms_src(MS_REG_INPUT, 0, 0xE4, MS_MOD_NONE);   /* ddx */
    words[n++] = ms_src(MS_REG_INPUT, 0, 0xE4, MS_MOD_NONE);   /* ddy */
    words[n++] = ms_src(MS_REG_PRED, 0, 0xE4, MS_MOD_NONE);
    words[n++] = ms_inst(MS_OP_MOV, 2, 0, 0);
    words[n++] = ms_dst(MS_REG_COLOROUT, 0, 0xf, 0);
    words[n++] = ms_src(MS_REG_TEMP, 1, 0xE4, MS_MOD_NONE);
    words[n++] = MS_OP_END;

    const MOJOSHADER_parseData *pd = parse_words(words, n);
    CHECK(pd->error_count == 0, "texldd: parses cleanly");
    /* The "#define texture2DGrad ..." preflight lines are not calls; count
     *  only the single assignment that actually performs the fetch. */
    CHECK(pd->output &&
          str_occurrences(pd->output, "= texture2DGrad(") == 1,
          "texldd: one GLSL fetch");
    CHECK(pd->output &&
          str_contains(pd->output, "ps_v0.xy, ps_v0.xy, ps_v0.xy"),
          "texldd: coords and both gradient groups forwarded");
    MOJOSHADER_freeParseData(pd);
}

static void test_loop_and_helper_reuse(void)
{
    count_case();
    /* Numeric side: apply a predicated op repeatedly, as it would run on each
     *  loop iteration and inside a called helper. */
    ms_state model;
    memset(&model, 0, sizeof (model));
    model.p = (ms_bvec4) { 1, 0, 1, 0 };
    math_case tc = {
        MS_OP_MUL, 0, 0xf, 0,
        { { MS_REG_TEMP, 0, 0xE4, MS_MOD_NONE },
          { MS_REG_TEMP, 1, 0xE4, MS_MOD_NONE } },
        1, MS_MOD_NONE, 0xE4
    };
    model.r[1] = ms_v4(2.0f, 2.0f, 2.0f, 2.0f);
    model.r[0] = ms_v4(1.0f, 1.0f, 1.0f, 1.0f);
    for (int iter = 0; iter < 3; iter++)
        run_math_model(&model, &tc);
    /* x,z multiply three times (2^3); y,w stay at their seed. */
    CHECK(feq(model.r[0].x, 8.0f), "loop: open lanes accumulate");
    CHECK(feq(model.r[0].y, 1.0f), "loop: closed lanes never change");
    CHECK(feq(model.r[0].z, 8.0f), "loop: z accumulates");
    CHECK(feq(model.r[0].w, 1.0f), "loop: w never changes");

    /* Structural side: real LOOP/ENDLOOP and LABEL/CALL around predicated
     *  MOV, mirroring tests/1.vsa's control-flow encoding. */
    uint32_t words[64];
    size_t n = emit_vs_header(words);
    /* defi i0 = (0, 3, 1, 0): start, count, step, unused. One dst plus four
     *  value dwords = five following tokens. */
    words[n++] = ms_inst(MS_OP_DEFI, 5, 0, 0);
    words[n++] = ms_dst(MS_REG_CONSTINT, 0, 0xf, 0);
    words[n++] = 0; words[n++] = 3; words[n++] = 1; words[n++] = 0;
    /* loop aL, i0 ... endloop (LOOP has two source dwords). */
    words[n++] = ms_inst(MS_OP_LOOP, 2, 0, 0);
    words[n++] = ms_src(MS_REG_LOOP, 0, 0xE4, MS_MOD_NONE);
    words[n++] = ms_src(MS_REG_CONSTINT, 0, 0xE4, MS_MOD_NONE);
    math_case inner = {
        MS_OP_MOV, 0, 0xf, 0,
        { { MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE } },
        1, MS_MOD_NONE, 0xE4
    };
    n += emit_math_words(&inner, words + n);
    words[n++] = ms_inst(MS_OP_ENDLOOP, 0, 0, 0);
    /* call l0; ret; label l0; predicated mov; ret */
    words[n++] = ms_inst(25 /* CALL */, 1, 0, 0);
    words[n++] = ms_src(MS_REG_LABEL, 0, 0xE4, MS_MOD_NONE);
    words[n++] = ms_inst(MS_OP_RET, 0, 0, 0);
    words[n++] = ms_inst(MS_OP_LABEL, 1, 0, 0);
    words[n++] = ms_src(MS_REG_LABEL, 0, 0xE4, MS_MOD_NONE);
    n += emit_math_words(&inner, words + n);
    words[n++] = ms_inst(MS_OP_RET, 0, 0, 0);
    words[n++] = MS_OP_END;

    const MOJOSHADER_parseData *pd = parse_words(words, n);
    CHECK(pd->error_count == 0, "control-flow: parses cleanly");
    CHECK(pd->output && str_contains(pd->output, "for (int aL"),
          "control-flow: predicated op sits in a loop");
    CHECK(pd->output && str_contains(pd->output, "l0()"),
          "control-flow: helper is called");
    CHECK(pd->output && str_occurrences(pd->output, "ms_pred_") >= 2,
          "control-flow: stable temps used in loop body and helper");
    MOJOSHADER_freeParseData(pd);
}

static void test_supported_opcode_sweep(void)
{
    count_case();
    /* Every arithmetic op the feature promises must translate under a
     *  predicate, emit per-channel guards, and avoid post-1.20 idioms. */
    const struct { int op; int sources; const char *name; } ops[] = {
        { MS_OP_MOV, 1, "mov" }, { MS_OP_ADD, 2, "add" },
        { MS_OP_SUB, 2, "sub" }, { MS_OP_MUL, 2, "mul" },
        { MS_OP_MAD, 3, "mad" }, { MS_OP_MIN, 2, "min" },
        { MS_OP_MAX, 2, "max" }, { MS_OP_DP3, 2, "dp3" },
        { MS_OP_DP4, 2, "dp4" }, { MS_OP_CMP, 3, "cmp" },
    };
    for (size_t i = 0; i < sizeof (ops) / sizeof (ops[0]); i++)
    {
        math_case tc;
        memset(&tc, 0, sizeof (tc));
        tc.op = ops[i].op;
        tc.dst = 0;
        tc.mask = 0xf;
        tc.predicated = 1;
        tc.pred_mod = MS_MOD_NONE;
        tc.pred_swiz = 0xE4;
        /* CMP is pixel-shader-only; everything else runs in the VS fixture.
         *  The PS header declares v0, the VS header declares v0 and v1. */
        const int is_ps = (ops[i].op == MS_OP_CMP);
        uint32_t words[32];
        size_t n = is_ps ? emit_ps_header(words, MS_DCL_SAMPLER2D)
                         : emit_vs_header(words);
        for (int s = 0; s < ops[i].sources; s++)
        {
            tc.s[s].regtype = MS_REG_INPUT;
            tc.s[s].regnum = is_ps ? 0 : 1;
            tc.s[s].swizzle = 0xE4;
            tc.s[s].mod = MS_MOD_NONE;
        }
        n += emit_math_words(&tc, words + n);
        words[n++] = MS_OP_END;

        const MOJOSHADER_parseData *pd = parse_words(words, n);
        CHECK(pd->error_count == 0, "sweep %s: parses cleanly", ops[i].name);
        CHECK(str_contains(pd->output, "ms_pred_"),
              "sweep %s: candidate temporary", ops[i].name);
        CHECK(str_occurrences(pd->output, "if (") >= 4,
              "sweep %s: four per-channel guards", ops[i].name);
        CHECK(!str_contains(pd->output, "any(") &&
              !str_contains(pd->output, "all("),
              "sweep %s: no vector predicate reduction", ops[i].name);
        CHECK(!str_contains(pd->output, "mix("),
              "sweep %s: no boolean-vector mix", ops[i].name);
        MOJOSHADER_freeParseData(pd);
    }
}

static void test_cmp_predicated_numeric(void)
{
    count_case();
    /* Mixed signs so the >= 0 branch differs per channel; predicate selects
     *  which of the four results are actually committed. */
    ms_state model;
    memset(&model, 0, sizeof (model));
    model.r[1] = ms_v4(1.0f, -1.0f, 0.25f, -4.0f);   /* src0 selector */
    model.r[2] = ms_v4(10.0f, 20.0f, 30.0f, 40.0f);  /* src1 (>=0) */
    model.r[3] = ms_v4(100.0f, 200.0f, 300.0f, 400.0f); /* src2 (<0) */
    model.r[0] = ms_v4(7.0f, 7.0f, 7.0f, 7.0f);      /* prior destination */
    model.p = (ms_bvec4) { 1, 1, 0, 0 };

    math_case tc = {
        MS_OP_CMP, 0, 0xf, 0,
        { { MS_REG_TEMP, 1, 0xE4, MS_MOD_NONE },
          { MS_REG_TEMP, 2, 0xE4, MS_MOD_NONE },
          { MS_REG_TEMP, 3, 0xE4, MS_MOD_NONE } },
        1, MS_MOD_NONE, 0xE4
    };
    run_math_model(&model, &tc);
    CHECK(feq(model.r[0].x, 10.0f),  "cmp: x takes src1 (>=0), pred open");
    CHECK(feq(model.r[0].y, 200.0f), "cmp: y takes src2 (<0), pred open");
    CHECK(feq(model.r[0].z, 7.0f),   "cmp: z predicate closed, preserved");
    CHECK(feq(model.r[0].w, 7.0f),   "cmp: w predicate closed, preserved");

    uint32_t words[32];
    size_t n = emit_ps_header(words, MS_DCL_SAMPLER2D);
    /* Initialize the three source temps from the declared input so the
     *  parser's uninitialized-temp check passes. The numeric model above
     *  supplies the actual per-channel values used for the assertions. */
    for (int t = 1; t <= 3; t++)
    {
        words[n++] = ms_inst(MS_OP_MOV, 2, 0, 0);
        words[n++] = ms_dst(MS_REG_TEMP, t, 0xf, 0);
        words[n++] = ms_src(MS_REG_INPUT, 0, 0xE4, MS_MOD_NONE);
    }
    n += emit_math_words(&tc, words + n);
    words[n++] = MS_OP_END;

    const MOJOSHADER_parseData *pd = parse_words(words, n);
    CHECK(pd->error_count == 0, "cmp: parses cleanly");
    CHECK(str_contains(pd->output, ">= 0.0"), "cmp: ternary comparison");
    CHECK(str_occurrences(pd->output, "if (ps_p0.") == 4,
          "cmp: all four lanes guarded");
    MOJOSHADER_freeParseData(pd);
}

static void test_dp3_sparse_mask(void)
{
    count_case();
    /* A sparse .xy DP3: one scalar dot product, broadcast into the two
     *  enabled channels and committed independently. */
    ms_state model;
    memset(&model, 0, sizeof (model));
    model.r[1] = ms_v4(1.0f, 2.0f, 3.0f, 0.0f);
    model.r[2] = ms_v4(4.0f, 5.0f, 6.0f, 0.0f);
    model.r[0] = ms_v4(9.0f, 9.0f, 9.0f, 9.0f);
    model.p = (ms_bvec4) { 1, 0, 1, 1 };

    math_case tc = {
        MS_OP_DP3, 0, 0x3 /* .xy */, 0,
        { { MS_REG_TEMP, 1, 0xE4, MS_MOD_NONE },
          { MS_REG_TEMP, 2, 0xE4, MS_MOD_NONE } },
        1, MS_MOD_NONE, 0xE4
    };
    run_math_model(&model, &tc);
    const float dot3 = 1.0f * 4.0f + 2.0f * 5.0f + 3.0f * 6.0f; /* 32 */
    CHECK(feq(model.r[0].x, dot3), "dp3: x gets dot product");
    CHECK(feq(model.r[0].y, 9.0f), "dp3: y predicate closed, preserved");
    CHECK(feq(model.r[0].z, 9.0f), "dp3: z masked, preserved");
    CHECK(feq(model.r[0].w, 9.0f), "dp3: w masked, preserved");

    uint32_t words[32];
    size_t n = emit_vs_header(words);
    /* Initialize the source temps from the declared v1 input. The model above
     *  carries the actual vectors the numeric assertions use. */
    for (int t = 1; t <= 2; t++)
    {
        words[n++] = ms_inst(MS_OP_MOV, 2, 0, 0);
        words[n++] = ms_dst(MS_REG_TEMP, t, 0xf, 0);
        words[n++] = ms_src(MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE);
    }
    n += emit_math_words(&tc, words + n);
    words[n++] = MS_OP_END;

    const MOJOSHADER_parseData *pd = parse_words(words, n);
    CHECK(pd->error_count == 0, "dp3: parses cleanly");
    CHECK(str_contains(pd->output, "vec2(dot("),
          "dp3: compact vec2 candidate aligned to .xy");
    CHECK(!str_contains(pd->output, ".z =") &&
          !str_contains(pd->output, ".w ="),
          "dp3: masked channels never assigned");
    MOJOSHADER_freeParseData(pd);
}

static void test_predicated_setp_partial(void)
{
    count_case();
    /* A predicated, partial SETP into p0 itself: the gate reads the old
     *  predicate, the .xz result is packed as a compact bvec2, and lanes
     *  outside the mask (or closed by the predicate) keep their booleans. */
    ms_state model;
    memset(&model, 0, sizeof (model));
    model.r[1] = ms_v4(1.0f, -1.0f, 1.0f, -1.0f);
    model.p = (ms_bvec4) { 1, 1, 0, 1 };  /* x open, z closed for the write. */

    const int mask = 0x5;  /* .xz */
    const ms_bvec4 before = model.p;
    /* SETP_LT: a masked lane is rewritten to (src < 0) only when its old
     *  predicate lane is open. Here x is open, z is closed. */
    ms_bvec4 expected = before;
    if (before.x) expected.x = (vec_get(&model, 1, 0) < 0.0f);  /* 1 < 0 -> 0 */
    /* z is predicate-closed, y/w are outside the mask: all stay unchanged. */
    CHECK(expected.x == 0, "pred-setp: open x recomputed to false");
    CHECK(before.y == expected.y && before.w == expected.w,
          "pred-setp: unmasked booleans preserved");
    CHECK(before.z == expected.z, "pred-setp: predicate-closed z preserved");

    uint32_t words[32];
    size_t n = emit_vs_header(words);
    /* establish p0 first, then predicated partial setp_lt (control 2). */
    words[n++] = ms_inst(MS_OP_SETP, 3, 0, 3);
    words[n++] = ms_dst(MS_REG_PRED, 0, 0xf, 0);
    words[n++] = ms_src(MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE);
    words[n++] = ms_src(MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE);
    words[n++] = ms_inst(MS_OP_SETP, 4, 1, 2);
    words[n++] = ms_dst(MS_REG_PRED, 0, mask, 0);
    words[n++] = ms_src(MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE);
    words[n++] = ms_src(MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE);
    words[n++] = ms_src(MS_REG_PRED, 0, 0xE4, MS_MOD_NONE);
    words[n++] = MS_OP_END;

    const MOJOSHADER_parseData *pd = parse_words(words, n);
    CHECK(pd->error_count == 0, "pred-setp: parses cleanly");
    CHECK(str_contains(pd->output, "bvec2 ms_pred_"),
          "pred-setp: compact bvec2 candidate for the .xz mask");
    CHECK(str_contains(pd->output, "if (vs_p0.x) { vs_p0.x = ms_pred_0.x; }"),
          "pred-setp: x gated on the old predicate");
    CHECK(str_contains(pd->output, "if (vs_p0.z) { vs_p0.z = ms_pred_0.y; }"),
          "pred-setp: z reads the second compact lane");
    CHECK(!str_contains(pd->output, "vs_p0.y =") &&
          !str_contains(pd->output, "vs_p0.w ="),
          "pred-setp: y and w never assigned");
    MOJOSHADER_freeParseData(pd);
}

static void test_stage_restrictions(void)
{
    count_case();
    /* CMP and TEXLDD are pixel-only. Predication must not widen their stage
     *  availability; the parser's shader-type check fires first. */
    {
        uint32_t w[24];
        size_t n = emit_vs_header(w);
        /* predicated CMP: dst + 3 src + predicate = 5 following. */
        w[n++] = ms_inst(MS_OP_CMP, 5, 1, 0);
        w[n++] = ms_dst(MS_REG_TEMP, 0, 0xf, 0);
        for (int s = 0; s < 3; s++)
            w[n++] = ms_src(MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE);
        w[n++] = ms_src(MS_REG_PRED, 0, 0xE4, MS_MOD_NONE);
        w[n++] = MS_OP_END;
        const MOJOSHADER_parseData *pd = parse_words(w, n);
        int found = 0;
        for (int i = 0; i < pd->error_count; i++)
            if (str_contains(pd->errors[i].error, "not available"))
                found = 1;
        CHECK(pd->error_count > 0 && found,
              "stage: predicated CMP rejected in vertex shader");
        MOJOSHADER_freeParseData(pd);
    }
    {
        uint32_t w[24];
        size_t n = emit_vs_header(w);
        /* predicated TEXLDD: dst + 4 src + predicate = 6 following. */
        w[n++] = ms_inst(MS_OP_TEXLDD, 6, 1, 0);
        w[n++] = ms_dst(MS_REG_TEMP, 0, 0xf, 0);
        for (int s = 0; s < 4; s++)
            w[n++] = ms_src(MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE);
        w[n++] = ms_src(MS_REG_PRED, 0, 0xE4, MS_MOD_NONE);
        w[n++] = MS_OP_END;
        const MOJOSHADER_parseData *pd = parse_words(w, n);
        int found = 0;
        for (int i = 0; i < pd->error_count; i++)
            if (str_contains(pd->errors[i].error, "not available"))
                found = 1;
        CHECK(pd->error_count > 0 && found,
              "stage: predicated TEXLDD rejected in vertex shader");
        MOJOSHADER_freeParseData(pd);
    }

    /* TEXLDL is legal in BOTH stages: confirm the pixel form translates. */
    {
        uint32_t w[32];
        size_t n = emit_ps_header(w, MS_DCL_SAMPLER2D);
        /* initialize coordinate/lod temp r0 from the declared v0. */
        w[n++] = ms_inst(MS_OP_MOV, 2, 0, 0);
        w[n++] = ms_dst(MS_REG_TEMP, 0, 0xf, 0);
        w[n++] = ms_src(MS_REG_INPUT, 0, 0xE4, MS_MOD_NONE);
        w[n++] = ms_inst(MS_OP_TEXLDL, 4, 1, 0);
        w[n++] = ms_dst(MS_REG_TEMP, 1, 0xf, 0);
        w[n++] = ms_src(MS_REG_TEMP, 0, 0xE4, MS_MOD_NONE);
        w[n++] = ms_src(MS_REG_SAMPLER, 0, 0xE4, MS_MOD_NONE);
        w[n++] = ms_src(MS_REG_PRED, 0, 0xE4, MS_MOD_NONE);
        w[n++] = ms_inst(MS_OP_MOV, 2, 0, 0);
        w[n++] = ms_dst(MS_REG_COLOROUT, 0, 0xf, 0);
        w[n++] = ms_src(MS_REG_TEMP, 1, 0xE4, MS_MOD_NONE);
        w[n++] = MS_OP_END;
        const MOJOSHADER_parseData *pd = parse_words(w, n);
        CHECK(pd->error_count == 0, "stage: predicated TEXLDL allowed in PS");
        CHECK(str_occurrences(pd->output, "= texture2DLod(") == 1,
              "stage: PS texldl performs one fetch");
        MOJOSHADER_freeParseData(pd);
    }
}

static void test_deterministic_output(void)
{
    count_case();
    uint32_t words[32];
    size_t n = emit_vs_header(words);
    math_case tc = {
        MS_OP_MAD, 0, 0xf, 0,
        { { MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE },
          { MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE },
          { MS_REG_INPUT, 1, 0xE4, MS_MOD_NONE } },
        1, MS_MOD_NOT, 0xE4
    };
    n += emit_math_words(&tc, words + n);
    words[n++] = MS_OP_END;

    const MOJOSHADER_parseData *a = parse_words(words, n);
    const MOJOSHADER_parseData *b = parse_words(words, n);
    CHECK(a->error_count == 0 && b->error_count == 0,
          "determinism: both parses succeed");
    CHECK(a->output && b->output && strcmp(a->output, b->output) == 0,
          "determinism: repeated translation is byte-identical");
    CHECK(str_contains(a->output, "ms_pred_0"),
          "determinism: temporary name is stable");
    MOJOSHADER_freeParseData(a);
    MOJOSHADER_freeParseData(b);
}

static void test_plain_unchanged(void)
{
    count_case();
    /* The exact non-predicated baseline fixture. */
    const uint32_t plain[] = {
        0xfffe0300, 0x0200001f, 0x80000000, 0x900f0000,
        0x0200001f, 0x80000000, 0xe00f0000, 0x02000001,
        0xe00f0000, 0x90e40000, 0x0000ffff
    };
    const MOJOSHADER_parseData *pd =
        parse_words(plain, sizeof (plain) / sizeof (plain[0]));
    CHECK(pd->error_count == 0, "plain: no errors");
    CHECK(pd->output && !str_contains(pd->output, "ms_pred_"),
          "plain: no predicate temporaries introduced");
    CHECK(pd->output && !str_contains(pd->output, "if ("),
          "plain: no guard blocks introduced");
    MOJOSHADER_freeParseData(pd);
}

static void expect_error(const uint32_t *words, size_t n, const char *needle,
                         const char *name)
{
    count_case();
    const MOJOSHADER_parseData *pd = parse_words(words, n);
    int found = 0;
    for (int i = 0; i < pd->error_count; i++)
        if (str_contains(pd->errors[i].error, needle))
            found = 1;
    CHECK(pd->error_count > 0 && found, "%s: expected error containing '%s'",
          name, needle);
    MOJOSHADER_freeParseData(pd);
}

static void test_rejections(void)
{
    /* Predicated RCP: not on the supported opcode list. */
    {
        uint32_t w[16];
        size_t n = emit_vs_header(w);
        w[n++] = ms_inst(6 /* RCP */, 3, 1, 0);
        w[n++] = ms_dst(MS_REG_TEMP, 0, 1, 0);
        w[n++] = ms_src(MS_REG_TEMP, 1, ms_replicate(0), MS_MOD_NONE);
        w[n++] = ms_src(MS_REG_PRED, 0, 0xE4, MS_MOD_NONE);
        w[n++] = MS_OP_END;
        expect_error(w, n, "predicated destination unsupported", "rcp");
    }

    /* Predicated IF: control flow must not swallow the predicate. */
    {
        uint32_t w[16];
        size_t n = emit_vs_header(w);
        w[n++] = ms_inst(MS_OP_IF, 2, 1, 0);
        w[n++] = ms_src(MS_REG_PRED, 0, ms_replicate(0), MS_MOD_NONE);
        w[n++] = ms_src(MS_REG_PRED, 0, 0xE4, MS_MOD_NONE);
        w[n++] = MS_OP_END;
        expect_error(w, n, "predicated destination unsupported", "if");
    }

    /* Non-2D sampler on a predicated TEXLDL. */
    {
        uint32_t w[24];
        size_t n = emit_vs_header(w);
        w[n++] = ms_inst(MS_OP_DCL, 2, 0, 0);
        w[n++] = MS_DCL_SAMPLERVOL;
        w[n++] = ms_dst(MS_REG_SAMPLER, 0, 0xf, 0);
        w[n++] = ms_inst(MS_OP_TEXLDL, 4, 1, 0);
        w[n++] = ms_dst(MS_REG_TEMP, 1, 0xf, 0);
        w[n++] = ms_src(MS_REG_TEMP, 0, 0xE4, MS_MOD_NONE);
        w[n++] = ms_src(MS_REG_SAMPLER, 0, 0xE4, MS_MOD_NONE);
        w[n++] = ms_src(MS_REG_PRED, 0, 0xE4, MS_MOD_NONE);
        w[n++] = MS_OP_END;
        expect_error(w, n, "only supports 2D samplers", "volume-texldl");
    }

    /* A predicate token that is not a predicate register. */
    {
        uint32_t w[16];
        size_t n = emit_vs_header(w);
        w[n++] = ms_inst(MS_OP_MOV, 3, 1, 0);
        w[n++] = ms_dst(MS_REG_TEMP, 0, 0xf, 0);
        w[n++] = ms_src(MS_REG_TEMP, 1, 0xE4, MS_MOD_NONE);
        w[n++] = ms_src(MS_REG_TEMP, 2, 0xE4, MS_MOD_NONE);
        w[n++] = MS_OP_END;
        expect_error(w, n, "not predicate register", "bad-pred-reg");
    }

    /* glsl120-only: the same predicated MOV fails on the glsl 1.10 profile. */
    {
        count_case();
        uint32_t w[16];
        size_t n = emit_vs_header(w);
        math_case tc = {
            MS_OP_MOV, 0, 0xf, 0,
            { { MS_REG_TEMP, 1, 0xE4, MS_MOD_NONE } },
            1, MS_MOD_NONE, 0xE4
        };
        n += emit_math_words(&tc, w + n);
        w[n++] = MS_OP_END;
        const MOJOSHADER_parseData *pd = MOJOSHADER_parse(
            "glsl", "main", (const unsigned char *) w,
            (unsigned)(n * sizeof (uint32_t)),
            NULL, 0, NULL, 0, NULL, NULL, NULL);
        int found = 0;
        for (int i = 0; i < pd->error_count; i++)
            if (str_contains(pd->errors[i].error, "glsl120 profile"))
                found = 1;
        CHECK(pd->error_count > 0 && found,
              "glsl110: predicated MOV rejected outside glsl120");
        MOJOSHADER_freeParseData(pd);
    }
}

int main(void)
{
    test_mixed_predicate_and_mask();
    test_negated_replicate_predicate();
    test_per_component_negate_no_swizzle();
    test_setp_partial_update();
    test_source_destination_aliasing();
    test_saturate_only_on_writes();
    test_texldl_single_fetch();
    test_texldd_gradients_ps();
    test_supported_opcode_sweep();
    test_cmp_predicated_numeric();
    test_dp3_sparse_mask();
    test_predicated_setp_partial();
    test_stage_restrictions();
    test_loop_and_helper_reuse();
    test_deterministic_output();
    test_plain_unchanged();
    test_rejections();

    printf("predication: cases=%d assertions=%d failures=%d\n",
           g_cases, g_assertions, g_failures);
    return g_failures ? 1 : 0;
}
