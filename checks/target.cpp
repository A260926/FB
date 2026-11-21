// Per-sample shading checks for the SoftGL software triangle fill path.
//
// Groups (registered through task_support/check.h, no main() here):
//   interpolation - per-sample varyings / FragCoord / sample metadata
//   derivatives   - same-lane 2x2 quad differences & implicit texture LOD
//   commit        - state/output masks, discard, depth, early-z, readback
//
// Expected values come from an independent double-precision barycentric model
// of the pipeline (unequal clip-space w included), not from renderer code.

#include <stddef.h>
#include "task_support/check.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>
#include <array>

#include "Render/Software/RendererSoft.h"
#include "Render/Software/ShaderProgramSoft.h"
#include "Render/Software/UniformSoft.h"

using namespace SoftGL;

namespace {

// ---------------------------------------------------------------------------
// test shader
// ---------------------------------------------------------------------------

constexpr int kMaxDim = 130;    // max viewport dimension (+ out-of-range lanes)
constexpr int kMaxSample = 4;

struct TAttribs {
  glm::vec4 position;
  glm::vec2 uv;
  float aux;
};
struct TVaryings {
  glm::vec2 uv;
  float aux;
};
struct TUniforms {
  Sampler2DSoft<RGBA> *tex = nullptr;   // must stay first (sampler slot)
  glm::vec4 color = glm::vec4(0.f);
  uint32_t outMask = 0xFFFFFFFFu;
  int discardSample = -999;             // discard when SampleID == this
  uint32_t flags = 0;
};
static_assert(offsetof(TUniforms, color) == 16, "uniform layout");

constexpr uint32_t kFlagWriteMask = 1u << 0;
constexpr uint32_t kFlagUseTexture = 1u << 1;
constexpr uint32_t kFlagColorUV = 1u << 2;

struct ShaderDefines {};
using ShaderAttributes = TAttribs;
using ShaderUniforms = TUniforms;
using ShaderVaryings = TVaryings;

struct FragRec {
  int sampleID = -999;
  glm::vec2 samplePos = glm::vec2(-1.f);
  glm::vec4 fragCoord = glm::vec4(0.f);
  uint32_t maskIn = 0;
  glm::vec2 uv = glm::vec2(0.f);
  float aux = 0.f;
  glm::vec2 ddx = glm::vec2(0.f);
  glm::vec2 ddy = glm::vec2(0.f);
  float dauxdx = 0.f;
  float dauxdy = 0.f;
};

std::atomic<long> gFragCount{0};
FragRec gRecs[kMaxDim][kMaxDim][kMaxSample];
std::atomic<int> gRecCnt[kMaxDim][kMaxDim][kMaxSample];

void resetRecords() {
  gFragCount.store(0, std::memory_order_relaxed);
  for (int y = 0; y < kMaxDim; y++) {
    for (int x = 0; x < kMaxDim; x++) {
      for (int s = 0; s < kMaxSample; s++) {
        gRecCnt[y][x][s].store(0, std::memory_order_relaxed);
      }
    }
  }
}

class TCommon : public ShaderSoft {
 public:
  CREATE_SHADER_OVERRIDE
  std::vector<std::string> &getDefines() override {
    static std::vector<std::string> defs;
    return defs;
  }
  std::vector<UniformDesc> &getUniformsDesc() override {
    static std::vector<UniformDesc> desc = {
        {"tex", offsetof(TUniforms, tex)},
        {"params", offsetof(TUniforms, color)},
    };
    return desc;
  }
};

class TVS : public TCommon {
 public:
  CREATE_SHADER_CLONE(TVS)
  void shaderMain() override {
    gl->Position = a->position;
    v->uv = a->uv;
    v->aux = a->aux;
  }
};

class TFS : public TCommon {
 public:
  CREATE_SHADER_CLONE(TFS)
  void setupSamplerDerivative() override {
    if (u->tex) {
      u->tex->setLodFunc(&texLodFunc);
    }
  }
  size_t getSamplerDerivativeOffset(BaseSampler<RGBA> *) const override {
    return offsetof(TVaryings, uv);
  }
  void shaderMain() override {
    int px = (int) std::floor(gl->FragCoord.x);
    int py = (int) std::floor(gl->FragCoord.y);
    int s = gl->SampleID < 0 ? 0 : gl->SampleID;
    if (px >= 0 && py >= 0 && px < kMaxDim && py < kMaxDim && s >= 0 && s < kMaxSample) {
      FragRec r;
      r.sampleID = gl->SampleID;
      r.samplePos = gl->SamplePosition;
      r.fragCoord = gl->FragCoord;
      r.maskIn = gl->SampleMaskIn;
      r.uv = v->uv;
      r.aux = v->aux;
      const float *p0 = gl->dfCtx.p0;
      const float *p1 = gl->dfCtx.p1;
      const float *p2 = gl->dfCtx.p2;
      if (p0 && p1 && p2) {
        r.ddx = glm::vec2(p1[0] - p0[0], p1[1] - p0[1]);
        r.ddy = glm::vec2(p2[0] - p0[0], p2[1] - p0[1]);
        r.dauxdx = p1[2] - p0[2];
        r.dauxdy = p2[2] - p0[2];
      }
      gRecs[py][px][s] = r;
      gRecCnt[py][px][s].fetch_add(1, std::memory_order_relaxed);
    }
    gFragCount.fetch_add(1, std::memory_order_relaxed);

    if (u->flags & kFlagWriteMask) {
      gl->SampleMask = u->outMask;
    }
    if (gl->SampleID == u->discardSample) {
      gl->discard = true;
    }
    if (u->flags & kFlagUseTexture) {
      gl->FragColor = texture(u->tex, v->uv);
    } else if (u->flags & kFlagColorUV) {
      gl->FragColor = glm::vec4(v->uv, v->aux, 1.f);
    } else {
      gl->FragColor = u->color;
    }
  }
};

// ---------------------------------------------------------------------------
// render harness
// ---------------------------------------------------------------------------

struct ModelTri {
  glm::dvec4 clip[3];
  glm::dvec2 uv[3];
  double aux[3];
};

struct Harness {
  RendererSoft renderer;
  int W = 0, H = 0;
  bool msaa = false;
  std::shared_ptr<Texture> colorTex, depthTex, dummyTex;
  std::shared_ptr<TextureSoft<RGBA>> colorT;
  std::shared_ptr<TextureSoft<float>> depthT;
  std::shared_ptr<FrameBuffer> fbo;
  std::shared_ptr<ShaderProgram> prog;
  std::shared_ptr<ShaderResources> res;
  std::shared_ptr<UniformBlock> paramsBlock;
  std::shared_ptr<UniformSampler> texSampler;
  TUniforms uniforms;

  void init(int w, int h, bool ms) {
    W = w;
    H = h;
    msaa = ms;
    TextureDesc cd;
    cd.width = w;
    cd.height = h;
    cd.usage = TextureUsage_AttachmentColor;
    cd.multiSample = ms;
    colorTex = renderer.createTexture(cd);
    colorTex->initImageData();
    TextureDesc dd = cd;
    dd.format = TextureFormat_FLOAT32;
    dd.usage = TextureUsage_AttachmentDepth;
    depthTex = renderer.createTexture(dd);
    depthTex->initImageData();
    fbo = renderer.createFrameBuffer(true);
    fbo->setColorAttachment(colorTex, 0);
    fbo->setDepthAttachment(depthTex);
    colorT = std::dynamic_pointer_cast<TextureSoft<RGBA>>(colorTex);
    depthT = std::dynamic_pointer_cast<TextureSoft<float>>(depthTex);

    prog = renderer.createShaderProgram();
    auto ps = std::dynamic_pointer_cast<ShaderProgramSoft>(prog);
    ps->SetShaders(std::make_shared<TVS>(), std::make_shared<TFS>());

    TextureDesc td;
    td.width = 2;
    td.height = 2;
    dummyTex = renderer.createTexture(td);
    auto dbuf = Buffer<RGBA>::makeDefault(2, 2);
    dbuf->setAll(RGBA(0, 0, 0, 255));
    dummyTex->setImageData({dbuf});

    res = std::make_shared<ShaderResources>();
    paramsBlock = renderer.createUniformBlock("params", (int) (sizeof(TUniforms) - offsetof(TUniforms, color)));
    res->blocks[1] = paramsBlock;
    texSampler = renderer.createUniformSampler("tex", td);
    texSampler->setTexture(dummyTex);
    res->samplers[0] = texSampler;

    renderer.setViewPort(0, 0, w, h);
    renderer.setShaderProgram(prog);
    uploadParams();
  }

  void uploadParams() {
    paramsBlock->setData(&uniforms.color, (int) (sizeof(TUniforms) - offsetof(TUniforms, color)));
    renderer.setShaderResources(res);
  }

  void begin(const glm::vec4 &clearColor, float clearDepth) {
    ClearStates cs;
    cs.colorFlag = true;
    cs.depthFlag = true;
    cs.clearColor = clearColor;
    cs.clearDepth = clearDepth;
    renderer.beginRenderPass(fbo, cs);
  }

