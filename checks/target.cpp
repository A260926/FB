//=============================================================================
//
// checks/target.cpp
//
// Behavioural tests for HLSL matrix rows and multi component selections in
// the GLSL 140 generator.  Every check translates HLSL through the real
// parser and generator, then executes the resulting GLSL subset with the
// small interpreter in glsl_exec.cpp and compares numbers.  Checking the
// generated text is done in addition, never instead.
//
//=============================================================================

#include "task_support/check.h"

#include "HLSLParser.h"
#include "GLSLGenerator.h"
#include "HLSLGenerator.h"

// The interpreter is small and only used by these checks, so compile it into
// this translation unit.  The run script only has to compile checks/target.cpp.
#include "glsl_exec.cpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

using namespace M4;

namespace
{

struct Translation
{
    bool        parsed;
    bool        generated;
    std::string glsl;
    std::string hlsl;

    Translation() : parsed(false), generated(false) {}
};

bool ParseOnly(const char* source)
{
    Allocator allocator;
    HLSLTree tree(&allocator);
    HLSLParser parser(&allocator, "target-check.hlsl", source, strlen(source));
    return parser.Parse(&tree);
}

bool Translate(const char* source, Translation& result)
{
    static const char* fileName = "target-check.hlsl";

    Allocator allocator;
    HLSLTree tree(&allocator);
    HLSLParser parser(&allocator, fileName, source, strlen(source));
    result.parsed = parser.Parse(&tree);
    if (!result.parsed)
    {
        return false;
    }

    GLSLGenerator glsl;
    result.generated = glsl.Generate(&tree, GLSLGenerator::Target_FragmentShader,
        GLSLGenerator::Version_140, "shade");
    if (result.generated)
    {
        result.glsl = glsl.GetResult();
    }

    // Write-back to HLSL must keep working on the same tree after GLSL was
    // generated, which also proves the generator did not corrupt the nodes.
    HLSLGenerator hlsl;
    if (hlsl.Generate(&tree, HLSLGenerator::Target_PixelShader, "shade", false))
    {
        result.hlsl = hlsl.GetResult();
    }

    return result.generated;
}

bool Near(float a, float b)
{
    return fabsf(a - b) < 0.001f;
}

/**
 * Runs a shader and expects the given colour components.  The shader is
 * expected to write meaningful values into rast_FragData[0] which the
 * interpreter exposes as the result colour.
 */
bool Expect(const Translation& translation, float uvX, float uvY,
            float x, float y, float z, float w, const char* file, int line)
{
    glsl::Result run = glsl::RunFragment(translation.glsl, uvX, uvY);
    if (!run.ok)
    {
        fprintf(stderr, "GLSL execution failed at %s:%d: %s\n", file, line, run.error.c_str());
        fprintf(stderr, "---- GLSL ----\n%s\n--------------\n", translation.glsl.c_str());
        return false;
    }

    const float expected[4] = { x, y, z, w };
    for (int i = 0; i < 4; ++i)
    {
        if (!Near(run.shaderOutput.data[i], expected[i]))
        {
            fprintf(stderr, "value mismatch at %s:%d component %d: got %.4f expected %.4f\n",
                file, line, i, run.shaderOutput.data[i], expected[i]);
            fprintf(stderr, "---- GLSL ----\n%s\n--------------\n", translation.glsl.c_str());
            return false;
        }
    }
    return true;
}

#define CHECK_TRUE(x) TASK_CHECK(x)
#define CHECK_NEAR(a, b) TASK_CHECK(fabsf((a) - (b)) < 0.001f)

/**
 * Asserts that the HLSL write-back output still contains the original matrix
 * syntax, which is the "existing HLSL write-back" behaviour that must keep
 * working.
 */
bool HasHlslMatrixSyntax(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

//-----------------------------------------------------------------------------
// Shader sources
//-----------------------------------------------------------------------------

// Every shader below is a complete fragment shader for the 140 target.
// They deliberately use ordinary statements so the generated GLSL stays in
// the subset understood by the interpreter.

const char* kReadShader =
    "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float3x3 m = float3x3(1.0, 2.0, 3.0,\n"
    "                        4.0, 5.0, 6.0,\n"
    "                        7.0, 8.0, 9.0);\n"
    "  float2x2 n = float2x2(1.0, 2.0, 3.0, 4.0);\n"
    "  float4x4 q = float4x4(1.0, 2.0, 3.0, 4.0,\n"
    "                        5.0, 6.0, 7.0, 8.0,\n"
    "                        9.0, 10.0, 11.0, 12.0,\n"
    "                        13.0, 14.0, 15.0, 16.0);\n"
    "  float3 row = m[1];\n"
    "  float3 mixed = m._m00_m12_m20;\n"
    "  float a = m._11;\n"
    "  float b = m._m01_m10.x;\n"
    "  float c = n[1][0];\n"
    "  return float4(row.x + a + b + c, mixed.x + mixed.y + mixed.z, m[2].z, q[2][1]);\n"
    "}\n";

const char* kReadSwizzleShader =
    "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float3x3 m = float3x3(11.0, 12.0, 13.0,\n"
    "                        21.0, 22.0, 23.0,\n"
    "                        31.0, 32.0, 33.0);\n"
    "  float2 a = m._m00_m22.yx;\n"
    "  float b = m[1].z;\n"
    "  float4 c = m._m11_m11_m11_m11;\n"
    "  float d = m._m02;\n"
    "  return float4(a.x, a.y, b + d, c.z);\n"
    "}\n";

const char* kWriteShader =
    "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float3x3 m = float3x3(1.0, 2.0, 3.0,\n"
    "                        4.0, 5.0, 6.0,\n"
    "                        7.0, 8.0, 9.0);\n"
    "  m[1] = float3(10.0, 20.0, 30.0);\n"
    "  m._m00_m12 = float2(100.0, 200.0);\n"
    "  m[2].yz = float2(-1.0, -2.0);\n"
    "  m[0][2] = 42.0;\n"
    "  m._m11 += 5.0;\n"
    "  m._m20_m21 -= float2(1.0, 2.0);\n"
    "  m[0] *= float3(2.0, 3.0, 4.0);\n"
    "  m[2] /= float3(2.0, 1.0, 2.0);\n"
    "  return float4(m[1].x, m._m12, m[2].y, m._m00);\n"
    "}\n";

const char* kOverlapShader =
    "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float3x3 m = float3x3(1.0, 2.0, 3.0,\n"
    "                        4.0, 5.0, 6.0,\n"
    "                        7.0, 8.0, 9.0);\n"
    "  m[0].xyz = m[0].yzx;\n"
    "  m._m10_m11 = m._m11_m10;\n"
    "  return float4(m._m00, m._m01, m._m02, m._m10);\n"
    "}\n";

const char* kStructuredShader =
    "struct Block { float3x3 m; float pad; };\n"
    "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  Block blocks[3];\n"
    "  int k = 0;\n"
    "  blocks[0].m = float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);\n"
    "  blocks[1].m = blocks[0].m;\n"
    "  blocks[2].m = blocks[0].m;\n"
    "  blocks[k++].m._m00_m11 = float2(10.0, 20.0);\n"
    "  blocks[0].m._m00 += 5.0;\n"
    "  return float4(blocks[0].m._m00, blocks[0].m._m11, blocks[1].m._m00, k);\n"
    "}\n";

const char* kFlowShader =
    "float step1(float x) { return x + 1.0; }\n"
    "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float3x3 m = float3x3(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);\n"
    "  for (int i = 0; i < 3; i++) {\n"
    "    if (i == 1) { continue; }\n"
    "    m[i].x = step1(float(i));\n"
    "  }\n"
    "  int idx = 0;\n"
    "  for (int rows = 0; rows < 2 && (idx = rows + 1) > 0; rows++) {\n"
    "    m._m11 += 1.0;\n"
    "  }\n"
    "  float a = m._m00;\n"
    "  float b = m._m20;\n"
    "  return float4(a, b, m._m11, float(idx));\n"
    "}\n";

const char* kShortCircuitShader =
    "float bump(float x) { return x + 1.0; }\n"
    "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float3x3 m = float3x3(1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);\n"
    "  bool t = uv.x > 0.5;\n"
    "  if (t && (m._m00 = 10.0) > 0.0) { }\n"
    "  if (!t || (m._m01 = 20.0) > 0.0) { }\n"
    "  m._m11 = t ? (m._m00 = 30.0) : (m._m02 = 40.0);\n"
    "  return float4(m._m00, m._m01, m._m02, m._m11);\n"
    "}\n";

const char* kOutParamShader =
    "struct Block { float3x3 m; float pad; };\n"
    "void produce(out float3 o) { o = float3(1.0, 2.0, 3.0); }\n"
    "void consume_inout(inout float3 v) { v = v * 2.0; }\n"
    "void consume_pair(inout float2 v) { v = v * 2.0; }\n"
    "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float3x3 m = float3x3(1.0, 2.0, 3.0,\n"
    "                        4.0, 5.0, 6.0,\n"
    "                        7.0, 8.0, 9.0);\n"
    "  Block b;\n"
    "  b.m = m;\n"
    "  int r = 1;\n"
    "  produce(m[2]);\n"
    "  consume_inout(m[0]);\n"
    "  float2 pair = m._m00_m12;\n"
    "  consume_pair(pair);\n"
    "  m._m00_m12 = pair;\n"
    "  produce(m._m00_m11_m22);\n"
    "  produce(b.m[r]);\n"
    "  return float4(m._m00, m._m11, m._m22, m._m20);\n"
    "}\n";

const char* kReturnShader =
    "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float3x3 m = float3x3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);\n"
    "  if (m._m00 > 0.0) {\n"
    "    return float4(m._m00, m._m01, m._m02, m._m10);\n"
    "  }\n"
    "  return float4(0.0, 0.0, 0.0, 0.0);\n"
    "}\n";

// The supported scope is the square matrices.  Asymmetric matrices keep the
// historical selector bounds: the first digit is limited by the type's
// height and the second by its component count.
const char* kAsymmetricAccepted =
    "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float4x3 a = float4x3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0);\n"
    "  float4x2 b = float4x2(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0);\n"
    "  return float4(a._m02, a._12, b._m01, b._11);\n"
    "}\n";

const char* kAsymmetricRejected =
    "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float4x3 a = float4x3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0);\n"
    "  return float4(a._m30, 0.0, 0.0, 1.0);\n"
    "}\n";

const char* kHalfShader =
    "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  half3x3 m = half3x3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);\n"
    "  m[0] = half3(9.0, 8.0, 7.0);\n"
    "  half2 pair = m._m00_m12;\n"
    "  return float4(pair.x, pair.y, m._m22, 1.0);\n"
    "}\n";

const char* kMatrix2Shader =
    "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float2x2 a = float2x2(1.0, 2.0, 3.0, 4.0);\n"
    "  float2 row = a._m10_m01;\n"
    "  a[1] = float2(7.0, 8.0);\n"
    "  return float4(row.x, row.y, a._m10, a._m11);\n"
    "}\n";

const char* kMatrix4Shader =
    "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float4x4 a = float4x4(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0);\n"
    "  float4 c = a._m03_m12_m21_m30;\n"
    "  a._m00_m11_m22_m33 = float4(0.0, 1.0, 2.0, 3.0);\n"
    "  return float4(c.x, c.y + c.z, c.w, a._m33);\n"
    "}\n";

} // namespace

//-----------------------------------------------------------------------------
// read
//-----------------------------------------------------------------------------

static int CheckReadSelector(void)
{
    Translation t;
    CHECK_TRUE(Translate(kReadShader, t));

    // m = 1 2 3 / 4 5 6 / 7 8 9
    // row = m[1] = (4,5,6); a = m._m00 = 1; b = m._m01_m10.x = 2;
    // c = n[1][0] = 3 (n = 1 2 / 3 4)
    // m._m00_m12_m20 = (1, 6, 7) and m[2].z = 9.
    // q[2][1] = 10 (q rows 1..4 / 5..8 / 9..12 / 13..16).
    CHECK_TRUE(Expect(t, 0, 0, 4 + 1 + 2 + 3, 1 + 6 + 7, 9, 10, __FILE__, __LINE__));

    // Zero based and one based selectors must agree, and multi selection must
    // preserve order.
    CHECK_TRUE(t.glsl.find("vec3(") != std::string::npos);

    // Write-back keeps the original selector syntax.
    CHECK_TRUE(HasHlslMatrixSyntax(t.hlsl, "m[1]"));
    CHECK_TRUE(HasHlslMatrixSyntax(t.hlsl, "_m00_m12_m20"));
    CHECK_TRUE(HasHlslMatrixSyntax(t.hlsl, "_m01_m10"));
    return 0;
}

static int CheckReadSwizzle(void)
{
    Translation t;
    CHECK_TRUE(Translate(kReadSwizzleShader, t));

    // m rows: (11,12,13) (21,22,23) (31,32,33)
    // a = m._m00_m22.yx = (33, 11)
    // b = m[1].z = 23, d = m._m02 = 13
    // c.z = 22
    CHECK_TRUE(Expect(t, 0, 0, 33, 11, 23 + 13, 22, __FILE__, __LINE__));
    CHECK_TRUE(HasHlslMatrixSyntax(t.hlsl, "_m00_m22"));
    CHECK_TRUE(HasHlslMatrixSyntax(t.hlsl, ".yx"));
    return 0;
}

static int CheckReadOutOfBoundsRejected(void)
{
    const char* bad =
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float3x3 m = float3x3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);\n"
        "  return float4(m._m33, 0.0, 0.0, 1.0);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(!Translate(bad, t));
    CHECK_TRUE(!t.generated);
    return 0;
}

