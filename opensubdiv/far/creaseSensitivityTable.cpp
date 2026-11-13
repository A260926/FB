//
//   Copyright 2024 DreamWorks Animation LLC.
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//
#include "../far/creaseSensitivityTable.h"
#include "../far/topologyRefiner.h"
#include "../sdc/catmarkScheme.h"
#include "../sdc/crease.h"
#include "../sdc/options.h"
#include "../vtr/level.h"
#include "../vtr/refinement.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Far {

namespace {

typedef Vtr::internal::Level       Level;
typedef Vtr::internal::Refinement  Refinement;
typedef Vtr::Index                 Index;

//
//  A sparse stencil over the coarse control vertices for a single vertex at
//  the current refinement level.  Columns are kept sorted and merged.
//
struct LocalStencil {
    std::vector<Index>  idx;
    std::vector<double> w;
    std::vector<double> s;

    void addTerm(Index column, double weight, double sharpnessWeight) {
        std::vector<Index>::iterator it =
            std::lower_bound(idx.begin(), idx.end(), column);
        size_t pos = (size_t)(it - idx.begin());
        if (it != idx.end() && *it == column) {
            w[pos] += weight;
            s[pos] += sharpnessWeight;
        } else {
            idx.insert(it, column);
            w.insert(w.begin() + pos, weight);
            s.insert(s.begin() + pos, sharpnessWeight);
        }
    }

