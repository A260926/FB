#include "Camera.h"

#include "brdf/FancyBRDF.h"
#include "light/PointLight.h"

void Camera::initialize() {
    // Calculate the aspect ratio
    aspect_ratio = double(image_width) / double(image_height);
    center = lookfrom; // Camera center is the lookfrom point

    // Determine viewport dimensions.
    auto focal_length = (lookfrom - lookat).length();
    auto theta = degrees_to_radians(vfov);
    auto h = std::tan(theta/2);
    auto viewport_height = 2 * h * focal_length;
    auto viewport_width = viewport_height * (double(image_width)/image_height);

    // std::cout << "image_width: " << image_width  << "\n" 
    //             << "image_height: " << image_height << "\n"
    //             << "aspect_ratio: " << aspect_ratio << "\n"
    //             << "viewport_width: " << viewport_width  << "\n" 
    //             << "viewport_height: " << viewport_height <<std::endl;

    // Calculate the u,v,w unit basis vectors for the camera coordinate frame.
    w = unit_vector(lookfrom - lookat);
    u = unit_vector(cross(vup, w));
    v = cross(w, u);

    defocus_disk_u = u;
    defocus_disk_v = v;

    // Calculate the vectors across the horizontal and down the vertical viewport edges.
    Vector3 viewport_u = viewport_width * u;    // Vector across viewport horizontal edge
    Vector3 viewport_v = viewport_height * -v;  // Vector down viewport vertical edge

    // Calculate the horizontal and vertical delta vectors from pixel to pixel.
    pixel_delta_u = viewport_u / image_width;
    pixel_delta_v = viewport_v / image_height;

    // Calculate the location of the upper left pixel.
    auto viewport_upper_left = center - (focal_length * w) - viewport_u/2 - viewport_v/2;
    first_pixel = viewport_upper_left + 0.5 * (pixel_delta_u + pixel_delta_v);
}


Vector3 Camera::jittered_sample(int sample_index) const {
    int sqrt_samples = static_cast<int>(std::sqrt(samples_per_pixel));
    int x = sample_index % sqrt_samples;
    int y = sample_index / sqrt_samples;

    double dx = random_double() / sqrt_samples;
    double dy = random_double() / sqrt_samples;

    return Vector3((x + dx) / sqrt_samples - 0.5, 
                (y + dy) / sqrt_samples - 0.5, 0);
}

std::vector<Vector3> Camera::generate_poisson_disk_samples(int sample_count, double min_distance) {
    std::vector<Vector3> samples;
    std::mt19937 gen(std::random_device{}());
    std::uniform_real_distribution<> dist(0.0, 1.0);

    for (int i = 0; i < sample_count; ++i) {
        int attempts = 0;
        while (attempts < 30) {
            double x = dist(gen) * 2.0 - 1.0; // [-1, 1]
            double y = dist(gen) * 2.0 - 1.0; // [-1, 1]
            Vector3 candidate(x, y, 0.0);

            bool valid = true;
            for (const auto& sample : samples) {
                if ((candidate - sample).length() < min_distance) {
                    valid = false;
                    break;
                }
            }

            if (valid) {
                samples.push_back(candidate);
                break;
            }
            attempts++;
        }
    }
    return samples;
}


// ANTI-ALIASING + DEFOUCS BLUR
Ray Camera::get_sampled_ray(int i, int j, Vector3 offset) const {
    Vector3 lens_sample = random_point_in_aperture();

    Vector3 offset_origin = center + lens_sample;

    Vector3 pixel_sample = first_pixel
                    + ((i + offset.x()) * pixel_delta_u)
                    + ((j + offset.y()) * pixel_delta_v);

    Vector3 focal_target = center + focus_dist * unit_vector(pixel_sample - center);
    // Vector3 ray_direction = focal_target - offset_origin;
    Vector3 ray_direction = focal_target - offset_origin;

    return Ray(offset_origin, ray_direction);
}


Radiance Camera::trace_binary(const Ray& r, const Object& world) const {
    // Check if the ray intersects any object in the scene
    // if hit, return red color, else return black
    IntersectionRecord rec;
    if (world.intersect(r, Interval(0, infinity), rec)) {
        return Radiance(1,0,0);
    }
    return Radiance(0,0,0);
}


