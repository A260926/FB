//
//   Copyright 2024 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

#ifndef OPENSUBDIV3_FAR_HIERARCHICAL_DISPLACEMENT_H
#define OPENSUBDIV3_FAR_HIERARCHICAL_DISPLACEMENT_H

#include "../version.h"

#include <memory>
#include <vector>

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Far {

class TopologyRefiner;
template <typename REAL> class StencilTableReal;
template <typename REAL> class LimitStencilTableReal;

/// \brief Descriptor of a limit-surface sample on the final sculpted layer.
///
/// \c face is a ptex face id (see Far::PtexIndices) of the control cage
/// produced by the last sculpting layer.  For Catmark quad cages the ptex
/// face id equals the quad face index; for Loop triangle cages each
/// triangle contributes three ptex sub-faces.  \c s and \c t follow the
/// Far ptex convention and name a smooth interior location of that ptex
/// face.  The sample set is fixed at plan construction time.
///
struct HierarchicalDisplacementSample {
    int    face = 0;
    double s = 0.0;
    double t = 0.0;
};

/// \brief Owned output of a successful HierarchicalDisplacement forward pass.
///
/// All arrays are owned by the result and have explicit lengths.  Vertex
/// arrays store packed XYZ triples, i.e. their length is three times the
/// number of vertices.
///
struct HierarchicalDisplacementResult {
    /// XYZ positions of every vertex of the final sculpted layer P_L.
    std::vector<double> finalLayerVertices;
    /// Limit-surface position of every requested sample, in sample order.
    std::vector<double> samplePositions;
    /// Unit limit-surface normal of every requested sample, in sample order.
    std::vector<double> sampleNormals;
};

/// \brief Reusable plan for layered subdivision with scalar displacement.
///
/// The plan performs L (1 <= L <= 3) sculpting layers.  At each layer the
/// previous geometry is subdivided with the regular Far subdivision rules,
/// then every refined vertex is moved along the unit area-weighted normal
/// of the current (pre-displacement) geometry by a per-vertex scalar
/// displacement.  The final sculpted layer is treated as a new control cage
/// and evaluated through the regular Far limit-surface machinery.
///
/// All topology, stencil and patch tables are built once by the constructor
/// and shared immutably between forwards.  Forward()/Pullback() never
/// rebuild any refiner, patch table or stencil table, and the plan as well
/// as the input refiner can be destroyed while tapes returned by
/// Forward() remain in use.
///
/// The supported domain is intentionally restricted:
///   - Catmark on oriented, manifold quad cages or Loop on oriented,
///     manifold triangle cages;
///   - no holes, no explicit edge/vertex creases, no isolated vertices;
///   - open boundaries, extraordinary valences and multiple disconnected
///     components are allowed;
///   - the boundary interpolation rule must be
///     Sdc::Options::VTX_BOUNDARY_EDGE_ONLY.
///
class HierarchicalDisplacement {
public:
    /// \brief Constructs a plan from a coarse-only TopologyRefiner.
    ///
    /// @param coarseRefiner  Refiner containing only the coarse level
    ///                       (GetMaxLevel() == 0).
    /// @param numLayers      Number of sculpting layers, in [1, 3].
    /// @param samples        Fixed sample locations on the final layer
    ///                       cage; must be non-empty and all ptex ids in
    ///                       range.
    ///
    /// Length, index-range, non-finite value and domain violations throw
    /// std::invalid_argument; failure to build the internal subdivision
    /// tables throws std::runtime_error.
    ///
    HierarchicalDisplacement(
        TopologyRefiner const & coarseRefiner,
        int numLayers,
        std::vector<HierarchicalDisplacementSample> const & samples);

    ~HierarchicalDisplacement();

    // Non-copyable: the plan owns heavy subdivision tables.
    HierarchicalDisplacement(HierarchicalDisplacement const &) = delete;
    HierarchicalDisplacement & operator=(
        HierarchicalDisplacement const &) = delete;

    int GetNumLayers() const;
    int GetNumSamples() const;

    /// Number of coarse control vertices (layer 0).
    int GetNumCoarseVertices() const;
    /// Number of vertices at the given layer; layer must be in [1, L].
    int GetNumLayerVertices(int layer) const;
    /// Number of faces of the final sculpted layer cage.
    int GetFinalNumFaces() const;
    /// Number of vertices per face of the final layer cage (4 or 3).
    int GetFinalFaceSize() const;
    /// Flat face-vertex indices of the final layer cage.
    std::vector<int> const & GetFinalFaceVertexIndices() const;

    /// \brief Tape recording one successful forward evaluation.
    ///
    /// A tape is self-contained: it owns (or shares immutable plan data and
    /// owns per-forward copies of every input) and remains valid after the
    /// originating plan, input refiner or input buffers are destroyed.
    /// Tapes never share mutable state.
    ///
    class Tape {
    public:
        ~Tape();

        /// Number of sculpting layers that produced this tape.
        int GetNumLayers() const;
        /// Pre-displacement subdivided points Q_l, packed XYZ, layer in [1,L].
        std::vector<double> const & GetSubdivPoints(int layer) const;
        /// Unit area normals n_l of layer l, packed XYZ, layer in [1,L].
        std::vector<double> const & GetUnitNormals(int layer) const;
        /// Post-displacement points P_l, packed XYZ, layer in [1,L].
        std::vector<double> const & GetLayerPoints(int layer) const;
        /// Limit u tangent Pu of every sample, packed XYZ in sample order.
        std::vector<double> const & GetSamplePu() const;
        /// Limit v tangent Pv of every sample, packed XYZ in sample order.
        std::vector<double> const & GetSamplePv() const;
        /// Copy of the coarse XYZ input used by this forward.
        std::vector<double> const & GetCoarseXYZ() const;
        /// Copy of the scalar displacement input of layer l in [1,L].
        std::vector<double> const & GetDisplacements(int layer) const;

        /// \brief Propagates sample adjoints back to the coarse points and
        ///        to every layer's scalar displacements.
        ///
        /// The propagation is fully analytic: it traverses the limit
        /// stencils, both unit-normal normalizations, the cross products of
        /// the area normals, the per-layer displacements and the linear
        /// subdivision stencils.  No finite differences, perturbing
        /// re-forwards or dense Jacobians are involved.
        ///
        /// Each output buffer is accumulated onto (never cleared), matching
        /// the convention of StencilTableReal::SparseTransposeAccumulate();
        /// zero-initialize buffers that hold a fresh gradient.
        ///
        /// @param samplePositionAdjoint  Three-component position adjoint
        ///                                per sample; its size must be
        ///                                3*GetNumSamples().
        /// @param sampleNormalAdjoint    Three-component unit-normal adjoint
        ///                                per sample; its size must be
        ///                                3*GetNumSamples().
        /// @param coarseXYZGradient      Accumulated coarse XYZ gradient;
        ///                                its size must be
        ///                                3*GetNumCoarseVertices().
        /// @param displacementGradients  One caller-owned scalar buffer per
        ///                                layer; buffer l must have size
        ///                                GetNumLayerVertices(l+1).
        ///
        /// Length or non-finite violations throw std::invalid_argument;
        /// the tape itself is never modified.
        ///
        void Pullback(std::vector<double> const & samplePositionAdjoint,
                      std::vector<double> const & sampleNormalAdjoint,
                      std::vector<double> & coarseXYZGradient,
                      std::vector<std::vector<double> *> const &
                          displacementGradients) const;

    private:
        friend class HierarchicalDisplacement;
        Tape();

        struct Data;
        std::shared_ptr<Data> _data;
    };

    /// \brief Runs the layered sculpting and limit-surface evaluation.
    ///
    /// @param coarseXYZ      Coarse XYZ positions; its size must be
    ///                       3*GetNumCoarseVertices(), packed per vertex.
    /// @param displacements  One scalar array per layer.  Array l-1 must
    ///                       have size GetNumLayerVertices(l) and is indexed
    ///                       by the vertex numbering of that layer.
    ///                       Displacement scalars may be positive, negative
    ///                       or zero.
    /// @param result         Filled with the final layer vertices and the
    ///                       sample positions and unit normals.
    ///
    /// @return A new tape for Pullback().  The tape does not alias any
    ///         caller buffer and remains valid after the plan and the
    ///         input refiner are destroyed.  A shared_ptr is returned so
    ///         that the tape's hidden implementation can remain incomplete
    ///         in client translation units.
    ///
    /// Length/index/non-finite violations throw std::invalid_argument.  A
    /// degenerate area normal (||A_v|| <= 1e-12 at any layer vertex) or a
    /// degenerate sample tangent frame (||Pu x Pv|| <= 1e-12) throws
    /// std::runtime_error; in that case no tape is published and previously
    /// produced tapes remain usable.
    ///
    std::shared_ptr<Tape> Forward(
            std::vector<double> const & coarseXYZ,
            std::vector<std::vector<double> > const & displacements,
            HierarchicalDisplacementResult & result) const;

private:
    struct PlanData;
    std::shared_ptr<PlanData> _plan;
};

} // end namespace Far

} // end namespace OPENSUBDIV_VERSION
using namespace OPENSUBDIV_VERSION;
} // end namespace OpenSubdiv

#endif // OPENSUBDIV3_FAR_HIERARCHICAL_DISPLACEMENT_H
