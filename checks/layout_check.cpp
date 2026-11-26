//=============================================================================
//
// checks/layout_check.cpp
//
// Behavioral checks for explicit cbuffer : packoffset support.
//
//  * Queries the standalone M4::CBufferLayout result directly.
//  * Generates emulated GLSL (Flag_EmulateConstantBuffer) and evaluates the
//    *actual generated access expressions* against independently listed
//    register data using a small built-in GLSL-expression evaluator.
//  * Covers holes, reversed explicit positions, automatic continuation,
//    conflicts, cross-register/placement rejection, bad array lengths and
//    unsupported types.
//  * Verifies HLSL writeback round trips through a reparse to the same layout.
//  * Keeps an exact old-layout output comparison for buffers without any
//    packoffset.
//
// Standard C++11 plus POSIX pipe calls only; no GPU or shader SDK.
//
//=============================================================================

#include "HLSLParser.h"
#include "GLSLGenerator.h"
#include "HLSLGenerator.h"
#include "CBufferLayout.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <array>
#include <map>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#define LAYOUT_CHECK_HAVE_POSIX_STDOUT_CAPTURE 1
#endif

using namespace M4;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do                                                                         \
    {                                                                          \
        ++g_checks;                                                             \
        if (!(cond))                                                            \
        {                                                                      \
            ++g_failures;                                                       \
            printf("CHECK FAILED (%s:%d): %s\n", __FILE__, __LINE__, msg);      \
        }                                                                      \
    } while (0)

static bool Contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

static bool Nearly(float a, float b)
{
    return fabsf(a - b) <= 1e-3f;
}

//=============================================================================
// Capture stdout (diagnostics from Log_Error go to stdout in this code base).
//=============================================================================

struct StdoutCapture
{
#ifdef LAYOUT_CHECK_HAVE_POSIX_STDOUT_CAPTURE
    int  pipefd[2];
    int  savedFd;
    bool active;

    StdoutCapture() : savedFd(-1), active(false) { pipefd[0] = pipefd[1] = -1; }

    void Begin()
    {
        fflush(stdout);
        if (pipe(pipefd) != 0)
        {
            active = false;
            return;
        }
        savedFd = dup(STDOUT_FILENO);
        fflush(stdout);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
        {
            active = false;
            return;
        }
        active = true;
    }

    std::string End()
    {
        if (!active)
        {
            return std::string();
        }
        fflush(stdout);
        dup2(savedFd, STDOUT_FILENO);
        close(pipefd[1]);
        close(savedFd);

        std::string result;
        char buffer[2048];
        ssize_t count;
        while ((count = read(pipefd[0], buffer, sizeof(buffer))) > 0)
        {
            result.append(buffer, static_cast<size_t>(count));
        }
        close(pipefd[0]);
        active = false;
        return result;
    }
#else
    // Portable fallback: diagnostics are still printed by the library, the
    // check just cannot capture their text.
    void Begin() {}
    std::string End() { return std::string(); }
#endif
};

//=============================================================================
// Parse/generate helpers.
//=============================================================================

static bool ParseSource(Allocator& allocator, HLSLTree& tree, const char* source)
{
    HLSLParser parser(&allocator, "layout_check.hlsl", source, strlen(source));
    return parser.Parse(&tree);
}

static bool GenerateGLSL(HLSLTree& tree, unsigned int flags, std::string& output,
                         std::string* log = NULL)
{
    GLSLGenerator generator;
    GLSLGenerator::Options options;
    options.flags = flags;

    StdoutCapture capture;
    capture.Begin();
    bool ok = generator.Generate(&tree, GLSLGenerator::Target_FragmentShader,
                                 GLSLGenerator::Version_140, "main", options);
    std::string captured = capture.End();
    if (log != NULL)
    {
        *log = captured;
    }
    output = generator.GetResult();
    return ok;
}

static bool BuildBufferLayout(HLSLTree& tree, const char* bufferName, CBufferLayout& layout)
{
    HLSLBuffer* buffer = tree.FindBuffer(bufferName);
    if (buffer == NULL)
    {
        return false;
    }
    return layout.Build(buffer, &tree);
}

/** Extracts the single 'return <expr>;' expression of a named no-arg function
 * as emitted by the generator. */
static bool ExtractReturnExpression(const std::string& glsl, const char* functionName,
                                    std::string& expression)
{
    std::string signature = std::string(functionName) + "() {";
    size_t pos = glsl.find(signature);
    if (pos == std::string::npos)
    {
        // Allow arbitrary whitespace before the opening brace.
        signature = std::string(functionName) + "()";
        pos = glsl.find(signature);
        if (pos == std::string::npos)
        {
            return false;
        }
        pos = glsl.find('{', pos);
        if (pos == std::string::npos)
        {
            return false;
        }
    }

    size_t returnPos = glsl.find("return ", pos);
    if (returnPos == std::string::npos)
    {
        return false;
    }
    returnPos += 7;

    size_t endPos = glsl.find(';', returnPos);
    if (endPos == std::string::npos)
    {
        return false;
    }

    expression = glsl.substr(returnPos, endPos - returnPos);
    return true;
}

//=============================================================================
// Tiny evaluator for the subset of GLSL emitted by the buffer access code.
//=============================================================================

struct EValue
{
    enum Kind
    {
        Number,
        Vector,
        Aggregate,
        Matrix,
        BufferRef
    };

    Kind                                   kind;
    std::vector<float>                     components;
    std::vector<EValue>                    items;
    std::string                            bufferName;

    EValue() : kind(Number) {}

    static EValue Num(float x)
    {
        EValue v;
        v.kind = Number;
        v.components.push_back(x);
        return v;
    }

    static EValue Vec(const std::vector<float>& x)
    {
        EValue v;
        v.kind = Vector;
        v.components = x;
        return v;
    }

    float AsScalar() const
    {
        if ((kind == Number || kind == Vector) && !components.empty())
        {
            return components[0];
        }
        throw std::string("expected scalar value");
    }
};

typedef std::array<float, 4> Register;
typedef std::map<std::string, std::vector<Register> > BufferBank;

