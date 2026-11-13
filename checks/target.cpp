// Copyright (c) The HLSL2GLSLFork Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.txt file.

// Target tests for ETranslateOpLowerDynamicSamplers.
//
// Three groups are exposed through unit_run_group():
//   * lowering:   AST-level rewriting structure, reflection preservation,
//                 constant-index passthrough and unsupported-use diagnostics.
//   * evaluation: the emitted GLSL fragments are turned into compilable C++
//                 through a small equivalent bridge (vector/sampler shims) and
//                 executed; texture sampling is replaced by a deterministic
//                 recorder so colors, candidate call order and side effects can
//                 be checked instead of merely counting text matches.
//   * compat:     byte-for-byte output and uniform reflection with the option
//                 off, plus source-line diagnostics for unsupported uses.

#include "suite.h"

#include "hlsl2glsl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace
{

unsigned kLower = ETranslateOpLowerDynamicSamplers;

std::string NormalizeWhitespace(std::string s)
{
	s.erase(std::remove_if(s.begin(), s.end(),
	        [](unsigned char c) { return std::isspace(c) != 0; }), s.end());
	return s;
}

bool Contains(const std::string& haystack, const char* needle)
{
	return haystack.find(needle) != std::string::npos;
}

struct Shader
{
	ShHandle handle;
	bool ok;
	std::string text;
	std::string log;

	Shader() : handle(0), ok(false) {}
};

// Parse and translate one shader with the same option set on both calls, as
// the production tool chain is expected to do.
Shader Compile(const char* source, ETargetVersion version, unsigned options)
{
	Shader result;
	result.handle = Hlsl2Glsl_ConstructCompiler(EShLangFragment);
	if (!Hlsl2Glsl_Parse(result.handle, source, version, 0, options))
	{
		result.log = Hlsl2Glsl_GetInfoLog(result.handle);
		return result;
	}
	if (!Hlsl2Glsl_Translate(result.handle, "main", version, options))
	{
		result.log = Hlsl2Glsl_GetInfoLog(result.handle);
		return result;
	}
	result.ok = true;
	result.text = Hlsl2Glsl_GetShader(result.handle);
	return result;
}

const ShUniformInfo* FindUniform(const Shader& shader, const char* name)
{
	const int count = Hlsl2Glsl_GetUniformCount(shader.handle);
	const ShUniformInfo* info = Hlsl2Glsl_GetUniformInfo(shader.handle);
	for (int i = 0; i < count; ++i)
	{
		if (std::strcmp(info[i].name, name) == 0)
			return &info[i];
	}
	return 0;
}

// Every dynamic sampler access in the emitted code must use a constant
// subscript; the parser only emits the indirect form textually as
// "name[non-digit ...]", so a digit after '[' proves the index is constant.
bool OnlyConstantSamplerIndices(const std::string& text, const char* arrayName)
{
	const std::string token = std::string(arrayName) + "[";
	size_t pos = 0;
	while ((pos = text.find(token, pos)) != std::string::npos)
	{
		const char after = text[pos + token.size()];
		if (!std::isdigit(static_cast<unsigned char>(after)))
			return false;
		pos += token.size();
	}
	return true;
}

int CountOccurrences(const std::string& text, const char* needle)
{
	int count = 0;
	size_t pos = 0;
	const size_t len = std::strlen(needle);
	while ((pos = text.find(needle, pos)) != std::string::npos)
	{
		++count;
		pos += len;
	}
	return count;
}

// ---------------------------------------------------------------------------
// Compilable equivalent bridge
// ---------------------------------------------------------------------------

const char* kBridgeRuntime =
R"CPP(
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

// The HLSL uniform named "index" would collide with the POSIX index()
// declaration pulled in by the libc headers; redirect the token only.
#define index g_index

struct vec2 {
    float x, y;
    vec2(float a=0, float b=0) : x(a), y(b) {}
};
struct vec3 {
    float x, y, z;
    vec3(float a=0, float b=0, float c=0) : x(a), y(b), z(c) {}
};
struct vec4 {
    float x, y, z, w;
    vec2 xy;
    vec3 xyz;
    vec4()
        : x(0), y(0), z(0), w(0), xy(0, 0), xyz(0, 0, 0) {}
    // GLSL single-argument constructor splats to every component.
    vec4(float a)
        : x(a), y(a), z(a), w(a), xy(a, a), xyz(a, a, a) {}
    vec4(float a, float b, float c, float d)
        : x(a), y(b), z(c), w(d), xy(a, b), xyz(a, b, c) {}
    vec4(const vec2& uv, float c, float d)
        : x(uv.x), y(uv.y), z(c), w(d), xy(uv.x, uv.y), xyz(uv.x, uv.y, c) {}
};
static vec4 operator+(const vec4& a, const vec4& b){ return vec4(a.x+b.x,a.y+b.y,a.z+b.z,a.w+b.w); }
static vec4& operator+=(vec4& a, const vec4& b){ a.x+=b.x;a.y+=b.y;a.z+=b.z;a.w+=b.w; a.xy=vec2(a.x,a.y); a.xyz=vec3(a.x,a.y,a.z); return a; }
static vec4& operator+=(vec4& a, float b){ a.x+=b;a.y+=b;a.z+=b;a.w+=b; a.xy=vec2(a.x,a.y); a.xyz=vec3(a.x,a.y,a.z); return a; }
static bool operator>(const vec4& a, float b){ return a.x > b; }
static vec2 operator+(const vec2& a, float b){ return vec2(a.x+b, a.y+b); }
static int to_int(float v){ return static_cast<int>(v); }
static int to_int(int v){ return v; }

struct Sampler { int id; explicit Sampler(int i=0) : id(i) {} };
// The emitted support-library helpers are declared with the GLSL type name.
typedef Sampler sampler2D;

struct Call { int samplerId; int kind; float a, b, c, d, e, f; };
// kind: 0 implicit, 1 bias, 2 lod, 3 gradient
static std::vector<Call> g_calls;
static void Record(Sampler& s, int kind, float a, float b, float c=0,
                   float d=0, float e=0, float f=0) {
    g_calls.push_back(Call{s.id, kind, a, b, c, d, e, f});
}
static vec4 SampleColor(Sampler& s, float u, float v) {
    // Deterministic color: first channel encodes the sampler id, others the
    // coordinate so forwarding is observable.
    return vec4(static_cast<float>(s.id + 1), u, v, 0.25f);
}
static vec4 texture2D(Sampler& s, vec2 uv) {
    Record(s, 0, uv.x, uv.y);
    return SampleColor(s, uv.x, uv.y);
}
static vec4 texture2D(Sampler& s, vec2 uv, float bias) {
    Record(s, 1, uv.x, uv.y, bias);
    return SampleColor(s, uv.x, uv.y);
}
static vec4 texture2DLod(Sampler& s, vec2 uv, float lod) {
    Record(s, 2, uv.x, uv.y, lod);
    return SampleColor(s, uv.x, uv.y);
}
static vec4 texture2DGradARB(Sampler& s, vec2 uv, vec2 dx, vec2 dy) {
    Record(s, 3, uv.x, uv.y, dx.x, dx.y, dy.x, dy.y);
    return SampleColor(s, uv.x, uv.y);
}
)CPP";

