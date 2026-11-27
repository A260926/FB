/*
 * Predicated-instruction translation tests for the glsl120 profile.
 *
 * All shader bytecode is constructed here as legal Shader Model 3 words
 * (no external assembler). Every fixture is paired with a readable assembly
 * listing under checks/asm/. The numeric behavior of the predicated ops is
 * checked by an independent SM3 register execution model below, while the
 * GLSL emitted through MOJOSHADER_parse() is checked structurally (stable
 * temporaries, per-component predicate guards, a single texture sample per
 * instruction). Nothing here needs a GPU, OpenGL, Python, or extra libs.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 */

#include "mojoshader.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ----------------------------------------------------------------------- */
/* Test accounting                                                          */
/* ----------------------------------------------------------------------- */

static int g_cases = 0;
static int g_assertions = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_assertions++; \
    if (!(cond)) { \
        g_failures++; \
        printf("  ASSERT FAILED (%s:%d): %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static void case_begin(const char *name)
{
    g_cases++;
    printf("case %d: %s\n", g_cases, name);
} /* case_begin */

/* ----------------------------------------------------------------------- */
/* SM3 token construction                                                   */
/* ----------------------------------------------------------------------- */

/* Register types, mirrored from mojoshader_internal.h (kept local so this
   test only depends on the public API). */
enum {
    T_TEMP = 0, T_INPUT = 1, T_CONST = 2, T_ADDRESS = 3, T_OUTPUT = 6,
    T_CONSTINT = 7, T_COLOROUT = 8, T_SAMPLER = 10, T_CONSTBOOL = 14,
    T_LOOP = 15, T_LABEL = 18, T_PREDICATE = 19
};

enum { SRCMOD_NONE = 0, SRCMOD_NEGATE = 1, SRCMOD_ABS = 11, SRCMOD_NOT = 13 };

/* SM3 comparison controls shared by SETP. */
enum { CMP_GT = 0, CMP_EQ = 1, CMP_GE = 2, CMP_LT = 3, CMP_NE = 4, CMP_LE = 5 };

#define SW_XYZW 0xE4u
#define SWZ(x,y,z,w) ((uint32_t)((x)|((y)<<2)|((z)<<4)|((w)<<6)))
#define SW_REP(c) SWZ(c,c,c,c)

#define OP_MOV 1
#define OP_ADD 2
#define OP_SUB 3
#define OP_MAD 4
#define OP_MUL 5
#define OP_RCP 6
#define OP_DP3 8
#define OP_DP4 9
#define OP_MIN 10
#define OP_MAX 11
#define OP_CALL 25
#define OP_LOOP 27
#define OP_RET 28
#define OP_ENDLOOP 29
#define OP_LABEL 30
#define OP_DCL 31
#define OP_IF 40
#define OP_MOVA 46
#define OP_DEFI 48
#define OP_DEF 81
#define OP_CMP 88
#define OP_TEXLDD 93
#define OP_SETP 94
#define OP_TEXLDL 95
#define OP_BREAKP 96

#define VERSION_VS3 0xFFFE0300u
#define VERSION_PS3 0xFFFF0300u
#define EOS_TOKEN   0x0000FFFFu
#define TEXTURE_2D  2

typedef struct
{
    uint32_t w[4096];
    size_t n;
} WordBuf;

typedef struct { int rnum; int cnum; float v[4]; } TempInit;

static void wb_push(WordBuf *b, uint32_t word)
{
    if (b->n < sizeof (b->w) / sizeof (b->w[0]))
        b->w[b->n++] = word;
} /* wb_push */

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof (u));
    return u;
} /* fbits */

static uint32_t regtype_bits(int type)
{
    /* Parser: regtype = ((token>>28)&7) | (((token>>8)&0x18)), i.e. the high
       two bits live at token positions 11-12. */
    return ((uint32_t)(type & 0x7) << 28) |
           ((uint32_t)((type >> 3) & 0x3) << 11);
} /* regtype_bits */

static uint32_t tok_src(int type, int regnum, uint32_t swizzle, int mod)
{
    return 0x80000000u | regtype_bits(type) | ((uint32_t)mod << 24) |
           (swizzle << 16) | ((uint32_t)regnum & 0x7FFu);
} /* tok_src */

static uint32_t tok_dst(int type, int regnum, int writemask,
                        int resultmod, int shift)
{
    return 0x80000000u | regtype_bits(type) | ((uint32_t)shift << 24) |
           ((uint32_t)resultmod << 20) | ((uint32_t)writemask << 16) |
           ((uint32_t)regnum & 0x7FFu);
} /* tok_dst */

/* Predicate operand: component < 0 means the full p#.xyzw vector. */
static uint32_t tok_pred(int regnum, int negate, int component)
{
    const uint32_t swizzle = (component < 0) ? SW_XYZW : SW_REP(component);
    return tok_src(T_PREDICATE, regnum, swizzle,
                   negate ? SRCMOD_NOT : SRCMOD_NONE);
} /* tok_pred */

/* SM3 DCL layout: opcode token, an "extended" token (bit31 set, with the
   usage/index or texture type packed in), then the register destination. */
static void wb_dcl_input(WordBuf *b, int regnum, int usage, int index, int mask)
{
    wb_push(b, OP_DCL | (2u << 24));
    wb_push(b, 0x80000000u | ((uint32_t)index << 16) | (uint32_t)usage);
    wb_push(b, tok_dst(T_INPUT, regnum, mask, 0, 0));
} /* wb_dcl_input */

static void wb_dcl_sampler(WordBuf *b, int regnum)
{
    wb_push(b, OP_DCL | (2u << 24));
    wb_push(b, 0x80000000u | ((uint32_t)TEXTURE_2D << 27));
    wb_push(b, tok_dst(T_SAMPLER, regnum, 0xF, 0, 0));
} /* wb_dcl_sampler */

static void wb_def(WordBuf *b, int regnum, float x, float y, float z, float w)
{
    wb_push(b, OP_DEF | (5u << 24));
    wb_push(b, tok_dst(T_CONST, regnum, 0xF, 0, 0));
    wb_push(b, fbits(x)); wb_push(b, fbits(y));
    wb_push(b, fbits(z)); wb_push(b, fbits(w));
} /* wb_def */

/* Materialize machine-preset temps from constants with identical values.
   SM3 requires every DEF before any instruction, so the constants and the
   MOVs that load them are emitted in two separate passes. */
static void wb_temp_init_defs(WordBuf *b, const TempInit *inits, int n)
{
    int k;
    for (k = 0; k < n; k++)
        wb_def(b, inits[k].cnum, inits[k].v[0], inits[k].v[1],
               inits[k].v[2], inits[k].v[3]);
} /* wb_temp_init_defs */

static void wb_temp_init_movs(WordBuf *b, const TempInit *inits, int n)
{
    int k;
    for (k = 0; k < n; k++)
    {
        wb_push(b, OP_MOV | (2u << 24));
        wb_push(b, tok_dst(T_TEMP, inits[k].rnum, 0xF, 0, 0));
        wb_push(b, tok_src(T_CONST, inits[k].cnum, SW_XYZW, SRCMOD_NONE));
    } /* for */
} /* wb_temp_init_movs */

static void wb_defi(WordBuf *b, int regnum, int x, int y, int z, int w)
{
    wb_push(b, OP_DEFI | (5u << 24));
    wb_push(b, tok_dst(T_CONSTINT, regnum, 0xF, 0, 0));
    wb_push(b, (uint32_t)x); wb_push(b, (uint32_t)y);
    wb_push(b, (uint32_t)z); wb_push(b, (uint32_t)w);
} /* wb_defi */

/* Raw control-flow tokens (the shared IR only describes data instructions). */
static void wb_loop(WordBuf *b, int intreg)
{
    wb_push(b, OP_LOOP | (2u << 24));
    wb_push(b, tok_src(T_LOOP, 0, SW_XYZW, SRCMOD_NONE));
    wb_push(b, tok_src(T_CONSTINT, intreg, SW_XYZW, SRCMOD_NONE));
} /* wb_loop */

static void wb_endloop(WordBuf *b)
{
    /* ENDLOOP takes no operand tokens: instruction count field stays zero. */
    wb_push(b, OP_ENDLOOP);
} /* wb_endloop */

static void wb_label(WordBuf *b, int label)
{
    wb_push(b, OP_LABEL | (1u << 24));
    wb_push(b, tok_src(T_LABEL, label, SW_XYZW, SRCMOD_NONE));
} /* wb_label */