  void draw(const ModelTri &t, const RenderStates &st) {
    TAttribs atts[3];
    for (int i = 0; i < 3; i++) {
      atts[i].position = glm::vec4((float) t.clip[i].x, (float) t.clip[i].y,
                                   (float) t.clip[i].z, (float) t.clip[i].w);
      atts[i].uv = glm::vec2((float) t.uv[i].x, (float) t.uv[i].y);
      atts[i].aux = (float) t.aux[i];
    }
    int32_t idx[3] = {0, 1, 2};
    VertexArray va;
    va.vertexesDesc = {{4, sizeof(TAttribs), 0}};
    va.vertexesBuffer = (uint8_t *) atts;
    va.vertexesBufferLength = sizeof(atts);
    va.indexBuffer = idx;
    va.indexBufferLength = sizeof(idx);
    auto vao = renderer.createVertexArrayObject(va);
    renderer.setVertexArrayObject(vao);
    auto ps = renderer.createPipelineStates(st);
    renderer.setPipelineStates(ps);
    renderer.draw();
  }

  void end() { renderer.endRenderPass(); }

  RGBA colorAt(int x, int y, int s) const {
    if (msaa) {
      return (*colorT->getImage().getBuffer()->bufferMs4x->get(x, y))[s];
    }
    return *colorT->getImage().getBuffer()->buffer->get(x, y);
  }

  float depthAt(int x, int y, int s) const {
    if (msaa) {
      return (*depthT->getImage().getBuffer()->bufferMs4x->get(x, y))[s];
    }
    return *depthT->getImage().getBuffer()->buffer->get(x, y);
  }
};

// ---------------------------------------------------------------------------
// independent double-precision model
// ---------------------------------------------------------------------------

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

glm::dvec2 sampleOffset(int ns, int s) {
  static const glm::dvec2 off4[4] = {
      {0.375, 0.875}, {0.875, 0.625}, {0.125, 0.375}, {0.625, 0.125}};
  return (ns == 4) ? off4[s] : glm::dvec2(0.5, 0.5);
}

// screen coordinates, double precision (independent truth)
void modelScreen(const ModelTri &t, double W, double H, glm::dvec3 s[3], double rw[3]) {
  for (int i = 0; i < 3; i++) {
    double invW = 1.0 / t.clip[i].w;
    glm::dvec3 ndc = glm::dvec3(t.clip[i]) * invW;
    s[i] = glm::dvec3(ndc.x * (W * 0.5) + W * 0.5,
                      ndc.y * (H * 0.5) + H * 0.5,
                      ndc.z);
    rw[i] = invW;
  }
}

// screen coordinates replicating the renderer's float transform; used only to
// reproduce the exact same raster block / pixel quad enumeration
void screenF(const ModelTri &t, int W, int H, glm::vec3 out[3]) {
  for (int i = 0; i < 3; i++) {
    glm::vec4 pos((float) t.clip[i].x, (float) t.clip[i].y,
                  (float) t.clip[i].z, (float) t.clip[i].w);
    float invW = 1.f / pos.w;
    pos *= invW;
    pos.w = invW;
    pos *= glm::vec4((float) W * 0.5f, (float) H * 0.5f, 1.f, 1.f);
    pos += glm::vec4((float) W * 0.5f, (float) H * 0.5f, 0.f, 0.f);
    out[i] = glm::vec3(pos);
  }
}

struct LaneEval {
  double l[3] = {0, 0, 0};
  double z = 0;
  double rw = 1;
  glm::dvec2 uv = glm::dvec2(0);
  double aux = 0;
  bool inside = false;
  double edge = 1;    // min |barycentric|, coverage guard
};

LaneEval evalPoint(const glm::dvec3 s[3], const double rw[3], const ModelTri &t,
                   double px, double py) {
  LaneEval e;
  double x0 = s[0].x, y0 = s[0].y;
  double x1 = s[1].x, y1 = s[1].y;
  double x2 = s[2].x, y2 = s[2].y;
  double den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
  if (std::fabs(den) < 1e-12) {
    e.edge = 0;
    return e;
  }
  double l0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / den;
  double l1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / den;
  double l2 = 1.0 - l0 - l1;
  e.l[0] = l0;
  e.l[1] = l1;
  e.l[2] = l2;
  e.edge = std::min(std::min(std::fabs(l0), std::fabs(l1)), std::fabs(l2));
  e.z = l0 * s[0].z + l1 * s[1].z + l2 * s[2].z;
  e.rw = l0 * rw[0] + l1 * rw[1] + l2 * rw[2];
  double c0 = l0 * rw[0] / e.rw;
  double c1 = l1 * rw[1] / e.rw;
  double c2 = l2 * rw[2] / e.rw;
  e.uv = c0 * t.uv[0] + c1 * t.uv[1] + c2 * t.uv[2];
  e.aux = c0 * t.aux[0] + c1 * t.aux[1] + c2 * t.aux[2];
  e.inside = (l0 >= 0.0 && l1 >= 0.0 && l2 >= 0.0) && (e.z >= 0.0 && e.z <= 1.0);
  return e;
}

// pixel quad origins, replicating the renderer's 32px block iteration
std::vector<glm::ivec2> enumerateQuads(const glm::vec3 sf[3], int W, int H) {
  float minX = std::min(std::min(sf[0].x, sf[1].x), sf[2].x);
  float minY = std::min(std::min(sf[0].y, sf[1].y), sf[2].y);
  float maxX = std::max(std::max(sf[0].x, sf[1].x), sf[2].x);
  float maxY = std::max(std::max(sf[0].y, sf[1].y), sf[2].y);
  minX = std::max(minX - 0.5f, 0.f);
  minY = std::max(minY - 0.5f, 0.f);
  maxX = std::min(maxX + 0.5f, (float) W - 1.f);
  maxY = std::min(maxY + 0.5f, (float) H - 1.f);
  glm::vec3 bmin(minX, minY, 0.f);
  glm::vec3 bmax(maxX, maxY, 0.f);
  bmin -= 1.f;
  const int blockSize = 32;
  int blockCntX = (bmax.x - bmin.x + blockSize - 1.f) / blockSize;
  int blockCntY = (bmax.y - bmin.y + blockSize - 1.f) / blockSize;
  std::vector<glm::ivec2> quads;
  for (int by = 0; by < blockCntY; by++) {
    for (int bx = 0; bx < blockCntX; bx++) {
      int bsx = bmin.x + bx * blockSize;
      int bsy = bmin.y + by * blockSize;
      for (int y = bsy + 1; y < bsy + blockSize && y <= bmax.y; y += 2) {
        for (int x = bsx + 1; x < bsx + blockSize && x <= bmax.x; x += 2) {
          quads.emplace_back(x, y);
        }
      }
    }
  }
  return quads;
}

RGBA toRGBA(const glm::vec4 &c) {
  glm::vec4 cl = glm::clamp(c, 0.f, 1.f) * 255.f;
  return RGBA((uint8_t) cl.r, (uint8_t) cl.g, (uint8_t) cl.b, (uint8_t) cl.a);
}

struct ShaderModel {
  glm::vec4 color = glm::vec4(0.f);
  bool colorUV = false;
  bool writeMask = false;
  uint32_t outMask = 0xFFFFFFFFu;
  int discardSample = -999;
};

struct Sim {
  int W = 0, H = 0, ns = 1;
  uint32_t targetMask = 1;
  int ES = 0;   // stride of the extended (W+2) arrays

  std::vector<std::array<RGBA, 4>> color;
  std::vector<std::array<double, 4>> depth;

  // per-draw state
  std::vector<std::array<int, 4>> execCnt;
  std::vector<uint32_t> maskInPx;
  std::vector<std::array<LaneEval, 4>> evals;   // (W+2)*(H+2)
  std::vector<std::array<uint8_t, 4>> ezPass;   // (W+2)*(H+2)
  std::vector<glm::ivec2> quadOf;               // (W+2)*(H+2)
  std::vector<int> laneOf;                      // (W+2)*(H+2)
  std::vector<LaneEval> repEval;                // per-pixel representative
  std::vector<glm::dvec2> repOff;
  std::vector<glm::ivec2> quads;
  long fragCount = 0;

  void init(int w, int h, int nsamples) {
    W = w;
    H = h;
    ns = nsamples;
    targetMask = (1u << ns) - 1u;
    ES = W + 2;
    color.assign((size_t) W * H, {});
    depth.assign((size_t) W * H, {});
    execCnt.assign((size_t) W * H, {});
    maskInPx.assign((size_t) W * H, 0);
    evals.assign((size_t) (W + 2) * (H + 2), {});
    ezPass.assign((size_t) (W + 2) * (H + 2), {});
    quadOf.assign((size_t) (W + 2) * (H + 2), glm::ivec2(-1));
    laneOf.assign((size_t) (W + 2) * (H + 2), -1);
    repEval.assign((size_t) W * H, {});
    repOff.assign((size_t) W * H, glm::dvec2(0));
  }

  void clear(const RGBA &c, double d) {
    for (auto &px : color) px.fill(c);
    for (auto &px : depth) px.fill(d);
  }

  void beginDraw() {
    for (auto &e : execCnt) e.fill(0);
    std::fill(maskInPx.begin(), maskInPx.end(), 0);
    for (auto &e : ezPass) e.fill(1);
    std::fill(laneOf.begin(), laneOf.end(), -1);
    std::fill(quadOf.begin(), quadOf.end(), glm::ivec2(-1));
    quads.clear();
    fragCount = 0;
  }

