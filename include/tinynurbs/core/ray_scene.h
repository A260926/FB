/**
 * Direct ray intersection against rational NURBS surfaces without any
 * triangulation. The scene owns deep copies of its RationalSurface3d objects
 * and keeps a homogeneous-control-net bounding hierarchy per surface.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef TINYNURBS_RAY_SCENE_H
#define TINYNURBS_RAY_SCENE_H

#include "glm/glm.hpp"
#include "surface.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace tinynurbs
{

/**
 * A verified transverse (or verified tangent) ray/surface intersection.
 */
struct RaySurfaceHit
{
    /// Index of the surface in the vector given to the scene constructor.
    std::size_t surface_index = 0;
    /// Parameters of the root in the surface's original parameter domain.
    double u = 0.0, v = 0.0;
    /// Ray parameter such that point == origin + t * direction.
    double t = 0.0;
    /// Back-substituted point on the surface (not the raw Newton iterate).
    glm::dvec3 point{0.0};
    /// Unit normal oriented along Su x Sv. On knot lines the right-hand
    /// element is used; at the upper domain bound the last (left) element.
    glm::dvec3 normal{0.0};
};

/**
 * A parameter region that could not be classified as hit or miss because of
 * tangency, degeneracy or an exhausted box budget. It is never reported as a
 * miss.
 */
struct RaySurfaceUnresolved
{
    std::size_t surface_index = 0;
    double u_min = 0.0, u_max = 0.0;
    double v_min = 0.0, v_max = 0.0;
};

/**
 * Error categories returned before any ray work is performed.
 */
enum class RaySceneError
{
    None,
    ZeroDirection,       ///< direction is the zero vector
    NonFiniteInput,      ///< origin/direction contain NaN or infinity
    InvalidInterval,     ///< tmin/tmax non-finite or tmin >= tmax
    NonPositiveTolerance, ///< a tolerance is non-finite or <= 0
    ZeroBudget           ///< box_budget is 0
};

/**
 * Error categories for batched control point edits. A rejected batch leaves
 * the scene completely unchanged.
 */
enum class RaySceneEditError
{
    None,
    SurfaceIndexOutOfRange,
    ControlPointIndexOutOfRange,
    DuplicateAddress, ///< same (surface, row, column) appears twice
    NonFiniteValue,
    NonPositiveWeight
};

/**
 * Numerical knobs of a ray query.
 */
struct RayQueryOptions
{
    /// World-space tolerance on the O+tD back-substitution residual.
    double position_tolerance = 1e-8;
    /// Parameter-space tolerance used to stop adaptive subdivision.
    double parameter_tolerance = 1e-9;
    /// Maximum number of conservative parameter boxes visited.
    std::size_t box_budget = 100000;
};

/**
 * Result of a ray query. Hits are sorted by (t, surface_index, u, v).
 * Distinct UV roots are always kept separately, even when they coincide in
 * world space; only same-surface parameter roots rediscovered across a knot
 * boundary are merged.
 */
struct RayQueryResult
{
    RaySceneError error = RaySceneError::None;
    /// False when at least one unresolved record was produced.
    bool complete = true;
    std::vector<RaySurfaceHit> hits;
    std::vector<RaySurfaceUnresolved> unresolved;
    /// Actual number of conservative box/ray tests performed (tree nodes and
    /// adaptive sub-patches). Never a constant.
    std::size_t box_visits = 0;
    /// Subset of box_visits spent inside leaf patch subdivision.
    std::size_t patch_boxes_tested = 0;
};

/**
 * One absolute control point replacement. Degrees and knots never change.
 */
struct ControlPointEdit
{
    std::size_t surface_index = 0;
    std::size_t row = 0; ///< control point row (u-direction index)
    std::size_t column = 0; ///< control point column (v-direction index)
    glm::dvec3 position{0.0};
    double weight = 1.0;
};

/**
 * Outcome of a batched edit.
 */
struct RaySceneEditResult
{
    RaySceneEditError error = RaySceneEditError::None;
    /// Leaf elements whose Bezier support contains an edited point and that
    /// were actually re-extracted. Zero for an empty batch.
    std::size_t reextracted_elements = 0;
    /// Hierarchy nodes whose bounds were refit after the edit. The tree
    /// topology and leaf numbering never change.
    std::size_t refit_nodes = 0;
};

/**
 * Global scene counters, useful for proving hierarchical behaviour.
 */
struct RaySceneStats
{
    std::size_t surface_count = 0;
    std::size_t total_elements = 0;
    std::size_t total_tree_nodes = 0;
    /// Cumulative number of surface element (Bezier net) extractions,
    /// including the initial build and every partial update.
    std::size_t element_extractions = 0;
};

/**
 * Owning ray-traceable collection of rational surfaces.
 *
 * Supported inputs: 3D rational surfaces with degrees 1..3 in both
 * directions, clamped non-decreasing knot vectors whose interior knot
 * multiplicity does not exceed the degree, strictly positive finite weights
 * and regular geometry. Trimmed and periodic surfaces are not supported.
 * The constructor throws std::invalid_argument for unsupported surfaces.
 */
class RaySurfaceScene
{
  public:
    explicit RaySurfaceScene(const std::vector<RationalSurface3d> &surfaces);
    ~RaySurfaceScene();

    RaySurfaceScene(const RaySurfaceScene &other);
    RaySurfaceScene &operator=(const RaySurfaceScene &other);
    RaySurfaceScene(RaySurfaceScene &&other) noexcept;
    RaySurfaceScene &operator=(RaySurfaceScene &&other) noexcept;

    /**
     * Find every intersection origin + t * direction with t in
     * [tmin, tmax). The direction is not normalized. On a validation error
     * the returned result carries the error category and no hits or
     * unresolved records.
     */
    RayQueryResult intersect(glm::dvec3 origin, glm::dvec3 direction, double tmin, double tmax,
                             const RayQueryOptions &options = RayQueryOptions()) const;

    /**
     * Apply a batch of absolute control point edits. Duplicate addresses and
     * illegal values reject the whole batch. Knots and degrees are fixed.
     */
    RaySceneEditResult updateControlPoints(const std::vector<ControlPointEdit> &edits);

    RaySceneStats stats() const;
    std::size_t surfaceCount() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tinynurbs

#endif // TINYNURBS_RAY_SCENE_H
