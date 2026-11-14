//=============================================================================
//
// checks/check-layout.cpp
//
// Verifies explicit packoffset support in the emulated constant buffer path:
//   * the standalone C++ layout query (offsets, reserved sizes, array strides)
//   * holes, backwards explicit positions, automatic fields continuing at the
//     high-water mark, conflicts and cross-register rejection
//   * struct arrays and matrices addressed through the generated GLSL
//   * generated expressions are actually evaluated against an independently
//     listed vec4 register table, not just checked for uniform array length
//   * unchanged output for buffers without packoffsets
//   * HLSL round-trip stability
//
// Standard C++11 and the project sources only; no GPU or shader SDK.
//
//=============================================================================

#include "HLSLParser.h"
#include "GLSLGenerator.h"
#include "HLSLGenerator.h"
#include "ConstantBufferLayout.h"

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace M4;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

static void CheckClose(float a, float b, const char* label)
{
    ++g_checks;
    if (std::fabs(a - b) > 1e-4f)
    {
        ++g_failures;
        std::printf("FAIL %s: got %f expected %f\n", label, a, b);
    }
}

// ---------------------------------------------------------------------------
// Parsing / generation helpers
// ---------------------------------------------------------------------------

static bool ParseShader(Allocator& allocator, HLSLTree& tree, const char* source)
{
    HLSLParser parser(&allocator, "check.hlsl", source, std::strlen(source));
    return parser.Parse(&tree);
}

static bool ParseShader(Allocator& allocator, HLSLTree& tree, const std::string& source)
{
    return ParseShader(allocator, tree, source.c_str());
}

static std::string GenerateGlsl(HLSLTree& tree, unsigned int flags, bool* ok = NULL)
{
    GLSLGenerator generator;
    GLSLGenerator::Options options;
    options.flags = flags;
    bool success = generator.Generate(&tree, GLSLGenerator::Target_FragmentShader,
                                      GLSLGenerator::Version_140, "main", options);
    if (ok != NULL) *ok = success;
    return success ? std::string(generator.GetResult()) : std::string();
}

static std::string GenerateHlsl(HLSLTree& tree, bool* ok = NULL)
{
    HLSLGenerator generator;
    bool success = generator.Generate(&tree, HLSLGenerator::Target_PixelShader, "main", false);
    if (ok != NULL) *ok = success;
    return success ? std::string(generator.GetResult()) : std::string();
}

struct CapturedError
{
    std::string message;
    static void Callback(void* userData, const char* message)
    {
        static_cast<CapturedError*>(userData)->message = message;
    }
};

static ConstantBufferLayout* MakeLayout(Allocator& allocator, HLSLTree& tree,
                                        HLSLBuffer* buffer, CapturedError& error)
{
    return BuildConstantBufferLayout(&allocator, &tree, buffer,
                                     &CapturedError::Callback, &error);
}

// ---------------------------------------------------------------------------
// Minimal GLSL expression evaluator.
//
// Understands exactly the expression shapes the generator emits for constant
// buffer reads: the Params vec4 array, xyzw swizzles, vec2/3/4 and mat4
// constructors, matrix row/column access, user struct constructors and their
// members, integer indices and a few local struct variables. Values keep up
// to 16 float components so whole-struct reads can be evaluated.
// ---------------------------------------------------------------------------

struct Val
{
    enum Kind { SCALAR, VEC, MAT } kind;
    int   count;
    float v[16];

    Val() : kind(SCALAR), count(0)
    {
        for (int i = 0; i < 16; ++i) v[i] = 0.0f;
    }
};

struct StructMemberInfo
{
    std::string name;
    int         offset;
    int         count;
};

class Evaluator
{
public:
    Evaluator(const float registers[][4], int numRegisters)
        : m_registers(registers), m_numRegisters(numRegisters) {}

    void SetInt(const std::string& name, int value) { m_ints[name] = value; }

    void RegisterStruct(const std::string& name,
                        const std::vector<StructMemberInfo>& members)
    {
        m_structs[name] = members;
    }

    // Declares a local struct variable from an already evaluated aggregate.
    void SetLocal(const std::string& name, const Val& value) { m_locals[name] = value; }