  void commit(const RenderStates &st, const ShaderModel &sh, double z,
              const LaneEval &colorEv, int px, int py, int sm) {
    if (px < 0 || py < 0 || px >= W || py >= H) {
      return;   // framebuffer clip
    }
    auto &dslot = depth[(size_t) py * W + px][sm];
    auto &cslot = color[(size_t) py * W + px][sm];
    if (st.depthTest) {
      double zc = clamp01(z);
      if (!(zc < dslot)) {   // DepthFunc_LESS
        return;
      }
      if (st.depthMask) {
        dslot = zc;
      }
    }
    cslot = sh.colorUV ? toRGBA(glm::vec4((float) colorEv.uv.x, (float) colorEv.uv.y,
                                          (float) colorEv.aux, 1.f))
                       : toRGBA(sh.color);
  }

  void drawTri(const ModelTri &t, const RenderStates &st, const ShaderModel &sh, bool earlyZ) {
    glm::dvec3 sd[3];
    double rw[3];
    modelScreen(t, (double) W, (double) H, sd, rw);
    glm::vec3 sf[3];
    screenF(t, W, H, sf);
    quads = enumerateQuads(sf, W, H);

    static const int laneDX[4] = {0, 1, 0, 1};
    static const int laneDY[4] = {0, 0, 1, 1};

    for (auto &q : quads) {
      LaneEval ev[4][4];
      bool geo[4][4] = {};
      uint32_t geoMask[4] = {0, 0, 0, 0};
      for (int p = 0; p < 4; p++) {
        int px = q.x + laneDX[p];
        int py = q.y + laneDY[p];
        for (int sm = 0; sm < ns; sm++) {
          glm::dvec2 off = sampleOffset(ns, sm);
          ev[p][sm] = evalPoint(sd, rw, t, px + off.x, py + off.y);
          geo[p][sm] = ev[p][sm].inside;
          if (geo[p][sm]) {
            geoMask[p] |= (1u << sm);
          }
          evals[(size_t) py * ES + px][sm] = ev[p][sm];
        }
        quadOf[(size_t) py * ES + px] = q;
        laneOf[(size_t) py * ES + px] = p;
      }

      uint32_t maskIn[4];
      for (int p = 0; p < 4; p++) {
        maskIn[p] = geoMask[p] & st.sampleMask & targetMask;
        int px = q.x + laneDX[p];
        int py = q.y + laneDY[p];
        if (px >= 0 && py >= 0 && px < W && py < H) {
          maskInPx[(size_t) py * W + px] = maskIn[p];
        }
      }

      // early z (read-only depth test against current depth)
      bool ez[4][4];
      for (int p = 0; p < 4; p++) {
        for (int sm = 0; sm < ns; sm++) {
          ez[p][sm] = geo[p][sm];
          if (earlyZ && st.depthTest && geo[p][sm]) {
            int px = q.x + laneDX[p];
            int py = q.y + laneDY[p];
            ez[p][sm] = px >= 0 && py >= 0 && px < W && py < H &&
                        clamp01(ev[p][sm].z) < depth[(size_t) py * W + px][sm];
          }
          int px = q.x + laneDX[p];
          int py = q.y + laneDY[p];
          ezPass[(size_t) py * ES + px][sm] = ez[p][sm] ? 1 : 0;
        }
      }

      if (st.sampleShading) {
        for (int sm = 0; sm < ns; sm++) {
          for (int p = 0; p < 4; p++) {
            bool exec = geo[p][sm] && ((maskIn[p] >> sm) & 1u);
            if (earlyZ && st.depthTest) {
              exec = exec && ez[p][sm];
            }
            if (!exec) {
              continue;
            }
            fragCount++;
            int px = q.x + laneDX[p];
            int py = q.y + laneDY[p];
            if (px >= 0 && py >= 0 && px < W && py < H) {
              execCnt[(size_t) py * W + px][sm]++;
            }
            if (sm == sh.discardSample) {
              continue;
            }
            uint32_t effMask = (sh.writeMask ? sh.outMask : targetMask) & targetMask;
            if (!((effMask >> sm) & 1u)) {
              continue;
            }
            commit(st, sh, ev[p][sm].z, ev[p][sm], px, py, sm);
          }
        }
      } else {
        for (int p = 0; p < 4; p++) {
          bool pixInside = geoMask[p] != 0;
          if (earlyZ && st.depthTest) {
            pixInside = false;
            for (int sm = 0; sm < ns; sm++) {
              pixInside = pixInside || ez[p][sm];
            }
          }
          if (!pixInside) {
            continue;
          }
          fragCount++;
          int px = q.x + laneDX[p];
          int py = q.y + laneDY[p];
          bool inRange = px >= 0 && py >= 0 && px < W && py < H;
          if (inRange) {
            execCnt[(size_t) py * W + px][0]++;
          }
          // representative point (matches renderer InitShadingSample)
          LaneEval repEv;
          glm::dvec2 roff(0.5, 0.5);
          if (ns == 1) {
            repEv = ev[p][0];
          } else {
            LaneEval ce = evalPoint(sd, rw, t, px + 0.5, py + 0.5);
            if (ce.inside) {
              repEv = ce;
            } else {
              int first = 0;
              for (int sm = 0; sm < ns; sm++) {
                if (geo[p][sm]) {
                  first = sm;
                  break;
                }
              }
              repEv = ev[p][first];
              roff = sampleOffset(ns, first);
            }
          }
          if (inRange) {
            repEval[(size_t) py * W + px] = repEv;
            repOff[(size_t) py * W + px] = roff;
          }
          if (-1 == sh.discardSample) {
            continue;   // SampleID == -1 in per-pixel mode
          }
          uint32_t effMask = (sh.writeMask ? sh.outMask : targetMask) & st.sampleMask & targetMask;
          for (int sm = 0; sm < ns; sm++) {
            bool cov = (earlyZ && st.depthTest) ? ez[p][sm] : geo[p][sm];
            if (!cov || !((effMask >> sm) & 1u)) {
              continue;
            }
            commit(st, sh, ev[p][sm].z, repEv, px, py, sm);
          }
        }
      }
    }
  }
};

// ---------------------------------------------------------------------------
// shared check helpers
// ---------------------------------------------------------------------------

RenderStates fillStates(bool sampleShading, uint32_t mask, bool depthTest) {
  RenderStates st;
  st.cullFace = false;
  st.depthTest = depthTest;
  st.depthMask = true;
  st.depthFunc = DepthFunc_LESS;
  st.primitiveType = Primitive_TRIANGLE;
  st.polygonMode = PolygonMode_FILL;
  st.sampleShading = sampleShading;
  st.sampleMask = mask;
  return st;
}

int checkFramebuffer(const Harness &h, const Sim &sim, int colorTol) {
  for (int y = 0; y < sim.H; y++) {
    for (int x = 0; x < sim.W; x++) {
      for (int s = 0; s < sim.ns; s++) {
        RGBA ec = sim.color[(size_t) y * sim.W + x][s];
        RGBA ac = h.colorAt(x, y, s);
        TASK_CHECK(std::abs((int) ec.r - (int) ac.r) <= colorTol);
        TASK_CHECK(std::abs((int) ec.g - (int) ac.g) <= colorTol);
        TASK_CHECK(std::abs((int) ec.b - (int) ac.b) <= colorTol);
        TASK_CHECK(std::abs((int) ec.a - (int) ac.a) <= colorTol);
        float ed = (float) sim.depth[(size_t) y * sim.W + x][s];
        TASK_CHECK(std::fabs(ed - h.depthAt(x, y, s)) <= 1e-4f);
      }
    }
  }
  return 0;
}

int checkExecCounts(const Sim &sim) {
  for (int y = 0; y < sim.H; y++) {
    for (int x = 0; x < sim.W; x++) {
      for (int s = 0; s < sim.ns; s++) {
        TASK_CHECK(gRecCnt[y][x][s].load() == sim.execCnt[(size_t) y * sim.W + x][s]);
        if (sim.execCnt[(size_t) y * sim.W + x][s] > 0) {
          TASK_CHECK(gRecs[y][x][s].maskIn == sim.maskInPx[(size_t) y * sim.W + x]);
        }
      }
    }
  }
  return 0;
}

// where the sample-offset model and the pixel-center model clearly differ
// (separation > 4*tol), the recorded value must be closer to the sample model
int checkDecisive(double exp, double ctr, float rec, float tol, int *decisive) {
  if (std::fabs(exp - ctr) <= 4.0 * tol) {
    return 0;
  }
  (*decisive)++;
  return std::fabs(rec - (float) exp) < std::fabs(rec - (float) ctr) ? 0 : 1;
}

// verify same-lane derivative differences against the independent model;
// also counts invocations in partially covered quads (helper lanes) and
// components where sample-offset diffs are distinguishable from pixel-center
int checkLaneDiffs(const Sim &sim, const ModelTri &tri, float tol,
                   int *partialOut, int *decisiveOut) {
  glm::dvec3 sd[3];
  double rw[3];
  modelScreen(tri, (double) sim.W, (double) sim.H, sd, rw);
  int partial = 0;
  int decisive = 0;
  for (int y = 0; y < sim.H; y++) {
    for (int x = 0; x < sim.W; x++) {
      for (int s = 0; s < sim.ns; s++) {
        if (sim.execCnt[(size_t) y * sim.W + x][s] == 0) {
          continue;
        }
        const FragRec &r = gRecs[y][x][s];
        glm::ivec2 q = sim.quadOf[(size_t) y * sim.ES + x];
        TASK_CHECK(q.x >= 0 && q.y >= 0);
        const LaneEval &e0 = sim.evals[(size_t) q.y * sim.ES + q.x][s];
        const LaneEval &e1 = sim.evals[(size_t) q.y * sim.ES + q.x + 1][s];
        const LaneEval &e2 = sim.evals[(size_t) (q.y + 1) * sim.ES + q.x][s];
        const LaneEval &e3 = sim.evals[(size_t) (q.y + 1) * sim.ES + q.x + 1][s];
        glm::dvec2 eddx = e1.uv - e0.uv;
        glm::dvec2 eddy = e2.uv - e0.uv;
        TASK_CHECK(std::fabs(r.ddx.x - (float) eddx.x) <= tol);
        TASK_CHECK(std::fabs(r.ddx.y - (float) eddx.y) <= tol);
        TASK_CHECK(std::fabs(r.ddy.x - (float) eddy.x) <= tol);
        TASK_CHECK(std::fabs(r.ddy.y - (float) eddy.y) <= tol);
        TASK_CHECK(std::fabs(r.dauxdx - (float) (e1.aux - e0.aux)) <= tol);
        TASK_CHECK(std::fabs(r.dauxdy - (float) (e2.aux - e0.aux)) <= tol);
        if (!(e0.inside && e1.inside && e2.inside && e3.inside)) {
          partial++;
        }
        LaneEval c0 = evalPoint(sd, rw, tri, q.x + 0.5, q.y + 0.5);
        LaneEval c1 = evalPoint(sd, rw, tri, q.x + 1.5, q.y + 0.5);
        LaneEval c2 = evalPoint(sd, rw, tri, q.x + 0.5, q.y + 1.5);
        glm::dvec2 cddx = c1.uv - c0.uv;
        glm::dvec2 cddy = c2.uv - c0.uv;
        TASK_CHECK(checkDecisive(eddx.x, cddx.x, r.ddx.x, tol, &decisive) == 0);
        TASK_CHECK(checkDecisive(eddx.y, cddx.y, r.ddx.y, tol, &decisive) == 0);
        TASK_CHECK(checkDecisive(eddy.x, cddy.x, r.ddy.x, tol, &decisive) == 0);
        TASK_CHECK(checkDecisive(eddy.y, cddy.y, r.ddy.y, tol, &decisive) == 0);
        TASK_CHECK(checkDecisive(e1.aux - e0.aux, c1.aux - c0.aux, r.dauxdx, tol, &decisive) == 0);
        TASK_CHECK(checkDecisive(e2.aux - e0.aux, c2.aux - c0.aux, r.dauxdy, tol, &decisive) == 0);
      }
    }
  }
  if (partialOut) *partialOut += partial;
  if (decisiveOut) *decisiveOut += decisive;
  return 0;
}

// ---------------------------------------------------------------------------
// geometry
// ---------------------------------------------------------------------------
// All triangles are nudged so that every MSAA sample point keeps a barycentric
// clearance > 1e-4 from the edges at the viewport used by the case (the
// float-vs-double coverage disagreement is ~1e-6, so counts stay exact).

// unequal clip-space w, fully inside the frustum; variant for 96x64
ModelTri triG1_96() {
  ModelTri t;
  t.clip[0] = {-0.6857, -0.5063, 0.20, 1.00};
  t.clip[1] = { 0.7912, -0.3948, 0.30, 1.50};
  t.clip[2] = {-0.2105,  0.6024, 0.50, 0.80};
  t.uv[0] = {0.05, 0.10};
  t.uv[1] = {0.90, 0.20};
  t.uv[2] = {0.30, 0.85};
  t.aux[0] = 0.2;
  t.aux[1] = 0.9;
  t.aux[2] = 0.5;
  return t;
}

// same w pattern, variant for 64x64
ModelTri triG1_64() {
  ModelTri t;
  t.clip[0] = {-0.7066, -0.5070, 0.20, 1.00};
  t.clip[1] = { 0.8019, -0.3879, 0.30, 1.50};
  t.clip[2] = {-0.2063,  0.6001, 0.50, 0.80};
  t.uv[0] = {0.05, 0.10};
  t.uv[1] = {0.90, 0.20};
  t.uv[2] = {0.30, 0.85};
  t.aux[0] = 0.2;
  t.aux[1] = 0.9;
  t.aux[2] = 0.5;
  return t;
}

// same w pattern, variant for 48x48
ModelTri triG1_48() {
  ModelTri t;
  t.clip[0] = {-0.6909, -0.5071, 0.20, 1.00};
  t.clip[1] = { 0.8128, -0.3914, 0.30, 1.50};
  t.clip[2] = {-0.2134,  0.5988, 0.50, 0.80};
  t.uv[0] = {0.05, 0.10};
  t.uv[1] = {0.90, 0.20};
  t.uv[2] = {0.30, 0.85};
  t.aux[0] = 0.2;
  t.aux[1] = 0.9;
  t.aux[2] = 0.5;
  return t;
}

// strong perspective (w in {0.6, 2.5, 1.0}) with a large uv gradient, so that
// sample-offset derivatives differ decisively from pixel-center derivatives
ModelTri triD() {
  ModelTri t;
  t.clip[0] = {-0.5099, -0.4015, 0.20, 0.60};
  t.clip[1] = { 0.6120, -0.3012, 0.30, 2.50};
  t.clip[2] = {-0.1867,  0.4909, 0.50, 1.00};
  t.uv[0] = {0.0, 0.0};
  t.uv[1] = {8.0, 1.0};
  t.uv[2] = {1.0, 9.0};
  t.aux[0] = 0.2;
  t.aux[1] = 0.9;
  t.aux[2] = 0.5;
  return t;
}

// w = 1, flat z, screen-linear uv gradient (implicit lod == 1.0 on 8x8 mip)
ModelTri triG5(int W, int H) {
  ModelTri t;
  t.clip[0] = {-0.8125, -0.7890, 0.5, 1.0};
  t.clip[1] = { 0.7875, -0.7890, 0.5, 1.0};
  t.clip[2] = {-0.8125,  0.8110, 0.5, 1.0};
  for (int i = 0; i < 3; i++) {
    double sx = t.clip[i].x * (W * 0.5) + W * 0.5;
    double sy = t.clip[i].y * (H * 0.5) + H * 0.5;
    t.uv[i] = {sx * 0.25 + sy * 0.0625, sy * 0.0625};
    t.aux[i] = 0.5;
  }
  return t;
}

// same shape as G1@64 but farther (for occlusion), z in [0.37, 0.75]
ModelTri triTestFar() {
  ModelTri t = triG1_64();
  t.clip[0].z = 0.50;
  t.clip[1].z = 0.55;
  t.clip[2].z = 0.60;
  return t;
}

// near occluder covering the right side, flat z = 0.3
ModelTri triOccluder() {
  ModelTri t;
  t.clip[0] = {-0.0155, -0.9140, 0.3, 1.0};
  t.clip[1] = { 0.8845, -0.9140, 0.3, 1.0};
  t.clip[2] = { 0.8845,  0.8860, 0.3, 1.0};
  t.uv[0] = {0.0, 0.0};
  t.uv[1] = {1.0, 0.0};
  t.uv[2] = {1.0, 1.0};
  t.aux[0] = 0.1;
  t.aux[1] = 0.2;
  t.aux[2] = 0.3;
  return t;
}

// huge triangle, clipped by the frustum in x/y, covers the whole viewport
ModelTri triClip() {
  ModelTri t;
  t.clip[0] = {-2.9893, -2.9871, 0.5, 1.0};
  t.clip[1] = { 3.0051, -3.0100, 0.5, 1.0};
  t.clip[2] = { 0.0031,  2.9938, 0.5, 1.0};
  t.uv[0] = {0.0, 0.0};
  t.uv[1] = {1.0, 0.0};
  t.uv[2] = {0.5, 1.0};
  t.aux[0] = 0.1;
  t.aux[1] = 0.2;
  t.aux[2] = 0.3;
  return t;
}

ModelTri triForeground() {
  ModelTri t;
  t.clip[0] = {-0.6015, -0.4909, 0.4, 1.0};
  t.clip[1] = { 0.6039, -0.4930, 0.4, 1.0};
  t.clip[2] = { 0.0009,  0.6135, 0.4, 1.0};
  t.uv[0] = {0.0, 0.0};
  t.uv[1] = {1.0, 0.0};
  t.uv[2] = {0.5, 1.0};
  t.aux[0] = 0.2;
  t.aux[1] = 0.4;
  t.aux[2] = 0.6;
  return t;
}

ModelTri triBackground() {
  ModelTri t;
  t.clip[0] = {-0.9195, -0.9140, 0.6, 1.0};
  t.clip[1] = { 0.8805, -0.9140, 0.6, 1.0};
  t.clip[2] = {-0.0195,  0.8860, 0.6, 1.0};
  t.uv[0] = {0.0, 0.0};
  t.uv[1] = {1.0, 0.0};
  t.uv[2] = {0.5, 1.0};
  t.aux[0] = 0.1;
  t.aux[1] = 0.3;
  t.aux[2] = 0.5;
  return t;
}

std::shared_ptr<Texture> makeMipTexture(RendererSoft &r, const RGBA levelColor[4]) {
  TextureDesc td;
  td.width = 8;
  td.height = 8;
  td.useMipmaps = true;
  auto tex = r.createTexture(td);
  auto buf0 = Buffer<RGBA>::makeDefault(8, 8);
  buf0->setAll(levelColor[0]);
  tex->setImageData({buf0});
  auto texSoft = std::dynamic_pointer_cast<TextureSoft<RGBA>>(tex);
  for (size_t l = 0; l < texSoft->getImage().levels.size(); l++) {
    texSoft->getImage().levels[l]->buffer->setAll(levelColor[std::min(l, (size_t) 3)]);
  }
  SamplerDesc sd;
  sd.filterMin = Filter_NEAREST_MIPMAP_NEAREST;
  tex->setSamplerDesc(sd);
  return tex;
}

// ---------------------------------------------------------------------------
// cases: interpolation
// ---------------------------------------------------------------------------

int case_interp_persample() {
  const int W = 96, H = 64;
  Harness h;
  h.init(W, H, true);
  h.uniforms.flags = kFlagColorUV;
  h.uploadParams();

  ModelTri tri = triG1_96();
  RenderStates st = fillStates(true, 0xFFFFFFFFu, false);

  Sim sim;
  sim.init(W, H, 4);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);
  sim.beginDraw();
  ShaderModel shm;
  shm.colorUV = true;
  sim.drawTri(tri, st, shm, true);

  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(tri, st);
  h.end();