// Reduce the emitted shader to the parts valid inside the C++ bridge:
// version/extension/global/varying lines and the generated main() wrapper
// are dropped, the GLSL "in" qualifier is removed, and the remaining generated
// helper functions and xlat_main are compiled verbatim with the shim runtime.
std::string ExtractBridgeFunctions(const std::string& glsl)
{
	std::istringstream in(glsl);
	std::ostringstream out;
	std::string line;
	bool insideMainWrapper = false;
	int wrapperDepth = 0;
	while (std::getline(in, line))
	{
		if (!insideMainWrapper && line.compare(0, 9, "void main") == 0)
		{
			insideMainWrapper = true;
			wrapperDepth = 0;
		}
		if (insideMainWrapper)
		{
			wrapperDepth += CountOccurrences(line, "{");
			wrapperDepth -= CountOccurrences(line, "}");
			if (wrapperDepth <= 0 && CountOccurrences(line, "}") > 0)
				insideMainWrapper = false;
			continue;
		}

		if (line.compare(0, 8, "#version") == 0 ||
		    line.compare(0, 10, "#extension") == 0 ||
		    line.compare(0, 5, "#line") == 0 ||
		    line.compare(0, 7, "uniform") == 0 ||
		    line.compare(0, 7, "varying") == 0 ||
		    line.compare(0, 6, "gl_") == 0 ||
		    line.compare(0, 18, "float xlat_mutable") == 0)
			continue;

		std::string replaced;
		replaced.reserve(line.size());
		for (size_t i = 0; i < line.size();)
		{
			if (line.compare(i, 4, " in ") == 0)
			{
				replaced.push_back(' ');
				i += 4;
			}
			// GLSL constructor casts "int(expr)" map to the shim conversion;
			// plain "int name" declarations must be left untouched.
			else if (line.compare(i, 4, "int(") == 0 &&
			         (i == 0 || (!std::isalnum(static_cast<unsigned char>(line[i - 1])) &&
			                     line[i - 1] != '_')))
			{
				replaced += "to_int(";
				i += 4;
			}
			else
			{
				replaced.push_back(line[i]);
				++i;
			}
		}
		out << replaced << "\n";
	}
	return out.str();
}