    Val Evaluate(const std::string& expression)
    {
        m_text = expression;
        m_pos  = 0;
        return ParseAdd();
    }

private:
    const float (*m_registers)[4];
    int          m_numRegisters;
    std::string  m_text;
    size_t       m_pos;
    std::map<std::string, int>                          m_ints;
    std::map<std::string, std::vector<StructMemberInfo> > m_structs;
    std::map<std::string, Val>                          m_locals;

    void SkipSpaces()
    {
        while (m_pos < m_text.size() && std::isspace((unsigned char)m_text[m_pos]))
            ++m_pos;
    }

    char Peek()
    {
        SkipSpaces();
        return m_pos < m_text.size() ? m_text[m_pos] : 0;
    }

    bool Match(char c)
    {
        if (Peek() == c) { ++m_pos; return true; }
        return false;
    }

    std::string ParseIdentifier()
    {
        SkipSpaces();
        size_t start = m_pos;
        while (m_pos < m_text.size() &&
               (std::isalnum((unsigned char)m_text[m_pos]) || m_text[m_pos] == '_'))
        {
            ++m_pos;
        }
        return m_text.substr(start, m_pos - start);
    }

    float ParseNumber()
    {
        SkipSpaces();
        char* end = NULL;
        float value = static_cast<float>(std::strtod(m_text.c_str() + m_pos, &end));
        m_pos = static_cast<size_t>(end - m_text.c_str());
        return value;
    }

