#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include "../integrator.hpp"

namespace Surface { class Base; }
class Material;

/******************************************************************************
Vertex Connection and Merging (VCM).

Progressive integrator. beginIteration(k) discards the previous light path pool
and spatial index, emits exactly N = light_paths new light paths with the finite
radius

    r_k = radius * (k + 1)^((alpha - 1) / 2)

and rebuilds the merging index at that radius. The first camera intersection is
shared by every strategy.

==============================================================================
Density convention (see README.md for the derivation)
==============================================================================
All path densities are expressed in *area measure*.  A light subpath
x_0 (emitter) -> x_1 -> ... -> x_t has density

    p_L(t) = p_select * p_area(x_0) * prod_{j=0}^{t-1}
                 [ pdf_dir(x_j -> x_{j+1}) * |cos(x_{j+1})| / d(x_j,x_{j+1})^2 ]

where pdf_dir is the solid-angle density used to sample the outgoing direction and
the cosine is the one at the *hit* vertex.  An eye subpath y_0 (camera) -> y_1 ->
... -> y_s has density

    p_E(s) = prod_{j=1}^{s-1}
                 [ pdf_dir(y_j -> y_{j+1}) * |cos(y_{j+1})| / d(y_j,y_{j+1})^2 ]

(the camera/pixel density is a common factor and is omitted).

The light-side transport coefficient stored at x_t is

    beta_t = W_L(x_t) = (light contribution to x_t) / p_L(t)

For the emitter endpoint x_0 this is beta_0 = Le * cos_0 / (p_select p_area pdf_dir_0).
For the first scatter vertex beta_1 = beta_0, and for t >= 2
beta_t = beta_{t-1} * f(x_{t-1}) cos(x_{t-1}) / pdf_dir(x_{t-1}).

The three strategies and their generalized densities are

  * eye emission         p_EE = p_E(s)             (n = 1)
  * connection           p_C  = p_E(s) * p_L(t)    (n = 1)
  * merging              p_M  = eta * p_C          (n = N), eta = N pi r_k^2

Every strategy weight is its sample count times generalized density divided by the
sum over all enabled and feasible strategies (balance heuristic).  A strategy whose
seam falls on a delta (mirror) vertex, or outside the receiving hemisphere, has
weight zero.

Connection estimator:     W_E(s) f(y_s) G(y_s,x_t) [f(x_t)] beta_t
Merging estimator:        W_E(s) f(y_s) beta_t k(r) / N,    k(r) = 1/(pi r^2)
(For a connection to the emitter endpoint the light side uses Le/(p_select p_area)
instead of f(x_t) beta_t, i.e. standard next-event estimation.)

The estimator uses the standard finite-radius, *consistent* VCM construction. It
is not claimed to be strictly unbiased at finite r.

==============================================================================
Supported subset
==============================================================================
sphere; triangle without interpolated vertex normals; pure Lambert; perfect
mirror; finite area emissive surfaces. Transparent, rough, complex-IOR materials
and all other surface types are rejected with std::invalid_argument.
******************************************************************************/
class VertexCM : public Integrator
{
public:
    enum class Mode
    {
        Combined,
        Connection,
        Merging
    };

    struct Config
    {
        size_t light_paths = 0;
        size_t max_vertices = 0;
        double radius = 0.0;
        double alpha = 0.0;
        size_t iterations = 0;
        uint32_t seed = 0;
        Mode mode = Mode::Combined;
    };

    struct LightVertex
    {
        glm::dvec3 position = glm::dvec3(0.0);
        glm::dvec3 normal = glm::dvec3(0.0);   // oriented toward the incoming ray
        glm::dvec3 incoming = glm::dvec3(0.0); // propagation direction that hit the vertex

        // Connection coefficient: beta_t = Le/(p_select p_area) times the
        // product of (cos_source/pdf_dir) and the surface BRDFs up to x_t.
        // The connection contribution is W_E * f(y_s) * G(y_s,x_t) * beta_t.
        glm::dvec3 beta = glm::dvec3(0.0);


