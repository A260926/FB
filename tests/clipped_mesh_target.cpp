// Regression tests for the device independent CPU clipped mesh builder.
// The armatures are generated as DragonBones JSON and driven through the real
// headless factory, animation, FFD and zOrder pipelines. Every expected
// position, clipping area, UV and source index is recomputed here independently.
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "headless.h"
#include "dragonBones/armature/CpuClippedMesh.h"

using namespace dragonBones;

namespace
{
    const float EPS = 1e-4f;

    struct P2
    {
        double x;
        double y;
    };

    P2 operator+(const P2& a, const P2& b) { return { a.x + b.x, a.y + b.y }; }
    P2 operator-(const P2& a, const P2& b) { return { a.x - b.x, a.y - b.y }; }
    P2 operator*(const P2& a, double s) { return { a.x * s, a.y * s }; }

    double cross(const P2& a, const P2& b)
    {
        return a.x * b.y - a.y * b.x;
    }

    double polygonArea(const std::vector<P2>& polygon)
    {
        if (polygon.size() < 3)
        {
            return 0.0;
        }

        double twiceArea = 0.0;
        for (std::size_t i = 0; i < polygon.size(); ++i)
        {
            const auto& a = polygon[i];
            const auto& b = polygon[(i + 1) % polygon.size()];
            twiceArea += a.x * b.y - b.x * a.y;
        }

        return std::fabs(twiceArea) * 0.5;
    }

    // Independent Sutherland-Hodgman implementation used only as test oracle.
    std::vector<P2> referenceClip(const std::vector<P2>& subject, const std::vector<P2>& clip)
    {
        auto output = subject;
        double orientedArea = 0.0;
        for (std::size_t i = 0; i < clip.size(); ++i)
        {
            const auto a = clip[i];
            const auto b = clip[(i + 1) % clip.size()];
            orientedArea += a.x * b.y - b.x * a.y;
        }
        const auto sign = orientedArea > 0.0 ? 1.0 : -1.0;

        for (std::size_t edge = 0; edge < clip.size() && !output.empty(); ++edge)
        {
            const auto a = clip[edge];
            const auto b = clip[(edge + 1) % clip.size()];
            const auto edgeVector = b - a;
            const auto input = output;
            output.clear();

            auto side = [&](const P2& p) {
                return sign * cross(edgeVector, p - a);
            };

            for (std::size_t i = 0; i < input.size(); ++i)
            {
                const auto current = input[i];
                const auto next = input[(i + 1) % input.size()];
                const auto currentSide = side(current);
                const auto nextSide = side(next);
                if ((currentSide >= 0.0) != (nextSide >= 0.0))
                {
                    const auto t = currentSide / (currentSide - nextSide);
                    output.push_back(current + (next - current) * t);
                }
                if (nextSide >= 0.0)
                {
                    output.push_back(next);
                }
            }
        }

        return output;
    }

    bool insidePolygon(const P2& point, const std::vector<P2>& polygon, double tolerance = 1e-5)
    {
        double orientedArea = 0.0;
        for (std::size_t i = 0; i < polygon.size(); ++i)
        {
            const auto a = polygon[i];
            const auto b = polygon[(i + 1) % polygon.size()];
            orientedArea += a.x * b.y - b.x * a.y;
        }
        const auto sign = orientedArea > 0.0 ? 1.0 : -1.0;
        for (std::size_t i = 0; i < polygon.size(); ++i)
        {
            const auto a = polygon[i];
            const auto b = polygon[(i + 1) % polygon.size()];
            if (sign * cross(b - a, point - a) < -tolerance)
            {
                return false;
            }
        }
        return true;
    }

    P2 applyMatrix(const Matrix& matrix, double x, double y)
    {
        return {
            matrix.a * x + matrix.c * y + matrix.tx,
            matrix.b * x + matrix.d * y + matrix.ty
        };
    }

    // Mirror of the production read-only slot matrix resolution. Empty slots
    // never update their cached globalTransformMatrix, so the test must not
    // rely on it.
    Matrix expectedSlotMatrix(Slot& slot)
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

    void barycentric(const P2& point, const P2& a, const P2& b, const P2& c, double weights[3])
    {
        const auto total = cross(b - a, c - a);
        weights[0] = cross(b - point, c - point) / total;
        weights[1] = cross(c - point, a - point) / total;
        weights[2] = cross(a - point, b - point) / total;
    }

    // Reads the packed mesh exactly like the renderer would, then reapplies the
    // deformation math independently so the production code cannot validate
    // itself.
    std::vector<P2> expectedStaticPositions(Slot& slot)
    {
        const auto matrix = expectedSlotMatrix(slot);
        const auto data = slot._deformVertices->verticesData;
        const auto intArray = data->data->intArray;
        const auto floatArray = data->data->floatArray;
        const auto vertexCount = intArray[data->offset + (unsigned)BinaryOffset::MeshVertexCount];
        int vertexOffset = intArray[data->offset + (unsigned)BinaryOffset::MeshFloatOffset];
        if (vertexOffset < 0)
        {
            vertexOffset += 65536;
        }
        const auto scale = slot.getArmature()->_armatureData->scale;
        const auto& deform = slot._deformVertices->vertices;
        std::vector<P2> result;
        for (int i = 0; i < vertexCount; ++i)
        {
            const auto x = floatArray[vertexOffset + i * 2] * scale + deform[i * 2];
            const auto y = floatArray[vertexOffset + i * 2 + 1] * scale + deform[i * 2 + 1];
            result.push_back(applyMatrix(matrix, x, y));
        }
        return result;
    }

    std::vector<P2> expectedSkinnedPositions(Slot& slot)
    {
        const auto data = slot._deformVertices->verticesData;
        const auto weight = data->weight;
        const auto intArray = data->data->intArray;
        const auto floatArray = data->data->floatArray;
        const auto vertexCount = intArray[data->offset + (unsigned)BinaryOffset::MeshVertexCount];
        int weightFloatOffset = intArray[weight->offset + (unsigned)BinaryOffset::WeigthFloatOffset];
        if (weightFloatOffset < 0)
        {
            weightFloatOffset += 65536;
        }
        const auto scale = slot.getArmature()->_armatureData->scale;
        const auto& deform = slot._deformVertices->vertices;
        const auto& bones = slot._deformVertices->bones;

        std::vector<P2> result;
        std::size_t influence = 0;
        std::size_t floatIndex = (std::size_t)weightFloatOffset;
        std::size_t header = weight->offset + (unsigned)BinaryOffset::WeigthBoneIndices + bones.size();
        for (unsigned vertex = 0; vertex < vertexCount; ++vertex)
        {
            const auto influenceCount = intArray[header++];
            P2 accumulated{ 0.0, 0.0 };
            for (int j = 0; j < influenceCount; ++j)
            {
                const auto boneIndex = intArray[header++];
                const auto boneWeight = floatArray[floatIndex++];
                const auto x = floatArray[floatIndex++] * scale + deform[influence * 2];
                const auto y = floatArray[floatIndex++] * scale + deform[influence * 2 + 1];
                ++influence;
                const auto& matrix = bones[boneIndex]->globalTransformMatrix;
                accumulated = accumulated + applyMatrix(matrix, x, y) * boneWeight;
            }
            result.push_back(accumulated);
        }
        return result;
    }

