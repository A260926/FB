/* ----------------------------------------------------------------------- *
 * This file is part of GEL, http://www.imm.dtu.dk/GEL
 * Copyright (C) the authors and DTU Informatics
 * For license and list of authors, see ../../doc/intro.pdf
 * ----------------------------------------------------------------------- */

/**
 * @file SurfaceFit.h
 * @brief Sampling-density independent fitting of an editable Manifold onto
 *        an independent snapshot of a target triangle surface.
 */

#ifndef __HMESH_SURFACEFIT_H__
#define __HMESH_SURFACEFIT_H__

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <GEL/CGLA/Vec3d.h>
#include <GEL/HMesh/Manifold.h>

namespace HMesh
{
    /// Outcome of a single SurfaceFitSession::iterate call.
    enum class SurfaceFitStatus
    {
        progress,    ///< a step was accepted and the source positions changed
        stationary,  ///< everything is fixed or the frozen proposal is null
        no_progress  ///< no acceptable backtracking step was found
    };

    /// User supplied fitting options. All three numerical options must be
    /// positive and finite. Fixed vertices stay pinned at their initial
    /// positions X0.
    struct SurfaceFitOptions
    {
        double lambda = 1.0;                 ///< Laplacian regularization weight
        double mu = 1.0;                     ///< pull-to-X0 regularization weight
        double d = 1.0;                      ///< truncation distance (positive, finite)
        std::vector<VertexID> fixed_vertices; ///< hard constrained source vertices
    };

    /// Self-owned report of one iteration. A failed iterate throws and leaves
    /// both the mesh and any previously returned report untouched.
    struct SurfaceFitReport
    {
        SurfaceFitStatus status = SurfaceFitStatus::stationary;
        double alpha = 0.0;              ///< accepted step length (0 when not progress)
        double energy_before = 0.0;      ///< full truncated objective at the start
        double energy_after = 0.0;       ///< full truncated objective at accepted X
        double residual = 0.0;           ///< relative inf-norm residual of the frozen CG solve
        std::size_t active_samples = 0;  ///< samples with matching distance strictly below d
        std::size_t node_visits = 0;     ///< total closest-point node visits this iteration
        std::size_t triangle_tests = 0;  ///< total closest-point triangle tests this iteration
        std::size_t sample_count = 0;    ///< total number of surface samples
    };

    /** @brief Owns a target snapshot and drives iterations on a source mesh.
     *
     * The session stores an independent double precision copy of the target
     * surface and builds its closest-point hierarchy exactly once. Later
     * modification or destruction of the target Manifold cannot affect the
     * session. The source Manifold must outlive the session; during that time
     * its positions are owned exclusively by the session while its topology
     * stays unchanged and it is neither compressed nor rebuilt.
     */
    class SurfaceFitSession
    {
    public:
        /** @brief Bind the editable source and snapshot the target.
         *  @throws std::invalid_argument for empty meshes, non-finite or
         *  out-of-range coordinates, degenerate/non-triangle faces, isolated
         *  vertices, invalid manifolds, bad options or invalid fixed handles.
         *  Throws before any source position is modified. */
        SurfaceFitSession(Manifold& source, const Manifold& target,
                          const SurfaceFitOptions& options);

        // The session holds a reference to the source; it is not copyable.
        SurfaceFitSession(const SurfaceFitSession&) = delete;
        SurfaceFitSession& operator=(const SurfaceFitSession&) = delete;
        ~SurfaceFitSession();

        /** @brief Run one match-and-solve iteration from the last accepted
         *  positions.
         *  @param max_cg positive upper bound on matrix-free CG iterations.
         *  @throws std::invalid_argument when max_cg is not positive;
         *  std::runtime_error when CG cannot reach the 1e-8 relative inf-norm
         *  residual within max_cg iterations or a non-finite value appears.
         *  On any failure the source positions are exactly as before the call. */
        SurfaceFitReport iterate(int max_cg);

        /// Number of source vertices owned by the session.
        std::size_t vertex_count() const;
        /// Number of surface samples (three per source face).
        std::size_t sample_count() const;

    private:
        struct Impl;
        Impl* impl;
    };
}

#endif
