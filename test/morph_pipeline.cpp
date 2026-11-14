//****************************************************************************//
// morph_pipeline.cpp                                                         //
// Incremental sparse morph evaluation: representation, skinning, cache and   //
// file roundtrip coverage.  Registered through task_support/check.h; the     //
// driver supplies main().                                                    //
//****************************************************************************//

#include "task_support/check.h"

#include "cal3d/cal3d.h"
#include "cal3d/physiquedualquaternion.h"
#include "cal3d/dualquaternion.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace cal3d;

namespace
{
    const float kEps = 1.0e-5f;

    bool nearFloat(float a, float b, float eps = kEps)
    {
        float d = a - b;
        if(d < 0.0f) d = -d;
        return d <= eps;
    }

    bool nearVec(const CalVector& a, const CalVector& b, float eps = kEps)
    {
        return nearFloat(a.x, b.x, eps) && nearFloat(a.y, b.y, eps)
            && nearFloat(a.z, b.z, eps);
    }

    bool nearBuf3(const float* p, const CalVector& v, float eps = kEps)
    {
        return nearFloat(p[0], v.x, eps) && nearFloat(p[1], v.y, eps)
            && nearFloat(p[2], v.z, eps);
    }

    // ------------------------------------------------------------------
    // Skeleton construction: a root bone and a child carrying a real
    // rotation (90 degrees about Z) plus a translation.
    // ------------------------------------------------------------------
    CalCoreSkeleton* buildTwoBoneSkeleton(CalCoreModel* core)
    {
        CalCoreSkeleton* skel = new CalCoreSkeleton;

        CalCoreBone* root = new CalCoreBone("root");
        root->setParentId(-1);
        root->setCoreSkeleton(skel);
        root->setTranslation(CalVector(0.0f, 0.0f, 0.0f));
        root->setRotation(CalQuaternion(0.0f, 0.0f, 0.0f, 1.0f));
        root->setTranslationBoneSpace(CalVector(0.0f, 0.0f, 0.0f));
        root->setRotationBoneSpace(CalQuaternion(0.0f, 0.0f, 0.0f, 1.0f));
        skel->addCoreBone(root);

        CalCoreBone* child = new CalCoreBone("child");
        child->setParentId(0);
        child->setCoreSkeleton(skel);
        child->setTranslation(CalVector(1.0f, 2.0f, 3.0f));
        // 90 degree rotation about Z: q = (0, 0, sin45, cos45)
        child->setRotation(CalQuaternion(0.0f, 0.0f, 0.70710678f, 0.70710678f));
        child->setTranslationBoneSpace(CalVector(1.0f, 2.0f, 3.0f));
        child->setRotationBoneSpace(CalQuaternion(0.0f, 0.0f, 0.0f, 1.0f));
        root->addChildId(1);
        skel->addCoreBone(child);

        skel->calculateState();
        core->setCoreSkeleton(skel);
        return skel;
    }

    struct TargetEdit
    {
        int        vertexId;
        CalVector  position;          // absolute target position
        CalVector  normal;            // absolute target normal
        bool       usePosition;
        bool       useNormal;
        float      u, v;              // absolute target UV
        bool       useUV;
    };

    struct TargetSpec
    {
        std::string             name;
        std::vector<TargetEdit> edits;   // empty == fully unchanged target
    };

    struct Scene
    {
        CalCoreModel*    core;
        CalCoreSkeleton* skel;
        CalCoreMesh*     mesh;
        CalCoreSubmesh*  coreSub;
        CalModel*        model;
        CalSubmesh*      sub;
        int              vertexCount;
        bool             withUV;
    };

    // Builds a scene with a large filler vertex set and a handful of morph
    // targets whose supports overlap or stay disjoint.  Every target is a
    // dense array of absolute blend vertices initialized from the base shape.
    Scene* buildScene(int vertexCount, bool withUV, bool withSpring,
                      const std::vector<TargetSpec>& targets, int lodCount = 0)
    {
        Scene* s = new Scene;
        s->vertexCount = vertexCount;
        s->withUV = withUV;
        s->core = new CalCoreModel("morph-scene");
        s->skel = buildTwoBoneSkeleton(s->core);

        s->mesh = new CalCoreMesh;
        s->coreSub = new CalCoreSubmesh;
        s->coreSub->reserve(vertexCount, withUV ? 1 : 0,
                            vertexCount > 2 ? 1 : 0, withSpring ? 1 : 0);
        s->coreSub->setLodCount(lodCount);

        for(int v = 0; v < vertexCount; ++v)
        {
            CalCoreSubmesh::Vertex vertex;
            // Non-zero base shape so zero-initialized data could never match.
            vertex.position = CalVector(2.0f + v, 1.0f + 0.5f * v, 0.5f * v);
            vertex.normal   = CalVector(0.0f, 1.0f, 0.0f);
            vertex.collapseId = (v < vertexCount - lodCount) ? v : vertexCount - lodCount - 1;
            vertex.faceCollapseCount = 0;

            CalCoreSubmesh::Influence inf;
            inf.boneId = (v % 2 == 0) ? 0 : 1;
            inf.weight = 1.0f;
            vertex.vectorInfluence.push_back(inf);
            s->coreSub->setVertex(v, vertex);

            if(withUV)
            {
                CalCoreSubmesh::TextureCoordinate tc;
                tc.u = 0.1f * v;
                tc.v = 0.2f * v;
                s->coreSub->setTextureCoordinate(v, 0, tc);
            }

            if(withSpring)
            {
                CalCoreSubmesh::PhysicalProperty pp;
                pp.weight = (v == 0) ? 1.0f : 0.0f;
                s->coreSub->setPhysicalProperty(v, pp);
            }
        }

        if(vertexCount > 2)
        {
            CalCoreSubmesh::Face face;
            face.vertexId[0] = 0;
            face.vertexId[1] = 1;
            face.vertexId[2] = 2;
            s->coreSub->setFace(0, face);
        }

        if(withSpring)
        {
            CalCoreSubmesh::Spring spring;
            spring.vertexId[0] = 0;
            spring.vertexId[1] = 1;
            spring.springCoefficient = 1.0f;
            spring.idleLength = 1.0f;
            s->coreSub->setSpring(0, spring);
        }

        for(size_t t = 0; t < targets.size(); ++t)
        {
            CalCoreSubMorphTarget* mt = new CalCoreSubMorphTarget;
            mt->reserve(vertexCount);
            mt->setName(targets[t].name);

            // Start every blend vertex from the base shape (omitted vertices
            // therefore inherit the base shape).
            for(int v = 0; v < vertexCount; ++v)
            {
                CalCoreSubMorphTarget::BlendVertex bv;
                bv.position = s->coreSub->getVectorVertex()[v].position;
                bv.normal   = s->coreSub->getVectorVertex()[v].normal;
                if(withUV)
                {
                    bv.textureCoords.push_back(
                        s->coreSub->getVectorVectorTextureCoordinate()[0][v]);
                }
                mt->setBlendVertex(v, bv);
            }

            for(size_t e = 0; e < targets[t].edits.size(); ++e)
            {
                const TargetEdit& edit = targets[t].edits[e];
                CalCoreSubMorphTarget::BlendVertex bv;
                bv.position = edit.usePosition ? edit.position
                             : s->coreSub->getVectorVertex()[edit.vertexId].position;
                bv.normal   = edit.useNormal ? edit.normal
                             : s->coreSub->getVectorVertex()[edit.vertexId].normal;
                if(withUV)
                {
                    CalCoreSubmesh::TextureCoordinate tc;
                    if(edit.useUV) { tc.u = edit.u; tc.v = edit.v; }
                    else { tc = s->coreSub->getVectorVectorTextureCoordinate()[0][edit.vertexId]; }
                    bv.textureCoords.push_back(tc);
                }
                mt->setBlendVertex(edit.vertexId, bv);
            }

            s->coreSub->addCoreSubMorphTarget(mt);
        }

        s->mesh->addCoreSubmesh(s->coreSub);
        s->core->addCoreMesh(s->mesh);

        s->model = new CalModel(s->core);
        if(!s->model->attachMesh(0)) return 0;
        s->model->getSkeleton()->calculateState();
        s->sub = s->model->getMesh(0)->getSubmesh(0);
        return s;
    }

    void destroyScene(Scene* s)
    {
        delete s->model;
        delete s->core;   // owns skeleton, mesh and submesh data
        delete s;
    }

