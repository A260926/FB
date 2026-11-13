// Target tests for paired vertex/fragment varying packing.
//
// Three groups, all driven through the public glslopt_optimizer.h API:
//   packing   - interface matching, qualifier grouping, integer and dead-output rules
//   execution - a small CPU GLSL-subset interpreter evaluates the *generated*
//               sources end to end (vertex writes, interpolation, fragment reads)
//               and compares them against an independent reference model
//   compat    - interface failures, name collisions, context lifetime, old API
#include "suite.h"
#include "glsl_optimizer.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

// ===========================================================================
// small GLSL-subset interpreter used by the execution group
// ===========================================================================
namespace {

struct Val {
   int n = 0;
   bool isInt = false;
   bool boolean = false;
   float f[4] = {0, 0, 0, 0};
   int i[4] = {0, 0, 0, 0};
};

struct Tok {
   enum Kind { ID, NUM, OP } kind;
   std::string text;
   double num = 0.0;
   bool integer = false;
};

struct Interp {
   std::vector<Tok> tok;
   size_t pos = 0;
   std::map<std::string, Val> env;
   std::map<std::string, std::string> slotQual; // slot name -> smooth/noperspective/flat

   static bool isIdentChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

   void tokenize(const std::string& src)
   {
      for (size_t i = 0; i < src.size();) {
         char c = src[i];
         if (isspace((unsigned char)c)) { ++i; continue; }
         if (isalpha((unsigned char)c) || c == '_') {
            size_t j = i + 1;
            while (j < src.size() && isIdentChar(src[j])) ++j;
            tok.push_back({Tok::ID, src.substr(i, j - i), 0.0, false});
            i = j;
            continue;
         }
         if (isdigit((unsigned char)c) || (c == '.' && i + 1 < src.size() && isdigit((unsigned char)src[i + 1]))) {
            size_t j = i + 1;
            bool dot = c == '.';
            while (j < src.size() && (isdigit((unsigned char)src[j]) || src[j] == '.')) {
               dot |= src[j] == '.';
               ++j;
            }
            bool integer = false;
            if (j < src.size() && (src[j] == 'u' || src[j] == 'U')) { integer = true; ++j; }
            std::string lit = src.substr(i, j - i - (integer ? 1 : 0));
            tok.push_back({Tok::NUM, lit, std::stod(lit), integer || !dot});
            i = j;
            continue;
         }
         tok.push_back({Tok::OP, std::string(1, c), 0.0, false});
         ++i;
      }
   }

   const Tok& peek(int off = 0) const { static Tok eof{Tok::OP, "", 0, false}; return pos + off < tok.size() ? tok[pos + off] : eof; }
   bool accept(const std::string& s) { if (peek().text == s) { ++pos; return true; } return false; }
   void expect(const std::string& s) { if (!accept(s)) { std::fprintf(stderr, "interp: expected '%s' at %zu\n", s.c_str(), pos); } }

   static const std::set<std::string>& ctorTypes()
   {
      static const std::set<std::string> s = {"vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4"};
      return s;
   }
   static const std::set<std::string>& scalarTypes()
   {
      static const std::set<std::string> s = {"float", "int", "uint", "bool", "vec2", "vec3", "vec4",
                                              "ivec2", "ivec3", "ivec4", "void"};
      return s;
   }

   Val makeFloat(float x) { Val v; v.n = 1; v.f[0] = x; return v; }
   Val makeInt(int x) { Val v; v.n = 1; v.isInt = true; v.i[0] = x; return v; }

   static int swizzleIndex(char c)
   {
      const char* axes = "xyzw";
      return (int)(strchr(axes, c) - axes);
   }
   static Val applySwizzle(const Val& v, const std::string& sw)
   {
      Val out = v;
      out.n = (int)sw.size();
      for (int k = 0; k < out.n; ++k) {
         int idx = swizzleIndex(sw[k]);
         if (v.isInt) out.i[k] = v.i[idx];
         else out.f[k] = v.f[idx];
      }
      return out;
   }

