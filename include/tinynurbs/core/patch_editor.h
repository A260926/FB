/**
 * Simultaneous editing of a network of rational NURBS patches.
 *
 * A PatchEditor copies a set of rational surfaces and a set of whole-edge
 * seam declarations.  Every solve computes the control-point configuration of
 * minimum squared displacement from the rest configuration (the configuration
 * stored at construction time) subject to the exact seam constraints and the
 * currently staged hard point handles.
 *
 * Use of this source code is governed by a BSD-style license that can be found in
 * the LICENSE file.
 */

#ifndef TINYNURBS_PATCH_EDITOR_H
#define TINYNURBS_PATCH_EDITOR_H

#include "glm/glm.hpp"
#include "surface.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace tinynurbs
{

/**
 * The four boundary edges of a surface patch, expressed in the patch's own
 * (effective) parameter domain.
 */
enum class SurfaceEdge
{
    UMin = 0,
    UMax = 1,
    VMin = 2,
    VMax = 3
};

// Convenience aliases.
using Edge = SurfaceEdge;
using SeamEdge = SurfaceEdge;

/**
 * A whole-edge seam between two patches.
 *
 * `reversed` states whether the normalized along-edge parameter of patch B
 * runs opposite to that of patch A.  A patch edge may occur in at most one
 * seam declaration.
 */
struct PatchSeam
{
    int patch_a = 0;
    SurfaceEdge edge_a = SurfaceEdge::UMin;
    int patch_b = 0;
    SurfaceEdge edge_b = SurfaceEdge::UMin;
    bool reversed = false;
};

/**
 * A hard point handle: the surface point of `patch` at the given parameter in
 * the patch's original effective domain is constrained to `target`.
 *
 * Handles are staged with PatchEditor::set_handles() and consumed by the next
 * solve().  Ids must be unique.
 */
struct PatchHandle
{
    int id = 0;
    int patch = 0;
    double u = 0.0;
    double v = 0.0;
    glm::dvec3 target = glm::dvec3(0.0);
};

/**
 * An owned, read-only snapshot of a successful solve.
 *
 * `patches` are expressed in the same parameter domains as the surfaces given
 * to the constructor (knot vectors may have been refined by exact knot
 * insertion).  `handles` is the handle set that was committed by that solve.
 *
 * `original_knots_u` / `original_knots_v` hold, per patch, the knot vectors of
 * the surfaces supplied to the constructor.  They are kept so that mesh
 * generation can subdivide the *original* knot cells rather than the cells
 * introduced by seam compatibility refinement.
 */
struct PatchEditorSnapshot
{
    std::vector<RationalSurface3d> patches;
    std::vector<PatchSeam> seams;
    std::vector<PatchHandle> handles;
    std::vector<std::vector<double>> original_knots_u;
    std::vector<std::vector<double>> original_knots_v;
};

using Seam = PatchSeam;
using Handle = PatchHandle;
using Snapshot = PatchEditorSnapshot;

/**
 * Editor for a network of rational bicubic-or-lower CAD patches.
 *
 * Construction copies the surfaces and the seam declarations and refines the
 * along-edge knot bases of seamed edges with exact knot insertion.  The
 * Cartesian control points stored in that compatible basis are the fixed
 * "rest" configuration used by every subsequent solve.
 *
 * Errors:
 *  - std::invalid_argument for malformed input (bad degrees, non-clamped
 *    knots, non-positive or non-finite weights, non-finite coordinates,
 *    interior knot multiplicity above the degree, invalid seam declarations,
 *    invalid handle fields, duplicate handle ids, or the same surface point
 *    constrained to two different targets).
 *  - std::runtime_error when the input is well formed but the simultaneous
 *    seam and handle constraints are incompatible, or a numerical solve
 *    cannot be completed.
 */
class PatchEditor
{
  public:
    using Seam = PatchSeam;
    using Handle = PatchHandle;
    using Snapshot = PatchEditorSnapshot;

    /**
     * Copy the patches and seam declarations.
     * @throws std::invalid_argument on malformed input.
     */
    PatchEditor(std::vector<RationalSurface3d> patches, std::vector<PatchSeam> seams);

    ~PatchEditor();
    PatchEditor(PatchEditor &&) noexcept;
    PatchEditor &operator=(PatchEditor &&) noexcept;
    PatchEditor(const PatchEditor &) = delete;
    PatchEditor &operator=(const PatchEditor &) = delete;

    /**
     * Replace the staged handle list.  The currently staged list is left
     * untouched if the supplied list is invalid.
     * @throws std::invalid_argument on malformed handles.
     */
    void set_handles(std::vector<PatchHandle> handles);

    /**
     * Solve relative to the fixed rest configuration and, on success, commit
     * the resulting patch set as well as the staged handle set.
     *
     * On failure the editor returns to the state of the last successful solve
     * (initially the rest configuration with no handles) and throws.
     *
     * @throws std::runtime_error if the constraints are inconsistent or the
     *         numerical solve fails.
     */
    Snapshot solve();

    /** Number of numerical factorizations of the global constraint system. */
    std::size_t factorization_count() const;

    /** Currently staged (not necessarily committed) handles. */
    const std::vector<PatchHandle> &staged_handles() const;

    /** Snapshot of the last successful solve (initially the rest state). */
    Snapshot snapshot() const;

    /** Number of patches in the network. */
    std::size_t patch_count() const;

    /** Total number of Cartesian control points in the compatible basis. */
    std::size_t control_point_count() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tinynurbs

#endif // TINYNURBS_PATCH_EDITOR_H
