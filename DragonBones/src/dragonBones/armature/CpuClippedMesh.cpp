#include "CpuClippedMesh.h"

#include "Armature.h"
#include "Slot.h"
#include "Bone.h"
#include "DeformVertices.h"
#include "../model/DisplayData.h"
#include "../model/DragonBonesData.h"
#include "../model/ArmatureData.h"

#include <cmath>

DRAGONBONES_NAMESPACE_BEGIN

namespace
{
    // Distance tolerance required by the clipping contract: points on or
    // inside a mask edge within 1e-6 are kept.
    const float kDistanceEps = 1e-6f;
    // Triangles whose signed double area is not larger than this are dropped,
    // so the output never contains zero area faces.
    const float kAreaEps = 1e-10f;

    struct ClipVertex
    {
        float x;
        float y;
        float u;
        float v;
    };

    struct HalfPlane
    {
        float px; // Point on the mask edge.
        float py;
        float nx; // Inward normal (not normalized).
        float ny;

        inline float distance(const ClipVertex& value) const
        {
            return nx * (value.x - px) + ny * (value.y - py);
        }
    };

    inline bool isFinitePair(float x, float y)
    {
        return std::isfinite(x) && std::isfinite(y);
    }

    // Resolve a slot's current armature space matrix without relying on the
    // engine update cache: empty and BoundingBox mask slots early return from
    // Slot::update() before their global matrix is refreshed, while the parent
    // bone always has a current matrix after advanceTime(). This mirrors
    // Slot::_updateGlobalTransformMatrix().
    Matrix resolveSlotMatrix(Slot* slot)
    {
        Matrix localMatrix;
        if (slot->origin != nullptr)
        {
            auto local = *slot->origin;
            local.add(slot->offset);
            local.toMatrix(localMatrix);
        }
        else
        {
            slot->offset.toMatrix(localMatrix);
        }

        auto matrix = localMatrix;
        const auto parent = slot->getParent();
        if (parent != nullptr)
        {
            matrix.concat(parent->globalTransformMatrix);
        }
        return matrix;
    }

    bool validatePolygon(const CpuMaskPolygon& polygon)
    {
        const auto& points = polygon.points;
        if (points.size() < 6 || points.size() % 2 != 0) // At least three x/y pairs.
        {
            return false;
        }

        for (std::size_t i = 0; i < points.size(); ++i)
        {
            if (!std::isfinite(points[i]))
            {
                return false;
            }
        }

        // Convexity: every edge turn must keep the same winding, collinear
        // turns are allowed but a fully degenerate (zero area) polygon fails.
        const auto count = points.size() / 2;
        int sign = 0;
        float area = 0.0f;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto j = (i + 1) % count;
            const auto k = (i + 2) % count;
            const auto ax = points[j * 2] - points[i * 2];
            const auto ay = points[j * 2 + 1] - points[i * 2 + 1];
            const auto bx = points[k * 2] - points[j * 2];
            const auto by = points[k * 2 + 1] - points[j * 2 + 1];
            const auto cross = ax * by - ay * bx;
            area += points[i * 2] * points[j * 2 + 1] - points[j * 2] * points[i * 2 + 1];

            if (cross > kAreaEps)
            {
                if (sign < 0)
                {
                    return false;
                }
                sign = 1;
            }
            else if (cross < -kAreaEps)
            {
                if (sign > 0)
                {
                    return false;
                }
                sign = -1;
            }
        }

