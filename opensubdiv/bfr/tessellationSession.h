//
//   Copyright 2021 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

#ifndef OPENSUBDIV3_BFR_TESSELLATION_SESSION_H
#define OPENSUBDIV3_BFR_TESSELLATION_SESSION_H

///
/// @file tessellationSession.h
///
/// @brief A long-lived, genuinely incremental tessellation production
///        session.
///
/// TessellationSession retains the subdivision edit session for a fixed
/// topology control cage: every control point or per-face sampling rate
/// edit is turned into a MeshDelta instead of re-exporting the whole
/// triangle mesh. A separate process that has no control cage and never
/// touches Bfr can replay those deltas with MeshMirror.
///
/// Supported input: orientably consistent combinatorial 2-manifold
/// Catmull-Clark quad cages, possibly with open boundaries and multiple
/// disconnected components. Interior vertices have valence 4; boundary
/// vertices are a single open fan incident to one or two faces; boundary
/// interpolation is Sdc::VTX_BOUNDARY_EDGE_ONLY. There are no holes,
/// isolated vertices, explicit creases or corner sharpness. Topology is
/// fixed for the lifetime of the session; positions and sampling rates
/// may change repeatedly.
///
/// Each face requests a rate in [1, 32]. The effective rate of a base
/// edge is the maximum request of its incident faces; a face's own
/// inner rate is its own request. The five rates are handed to the
/// existing Bfr::Tessellation, which remains the only subdivision
/// evaluator and tessellator used.
///

#include "../version.h"

#include "../far/topologyRefiner.h"
#include "meshDelta.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Bfr {

///
/// @brief Absolute replacement of one coarse control point position.
///
struct PositionEdit {
    int index = 0;                          ///< Coarse vertex index
    std::array<double, 3> position = {0.0, 0.0, 0.0};
};

///
/// @brief Absolute replacement of one base face sampling rate.
///
struct RateEdit {
    int face = 0;                           ///< Base face index
    int rate = 0;                           ///< Requested rate in [1, 32]
};

///
/// @brief Incremental tessellation session over a fixed topology.
///
/// Construction builds the initial mesh and makes the 0 -> 1 delta
/// available through InitialDelta(). Update() validates and applies a
/// batch of absolute edits, returning the next delta; an edit batch
/// that changes nothing returns an empty delta at the unchanged
/// revision. Failed validation or evaluation rolls the complete session
/// state back, including the ID allocators.
///
/// All working state (compiled Surfaces, support dependencies, site
/// tables, ID allocators, published positions) is private and hidden
/// behind an opaque implementation.
///
class TessellationSession {
public:
    ///
    /// @brief Construct the session and the initial 0 -> 1 mesh delta.
    ///
    /// @param refiner          Fixed-topology Catmull-Clark refiner.
    /// @param controlPoints    @a numControlPoints triples of doubles.
    /// @param numControlPoints Number of coarse control points; must
    ///                         match the refiner's base vertex count.
    /// @param faceRates        One requested rate per base face.
    /// @param numFaceRates     Number of rate entries; must match the
    ///                         refiner's base face count.
    ///
    /// @throws std::invalid_argument on length mismatches, rates outside
    ///         [1, 32] or non-finite control point positions.
    /// @throws std::runtime_error if the initial evaluation produces
    ///         non-finite values or a degenerate parametric tangent
    ///         frame, or if two faces disagree on a shared position by
    ///         more than 1e-9.
    ///
    TessellationSession(Far::TopologyRefiner const & refiner,
                        std::array<double, 3> const * controlPoints,
                        size_t numControlPoints,
                        int const * faceRates,
                        size_t numFaceRates);

    ~TessellationSession();

    TessellationSession() = delete;
    TessellationSession(TessellationSession const &) = delete;
    TessellationSession & operator=(TessellationSession const &) = delete;

    /// @brief Number of base faces in the fixed topology.
    int GetNumFaces() const;

    /// @brief Number of coarse control points in the fixed topology.
    int GetNumControlPoints() const;

    /// @brief Current mesh revision (1 after construction).
    uint64_t Revision() const;

    /// @brief The initial 0 -> 1 delta that bootstraps a MeshMirror.
    MeshDelta const & InitialDelta() const;

    ///
    /// @brief Apply one transaction of absolute point and rate edits.
    ///
    /// Edits whose absolute values equal the current state are not
    /// changes. If the batch contains no effective change, the revision
    /// is not advanced and an empty delta at the current revision is
    /// returned. Otherwise the returned delta advances the revision by
    /// exactly one and contains only records of the faces actually
    /// re-evaluated or re-meshed.
    ///
    /// @throws std::invalid_argument on duplicate edit indices, out of
    ///         range indices, illegal rates or non-finite positions.
    /// @throws std::runtime_error on non-finite evaluation, a tangent
    ///         frame with ||Pu x Pv|| / (||Pu|| ||Pv||) <= 1e-12, or a
    ///         shared-position disagreement above 1e-9. On any throw
    ///         the full session state (control points, rates, mesh,
    ///         revision and next IDs) is rolled back.
    ///
    MeshDelta Update(std::vector<PositionEdit> const & pointEdits,
                     std::vector<RateEdit> const & rateEdits);

    ///
    /// @brief Faces evaluated by the most recent Update(), ascending and
    ///        without duplicates: exactly the union of faces supported by
    ///        changed control points and faces whose rate template
    ///        changed. Empty before the first Update or after a no-op.
    ///
    std::vector<int> const & EvaluatedFaces() const;

    ///
    /// @brief Faces whose five effective rates changed in the most recent
    ///        Update(), ascending and without duplicates. Empty before
    ///        the first Update or after a no-op.
    ///
    std::vector<int> const & RemeshedFaces() const;

    /// @brief Next position ID that would be allocated (never reused).
    uint64_t NextPositionId() const;

    /// @brief Next triangle ID that would be allocated (never reused).
    uint64_t NextTriangleId() const;

    ///
    /// @brief Owning snapshot with live positions and triangles sorted
    ///        by ID. The returned copy is unaffected by later edits.
    ///
    MeshSnapshot Snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // end namespace Bfr

} // end namespace OPENSUBDIV_VERSION
using namespace OPENSUBDIV_VERSION;
} // end namespace OpenSubdiv

#endif /* OPENSUBDIV3_BFR_TESSELLATION_SESSION_H */
