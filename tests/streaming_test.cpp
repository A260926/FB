// Tests for the plane-streaming dual marching cubes extraction
// (DualMC<T>::buildStreaming). Registered through task_support/check.h;
// the driver provides main().

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "dualmc.h"
#include "task_support/check.h"

namespace {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

struct Mesh {
    std::vector<dualmc::Vertex> vertices;
    std::vector<dualmc::Quad> quads;
};

// Reference extraction through the classic in-memory entry point.
Mesh buildClassic(const std::vector<float>& data, int nx, int ny, int nz,
                  float iso, bool manifold, bool soup) {
    dualmc::DualMC<float> builder;
    Mesh mesh;
    builder.build(data.data(), nx, ny, nz, iso, manifold, soup,
                  mesh.vertices, mesh.quads);
    return mesh;
}

// Everything observed during one streaming run.
struct StreamCapture {
    Mesh mesh;
    dualmc::StreamStats stats;
    std::vector<int32_t> readOrder;
    std::vector<dualmc::QuadIndexType> vertexIds;
    std::vector<std::size_t> verticesBeforeQuad;
    bool ok;
    int readCalls;
    int vertexCalls;
    int quadCalls;
    StreamCapture() : ok(false), readCalls(0), vertexCalls(0), quadCalls(0) {}
};

// Run the streaming extraction over an in-memory volume, plane by plane.
StreamCapture runStreaming(dualmc::DualMC<float>& builder,
                           const std::vector<float>& data,
                           int nx, int ny, int nz, float iso, bool manifold) {
    StreamCapture cap;
    cap.ok = builder.buildStreaming(nx, ny, nz, iso, manifold,
        [&](int32_t z, float* dst, std::size_t count) -> bool {
            ++cap.readCalls;
            cap.readOrder.push_back(z);
            const float* src = data.data() + std::size_t(z) * std::size_t(nx) * std::size_t(ny);
            for (std::size_t i = 0; i < count; ++i) dst[i] = src[i];
            return true;
        },
        [&](dualmc::QuadIndexType id, const dualmc::Vertex& v) -> bool {
            ++cap.vertexCalls;
            cap.vertexIds.push_back(id);
            cap.mesh.vertices.push_back(v);
            return true;
        },
        [&](const dualmc::Quad& q) -> bool {
            ++cap.quadCalls;
            cap.verticesBeforeQuad.push_back(cap.mesh.vertices.size());
            cap.mesh.quads.push_back(q);
            return true;
        },
        cap.stats);
    return cap;
}

// Count content differences between two meshes (1e-6 position tolerance).
int meshDifferences(const Mesh& a, const Mesh& b) {
    int diffs = 0;
    if (a.vertices.size() != b.vertices.size()) ++diffs;
    if (a.quads.size() != b.quads.size()) ++diffs;
    const std::size_t nv = a.vertices.size() < b.vertices.size()
        ? a.vertices.size() : b.vertices.size();
    for (std::size_t i = 0; i < nv; ++i) {
        if (std::fabs(a.vertices[i].x - b.vertices[i].x) > 1e-6f) ++diffs;
        if (std::fabs(a.vertices[i].y - b.vertices[i].y) > 1e-6f) ++diffs;
        if (std::fabs(a.vertices[i].z - b.vertices[i].z) > 1e-6f) ++diffs;
    }
    const std::size_t nq = a.quads.size() < b.quads.size()
        ? a.quads.size() : b.quads.size();
    for (std::size_t i = 0; i < nq; ++i) {
        if (a.quads[i].i0 != b.quads[i].i0) ++diffs;
        if (a.quads[i].i1 != b.quads[i].i1) ++diffs;
        if (a.quads[i].i2 != b.quads[i].i2) ++diffs;
        if (a.quads[i].i3 != b.quads[i].i3) ++diffs;
    }
    return diffs;
}

// Analytic plane field, surface perpendicular to the x axis.
std::vector<float> makePlaneField(int nx, int ny, int nz) {
    std::vector<float> data(std::size_t(nx) * std::size_t(ny) * std::size_t(nz));
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x)
                data[std::size_t(x) + std::size_t(nx) * (std::size_t(y) + std::size_t(ny) * std::size_t(z))]
                    = float(x) - 2.25f;
    return data;
}