static void wb_call(WordBuf *b, int label)
{
    wb_push(b, OP_CALL | (1u << 24));
    wb_push(b, tok_src(T_LABEL, label, SW_XYZW, SRCMOD_NONE));
} /* wb_call */

static void wb_ret(WordBuf *b)
{
    wb_push(b, OP_RET);
} /* wb_ret */

/* A predicated opcode that is intentionally NOT in the supported set, built
   straight from words for the rejection tests. Sources are constants with a
   caller-chosen swizzle, so opcode-level validation passes and the predicate
   is what gets rejected. */
static void wb_predicated_raw(WordBuf *b, int opcode, int nsrc,
                              int dtype, int dnum, int dmask, uint32_t swz)
{
    const int following = 1 + nsrc + 1;
    wb_push(b, (uint32_t)opcode | ((uint32_t)following << 24) | (1u << 28));
    wb_push(b, tok_dst(dtype, dnum, dmask, 0, 0));
    int i;
    for (i = 0; i < nsrc; i++)
        wb_push(b, tok_src(T_CONST, i + 1, swz, SRCMOD_NONE));
    wb_push(b, tok_pred(0, 0, -1));
} /* wb_predicated_raw */

/* Predicated destination-less control opcode (IF/CALL/...), for verifying
   these are rejected rather than silently ignoring the predicate. */
static void wb_predicated_nodst(WordBuf *b, int opcode, int nsrc)
{
    const int following = nsrc + 1;
    wb_push(b, (uint32_t)opcode | ((uint32_t)following << 24) | (1u << 28));
    int i;
    for (i = 0; i < nsrc; i++)
        wb_push(b, tok_src(T_PREDICATE, 0, SW_REP(0), SRCMOD_NONE));
    wb_push(b, tok_pred(0, 0, -1));
} /* wb_predicated_nodst */

/* ----------------------------------------------------------------------- */
/* Shared scenario IR: one description feeds both the word builder and the  */
/* independent execution model, so bytes and expected results never drift.  */
/* ----------------------------------------------------------------------- */

typedef enum
{
    IR_MOV, IR_ADD, IR_SUB, IR_MUL, IR_MAD, IR_MIN, IR_MAX,
    IR_DP3, IR_DP4, IR_CMP, IR_SETP, IR_TEXLDL, IR_TEXLDD
} IROp;

typedef struct
{
    int type;       /* T_TEMP/T_INPUT/T_CONST/T_SAMPLER ... */
    int num;
    int swz[4];     /* source component selectors */
    int mod;        /* SRCMOD_* */
} IRSrc;

typedef struct
{
    IROp op;
    int dreg;       /* destination temp (r#) or predicate register for SETP */
    int dmask;      /* 0..15 */
    int saturate;
    IRSrc s[4];
    int nsrc;
    int control;    /* SETP comparison control */
    int pred;       /* predicate register number, or -1 */
    int pred_neg;
    int pred_comp;  /* -1 = full vector, else a replicated component */
} IRInst;

static IRSrc mk_src(int type, int num, int x, int y, int z, int w, int mod)
{
    IRSrc r;
    r.type = type; r.num = num; r.mod = mod;
    r.swz[0] = x; r.swz[1] = y; r.swz[2] = z; r.swz[3] = w;
    return r;
} /* mk_src */

static IRSrc mk_r(int num)
{
    return mk_src(T_TEMP, num, 0, 1, 2, 3, SRCMOD_NONE);
} /* mk_r */

static void ir_emit_words(const IRInst *in, WordBuf *b)
{
    const int predicated = (in->pred >= 0);
    const int following = 1 /*dst*/ + in->nsrc + (predicated ? 1 : 0);
    const int dsttype = (in->op == IR_SETP) ? T_PREDICATE : T_TEMP;
    uint32_t opcode;

    switch (in->op)
    {
        case IR_MOV: opcode = OP_MOV; break;
        case IR_ADD: opcode = OP_ADD; break;
        case IR_SUB: opcode = OP_SUB; break;
        case IR_MUL: opcode = OP_MUL; break;
        case IR_MAD: opcode = OP_MAD; break;
        case IR_MIN: opcode = OP_MIN; break;
        case IR_MAX: opcode = OP_MAX; break;
        case IR_DP3: opcode = OP_DP3; break;
        case IR_DP4: opcode = OP_DP4; break;
        case IR_CMP: opcode = OP_CMP; break;
        case IR_SETP: opcode = OP_SETP; break;
        case IR_TEXLDL: opcode = OP_TEXLDL; break;
        case IR_TEXLDD: opcode = OP_TEXLDD; break;
        default: opcode = 0; break;
    } /* switch */

    wb_push(b, opcode | ((uint32_t)in->control << 16) |
              ((uint32_t)following << 24) |
              (predicated ? (1u << 28) : 0u));
    wb_push(b, tok_dst(dsttype, in->dreg, in->dmask,
                       in->saturate ? 1 : 0, 0));

    int i;
    for (i = 0; i < in->nsrc; i++)
    {
        const IRSrc *sp = &in->s[i];
        wb_push(b, tok_src(sp->type, sp->num,
                           SWZ(sp->swz[0], sp->swz[1], sp->swz[2], sp->swz[3]),
                           sp->mod));
    } /* for */

    if (predicated)
        wb_push(b, tok_pred(in->pred, in->pred_neg, in->pred_comp));
} /* ir_emit_words */

/* ----------------------------------------------------------------------- */
/* Independent SM3 register execution model                                 */
/* ----------------------------------------------------------------------- */

typedef struct
{
    int sampler;
    float coord[2];
    float lod;
    float ddx[2];
    float ddy[2];
    int have_grads;
    float color[4];
} SampleRecord;

#define MAX_SAMPLES 64
#define MAX_REGS 32
#define MAX_CONST 256
#define MAX_INPUTS 8

typedef struct
{
    float r[MAX_REGS][4];
    float in[MAX_INPUTS][4];
    float c[MAX_CONST][4];
    int   p[4];
    SampleRecord samples[MAX_SAMPLES];
    int sample_count;
} Machine;

static float fracf(float v)
{
    return v - floorf(v);
} /* fracf */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
} /* clampf */

static int nearly(float a, float b)
{
    const float d = fabsf(a - b);
    return d <= 1e-5f || d <= 1e-5f * fabsf(b);
} /* nearly */

static void machine_read(Machine *m, const IRSrc *sp, float out[4])
{
    const float *base = NULL;
    if (sp->type == T_TEMP) base = m->r[sp->num];
    else if (sp->type == T_INPUT) base = m->in[sp->num];
    else if (sp->type == T_CONST) base = m->c[sp->num];

    int i;
    for (i = 0; i < 4; i++)
        out[i] = base ? base[sp->swz[i]] : 0.0f;

    if (sp->mod == SRCMOD_NEGATE)
        for (i = 0; i < 4; i++) out[i] = -out[i];
    else if (sp->mod == SRCMOD_ABS)
        for (i = 0; i < 4; i++) out[i] = fabsf(out[i]);
} /* machine_read */

/* Deterministic stand-in for a 2D texture fetch; records every argument. */
static void cpu_sample2D(Machine *m, int sampler, const float uv[2], float lod,
                         const float ddx[2], const float ddy[2], int grads,
                         float out[4])
{
    SampleRecord *s = &m->samples[m->sample_count++];
    s->sampler = sampler;
    s->coord[0] = uv[0]; s->coord[1] = uv[1];
    s->lod = lod;
    s->have_grads = grads;
    if (grads)
    {
        s->ddx[0] = ddx[0]; s->ddx[1] = ddx[1];
        s->ddy[0] = ddy[0]; s->ddy[1] = ddy[1];
    } /* if */
    out[0] = fracf(uv[0] * 0.37f + (float)sampler + lod);
    out[1] = fracf(uv[1] * 0.53f + (float)sampler);
    out[2] = fracf(uv[0] + uv[1] + lod);
    out[3] = clampf(uv[0] * 0.11f + lod, 0.0f, 1.0f);
    s->color[0] = out[0]; s->color[1] = out[1];
    s->color[2] = out[2]; s->color[3] = out[3];
} /* cpu_sample2D */

static int predicate_channel(const IRInst *in, const Machine *m, int channel)
{
    const int comp = (in->pred_comp < 0) ? channel : in->pred_comp;
    int v = m->p[comp];
    return in->pred_neg ? !v : v;
} /* predicate_channel */

static int compare_true(int control, float a, float b)
{
    switch (control)
    {
        case CMP_GT: return a > b;
        case CMP_EQ: return a == b;
        case CMP_GE: return a >= b;
        case CMP_LT: return a < b;
        case CMP_NE: return a != b;
        case CMP_LE: return a <= b;
    } /* switch */
    return 0;
} /* compare_true */

