// recorded_material.cpp — record two material draws, release the sources, then
// replay onto two offscreen targets and write the results as PPM images.
#include <cstdio>
#include <memory>
#include <vector>

#include "Render/Software/CommandListSoft.h"
#include "Render/Software/RendererSoft.h"
#include "Render/Software/ShaderProgramSoft.h"
#include "Render/Software/TextureSoft.h"
#include "Render/Software/UniformSoft.h"

using namespace SoftGL;

namespace {

constexpr int kWidth = 96;
constexpr int kHeight = 96;

struct ShaderDefines {
  uint8_t USE_MAP;
};

struct ShaderAttributes {
  glm::vec4 position;
  glm::vec2 uv;
};

struct ShaderUniforms {
  glm::vec4 baseColor;
  glm::vec2 uvScale;
  Sampler2DSoft<RGBA> *map;
};

struct ShaderVaryings {
  glm::vec2 uv;
};

class MaterialShader : public ShaderSoft {
 public:
  CREATE_SHADER_OVERRIDE

  std::vector<std::string> &getDefines() override {
    static std::vector<std::string> defines = {"USE_MAP"};
    return defines;
  }

  std::vector<UniformDesc> &getUniformsDesc() override {
    static std::vector<UniformDesc> desc = {
        {"baseColor", offsetof(ShaderUniforms, baseColor)},
        {"uvScale", offsetof(ShaderUniforms, uvScale)},
        {"map", offsetof(ShaderUniforms, map)},
    };
    return desc;
  }
};

class VS : public MaterialShader {
 public:
  CREATE_SHADER_CLONE(VS)
  void shaderMain() override {
    gl->Position = a->position;
    v->uv = a->uv;
  }
};

class FS : public MaterialShader {
 public:
  CREATE_SHADER_CLONE(FS)

  size_t getSamplerDerivativeOffset(BaseSampler<RGBA> *) const override {
    return offsetof(ShaderVaryings, uv);
  }

  void setupSamplerDerivative() override {
    if (def->USE_MAP) {
      u->map->setLodFunc(&texLodFunc);
    }
  }

  void shaderMain() override {
    glm::vec4 color = u->baseColor;
    if (def->USE_MAP) {
      color *= texture(u->map, v->uv * u->uvScale);
    }
    gl->FragColor = color;
  }
};

// An in-memory checker / gradient texture.
std::shared_ptr<Texture> makeMemoryTexture(RendererSoft &renderer, int size, RGBA a, RGBA b) {
  TextureDesc desc;
  desc.width = size;
  desc.height = size;
  desc.useMipmaps = true;
  auto tex = renderer.createTexture(desc);
  auto pixels = Buffer<RGBA>::makeDefault(size, size);
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      bool checker = ((x / 4) + (y / 4)) % 2 == 0;
      RGBA c = checker ? a : b;
      float shade = 0.6f + 0.4f * ((float) x / (float) (size - 1));
      *pixels->get(x, y) = RGBA((uint8_t) (c.r * shade), (uint8_t) (c.g * shade),
                                (uint8_t) (c.b * shade), 255);
    }
  }
  tex->setImageData({pixels});
  SamplerDesc sampler;
  sampler.filterMin = Filter_LINEAR_MIPMAP_LINEAR;
  sampler.filterMag = Filter_LINEAR;
  sampler.wrapS = Wrap_REPEAT;
  sampler.wrapT = Wrap_REPEAT;
  tex->setSamplerDesc(sampler);
  return tex;
}

// One material: geometry, program, uniforms and sampler, all source-side.
struct Material {
  std::shared_ptr<Texture> texture;
  std::shared_ptr<ShaderProgramSoft> program;
  std::shared_ptr<ShaderResources> resources;
  std::shared_ptr<UniformBlock> colorBlock;
  std::shared_ptr<UniformBlock> scaleBlock;
  std::shared_ptr<UniformSampler> sampler;
  std::shared_ptr<VertexArrayObject> vao;
  std::shared_ptr<PipelineStates> pipeline;
  std::vector<ShaderAttributes> verts;
  std::vector<int32_t> indices;

