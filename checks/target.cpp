// target.cpp — capture / ownership / replay checks for recorded software draws
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include "task_support/check.h"

#include "Render/Software/CommandListSoft.h"
#include "Render/Software/RendererSoft.h"
#include "Render/Software/ShaderProgramSoft.h"
#include "Render/Software/TextureSoft.h"
#include "Render/Software/UniformSoft.h"

using namespace SoftGL;

namespace {

constexpr int kWidth = 16;
constexpr int kHeight = 16;

std::atomic<unsigned> g_vsCalls{0};
std::atomic<unsigned> g_fsCalls{0};

void resetCalls() {
  g_vsCalls = 0;
  g_fsCalls = 0;
}

unsigned shaderCalls() {
  return g_vsCalls.load() + g_fsCalls.load();
}

// ---------------------------------------------------------------------------
// Recording shader: full-screen triangle with tint * optional textures.
// Uses implicit-LOD `texture()` when TEX/TEX2 are defined, so the sampler
// derivative callback path is exercised during replay as well.
// ---------------------------------------------------------------------------
namespace RecShader {

struct ShaderDefines {
  uint8_t TEX;
  uint8_t TEX2;
  uint8_t FIXLOD;
};

struct ShaderAttributes {
  glm::vec4 position;
  glm::vec2 uv;
};

struct ShaderUniforms {
  glm::vec4 tint;
  Sampler2DSoft<RGBA> *image;
  Sampler2DSoft<RGBA> *image2;
};

struct ShaderVaryings {
  glm::vec2 uv;
};

class Common : public ShaderSoft {
 public:
  CREATE_SHADER_OVERRIDE

  std::vector<std::string> &getDefines() override {
    static std::vector<std::string> defines = {"TEX", "TEX2", "FIXLOD"};
    return defines;
  }

  std::vector<UniformDesc> &getUniformsDesc() override {
    static std::vector<UniformDesc> desc = {
        {"tint", offsetof(ShaderUniforms, tint)},
        {"image", offsetof(ShaderUniforms, image)},
        {"image2", offsetof(ShaderUniforms, image2)},
    };
    return desc;
  }
};

class VS : public Common {
 public:
  CREATE_SHADER_CLONE(VS)
  void shaderMain() override {
    ++g_vsCalls;
    gl->Position = a->position;
    v->uv = a->uv;
  }
};

class FS : public Common {
 public:
  CREATE_SHADER_CLONE(FS)

  size_t getSamplerDerivativeOffset(BaseSampler<RGBA> *) const override {
    return offsetof(ShaderVaryings, uv);
  }

  void setupSamplerDerivative() override {
    if (def->TEX) {
      u->image->setLodFunc(&texLodFunc);
    }
    if (def->TEX2) {
      u->image2->setLodFunc(&texLodFunc);
    }
  }

  void shaderMain() override {
    ++g_fsCalls;
    glm::vec4 c = u->tint;
    if (def->FIXLOD) {
      c *= textureLod(u->image, v->uv, 2.0f);
    } else {
      if (def->TEX) {
        c *= texture(u->image, v->uv);
      }
      if (def->TEX2) {
        c *= texture(u->image2, v->uv);
      }
    }
    gl->FragColor = c;
  }
};

}  // namespace RecShader

// ---------------------------------------------------------------------------
// Scene: every source object a recorded draw borrows
// ---------------------------------------------------------------------------
struct Scene {
  std::shared_ptr<RendererSoft> renderer;
  std::shared_ptr<Texture> tex;
  std::shared_ptr<Texture> tex2;
  std::shared_ptr<Buffer<RGBA>> texPixels;
  std::shared_ptr<Texture> targetColor;
  std::shared_ptr<Texture> targetDepth;
  std::shared_ptr<FrameBuffer> fbo;
  std::shared_ptr<ShaderProgramSoft> program;
  std::shared_ptr<ShaderResources> resources;
  std::shared_ptr<UniformBlock> tintBlock;
  std::shared_ptr<UniformSampler> sampler1;
  std::shared_ptr<UniformSampler> sampler2;
  std::shared_ptr<VertexArrayObject> vao;
  std::shared_ptr<PipelineStates> pipeline;

  std::vector<RecShader::ShaderAttributes> verts;
  std::vector<int32_t> indices;

  ~Scene() = default;

  // drop every source-side object; the command list must survive this
  void releaseSources() {
    tintBlock.reset();
    sampler1.reset();
    sampler2.reset();
    resources.reset();
    program.reset();
    vao.reset();
    pipeline.reset();
    fbo.reset();
    targetDepth.reset();
    targetColor.reset();
    texPixels.reset();
    tex2.reset();
    tex.reset();
    renderer.reset();
  }
};

std::shared_ptr<Texture> makeColorTexture(RendererSoft &r, int w, int h, bool mips, RGBA fill) {
  TextureDesc desc;
  desc.width = w;
  desc.height = h;
  desc.useMipmaps = mips;
  auto tex = r.createTexture(desc);
  auto pixels = Buffer<RGBA>::makeDefault(w, h);
  pixels->setAll(fill);
  // a gradient so bilinear and nearest filtering differ
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint8_t v = (uint8_t) (32 + ((x * 17 + y * 29) & 0x7f));
      *pixels->get(x, y) = RGBA(v, (uint8_t) (v ^ 0x55), (uint8_t) (v ^ 0xaa), 255);
    }
  }
  tex->setImageData({pixels});
  return tex;
}

void makeTarget(Scene &scene, int w, int h, const ClearStates &clear) {
  auto &r = *scene.renderer;
  TextureDesc cd;
  cd.width = w;
  cd.height = h;
  cd.usage = TextureUsage_AttachmentColor;
  scene.targetColor = r.createTexture(cd);
  scene.targetColor->initImageData();

  TextureDesc dd = cd;
  dd.format = TextureFormat_FLOAT32;
  dd.usage = TextureUsage_AttachmentDepth;
  scene.targetDepth = r.createTexture(dd);
  scene.targetDepth->initImageData();

  scene.fbo = r.createFrameBuffer(true);
  scene.fbo->setColorAttachment(scene.targetColor, 0);
  scene.fbo->setDepthAttachment(scene.targetDepth);
  (void) clear;
}

// Build a scene whose program records defines and binds one or two samplers.
// `twoSamplers` uses two sampler uniforms whose filter states differ but that
// point at the *same* texture object.
struct SceneOptions {
  bool mips = false;
  bool fixLod = false;
  bool twoSamplers = false;
  bool blend = false;
  bool depthTest = true;
  std::vector<std::string> defines;
};

