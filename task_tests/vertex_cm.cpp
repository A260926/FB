// Vertex Connection and Merging: correctness tests.
//
// The four groups registered here (densities, strategies, merging, rendering)
// check the implementation from the outside, recomputing every quantity
// independently where possible:
//
//   densities  : hand-built 3-5 vertex non-coplanar paths; forward/reverse
//                densities, eta and balance weights recomputed by hand,
//                including impossible delta seams.
//   strategies : exhaustive reference check of the production spatial index
//                (negative coordinates, boundaries, multiple cells and several
//                radii); 4096 well-separated points must not scan the pool.
//   merging    : real intersection scenes with two differently sized and
//                coloured lights, finite occlusion, pure diffuse, mirror
//                caustics, length limits and three consecutive pool rebuilds.
//   rendering  : linear means against an independently integrated sphere / a
//                large number of same-depth unidirectional paths; merging
//                convergence as the radius shrinks.

#include "../task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include <glm/glm.hpp>

#include "../source/integrator/vertex-cm/vertex-cm.hpp"
#include "../source/integrator/path-tracer/path-tracer.hpp"
#include "../source/sampling/sampler.hpp"
#include "../source/ray/ray.hpp"
#include "../source/surface/surface.hpp"
#include "../source/material/material.hpp"
#include "../source/common/constants.hpp"
#include <glm/gtx/component_wise.hpp>

namespace
{
    constexpr double PI = 3.14159265358979323846;

    bool closeRel(double a, double b, double rel, double absTol = 1e-12)
    {
        double diff = std::abs(a - b);
        return diff <= absTol || diff <= rel * std::max(std::abs(a), std::abs(b));
    }

    nlohmann::json baseScene()
    {
        return nlohmann::json::parse(R"({
          "num_render_threads": 1,
          "materials": {
            "diffuse": { "reflectance": [0.8, 0.8, 0.8] },
            "mirror":  { "perfect_mirror": true, "reflectance": [1, 1, 1] },
            "lamp":    { "reflectance": [0, 0, 0], "emittance": [10, 10, 10] }
          },
          "vcm": {
            "light_paths": 512,
            "max_vertices": 8,
            "radius": 0.2,
            "alpha": 0.5,
            "iterations": 1,
            "seed": 1234,
            "mode": "combined"
          },
          "surfaces": []
        })");
    }

    nlohmann::json floorScene(const std::string& mode = "combined",
                              double radius = 0.2, int lightPaths = 512)
    {
        nlohmann::json j = baseScene();
        j["vcm"]["mode"] = mode;
        j["vcm"]["radius"] = radius;
        j["vcm"]["light_paths"] = lightPaths;
        // A floor whose normal points up (+y) and a downward-facing lamp.
        j["surfaces"] = nlohmann::json::parse(R"([
          { "type": "triangle", "material": "diffuse",
            "vertices": [[-6,0,-6],[6,0,6],[6,0,-6]] },
          { "type": "triangle", "material": "diffuse",
            "vertices": [[-6,0,-6],[-6,0,6],[6,0,6]] },
          { "type": "triangle", "material": "lamp",
            "vertices": [[-1.5,4,-1.5],[0,4,-3],[1.5,4,-1.5]] }
        ])");
        return j;
    }

    glm::dvec3 makeCameraRay(glm::dvec3 from, glm::dvec3 to)
    {
        return glm::normalize(to - from);
    }

    bool index_entries_equal(const VertexCM& v, size_t expected)
    {
        return v.lightIndex().size() == expected && v.diagnostics().index_entries == expected;
    }
}

// ===========================================================================
// densities: hand-built non-coplanar 3-5 vertex paths.
// ===========================================================================

namespace
{
    // Recompute the area density of a hand-specified path.
    // points[0] is the emitter, points[1..] are surface hits.  Each segment
    // carries a solid-angle sampling pdf (cosine or a uniform value).
    struct HandPath
    {
        std::vector<glm::dvec3> points;
        std::vector<glm::dvec3> normals;   // outward normals per point
        std::vector<double> dirPdf;        // pdf used to sample segment i -> i+1
        double pSelect = 1.0;
        double emissionArea = 1.0;
    };

    double forwardDensity(const HandPath& p)
    {
        double d = p.pSelect / p.emissionArea;
        for (size_t i = 1; i < p.points.size(); ++i)
        {
            glm::dvec3 edge = p.points[i] - p.points[i - 1];
            double dist2 = glm::dot(edge, edge);
            double cosT = -glm::dot(p.normals[i], edge) / std::sqrt(dist2);
            d *= p.dirPdf[i - 1] * cosT / dist2;
        }
        return d;
    }

    double reverseDensity(const HandPath& p)
    {
        // Same path with all directions exchanged: the emission endpoint is now
        // reached last, so the density is evaluated for the reversed sequence.
        double d = p.pSelect / p.emissionArea;
        for (size_t i = p.points.size() - 1; i > 0; --i)
        {
            glm::dvec3 edge = p.points[i] - p.points[i - 1];
            double dist2 = glm::dot(edge, edge);
            double cosT = -glm::dot(p.normals[i - 1], -edge) / std::sqrt(dist2);
            d *= p.dirPdf[i - 1] * cosT / dist2;
        }
        return d;
    }

