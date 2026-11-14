/*
 * Regression tests for the headless CPU clipped mesh pipeline.
 *
 * Every armature is generated programmatically from DragonBones JSON and fed
 * through the real runtime (parsing, armature building, advanceTime bone and
 * FFD animation). All expected geometry is then recomputed independently with
 * double precision helpers in this file instead of being read back from the
 * runtime.
 */
#include "headless.h"
#include "dragonBones/armature/CpuClippedMesh.h"
#include "check.h"

#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace dragonBones;

namespace
{
    const double EPS = 1e-5;
    const double EPS_AREA = 2e-4;

    struct V2
    {
        double x, y;
        V2 operator+(const V2& o) const { return {x + o.x, y + o.y}; }
        V2 operator-(const V2& o) const { return {x - o.x, y - o.y}; }
        V2 operator*(double k) const { return {x * k, y * k}; }
    };

    inline double cross(const V2& a, const V2& b)
    {
        return a.x * b.y - a.y * b.x;
    }

    inline double area2(const V2& a, const V2& b, const V2& c)
    {
        return cross(b - a, c - a);
    }

    // Independent convex polygon clipping (Sutherland-Hodgman, double precision).
    std::vector<V2> clipPolygon(std::vector<V2> polygon, const std::vector<V2>& convex)
    {
        const auto n = convex.size();
        V2 centroid{0.0, 0.0};
        for (const auto& p : convex) centroid = centroid + p;
        centroid = centroid * (1.0 / static_cast<double>(n));

        for (std::size_t i = 0; i < n; ++i)
        {
            const auto a = convex[i];
            const V2 b = convex[(i + 1) % n];
            V2 normal{-(b.y - a.y), b.x - a.x};
            if (normal.x * (centroid.x - a.x) + normal.y * (centroid.y - a.y) < 0.0)
            {
                normal = normal * -1.0;
            }
            const auto inside = [&](const V2& p)
            {
                return normal.x * (p.x - a.x) + normal.y * (p.y - a.y) >= -1e-9;
            };

            std::vector<V2> output;
            if (polygon.empty())
            {
                return output;
            }
            auto s = polygon.back();
            for (const auto& e : polygon)
            {
                const auto dS = normal.x * (s.x - a.x) + normal.y * (s.y - a.y);
                const auto dE = normal.x * (e.x - a.x) + normal.y * (e.y - a.y);
                const auto inS = dS >= -1e-9;
                const auto inE = dE >= -1e-9;
                if (inE)
                {
                    if (!inS) output.push_back(s + (e - s) * (dS / (dS - dE)));
                    output.push_back(e);
                }
                else if (inS)
                {
                    output.push_back(s + (e - s) * (dS / (dS - dE)));
                }
                s = e;
            }
            polygon = std::move(output);
        }
        return polygon;
    }

