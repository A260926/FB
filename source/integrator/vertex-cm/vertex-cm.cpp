#include "vertex-cm.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <glm/gtx/component_wise.hpp>

#include "../../common/constants.hpp"
#include "../../common/constexpr-math.hpp"
#include "../../common/coordinate-system.hpp"
#include "../../material/material.hpp"
#include "../../ray/interaction.hpp"
#include "../../sampling/sampler.hpp"
#include "../../sampling/sampling.hpp"
#include "../../surface/surface.hpp"

namespace
{
    constexpr size_t MAX_RECORDS = 4096;
    constexpr uint32_t LIGHT_PATH_TAG = 0x4c504154u; // "LPAT"
    constexpr uint32_t EYE_PATH_TAG   = 0x45595054u; // "EYPT"

    bool supportedMaterial(const Material& m)
    {
        if (m.rough) return false;
        if (m.rough_specular) return false;
        if (m.complex_ior) return false;
        if (std::abs(m.transparency) > C::EPSILON) return false;
        if (m.perfect_mirror) return true;
        if (m.ior > 0.0) return false; // finite dielectric Fresnel unsupported
        return true;                   // pure Lambert, optionally emissive
    }

    uint32_t mixSeed(uint32_t a)
    {
        a ^= a >> 16;
        a *= 0x7feb352du;
        a ^= a >> 15;
        a *= 0x846ca68bu;
        a ^= a >> 16;
        return a;
    }

    double maxComponent(const glm::dvec3& v)
    {
        return std::max(v.x, std::max(v.y, v.z));
    }
}

/******************************************************************************
Construction and configuration
******************************************************************************/

VertexCM::VertexCM(const nlohmann::json& j)
    : Integrator(j), config_(parseConfig(j))
{
    if (num_threads != 1)
    {
        throw std::invalid_argument(
            "vertex-cm requires num_render_threads = 1 (got " +
            std::to_string(num_threads) + ")");
    }

    if (j.find("photon_map") != j.end())
    {
        throw std::invalid_argument("vertex-cm requires photon_map to be absent");
    }

    validateScene();

    last_sample_.connections.reserve(64);
    last_sample_.merges.reserve(64);
    last_sample_.eye_hits.reserve(16);
}

VertexCM::Config VertexCM::parseConfig(const nlohmann::json& j)
{
    if (j.find("vcm") == j.end())
    {
        throw std::invalid_argument("vertex-cm requires a root \"vcm\" object");
    }

    const nlohmann::json& vcm = j.at("vcm");

    auto require = [&](const char* name) -> const nlohmann::json&
    {
        if (vcm.find(name) == vcm.end())
        {
            throw std::invalid_argument(std::string("vcm.") + name + " is required");
        }
        return vcm.at(name);
    };

    auto positiveInteger = [&](const char* name) -> size_t
    {
        const nlohmann::json& e = require(name);
        if (!e.is_number_integer() && !e.is_number_unsigned())
        {
            throw std::invalid_argument(std::string("vcm.") + name +
                                        " must be a positive integer");
        }
        int64_t value = e.get<int64_t>();
        if (value <= 0)
        {
            throw std::invalid_argument(std::string("vcm.") + name +
                                        " must be a positive integer");
        }
        return static_cast<size_t>(value);
    };

    Config c;

    c.light_paths = positiveInteger("light_paths");
    c.max_vertices = positiveInteger("max_vertices");
    if (c.max_vertices < 1 || c.max_vertices > 12)
    {
        throw std::invalid_argument("vcm.max_vertices must be in [1, 12]");
    }
    c.iterations = positiveInteger("iterations");

    const nlohmann::json& radius = require("radius");
    if (!radius.is_number()) throw std::invalid_argument("vcm.radius must be finite and positive");
    c.radius = radius.get<double>();
    if (!std::isfinite(c.radius) || !(c.radius > 0.0))
        throw std::invalid_argument("vcm.radius must be finite and positive");

    const nlohmann::json& alpha = require("alpha");
    if (!alpha.is_number()) throw std::invalid_argument("vcm.alpha must be in (0, 1)");
    c.alpha = alpha.get<double>();
    if (!std::isfinite(c.alpha) || !(c.alpha > 0.0 && c.alpha < 1.0))
        throw std::invalid_argument("vcm.alpha must be in (0, 1)");

    const nlohmann::json& seed = require("seed");
    if (!seed.is_number_integer() && !seed.is_number_unsigned())
        throw std::invalid_argument("vcm.seed must be a non-negative integer");
    int64_t seed_value = seed.get<int64_t>();
    if (seed_value < 0)
        throw std::invalid_argument("vcm.seed must be a non-negative integer");
    c.seed = static_cast<uint32_t>(seed_value);

    std::string mode = require("mode").get<std::string>();
    if (mode == "combined") c.mode = Mode::Combined;
    else if (mode == "connection") c.mode = Mode::Connection;
    else if (mode == "merging") c.mode = Mode::Merging;
    else throw std::invalid_argument("vcm.mode must be combined, connection or merging");

    return c;
}