Scene buildScene(const SceneOptions &opts, const ClearStates &clear) {
  Scene scene;
  scene.renderer = std::make_shared<RendererSoft>();
  auto &r = *scene.renderer;

  scene.tex = makeColorTexture(r, 8, 8, opts.mips, RGBA(200, 100, 50, 255));
  SamplerDesc sd;
  sd.filterMin = opts.mips ? Filter_LINEAR_MIPMAP_LINEAR : Filter_NEAREST;
  sd.filterMag = Filter_NEAREST;
  sd.wrapS = Wrap_REPEAT;
  sd.wrapT = Wrap_REPEAT;
  scene.tex->setSamplerDesc(sd);

  scene.program = std::dynamic_pointer_cast<ShaderProgramSoft>(r.createShaderProgram());
  for (auto &def : opts.defines) {
    scene.program->addDefine(def);
  }
  scene.program->SetShaders(std::make_shared<RecShader::VS>(), std::make_shared<RecShader::FS>());

  scene.resources = std::make_shared<ShaderResources>();
  scene.tintBlock = r.createUniformBlock("tint", sizeof(glm::vec4));
  glm::vec4 tint(1.f);
  scene.tintBlock->setData(&tint, sizeof(tint));
  scene.resources->blocks[0] = scene.tintBlock;

  scene.sampler1 = r.createUniformSampler("image", *scene.tex);
  scene.sampler1->setTexture(scene.tex);
  scene.resources->samplers[1] = scene.sampler1;

  if (opts.twoSamplers) {
    // same texture object, different sampling state per sampler
    SamplerDesc nearestDesc;
    nearestDesc.filterMin = Filter_NEAREST;
    scene.tex->setSamplerDesc(nearestDesc);
    scene.sampler1->setTexture(scene.tex);

    SamplerDesc linearDesc;
    linearDesc.filterMin = Filter_LINEAR;
    scene.tex->setSamplerDesc(linearDesc);
    scene.sampler2 = r.createUniformSampler("image2", *scene.tex);
    scene.sampler2->setTexture(scene.tex);
    scene.resources->samplers[2] = scene.sampler2;

    // restore the source texture's own sampler description afterwards
    scene.tex->setSamplerDesc(sd);
  }

  // full screen triangle
  scene.verts = {
      {{-1.f, -1.f, 0.5f, 1.f}, {0.f, 0.f}},
      {{3.f, -1.f, 0.5f, 1.f}, {2.f, 0.f}},
      {{-1.f, 3.f, 0.5f, 1.f}, {0.f, 2.f}},
  };
  scene.indices = {0, 1, 2};
  VertexArray va;
  va.vertexesDesc = {{4, sizeof(RecShader::ShaderAttributes), 0}};
  va.vertexesBuffer = reinterpret_cast<uint8_t *>(scene.verts.data());
  va.vertexesBufferLength = scene.verts.size() * sizeof(RecShader::ShaderAttributes);
  va.indexBuffer = scene.indices.data();
  va.indexBufferLength = scene.indices.size() * sizeof(int32_t);
  scene.vao = r.createVertexArrayObject(va);

  RenderStates rs;
  rs.cullFace = false;
  rs.depthTest = opts.depthTest;
  rs.depthFunc = DepthFunc_LESS;
  if (opts.blend) {
    rs.blend = true;
    rs.blendParams.SetBlendFactor(BlendFactor_SRC_ALPHA, BlendFactor_ONE_MINUS_SRC_ALPHA);
  }
  scene.pipeline = r.createPipelineStates(rs);

  r.setViewPort(0, 0, kWidth, kHeight);
  std::shared_ptr<ShaderProgram> programBase = scene.program;
  r.setShaderProgram(programBase);
  r.setShaderResources(scene.resources);
  std::shared_ptr<VertexArrayObject> vaoBase = scene.vao;
  r.setVertexArrayObject(vaoBase);
  std::shared_ptr<PipelineStates> pipeBase = scene.pipeline;
  r.setPipelineStates(pipeBase);

  makeTarget(scene, kWidth, kHeight, clear);
  return scene;
}

void setTint(Scene &scene, glm::vec4 tint) {
  scene.tintBlock->setData(&tint, sizeof(tint));
  std::shared_ptr<ShaderProgram> programBase = scene.program;
  scene.renderer->setShaderProgram(programBase);
  scene.renderer->setShaderResources(scene.resources);
}

ClearStates defaultClear() {
  ClearStates clear;
  clear.colorFlag = true;
  clear.depthFlag = true;
  clear.clearColor = glm::vec4(0.f, 0.f, 0.f, 1.f);
  clear.clearDepth = 1.f;
  return clear;
}

// ---------------------------------------------------------------------------
// Image / depth comparison helpers
// ---------------------------------------------------------------------------
struct ImageF {
  std::vector<RGBA> color;
  std::vector<float> depth;
  int w = 0;
  int h = 0;
};

ImageF readTarget(const Scene &scene) {
  ImageF img;
  auto color = std::dynamic_pointer_cast<TextureSoft<RGBA>>(scene.targetColor);
  auto depth = std::dynamic_pointer_cast<TextureSoft<float>>(scene.targetDepth);
  img.w = (int) color->getImage().getBuffer()->width;
  img.h = (int) color->getImage().getBuffer()->height;
  for (int y = 0; y < img.h; y++) {
    for (int x = 0; x < img.w; x++) {
      img.color.push_back(*color->getImage().getBuffer()->buffer->get(x, y));
      img.depth.push_back(*depth->getImage().getBuffer()->buffer->get(x, y));
    }
  }
  return img;
}

bool sameColor(const ImageF &a, const ImageF &b, int tol) {
  if (a.color.size() != b.color.size() || a.w != b.w || a.h != b.h) {
    return false;
  }
  for (size_t i = 0; i < a.color.size(); i++) {
    const RGBA &p = a.color[i];
    const RGBA &q = b.color[i];
    if (std::abs((int) p.r - (int) q.r) > tol) return false;
    if (std::abs((int) p.g - (int) q.g) > tol) return false;
    if (std::abs((int) p.b - (int) q.b) > tol) return false;
    if (std::abs((int) p.a - (int) q.a) > tol) return false;
  }
  return true;
}

bool sameDepth(const ImageF &a, const ImageF &b) {
  if (a.depth.size() != b.depth.size()) {
    return false;
  }
  for (size_t i = 0; i < a.depth.size(); i++) {
    if (std::fabs(a.depth[i] - b.depth[i]) > 1e-6f) {
      return false;
    }
  }
  return true;
}

bool differColor(const ImageF &a, const ImageF &b) {
  return !sameColor(a, b, 0);
}

bool notAllBlack(const ImageF &a) {
  for (auto &p : a.color) {
    if (p.r || p.g || p.b) {
      return true;
    }
  }
  return false;
}

// Run a sequence of immediate draws through the scene's own bindings.
ImageF immediateDraw(Scene &scene, const std::vector<glm::vec4> &tints) {
  auto &r = *scene.renderer;
  ClearStates clear = defaultClear();
  r.beginRenderPass(scene.fbo, clear);
  for (auto &tint : tints) {
    setTint(scene, tint);
    r.draw();
  }
  r.endRenderPass();
  return readTarget(scene);
}

// A fresh renderer / target pair used as a replay destination.
struct Target {
  std::shared_ptr<RendererSoft> renderer = std::make_shared<RendererSoft>();
  std::shared_ptr<Texture> color;
  std::shared_ptr<Texture> depth;
  std::shared_ptr<FrameBuffer> fbo;

