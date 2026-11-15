#include <TestHelpers.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(Geometry, WorldTransformsAndInverseTranspose) {
  auto t =
      triangle({Vertex({0, 0, 0}, {0, -1, 1}), Vertex({1, 0, 0}, {0, -1, 1}),
                Vertex({0, 1, 1}, {0, -1, 1})});
  t->updateModelMatrix({0, 1, 0}, 31, {2, 3, -2}, {2, 1, .5f});
  std::vector<RasterTriangle> stream;
  t->appendRasterTriangles(stream);
  auto v = stream.front().vertices;
  auto a = v[1].position - v[0].position, b = v[2].position - v[0].position;
  auto n = unit(glm::cross(a, b));
  expectNear(glm::dot(n, a), 0);
  expectNear(glm::dot(v[0].normal, a), 0);
  expectNear(glm::dot(v[0].normal, b), 0);
  expectNear(t->getArea(), glm::length(glm::cross(a, b)) * .5f);
  auto center = (v[0].position + v[1].position + v[2].position) / 3.f;
  auto h = t->getIntersect(Ray(center + 2.f * n, -n));
  ASSERT_TRUE(h.intersected) << "transformed triangle missed";
  expectNear(h.coords, center);
  expectNear(h.normal, n);
  auto before = t->getBounds();
  EXPECT_THROW(
      ([&] { t->updateModelMatrix({0, 1, 0}, 0, {0, 0, 0}, {0, 1, 1}); })(),
      std::exception);
  expectNear(t->getBounds().min, before.min);
  auto s = cameraScene();
  s->addGraphicObj(t, "t");
  s->prepare();
  s->setViewMatrix({4, 2, 1}, {0, 0, 0}, {0, 1, 0});
  s->prepare();
  expectNear(s->intersect(Ray(center + 2.f * n, -n)).coords, center);
  expectNear(t->getBounds().min, before.min);
}

} // namespace