    // Independent local morph formula computed straight from the core data:
    // local = base + sum(w * (absoluteTarget - base)).
    CalVector expectedLocalPosition(const Scene* s, int vertexId,
                                    const std::vector<float>& weights)
    {
        const CalVector& base = s->coreSub->getVectorVertex()[vertexId].position;
        CalVector result = base;
        const std::vector<CalCoreSubMorphTarget*>& targets =
            s->coreSub->getVectorCoreSubMorphTarget();
        for(size_t t = 0; t < targets.size() && t < weights.size(); ++t)
        {
            const CalVector& target =
                targets[t]->getBlendVertex(vertexId)->position;
            float w = weights[t];
            result.x += w * (target.x - base.x);
            result.y += w * (target.y - base.y);
            result.z += w * (target.z - base.z);
        }
        return result;
    }

    CalVector expectedLocalNormal(const Scene* s, int vertexId,
                                  const std::vector<float>& weights)
    {
        const CalVector& base = s->coreSub->getVectorVertex()[vertexId].normal;
        CalVector result = base;
        const std::vector<CalCoreSubMorphTarget*>& targets =
            s->coreSub->getVectorCoreSubMorphTarget();
        for(size_t t = 0; t < targets.size() && t < weights.size(); ++t)
        {
            const CalVector& target =
                targets[t]->getBlendVertex(vertexId)->normal;
            float w = weights[t];
            result.x += w * (target.x - base.x);
            result.y += w * (target.y - base.y);
            result.z += w * (target.z - base.z);
        }
        return result;
    }

    // Independent linear blend skinning using the live bone matrices.
    CalVector expectedLbsPoint(const Scene* s, int vertexId, const CalVector& local)
    {
        const CalCoreSubmesh::Vertex& vertex =
            s->coreSub->getVectorVertex()[vertexId];
        const std::vector<CalBone*>& bones = s->model->getSkeleton()->getVectorBone();
        CalVector out(0.0f, 0.0f, 0.0f);
        for(size_t i = 0; i < vertex.vectorInfluence.size(); ++i)
        {
            const CalCoreSubmesh::Influence& inf = vertex.vectorInfluence[i];
            CalVector v(local);
            v *= bones[inf.boneId]->getTransformMatrix();
            v += bones[inf.boneId]->getTranslationBoneSpace();
            out.x += inf.weight * v.x;
            out.y += inf.weight * v.y;
            out.z += inf.weight * v.z;
        }
        return out;
    }

    CalVector expectedLbsDirection(const Scene* s, int vertexId, const CalVector& local)
    {
        const CalCoreSubmesh::Vertex& vertex =
            s->coreSub->getVectorVertex()[vertexId];
        const std::vector<CalBone*>& bones = s->model->getSkeleton()->getVectorBone();
        CalVector out(0.0f, 0.0f, 0.0f);
        for(size_t i = 0; i < vertex.vectorInfluence.size(); ++i)
        {
            const CalCoreSubmesh::Influence& inf = vertex.vectorInfluence[i];
            CalVector v(local);
            v *= bones[inf.boneId]->getTransformMatrix();
            out.x += inf.weight * v.x;
            out.y += inf.weight * v.y;
            out.z += inf.weight * v.z;
        }
        return out;
    }

    std::vector<float> zeroWeights(const Scene* s)
    {
        return std::vector<float>(s->sub->getMorphTargetWeightCount(), 0.0f);
    }

    // Scratch-buffer helpers sized to the full active vertex set so callers
    // never overflow a fixed buffer when exercising large filler scenes.
    void evaluateVertices(const CalPhysique& physique, CalSubmesh* sub)
    {
        std::vector<float> buffer(sub->getVertexCount() * 3);
        physique.calculateVertices(sub, &buffer[0], 0);
    }

    void evaluateNormals(const CalPhysique& physique, CalSubmesh* sub)
    {
        std::vector<float> buffer(sub->getVertexCount() * 3);
        physique.calculateNormals(sub, &buffer[0], 0);
    }

    void evaluateVerticesAndNormals(const CalPhysique& physique, CalSubmesh* sub)
    {
        std::vector<float> buffer(sub->getVertexCount() * 6);
        physique.calculateVerticesAndNormals(sub, &buffer[0], 0);
    }

    std::string tempPath(const char* name)
    {
        const char* dir = std::getenv("TMPDIR");
        std::string base = dir ? dir : ".";
        return base + "/" + name;
    }

    // The canonical scenario from the task description.  Vertex 0 already has
    // the exact base position (2,1,0) from the generic builder.
    Scene* buildCanonicalScene()
    {
        TargetSpec target;
        target.name = "smile";
        TargetEdit edit;
        edit.vertexId = 0;
        edit.usePosition = true;
        edit.position = CalVector(4.0f, 1.0f, 0.0f);
        edit.useNormal = true;
        edit.normal = CalVector(1.0f, 0.0f, 0.0f);
        edit.useUV = false;
        edit.u = edit.v = 0.0f;
        target.edits.push_back(edit);

        std::vector<TargetSpec> targets;
        targets.push_back(target);
        return buildScene(4, true, false, targets);
    }
}

// ==================================================================
// representation group
// ==================================================================

static int representation_canonical_all_entries_agree()
{
    Scene* s = buildCanonicalScene();
    s->sub->setMorphTargetWeight(0, 0.5f);

    CalPhysique lbs(s->model);
    CalPhysiqueDualQuat dq(s->model);
    lbs.setNormalization(false);
    dq.setNormalization(false);

    const int vertexCount = s->sub->getVertexCount();
    std::vector<float> pos(vertexCount * 3);
    std::vector<float> nrm(vertexCount * 3);
    std::vector<float> combined(vertexCount * 6);
    std::vector<float> dpos(vertexCount * 3);
    std::vector<float> tex(vertexCount * 8, 0.0f);

    TASK_CHECK(lbs.calculateVertices(s->sub, &pos[0], 0) == vertexCount);
    TASK_CHECK(lbs.calculateNormals(s->sub, &nrm[0], 0) == vertexCount);
    TASK_CHECK(lbs.calculateVerticesAndNormals(s->sub, &combined[0], 0) == vertexCount);
    TASK_CHECK(dq.calculateVertices(s->sub, &dpos[0], 0) == vertexCount);
    TASK_CHECK(dq.calculateVerticesNormalsAndTexCoords(s->sub, &tex[0], 1) == vertexCount);

    // base (2,1,0), absolute target (4,1,0), w=0.5 -> local (3,1,0).
    CalVector expectedLocal(3.0f, 1.0f, 0.0f);
    TASK_CHECK(nearVec(s->sub->getVectorMorphedVertex()[0], expectedLocal));

    // All skinning entries must produce the same position for a single
    // influence vertex under identity-ish bone 0.
    CalVector expectedSkinned = expectedLbsPoint(s, 0, expectedLocal);
    TASK_CHECK(nearBuf3(&pos[0], expectedSkinned));
    TASK_CHECK(nearBuf3(&combined[0], expectedSkinned));
    TASK_CHECK(nearBuf3(&dpos[0], expectedSkinned));
    TASK_CHECK(nearFloat(tex[0], expectedSkinned.x));
    TASK_CHECK(nearFloat(tex[1], expectedSkinned.y));
    TASK_CHECK(nearFloat(tex[2], expectedSkinned.z));

    // Local normal: base (0,1,0), absolute target (1,0,0), w=0.5
    // -> (0,1,0) + 0.5*((1,0,0)-(0,1,0)) = (0.5,0.5,0).
    CalVector expectedLocalN(0.5f, 0.5f, 0.0f);
    TASK_CHECK(nearVec(s->sub->getVectorMorphedNormal()[0], expectedLocalN));
    CalVector expectedSkinnedN = expectedLbsDirection(s, 0, expectedLocalN);
    TASK_CHECK(nearBuf3(&nrm[0], expectedSkinnedN));
    TASK_CHECK(nearFloat(combined[3], expectedSkinnedN.x));
    TASK_CHECK(nearFloat(combined[4], expectedSkinnedN.y));
    TASK_CHECK(nearFloat(combined[5], expectedSkinnedN.z));

    // Single-vertex entry point uses the same local cache.
    CalVector single = lbs.calculateVertex(s->sub, 0);
    TASK_CHECK(nearVec(single, expectedSkinned));

    destroyScene(s);
    return 0;
}

