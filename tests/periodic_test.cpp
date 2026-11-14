// Periodic Dual Marching Cubes task tests.
// Registered through task_support/check.h; the driver supplies main().

#include "task_support/check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "dualmc.h"

namespace {

using DMC = dualmc::DualMC<float>;
using V = dualmc::Vertex;

constexpr float ISO = 0.0f;
constexpr float POS_TOL = 1.0e-4f;

bool near(float a, float b) { return std::fabs(a - b) < POS_TOL; }

int32_t wrap(int32_t v, int32_t n) {
    int32_t r = v % n;
    if (r < 0) r += n;
    return r;
}

float sample(const std::vector<float> &f, int nx, int ny, int nz,
             int x, int y, int z) {
    return f[wrap(x, nx) + nx * (wrap(y, ny) + ny * wrap(z, nz))];
}

struct Lifted {
    float x, y, z;
};

Lifted liftPoint(const V &v, const int32_t off[3], int nx, int ny, int nz) {
    return {v.x + off[0] * static_cast<float>(nx),
            v.y + off[1] * static_cast<float>(ny),
            v.z + off[2] * static_cast<float>(nz)};
}

// Periodic inside flag, value == iso counts as inside (matches the >= rule).
bool inside(float v) { return v >= ISO; }

// Independent reference: number of sign-changing positive grid edges,
// each of which owns exactly one dual quad.
int64_t countOwnedEdges(const std::vector<float> &f, int nx, int ny, int nz) {
    int64_t count = 0;
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                if (inside(sample(f, nx, ny, nz, x, y, z)) !=
                    inside(sample(f, nx, ny, nz, x + 1, y, z)))
                    ++count;
                if (inside(sample(f, nx, ny, nz, x, y, z)) !=
                    inside(sample(f, nx, ny, nz, x, y + 1, z)))
                    ++count;
                if (inside(sample(f, nx, ny, nz, x, y, z)) !=
                    inside(sample(f, nx, ny, nz, x, y, z + 1)))
                    ++count;
            }
    return count;
}

// Deterministic pseudo-random periodic field, including values equal to iso.
std::vector<float> randomField(int nx, int ny, int nz, unsigned seed) {
    std::vector<float> f(static_cast<size_t>(nx) * ny * nz);
    unsigned s = seed;
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                s = s * 1103515245u + 12345u;
                unsigned r = (s >> 16) % 10u;
                float v = (r < 4u) ? -1.0f : (r > 6u ? 1.0f : 0.0f);
                f[x + nx * (y + ny * z)] = v;
            }
    return f;
}

// Strictly binary pseudo-random periodic field. The chosen seed places
// C16/C19 ambiguous cell pairs across period seams.
std::vector<float> binaryField(int nx, int ny, int nz, unsigned seed) {
    std::vector<float> f(static_cast<size_t>(nx) * ny * nz);
    unsigned s = seed;
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                s = s * 1103515245u + 12345u;
                int r = (s >> 16) & 1;
                f[x + nx * (y + ny * z)] = r ? 1.0f : -1.0f;
            }
    return f;
}

// Smooth analytic periodic field used as an independent geometry reference.
std::vector<float> analyticField(int nx, int ny, int nz) {
    std::vector<float> f(static_cast<size_t>(nx) * ny * nz);
    constexpr float PI = 3.14159265358979323846f;
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                float v = std::sin(2.0f * PI * x / nx) *
                              std::cos(2.0f * PI * y / ny) +
                          0.7f * std::sin(2.0f * PI * z / nz + 0.5f * x) +
                          0.3f * static_cast<float>((x + y + z) % 3 - 1);
                f[x + nx * (y + ny * z)] = v;
            }
    return f;
}