    //  Fold a parent-level stencil (pW position weight, pS sharpness weight
    //  for the parent point) into this child stencil.  The local mask itself
    //  may depend on the sharpness (maskW, maskS are the mask's value and its
    //  sharpness derivative):
    //      position     += maskW * pW
    //      sensitivity  += maskS * pW + maskW * pS
    //
    void addParent(LocalStencil const& parent, double maskW, double maskS) {
        for (size_t k = 0; k < parent.idx.size(); ++k) {
            addTerm(parent.idx[k],
                    maskW * parent.w[k],
                    maskS * parent.w[k] + maskW * parent.s[k]);
        }
    }
};

//
//  Sharpness classification always uses the float values stored by Vtr,
//  i.e. the exact same piecewise rules Sdc applies at run time.
//
inline bool sharpFloat(float s)  { return Sdc::Crease::IsSharp(s); }

//
//  Derivative of the uniform-crease fractional transition weight at a vertex
//  with respect to the selected edge sharpness.  This mirrors
//  Sdc::Crease::ComputeFractionalWeightAtVertex() in the uniform case, which
//  averages the parent edge sharpness values in (0, 1]; vertex sharpness is
//  zero throughout the supported domain.  The selected edge is guaranteed to
//  be at least 0.05 away from any integer, so float quantization cannot make
//  a genuinely changing term read as zero here.
//
double fractionalWeightDeriv(int                edgeCount,
                             float const*       parentSharpness,
                             double const*      parentSharpnessDeriv) {
    double derivSum = 0.0;
    int    count    = 0;
    for (int i = 0; i < edgeCount; ++i) {
        if (sharpFloat(parentSharpness[i]) && (parentSharpness[i] <= 1.0f)) {
            derivSum += parentSharpnessDeriv[i];
            ++count;
        }
    }
    if (count == 0) {
        return 0.0;
    }
    double deriv = derivSum / (double)count;
    //  The Sdc result is clamped to 1; the derivative vanishes on the clamp.
    float sum = 0.0f;
    for (int i = 0; i < edgeCount; ++i) {
        if (sharpFloat(parentSharpness[i]) && (parentSharpness[i] <= 1.0f)) {
            sum += parentSharpness[i];
        }
    }
    if (sum / (float)count >= 1.0f) {
        return 0.0;
    }
    return deriv;
}

//
//  Catmull-Clark vertex-vertex slot weights (one vertex slot, one edge slot
//  per incident edge and one face slot per incident face).  These duplicate
//  the assignments in sdc/catmarkScheme.h so that the matching analytic
//  derivatives can be formed alongside them.
//
void smoothVertexWeights(int      valence,
                         double&  vertexWeight,
                         double*  edgeWeights,
                         double*  faceWeights) {
    double f = 1.0 / (double)(valence * valence);
    vertexWeight = (double)(valence - 2) / (double)valence;
    for (int i = 0; i < valence; ++i) {
        edgeWeights[i] = f;
        faceWeights[i] = f;
    }
}

void creaseVertexWeights(int           valence,
                         int const     creaseEnds[2],
                         double&       vertexWeight,
                         double*       edgeWeights) {
    vertexWeight = 0.75;
    for (int i = 0; i < valence; ++i) {
        edgeWeights[i] = 0.0;
    }
    edgeWeights[creaseEnds[0]] = 0.125;
    edgeWeights[creaseEnds[1]] = 0.125;
}

//
//  Smooth edge-vertex weights for the supported (default triangle) Catmark
//  options.  Interior edges have two incident faces, boundary edges one.
//  The two endpoint weights are 1/4 and the remaining weight is split evenly
//  over the incident face centers.
//
void smoothEdgeWeights(int      faceCount,
                       double&  endpointWeight,
                       double&  faceWeight) {
    endpointWeight = 0.25;
    faceWeight = (faceCount == 2) ? 0.25 : (0.5 / (double)faceCount);
}

void requireSupported(bool condition, const char* message) {
    if (!condition) {
        throw std::invalid_argument(message);
    }
}

//
//  Validate everything the supported domain promises and mirror the tag
//  conventions established by TopologyRefinerFactory.
//
void validateBase(TopologyRefiner const& refiner, int level, Index coarseEdge) {

    requireSupported(level >= 1 && level <= 4,
        "CreaseSensitivityTable::Create(): level must be between 1 and 4");

    requireSupported(refiner.GetNumLevels() == 1,
        "CreaseSensitivityTable::Create(): refiner must contain the base level only");

    requireSupported(refiner.GetSchemeType() == Sdc::SCHEME_CATMARK,
        "CreaseSensitivityTable::Create(): only Catmull-Clark is supported");

    Sdc::Options options = refiner.GetSchemeOptions();
    requireSupported(options.GetCreasingMethod() == Sdc::Options::CREASE_UNIFORM,
        "CreaseSensitivityTable::Create(): only uniform creasing is supported");
    requireSupported(options.GetVtxBoundaryInterpolation()
                         == Sdc::Options::VTX_BOUNDARY_EDGE_ONLY,
        "CreaseSensitivityTable::Create(): only edge-only boundary interpolation is supported");
    requireSupported(options.GetTriangleSubdivision()
                         == Sdc::Options::TRI_SUB_CATMARK,
        "CreaseSensitivityTable::Create(): only default Catmark triangle weights are supported");

    requireSupported(refiner.GetNumFVarChannels() == 0,
        "CreaseSensitivityTable::Create(): face-varying channels are not supported");
    requireSupported(!refiner.HasHoles(),
        "CreaseSensitivityTable::Create(): hole faces are not supported");

    Level const& base = refiner.getLevel(0);

    requireSupported(coarseEdge >= 0 && coarseEdge < base.getNumEdges(),
        "CreaseSensitivityTable::Create(): coarse edge index is out of range");

    for (Index f = 0; f < base.getNumFaces(); ++f) {
        int corners = base.getFaceVertices(f).size();
        requireSupported(corners >= 3 && corners <= 8,
            "CreaseSensitivityTable::Create(): faces must have 3 to 8 corners");
        requireSupported(!base.getFaceTag(f)._hole,
            "CreaseSensitivityTable::Create(): hole faces are not supported");
    }

    for (Index v = 0; v < base.getNumVertices(); ++v) {
        requireSupported(base.getVertexEdges(v).size() > 0,
            "CreaseSensitivityTable::Create(): isolated vertices are not supported");
        requireSupported(!base.getVertexTag(v)._nonManifold,
            "CreaseSensitivityTable::Create(): mesh must be manifold");
        float vSharp = base.getVertexSharpness(v);
        requireSupported(vSharp == 0.0f,
            "CreaseSensitivityTable::Create(): explicit vertex sharpness must be zero");
    }

    for (Index e = 0; e < base.getNumEdges(); ++e) {
        float s = base.getEdgeSharpness(e);
        requireSupported(std::isfinite(s) && s >= 0.0f && s <= 10.0f,
            "CreaseSensitivityTable::Create(): edge sharpness must be in [0, 10]");
        requireSupported(!base.getEdgeTag(e)._nonManifold,
            "CreaseSensitivityTable::Create(): mesh must be manifold");
        requireSupported(base.getEdgeFaces(e).size() >= 1 &&
                         base.getEdgeFaces(e).size() <= 2,
            "CreaseSensitivityTable::Create(): mesh must be oriented and manifold");
    }

    //  The differentiated edge: interior, finite sharpness, away from integers
    //  (boundary edges are made infinitely sharp by the boundary rule).
    Level::ETag const& selectedTag = base.getEdgeTag(coarseEdge);
    requireSupported(!selectedTag._boundary && !selectedTag._nonManifold,
        "CreaseSensitivityTable::Create(): selected edge must be an interior edge");
    requireSupported(!selectedTag._infSharp && selectedTag._semiSharp,
        "CreaseSensitivityTable::Create(): selected edge must have finite sharpness");

    float selected = base.getEdgeSharpness(coarseEdge);
    requireSupported(selected > 0.0f && selected < 10.0f && std::isfinite(selected),
        "CreaseSensitivityTable::Create(): selected edge sharpness must be in (0, 10)");

    float nearestInteger = std::nearbyint(selected);
    requireSupported(std::fabs(selected - nearestInteger) >= 0.05f,
        "CreaseSensitivityTable::Create(): selected edge sharpness must be at least 0.05 from an integer");
}

//
//  Propagate position and sharpness-sensitivity stencils through one uniform
//  refinement.  Parent stencils are already expressed over coarse vertices;
//  the returned child stencils are as well.
//
//  edgeSharpnessDeriv holds d(edge sharpness)/d(selected sharpness) for every
//  parent edge; the equivalent child values are returned in
//  childEdgeSharpnessDeriv.
//
std::vector<LocalStencil>
propagateLevel(Refinement const&              refinement,
               std::vector<LocalStencil> const& parentStencils,
               std::vector<double> const&     edgeSharpnessDeriv,
               std::vector<double>&           childEdgeSharpnessDeriv) {

    Level const& parent = refinement.parent();
    Level const& child  = refinement.child();

    int numParentVerts = parent.getNumVertices();
    int numParentFaces = parent.getNumFaces();
    int numParentEdges = parent.getNumEdges();

    std::vector<LocalStencil> childStencils(child.getNumVertices());

    Sdc::Crease crease(refinement.getOptions());

    // ------------------------------------------------------------------
    //  Child edge sharpness sensitivities (uniform decrement, with the same
    //  smooth/infinite clamps used in Refinement::subdivideEdgeSharpness).
    // ------------------------------------------------------------------
    childEdgeSharpnessDeriv.assign(child.getNumEdges(), 0.0);
    for (Index e = 0; e < numParentEdges; ++e) {
        Vtr::ConstIndexArray childEdges = refinement.getEdgeChildEdges(e);
        for (int k = 0; k < 2; ++k) {
            Index ce = childEdges[k];
            if (child.getEdgeTag(ce)._semiSharp) {
                childEdgeSharpnessDeriv[ce] = edgeSharpnessDeriv[e];
            }
        }
    }

    // ------------------------------------------------------------------
    //  Vertices originating from faces.
    // ------------------------------------------------------------------
    for (Index f = 0; f < numParentFaces; ++f) {
        Index cv = refinement.getFaceChildVertex(f);
        if (!Vtr::IndexIsValid(cv)) continue;

        Vtr::ConstIndexArray fVerts = parent.getFaceVertices(f);
        double weight = 1.0 / (double)fVerts.size();

        //  Face-vertex masks depend only on topology: no sharpness derivative
        //  term, but parent sensitivity is propagated through the weights.
        for (int i = 0; i < fVerts.size(); ++i) {
            childStencils[cv].addParent(parentStencils[fVerts[i]], weight, 0.0);
        }
    }

    // ------------------------------------------------------------------
    //  Vertices originating from edges.
    // ------------------------------------------------------------------
    for (Index e = 0; e < numParentEdges; ++e) {
        Index cv = refinement.getEdgeChildVertex(e);
        if (!Vtr::IndexIsValid(cv)) continue;

        Vtr::ConstIndexArray eVerts = parent.getEdgeVertices(e);
        Vtr::ConstIndexArray eFaces = parent.getEdgeFaces(e);

        float parentEdgeSharp = parent.getEdgeSharpness(e);
        double parentEdgeDeriv = edgeSharpnessDeriv[e];

        Sdc::Crease::Rule parentRule =
            sharpFloat(parentEdgeSharp) ? Sdc::Crease::RULE_CREASE
                                       : Sdc::Crease::RULE_SMOOTH;
        Sdc::Crease::Rule childRule = child.getVertexRule(cv);

        bool isTransition = false;
        if (parentRule == Sdc::Crease::RULE_SMOOTH) {
            isTransition = false;
        } else if (childRule == Sdc::Crease::RULE_CREASE ||
                   parentEdgeSharp >= 1.0f) {
            isTransition = false;
        } else {
            //  Crease-to-smooth fractional blend (uniform creasing guarantees
            //  the child is smooth once the parent sharpness is below 1).
            isTransition = true;
        }

        //  Mask weights over the two endpoints and incident face centers.
        double endpointW = 0.5;
        double faceW     = 0.0;
        if (parentRule == Sdc::Crease::RULE_SMOOTH || isTransition) {
            smoothEdgeWeights(eFaces.size(), endpointW, faceW);
        }

        double blend = isTransition ? (double)parentEdgeSharp : 1.0;
        double smoothBlend = 1.0 - blend;

        //  Sharpness derivative of the local mask; zero away from a
        //  transition (see Scheme::ComputeEdgeVertexMask).
        double dBlend = isTransition ? parentEdgeDeriv : 0.0;

        for (int k = 0; k < 2; ++k) {
            double maskW, maskS;
            if (parentRule == Sdc::Crease::RULE_SMOOTH) {
                maskW = endpointW;              // 1/4 smooth weight
                maskS = 0.0;
            } else if (!isTransition) {
                maskW = 0.5;                    // pure crease midpoint
                maskS = 0.0;
            } else {
                //  blend * 0.5 + (1-blend) * smoothEndpoint
                maskW = blend * 0.5 + smoothBlend * endpointW;
                maskS = dBlend * (0.5 - endpointW);
            }
            childStencils[cv].addParent(parentStencils[eVerts[k]], maskW, maskS);
        }

        if (parentRule == Sdc::Crease::RULE_SMOOTH || isTransition) {
            for (int i = 0; i < eFaces.size(); ++i) {
                Index faceChild = refinement.getFaceChildVertex(eFaces[i]);
                double maskW, maskS;
                if (!isTransition) {
                    maskW = faceW;
                    maskS = 0.0;
                } else {
                    maskW = smoothBlend * faceW;
                    maskS = -dBlend * faceW;
                }
                childStencils[cv].addParent(childStencils[faceChild], maskW, maskS);
            }
        }
    }

    // ------------------------------------------------------------------
    //  Vertices originating from vertices.
    // ------------------------------------------------------------------
    int maxValence = parent.getMaxValence();
    std::vector<float>  parentEdgeSharp(maxValence);
    std::vector<float>  childEdgeSharp(maxValence);
    std::vector<double> parentEdgeDerivs(maxValence);
    std::vector<double> parentMaskVW(maxValence + 1);
    std::vector<double> parentMaskEW(maxValence);
    std::vector<double> parentMaskFW(maxValence);
    std::vector<double> childMaskVW(maxValence + 1);
    std::vector<double> childMaskEW(maxValence);
    std::vector<double> childMaskFW(maxValence);

    for (Index v = 0; v < numParentVerts; ++v) {
        Index cv = refinement.getVertexChildVertex(v);
        if (!Vtr::IndexIsValid(cv)) continue;

        Vtr::ConstIndexArray vEdges = parent.getVertexEdges(v);
        Vtr::ConstIndexArray vFaces = parent.getVertexFaces(v);
        int valence = vEdges.size();

        Sdc::Crease::Rule parentRule = parent.getVertexRule(v);
        Sdc::Crease::Rule childRule  = child.getVertexRule(cv);

        for (int i = 0; i < valence; ++i) {
            parentEdgeSharp[i] = parent.getEdgeSharpness(vEdges[i]);
            parentEdgeDerivs[i] = edgeSharpnessDeriv[vEdges[i]];
        }
        crease.SubdivideEdgeSharpnessesAroundVertex(
            valence, parentEdgeSharp.data(), childEdgeSharp.data());

        //  Build the parent and child rule masks over a common slot layout:
        //  slot 0 is the vertex itself, slots [1..valence] are edge-opposite
        //  vertices and the following slots the incident face centers.
        double& pVW = parentMaskVW[0];
        double& cVW = childMaskVW[0];
        double* pEW = parentMaskEW.data();
        double* cEW = childMaskEW.data();
        double* pFW = parentMaskFW.data();
        double* cFW = childMaskFW.data();

        for (int i = 0; i < valence; ++i) {
            pEW[i] = cEW[i] = pFW[i] = cFW[i] = 0.0;
        }
        pVW = cVW = 0.0;

        auto fillMask = [&](Sdc::Crease::Rule rule,
                            float const* edgeSharpness,
                            double& vw, double* ew, double* fw) {
            if (rule == Sdc::Crease::RULE_SMOOTH ||
                rule == Sdc::Crease::RULE_DART) {
                smoothVertexWeights(valence, vw, ew, fw);
            } else if (rule == Sdc::Crease::RULE_CREASE) {
                int creaseEnds[2];
                crease.GetSharpEdgePairOfCrease(edgeSharpness, valence, creaseEnds);
                creaseVertexWeights(valence, creaseEnds, vw, ew);
            } else {
                vw = 1.0;  // corner
            }
        };

        fillMask(parentRule, parentEdgeSharp.data(), pVW, pEW, pFW);

        //  Mirror Scheme::ComputeVertexVertexMask: smooth and dart parent
        //  rules apply the smooth mask directly and never enter a transition,
        //  regardless of the child rule.
        bool hasTransition = (childRule != parentRule) &&
                             parentRule != Sdc::Crease::RULE_SMOOTH &&
                             parentRule != Sdc::Crease::RULE_DART;
        double fractional = 0.0;
        double fractionalDeriv = 0.0;
        if (hasTransition) {
            fillMask(childRule, childEdgeSharp.data(), cVW, cEW, cFW);
            fractional = (double)crease.ComputeFractionalWeightAtVertex(
                0.0f, 0.0f, valence,
                parentEdgeSharp.data(), childEdgeSharp.data());
            fractionalDeriv = fractionalWeightDeriv(
                valence, parentEdgeSharp.data(), parentEdgeDerivs.data());
        }

        double childBlend = 1.0 - fractional;

        //  The mask weights themselves (smooth/crease/corner) only depend on
        //  which edges are sharp, a classification the 0.05 integer guard keeps
        //  locally invariant for changing edges.  All local sharpness
        //  dependence therefore enters through the fractional transition.
        //  Without a transition the parent rule is applied verbatim.
        auto slotWeights = [&](double pw, double cw) {
            if (!hasTransition) {
                return std::pair<double, double>(pw, 0.0);
            }
            return std::pair<double, double>(
                fractional * pw + childBlend * cw,
                fractionalDeriv * (pw - cw));
        };

        //  Vertex slot.
        std::pair<double, double> vslot = slotWeights(pVW, cVW);
        childStencils[cv].addParent(parentStencils[v], vslot.first, vslot.second);

        //  Edge slots: weights on the vertex opposite each parent edge.
        for (int i = 0; i < valence; ++i) {
            if (pEW[i] == 0.0 && cEW[i] == 0.0) continue;
            std::pair<double, double> eslot = slotWeights(pEW[i], cEW[i]);
            Vtr::ConstIndexArray eVerts = parent.getEdgeVertices(vEdges[i]);
            Index opposite = (eVerts[0] == v) ? eVerts[1] : eVerts[0];
            childStencils[cv].addParent(parentStencils[opposite],
                                        eslot.first, eslot.second);
        }

        //  Face slots: weights on the face center child vertices.
        for (int i = 0; i < vFaces.size(); ++i) {
            if (pFW[i] == 0.0 && cFW[i] == 0.0) continue;
            std::pair<double, double> fslot = slotWeights(pFW[i], cFW[i]);
            Index faceChild = refinement.getFaceChildVertex(vFaces[i]);
            childStencils[cv].addParent(childStencils[faceChild],
                                        fslot.first, fslot.second);
        }
    }

    return childStencils;
}

} // anonymous namespace