    std::vector<P2> rawUvs(Slot& slot)
    {
        const auto data = slot._deformVertices->verticesData;
        const auto intArray = data->data->intArray;
        const auto floatArray = data->data->floatArray;
        const auto vertexCount = intArray[data->offset + (unsigned)BinaryOffset::MeshVertexCount];
        int vertexOffset = intArray[data->offset + (unsigned)BinaryOffset::MeshFloatOffset];
        if (vertexOffset < 0)
        {
            vertexOffset += 65536;
        }
        const auto uvOffset = vertexOffset + vertexCount * 2;
        std::vector<P2> result;
        for (int i = 0; i < vertexCount; ++i)
        {
            result.push_back({ floatArray[uvOffset + i * 2], floatArray[uvOffset + i * 2 + 1] });
        }
        return result;
    }

    std::vector<std::vector<unsigned>> sourceTriangles(Slot& slot)
    {
        const auto data = slot._deformVertices->verticesData;
        const auto intArray = data->data->intArray;
        const auto triangleCount = intArray[data->offset + (unsigned)BinaryOffset::MeshTriangleCount];
        const auto indexOffset = data->offset + (unsigned)BinaryOffset::MeshVertexIndices;
        std::vector<std::vector<unsigned>> result;
        for (int triangle = 0; triangle < triangleCount; ++triangle)
        {
            result.push_back({
                (unsigned)intArray[indexOffset + triangle * 3],
                (unsigned)intArray[indexOffset + triangle * 3 + 1],
                (unsigned)intArray[indexOffset + triangle * 3 + 2]
            });
        }
        return result;
    }

    std::vector<P2> worldMaskPolygon(Slot& maskSlot, const std::vector<CpuMaskPoint>& local)
    {
        const auto matrix = expectedSlotMatrix(maskSlot);
        std::vector<P2> result;
        for (const auto& point : local)
        {
            result.push_back(applyMatrix(matrix, point.x, point.y));
        }
        return result;
    }

    // Expected intersection area of one slot's deformed triangles and the
    // active mask polygons.
    double expectedClipArea(
        const std::vector<P2>& positions,
        const std::vector<std::vector<unsigned>>& triangles,
        const std::vector<std::vector<P2>>& masks
    )
    {
        double area = 0.0;
        for (const auto& triangle : triangles)
        {
            std::vector<P2> subject = {
                positions[triangle[0]],
                positions[triangle[1]],
                positions[triangle[2]]
            };
            for (const auto& mask : masks)
            {
                subject = referenceClip(subject, mask);
                if (subject.empty())
                {
                    break;
                }
            }
            area += polygonArea(subject);
        }
        return area;
    }

    double outputArea(const CpuSlotMesh& package)
    {
        double area = 0.0;
        for (const auto& triangle : package.triangles)
        {
            const P2 a{ triangle.vertices[0].x, triangle.vertices[0].y };
            const P2 b{ triangle.vertices[1].x, triangle.vertices[1].y };
            const P2 c{ triangle.vertices[2].x, triangle.vertices[2].y };
            area += std::fabs(cross(b - a, c - a)) * 0.5;
        }
        return area;
    }

