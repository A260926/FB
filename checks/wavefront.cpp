#include "task_support/check.h"

#include "source/integrator/path-tracer/path-tracer.hpp"
#include "source/integrator/path-tracer/wavefront.hpp"
#include "source/sampling/sampler.hpp"
#include "source/sampling/sampling.hpp"
#include "source/camera/camera.hpp"
#include "source/common/option.hpp"

#include <glm/gtx/component_wise.hpp>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32_t FROZEN_RENDER_SEED = 987654321u;
    constexpr double TOL = 1e-11;

    bool close(double a, double b)
    {
        double scale = std::max({ 1.0, std::abs(a), std::abs(b) });
        return std::abs(a - b) <= TOL * scale;
    }

    bool closeVec(const glm::dvec3& a, const glm::dvec3& b)
    {
        for (int c = 0; c < 3; ++c)
        {
            if (!close(a[c], b[c])) return false;
        }
        return true;
    }

    nlohmann::json baseScene()
    {
        return nlohmann::json::parse(R"({
            "num_render_threads": 1,
            "materials": {
                "light":  {"reflectance": [0,0,0], "specular_reflectance": [0,0,0], "emittance": [2,3,4]},
                "black":  {"reflectance": [0,0,0], "specular_reflectance": [0,0,0]},
                "white":  {"reflectance": [0.7,0.7,0.7], "specular_reflectance": [0.2,0.2,0.2]},
                "mirror": {"perfect_mirror": true},
                "glass":  {"ior": 1.5, "transparency": 1.0, "transmittance": [1,1,1]}
            },
            "surfaces": [
                {"type":"sphere", "radius":1.0, "material":"light", "position":[0,0,0]}
            ],
            "bvh": {"type":"BINARY_SAH", "bins_per_axis":8}
        })");
    }

    // Establishes a frozen, fully deterministic per-path random context and returns
    // the sampler state as it is after setIndex (before the path's first shuffle).
    Sampler::State frozenState(uint32_t pixel_seed, uint32_t path_index,
                               uint32_t render_seed = FROZEN_RENDER_SEED)
    {
        Sampler::initiate(pixel_seed, render_seed);
        Sampler::setIndex(path_index);
        return Sampler::capture();
    }

    struct PairRun
    {
        glm::dvec3 scalar = glm::dvec3(0.0);
        glm::dvec3 wavefront = glm::dvec3(0.0);
        WavefrontBatch::Stats stats;
        size_t results_taken = 0;
        size_t queries = 0;
        bool ok = true;
    };

    // Runs one ray both through the scalar entry point and through a WavefrontBatch,
    // starting both from the identical frozen sampler state.
    PairRun runPair(PathTracer& tracer, const Ray& ray, const Sampler::State& start,
                    size_t capacity, size_t budget)
    {
        PairRun out;
#define FAIL_PAIR(cond) do { if (!(cond)) { out.ok = false; return out; } } while (0)

        // Scalar reference.
        Sampler::restore(start);
        out.scalar = tracer.sampleRay(ray);

        // Wavefront transport, driven in arbitrary small steps.
        WavefrontBatch batch(tracer, capacity);
        FAIL_PAIR(batch.submit(1, ray, start));

        std::unordered_map<uint64_t, glm::dvec3> collected;
        size_t guard = 0;
        while (batch.retainedCount() > 0)
        {
            size_t used = batch.step(budget);
            out.queries += used;
            for (const auto& r : batch.takeCompleted())
            {
                // Every id must be returned exactly once.
                FAIL_PAIR(collected.emplace(r.id, r.radiance).second);
                ++out.results_taken;
            }
            if (++guard > 100000) break;
        }

        FAIL_PAIR(guard <= 100000);
        FAIL_PAIR(collected.size() == 1);
        FAIL_PAIR(collected.count(1) == 1);
        out.wavefront = collected[1];
        out.stats = batch.stats();
        return out;
#undef FAIL_PAIR
    }

    Ray viewRay(const Scene& scene)
    {
        return Ray({ 0.0, 0.0, 3.0 }, { 0.0, 0.0, -1.0 }, scene.ior);
    }
}

