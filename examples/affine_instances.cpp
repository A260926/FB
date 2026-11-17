/// @file affine_instances.cpp
/// @brief Headless example: one frozen prototype placed many times with
///        finite, invertible affine instance transforms (scale, shear and
///        mirror). No window is created; an offscreen framebuffer and the
///        split prepareFrame()/rasterizeBand() pipeline drive every frame.
///
/// Build (desktop):
///   c++ -std=c++17 -I. -Itask_support -Isrc \
///       examples/affine_instances.cpp -L.task-build -ljet -pthread -o affine_demo
///
/// Key points demonstrated below:
///   * addInstance()/setInstanceTransform() reject a non-finite or singular
///     basis and return false without touching existing placements.
///   * setInstanceTransform() is called ONLY between frames — after the
///     previous frame's rasterizeBand() has finished and before the next
///     prepareFrame(). It refreshes the batch bounding box itself, so no
///     manual calculateBoundingBox() and no prototype rebuild is needed.
///   * The prototype's vertices, triangles and position cache stay immutable;
///     every placement shares the same frozen mesh.
///   * A negative-determinant basis mirrors geometry while keeping the
///     prototype's front face (the renderer compensates triangle winding).

#include "Jet.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace Renderer;

namespace {

constexpr int W = 160;
constexpr int H = 120;

// A small two-triangle prototype; normals are explicit so lighting has real
// geometry to work with after scaling and shearing.
Object::SharedMesh makePrototype(Material& paint) {
    Object authored;
    struct Face { Vector3 a, b, c; };
    const Face faces[2] = {
        {{-40, -40, 0}, {40, -40, 0}, {0, 40, 20}},
        {{-40, 20, -20}, {20, 20, -20}, {-20, -40, 40}},
    };
    for (const Face& f : faces) {
        const Vector3 u = f.b - f.a;
        const Vector3 v = f.c - f.a;
        float nx = (float)(u.y * v.z - u.z * v.y);
        float ny = (float)(u.z * v.x - u.x * v.z);
        float nz = (float)(u.x * v.y - u.y * v.x);
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        nx = nx * (float)FIXED_POINT_SCALE / len;
        ny = ny * (float)FIXED_POINT_SCALE / len;
        nz = nz * (float)FIXED_POINT_SCALE / len;
        const Vector3 normal((int32_t)nx, (int32_t)ny, (int32_t)nz);
        const uint16_t base = (uint16_t)authored.vertices.size();
        authored.addVertex({f.a, {0, 0}, normal});
        authored.addVertex({f.b, {0, 0}, normal});
        authored.addVertex({f.c, {0, 0}, normal});
        authored.addTriangle(base, base + 1, base + 2, &paint);
    }
    return Object::freezeMesh(std::move(authored));
}

Object::InstanceTransform affine(const float basis[9], const Vector3& position) {
    Object::InstanceTransform t;
    for (int i = 0; i < 9; ++i) t.basis[i] = basis[i];
    t.position = position;
    return t;
}

} // namespace

int main() {
    Material paint(0xF800);
    paint.shadingMode = ShadingMode::FLAT;

    // Author once. Both instances below share this single prototype; its
    // vertex/triangle storage and position cache are never copied or rebuilt.
    Object::SharedMesh prototype = makePrototype(paint);
    if (!prototype) {
        std::printf("Failed to freeze prototype\n");
        return 1;
    }

    // Non-uniform scale + shear (det > 0) ...
    const float stretched[9] = {
        1.5f, 0.25f, 0.0f,
        0.0f, 0.75f, 0.0f,
        0.5f, 0.0f,  1.25f};
    // ... and a mirrored, scaled basis (det < 0). The mirror keeps the
    // prototype's front face because the renderer swaps the affected winding.
    const float mirrored[9] = {
       -1.2f, 0.5f, 0.0f,
        0.0f, 0.8f, 0.25f,
        0.0f, 0.0f, 1.0f};

    Object batch;
    if (!batch.addInstance(prototype, affine(stretched, {-70, 0, 0})) ||
        !batch.addInstance(prototype, affine(mirrored, {70, 0, 0}))) {
        std::printf("Rejected a valid placement\n");
        return 1;
    }
    batch.position = {0, 0, 700};
    batch.calculateBoundingBox(); // initial bounds before the first frame

    // Invalid placements are refused and leave the batch exactly as it was.
    Object::InstanceTransform singular;
    singular.basis[8] = 0.0f; // collapses the z axis -> determinant zero
    if (batch.addInstance(prototype, singular) ||
        batch.setInstanceTransform(0, singular)) {
        std::printf("A singular basis was wrongly accepted\n");
        return 1;
    }

    std::vector<uint16_t> framebuffer(W * H, 0);
    std::vector<uint16_t> depth(W * H, 65535);

    Camera camera;
    camera.nearPlane = 100;
    camera.farPlane = 4000;
    camera.setFOV(70.0f, W);

    DirectionalLight light(Vector3(0, 0, 0), Color{255, 255, 255}, 255);
    AmbientLight ambient({40, 40, 40});

    Scene scene(framebuffer.data(), depth.data(), W, H);
    scene.setCamera(&camera);
    scene.setDirectionalLight(&light);
    scene.setAmbientLight(&ambient);
    scene.addObject(&batch);

    for (int frame = 0; frame < 4; ++frame) {
        // ---- Frame phase: transform, cull, queue, then rasterize. ----
        scene.prepareFrame();
        scene.rasterizeBand(0, H);

        std::printf("frame %d: drew %d triangles; bounds x=[%d,%d]\n",
                    frame, scene.lastFrameDrawnTriangles,
                    batch.boundingBoxMin.x, batch.boundingBoxMax.x);

        // ---- Update phase: only AFTER the previous frame finished. ----
        // Slide the stretched instance well beyond its old box. The call
        // swaps the placement and refreshes the batch bounding box, so the
        // next prepareFrame() culls and lights from the new placement with no
        // manual calculateBoundingBox() and no per-vertex prototype rebuild.
        const Vector3 nextPosition(frame * 400 - 600, 0, 0);
        if (!batch.setInstanceTransform(0, affine(stretched, nextPosition))) {
            std::printf("setInstanceTransform rejected a valid placement\n");
            return 1;
        }
        // Direct edits to the public instances array remain supported, but
        // those follow the old convention and need an explicit
        // batch.calculateBoundingBox() before the next frame.
    }

    // The shared prototype is still the single, untouched source of geometry.
    std::printf("Shared prototype triangles=%zu, batch placements=%zu\n",
                prototype->triangles.size(), batch.instances.size());
    return 0;
}
