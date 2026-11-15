#pragma once
#include <Tools.hpp>
#include <cstdint>

namespace SoftRasterizer {
// Each pixel/sample owns its state: reproducible under any task scheduling.
class Sampler {
public:
  explicit Sampler(std::uint64_t seed = 1, std::uint64_t pixel = 0,
                   std::uint64_t sample = 0)
      : m_state(mix(seed ^ mix(pixel) ^ mix(sample + 0x632be59bd9b4e019ULL))) {}

  float next() {
    m_state += 0x9e3779b97f4a7c15ULL;
    return static_cast<float>(mix(m_state) >> 40) * (1.f / 16777216.f);
  }

  glm::vec3 sphere() {
    const float z = 1 - 2 * next(), phi = 2 * Pi * next();
    const float r = std::sqrt(std::max(0.f, 1 - z * z));
    return {r * std::cos(phi), r * std::sin(phi), z};
  }

  glm::vec3 cosineHemisphere(const glm::vec3 &n) {
    const float r = std::sqrt(next()), phi = 2 * Pi * next();
    return localToWorld({r * std::cos(phi), r * std::sin(phi),
                         std::sqrt(std::max(0.f, 1 - r * r))},
                        n);
  }

private:
  std::uint64_t m_state;

  //hash function from https://zimbry.blogspot.com/2011/09/better-bit-mixing-improving-on.html
  static std::uint64_t mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
  }
};
} // namespace SoftRasterizer
