// Tests for cal3d::TransparentPacket: geometric BSP splitting, camera-ray
// compositing against the unsplit source triangles, and end-to-end animation
// through CalSkeleton/CalPhysique.

#include "support/check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <vector>

#include "cal3d/animation.h"
#include "cal3d/bonetransform.h"
#include "cal3d/color.h"
#include "cal3d/coreanimation.h"
#include "cal3d/corebone.h"
#include "cal3d/corekeyframe.h"
#include "cal3d/coremorphtarget.h"
#include "cal3d/coreskeleton.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/coretrack.h"
#include "cal3d/matrix.h"
#include "cal3d/mixer.h"
#include "cal3d/physique.h"
#include "cal3d/quaternion.h"
#include "cal3d/skeleton.h"
#include "cal3d/submesh.h"
#include "cal3d/transform.h"
#include "cal3d/transparentpacket.h"
#include "cal3d/vector.h"

namespace {
    const float kEps = 1e-5f;

    bool close(float a, float b, float eps = kEps) {
        return std::fabs(a - b) <= eps;
    }

    bool closeVec(const CalVector& a, const CalVector& b, float eps = kEps) {
        return close(a.x, b.x, eps)
            && close(a.y, b.y, eps)
            && close(a.z, b.z, eps);
    }

    CalColor32 packColor(int r, int g, int b, int a) {
        return static_cast<CalColor32>(
            (static_cast<unsigned>(a) << 24)
            | (static_cast<unsigned>(r) << 16)
            | (static_cast<unsigned>(g) << 8)
            | static_cast<unsigned>(b));
    }

    struct SpecVertex {
        CalVector position;
        CalVector normal;
        std::array<float, 2> uv;
        CalColor32 color;
        std::vector<std::pair<unsigned, float>> influences;
    };

    struct MeshSpec {
        bool hasUV = true;
        std::vector<SpecVertex> vertices;
        std::vector<std::array<CalIndex, 3>> faces;
        int materialThreadId = 0;
    };

    ISTD::shared_ptr<CalCoreSubmesh> makeCoreSubmesh(const MeshSpec& spec) {
        auto core = ISTD::make_shared<CalCoreSubmesh>(
            static_cast<int>(spec.vertices.size()),
            spec.hasUV,
            static_cast<int>(spec.faces.size()));
        for (size_t i = 0; i < spec.vertices.size(); ++i) {
            const SpecVertex& sv = spec.vertices[i];
            CalCoreSubmesh::Vertex v;
            v.position = CalPoint4(sv.position.x, sv.position.y, sv.position.z);
            v.normal = CalVector4(sv.normal.x, sv.normal.y, sv.normal.z);
            std::vector<CalCoreSubmesh::Influence> inf;
            for (const auto& pair : sv.influences) {
                inf.push_back(CalCoreSubmesh::Influence(
                    pair.first, pair.second, false));
            }
            core->addVertex(v, sv.color, inf);
            if (spec.hasUV) {
                core->setTextureCoordinate(
                    static_cast<int>(i),
                    CalCoreSubmesh::TextureCoordinate(sv.uv[0], sv.uv[1]));
            }
        }
        for (const auto& face : spec.faces) {
            core->addFace(CalCoreSubmesh::Face(face[0], face[1], face[2]));
        }
        core->coreMaterialThreadId = spec.materialThreadId;
        return core;
    }

    BoneTransform identityBone() {
        return BoneTransform(CalMatrix(), CalVector(0.0f, 0.0f, 0.0f));
    }

    cal3d::RotateTranslate identityObject() {
        return cal3d::RotateTranslate(CalQuaternion(), CalVector(0, 0, 0));
    }

    cal3d::TransparentInput singleBoneInput(
        const CalSubmesh* submesh,
        const BoneTransform* palette,
        size_t boneCount,
        const cal3d::RotateTranslate& object = identityObject())
    {
        cal3d::TransparentInput input;
        input.submesh = submesh;
        input.bones = palette;
        input.boneCount = boneCount;
        input.objectToWorld = object;
        return input;
    }

    float triangleArea(
        const CalVector& a, const CalVector& b, const CalVector& c)
    {
        return cross(b - a, c - a).length() * 0.5f;
    }

    // Moller-Trumbore, two sided.
    bool rayTriangle(
        const CalVector& origin,
        const CalVector& direction,
        const CalVector& a,
        const CalVector& b,
        const CalVector& c,
        float& hitT,
        float& hitU,
        float& hitV)
    {
        const float rayEps = 1e-9f;
        const CalVector e1 = b - a;
        const CalVector e2 = c - a;
        const CalVector p = cross(direction, e2);
        const float determinant = dot(e1, p);
        if (std::fabs(determinant) < rayEps) {
            return false;
        }
        const float inverse = 1.0f / determinant;
        const CalVector s = origin - a;
        hitU = dot(s, p) * inverse;
        if (hitU < -rayEps || hitU > 1.0f + rayEps) {
            return false;
        }
        const CalVector q = cross(s, e1);
        hitV = dot(direction, q) * inverse;
        if (hitV < -rayEps || hitU + hitV > 1.0f + rayEps) {
            return false;
        }
        hitT = dot(e2, q) * inverse;
        return hitT > rayEps;
    }

    float pointEdgeDistance(
        const CalVector& point,
        const CalVector& a,
        const CalVector& b)
    {
        const CalVector edge = b - a;
        return cross(edge, point - a).length() / edge.length();
    }

    struct Rgba {
        float r, g, b, a;
    };

    Rgba sourceOver(const Rgba& source, const Rgba& backdrop) {
        Rgba result;
        result.r = source.r * source.a + backdrop.r * (1.0f - source.a);
        result.g = source.g * source.a + backdrop.g * (1.0f - source.a);
        result.b = source.b * source.a + backdrop.b * (1.0f - source.a);
        result.a = source.a + backdrop.a * (1.0f - source.a);
        return result;
    }

    Rgba interpolateRgba(
        const cal3d::TransparentFragment& fragment,
        float u,
        float v)
    {
        const float w0 = 1.0f - u - v;
        const float w[3] = {w0, u, v};
        Rgba result{0, 0, 0, 0};
        for (int k = 0; k < 3; ++k) {
            const cal3d::TransparentCorner& c = fragment.corner[k];
            result.r += w[k] * c.r;
            result.g += w[k] * c.g;
            result.b += w[k] * c.b;
            result.a += w[k] * c.a;
        }
        return result;
    }

