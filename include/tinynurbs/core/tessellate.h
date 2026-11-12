/**
 * Adaptive tessellation of one or more rational NURBS surfaces, with shared
 * parameter boundaries (seams) meshed as a single indexed triangle net.
 *
 * The tessellator refines each non-zero knot interval independently.  It
 * never modifies the input surfaces.
 *
 * Use of this source code is governed by a BSD-style license that can be found in
 * the LICENSE file.
 */

#ifndef TINYNURBS_TESSELLATE_H
#define TINYNURBS_TESSELLATE_H

#include "../util/array2.h"
#include "../util/util.h"
#include "evaluate.h"
#include "surface.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace tinynurbs
{

/**
 * Identifies one of the four parameter-space boundary edges of a surface.
 * UMin/UMax run in the direction of increasing v, VMin/VMax run in the
 * direction of increasing u.
 */
enum class PatchEdge
{
    UMin = 0,
    UMax = 1,
    VMin = 2,
    VMax = 3
};

/**
 * Declares that two patch edges are the same geometric curve.
 *
 * patch_a / patch_b index the input surface list, edge_a / edge_b identify
 * the corresponding boundary edges.  The edge parameter is first normalized
 * to [0,1] over each patch's valid domain.  When reversed is true the
 * correspondence is t on patch_a <-> 1 - t on patch_b.
 */
struct PatchSeam
{
    size_t patch_a = 0;
    size_t patch_b = 0;
    PatchEdge edge_a = PatchEdge::UMin;
    PatchEdge edge_b = PatchEdge::UMin;
    bool reversed = false;
};

/**
 * Options controlling tessellation.
 *
 * chord_tolerance: maximum distance between the true surface and the linear
 *   triangle at the three edge midpoints and the centroid of each parameter
 *   triangle.  This is a sampled flatness criterion, not a continuous
 *   Hausdorff bound.
 * seam_tolerance: maximum geometric disagreement allowed along a declared
 *   seam.
 * max_depth: maximum number of subdivision generations below each initial
 *   non-zero knot interval.  0 means no subdivision is permitted.
 */
struct TessellationOptions
{
    double chord_tolerance = 1e-3;
    double seam_tolerance = 1e-9;
    unsigned int max_depth = 12;
};

/**
 * Indexed triangle mesh returned by tessellatePatches.
 *
 * positions holds one position per vertex.  triangles holds three vertex
 * indices per triangle.  triangle_patch holds the source patch index for each
 * triangle.  triangle_uv holds one parameter value per triangle corner, in
 * the source patch's own parameter domain.  Conforming seam vertices are
 * shared by index, so a position is shared while each adjacent patch keeps
 * its own UVs.
 */
struct TessellationMesh
{
    std::vector<glm::dvec3> positions;
    std::vector<std::array<size_t, 3>> triangles;
    std::vector<size_t> triangle_patch;
    std::vector<std::array<glm::dvec2, 3>> triangle_uv;
};

namespace internal
{
namespace tessellate
{

/////////////////////////////////////////////////////////////////////
// Small value and key types
/////////////////////////////////////////////////////////////////////

inline bool finiteValue(double x) { return std::isfinite(x); }

constexpr double kParamMergeEps = 1e-12;
constexpr unsigned int kMaxCellLevel = 40;

struct Interval
{
    double lo = 0.0;
    double hi = 0.0;
};

/** A rational position inside one non-zero knot interval: num / 2^level. */
struct AxisKey
{
    int cell = 0;
    uint64_t num = 0;
    unsigned int level = 0;
};

struct AxisKeyLess
{
    bool operator()(const AxisKey &a, const AxisKey &b) const
    {
        if (a.cell != b.cell)
            return a.cell < b.cell;
        if (a.level != b.level)
            return a.level < b.level;
        return a.num < b.num;
    }
};

inline bool axisKeyEqual(const AxisKey &a, const AxisKey &b)
{
    return a.cell == b.cell && a.level == b.level && a.num == b.num;
}

/** Two-dimensional parameter key, one AxisKey per direction. */
struct VertexKey
{
    AxisKey u;
    AxisKey v;
};

/** A leaf cell inside one initial knot interval. */
struct Cell
{
    int root_u = 0;
    int root_v = 0;
    unsigned int level = 0;
    uint64_t ix = 0;
    uint64_t iy = 0;
};

struct CellLess
{
    bool operator()(const Cell &a, const Cell &b) const
    {
        if (a.root_u != b.root_u)
            return a.root_u < b.root_u;
        if (a.root_v != b.root_v)
            return a.root_v < b.root_v;
        if (a.level != b.level)
            return a.level < b.level;
        if (a.ix != b.ix)
            return a.ix < b.ix;
        return a.iy < b.iy;
    }
};

/** Identity of a mesh vertex occurrence. */
struct OccKey
{
    // kind 0: structural (one patch's parameter ladder); kind 1: seam class.
    int kind = 0;
    size_t patch = 0;
    AxisKey u;
    AxisKey v;
    size_t root = 0;
};

struct OccKeyLess
{
    bool operator()(const OccKey &a, const OccKey &b) const
    {
        if (a.kind != b.kind)
            return a.kind < b.kind;
        if (a.kind == 1)
            return a.root < b.root;
        if (a.patch != b.patch)
            return a.patch < b.patch;
        AxisKeyLess less;
        if (!axisKeyEqual(a.u, b.u))
            return less(a.u, b.u);
        return less(a.v, b.v);
    }
};

inline OccKey structuralOcc(size_t patch, const VertexKey &key)
{
    OccKey occ;
    occ.kind = 0;
    occ.patch = patch;
    occ.u = key.u;
    occ.v = key.v;
    occ.root = 0;
    return occ;
}

inline OccKey seamOcc(size_t root)
{
    OccKey occ;
    occ.kind = 1;
    occ.patch = 0;
    occ.root = root;
    return occ;
}

/////////////////////////////////////////////////////////////////////
// Patch data and validation
/////////////////////////////////////////////////////////////////////

struct PatchData
{
    unsigned int degree_u = 0;
    unsigned int degree_v = 0;
    std::vector<double> knots_u;
    std::vector<double> knots_v;
    array2<glm::dvec4> homo;
    std::vector<Interval> roots_u;
    std::vector<Interval> roots_v;
    double umin = 0.0;
    double umax = 0.0;
    double vmin = 0.0;
    double vmax = 0.0;
    // Per-edge seam index, -1 when the edge is not part of a seam.
    int seam_index[4];
    PatchData() { seam_index[0] = seam_index[1] = seam_index[2] = seam_index[3] = -1; }
};

inline std::string patchError(size_t index, const std::string &message)
{
    return "tessellatePatches: patch " + std::to_string(index) + ": " + message;
}

inline void validateKnotVector(const std::vector<double> &knots, unsigned int degree,
                               size_t num_control_points, size_t patch_index,
                               const char *axis)
{
    const std::string prefix = axis;
    if (knots.size() != num_control_points + degree + 1)
    {
        throw std::invalid_argument(
            patchError(patch_index, prefix + " knot vector size does not match degree and "
                                            "control point count"));
    }
    for (double k : knots)
    {
        if (!finiteValue(k))
            throw std::invalid_argument(patchError(patch_index, prefix + " knot is not finite"));
    }
    for (size_t i = 1; i < knots.size(); ++i)
    {
        if (knots[i] < knots[i - 1])
            throw std::invalid_argument(
                patchError(patch_index, prefix + " knot vector is not nondecreasing"));
    }
    if (knots.empty())
        throw std::invalid_argument(patchError(patch_index, prefix + " knot vector is empty"));

    // Analyse runs of equal knot values.
    std::vector<std::pair<double, size_t>> runs;
    size_t i = 0;
    while (i < knots.size())
    {
        size_t j = i;
        while (j + 1 < knots.size() && knots[j + 1] == knots[i])
            ++j;
        runs.push_back(std::make_pair(knots[i], j - i + 1));
        i = j + 1;
    }
    if (runs.empty())
        throw std::invalid_argument(patchError(patch_index, prefix + " knot vector is empty"));
    if (knots.front() == knots.back())
    {
        // A constant knot vector satisfies the size relation but spans no
        // parameter interval.  It is structurally well formed; the empty
        // domain is diagnosed later as a degenerate parameter region.
        return;
    }
    if (runs.front().second != degree + 1)
    {
        throw std::invalid_argument(patchError(patch_index, prefix + " knot vector is not clamped "
                                                            "at the start"));
    }
    if (runs.back().second != degree + 1)
    {
        throw std::invalid_argument(
            patchError(patch_index, prefix + " knot vector is not clamped at the end"));
    }
    for (size_t r = 1; r + 1 < runs.size(); ++r)
    {
        if (runs[r].second > degree)
        {
            throw std::invalid_argument(
                patchError(patch_index, prefix + " interior knot multiplicity exceeds degree"));
        }
    }
}

inline PatchData makePatchData(const RationalSurface3d &srf, size_t patch_index)
{
    if (srf.degree_u < 1 || srf.degree_u > 3 || srf.degree_v < 1 || srf.degree_v > 3)
    {
        throw std::invalid_argument(
            patchError(patch_index, "degree must be between 1 and 3 in both directions"));
    }
    if (srf.control_points.rows() == 0 || srf.control_points.cols() == 0)
    {
        throw std::invalid_argument(patchError(patch_index, "control point grid is empty"));
    }
    if (srf.weights.rows() != srf.control_points.rows() ||
        srf.weights.cols() != srf.control_points.cols())
    {
        throw std::invalid_argument(patchError(patch_index, "weight grid dimensions do not match "
                                                            "control point grid"));
    }
    validateKnotVector(srf.knots_u, srf.degree_u, srf.control_points.rows(), patch_index, "u");
    validateKnotVector(srf.knots_v, srf.degree_v, srf.control_points.cols(), patch_index, "v");
    if (srf.knots_u.front() == srf.knots_u.back() || srf.knots_v.front() == srf.knots_v.back())
    {
        throw std::runtime_error(patchError(patch_index, "degenerate parameter region"));
    }

    PatchData out;
    out.degree_u = srf.degree_u;
    out.degree_v = srf.degree_v;
    out.knots_u = srf.knots_u;
    out.knots_v = srf.knots_v;
    out.homo.resize(srf.control_points.rows(), srf.control_points.cols());
    for (size_t r = 0; r < srf.control_points.rows(); ++r)
    {
        for (size_t c = 0; c < srf.control_points.cols(); ++c)
        {
            const glm::dvec3 &cp = srf.control_points(r, c);
            const double w = srf.weights(r, c);
            if (!finiteValue(cp.x) || !finiteValue(cp.y) || !finiteValue(cp.z))
            {
                throw std::invalid_argument(
                    patchError(patch_index, "control point is not finite"));
            }
            if (!finiteValue(w) || !(w > 0.0))
            {
                throw std::invalid_argument(patchError(patch_index,
                                                       "weight must be positive and finite"));
            }
            out.homo(r, c) = glm::dvec4(cp.x * w, cp.y * w, cp.z * w, w);
        }
    }

    const size_t ncp_u = srf.control_points.rows();
    const size_t ncp_v = srf.control_points.cols();
    out.umin = out.knots_u[srf.degree_u];
    out.umax = out.knots_u[ncp_u];
    out.vmin = out.knots_v[srf.degree_v];
    out.vmax = out.knots_v[ncp_v];
    if (!(out.umax > out.umin) || !(out.vmax > out.vmin))
    {
        throw std::runtime_error(patchError(patch_index, "degenerate parameter region"));
    }
    for (size_t k = srf.degree_u; k < ncp_u; ++k)
    {
        if (out.knots_u[k + 1] > out.knots_u[k])
            out.roots_u.push_back(Interval{out.knots_u[k], out.knots_u[k + 1]});
    }
    for (size_t k = srf.degree_v; k < ncp_v; ++k)
    {
        if (out.knots_v[k + 1] > out.knots_v[k])
            out.roots_v.push_back(Interval{out.knots_v[k], out.knots_v[k + 1]});
    }
    if (out.roots_u.empty() || out.roots_v.empty())
    {
        throw std::runtime_error(patchError(patch_index, "degenerate parameter region"));
    }
    return out;
}

/////////////////////////////////////////////////////////////////////
// Tessellator
/////////////////////////////////////////////////////////////////////

class Tessellator
{
  public:
    Tessellator(const std::vector<RationalSurface3d> &patches,
                const std::vector<PatchSeam> &seams, const TessellationOptions &options)
        : patches_(patches), seams_(seams), options_(options)
    {
    }

    TessellationMesh run();

  private:
    struct GroupEntry
    {
        double pos = 0.0;
        double u = 0.0;
        double v = 0.0;
        OccKey occ;
    };

    struct PatchGroups
    {
        std::map<AxisKey, std::vector<GroupEntry>, AxisKeyLess> vertical;
        std::map<AxisKey, std::vector<GroupEntry>, AxisKeyLess> horizontal;
    };

    struct Node
    {
        double t = 0.0;
        double ua = 0.0, va = 0.0, ub = 0.0, vb = 0.0;
        size_t seam = 0;
    };

    struct SeamState
    {
        std::vector<Node> nodes;
        std::vector<size_t> begin;
        std::vector<size_t> end;
        std::vector<std::map<AxisKey, size_t, AxisKeyLess>> pos_a;
        std::vector<std::map<AxisKey, size_t, AxisKeyLess>> pos_b;
        std::vector<size_t> parent;
        std::vector<size_t> node_root;
        // Deterministic representative node per union class, chosen as the
        // smallest node id.  Its position is the canonical seam position.
        std::vector<size_t> representative;
    };

    struct PVert
    {
        double u = 0.0;
        double v = 0.0;
        OccKey occ;
    };

    void validateInputs();
    void initialiseLeaves();
    void buildSeamState(SeamState &state);
    void buildGroups(const SeamState &state);
    bool refineOnce(const SeamState &state, bool &changed);
    void splitCell(size_t patch, const Cell &cell);
    TessellationMesh assemble(const SeamState &state);
    void checkSeamGeometry(const SeamState &state);

    // Parameter-ladder helpers
    AxisKey canonicalAxis(int cell, uint64_t num, unsigned int level, size_t num_cells) const;
    double axisValue(const std::vector<Interval> &roots, const AxisKey &key) const;
    VertexKey cornerKey(size_t patch, const Cell &cell, int which) const;
    VertexKey domainCornerKey(size_t patch, int corner) const;
    AxisKey uMinKey(size_t patch) const;
    AxisKey uMaxKey(size_t patch) const;
    AxisKey vMinKey(size_t patch) const;
    AxisKey vMaxKey(size_t patch) const;
    bool cornerOnEdge(const VertexKey &key, PatchEdge edge, size_t patch) const;

    // Geometry
    glm::dvec3 evaluate(size_t patch, double u, double v) const;
    glm::dvec2 mapToSide(const PatchSeam &seam, bool side_a, double t) const;
    OccKey resolveCorner(size_t patch, const VertexKey &key, const SeamState &state) const;
    bool trianglePasses(size_t patch, const PVert &a, const PVert &b, const PVert &c,
                        const SeamState &state, std::map<OccKey, glm::dvec3, OccKeyLess> &cache,
                        glm::dvec3 &out_a, glm::dvec3 &out_b, glm::dvec3 &out_c);
    glm::dvec3 positionFor(const OccKey &occ, size_t patch, double u, double v,
                           const SeamState &state,
                           std::map<OccKey, glm::dvec3, OccKeyLess> &cache) const;

    // Mesh construction helpers
    std::vector<PVert> makePolygon(size_t patch, const Cell &cell, const SeamState &state,
                                   PVert &center) const;
    std::vector<const GroupEntry *>
    collectEdgeEntries(const std::vector<GroupEntry> *group, double lo, double hi,
                       bool ascending) const;
    GroupEntry entryFromCorner(size_t patch, const VertexKey &key, const SeamState &state) const;

    const std::vector<RationalSurface3d> &patches_;
    const std::vector<PatchSeam> &seams_;
    TessellationOptions options_;
    std::vector<PatchData> data_;
    std::vector<std::set<Cell, CellLess>> leaves_;
    std::vector<PatchGroups> groups_;
    bool validated_ = false;
};

inline AxisKey Tessellator::canonicalAxis(int cell, uint64_t num, unsigned int level,
                                          size_t num_cells) const
{
    if (num == 0)
    {
        AxisKey out;
        out.cell = cell;
        out.num = 0;
        out.level = 0;
        return out;
    }
    const uint64_t den = (uint64_t(1) << level);
    if (num == den)
    {
        if (cell + 1 < static_cast<int>(num_cells))
        {
            AxisKey out;
            out.cell = cell + 1;
            out.num = 0;
            out.level = 0;
            return out;
        }
        AxisKey out;
        out.cell = cell;
        out.num = 1;
        out.level = 0;
        return out;
    }
    while (level > 0 && (num & 1ull) == 0)
    {
        num >>= 1;
        --level;
    }
    AxisKey out;
    out.cell = cell;
    out.num = num;
    out.level = level;
    return out;
}

inline double Tessellator::axisValue(const std::vector<Interval> &roots,
                                     const AxisKey &key) const
{
    const Interval &root = roots[static_cast<size_t>(key.cell)];
    if (key.num == 0)
        return root.lo;
    const uint64_t den = (uint64_t(1) << key.level);
    if (key.num == den)
        return root.hi;
    return root.lo + (root.hi - root.lo) * (static_cast<double>(key.num) /
                                            static_cast<double>(den));
}

inline VertexKey Tessellator::cornerKey(size_t patch, const Cell &cell, int which) const
{
    uint64_t iu = cell.ix;
    uint64_t iv = cell.iy;
    if (which == 1 || which == 2)
        iu += 1;
    if (which == 2 || which == 3)
        iv += 1;
    VertexKey key;
    key.u = canonicalAxis(cell.root_u, iu, cell.level, data_[patch].roots_u.size());
    key.v = canonicalAxis(cell.root_v, iv, cell.level, data_[patch].roots_v.size());
    return key;
}

inline VertexKey Tessellator::domainCornerKey(size_t patch, int corner) const
{
    VertexKey key;
    key.u = (corner == 0 || corner == 3) ? uMinKey(patch) : uMaxKey(patch);
    key.v = (corner == 0 || corner == 1) ? vMinKey(patch) : vMaxKey(patch);
    return key;
}

inline AxisKey Tessellator::uMinKey(size_t patch) const
{
    return canonicalAxis(0, 0, 0, data_[patch].roots_u.size());
}

inline AxisKey Tessellator::uMaxKey(size_t patch) const
{
    const size_t n = data_[patch].roots_u.size();
    return canonicalAxis(static_cast<int>(n) - 1, 1, 0, n);
}

inline AxisKey Tessellator::vMinKey(size_t patch) const
{
    return canonicalAxis(0, 0, 0, data_[patch].roots_v.size());
}

inline AxisKey Tessellator::vMaxKey(size_t patch) const
{
    const size_t n = data_[patch].roots_v.size();
    return canonicalAxis(static_cast<int>(n) - 1, 1, 0, n);
}

inline bool Tessellator::cornerOnEdge(const VertexKey &key, PatchEdge edge, size_t patch) const
{
    switch (edge)
    {
    case PatchEdge::UMin:
        return axisKeyEqual(key.u, uMinKey(patch));
    case PatchEdge::UMax:
        return axisKeyEqual(key.u, uMaxKey(patch));
    case PatchEdge::VMin:
        return axisKeyEqual(key.v, vMinKey(patch));
    case PatchEdge::VMax:
        return axisKeyEqual(key.v, vMaxKey(patch));
    }
    return false;
}

inline glm::dvec3 Tessellator::evaluate(size_t patch, double u, double v) const
{
    const PatchData &p = data_[patch];
    glm::dvec4 h =
        internal::surfacePoint<4, double>(p.degree_u, p.degree_v, p.knots_u, p.knots_v,
                                         p.homo, u, v);
    if (!finiteValue(h.x) || !finiteValue(h.y) || !finiteValue(h.z) || !finiteValue(h.w) ||
        !(h.w > 0.0))
    {
        throw std::runtime_error("tessellatePatches: surface evaluation failed");
    }
    return glm::dvec3(h.x / h.w, h.y / h.w, h.z / h.w);
}

inline glm::dvec2 Tessellator::mapToSide(const PatchSeam &seam, bool side_a, double t) const
{
    const size_t patch = side_a ? seam.patch_a : seam.patch_b;
    const PatchEdge edge = side_a ? seam.edge_a : seam.edge_b;
    if (!side_a && seam.reversed)
        t = 1.0 - t;
    const PatchData &p = data_[patch];
    switch (edge)
    {
    case PatchEdge::UMin:
        return glm::dvec2(p.umin, p.vmin + t * (p.vmax - p.vmin));
    case PatchEdge::UMax:
        return glm::dvec2(p.umax, p.vmin + t * (p.vmax - p.vmin));
    case PatchEdge::VMin:
        return glm::dvec2(p.umin + t * (p.umax - p.umin), p.vmin);
    case PatchEdge::VMax:
        return glm::dvec2(p.umin + t * (p.umax - p.umin), p.vmax);
    }
    return glm::dvec2(0.0, 0.0);
}

/////////////////////////////////////////////////////////////////////
// Input validation
/////////////////////////////////////////////////////////////////////

inline void Tessellator::validateInputs()
{
    if (!finiteValue(options_.chord_tolerance) || !(options_.chord_tolerance > 0.0))
    {
        throw std::invalid_argument(
            "tessellatePatches: chord_tolerance must be positive and finite");
    }
    if (!finiteValue(options_.seam_tolerance) || !(options_.seam_tolerance > 0.0))
    {
        throw std::invalid_argument(
            "tessellatePatches: seam_tolerance must be positive and finite");
    }

    // Validate surface structure before evaluating anything.
    data_.clear();
    data_.reserve(patches_.size());
    for (size_t i = 0; i < patches_.size(); ++i)
        data_.push_back(makePatchData(patches_[i], i));

    // Validate seams and edge use.
    std::set<std::pair<size_t, int>> used_edges;
    for (size_t s = 0; s < seams_.size(); ++s)
    {
        const PatchSeam &seam = seams_[s];
        if (seam.patch_a >= patches_.size() || seam.patch_b >= patches_.size())
        {
            throw std::invalid_argument("tessellatePatches: seam references an out-of-range "
                                        "patch index");
        }
        if (seam.patch_a == seam.patch_b)
        {
            throw std::invalid_argument(
                "tessellatePatches: a seam cannot connect a patch edge to itself");
        }
        const int ea = static_cast<int>(seam.edge_a);
        const int eb = static_cast<int>(seam.edge_b);
        if (ea < 0 || ea > 3 || eb < 0 || eb > 3)
        {
            throw std::invalid_argument("tessellatePatches: seam has an invalid edge value");
        }
        if (!used_edges.insert(std::make_pair(seam.patch_a, ea)).second ||
            !used_edges.insert(std::make_pair(seam.patch_b, eb)).second)
        {
            throw std::invalid_argument(
                "tessellatePatches: a patch edge is used by more than one seam");
        }
        data_[seam.patch_a].seam_index[ea] = static_cast<int>(s);
        data_[seam.patch_b].seam_index[eb] = static_cast<int>(s);
    }
    validated_ = true;
}

/////////////////////////////////////////////////////////////////////
// Initial leaves
/////////////////////////////////////////////////////////////////////

inline void Tessellator::initialiseLeaves()
{
    leaves_.clear();
    leaves_.resize(data_.size());
    for (size_t p = 0; p < data_.size(); ++p)
    {
        for (size_t ru = 0; ru < data_[p].roots_u.size(); ++ru)
        {
            for (size_t rv = 0; rv < data_[p].roots_v.size(); ++rv)
            {
                Cell cell;
                cell.root_u = static_cast<int>(ru);
                cell.root_v = static_cast<int>(rv);
                cell.level = 0;
                cell.ix = 0;
                cell.iy = 0;
                leaves_[p].insert(cell);
            }
        }
    }
}

/////////////////////////////////////////////////////////////////////
// Seam state: sample union, node identities and corner classes
/////////////////////////////////////////////////////////////////////

namespace
{

struct EdgeSample
{
    double t = 0.0;
    AxisKey pos;
    double u = 0.0;
    double v = 0.0;
};

struct RawNode
{
    double t = 0.0;
    double ua = 0.0, va = 0.0, ub = 0.0, vb = 0.0;
    bool has_a_pos = false;
    bool has_b_pos = false;
    AxisKey pos_a;
    AxisKey pos_b;
    size_t seam = 0;
};

inline bool sampleLess(const EdgeSample &a, const EdgeSample &b)
{
    if (a.t != b.t)
        return a.t < b.t;
    AxisKeyLess less;
    return less(a.pos, b.pos);
}

inline bool rawNodeLess(const RawNode &a, const RawNode &b)
{
    if (a.t != b.t)
        return a.t < b.t;
    if (a.ua != b.ua)
        return a.ua < b.ua;
    if (a.va != b.va)
        return a.va < b.va;
    if (a.ub != b.ub)
        return a.ub < b.ub;
    return a.vb < b.vb;
}

} // namespace

inline void Tessellator::buildSeamState(SeamState &state)
{
    state.nodes.clear();
    state.begin.assign(seams_.size(), 0);
    state.end.assign(seams_.size(), 0);
    state.pos_a.assign(seams_.size(), std::map<AxisKey, size_t, AxisKeyLess>());
    state.pos_b.assign(seams_.size(), std::map<AxisKey, size_t, AxisKeyLess>());

    for (size_t s = 0; s < seams_.size(); ++s)
    {
        const PatchSeam &seam = seams_[s];
        const PatchEdge ea = seam.edge_a;
        const PatchEdge eb = seam.edge_b;
        const bool av = (ea == PatchEdge::UMin || ea == PatchEdge::UMax);
        const bool bv = (eb == PatchEdge::UMin || eb == PatchEdge::UMax);
        const size_t pa = seam.patch_a;
        const size_t pb = seam.patch_b;

        // Sample the boundary vertices contributed by each side.
        std::map<AxisKey, EdgeSample, AxisKeyLess> asamples, bsamples;
        for (const Cell &cell : leaves_[pa])
        {
            for (int ci = 0; ci < 4; ++ci)
            {
                VertexKey key = cornerKey(pa, cell, ci);
                if (!cornerOnEdge(key, ea, pa))
                    continue;
                const double u = axisValue(data_[pa].roots_u, key.u);
                const double v = axisValue(data_[pa].roots_v, key.v);
                EdgeSample sample;
                sample.pos = av ? key.v : key.u;
                sample.u = u;
                sample.v = v;
                double t;
                if (av)
                    t = (v - data_[pa].vmin) / (data_[pa].vmax - data_[pa].vmin);
                else
                    t = (u - data_[pa].umin) / (data_[pa].umax - data_[pa].umin);
                sample.t = std::max(0.0, std::min(1.0, t));
                asamples[sample.pos] = sample;
            }
        }
        for (const Cell &cell : leaves_[pb])
        {
            for (int ci = 0; ci < 4; ++ci)
            {
                VertexKey key = cornerKey(pb, cell, ci);
                if (!cornerOnEdge(key, eb, pb))
                    continue;
                const double u = axisValue(data_[pb].roots_u, key.u);
                const double v = axisValue(data_[pb].roots_v, key.v);
                EdgeSample sample;
                sample.pos = bv ? key.v : key.u;
                sample.u = u;
                sample.v = v;
                double t;
                if (bv)
                    t = (v - data_[pb].vmin) / (data_[pb].vmax - data_[pb].vmin);
                else
                    t = (u - data_[pb].umin) / (data_[pb].umax - data_[pb].umin);
                sample.t = std::max(0.0, std::min(1.0, t));
                if (seam.reversed)
                    sample.t = 1.0 - sample.t;
                bsamples[sample.pos] = sample;
            }
        }

        std::vector<EdgeSample> as, bs;
        for (const auto &it : asamples)
            as.push_back(it.second);
        for (const auto &it : bsamples)
            bs.push_back(it.second);
        std::sort(as.begin(), as.end(), sampleLess);
        std::sort(bs.begin(), bs.end(), sampleLess);

        const double eps = kParamMergeEps;
        std::vector<RawNode> raw;
        // Both sequences are sorted ascending in the shared parameter t, so
        // matching equal sites is a linear sweep.  A-only sites that fall
        // between two B samples are emitted on that side immediately, so no
        // contribution is ever skipped.
        size_t ia = 0;
        for (const EdgeSample &b : bs)
        {
            while (ia < as.size() && as[ia].t < b.t - eps)
            {
                const EdgeSample &a = as[ia];
                RawNode node;
                node.seam = s;
                node.t = a.t;
                node.ua = a.u;
                node.va = a.v;
                glm::dvec2 mapped = mapToSide(seam, false, a.t);
                node.ub = mapped.x;
                node.vb = mapped.y;
                node.has_a_pos = true;
                node.pos_a = a.pos;
                raw.push_back(node);
                ++ia;
            }
            if (ia < as.size() && std::abs(as[ia].t - b.t) <= eps)
            {
                RawNode node;
                node.seam = s;
                const EdgeSample &a = as[ia];
                node.t = a.t;
                node.ua = a.u;
                node.va = a.v;
                node.ub = b.u;
                node.vb = b.v;
                node.has_a_pos = true;
                node.has_b_pos = true;
                node.pos_a = a.pos;
                node.pos_b = b.pos;
                raw.push_back(node);
                ++ia;
            }
            else
            {
                RawNode node;
                node.seam = s;
                node.t = b.t;
                glm::dvec2 a = mapToSide(seam, true, b.t);
                node.ua = a.x;
                node.va = a.y;
                node.ub = b.u;
                node.vb = b.v;
                node.has_b_pos = true;
                node.pos_b = b.pos;
                raw.push_back(node);
            }
        }
        for (size_t j = ia; j < as.size(); ++j)
        {
            const EdgeSample &a = as[j];
            RawNode node;
            node.seam = s;
            node.t = a.t;
            node.ua = a.u;
            node.va = a.v;
            glm::dvec2 b = mapToSide(seam, false, a.t);
            node.ub = b.x;
            node.vb = b.y;
            node.has_a_pos = true;
            node.pos_a = a.pos;
            raw.push_back(node);
        }
        std::stable_sort(raw.begin(), raw.end(), rawNodeLess);

        state.begin[s] = state.nodes.size();
        for (const RawNode &rn : raw)
        {
            Node node;
            node.t = rn.t;
            node.ua = rn.ua;
            node.va = rn.va;
            node.ub = rn.ub;
            node.vb = rn.vb;
            node.seam = s;
            const size_t id = state.nodes.size();
            state.nodes.push_back(node);
            if (rn.has_a_pos)
                state.pos_a[s].insert(std::make_pair(rn.pos_a, id));
            if (rn.has_b_pos)
                state.pos_b[s].insert(std::make_pair(rn.pos_b, id));
        }
        state.end[s] = state.nodes.size();
    }

    const size_t node_count = state.nodes.size();
    const size_t num_patches = data_.size();
    state.parent.resize(node_count + 4 * num_patches);
    for (size_t i = 0; i < state.parent.size(); ++i)
        state.parent[i] = i;

    auto find_root = [&](size_t x) -> size_t {
        while (state.parent[x] != x)
        {
            state.parent[x] = state.parent[state.parent[x]];
            x = state.parent[x];
        }
        return x;
    };
    auto unite = [&](size_t a, size_t b) {
        size_t ra = find_root(a);
        size_t rb = find_root(b);
        if (ra != rb)
        {
            if (ra < rb)
                state.parent[rb] = ra;
            else
                state.parent[ra] = rb;
        }
    };

    // Node endpoints at corners, plus corner-token identity across seams.
    auto lookup_node = [&](size_t s, bool side_a, const AxisKey &pos) -> size_t {
        const auto &map = side_a ? state.pos_a[s] : state.pos_b[s];
        auto it = map.find(pos);
        if (it != map.end())
            return it->second;
        return state.nodes.size(); // no node there
    };

    for (size_t s = 0; s < seams_.size(); ++s)
    {
        const PatchSeam &seam = seams_[s];
        const PatchEdge ea = seam.edge_a;
        const PatchEdge eb = seam.edge_b;
        const bool av = (ea == PatchEdge::UMin || ea == PatchEdge::UMax);
        const bool bv = (eb == PatchEdge::UMin || eb == PatchEdge::UMax);
        int ca_start, ca_end, cb_start, cb_end;
        switch (ea)
        {
        case PatchEdge::UMin:
            ca_start = 0; ca_end = 3; break;
        case PatchEdge::UMax:
            ca_start = 1; ca_end = 2; break;
        case PatchEdge::VMin:
            ca_start = 0; ca_end = 1; break;
        case PatchEdge::VMax:
            ca_start = 3; ca_end = 2; break;
        default:
            ca_start = 0; ca_end = 1; break;
        }
        switch (eb)
        {
        case PatchEdge::UMin:
            cb_start = 0; cb_end = 3; break;
        case PatchEdge::UMax:
            cb_start = 1; cb_end = 2; break;
        case PatchEdge::VMin:
            cb_start = 0; cb_end = 1; break;
        case PatchEdge::VMax:
            cb_start = 3; cb_end = 2; break;
        default:
            cb_start = 0; cb_end = 1; break;
        }
        VertexKey a_start = domainCornerKey(seam.patch_a, ca_start);
        VertexKey a_end = domainCornerKey(seam.patch_a, ca_end);
        AxisKey a0pos = av ? a_start.v : a_start.u;
        AxisKey a1pos = av ? a_end.v : a_end.u;

        // canonical t = 0 corresponds to A's start; for B it flips when reversed.
        int corner_b_for_t0 = seam.reversed ? cb_end : cb_start;
        int corner_b_for_t1 = seam.reversed ? cb_start : cb_end;
        VertexKey b_t0 = domainCornerKey(seam.patch_b, corner_b_for_t0);
        VertexKey b_t1 = domainCornerKey(seam.patch_b, corner_b_for_t1);
        AxisKey b_t0pos = bv ? b_t0.v : b_t0.u;
        AxisKey b_t1pos = bv ? b_t1.v : b_t1.u;

        size_t na0 = lookup_node(s, true, a0pos);
        size_t na1 = lookup_node(s, true, a1pos);
        size_t nb0 = lookup_node(s, false, b_t0pos);
        size_t nb1 = lookup_node(s, false, b_t1pos);

        const size_t corner_a0 = node_count + seam.patch_a * 4 +
                                 static_cast<size_t>(ca_start);
        const size_t corner_a1 = node_count + seam.patch_a * 4 +
                                 static_cast<size_t>(ca_end);
        const size_t corner_b0 = node_count + seam.patch_b * 4 +
                                 static_cast<size_t>(corner_b_for_t0);
        const size_t corner_b1 = node_count + seam.patch_b * 4 +
                                 static_cast<size_t>(corner_b_for_t1);

        if (na0 < node_count)
            unite(na0, corner_a0);
        if (nb0 < node_count)
            unite(nb0, corner_b0);
        if (na1 < node_count)
            unite(na1, corner_a1);
        if (nb1 < node_count)
            unite(nb1, corner_b1);
    }

    // Seam endpoints are distinct nodes by construction; unite their corner
    // tokens here so corners meeting across several seams share one identity.
    for (size_t s = 0; s < seams_.size(); ++s)
    {
        const PatchSeam &seam = seams_[s];
        const PatchEdge ea = seam.edge_a;
        const PatchEdge eb = seam.edge_b;
        int ca_start, ca_end, cb_start, cb_end;
        switch (ea)
        {
        case PatchEdge::UMin: ca_start = 0; ca_end = 3; break;
        case PatchEdge::UMax: ca_start = 1; ca_end = 2; break;
        case PatchEdge::VMin: ca_start = 0; ca_end = 1; break;
        case PatchEdge::VMax: ca_start = 3; ca_end = 2; break;
        default: ca_start = 0; ca_end = 1; break;
        }
        switch (eb)
        {
        case PatchEdge::UMin: cb_start = 0; cb_end = 3; break;
        case PatchEdge::UMax: cb_start = 1; cb_end = 2; break;
        case PatchEdge::VMin: cb_start = 0; cb_end = 1; break;
        case PatchEdge::VMax: cb_start = 3; cb_end = 2; break;
        default: cb_start = 0; cb_end = 1; break;
        }
        const int corner_b_for_t0 = seam.reversed ? cb_end : cb_start;
        const int corner_b_for_t1 = seam.reversed ? cb_start : cb_end;
        unite(node_count + seam.patch_a * 4 + static_cast<size_t>(ca_start),
              node_count + seam.patch_b * 4 + static_cast<size_t>(corner_b_for_t0));
        unite(node_count + seam.patch_a * 4 + static_cast<size_t>(ca_end),
              node_count + seam.patch_b * 4 + static_cast<size_t>(corner_b_for_t1));
    }

    state.node_root.resize(node_count);
    for (size_t i = 0; i < node_count; ++i)
        state.node_root[i] = find_root(i);
    state.representative.assign(state.parent.size(), static_cast<size_t>(-1));
    for (size_t i = 0; i < node_count; ++i)
    {
        size_t &rep = state.representative[state.node_root[i]];
        if (rep == static_cast<size_t>(-1))
            rep = i;
    }
}

/////////////////////////////////////////////////////////////////////
// Groups of vertices lying on parameter lines
/////////////////////////////////////////////////////////////////////

inline void Tessellator::buildGroups(const SeamState &state)
{
    groups_.clear();
    groups_.resize(data_.size());

    for (size_t p = 0; p < data_.size(); ++p)
    {
        std::map<AxisKey, std::map<OccKey, GroupEntry, OccKeyLess>, AxisKeyLess> vert_raw;
        std::map<AxisKey, std::map<OccKey, GroupEntry, OccKeyLess>, AxisKeyLess> horiz_raw;

        auto add = [](std::map<AxisKey, std::map<OccKey, GroupEntry, OccKeyLess>, AxisKeyLess> &raw,
                      const AxisKey &line, GroupEntry entry, double pos) {
            entry.pos = pos;
            raw[line].insert(std::make_pair(entry.occ, entry));
        };

        // Every leaf corner is inserted into both incident parameter lines.
        for (const Cell &cell : leaves_[p])
        {
            for (int ci = 0; ci < 4; ++ci)
            {
                VertexKey key = cornerKey(p, cell, ci);
                GroupEntry entry = entryFromCorner(p, key, state);
                add(vert_raw, key.u, entry, entry.v);
                add(horiz_raw, key.v, entry, entry.u);
            }
        }

        // Seam nodes are inserted into the seam line of each adjacent patch.
        // Every node of the union belongs on both sides: points first sampled
        // on one side must appear as real polygon vertices on the other side.
        for (size_t s = 0; s < seams_.size(); ++s)
        {
            const PatchSeam &seam = seams_[s];
            for (int side = 0; side < 2; ++side)
            {
                const bool side_a = side == 0;
                const size_t patch = side_a ? seam.patch_a : seam.patch_b;
                if (patch != p)
                    continue;
                const PatchEdge edge = side_a ? seam.edge_a : seam.edge_b;
                const bool varies_v =
                    (edge == PatchEdge::UMin || edge == PatchEdge::UMax);
                const AxisKey line = varies_v
                                         ? (edge == PatchEdge::UMin ? uMinKey(p) : uMaxKey(p))
                                         : (edge == PatchEdge::VMin ? vMinKey(p) : vMaxKey(p));
                for (size_t id = state.begin[s]; id < state.end[s]; ++id)
                {
                    const Node &node = state.nodes[id];
                    GroupEntry entry;
                    entry.u = side_a ? node.ua : node.ub;
                    entry.v = side_a ? node.va : node.vb;
                    entry.pos = varies_v ? entry.v : entry.u;
                    entry.occ = seamOcc(state.node_root[id]);
                    if (varies_v)
                        add(vert_raw, line, entry, entry.v);
                    else
                        add(horiz_raw, line, entry, entry.u);
                }
            }
        }

        // Convert to sorted vectors.
        auto finalise = [](const std::map<AxisKey, std::map<OccKey, GroupEntry, OccKeyLess>,
                                          AxisKeyLess> &raw,
                           std::map<AxisKey, std::vector<GroupEntry>, AxisKeyLess> &out) {
            for (const auto &line : raw)
            {
                std::vector<GroupEntry> entries;
                entries.reserve(line.second.size());
                for (const auto &e : line.second)
                    entries.push_back(e.second);
                std::sort(entries.begin(), entries.end(),
                          [](const GroupEntry &a, const GroupEntry &b) {
                              if (a.pos != b.pos)
                                  return a.pos < b.pos;
                              OccKeyLess less;
                              return less(a.occ, b.occ);
                          });
                out[line.first] = entries;
            }
        };
        finalise(vert_raw, groups_[p].vertical);
        finalise(horiz_raw, groups_[p].horizontal);
    }
}

inline Tessellator::GroupEntry Tessellator::entryFromCorner(size_t patch, const VertexKey &key,
                                                            const SeamState &state) const
{
    GroupEntry entry;
    entry.u = axisValue(data_[patch].roots_u, key.u);
    entry.v = axisValue(data_[patch].roots_v, key.v);
    entry.pos = 0.0;
    entry.occ = resolveCorner(patch, key, state);
    return entry;
}

inline OccKey Tessellator::resolveCorner(size_t patch, const VertexKey &key,
                                         const SeamState &state) const
{
    for (int ei = 0; ei < 4; ++ei)
    {
        const int seam_id = data_[patch].seam_index[ei];
        if (seam_id < 0)
            continue;
        const PatchEdge edge = static_cast<PatchEdge>(ei);
        if (!cornerOnEdge(key, edge, patch))
            continue;
        const size_t s = static_cast<size_t>(seam_id);
        const bool side_a = seams_[s].patch_a == patch;
        const bool varies_v = (edge == PatchEdge::UMin || edge == PatchEdge::UMax);
        const AxisKey pos = varies_v ? key.v : key.u;
        const auto &map = side_a ? state.pos_a[s] : state.pos_b[s];
        auto it = map.find(pos);
        if (it != map.end())
            return seamOcc(state.node_root[it->second]);
    }
    return structuralOcc(patch, key);
}

/////////////////////////////////////////////////////////////////////
// Polygon extraction and error criterion
/////////////////////////////////////////////////////////////////////

inline std::vector<const Tessellator::GroupEntry *>
Tessellator::collectEdgeEntries(const std::vector<GroupEntry> *group, double lo, double hi,
                                bool ascending) const
{
    std::vector<const GroupEntry *> out;
    if (group == nullptr)
        return out;
    // The vector is sorted by pos.  Since edge groups are small, a linear
    // scan keeps the code simple and deterministic.
    for (const GroupEntry &entry : *group)
    {
        if (entry.pos > lo && entry.pos < hi)
            out.push_back(&entry);
    }
    if (!ascending)
        std::reverse(out.begin(), out.end());
    return out;
}

inline std::vector<Tessellator::PVert>
Tessellator::makePolygon(size_t patch, const Cell &cell, const SeamState &state,
                         PVert &center) const
{
    std::vector<PVert> poly;

    VertexKey c00 = cornerKey(patch, cell, 0);
    VertexKey c10 = cornerKey(patch, cell, 1);
    VertexKey c11 = cornerKey(patch, cell, 2);
    VertexKey c01 = cornerKey(patch, cell, 3);

    auto vertex = [&](const VertexKey &key) {
        PVert pv;
        pv.u = axisValue(data_[patch].roots_u, key.u);
        pv.v = axisValue(data_[patch].roots_v, key.v);
        pv.occ = resolveCorner(patch, key, state);
        return pv;
    };

    const PVert p00 = vertex(c00);
    const PVert p10 = vertex(c10);
    const PVert p11 = vertex(c11);
    const PVert p01 = vertex(c01);

    const auto &vg = groups_[patch].vertical;
    const auto &hg = groups_[patch].horizontal;
    auto find_group = [](const std::map<AxisKey, std::vector<GroupEntry>, AxisKeyLess> &m,
                         const AxisKey &key) -> const std::vector<GroupEntry> * {
        auto it = m.find(key);
        return it == m.end() ? nullptr : &it->second;
    };

    const std::vector<GroupEntry> *bottom = find_group(hg, c00.v);
    const std::vector<GroupEntry> *right = find_group(vg, c10.u);
    const std::vector<GroupEntry> *top = find_group(hg, c01.v);
    const std::vector<GroupEntry> *left = find_group(vg, c00.u);

    const double u0 = p00.u;
    const double u1 = p10.u;
    const double v0 = p00.v;
    const double v1 = p01.v;

    poly.push_back(p00);
    {
        auto entries = collectEdgeEntries(bottom, u0, u1, true);
        for (const GroupEntry *e : entries)
            poly.push_back(PVert{e->u, e->v, e->occ});
    }
    poly.push_back(p10);
    {
        auto entries = collectEdgeEntries(right, v0, v1, true);
        for (const GroupEntry *e : entries)
            poly.push_back(PVert{e->u, e->v, e->occ});
    }
    poly.push_back(p11);
    {
        auto entries = collectEdgeEntries(top, u0, u1, false);
        for (const GroupEntry *e : entries)
            poly.push_back(PVert{e->u, e->v, e->occ});
    }
    poly.push_back(p01);
    {
        auto entries = collectEdgeEntries(left, v0, v1, false);
        for (const GroupEntry *e : entries)
            poly.push_back(PVert{e->u, e->v, e->occ});
    }

    const unsigned int center_level = cell.level + 1;
    if (center_level >= 63)
        throw std::runtime_error("tessellatePatches: parameter refinement limit reached");
    AxisKey cu = canonicalAxis(cell.root_u, 2 * cell.ix + 1, center_level,
                               data_[patch].roots_u.size());
    AxisKey cv = canonicalAxis(cell.root_v, 2 * cell.iy + 1, center_level,
                               data_[patch].roots_v.size());
    center.u = axisValue(data_[patch].roots_u, cu);
    center.v = axisValue(data_[patch].roots_v, cv);
    center.occ = structuralOcc(patch, VertexKey{cu, cv});
    return poly;
}

inline glm::dvec3
Tessellator::positionFor(const OccKey &occ, size_t patch, double u, double v,
                         const SeamState &state,
                         std::map<OccKey, glm::dvec3, OccKeyLess> &cache) const
{
    auto it = cache.find(occ);
    if (it != cache.end())
        return it->second;
    glm::dvec3 position(0.0);
    if (occ.kind == 1)
    {
        // Use the deterministic representative node of this seam class.
        const size_t chosen = state.representative[occ.root];
        if (chosen >= state.nodes.size())
            throw std::runtime_error("tessellatePatches: seam vertex has no representative");
        const Node &node = state.nodes[chosen];
        const size_t source_patch = seams_[node.seam].patch_a;
        position = evaluate(source_patch, node.ua, node.va);
    }
    else
    {
        position = evaluate(patch, u, v);
    }
    cache.insert(std::make_pair(occ, position));
    return position;
}

inline bool Tessellator::trianglePasses(size_t patch, const PVert &a, const PVert &b,
                                        const PVert &c, const SeamState &state,
                                        std::map<OccKey, glm::dvec3, OccKeyLess> &cache,
                                        glm::dvec3 &out_a, glm::dvec3 &out_b,
                                        glm::dvec3 &out_c)
{
    out_a = positionFor(a.occ, patch, a.u, a.v, state, cache);
    out_b = positionFor(b.occ, patch, b.u, b.v, state, cache);
    out_c = positionFor(c.occ, patch, c.u, c.v, state, cache);

    const glm::dvec2 uv_a(a.u, a.v), uv_b(b.u, b.v), uv_c(c.u, c.v);
    // Edge midpoints and centroid of the parameter triangle.
    for (int sample = 0; sample < 4; ++sample)
    {
        double w0, w1, w2;
        if (sample == 0)
        {
            w0 = 0.5; w1 = 0.5; w2 = 0.0;
        }
        else if (sample == 1)
        {
            w0 = 0.5; w1 = 0.0; w2 = 0.5;
        }
        else if (sample == 2)
        {
            w0 = 0.0; w1 = 0.5; w2 = 0.5;
        }
        else
        {
            w0 = 1.0 / 3.0; w1 = 1.0 / 3.0; w2 = 1.0 / 3.0;
        }
        const glm::dvec2 uv = w0 * uv_a + w1 * uv_b + w2 * uv_c;
        const glm::dvec3 linear = w0 * out_a + w1 * out_b + w2 * out_c;
        const glm::dvec3 truth = evaluate(patch, uv.x, uv.y);
        if (glm::length(truth - linear) > options_.chord_tolerance)
        {
            return false;
        }
    }
    return true;
}

/////////////////////////////////////////////////////////////////////
// Refinement
/////////////////////////////////////////////////////////////////////

inline bool Tessellator::refineOnce(const SeamState &state, bool &changed)
{
    buildGroups(state);
    std::vector<std::pair<size_t, Cell>> to_split;
    std::map<OccKey, glm::dvec3, OccKeyLess> cache;

    for (size_t p = 0; p < leaves_.size(); ++p)
    {
        for (const Cell &cell : leaves_[p])
        {
            PVert center;
            std::vector<PVert> poly = makePolygon(p, cell, state, center);
            if (poly.size() < 3)
                throw std::runtime_error(
                    "tessellatePatches: degenerate parameter cell encountered");
            bool ok = true;
            for (size_t i = 0; i < poly.size(); ++i)
            {
                glm::dvec3 pa, pb, pc;
                if (!trianglePasses(p, center, poly[i], poly[(i + 1) % poly.size()], state,
                                    cache, pa, pb, pc))
                {
                    ok = false;
                    break;
                }
            }
            if (!ok)
            {
                if (cell.level >= options_.max_depth)
                {
                    throw std::runtime_error(
                        "tessellatePatches: max_depth exhausted before chord tolerance was "
                        "met");
                }
                to_split.push_back(std::make_pair(p, cell));
            }
        }
    }

    changed = !to_split.empty();
    for (const auto &item : to_split)
        splitCell(item.first, item.second);
    return changed;
}

inline void Tessellator::splitCell(size_t patch, const Cell &cell)
{
    auto it = leaves_[patch].find(cell);
    if (it == leaves_[patch].end())
        return;
    leaves_[patch].erase(it);
    if (cell.level >= kMaxCellLevel)
        throw std::runtime_error("tessellatePatches: numerical subdivision limit reached");
    const unsigned int level = cell.level + 1;
    for (int dy = 0; dy < 2; ++dy)
    {
        for (int dx = 0; dx < 2; ++dx)
        {
            Cell child;
            child.root_u = cell.root_u;
            child.root_v = cell.root_v;
            child.level = level;
            child.ix = 2 * cell.ix + static_cast<uint64_t>(dx);
            child.iy = 2 * cell.iy + static_cast<uint64_t>(dy);
            leaves_[patch].insert(child);
        }
    }
}

/////////////////////////////////////////////////////////////////////
// Seam geometry checks
/////////////////////////////////////////////////////////////////////

inline void Tessellator::checkSeamGeometry(const SeamState &state)
{
    for (size_t s = 0; s < seams_.size(); ++s)
    {
        const PatchSeam &seam = seams_[s];
        const size_t begin = state.begin[s];
        const size_t end = state.end[s];
        if (end <= begin)
            throw std::runtime_error(
                "tessellatePatches: seam has no sampled boundary vertices");
        for (size_t i = begin; i < end; ++i)
        {
            const Node &node = state.nodes[i];
            const glm::dvec3 pa = evaluate(seam.patch_a, node.ua, node.va);
            const glm::dvec3 pb = evaluate(seam.patch_b, node.ub, node.vb);
            if (glm::length(pa - pb) > options_.seam_tolerance)
            {
                throw std::runtime_error(
                    "tessellatePatches: seam vertex positions disagree beyond seam_tolerance");
            }
        }
        for (size_t i = begin; i + 1 < end; ++i)
        {
            const double tmid = 0.5 * (state.nodes[i].t + state.nodes[i + 1].t);
            const glm::dvec2 uva = mapToSide(seam, true, tmid);
            const glm::dvec2 uvb = mapToSide(seam, false, tmid);
            const glm::dvec3 pa = evaluate(seam.patch_a, uva.x, uva.y);
            const glm::dvec3 pb = evaluate(seam.patch_b, uvb.x, uvb.y);
            if (glm::length(pa - pb) > options_.seam_tolerance)
            {
                throw std::runtime_error(
                    "tessellatePatches: seam segment midpoints disagree beyond seam_tolerance");
            }
        }
    }
}

/////////////////////////////////////////////////////////////////////
// Mesh assembly
/////////////////////////////////////////////////////////////////////

inline TessellationMesh Tessellator::assemble(const SeamState &state)
{
    buildGroups(state);
    TessellationMesh mesh;
    std::map<OccKey, size_t, OccKeyLess> ids;
    std::map<OccKey, glm::dvec3, OccKeyLess> cache;

    auto get_id = [&](size_t patch, const PVert &v) -> size_t {
        auto it = ids.find(v.occ);
        if (it != ids.end())
            return it->second;
        const size_t id = mesh.positions.size();
        mesh.positions.push_back(positionFor(v.occ, patch, v.u, v.v, state, cache));
        ids.insert(std::make_pair(v.occ, id));
        return id;
    };

    for (size_t p = 0; p < leaves_.size(); ++p)
    {
        for (const Cell &cell : leaves_[p])
        {
            PVert center;
            std::vector<PVert> poly = makePolygon(p, cell, state, center);
            const size_t center_id = get_id(p, center);
            for (size_t i = 0; i < poly.size(); ++i)
            {
                const PVert &a = poly[i];
                const PVert &b = poly[(i + 1) % poly.size()];
                const size_t ia = get_id(p, a);
                const size_t ib = get_id(p, b);
                mesh.triangles.push_back(std::array<size_t, 3>{{center_id, ia, ib}});
                mesh.triangle_patch.push_back(p);
                mesh.triangle_uv.push_back(
                    std::array<glm::dvec2, 3>{{glm::dvec2(center.u, center.v),
                                              glm::dvec2(a.u, a.v),
                                              glm::dvec2(b.u, b.v)}});
            }
        }
    }
    return mesh;
}

/////////////////////////////////////////////////////////////////////
// Driver
/////////////////////////////////////////////////////////////////////

inline TessellationMesh Tessellator::run()
{
    if (!validated_)
        validateInputs();
    if (data_.empty())
        return TessellationMesh();

    initialiseLeaves();
    SeamState state;
    buildSeamState(state);
    checkSeamGeometry(state);

    size_t iterations = 0;
    while (true)
    {
        if (++iterations > 100000)
            throw std::runtime_error("tessellatePatches: refinement did not converge");
        buildSeamState(state);
        bool changed = false;
        refineOnce(state, changed);
        if (!changed)
            break;
    }

    buildSeamState(state);
    checkSeamGeometry(state);
    return assemble(state);
}

} // namespace tessellate
} // namespace internal

/**
 * Tessellate one or more rational surfaces into a single indexed triangle
 * mesh, conforming across declared seams.
 *
 * @param[in] patches Surfaces in double precision.  A non-rational Surface3d
 *   can be converted with RationalSurface3d(srf), which assigns unit weights.
 * @param[in] seams Declared shared boundary edges.  Every patch edge may
 *   take part in at most one seam.
 * @param[in] options Tessellation tolerances and depth limit.
 * @return The tessellated indexed triangle mesh.
 *
 * Throws std::invalid_argument for malformed surfaces, options or seam
 * declarations, and std::runtime_error when a tolerance or connection
 * requirement cannot be met within the depth limit.
 */
inline TessellationMesh tessellatePatches(
    const std::vector<RationalSurface3d> &patches,
    const std::vector<PatchSeam> &seams = std::vector<PatchSeam>(),
    const TessellationOptions &options = TessellationOptions())
{
    return internal::tessellate::Tessellator(patches, seams, options).run();
}

} // namespace tinynurbs

#endif // TINYNURBS_TESSELLATE_H
