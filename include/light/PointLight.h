#ifndef POINTLIGHT_H
#define POINTLIGHT_H

#include "util/Utilities.h"
#include "light/Light.h"

// Child of Light class
// Represents a point light source
class PointLight : public Light {
    public:
        Point3 position; // Position of the point light
        Radiance intensity; // Intensity of the point light

        double shadow_frac = 0.1; // Design choice for shadowing, not fully dark

        PointLight(const Point3& position, const Radiance& intensity)
            : position(position), intensity(intensity) {}

        Radiance intensity_at(const Point3& point, Vector3& direction) const {
            direction = unit_vector(position - point); // Direction from point to light
            Radiance intensity_n = intensity / dot(position - point, position - point);
            return intensity_n; // Falloff with distance squared
        }

        Radiance phong_shading(
            const Point3& p,
            const Vector3& normal,
            const Vector3& view_dir,
            const IntersectionRecord& rec,
            const Object& world,
            const Radiance& ambient_light
        ) const override {
                const Material* mat = rec.mat_ptr;
                Vector3 light_dir;
                Radiance light_intensity = intensity_at(p, light_dir);
                double shadow = shadow_factor(p, world);
                // BLINN-PHONG SHADING

                // Sample texture color (if any) for diffuse contribution
                Radiance texture_color = mat->diffusecolor; // Default to diffusecolor
                if (mat->has_texture) {
                    texture_color = mat->get_texture_color(rec.u, rec.v); // Use texture color
                }
                // Diffuse contribution
                double diff = std::max(0.0, dot(normal, light_dir));
                Radiance diffuse = shadow * mat->kd * texture_color * light_intensity * diff;
                // Specular contribution
                Vector3 halfway = unit_vector(light_dir + view_dir);
                double spec = pow(std::max(0.0, dot(normal, halfway)), mat->specularexponent);
                Radiance specular = shadow * mat->ks * mat->specularcolor * light_intensity * spec;
                return diffuse + specular;
        }



        double shadow_factor(const Point3& p, const Object& world) const {
            // Calculate the shadow factor for a point on the surface
            Vector3 light_dir = unit_vector(position - p);
            Ray shadow_ray(p + 0.001 * light_dir, light_dir);

            IntersectionRecord shadow_rec;
            double shadow = 1.0; // Fully illuminated by default
            int depth = 0;       // Initialize recursion depth

            double max_t = (position - p).length();
            while (world.intersect(shadow_ray, Interval(0.001, max_t), shadow_rec)) {
                Material* mat = shadow_rec.mat_ptr;

                if (mat->is_refractive) {
                    double transmittance = 1.0 / mat->refractiveindex;
                    shadow *= transmittance;

                    double eta = shadow_rec.front_face ? (1.0 / mat->refractiveindex) : mat->refractiveindex;
                    Vector3 refract_dir = refract(shadow_ray.direction(), shadow_rec.normal, eta);
                    shadow_ray = Ray(shadow_rec.p + 0.001 * refract_dir, refract_dir);
                    max_t = (position - shadow_ray.origin()).length();
                } else {
                    shadow *= shadow_frac; 
                    break;
                }

                depth++;
                if (depth >= 8) {
                    shadow *= shadow_frac;
                    break;
                }
            }

            return shadow;
        }

        Radiance brdf_shading(            
            const Point3& p,
            const Vector3& normal,
            const Vector3& view_dir,
            const FancyBRDF& brdf,
            const Object& world,
            const Vector3& coefficient
            ) const override  {
                // Calculate the shading contribution from the BRDF
                double shadow = shadow_factor(p, world);
                if (fabs(shadow - shadow_frac) < 1e-10){
                    return Radiance(0, 0, 0); // Skip shadowed points
                }

                Vector3 light_dir;
                Radiance attenuated_intensity = intensity_at(p, light_dir);
                Vector3 weight = brdf.Evaluate(light_dir, view_dir);

                return shadow * attenuated_intensity * weight * coefficient;

        }





};

#endif // POINT_LIGHT_H