static int representation_negative_and_overlapping_weights()
{
    // Target A touches vertices 0 and 1; target B overlaps at 1 and adds 2.
    std::vector<TargetSpec> specs(2);
    specs[0].name = "A";
    specs[1].name = "B";
    TargetEdit a0 = { 0, CalVector(4.0f, 1.0f, 0.0f), CalVector(0, 1, 0), true, false, 0, 0, false };
    TargetEdit a1 = { 1, CalVector(9.0f, 1.5f, 0.5f), CalVector(0, 1, 0), true, false, 0, 0, false };
    TargetEdit b1 = { 1, CalVector(6.0f, 3.0f, 0.5f), CalVector(0, 1, 0), true, false, 0, 0, false };
    TargetEdit b2 = { 2, CalVector(8.0f, 2.0f, 1.0f), CalVector(0, 1, 0), true, false, 0, 0, false };
    specs[0].edits.push_back(a0);
    specs[0].edits.push_back(a1);
    specs[1].edits.push_back(b1);
    specs[1].edits.push_back(b2);

    Scene* s = buildScene(32, false, false, specs);
    s->sub->setMorphTargetWeight(0, 0.5f);   // positive
    s->sub->setMorphTargetWeight(1, -0.25f); // negative weight must be supported

    CalPhysique lbs(s->model);
    lbs.setNormalization(false);
    evaluateVertices(lbs, s->sub);

    std::vector<float> weights;
    weights.push_back(0.5f);
    weights.push_back(-0.25f);
    for(int v = 0; v < 32; ++v)
    {
        CalVector expected = expectedLocalPosition(s, v, weights);
        TASK_CHECK(nearVec(s->sub->getVectorMorphedVertex()[v], expected));
    }

    destroyScene(s);
    return 0;
}

static int representation_normalization_flag()
{
    std::vector<TargetSpec> specs(1);
    specs[0].name = "N";
    TargetEdit edit = { 0, CalVector(2, 1, 0), CalVector(3.0f, 0.0f, 4.0f), false, true, 0, 0, false };
    specs[0].edits.push_back(edit);

    Scene* s = buildScene(8, false, false, specs);
    s->sub->setMorphTargetWeight(0, 1.0f);
    CalPhysique lbs(s->model);

    std::vector<float> nrmOff(s->sub->getVertexCount() * 3);
    std::vector<float> nrmOn(s->sub->getVertexCount() * 3);
    lbs.setNormalization(false);
    TASK_CHECK(lbs.calculateNormals(s->sub, &nrmOff[0], 0) == s->sub->getVertexCount());
    CalVector localN = s->sub->getVectorMorphedNormal()[0];
    // Independently recomputed local normal at full weight equals the target.
    std::vector<float> fullWeight(1, 1.0f);
    TASK_CHECK(nearVec(localN, expectedLocalNormal(s, 0, fullWeight)));
    CalVector raw = expectedLbsDirection(s, 0, localN);
    // Off: the transformed normal length is preserved exactly.
    TASK_CHECK(nearBuf3(&nrmOff[0], raw));
    TASK_CHECK(nearFloat(std::sqrt(nrmOff[0]*nrmOff[0] + nrmOff[1]*nrmOff[1]
                                   + nrmOff[2]*nrmOff[2]), raw.length()));

    lbs.setNormalization(true);
    TASK_CHECK(lbs.calculateNormals(s->sub, &nrmOn[0], 0) == s->sub->getVertexCount());
    float len = std::sqrt(nrmOn[0]*nrmOn[0] + nrmOn[1]*nrmOn[1] + nrmOn[2]*nrmOn[2]);
    TASK_CHECK(nearFloat(len, 1.0f));
    destroyScene(s);

    // A zero blended normal stays zero instead of becoming NaN/Inf.
    std::vector<TargetSpec> zeroSpecs(1);
    zeroSpecs[0].name = "zero";
    TargetEdit zeroEdit = { 0, CalVector(2, 1, 0), CalVector(0.0f, 0.0f, 0.0f),
                            false, true, 0, 0, false };
    zeroSpecs[0].edits.push_back(zeroEdit);
    Scene* z = buildScene(4, false, false, zeroSpecs);
    z->sub->setMorphTargetWeight(0, 1.0f);
    CalPhysique zlbs(z->model);
    zlbs.setNormalization(true);
    std::vector<float> zeroNrm(z->sub->getVertexCount() * 3, 0.0f);
    TASK_CHECK(zlbs.calculateNormals(z->sub, &zeroNrm[0], 0) == z->sub->getVertexCount());
    TASK_CHECK(nearFloat(zeroNrm[0], 0.0f));
    TASK_CHECK(nearFloat(zeroNrm[1], 0.0f));
    TASK_CHECK(nearFloat(zeroNrm[2], 0.0f));
    destroyScene(z);
    return 0;
}

static int representation_output_uv_uses_base_mesh()
{
    std::vector<TargetSpec> specs(1);
    specs[0].name = "UV";
    TargetEdit edit = { 0, CalVector(2, 1, 0), CalVector(0, 1, 0),
                        false, false, 0.75f, 0.9f, true };
    specs[0].edits.push_back(edit);

    Scene* s = buildScene(6, true, false, specs);
    s->sub->setMorphTargetWeight(0, 1.0f);

    CalPhysique lbs(s->model);
    float buffer[64];
    TASK_CHECK(lbs.calculateVerticesNormalsAndTexCoords(s->sub, buffer, 1));

    // Output UV follows the base mesh even though the target carries other UVs.
    const CalCoreSubmesh::TextureCoordinate& baseUV =
        s->coreSub->getVectorVectorTextureCoordinate()[0][0];
    TASK_CHECK(nearFloat(buffer[6], baseUV.u));
    TASK_CHECK(nearFloat(buffer[7], baseUV.v));

    // The target UV itself is retained intact on the core data.
    const CalCoreSubMorphTarget::BlendVertex* bv =
        s->coreSub->getVectorCoreSubMorphTarget()[0]->getBlendVertex(0);
    TASK_CHECK(bv->textureCoords.size() == 1);
    TASK_CHECK(nearFloat(bv->textureCoords[0].u, 0.75f));
    TASK_CHECK(nearFloat(bv->textureCoords[0].v, 0.9f));

    destroyScene(s);
    return 0;
}

static int representation_dq_is_not_replaced_by_lbs()
{
    // Hand-built vertex with two influences so LBS and DQ genuinely differ.
    CalCoreModel core("dq");
    buildTwoBoneSkeleton(&core);
    CalCoreMesh* mesh = new CalCoreMesh;
    CalCoreSubmesh* sub = new CalCoreSubmesh;
    sub->reserve(1, 0, 0, 0);
    CalCoreSubmesh::Vertex v;
    v.position = CalVector(2.0f, 1.0f, 0.0f);
    v.normal = CalVector(0.0f, 1.0f, 0.0f);
    v.collapseId = 0;
    v.faceCollapseCount = 0;
    CalCoreSubmesh::Influence i0; i0.boneId = 0; i0.weight = 0.5f;
    CalCoreSubmesh::Influence i1; i1.boneId = 1; i1.weight = 0.5f;
    v.vectorInfluence.push_back(i0);
    v.vectorInfluence.push_back(i1);
    sub->setVertex(0, v);
    mesh->addCoreSubmesh(sub);
    core.addCoreMesh(mesh);

    CalModel model(&core);
    TASK_CHECK(model.attachMesh(0));
    model.getSkeleton()->calculateState();
    CalSubmesh* sm = model.getMesh(0)->getSubmesh(0);

    CalPhysique lbs(&model);
    CalPhysiqueDualQuat dq(&model);
    lbs.setNormalization(false);
    dq.setNormalization(false);

    float lpos[3], dpos[3], lcombined[6], dcombined[6];
    TASK_CHECK(lbs.calculateVertices(sm, lpos, 0));
    TASK_CHECK(dq.calculateVertices(sm, dpos, 0));
    TASK_CHECK(lbs.calculateVerticesAndNormals(sm, lcombined, 0));
    TASK_CHECK(dq.calculateVerticesAndNormals(sm, dcombined, 0));

    // Independent LBS value derived directly from bone matrices.
    const std::vector<CalBone*>& bones = model.getSkeleton()->getVectorBone();
    CalVector lbsPoint(0, 0, 0);
    for(int b = 0; b < 2; ++b)
    {
        CalVector tv(v.position);
        tv *= bones[b]->getTransformMatrix();
        tv += bones[b]->getTranslationBoneSpace();
        lbsPoint.x += 0.5f * tv.x;
        lbsPoint.y += 0.5f * tv.y;
        lbsPoint.z += 0.5f * tv.z;
    }
    TASK_CHECK(nearBuf3(lpos, lbsPoint));
    TASK_CHECK(nearBuf3(lcombined, lbsPoint));

    // DQ value computed through the dual quaternion helper independently.
    CalDualQuaternion blended;
    blended.nondual = CalQuaternion(0, 0, 0, 0);
    blended.dual = CalQuaternion(0, 0, 0, 0);
    CalQuaternion pivot;
    for(int b = 0; b < 2; ++b)
    {
        CalDualQuaternion bt(bones[b]->getRotationBoneSpace(),
                             bones[b]->getTranslationBoneSpace());
        if(b == 0) pivot = bt.nondual;
        else if(dot(bt.nondual, pivot) < 0.0f) bt *= -1.0f;
        bt *= 0.5f;
        blended += bt;
    }
    blended.normalize();
    CalVector dqExpected;
    blended.transformPoint(v.position, dqExpected);
    TASK_CHECK(nearBuf3(dpos, dqExpected));
    TASK_CHECK(nearBuf3(dcombined, dqExpected));

    // The two blending methods must not collapse onto the same number.
    TASK_CHECK(!nearBuf3(dpos, lbsPoint, 1.0e-4f));
    return 0;
}