// Tile one period T times per axis, materializing the duplicated endpoint
// samples that the finite-domain build() expects.
std::vector<float> tileField(const std::vector<float> &small,
                             int nx, int ny, int nz, int T) {
    int dx = T * nx + 1, dy = T * ny + 1, dz = T * nz + 1;
    std::vector<float> big(static_cast<size_t>(dx) * dy * dz);
    for (int z = 0; z < dz; ++z)
        for (int y = 0; y < dy; ++y)
            for (int x = 0; x < dx; ++x)
                big[x + dx * (y + dy * z)] =
                    small[(x % nx) + nx * ((y % ny) + ny * (z % nz))];
    return big;
}

int findVertex(const std::vector<V> &verts, float x, float y, float z) {
    for (size_t i = 0; i < verts.size(); ++i)
        if (near(verts[i].x, x) && near(verts[i].y, y) && near(verts[i].z, z))
            return static_cast<int>(i);
    return -1;
}

// Lift the periodic mesh into a T*T*T lattice and compare every interior
// quad (vertex set and cyclic winding) against the finite-domain build()
// run on the explicitly tiled density box.
bool matchesTiledBuild(const std::vector<float> &small,
                       int nx, int ny, int nz, int T, bool manifold) {
    std::vector<float> big = tileField(small, nx, ny, nz, T);
    int dx = T * nx + 1, dy = T * ny + 1, dz = T * nz + 1;

    DMC builder;
    std::vector<V> oldVerts;
    std::vector<dualmc::Quad> oldQuads;
    builder.build(big.data(), dx, dy, dz, ISO, manifold, false,
                  oldVerts, oldQuads);

    dualmc::PeriodicMesh pm;
    builder.buildPeriodic(small.data(), nx, ny, nz, ISO, manifold, pm);

    const float lo = 1.0f;
    const float hiX = static_cast<float>(T * nx - 1);
    const float hiY = static_cast<float>(T * ny - 1);
    const float hiZ = static_cast<float>(T * nz - 1);
    auto interior = [&](const Lifted &p) {
        return p.x >= lo && p.x <= hiX && p.y >= lo && p.y <= hiY &&
               p.z >= lo && p.z <= hiZ;
    };

    std::set<std::array<int, 4>> oldFaces;
    std::map<std::array<int, 4>, std::array<int, 4>> oldWinding;
    for (const auto &q : oldQuads) {
        int ids[4] = {q.i0, q.i1, q.i2, q.i3};
        bool in = true;
        for (int i = 0; i < 4; ++i)
            if (!interior({oldVerts[ids[i]].x, oldVerts[ids[i]].y,
                           oldVerts[ids[i]].z}))
                in = false;
        if (!in) continue;
        std::array<int, 4> key = {ids[0], ids[1], ids[2], ids[3]};
        std::sort(key.begin(), key.end());
        oldFaces.insert(key);
        oldWinding[key] = {ids[0], ids[1], ids[2], ids[3]};
    }

    std::set<std::array<int, 4>> newFaces;
    size_t windingMatches = 0;
    for (int tz = 0; tz < T; ++tz)
        for (int ty = 0; ty < T; ++ty)
            for (int tx = 0; tx < T; ++tx) {
                for (const auto &q : pm.quads) {
                    int ids[4] = {q.i0, q.i1, q.i2, q.i3};
                    Lifted lifted[4];
                    bool in = true;
                    for (int k = 0; k < 4; ++k) {
                        const V &v = pm.vertices[ids[k]];
                        int32_t off[3] = {q.offset[k][0] + tx,
                                          q.offset[k][1] + ty,
                                          q.offset[k][2] + tz};
                        lifted[k] = liftPoint(v, off, nx, ny, nz);
                        if (!interior(lifted[k])) in = false;
                    }
                    if (!in) continue;

                    std::array<int, 4> newIds;
                    for (int k = 0; k < 4; ++k) {
                        newIds[k] = findVertex(oldVerts, lifted[k].x,
                                               lifted[k].y, lifted[k].z);
                        if (newIds[k] < 0) return false;
                    }
                    std::array<int, 4> key = newIds;
                    std::sort(key.begin(), key.end());
                    if (oldFaces.find(key) == oldFaces.end()) return false;
                    newFaces.insert(key);

                    const std::array<int, 4> &oldIds = oldWinding.at(key);
                    int rot = -1;
                    for (int r = 0; r < 4; ++r)
                        if (newIds[r] == oldIds[0]) rot = r;
                    if (rot < 0) return false;
                    for (int k = 0; k < 4; ++k)
                        if (newIds[(rot + k) % 4] != oldIds[k]) return false;
                    ++windingMatches;
                }
            }

    return newFaces == oldFaces && windingMatches == oldFaces.size();
}

