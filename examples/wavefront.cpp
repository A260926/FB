// Wavefront path tracing example.
//
// Demonstrates the staged WavefrontBatch transport: camera rays are submitted, the
// batch is advanced one small query budget at a time (the execution yields after
// every step and is resumed later), and completed radiance results are deposited in
// submission order. The image produced by this run is written as a binary PPM (P6).

#include "source/integrator/path-tracer/path-tracer.hpp"
#include "source/integrator/path-tracer/wavefront.hpp"
#include "source/sampling/sampler.hpp"
#include "source/sampling/sampling.hpp"
#include "source/color/srgb.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t RENDER_SEED = 20240617u;
    constexpr size_t WIDTH = 72;
    constexpr size_t HEIGHT = 54;
    constexpr size_t SPP = 4;
    constexpr size_t BATCH_CAPACITY = 13;

    struct Job
    {
        size_t x, y, sample;
    };

    uint8_t toByte(double linear)
    {
        linear = std::clamp(linear, 0.0, 1.0);
        return static_cast<uint8_t>(std::round(sRGB::gammaCompress(glm::dvec3(linear))[0] * 255.0));
    }
}

int main(int argc, char** argv)
{
    const std::string out_path = (argc > 1) ? argv[1] : "wavefront.ppm";

    auto scene = nlohmann::json::parse(R"({
        "num_render_threads": 1,
        "materials": {
            "light":  {"reflectance":[0,0,0], "specular_reflectance":[0,0,0], "emittance":[12,11,9]},
            "white":  {"reflectance":[0.75,0.75,0.75], "specular_reflectance":[0.2,0.2,0.2]},
            "mirror": {"perfect_mirror": true},
            "glass":  {"ior":1.5, "transparency":1.0, "transmittance":[1,1,1]},
            "black":  {"reflectance":[0,0,0], "specular_reflectance":[0,0,0]}
        },
        "surfaces": [
            {"type":"sphere", "radius":1.0, "material":"light",  "position":[0,3.5,-3]},
            {"type":"sphere", "radius":1.0, "material":"white",  "position":[1.6,-0.6,-2.2]},
            {"type":"sphere", "radius":1.0, "material":"mirror", "position":[-1.7,-0.6,-2.4]},
            {"type":"sphere", "radius":0.8, "material":"glass",  "position":[0,0.2,-3.4]},
            {"type":"sphere", "radius":6.0, "material":"black",  "position":[0,6.2,-1]}
        ],
        "bvh": {"type":"BINARY_SAH", "bins_per_axis":8}
    })");

    PathTracer tracer(scene);
    WavefrontBatch batch(tracer, BATCH_CAPACITY);

    // Pinhole camera setup, matching the production Camera rules.
    const glm::dvec3 eye(0.0, 0.0, 3.0);
    const glm::dvec3 forward(0.0, 0.0, -1.0);
    const glm::dvec3 up(0.0, 1.0, 0.0);
    const glm::dvec3 left = glm::normalize(glm::cross(up, forward));
    const double focal_length = 0.035;
    const double sensor_width = 0.035;
    const double pixel_size = sensor_width / WIDTH;
    const glm::dvec2 half_dim = glm::dvec2(WIDTH, HEIGHT) * 0.5;

    // All camera samples form one shared work list; many pixels share the batch.
    std::deque<Job> jobs;
    for (size_t y = 0; y < HEIGHT; ++y)
        for (size_t x = 0; x < WIDTH; ++x)
            for (size_t s = 0; s < SPP; ++s)
                jobs.push_back(Job{ x, y, s });

    std::vector<glm::dvec3> accumulator(WIDTH * HEIGHT, glm::dvec3(0.0));

    // Pixel position of every retained path, aligned from the front with the batch's
    // submission order because takeCompleted releases a completed prefix.
    std::deque<glm::ivec2> pending_pixels;
    uint64_t next_id = 0;
    size_t steps = 0;

    while (!jobs.empty() || batch.retainedCount() > 0)
    {
        // Fill free capacity before advancing the staged transport.
        while (!jobs.empty() && batch.retainedCount() < BATCH_CAPACITY)
        {
            Job job = jobs.front();
            jobs.pop_front();

            Sampler::initiate(static_cast<uint32_t>(job.y * WIDTH + job.x), RENDER_SEED);
            Sampler::setIndex(static_cast<uint32_t>(job.sample));

            auto u = Sampler::get<Dim::PIXEL, 2>();
            glm::dvec2 px(job.x + u[0], job.y + u[1]);
            glm::dvec2 local = pixel_size * (half_dim - px);
            glm::dvec3 direction = glm::normalize(forward * focal_length + left * local.x + up * local.y);
            Ray ray(eye, direction, tracer.scene.ior);

            // State captured after camera sampling, before the path's first shuffle.
            Sampler::State state = Sampler::capture();

            while (!batch.submit(next_id++, ray, state))
            {
                // All slots retained: advance one intersection, then resume filling.
                batch.step(1);
                for (const auto& r : batch.takeCompleted())
                {
                    glm::ivec2 p = pending_pixels.front();
                    pending_pixels.pop_front();
                    accumulator[p.y * WIDTH + p.x] += r.radiance;
                }
            }
            pending_pixels.push_back(glm::ivec2(static_cast<int>(job.x), static_cast<int>(job.y)));
        }

        if (batch.retainedCount() == 0)
        {
            break;
        }

        // Yield after a single-intersection budget, then resume on the next loop pass.
        batch.step(1);
        ++steps;

        for (const auto& r : batch.takeCompleted())
        {
            glm::ivec2 p = pending_pixels.front();
            pending_pixels.pop_front();
            accumulator[p.y * WIDTH + p.x] += r.radiance;
        }
    }

    const auto& stats = batch.stats();
    std::cout << "Wavefront render finished after " << steps << " one-query steps.\n"
              << "  path intersections   : " << stats.path_intersections << "\n"
              << "  shadow intersections : " << stats.shadow_intersections << "\n"
              << "  completed paths      : " << stats.completed_paths << "\n"
              << "  peak ray/shade/shadow: " << stats.peak_ray_queue << "/"
              << stats.peak_shade_queue << "/" << stats.peak_shadow_queue << "\n";

    std::ofstream out(out_path, std::ios::binary);
    out << "P6\n" << WIDTH << " " << HEIGHT << "\n255\n";
    for (size_t i = 0; i < WIDTH * HEIGHT; ++i)
    {
        glm::dvec3 color = accumulator[i] / static_cast<double>(SPP);
        uint8_t rgb[3] = { toByte(color.r), toByte(color.g), toByte(color.b) };
        out.write(reinterpret_cast<char*>(rgb), 3);
    }
    out.close();

    std::cout << "Wrote PPM: " << out_path << "\n";
    return 0;
}
