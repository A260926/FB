#include "Camera.h"

#include "brdf/FancyBRDF.h"

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


Radiance Camera::trace_phong(const Ray& r, const Object& world, const Light& lights, int depth) const {
    IntersectionRecord rec;
    Radiance result;

    // If the ray hits nothing, return the background color.
    if (!world.intersect(r, Interval(0.001, infinity), rec)) {
        return background;
    }

    // Compute Blinn-Phong shading at the intersection point
    Vector3 view_dir = unit_vector(-r.direction());
    Radiance local_shading = lights.phong_shading(rec.p, rec.normal, view_dir, rec, world, ambient_light);

    // Compute ambient light contribution
    Radiance ambient = calculate_ambient(*rec.mat_ptr, ambient_light, rec.u, rec.v);

    // Stop recursion if max depth is reached
    if (depth <= 0) {
        return ambient + local_shading;
    }

    // Initialize reflection and refraction contributions
    Radiance reflected_color(0, 0, 0);
    Radiance refracted_color(0, 0, 0);

    // Reflective only case
    if (rec.mat_ptr->is_reflective && !rec.mat_ptr->is_refractive) {
        Vector3 reflect_dir = reflect_pm(r.direction(), rec.normal);
        Ray reflect_ray(rec.p + 0.001 * reflect_dir, reflect_dir);
        reflected_color = trace_phong(reflect_ray, world, lights, depth - 1);
        result = (1 - rec.mat_ptr->reflectivity) * (ambient + local_shading) +
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
        result = schlick_blend((ambient + local_shading), reflected_color, refracted_color, eta, cos_theta_i, R0);
        // result = ambient + local_shading + reflected_color + refracted_color;
        return result;
    }

    // Local shading only case
    result = ambient + local_shading;
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
            auto pixel_center = first_pixel + (i * pixel_delta_u) + (j * pixel_delta_v);
            auto ray_direction = pixel_center - center;
            Ray r(center, ray_direction);
            // Pass `nbounces` to control recursive depth
            Radiance pixel_color = trace_phong(r, world, lights, nbounces);
            pixelData[static_cast<size_t>(j) * static_cast<size_t>(image_width) + static_cast<size_t>(i)] = tone_map(pixel_color);
        }
    }
    return pixelData;
}