// TASK_CHECK returns from the registered case on failure; the runPair helper instead
// records failure in PairRun::ok so each case can assert it via TASK_CHECK.

namespace
{
    // ------------------------------------------------------------------ contexts
    int contexts_state_roundtrip()
    {
        Sampler::initiate(42u, 7u);
        Sampler::setIndex(5u);
        Sampler::shuffle();
        auto dims = Sampler::get<Dim::BSDF, 2>();
        (void)dims;

        Sampler::State saved = Sampler::capture();
        Sampler::initiate(999u, 1234u);
        Sampler::setIndex(11u);
        Sampler::shuffle();
        TASK_CHECK(Sampler::capture() != saved);

        Sampler::restore(saved);
        TASK_CHECK(Sampler::capture() == saved);

        // Replayed dimensions from a restored state must match exactly.
        auto again = Sampler::get<Dim::BSDF, 2>();
        TASK_CHECK(again[0] == dims[0] && again[1] == dims[1]);
        return 0;
    }

    int contexts_explicit_seed_matches()
    {
        // The two-argument initiate uses the explicit render seed; re-deriving the
        // same state must reproduce the same Sobol samples deterministically.
        Sampler::initiate(17u, FROZEN_RENDER_SEED);
        Sampler::setIndex(3u);
        auto first = Sampler::get<0, 3>();

        Sampler::initiate(17u, FROZEN_RENDER_SEED);
        Sampler::setIndex(3u);
        auto second = Sampler::get<0, 3>();
        TASK_CHECK(first == second);

        // A different render seed changes the stream.
        Sampler::initiate(17u, FROZEN_RENDER_SEED ^ 0xffffffffu);
        Sampler::setIndex(3u);
        auto other = Sampler::get<0, 3>();
        TASK_CHECK(other != first);
        return 0;
    }

    int contexts_step_restores_caller_sampler()
    {
        auto j = baseScene();
        PathTracer tracer(j);
        Ray ray = viewRay(tracer.scene);
        Sampler::State start = frozenState(1u, 0u);

        WavefrontBatch batch(tracer, 4);
        TASK_CHECK(batch.submit(7, ray, start));

        Sampler::State caller = frozenState(33u, 9u); // unrelated caller context
        Sampler::restore(caller);

        TASK_CHECK(batch.step(0) == 0);
        TASK_CHECK(Sampler::capture() == caller);
        TASK_CHECK(batch.retainedCount() == 1);

        TASK_CHECK(batch.step(1) >= 1);
        TASK_CHECK(Sampler::capture() == caller);

        while (batch.retainedCount() > 0)
        {
            batch.step(3);
            batch.takeCompleted();
        }
        TASK_CHECK(Sampler::capture() == caller);
        return 0;
    }

    int contexts_step_zero_advances_nothing()
    {
        auto j = baseScene();
        PathTracer tracer(j);
        Ray ray = viewRay(tracer.scene);
        Sampler::State start = frozenState(2u, 1u);

        WavefrontBatch batch(tracer, 2);
        TASK_CHECK(batch.submit(1, ray, start));
        TASK_CHECK(batch.step(0) == 0);
        TASK_CHECK(batch.takeCompleted().empty());
        TASK_CHECK(batch.retainedCount() == 1);
        TASK_CHECK(batch.stats().path_intersections == 0);
        TASK_CHECK(batch.stats().shadow_intersections == 0);
        return 0;
    }

