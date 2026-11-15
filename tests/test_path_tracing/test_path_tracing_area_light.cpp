#include <TestHelpers.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(PathTracing, AreaLightMatchesQuadratureWithoutDuplicateEmission) {
  auto s = cameraScene(.01f);
  auto receiver = mat(glm::vec3(.6f));
  s->addGraphicObj(plane(-2, 10, receiver), "receiver");
  auto emitter = mat(glm::vec3(0));
  emitter->emission = glm::vec3(5);
  auto light = plane(-1, .5f, emitter, false);
  light->updateModelMatrix({0, 1, 0}, 0, {1, 0, 0}, {1, 1, 1});
  s->addGraphicObj(light, "light");
  PathTracing pt(1, 1);
  auto config = settings(65536, 1);
  pt.configure(config);
  pt.addScene(s);
  pt.draw();
  double integral = 0;
  constexpr int n = 256;
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      double px = .5 + (x + .5) / n, py = -.5 + (y + .5) / n;
      double r2 = px * px + py * py + 1;
      integral += 1 / (r2 * r2);
    }
  }
  float expected = float(integral / (n * n)) * .6f * 5 / Pi;
  expectNear(pt.pixels()[0], glm::vec3(expected), expected * .02f);
  auto first = pt.pixels()[0];
  config.maxDepth = 2;
  pt.configure(config);
  pt.draw();
  expectNear(pt.pixels()[0], first, .00001f);
}

} // namespace
