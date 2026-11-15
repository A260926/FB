#include <scene/Scene.hpp>

SoftRasterizer::Scene::Scene(std::string, const glm::vec3 &eye,
                             const glm::vec3 &center, const glm::vec3 &up,
                             const glm::vec3 &backgroundColor)
    : m_backgroundColor(backgroundColor) {
  if (!finite(backgroundColor) ||
      glm::any(glm::lessThan(backgroundColor, glm::vec3(0)))) {
    throw std::invalid_argument("invalid background");
  }
  m_camera.lookAt(eye, center, up);
}

bool SoftRasterizer::Scene::addGraphicObj(std::shared_ptr<Object> object,
                                          const std::string &objectName) {
  if (!object || m_loadedObjs.count(objectName) ||
      m_objLoaders.count(objectName)) {
    return false;
  }
  m_loadedObjs.emplace(objectName, ObjData{std::move(object), 0});
  m_geometryDirty = true;
  return true;
}

bool SoftRasterizer::Scene::addGraphicObj(const std::string &path,
                                          const std::string &meshName,
                                          const glm::vec3 &axis, float angle,
                                          const glm::vec3 &translation,
                                          const glm::vec3 &scale) {
  if (m_loadedObjs.count(meshName) || m_objLoaders.count(meshName)) {
    return false;
  }
  m_objLoaders.emplace(
      meshName, std::make_unique<ObjLoader>(
                    path, composeTransform(axis, angle, translation, scale)));
  return true;
}

bool SoftRasterizer::Scene::startLoadingMesh(const std::string &meshName) {
  if (m_loadedObjs.count(meshName)) {
    return true;
  }
  auto loaderIter = m_objLoaders.find(meshName);
  if (loaderIter == m_objLoaders.end()) {
    return false;
  }
  auto mesh = loaderIter->second->load();
  m_loadedObjs.emplace(meshName, ObjData{std::move(mesh), 0});
  m_geometryDirty = true;
  return true;
}

std::optional<std::shared_ptr<SoftRasterizer::Object>>
SoftRasterizer::Scene::getMeshObj(const std::string &meshName) const {
  auto meshIter = m_loadedObjs.find(meshName);
  if (meshIter == m_loadedObjs.end()) {
    return std::nullopt;
  }
  return meshIter->second.mesh;
}

bool SoftRasterizer::Scene::setModelMatrix(const std::string &meshName,
                                           const glm::vec3 &axis, float angle,
                                           const glm::vec3 &translation,
                                           const glm::vec3 &scale) {
  startLoadingMesh(meshName);
  auto mesh = getMeshObj(meshName);
  if (!mesh) {
    return false;
  }
  (*mesh)->updateModelMatrix(axis, angle, translation, scale);
  return true;
}

bool SoftRasterizer::Scene::addShader(const std::string &shaderName,
                                      const std::string &texturePath,
                                      SHADERS_TYPE type) {
  if (m_shaders.count(shaderName)) {
    return false;
  }
  auto shader = std::make_shared<Shader>();
  shader->setFragmentShader(type);
  if (!texturePath.empty()) {
    shader->texture = std::make_shared<TextureLoader>(texturePath);
  }
  m_shaders.emplace(shaderName, std::move(shader));
  return true;
}

bool SoftRasterizer::Scene::bindShader2Mesh(const std::string &meshName,
                                            const std::string &shaderName) {
  startLoadingMesh(meshName);
  auto mesh = getMeshObj(meshName);
  auto shaderIter = m_shaders.find(shaderName);
  if (!mesh || shaderIter == m_shaders.end()) {
    return false;
  }
  (*mesh)->bindShader2Mesh(shaderIter->second);
  return true;
}

void SoftRasterizer::Scene::addLight(const std::string &name,
                                     std::shared_ptr<light_struct> light) {
  if (!light || !finite(light->position) || !finite(light->intensity) ||
      glm::any(glm::lessThan(light->intensity, glm::vec3(0)))) {
    throw std::invalid_argument("invalid point light");
  }
  m_lights[name] = std::move(light);
}

/*Load pending meshes, then update geometry caches only when objects change.*/
void SoftRasterizer::Scene::prepare() {
  for (const auto &[meshName, loader] : m_objLoaders) {
    startLoadingMesh(meshName);
  }
  m_objLoaders.clear();
  for (const auto &[name, objData] : m_loadedObjs) {
    if (objData.preparedRevision != objData.mesh->revision()) {
      m_geometryDirty = true;
    }
  }
  if (m_geometryDirty) {
    m_primitives.clear();
    m_rasterTriangles.clear();
    for (auto &[name, objData] : m_loadedObjs) {
      objData.mesh->appendPrimitives(m_primitives);
      objData.mesh->appendRasterTriangles(m_rasterTriangles);
      objData.preparedRevision = objData.mesh->revision();
    }
    m_bvh.rebuild(m_primitives);
    m_geometryDirty = false;
  }
  updateLightDistribution();
}

SoftRasterizer::SurfaceSample
SoftRasterizer::Scene::sampleLight(Sampler &sampler) const {
  if (m_emissiveObjs.empty() || m_lightArea <= 0) {
    return {};
  }
  auto cdfIter = std::upper_bound(m_lightCdf.begin(), m_lightCdf.end(),
                                  sampler.next() * m_lightArea);
  auto lightIndex = std::min(std::size_t(cdfIter - m_lightCdf.begin()),
                             m_emissiveObjs.size() - 1);
  auto lightSample = m_emissiveObjs[lightIndex]->sample(sampler);
  lightSample.pdfArea = 1 / m_lightArea;
  return lightSample;
}

/*Refresh sampling weights because callers can edit material emission
 * directly.*/
void SoftRasterizer::Scene::updateLightDistribution() {
  for (const auto &[name, light] : m_lights) {
    if (!finite(light->position) || !finite(light->intensity) ||
        glm::any(glm::lessThan(light->intensity, glm::vec3(0)))) {
      throw std::invalid_argument("invalid point light");
    }
  }
  m_emissiveObjs.clear();
  m_lightCdf.clear();
  m_lightArea = 0;
  for (const auto &object : m_primitives) {
    const auto &material = *object->getMaterial();
    if (!finite(material.Kd) || !finite(material.Ks) ||
        !finite(material.emission) ||
        glm::any(glm::lessThan(material.emission, glm::vec3(0))) ||
        !std::isfinite(material.ior) || material.ior <= 0 ||
        !std::isfinite(material.specularExponent) ||
        material.specularExponent < 0) {
      throw std::invalid_argument("invalid material");
    }
    if (!material.hasEmission()) {
      continue;
    }
    const float area = object->getArea();
    if (area > 0) {
      m_lightArea += area;
      m_emissiveObjs.push_back(object);
      m_lightCdf.push_back(m_lightArea);
    }
  }
}

void SoftRasterizer::Scene::setViewMatrix(const glm::vec3 &eye,
                                          const glm::vec3 &center,
                                          const glm::vec3 &up) {
  m_camera.lookAt(eye, center, up);
}

void SoftRasterizer::Scene::setProjectionMatrix(const float fovy,
                                                const float zNear,
                                                const float zFar) {
  m_camera.projection(fovy, zNear, zFar);
}

/*Generating BVH Structure*/
void SoftRasterizer::Scene::buildBVHAccel() {
  m_geometryDirty = true;
  prepare();
}
