#include "VoxelDirect.hpp"
#include "VoxelOctree.hpp"
#include "VoxelOctreeReader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

const double kPi = 3.14159265358979323846;

// Exact open-interval visibility test driven directly by the compact
// hierarchy. The ray is walked through octree nodes (not finest cells): empty
// octants are skipped through the child masks, so large empty regions cost a
// single descriptor test. A leaf only casts a shadow when the ray crosses it
// over a positive parameter length strictly between the receiver (t = 0) and
// the light sample (t = 1). The receiver and light leaves are identified by
// payload slots so surface contacts at either endpoint never self-occlude,
// and no epsilon is applied, keeping one-cell-thick blockers fully effective.
bool unobstructed(const OctreeReader &reader, const Vec3 &P, const Vec3 &S,
                  uint64 lightSlot, uint64 receiverSlot, bool receiverKnown)
{
    const double h = VoxelSurfaceLights::WorldScale;

    double D[3] = {double(S.x) - double(P.x),
                   double(S.y) - double(P.y),
                   double(S.z) - double(P.z)};
    double O[3] = {double(P.x), double(P.y), double(P.z)};

    struct Frame {
        uint64 desc;
        int32 lo[3];
        int32 size;
        double tLo, tHi;
    };
    Frame stack[OctreeReader::GridBits + 1];
    int sp = 0;
    stack[sp++] = Frame{0, {0, 0, 0}, OctreeReader::GridSize, 0.0, 1.0};

    struct Candidate {
        int octant;
        double tLo, tHi;
    };

    while (sp > 0) {
        Frame frame = stack[--sp];
        if (frame.tHi <= 0.0 || frame.tLo >= 1.0)
            continue;

        OctreeReader::NodeInfo info = reader.decode(frame.desc);
        int32 half = frame.size >> 1;
        int32 mid[3] = {frame.lo[0] + half, frame.lo[1] + half, frame.lo[2] + half};

        Candidate list[8];
        int count = 0;
        for (int octant = 0; octant < 8; ++octant) {
            uint32 bit = 128u >> octant;
            if (!(info.childMask & bit))
                continue;

            int32 boxLo[3], boxHi[3];
            for (int a = 0; a < 3; ++a) {
                boxLo[a] = (octant & (1 << a)) ? frame.lo[a] : mid[a];
                boxHi[a] = boxLo[a] + half;
            }

            // Ray/box slab intersection in parameter space.
            double tLo = frame.tLo, tHi = frame.tHi;
            bool hit = true;
            for (int a = 0; a < 3; ++a) {
                double wLo = 1.0 + double(boxLo[a]) * h;
                double wHi = 1.0 + double(boxHi[a]) * h;
                if (D[a] == 0.0) {
                    if (O[a] < wLo || O[a] > wHi) { hit = false; break; }
                } else {
                    double t1 = (wLo - O[a]) / D[a];
                    double t2 = (wHi - O[a]) / D[a];
                    double lo = t1 < t2 ? t1 : t2;
                    double hi = t1 < t2 ? t2 : t1;
                    if (lo > tLo) tLo = lo;
                    if (hi < tHi) tHi = hi;
                    if (tLo >= tHi) { hit = false; break; }
                }
            }
            if (!hit)
                continue;

            Candidate cand;
            cand.octant = octant;
            cand.tLo = tLo;
            cand.tHi = tHi;
            int j = count++;
            while (j > 0 && list[j - 1].tLo > cand.tLo) {
                list[j] = list[j - 1];
                --j;
            }
            list[j] = cand;
        }

        // Push far children first so the nearest is processed first.
        for (int k = count - 1; k >= 0; --k) {
            int octant = list[k].octant;
            uint32 bit = 128u >> octant;
            uint64 slot = reader.childSlot(info, octant);

            int32 childLo[3];
            for (int a = 0; a < 3; ++a)
                childLo[a] = (octant & (1 << a)) ? frame.lo[a] : mid[a];

            if (!(info.leafMask & bit)) {
                double openLo = std::max(list[k].tLo, 0.0);
                double openHi = std::min(list[k].tHi, 1.0);
                if (openHi > openLo &&
                    slot != lightSlot &&
                    !(receiverKnown && slot == receiverSlot))
                    return false;
            } else {
                if (sp >= OctreeReader::GridBits + 1)
                    return false;
                Frame child;
                child.desc = slot;
                child.lo[0] = childLo[0]; child.lo[1] = childLo[1]; child.lo[2] = childLo[2];
                child.size = half;
                child.tLo = list[k].tLo;
                child.tHi = list[k].tHi;
                stack[sp++] = child;
            }
        }
    }

    return true;
}

} // namespace