    static bool IsSwizzle(const std::string& s)
    {
        if (s.empty() || s.size() > 4) return false;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] != 'x' && s[i] != 'y' && s[i] != 'z' && s[i] != 'w')
                return false;
        }
        return true;
    }

    static int ComponentIndex(char c)
    {
        switch (c)
        {
        case 'x': return 0;
        case 'y': return 1;
        case 'z': return 2;
        case 'w': return 3;
        }
        return 0;
    }

    const std::vector<StructMemberInfo>* FindStructMembers(const std::string& name) const
    {
        std::map<std::string, std::vector<StructMemberInfo> >::const_iterator it =
            m_structs.find(name);
        return it == m_structs.end() ? NULL : &it->second;
    }

    // Which user struct (if any) has a member with this name.
    const StructMemberInfo* FindMember(const std::string& memberName,
                                       const std::string*& structName) const
    {
        for (std::map<std::string, std::vector<StructMemberInfo> >::const_iterator it =
                 m_structs.begin(); it != m_structs.end(); ++it)
        {
            const std::vector<StructMemberInfo>& members = it->second;
            for (size_t j = 0; j < members.size(); ++j)
            {
                if (members[j].name == memberName)
                {
                    structName = &it->first;
                    return &members[j];
                }
            }
        }
        structName = NULL;
        return NULL;
    }

    Val MakeScalar(float f)
    {
        Val val;
        val.kind  = Val::SCALAR;
        val.count = 1;
        val.v[0]  = f;
        return val;
    }

    Val MakeVec(const float* components, int count)
    {
        Val val;
        val.kind  = Val::VEC;
        val.count = count;
        for (int i = 0; i < count && i < 16; ++i) val.v[i] = components[i];
        return val;
    }

    std::vector<Val> ParseArguments()
    {
        std::vector<Val> args;
        Match('(');
        if (Peek() != ')')
        {
            for (;;)
            {
                args.push_back(ParseAdd());
                if (!Match(',')) break;
            }
        }
        Match(')');
        return args;
    }

    Val ApplyMemberOrSwizzle(const Val& object, const std::string& name)
    {
        if (IsSwizzle(name))
        {
            Val result;
            result.kind  = Val::VEC;
            result.count = static_cast<int>(name.size());
            for (int i = 0; i < result.count; ++i)
            {
                int idx = ComponentIndex(name[i]);
                result.v[i] = idx < object.count ? object.v[idx] : 0.0f;
            }
            return result;
        }

        const std::string* structName = NULL;
        const StructMemberInfo* member = FindMember(name, structName);
        if (member != NULL)
        {
            Val result;
            result.kind  = Val::VEC;
            result.count = member->count;
            for (int k = 0; k < member->count; ++k)
            {
                int idx = member->offset + k;
                result.v[k] = idx < object.count ? object.v[idx] : 0.0f;
            }
            return result;
        }
        return Val();
    }

    Val ApplyPostfix(Val object)
    {
        for (;;)
        {
            if (Peek() == '[')
            {
                Match('[');
                Val index = ParseAdd();
                Match(']');
                int i = static_cast<int>(index.v[0]);
                if (object.kind == Val::MAT)
                {
                    // Matrix column access m[i].
                    float components[4];
                    for (int row = 0; row < 4; ++row)
                        components[row] = object.v[i * 4 + row];
                    object = MakeVec(components, 4);
                }
                // Non-matrix indexing is not generated for these cbuffer reads.
            }
            else if (Peek() == '.')
            {
                Match('.');
                std::string name = ParseIdentifier();
                object = ApplyMemberOrSwizzle(object, name);
            }
            else
            {
                break;
            }
        }
        return object;
    }

    Val ParsePrimary()
    {
        SkipSpaces();
        char c = Peek();

        if (c == '(')
        {
            Match('(');
            Val inner = ParseAdd();
            Match(')');
            return ApplyPostfix(inner);
        }

        if (std::isdigit((unsigned char)c))
        {
            return ApplyPostfix(MakeScalar(ParseNumber()));
        }

        std::string identifier = ParseIdentifier();

        if (identifier == "Params")
        {
            Match('[');
            Val index = ParseAdd();
            Match(']');
            int indexValue = static_cast<int>(index.v[0]);
            float components[4] = {0, 0, 0, 0};
            if (indexValue >= 0 && indexValue < m_numRegisters)
            {
                for (int i = 0; i < 4; ++i)
                    components[i] = m_registers[indexValue][i];
            }
            return ApplyPostfix(MakeVec(components, 4));
        }

        std::map<std::string, int>::const_iterator intIt = m_ints.find(identifier);
        if (intIt != m_ints.end())
            return ApplyPostfix(MakeScalar(static_cast<float>(intIt->second)));

        std::map<std::string, Val>::const_iterator localIt = m_locals.find(identifier);
        if (localIt != m_locals.end())
            return ApplyPostfix(localIt->second);

        if (Peek() == '(')
        {
            std::vector<Val> args = ParseArguments();

            if (identifier == "vec4" || identifier == "vec3" || identifier == "vec2")
            {
                int wanted = identifier[3] - '0';
                float components[16] = {0};
                int out = 0;
                for (size_t a = 0; a < args.size() && out < wanted; ++a)
                {
                    for (int k = 0; k < args[a].count && out < wanted; ++k)
                        components[out++] = args[a].v[k];
                }
                return ApplyPostfix(MakeVec(components, wanted));
            }

            if (identifier == "mat4")
            {
                Val mat;
                mat.kind  = Val::MAT;
                mat.count = 16;
                for (int col = 0; col < 4 && col < static_cast<int>(args.size()); ++col)
                {
                    for (int row = 0; row < 4; ++row)
                        mat.v[col * 4 + row] = args[col].v[row];
                }
                return ApplyPostfix(mat);
            }

            if (identifier.compare(0, 10, "matrix_row") == 0)
            {
                const Val& mat = args[0];
                int row = static_cast<int>(args[1].v[0]);
                float components[4];
                for (int col = 0; col < 4; ++col)
                    components[col] = mat.v[col * 4 + row];
                return ApplyPostfix(MakeVec(components, 4));
            }

            // User defined struct constructor: concatenate argument components.
            Val aggregate;
            aggregate.kind  = Val::VEC;
            aggregate.count = 0;
            for (size_t a = 0; a < args.size(); ++a)
            {
                for (int k = 0; k < args[a].count && aggregate.count < 16; ++k)
                    aggregate.v[aggregate.count++] = args[a].v[k];
            }
            return ApplyPostfix(aggregate);
        }

        return Val();
    }

    Val ParseUnary()
    {
        if (Peek() == '-')
        {
            Match('-');
            Val value = ParseUnary();
            value.v[0] = -value.v[0];
            return value;
        }
        return ParsePrimary();
    }

    Val ParseMul()
    {
        Val left = ParseUnary();
        while (Peek() == '*' || Peek() == '/')
        {
            char op = m_text[m_pos++];
            Val right = ParseUnary();
            left.v[0] = op == '*' ? left.v[0] * right.v[0] : left.v[0] / right.v[0];
            left.count = 1;
            left.kind  = Val::SCALAR;
        }
        return left;
    }

    Val ParseAdd()
    {
        Val left = ParseMul();
        while (Peek() == '+' || Peek() == '-')
        {
            char op = m_text[m_pos++];
            Val right = ParseMul();
            int count = left.count > right.count ? left.count : right.count;
            Val result;
            result.kind  = Val::VEC;
            result.count = count;
            for (int i = 0; i < count; ++i)
            {
                float a = i < left.count ? left.v[i] : 0.0f;
                float b = i < right.count ? right.v[i] : 0.0f;
                result.v[i] = op == '+' ? a + b : a - b;
            }
            left = result;
        }
        return left;
    }
};

