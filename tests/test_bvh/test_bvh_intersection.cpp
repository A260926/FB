#include <TestHelpers.hpp>
#include <object/Sphere.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(BVH, MatchesBruteForceAndParallelSlabs) {
  std::vector<std::shared_ptr<Object>> objects;
  Sampler rng(9);
  for (int i = 0; i < 128; ++i) {
    objects.push_back(std::make_shared<Sphere>(glm::vec3(12 * rng.next() - 6,
                                                         8 * rng.next() - 4,
                                                         -2 - 12 * rng.next()),
                                               .15f + .6f * rng.next()));
  }

  BVHAcceleration bvh;
  bvh.rebuild(objects);
  
  for (int i = 0; i < 2000; ++i) {
    Ray ray({0, 0, 2}, unit({2 * rng.next() - 1, 2 * rng.next() - 1, -1}),
            .001f, 5 + 20 * rng.next());
    Intersection expected;
    for (auto &o : objects) {
      auto h = o->getIntersect(ray);
      if (h.intersected && h.intersect_time < expected.intersect_time) {
        expected = h;
      }
    }

    auto actual = bvh.getIntersection(ray);
    ASSERT_TRUE(actual.intersected == expected.intersected)
        << "BVH visibility differs";
    ASSERT_TRUE(bvh.occluded(ray) == expected.intersected)
        << "shadow query differs";

    if (actual.intersected) {
      expectNear(actual.intersect_time, expected.intersect_time);
      ASSERT_TRUE(actual.obj == expected.obj) << "wrong nearest primitive";
    }
  }

  bvh.rebuild({});
  ASSERT_TRUE(!bvh.getIntersection(Ray({0, 0, 0}, {0, 0, -1})).intersected)
      << "empty BVH hit";
  Bounds3 box({-1, -1, -1}, {1, 1, 1});
  ASSERT_TRUE(box.intersect(Ray({1, 0, 2}, {0, 0, -1}), 10))
      << "parallel boundary ray missed";
  ASSERT_TRUE(!box.intersect(Ray({2, 0, 2}, {0, 0, -1}), 10))
      << "parallel outside ray hit";
}

} // namespace