    // True when every vertex of each triangle lies strictly on both sides of
    // the other's plane: the only transverse intersection the BSP must remove.
    bool transverseIntersection(
        const cal3d::TransparentFragment& x,
        const cal3d::TransparentFragment& y,
        float epsilon)
    {
        const CalVector nx = cross(
            x.corner[1].position - x.corner[0].position,
            x.corner[2].position - x.corner[0].position);
        const CalVector ny = cross(
            y.corner[1].position - y.corner[0].position,
            y.corner[2].position - y.corner[0].position);
        if (nx.lengthSquared() == 0.0f || ny.lengthSquared() == 0.0f) {
            return false;
        }
        int xSide[3];
        int ySide[3];
        for (int k = 0; k < 3; ++k) {
            const float dx = dot(nx, y.corner[k].position - x.corner[0].position);
            const float dy = dot(ny, x.corner[k].position - y.corner[0].position);
            xSide[k] = dx > epsilon ? 1 : (dx < -epsilon ? -1 : 0);
            ySide[k] = dy > epsilon ? 1 : (dy < -epsilon ? -1 : 0);
        }
        const bool xStraddlesY =
            std::min(xSide[0], std::min(xSide[1], xSide[2])) < 0
            && std::max(xSide[0], std::max(xSide[1], xSide[2])) > 0;
        const bool yStraddlesX =
            std::min(ySide[0], std::min(ySide[1], ySide[2])) < 0
            && std::max(ySide[0], std::max(ySide[1], ySide[2])) > 0;
        return xStraddlesY && yStraddlesX;
    }

    // Two triangles genuinely crossing, sharing only their apex: the same
    // conflicting pair that pure reordering cannot resolve.
    MeshSpec crossingSpec() {
        MeshSpec spec;
        const CalVector positions[6] = {
            CalVector(-1, -1, -1), CalVector(1, -1, 1), CalVector(0, 1, 0),
            CalVector(-1, -1, 1), CalVector(1, -1, -1), CalVector(0, 1, 0)
        };
        const CalVector normals[6] = {
            CalVector(0, 0, 2), CalVector(1, 1, 1), CalVector(-1, 0, 1),
            CalVector(2, 0, 0), CalVector(0, 3, 0), CalVector(1, -1, 1)
        };
        const CalColor32 colors[6] = {
            packColor(255, 0, 0, 128), packColor(0, 255, 0, 128),
            packColor(0, 0, 255, 128), packColor(255, 255, 0, 128),
            packColor(0, 255, 255, 128), packColor(255, 0, 255, 128)
        };
        const std::array<float, 2> uvs[6] = {
            {{0.1f, 0.2f}}, {{0.7f, 0.9f}}, {{0.4f, 0.35f}},
            {{0.3f, 0.8f}}, {{0.95f, 0.15f}}, {{0.55f, 0.6f}}
        };
        for (int i = 0; i < 6; ++i) {
            SpecVertex sv;
            sv.position = positions[i];
            sv.normal = normals[i];
            sv.uv = uvs[i];
            sv.color = colors[i];
            sv.influences.push_back(std::make_pair(0u, 1.0f));
            spec.vertices.push_back(sv);
        }
        spec.faces.push_back({{0, 1, 2}});
        spec.faces.push_back({{3, 4, 5}});
        return spec;
    }
}

// ---------------------------------------------------------------------------
// bsp group
// ---------------------------------------------------------------------------

CASE(bsp, real_crossing_is_split_and_attributes_track_originals) {
    MeshSpec spec = crossingSpec();
    auto core = makeCoreSubmesh(spec);
    CalSubmesh submesh(core);
    BoneTransform palette[1] = {identityBone()};
    cal3d::TransparentInput input = singleBoneInput(&submesh, palette, 1);

    cal3d::TransparentPacket packet;
    CHECK(packet.build(&input, 1, 1e-4f, 1000));
    const auto& fragments = packet.fragments();
    CHECK(fragments.size() >= 3); // at least one source face had to be cut

    std::set<size_t> seenFaces;
    for (const auto& f : fragments) {
        CHECK(f.inputIndex == 0);
        seenFaces.insert(f.faceIndex);
        CHECK(f.hasUV);
        CHECK(f.coreMaterialThreadId == 0);
    }
    CHECK(seenFaces.size() == 2);

    // No transverse intersections may survive the split.
    for (size_t i = 0; i < fragments.size(); ++i) {
        for (size_t j = i + 1; j < fragments.size(); ++j) {
            CHECK(!transverseIntersection(fragments[i], fragments[j], 1e-4f));
        }
    }

    // Per source face: coverage, area, winding and barycentric provenance.
    for (size_t face = 0; face < 2; ++face) {
        const auto& f0 = spec.faces[face];
        const CalVector original[3] = {
            spec.vertices[f0[0]].position,
            spec.vertices[f0[1]].position,
            spec.vertices[f0[2]].position
        };
        const CalVector originalNormal = cross(
            original[1] - original[0], original[2] - original[0]);
        const float originalArea = triangleArea(original[0], original[1], original[2]);

        float coveredArea = 0.0f;
        size_t pieces = 0;
        for (const auto& fragment : fragments) {
            if (fragment.faceIndex != face) {
                continue;
            }
            ++pieces;
            const CalVector fragmentNormal = cross(
                fragment.corner[1].position - fragment.corner[0].position,
                fragment.corner[2].position - fragment.corner[0].position);
            // Winding must match the source face.
            CHECK(dot(fragmentNormal, originalNormal) > 0.0f);
            coveredArea += fragmentNormal.length() * 0.5f;

            for (int k = 0; k < 3; ++k) {
                const cal3d::TransparentCorner& c = fragment.corner[k];
                const float weightSum =
                    c.barycentric[0] + c.barycentric[1] + c.barycentric[2];
                CHECK(close(weightSum, 1.0f, 1e-6f));
                for (int w = 0; w < 3; ++w) {
                    CHECK(c.barycentric[w] >= -1e-6f);
                    CHECK(c.barycentric[w] <= 1.0f + 1e-6f);
                }
                CalVector reconstructed;
                CalVector expectedNormal(0, 0, 0);
                float expectedU = 0.0f;
                float expectedV = 0.0f;
                for (int w = 0; w < 3; ++w) {
                    const SpecVertex& sv = spec.vertices[f0[w]];
                    reconstructed = reconstructed
                        + sv.position * c.barycentric[w];
                    expectedNormal = expectedNormal
                        + sv.normal * c.barycentric[w];
                    expectedU += sv.uv[0] * c.barycentric[w];
                    expectedV += sv.uv[1] * c.barycentric[w];
                }
                CHECK(closeVec(c.position, reconstructed, 2e-5f));
                // Normals are interpolated, not renormalized.
                CHECK(closeVec(c.normal, expectedNormal, 2e-5f));
                CHECK(close(c.u, expectedU, 2e-5f));
                CHECK(close(c.v, expectedV, 2e-5f));
            }
        }
        CHECK(pieces >= 1);
        CHECK(close(coveredArea, originalArea, 1e-4f));
    }
}