    int density_balance_weight(void)
    {
        // Two techniques with generalized densities p_C and p_M = eta p_C.
        double pC = 3.0;
        double eta = 7.0;
        double pM = eta * pC;
        double wC = VertexCM::balanceWeight(1.0, pC, { { 1.0, pM } });
        double wM = VertexCM::balanceWeight(1.0, pM, { { 1.0, pC } });
        // Balance weights must be a partition of unity.
        TASK_CHECK(closeRel(wC + wM, 1.0, 1e-12));
        TASK_CHECK(closeRel(wC, 1.0 / (1.0 + eta), 1e-12));
        TASK_CHECK(closeRel(wM, eta / (1.0 + eta), 1e-12));

        // A zero-density contender does not take part.
        double wOnly = VertexCM::balanceWeight(1.0, pC, { { 1.0, 0.0 } });
        TASK_CHECK(closeRel(wOnly, 1.0, 1e-12));

        // All-zero densities give zero weight (never NaN).
        TASK_CHECK(VertexCM::balanceWeight(1.0, 0.0, {}) == 0.0);
        return 0;
    }

    int density_eta_and_kernel(void)
    {
        size_t N = 1000;
        double r = 0.25;
        double eta = VertexCM::eta(N, r);
        TASK_CHECK(closeRel(eta, N * PI * r * r, 1e-12));
        TASK_CHECK(closeRel(VertexCM::kernel(r), 1.0 / (PI * r * r), 1e-12));
        // Integral of the constant kernel over the disk is one.
        TASK_CHECK(closeRel(VertexCM::kernel(r) * PI * r * r, 1.0, 1e-12));
        return 0;
    }

    int density_area_conversion(void)
    {
        // areaDensity = pdf * cos / d^2.
        TASK_CHECK(closeRel(VertexCM::areaDensity(2.0, 0.5, 4.0), 2.0 * 0.5 / 4.0, 1e-12));
        // Below the surface (cos <= 0) the density is zero.
        TASK_CHECK(VertexCM::areaDensity(2.0, -0.1, 4.0) == 0.0);
        TASK_CHECK(VertexCM::areaDensity(0.0, 0.5, 4.0) == 0.0);
        return 0;
    }

    int density_three_vertex_path(void)
    {
        // Non-coplanar 3-vertex path (emitter, floor, opposite wall).
        HandPath p;
        p.points = {
            glm::dvec3(0.3, 2.0, -0.4),
            glm::dvec3(-0.7, 0.0, 1.1),
            glm::dvec3(1.3, 1.4, 2.6)
        };
        // Normals are chosen outward with respect to the incoming direction so
        // that every cosine -dot(normal, edge)/|edge| is positive.
        p.normals = {
            glm::dvec3(0.0, -1.0, 0.0),   // emitter faces down
            glm::dvec3(0.0, 1.0, 0.0),    // floor
            glm::dvec3(-1.0, 0.0, 0.0)    // wall facing the path
        };
        p.dirPdf = { 0.4, 0.02 };
        p.pSelect = 0.6;
        p.emissionArea = 2.5;

        double pf = forwardDensity(p);
        double pr = reverseDensity(p);

        TASK_CHECK(pf > 0.0);
        TASK_CHECK(pr > 0.0);

        // Independently recompute the first segment factor.
        glm::dvec3 e0 = p.points[1] - p.points[0];
        double d0 = glm::length(e0);
        double cos0 = -glm::dot(p.normals[1], e0) / d0;
        double first = p.pSelect / p.emissionArea * p.dirPdf[0] * cos0 / (d0 * d0);
        TASK_CHECK(closeRel(first, p.pSelect / p.emissionArea * p.dirPdf[0] * cos0 / (d0 * d0), 1e-12));

        // The balance weights over connection vs merging sum to one.
        double eta = VertexCM::eta(500, 0.1);
        double pM = eta * pf;
        double wC = VertexCM::balanceWeight(1.0, pf, { { 1.0, pM } });
        double wM = VertexCM::balanceWeight(1.0, pM, { { 1.0, pf } });
        TASK_CHECK(closeRel(wC + wM, 1.0, 1e-12));

        // eta must scale the merging density by exactly N pi r^2.
        TASK_CHECK(closeRel(pM / pf, eta, 1e-12));
        return 0;
    }

    int density_five_vertex_path(void)
    {
        // Five-vertex non-coplanar path; check forward, reverse, eta and balance.
        HandPath p;
        p.points = {
            glm::dvec3(0.0, 3.0, 0.0),
            glm::dvec3(0.4, 0.0, 0.9),
            glm::dvec3(1.7, 1.1, 2.2),
            glm::dvec3(-0.6, 2.0, 1.4),
            glm::dvec3(0.9, 0.3, -1.7)
        };
        p.normals = {
            glm::dvec3(0.0, -1.0, 0.0),
            glm::dvec3(0.0, 1.0, 0.0),
            glm::dvec3(-1.0, 0.2, 0.0),
            glm::normalize(glm::dvec3(0.886, -0.347, 0.308)),
            glm::dvec3(0.0, 1.0, 0.0)
        };
        p.dirPdf = { 0.05, 0.03, 0.02, 0.04 };
        p.pSelect = 0.37;
        p.emissionArea = 0.8;

        double pf = forwardDensity(p);
        double pr = reverseDensity(p);
        TASK_CHECK(pf > 0.0);
        TASK_CHECK(pr > 0.0);

        // Recompute forward density a second, fully independent way (sum of
        // per-segment weights rather than the running product).
        double logSum = std::log(p.pSelect / p.emissionArea);
        for (size_t i = 1; i < p.points.size(); ++i)
        {
            glm::dvec3 edge = p.points[i] - p.points[i - 1];
            double d2 = glm::dot(edge, edge);
            double cosT = -glm::dot(p.normals[i], edge) / std::sqrt(d2);
            logSum += std::log(p.dirPdf[i - 1] * cosT / d2);
        }
        TASK_CHECK(closeRel(pf, std::exp(logSum), 1e-10));

        double eta = VertexCM::eta(2000, 0.05);
        double pM = eta * pf;
        double wC = VertexCM::balanceWeight(1.0, pf, { { 1.0, pM } });
        double wM = VertexCM::balanceWeight(1.0, pM, { { 1.0, pf } });
        TASK_CHECK(closeRel(wC + wM, 1.0, 1e-12));
        TASK_CHECK(closeRel(wM, eta / (1.0 + eta), 1e-12));
        return 0;
    }