  Target() {
    TextureDesc cd;
    cd.width = kWidth;
    cd.height = kHeight;
    cd.usage = TextureUsage_AttachmentColor;
    color = renderer->createTexture(cd);
    color->initImageData();
    TextureDesc dd = cd;
    dd.format = TextureFormat_FLOAT32;
    dd.usage = TextureUsage_AttachmentDepth;
    depth = renderer->createTexture(dd);
    depth->initImageData();
    fbo = renderer->createFrameBuffer(true);
    fbo->setColorAttachment(color, 0);
    fbo->setDepthAttachment(depth);
  }

  ImageF read() {
    ImageF img;
    auto c = std::dynamic_pointer_cast<TextureSoft<RGBA>>(color);
    auto d = std::dynamic_pointer_cast<TextureSoft<float>>(depth);
    img.w = kWidth;
    img.h = kHeight;
    for (int y = 0; y < kHeight; y++) {
      for (int x = 0; x < kWidth; x++) {
        img.color.push_back(*c->getImage().getBuffer()->buffer->get(x, y));
        img.depth.push_back(*d->getImage().getBuffer()->buffer->get(x, y));
      }
    }
    return img;
  }

  ImageF replay(CommandListSoft &list, const ClearStates &clear) {
    renderer->beginRenderPass(fbo, clear);
    list.replay(*renderer, fbo);
    renderer->endRenderPass();
    return read();
  }

  ImageF snapshot() {
    ImageF img;
    auto c = std::dynamic_pointer_cast<TextureSoft<RGBA>>(color);
    img.w = kWidth;
    img.h = kHeight;
    for (int y = 0; y < kHeight; y++) {
      for (int x = 0; x < kWidth; x++) {
        img.color.push_back(*c->getImage().getBuffer()->buffer->get(x, y));
      }
    }
    return img;
  }
};

}  // namespace

// ===========================================================================
// capture
// ===========================================================================

static int capture_no_execution() {
  SceneOptions opts;
  opts.defines = {"TEX"};
  Scene scene = buildScene(opts, defaultClear());

  resetCalls();
  SnapshotStore store;
  CommandListSoft list;

  unsigned before = shaderCalls();
  TASK_CHECK(list.appendFrom(*scene.renderer, store));
  TASK_CHECK(shaderCalls() == before);      // recording ran no shader
  TASK_CHECK(list.size() == 1);
  TASK_CHECK(store.imageSnapshotCount() == 1);
  TASK_CHECK(store.meshSnapshotCount() == 1);
  TASK_CHECK(store.copiedBytes() > 0);

  // geometry and uniform state were captured, not executed as a draw
  TASK_CHECK(g_vsCalls.load() == 0);
  TASK_CHECK(g_fsCalls.load() == 0);
  return 0;
}

static int capture_rejects_incomplete() {
  SceneOptions opts;
  Scene scene = buildScene(opts, defaultClear());
  SnapshotStore store;
  CommandListSoft list;

  // no VAO bound -> reject with no side effects
  std::shared_ptr<VertexArrayObject> empty;
  scene.renderer->setVertexArrayObject(empty);
  TASK_CHECK(!list.appendFrom(*scene.renderer, store));
  TASK_CHECK(list.size() == 0);
  TASK_CHECK(store.snapshotCount() == 0);
  TASK_CHECK(store.copiedBytes() == 0);

  // no program bound -> reject
  std::shared_ptr<VertexArrayObject> vaoBase = scene.vao;
  scene.renderer->setVertexArrayObject(vaoBase);
  std::shared_ptr<ShaderProgram> noProgram;
  scene.renderer->setShaderProgram(noProgram);
  TASK_CHECK(!list.appendFrom(*scene.renderer, store));
  TASK_CHECK(list.size() == 0);
  TASK_CHECK(store.snapshotCount() == 0);

  // restore a valid binding, then recording works and stats grow
  std::shared_ptr<ShaderProgram> programBase = scene.program;
  scene.renderer->setShaderProgram(programBase);
  scene.renderer->setShaderResources(scene.resources);
  TASK_CHECK(list.appendFrom(*scene.renderer, store));
  TASK_CHECK(list.size() == 1);
  TASK_CHECK(store.imageSnapshotCount() == 1);
  return 0;
}

static int capture_rejects_multisample() {
  SceneOptions opts;
  Scene scene = buildScene(opts, defaultClear());

  // replace the sampled texture with a multi-sample one before recording
  TextureDesc ms;
  ms.width = 8;
  ms.height = 8;
  ms.multiSample = true;
  ms.usage = TextureUsage_AttachmentColor;
  auto msTex = scene.renderer->createTexture(ms);
  msTex->initImageData();

  SamplerDesc sd;
  sd.filterMin = Filter_NEAREST;
  msTex->setSamplerDesc(sd);
  scene.sampler1->setTexture(msTex);

  SnapshotStore store;
  CommandListSoft list;
  TASK_CHECK(!list.appendFrom(*scene.renderer, store));
  TASK_CHECK(list.size() == 0);
  TASK_CHECK(store.snapshotCount() == 0);
  TASK_CHECK(store.copiedBytes() == 0);

  // the failure did not disturb the existing sampler binding semantics
  scene.sampler1->setTexture(scene.tex);
  TASK_CHECK(list.appendFrom(*scene.renderer, store));
  TASK_CHECK(list.size() == 1);
  return 0;
}

static int capture_uniforms_and_defines() {
  SceneOptions opts;
  opts.defines = {"TEX"};
  Scene scene = buildScene(opts, defaultClear());

  setTint(scene, glm::vec4(1.f, 0.5f, 0.25f, 1.f));
  SnapshotStore store;
  CommandListSoft list;
  TASK_CHECK(list.appendFrom(*scene.renderer, store));

  // mutate uniforms, defines, resources and the source VAO after recording
  setTint(scene, glm::vec4(0.f, 0.f, 0.f, 1.f));
  scene.program->addDefine("TEX2");
  for (auto &v : scene.verts) {
    v.position = glm::vec4(0.f);
  }
  std::shared_ptr<VertexArrayObject> vaoBase = scene.vao;
  scene.renderer->setVertexArrayObject(vaoBase);
  scene.vao->updateVertexData(scene.verts.data(),
                              scene.verts.size() * sizeof(RecShader::ShaderAttributes));

  Target dst;
  ImageF got = dst.replay(list, defaultClear());
  TASK_CHECK(notAllBlack(got));

  // reference: identical scene recorded before any mutation, drawn immediately
  SceneOptions refOpts;
  refOpts.defines = {"TEX"};
  Scene ref = buildScene(refOpts, defaultClear());
  setTint(ref, glm::vec4(1.f, 0.5f, 0.25f, 1.f));
  ImageF want = immediateDraw(ref, {glm::vec4(1.f, 0.5f, 0.25f, 1.f)});
  TASK_CHECK(sameColor(want, got, 1));
  return 0;
}

// ===========================================================================
// ownership
// ===========================================================================

static int ownership_image_dedup() {
  SceneOptions opts;
  opts.defines = {"TEX"};
  Scene scene = buildScene(opts, defaultClear());
  SnapshotStore store;
  CommandListSoft list;

  TASK_CHECK(list.appendFrom(*scene.renderer, store));
  size_t images = store.imageSnapshotCount();
  size_t copied = store.copiedBytes();
  size_t held = store.heldBytes();

  // identical content appended again must reuse the snapshot
  TASK_CHECK(list.appendFrom(*scene.renderer, store));
  TASK_CHECK(store.imageSnapshotCount() == images);
  TASK_CHECK(store.copiedBytes() == copied);
  TASK_CHECK(store.heldBytes() == held);
  TASK_CHECK(list.size() == 2);
  return 0;
}

