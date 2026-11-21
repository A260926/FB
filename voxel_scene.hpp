//
// voxel_scene.hpp
//
// Incremental voxel preview for a fixed-topology triangle mesh.  The scene
// keeps its own copy of the input mesh and only re-rasterizes the triangles
// attached to vertices whose position changed; pure color updates reuse the
// persistent triangle/cell interpolation weights and perform no triangle/box
// tests at all.
//
// Grid convention:
//  - cells are identified by signed integer keys (x, y, z);
//  - a cell center is key * voxel step on every axis (steps may differ);
//  - a cell's color is supplied by the still-covering triangle with the
//    smallest id, where triangle ids are the original index triplet order.
//

#ifndef VOXEL_SCENE_HPP
#define VOXEL_SCENE_HPP

#include "voxelizer.h"

#include <array>
#include <cstddef>
#include <map>
#include <vector>

namespace voxel_scene {

// Signed integer grid key.  Lexicographic order is the snapshot ordering.
using CellKey = std::array<int, 3>;

struct VoxelRecord {
    CellKey key;
    vx_color_t color;             // meaningful only for a colored scene
    unsigned int winnerTriangle;  // smallest covering triangle id
    unsigned int contributorCount;
};

// One requested vertex change.  Duplicate entries for the same vertex in a
// single apply() batch collapse to the last one.  Color is ignored when the
// source mesh has no colors.
struct VertexUpdate {
    unsigned int vertex;
    vx_vertex_t position;
    vx_color_t color;
};

struct VoxelWork {
    // Triangles that actually went through geometric rasterization,
    // ascending and unique.
    std::vector<unsigned int> triangleIds;
    // Triangles whose contribution only needed a color re-evaluation,
    // ascending and unique.  Disjoint from triangleIds.
    std::vector<unsigned int> colorTriangleIds;
    // Number of triangle/box overlap tests performed by the batch.
    unsigned long long candidateTests = 0;
};

struct VoxelPatch {
    // Full records of cells created or modified relative to the state before
    // apply(), sorted and unique by key.
    std::vector<VoxelRecord> upsert;
    // Keys of cells that disappeared, sorted and unique.
    std::vector<CellKey> remove;
    VoxelWork work;
};

class VoxelScene {
public:
    // The mesh is copied: topology is fixed afterwards and triangle ids are
    // the indices triplet numbering (indices[0..2] is triangle 0, ...).
    // Validity/finiteness of the input, power-of-two steps in [1/128, 64] and
    // precision in [0, minStep/4] are guaranteed by the caller.
    VoxelScene(vx_mesh_t const* mesh,
               float stepX, float stepY, float stepZ,
               float precision);
    ~VoxelScene();

    VoxelScene(VoxelScene const&) = delete;
    VoxelScene& operator=(VoxelScene const&) = delete;

    // Applies a batch of vertex updates and returns the patch relative to the
    // state on entry.  Replaying the patch (upserts then removes) reproduces
    // snapshot().  Never calls the public full-mesh voxelizers.
    VoxelPatch apply(std::vector<VertexUpdate> const& records);

    // All currently occupied cells, lexicographically ordered by key.
    std::vector<VoxelRecord> snapshot() const;

    // Freshly allocated exports; release with the original free functions.
    // No returned storage borrows from the scene.
    vx_point_cloud_t* pointCloud() const;
    vx_mesh_t* cubeMesh() const;

    bool colored() const { return colored_; }
    std::size_t voxelCount() const { return cells_.size(); }

private:
    struct Weights {
        float w[3];
    };
    struct CellState {
        // Covering triangles keyed by id so begin() is always the winner.
        std::map<unsigned int, Weights> triangles;
    };
    struct RasterContext {
        unsigned int triangleId;
        std::map<CellKey, Weights>* triangleCells;
        std::map<CellKey, CellState>* cells;
    };

    static void rasterCallback(void* user,
        int cellx, int celly, int cellz,
        float w1, float w2, float w3,
        vx_vertex_t center);
    void rasterizeTriangle(unsigned int triangleId, unsigned long long* tests);
    VoxelRecord recordFor(CellKey const& key) const;
    vx_color_t colorOf(unsigned int triangleId, Weights const& weights) const;

    float step_[3];
    float precision_;
    bool colored_;
    std::size_t vertexCount_;
    std::size_t triangleCount_;
    std::vector<vx_vertex_t> positions_;
    std::vector<vx_color_t> colors_;
    std::vector<unsigned int> indices_;
    // Incident triangle ids per vertex.
    std::vector<std::vector<unsigned int> > vertexTriangles_;
    // Persistent contributions of each triangle (sparse, no dense grid).
    std::vector<std::map<CellKey, Weights> > triangleCells_;
    // Occupied cells with every covering triangle.
    std::map<CellKey, CellState> cells_;
};

} // namespace voxel_scene

#endif // VOXEL_SCENE_HPP
