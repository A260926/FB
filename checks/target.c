/*
 * Target tests for comparison (depth-reference) texture sampling.
 *
 * Everything below drives the compiler only through its public tsCompile API
 * and walks the emitted SPIR-V with a small, self-contained module reader.
 * No shader SDK and no GPU texture filtering is required.
 */
#include "task_support/check.h"

#include "tinyshader.h"
#include "spirv.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Minimal SPIR-V module reader                                        */
/* ------------------------------------------------------------------ */

typedef struct SpirvModule
{
    const uint32_t *words;
    size_t word_count;
    uint32_t id_bound;

    size_t *inst_offset;   /* instruction index -> word offset          */
    uint32_t *inst_opcode; /* instruction index -> opcode               */
    size_t inst_count;

    size_t *id_inst; /* result id -> instruction index, or -1         */

    uint32_t entry_function_id;

    /* OpFunctionParameter result id -> owning function result id. */
    uint32_t *param_owner;
    /* OpFunctionParameter result id -> ordinal within its function. */
    uint32_t *param_index;
    /* Function-scope pointer id -> value stored by the last OpStore. */
    uint32_t *stored_value;
    /* User-function parameter id -> argument supplied by the entry wrapper. */
    uint32_t *entry_arg_for_param;
} SpirvModule;

static const uint32_t *inst_words(const SpirvModule *m, size_t idx)
{
    return &m->words[m->inst_offset[idx]];
}

static uint32_t inst_len(const SpirvModule *m, size_t idx)
{
    return m->words[m->inst_offset[idx]] >> 16;
}

static size_t find_def(const SpirvModule *m, uint32_t id)
{
    if (id == 0 || id >= m->id_bound) return (size_t)-1;
    return m->id_inst[id];
}

/* Returns the word index of the instruction's result id, or 0. */
static uint32_t result_id_word(uint32_t opcode)
{
    switch (opcode)
    {
    /* Types declare the result id as their first operand. */
    case SpvOpTypeVoid:
    case SpvOpTypeBool:
    case SpvOpTypeFloat:
    case SpvOpTypeInt:
    case SpvOpTypeSampler:
    case SpvOpTypeImage:
    case SpvOpTypeSampledImage:
    case SpvOpTypeVector:
    case SpvOpTypePointer:
    case SpvOpTypeArray:
    case SpvOpTypeRuntimeArray:
    case SpvOpTypeStruct:
    case SpvOpTypeFunction:
        return 1;

    /* Value-producing instructions: result type then result id. */
    case SpvOpConstant:
    case SpvOpConstantComposite:
    case SpvOpVariable:
    case SpvOpLoad:
    case SpvOpAccessChain:
    case SpvOpSampledImage:
    case SpvOpCompositeExtract:
    case SpvOpCompositeConstruct:
    case SpvOpFunction:
    case SpvOpFunctionParameter:
    case SpvOpImageSampleImplicitLod:
    case SpvOpImageSampleExplicitLod:
    case SpvOpImageSampleDrefImplicitLod:
    case SpvOpImageSampleDrefExplicitLod:
    case SpvOpFNegate:
    case SpvOpFAdd:
    case SpvOpFSub:
    case SpvOpFMul:
    case SpvOpConvertSToF:
    case SpvOpConvertUToF:
        return 2;

    default: return 0;
    }
}

static int spirv_load(SpirvModule *m, const unsigned char *raw, size_t bytes)
{
    memset(m, 0, sizeof(*m));

    if (!raw || bytes < 20 || (bytes % 4) != 0) return 0;

    m->words = (const uint32_t *)raw;
    m->word_count = bytes / 4;

    if (m->words[0] != SpvMagicNumber) return 0;

    m->id_bound = m->words[3];
    if (m->id_bound == 0) return 0;

    m->id_inst = (size_t *)malloc(sizeof(size_t) * m->id_bound);
    if (!m->id_inst) return 0;
    for (uint32_t i = 0; i < m->id_bound; ++i) m->id_inst[i] = (size_t)-1;

    size_t cap = 32;
    m->inst_offset = (size_t *)malloc(sizeof(size_t) * cap);
    m->inst_opcode = (uint32_t *)malloc(sizeof(uint32_t) * cap);
    m->inst_count = 0;

    size_t at = 5;
    while (at < m->word_count)
    {
        uint32_t first = m->words[at];
        uint32_t len = first >> 16;
        uint32_t op = first & 0xffff;
        if (len == 0 || at + len > m->word_count) return 0;

        if (m->inst_count == cap)
        {
            cap *= 2;
            m->inst_offset = (size_t *)realloc(m->inst_offset, sizeof(size_t) * cap);
            m->inst_opcode =
                (uint32_t *)realloc(m->inst_opcode, sizeof(uint32_t) * cap);
        }

        size_t idx = m->inst_count++;
        m->inst_offset[idx] = at;
        m->inst_opcode[idx] = op;

        uint32_t rid_word = result_id_word(op);
        if (rid_word && rid_word < len)
        {
            uint32_t rid = m->words[at + rid_word];
            if (rid < m->id_bound) m->id_inst[rid] = idx;
        }

        at += len;
    }

    /* Second pass: entry point, function parameter ownership and the value
     * most recently stored into each function-scope pointer. */
    m->param_owner = (uint32_t *)malloc(sizeof(uint32_t) * m->id_bound);
    m->param_index = (uint32_t *)malloc(sizeof(uint32_t) * m->id_bound);
    m->stored_value = (uint32_t *)malloc(sizeof(uint32_t) * m->id_bound);
    m->entry_arg_for_param = (uint32_t *)malloc(sizeof(uint32_t) * m->id_bound);
    if (!m->param_owner || !m->param_index || !m->stored_value ||
        !m->entry_arg_for_param)
    {
        return 0;
    }
    for (uint32_t i = 0; i < m->id_bound; ++i)
    {
        m->param_owner[i] = 0;
        m->param_index[i] = 0;
        m->stored_value[i] = 0;
        m->entry_arg_for_param[i] = 0;
    }

    uint32_t current_function = 0;
    uint32_t param_ordinal = 0;
    for (size_t i = 0; i < m->inst_count; ++i)
    {
        uint32_t op = m->inst_opcode[i];
        const uint32_t *w = inst_words(m, i);

        if (op == SpvOpEntryPoint)
        {
            m->entry_function_id = w[2];
        }
        else if (op == SpvOpFunction)
        {
            current_function = w[2];
            param_ordinal = 0;
        }
        else if (op == SpvOpFunctionEnd)
        {
            current_function = 0;
        }
        else if (op == SpvOpFunctionParameter)
        {
            if (w[2] < m->id_bound)
            {
                m->param_owner[w[2]] = current_function;
                m->param_index[w[2]] = param_ordinal++;
            }
        }
        else if (op == SpvOpStore)
        {
            if (w[1] < m->id_bound) m->stored_value[w[1]] = w[2];
        }
    }

    /* Map each user-function parameter onto the argument the entry wrapper
     * passes at the matching ordinal. */
    for (size_t i = 0; i < m->inst_count; ++i)
    {
        if (m->inst_opcode[i] != SpvOpFunctionCall) continue;
        const uint32_t *w = inst_words(m, i);
        uint32_t callee = w[3];
        uint32_t arg_count = inst_len(m, i) - 4;
        for (uint32_t ord = 0; ord < arg_count; ++ord)
        {
            for (uint32_t id = 1; id < m->id_bound; ++id)
            {
                if (m->param_owner[id] == callee && m->param_index[id] == ord)
                {
                    m->entry_arg_for_param[id] = w[4 + ord];
                    break;
                }
            }
        }
    }

    return 1;
}