// Deterministic pseudo-random binary field, rich in cell configurations.
std::vector<float> makeHashVolume(int nx, int ny, int nz) {
    std::vector<float> data(std::size_t(nx) * std::size_t(ny) * std::size_t(nz));
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                unsigned h = unsigned(x) * 2654435761u ^ unsigned(y) * 40503u ^ unsigned(z) * 214013u;
                h ^= h >> 13;
                h *= 16777619u;
                h ^= h >> 16;
                data[std::size_t(x) + std::size_t(nx) * (std::size_t(y) + std::size_t(ny) * std::size_t(z))]
                    = (h % 100u) < 42u ? 1.0f : 0.0f;
            }
    return data;
}

// Count pairs of distinct vertices which sit in the same grid cell.
int countSiblingVertices(const Mesh& mesh) {
    int siblings = 0;
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
        for (std::size_t j = i + 1; j < mesh.vertices.size(); ++j) {
            const dualmc::Vertex& a = mesh.vertices[i];
            const dualmc::Vertex& b = mesh.vertices[j];
            if (int(a.x) == int(b.x) && int(a.y) == int(b.y) && int(a.z) == int(b.z))
                ++siblings;
        }
    return siblings;
}

// ---------------------------------------------------------------------------
// geometry group
// ---------------------------------------------------------------------------

// Analytic plane field: full cross-check of the streaming output against
// the classic entry point, including the emission protocol and statistics.
int geometry_plane_field_shared() {
    const int n = 7;
    const std::vector<float> data = makePlaneField(n, n, n);
    const Mesh ref = buildClassic(data, n, n, n, 0.0f, true, false);
    TASK_CHECK(!ref.vertices.empty());
    TASK_CHECK(!ref.quads.empty());

    dualmc::DualMC<float> builder;
    const StreamCapture cap = runStreaming(builder, data, n, n, n, 0.0f, true);
    TASK_CHECK(cap.ok);

    // readPlane must have been called exactly once per plane, in order
    TASK_CHECK(cap.readCalls == n);
    TASK_CHECK(cap.readOrder.size() == std::size_t(n));
    bool orderOk = true;
    for (std::size_t i = 0; i < cap.readOrder.size(); ++i)
        orderOk = orderOk && cap.readOrder[i] == int32_t(i);
    TASK_CHECK(orderOk);

    // identical mesh: compact numbering, positions, quad order and winding
    TASK_CHECK(meshDifferences(ref, cap.mesh) == 0);

    // compact sequential vertex ids, every vertex emitted exactly once
    TASK_CHECK(cap.vertexIds.size() == ref.vertices.size());
    bool idsOk = true;
    for (std::size_t i = 0; i < cap.vertexIds.size(); ++i)
        idsOk = idsOk && cap.vertexIds[i] == dualmc::QuadIndexType(i);
    TASK_CHECK(idsOk);

    // every quad references only vertices emitted before it
    TASK_CHECK(cap.verticesBeforeQuad.size() == ref.quads.size());
    bool refsOk = true;
    for (std::size_t i = 0; i < cap.mesh.quads.size(); ++i) {
        const dualmc::Quad& q = cap.mesh.quads[i];
        const std::size_t emitted = cap.verticesBeforeQuad[i];
        refsOk = refsOk && q.i0 >= 0 && std::size_t(q.i0) < emitted;
        refsOk = refsOk && q.i1 >= 0 && std::size_t(q.i1) < emitted;
        refsOk = refsOk && q.i2 >= 0 && std::size_t(q.i2) < emitted;
        refsOk = refsOk && q.i3 >= 0 && std::size_t(q.i3) < emitted;
    }
    TASK_CHECK(refsOk);

    // statistics
    TASK_CHECK(cap.stats.emittedVertices == ref.vertices.size());
    TASK_CHECK(cap.stats.emittedQuads == ref.quads.size());
    TASK_CHECK(cap.stats.emittedVertices == std::size_t(cap.vertexCalls));
    TASK_CHECK(cap.stats.emittedQuads == std::size_t(cap.quadCalls));
    TASK_CHECK(cap.stats.peakSampleValues == 4u * std::size_t(n) * std::size_t(n));
    TASK_CHECK(cap.stats.peakVertexKeys > 0u);
    TASK_CHECK(cap.stats.peakVertexKeys <= 8u * std::size_t(n) * std::size_t(n));
    return 0;
}

