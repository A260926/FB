/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2012-2018 DragonBones team and other contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "CpuClippedMesh.h"

#include <cmath>
#include <cstddef>

#include "Armature.h"
#include "Slot.h"
#include "Bone.h"
#include "DeformVertices.h"
#include "../geom/Transform.h"
#include "../model/DragonBonesData.h"
#include "../model/DisplayData.h"

DRAGONBONES_NAMESPACE_BEGIN

namespace
{
    const float EPS_DIST = 1e-6f;
    const float EPS_AREA = EPS_DIST * EPS_DIST;
    const float EPS_EDGE = 1e-12f;

    struct Vec2
    {
        float x;
        float y;
    };

    struct ClipVertex
    {
        float x;
        float y;
        float u;
        float v;
    };

    inline bool isFinite(float value)
    {
        return std::isfinite(value);
    }

    inline float crossVec(const Vec2& a, const Vec2& b)
    {
        return a.x * b.y - a.y * b.x;
    }

    inline Vec2 transformPoint(const Matrix& matrix, float x, float y)
    {
        return Vec2{
            matrix.a * x + matrix.c * y + matrix.tx,
            matrix.b * x + matrix.d * y + matrix.ty
        };
    }

    /**
     * Evaluates a slot's current armature space matrix without touching the
     * slot's transform cache or any dirty flag. Empty mask slots never run
     * Slot::update() (they have no display), so their cached matrix can be
     * stale; rebuilding it from origin/offset and the parent bone matches
     * Slot::_updateGlobalTransformMatrix exactly.
     */
    Matrix resolveSlotMatrix(Slot& slot)
    {
        Matrix local;
        if (slot.origin != nullptr)
        {
            auto transform = *slot.origin;
            transform.add(slot.offset);
            transform.toMatrix(local);
        }
        else
        {
            slot.offset.toMatrix(local);
        }

        const auto parent = slot.getParent();
        if (parent != nullptr)
        {
            local.concat(parent->globalTransformMatrix);
        }

        return local;
    }

    /**
     * A valid mask polygon has at least three finite, positively area convex
     * points. Collinear points are tolerated; the winding direction may be
     * either clockwise or counter clockwise.
     */
    bool validateConvexPolygon(const std::vector<CpuMaskPoint>& polygon)
    {
        const auto count = polygon.size();
        if (count < 3)
        {
            return false;
        }

        float direction = 0.0f;
        float area2 = 0.0f;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& p = polygon[i];
            const auto& q = polygon[(i + 1) % count];
            const auto& r = polygon[(i + 2) % count];
            if (!isFinite(p.x) || !isFinite(p.y))
            {
                return false;
            }

            const Vec2 edgeA{ q.x - p.x, q.y - p.y };
            const Vec2 edgeB{ r.x - q.x, r.y - q.y };
            const auto turn = crossVec(edgeA, edgeB);
            area2 += p.x * q.y - q.x * p.y;

            if (std::fabs(turn) <= EPS_EDGE)
            {
                continue; // Collinear edge.
            }

            if (direction == 0.0f)
            {
                direction = turn;
            }
            else if (direction * turn < 0.0f)
            {
                return false; // Turns in both directions: not convex.
            }
        }