Ray Camera::pixel_center_ray(int i, int j) const {
    auto pixel_center = first_pixel + (i * pixel_delta_u) + (j * pixel_delta_v);
    auto ray_direction = pixel_center - center;
    return Ray(center, ray_direction);
}

// Ambient plus direct Blinn-Phong lighting at a surface point. Shared by the
// PHONG renderer and the gaussian-medium renderer so the surface shader is
// never duplicated.
Radiance Camera::phong_surface_term(const Ray& r, const IntersectionRecord& rec,
                                    const Object& world, const Light& lights) const {
    Vector3 view_dir = unit_vector(-r.direction());
    Radiance local_shading = lights.phong_shading(rec.p, rec.normal, view_dir,
                                                  rec, world, ambient_light);
    Radiance ambient = calculate_ambient(*rec.mat_ptr, ambient_light, rec.u, rec.v);
    return ambient + local_shading;
}

Radiance Camera::trace_phong(const Ray& r, const Object& world, const Light& lights, int depth) const {
    IntersectionRecord rec;
    Radiance result;

    // If the ray hits nothing, return the background color.
    if (!world.intersect(r, Interval(0.001, infinity), rec)) {
        return background;
    }

    // Compute Blinn-Phong shading at the intersection point
    Radiance surface_term = phong_surface_term(r, rec, world, lights);

    // Stop recursion if max depth is reached
    if (depth <= 0) {
        return surface_term;
    }

    // Initialize reflection and refraction contributions
    Radiance reflected_color(0, 0, 0);
    Radiance refracted_color(0, 0, 0);

    // Reflective only case
    if (rec.mat_ptr->is_reflective && !rec.mat_ptr->is_refractive) {
        Vector3 reflect_dir = reflect_pm(r.direction(), rec.normal);
        Ray reflect_ray(rec.p + 0.001 * reflect_dir, reflect_dir);
        reflected_color = trace_phong(reflect_ray, world, lights, depth - 1);
        result = (1 - rec.mat_ptr->reflectivity) * surface_term +
                rec.mat_ptr->reflectivity * reflected_color;
        return result;
    }

    // Combined refractive and reflective case
    if (rec.mat_ptr->is_refractive) {
        double eta = rec.front_face ? (1.0 / rec.mat_ptr->refractiveindex) : rec.mat_ptr->refractiveindex;

        // Calculate cos(theta) for the incident ray
        double cos_theta_i = std::fabs(dot(-unit_vector(r.direction()), rec.normal));
        double sin_theta_t = eta * std::sqrt(1 - cos_theta_i * cos_theta_i);

        if (sin_theta_t > 1.0) {
            // Total Internal Reflection: Treat as reflective
            Vector3 reflect_dir = reflect_pm(r.direction(), rec.normal);
            Ray reflect_ray(rec.p + 0.001 * reflect_dir, reflect_dir);
            refracted_color = trace_phong(reflect_ray, world, lights, depth - 1);
        } else {
            // Compute refraction direction using Snell's law
            Vector3 refract_dir = refract(unit_vector(r.direction()), rec.normal, eta);
            Ray refract_ray(rec.p + 0.001 * refract_dir, refract_dir);
            refracted_color = trace_phong(refract_ray, world, lights, depth - 1);
        }

        // Reflective contribution
        if (rec.mat_ptr->is_reflective) {
            Vector3 reflect_dir = reflect_pm(r.direction(), rec.normal);
            Ray reflect_ray(rec.p + 0.001 * reflect_dir, reflect_dir);
            reflected_color = trace_phong(reflect_ray, world, lights, depth - 1);
        }

        // Combine contributions
        // Use Schlick approximation for Fresnel effect
        double R0 = pow((1.0 - rec.mat_ptr->refractiveindex) / (1.0 + rec.mat_ptr->refractiveindex), 2);
        result = schlick_blend(surface_term, reflected_color, refracted_color, eta, cos_theta_i, R0);
        // result = ambient + local_shading + reflected_color + refracted_color;
        return result;
    }

    // Local shading only case
    result = surface_term;
    return result;
}