// Closed surface (sphere distance field) with manifold correction.
int geometry_closed_surface_manifold() {
    const int n = 9;
    std::vector<float> data(std::size_t(n) * n * n);
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                data[std::size_t(x) + std::size_t(n) * (std::size_t(y) + std::size_t(n) * std::size_t(z))]
                    = float((x - 4) * (x - 4) + (y - 4) * (y - 4) + (z - 4) * (z - 4)) - 10.5f;
    const Mesh ref = buildClassic(data, n, n, n, 0.0f, true, false);
    TASK_CHECK(!ref.vertices.empty());
    dualmc::DualMC<float> builder;
    const StreamCapture cap = runStreaming(builder, data, n, n, n, 0.0f, true);
    TASK_CHECK(cap.ok);
    TASK_CHECK(meshDifferences(ref, cap.mesh) == 0);
    TASK_CHECK(cap.stats.emittedVertices == ref.vertices.size());
    TASK_CHECK(cap.stats.emittedQuads == ref.quads.size());
    return 0;
}

// Same field without the manifold correction.
int geometry_closed_surface_nonmanifold() {
    const int n = 9;
    std::vector<float> data(std::size_t(n) * n * n);
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                data[std::size_t(x) + std::size_t(n) * (std::size_t(y) + std::size_t(n) * std::size_t(z))]
                    = float((x - 4) * (x - 4) + (y - 4) * (y - 4) + (z - 4) * (z - 4)) - 10.5f;
    const Mesh ref = buildClassic(data, n, n, n, 0.0f, false, false);
    TASK_CHECK(!ref.vertices.empty());
    dualmc::DualMC<float> builder;
    const StreamCapture cap = runStreaming(builder, data, n, n, n, 0.0f, false);
    TASK_CHECK(cap.ok);
    TASK_CHECK(meshDifferences(ref, cap.mesh) == 0);
    return 0;
}

// The streaming entry point also works for integer sample types.
int geometry_integer_sample_type() {
    const int n = 6;
    std::vector<unsigned char> data(std::size_t(n) * n * n);
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                data[std::size_t(x) + std::size_t(n) * (std::size_t(y) + std::size_t(n) * std::size_t(z))]
                    = (unsigned char)((x - 2) * (x - 2) + (y - 2) * (y - 2) + (z - 2) * (z - 2));
    dualmc::DualMC<unsigned char> refBuilder;
    Mesh ref;
    refBuilder.build(data.data(), n, n, n, (unsigned char)5, true, false,
                     ref.vertices, ref.quads);
    TASK_CHECK(!ref.vertices.empty());

    dualmc::DualMC<unsigned char> builder;
    Mesh mesh;
    dualmc::StreamStats stats;
    std::vector<int32_t> readOrder;
    const bool ok = builder.buildStreaming(n, n, n, (unsigned char)5, true,
        [&](int32_t z, unsigned char* dst, std::size_t count) -> bool {
            readOrder.push_back(z);
            const unsigned char* src = data.data() + std::size_t(z) * n * n;
            for (std::size_t i = 0; i < count; ++i) dst[i] = src[i];
            return true;
        },
        [&](dualmc::QuadIndexType, const dualmc::Vertex& v) -> bool {
            mesh.vertices.push_back(v);
            return true;
        },
        [&](const dualmc::Quad& q) -> bool {
            mesh.quads.push_back(q);
            return true;
        },
        stats);
    TASK_CHECK(ok);
    TASK_CHECK(readOrder.size() == std::size_t(n));
    TASK_CHECK(meshDifferences(ref, mesh) == 0);
    TASK_CHECK(stats.peakSampleValues == 4u * std::size_t(n) * std::size_t(n));
    return 0;
}

