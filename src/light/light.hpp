#ifndef LIGHT_HPP_
#define LIGHT_HPP_

#include "ray.hpp"
#include "vec.hpp"
#include "objects.hpp"
#include "camera.hpp"
#include "common.hpp"
#include <vector>
#include <memory>
#include <optional>

enum class LightType : int {
    AMBIENT,
    POINT,
    DIRECTIONAL,
};

struct Light {
    LightType type;
    float intensity;
    // only valid for point/directional light
    std::optional<Vec3f> data{};
};

class Lights {
public:
  void AddAmbient(float intensity);
  void AddPoint(float intensity, float posx, float posy, float posz);
  void AddDir(float intensity, float dirx, float diry, float dirz);
  // call it having added all lights to normalize their intensities
  void Normalize();

  // Optionally route point-light contributions through a single sphere
  // lens (air -> sphere -> air connections). nullptr (the default) keeps
  // the legacy straight-line shadow behavior. The caller owns the sphere
  // and guarantees it outlives the Lights and stays present in the scene;
  // it may be replaced or reset to nullptr between Trace() calls.
  void SetPointRefractor(const Sphere* sphere) {
    point_refractor_ = sphere;
  }

  // diffuse and specular light contribution at a point on an object;
  // `normal` is the surface normal already computed at `at`
  Vec3u8 ColorAt(const std::vector<std::unique_ptr<Object>>& objects,
                 const Object &object,
                 const Vec3f &at,
                 const Camera &camera,
                 const Vec3f &normal) const;

private:
  std::vector<Light> lights_;
  const Sphere* point_refractor_{nullptr};

  static float ShadowFactor(const Light& light,
                            const std::vector<std::unique_ptr<Object>>& objects,
                            const Object& object,
                            const Vec3f& at,
                            const Vec3f& normal);
};

#endif // LIGHT_HPP_