struct BridgeResult
{
	bool compiled;
	std::string output;
};

// Compile and run a bridge executable built from the generated functions.
BridgeResult RunBridge(const std::string& bridgeSource)
{
	BridgeResult result;
	result.compiled = false;

	static unsigned bridgeCounter = 0;
	const char* tmpRoot = std::getenv("TMPDIR");
	std::ostringstream dir;
	dir << (tmpRoot && tmpRoot[0] ? tmpRoot : ".work") << "/hlsl2glsl_bridge_"
	    << bridgeCounter++;
	const std::string dirName = dir.str();
	const std::string mk = "mkdir -p " + dirName;
	if (std::system(mk.c_str()) != 0)
		return result;

	const std::string srcPath = dirName + "/bridge.cpp";
	const std::string binPath = dirName + "/bridge";
	const std::string outPath = dirName + "/out.txt";
	{
		std::ofstream src(srcPath.c_str());
		src << bridgeSource;
	}

	const std::string compile = "c++ -std=c++14 -O0 " + srcPath + " -o " + binPath +
	                            " >" + dirName + "/build.txt 2>&1";
	if (std::system(compile.c_str()) != 0)
	{
		std::ifstream buildLog(dirName + "/build.txt");
		std::ostringstream captured;
		captured << buildLog.rdbuf();
		result.output = captured.str();
		std::system(("rm -rf " + dirName).c_str());
		return result;
	}

	const std::string run = binPath + " >" + outPath + " 2>&1";
	const int rc = std::system(run.c_str());
	if (rc != 0)
		return result;

	std::ifstream out(outPath.c_str());
	std::ostringstream captured;
	captured << out.rdbuf();
	result.output = captured.str();
	result.compiled = true;

	std::system(("rm -rf " + dirName).c_str());
	return result;
}

// ---------------------------------------------------------------------------
// lowering group
// ---------------------------------------------------------------------------