  TASK_CHECK(gFragCount.load() == sim.fragCount);
  TASK_CHECK(sim.quads.size() > 16);
  int minQx = 1 << 20, maxQx = -(1 << 20);
  for (auto &q : sim.quads) {
    minQx = std::min(minQx, q.x);
    maxQx = std::max(maxQx, q.x);
  }
  TASK_CHECK(maxQx - minQx >= 32);   // crosses 32px raster blocks

  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      for (int s = 0; s < 4; s++) {
        int expect = sim.execCnt[(size_t) y * W + x][s];
        TASK_CHECK(gRecCnt[y][x][s].load() == expect);
        if (!expect) {
          continue;
        }
        const FragRec &r = gRecs[y][x][s];
        const LaneEval &ev = sim.evals[(size_t) y * sim.ES + x][s];
        glm::dvec2 off = sampleOffset(4, s);
        TASK_CHECK(r.sampleID == s);
        TASK_CHECK(std::fabs(r.samplePos.x - (float) off.x) < 1e-5f);
        TASK_CHECK(std::fabs(r.samplePos.y - (float) off.y) < 1e-5f);
        TASK_CHECK(std::fabs(r.fragCoord.x - ((float) x + (float) off.x)) < 1e-4f);
        TASK_CHECK(std::fabs(r.fragCoord.y - ((float) y + (float) off.y)) < 1e-4f);
        TASK_CHECK(std::fabs(r.fragCoord.z - (float) ev.z) < 1e-3f);
        TASK_CHECK(std::fabs(r.fragCoord.w - (float) ev.rw) < 1e-3f);
        TASK_CHECK(std::fabs(r.uv.x - (float) ev.uv.x) < 1e-3f);
        TASK_CHECK(std::fabs(r.uv.y - (float) ev.uv.y) < 1e-3f);
        TASK_CHECK(std::fabs(r.aux - (float) ev.aux) < 1e-3f);
        TASK_CHECK(r.maskIn == sim.maskInPx[(size_t) y * W + x]);
        TASK_CHECK(ev.edge > 1e-4);
      }
    }
  }
  if (checkFramebuffer(h, sim, 2)) {
    return 1;
  }
  return 0;
}