    // Clipping rotates the subject polygon per edge, so vertex order is not a
    // stable identity; match by position instead.
    bool packageContainsVertex(const CpuSlotMesh& package, double x, double y)
    {
        for (const auto& triangle : package.triangles)
        {
            for (const auto& vertex : triangle.vertices)
            {
                if (std::fabs(vertex.x - x) < EPS && std::fabs(vertex.y - y) < EPS)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Flat content signature, so two builds can be compared independent of any
    // object identity.
    std::string packageSignature(const CpuSlotMesh& package)
    {
        std::string signature = package.slotName + "|" + package.displayName + "|" +
            package.materialPath + "|" + std::to_string(package.triangles.size());
        for (const auto& triangle : package.triangles)
        {
            signature += "#" + std::to_string(triangle.sourceTriangle);
            for (const auto& vertex : triangle.vertices)
            {
                signature += ":" + std::to_string(vertex.x) + "," + std::to_string(vertex.y) +
                    "," + std::to_string(vertex.u) + "," + std::to_string(vertex.v);
            }
        }
        return signature;
    }

    // Verifies clipping geometry, winding, UV interpolation and source indices
    // against independently computed data.
    void checkPackage(
        const CpuSlotMesh& package,
        const std::vector<P2>& positions,
        const std::vector<P2>& uvs,
        const std::vector<std::vector<unsigned>>& triangles,
        const std::vector<std::vector<P2>>& masks,
        bool clipped
    )
    {
        const auto expectedArea = expectedClipArea(positions, triangles, masks);
        CLIP_CHECK(std::fabs(outputArea(package) - expectedArea) < 1e-3);
        if (!clipped)
        {
            std::size_t sourceCount = 0;
            for (const auto& triangle : package.triangles)
            {
                CLIP_CHECK(triangle.sourceTriangle == sourceCount);
                ++sourceCount;
            }
            CLIP_CHECK(sourceCount == triangles.size());
        }

        for (const auto& triangle : package.triangles)
        {
            CLIP_CHECK(triangle.sourceTriangle < triangles.size());
            const auto& source = triangles[triangle.sourceTriangle];
            const P2 sourcePoints[3] = {
                positions[source[0]],
                positions[source[1]],
                positions[source[2]]
            };
            const P2 sourceUvs[3] = { uvs[source[0]], uvs[source[1]], uvs[source[2]] };
            const auto sourceSign = cross(sourcePoints[1] - sourcePoints[0], sourcePoints[2] - sourcePoints[0]);

            const P2 outputPoints[3] = {
                { triangle.vertices[0].x, triangle.vertices[0].y },
                { triangle.vertices[1].x, triangle.vertices[1].y },
                { triangle.vertices[2].x, triangle.vertices[2].y }
            };
            const auto outputSign = cross(outputPoints[1] - outputPoints[0], outputPoints[2] - outputPoints[0]);
            CLIP_CHECK(sourceSign * outputSign > 0.0); // Winding follows the source face.
            CLIP_CHECK(std::fabs(outputSign) * 0.5 > 1e-12); // No zero area faces.

            for (std::size_t corner = 0; corner < 3; ++corner)
            {
                for (const auto& mask : masks)
                {
                    CLIP_CHECK(insidePolygon(outputPoints[corner], mask));
                }

                double weights[3];
                barycentric(outputPoints[corner], sourcePoints[0], sourcePoints[1], sourcePoints[2], weights);
                CLIP_CHECK(std::fabs(weights[0] + weights[1] + weights[2] - 1.0) < 1e-5);
                const auto expectedU = weights[0] * sourceUvs[0].x + weights[1] * sourceUvs[1].x + weights[2] * sourceUvs[2].x;
                const auto expectedV = weights[0] * sourceUvs[0].y + weights[1] * sourceUvs[1].y + weights[2] * sourceUvs[2].y;
                CLIP_CHECK(std::fabs(triangle.vertices[corner].u - expectedU) < 1e-5);
                CLIP_CHECK(std::fabs(triangle.vertices[corner].v - expectedV) < 1e-5);
            }
        }
    }

    CpuMaskRange makeRange(const std::string& mask, const std::string& end, std::vector<CpuMaskPoint> polygon)
    {
        CpuMaskRange range;
        range.maskSlot = mask;
        range.endSlot = end;
        range.polygon = std::move(polygon);
        return range;
    }

    // Four vertex quad, two triangles. Bone root slides x 0 -> 10 in 12 frames
    // and the FFD shifts every vertex x by 0 -> 1 in the same window.
    const char* staticRigJson()
    {
        return R"JSON(
        {
          "version": "5.5", "compatibleVersion": "5.5", "name": "staticfixture", "frameRate": 24,
          "armature": [{
            "name": "rig", "type": "Armature", "frameRate": 24,
            "bone": [{ "name": "root" }],
            "slot": [
              { "name": "mask", "parent": "root", "displayIndex": 0 },
              { "name": "mesh", "parent": "root", "displayIndex": 0, "blendMode": "add",
                "color": { "aM": 80, "rM": 90, "gM": 70, "bM": 60 } },
              { "name": "end", "parent": "root", "displayIndex": 0 },
              { "name": "tail", "parent": "root", "displayIndex": 0 }
            ],
            "skin": [{
              "name": "default",
              "slot": [
                { "name": "mesh", "display": [{
                  "name": "grid", "type": "mesh", "path": "material/grid.png",
                  "vertices": [0, 0, 4, 0, 4, 4, 0, 4],
                  "uvs": [0, 0, 1, 0, 1, 1, 0, 1],
                  "triangles": [0, 1, 2, 0, 2, 3]
                }]},
                { "name": "tail", "display": [{
                  "name": "tailgrid", "type": "mesh", "path": "material/tail.png",
                  "vertices": [10, 0, 12, 0, 12, 2, 10, 2],
                  "uvs": [0, 0, 1, 0, 1, 1, 0, 1],
                  "triangles": [0, 1, 2, 0, 2, 3]
                }]}
              ]
            }],
            "animation": [{
              "name": "move", "duration": 24, "playTimes": 1,
              "bone": [{ "name": "root", "translateFrame": [
                { "duration": 12, "tweenEasing": 0, "x": 0, "y": 0 },
                { "duration": 12, "x": 10, "y": 0 }
              ]}],
              "ffd": [{ "slot": "mesh", "name": "grid", "frame": [
                { "duration": 12, "tweenEasing": 0, "vertices": [0, 0, 0, 0, 0, 0, 0, 0] },
                { "duration": 12, "vertices": [1, 0, 1, 0, 1, 0, 1, 0] }
              ]}]
            }]
          }]
        })JSON";
    }
} // namespace

// Unskinned mesh: FFD plus slot matrix, clipping against an empty mask slot.
CLIP_CASE(unskinned_mesh_ffd_clip)
{
    task_support::Factory factory;
    CLIP_CHECK(factory.parseDragonBonesData(staticRigJson()));
    auto armature = factory.buildArmature("rig", "staticfixture");
    CLIP_CHECK(armature != nullptr);
    armature->getAnimation()->play("move");
    armature->advanceTime(0.25f); // Frame 6: root x = 5, FFD x shift = 0.5.

    auto meshSlot = armature->getSlot("mesh");
    CLIP_CHECK(meshSlot != nullptr);
    CLIP_CHECK(std::fabs(meshSlot->_deformVertices->vertices[0] - 0.5f) < EPS);

    const std::vector<CpuMaskPoint> localPolygon = {
        { 1.0f, -1.0f }, { 5.0f, -1.0f }, { 5.0f, 5.0f }, { 1.0f, 5.0f }
    };
    const auto ranges = {
        makeRange("mask", "end", localPolygon)
    };

    std::vector<CpuSlotMesh> packages;
    CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));

    const auto positions = expectedStaticPositions(*meshSlot);
    const auto uvs = rawUvs(*meshSlot);
    const auto triangles = sourceTriangles(*meshSlot);
    const auto maskWorld = worldMaskPolygon(*armature->getSlot("mask"), localPolygon);

    // mask and end emit nothing; tail is outside the range and stays full.
    CLIP_CHECK(packages.size() == 2);
    CLIP_CHECK(packages[0].slotName == "mesh");
    CLIP_CHECK(packages[1].slotName == "tail");
    CLIP_CHECK(packages[0].displayName == "grid");
    CLIP_CHECK(packages[0].materialPath == "material/grid.png");
    CLIP_CHECK(packages[0].blendMode == BlendMode::Add);
    // The package must copy the slot's live color transform; float32 rounding
    // in the runtime pose timeline is reproduced rather than "corrected".
    const auto& liveColor = meshSlot->_colorTransform;
    CLIP_CHECK(packages[0].colorTransform.alphaMultiplier == liveColor.alphaMultiplier);
    CLIP_CHECK(packages[0].colorTransform.redMultiplier == liveColor.redMultiplier);
    CLIP_CHECK(packages[0].colorTransform.greenMultiplier == liveColor.greenMultiplier);
    CLIP_CHECK(packages[0].colorTransform.blueMultiplier == liveColor.blueMultiplier);
    CLIP_CHECK(packages[0].colorTransform.alphaOffset == liveColor.alphaOffset);
    CLIP_CHECK(std::fabs(liveColor.alphaMultiplier - 0.8f) < 0.02f);
    CLIP_CHECK(std::fabs(liveColor.greenMultiplier - 0.7f) < 0.02f);