// ==================================================================
// skinning group
// ==================================================================

static int skinning_rotated_translated_skeleton()
{
    Scene* s = buildScene(12, false, false, std::vector<TargetSpec>());
    CalPhysique lbs(s->model);
    CalPhysiqueDualQuat dq(s->model);
    lbs.setNormalization(false);
    dq.setNormalization(false);

    const int vertexCount = s->sub->getVertexCount();
    std::vector<float> lbuf(vertexCount * 3);
    std::vector<float> dbuf(vertexCount * 3);
    std::vector<float> cbuf(vertexCount * 6);
    TASK_CHECK(lbs.calculateVertices(s->sub, &lbuf[0], 0) == vertexCount);
    TASK_CHECK(dq.calculateVertices(s->sub, &dbuf[0], 0) == vertexCount);
    TASK_CHECK(lbs.calculateVerticesAndNormals(s->sub, &cbuf[0], 0) == vertexCount);

    for(int v = 0; v < vertexCount; ++v)
    {
        const CalVector& local = s->coreSub->getVectorVertex()[v].position;
        CalVector expected = expectedLbsPoint(s, v, local);
        // Single-influence vertices: LBS and DQ reduce to the same rigid
        // transform, and the interleaved entry agrees with the batch entry.
        TASK_CHECK(nearVec(CalVector(lbuf[3*v], lbuf[3*v+1], lbuf[3*v+2]),
                           expected));
        TASK_CHECK(nearVec(CalVector(dbuf[3*v], dbuf[3*v+1], dbuf[3*v+2]),
                           expected));
        TASK_CHECK(nearVec(CalVector(cbuf[6*v], cbuf[6*v+1], cbuf[6*v+2]),
                           expected));
    }

    destroyScene(s);
    return 0;
}

static int skinning_axis_factors_apply_after_morph()
{
    Scene* s = buildCanonicalScene();
    s->sub->setMorphTargetWeight(0, 0.5f);
    CalPhysique lbs(s->model);
    lbs.setNormalization(false);
    lbs.setAxisFactorX(2.0f);
    lbs.setAxisFactorY(3.0f);
    lbs.setAxisFactorZ(4.0f);

    std::vector<float> pos(s->sub->getVertexCount() * 3);
    TASK_CHECK(lbs.calculateVertices(s->sub, &pos[0], 0) == s->sub->getVertexCount());
    CalVector local(3.0f, 1.0f, 0.0f);
    CalVector base = expectedLbsPoint(s, 0, local);
    TASK_CHECK(nearFloat(pos[0], base.x * 2.0f));
    TASK_CHECK(nearFloat(pos[1], base.y * 3.0f));
    TASK_CHECK(nearFloat(pos[2], base.z * 4.0f));

    // The local cache stays in unscaled, pre-skinning coordinates.
    TASK_CHECK(nearVec(s->sub->getVectorMorphedVertex()[0], local));

    destroyScene(s);
    return 0;
}

static int skinning_byte_stride_padding_preserved()
{
    Scene* s = buildCanonicalScene();
    s->sub->setMorphTargetWeight(0, 0.5f);
    CalPhysique lbs(s->model);
    lbs.setNormalization(false);

    const int vertexCount = s->sub->getVertexCount();
    const int strideFloats = 4; // one float of padding per vertex
    std::vector<float> buffer(vertexCount * strideFloats, -1234.5f);
    TASK_CHECK(lbs.calculateVertices(s->sub, &buffer[0],
                                     strideFloats * (int)sizeof(float))
                == vertexCount);

    CalVector local(3.0f, 1.0f, 0.0f);
    CalVector expected = expectedLbsPoint(s, 0, local);
    TASK_CHECK(nearFloat(buffer[0], expected.x));
    TASK_CHECK(nearFloat(buffer[3], -1234.5f)); // padding untouched
    TASK_CHECK(nearFloat(buffer[strideFloats + 3], -1234.5f));

    destroyScene(s);
    return 0;
}

static int skinning_springs_internal_write_rules()
{
    Scene* s = buildScene(6, false, true, std::vector<TargetSpec>());
    TASK_CHECK(s->sub->hasInternalData());
    CalPhysique lbs(s->model);
    lbs.setNormalization(false);

    std::vector<float> buffer(6 * 3, 777.0f);
    TASK_CHECK(lbs.calculateVertices(s->sub, &buffer[0], 0) == 6);

    // Vertex 0 has a non-zero physical weight: its slot must be left alone.
    TASK_CHECK(nearFloat(buffer[0], 777.0f));
    // Other vertices are free physical particles and still get written.
    CalVector expected1 = expectedLbsPoint(s, 1,
        s->coreSub->getVectorVertex()[1].position);
    TASK_CHECK(nearFloat(buffer[3], expected1.x));

    destroyScene(s);
    return 0;
}

static int skinning_lod_changes_count_without_morph_rebuild()
{
    std::vector<TargetSpec> specs(1);
    specs[0].name = "A";
    TargetEdit edit = { 0, CalVector(9.0f, 1.0f, 0.0f), CalVector(0, 1, 0),
                        true, false, 0, 0, false };
    specs[0].edits.push_back(edit);

    Scene* s = buildScene(8, false, false, specs, 2);
    s->sub->setMorphTargetWeight(0, 0.5f);
    CalPhysique lbs(s->model);
    lbs.setNormalization(false);
    float pos[32];
    TASK_CHECK(lbs.calculateVertices(s->sub, pos, 0) == 8);
    MorphWorkStats first = s->sub->getMorphWorkStats();
    TASK_CHECK(first.rebuiltVertices == 1);

    // Lower the LOD level: output vertex count changes but the local morph
    // cache must not be rebuilt because no weight changed.
    s->sub->setLodLevel(0.5f);
    TASK_CHECK(s->sub->getVertexCount() == 7);
    TASK_CHECK(lbs.calculateVertices(s->sub, pos, 0) == 7);
    MorphWorkStats atLod = s->sub->getMorphWorkStats();
    TASK_CHECK(atLod.rebuiltVertices == 0);
    TASK_CHECK(atLod.deltaVisits == 0);

    s->sub->setLodLevel(1.0f);
    TASK_CHECK(s->sub->getVertexCount() == 8);
    TASK_CHECK(lbs.calculateVertices(s->sub, pos, 0) == 8);
    TASK_CHECK(s->sub->getMorphWorkStats().rebuiltVertices == 0);

    destroyScene(s);
    return 0;
}

// ==================================================================
// cache group
// ==================================================================

namespace
{
    // Large scene: 64 vertices, targets A {0,1}, B {1,2}, C {3}, an empty
    // target, plus several unrelated empty targets.
    Scene* buildSparseWorkScene()
    {
        std::vector<TargetSpec> specs(6);
        specs[0].name = "A";
        specs[1].name = "B";
        specs[2].name = "C";
        specs[3].name = "empty1";
        specs[4].name = "empty2";
        specs[5].name = "empty3";
        specs[0].edits.push_back(TargetEdit{ 0, CalVector(9, 1, 0), CalVector(0, 1, 0), true, false, 0, 0, false });
        specs[0].edits.push_back(TargetEdit{ 1, CalVector(9, 2, 0), CalVector(0, 1, 0), true, false, 0, 0, false });
        specs[1].edits.push_back(TargetEdit{ 1, CalVector(3, 9, 0), CalVector(0, 1, 0), true, false, 0, 0, false });
        specs[1].edits.push_back(TargetEdit{ 2, CalVector(4, 9, 1), CalVector(0, 1, 0), true, false, 0, 0, false });
        specs[2].edits.push_back(TargetEdit{ 3, CalVector(5, 2, 9), CalVector(0, 1, 0), true, false, 0, 0, false });
        return buildScene(64, false, false, specs);
    }
}