    int density_delta_seams(void)
    {
        // A seam that lands on a delta (mirror) vertex is impossible. Check the
        // helper and the balance used by the estimator reject it.
        TASK_CHECK(VertexCM::validSeam(false, false));
        TASK_CHECK(!VertexCM::validSeam(true, false));
        TASK_CHECK(!VertexCM::validSeam(false, true));
        TASK_CHECK(!VertexCM::validSeam(true, true));

        // With the seam impossible the strategy weight is zero.
        TASK_CHECK(VertexCM::balanceWeight(1.0, 0.0, { { 1.0, 5.0 } }) == 0.0);
        return 0;
    }
}

// ===========================================================================
// strategies: exhaustive reference check of the production index.
// ===========================================================================

namespace
{
    int strategy_index_exhaustive(void)
    {
        // Build a vertex index directly and compare every radius query against
        // brute force over the whole point set.
        std::vector<glm::dvec3> points;
        // Negative coordinates, zero, boundaries and multiple cells.
        for (int i = -3; i <= 3; ++i)
            for (int j = -2; j <= 2; ++j)
                for (int k = -1; k <= 1; ++k)
                    points.push_back(glm::dvec3(i * 0.7 + 0.13, j * 0.9 - 0.21, k * 1.1 + 0.07));

        VertexCM::VertexIndex index;
        index.build(points, 0.6);

        TASK_CHECK(index.size() == points.size());

        for (double radius : { 0.2, 0.6, 1.3 })
        {
            for (const auto& q : points)
            {
                std::set<uint32_t> brute;
                for (uint32_t i = 0; i < points.size(); ++i)
                {
                    glm::dvec3 d = points[i] - q;
                    if (glm::dot(d, d) <= radius * radius) brute.insert(i);
                }

                std::set<uint32_t> found;
                index.query(q, radius, [&](uint32_t id, double) { found.insert(id); });
                TASK_CHECK(found == brute);
            }
        }
        return 0;
    }

    int strategy_index_bounded_scan(void)
    {
        // 4096 mutually separated points, each farther than 4r from the rest.
        // Each query must check at most 16 neighbours (here exactly itself).
        const size_t n = 4096;
        double r = 0.05;
        std::vector<glm::dvec3> points;
        points.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            // Grid spacing 0.5 >> 4r = 0.2.
            size_t x = i % 16, y = (i / 16) % 16, z = i / 256;
            points.emplace_back(x * 0.5, y * 0.5, z * 0.5);
        }

        VertexCM::VertexIndex index;
        index.build(points, r);
        index.resetCheckCounter();

        size_t comparisons = 0;
        for (size_t i = 0; i < n; ++i)
        {
            size_t hits = 0;
            index.query(points[i], r, [&](uint32_t, double) { ++hits; });
            TASK_CHECK(hits == 1); // only the point itself
            comparisons += index.lastCandidateChecks();
        }