int case_interp_single_target() {
  const int W = 48, H = 48;
  Harness h;
  h.init(W, H, false);   // single-sample target
  h.uniforms.flags = kFlagColorUV;
  h.uploadParams();

  ModelTri tri = triG1_48();
  RenderStates st = fillStates(true, 0xFFFFFFFFu, false);

  Sim sim;
  sim.init(W, H, 1);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);
  sim.beginDraw();
  ShaderModel shm;
  shm.colorUV = true;
  sim.drawTri(tri, st, shm, true);

  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(tri, st);
  h.end();

  TASK_CHECK(gFragCount.load() == sim.fragCount);
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      int expect = sim.execCnt[(size_t) y * W + x][0];
      TASK_CHECK(gRecCnt[y][x][0].load() == expect);
      if (!expect) {
        continue;
      }
      const FragRec &r = gRecs[y][x][0];
      const LaneEval &ev = sim.evals[(size_t) y * sim.ES + x][0];
      TASK_CHECK(r.sampleID == 0);
      TASK_CHECK(std::fabs(r.samplePos.x - 0.5f) < 1e-5f);
      TASK_CHECK(std::fabs(r.samplePos.y - 0.5f) < 1e-5f);
      TASK_CHECK(std::fabs(r.fragCoord.x - ((float) x + 0.5f)) < 1e-4f);
      TASK_CHECK(std::fabs(r.fragCoord.y - ((float) y + 0.5f)) < 1e-4f);
      TASK_CHECK(std::fabs(r.fragCoord.z - (float) ev.z) < 1e-3f);
      TASK_CHECK(std::fabs(r.fragCoord.w - (float) ev.rw) < 1e-3f);
      TASK_CHECK(std::fabs(r.uv.x - (float) ev.uv.x) < 1e-3f);
      TASK_CHECK(std::fabs(r.uv.y - (float) ev.uv.y) < 1e-3f);
      TASK_CHECK(r.maskIn == 1u);
      TASK_CHECK(ev.edge > 1e-4);
    }
  }
  if (checkFramebuffer(h, sim, 2)) {
    return 1;
  }
  return 0;
}

int case_interp_perpixel() {
  const int W = 96, H = 64;
  Harness h;
  h.init(W, H, true);
  h.uniforms.flags = kFlagColorUV;
  h.uploadParams();

  ModelTri tri = triG1_96();
  RenderStates st = fillStates(false, 0xFFFFFFFFu, false);   // per-pixel mode

  Sim sim;
  sim.init(W, H, 4);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);
  sim.beginDraw();
  ShaderModel shm;
  shm.colorUV = true;
  sim.drawTri(tri, st, shm, true);

  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(tri, st);
  h.end();

  TASK_CHECK(gFragCount.load() == sim.fragCount);
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      int expect = sim.execCnt[(size_t) y * W + x][0];
      TASK_CHECK(gRecCnt[y][x][0].load() == expect);
      for (int s = 1; s < 4; s++) {
        TASK_CHECK(gRecCnt[y][x][s].load() == 0);
      }
      if (!expect) {
        continue;
      }
      const FragRec &r = gRecs[y][x][0];
      const LaneEval &rep = sim.repEval[(size_t) y * W + x];
      glm::dvec2 roff = sim.repOff[(size_t) y * W + x];
      TASK_CHECK(r.sampleID == -1);
      TASK_CHECK(std::fabs(r.samplePos.x - (float) roff.x) < 1e-5f);
      TASK_CHECK(std::fabs(r.samplePos.y - (float) roff.y) < 1e-5f);
      TASK_CHECK(std::fabs(r.fragCoord.x - ((float) x + (float) roff.x)) < 1e-4f);
      TASK_CHECK(std::fabs(r.fragCoord.y - ((float) y + (float) roff.y)) < 1e-4f);
      TASK_CHECK(std::fabs(r.fragCoord.z - (float) rep.z) < 1e-3f);
      TASK_CHECK(std::fabs(r.fragCoord.w - (float) rep.rw) < 1e-3f);
      TASK_CHECK(std::fabs(r.uv.x - (float) rep.uv.x) < 1e-3f);
      TASK_CHECK(std::fabs(r.uv.y - (float) rep.uv.y) < 1e-3f);
      TASK_CHECK(r.maskIn == sim.maskInPx[(size_t) y * W + x]);
    }
  }
  if (checkFramebuffer(h, sim, 2)) {
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// cases: derivatives
// ---------------------------------------------------------------------------

int case_deriv_lane_diffs() {
  const int W = 96, H = 64;
  Harness h;
  h.init(W, H, true);
  h.uniforms.flags = kFlagColorUV;
  h.uploadParams();

  ModelTri tri = triD();   // strong perspective: sample offsets matter
  RenderStates st = fillStates(true, 0xFFFFFFFFu, false);

  Sim sim;
  sim.init(W, H, 4);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);
  sim.beginDraw();
  ShaderModel shm;
  shm.colorUV = true;
  sim.drawTri(tri, st, shm, true);

  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(tri, st);
  h.end();

  TASK_CHECK(gFragCount.load() == sim.fragCount);
  if (checkExecCounts(sim)) {
    return 1;
  }
  // perspective denominator must be safely non-zero at every quad lane
  for (int y = 0; y < H + 2; y++) {
    for (int x = 0; x < W + 2; x++) {
      if (sim.laneOf[(size_t) y * sim.ES + x] < 0) {
        continue;
      }
      for (int s = 0; s < 4; s++) {
        TASK_CHECK(sim.evals[(size_t) y * sim.ES + x][s].rw > 0.05);
      }
    }
  }
  int partial = 0;
  int decisive = 0;
  if (checkLaneDiffs(sim, tri, 2e-4f, &partial, &decisive)) {
    return 1;
  }
  TASK_CHECK(partial > 0);        // helper lanes (uncovered neighbors) exercised
  TASK_CHECK(decisive > 100);     // sample-offset diffs proven, not pixel-center
  return 0;
}

