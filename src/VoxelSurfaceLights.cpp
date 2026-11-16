#include "VoxelSurfaceLights.hpp"
#include "VoxelOctree.hpp"
#include "VoxelOctreeReader.hpp"

#include <algorithm>
#include <cmath>

namespace {

const double kLumR = 0.2126;
const double kLumG = 0.7152;
const double kLumB = 0.0722;

struct IntRect {
    int32 lo0, hi0, lo1, hi1;
};

// Splits every free rectangle around one occupied neighbor rectangle.
// All coordinates are integer cell bounds on the root grid.
void subtractRect(std::vector<IntRect> &freeRects, const IntRect &blocker)
{
    std::vector<IntRect> result;
    result.reserve(freeRects.size() * 4);

    for (const IntRect &r : freeRects) {
        int32 lo0 = std::max(r.lo0, blocker.lo0);
        int32 hi0 = std::min(r.hi0, blocker.hi0);
        int32 lo1 = std::max(r.lo1, blocker.lo1);
        int32 hi1 = std::min(r.hi1, blocker.hi1);

        if (lo0 >= hi0 || lo1 >= hi1) {
            result.push_back(r);
            continue;
        }

        if (r.lo0 < lo0)
            result.push_back(IntRect{r.lo0, lo0, r.lo1, r.hi1});
        if (hi0 < r.hi0)
            result.push_back(IntRect{hi0, r.hi0, r.lo1, r.hi1});
        if (r.lo1 < lo1)
            result.push_back(IntRect{lo0, hi0, r.lo1, lo1});
        if (hi1 < r.hi1)
            result.push_back(IntRect{lo0, hi0, hi1, r.hi1});
    }

    freeRects.swap(result);
}

} // namespace

double VoxelSurfaceLights::luminanceOf(const Vec3 &radiance)
{
    return kLumR * double(radiance.x) + kLumG * double(radiance.y) +
           kLumB * double(radiance.z);
}

const Vec3 *VoxelSurfaceLights::emissionOf(uint32 key) const
{
    static const Vec3 black(0.0f, 0.0f, 0.0f);
    auto it = _emission.find(key);
    return it == _emission.end() ? &black : &it->second;
}

VoxelSurfaceLights::VoxelSurfaceLights(const VoxelOctree *tree)
    : _tree(tree), _octree(tree->octreeData()), _octreeSize(tree->octreeSize()),
      _updateStamp(0)
{
    _stats.geometryNodeVisits = 0;
    _stats.massNodesVisited = 0;
    _stats.massNodesUpdated = 0;
    buildGeometry();
}

void VoxelSurfaceLights::buildGeometry()
{
    std::vector<std::vector<int32>> massChildren;

    MassNode root;
    root.parent = -1;
    root.firstChild = -1;
    root.childCount = 0;
    root.mass = 0.0;
    root.subtreeArea = 0.0;
    root.isPatchLeaf = false;
    root.faceId = -1;
    root.stamp = 0;
    _mass.push_back(root);
    massChildren.emplace_back();

    int32 rootLo[3] = {0, 0, 0};
    enumerateLeaves(0, rootLo, GridSize, 0, massChildren);

    // Flatten the child lists; parents always carry smaller indices than
    // their children, so a reverse pass accumulates subtree areas exactly.
    for (size_t n = 0; n < _mass.size(); ++n) {
        if (massChildren[n].empty()) {
            _mass[n].firstChild = -1;
            _mass[n].childCount = 0;
        } else {
            _mass[n].firstChild = int32(_massChildList.size());
            _mass[n].childCount = int32(massChildren[n].size());
            _massChildList.insert(_massChildList.end(),
                                  massChildren[n].begin(), massChildren[n].end());
        }
    }

    for (int32 n = int32(_mass.size()) - 1; n >= 0; --n) {
        const MassNode &node = _mass[n];
        if (node.isPatchLeaf)
            continue;
        double area = 0.0;
        for (int32 k = 0; k < node.childCount; ++k)
            area += _mass[_massChildList[node.firstChild + k]].subtreeArea;
        _mass[n].subtreeArea = area;
    }
}

