#pragma once

#include <string>
#include <vector>
#include "RGBColor.h"
#include "VECTOR2D.H"
#include "Vector3D.h"
#include "Triangle.h"
#include "Light.h"
#include "Camera.h"

class Material {
protected:
  float k_shininess;
public:
  Material();
  Material(const float shininess);
  ~Material();

  static const RGBColor shade(const std::vector<Light*>& lights, const Camera& camera, const Fragment& fragment);
  // Direct (per-light) lighting with each light modulated by its own shadow
  // occlusion factor. `shadow_factors[i]` is the [0, 1] occlusion of lights[i]:
  // 0 means fully lit, 1 means fully occluded. Ambient is added once and is
  // never affected by shadows. The unclamped sum is clamped to [0, 1] right
  // before it is multiplied by the vertex color.
  static const RGBColor shade(const std::vector<Light*>& lights, const std::vector<double>& shadow_factors, const Camera& camera, const Fragment& fragment);
  virtual const RGBColor getDiffuseColor(const Vector2D& text_coords) const = 0;
  virtual const RGBColor getSpecularColor(const Vector2D& text_coords) const = 0;
  virtual const Vector3D getNormal(const Triangle3D& triangle_world, const Vector2D& text_coords) const = 0;

protected:
  static const RGBColor phongEquation(const std::vector<Light*>& lights, const Vector3D& N, const Vector3D& V, const Point3D vertex_position,
    const RGBColor& vertex_color, const RGBColor& diffuse_color, const RGBColor& specular_color, const float shininess,
    const std::vector<double>* shadow_factors = nullptr);

  static const RGBColor blinnPhongEquation(const std::vector<Light*>& lights, const Vector3D& N, const Vector3D& V, const Point3D vertex_position,
    const RGBColor& vertex_color, const RGBColor& diffuse_color, const RGBColor& specular_color, const float shininess,
    const std::vector<double>* shadow_factors = nullptr);
};

class FlatMaterial : public Material {
public:
  RGBColor k_d, k_s;

  FlatMaterial();
  FlatMaterial(const RGBColor& d, const RGBColor& s, float shininess);
  ~FlatMaterial();

  virtual const RGBColor getDiffuseColor(const Vector2D& text_coords = Vector2D(0,0)) const override;
  virtual const RGBColor getSpecularColor(const Vector2D& text_coords = Vector2D(0,0)) const override;
  virtual const Vector3D getNormal(const Triangle3D& triangle_world, const Vector2D& text_coords) const override;

};

class TexturedMaterial : public Material {
private:
  std::vector<RGBColor> m_texture_diffuse,
    m_texture_specular,
    m_texture_normal;
  
  int m_texture_diffuse_width,
    m_texture_diffuse_height,
    m_texture_specular_width,
    m_texture_specular_height,
    m_texture_normal_width,
    m_texture_normal_height;

public:
  TexturedMaterial();
  TexturedMaterial(const std::string texture_diffuse_file, const std::string texture_specular_file, const std::string texture_normal_file, float shininess);
  ~TexturedMaterial();

  virtual const RGBColor getDiffuseColor(const Vector2D& text_coords) const override;
  virtual const RGBColor getSpecularColor(const Vector2D& text_coords) const override;
  virtual const Vector3D getNormal(const Triangle3D& triangle_world, const Vector2D& text_coords) const override;

private:
  const RGBColor getTextureColor(const std::vector<RGBColor>& texture, int texture_width, int texture_height, const Vector2D& text_coords) const;
  std::vector<RGBColor> loadTexture(int& texture_width, int& texture_height, const std::string import_path) const;


  const Vector3D TangentToWorld(const Vector3D& v, const Vector3D& tangent, const Vector3D& bitangent, const Vector3D& normal) const;
  void calculateTangentSpace(Vector3D& tangent, Vector3D& bitangent, const Triangle3D& triangle_world) const;

};



