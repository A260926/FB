#pragma once
#include <hpc/Sampler.hpp>
#include <loader/TextureLoader.hpp>
#include <memory>

namespace SoftRasterizer {
enum class MaterialType {
  Diffuse,
  Mirror,
  Dielectric,
  // Source-compatible aliases. Diffuse is Lambertian in the path tracer.
  DIFFUSE_AND_GLOSSY = Diffuse,
  REFLECTION = Mirror,
  REFLECTION_AND_REFRACTION = Dielectric
};

struct BsdfSample {
  glm::vec3 direction{0};
  // Ordinary finite BRDF density; zero when the sample is a delta event.
  glm::vec3 fr{0};
  // Integrated delta-event response, not an ordinary continuous BRDF.
  glm::vec3 deltaCoefficient{0};
  // PDF of a continuous sampled direction with respect to solid angle
  // (sr^-1). Its value follows the sampler; cosine sampling uses cos(theta)/pi
  // and is not a uniform hemisphere distribution.
  float solidAnglePdf = 0;
  // Dimensionless probability mass of the selected delta branch. It is 1 for
  // a deterministic mirror/TIR event, and F or 1-F for dielectric branches.
  float deltaEventProbability = 0;
  bool delta = false, transmitted = false;
};

struct Material {
  MaterialType type = MaterialType::DIFFUSE_AND_GLOSSY;
  glm::vec3 Kd{0.7f}, emission{0}, Ks{0};
  float ior = 1.5f, specularExponent = 32;
  std::shared_ptr<TextureLoader> texture;

  explicit Material(MaterialType t = MaterialType::DIFFUSE_AND_GLOSSY)
      : type(t) {}

  glm::vec3 albedo(const glm::vec2 &uv) const {
    return glm::clamp(
        Kd * (texture ? texture->getTextureColor(uv) : glm::vec3(1)),
        glm::vec3(0), glm::vec3(1));
  }

  bool hasEmission() const {
    return maxComponent(emission) > 0;
  }

  bool isDelta() const {
    return type != MaterialType::DIFFUSE_AND_GLOSSY;
  }

  BsdfSample sample(const glm::vec3 &incoming, const glm::vec3 &normal,
                    bool frontFace, const glm::vec2 &uv, Sampler &rng) const;
};
} // namespace SoftRasterizer
