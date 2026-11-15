//****************************************************************************//
// sculptmorph_example.cpp                                                    //
// Windowless end-to-end example for the CalSculptMorph capture/bake API.    //
//                                                                            //
// Build (from the repository root), reusing the task build flags:           //
//   c++ -std=c++17 -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -Isrc -I. \ //
//       devcheck/sculptmorph_example.cpp .task-build/libcal3d.a             //
//                                                                            //
// The program creates a small skeleton and submesh, captures the same       //
// sculpted vertex in two different animated poses, bakes one bind-space     //
// additive morph target, attaches it and shows that a plain weight-1        //
// CalSubmesh reproduces the sculpted model-space positions.                 //
//****************************************************************************//

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "cal3d/animation.h"
#include "cal3d/coreanimation.h"
#include "cal3d/corebone.h"
#include "cal3d/corekeyframe.h"
#include "cal3d/coremesh.h"
#include "cal3d/coremorphtarget.h"
#include "cal3d/coreskeleton.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/coretrack.h"
#include "cal3d/mixer.h"
#include "cal3d/physique.h"
#include "cal3d/quaternion.h"
#include "cal3d/sculptmorph.h"
#include "cal3d/skeleton.h"
#include "cal3d/submesh.h"

using namespace CalSculptMorph;

namespace {

void skin(const CalSkeleton& skeleton, const CalSubmesh& submesh, std::vector<CalVector4>& out) {
    out.resize(2 * submesh.coreSubmesh->getVertexCount());
    CalPhysique::calculateVerticesAndNormals(
        cal3d::pointerFromVector(skeleton.boneTransforms),
        &submesh,
        &out[0].x);
}

int fail(const std::string& stage, const std::string& message) {
    std::fprintf(stderr, "%s failed: %s\n", stage.c_str(), message.c_str());
    return 1;
}

} // namespace

