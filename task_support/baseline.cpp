#include "source/integrator/path-tracer/path-tracer.hpp"
#include "source/sampling/sampler.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    auto scene = nlohmann::json::parse(R"({
        "num_render_threads": 1,
        "materials": {
            "light": {"reflectance": [0,0,0], "specular_reflectance": [0,0,0], "emittance": [2,3,4]},
            "black": {"reflectance": [0,0,0], "specular_reflectance": [0,0,0]}
        },
        "surfaces": [{"type":"sphere", "radius":1, "material":"light"}],
        "bvh": {"type":"BINARY_SAH", "bins_per_axis":8}
    })");
    for (int index = 0; index < 8; ++index)
        scene["surfaces"].push_back({{"type","sphere"}, {"radius",0.25},
            {"material","black"}, {"position", {10 + 3 * index,0,0}}});
    PathTracer tracer(scene);
    const Ray front({0,0,3}, {0,0,-1}, 1.0);
    const Ray away({0,0,3}, {0,0,1}, 1.0);
    const auto hit = tracer.scene.intersect(front);
    require(bool(hit) && std::abs(hit.t - 2.0) < 1e-12, "production sphere/BVH hit");

    Sampler::initiate(17);
    Sampler::setIndex(3);
    const auto sample = Sampler::get<0,3>();
    Sampler::shuffle();
    Sampler::setIndex(3);
    require(sample == Sampler::get<0,3>(), "original sampler index replay");

    const auto emission = tracer.sampleRay(front);
    const double area = 4.0 * std::acos(-1.0);
    for (int component = 0; component < 3; ++component)
        require(std::isfinite(emission[component]) &&
                std::abs(emission[component] - (component + 2.0) / area) < 1e-11,
                "scalar direct emitter radiosity");

    const auto sky = tracer.sampleRay(away);
    for (int component = 0; component < 3; ++component)
        require(std::abs(sky[component] - 0.5) < 1e-12, "scalar miss sky");

    auto blocked = scene;
    blocked["surfaces"].push_back({{"type","sphere"}, {"radius",0.5},
        {"material","black"}, {"position", {0,0,2}}});
    PathTracer occluded(blocked);
    const auto black = occluded.sampleRay(front);
    for (int component = 0; component < 3; ++component)
        require(std::isfinite(black[component]) && std::abs(black[component]) < 1e-12,
                "opaque black surface occludes emitter");
    std::cout << "SCALAR_PRODUCTION_BASELINE_OK bvh=1 sampler_replay=1 emitter=1 sky=1 black_occlusion=1\n";
}