// ---------------------------------------------------------------------------
// frontier group
// ---------------------------------------------------------------------------

// Two vertically adjacent C16/C19 cells (cube codes 103 and 118) whose
// ambiguous faces point at each other across a z plane boundary. The
// manifold correction of the upper cell needs the cell codes of a layer
// whose sample planes are already evicted, so a streaming extractor must
// carry them across the layer boundary.
int frontier_ambiguous_pair_across_z() {
    const int nx = 4, ny = 4, nz = 6;
    std::vector<float> data(std::size_t(nx) * ny * nz, 0.0f);
    auto set = [&](int x, int y, int z) {
        data[std::size_t(x) + std::size_t(nx) * (std::size_t(y) + std::size_t(ny) * std::size_t(z))] = 1.0f;
    };
    // cell (1,1,1) -> cube code 103 (ambiguous face towards +z)
    set(1,1,1); set(2,1,1); set(1,2,1);
    // shared plane
    set(2,1,2); set(1,2,2);
    // cell (1,1,2) -> cube code 118 (ambiguous face towards -z)
    set(1,1,3); set(2,1,3); set(1,2,3);

    const Mesh refOn = buildClassic(data, nx, ny, nz, 0.5f, true, false);
    const Mesh refOff = buildClassic(data, nx, ny, nz, 0.5f, false, false);
    TASK_CHECK(!refOn.quads.empty());
    // sanity: the ambiguous pair must actually trigger the correction,
    // i.e. the manifold rule changes the extracted mesh for this input
    TASK_CHECK(meshDifferences(refOn, refOff) > 0);

    dualmc::DualMC<float> builder;
    const StreamCapture capOn = runStreaming(builder, data, nx, ny, nz, 0.5f, true);
    const StreamCapture capOff = runStreaming(builder, data, nx, ny, nz, 0.5f, false);
    TASK_CHECK(capOn.ok);
    TASK_CHECK(capOff.ok);
    TASK_CHECK(meshDifferences(refOn, capOn.mesh) == 0);
    TASK_CHECK(meshDifferences(refOff, capOff.mesh) == 0);
    return 0;
}

// Cells whose marching cubes case produces several faces have several dual
// points at the same cell coordinates but with different point codes.
// Those vertices must not be merged.
int frontier_sibling_point_codes() {
    const int n = 8;
    const std::vector<float> data = makeHashVolume(n, n, n);
    const Mesh ref = buildClassic(data, n, n, n, 0.5f, true, false);
    const Mesh refOff = buildClassic(data, n, n, n, 0.5f, false, false);
    // the volume must contain sibling vertices (same cell, distinct ids)
    const int refSiblings = countSiblingVertices(ref);
    TASK_CHECK(refSiblings > 0);

    dualmc::DualMC<float> builder;
    const StreamCapture cap = runStreaming(builder, data, n, n, n, 0.5f, true);
    TASK_CHECK(cap.ok);
    TASK_CHECK(meshDifferences(ref, cap.mesh) == 0);
    // siblings survived the streaming extraction unmerged
    TASK_CHECK(countSiblingVertices(cap.mesh) == refSiblings);

    const StreamCapture capOff = runStreaming(builder, data, n, n, n, 0.5f, false);
    TASK_CHECK(capOff.ok);
    TASK_CHECK(meshDifferences(refOff, capOff.mesh) == 0);
    return 0;
}