    double polygonArea(const std::vector<V2>& polygon)
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < polygon.size(); ++i)
        {
            const auto& a = polygon[i];
            const auto& b = polygon[(i + 1) % polygon.size()];
            sum += a.x * b.y - b.x * a.y;
        }
        return std::fabs(sum) * 0.5;
    }

    // Area of one source triangle after clipping against every active mask.
    double expectedTriangleArea(const std::vector<V2>& triangle, const std::vector<std::vector<V2>>& masks)
    {
        auto polygon = triangle;
        for (const auto& mask : masks)
        {
            polygon = clipPolygon(polygon, mask);
            if (polygon.size() < 3) return 0.0;
        }
        return polygonArea(polygon);
    }

    bool insideConvex(const V2& p, const std::vector<V2>& convex)
    {
        const auto n = convex.size();
        double winding = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            winding += cross(convex[i], convex[(i + 1) % n]);
        }
        // Scale the boundary tolerance with the polygon size so coordinates on
        // an edge are still accepted.
        const auto tolerance = 1e-5 * (1.0 + std::fabs(winding));
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto a = convex[i];
            const auto b = convex[(i + 1) % n];
            const auto side = cross(b - a, p - a);
            if (winding * side < -tolerance)
            {
                return false;
            }
        }
        return true;
    }

    // Analytic transforms used by the hand built fixtures (all rigid bodies).
    V2 apply(double tx, double ty, double rotationDeg, double sx, double sy, const V2& p)
    {
        const auto r = rotationDeg * 3.14159265358979323846 / 180.0;
        const auto c = std::cos(r);
        const auto s = std::sin(r);
        const auto x = sx * p.x;
        const auto y = sy * p.y;
        return {c * x - s * y + tx, s * x + c * y + ty};
    }

    std::vector<V2> transformPolygon(const std::vector<V2>& local, double tx, double ty, double rotDeg, double sx, double sy)
    {
        std::vector<V2> result;
        result.reserve(local.size());
        for (const auto& p : local) result.push_back(apply(tx, ty, rotDeg, sx, sy, p));
        return result;
    }

    double totalArea(const CpuSlotMesh& package)
    {
        double sum = 0.0;
        for (const auto& t : package.triangles)
        {
            const V2 a{t.vertices[0].x, t.vertices[0].y};
            const V2 b{t.vertices[1].x, t.vertices[1].y};
            const V2 c{t.vertices[2].x, t.vertices[2].y};
            sum += std::fabs(area2(a, b, c)) * 0.5;
        }
        return sum;
    }

    double totalAreaBySource(const CpuSlotMesh& package, unsigned source, std::size_t& count)
    {
        double sum = 0.0;
        count = 0;
        for (const auto& t : package.triangles)
        {
            if (t.sourceTriangle == source)
            {
                const V2 a{t.vertices[0].x, t.vertices[0].y};
                const V2 b{t.vertices[1].x, t.vertices[1].y};
                const V2 c{t.vertices[2].x, t.vertices[2].y};
                sum += std::fabs(area2(a, b, c)) * 0.5;
                ++count;
            }
        }
        return sum;
    }

    // Shared quad mesh: bind vertices, UVs, two triangles, counter clockwise.
    const std::vector<V2> kQuad = {{0.0, 0.0}, {4.0, 0.0}, {4.0, 2.0}, {0.0, 2.0}};
    const std::vector<V2> kQuadUv = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};

    // Mask polygon in mask slot local space.
    const std::vector<V2> kMaskLocal = {{-1.0, -1.0}, {3.0, -1.0}, {3.0, 3.0}, {-1.0, 3.0}};
    const std::vector<V2> kMask2Local = {{1.0, 0.0}, {5.0, 0.0}, {5.0, 4.0}, {1.0, 4.0}};

    CpuMaskPolygon makePolygon(const std::vector<V2>& local)
    {
        CpuMaskPolygon polygon;
        for (const auto& p : local)
        {
            polygon.points.push_back(static_cast<float>(p.x));
            polygon.points.push_back(static_cast<float>(p.y));
        }
        return polygon;
    }

    std::string quadMeshJson(const std::string& name, const std::string& path, bool weighted)
    {
        std::ostringstream os;
        os << "{\"name\":\"" << name << "\",\"type\":\"mesh\",\"path\":\"" << path << "\","
           << "\"vertices\":[0,0,4,0,4,2,0,2],"
           << "\"uvs\":[0,0,1,0,1,1,0,1],"
           << "\"triangles\":[0,1,2,0,2,3]";
        if (weighted)
        {
            // slotPose: identity. bonePose: boneL identity at x=0, boneR translated x=4.
            // Every vertex is influenced by both bones (weights sum to 1).
            os << ",\"slotPose\":[1,0,0,1,0,0],"
               << "\"bonePose\":[1,1,0,0,1,0,0, 2,1,0,0,1,4,0],"
               << "\"weights\":["
               << "2,1,0.8,2,0.2,"      // v0 (0,0) -> boneL (0,0), boneR (-4,0)
               << "2,1,0.2,2,0.8,"      // v1 (4,0) -> boneL (4,0), boneR (0,0)
               << "2,1,0.2,2,0.8,"      // v2 (4,2)
               << "2,1,0.8,2,0.2]";     // v3 (0,2)
        }
        os << "}";
        return os.str();
    }

    // Main fixture: two bones, plain + weighted meshes, empty and bounding box
    // mask slots, image / armature / multi display slots.
    const char* kMainJson = R"JSON(
{
 "version":"5.5","compatibleVersion":"5.5","name":"fixture","frameRate":24,
 "armature":[{
  "name":"rig","type":"Armature","frameRate":24,
  "bone":[
   {"name":"root"},
   {"name":"boneL","parent":"root","transform":{"x":0}},
   {"name":"boneR","parent":"root","transform":{"x":4}}
  ],
  "slot":[
   {"name":"mask","parent":"boneL"},
   {"name":"plain","parent":"root","blendMode":"add","color":{"aM":80,"rM":50,"gM":60,"bM":70}},
   {"name":"mask2","parent":"root"},
   {"name":"skin","parent":"root"},
   {"name":"end","parent":"root"},
   {"name":"imgslot","parent":"root"},
   {"name":"armslot","parent":"root"},
   {"name":"swap","parent":"root"}
  ],
  "skin":[{"name":"default","slot":[
   {"name":"plain","display":[MESH_PLAIN]},
   {"name":"mask2","display":[{"name":"bb","type":"boundingBox","subType":"polygon","vertices":[-2,-2,6,-2,6,6,-2,6]}]},
   {"name":"skin","display":[MESH_SKIN]},
   {"name":"imgslot","display":[{"name":"img","type":"image","path":"tex/img.png"}]},
   {"name":"armslot","display":[{"name":"child","type":"armature","path":"childrig"}]},
   {"name":"swap","display":[
     {"name":"small","type":"mesh","path":"tex/small.png","vertices":[0,0,2,0,2,2,0,2],"uvs":[0,0,1,0,1,1,0,1],"triangles":[0,1,2,0,2,3]},
     {"name":"large","type":"mesh","path":"tex/large.png","vertices":[0,0,8,0,8,4,0,4],"uvs":[0,0,1,0,1,1,0,1],"triangles":[0,1,2,0,2,3]}
   ]}
  ]}],
  "animation":[
   {"name":"move","duration":24,"playTimes":0,
    "bone":[
     {"name":"boneL","translateFrame":[{"duration":12,"tweenEasing":0,"x":0,"y":0},{"duration":12,"tweenEasing":0,"x":6,"y":0}]},
     {"name":"boneR","translateFrame":[{"duration":12,"tweenEasing":0,"x":4,"y":0},{"duration":12,"tweenEasing":0,"x":10,"y":0}]}
    ],
    "ffd":[
     {"name":"plainmesh","slot":"plain","frame":[
       {"duration":12,"tweenEasing":0,"vertices":[0,0,1,0,1,0,0,0]},
       {"duration":12,"tweenEasing":0,"vertices":[0,0,0,0,0,0,0,0]}
     ]},
     {"name":"skinmesh","slot":"skin","frame":[
       {"duration":24,"vertices":[0,1,0,1,0,1,0,1]}
     ]}
    ]
   },
   {"name":"fliprot","duration":24,"playTimes":0,
    "bone":[{"name":"root","rotateFrame":[{"duration":24,"rotate":180}]}]
   },
   {"name":"flipref","duration":24,"playTimes":0,
    "bone":[{"name":"root","scaleFrame":[{"duration":24,"x":-1,"y":1}]}]
   },
   {"name":"swapanim","duration":24,"playTimes":0,
    "slot":[{"name":"swap","displayFrame":[{"duration":6},{"duration":18,"value":1}]}]
   }
  ]
 },
 {"name":"childrig","type":"Armature","frameRate":24,"bone":[{"name":"croot"}]}]
}
)JSON";

    std::string mainJson()
    {
        std::string text = kMainJson;
        const auto plain = quadMeshJson("plainmesh", "tex/plain.png", false);
        const auto skin = quadMeshJson("skinmesh", "tex/skin.png", true);
        const auto replace = [&](const std::string& token, const std::string& value)
        {
            const auto pos = text.find(token);
            text.replace(pos, token.size(), value);
        };
        replace("MESH_PLAIN", plain);
        replace("MESH_SKIN", skin);
        return text;
    }

    // Small fixture dedicated to draw order changes.
    const char* kZJson = R"JSON(
{
 "version":"5.5","compatibleVersion":"5.5","name":"zfix","frameRate":24,
 "armature":[{
  "name":"zrig","type":"Armature","frameRate":24,
  "bone":[{"name":"root"}],
  "slot":[
   {"name":"zmask","parent":"root"},
   {"name":"zmesh","parent":"root"},
   {"name":"zend","parent":"root"}
  ],
  "skin":[{"name":"default","slot":[
   {"name":"zmesh","display":[{"name":"zm","type":"mesh","path":"tex/zm.png",
     "vertices":[0,0,4,0,4,2,0,2],"uvs":[0,0,1,0,1,1,0,1],"triangles":[0,1,2,0,2,3]}]}
  ]}],
  "animation":[{"name":"reorder","duration":24,"playTimes":0,
   "zOrder":{"frame":[{"duration":6},{"duration":18,"zOrder":[0,1,1,-1]}]}
  }]
 }]
}
)JSON";

    // Expected deformed plain quad at frame 6: right edge pushed +0.5 in x
    // (the 0..1 FFD tween is halfway through its 12 frame interpolation).
    std::vector<V2> expectedPlainQuad()
    {
        return {{0.0, 0.0}, {4.5, 0.0}, {4.5, 2.0}, {0.0, 2.0}};
    }

    // Expected weighted quad at frame 6, computed independently in this test
    // from the live bone matrices and the packed bind pose influences. The
    // constant FFD point (0, 1) is converted per influence through the
    // inverse bind pose, exactly as the parser stores it. The weighted result
    // is already in armature space and must not use the slot matrix.
    std::vector<V2> expectedWeightedQuad(Armature* armature)
    {
        const auto slot = armature->getSlot("skin");
        const auto vd = slot->_deformVertices->verticesData;
        const auto ia = vd->data->intArray;
        const auto fa = vd->data->floatArray;
        const auto w = vd->weight;
        const auto& bones = slot->_deformVertices->bones;
        int wfo = ia[w->offset + (int)BinaryOffset::WeigthFloatOffset];
        if (wfo < 0) wfo += 65536;
        std::size_t iB = w->offset + (unsigned)BinaryOffset::WeigthBoneIndices + w->bones.size();
        std::size_t iV = (unsigned)wfo;

        std::vector<Matrix> inverseBind(bones.size());
        for (std::size_t i = 0; i < bones.size(); ++i)
        {
            auto bind = bones[i]->getBoneData()->transform;
            bind.toMatrix(inverseBind[i]);
            inverseBind[i].invert();
        }

        std::vector<V2> result;
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto boneCount = (unsigned)ia[iB++];
            double xG = 0.0;
            double yG = 0.0;
            for (unsigned j = 0; j < boneCount; ++j)
            {
                const auto boneIndex = (unsigned)ia[iB++];
                const auto weight = fa[iV++];
                auto lx = fa[iV++];
                auto ly = fa[iV++];
                // The parser stores FFD per influence with the linear part of
                // the inverse bind pose (delta transform, no translation).
                Point ffdLocal;
                inverseBind[boneIndex].transformPoint(0.0f, 1.0f, ffdLocal, true); // Constant FFD (0, 1).
                lx += ffdLocal.x;
                ly += ffdLocal.y;
                const auto& m = bones[boneIndex]->globalTransformMatrix;
                xG += (m.a * lx + m.c * ly + m.tx) * weight;
                yG += (m.b * lx + m.d * ly + m.ty) * weight;
            }
            result.push_back({xG, yG});
        }
        return result;
    }

    std::vector<V2> quadTriangle(const std::vector<V2>& quad, unsigned triangle)
    {
        if (triangle == 0) return {quad[0], quad[1], quad[2]};
        return {quad[0], quad[2], quad[3]};
    }

    void checkApprox(double value, double expected, double tolerance)
    {
        CLIP_CHECK(std::fabs(value - expected) <= tolerance);
    }

    // Verify every corner of every output triangle against independently
    // computed source geometry: position inside masks, UV barycentric, winding.
    void verifyTriangles(
        const CpuSlotMesh& package,
        const std::vector<V2>& expectedQuad,
        const std::vector<V2>& expectedUv,
        const std::vector<std::vector<V2>>& masks
    )
    {
        for (const auto& triangle : package.triangles)
        {
            CLIP_CHECK(triangle.sourceTriangle < 2);
            const auto p0 = expectedQuad[triangle.sourceTriangle == 0 ? 0 : 0];
            const auto p1 = expectedQuad[triangle.sourceTriangle == 0 ? 1 : 2];
            const auto p2 = expectedQuad[triangle.sourceTriangle == 0 ? 2 : 3];
            const auto u0 = expectedUv[triangle.sourceTriangle == 0 ? 0 : 0];
            const auto u1 = expectedUv[triangle.sourceTriangle == 0 ? 1 : 2];
            const auto u2 = expectedUv[triangle.sourceTriangle == 0 ? 2 : 3];

            const auto sourceSign = area2(p0, p1, p2);
            CLIP_CHECK(std::fabs(sourceSign) > 1e-9);

            for (const auto& corner : triangle.vertices)
            {
                const V2 position{corner.x, corner.y};
                for (const auto& mask : masks)
                {
                    CLIP_CHECK(insideConvex(position, mask));
                }

                const auto total = area2(p0, p1, p2);
                const auto w1 = area2(p0, position, p2) / total;
                const auto w2 = area2(p0, p1, position) / total;
                const auto w0 = 1.0 - w1 - w2;
                CLIP_CHECK(w0 >= -1e-4 && w1 >= -1e-4 && w2 >= -1e-4);
                const auto expectU = w0 * u0.x + w1 * u1.x + w2 * u2.x;
                const auto expectV = w0 * u0.y + w1 * u1.y + w2 * u2.y;
                checkApprox(corner.u, expectU, 1e-3);
                checkApprox(corner.v, expectV, 1e-3);
            }

            const V2 a{triangle.vertices[0].x, triangle.vertices[0].y};
            const V2 b{triangle.vertices[1].x, triangle.vertices[1].y};
            const V2 c{triangle.vertices[2].x, triangle.vertices[2].y};
            CLIP_CHECK(area2(a, b, c) * sourceSign > 0.0); // Winding preserved.
        }
    }

    struct PackageChecksum
    {
        std::string slotName;
        std::size_t triangleCount;
        double area;
        float firstX, firstY, firstU, firstV;

        static PackageChecksum of(const CpuSlotMesh& package)
        {
            PackageChecksum sum;
            sum.slotName = package.slotName;
            sum.triangleCount = package.triangles.size();
            sum.area = totalArea(package);
            if (!package.triangles.empty())
            {
                const auto& corner = package.triangles.front().vertices[0];
                sum.firstX = corner.x;
                sum.firstY = corner.y;
                sum.firstU = corner.u;
                sum.firstV = corner.v;
            }
            return sum;
        }
        bool equals(const PackageChecksum& other) const
        {
            return slotName == other.slotName && triangleCount == other.triangleCount &&
                   std::fabs(area - other.area) < EPS_AREA &&
                   std::fabs(firstX - other.firstX) < EPS &&
                   std::fabs(firstY - other.firstY) < EPS &&
                   std::fabs(firstU - other.firstU) < EPS &&
                   std::fabs(firstV - other.firstV) < EPS;
        }
    };
}

