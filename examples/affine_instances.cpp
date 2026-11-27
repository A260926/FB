/// @file affine_instances.cpp
/// @brief Affine instance placements: scale, shear, mirror and per-frame updates.
///
/// This example shows how a single frozen prototype is shared while each
/// placement applies a finite, invertible affine basis (non-uniform scale,
/// shear and mirror) on top of the translation. It also demonstrates the
/// update timing: setInstanceTransform() is called after the previous frame
/// has finished drawing and before the next prepareFrame(), and it refreshes
/// the batch bounds in place so no manual calculateBoundingBox() is needed.
///
/// An affine basis maps prototype-local positions into owner-local space
/// (`basis * p + position`). Normals are handled internally with the inverse
/// transpose and renormalised, so lighting stays perpendicular to the
/// transformed surface; mirroring (a negative determinant) keeps the
/// prototype's authored front face front for every CullingMode.

#include "Jet.hpp"

namespace AffineInstancesExample {

enum class Placement {
    kStretch,   // non-uniform scale + shear, positive determinant
    kMirror,    // the same shape reflected across X (negative determinant)
};

// Build one shared prototype once; every placement reuses it.
Renderer::Object::SharedMesh buildSharedPrototype(Renderer::Material* material) {
    Renderer::Object authored;
    // A small folded two-face plate so the lighting response is visible when
    // the affine basis stretches and shears it.
    authored.addVertex({{-40, -30,   0}, {0, 0}, {0, 0, 0}});
    authored.addVertex({{ 90, -10,  20}, {1024, 0}, {0, 0, 0}});
    authored.addVertex({{ 20,  90,  60}, {0, 1024}, {0, 0, 0}});
    authored.addTriangle(0, 1, 2, material);
    authored.addVertex({{-60, 20, 70}, {0, 0}, {0, 0, 0}});
    authored.addVertex({{ 40, 80, 10}, {1024, 0}, {0, 0, 0}});
    authored.addVertex({{  70, -20, 90}, {0, 1024}, {0, 0, 0}});
    authored.addTriangle(3, 4, 5, material);
    authored.computeFlatNormals();
    // freezeMesh prepares bounds and packed positions once and moves the
    // geometry into an immutable, reference-counted prototype.
    return Renderer::Object::freezeMesh(std::move(authored));
}

Renderer::Object::InstanceTransform makePlacement(Placement kind, Vector3 position) {
    Renderer::Object::InstanceTransform transform;
    // Basis is row-major and acts on column vectors.
    //   kStretch: x doubled, z tripled, plus an x<-y and y<-z shear.
    //   kMirror:  the same shape with x negated, which flips the determinant.
    const float stretch[9] = { 2, 1, 0,  0, 1, 0,  0, 1, 3 };
    const float mirror[9]  = { -2, 1, 0,  0, 1, 0,  0, 1, 3 };
    std::copy(kind == Placement::kMirror ? mirror : stretch,
              (kind == Placement::kMirror ? mirror : stretch) + 9, transform.basis);
    transform.position = position;
    return transform;
}

// One owner shared by all placements, plus a unique piece of geometry.
class AffineScene {
public:
    AffineScene(Renderer::Scene* scene, Renderer::Material* material)
        : prototype_(buildSharedPrototype(material)), material_(material) {
        scene_ = scene;
        scene_->addObject(&batch_);
        batch_.cullingMode = Renderer::CullingMode::CULL_BACKFACES;

        // Two shared instances of the same prototype. The first is mirrored,
        // the second stretched; both borrow the prototype and a material.
        batch_.addInstance(prototype_, makePlacement(Placement::kMirror,  {-60, 0, 380}), material_);
        batch_.addInstance(prototype_, makePlacement(Placement::kStretch, { 80, 0, 520}), material_);
        // Initial placement: compute the batch bounds once.
        batch_.calculateBoundingBox();
    }

    // Re-place an instance between frames.
    //
    // Call this AFTER the previous frame has finished drawing (rasterisation
    // has read the old transforms) and BEFORE the next Scene::prepareFrame().
    // On success the next prepareFrame() uses the new placement and the batch
    // bounding box is refreshed in place, so the caller never calls
    // calculateBoundingBox() and the frozen prototype is never rebuilt.
    // Non-finite or singular bases are rejected and leave the existing
    // instance, bounds and material binding untouched.
    bool moveInstance(size_t index, Vector3 localPosition, Placement kind) {
        return batch_.setInstanceTransform(index, makePlacement(kind, localPosition));
    }

    bool rebindMaterial(size_t index, Renderer::Material* material) {
        batch_.instances[index].materialOverride = material;
        return true;
    }

    const Renderer::Object& batch() const { return batch_; }

private:
    Renderer::Object::SharedMesh prototype_;
    Renderer::Material* material_;
    Renderer::Object batch_;
    Renderer::Scene* scene_ = nullptr;
};

// Sketch of a per-frame loop. The key ordering is the update window above.
//
//   AffineScene world(&scene, &paint);
//   for (int frame = 0; ; ++frame) {
//       const bool mirror = (frame / 30) % 2 == 0;
//       // Optional: move a placement between frame draws.
//       world.moveInstance(0, { -60, 0, 380 + (frame % 60) }, mirror ? kMirror : kStretch);
//       // Or swap the borrowed material; no bounds refresh is required.
//       world.rebindMaterial(1, (frame % 120) < 60 ? &paint : &alt);
//       scene.render();   // prepareFrame() + rasteriseBand() + post-FX
//   }
//
// With the explicit pipeline, replace scene.render() with:
//   scene.prepareFrame(); scene.rasterizeBand(0, height);

} // namespace AffineInstancesExample