    int contexts_interleaved_batches_isolated()
    {
        auto j = baseScene();
        PathTracer tracer(j);

        Ray ray_a = viewRay(tracer.scene);
        Ray ray_b({ 0.0, 0.0, 3.0 }, { 0.0, 0.0, 1.0 }, tracer.scene.ior); // misses -> sky

        Sampler::State sa = frozenState(5u, 0u);
        Sampler::State sb = frozenState(6u, 0u);

        Sampler::restore(sa);
        glm::dvec3 scalar_a = tracer.sampleRay(ray_a);
        Sampler::restore(sb);
        glm::dvec3 scalar_b = tracer.sampleRay(ray_b);

        WavefrontBatch a(tracer, 2), b(tracer, 2);
        TASK_CHECK(a.submit(100, ray_a, sa));
        TASK_CHECK(b.submit(200, ray_b, sb));

        Sampler::State caller = frozenState(77u, 3u);
        Sampler::restore(caller);

        std::unordered_map<uint64_t, glm::dvec3> got;
        size_t guard = 0;
        while (a.retainedCount() > 0 || b.retainedCount() > 0)
        {
            a.step(1);
            b.step(1);
            for (const auto& r : a.takeCompleted()) TASK_CHECK(got.emplace(r.id, r.radiance).second);
            for (const auto& r : b.takeCompleted()) TASK_CHECK(got.emplace(r.id, r.radiance).second);
            TASK_CHECK(Sampler::capture() == caller);
            if (++guard > 100000) { TASK_CHECK(false); break; }
        }

        TASK_CHECK(got.size() == 2);
        TASK_CHECK(closeVec(got[100], scalar_a));
        TASK_CHECK(closeVec(got[200], scalar_b));
        return 0;
    }

    // ----------------------------------------------------------------- transport
    int transport_matches_scalar_many_samples()
    {
        auto j = baseScene();
        // Add a diffuse, mirror and glass sphere so paths exercise every material.
        j["surfaces"].push_back({ {"type","sphere"}, {"radius",0.5}, {"material","white"}, {"position",{1.5,0,-1.5}} });
        j["surfaces"].push_back({ {"type","sphere"}, {"radius",0.5}, {"material","mirror"}, {"position",{-1.5,0,-1.5}} });
        j["surfaces"].push_back({ {"type","sphere"}, {"radius",0.5}, {"material","glass"}, {"position",{0,1.2,-2.0}} });
        PathTracer tracer(j);
        Ray ray = viewRay(tracer.scene);

        for (uint32_t i = 0; i < 48; ++i)
        {
            Sampler::State start = frozenState(11u, i);
            PairRun r = runPair(tracer, ray, start, 7, 1);
            TASK_CHECK(r.ok);
            TASK_CHECK(r.results_taken == 1);
            TASK_CHECK(closeVec(r.scalar, r.wavefront));
            TASK_CHECK(r.stats.completed_paths == 1);
            TASK_CHECK(r.queries == r.stats.path_intersections + r.stats.shadow_intersections);
        }
        return 0;
    }

    int transport_capacities_one_three_seventeen()
    {
        auto j = baseScene();
        j["surfaces"].push_back({ {"type","sphere"}, {"radius",0.6}, {"material","white"}, {"position",{1.2,0,-1.2}} });
        PathTracer tracer(j);
        Ray ray = viewRay(tracer.scene);

        for (size_t capacity : { size_t(1), size_t(3), size_t(17) })
        {
            for (uint32_t i = 0; i < 12; ++i)
            {
                Sampler::State start = frozenState(21u, i);
                PairRun r = runPair(tracer, ray, start, capacity, capacity == 1 ? 1 : 2);
                TASK_CHECK(r.ok);
                TASK_CHECK(closeVec(r.scalar, r.wavefront));
                TASK_CHECK(r.stats.peak_ray_queue >= 1);
            }
        }
        return 0;
    }