// The 4x5x6 field from the task description: only the x=0 sample plane is on.
std::vector<float> sheetField(int nx, int ny, int nz, float onValue) {
    std::vector<float> f(static_cast<size_t>(nx) * ny * nz, -1.0f);
    for (int y = 0; y < ny; ++y)
        for (int z = 0; z < nz; ++z)
            f[0 + nx * (y + ny * z)] = onValue;
    return f;
}

bool noFreeVertices(const dualmc::PeriodicMesh &m) {
    std::vector<char> used(m.vertices.size(), 0);
    for (const auto &q : m.quads) {
        int ids[4] = {q.i0, q.i1, q.i2, q.i3};
        for (int i = 0; i < 4; ++i) {
            if (ids[i] < 0 ||
                static_cast<size_t>(ids[i]) >= m.vertices.size())
                return false;
            used[ids[i]] = 1;
        }
    }
    for (char c : used)
        if (!c) return false;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// group wrap_sampling
// ---------------------------------------------------------------------------

static int wrap_sheet_counts() {
    const int nx = 4, ny = 5, nz = 6;
    std::vector<float> f = sheetField(nx, ny, nz, 1.0f);
    DMC builder;
    dualmc::PeriodicMesh m;
    builder.buildPeriodic(f.data(), nx, ny, nz, ISO, true, m);

    // Two layers (x=0.5 and the wrapped x=3.5), 5*6 quads on each.
    TASK_CHECK(m.quads.size() == 60);
    TASK_CHECK(m.vertices.size() == 60);
    TASK_CHECK(noFreeVertices(m));

    bool haveHalf = false, haveWrapped = false;
    for (const V &v : m.vertices) {
        TASK_CHECK(v.x >= 0.0f && v.x <= static_cast<float>(nx));
        TASK_CHECK(v.y >= 0.0f && v.y <= static_cast<float>(ny));
        TASK_CHECK(v.z >= 0.0f && v.z <= static_cast<float>(nz));
        if (near(v.x, 0.5f)) haveHalf = true;
        if (near(v.x, static_cast<float>(nx) - 0.5f)) haveWrapped = true;
    }
    TASK_CHECK(haveHalf && haveWrapped);

    // Independent edge ownership bookkeeping gives the same quad count.
    TASK_CHECK(static_cast<size_t>(countOwnedEdges(f, nx, ny, nz)) ==
               m.quads.size());
    return 0;
}

static int wrap_iso_equal_classification() {
    const int nx = 4, ny = 5, nz = 6;
    // Samples exactly on iso must be treated as inside, so the surface is
    // identical to the strictly-positive sheet.
    std::vector<float> equal = sheetField(nx, ny, nz, 0.0f);
    std::vector<float> above = sheetField(nx, ny, nz, 1.0f);

    DMC builder;
    dualmc::PeriodicMesh a, b;
    builder.buildPeriodic(equal.data(), nx, ny, nz, ISO, false, a);
    builder.buildPeriodic(above.data(), nx, ny, nz, ISO, false, b);
    // The >= rule makes the on-iso samples inside: topology (vertex count,
    // quad count, connectivity and period offsets) must be identical. The
    // interpolation positions themselves differ (t=0 versus t=0.5), which is
    // expected and not asserted here.
    TASK_CHECK(a.quads.size() == 60 && b.quads.size() == 60);
    TASK_CHECK(a.vertices.size() == b.vertices.size());
    TASK_CHECK(a.quads.size() == b.quads.size());
    for (size_t i = 0; i < a.quads.size(); ++i) {
        TASK_CHECK(a.quads[i].i0 == b.quads[i].i0);
        TASK_CHECK(a.quads[i].i1 == b.quads[i].i1);
        TASK_CHECK(a.quads[i].i2 == b.quads[i].i2);
        TASK_CHECK(a.quads[i].i3 == b.quads[i].i3);
        for (int k = 0; k < 4; ++k)
            for (int ax = 0; ax < 3; ++ax)
                TASK_CHECK(a.quads[i].offset[k][ax] ==
                           b.quads[i].offset[k][ax]);
    }
    // Shared-index identity plus identical quad connectivity already
    // establishes that both fields produce the same (cell, code) vertices.
    return 0;
}

static int wrap_single_sample_shell() {
    // A single inside sample is a closed shell around the wrapped corner:
    // all three negative seams are crossed at once.
    const int n = 4;
    std::vector<float> f(static_cast<size_t>(n) * n * n, -1.0f);
    f[0] = 1.0f;

    DMC builder;
    dualmc::PeriodicMesh m;
    builder.buildPeriodic(f.data(), n, n, n, ISO, true, m);
    TASK_CHECK(m.quads.size() == 6);
    TASK_CHECK(m.vertices.size() == 8);
    TASK_CHECK(noFreeVertices(m));

    // Every quad must carry at least one negative-seam offset, proving the
    // four cells around a seam edge were resolved through wrapping.
    bool sawNegative[3] = {false, false, false};
    for (const auto &q : m.quads) {
        bool hasNeg = false;
        for (int k = 0; k < 4; ++k)
            for (int a = 0; a < 3; ++a) {
                TASK_CHECK(q.offset[k][a] >= -1 && q.offset[k][a] <= 0);
                if (q.offset[k][a] < 0) {
                    hasNeg = true;
                    sawNegative[a] = true;
                }
            }
        TASK_CHECK(hasNeg);
    }
    TASK_CHECK(sawNegative[0] && sawNegative[1] && sawNegative[2]);
    return 0;
}

static int wrap_axis_aligned_sheets() {
    // Sheets perpendicular to y and z exercise the other two edge families.
    const int nx = 4, ny = 5, nz = 6;
    DMC builder;
    dualmc::PeriodicMesh my, mz;

    std::vector<float> fy(static_cast<size_t>(nx) * ny * nz, -1.0f);
    for (int x = 0; x < nx; ++x)
        for (int z = 0; z < nz; ++z)
            fy[x + nx * (0 + ny * z)] = 1.0f;
    builder.buildPeriodic(fy.data(), nx, ny, nz, ISO, false, my);
    // Two interfaces: the direct y=0.5 layer and the wrapped y=(ny-0.5) layer.
    TASK_CHECK(my.quads.size() == static_cast<size_t>(2 * nx * nz));

    std::vector<float> fz(static_cast<size_t>(nx) * ny * nz, -1.0f);
    for (int x = 0; x < nx; ++x)
        for (int y = 0; y < ny; ++y)
            fz[x + nx * (y + ny * 0)] = 1.0f;
    builder.buildPeriodic(fz.data(), nx, ny, nz, ISO, false, mz);
    TASK_CHECK(mz.quads.size() == static_cast<size_t>(2 * nx * ny));
    return 0;
}

// ---------------------------------------------------------------------------
// group seam_topology
// ---------------------------------------------------------------------------

static int seam_edge_ownership_random() {
    const int nx = 4, ny = 5, nz = 6;
    std::vector<float> f = randomField(nx, ny, nz, 123u);
    DMC builder;
    dualmc::PeriodicMesh m;
    builder.buildPeriodic(f.data(), nx, ny, nz, ISO, true, m);
    TASK_CHECK(noFreeVertices(m));
    // One face per sign-changing positive grid edge, seam edges included.
    TASK_CHECK(static_cast<size_t>(countOwnedEdges(f, nx, ny, nz)) ==
               m.quads.size());

    // Seam faces exist on every axis and only use period offsets -1 or 0.
    bool seam[3] = {false, false, false};
    for (const auto &q : m.quads)
        for (int k = 0; k < 4; ++k)
            for (int a = 0; a < 3; ++a) {
                TASK_CHECK(q.offset[k][a] >= -1 && q.offset[k][a] <= 0);
                if (q.offset[k][a] < 0) seam[a] = true;
            }
    TASK_CHECK(seam[0] && seam[1] && seam[2]);
    return 0;
}

static int seam_ambiguous_neighbors() {
    // This seeded field contains C16/C19 ambiguous cell pairs whose shared
    // face lies across a period seam. Both manifold modes must reproduce the
    // finite-domain build on the explicitly tiled box, which proves the
    // neighbor correction crosses the seam.
    const int nx = 4, ny = 5, nz = 6;
    std::vector<float> f = binaryField(nx, ny, nz, 14u);
    TASK_CHECK(matchesTiledBuild(f, nx, ny, nz, 3, false));
    TASK_CHECK(matchesTiledBuild(f, nx, ny, nz, 3, true));
    return 0;
}

static int seam_manifold_edge_regularity() {
    // Single inside sample: a manifold closed shell. After lifting into
    // neighboring periods every interior lifted edge must be shared by
    // exactly two quads.
    const int n = 4;
    std::vector<float> f(static_cast<size_t>(n) * n * n, -1.0f);
    f[0] = 1.0f;
    DMC builder;
    dualmc::PeriodicMesh m;
    builder.buildPeriodic(f.data(), n, n, n, ISO, true, m);

    std::map<std::pair<std::array<float, 3>, std::array<float, 3>>, int> edges;
    const float lo = 1.0f, hi = static_cast<float>(3 * n - 1);
    for (int tx = 0; tx < 3; ++tx)
        for (int ty = 0; ty < 3; ++ty)
            for (int tz = 0; tz < 3; ++tz)
                for (const auto &q : m.quads) {
                    int ids[4] = {q.i0, q.i1, q.i2, q.i3};
                    std::array<float, 3> p[4];
                    for (int k = 0; k < 4; ++k) {
                        const V &v = m.vertices[ids[k]];
                        int32_t off[3] = {q.offset[k][0] + tx,
                                          q.offset[k][1] + ty,
                                          q.offset[k][2] + tz};
                        Lifted lp = liftPoint(v, off, n, n, n);
                        p[k] = {lp.x, lp.y, lp.z};
                    }
                    for (int k = 0; k < 4; ++k) {
                        auto e = std::minmax(p[k], p[(k + 1) % 4]);
                        auto insidePt = [&](const std::array<float, 3> &a) {
                            return a[0] >= lo && a[0] <= hi && a[1] >= lo &&
                                   a[1] <= hi && a[2] >= lo && a[2] <= hi;
                        };
                        if (insidePt(e.first) && insidePt(e.second))
                            edges[e]++;
                    }
                }
    TASK_CHECK(edges.size() > 0);
    for (const auto &e : edges)
        TASK_CHECK(e.second == 2);
    return 0;
}

// ---------------------------------------------------------------------------
// group lifted_geometry
// ---------------------------------------------------------------------------

static int lifted_offsets_and_compactness() {
    const int nx = 4, ny = 5, nz = 6;
    std::vector<float> f = sheetField(nx, ny, nz, 1.0f);
    DMC builder;
    dualmc::PeriodicMesh m;
    builder.buildPeriodic(f.data(), nx, ny, nz, ISO, true, m);

    bool sawNegativeY = false, sawNegativeZ = false;
    for (const auto &q : m.quads) {
        int ids[4] = {q.i0, q.i1, q.i2, q.i3};
        Lifted p[4];
        for (int k = 0; k < 4; ++k)
            p[k] = liftPoint(m.vertices[ids[k]], q.offset[k], nx, ny, nz);

        // A quad stays inside one period cell: it must never span a whole
        // period along any axis.
        for (int a = 0; a < 3; ++a) {
            float mn = 1.0e30f, mx = -1.0e30f;
            for (int k = 0; k < 4; ++k) {
                float c = (a == 0) ? p[k].x : (a == 1 ? p[k].y : p[k].z);
                mn = std::min(mn, c);
                mx = std::max(mx, c);
            }
            TASK_CHECK(mx - mn <= 1.5f);
        }

        // The lifted position must be exactly canonical position plus the
        // recorded period translation, including negative seam images.
        for (int k = 0; k < 4; ++k) {
            const V &v = m.vertices[ids[k]];
            TASK_CHECK(near(p[k].x, v.x + q.offset[k][0] * nx));
            TASK_CHECK(near(p[k].y, v.y + q.offset[k][1] * ny));
            TASK_CHECK(near(p[k].z, v.z + q.offset[k][2] * nz));
            if (q.offset[k][1] < 0) {
                sawNegativeY = true;
                TASK_CHECK(p[k].y >= -1.0f && p[k].y < 0.0f);
            }
            if (q.offset[k][2] < 0) {
                sawNegativeZ = true;
                TASK_CHECK(p[k].z >= -1.0f && p[k].z < 0.0f);
            }
        }
    }
    TASK_CHECK(sawNegativeY && sawNegativeZ);
    return 0;
}

static int lifted_tiled_geometry_match() {
    // Analytic periodic plane-like field versus the finite-domain build on a
    // 3x3x3 repeat, for both manifold modes: positions, faces and winding.
    const int nx = 5, ny = 4, nz = 3;
    std::vector<float> f = analyticField(nx, ny, nz);
    TASK_CHECK(matchesTiledBuild(f, nx, ny, nz, 3, false));
    TASK_CHECK(matchesTiledBuild(f, nx, ny, nz, 3, true));
    return 0;
}

static int lifted_vertex_identity() {
    // Identity is (canonical cell, corrected dual point code), never the
    // floating point position.
    const int nx = 4, ny = 5, nz = 6;
    std::vector<float> f = randomField(nx, ny, nz, 7u);
    DMC builder;
    dualmc::PeriodicMesh m;
    builder.buildPeriodic(f.data(), nx, ny, nz, ISO, true, m);
    TASK_CHECK(noFreeVertices(m));

    // Canonical cell of each vertex is its floored unit-box position.
    // A cell that contributes several dual points must expose them as
    // distinct indices; the same index may be reused by several quads.
    std::map<std::array<int32_t, 3>, std::set<int32_t>> perCell;
    std::vector<int> refCount(m.vertices.size(), 0);
    auto canonicalCell = [](float c, int n) {
        int32_t cell = static_cast<int32_t>(std::floor(c));
        // A dual point can lie exactly on a cell's upper face.
        if (cell >= n) cell = n - 1;
        return cell;
    };
    for (const auto &q : m.quads) {
        int ids[4] = {q.i0, q.i1, q.i2, q.i3};
        for (int k = 0; k < 4; ++k) {
            const V &v = m.vertices[ids[k]];
            int32_t cell[3] = {canonicalCell(v.x, nx),
                               canonicalCell(v.y, ny),
                               canonicalCell(v.z, nz)};
            TASK_CHECK(cell[0] >= 0 && cell[0] < nx);
            TASK_CHECK(cell[1] >= 0 && cell[1] < ny);
            TASK_CHECK(cell[2] >= 0 && cell[2] < nz);
            perCell[{cell[0], cell[1], cell[2]}].insert(ids[k]);
            refCount[ids[k]]++;
        }
    }

    bool multiPointCell = false, sharedIndex = false;
    for (const auto &e : perCell)
        if (e.second.size() > 1) multiPointCell = true;
    for (int c : refCount)
        if (c > 1) sharedIndex = true;
    TASK_CHECK(multiPointCell);
    TASK_CHECK(sharedIndex);
    TASK_CHECK(m.vertices.size() < 4u * m.quads.size());
    return 0;
}

static int lifted_seam_images_stay_distinct() {
    // Two different canonical cells that geometrically coincide after
    // tiling must still be separate vertex indices (no float welding).
    const int n = 4;
    std::vector<float> f(static_cast<size_t>(n) * n * n, -1.0f);
    f[0] = 1.0f;
    DMC builder;
    dualmc::PeriodicMesh m;
    builder.buildPeriodic(f.data(), n, n, n, ISO, false, m);

    // Cell (0,0,0) dual point and cell (n-1,0,0) dual point: when the
    // latter is shifted by one negative x period their lifted x positions
    // coincide across a tile boundary, yet they are distinct indices.
    int32_t idFirst = -1, idLast = -1;
    for (size_t i = 0; i < m.vertices.size(); ++i) {
        const V &v = m.vertices[i];
        if (v.x < 1.0f && v.y < 1.0f && v.z < 1.0f) idFirst = (int32_t)i;
        if (v.x > static_cast<float>(n - 1) && v.y < 1.0f && v.z < 1.0f)
            idLast = (int32_t)i;
    }
    TASK_CHECK(idFirst >= 0 && idLast >= 0 && idFirst != idLast);
    return 0;
}

// ---------------------------------------------------------------------------
// group reuse_compat
// ---------------------------------------------------------------------------

static int reuse_interleaved_builds() {
    const int nx = 4, ny = 5, nz = 6;
    std::vector<float> periodic = randomField(nx, ny, nz, 21u);
    std::vector<float> boxed(5 * 5 * 5, 0.0f);
    boxed[2 + 5 * (2 + 5 * 2)] = 1.0f;

    DMC working;
    DMC fresh;
    dualmc::PeriodicMesh pa, pb;
    std::vector<V> va, vb;
    std::vector<dualmc::Quad> qa, qb;

    for (int round = 0; round < 3; ++round) {
        // Alternate both APIs and both manifold and soup modes on one object.
        working.buildPeriodic(periodic.data(), nx, ny, nz, ISO, true, pa);
        fresh.buildPeriodic(periodic.data(), nx, ny, nz, ISO, true, pb);
        TASK_CHECK(pa.vertices.size() == pb.vertices.size());
        TASK_CHECK(pa.quads.size() == pb.quads.size());
        for (size_t i = 0; i < pa.quads.size(); ++i) {
            TASK_CHECK(pa.quads[i].i0 == pb.quads[i].i0);
            TASK_CHECK(pa.quads[i].i3 == pb.quads[i].i3);
            for (int k = 0; k < 4; ++k)
                for (int a = 0; a < 3; ++a)
                    TASK_CHECK(pa.quads[i].offset[k][a] ==
                               pb.quads[i].offset[k][a]);
        }

        working.build(boxed.data(), 5, 5, 5, 0.5f, false, false, va, qa);
        fresh.build(boxed.data(), 5, 5, 5, 0.5f, false, false, vb, qb);
        TASK_CHECK(va.size() == 8 && qa.size() == 6);
        TASK_CHECK(va.size() == vb.size() && qa.size() == qb.size());

        working.build(boxed.data(), 5, 5, 5, 0.5f, true, true, va, qa);
        TASK_CHECK(va.size() == 24 && qa.size() == 6);

        working.buildPeriodic(periodic.data(), nx, ny, nz, ISO, false, pa);
        fresh.buildPeriodic(periodic.data(), nx, ny, nz, ISO, false, pb);
        TASK_CHECK(pa.vertices.size() == pb.vertices.size());
        TASK_CHECK(pa.quads.size() == pb.quads.size());
    }
    return 0;
}

static int reuse_output_replaced() {
    const int nx = 4, ny = 4, nz = 4;
    std::vector<float> rich = randomField(nx, ny, nz, 31u);
    std::vector<float> empty(static_cast<size_t>(nx) * ny * nz, -1.0f);

    DMC builder;
    dualmc::PeriodicMesh m;
    m.vertices.emplace_back(9.0f, 9.0f, 9.0f);
    m.quads.emplace_back(7, 7, 7, 7);

    builder.buildPeriodic(rich.data(), nx, ny, nz, ISO, false, m);
    size_t richVerts = m.vertices.size();
    size_t richQuads = m.quads.size();
    TASK_CHECK(richQuads > 0 && richVerts > 0);
    TASK_CHECK(noFreeVertices(m));
    // The pre-seeded stale (9,9,9) vertex must be gone; no vertex of a
    // 4-period domain can lie at coordinate 9 anyway.
    for (const V &v : m.vertices) {
        TASK_CHECK(v.x >= 0.0f && v.x <= static_cast<float>(nx));
        TASK_CHECK(!(near(v.x, 9.0f) && near(v.y, 9.0f) && near(v.z, 9.0f)));
    }

    // A second call fully replaces the previous output; an all-outside field
    // yields an empty mesh.
    builder.buildPeriodic(empty.data(), nx, ny, nz, ISO, false, m);
    TASK_CHECK(m.vertices.empty() && m.quads.empty());
    return 0;
}

static int reuse_finite_domain_unchanged() {
    // Guards the original behavior while the new API lives in the same class.
    std::vector<float> field(125, 0.0f);
    field[2 + 5 * (2 + 5 * 2)] = 1.0f;
    DMC dmc;
    std::vector<V> v, s;
    std::vector<dualmc::Quad> q, t;

    for (int manifold = 0; manifold < 2; ++manifold) {
        dmc.build(field.data(), 5, 5, 5, 0.5f, manifold, false, v, q);
        TASK_CHECK(v.size() == 8 && q.size() == 6);
        for (const auto &p : v) {
            TASK_CHECK(near(p.x, 11.0f / 6.0f) || near(p.x, 13.0f / 6.0f));
            TASK_CHECK(near(p.y, 11.0f / 6.0f) || near(p.y, 13.0f / 6.0f));
            TASK_CHECK(near(p.z, 11.0f / 6.0f) || near(p.z, 13.0f / 6.0f));
        }
        dmc.build(field.data(), 5, 5, 5, 0.5f, manifold, true, s, t);
        TASK_CHECK(s.size() == 24 && t.size() == 6);
        dmc.build(field.data(), 5, 5, 5, 2.0f, manifold, false, v, q);
        TASK_CHECK(v.empty() && q.empty());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    {"wrap_sampling", "sheet_counts", wrap_sheet_counts},
    {"wrap_sampling", "iso_equal_classification", wrap_iso_equal_classification},
    {"wrap_sampling", "single_sample_shell", wrap_single_sample_shell},
    {"wrap_sampling", "axis_aligned_sheets", wrap_axis_aligned_sheets},

    {"seam_topology", "edge_ownership_random", seam_edge_ownership_random},
    {"seam_topology", "ambiguous_neighbors", seam_ambiguous_neighbors},
    {"seam_topology", "manifold_edge_regularity", seam_manifold_edge_regularity},

    {"lifted_geometry", "offsets_and_compactness", lifted_offsets_and_compactness},
    {"lifted_geometry", "tiled_geometry_match", lifted_tiled_geometry_match},
    {"lifted_geometry", "vertex_identity", lifted_vertex_identity},
    {"lifted_geometry", "seam_images_stay_distinct", lifted_seam_images_stay_distinct},

    {"reuse_compat", "interleaved_builds", reuse_interleaved_builds},
    {"reuse_compat", "output_replaced", reuse_output_replaced},
    {"reuse_compat", "finite_domain_unchanged", reuse_finite_domain_unchanged},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