Radiance Camera::trace_path_phong(const Ray& r, const Object& world, const Light& lights, int depth) const {
    if (depth <= 0) {
        return Radiance(0, 0, 0);
    }
    IntersectionRecord rec;
    if (!world.intersect(r, Interval(0.001, infinity), rec)) {
        return background;
    }

    Material* mat = rec.mat_ptr;
    Radiance emitted = Radiance(0,0,0);
    Vector3 view_dir = unit_vector(-r.direction());
    Radiance surface_color = lights.phong_shading(rec.p, rec.normal, view_dir, rec, world, ambient_light);
    // Albedo for indirect diffuse bounce throughput
    Radiance albedo = rec.mat_ptr->kd * (rec.mat_ptr->has_texture 
        ? rec.mat_ptr->get_texture_color(rec.u, rec.v) 
        : rec.mat_ptr->diffusecolor);

    // Russian Roulette: check BEFORE recursive traces
    double rr_weight = (depth > 3) ? 1.0 / RR_PROB : 1.0;
    if (depth > 3) {
        if (random_double() > RR_PROB) {
            // Return only non-recursive local contribution based on branch
            if (mat->is_reflective && mat->is_refractive) {
                return emitted;  // Fresnel blend has no local term
            } else if (mat->is_reflective) {
                return emitted + (1.0 - mat->reflectivity) * surface_color;
            } else if (mat->is_refractive) {
                return emitted + (1.0 - mat->reflectivity) * surface_color;
            } else {
                return emitted + surface_color;  // diffuse: full direct lighting
            }
        }
    }

    // Ideal reflection and refraction
    Radiance reflection_color(0, 0, 0);
    Radiance refraction_color(0, 0, 0);
    if (mat->is_reflective) {
        Vector3 reflect_dir = reflect_pm(r.direction(), rec.normal);
        Ray reflect_ray(rec.p + 0.001 * reflect_dir, reflect_dir);
        reflection_color = trace_path(reflect_ray, world, lights, depth - 1);
    }
    if (mat->is_refractive) {
        double eta = rec.front_face ? (1.0 / mat->refractiveindex) : mat->refractiveindex;
        double cos_theta_i = std::fabs(dot(-unit_vector(r.direction()), rec.normal));
        double sin_theta_t_sq = eta * eta * (1.0 - cos_theta_i * cos_theta_i);

        if (sin_theta_t_sq > 1.0) {
            Vector3 reflect_dir = reflect_pm(r.direction(), rec.normal);
            Ray reflect_ray(rec.p + 0.001 * reflect_dir, reflect_dir);
            refraction_color = trace_path(reflect_ray, world, lights, depth - 1);
        } else {
            Vector3 refract_dir = refract(unit_vector(r.direction()), rec.normal, eta);
            Ray refract_ray(rec.p + 0.001 * refract_dir, refract_dir);
            refraction_color = trace_path(refract_ray, world, lights, depth - 1);
        }
    }

    Radiance result = emitted;
    if (mat->is_reflective && mat->is_refractive) {
        double cos_theta = std::fabs(dot(-unit_vector(r.direction()), rec.normal));
        double R0 = pow((1.0 - mat->refractiveindex) / (1.0 + mat->refractiveindex), 2);
        double reflectance = R0 + (1.0 - R0) * pow(1.0 - cos_theta, 5);
        result += rr_weight * (reflectance * reflection_color + (1.0 - reflectance) * refraction_color);
    } else if (mat->is_reflective) {
        result += (1.0 - mat->reflectivity) * surface_color + rr_weight * mat->reflectivity * reflection_color;
    } else if (mat->is_refractive) {
        result += (1.0 - mat->reflectivity) * surface_color + rr_weight * mat->reflectivity * refraction_color;
    } else {
        Vector3 scatter_dir = unit_vector(random_in_hemisphere(rec.normal));
        double cos_theta = std::max(0.0, dot(scatter_dir, rec.normal));
        Ray scattered_ray(rec.p + 0.001 * scatter_dir, scatter_dir);
        Radiance indirect = 2.0 * cos_theta * albedo * trace_path(scattered_ray, world, lights, depth - 1);
        result += surface_color + rr_weight * indirect;
    }

    return result;
}