static int CheckReadMixedNumberingRejected(void)
{
    const char* bad =
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float3x3 m = float3x3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);\n"
        "  return float4(m._m00_11, 0.0, 0.0, 1.0);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(!Translate(bad, t));
    CHECK_TRUE(!t.generated);
    return 0;
}

static int CheckReadTooManyComponentsRejected(void)
{
    const char* bad =
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float4x4 m = float4x4(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);\n"
        "  return float4(m._m00_m11_m22_m33_m00, 1.0);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(!Translate(bad, t));
    return 0;
}

static int CheckReadRepeatedComponentAllowed(void)
{
    const char* src =
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float3x3 m = float3x3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);\n"
        "  float3 v = m._m00_m00_m00;\n"
        "  float4 w = m._m00_m11_m00_m22;\n"
        "  return float4(v.x + w.y, v.y, w.x + w.z, w.w);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(Translate(src, t));
    // v = (1,1,1), w = (1,5,1,9) -> (1+5, 1, 1+1, 9)
    CHECK_TRUE(Expect(t, 0, 0, 6, 1, 2, 9, __FILE__, __LINE__));
    return 0;
}

static int CheckReadDynamicDoubleIndex(void)
{
    const char* src =
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float3x3 m = float3x3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);\n"
        "  int r = 1;\n"
        "  float a = m[r][2];\n"
        "  float3 row = m[2];\n"
        "  float b = row.x;\n"
        "  return float4(a, b, 0.0, 1.0);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(Translate(src, t));
    // m[r][2] = m[1][2] = 6, m[2].x = 7
    CHECK_TRUE(Expect(t, 0, 0, 6, 7, 0, 1, __FILE__, __LINE__));
    return 0;
}