static void machine_exec(Machine *m, const IRInst *in)
{
    float s0[4], s1[4], s2[4], s3[4];
    float cand[4];
    int i;

    /* Every source (and the predicate) is sampled before any write, so an
       aliased destination can't feed a later channel of the same op. */
    if (in->nsrc > 0) machine_read(m, &in->s[0], s0);
    if (in->nsrc > 1) machine_read(m, &in->s[1], s1);
    if (in->nsrc > 2) machine_read(m, &in->s[2], s2);
    if (in->nsrc > 3) machine_read(m, &in->s[3], s3);

    if (in->op == IR_SETP)
    {
        for (i = 0; i < 4; i++)
        {
            if (!(in->dmask & (1 << i))) continue;  /* partial write keeps rest */
            m->p[i] = compare_true(in->control, s0[i], s1[i]);
        } /* for */
        return;
    } /* if */

    switch (in->op)
    {
        case IR_MOV:
            for (i = 0; i < 4; i++) cand[i] = s0[i];
            break;
        case IR_ADD:
            for (i = 0; i < 4; i++) cand[i] = s0[i] + s1[i];
            break;
        case IR_SUB:
            for (i = 0; i < 4; i++) cand[i] = s0[i] - s1[i];
            break;
        case IR_MUL:
            for (i = 0; i < 4; i++) cand[i] = s0[i] * s1[i];
            break;
        case IR_MAD:
            for (i = 0; i < 4; i++) cand[i] = s0[i] * s1[i] + s2[i];
            break;
        case IR_MIN:
            for (i = 0; i < 4; i++) cand[i] = fminf(s0[i], s1[i]);
            break;
        case IR_MAX:
            for (i = 0; i < 4; i++) cand[i] = fmaxf(s0[i], s1[i]);
            break;
        case IR_DP3:
        {
            float d = s0[0]*s1[0] + s0[1]*s1[1] + s0[2]*s1[2];
            for (i = 0; i < 4; i++) cand[i] = d;
            break;
        } /* case */
        case IR_DP4:
        {
            float d = s0[0]*s1[0] + s0[1]*s1[1] + s0[2]*s1[2] + s0[3]*s1[3];
            for (i = 0; i < 4; i++) cand[i] = d;
            break;
        } /* case */
        case IR_CMP:
            for (i = 0; i < 4; i++)
                cand[i] = (s0[i] >= 0.0f) ? s1[i] : s2[i];
            break;
        case IR_TEXLDL:
        {
            /* s0 holds xy coords and the lod in .w; s1 is the sampler. */
            float uv[2] = { s0[0], s0[1] };
            cpu_sample2D(m, in->s[1].num, uv, s0[3], NULL, NULL, 0, cand);
            break;
        } /* case */
        case IR_TEXLDD:
        {
            float uv[2] = { s0[0], s0[1] };
            cpu_sample2D(m, in->s[1].num, uv, 0.0f, s2, s3, 1, cand);
            break;
        } /* case */
        default:
            for (i = 0; i < 4; i++) cand[i] = 0.0f;
            break;
    } /* switch */

    for (i = 0; i < 4; i++)
    {
        if (!(in->dmask & (1 << i))) continue;            /* static mask */
        if (!predicate_channel(in, m, i)) continue;       /* predicate off */
        float v = cand[i];
        if (in->saturate) v = clampf(v, 0.0f, 1.0f);
        m->r[in->dreg][i] = v;
    } /* for */
} /* machine_exec */

/* ----------------------------------------------------------------------- */
/* MOJOSHADER_parse wrappers and structural GLSL checks                     */
/* ----------------------------------------------------------------------- */

static const MOJOSHADER_parseData *parse_words(const WordBuf *b,
                                               const char *profile)
{
    return MOJOSHADER_parse(profile, "main",
                            (const unsigned char *)b->w,
                            (unsigned)(b->n * sizeof (uint32_t)),
                            NULL, 0, NULL, 0, NULL, NULL, NULL);
} /* parse_words */

static int count_occurrences(const char *hay, const char *needle)
{
    int n = 0;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) { n++; p += strlen(needle); }
    return n;
} /* count_occurrences */

static int parse_ok(const WordBuf *b, const char *profile)
{
    const MOJOSHADER_parseData *pd = parse_words(b, profile);
    const int ok = (pd->error_count == 0) && (pd->output != NULL);
    if (!ok)
    {
        int i;
        for (i = 0; i < pd->error_count; i++)
            printf("  parse error: %s\n", pd->errors[i].error);
    } /* if */
    MOJOSHADER_freeParseData(pd);
    return ok;
} /* parse_ok */

static int parse_fails(const WordBuf *b, const char *profile)
{
    const MOJOSHADER_parseData *pd = parse_words(b, profile);
    const int failed = pd->error_count > 0;
    if (failed)
        printf("  expected error: %s\n", pd->errors[0].error);
    MOJOSHADER_freeParseData(pd);
    return failed;
} /* parse_fails */

/* ----------------------------------------------------------------------- */
/* Scenario programs                                                         */
/* ----------------------------------------------------------------------- */

typedef struct
{
    IRInst inst[256];
    int n;
} Program;

static IRInst *prog_add(Program *p, IROp op, int dreg, int dmask)
{
    IRInst *in = &p->inst[p->n++];
    memset(in, 0, sizeof (*in));
    in->op = op;
    in->dreg = dreg;
    in->dmask = dmask;
    in->pred = -1;
    in->pred_comp = -1;
    return in;
} /* prog_add */

static void in_src(IRInst *in, int idx, IRSrc s)
{
    in->s[idx] = s;
    in->nsrc = (idx + 1 > in->nsrc) ? idx + 1 : in->nsrc;
} /* in_src */

static void in_pred(IRInst *in, int reg, int negate, int component)
{
    in->pred = reg;
    in->pred_neg = negate;
    in->pred_comp = component;
} /* in_pred */

static void prog_emit_body(const Program *p, WordBuf *b)
{
    int i;
    for (i = 0; i < p->n; i++)
        ir_emit_words(&p->inst[i], b);
} /* prog_emit_body */

/* Frame a program whose source temps the machine model presets directly:
   each is materialized in the bytecode from its own constant so the parser's
   initialize-before-read rule is satisfied with identical values. */
static void prog_frame_inits(const Program *p, WordBuf *b, uint32_t version,
                             const TempInit *inits, int ninit)
{
    wb_push(b, version);
    wb_temp_init_defs(b, inits, ninit);   /* all DEFs before instructions */
    wb_temp_init_movs(b, inits, ninit);
    prog_emit_body(p, b);
    wb_push(b, EOS_TOKEN);
} /* prog_frame_inits */

static void check_vec4(const char *what, const float got[4],
                       const float want[4])
{
    int i;
    for (i = 0; i < 4; i++)
    {
        if (!nearly(got[i], want[i]))
        {
            g_assertions++;
            g_failures++;
            printf("  ASSERT FAILED: %s[%d] got %.6f want %.6f\n",
                   what, i, got[i], want[i]);
        } /* if */
        else
            g_assertions++;
    } /* for */
} /* check_vec4 */

static void check_pvec4(const char *what, const int got[4], const int want[4])
{
    int i;
    for (i = 0; i < 4; i++)
    {
        CHECK(got[i] == want[i]);
        if (got[i] != want[i])
            printf("    (%s[%d] got %d want %d)\n", what, i, got[i], want[i]);
    } /* for */
} /* check_pvec4 */

/* Structural verification of one predicated instruction's GLSL block. */
static void check_predicated_block(const char *out, int temp_index,
                                   int expected_commit_lines)
{
    char needle[64];
    snprintf(needle, sizeof (needle), "_mojoshader_predtmp%d", temp_index);
    /* one declaration ... */
    char decl[80];
    snprintf(decl, sizeof (decl), "vec4 %s;", needle);
    CHECK(count_occurrences(out, decl) == 1);
    /* ... one candidate store into it ... */
    char assign[80];
    snprintf(assign, sizeof (assign), "%s = ", needle);
    CHECK(count_occurrences(out, assign) == 1);
    /* ... and exactly one guarded commit per written channel. */
    char commit[96];
    snprintf(commit, sizeof (commit), "? %s.", needle);
    CHECK(count_occurrences(out, commit) == expected_commit_lines);
    /* vector predicate must never be collapsed to any()/all(). */
    CHECK(strstr(out, "any(") == NULL);
    CHECK(strstr(out, "all(") == NULL);
} /* check_predicated_block */

