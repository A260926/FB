#ifndef RECTANGULARLIGHT_H
#define RECTANGULARLIGHT_H

#include "light/Light.h"

class RectangularLight : public Light {
public:
    Point3 position;  // Center of the area light
    Vector3 u, v;        // Basis vectors defining the area light plane
    double width, height;  // Dimensions of the area light
    Radiance intensity;  // Light intensity

    int num_samples;  // Number of samples for soft shadows
    double shadow_frac = 0.0;  // Design choice, shadow are compeletly dark

    RectangularLight(
        const Point3& position,
        const Vector3& u,
        const Vector3& v,
        double width,
        double height,
        const Radiance& intensity,
        int num_samples = 16
    ) : position(position), u(u), v(v), width(width), height(height), intensity(intensity), num_samples(num_samples) {}

    Radiance intensity_at(const Point3& point, const Point3& sample_point, Vector3& direction) const {
        // Compute the direction and intensity of the light at a given point
        direction = unit_vector(sample_point - point);
        return intensity / dot(sample_point - point, sample_point - point);
    }

Radiance phong_shading(
    const Point3& p,
    const Vector3& normal,
    const Vector3& view_dir,
    const IntersectionRecord& rec,
    const Object& world,
    const Radiance& ambient_light
) const override {
    // Calculate the shading contribution from the area light
    const Material* mat = rec.mat_ptr;
    Radiance total_result(0, 0, 0); // Accumulate results from all samples

    for (int i = 0; i < num_samples; ++i) {
        // Sample a point on the area light's surface
        Point3 sample_point = random_sample_on_surface();
        Vector3 light_dir = unit_vector(sample_point - p);

        // Shadow factor (0 for fully shadowed, 1 for fully lit)
        double shadow = shadow_factor_from_sample(p, sample_point, world);
        if (shadow == 0.0) continue; // Skip if fully shadowed

        // Light intensity from the sampled point
        Radiance light_intensity = intensity_at(p,sample_point,light_dir); // Falloff with distance^2


        //BLINN-PHONG SHADING
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

        // Combine diffuse and specular and accumulate
        total_result += diffuse + specular;
    }

    // Average the accumulated contributions from all samples
    return total_result / num_samples;
}


    Radiance brdf_shading(            
        const Point3& p,
        const Vector3& normal,
        const Vector3& view_dir,
        const FancyBRDF& brdf,
        const Object& world,
        const Vector3& coefficient
    ) const override {
        // Calculate the shading contribution from the area light
        Radiance total_result(0, 0, 0); // Accumulator for all sample contributions
        double area = width * height;
        Vector3 light_normal = unit_vector(cross(u, v));

        for (int i = 0; i < num_samples; ++i) {
            // Sample a point on the area light's surface
            Point3 sample_point = random_sample_on_surface();
            Vector3 light_dir;

            // Compute shadow factor for the sampled light point
            double shadow = shadow_factor_from_sample(p, sample_point, world);
            if (shadow == 0.0) {
                continue; // Skip fully shadowed samples
            }

            // Compute light intensity from the sampled point
            Radiance light_intensity = intensity_at(p, sample_point, light_dir); // Falloff with distance^2

            // Evaluate the BRDF for the given light and view directions
            Vector3 brdf_weight = brdf.Evaluate(light_dir, view_dir);

            // Uniform area sampling: 1 / area. intensity_at() already includes the
            // distance falloff, so only the light-facing cosine and area remain here.
            double cos_light = std::max(0.0, dot(light_normal, -light_dir));
            if (cos_light < 1e-8) {
                continue; // Light sample faces away from the shaded point
            }
            double area_weight = area * cos_light;

            // Accumulate the weighted contribution from this sample
            total_result += shadow * light_intensity * brdf_weight * coefficient * area_weight;
        }

        // Average the accumulated contributions
        return total_result / num_samples;
    }

private:
    Point3 random_sample_on_surface() const {
        // Uniform sampling on a rectangle
        double s = random_double(-0.5, 0.5);
        double t = random_double(-0.5, 0.5);
        return position + (s * width * u) + (t * height * v);
    }

    double shadow_factor_from_sample(const Point3& p, const Point3& sample_point, const Object& world) const {
        // Vector3 light_dir = unit_vector(sample_point - p);
        // Ray shadow_ray(p + 0.001 * light_dir, light_dir);

        // IntersectionRecord shadow_rec;
        // bool is_shadowed = world.intersect(shadow_ray, Interval(0.001, infinity), shadow_rec);

        // return is_shadowed ? 0.0 : 1.0;  // Fully blocked or fully illuminated

        Vector3 light_dir = unit_vector(sample_point - p);
        Ray shadow_ray(p + 0.001 * light_dir, light_dir);

        IntersectionRecord shadow_rec;
        double shadow = 1.0; // Fully illuminated by default
        int depth = 0;       // Initialize recursion depth

        double max_t = (sample_point - p).length();
        while (world.intersect(shadow_ray, Interval(0.001, max_t), shadow_rec)) {
            Material* mat = shadow_rec.mat_ptr;

            if (mat->is_refractive) {
                double transmittance = 1.0 / mat->refractiveindex;
                shadow *= transmittance;

                double eta = shadow_rec.front_face ? (1.0 / mat->refractiveindex) : mat->refractiveindex;
                Vector3 refract_dir = refract(shadow_ray.direction(), shadow_rec.normal, eta);
                shadow_ray = Ray(shadow_rec.p + 0.001 * refract_dir, refract_dir);
                max_t = (sample_point - shadow_ray.origin()).length();
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
};

#endif // AREA_LIGHT_H