class ExpressionEvaluator
{
public:
    ExpressionEvaluator(const std::string& text, const BufferBank& buffers,
                        const std::map<std::string, float>& variables)
        : m_text(text), m_pos(0), m_buffers(buffers), m_variables(variables)
    {
    }

    EValue Run()
    {
        SkipSpaces();
        EValue result = ParseExpression();
        SkipSpaces();
        if (m_pos != m_text.size())
        {
            throw std::string("unexpected trailing characters in '") + m_text + "'";
        }
        return result;
    }

    static EValue Eval(const std::string& text, const BufferBank& buffers,
                       const std::map<std::string, float>& variables)
    {
        ExpressionEvaluator evaluator(text, buffers, variables);
        return evaluator.Run();
    }

private:
    const std::string&             m_text;
    size_t                         m_pos;
    const BufferBank&              m_buffers;
    const std::map<std::string, float>& m_variables;

    void SkipSpaces()
    {
        while (m_pos < m_text.size() &&
               (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' ||
                m_text[m_pos] == '\n' || m_text[m_pos] == '\r'))
        {
            ++m_pos;
        }
    }

    bool Consume(char c)
    {
        SkipSpaces();
        if (m_pos < m_text.size() && m_text[m_pos] == c)
        {
            ++m_pos;
            return true;
        }
        return false;
    }

    void Expect(char c)
    {
        if (!Consume(c))
        {
            throw std::string("expected '") + c + "' in '" + m_text + "'";
        }
    }

    std::string ParseIdentifier()
    {
        SkipSpaces();
        size_t start = m_pos;
        if (m_pos < m_text.size() &&
            (isalpha(static_cast<unsigned char>(m_text[m_pos])) || m_text[m_pos] == '_'))
        {
            ++m_pos;
            while (m_pos < m_text.size() &&
                   (isalnum(static_cast<unsigned char>(m_text[m_pos])) || m_text[m_pos] == '_'))
            {
                ++m_pos;
            }
        }
        return m_text.substr(start, m_pos - start);
    }

    float ParseNumber()
    {
        SkipSpaces();
        const char* start = m_text.c_str() + m_pos;
        char* end = NULL;
        double value = strtod(start, &end);
        if (end == start)
        {
            throw std::string("expected number in '") + m_text + "'";
        }
        m_pos = static_cast<size_t>(end - m_text.c_str());
        // Skip an optional float suffix such as 'f'.
        SkipSpaces();
        if (m_pos < m_text.size() && (m_text[m_pos] == 'f' || m_text[m_pos] == 'F'))
        {
            ++m_pos;
        }
        return static_cast<float>(value);
    }

    EValue ParseExpression()
    {
        EValue left = ParseTerm();
        for (;;)
        {
            SkipSpaces();
            char op = 0;
            if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-'))
            {
                op = m_text[m_pos++];
            }
            else
            {
                break;
            }
            EValue right = ParseTerm();
            left = ApplyBinary(op, left, right);
        }
        return left;
    }

    EValue ParseTerm()
    {
        EValue left = ParseUnary();
        for (;;)
        {
            SkipSpaces();
            char op = 0;
            if (m_pos < m_text.size() && (m_text[m_pos] == '*' || m_text[m_pos] == '/'))
            {
                op = m_text[m_pos++];
            }
            else
            {
                break;
            }
            EValue right = ParseUnary();
            left = ApplyBinary(op, left, right);
        }
        return left;
    }

    EValue ParseUnary()
    {
        SkipSpaces();
        if (m_pos < m_text.size() && m_text[m_pos] == '-')
        {
            ++m_pos;
            EValue value = ParseUnary();
            for (size_t i = 0; i < value.components.size(); ++i)
            {
                value.components[i] = -value.components[i];
            }
            return value;
        }
        if (m_pos < m_text.size() && m_text[m_pos] == '+')
        {
            ++m_pos;
            return ParseUnary();
        }
        return ParsePostfix();
    }

    EValue ParsePostfix()
    {
        EValue value = ParsePrimary();

        for (;;)
        {
            SkipSpaces();
            if (m_pos < m_text.size() && m_text[m_pos] == '[')
            {
                ++m_pos;
                EValue indexValue = ParseExpression();
                Expect(']');
                int index = static_cast<int>(indexValue.AsScalar() + 0.001f);

                if (value.kind == EValue::BufferRef)
                {
                    BufferBank::const_iterator it = m_buffers.find(value.bufferName);
                    if (it == m_buffers.end() || index < 0 ||
                        index >= static_cast<int>(it->second.size()))
                    {
                        throw std::string("uniform register index out of range for ") +
                              value.bufferName;
                    }
                    const Register& reg = it->second[index];
                    std::vector<float> components(reg.begin(), reg.end());
                    value = EValue::Vec(components);
                }
                else if (value.kind == EValue::Vector || value.kind == EValue::Number)
                {
                    if (index < 0 || index >= static_cast<int>(value.components.size()))
                    {
                        throw std::string("vector component index out of range");
                    }
                    value = EValue::Num(value.components[index]);
                }
                else if (value.kind == EValue::Matrix)
                {
                    if (index < 0 || index >= static_cast<int>(value.items.size()))
                    {
                        throw std::string("matrix column index out of range");
                    }
                    value = value.items[index];
                }
                else
                {
                    throw std::string("index operation on unsupported value");
                }
            }
            else if (m_pos < m_text.size() && m_text[m_pos] == '.')
            {
                ++m_pos;
                std::string swizzle = ParseIdentifier();
                if (value.kind != EValue::Vector && value.kind != EValue::Number)
                {
                    throw std::string("swizzle on non-vector value");
                }
                std::vector<float> out;
                for (size_t i = 0; i < swizzle.size(); ++i)
                {
                    int component = SwizzleIndex(swizzle[i]);
                    if (component < 0 || component >= static_cast<int>(value.components.size()))
                    {
                        throw std::string("swizzle component out of range");
                    }
                    out.push_back(value.components[component]);
                }
                value = (out.size() == 1) ? EValue::Num(out[0]) : EValue::Vec(out);
            }
            else
            {
                break;
            }
        }

        return value;
    }