    checkPackage(packages[0], positions, uvs, triangles, { maskWorld }, true);
    // The quad spans world x 5.5 .. 9.5 and the mask x 6 .. 10 over height 4.
    CLIP_CHECK(std::fabs(outputArea(packages[0]) - 14.0) < 1e-3);

    const auto tailPositions = expectedStaticPositions(*armature->getSlot("tail"));
    const auto tailUvs = rawUvs(*armature->getSlot("tail"));
    const auto tailTriangles = sourceTriangles(*armature->getSlot("tail"));
    checkPackage(packages[1], tailPositions, tailUvs, tailTriangles, {}, false);

    // Same CPU entry point with no masks must return the full deformed mesh.
    std::vector<CpuSlotMesh> fullPackages;
    CLIP_CHECK(buildClippedMesh(*armature, {}, fullPackages));
    CLIP_CHECK(fullPackages.size() == 2);
    checkPackage(fullPackages[0], positions, uvs, triangles, {}, false);
    CLIP_CHECK(std::fabs(outputArea(fullPackages[0]) - 16.0) < 1e-3);

    armature->dispose();
    factory.runtime.advanceTime(0.0f);
}

// Two bone weighted mesh with per influence FFD; results are already in
// armature space and must not be transformed by the slot matrix.
CLIP_CASE(two_bone_weighted_mesh)
{
    static const char* json = R"JSON(
    {
      "version": "5.5", "compatibleVersion": "5.5", "name": "skin fixture", "frameRate": 24,
      "armature": [{
        "name": "rig", "type": "Armature", "frameRate": 24,
        "bone": [
          { "name": "b0" },
          { "name": "b1", "parent": "b0", "transform": { "x": 2 } }
        ],
        "slot": [
          { "name": "mask", "parent": "b0", "displayIndex": 0 },
          { "name": "skin", "parent": "b0", "displayIndex": 0 },
          { "name": "end", "parent": "b0", "displayIndex": 0 }
        ],
        "skin": [{
          "name": "default",
          "slot": [{ "name": "skin", "display": [{
            "name": "weighted", "type": "mesh", "path": "material/skin.png",
            "vertices": [0, 0, 4, 0, 4, 2, 0, 2],
            "uvs": [0, 0, 1, 0, 1, 1, 0, 1],
            "triangles": [0, 1, 2, 0, 2, 3],
            "slotPose": [1, 0, 0, 1, 0, 0],
            "bonePose": [0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 2, 0],
            "weights": [1, 0, 1.0, 2, 0, 0.5, 1, 0.5, 2, 0, 0.5, 1, 0.5, 1, 0, 1.0]
          }]}]
        }],
        "animation": [{
          "name": "move", "duration": 24, "playTimes": 1,
          "bone": [{ "name": "b0", "translateFrame": [
            { "duration": 12, "tweenEasing": 0, "x": 0, "y": 0 },
            { "duration": 12, "x": 4, "y": 0 }
          ]}],
          "ffd": [{ "slot": "skin", "name": "weighted", "frame": [
            { "duration": 12, "tweenEasing": 0,
              "vertices": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0] },
            { "duration": 12,
              "vertices": [0, 0.5, 0, 0.5, 0, 0.5, 0, 0.5, 0, 0.5, 0, 0.5] }
          ]}]
        }]
      }]
    })JSON";

    task_support::Factory factory;
    CLIP_CHECK(factory.parseDragonBonesData(json));
    auto armature = factory.buildArmature("rig", "skin fixture");
    CLIP_CHECK(armature != nullptr);
    armature->getAnimation()->play("move");
    armature->advanceTime(0.25f); // b0 x = 2, FFD y shift = 0.125 per influence.

    auto slot = armature->getSlot("skin");
    const auto positions = expectedSkinnedPositions(*slot);
    CLIP_CHECK(positions.size() == 4);
    CLIP_CHECK(std::fabs(positions[0].x - 2.0) < EPS);
    CLIP_CHECK(std::fabs(positions[0].y - 0.25) < EPS);
    // v1: b0(2) on local 4 -> 6, b1(4) on local 2 -> 6, 50/50 stays 6.
    CLIP_CHECK(std::fabs(positions[1].x - 6.0) < EPS);
    CLIP_CHECK(std::fabs(positions[1].y - 0.25) < EPS);
    CLIP_CHECK(std::fabs(positions[3].x - 2.0) < EPS);

    const std::vector<CpuMaskPoint> localPolygon = {
        { 1.0f, -1.0f }, { 8.0f, -1.0f }, { 8.0f, 3.0f }, { 1.0f, 3.0f }
    };
    const auto ranges = { makeRange("mask", "end", localPolygon) };
    std::vector<CpuSlotMesh> packages;
    CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));
    CLIP_CHECK(packages.size() == 1);
    CLIP_CHECK(packages[0].slotName == "skin");

    const auto uvs = rawUvs(*slot);
    const auto triangles = sourceTriangles(*slot);
    const auto maskWorld = worldMaskPolygon(*armature->getSlot("mask"), localPolygon);
    checkPackage(packages[0], positions, uvs, triangles, { maskWorld }, true);

    armature->dispose();
    factory.runtime.advanceTime(0.0f);
}