// Extracts the "return EXPR;" body of the generated vec4 main function.
static std::string ExtractReturnExpression(const std::string& glsl)
{
    const std::string marker = "vec4 main(";
    size_t mainPos = glsl.find(marker);
    if (mainPos == std::string::npos) return "";
    size_t retPos = glsl.find("return ", mainPos);
    if (retPos == std::string::npos) return "";
    retPos += 7;
    size_t end = glsl.find(';', retPos);
    if (end == std::string::npos) return "";
    return glsl.substr(retPos, end - retPos);
}

// Finds a generated local struct declaration "Type name = EXPR;" in main and
// returns the initializer expression, or "" if not present.
static std::string ExtractLocalInitializer(const std::string& glsl, const std::string& name)
{
    std::string needle = " " + name + " = ";
    size_t pos = glsl.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    size_t end = glsl.find(';', pos);
    if (end == std::string::npos) return "";
    return glsl.substr(pos, end - pos);
}

// ---------------------------------------------------------------------------
// Test data
// ---------------------------------------------------------------------------

static const int kNumRegisters = 19;
static float RegisterValue(int reg, int comp)
{
    return static_cast<float>(1000 * reg + comp);
}

static const char* kValueShaderPrefix =
    "struct Data { float2 scale; float weight; float3 bias; float4 tint; };\n"
    "cbuffer Params {\n"
    "    float4x4 mtx    : packoffset(c2);\n"
    "    Data    items[2]: packoffset(c8);\n"
    "    float   alpha   : packoffset(c0.x);\n"
    "    float   beta    : packoffset(c1.z);\n"
    "    float3  extra;\n"
    "    float   gap     : packoffset(c18.w);\n"
    "};\n"
    "float4 main(float2 p:TEXCOORD0):COLOR0 {\n"
    "    int i = int(p.x);\n";

struct ValueCase
{
    const char* hlslReturn;
    int         dynamicI;       // -1 when the expression does not use i
    float       expected[4];
    bool        wholeStruct;    // reads the struct through a local variable
};