        return std::fabs(area) > kAreaEps;
    }

    // Turn a mask slot's local convex polygon into armature space half planes.
    // The normal of every edge is oriented towards the polygon centroid, so a
    // reflected globalTransformMatrix keeps "inside" meaning inside instead of
    // flipping the mask into its complement.
    bool buildPlanes(Slot* maskSlot, const CpuMaskPolygon& polygon, std::vector<HalfPlane>& planes)
    {
        const auto matrix = resolveSlotMatrix(maskSlot);

        const auto count = polygon.points.size() / 2;
        std::vector<ClipVertex> world;
        world.reserve(count);
        auto centroidX = 0.0f;
        auto centroidY = 0.0f;

        for (std::size_t i = 0; i < count; ++i)
        {
            const auto lx = polygon.points[i * 2];
            const auto ly = polygon.points[i * 2 + 1];
            ClipVertex vertex;
            vertex.x = matrix.a * lx + matrix.c * ly + matrix.tx;
            vertex.y = matrix.b * lx + matrix.d * ly + matrix.ty;
            if (!isFinitePair(vertex.x, vertex.y))
            {
                return false;
            }
            centroidX += vertex.x;
            centroidY += vertex.y;
            world.push_back(vertex);
        }

        centroidX /= static_cast<float>(count);
        centroidY /= static_cast<float>(count);
        planes.clear();
        planes.reserve(count);

        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& p0 = world[i];
            const auto& p1 = world[(i + 1) % count];
            HalfPlane plane;
            plane.px = p0.x;
            plane.py = p0.y;
            plane.nx = -(p1.y - p0.y);
            plane.ny = p1.x - p0.x;
            // Point the normal at the centroid: this side is the interior.
            if (plane.nx * (centroidX - p0.x) + plane.ny * (centroidY - p0.y) < 0.0f)
            {
                plane.nx = -plane.nx;
                plane.ny = -plane.ny;
            }
            // Normalize so the 1e-6 tolerance is a real Euclidean distance and
            // classification does not depend on the edge length.
            const auto normalLength = std::sqrt(plane.nx * plane.nx + plane.ny * plane.ny);
            if (!(normalLength > 0.0f) || !std::isfinite(normalLength))
            {
                return false;
            }
            plane.nx /= normalLength;
            plane.ny /= normalLength;
            planes.push_back(plane);
        }

        return true;
    }

    inline ClipVertex lerpVertex(const ClipVertex& a, const ClipVertex& b, float t)
    {
        ClipVertex result;
        result.x = a.x + (b.x - a.x) * t;
        result.y = a.y + (b.y - a.y) * t;
        result.u = a.u + (b.u - a.u) * t;
        result.v = a.v + (b.v - a.v) * t;
        return result;
    }

    // Sutherland-Hodgman clipping of one convex input polygon (a triangle)
    // against one half plane. Winding order is preserved, UVs are interpolated
    // linearly at every intersection.
    std::vector<ClipVertex> clipWithPlane(const std::vector<ClipVertex>& input, const HalfPlane& plane)
    {
        std::vector<ClipVertex> output;
        if (input.empty())
        {
            return output;
        }
        output.reserve(input.size() + 1);

        auto s = input.back();
        auto dS = plane.distance(s);

        for (const auto& e : input)
        {
            const auto dE = plane.distance(e);
            const auto insideS = dS >= -kDistanceEps;
            const auto insideE = dE >= -kDistanceEps;

            if (insideE)
            {
                if (!insideS)
                {
                    auto t = dS / (dS - dE);
                    if (t < 0.0f) t = 0.0f;
                    else if (t > 1.0f) t = 1.0f;
                    output.push_back(lerpVertex(s, e, t));
                }
                output.push_back(e);
            }
            else if (insideS)
            {
                auto t = dS / (dS - dE);
                if (t < 0.0f) t = 0.0f;
                else if (t > 1.0f) t = 1.0f;
                output.push_back(lerpVertex(s, e, t));
            }

            s = e;
            dS = dE;
        }

        return output;
    }

    inline double triangleArea2(const ClipVertex& a, const ClipVertex& b, const ClipVertex& c)
    {
        return static_cast<double>(b.x - a.x) * (c.y - a.y) - static_cast<double>(b.y - a.y) * (c.x - a.x);
    }

    // Read the current deformed mesh of one slot. Positions are armature
    // space; weighted meshes are already in armature space, so they do not get
    // the slot matrix applied.
    bool buildDeformedVertices(Slot* slot, std::vector<ClipVertex>& vertices, std::vector<int>& indices, unsigned& triangleCount)
    {
        const auto deformVertices = slot->_deformVertices;
        if (deformVertices == nullptr || deformVertices->verticesData == nullptr)
        {
            return false;
        }

        const auto verticesData = deformVertices->verticesData;
        const auto data = verticesData->data;
        if (data == nullptr || data->intArray == nullptr || data->floatArray == nullptr)
        {
            return false;
        }

        const auto intArray = data->intArray;
        const auto floatArray = data->floatArray;
        const auto vertexCount = static_cast<unsigned>(intArray[verticesData->offset + static_cast<unsigned>(BinaryOffset::MeshVertexCount)]);
        triangleCount = static_cast<unsigned>(intArray[verticesData->offset + static_cast<unsigned>(BinaryOffset::MeshTriangleCount)]);
        if (vertexCount == 0 || triangleCount == 0)
        {
            return false;
        }

        auto vertexOffset = intArray[verticesData->offset + static_cast<unsigned>(BinaryOffset::MeshFloatOffset)];
        if (vertexOffset < 0)
        {
            vertexOffset += 65536;
        }
        const auto uvOffset = static_cast<unsigned>(vertexOffset) + vertexCount * 2;
        const auto scale = slot->getArmature()->getArmatureData()->scale;
        const auto& deform = deformVertices->vertices;

        vertices.clear();
        vertices.resize(vertexCount);

        const auto weightData = verticesData->weight;
        if (weightData != nullptr) // Weighted mesh: bone matrices, result is already in armature space.
        {
            auto weightFloatOffset = intArray[weightData->offset + static_cast<unsigned>(BinaryOffset::WeigthFloatOffset)];
            if (weightFloatOffset < 0)
            {
                weightFloatOffset += 65536;
            }

            const auto& bones = deformVertices->bones;
            const auto hasFFD = !deform.empty();
            std::size_t iB = weightData->offset + static_cast<unsigned>(BinaryOffset::WeigthBoneIndices) + bones.size();
            std::size_t iV = static_cast<unsigned>(weightFloatOffset);
            std::size_t iF = 0;

            for (unsigned i = 0; i < vertexCount; ++i)
            {
                const auto boneCount = static_cast<unsigned>(intArray[iB++]);
                auto xG = 0.0f;
                auto yG = 0.0f;

                for (unsigned j = 0; j < boneCount; ++j)
                {
                    const auto boneIndex = static_cast<unsigned>(intArray[iB++]);
                    const auto bone = boneIndex < bones.size() ? bones[boneIndex] : nullptr;
                    const auto weight = floatArray[iV++];
                    auto xL = floatArray[iV++] * scale;
                    auto yL = floatArray[iV++] * scale;

                    if (hasFFD && iF + 1 < deform.size())
                    {
                        xL += deform[iF++];
                        yL += deform[iF++];
                    }

                    if (bone != nullptr)
                    {
                        const auto& matrix = bone->globalTransformMatrix;
                        xG += (matrix.a * xL + matrix.c * yL + matrix.tx) * weight;
                        yG += (matrix.b * xL + matrix.d * yL + matrix.ty) * weight;
                    }
                }

                const auto uvIndex = uvOffset + i * 2;
                vertices[i].x = xG;
                vertices[i].y = yG;
                vertices[i].u = floatArray[uvIndex];
                vertices[i].v = floatArray[uvIndex + 1];
                if (!isFinitePair(vertices[i].x, vertices[i].y) || !isFinitePair(vertices[i].u, vertices[i].v))
                {
                    return false;
                }
            }
        }
        else // Normal mesh: bind pose vertices plus FFD, then the slot matrix.
        {
            const auto matrix = resolveSlotMatrix(slot);
            const auto hasFFD = deform.size() >= vertexCount * 2;

            for (unsigned i = 0; i < vertexCount; ++i)
            {
                auto xL = floatArray[vertexOffset + i * 2] * scale;
                auto yL = floatArray[vertexOffset + i * 2 + 1] * scale;
                if (hasFFD)
                {
                    xL += deform[i * 2];
                    yL += deform[i * 2 + 1];
                }

                vertices[i].x = matrix.a * xL + matrix.c * yL + matrix.tx;
                vertices[i].y = matrix.b * xL + matrix.d * yL + matrix.ty;
                vertices[i].u = floatArray[uvOffset + i * 2];
                vertices[i].v = floatArray[uvOffset + i * 2 + 1];
                if (!isFinitePair(vertices[i].x, vertices[i].y) || !isFinitePair(vertices[i].u, vertices[i].v))
                {
                    return false;
                }
            }
        }

        indices.resize(triangleCount * 3);
        for (unsigned i = 0; i < triangleCount * 3; ++i)
        {
            const auto index = intArray[verticesData->offset + static_cast<unsigned>(BinaryOffset::MeshVertexIndices) + i];
            if (index < 0 || static_cast<unsigned>(index) >= vertexCount)
            {
                return false;
            }
            indices[i] = index;
        }

        return true;
    }

    struct RangeInfo
    {
        std::size_t rangeIndex;
        std::size_t maskIndex;
        std::size_t endIndex;
        Slot* maskSlot;
        bool maskVisible;
        std::vector<HalfPlane> planes; // Empty while the mask is hidden: such a range clips everything.
    };
}