// Rotated and reflected mask bone: clipping must keep the polygon interior
// rather than flipping to the exterior.
CLIP_CASE(rotated_reflected_mask)
{
    static const char* json = R"JSON(
    {
      "version": "5.5", "compatibleVersion": "5.5", "name": "reflectfixture", "frameRate": 24,
      "armature": [{
        "name": "rig", "type": "Armature", "frameRate": 24,
        "bone": [{ "name": "mb", "transform": { "rotate": -90, "scX": -1 } }, { "name": "root" }],
        "slot": [
          { "name": "mask", "parent": "mb", "displayIndex": 0 },
          { "name": "mesh", "parent": "root", "displayIndex": 0 },
          { "name": "end", "parent": "root", "displayIndex": 0 }
        ],
        "skin": [{
          "name": "default",
          "slot": [{ "name": "mesh", "display": [{
            "name": "plane", "type": "mesh", "path": "material/plane.png",
            "vertices": [-4, -4, 4, -4, 4, 4, -4, 4],
            "uvs": [0, 0, 1, 0, 1, 1, 0, 1],
            "triangles": [0, 1, 2, 0, 2, 3]
          }]}]
        }]
      }]
    })JSON";

    task_support::Factory factory;
    CLIP_CHECK(factory.parseDragonBonesData(json));
    auto armature = factory.buildArmature("rig", "reflectfixture");
    CLIP_CHECK(armature != nullptr);
    armature->advanceTime(0.0f);

    auto maskSlot = armature->getSlot("mask");
    const auto maskMatrix = expectedSlotMatrix(*maskSlot);
    const auto determinant =
        maskMatrix.a * maskMatrix.d -
        maskMatrix.b * maskMatrix.c;
    CLIP_CHECK(determinant < 0.0f); // The mask transform really is reflected.

    const std::vector<CpuMaskPoint> localPolygon = {
        { 0.0f, 0.0f }, { 2.0f, 0.0f }, { 2.0f, 2.0f }, { 0.0f, 2.0f }
    };
    const auto ranges = { makeRange("mask", "end", localPolygon) };
    std::vector<CpuSlotMesh> packages;
    CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));
    CLIP_CHECK(packages.size() == 1);

    auto meshSlot = armature->getSlot("mesh");
    const auto positions = expectedStaticPositions(*meshSlot);
    const auto uvs = rawUvs(*meshSlot);
    const auto triangles = sourceTriangles(*meshSlot);
    const auto maskWorld = worldMaskPolygon(*maskSlot, localPolygon);

    // Reflected square occupies area 4; the kept area must match the interior
    // intersection and stay far below the exterior area of the source mesh.
    checkPackage(packages[0], positions, uvs, triangles, { maskWorld }, true);
    CLIP_CHECK(std::fabs(outputArea(packages[0]) - 4.0) < 1e-3);
    CLIP_CHECK(outputArea(packages[0]) < 64.0 - 4.0 - 1.0);

    armature->dispose();
    factory.runtime.advanceTime(0.0f);
}

// Nested ranges clip to the intersection of both masks; separate ranges never
// interact. A BoundingBox display is used as the inner mask slot.
CLIP_CASE(nested_and_separate_ranges)
{
    static const char* json = R"JSON(
    {
      "version": "5.5", "compatibleVersion": "5.5", "name": "nestfixture", "frameRate": 24,
      "armature": [{
        "name": "rig", "type": "Armature", "frameRate": 24,
        "bone": [{ "name": "root" }],
        "slot": [
          { "name": "outerMask", "parent": "root", "displayIndex": 0 },
          { "name": "innerMask", "parent": "root", "displayIndex": 0 },
          { "name": "mesh", "parent": "root", "displayIndex": 0 },
          { "name": "innerEnd", "parent": "root", "displayIndex": 0 },
          { "name": "outerEnd", "parent": "root", "displayIndex": 0 },
          { "name": "gap", "parent": "root", "displayIndex": 0 },
          { "name": "secondMask", "parent": "root", "displayIndex": 0 },
          { "name": "meshB", "parent": "root", "displayIndex": 0 },
          { "name": "secondEnd", "parent": "root", "displayIndex": 0 }
        ],
        "skin": [{
          "name": "default",
          "slot": [
            { "name": "innerMask", "display": [{
              "name": "innerBox", "type": "boundingBox", "subType": "polygon",
              "width": 4, "height": 4, "vertices": [0, 0, 4, 0, 4, 4, 0, 4]
            }]},
            { "name": "mesh", "display": [{
              "name": "grid", "type": "mesh", "path": "material/grid.png",
              "vertices": [0, 0, 10, 0, 10, 10, 0, 10],
              "uvs": [0, 0, 1, 0, 1, 1, 0, 1],
              "triangles": [0, 1, 2, 0, 2, 3]
            }]},
            { "name": "gap", "display": [{
              "name": "gapgrid", "type": "mesh", "path": "material/gap.png",
              "vertices": [0, 0, 1, 0, 1, 1, 0, 1],
              "uvs": [0, 0, 1, 0, 1, 1, 0, 1],
              "triangles": [0, 1, 2, 0, 2, 3]
            }]},
            { "name": "meshB", "display": [{
              "name": "gridb", "type": "mesh", "path": "material/gridb.png",
              "vertices": [0, 0, 10, 0, 10, 10, 0, 10],
              "uvs": [0, 0, 1, 0, 1, 1, 0, 1],
              "triangles": [0, 1, 2, 0, 2, 3]
            }]}
          ]
        }]
      }]
    })JSON";

    task_support::Factory factory;
    CLIP_CHECK(factory.parseDragonBonesData(json));
    auto armature = factory.buildArmature("rig", "nestfixture");
    CLIP_CHECK(armature != nullptr);
    armature->advanceTime(0.0f);
    CLIP_CHECK(armature->getSlot("innerMask")->getBoundingBoxData() != nullptr);

    const std::vector<CpuMaskPoint> outerLocal = {
        { 0.0f, 0.0f }, { 8.0f, 0.0f }, { 8.0f, 8.0f }, { 0.0f, 8.0f }
    };
    const std::vector<CpuMaskPoint> innerLocal = {
        { 2.0f, 2.0f }, { 6.0f, 2.0f }, { 6.0f, 6.0f }, { 2.0f, 6.0f }
    };
    const std::vector<CpuMaskPoint> secondLocal = {
        { 1.0f, 1.0f }, { 3.0f, 1.0f }, { 3.0f, 3.0f }, { 1.0f, 3.0f }
    };

    const std::vector<CpuMaskRange> ranges = {
        makeRange("outerMask", "outerEnd", outerLocal),
        makeRange("innerMask", "innerEnd", innerLocal),
        makeRange("secondMask", "secondEnd", secondLocal)
    };
    std::vector<CpuSlotMesh> packages;
    CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));

    // mesh, gap (full, between ranges), meshB (second range); mask slots output
    // nothing even though innerMask carries a BoundingBox display.
    CLIP_CHECK(packages.size() == 3);
    CLIP_CHECK(packages[0].slotName == "mesh");
    CLIP_CHECK(packages[1].slotName == "gap");
    CLIP_CHECK(packages[2].slotName == "meshB");

    auto meshSlot = armature->getSlot("mesh");
    const auto positions = expectedStaticPositions(*meshSlot);
    const auto uvs = rawUvs(*meshSlot);
    const auto triangles = sourceTriangles(*meshSlot);
    const auto outerWorld = worldMaskPolygon(*armature->getSlot("outerMask"), outerLocal);
    const auto innerWorld = worldMaskPolygon(*armature->getSlot("innerMask"), innerLocal);
    checkPackage(packages[0], positions, uvs, triangles, { outerWorld, innerWorld }, true);
    CLIP_CHECK(std::fabs(outputArea(packages[0]) - 16.0) < 1e-3); // Inner square 4x4.

    auto gapSlot = armature->getSlot("gap");
    const auto gapPositions = expectedStaticPositions(*gapSlot);
    const auto gapUvs = rawUvs(*gapSlot);
    const auto gapTriangles = sourceTriangles(*gapSlot);
    checkPackage(packages[1], gapPositions, gapUvs, gapTriangles, {}, false);

    auto meshBSlot = armature->getSlot("meshB");
    const auto positionsB = expectedStaticPositions(*meshBSlot);
    const auto uvsB = rawUvs(*meshBSlot);
    const auto trianglesB = sourceTriangles(*meshBSlot);
    const auto secondWorld = worldMaskPolygon(*armature->getSlot("secondMask"), secondLocal);
    checkPackage(packages[2], positionsB, uvsB, trianglesB, { secondWorld }, true);
    CLIP_CHECK(std::fabs(outputArea(packages[2]) - 4.0) < 1e-3);

    armature->dispose();
    factory.runtime.advanceTime(0.0f);
}