int RunLoweringGroup()
{
	const char* kBasic =
		"sampler2D maps[3]; float index;\n"
		"float4 main(float2 uv:TEXCOORD0):COLOR { return tex2D(maps[(int)index],uv); }\n";

	// The option is opt-in: without it the dynamic index is still emitted.
	Shader off = Compile(kBasic, ETargetGLSL_120, ETranslateOpNone);
	UNIT_CHECK(off.ok);
	UNIT_CHECK(Contains(off.text, "maps[int(index)]"));

	Shader on = Compile(kBasic, ETargetGLSL_120, kLower);
	UNIT_CHECK(on.ok);
	UNIT_CHECK(!Contains(on.text, "maps[int(index)]"));
	UNIT_CHECK(OnlyConstantSamplerIndices(on.text, "maps"));
	UNIT_CHECK(Contains(on.text, "xlat_dynsampler2D_"));

	// The original uniform survives with its name, size and reflection type.
	const ShUniformInfo* maps = FindUniform(on, "maps");
	UNIT_CHECK(maps != 0);
	UNIT_CHECK(maps && maps->arraySize == 3);
	UNIT_CHECK(maps && maps->type == EShTypeSampler2D);
	UNIT_CHECK(Contains(on.text, "uniform sampler2D maps[3];"));
	UNIT_CHECK(Hlsl2Glsl_GetUniformCount(on.handle) == Hlsl2Glsl_GetUniformCount(off.handle));

	// All four supported forms keep their sampling semantics, and every helper
	// evaluates one candidate per array element with a constant index.
	const char* kAllForms =
		"sampler2D maps[3]; float index;\n"
		"float4 main(float2 uv:TEXCOORD0):COLOR {\n"
		" float4 a=tex2D(maps[(int)index],uv);\n"
		" float4 b=tex2Dbias(maps[(int)index],float4(uv,0.0,0.5));\n"
		" float4 c=tex2Dlod(maps[(int)index],float4(uv,0.0,1.0));\n"
		" float4 d=tex2D(maps[(int)index],uv,float2(0.1,0.2),float2(0.3,0.4));\n"
		" return a+b+c+d;\n}\n";
	Shader forms = Compile(kAllForms, ETargetGLSL_120, kLower);
	UNIT_CHECK(forms.ok);
	UNIT_CHECK(OnlyConstantSamplerIndices(forms.text, "maps"));
	UNIT_CHECK(CountOccurrences(forms.text, "texture2D( maps[") == 3);
	UNIT_CHECK(CountOccurrences(forms.text, "xll_tex2Dbias( maps[") == 3);
	UNIT_CHECK(CountOccurrences(forms.text, "xll_tex2Dlod( maps[") == 3);
	UNIT_CHECK(CountOccurrences(forms.text, "xll_tex2Dgrad( maps[") == 3);
	UNIT_CHECK(CountOccurrences(forms.text, "xlat_dynsampler2D_") >= 4);
	UNIT_CHECK(Contains(forms.text, "vec2( 0.1, 0.2)") && Contains(forms.text, "vec2( 0.3, 0.4)"));

	// ES 1.00 lowering is valid and keeps the same uniform reflection.
	Shader es = Compile(kBasic, ETargetGLSL_ES_100, kLower);
	UNIT_CHECK(es.ok);
	UNIT_CHECK(OnlyConstantSamplerIndices(es.text, "maps"));
	const ShUniformInfo* esMaps = FindUniform(es, "maps");
	UNIT_CHECK(esMaps && esMaps->arraySize == 3 && esMaps->type == EShTypeSampler2D);

	// A constant index never enters the lowering path.
	const char* kConst =
		"sampler2D maps[3];\n"
		"float4 main(float2 uv:TEXCOORD0):COLOR { return tex2D(maps[1],uv); }\n";
	Shader constant = Compile(kConst, ETargetGLSL_120, kLower);
	UNIT_CHECK(constant.ok);
	UNIT_CHECK(!Contains(constant.text, "xlat_dynsampler2D_"));
	UNIT_CHECK(Contains(constant.text, "texture2D( maps[1], uv)"));

	// Two dynamic samples in one expression and a non-recursive helper each
	// produce their own generated function.
	const char* kTwoSites =
		"sampler2D maps[3]; float index; float counter;\n"
		"float4 pick(float2 uv, float j){ return tex2D(maps[(int)j],uv); }\n"
		"float4 main(float2 uv:TEXCOORD0):COLOR {\n"
		" return tex2D(maps[(int)(index+counter)], pick(uv,index).xy); }\n";
	Shader two = Compile(kTwoSites, ETargetGLSL_120, kLower);
	UNIT_CHECK(two.ok);
	// Two helper definitions; each signature takes the lowered integer index.
	UNIT_CHECK(CountOccurrences(two.text, "( in int ") == 2);
	UNIT_CHECK(Contains(two.text, "vec4 pick("));

	// Identifier collision avoidance: a user symbol that resembles the
	// generated prefix must not be hijacked.
	const char* kCollision =
		"sampler2D maps[2]; float index;\n"
		"float xlat_dynsampler2D_0 = 1.0;\n"
		"float4 main(float2 uv:TEXCOORD0):COLOR {\n"
		" return tex2D(maps[(int)index],uv) + float4(xlat_dynsampler2D_0,0,0,0); }\n";
	Shader collision = Compile(kCollision, ETargetGLSL_120, kLower);
	UNIT_CHECK(collision.ok);
	UNIT_CHECK(OnlyConstantSamplerIndices(collision.text, "maps"));
	UNIT_CHECK(Contains(collision.text, "float xlat_dynsampler2D_0"));
	UNIT_CHECK(Contains(collision.text, "xlat_dynsampler2D_1("));

	return unit_checks;
}

// ---------------------------------------------------------------------------
// evaluation group (compilable equivalent bridge)
// ---------------------------------------------------------------------------

std::string BuildBridge(const std::string& glsl, const std::string& globals,
                        const std::string& driver)
{
	std::ostringstream bridge;
	bridge << kBridgeRuntime;
	bridge << globals << "\n";
	bridge << ExtractBridgeFunctions(glsl) << "\n";
	bridge << "int main() {\n" << driver << "\n}\n";
	return bridge.str();
}

