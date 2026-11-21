// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file nanovdb/tools/FogAdjoint.h

    \brief CPU forward preview of a sparse NanoVDB fog density field together
           with an analytic vector-Jacobian product (VJP) that back-propagates
           pixel RGB and transmittance adjoints onto the stored density
           parameters, a global extinction coefficient and an RGB emission.

    \details The public surface is intentionally small:

      - fog::Catalog  : the parameter catalog built from the active storage of
                        a nanovdb::FloatGrid (one scalar per active leaf voxel
                        and per active constant tile, tiles are never expanded
                        into voxels). It also owns the support hierarchy used
                        to skip empty space during ray marching.
      - fog::forward  : the forward renderer. The density vector is an explicit
                        argument so it can be replaced (e.g. by an optimizer)
                        without rebuilding the catalog; the active topology is
                        fixed by the catalog.
      - fog::Record   : an owning record of a single forward pass. It deep
                        copies everything the backward pass needs, so it stays
                        valid after the source grid, the catalog and the input
                        vectors are destroyed or modified.
      - fog::vjp      : the analytic adjoint. Given per-ray RGB adjoints g and
                        transmittance adjoints q it returns the derivatives of
                        sum(g.RGB + q*T) with respect to the whole density
                        vector, the extinction k and the emission e.

    See README.md (section "Fog adjoint tool") for the exact definition of the
    parameter identities, the record ownership semantics and the limitations of
    the fixed sampling scheme.
*/

#ifndef NANOVDB_TOOLS_FOG_ADJOINT_H_HAS_BEEN_INCLUDED
#define NANOVDB_TOOLS_FOG_ADJOINT_H_HAS_BEEN_INCLUDED

#include <cstdint>
#include <memory>
#include <vector>

#include <nanovdb/NanoVDB.h>

namespace nanovdb {
namespace tools {
namespace fog {

/// @brief One finite ray segment in world space.
///
/// The ray is r(t) = origin + t * direction for t in [t0, t1]. The direction
/// does not need to be normalized; its norm only scales the world length
/// D = (t1 - t0) * |direction| over which the integral is taken.
struct Ray {
    Vec3d origin{0.0, 0.0, 0.0};
    Vec3d direction{0.0, 0.0, 1.0};
    double t0 = 0.0; ///< start of the parameter window (finite, t0 < t1)
    double t1 = 1.0; ///< end of the parameter window (finite, t0 < t1)
    Vec3d background{0.0, 0.0, 0.0}; ///< per-ray background RGB (finite)
};

/// @brief Result of one forward batch.
struct ForwardResult {
    std::vector<Vec3d> rgb;           ///< per-ray final color C + T * background
    std::vector<double> transmittance;///< per-ray final transmittance T
    uint64_t sampleEvaluations = 0;   ///< number of sample candidates actually evaluated
    uint64_t hierarchyVisits = 0;     ///< number of support-hierarchy node visits
};

/// @brief Parameter catalog of a FloatGrid's active density storage.
///
/// One scalar parameter is assigned to every active leaf voxel (level 0, edge
/// length 1) and to every active constant tile (levels 1/2/3, edge lengths
/// 8/128/4096). Tiles are kept as single scalars, never expanded to voxels.
/// Parameters are numbered in ascending (level, origin.x, origin.y, origin.z)
/// order. Active values of zero keep their parameter; inactive values are
/// always treated as zero and have no parameter, even when the stored value
/// is non-zero. Only grid maps of the form world = scale * index + translate
/// with a positive uniform scale are supported (negative indices are fine).
///
/// The catalog owns the lookup structures and the support hierarchy used for
/// empty-space skipping; it is cheap to share between any number of forward
/// calls but is not needed by the backward pass.
class Catalog
{
public:
    /// @brief Build the catalog from the active storage of @c grid.
    /// @throws std::invalid_argument if the grid map is not a positive uniform
    ///         scale plus translation.
    explicit Catalog(const FloatGrid& grid);

    ~Catalog();
    Catalog(Catalog&&) noexcept;
    Catalog& operator=(Catalog&&) noexcept;
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    /// @brief Number of density parameters.
    uint64_t paramCount() const;
    /// @brief Level of parameter @c i: 0 = voxel (edge 1), 1/2/3 = constant
    ///        tiles of edge length 8/128/4096.
    int paramLevel(uint64_t i) const;
    /// @brief Index-space origin (minimum corner) of parameter @c i's value region.
    Coord paramOrigin(uint64_t i) const;
    /// @brief Initial density vector gathered from the grid's active values.
    const std::vector<double>& initialDensity() const;