// A zOrder animation reorders the slots: a range valid in the first half must
// be rejected once the end slot is sorted before the mask slot.
CLIP_CASE(zorder_change_rejudges_ranges)
{
    static const char* json = R"JSON(
    {
      "version": "5.5", "compatibleVersion": "5.5", "name": "zorderfixture", "frameRate": 24,
      "armature": [{
        "name": "rig", "type": "Armature", "frameRate": 24,
        "bone": [{ "name": "root" }],
        "slot": [
          { "name": "mask", "parent": "root", "displayIndex": 0 },
          { "name": "mesh", "parent": "root", "displayIndex": 0 },
          { "name": "end", "parent": "root", "displayIndex": 0 },
          { "name": "tail", "parent": "root", "displayIndex": 0 }
        ],
        "skin": [{
          "name": "default",
          "slot": [
            { "name": "mesh", "display": [{
              "name": "grid", "type": "mesh", "path": "material/grid.png",
              "vertices": [0, 0, 4, 0, 4, 4, 0, 4],
              "uvs": [0, 0, 1, 0, 1, 1, 0, 1],
              "triangles": [0, 1, 2, 0, 2, 3]
            }]},
            { "name": "tail", "display": [{
              "name": "tailgrid", "type": "mesh", "path": "material/tail.png",
              "vertices": [0, 0, 1, 0, 1, 1, 0, 1],
              "uvs": [0, 0, 1, 0, 1, 1, 0, 1],
              "triangles": [0, 1, 2, 0, 2, 3]
            }]}
          ]
        }],
        "animation": [{
          "name": "reorder", "duration": 24, "playTimes": 1,
          "zOrder": { "frame": [
            { "duration": 12 },
            { "duration": 12, "zOrder": [0, 1, 1, 1, 2, -2] }
          ]}
        }]
      }]
    })JSON";

    task_support::Factory factory;
    CLIP_CHECK(factory.parseDragonBonesData(json));
    auto armature = factory.buildArmature("rig", "zorderfixture");
    CLIP_CHECK(armature != nullptr);
    armature->getAnimation()->play("reorder");
    armature->advanceTime(0.25f); // Frame 6, original order still in effect.

    const std::vector<CpuMaskPoint> localPolygon = {
        { 1.0f, -1.0f }, { 5.0f, -1.0f }, { 5.0f, 5.0f }, { 1.0f, 5.0f }
    };
    const auto ranges = { makeRange("mask", "end", localPolygon) };
    std::vector<CpuSlotMesh> packages;
    CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));
    CLIP_CHECK(packages.size() == 2);
    CLIP_CHECK(packages[0].slotName == "mesh");

    armature->advanceTime(0.5f); // Reaches frame 18 with the swapped order.
    const auto& orderedSlots = armature->getSlots();
    CLIP_CHECK(orderedSlots.front()->getName() == "end");

    std::vector<CpuSlotMesh> swappedPackages;
    CLIP_CHECK(!buildClippedMesh(*armature, ranges, swappedPackages));
    CLIP_CHECK(swappedPackages.empty());

    armature->dispose();
    factory.runtime.advanceTime(0.0f);
}