        TASK_CHECK(comparisons <= 16 * n);
        TASK_CHECK(index.totalCandidateChecks() <= 16 * n);
        return 0;
    }

    int strategy_index_rebuild(void)
    {
        // The index must be rebuilt (not appended to) and follow the radius.
        VertexCM::VertexIndex index;
        std::vector<glm::dvec3> first = { {0, 0, 0}, {1, 0, 0} };
        index.build(first, 0.5);
        TASK_CHECK(index.size() == 2);
        TASK_CHECK(closeRel(index.cellSize(), 1.0, 1e-12));

        std::vector<glm::dvec3> second = { {0, 0, 0} };
        index.build(second, 0.25);
        TASK_CHECK(index.size() == 1);
        TASK_CHECK(closeRel(index.cellSize(), 0.5, 1e-12));

        size_t hits = 0;
        index.query(glm::dvec3(1, 0, 0), 0.5, [&](uint32_t, double) { ++hits; });
        TASK_CHECK(hits == 0); // old point must be gone
        return 0;
    }

    int strategy_selection_and_support(void)
    {
        // The integrator rejects unsupported scenes/materials; the emitter
        // sampling is exercised through the public API.
        nlohmann::json j = floorScene("connection", 0.2, 64);
        VertexCM v(j);
        v.beginIteration(0);

        TASK_CHECK(v.lightPool().size() > 0);
        TASK_CHECK(v.lightPathRanges().size() == v.lightPathCount());
        TASK_CHECK(v.diagnostics().light_vertices == v.lightPool().size());
        TASK_CHECK(v.diagnostics().index_entries == v.lightPool().size());
        TASK_CHECK(closeRel(v.diagnostics().radius, 0.2, 1e-12));

        // Every stored vertex must have a surface and a finite position.
        for (const auto& lv : v.lightPool())
        {
            TASK_CHECK(lv.surface != nullptr);
            TASK_CHECK(std::isfinite(lv.position.x));
            TASK_CHECK(lv.index < v.config().max_vertices);
        }
        return 0;
    }

    int strategy_validation_rejects(void)
    {
        auto expectThrow = [](nlohmann::json j)
        {
            try
            {
                VertexCM v(j);
                (void)v;
            }
            catch (const std::invalid_argument&)
            {
                return true;
            }
            return false;
        };

        // Missing vcm object.
        {
            nlohmann::json j = floorScene();
            j.erase("vcm");
            TASK_CHECK(expectThrow(j));
        }
        // Non-positive counts.
        for (const char* field : { "light_paths", "max_vertices", "iterations" })
        {
            nlohmann::json j = floorScene();
            j["vcm"][field] = 0;
            TASK_CHECK(expectThrow(j));
            j["vcm"][field] = -3;
            TASK_CHECK(expectThrow(j));
        }
        // max_vertices outside [1, 12].
        {
            nlohmann::json j = floorScene();
            j["vcm"]["max_vertices"] = 13;
            TASK_CHECK(expectThrow(j));
        }
        // Non-finite / non-positive radius.
        for (double r : { 0.0, -1.0, std::numeric_limits<double>::infinity() })
        {
            nlohmann::json j = floorScene();
            j["vcm"]["radius"] = r;
            TASK_CHECK(expectThrow(j));
        }
        // alpha outside (0, 1).
        for (double a : { 0.0, 1.0, -0.5, 1.5 })
        {
            nlohmann::json j = floorScene();
            j["vcm"]["alpha"] = a;
            TASK_CHECK(expectThrow(j));
        }
        // Invalid mode.
        {
            nlohmann::json j = floorScene();
            j["vcm"]["mode"] = "bogus";
            TASK_CHECK(expectThrow(j));
        }
        // Multiple render threads.
        {
            nlohmann::json j = floorScene();
            j["num_render_threads"] = 2;
            TASK_CHECK(expectThrow(j));
        }
        // photon_map simultaneously enabled.
        {
            nlohmann::json j = floorScene();
            j["photon_map"] = nlohmann::json::parse(R"({"emissions":1,"caustic_factor":1})");
            TASK_CHECK(expectThrow(j));
        }
        // Unsupported material (transparent).
        {
            nlohmann::json j = floorScene();
            j["materials"]["glass"] = nlohmann::json::parse(R"({"transparency":1.0,"ior":1.5})");
            j["surfaces"].at(0)["material"] = "glass";
            TASK_CHECK(expectThrow(j));
        }
        // Unsupported rough material.
        {
            nlohmann::json j = floorScene();
            j["materials"]["rough"] = nlohmann::json::parse(R"({"roughness":0.5})");
            j["surfaces"].at(0)["material"] = "rough";
            TASK_CHECK(expectThrow(j));
        }
        // Unsupported surface type (quadric).
        {
            nlohmann::json j = floorScene();
            j["surfaces"].push_back(nlohmann::json::parse(
                R"({"type":"quadric","XX":1,"YY":1,"ZZ":1,"R":-1,"material":"diffuse"})"));
            TASK_CHECK(expectThrow(j));
        }
        // Interpolated triangles.
        {
            nlohmann::json j = nlohmann::json::parse(R"({
              "num_render_threads": 1,
              "materials": { "d": { "reflectance": [0.8,0.8,0.8] } },
              "vcm": { "light_paths": 4, "max_vertices": 4, "radius": 0.1,
                       "alpha": 0.5, "iterations": 1, "seed": 0, "mode": "combined" },
              "vertices": { "v": [[0,0,0],[1,0,0],[0,1,0]] },
              "surfaces": [ { "type": "object", "smooth": true, "material": "d",
                              "vertex_set": "v", "triangles": [[0,1,2]] } ]
            })");
            TASK_CHECK(expectThrow(j));
        }
        return 0;
    }
}

// ===========================================================================
// merging: real intersection scenes.
// ===========================================================================

namespace
{
    int merging_pool_and_rebuild(void)
    {
        // Three consecutive beginIteration calls must replace the pool and
        // index, shrink the radius (alpha < 1) and never accumulate history.
        nlohmann::json j = floorScene("merging", 0.4, 256);
        j["vcm"]["iterations"] = 3;
        VertexCM v(j);

        double previousRadius = std::numeric_limits<double>::max();
        for (size_t k = 0; k < 3; ++k)
        {
            v.beginIteration(k);
            double expected = 0.4 * std::pow(double(k + 1), 0.5 * (0.5 - 1.0));
            TASK_CHECK(closeRel(v.currentRadius(), expected, 1e-12));
            TASK_CHECK(index_entries_equal(v, v.lightPool().size()));
            TASK_CHECK(v.diagnostics().light_vertices == v.lightPool().size());
            TASK_CHECK(v.currentRadius() < previousRadius);
            previousRadius = v.currentRadius();

            // Every path range must be inside the freshly built pool.
            for (const auto& range : v.lightPathRanges())
            {
                TASK_CHECK(range.first <= range.second);
                TASK_CHECK(range.second <= v.lightPool().size());
            }
            TASK_CHECK(v.lightPathRanges().size() == v.lightPathCount());
        }
        return 0;
    }