        double p_forward = 0.0; // p_L prefix area density (includes p_select p_area)
        double p_reverse = 0.0; // p_L prefix with all directions exchanged
        double p_select = 0.0;
        double p_area = 0.0;
        double p_dir = 0.0;

        // Lambert BRDF * (1/pi) at the vertex (zero for a mirror).
        glm::dvec3 brdf = glm::dvec3(0.0);

        bool delta = false;
        bool emissive = false;

        uint32_t path = 0;
        uint32_t index = 0;

        std::shared_ptr<Surface::Base> surface;
        Ray ray = Ray(glm::dvec3(0.0), glm::dvec3(0.0, 0.0, 1.0), 1.0);
        double t = 0.0;
    };

    struct EyeVertex
    {
        glm::dvec3 position = glm::dvec3(0.0);
        glm::dvec3 normal = glm::dvec3(0.0);
        glm::dvec3 incoming = glm::dvec3(0.0);

        // Eye throughput W_E (BRDF*cos/pdf products for preceding segments).
        glm::dvec3 throughput = glm::dvec3(1.0);
        glm::dvec3 brdf = glm::dvec3(0.0);

        double p_forward = 1.0; // p_E prefix area density (diagnostic)
        double p_reverse = 1.0;
        double p_dir = 0.0;     // source direction pdf at this vertex

        bool delta = false;
        bool emissive = false;

        std::shared_ptr<Surface::Base> surface;
        Ray ray = Ray(glm::dvec3(0.0), glm::dvec3(0.0, 0.0, 1.0), 1.0);
        double t = 0.0;
    };

    struct StrategyStats
    {
        size_t candidates = 0;
        size_t feasible = 0;
        size_t accepted = 0;
        double n_samples = 0.0;
        double p_forward_sum = 0.0;
        double p_reverse_sum = 0.0;
        double eta = 0.0;
        double mis_weight_sum = 0.0;
        glm::dvec3 unweighted = glm::dvec3(0.0);
        glm::dvec3 weighted = glm::dvec3(0.0);
    };

    struct ConnectionRecord
    {
        size_t eye_vertex = 0;
        size_t light_vertex = 0;
        size_t light_path = 0;
        bool emitter = false;

        bool visible = false;
        bool delta_seam = false;
        bool hemisphere = false;
        bool accepted = false;

        double distance = 0.0;
        double cos_eye = 0.0;
        double cos_light = 0.0;

        double p_eye = 0.0;
        double p_light_forward = 0.0;
        double p_light_reverse = 0.0;
        double p_connection = 0.0;
        double p_merging = 0.0;
        double p_eye_emission_alt = 0.0;

        double n_connection = 1.0;
        double n_merging = 1.0;
        double eta = 0.0;

        double mis_weight = 0.0;
        glm::dvec3 unweighted = glm::dvec3(0.0);
        glm::dvec3 weighted = glm::dvec3(0.0);
    };

    struct MergeRecord
    {
        size_t eye_vertex = 0;
        size_t light_vertex = 0;
        size_t light_path = 0;

        double distance = 0.0;
        double normal_dot = 0.0;
        bool accepted = false;
        bool used_before = false;
        bool delta_vertex = false;
        bool wrong_hemisphere = false;
        bool visible = false;

        double p_eye = 0.0;
        double p_light_forward = 0.0;
        double p_light_reverse = 0.0;
        double kernel = 0.0;
        double p_connection = 0.0;
        double p_merging = 0.0;

        double n_merging = 1.0;
        double n_connection = 1.0;
        double eta = 0.0;

        double mis_weight = 0.0;
        glm::dvec3 unweighted = glm::dvec3(0.0);
        glm::dvec3 weighted = glm::dvec3(0.0);
    };