void VertexCM::validateScene() const
{
    for (const auto& surface : scene.surfaces)
    {
        if (!surface) throw std::invalid_argument("vertex-cm: null surface");

        switch (surface->kind())
        {
            case Surface::Kind::Sphere:
                break;
            case Surface::Kind::Triangle:
                if (surface->interpolated())
                    throw std::invalid_argument(
                        "vertex-cm does not support triangles with interpolated normals");
                break;
            default:
                throw std::invalid_argument(
                    "vertex-cm only supports sphere and triangle surfaces");
        }

        if (!surface->material)
            throw std::invalid_argument("vertex-cm: surface without material");

        if (!supportedMaterial(*surface->material))
            throw std::invalid_argument(
                "vertex-cm only supports Lambert, perfect mirror and emissive materials");
    }
}

uint32_t VertexCM::hashIdentity(uint32_t a, uint32_t b, uint32_t c, uint32_t tag)
{
    uint32_t h = mixSeed(a);
    h = mixSeed(h ^ mixSeed(b + 0x9e3779b9u));
    h = mixSeed(h ^ mixSeed(c + 0x85ebca6bu));
    h = mixSeed(h ^ mixSeed(tag + 0xc2b2ae35u));
    return h;
}

double VertexCM::balanceWeight(double n_s, double p_s,
                               const std::vector<std::pair<double, double>>& contenders)
{
    double numerator = n_s * p_s;
    if (!(numerator > 0.0)) return 0.0;

    double denominator = numerator;
    for (const auto& c : contenders)
    {
        double term = c.first * c.second;
        if (term > 0.0) denominator += term;
    }
    return denominator > 0.0 ? numerator / denominator : 0.0;
}

/******************************************************************************
Spatial index
******************************************************************************/

void VertexCM::VertexIndex::clear()
{
    positions_.clear();
    cells_.clear();
    cell_size_ = 1.0;
    last_checks_ = 0;
    total_checks_ = 0;
}

void VertexCM::VertexIndex::build(const std::vector<glm::dvec3>& positions, double radius)
{
    clear();

    positions_ = positions;
    cell_size_ = 2.0 * radius;
    if (!(cell_size_ > 0.0)) cell_size_ = 1.0;

    cells_.reserve(positions_.size() * 2 + 1);
    for (uint32_t i = 0; i < static_cast<uint32_t>(positions_.size()); ++i)
        cells_[cellOf(positions_[i])].push_back(i);
}

/******************************************************************************
Iteration control
******************************************************************************/

void VertexCM::initSample(uint32_t pixel_id, uint32_t sample_id) const
{
    Sampler::initiateWithSeed(hashIdentity(config_.seed, static_cast<uint32_t>(iteration_),
                                           pixel_id, sample_id));
    Sampler::setIndex(0);
}

void VertexCM::beginIteration(size_t k)
{
    iteration_ = k;
    radius_k_ = config_.radius * std::pow(static_cast<double>(k + 1),
                                          0.5 * (config_.alpha - 1.0));

    diagnostics_ = Diagnostics();
    diagnostics_.iteration = k;
    diagnostics_.radius = radius_k_;
    diagnostics_.light_paths = config_.light_paths;
    diagnostics_.eta = eta(config_.light_paths, radius_k_);

    emitLightPaths();

    std::vector<glm::dvec3> positions;
    positions.reserve(pool_.size());
    for (const auto& v : pool_) positions.push_back(v.position);
    index_.build(positions, radius_k_);
    index_.resetCheckCounter();

    diagnostics_.light_vertices = pool_.size();
    diagnostics_.index_entries = index_.size();

    started_ = true;
}