static int cache_initial_evaluation_is_free()
{
    Scene* s = buildSparseWorkScene();
    CalPhysique lbs(s->model);
    evaluateVertices(lbs, s->sub);
    MorphWorkStats stats = s->sub->getMorphWorkStats();
    TASK_CHECK(stats.rebuiltVertices == 0);
    TASK_CHECK(stats.deltaVisits == 0);

    // All local results start at the base shape.
    for(int v = 0; v < 64; ++v)
    {
        TASK_CHECK(nearVec(s->sub->getVectorMorphedVertex()[v],
                           s->coreSub->getVectorVertex()[v].position));
    }
    destroyScene(s);
    return 0;
}

static int cache_first_change_matches_support_union()
{
    Scene* s = buildSparseWorkScene();
    CalPhysique lbs(s->model);
    lbs.setNormalization(false);
    evaluateVertices(lbs, s->sub); // warm up, zero work

    s->sub->setMorphTargetWeight(0, 0.5f); // A touches {0,1}
    evaluateVertices(lbs, s->sub);
    MorphWorkStats stats = s->sub->getMorphWorkStats();
    TASK_CHECK(stats.rebuiltVertices == 2);
    TASK_CHECK(stats.deltaVisits == 2); // one nonzero delta per dirty vertex

    // Only the support vertices changed; filler vertices stay on the base.
    TASK_CHECK(nearVec(s->sub->getVectorMorphedVertex()[7],
                       s->coreSub->getVectorVertex()[7].position));

    std::vector<float> w = zeroWeights(s);
    w[0] = 0.5f;
    TASK_CHECK(nearVec(s->sub->getVectorMorphedVertex()[0],
                       expectedLocalPosition(s, 0, w)));
    TASK_CHECK(nearVec(s->sub->getVectorMorphedVertex()[1],
                       expectedLocalPosition(s, 1, w)));
    destroyScene(s);
    return 0;
}

static int cache_overlapping_targets_incremental()
{
    Scene* s = buildSparseWorkScene();
    CalPhysique lbs(s->model);
    lbs.setNormalization(false);
    evaluateVertices(lbs, s->sub);

    s->sub->setMorphTargetWeight(0, 0.5f); // A {0,1}
    evaluateVertices(lbs, s->sub);

    s->sub->setMorphTargetWeight(1, 0.5f); // B {1,2}; union {1,2}
    evaluateVertices(lbs, s->sub);
    MorphWorkStats stats = s->sub->getMorphWorkStats();
    TASK_CHECK(stats.rebuiltVertices == 2);
    // Vertex 1 now reads both A and B; vertex 2 reads only B.
    TASK_CHECK(stats.deltaVisits == 3);

    // The shared vertex 1 reflects both weights, recomputed from base.
    std::vector<float> w = zeroWeights(s);
    w[0] = 0.5f;
    w[1] = 0.5f;
    TASK_CHECK(nearVec(s->sub->getVectorMorphedVertex()[1],
                       expectedLocalPosition(s, 1, w)));

    // Switch A off: only A's support {0,1} is rebuilt; at vertex 1 just B
    // remains nonzero, so one delta visit; vertex 0 has no nonzero delta.
    s->sub->setMorphTargetWeight(0, 0.0f);
    evaluateVertices(lbs, s->sub);
    stats = s->sub->getMorphWorkStats();
    TASK_CHECK(stats.rebuiltVertices == 2);
    TASK_CHECK(stats.deltaVisits == 1);

    w[0] = 0.0f;
    TASK_CHECK(nearVec(s->sub->getVectorMorphedVertex()[1],
                       expectedLocalPosition(s, 1, w)));
    TASK_CHECK(nearVec(s->sub->getVectorMorphedVertex()[0],
                       expectedLocalPosition(s, 0, w)));
    destroyScene(s);
    return 0;
}

static int cache_change_then_restore_between_queries_is_free()
{
    Scene* s = buildSparseWorkScene();
    CalPhysique lbs(s->model);
    evaluateVertices(lbs, s->sub);

    s->sub->setMorphTargetWeight(2, 0.7f); // C touches {3}
    evaluateVertices(lbs, s->sub);
    TASK_CHECK(s->sub->getMorphWorkStats().rebuiltVertices == 1);

    // Mutate weights with no query in between, then restore every one of them.
    s->sub->setMorphTargetWeight(0, 0.3f);
    s->sub->setMorphTargetWeight(1, -0.3f);
    s->sub->setMorphTargetWeight(2, 0.25f);
    s->sub->setMorphTargetWeight(0, 0.0f);
    s->sub->setMorphTargetWeight(1, 0.0f);
    s->sub->setMorphTargetWeight(2, 0.7f);

    evaluateVertices(lbs, s->sub);
    MorphWorkStats stats = s->sub->getMorphWorkStats();
    TASK_CHECK(stats.rebuiltVertices == 0);
    TASK_CHECK(stats.deltaVisits == 0);
    destroyScene(s);
    return 0;
}

static int cache_three_weight_write_paths()
{
    Scene* s = buildSparseWorkScene();
    CalPhysique lbs(s->model);
    lbs.setNormalization(false);
    evaluateVertices(lbs, s->sub);

    // Path 1: setMorphTargetWeight.
    s->sub->setMorphTargetWeight(0, 0.4f);
    evaluateVertices(lbs, s->sub);
    TASK_CHECK(s->sub->getMorphWorkStats().rebuiltVertices == 2);
    s->sub->setMorphTargetWeight(0, 0.0f);
    evaluateVertices(lbs, s->sub);

    // Path 2: writable reference returned by getVectorMorphTargetWeight.
    std::vector<float>& writable = s->sub->getVectorMorphTargetWeight();
    writable[2] = 0.6f; // C -> {3}
    evaluateVertices(lbs, s->sub);
    TASK_CHECK(s->sub->getMorphWorkStats().rebuiltVertices == 1);
    std::vector<float> w = zeroWeights(s);
    w[2] = 0.6f;
    TASK_CHECK(nearVec(s->sub->getVectorMorphedVertex()[3],
                       expectedLocalPosition(s, 3, w)));
    writable[2] = 0.0f;
    evaluateVertices(lbs, s->sub);

    // Path 3: the MorphTargetMixer drives setMorphTargetWeight internally.
    CalCoreAnimatedMorph* animated = new CalCoreAnimatedMorph;
    animated->setName("anim");
    animated->setDuration(2.0f);
    CalCoreMorphTrack* track = new CalCoreMorphTrack;
    track->setMorphID(1); // morph B
    track->setTargetMesh(0);
    track->addTargetSubMesh(0);
    CalCoreMorphKeyframe k0;
    k0.setTime(0.0f);
    k0.setWeight(0.0f);
    CalCoreMorphKeyframe k1;
    k1.setTime(1.0f);
    k1.setWeight(1.0f);
    track->addCoreMorphKeyframe(&k0);
    track->addCoreMorphKeyframe(&k1);
    animated->addCoreTrack(track);
    TASK_CHECK(s->core->addCoreAnimatedMorph(animated) >= 0);

    CalMorphTargetMixer* mixer = s->model->getMorphTargetMixer();
    TASK_CHECK(mixer->manualBlend(0, 1.0f, 0.5f));
    mixer->update(0.0f);
    TASK_CHECK(nearFloat(s->sub->getMorphTargetWeight(1), 0.5f));

    evaluateVertices(lbs, s->sub);
    // B support {1,2} was activated through the mixer.
    TASK_CHECK(s->sub->getMorphWorkStats().rebuiltVertices == 2);
    destroyScene(s);
    return 0;
}