CASE(bsp, three_blade_occlusion_cycle_resolves_to_drawable_pieces) {
    MeshSpec spec;
    spec.hasUV = false;
    const float angles[3] = {0.0f, 1.04719755f, 2.0943951f};
    for (int blade = 0; blade < 3; ++blade) {
        const float c = std::cos(angles[blade]);
        const float s = std::sin(angles[blade]);
        auto rotate = [&](const CalVector& p) {
            return CalVector(
                c * p.x - s * p.y,
                s * p.x + c * p.y,
                p.z);
        };
        const CalVector local[3] = {
            CalVector(-1.2f, 0.0f, -0.4f),
            CalVector(1.2f, 0.0f, -0.4f),
            CalVector(0.0f, 0.0f, 1.0f)
        };
        const CalIndex base = static_cast<CalIndex>(spec.vertices.size());
        for (int k = 0; k < 3; ++k) {
            SpecVertex sv;
            sv.position = rotate(local[k]);
            sv.normal = CalVector(0, 1, 0);
            sv.uv = {{0, 0}};
            sv.color = packColor(100 + 40 * blade, 20 * k, 200, 160);
            sv.influences.push_back(std::make_pair(0u, 1.0f));
            spec.vertices.push_back(sv);
        }
        spec.faces.push_back({{base, static_cast<CalIndex>(base + 1),
                               static_cast<CalIndex>(base + 2)}});
    }

    auto core = makeCoreSubmesh(spec);
    CalSubmesh submesh(core);
    BoneTransform palette[1] = {identityBone()};
    cal3d::TransparentInput input = singleBoneInput(&submesh, palette, 1);

    cal3d::TransparentPacket packet;
    CHECK(packet.build(&input, 1, 1e-4f, 1000));
    const auto& fragments = packet.fragments();
    // Three pairwise-intersecting blades cannot stay as three whole triangles.
    CHECK(fragments.size() > 3);

    for (size_t i = 0; i < fragments.size(); ++i) {
        for (size_t j = i + 1; j < fragments.size(); ++j) {
            CHECK(!transverseIntersection(fragments[i], fragments[j], 1e-4f));
        }
    }

    for (size_t face = 0; face < 3; ++face) {
        const auto& f0 = spec.faces[face];
        const CalVector original[3] = {
            spec.vertices[f0[0]].position,
            spec.vertices[f0[1]].position,
            spec.vertices[f0[2]].position
        };
        const CalVector originalNormal = cross(
            original[1] - original[0], original[2] - original[0]);
        const float originalArea = triangleArea(original[0], original[1], original[2]);
        float coveredArea = 0.0f;
        for (const auto& fragment : fragments) {
            if (fragment.faceIndex != face) {
                continue;
            }
            CHECK(!fragment.hasUV);
            const CalVector fragmentNormal = cross(
                fragment.corner[1].position - fragment.corner[0].position,
                fragment.corner[2].position - fragment.corner[0].position);
            CHECK(dot(fragmentNormal, originalNormal) > 0.0f);
            coveredArea += fragmentNormal.length() * 0.5f;
            for (int k = 0; k < 3; ++k) {
                CHECK(close(fragment.corner[k].u, 0.0f));
                CHECK(close(fragment.corner[k].v, 0.0f));
            }
        }
        CHECK(close(coveredArea, originalArea, 1e-4f));
    }
}

CASE(bsp, coplanar_shared_edge_and_duplicate_faces_keep_identity) {
    MeshSpec spec;
    auto add = [&](CalVector p, CalColor32 color) {
        SpecVertex sv;
        sv.position = p;
        sv.normal = CalVector(0, 0, 1);
        sv.uv = {{p.x * 0.5f, p.y * 0.5f}};
        sv.color = color;
        sv.influences.push_back(std::make_pair(0u, 1.0f));
        spec.vertices.push_back(sv);
    };
    // Two triangles sharing edge (1,0,0)-(1,1,0), all in z = 0.
    add(CalVector(0, 0, 0), packColor(10, 10, 10, 200));
    add(CalVector(1, 0, 0), packColor(20, 20, 20, 200));
    add(CalVector(1, 1, 0), packColor(30, 30, 30, 200));
    add(CalVector(0, 1, 0), packColor(40, 40, 40, 200));
    // Duplicate of face 0 with its own color at identical positions.
    add(CalVector(0, 0, 0), packColor(200, 0, 0, 200));
    add(CalVector(1, 0, 0), packColor(0, 200, 0, 200));
    add(CalVector(1, 1, 0), packColor(0, 0, 200, 200));
    spec.faces.push_back({{0, 1, 2}});
    spec.faces.push_back({{0, 2, 3}});
    spec.faces.push_back({{4, 5, 6}});

    auto core = makeCoreSubmesh(spec);
    CalSubmesh submesh(core);
    BoneTransform palette[1] = {identityBone()};
    cal3d::TransparentInput input = singleBoneInput(&submesh, palette, 1);

    cal3d::TransparentPacket packet;
    CHECK(packet.build(&input, 1, 1e-4f, 1000));
    const auto& fragments = packet.fragments();
    // Coplanar geometry must never be cut: three faces, three fragments.
    CHECK(fragments.size() == 3);

    const auto& duplicate = fragments[2];
    const auto& original = fragments[0];
    for (int k = 0; k < 3; ++k) {
        CHECK(closeVec(duplicate.corner[k].position,
                       original.corner[k].position));
        CHECK(duplicate.corner[k].r != original.corner[k].r
              || duplicate.corner[k].g != original.corner[k].g
              || duplicate.corner[k].b != original.corner[k].b);
    }

    std::vector<size_t> order;
    CHECK(packet.order(CalVector(0.2f, 0.2f, 5.0f), order));
    CHECK(order.size() == 3);
    // Coplanar fragments draw in inputIndex, faceIndex order.
    CHECK(fragments[order[0]].faceIndex == 0);
    CHECK(fragments[order[1]].faceIndex == 1);
    CHECK(fragments[order[2]].faceIndex == 2);

    // A folded pair sharing an edge must also survive without splitting.
    MeshSpec folded;
    for (int i = 0; i < 4; ++i) {
        SpecVertex sv;
        sv.position = CalVector(i < 2 ? 0.0f : 1.0f, i % 2, i == 2 ? 1.0f : 0.0f);
        sv.normal = CalVector(0, 0, 1);
        sv.uv = {{0, 0}};
        sv.color = packColor(10, 20, 30, 128);
        sv.influences.push_back(std::make_pair(0u, 1.0f));
        folded.vertices.push_back(sv);
    }
    folded.faces.push_back({{0, 1, 2}});
    folded.faces.push_back({{0, 2, 3}});
    auto foldedCore = makeCoreSubmesh(folded);
    CalSubmesh foldedSubmesh(foldedCore);
    cal3d::TransparentInput foldedInput =
        singleBoneInput(&foldedSubmesh, palette, 1);
    cal3d::TransparentPacket foldedPacket;
    CHECK(foldedPacket.build(&foldedInput, 1, 1e-4f, 1000));
    CHECK(foldedPacket.fragments().size() == 2);
}

