//
//   Copyright 2026 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

#ifndef OPENSUBDIV3_BFR_SURFACE_FITTER_H
#define OPENSUBDIV3_BFR_SURFACE_FITTER_H

#include "../version.h"

#include "../sdc/types.h"

#include <memory>
#include <vector>

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Far {
    class TopologyRefiner;
}

namespace Bfr {

///
/// @brief Fits coarse control points to limit-surface position and normal
///        targets.
///
/// LimitSurfaceFitter solves for a new set of coarse (base level) control
/// points of a Catmull-Clark or Loop mesh such that the limit surface
/// evaluated at a fixed set of samples matches given target positions and
/// target normals as closely as possible:
///
/// \f[
///     E(X) = \sum_i w_{p_i} \left| P_i(X) - p_i \right|^2
///          + \sum_i w_{n_i} \left| n_i(X) - n^*_i \right|^2
///          + \lambda \sum_v \left| X_v - R_v \right|^2
/// \f]
///
/// where P_i, n_i are the limit-surface position and unit normal at sample i,
/// R_v is the rest position of coarse vertex v and X_v is an unknown. The
/// unit normal is n = normalize(Du x Dv), and target normals are normalized
/// when the sample data are supplied to Solve().
///
/// Geometric templates -- the limit-surface stencils for position, Du and Dv
/// at each sample -- are built exactly once, when the fitter is constructed.
/// Subsequent calls to Solve() reuse them, regardless of how many times the
/// targets or anchors change. All residual derivatives with respect to the
/// coarse control points are computed analytically from these templates:
/// no finite differences and no re-construction of the limit surface are
/// performed during a solve. The implementation is also sparse -- unknowns
/// are solved for with an implicitly applied block-sparse Hessian (never a
/// dense matrix of size 3V).
///
/// The following example constructs a fitter for a simple mesh and solves
/// for new positions and normals at a sample in the first face:
///
/// @code
/// #include <opensubdiv/far/topologyDescriptor.h>
/// #include <opensubdiv/far/topologyRefinerFactory.h>
/// #include <opensubdiv/bfr/surfaceFitter.h>
///
/// #include <memory>
/// #include <vector>
///
/// void example() {
///     using namespace OpenSubdiv;
///
///     int const sizes[6] = { 4, 4, 4, 4, 4, 4 };
///     int const faces[24] = { 0,3,2,1, 4,5,6,7, 0,1,5,4,
///                             1,2,6,5, 2,3,7,6, 3,0,4,7 };
///     double const rest[24] = { -1,-1,-1,  1,-1,-1,  1,1,-1,  -1,1,-1,
///                              -1,-1, 1,  1,-1, 1,  1,1, 1,  -1,1, 1 };
///
///     Far::TopologyDescriptor desc;
///     desc.numVertices = 8;
///     desc.numFaces = 6;
///     desc.numVertsPerFace = sizes;
///     desc.vertIndicesPerFace = faces;
///
///     typedef Far::TopologyRefinerFactory<Far::TopologyDescriptor> Factory;
///     std::unique_ptr<Far::TopologyRefiner> refiner(
///         Factory::Create(desc, Factory::Options(Sdc::SCHEME_CATMARK)));
///
///     std::vector<Bfr::LimitSurfaceFitter::FitSample> samples;
///     samples.push_back(
///         Bfr::LimitSurfaceFitter::FitSample(0, 0.4, 0.3, 1.0, 1.0));
///
///     Bfr::LimitSurfaceFitter fitter(*refiner, rest,
///                                    samples.data(), (int)samples.size());
///
///     std::vector<double> targets(3, 0.0);
///     std::vector<double> normals(3, 0.0);
///     targets[0] = 0.2; targets[1] = 0.1; targets[2] = 0.0;
///     normals[0] = 0.0; normals[1] = 0.0; normals[2] = 1.0;
///
///     Bfr::LimitSurfaceFitter::Options options;
///     options.SetLambda(1.0e-3).SetGradientTolerance(1.0e-7);
///
///     std::vector<Bfr::LimitSurfaceFitter::Anchor> anchors;
///     Bfr::LimitSurfaceFitter::Result result =
///         fitter.Solve(targets.data(), normals.data(),
///                      anchors.data(), (int)anchors.size(), options);
///
///     if (result.status == Bfr::LimitSurfaceFitter::STATUS_CONVERGED) {
///         // result.controlPoints holds 3 values for each coarse vertex.
///     }
/// }
/// @endcode
///
/// Samples must lie strictly inside a smooth sub-patch of the face they
/// reference -- i.e. away from extraordinary vertices, crease lines and
/// the boundaries between the face's parametric sub-faces. Holes, invalid
/// face or (u,v) coordinates, non-finite values, negative weights and
/// duplicate anchors are explicitly rejected.
///
class LimitSurfaceFitter {
public:
    /// @brief Integer type representing a mesh index
    typedef int    Index;