    int transport_mixed_budgets_identical()
    {
        auto j = baseScene();
        j["surfaces"].push_back({ {"type","sphere"}, {"radius",0.6}, {"material","white"}, {"position",{1.2,0,-1.2}} });
        PathTracer tracer(j);
        Ray ray = viewRay(tracer.scene);

        for (size_t budget : { size_t(1), size_t(2), size_t(5), size_t(1000) })
        {
            Sampler::State start = frozenState(31u, 4u);
            PairRun r = runPair(tracer, ray, start, 5, budget);
            TASK_CHECK(r.ok);
            TASK_CHECK(closeVec(r.scalar, r.wavefront));
        }

        // Explicitly mixed budget sequence on one batch.
        Sampler::State start = frozenState(31u, 4u);
        Sampler::restore(start);
        glm::dvec3 reference = tracer.sampleRay(ray);

        WavefrontBatch batch(tracer, 5);
        TASK_CHECK(batch.submit(1, ray, start));
        const size_t cycle[] = { 1, 3, 1, 7, 2 };
        size_t step_idx = 0;
        glm::dvec3 got;
        bool have = false;
        size_t guard = 0;
        while (batch.retainedCount() > 0)
        {
            batch.step(cycle[step_idx++ % 5]);
            auto done = batch.takeCompleted();
            if (!done.empty()) { got = done.front().radiance; have = true; }
            if (++guard > 100000) break;
        }
        TASK_CHECK(have);
        TASK_CHECK(closeVec(reference, got));
        return 0;
    }

    int transport_specular_and_two_refractions()
    {
        // A perfect mirror plus two concentric dielectric shells and no occluding
        // light. A central ray that transmits crosses four interfaces: outer
        // front/back and inner front/back, raising the refraction level to two
        // before unwinding again.
        auto j = nlohmann::json::parse(R"({
            "num_render_threads": 1,
            "materials": {
                "mirror": {"perfect_mirror": true},
                "glass":  {"ior": 1.5, "transparency": 1.0, "transmittance": [1,1,1]},
                "dense":  {"ior": 2.0, "transparency": 1.0, "transmittance": [1,1,1]}
            },
            "surfaces": [
                {"type":"sphere", "radius":0.8,  "material":"mirror", "position":[-2,0,-2]},
                {"type":"sphere", "radius":1.0,  "material":"glass",  "position":[0,0,-4]},
                {"type":"sphere", "radius":0.35, "material":"dense",  "position":[0,0,-4]}
            ],
            "bvh": {"type":"BINARY_SAH", "bins_per_axis":8}
        })");
        PathTracer tracer(j);

        Ray mirror_ray({ 0,0,3 }, glm::normalize(glm::dvec3(-0.6, 0.0, -1.0)), tracer.scene.ior);
        Ray glass_ray({ 0,0,3 }, { 0,0,-1 }, tracer.scene.ior); // through the shell centers