CASE(bsp, zero_area_faces_are_ignored_and_empty_scene_is_valid) {
    MeshSpec spec;
    for (int i = 0; i < 4; ++i) {
        SpecVertex sv;
        sv.position = CalVector(i, 0, 0); // all collinear
        sv.normal = CalVector(0, 0, 1);
        sv.uv = {{0, 0}};
        sv.color = packColor(1, 2, 3, 4);
        sv.influences.push_back(std::make_pair(0u, 1.0f));
        spec.vertices.push_back(sv);
    }
    spec.faces.push_back({{0, 1, 2}}); // zero area
    auto core = makeCoreSubmesh(spec);
    CalSubmesh submesh(core);
    BoneTransform palette[1] = {identityBone()};
    cal3d::TransparentInput input = singleBoneInput(&submesh, palette, 1);

    cal3d::TransparentPacket packet;
    CHECK(packet.build(&input, 1, 1e-4f, 1000));
    CHECK(packet.fragments().empty());
    std::vector<size_t> order;
    CHECK(packet.order(CalVector(0, 0, 5), order));
    CHECK(order.empty());

    cal3d::TransparentPacket emptyPacket;
    CHECK(emptyPacket.build(0, 0, 1e-4f, 1000));
    CHECK(emptyPacket.fragments().empty());
    CHECK(emptyPacket.order(CalVector(1, 2, 3), order));
    CHECK(order.empty());
}

// ---------------------------------------------------------------------------
// rays group
// ---------------------------------------------------------------------------

CASE(rays, fragment_order_composites_like_unsplit_depth_sorting) {
    MeshSpec spec = crossingSpec();
    auto core = makeCoreSubmesh(spec);
    CalSubmesh submesh(core);
    BoneTransform palette[1] = {identityBone()};
    cal3d::TransparentInput input = singleBoneInput(&submesh, palette, 1);

    const float epsilon = 1e-4f;
    cal3d::TransparentPacket packet;
    CHECK(packet.build(&input, 1, epsilon, 1000));
    const auto& fragments = packet.fragments();
    CHECK(fragments.size() >= 3);

    // A cut corner carries at least two positive barycentric weights (an
    // original corner has exactly one unit weight) and asymmetrically
    // interpolated UV, color and unnormalized normal.
    bool foundCut = false;
    for (const auto& fragment : fragments) {
        for (int k = 0; k < 3 && !foundCut; ++k) {
            const cal3d::TransparentCorner& c = fragment.corner[k];
            int positiveWeights = 0;
            for (int w = 0; w < 3; ++w) {
                if (c.barycentric[w] > 1e-4f) {
                    ++positiveWeights;
                }
            }
            if (positiveWeights < 2) {
                continue; // original vertex, not a cut point
            }
            foundCut = true;
            const auto& f0 = spec.faces[fragment.faceIndex];
            CalVector expectedNormal(0, 0, 0);
            Rgba expectedColor{0, 0, 0, 0};
            float expectedU = 0.0f;
            float expectedV = 0.0f;
            const CalColor32 rawColors[3] = {
                spec.vertices[f0[0]].color,
                spec.vertices[f0[1]].color,
                spec.vertices[f0[2]].color
            };
            for (int w = 0; w < 3; ++w) {
                const SpecVertex& sv = spec.vertices[f0[w]];
                expectedNormal = expectedNormal + sv.normal * c.barycentric[w];
                expectedU += sv.uv[0] * c.barycentric[w];
                expectedV += sv.uv[1] * c.barycentric[w];
                const CalVector4 rgba = CalVector4FromColor(rawColors[w]);
                expectedColor.r += rgba.x * c.barycentric[w];
                expectedColor.g += rgba.y * c.barycentric[w];
                expectedColor.b += rgba.z * c.barycentric[w];
                expectedColor.a += rgba.w * c.barycentric[w];
            }
            CHECK(closeVec(c.normal, expectedNormal, 2e-5f));
            CHECK(close(c.u, expectedU, 2e-5f));
            CHECK(close(c.v, expectedV, 2e-5f));
            CHECK(close(c.r, expectedColor.r, 2e-5f));
            CHECK(close(c.g, expectedColor.g, 2e-5f));
            CHECK(close(c.b, expectedColor.b, 2e-5f));
            CHECK(close(c.a, expectedColor.a, 2e-5f));
            // Asymmetric: the cut matches neither source vertex.
            CHECK(!close(c.u, spec.vertices[f0[0]].uv[0], 1e-3f)
                  || !close(c.v, spec.vertices[f0[0]].uv[1], 1e-3f));
            // Normals are interpolated without normalization: when the
            // interpolated source normal is not unit length, the stored one
            // must keep that same non-unit length.
            if (std::fabs(expectedNormal.length() - 1.0f) > 2e-3f) {
                CHECK(std::fabs(c.normal.length() - expectedNormal.length())
                      < 2e-4f);
                CHECK(std::fabs(c.normal.length() - 1.0f) > 1e-3f);
            }
        }
    }
    CHECK(foundCut);

    const CalVector eyes[5] = {
        CalVector(2.5f, -2.0f, 3.0f),
        CalVector(-2.5f, 1.5f, 2.0f),
        CalVector(0.5f, 2.5f, -3.0f),
        CalVector(-2.0f, -2.5f, -2.0f),
        CalVector(2.0f, 2.0f, 2.0f)
    };

    // All edges, including the BSP cut edges: samples must keep clear of them.
    std::vector<std::pair<CalVector, CalVector>> protectedEdges;
    for (const auto& fragment : fragments) {
        protectedEdges.push_back({
            fragment.corner[0].position, fragment.corner[1].position});
        protectedEdges.push_back({
            fragment.corner[1].position, fragment.corner[2].position});
        protectedEdges.push_back({
            fragment.corner[2].position, fragment.corner[0].position});
    }

    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> jitter(-0.35f, 0.35f);
    int acceptedRays = 0;

    for (const CalVector& eye : eyes) {
        std::vector<size_t> order;
        CHECK(packet.order(eye, order));
        CHECK(order.size() == fragments.size());
        std::set<size_t> uniqueOrder(order.begin(), order.end());
        CHECK(uniqueOrder.size() == order.size());

        for (int gy = 0; gy < 24; ++gy) {
            for (int gx = 0; gx < 24; ++gx) {
                const CalVector target(
                    -0.85f + 1.7f * gx / 23.0f + jitter(rng) * 0.02f,
                    -0.85f + 1.7f * gy / 23.0f + jitter(rng) * 0.02f,
                    jitter(rng) * 0.02f);
                const CalVector direction = target - eye;

                struct Hit {
                    float t;
                    Rgba color;
                    CalVector point;
                };
                std::vector<Hit> referenceHits;
                for (size_t face = 0; face < spec.faces.size(); ++face) {
                    const auto& f0 = spec.faces[face];
                    const CalVector a = spec.vertices[f0[0]].position;
                    const CalVector b = spec.vertices[f0[1]].position;
                    const CalVector c = spec.vertices[f0[2]].position;
                    float t, u, v;
                    if (!rayTriangle(eye, direction, a, b, c, t, u, v)) {
                        continue;
                    }
                    const float w[3] = {1.0f - u - v, u, v};
                    Rgba color{0, 0, 0, 0};
                    CalVector point(0, 0, 0);
                    for (int k = 0; k < 3; ++k) {
                        const CalVector4 rgba =
                            CalVector4FromColor(spec.vertices[f0[k]].color);
                        color.r += w[k] * rgba.x;
                        color.g += w[k] * rgba.y;
                        color.b += w[k] * rgba.z;
                        color.a += w[k] * rgba.w;
                        point = point + (k == 0 ? a : (k == 1 ? b : c)) * w[k];
                    }
                    referenceHits.push_back(Hit{t, color, point});
                }
                if (referenceHits.size() < 2) {
                    continue; // ordering only matters where surfaces overlap
                }

                bool awayFromEdges = true;
                for (const Hit& hit : referenceHits) {
                    for (const auto& edge : protectedEdges) {
                        if (pointEdgeDistance(hit.point, edge.first, edge.second)
                            < 0.08f) {
                            awayFromEdges = false;
                        }
                    }
                }
                if (!awayFromEdges) {
                    continue;
                }

                // Reference: independently sort the unsplit triangle hits
                // far to near, then source-over.
                std::sort(referenceHits.begin(), referenceHits.end(),
                    [](const Hit& x, const Hit& y) { return x.t > y.t; });
                Rgba reference{0, 0, 0, 0};
                for (const Hit& hit : referenceHits) {
                    reference = sourceOver(hit.color, reference);
                }

                // Packet path: trust order() completely, no depth re-sort.
                Rgba painted{0, 0, 0, 0};
                int paintedHits = 0;
                for (size_t index : order) {
                    const auto& fragment = fragments[index];
                    const CalVector a = fragment.corner[0].position;
                    const CalVector b = fragment.corner[1].position;
                    const CalVector c = fragment.corner[2].position;
                    float t, u, v;
                    if (!rayTriangle(eye, direction, a, b, c, t, u, v)) {
                        continue;
                    }
                    painted = sourceOver(interpolateRgba(fragment, u, v), painted);
                    ++paintedHits;
                }
                if (paintedHits != static_cast<int>(referenceHits.size())) {
                    continue; // sample grazing a cut boundary
                }
                CHECK(close(painted.r, reference.r, 2e-3f));
                CHECK(close(painted.g, reference.g, 2e-3f));
                CHECK(close(painted.b, reference.b, 2e-3f));
                CHECK(close(painted.a, reference.a, 2e-3f));
                ++acceptedRays;
            }
        }
    }
    CHECK(acceptedRays >= 40);
}

