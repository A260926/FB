#include <TestHelpers.hpp>
#include <object/Sphere.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(Scene, OwnershipRevisionAndJointLightPDF) {
  auto o = std::make_shared<Sphere>(glm::vec3(0, 0, -3), 1);
  auto s = cameraScene();
  ASSERT_TRUE(s->addGraphicObj(o, "sphere")) << "add failed";
  ASSERT_TRUE(!s->addGraphicObj(o, "sphere")) << "duplicate accepted";
  s->prepare();
  expectNear(s->intersect(Ray({0, 0, 0}, {0, 0, -1})).intersect_time, 2);
  o->updateModelMatrix({0, 1, 0}, 0, {3, 0, 0}, {1, 1, 1});
  s->prepare();
  ASSERT_TRUE(!s->intersect(Ray({0, 0, 0}, {0, 0, -1})).intersected)
      << "stale BVH";
  Sampler rng;
  ASSERT_TRUE(s->sampleLight(rng).pdfArea == 0) << "nonemissive selected";
  o->getMaterial()->emission = {3, 3, 3};
  s->prepare();
  expectNear(s->sampleLight(rng).pdfArea, 1 / (4 * Pi));
  auto second = std::make_shared<Sphere>(glm::vec3(-4, 0, -3), 2);
  second->getMaterial()->emission = {1, 1, 1};
  s->addGraphicObj(second, "second");
  s->prepare();
  for (int i = 0; i < 100; ++i) {
    expectNear(s->sampleLight(rng).pdfArea, 1 / (20 * Pi));
  }
  auto handle = *s->getMeshObj("sphere");
  s.reset();
  o.reset();
  ASSERT_TRUE(handle->getArea() > 0) << "dangling scene handle";
}

} // namespace