void VoxelSurfaceLights::enumerateLeaves(uint64 desc, const int32 lo[3], int32 size,
                                         int32 myMass,
                                         std::vector<std::vector<int32>> &massChildren)
{
    ++_stats.geometryNodeVisits;

    OctreeReader reader(_octree, _octreeSize);
    OctreeReader::NodeInfo info = reader.decode(desc);

    int32 half = size >> 1;
    int32 mid[3] = {lo[0] + half, lo[1] + half, lo[2] + half};

    for (int octant = 0; octant < 8; ++octant) {
        uint32 bit = 128u >> octant;
        if (!(info.childMask & bit))
            continue;

        int32 childLo[3];
        for (int a = 0; a < 3; ++a)
            childLo[a] = (octant & (1 << a)) ? lo[a] : mid[a];

        uint64 slot = reader.childSlot(info, octant);

        MassNode node;
        node.parent = myMass;
        node.firstChild = -1;
        node.childCount = 0;
        node.mass = 0.0;
        node.subtreeArea = 0.0;
        node.isPatchLeaf = false;
        node.faceId = -1;
        node.stamp = 0;
        int32 nodeMass = int32(_mass.size());
        _mass.push_back(node);
        massChildren.emplace_back();
        massChildren[myMass].push_back(nodeMass);

        if (!(info.leafMask & bit)) {
            uint32 payload = _octree[slot];
            processLeaf(slot, childLo, half, payload, nodeMass, massChildren);
        } else {
            enumerateLeaves(slot, childLo, half, nodeMass, massChildren);
        }
    }
}

void VoxelSurfaceLights::collectBoxLeaves(uint64 desc, const int32 nodeLo[3], int32 size,
                                          const int32 boxLo[3], const int32 boxHi[3],
                                          std::vector<NeighborRect> &out)
{
    ++_stats.geometryNodeVisits;

    OctreeReader reader(_octree, _octreeSize);
    OctreeReader::NodeInfo info = reader.decode(desc);

    int32 half = size >> 1;
    int32 mid[3] = {nodeLo[0] + half, nodeLo[1] + half, nodeLo[2] + half};

    for (int octant = 0; octant < 8; ++octant) {
        uint32 bit = 128u >> octant;
        if (!(info.childMask & bit))
            continue;

        int32 childLo[3], childHi[3];
        for (int a = 0; a < 3; ++a) {
            childLo[a] = (octant & (1 << a)) ? nodeLo[a] : mid[a];
            childHi[a] = childLo[a] + half;
        }

        bool overlaps = true;
        for (int a = 0; a < 3; ++a)
            if (childHi[a] <= boxLo[a] || childLo[a] >= boxHi[a])
                overlaps = false;
        if (!overlaps)
            continue;

        uint64 slot = reader.childSlot(info, octant);

        if (!(info.leafMask & bit)) {
            NeighborRect rect;
            for (int a = 0; a < 3; ++a) {
                rect.lo[a] = std::max(childLo[a], boxLo[a]);
                rect.hi[a] = std::min(childHi[a], boxHi[a]);
            }
            out.push_back(rect);
        } else {
            collectBoxLeaves(slot, childLo, half, boxLo, boxHi, out);
        }
    }
}

