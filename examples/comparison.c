/*
 * Example: depth-comparison sampling through the public tsCompile API.
 *
 * Compiles a fragment shader that uses both SampleCmp (implicit LOD) and
 * SampleCmpLevelZero (explicit zero LOD) on scalar float textures, plus a
 * vertex shader to show the LevelZero variant outside the fragment stage.
 * It then sanity-checks the emitted SPIR-V opcodes before reporting success.
 */
#include "tinyshader.h"
#include "spirv.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

static const char *fragment_source =
    "Texture2D<float> depth_tex;\n"
    "TextureCube<float> cube_tex;\n"
    "SamplerComparisonState cmp_samp;\n"
    "float main(float2 uv : TEXCOORD0,\n"
    "           float3 dir : TEXCOORD1,\n"
    "           float reference : TEXCOORD2) : SV_Target\n"
    "{\n"
    "    float a = depth_tex.SampleCmp(cmp_samp, uv, reference);\n"
    "    float b = depth_tex.SampleCmpLevelZero(cmp_samp, uv, reference);\n"
    "    float c = cube_tex.SampleCmp(cmp_samp, dir, reference);\n"
    "    return a + b + c;\n"
    "}\n";

static const char *vertex_source =
    "Texture2D<float> depth_tex;\n"
    "SamplerComparisonState cmp_samp;\n"
    "float main(float2 uv : TEXCOORD0, float reference : TEXCOORD1) : SV_Target\n"
    "{\n"
    "    return depth_tex.SampleCmpLevelZero(cmp_samp, uv, reference);\n"
    "}\n";

static unsigned count_opcode(const uint32_t *words, size_t word_count, SpvOp opcode)
{
    unsigned count = 0;
    for (size_t i = 5; i < word_count;)
    {
        uint32_t length = words[i] >> 16;
        uint32_t op = words[i] & 0xffff;
        if (op == (uint32_t)opcode) ++count;
        i += length;
    }
    return count;
}

static int compile_shader(
    const char *source, TsShaderStage stage, unsigned *implicit, unsigned *explicit_lod)
{
    TsCompilerOptions *options = tsCompilerOptionsCreate();
    tsCompilerOptionsSetStage(options, stage);
    tsCompilerOptionsSetEntryPoint(options, "main", 4);
    tsCompilerOptionsSetSource(
        options, source, strlen(source), "comparison_example.hlsl", 22);

    TsCompilerOutput *output = tsCompile(options);
    const char *error = tsCompilerOutputGetErrors(output);
    if (error)
    {
        fprintf(stderr, "compilation failed:\n%s\n", error);
        tsCompilerOutputDestroy(output);
        tsCompilerOptionsDestroy(options);
        return 0;
    }

    size_t byte_size = 0;
    const unsigned char *spirv = tsCompilerOutputGetSpirv(output, &byte_size);
    if (!spirv || byte_size < 20 || (byte_size % 4) != 0)
    {
        fprintf(stderr, "compilation produced no valid SPIR-V module\n");
        tsCompilerOutputDestroy(output);
        tsCompilerOptionsDestroy(options);
        return 0;
    }

    const uint32_t *words = (const uint32_t *)spirv;
    *implicit = count_opcode(words, byte_size / 4, SpvOpImageSampleDrefImplicitLod);
    *explicit_lod = count_opcode(words, byte_size / 4, SpvOpImageSampleDrefExplicitLod);

    tsCompilerOutputDestroy(output);
    tsCompilerOptionsDestroy(options);
    return 1;
}

int main(void)
{
    unsigned frag_implicit = 0, frag_explicit = 0;
    unsigned vert_implicit = 0, vert_explicit = 0;

    if (!compile_shader(
            fragment_source,
            TS_SHADER_STAGE_FRAGMENT,
            &frag_implicit,
            &frag_explicit))
    {
        return 1;
    }

    if (!compile_shader(
            vertex_source,
            TS_SHADER_STAGE_VERTEX,
            &vert_implicit,
            &vert_explicit))
    {
        return 1;
    }

    /* Fragment shader: two SampleCmp and one SampleCmpLevelZero. */
    if (frag_implicit != 2 || frag_explicit != 1)
    {
        fprintf(
            stderr,
            "unexpected fragment opcode counts: implicit=%u explicit=%u\n",
            frag_implicit,
            frag_explicit);
        return 1;
    }

    /* Vertex shader: SampleCmp is fragment-only; LevelZero is permitted. */
    if (vert_implicit != 0 || vert_explicit != 1)
    {
        fprintf(
            stderr,
            "unexpected vertex opcode counts: implicit=%u explicit=%u\n",
            vert_implicit,
            vert_explicit);
        return 1;
    }

    puts("COMPARISON_EXAMPLE_OK");
    return 0;
}