Vec3 estimateDirect(const VoxelSurfaceLights &lights,
                    const Vec3 &P, const Vec3 &N, const Vec3 &albedo,
                    double u0, double u1, double u2)
{
    const Vec3 black(0.0f, 0.0f, 0.0f);

    if (!std::isfinite(P.x) || !std::isfinite(P.y) || !std::isfinite(P.z) ||
        !std::isfinite(N.x) || !std::isfinite(N.y) || !std::isfinite(N.z) ||
        !std::isfinite(albedo.x) || !std::isfinite(albedo.y) ||
        !std::isfinite(albedo.z) ||
        !std::isfinite(u0) || !std::isfinite(u1) || !std::isfinite(u2))
        throw std::invalid_argument("estimateDirect inputs must be finite");

    float nLen = N.length();
    if (std::fabs(nLen - 1.0f) > 1e-4f)
        throw std::invalid_argument("N must be unit length");
    if (albedo.x < 0.0f || albedo.y < 0.0f || albedo.z < 0.0f)
        throw std::invalid_argument("albedo must be non-negative");
    if (u0 < 0.0 || u0 >= 1.0 || u1 < 0.0 || u1 >= 1.0 ||
        u2 < 0.0 || u2 >= 1.0)
        throw std::invalid_argument("random numbers must lie in [0,1)");

    SurfaceLightSample sample;
    if (!lights.sample(u0, u1, u2, sample))
        return black;

    Vec3 delta = sample.position - P;
    double dist2 = double(delta.dot(delta));
    if (!(dist2 > 0.0))
        return black;
    double dist = std::sqrt(dist2);

    Vec3 wi(delta.x / float(dist), delta.y / float(dist), delta.z / float(dist));
    double cosReceiver = double(N.dot(wi));
    double cosLight = double(sample.normal.dot(Vec3(-wi.x, -wi.y, -wi.z)));
    if (cosReceiver <= 0.0 || cosLight <= 0.0)
        return black;

    const VoxelOctree *tree = lights.tree();
    OctreeReader reader(tree->octreeData(), tree->octreeSize());
    uint64 lightSlot = lights.faceLeafSlot(sample.faceId);

    // Resolve the leaf containing the receiver point (if any) so contact at
    // the receiving surface is not mistaken for an occluder.
    const double h = VoxelSurfaceLights::WorldScale;
    int32 receiverCell[3];
    for (int a = 0; a < 3; ++a) {
        double q = (double(P.a[a]) - 1.0) / h;
        receiverCell[a] = int32(std::floor(q));
    }
    uint64 receiverSlot = 0;
    bool receiverKnown = reader.leafSlotAt(receiverCell[0], receiverCell[1],
                                            receiverCell[2], receiverSlot, nullptr);

    if (!unobstructed(reader, P, sample.position, lightSlot, receiverSlot, receiverKnown))
        return black;

    double weight = (cosReceiver * cosLight) / (dist2 * sample.pdfArea * kPi);
    return Vec3(float(sample.radiance.x * albedo.x * weight),
                float(sample.radiance.y * albedo.y * weight),
                float(sample.radiance.z * albedo.z * weight));
}