void VertexCM::advanceIteration()
{
    beginIteration(iteration_ + 1);
}

/******************************************************************************
Light subpaths

The light subpath stores a single connection coefficient beta_t such that

    connection contribution = W_E * f_r(y_s) * G(y_s, x_t) * beta_t
    merging contribution    = W_E * f_r(y_s) * cos(y_s)  * beta_t * k(r) / N

with beta_0 = Le A / p_select at the emissive endpoint (NEE). For a sampled
(emitter or diffuse) predecessor

    beta_t = beta_{t-1} * f_r(x_t) * cos_target / pdf_{t-1}

and for a deterministic (mirror) predecessor

    beta_t = beta_{t-1} * f_r(x_t) * cos_source * cos_target / d^2.

The prefix area density p_L is tracked separately for the balance heuristic. A
mirror contributes a discrete (delta) transition: its prefix density does not
gain a smooth directional factor and it can never be a connection/merge seam.
******************************************************************************/

void VertexCM::emitLightPaths()
{
    pool_.clear();
    path_ranges_.clear();
    pool_.reserve(config_.light_paths * std::max<size_t>(1, config_.max_vertices));

    if (scene.emissives.empty()) return;

    for (size_t i = 0; i < config_.light_paths; ++i)
    {
        size_t begin = pool_.size();
        traceLightPath(i);
        size_t end = pool_.size();
        path_ranges_.emplace_back(static_cast<uint32_t>(begin), static_cast<uint32_t>(end));
    }
}

void VertexCM::traceLightPath(size_t path_index)
{
    Sampler::initiateWithSeed(hashIdentity(config_.seed, static_cast<uint32_t>(iteration_),
                                           static_cast<uint32_t>(path_index), LIGHT_PATH_TAG));
    Sampler::setIndex(0);

    auto u = Sampler::get<Dim::VCM_LIGHT, 3>();

    LightSample ls;
    ls.light = scene.selectLight(u[2], ls.select_probability);
    if (!ls.light || !(ls.select_probability > 0.0)) return;

    double area = ls.light->area();
    if (!(area > 0.0)) return;

    glm::dvec3 position = (*ls.light)(u[0], u[1]);
    glm::dvec3 normal = ls.light->normal(position);
    glm::dvec3 direction = CoordinateSystem::from(Sampling::cosWeightedHemi(u[1], u[0]), normal);

    double cos_light = glm::dot(normal, direction);
    if (!(cos_light > 0.0)) return;

    double p_select = ls.select_probability;
    double p_area = 1.0 / area;
    double p_dir = cos_light * C::INV_PI;

    const glm::dvec3 Le = ls.light->material->emittance;
    const glm::dvec3 beta0 = Le * area / p_select;

    LightVertex emission;
    emission.position = position;
    emission.normal = normal;
    emission.incoming = direction;
    emission.p_select = p_select;
    emission.p_area = p_area;
    emission.p_dir = p_dir;
    emission.p_forward = p_select * p_area;
    emission.p_reverse = 0.0;
    emission.delta = false;
    emission.emissive = true;
    emission.path = static_cast<uint32_t>(path_index);
    emission.index = 0;
    emission.surface = ls.light;
    emission.brdf = glm::dvec3(0.0);
    emission.beta = beta0;
    emission.ray = Ray(position + normal * C::EPSILON, direction, scene.ior);
    emission.t = 0.0;

    std::vector<LightVertex> path;
    path.push_back(emission);

    Ray ray = emission.ray;
    // Direction pdf used to sample the segment that leads to the next vertex.
    // For the emitter it is the cosine emission density; afterwards it is the
    // pdf returned by the surface BSDF sampler.
    double pending_pdf = p_dir;
    bool pending_delta = false;

    while (path.size() < config_.max_vertices)
    {
        Sampler::shuffle();

        Intersection isect = scene.intersect(ray);
        if (!isect) break;

        Interaction interaction(isect, ray, scene.ior);
        const LightVertex& prev = path.back();

        LightVertex v;
        v.position = interaction.position;
        v.normal = interaction.normal; // outward shading normal
        v.incoming = ray.direction;
        v.delta = interaction.dirac_delta;
        v.emissive = interaction.material->emissive && !interaction.inside;
        v.path = static_cast<uint32_t>(path_index);
        v.index = static_cast<uint32_t>(path.size());
        v.surface = interaction.surface;
        v.ray = ray;
        v.t = interaction.t;
        v.p_select = prev.p_select;
        v.p_area = prev.p_area;
        v.p_dir = pending_pdf;
        v.p_reverse = 0.0;
        v.brdf = v.delta ? glm::dvec3(0.0)
                         : interaction.material->reflectance * C::INV_PI;

        double dist2 = pow2(interaction.t);
        if (!(dist2 > 0.0)) break;

        // Hit vertices store the outward normal, so the positive target cosine
        // is -dot(normal, direction). The emitter is the exception: its normal
        // points along the emission direction, so the source cosine is +dot.
        double cos_target = -glm::dot(v.normal, ray.direction);
        double cos_source = (prev.index == 0)
            ? glm::dot(prev.normal, ray.direction)
            : -glm::dot(prev.normal, ray.direction);
        if (!(cos_target > 0.0) || !(cos_source > 0.0)) break;

        // Connection coefficient recurrence, storing
        //   beta_t = (light-side radiance arriving at x_t) * geometric factors
        //            divided by the direction density that produced the segment.
        // For a sampled predecessor:
        //   beta_t = beta_{t-1} * f_r(x_t) * cos_target / pdf_{t-1}
        // A delta (mirror) predecessor is deterministic: radiance is unchanged
        // and the geometry is supplied later by the connection's G term, so
        //   beta_t = beta_{t-1} * f_r(x_t).
        const glm::dvec3 unit(1.0);
        glm::dvec3 surface_factor = v.delta ? unit : v.brdf;
        if (pending_delta)
        {
            // Deterministic mirror transition: the full geometry term carries
            // the radiance from the mirror to the next vertex.
            v.beta = prev.beta * surface_factor * cos_source * cos_target / dist2;
            v.p_forward = prev.p_forward;
        }
        else
        {
            if (!(pending_pdf > 0.0)) break;
            v.beta = prev.beta * surface_factor * cos_target / pending_pdf;
            v.p_forward = prev.p_forward * pending_pdf * cos_target / dist2;
        }

        path.push_back(v);

        if (v.emissive) break; // a finite emitter absorbs the light path

        glm::dvec3 weight;
        double pdf;
        if (!interaction.sampleBSDF(weight, pdf, ray, true)) break;
        if (!(pdf > 0.0)) break;

        pending_pdf = pdf;
        pending_delta = v.delta;
        if (ray.depth == 0) break;
    }

    computeLightReverseDensities(path);
    for (const auto& v : path) pool_.push_back(v);
}