// Case 1: unskinned mesh, animated FFD, mask following an animated bone.
CLIP_CASE(unskinned_mesh_ffd_clipping)
{
    task_support::Factory factory;
    CLIP_CHECK(factory.parseDragonBonesData(mainJson().c_str()));
    auto armature = factory.buildArmature("rig", "fixture");
    CLIP_CHECK(armature != nullptr);
    CLIP_CHECK(armature->getAnimation()->play("move"));
    armature->advanceTime(0.25f); // Frame 6 of 24.

    // The mask rides boneL, which has moved +1.5 in x at frame 6.
    const auto maskWorld = transformPolygon(kMaskLocal, 3.0, 0.0, 0.0, 1.0, 1.0);
    const auto quad = expectedPlainQuad();

    std::vector<CpuMaskRange> ranges;
    ranges.emplace_back("mask", "end", makePolygon(kMaskLocal));
    std::vector<CpuSlotMesh> packages;
    CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));

    // Only plain and skin survive; mask slots and the image/armature/swap
    // slots are not meshed in this range layout check below.
    const auto plainIndex = [&]() -> int
    {
        for (std::size_t i = 0; i < packages.size(); ++i)
        {
            if (packages[i].slotName == "plain") return static_cast<int>(i);
        }
        return -1;
    }();
    CLIP_CHECK(plainIndex >= 0);
    const auto& plain = packages[plainIndex];

    CLIP_CHECK(plain.displayName == "plainmesh");
    CLIP_CHECK(plain.materialPath == "tex/plain.png");
    CLIP_CHECK(plain.blendMode == BlendMode::Add);
    // The runtime stores colors as int percent quantized through float32,
    // which can drift by ~0.01; a 0.02 tolerance still proves the exact copy.
    const float colorEps = 0.02f;
    CLIP_CHECK(std::fabs(plain.colorTransform.alphaMultiplier - 0.8f) < colorEps);
    CLIP_CHECK(std::fabs(plain.colorTransform.redMultiplier - 0.5f) < colorEps);
    CLIP_CHECK(std::fabs(plain.colorTransform.greenMultiplier - 0.6f) < colorEps);
    CLIP_CHECK(std::fabs(plain.colorTransform.blueMultiplier - 0.7f) < colorEps);
    CLIP_CHECK(plain.colorTransform.alphaOffset == 0 && plain.colorTransform.redOffset == 0);

    double expected = 0.0;
    for (unsigned t = 0; t < 2; ++t)
    {
        expected += expectedTriangleArea(quadTriangle(quad, t), {maskWorld});
    }
    CLIP_CHECK(expected > 0.0 && expected < 9.0); // Something was cut.
    checkApprox(totalArea(plain), expected, EPS_AREA);

    for (unsigned t = 0; t < 2; ++t)
    {
        std::size_t count = 0;
        const auto area = totalAreaBySource(plain, t, count);
        CLIP_CHECK(count > 0);
        checkApprox(area, expectedTriangleArea(quadTriangle(quad, t), {maskWorld}), EPS_AREA);
    }

    verifyTriangles(plain, quad, kQuadUv, {maskWorld});

    // Packages keep their own copies: a second call leaves the first intact.
    const auto checksum = PackageChecksum::of(plain);
    std::vector<CpuSlotMesh> again;
    CLIP_CHECK(buildClippedMesh(*armature, ranges, again));
    CLIP_CHECK(PackageChecksum::of(packages[plainIndex]).equals(checksum));

    armature->dispose();
    factory.runtime.advanceTime(0);
}

