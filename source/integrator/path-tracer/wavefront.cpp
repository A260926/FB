#include "wavefront.hpp"

#include <algorithm>

#include "path-tracer.hpp"

#include "../../scene/scene.hpp"
#include "../../surface/surface.hpp"
#include "../../material/material.hpp"
#include "../../sampling/sampling.hpp"
#include "../../common/util.hpp"
#include "../../common/constexpr-math.hpp"
#include "../../common/constants.hpp"

WavefrontBatch::WavefrontBatch(PathTracer& tracer, size_t capacity)
    : tracer_(tracer), capacity_(capacity), slots_(capacity)
{
    if (capacity == 0)
    {
        throw std::invalid_argument("WavefrontBatch capacity must be positive.");
    }

    free_slots_.reserve(capacity);
    for (size_t i = 0; i < capacity; ++i)
    {
        free_slots_.push_back(i);
    }
}

bool WavefrontBatch::submit(uint64_t id, const Ray& ray, const Sampler::State& sampler_state)
{
    // A duplicate id is only an error while that id is still retained.
    for (size_t retained : submission_order_)
    {
        if (slots_[retained]->id == id)
        {
            throw std::invalid_argument("WavefrontBatch received a duplicate retained id.");
        }
    }

    // Completed-but-untaken results occupy a slot, so this also covers that case.
    if (free_slots_.empty())
    {
        return false;
    }

    size_t slot_idx = free_slots_.back();
    free_slots_.pop_back();

    slots_[slot_idx] = std::make_unique<Slot>(ray);
    Slot& s = *slots_[slot_idx];
    s.id = id;
    s.phase = Phase::ACTIVE;
    s.rng = sampler_state;
    s.result.id = id;

    submission_order_.push_back(slot_idx);
    pushRay(slot_idx);
    return true;
}

size_t WavefrontBatch::step(size_t query_budget)
{
    if (query_budget == 0)
    {
        return 0;
    }

    // The calling thread's own sampler state must be untouched on return.
    Sampler::State outer_state = Sampler::capture();

    size_t used = 0;
    while (used < query_budget)
    {
        if (!ray_queue_.empty())
        {
            size_t slot_idx = ray_queue_.front();
            ray_queue_.pop_front();
            runRayTask(slot_idx);
            ++used; // one real Scene intersection
        }
        else if (!shadow_queue_.empty())
        {
            size_t slot_idx = shadow_queue_.front();
            shadow_queue_.pop_front();
            runShadowTask(slot_idx);
            ++used; // one real Scene intersection
        }
        else if (!shade_queue_.empty())
        {
            size_t slot_idx = shade_queue_.front();
            shade_queue_.pop_front();
            runShadeTask(slot_idx); // shading performs no Scene intersection
        }
        else
        {
            break;
        }
    }

    Sampler::restore(outer_state);
    return used;
}

std::vector<WavefrontBatch::Result> WavefrontBatch::takeCompleted()
{
    std::vector<Result> completed;

    // Only the completed prefix of the submission order is released.
    while (!submission_order_.empty())
    {
        size_t slot_idx = submission_order_.front();
        if (slots_[slot_idx]->phase != Phase::COMPLETE)
        {
            break;
        }

        submission_order_.pop_front();
        completed.push_back(slots_[slot_idx]->result);

        slots_[slot_idx].reset();
        free_slots_.push_back(slot_idx);
    }

    return completed;
}

size_t WavefrontBatch::retainedCount() const
{
    return submission_order_.size();
}

void WavefrontBatch::pushRay(size_t slot_idx)
{
    ray_queue_.push_back(slot_idx);
    stats_.peak_ray_queue = std::max(stats_.peak_ray_queue, ray_queue_.size());
}

void WavefrontBatch::pushShade(size_t slot_idx)
{
    shade_queue_.push_back(slot_idx);
    stats_.peak_shade_queue = std::max(stats_.peak_shade_queue, shade_queue_.size());
}

void WavefrontBatch::pushShadow(size_t slot_idx)
{
    shadow_queue_.push_back(slot_idx);
    stats_.peak_shadow_queue = std::max(stats_.peak_shadow_queue, shadow_queue_.size());
}

void WavefrontBatch::complete(size_t slot_idx)
{
    Slot& s = slot(slot_idx);
    s.phase = Phase::COMPLETE;
    s.result.id = s.id;
    s.result.radiance = s.radiance;
    ++stats_.completed_paths;
}

// Stage 1: main/continuation ray intersection. Mirrors the shuffle()+intersect()
// at the top of the scalar PathTracer::sampleRay loop.
void WavefrontBatch::runRayTask(size_t slot_idx)
{
    Slot& s = slot(slot_idx);
    Sampler::restore(s.rng);

    Sampler::shuffle();

    Intersection intersection = tracer_.scene.intersect(s.ray);
    ++stats_.path_intersections;

    s.rng = Sampler::capture();

    if (!intersection)
    {
        s.radiance += tracer_.scene.skyColor(s.ray) * s.throughput;
        complete(slot_idx);
        return;
    }

    s.pending_intersection = intersection;
    pushShade(slot_idx);
}