    /// @brief Uniform world scale of the grid map (world = scale*index + translate).
    double mapScale() const;
    /// @brief Translation of the grid map.
    Vec3d mapTranslate() const;

    class Impl;
private:
    std::unique_ptr<Impl> mImpl;
    friend struct CatalogAccess;
};

/// @brief Owning record of one forward pass, consumed by fog::vjp.
///
/// The record deep-copies every quantity the backward pass needs (per-segment
/// corner parameter indices and weights, densities, segment lengths, incoming
/// transmittances, per-ray backgrounds, k, e and the parameter count). It
/// therefore stays valid after the source GridHandle, the Catalog and the
/// input vectors are destroyed or modified. Records are move-only; distinct
/// records are fully independent and may be interleaved arbitrarily.
class Record
{
public:
    Record();
    ~Record();
    Record(Record&&) noexcept;
    Record& operator=(Record&&) noexcept;
    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;

    /// @brief Number of rays stored in the record.
    uint64_t rayCount() const;
    /// @brief Number of density parameters the record differentiates.
    uint64_t paramCount() const;
    /// @brief Number of recorded (actually evaluated) segments, summed over rays.
    uint64_t segmentCount() const;

    class Impl;
private:
    std::unique_ptr<Impl> mImpl;
    friend struct RecordAccess;
};

/// @brief Forward fog integration for a batch of rays.
///
/// For every ray the world length D = (t1-t0)*|d| is partitioned from t0 into
/// segments of world length @c h (the last segment takes the remaining length
/// L <= h). At each segment midpoint the density rho is sampled trilinearly:
/// integer coordinates are the sample nodes, octant weights come from the
/// floor and fractional parts, and missing or inactive corners contribute 0.
/// With a = exp(-rho*k*Lseg) the recurrence
///   C += T * rho * e * (1-a)/(rho*k),  T *= a
/// is accumulated from C = 0, T = 1; a zero denominator falls back to the
/// continuous limit Lseg. The final color is C + T*background. Integration is
/// not clipped to the active bounding box (the one-cell trilinear support
/// halo also contributes) and never terminates early on transmittance.
///
/// @param catalog   parameter catalog (defines the active topology).
/// @param density   replacement density vector, size must equal
///                  catalog.paramCount(); all entries finite and non-negative.
/// @param extinction global extinction coefficient k, finite and non-negative.
/// @param emission   RGB emission e, finite and non-negative.
/// @param rays       finite rays; directions non-zero, windows finite and increasing.
/// @param h          world step length, finite and strictly positive.
/// @param record     optional output; when non-null it is filled with an owning
///                   record of this pass suitable for fog::vjp.
/// @throws std::invalid_argument on any invalid input, before any work is done.
ForwardResult forward(const Catalog& catalog,
                      const std::vector<double>& density,
                      double extinction,
                      const Vec3d& emission,
                      const std::vector<Ray>& rays,
                      double h,
                      Record* record = nullptr);

/// @brief Result of one vector-Jacobian product.
struct VJPResult {
    std::vector<double> dDensity;     ///< d sum(g.RGB + q*T) / d density (size = paramCount)
    double dExtinction = 0.0;         ///< d sum(g.RGB + q*T) / d k
    Vec3d dEmission{0.0, 0.0, 0.0};   ///< d sum(g.RGB + q*T) / d e
};

/// @brief Analytic adjoint of fog::forward.
///
/// Returns the derivatives of sum over rays of (g_r . RGB_r + q_r * T_r) with
/// respect to the full density vector, k and e. Parameters never touched by
/// any sample receive exactly zero. The propagation is fully analytic (no
/// perturbation, no re-rendering, no automatic differentiation); at rho = 0
/// or k = 0 the right derivatives of the analytic continuous limits are used.
/// Every call starts from zero, so repeated and interleaved calls on distinct
/// records are independent. All accumulation is done in double precision and
/// the cost is linear in the number of recorded segments and parameters.
///
/// @throws std::invalid_argument if the adjoint sizes do not match the record.
VJPResult vjp(const Record& record,
              const std::vector<Vec3d>& rgbAdjoint,
              const std::vector<double>& transmittanceAdjoint);

} // namespace fog
} // namespace tools
} // namespace nanovdb

#endif // NANOVDB_TOOLS_FOG_ADJOINT_H_HAS_BEEN_INCLUDED