// Case 2: two bone weighted mesh with weighted FFD.
CLIP_CASE(weighted_two_bone_mesh)
{
    task_support::Factory factory;
    CLIP_CHECK(factory.parseDragonBonesData(mainJson().c_str()));
    auto armature = factory.buildArmature("rig", "fixture");
    CLIP_CHECK(armature != nullptr);
    CLIP_CHECK(armature->getAnimation()->play("move"));
    armature->advanceTime(0.25f);

    const auto maskWorld = transformPolygon(kMaskLocal, 3.0, 0.0, 0.0, 1.0, 1.0);
    const auto quad = expectedWeightedQuad(armature);

    std::vector<CpuMaskRange> ranges;
    ranges.emplace_back("mask", "end", makePolygon(kMaskLocal));
    std::vector<CpuSlotMesh> packages;
    CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));

    const auto skin = [&]() -> const CpuSlotMesh*
    {
        for (const auto& package : packages)
        {
            if (package.slotName == "skin") return &package;
        }
        return nullptr;
    }();
    CLIP_CHECK(skin != nullptr);
    CLIP_CHECK(skin->displayName == "skinmesh");
    CLIP_CHECK(skin->materialPath == "tex/skin.png");

    // The independently recomputed left edge corners must appear verbatim in
    // the output whenever they lie inside the mask.
    const V2 v0 = quad[0];
    const V2 v3 = quad[3];
    bool sawV0 = false;
    bool sawV3 = false;
    for (const auto& triangle : skin->triangles)
    {
        for (const auto& corner : triangle.vertices)
        {
            if (std::fabs(corner.x - v0.x) < EPS && std::fabs(corner.y - v0.y) < EPS) sawV0 = true;
            if (std::fabs(corner.x - v3.x) < EPS && std::fabs(corner.y - v3.y) < EPS) sawV3 = true;
        }
    }
    CLIP_CHECK(sawV0 && sawV3); // Uncut corners keep exact deformed positions.

    double expected = 0.0;
    for (unsigned t = 0; t < 2; ++t)
    {
        expected += expectedTriangleArea(quadTriangle(quad, t), {maskWorld});
    }
    checkApprox(totalArea(*skin), expected, EPS_AREA);
    verifyTriangles(*skin, quad, kQuadUv, {maskWorld});

    armature->dispose();
    factory.runtime.advanceTime(0);
}