int main()
{
    // Each test section gets its own allocator. HLSLTree frees its node pages
    // through the allocator in its destructor, so one allocator is paired with
    // the trees created inside a single scope.

    // -----------------------------------------------------------------------
    // 1. Layout query: holes, backwards positions, automatic continuation.
    // -----------------------------------------------------------------------
    {
        Allocator allocator;
        const char* source =
            "struct Data { float2 scale; float weight; float3 bias; float4 tint; };\n"
            "cbuffer Params {\n"
            "    float4x4 mtx    : packoffset(c2);\n"
            "    Data    items[2]: packoffset(c8);\n"
            "    float   alpha   : packoffset(c0.x);\n"
            "    float   beta    : packoffset(c1.z);\n"
            "    float3  extra;\n"
            "    float   gap     : packoffset(c18.w);\n"
            "};\n"
            "float4 main(float2 p:TEXCOORD0):COLOR0 { return mtx[0]; }\n";

        HLSLTree tree(&allocator);
        CHECK(ParseShader(allocator, tree, source));
        HLSLBuffer* buffer = tree.FindBuffer("Params");
        CHECK(buffer != NULL);

        CapturedError error;
        ConstantBufferLayout* layout = MakeLayout(allocator, tree, buffer, error);
        CHECK(layout != NULL);
        CHECK(layout->valid);
        CHECK(error.message.empty());
        CHECK(layout->hasExplicitOffsets);
        // Data is 12 floats (scale 0-1, weight 2, bias 4-6, tint 8-11); the
        // two-element array spans slots 32..55 and gap ends at slot 76, which
        // is already register aligned, so the buffer holds 19 registers.
        CHECK(layout->size == 76);
        CHECK(layout->numRegisters == 19);

        const ConstantBufferFieldLayout* mtx   = layout->FindField("mtx");
        const ConstantBufferFieldLayout* items = layout->FindField("items");
        const ConstantBufferFieldLayout* alpha = layout->FindField("alpha");
        const ConstantBufferFieldLayout* beta  = layout->FindField("beta");
        const ConstantBufferFieldLayout* extra = layout->FindField("extra");
        const ConstantBufferFieldLayout* gap   = layout->FindField("gap");

        CHECK(mtx && mtx->offset == 8 && mtx->size == 16 && mtx->arrayStride == 0);
        CHECK(items && items->offset == 32 && items->size == 24 && items->arrayStride == 12);
        CHECK(alpha && alpha->offset == 0 && alpha->size == 1);
        // c1.z is linear slot 6.
        CHECK(beta && beta->offset == 6 && beta->size == 1);
        // Automatic field starts at the high-water mark (56), never backfills.
        CHECK(extra && extra->offset == 56 && extra->size == 3 && !extra->explicitOffset);
        CHECK(gap && gap->offset == 75 && gap->size == 1);

        unsigned int memberOffset = 0;
        HLSLType dataType = items->declaration->type;
        CHECK(GetStructMemberOffset(&tree, dataType, "scale", memberOffset) && memberOffset == 0);
        CHECK(GetStructMemberOffset(&tree, dataType, "weight", memberOffset) && memberOffset == 2);
        CHECK(GetStructMemberOffset(&tree, dataType, "bias", memberOffset) && memberOffset == 4);
        CHECK(GetStructMemberOffset(&tree, dataType, "tint", memberOffset) && memberOffset == 8);
        unsigned int stride = 0;
        HLSLType elementType = dataType;
        elementType.array = false;
        CHECK(GetConstantArrayStride(&tree, elementType, stride) && stride == 12);
    }

    // -----------------------------------------------------------------------
    // 2. Diagnostics through the standalone layout result.
    // -----------------------------------------------------------------------
    {
        Allocator allocator;
    struct DiagCase
    {
        const char* source;
        const char* messageFragment;
    };
    const DiagCase diagCases[] =
    {
        {
            "cbuffer P { float2 a : packoffset(c0); float b : packoffset(c0.y); };\n"
            "float4 main(float2 q:TEXCOORD0):COLOR0 { return float4(a,b,1); }\n",
            "conflict"
        },
        {
            "cbuffer P { float3 a : packoffset(c0.z); };\n"
            "float4 main(float2 q:TEXCOORD0):COLOR0 { return float4(a,1); }\n",
            "crosses a register"
        },
        {
            "cbuffer P { float4 a[2] : packoffset(c2.y); };\n"
            "float4 main(float2 q:TEXCOORD0):COLOR0 { return a[0]; }\n",
            "start of a register"
        },
        {
            "cbuffer P { float4x4 a : packoffset(c3.z); };\n"
            "float4 main(float2 q:TEXCOORD0):COLOR0 { return a[0]; }\n",
            "start of a register"
        },
        {
            "struct S { float a; float4 b; };\n"
            "cbuffer P { S s : packoffset(c1.y); };\n"
            "float4 main(float2 q:TEXCOORD0):COLOR0 { return s.b; }\n",
            "start of a register"
        },
        {
            "cbuffer P { float a[0]; };\n"
            "float4 main(float2 q:TEXCOORD0):COLOR0 { return a[0]; }\n",
            "positive constant array length"
        },
        {
            "cbuffer P { int a; };\n"
            "float4 main(float2 q:TEXCOORD0):COLOR0 { return float4(a,1,1,1); }\n",
            "not supported"
        },
    };

    for (size_t i = 0; i < sizeof(diagCases) / sizeof(diagCases[0]); ++i)
    {
        Allocator caseAllocator;
        HLSLTree tree(&caseAllocator);
        CHECK(ParseShader(caseAllocator, tree, diagCases[i].source));
        HLSLBuffer* buffer = tree.FindBuffer("P");
        CHECK(buffer != NULL);
        CapturedError error;
        ConstantBufferLayout* layout = MakeLayout(caseAllocator, tree, buffer, error);
        CHECK(layout != NULL);
        CHECK(!layout->valid);
        CHECK(error.message.find(diagCases[i].messageFragment) != std::string::npos);

        bool ok = true;
        std::string glsl = GenerateGlsl(tree, GLSLGenerator::Flag_EmulateConstantBuffer, &ok);
        CHECK(!ok);
        CHECK(glsl.empty());
    }

    const char* parseBadCases[] =
    {
        "cbuffer P { float a : packoffset(c0.q); };\n"
        "float4 main(float2 q:TEXCOORD0):COLOR0 { return float4(a,1,1,1); }\n",
        "cbuffer P { float a : packoffset(b3); };\n"
        "float4 main(float2 q:TEXCOORD0):COLOR0 { return float4(a,1,1,1); }\n",
        "cbuffer P { float a : packoffset(c); };\n"
        "float4 main(float2 q:TEXCOORD0):COLOR0 { return float4(a,1,1,1); }\n",
    };
    for (size_t i = 0; i < sizeof(parseBadCases) / sizeof(parseBadCases[0]); ++i)
    {
        Allocator caseAllocator;
        HLSLTree tree(&caseAllocator);
        CHECK(!ParseShader(caseAllocator, tree, parseBadCases[i]));
    }

    // Explicit packoffset without emulation must be rejected.
    {
        const char* source =
            "cbuffer P { float a : packoffset(c0.x); };\n"
            "float4 main(float2 q:TEXCOORD0):COLOR0 { return float4(a,1,1,1); }\n";
        HLSLTree tree(&allocator);
        CHECK(ParseShader(allocator, tree, source));
        bool ok = true;
        std::string glsl = GenerateGlsl(tree, 0, &ok);
        CHECK(!ok);
        CHECK(glsl.empty());
    }
    } // section 2

    // -----------------------------------------------------------------------
    // 3. Generated expressions evaluated against the register table.
    // -----------------------------------------------------------------------
    {
    float registers[kNumRegisters][4];
    for (int r = 0; r < kNumRegisters; ++r)
        for (int c = 0; c < 4; ++c)
            registers[r][c] = RegisterValue(r, c);

    // Member positions inside the generated struct constructor, which
    // concatenates the member reads without the in-memory padding.
    std::vector<StructMemberInfo> dataMembers;
    dataMembers.push_back(StructMemberInfo{"scale", 0, 2});
    dataMembers.push_back(StructMemberInfo{"weight", 2, 1});
    dataMembers.push_back(StructMemberInfo{"bias", 3, 3});
    dataMembers.push_back(StructMemberInfo{"tint", 6, 4});

    const ValueCase valueCases[] =
    {
        { "return float4(alpha,0,0,0);", -1, { RegisterValue(0,0), 0, 0, 0 }, false },
        { "return float4(beta,0,0,0);",  -1, { RegisterValue(1,2), 0, 0, 0 }, false },
        { "return float4(extra,0);",     -1, { RegisterValue(14,0), RegisterValue(14,1), RegisterValue(14,2), 0 }, false },
        { "return float4(items[0].weight,0,0,0);", -1, { RegisterValue(8,2), 0, 0, 0 }, false },
        { "return items[1].tint;",       -1, { RegisterValue(13,0), RegisterValue(13,1), RegisterValue(13,2), RegisterValue(13,3) }, false },
        { "return float4(items[0].bias,0);", -1, { RegisterValue(9,0), RegisterValue(9,1), RegisterValue(9,2), 0 }, false },
        { "return float4(items[i].scale,0,0);", 0, { RegisterValue(8,0), RegisterValue(8,1), 0, 0 }, false },
        { "return float4(items[i].scale,0,0);", 1, { RegisterValue(11,0), RegisterValue(11,1), 0, 0 }, false },
        // mtx[2] is row 2: the .z component of columns at c2..c5.
        { "return mtx[2];",              -1, { RegisterValue(2,2), RegisterValue(3,2), RegisterValue(4,2), RegisterValue(5,2) }, false },
        { "return float4(gap,0,0,0);",   -1, { RegisterValue(18,3), 0, 0, 0 }, false },
        // Whole-struct read through a local must resolve to the element address.
        { "Data d = items[1]; return d.tint;", -1, { RegisterValue(13,0), RegisterValue(13,1), RegisterValue(13,2), RegisterValue(13,3) }, true },
    };

    for (size_t i = 0; i < sizeof(valueCases) / sizeof(valueCases[0]); ++i)
    {
        std::string source = std::string(kValueShaderPrefix);
        source += "    ";
        source += valueCases[i].hlslReturn;
        source += "\n}\n";

        Allocator caseAllocator;
        HLSLTree tree(&caseAllocator);
        CHECK(ParseShader(caseAllocator, tree, source));
        bool ok = false;
        std::string glsl = GenerateGlsl(tree, GLSLGenerator::Flag_EmulateConstantBuffer, &ok);
        CHECK(ok);
        if (!ok) continue;

        CHECK(glsl.find("uniform vec4 Params[19]") != std::string::npos);

        Evaluator evaluator(registers, kNumRegisters);
        evaluator.RegisterStruct("Data", dataMembers);
        if (valueCases[i].dynamicI >= 0)
            evaluator.SetInt("i", valueCases[i].dynamicI);

        Val result;
        if (valueCases[i].wholeStruct)
        {
            std::string initializer = ExtractLocalInitializer(glsl, "d");
            CHECK(!initializer.empty());
            Val whole = evaluator.Evaluate(initializer);
            evaluator.SetLocal("d", whole);
            std::string expression = ExtractReturnExpression(glsl);
            CHECK(!expression.empty());
            result = evaluator.Evaluate(expression);
        }
        else
        {
            std::string expression = ExtractReturnExpression(glsl);
            CHECK(!expression.empty());
            result = evaluator.Evaluate(expression);
        }

        char label[64];
        String_Printf(label, sizeof(label), "value case %d (%s)",
                      static_cast<int>(i), valueCases[i].hlslReturn);
        for (int c = 0; c < 4; ++c)
            CheckClose(result.v[c], valueCases[i].expected[c], label);
    }

    // Constant dynamic index out of range is a clean diagnostic.
    {
        const char* source =
            "struct Data { float2 scale; float weight; float3 bias; float4 tint; };\n"
            "cbuffer Params { Data items[2] : packoffset(c8); };\n"
            "float4 main(float2 p:TEXCOORD0):COLOR0 { return items[5].tint; }\n";
        Allocator subAllocator;
        HLSLTree tree(&subAllocator);
        CHECK(ParseShader(subAllocator, tree, source));
        bool ok = true;
        std::string glsl = GenerateGlsl(tree, GLSLGenerator::Flag_EmulateConstantBuffer, &ok);
        CHECK(!ok);
        CHECK(glsl.empty());
    }

    // A side-effecting dynamic index is rejected.
    {
        const char* source =
            "cbuffer Params { float4 a[4] : packoffset(c0); float j; };\n"
            "float4 main(float2 p:TEXCOORD0):COLOR0 { int k = int(p.x); return a[k++]; }\n";
        Allocator subAllocator;
        HLSLTree tree(&subAllocator);
        CHECK(ParseShader(subAllocator, tree, source));
        bool ok = true;
        std::string glsl = GenerateGlsl(tree, GLSLGenerator::Flag_EmulateConstantBuffer, &ok);
        CHECK(!ok);
        CHECK(glsl.empty());
    }

    // A pure, side-effect-free dynamic index is accepted.
    {
        const char* source =
            "cbuffer Params { float4 a[4] : packoffset(c0); };\n"
            "float4 main(float2 p:TEXCOORD0):COLOR0 { int k = int(p.x); return a[(k+1)*2]; }\n";
        Allocator subAllocator;
        HLSLTree tree(&subAllocator);
        CHECK(ParseShader(subAllocator, tree, source));
        bool ok = false;
        std::string glsl = GenerateGlsl(tree, GLSLGenerator::Flag_EmulateConstantBuffer, &ok);
        CHECK(ok);
        // A pure dynamic index is emitted as register-stride arithmetic
        // referencing k, not rejected.
        CHECK(glsl.find("1*(") != std::string::npos);
        CHECK(glsl.find("k") != std::string::npos);
    }
    } // section 3

    // -----------------------------------------------------------------------
    // 4. Buffers without packoffsets keep the legacy layout and output.
    // -----------------------------------------------------------------------
    {
        Allocator allocator;
        const char* source =
            "cbuffer Params { float2 uv; float fog; float4 tint[2]; };\n"
            "float4 main(float2 p:TEXCOORD0):COLOR0 { return tint[1] + float4(uv,fog,1); }\n";
        HLSLTree tree(&allocator);
        CHECK(ParseShader(allocator, tree, source));
        HLSLBuffer* buffer = tree.FindBuffer("Params");
        CapturedError error;
        ConstantBufferLayout* layout = MakeLayout(allocator, tree, buffer, error);
        CHECK(layout->valid);
        CHECK(!layout->hasExplicitOffsets);
        CHECK(layout->numRegisters == 3);

        const ConstantBufferFieldLayout* uv   = layout->FindField("uv");
        const ConstantBufferFieldLayout* fog  = layout->FindField("fog");
        const ConstantBufferFieldLayout* tint = layout->FindField("tint");
        CHECK(uv && uv->offset == 0 && uv->size == 2);
        CHECK(fog && fog->offset == 2 && fog->size == 1);
        CHECK(tint && tint->offset == 4 && tint->size == 8 && tint->arrayStride == 4);

        bool ok = false;
        std::string glsl = GenerateGlsl(tree, GLSLGenerator::Flag_EmulateConstantBuffer, &ok);
        CHECK(ok);
        CHECK(glsl.find("uniform vec4 Params[3]") != std::string::npos);
        CHECK(glsl.find("Params[2]") != std::string::npos);
        CHECK(glsl.find("Params[0].xy") != std::string::npos);
        CHECK(glsl.find("Params[0].z") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 5. HLSL round-trip and repeated generation stability.
    // -----------------------------------------------------------------------
    {
        Allocator allocator;
        const char* source =
            "cbuffer Params {\n"
            "    float a : packoffset(c0.x);\n"
            "    float b : packoffset(c3.w);\n"
            "    float c;\n"
            "};\n"
            "float4 main(float2 p:TEXCOORD0):COLOR0 { return float4(a,b,c,1); }\n";

        HLSLTree tree(&allocator);
        CHECK(ParseShader(allocator, tree, source));

        bool hlslOk = false;
        std::string hlsl = GenerateHlsl(tree, &hlslOk);
        CHECK(hlslOk);
        CHECK(hlsl.find("packoffset(c0.x)") != std::string::npos);
        CHECK(hlsl.find("packoffset(c3.w)") != std::string::npos);

        // The generated translation unit contains engine-only helper structs
        // this parser does not re-ingest, so isolate the written cbuffer block
        // and reparse that; re-parsing must yield the identical layout.
        size_t blockStart = hlsl.find("cbuffer Params");
        size_t blockEnd   = hlsl.find("};", blockStart);
        CHECK(blockStart != std::string::npos && blockEnd != std::string::npos);
        std::string block = hlsl.substr(blockStart, blockEnd - blockStart) + "};";

        Allocator reparseAllocator;
        HLSLTree reparsed(&reparseAllocator);
        CHECK(ParseShader(reparseAllocator, reparsed, block));
        CapturedError e1, e2;
        ConstantBufferLayout* l1 = MakeLayout(allocator, tree, tree.FindBuffer("Params"), e1);
        ConstantBufferLayout* l2 = MakeLayout(reparseAllocator, reparsed, reparsed.FindBuffer("Params"), e2);
        CHECK(l1->valid && l2->valid);
        CHECK(l1->size == l2->size);
        CHECK(l1->fields.GetSize() == l2->fields.GetSize());
        for (int i = 0; i < l1->fields.GetSize(); ++i)
        {
            CHECK(l1->fields[i].offset == l2->fields[i].offset);
            CHECK(l1->fields[i].size == l2->fields[i].size);
            CHECK(l1->fields[i].arrayStride == l2->fields[i].arrayStride);
            CHECK(l1->fields[i].explicitOffset == l2->fields[i].explicitOffset);
        }

        bool g1 = false, g2 = false;
        std::string glsl1 = GenerateGlsl(tree, GLSLGenerator::Flag_EmulateConstantBuffer, &g1);
        std::string glsl2 = GenerateGlsl(tree, GLSLGenerator::Flag_EmulateConstantBuffer, &g2);
        CHECK(g1 && g2);
        CHECK(glsl1 == glsl2);
    }

    if (g_failures == 0)
    {
        std::printf("LAYOUT_CHECKS_OK (%d assertions)\n", g_checks);
        return 0;
    }
    std::printf("LAYOUT_CHECKS_FAILED (%d/%d failed)\n", g_failures, g_checks);
    return 1;
}