int RunEvaluationGroup()
{
	// Basic selection, candidate order, single evaluation, negative index.
	{
		const char* source =
			"sampler2D maps[3]; float index;\n"
			"float4 main(float2 uv:TEXCOORD0):COLOR { return tex2D(maps[(int)index],uv); }\n";
		Shader shader = Compile(source, ETargetGLSL_120, kLower);
		UNIT_CHECK(shader.ok);

		const std::string globals = "Sampler maps[3] = { Sampler(0), Sampler(1), Sampler(2) };\nfloat index;";
		const std::string driver =
			"    index = 2;\n"
			"    vec4 picked = xlat_main(vec2(0.10f, 0.20f));\n"
			"    std::printf(\"PICKED %g %g %g\\n\", picked.x, picked.y, picked.z);\n"
			"    std::printf(\"CALLS %d\\n\", (int)g_calls.size());\n"
			"    for (size_t i=0;i<g_calls.size();++i)\n"
			"        std::printf(\"C %d %d %g %g\\n\", g_calls[i].samplerId, g_calls[i].kind, g_calls[i].a, g_calls[i].b);\n"
			"    g_calls.clear();\n"
			"    index = -1;\n"
			"    vec4 oob = xlat_main(vec2(0.30f, 0.40f));\n"
			"    std::printf(\"OOB %g %g %g %g\\n\", oob.x, oob.y, oob.z, oob.w);\n"
			"    std::printf(\"OOB_CALLS %d\\n\", (int)g_calls.size());\n"
			"    std::string order;\n"
			"    char buf[16];\n"
			"    for (size_t i=0;i<g_calls.size();++i){ std::snprintf(buf,sizeof(buf),\"%d,\",g_calls[i].samplerId); order+=buf; }\n"
			"    std::printf(\"ORDER %s\\n\", order.c_str());\n";
		BridgeResult bridge = RunBridge(BuildBridge(shader.text, globals, driver));
		UNIT_CHECK(bridge.compiled);
		UNIT_CHECK(Contains(bridge.output, "PICKED 3 0.1 0.2"));
		UNIT_CHECK(Contains(bridge.output, "CALLS 3"));
		UNIT_CHECK(Contains(bridge.output, "C 0 0 0.1 0.2"));
		UNIT_CHECK(Contains(bridge.output, "C 1 0 0.1 0.2"));
		UNIT_CHECK(Contains(bridge.output, "C 2 0 0.1 0.2"));
		UNIT_CHECK(Contains(bridge.output, "OOB 0 0 0 0"));
		UNIT_CHECK(Contains(bridge.output, "OOB_CALLS 3"));
		UNIT_CHECK(Contains(bridge.output, "ORDER 0,1,2,"));
	}

	// Bias, explicit LOD and explicit gradients reach every candidate.
	{
		const char* source =
			"sampler2D maps[3]; float index;\n"
			"float4 main(float2 uv:TEXCOORD0):COLOR {\n"
			" float4 b=tex2Dbias(maps[(int)index],float4(uv,0.0,0.5));\n"
			" float4 l=tex2Dlod(maps[(int)index],float4(uv,0.0,1.0));\n"
			" float4 g=tex2D(maps[(int)index],uv,float2(0.1,0.2),float2(0.3,0.4));\n"
			" return b+l+g;\n}\n";
		Shader shader = Compile(source, ETargetGLSL_120, kLower);
		UNIT_CHECK(shader.ok);

		const std::string globals = "Sampler maps[3] = { Sampler(0), Sampler(1), Sampler(2) };\nfloat index;";
		const std::string driver =
			"    index = 1;\n"
			"    xlat_main(vec2(0.5f, 0.6f));\n"
			"    int kinds[4] = {0,0,0,0};\n"
			"    for (size_t i=0;i<g_calls.size();++i) kinds[g_calls[i].kind]++;\n"
			"    std::printf(\"KINDS %d %d %d %d\\n\", kinds[0], kinds[1], kinds[2], kinds[3]);\n"
			"    const Call& bias = g_calls[0];\n"
			"    std::printf(\"BIAS %d %g\\n\", bias.samplerId, bias.c);\n"
			"    const Call& lod = g_calls[3];\n"
			"    std::printf(\"LOD %d %g\\n\", lod.samplerId, lod.c);\n"
			"    const Call& grad = g_calls[6];\n"
			"    std::printf(\"GRAD %d %g %g %g %g\\n\", grad.samplerId, grad.c, grad.d, grad.e, grad.f);\n";
		BridgeResult bridge = RunBridge(BuildBridge(shader.text, globals, driver));
		UNIT_CHECK(bridge.compiled);
		UNIT_CHECK(Contains(bridge.output, "KINDS 0 3 3 3"));
		UNIT_CHECK(Contains(bridge.output, "BIAS 0 0.5"));
		UNIT_CHECK(Contains(bridge.output, "LOD 0 1"));
		UNIT_CHECK(Contains(bridge.output, "GRAD 0 0.1 0.2 0.3 0.4"));
	}

	// Short-circuit: an unexecuted && / ?: side performs no sampling and no
	// index/coordinate side effects at all.
	{
		const char* source =
			"sampler2D maps[3]; float index; bool flag; float counter;\n"
			"float4 main(float2 uv:TEXCOORD0):COLOR {\n"
			" float4 acc=0;\n"
			" if(flag && tex2D(maps[(int)counter++],uv).x>0.0) acc+=1.0;\n"
			" acc += flag ? tex2D(maps[(int)counter++],uv) : float4(7.0,7.0,7.0,7.0);\n"
			" return acc;\n}\n";
		Shader shader = Compile(source, ETargetGLSL_120, kLower);
		UNIT_CHECK(shader.ok);

		const std::string globals =
			"Sampler maps[3] = { Sampler(0), Sampler(1), Sampler(2) };\n"
			"float index;\nbool flag;\nfloat xlat_mutablecounter;";
		const std::string driver =
			"    flag = false;\n"
			"    xlat_mutablecounter = 0;\n"
			"    vec4 skipped = xlat_main(vec2(0.1f, 0.2f));\n"
			"    std::printf(\"SKIP_CALLS %d MUT %g COLOR %g\\n\", (int)g_calls.size(), xlat_mutablecounter, skipped.x);\n"
			"    g_calls.clear();\n"
			"    flag = true;\n"
			"    xlat_mutablecounter = 0;\n"
			"    vec4 taken = xlat_main(vec2(0.1f, 0.2f));\n"
			"    std::printf(\"TAKE_CALLS %d MUT %g COLOR %g\\n\", (int)g_calls.size(), xlat_mutablecounter, taken.x);\n";
		BridgeResult bridge = RunBridge(BuildBridge(shader.text, globals, driver));
		UNIT_CHECK(bridge.compiled);
		UNIT_CHECK(Contains(bridge.output, "SKIP_CALLS 0 MUT 0 COLOR 7"));
		// Two live dynamic samples, each expands to three candidate calls, and
		// each index expression evaluated exactly once (counter ends at 2).
		UNIT_CHECK(Contains(bridge.output, "TAKE_CALLS 6 MUT 2"));
	}

	// Loop condition recomputes each iteration and continue still runs the
	// update expression; only non-continued iterations sample.
	{
		const char* source =
			"sampler2D maps[3];\n"
			"float4 main(float2 uv:TEXCOORD0):COLOR {\n"
			" float4 acc=0;\n"
			" for(int i=0;i<3;i++){ if(i==1) continue; acc += tex2D(maps[i],uv); }\n"
			" return acc;\n}\n";
		Shader shader = Compile(source, ETargetGLSL_120, kLower);
		UNIT_CHECK(shader.ok);

		const std::string globals = "Sampler maps[3] = { Sampler(0), Sampler(1), Sampler(2) };";
		const std::string driver =
			"    vec4 acc = xlat_main(vec2(0.4f, 0.5f));\n"
			"    std::printf(\"LOOP_CALLS %d\\n\", (int)g_calls.size());\n"
			"    std::string ids; char buf[16];\n"
			"    for (size_t i=0;i<g_calls.size();++i){ std::snprintf(buf,sizeof(buf),\"%d,\",g_calls[i].samplerId); ids+=buf; }\n"
			"    std::printf(\"LOOP_IDS %s\\n\", ids.c_str());\n"
			"    std::printf(\"LOOP_COLOR %g\\n\", acc.x);\n";
		BridgeResult bridge = RunBridge(BuildBridge(shader.text, globals, driver));
		UNIT_CHECK(bridge.compiled);
		// Iterations 0 and 2 survive, each with three candidate samples.
		UNIT_CHECK(Contains(bridge.output, "LOOP_CALLS 6"));
		UNIT_CHECK(Contains(bridge.output, "LOOP_IDS 0,1,2,0,1,2,"));
		// Colors come from sampler 0 (id 1) then sampler 2 (id 3).
		UNIT_CHECK(Contains(bridge.output, "LOOP_COLOR 4"));
	}

	// Two dynamic samples in one expression evaluate left-to-right, and a
	// nested helper call completes before the outer sample.
	{
		const char* source =
			"sampler2D maps[3]; float index; float counter;\n"
			"float4 pick(float2 uv, float j){ return tex2D(maps[(int)j],uv); }\n"
			"float4 main(float2 uv:TEXCOORD0):COLOR {\n"
			" return tex2D(maps[(int)(index+counter)], pick(uv,index).xy); }\n";
		Shader shader = Compile(source, ETargetGLSL_120, kLower);
		UNIT_CHECK(shader.ok);

		const std::string globals =
			"Sampler maps[3] = { Sampler(0), Sampler(1), Sampler(2) };\n"
			"float index, counter;";
		const std::string driver =
			"    index = 1; counter = 1;\n"
			"    vec4 color = xlat_main(vec2(0.7f, 0.8f));\n"
			"    std::printf(\"NESTED_CALLS %d\\n\", (int)g_calls.size());\n"
			"    std::string ids; char buf[16];\n"
			"    for (size_t i=0;i<g_calls.size();++i){ std::snprintf(buf,sizeof(buf),\"%d,\",g_calls[i].samplerId); ids+=buf; }\n"
			"    std::printf(\"NESTED_IDS %s\\n\", ids.c_str());\n"
			"    std::printf(\"NESTED_COLOR %g %g\\n\", color.x, color.y);\n";
		BridgeResult bridge = RunBridge(BuildBridge(shader.text, globals, driver));
		UNIT_CHECK(bridge.compiled);
		// Inner helper (index 1) runs its three candidates first, then the
		// outer helper (index 2) runs its own three candidates.
		UNIT_CHECK(Contains(bridge.output, "NESTED_CALLS 6"));
		UNIT_CHECK(Contains(bridge.output, "NESTED_IDS 0,1,2,0,1,2,"));
		// Outer selection is sampler 2, whose color first channel is 3; the
		// coordinate is the inner helper's sampled .xy (sampler 1 -> id 2).
		UNIT_CHECK(Contains(bridge.output, "NESTED_COLOR 3 2"));
	}

	// Two sibling dynamic samples in one expression run strictly left-to-right,
	// each capturing its own index/coordinate once (no shared temporaries).
	{
		const char* source =
			"sampler2D maps[3]; float counter;\n"
			"float4 main(float2 uv:TEXCOORD0):COLOR {\n"
			" return tex2D(maps[(int)counter++], uv)\n"
			"      + tex2D(maps[(int)counter++], float2(uv.x+1.0, uv.y)); }\n";
		Shader shader = Compile(source, ETargetGLSL_120, kLower);
		UNIT_CHECK(shader.ok);
		UNIT_CHECK(CountOccurrences(shader.text, "( in int ") == 2);

		const std::string globals =
			"Sampler maps[3] = { Sampler(0), Sampler(1), Sampler(2) };\n"
			"float xlat_mutablecounter;";
		const std::string driver =
			"    xlat_mutablecounter = 0;\n"
			"    vec4 color = xlat_main(vec2(0.7f, 0.8f));\n"
			"    std::printf(\"SIB_CALLS %d SIB_MUT %g SIB_COLOR %g\\n\",\n"
			"                (int)g_calls.size(), xlat_mutablecounter, color.x);\n"
			"    int firstCoord = 0, secondCoord = 0;\n"
			"    for (size_t i=0;i<g_calls.size();++i) {\n"
			"        if (g_calls[i].a > 1.0f) ++secondCoord; else ++firstCoord;\n"
			"    }\n"
			"    std::printf(\"SIB_COORDS %d %d\\n\", firstCoord, secondCoord);\n"
			"    std::string ids; char buf[16];\n"
			"    for (size_t i=0;i<g_calls.size();++i){ std::snprintf(buf,sizeof(buf),\"%d,\",g_calls[i].samplerId); ids+=buf; }\n"
			"    std::printf(\"SIB_IDS %s\\n\", ids.c_str());\n";
		BridgeResult bridge = RunBridge(BuildBridge(shader.text, globals, driver));
		UNIT_CHECK(bridge.compiled);
		// Six candidate calls (two helpers x three samplers), counter advanced
		// exactly twice, and the selected colors sum sampler0+sampler1 ids.
		UNIT_CHECK(Contains(bridge.output, "SIB_CALLS 6 SIB_MUT 2 SIB_COLOR 3"));
		// Each call site captured its own coordinate: three of each, proving
		// the two samples did not share a captured temporary.
		UNIT_CHECK(Contains(bridge.output, "SIB_COORDS 3 3"));
		// Each helper sweeps samplers 0,1,2 with constant indices.
		UNIT_CHECK(Contains(bridge.output, "SIB_IDS 0,1,2,0,1,2,"));
	}

	return unit_checks;
}