// Case 2b: the independent convex test itself is sound under reflection.
CLIP_CASE(inside_test_reflected_polygon)
{
    const auto reflected = transformPolygon(kMaskLocal, 0.0, 0.0, 0.0, -1.0, 1.0);
    const std::vector<V2> points = {{0.0, 0.0}, {-1.0, 1.0}, {-2.0, 2.0}, {-0.5, -0.5}};
    for (const auto& p : points)
    {
        CLIP_CHECK(insideConvex(p, reflected));
    }
    const std::vector<V2> outside = {{2.0, 0.0}, {5.0, 5.0}, {-5.0, 0.0}};
    for (const auto& p : outside)
    {
        CLIP_CHECK(!insideConvex(p, reflected));
    }
}

// Case 3: rotated and reflected masks must keep the interior, never invert.
CLIP_CASE(rotated_reflected_mask)
{
    task_support::Factory factory;
    CLIP_CHECK(factory.parseDragonBonesData(mainJson().c_str()));
    auto armature = factory.buildArmature("rig", "fixture");
    CLIP_CHECK(armature != nullptr);

    std::vector<CpuMaskRange> ranges;
    ranges.emplace_back("mask", "end", makePolygon(kMaskLocal));

    // 180 degree rotation.
    CLIP_CHECK(armature->getAnimation()->play("fliprot"));
    armature->advanceTime(0.25f);
    {
        const auto root = armature->getBone("root");
        CLIP_CHECK(root != nullptr);
        const auto determinant = root->globalTransformMatrix.a * root->globalTransformMatrix.d -
                                 root->globalTransformMatrix.b * root->globalTransformMatrix.c;
        checkApprox(determinant, 1.0, 1e-4);

        const auto maskWorld = transformPolygon(kMaskLocal, 0.0, 0.0, 180.0, 1.0, 1.0);
        const auto quad = transformPolygon(kQuad, 0.0, 0.0, 180.0, 1.0, 1.0); // Mesh rotates with root.
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));
        const auto plain = [&]() -> const CpuSlotMesh*
        {
            for (const auto& package : packages) if (package.slotName == "plain") return &package;
            return nullptr;
        }();
        CLIP_CHECK(plain != nullptr);
        double expected = 0.0;
        for (unsigned t = 0; t < 2; ++t)
        {
            expected += expectedTriangleArea(quadTriangle(quad, t), {maskWorld});
        }
        checkApprox(totalArea(*plain), expected, EPS_AREA);
        verifyTriangles(*plain, quad, kQuadUv, {maskWorld});
    }

    // Reflection: negative x scale. The mask must not become its complement.
    CLIP_CHECK(armature->getAnimation()->play("flipref"));
    armature->advanceTime(0.25f);
    {
        const auto root = armature->getBone("root");
        const auto determinant = root->globalTransformMatrix.a * root->globalTransformMatrix.d -
                                 root->globalTransformMatrix.b * root->globalTransformMatrix.c;
        CLIP_CHECK(determinant < 0.0);

        const auto maskWorld = transformPolygon(kMaskLocal, 0.0, 0.0, 0.0, -1.0, 1.0);
        const auto quad = transformPolygon(kQuad, 0.0, 0.0, 0.0, -1.0, 1.0); // Mesh reflects with root.
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));
        const auto plain = [&]() -> const CpuSlotMesh*
        {
            for (const auto& package : packages) if (package.slotName == "plain") return &package;
            return nullptr;
        }();
        CLIP_CHECK(plain != nullptr);

        double expected = 0.0;
        for (unsigned t = 0; t < 2; ++t)
        {
            expected += expectedTriangleArea(quadTriangle(quad, t), {maskWorld});
        }
        CLIP_CHECK(expected > 0.0);
        checkApprox(totalArea(*plain), expected, EPS_AREA);
        // The reflected mask covers negative x: surviving geometry must be there.
        for (const auto& triangle : plain->triangles)
        {
            for (const auto& corner : triangle.vertices)
            {
                CLIP_CHECK(corner.x <= 1e-4);
            }
        }
        verifyTriangles(*plain, quad, kQuadUv, {maskWorld});
    }

    armature->dispose();
    factory.runtime.advanceTime(0);
}