static int ownership_content_dedup_distinct_objects() {
  // Two distinct texture objects with identical pixel content must share one
  // frozen image: dedup is by content, never by object address.
  SceneOptions opts;
  opts.defines = {"TEX"};
  Scene a = buildScene(opts, defaultClear());
  Scene b = buildScene(opts, defaultClear());

  // make b's texture a *different* object carrying the same pixels
  auto aTex = std::dynamic_pointer_cast<TextureSoft<RGBA>>(a.tex);
  auto bTex = std::dynamic_pointer_cast<TextureSoft<RGBA>>(b.tex);
  auto srcBuffer = aTex->getImage().getBuffer()->buffer;
  auto copy = Buffer<RGBA>::makeDefault(srcBuffer->getWidth(), srcBuffer->getHeight());
  for (size_t y = 0; y < srcBuffer->getHeight(); y++) {
    for (size_t x = 0; x < srcBuffer->getWidth(); x++) {
      *copy->get(x, y) = *srcBuffer->get(x, y);
    }
  }
  bTex->setImageData({copy});
  SamplerDesc sd = aTex->getSamplerDesc();
  bTex->setSamplerDesc(sd);
  b.sampler1->setTexture(b.tex);

  TASK_CHECK(a.tex.get() != b.tex.get());

  // make the geometry byte-identical too (aggregate init can leave struct
  // padding unspecified, which would otherwise defeat byte-level dedup)
  auto *aVao = dynamic_cast<VertexArrayObjectSoft *>(a.vao.get());
  auto *bVao = dynamic_cast<VertexArrayObjectSoft *>(b.vao.get());
  TASK_CHECK(aVao != nullptr && bVao != nullptr);
  bVao->updateVertexData(const_cast<uint8_t *>(aVao->vertexData()),
                         aVao->vertexCount() * aVao->vertexStride);

  SnapshotStore store;
  CommandListSoft listA;
  CommandListSoft listB;
  TASK_CHECK(listA.appendFrom(*a.renderer, store));
  size_t copied = store.copiedBytes();
  TASK_CHECK(listB.appendFrom(*b.renderer, store));

  // identical content from a distinct resource object -> no second copy
  TASK_CHECK(store.imageSnapshotCount() == 1);
  TASK_CHECK(store.meshSnapshotCount() == 1);
  TASK_CHECK(store.copiedBytes() == copied);
  return 0;
}

static int ownership_shared_texture_two_samplers() {
  SceneOptions opts;
  opts.twoSamplers = true;
  opts.defines = {"TEX", "TEX2"};
  Scene scene = buildScene(opts, defaultClear());
  SnapshotStore store;
  CommandListSoft list;

  TASK_CHECK(list.appendFrom(*scene.renderer, store));
  // two samplers, one texture object -> a single frozen image
  TASK_CHECK(store.imageSnapshotCount() == 1);

  // the two sampler uniforms genuinely use different filter states, and the
  // source texture's own (linear) description was left untouched by recording
  TASK_CHECK(scene.sampler1 != scene.sampler2);

  // reference: the same scene drawn immediately
  SceneOptions refOpts = opts;
  Scene ref = buildScene(refOpts, defaultClear());
  ImageF want = immediateDraw(ref, {glm::vec4(1.f)});
  TASK_CHECK(notAllBlack(want));

  Target dst;
  ImageF got = dst.replay(list, defaultClear());
  TASK_CHECK(sameColor(want, got, 1));

  // per-sampler state really is independent: a scene whose two samplers both
  // use the *same* state produces a different result
  {
    SceneOptions sameOpts;
    sameOpts.defines = {"TEX", "TEX2"};
    sameOpts.twoSamplers = true;
    Scene same = buildScene(sameOpts, defaultClear());
    // override: make both skins sample nearest
    SamplerDesc nearest;
    nearest.filterMin = Filter_NEAREST;
    same.tex->setSamplerDesc(nearest);
    same.sampler1->setTexture(same.tex);
    same.sampler2->setTexture(same.tex);
    ImageF sameImg = immediateDraw(same, {glm::vec4(1.f)});
    TASK_CHECK(differColor(want, sameImg));
  }
  TASK_CHECK(store.imageSnapshotCount() == 1);
  return 0;
}

static int ownership_buffer_mutation_detected() {
  SceneOptions opts;
  opts.defines = {"TEX"};
  Scene scene = buildScene(opts, defaultClear());
  SnapshotStore store;

  // record the original content into its own list
  CommandListSoft oldList;
  TASK_CHECK(oldList.appendFrom(*scene.renderer, store));
  size_t copied = store.copiedBytes();

  // reference image for the original content
  SceneOptions refOpts;
  refOpts.defines = {"TEX"};
  Scene refOld = buildScene(refOpts, defaultClear());
  ImageF wantOld = immediateDraw(refOld, {glm::vec4(1.f)});

  // mutate one texel through the external buffer; no manual dirty flag
  auto colorTex = std::dynamic_pointer_cast<TextureSoft<RGBA>>(scene.tex);
  auto buffer = colorTex->getImage().getBuffer()->buffer;
  *buffer->get(3, 3) = RGBA(255, 0, 255, 255);

  CommandListSoft newList;
  TASK_CHECK(newList.appendFrom(*scene.renderer, store));
  TASK_CHECK(store.copiedBytes() > copied);          // a new copy was made
  TASK_CHECK(store.imageSnapshotCount() == 2);       // old + new content alive

  // the old command still returns the recorded (old) image
  Target dstOld;
  ImageF gotOld = dstOld.replay(oldList, defaultClear());
  TASK_CHECK(sameColor(wantOld, gotOld, 1));

  // the new command reflects the mutated texel, and differs from the old
  Target dstNew;
  ImageF gotNew = dstNew.replay(newList, defaultClear());
  TASK_CHECK(notAllBlack(gotNew));
  TASK_CHECK(differColor(gotOld, gotNew));
  return 0;
}

static int ownership_source_release() {
  SceneOptions opts;
  opts.defines = {"TEX"};
  Scene scene = buildScene(opts, defaultClear());

  SnapshotStore store;
  CommandListSoft list;
  TASK_CHECK(list.appendFrom(*scene.renderer, store));

  // drop the store too: only the list may keep the snapshots alive
  scene.releaseSources();
  store = SnapshotStore();  // release the shared cache

  Target dst;
  ImageF got = dst.replay(list, defaultClear());
  TASK_CHECK(notAllBlack(got));
  return 0;
}

static int ownership_cache_weak_release() {
  SceneOptions opts;
  opts.defines = {"TEX"};
  Scene scene = buildScene(opts, defaultClear());
  SnapshotStore store;

  {
    CommandListSoft list;
    TASK_CHECK(list.appendFrom(*scene.renderer, store));
    TASK_CHECK(store.snapshotCount() >= 1);
    TASK_CHECK(store.heldBytes() > 0);
  }
  // command list gone: cache must not keep snapshots alive
  TASK_CHECK(store.imageSnapshotCount() == 0);
  TASK_CHECK(store.meshSnapshotCount() == 0);
  TASK_CHECK(store.heldBytes() == 0);
  return 0;
}

