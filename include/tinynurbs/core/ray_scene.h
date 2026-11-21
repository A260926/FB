/**
 * Direct ray intersection with rational NURBS surfaces.
 *
 * RaySurfaceScene owns a copy of a list of RationalSurface3d surfaces and
 * answers all intersections of a ray O + tD with the true parametric
 * surfaces, without any triangulation. A homogeneous-control-net bounding
 * hierarchy provides conservative pruning; roots are then found on the
 * actual rational surfaces by Bezier subdivision followed by Newton
 * polishing.
 *
 * Supported surfaces: 3D rational surfaces, degrees 1..3 in both directions,
 * clamped (open) knot vectors with interior knot multiplicity at most equal
 * to the degree, strictly positive finite weights and regular parametrization.
 * Trimmed and periodic surfaces are not supported.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef TINYNURBS_RAY_SCENE_H
#define TINYNURBS_RAY_SCENE_H

#include "tinynurbs/core/surface.h"
#include "glm/glm.hpp"
#include <cstddef>
#include <memory>
#include <vector>

namespace tinynurbs
{

/**
 * Error categories reported by scene construction, queries and control point
 * updates.
 */
enum class RaySceneError
{
    /// No error.
    None,
    /// A surface handed to the constructor fails the supported-class checks
    /// (bad degree, non-clamped knots, bad multiplicity, non-finite or
    /// non-positive weights, non-finite control points, singular parametrization).
    InvalidSurface,
    /// Ray direction is zero or contains non-finite values, or the origin is
    /// not finite.
    InvalidRay,
    /// t interval is not finite or tmin >= tmax.
    InvalidInterval,
    /// One of the tolerances is not a strictly positive finite number.
    InvalidTolerance,
    /// The work budget is zero.
    InvalidBudget,
    /// An update references an out-of-range surface/control point or carries a
    /// non-finite position or a non-positive finite weight.
    InvalidControlPoint,
    /// An update batch references the same (surface, row, column) twice.
    DuplicateControlPoint
};

/**
 * Why a parameter region could not be resolved to verified hits.
 */
enum class UnresolvedReason
{
    /// A tangential contact is suspected but cannot be verified.
    Tangency,
    /// The surface normal degenerates inside the region.
    Degenerate,
    /// The box budget was exhausted while the region could still contain
    /// roots.
    Budget
};

/**
 * Numerical options for a ray query.
 */
struct RayOptions
{
    /// World-space tolerance: every reported hit satisfies
    /// ||S(u,v) - (O + tD)|| <= spatial_tolerance.
    double spatial_tolerance = 1e-8;
    /// Parameter-space tolerance at which subdivision stops and roots are
    /// considered identical.
    double parameter_tolerance = 1e-9;
    /// Maximum number of parameter boxes that may be processed.
    unsigned long long box_budget = 100000;
};

/**
 * A single verified ray/surface intersection.
 */
struct RayHit
{
    /// Index of the surface in the list passed to the scene constructor.
    unsigned int surface_index = 0;
    /// Parameters of the root in the original surface domain.
    double u = 0.0, v = 0.0;
    /// Ray parameter, with the un-normalized direction D.
    double t = 0.0;
    /// Back-substituted surface point S(u, v).
    glm::dvec3 position{0.0};
    /// Unit normal oriented by Su x Sv (right element on knot lines, left
    /// element at the upper domain bound).
    glm::dvec3 normal{0.0};
};

/**
 * A parameter region that may contain intersections but could not be
 * resolved. It is never silently treated as a miss.
 */
struct RayUnresolved
{
    unsigned int surface_index = 0;
    /// UV range enclosing the unresolved region, in the original domain.
    double u_min = 0.0, u_max = 0.0;
    double v_min = 0.0, v_max = 0.0;
    UnresolvedReason reason = UnresolvedReason::Tangency;
};

/**
 * Result of a ray query. Hits and unresolved records are owned by the result
 * and stay valid after later scene updates or scene destruction.
 */
struct RayQueryResult
{
    /// False when at least one unresolved record is present.
    bool complete = false;
    /// Rejection reason for the query; None when the query was accepted.
    RaySceneError error = RaySceneError::None;
    /// Verified hits, sorted by (t, surface_index, u, v). Distinct UV roots
    /// are all retained, even when spatially coincident.
    std::vector<RayHit> hits;
    /// Regions that could not be ruled out or verified.
    std::vector<RayUnresolved> unresolved;
    /// Actual number of BVH nodes opened during the query.
    unsigned long long bvh_nodes_visited = 0;
    /// Actual number of parametric surface boxes processed.
    unsigned long long parameter_boxes_visited = 0;
    /// Total box accesses (BVH nodes plus parametric boxes).
    unsigned long long boxes_visited = 0;
};

/**
 * One absolute control point replacement.
 */
struct ControlPointEdit
{
    unsigned int surface_index = 0;
    unsigned int row = 0;       ///< Control point row (u direction).
    unsigned int column = 0;    ///< Control point column (v direction).
    glm::dvec3 position{0.0};   ///< New absolute position.
    double weight = 1.0;        ///< New strictly positive finite weight.
};

/**
 * Outcome of a batched control point update.
 */
struct RaySceneUpdateStats
{
    /// True only when the whole batch was applied.
    bool accepted = false;
    RaySceneError error = RaySceneError::None;
    /// Number of surface elements whose Bezier control net was re-extracted.
    std::size_t elements_extracted = 0;
    /// Number of hierarchy nodes whose bounding box was refit. Tree topology
    /// and leaf numbering never change.
    std::size_t bvh_nodes_refit = 0;
};

/**
 * Owning scene of rational surfaces that answers direct ray queries against
 * the parametric NURBS representation.
 */
class RaySurfaceScene
{
  public:
    /**
     * Construct by copying the given surfaces. If any surface is outside the
     * supported class, status() returns the rejection category and every query
     * is refused; the constructor itself does not throw for bad input.
     */
    explicit RaySurfaceScene(const std::vector<RationalSurface3d> &surfaces);
    ~RaySurfaceScene();

    RaySurfaceScene(const RaySurfaceScene &) = delete;
    RaySurfaceScene &operator=(const RaySurfaceScene &) = delete;
    RaySurfaceScene(RaySurfaceScene &&) noexcept;
    RaySurfaceScene &operator=(RaySurfaceScene &&) noexcept;

    /// Construction status; None when the scene is usable.
    RaySceneError status() const;
    std::size_t surfaceCount() const;
    /// Number of knot elements of one surface (non-zero knot intervals product).
    std::size_t elementCount(std::size_t surface_index) const;
    std::size_t leafCount() const;
    std::size_t nodeCount() const;
    /// Cumulative number of element Bezier extractions (build plus updates).
    unsigned long long totalElementsExtracted() const;

    /**
     * Find every intersection O + tD with t in [tmin, tmax). D is used
     * un-normalized. Invalid rays, intervals, tolerances and budgets are
     * rejected before any work is done.
     */
    RayQueryResult intersect(const glm::dvec3 &origin, const glm::dvec3 &direction, double tmin,
                             double tmax, const RayOptions &options) const;

    /**
     * Apply a batch of absolute control point edits. Knots and degrees are
     * untouched. A duplicate address or any illegal value rejects the whole
     * batch and leaves the scene queryable in its previous state. An empty
     * batch performs no extraction. Only elements supported by the edited
     * basis functions are re-extracted, and only the hierarchy nodes above
     * those leaves are refit.
     */
    RaySceneUpdateStats updateControlPoints(const std::vector<ControlPointEdit> &edits);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tinynurbs

#endif // TINYNURBS_RAY_SCENE_H