//brdf supported
Radiance Camera::trace_path_brdf(const Ray& r, const Object& world, const Light& lights, int depth) const {
    if(depth <=0) {
        return Radiance(0, 0, 0);
    }

    IntersectionRecord rec;
    if (!world.intersect(r, Interval(0.001, infinity), rec)) {
        return background;
    }

    Vector3 view_dir = -r.direction();

    FancyBRDF brdf = rec.mat_ptr->build_brdf( rec.normal, rec.u, rec.v);

    Vector3 coefficient = Vector3(1, 1, 1);
    Radiance direct = lights.brdf_shading(rec.p, rec.normal, view_dir, brdf, world, coefficient);

    // Russian Roulette: check BEFORE recursive trace
    if (depth > 3) {
        if (random_double() > RR_PROB) {
            return direct;
        }
    }
    double rr_weight = (depth > 3) ? 1.0 / RR_PROB : 1.0;

    Vector3 l;
    Vector3 sample = Vector3(random_double(), random_double(), 0);
    Vector3 weight = brdf.Sample(l,view_dir, sample);

    coefficient *= weight;

    Ray scattered_ray(rec.p + 0.001 * l, l);
    Radiance indirect = trace_path(scattered_ray, world, lights, depth - 1);

    Radiance result = direct + rr_weight * coefficient * indirect;

    return result;
}


// RENDERING MODES

// std::vector<Radiance> Camera::render_path(const Object& world, Light& lights) {
//     initialize();
//     std::vector<Radiance> pixelData(image_width * image_height);
//     for (int j = 0; j < image_height; j++) {
//         std::clog << "\rRemaining rows: " << (image_height - j) << ' ' << std::flush;
//         for (int i = 0; i < image_width; i++) {
//             Radiance pixel_color(0, 0, 0);
//             // Multi-sampling loop
//             for (int sample = 0; sample < samples_per_pixel; ++sample) {
//                 Ray r = get_sampled_ray(i, j);  // Generate a ray with random sampling
//                 pixel_color += trace_path(r, world, lights, nbounces);
//             }
//             // Average the color of the sampled rays
//             pixel_color *= (1.0 / samples_per_pixel);
//             // Apply tone mapping
//             pixelData[j * image_width + i] = tone_map(pixel_color);
//         }
//     }
//     return pixelData;
// }


std::vector<Radiance> Camera::render_path(const Object& world, Light& lights) {
    initialize();
    std::vector<Radiance> pixelData(static_cast<size_t>(image_width) * static_cast<size_t>(image_height));

    std::vector<Vector3> poisson_samples;
    if (sampling_method == AntiAliasing::POISSON) {
        double min_distance = 0.5 / std::sqrt(samples_per_pixel);
        poisson_samples = generate_poisson_disk_samples(samples_per_pixel, min_distance);
    }

    for (int j = 0; j < image_height; j++) {
        std::clog << "\rRemaining rows: " << (image_height - j) << ' ' << std::flush;
        for (int i = 0; i < image_width; i++) {
            Radiance pixel_color(0, 0, 0);
            for (int sample = 0; sample < samples_per_pixel; ++sample) {
                Vector3 offset;
                switch (sampling_method) {
                    case AntiAliasing::RANDOM:
                        offset = sample_offset();
                        break;
                    case AntiAliasing::JITTERED:
                        offset = jittered_sample(sample);
                        break;
                    case AntiAliasing::POISSON:
                        offset = poisson_samples[sample % poisson_samples.size()];
                        break;
                }
                Ray r = get_sampled_ray(i, j, offset);
                pixel_color += trace_path(r, world, lights, nbounces);
            }
            pixel_color *= (1.0 / samples_per_pixel); // Average the samples
            pixelData[static_cast<size_t>(j) * static_cast<size_t>(image_width) + static_cast<size_t>(i)] = tone_map(pixel_color); // Tone mapping
        }
    }

    return pixelData;
}

