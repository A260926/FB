#ifndef AFFINE_SUPPORT_HPP
#define AFFINE_SUPPORT_HPP

/// @file affine_support.hpp
/// @brief Independent affine-expansion reference and render fixture used by
///        task_tests/affine_instances.cpp.
///
/// The expansion below deliberately re-implements the placement semantics
/// instead of calling the production transform code: positions are
/// `basis * p + position`, normals use the inverse transpose normalised back
/// to FIXED_POINT_SCALE, and a negative determinant swaps the last two
/// triangle indices so the prototype's authored front face stays front.

#include "Scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace AffineCheck {

constexpr int kWidth  = 200;
constexpr int kHeight = 150;
constexpr int kPixels = kWidth * kHeight;

struct BasisInfo {
    double a[9] = {1,0,0, 0,1,0, 0,0,1};
    double det = 1.0;
    double gram = 0.0;
    bool orthogonal = true;
};

inline BasisInfo analyze(const Renderer::Object::InstanceTransform& t) {
    BasisInfo b;
    std::copy(t.basis, t.basis + 9, b.a);
    b.det = b.a[0]*(b.a[4]*b.a[8] - b.a[5]*b.a[7])
          - b.a[1]*(b.a[3]*b.a[8] - b.a[5]*b.a[6])
          + b.a[2]*(b.a[3]*b.a[7] - b.a[4]*b.a[6]);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const double d = b.a[0*3+i]*b.a[0*3+j]
                           + b.a[1*3+i]*b.a[1*3+j]
                           + b.a[2*3+i]*b.a[2*3+j];
            b.gram = std::max(b.gram, std::abs(d - (i == j ? 1.0 : 0.0)));
        }
    }
    // Mirrors Scene.cpp's rigid/affine split: trig-built rotations land near
    // 7e-3, while a real scale/shear is far above this bound.
    b.orthogonal = b.gram <= 0.02;
    return b;
}

inline bool finiteBasis(const float* m) {
    for (int i = 0; i < 9; ++i) if (!std::isfinite(m[i])) return false;
    return true;
}

inline bool usableBasis(const float* m) {
    if (!finiteBasis(m)) return false;
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double det = a*(e*i - f*h) - b*(d*i - f*g) + c*(d*h - e*g);
    const double scale = std::max({std::abs(a),std::abs(b),std::abs(c),
                                   std::abs(d),std::abs(e),std::abs(f),
                                   std::abs(g),std::abs(h),std::abs(i), 1e-30});
    return std::isfinite(det) && std::abs(det) > scale*scale*scale*1e-6;
}

// Expand every placement into one ordinary Object. The owner's own geometry
// (if any) is retained; instances are appended in insertion order.
inline Renderer::Object expandReference(const Renderer::Object& batch) {
    using Renderer::Object;
    Object out = batch;
    out.instances.clear();
    for (const auto& inst : batch.instances) {
        if (!inst.mesh) continue;
        const BasisInfo bi = analyze(inst.transform);
        const double* m = bi.a;
        const auto& pos = inst.transform.position;
        const size_t base = out.vertices.size();
        for (const auto& src : inst.mesh->vertices) {
            Object::Vertex v = src;
            const double x = src.position.x, y = src.position.y, z = src.position.z;
            v.position = {
                (int32_t)(m[0]*x + m[1]*y + m[2]*z + pos.x),
                (int32_t)(m[3]*x + m[4]*y + m[5]*z + pos.y),
                (int32_t)(m[6]*x + m[7]*y + m[8]*z + pos.z) };
            const double nx = src.normal.x, ny = src.normal.y, nz = src.normal.z;
            if (bi.orthogonal) {
                v.normal = {
                    (int32_t)(m[0]*nx + m[1]*ny + m[2]*nz),
                    (int32_t)(m[3]*nx + m[4]*ny + m[5]*nz),
                    (int32_t)(m[6]*nx + m[7]*ny + m[8]*nz) };
            } else {
                const double inv[9] = {
                     (m[4]*m[8]-m[5]*m[7])/bi.det, -(m[1]*m[8]-m[2]*m[7])/bi.det,  (m[1]*m[5]-m[2]*m[4])/bi.det,
                    -(m[3]*m[8]-m[5]*m[6])/bi.det,  (m[0]*m[8]-m[2]*m[6])/bi.det, -(m[0]*m[5]-m[2]*m[3])/bi.det,
                     (m[3]*m[7]-m[4]*m[6])/bi.det, -(m[0]*m[7]-m[1]*m[6])/bi.det,  (m[0]*m[4]-m[1]*m[3])/bi.det };
                double tx = inv[0]*nx + inv[3]*ny + inv[6]*nz;
                double ty = inv[1]*nx + inv[4]*ny + inv[7]*nz;
                double tz = inv[2]*nx + inv[5]*ny + inv[8]*nz;
                const double len = std::sqrt(tx*tx + ty*ty + tz*tz);
                if (len > 0.0) {
                    const double s = double(Renderer::FIXED_POINT_SCALE) / len;
                    tx *= s; ty *= s; tz *= s;
                }
                v.normal = { (int32_t)tx, (int32_t)ty, (int32_t)tz };
            }
            out.addVertex(v);
        }
        size_t triangleIndex = 0;
        for (Object::Triangle tri : inst.mesh->triangles) {
            tri.v1 = (uint16_t)(tri.v1 + base);
            tri.v2 = (uint16_t)(tri.v2 + base);
            tri.v3 = (uint16_t)(tri.v3 + base);
            if (bi.det < 0.0) std::swap(tri.v2, tri.v3);
            Renderer::Material* replacement = inst.materialOverride;
            if (!replacement && inst.triangleMaterials &&
                inst.triangleMaterials->size() == inst.mesh->triangles.size())
                replacement = (*inst.triangleMaterials)[triangleIndex];
            if (replacement) {
                tri.material = replacement;
                tri.colorBaked = false;
            }
            out.triangles.push_back(tri);
            ++triangleIndex;
        }
    }
    out.calculateBoundingBox();
    return out;
}