// Case 4: nested and disjoint ranges, crossing ranges rejected, bad input rejected.
CLIP_CASE(nested_disjoint_and_invalid_ranges)
{
    task_support::Factory factory;
    CLIP_CHECK(factory.parseDragonBonesData(mainJson().c_str()));
    auto armature = factory.buildArmature("rig", "fixture");
    CLIP_CHECK(armature != nullptr);
    CLIP_CHECK(armature->getAnimation()->play("move"));
    armature->advanceTime(0.25f);

    const auto maskWorld = transformPolygon(kMaskLocal, 3.0, 0.0, 0.0, 1.0, 1.0);
    const auto mask2World = transformPolygon(kMask2Local, 0.0, 0.0, 0.0, 1.0, 1.0);
    const auto skinQuad = expectedWeightedQuad(armature);

    // Nested: mask..end contains mask2..end (mask2 slot sits between them).
    {
        std::vector<CpuMaskRange> ranges;
        ranges.emplace_back("mask", "end", makePolygon(kMaskLocal));
        ranges.emplace_back("mask2", "end", makePolygon(kMask2Local));
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));

        const auto skin = [&]() -> const CpuSlotMesh*
        {
            for (const auto& package : packages) if (package.slotName == "skin") return &package;
            return nullptr;
        }();
        CLIP_CHECK(skin != nullptr);
        double expected = 0.0;
        for (unsigned t = 0; t < 2; ++t)
        {
            expected += expectedTriangleArea(quadTriangle(skinQuad, t), {maskWorld, mask2World});
        }
        checkApprox(totalArea(*skin), expected, EPS_AREA);
        verifyTriangles(*skin, skinQuad, kQuadUv, {maskWorld, mask2World});

        // plain only sees the outer mask.
        const auto plain = [&]() -> const CpuSlotMesh*
        {
            for (const auto& package : packages) if (package.slotName == "plain") return &package;
            return nullptr;
        }();
        CLIP_CHECK(plain != nullptr);
        double expectedPlain = 0.0;
        const auto plainQuad = expectedPlainQuad();
        for (unsigned t = 0; t < 2; ++t)
        {
            expectedPlain += expectedTriangleArea(quadTriangle(plainQuad, t), {maskWorld});
        }
        checkApprox(totalArea(*plain), expectedPlain, EPS_AREA);
    }

    // Disjoint ranges: mask..plain and mask2..end.
    {
        std::vector<CpuMaskRange> ranges;
        ranges.emplace_back("mask", "plain", makePolygon(kMaskLocal));
        ranges.emplace_back("mask2", "end", makePolygon(kMask2Local));
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));
        const auto plain = [&]() -> const CpuSlotMesh*
        {
            for (const auto& package : packages) if (package.slotName == "plain") return &package;
            return nullptr;
        }();
        const auto skin = [&]() -> const CpuSlotMesh*
        {
            for (const auto& package : packages) if (package.slotName == "skin") return &package;
            return nullptr;
        }();
        CLIP_CHECK(plain != nullptr && skin != nullptr);
        double expectedSkin = 0.0;
        for (unsigned t = 0; t < 2; ++t)
        {
            expectedSkin += expectedTriangleArea(quadTriangle(skinQuad, t), {mask2World});
        }
        checkApprox(totalArea(*skin), expectedSkin, EPS_AREA);
    }

    // Crossing ranges are rejected.
    {
        std::vector<CpuMaskRange> ranges;
        ranges.emplace_back("mask", "skin", makePolygon(kMaskLocal));
        ranges.emplace_back("plain", "end", makePolygon(kMask2Local));
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(!buildClippedMesh(*armature, ranges, packages));
        CLIP_CHECK(packages.empty());
    }

    // Invalid slot references.
    {
        std::vector<CpuMaskRange> ranges;
        ranges.emplace_back("nope", "end", makePolygon(kMaskLocal));
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(!buildClippedMesh(*armature, ranges, packages));
    }
    {
        std::vector<CpuMaskRange> ranges;
        ranges.emplace_back("end", "mask", makePolygon(kMaskLocal)); // Reversed order.
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(!buildClippedMesh(*armature, ranges, packages));
    }
    {
        std::vector<CpuMaskRange> ranges;
        ranges.emplace_back("mask", "mask", makePolygon(kMaskLocal)); // Same slot.
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(!buildClippedMesh(*armature, ranges, packages));
    }

    // Non convex, too small, non finite polygons.
    {
        std::vector<CpuMaskRange> ranges;
        ranges.emplace_back("mask", "end", makePolygon({{0.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}, {2.0, 0.0}})); // Bowtie.
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(!buildClippedMesh(*armature, ranges, packages));
    }
    {
        std::vector<CpuMaskRange> ranges;
        ranges.emplace_back("mask", "end", makePolygon({{0.0, 0.0}, {1.0, 1.0}})); // Only two points.
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(!buildClippedMesh(*armature, ranges, packages));
    }
    {
        auto polygon = makePolygon(kMaskLocal);
        polygon.points[2] = std::numeric_limits<float>::infinity();
        std::vector<CpuMaskRange> ranges;
        ranges.emplace_back("mask", "end", polygon);
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(!buildClippedMesh(*armature, ranges, packages));
    }

    armature->dispose();
    factory.runtime.advanceTime(0);
}