std::vector<Radiance> Camera::render_path_parallel(const Object& world, Light& lights, size_t num_threads) {
    initialize();

    std::vector<Radiance> pixelData(static_cast<size_t>(image_width) * static_cast<size_t>(image_height));

    std::vector<Vector3> poisson_samples;
    if (sampling_method == AntiAliasing::POISSON) {
        double min_distance = 0.5 / std::sqrt(samples_per_pixel);
        poisson_samples = generate_poisson_disk_samples(samples_per_pixel, min_distance);
    }

    const int tile_size = 32;
    const int tiles_x = (image_width + tile_size - 1) / tile_size;
    const int tiles_y = (image_height + tile_size - 1) / tile_size;

    ThreadPool pool(num_threads);
    std::vector<std::future<void>> futures;
    futures.reserve(static_cast<size_t>(tiles_x) * static_cast<size_t>(tiles_y));

    for (int ty = 0; ty < tiles_y; ++ty) {
        for (int tx = 0; tx < tiles_x; ++tx) {
            int x0 = tx * tile_size;
            int y0 = ty * tile_size;
            int x1 = std::min(x0 + tile_size, image_width);
            int y1 = std::min(y0 + tile_size, image_height);

            futures.push_back(pool.enqueue([this, &world, &lights, &pixelData, &poisson_samples, x0, y0, x1, y1]() {
                for (int j = y0; j < y1; ++j) {
                    for (int i = x0; i < x1; ++i) {
                        Radiance pixel_color(0, 0, 0);
                        for (int sample = 0; sample < samples_per_pixel; ++sample) {
                            Vector3 offset;
                            switch (sampling_method) {
                                case AntiAliasing::RANDOM:
                                    offset = sample_offset();
                                    break;
                                case AntiAliasing::JITTERED:
                                    offset = jittered_sample(sample);
                                    break;
                                case AntiAliasing::POISSON:
                                    offset = poisson_samples[sample % poisson_samples.size()];
                                    break;
                            }
                            Ray r = get_sampled_ray(i, j, offset);
                            pixel_color += trace_path(r, world, lights, nbounces);
                        }
                        pixel_color *= (1.0 / samples_per_pixel);
                        pixelData[static_cast<size_t>(j) * static_cast<size_t>(image_width) + static_cast<size_t>(i)] = tone_map(pixel_color);
                    }
                }
            }));
        }
    }

    for (auto& f : futures) {
        f.get();
    }

    return pixelData;
}

std::vector<Radiance> Camera::render_binary(const Object& world) {
    initialize();
    std::vector<Radiance> pixelData(static_cast<size_t>(image_width) * static_cast<size_t>(image_height));
    for (int j = 0; j < image_height; j++) {
        std::clog << "\rRemaining rows: " << (image_height - j) << ' ' << std::flush;
        for (int i = 0; i < image_width; i++) {
            auto pixel_center = first_pixel + (i * pixel_delta_u) + (j * pixel_delta_v);
            auto ray_direction = pixel_center - center;
            Ray r(center, ray_direction);
            // Pass `nbounces` to control recursive depth
            Radiance pixel_color = trace_binary(r, world);
            pixelData[static_cast<size_t>(j) * static_cast<size_t>(image_width) + static_cast<size_t>(i)] = pixel_color;
        }
    }
    return pixelData;
}

std::vector<Radiance> Camera::render_phong(const Object& world, const Light& lights) {
    initialize();
    std::vector<Radiance> pixelData(static_cast<size_t>(image_width) * static_cast<size_t>(image_height));
    for (int j = 0; j < image_height; j++) {
        std::clog << "\rRemaining rows: " << (image_height - j) << ' ' << std::flush;
        for (int i = 0; i < image_width; i++) {
            Ray r = pixel_center_ray(i, j);
            // Pass `nbounces` to control recursive depth
            Radiance pixel_color = trace_phong(r, world, lights, nbounces);
            pixelData[static_cast<size_t>(j) * static_cast<size_t>(image_width) + static_cast<size_t>(i)] = tone_map(pixel_color);
        }
    }
    return pixelData;
}

// ---------------------------------------------------------------------------
// Gaussian participating-medium rendering
// ---------------------------------------------------------------------------

namespace {

bool finite_nonneg_rgb(const Radiance& c) {
    return std::isfinite(c.x()) && std::isfinite(c.y()) && std::isfinite(c.z()) &&
           c.x() >= 0.0 && c.y() >= 0.0 && c.z() >= 0.0;
}

// The gaussian entry only accepts ordinary Phong materials: no refraction and
// no microfacet BRDF, with finite non-negative parameters.
bool gaussian_material_supported(const Material& m) {
    if (m.is_refractive || m.has_brdf) return false;
    if (!std::isfinite(m.reflectivity) || m.reflectivity < 0.0 ||
        m.reflectivity > 1.0)
        return false;
    if (!std::isfinite(m.kd) || m.kd < 0.0 || !std::isfinite(m.ks) || m.ks < 0.0 ||
        !std::isfinite(m.ka) || m.ka < 0.0 || !std::isfinite(m.specularexponent) ||
        m.specularexponent < 0.0)
        return false;
    return finite_nonneg_rgb(m.diffusecolor) && finite_nonneg_rgb(m.specularcolor) &&
           finite_nonneg_rgb(m.base_color);
}

} // namespace