// ---------------------------------------------------------------------------
// compat group
// ---------------------------------------------------------------------------

int RunCompatGroup()
{
	const char* kBasic =
		"sampler2D maps[3]; float index;\n"
		"float4 main(float2 uv:TEXCOORD0):COLOR { return tex2D(maps[(int)index],uv); }\n";

	// Output with the flag off is byte-identical across both translation APIs
	// and independent of the new pass.
	Shader first = Compile(kBasic, ETargetGLSL_120, ETranslateOpNone);
	Shader second = Compile(kBasic, ETargetGLSL_120, ETranslateOpNone);
	UNIT_CHECK(first.ok && second.ok);
	UNIT_CHECK(first.text == second.text);
	UNIT_CHECK(NormalizeWhitespace(first.text) == NormalizeWhitespace(second.text));

	// Reflection does not change merely by enabling the option.
	Shader lowered = Compile(kBasic, ETargetGLSL_120, kLower);
	UNIT_CHECK(lowered.ok);
	UNIT_CHECK(Hlsl2Glsl_GetUniformCount(first.handle) == Hlsl2Glsl_GetUniformCount(lowered.handle));
	const ShUniformInfo* a = FindUniform(first, "maps");
	const ShUniformInfo* b = FindUniform(lowered, "maps");
	UNIT_CHECK(a && b);
	UNIT_CHECK(a && b && a->arraySize == b->arraySize && a->type == b->type &&
	          std::strcmp(a->name, b->name) == 0);
	const ShUniformInfo* ia = FindUniform(first, "index");
	const ShUniformInfo* ib = FindUniform(lowered, "index");
	UNIT_CHECK(ia && ib && ia->type == ib->type);

	// The legacy constant-array fixture still translates identically.
	std::ifstream inFile("tests/fragment/array-const-in.txt");
	std::ifstream expectedFile("tests/fragment/array-const-out.txt");
	std::stringstream inBuf, expectedBuf;
	inBuf << inFile.rdbuf();
	expectedBuf << expectedFile.rdbuf();
	UNIT_CHECK(!inBuf.str().empty() && !expectedBuf.str().empty());
	Shader legacy = Compile(inBuf.str().c_str(), ETargetGLSL_110, ETranslateOpNone);
	UNIT_CHECK(legacy.ok);
	UNIT_CHECK(NormalizeWhitespace(legacy.text) == NormalizeWhitespace(expectedBuf.str()));
	Shader legacyLowered = Compile(inBuf.str().c_str(), ETargetGLSL_110, kLower);
	UNIT_CHECK(legacyLowered.ok);
	UNIT_CHECK(NormalizeWhitespace(legacy.text) == NormalizeWhitespace(legacyLowered.text));

	// Unsupported usages fail cleanly with source line numbers, never assert,
	// and never leave a shader behind.
	struct ErrorCase { const char* name; const char* source; const char* lineTag; };
	const ErrorCase cases[] =
	{
		{
			"function-argument",
			"sampler2D maps[3]; float index;\n"
			"void take(sampler2D s){}\n"
			"float4 main(float2 uv:TEXCOORD0):COLOR { take(maps[(int)index]); return float4(0,0,0,0); }\n",
			"(3):"
		},
		{
			"texture-projection",
			"sampler2D maps[3]; float index;\n"
			"float4 main(float2 uv:TEXCOORD0):COLOR { return tex2Dproj(maps[(int)index],float4(uv,0.0,1.0)); }\n",
			"(2):"
		},
		{
			"sampler-3d",
			"sampler3D maps[2]; float index;\n"
			"float4 main(float3 uv:TEXCOORD0):COLOR { return tex3D(maps[(int)index],uv); }\n",
			"(2):"
		},
		{
			"local-array",
			"float index;\n"
			"float4 main(float2 uv:TEXCOORD0):COLOR {\n"
			" sampler2D local[2];\n"
			" return tex2D(local[(int)index],uv); }\n",
			"(4):"
		},
	};
	for (const ErrorCase& c : cases)
	{
		Shader bad = Compile(c.source, ETargetGLSL_120, kLower);
		UNIT_CHECK(!bad.ok);
		UNIT_CHECK(Contains(bad.log, c.lineTag));
		UNIT_CHECK(!Contains(bad.log, "Assertion"));
		(void)c.name;
	}

	return unit_checks;
}

} // namespace

int unit_run_group(const char* group)
{
	Hlsl2Glsl_Initialize();

	if (std::strcmp(group, "lowering") == 0)
		return RunLoweringGroup();
	if (std::strcmp(group, "evaluation") == 0)
		return RunEvaluationGroup();
	if (std::strcmp(group, "compat") == 0)
		return RunCompatGroup();
	return 0;
}