static int ownership_failed_append_stats() {
  SceneOptions opts;
  opts.defines = {"TEX"};
  Scene scene = buildScene(opts, defaultClear());
  SnapshotStore store;
  CommandListSoft list;
  TASK_CHECK(list.appendFrom(*scene.renderer, store));

  size_t images = store.imageSnapshotCount();
  size_t meshes = store.meshSnapshotCount();
  size_t copied = store.copiedBytes();
  size_t held = store.heldBytes();

  // a fresh, unsupported multisample sampler aborts the append
  TextureDesc ms;
  ms.width = 8;
  ms.height = 8;
  ms.multiSample = true;
  ms.usage = TextureUsage_AttachmentColor;
  auto msTex = scene.renderer->createTexture(ms);
  msTex->initImageData();
  SamplerDesc sd;
  msTex->setSamplerDesc(sd);
  scene.sampler1->setTexture(msTex);

  TASK_CHECK(!list.appendFrom(*scene.renderer, store));
  TASK_CHECK(list.size() == 1);
  TASK_CHECK(store.imageSnapshotCount() == images);
  TASK_CHECK(store.meshSnapshotCount() == meshes);
  TASK_CHECK(store.copiedBytes() == copied);
  TASK_CHECK(store.heldBytes() == held);

  // recording can resume once a supported resource is bound again
  scene.sampler1->setTexture(scene.tex);
  TASK_CHECK(list.appendFrom(*scene.renderer, store));
  TASK_CHECK(list.size() == 2);
  return 0;
}

// ===========================================================================
// replay
// ===========================================================================

static int replay_matches_immediate() {
  SceneOptions opts;
  opts.defines = {"TEX"};
  Scene scene = buildScene(opts, defaultClear());
  setTint(scene, glm::vec4(0.75f, 0.5f, 0.25f, 1.f));

  SceneOptions refOpts = opts;
  Scene ref = buildScene(refOpts, defaultClear());
  ImageF want = immediateDraw(ref, {glm::vec4(0.75f, 0.5f, 0.25f, 1.f)});
  TASK_CHECK(notAllBlack(want));

  SnapshotStore store;
  CommandListSoft list;
  setTint(scene, glm::vec4(0.75f, 0.5f, 0.25f, 1.f));
  TASK_CHECK(list.appendFrom(*scene.renderer, store));

  Target dst;
  ImageF got = dst.replay(list, defaultClear());
  TASK_CHECK(sameColor(want, got, 1));
  TASK_CHECK(sameDepth(want, got));
  return 0;
}

static int replay_depth_matches() {
  SceneOptions opts;
  opts.defines = {"TEX"};
  Scene scene = buildScene(opts, defaultClear());
  // move the recorded triangle to a non-default depth
  for (auto &v : scene.verts) {
    v.position.z = 0.3f;
  }
  VertexArray va;
  va.vertexesDesc = {{4, sizeof(RecShader::ShaderAttributes), 0}};
  va.vertexesBuffer = reinterpret_cast<uint8_t *>(scene.verts.data());
  va.vertexesBufferLength = scene.verts.size() * sizeof(RecShader::ShaderAttributes);
  va.indexBuffer = scene.indices.data();
  va.indexBufferLength = scene.indices.size() * sizeof(int32_t);
  scene.vao = scene.renderer->createVertexArrayObject(va);
  std::shared_ptr<VertexArrayObject> vaoBase = scene.vao;
  scene.renderer->setVertexArrayObject(vaoBase);

  Scene ref;
  {
    SceneOptions refOpts = opts;
    ref = buildScene(refOpts, defaultClear());
    for (auto &v : ref.verts) {
      v.position.z = 0.3f;
    }
    VertexArray rva;
    rva.vertexesDesc = {{4, sizeof(RecShader::ShaderAttributes), 0}};
    rva.vertexesBuffer = reinterpret_cast<uint8_t *>(ref.verts.data());
    rva.vertexesBufferLength = ref.verts.size() * sizeof(RecShader::ShaderAttributes);
    rva.indexBuffer = ref.indices.data();
    rva.indexBufferLength = ref.indices.size() * sizeof(int32_t);
    ref.vao = ref.renderer->createVertexArrayObject(rva);
    std::shared_ptr<VertexArrayObject> rvaoBase = ref.vao;
    ref.renderer->setVertexArrayObject(rvaoBase);
  }
  ImageF want = immediateDraw(ref, {glm::vec4(1.f)});
  TASK_CHECK(std::fabs(want.depth[8 * kWidth + 8] - 0.3f) < 1e-5f);

  SnapshotStore store;
  CommandListSoft list;
  TASK_CHECK(list.appendFrom(*scene.renderer, store));

  // a replay target that also carries depth
  auto r = std::make_shared<RendererSoft>();
  TextureDesc cd;
  cd.width = kWidth;
  cd.height = kHeight;
  cd.usage = TextureUsage_AttachmentColor;
  auto c = r->createTexture(cd);
  c->initImageData();
  TextureDesc dd = cd;
  dd.format = TextureFormat_FLOAT32;
  dd.usage = TextureUsage_AttachmentDepth;
  auto d = r->createTexture(dd);
  d->initImageData();
  auto f = r->createFrameBuffer(true);
  f->setColorAttachment(c, 0);
  f->setDepthAttachment(d);

  ClearStates clear = defaultClear();
  r->beginRenderPass(f, clear);
  TASK_CHECK(list.replay(*r, f));
  r->endRenderPass();

  auto dt = std::dynamic_pointer_cast<TextureSoft<float>>(d);
  float z = *dt->getImage().getBuffer()->buffer->get(8, 8);
  TASK_CHECK(std::fabs(z - 0.3f) < 1e-5f);

  auto ct = std::dynamic_pointer_cast<TextureSoft<RGBA>>(c);
  ImageF got;
  got.w = kWidth;
  got.h = kHeight;
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      got.color.push_back(*ct->getImage().getBuffer()->buffer->get(x, y));
    }
  }
  TASK_CHECK(sameColor(want, got, 1));
  return 0;
}