struct Camera::GaussianTraceOut {
    Radiance value{0, 0, 0};
    Radiance error{0, 0, 0};
    double transmittance = 1.0; // first segment of this ray only
};

bool Camera::validate_gaussian_lights(const Light& light) const {
    if (const auto* point = dynamic_cast<const PointLight*>(&light)) {
        return std::isfinite(point->position.x()) &&
               std::isfinite(point->position.y()) &&
               std::isfinite(point->position.z()) &&
               finite_nonneg_rgb(point->intensity);
    }
    if (const auto* list = dynamic_cast<const LightList*>(&light)) {
        for (const auto& child : list->lights) {
            if (!child || !validate_gaussian_lights(*child)) return false;
        }
        return true;
    }
    return false; // RectangularLight and any other light type are unsupported
}

bool Camera::gaussian_light_shading(const Light& light, const Point3& p,
                                    const Vector3& normal, const Vector3& view_dir,
                                    const IntersectionRecord& rec,
                                    const Object& world,
                                    const GaussianMedium& medium,
                                    Radiance& shading) const {
    if (const auto* point = dynamic_cast<const PointLight*>(&light)) {
        const Material* mat = rec.mat_ptr;

        // Geometric shadow_frac is computed by the existing point-light code,
        // so opaque/refractive occlusion behaves exactly as in plain Phong.
        double shadow = point->shadow_factor(p, world);

        // Finite shadow line from the actual offset origin to the light.
        // The ray matches PointLight::shadow_factor exactly (unit direction,
        // same 0.001 offset) so geometric shadow_frac and extinction share one
        // geometric line.
        Vector3 light_dir = unit_vector(point->position - p);
        Point3 shadow_origin = p + 0.001 * light_dir;
        Ray shadow_ray(shadow_origin, light_dir);
        double line_end = (point->position - shadow_origin).length();
        IntersectionRecord shadow_rec;
        if (world.intersect(shadow_ray, Interval(0.001, line_end), shadow_rec)) {
            line_end = shadow_rec.t; // kernels behind an occluder are not seen
        }
        MediumStats shadow_stats;
        // Extinction only: emission is never added along shadow lines.
        shadow *= medium.transmittance(shadow_ray, Interval(0.0, line_end),
                                       shadow_stats);

        Radiance texture_color = mat->diffusecolor;
        if (mat->has_texture) {
            texture_color = mat->get_texture_color(rec.u, rec.v);
        }
        Radiance light_intensity = point->intensity_at(p, light_dir);
        double diff = std::max(0.0, dot(normal, light_dir));
        Radiance diffuse = shadow * mat->kd * texture_color * light_intensity * diff;
        Vector3 halfway = unit_vector(light_dir + view_dir);
        double spec = pow(std::max(0.0, dot(normal, halfway)), mat->specularexponent);
        Radiance specular = shadow * mat->ks * mat->specularcolor *
                            light_intensity * spec;
        shading += diffuse + specular;
        return true;
    }

    if (const auto* list = dynamic_cast<const LightList*>(&light)) {
        for (const auto& child : list->lights) {
            if (!child || !gaussian_light_shading(*child, p, normal, view_dir, rec,
                                                  world, medium, shading))
                return false;
        }
        return true;
    }
    return false;
}