static int cache_two_instances_stay_independent()
{
    std::vector<TargetSpec> specs(1);
    specs[0].name = "A";
    specs[0].edits.push_back(TargetEdit{ 0, CalVector(9, 1, 0), CalVector(0, 1, 0), true, false, 0, 0, false });

    // Two models share the same immutable core data.
    CalCoreModel* core = new CalCoreModel("shared");
    buildTwoBoneSkeleton(core);
    CalCoreMesh* mesh = new CalCoreMesh;
    CalCoreSubmesh* coreSub = new CalCoreSubmesh;
    coreSub->reserve(20, 0, 0, 0);
    for(int v = 0; v < 20; ++v)
    {
        CalCoreSubmesh::Vertex vert;
        vert.position = CalVector(2.0f + v, 1.0f, 0.0f);
        vert.normal = CalVector(0, 1, 0);
        vert.collapseId = v;
        vert.faceCollapseCount = 0;
        CalCoreSubmesh::Influence inf;
        inf.boneId = 0;
        inf.weight = 1.0f;
        vert.vectorInfluence.push_back(inf);
        coreSub->setVertex(v, vert);
    }
    CalCoreSubMorphTarget* mt = new CalCoreSubMorphTarget;
    mt->reserve(20);
    mt->setName("A");
    for(int v = 0; v < 20; ++v)
    {
        CalCoreSubMorphTarget::BlendVertex bv;
        bv.position = coreSub->getVectorVertex()[v].position;
        bv.normal = coreSub->getVectorVertex()[v].normal;
        mt->setBlendVertex(v, bv);
    }
    CalCoreSubMorphTarget::BlendVertex bv0;
    bv0.position = CalVector(9.0f, 1.0f, 0.0f);
    bv0.normal = CalVector(0, 1, 0);
    mt->setBlendVertex(0, bv0);
    coreSub->addCoreSubMorphTarget(mt);
    mesh->addCoreSubmesh(coreSub);
    core->addCoreMesh(mesh);

    CalModel* modelA = new CalModel(core);
    CalModel* modelB = new CalModel(core);
    TASK_CHECK(modelA->attachMesh(0));
    TASK_CHECK(modelB->attachMesh(0));
    modelA->getSkeleton()->calculateState();
    modelB->getSkeleton()->calculateState();
    CalSubmesh* a = modelA->getMesh(0)->getSubmesh(0);
    CalSubmesh* b = modelB->getMesh(0)->getSubmesh(0);

    CalPhysique physA(modelA);
    CalPhysique physB(modelB);
    std::vector<float> buf(20 * 3);
    physA.calculateVertices(a, &buf[0], 0);
    physB.calculateVertices(b, &buf[0], 0);

    // Alternate weights: each instance only pays for its own support union.
    a->setMorphTargetWeight(0, 0.5f);
    physA.calculateVertices(a, &buf[0], 0);
    TASK_CHECK(a->getMorphWorkStats().rebuiltVertices == 1);
    TASK_CHECK(b->getMorphWorkStats().rebuiltVertices == 0);
    TASK_CHECK(nearVec(b->getVectorMorphedVertex()[0],
                       coreSub->getVectorVertex()[0].position));

    b->setMorphTargetWeight(0, 0.25f);
    physB.calculateVertices(b, &buf[0], 0);
    TASK_CHECK(b->getMorphWorkStats().rebuiltVertices == 1);
    // A's second query with unchanged weights does no morph work at all.
    physA.calculateVertices(a, &buf[0], 0);
    TASK_CHECK(a->getMorphWorkStats().rebuiltVertices == 0);
    TASK_CHECK(a->getMorphWorkStats().deltaVisits == 0);

    CalVector expectedA(2.0f + 0.5f * (9.0f - 2.0f), 1.0f, 0.0f);
    CalVector expectedB(2.0f + 0.25f * (9.0f - 2.0f), 1.0f, 0.0f);
    TASK_CHECK(nearVec(a->getVectorMorphedVertex()[0], expectedA));
    TASK_CHECK(nearVec(b->getVectorMorphedVertex()[0], expectedB));

    delete modelA;
    delete modelB;
    delete core;
    return 0;
}

static int cache_repeated_toggles_have_no_drift_and_cross_entry_reuse()
{
    Scene* s = buildSparseWorkScene();
    CalPhysique lbs(s->model);
    CalPhysiqueDualQuat dq(s->model);
    lbs.setNormalization(false);
    dq.setNormalization(false);

    std::vector<float> w = zeroWeights(s);
    std::vector<float> nrm(s->sub->getVertexCount() * 3);
    std::vector<float> combined(s->sub->getVertexCount() * 6);
    std::vector<float> dpos(s->sub->getVertexCount() * 3);

    // Repeatedly toggle the same weights; results must always match the
    // formula computed from the base, never accumulated deltas.
    for(int iteration = 0; iteration < 8; ++iteration)
    {
        s->sub->setMorphTargetWeight(0, (iteration % 2) ? 0.5f : 0.0f);
        s->sub->setMorphTargetWeight(1, (iteration % 3) ? 0.25f : 0.0f);
        evaluateVertices(lbs, s->sub);
        w[0] = s->sub->getMorphTargetWeight(0);
        w[1] = s->sub->getMorphTargetWeight(1);
        for(int v = 0; v < 4; ++v)
        {
            TASK_CHECK(nearVec(s->sub->getVectorMorphedVertex()[v],
                               expectedLocalPosition(s, v, w)));
        }
    }

    // After the last evaluation, switching output entry points reuses the
    // cache: no morph rebuild, but skinning still runs.
    MorphWorkStats before = s->sub->getMorphWorkStats();
    TASK_CHECK(before.rebuiltVertices > 0);
    evaluateNormals(lbs, s->sub);
    evaluateVerticesAndNormals(lbs, s->sub);
    dq.calculateVertices(s->sub, &dpos[0], 0);
    MorphWorkStats after = s->sub->getMorphWorkStats();
    TASK_CHECK(after.rebuiltVertices == 0);
    TASK_CHECK(after.deltaVisits == 0);

    // Bone pose changes still flow through without rebuilding the morph.
    s->model->getSkeleton()->getBone(1)->setCoreState();
    dq.calculateVertices(s->sub, &dpos[0], 0);
    TASK_CHECK(s->sub->getMorphWorkStats().rebuiltVertices == 0);
    TASK_CHECK(std::isfinite(dpos[0]) && std::isfinite(dpos[1]));

    // Skinned coordinates never leak into the local cache.
    TASK_CHECK(nearVec(s->sub->getVectorMorphedVertex()[0],
                       expectedLocalPosition(s, 0, w)));
    destroyScene(s);
    return 0;
}

static int cache_delta_visits_bounded_by_associations()
{
    Scene* s = buildSparseWorkScene();
    CalPhysique lbs(s->model);
    evaluateVertices(lbs, s->sub);

    // Activate every supported target at once.
    s->sub->setMorphTargetWeight(0, 0.5f);
    s->sub->setMorphTargetWeight(1, 0.5f);
    s->sub->setMorphTargetWeight(2, 0.5f);
    evaluateVertices(lbs, s->sub);
    MorphWorkStats stats = s->sub->getMorphWorkStats();

    // Union of supports: A {0,1}, B {1,2}, C {3} -> four unique vertices.
    TASK_CHECK(stats.rebuiltVertices == 4);

    // Associated items: vertex 1 carries two pairs, the others one each.
    size_t associations = 1 + 2 + 1 + 1;
    TASK_CHECK(stats.deltaVisits <= associations);
    TASK_CHECK(stats.deltaVisits == associations); // all weights nonzero

    // The 60 filler vertices and three empty targets added no work.
    destroyScene(s);
    return 0;
}

// ==================================================================
// roundtrip group
// ==================================================================