/* ----------------------------------------------------------------------- */
/* Individual scenarios                                                      */
/* ----------------------------------------------------------------------- */

/* 1. Mixed true/false vector predicate, full mask, plain MOV. */
static void case_mixed_vector_predicate(void)
{
    case_begin("mixed true/false vector predicate (MOV, full mask)");
    Program p; memset(&p, 0, sizeof (p));
    WordBuf full; memset(&full, 0, sizeof (full));

    Machine m; memset(&m, 0, sizeof (m));
    m.in[0][0] = 10; m.in[0][1] = 20; m.in[0][2] = 30; m.in[0][3] = 40;
    m.r[1][0] = 1; m.r[1][1] = 2; m.r[1][2] = 3; m.r[1][3] = 4;
    m.p[0] = 1; m.p[1] = 0; m.p[2] = 1; m.p[3] = 0;

    /* setp_ge p0, v0, v0 -- all true, then override p for the test below. */
    IRInst *setp = prog_add(&p, IR_SETP, 0, 0xF);
    in_src(setp, 0, mk_src(T_INPUT, 0, 0,1,2,3, SRCMOD_NONE));
    in_src(setp, 1, mk_src(T_INPUT, 0, 0,1,2,3, SRCMOD_NONE));
    setp->control = CMP_GE;

    /* (p0) mov r1, v0 : channels 1 and 3 must keep their old values. */
    IRInst *mv = prog_add(&p, IR_MOV, 1, 0xF);
    in_src(mv, 0, mk_src(T_INPUT, 0, 0,1,2,3, SRCMOD_NONE));
    in_pred(mv, 0, 0, -1);

    wb_push(&full, VERSION_VS3);
    wb_dcl_input(&full, 0, 5 /*TEXCOORD*/, 0, 0xF);
    prog_emit_body(&p, &full);
    wb_push(&full, EOS_TOKEN);

    /* Reference execution with the hand-picked p vector. */
    m.p[0]=1; m.p[1]=0; m.p[2]=1; m.p[3]=0;
    machine_exec(&m, &p.inst[1]);
    const float want_r1[4] = { 10, 2, 30, 4 };
    check_vec4("r1", m.r[1], want_r1);

    CHECK(parse_ok(&full, "glsl120"));
    const MOJOSHADER_parseData *pd = parse_words(&full, "glsl120");
    CHECK(pd->error_count == 0);
    check_predicated_block(pd->output, 0, 4);
    CHECK(strstr(pd->output, "vs_p0.x) ?") != NULL);
    CHECK(strstr(pd->output, "vs_p0.y) ?") != NULL);
    CHECK(strstr(pd->output, "vs_p0.z) ?") != NULL);
    CHECK(strstr(pd->output, "vs_p0.w) ?") != NULL);
    MOJOSHADER_freeParseData(pd);
} /* case_mixed_vector_predicate */

/* 2. Negated replicate predicate (!p0.x) applied to every channel. */
static void case_negated_replicate(void)
{
    case_begin("negated replicate predicate (!p0.x) on ADD");
    Program p; memset(&p, 0, sizeof (p));
    WordBuf b; memset(&b, 0, sizeof (b));

    Machine m; memset(&m, 0, sizeof (m));
    m.r[0][0]=1; m.r[0][1]=1; m.r[0][2]=1; m.r[0][3]=1;
    m.r[1][0]=5; m.r[1][1]=5; m.r[1][2]=5; m.r[1][3]=5;
    m.p[0]=1;  /* !p0.x is false everywhere: nothing may be written. */

    IRInst *add = prog_add(&p, IR_ADD, 1, 0xF);
    in_src(add, 0, mk_r(0));
    in_src(add, 1, mk_r(1));
    in_pred(add, 0, 1 /*negate*/, 0 /*replicate .x*/);

    const TempInit inits[] = {
        { 0, 10, {1,1,1,1} }, { 1, 11, {5,5,5,5} }
    };
    prog_frame_inits(&p, &b, VERSION_VS3, inits,
                     (int)(sizeof(inits)/sizeof(inits[0])));
    machine_exec(&m, add);
    const float want[4] = { 5,5,5,5 };
    check_vec4("r1 unchanged", m.r[1], want);

    CHECK(parse_ok(&b, "glsl120"));
    const MOJOSHADER_parseData *pd = parse_words(&b, "glsl120");
    check_predicated_block(pd->output, 0, 4);
    /* every guard references the same replicated, negated component. */
    CHECK(count_occurrences(pd->output, "(!vs_p0.x) ?") == 4);
    CHECK(strstr(pd->output, "(!vs_p0.y)") == NULL);
    MOJOSHADER_freeParseData(pd);
} /* case_negated_replicate */

/* 3. Sparse write mask intersects the predicate. */
static void case_sparse_mask(void)
{
    case_begin("sparse write mask (.xz) with vector predicate");
    Program p; memset(&p, 0, sizeof (p));
    WordBuf b; memset(&b, 0, sizeof (b));

    Machine m; memset(&m, 0, sizeof (m));
    m.r[2][0]=9; m.r[2][1]=9; m.r[2][2]=9; m.r[2][3]=9;
    m.r[0][0]=1; m.r[0][1]=1; m.r[0][2]=1; m.r[0][3]=1;
    m.p[0]=1; m.p[1]=1; m.p[2]=0; m.p[3]=1;

    /* (p0) mul r2.xz, r0, r0 -- only x/z eligible; z's predicate is off. */
    IRInst *mul = prog_add(&p, IR_MUL, 2, 0x5 /*xz*/);
    in_src(mul, 0, mk_r(0));
    in_src(mul, 1, mk_r(0));
    in_pred(mul, 0, 0, -1);

    const TempInit inits3[] = {
        { 0, 10, {1,1,1,1} }, { 2, 12, {9,9,9,9} }
    };
    prog_frame_inits(&p, &b, VERSION_VS3, inits3,
                     (int)(sizeof(inits3)/sizeof(inits3[0])));
    machine_exec(&m, mul);
    const float want[4] = { 1, 9, 9, 9 };  /* x written, z blocked, y/w kept */
    check_vec4("r2", m.r[2], want);

    const MOJOSHADER_parseData *pd = parse_words(&b, "glsl120");
    CHECK(pd->error_count == 0);
    check_predicated_block(pd->output, 0, 2);
    CHECK(strstr(pd->output, "vs_r2.x = (vs_p0.x)") != NULL);
    CHECK(strstr(pd->output, "vs_r2.z = (vs_p0.z)") != NULL);
    CHECK(strstr(pd->output, "vs_r2.y =") == NULL);
    CHECK(strstr(pd->output, "vs_r2.w =") == NULL);
    MOJOSHADER_freeParseData(pd);
} /* case_sparse_mask */

/* 4. SETP partial update leaves untouched predicate components intact. */
static void case_setp_partial(void)
{
    case_begin("SETP partial write preserves other boolean components");
    Program p; memset(&p, 0, sizeof (p));
    WordBuf full; memset(&full, 0, sizeof (full));

    Machine m; memset(&m, 0, sizeof (m));
    m.in[0][0]=1; m.in[0][1]=2; m.in[0][2]=3; m.in[0][3]=4;
    m.in[1][0]=9; m.in[1][1]=0; m.in[1][2]=9; m.in[1][3]=0;
    m.p[0]=1; m.p[1]=0; m.p[2]=1; m.p[3]=0;

    /* setp_lt p0.yz, v1, v0 : y -> (0<2)=1, z -> (9<3)=0; x,w unchanged. */
    IRInst *setp = prog_add(&p, IR_SETP, 0, 0x6 /*yz*/);
    in_src(setp, 0, mk_src(T_INPUT,1, 0,1,2,3, SRCMOD_NONE));
    in_src(setp, 1, mk_src(T_INPUT,0, 0,1,2,3, SRCMOD_NONE));
    setp->control = CMP_LT;

    wb_push(&full, VERSION_VS3);
    wb_dcl_input(&full, 0, 5, 0, 0xF);
    wb_dcl_input(&full, 1, 5, 1, 0xF);
    prog_emit_body(&p, &full);
    wb_push(&full, EOS_TOKEN);

    machine_exec(&m, setp);
    const int want_p[4] = { 1, 1, 0, 0 };
    check_pvec4("p0", m.p, want_p);

    const MOJOSHADER_parseData *pd = parse_words(&full, "glsl120");
    CHECK(pd->error_count == 0);
    CHECK(strstr(pd->output, "vs_p0.yz =") != NULL);
    CHECK(strstr(pd->output, "vs_p0.x =") == NULL);
    CHECK(strstr(pd->output, "vs_p0.w =") == NULL);
    MOJOSHADER_freeParseData(pd);
} /* case_setp_partial */