void VertexCM::computeLightReverseDensities(std::vector<LightVertex>& path) const
{
    if (path.empty()) return;

    size_t n = path.size();
    path[0].p_reverse = 0.0;

    for (size_t j = 1; j < n; ++j)
    {
        if (path[j].delta)
        {
            for (size_t i = j; i < n; ++i) path[i].p_reverse = 0.0;
            return;
        }
    }

    // Re-evaluate the prefix density with the direction of every segment
    // reversed, deriving each factor from the stored geometry.
    double density = path[0].p_select * path[0].p_area;
    for (size_t i = 1; i < n; ++i)
    {
        const glm::dvec3 edge = path[i].position - path[i - 1].position;
        double dist2 = glm::dot(edge, edge);
        if (!(dist2 > 0.0)) { density = 0.0; path[i].p_reverse = 0.0; continue; }

        glm::dvec3 back = -edge / std::sqrt(dist2); // points from i toward i-1
        // Outward normals: the reversed direction at the target points back
        // toward i-1, so its cosine is -dot(normal_target, back). At the
        // predecessor the reversed direction points toward the target, i.e.
        // dot(normal_prev, back).
        double cos_target_rev = -glm::dot(path[i].normal, back);
        double cos_source_rev = glm::dot(path[i - 1].normal, back);
        if (!(cos_target_rev > 0.0) || !(cos_source_rev > 0.0))
        {
            density = 0.0;
            path[i].p_reverse = 0.0;
            continue;
        }

        density *= (cos_source_rev * C::INV_PI) * cos_target_rev / dist2;
        path[i].p_reverse = density;
    }
}