//
//  Public construction.
//
CreaseSensitivityTable const*
CreaseSensitivityTable::Create(TopologyRefiner const& base,
                               int level,
                               Index coarseEdge) {

    validateBase(base, level, coarseEdge);

    //  Work on a private copy so the caller's refiner (only its base level is
    //  shared) is not modified.
    TopologyRefiner work(base);

    TopologyRefiner::UniformOptions options(level);
    options.orderVerticesFromFacesFirst = false;
    options.fullTopologyInLastLevel = true;
    work.RefineUniform(options);

    Level const& baseLevel = work.getLevel(0);

    std::vector<double> edgeSharpnessDeriv(baseLevel.getNumEdges(), 0.0);
    edgeSharpnessDeriv[coarseEdge] = 1.0;

    //  Level-0 stencils are the trivial control-vertex stencils.
    std::vector<LocalStencil> stencils(baseLevel.getNumVertices());
    for (Index v = 0; v < baseLevel.getNumVertices(); ++v) {
        stencils[v].idx.push_back(v);
        stencils[v].w.push_back(1.0);
        stencils[v].s.push_back(0.0);
    }

    std::vector<double> nextEdgeDeriv;
    for (int l = 1; l <= level; ++l) {
        Refinement const& refinement = work.getRefinement(l - 1);
        stencils = propagateLevel(refinement, stencils,
                                  edgeSharpnessDeriv, nextEdgeDeriv);
        edgeSharpnessDeriv.swap(nextEdgeDeriv);
    }

    std::unique_ptr<CreaseSensitivityTable> result(new CreaseSensitivityTable());
    result->_numControlVertices = baseLevel.getNumVertices();

    result->_offsets.resize(stencils.size() + 1);
    result->_offsets[0] = 0;
    for (size_t row = 0; row < stencils.size(); ++row) {
        LocalStencil const& stencil = stencils[row];
        for (size_t k = 0; k < stencil.idx.size(); ++k) {
            result->_indices.push_back(stencil.idx[k]);
            result->_weights.push_back(stencil.w[k]);
            result->_sharpnessWeights.push_back(stencil.s[k]);
        }
        result->_offsets[row + 1] = (int)result->_indices.size();
    }

    return result.release();
}