static int roundtrip_diffmap_takes_offsets_and_expands_absolute()
{
    CalCoreModel core("diff");
    buildTwoBoneSkeleton(&core);
    CalCoreMesh* mesh = new CalCoreMesh;
    CalCoreSubmesh* sub = new CalCoreSubmesh;
    sub->reserve(3, 0, 0, 0);
    for(int v = 0; v < 3; ++v)
    {
        CalCoreSubmesh::Vertex vert;
        vert.position = CalVector(2.0f + v, 1.0f, 0.0f);
        vert.normal = CalVector(0.0f, 1.0f, 0.0f);
        vert.collapseId = v;
        vert.faceCollapseCount = 0;
        CalCoreSubmesh::Influence inf;
        inf.boneId = 0;
        inf.weight = 1.0f;
        vert.vectorInfluence.push_back(inf);
        sub->setVertex(v, vert);
    }

    // Difference map receives offsets; BlendVertex results stay absolute.
    CalCoreSubMorphTargetDiffMap* dm = new CalCoreSubMorphTargetDiffMap;
    TASK_CHECK(dm->reserve(1));
    CalCoreSubMorphTarget::BlendVertex offset;
    offset.position = CalVector(2.0f, 0.0f, 0.0f); // positional offset
    offset.normal   = CalVector(1.0f, 0.0f, 0.0f); // normal offset
    TASK_CHECK(dm->appendBlendVertex(1, offset));
    sub->addCoreSubMorphTarget(dm); // expands against the base shape

    const CalCoreSubMorphTarget::BlendVertex* omitted =
        sub->getVectorCoreSubMorphTarget()[0]->getBlendVertex(0);
    const CalCoreSubMorphTarget::BlendVertex* changed =
        sub->getVectorCoreSubMorphTarget()[0]->getBlendVertex(1);
    TASK_CHECK(nearVec(omitted->position, CalVector(2.0f, 1.0f, 0.0f)));
    TASK_CHECK(nearVec(changed->position, CalVector(5.0f, 1.0f, 0.0f)));
    TASK_CHECK(nearVec(changed->normal, CalVector(1.0f, 1.0f, 0.0f)));

    mesh->addCoreSubmesh(sub);
    core.addCoreMesh(mesh);
    CalModel model(&core);
    TASK_CHECK(model.attachMesh(0));
    model.getSkeleton()->calculateState();
    CalSubmesh* sm = model.getMesh(0)->getSubmesh(0);
    sm->setMorphTargetWeight(0, 0.5f);
    CalPhysique lbs(&model);
    std::vector<float> pos(sm->getVertexCount() * 3);
    lbs.calculateVertices(sm, &pos[0], 0);
    TASK_CHECK(nearVec(sm->getVectorMorphedVertex()[1],
                       CalVector(4.0f, 1.0f, 0.0f)));
    return 0;
}

static int roundtrip_xml_preserves_sparse_records()
{
    // One submesh exercising: tiny position move, normal-only edit,
    // UV-only edit and a fully unchanged target.
    CalCoreModel core("xmlrt");
    buildTwoBoneSkeleton(&core);
    CalCoreMesh* mesh = new CalCoreMesh;
    CalCoreSubmesh* sub = new CalCoreSubmesh;
    sub->reserve(4, 1, 0, 0);
    for(int v = 0; v < 4; ++v)
    {
        CalCoreSubmesh::Vertex vert;
        vert.position = CalVector(2.0f + v, 1.0f, 0.0f);
        vert.normal = CalVector(0.0f, 1.0f, 0.0f);
        vert.collapseId = v;
        vert.faceCollapseCount = 0;
        CalCoreSubmesh::Influence inf;
        inf.boneId = 0;
        inf.weight = 1.0f;
        vert.vectorInfluence.push_back(inf);
        sub->setVertex(v, vert);
        CalCoreSubmesh::TextureCoordinate tc;
        tc.u = 0.1f * v;
        tc.v = 0.2f * v;
        sub->setTextureCoordinate(v, 0, tc);
    }

    const int targetCount = 4;
    const char* names[targetCount] = { "tiny", "normalonly", "uvonly", "unchanged" };
    for(int t = 0; t < targetCount; ++t)
    {
        CalCoreSubMorphTarget* mt = new CalCoreSubMorphTarget;
        mt->reserve(4);
        mt->setName(names[t]);
        for(int v = 0; v < 4; ++v)
        {
            CalCoreSubMorphTarget::BlendVertex bv;
            bv.position = sub->getVectorVertex()[v].position;
            bv.normal = sub->getVectorVertex()[v].normal;
            bv.textureCoords.push_back(sub->getVectorVectorTextureCoordinate()[0][v]);
            mt->setBlendVertex(v, bv);
        }
        sub->addCoreSubMorphTarget(mt);
    }

    // Tiny position move (well under the old 1.0 write threshold).
    CalCoreSubMorphTarget::BlendVertex tiny =
        *sub->getVectorCoreSubMorphTarget()[0]->getBlendVertex(0);
    tiny.position.x += 0.001f;
    sub->getVectorCoreSubMorphTarget()[0]->setBlendVertex(0, tiny);

    // Normal-only change.
    CalCoreSubMorphTarget::BlendVertex normalOnly =
        *sub->getVectorCoreSubMorphTarget()[1]->getBlendVertex(1);
    normalOnly.normal = CalVector(1.0f, 0.0f, 0.0f);
    sub->getVectorCoreSubMorphTarget()[1]->setBlendVertex(1, normalOnly);

    // UV-only change.
    CalCoreSubMorphTarget::BlendVertex uvOnly =
        *sub->getVectorCoreSubMorphTarget()[2]->getBlendVertex(2);
    uvOnly.textureCoords[0].u = 0.333f;
    uvOnly.textureCoords[0].v = 0.666f;
    sub->getVectorCoreSubMorphTarget()[2]->setBlendVertex(2, uvOnly);

    mesh->addCoreSubmesh(sub);
    core.addCoreMesh(mesh);

    std::string path = tempPath("cal3d_morph_roundtrip.xmf");
    TASK_CHECK(CalSaver::saveXmlCoreMesh(path, mesh));

    // The file must carry one record per changed target and none for the
    // unchanged one, while the morph element itself is still written.
    std::ifstream in(path.c_str());
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    in.close();
    size_t recordCount = 0;
    size_t searchFrom = 0;
    while((searchFrom = text.find("<BLENDVERTEX", searchFrom)) != std::string::npos)
    {
        ++recordCount;
        searchFrom += 12;
    }
    TASK_CHECK(recordCount == 3);
    TASK_CHECK(text.find("NAME=\"unchanged\"") != std::string::npos);
    TASK_CHECK(text.find("NUMBLENDVERTS=\"0\"") != std::string::npos);
    TASK_CHECK(text.find("0.333") != std::string::npos);

    CalCoreMeshPtr loaded = CalLoader::loadXmlCoreMesh(path);
    TASK_CHECK(loaded.get() != 0);
    CalCoreSubmesh* lsub = loaded->getCoreSubmesh(0);
    TASK_CHECK(lsub->getCoreSubMorphTargetCount() == 4);

    const CalCoreSubMorphTarget* tinyTarget = lsub->getCoreSubMorphTarget(0);
    TASK_CHECK(nearFloat(tinyTarget->getBlendVertex(0)->position.x, 2.001f, 1.0e-6f));
    // Omitted vertices inherit the base shape.
    TASK_CHECK(nearVec(tinyTarget->getBlendVertex(1)->position,
                       sub->getVectorVertex()[1].position));

    const CalCoreSubMorphTarget* normalTarget = lsub->getCoreSubMorphTarget(1);
    TASK_CHECK(nearVec(normalTarget->getBlendVertex(1)->normal,
                       CalVector(1.0f, 0.0f, 0.0f)));
    TASK_CHECK(nearVec(normalTarget->getBlendVertex(0)->normal,
                       CalVector(0.0f, 1.0f, 0.0f)));

    const CalCoreSubMorphTarget* uvTarget = lsub->getCoreSubMorphTarget(2);
    TASK_CHECK(nearFloat(uvTarget->getBlendVertex(2)->textureCoords[0].u, 0.333f, 1.0e-6f));
    TASK_CHECK(nearFloat(uvTarget->getBlendVertex(2)->textureCoords[0].v, 0.666f, 1.0e-6f));

    const CalCoreSubMorphTarget* unchanged = lsub->getCoreSubMorphTarget(3);
    TASK_CHECK(unchanged->getBlendVertexCount() == 4);
    for(int v = 0; v < 4; ++v)
    {
        TASK_CHECK(nearVec(unchanged->getBlendVertex(v)->position,
                           sub->getVectorVertex()[v].position));
    }
    return 0;
}