// Case 5: zOrder animation changes which slots fall inside the range.
CLIP_CASE(zorder_change_reinterprets_order)
{
    task_support::Factory factory;
    CLIP_CHECK(factory.parseDragonBonesData(kZJson));
    auto armature = factory.buildArmature("zrig", "zfix");
    CLIP_CHECK(armature != nullptr);
    CLIP_CHECK(armature->getAnimation()->play("reorder"));

    // Narrow mask that cuts the quad's right portion (x in [-1, 2]).
    const std::vector<V2> zMaskLocal = {{-1.0, -1.0}, {2.0, -1.0}, {2.0, 3.0}, {-1.0, 3.0}};
    std::vector<CpuMaskRange> ranges;
    ranges.emplace_back("zmask", "zend", makePolygon(zMaskLocal));

    // Before the zOrder frame: order is mask, mesh, end -> mesh is clipped.
    armature->advanceTime(0.125f); // Frame 3.
    {
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));
        CLIP_CHECK(packages.size() == 1);
        CLIP_CHECK(packages[0].slotName == "zmesh");
        const auto maskWorld = transformPolygon(zMaskLocal, 0.0, 0.0, 0.0, 1.0, 1.0);
        double expected = 0.0;
        const std::vector<V2> quad = kQuad;
        for (unsigned t = 0; t < 2; ++t)
        {
            expected += expectedTriangleArea(quadTriangle(quad, t), {maskWorld});
        }
        checkApprox(totalArea(packages[0]), expected, EPS_AREA);
        CLIP_CHECK(expected < 8.0); // The narrow mask really cut the mesh.
    }

    // After the zOrder frame: order becomes mesh, mask, end -> the mesh is
    // now drawn before the mask opens, so it is no longer covered by the
    // range and keeps its full, unclipped geometry.
    armature->advanceTime(0.375f); // Frame 12 (past the frame 6 swap).
    {
        CLIP_CHECK(armature->getSlots()[0]->getName() == "zmesh");
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));
        CLIP_CHECK(packages.size() == 1);
        CLIP_CHECK(packages[0].slotName == "zmesh");
        checkApprox(totalArea(packages[0]), 8.0, EPS_AREA); // Full 4 x 2 quad.
    }

    armature->dispose();
    factory.runtime.advanceTime(0);
}

// Case 6: hidden end slot, hidden mask, display swapping, next frame and persistence.
CLIP_CASE(hidden_slots_swap_next_frame_persistence)
{
    task_support::Factory factory;
    CLIP_CHECK(factory.parseDragonBonesData(mainJson().c_str()));
    auto armature = factory.buildArmature("rig", "fixture");
    CLIP_CHECK(armature != nullptr);
    CLIP_CHECK(armature->getAnimation()->play("move"));
    armature->advanceTime(0.25f);

    const auto maskWorld = transformPolygon(kMaskLocal, 3.0, 0.0, 0.0, 1.0, 1.0);

    // Hidden end slot: the range still closes there.
    armature->getSlot("end")->setVisible(false);
    {
        std::vector<CpuMaskRange> ranges;
        ranges.emplace_back("mask", "end", makePolygon(kMaskLocal));
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));
        const auto plain = [&]() -> const CpuSlotMesh*
        {
            for (const auto& package : packages) if (package.slotName == "plain") return &package;
            return nullptr;
        }();
        CLIP_CHECK(plain != nullptr);
        double expected = 0.0;
        const auto quad = expectedPlainQuad();
        for (unsigned t = 0; t < 2; ++t)
        {
            expected += expectedTriangleArea(quadTriangle(quad, t), {maskWorld});
        }
        checkApprox(totalArea(*plain), expected, EPS_AREA);
    }

    // Hidden mask: its range is empty, every covered slot disappears.
    armature->getSlot("mask")->setVisible(false);
    {
        std::vector<CpuMaskRange> ranges;
        ranges.emplace_back("mask", "end", makePolygon(kMaskLocal));
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(buildClippedMesh(*armature, ranges, packages));
        bool hasPlainOrSkin = false;
        for (const auto& package : packages)
        {
            hasPlainOrSkin = hasPlainOrSkin || package.slotName == "plain" || package.slotName == "skin";
        }
        CLIP_CHECK(!hasPlainOrSkin);
    }
    armature->getSlot("mask")->setVisible(true);

    // Display swap through an animation: different mesh after frame 6.
    CLIP_CHECK(armature->getAnimation()->play("swapanim"));
    armature->advanceTime(0.125f); // Frame 3: small display.
    std::vector<CpuSlotMesh> saved;
    {
        std::vector<CpuMaskRange> ranges; // No mask: same CPU entry, full mesh.
        CLIP_CHECK(buildClippedMesh(*armature, ranges, saved));
        const auto swap = [&]() -> const CpuSlotMesh*
        {
            for (const auto& package : saved) if (package.slotName == "swap") return &package;
            return nullptr;
        }();
        CLIP_CHECK(swap != nullptr);
        CLIP_CHECK(swap->displayName == "small");
        CLIP_CHECK(swap->materialPath == "tex/small.png");
        CLIP_CHECK(swap->triangles.size() == 2);
        checkApprox(totalArea(*swap), 4.0, EPS_AREA); // 2 x 2 quad.
    }
    const auto savedChecksum = [&]()
    {
        for (const auto& package : saved)
        {
            if (package.slotName == "swap") return PackageChecksum::of(package);
        }
        return PackageChecksum{};
    }();

    armature->advanceTime(0.375f); // Past frame 6: large display.
    {
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(buildClippedMesh(*armature, std::vector<CpuMaskRange>{}, packages));
        const auto swap = [&]() -> const CpuSlotMesh*
        {
            for (const auto& package : packages) if (package.slotName == "swap") return &package;
            return nullptr;
        }();
        CLIP_CHECK(swap != nullptr);
        CLIP_CHECK(swap->displayName == "large");
        checkApprox(totalArea(*swap), 32.0, EPS_AREA); // 8 x 4 quad.
    }

    // The saved package is untouched by the next frame and by armature disposal.
    CLIP_CHECK(savedChecksum.triangleCount == 2);
    CLIP_CHECK(std::fabs(savedChecksum.area - 4.0) < EPS_AREA);
    armature->dispose();
    factory.runtime.advanceTime(0);
    CLIP_CHECK(savedChecksum.triangleCount == 2);
    CLIP_CHECK(std::fabs(savedChecksum.area - 4.0) < EPS_AREA);
    CLIP_CHECK(savedChecksum.slotName == "swap");
}

