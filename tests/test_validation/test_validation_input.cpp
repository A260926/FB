#include <TestHelpers.hpp>
#include <object/Cube.hpp>
#include <object/Sphere.hpp>
#include <render/Rasterizer.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(Validation, RejectsInvalidInputAndHandlesEmptySceneAndCube) {
  EXPECT_THROW(([] { TraditionalRasterizer r(0, 1); })(), std::exception);
  EXPECT_THROW(([] { PathTracing p(1, 1, 0); })(), std::exception);
  PathTracing p(1, 1);
  auto invalid = settings();
  invalid.survivalProbability = 0;
  EXPECT_THROW(([&] { p.configure(invalid); })(), std::exception);
  EXPECT_THROW(([] {
    Shader s;
    s.setFragmentShader(SHADERS_TYPE::BUMP);
  })(), std::exception);
  TraditionalRasterizer empty(3, 3);
  empty.draw();
  for (auto c : empty.pixels())
    expectNear(c, glm::vec3(0));
  EXPECT_THROW(([] { Sphere s({0, 0, 0}, 1e30f); })(), std::exception);
  auto owned = cameraScene();
  ASSERT_TRUE(owned->addGraphicObj(std::make_unique<Sphere>(), "unique")) << "unique_ptr overload failed";
  auto s = cameraScene();
  s->addGraphicObj(std::make_shared<Cube>(), "cube");
  s->prepare();
  ASSERT_TRUE(s->triangles().size() == 12) << "cube geometry missing";
}

} // namespace