int case_deriv_lod_uniform() {
  const int W = 64, H = 64;
  Harness h;
  h.init(W, H, true);
  const RGBA levelColor[4] = {{200, 30, 30, 255}, {30, 200, 30, 255},
                              {30, 30, 200, 255}, {240, 240, 240, 255}};
  auto mip = makeMipTexture(h.renderer, levelColor);
  h.texSampler->setTexture(mip);
  h.uniforms.flags = kFlagUseTexture;
  h.uploadParams();

  ModelTri tri = triG5(W, H);
  RenderStates st = fillStates(true, 0xFFFFFFFFu, false);

  Sim sim;
  sim.init(W, H, 4);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);
  sim.beginDraw();
  ShaderModel shm;
  sim.drawTri(tri, st, shm, true);

  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(tri, st);
  h.end();

  TASK_CHECK(gFragCount.load() == sim.fragCount);
  if (checkExecCounts(sim)) {
    return 1;
  }

  // every covered sample must come from mip level 1
  RGBA expectC = toRGBA(glm::vec4(levelColor[1]) / 255.f);
  int covered = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      for (int s = 0; s < 4; s++) {
        RGBA ac = h.colorAt(x, y, s);
        if (sim.execCnt[(size_t) y * W + x][s]) {
          covered++;
          TASK_CHECK(std::abs((int) ac.r - (int) expectC.r) <= 1);
          TASK_CHECK(std::abs((int) ac.g - (int) expectC.g) <= 1);
          TASK_CHECK(std::abs((int) ac.b - (int) expectC.b) <= 1);
          TASK_CHECK(std::abs((int) ac.a - (int) expectC.a) <= 1);
          // sanity: model lod is comfortably inside the level-1 bucket
          glm::ivec2 q = sim.quadOf[(size_t) y * sim.ES + x];
          const LaneEval &e0 = sim.evals[(size_t) q.y * sim.ES + q.x][s];
          const LaneEval &e1 = sim.evals[(size_t) q.y * sim.ES + q.x + 1][s];
          const LaneEval &e2 = sim.evals[(size_t) (q.y + 1) * sim.ES + q.x][s];
          glm::dvec2 dx = (e1.uv - e0.uv) * 8.0;
          glm::dvec2 dy = (e2.uv - e0.uv) * 8.0;
          double d = std::max(glm::dot(dx, dx), glm::dot(dy, dy));
          double lod = std::max(0.5 * std::log2(d), 0.0);
          TASK_CHECK(lod > 0.7 && lod < 1.3);
        } else {
          TASK_CHECK(ac == RGBA(0, 0, 0, 0));
        }
      }
    }
  }
  TASK_CHECK(covered > 0);
  return 0;
}

int case_deriv_lod_perspective() {
  const int W = 96, H = 64;
  Harness h;
  h.init(W, H, true);
  const RGBA levelColor[4] = {{200, 30, 30, 255}, {30, 200, 30, 255},
                              {30, 30, 200, 255}, {240, 240, 240, 255}};
  auto mip = makeMipTexture(h.renderer, levelColor);
  h.texSampler->setTexture(mip);
  h.uniforms.flags = kFlagUseTexture;
  h.uploadParams();

  ModelTri tri = triG1_96();
  tri.uv[0] = {0.0, 0.0};
  tri.uv[1] = {24.0, 4.0};
  tri.uv[2] = {4.0, 28.0};
  RenderStates st = fillStates(true, 0xFFFFFFFFu, false);

  Sim sim;
  sim.init(W, H, 4);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);
  sim.beginDraw();
  ShaderModel shm;
  sim.drawTri(tri, st, shm, true);

  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(tri, st);
  h.end();

  TASK_CHECK(gFragCount.load() == sim.fragCount);
  int levelSeen[4] = {0, 0, 0, 0};
  int checked = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      for (int s = 0; s < 4; s++) {
        if (!sim.execCnt[(size_t) y * W + x][s]) {
          continue;
        }
        glm::ivec2 q = sim.quadOf[(size_t) y * sim.ES + x];
        const LaneEval &e0 = sim.evals[(size_t) q.y * sim.ES + q.x][s];
        const LaneEval &e1 = sim.evals[(size_t) q.y * sim.ES + q.x + 1][s];
        const LaneEval &e2 = sim.evals[(size_t) (q.y + 1) * sim.ES + q.x][s];
        glm::dvec2 dx = (e1.uv - e0.uv) * 8.0;
        glm::dvec2 dy = (e2.uv - e0.uv) * 8.0;
        double d = std::max(glm::dot(dx, dx), glm::dot(dy, dy));
        double lod = std::max(0.5 * std::log2(d), 0.0);
        double fb = lod + 0.5;
        double frac = fb - std::floor(fb);
        if (frac < 0.1 || frac > 0.9) {
          continue;   // too close to a level boundary for float/double compare
        }
        int level = std::min(std::max((int) std::ceil(fb) - 1, 0), 3);
        RGBA expectC = toRGBA(glm::vec4(levelColor[level]) / 255.f);
        RGBA ac = h.colorAt(x, y, s);
        TASK_CHECK(std::abs((int) ac.r - (int) expectC.r) <= 1);
        TASK_CHECK(std::abs((int) ac.g - (int) expectC.g) <= 1);
        TASK_CHECK(std::abs((int) ac.b - (int) expectC.b) <= 1);
        TASK_CHECK(std::abs((int) ac.a - (int) expectC.a) <= 1);
        levelSeen[level]++;
        checked++;
      }
    }
  }
  TASK_CHECK(checked > 200);
  int distinct = 0;
  for (int l = 0; l < 4; l++) {
    if (levelSeen[l] > 0) {
      distinct++;
    }
  }
  TASK_CHECK(distinct >= 2);
  return 0;
}

int case_deriv_depth_failed_helpers() {
  const int W = 64, H = 64;
  Harness h;
  h.init(W, H, true);
  h.renderer.setEnableEarlyZ(true);
  h.uniforms.flags = 0;
  h.uniforms.color = glm::vec4(0.f, 0.f, 1.f, 1.f);
  h.uploadParams();

  ModelTri occ = triOccluder();
  ModelTri test = triTestFar();
  RenderStates st = fillStates(true, 0xFFFFFFFFu, true);

  Sim sim;
  sim.init(W, H, 4);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);
  sim.beginDraw();
  ShaderModel shmOcc;
  shmOcc.color = glm::vec4(0.f, 0.f, 1.f, 1.f);
  sim.drawTri(occ, st, shmOcc, true);
  sim.beginDraw();
  ShaderModel shmTest;
  shmTest.color = glm::vec4(1.f, 0.f, 0.f, 1.f);
  sim.drawTri(test, st, shmTest, true);

  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(occ, st);
  resetRecords();   // count test-triangle invocations only
  h.uniforms.color = glm::vec4(1.f, 0.f, 0.f, 1.f);
  h.uploadParams();
  h.draw(test, st);
  h.end();

  TASK_CHECK(gFragCount.load() == sim.fragCount);
  if (checkExecCounts(sim)) {
    return 1;
  }
  // same-lane diffs still match the model even with depth-failed neighbors
  int partial = 0;
  if (checkLaneDiffs(sim, test, 2e-4f, &partial, nullptr)) {
    return 1;
  }
  // count invocations whose quad has a depth-failed (but covered) helper lane
  static const int laneDX[4] = {0, 1, 0, 1};
  static const int laneDY[4] = {0, 0, 1, 1};
  int depthHelpers = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      for (int s = 0; s < 4; s++) {
        if (!sim.execCnt[(size_t) y * W + x][s]) {
          continue;
        }
        glm::ivec2 q = sim.quadOf[(size_t) y * sim.ES + x];
        for (int p = 0; p < 4; p++) {
          int lx = q.x + laneDX[p];
          int ly = q.y + laneDY[p];
          bool cov = sim.evals[(size_t) ly * sim.ES + lx][s].inside;
          bool pass = sim.ezPass[(size_t) ly * sim.ES + lx][s] != 0;
          if (cov && !pass) {
            depthHelpers++;
            break;
          }
        }
      }
    }
  }
  TASK_CHECK(depthHelpers > 0);
  return 0;
}