static void spirv_free(SpirvModule *m)
{
    free(m->inst_offset);
    free(m->inst_opcode);
    free(m->id_inst);
    free(m->param_owner);
    free(m->param_index);
    free(m->stored_value);
    free(m->entry_arg_for_param);
    memset(m, 0, sizeof(*m));
}

static size_t find_first_opcode(const SpirvModule *m, uint32_t opcode)
{
    for (size_t i = 0; i < m->inst_count; ++i)
    {
        if (m->inst_opcode[i] == opcode) return i;
    }
    return (size_t)-1;
}

static unsigned count_opcode(const SpirvModule *m, uint32_t opcode)
{
    unsigned n = 0;
    for (size_t i = 0; i < m->inst_count; ++i)
    {
        if (m->inst_opcode[i] == opcode) ++n;
    }
    return n;
}

/* Resolve an id to the words of the instruction that defines it. */
static const uint32_t *def_words(const SpirvModule *m, uint32_t id, size_t *idx_out)
{
    size_t idx = find_def(m, id);
    if (idx == (size_t)-1) return NULL;
    if (idx_out) *idx_out = idx;
    return inst_words(m, idx);
}

static int decoration_value(
    const SpirvModule *m, uint32_t target, uint32_t decoration, uint32_t *value_out)
{
    for (size_t i = 0; i < m->inst_count; ++i)
    {
        if (m->inst_opcode[i] != SpvOpDecorate) continue;
        const uint32_t *w = inst_words(m, i);
        uint32_t len = inst_len(m, i);
        if (len >= 4 && w[1] == target && w[2] == decoration)
        {
            *value_out = w[3];
            return 1;
        }
    }
    return 0;
}

/* Follow an OpLoad/OpAccessChain pointer back to its originating OpVariable. */
static size_t resolve_variable(const SpirvModule *m, uint32_t pointer_id)
{
    size_t idx;
    const uint32_t *w = def_words(m, pointer_id, &idx);
    if (!w) return (size_t)-1;

    uint32_t op = m->inst_opcode[idx];
    if (op == SpvOpVariable) return idx;
    if (op == SpvOpAccessChain) return resolve_variable(m, w[3]);
    return (size_t)-1;
}

typedef struct ImageInfo
{
    uint32_t variable_id;
    uint32_t image_type_id;
    uint32_t dim;
    uint32_t depth;
    uint32_t binding;
    uint32_t set;
    int has_binding;
} ImageInfo;

/* Resolve an SSA image id (an OpLoad result) to its descriptor identity. */
static int resolve_image(const SpirvModule *m, uint32_t image_id, ImageInfo *out)
{
    size_t load_idx;
    const uint32_t *load = def_words(m, image_id, &load_idx);
    if (!load || m->inst_opcode[load_idx] != SpvOpLoad) return 0;

    size_t var_idx = resolve_variable(m, load[3]);
    if (var_idx == (size_t)-1) return 0;

    const uint32_t *var = inst_words(m, var_idx);
    uint32_t var_id = var[2];
    uint32_t ptr_type_id = var[1];

    size_t ptr_idx;
    const uint32_t *ptr_type = def_words(m, ptr_type_id, &ptr_idx);
    if (!ptr_type || m->inst_opcode[ptr_idx] != SpvOpTypePointer) return 0;

    uint32_t image_type_id = ptr_type[3];
    size_t img_idx;
    const uint32_t *img = def_words(m, image_type_id, &img_idx);
    if (!img || m->inst_opcode[img_idx] != SpvOpTypeImage) return 0;

    memset(out, 0, sizeof(*out));
    out->variable_id = var_id;
    out->image_type_id = image_type_id;
    out->dim = img[3];
    out->depth = img[4];
    out->has_binding = decoration_value(m, var_id, SpvDecorationBinding, &out->binding) &&
                       decoration_value(m, var_id, SpvDecorationDescriptorSet, &out->set);
    return 1;
}

/* The sampler SSA id must resolve to an OpTypeSampler-backed variable. */
static int resolve_sampler(const SpirvModule *m, uint32_t sampler_id)
{
    size_t load_idx;
    const uint32_t *load = def_words(m, sampler_id, &load_idx);
    if (!load || m->inst_opcode[load_idx] != SpvOpLoad) return 0;

    size_t var_idx = resolve_variable(m, load[3]);
    if (var_idx == (size_t)-1) return 0;

    const uint32_t *var = inst_words(m, var_idx);
    size_t ptr_idx;
    const uint32_t *ptr_type = def_words(m, var[1], &ptr_idx);
    if (!ptr_type || m->inst_opcode[ptr_idx] != SpvOpTypePointer) return 0;

    size_t samp_idx;
    const uint32_t *samp = def_words(m, ptr_type[3], &samp_idx);
    return samp && m->inst_opcode[samp_idx] == SpvOpTypeSampler;
}

/* Coordinate dimension: scalar float -> 1, otherwise OpTypeVector count.
 * The argument is a coordinate *value* id, so its result type is resolved
 * first. */