/* 5. Source/destination aliasing with a rearranged swizzle. */
static void case_aliasing(void)
{
    case_begin("source/destination aliasing with reordered swizzle");
    Program p; memset(&p, 0, sizeof (p));
    WordBuf b; memset(&b, 0, sizeof (b));

    Machine m; memset(&m, 0, sizeof (m));
    m.r[0][0]=1; m.r[0][1]=2; m.r[0][2]=3; m.r[0][3]=4;
    m.p[0]=1; m.p[1]=1; m.p[2]=1; m.p[3]=1;

    /* (p0) mov r0, r0.wzyx -- every channel is sourced before any commit. */
    IRInst *mv = prog_add(&p, IR_MOV, 0, 0xF);
    in_src(mv, 0, mk_src(T_TEMP,0, 3,2,1,0, SRCMOD_NONE));
    in_pred(mv, 0, 0, -1);

    const TempInit inits5[] = { { 0, 10, {1,2,3,4} } };
    prog_frame_inits(&p, &b, VERSION_VS3, inits5,
                     (int)(sizeof(inits5)/sizeof(inits5[0])));
    machine_exec(&m, mv);
    const float want[4] = { 4, 3, 2, 1 };
    check_vec4("r0 reversed", m.r[0], want);

    const MOJOSHADER_parseData *pd = parse_words(&b, "glsl120");
    CHECK(pd->error_count == 0);
    /* the temporary store precedes every destination commit. */
    const char *store = strstr(pd->output, "_mojoshader_predtmp0 = ");
    const char *commit = strstr(pd->output, "vs_r0.x = ");
    CHECK(store != NULL && commit != NULL && store < commit);
    MOJOSHADER_freeParseData(pd);
} /* case_aliasing */

/* 6. Saturate only affects channels that are actually committed. */
static void case_saturate(void)
{
    case_begin("saturate applied only on committed channels");
    Program p; memset(&p, 0, sizeof (p));
    WordBuf b; memset(&b, 0, sizeof (b));

    Machine m; memset(&m, 0, sizeof (m));
    m.r[0][0]=-2; m.r[0][1]=-2; m.r[0][2]=2; m.r[0][3]=2;
    m.r[3][0]=7; m.r[3][1]=7; m.r[3][2]=7; m.r[3][3]=7;
    m.p[0]=1; m.p[1]=0; m.p[2]=1; m.p[3]=0;

    /* (p0) mov_sat r3, r0 : x clamps to 0, z clamps to 1, y/w stay 7. */
    IRInst *mv = prog_add(&p, IR_MOV, 3, 0xF);
    in_src(mv, 0, mk_r(0));
    mv->saturate = 1;
    in_pred(mv, 0, 0, -1);

    const TempInit inits6[] = {
        { 0, 10, {-2,-2,2,2} }, { 3, 13, {7,7,7,7} }
    };
    prog_frame_inits(&p, &b, VERSION_VS3, inits6,
                     (int)(sizeof(inits6)/sizeof(inits6[0])));
    machine_exec(&m, mv);
    const float want[4] = { 0, 7, 1, 7 };
    check_vec4("r3", m.r[3], want);

    const MOJOSHADER_parseData *pd = parse_words(&b, "glsl120");
    CHECK(pd->error_count == 0);
    CHECK(count_occurrences(pd->output, "clamp(") == 4);
    MOJOSHADER_freeParseData(pd);
} /* case_saturate */

/* 7. Every supported arithmetic opcode, numeric results cross-checked. */
static void case_all_arithmetic(void)
{
    case_begin("all predicated arithmetic opcodes (ADD SUB MUL MAD MIN MAX DP3 DP4)");
    Program p; memset(&p, 0, sizeof (p));
    WordBuf b; memset(&b, 0, sizeof (b));
    Machine m; memset(&m, 0, sizeof (m));

    m.r[1][0]= 2; m.r[1][1]=-4; m.r[1][2]= 8; m.r[1][3]=16;
    m.r[2][0]= 3; m.r[2][1]= 2; m.r[2][2]=-1; m.r[2][3]= 2;
    m.r[3][0]=10; m.r[3][1]=20; m.r[3][2]=30; m.r[3][3]=40;
    m.r[5][0]=1; m.r[5][1]=1; m.r[5][2]=1; m.r[5][3]=1;  /* destination */
    m.p[0]=1; m.p[1]=0; m.p[2]=1; m.p[3]=0;

    IRInst *add = prog_add(&p, IR_ADD, 5, 0xF);
    in_src(add, 0, mk_r(1)); in_src(add, 1, mk_r(2)); in_pred(add,0,0,-1);
    IRInst *sub = prog_add(&p, IR_SUB, 5, 0xF);
    in_src(sub, 0, mk_r(1)); in_src(sub, 1, mk_r(2)); in_pred(sub,0,0,-1);
    IRInst *mul = prog_add(&p, IR_MUL, 5, 0xF);
    in_src(mul, 0, mk_r(1)); in_src(mul, 1, mk_r(2)); in_pred(mul,0,0,-1);
    IRInst *mad = prog_add(&p, IR_MAD, 5, 0xF);
    in_src(mad, 0, mk_r(1)); in_src(mad, 1, mk_r(2));
    in_src(mad, 2, mk_r(3)); in_pred(mad,0,0,-1);
    IRInst *mn = prog_add(&p, IR_MIN, 5, 0xF);
    in_src(mn, 0, mk_r(1)); in_src(mn, 1, mk_r(2)); in_pred(mn,0,0,-1);
    IRInst *mx = prog_add(&p, IR_MAX, 5, 0xF);
    in_src(mx, 0, mk_r(1)); in_src(mx, 1, mk_r(2)); in_pred(mx,0,0,-1);
    IRInst *dp3 = prog_add(&p, IR_DP3, 5, 0xF);
    in_src(dp3, 0, mk_r(1)); in_src(dp3, 1, mk_r(2)); in_pred(dp3,0,0,-1);
    IRInst *dp4 = prog_add(&p, IR_DP4, 5, 0xF);
    in_src(dp4, 0, mk_r(1)); in_src(dp4, 1, mk_r(2)); in_pred(dp4,0,0,-1);

    const TempInit inits7[] = {
        { 1, 10, {2,-4,8,16} },
        { 2, 11, {3,2,-1,2} },
        { 3, 12, {10,20,30,40} },
        { 5, 13, {1,1,1,1} }
    };
    prog_frame_inits(&p, &b, VERSION_VS3, inits7,
                     (int)(sizeof(inits7)/sizeof(inits7[0])));
    CHECK(parse_ok(&b, "glsl120"));

    const float init5[4] = {1,1,1,1};
    const struct { IRInst *in; float want[4]; } expect[] = {
        { add, { 5, 1, 7, 1} },                 /* y,w keep prior value */
        { sub, { -1, 1, 9, 1} },
        { mul, { 6, 1, -8, 1} },
        { mad, { 16, 1, 22, 1} },
        { mn,  { 2, 1, -1, 1} },
        { mx,  { 3, 1, 8, 1} },
        { dp3, { -10, 1, -10, 1} },            /* 2*3 + -4*2 + 8*-1 */
        { dp4, { 22, 1, 22, 1} },              /* ... + 16*2 */
    };

    int k;
    for (k = 0; k < (int)(sizeof(expect)/sizeof(expect[0])); k++)
    {
        m.r[5][0]=init5[0]; m.r[5][1]=init5[1];
        m.r[5][2]=init5[2]; m.r[5][3]=init5[3];
        machine_exec(&m, expect[k].in);
        check_vec4("arithmetic r5", m.r[5], expect[k].want);
    } /* for */

    const MOJOSHADER_parseData *pd = parse_words(&b, "glsl120");
    /* one temporary per predicated instruction, numbered in order. */
    CHECK(count_occurrences(pd->output, "_mojoshader_predtmp7;") == 1);
    CHECK(count_occurrences(pd->output, "vec4 _mojoshader_predtmp") == 8);
    MOJOSHADER_freeParseData(pd);
} /* case_all_arithmetic */