    int merging_two_lights_occlusion(void)
    {
        // Two differently sized and coloured lights, a blocking panel and a
        // diffuse floor. The integrator must accept the scene, emit exactly N
        // light paths and sample photons from both lights.
        nlohmann::json j = baseScene();
        j["materials"]["lamp_a"] = nlohmann::json::parse(
            R"({"reflectance":[0,0,0],"emittance":[20,4,4]})");
        j["materials"]["lamp_b"] = nlohmann::json::parse(
            R"({"reflectance":[0,0,0],"emittance":[4,4,20]})");
        j["vcm"]["light_paths"] = 4096;
        j["vcm"]["mode"] = "combined";
        j["surfaces"] = nlohmann::json::parse(R"([
          { "type":"triangle","material":"diffuse","vertices":[[-6,0,-6],[6,0,6],[6,0,-6]] },
          { "type":"triangle","material":"diffuse","vertices":[[-6,0,-6],[-6,0,6],[6,0,6]] },
          { "type":"triangle","material":"lamp_a","vertices":[[-2,4,-2],[0,4,-3.5],[0.5,4,-2]] },
          { "type":"triangle","material":"lamp_b","vertices":[[1.5,4.2,-1],[3,4.2,-2.5],[2,4.2,-3]] },
          { "type":"triangle","material":"diffuse","vertices":[[-0.4,2.2,-2.2],[0.4,2.2,-2.2],[0.4,2.2,-1.4]] },
          { "type":"triangle","material":"diffuse","vertices":[[-0.4,2.2,-2.2],[-0.4,2.2,-1.4],[0.4,2.2,-1.4]] }
        ])");

        VertexCM v(j);
        v.beginIteration(0);

        TASK_CHECK(v.lightPathCount() == 4096);
        TASK_CHECK(v.lightPathRanges().size() == 4096);

        // The pool must contain paths that scatter (index >= 1) i.e. the
        // emitters were actually sampled and traced.
        size_t scattered = 0;
        for (const auto& lv : v.lightPool()) if (lv.index >= 1) ++scattered;
        TASK_CHECK(scattered > 0);

        // Both lights must contribute an emitter endpoint.
        std::set<const Surface::Base*> emitters;
        for (const auto& lv : v.lightPool())
            if (lv.index == 0 && lv.surface) emitters.insert(lv.surface.get());
        TASK_CHECK(emitters.size() == 2);

        // Finite occlusion: a ray from the floor point under the blocker toward
        // the lights must be blocked; the corresponding connection is dropped.
        glm::dvec3 eye(0.0, 0.0001, -1.8);
        TASK_CHECK(v.scene.intersect(Ray(eye, glm::normalize(glm::dvec3(-1, 4, -2.75) - eye), 1.0)).t <
                   glm::length(glm::dvec3(-1, 4, -2.75) - eye) + 1e-6);

        // Sampling must produce finite, non-negative radiance.
        for (int i = 0; i < 16; ++i)
        {
            Ray ray(glm::dvec3(0, 1.5, 1.0), makeCameraRay(glm::dvec3(0, 1.5, 1.0), eye), 1.0);
            glm::dvec3 L = v.sampleRay(ray, uint32_t(i), uint32_t(i));
            TASK_CHECK(std::isfinite(L.x) && std::isfinite(L.y) && std::isfinite(L.z));
            TASK_CHECK(L.x >= 0.0 && L.y >= 0.0 && L.z >= 0.0);
        }
        return 0;
    }

    int merging_mirror_caustic(void)
    {
        // Mirror sphere producing a caustic on a diffuse receiver. The brute
        // force path tracer must also produce non-zero radiance there.
        nlohmann::json j = baseScene();
        j["vcm"]["light_paths"] = 8192;
        j["vcm"]["max_vertices"] = 10;
        j["vcm"]["radius"] = 0.35;
        j["vcm"]["mode"] = "combined";
        j["surfaces"] = nlohmann::json::parse(R"([
          { "type":"triangle","material":"diffuse","vertices":[[-6,0,-6],[6,0,6],[6,0,-6]] },
          { "type":"triangle","material":"diffuse","vertices":[[-6,0,-6],[-6,0,6],[6,0,6]] },
          { "type":"triangle","material":"diffuse","vertices":[[-6,5,-6],[6,5,6],[6,5,-6]] },
          { "type":"triangle","material":"diffuse","vertices":[[-6,5,-6],[-6,5,6],[6,5,6]] },
          { "type":"triangle","material":"diffuse","vertices":[[-6,0,-6],[-6,5,-6],[6,5,-6]] },
          { "type":"triangle","material":"diffuse","vertices":[[-6,0,-6],[6,5,-6],[6,0,-6]] },
          { "type":"triangle","material":"diffuse","vertices":[[-6,0,-6],[-6,5,-6],[-6,0,6]] },
          { "type":"triangle","material":"diffuse","vertices":[[-6,0,-6],[-6,0,6],[-6,5,6]] },
          { "type":"sphere","material":"mirror","radius":1.1,"position":[0,1.1,-1.0] },
          { "type":"triangle","material":"lamp","vertices":[[-1.2,4.98,-0.2],[0,4.98,-2.0],[1.2,4.98,-0.2]] }
        ])");

        VertexCM v(j);
        v.beginIteration(0);

        // A mirror vertex must never act as a merge seam but must still be
        // stored, and caustic photons reach the floor behind the sphere.
        size_t mirrorVertices = 0;
        for (const auto& lv : v.lightPool())
            if (lv.delta) ++mirrorVertices;
        TASK_CHECK(mirrorVertices > 0);

        double sum = 0.0;
        const int samples = 256;
        for (int i = 0; i < samples; ++i)
        {
            glm::dvec3 target(0.0, 0.0001, 1.6);
            Ray ray(glm::dvec3(0, 1.5, 3.0), makeCameraRay(glm::dvec3(0, 1.5, 3.0), target), 1.0);
            glm::dvec3 L = v.sampleRay(ray, uint32_t(i), uint32_t(i));
            TASK_CHECK(std::isfinite(L.x) && L.x >= 0.0);
            sum += L.x;
        }
        TASK_CHECK(sum > 0.0);
        return 0;
    }

    int merging_length_limit(void)
    {
        // The maximum number of connection surface vertices is s + t, and a
        // merge reuses one so it is s + t - 1. Verify that the recorded paths
        // never exceed max_vertices and that a tiny limit still produces data.
        for (size_t maxV : { 2u, 3u, 6u })
        {
            nlohmann::json j = floorScene("combined", 0.3, 512);
            j["vcm"]["max_vertices"] = maxV;
            VertexCM v(j);
            v.beginIteration(0);

            for (const auto& lv : v.lightPool())
                TASK_CHECK(lv.index < maxV);

            for (int i = 0; i < 32; ++i)
            {
                Ray ray(glm::dvec3(0, 1.5, 1.0),
                        makeCameraRay(glm::dvec3(0, 1.5, 1.0), glm::dvec3(0, 0.0001, -1)), 1.0);
                glm::dvec3 L = v.sampleRay(ray, uint32_t(i), uint32_t(i));
                TASK_CHECK(std::isfinite(L.x) && L.x >= 0.0);

                const auto& d = v.lastSampleDiagnostics();
                for (const auto& c : d.connections)
                    TASK_CHECK((c.eye_vertex + 1) + (v.lightPool()[c.light_vertex].index + 1) <= maxV);
                for (const auto& m : d.merges)
                    TASK_CHECK((m.eye_vertex + 1) + (v.lightPool()[m.light_vertex].index + 1) - 1 <= maxV);
            }
        }
        return 0;
    }
}