// ---------------------------------------------------------------------------
// Render fixture. Camera rotation stays zero so every world-space comparison
// runs through the same integer transform with no camera-angle rounding.
// ---------------------------------------------------------------------------
struct Snapshot {
    std::vector<uint16_t> pixels;
    int drawnTriangles = 0;
    int rasterizedTriangles = 0;
    int drawnObjects = 0;
};

struct Fixture {
    std::vector<uint16_t> framebuffer;
    std::vector<uint16_t> depth;
    Renderer::Camera camera;
    Renderer::DirectionalLight light;
    Renderer::AmbientLight ambient;
    std::unique_ptr<Renderer::Scene> scene;

    Fixture()
        : framebuffer(kPixels, 0), depth(kPixels, 0xFFFF),
          light({20, 40, 0}, {255, 255, 255}, 220), ambient({70, 70, 70}) {
        camera.nearPlane = 100;
        camera.farPlane  = 1500;
        camera.setFOV(70.0f, kWidth);
        scene.reset(new Renderer::Scene(framebuffer.data(), depth.data(), kWidth, kHeight));
        scene->setCamera(&camera);
        scene->setDirectionalLight(&light);
        scene->setAmbientLight(&ambient);
        scene->setBackcolor(0);
    }

    Snapshot render(const Renderer::PickQuery* query = nullptr, int queryCount = 0) {
        std::fill(framebuffer.begin(), framebuffer.end(), 0);
        std::fill(depth.begin(), depth.end(), 0xFFFF);
        if (query && queryCount > 0) scene->setPickQueries(query, queryCount);
        else scene->setPickQueries(nullptr, 0);
        scene->prepareFrame();
        scene->rasterizeBand(0, kHeight);
        Snapshot s;
        s.pixels = framebuffer;
        s.drawnTriangles = scene->lastFrameDrawnTriangles;
        s.rasterizedTriangles = scene->lastFrameRasterizedTriangles;
        s.drawnObjects = scene->lastFrameDrawnObjects;
        return s;
    }

    const Renderer::PickResult* picks() const { return scene->getPickResults(); }
};

inline bool pixelsMatch(const std::vector<uint16_t>& expected,
                        const std::vector<uint16_t>& actual,
                        int tolerance, const char* label) {
    int worst = 0, bad = 0;
    for (int i = 0; i < kPixels; ++i) {
        const int ar = (expected[i] >> 11) & 0x1F, ag = (expected[i] >> 5) & 0x3F, ab = expected[i] & 0x1F;
        const int br = (actual[i] >> 11) & 0x1F, bg = (actual[i] >> 5) & 0x3F, bb = actual[i] & 0x1F;
        const int dr = std::abs(ar - br), dg = std::abs(ag - bg), db = std::abs(ab - bb);
        const int d = std::max({dr, dg, db});
        worst = std::max(worst, d);
        if (dr > tolerance || dg > tolerance || db > tolerance) ++bad;
    }
    if (bad) {
        std::printf("  %s: %d pixels exceed tolerance %d (worst channel delta %d)\n",
                    label, bad, tolerance, worst);
        return false;
    }
    return true;
}