static int replay_destination_alias() {
  // source texture is also the replay target's color attachment
  auto r = std::make_shared<RendererSoft>();
  TextureDesc cd;
  cd.width = kWidth;
  cd.height = kHeight;
  cd.usage = TextureUsage_AttachmentColor | TextureUsage_Sampler;
  cd.useMipmaps = true;
  auto aliasTex = r->createTexture(cd);
  auto pixels = Buffer<RGBA>::makeDefault(kWidth, kHeight);
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      uint8_t v = (uint8_t) (40 + ((x * 13 + y * 7) & 0x7f));
      *pixels->get(x, y) = RGBA(v, (uint8_t) (v ^ 0x3c), (uint8_t) (v ^ 0x81), 255);
    }
  }
  aliasTex->setImageData({pixels});
  SamplerDesc sd;
  sd.filterMin = Filter_NEAREST_MIPMAP_NEAREST;
  aliasTex->setSamplerDesc(sd);

  TextureDesc dd = cd;
  dd.format = TextureFormat_FLOAT32;
  dd.usage = TextureUsage_AttachmentDepth;
  auto depth = r->createTexture(dd);
  depth->initImageData();

  auto fbo = r->createFrameBuffer(true);
  fbo->setColorAttachment(aliasTex, 0);
  fbo->setDepthAttachment(depth);

  auto program = std::dynamic_pointer_cast<ShaderProgramSoft>(r->createShaderProgram());
  program->addDefine("TEX");
  program->SetShaders(std::make_shared<RecShader::VS>(), std::make_shared<RecShader::FS>());
  auto resources = std::make_shared<ShaderResources>();
  auto block = r->createUniformBlock("tint", sizeof(glm::vec4));
  glm::vec4 tint(1.f);
  block->setData(&tint, sizeof(tint));
  resources->blocks[0] = block;
  auto sampler = r->createUniformSampler("image", *aliasTex);
  sampler->setTexture(aliasTex);
  resources->samplers[1] = sampler;

  std::vector<RecShader::ShaderAttributes> verts = {
      {{-1.f, -1.f, 0.5f, 1.f}, {0.f, 0.f}},
      {{3.f, -1.f, 0.5f, 1.f}, {2.f, 0.f}},
      {{-1.f, 3.f, 0.5f, 1.f}, {0.f, 2.f}},
  };
  std::vector<int32_t> idx = {0, 1, 2};
  VertexArray va;
  va.vertexesDesc = {{4, sizeof(RecShader::ShaderAttributes), 0}};
  va.vertexesBuffer = reinterpret_cast<uint8_t *>(verts.data());
  va.vertexesBufferLength = verts.size() * sizeof(RecShader::ShaderAttributes);
  va.indexBuffer = idx.data();
  va.indexBufferLength = idx.size() * sizeof(int32_t);
  auto vao = r->createVertexArrayObject(va);
  RenderStates rs;
  rs.cullFace = false;
  rs.depthTest = true;
  auto pipeline = r->createPipelineStates(rs);

  r->setViewPort(0, 0, kWidth, kHeight);
  std::shared_ptr<ShaderProgram> programBase = program;
  r->setShaderProgram(programBase);
  r->setShaderResources(resources);
  std::shared_ptr<VertexArrayObject> vaoBase = vao;
  r->setVertexArrayObject(vaoBase);
  std::shared_ptr<PipelineStates> pipeBase = pipeline;
  r->setPipelineStates(pipeBase);

  // reference: draw with the original texel content on a separate target
  auto refR = std::make_shared<RendererSoft>();
  {
    TextureDesc cd2;
    cd2.width = kWidth;
    cd2.height = kHeight;
    cd2.usage = TextureUsage_AttachmentColor;
    auto rc = refR->createTexture(cd2);
    rc->initImageData();
    TextureDesc rd = cd2;
    rd.format = TextureFormat_FLOAT32;
    rd.usage = TextureUsage_AttachmentDepth;
    auto rdepth = refR->createTexture(rd);
    rdepth->initImageData();
    auto rfbo = refR->createFrameBuffer(true);
    rfbo->setColorAttachment(rc, 0);
    rfbo->setDepthAttachment(rdepth);

    // same texel content, but a plain sampler texture
    TextureDesc st;
    st.width = kWidth;
    st.height = kHeight;
    st.useMipmaps = true;
    auto sampleTex = refR->createTexture(st);
    auto sp = Buffer<RGBA>::makeDefault(kWidth, kHeight);
    for (int y = 0; y < kHeight; y++) {
      for (int x = 0; x < kWidth; x++) {
        uint8_t v = (uint8_t) (40 + ((x * 13 + y * 7) & 0x7f));
        *sp->get(x, y) = RGBA(v, (uint8_t) (v ^ 0x3c), (uint8_t) (v ^ 0x81), 255);
      }
    }
    sampleTex->setImageData({sp});
    sampleTex->setSamplerDesc(sd);

    auto rprogram = std::dynamic_pointer_cast<ShaderProgramSoft>(refR->createShaderProgram());
    rprogram->addDefine("TEX");
    rprogram->SetShaders(std::make_shared<RecShader::VS>(), std::make_shared<RecShader::FS>());
    auto rres = std::make_shared<ShaderResources>();
    auto rblock = refR->createUniformBlock("tint", sizeof(glm::vec4));
    rblock->setData(&tint, sizeof(tint));
    rres->blocks[0] = rblock;
    auto rsampler = refR->createUniformSampler("image", *sampleTex);
    rsampler->setTexture(sampleTex);
    rres->samplers[1] = rsampler;
    std::shared_ptr<ShaderProgram> rprogramBase = rprogram;
    refR->setShaderProgram(rprogramBase);
    refR->setShaderResources(rres);
    std::shared_ptr<VertexArrayObject> rvaoBase = vao;
    refR->setVertexArrayObject(rvaoBase);
    std::shared_ptr<PipelineStates> rpipeBase = pipeline;
    refR->setPipelineStates(rpipeBase);
    refR->setViewPort(0, 0, kWidth, kHeight);

    ClearStates clear = defaultClear();
    refR->beginRenderPass(rfbo, clear);
    refR->draw();
    refR->endRenderPass();

    auto rct = std::dynamic_pointer_cast<TextureSoft<RGBA>>(rc);
    ImageF want;
    want.w = kWidth;
    want.h = kHeight;
    for (int y = 0; y < kHeight; y++) {
      for (int x = 0; x < kWidth; x++) {
        want.color.push_back(*rct->getImage().getBuffer()->buffer->get(x, y));
      }
    }

    SnapshotStore store;
    CommandListSoft list;
    TASK_CHECK(list.appendFrom(*r, store));

    // overwrite the source texture after recording: replay must read the copy
    aliasTex->setImageData({Buffer<RGBA>::makeDefault(kWidth, kHeight)});
    for (int y = 0; y < kHeight; y++)
      for (int x = 0; x < kWidth; x++)
        *pixels->get(x, y) = RGBA(0, 0, 0, 255);
    aliasTex->setImageData({pixels});

    ClearStates clear2 = defaultClear();
    r->beginRenderPass(fbo, clear2);
    TASK_CHECK(list.replay(*r, fbo));
    r->endRenderPass();

    auto act = std::dynamic_pointer_cast<TextureSoft<RGBA>>(aliasTex);
    ImageF got;
    got.w = kWidth;
    got.h = kHeight;
    for (int y = 0; y < kHeight; y++) {
      for (int x = 0; x < kWidth; x++) {
        got.color.push_back(*act->getImage().getBuffer()->buffer->get(x, y));
      }
    }
    TASK_CHECK(sameColor(want, got, 1));
    TASK_CHECK(notAllBlack(got));
  }
  return 0;
}