/* 8. Predicated CMP in a pixel shader. */
static void case_cmp_pixel(void)
{
    case_begin("predicated CMP (PS3 only)");
    Program p; memset(&p, 0, sizeof (p));
    WordBuf full; memset(&full, 0, sizeof (full));
    Machine m; memset(&m, 0, sizeof (m));

    /* c0 = test vector, c1 = >=0 branch, c2 = <0 branch. */
    m.c[0][0]= 1; m.c[0][1]=-1; m.c[0][2]= 0; m.c[0][3]=-5;
    m.c[1][0]=100; m.c[1][1]=100; m.c[1][2]=100; m.c[1][3]=100;
    m.c[2][0]=200; m.c[2][1]=200; m.c[2][2]=200; m.c[2][3]=200;
    m.r[4][0]=7; m.r[4][1]=7; m.r[4][2]=7; m.r[4][3]=7;
    m.p[0]=1; m.p[1]=0; m.p[2]=0; m.p[3]=1;

    IRInst *cmp = prog_add(&p, IR_CMP, 4, 0xF);
    in_src(cmp, 0, mk_src(T_CONST,0, 0,1,2,3, SRCMOD_NONE));
    in_src(cmp, 1, mk_src(T_CONST,1, 0,1,2,3, SRCMOD_NONE));
    in_src(cmp, 2, mk_src(T_CONST,2, 0,1,2,3, SRCMOD_NONE));
    in_pred(cmp, 0, 0, -1);

    wb_push(&full, VERSION_PS3);
    wb_def(&full, 0, 1,-1,0,-5);
    wb_def(&full, 1, 100,100,100,100);
    wb_def(&full, 2, 200,200,200,200);
    ir_emit_words(cmp, &full);
    wb_push(&full, EOS_TOKEN);

    machine_exec(&m, cmp);
    /* x: >=0 -> 100; y: predicate off -> stays 7; z: off -> 7; w: <0 -> 200 */
    const float want[4] = { 100, 7, 7, 200 };
    check_vec4("r4", m.r[4], want);

    const MOJOSHADER_parseData *pd = parse_words(&full, "glsl120");
    CHECK(pd->error_count == 0);
    check_predicated_block(pd->output, 0, 4);
    MOJOSHADER_freeParseData(pd);
} /* case_cmp_pixel */

/* 9. Predicated TEXLDL: one sample, lod forwarded, then channel commit. */
static void case_texldl(void)
{
    case_begin("predicated TEXLDL (VS3, 2D sampler, explicit LOD)");
    Program p; memset(&p, 0, sizeof (p));
    WordBuf b; memset(&b, 0, sizeof (b));
    Machine m; memset(&m, 0, sizeof (m));

    m.c[0][0]=0.25f; m.c[0][1]=0.75f; m.c[0][2]=0.0f; m.c[0][3]=3.0f; /* uv + lod */
    m.r[6][0]=9; m.r[6][1]=9; m.r[6][2]=9; m.r[6][3]=9;
    m.p[0]=1; m.p[1]=0; m.p[2]=1; m.p[3]=0;

    wb_dcl_sampler(&b, 0);
    IRInst *tx = prog_add(&p, IR_TEXLDL, 6, 0xF);
    in_src(tx, 0, mk_src(T_CONST,0, 0,1,2,3, SRCMOD_NONE));
    in_src(tx, 1, mk_src(T_SAMPLER,0, 0,1,2,3, SRCMOD_NONE));
    in_pred(tx, 0, 0, -1);

    WordBuf full; memset(&full, 0, sizeof (full));
    wb_push(&full, VERSION_VS3);
    wb_dcl_sampler(&full, 0);
    ir_emit_words(tx, &full);
    wb_push(&full, EOS_TOKEN);

    machine_exec(&m, tx);
    CHECK(m.sample_count == 1);  /* exactly one fetch for the whole vec4. */
    CHECK(m.samples[0].sampler == 0);
    CHECK(nearly(m.samples[0].coord[0], 0.25f));
    CHECK(nearly(m.samples[0].coord[1], 0.75f));
    CHECK(nearly(m.samples[0].lod, 3.0f));
    CHECK(m.samples[0].have_grads == 0);
    /* y and w were predicate-off: they keep 9, x and z take sample colors. */
    CHECK(nearly(m.r[6][0], m.samples[0].color[0]));
    CHECK(nearly(m.r[6][1], 9.0f));
    CHECK(nearly(m.r[6][2], m.samples[0].color[2]));
    CHECK(nearly(m.r[6][3], 9.0f));

    const MOJOSHADER_parseData *pd = parse_words(&full, "glsl120");
    CHECK(pd->error_count == 0);
    /* the single fetch is the one stored into the candidate temporary;
       preflight #define lines must not be counted as extra samples. */
    CHECK(strstr(pd->output, "_mojoshader_predtmp0 = texture2DLod(") != NULL);
    CHECK(count_occurrences(pd->output, " = texture2DLod(") == 1);
    check_predicated_block(pd->output, 0, 4);
    MOJOSHADER_freeParseData(pd);
} /* case_texldl */

/* 10. Predicated TEXLDD: explicit gradients forwarded, single fetch. */
static void case_texldd(void)
{
    case_begin("predicated TEXLDD (PS3, 2D sampler, explicit gradients)");
    Program p; memset(&p, 0, sizeof (p));
    WordBuf full; memset(&full, 0, sizeof (full));
    Machine m; memset(&m, 0, sizeof (m));

    m.c[0][0]=0.4f; m.c[0][1]=0.6f; m.c[0][2]=0; m.c[0][3]=0;     /* uv */
    m.c[1][0]=0.01f; m.c[1][1]=0.02f; m.c[1][2]=0; m.c[1][3]=0;   /* ddx */
    m.c[2][0]=0.03f; m.c[2][1]=0.04f; m.c[2][2]=0; m.c[2][3]=0;   /* ddy */
    m.r[7][0]=5; m.r[7][1]=5; m.r[7][2]=5; m.r[7][3]=5;
    m.p[0]=0; m.p[1]=1; m.p[2]=1; m.p[3]=0;

    IRInst *tx = prog_add(&p, IR_TEXLDD, 7, 0x7 /*xyz*/);
    in_src(tx, 0, mk_src(T_CONST,0, 0,1,2,3, SRCMOD_NONE));
    in_src(tx, 1, mk_src(T_SAMPLER,0, 0,1,2,3, SRCMOD_NONE));
    in_src(tx, 2, mk_src(T_CONST,1, 0,1,2,3, SRCMOD_NONE));
    in_src(tx, 3, mk_src(T_CONST,2, 0,1,2,3, SRCMOD_NONE));
    in_pred(tx, 0, 0, -1);

    wb_push(&full, VERSION_PS3);
    wb_dcl_sampler(&full, 0);
    ir_emit_words(tx, &full);
    wb_push(&full, EOS_TOKEN);

    machine_exec(&m, tx);
    CHECK(m.sample_count == 1);
    CHECK(m.samples[0].have_grads == 1);
    CHECK(nearly(m.samples[0].ddx[0], 0.01f));
    CHECK(nearly(m.samples[0].ddx[1], 0.02f));
    CHECK(nearly(m.samples[0].ddy[0], 0.03f));
    CHECK(nearly(m.samples[0].ddy[1], 0.04f));
    /* mask xyz + predicate: x off (stays 5), y/z written, w untouched (5). */
    CHECK(nearly(m.r[7][0], 5.0f));
    CHECK(nearly(m.r[7][1], m.samples[0].color[1]));
    CHECK(nearly(m.r[7][2], m.samples[0].color[2]));
    CHECK(nearly(m.r[7][3], 5.0f));

    const MOJOSHADER_parseData *pd = parse_words(&full, "glsl120");
    CHECK(pd->error_count == 0);
    CHECK(strstr(pd->output, "_mojoshader_predtmp0 = texture2DGrad(") != NULL);
    CHECK(count_occurrences(pd->output, " = texture2DGrad(") == 1);
    check_predicated_block(pd->output, 0, 3);  /* one commit per masked xyz */
    MOJOSHADER_freeParseData(pd);
} /* case_texldd */

