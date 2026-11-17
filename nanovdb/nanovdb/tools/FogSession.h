// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file nanovdb/tools/FogSession.h

    \brief Incremental, resumable CPU ray integration through one or more
           sparse NanoVDB fog volumes.

    A @c FogSession walks every volume's NanoVDB node hierarchy lazily along a
    single world-space ray and merges the volumes' constant-density intervals
    as a function of the world ray parameter.  Work can be split over several
    calls to @c advance() so that a renderer can trade a bounded amount of
    traversal per frame for latency.

    \section fog_session_optics Optics

    A voxel is the half-open index-space box [i, i+1).  Only *active* voxels
    and *active* constant tiles contribute; inactive values are vacuum even if
    their stored value is non-zero.  The density of an active value is
    max(value, 0).  For a constant-coefficient world-space segment of length
    @c L the overlapping volumes are combined as

    \code
        sigma = sum(density * extinction)
        j     = sum(density * emission)
        C    += T * j * (1 - exp(-sigma * L)) / sigma     // sigma > 0
        C    += T * j * L                                 // sigma == 0
        T    *= exp(-sigma * L)
    \endcode

    integrated from near to far.  The completed result is
    @c radiance = C + T * background.  An incomplete result returns the
    committed @c C and @c T without the background term.

    \section fog_session_traversal Traversal and resumability

    Each volume keeps a cursor for its current constant hierarchy interval.
    Cursors are advanced only far enough to answer the next world-space
    boundary, so the session never expands a grid densely, never uses a fixed
    small step size, and never collects the whole ray's events up front.
    Persistent storage is proportional to the number of volumes (and the tree
    depth), not to the number of voxels traversed.  @c spanSteps() reports how
    many hierarchy constant intervals have actually been fetched or advanced,
    including vacuum intervals.

    @c advance(budget) performs at most @c budget such span steps and returns
    whether the ray has been fully integrated.  A budget of zero makes no
    progress.  Once complete, further calls return @c true and leave the
    result unchanged.  Sessions do not share mutable state and may be advanced
    in an interleaved fashion.  Copying a session is disabled.

    \section fog_session_example Example

    \code
    #include <nanovdb/tools/FogSession.h>

    std::vector<nanovdb::tools::FogVolume> volumes;
    volumes.push_back({gridA, 0.25, nanovdb::Vec3d(0.1, 0.2, 0.3)});
    volumes.push_back({gridB, 0.50, nanovdb::Vec3d(0.0, 0.4, 0.1)});

    nanovdb::math::Ray<double> ray(nanovdb::Vec3d(-20, 0, 0),
                                   nanovdb::Vec3d(1, 0, 0));
    nanovdb::tools::FogSession session(volumes, ray, nanovdb::Vec3d(0.2));

    while (!session.advance(64)) {
        // ... hand control back to the renderer ...
    }
    const nanovdb::tools::FogResult result = session.result();
    \endcode

    The grids are owned by the caller and must stay alive and unmodified for
    the lifetime of the session.

    \note The supported grid mapping is a positive uniform scale combined with
          a translation (the mapping written by the bundled grid builders and
          primitives).  The world ray direction need not be normalized and the
          ray interval must be finite.
*/

#ifndef NANOVDB_TOOLS_FOGSESSION_H_HAS_BEEN_INCLUDED
#define NANOVDB_TOOLS_FOGSESSION_H_HAS_BEEN_INCLUDED

#include <nanovdb/NanoVDB.h>
#include <nanovdb/math/Ray.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace nanovdb {// ==========================================================

namespace tools {// ==========================================================

/// @brief One participating fog volume.
struct FogVolume
{
    /// @brief Grid sampled by this volume.  Must remain alive and unmodified.
    const FloatGrid* grid = nullptr;
    /// @brief Extinction coefficient multiplied by the active density.
    double extinction = 0.0;
    /// @brief Emission colour multiplied by the active density.
    Vec3d emission = Vec3d(0.0);
};

/// @brief Result of a (possibly partial) fog integration.
struct FogResult
{
    /// @brief Remaining transmittance of the committed interval.
    double transmittance = 1.0;
    /// @brief Accumulated radiance.  Includes the background only once the
    ///        session has completed.
    Vec3d radiance = Vec3d(0.0);
};

/// @brief Incremental, resumable integration of a world-space ray through
///        several sparse NanoVDB fog volumes.
///
/// @details Construction does not traverse any grid.  Traversal happens in
///          @c advance(), which performs at most @p budget hierarchy span
///          steps.  See the file documentation for the optical model and for
///          an example.
class FogSession
{
public:
    /// @brief Start a session.
    /// @param volumes   Participating volumes.  The grids are borrowed and
    ///                  must outlive the session and stay unmodified.
    /// @param ray       World-space ray.  Its direction must be non-zero and
    ///                  its [t0, t1] interval is used as the integration range.
    /// @param background Background radiance added behind the ray once the
    ///                  session has completed.
    FogSession(std::vector<FogVolume> volumes,
               const math::Ray<double>& ray,
               const Vec3d& background);

    ~FogSession();

    /// @brief Sessions cannot be copied because each owns traversal state.
    FogSession(const FogSession&) = delete;
    FogSession& operator=(const FogSession&) = delete;

    /// @brief Moves are allowed.
    FogSession(FogSession&&) noexcept;
    FogSession& operator=(FogSession&&) noexcept;

    /// @brief Advance the traversal by at most @p budget span steps.
    /// @return @c true once the whole ray interval has been integrated.
    ///         A @p budget of zero makes no progress and returns whether the
    ///         session is already complete.
    bool advance(std::size_t budget);

    /// @brief Return the committed transmittance and radiance.
    /// @details Before completion the radiance excludes the background.
    ///          Repeated calls do not modify the session.
    FogResult result() const;

    /// @brief Number of hierarchy constant intervals fetched or advanced so
    ///        far, including vacuum intervals.
    std::size_t spanSteps() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
}; // class FogSession

}// namespace tools

}// namespace nanovdb

#endif // NANOVDB_TOOLS_FOGSESSION_H_HAS_BEEN_INCLUDED