    static int SwizzleIndex(char c)
    {
        if (c == 'x' || c == 'r') return 0;
        if (c == 'y' || c == 'g') return 1;
        if (c == 'z' || c == 'b') return 2;
        if (c == 'w' || c == 'a') return 3;
        return -1;
    }

    EValue ParsePrimary()
    {
        SkipSpaces();
        if (m_pos >= m_text.size())
        {
            throw std::string("unexpected end of expression");
        }

        if (m_text[m_pos] == '(')
        {
            ++m_pos;
            EValue value = ParseExpression();
            Expect(')');
            return value;
        }

        if (isdigit(static_cast<unsigned char>(m_text[m_pos])) || m_text[m_pos] == '.')
        {
            return EValue::Num(ParseNumber());
        }

        std::string identifier = ParseIdentifier();
        if (identifier.empty())
        {
            throw std::string("expected identifier in '") + m_text + "'";
        }

        SkipSpaces();
        if (m_pos < m_text.size() && m_text[m_pos] == '(')
        {
            return ParseCall(identifier);
        }

        std::map<std::string, float>::const_iterator vit = m_variables.find(identifier);
        if (vit != m_variables.end())
        {
            return EValue::Num(vit->second);
        }

        if (m_buffers.find(identifier) != m_buffers.end())
        {
            EValue value;
            value.kind = EValue::BufferRef;
            value.bufferName = identifier;
            return value;
        }

        throw std::string("unknown identifier '") + identifier + "'";
    }

    std::vector<EValue> ParseArguments()
    {
        std::vector<EValue> arguments;
        Expect('(');
        SkipSpaces();
        if (m_pos < m_text.size() && m_text[m_pos] == ')')
        {
            ++m_pos;
            return arguments;
        }
        for (;;)
        {
            arguments.push_back(ParseExpression());
            SkipSpaces();
            if (Consume(','))
            {
                continue;
            }
            Expect(')');
            break;
        }
        return arguments;
    }

    EValue ParseCall(const std::string& name)
    {
        // Current token is '('.
        std::vector<EValue> args = ParseArguments();

        if (name == "float" || name == "int" || name == "uint" || name == "bool")
        {
            if (args.empty()) throw std::string("scalar ctor with no args");
            return EValue::Num(args[0].AsScalar());
        }
        if (name == "vec2" || name == "vec3" || name == "vec4")
        {
            int wanted = name[3] - '0';
            std::vector<float> flat = FlattenComponents(args);
            if (flat.size() == 1 && wanted > 1)
            {
                return EValue::Vec(std::vector<float>(wanted, flat[0]));
            }
            if (static_cast<int>(flat.size()) != wanted)
            {
                throw std::string("constructor component count mismatch for ") + name;
            }
            return EValue::Vec(flat);
        }
        if (name == "mat4")
        {
            EValue matrix;
            matrix.kind = EValue::Matrix;
            for (size_t i = 0; i < args.size(); ++i)
            {
                matrix.items.push_back(args[i]);
            }
            return matrix;
        }
        if (name.compare(0, 11, "matrix_row0") == 0 ||
            name.compare(0, 10, "matrix_row") == 0)
        {
            // Emitted helper: matrix_rowN(mat, row) returns the HLSL row as a
            // GLSL vector by reading the matching component of each column.
            if (args.size() != 2 || args[0].kind != EValue::Matrix)
            {
                throw std::string("bad matrix_row call");
            }
            int row = static_cast<int>(args[1].AsScalar() + 0.001f);
            std::vector<float> rowComponents;
            for (size_t col = 0; col < args[0].items.size(); ++col)
            {
                if (row < 0 || row >= static_cast<int>(args[0].items[col].components.size()))
                {
                    throw std::string("matrix row out of range");
                }
                rowComponents.push_back(args[0].items[col].components[row]);
            }
            return EValue::Vec(rowComponents);
        }

        // Any other constructor is a struct constructor; preserve arguments
        // positionally as aggregate fields.
        EValue aggregate;
        aggregate.kind = EValue::Aggregate;
        aggregate.items = args;
        return aggregate;
    }

    static std::vector<float> FlattenComponents(const std::vector<EValue>& args)
    {
        std::vector<float> result;
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (args[i].kind == EValue::Number || args[i].kind == EValue::Vector)
            {
                for (size_t j = 0; j < args[i].components.size(); ++j)
                {
                    result.push_back(args[i].components[j]);
                }
            }
            else
            {
                throw std::string("expected numeric constructor argument");
            }
        }
        return result;
    }

    static EValue ApplyBinary(char op, const EValue& left, const EValue& right)
    {
        if (left.kind == EValue::Number || left.kind == EValue::Vector)
        {
            std::vector<float> a = left.components;
            std::vector<float> b = right.components;
            if (a.size() == 1 && b.size() > 1)
            {
                a = std::vector<float>(b.size(), a[0]);
            }
            if (b.size() == 1 && a.size() > 1)
            {
                b = std::vector<float>(a.size(), b[0]);
            }
            if (a.size() != b.size())
            {
                throw std::string("component count mismatch in binary op");
            }
            std::vector<float> out(a.size());
            for (size_t i = 0; i < a.size(); ++i)
            {
            if (op == '+') out[i] = a[i] + b[i];
            else if (op == '-') out[i] = a[i] - b[i];
            else if (op == '*') out[i] = a[i] * b[i];
            else if (op == '/') out[i] = a[i] / b[i];
            }
            if (out.size() == 1)
            {
                return EValue::Num(out[0]);
            }
            return EValue::Vec(out);
        }
        throw std::string("binary operation on unsupported values");
    }
};

static bool CheckComponents(const EValue& value, const float* expected, int count,
                            const char* label)
{
    if (value.kind != EValue::Number && value.kind != EValue::Vector)
    {
        printf("  [%s] expected numeric vector, got kind %d\n", label, (int)value.kind);
        return false;
    }
    if (static_cast<int>(value.components.size()) != count)
    {
        printf("  [%s] expected %d components, got %d\n", label, count,
               static_cast<int>(value.components.size()));
        return false;
    }
    for (int i = 0; i < count; ++i)
    {
        if (!Nearly(value.components[i], expected[i]))
        {
            printf("  [%s] component %d: generated value %.6g != independently listed %.6g\n",
                   label, i, value.components[i], expected[i]);
            return false;
        }
    }
    return true;
}