//-----------------------------------------------------------------------------
// write
//-----------------------------------------------------------------------------

static int CheckWriteAssignments(void)
{
    Translation t;
    CHECK_TRUE(Translate(kWriteShader, t));

    // Start:                     final:
    // 1 2 3                      m[0] = (200, 6, 42)
    // 4 5 6      ->               m[1] = (10, 20, 30)
    // 7 8 9                      m[2] = (-1, -2, -2.5)
    // m._m20_m21 -= (1,2) changes m[2].x from 7 to 6? Let's recompute below.
    //
    // Step by step on the HLSL matrix:
    //  m[1] = (10,20,30)          -> rows: (1,2,3),(10,20,30),(7,8,9)
    //  m._m00_m12 = (100,200)     -> rows: (100,2,3),(10,20,200),(7,8,9)
    //  m[2].yz = (-1,-2)          -> rows: (100,2,3),(10,20,200),(7,-1,-2)
    //  m[0][2] = 42               -> rows: (100,2,42),(10,20,200),(7,-1,-2)
    //  m._m11 += 5                -> m[1][1] = 25
    //  m._m20_m21 -= (1,2)        -> m[2][0] = 6, m[2][1] = -3
    //  m[0] *= (2,3,4)            -> m[0] = (200,6,168)
    //  m[2] /= (2,1,2)            -> m[2] = (3,-3,-1)
    // Return: (m[1].x, m._m12, m[2].y, m._m00) = (10, 200, -3, 200).
    CHECK_TRUE(Expect(t, 0, 0, 10, 200, -3, 200, __FILE__, __LINE__));

    CHECK_TRUE(HasHlslMatrixSyntax(t.hlsl, "_m00_m12"));
    CHECK_TRUE(HasHlslMatrixSyntax(t.hlsl, "_m11"));
    return 0;
}