// Display switching changes the emitted geometry; active Image and child
// Armature displays are explicit failures instead of silent holes.
CLIP_CASE(display_switch_and_unsupported_displays)
{
    static const char* json = R"JSON(
    {
      "version": "5.5", "compatibleVersion": "5.5", "name": "switchfixture", "frameRate": 24,
      "armature": [
        {
          "name": "rig", "type": "Armature", "frameRate": 24,
          "bone": [{ "name": "root" }],
          "slot": [
            { "name": "mask", "parent": "root", "displayIndex": 0 },
            { "name": "swap", "parent": "root", "displayIndex": 0 },
            { "name": "end", "parent": "root", "displayIndex": 0 }
          ],
          "skin": [{
            "name": "default",
            "slot": [
              { "name": "swap", "display": [
                {
                  "name": "meshA", "type": "mesh", "path": "material/a.png",
                  "vertices": [0, 0, 4, 0, 4, 4, 0, 4],
                  "uvs": [0, 0, 1, 0, 1, 1, 0, 1],
                  "triangles": [0, 1, 2, 0, 2, 3]
                },
                {
                  "name": "meshB", "type": "mesh", "path": "material/b.png",
                  "vertices": [0, 0, 2, 0, 1, 2],
                  "uvs": [0, 0, 1, 0, 0.5, 1],
                  "triangles": [0, 1, 2]
                }
              ]}
            ]
          }],
          "animation": [{
            "name": "switch", "duration": 24, "playTimes": 1,
            "slot": [{ "name": "swap", "displayFrame": [
              { "duration": 12, "value": 0 },
              { "duration": 12, "value": 1 }
            ]}]
          }]
        },
        {
          "name": "imageRig", "type": "Armature", "frameRate": 24,
          "bone": [{ "name": "root" }],
          "slot": [{ "name": "imageSlot", "parent": "root", "displayIndex": 0 }],
          "skin": [{
            "name": "default",
            "slot": [{ "name": "imageSlot", "display": [
              { "name": "img", "type": "image", "path": "material/img.png" }
            ]}]
          }]
        },
        {
          "name": "child", "type": "Armature", "frameRate": 24,
          "bone": [{ "name": "root" }],
          "slot": [{ "name": "empty", "parent": "root", "displayIndex": 0 }]
        },
        {
          "name": "host", "type": "Armature", "frameRate": 24,
          "bone": [{ "name": "root" }],
          "slot": [{ "name": "childSlot", "parent": "root", "displayIndex": 0 }],
          "skin": [{
            "name": "default",
            "slot": [{ "name": "childSlot", "display": [
              { "name": "child", "type": "armature", "path": "child" }
            ]}]
          }]
        },
        {
          "name": "badMaskRig", "type": "Armature", "frameRate": 24,
          "bone": [{ "name": "root" }],
          "slot": [
            { "name": "mask", "parent": "root", "displayIndex": 0 },
            { "name": "helper", "parent": "root", "displayIndex": 0 },
            { "name": "mesh", "parent": "root", "displayIndex": 0 },
            { "name": "end", "parent": "root", "displayIndex": 0 }
          ],
          "skin": [{
            "name": "default",
            "slot": [
              { "name": "mask", "display": [{
                "name": "maskmesh", "type": "mesh", "path": "material/maskmesh.png",
                "vertices": [0, 0, 1, 0, 0, 1],
                "uvs": [0, 0, 1, 0, 0, 1],
                "triangles": [0, 1, 2]
              }]},
              { "name": "helper", "display": [{
                "name": "helperbox", "type": "boundingBox", "subType": "polygon",
                "width": 2, "height": 2, "vertices": [0, 0, 2, 0, 2, 2, 0, 2]
              }]},
              { "name": "mesh", "display": [{
                "name": "grid", "type": "mesh", "path": "material/grid.png",
                "vertices": [0, 0, 2, 0, 2, 2, 0, 2],
                "uvs": [0, 0, 1, 0, 1, 1, 0, 1],
                "triangles": [0, 1, 2, 0, 2, 3]
              }]}
            ]
          }]
        }
      ]
    })JSON";

    task_support::Factory factory;
    CLIP_CHECK(factory.parseDragonBonesData(json));
    auto armature = factory.buildArmature("rig", "switchfixture");
    CLIP_CHECK(armature != nullptr);
    armature->getAnimation()->play("switch");
    armature->advanceTime(0.25f);

    const std::vector<CpuMaskPoint> localPolygon = {
        { -1.0f, -1.0f }, { 5.0f, -1.0f }, { 5.0f, 5.0f }, { -1.0f, 5.0f }
    };
    const auto ranges = { makeRange("mask", "end", localPolygon) };
    std::vector<CpuSlotMesh> packages;
    CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));
    CLIP_CHECK(packages.size() == 1);
    CLIP_CHECK(packages[0].displayName == "meshA");
    CLIP_CHECK(packages[0].triangles.size() == 2);

    armature->advanceTime(0.5f); // Frame 18: display index switches to meshB.
    std::vector<CpuSlotMesh> switched;
    CLIP_CHECK(buildClippedMesh(*armature, ranges, switched));
    CLIP_CHECK(switched.size() == 1);
    CLIP_CHECK(switched[0].displayName == "meshB");
    CLIP_CHECK(switched[0].materialPath == "material/b.png");
    CLIP_CHECK(switched[0].triangles.size() == 1);
    CLIP_CHECK(switched[0].triangles[0].sourceTriangle == 0);

    // An active Image display can never be emitted as CPU mesh geometry.
    auto imageArmature = factory.buildArmature("imageRig", "switchfixture");
    CLIP_CHECK(imageArmature != nullptr);
    imageArmature->advanceTime(0.0f);
    std::vector<CpuSlotMesh> imageAttempt;
    CLIP_CHECK(!buildClippedMesh(*imageArmature, {}, imageAttempt));
    CLIP_CHECK(imageAttempt.empty());

    // The same Image hidden no longer participates, so the frame is valid.
    imageArmature->getSlot("imageSlot")->setVisible(false);
    std::vector<CpuSlotMesh> hiddenImage;
    CLIP_CHECK(buildClippedMesh(*imageArmature, {}, hiddenImage));
    CLIP_CHECK(hiddenImage.empty());

    // An active child Armature display is rejected the same way.
    auto host = factory.buildArmature("host", "switchfixture");
    CLIP_CHECK(host != nullptr);
    host->advanceTime(0.0f);
    std::vector<CpuSlotMesh> childAttempt;
    CLIP_CHECK(!buildClippedMesh(*host, {}, childAttempt));
    CLIP_CHECK(childAttempt.empty());

    // A mask marker that actually shows a Mesh is rejected instead of being
    // treated as a clip source.
    auto badMask = factory.buildArmature("badMaskRig", "switchfixture");
    CLIP_CHECK(badMask != nullptr);
    badMask->advanceTime(0.0f);
    const std::vector<CpuMaskPoint> markerPolygon = {
        { 0.0f, 0.0f }, { 2.0f, 0.0f }, { 2.0f, 2.0f }, { 0.0f, 2.0f }
    };
    std::vector<CpuSlotMesh> badMaskAttempt;
    CLIP_CHECK(!buildClippedMesh(*badMask, { makeRange("mask", "end", markerPolygon) }, badMaskAttempt));
    CLIP_CHECK(badMaskAttempt.empty());

    // A BoundingBox slot outside any range is legal but emits no package; the
    // regular mesh next to it still builds fully.
    std::vector<CpuSlotMesh> helperBuild;
    CLIP_CHECK(buildClippedMesh(*badMask, {}, helperBuild));
    CLIP_CHECK(helperBuild.size() == 2);
    CLIP_CHECK(helperBuild[0].slotName == "mask");
    CLIP_CHECK(helperBuild[1].slotName == "mesh");

    armature->dispose();
    imageArmature->dispose();
    host->dispose();
    badMask->dispose();
    factory.runtime.advanceTime(0.0f);
}