        size_t deepest = 0;
        for (uint32_t i = 0; i < 24; ++i)
        {
            PairRun rm = runPair(tracer, mirror_ray, frozenState(41u, i), 4, 1);
            TASK_CHECK(rm.ok);
            TASK_CHECK(closeVec(rm.scalar, rm.wavefront));

            PairRun rg = runPair(tracer, glass_ray, frozenState(42u, i), 4, 1);
            TASK_CHECK(rg.ok);
            TASK_CHECK(closeVec(rg.scalar, rg.wavefront));
            deepest = std::max(deepest, rg.stats.path_intersections);
        }
        // Four dielectric interfaces on a fully transmitting central path means the
        // path entered both nested shells (two refraction layers), not just one.
        TASK_CHECK(deepest >= 4);
        return 0;
    }

    int transport_roulette_enclosed()
    {
        // Enclosing diffuse sphere keeps paths bouncing until Russian roulette / the
        // depth cap, while the small emitter provides illumination.
        auto j = nlohmann::json::parse(R"({
            "num_render_threads": 1,
            "materials": {
                "light": {"reflectance":[0,0,0], "specular_reflectance":[0,0,0], "emittance":[3,3,3]},
                "white": {"reflectance":[0.8,0.8,0.8], "specular_reflectance":[0.2,0.2,0.2]}
            },
            "surfaces": [
                {"type":"sphere", "radius":20.0, "material":"white", "position":[0,0,0]},
                {"type":"sphere", "radius":1.0, "material":"light", "position":[4,4,-4]}
            ],
            "bvh": {"type":"BINARY_SAH", "bins_per_axis":8}
        })");
        PathTracer tracer(j);
        Ray ray({ 0,0,0 }, { 0,0,-1 }, tracer.scene.ior);

        size_t max_depth_paths = 0;
        for (uint32_t i = 0; i < 64; ++i)
        {
            PairRun r = runPair(tracer, ray, frozenState(51u, i), 9, 3);
            TASK_CHECK(r.ok);
            TASK_CHECK(closeVec(r.scalar, r.wavefront));
            // Long chains prove roulette/deep-path handling is staged correctly.
            if (r.stats.path_intersections > 4) ++max_depth_paths;
        }
        TASK_CHECK(max_depth_paths > 0);
        return 0;
    }

    int transport_full_queue_and_duplicate_id()
    {
        auto j = baseScene();
        PathTracer tracer(j);
        Ray ray = viewRay(tracer.scene);
        Sampler::State start = frozenState(61u, 0u);

        WavefrontBatch batch(tracer, 2);
        TASK_CHECK(batch.capacity() == 2);
        TASK_CHECK(batch.submit(1, ray, start));
        TASK_CHECK(batch.submit(2, ray, start));
        TASK_CHECK(batch.retainedCount() == 2);

        // Full: submit must fail without changing state.
        TASK_CHECK(!batch.submit(3, ray, start));
        TASK_CHECK(batch.retainedCount() == 2);

        // Duplicate retained id throws and changes nothing.
        bool threw = false;
        try { batch.submit(2, ray, start); }
        catch (const std::invalid_argument&) { threw = true; }
        TASK_CHECK(threw);
        TASK_CHECK(batch.retainedCount() == 2);

        // Drain everything; slots and ids can then be reused.
        size_t guard = 0;
        while (batch.retainedCount() > 0)
        {
            batch.step(1);
            batch.takeCompleted();
            if (++guard > 100000) { TASK_CHECK(false); break; }
        }
        TASK_CHECK(batch.retainedCount() == 0);

        TASK_CHECK(batch.submit(2, ray, start)); // id 2 reusable, no throw
        TASK_CHECK(batch.retainedCount() == 1);

        bool threw_again = false;
        try { batch.submit(2, ray, start); }
        catch (const std::invalid_argument&) { threw_again = true; }
        TASK_CHECK(threw_again);
        return 0;
    }

    int transport_prefix_order_exact_once_and_bounds()
    {
        auto j = baseScene();
        j["surfaces"].push_back({ {"type","sphere"}, {"radius",0.6}, {"material","white"}, {"position",{1.2,0,-1.2}} });
        PathTracer tracer(j);

        const size_t capacity = 3;
        WavefrontBatch batch(tracer, capacity);

        Ray hit_ray = viewRay(tracer.scene);
        Ray miss_ray({ 0,0,3 }, { 0,0,1 }, tracer.scene.ior);

        const uint64_t ids[3] = { 10, 20, 30 };
        TASK_CHECK(batch.submit(ids[0], hit_ray, frozenState(70u, 0u)));   // head, longest
        TASK_CHECK(batch.submit(ids[1], miss_ray, frozenState(70u, 1u)));  // quick sky miss
        TASK_CHECK(batch.submit(ids[2], miss_ray, frozenState(70u, 2u)));
        TASK_CHECK(batch.retainedCount() == 3);

        std::vector<uint64_t> order;
        size_t guard = 0;
        while (batch.retainedCount() > 0)
        {
            batch.step(1);
            for (const auto& r : batch.takeCompleted())
            {
                // No result may be handed out twice and a later id cannot pass the head.
                for (uint64_t seen : order) TASK_CHECK(seen != r.id);
                order.push_back(r.id);
            }
            if (++guard > 100000) { TASK_CHECK(false); break; }
        }

        TASK_CHECK(order.size() == 3);
        TASK_CHECK(order[0] == 10 && order[1] == 20 && order[2] == 30);
        TASK_CHECK(batch.retainedCount() == 0);
        TASK_CHECK(batch.takeCompleted().empty()); // empty queue is a normal no-op

        const auto& st = batch.stats();
        TASK_CHECK(st.peak_ray_queue <= capacity);
        TASK_CHECK(st.peak_shade_queue <= capacity);
        TASK_CHECK(st.peak_shadow_queue <= capacity);
        TASK_CHECK(st.completed_paths == 3);
        return 0;
    }

    int transport_zero_capacity_throws()
    {
        auto j = baseScene();
        PathTracer tracer(j);
        bool threw = false;
        try { WavefrontBatch bad(tracer, 0); }
        catch (const std::invalid_argument&) { threw = true; }
        TASK_CHECK(threw);
        return 0;
    }

    // ------------------------------------------------------------------ visibility
    int visibility_direct_emissive_handcalc()
    {
        auto j = baseScene();
        PathTracer tracer(j);
        Ray ray = viewRay(tracer.scene);

        PairRun r = runPair(tracer, ray, frozenState(71u, 0u), 3, 1);
        TASK_CHECK(r.ok);
        double area = 4.0 * std::acos(-1.0);
        for (int c = 0; c < 3; ++c)
        {
            double expected = (c + 2.0) / area; // flux -> radiosity, direct view
            TASK_CHECK(std::abs(r.wavefront[c] - expected) < 1e-11);
            TASK_CHECK(r.wavefront[c] > 0.0);
        }
        TASK_CHECK(closeVec(r.scalar, r.wavefront));
        TASK_CHECK(r.stats.path_intersections >= 1);
        return 0;
    }

    // Diffuse receiver at the origin, an area light in its front (+z) hemisphere,
    // and an opaque wall placed between them. The camera ray hits the receiver once,
    // the single shadow ray is blocked, and nothing is reflected.
    nlohmann::json occluderScene(bool with_wall, const std::string& receiver_material)
    {
        auto j = nlohmann::json::parse(R"({
            "num_render_threads": 1,
            "materials": {
                "light": {"reflectance":[0,0,0], "specular_reflectance":[0,0,0], "emittance":[8,8,8]},
                "black": {"reflectance":[0,0,0], "specular_reflectance":[0,0,0]},
                "white": {"reflectance":[0.8,0.8,0.8], "specular_reflectance":[0.2,0.2,0.2]}
            },
            "surfaces": [
                {"type":"sphere", "radius":1.0, "material":"black", "position":[0,0,0]},
                {"type":"triangle", "material":"light",
                 "vertices":[[-1,3,2.5],[1,3,2.5],[0,3,4]]}
            ],
            "bvh": {"type":"BINARY_SAH", "bins_per_axis":8}
        })");
        j["surfaces"][0]["material"] = receiver_material;
        if (with_wall)
        {
            // Large horizontal wall at y = 1.5, between the receiver (y = 0 hit) and
            // the light (y = 3). Double-sided triangle intersection always registers.
            j["surfaces"].push_back({
                {"type","triangle"}, {"material","black"},
                {"vertices", {{-10,1.5,-10},{10,1.5,-10},{0,1.5,10}}}
            });
        }
        return j;
    }

    int visibility_fully_occluded_zero()
    {
        PathTracer tracer(occluderScene(true, "black"));
        Ray ray = viewRay(tracer.scene); // (0,0,3) -> -z, hits the receiver once

        PairRun r = runPair(tracer, ray, frozenState(81u, 0u), 1, 1);
        TASK_CHECK(r.ok);

        for (int c = 0; c < 3; ++c) TASK_CHECK(r.wavefront[c] == 0.0);
        TASK_CHECK(closeVec(r.scalar, r.wavefront));

        // Exact bookkeeping: one main ray, one blocked shadow ray, one completed path.
        TASK_CHECK(r.stats.path_intersections == 1);
        TASK_CHECK(r.stats.shadow_intersections == 1);
        TASK_CHECK(r.stats.completed_paths == 1);
        TASK_CHECK(r.stats.peak_shadow_queue == 1);
        return 0;
    }

    int visibility_area_light_mis_and_shadow_counts()
    {
        // Non-delta triangle area light above and in front of a diffuse receiver:
        // the direct term goes through the shadow queue with a light/BSDF MIS weight.
        auto j = nlohmann::json::parse(R"({
            "num_render_threads": 1,
            "materials": {
                "light": {"reflectance":[0,0,0], "specular_reflectance":[0,0,0], "emittance":[8,8,8]},
                "white": {"reflectance":[0.7,0.7,0.7], "specular_reflectance":[0.2,0.2,0.2]}
            },
            "surfaces": [
                {"type":"triangle", "material":"light", "vertices":[[-1,2,2],[1,2,2],[0,2,4]]},
                {"type":"sphere", "radius":1.0, "material":"white", "position":[0,0,0]}
            ],
            "bvh": {"type":"BINARY_SAH", "bins_per_axis":8}
        })");
        PathTracer tracer(j);
        // Horizontal ray strikes the front of the receiver at (0,0,2); the light is
        // above it in the front (+z) hemisphere, so a shadow ray is traced.
        Ray ray({ 0,0,3 }, { 0,0,-1 }, tracer.scene.ior);

        size_t shadow_seen = 0;
        for (uint32_t i = 0; i < 24; ++i)
        {
            PairRun r = runPair(tracer, ray, frozenState(91u, i), 6, 2);
            TASK_CHECK(r.ok);
            TASK_CHECK(closeVec(r.scalar, r.wavefront));
            if (r.stats.shadow_intersections > 0)
            {
                ++shadow_seen;
                TASK_CHECK(r.stats.peak_shadow_queue >= 1);
            }
        }
        TASK_CHECK(shadow_seen > 0);
        return 0;
    }

    int visibility_occluder_blocks_area_light()
    {
        Ray view({ 0.0, 0.0, 3.0 }, { 0.0, 0.0, -1.0 }, 1.0);

        // Without the wall the same light reaches the receiver: a non-zero direct
        // contribution delivered through exactly one visibility query.
        PathTracer open_tracer(occluderScene(false, "white"));
        PairRun open_run = runPair(open_tracer, view, frozenState(101u, 0u), 2, 1);
        TASK_CHECK(open_run.ok);
        TASK_CHECK(open_run.stats.shadow_intersections >= 1);
        TASK_CHECK(glm::compAdd(open_run.wavefront) > 0.0);
        TASK_CHECK(closeVec(open_run.scalar, open_run.wavefront));

        // With the wall the single shadow ray is blocked and the contribution vanishes.
        PathTracer blocked_tracer(occluderScene(true, "black"));
        PairRun blocked_run = runPair(blocked_tracer, view, frozenState(101u, 0u), 2, 1);
        TASK_CHECK(blocked_run.ok);
        TASK_CHECK(blocked_run.wavefront == glm::dvec3(0.0));
        TASK_CHECK(blocked_run.stats.shadow_intersections == 1);
        TASK_CHECK(closeVec(blocked_run.scalar, blocked_run.wavefront));
        return 0;
    }

    // ---------------------------------------------------------------------- camera
    nlohmann::json cameraScene(size_t wavefront_capacity, bool thin_lens)
    {
        auto j = baseScene();
        j["surfaces"].push_back({ {"type","sphere"}, {"radius",0.5}, {"material","white"}, {"position",{1.4,0,-1.4}} });
        j["surfaces"].push_back({ {"type","sphere"}, {"radius",0.5}, {"material","mirror"}, {"position",{-1.4,0,-1.4}} });
        j["surfaces"].push_back({ {"type","sphere"}, {"radius",0.5}, {"material","glass"}, {"position",{0,1.1,-2.0}} });

        nlohmann::json cam{
            {"focal_length", 35}, {"sensor_width", 35},
            {"eye", {0,0,3}}, {"forward",{0,0,-1}}, {"up",{0,1,0}},
            {"image", {{"width", 8}, {"height", 6}, {"plain", true}}},
            {"sqrtspp", 2}, {"savename", "wf-cam"},
            {"render_seed", FROZEN_RENDER_SEED},
            {"wavefront_capacity", wavefront_capacity}
        };
        if (thin_lens)
        {
            cam["f_stop"] = 2.0;
            cam["focus_distance"] = 3.0;
        }
        j["cameras"] = nlohmann::json::array({ cam });
        return j;
    }

    int camera_pixel_parity(bool thin_lens)
    {
        nlohmann::json scalar_json = cameraScene(0, thin_lens);
        nlohmann::json wave_json = cameraScene(4, thin_lens);

        Option opt({}, "cam", 0, false);
        Camera scalar_cam(scalar_json, opt);
        Camera wave_cam(wave_json, opt);

        TASK_CHECK(scalar_cam.thin_lens == thin_lens);
        TASK_CHECK(wave_cam.thin_lens == thin_lens);

        scalar_cam.sampleImage();
        wave_cam.sampleImage();

        TASK_CHECK(scalar_cam.image.width == wave_cam.image.width);
        TASK_CHECK(scalar_cam.image.height == wave_cam.image.height);

        for (size_t y = 0; y < scalar_cam.image.height; ++y)
        {
            for (size_t x = 0; x < scalar_cam.image.width; ++x)
            {
                TASK_CHECK(closeVec(scalar_cam.image(x, y), wave_cam.image(x, y)));
            }
        }
        return 0;
    }

    int camera_pinhole_parity() { return camera_pixel_parity(false); }
    int camera_thin_lens_parity() { return camera_pixel_parity(true); }

    int camera_multi_pixel_shared_batch()
    {
        // Capacity smaller than the number of samples forces several pixels to share
        // one batch (never one drained batch per pixel).
        nlohmann::json j = cameraScene(3, false);
        Option opt({}, "cam", 0, false);
        Camera cam(j, opt);
        cam.sampleImage();

        size_t illuminated = 0;
        for (size_t y = 0; y < cam.image.height; ++y)
            for (size_t x = 0; x < cam.image.width; ++x)
                if (glm::compAdd(cam.image(x, y)) > 0.0) ++illuminated;
        TASK_CHECK(illuminated > 1);
        return 0;
    }
}

