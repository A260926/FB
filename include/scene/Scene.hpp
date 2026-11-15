#pragma once
#include <bvh/BVHAcceleration.hpp>
#include <light/Light.hpp>
#include <loader/ObjLoader.hpp>
#include <map>
#include <optional>
#include <scene/Camera.hpp>

namespace SoftRasterizer {
class Scene {
public:
  explicit Scene(std::string sceneName = "Scene",
                 const glm::vec3 &eye = {0, 0, 3},
                 const glm::vec3 &center = {0, 0, 0},
                 const glm::vec3 &up = {0, 1, 0},
                 const glm::vec3 &backgroundColor = {0, 0, 0});

  /*Set camera view and projection; geometry remains in world space.*/
  void setViewMatrix(const glm::vec3 &eye, const glm::vec3 &center,
                     const glm::vec3 &up);
  void setProjectionMatrix(const float fovy, const float zNear,
                           const float zFar);

  const Camera &getCamera() const {
    return m_camera;
  }

  glm::vec3 backgroundColor() const {
    return m_backgroundColor;
  }

  template <class T>
  bool addGraphicObj(std::unique_ptr<T> object, const std::string &name) {
    return addGraphicObj(std::shared_ptr<Object>(std::move(object)), name);
  }

  bool addGraphicObj(std::shared_ptr<Object> object, const std::string &name);
  bool addGraphicObj(const std::string &path, const std::string &name,
                     const glm::vec3 &axis = {0, 1, 0}, float degrees = 0,
                     const glm::vec3 &translation = {0, 0, 0},
                     const glm::vec3 &scale = {1, 1, 1});
  /*Load once and register the mesh; prepare() clears the loaders.*/
  bool startLoadingMesh(const std::string &name);
  std::optional<std::shared_ptr<Object>>
  getMeshObj(const std::string &name) const;
  bool setModelMatrix(const std::string &name, const glm::vec3 &axis,
                      float degrees, const glm::vec3 &translation,
                      const glm::vec3 &scale);
  bool addShader(const std::string &name, const std::string &texture,
                 SHADERS_TYPE type);
  bool bindShader2Mesh(const std::string &object, const std::string &shader);
  void addLight(const std::string &name, std::shared_ptr<light_struct> light);
  /*Prepare geometry and lights before rendering. Do not mutate during draw().*/
  void prepare();

  void buildBVHAccel();

  Intersection intersect(const Ray &ray) const {
    return m_bvh.getIntersection(ray);
  }

  bool occluded(const Ray &ray) const {
    return m_bvh.occluded(ray);
  }

  SurfaceSample sampleLight(Sampler &rng) const;

  const std::vector<RasterTriangle> &triangles() const {
    return m_rasterTriangles;
  }

  const auto &pointLights() const {
    return m_lights;
  }

private:
  void updateLightDistribution();

private:
  /*Objects and resources loaded by name, as in the original scene interface.*/
  struct ObjData {
    std::shared_ptr<Object> mesh;
    std::uint64_t preparedRevision = 0;
  };

  std::map<std::string, ObjData> m_loadedObjs;
  std::map<std::string, std::unique_ptr<ObjLoader>> m_objLoaders;
  std::map<std::string, std::shared_ptr<Shader>> m_shaders;
  std::map<std::string, std::shared_ptr<light_struct>> m_lights;
  /*World-space acceleration and cached raster triangle stream.*/
  BVHAcceleration m_bvh;
  std::vector<std::shared_ptr<Object>> m_primitives, m_emissiveObjs;
  std::vector<float> m_lightCdf;
  std::vector<RasterTriangle> m_rasterTriangles;
  /*Camera and scene settings.*/
  bool m_geometryDirty = true;
  float m_lightArea = 0;
  Camera m_camera;
  glm::vec3 m_backgroundColor{0};
};
} // namespace SoftRasterizer