static int CheckWriteOverlap(void)
{
    Translation t;
    CHECK_TRUE(Translate(kOverlapShader, t));

    // m[0].xyz = m[0].yzx must read the right hand side first:
    // (1,2,3) becomes (2,3,1).
    // m._m10_m11 = m._m11_m10: m[1] is (4,5,6), the selection yields (5,4),
    // so m[1][0] = 5 and m[1][1] = 4.
    // Return (m._m00, m._m01, m._m02, m._m10) = (2, 3, 1, 5).
    CHECK_TRUE(Expect(t, 0, 0, 2, 3, 1, 5, __FILE__, __LINE__));
    return 0;
}

static int CheckWriteDuplicateRejected(void)
{
    const char* bad =
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float3x3 m = float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);\n"
        "  m._m00_m00 = float2(1.0, 2.0);\n"
        "  return float4(m._m00, 0.0, 0.0, 1.0);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(!Translate(bad, t));
    CHECK_TRUE(!t.generated);
    return 0;
}

static int CheckWriteReadOnlyRejected(void)
{
    {
        const char* bad =
            "const float3x3 gm = float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);\n"
            "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
            "  gm._m00 = 5.0;\n"
            "  return float4(gm._m00, 0.0, 0.0, 1.0);\n"
            "}\n";
        Translation t;
        CHECK_TRUE(!Translate(bad, t));
    }
    {
        // A const local is read-only as well.
        const char* bad =
            "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
            "  const float3x3 m = float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);\n"
            "  m[0] = float3(2.0, 2.0, 2.0);\n"
            "  return float4(m._m00, 0.0, 0.0, 1.0);\n"
            "}\n";
        Translation t;
        CHECK_TRUE(!Translate(bad, t));
    }
    {
        // An increment is a write and must be rejected too.
        const char* bad =
            "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
            "  const float3x3 m = float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);\n"
            "  m._m00++;\n"
            "  return float4(m._m00, 0.0, 0.0, 1.0);\n"
            "}\n";
        Translation t;
        CHECK_TRUE(!Translate(bad, t));
    }
    return 0;
}