// The streaming extractor must keep the boundary traversal range of the
// classic extractor: no quads are added near the volume boundary and
// homogeneous volumes stay empty.
int frontier_boundary_traversal_range() {
    // fully inside / fully outside volumes produce no geometry at all
    {
        const int n = 4;
        const std::vector<float> inside(std::size_t(n) * n * n, 1.0f);
        const std::vector<float> outside(std::size_t(n) * n * n, -1.0f);
        dualmc::DualMC<float> builder;
        const StreamCapture capIn = runStreaming(builder, inside, n, n, n, 0.5f, true);
        const StreamCapture capOut = runStreaming(builder, outside, n, n, n, 0.5f, true);
        TASK_CHECK(capIn.ok);
        TASK_CHECK(capOut.ok);
        TASK_CHECK(capIn.mesh.vertices.empty());
        TASK_CHECK(capIn.mesh.quads.empty());
        TASK_CHECK(capOut.mesh.vertices.empty());
        TASK_CHECK(capOut.mesh.quads.empty());
        TASK_CHECK(capIn.stats.emittedVertices == 0u);
        TASK_CHECK(capIn.stats.emittedQuads == 0u);
    }
    // plane fields perpendicular to each axis on small volumes
    for (int axis = 0; axis < 3; ++axis) {
        const int n = 4;
        std::vector<float> data(std::size_t(n) * n * n);
        for (int z = 0; z < n; ++z)
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    const int c[3] = {x, y, z};
                    data[std::size_t(x) + std::size_t(n) * (std::size_t(y) + std::size_t(n) * std::size_t(z))]
                        = float(c[axis]) - 1.5f;
                }
        const Mesh ref = buildClassic(data, n, n, n, 0.0f, true, false);
        TASK_CHECK(!ref.quads.empty());
        dualmc::DualMC<float> builder;
        const StreamCapture cap = runStreaming(builder, data, n, n, n, 0.0f, true);
        TASK_CHECK(cap.ok);
        TASK_CHECK(meshDifferences(ref, cap.mesh) == 0);
    }
    // minimal depth volume (nz = 3): only vertex layer 0 exists
    {
        const int nx = 5, ny = 5, nz = 3;
        const std::vector<float> data = makeHashVolume(nx, ny, nz);
        const Mesh ref = buildClassic(data, nx, ny, nz, 0.5f, true, false);
        dualmc::DualMC<float> builder;
        const StreamCapture cap = runStreaming(builder, data, nx, ny, nz, 0.5f, true);
        TASK_CHECK(cap.ok);
        TASK_CHECK(cap.readCalls == nz);
        TASK_CHECK(meshDifferences(ref, cap.mesh) == 0);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// lifecycle group
// ---------------------------------------------------------------------------

// In a thin, deep volume the peak memory figures must not grow with depth.
int lifecycle_peaks_independent_of_depth() {
    const int nx = 5, ny = 5;
    const int depths[2] = {40, 140};
    dualmc::StreamStats stats[2];
    std::size_t emitted[2];
    for (int k = 0; k < 2; ++k) {
        const int nz = depths[k];
        const std::vector<float> data = makePlaneField(nx, ny, nz);
        dualmc::DualMC<float> builder;
        const StreamCapture cap = runStreaming(builder, data, nx, ny, nz, 0.0f, true);
        TASK_CHECK(cap.ok);
        TASK_CHECK(cap.readCalls == nz);
        stats[k] = cap.stats;
        emitted[k] = cap.stats.emittedVertices;
    }
    // exactly four resident sample planes, independent of depth
    TASK_CHECK(stats[0].peakSampleValues == 4u * std::size_t(nx) * std::size_t(ny));
    TASK_CHECK(stats[1].peakSampleValues == 4u * std::size_t(nx) * std::size_t(ny));
    // same bound on active vertex keys for both depths
    TASK_CHECK(stats[0].peakVertexKeys == stats[1].peakVertexKeys);
    TASK_CHECK(stats[1].peakVertexKeys <= 8u * std::size_t(nx) * std::size_t(ny));
    TASK_CHECK(stats[1].peakVertexKeys > 0u);
    // the output itself does grow with depth
    TASK_CHECK(emitted[1] > emitted[0]);
    return 0;
}

// A failing reader aborts the extraction; no callback is invoked
// afterwards. The same object can be reused immediately.
int lifecycle_reader_failure() {
    const int nx = 6, ny = 6, nz = 8;
    const std::vector<float> data = makeHashVolume(nx, ny, nz);
    dualmc::DualMC<float> builder;
    dualmc::StreamStats stats;
    // statistics are reset at call start
    stats.peakSampleValues = 999999u;
    stats.peakVertexKeys = 999999u;
    stats.emittedVertices = 999999u;
    stats.emittedQuads = 999999u;
    int readCalls = 0, vertexCalls = 0, quadCalls = 0;
    const int failPlane = 3;
    const bool ok = builder.buildStreaming(nx, ny, nz, 0.5f, true,
        [&](int32_t z, float* dst, std::size_t count) -> bool {
            ++readCalls;
            if (z == failPlane) return false;
            const float* src = data.data() + std::size_t(z) * nx * ny;
            for (std::size_t i = 0; i < count; ++i) dst[i] = src[i];
            return true;
        },
        [&](dualmc::QuadIndexType, const dualmc::Vertex&) -> bool {
            ++vertexCalls;
            return true;
        },
        [&](const dualmc::Quad&) -> bool {
            ++quadCalls;
            return true;
        },
        stats);
    TASK_CHECK(!ok);
    // the reader was consulted for planes 0..failPlane exactly once each
    TASK_CHECK(readCalls == failPlane + 1);
    // stats were reset at call start and only count successful emissions
    TASK_CHECK(stats.peakSampleValues == 4u * std::size_t(nx) * std::size_t(ny));
    TASK_CHECK(stats.emittedVertices == std::size_t(vertexCalls));
    TASK_CHECK(stats.emittedQuads == std::size_t(quadCalls));

    // retry on the same object with a healthy reader matches a fresh object
    const StreamCapture retried = runStreaming(builder, data, nx, ny, nz, 0.5f, true);
    dualmc::DualMC<float> fresh;
    const StreamCapture reference = runStreaming(fresh, data, nx, ny, nz, 0.5f, true);
    TASK_CHECK(retried.ok);
    TASK_CHECK(meshDifferences(reference.mesh, retried.mesh) == 0);
    TASK_CHECK(retried.stats.emittedVertices == reference.stats.emittedVertices);
    TASK_CHECK(retried.stats.emittedQuads == reference.stats.emittedQuads);
    TASK_CHECK(retried.stats.peakVertexKeys == reference.stats.peakVertexKeys);
    return 0;
}

// A cancelling sink (vertex or quad callback returning false) stops the
// extraction immediately; no further callback is invoked.
int lifecycle_sink_cancellation() {
    const int nx = 5, ny = 5, nz = 6;
    std::vector<float> data(std::size_t(nx) * ny * nz);
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x)
                data[std::size_t(x) + std::size_t(nx) * (std::size_t(y) + std::size_t(ny) * std::size_t(z))]
                    = float(z) - 0.5f;

    // cancel on the second emitVertex call
    {
        dualmc::DualMC<float> builder;
        dualmc::StreamStats stats;
        int readCalls = 0, vertexCalls = 0, quadCalls = 0;
        const bool ok = builder.buildStreaming(nx, ny, nz, 0.0f, true,
            [&](int32_t z, float* dst, std::size_t count) -> bool {
                ++readCalls;
                const float* src = data.data() + std::size_t(z) * nx * ny;
                for (std::size_t i = 0; i < count; ++i) dst[i] = src[i];
                return true;
            },
            [&](dualmc::QuadIndexType, const dualmc::Vertex&) -> bool {
                ++vertexCalls;
                return vertexCalls < 2;
            },
            [&](const dualmc::Quad&) -> bool {
                ++quadCalls;
                return true;
            },
            stats);
        TASK_CHECK(!ok);
        TASK_CHECK(vertexCalls == 2);
        TASK_CHECK(quadCalls == 0);
        // only the first vertex emission counted
        TASK_CHECK(stats.emittedVertices == 1u);
        TASK_CHECK(stats.emittedQuads == 0u);
        // nothing was read beyond the planes needed for the first layer
        TASK_CHECK(readCalls == 3);
    }

    // cancel on the first emitQuad call
    {
        dualmc::DualMC<float> builder;
        dualmc::StreamStats stats;
        int readCalls = 0, vertexCalls = 0, quadCalls = 0;
        const bool ok = builder.buildStreaming(nx, ny, nz, 0.0f, true,
            [&](int32_t z, float* dst, std::size_t count) -> bool {
                ++readCalls;
                const float* src = data.data() + std::size_t(z) * nx * ny;
                for (std::size_t i = 0; i < count; ++i) dst[i] = src[i];
                return true;
            },
            [&](dualmc::QuadIndexType, const dualmc::Vertex&) -> bool {
                ++vertexCalls;
                return true;
            },
            [&](const dualmc::Quad&) -> bool {
                ++quadCalls;
                return false;
            },
            stats);
        TASK_CHECK(!ok);
        TASK_CHECK(quadCalls == 1);
        // the four vertices of the cancelled quad were emitted first
        TASK_CHECK(vertexCalls == 4);
        TASK_CHECK(stats.emittedVertices == 4u);
        TASK_CHECK(stats.emittedQuads == 0u);
        TASK_CHECK(readCalls == 3);
    }
    return 0;
}