extern const struct task_case task_cases[] = {
    { "contexts",   "state_roundtrip",                contexts_state_roundtrip },
    { "contexts",   "explicit_seed_matches",          contexts_explicit_seed_matches },
    { "contexts",   "step_restores_caller_sampler",   contexts_step_restores_caller_sampler },
    { "contexts",   "step_zero_advances_nothing",     contexts_step_zero_advances_nothing },
    { "contexts",   "interleaved_batches_isolated",   contexts_interleaved_batches_isolated },

    { "transport",  "matches_scalar_many_samples",    transport_matches_scalar_many_samples },
    { "transport",  "capacities_1_3_17",              transport_capacities_one_three_seventeen },
    { "transport",  "mixed_budgets_identical",        transport_mixed_budgets_identical },
    { "transport",  "specular_and_two_refractions",   transport_specular_and_two_refractions },
    { "transport",  "roulette_enclosed",              transport_roulette_enclosed },
    { "transport",  "full_queue_and_duplicate_id",    transport_full_queue_and_duplicate_id },
    { "transport",  "prefix_order_exact_once_bounds", transport_prefix_order_exact_once_and_bounds },
    { "transport",  "zero_capacity_throws",           transport_zero_capacity_throws },

    { "visibility", "direct_emissive_handcalc",       visibility_direct_emissive_handcalc },
    { "visibility", "fully_occluded_zero",            visibility_fully_occluded_zero },
    { "visibility", "area_light_mis_shadow_counts",   visibility_area_light_mis_and_shadow_counts },
    { "visibility", "occluder_blocks_area_light",     visibility_occluder_blocks_area_light },

    { "camera",     "pinhole_pixel_parity",           camera_pinhole_parity },
    { "camera",     "thin_lens_pixel_parity",         camera_thin_lens_parity },
    { "camera",     "multi_pixel_shared_batch",       camera_multi_pixel_shared_batch },
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