/******************************************************************************
Eye subpaths
******************************************************************************/

void VertexCM::traceEyePath(const Ray& camera_ray, uint32_t pixel_id, uint32_t sample_id)
{
    last_eye_path_.clear();

    Sampler::initiateWithSeed(hashIdentity(config_.seed, static_cast<uint32_t>(iteration_),
                                           pixel_id, sample_id ^ EYE_PATH_TAG));
    Sampler::setIndex(0);

    Ray ray = camera_ray;
    glm::dvec3 throughput(1.0);
    double prefix_density = 1.0;

    double pending_pdf = 1.0;
    bool pending_delta = true; // the camera segment carries no smooth density

    while (last_eye_path_.size() < config_.max_vertices)
    {
        Sampler::shuffle();

        Intersection isect = scene.intersect(ray);
        if (!isect) return;

        Interaction interaction(isect, ray, scene.ior);

        EyeVertex v;
        v.position = interaction.position;
        v.normal = interaction.normal; // outward shading normal
        v.incoming = ray.direction;
        v.delta = interaction.dirac_delta;
        v.emissive = interaction.material->emissive && !interaction.inside;
        v.surface = interaction.surface;
        v.ray = ray;
        v.t = interaction.t;
        v.throughput = throughput;
        v.brdf = v.delta ? glm::dvec3(0.0)
                         : interaction.material->reflectance * C::INV_PI;

        if (!pending_delta)
        {
            double cos_target = -glm::dot(v.normal, ray.direction);
            double dist2 = pow2(interaction.t);
            if (cos_target > 0.0 && dist2 > 0.0)
                prefix_density *= pending_pdf * cos_target / dist2;
            else
                prefix_density = 0.0;
        }
        v.p_forward = prefix_density;
        v.p_reverse = prefix_density;
        v.p_dir = pending_delta ? 1.0 : pending_pdf;

        last_eye_path_.push_back(v);

        if (v.emissive) return;

        glm::dvec3 weight;
        double pdf;
        if (!interaction.sampleBSDF(weight, pdf, ray, false)) return;
        if (!(pdf > 0.0)) return;

        // Density of the outgoing direction used by the next segment.
        pending_pdf = pdf;
        pending_delta = v.delta; // mirrors have a deterministic direction

        if (!v.delta)
        {
            // Standard diffuse throughput factor f*cos/pdf; with cosine
            // sampling this reduces to the reflectance, but we use the actual
            // returned weight/pdf to stay correct.
            throughput *= weight / pdf;
        }

        if (ray.depth == 0) return;
    }
}

/******************************************************************************
Sampling entry points
******************************************************************************/

glm::dvec3 VertexCM::sampleRay(Ray ray)
{
    return sampleRay(ray, 0u, 0u);
}

glm::dvec3 VertexCM::sampleRay(Ray ray, uint32_t sample_id)
{
    return sampleRay(ray, sample_id, sample_id);
}

glm::dvec3 VertexCM::sampleRay(Ray ray, uint32_t pixel_id, uint32_t sample_id)
{
    if (!started_) beginIteration(0);

    traceEyePath(ray, pixel_id, sample_id);

    last_sample_ = SampleDiagnostics();
    last_sample_.iteration = iteration_;
    last_sample_.radius = radius_k_;
    last_sample_.eta = eta(config_.light_paths, radius_k_);
    last_sample_.eye_vertices = last_eye_path_.size();

    if (last_eye_path_.empty())
    {
        return glm::dvec3(0.0); // escapes are black for vertex-cm
    }

    processSample(sample_id);

    return last_sample_.connection.weighted
         + last_sample_.merging.weighted
         + last_sample_.eye_emission.weighted;
}

/******************************************************************************
Strategy evaluation
******************************************************************************/