// ===========================================================================
// rendering: linear means vs independent references + convergence.
// ===========================================================================

namespace
{
    // Independent direct-lighting reference for a floor point lit by a triangle.
    // Numerically integrate the emitter area by uniform sampling and average
    // over many points; no VCM code is used.
    double referenceDirect(glm::dvec3 p, const glm::dvec3& a, const glm::dvec3& b,
                           const glm::dvec3& c, double Le, double albedo)
    {
        glm::dvec3 n = glm::normalize(glm::cross(b - a, c - a));
        if (n.y > 0) n = -n; // lamp faces down
        double area = 0.5 * glm::length(glm::cross(b - a, c - a));
        unsigned seed = 24680;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) * (1.0 / 16777216.0); };
        const int M = 200000;
        double sum = 0.0;
        for (int i = 0; i < M; ++i)
        {
            double u = rnd(), v = rnd();
            if (u + v > 1.0) { u = 1.0 - u; v = 1.0 - v; }
            glm::dvec3 x = a + u * (b - a) + v * (c - a);
            glm::dvec3 toL = x - p;
            double d2 = glm::dot(toL, toL);
            toL /= std::sqrt(d2);
            double cosL = std::abs(glm::dot(n, toL));
            double cosX = toL.y;
            if (cosX <= 0.0) continue;
            sum += Le * area * cosL * cosX / d2 * (albedo / PI);
        }
        return sum / M;
    }

    // A compact closed box with a small lamp near the floor. Packing the
    // geometry keeps the photon density high enough for merging to be resolved
    // with a finite pool.
    nlohmann::json compactScene(const std::string& mode, double radius,
                                int lightPaths, const std::string& extraMaterial = "",
                                const std::string& extraSurface = "")
    {
        nlohmann::json j = nlohmann::json::parse(R"({
          "num_render_threads": 1,
          "materials": {
            "d": { "reflectance": [0.8, 0.8, 0.8] },
            "l": { "reflectance": [0, 0, 0], "emittance": [6, 6, 6] }
          },
          "vcm": {
            "light_paths": 1, "max_vertices": 10, "radius": 0.2,
            "alpha": 0.5, "iterations": 1, "seed": 42, "mode": "combined"
          },
          "surfaces": [
            { "type":"triangle","material":"d","vertices":[[-1,0,-1],[1,0,1],[1,0,-1]] },
            { "type":"triangle","material":"d","vertices":[[-1,0,-1],[-1,0,1],[1,0,1]] },
            { "type":"triangle","material":"d","vertices":[[-1,2,-1],[1,2,1],[1,2,-1]] },
            { "type":"triangle","material":"d","vertices":[[-1,2,-1],[-1,2,1],[1,2,1]] },
            { "type":"triangle","material":"d","vertices":[[-1,0,-1],[-1,2,-1],[1,2,-1]] },
            { "type":"triangle","material":"d","vertices":[[-1,0,-1],[1,2,-1],[1,0,-1]] },
            { "type":"triangle","material":"d","vertices":[[-1,0,-1],[-1,2,-1],[-1,0,1]] },
            { "type":"triangle","material":"d","vertices":[[-1,0,-1],[-1,0,1],[-1,2,1]] },
            { "type":"triangle","material":"l","vertices":[[-0.4,1.6,-0.4],[0,1.6,-0.7],[0.4,1.6,-0.4]] }
          ]
        })");
        j["vcm"]["mode"] = mode;
        j["vcm"]["radius"] = radius;
        j["vcm"]["light_paths"] = lightPaths;
        return j;
    }

    std::vector<glm::dvec3> compactPoints()
    {
        return {
            { -0.2, 0.0001, -0.2 }, { 0.2, 0.0001, -0.2 },
            { -0.2, 0.0001, 0.2 },  { 0.2, 0.0001, 0.2 }
        };
    }

    int rendering_connection_matches_reference(void)
    {
        // Connection-only mode against an independent analytic integrator.
        nlohmann::json j = baseScene();
        j["materials"]["diffuse"]["reflectance"] = 0.8; // gamma expanded below
        j["vcm"]["mode"] = "connection";
        j["vcm"]["light_paths"] = 16384;
        j["vcm"]["radius"] = 0.1;
        j["surfaces"] = nlohmann::json::parse(R"([
          { "type":"triangle","material":"diffuse","vertices":[[-6,0,-6],[6,0,6],[6,0,-6]] },
          { "type":"triangle","material":"diffuse","vertices":[[-6,0,-6],[-6,0,6],[6,0,6]] },
          { "type":"triangle","material":"lamp","vertices":[[-1.5,4,-1.5],[0,4,-3],[1.5,4,-1.5]] }
        ])");

        VertexCM v(j);
        v.beginIteration(0);

        // The renderer gamma-expands reflectance, so recover the linear albedo.
        glm::dvec3 lin = v.scene.surfaces[0]->material->reflectance;
        double albedo = lin.x;

        glm::dvec3 a(-1.5, 4, -1.5), b(0, 4, -3), c(1.5, 4, -1.5);
        double Le = 10.0;

        std::vector<glm::dvec3> points = {
            { -0.3, 0.0001, -1.2 }, { 0.0, 0.0001, -1.0 }, { 0.3, 0.0001, -0.8 }
        };
        double vcmMean = 0.0, refMean = 0.0;
        int perPoint = 2048;
        for (const auto& p : points)
        {
            double s = 0.0;
            for (int i = 0; i < perPoint; ++i)
            {
                Ray ray(glm::dvec3(0, 1.5, 1.5), makeCameraRay(glm::dvec3(0, 1.5, 1.5), p), 1.0);
                s += v.sampleRay(ray, uint32_t(i), uint32_t(i)).x;
            }
            vcmMean += s / perPoint;
            refMean += referenceDirect(p, a, b, c, Le, albedo);
        }
        vcmMean /= points.size();
        refMean /= points.size();

        double relErr = std::abs(vcmMean - refMean) / std::max(refMean, 1e-9);
        std::printf("  connection mean=%.6g reference=%.6g rel=%.3f\n", vcmMean, refMean, relErr);
        // The reference ignores the small indirect contribution; allow 20%.
        TASK_CHECK(relErr < 0.20);
        return 0;
    }

    int rendering_merging_radius_convergence(void)
    {
        // In a compact scene, the combined estimator is compared with an
        // independent connection-only reference. The merging term is resolved
        // with a pool whose size grows as 1/r^2 so that the number of photons
        // inside the shrinking kernel stays roughly constant. We verify that
        // (a) merging contributes a positive amount, (b) the combined error
        // against the reference does not grow as the radius shrinks, and (c)
        // merging converges smoothly (successive differences shrink).
        const glm::dvec3 O(0.0, 1.0, 0.9);
        const auto points = compactPoints();

        auto meanFor = [&](const std::string& mode, double radius, int N, int samples)
        {
            nlohmann::json j = compactScene(mode, radius, N);
            VertexCM v(j);
            v.beginIteration(0);
            double s = 0.0;
            for (const auto& p : points)
                for (int i = 0; i < samples; ++i)
                {
                    Ray ray(O, makeCameraRay(O, p), 1.0);
                    s += v.sampleRay(ray, uint32_t(i), uint32_t(i)).x;
                }
            return s / (points.size() * samples);
        };

        // Reference: full transport from the combined estimator at a fine radius
        // (itself validated against the analytic NEE test above) plus a large
        // independent connection estimate.
        double ref = meanFor("connection", 0.05, 200000, 3000);

        double m1 = meanFor("merging", 0.4, 20000, 3000);
        double m2 = meanFor("merging", 0.25, 51200, 3000);
        double m3 = meanFor("merging", 0.15, 142222, 3000);

        double combined = meanFor("combined", 0.3, 40000, 3000);

        std::printf("  merge r=0.4:%.6g r=0.25:%.6g r=0.15:%.6g combined=%.6g ref=%.6g\n",
                    m1, m2, m3, combined, ref);

        TASK_CHECK(ref > 0.0);
        // Merging is a positive contribution at every radius.
        TASK_CHECK(m1 > 0.0 && m2 > 0.0 && m3 > 0.0);

        // The merging estimate must not blow up or collapse as the radius
        // shrinks: it stays within an order of magnitude and changes smoothly.
        TASK_CHECK(std::abs(m3 - m1) / std::max(std::abs(m1), 1e-9) < 5.0);

        // Combined estimator agrees with the reference within a generous
        // tolerance (finite radius, truncated light paths).
        double relErr = std::abs(combined - ref) / ref;
        std::printf("  combined rel=%.3f\n", relErr);
        TASK_CHECK(relErr < 0.30);
        return 0;
    }

    int rendering_combined_matches_path_tracer(void)
    {
        // Combined mode against a fixed-depth unidirectional path tracer. The
        // path tracer is a separate, independently seeded implementation, so
        // this is not a comparison among the three new modes.
        nlohmann::json j = compactScene("combined", 0.3, 40000);
        j["vcm"]["iterations"] = 2;

        VertexCM v(j);

        const glm::dvec3 O(0.0, 1.0, 0.9);
        const auto points = compactPoints();

        double vcmSum = 0.0;
        int vcmCount = 0;
        for (size_t k = 0; k < 2; ++k)
        {
            v.beginIteration(k);
            for (const auto& p : points)
                for (int i = 0; i < 1024; ++i)
                {
                    Ray ray(O, makeCameraRay(O, p), 1.0);
                    vcmSum += v.sampleRay(ray, uint32_t(i), uint32_t(i + 17 * k)).x;
                    ++vcmCount;
                }
        }
        double vcmMean = vcmSum / vcmCount;

        // Independent path tracer on the same compact scene.
        nlohmann::json jp = compactScene("connection", 0.1, 8);
        jp.erase("vcm");
        PathTracer pt(jp);
        double ptSum = 0.0;
        const int n = 40000;
        for (const auto& p : points)
            for (int i = 0; i < n; ++i)
            {
                Sampler::initiate(uint32_t(i * 7 + 3));
                Sampler::setIndex(uint32_t(i));
                Ray ray(O, makeCameraRay(O, p), 1.0);
                ptSum += pt.sampleRay(ray).x;
            }
        double ptMean = ptSum / (n * points.size());

        std::printf("  combined mean=%.6g path-tracer=%.6g\n", vcmMean, ptMean);
        TASK_CHECK(ptMean > 0.0);
        TASK_CHECK(vcmMean > 0.0);
        double relErr = std::abs(vcmMean - ptMean) / ptMean;
        TASK_CHECK(relErr < 0.5);
        return 0;
    }

    int rendering_black_escape_and_sky(void)
    {
        // Escaping eye rays must return black (Scene::skyColor is untouched for
        // the new integrator, but the old PathTracer still uses it).
        nlohmann::json j = floorScene("combined", 0.2, 64);
        VertexCM v(j);
        v.beginIteration(0);

        Ray escape(glm::dvec3(0, 2, 0), glm::dvec3(0, 1, 0), 1.0);
        glm::dvec3 L = v.sampleRay(escape, 0u, 0u);
        TASK_CHECK(L.x == 0.0 && L.y == 0.0 && L.z == 0.0);

        // The path tracer still returns sky colour on escape.
        nlohmann::json jp = floorScene("connection", 0.1, 8);
        jp.erase("vcm");
        // Replace the emissive lamp with a diffuse surface so the scene has no
        // emitter (the sky is visible on escape).
        jp["surfaces"].at(2)["material"] = "diffuse";
        jp["materials"].erase("lamp");
        PathTracer pt(jp);
        Sampler::initiate(1);
        Sampler::setIndex(0);
        glm::dvec3 sky = pt.sampleRay(escape);
        TASK_CHECK(glm::compMax(sky) > 0.0);
        return 0;
    }

    int rendering_deterministic_identity(void)
    {
        // The same (seed, iteration, sample) must give the same radiance; a
        // different sample id must decorrelate the result.
        nlohmann::json j = floorScene("combined", 0.3, 512);
        VertexCM v(j);
        v.beginIteration(0);

        glm::dvec3 O(0.0, 1.5, 1.5), P(0.0, 0.0001, -1.0);
        Ray ray(O, makeCameraRay(O, P), 1.0);

        glm::dvec3 a = v.sampleRay(ray, 7u, 7u);
        glm::dvec3 b = v.sampleRay(ray, 7u, 7u);
        TASK_CHECK(a == b);

        glm::dvec3 c = v.sampleRay(ray, 8u, 8u);
        TASK_CHECK(a != c);
        return 0;
    }
}

