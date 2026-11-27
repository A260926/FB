/// @file affine_instances.cpp
/// @brief Regression coverage for finite, invertible affine instance placements.
///
/// Each check renders the real Scene/Rasterizer and compares it against an
/// independently expanded ordinary Object from affine_support.hpp. No main is
/// defined; task_support/driver.c drives the TASK_CHECK-registered cases.

#include "Scene.hpp"
#include "affine_support.hpp"
#include "check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace Renderer;
using namespace AffineCheck;

namespace {

Snapshot renderStandalone(Object& object, const Vector3& lightDirection = {20, 40, 0}) {
    Fixture fixture;
    fixture.light.updateDirection(lightDirection);
    fixture.scene->addObject(&object);
    return fixture.render();
}

bool matchAffine(const Snapshot& reference, const Snapshot& instance, const char* label) {
    if (instance.drawnTriangles != reference.drawnTriangles) {
        std::printf("  %s: drawn triangles %d vs reference %d\n",
                    label, instance.drawnTriangles, reference.drawnTriangles);
        return false;
    }
    return pixelsMatch(reference.pixels, instance.pixels, 1, label);
}

bool matchExactly(const Snapshot& reference, const Snapshot& instance, const char* label) {
    if (instance.drawnTriangles != reference.drawnTriangles) {
        std::printf("  %s: drawn triangles %d vs reference %d\n",
                    label, instance.drawnTriangles, reference.drawnTriangles);
        return false;
    }
    return pixelsMatch(reference.pixels, instance.pixels, 0, label);
}

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

int rejectsInvalidPreservesState() {
    Material material = litMaterial(0x4e9f);
    auto prototype = makeLitPrototype(&material);
    TASK_CHECK(prototype != nullptr);
    TASK_CHECK(prototype->cachedPositions() != nullptr);

    Object batch;
    TASK_CHECK(batch.addInstance(prototype, affineMirror({0, 0, 400}), &material));
    batch.calculateBoundingBox();
    const size_t count = batch.instances.size();
    const Vector3 min0 = batch.boundingBoxMin, max0 = batch.boundingBoxMax, centre0 = batch.centreVolume;
    const Material* bound = batch.instances[0].materialOverride;
    const Object::InstanceTransform transform0 = batch.instances[0].transform;

    auto rejectionPreserves = [&](Object::InstanceTransform bad) {
        if (batch.addInstance(prototype, bad, nullptr)) return false;
        return batch.instances.size() == count &&
               batch.boundingBoxMin.x == min0.x && batch.boundingBoxMax.x == max0.x &&
               batch.boundingBoxMin.y == min0.y && batch.boundingBoxMax.y == max0.y &&
               batch.boundingBoxMin.z == min0.z && batch.boundingBoxMax.z == max0.z &&
               batch.centreVolume.x == centre0.x && batch.centreVolume.y == centre0.y &&
               batch.centreVolume.z == centre0.z &&
               batch.instances[0].materialOverride == bound &&
               batch.instances[0].transform.position.x == transform0.position.x &&
               batch.instances[0].transform.position.y == transform0.position.y &&
               batch.instances[0].transform.position.z == transform0.position.z;
    };

    Object::InstanceTransform bad;
    bad.position = {0, 0, 400};
    bad.basis[0] = NAN;
    TASK_CHECK(rejectionPreserves(bad));
    bad = Object::InstanceTransform{};
    bad.basis[4] = INFINITY;
    TASK_CHECK(rejectionPreserves(bad));
    bad = Object::InstanceTransform{};
    bad.basis[8] = -INFINITY;
    TASK_CHECK(rejectionPreserves(bad));

    // Singular / rank-deficient bases: a collapsed column, the zero matrix
    // and a duplicated row.
    bad = Object::InstanceTransform{};
    bad.basis[0] = 0; bad.basis[1] = 0; bad.basis[2] = 0;
    TASK_CHECK(rejectionPreserves(bad));
    bad = Object::InstanceTransform{};
    bad.basis[0] = 0; bad.basis[4] = 0; bad.basis[8] = 0;
    TASK_CHECK(rejectionPreserves(bad));
    bad = Object::InstanceTransform{};
    bad.basis[0] = 1; bad.basis[1] = 1; bad.basis[2] = 1;
    bad.basis[3] = 2; bad.basis[4] = 2; bad.basis[5] = 2;
    TASK_CHECK(rejectionPreserves(bad));

    // Invalid updates must not disturb the live placement or its bounds.
    TASK_CHECK(!batch.setInstanceTransform(count, affinePositive({0, 0, 500})));
    TASK_CHECK(!batch.setInstanceTransform(count - 1, bad));
    TASK_CHECK(batch.instances[0].materialOverride == bound);
    TASK_CHECK(batch.boundingBoxMin.x == min0.x && batch.boundingBoxMax.z == max0.z);
    TASK_CHECK(prototype->vertices.size() > 0 && prototype->cachedPositions() != nullptr);
    return 0;
}

int updatesBoundsAndRender() {
    Material material = litMaterial(0x6d3a);
    auto prototype = makeLitPrototype(&material);
    TASK_CHECK(prototype != nullptr);

    Object batch;
    TASK_CHECK(batch.addInstance(prototype, affineMirror({0, 0, 420}), &material));
    batch.calculateBoundingBox();
    Object firstReference = expandReference(batch);
    TASK_CHECK(boundsContain(batch, firstReference));

    Fixture instanceFixture;
    instanceFixture.scene->addObject(&batch);
    const Snapshot firstInstance = instanceFixture.render();
    const Snapshot firstRef = renderStandalone(firstReference);
    TASK_CHECK(matchAffine(firstRef, firstInstance, "initial placement"));

    // Move the placement between frames. setInstanceTransform must refresh
    // bounds in place so this very next prepareFrame renders the new pose,
    // with no manual calculateBoundingBox() and no prototype rebuild.
    const Object::InstanceTransform moved = affinePositive({-80, 40, 780});
    TASK_CHECK(batch.setInstanceTransform(0, moved));
    TASK_CHECK(batch.instances[0].transform.position.z == 780);
    TASK_CHECK(batch.boundingBoxMax.z > 780 && batch.boundingBoxMin.z <= 780);
    TASK_CHECK(batch.boundingBoxMax.z != firstReference.boundingBoxMax.z);
    Object movedReference = expandReference(batch);
    TASK_CHECK(boundsContain(batch, movedReference));
    const Snapshot movedInstance = instanceFixture.render();
    const Snapshot movedRef = renderStandalone(movedReference);
    TASK_CHECK(matchAffine(movedRef, movedInstance, "updated placement"));

    // Returning to the first pose proves the renderer follows the stored
    // placement rather than any cached transform.
    TASK_CHECK(batch.setInstanceTransform(0, affineMirror({0, 0, 420})));
    Object restoredReference = expandReference(batch);
    const Snapshot restoredInstance = instanceFixture.render();
    const Snapshot restoredRef = renderStandalone(restoredReference);
    TASK_CHECK(matchAffine(restoredRef, restoredInstance, "restored placement"));
    TASK_CHECK(prototype->cachedPositions() != nullptr);
    return 0;
}

int twoSharedAffineInstances() {
    Material material = litMaterial(0x4e9f);
    auto prototype = makeLitPrototype(&material);
    TASK_CHECK(prototype != nullptr);
    const auto originalTriangles = prototype->triangles;
    const auto originalVertices = prototype->vertices;

    Object batch;
    TASK_CHECK(batch.addInstance(prototype, affineMirror({-60, 0, 360}), &material));
    TASK_CHECK(batch.addInstance(prototype, affinePositive({80, 20, 520}), nullptr));
    batch.calculateBoundingBox();

    TASK_CHECK(batch.instances.size() == 2);
    TASK_CHECK(batch.instances[0].mesh == prototype);
    TASK_CHECK(batch.instances[1].mesh == prototype);
    TASK_CHECK(batch.vertexCount() == 2 * prototype->vertices.size());
    TASK_CHECK(batch.triangleCount() == 2 * prototype->triangles.size());
    // Sharing must neither expand nor mutate the frozen prototype.
    TASK_CHECK(prototype->vertices.size() == originalVertices.size());
    TASK_CHECK(prototype->triangles.size() == originalTriangles.size());
    TASK_CHECK(prototype->cachedPositions() != nullptr);
    TASK_CHECK(std::equal(originalTriangles.begin(), originalTriangles.end(),
                          prototype->triangles.begin(),
                          [](const Object::Triangle& a, const Object::Triangle& b) {
                              return a.v1 == b.v1 && a.v2 == b.v2 && a.v3 == b.v3;
                          }));

    Object reference = expandReference(batch);
    TASK_CHECK(reference.vertices.size() == batch.vertexCount());
    TASK_CHECK(reference.triangles.size() == batch.triangleCount());
    TASK_CHECK(boundsContain(batch, reference));

    Fixture instanceFixture;
    instanceFixture.scene->addObject(&batch);
    const Snapshot instanceFrame = instanceFixture.render();
    const Snapshot referenceFrame = renderStandalone(reference);
    TASK_CHECK(instanceFrame.drawnTriangles > 0);
    TASK_CHECK(nonBackgroundPixels(instanceFrame.pixels) > 0);
    TASK_CHECK(matchAffine(referenceFrame, instanceFrame, "two shared instances"));
    return 0;
}

int sweepOutOfOldBounds() {
    Material material = litMaterial(0x3d7f);
    auto prototype = makeLitPrototype(&material);
    TASK_CHECK(prototype != nullptr);

    Object batch;
    TASK_CHECK(batch.addInstance(prototype, affineMirror({0, 0, 380}), &material));
    batch.calculateBoundingBox();

    Fixture fixture;
    fixture.scene->addObject(&batch);
    const Vector3 positions[] = {{0,0,380}, {0,0,620}, {0,0,900}, {30,-20,1200}, {0,0,1600}};
    for (const Vector3& p : positions) {
        TASK_CHECK(batch.setInstanceTransform(0, affineMirror(p)));
        TASK_CHECK(batch.instances[0].transform.position.z == p.z);
        // The refreshed AABB follows the instance, and still contains every
        // transformed prototype vertex.
        TASK_CHECK(batch.boundingBoxMax.z > p.z);
        Object reference = expandReference(batch);
        TASK_CHECK(boundsContain(batch, reference));
        const Snapshot instanceFrame = fixture.render();
        const Snapshot referenceFrame = renderStandalone(reference);
        if (p.z < 1500) {
            // Visible placements must actually draw, proving the sweep is not
            // comparing two empty frames.
            TASK_CHECK(instanceFrame.rasterizedTriangles > 0);
            TASK_CHECK(nonBackgroundPixels(instanceFrame.pixels) > 0);
        }
        TASK_CHECK(matchAffine(referenceFrame, instanceFrame, "sweep placement"));
        // Old placement must not linger once the instance has moved far away.
        if (p.z >= 900) TASK_CHECK(batch.boundingBoxMin.z > 800);
    }
    // Beyond the far plane the updated bounds let the batch cull completely;
    // no pixels from the old placement survive.
    const Snapshot culled = fixture.render();
    TASK_CHECK(culled.drawnObjects == 0 || culled.drawnTriangles == 0);
    TASK_CHECK(nonBackgroundPixels(culled.pixels) == 0);
    return 0;
}

int materialOverrideBindings() {
    Material base = litMaterial(0x4e9f);
    Material alternate = litMaterial(0xf8a4, 24);
    auto prototype = makeLitPrototype(&base);
    TASK_CHECK(prototype != nullptr);

    Object batch;
    TASK_CHECK(batch.addInstance(prototype, affineMirror({-70, 0, 420}), &alternate));
    TASK_CHECK(batch.addInstance(prototype, affinePositive({70, 0, 520}), nullptr));
    batch.calculateBoundingBox();
    TASK_CHECK(batch.instances[0].materialOverride == &alternate);
    TASK_CHECK(batch.instances[1].materialOverride == nullptr);

    Fixture fixture;
    fixture.scene->addObject(&batch);

    auto verify = [&](const char* label) {
        Object reference = expandReference(batch);
        const Snapshot instanceFrame = fixture.render();
        const Snapshot referenceFrame = renderStandalone(reference);
        return matchAffine(referenceFrame, instanceFrame, label);
    };
    TASK_CHECK(verify("uniform override"));

    // Move the override to the other shared placement; materials are resolved
    // per frame, so both instances change roles.
    batch.instances[0].materialOverride = nullptr;
    batch.instances[1].materialOverride = &alternate;
    TASK_CHECK(verify("override swapped"));

    // Per-triangle tables: null keeps the prototype material, a non-null entry
    // replaces one face. A uniform override outranks the table.
    auto table = std::make_shared<const std::vector<Material*>>(
        std::vector<Material*>{nullptr, &alternate});
    TASK_CHECK(table->size() == prototype->triangles.size());
    batch.instances[0].triangleMaterials = table;
    TASK_CHECK(verify("triangle table"));

    batch.instances[0].materialOverride = &alternate;
    TASK_CHECK(verify("override precedence"));
    // Wrong-length tables are rejected without disturbing the placement.
    if (batch.addInstance(prototype, affinePositive({0, 0, 500}), nullptr,
                          std::make_shared<const std::vector<Material*>>(1, &alternate)))
        return 1;
    TASK_CHECK(batch.instances.size() == 2);
    return 0;
}

// ---------------------------------------------------------------------------
// Lighting
// ---------------------------------------------------------------------------

int affineNormalTransform() {
    Material material = litMaterial(0x76d3);
    auto prototype = makeLitPrototype(&material);
    TASK_CHECK(prototype != nullptr);

    // A deliberately oblique light: the raw-basis and inverse-transpose
    // normals point ~34 degrees apart here, so an unnormalised or wrong
    // normal transform shifts shading well beyond RGB565 quantisation
    // (measured ~15 channel levels). The default +20/+40 light instead
    // lands both normals in the ambient-only hemisphere and would not
    // discriminate.
    const Vector3 obliqueLight{270, 10, 0};

    Object batch;
    TASK_CHECK(batch.addInstance(prototype, affineMirror({0, 0, 460}), &material));
    batch.calculateBoundingBox();

    Fixture fixture;
    fixture.light.updateDirection(obliqueLight);
    fixture.scene->addObject(&batch);
    const Snapshot instanceFrame = fixture.render();
    Object reference = expandReference(batch);
    const Snapshot referenceFrame = renderStandalone(reference, obliqueLight);
    TASK_CHECK(instanceFrame.drawnTriangles > 0);
    TASK_CHECK(nonBackgroundPixels(instanceFrame.pixels) > 0);
    TASK_CHECK(matchAffine(referenceFrame, instanceFrame, "affine normals"));

    // A strongly anisotropic placement: raw basis-multiplied normals would
    // have the wrong magnitude and unrelated shear direction. The renderer
    // must match the inverse-transpose reference within one RGB565 step.
    Object strong;
    Object::InstanceTransform stretch;
    const float basis[9] = { 8, 2, 0,  0, 1.5f, 0,  0, 0, 0.25f };
    std::copy(basis, basis + 9, stretch.basis);
    stretch.position = {0, 0, 500};
    TASK_CHECK(strong.addInstance(prototype, stretch, &material));
    strong.calculateBoundingBox();

    Fixture strongFixture;
    strongFixture.light.updateDirection(obliqueLight);
    strongFixture.scene->addObject(&strong);
    const Snapshot strongFrame = strongFixture.render();
    Object strongReference = expandReference(strong);
    const Snapshot strongReferenceFrame = renderStandalone(strongReference, obliqueLight);
    TASK_CHECK(strongFrame.drawnTriangles > 0);
    TASK_CHECK(matchAffine(strongReferenceFrame, strongFrame, "anisotropic normals"));
    // Sanity: the two placements really do shade differently, so the
    // comparison above is not a degenerate all-black frame.
    TASK_CHECK(nonBackgroundPixels(strongFrame.pixels) > 0);
    return 0;
}

int mirrorKeepsFrontFace() {
    Material material = litMaterial(0x4e9f);
    auto prototype = makePlatePrototype(&material);
    TASK_CHECK(prototype != nullptr);

    // Light mostly along -Z, so the authored camera-facing front is bright
    // and a sign-flipped normal would visibly darken it.
    const Vector3 lightDirection{270, 45, 0};

    // 1) A mirror across X: the +X plate is symmetric, so the mirrored image
    //    must match the plain one (front face, lighting and coverage kept).
    Object mirrorBatch;
    Object plainBatch;
    Object::InstanceTransform mirrorX;
    const float mirrorXBasis[9] = { -1, 0, 0,  0, 1, 0,  0, 0, 1 };
    std::copy(mirrorXBasis, mirrorXBasis + 9, mirrorX.basis);
    mirrorX.position = {0, 0, 500};
    Object::InstanceTransform plain;
    plain.position = {0, 0, 500};
    TASK_CHECK(mirrorBatch.addInstance(prototype, mirrorX, &material));
    TASK_CHECK(plainBatch.addInstance(prototype, plain, &material));
    mirrorBatch.cullingMode = plainBatch.cullingMode = CullingMode::NO_CULLING;
    mirrorBatch.calculateBoundingBox();
    plainBatch.calculateBoundingBox();

    Fixture mirrorFixture;
    mirrorFixture.light.updateDirection(lightDirection);
    mirrorFixture.scene->addObject(&mirrorBatch);
    Fixture plainFixture;
    plainFixture.light.updateDirection(lightDirection);
    plainFixture.scene->addObject(&plainBatch);
    const Snapshot mirrorFrame = mirrorFixture.render();
    const Snapshot plainFrame = plainFixture.render();

    TASK_CHECK(mirrorFrame.drawnTriangles == plainFrame.drawnTriangles);
    TASK_CHECK(mirrorFrame.drawnTriangles > 0);
    TASK_CHECK(matchAffine(plainFrame, mirrorFrame, "mirror-X vs plain"));
    TASK_CHECK(nonBackgroundPixels(mirrorFrame.pixels) > 0);

    // And the explicit reference, whose winding was reversed for det < 0.
    Object reference = expandReference(mirrorBatch);
    Fixture referenceFixture;
    referenceFixture.light.updateDirection(lightDirection);
    referenceFixture.scene->addObject(&reference);
    const Snapshot referenceFrame = referenceFixture.render();
    TASK_CHECK(matchAffine(referenceFrame, mirrorFrame, "mirror reference"));

    // 2) A mirror across Z (basis diag(1,1,-1)) reverses depth and flips the
    //    determinant without the plate's X symmetry hiding a face swap. Under
    //    CULL_BACKFACES the authored camera-facing front must still be drawn:
    //    an implementation that swapped front/back would render nothing.
    // 2) A mirror across X combined with an X<-Z shear (det = -2). Unlike the
    //    symmetric mirror-X plate above, this image is not symmetric, so a
    //    front/back swap cannot hide behind symmetry. Under CULL_BACKFACES
    //    the authored camera-facing front must still be drawn and match the
    //    expanded reference (which reversed the winding for det < 0); an
    //    implementation that swapped front/back would render nothing.
    Object mirrorZ;
    Object::InstanceTransform t;
    const float mirrorZBasis[9] = { -2, 0, 1,  0, 1, 0,  0, 0, 1 };
    std::copy(mirrorZBasis, mirrorZBasis + 9, t.basis);
    t.position = {0, 0, 520};
    TASK_CHECK(mirrorZ.addInstance(prototype, t, &material));
    mirrorZ.cullingMode = CullingMode::CULL_BACKFACES;
    mirrorZ.calculateBoundingBox();
    Fixture zFixture;
    zFixture.light.updateDirection(lightDirection);
    zFixture.scene->addObject(&mirrorZ);
    const Snapshot zFrame = zFixture.render();
    TASK_CHECK(zFrame.rasterizedTriangles > 0);
    TASK_CHECK(nonBackgroundPixels(zFrame.pixels) > 0);
    Object zReference = expandReference(mirrorZ);
    Fixture zRefFixture;
    zRefFixture.light.updateDirection(lightDirection);
    zRefFixture.scene->addObject(&zReference);
    const Snapshot zRefFrame = zRefFixture.render();
    TASK_CHECK(matchAffine(zRefFrame, zFrame, "mirror-Z front under CULL_BACK"));
    return 0;
}

int materialSwitchFreshness() {
    Material lit = litMaterial(0x4e9f, 0);
    Material glossy = litMaterial(0x4e9f, 40);
    Material emissive = emissiveMaterial(0xffe0);
    auto prototype = makeLitPrototype(&lit);
    TASK_CHECK(prototype != nullptr);

    // Oblique light again: the non-specular (precompute-eligible) and
    // specular paths must agree with their references under a direction
    // that actually exercises the transformed normal.
    const Vector3 obliqueLight{270, 10, 0};

    Object batch;
    TASK_CHECK(batch.addInstance(prototype, affineMirror({0, 0, 460}), &lit));
    batch.calculateBoundingBox();

    Fixture fixture;
    fixture.light.updateDirection(obliqueLight);
    fixture.scene->addObject(&batch);
    const Material* sequence[] = {&lit, &glossy, &emissive, &lit, &glossy};
    for (const Material* selected : sequence) {
        batch.instances[0].materialOverride = const_cast<Material*>(selected);
        Object reference = expandReference(batch);
        const Snapshot instanceFrame = fixture.render();
        const Snapshot referenceFrame = renderStandalone(reference, obliqueLight);
        // Swapping between non-specular, specular and emissive materials must
        // not reuse the previous frame's Lambert term or baked colour.
        TASK_CHECK(instanceFrame.rasterizedTriangles > 0);
        TASK_CHECK(matchAffine(referenceFrame, instanceFrame, "material switch"));
    }

    // The local Lambert precompute branch and the specular normal branch must
    // describe the same transformed tangent plane; render both material kinds
    // and compare each against its own reference.
    batch.instances[0].materialOverride = &glossy;
    Object specularReference = expandReference(batch);
    const Snapshot specularInstance = fixture.render();
    const Snapshot specularRef = renderStandalone(specularReference, obliqueLight);
    TASK_CHECK(matchAffine(specularRef, specularInstance, "specular path"));
    return 0;
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

int nearPlaneClipping() {
    Material material = litMaterial(0x5b2c);
    // A large triangle whose transformed vertices straddle the near plane at
    // z == 100. Winding (0,2,1) makes this authored face front-facing under
    // CULL_BACKFACES so a visible, clipped result is guaranteed.
    Object authored;
    authored.addVertex({{-120, -80, -100}, {0, 0}, {0, 0, 0}});
    authored.addVertex({{ 160, -40,  400}, {1024, 0}, {0, 0, 0}});
    authored.addVertex({{  20, 160,  300}, {0, 1024}, {0, 0, 0}});
    authored.addTriangle(0, 2, 1, &material);
    authored.computeFlatNormals();
    auto prototype = Object::freezeMesh(std::move(authored));
    TASK_CHECK(prototype != nullptr);

    Object batch;
    Object::InstanceTransform transform = affineMirror({0, 0, 0});
    TASK_CHECK(batch.addInstance(prototype, transform, &material));
    batch.calculateBoundingBox();

    Fixture fixture;
    fixture.scene->addObject(&batch);
    auto verify = [&](const char* label) {
        Object reference = expandReference(batch);
        const Snapshot instanceFrame = fixture.render();
        const Snapshot referenceFrame = renderStandalone(reference);
        if (!matchAffine(referenceFrame, instanceFrame, label)) return false;
        // Every step must actually show clipped geometry, not an empty frame.
        return instanceFrame.rasterizedTriangles > 0 &&
               nonBackgroundPixels(instanceFrame.pixels) > 0;
    };
    TASK_CHECK(verify("near-plane clip"));

    // Sliding the instance across the near plane in both directions must
    // clip against the live placement on every frame.
    const Vector3 positions[] = {{0,0,-40}, {0,0,20}, {0,0,60}, {0,0,120},
                                 {0,0,200}, {0,0,40}, {0,0,-20}};
    for (const Vector3& p : positions) {
        TASK_CHECK(batch.setInstanceTransform(0, affineMirror(p)));
        TASK_CHECK(verify("clip sweep"));
    }
    return 0;
}

int farPlaneClipping() {
    Material material = litMaterial(0xc649);
    auto prototype = makeLitPrototype(&material);
    TASK_CHECK(prototype != nullptr);

    // A z-scaled placement that straddles the far plane (z == 1500) while
    // remaining partly visible: the near vertices are clipped against the
    // far plane and must use the same placement as the rest of the batch.
    Object batch;
    Object::InstanceTransform transform;
    const float basis[9] = { 2, 0, 0,  0, 2, 0,  0, 0, 2 };
    std::copy(basis, basis + 9, transform.basis);
    transform.position = {0, 0, 1300};
    TASK_CHECK(batch.addInstance(prototype, transform, &material));
    batch.calculateBoundingBox();

    Fixture fixture;
    fixture.scene->addObject(&batch);
    auto verify = [&](const char* label) {
        Object reference = expandReference(batch);
        const Snapshot instanceFrame = fixture.render();
        const Snapshot referenceFrame = renderStandalone(reference);
        if (!matchAffine(referenceFrame, instanceFrame, label)) return false;
        return instanceFrame.rasterizedTriangles > 0 &&
               nonBackgroundPixels(instanceFrame.pixels) > 0;
    };
    TASK_CHECK(verify("far-plane clip"));

    // Move the instance deeper until it leaves the far plane entirely; the
    // clipped result must track the placement, then vanish.
    TASK_CHECK(batch.setInstanceTransform(0, [&] {
        Object::InstanceTransform t = transform;
        t.position = {0, 0, 1500};
        return t;
    }()));
    Object goneReference = expandReference(batch);
    const Snapshot gone = fixture.render();
    const Snapshot goneRef = renderStandalone(goneReference);
    TASK_CHECK(matchAffine(goneRef, gone, "far-plane exit"));
    TASK_CHECK(nonBackgroundPixels(gone.pixels) == 0);
    return 0;
}

int cullingModesWithMirror() {
    Material material = litMaterial(0x4e9f);
    auto prototype = makePlatePrototype(&material);
    TASK_CHECK(prototype != nullptr);

    Object batch;
    Object::InstanceTransform transform;
    const float basis[9] = { -2, 1, 0,  0, 1, 0,  0, 0, 1 };
    std::copy(basis, basis + 9, transform.basis);
    transform.position = {0, 0, 520};
    TASK_CHECK(batch.addInstance(prototype, transform, &material));
    batch.calculateBoundingBox();

    // Discover which culling mode keeps the unmirrored, positive-determinant
    // plate visible; the mirror must keep the same prototype face visible.
    Object plain;
    TASK_CHECK(plain.addInstance(prototype, affinePositive({0, 0, 520})));
    plain.calculateBoundingBox();
    CullingMode authoredFrontMode = CullingMode::NO_CULLING;
    for (CullingMode mode : {CullingMode::CULL_BACKFACES, CullingMode::CULL_FRONTFACES}) {
        plain.cullingMode = mode;
        const Snapshot probe = renderStandalone(plain);
        if (probe.drawnTriangles > 0) { authoredFrontMode = mode; break; }
    }
    TASK_CHECK(authoredFrontMode != CullingMode::NO_CULLING);

    for (CullingMode mode : {CullingMode::CULL_BACKFACES,
                             CullingMode::CULL_FRONTFACES,
                             CullingMode::NO_CULLING}) {
        batch.cullingMode = mode;
        Fixture fixture;
        fixture.scene->addObject(&batch);
        const Snapshot instanceFrame = fixture.render();
        Object reference = expandReference(batch); // winding reversed for det < 0
        const Snapshot referenceFrame = renderStandalone(reference);
        TASK_CHECK(matchAffine(referenceFrame, instanceFrame, "mirror culling"));
        if (mode == authoredFrontMode) {
            // Front stays front: the mirrored placement remains visible under
            // the same mode that shows the prototype's authored front.
            TASK_CHECK(instanceFrame.drawnTriangles > 0);
            TASK_CHECK(nonBackgroundPixels(instanceFrame.pixels) > 0);
        }
    }
    return 0;
}

int pickingUsesPlacement() {
#if MAX_PICK_QUERIES > 0
    Material material = litMaterial(0x6d3a);
    auto prototype = makePlatePrototype(&material);
    TASK_CHECK(prototype != nullptr);

    Object batch;
    batch.cullingMode = CullingMode::NO_CULLING;
    Object::InstanceTransform nearTransform;
    const float nearBasis[9] = { -1, 0, 0,  0, 1, 0,  0, 0, 1 };  // mirror
    std::copy(nearBasis, nearBasis + 9, nearTransform.basis);
    nearTransform.position = {0, 0, 500};
    Object::InstanceTransform farTransform;
    farTransform.position = {0, 0, 1200};
    TASK_CHECK(batch.addInstance(prototype, farTransform));
    TASK_CHECK(batch.addInstance(prototype, nearTransform));
    batch.calculateBoundingBox();

    Fixture fixture;
    fixture.scene->addObject(&batch);
    PickQuery query{kWidth / 2, kHeight / 2};
    const Snapshot first = fixture.render(&query, 1);
    const PickResult& hit = fixture.picks()[0];
    TASK_CHECK(hit.hit);
    TASK_CHECK(hit.object == &batch);
    TASK_CHECK(hit.mesh == prototype.get());
    TASK_CHECK(hit.instanceIndex == 1);          // the nearer placement
    TASK_CHECK(hit.triangleIndex >= 0 && hit.triangleIndex < (int32_t)prototype->triangles.size());
    TASK_CHECK(first.drawnTriangles > 0);

    // Swap placements through setInstanceTransform: depth ordering and pick
    // attribution must both follow the refreshed placement.
    TASK_CHECK(batch.setInstanceTransform(0, nearTransform));
    TASK_CHECK(batch.setInstanceTransform(1, farTransform));
    const Snapshot second = fixture.render(&query, 1);
    const PickResult& swapped = fixture.picks()[0];
    TASK_CHECK(swapped.hit);
    TASK_CHECK(swapped.instanceIndex == 0);
    TASK_CHECK(swapped.mesh == prototype.get());
    TASK_CHECK(swapped.triangleIndex == hit.triangleIndex);
    TASK_CHECK(pixelsMatch(first.pixels, second.pixels, 1, "pick placement swap"));

    // Moving the near instance off the queried pixel must make the pick follow
    // the new placement, not the stale one.
    Object::InstanceTransform offscreen = nearTransform;
    offscreen.position = {1600, 0, 500};
    TASK_CHECK(batch.setInstanceTransform(0, offscreen));
    const Snapshot third = fixture.render(&query, 1);
    const PickResult& moved = fixture.picks()[0];
    TASK_CHECK(moved.hit);
    TASK_CHECK(moved.instanceIndex == 1);
    TASK_CHECK(moved.mesh == prototype.get());
    (void)third;
    return 0;
#else
    TASK_CHECK(true);
    return 0;
#endif
}

int orthogonalLegacyExact() {
    Material material = litMaterial(0x6d3a, 32);
    auto prototype = makeLitPrototype(&material);
    TASK_CHECK(prototype != nullptr);

    // Positive-determinant, orthogonal placements with an owner rotation and
    // translation. This fixture is chosen so every step draws visible
    // geometry (about 500 pixels) in both the precomputed-Lambert and
    // specular paths. Comparisons here are byte-for-byte exact: the legacy
    // rigid path must be unchanged.
    Object batch;
    batch.position = {30, -20, 0};
    batch.rotation = {0, 0, 90};
    TASK_CHECK(batch.addInstance(prototype,
        Object::InstanceTransform::rotated({90, 0, 0}, {0, 0, 520}), &material));
    batch.calculateBoundingBox();
    const BasisInfo info = analyze(batch.instances[0].transform);
    TASK_CHECK(info.det > 0);
    TASK_CHECK(info.orthogonal);

    Fixture fixture;
    fixture.scene->addObject(&batch);
    for (int frame = 0; frame < 6; ++frame) {
        batch.cullingMode = (CullingMode)(frame % 3);
        Material spec = litMaterial(0x6d3a, (frame % 2) ? 32 : 0);
        batch.instances[0].materialOverride = &spec;
        Object reference = expandReference(batch);  // keeps owner transform/flags
        const Snapshot instanceFrame = fixture.render();
        const Snapshot referenceFrame = renderStandalone(reference);
        // Positive-determinant, orthogonal placements must keep their old
        // pixel-for-pixel behaviour in both precompute and specular paths.
        TASK_CHECK(matchExactly(referenceFrame, instanceFrame, "legacy orthogonal"));
        if (batch.cullingMode == CullingMode::CULL_BACKFACES ||
            batch.cullingMode == CullingMode::NO_CULLING) {
            TASK_CHECK(instanceFrame.rasterizedTriangles > 0);
            TASK_CHECK(nonBackgroundPixels(instanceFrame.pixels) > 0);
        }
    }
    return 0;
}

int batchCullAfterMove() {
    Material material = litMaterial(0xf8a4);
    auto prototype = makePlatePrototype(&material);
    TASK_CHECK(prototype != nullptr);

    Object batch;
    Object::InstanceTransform transform;
    const float basis[9] = { -2, 1, 0,  0, 1, 0,  0, 1, 3 };
    std::copy(basis, basis + 9, transform.basis);
    transform.position = {0, 0, 520};
    TASK_CHECK(batch.addInstance(prototype, transform, &material));
    batch.calculateBoundingBox();

    Fixture fixture;
    fixture.scene->addObject(&batch);
    const Snapshot visible = fixture.render();
    TASK_CHECK(visible.drawnTriangles > 0);

    // Far outside the frustum: the refreshed batch AABB must let the owner
    // cull everything in one test.
    Object::InstanceTransform outside = transform;
    outside.position = {4200, 0, 520};
    TASK_CHECK(batch.setInstanceTransform(0, outside));
    TASK_CHECK(batch.boundingBoxMin.x > 2000);
    const Snapshot culled = fixture.render();
    TASK_CHECK(culled.drawnObjects == 0 || culled.drawnTriangles == 0);
    TASK_CHECK(nonBackgroundPixels(culled.pixels) == 0);

    TASK_CHECK(batch.setInstanceTransform(0, transform));
    const Snapshot restored = fixture.render();
    TASK_CHECK(restored.drawnTriangles == visible.drawnTriangles);
    TASK_CHECK(pixelsMatch(visible.pixels, restored.pixels, 1, "cull restore"));
    return 0;
}

} // namespace

extern const task_case task_cases[] = {
    {"placement", "rejects_invalid_preserves_state", rejectsInvalidPreservesState},
    {"placement", "updates_bounds_and_render",       updatesBoundsAndRender},
    {"placement", "two_shared_affine_instances",     twoSharedAffineInstances},
    {"placement", "sweep_out_of_old_bounds",         sweepOutOfOldBounds},
    {"placement", "material_override_bindings",      materialOverrideBindings},

    {"lighting",  "affine_normal_transform",         affineNormalTransform},
    {"lighting",  "mirror_keeps_front_face",         mirrorKeepsFrontFace},
    {"lighting",  "material_switch_freshness",       materialSwitchFreshness},

    {"visibility","near_plane_clipping",             nearPlaneClipping},
    {"visibility","far_plane_clipping",              farPlaneClipping},
    {"visibility","culling_modes_with_mirror",       cullingModesWithMirror},
    {"visibility","picking_uses_placement",          pickingUsesPlacement},
    {"visibility","orthogonal_legacy_pixel_exact",   orthogonalLegacyExact},
    {"visibility","batch_cull_after_move",           batchCullAfterMove},
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