CreaseSensitivityTable::CreaseSensitivityTable()
    : _numControlVertices(0) {
}

CreaseSensitivityTable::~CreaseSensitivityTable() = default;

void
CreaseSensitivityTable::Evaluate(std::vector<double> const& control,
                                 size_t channels,
                                 std::vector<double>& values,
                                 std::vector<double>& derivatives) const {

    if (channels == 0) {
        throw std::invalid_argument(
            "CreaseSensitivityTable::Evaluate(): channels must be positive");
    }
    if (&values == &derivatives) {
        throw std::invalid_argument(
            "CreaseSensitivityTable::Evaluate(): output vectors must be distinct");
    }
    if (control.size() != (size_t)_numControlVertices * channels) {
        throw std::invalid_argument(
            "CreaseSensitivityTable::Evaluate(): control vector has unexpected length");
    }
    for (size_t i = 0; i < control.size(); ++i) {
        if (!std::isfinite(control[i])) {
            throw std::invalid_argument(
                "CreaseSensitivityTable::Evaluate(): control values must be finite");
        }
    }

    int numRows = GetNumVertices();
    values.assign((size_t)numRows * channels, 0.0);
    derivatives.assign((size_t)numRows * channels, 0.0);

    for (int row = 0; row < numRows; ++row) {
        int begin = _offsets[row];
        int end   = _offsets[row + 1];
        for (int k = begin; k < end; ++k) {
            Index column = _indices[k];
            double w  = _weights[k];
            double sw = _sharpnessWeights[k];
            for (size_t c = 0; c < channels; ++c) {
                double src = control[(size_t)column * channels + c];
                values[(size_t)row * channels + c]      += w  * src;
                derivatives[(size_t)row * channels + c] += sw * src;
            }
        }
    }
}

} // end namespace Far

} // end namespace OPENSUBDIV_VERSION
} // end namespace OpenSubdiv
