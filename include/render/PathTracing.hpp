#pragma once
#include <base/Render.hpp>

namespace SoftRasterizer {

/*Path tracing sample and termination settings.*/
struct PathTracingSettings {
  std::size_t samplesPerPixel = 16;
  unsigned maxDepth = 32, rrDepth = 4;
  float survivalProbability = 0.9f;
  std::uint64_t seed = 1;
};

class PathTracing : public RenderingPipeline {
public:
  explicit PathTracing(const std::size_t width = 800,
                       const std::size_t height = 600,
                       const std::size_t spp = 16);

  // Samples Per Pixel
  void setSPP(const std::size_t spp);
  void configure(const PathTracingSettings &settings);
  void draw(Primitive type = Primitive::TRIANGLES) override;

private:
  /*Iterative path integration, including emission and indirect scattering.*/
  glm::vec3 pathTracingShading(const Scene &scene, Ray ray,
                               Sampler &sampler) const;

  /*Direct-light entry: sample an area light, then visit point lights.*/
  void pathTracingDirectLight(const Scene &scene,
                              const Intersection &shadeObjIntersection,
                              const glm::vec3 &throughput, Sampler &sampler,
                              glm::vec3 &radiance) const;

  /*Sample one area-light point and accumulate its PDF-weighted contribution.*/
  void pathTracingAreaLight(const Scene &scene,
                            const Intersection &shadeObjIntersection,
                            const glm::vec3 &albedo,
                            const glm::vec3 &throughput, Sampler &sampler,
                            glm::vec3 &radiance) const;

  /*Accumulate direct illumination from all point lights.*/
  void pathTracingPointLights(const Scene &scene,
                              const Intersection &shadeObjIntersection,
                              const glm::vec3 &albedo,
                              const glm::vec3 &throughput,
                              glm::vec3 &radiance) const;

  /*Test the finite segment between already-offset endpoints.*/
  static bool isLightVisible(const Scene &scene, const glm::vec3 &start,
                             const glm::vec3 &end);

private:
  PathTracingSettings m_settings;
};
} // namespace SoftRasterizer