    /// @brief Floating point type used throughout (double precision)
    typedef double Real;

    ///
    /// @brief Status returned by construction and by each solve.
    ///
    /// STATUS_OK is only used to report successful construction. Solve()
    /// returns one of the remaining values, with CONVERGED, ITERATION_LIMIT
    /// and NO_PROGRESS indicating that a valid (accepted) solution is
    /// available in the result.
    ///
    enum Status {
        STATUS_OK = 0,              ///< constructed successfully
        STATUS_CONVERGED,           ///< gradient met the requested tolerance
        STATUS_ITERATION_LIMIT,     ///< nonlinear iteration limit reached
        STATUS_NO_PROGRESS,         ///< no further acceptable step could be found
        STATUS_INVALID_INITIAL,     ///< initial state violates the normal checks
        STATUS_INVALID_INPUT        ///< invalid input was supplied
    };

    ///
    /// @brief Specification of a sample point on a face of the mesh.
    ///
    /// The weights are non-negative. A sample with normalWeight == 0 does
    /// not contribute to the normal term, but its position and derivative
    /// templates are still available for position fitting and for the
    /// reported minimum sample area.
    ///
    struct FitSample {
        FitSample();
        FitSample(Index faceIndex, Real u, Real v,
                  Real positionWeight, Real normalWeight);

        Index faceIndex;        ///< base face index
        Real  u, v;             ///< (u,v) coordinate within the face
        Real  positionWeight;   ///< weight of the position term
        Real  normalWeight;     ///< weight of the normal term
    };

    ///
    /// @brief Specification of a hard anchor.
    ///
    /// An anchor fixes the control point of a coarse vertex exactly (it is
    /// eliminated from the unknowns -- it is not approximated with a large
    /// penalty). An anchor constructed from a vertex index alone is fixed
    /// at the rest position of that vertex; an explicit position overrides
    /// the rest position. A vertex may be anchored at most once per solve.
    ///
    struct Anchor {
        Anchor();
        explicit Anchor(Index vertexIndex);
        Anchor(Index vertexIndex, Real const position[3]);
        Anchor(Index vertexIndex, Real x, Real y, Real z);

        Index vertexIndex;
        Real  position[3];
        bool  positionIsRest;   ///< true if position is the rest position
    };

    ///
    /// @brief Options controlling a solve.
    ///
    /// The rest weight (lambda) must be strictly positive. The gradient
    /// tolerance is applied to the infinity norm of the gradient of the
    /// complete objective with respect to the free (non-anchored) control
    /// points.
    ///
    class Options {
    public:
        Options();

        /// @brief Assign the rest (regularization) weight (must be > 0)
        Options & SetLambda(Real lambda);
        /// @brief Return the rest weight
        Real      GetLambda() const { return _lambda; }

        /// @brief Assign the rest weight (alias of SetLambda)
        Options & SetRestWeight(Real lambda) { return SetLambda(lambda); }
        /// @brief Return the rest weight (alias of GetLambda)
        Real      GetRestWeight() const { return GetLambda(); }

        /// @brief Assign the free-gradient convergence tolerance
        Options & SetGradientTolerance(Real tolerance);
        /// @brief Return the free-gradient convergence tolerance
        Real      GetGradientTolerance() const { return _gradientTolerance; }

        /// @brief Assign the maximum number of nonlinear iterations
        Options & SetMaxIterations(int iterations);
        /// @brief Return the maximum number of nonlinear iterations
        int       GetMaxIterations() const { return _maxIterations; }

        /// @brief Assign the maximum number of line-search halvings
        Options & SetMaxLineSearchSteps(int steps);
        /// @brief Return the maximum number of line-search halvings
        int       GetMaxLineSearchSteps() const { return _maxLineSearchSteps; }

    private:
        Real _lambda;
        Real _gradientTolerance;
        int  _maxIterations;
        int  _maxLineSearchSteps;
    };

    ///
    /// @brief Metrics recorded for each accepted state.
    ///
    /// The energy is recomputed in full for the accepted state. The
    /// gradient norm is the infinity norm of the gradient of the complete
    /// objective with respect to the free control points. The minimum
    /// sample area is the smallest |Du x Dv| over all samples.
    ///
    struct IterationRecord {
        IterationRecord();

        Real energy;             ///< complete objective at the accepted state
        Real freeGradientNorm;   ///< infinity norm of the free gradient
        Real minSampleArea;      ///< min |Du x Dv| over all samples
        Real stepNorm;           ///< infinity norm of the accepted step
    };

    ///
    /// @brief Result of a solve.
    ///
    /// When status is CONVERGED, ITERATION_LIMIT or NO_PROGRESS the last
    /// accepted state is returned in controlPoints. Trial states that were
    /// rejected are never published. For INVALID_INITIAL, controlPoints (if
    /// non-empty) contains the initial state rest+anchors, which is not a
    /// valid solution. For INVALID_INPUT no solution is published.
    ///
    struct Result {
        Result();

