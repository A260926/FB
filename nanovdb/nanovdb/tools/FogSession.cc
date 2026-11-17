// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <nanovdb/tools/FogSession.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace nanovdb {// ==========================================================

namespace tools {// ==========================================================

namespace {

using UpperT = NanoUpper<float>;
using LowerT = NanoLower<float>;
using LeafT = NanoLeaf<float>;

/// @brief A constant region of a volume's hierarchy along the ray.
struct Cell
{
    Coord origin;    ///< index-space origin of the (aligned) region
    uint32_t dim;    ///< index-space edge length of the region
    float value;     ///< stored value of the region
    bool active;     ///< whether the region is active (tile or voxel)
};

/// @brief Convert a world-ray time to an index-space integer coordinate.
///
/// Positions exactly on a cell's maximum face belong to the next cell, which
/// is what floor() gives, so voxels behave as the half-open box [i, i+1).
int32_t indexFloor(double x)
{
    const double f = std::floor(x);
    if (!(f > double(std::numeric_limits<int32_t>::min())))
        return std::numeric_limits<int32_t>::min();
    if (!(f < double(std::numeric_limits<int32_t>::max())))
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(f);
}

Cell leafCell(const LeafT& leaf, const Coord& ijk)
{
    const uint32_t n = LeafT::CoordToOffset(ijk);
    return Cell{ijk, 1u, leaf.getValue(n), leaf.isActive(n)};
}

Cell lowerCell(const LowerT& lower, const Coord& ijk)
{
    const uint32_t n = LowerT::CoordToOffset(ijk);
    if (lower.childMask().isOn(n))
        return leafCell(*lower.getChild(n), ijk);
    return Cell{lower.offsetToGlobalCoord(n),
                LeafT::DIM,
                lower.data()->getValue(n),
                lower.data()->isActive(n)};
}

Cell upperCell(const UpperT& upper, const Coord& ijk)
{
    const uint32_t n = UpperT::CoordToOffset(ijk);
    if (upper.childMask().isOn(n))
        return lowerCell(*upper.getChild(n), ijk);
    return Cell{upper.offsetToGlobalCoord(n),
                LowerT::DIM,
                upper.data()->getValue(n),
                upper.data()->isActive(n)};
}

/// @brief Find the coarsest constant hierarchy region containing @a ijk.
///
/// This is the hierarchical equivalent of NanoVDB's getDim(): a leaf yields a
/// single voxel, a constant lower/upper tile yields its 8^3/128^3 region and a
/// root-level constant tile or the implicit background yields a 4096^3 cell.
/// Render-skip flags are deliberately ignored: they are a statistics hint and
/// would be wrong for a volume whose non-zero values happen to be flagged.
///
/// @param cachedTile  Optional one-entry cache for the last root tile used.
///                    The caller keeps it per volume; it keeps repeated
///                    lookups inside the same 4096^3 root cell O(1) instead of
///                    rescanning the root table.  Its memory cost is a pointer.
Cell findCell(const FloatGrid& grid, const Coord& ijk,
              const RootData<UpperT>::Tile** cachedTile = nullptr)
{
    const auto& root = grid.tree().root();
    if (cachedTile && *cachedTile) {
        const auto& tile = **cachedTile;
        const Coord origin = tile.origin();
        if (ijk[0] >= origin[0] && ijk[0] < origin[0] + int(UpperT::DIM) &&
            ijk[1] >= origin[1] && ijk[1] < origin[1] + int(UpperT::DIM) &&
            ijk[2] >= origin[2] && ijk[2] < origin[2] + int(UpperT::DIM)) {
            return tile.isChild() ? upperCell(*root.getChild(&tile), ijk)
                                  : Cell{origin, UpperT::DIM, tile.value, tile.state != 0u};
        }
    }
    if (const auto* tile = root.probeTile(ijk)) {
        if (cachedTile)
            *cachedTile = tile;
        if (tile->isChild())
            return upperCell(*root.getChild(tile), ijk);
        return Cell{tile->origin(), UpperT::DIM, tile->value, tile->state != 0u};
    }
    // No tile at all: the aligned root-level cell is a constant background
    // region.  It is always inactive and therefore vacuum, regardless of the
    // (possibly non-zero) stored background value.
    if (cachedTile)
        *cachedTile = nullptr;
    return Cell{root.KeyToCoord(root.CoordToKey(ijk)), UpperT::DIM, root.background(), false};
}

} // namespace

// ============================================================================

struct FogSession::Impl
{
    /// @brief Per-volume cursor for the constant hierarchy region currently
    ///        intersected by the world ray.
    struct Cursor
    {
        const FloatGrid* grid = nullptr;
        Vec3d eyeIndex = Vec3d(0);     ///< ray origin in index space
        Vec3d dirIndex = Vec3d(0);     ///< ray direction in index space
        double extinction = 0.0;
        Vec3d emission = Vec3d(0.0);
        double tEnd = 0.0;             ///< world-ray time where the region exits
        double density = 0.0;          ///< max(value, 0) of an active region
        bool initialized = false;
        /// @brief One-entry cache of the root tile last used.  This is the
        ///        only root lookup state kept and it is bounded by the volume
        ///        count, not by the traversal length.
        const RootData<UpperT>::Tile* rootTile = nullptr;
    };