/* 11. Predicate reused across loop iterations; temp name stays stable. */
static void case_loop_reuse(void)
{
    case_begin("predicated instruction reused inside a LOOP");
    WordBuf b; memset(&b, 0, sizeof (b));
    Machine m; memset(&m, 0, sizeof (m));

    m.in[0][0]=2; m.in[0][1]=2; m.in[0][2]=2; m.in[0][3]=2;
    m.c[0][0]=5; m.c[0][1]=5; m.c[0][2]=5; m.c[0][3]=5;   /* threshold */
    m.c[1][0]=1; m.c[1][1]=1; m.c[1][2]=1; m.c[1][3]=1;   /* increment */

    /* declarations and literal constants must precede any instruction. */
    wb_push(&b, VERSION_VS3);
    wb_dcl_input(&b, 0, 5, 0, 0xF);
    wb_defi(&b, 0, 0, 2, 1, 0);          /* i0: start 0, count 2, step 1 */
    wb_def(&b, 0, 5,5,5,5);              /* c0 threshold */
    wb_def(&b, 1, 1,1,1,1);              /* c1 increment */
    /* r0 = v0 ; r1 = 0 */
    wb_push(&b, OP_MOV | (2u << 24));
    wb_push(&b, tok_dst(T_TEMP, 0, 0xF, 0, 0));
    wb_push(&b, tok_src(T_INPUT, 0, SW_XYZW, SRCMOD_NONE));
    wb_push(&b, OP_MOV | (2u << 24));
    wb_push(&b, tok_dst(T_TEMP, 1, 0xF, 0, 0));
    wb_push(&b, tok_src(T_CONST, 1, SWZ(0,0,0,0), SRCMOD_NONE));
    wb_loop(&b, 0);
    /* setp_gt p0, r0, c0 */
    wb_push(&b, OP_SETP | (3u << 24));
    wb_push(&b, tok_dst(T_PREDICATE, 0, 0xF, 0, 0));
    wb_push(&b, tok_src(T_TEMP, 0, SW_XYZW, SRCMOD_NONE));
    wb_push(&b, tok_src(T_CONST, 0, SW_XYZW, SRCMOD_NONE));
    /* (p0) add r1, r1, c1 */
    wb_push(&b, OP_ADD | (4u << 24) | (1u << 28));
    wb_push(&b, tok_dst(T_TEMP, 1, 0xF, 0, 0));
    wb_push(&b, tok_src(T_TEMP, 1, SW_XYZW, SRCMOD_NONE));
    wb_push(&b, tok_src(T_CONST, 1, SW_XYZW, SRCMOD_NONE));
    wb_push(&b, tok_pred(0, 0, -1));
    wb_endloop(&b);
    wb_push(&b, EOS_TOKEN);

    /* Model the two iterations directly. */
    IRInst setp_m; memset(&setp_m, 0, sizeof (setp_m));
    setp_m.op = IR_SETP; setp_m.dreg = 0; setp_m.dmask = 0xF;
    setp_m.control = CMP_GT; setp_m.pred = -1; setp_m.pred_comp = -1;
    setp_m.s[0] = mk_r(0); setp_m.s[1] = mk_src(T_CONST,0,0,1,2,3,SRCMOD_NONE);
    setp_m.nsrc = 2;
    IRInst add_m; memset(&add_m, 0, sizeof (add_m));
    add_m.op = IR_ADD; add_m.dreg = 1; add_m.dmask = 0xF;
    add_m.s[0] = mk_r(1);
    add_m.s[1] = mk_src(T_CONST,1,0,1,2,3,SRCMOD_NONE);
    add_m.nsrc = 2; add_m.pred = 0; add_m.pred_comp = -1;

    int it;
    for (it = 0; it < 2; it++)
    {
        machine_exec(&m, &setp_m);   /* 2 > 5 is false: p0 all zero */
        machine_exec(&m, &add_m);    /* predicated add writes nothing */
    } /* for */
    const float zero[4] = {0,0,0,0};
    check_vec4("r1 stays zero", m.r[1], zero);

    const MOJOSHADER_parseData *pd = parse_words(&b, "glsl120");
    CHECK(pd->error_count == 0);
    CHECK(strstr(pd->output, "for (int aL") != NULL);
    /* a single static temporary, reused on every iteration. */
    CHECK(count_occurrences(pd->output, "vec4 _mojoshader_predtmp0;") == 1);
    MOJOSHADER_freeParseData(pd);
} /* case_loop_reuse */

/* 12. Predicate reused inside a CALLed helper. */
static void case_helper_reuse(void)
{
    case_begin("predicated instruction inside a CALLed subroutine");
    WordBuf b; memset(&b, 0, sizeof (b));
    Machine m; memset(&m, 0, sizeof (m));

    m.r[0][0]=1; m.r[0][1]=1; m.r[0][2]=1; m.r[0][3]=1;
    m.r[1][0]=2; m.r[1][1]=2; m.r[1][2]=2; m.r[1][3]=2;
    m.r[2][0]=0; m.r[2][1]=0; m.r[2][2]=0; m.r[2][3]=0;
    m.p[0]=1; m.p[1]=0; m.p[2]=1; m.p[3]=0;

    wb_push(&b, VERSION_VS3);
    const TempInit helper_inits[] = {
        { 0, 10, {1,1,1,1} }, { 1, 11, {2,2,2,2} }
    };
    wb_temp_init_defs(&b, helper_inits, 2);
    wb_temp_init_movs(&b, helper_inits, 2);
    wb_call(&b, 0);
    wb_ret(&b);
    wb_label(&b, 0);
    /* setp_ge p0, r0, r0 -> all true, then hand-set p for the model. */
    wb_push(&b, OP_SETP | (3u << 24));
    wb_push(&b, tok_dst(T_PREDICATE, 0, 0xF, 0, 0));
    wb_push(&b, tok_src(T_TEMP, 0, SW_XYZW, SRCMOD_NONE));
    wb_push(&b, tok_src(T_TEMP, 0, SW_XYZW, SRCMOD_NONE));
    /* (p0) mov r2, r1 */
    wb_push(&b, OP_MOV | (3u << 24) | (1u << 28));
    wb_push(&b, tok_dst(T_TEMP, 2, 0xF, 0, 0));
    wb_push(&b, tok_src(T_TEMP, 1, SW_XYZW, SRCMOD_NONE));
    wb_push(&b, tok_pred(0, 0, -1));
    wb_ret(&b);
    wb_push(&b, EOS_TOKEN);

    IRInst mv; memset(&mv, 0, sizeof (mv));
    mv.op = IR_MOV; mv.dreg = 2; mv.dmask = 0xF;
    mv.s[0] = mk_r(1); mv.nsrc = 1;
    mv.pred = 0; mv.pred_comp = -1;
    machine_exec(&m, &mv);
    const float want[4] = { 2, 0, 2, 0 };
    check_vec4("r2 via helper", m.r[2], want);

    const MOJOSHADER_parseData *pd = parse_words(&b, "glsl120");
    CHECK(pd->error_count == 0);
    CHECK(strstr(pd->output, "void vs_l0()") != NULL);
    CHECK(count_occurrences(pd->output, "vs_l0();") == 1);
    check_predicated_block(pd->output, 0, 4);
    MOJOSHADER_freeParseData(pd);
} /* case_helper_reuse */

/* 13. Repeated translation produces byte-identical, stable output. */
static void case_stable_output(void)
{
    case_begin("deterministic, stable temporary naming across translations");
    Program p; memset(&p, 0, sizeof (p));
    WordBuf b; memset(&b, 0, sizeof (b));

    IRInst *a = prog_add(&p, IR_MUL, 0, 0xF);
    in_src(a, 0, mk_r(1)); in_src(a, 1, mk_r(2)); in_pred(a,0,0,-1);
    IRInst *c = prog_add(&p, IR_MOV, 3, 0xF);
    in_src(c, 0, mk_r(1)); in_pred(c,0,1,2);  /* !p0.z replicated */
    const TempInit inits13[] = {
        { 1, 10, {1,1,1,1} }, { 2, 11, {2,2,2,2} }
    };
    prog_frame_inits(&p, &b, VERSION_VS3, inits13,
                     (int)(sizeof(inits13)/sizeof(inits13[0])));

    const MOJOSHADER_parseData *p1 = parse_words(&b, "glsl120");
    const MOJOSHADER_parseData *p2 = parse_words(&b, "glsl120");
    CHECK(p1->error_count == 0 && p2->error_count == 0);
    CHECK(strcmp(p1->output, p2->output) == 0);
    CHECK(strstr(p1->output, "_mojoshader_predtmp0;") != NULL);
    CHECK(strstr(p1->output, "_mojoshader_predtmp1;") != NULL);
    CHECK(count_occurrences(p1->output, "(!vs_p0.z) ?") == 4);
    MOJOSHADER_freeParseData(p1);
    MOJOSHADER_freeParseData(p2);
} /* case_stable_output */

