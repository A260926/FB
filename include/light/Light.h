#ifndef LIGHT_H
#define LIGHT_H

#include "util/Utilities.h"
#include "object/Object.h"
#include "brdf/BRDF.h"

#include "Material.h"

class Light {
    public:

     virtual ~Light() = default;

     virtual Radiance phong_shading(
      const Point3& p,                // Point of intersection
      const Vector3& normal,          // Normal at the point of intersection
      const Vector3& view_dir,        // Direction of the view
      const IntersectionRecord& rec,  // Intersection record
      const Object& world,            // World object
      const Radiance& ambient_light   // Ambient light
     ) const = 0;

     virtual Radiance brdf_shading(
      const Point3& p,                // Point of intersection
      const Vector3& normal,          // Normal at the point of intersection
      const Vector3& view_dir,        // Direction of the view
      const FancyBRDF& brdf,          // BRDF of the material
      const Object& world,            // World object
      const Vector3& coefficient      // Coefficient for the BRDF contribution
     ) const = 0;

};

inline Radiance calculate_ambient(const Material& mat, Radiance ambient_light, double u, double v) {
    // Calculate ambient light contribution (texture-aware)
    Radiance base_color = mat.has_texture ? mat.get_texture_color(u, v) : mat.diffusecolor;
    Radiance diffuse = mat.kd * base_color * ambient_light;
    Radiance specular = mat.ks * mat.specularcolor * ambient_light;
    Radiance ambient = mat.ka * diffuse + specular;
    return ambient;
}

#endif // LIGHT_H