static int CheckWriteValidationHasLocation(void)
{
    // Invalid targets must produce a diagnostic with the source file and
    // line, and must not look successful.  Log_Error writes to stdout; the
    // run script captures it, so redirect here to inspect it.
    const char* bad =
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float3x3 m = float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);\n"
        "  m._m00_m00 = float2(1.0, 2.0);\n"
        "  return float4(m._m00, 0.0, 0.0, 1.0);\n"
        "}\n";

    // Capture stdout while translating.
    FILE* saved = stdout;
#if defined(_WIN32)
    FILE* capture = tmpfile();
#else
    // tmpfile() honours TMPDIR; point it at the sandbox writable location
    // when the environment leaves it unset.
    if (getenv("TMPDIR") == NULL)
    {
        setenv("TMPDIR", "/tmp", 1);
    }
    FILE* capture = tmpfile();
#endif
    if (capture == NULL)
    {
        TASK_CHECK(0);
    }
    stdout = capture;
    Translation t;
    bool generated = Translate(bad, t);
    fflush(stdout);
    stdout = saved;

    char buffer[2048];
    size_t length = 0;
    rewind(capture);
    length = fread(buffer, 1, sizeof(buffer) - 1, capture);
    fclose(capture);
    buffer[length] = 0;

    TASK_CHECK(!generated);
    if (strstr(buffer, "target-check.hlsl(3)") == NULL || strstr(buffer, ":") == NULL)
    {
        fprintf(stderr, "diagnostic buffer was: [%s]\n", buffer);
        return 1;
    }
    return 0;
}

static int CheckWriteStructuredDynamic(void)
{
    Translation t;
    CHECK_TRUE(Translate(kStructuredShader, t));

    // Blocks start as identity.  blocks[0].m._m00_m11 = (10,20) writes to
    // block 0 and the post increment of k must happen exactly once, so k is
    // 1.  blocks[0].m._m00 += 5 makes it 15.
    CHECK_TRUE(Expect(t, 0, 0, 15, 20, 1, 1, __FILE__, __LINE__));
    return 0;
}

static int CheckWriteMissingMatrixFieldRejected(void)
{
    const char* bad =
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float2x2 m = float2x2(1.0, 2.0, 3.0, 4.0);\n"
        "  float3x3 n = float3x3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);\n"
        "  n[0].x = m._m0;\n"
        "  return float4(n._m00, 0.0, 0.0, 1.0);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(!Translate(bad, t));
    return 0;
}

static int CheckWriteScalarAndTruncation(void)
{
    const char* source =
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float3x3 m = float3x3(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);\n"
        "  m[0] = 5.0;\n"
        "  m._m10_m11_m12 = float4(1.0, 2.0, 3.0, 4.0);\n"
        "  return float4(m._m00, m._m01, m._m10, m._m12);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(Translate(source, t));
    // Scalar broadcast fills the row, then a wider vector is truncated.
    CHECK_TRUE(Expect(t, 0, 0, 5, 5, 1, 3, __FILE__, __LINE__));
    return 0;
}

static int CheckWriteHalfAndSizes(void)
{
    {
        Translation t;
        CHECK_TRUE(Translate(kHalfShader, t));
        // m[0] = (9,8,7); pair = (m00, m12) = (9, 6); m._m22 = 9.
        CHECK_TRUE(Expect(t, 0, 0, 9, 6, 9, 1, __FILE__, __LINE__));
    }
    {
        Translation t;
        CHECK_TRUE(Translate(kMatrix2Shader, t));
        // a = 1 2 / 3 4; row = a._m10_m01 = (3, 2); a[1] = (7,8).
        CHECK_TRUE(Expect(t, 0, 0, 3, 2, 7, 8, __FILE__, __LINE__));
    }
    {
        Translation t;
        CHECK_TRUE(Translate(kMatrix4Shader, t));
        // a._m03_m12_m21_m30 = (4, 7, 10, 13)
        // a._m00_m11_m22_m33 = (0,1,2,3) so m._m33 = 3
        CHECK_TRUE(Expect(t, 0, 0, 4, 7 + 10, 13, 3, __FILE__, __LINE__));
    }
    return 0;
}

//-----------------------------------------------------------------------------
// flow
//-----------------------------------------------------------------------------

static int CheckFlowLoopsAndContinue(void)
{
    Translation t;
    CHECK_TRUE(Translate(kFlowShader, t));

    // m starts all zero.
    // Loop i = 0: m[0].x = step1(0) = 1.
    // i = 1: continue (no write).
    // i = 2: m[2].x = step1(2) = 3.
    // while runs twice, m._m11 = 2, idx = 2.
    CHECK_TRUE(Expect(t, 0, 0, 1, 3, 2, 2, __FILE__, __LINE__));
    return 0;
}