bool buildClippedMesh(
    Armature& armature,
    const std::vector<CpuMaskRange>& ranges,
    std::vector<CpuSlotMesh>& out
)
{
    out.clear();

    const auto& slots = armature.getSlots();
    const auto slotCount = slots.size();

    // Resolve and validate every range against this frame's actual draw order.
    std::vector<RangeInfo> rangeInfos;
    rangeInfos.reserve(ranges.size());
    for (const auto& range : ranges)
    {
        if (!validatePolygon(range.polygon))
        {
            return false;
        }

        auto maskSlot = armature.getSlot(range.maskSlot);
        auto endSlot = armature.getSlot(range.endSlot);
        if (maskSlot == nullptr || endSlot == nullptr || maskSlot == endSlot)
        {
            return false;
        }

        int maskIndex = -1;
        int endIndex = -1;
        for (std::size_t i = 0; i < slotCount; ++i)
        {
            if (slots[i] == maskSlot)
            {
                maskIndex = static_cast<int>(i);
            }
            if (slots[i] == endSlot)
            {
                endIndex = static_cast<int>(i);
            }
        }
        if (maskIndex < 0 || endIndex < 0 || maskIndex >= endIndex)
        {
            return false;
        }

        RangeInfo info;
        info.rangeIndex = rangeInfos.size();
        info.maskIndex = static_cast<std::size_t>(maskIndex);
        info.endIndex = static_cast<std::size_t>(endIndex);
        info.maskSlot = maskSlot;
        info.maskVisible = true;
        rangeInfos.push_back(std::move(info));
    }

    // Reject crossing intervals: every pair must be disjoint or nested.
    for (std::size_t i = 0; i < rangeInfos.size(); ++i)
    {
        for (std::size_t j = i + 1; j < rangeInfos.size(); ++j)
        {
            const auto& a = rangeInfos[i];
            const auto& b = rangeInfos[j];
            const auto disjoint = a.endIndex <= b.maskIndex || b.endIndex <= a.maskIndex;
            const auto aContainsB = a.maskIndex <= b.maskIndex && b.endIndex <= a.endIndex;
            const auto bContainsA = b.maskIndex <= a.maskIndex && a.endIndex <= b.endIndex;
            if (!disjoint && !aContainsB && !bContainsA)
            {
                return false;
            }
        }
    }

    // Precompute armature space clip planes. A hidden mask makes its range
    // empty: geometry covered by that range is clipped away completely.
    for (auto& info : rangeInfos)
    {
        info.maskVisible = info.maskSlot->getVisible();
        info.planes.clear();
        if (info.maskVisible)
        {
            if (!buildPlanes(info.maskSlot, ranges[info.rangeIndex].polygon, info.planes))
            {
                return false;
            }
        }
    }

    std::vector<ClipVertex> meshVertices;
    std::vector<int> meshIndices;
    std::vector<ClipVertex> polygon;
    std::vector<ClipVertex> clipped;
    std::vector<CpuSlotMesh> result;

    for (std::size_t slotOrder = 0; slotOrder < slotCount; ++slotOrder)
    {
        auto slot = slots[slotOrder];

        // Mask slots open their range after themselves and never emit geometry.
        bool isMaskSlot = false;
        for (const auto& info : rangeInfos)
        {
            if (info.maskIndex == slotOrder)
            {
                isMaskSlot = true;
                break;
            }
        }
        if (isMaskSlot)
        {
            continue;
        }

        // Active masks at this draw position are the intersection of all
        // ranges currently open. A hidden open mask empties its range, so the
        // covered slot produces no geometry at all.
        bool inRange = false;
        bool clippedAway = false;
        std::vector<const HalfPlane*> activePlanes;
        for (const auto& info : rangeInfos)
        {
            if (info.maskIndex < slotOrder && slotOrder <= info.endIndex)
            {
                inRange = true;
                if (!info.maskVisible)
                {
                    clippedAway = true;
                    break;
                }
                for (const auto& plane : info.planes)
                {
                    activePlanes.push_back(&plane);
                }
            }
        }

        if (!slot->getVisible() || slot->getDisplay() == nullptr)
        {
            continue;
        }

        // Only slots covered by an active range are required to be meshes:
        // Image, child Armature and Path displays outside every range are
        // simply not part of a clipped mesh, while an active one inside a
        // range cannot be expressed as mesh geometry and is rejected.
        if (slot->getChildArmature() != nullptr)
        {
            if (inRange)
            {
                return false;
            }
            continue;
        }

        const auto displayData = slot->_displayData;
        if (displayData == nullptr || slot->getDisplay() != slot->_meshDisplay || displayData->type != DisplayType::Mesh)
        {
            if (inRange)
            {
                return false;
            }
            continue;
        }

        if (clippedAway)
        {
            continue;
        }

        unsigned triangleCount = 0;
        if (!buildDeformedVertices(slot, meshVertices, meshIndices, triangleCount))
        {
            return false;
        }

        CpuSlotMesh package;
        package.slotName = slot->getName();
        package.displayName = displayData->name;
        package.materialPath = displayData->path;
        package.blendMode = slot->_blendMode;
        package.colorTransform = slot->_colorTransform;

        for (unsigned triangle = 0; triangle < triangleCount; ++triangle)
        {
            const auto i0 = meshIndices[triangle * 3];
            const auto i1 = meshIndices[triangle * 3 + 1];
            const auto i2 = meshIndices[triangle * 3 + 2];

            polygon.clear();
            polygon.push_back(meshVertices[i0]);
            polygon.push_back(meshVertices[i1]);
            polygon.push_back(meshVertices[i2]);

            // Clip the deformed triangle, never the bind pose shape; vertices
            // outside the mask are cut instead of dropped.
            clipped = polygon;
            for (const auto plane : activePlanes)
            {
                clipped = clipWithPlane(clipped, *plane);
                if (clipped.empty())
                {
                    break;
                }
            }

            if (clipped.size() < 3)
            {
                continue;
            }

            // Fan triangulation keeps the source winding; drop degenerate faces.
            for (std::size_t i = 1; i + 1 < clipped.size(); ++i)
            {
                const auto& a = clipped[0];
                const auto& b = clipped[i];
                const auto& c = clipped[i + 1];
                if (std::fabs(triangleArea2(a, b, c)) <= kAreaEps)
                {
                    continue;
                }

                CpuMeshTriangle outputTriangle;
                outputTriangle.sourceTriangle = triangle;
                auto fill = [](CpuMeshVertex& target, const ClipVertex& source)
                {
                    target.x = source.x;
                    target.y = source.y;
                    target.u = source.u;
                    target.v = source.v;
                };
                fill(outputTriangle.vertices[0], a);
                fill(outputTriangle.vertices[1], b);
                fill(outputTriangle.vertices[2], c);
                package.triangles.push_back(outputTriangle);
            }
        }

        // Only slots that still cover area produce a record; slots never merge.
        if (!package.triangles.empty())
        {
            result.push_back(std::move(package));
        }
    }

    out = std::move(result);
    return true;
}

DRAGONBONES_NAMESPACE_END