int main() {
    // Two bones, each with a non-identity inverse-bind pose.
    ISTD::shared_ptr<CalCoreSkeleton> coreSkeleton = ISTD::make_shared<CalCoreSkeleton>();
    ISTD::shared_ptr<CalCoreBone> root = ISTD::make_shared<CalCoreBone>("root", -1);
    root->relativeTransform = cal3d::RotateTranslate(CalQuaternion(), CalVector(1.0f, 0.0f, 0.0f));
    root->inverseBindPoseTransform = cal3d::RotateTranslate(CalQuaternion(), CalVector(-1.0f, 0.0f, 0.0f));
    coreSkeleton->addCoreBone(root);
    coreSkeleton->coreBones[0]->relativeTransform.translation = CalVector(1.0f, 0.0f, 0.0f);

    ISTD::shared_ptr<CalCoreBone> child = ISTD::make_shared<CalCoreBone>("child", 0);
    child->relativeTransform = cal3d::RotateTranslate(CalQuaternion(), CalVector(0.0f, 1.0f, 0.0f));
    child->inverseBindPoseTransform = cal3d::RotateTranslate(CalQuaternion(), CalVector(0.0f, -1.0f, 0.0f));
    coreSkeleton->addCoreBone(child);

    // One vertex weighted evenly across both bones.
    ISTD::shared_ptr<CalCoreSubmesh> coreSubmesh = ISTD::make_shared<CalCoreSubmesh>(1, false, 0);
    CalCoreSubmesh::Vertex vertex;
    vertex.position = CalPoint4(0.5f, 0.5f, 0.5f);
    vertex.normal = CalVector4(0.0f, 0.0f, 1.0f);
    std::vector<CalCoreSubmesh::Influence> influences;
    influences.push_back(CalCoreSubmesh::Influence(0, 0.5f, false));
    influences.push_back(CalCoreSubmesh::Influence(1, 0.5f, true));
    coreSubmesh->addVertex(vertex, 0, influences);

    CalSkeleton skeleton(coreSkeleton);
    CalMixer mixer;

    ISTD::shared_ptr<CalCoreAnimation> coreAnimation = ISTD::make_shared<CalCoreAnimation>();
    CalCoreTrack::KeyframeList keys;
    keys.push_back(CalCoreKeyframe(0, CalVector(0, 0, 0), CalQuaternion(0, 0, 1, 0)));
    coreAnimation->tracks.push_back(CalCoreTrack(1, keys));
    ISTD::shared_ptr<CalAnimation> animation = ISTD::make_shared<CalAnimation>(coreAnimation, 1, 0);

    CalSubmesh submesh(coreSubmesh);

    // The artist sculpted a bind-space shape; in each pose that same bind-
    // space increment maps to model space through the vertex's blended
    // palette.  Capture the resulting model-space target in two poses.
    const double bindIncrement[3] = { 0.10, -0.07, 0.04 };
    const CalCoreSubmesh::InfluenceVector& influenceVector = coreSubmesh->getInfluences();
    Snapshot snapshots[2];

    for (int pose = 0; pose < 2; ++pose) {
        if (pose == 0) {
            mixer.addAnimation(animation);
        } else {
            mixer.removeAnimation(animation);
        }
        mixer.updateSkeleton(&skeleton, {}, {});

        std::vector<CalVector4> current;
        skin(skeleton, submesh, current);

        // Same palette interpretation as the production scalar skinning path.
        BoneTransform palette = CalPhysique::calculateVertexPalette(
            cal3d::pointerFromVector(skeleton.boneTransforms),
            cal3d::pointerFromVector(influenceVector),
            influenceVector.size());
        const float linear[3][3] = {
            { palette.rowx.x, palette.rowx.y, palette.rowx.z },
            { palette.rowy.x, palette.rowy.y, palette.rowy.z },
            { palette.rowz.x, palette.rowz.y, palette.rowz.z }
        };
        float modelDelta[3] = { 0.0f, 0.0f, 0.0f };
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                modelDelta[r] += linear[r][c] * static_cast<float>(bindIncrement[c]);
            }
        }

        std::vector<Observation> observations;
        observations.push_back(Observation(0, CalPoint4(
            current[0].x + modelDelta[0],
            current[0].y + modelDelta[1],
            current[0].z + modelDelta[2])));

        std::string error;
        if (!captureSculptPose(skeleton, submesh, &observations[0], observations.size(),
                1.0, snapshots[pose], &error)) {
            return fail("capture", error);
        }
    }

    // Bake both snapshots into one bind-space additive target.
    const Snapshot* snapshotPtrs[2] = { &snapshots[0], &snapshots[1] };
    ISTD::shared_ptr<CalCoreMorphTarget> morphTarget;
    BakeReport report;
    std::string error;
    if (!bakeSculptMorphTarget(coreSubmesh, snapshotPtrs, 2, "sculpt.additive",
            1e-4, morphTarget, report, &error)) {
        return fail("bake", error);
    }

    std::printf("baked %u sparse offset(s); rank=%u maxResidual=%.3g\n",
        static_cast<unsigned>(morphTarget->vertexOffsets.size()),
        report.vertexReports[0].effectiveRank,
        report.maxResidual);

    // The bake never attaches the target; the caller does that explicitly.
    coreSubmesh->addMorphTarget(morphTarget);

    // Play back the second pose through an ordinary weight-1 submesh.
    mixer.removeAnimation(animation);
    mixer.updateSkeleton(&skeleton, {}, {});

    std::vector<CalVector4> baseline;
    skin(skeleton, submesh, baseline);

    CalSubmesh playback(coreSubmesh);
    playback.setMorphTargetWeight("sculpt.additive", 1.0f);
    std::vector<CalVector4> morphed;
    skin(skeleton, playback, morphed);

    // Recompute the pose-2 model-space delta implied by the bind increment.
    BoneTransform palette = CalPhysique::calculateVertexPalette(
        cal3d::pointerFromVector(skeleton.boneTransforms),
        cal3d::pointerFromVector(influenceVector),
        influenceVector.size());
    const float linear[3][3] = {
        { palette.rowx.x, palette.rowx.y, palette.rowx.z },
        { palette.rowy.x, palette.rowy.y, palette.rowy.z },
        { palette.rowz.x, palette.rowz.y, palette.rowz.z }
    };
    float expectedDelta[3] = { 0.0f, 0.0f, 0.0f };
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            expectedDelta[r] += linear[r][c] * static_cast<float>(bindIncrement[c]);
        }
    }

    const double dx = morphed[0].x - baseline[0].x;
    const double dy = morphed[0].y - baseline[0].y;
    const double dz = morphed[0].z - baseline[0].z;
    std::printf("reproduced model-space delta: (%.4f, %.4f, %.4f)\n", dx, dy, dz);

    if (std::fabs(dx - expectedDelta[0]) > 2e-4
        || std::fabs(dy - expectedDelta[1]) > 2e-4
        || std::fabs(dz - expectedDelta[2]) > 2e-4) {
        return fail("playback", "morphed position does not match the sculpt target");
    }
    std::puts("SCULPT_MORPH_EXAMPLE_OK");
    return 0;
}