static int roundtrip_binary_cmf_sparse_records()
{
    CalCoreModel core("cmfrt");
    buildTwoBoneSkeleton(&core);
    CalCoreMesh* mesh = new CalCoreMesh;
    CalCoreSubmesh* sub = new CalCoreSubmesh;
    sub->reserve(3, 0, 0, 0);
    for(int v = 0; v < 3; ++v)
    {
        CalCoreSubmesh::Vertex vert;
        vert.position = CalVector(2.0f + v, 1.0f, 0.0f);
        vert.normal = CalVector(0.0f, 1.0f, 0.0f);
        vert.collapseId = v;
        vert.faceCollapseCount = 0;
        CalCoreSubmesh::Influence inf;
        inf.boneId = 0;
        inf.weight = 1.0f;
        vert.vectorInfluence.push_back(inf);
        sub->setVertex(v, vert);
    }
    CalCoreSubMorphTarget* changed = new CalCoreSubMorphTarget;
    changed->reserve(3);
    changed->setName("changed");
    CalCoreSubMorphTarget* empty = new CalCoreSubMorphTarget;
    empty->reserve(3);
    empty->setName("empty");
    for(int t = 0; t < 2; ++t)
    {
        CalCoreSubMorphTarget* mt = (t == 0) ? changed : empty;
        for(int v = 0; v < 3; ++v)
        {
            CalCoreSubMorphTarget::BlendVertex bv;
            bv.position = sub->getVectorVertex()[v].position;
            bv.normal = sub->getVectorVertex()[v].normal;
            mt->setBlendVertex(v, bv);
        }
    }
    CalCoreSubMorphTarget::BlendVertex bv = *changed->getBlendVertex(2);
    bv.position = CalVector(4.0f + 0.0007f, 1.0f, 0.0f); // sub-threshold move
    changed->setBlendVertex(2, bv);
    sub->addCoreSubMorphTarget(changed);
    sub->addCoreSubMorphTarget(empty);

    mesh->addCoreSubmesh(sub);
    core.addCoreMesh(mesh);

    std::string path = tempPath("cal3d_morph_roundtrip.cmf");
    TASK_CHECK(CalSaver::saveCoreMesh(path, mesh));
    CalCoreMeshPtr loaded = CalLoader::loadCoreMesh(path);
    TASK_CHECK(loaded.get() != 0);
    CalCoreSubmesh* lsub = loaded->getCoreSubmesh(0);
    TASK_CHECK(lsub->getCoreSubMorphTargetCount() == 2);
    TASK_CHECK(nearFloat(lsub->getCoreSubMorphTarget(0)->getBlendVertex(2)->position.x,
                         4.0007f, 1.0e-6f));
    // The empty target survives with all vertices inheriting the base shape.
    const CalCoreSubMorphTarget* emptyLoaded = lsub->getCoreSubMorphTarget(1);
    TASK_CHECK(emptyLoaded->getBlendVertexCount() == 3);
    TASK_CHECK(nearVec(emptyLoaded->getBlendVertex(0)->position,
                       sub->getVectorVertex()[0].position));
    return 0;
}

static int roundtrip_handwritten_sparse_fixture()
{
    CalCoreMeshPtr loaded =
        CalLoader::loadCoreMesh(std::string("task_support/fixtures/sparse.xmf"));
    TASK_CHECK(loaded.get() != 0);
    CalCoreSubmesh* sub = loaded->getCoreSubmesh(0);
    const CalCoreSubMorphTarget* morph = sub->getCoreSubMorphTarget(0);

    // Vertex 0 is omitted in the hand-written sparse material: it must
    // inherit the base position (2,1,0), not load as zeros.
    TASK_CHECK(nearVec(morph->getBlendVertex(0)->position,
                       CalVector(2.0f, 1.0f, 0.0f)));
    TASK_CHECK(nearVec(morph->getBlendVertex(1)->position,
                       CalVector(4.001f, 1.0f, 0.0f)));
    TASK_CHECK(nearVec(morph->getBlendVertex(2)->position,
                       CalVector(2.0f, 3.0f, 0.0f)));

    // Runtime evaluation of the loaded target matches the absolute formula.
    CalCoreModel core("fixture");
    buildTwoBoneSkeleton(&core);
    // Share ownership: the local RefPtr and the core model both hold a
    // reference, so the mesh is released exactly once at scope exit.
    core.addCoreMesh(loaded.get());
    CalModel model(&core);
    TASK_CHECK(model.attachMesh(0));
    model.getSkeleton()->calculateState();
    CalSubmesh* sm = model.getMesh(0)->getSubmesh(0);
    sm->setMorphTargetWeight(0, 0.5f);
    CalPhysique lbs(&model);
    std::vector<float> pos(sm->getVertexCount() * 3);
    lbs.calculateVertices(sm, &pos[0], 0);
    CalVector expectedLocal(4.0f + 0.5f * 0.001f, 1.0f, 0.0f);
    TASK_CHECK(nearVec(sm->getVectorMorphedVertex()[1], expectedLocal, 1.0e-5f));
    return 0;
}

static int roundtrip_empty_target_support_and_stats()
{
    // A fully unchanged target must save/load and must never create sparse
    // work even when assigned a nonzero weight.
    std::vector<TargetSpec> specs(2);
    specs[0].name = "real";
    specs[1].name = "noop";
    specs[0].edits.push_back(TargetEdit{ 0, CalVector(9, 1, 0), CalVector(0, 1, 0), true, false, 0, 0, false });

    Scene* s = buildScene(10, false, false, specs);
    s->sub->setMorphTargetWeight(1, 0.9f); // noop target
    CalPhysique lbs(s->model);
    evaluateVertices(lbs, s->sub);
    TASK_CHECK(s->sub->getMorphWorkStats().rebuiltVertices == 0);
    TASK_CHECK(s->sub->getMorphWorkStats().deltaVisits == 0);
    TASK_CHECK(nearVec(s->sub->getVectorMorphedVertex()[0],
                       s->coreSub->getVectorVertex()[0].position));

    std::string path = tempPath("cal3d_empty_target.xmf");
    TASK_CHECK(CalSaver::saveXmlCoreMesh(path, s->mesh));
    CalCoreMeshPtr loaded = CalLoader::loadXmlCoreMesh(path);
    TASK_CHECK(loaded.get() != 0);
    TASK_CHECK(loaded->getCoreSubmesh(0)->getCoreSubMorphTargetCount() == 2);
    TASK_CHECK(loaded->getCoreSubmesh(0)->getCoreSubMorphTarget(1)
                 ->getBlendVertexCount() == 10);
    destroyScene(s);
    return 0;
}

// ==================================================================
// Registration
// ==================================================================

extern const struct task_case task_cases[];
const struct task_case task_cases[] =
{
    { "representation", "canonical_all_entries_agree",       representation_canonical_all_entries_agree },
    { "representation", "negative_and_overlapping_weights",  representation_negative_and_overlapping_weights },
    { "representation", "normalization_flag",                representation_normalization_flag },
    { "representation", "output_uv_uses_base_mesh",          representation_output_uv_uses_base_mesh },
    { "representation", "dq_is_not_replaced_by_lbs",         representation_dq_is_not_replaced_by_lbs },

    { "skinning", "rotated_translated_skeleton",            skinning_rotated_translated_skeleton },
    { "skinning", "axis_factors_apply_after_morph",         skinning_axis_factors_apply_after_morph },
    { "skinning", "byte_stride_padding_preserved",          skinning_byte_stride_padding_preserved },
    { "skinning", "springs_internal_write_rules",           skinning_springs_internal_write_rules },
    { "skinning", "lod_changes_count_without_morph_rebuild", skinning_lod_changes_count_without_morph_rebuild },

    { "cache", "initial_evaluation_is_free",                cache_initial_evaluation_is_free },
    { "cache", "first_change_matches_support_union",        cache_first_change_matches_support_union },
    { "cache", "overlapping_targets_incremental",           cache_overlapping_targets_incremental },
    { "cache", "change_then_restore_between_queries_is_free", cache_change_then_restore_between_queries_is_free },
    { "cache", "three_weight_write_paths",                  cache_three_weight_write_paths },
    { "cache", "two_instances_stay_independent",            cache_two_instances_stay_independent },
    { "cache", "repeated_toggles_no_drift_cross_entry_reuse", cache_repeated_toggles_have_no_drift_and_cross_entry_reuse },
    { "cache", "delta_visits_bounded_by_associations",      cache_delta_visits_bounded_by_associations },

    { "roundtrip", "diffmap_offsets_expand_absolute",       roundtrip_diffmap_takes_offsets_and_expands_absolute },
    { "roundtrip", "xml_preserves_sparse_records",          roundtrip_xml_preserves_sparse_records },
    { "roundtrip", "binary_cmf_sparse_records",             roundtrip_binary_cmf_sparse_records },
    { "roundtrip", "handwritten_sparse_fixture",            roundtrip_handwritten_sparse_fixture },
    { "roundtrip", "empty_target_support_and_stats",        roundtrip_empty_target_support_and_stats },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