static int CheckFlowShortCircuitAndTernary(void)
{
    Translation t;
    CHECK_TRUE(Translate(kShortCircuitShader, t));

    // uv.x = 0 -> t is false.
    // t && (...) short circuits, so m._m00 stays 1.
    // !t || (...) short circuits, so m._m01 stays 0.
    // Ternary takes the false branch, so m._m02 = 40 and m._m11 = 40.
    CHECK_TRUE(Expect(t, 0.0f, 0.0f, 1, 0, 40, 40, __FILE__, __LINE__));

    // uv.x = 1 -> t is true.
    // m._m00 = 10 and m._m01 = 20; the true ternary branch then reassigns
    // m._m00 = 30 and yields 30 for m._m11.  m._m02 stays 0.
    CHECK_TRUE(Expect(t, 1.0f, 0.0f, 30, 20, 0, 30, __FILE__, __LINE__));
    return 0;
}

static int CheckFlowOutInout(void)
{
    Translation t;
    CHECK_TRUE(Translate(kOutParamShader, t));

    // Initial m row major: 1 2 3 / 4 5 6 / 7 8 9.
    // produce(m[2]) sets m[2] = (1,2,3).
    // consume_inout(m[0]) doubles row 0: (2,4,6).
    // pair = (m00, m12) = (2, 6); consume_pair doubles it to (4, 12);
    //   writing it back makes m00 = 4 and m12 = 12.
    // produce(m._m00_m11_m22) sets m00 = 1, m11 = 2, m22 = 3.
    // produce(b.m[r]) with r = 1 writes into the copy stored in b, not m.
    // Final m: m00 = 1, m11 = 2, m22 = 3, m20 = 1.
    CHECK_TRUE(Expect(t, 0, 0, 1, 2, 3, 1, __FILE__, __LINE__));
    CHECK_TRUE(HasHlslMatrixSyntax(t.hlsl, "m[2]"));
    return 0;
}

static int CheckFlowEarlyReturn(void)
{
    Translation t;
    CHECK_TRUE(Translate(kReturnShader, t));
    CHECK_TRUE(Expect(t, 0, 0, 1, 2, 3, 4, __FILE__, __LINE__));
    return 0;
}

static int CheckFlowAsymmetric(void)
{
    // The historical row/column correspondence is kept: the first selector
    // digit is bounded by the type's height (3 for float4x3, 2 for float4x2)
    // and the second by its component count (4 for both).
    CHECK_TRUE(ParseOnly(kAsymmetricAccepted));
    CHECK_TRUE(!ParseOnly(kAsymmetricRejected));

    // Same bounds on the write side.
    const char* badWrite =
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float4x2 b = float4x2(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0);\n"
        "  b._m20 = 1.0;\n"
        "  return float4(b._m01, 0.0, 0.0, 1.0);\n"
        "}\n";
    CHECK_TRUE(!ParseOnly(badWrite));
    return 0;
}

static int CheckFlowRepeatedGeneration(void)
{
    // Generating GLSL twice from the same tree, then HLSL, must not corrupt
    // the nodes or change the results.
    const char* source = kWriteShader;

    Allocator allocator;
    HLSLTree tree(&allocator);
    HLSLParser parser(&allocator, "repeat.hlsl", source, strlen(source));
    CHECK_TRUE(parser.Parse(&tree));

    GLSLGenerator first;
    CHECK_TRUE(first.Generate(&tree, GLSLGenerator::Target_FragmentShader, GLSLGenerator::Version_140, "shade"));
    std::string firstResult = first.GetResult();

    GLSLGenerator second;
    CHECK_TRUE(second.Generate(&tree, GLSLGenerator::Target_FragmentShader, GLSLGenerator::Version_140, "shade"));
    std::string secondResult = second.GetResult();

    CHECK_TRUE(firstResult == secondResult);

    // Reusing one generator instance must also be stable.
    CHECK_TRUE(first.Generate(&tree, GLSLGenerator::Target_FragmentShader, GLSLGenerator::Version_140, "shade"));
    CHECK_TRUE(firstResult == std::string(first.GetResult()));

    HLSLGenerator hlsl;
    CHECK_TRUE(hlsl.Generate(&tree, HLSLGenerator::Target_PixelShader, "shade", false));
    std::string hlslResult = hlsl.GetResult();
    CHECK_TRUE(HasHlslMatrixSyntax(hlslResult, "_m00_m12"));
    CHECK_TRUE(HasHlslMatrixSyntax(hlslResult, "m[1]"));

    // The HLSL write-back must be stable as well.
    HLSLGenerator hlsl2;
    CHECK_TRUE(hlsl2.Generate(&tree, HLSLGenerator::Target_PixelShader, "shade", false));
    CHECK_TRUE(hlslResult == hlsl2.GetResult());

    glsl::Result run = glsl::RunFragment(firstResult, 0, 0);
    CHECK_TRUE(run.ok);
    CHECK_TRUE(Near(run.shaderOutput.data[0], 10));
    CHECK_TRUE(Near(run.shaderOutput.data[1], 200));
    CHECK_TRUE(Near(run.shaderOutput.data[2], -3));
    CHECK_TRUE(Near(run.shaderOutput.data[3], 200));
    return 0;
}