CASE(rays, eye_on_split_plane_goes_negative_side_first) {
    MeshSpec spec = crossingSpec();
    auto core = makeCoreSubmesh(spec);
    CalSubmesh submesh(core);
    BoneTransform palette[1] = {identityBone()};
    cal3d::TransparentInput input = singleBoneInput(&submesh, palette, 1);

    cal3d::TransparentPacket packet;
    CHECK(packet.build(&input, 1, 1e-4f, 1000));

    // Face 0 lies in the plane through the origin with normal
    // cross(v1-v0, v2-v0).  Put the eye exactly in that plane.
    const CalVector a = spec.vertices[0].position;
    const CalVector b = spec.vertices[1].position;
    const CalVector c = spec.vertices[2].position;
    const CalVector normal = cross(b - a, c - a);
    CalVector inPlane = cross(normal, CalVector(0, 1, 0));
    if (inPlane.lengthSquared() == 0.0f) {
        inPlane = cross(normal, CalVector(1, 0, 0));
    }
    inPlane.normalize();
    const CalVector eye = inPlane * 4.0f;
    CHECK(close(dot(normal, eye - a), 0.0f, 1e-4f));

    std::vector<size_t> order;
    CHECK(packet.order(eye, order));
    CHECK(order.size() == packet.fragments().size());

    // The first fragment must come from the negative-side subtree of the
    // root plane, i.e. a face-1 piece sitting on the negative side.
    const auto& first = packet.fragments()[order.front()];
    bool negativePiece = false;
    for (int k = 0; k < 3; ++k) {
        if (dot(normal, first.corner[k].position - a) < -1e-4f) {
            negativePiece = true;
        }
    }
    CHECK(negativePiece);

    // Non-finite eyes are rejected and leave the output untouched.
    std::vector<size_t> preserved = order;
    CalVector badEye(std::numeric_limits<float>::quiet_NaN(), 0, 0);
    CHECK(!packet.order(badEye, order));
    CHECK(order == preserved);
}

// ---------------------------------------------------------------------------
// animation group
// ---------------------------------------------------------------------------

namespace {
    struct AnimatedRig {
        ISTD::shared_ptr<CalCoreSkeleton> coreSkeleton;
        ISTD::shared_ptr<CalSkeleton> skeleton;
        ISTD::shared_ptr<CalCoreAnimation> coreAnimation;
        ISTD::shared_ptr<CalAnimation> animation;
        CalMixer mixer;
    };

