#include <object/Material.hpp>

SoftRasterizer::BsdfSample
SoftRasterizer::Material::sample(const glm::vec3 &incoming,
                                 const glm::vec3 &normal, bool frontFace,
                                 const glm::vec2 &uv, Sampler &rng) const {
  const auto tint = albedo(uv);
  BsdfSample bsdf;
  if (type == MaterialType::DIFFUSE_AND_GLOSSY) {
    bsdf.direction = rng.cosineHemisphere(normal);
    bsdf.fr = tint / Pi;
    const float cosine = std::max(0.f, glm::dot(normal, bsdf.direction));
    bsdf.solidAnglePdf = cosine / Pi;
    return bsdf;
  }
  bsdf.delta = true;
  if (type == MaterialType::REFLECTION) {
    bsdf.direction = unit(glm::reflect(incoming, normal));
    bsdf.deltaCoefficient = tint;
    bsdf.deltaEventProbability = 1;
    return bsdf;
  }
  if (!std::isfinite(ior) || ior <= 0) {
    throw std::invalid_argument("invalid dielectric IOR");
  }
  const float incidentIOR = frontFace ? 1.f : ior,
              transmittedIOR = frontFace ? ior : 1.f,
              eta = incidentIOR / transmittedIOR;
  const float fresnel = dielectricFresnel(-glm::dot(incoming, normal),
                                          incidentIOR, transmittedIOR);
  if (rng.next() < fresnel) {
    bsdf.direction = unit(glm::reflect(incoming, normal));
    bsdf.deltaCoefficient = tint * fresnel;
    bsdf.deltaEventProbability = fresnel;
    return bsdf;
  }
  auto direction = glm::refract(incoming, normal, eta);
  if (glm::dot(direction, direction) < 1e-12f) {
    // A failed refraction sample has no valid event probability.
    return {};
  }
  // Radiance transport across a dielectric interface includes eta^2.
  bsdf.direction = unit(direction);
  bsdf.deltaCoefficient = tint * (1 - fresnel) * (eta * eta);
  bsdf.deltaEventProbability = 1 - fresnel;
  bsdf.transmitted = true;
  return bsdf;
}