void VoxelSurfaceLights::emitExposedRects(uint64 leafSlot,
                                          const std::vector<NeighborRect> &neighbors,
                                          const int32 queryLo[2], const int32 queryHi[2],
                                          int axis, int sign, int32 plane,
                                          uint32 payload,
                                          int32 groupMass,
                                          std::vector<std::vector<int32>> &massChildren)
{
    std::vector<IntRect> freeRects;
    freeRects.push_back(IntRect{queryLo[0], queryHi[0], queryLo[1], queryHi[1]});

    int a1 = (axis + 1) % 3;
    int a2 = (axis + 2) % 3;
    for (const NeighborRect &n : neighbors) {
        IntRect blocker;
        blocker.lo0 = std::max(n.lo[a1], queryLo[0]);
        blocker.hi0 = std::min(n.hi[a1], queryHi[0]);
        blocker.lo1 = std::max(n.lo[a2], queryLo[1]);
        blocker.hi1 = std::min(n.hi[a2], queryHi[1]);
        if (blocker.lo0 < blocker.hi0 && blocker.lo1 < blocker.hi1)
            subtractRect(freeRects, blocker);
    }

    if (freeRects.empty())
        return;

    const double ws = WorldScale;

    for (const IntRect &r : freeRects) {
        SurfaceLightFace face;
        face.low0 = r.lo0;
        face.high0 = r.hi0;
        face.low1 = r.lo1;
        face.high1 = r.hi1;
        face.plane = plane;
        face.axis = axis;
        face.sign = sign;
        face.payload = payload;
        face.area = double(r.hi0 - r.lo0) * double(r.hi1 - r.lo1) * ws * ws;
        face.normal = Vec3(0.0f, 0.0f, 0.0f);
        face.normal.a[axis] = float(sign);
        face.leafSlot = leafSlot;

        _faces.push_back(face);
        int32 faceId = int32(_faces.size()) - 1;

        MassNode patch;
        patch.parent = groupMass;
        patch.firstChild = -1;
        patch.childCount = 0;
        patch.mass = 0.0;
        patch.subtreeArea = face.area;
        patch.isPatchLeaf = true;
        patch.faceId = faceId;
        patch.stamp = 0;
        int32 patchMass = int32(_mass.size());
        _mass.push_back(patch);
        massChildren.emplace_back();
        massChildren[groupMass].push_back(patchMass);

        _keyPatchNodes[payload].push_back(patchMass);
    }
}

void VoxelSurfaceLights::processLeaf(uint64 slotDesc, const int32 lo[3], int32 size,
                                     uint32 payload, int32 groupMass,
                                     std::vector<std::vector<int32>> &massChildren)
{
    int32 hi[3] = {lo[0] + size, lo[1] + size, lo[2] + size};

    for (int axis = 0; axis < 3; ++axis) {
        int a1 = (axis + 1) % 3;
        int a2 = (axis + 2) % 3;
        int32 queryLo[2] = {lo[a1], lo[a2]};
        int32 queryHi[2] = {hi[a1], hi[a2]};

        for (int side = 0; side < 2; ++side) {
            int sign = (side == 0) ? -1 : 1;
            int32 plane = (side == 0) ? lo[axis] : hi[axis];

            // One-cell-thick slab immediately adjacent to this face, covering
            // exactly the in-plane extent of the leaf.
            int32 boxLo[3], boxHi[3];
            for (int a = 0; a < 3; ++a) {
                if (a == axis) {
                    boxLo[a] = (side == 0) ? lo[axis] - 1 : hi[axis];
                    boxHi[a] = boxLo[a] + 1;
                } else {
                    boxLo[a] = lo[a];
                    boxHi[a] = hi[a];
                }
            }

            std::vector<NeighborRect> neighbors;
            bool insideRoot = true;
            for (int a = 0; a < 3; ++a)
                if (boxHi[a] <= 0 || boxLo[a] >= GridSize)
                    insideRoot = false;
            if (insideRoot) {
                for (int a = 0; a < 3; ++a) {
                    boxLo[a] = std::max(boxLo[a], int32(0));
                    boxHi[a] = std::min(boxHi[a], int32(GridSize));
                }
                int32 rootLo[3] = {0, 0, 0};
                collectBoxLeaves(0, rootLo, GridSize, boxLo, boxHi, neighbors);
            }

            emitExposedRects(slotDesc, neighbors, queryLo, queryHi, axis, sign, plane,
                             payload, groupMass, massChildren);
        }
    }
}

