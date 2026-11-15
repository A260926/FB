//****************************************************************************//
// sculptmorph.h                                                              //
// Bake sculpted vertex positions, captured on posed runtime characters,     //
// into a single bind-space additive Cal3D morph target.                     //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cal3d/BoostToStd.h"
#include "cal3d/global.h"
#include "cal3d/vector4.h"

class CalSkeleton;
class CalSubmesh;
class CalCoreSubmesh;
class CalCoreMorphTarget;

namespace CalSculptMorph {

// One sculpted vertex: a core-submesh vertex id and the model-space position
// the artist wants that vertex to reach in the current pose.
struct Observation {
    Observation()
        : vertexId(0)
    {}

    Observation(size_t id, const CalPoint4& modelSpacePosition)
        : vertexId(id)
        , targetPosition(modelSpacePosition)
    {}

    size_t vertexId;
    CalPoint4 targetPosition;
};

// Per-vertex line of the bake report.  Reports are emitted in ascending
// vertexId order, one per vertex covered by at least one observation.
struct VertexReport {
    VertexReport()
        : vertexId(0)
        , effectiveRank(0)
        , maxResidual(0.0)
    {}

    size_t vertexId;

    // Number of singular values of the stacked sqrt(weight) * A matrices that
    // survive the 1e-8 * max(1, sigmaMax) zero threshold.  Always in [0, 3];
    // a rank below 3 is a normal outcome, not an error.
    unsigned effectiveRank;

    // Largest Euclidean model-space residual over every observation of this
    // vertex, recomputed from the float d that is actually stored in the
    // morph target.
    double maxResidual;
};

struct BakeReport {
    BakeReport()
        : maxResidual(0.0)
        , snapshotCount(0)
    {}

    // One entry per observed vertex, sorted by vertexId.
    std::vector<VertexReport> vertexReports;

    // Largest maxResidual across every observed vertex (0.0 when no vertex was
    // observed).
    double maxResidual;

    // Number of snapshots that participated in the bake.
    size_t snapshotCount;
};

// An owning, runtime-independent capture of one posed character.
//
// A snapshot keeps a shared_ptr to the core submesh and plain-old-data copies
// of everything else.  After captureSculptPose() returns, the caller may
// retarget animations, change morph weights, mutate or free the observation
// array, and destroy the CalSkeleton and CalSubmesh; none of that affects the
// snapshot.  Default-constructed snapshots are empty; move them freely, do not
// copy them across modules expecting identity.
class CAL3D_API Snapshot {
public:
    Snapshot();
    ~Snapshot();
    Snapshot(Snapshot&& other);
    Snapshot& operator=(Snapshot&& other);

    bool empty() const {
        return m_records.empty();
    }

    size_t observationCount() const {
        return m_records.size();
    }

    double observationWeight() const {
        return m_weight;
    }

    // The core submesh this snapshot was captured from.  Never null for a
    // snapshot returned successfully from captureSculptPose().
    const ISTD::shared_ptr<CalCoreSubmesh>& coreSubmesh() const {
        return m_coreSubmesh;
    }

private:
    friend CAL3D_API bool captureSculptPose(
        const CalSkeleton& skeleton,
        const CalSubmesh& submesh,
        const Observation* observations,
        size_t observationCount,
        double observationWeight,
        Snapshot& outSnapshot,
        std::string* errorMessage);

    friend CAL3D_API bool bakeSculptMorphTarget(
        const ISTD::shared_ptr<CalCoreSubmesh>& coreSubmesh,
        const Snapshot* const* snapshots,
        size_t snapshotCount,
        const std::string& targetName,
        double residualLimit,
        ISTD::shared_ptr<CalCoreMorphTarget>& outMorphTarget,
        BakeReport& outReport,
        std::string* errorMessage);

    // Internal bake-time verifier, implemented in sculptmorph.cpp.
    static bool verifyForBake(const Snapshot& snapshot, std::string* errorMessage);

    struct Record {
        Record();

        size_t vertexId;

        // Artist-supplied model-space target, as doubles.
        double target[3];

        // Model-space position produced by the production CPU skinning path
        // at capture time, including every morph target active on the
        // CalSubmesh, as doubles.
        double skinnedBase[3];

        // 3x3 linear part of the vertex's actual bone-weight blended palette
        // at capture time (row-major, doubles).  Translation is deliberately
        // not stored: the unknown is a bind-space position increment, which
        // the palette maps to model space linearly.
        double palette[3][3];

        // The exact influence run (bone id, weight) used to build palette,
        // retained so bake can re-verify bone indices and weights against the
        // core submesh.
        std::vector<std::pair<unsigned, float> > influences;

        // Core (bind-space) vertex position at capture time, for the
        // unchanged-mesh re-verification at bake time.
        double corePosition[3];
        double coreNormal[3];
    };

    ISTD::shared_ptr<CalCoreSubmesh> m_coreSubmesh;
    size_t m_vertexCount;
    size_t m_faceCount;
    double m_weight;
    std::vector<Record> m_records;

