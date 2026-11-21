//
// voxel_scene.cpp
//
// Incremental voxel scene implementation.  All geometric rasterization goes
// through vx__tri_enumerate_cells, the same single-triangle entry used by the
// public vx_voxelize / vx_voxelize_pc path; this file holds no triangle/box
// intersector of its own and never invokes the full-mesh public voxelizers.
//

#include "voxel_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace voxel_scene {

namespace {

bool sameColor(vx_color_t const& a, vx_color_t const& b)
{
    // Exact comparison: +0.0f and -0.0f compare equal, NaNs are excluded by
    // the caller's finiteness guarantee.
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

bool samePosition(vx_vertex_t const& a, vx_vertex_t const& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

} // namespace

void VoxelScene::rasterCallback(void* user,
    int cellx, int celly, int cellz,
    float w1, float w2, float w3,
    vx_vertex_t center)
{
    (void) center;
    RasterContext* ctx = static_cast<RasterContext*>(user);
    CellKey key = {{ cellx, celly, cellz }};

    Weights weights;
    weights.w[0] = w1;
    weights.w[1] = w2;
    weights.w[2] = w3;

    (*ctx->triangleCells)[key] = weights;
    (*ctx->cells)[key].triangles[ctx->triangleId] = weights;
}

VoxelScene::VoxelScene(vx_mesh_t const* mesh,
                       float stepX, float stepY, float stepZ,
                       float precision)
{
    step_[0] = stepX;
    step_[1] = stepY;
    step_[2] = stepZ;
    precision_ = precision;
    colored_ = mesh->colors != NULL;
    vertexCount_ = mesh->nvertices;
    triangleCount_ = mesh->nindices / 3;

    positions_.resize(vertexCount_);
    for (std::size_t i = 0; i < vertexCount_; ++i) {
        positions_[i] = mesh->vertices[i];
    }

    if (colored_) {
        colors_.resize(vertexCount_);
        for (std::size_t i = 0; i < vertexCount_; ++i) {
            colors_[i] = mesh->colors[i];
        }
    }

    indices_.resize(mesh->nindices);
    for (std::size_t i = 0; i < mesh->nindices; ++i) {
        indices_[i] = mesh->indices[i];
    }

    vertexTriangles_.resize(vertexCount_);
    for (unsigned int t = 0; t < triangleCount_; ++t) {
        for (int k = 0; k < 3; ++k) {
            vertexTriangles_[indices_[3 * t + k]].push_back(t);
        }
    }

    triangleCells_.resize(triangleCount_);

    // Initial full rasterization, one triangle at a time, through the shared
    // single-triangle entry.
    for (unsigned int t = 0; t < triangleCount_; ++t) {
        unsigned long long tests = 0;
        rasterizeTriangle(t, &tests);
    }
}

VoxelScene::~VoxelScene() = default;

vx_color_t VoxelScene::colorOf(unsigned int triangleId, Weights const& weights) const
{
    unsigned int i0 = indices_[3 * triangleId + 0];
    unsigned int i1 = indices_[3 * triangleId + 1];
    unsigned int i2 = indices_[3 * triangleId + 2];

    // Keep the exact multiply/add order of vx__voxelize_cell_hit so the
    // resulting float bits match the public voxelizer output.
    vx_color_t c1 = colors_[i0];
    vx_color_t c2 = colors_[i1];
    vx_color_t c3 = colors_[i2];

    c1.r *= weights.w[0]; c1.g *= weights.w[0]; c1.b *= weights.w[0];
    c2.r *= weights.w[1]; c2.g *= weights.w[1]; c2.b *= weights.w[1];
    c3.r *= weights.w[2]; c3.g *= weights.w[2]; c3.b *= weights.w[2];

    c1.r += c2.r; c1.g += c2.g; c1.b += c2.b;
    c1.r += c3.r; c1.g += c3.g; c1.b += c3.b;
    return c1;
}

VoxelRecord VoxelScene::recordFor(CellKey const& key) const
{
    CellState const& state = cells_.at(key);
    VoxelRecord record;
    record.key = key;
    record.contributorCount = static_cast<unsigned int>(state.triangles.size());
    record.winnerTriangle = state.triangles.begin()->first;
    if (colored_) {
        Weights const& weights = state.triangles.begin()->second;
        record.color = colorOf(record.winnerTriangle, weights);
    } else {
        record.color.r = record.color.g = record.color.b = 0.0f;
    }
    return record;
}

void VoxelScene::rasterizeTriangle(unsigned int triangleId,
                                   unsigned long long* tests)
{
    unsigned int i0 = indices_[3 * triangleId + 0];
    unsigned int i1 = indices_[3 * triangleId + 1];
    unsigned int i2 = indices_[3 * triangleId + 2];

    vx_vertex_t points[3];
    points[0] = positions_[i0];
    points[1] = positions_[i1];
    points[2] = positions_[i2];

    RasterContext ctx;
    ctx.triangleId = triangleId;
    ctx.triangleCells = &triangleCells_[triangleId];
    ctx.cells = &cells_;

    int degenerate = 0;
    *tests += vx__tri_enumerate_cells(points, step_, precision_,
        rasterCallback, &ctx, &degenerate);
}

VoxelPatch VoxelScene::apply(std::vector<VertexUpdate> const& records)
{
    VoxelPatch patch;

    // Collapse duplicate vertices: the last record for a vertex wins.
    std::map<unsigned int, VertexUpdate> merged;
    for (VertexUpdate const& update : records) {
        merged[update.vertex] = update;
    }

    // Final per-vertex change flags, evaluated against the current state.
    std::map<unsigned int, bool> positionChanged;
    std::map<unsigned int, bool> colorChanged;
    for (auto const& entry : merged) {
        unsigned int v = entry.first;
        VertexUpdate const& update = entry.second;
        positionChanged[v] = !samePosition(positions_[v], update.position);
        colorChanged[v] = colored_ && !sameColor(colors_[v], update.color);
    }

    // Merge shared-vertex dependencies: a triangle is touched once even when
    // several of its vertices move in the same batch.
    std::map<unsigned int, bool> geometryTriangles;
    std::map<unsigned int, bool> colorTriangles;
    for (auto const& entry : merged) {
        unsigned int v = entry.first;
        for (unsigned int t : vertexTriangles_[v]) {
            if (positionChanged[v]) {
                geometryTriangles[t] = true;
                colorTriangles.erase(t);
            } else if (colorChanged[v] && !geometryTriangles.count(t)) {
                colorTriangles[t] = true;
            }
        }
    }

    // Every cell touched by an affected triangle is part of the comparison.
    std::map<CellKey, VoxelRecord> before;
    auto collectCells = [&](unsigned int t) {
        for (auto const& entry : triangleCells_[t]) {
            CellKey const& key = entry.first;
            if (!before.count(key)) {
                before[key] = recordFor(key);
            }
        }
    };
    for (auto const& entry : geometryTriangles) { collectCells(entry.first); }
    for (auto const& entry : colorTriangles) { collectCells(entry.first); }

    // Commit new vertex values before recomputing anything.
    for (auto const& entry : merged) {
        unsigned int v = entry.first;
        if (positionChanged[v]) {
            positions_[v] = entry.second.position;
        }
        if (colored_ && colorChanged[v]) {
            colors_[v] = entry.second.color;
        }
    }

    // Geometry pass: drop old contributions, re-rasterize once per triangle.
    for (auto const& entry : geometryTriangles) {
        unsigned int t = entry.first;
        for (auto const& cellEntry : triangleCells_[t]) {
            CellKey const& key = cellEntry.first;
            auto cellIt = cells_.find(key);
            cellIt->second.triangles.erase(t);
            if (cellIt->second.triangles.empty()) {
                cells_.erase(cellIt);
            }
        }
        triangleCells_[t].clear();
        rasterizeTriangle(t, &patch.work.candidateTests);
    }

    // Color-only pass: weights stay valid because geometry did not move, so
    // there is nothing to recompute structurally and no overlap test to run.

    // Diff final state against the state on entry.
    std::map<CellKey, char> touched;
    for (auto const& entry : before) { touched[entry.first] = 1; }
    for (auto const& entry : geometryTriangles) {
        for (auto const& cellEntry : triangleCells_[entry.first]) {
            touched[cellEntry.first] = 1;
        }
    }
    for (auto const& entry : colorTriangles) {
        for (auto const& cellEntry : triangleCells_[entry.first]) {
            touched[cellEntry.first] = 1;
        }
    }

    for (auto const& entry : touched) {
        CellKey const& key = entry.first;
        bool had = before.count(key) != 0;
        auto afterIt = cells_.find(key);
        bool has = afterIt != cells_.end();

        if (had && !has) {
            patch.remove.push_back(key);
            continue;
        }
        if (!has) {
            continue;
        }

        VoxelRecord after = recordFor(key);
        if (!had) {
            patch.upsert.push_back(after);
            continue;
        }

        VoxelRecord const& oldRecord = before.at(key);
        bool changed = oldRecord.winnerTriangle != after.winnerTriangle ||
                       oldRecord.contributorCount != after.contributorCount;
        if (colored_ && !changed) {
            changed = !sameColor(oldRecord.color, after.color);
        }
        if (changed) {
            patch.upsert.push_back(after);
        }
    }

    // Both cell arrays are already sorted and unique because cells_/maps are
    // ordered; normalize defensively anyway.
    std::sort(patch.remove.begin(), patch.remove.end());
    patch.remove.erase(std::unique(patch.remove.begin(), patch.remove.end()),
        patch.remove.end());
    std::sort(patch.upsert.begin(), patch.upsert.end(),
        [](VoxelRecord const& a, VoxelRecord const& b) { return a.key < b.key; });
    patch.upsert.erase(std::unique(patch.upsert.begin(), patch.upsert.end(),
        [](VoxelRecord const& a, VoxelRecord const& b) { return a.key == b.key; }),
        patch.upsert.end());

    for (auto const& entry : geometryTriangles) {
        patch.work.triangleIds.push_back(entry.first);
    }
    for (auto const& entry : colorTriangles) {
        // A degenerate triangle covers no cell: its vertex colors cannot
        // influence any record, so it did no color work.
        if (!triangleCells_[entry.first].empty()) {
            patch.work.colorTriangleIds.push_back(entry.first);
        }
    }

    return patch;
}

std::vector<VoxelRecord> VoxelScene::snapshot() const
{
    std::vector<VoxelRecord> out;
    out.reserve(cells_.size());
    for (auto const& entry : cells_) {
        out.push_back(recordFor(entry.first));
    }
    return out;
}

vx_point_cloud_t* VoxelScene::pointCloud() const
{
    vx_point_cloud_t* pc = static_cast<vx_point_cloud_t*>(
        std::malloc(sizeof(vx_point_cloud_t)));
    std::size_t n = cells_.size();
    pc->nvertices = n;
    pc->vertices = static_cast<vx_vertex_t*>(
        std::malloc((n ? n : 1) * sizeof(vx_vertex_t)));
    pc->colors = colored_
        ? static_cast<vx_color_t*>(std::malloc((n ? n : 1) * sizeof(vx_color_t)))
        : NULL;

    std::size_t i = 0;
    for (auto const& entry : cells_) {
        CellKey const& key = entry.first;
        pc->vertices[i].x = key[0] * step_[0];
        pc->vertices[i].y = key[1] * step_[1];
        pc->vertices[i].z = key[2] * step_[2];
        if (colored_) {
            pc->colors[i] = recordFor(key).color;
        }
        ++i;
    }
    return pc;
}

vx_mesh_t* VoxelScene::cubeMesh() const
{
    std::size_t voxels = cells_.size();
    vx_mesh_t* outmesh = static_cast<vx_mesh_t*>(std::malloc(sizeof(vx_mesh_t)));
    std::size_t nvertices = voxels * 8;
    std::size_t nindices = voxels * VOXELIZER_INDICES_SIZE;
    outmesh->nnormals = VOXELIZER_NORMAL_INDICES_SIZE;
    outmesh->vertices = static_cast<vx_vertex_t*>(
        std::calloc(nvertices ? nvertices : 1, sizeof(vx_vertex_t)));
    outmesh->normals = static_cast<vx_vec3_t*>(std::calloc(6, sizeof(vx_vec3_t)));
    outmesh->colors = colored_
        ? static_cast<vx_color_t*>(std::calloc(nvertices ? nvertices : 1,
              sizeof(vx_color_t)))
        : NULL;
    outmesh->indices = static_cast<unsigned int*>(
        std::calloc(nindices ? nindices : 1, sizeof(unsigned int)));
    outmesh->normalindices = static_cast<unsigned int*>(
        std::calloc(nindices ? nindices : 1, sizeof(unsigned int)));
    outmesh->nindices = 0;
    outmesh->nvertices = 0;
    outmesh->nnormals = VOXELIZER_NORMAL_INDICES_SIZE;

    std::memcpy(outmesh->normals, vx_normals, 18 * sizeof(float));

    float hvs[3] = { step_[0] * 0.5f, step_[1] * 0.5f, step_[2] * 0.5f };
    float vertices[24] = {
        -hvs[0],  hvs[1],  hvs[2],
        -hvs[0], -hvs[1],  hvs[2],
         hvs[0], -hvs[1],  hvs[2],
         hvs[0],  hvs[1],  hvs[2],
        -hvs[0],  hvs[1], -hvs[2],
        -hvs[0], -hvs[1], -hvs[2],
         hvs[0], -hvs[1], -hvs[2],
         hvs[0],  hvs[1], -hvs[2],
    };

    for (auto const& entry : cells_) {
        CellKey const& key = entry.first;
        vx_vertex_t center;
        center.x = key[0] * step_[0];
        center.y = key[1] * step_[1];
        center.z = key[2] * step_[2];
        vx_color_t color = colored_ ? recordFor(key).color
                                    : vx_color_t{};
        vx__add_voxel(outmesh, &center, color, vertices);
    }

    return outmesh;
}

} // namespace voxel_scene