        Status                    status;
        std::vector<Real>         controlPoints;  ///< 3 values per vertex
        std::vector<IterationRecord> iterations;  ///< one per accepted state
        int                       numTemplateBuilds; ///< templates built to date

        /// @brief Return true if a valid accepted solution is present
        bool HasSolution() const {
            return (status == STATUS_CONVERGED) ||
                   (status == STATUS_ITERATION_LIMIT) ||
                   (status == STATUS_NO_PROGRESS);
        }
        /// @brief Return true if the gradient tolerance was met
        bool IsConverged() const { return status == STATUS_CONVERGED; }
    };

public:
    //@{
    /// @name Construction and queries
    ///

    /// @brief Construct a fitter for the given mesh, rest points and samples
    ///
    /// @param mesh              base level topology of the mesh
    /// @param restControlPoints rest positions, 3 values per coarse vertex
    /// @param samples           sample specifications
    /// @param numSamples        number of samples
    ///
    /// Invalid construction leaves the fitter in an invalid state (test with
    /// IsValid()) and records the reason from GetConstructionStatus().
    ///
    LimitSurfaceFitter(Far::TopologyRefiner const & mesh,
                       Real const restControlPoints[],
                       FitSample const samples[], int numSamples);

    /// @brief Construct from vectors (convenience overload)
    LimitSurfaceFitter(Far::TopologyRefiner const & mesh,
                       std::vector<Real> const & restControlPoints,
                       std::vector<FitSample> const & samples);

    /// @brief Construct from a rest vector and a sample pointer/count
    LimitSurfaceFitter(Far::TopologyRefiner const & mesh,
                       std::vector<Real> const & restControlPoints,
                       FitSample const samples[], int numSamples);

    ~LimitSurfaceFitter();

    LimitSurfaceFitter(LimitSurfaceFitter const &) = delete;
    LimitSurfaceFitter & operator=(LimitSurfaceFitter const &) = delete;

    /// @brief Return true if construction succeeded
    bool IsValid() const;

    /// @brief Return the status recorded by construction
    Status GetConstructionStatus() const;

    /// @brief Return the number of coarse vertices in the mesh
    int GetNumVertices() const;

    /// @brief Return the number of samples
    int GetNumSamples() const;

    /// @brief Return the number of geometry templates built so far
    ///
    /// This count is incremented exactly once per sample when the fitter is
    /// constructed. Solve() reuses the templates and never increments it.
    ///
    int GetNumTemplateBuilds() const;

    /// @brief Return the mesh associated with the fitter
    Far::TopologyRefiner const & GetMesh() const;

    /// @brief Return the rest control points (3 values per vertex)
    Real const * GetRestControlPoints() const;
    //@}

public:
    //@{
    /// @name Solving
    ///

    /// @brief Solve for new control points given targets, normals and anchors
    ///
    /// @param targetPositions  target positions, 3 values per sample
    /// @param targetNormals    target normals, 3 values per sample (normalized
    ///                         internally; used only where normalWeight > 0)
    /// @param anchors          hard anchors, empty if none
    /// @param numAnchors        number of anchors
    /// @param options          solve options
    ///
    Result Solve(Real const targetPositions[],
                 Real const targetNormals[],
                 Anchor const anchors[], int numAnchors,
                 Options const & options = Options()) const;

    /// @brief Solve without anchors
    Result Solve(Real const targetPositions[],
                 Real const targetNormals[],
                 Options const & options = Options()) const;

    /// @brief Solve using vectors (convenience overload)
    Result Solve(std::vector<Real> const & targetPositions,
                 std::vector<Real> const & targetNormals,
                 std::vector<Anchor> const & anchors,
                 Options const & options = Options()) const;

    /// @brief Solve using vectors and no anchors
    Result Solve(std::vector<Real> const & targetPositions,
                 std::vector<Real> const & targetNormals,
                 Options const & options = Options()) const;

    /// @brief Lower-case convenience aliases for Solve()
    Result solve(std::vector<Real> const & targetPositions,
                 std::vector<Real> const & targetNormals,
                 std::vector<Anchor> const & anchors,
                 Options const & options = Options()) const {
        return Solve(targetPositions, targetNormals, anchors, options);
    }
    Result solve(std::vector<Real> const & targetPositions,
                 std::vector<Real> const & targetNormals,
                 Options const & options = Options()) const {
        return Solve(targetPositions, targetNormals, options);
    }
    //@}

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // end namespace Bfr

} // end namespace OPENSUBDIV_VERSION
using namespace OPENSUBDIV_VERSION;

} // end namespace OpenSubdiv

#endif /* OPENSUBDIV3_BFR_SURFACE_FITTER */
