#include <hpc/Parallel.hpp>
#include <render/PathTracing.hpp>

namespace Parallel = SoftRasterizer::Parallel;

SoftRasterizer::PathTracing::PathTracing(const std::size_t width,
                                         const std::size_t height,
                                         const std::size_t spp)
    : RenderingPipeline(width, height) {
  setSPP(spp);
}

// Samples Per Pixel
void SoftRasterizer::PathTracing::setSPP(const std::size_t spp) {
  if (!spp) {
    throw std::invalid_argument("SPP must be positive");
  }
  m_settings.samplesPerPixel = spp;
}

void SoftRasterizer::PathTracing::configure(
    const PathTracingSettings &settings) {
  if (!settings.samplesPerPixel || !settings.maxDepth ||
      settings.maxDepth > 1024 ||
      !std::isfinite(settings.survivalProbability) ||
      settings.survivalProbability <= 0 || settings.survivalProbability > 1) {
    throw std::invalid_argument("invalid path tracing settings");
  }
  m_settings = settings;
}

/*Calculate the direct contribution from area lights and point lights.*/
void SoftRasterizer::PathTracing::pathTracingDirectLight(
    const Scene &scene, const Intersection &shadeObjIntersection,
    const glm::vec3 &throughput, Sampler &sampler, glm::vec3 &radiance) const {
  const auto &material = *shadeObjIntersection.material;

  const auto albedo =
      material.albedo(shadeObjIntersection.uv) * shadeObjIntersection.color;
  pathTracingAreaLight(scene, shadeObjIntersection, albedo, throughput, sampler,
                       radiance);
  pathTracingPointLights(scene, shadeObjIntersection, albedo, throughput,
                         radiance);
}

/*Sample one area-light point, test visibility and apply its area PDF.*/
void SoftRasterizer::PathTracing::pathTracingAreaLight(
    const Scene &scene, const Intersection &shadeObjIntersection,
    const glm::vec3 &albedo, const glm::vec3 &throughput, Sampler &sampler,
    glm::vec3 &radiance) const {
  auto lightSample = scene.sampleLight(sampler);
  if (!(lightSample.pdfArea > 0)) {
    return;
  }
  auto lightDirection = lightSample.position - shadeObjIntersection.coords;
  float distanceSquared = glm::dot(lightDirection, lightDirection);
  if (!(distanceSquared > 1e-12f)) {
    return;
  }
  auto wi = lightDirection / std::sqrt(distanceSquared);

  const auto geometricNormal = shadeObjIntersection.frontFace
                                   ? shadeObjIntersection.geometricNormal
                                   : -shadeObjIntersection.geometricNormal;
  const float objectCosine = glm::dot(geometricNormal, wi);
  if (objectCosine <= 0) {
    return;
  }

  float lightCosine = std::max(0.f, glm::dot(lightSample.normal, -wi));
  if (lightCosine <= 0) {
    return;
  }
  auto start = offsetOrigin(shadeObjIntersection.coords,
                            shadeObjIntersection.geometricNormal, wi);
  auto end = offsetOrigin(lightSample.position, lightSample.normal, -wi);
  if (!isLightVisible(scene, start, end)) {
    return;
  }
  const auto fr = albedo / Pi;
  const auto Li = lightSample.material->emission;
  radiance +=
      throughput * Li * fr *
      (objectCosine * lightCosine / (distanceSquared * lightSample.pdfArea));
}

/*Visit point lights; no surface sampling or area PDF is needed.*/
void SoftRasterizer::PathTracing::pathTracingPointLights(
    const Scene &scene, const Intersection &shadeObjIntersection,
    const glm::vec3 &albedo, const glm::vec3 &throughput,
    glm::vec3 &radiance) const {
  for (const auto &[name, pointLight] : scene.pointLights()) {
    auto lightDirection = pointLight->position - shadeObjIntersection.coords;
    float distanceSquared = glm::dot(lightDirection, lightDirection);
    if (distanceSquared <= 1e-12f) {
      continue;
    }
    auto wi = lightDirection / std::sqrt(distanceSquared);
    const auto geometricNormal = shadeObjIntersection.frontFace
                                     ? shadeObjIntersection.geometricNormal
                                     : -shadeObjIntersection.geometricNormal;
    const float objectCosine = glm::dot(geometricNormal, wi);
    if (objectCosine <= 0) {
      continue;
    }
    auto start = offsetOrigin(shadeObjIntersection.coords,
                              shadeObjIntersection.geometricNormal, wi);
    if (isLightVisible(scene, start, pointLight->position)) {
      const auto fr = albedo / Pi;
      const auto incidentIrradiance =
          pointLight->intensity * (objectCosine / distanceSquared);
      radiance += throughput * fr * incidentIrradiance;
    }
  }
}

bool SoftRasterizer::PathTracing::isLightVisible(const Scene &scene,
                                                 const glm::vec3 &start,
                                                 const glm::vec3 &end) {
  auto shadow = end - start;
  float shadowDistance = glm::length(shadow);
  // normalize gives a unit direction; tMax still needs the segment length.
  // Objects beyond the light must not count as occluders.
  return shadowDistance > 0 &&
         !scene.occluded(Ray(start, glm::normalize(shadow), 0, shadowDistance));
}