static int CheckFlowDynamicIndexCount(void)
{
    // The increment in blocks[k++].m[row++] must happen exactly once per
    // assignment / output argument call.
    const char* source =
        "struct Block { float3x3 m; };\n"
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  Block blocks[4];\n"
        "  blocks[0].m = float3x3(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);\n"
        "  blocks[1].m = blocks[0].m;\n"
        "  blocks[2].m = blocks[0].m;\n"
        "  blocks[3].m = blocks[0].m;\n"
        "  int k = 0;\n"
        "  int row = 1;\n"
        "  blocks[k++].m[row++] = float3(5.0, 6.0, 7.0);\n"
        "  blocks[k++].m[row++] = float3(8.0, 9.0, 10.0);\n"
        "  return float4(float(k), float(row), blocks[0].m[1].x, blocks[1].m[2].x);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(Translate(source, t));
    CHECK_TRUE(Expect(t, 0, 0, 2, 3, 5, 8, __FILE__, __LINE__));
    return 0;
}

static int CheckFlowDynamicOutArgument(void)
{
    const char* source =
        "struct Block { float3x3 m; };\n"
        "void produce(out float3 o) { o = float3(1.0, 2.0, 3.0); }\n"
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  Block blocks[4];\n"
        "  blocks[0].m = float3x3(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);\n"
        "  blocks[1].m = blocks[0].m;\n"
        "  int k = 0;\n"
        "  int row = 0;\n"
        "  produce(blocks[k++].m[row]);\n"
        "  produce(blocks[k++].m[row]);\n"
        "  return float4(float(k), blocks[0].m[0].y, blocks[1].m[0].z, 1.0);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(Translate(source, t));
    CHECK_TRUE(Expect(t, 0, 0, 2, 2, 3, 1, __FILE__, __LINE__));
    return 0;
}

static int CheckFlowTwoOutputsAndSwap(void)
{
    const char* source =
        "void fill(out float2 a, out float2 b) { a = float2(1.0, 2.0); b = float2(3.0, 4.0); }\n"
        "void addone(inout float x) { x = x + 1.0; }\n"
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float3x3 m = float3x3(10.0, 0.0, 0.0, 0.0, 20.0, 0.0, 0.0, 0.0, 30.0);\n"
        "  int i = 0;\n"
        "  int j = 1;\n"
        "  fill(m[i], m[j]);\n"
        "  addone(m._m22);\n"
        "  float kept = m[1].x;\n"
        "  m._m00_m11 = m._m11_m00;\n"
        "  return float4(m._m00, m._m11, m._m22, kept);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(Translate(source, t));
    // fill writes rows 0 = (1,2,3) and 1 = (3,4,5); addone raises m22 to
    // 31; kept = m[1].x = 3.  The swap m._m00_m11 = m._m11_m00 turns the
    // diagonal (1,4) into (4,1), so m00 = 4 and m11 = 1.
    CHECK_TRUE(Expect(t, 0, 0, 4, 1, 31, 3, __FILE__, __LINE__));
    return 0;
}

static int CheckFlowReturnValueWithOut(void)
{
    const char* source =
        "float pick(float3x3 m, out float3 o, inout float3 io) {\n"
        "  o = m[0];\n"
        "  io = io + float3(1.0, 1.0, 1.0);\n"
        "  return m._m11 + m._m22;\n"
        "}\n"
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float3x3 m = float3x3(1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0);\n"
        "  float3 io = float3(10.0,20.0,30.0);\n"
        "  float r = pick(m, m[1], io);\n"
        "  return float4(r, m._m10, io.x, 1.0);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(Translate(source, t));
    // pick returns m11 + m22 = 5 + 9 = 14; o = m[0] = (1,2,3) is written
    // into row 1, so m10 = 1; io.x becomes 11.
    CHECK_TRUE(Expect(t, 0, 0, 14, 1, 11, 1, __FILE__, __LINE__));
    return 0;
}