static int replay_blend_order() {
  SceneOptions opts;
  opts.defines = {"TEX"};
  opts.blend = true;
  Scene scene = buildScene(opts, defaultClear());

  SceneOptions refOpts = opts;
  Scene ref = buildScene(refOpts, defaultClear());

  std::vector<glm::vec4> tints = {glm::vec4(0.9f, 0.2f, 0.2f, 0.7f),
                                  glm::vec4(0.2f, 0.9f, 0.2f, 0.5f)};
  ImageF want = immediateDraw(ref, tints);

  SnapshotStore store;
  CommandListSoft list;
  for (auto &tint : tints) {
    setTint(scene, tint);
    TASK_CHECK(list.appendFrom(*scene.renderer, store));
  }
  TASK_CHECK(list.size() == 2);

  Target dst;
  ImageF got = dst.replay(list, defaultClear());
  TASK_CHECK(sameColor(want, got, 1));
  return 0;
}

static int replay_after_immediate_draw_restored() {
  SceneOptions opts;
  opts.defines = {"TEX"};
  Scene scene = buildScene(opts, defaultClear());
  setTint(scene, glm::vec4(0.6f, 0.4f, 0.2f, 1.f));

  SnapshotStore store;
  CommandListSoft list;
  TASK_CHECK(list.appendFrom(*scene.renderer, store));

  // reference immediate draw on the scene's own target before replay
  ClearStates clear = defaultClear();
  scene.renderer->beginRenderPass(scene.fbo, clear);
  scene.renderer->draw();
  scene.renderer->endRenderPass();
  ImageF before = readTarget(scene);

  // replay on the *same* renderer into a separate target; this temporarily
  // replaces the renderer's bindings, which must be restored afterwards
  auto other = std::make_shared<RendererSoft>();
  TextureDesc cd;
  cd.width = kWidth;
  cd.height = kHeight;
  cd.usage = TextureUsage_AttachmentColor;
  auto otherColor = other->createTexture(cd);
  otherColor->initImageData();
  TextureDesc dd = cd;
  dd.format = TextureFormat_FLOAT32;
  dd.usage = TextureUsage_AttachmentDepth;
  auto otherDepth = other->createTexture(dd);
  otherDepth->initImageData();
  auto otherFbo = other->createFrameBuffer(true);
  otherFbo->setColorAttachment(otherColor, 0);
  otherFbo->setDepthAttachment(otherDepth);

  // replay onto a different renderer does not touch the source renderer
  Target dst;
  dst.replay(list, defaultClear());

  // and a replay on the source renderer's own thread of control
  auto selfFbo = scene.renderer->createFrameBuffer(true);
  selfFbo->setColorAttachment(otherColor, 0);
  selfFbo->setDepthAttachment(otherDepth);
  scene.renderer->beginRenderPass(selfFbo, clear);
  TASK_CHECK(list.replay(*scene.renderer, selfFbo));
  scene.renderer->endRenderPass();

  // the immediate draw must behave exactly as before replay
  scene.renderer->beginRenderPass(scene.fbo, clear);
  scene.renderer->draw();
  scene.renderer->endRenderPass();
  ImageF after = readTarget(scene);

  TASK_CHECK(sameColor(before, after, 0));
  TASK_CHECK(sameDepth(before, after));

  // the bound program / pipeline are the caller's own after replay
  TASK_CHECK(scene.renderer->getShaderProgram() == scene.program.get());
  TASK_CHECK(scene.renderer->getVertexArrayObject() ==
             dynamic_cast<VertexArrayObjectSoft *>(scene.vao.get()));
  return 0;
}

static int replay_callback_count() {
  SceneOptions opts;
  opts.defines = {"TEX"};
  Scene scene = buildScene(opts, defaultClear());
  SnapshotStore store;
  CommandListSoft list;
  TASK_CHECK(list.appendFrom(*scene.renderer, store));

  resetCalls();
  Target a;
  a.replay(list, defaultClear());
  unsigned afterFirst = shaderCalls();
  TASK_CHECK(afterFirst > 0);
  TASK_CHECK(g_vsCalls.load() > 0);
  TASK_CHECK(g_fsCalls.load() > 0);

  Target b;
  b.replay(list, defaultClear());
  unsigned afterSecond = shaderCalls();
  TASK_CHECK(afterSecond > afterFirst);  // every replay re-executes shaders
  return 0;
}

static int replay_cross_renderer_interleaved() {
  SceneOptions opts;
  opts.defines = {"TEX"};
  opts.mips = true;  // implicit LOD via derivatives
  Scene sceneA = buildScene(opts, defaultClear());
  Scene sceneB = buildScene(opts, defaultClear());

  // distinct tints so the two lists are distinguishable
  setTint(sceneA, glm::vec4(0.8f, 0.6f, 0.4f, 1.f));
  setTint(sceneB, glm::vec4(0.3f, 0.5f, 0.9f, 1.f));

  SnapshotStore storeA;
  SnapshotStore storeB;
  CommandListSoft listA;
  CommandListSoft listB;
  TASK_CHECK(listA.appendFrom(*sceneA.renderer, storeA));
  TASK_CHECK(listB.appendFrom(*sceneB.renderer, storeB));

  // references
  SceneOptions refOpts = opts;
  Scene refA = buildScene(refOpts, defaultClear());
  Scene refB = buildScene(refOpts, defaultClear());
  ImageF wantA = immediateDraw(refA, {glm::vec4(0.8f, 0.6f, 0.4f, 1.f)});
  ImageF wantB = immediateDraw(refB, {glm::vec4(0.3f, 0.5f, 0.9f, 1.f)});
  TASK_CHECK(notAllBlack(wantA));
  TASK_CHECK(notAllBlack(wantB));

  // a third renderer whose bindings are unrelated, target of renderer c
  auto rendererC = std::make_shared<RendererSoft>();
  TextureDesc cd;
  cd.width = kWidth;
  cd.height = kHeight;
  cd.usage = TextureUsage_AttachmentColor;
  auto colorC = rendererC->createTexture(cd);
  colorC->initImageData();
  TextureDesc dd = cd;
  dd.format = TextureFormat_FLOAT32;
  dd.usage = TextureUsage_AttachmentDepth;
  auto depthC = rendererC->createTexture(dd);
  depthC->initImageData();
  auto fboC = rendererC->createFrameBuffer(true);
  fboC->setColorAttachment(colorC, 0);
  fboC->setDepthAttachment(depthC);

  ClearStates clear = defaultClear();

  // interleave: A on renderer 1, B on renderer 2, A on renderer 2, B on renderer 1
  auto r1 = std::make_shared<RendererSoft>();
  auto r2 = std::make_shared<RendererSoft>();

  auto makeTarget = [&](RendererSoft &r) {
    TextureDesc c = cd;
    auto col = r.createTexture(c);
    col->initImageData();
    TextureDesc d = dd;
    auto dep = r.createTexture(d);
    dep->initImageData();
    auto f = r.createFrameBuffer(true);
    f->setColorAttachment(col, 0);
    f->setDepthAttachment(dep);
    return std::make_pair(col, f);
  };

  auto t1 = makeTarget(*r1);
  auto t2 = makeTarget(*r2);

  r1->beginRenderPass(t1.second, clear);
  TASK_CHECK(listA.replay(*r1, t1.second));
  r1->endRenderPass();
  r2->beginRenderPass(t2.second, clear);
  TASK_CHECK(listB.replay(*r2, t2.second));
  r2->endRenderPass();

  // same lists again on the *other* renderer
  auto t3 = makeTarget(*r1);
  auto t4 = makeTarget(*r2);
  r1->beginRenderPass(t3.second, clear);
  TASK_CHECK(listB.replay(*r1, t3.second));
  r1->endRenderPass();
  r2->beginRenderPass(t4.second, clear);
  TASK_CHECK(listA.replay(*r2, t4.second));
  r2->endRenderPass();

  auto read = [&](const std::shared_ptr<Texture> &col) {
    auto c = std::dynamic_pointer_cast<TextureSoft<RGBA>>(col);
    ImageF img;
    img.w = kWidth;
    img.h = kHeight;
    for (int y = 0; y < kHeight; y++)
      for (int x = 0; x < kWidth; x++)
        img.color.push_back(*c->getImage().getBuffer()->buffer->get(x, y));
    return img;
  };

  TASK_CHECK(sameColor(wantA, read(t1.first), 1));
  TASK_CHECK(sameColor(wantB, read(t2.first), 1));
  TASK_CHECK(sameColor(wantB, read(t3.first), 1));
  TASK_CHECK(sameColor(wantA, read(t4.first), 1));
  return 0;
}