    struct EyeHitRecord
    {
        size_t eye_vertex = 0;
        bool direct = false;
        double p_eye = 0.0;
        double p_connection_alt = 0.0;
        double n_eye_emission = 1.0;
        double mis_weight = 1.0;
        glm::dvec3 unweighted = glm::dvec3(0.0);
        glm::dvec3 weighted = glm::dvec3(0.0);
    };

    struct SampleDiagnostics
    {
        size_t iteration = 0;
        double radius = 0.0;
        double eta = 0.0;
        size_t eye_vertices = 0;
        size_t merge_candidate_checks = 0;
        size_t merge_candidates = 0;
        size_t merge_accepted = 0;
        size_t merge_used = 0;
        bool records_truncated = false;

        StrategyStats connection;
        StrategyStats merging;
        StrategyStats eye_emission;

        std::vector<ConnectionRecord> connections;
        std::vector<MergeRecord> merges;
        std::vector<EyeHitRecord> eye_hits;
    };

    struct Diagnostics
    {
        size_t iteration = 0;
        double radius = 0.0;
        size_t light_paths = 0;
        size_t light_vertices = 0;
        size_t index_entries = 0;
        size_t connection_candidates = 0;
        size_t connection_visible = 0;
        size_t connection_used = 0;
        size_t merge_checks = 0;
        size_t merge_candidates = 0;
        size_t merge_accepted = 0;
        size_t merge_used = 0;
        double eta = 0.0;
    };

    /**************************************************************************
    Uniform grid used for radius queries. The cell size is twice the search
    radius, so a query only examines the (at most) eight cells overlapping the
    query sphere. Rebuilt from scratch every iteration with the light pool.
    **************************************************************************/
    class VertexIndex
    {
    public:
        void build(const std::vector<glm::dvec3>& positions, double radius);
        void clear();

        template <class Callback>
        void query(const glm::dvec3& p, double radius, Callback&& callback) const
        {
            last_checks_ = 0;
            if (cells_.empty() || !(radius > 0.0)) return;

            glm::ivec3 lo = cellOf(p - glm::dvec3(radius));
            glm::ivec3 hi = cellOf(p + glm::dvec3(radius));

            for (int z = lo.z; z <= hi.z; ++z)
                for (int y = lo.y; y <= hi.y; ++y)
                    for (int x = lo.x; x <= hi.x; ++x)
                    {
                        auto it = cells_.find(glm::ivec3(x, y, z));
                        if (it == cells_.end()) continue;
                        for (uint32_t id : it->second)
                        {
                            ++last_checks_;
                            ++total_checks_;
                            const glm::dvec3 d = positions_[id] - p;
                            double d2 = glm::dot(d, d);
                            if (d2 <= radius * radius) callback(id, d2);
                        }
                    }
        }

        size_t size() const { return positions_.size(); }
        double cellSize() const { return cell_size_; }
        size_t lastCandidateChecks() const { return last_checks_; }
        size_t totalCandidateChecks() const { return total_checks_; }
        void resetCheckCounter() { total_checks_ = 0; }
        const glm::dvec3& position(uint32_t id) const { return positions_[id]; }

    private:
        struct KeyHash
        {
            size_t operator()(const glm::ivec3& k) const
            {
                size_t h = static_cast<size_t>(static_cast<uint32_t>(k.x)) * 73856093u;
                h ^= static_cast<size_t>(static_cast<uint32_t>(k.y)) * 19349663u;
                h ^= static_cast<size_t>(static_cast<uint32_t>(k.z)) * 83492791u;
                return h;
            }
        };

        glm::ivec3 cellOf(const glm::dvec3& p) const
        {
            return glm::ivec3(
                static_cast<int>(std::floor(p.x / cell_size_)),
                static_cast<int>(std::floor(p.y / cell_size_)),
                static_cast<int>(std::floor(p.z / cell_size_)));
        }