int case_deriv_across_draws() {
  const int W = 96, H = 64;
  Harness h;
  h.init(W, H, true);
  h.uniforms.flags = kFlagColorUV;
  h.uploadParams();

  ModelTri triA = triG1_96();
  ModelTri triB = triG1_96();
  triB.uv[0] = {0.0, 0.0};
  triB.uv[1] = {24.0, 4.0};
  triB.uv[2] = {4.0, 28.0};
  triB.aux[0] = 0.7;
  triB.aux[1] = 0.1;
  triB.aux[2] = 0.9;
  RenderStates st = fillStates(true, 0xFFFFFFFFu, false);

  Sim sim;
  sim.init(W, H, 4);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);

  ShaderModel shm;
  shm.colorUV = true;

  // draw A
  sim.beginDraw();
  sim.drawTri(triA, st, shm, true);
  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(triA, st);
  TASK_CHECK(gFragCount.load() == sim.fragCount);
  if (checkLaneDiffs(sim, triA, 2e-4f, nullptr, nullptr)) {
    return 1;
  }

  // draw B (consecutive, different varyings): lanes must not go stale
  sim.beginDraw();
  sim.drawTri(triB, st, shm, true);
  resetRecords();
  h.draw(triB, st);
  h.end();
  TASK_CHECK(gFragCount.load() == sim.fragCount);
  if (checkExecCounts(sim)) {
    return 1;
  }
  if (checkLaneDiffs(sim, triB, 2e-4f, nullptr, nullptr)) {
    return 1;
  }
  // spot-check varyings of draw B
  for (int y = 0; y < H; y += 3) {
    for (int x = 0; x < W; x += 3) {
      for (int s = 0; s < 4; s++) {
        if (!sim.execCnt[(size_t) y * W + x][s]) {
          continue;
        }
        const FragRec &r = gRecs[y][x][s];
        const LaneEval &ev = sim.evals[(size_t) y * sim.ES + x][s];
        TASK_CHECK(std::fabs(r.uv.x - (float) ev.uv.x) < 1e-3f);
        TASK_CHECK(std::fabs(r.uv.y - (float) ev.uv.y) < 1e-3f);
        TASK_CHECK(std::fabs(r.aux - (float) ev.aux) < 1e-3f);
      }
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// cases: commit
// ---------------------------------------------------------------------------

int case_commit_state_mask() {
  const int W = 64, H = 64;
  Harness h;
  h.init(W, H, true);
  h.uniforms.flags = 0;
  h.uniforms.color = glm::vec4(1.f, 0.f, 0.f, 1.f);
  h.uploadParams();

  ModelTri tri = triG1_64();
  RenderStates st = fillStates(true, 0b0101u, true);

  Sim sim;
  sim.init(W, H, 4);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);
  sim.beginDraw();
  ShaderModel shm;
  shm.color = glm::vec4(1.f, 0.f, 0.f, 1.f);
  sim.drawTri(tri, st, shm, true);

  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(tri, st);
  h.end();

  TASK_CHECK(gFragCount.load() == sim.fragCount);
  if (checkExecCounts(sim)) {
    return 1;
  }
  if (checkFramebuffer(h, sim, 1)) {
    return 1;
  }
  // state mask leaves samples 1 and 3 untouched everywhere
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      TASK_CHECK(h.colorAt(x, y, 1) == RGBA(0, 0, 0, 0));
      TASK_CHECK(h.colorAt(x, y, 3) == RGBA(0, 0, 0, 0));
      TASK_CHECK(h.depthAt(x, y, 1) == 1.0f);
      TASK_CHECK(h.depthAt(x, y, 3) == 1.0f);
    }
  }
  return 0;
}

int case_commit_output_mask() {
  const int W = 64, H = 64;
  Harness h;
  h.init(W, H, true);
  h.uniforms.flags = kFlagWriteMask;
  h.uniforms.outMask = 0b1001u;
  h.uniforms.color = glm::vec4(0.f, 1.f, 0.f, 1.f);
  h.uploadParams();

  ModelTri tri = triG1_64();
  RenderStates st = fillStates(true, 0xFFFFFFFFu, true);

  Sim sim;
  sim.init(W, H, 4);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);
  sim.beginDraw();
  ShaderModel shm;
  shm.color = glm::vec4(0.f, 1.f, 0.f, 1.f);
  shm.writeMask = true;
  shm.outMask = 0b1001u;
  sim.drawTri(tri, st, shm, true);

  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(tri, st);
  h.end();

  // output mask must not reduce invocations, only commits
  TASK_CHECK(gFragCount.load() == sim.fragCount);
  if (checkExecCounts(sim)) {
    return 1;
  }
  if (checkFramebuffer(h, sim, 1)) {
    return 1;
  }
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      TASK_CHECK(h.colorAt(x, y, 1) == RGBA(0, 0, 0, 0));
      TASK_CHECK(h.colorAt(x, y, 2) == RGBA(0, 0, 0, 0));
    }
  }
  return 0;
}

int case_commit_discard_sampleid() {
  const int W = 64, H = 64;
  Harness h;
  h.init(W, H, true);
  h.uniforms.flags = 0;
  h.uniforms.discardSample = 2;
  h.uniforms.color = glm::vec4(0.f, 0.f, 1.f, 1.f);
  h.uploadParams();

  ModelTri tri = triG1_64();
  RenderStates st = fillStates(true, 0xFFFFFFFFu, true);

  Sim sim;
  sim.init(W, H, 4);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);
  sim.beginDraw();
  ShaderModel shm;
  shm.color = glm::vec4(0.f, 0.f, 1.f, 1.f);
  shm.discardSample = 2;
  sim.drawTri(tri, st, shm, true);

  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(tri, st);
  h.end();

  // discard cancels the commit, not the invocation
  TASK_CHECK(gFragCount.load() == sim.fragCount);
  if (checkExecCounts(sim)) {
    return 1;
  }
  if (checkFramebuffer(h, sim, 1)) {
    return 1;
  }
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      TASK_CHECK(h.colorAt(x, y, 2) == RGBA(0, 0, 0, 0));
      TASK_CHECK(h.depthAt(x, y, 2) == 1.0f);
    }
  }
  return 0;
}

int case_commit_background_fill() {
  const int W = 64, H = 64;
  Harness h;
  h.init(W, H, true);

  ModelTri fg = triForeground();
  ModelTri bg = triBackground();
  RenderStates st = fillStates(true, 0xFFFFFFFFu, true);

  Sim sim;
  sim.init(W, H, 4);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);

  // draw 1: foreground (near), commits only samples 0 and 2
  ShaderModel shmFg;
  shmFg.color = glm::vec4(1.f, 0.f, 0.f, 1.f);
  shmFg.writeMask = true;
  shmFg.outMask = 0b0101u;
  sim.beginDraw();
  sim.drawTri(fg, st, shmFg, true);
  long count1 = sim.fragCount;

  // draw 2: background (far), fills the holes left by the foreground
  ShaderModel shmBg;
  shmBg.color = glm::vec4(0.f, 1.f, 0.f, 1.f);
  sim.beginDraw();
  sim.drawTri(bg, st, shmBg, true);
  long count2 = sim.fragCount;

  h.uniforms.color = glm::vec4(1.f, 0.f, 0.f, 1.f);
  h.uniforms.flags = kFlagWriteMask;
  h.uniforms.outMask = 0b0101u;
  h.uploadParams();
  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(fg, st);
  h.uniforms.color = glm::vec4(0.f, 1.f, 0.f, 1.f);
  h.uniforms.flags = 0;
  h.uploadParams();
  h.draw(bg, st);
  h.end();

  TASK_CHECK(gFragCount.load() == count1 + count2);
  if (checkFramebuffer(h, sim, 1)) {
    return 1;
  }
  // foreground holes (samples 1,3) must show the background, i.e. the
  // skipped foreground samples never occupied depth
  RGBA red = toRGBA(glm::vec4(1.f, 0.f, 0.f, 1.f));
  RGBA green = toRGBA(glm::vec4(0.f, 1.f, 0.f, 1.f));
  int holes = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      const auto &c = sim.color[(size_t) y * W + x];
      if (c[0] == red && c[2] == red && c[1] == green && c[3] == green) {
        holes++;
        TASK_CHECK(h.colorAt(x, y, 0) == red);
        TASK_CHECK(h.colorAt(x, y, 2) == red);
        TASK_CHECK(h.colorAt(x, y, 1) == green);
        TASK_CHECK(h.colorAt(x, y, 3) == green);
        TASK_CHECK(std::fabs(h.depthAt(x, y, 0) - 0.4f) < 1e-3f);
        TASK_CHECK(std::fabs(h.depthAt(x, y, 2) - 0.4f) < 1e-3f);
        TASK_CHECK(std::fabs(h.depthAt(x, y, 1) - 0.6f) < 1e-3f);
        TASK_CHECK(std::fabs(h.depthAt(x, y, 3) - 0.6f) < 1e-3f);
      }
    }
  }
  TASK_CHECK(holes > 0);
  return 0;
}