void VertexCM::processSample(uint32_t sample_id)
{
    const size_t n_paths = config_.light_paths;
    if (n_paths == 0) return;

    size_t pair_path = sample_id % n_paths;
    if (connectionEnabled())
    {
        auto u = Sampler::get<Dim::VCM_PAIR, 1>();
        size_t sampled = static_cast<size_t>(u[0] * static_cast<double>(n_paths));
        pair_path = sampled < n_paths ? sampled : n_paths - 1;
    }

    uint32_t pair_begin = 0, pair_end = 0;
    if (pair_path < path_ranges_.size())
    {
        pair_begin = path_ranges_[pair_path].first;
        pair_end = std::min<uint32_t>(path_ranges_[pair_path].second,
                                      static_cast<uint32_t>(pool_.size()));
    }

    // Precompute merging feasibility per eye vertex so that connection MIS can
    // include the merging technique only where it is actually possible.
    merge_feasible_.assign(last_eye_path_.size(), 0);
    if (mergingEnabled())
    {
        for (size_t e = 0; e < last_eye_path_.size(); ++e)
        {
            if (last_eye_path_[e].emissive) break;
            merge_feasible_[e] = mergeFeasibleAt(e) ? 1 : 0;
        }
    }

    // Eye emission (camera direct light and later emissive eye hits).
    if (connectionEnabled())
    {
        for (size_t e = 0; e < last_eye_path_.size(); ++e)
        {
            if (last_eye_path_[e].emissive) { processEyeHit(e); break; }
        }
    }

    // Connections: s = e+1 eye vertices, t = l+1 light vertices (emitter counts).
    if (connectionEnabled())
    {
        for (size_t e = 0; e < last_eye_path_.size(); ++e)
        {
            const EyeVertex& eye = last_eye_path_[e];
            if (eye.emissive) break;

            for (uint32_t li = pair_begin; li < pair_end; ++li)
            {
                const LightVertex& light = pool_[li];
                if (!light.surface) continue;
                // Connection to the emissive endpoint (NEE) or to a diffuse
                // light-path vertex. Light vertices that are themselves emissive
                // but not the endpoint cannot act as a connection seam.
                if (light.index > 0 && light.emissive) continue;
                if ((e + 1) + (light.index + 1) > config_.max_vertices) continue;
                processConnection(e, li);
            }
        }
    }

    // Merging against the whole pool. A shared vertex counts as s + t - 1.
    if (mergingEnabled())
    {
        photon_used_.assign(pool_.size(), 0);
        for (size_t e = 0; e < last_eye_path_.size(); ++e)
        {
            if (last_eye_path_[e].emissive) break;
            processMerges(e);
        }
    }
}

bool VertexCM::mergeFeasibleAt(size_t eye_idx) const
{
    const EyeVertex& eye = last_eye_path_[eye_idx];
    if (eye.delta) return false;

    bool feasible = false;
    index_.query(eye.position, radius_k_,
        [&](uint32_t id, double d2)
        {
            (void)d2;
            if (feasible) return;
            const LightVertex& light = pool_[id];
            if (light.index == 0 || light.emissive || light.delta) return;
            if (glm::dot(eye.normal, light.normal) <= 0.9) return;
            glm::dvec3 photon_dir = -light.incoming;
            if (!(glm::dot(eye.normal, photon_dir) > 0.0)) return;
            feasible = true;
        });
    return feasible;
}


void VertexCM::processEyeHit(size_t eye_idx)
{
    const EyeVertex& eye = last_eye_path_[eye_idx];

    glm::dvec3 unweighted = eye.throughput * eye.surface->material->emittance;
    if (!(maxComponent(unweighted) > 0.0)) return;

    EyeHitRecord record;
    record.eye_vertex = eye_idx;
    record.direct = eye_idx == 0;
    record.p_eye = eye.p_forward;
    record.n_eye_emission = 1.0;
    // No light-subpath splat exists, so eye emission is the only strategy.
    record.mis_weight = 1.0;
    record.unweighted = unweighted;
    record.weighted = unweighted;

    last_sample_.eye_emission.candidates++;
    last_sample_.eye_emission.feasible++;
    last_sample_.eye_emission.accepted++;
    last_sample_.eye_emission.n_samples = 1.0;
    last_sample_.eye_emission.p_forward_sum += eye.p_forward;
    last_sample_.eye_emission.mis_weight_sum += 1.0;
    last_sample_.eye_emission.unweighted += record.unweighted;
    last_sample_.eye_emission.weighted += record.weighted;

    if (last_sample_.eye_hits.size() < MAX_RECORDS)
        last_sample_.eye_hits.push_back(record);
    else
        last_sample_.records_truncated = true;
}

