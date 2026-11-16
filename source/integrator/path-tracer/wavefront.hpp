#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <glm/vec3.hpp>

#include "../../sampling/sampler.hpp"
#include "../../ray/ray.hpp"
#include "../../ray/interaction.hpp"
#include "../../integrator/integrator.hpp"

class PathTracer;

// Staged wavefront path transport.
//
// A path is never run to completion as a single task: every bounce moves through
// three independent pending queues, one per transport stage:
//
//   1. ray queue     -> main/continuation ray intersection (Scene/BVH)
//   2. shade queue   -> Interaction, emissive and direct-light setup, BSDF (material stage)
//   3. shadow queue  -> shadow/visibility ray intersection
//
// Between steps the batch retains the full per-path state (throughput, accumulated
// radiance, refraction history, previous light sample and BSDF density, sampler
// state), so execution can yield in the middle of a frame and resume later. The
// scene is only read while a batch is alive.
//
// Calls are expected to be handed off sequentially between threads; a batch is not
// internally synchronized for concurrent invocation.
class WavefrontBatch
{
public:
    struct Result
    {
        uint64_t id = 0;
        glm::dvec3 radiance = glm::dvec3(0.0);
    };

    struct Stats
    {
        // Number of real Scene::intersect calls, counted where the work happens.
        size_t path_intersections = 0;
        size_t shadow_intersections = 0;

        // Number of paths that reached a terminal state.
        size_t completed_paths = 0;

        // Largest number of tasks ever pending in each stage queue.
        size_t peak_ray_queue = 0;
        size_t peak_shade_queue = 0;
        size_t peak_shadow_queue = 0;
    };

    WavefrontBatch(PathTracer& tracer, size_t capacity);

    // Enqueue a camera/continuation ray together with the sampler state captured
    // after camera sampling and before the path's first shuffle. Returns false (and
    // changes nothing) when all capacity slots are retained. Throws std::invalid_argument
    // if an equal id is still retained.
    bool submit(uint64_t id, const Ray& ray, const Sampler::State& sampler_state);

    // Advance pending work using at most query_budget real Scene intersections.
    // Both main-path and shadow rays count against the budget. Returns the number
    // of intersections actually performed. step(0) performs no work at all.
    size_t step(size_t query_budget);

    // Remove only the results that are completed contiguously from the head of the
    // submission order. Returns an empty vector when nothing is ready.
    std::vector<Result> takeCompleted();

    // Paths currently retained: in-flight plus completed results not yet taken.
    size_t retainedCount() const;

    const Stats& stats() const { return stats_; }
    size_t capacity() const { return capacity_; }

private:
    enum class Phase : uint8_t
    {
        EMPTY,    // free slot
        ACTIVE,   // path is present in one of the stage queues
        COMPLETE  // terminal result retained until takeCompleted()
    };

    struct Slot
    {
        uint64_t id = 0;
        Phase phase = Phase::EMPTY;

        Sampler::State rng;
        Ray ray;
        glm::dvec3 radiance = glm::dvec3(0.0);
        glm::dvec3 throughput = glm::dvec3(1.0);

        // Carried across bounces, exactly as in the scalar PathTracer loop.
        RefractionHistory refraction_history;
        Integrator::LightSample ls;
        Intersection pending_intersection;
        std::optional<Interaction> interaction;

        // Pending direct-light (shadow ray) work.
        std::optional<Ray> shadow_ray;
        std::shared_ptr<Surface::Base> shadow_light;
        double shadow_select_probability = 0.0;
        double shadow_cos_light_theta = 0.0;
        bool has_shadow = false;

        Result result;

        Slot(const Ray& r) : ray(r), refraction_history(r) { }
    };

    void runRayTask(size_t slot_idx);
    void runShadeTask(size_t slot_idx);
    void runShadowTask(size_t slot_idx);
    void continueBounce(size_t slot_idx);
    void complete(size_t slot_idx);

    void pushRay(size_t slot_idx);
    void pushShade(size_t slot_idx);
    void pushShadow(size_t slot_idx);

    Slot& slot(size_t i) { return *slots_[i]; }

    PathTracer& tracer_;
    const size_t capacity_;

    // Stable addresses for the lifetime of a retained path; empty entries are free.
    std::vector<std::unique_ptr<Slot>> slots_;
    std::vector<size_t> free_slots_;

    // Stage queues hold indices into slots_. A retained path lives in exactly one
    // stage queue at a time, so the three queues together never hold more than
    // capacity tasks (<= 3 * capacity).
    std::deque<size_t> ray_queue_;
    std::deque<size_t> shade_queue_;
    std::deque<size_t> shadow_queue_;

    // Slot indices in submission order; completed heads are removed by takeCompleted.
    std::deque<size_t> submission_order_;

    Stats stats_;
};