inline int nonBackgroundPixels(const std::vector<uint16_t>& pixels) {
    int n = 0;
    for (uint16_t p : pixels) if (p != 0) ++n;
    return n;
}

// Material helpers -----------------------------------------------------------
inline Renderer::Material litMaterial(uint16_t colour, uint8_t specular = 0) {
    Renderer::Material m(colour, nullptr, nullptr, false, 255, 255, specular);
    m.shadingMode = Renderer::ShadingMode::FLAT;
    return m;
}

inline Renderer::Material emissiveMaterial(uint16_t colour) {
    Renderer::Material m(colour, nullptr, nullptr, true, 255, 255, 0);
    m.shadingMode = Renderer::ShadingMode::UNLIT;
    return m;
}

// A two-face prototype with generic normals so any wrong normal transform
// changes shading measurably.
inline Renderer::Object::SharedMesh makeLitPrototype(Renderer::Material* material) {
    using Renderer::Object;
    Object authored;
    authored.addVertex({{-40, -30, 0}, {0, 0}, {0, 0, 0}});
    authored.addVertex({{ 90, -10, 20}, {1024, 0}, {0, 0, 0}});
    authored.addVertex({{ 20,  90, 60}, {0, 1024}, {0, 0, 0}});
    authored.addTriangle(0, 1, 2, material);
    authored.addVertex({{-60, 20, 70}, {0, 0}, {0, 0, 0}});
    authored.addVertex({{ 40, 80, 10}, {1024, 0}, {0, 0, 0}});
    authored.addVertex({{ 70, -20, 90}, {0, 1024}, {0, 0, 0}});
    authored.addTriangle(3, 4, 5, material);
    authored.computeFlatNormals();
    return Object::freezeMesh(std::move(authored));
}

// A four-vertex plate whose authored front faces the camera at -Z; used for
// front/back and picking checks. (Winding is reversed relative to a +Z plate
// so CULL_BACKFACES keeps it visible from a camera looking down +Z.)
inline Renderer::Object::SharedMesh makePlatePrototype(Renderer::Material* material) {
    using Renderer::Object;
    Object authored;
    authored.addVertex({{-300, -300, 0}, {0, 0}, {0, 0, 0}});
    authored.addVertex({{ 300, -300, 0}, {1024, 0}, {0, 0, 0}});
    authored.addVertex({{ 300,  300, 0}, {1024, 1024}, {0, 0, 0}});
    authored.addVertex({{-300,  300, 0}, {0, 1024}, {0, 0, 0}});
    authored.addTriangle(0, 2, 1, material);
    authored.addTriangle(0, 3, 2, material);
    authored.computeFlatNormals();
    return Object::freezeMesh(std::move(authored));
}

// Non-uniform scale (x -2, z 3), two shears and a mirror; det = -6.
inline Renderer::Object::InstanceTransform affineMirror(Vector3 position) {
    Renderer::Object::InstanceTransform t;
    const float basis[9] = { -2, 1, 0,  0, 1, 0,  0, 1, 3 };
    std::copy(basis, basis + 9, t.basis);
    t.position = position;
    return t;
}

// Same scale/shear shape with a positive determinant.
inline Renderer::Object::InstanceTransform affinePositive(Vector3 position) {
    Renderer::Object::InstanceTransform t;
    const float basis[9] = { 2, 1, 0,  0, 1, 0,  0, 1, 3 };
    std::copy(basis, basis + 9, t.basis);
    t.position = position;
    return t;
}

inline bool boundsContain(const Renderer::Object& batch, const Renderer::Object& reference) {
    for (const auto& v : reference.vertices) {
        if (v.position.x < batch.boundingBoxMin.x || v.position.x > batch.boundingBoxMax.x) return false;
        if (v.position.y < batch.boundingBoxMin.y || v.position.y > batch.boundingBoxMax.y) return false;
        if (v.position.z < batch.boundingBoxMin.z || v.position.z > batch.boundingBoxMax.z) return false;
    }
    return true;
}

} // namespace AffineCheck

#endif // AFFINE_SUPPORT_HPP