    // Non-copyable: a snapshot owns shared mesh state and is meant to be moved.
    Snapshot(const Snapshot&);
    Snapshot& operator=(const Snapshot&);
};

// Capture one posed character.
//
// Call after CalMixer::updateSkeleton() has finished for this frame; the
// skeleton's current boneTransforms are sampled as-is, with no fixup or bone
// id reordering.  For every observation the production CPU skinning path
// (CalPhysique, including the currently active morph target weights on the
// submesh) establishes the skinned baseline position, and the actual
// bone-weight blended palette is captured alongside it.
//
// observationWeight must be strictly positive and finite; it weights this
// snapshot's rows in the bake least squares.  An empty observation array is
// accepted and yields an empty snapshot.
//
// Validation, failing with false (and an error message when errorMessage is
// non-null) without touching outSnapshot:
//   * every target position is finite;
//   * every vertexId is in range of the submesh and no vertexId repeats in
//     this call;
//   * every core-submesh influence weight is finite and non-negative, each
//     vertex's influence weights sum to 1 (weights are never renormalized to
//     hide bad data), influence runs are well formed, and every bone id is in
//     range of the skeleton;
//   * the captured palette entries are finite.
//
// On success outSnapshot is overwritten with an owning snapshot; inputs are
// never modified.
CAL3D_API bool captureSculptPose(
    const CalSkeleton& skeleton,
    const CalSubmesh& submesh,
    const Observation* observations,
    size_t observationCount,
    double observationWeight,
    Snapshot& outSnapshot,
    std::string* errorMessage = 0);

// Bake one bind-space additive morph target from several snapshots of the
// same core submesh.
//
// For each observed vertex the unknown is a single bind-space position
// increment d.  In snapshot k, A_k is the 3x3 linear part of the captured
// palette and b_k is (sculpt target - skinned baseline).  The bake solves
//
//     minimize  sum_k weight_k * || A_k * d - b_k ||^2
//
// in double precision, jointly over all snapshots, taking the minimum
// Euclidean-norm solution when the system is rank deficient.  A single
// snapshot's palette may be singular; several poses together can restore the
// rank, and rank deficiency is reported, never treated as an error.  No
// regularization is added.  A singular value of the stacked
// sqrt(weight_k) * A_k matrix counts as zero when it is no greater than
// 1e-8 * max(1, sigmaMax).
//
// targetName must end in ".additive" (the target is created as
// CalMorphTargetTypeAdditive through the ordinary CalCoreMorphTarget
// constructor).  residualLimit must be finite and non-negative; it is compared
// against residuals recomputed from the float d actually stored.  Vertices
// with no observations are left untouched; an exactly zero float increment
// produces no sparse VertexOffset, so an entirely empty target is possible;
// all normal increments are zero.
//
// The returned target is independent: it is not attached to the submesh, the
// submesh and the snapshots are not modified, and callers attach it
// themselves with CalCoreSubmesh::addMorphTarget().  Existing CMF
// save/load code needs no format changes.
//
// Validation, failing with false (plus errorMessage), a null out target and
// no partial results:
//   * targetName ends in ".additive", residualLimit is finite and >= 0;
//   * every snapshot belongs to coreSubmesh (pointer identity of the shared
//     core mesh), snapshots and their weight/records are finite and in range,
//     no vertex id repeats inside one snapshot;
//   * the core submesh's vertices, normals, faces and influence bone
//     ids/weights still match what was captured (the capture-to-bake
//     invariant);
//   * any stored float-d residual exceeds residualLimit.
CAL3D_API bool bakeSculptMorphTarget(
    const ISTD::shared_ptr<CalCoreSubmesh>& coreSubmesh,
    const Snapshot* const* snapshots,
    size_t snapshotCount,
    const std::string& targetName,
    double residualLimit,
    ISTD::shared_ptr<CalCoreMorphTarget>& outMorphTarget,
    BakeReport& outReport,
    std::string* errorMessage = 0);

} // namespace CalSculptMorph

// Windowless usage sketch (also provided as a compilable program in
// devcheck/sculptmorph_example.cpp):
//
//   using namespace CalSculptMorph;
//   CalSculptMorph::Snapshot snapshots[2];
//   const Observation obs0[] = { Observation(0, CalPoint4(1.2f, 0.1f, 0.0f)) };
//   mixer.updateSkeleton(&skeleton, {}, {});
//   std::string err;
//   if (!captureSculptPose(skeleton, submesh, obs0, 1, 1.0, snapshots[0], &err)) {
//       /* err describes the bad input; nothing was changed */
//       return 1;
//   }
//   // ... drive the mixer to another pose and capture snapshots[1] ...
//   const Snapshot* all[] = { &snapshots[0], &snapshots[1] };
//   ISTD::shared_ptr<CalCoreMorphTarget> target;
//   BakeReport report;
//   if (!bakeSculptMorphTarget(coreSubmesh, all, 2, "sculpt.additive",
//                              1e-4, target, report, &err)) {
//       /* rank deficiency alone does not fail; only validation or the
//          residual limit does.  target stays null and inputs are untouched. */
//       return 1;
//   }
//   coreSubmesh->addMorphTarget(target); // attach explicitly, if desired