// Hidden end slot still terminates its range; hidden mask empties its range;
// invalid input is rejected; saved packages survive the next frame and pool
// disposal with their own copies.
CLIP_CASE(visibility_errors_and_package_lifetime)
{
    task_support::Factory factory;
    CLIP_CHECK(factory.parseDragonBonesData(staticRigJson()));
    auto armature = factory.buildArmature("rig", "staticfixture");
    CLIP_CHECK(armature != nullptr);
    armature->getAnimation()->play("move");
    armature->advanceTime(0.25f);

    const std::vector<CpuMaskPoint> localPolygon = {
        { 1.0f, -1.0f }, { 5.0f, -1.0f }, { 5.0f, 5.0f }, { 1.0f, 5.0f }
    };

    // Hiding the end slot does not extend the range past it: tail stays full.
    armature->getSlot("end")->setVisible(false);
    auto ranges = { makeRange("mask", "end", localPolygon) };
    std::vector<CpuSlotMesh> hiddenEnd;
    CLIP_CHECK(buildClippedMesh(*armature, ranges, hiddenEnd));
    CLIP_CHECK(hiddenEnd.size() == 2);
    CLIP_CHECK(hiddenEnd[0].slotName == "mesh");
    CLIP_CHECK(hiddenEnd[1].slotName == "tail");
    CLIP_CHECK(std::fabs(outputArea(hiddenEnd[1]) - 4.0) < 1e-3);
    armature->getSlot("end")->setVisible(true);

    // A hidden mask produces an empty range, so mesh disappears but the
    // uncovered tail slot is still emitted.
    armature->getSlot("mask")->setVisible(false);
    std::vector<CpuSlotMesh> hiddenMask;
    CLIP_CHECK(buildClippedMesh(*armature, ranges, hiddenMask));
    CLIP_CHECK(hiddenMask.size() == 1);
    CLIP_CHECK(hiddenMask[0].slotName == "tail");
    armature->getSlot("mask")->setVisible(true);

    // Illegal slot references.
    std::vector<CpuSlotMesh> badOutput;
    CLIP_CHECK(!buildClippedMesh(*armature, { makeRange("ghost", "end", localPolygon) }, badOutput));
    CLIP_CHECK(badOutput.empty());
    CLIP_CHECK(!buildClippedMesh(*armature, { makeRange("mask", "ghost", localPolygon) }, badOutput));

    // Non convex bowtie polygon.
    const std::vector<CpuMaskPoint> bowtie = {
        { 0.0f, 0.0f }, { 4.0f, 4.0f }, { 4.0f, 0.0f }, { 0.0f, 4.0f }
    };
    CLIP_CHECK(!buildClippedMesh(*armature, { makeRange("mask", "end", bowtie) }, badOutput));

    // Polygon with a non finite point.
    const std::vector<CpuMaskPoint> infinite = {
        { 0.0f, 0.0f }, { 2.0f, 0.0f }, { std::numeric_limits<float>::infinity(), 2.0f }, { 0.0f, 2.0f }
    };
    CLIP_CHECK(!buildClippedMesh(*armature, { makeRange("mask", "end", infinite) }, badOutput));

    // Too few points.
    const std::vector<CpuMaskPoint> twoPoints = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
    CLIP_CHECK(!buildClippedMesh(*armature, { makeRange("mask", "end", twoPoints) }, badOutput));

    // Crossing ranges are rejected, nested ones are accepted.
    const std::vector<CpuMaskRange> crossing = {
        makeRange("mask", "end", localPolygon),
        makeRange("mesh", "tail", localPolygon)
    };
    CLIP_CHECK(!buildClippedMesh(*armature, crossing, badOutput));

    // Save a package, advance another frame and rebuild: the old copy must be
    // untouched while the new build reflects the new pose.
    std::vector<CpuSlotMesh> saved;
    CLIP_CHECK(buildClippedMesh(*armature, { makeRange("mask", "end", localPolygon) }, saved));
    CLIP_CHECK(saved.size() == 2);
    const auto savedArea = outputArea(saved[0]);
    const auto savedSignature = packageSignature(saved[0]);
    const auto savedTriangleCount = saved[0].triangles.size();
    const auto savedName = saved[0].slotName;
    const auto savedPath = saved[0].materialPath;

    armature->advanceTime(0.25f); // Frame 12: root x = 10, FFD shift = 1.
    std::vector<CpuSlotMesh> nextFrame;
    CLIP_CHECK(buildClippedMesh(*armature, { makeRange("mask", "end", localPolygon) }, nextFrame));
    CLIP_CHECK(nextFrame.size() == 2);
    // The mask rides the same bone, so the quad ends up fully inside it.
    CLIP_CHECK(std::fabs(outputArea(nextFrame[0]) - 16.0) < 1e-3);
    // Original quad vertex 0 moves from (5.5, 0) to (11, 0).
    CLIP_CHECK(packageContainsVertex(nextFrame[0], 11.0, 0.0));
    // The previously saved package is still the frame 6 snapshot.
    CLIP_CHECK(std::fabs(outputArea(saved[0]) - savedArea) < 1e-6);
    CLIP_CHECK(saved[0].triangles.size() == savedTriangleCount);
    CLIP_CHECK(packageSignature(saved[0]) == savedSignature);
    CLIP_CHECK(saved[0].slotName == savedName);
    CLIP_CHECK(saved[0].materialPath == savedPath);

    // A mask placed completely outside the mesh leaves the slot without a
    // record, while the uncovered tail slot is still emitted.
    const std::vector<CpuMaskPoint> farPolygon = {
        { 100.0f, 100.0f }, { 104.0f, 100.0f }, { 104.0f, 104.0f }, { 100.0f, 104.0f }
    };
    std::vector<CpuSlotMesh> missed;
    CLIP_CHECK(buildClippedMesh(*armature, { makeRange("mask", "end", farPolygon) }, missed));
    CLIP_CHECK(missed.size() == 1);
    CLIP_CHECK(missed[0].slotName == "tail");

    // Releasing the source armature back to its pool cannot invalidate the
    // self contained package copies.
    armature->dispose();
    factory.runtime.advanceTime(0.0f);
    CLIP_CHECK(saved.size() == 2);
    CLIP_CHECK(saved[0].slotName == "mesh");
    CLIP_CHECK(saved[0].materialPath == "material/grid.png");
    CLIP_CHECK(saved[0].triangles.size() == savedTriangleCount);
    CLIP_CHECK(std::fabs(outputArea(saved[0]) - savedArea) < 1e-6);
    CLIP_CHECK(saved[1].slotName == "tail");
}
