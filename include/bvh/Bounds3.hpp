#pragma once
#include <Tools.hpp>
#include <ray/Ray.hpp>

namespace SoftRasterizer {
struct Bounds3 {
  glm::vec3 min{std::numeric_limits<float>::infinity()};
  glm::vec3 max{-std::numeric_limits<float>::infinity()};
  Bounds3() = default;

  explicit Bounds3(const glm::vec3 &p) : min(p), max(p) {}

  Bounds3(const glm::vec3 &a, const glm::vec3 &b)
      : min(glm::min(a, b)), max(glm::max(a, b)) {}

  inline bool empty() const {
    return glm::any(glm::greaterThan(min, max));
  }

  inline void expand(const Bounds3 &b) {
    if (!b.empty()) {
      min = glm::min(min, b.min);
      max = glm::max(max, b.max);
    }
  }

  inline void expand(const glm::vec3 &p) {
    min = glm::min(min, p);
    max = glm::max(max, p);
  }

  inline glm::vec3 centroid() const {
    return 0.5f * (min + max);
  }

  inline glm::vec3 diagonal() const {
    return max - min;
  }

  inline bool intersect(const Ray &ray, float limit,
                        float *entry = nullptr) const {
    if (empty() || !finite(ray.origin) || !finite(ray.direction)) {
      return false;
    }
    double intervalNear = ray.tMin, intervalFar = std::min(limit, ray.tMax);
    for (int i = 0; i < 3; ++i) {
      // Exact zero is parallel; tiny nonzero directions still have a valid
      // interval.
      if (ray.direction[i] == 0) {
        if (ray.origin[i] < min[i] || ray.origin[i] > max[i]) {
          return false;
        }
        continue;
      }
      double slabNear = (double(min[i]) - ray.origin[i]) / ray.direction[i];
      double slabFar = (double(max[i]) - ray.origin[i]) / ray.direction[i];
      if (slabNear > slabFar) {
        std::swap(slabNear, slabFar);
      }
      intervalNear = std::max(intervalNear, slabNear);
      intervalFar = std::min(intervalFar, slabFar);
      if (intervalNear > intervalFar) {
        return false;
      }
    }
    if (entry) {
      *entry = static_cast<float>(intervalNear);
    }
    return intervalNear <= intervalFar;
  }
};
} // namespace SoftRasterizer