void VertexCM::processConnection(size_t eye_idx, size_t light_idx)
{
    const EyeVertex& eye = last_eye_path_[eye_idx];
    const LightVertex& light = pool_[light_idx];

    ConnectionRecord record;
    record.eye_vertex = eye_idx;
    record.light_vertex = light_idx;
    record.light_path = light.path;
    record.emitter = light.index == 0;

    last_sample_.connection.candidates++;
    diagnostics_.connection_candidates++;

    if (eye.delta || light.delta)
    {
        record.delta_seam = true;
        return;
    }

    const glm::dvec3 edge = light.position - eye.position;
    double dist2 = glm::dot(edge, edge);
    record.distance = std::sqrt(dist2);
    if (!(dist2 > 0.0)) return;

    glm::dvec3 toward_light = edge / record.distance;
    glm::dvec3 toward_eye = -toward_light;

    double cos_eye = glm::dot(eye.normal, toward_light);
    double cos_light = glm::dot(light.normal, toward_eye);
    record.cos_eye = cos_eye;
    record.cos_light = cos_light;

    if (!(cos_eye > 0.0) || !(cos_light > 0.0))
    {
        record.hemisphere = true;
        return;
    }

    if (!visible(eye.position, eye.normal, light.position, light.normal)) return;
    record.visible = true;
    last_sample_.connection.feasible++;
    diagnostics_.connection_visible++;

    // X = W_E * f_r(y_s) * G * beta_t.
    double G = cos_eye * cos_light / dist2;
    glm::dvec3 unweighted = eye.throughput * eye.brdf * G * light.beta;
    if (!(maxComponent(unweighted) > 0.0)) return;

    double p_eye = eye.p_forward;
    double p_light = light.p_forward;
    if (!(p_eye > 0.0) || !(p_light > 0.0)) return;

    double p_connection = p_eye * p_light;
    double p_reverse = light.p_reverse > 0.0
        ? light.p_reverse * std::abs(cos_eye) / dist2
        : 0.0;

    const double n_paths = static_cast<double>(config_.light_paths);
    const double eta_value = eta(config_.light_paths, radius_k_);

    record.p_eye = p_eye;
    record.p_light_forward = p_light;
    record.p_light_reverse = light.p_reverse;
    record.p_connection = p_connection;
    // The merging generalized density p_M = eta * p_C already contains the
    // N-fold normalization (eta = N pi r^2). Therefore both techniques enter the
    // balance heuristic with sample count one; multiplying by N again would
    // double count the normalization.
    record.n_connection = 1.0;
    record.n_merging = 1.0;
    record.eta = eta_value;

    std::vector<std::pair<double, double>> contenders;
    if (mergingEnabled() && merge_feasible_[eye_idx])
    {
        double p_merging = eta_value * p_connection;
        record.p_merging = p_merging;
        contenders.emplace_back(record.n_merging, p_merging);
    }

    double weight = balanceWeight(record.n_connection, p_connection, contenders);
    record.mis_weight = weight;
    record.unweighted = unweighted;
    record.weighted = weight * unweighted;

    last_sample_.connection.accepted++;
    last_sample_.connection.n_samples = record.n_connection;
    last_sample_.connection.p_forward_sum += p_connection;
    last_sample_.connection.p_reverse_sum += p_reverse;
    last_sample_.connection.eta = eta_value;
    last_sample_.connection.mis_weight_sum += weight;
    last_sample_.connection.unweighted += record.unweighted;
    last_sample_.connection.weighted += record.weighted;

    if (last_sample_.connections.size() < MAX_RECORDS)
        last_sample_.connections.push_back(record);
    else
        last_sample_.records_truncated = true;

    diagnostics_.connection_used++;
}