static int coordinate_dimension(const SpirvModule *m, uint32_t coord_id, int *dim_out)
{
    size_t val_idx;
    const uint32_t *val = def_words(m, coord_id, &val_idx);
    if (!val) return 0;

    uint32_t rid_word = result_id_word(m->inst_opcode[val_idx]);
    if (rid_word < 2) return 0; /* value instructions carry a result type */

    uint32_t type_id = val[1];
    size_t idx;
    const uint32_t *ty = def_words(m, type_id, &idx);
    if (!ty) return 0;

    uint32_t op = m->inst_opcode[idx];
    if (op == SpvOpTypeFloat)
    {
        *dim_out = 1;
        return 1;
    }
    if (op == SpvOpTypeVector)
    {
        *dim_out = (int)ty[3];
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Compilation helpers                                                 */
/* ------------------------------------------------------------------ */

typedef struct CompileResult
{
    TsCompilerOptions *options;
    TsCompilerOutput *output;
    SpirvModule spirv;
    int has_module;
    const char *error;
} CompileResult;

static int compile_hlsl(const char *src, TsShaderStage stage, CompileResult *cr)
{
    memset(cr, 0, sizeof(*cr));

    cr->options = tsCompilerOptionsCreate();
    tsCompilerOptionsSetStage(cr->options, stage);
    tsCompilerOptionsSetEntryPoint(cr->options, "main", 4);
    tsCompilerOptionsSetSource(cr->options, src, strlen(src), "target.hlsl", 11);

    cr->output = tsCompile(cr->options);
    cr->error = tsCompilerOutputGetErrors(cr->output);

    size_t bytes = 0;
    const unsigned char *raw = tsCompilerOutputGetSpirv(cr->output, &bytes);

    if (cr->error)
    {
        cr->has_module = 0;
        return 0;
    }

    cr->has_module = spirv_load(&cr->spirv, raw, bytes);
    return cr->has_module;
}

static void compile_destroy(CompileResult *cr)
{
    if (cr->has_module) spirv_free(&cr->spirv);
    tsCompilerOutputDestroy(cr->output);
    tsCompilerOptionsDestroy(cr->options);
}

static int has_source_location(const char *error)
{
    const char *marker = strstr(error, ": error: ");
    if (!marker) return 0;

    /* Walk back over the column number, its colon and the line number. */
    const char *p = marker;
    if (p == error || !(*(p - 1) >= '0' && *(p - 1) <= '9')) return 0;
    while (p > error && *(p - 1) >= '0' && *(p - 1) <= '9') --p;
    if (p == error || *(p - 1) != ':') return 0;
    --p;
    if (p == error || !(*(p - 1) >= '0' && *(p - 1) <= '9')) return 0;
    while (p > error && *(p - 1) >= '0' && *(p - 1) <= '9') --p;
    return 1;
}

static int expect_error(const char *src, TsShaderStage stage, const char *needle)
{
    CompileResult cr;
    int ok_compile = compile_hlsl(src, stage, &cr);

    int ok = 1;
    ok &= !ok_compile;
    ok &= cr.error != NULL;
    /* Diagnostics carry a source location (line:col), with or without a
     * resolvable file path. */
    ok &= cr.error != NULL && has_source_location(cr.error);
    if (needle) ok &= cr.error != NULL && strstr(cr.error, needle) != NULL;
    /* A failed compile must never emit a partial module. */
    ok &= !cr.has_module;

    compile_destroy(&cr);
    return ok;
}

/* Validate the image -> OpSampledImage -> sampler -> Dref data flow for one
 * depth sampling instruction. */
static int validate_dref_chain(
    const SpirvModule *m,
    size_t dref_idx,
    int expected_coord_dim,
    ImageInfo *image_out,
    uint32_t *dref_id_out,
    int explicit_lod)
{
    const uint32_t *w = inst_words(m, dref_idx);
    uint32_t op = m->inst_opcode[dref_idx];

    if (explicit_lod)
    {
        if (op != SpvOpImageSampleDrefExplicitLod) return 0;
        if (inst_len(m, dref_idx) < 8) return 0;
    }
    else
    {
        if (op != SpvOpImageSampleDrefImplicitLod) return 0;
        if (inst_len(m, dref_idx) != 6) return 0;
    }

    uint32_t result_type = w[1];
    uint32_t sampled_image_id = w[3];
    uint32_t coord_id = w[4];
    uint32_t dref_id = w[5];

    /* Result type is always a 32-bit scalar float. */
    size_t rt_idx;
    const uint32_t *rt = def_words(m, result_type, &rt_idx);
    if (!rt || m->inst_opcode[rt_idx] != SpvOpTypeFloat || rt[2] != 32) return 0;

    /* The reference is an independent operand, distinct from the coordinate. */
    if (dref_id == 0 || dref_id == coord_id) return 0;

    /* OpSampledImage combines the image and sampler exactly once. */
    size_t si_idx;
    const uint32_t *si = def_words(m, sampled_image_id, &si_idx);
    if (!si || m->inst_opcode[si_idx] != SpvOpSampledImage) return 0;
    uint32_t image_id = si[3];
    uint32_t sampler_id = si[4];

    ImageInfo info;
    if (!resolve_image(m, image_id, &info)) return 0;
    if (info.depth != 1) return 0; /* depth image required for Dref */
    if (!resolve_sampler(m, sampler_id)) return 0;

    int dim = 0;
    if (!coordinate_dimension(m, coord_id, &dim)) return 0;
    if (dim != expected_coord_dim) return 0;

    if (explicit_lod)
    {
        /* Image operands must carry Lod with a floating-point zero. */
        if (w[6] != SpvImageOperandsLodMask) return 0;
        uint32_t lod_id = w[7];
        size_t lod_idx;
        const uint32_t *lod = def_words(m, lod_id, &lod_idx);
        if (!lod || m->inst_opcode[lod_idx] != SpvOpConstant) return 0;
        if (lod[3] != 0u) return 0; /* bit pattern of float 0.0 */
        /* The LOD must never be aliased onto the reference. */
        if (lod_id == dref_id) return 0;
    }

    if (image_out) *image_out = info;
    if (dref_id_out) *dref_id_out = dref_id;
    return 1;
}

/* ------------------------------------------------------------------ */
/* typing group: frontend type distinctions and located diagnostics    */
/* ------------------------------------------------------------------ */

static int typing_sampler_kinds_distinct(void)
{
    /* Comparison sampler where an ordinary one is required. */
    TASK_CHECK(expect_error(
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0):SV_Target { return t.Sample(s, uv); }",
        TS_SHADER_STAGE_FRAGMENT,
        "SamplerState"));

    /* Ordinary sampler where a comparison one is required. */
    TASK_CHECK(expect_error(
        "Texture2D<float> t; SamplerState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        "SamplerComparisonState"));

    /* SampleLevel also rejects a comparison sampler. */
    TASK_CHECK(expect_error(
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0):SV_Target "
        "{ return t.SampleLevel(s, uv, 0.0); }",
        TS_SHADER_STAGE_VERTEX,
        "SamplerState"));
    return 0;
}

static int typing_coordinate_dimensions(void)
{
    /* float3 into a 2D coordinate: vectors must not be silently truncated. */
    TASK_CHECK(expect_error(
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float3 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        "float2"));

    /* float2 into a 1D scalar coordinate is a vector/scalar mismatch. */
    TASK_CHECK(expect_error(
        "Texture1D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        "float"));

    /* float2 into a cube float3 coordinate is a vector-size mismatch. */
    TASK_CHECK(expect_error(
        "TextureCube<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        "float3"));

    /* Scalar-to-vector coordinate broadcast remains a legal conversion. */
    CompileResult cr;
    TASK_CHECK(compile_hlsl(
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmpLevelZero(s, uv, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        &cr));
    TASK_CHECK(count_opcode(&cr.spirv, SpvOpImageSampleDrefExplicitLod) == 1);
    compile_destroy(&cr);
    return 0;
}

static int typing_reference_is_scalar_float(void)
{
    /* A vector reference cannot be narrowed to a scalar Dref silently. */
    TASK_CHECK(expect_error(
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0):SV_Target "
        "{ return t.SampleCmp(s, uv, uv); }",
        TS_SHADER_STAGE_FRAGMENT,
        "float"));

    /* Scalar integer literals still coerce to float. */
    CompileResult cr;
    TASK_CHECK(compile_hlsl(
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0):SV_Target "
        "{ return t.SampleCmp(s, uv, 1); }",
        TS_SHADER_STAGE_FRAGMENT,
        &cr));
    TASK_CHECK(count_opcode(&cr.spirv, SpvOpImageSampleDrefImplicitLod) == 1);
    compile_destroy(&cr);
    return 0;
}

static int typing_argument_counts(void)
{
    TASK_CHECK(expect_error(
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0):SV_Target "
        "{ return t.SampleCmp(s, uv); }",
        TS_SHADER_STAGE_FRAGMENT,
        "wrong amount of parameters"));

    TASK_CHECK(expect_error(
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0):SV_Target "
        "{ return t.SampleCmpLevelZero(s, uv); }",
        TS_SHADER_STAGE_FRAGMENT,
        "wrong amount of parameters"));

    TASK_CHECK(expect_error(
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r, uv); }",
        TS_SHADER_STAGE_FRAGMENT,
        "wrong amount of parameters"));
    return 0;
}

static int typing_resource_restrictions(void)
{
    /* Integer textures are outside the depth-comparison subset. */
    TASK_CHECK(expect_error(
        "Texture2D<int> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        "scalar float depth texture"));

    /* Multi-component textures are outside the subset. */
    TASK_CHECK(expect_error(
        "Texture2D<float4> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        "scalar float depth texture"));

    /* 3D textures are not extended. */
    TASK_CHECK(expect_error(
        "Texture3D<float> t; SamplerComparisonState s; "
        "float main(float3 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        "3D textures"));

    /* A comparison method on a non-image resource is an invalid method call. */
    TASK_CHECK(expect_error(
        "SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0):SV_Target "
        "{ return s.SampleCmp(uv, 0.5); }",
        TS_SHADER_STAGE_FRAGMENT,
        "invalid method call"));
    return 0;
}

static int typing_stage_restrictions(void)
{
    const char *cmp_src =
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r); }";
    const char *lz_src =
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmpLevelZero(s, uv, r); }";

    /* Implicit-LOD depth sampling is fragment-only. */
    TASK_CHECK(expect_error(cmp_src, TS_SHADER_STAGE_VERTEX, "fragment shaders"));

    const char *compute_cmp =
        "Texture2D<float> t; SamplerComparisonState s; "
        "[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) "
        "{ float x = t.SampleCmp(s, float2(0.0, 0.0), 0.5); }";
    TASK_CHECK(expect_error(compute_cmp, TS_SHADER_STAGE_COMPUTE, "fragment shaders"));

    /* Explicit zero LOD is accepted in all three stages. */
    CompileResult frag, vert, comp;
    TASK_CHECK(compile_hlsl(lz_src, TS_SHADER_STAGE_FRAGMENT, &frag));
    TASK_CHECK(count_opcode(&frag.spirv, SpvOpImageSampleDrefExplicitLod) == 1);
    compile_destroy(&frag);

    TASK_CHECK(compile_hlsl(lz_src, TS_SHADER_STAGE_VERTEX, &vert));
    TASK_CHECK(count_opcode(&vert.spirv, SpvOpImageSampleDrefExplicitLod) == 1);
    TASK_CHECK(count_opcode(&vert.spirv, SpvOpImageSampleDrefImplicitLod) == 0);
    compile_destroy(&vert);

    const char *comp_lz =
        "Texture2D<float> t; SamplerComparisonState s; "
        "[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) "
        "{ float x = t.SampleCmpLevelZero(s, float2(0.0, 0.0), 0.5); }";
    TASK_CHECK(compile_hlsl(comp_lz, TS_SHADER_STAGE_COMPUTE, &comp));
    TASK_CHECK(count_opcode(&comp.spirv, SpvOpImageSampleDrefExplicitLod) == 1);
    compile_destroy(&comp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* lowering group: independent SPIR-V data-flow verification           */
/* ------------------------------------------------------------------ */

static int lowering_implicit_dref_fragment(void)
{
    CompileResult cr;
    TASK_CHECK(compile_hlsl(
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        &cr));

    const SpirvModule *m = &cr.spirv;
    TASK_CHECK(count_opcode(m, SpvOpImageSampleDrefImplicitLod) == 1);
    TASK_CHECK(count_opcode(m, SpvOpImageSampleDrefExplicitLod) == 0);

    size_t idx = find_first_opcode(m, SpvOpImageSampleDrefImplicitLod);
    ImageInfo info;
    uint32_t dref_id = 0;
    TASK_CHECK(validate_dref_chain(m, idx, 2, &info, &dref_id, 0));
    TASK_CHECK(info.depth == 1);
    TASK_CHECK(info.dim == SpvDim2D);
    TASK_CHECK(dref_id != 0);

    compile_destroy(&cr);
    return 0;
}

static int lowering_explicit_zero_lod(void)
{
    CompileResult cr;
    TASK_CHECK(compile_hlsl(
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmpLevelZero(s, uv, r); }",
        TS_SHADER_STAGE_VERTEX,
        &cr));

    const SpirvModule *m = &cr.spirv;
    size_t idx = find_first_opcode(m, SpvOpImageSampleDrefExplicitLod);
    ImageInfo info;
    uint32_t dref_id = 0;
    TASK_CHECK(validate_dref_chain(m, idx, 2, &info, &dref_id, 1));

    /* The explicit Lod operand is a separate constant; reference is untouched. */
    const uint32_t *w = inst_words(m, idx);
    TASK_CHECK(w[6] == SpvImageOperandsLodMask);
    TASK_CHECK(w[7] != dref_id);

    compile_destroy(&cr);
    return 0;
}

static int lowering_dimensions_1d_and_cube(void)
{
    CompileResult one_d, cube;

    TASK_CHECK(compile_hlsl(
        "Texture1D<float> t; SamplerComparisonState s; "
        "float main(float u:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, u, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        &one_d));
    size_t idx1 = find_first_opcode(&one_d.spirv, SpvOpImageSampleDrefImplicitLod);
    ImageInfo info1;
    TASK_CHECK(validate_dref_chain(&one_d.spirv, idx1, 1, &info1, NULL, 0));
    TASK_CHECK(info1.dim == SpvDim1D);
    compile_destroy(&one_d);

    TASK_CHECK(compile_hlsl(
        "TextureCube<float> t; SamplerComparisonState s; "
        "float main(float3 d:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, d, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        &cube));
    size_t idxc = find_first_opcode(&cube.spirv, SpvOpImageSampleDrefImplicitLod);
    ImageInfo infoc;
    TASK_CHECK(validate_dref_chain(&cube.spirv, idxc, 3, &infoc, NULL, 0));
    TASK_CHECK(infoc.dim == SpvDimCube);
    compile_destroy(&cube);
    return 0;
}

/* Asymmetric coordinates paired with distinct references. */
static int lowering_asymmetric_coords_and_refs(void)
{
    CompileResult cr;
    TASK_CHECK(compile_hlsl(
        "Texture2D<float> t2; TextureCube<float> tc; "
        "SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float3 dir:TEXCOORD1,"
        "float r1:TEXCOORD2,float r2:TEXCOORD3):SV_Target "
        "{ return t2.SampleCmp(s, uv, r1) + tc.SampleCmp(s, dir, r2); }",
        TS_SHADER_STAGE_FRAGMENT,
        &cr));

    const SpirvModule *m = &cr.spirv;
    TASK_CHECK(count_opcode(m, SpvOpImageSampleDrefImplicitLod) == 2);

    unsigned seen_dim[4] = {0, 0, 0, 0};
    uint32_t dref_ids[2] = {0, 0};
    unsigned found = 0;

    for (size_t i = 0; i < m->inst_count; ++i)
    {
        if (m->inst_opcode[i] != SpvOpImageSampleDrefImplicitLod) continue;

        const uint32_t *w = inst_words(m, i);
        int dim = 0;
        TASK_CHECK(coordinate_dimension(m, w[4], &dim));
        TASK_CHECK(dim >= 1 && dim <= 3);
        seen_dim[dim]++;

        ImageInfo info;
        TASK_CHECK(validate_dref_chain(m, i, dim, &info, &dref_ids[found], 0));
        ++found;
    }

    TASK_CHECK(found == 2);
    TASK_CHECK(seen_dim[2] == 1);
    TASK_CHECK(seen_dim[3] == 1);
    /* The two references are distinct SSA values. */
    TASK_CHECK(dref_ids[0] != 0 && dref_ids[1] != 0);
    TASK_CHECK(dref_ids[0] != dref_ids[1]);

    compile_destroy(&cr);
    return 0;
}

/* One texture shared by ordinary and comparison sampling keeps one identity. */
static int lowering_mixed_sampling_single_descriptor(void)
{
    CompileResult cr;
    TASK_CHECK(compile_hlsl(
        "Texture2D<float> t; SamplerState ordinary; SamplerComparisonState cmp; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.Sample(ordinary, uv) + t.SampleCmp(cmp, uv, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        &cr));

    const SpirvModule *m = &cr.spirv;
    TASK_CHECK(count_opcode(m, SpvOpImageSampleImplicitLod) == 1);
    TASK_CHECK(count_opcode(m, SpvOpImageSampleDrefImplicitLod) == 1);

    /* Exactly one image variable/descriptor is emitted for the shared texture. */
    unsigned image_variables = 0;
    uint32_t image_var_id = 0;
    for (size_t i = 0; i < m->inst_count; ++i)
    {
        if (m->inst_opcode[i] != SpvOpVariable) continue;
        const uint32_t *var = inst_words(m, i);
        if (var[3] != SpvStorageClassUniformConstant) continue;

        size_t ptr_idx;
        const uint32_t *ptr = def_words(m, var[1], &ptr_idx);
        if (ptr && m->inst_opcode[ptr_idx] == SpvOpTypePointer)
        {
            size_t sub_idx;
            const uint32_t *sub = def_words(m, ptr[3], &sub_idx);
            if (sub && m->inst_opcode[sub_idx] == SpvOpTypeImage)
            {
                ++image_variables;
                image_var_id = var[2];
            }
        }
    }
    TASK_CHECK(image_variables == 1);
    TASK_CHECK(image_var_id != 0);

    /* Both sampling paths ultimately name that same descriptor. */
    size_t dref_idx = find_first_opcode(m, SpvOpImageSampleDrefImplicitLod);
    ImageInfo dref_image;
    TASK_CHECK(validate_dref_chain(m, dref_idx, 2, &dref_image, NULL, 0));
    TASK_CHECK(dref_image.variable_id == image_var_id);

    size_t ordinary_idx = find_first_opcode(m, SpvOpImageSampleImplicitLod);
    const uint32_t *ow = inst_words(m, ordinary_idx);
    size_t si_idx;
    const uint32_t *si = def_words(m, ow[3], &si_idx);
    TASK_CHECK(si && m->inst_opcode[si_idx] == SpvOpSampledImage);
    ImageInfo ordinary_image;
    TASK_CHECK(resolve_image(m, si[3], &ordinary_image));
    TASK_CHECK(ordinary_image.variable_id == image_var_id);

    /* Ordinary scalar sampling: vec4 result first, then an extraction. */
    size_t ordinary_type_idx;
    const uint32_t *ordinary_type = def_words(m, ow[1], &ordinary_type_idx);
    TASK_CHECK(ordinary_type && m->inst_opcode[ordinary_type_idx] == SpvOpTypeVector);
    TASK_CHECK(ordinary_type[3] == 4);
    TASK_CHECK(count_opcode(m, SpvOpCompositeExtract) >= 1);

    /* Comparison sampling stays scalar. */
    const uint32_t *dw = inst_words(m, dref_idx);
    size_t dref_type_idx;
    const uint32_t *dref_type = def_words(m, dw[1], &dref_type_idx);
    TASK_CHECK(dref_type && m->inst_opcode[dref_type_idx] == SpvOpTypeFloat);

    compile_destroy(&cr);
    return 0;
}

/* Explicit binding/set survive comparison lowering without cloning. */
static int lowering_binding_preserved(void)
{
    CompileResult cr;
    TASK_CHECK(compile_hlsl(
        "[[vk::binding(7, 3)]] Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        &cr));

    const SpirvModule *m = &cr.spirv;
    size_t idx = find_first_opcode(m, SpvOpImageSampleDrefImplicitLod);
    ImageInfo info;
    TASK_CHECK(validate_dref_chain(m, idx, 2, &info, NULL, 0));
    TASK_CHECK(info.has_binding);
    TASK_CHECK(info.binding == 7);
    TASK_CHECK(info.set == 3);

    compile_destroy(&cr);
    return 0;
}

/* Sampler and texture passed through a non-recursive helper keep their types. */
static int lowering_helper_parameters(void)
{
    CompileResult cr;
    TASK_CHECK(compile_hlsl(
        "Texture2D<float> t; SamplerComparisonState s; "
        "float probe(Texture2D<float> tex, SamplerComparisonState samp, "
        "float2 uv, float ref_value) "
        "{ return tex.SampleCmp(samp, uv, ref_value); } "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return probe(t, s, uv, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        &cr));

    const SpirvModule *m = &cr.spirv;
    size_t idx = find_first_opcode(m, SpvOpImageSampleDrefImplicitLod);
    ImageInfo info;
    TASK_CHECK(validate_dref_chain(m, idx, 2, &info, NULL, 0));
    TASK_CHECK(info.depth == 1);

    /* The helper receives a depth-image parameter and a sampler parameter. */
    int found_image_param = 0;
    int found_sampler_param = 0;
    for (size_t i = 0; i < m->inst_count; ++i)
    {
        if (m->inst_opcode[i] != SpvOpFunctionParameter) continue;
        const uint32_t *fp = inst_words(m, i);
        size_t ty_idx;
        const uint32_t *ty = def_words(m, fp[1], &ty_idx);
        if (!ty) continue;

        if (m->inst_opcode[ty_idx] == SpvOpTypeImage && ty[4] == 1)
        {
            found_image_param = 1;
        }
        if (m->inst_opcode[ty_idx] == SpvOpTypeSampler)
        {
            found_sampler_param = 1;
        }
    }
    TASK_CHECK(found_image_param);
    TASK_CHECK(found_sampler_param);

    /* The original global descriptor survives with one binding decoration. */
    int global_image_count = 0;
    uint32_t global_image_var = 0;
    for (size_t i = 0; i < m->inst_count; ++i)
    {
        if (m->inst_opcode[i] != SpvOpVariable) continue;
        const uint32_t *var = inst_words(m, i);
        if (var[3] != SpvStorageClassUniformConstant) continue;

        size_t ptr_idx;
        const uint32_t *ptr = def_words(m, var[1], &ptr_idx);
        if (!ptr || m->inst_opcode[ptr_idx] != SpvOpTypePointer) continue;

        size_t sub_idx;
        const uint32_t *sub = def_words(m, ptr[3], &sub_idx);
        if (sub && m->inst_opcode[sub_idx] == SpvOpTypeImage && sub[4] == 1)
        {
            ++global_image_count;
            global_image_var = var[2];
        }
    }
    TASK_CHECK(global_image_count == 1);
    uint32_t binding = 0;
    TASK_CHECK(decoration_value(m, global_image_var, SpvDecorationBinding, &binding));

    compile_destroy(&cr);
    return 0;
}

/* Ordinary scalar-texture sampling emits a legal vec4 sample then narrows. */
static int lowering_scalar_ordinary_sample_valid(void)
{
    CompileResult cr;
    TASK_CHECK(compile_hlsl(
        "Texture2D<float> t; SamplerState s; "
        "float main(float2 uv:TEXCOORD0):SV_Target { return t.Sample(s, uv); }",
        TS_SHADER_STAGE_FRAGMENT,
        &cr));

    const SpirvModule *m = &cr.spirv;
    size_t idx = find_first_opcode(m, SpvOpImageSampleImplicitLod);
    TASK_CHECK(idx != (size_t)-1);

    const uint32_t *w = inst_words(m, idx);
    size_t ty_idx;
    const uint32_t *ty = def_words(m, w[1], &ty_idx);
    TASK_CHECK(ty && m->inst_opcode[ty_idx] == SpvOpTypeVector && ty[3] == 4);

    /* The vec4 sample result feeds a CompositeExtract to produce the scalar. */
    uint32_t sample_result = w[2];
    int extracted = 0;
    for (size_t i = 0; i < m->inst_count; ++i)
    {
        if (m->inst_opcode[i] != SpvOpCompositeExtract) continue;
        const uint32_t *ce = inst_words(m, i);
        if (ce[3] == sample_result) extracted = 1;
    }
    TASK_CHECK(extracted);
    TASK_CHECK(count_opcode(m, SpvOpImageSampleDrefImplicitLod) == 0);

    compile_destroy(&cr);
    return 0;
}

/* Declaration order must not change the generated module's meaning. Bindings
 * and SSA ids may shift, but the instruction stream and depth classification
 * must be identical. */
static int lowering_declaration_order_stable(void)
{
    const char *a =
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r); }";
    const char *b =
        "SamplerComparisonState s; Texture2D<float> t; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r); }";

    CompileResult cra, crb;
    TASK_CHECK(compile_hlsl(a, TS_SHADER_STAGE_FRAGMENT, &cra));
    TASK_CHECK(compile_hlsl(b, TS_SHADER_STAGE_FRAGMENT, &crb));

    const SpirvModule *ma = &cra.spirv;
    const SpirvModule *mb = &crb.spirv;

    /* The same instruction set is emitted regardless of declaration order;
     * only type/global declaration ordering (hence id assignment) may shift. */
    TASK_CHECK(ma->inst_count == mb->inst_count);

    uint32_t *ops_a = (uint32_t *)malloc(sizeof(uint32_t) * ma->inst_count);
    uint32_t *ops_b = (uint32_t *)malloc(sizeof(uint32_t) * mb->inst_count);
    TASK_CHECK(ops_a != NULL && ops_b != NULL);
    for (size_t i = 0; i < ma->inst_count; ++i)
    {
        ops_a[i] = ma->inst_opcode[i];
        ops_b[i] = mb->inst_opcode[i];
    }
    for (size_t i = 1; i < ma->inst_count; ++i)
    {
        uint32_t ka = ops_a[i], kb = ops_b[i];
        size_t j = i;
        while (j > 0 && ops_a[j - 1] > ka) { ops_a[j] = ops_a[j - 1]; --j; }
        ops_a[j] = ka;
        j = i;
        while (j > 0 && ops_b[j - 1] > kb) { ops_b[j] = ops_b[j - 1]; --j; }
        ops_b[j] = kb;
    }
    int same_opcodes = 1;
    for (size_t i = 0; i < ma->inst_count; ++i)
    {
        if (ops_a[i] != ops_b[i]) same_opcodes = 0;
    }
    TASK_CHECK(same_opcodes);
    free(ops_a);
    free(ops_b);

    /* Both classify the sole image as a depth 2D image. */
    unsigned depth_images_a = 0, depth_images_b = 0;
    for (size_t i = 0; i < ma->inst_count; ++i)
    {
        if (ma->inst_opcode[i] == SpvOpTypeImage &&
            inst_words(ma, i)[4] == 1 && inst_words(ma, i)[3] == SpvDim2D)
        {
            ++depth_images_a;
        }
        if (mb->inst_opcode[i] == SpvOpTypeImage &&
            inst_words(mb, i)[4] == 1 && inst_words(mb, i)[3] == SpvDim2D)
        {
            ++depth_images_b;
        }
    }
    TASK_CHECK(depth_images_a == 1);
    TASK_CHECK(depth_images_b == 1);

    TASK_CHECK(count_opcode(ma, SpvOpImageSampleDrefImplicitLod) == 1);
    TASK_CHECK(count_opcode(mb, SpvOpImageSampleDrefImplicitLod) == 1);

    compile_destroy(&cra);
    compile_destroy(&crb);
    return 0;
}

/* ------------------------------------------------------------------ */
/* compat group: interleaved compiles and a reference mini-interpreter  */
/* ------------------------------------------------------------------ */

static int compat_interleaved_compilation(void)
{
    const char *depth_src =
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r); }";
    const char *ordinary_src =
        "Texture2D<float4> t; SamplerState s; "
        "float4 main(float2 uv:TEXCOORD0):SV_Target "
        "{ return t.SampleLevel(s, uv, 1.0); }";

    CompileResult a1, b, a2;
    TASK_CHECK(compile_hlsl(depth_src, TS_SHADER_STAGE_FRAGMENT, &a1));
    TASK_CHECK(compile_hlsl(ordinary_src, TS_SHADER_STAGE_VERTEX, &b));
    TASK_CHECK(compile_hlsl(depth_src, TS_SHADER_STAGE_FRAGMENT, &a2));

    TASK_CHECK(count_opcode(&a1.spirv, SpvOpImageSampleDrefImplicitLod) == 1);
    TASK_CHECK(count_opcode(&a2.spirv, SpvOpImageSampleDrefImplicitLod) == 1);
    TASK_CHECK(count_opcode(&b.spirv, SpvOpImageSampleDrefImplicitLod) == 0);
    TASK_CHECK(count_opcode(&b.spirv, SpvOpImageSampleDrefExplicitLod) == 0);
    TASK_CHECK(count_opcode(&b.spirv, SpvOpImageSampleExplicitLod) == 1);

    /* Repeated independent compiles of the same source are deterministic. */
    TASK_CHECK(a1.spirv.word_count == a2.spirv.word_count);
    TASK_CHECK(memcmp(a1.spirv.words, a2.spirv.words,
                      a1.spirv.word_count * 4) == 0);

    compile_destroy(&a1);
    compile_destroy(&b);
    compile_destroy(&a2);
    return 0;
}

/* Symbolic classification of an SSA id's origin. */
typedef enum Origin
{
    ORIGIN_UNKNOWN = 0,
    ORIGIN_CONSTANT = 1,
    ORIGIN_INPUT = 2,
} Origin;

/* Resolve an SSA id to the underlying value, transparently following loads
 * of function-scope allocas back through their stores, and user-function
 * parameters back to the entry wrapper's arguments. */
static uint32_t trace_origin_id(const SpirvModule *m, uint32_t id, unsigned depth)
{
    if (depth > 64 || id == 0) return 0;

    size_t idx;
    const uint32_t *w = def_words(m, id, &idx);
    if (!w) return 0;

    uint32_t op = m->inst_opcode[idx];

    if (op == SpvOpLoad)
    {
        uint32_t ptr = w[3];
        if (m->stored_value[ptr] != 0)
        {
            return trace_origin_id(m, m->stored_value[ptr], depth + 1);
        }
        return id; /* global/input variable load: keep the load */
    }

    if (op == SpvOpFunctionParameter && m->entry_arg_for_param[id] != 0)
    {
        return trace_origin_id(m, m->entry_arg_for_param[id], depth + 1);
    }

    return id;
}

static Origin classify_origin(const SpirvModule *m, uint32_t id, unsigned depth);

static Origin merge_origin(Origin a, Origin b)
{
    if (a == ORIGIN_INPUT || b == ORIGIN_INPUT) return ORIGIN_INPUT;
    if (a == ORIGIN_CONSTANT && b == ORIGIN_CONSTANT) return ORIGIN_CONSTANT;
    return ORIGIN_UNKNOWN;
}

static Origin classify_origin(const SpirvModule *m, uint32_t raw_id, unsigned depth)
{
    if (depth > 64) return ORIGIN_UNKNOWN;

    uint32_t id = trace_origin_id(m, raw_id, 0);
    if (id == 0) return ORIGIN_UNKNOWN;

    size_t idx;
    const uint32_t *w = def_words(m, id, &idx);
    if (!w) return ORIGIN_UNKNOWN;

    uint32_t op = m->inst_opcode[idx];
    switch (op)
    {
    case SpvOpConstant:
    case SpvOpConstantComposite:
        return ORIGIN_CONSTANT;

    case SpvOpLoad: {
        size_t var_idx = resolve_variable(m, w[3]);
        if (var_idx == (size_t)-1) return ORIGIN_UNKNOWN;
        const uint32_t *var = inst_words(m, var_idx);
        /* Input storage class is 1. */
        return (var[3] == SpvStorageClassInput) ? ORIGIN_INPUT : ORIGIN_UNKNOWN;
    }

    case SpvOpFAdd:
    case SpvOpFSub:
    case SpvOpFMul:
        return merge_origin(
            classify_origin(m, w[3], depth + 1),
            classify_origin(m, w[4], depth + 1));

    case SpvOpFNegate:
        return classify_origin(m, w[3], depth + 1);

    case SpvOpCompositeConstruct: {
        Origin merged = ORIGIN_CONSTANT;
        for (uint32_t k = 3; k < inst_len(m, idx); ++k)
        {
            merged = merge_origin(merged, classify_origin(m, w[k], depth + 1));
        }
        return merged;
    }

    case SpvOpCompositeExtract:
        return classify_origin(m, w[3], depth + 1);

    case SpvOpConvertSToF:
    case SpvOpConvertUToF:
        return classify_origin(m, w[3], depth + 1);

    default:
        return ORIGIN_UNKNOWN;
    }
}

/* A focused numeric interpreter for the small runtime-reference expression. */
static float interpret_float(
    const SpirvModule *m, uint32_t raw_id, float input_value, unsigned depth)
{
    if (depth > 64) return 0.0f;

    uint32_t id = trace_origin_id(m, raw_id, 0);
    if (id == 0) return 0.0f;

    size_t idx;
    const uint32_t *w = def_words(m, id, &idx);
    if (!w) return 0.0f;

    uint32_t op = m->inst_opcode[idx];
    switch (op)
    {
    case SpvOpConstant: {
        uint32_t bits = w[3];
        float f;
        memcpy(&f, &bits, sizeof(f));
        return f;
    }

    case SpvOpLoad: {
        size_t var_idx = resolve_variable(m, w[3]);
        if (var_idx != (size_t)-1 &&
            inst_words(m, var_idx)[3] == SpvStorageClassInput)
        {
            return input_value;
        }
        return 0.0f;
    }

    case SpvOpFAdd:
        return interpret_float(m, w[3], input_value, depth + 1) +
               interpret_float(m, w[4], input_value, depth + 1);
    case SpvOpFSub:
        return interpret_float(m, w[3], input_value, depth + 1) -
               interpret_float(m, w[4], input_value, depth + 1);
    case SpvOpFMul:
        return interpret_float(m, w[3], input_value, depth + 1) *
               interpret_float(m, w[4], input_value, depth + 1);
    case SpvOpFNegate:
        return -interpret_float(m, w[3], input_value, depth + 1);

    case SpvOpCompositeExtract:
        return interpret_float(m, w[3], input_value, depth + 1);

    case SpvOpConvertSToF:
    case SpvOpConvertUToF:
        return interpret_float(m, w[3], input_value, depth + 1);

    default:
        return 0.0f;
    }
}

static int compat_runtime_reference_dataflow(void)
{
    CompileResult cr;
    TASK_CHECK(compile_hlsl(
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmp(s, uv, r * 2.0 + 0.25); }",
        TS_SHADER_STAGE_FRAGMENT,
        &cr));

    const SpirvModule *m = &cr.spirv;
    size_t idx = find_first_opcode(m, SpvOpImageSampleDrefImplicitLod);
    const uint32_t *w = inst_words(m, idx);
    uint32_t dref_id = w[5];

    /* The reference operand is not a folded constant. */
    size_t dref_def;
    const uint32_t *dd = def_words(m, dref_id, &dref_def);
    TASK_CHECK(dd != NULL);
    TASK_CHECK(m->inst_opcode[dref_def] != SpvOpConstant);
    TASK_CHECK(m->inst_opcode[dref_def] != SpvOpConstantComposite);

    /* Independent data-flow classification proves an input dependency. */
    TASK_CHECK(classify_origin(m, dref_id, 0) == ORIGIN_INPUT);

    /* Numeric interpretation matches the independently evaluated expression. */
    float got_a = interpret_float(m, dref_id, 0.25f, 0);
    float got_b = interpret_float(m, dref_id, 0.5f, 0);
    TASK_CHECK(fabsf(got_a - (0.25f * 2.0f + 0.25f)) < 1e-6f);
    TASK_CHECK(fabsf(got_b - (0.5f * 2.0f + 0.25f)) < 1e-6f);

    compile_destroy(&cr);
    return 0;
}

/* A runtime reference computed inside a non-recursive helper must keep its
 * input dependency through the OpFunctionCall boundary. */
static int compat_runtime_reference_through_helper(void)
{
    CompileResult cr;
    TASK_CHECK(compile_hlsl(
        "Texture2D<float> t; SamplerComparisonState s; "
        "float probe(Texture2D<float> tex, SamplerComparisonState samp, "
        "float2 uv, float raw) "
        "{ return tex.SampleCmp(samp, uv, raw * 0.5 - 0.1); } "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return probe(t, s, uv, r); }",
        TS_SHADER_STAGE_FRAGMENT,
        &cr));

    const SpirvModule *m = &cr.spirv;
    size_t idx = find_first_opcode(m, SpvOpImageSampleDrefImplicitLod);
    TASK_CHECK(idx != (size_t)-1);
    const uint32_t *w = inst_words(m, idx);
    uint32_t dref_id = w[5];

    /* Not constant-folded. */
    size_t dref_def;
    TASK_CHECK(def_words(m, dref_id, &dref_def) != NULL);
    TASK_CHECK(m->inst_opcode[dref_def] != SpvOpConstant);

    /* The reference still traces back to an entry-point input. */
    TASK_CHECK(classify_origin(m, dref_id, 0) == ORIGIN_INPUT);

    float got = interpret_float(m, dref_id, 0.8f, 0);
    TASK_CHECK(fabsf(got - (0.8f * 0.5f - 0.1f)) < 1e-6f);

    compile_destroy(&cr);
    return 0;
}

static int compat_all_stages_roundtrip(void)
{
    const char *src =
        "Texture2D<float> t; SamplerComparisonState s; "
        "float main(float2 uv:TEXCOORD0,float r:TEXCOORD1):SV_Target "
        "{ return t.SampleCmpLevelZero(s, uv, r); }";

    TsShaderStage stages[3] = {
        TS_SHADER_STAGE_VERTEX,
        TS_SHADER_STAGE_FRAGMENT,
        TS_SHADER_STAGE_COMPUTE,
    };
    const char *compute_src =
        "Texture2D<float> t; SamplerComparisonState s; "
        "[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) "
        "{ float x = t.SampleCmpLevelZero(s, float2(0.0, 0.0), 0.5); }";

    for (int i = 0; i < 2; ++i)
    {
        CompileResult cr;
        TASK_CHECK(compile_hlsl(src, stages[i], &cr));
        TASK_CHECK(count_opcode(&cr.spirv, SpvOpImageSampleDrefExplicitLod) == 1);
        size_t idx = find_first_opcode(&cr.spirv, SpvOpImageSampleDrefExplicitLod);
        ImageInfo info;
        TASK_CHECK(validate_dref_chain(&cr.spirv, idx, 2, &info, NULL, 1));
        compile_destroy(&cr);
    }

    CompileResult comp;
    TASK_CHECK(compile_hlsl(compute_src, TS_SHADER_STAGE_COMPUTE, &comp));
    TASK_CHECK(count_opcode(&comp.spirv, SpvOpImageSampleDrefExplicitLod) == 1);
    size_t idx = find_first_opcode(&comp.spirv, SpvOpImageSampleDrefExplicitLod);
    ImageInfo info;
    TASK_CHECK(validate_dref_chain(&comp.spirv, idx, 2, &info, NULL, 1));
    compile_destroy(&comp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"typing", "sampler_kinds_distinct", typing_sampler_kinds_distinct},
    {"typing", "coordinate_dimensions", typing_coordinate_dimensions},
    {"typing", "reference_is_scalar_float", typing_reference_is_scalar_float},
    {"typing", "argument_counts", typing_argument_counts},
    {"typing", "resource_restrictions", typing_resource_restrictions},
    {"typing", "stage_restrictions", typing_stage_restrictions},

    {"lowering", "implicit_dref_fragment", lowering_implicit_dref_fragment},
    {"lowering", "explicit_zero_lod", lowering_explicit_zero_lod},
    {"lowering", "dimensions_1d_and_cube", lowering_dimensions_1d_and_cube},
    {"lowering", "asymmetric_coords_and_refs", lowering_asymmetric_coords_and_refs},
    {"lowering", "mixed_sampling_single_descriptor", lowering_mixed_sampling_single_descriptor},
    {"lowering", "binding_preserved", lowering_binding_preserved},
    {"lowering", "helper_parameters", lowering_helper_parameters},
    {"lowering", "scalar_ordinary_sample_valid", lowering_scalar_ordinary_sample_valid},
    {"lowering", "declaration_order_stable", lowering_declaration_order_stable},

    {"compat", "interleaved_compilation", compat_interleaved_compilation},
    {"compat", "runtime_reference_dataflow", compat_runtime_reference_dataflow},
    {"compat", "runtime_reference_through_helper", compat_runtime_reference_through_helper},
    {"compat", "all_stages_roundtrip", compat_all_stages_roundtrip},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