    AnimatedRig makeForkRig() {
        AnimatedRig rig;
        std::vector<ISTD::shared_ptr<CalCoreBone>> coreBones;
        auto root = ISTD::make_shared<CalCoreBone>("root", -1);
        auto left = ISTD::make_shared<CalCoreBone>("left", 0);
        auto right = ISTD::make_shared<CalCoreBone>("right", 0);
        coreBones.push_back(root);
        coreBones.push_back(left);
        coreBones.push_back(right);
        rig.coreSkeleton = ISTD::make_shared<CalCoreSkeleton>(coreBones);

        rig.skeleton = ISTD::make_shared<CalSkeleton>(rig.coreSkeleton);

        rig.coreAnimation = ISTD::make_shared<CalCoreAnimation>();
        rig.coreAnimation->duration = 1.0f;
        for (unsigned bone = 0; bone < 3; ++bone) {
            CalCoreTrack::KeyframeList keyframes;
            const float spread0 = bone == 1 ? 0.15f : (bone == 2 ? -0.15f : 0.0f);
            const float spread1 = bone == 1 ? 0.32f : (bone == 2 ? -0.32f : 0.0f);
            CalQuaternion quat;
            quat.setAxisAngle(
                CalVector(0, 1, 0),
                bone == 1 ? 0.15f : (bone == 2 ? -0.15f : 0.0f));
            keyframes.push_back(CalCoreKeyframe(
                0.0f, CalVector(spread0, 0, 0), CalQuaternion()));
            keyframes.push_back(CalCoreKeyframe(
                1.0f, CalVector(spread1, 0, 0), quat));
            rig.coreAnimation->tracks.push_back(
                CalCoreTrack(static_cast<int>(bone), keyframes));
        }
        rig.animation = ISTD::make_shared<CalAnimation>(
            rig.coreAnimation, 1.0f, 0);
        rig.mixer.addAnimation(rig.animation);
        return rig;
    }

    void poseAt(AnimatedRig& rig, float time) {
        rig.animation->time = time;
        rig.mixer.updateSkeleton(rig.skeleton.get(),
            std::vector<BoneTransformAdjustment>(),
            std::vector<BoneScaleAdjustment>());
    }

    // A four-vertex cloth patch, two triangles, with two-bone weights and a
    // sparse morph touching only two of the vertices.
    ISTD::shared_ptr<CalCoreSubmesh> makeClothCore() {
        MeshSpec spec;
        const CalVector positions[4] = {
            CalVector(-1, -1, 0), CalVector(1, -1, 0),
            CalVector(1, 1, 0), CalVector(-1, 1, 0)
        };
        const CalColor32 colors[4] = {
            packColor(255, 40, 40, 110), packColor(40, 255, 40, 110),
            packColor(40, 40, 255, 110), packColor(255, 255, 40, 110)
        };
        const float weights[4][2] = {
            {0.4f, 0.6f}, {0.6f, 0.4f}, {0.5f, 0.5f}, {0.3f, 0.7f}
        };
        for (int i = 0; i < 4; ++i) {
            SpecVertex sv;
            sv.position = positions[i];
            sv.normal = CalVector(0, 0, 1);
            sv.uv = {{0.25f * i, 0.5f - 0.1f * i}};
            sv.color = colors[i];
            sv.influences.push_back(std::make_pair(1u, weights[i][0]));
            sv.influences.push_back(std::make_pair(2u, weights[i][1]));
            spec.vertices.push_back(sv);
        }
        spec.faces.push_back({{0, 1, 2}});
        spec.faces.push_back({{0, 2, 3}});
        spec.materialThreadId = 7;
        return makeCoreSubmesh(spec);
    }
}

CASE(animation, two_frames_two_crossing_instances_cameras_rebuild_release) {
    AnimatedRig rig = makeForkRig();
    auto core = makeClothCore();

    CalCoreMorphTarget::VertexOffsetArray offsets;
    offsets.push_back(VertexOffset(
        0, CalPoint4(0.0f, 0.0f, 0.4f), CalVector4(0, 0, 0)));
    offsets.push_back(VertexOffset(
        2, CalPoint4(0.0f, 0.0f, -0.3f), CalVector4(0, 0, 0)));
    core->addMorphTarget(ISTD::make_shared<CalCoreMorphTarget>(
        "sparse", 4, offsets));

    auto submesh = ISTD::make_shared<CalSubmesh>(core);
    submesh->setMorphTargetWeight("sparse", 0.25f);

    cal3d::RotateTranslate rotatedObject;
    rotatedObject.rotation.setAxisAngle(
        CalVector(0, 1, 0), 1.5707963f);
    rotatedObject.translation = CalVector(0, 0, 0);

    auto buildForFrame = [&](float time, cal3d::TransparentPacket& packet) {
        poseAt(rig, time);
        const BoneTransform* palette = &rig.skeleton->boneTransforms[0];
        const size_t boneCount = rig.skeleton->boneTransforms.size();
        CHECK(boneCount == 3);

        cal3d::TransparentInput inputs[2];
        inputs[0] = singleBoneInput(submesh.get(), palette, boneCount,
                                    identityObject());
        inputs[1] = singleBoneInput(submesh.get(), palette, boneCount,
                                    rotatedObject);
        return packet.build(inputs, 2, 1e-4f, 500);
    };

    cal3d::TransparentPacket frame0;
    CHECK(buildForFrame(0.0f, frame0));
    CHECK(frame0.fragments().size() >= 4);
    std::set<size_t> frameInputs;
    for (const auto& f : frame0.fragments()) {
        frameInputs.insert(f.inputIndex);
        CHECK(f.coreMaterialThreadId == 7);
        for (int k = 0; k < 3; ++k) {
            const float sum = f.corner[k].barycentric[0]
                + f.corner[k].barycentric[1]
                + f.corner[k].barycentric[2];
            CHECK(close(sum, 1.0f, 1e-6f));
        }
    }
    CHECK(frameInputs.size() == 2); // both crossing instances present

    // The rotated instance genuinely crosses the identity one.
    bool hasSplit = frame0.fragments().size() > 4;
    CHECK(hasSplit);

    // Multiple cameras reuse the same tree without rebuilding.
    std::vector<size_t> orderA;
    std::vector<size_t> orderB;
    CHECK(frame0.order(CalVector(3, 2, 4), orderA));
    CHECK(frame0.order(CalVector(-3, -2, -4), orderB));
    CHECK(orderA.size() == frame0.fragments().size());
    CHECK(orderB.size() == frame0.fragments().size());
    CHECK(orderA != orderB);

    // Keep a private copy of a fragment before the next build.
    const cal3d::TransparentFragment kept = frame0.fragments()[0];

    // Second frame: different pose and morph weight produce new geometry.
    submesh->setMorphTargetWeight("sparse", 0.5f);
    cal3d::TransparentPacket frame1;
    CHECK(buildForFrame(1.0f, frame1));
    CHECK(frame1.fragments().size() >= 4);
    bool geometryChanged = false;
    for (const auto& f : frame1.fragments()) {
        if (!closeVec(f.corner[0].position, kept.corner[0].position, 1e-4f)) {
            geometryChanged = true;
        }
    }
    CHECK(geometryChanged);

    // The caller's copy from the previous successful packet is untouched.
    CHECK(closeVec(kept.corner[0].position,
                   frame0.fragments()[0].corner[0].position));
    CHECK(close(kept.corner[1].r, frame0.fragments()[0].corner[1].r));

    // Release every source object, then keep using the built packet.
    const size_t builtCount = frame1.fragments().size();
    const CalVector rememberedPosition =
        frame1.fragments()[0].corner[0].position;
    std::vector<size_t> orderBeforeRelease;
    CHECK(frame1.order(CalVector(1, 2, 3), orderBeforeRelease));
    submesh.reset();
    core.reset();
    std::vector<BoneTransform> detachedPalette(
        rig.skeleton->boneTransforms.size());
    for (size_t i = 0; i < detachedPalette.size(); ++i) {
        detachedPalette[i] = rig.skeleton->boneTransforms[i];
    }
    rig.skeleton.reset();
    rig.coreSkeleton.reset();
    rig.coreAnimation.reset();
    rig.animation.reset();

    std::vector<size_t> orderAfterRelease;
    CHECK(frame1.order(CalVector(1, 2, 3), orderAfterRelease));
    CHECK(orderAfterRelease == orderBeforeRelease);
    CHECK(frame1.fragments().size() == builtCount);
    CHECK(closeVec(frame1.fragments()[0].corner[0].position,
                   rememberedPosition));
}