/* 14. Rejection: unsupported predicated opcodes, control flow, bad inputs. */
static void case_rejections(void)
{
    case_begin("unsupported predicated forms and illegal predicates still fail");

    /* predicated RCP: legal opcode, but not enabled for predication. Its
       state check demands a replicate swizzle, so give it a legal shape. */
    WordBuf rcp; memset(&rcp, 0, sizeof (rcp));
    wb_push(&rcp, VERSION_VS3);
    wb_predicated_raw(&rcp, OP_RCP, 1, T_TEMP, 0, 0xF, SW_REP(0));
    wb_push(&rcp, EOS_TOKEN);
    CHECK(parse_fails(&rcp, "glsl120"));

    /* predicated MOVA: another data opcode outside the supported set. */
    WordBuf mova; memset(&mova, 0, sizeof (mova));
    wb_push(&mova, VERSION_VS3);
    wb_predicated_raw(&mova, OP_MOVA, 1, T_ADDRESS, 0, 0xF, SW_XYZW);
    wb_push(&mova, EOS_TOKEN);
    CHECK(parse_fails(&mova, "glsl120"));

    /* predicated IF: control flow must not silently swallow the predicate.
       Its own operand is a legal p# scalar, so only the extra predicate fails. */
    WordBuf ifb; memset(&ifb, 0, sizeof (ifb));
    wb_push(&ifb, VERSION_VS3);
    wb_predicated_nodst(&ifb, OP_IF, 1);
    wb_push(&ifb, EOS_TOKEN);
    CHECK(parse_fails(&ifb, "glsl120"));

    /* illegal predicate register (a constant where p# is required). Using a
       non-temp operand keeps the initialize-before-read rule out of it, so
       the predicate-register validation is what actually fails. */
    WordBuf badpred; memset(&badpred, 0, sizeof (badpred));
    wb_push(&badpred, VERSION_VS3);
    wb_push(&badpred, OP_MOV | (3u << 24) | (1u << 28));
    wb_push(&badpred, tok_dst(T_TEMP, 0, 0xF, 0, 0));
    wb_push(&badpred, tok_src(T_CONST, 1, SW_XYZW, SRCMOD_NONE));
    wb_push(&badpred, tok_src(T_CONST, 2, SW_XYZW, SRCMOD_NONE)); /* not p# */
    wb_push(&badpred, EOS_TOKEN);
    CHECK(parse_fails(&badpred, "glsl120"));

    /* same predicated MOV on the plain glsl (1.10) profile: still rejected. */
    WordBuf ok; memset(&ok, 0, sizeof (ok));
    wb_push(&ok, VERSION_VS3);
    wb_predicated_raw(&ok, OP_MOV, 1, T_TEMP, 0, 0xF, SW_XYZW);
    wb_push(&ok, EOS_TOKEN);
    CHECK(parse_fails(&ok, "glsl"));

    /* predication is a Shader Model 3 feature; vs_2_0 must reject it. */
    WordBuf sm2; memset(&sm2, 0, sizeof (sm2));
    wb_push(&sm2, 0xFFFE0200u);
    wb_predicated_raw(&sm2, OP_MOV, 1, T_TEMP, 0, 0xF, SW_XYZW);
    wb_push(&sm2, EOS_TOKEN);
    CHECK(parse_fails(&sm2, "glsl120"));

    /* stage restrictions are untouched: TEXLDD is pixel-shader only. */
    WordBuf texldd_vs; memset(&texldd_vs, 0, sizeof (texldd_vs));
    wb_push(&texldd_vs, VERSION_VS3);
    wb_dcl_sampler(&texldd_vs, 0);
    wb_push(&texldd_vs, OP_TEXLDD | (6u << 24) | (1u << 28));
    wb_push(&texldd_vs, tok_dst(T_TEMP, 0, 0xF, 0, 0));
    wb_push(&texldd_vs, tok_src(T_TEMP, 1, SW_XYZW, SRCMOD_NONE));
    wb_push(&texldd_vs, tok_src(T_SAMPLER, 0, SW_XYZW, SRCMOD_NONE));
    wb_push(&texldd_vs, tok_src(T_TEMP, 2, SW_XYZW, SRCMOD_NONE));
    wb_push(&texldd_vs, tok_src(T_TEMP, 3, SW_XYZW, SRCMOD_NONE));
    wb_push(&texldd_vs, tok_pred(0, 0, -1));
    wb_push(&texldd_vs, EOS_TOKEN);
    CHECK(parse_fails(&texldd_vs, "glsl120"));

    /* the D3D profile still emits its native predicate syntax, unchanged. */
    const MOJOSHADER_parseData *d3d = parse_words(&ok, "d3d");
    CHECK(d3d->error_count == 0);
    CHECK(strstr(d3d->output, "(p0)") != NULL);
    MOJOSHADER_freeParseData(d3d);
} /* case_rejections */

/* 15. Plain, non-predicated output stays byte-identical to the baseline. */
static void case_plain_unchanged(void)
{
    case_begin("non-predicated GLSL output is unchanged");
    /* The exact plain fixture from checks/baseline.c, so the comparison is
       against the same words the existing baseline translates. */
    const uint32_t words[] = {
        0xfffe0300,
        0x0200001f, 0x80000000, 0x900f0000,   /* dcl input v0      */
        0x0200001f, 0x80000000, 0xe00f0000,   /* dcl output o0     */
        0x02000001, 0xe00f0000, 0x90e40000,   /* mov o0, v0        */
        0x0000ffff
    };
    WordBuf b; memset(&b, 0, sizeof (b));
    size_t i;
    for (i = 0; i < sizeof(words)/sizeof(words[0]); i++)
        wb_push(&b, words[i]);

    /* The exact historical output produced by checks/baseline.sh. */
    const char *golden =
        "#version 120\n"
        "attribute vec4 vs_v0;\n"
        "#define vs_o0 gl_Position\n"
        "\n"
        "void main()\n"
        "{\n"
        "\tvs_o0 = vs_v0;\n"
        "}\n"
        "\n";

    const MOJOSHADER_parseData *pd = parse_words(&b, "glsl120");
    CHECK(pd->error_count == 0);
    CHECK(strstr(pd->output, "_mojoshader_predtmp") == NULL);
    CHECK(strcmp(pd->output, golden) == 0);
    MOJOSHADER_freeParseData(pd);
} /* case_plain_unchanged */

/* 16. Predicated write to a genuinely scalar destination (pixel depth
       output) must not emit a component suffix on the scalar GLSL variable. */
static void case_scalar_destination(void)
{
    case_begin("predicated MOV into scalar depth output (PS3)");
    WordBuf b; memset(&b, 0, sizeof (b));

    enum { T_DEPTHOUT = 9 };
    wb_push(&b, VERSION_PS3);
    wb_def(&b, 0, 1,1,1,1);
    /* setp_ge p0, c0, c0 (all true); control 2 == greater-than-or-equal. */
    wb_push(&b, OP_SETP | (2u << 16) | (3u << 24));
    wb_push(&b, tok_dst(T_PREDICATE, 0, 0xF, 0, 0));
    wb_push(&b, tok_src(T_CONST, 0, SW_XYZW, SRCMOD_NONE));
    wb_push(&b, tok_src(T_CONST, 0, SW_XYZW, SRCMOD_NONE));
    /* (p0.x) mov oDepth, c0.x -- replicated predicate, scalar destination. */
    wb_push(&b, OP_MOV | (3u << 24) | (1u << 28));
    wb_push(&b, tok_dst(T_DEPTHOUT, 0, 0x1, 0, 0));
    wb_push(&b, tok_src(T_CONST, 0, SW_REP(0), SRCMOD_NONE));
    wb_push(&b, tok_pred(0, 0, 0));
    wb_push(&b, EOS_TOKEN);

    const MOJOSHADER_parseData *pd = parse_words(&b, "glsl120");
    if (pd->error_count)
        printf("  depth parse error: %s\n", pd->errors[0].error);
    CHECK(pd->error_count == 0);
    /* scalar guard and assignment: a component suffix on the depth
       destination would be invalid GLSL. */
    CHECK(strstr(pd->output, "(ps_p0.x) ?") != NULL);
    CHECK(strstr(pd->output, ".x = (ps_p0.x) ?") == NULL);
    MOJOSHADER_freeParseData(pd);
} /* case_scalar_destination */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    case_mixed_vector_predicate();
    case_negated_replicate();
    case_sparse_mask();
    case_setp_partial();
    case_aliasing();
    case_saturate();
    case_all_arithmetic();
    case_cmp_pixel();
    case_texldl();
    case_texldd();
    case_loop_reuse();
    case_helper_reuse();
    case_stable_output();
    case_rejections();
    case_plain_unchanged();
    case_scalar_destination();

    printf("\ncases=%d assertions=%d failures=%d\n",
           g_cases, g_assertions, g_failures);
    return g_failures ? 1 : 0;
} /* main */