// Exceptions thrown by callbacks propagate unchanged; the already emitted
// prefix is not rolled back and the object stays usable.
int lifecycle_exception_propagation() {
    const int nx = 5, ny = 5, nz = 6;
    const std::vector<float> data = makeHashVolume(nx, ny, nz);

    // emitVertex throws
    {
        dualmc::DualMC<float> builder;
        dualmc::StreamStats stats;
        int vertexCalls = 0;
        bool caught = false;
        try {
            builder.buildStreaming(nx, ny, nz, 0.5f, true,
                [&](int32_t z, float* dst, std::size_t count) -> bool {
                    const float* src = data.data() + std::size_t(z) * nx * ny;
                    for (std::size_t i = 0; i < count; ++i) dst[i] = src[i];
                    return true;
                },
                [&](dualmc::QuadIndexType, const dualmc::Vertex&) -> bool {
                    ++vertexCalls;
                    if (vertexCalls == 3) throw std::runtime_error("stream-stop");
                    return true;
                },
                [&](const dualmc::Quad&) -> bool { return true; },
                stats);
        } catch (const std::runtime_error& e) {
            caught = std::strcmp(e.what(), "stream-stop") == 0;
        }
        TASK_CHECK(caught);
        TASK_CHECK(vertexCalls == 3);
        // the two successful emissions before the throw are still counted
        TASK_CHECK(stats.emittedVertices == 2u);

        // the same object still works after the exception
        const StreamCapture cap = runStreaming(builder, data, nx, ny, nz, 0.5f, true);
        dualmc::DualMC<float> fresh;
        const StreamCapture ref = runStreaming(fresh, data, nx, ny, nz, 0.5f, true);
        TASK_CHECK(cap.ok);
        TASK_CHECK(meshDifferences(ref.mesh, cap.mesh) == 0);
    }

    // emitQuad throws a different exception type
    {
        dualmc::DualMC<float> builder;
        dualmc::StreamStats stats;
        int quadCalls = 0;
        bool caught = false;
        try {
            builder.buildStreaming(nx, ny, nz, 0.5f, true,
                [&](int32_t z, float* dst, std::size_t count) -> bool {
                    const float* src = data.data() + std::size_t(z) * nx * ny;
                    for (std::size_t i = 0; i < count; ++i) dst[i] = src[i];
                    return true;
                },
                [&](dualmc::QuadIndexType, const dualmc::Vertex&) -> bool { return true; },
                [&](const dualmc::Quad&) -> bool {
                    ++quadCalls;
                    throw std::logic_error("quad-stop");
                },
                stats);
        } catch (const std::logic_error& e) {
            caught = std::strcmp(e.what(), "quad-stop") == 0;
        }
        TASK_CHECK(caught);
        TASK_CHECK(quadCalls == 1);
        TASK_CHECK(stats.emittedQuads == 0u);
        TASK_CHECK(stats.emittedVertices > 0u);
    }
    return 0;
}