glm::vec3
SoftRasterizer::PathTracing::pathTracingShading(const Scene &scene, Ray ray,
                                                Sampler &sampler) const {
  glm::vec3 radiance(0), throughput(1);
  bool previousDelta = true;
  /*Trace the path in world space; camera projection is only used for primary
   * rays.*/
  for (unsigned currentDepth = 0; currentDepth < m_settings.maxDepth;
       ++currentDepth) {
    auto shadeObjIntersection = scene.intersect(ray);
    if (!shadeObjIntersection.intersected) {
      radiance += throughput * scene.backgroundColor();
      break;
    }
    /*Emission after a camera or delta event. Diffuse connections use NEE.*/
    const auto &material = *shadeObjIntersection.material;
    const bool deltaMaterial = material.isDelta();
    if (previousDelta && shadeObjIntersection.frontFace) {
      radiance += throughput * material.emission;
    }
    /*Calculate direct light.*/
    if (!deltaMaterial) {
      pathTracingDirectLight(scene, shadeObjIntersection, throughput, sampler,
                             radiance);
    }
    if (currentDepth + 1 == m_settings.maxDepth) {
      break;
    }
    // Delta interfaces use the geometric boundary normal to keep medium
    // transitions consistent.
    /*Sample the indirect ray and update its throughput.*/
    auto boundaryNormal = shadeObjIntersection.frontFace
                              ? shadeObjIntersection.geometricNormal
                              : -shadeObjIntersection.geometricNormal;
    auto samplingNormal =
        deltaMaterial ? boundaryNormal : shadeObjIntersection.normal;
    auto bsdf = material.sample(ray.direction, samplingNormal,
                                shadeObjIntersection.frontFace,
                                shadeObjIntersection.uv, sampler);
    if (!finite(bsdf.direction)) {
      break;
    }
    // Prevent interpolated shading normals from sending reflective paths
    // through the surface.
    float hemisphere = glm::dot(bsdf.direction, boundaryNormal);
    if ((!bsdf.transmitted && hemisphere <= 0) ||
        (bsdf.transmitted && hemisphere >= 0)) {
      break;
    }
    if (bsdf.delta) {
      // A delta event has probability mass rather than a solid-angle PDF.
      // Its coefficient already contains the integrated directional response.
      if (!std::isfinite(bsdf.deltaEventProbability) ||
          bsdf.deltaEventProbability <= 0) {
        break;
      }
      throughput *= bsdf.deltaCoefficient / bsdf.deltaEventProbability *
                    shadeObjIntersection.color;
    } else {
      // The next hit supplies Li; this bounce contributes
      // fr * cos(theta) / solidAnglePdf.
      if (!std::isfinite(bsdf.solidAnglePdf) || bsdf.solidAnglePdf <= 0) {
        break;
      }
      const float cosine =
          std::max(0.f, glm::dot(samplingNormal, bsdf.direction));
      throughput *= bsdf.fr * cosine / bsdf.solidAnglePdf *
                    shadeObjIntersection.color;
    }
    if (!finite(throughput) || maxComponent(throughput) <= 0) {
      break;
    }
    previousDelta = bsdf.delta;
    /*Russian Roulette: compensate surviving paths by their probability.*/
    if (currentDepth + 1 >= m_settings.rrDepth) {
      if (sampler.next() >= m_settings.survivalProbability) {
        break;
      }
      throughput /= m_settings.survivalProbability;
    }
    ray =
        Ray(offsetOrigin(shadeObjIntersection.coords,
                         shadeObjIntersection.geometricNormal, bsdf.direction),
            bsdf.direction);
  }
  return radiance;
}

void SoftRasterizer::PathTracing::draw(Primitive type) {
  if (type != Primitive::TRIANGLES) {
    throw std::invalid_argument("PT does not support wireframes");
  }
  if (m_scenes.size() > 1) {
    throw std::invalid_argument("PT renders one scene at a time");
  }
  clear();
  if (m_scenes.empty()) {
    return;
  }
  /*Prepare the geometry and scene BVH before emitting rays.*/
  auto &SceneObj = *m_scenes.front();
  SceneObj.prepare();
  /*Each pixel owns its sample streams and writes its final average once.*/
  Parallel::parallelFor(std::size_t(0), m_height, [&](std::size_t ry) {
    for (std::size_t rx = 0; rx < m_width; ++rx) {
      glm::dvec3 partialColor(0);
      const auto pixelIndex = ry * m_width + rx;
      for (std::size_t sampleIndex = 0;
           sampleIndex < m_settings.samplesPerPixel; ++sampleIndex) {
        Sampler sampler(m_settings.seed, pixelIndex, sampleIndex);
        float jitterX = sampler.next(), jitterY = sampler.next();
        partialColor += glm::dvec3(pathTracingShading(
            SceneObj,
            SceneObj.getCamera().generateRay(
                float(rx) + jitterX, float(ry) + jitterY, m_width, m_height),
            sampler));
      }
      m_color[pixelIndex] =
          glm::vec3(partialColor / double(m_settings.samplesPerPixel));
    }
  });
}