    std::vector<Cursor> cursors;
    double worldSpeed = 0.0;           ///< |world direction|; converts dt to length
    double tEnd = 0.0;                 ///< finite end of the ray interval
    double t = 0.0;                    ///< committed world-ray parameter
    Vec3d background = Vec3d(0.0);
    Vec3d accumulated = Vec3d(0.0);    ///< C without the background term
    double transmittance = 1.0;        ///< T
    bool complete = false;
    std::size_t steps = 0;             ///< hierarchy span steps performed

    Impl(std::vector<FogVolume> volumes, const math::Ray<double>& ray, const Vec3d& bg);

    void fetch(std::size_t index);
    void integrateTo(double tNext);
    bool advance(std::size_t budget);
};

FogSession::Impl::Impl(std::vector<FogVolume> volumes,
                       const math::Ray<double>& ray,
                       const Vec3d& bg)
    : tEnd(ray.t1())
    , t(ray.t0())
    , background(bg)
{
    worldSpeed = ray.dir().length();
    cursors.reserve(volumes.size());
    for (const auto& volume : volumes) {
        if (volume.grid == nullptr)
            continue; // a missing grid contributes vacuum
        Cursor cursor;
        cursor.grid = volume.grid;
        cursor.extinction = volume.extinction;
        cursor.emission = volume.emission;
        cursor.eyeIndex = volume.grid->worldToIndex(ray.eye());
        cursor.dirIndex = volume.grid->worldToIndexDir(ray.dir());
        cursors.push_back(cursor);
    }
    const double tBegin = ray.t0();
    if (!(worldSpeed > 0.0) || !(tEnd > tBegin) || cursors.empty())
        complete = true; // nothing to traverse
}

void FogSession::Impl::fetch(std::size_t index)
{
    Cursor& cursor = cursors[index];
    const Vec3d p = cursor.eyeIndex + cursor.dirIndex * t;
    Coord       ijk(indexFloor(p[0]), indexFloor(p[1]), indexFloor(p[2]));

    // Exit time of a constant region's half-open index box
    // [origin, origin + dim), together with the axis whose face is hit first.
    auto exitOf = [&](const Cell& cell, int& axisOut) {
        double exit = tEnd;
        axisOut = -1;
        for (int axis = 0; axis < 3; ++axis) {
            const double d = cursor.dirIndex[axis];
            if (d == 0.0)
                continue;
            const double plane = d > 0.0 ? double(cell.origin[axis]) + double(cell.dim)
                                         : double(cell.origin[axis]);
            const double time = (plane - cursor.eyeIndex[axis]) / d;
            if (time < exit) {
                exit = time;
                axisOut = axis;
            }
        }
        return exit;
    };

    Cell cell = findCell(*cursor.grid, ijk, &cursor.rootTile);
    int  axis = -1;
    double exit = exitOf(cell, axis);

    // If the sampled coordinate sits exactly on a face that the ray is
    // leaving, floor() selected the region on the wrong side.  Step one index
    // cell in the travel direction and retry.  One index cell is enough: the
    // region containing the neighbouring voxel is the region the ray enters.
    // This is a rounding tie of a single representable coordinate, never an
    // interval, so no non-zero interval is skipped.
    for (int guard = 0; !(exit > t) && axis >= 0 && guard < 16; ++guard) {
        ijk[axis] += cursor.dirIndex[axis] > 0.0 ? 1 : -1;
        cell = findCell(*cursor.grid, ijk, &cursor.rootTile);
        exit = exitOf(cell, axis);
    }
    if (!(exit > t)) // defensive: cannot leave this region, cover the rest
        exit = tEnd;
    if (exit > tEnd)
        exit = tEnd;

    cursor.tEnd = exit;
    cursor.density = cell.active ? (double(cell.value) > 0.0 ? double(cell.value) : 0.0) : 0.0;
    cursor.initialized = true;
    ++steps;
}

void FogSession::Impl::integrateTo(double tNext)
{
    if (!(tNext > t))
        return;
    const double length = worldSpeed * (tNext - t);

    double sigma = 0.0;
    Vec3d j(0.0);
    for (const auto& cursor : cursors) {
        if (cursor.density != 0.0) {
            sigma += cursor.density * cursor.extinction;
            j += cursor.emission * cursor.density;
        }
    }

    if (sigma > 0.0) {
        const double opticalDepth = sigma * length;
        const double attenuation = std::exp(-opticalDepth);
        // (1 - exp(-x)) / sigma, evaluated stably for small x.
        const double factor = -std::expm1(-opticalDepth) / sigma;
        accumulated += j * (transmittance * factor);
        transmittance *= attenuation;
    } else {
        // Zero extinction still emits along the full world-space length.
        accumulated += j * (transmittance * length);
    }
    t = tNext;
}

bool FogSession::Impl::advance(std::size_t budget)
{
    if (complete)
        return true;
    if (budget == 0)
        return false; // a zero budget must not make any progress

    std::size_t used = 0;
    for (;;) {
        if (complete)
            return true;

        // A cursor must be (re)fetched when it is uninitialized or when its
        // current region ended exactly at the committed parameter.  Fetching a
        // region is the only kind of work charged to the budget.
        std::size_t pending = cursors.size();
        for (std::size_t i = 0; i < cursors.size(); ++i) {
            if (!cursors[i].initialized || !(cursors[i].tEnd > t)) {
                pending = i;
                break;
            }
        }
        if (pending < cursors.size()) {
            if (used >= budget)
                return false; // keep the pending state for the next call
            fetch(pending);
            ++used;
            continue;
        }

        // All cursors cover [t, tEnd).  The next world-space event is the
        // nearest region exit; every volume keeps its value up to that time.
        double tNext = tEnd;
        for (const auto& cursor : cursors) {
            if (cursor.tEnd < tNext)
                tNext = cursor.tEnd;
        }
        if (!(tNext > t)) { // defensive: never stall on a rounding tie
            if (used >= budget)
                return false;
            std::size_t which = 0;
            for (std::size_t i = 1; i < cursors.size(); ++i) {
                if (cursors[i].tEnd < cursors[which].tEnd)
                    which = i;
            }
            fetch(which);
            ++used;
            continue;
        }

        integrateTo(tNext);
        if (!(t < tEnd))
            complete = true;
        if (complete)
            return true;
        // Any cursor whose region ended at t is fetched on the next iteration
        // (or, if the budget is exhausted, on the next advance()).
    }
}

// ============================================================================

FogSession::FogSession(std::vector<FogVolume> volumes,
                       const math::Ray<double>& ray,
                       const Vec3d& background)
    : mImpl(new Impl(std::move(volumes), ray, background))
{
}

FogSession::~FogSession() = default;

FogSession::FogSession(FogSession&&) noexcept = default;

FogSession& FogSession::operator=(FogSession&&) noexcept = default;

bool FogSession::advance(std::size_t budget)
{
    return mImpl->advance(budget);
}

FogResult FogSession::result() const
{
    FogResult result;
    result.transmittance = mImpl->transmittance;
    result.radiance = mImpl->complete
                          ? mImpl->accumulated + mImpl->background * mImpl->transmittance
                          : mImpl->accumulated;
    return result;
}

std::size_t FogSession::spanSteps() const
{
    return mImpl->steps;
}

}// namespace tools

}// namespace nanovdb