  void release() {
    colorBlock.reset();
    scaleBlock.reset();
    sampler.reset();
    resources.reset();
    program.reset();
    vao.reset();
    pipeline.reset();
    texture.reset();
  }
};

Material buildMaterial(RendererSoft &renderer, RGBA a, RGBA b, glm::vec4 base, glm::vec2 uvScale,
                       float depth, bool blend) {
  Material m;
  m.texture = makeMemoryTexture(renderer, 32, a, b);

  m.program = std::dynamic_pointer_cast<ShaderProgramSoft>(renderer.createShaderProgram());
  m.program->addDefine("USE_MAP");
  m.program->SetShaders(std::make_shared<VS>(), std::make_shared<FS>());

  m.resources = std::make_shared<ShaderResources>();
  m.colorBlock = renderer.createUniformBlock("baseColor", sizeof(glm::vec4));
  m.colorBlock->setData(&base, sizeof(base));
  m.resources->blocks[0] = m.colorBlock;

  m.scaleBlock = renderer.createUniformBlock("uvScale", sizeof(glm::vec2));
  m.scaleBlock->setData(&uvScale, sizeof(uvScale));
  m.resources->blocks[1] = m.scaleBlock;

  m.sampler = renderer.createUniformSampler("map", *m.texture);
  m.sampler->setTexture(m.texture);
  m.resources->samplers[2] = m.sampler;

  m.verts = {
      {{-0.9f, -0.9f, depth, 1.f}, {0.f, 0.f}},
      {{0.9f, -0.9f, depth, 1.f}, {1.f, 0.f}},
      {{-0.9f, 0.9f, depth, 1.f}, {0.f, 1.f}},
      {{0.9f, 0.9f, depth, 1.f}, {1.f, 1.f}},
  };
  m.indices = {0, 1, 2, 2, 1, 3};

  VertexArray va;
  va.vertexesDesc = {{4, sizeof(ShaderAttributes), 0}};
  va.vertexesBuffer = reinterpret_cast<uint8_t *>(m.verts.data());
  va.vertexesBufferLength = m.verts.size() * sizeof(ShaderAttributes);
  va.indexBuffer = m.indices.data();
  va.indexBufferLength = m.indices.size() * sizeof(int32_t);
  m.vao = renderer.createVertexArrayObject(va);

  RenderStates states;
  states.cullFace = false;
  states.depthTest = true;
  if (blend) {
    states.blend = true;
    states.blendParams.SetBlendFactor(BlendFactor_SRC_ALPHA, BlendFactor_ONE_MINUS_SRC_ALPHA);
  }
  m.pipeline = renderer.createPipelineStates(states);
  return m;
}

void writePPM(const char *path, const std::shared_ptr<Texture> &color) {
  auto tex = std::dynamic_pointer_cast<TextureSoft<RGBA>>(color);
  auto *buffer = tex->getImage().getBuffer()->buffer.get();
  FILE *f = std::fopen(path, "wb");
  if (!f) {
    std::fprintf(stderr, "failed to open %s\n", path);
    return;
  }
  std::fprintf(f, "P6\n%d %d\n255\n", kWidth, kHeight);
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      RGBA c = *buffer->get(x, y);
      std::fputc(c.r, f);
      std::fputc(c.g, f);
      std::fputc(c.b, f);
    }
  }
  std::fclose(f);
}

struct Target {
  std::shared_ptr<FrameBuffer> fbo;
  std::shared_ptr<Texture> color;