        return std::fabs(area2) * 0.5f > EPS_AREA;
    }

    /**
     * Sutherland-Hodgman clipping of a (convex) polygon against one convex
     * mask polygon. Winding order is preserved and UVs follow every new
     * intersection point linearly.
     */
    std::vector<ClipVertex> clipAgainstPolygon(const std::vector<ClipVertex>& input, const std::vector<Vec2>& mask)
    {
        auto output = input;
        const auto edgeCount = mask.size();
        // Signed area decides which side of an edge is the interior, so a
        // reflected mask keeps clipping the inside instead of flipping outside.
        float area2 = 0.0f;
        for (std::size_t i = 0; i < edgeCount; ++i)
        {
            const auto& a = mask[i];
            const auto& b = mask[(i + 1) % edgeCount];
            area2 += a.x * b.y - b.x * a.y;
        }

        const auto orientation = area2 > 0.0f ? 1.0f : -1.0f;

        for (std::size_t edgeIndex = 0; edgeIndex < edgeCount && !output.empty(); ++edgeIndex)
        {
            const auto edgeA = mask[edgeIndex];
            const auto edgeB = mask[(edgeIndex + 1) % edgeCount];
            const Vec2 edge{ edgeB.x - edgeA.x, edgeB.y - edgeA.y };
            const auto edgeLength = std::sqrt(edge.x * edge.x + edge.y * edge.y);
            if (edgeLength <= EPS_EDGE)
            {
                continue; // Duplicated polygon point, no usable half plane.
            }

            auto inputPolygon = output;
            output.clear();
            const auto vertexCount = inputPolygon.size();

            auto signedDistance = [&](const ClipVertex& vertex) -> float {
                const Vec2 relative{ vertex.x - edgeA.x, vertex.y - edgeA.y };
                return orientation * crossVec(edge, relative) / edgeLength;
            };

            for (std::size_t i = 0; i < vertexCount; ++i)
            {
                const auto& current = inputPolygon[i];
                const auto& next = inputPolygon[(i + 1) % vertexCount];
                const auto currentDistance = signedDistance(current);
                const auto nextDistance = signedDistance(next);
                const auto currentInside = currentDistance >= -EPS_DIST;
                const auto nextInside = nextDistance >= -EPS_DIST;

                if (currentInside != nextInside)
                {
                    // Unnormalized cross values share the edge length factor,
                    // so the ratio is still the segment intersection ratio.
                    const Vec2 currentRelative{ current.x - edgeA.x, current.y - edgeA.y };
                    const Vec2 nextRelative{ next.x - edgeA.x, next.y - edgeA.y };
                    const auto currentSide = orientation * crossVec(edge, currentRelative);
                    const auto nextSide = orientation * crossVec(edge, nextRelative);
                    const auto denominator = currentSide - nextSide;
                    const auto t = std::fabs(denominator) <= EPS_EDGE ? 0.0f : currentSide / denominator;

                    ClipVertex intersection;
                    intersection.x = current.x + (next.x - current.x) * t;
                    intersection.y = current.y + (next.y - current.y) * t;
                    intersection.u = current.u + (next.u - current.u) * t;
                    intersection.v = current.v + (next.v - current.v) * t;
                    output.push_back(intersection);
                }

                if (nextInside)
                {
                    output.push_back(next);
                }
            }
        }

        return output;
    }

    /**
     * Removes consecutive points that collapsed onto each other while
     * crossing a boundary, so fan triangulation cannot emit zero area faces.
     */
    std::vector<ClipVertex> compactPolygon(std::vector<ClipVertex> polygon)
    {
        if (polygon.size() < 2)
        {
            return polygon;
        }

        std::vector<ClipVertex> result;
        result.push_back(polygon.front());
        for (std::size_t i = 1; i < polygon.size(); ++i)
        {
            const auto& last = result.back();
            const auto& point = polygon[i];
            if (std::fabs(point.x - last.x) > EPS_DIST || std::fabs(point.y - last.y) > EPS_DIST)
            {
                result.push_back(point);
            }
        }

        if (result.size() > 1)
        {
            const auto& first = result.front();
            const auto& last = result.back();
            if (std::fabs(first.x - last.x) <= EPS_DIST && std::fabs(first.y - last.y) <= EPS_DIST)
            {
                result.pop_back();
            }
        }

        return result;
    }

    /**
     * Reads the current frame mesh of a slot and evaluates every vertex in
     * armature space. Static meshes are original vertices plus FFD passed
     * through the slot matrix, skinned meshes are weighted bone poses plus
     * per influence FFD and already land in armature space.
     * Returns false only on malformed mesh data.
     */
    bool buildDeformedVertices(Slot& slot, float scale, std::vector<Vec2>& positions, std::vector<Vec2>& uvs)
    {
        const auto deformVertices = slot._deformVertices;
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
        const auto offset = verticesData->offset;
        const auto vertexCount = (std::size_t)intArray[offset + (unsigned)BinaryOffset::MeshVertexCount];
        const auto triangleCount = (std::size_t)intArray[offset + (unsigned)BinaryOffset::MeshTriangleCount];
        if (vertexCount < 3 || triangleCount == 0)
        {
            return false;
        }

        int vertexOffset = intArray[offset + (unsigned)BinaryOffset::MeshFloatOffset];
        if (vertexOffset < 0)
        {
            vertexOffset += 65536;
        }

        const auto uvOffset = (std::size_t)vertexOffset + vertexCount * 2;
        const auto& deform = deformVertices->vertices;
        const auto hasFFD = !deform.empty();

        positions.clear();
        uvs.clear();
        positions.resize(vertexCount);
        uvs.resize(vertexCount);

        for (std::size_t i = 0; i < vertexCount; ++i)
        {
            uvs[i] = Vec2{ floatArray[uvOffset + i * 2], floatArray[uvOffset + i * 2 + 1] };
            if (!isFinite(uvs[i].x) || !isFinite(uvs[i].y))
            {
                return false;
            }
        }

        const auto weightData = verticesData->weight;
        if (weightData == nullptr)
        {
            // Static mesh: original vertex plus FFD, then slot global matrix.
            const auto matrix = resolveSlotMatrix(slot);
            for (std::size_t i = 0; i < vertexCount; ++i)
            {
                auto localX = floatArray[vertexOffset + i * 2] * scale;
                auto localY = floatArray[vertexOffset + i * 2 + 1] * scale;
                if (hasFFD)
                {
                    localX += deform[i * 2];
                    localY += deform[i * 2 + 1];
                }

                if (!isFinite(localX) || !isFinite(localY))
                {
                    return false;
                }

                positions[i] = transformPoint(matrix, localX, localY);
            }
        }
        else
        {
            // Skinned mesh: every bone influence carries its own FFD entry and
            // contributes through the current bone matrix. The weighted result
            // is already in armature space, the slot matrix must not be applied.
            int weightFloatOffset = intArray[weightData->offset + (unsigned)BinaryOffset::WeigthFloatOffset];
            if (weightFloatOffset < 0)
            {
                weightFloatOffset += 65536;
            }

            const auto& bones = deformVertices->bones;
            std::size_t influenceIndex = 0;
            std::size_t floatIndex = (std::size_t)weightFloatOffset;
            std::size_t boneHeaderIndex =
                weightData->offset + (unsigned)BinaryOffset::WeigthBoneIndices + bones.size();

            for (std::size_t i = 0; i < vertexCount; ++i)
            {
                const auto influenceCount = (std::size_t)intArray[boneHeaderIndex++];
                auto xGlobal = 0.0f;
                auto yGlobal = 0.0f;

                for (std::size_t j = 0; j < influenceCount; ++j)
                {
                    const auto boneIndex = (std::size_t)intArray[boneHeaderIndex++];
                    const auto weight = floatArray[floatIndex++];
                    auto localX = floatArray[floatIndex++] * scale;
                    auto localY = floatArray[floatIndex++] * scale;
                    if (hasFFD)
                    {
                        localX += deform[influenceIndex * 2];
                        localY += deform[influenceIndex * 2 + 1];
                    }
                    ++influenceIndex;

                    if (boneIndex < bones.size() && bones[boneIndex] != nullptr)
                    {
                        const auto& matrix = bones[boneIndex]->globalTransformMatrix;
                        xGlobal += (matrix.a * localX + matrix.c * localY + matrix.tx) * weight;
                        yGlobal += (matrix.b * localX + matrix.d * localY + matrix.ty) * weight;
                    }
                }

                if (!isFinite(xGlobal) || !isFinite(yGlobal))
                {
                    return false;
                }

                positions[i] = Vec2{ xGlobal, yGlobal };
            }
        }

        return true;
    }

    struct RangeInfo
    {
        const CpuMaskRange* range;
        std::size_t maskIndex;
        std::size_t endIndex;
        Slot* maskSlot;
        bool empty;
        std::vector<Vec2> worldPolygon;
    };
} // namespace