CASE(animation, failed_builds_and_cap_preserve_last_good_packet) {
    AnimatedRig rig = makeForkRig();
    auto core = makeClothCore();
    auto submesh = ISTD::make_shared<CalSubmesh>(core);

    cal3d::RotateTranslate rotatedObject;
    rotatedObject.rotation.setAxisAngle(CalVector(0, 1, 0), 1.5707963f);

    poseAt(rig, 0.0f);
    const BoneTransform* palette = &rig.skeleton->boneTransforms[0];
    const size_t boneCount = rig.skeleton->boneTransforms.size();

    cal3d::TransparentInput inputs[2];
    inputs[0] = singleBoneInput(submesh.get(), palette, boneCount,
                                identityObject());
    inputs[1] = singleBoneInput(submesh.get(), palette, boneCount,
                                rotatedObject);

    cal3d::TransparentPacket packet;
    CHECK(packet.build(inputs, 2, 1e-4f, 500));
    const size_t goodCount = packet.fragments().size();
    CHECK(goodCount > 0);
    std::vector<size_t> goodOrder;
    CHECK(packet.order(CalVector(2, 3, 4), goodOrder));

    auto expectFailure = [&](const cal3d::TransparentInput* badInputs,
                             size_t badCount,
                             float badEpsilon,
                             size_t badCap) {
        const size_t before = packet.fragments().size();
        CHECK(!packet.build(badInputs, badCount, badEpsilon, badCap));
        CHECK(packet.fragments().size() == before);
        std::vector<size_t> afterOrder;
        CHECK(packet.order(CalVector(2, 3, 4), afterOrder));
        CHECK(afterOrder == goodOrder);
    };

    expectFailure(inputs, 2, 0.0f, 500);          // non-positive epsilon
    expectFailure(inputs, 2, -1.0f, 500);
    expectFailure(inputs, 2, 1e-4f, 0);           // zero cap
    expectFailure(inputs, 2,
        std::numeric_limits<float>::infinity(), 500); // non-finite epsilon
    CalVector nan(std::numeric_limits<float>::quiet_NaN(), 0, 0);
    cal3d::TransparentInput badTransform = inputs[0];
    badTransform.objectToWorld.translation = nan;
    expectFailure(&badTransform, 1, 1e-4f, 500);  // non-finite transform

    // Missing palette with a real submesh is rejected; an empty input set
    // remains a legal empty scene (checked on a separate packet so the last
    // good packet above is not replaced).
    cal3d::TransparentInput missingPalette = inputs[0];
    missingPalette.bones = 0;
    expectFailure(&missingPalette, 1, 1e-4f, 500);
    cal3d::TransparentPacket emptyPacket;
    CHECK(emptyPacket.build(0, 0, 1e-4f, 500));
    CHECK(emptyPacket.fragments().empty());

    // Invalid bone index inside the mesh.
    MeshSpec badSpec;
    for (int i = 0; i < 3; ++i) {
        SpecVertex sv;
        sv.position = CalVector(i, 0, 0);
        sv.normal = CalVector(0, 0, 1);
        sv.uv = {{0, 0}};
        sv.color = packColor(1, 2, 3, 4);
        sv.influences.push_back(std::make_pair(99u, 1.0f));
        badSpec.vertices.push_back(sv);
    }
    badSpec.faces.push_back({{0, 1, 2}});
    auto badCore = makeCoreSubmesh(badSpec);
    CalSubmesh badSubmesh(badCore);
    cal3d::TransparentInput badBoneInput =
        singleBoneInput(&badSubmesh, palette, boneCount);
    expectFailure(&badBoneInput, 1, 1e-4f, 500);

    // Out of range face index.
    MeshSpec badFaceSpec;
    for (int i = 0; i < 3; ++i) {
        SpecVertex sv;
        sv.position = CalVector(i, i, 0);
        sv.normal = CalVector(0, 0, 1);
        sv.uv = {{0, 0}};
        sv.color = packColor(1, 2, 3, 4);
        sv.influences.push_back(std::make_pair(0u, 1.0f));
        badFaceSpec.vertices.push_back(sv);
    }
    badFaceSpec.faces.push_back({{0, 1, 5}});
    auto badFaceCore = makeCoreSubmesh(badFaceSpec);
    CalSubmesh badFaceSubmesh(badFaceCore);
    cal3d::TransparentInput badFaceInput =
        singleBoneInput(&badFaceSubmesh, palette, boneCount);
    expectFailure(&badFaceInput, 1, 1e-4f, 500);

    // Hard cap: exactly the required count succeeds, one less fails.
    cal3d::TransparentPacket capped;
    CHECK(capped.build(inputs, 2, 1e-4f, goodCount));
    CHECK(capped.fragments().size() == goodCount);
    cal3d::TransparentPacket underCapped;
    CHECK(!underCapped.build(inputs, 2, 1e-4f, goodCount - 1));
    CHECK(underCapped.fragments().empty());
    CHECK(!capped.build(inputs, 2, 1e-4f, goodCount - 1));
    // The previously successful capped packet stays intact.
    CHECK(capped.fragments().size() == goodCount);
}