   Val parseExpr()
   {
      Val a = parseCmp();
      return a;
   }
   Val parseCmp()
   {
      Val a = parseAdd();
      if (peek().kind == Tok::OP && (peek().text == "<" || peek().text == ">" || peek().text == "==")) {
         std::string op = peek().text;
         ++pos;
         Val b = parseAdd();
         Val r;
         r.n = 1;
         r.boolean = true;
         float x = a.isInt ? (float)a.i[0] : a.f[0];
         float y = b.isInt ? (float)b.i[0] : b.f[0];
         r.f[0] = op == "<" ? x < y : op == ">" ? x > y : x == y;
         return r;
      }
      return a;
   }
   Val parseAdd()
   {
      Val a = parseMul();
      while (peek().kind == Tok::OP && (peek().text == "+" || peek().text == "-")) {
         std::string op = peek().text; ++pos;
         Val b = parseMul();
         a = binary(a, b, op[0]);
      }
      return a;
   }
   Val parseMul()
   {
      Val a = parseUnary();
      while (peek().kind == Tok::OP && (peek().text == "*" || peek().text == "/")) {
         std::string op = peek().text; ++pos;
         Val b = parseUnary();
         a = binary(a, b, op[0]);
      }
      return a;
   }
   static Val binary(const Val& a, const Val& b, char op)
   {
      Val r;
      r.n = a.n;
      r.isInt = a.isInt && b.isInt;
      for (int k = 0; k < r.n; ++k) {
         int bk = b.n == 1 ? 0 : k;
         if (r.isInt) {
            int x = a.i[k], y = b.i[bk];
            r.i[k] = op == '+' ? x + y : op == '-' ? x - y : op == '*' ? x * y : x / y;
         } else {
            float x = a.isInt ? (float)a.i[k] : a.f[k];
            float y = b.isInt ? (float)b.i[bk] : b.f[bk];
            r.f[k] = op == '+' ? x + y : op == '-' ? x - y : op == '*' ? x * y : x / y;
         }
      }
      return r;
   }
   Val parseUnary()
   {
      if (accept("-")) {
         Val v = parseUnary();
         if (v.isInt) v.i[0] = -v.i[0];
         else for (int k = 0; k < v.n; ++k) v.f[k] = -v.f[k];
         return v;
      }
      return parsePrimary();
   }
   Val parsePrimary()
   {
      if (accept("(")) { Val v = parseExpr(); expect(")"); return parsePostfix(v); }
      if (peek().kind == Tok::NUM) {
         Tok t = peek(); ++pos;
         Val v;
         v.n = 1;
         if (t.integer) { v.isInt = true; v.i[0] = (int)t.num; }
         else v.f[0] = (float)t.num;
         return parsePostfix(v);
      }
      const std::string id = peek().text;
      if (peek().kind != Tok::ID) { std::fprintf(stderr, "interp: unexpected token '%s'\n", id.c_str()); return makeFloat(0); }
      ++pos;
      if ((id == "int" || id == "float") && peek().text == "(") {
         ++pos;
         Val v = parseExpr();
         expect(")");
         Val r;
         r.n = v.n;
         r.isInt = id == "int";
         for (int k = 0; k < v.n; ++k) {
            float x = v.isInt ? (float)v.i[k] : v.f[k];
            if (r.isInt) r.i[k] = (int)x; else r.f[k] = x;
         }
         return parsePostfix(r);
      }
      if (ctorTypes().count(id)) {
         expect("(");
         bool iv = id[0] == 'i';
         int width = id[id.size() - 1] - '0';
         Val r;
         r.n = 0;
         r.isInt = iv;
         while (!accept(")")) {
            Val a = parseExpr();
            for (int k = 0; k < a.n && r.n < width; ++k) {
               if (iv) r.i[r.n] = a.isInt ? a.i[k] : (int)a.f[k];
               else r.f[r.n] = a.isInt ? (float)a.i[k] : a.f[k];
               ++r.n;
            }
            accept(",");
         }
         while (r.n < width) { if (iv) r.i[r.n] = 0; else r.f[r.n] = 0; ++r.n; }
         return parsePostfix(r);
      }
      // plain variable
      Val v = env[id];
      if (v.n == 0) {
         // implicit zero initialization of an interface variable
         v.n = 4;
         v.isInt = slotQual[id] == "flat";
         env[id] = v;
      }
      return parsePostfix(v);
   }
   Val parsePostfix(Val v)
   {
      while (accept(".")) {
         std::string sw = peek().text;
         ++pos;
         v = applySwizzle(v, sw);
      }
      return v;
   }

   void assignVar(const std::string& name, const std::string& sw, Val rhs)
   {
      if (!env.count(name)) {
         Val z;
         z.n = 4;
         z.isInt = slotQual[name] == "flat";
         env[name] = z;
      }
      Val& dst = env[name];
      if (sw.empty()) {
         dst = rhs;
         return;
      }
      for (int k = 0; k < (int)sw.size(); ++k) {
         int idx = swizzleIndex(sw[k]);
         if (dst.isInt) dst.i[idx] = rhs.isInt ? rhs.i[k] : (int)rhs.f[k];
         else dst.f[idx] = rhs.isInt ? (float)rhs.i[k] : rhs.f[k];
      }
   }

   bool truthy(const Val& v) { return v.boolean ? v.f[0] != 0 : (v.isInt ? v.i[0] != 0 : v.f[0] != 0); }

   void runStatement()
   {
      if (accept(";") || accept("}")) return;
      if (accept("if")) {
         expect("(");
         Val cond = parseExpr();
         expect(")");
         bool taken = truthy(cond);
         if (peek().text == "{") {
            ++pos;
            if (taken) while (!accept("}")) runStatement();
            else { int depth = 1; while (depth) { if (accept("{")) ++depth; else if (accept("}")) --depth; else ++pos; } }
         } else if (taken) runStatement();
         else while (!accept(";")) ++pos;
         if (accept("else")) {
            bool runElse = !taken;
            if (peek().text == "{") {
               ++pos;
               if (runElse) while (!accept("}")) runStatement();
               else { int depth = 1; while (depth) { if (accept("{")) ++depth; else if (accept("}")) --depth; else ++pos; } }
            } else if (runElse) runStatement();
            else while (!accept(";")) ++pos;
         }
         return;
      }
      // declaration or assignment
      if (scalarTypes().count(peek().text)) {
         ++pos;
         // "vecN tK = expr ;" -> treat as assignment
         if (peek().kind == Tok::ID) {
            std::string name = peek().text; ++pos;
            if (accept("=")) {
               Val rhs = parseExpr();
               assignVar(name, "", rhs);
            }
         }
         while (!accept(";")) ++pos;
         return;
      }
      std::string name = peek().text;
      ++pos;
      std::string sw;
      if (accept(".")) { sw = peek().text; ++pos; }
      expect("=");
      Val rhs = parseExpr();
      assignVar(name, sw, rhs);
      expect(";");
   }