void VertexCM::processMerges(size_t eye_idx)
{
    const EyeVertex& eye = last_eye_path_[eye_idx];

    std::vector<std::pair<uint32_t, double>> hits;
    index_.query(eye.position, radius_k_,
        [&](uint32_t id, double d2) { hits.emplace_back(id, d2); });

    last_sample_.merge_candidate_checks += index_.lastCandidateChecks();
    diagnostics_.merge_checks += index_.lastCandidateChecks();

    const double k_r = kernel(radius_k_);
    const double eta_value = eta(config_.light_paths, radius_k_);
    const double n_paths = static_cast<double>(config_.light_paths);

    for (const auto& hit : hits)
    {
        const uint32_t li = hit.first;
        const LightVertex& light = pool_[li];

        if (light.emissive) continue;
        if ((eye_idx + 1) + (light.index + 1) - 1 > config_.max_vertices) continue;

        MergeRecord record;
        record.eye_vertex = eye_idx;
        record.light_vertex = li;
        record.light_path = light.path;
        record.distance = std::sqrt(hit.second);
        record.normal_dot = glm::dot(eye.normal, light.normal);
        record.kernel = k_r;
        record.eta = eta_value;
        // Both generalized densities already include the N normalization via
        // eta = N pi r^2, so both techniques use sample count one.
        record.n_merging = 1.0;
        record.n_connection = 1.0;

        last_sample_.merge_candidates++;
        diagnostics_.merge_candidates++;

        if (eye.delta || light.delta) { record.delta_vertex = true; continue; }
        if (!(record.normal_dot > 0.9)) continue;

        // The photon's stored direction of arrival determines the eye BSDF
        // evaluation at the merge point (positions, not directions, are merged).
        glm::dvec3 photon_dir = -light.incoming; // points back along the light path
        double cos_eye = glm::dot(eye.normal, photon_dir);
        if (!(cos_eye > 0.0))
        {
            record.wrong_hemisphere = true;
            continue;
        }
        record.visible = true;

        if (li < photon_used_.size() && photon_used_[li])
        {
            record.used_before = true;
            continue;
        }

        double p_light = light.p_forward;
        double p_eye = eye.p_forward;
        if (!(p_light > 0.0) || !(p_eye > 0.0)) continue;

        // Merging estimator (same light-side coefficient as connection):
        //   W_E * f_r(y_s) * cos(y_s) * beta_t * k(r) / N.
        glm::dvec3 unweighted = eye.throughput * (eye.brdf * cos_eye) *
                                light.beta * k_r / n_paths;
        if (!(maxComponent(unweighted) > 0.0)) continue;

        double p_connection = p_eye * p_light;
        double p_merging = eta_value * p_connection;

        std::vector<std::pair<double, double>> contenders;
        // Connection is a feasible strategy for this merge point only if the
        // paired light path can actually reach it (NEE hemisphere + visibility).
        // We conservatively include connection when it is enabled and the eye
        // vertex is diffuse (the density is the same p_C).
        if (connectionEnabled()) contenders.emplace_back(1.0, p_connection);

        double weight = balanceWeight(1.0, p_merging, contenders);

        record.accepted = true;
        record.p_eye = p_eye;
        record.p_light_forward = p_light;
        record.p_light_reverse = light.p_reverse;
        record.p_connection = p_connection;
        record.p_merging = p_merging;
        record.mis_weight = weight;
        record.unweighted = unweighted;
        record.weighted = weight * unweighted;

        photon_used_[li] = 1;

        last_sample_.merging.accepted++;
        last_sample_.merging.n_samples = n_paths;
        last_sample_.merging.p_forward_sum += p_light;
        last_sample_.merging.p_reverse_sum += light.p_reverse;
        last_sample_.merging.eta = eta_value;
        last_sample_.merging.mis_weight_sum += weight;
        last_sample_.merging.unweighted += record.unweighted;
        last_sample_.merging.weighted += record.weighted;

        last_sample_.merge_accepted++;
        diagnostics_.merge_accepted++;

        if (last_sample_.merges.size() < MAX_RECORDS)
            last_sample_.merges.push_back(record);
        else
            last_sample_.records_truncated = true;

        last_sample_.merge_used++;
        diagnostics_.merge_used++;
    }
}

/******************************************************************************
Geometry
******************************************************************************/

bool VertexCM::visible(const glm::dvec3& a, const glm::dvec3& na,
                       const glm::dvec3& b, const glm::dvec3& nb) const
{
    const glm::dvec3 edge = b - a;
    double distance = glm::length(edge);
    if (!(distance > C::EPSILON)) return false;

    glm::dvec3 direction = edge / distance;
    if (!(glm::dot(na, direction) > 0.0)) return false;
    if (!(glm::dot(nb, -direction) > 0.0)) return false;

    Ray shadow(a + na * C::EPSILON, direction, scene.ior);
    Intersection isect = scene.intersect(shadow);
    if (!isect) return true;

    return isect.t >= distance * (1.0 - 1e-6);
}