// Stage 2: material shading. Builds the Interaction, accounts for emissive hits and
// prepares at most one direct-light (shadow) sample. The next bounce is not started
// until that direct-light sample has been resolved.
void WavefrontBatch::runShadeTask(size_t slot_idx)
{
    Slot& s = slot(slot_idx);
    Sampler::restore(s.rng);

    s.interaction.emplace(s.pending_intersection, s.ray,
                          s.refraction_history.externalIOR(s.ray));
    const Interaction& interaction = *s.interaction;

    s.radiance += tracer_.sampleEmissive(interaction, s.ls) * s.throughput;

    // First half of Integrator::sampleDirect (everything before the shadow
    // intersection), split here so the visibility query becomes its own task.
    if (tracer_.scene.emissives.empty() || interaction.material->dirac_delta)
    {
        s.ls.light = nullptr;
    }
    else
    {
        auto u = Sampler::get<Dim::LIGHT, 3>();

        s.ls.light = tracer_.scene.selectLight(u[2], s.ls.select_probability);

        glm::dvec3 light_pos = (*s.ls.light)(u[0], u[1]);
        Ray shadow_ray(interaction.position + interaction.normal * C::EPSILON, light_pos);

        double cos_light_theta = glm::dot(-shadow_ray.direction, s.ls.light->normal(light_pos));

        if (cos_light_theta > 0.0)
        {
            double cos_theta = glm::dot(shadow_ray.direction, interaction.normal);
            bool enqueue = true;

            if (cos_theta <= 0.0)
            {
                if (interaction.material->opaque || cos_theta == 0.0)
                {
                    enqueue = false;
                }
                else
                {
                    // Try transmission
                    shadow_ray = Ray(interaction.position - interaction.normal * C::EPSILON, light_pos);
                }
            }

            if (enqueue)
            {
                s.shadow_ray.emplace(shadow_ray);
                s.shadow_light = s.ls.light;
                s.shadow_select_probability = s.ls.select_probability;
                s.shadow_cos_light_theta = cos_light_theta;
                s.has_shadow = true;
            }
        }
    }

    s.rng = Sampler::capture();

    if (s.has_shadow)
    {
        pushShadow(slot_idx);
    }
    else
    {
        continueBounce(slot_idx);
    }
}

// Stage 3: shadow/visibility intersection followed by the BSDF/MIS evaluation of
// the direct-light sample (second half of Integrator::sampleDirect), then the
// continuation that the scalar loop performs on the same bounce.
void WavefrontBatch::runShadowTask(size_t slot_idx)
{
    Slot& s = slot(slot_idx);
    Sampler::restore(s.rng);

    const Ray& shadow_ray = *s.shadow_ray;

    Intersection shadow_intersection = tracer_.scene.intersect(shadow_ray);
    ++stats_.shadow_intersections;

    glm::dvec3 direct(0.0);
    if (shadow_intersection && shadow_intersection.surface == s.shadow_light)
    {
        const Interaction& interaction = *s.interaction;

        double light_pdf = pow2(shadow_intersection.t) /
                           (s.shadow_light->area() * s.shadow_cos_light_theta);

        double bsdf_pdf;
        glm::dvec3 bsdf_absIdotN;
        if (interaction.BSDF(bsdf_absIdotN, shadow_ray.direction, bsdf_pdf))
        {
            double mis_weight = powerHeuristic(light_pdf, bsdf_pdf);
            direct = mis_weight * bsdf_absIdotN * s.shadow_light->material->emittance /
                     (light_pdf * s.shadow_select_probability);
        }
    }

    s.radiance += direct * s.throughput;

    s.has_shadow = false;
    s.shadow_ray.reset();
    s.shadow_light.reset();

    s.rng = Sampler::capture();
    continueBounce(slot_idx);
}

// BSDF sampling, throughput update, Russian roulette and refraction bookkeeping,
// i.e. the tail of one scalar loop iteration after sampleEmissive/sampleDirect.
void WavefrontBatch::continueBounce(size_t slot_idx)
{
    Slot& s = slot(slot_idx);
    Sampler::restore(s.rng);

    glm::dvec3 bsdf_absIdotN;
    if (!s.interaction->sampleBSDF(bsdf_absIdotN, s.ls.bsdf_pdf, s.ray))
    {
        s.rng = Sampler::capture();
        complete(slot_idx);
        return;
    }

    s.throughput *= bsdf_absIdotN / s.ls.bsdf_pdf;

    if (tracer_.absorb(s.ray, s.throughput))
    {
        s.rng = Sampler::capture();
        complete(slot_idx);
        return;
    }

    s.refraction_history.update(s.ray);
    s.interaction.reset();

    s.rng = Sampler::capture();
    pushRay(slot_idx);
}