   // collect "qualifier* (in|out) vecN slot;" declarations
   void scanDecls(const std::vector<std::string>& lines)
   {
      for (const std::string& line : lines) {
         if (line.find(" in ") == std::string::npos && line.find(" out ") == std::string::npos)
            continue;
         std::string qual = "smooth";
         if (line.find("noperspective") != std::string::npos) qual = "noperspective";
         else if (line.find("flat") != std::string::npos) qual = "flat";
         size_t posIn = line.find(" in ");
         size_t posOut = line.find(" out ");
         size_t sp = std::string::npos;
         if (posIn != std::string::npos) sp = posIn + 4;
         if (posOut != std::string::npos && (sp == std::string::npos || posOut < sp)) sp = posOut + 5;
         if (sp == std::string::npos) continue;
         size_t nameStart = line.find(' ', sp) + 1;
         size_t nameEnd = line.find(';', nameStart);
         std::string nm = line.substr(nameStart, nameEnd - nameStart);
         slotQual[nm] = qual;
      }
   }

   Val execute(const std::string& source, const std::string& outputName)
   {
      std::vector<std::string> lines;
      for (size_t start = 0; start < source.size();) {
         size_t e = source.find('\n', start);
         if (e == std::string::npos) e = source.size();
         lines.push_back(source.substr(start, e - start));
         start = e + 1;
      }
      scanDecls(lines);
      // skip the global declaration block: execute only inside main()
      size_t brace = source.find('{');
      std::string body = source.substr(brace + 1, source.rfind('}') - brace - 1);
      tok.clear();
      pos = 0;
      tokenize(body);
      while (pos < tok.size()) runStatement();
      return env[outputName];
   }
};

// ---------------------------------------------------------------------------
// reference math model (independent of the generated source)
// ---------------------------------------------------------------------------
struct V4 { float x, y, z, w; };
struct VertexOut {
   V4 clip;
   std::map<std::string, Val> vary; // original named varyings
};

// Perspective-correct (smooth) interpolation given screen-space barycentrics.
Val perspInterp(const Val vals[3], const V4 clip[3], const float gamma[3])
{
   float wi[3] = {1.0f / clip[0].w, 1.0f / clip[1].w, 1.0f / clip[2].w};
   float denom = gamma[0] * wi[0] + gamma[1] * wi[1] + gamma[2] * wi[2];
   Val r;
   r.n = vals[0].n;
   r.isInt = vals[0].isInt;
   for (int k = 0; k < r.n; ++k) {
      float acc = 0;
      for (int v = 0; v < 3; ++v)
         acc += gamma[v] * wi[v] * (vals[v].isInt ? (float)vals[v].i[k] : vals[v].f[k]);
      r.f[k] = acc / denom;
   }
   return r;
}
Val linearInterp(const Val vals[3], const float gamma[3])
{
   Val r;
   r.n = vals[0].n;
   r.isInt = vals[0].isInt;
   for (int k = 0; k < r.n; ++k) {
      float acc = 0;
      for (int v = 0; v < 3; ++v)
         acc += gamma[v] * (vals[v].isInt ? (float)vals[v].i[k] : vals[v].f[k]);
      r.f[k] = acc;
   }
   return r;
}
bool close(const Val& a, const Val& b, float eps = 1e-4f)
{
   if (a.n != b.n) return false;
   for (int k = 0; k < a.n; ++k) {
      float x = a.isInt ? (float)a.i[k] : a.f[k];
      float y = b.isInt ? (float)b.i[k] : b.f[k];
      if (std::fabs(x - y) > eps) return false;
   }
   return true;
}
bool close4(const Val& a, const Val& b) { return close(a, b); }

// exact placement of one original varying inside a packed slot, as the
// documented bin-packing rules (width desc, then name; lowest free span)
// require for the two execution shaders
struct Place { int slot; int offset; int count; bool isInt; };

bool expectPlacement(Interp& ip, const std::vector<std::string>& slots, const std::map<std::string, Place>& want,
                      const std::map<std::string, Val>& ref)
{
   for (const auto& kv : want) {
      const std::string& name = kv.first;
      const Place& pl = kv.second;
      const Val& packed = ip.env[slots[pl.slot]];
      const Val& expected = ref.at(name);
      for (int k = 0; k < pl.count; ++k) {
         float got = pl.isInt ? (float)packed.i[pl.offset + k] : packed.f[pl.offset + k];
         float expv = expected.isInt ? (float)expected.i[k] : expected.f[k];
         if (std::fabs(got - expv) >= 1e-5f) return false;
      }
   }
   return true;
}

// reference vertex math for the straight-line shader (scenario 1)
VertexOut refVertex1(V4 p)
{
   VertexOut o;
   o.clip = {p.x, p.y, 0.0f, p.w};
   Val base; base.n = 3; base.f[0] = p.x + 0.1f; base.f[1] = p.y + 0.2f; base.f[2] = p.z + 0.3f;
   Val alpha = Interp{}.makeFloat(p.w * 0.5f);
   Val uv; uv.n = 2; uv.f[0] = p.x * 2.0f; uv.f[1] = p.y * 2.0f;
   Val k = Interp{}.makeFloat(p.z - 1.0f);
   o.vary["base"] = base;
   o.vary["alpha"] = alpha;
   o.vary["uv"] = uv;
   o.vary["k"] = k;
   return o;
}
// reference fragment for scenario 1: color = vec4(base,alpha) + vec4(uv,k,0)
Val refFrag1(const std::map<std::string, Val>& v)
{
   const Val& base = v.at("base");
   const Val& alpha = v.at("alpha");
   const Val& uv = v.at("uv");
   const Val& k = v.at("k");
   Val r; r.n = 4;
   r.f[0] = base.f[0] + uv.f[0];
   r.f[1] = base.f[1] + uv.f[1];
   r.f[2] = base.f[2] + k.f[0];
   r.f[3] = alpha.f[0];
   return r;
}

// reference vertex math for the branch shader (scenario 2)
VertexOut refVertex2(V4 p)
{
   VertexOut o;
   o.clip = p;
   float a, b;
   if (p.x > 0.0f) { a = p.y; b = p.z; } else { a = p.z; b = p.y; }
   Val va = Interp{}.makeFloat(a);
   Val vb = Interp{}.makeFloat(b);
   Val g; g.n = 2; g.f[0] = a; g.f[1] = b;
   Val sel; sel.n = 1; sel.isInt = true; sel.i[0] = (int)p.w;
   o.vary["a"] = va;
   o.vary["b"] = vb;
   o.vary["g"] = g;
   o.vary["sel"] = sel;
   return o;
}
Val refFrag2(const std::map<std::string, Val>& v)
{
   const Val& a = v.at("a");
   const Val& b = v.at("b");
   const Val& g = v.at("g");
   const Val& sel = v.at("sel");
   Val r; r.n = 4;
   r.f[0] = a.f[0] + (float)sel.i[0];
   r.f[1] = b.f[0] + (float)sel.i[0];
   r.f[2] = g.f[0] + (float)sel.i[0];
   r.f[3] = g.f[1] + (float)sel.i[0];
   return r;
}

// Run the generated pair over a triangle and return the fragment color from
// the packed pipeline by interpreting both generated sources.
Val runPackedPipeline(const std::string& vsSrc, const std::string& fsSrc, const V4 p[3],
                      const float gamma[3], const std::map<std::string, std::string>& qual,
                      const std::vector<std::string>& slotOrder, bool corruptSwizzle)
{
   Val slotVerts[3][8];
   for (int vi = 0; vi < 3; ++vi) {
      Interp ip;
      ip.env["p"].n = 4;
      ip.env["p"].f[0] = p[vi].x; ip.env["p"].f[1] = p[vi].y; ip.env["p"].f[2] = p[vi].z; ip.env["p"].f[3] = p[vi].w;
      ip.execute(vsSrc, "gl_Position");
      for (size_t s = 0; s < slotOrder.size(); ++s)
         slotVerts[vi][s] = ip.env[slotOrder[s]];
   }
   std::map<std::string, Val> interpolated;
   for (size_t s = 0; s < slotOrder.size(); ++s) {
      Val vals[3] = {slotVerts[0][s], slotVerts[1][s], slotVerts[2][s]};
      const std::string& q = qual.at(slotOrder[s]);
      Val iv = q == "flat" ? vals[0] : q == "noperspective" ? linearInterp(vals, gamma) : perspInterp(vals, p, gamma);
      if (corruptSwizzle && iv.n >= 2) std::swap(iv.f[0], iv.f[1]);
      interpolated[slotOrder[s]] = iv;
   }
   Interp fp;
   fp.env = interpolated;
   return fp.execute(fsSrc, "color");
}

const char* VS1 =
"#version 330 core\n"
"in vec4 p;\n"
"out vec3 base;out float alpha;noperspective out vec2 uv;noperspective out float k;\n"
"void main(){gl_Position=vec4(p.xy,0.0,p.w);base=p.xyz+vec3(0.1,0.2,0.3);alpha=p.w*0.5;uv=p.xy*2.0;k=p.z-1.0;}\n";
const char* FS1 =
"#version 330 core\n"
"in vec3 base;in float alpha;noperspective in vec2 uv;noperspective in float k;out vec4 color;\n"
"void main(){ color=vec4(base,alpha)+vec4(uv,k,0.0); }\n";

const char* VS2 =
"#version 330 core\n"
"in vec4 p;\nout float a;out float b;out vec2 g;flat out int sel;\n"
"void main(){gl_Position=p; if(p.x>0.0){a=p.y;b=p.z;}else{a=p.z;b=p.y;} g=vec2(a,b); sel=int(p.w);}\n";
const char* FS2 =
"#version 330 core\n"
"in float a;in float b;in vec2 g;flat in int sel;out vec4 color;\n"
"void main(){ color=vec4(a,b,g)+vec4(float(sel)); }\n";

const char* VS3 =
"#version 330 core\n"
"in vec4 p;\nout vec2 persp;noperspective out vec2 lin;\n"
"void main(){gl_Position=p;persp=p.xy;lin=p.xy;}\n";
const char* FS3 =
"#version 330 core\n"
"in vec2 persp;noperspective in vec2 lin;out vec4 color;\n"
"void main(){ color=vec4(persp,lin); }\n";

// ===========================================================================
// helpers shared by the packing/compat groups
// ===========================================================================
glslopt_ctx* ctx()
{
   static glslopt_ctx* c = nullptr;
   if (!c) c = glslopt_initialize(kGlslTargetOpenGL);
   return c;
}

std::vector<std::string> packedSlots(const std::string& src)
{
   std::vector<std::string> out;
   for (size_t i = src.find("glslopt_pv"); i != std::string::npos; i = src.find("glslopt_pv", i + 1)) {
      size_t e = i;
      while (e < src.size() && (isalnum((unsigned char)src[e]) || src[e] == '_')) ++e;
      std::string nm = src.substr(i, e - i);
      bool seen = false;
      for (auto& s : out) if (s == nm) seen = true;
      if (!seen) out.push_back(nm);
   }
   return out;
}

bool pairOk(const char* vs, const char* fs, glslopt_shader** v, glslopt_shader** f)
{
   return glslopt_optimize_pair(ctx(), vs, fs, 0, v, f) && glslopt_get_status(*v) && glslopt_get_status(*f);
}

const char* VS_BASE =
"#version 330 core\n"
"in vec4 p;out vec2 uv;out float fog;out vec3 unusedColor;\n"
"void main(){gl_Position=p;uv=p.xy;fog=p.z;unusedColor=p.xyz;}\n";
const char* FS_BASE =
"#version 330 core\n"
"in vec2 uv;in float fog;out vec4 color;\n"
"void main(){color=vec4(uv,fog,1.0);}\n";

} // namespace