// Interleaving the classic shared/soup entry points and the streaming
// entry point on one object behaves exactly like using fresh objects.
int lifecycle_reuse_and_interleave() {
    const int nx = 6, ny = 6, nz = 7;
    const std::vector<float> data = makeHashVolume(nx, ny, nz);

    dualmc::DualMC<float> builder;
    // classic shared-vertex extraction
    const Mesh shared1 = buildClassic(data, nx, ny, nz, 0.5f, true, false);
    (void)shared1;
    Mesh sharedOnBuilder;
    builder.build(data.data(), nx, ny, nz, 0.5f, true, false,
                  sharedOnBuilder.vertices, sharedOnBuilder.quads);
    // streaming extraction on the same object
    const StreamCapture stream1 = runStreaming(builder, data, nx, ny, nz, 0.5f, true);
    // classic quad soup on the same object
    Mesh soupOnBuilder;
    builder.build(data.data(), nx, ny, nz, 0.5f, true, true,
                  soupOnBuilder.vertices, soupOnBuilder.quads);
    // and streaming once more
    const StreamCapture stream2 = runStreaming(builder, data, nx, ny, nz, 0.5f, true);

    // fresh-object references
    dualmc::DualMC<float> fresh;
    Mesh freshShared, freshSoup;
    fresh.build(data.data(), nx, ny, nz, 0.5f, true, false,
                freshShared.vertices, freshShared.quads);
    const StreamCapture freshStream = runStreaming(fresh, data, nx, ny, nz, 0.5f, true);
    fresh.build(data.data(), nx, ny, nz, 0.5f, true, true,
                freshSoup.vertices, freshSoup.quads);

    TASK_CHECK(meshDifferences(freshShared, sharedOnBuilder) == 0);
    TASK_CHECK(meshDifferences(freshSoup, soupOnBuilder) == 0);
    TASK_CHECK(stream1.ok);
    TASK_CHECK(stream2.ok);
    TASK_CHECK(meshDifferences(freshStream.mesh, stream1.mesh) == 0);
    TASK_CHECK(meshDifferences(freshStream.mesh, stream2.mesh) == 0);
    TASK_CHECK(stream1.stats.emittedVertices == freshStream.stats.emittedVertices);
    TASK_CHECK(stream2.stats.peakVertexKeys == freshStream.stats.peakVertexKeys);
    TASK_CHECK(stream2.stats.peakSampleValues == freshStream.stats.peakSampleValues);
    // the streaming output still matches the classic shared-vertex mesh
    TASK_CHECK(meshDifferences(freshShared, stream2.mesh) == 0);
    return 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// case registry
// ---------------------------------------------------------------------------

const task_case task_cases[] = {
    {"geometry",  "plane_field_shared",          &geometry_plane_field_shared},
    {"geometry",  "closed_surface_manifold",     &geometry_closed_surface_manifold},
    {"geometry",  "closed_surface_nonmanifold",  &geometry_closed_surface_nonmanifold},
    {"geometry",  "integer_sample_type",         &geometry_integer_sample_type},
    {"frontier",  "ambiguous_pair_across_z",     &frontier_ambiguous_pair_across_z},
    {"frontier",  "sibling_point_codes",         &frontier_sibling_point_codes},
    {"frontier",  "boundary_traversal_range",    &frontier_boundary_traversal_range},
    {"lifecycle", "peaks_independent_of_depth",  &lifecycle_peaks_independent_of_depth},
    {"lifecycle", "reader_failure",              &lifecycle_reader_failure},
    {"lifecycle", "sink_cancellation",           &lifecycle_sink_cancellation},
    {"lifecycle", "exception_propagation",       &lifecycle_exception_propagation},
    {"lifecycle", "reuse_and_interleave",        &lifecycle_reuse_and_interleave}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
