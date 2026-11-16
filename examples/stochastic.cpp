// Progressive stochastic transparency demonstration.
//
// Renders two intersecting thin surfaces (a red and a blue quad, each 50%
// opaque), an opaque occluder in front of them, and writes the converged
// frame to a binary PPM. Everything goes through the production
// TraditionalRasterizer stochastic API; no test-only code is involved.
#include <object/Mesh.hpp>
#include <render/Rasterizer.hpp>
#include <iostream>

using namespace SoftRasterizer;

namespace {

// Camera-facing quad made of two source triangles.
std::shared_ptr<Mesh> quad(glm::vec3 color, float opacity) {
  std::vector<Vertex> vertices{
      Vertex({-1.2f, -1.2f, 0.f}, {}, {0, 0}),
      Vertex({1.2f, -1.2f, 0.f}, {}, {1, 0}),
      Vertex({1.2f, 1.2f, 0.f}, {}, {1, 1}),
      Vertex({-1.2f, 1.2f, 0.f}, {}, {0, 1})};
  std::vector<glm::uvec3> faces{{0, 1, 2}, {0, 2, 3}};
  auto material = std::make_shared<Material>();
  material->Kd = color;
  material->setOpacity(opacity);
  auto mesh = std::make_shared<Mesh>(
      vertices, faces,
      std::vector<std::shared_ptr<Material>>{material, material});
  auto shader = std::make_shared<Shader>();
  shader->type = SHADERS_TYPE::TEXTURE; // Unlit, so colors stay exact.
  mesh->bindShader2Mesh(shader);
  return mesh;
}

} // namespace

int main(int argc, char **argv) {
  const std::string outputPath =
      argc > 1 ? argv[1] : "stochastic.ppm";
  constexpr std::size_t width = 256, height = 256;

  auto scene = std::make_shared<Scene>(
      "stochastic_demo", glm::vec3(0.f), glm::vec3(0.f, 0.f, -1.f),
      glm::vec3(0.f, 1.f, 0.f),
      /*background=*/glm::vec3(0.02f, 0.02f, 0.03f));
  scene->setProjectionMatrix(60.f, 0.05f, 100.f);

  // Two thin, half-opaque quads whose projections cross. The blue quad is
  // rotated about the vertical axis and pushed slightly back; the red quad
  // faces the camera. Their overlap is the region the stochastic path
  // resolves progressively instead of keeping only the nearest color.
  auto red = quad(glm::vec3(0.9f, 0.08f, 0.08f), 0.5f);
  red->updateModelMatrix(glm::vec3(0, 1, 0), 0.f,
                         glm::vec3(-0.25f, 0.f, -3.f), glm::vec3(1.f));
  auto blue = quad(glm::vec3(0.08f, 0.2f, 0.95f), 0.5f);
  blue->updateModelMatrix(glm::vec3(0, 1, 0), 55.f,
                          glm::vec3(0.25f, 0.f, -3.4f), glm::vec3(1.f));
  // A small fully opaque occluder in front of both transparent surfaces.
  auto occluder = quad(glm::vec3(0.85f, 0.85f, 0.85f), 1.f);
  occluder->updateModelMatrix(glm::vec3(0, 1, 0), 0.f,
                              glm::vec3(0.f, -0.55f, -2.2f),
                              glm::vec3(0.28f));

  scene->addGraphicObj(red, "red");
  scene->addGraphicObj(blue, "blue");
  scene->addGraphicObj(occluder, "occluder");
  // Stable, non-overlapping id ranges (each quad owns two source triangles).
  scene->setSurfaceId("red", 10);      // 10..11
  scene->setSurfaceId("blue", 20);     // 20..21
  scene->setSurfaceId("occluder", 30); // 30..31

  TraditionalRasterizer rasterizer(width, height);
  rasterizer.addScene(scene);

  StochasticSettings settings;
  settings.seed = 0xC0FFEEULL;
  settings.generation = 0;
  rasterizer.configureStochastic(settings);

  // Fixed-memory convergence: many frames, eight samples per pixel, no
  // transparent fragment lists.
  constexpr std::uint64_t frames = 256;
  rasterizer.advance(0, frames);
  rasterizer.resolveStochastic();
  std::cout << "stochastic frames accumulated: "
            << rasterizer.accumulatedFrames() << "\n";

  rasterizer.save(outputPath);
  std::cout << "wrote " << outputPath << "\n";
  return 0;
}