void VoxelSurfaceLights::setEmission(uint32 key, Vec3 radiance)
{
    if (!std::isfinite(radiance.x) || !std::isfinite(radiance.y) ||
        !std::isfinite(radiance.z) ||
        radiance.x < 0.0f || radiance.y < 0.0f || radiance.z < 0.0f)
        throw std::invalid_argument("emission must be finite and non-negative");

    auto oldIt = _emission.find(key);
    Vec3 oldRadiance(0.0f, 0.0f, 0.0f);
    if (oldIt != _emission.end())
        oldRadiance = oldIt->second;
    if (oldRadiance == radiance) {
        _stats.massNodesVisited = 0;
        _stats.massNodesUpdated = 0;
        return;
    }

    ++_updateStamp;
    uint64 visited = 0;
    uint64 updated = 0;

    double newY = luminanceOf(radiance);

    // Touch every patch carrying this key and mark its ancestor chain. The
    // patch masses are set to the exact new value; ancestor masses are then
    // recomputed bottom-up as sums of their children so blacking a key out
    // leaves a true zero rather than a floating-point residual.
    std::vector<int32> touched;
    auto patchIt = _keyPatchNodes.find(key);
    if (patchIt != _keyPatchNodes.end()) {
        for (int32 patchNode : patchIt->second) {
            double area = _mass[patchNode].subtreeArea;

            ++visited;
            if (_mass[patchNode].stamp != _updateStamp) {
                _mass[patchNode].stamp = _updateStamp;
                ++updated;
            }
            _mass[patchNode].mass = area * newY;

            for (int32 p = _mass[patchNode].parent; p >= 0; p = _mass[p].parent) {
                ++visited;
                if (_mass[p].stamp != _updateStamp) {
                    _mass[p].stamp = _updateStamp;
                    touched.push_back(p);
                    ++updated;
                }
            }
        }
    }

    // Parents always have smaller indices than children, so descending order
    // recomputes every touched node after all of its touched descendants.
    std::sort(touched.begin(), touched.end(), std::greater<int32>());
    for (int32 p : touched) {
        const MassNode &node = _mass[p];
        double sum = 0.0;
        for (int32 k = 0; k < node.childCount; ++k) {
            ++visited;
            sum += _mass[_massChildList[node.firstChild + k]].mass;
        }
        _mass[p].mass = sum;
    }

    bool black = (radiance.x == 0.0f && radiance.y == 0.0f && radiance.z == 0.0f);
    if (black)
        _emission.erase(key);
    else
        _emission[key] = radiance;

    _stats.massNodesVisited = visited;
    _stats.massNodesUpdated = updated;
}

bool VoxelSurfaceLights::sample(double u0, double u1, double u2,
                                SurfaceLightSample &result) const
{
    result = SurfaceLightSample();
    double total = _mass[0].mass;
    if (!(total > 0.0))
        return false;

    double target = u0 * total;
    int32 node = 0;
    while (!_mass[node].isPatchLeaf) {
        const MassNode &n = _mass[node];
        int32 chosen = -1;
        for (int32 k = 0; k < n.childCount; ++k) {
            int32 child = _massChildList[n.firstChild + k];
            double childMass = _mass[child].mass;
            if (target < childMass) {
                chosen = child;
                break;
            }
            target -= childMass;
        }
        if (chosen < 0) {
            // Numerical overshoot at the total boundary: take the last lit child.
            for (int32 k = n.childCount - 1; k >= 0; --k) {
                int32 child = _massChildList[n.firstChild + k];
                if (_mass[child].mass > 0.0) {
                    chosen = child;
                    break;
                }
            }
        }
        if (chosen < 0)
            return false; // defensive: no lit descendant despite positive total
        node = chosen;
    }

    int32 faceId = _mass[node].faceId;
    const SurfaceLightFace &face = _faces[faceId];
    const Vec3 *radiance = emissionOf(face.payload);

    int a1 = (face.axis + 1) % 3;
    int a2 = (face.axis + 2) % 3;
    const double ws = WorldScale;

    result.position = Vec3(0.0f, 0.0f, 0.0f);
    result.position.a[face.axis] = float(1.0 + double(face.plane) * ws);
    result.position.a[a1] = float(1.0 +
        (double(face.low0) + u1 * double(face.high0 - face.low0)) * ws);
    result.position.a[a2] = float(1.0 +
        (double(face.low1) + u2 * double(face.high1 - face.low1)) * ws);
    result.normal = face.normal;
    result.radiance = *radiance;
    result.payload = face.payload;
    result.faceId = faceId;
    result.pdfArea = luminanceOf(*radiance) / total;
    return true;
}

double VoxelSurfaceLights::pdfArea(int32 faceId) const
{
    const SurfaceLightFace &face = _faces[faceId];
    double total = _mass[0].mass;
    if (!(total > 0.0))
        return 0.0;
    const Vec3 *radiance = emissionOf(face.payload);
    return luminanceOf(*radiance) / total;
}

double VoxelSurfaceLights::surfaceArea() const
{
    return _mass[0].subtreeArea;
}

double VoxelSurfaceLights::totalMass() const
{
    return _mass[0].mass;
}
