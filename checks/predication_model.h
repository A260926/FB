/*
 * predication_model.h
 *
 * Independent, CPU-only reference model for the predicated SM3 translation
 *  tests. This knows nothing about GLSL: it interprets the same D3D9 token
 *  fixtures the parser receives, applying source swizzles/modifiers, the
 *  static write mask, saturation and the four-component predicate, and
 *  records every texture fetch through a deterministic stub. The translated
 *  GLSL is then checked against the numeric state and call log produced here.
 *
 * No GPU, no external assembler, no extra libraries: every shader "word" is
 *  built by the small encoders at the bottom of this header.
 */

#ifndef PREDICATION_MODEL_H
#define PREDICATION_MODEL_H

#include <stdint.h>
#include <stddef.h>
#include <math.h>

/* ----- D3D9 register type numbers (see mojoshader_internal.h) ------------- */
#define MS_REG_TEMP      0
#define MS_REG_INPUT     1
#define MS_REG_CONST     2
#define MS_REG_OUTPUT    6
#define MS_REG_CONSTINT  7
#define MS_REG_COLOROUT  8
#define MS_REG_SAMPLER   10
#define MS_REG_LOOP      15
#define MS_REG_LABEL     18
#define MS_REG_PRED      19

/* ----- source modifiers --------------------------------------------------- */
#define MS_MOD_NONE   0
#define MS_MOD_NEG    1
#define MS_MOD_ABS    11
#define MS_MOD_NOT    13

/* ----- opcodes (table indices) -------------------------------------------- */
#define MS_OP_NOP      0
#define MS_OP_MOV      1
#define MS_OP_ADD      2
#define MS_OP_SUB      3
#define MS_OP_MAD      4
#define MS_OP_MUL      5
#define MS_OP_DP3      8
#define MS_OP_DP4      9
#define MS_OP_MIN      10
#define MS_OP_MAX      11
#define MS_OP_LOOP     27
#define MS_OP_RET      28
#define MS_OP_ENDLOOP  29
#define MS_OP_LABEL    30
#define MS_OP_DCL      31
#define MS_OP_IF       40
#define MS_OP_ENDIF    43
#define MS_OP_DEFI     48
#define MS_OP_TEXLD    66
#define MS_OP_CMP      88
#define MS_OP_TEXLDD   93
#define MS_OP_SETP     94
#define MS_OP_TEXLDL   95
#define MS_OP_END      0xffff

#define MS_MAX_REGS 16

typedef struct
{
    float x, y, z, w;
} ms_vec4;

typedef struct
{
    int x, y, z, w;
} ms_bvec4;

/* One recorded texture invocation, so tests can check more than strings. */
typedef struct
{
    int sampler;
    float coord[4];
    float lod;
    float gradx[4];
    float grady[4];
    ms_vec4 color;
    int calls;
} ms_texture_log;

typedef struct
{
    ms_vec4 r[MS_MAX_REGS];       /* temporaries (also inputs/outputs). */
    ms_vec4 c[MS_MAX_REGS];       /* float constants. */
    ms_bvec4 p;                   /* predicate register p0. */
    int const_int[4];             /* i0 used as the LOOP counter block. */
    ms_texture_log tex[MS_MAX_REGS];
    int texture_calls;
} ms_state;

static ms_vec4 ms_v4(float x, float y, float z, float w)
{
    const ms_vec4 v = { x, y, z, w };
    return v;
}

static int ms_chan(const int swizzle, const int component)
{
    return (swizzle >> (component * 2)) & 0x3;
}

static float ms_read_src(ms_state *st, int regtype, int regnum,
                         int swizzle, int mod, int component)
{
    ms_vec4 v;
    if (regtype == MS_REG_TEMP || regtype == MS_REG_INPUT ||
        regtype == MS_REG_OUTPUT || regtype == MS_REG_COLOROUT)
        v = st->r[regnum];
    else if (regtype == MS_REG_CONST)
        v = st->c[regnum];
    else
        v = ms_v4(0.0f, 0.0f, 0.0f, 0.0f);

    const float raw[4] = { v.x, v.y, v.z, v.w };
    float value = raw[ms_chan(swizzle, component)];
    if (mod == MS_MOD_NEG) value = -value;
    else if (mod == MS_MOD_ABS) value = fabsf(value);
    return value;
}