bool Camera::trace_gaussian(const Ray& r, const Object& world,
                            const Light& lights, const GaussianMedium& medium,
                            double abs_error, std::uint64_t& work_left,
                            int depth, GaussianTraceOut& out) const {
    IntersectionRecord rec;
    bool hit = world.intersect(r, Interval(0.001, infinity), rec);
    if (hit && !gaussian_material_supported(*rec.mat_ptr)) return false;

    // A mirror child (if any) shares this ray's error budget; splitting it
    // keeps the accumulated per-pixel bound inside abs_error at every depth.
    bool spawn_child = hit && depth > 0 && rec.mat_ptr->is_reflective &&
                       !rec.mat_ptr->is_refractive && rec.mat_ptr->reflectivity > 0.0;
    double segment_tolerance = spawn_child ? 0.5 * abs_error : abs_error;

    // Medium segment from this ray's actual origin to the next surface (or to
    // the end of all kernel support on a miss).
    SegmentResult segment;
    MediumStats medium_stats;
    std::uint64_t budget = work_left == 0 ? 1 : work_left;
    if (!medium.segment(r, Interval(0.0, hit ? rec.t : infinity),
                        segment_tolerance, budget, segment, medium_stats))
        return false;
    if (medium_stats.interval_evaluations > work_left) return false;
    work_left -= medium_stats.interval_evaluations;

    const double first_T = segment.transmittance;
    Radiance value = segment.emission;
    Radiance error = segment.error;

    if (!hit) {
        // Miss: medium emission over the full support, then the old background.
        value += first_T * background;
        out.value = value;
        out.error = error;
        out.transmittance = first_T;
        return true;
    }

    Radiance surface = calculate_ambient(*rec.mat_ptr, ambient_light, rec.u, rec.v);
    Radiance direct(0, 0, 0);
    if (!gaussian_light_shading(lights, rec.p, rec.normal,
                                unit_vector(-r.direction()), rec, world, medium,
                                direct))
        return false;
    surface += direct;

    if (!spawn_child) {
        // Depth exhausted or plain local material: Phong local term only.
        value += first_T * surface;
        out.value = value;
        out.error = error;
        out.transmittance = first_T;
        return true;
    }

    // Reflective branch reuses the exact Phong reflection construction.
    double reflectivity = rec.mat_ptr->reflectivity;
    Vector3 reflect_dir = reflect_pm(r.direction(), rec.normal);
    Ray reflect_ray(rec.p + 0.001 * reflect_dir, reflect_dir);
    GaussianTraceOut child;
    if (!trace_gaussian(reflect_ray, world, lights, medium, 0.5 * abs_error,
                        work_left, depth - 1, child))
        return false;

    value += first_T * ((1.0 - reflectivity) * surface +
                        reflectivity * child.value);
    // Surface terms carry no medium error; only recursive emission propagates,
    // weighted by this segment's transmittance and the mirror weight.
    error += first_T * reflectivity * child.error;
    for (int c = 0; c < 3; ++c) error[c] = std::min(error[c], abs_error);

    out.value = value;
    out.error = error;
    // The camera's first-segment transmittance never takes the mirror value.
    out.transmittance = first_T;
    return true;
}

bool Camera::render_gaussian(const Object& world, const Light& lights,
                             const GaussianMedium& medium, double abs_error,
                             std::uint64_t max_work, GaussianFrame& out) {
    if (!std::isfinite(abs_error) || abs_error <= 0.0) return false;
    if (max_work < 1) return false;
    if (!validate_gaussian_lights(lights)) return false;

    initialize();
    const std::size_t pixel_count =
        static_cast<std::size_t>(image_width) * static_cast<std::size_t>(image_height);

    // Render into a private frame first; only a fully successful frame replaces
    // the caller's output.
    GaussianFrame frame;
    frame.width = image_width;
    frame.height = image_height;
    frame.value.resize(pixel_count);
    frame.error.resize(pixel_count);
    frame.transmittance.resize(pixel_count);

    for (int j = 0; j < image_height; ++j) {
        for (int i = 0; i < image_width; ++i) {
            Ray r = pixel_center_ray(i, j);
            std::uint64_t work_left = max_work;
            GaussianTraceOut trace_out;
            if (!trace_gaussian(r, world, lights, medium, abs_error, work_left,
                                nbounces, trace_out))
                return false;
            std::size_t idx = static_cast<std::size_t>(j) *
                                  static_cast<std::size_t>(image_width) +
                              static_cast<std::size_t>(i);
            frame.value[idx] = trace_out.value;
            frame.error[idx] = trace_out.error;
            frame.transmittance[idx] = trace_out.transmittance;
        }
    }

    out = std::move(frame); // linear data, no tone mapping is applied
    return true;
}
