//
//   Copyright 2024 DreamWorks Animation LLC.
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//
#ifndef OPENSUBDIV3_FAR_CREASE_SENSITIVITY_TABLE_H
#define OPENSUBDIV3_FAR_CREASE_SENSITIVITY_TABLE_H

#include "../version.h"

#include "../far/types.h"

#include <cstddef>
#include <vector>

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Far {

class TopologyRefiner;

///
///  \brief Sparse stencils for subdivision positions and their first-order
///         sensitivity to a single semi-sharp crease edge.
///
///  For each vertex produced by a uniform Catmull-Clark refinement, a
///  CreaseSensitivityTable stores two sparse rows over the original coarse
///  control vertices:
///
///      - \c weights           : the ordinary subdivision position weights
///      - \c sharpnessWeights  : the derivative of that position with respect
///                               to the current sharpness of a single selected
///                               coarse edge
///
///  Both rows share the same sparse support (columns for which the sharpness
///  derivative happens to be zero are retained).  Rows are stored in standard
///  CSR layout: \c offsets gives the row boundaries, while \c indices,
///  \c weights and \c sharpnessWeights are equal-length, column-sorted arrays.
///  Row ordering matches the vertex ordering of TopologyRefiner::RefineUniform()
///  at the requested level (vertices-from-vertices first, then vertices-from-
///  faces, then vertices-from-edges), and columns within each row reference
///  coarse vertex indices in increasing order, with duplicates merged.
///
///  The table owns copies of all of its arrays.  It does not retain a reference
///  to the TopologyRefiner used to create it and remains fully evaluatable
///  once that refiner has been destroyed.
///
///  Sharpness derivatives are propagated analytically through the uniform
///  creasing and smooth/crease/corner blending rules of Sdc; they are never
///  computed by finite differences over rebuilt meshes.  Once the selected
///  edge's sharpness has decayed to zero at some level, subsequent smooth
///  subdivision continues to propagate the accumulated geometric sensitivity,
///  so the derivative field is never cleared.
///
class CreaseSensitivityTable {
public:
    ///
    ///  \brief Build the position and crease-sensitivity stencils.
    ///
    ///  @param base         A TopologyRefiner containing only the base level
    ///                      (i.e. not yet refined).  It is refined internally
    ///                      and left unchanged on return.
    ///  @param level        Number of uniform refinement iterations, 1 to 4.
    ///  @param coarseEdge   Index, in the base level, of the interior edge whose
    ///                      current sharpness is the differentiation variable.
    ///                      Its sharpness must be finite and strictly between
    ///                      0 and 10 and at least 0.05 away from any integer.
    ///
    ///  Only Catmull-Clark with CREASE_UNIFORM creasing and
    ///  VTX_BOUNDARY_EDGE_ONLY boundary interpolation is supported.  Any
    ///  unsupported option or topology, invalid index or disallowed sharpness
    ///  results in std::invalid_argument.
    ///
    static CreaseSensitivityTable const* Create(TopologyRefiner const& base,
                                                 int level,
                                                 Index coarseEdge);

    ~CreaseSensitivityTable();

    //  Non-copyable (use the owning pointer returned by Create):
    CreaseSensitivityTable(CreaseSensitivityTable const&) = delete;
    CreaseSensitivityTable& operator=(CreaseSensitivityTable const&) = delete;

    /// \brief Number of coarse control vertices (stencil column count).
    int GetNumControlVertices() const { return _numControlVertices; }

    /// \brief Number of refined vertices represented (stencil row count).
    int GetNumVertices() const { return (int)_offsets.size() - 1; }

    /// \brief CSR row boundaries, size GetNumVertices() + 1.
    std::vector<int> const& GetOffsets() const { return _offsets; }

    /// \brief Coarse vertex indices for every sparse entry, sorted per row.
    std::vector<Index> const& GetIndices() const { return _indices; }

    /// \brief Subdivision position weights, parallel to GetIndices().
    std::vector<double> const& GetWeights() const { return _weights; }

    /// \brief First derivative with respect to the selected edge sharpness,
    ///        parallel to GetIndices().
    std::vector<double> const& GetSharpnessWeights() const {
        return _sharpnessWeights;
    }

    ///
    ///  \brief Evaluate positions and sharpness derivatives for arbitrary
    ///         primvar data.
    ///
    ///  Data is laid out vertex-first with interleaved channels, i.e.
    ///  control[v*channels + c].  Both outputs are resized to
    ///  GetNumVertices()*channels entries with the same interleaving.
    ///
    ///  @param control      Length numControlVertices*channels, finite values.
    ///  @param channels     Positive channel count.
    ///  @param values       Distinct output vector for the refined positions.
    ///  @param derivatives  Distinct output vector for the sharpness
    ///                      derivatives.
    ///
    ///  Because each position row sums to one and each derivative row sums to
    ///  zero, translating every control vertex translates the values but
    ///  leaves the derivatives unchanged.
    ///
    void Evaluate(std::vector<double> const& control,
                  size_t channels,
                  std::vector<double>& values,
                  std::vector<double>& derivatives) const;

private:
    CreaseSensitivityTable();

    int                   _numControlVertices;
    std::vector<int>      _offsets;
    std::vector<Index>    _indices;
    std::vector<double>   _weights;
    std::vector<double>   _sharpnessWeights;
};

} // end namespace Far

} // end namespace OPENSUBDIV_VERSION
using namespace OPENSUBDIV_VERSION;
} // end namespace OpenSubdiv

#endif /* OPENSUBDIV3_FAR_CREASE_SENSITIVITY_TABLE_H */