static int CheckFlowOutCallInBranches(void)
{
    const char* source =
        "void setrow(out float3 o) { o = float3(2.0, 3.0, 4.0); }\n"
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float3x3 m = float3x3(1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0);\n"
        "  if (uv.x > 0.5) { setrow(m[1]); }\n"
        "  else { setrow(m[2]); }\n"
        "  return float4(m._m10, m._m20, 0.0, 1.0);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(Translate(source, t));

    // Only the taken branch performs the call.
    CHECK_TRUE(Expect(t, 1.0f, 0.0f, 2, 1, 0, 1, __FILE__, __LINE__));
    CHECK_TRUE(Expect(t, 0.0f, 0.0f, 1, 2, 0, 1, __FILE__, __LINE__));
    return 0;
}

static int CheckFlowArrayOfMatrices(void)
{
    const char* source =
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float3x3 arr[3];\n"
        "  arr[0] = float3x3(1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0);\n"
        "  arr[1] = arr[0];\n"
        "  arr[2] = arr[0];\n"
        "  int i = 1;\n"
        "  int j = 2;\n"
        "  float a = arr[i][j];\n"
        "  float b = arr[i]._m21;\n"
        "  arr[i][j] = 42.0;\n"
        "  arr[i]._m01 = 43.0;\n"
        "  return float4(a, b, arr[1][2].x, arr[1]._m01);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(Translate(source, t));
    // arr[i][j] selects row 2 of arr[1] = (7,8,9) and truncates to 7.
    // b = arr[1]._m21 = 8.  The write broadcasts 42 over row 2, so
    // arr[1][2] truncates to 42, and arr[1]._m01 becomes 43.
    CHECK_TRUE(Expect(t, 0, 0, 7, 8, 42, 43, __FILE__, __LINE__));

    return 0;
}

static int CheckFlowOutDoesNotReadOldValue(void)
{
    // A pure out call must not read the old value.  This is checked by
    // observing an uninitialised-looking array element: the write-back only
    // happens from the out temporary.
    const char* source =
        "void produce(out float3 o) { o = float3(7.0, 8.0, 9.0); }\n"
        "float4 shade(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float3x3 m = float3x3(1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0);\n"
        "  produce(m[1]);\n"
        "  return float4(m[1].x, m[1].y, m[1].z, m._m10);\n"
        "}\n";
    Translation t;
    CHECK_TRUE(Translate(source, t));
    CHECK_TRUE(Expect(t, 0, 0, 7, 8, 9, 7, __FILE__, __LINE__));
    return 0;
}

//-----------------------------------------------------------------------------

const task_case task_cases[] =
{
    { "read",  "selectors",              CheckReadSelector },
    { "read",  "swizzle_of_selection",   CheckReadSwizzle },
    { "read",  "out_of_bounds_rejected", CheckReadOutOfBoundsRejected },
    { "read",  "mixed_numbering_rejected", CheckReadMixedNumberingRejected },
    { "read",  "too_many_components_rejected", CheckReadTooManyComponentsRejected },
    { "read",  "repeated_component_allowed", CheckReadRepeatedComponentAllowed },
    { "read",  "dynamic_double_index",   CheckReadDynamicDoubleIndex },
    { "write", "assignments",            CheckWriteAssignments },
    { "write", "overlap_evaluates_rhs_first", CheckWriteOverlap },
    { "write", "duplicate_target_rejected", CheckWriteDuplicateRejected },
    { "write", "read_only_rejected",     CheckWriteReadOnlyRejected },
    { "write", "diagnostic_has_location", CheckWriteValidationHasLocation },
    { "write", "structured_dynamic",     CheckWriteStructuredDynamic },
    { "write", "missing_selector_rejected", CheckWriteMissingMatrixFieldRejected },
    { "write", "scalar_and_truncation",  CheckWriteScalarAndTruncation },
    { "write", "half_and_all_sizes",     CheckWriteHalfAndSizes },
    { "flow",  "loops_and_continue",     CheckFlowLoopsAndContinue },
    { "flow",  "short_circuit_and_ternary", CheckFlowShortCircuitAndTernary },
    { "flow",  "out_inout_parameters",   CheckFlowOutInout },
    { "flow",  "early_return",           CheckFlowEarlyReturn },
    { "flow",  "asymmetric_matrix",      CheckFlowAsymmetric },
    { "flow",  "repeated_generation",    CheckFlowRepeatedGeneration },
    { "flow",  "dynamic_index_count",    CheckFlowDynamicIndexCount },
    { "flow",  "dynamic_out_argument",   CheckFlowDynamicOutArgument },
    { "flow",  "two_outputs_and_swap",    CheckFlowTwoOutputsAndSwap },
    { "flow",  "return_value_with_out",   CheckFlowReturnValueWithOut },
    { "flow",  "out_call_in_branches",    CheckFlowOutCallInBranches },
    { "flow",  "array_of_matrices",       CheckFlowArrayOfMatrices },
    { "flow",  "out_does_not_read_old_value", CheckFlowOutDoesNotReadOldValue },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