// Case 7: no mask still uses the CPU pipeline; Image, Armature and Path fail.
CLIP_CASE(no_mask_full_mesh_and_unsupported_displays)
{
    task_support::Factory factory;
    CLIP_CHECK(factory.parseDragonBonesData(mainJson().c_str()));
    auto armature = factory.buildArmature("rig", "fixture");
    CLIP_CHECK(armature != nullptr);
    CLIP_CHECK(armature->getAnimation()->play("move"));
    armature->advanceTime(0.25f);

    // No mask: complete deformed meshes through the same entry point.
    std::vector<CpuSlotMesh> full;
    CLIP_CHECK(buildClippedMesh(*armature, {}, full));
    const auto plain = [&]() -> const CpuSlotMesh*
    {
        for (const auto& package : full) if (package.slotName == "plain") return &package;
        return nullptr;
    }();
    const auto skin = [&]() -> const CpuSlotMesh*
    {
        for (const auto& package : full) if (package.slotName == "skin") return &package;
        return nullptr;
    }();
    CLIP_CHECK(plain != nullptr && skin != nullptr);
    CLIP_CHECK(plain->triangles.size() == 2);
    CLIP_CHECK(skin->triangles.size() == 2);
    for (const auto& triangle : plain->triangles)
    {
        for (const auto& corner : triangle.vertices)
        {
            CLIP_CHECK(std::isfinite(corner.x) && std::isfinite(corner.y));
        }
    }
    const auto plainQuad = expectedPlainQuad();
    const auto skinQuad = expectedWeightedQuad(armature);
    double expectedPlain = 0.0;
    double expectedSkin = 0.0;
    for (unsigned t = 0; t < 2; ++t)
    {
        expectedPlain += polygonArea(quadTriangle(plainQuad, t));
        expectedSkin += polygonArea(quadTriangle(skinQuad, t));
    }
    checkApprox(totalArea(*plain), expectedPlain, EPS_AREA);
    checkApprox(totalArea(*skin), expectedSkin, EPS_AREA);

    // Output ordering follows the slot draw order, slots are never merged.
    int plainOrder = -1;
    int skinOrder = -1;
    for (std::size_t i = 0; i < full.size(); ++i)
    {
        if (full[i].slotName == "plain") plainOrder = static_cast<int>(i);
        if (full[i].slotName == "skin") skinOrder = static_cast<int>(i);
    }
    CLIP_CHECK(plainOrder >= 0 && skinOrder > plainOrder);

    // Source deform buffer is untouched.
    const auto deformSlot = armature->getSlot("plain");
    CLIP_CHECK(deformSlot != nullptr && deformSlot->_deformVertices != nullptr);
    const auto deformCopy = deformSlot->_deformVertices->vertices;
    CLIP_CHECK(!deformCopy.empty());
    std::vector<CpuSlotMesh> second;
    CLIP_CHECK(buildClippedMesh(*armature, {}, second));
    CLIP_CHECK(deformSlot->_deformVertices->vertices == deformCopy);

    // Active Image display inside a range is rejected; outside ranges it is
    // simply skipped (which is why the no mask call above succeeds).
    {
        std::vector<CpuMaskRange> ranges;
        ranges.emplace_back("mask", "imgslot", makePolygon(kMaskLocal));
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(!buildClippedMesh(*armature, ranges, packages));
        CLIP_CHECK(packages.empty());
        const auto imageSlot = armature->getSlot("imgslot");
        CLIP_CHECK(imageSlot != nullptr && imageSlot->getDisplay() != nullptr && imageSlot->getChildArmature() == nullptr);
    }

    // Active child armature display inside a range is rejected.
    {
        const auto armSlot = armature->getSlot("armslot");
        CLIP_CHECK(armSlot != nullptr && armSlot->getChildArmature() != nullptr);
        std::vector<CpuMaskRange> ranges;
        ranges.emplace_back("mask", "armslot", makePolygon(kMaskLocal));
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(!buildClippedMesh(*armature, ranges, packages));
    }

    // Active Path display inside a range is rejected. The other unsupported
    // slots are hidden so the range can actually reach the swapped mesh slot.
    {
        armature->getSlot("imgslot")->setVisible(false);
        armature->getSlot("armslot")->setVisible(false);
        const auto swapSlot = armature->getSlot("swap");
        CLIP_CHECK(swapSlot != nullptr);
        auto* savedData = swapSlot->_displayData;
        CLIP_CHECK(savedData != nullptr);
        savedData->type = DisplayType::Path;
        std::vector<CpuMaskRange> ranges;
        ranges.emplace_back("mask", "swap", makePolygon(kMaskLocal));
        std::vector<CpuSlotMesh> packages;
        CLIP_CHECK(!buildClippedMesh(*armature, ranges, packages));
        savedData->type = DisplayType::Mesh;
        std::vector<CpuSlotMesh> recovered;
        CLIP_CHECK(buildClippedMesh(*armature, ranges, recovered));
        armature->getSlot("imgslot")->setVisible(true);
        armature->getSlot("armslot")->setVisible(true);
    }

    armature->dispose();
    factory.runtime.advanceTime(0);
}