//=============================================================================
// The shaders under test.
//=============================================================================

static const char* kMainShader =
    "struct Particle { float3 pos; float mass; float4 color; };"
    "cbuffer L {"
    " float a : packoffset(c4.x);"
    " float b;"
    " float4 v;"
    " float2 hole : packoffset(c1.y);"
    " float afterHole;"
    " float2 auto2;"
    " float back : packoffset(c1.w);"
    "};"
    "cbuffer Data {"
    " float4x4 M : packoffset(c0);"
    " Particle parts[3] : packoffset(c5);"
    " Particle one : packoffset(c12);"
    "};"
    "float4 main() : COLOR0 {"
    " return float4(a + b + v.x + hole.x + afterHole + auto2.x + back, 0, 0, 1);"
    "}"
    "float4 probeStatic() : COLOR0 {"
    " return float4(parts[0].pos.x, parts[1].mass, parts[2].color.z, one.color.w);"
    "}"
    "float4 probeAt1() : COLOR0 {"
    " return float4(parts[1].pos.x, parts[1].mass, parts[1].color.y, parts[1].color.z);"
    "}"
    "float4 probeDyn() : COLOR0 {"
    " int idx = 1;"
    " return float4(parts[idx].pos.x, parts[idx].mass, parts[idx].color.y, parts[idx].color.z);"
    "}"
    "float4 probeDynNext() : COLOR0 {"
    " int idx = 1;"
    " return float4(parts[idx+1].pos.x, parts[idx+1].mass, parts[idx+1].color.y, parts[idx+1].color.z);"
    "}"
    "Particle probeWhole() {"
    " return parts[2];"
    "}"
    "Particle probeWholeOne() {"
    " return one;"
    "}"
    "float4 probeMatrix() : COLOR0 {"
    " return float4(M[0][0], M[1][2], M[2][1], M[3][3]);"
    "}";

// Exact pre-packoffset behavior, used as the old-output comparison.
static const char* kOldShader =
    "struct Light { float3 pos; float4 color; };"
    "cbuffer B { float a; float2 b; float4x4 m; Light lights[2]; Light whole; float auto1; };"
    "float4 main(float2 p:TEXCOORD0):COLOR0 {"
    " Light L = lights[1]; int i = 0;"
    " return float4(a + b.x + m[0][0] + lights[0].pos.x + L.color.y + whole.pos.z +"
    " lights[i].color.z + auto1, 0,0,1);"
    "}";