bool buildClippedMesh(
    Armature& armature,
    const std::vector<CpuMaskRange>& ranges,
    std::vector<CpuSlotMesh>& output
)
{
    output.clear();

    const auto& slots = armature.getSlots();
    const auto slotCount = slots.size();

    std::map<std::string, std::size_t> slotIndices;
    for (std::size_t i = 0; i < slotCount; ++i)
    {
        slotIndices[slots[i]->getName()] = i;
    }

    // Validate ranges against this frame's actual draw order. zOrder changes
    // simply produce a different order here, so ranges are re-judged every call.
    std::vector<RangeInfo> rangeInfos;
    rangeInfos.reserve(ranges.size());
    for (const auto& range : ranges)
    {
        if (!validateConvexPolygon(range.polygon))
        {
            output.clear();
            return false;
        }

        const auto maskIterator = slotIndices.find(range.maskSlot);
        const auto endIterator = slotIndices.find(range.endSlot);
        if (maskIterator == slotIndices.end() || endIterator == slotIndices.end())
        {
            output.clear();
            return false;
        }

        RangeInfo info;
        info.range = &range;
        info.maskIndex = maskIterator->second;
        info.endIndex = endIterator->second;
        info.maskSlot = slots[info.maskIndex];
        info.empty = false;
        if (info.maskIndex >= info.endIndex)
        {
            output.clear();
            return false;
        }

        rangeInfos.push_back(info);
    }

    // Ranges may nest or be separate, but may not cross.
    for (std::size_t i = 0; i < rangeInfos.size(); ++i)
    {
        for (std::size_t j = i + 1; j < rangeInfos.size(); ++j)
        {
            const auto& a = rangeInfos[i];
            const auto& b = rangeInfos[j];
            if (a.maskIndex == b.maskIndex)
            {
                output.clear();
                return false; // One slot cannot open two ranges.
            }

            const auto disjoint = a.endIndex <= b.maskIndex || b.endIndex <= a.maskIndex;
            const auto nested =
                (a.maskIndex <= b.maskIndex && b.endIndex <= a.endIndex) ||
                (b.maskIndex <= a.maskIndex && a.endIndex <= b.endIndex);
            if (!disjoint && !nested)
            {
                output.clear();
                return false;
            }
        }
    }

    // Resolve mask geometry in armature space. A hidden mask makes its whole
    // range empty; a hidden end slot still terminates the range normally.
    for (auto& info : rangeInfos)
    {
        if (!info.maskSlot->getVisible())
        {
            info.empty = true;
            continue;
        }

        const auto matrix = resolveSlotMatrix(*info.maskSlot);
        info.worldPolygon.reserve(info.range->polygon.size());
        for (const auto& point : info.range->polygon)
        {
            const auto world = transformPoint(matrix, point.x, point.y);
            if (!isFinite(world.x) || !isFinite(world.y))
            {
                output.clear();
                return false;
            }

            info.worldPolygon.push_back(world);
        }
    }

    const auto scale = armature._armatureData->scale;

    auto isMaskSlot = [&rangeInfos](std::size_t slotOrder) {
        for (const auto& info : rangeInfos)
        {
            if (info.maskIndex == slotOrder)
            {
                return true;
            }
        }
        return false;
    };

    // Active unsupported displays are an explicit error, never silently dropped.
    // Hidden slots do not participate in the frame. A mask marker has to be an
    // empty slot or a BoundingBox; a render slot may only show a Mesh (a stray
    // BoundingBox simply has no renderable geometry).
    for (std::size_t slotOrder = 0; slotOrder < slotCount; ++slotOrder)
    {
        const auto slot = slots[slotOrder];
        if (!slot->getVisible())
        {
            continue;
        }

        const auto hasDisplay =
            slot->getDisplayIndex() >= 0 && slot->getDisplay() != nullptr && slot->_displayData != nullptr;
        if (!hasDisplay)
        {
            continue; // Empty slot, legal as a mask marker.
        }

        const auto type = slot->_displayData->type;
        if (isMaskSlot(slotOrder))
        {
            if (type != DisplayType::BoundingBox)
            {
                output.clear();
                return false;
            }
        }
        else if (type != DisplayType::Mesh && type != DisplayType::BoundingBox)
        {
            output.clear();
            return false;
        }
    }

    for (std::size_t slotOrder = 0; slotOrder < slotCount; ++slotOrder)
    {
        const auto slot = slots[slotOrder];

        // A mask slot never emits geometry itself.
        bool opensMask = false;
        for (const auto& info : rangeInfos)
        {
            if (info.maskIndex == slotOrder)
            {
                opensMask = true;
                break;
            }
        }
        if (opensMask)
        {
            continue;
        }

        if (!slot->getVisible())
        {
            continue; // Hidden slots are not drawn, though ranges still use them.
        }

        if (slot->getDisplayIndex() < 0 || slot->getDisplay() == nullptr || slot->_displayData == nullptr)
        {
            continue;
        }

        if (slot->_displayData->type != DisplayType::Mesh)
        {
            continue; // Bounding box helpers carry no renderable geometry.
        }

        // Collect the masks that cover this draw-order position. A range runs
        // after its mask slot and up to and including its end slot.
        std::vector<const RangeInfo*> activeRanges;
        auto coveredByEmptyRange = false;
        for (const auto& info : rangeInfos)
        {
            if (info.maskIndex < slotOrder && slotOrder <= info.endIndex)
            {
                if (info.empty)
                {
                    coveredByEmptyRange = true;
                    break;
                }

                activeRanges.push_back(&info);
            }
        }
        if (coveredByEmptyRange)
        {
            continue;
        }

        const auto meshDisplay = static_cast<MeshDisplayData*>(slot->_displayData);
        std::vector<Vec2> positions;
        std::vector<Vec2> uvs;
        if (!buildDeformedVertices(*slot, scale, positions, uvs))
        {
            output.clear();
            return false;
        }

        const auto verticesData = slot->_deformVertices->verticesData;
        const auto intArray = verticesData->data->intArray;
        const auto triangleCount = (std::size_t)intArray[verticesData->offset + (unsigned)BinaryOffset::MeshTriangleCount];
        const auto indexOffset = verticesData->offset + (unsigned)BinaryOffset::MeshVertexIndices;

        CpuSlotMesh package;
        package.slotName = slot->getName();
        package.displayName = meshDisplay->name;
        package.materialPath = meshDisplay->path;
        package.blendMode = slot->_blendMode;
        package.colorTransform = slot->_colorTransform;

        for (std::size_t triangle = 0; triangle < triangleCount; ++triangle)
        {
            const auto i0 = (std::size_t)intArray[indexOffset + triangle * 3];
            const auto i1 = (std::size_t)intArray[indexOffset + triangle * 3 + 1];
            const auto i2 = (std::size_t)intArray[indexOffset + triangle * 3 + 2];
            if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size())
            {
                output.clear();
                return false;
            }

            std::vector<ClipVertex> polygon;
            polygon.reserve(3);
            const std::size_t sourceIndices[3] = { i0, i1, i2 };
            for (const auto index : sourceIndices)
            {
                ClipVertex vertex;
                vertex.x = positions[index].x;
                vertex.y = positions[index].y;
                vertex.u = uvs[index].x;
                vertex.v = uvs[index].y;
                polygon.push_back(vertex);
            }

            // Clip the deformed triangle; never the bind-pose shape.
            for (const auto info : activeRanges)
            {
                polygon = clipAgainstPolygon(polygon, info->worldPolygon);
                if (polygon.empty())
                {
                    break;
                }
            }

            if (polygon.size() < 3)
            {
                continue;
            }

            polygon = compactPolygon(polygon);
            if (polygon.size() < 3)
            {
                continue;
            }

            // The clipped polygon stays convex and keeps the source winding,
            // so a fan triangulation reproduces the original face order.
            for (std::size_t i = 1; i + 1 < polygon.size(); ++i)
            {
                const ClipVertex corners[3] = { polygon[0], polygon[i], polygon[i + 1] };
                const Vec2 edgeA{ corners[1].x - corners[0].x, corners[1].y - corners[0].y };
                const Vec2 edgeB{ corners[2].x - corners[0].x, corners[2].y - corners[0].y };
                if (std::fabs(crossVec(edgeA, edgeB)) * 0.5f <= EPS_AREA)
                {
                    continue; // No zero area output faces.
                }

                CpuMeshTriangle outputTriangle;
                for (std::size_t corner = 0; corner < 3; ++corner)
                {
                    outputTriangle.vertices[corner].x = corners[corner].x;
                    outputTriangle.vertices[corner].y = corners[corner].y;
                    outputTriangle.vertices[corner].u = corners[corner].u;
                    outputTriangle.vertices[corner].v = corners[corner].v;
                }
                outputTriangle.sourceTriangle = (unsigned)triangle;
                package.triangles.push_back(outputTriangle);
            }
        }

        // Only slots that still cover some area after clipping get a record,
        // and different slots are never merged.
        if (!package.triangles.empty())
        {
            output.push_back(std::move(package));
        }
    }

    return true;
}

DRAGONBONES_NAMESPACE_END