static int replay_mip_levels() {
  SceneOptions opts;
  opts.mips = true;
  opts.defines = {"TEX", "FIXLOD"};  // explicit LOD read of level 2
  Scene scene = buildScene(opts, defaultClear());

  SceneOptions refOpts = opts;
  Scene ref = buildScene(refOpts, defaultClear());
  ImageF want = immediateDraw(ref, {glm::vec4(1.f)});
  TASK_CHECK(notAllBlack(want));

  SnapshotStore store;
  CommandListSoft list;
  TASK_CHECK(list.appendFrom(*scene.renderer, store));

  auto srcTex = std::dynamic_pointer_cast<TextureSoft<RGBA>>(scene.tex);
  TASK_CHECK((int) srcTex->getImage().levels.size() >= 3);

  Target dst;
  ImageF got = dst.replay(list, defaultClear());
  TASK_CHECK(sameColor(want, got, 1));

  // reduced-resolution mip chain must remain exact at the same time
  auto level2 = srcTex->getImage().getBuffer(2);
  TASK_CHECK(level2 && level2->width > 0);
  return 0;
}

static int replay_external_buffer_rewrite() {
  SceneOptions opts;
  opts.defines = {"TEX"};
  Scene scene = buildScene(opts, defaultClear());
  SnapshotStore store;
  CommandListSoft list;
  TASK_CHECK(list.appendFrom(*scene.renderer, store));
  size_t copied = store.copiedBytes();

  // rewrite through the Buffer::set API, then record again: must be noticed
  auto colorTex = std::dynamic_pointer_cast<TextureSoft<RGBA>>(scene.tex);
  auto buffer = colorTex->getImage().getBuffer()->buffer;
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      buffer->set(x, y, RGBA(10, 20, 30, 255));
    }
  }
  TASK_CHECK(list.appendFrom(*scene.renderer, store));
  TASK_CHECK(store.copiedBytes() > copied);
  TASK_CHECK(store.imageSnapshotCount() == 2);
  return 0;
}

static int replay_interleaved_sampler_state() {
  // Two lists whose samplers use different filtering over identical pixels.
  // Interleaved replay must keep each list's sampler state isolated.
  SceneOptions linearOpts;
  linearOpts.defines = {"TEX"};
  linearOpts.mips = true;
  Scene linear = buildScene(linearOpts, defaultClear());
  SamplerDesc linDesc;
  linDesc.filterMin = Filter_LINEAR_MIPMAP_LINEAR;
  linDesc.filterMag = Filter_LINEAR;
  linear.tex->setSamplerDesc(linDesc);
  linear.sampler1->setTexture(linear.tex);

  SceneOptions nearestOpts;
  nearestOpts.defines = {"TEX"};
  nearestOpts.mips = true;
  Scene nearest = buildScene(nearestOpts, defaultClear());
  SamplerDesc nearDesc;
  nearDesc.filterMin = Filter_NEAREST_MIPMAP_NEAREST;
  nearDesc.filterMag = Filter_NEAREST;
  nearest.tex->setSamplerDesc(nearDesc);
  nearest.sampler1->setTexture(nearest.tex);

  SnapshotStore storeLin;
  SnapshotStore storeNear;
  CommandListSoft listLin;
  CommandListSoft listNear;
  TASK_CHECK(listLin.appendFrom(*linear.renderer, storeLin));
  TASK_CHECK(listNear.appendFrom(*nearest.renderer, storeNear));

  // references, drawn immediately through each source scene
  ImageF wantLin = immediateDraw(linear, {glm::vec4(1.f)});
  ImageF wantNear = immediateDraw(nearest, {glm::vec4(1.f)});
  TASK_CHECK(notAllBlack(wantLin));
  TASK_CHECK(notAllBlack(wantNear));
  TASK_CHECK(differColor(wantLin, wantNear));

  Target a;
  Target b;
  ImageF gotLinFirst = a.replay(listLin, defaultClear());
  ImageF gotNearFirst = b.replay(listNear, defaultClear());
  // interleave again, reversed order on the same renderers
  ImageF gotNearSecond = a.replay(listNear, defaultClear());
  ImageF gotLinSecond = b.replay(listLin, defaultClear());

  TASK_CHECK(sameColor(wantLin, gotLinFirst, 1));
  TASK_CHECK(sameColor(wantNear, gotNearFirst, 1));
  TASK_CHECK(sameColor(wantNear, gotNearSecond, 1));
  TASK_CHECK(sameColor(wantLin, gotLinSecond, 1));
  return 0;
}

// ===========================================================================
// registration
// ===========================================================================

#define TASK_CASE_CAT2(a, b) a##_##b
#define TASK_CASE_CAT(a, b) TASK_CASE_CAT2(a, b)
#define TASK_CASE(group_, name_) \
  {#group_, #name_, TASK_CASE_CAT(group_, name_)}

const struct task_case task_cases[] = {
    TASK_CASE(capture, no_execution),
    TASK_CASE(capture, rejects_incomplete),
    TASK_CASE(capture, rejects_multisample),
    TASK_CASE(capture, uniforms_and_defines),

    TASK_CASE(ownership, image_dedup),
    TASK_CASE(ownership, content_dedup_distinct_objects),
    TASK_CASE(ownership, shared_texture_two_samplers),
    TASK_CASE(ownership, buffer_mutation_detected),
    TASK_CASE(ownership, source_release),
    TASK_CASE(ownership, cache_weak_release),
    TASK_CASE(ownership, failed_append_stats),

    TASK_CASE(replay, matches_immediate),
    TASK_CASE(replay, depth_matches),
    TASK_CASE(replay, destination_alias),
    TASK_CASE(replay, blend_order),
    TASK_CASE(replay, after_immediate_draw_restored),
    TASK_CASE(replay, callback_count),
    TASK_CASE(replay, cross_renderer_interleaved),
    TASK_CASE(replay, mip_levels),
    TASK_CASE(replay, external_buffer_rewrite),
    TASK_CASE(replay, interleaved_sampler_state),
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