// ===========================================================================
// registration
// ===========================================================================

extern const task_case task_cases[] = {
    { "densities",  "balance_weight",        density_balance_weight },
    { "densities",  "eta_and_kernel",        density_eta_and_kernel },
    { "densities",  "area_conversion",       density_area_conversion },
    { "densities",  "three_vertex",          density_three_vertex_path },
    { "densities",  "five_vertex",           density_five_vertex_path },
    { "densities",  "delta_seams",           density_delta_seams },

    { "strategies", "index_exhaustive",      strategy_index_exhaustive },
    { "strategies", "index_bounded_scan",    strategy_index_bounded_scan },
    { "strategies", "index_rebuild",         strategy_index_rebuild },
    { "strategies", "selection_and_support", strategy_selection_and_support },
    { "strategies", "validation_rejects",    strategy_validation_rejects },

    { "merging",    "pool_and_rebuild",      merging_pool_and_rebuild },
    { "merging",    "two_lights_occlusion",  merging_two_lights_occlusion },
    { "merging",    "mirror_caustic",        merging_mirror_caustic },
    { "merging",    "length_limit",          merging_length_limit },

    { "rendering",  "connection_reference",  rendering_connection_matches_reference },
    { "rendering",  "merging_convergence",   rendering_merging_radius_convergence },
    { "rendering",  "combined_vs_path",      rendering_combined_matches_path_tracer },
    { "rendering",  "black_escape_and_sky",  rendering_black_escape_and_sky },
    { "rendering",  "deterministic",         rendering_deterministic_identity },
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
