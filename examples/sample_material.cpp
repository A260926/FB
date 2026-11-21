// Per-sample material demo for the SoftGL software renderer.
//
// Renders a scene through the software triangle fill path with per-sample
// shading (RenderStates::sampleShading) into a 4x MSAA offscreen target and
// writes the resolved image as a binary PPM (P6) file.
//
//   usage: sample_material <output.ppm>
//
// The fragment shader gives every MSAA sample its own material: a per-sample
// tint (indexed by ShaderBuiltin::SampleID) modulates a checkerboard, and
// transparent holes are punched at sample granularity with discard, so the
// background shows through 1 of 4 samples in every other checker cell.

#include <cstdio>
#include <fstream>

#include "Render/Software/RendererSoft.h"
#include "Render/Software/ShaderProgramSoft.h"
#include "Render/Software/UniformSoft.h"

using namespace SoftGL;

namespace {

struct ShaderDefines {};
struct ShaderAttributes {
  glm::vec4 position;
  glm::vec2 uv;
};
struct ShaderVaryings {
  glm::vec2 uv;
};
struct ShaderUniforms {
  glm::vec4 tint[4];   // per-sample material tint, indexed by SampleID
  glm::vec4 base;      // base material color
  float cells;         // checker cells per uv unit
  int holes;           // punch per-sample transparent holes when != 0
};

class Common : public ShaderSoft {
 public:
  CREATE_SHADER_OVERRIDE
  std::vector<std::string> &getDefines() override {
    static std::vector<std::string> defs;
    return defs;
  }
  std::vector<UniformDesc> &getUniformsDesc() override {
    static std::vector<UniformDesc> desc = {{"params", offsetof(ShaderUniforms, tint)}};
    return desc;
  }
};

class VS : public Common {
 public:
  CREATE_SHADER_CLONE(VS)
  void shaderMain() override {
    gl->Position = a->position;
    v->uv = a->uv;
  }
};

class FS : public Common {
 public:
  CREATE_SHADER_CLONE(FS)
  void shaderMain() override {
    glm::vec2 cell = glm::floor(v->uv * u->cells);
    float checker = glm::mod(cell.x + cell.y, 2.f);
    glm::vec4 color = u->base * (0.6f + 0.4f * checker);

    // per-sample material: every sample of the pixel gets its own tint
    int s = gl->SampleID >= 0 ? gl->SampleID : 0;
    color *= u->tint[s];

    // per-sample transparent holes: each checker cell drops one sample
    if (u->holes && gl->SampleID == ((int(cell.x) + 3 * int(cell.y)) & 3)) {
      gl->discard = true;
      return;
    }
    gl->FragColor = color;
  }
};

struct Vertex {
  glm::vec4 position;
  glm::vec2 uv;
};

void drawTriangle(RendererSoft &r, const Vertex (&verts)[3], const RenderStates &states) {
  int32_t indices[] = {0, 1, 2};
  VertexArray va;
  va.vertexesDesc = {{4, sizeof(Vertex), 0}};
  va.vertexesBuffer = (uint8_t *) verts;
  va.vertexesBufferLength = sizeof(verts);
  va.indexBuffer = indices;
  va.indexBufferLength = sizeof(indices);
  auto vao = r.createVertexArrayObject(va);
  r.setVertexArrayObject(vao);
  auto pipeline = r.createPipelineStates(states);
  r.setPipelineStates(pipeline);
  r.draw();
}

}   // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <output.ppm>\n", argv[0]);
    return 1;
  }

  const int W = 256, H = 256;
  RendererSoft renderer;

  // 4x MSAA offscreen target
  TextureDesc cd;
  cd.width = W;
  cd.height = H;
  cd.usage = TextureUsage_AttachmentColor;
  cd.multiSample = true;
  auto color = renderer.createTexture(cd);
  color->initImageData();
  TextureDesc dd = cd;
  dd.format = TextureFormat_FLOAT32;
  dd.usage = TextureUsage_AttachmentDepth;
  auto depth = renderer.createTexture(dd);
  depth->initImageData();
  auto fbo = renderer.createFrameBuffer(true);
  fbo->setColorAttachment(color, 0);
  fbo->setDepthAttachment(depth);

  // shader program
  auto program = renderer.createShaderProgram();
  auto ps = std::dynamic_pointer_cast<ShaderProgramSoft>(program);
  ps->SetShaders(std::make_shared<VS>(), std::make_shared<FS>());

  // uniforms
  ShaderUniforms uniforms{};
  uniforms.cells = 6.f;
  auto block = renderer.createUniformBlock("params", sizeof(ShaderUniforms));
  auto resources = std::make_shared<ShaderResources>();
  resources->blocks[0] = block;

  renderer.setViewPort(0, 0, W, H);
  renderer.setShaderProgram(program);

  RenderStates states;
  states.cullFace = false;
  states.depthTest = true;
  states.depthFunc = DepthFunc_LESS;
  states.sampleShading = true;   // one fragment shader execution per sample

  ClearStates clear;
  clear.colorFlag = true;
  clear.depthFlag = true;
  clear.clearColor = glm::vec4(0.02f, 0.02f, 0.05f, 1.f);
  clear.clearDepth = 1.f;
  renderer.beginRenderPass(fbo, clear);

  // background: cool gradient checker, all samples tinted equally
  uniforms.base = glm::vec4(0.15f, 0.25f, 0.55f, 1.f);
  for (auto &t : uniforms.tint) {
    t = glm::vec4(1.f);
  }
  uniforms.holes = 0;
  block->setData(&uniforms, sizeof(uniforms));
  renderer.setShaderResources(resources);
  Vertex bg[3] = {
      {{-1.f, -1.f, 0.8f, 1.f}, {0.f, 0.f}},
      {{ 3.f, -1.f, 0.8f, 1.f}, {2.f, 0.f}},
      {{-1.f,  3.f, 0.8f, 1.f}, {0.f, 2.f}},
  };
  drawTriangle(renderer, bg, states);

  // mid layer: warm triangle with per-sample tints
  uniforms.base = glm::vec4(0.85f, 0.45f, 0.15f, 1.f);
  uniforms.tint[0] = glm::vec4(1.00f, 0.55f, 0.55f, 1.f);
  uniforms.tint[1] = glm::vec4(0.55f, 1.00f, 0.55f, 1.f);
  uniforms.tint[2] = glm::vec4(0.55f, 0.65f, 1.00f, 1.f);
  uniforms.tint[3] = glm::vec4(1.00f, 1.00f, 0.60f, 1.f);
  uniforms.holes = 0;
  block->setData(&uniforms, sizeof(uniforms));
  renderer.setShaderResources(resources);
  Vertex mid[3] = {
      {{-0.85f, -0.75f, 0.5f, 1.f}, {0.f, 0.f}},
      {{ 0.95f, -0.65f, 0.5f, 1.f}, {1.2f, 0.f}},
      {{-0.55f,  0.85f, 0.5f, 1.f}, {0.3f, 1.1f}},
  };
  drawTriangle(renderer, mid, states);

  // foreground: bright triangle with per-sample transparent holes
  uniforms.base = glm::vec4(0.95f, 0.85f, 0.30f, 1.f);
  uniforms.holes = 1;
  block->setData(&uniforms, sizeof(uniforms));
  renderer.setShaderResources(resources);
  Vertex fg[3] = {
      {{-0.45f, -0.45f, 0.3f, 1.f}, {0.f, 0.f}},
      {{ 0.65f, -0.35f, 0.3f, 1.f}, {1.0f, 0.f}},
      {{-0.25f,  0.55f, 0.3f, 1.f}, {0.4f, 0.9f}},
  };
  drawTriangle(renderer, fg, states);

  renderer.endRenderPass();

  // the 4x target is resolved inside draw(); dump it as PPM
  auto colorSoft = std::dynamic_pointer_cast<TextureSoft<RGBA>>(color);
  auto resolved = colorSoft->getImage().getBuffer()->buffer;
  std::ofstream out(argv[1], std::ios::binary);
  if (!out.is_open()) {
    std::fprintf(stderr, "failed to open output: %s\n", argv[1]);
    return 1;
  }
  out << "P6\n" << W << " " << H << "\n255\n";
  for (int y = H - 1; y >= 0; y--) {
    for (int x = 0; x < W; x++) {
      RGBA px = *resolved->get(x, y);
      char rgb[3] = {(char) px.r, (char) px.g, (char) px.b};
      out.write(rgb, 3);
    }
  }
  out.close();
  if (!out) {
    std::fprintf(stderr, "failed to write output: %s\n", argv[1]);
    return 1;
  }
  std::printf("wrote %s (%dx%d, 4x MSAA, per-sample shading)\n", argv[1], W, H);
  return 0;
}