int main()
{
    //------------------------------------------------------------------------
    // 1. Direct C++ layout query: holes / reversed order / auto continuation.
    //------------------------------------------------------------------------
    {
        Allocator allocator;
        HLSLTree tree(&allocator);
        CHECK(ParseSource(allocator, tree, kMainShader), "main shader parses");

        CBufferLayout layoutL;
        CHECK(BuildBufferLayout(tree, "L", layoutL), "L layout builds");
        CHECK(layoutL.HasExplicitOffsets(), "L reports explicit offsets");
        CHECK(layoutL.GetFieldCount() == 7, "L has 7 fields");
        CHECK(layoutL.GetOccupiedEnd() == 27, "L occupied high-water end is 27");
        CHECK(layoutL.GetRegisterCount() == 7, "L rounds size up to 7 vec4 registers");

        const CBufferLayout::Field* a = layoutL.FindField("a");
        const CBufferLayout::Field* b = layoutL.FindField("b");
        const CBufferLayout::Field* v = layoutL.FindField("v");
        const CBufferLayout::Field* hole = layoutL.FindField("hole");
        const CBufferLayout::Field* afterHole = layoutL.FindField("afterHole");
        const CBufferLayout::Field* auto2 = layoutL.FindField("auto2");
        const CBufferLayout::Field* back = layoutL.FindField("back");

        CHECK(a && a->offset == 16 && a->size == 1 && a->explicitOffset, "a at c4.x");
        CHECK(b && b->offset == 17 && b->size == 1 && !b->explicitOffset, "auto b continues at c4.y");
        CHECK(v && v->offset == 20 && v->size == 4, "auto vec4 v aligns to c5.x");
        CHECK(hole && hole->offset == 5 && hole->size == 2 && hole->explicitOffset,
              "reversed hole at c1.y");
        CHECK(afterHole && afterHole->offset == 24, "auto afterHole stays at high cursor c6.x");
        CHECK(auto2 && auto2->offset == 25 && auto2->size == 2, "auto2 at c6.y");
        CHECK(back && back->offset == 7 && back->size == 1, "later explicit back fills c1.w");

        CBufferLayout layoutData;
        CHECK(BuildBufferLayout(tree, "Data", layoutData), "Data layout builds");
        CHECK(layoutData.GetRegisterCount() == 14, "Data has 14 vec4 registers");
        const CBufferLayout::Field* M = layoutData.FindField("M");
        const CBufferLayout::Field* parts = layoutData.FindField("parts");
        const CBufferLayout::Field* one = layoutData.FindField("one");
        CHECK(M && M->offset == 0 && M->size == 16, "matrix M is 16 floats at c0");
        CHECK(parts && parts->offset == 20 && parts->size == 24 &&
              parts->arrayStride == 8 && parts->arrayCount == 3,
              "parts[3] at c5 with 8-float stride");
        CHECK(one && one->offset == 48 && one->size == 8, "struct one at c12 with padded size 8");
    }

    //------------------------------------------------------------------------
    // 2. Generate emulated GLSL and evaluate the actual expressions against
    //    independently listed register data.
    //------------------------------------------------------------------------
    std::string glsl;
    {
        Allocator allocator;
        HLSLTree tree(&allocator);
        CHECK(ParseSource(allocator, tree, kMainShader), "main shader parses for generation");
        bool ok = GenerateGLSL(tree, GLSLGenerator::Flag_EmulateConstantBuffer, glsl);
        CHECK(ok, "emulated GLSL generation succeeds");
        CHECK(Contains(glsl, "uniform vec4 L[7];"), "generated L length uses layout result");
        CHECK(Contains(glsl, "uniform vec4 Data[14];"), "generated Data length uses layout result");
    }

    // Independently listed register bank (hand-authored, asymmetric values).
    BufferBank bank;
    {
        static const float kL[7][4] = {
            {  1,  2,  3,  4 }, // c0
            { 10, 11, 12, 13 }, // c1: hole.y=11, back.w=13
            { 20, 21, 22, 23 }, // c2
            { 30, 31, 32, 33 }, // c3
            { 40, 41, 42, 43 }, // c4: a=40, b=41
            { 50, 51, 52, 53 }, // c5: v.x=50
            { 60, 61, 62, 63 }  // c6: afterHole=60, auto2.y-start=61
        };
        std::vector<Register> regs;
        for (int i = 0; i < 7; ++i)
        {
            Register r = {{ kL[i][0], kL[i][1], kL[i][2], kL[i][3] }};
            regs.push_back(r);
        }
        bank["L"] = regs;
    }
    {
        static const float kData[14][4] = {
            {   1,   2,   3,   4 }, // c0 M column 0
            {   5,   6,   7,   8 }, // c1 M column 1
            {   9,  10,  11,  12 }, // c2 M column 2
            {  13,  14,  15,  16 }, // c3 M column 3
            {  41,  42,  43,  44 }, // c4 unused sentinel
            { 101, 102, 103, 104 }, // c5 parts[0].pos + mass
            { 105, 106, 107, 108 }, // c6 parts[0].color
            { 201, 202, 203, 204 }, // c7 parts[1].pos + mass
            { 205, 206, 207, 208 }, // c8 parts[1].color
            { 301, 302, 303, 304 }, // c9 parts[2].pos + mass
            { 305, 306, 307, 308 }, // c10 parts[2].color
            { 901, 902, 903, 904 }, // c11 unused sentinel
            { 401, 402, 403, 404 }, // c12 one.pos + mass
            { 405, 406, 407, 408 }  // c13 one.color
        };
        std::vector<Register> regs;
        for (int i = 0; i < 14; ++i)
        {
            Register r = {{ kData[i][0], kData[i][1], kData[i][2], kData[i][3] }};
            regs.push_back(r);
        }
        bank["Data"] = regs;
    }

    std::map<std::string, float> noVars;
    std::map<std::string, float> idx1;
    idx1["idx"] = 1.0f;

    try
    {
        std::string expr;

        // Hole/reversed/auto sum: 40+41+50+11+60+61+13 = 276.
        CHECK(ExtractReturnExpression(glsl, "main", expr), "extract main return");
        EValue mainValue = ExpressionEvaluator::Eval(expr, bank, noVars);
        float expectedMain[4] = { 276.0f, 0.0f, 0.0f, 1.0f };
        CHECK(CheckComponents(mainValue, expectedMain, 4, "holes+auto sum"),
              "main expression evaluates to independently listed values");

        CHECK(ExtractReturnExpression(glsl, "probeStatic", expr), "extract probeStatic");
        EValue staticValue = ExpressionEvaluator::Eval(expr, bank, noVars);
        float expectedStatic[4] = { 101.0f, 204.0f, 307.0f, 408.0f };
        CHECK(CheckComponents(staticValue, expectedStatic, 4, "static nested fields"),
              "static struct array reads match register listing");

        CHECK(ExtractReturnExpression(glsl, "probeAt1", expr), "extract probeAt1");
        EValue at1Value = ExpressionEvaluator::Eval(expr, bank, noVars);
        float expectedAt1[4] = { 201.0f, 204.0f, 206.0f, 207.0f };
        CHECK(CheckComponents(at1Value, expectedAt1, 4, "literal index 1"),
              "literal parts[1] reads match register listing");

        CHECK(ExtractReturnExpression(glsl, "probeDyn", expr), "extract probeDyn");
        EValue dynValue = ExpressionEvaluator::Eval(expr, bank, idx1);
        CHECK(CheckComponents(dynValue, expectedAt1, 4, "dynamic index idx=1"),
              "dynamic struct array index lands on the same address as literal 1");

        CHECK(ExtractReturnExpression(glsl, "probeDynNext", expr), "extract probeDynNext");
        EValue dynNextValue = ExpressionEvaluator::Eval(expr, bank, idx1);
        float expectedNext[4] = { 301.0f, 304.0f, 306.0f, 307.0f };
        CHECK(CheckComponents(dynNextValue, expectedNext, 4, "dynamic idx+1=2"),
              "pure integer idx+1 expression lands on parts[2]");
        CHECK(Contains(glsl, "2*("), "dynamic array index is emitted as a stride-scaled expression");

        // Matrix reads through the generated matrix_row helper.
        CHECK(ExtractReturnExpression(glsl, "probeMatrix", expr), "extract probeMatrix");
        EValue matrixValue = ExpressionEvaluator::Eval(expr, bank, noVars);
        float expectedMatrix[4] = { 1.0f, 10.0f, 7.0f, 16.0f };
        CHECK(CheckComponents(matrixValue, expectedMatrix, 4, "matrix asymmetric reads"),
              "matrix reads match independently listed column-vector registers");

        // Whole struct read evaluates to the same leaves.
        CHECK(ExtractReturnExpression(glsl, "probeWhole", expr), "extract probeWhole");
        EValue whole = ExpressionEvaluator::Eval(expr, bank, noVars);
        CHECK(whole.kind == EValue::Aggregate && whole.items.size() == 3,
              "whole parts[2] is a 3-field aggregate");
        if (whole.kind == EValue::Aggregate && whole.items.size() == 3)
        {
            float expectedPos[3] = { 301, 302, 303 };
            CHECK(CheckComponents(whole.items[0], expectedPos, 3, "whole pos"),
                  "whole struct pos matches");
            float expectedMass[1] = { 304 };
            CHECK(CheckComponents(whole.items[1], expectedMass, 1, "whole mass"),
                  "whole struct mass matches");
            float expectedColor[4] = { 305, 306, 307, 308 };
            CHECK(CheckComponents(whole.items[2], expectedColor, 4, "whole color"),
                  "whole struct color matches");
        }
        // The whole-struct expression must only touch registers 9 and 10.
        CHECK(Contains(expr, "Data[9]") && Contains(expr, "Data[10]") &&
              !Contains(expr, "Data[5]") && !Contains(expr, "Data[11]"),
              "whole parts[2] addresses c9/c10 only");

        CHECK(ExtractReturnExpression(glsl, "probeWholeOne", expr), "extract probeWholeOne");
        EValue wholeOne = ExpressionEvaluator::Eval(expr, bank, noVars);
        CHECK(wholeOne.kind == EValue::Aggregate && wholeOne.items.size() == 3,
              "whole one is a 3-field aggregate");
        if (wholeOne.kind == EValue::Aggregate && wholeOne.items.size() == 3)
        {
            float expectedPos[3] = { 401, 402, 403 };
            float expectedMass[1] = { 404 };
            float expectedColor[4] = { 405, 406, 407, 408 };
            CHECK(CheckComponents(wholeOne.items[0], expectedPos, 3, "wholeOne pos"), "");
            CHECK(CheckComponents(wholeOne.items[1], expectedMass, 1, "wholeOne mass"), "");
            CHECK(CheckComponents(wholeOne.items[2], expectedColor, 4, "wholeOne color"), "");
        }
    }
    catch (const std::string& error)
    {
        CHECK(false, error.c_str());
    }

    //------------------------------------------------------------------------
    // 2b. Nested struct padding and a scalar array with dynamic indices.
    //------------------------------------------------------------------------
    static const char* kNestedShader =
        "struct Inner { float a; float2 b; };"
        "struct Outer { float x; Inner mid; float y; };"
        "cbuffer N { Outer o : packoffset(c2); float vals[3] : packoffset(c5); };"
        "float4 pNest() : COLOR0 { return float4(o.x, o.mid.a, o.mid.b.x, o.y); }"
        "float4 pVals() : COLOR0 { int i = 1; return float4(vals[0], vals[i], vals[2], vals[i+1]); }"
        "float4 main() : COLOR0 { return pNest(); }";

    {
        Allocator allocator;
        HLSLTree tree(&allocator);
        CHECK(ParseSource(allocator, tree, kNestedShader), "nested shader parses");

        CBufferLayout layoutN;
        CHECK(BuildBufferLayout(tree, "N", layoutN), "nested layout builds");
        const CBufferLayout::Field* o = layoutN.FindField("o");
        const CBufferLayout::Field* vals = layoutN.FindField("vals");
        CHECK(o && o->offset == 8 && o->size == 12, "nested Outer reserves padded 12 floats at c2");
        CHECK(vals && vals->offset == 20 && vals->arrayStride == 4 && vals->arrayCount == 3,
              "scalar vals[3] at c5 with one-register stride");
        CHECK(layoutN.GetRegisterCount() == 8, "nested buffer size is c8");

        std::string nestedGlsl;
        bool ok = GenerateGLSL(tree, GLSLGenerator::Flag_EmulateConstantBuffer, nestedGlsl);
        CHECK(ok, "nested shader generates");

        BufferBank nbank;
        static const float kN[8][4] = {
            {0,0,0,0}, {0,0,0,0},
            { 1,  2,  3,  4 },  // c2 o.x=1
            { 10, 11, 12, 13 }, // c3 o.mid.a=10, b=(11,12)
            { 20, 21, 22, 23 }, // c4 o.y=20
            { 100, 0, 0, 0 },   // c5 vals[0]
            { 200, 0, 0, 0 },   // c6 vals[1]
            { 300, 0, 0, 0 }    // c7 vals[2]
        };
        std::vector<Register> regs;
        for (int i = 0; i < 8; ++i)
        {
            Register r = {{ kN[i][0], kN[i][1], kN[i][2], kN[i][3] }};
            regs.push_back(r);
        }
        nbank["N"] = regs;

        std::string expr;
        try
        {
            CHECK(ExtractReturnExpression(nestedGlsl, "pNest", expr), "extract pNest");
            EValue nestValue = ExpressionEvaluator::Eval(expr, nbank, noVars);
            float expectedNest[4] = { 1.0f, 10.0f, 11.0f, 20.0f };
            CHECK(CheckComponents(nestValue, expectedNest, 4, "nested fields"),
                  "nested struct member addresses match independent register data");

            std::map<std::string, float> iVar;
            iVar["i"] = 1.0f;
            CHECK(ExtractReturnExpression(nestedGlsl, "pVals", expr), "extract pVals");
            EValue valsValue = ExpressionEvaluator::Eval(expr, nbank, iVar);
            float expectedVals[4] = { 100.0f, 200.0f, 300.0f, 300.0f };
            CHECK(CheckComponents(valsValue, expectedVals, 4, "scalar array dynamic"),
                  "scalar array static/dynamic indices land on the same registers");
        }
        catch (const std::string& error)
        {
            CHECK(false, error.c_str());
        }
    }

    //------------------------------------------------------------------------
    // 3. Diagnostics: conflicts name both fields/positions; rejection cases.
    //------------------------------------------------------------------------
    struct RejectCase
    {
        const char* name;
        const char* bufferName;
        const char* source;
        const char* mustContain[4];
    };

    static const RejectCase rejectCases[] = {
        {
            "overlap", "C",
            "cbuffer C { float a : packoffset(c2.x); float4 b : packoffset(c2); };",
            { "a", "b", "conflict", "c2" }
        },
        {
            "aggregate padding conflict", "P",
            "struct S { float x; };"
            "cbuffer P { S s : packoffset(c0); float q : packoffset(c0.y); };",
            { "s", "q", "conflict" }
        },
        {
            "vector crosses register", "X",
            "cbuffer X { float2 v : packoffset(c3.w); };",
            { "v", "crosses a register" }
        },
        {
            "float3 crosses register", "X3",
            "cbuffer X3 { float3 v : packoffset(c2.z); };",
            { "v", "crosses a register" }
        },
        {
            "array not at x", "A",
            "cbuffer A { float4 arr[2] : packoffset(c2.y); };",
            { "arr", "start of a register" }
        },
        {
            "matrix not at x", "M",
            "cbuffer M { float4x4 mat : packoffset(c1.z); };",
            { "mat", "start of a register" }
        },
        {
            "struct not at x", "S",
            "struct Q { float x; };"
            "cbuffer S { Q q : packoffset(c3.y); };",
            { "q", "start of a register" }
        },
        {
            "illegal component", "IC",
            "cbuffer IC { float x : packoffset(c2.q); };",
            { "x", "illegal component" }
        },
        {
            "multi component", "MC",
            "cbuffer MC { float2 x : packoffset(c2.xy); };",
            { "x", "illegal component" }
        },
        {
            "register out of range", "BIG",
            "cbuffer BIG { float x : packoffset(c999999999999); };",
            { "x", "out of range" }
        },
        {
            "missing register digits", "NOD",
            "cbuffer NOD { float x : packoffset(c); };",
            { "x", "invalid" }
        },
        {
            "zero array length", "Z",
            "cbuffer Z { float a[0]; };",
            { "positive" }
        },
        {
            "negative array length", "N",
            "cbuffer N { float a[-2]; };",
            { "positive" }
        },
        {
            "unsupported field type", "U",
            "cbuffer U { int3 q : packoffset(c0); };",
            { "q", "not supported" }
        }
    };

    for (size_t caseIndex = 0;
         caseIndex < sizeof(rejectCases) / sizeof(rejectCases[0]);
         ++caseIndex)
    {
        const RejectCase& testCase = rejectCases[caseIndex];
        Allocator allocator;
        HLSLTree tree(&allocator);
        bool parsed = ParseSource(allocator, tree, testCase.source);

        CBufferLayout layout;
        bool layoutOk = false;
        if (parsed)
        {
            layoutOk = BuildBufferLayout(tree, testCase.bufferName, layout);
        }

        char label[256];
        String_Printf(label, sizeof(label), "reject case '%s' produces a layout diagnostic",
                      testCase.name);
        CHECK(parsed && !layoutOk && !layout.GetError().empty(), label);

        for (int tokenIndex = 0; tokenIndex < 4; ++tokenIndex)
        {
            const char* token = testCase.mustContain[tokenIndex];
            if (token == NULL)
            {
                break;
            }
            String_Printf(label, sizeof(label), "case '%s' diagnostic contains '%s' (got: %s)",
                          testCase.name, token, layout.GetError().c_str());
            CHECK(Contains(layout.GetError(), token), label);
        }

        // End to end: emulated generation must fail and emit no result that
        // could be misused.
        if (parsed)
        {
            std::string output;
            bool generated = GenerateGLSL(tree, GLSLGenerator::Flag_EmulateConstantBuffer, output);
            String_Printf(label, sizeof(label), "case '%s' makes GLSL generation fail cleanly",
                          testCase.name);
            CHECK(!generated, label);
            CHECK(output.empty(), "invalid layout emits no GLSL result");
        }
    }

    // Malformed annotation syntax is rejected by the parser itself without
    // assertions/crashes.
    {
        Allocator allocator;
        HLSLTree tree(&allocator);
        bool parsed = ParseSource(allocator, tree,
                                  "cbuffer BAD { float a : packoffset(c2-1); }; float4 main():COLOR0 { return 0; }");
        CHECK(!parsed, "malformed packoffset register is a parse diagnostic");
    }

    // An overlong register number must be truncated safely by the tokenizer
    // and reported as an out-of-range layout error, never overflow a buffer.
    {
        std::string huge = "cbuffer HUGE { float x : packoffset(c";
        huge.append(300, '9');
        huge.append("); }; float4 main():COLOR0 { return x; }");

        Allocator allocator;
        HLSLTree tree(&allocator);
        bool parsed = ParseSource(allocator, tree, huge.c_str());
        CHECK(parsed, "overlong register token is truncated and still parses structurally");

        CBufferLayout layout;
        bool ok = parsed && BuildBufferLayout(tree, "HUGE", layout);
        CHECK(!ok && Contains(layout.GetError(), "out of range"),
              "overlong register is an out-of-range diagnostic");

        std::string output;
        bool generated = parsed &&
            GenerateGLSL(tree, GLSLGenerator::Flag_EmulateConstantBuffer, output);
        CHECK(!generated && output.empty(), "overlong register generates no usable GLSL");
    }

    //------------------------------------------------------------------------
    // 4. Explicit packoffset without emulation must be clearly rejected.
    //------------------------------------------------------------------------
    {
        Allocator allocator;
        HLSLTree tree(&allocator);
        CHECK(ParseSource(allocator, tree,
                          "cbuffer E { float a : packoffset(c0); };"
                          "float4 main():COLOR0 { return a; }"),
              "no-emulation case parses");

        std::string output;
        std::string log;
        bool ok = GenerateGLSL(tree, /*flags=*/0, output, &log);
        CHECK(!ok, "explicit packoffset rejected when emulation flag is off");
        CHECK(output.empty(), "rejected non-emulated output is empty");
#ifdef LAYOUT_CHECK_HAVE_POSIX_STDOUT_CAPTURE
        CHECK(Contains(log, "packoffset") && Contains(log, "Flag_EmulateConstantBuffer"),
              "non-emulated rejection names packoffset and Flag_EmulateConstantBuffer");
#else
        (void)log;
#endif

        // The ordinary no-packoffset GLSL path still generates fine without
        // the flag (std140 block).
        Allocator allocator2;
        HLSLTree tree2(&allocator2);
        CHECK(ParseSource(allocator2, tree2,
                          "cbuffer Plain { float a; float4 b; };"
                          "float4 main():COLOR0 { return b + a; }"),
              "plain cbuffer parses");
        std::string plain;
        bool plainOk = GenerateGLSL(tree2, /*flags=*/0, plain);
        CHECK(plainOk, "plain non-emulated GLSL still generates");
        CHECK(Contains(plain, "layout (std140) uniform Plain"),
              "plain path keeps std140 uniform block");

        // Types that only the std140 declaration path supports must keep
        // generating when emulation (and therefore float layout) is off.
        Allocator allocator3;
        HLSLTree tree3(&allocator3);
        CHECK(ParseSource(allocator3, tree3,
                          "cbuffer Typed { int count; bool flag; float3 pos; };"
                          "float4 main():COLOR0 { return float4(pos, count) + (flag ? 1 : 0); }"),
              "typed plain cbuffer parses");
        std::string typedGlsl;
        bool typedOk = GenerateGLSL(tree3, /*flags=*/0, typedGlsl);
        CHECK(typedOk, "plain int/bool std140 cbuffer still generates");
        CHECK(Contains(typedGlsl, "layout (std140) uniform Typed"),
              "typed plain cbuffer keeps std140 block");
    }

    //------------------------------------------------------------------------
    // 5. HLSL writer preserves packoffset; reparsing gives the same layout.
    //------------------------------------------------------------------------
    {
        Allocator allocator;
        HLSLTree tree(&allocator);
        CHECK(ParseSource(allocator, tree, kMainShader), "roundtrip shader parses");

        HLSLGenerator hlslGenerator;
        bool hlslOk = hlslGenerator.Generate(&tree, HLSLGenerator::Target_PixelShader,
                                             "main", /*legacy=*/false);
        CHECK(hlslOk, "HLSL generation with packoffset succeeds");
        std::string hlsl = hlslGenerator.GetResult();
        CHECK(Contains(hlsl, ": packoffset(c4.x)"), "HLSL keeps a c4.x packoffset");
        CHECK(Contains(hlsl, ": packoffset(c1.y)"), "HLSL keeps a c1.y packoffset");
        CHECK(Contains(hlsl, ": packoffset(c1.w)"), "HLSL keeps a c1.w packoffset");
        CHECK(Contains(hlsl, ": packoffset(c5)"), "HLSL keeps array packoffset(c5)");

        Allocator allocator2;
        HLSLTree reparsed(&allocator2);
        bool reparsedOk = ParseSource(allocator2, reparsed, hlsl.c_str());
        CHECK(reparsedOk, "generated HLSL reparses");

        if (reparsedOk)
        {
            CBufferLayout originalL;
            CBufferLayout reparsedL;
            CHECK(BuildBufferLayout(tree, "L", originalL), "original L layout");
            CHECK(BuildBufferLayout(reparsed, "L", reparsedL), "reparsed L layout");
            CHECK(originalL.GetFieldCount() == reparsedL.GetFieldCount(),
                  "roundtrip keeps L field count");
            bool same = originalL.GetRegisterCount() == reparsedL.GetRegisterCount();
            for (unsigned int i = 0;
                 same && i < originalL.GetFieldCount() && i < reparsedL.GetFieldCount();
                 ++i)
            {
                const CBufferLayout::Field& a = originalL.GetField(i);
                const CBufferLayout::Field& b = reparsedL.GetField(i);
                same = a.name == b.name && a.offset == b.offset && a.size == b.size &&
                       a.arrayStride == b.arrayStride && a.arrayCount == b.arrayCount &&
                       a.explicitOffset == b.explicitOffset;
            }
            CHECK(same, "reparsed HLSL yields the identical L layout");

            CBufferLayout originalD;
            CBufferLayout reparsedD;
            CHECK(BuildBufferLayout(tree, "Data", originalD), "original Data layout");
            CHECK(BuildBufferLayout(reparsed, "Data", reparsedD), "reparsed Data layout");
            bool sameData = originalD.GetRegisterCount() == reparsedD.GetRegisterCount() &&
                            originalD.GetFieldCount() == reparsedD.GetFieldCount();
            for (unsigned int i = 0;
                 sameData && i < originalD.GetFieldCount() && i < reparsedD.GetFieldCount();
                 ++i)
            {
                const CBufferLayout::Field& a = originalD.GetField(i);
                const CBufferLayout::Field& b = reparsedD.GetField(i);
                sameData = a.name == b.name && a.offset == b.offset && a.size == b.size &&
                           a.arrayStride == b.arrayStride && a.arrayCount == b.arrayCount;
            }
            CHECK(sameData, "reparsed HLSL yields the identical Data layout");
        }
    }

    //------------------------------------------------------------------------
    // 6. Old no-packoffset layout/output comparison stays exactly historical.
    //------------------------------------------------------------------------
    {
        Allocator allocator;
        HLSLTree tree(&allocator);
        CHECK(ParseSource(allocator, tree, kOldShader), "old shader parses");

        CBufferLayout oldLayout;
        CHECK(BuildBufferLayout(tree, "B", oldLayout), "old layout query builds");
        CHECK(!oldLayout.HasExplicitOffsets(), "old buffer is not explicit");
        CHECK(oldLayout.GetRegisterCount() == 12, "old layout query reports B[12]");

        std::string oldGlsl;
        bool ok = GenerateGLSL(tree, GLSLGenerator::Flag_EmulateConstantBuffer, oldGlsl);
        CHECK(ok, "old shader generates");
        CHECK(Contains(oldGlsl, "uniform vec4 B[12];"), "old uniform length unchanged");
        // These exact address expressions are the historical output.
        CHECK(Contains(oldGlsl, "Light L = Light(B[7].xyz,B[7]);"),
              "old whole-struct expression is byte-for-byte preserved");
        CHECK(Contains(oldGlsl,
            "return vec4((((((((B[0].x + (B[0].yz).x) + "
            "matrix_row0(mat4(B[1],B[2],B[3],B[4]),0)[0]) + (B[5].xyz).x) + "
            "((L).color).y) + (B[9].xyz).z) + (B[2*(i)+6]).z) + B[11].x), 0, 0, 1);"),
              "old return expression addressing is byte-for-byte preserved");
    }

    //------------------------------------------------------------------------
    // 7. Repeated query/generate stability on independent builds.
    //------------------------------------------------------------------------
    {
        for (int iteration = 0; iteration < 3; ++iteration)
        {
            Allocator allocator;
            HLSLTree tree(&allocator);
            ParseSource(allocator, tree, kMainShader);
            CBufferLayout first;
            BuildBufferLayout(tree, "Data", first);
            CBufferLayout second;
            BuildBufferLayout(tree, "Data", second);
            CHECK(first.GetRegisterCount() == second.GetRegisterCount() &&
                  first.GetField(0).offset == second.GetField(0).offset &&
                  first.GetField(1).arrayStride == second.GetField(1).arrayStride,
                  "repeated layout queries are stable");
        }
    }

    printf("\nlayout_check: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0)
    {
        puts("LAYOUT_CHECK_OK");
        return 0;
    }
    return 1;
}