  static Target make(RendererSoft &renderer) {
    Target t;
    TextureDesc colorDesc;
    colorDesc.width = kWidth;
    colorDesc.height = kHeight;
    colorDesc.usage = TextureUsage_AttachmentColor;
    t.color = renderer.createTexture(colorDesc);
    t.color->initImageData();

    TextureDesc depthDesc = colorDesc;
    depthDesc.format = TextureFormat_FLOAT32;
    depthDesc.usage = TextureUsage_AttachmentDepth;
    auto depth = renderer.createTexture(depthDesc);
    depth->initImageData();

    t.fbo = renderer.createFrameBuffer(true);
    t.fbo->setColorAttachment(t.color, 0);
    t.fbo->setDepthAttachment(depth);
    return t;
  }
};

}  // namespace

int main(int argc, char **argv) {
  const char *outDir = argc > 1 ? argv[1] : ".";

  // --- record phase -------------------------------------------------------
  auto recorder = std::make_shared<RendererSoft>();
  Material materialA = buildMaterial(*recorder, RGBA(28, 200, 120, 255), RGBA(20, 60, 180, 255),
                                     glm::vec4(1.f, 1.f, 1.f, 1.f), glm::vec2(2.f, 2.f), 0.4f,
                                     false);
  Material materialB = buildMaterial(*recorder, RGBA(230, 90, 30, 255), RGBA(250, 230, 90, 255),
                                     glm::vec4(0.9f, 0.9f, 1.f, 0.6f), glm::vec2(1.f, 3.f), 0.6f,
                                     true);

  SnapshotStore store;
  CommandListSoft list;

  for (Material *m : {&materialA, &materialB}) {
    std::shared_ptr<ShaderProgram> programBase = m->program;
    std::shared_ptr<VertexArrayObject> vaoBase = m->vao;
    std::shared_ptr<PipelineStates> pipeBase = m->pipeline;
    recorder->setShaderProgram(programBase);
    recorder->setShaderResources(m->resources);
    recorder->setVertexArrayObject(vaoBase);
    recorder->setPipelineStates(pipeBase);
    recorder->setViewPort(0, 0, kWidth, kHeight);
    if (!list.appendFrom(*recorder, store)) {
      std::fprintf(stderr, "record failed\n");
      return 1;
    }
  }

  std::printf("recorded draws=%zu image_snapshots=%zu mesh_snapshots=%zu held_bytes=%zu copied_bytes=%zu\n",
              list.size(), store.imageSnapshotCount(), store.meshSnapshotCount(), store.heldBytes(),
              store.copiedBytes());

  // --- release every source object, including the recording renderer ---------
  materialA.release();
  materialB.release();
  recorder.reset();

  // --- replay phase: two different materials, two offscreen targets ----------
  RendererSoft replayA;
  RendererSoft replayB;

  Target targetA = Target::make(replayA);
  Target targetB = Target::make(replayB);

  ClearStates clear;
  clear.colorFlag = true;
  clear.depthFlag = true;
  clear.clearColor = glm::vec4(0.05f, 0.06f, 0.08f, 1.f);
  clear.clearDepth = 1.f;

  replayA.beginRenderPass(targetA.fbo, clear);
  if (!list.replay(replayA, targetA.fbo)) {
    std::fprintf(stderr, "replay A failed\n");
    return 1;
  }
  replayA.endRenderPass();

  // the same command list handed to a second renderer
  replayB.beginRenderPass(targetB.fbo, clear);
  if (!list.replay(replayB, targetB.fbo)) {
    std::fprintf(stderr, "replay B failed\n");
    return 1;
  }
  replayB.endRenderPass();

  char pathA[1024];
  char pathB[1024];
  std::snprintf(pathA, sizeof(pathA), "%s/recorded_material_a.ppm", outDir);
  std::snprintf(pathB, sizeof(pathB), "%s/recorded_material_b.ppm", outDir);
  writePPM(pathA, targetA.color);
  writePPM(pathB, targetB.color);
  std::printf("wrote %s and %s\n", pathA, pathB);
  return 0;
}