static int ms_read_pred(const ms_state *st, int swizzle, int mod,
                        int component)
{
    const int raw[4] = { st->p.x, st->p.y, st->p.z, st->p.w };
    /* No swizzle selects the matching lane; a replicate swizzle broadcasts. */
    const int lane = (swizzle == 0xE4) ? component : (swizzle & 0x3);
    int value = raw[lane];
    if (mod == MS_MOD_NOT) value = !value;
    return value;
}

static float ms_sat(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Deterministic stand-in for a 2D texture: every channel is a pure function
 *  of sampler, coordinate and lod, so expected colors can be asserted. */
static ms_vec4 ms_sample_2d(ms_state *st, int sampler, const float coord[4],
                            float lod)
{
    ms_texture_log *log = &st->tex[sampler];
    log->sampler = sampler;
    log->coord[0] = coord[0];
    log->coord[1] = coord[1];
    log->coord[2] = coord[2];
    log->coord[3] = coord[3];
    log->lod = lod;
    log->calls++;
    st->texture_calls++;
    const ms_vec4 color = ms_v4(
        0.10f * (float)(sampler + 1) + coord[0],
        0.20f * (float)(sampler + 1) + coord[1],
        0.30f * (float)(sampler + 1) + lod,
        0.40f * (float)(sampler + 1) + coord[0] + coord[1]);
    log->color = color;
    return color;
}

/* Explicit-gradient stand-in for TEXLDD: records both gradient groups and the
 *  coordinate, counts the call, and returns a deterministic color. */
static ms_vec4 ms_sample_grad_2d(ms_state *st, int sampler,
                                 const float coord[4],
                                 const float gradx[4],
                                 const float grady[4])
{
    ms_texture_log *log = &st->tex[sampler];
    log->sampler = sampler;
    for (int k = 0; k < 4; k++)
    {
        log->coord[k] = coord[k];
        log->gradx[k] = gradx[k];
        log->grady[k] = grady[k];
    } // for
    log->lod = -1.0f;  /* sentinel: an explicit-gradient fetch has no LOD. */
    log->calls++;
    st->texture_calls++;
    const ms_vec4 color = ms_v4(
        0.11f * (float)(sampler + 1) + coord[0],
        0.22f * (float)(sampler + 1) + coord[1],
        0.33f * (float)(sampler + 1) + gradx[0],
        0.44f * (float)(sampler + 1) + grady[1]);
    log->color = color;
    return color;
}

static float ms_apply_op(int op, int component, float s0, float s1, float s2)
{
    (void) component;
    switch (op)
    {
        case MS_OP_MOV: return s0;
        case MS_OP_ADD: return s0 + s1;
        case MS_OP_SUB: return s0 - s1;
        case MS_OP_MUL: return s0 * s1;
        case MS_OP_MAD: return (s0 * s1) + s2;
        case MS_OP_MIN: return fminf(s0, s1);
        case MS_OP_MAX: return fmaxf(s0, s1);
        case MS_OP_CMP: return (s0 >= 0.0f) ? s1 : s2;
        default: return 0.0f;
    } // switch
}

/* ----- token word encoders (little-endian D3D9 dwords) -------------------- */

static uint32_t ms_reg_hi(int regtype)
{
    return (1u << 31) |
           ((uint32_t)(regtype & 0x7) << 28) |
           ((uint32_t)((regtype >> 3) & 0x3) << 11);
}

static uint32_t ms_dst(int regtype, int regnum, int mask, int saturate)
{
    return ms_reg_hi(regtype) | (uint32_t)regnum |
           ((uint32_t)mask << 16) | (saturate ? 0x00100000u : 0u);
}

static uint32_t ms_src(int regtype, int regnum, int swizzle, int mod)
{
    return ms_reg_hi(regtype) | (uint32_t)regnum |
           ((uint32_t)swizzle << 16) | ((uint32_t)mod << 24);
}

static uint32_t ms_inst(int op, int following, int predicated, int controls)
{
    return (uint32_t)op | ((uint32_t)controls << 16) |
           ((uint32_t)following << 24) |
           (predicated ? 0x10000000u : 0u);
}

static int ms_replicate(int component)
{
    return component | (component << 2) | (component << 4) |
           (component << 6);
}

/* DCL placeholder dwords; the high bits carry the sampler texture type. */
#define MS_DCL_INPUT    0x80000000u
#define MS_DCL_SAMPLER2D 0x90000000u
#define MS_DCL_SAMPLERVOL 0xA0000000u

#endif /* PREDICATION_MODEL_H */