// ===========================================================================
// packing
// ===========================================================================
static int group_packing()
{
   {
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(pairOk(VS_BASE, FS_BASE, &v, &f));
      std::string vo = glslopt_get_output(v), fo = glslopt_get_output(f);
      UNIT_CHECK(glslopt_shader_get_input_count(f) == 1);
      UNIT_CHECK(vo.find("unusedColor") == std::string::npos);
      UNIT_CHECK(fo.find("uv") == std::string::npos && fo.find("fog") == std::string::npos);
      auto vs = packedSlots(vo), fs2 = packedSlots(fo);
      UNIT_CHECK(vs.size() == 1 && fs2.size() == 1 && vs[0] == fs2[0]);
      UNIT_CHECK(vo.find(vs[0] + ".xy") != std::string::npos && vo.find(vs[0] + ".z") != std::string::npos);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // vec3 + float share a slot; two vec3 do not
      const char* vs = "#version 330 core\nin vec4 p;out vec3 a;out float b;out vec3 c;\n"
                       "void main(){gl_Position=p;a=p.xyz;b=p.w;c=p.zyx;}\n";
      const char* fs = "#version 330 core\nin vec3 a;in float b;in vec3 c;out vec4 color;\n"
                       "void main(){color=vec4(a,b)+vec4(c,1.0);}\n";
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(pairOk(vs, fs, &v, &f));
      UNIT_CHECK((int)packedSlots(glslopt_get_output(v)).size() == 2);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // vec2 + vec2 share one slot
      const char* vs = "#version 330 core\nin vec4 p;out vec2 a;out vec2 b;\n"
                       "void main(){gl_Position=p;a=p.xy;b=p.zw;}\n";
      const char* fs = "#version 330 core\nin vec2 a;in vec2 b;out vec4 color;\n"
                       "void main(){color=vec4(a,b);}\n";
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(pairOk(vs, fs, &v, &f));
      UNIT_CHECK((int)packedSlots(glslopt_get_output(v)).size() == 1);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // qualifiers split groups; centroid and noperspective stay distinct
      const char* vs = "#version 330 core\nin vec4 p;\nout float s;noperspective out float n;"
                       "smooth centroid out float c;flat out int q;\n"
                       "void main(){gl_Position=p;s=p.x;n=p.y;c=p.z;q=int(p.w);}\n";
      const char* fs = "#version 330 core\nin float s;noperspective in float n;smooth centroid in float c;"
                       "flat in int q;out vec4 color;\n"
                       "void main(){color=vec4(s,n,c,float(q));}\n";
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(pairOk(vs, fs, &v, &f));
      std::string vo = glslopt_get_output(v);
      UNIT_CHECK((int)packedSlots(vo).size() == 4);
      UNIT_CHECK(vo.find("noperspective out vec4") != std::string::npos);
      UNIT_CHECK(vo.find("smooth centroid out vec4") != std::string::npos);
      UNIT_CHECK(vo.find("flat out ivec4") != std::string::npos);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // integer varyings must be flat on both ends
      const char* vs = "#version 330 core\nin vec4 p;flat out int a;flat out uint b;\n"
                       "void main(){gl_Position=p;a=int(p.x);b=uint(p.y);}\n";
      const char* fs = "#version 330 core\nflat in int a;flat in uint b;out vec4 color;\n"
                       "void main(){color=vec4(a,int(b),0,1);}\n";
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(pairOk(vs, fs, &v, &f));
      std::string vo = glslopt_get_output(v);
      UNIT_CHECK(vo.find("ivec4") != std::string::npos);
      UNIT_CHECK(vo.find("uvec4") != std::string::npos);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // dead output removed; its side-effecting computation stays
      const char* vs = "#version 330 core\n#extension GL_ARB_shader_image_load_store : enable\n"
                       "layout(r32ui) uniform uimage2D img;\n"
                       "in vec4 p;out float kept;flat out uint deadV;\n"
                       "void main(){gl_Position=p;kept=p.x;deadV=imageAtomicAdd(img,ivec2(0),1u);}\n";
      const char* fs = "#version 330 core\nin float kept;out vec4 color;void main(){color=vec4(kept);}\n";
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(pairOk(vs, fs, &v, &f));
      std::string vo = glslopt_get_output(v);
      UNIT_CHECK(vo.find("deadV") == std::string::npos);
      UNIT_CHECK(vo.find("imageAtomicAdd") != std::string::npos);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // unsupported shapes fail explicitly
      const char* blockV = "#version 330 core\nin vec4 p;out B{float x;} bb;void main(){gl_Position=p;bb.x=p.x;}\n";
      const char* blockF = "#version 330 core\nin B{float x;} bb;out vec4 color;void main(){color=vec4(bb.x);}\n";
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(!glslopt_optimize_pair(ctx(), blockV, blockF, 0, &v, &f));
      UNIT_CHECK(v && f && !glslopt_get_status(v) && !glslopt_get_status(f));
      UNIT_CHECK(std::strstr(glslopt_get_log(v), "bb") != nullptr);
      glslopt_shader_delete(v); glslopt_shader_delete(f);

      const char* matV = "#version 330 core\nin vec4 p;out mat2 m;void main(){gl_Position=p;m=mat2(p);}\n";
      const char* matF = "#version 330 core\nin mat2 m;out vec4 color;void main(){color=vec4(m);}\n";
      UNIT_CHECK(!glslopt_optimize_pair(ctx(), matV, matF, 0, &v, &f));
      UNIT_CHECK(v && f && !glslopt_get_status(v) && !glslopt_get_status(f));
      glslopt_shader_delete(v); glslopt_shader_delete(f);

      const char* arrV = "#version 330 core\nin vec4 p;out float a[2];void main(){gl_Position=p;a[0]=p.x;a[1]=p.y;}\n";
      const char* arrF = "#version 330 core\nin float a[2];out vec4 color;void main(){color=vec4(a[0],a[1],0,1);}\n";
      UNIT_CHECK(!glslopt_optimize_pair(ctx(), arrV, arrF, 0, &v, &f));
      UNIT_CHECK(v && f);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // name and type mismatches name the offending variable
      const char* vs = "#version 330 core\nin vec4 p;out float a;void main(){gl_Position=p;a=p.x;}\n";
      const char* fsMiss = "#version 330 core\nin float a;in float b;out vec4 color;void main(){color=vec4(a,b,0,1);}\n";
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(!glslopt_optimize_pair(ctx(), vs, fsMiss, 0, &v, &f));
      UNIT_CHECK(std::strstr(glslopt_get_log(f), "b") != nullptr);
      glslopt_shader_delete(v); glslopt_shader_delete(f);

      const char* vsType = "#version 330 core\nin vec4 p;out vec2 a;void main(){gl_Position=p;a=p.xy;}\n";
      const char* fsType = "#version 330 core\nin float a;out vec4 color;void main(){color=vec4(a);}\n";
      UNIT_CHECK(!glslopt_optimize_pair(ctx(), vsType, fsType, 0, &v, &f));
      UNIT_CHECK(std::strstr(glslopt_get_log(v), "a") != nullptr);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   return unit_checks;
}

// ===========================================================================
// execution
// ===========================================================================
static int group_execution()
{
   { // straight-line: vertex values, perspective + noperspective, fragment color
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(pairOk(VS1, FS1, &v, &f));
      std::string vs = glslopt_get_output(v), fs = glslopt_get_output(f);
      std::vector<std::string> slots = packedSlots(vs);
      std::map<std::string, std::string> qual;
      for (auto& s : slots) {
         size_t at = vs.find(s);
         size_t lineStart = vs.rfind('\n', at) + 1;
         std::string line = vs.substr(lineStart, vs.find(';', at) - lineStart);
         if (line.find("noperspective") != std::string::npos) qual[s] = "noperspective";
         else if (line.find("flat") != std::string::npos) qual[s] = "flat";
         else qual[s] = "smooth";
      }
      // asymmetric vertices, distinct w to make perspective and linear differ
      V4 p[3] = {{0.2f, 0.3f, 0.4f, 1.0f}, {-0.7f, 0.5f, 0.9f, 2.0f}, {0.4f, -0.6f, 0.1f, 0.5f}};
      float gamma[3] = {0.5f, 0.3f, 0.2f};
      // pv0 smooth: base.xyz at 0..2, alpha at 3; pv1 noperspective: uv.xy at 0..1, k at 2
      const std::map<std::string, Place> place1 = {
         {"base", {0, 0, 3, false}}, {"alpha", {0, 3, 1, false}},
         {"uv", {1, 0, 2, false}}, {"k", {1, 2, 1, false}},
      };
      for (int vi = 0; vi < 3; ++vi) {
         Interp ip;
         ip.env["p"].n = 4;
         ip.env["p"].f[0] = p[vi].x; ip.env["p"].f[1] = p[vi].y; ip.env["p"].f[2] = p[vi].z; ip.env["p"].f[3] = p[vi].w;
         ip.execute(vs, "gl_Position");
         UNIT_CHECK(expectPlacement(ip, slots, place1, refVertex1(p[vi]).vary));
      }
      Val packed = runPackedPipeline(vs, fs, p, gamma, qual, slots, false);
      VertexOut rv[3] = {refVertex1(p[0]), refVertex1(p[1]), refVertex1(p[2])};
      std::map<std::string, Val> refVary;
      for (auto& kv : rv[0].vary) {
         Val vals[3] = {rv[0].vary[kv.first], rv[1].vary[kv.first], rv[2].vary[kv.first]};
         bool nopersp = kv.first == "uv" || kv.first == "k";
         refVary[kv.first] = nopersp ? linearInterp(vals, gamma) : perspInterp(vals, p, gamma);
      }
      Val expected = refFrag1(refVary);
      UNIT_CHECK(close4(packed, expected));
      // a one-end swizzle swap must change the color
      Val corrupted = runPackedPipeline(vs, fs, p, gamma, qual, slots, true);
      UNIT_CHECK(!close4(corrupted, expected));
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // branch shader: both arms, flat integer, repeated conditional writes
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(pairOk(VS2, FS2, &v, &f));
      std::string vs = glslopt_get_output(v), fs = glslopt_get_output(f);
      std::vector<std::string> slots = packedSlots(vs);
      std::map<std::string, std::string> qual;
      for (auto& s : slots) {
         size_t at = vs.find(s);
         size_t lineStart = vs.rfind('\n', at) + 1;
         std::string line = vs.substr(lineStart, vs.find(';', at) - lineStart);
         qual[s] = line.find("flat") != std::string::npos ? "flat" : "smooth";
      }
      V4 p[3] = {{0.25f, 1.0f, 2.0f, 3.0f}, {-0.5f, 4.0f, 5.0f, 6.0f}, {0.8f, 7.0f, 8.0f, 9.0f}};
      float gamma[3] = {0.4f, 0.4f, 0.2f};
      // pv0 flat ivec4: sel.x; pv1 smooth: g.xy, a at z, b at w
      const std::map<std::string, Place> place2 = {
         {"sel", {0, 0, 1, true}},
         {"g", {1, 0, 2, false}}, {"a", {1, 2, 1, false}}, {"b", {1, 3, 1, false}},
      };
      // per-vertex branch semantics through the generated source
      for (int vi = 0; vi < 3; ++vi) {
         Interp ip;
         ip.env["p"].n = 4;
         ip.env["p"].f[0] = p[vi].x; ip.env["p"].f[1] = p[vi].y; ip.env["p"].f[2] = p[vi].z; ip.env["p"].f[3] = p[vi].w;
         ip.execute(vs, "gl_Position");
         UNIT_CHECK(expectPlacement(ip, slots, place2, refVertex2(p[vi]).vary));
      }
      Val packed = runPackedPipeline(vs, fs, p, gamma, qual, slots, false);
      VertexOut rv[3] = {refVertex2(p[0]), refVertex2(p[1]), refVertex2(p[2])};
      std::map<std::string, Val> refVary;
      for (auto& kv : rv[0].vary) {
         Val vals[3] = {rv[0].vary[kv.first], rv[1].vary[kv.first], rv[2].vary[kv.first]};
         refVary[kv.first] = kv.second.isInt ? vals[0] : perspInterp(vals, p, gamma);
      }
      Val expected = refFrag2(refVary);
      UNIT_CHECK(close4(packed, expected));
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // same data, smooth vs noperspective must interpolate differently
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(pairOk(VS3, FS3, &v, &f));
      std::string vs = glslopt_get_output(v), fs = glslopt_get_output(f);
      std::vector<std::string> slots = packedSlots(vs);
      std::map<std::string, std::string> qual;
      for (auto& s : slots) {
         size_t at = vs.find(s);
         size_t lineStart = vs.rfind('\n', at) + 1;
         std::string line = vs.substr(lineStart, vs.find(';', at) - lineStart);
         qual[s] = line.find("noperspective") != std::string::npos ? "noperspective" : "smooth";
      }
      V4 p[3] = {{0.1f, 0.2f, 0.0f, 1.0f}, {0.9f, 0.8f, 0.0f, 3.0f}, {0.5f, -0.4f, 0.0f, 0.25f}};
      float gamma[3] = {0.6f, 0.1f, 0.3f};
      Val color = runPackedPipeline(vs, fs, p, gamma, qual, slots, false);
      Val vals[3];
      for (int i = 0; i < 3; ++i) { vals[i].n = 2; vals[i].f[0] = p[i].x; vals[i].f[1] = p[i].y; }
      Val sp = perspInterp(vals, p, gamma), lin = linearInterp(vals, gamma);
      UNIT_CHECK(std::fabs(sp.f[0] - lin.f[0]) > 1e-3f);
      UNIT_CHECK(std::fabs(color.f[0] - sp.f[0]) < 1e-4f && std::fabs(color.f[2] - lin.f[0]) < 1e-4f);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   return unit_checks;
}

// ===========================================================================
// compat
// ===========================================================================
static int group_compat()
{
   { // failed interface still yields two deletable, failed results with logs
      const char* vs = "#version 330 core\nin vec4 p;out float a;void main(){gl_Position=p;a=p.x;}\n";
      const char* fs = "#version 330 core\nin float a;in float b;out vec4 color;void main(){color=vec4(a,b,0,1);}\n";
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(!glslopt_optimize_pair(ctx(), vs, fs, 0, &v, &f));
      UNIT_CHECK(v && f);
      UNIT_CHECK(!glslopt_get_status(v) && !glslopt_get_status(f));
      UNIT_CHECK(glslopt_get_log(v) && glslopt_get_log(v)[0]);
      UNIT_CHECK(glslopt_get_log(f) && glslopt_get_log(f)[0]);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // a stage compile failure fails both, never leaves a success behind
      const char* good = "#version 330 core\nin vec4 p;out float a;void main(){gl_Position=p;a=p.x;}\n";
      const char* broken = "#version 330 core\nin float a;out vec4 color;void main(){color=vec4(this is not glsl);}\n";
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(!glslopt_optimize_pair(ctx(), good, broken, 0, &v, &f));
      UNIT_CHECK(v && f && !glslopt_get_status(v) && !glslopt_get_status(f));
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // generated names avoid all user identifiers on both ends
      const char* vs = "#version 330 core\nin vec4 p;out float glslopt_pv0;out float other;\n"
                       "void main(){gl_Position=p;glslopt_pv0=p.x;other=p.y;}\n";
      const char* fs = "#version 330 core\nin float glslopt_pv0;in float other;out vec4 color;\n"
                       "void main(){color=vec4(glslopt_pv0,other,0,1);}\n";
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(pairOk(vs, fs, &v, &f));
      std::string vo = glslopt_get_output(v);
      UNIT_CHECK(vo.find("out vec4 glslopt_pv0") == std::string::npos);
      UNIT_CHECK(vo.find("glslopt_pv1") != std::string::npos);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // ... including identifiers that only appear as struct type names
      const char* vs = "#version 330 core\nstruct glslopt_pv0 { float x; };\n"
                       "in vec4 p;out float a;out float b;\n"
                       "void main(){gl_Position=p;glslopt_pv0 s; s.x=p.x;a=s.x;b=p.y;}\n";
      const char* fs = "#version 330 core\nin float a;in float b;out vec4 color;\n"
                       "void main(){color=vec4(a,b,0,1);}\n";
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(pairOk(vs, fs, &v, &f));
      UNIT_CHECK(std::string(glslopt_get_output(v)).find("out vec4 glslopt_pv0 ") == std::string::npos &&
                 std::string(glslopt_get_output(v)).find("out vec4 glslopt_pv0;") == std::string::npos);
      UNIT_CHECK(std::strstr(glslopt_get_output(v), "glslopt_pv1") != nullptr);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // results outlive their context
      glslopt_ctx* local = glslopt_initialize(kGlslTargetOpenGL);
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(glslopt_optimize_pair(local, VS_BASE, FS_BASE, 0, &v, &f));
      glslopt_cleanup(local);
      UNIT_CHECK(glslopt_get_status(v) && glslopt_get_status(f));
      UNIT_CHECK(std::strstr(glslopt_get_output(v), "glslopt_pv") != nullptr);
      UNIT_CHECK(glslopt_shader_get_input_count(f) == 1);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // the single-shader API keeps its original behavior and reflection
      auto* sv = glslopt_optimize(ctx(), kGlslOptShaderVertex, VS_BASE, 0);
      auto* sf = glslopt_optimize(ctx(), kGlslOptShaderFragment, FS_BASE, 0);
      UNIT_CHECK(glslopt_get_status(sv) && glslopt_get_status(sf));
      UNIT_CHECK(glslopt_shader_get_input_count(sf) == 2);
      UNIT_CHECK(std::strstr(glslopt_get_output(sv), "unusedColor") != nullptr);
      const char* withUniform = "#version 330 core\nuniform float u;in vec4 p;out vec4 c;\n"
                                "void main(){c=p*u;}\n";
      auto* su = glslopt_optimize(ctx(), kGlslOptShaderVertex, withUniform, 0);
      UNIT_CHECK(glslopt_shader_get_uniform_count(su) == 1);
      glslopt_shader_delete(sv); glslopt_shader_delete(sf); glslopt_shader_delete(su);
   }
   { // repeated runs are byte-identical; outputs parse through the library again
      glslopt_shader *v1 = nullptr, *f1 = nullptr, *v2 = nullptr, *f2 = nullptr;
      UNIT_CHECK(pairOk(VS_BASE, FS_BASE, &v1, &f1));
      UNIT_CHECK(pairOk(VS_BASE, FS_BASE, &v2, &f2));
      UNIT_CHECK(std::string(glslopt_get_output(v1)) == glslopt_get_output(v2));
      UNIT_CHECK(std::string(glslopt_get_output(f1)) == glslopt_get_output(f2));
      auto* rv = glslopt_optimize(ctx(), kGlslOptShaderVertex, glslopt_get_output(v1), 0);
      auto* rf = glslopt_optimize(ctx(), kGlslOptShaderFragment, glslopt_get_output(f1), 0);
      UNIT_CHECK(glslopt_get_status(rv) && glslopt_get_status(rf));
      glslopt_shader_delete(v1); glslopt_shader_delete(f1); glslopt_shader_delete(v2); glslopt_shader_delete(f2);
      glslopt_shader_delete(rv); glslopt_shader_delete(rf);
   }
   { // only desktop GLSL 330 core pairs are accepted
      glslopt_ctx* es = glslopt_initialize(kGlslTargetOpenGLES30);
      glslopt_shader *v = nullptr, *f = nullptr;
      UNIT_CHECK(!glslopt_optimize_pair(es, VS_BASE, FS_BASE, 0, &v, &f));
      UNIT_CHECK(v && f);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
      glslopt_cleanup(es);

      const char* old = "#version 150 core\nin vec4 p;out float a;void main(){gl_Position=p;a=p.x;}\n";
      const char* oldF = "#version 150 core\nin float a;out vec4 color;void main(){color=vec4(a);}\n";
      UNIT_CHECK(!glslopt_optimize_pair(ctx(), old, oldF, 0, &v, &f));
      UNIT_CHECK(v && f);
      glslopt_shader_delete(v); glslopt_shader_delete(f);
   }
   { // null arguments
      glslopt_shader *v = (glslopt_shader*)0x1, *f = (glslopt_shader*)0x1;
      UNIT_CHECK(!glslopt_optimize_pair(nullptr, VS_BASE, FS_BASE, 0, &v, &f) && !v && !f);
      UNIT_CHECK(!glslopt_optimize_pair(ctx(), nullptr, FS_BASE, 0, &v, &f) && !v && !f);
      UNIT_CHECK(!glslopt_optimize_pair(ctx(), VS_BASE, nullptr, 0, &v, &f) && !v && !f);
      UNIT_CHECK(!glslopt_optimize_pair(ctx(), VS_BASE, FS_BASE, 0, nullptr, &f));
      UNIT_CHECK(!glslopt_optimize_pair(ctx(), VS_BASE, FS_BASE, 0, &v, nullptr));
   }
   return unit_checks;
}

int unit_run_group(const char* name)
{
   if (!std::strcmp(name, "packing")) return group_packing();
   if (!std::strcmp(name, "execution")) return group_execution();
   if (!std::strcmp(name, "compat")) return group_compat();
   return -1;
}
