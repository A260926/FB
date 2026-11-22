#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

#include "../scene/scene.hpp"

class Integrator
{
public:
    Integrator(const nlohmann::json &j);

    virtual ~Integrator() { }

    struct LightSample
    {
        double bsdf_pdf = 0.0, select_probability = 0.0;
        std::shared_ptr<Surface::Base> light;
    };

    virtual glm::dvec3 sampleRay(Ray ray) = 0;

    // Integrators that need a deterministic per-sample identity (e.g. VCM) can
    // override this overload. The default keeps the old path tracer behavior.
    virtual glm::dvec3 sampleRay(Ray ray, uint32_t)
    {
        return sampleRay(ray);
    }

    // Variant carrying both the pixel and the sample index.
    virtual glm::dvec3 sampleRay(Ray ray, uint32_t, uint32_t sample_id)
    {
        return sampleRay(ray, sample_id);
    }

    // Progressive/iterative integrators advance one round per sampleImage() call.
    virtual void beginIteration(size_t) { }
    virtual void advanceIteration() { beginIteration(0); }
    virtual size_t iterations() const { return 1; }
    virtual bool iterative() const { return false; }

    // Used by the camera to initialize all random dimensions belonging to a
    // pixel sample without consuming any of the integrator's random sequence.
    virtual void initSample(uint32_t) const { }
    virtual void initSample(uint32_t, uint32_t sample_id) const { initSample(sample_id); }
    virtual bool deterministicSamples() const { return false; }

    glm::dvec3 sampleDirect(const Interaction& interaction, LightSample& ls) const;
    glm::dvec3 sampleEmissive(const Interaction& interaction, const LightSample& ls) const;
    bool absorb(const Ray& ray, glm::dvec3& throughput) const;

    size_t num_threads;
    Scene scene;

    const uint8_t min_ray_depth = 3;
    const uint8_t min_priority_ray_depth = 16;
};