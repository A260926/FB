#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>

namespace SoftRasterizer {
inline constexpr float Pi = 3.14159265358979323846f;

inline bool finite(const glm::vec3 &v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline glm::vec3 unit(const glm::vec3 &v,
                      const glm::vec3 &fallback = {0, 1, 0}) {
  const double l2 = glm::dot(glm::dvec3(v), glm::dvec3(v));
  return finite(v) && l2 > 1e-20f ? glm::vec3(glm::dvec3(v) / std::sqrt(l2))
                                  : fallback;
}

inline float maxComponent(const glm::vec3 &v) {
  return std::max({v.x, v.y, v.z});
}

// Transform a local direction to world space; n must be a unit normal.
inline glm::vec3 localToWorld(const glm::vec3 &v, const glm::vec3 &n) {
  constexpr float parallelEpsilon = 1e-3f;
  glm::vec3 reference(0, 1, 0);
  if (std::abs(n.y) > 1.f - parallelEpsilon) {
    reference = glm::vec3(0, 0, 1);
  }
  const glm::vec3 t = unit(glm::cross(reference, n));
  const glm::vec3 b = glm::cross(n, t);
  const glm::mat3 tbn(t, b, n); // GLM stores these axes as columns.
  return tbn * v;
}

inline glm::vec3 offsetOrigin(const glm::vec3 &p, const glm::vec3 &n,
                              const glm::vec3 &dir) {
  const float amount = 2e-5f * std::max(1.f, maxComponent(glm::abs(p)));
  return p + n * (glm::dot(dir, n) >= 0 ? amount : -amount);
}

inline float srgbToLinear(float x) {
  return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f);
}

inline float linearToSrgb(float x) {
  x = std::max(0.f, x);
  return x <= .0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.f / 2.4f) - .055f;
}

inline glm::mat4 composeTransform(const glm::vec3 &axis, float degrees,
                                  const glm::vec3 &translation,
                                  const glm::vec3 &scale) {
  if (!finite(axis) || !finite(translation) || !finite(scale) ||
      !std::isfinite(degrees) || glm::dot(axis, axis) < 1e-12f ||
      glm::any(glm::lessThanEqual(glm::abs(scale), glm::vec3(1e-8f)))) {
    throw std::invalid_argument("invalid model transform");
  }
  return glm::translate(glm::mat4(1), translation) *
         glm::rotate(glm::mat4(1), glm::radians(degrees), unit(axis)) *
         glm::scale(glm::mat4(1), scale);
}

inline float dielectricFresnel(float cosIncident, float etaI, float etaT) {
  cosIncident = std::clamp(std::abs(cosIncident), 0.f, 1.f);
  const float sinT2 =
      (etaI * etaI / (etaT * etaT)) * (1 - cosIncident * cosIncident);
  if (sinT2 >= 1) {
    return 1;
  }
  const float cosT = std::sqrt(1 - sinT2);
  const float rs =
      (etaI * cosIncident - etaT * cosT) / (etaI * cosIncident + etaT * cosT);
  const float rp =
      (etaT * cosIncident - etaI * cosT) / (etaT * cosIncident + etaI * cosT);
  return .5f * (rs * rs + rp * rp);
}
} // namespace SoftRasterizer