        std::vector<glm::dvec3> positions_;
        std::unordered_map<glm::ivec3, std::vector<uint32_t>, KeyHash> cells_;
        double cell_size_ = 1.0;
        mutable size_t last_checks_ = 0;
        mutable size_t total_checks_ = 0;
    };

    explicit VertexCM(const nlohmann::json& j);

    glm::dvec3 sampleRay(Ray ray) override;
    glm::dvec3 sampleRay(Ray ray, uint32_t sample_id) override;
    glm::dvec3 sampleRay(Ray ray, uint32_t pixel_id, uint32_t sample_id) override;
    void beginIteration(size_t k) override;
    void advanceIteration() override;
    size_t iterations() const override { return config_.iterations; }
    bool iterative() const override { return true; }
    bool deterministicSamples() const override { return true; }
    void initSample(uint32_t) const override { }
    void initSample(uint32_t pixel_id, uint32_t sample_id) const override;

    const Config& config() const { return config_; }
    Mode mode() const { return config_.mode; }
    bool connectionEnabled() const { return config_.mode != Mode::Merging; }
    bool mergingEnabled() const { return config_.mode != Mode::Connection; }
    size_t currentIteration() const { return iteration_; }
    double currentRadius() const { return radius_k_; }
    size_t lightPathCount() const { return config_.light_paths; }

    const std::vector<LightVertex>& lightPool() const { return pool_; }
    const std::vector<std::pair<uint32_t, uint32_t>>& lightPathRanges() const { return path_ranges_; }
    const VertexIndex& lightIndex() const { return index_; }
    const std::vector<EyeVertex>& lastEyePath() const { return last_eye_path_; }
    const SampleDiagnostics& lastSampleDiagnostics() const { return last_sample_; }
    const Diagnostics& diagnostics() const { return diagnostics_; }

    static Config parseConfig(const nlohmann::json& j);
    static constexpr double PI = 3.14159265358979323846;
    static double kernel(double radius) { return 1.0 / (PI * radius * radius); }
    static double eta(size_t n_paths, double radius)
    {
        return static_cast<double>(n_paths) * PI * radius * radius;
    }
    static double areaDensity(double pdf_solid_angle, double cos_target, double distance2)
    {
        if (!(pdf_solid_angle > 0.0) || !(cos_target > 0.0) || !(distance2 > 0.0)) return 0.0;
        return pdf_solid_angle * cos_target / distance2;
    }
    static bool validSeam(bool a_delta, bool b_delta) { return !a_delta && !b_delta; }
    // Balance heuristic: n_s p_s / sum(n_i p_i); contenders are (n_i, p_i).
    static double balanceWeight(double n_s, double p_s,
                                const std::vector<std::pair<double, double>>& contenders);
    static uint32_t hashIdentity(uint32_t a, uint32_t b, uint32_t c, uint32_t tag);

private:
    void validateScene() const;

    void emitLightPaths();
    void traceLightPath(size_t path_index);
    void computeLightReverseDensities(std::vector<LightVertex>& path) const;

    void traceEyePath(const Ray& ray, uint32_t pixel_id, uint32_t sample_id);
    void processSample(uint32_t sample_id);

    void processConnection(size_t eye_idx, size_t light_idx);
    void processMerges(size_t eye_idx);
    void processEyeHit(size_t eye_idx);
    bool mergeFeasibleAt(size_t eye_idx) const;

    bool visible(const glm::dvec3& a, const glm::dvec3& na,
                 const glm::dvec3& b, const glm::dvec3& nb) const;

    Config config_;
    size_t iteration_ = 0;
    double radius_k_ = 0.0;
    bool started_ = false;

    std::vector<LightVertex> pool_;
    std::vector<std::pair<uint32_t, uint32_t>> path_ranges_;
    VertexIndex index_;
    std::vector<EyeVertex> last_eye_path_;

    SampleDiagnostics last_sample_;
    Diagnostics diagnostics_;

    std::vector<char> photon_used_;
    std::vector<char> merge_feasible_;
};