int case_commit_earlyz() {
  const int W = 64, H = 64;
  Harness h;
  h.init(W, H, true);

  ModelTri occ = triOccluder();
  ModelTri test = triTestFar();
  RenderStates st = fillStates(true, 0xFFFFFFFFu, true);

  ShaderModel shmOcc;
  shmOcc.color = glm::vec4(0.f, 0.f, 1.f, 1.f);
  ShaderModel shmTest;
  shmTest.color = glm::vec4(1.f, 0.f, 0.f, 1.f);

  Sim simEz;
  simEz.init(W, H, 4);
  simEz.clear(RGBA(0, 0, 0, 0), 1.0);
  simEz.beginDraw();
  simEz.drawTri(occ, st, shmOcc, true);
  simEz.beginDraw();
  simEz.drawTri(test, st, shmTest, true);
  long simEzCount = simEz.fragCount;

  Sim simNo;
  simNo.init(W, H, 4);
  simNo.clear(RGBA(0, 0, 0, 0), 1.0);
  simNo.beginDraw();
  simNo.drawTri(occ, st, shmOcc, false);
  simNo.beginDraw();
  simNo.drawTri(test, st, shmTest, false);
  long simNoCount = simNo.fragCount;

  // early-z may only reduce invocations, final buffers must be identical
  TASK_CHECK(simEzCount <= simNoCount);
  for (size_t i = 0; i < simEz.color.size(); i++) {
    for (int s = 0; s < 4; s++) {
      TASK_CHECK(simEz.color[i][s] == simNo.color[i][s]);
      TASK_CHECK(simEz.depth[i][s] == simNo.depth[i][s]);
    }
  }

  // run A: early z on
  h.renderer.setEnableEarlyZ(true);
  h.uniforms.flags = 0;
  h.uniforms.color = glm::vec4(0.f, 0.f, 1.f, 1.f);
  h.uploadParams();
  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(occ, st);
  resetRecords();   // count test-triangle invocations only
  h.uniforms.color = glm::vec4(1.f, 0.f, 0.f, 1.f);
  h.uploadParams();
  h.draw(test, st);
  h.end();
  long countA = gFragCount.load();
  TASK_CHECK(countA == simEzCount);
  if (checkExecCounts(simEz)) {
    return 1;
  }
  // snapshot run A buffers
  std::vector<std::array<RGBA, 4>> snapC((size_t) W * H);
  std::vector<std::array<float, 4>> snapD((size_t) W * H);
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      for (int s = 0; s < 4; s++) {
        snapC[(size_t) y * W + x][s] = h.colorAt(x, y, s);
        snapD[(size_t) y * W + x][s] = h.depthAt(x, y, s);
      }
    }
  }

  // run B: early z off
  h.renderer.setEnableEarlyZ(false);
  h.uniforms.color = glm::vec4(0.f, 0.f, 1.f, 1.f);
  h.uploadParams();
  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(occ, st);
  resetRecords();
  h.uniforms.color = glm::vec4(1.f, 0.f, 0.f, 1.f);
  h.uploadParams();
  h.draw(test, st);
  h.end();
  long countB = gFragCount.load();
  TASK_CHECK(countB == simNoCount);
  if (checkExecCounts(simNo)) {
    return 1;
  }
  // identical color and depth under both early-z settings
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      for (int s = 0; s < 4; s++) {
        TASK_CHECK(h.colorAt(x, y, s) == snapC[(size_t) y * W + x][s]);
        TASK_CHECK(h.depthAt(x, y, s) == snapD[(size_t) y * W + x][s]);
      }
    }
  }
  if (checkFramebuffer(h, simNo, 1)) {
    return 1;
  }
  h.renderer.setEnableEarlyZ(true);
  return 0;
}

int case_commit_perpixel_mask() {
  const int W = 64, H = 64;
  Harness h;
  h.init(W, H, true);
  h.uniforms.flags = kFlagWriteMask;
  h.uniforms.outMask = 0b0101u;
  h.uniforms.color = glm::vec4(1.f, 0.f, 1.f, 1.f);
  h.uploadParams();

  ModelTri tri = triG1_64();
  RenderStates st = fillStates(false, 0b0011u, true);   // per-pixel mode

  Sim sim;
  sim.init(W, H, 4);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);
  sim.beginDraw();
  ShaderModel shm;
  shm.color = glm::vec4(1.f, 0.f, 1.f, 1.f);
  shm.writeMask = true;
  shm.outMask = 0b0101u;
  sim.drawTri(tri, st, shm, true);

  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(tri, st);
  h.end();

  // one invocation per covered pixel; commits = coverage & state & output
  TASK_CHECK(gFragCount.load() == sim.fragCount);
  if (checkExecCounts(sim)) {
    return 1;
  }
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if (sim.execCnt[(size_t) y * W + x][0] > 0) {
        TASK_CHECK(gRecs[y][x][0].sampleID == -1);
      }
    }
  }
  if (checkFramebuffer(h, sim, 1)) {
    return 1;
  }
  // only sample 0 (0b0011 & 0b0101) may be written
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      for (int s = 1; s < 4; s++) {
        TASK_CHECK(h.colorAt(x, y, s) == RGBA(0, 0, 0, 0));
        TASK_CHECK(h.depthAt(x, y, s) == 1.0f);
      }
    }
  }
  return 0;
}

int case_commit_clipped() {
  const int W = 48, H = 48;
  Harness h;
  h.init(W, H, true);
  h.uniforms.flags = 0;
  h.uniforms.color = glm::vec4(0.25f, 0.5f, 0.75f, 1.f);
  h.uploadParams();

  ModelTri tri = triClip();
  RenderStates st = fillStates(true, 0xFFFFFFFFu, true);

  Sim sim;
  sim.init(W, H, 4);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);
  sim.beginDraw();
  ShaderModel shm;
  shm.color = glm::vec4(0.25f, 0.5f, 0.75f, 1.f);
  sim.drawTri(tri, st, shm, true);

  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(tri, st);
  h.end();

  TASK_CHECK(gFragCount.load() == sim.fragCount);
  if (checkFramebuffer(h, sim, 1)) {
    return 1;
  }
  // the clipped fan covers the whole viewport on every sample
  RGBA expectC = toRGBA(glm::vec4(0.25f, 0.5f, 0.75f, 1.f));
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      for (int s = 0; s < 4; s++) {
        TASK_CHECK(h.colorAt(x, y, s) == expectC);
        TASK_CHECK(std::fabs(h.depthAt(x, y, s) - 0.5f) < 1e-4f);
      }
    }
  }
  // perspective interpolation is invariant under clipping: recorded varyings
  // still follow the original triangle
  for (int y = 0; y < H; y += 5) {
    for (int x = 0; x < W; x += 5) {
      for (int s = 0; s < 4; s++) {
        if (!sim.execCnt[(size_t) y * W + x][s]) {
          continue;
        }
        const FragRec &r = gRecs[y][x][s];
        const LaneEval &ev = sim.evals[(size_t) y * sim.ES + x][s];
        TASK_CHECK(std::fabs(r.uv.x - (float) ev.uv.x) < 1e-3f);
        TASK_CHECK(std::fabs(r.uv.y - (float) ev.uv.y) < 1e-3f);
        TASK_CHECK(std::fabs(r.aux - (float) ev.aux) < 1e-3f);
        TASK_CHECK(std::fabs(r.fragCoord.z - (float) ev.z) < 1e-3f);
        TASK_CHECK(ev.edge > 1e-4);
      }
    }
  }
  return 0;
}

int case_commit_zero_mask() {
  const int W = 64, H = 64;
  Harness h;
  h.init(W, H, true);
  h.uniforms.flags = 0;
  h.uniforms.color = glm::vec4(1.f, 0.f, 0.f, 1.f);
  h.uploadParams();

  ModelTri tri = triG1_64();

  // per-sample mode: state mask 0 -> no shader execution at all
  RenderStates stPs = fillStates(true, 0u, true);
  resetRecords();
  h.begin(glm::vec4(0.f), 1.f);
  h.draw(tri, stPs);
  TASK_CHECK(gFragCount.load() == 0);

  // per-pixel mode: shader still runs once per covered pixel, commits nothing
  RenderStates stPp = fillStates(false, 0u, true);
  Sim sim;
  sim.init(W, H, 4);
  sim.clear(RGBA(0, 0, 0, 0), 1.0);
  sim.beginDraw();
  ShaderModel shm;
  shm.color = glm::vec4(1.f, 0.f, 0.f, 1.f);
  sim.drawTri(tri, stPp, shm, true);
  resetRecords();
  h.draw(tri, stPp);
  h.end();
  TASK_CHECK(gFragCount.load() == sim.fragCount);
  TASK_CHECK(sim.fragCount > 0);

  // everything stays at the clear values
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      for (int s = 0; s < 4; s++) {
        TASK_CHECK(h.colorAt(x, y, s) == RGBA(0, 0, 0, 0));
        TASK_CHECK(h.depthAt(x, y, s) == 1.0f);
      }
    }
  }
  return 0;
}

}   // namespace

const task_case task_cases[] = {
    {"interpolation", "persample_varyings_fragcoord", case_interp_persample},
    {"interpolation", "persample_single_target", case_interp_single_target},
    {"interpolation", "perpixel_representative", case_interp_perpixel},
    {"derivatives", "quad_same_lane_diffs", case_deriv_lane_diffs},
    {"derivatives", "implicit_lod_mip", case_deriv_lod_uniform},
    {"derivatives", "implicit_lod_perspective", case_deriv_lod_perspective},
    {"derivatives", "depth_failed_helper_diffs", case_deriv_depth_failed_helpers},
    {"derivatives", "dfctx_across_draws", case_deriv_across_draws},
    {"commit", "state_mask_persample", case_commit_state_mask},
    {"commit", "output_mask_persample", case_commit_output_mask},
    {"commit", "discard_by_sampleid", case_commit_discard_sampleid},
    {"commit", "background_fills_holes", case_commit_background_fill},
    {"commit", "earlyz_equivalence", case_commit_earlyz},
    {"commit", "perpixel_state_and_output_mask", case_commit_perpixel_mask},
    {"commit", "clipped_triangle_full_coverage", case_commit_clipped},
    {"commit", "zero_state_mask", case_commit_zero_mask},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
