#ifndef VOXELSURFACELIGHTS_HPP_
#define VOXELSURFACELIGHTS_HPP_

#include "IntTypes.hpp"
#include "math/Vec3.hpp"

#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <vector>

class VoxelOctree;

// One exposed rectangular patch of an occupied leaf's outer surface.
// faceId is the stable index in the resource's descriptor array and never
// changes after construction, even when lights are toggled.
struct SurfaceLightFace {
    // Axis aligned rectangle in the integer root grid [0, GridSize)^3.
    // low0/high0 and low1/high1 span the two in-plane axes and plane is the
    // fixed coordinate along the face axis.
    int32 low0, high0;
    int32 low1, high1;
    int32 plane;
    int32 axis;    // 0 = x face, 1 = y face, 2 = z face
    int32 sign;    // +1 outward normal points along +axis, -1 along -axis

    uint32 payload;   // full 32 bit leaf payload, used as emission table key
    double area;      // world-space area of the exposed rectangle
    Vec3 normal;       // outward unit geometric normal

    uint64 leafSlot;  // compact-tree slot of the owning leaf's payload
};

struct SurfaceLightSample {
    Vec3 position;
    Vec3 normal;
    Vec3 radiance;
    uint32 payload;
    int32 faceId;
    double pdfArea;
};

struct SurfaceLightStats {
    uint64 geometryNodeVisits; // compact-tree descriptor reads during construction
    uint64 massNodesVisited;   // mass nodes read by the most recent setEmission
    uint64 massNodesUpdated;   // distinct mass nodes rewritten by the most recent setEmission
};

// A surface-light resource bound to an (immutable) VoxelOctree. Several
// resources may bind the same tree; each keeps an independent emission table
// and sampling structure. The tree must outlive every resource bound to it.
class VoxelSurfaceLights {
public:
    enum {
        GridBits = 23,               // matches VoxelOctree::MaxScale
        GridSize = 1 << GridBits     // root spans [1,2] in world space
    };
    static constexpr double WorldScale = 1.0 / double(GridSize);

private:
    // Hierarchical mass tree. Internal nodes mirror octree descriptors and
    // leaf groups; mass leaves are the individual exposed patches. Parents
    // always have a smaller index than their children.
    struct MassNode {
        int32 parent;
        int32 firstChild;
        int32 childCount;
        double mass;        // subtree mass (area * luminance), 0 while dark
        double subtreeArea; // total patch area in the subtree
        bool isPatchLeaf;
        int32 faceId;       // valid when isPatchLeaf
        uint64 stamp;       // incremental-update membership marker
    };

    struct NeighborRect {
        int32 lo[3];
        int32 hi[3];
    };

    const VoxelOctree *_tree;
    const uint32 *_octree;
    uint64 _octreeSize;

    std::vector<SurfaceLightFace> _faces;

    // Emission table keyed by the full 32 bit leaf payload.
    std::unordered_map<uint32, Vec3> _emission;
    // Mass leaf (patch) indices carrying a given payload key.
    std::unordered_map<uint32, std::vector<int32>> _keyPatchNodes;

    std::vector<MassNode> _mass;
    std::vector<int32> _massChildList;
    uint64 _updateStamp;

    SurfaceLightStats _stats;

    // Geometry construction (hierarchical, no finest-grid expansion).
    void buildGeometry();
    void enumerateLeaves(uint64 desc, const int32 lo[3], int32 size, int32 parentMass,
                         std::vector<std::vector<int32>> &massChildren);
    void processLeaf(uint64 slotDesc, const int32 lo[3], int32 size, uint32 payload,
                     int32 groupMass, std::vector<std::vector<int32>> &massChildren);
    // Collects occupied leaves intersecting the grid box [boxLo, boxHi),
    // clipped to that box, by hierarchical traversal with octant pruning.
    // Used for face-neighbor slabs; no finest-grid expansion is performed.
    void collectBoxLeaves(uint64 desc, const int32 nodeLo[3], int32 size,
                          const int32 boxLo[3], const int32 boxHi[3],
                          std::vector<NeighborRect> &out);
    void emitExposedRects(uint64 leafSlot,
                          const std::vector<NeighborRect> &neighbors,
                          const int32 queryLo[2], const int32 queryHi[2],
                          int axis, int sign, int32 plane, uint32 payload,
                          int32 groupMass, std::vector<std::vector<int32>> &massChildren);

    static double luminanceOf(const Vec3 &radiance);
    const Vec3 *emissionOf(uint32 key) const;

public:
    explicit VoxelSurfaceLights(const VoxelOctree *tree);

    // Sets the radiance associated with one payload key. Values must be
    // finite and non-negative; otherwise std::invalid_argument is thrown and
    // the previous emission distribution is left completely untouched.
    // Assigning the same value again performs no work.
    void setEmission(uint32 key, Vec3 radiance);

    // Area sampling with three independent random numbers in [0,1).
    // Returns false (and leaves the result zeroed) when totalMass() == 0.
    bool sample(double u0, double u1, double u2, SurfaceLightSample &result) const;

    // Joint area density of a face: Y(face) / totalMass, zero for dark faces.
    double pdfArea(int32 faceId) const;

    double surfaceArea() const;
    double totalMass() const;
    const SurfaceLightStats &lastUpdateStats() const {
        return _stats;
    }

    size_t faceCount() const {
        return _faces.size();
    }

    const SurfaceLightFace &face(int32 faceId) const {
        return _faces[faceId];
    }

    // Compact-tree payload slot of the leaf owning the given face. Shadow
    // rays use it to exempt the light leaf from endpoint self-occlusion.
    uint64 faceLeafSlot(int32 faceId) const {
        return _faces[faceId].leafSlot;
    }

    const VoxelOctree *tree() const {
        return _tree;
    }
};

#endif /* VOXELSURFACELIGHTS_HPP_ */