CASE(animation, invalid_inputs_are_rejected_and_source_data_is_untouched) {
    AnimatedRig rig = makeForkRig();
    poseAt(rig, 0.0f);
    const BoneTransform* palette = &rig.skeleton->boneTransforms[0];
    const size_t boneCount = rig.skeleton->boneTransforms.size();

    // Non-unit quaternion: an unsupported transform.
    auto goodCore = makeClothCore();
    CalSubmesh goodSubmesh(goodCore);
    cal3d::TransparentInput goodInput =
        singleBoneInput(&goodSubmesh, palette, boneCount);
    cal3d::TransparentInput scaledInput = goodInput;
    scaledInput.objectToWorld.rotation = CalQuaternion(0, 0, 0, 2.0f);
    cal3d::TransparentPacket packet;
    CHECK(!packet.build(&scaledInput, 1, 1e-4f, 1000));
    CHECK(packet.fragments().empty());

    // A successful build first, so later failures must preserve it.
    CHECK(packet.build(&goodInput, 1, 1e-4f, 1000));
    const size_t goodFragments = packet.fragments().size();

    // Empty submesh (no vertices/faces) is rejected, not treated as empty scene.
    auto emptyCore = ISTD::make_shared<CalCoreSubmesh>(0, false, 0);
    CalSubmesh emptySubmesh(emptyCore);
    cal3d::TransparentInput emptyInput =
        singleBoneInput(&emptySubmesh, palette, boneCount);
    CHECK(!packet.build(&emptyInput, 1, 1e-4f, 1000));
    CHECK(packet.fragments().size() == goodFragments);

    // Negative and non-normalized bone weights are rejected.
    MeshSpec badWeights;
    for (int i = 0; i < 3; ++i) {
        SpecVertex sv;
        sv.position = CalVector(i, 0, 0);
        sv.normal = CalVector(0, 0, 1);
        sv.uv = {{0, 0}};
        sv.color = packColor(1, 2, 3, 4);
        sv.influences.push_back(std::make_pair(0u, i == 0 ? -0.1f : 0.4f));
        sv.influences.push_back(std::make_pair(1u, i == 0 ? 1.1f : 0.6f));
        badWeights.vertices.push_back(sv);
    }
    badWeights.faces.push_back({{0, 1, 2}});
    auto badWeightCore = makeCoreSubmesh(badWeights);
    CalSubmesh badWeightSubmesh(badWeightCore);
    cal3d::TransparentInput badWeightInput =
        singleBoneInput(&badWeightSubmesh, palette, boneCount);
    CHECK(!packet.build(&badWeightInput, 1, 1e-4f, 1000));
    CHECK(packet.fragments().size() == goodFragments);

    // Non-finite morph weight is rejected.
    auto morphCore = makeClothCore();
    CalCoreMorphTarget::VertexOffsetArray offsets;
    offsets.push_back(VertexOffset(
        0, CalPoint4(0, 0, 0.2f), CalVector4(0, 0, 0)));
    morphCore->addMorphTarget(ISTD::make_shared<CalCoreMorphTarget>(
        "m", 4, offsets));
    CalSubmesh morphSubmesh(morphCore);
    morphSubmesh.setMorphTargetWeight(
        "m", std::numeric_limits<float>::infinity());
    cal3d::TransparentInput morphInput =
        singleBoneInput(&morphSubmesh, palette, boneCount);
    CHECK(!packet.build(&morphInput, 1, 1e-4f, 1000));
    CHECK(packet.fragments().size() == goodFragments);

    // Building never mutates source faces, base vertices or morph offsets.
    const CalVector savedPositions[4] = {
        goodCore->getVectorVertex()[0].position.asCalVector(),
        goodCore->getVectorVertex()[1].position.asCalVector(),
        goodCore->getVectorVertex()[2].position.asCalVector(),
        goodCore->getVectorVertex()[3].position.asCalVector()
    };
    const size_t savedFaceCount = goodCore->getFaceCount();
    CHECK(packet.build(&goodInput, 1, 1e-4f, 1000));
    CHECK(goodCore->getFaceCount() == savedFaceCount);
    for (int i = 0; i < 4; ++i) {
        CHECK(closeVec(goodCore->getVectorVertex()[i].position.asCalVector(),
                       savedPositions[i]));
    }
    CHECK(goodSubmesh.morphTargets.empty()
          || goodSubmesh.morphTargets[0].weight == 0.0f);
}

CASE(animation, same_submesh_reused_with_different_palettes_and_transforms) {
    AnimatedRig rig = makeForkRig();
    auto core = makeClothCore();
    auto submesh = ISTD::make_shared<CalSubmesh>(core);

    poseAt(rig, 0.5f);
    const size_t boneCount = rig.skeleton->boneTransforms.size();

    // A second, deliberately different palette for the same submesh.
    std::vector<BoneTransform> shifted(boneCount);
    for (size_t i = 0; i < boneCount; ++i) {
        shifted[i] = rig.skeleton->boneTransforms[i];
    }
    shifted[1].rowx.w += 0.7f;

    cal3d::RotateTranslate translated;
    translated.translation = CalVector(0, 0, 2.5f);

    cal3d::TransparentInput inputs[3];
    inputs[0] = singleBoneInput(submesh.get(),
        &rig.skeleton->boneTransforms[0], boneCount, identityObject());
    inputs[1] = singleBoneInput(submesh.get(), shifted.data(), boneCount,
                                translated);
    cal3d::RotateTranslate tilted;
    tilted.rotation.setAxisAngle(CalVector(1, 0, 0), 0.6f);
    inputs[2] = singleBoneInput(submesh.get(),
        &rig.skeleton->boneTransforms[0], boneCount, tilted);

    cal3d::TransparentPacket packet;
    CHECK(packet.build(inputs, 3, 1e-4f, 1000));
    std::set<size_t> seenInputs;
    std::set<int> materialThreads;
    for (const auto& f : packet.fragments()) {
        seenInputs.insert(f.inputIndex);
        materialThreads.insert(f.coreMaterialThreadId);
    }
    CHECK(seenInputs.size() == 3);
    CHECK(materialThreads.size() == 1);

    // The translated instance's fragments really are two and a half units up.
    for (const auto& f : packet.fragments()) {
        if (f.inputIndex != 1) {
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            CHECK(close(f.corner[k].position.z, 2.5f, 0.2f));
        }
    }

    // Ordering from several eyes stays a complete, valid permutation.
    const CalVector eyes[3] = {
        CalVector(4, 4, 6), CalVector(-4, 0, -4), CalVector(0, -5, 1)
    };
    for (const CalVector& eye : eyes) {
        std::vector<size_t> order;
        CHECK(packet.order(eye, order));
        CHECK(order.size() == packet.fragments().size());
        std::set<size_t> unique(order.begin(), order.end());
        CHECK(unique.size() == order.size());
    }
}
