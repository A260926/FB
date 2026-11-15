//****************************************************************************//
// sculpt_bake_target.cpp                                                     //
// Task checks for the bind-space sculpt morph capture/bake pipeline.        //
//                                                                            //
// Registered through task_support/check.h; this translation unit provides   //
// no main() -- task_support/driver.c supplies the runner.                   //
//****************************************************************************//

// The C test driver (task_support/driver.c) is fed to the C++ compiler by
// task_support/run.sh, so the registration symbols share C++ linkage; check.h
// is included plain and its TASK_CHECK macro is used as written.
#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cal3d/animation.h"
#include "cal3d/bone.h"
#include "cal3d/bonetransform.h"
#include "cal3d/buffersource.h"
#include "cal3d/coreanimation.h"
#include "cal3d/corebone.h"
#include "cal3d/corekeyframe.h"
#include "cal3d/coremesh.h"
#include "cal3d/coremorphtarget.h"
#include "cal3d/coreskeleton.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/coretrack.h"
#include "cal3d/loader.h"
#include "cal3d/mixer.h"
#include "cal3d/physique.h"
#include "cal3d/quaternion.h"
#include "cal3d/saver.h"
#include "cal3d/sculptmorph.h"
#include "cal3d/skeleton.h"
#include "cal3d/submesh.h"
#include "cal3d/transform.h"

using namespace CalSculptMorph;

namespace {

const double kPi = 3.14159265358979323846;

bool closeDouble(double a, double b, double tolerance) {
    return std::fabs(a - b) <= tolerance;
}

template<typename TA, typename TB>
bool closeVector3(const TA a[3], const TB b[3], double tolerance) {
    for (int i = 0; i < 3; ++i) {
        if (!closeDouble(static_cast<double>(a[i]), static_cast<double>(b[i]), tolerance)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Independent numerical reference.
//
// The production code solves the weighted least-squares system with a
// one-sided Jacobi SVD of the tall stacked matrix.  This reference uses a
// deliberately different route: it forms the 3x3 Gram matrix M^T * M and
// M^T * rhs, runs a cyclic Jacobi eigen-decomposition of that symmetric
// matrix, and builds the pseudo-inverse from its eigenvalues/eigenvectors.
// Singular values are sqrt(eigenvalue); the same 1e-8 * max(1, sigmaMax)
// threshold decides rank.  No code is shared with sculptmorph.cpp.
// ---------------------------------------------------------------------------

struct RefRow {
    double a[3][3];
    double b[3];
    double weight;
};

void symmetricJacobiEigen3(double input[3][3], double eigenvalues[3], double eigenvectors[3][3]) {
    double a[3][3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            a[r][c] = input[r][c];
            eigenvectors[r][c] = (r == c) ? 1.0 : 0.0;
        }
    }

    for (int sweep = 0; sweep < 80; ++sweep) {
        double offDiagonal = 0.0;
        for (int p = 0; p < 3; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                offDiagonal += a[p][q] * a[p][q];
            }
        }
        if (offDiagonal <= 1e-300) {
            break;
        }

        for (int p = 0; p < 3; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (a[p][q] == 0.0) {
                    continue;
                }
                const double tau = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (tau >= 0.0 ? 1.0 : -1.0)
                    / (std::fabs(tau) + std::sqrt(1.0 + tau * tau));
                const double c = 1.0 / std::sqrt(1.0 + t * t);
                const double s = t * c;

                for (int k = 0; k < 3; ++k) {
                    const double apk = a[p][k];
                    const double aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double akp = a[k][p];
                    const double akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = eigenvectors[k][p];
                    const double vkq = eigenvectors[k][q];
                    eigenvectors[k][p] = c * vkp - s * vkq;
                    eigenvectors[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }

    for (int i = 0; i < 3; ++i) {
        eigenvalues[i] = a[i][i];
    }

    // Ascending insertion sort of eigenvalues, carrying eigenvector columns.
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            if (eigenvalues[j] < eigenvalues[i]) {
                std::swap(eigenvalues[i], eigenvalues[j]);
                for (int k = 0; k < 3; ++k) {
                    std::swap(eigenvectors[k][i], eigenvectors[k][j]);
                }
            }
        }
    }
}

// Minimum-norm weighted least squares, independent of the production SVD.
void referenceSolve(const std::vector<RefRow>& rows, double dOut[3], unsigned& rankOut, double& sigmaMaxOut) {
    double gram[3][3] = { {0, 0, 0}, {0, 0, 0}, {0, 0, 0} };
    double normalRhs[3] = { 0.0, 0.0, 0.0 };

    for (size_t k = 0; k < rows.size(); ++k) {
        const double scale = std::sqrt(rows[k].weight);
        double m[3][3];
        double y[3];
        for (int r = 0; r < 3; ++r) {
            y[r] = scale * rows[k].b[r];
            for (int c = 0; c < 3; ++c) {
                m[r][c] = scale * rows[k].a[r][c];
            }
        }
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                double sum = 0.0;
                for (int r = 0; r < 3; ++r) {
                    sum += m[r][i] * m[r][j];
                }
                gram[i][j] += sum;
            }
            double rhsSum = 0.0;
            for (int r = 0; r < 3; ++r) {
                rhsSum += m[r][i] * y[r];
            }
            normalRhs[i] += rhsSum;
        }
    }

    double eigenvalues[3];
    double eigenvectors[3][3];
    symmetricJacobiEigen3(gram, eigenvalues, eigenvectors);

    double sigma[3];
    for (int i = 0; i < 3; ++i) {
        sigma[i] = std::sqrt(std::max(0.0, eigenvalues[i]));
    }
    sigmaMaxOut = sigma[2];
    const double threshold = 1e-8 * std::max(1.0, sigmaMaxOut);

    for (int i = 0; i < 3; ++i) {
        dOut[i] = 0.0;
    }
    rankOut = 0;
    for (int j = 0; j < 3; ++j) {
        if (sigma[j] <= threshold) {
            continue;
        }
        ++rankOut;
        double projection = 0.0;
        for (int i = 0; i < 3; ++i) {
            projection += eigenvectors[i][j] * normalRhs[i];
        }
        const double coefficient = projection / (sigma[j] * sigma[j]);
        for (int i = 0; i < 3; ++i) {
            dOut[i] += coefficient * eigenvectors[i][j];
        }
    }
}

double referenceResidual(const std::vector<RefRow>& rows, const float dFloat[3]) {
    double worst = 0.0;
    for (size_t k = 0; k < rows.size(); ++k) {
        double squared = 0.0;
        for (int r = 0; r < 3; ++r) {
            double predicted = 0.0;
            for (int c = 0; c < 3; ++c) {
                predicted += rows[k].a[r][c] * static_cast<double>(dFloat[c]);
            }
            const double residual = predicted - rows[k].b[r];
            squared += residual * residual;
        }
        worst = std::max(worst, std::sqrt(squared));
    }
    return worst;
}

// Sculpt targets are stored as float CalPoint4 values; capture stores
// (float target - double skinned baseline).  Replicate that exact rounding so
// the reference least squares sees the same b rows as the production code.
void actualTargetDelta(const CalVector4& skinned, const double intended[3], double out[3]) {
    for (int r = 0; r < 3; ++r) {
        const float target = static_cast<float>((&skinned.x)[r] + intended[r]);
        out[r] = static_cast<double>(target) - static_cast<double>((&skinned.x)[r]);
    }
}

// Re-derives a vertex palette straight from the float bone transforms and
// the recorded influence weights, without calling the production palette
// helper, so tests cross-check the shared CalPhysique interpretation.
void referencePalette(
    const CalSkeleton& skeleton,
    const std::vector<CalCoreSubmesh::Influence>& influences,
    double out[3][3])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r][c] = 0.0;
        }
    }
    for (size_t i = 0; i < influences.size(); ++i) {
        const BoneTransform& bt = skeleton.boneTransforms[influences[i].boneId];
        const float rows[3][3] = {
            { bt.rowx.x, bt.rowx.y, bt.rowx.z },
            { bt.rowy.x, bt.rowy.y, bt.rowy.z },
            { bt.rowz.x, bt.rowz.y, bt.rowz.z }
        };
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                out[r][c] += static_cast<double>(influences[i].weight) * rows[r][c];
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Test rig construction.
// ---------------------------------------------------------------------------

CalQuaternion axisAngle(float x, float y, float z, float angle) {
    CalQuaternion q;
    q.setAxisAngle(CalVector(x, y, z), angle);
    return q;
}

// Three-bone skeleton with deliberately non-identity inverse-bind poses.
ISTD::shared_ptr<CalCoreSkeleton> buildThreeBoneCoreSkeleton() {
    auto core = ISTD::make_shared<CalCoreSkeleton>();

    auto root = ISTD::make_shared<CalCoreBone>("root", -1);
    root->relativeTransform = cal3d::RotateTranslate(CalQuaternion(), CalVector(1.0f, 0.0f, 0.0f));
    root->inverseBindPoseTransform = cal3d::RotateTranslate(
        axisAngle(0, 0, 1, static_cast<float>(kPi / 6.0)),
        CalVector(-1.0f, 0.0f, 0.0f));
    core->addCoreBone(root);

    auto child = ISTD::make_shared<CalCoreBone>("child", 0);
    child->relativeTransform = cal3d::RotateTranslate(CalQuaternion(), CalVector(0.0f, 1.0f, 0.0f));
    child->inverseBindPoseTransform = cal3d::RotateTranslate(
        axisAngle(1, 0, 0, static_cast<float>(kPi / 4.0)),
        CalVector(0.0f, -1.0f, 0.0f));
    core->addCoreBone(child);

    auto grandChild = ISTD::make_shared<CalCoreBone>("grandchild", 1);
    grandChild->relativeTransform = cal3d::RotateTranslate(CalQuaternion(), CalVector(0.0f, 0.0f, 1.0f));
    grandChild->inverseBindPoseTransform = cal3d::RotateTranslate(
        axisAngle(0, 1, 0, static_cast<float>(kPi / 3.0)),
        CalVector(0.0f, 0.0f, -1.0f));
    core->addCoreBone(grandChild);

    // addCoreBone canonicalizes the root; restore the intended bind reference,
    // mirroring the established baseline harness convention.
    core->coreBones[0]->relativeTransform.translation = CalVector(1.0f, 0.0f, 0.0f);
    return core;
}

typedef std::vector<std::vector<CalCoreSubmesh::Influence> > InfluenceTable;

InfluenceTable buildThreeBoneInfluences() {
    InfluenceTable table(4);
    table[0].push_back(CalCoreSubmesh::Influence(0, 0.5f, false));
    table[0].push_back(CalCoreSubmesh::Influence(1, 0.5f, true));
    table[1].push_back(CalCoreSubmesh::Influence(0, 0.25f, false));
    table[1].push_back(CalCoreSubmesh::Influence(1, 0.25f, false));
    table[1].push_back(CalCoreSubmesh::Influence(2, 0.5f, true));
    table[2].push_back(CalCoreSubmesh::Influence(0, 1.0f, true));
    table[3].push_back(CalCoreSubmesh::Influence(2, 1.0f, true));
    return table;
}

ISTD::shared_ptr<CalCoreSubmesh> buildFourVertexSubmesh(const InfluenceTable& table) {
    auto sub = ISTD::make_shared<CalCoreSubmesh>(4, false, 1);
    const CalPoint4 positions[4] = {
        CalPoint4(0.2f, 0.3f, 0.4f),
        CalPoint4(-0.1f, 0.2f, 0.7f),
        CalPoint4(0.5f, 0.5f, 0.5f),
        CalPoint4(0.1f, 0.1f, 0.1f)
    };
    for (size_t i = 0; i < 4; ++i) {
        CalCoreSubmesh::Vertex vertex;
        vertex.position = positions[i];
        vertex.normal = CalVector4(0.0f, 0.0f, 1.0f);
        sub->addVertex(vertex, 0, table[i]);
    }
    sub->addFace(CalCoreSubmesh::Face(0, 1, 2));
    return sub;
}

void runProductionSkinning(const CalSkeleton& skeleton, const CalSubmesh& sub, std::vector<CalVector4>& out) {
    out.resize(2 * sub.coreSubmesh->getVertexCount());
    CalPhysique::calculateVerticesAndNormals(
        cal3d::pointerFromVector(skeleton.boneTransforms),
        &sub,
        &out[0].x);
}

void setBoneLinear(CalSkeleton& skeleton, unsigned boneId, const double linear[3][3], const CalVector& translation) {
    BoneTransform& bt = skeleton.boneTransforms[boneId];
    bt.rowx.set((float)linear[0][0], (float)linear[0][1], (float)linear[0][2], translation.x);
    bt.rowy.set((float)linear[1][0], (float)linear[1][1], (float)linear[1][2], translation.y);
    bt.rowz.set((float)linear[2][0], (float)linear[2][1], (float)linear[2][2], translation.z);
}

void identityLinear(double out[3][3]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r][c] = (r == c) ? 1.0 : 0.0;
        }
    }
}

void diagonalLinear(double x, double y, double z, double out[3][3]) {
    identityLinear(out);
    out[0][0] = x;
    out[1][1] = y;
    out[2][2] = z;
}

// Single-bone submesh whose one vertex is fully weighted on bone 0, used by
// the crafted singular-palette observability cases.
ISTD::shared_ptr<CalCoreSubmesh> buildSingleVertexSubmesh() {
    auto sub = ISTD::make_shared<CalCoreSubmesh>(1, false, 0);
    CalCoreSubmesh::Vertex vertex;
    vertex.position = CalPoint4(0.0f, 0.0f, 0.0f);
    vertex.normal = CalVector4(0.0f, 0.0f, 1.0f);
    std::vector<CalCoreSubmesh::Influence> inf;
    inf.push_back(CalCoreSubmesh::Influence(0, 1.0f, true));
    sub->addVertex(vertex, 0, inf);
    return sub;
}

ISTD::shared_ptr<CalCoreSkeleton> buildOneBoneCoreSkeleton() {
    auto core = ISTD::make_shared<CalCoreSkeleton>();
    auto root = ISTD::make_shared<CalCoreBone>("root", -1);
    root->relativeTransform = cal3d::RotateTranslate(CalQuaternion(), CalVector(2.0f, 0.0f, 0.0f));
    root->inverseBindPoseTransform = cal3d::RotateTranslate(
        axisAngle(0, 1, 0, static_cast<float>(kPi / 5.0)),
        CalVector(-2.0f, 0.0f, 0.0f));
    core->addCoreBone(root);
    core->coreBones[0]->relativeTransform.translation = CalVector(2.0f, 0.0f, 0.0f);
    return core;
}

bool captureOne(
    const CalSkeleton& skeleton,
    const CalSubmesh& sub,
    const std::vector<Observation>& observations,
    double weight,
    Snapshot& snapshot)
{
    std::string error;
    return captureSculptPose(
        skeleton, sub,
        observations.empty() ? 0 : &observations[0],
        observations.size(),
        weight, snapshot, &error);
}

bool bakeOne(
    const ISTD::shared_ptr<CalCoreSubmesh>& coreSubmesh,
    const std::vector<const Snapshot*>& snapshotPtrs,
    const std::string& name,
    double limit,
    ISTD::shared_ptr<CalCoreMorphTarget>& target,
    BakeReport& report)
{
    std::string error;
    return bakeSculptMorphTarget(
        coreSubmesh,
        snapshotPtrs.empty() ? 0 : &snapshotPtrs[0],
        snapshotPtrs.size(),
        name, limit, target, report, &error);
}

} // namespace

// ===========================================================================
// Group: observability -- rank, minimum-norm and residual semantics, checked
// against the independent eigen-based reference.
// ===========================================================================

// One singular pose loses two directions; the minimum-norm solution must not
// invent components in the null space.
static int observability_rank_one_minimum_norm() {
    auto coreSkeleton = buildOneBoneCoreSkeleton();
    auto coreSubmesh = buildSingleVertexSubmesh();
    CalSkeleton skeleton(coreSkeleton);
    CalMixer mixer;
    mixer.updateSkeleton(&skeleton, {}, {});

    double linear[3][3];
    diagonalLinear(1.0, 0.0, 0.0, linear);
    setBoneLinear(skeleton, 0, linear, CalVector(0.3f, -0.2f, 0.1f));

    CalSubmesh sub(coreSubmesh);
    const std::vector<CalCoreSubmesh::Influence> inf(1, CalCoreSubmesh::Influence(0, 1.0f, true));
    double palette[3][3];
    referencePalette(skeleton, inf, palette);

    const double intendedB[3] = { 0.7, 0.3, 0.5 };
    std::vector<CalVector4> skinned;
    runProductionSkinning(skeleton, sub, skinned);

    // The capture stores float target minus double baseline; use the exact
    // stored delta on both sides of the cross-check.
    double b[3];
    actualTargetDelta(skinned[0], intendedB, b);
    std::vector<Observation> observations;
    observations.push_back(Observation(0, CalPoint4(
        (float)(skinned[0].x + intendedB[0]),
        (float)(skinned[0].y + intendedB[1]),
        (float)(skinned[0].z + intendedB[2]))));

    Snapshot snapshot;
    TASK_CHECK(captureOne(skeleton, sub, observations, 1.0, snapshot));

    std::vector<RefRow> rows(1);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            rows[0].a[r][c] = palette[r][c];
        }
        rows[0].b[r] = b[r];
    }
    rows[0].weight = 1.0;

    double refD[3];
    unsigned refRank = 0;
    double refSigmaMax = 0.0;
    referenceSolve(rows, refD, refRank, refSigmaMax);
    TASK_CHECK(refRank == 1);
    // The minimum-norm solution keeps exactly the solvable x component and
    // invents nothing in the null space.
    TASK_CHECK(closeDouble(refD[0], b[0], 1e-12));
    TASK_CHECK(refD[1] == 0.0);
    TASK_CHECK(refD[2] == 0.0);

    ISTD::shared_ptr<CalCoreMorphTarget> target;
    BakeReport report;
    TASK_CHECK(bakeOne(coreSubmesh, std::vector<const Snapshot*>(1, &snapshot),
        "sculpt.additive", 1.0, target, report));
    TASK_CHECK(target != 0);
    TASK_CHECK(report.vertexReports.size() == 1);
    TASK_CHECK(report.vertexReports[0].vertexId == 0);
    TASK_CHECK(report.vertexReports[0].effectiveRank == 1);
    TASK_CHECK(target->vertexOffsets.size() == 1);
    const CalPoint4& stored = target->vertexOffsets[0].position;
    TASK_CHECK(closeDouble(stored.x, 0.7f, 1e-6));
    TASK_CHECK(stored.y == 0.0f);
    TASK_CHECK(stored.z == 0.0f);
    const float storedFloat[3] = { stored.x, stored.y, stored.z };
    const double refResidual = referenceResidual(rows, storedFloat);
    TASK_CHECK(closeDouble(report.vertexReports[0].maxResidual, refResidual, 1e-10));
    TASK_CHECK(closeDouble(report.maxResidual, report.vertexReports[0].maxResidual, 1e-14));
    return 0;
}

// Several singular poses jointly restore full rank and recover a known d.
static int observability_multiple_poses_full_rank() {
    auto coreSkeleton = buildOneBoneCoreSkeleton();
    auto coreSubmesh = buildSingleVertexSubmesh();
    CalSkeleton skeleton(coreSkeleton);
    CalMixer mixer;
    mixer.updateSkeleton(&skeleton, {}, {});
    CalSubmesh sub(coreSubmesh);

    const std::vector<CalCoreSubmesh::Influence> inf(1, CalCoreSubmesh::Influence(0, 1.0f, true));
    const double knownD[3] = { 0.2, -0.3, 0.4 };

    double linearA[3][3];
    diagonalLinear(1.0, 0.0, 1.0, linearA); // rank 2: keeps x and z
    double linearB[3][3];
    diagonalLinear(0.0, 1.0, 1.0, linearB); // rank 2: keeps y and z

    std::vector<Snapshot> snapshots;
    std::vector<RefRow> rows;
    const double weights[2] = { 2.0, 0.5 };
    const double(*linears[2])[3] = { linearA, linearB };

    for (int pose = 0; pose < 2; ++pose) {
        setBoneLinear(skeleton, 0, linears[pose], CalVector(0.1f * pose, 0.0f, 0.0f));
        double palette[3][3];
        referencePalette(skeleton, inf, palette);

        std::vector<CalVector4> skinned;
        runProductionSkinning(skeleton, sub, skinned);

        double intendedB[3];
        for (int r = 0; r < 3; ++r) {
            intendedB[r] = 0.0;
            for (int c = 0; c < 3; ++c) {
                intendedB[r] += palette[r][c] * knownD[c];
            }
        }
        double b[3];
        actualTargetDelta(skinned[0], intendedB, b);
        std::vector<Observation> observations;
        observations.push_back(Observation(0, CalPoint4(
            (float)(skinned[0].x + intendedB[0]),
            (float)(skinned[0].y + intendedB[1]),
            (float)(skinned[0].z + intendedB[2]))));

        Snapshot snapshot;
        TASK_CHECK(captureOne(skeleton, sub, observations, weights[pose], snapshot));
        snapshots.push_back(std::move(snapshot));

        RefRow row;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                row.a[r][c] = palette[r][c];
            }
            row.b[r] = b[r];
        }
        row.weight = weights[pose];
        rows.push_back(row);
    }

    double refD[3];
    unsigned refRank = 0;
    double refSigmaMax = 0.0;
    referenceSolve(rows, refD, refRank, refSigmaMax);
    TASK_CHECK(refRank == 3);
    // The palettes and targets pass through float storage, so the recovered
    // increment matches the intended one at float scale, not machine epsilon.
    TASK_CHECK(closeVector3(refD, knownD, 1e-6));

    std::vector<const Snapshot*> ptrs;
    for (size_t i = 0; i < snapshots.size(); ++i) {
        ptrs.push_back(&snapshots[i]);
    }
    ISTD::shared_ptr<CalCoreMorphTarget> target;
    BakeReport report;
    TASK_CHECK(bakeOne(coreSubmesh, ptrs, "full.additive", 1e-4, target, report));
    TASK_CHECK(report.vertexReports[0].effectiveRank == 3);
    const CalPoint4& stored = target->vertexOffsets[0].position;
    const float storedFloat[3] = { stored.x, stored.y, stored.z };
    TASK_CHECK(closeVector3(storedFloat, knownD, 1e-6));
    TASK_CHECK(closeDouble(report.vertexReports[0].maxResidual,
        referenceResidual(rows, storedFloat), 1e-10));
    TASK_CHECK(report.vertexReports[0].maxResidual <= 1e-9);
    return 0;
}

// Stacked poses that remain rank deficient still get the minimum-norm d.
static int observability_still_rank_deficient_minimum_norm() {
    auto coreSkeleton = buildOneBoneCoreSkeleton();
    auto coreSubmesh = buildSingleVertexSubmesh();
    CalSkeleton skeleton(coreSkeleton);
    CalMixer mixer;
    mixer.updateSkeleton(&skeleton, {}, {});
    CalSubmesh sub(coreSubmesh);
    const std::vector<CalCoreSubmesh::Influence> inf(1, CalCoreSubmesh::Influence(0, 1.0f, true));

    const double knownD[3] = { 0.15, -0.25, 0.0 };
    double linearA[3][3];
    diagonalLinear(1.0, 0.0, 0.0, linearA);
    double linearB[3][3];
    diagonalLinear(0.5, 0.0, 0.0, linearB); // same one-dimensional column space

    std::vector<Snapshot> snapshots;
    std::vector<RefRow> rows;
    for (int pose = 0; pose < 2; ++pose) {
        setBoneLinear(skeleton, 0, pose == 0 ? linearA : linearB, CalVector(0.0f, pose, 0.0f));
        double palette[3][3];
        referencePalette(skeleton, inf, palette);
        std::vector<CalVector4> skinned;
        runProductionSkinning(skeleton, sub, skinned);

        double intendedB[3] = { 0.0, 0.0, 0.0 };
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                intendedB[r] += palette[r][c] * knownD[c];
            }
        }
        double b[3];
        actualTargetDelta(skinned[0], intendedB, b);
        std::vector<Observation> observations;
        observations.push_back(Observation(0, CalPoint4(
            (float)(skinned[0].x + intendedB[0]),
            (float)(skinned[0].y + intendedB[1]),
            (float)(skinned[0].z + intendedB[2]))));
        Snapshot snapshot;
        TASK_CHECK(captureOne(skeleton, sub, observations, 1.0, snapshot));
        snapshots.push_back(std::move(snapshot));

        RefRow row;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                row.a[r][c] = palette[r][c];
            }
            row.b[r] = b[r];
        }
        row.weight = 1.0;
        rows.push_back(row);
    }

    double refD[3];
    unsigned refRank = 0;
    double refSigmaMax = 0.0;
    referenceSolve(rows, refD, refRank, refSigmaMax);
    TASK_CHECK(refRank == 1);
    // Only the observed x direction can be recovered; the null-space
    // components stay exactly zero.  Float rounding applies to x.
    TASK_CHECK(closeDouble(refD[0], 0.15, 1e-6));
    TASK_CHECK(refD[1] == 0.0);
    TASK_CHECK(refD[2] == 0.0);

    std::vector<const Snapshot*> ptrs;
    for (size_t i = 0; i < snapshots.size(); ++i) {
        ptrs.push_back(&snapshots[i]);
    }
    ISTD::shared_ptr<CalCoreMorphTarget> target;
    BakeReport report;
    TASK_CHECK(bakeOne(coreSubmesh, ptrs, "deficient.additive", 1.0, target, report));
    TASK_CHECK(report.vertexReports[0].effectiveRank == 1);
    const CalPoint4& stored = target->vertexOffsets[0].position;
    TASK_CHECK(closeDouble(stored.x, 0.15f, 1e-6));
    TASK_CHECK(stored.y == 0.0f);
    TASK_CHECK(stored.z == 0.0f);
    return 0;
}

// Singular values straddling the 1e-8 * max(1, sigmaMax) threshold, including
// the exact boundary, must be counted zero when no greater than the cutoff.
static int observability_near_threshold_singular_values() {
    auto coreSkeleton = buildOneBoneCoreSkeleton();
    auto coreSubmesh = buildSingleVertexSubmesh();
    CalSkeleton skeleton(coreSkeleton);
    CalMixer mixer;
    mixer.updateSkeleton(&skeleton, {}, {});
    CalSubmesh sub(coreSubmesh);
    const std::vector<CalCoreSubmesh::Influence> inf(1, CalCoreSubmesh::Influence(0, 1.0f, true));

    // First case: sigma = (1, 1.5e-8, 5e-9); cutoff 1e-8, so rank 2.
    double linearNear[3][3];
    diagonalLinear(1.0, 1.5e-8, 5e-9, linearNear);
    setBoneLinear(skeleton, 0, linearNear, CalVector(0, 0, 0));
    double paletteNear[3][3];
    referencePalette(skeleton, inf, paletteNear);
    std::vector<CalVector4> skinned;
    runProductionSkinning(skeleton, sub, skinned);
    const double intendedNear[3] = { 1.0, 1.0, 1.0 };
    double bNear[3];
    actualTargetDelta(skinned[0], intendedNear, bNear);
    std::vector<Observation> observationsNear;
    observationsNear.push_back(Observation(0, CalPoint4(
        (float)(skinned[0].x + intendedNear[0]),
        (float)(skinned[0].y + intendedNear[1]),
        (float)(skinned[0].z + intendedNear[2]))));
    Snapshot snapshotNear;
    TASK_CHECK(captureOne(skeleton, sub, observationsNear, 1.0, snapshotNear));

    std::vector<RefRow> rowsNear(1);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            rowsNear[0].a[r][c] = paletteNear[r][c];
        }
    }
    rowsNear[0].b[0] = bNear[0];
    rowsNear[0].b[1] = bNear[1];
    rowsNear[0].b[2] = bNear[2];
    rowsNear[0].weight = 1.0;
    double refDNear[3];
    unsigned refRankNear = 0;
    double refSigmaNear = 0.0;
    referenceSolve(rowsNear, refDNear, refRankNear, refSigmaNear);
    TASK_CHECK(refRankNear == 2);

    ISTD::shared_ptr<CalCoreMorphTarget> targetNear;
    BakeReport reportNear;
    // The unfit null-space direction leaves a residual around 1.0; this case
    // exists to check the rank threshold, so allow it generously.
    TASK_CHECK(bakeOne(coreSubmesh, std::vector<const Snapshot*>(1, &snapshotNear),
        "near.additive", 2.0, targetNear, reportNear));
    TASK_CHECK(reportNear.vertexReports[0].effectiveRank == 2);

    // Boundary case: sigmaMax 2 -> cutoff 2e-8; sigma = 1e-8 equals neither
    // surviving value, both small singular values count as zero -> rank 1.
    double linearBoundary[3][3];
    diagonalLinear(2.0, 1e-8, 1e-8, linearBoundary);
    setBoneLinear(skeleton, 0, linearBoundary, CalVector(0, 0, 0));
    double paletteBoundary[3][3];
    referencePalette(skeleton, inf, paletteBoundary);
    runProductionSkinning(skeleton, sub, skinned);
    const double intendedBoundary[3] = { 1.0, 1.0, 1.0 };
    double bBoundary[3];
    actualTargetDelta(skinned[0], intendedBoundary, bBoundary);
    std::vector<Observation> observationsBoundary;
    observationsBoundary.push_back(Observation(0, CalPoint4(
        (float)(skinned[0].x + intendedBoundary[0]),
        (float)(skinned[0].y + intendedBoundary[1]),
        (float)(skinned[0].z + intendedBoundary[2]))));
    Snapshot snapshotBoundary;
    TASK_CHECK(captureOne(skeleton, sub, observationsBoundary, 1.0, snapshotBoundary));

    std::vector<RefRow> rowsBoundary(1);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            rowsBoundary[0].a[r][c] = paletteBoundary[r][c];
        }
    }
    rowsBoundary[0].b[0] = bBoundary[0];
    rowsBoundary[0].b[1] = bBoundary[1];
    rowsBoundary[0].b[2] = bBoundary[2];
    rowsBoundary[0].weight = 1.0;
    double refDBoundary[3];
    unsigned refRankBoundary = 0;
    double refSigmaBoundary = 0.0;
    referenceSolve(rowsBoundary, refDBoundary, refRankBoundary, refSigmaBoundary);
    TASK_CHECK(refRankBoundary == 1);

    ISTD::shared_ptr<CalCoreMorphTarget> targetBoundary;
    BakeReport reportBoundary;
    TASK_CHECK(bakeOne(coreSubmesh, std::vector<const Snapshot*>(1, &snapshotBoundary),
        "boundary.additive", 2.0, targetBoundary, reportBoundary));
    TASK_CHECK(reportBoundary.vertexReports[0].effectiveRank == 1);
    return 0;
}

// Mutually contradictory full-rank observations cannot be fit exactly; the
// residual must be reported from the stored float d and a tight limit must
// fail without returning a partial target.
static int observability_contradictory_observations() {
    auto coreSkeleton = buildOneBoneCoreSkeleton();
    auto coreSubmesh = buildSingleVertexSubmesh();
    CalSkeleton skeleton(coreSkeleton);
    CalMixer mixer;
    mixer.updateSkeleton(&skeleton, {}, {});
    CalSubmesh sub(coreSubmesh);
    const std::vector<CalCoreSubmesh::Influence> inf(1, CalCoreSubmesh::Influence(0, 1.0f, true));

    double identity[3][3];
    identityLinear(identity);
    double rotation[3][3] = {
        { 0.0, -1.0, 0.0 },
        { 1.0,  0.0, 0.0 },
        { 0.0,  0.0, 1.0 }
    };
    const double trueD[3] = { 0.3, 0.5, -0.2 };
    const double contradiction[3] = { 0.1, 0.0, 0.0 };

    std::vector<Snapshot> snapshots;
    std::vector<RefRow> rows;
    for (int pose = 0; pose < 2; ++pose) {
        setBoneLinear(skeleton, 0, pose == 0 ? identity : rotation, CalVector(0.0f, 0.0f, pose));
        double palette[3][3];
        referencePalette(skeleton, inf, palette);
        std::vector<CalVector4> skinned;
        runProductionSkinning(skeleton, sub, skinned);

        double intendedB[3] = { 0.0, 0.0, 0.0 };
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                intendedB[r] += palette[r][c] * trueD[c];
            }
            if (pose == 1) {
                intendedB[r] += contradiction[r];
            }
        }
        double b[3];
        actualTargetDelta(skinned[0], intendedB, b);
        std::vector<Observation> observations;
        observations.push_back(Observation(0, CalPoint4(
            (float)(skinned[0].x + intendedB[0]),
            (float)(skinned[0].y + intendedB[1]),
            (float)(skinned[0].z + intendedB[2]))));
        Snapshot snapshot;
        TASK_CHECK(captureOne(skeleton, sub, observations, 1.0, snapshot));
        snapshots.push_back(std::move(snapshot));

        RefRow row;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                row.a[r][c] = palette[r][c];
            }
            row.b[r] = b[r];
        }
        row.weight = 1.0;
        rows.push_back(row);
    }

    double refD[3];
    unsigned refRank = 0;
    double refSigmaMax = 0.0;
    referenceSolve(rows, refD, refRank, refSigmaMax);
    TASK_CHECK(refRank == 3);

    std::vector<const Snapshot*> ptrs;
    for (size_t i = 0; i < snapshots.size(); ++i) {
        ptrs.push_back(&snapshots[i]);
    }

    ISTD::shared_ptr<CalCoreMorphTarget> rejected;
    BakeReport failedReport;
    std::string error;
    const bool rejectedBake = bakeSculptMorphTarget(
        coreSubmesh, &ptrs[0], ptrs.size(), "conflict.additive", 0.0,
        rejected, failedReport, &error);
    TASK_CHECK(!rejectedBake);
    TASK_CHECK(rejected == 0);
    TASK_CHECK(!error.empty());
    TASK_CHECK(failedReport.vertexReports[0].effectiveRank == 3);
    TASK_CHECK(failedReport.maxResidual > 0.0);

    // Inputs survive the failed bake: the same snapshots bake successfully
    // with a permissive limit, and the reported residual matches the
    // independent reference.
    ISTD::shared_ptr<CalCoreMorphTarget> target;
    BakeReport report;
    TASK_CHECK(bakeOne(coreSubmesh, ptrs, "conflict.additive", 1.0, target, report));
    const CalPoint4& stored = target->vertexOffsets[0].position;
    const float storedFloat[3] = { stored.x, stored.y, stored.z };
    TASK_CHECK(closeVector3(storedFloat, refD, 1e-6));
    const double refResidual = referenceResidual(rows, storedFloat);
    TASK_CHECK(closeDouble(report.maxResidual, refResidual, 1e-10));
    TASK_CHECK(closeDouble(report.maxResidual, failedReport.maxResidual, 1e-10));
    return 0;
}

// ===========================================================================
// Group: capture_pipeline -- real Mixer/Skeleton, non-identity inverse bind,
// multi-bone weights, pre-existing morphs, runtime teardown, sparse unions.
// ===========================================================================

namespace {

struct PipelineRig {
    ISTD::shared_ptr<CalCoreSkeleton> coreSkeleton;
    ISTD::shared_ptr<CalCoreSubmesh> coreSubmesh;
    InfluenceTable influences;
    ISTD::unique_ptr<CalSkeleton> skeleton;
    ISTD::unique_ptr<CalMixer> mixer;
    ISTD::unique_ptr<CalSubmesh> runtimeSubmesh;
    ISTD::shared_ptr<CalAnimation> animation;
};

void rigPose(PipelineRig& rig, bool animated) {
    if (animated) {
        rig.mixer->addAnimation(rig.animation);
    } else {
        rig.mixer->removeAnimation(rig.animation);
    }
    rig.mixer->updateSkeleton(rig.skeleton.get(), {}, {});
}

// Builds sculpt targets from a known bind-space increment per vertex using
// the current production skinned baseline and an independently derived
// palette, then captures the pose.
bool captureKnownIncrement(
    PipelineRig& rig,
    const std::vector<size_t>& vertexIds,
    const double knownD[][3],
    double weight,
    Snapshot& snapshot)
{
    std::vector<CalVector4> skinned;
    runProductionSkinning(*rig.skeleton, *rig.runtimeSubmesh, skinned);

    std::vector<Observation> observations;
    for (size_t i = 0; i < vertexIds.size(); ++i) {
        const size_t vertexId = vertexIds[i];
        double palette[3][3];
        referencePalette(*rig.skeleton, rig.influences[vertexId], palette);
        double delta[3] = { 0.0, 0.0, 0.0 };
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                delta[r] += palette[r][c] * knownD[i][c];
            }
        }
        observations.push_back(Observation(vertexId, CalPoint4(
            (float)(skinned[2 * vertexId].x + delta[0]),
            (float)(skinned[2 * vertexId].y + delta[1]),
            (float)(skinned[2 * vertexId].z + delta[2]))));
    }
    return captureOne(*rig.skeleton, *rig.runtimeSubmesh, observations, weight, snapshot);
}

const size_t kObservedVertices[3] = { 0, 1, 2 };
const double kKnownIncrements[3][3] = {
    { 0.10, -0.05, 0.08 },
    { 0.02,  0.03, -0.04 },
    { -0.06, 0.07, 0.01 }
};

PipelineRig buildPipelineRig(bool withExistingMorph) {
    PipelineRig rig;
    rig.coreSkeleton = buildThreeBoneCoreSkeleton();
    rig.influences = buildThreeBoneInfluences();
    rig.coreSubmesh = buildFourVertexSubmesh(rig.influences);

    if (withExistingMorph) {
        CalCoreMorphTarget::VertexOffsetArray offsets;
        offsets.push_back(VertexOffset(0,
            CalPoint4(0.4f, 0.6f, 1.0f, 1.0f),
            CalVector4(0.0f, 0.0f, 0.0f, 0.0f)));
        rig.coreSubmesh->addMorphTarget(
            ISTD::make_shared<CalCoreMorphTarget>("known.additive", 4, offsets));
    }

    rig.skeleton = ISTD::unique_ptr<CalSkeleton>(new CalSkeleton(rig.coreSkeleton));
    rig.mixer = ISTD::unique_ptr<CalMixer>(new CalMixer());

    auto coreAnimation = ISTD::make_shared<CalCoreAnimation>();
    CalCoreTrack::KeyframeList keys;
    keys.push_back(CalCoreKeyframe(0, CalVector(0, 0, 0), CalQuaternion(0, 0, 1, 0)));
    coreAnimation->tracks.push_back(CalCoreTrack(1, keys));
    rig.animation = ISTD::make_shared<CalAnimation>(coreAnimation, 1, 0);

    rig.runtimeSubmesh = ISTD::unique_ptr<CalSubmesh>(new CalSubmesh(rig.coreSubmesh));
    if (withExistingMorph) {
        rig.runtimeSubmesh->setMorphTargetWeight("known.additive", 1.0f);
    }
    return rig;
}

} // namespace

// Full mixer-driven capture in two poses, bake, then reproduce the sculpted
// model-space positions through an ordinary weight-1 CalSubmesh, including
// alongside the pre-existing morph target.
static int capture_pipeline_end_to_end() {
    PipelineRig rig = buildPipelineRig(true);

    std::vector<Snapshot> snapshots;
    std::vector<CalPoint4> sculptedPose1(4);
    std::vector<CalPoint4> sculptedPose2(4);

    rigPose(rig, true);
    {
        Snapshot snapshot;
        TASK_CHECK(captureKnownIncrement(
            rig, std::vector<size_t>(kObservedVertices, kObservedVertices + 3),
            kKnownIncrements, 1.0, snapshot));
        std::vector<CalVector4> skinned;
        runProductionSkinning(*rig.skeleton, *rig.runtimeSubmesh, skinned);
        for (size_t v = 0; v < 4; ++v) {
            sculptedPose1[v] = CalPoint4(skinned[2 * v].x, skinned[2 * v].y, skinned[2 * v].z);
        }
        for (size_t i = 0; i < 3; ++i) {
            const size_t v = kObservedVertices[i];
            double palette[3][3];
            referencePalette(*rig.skeleton, rig.influences[v], palette);
            for (int r = 0; r < 3; ++r) {
                double delta = 0.0;
                for (int c = 0; c < 3; ++c) {
                    delta += palette[r][c] * kKnownIncrements[i][c];
                }
                (&sculptedPose1[v].x)[r] += (float)delta;
            }
        }
        snapshots.push_back(std::move(snapshot));
    }

    rigPose(rig, false);
    {
        Snapshot snapshot;
        TASK_CHECK(captureKnownIncrement(
            rig, std::vector<size_t>(kObservedVertices, kObservedVertices + 3),
            kKnownIncrements, 1.0, snapshot));
        std::vector<CalVector4> skinned;
        runProductionSkinning(*rig.skeleton, *rig.runtimeSubmesh, skinned);
        for (size_t v = 0; v < 4; ++v) {
            sculptedPose2[v] = CalPoint4(skinned[2 * v].x, skinned[2 * v].y, skinned[2 * v].z);
        }
        for (size_t i = 0; i < 3; ++i) {
            const size_t v = kObservedVertices[i];
            double palette[3][3];
            referencePalette(*rig.skeleton, rig.influences[v], palette);
            for (int r = 0; r < 3; ++r) {
                double delta = 0.0;
                for (int c = 0; c < 3; ++c) {
                    delta += palette[r][c] * kKnownIncrements[i][c];
                }
                (&sculptedPose2[v].x)[r] += (float)delta;
            }
        }
        snapshots.push_back(std::move(snapshot));
    }

    std::vector<const Snapshot*> ptrs;
    for (size_t i = 0; i < snapshots.size(); ++i) {
        ptrs.push_back(&snapshots[i]);
    }
    ISTD::shared_ptr<CalCoreMorphTarget> target;
    BakeReport report;
    TASK_CHECK(bakeOne(rig.coreSubmesh, ptrs, "sculpt.additive", 1e-4, target, report));
    TASK_CHECK(report.snapshotCount == 2);
    TASK_CHECK(report.vertexReports.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        TASK_CHECK(report.vertexReports[i].vertexId == kObservedVertices[i]); // ascending
        TASK_CHECK(report.vertexReports[i].effectiveRank == 3);
        TASK_CHECK(report.vertexReports[i].maxResidual <= 2e-4);
    }
    for (size_t i = 0; i < target->vertexOffsets.size(); ++i) {
        const CalVector4& normal = target->vertexOffsets[i].normal;
        TASK_CHECK(normal.x == 0.0f && normal.y == 0.0f && normal.z == 0.0f);
        TASK_CHECK(target->vertexOffsets[i].position.w == 1.0f);
    }

    // Attach explicitly; the bake never modified the core submesh itself.
    TASK_CHECK(rig.coreSubmesh->getMorphTargets().size() == 1);
    rig.coreSubmesh->addMorphTarget(target);
    TASK_CHECK(rig.coreSubmesh->getMorphTargets().size() == 2);

    // Reproduce pose 2 with both morphs active on a fresh runtime submesh.
    {
        CalSubmesh playback(rig.coreSubmesh);
        playback.setMorphTargetWeight("known.additive", 1.0f);
        playback.setMorphTargetWeight("sculpt.additive", 1.0f);
        std::vector<CalVector4> output;
        runProductionSkinning(*rig.skeleton, playback, output);
        for (size_t v = 0; v < 4; ++v) {
            for (int r = 0; r < 3; ++r) {
                TASK_CHECK(closeDouble((&output[2 * v].x)[r], (&sculptedPose2[v].x)[r], 2e-4));
            }
        }
    }

    // Reproduce pose 1 as well, again with the capture-time morph active.
    rigPose(rig, true);
    {
        CalSubmesh playback(rig.coreSubmesh);
        playback.setMorphTargetWeight("known.additive", 1.0f);
        playback.setMorphTargetWeight("sculpt.additive", 1.0f);
        std::vector<CalVector4> output;
        runProductionSkinning(*rig.skeleton, playback, output);
        for (size_t v = 0; v < 4; ++v) {
            for (int r = 0; r < 3; ++r) {
                TASK_CHECK(closeDouble((&output[2 * v].x)[r], (&sculptedPose1[v].x)[r], 2e-4));
            }
        }
    }
    return 0;
}

// After capture, retargeting, weight changes and runtime destruction must not
// touch the snapshots; they retain shared ownership of the core mesh.
static int capture_pipeline_runtime_teardown_independence() {
    ISTD::shared_ptr<CalCoreSubmesh> coreSubmesh;
    Snapshot snapshot;
    {
        PipelineRig rig = buildPipelineRig(false);
        rigPose(rig, true);
        coreSubmesh = rig.coreSubmesh;
        const std::vector<size_t> ids(1, static_cast<size_t>(2));
        const double d[1][3] = { { 0.03, -0.02, 0.04 } };
        TASK_CHECK(captureKnownIncrement(rig, ids, d, 1.5, snapshot));
        TASK_CHECK(!snapshot.empty());
        TASK_CHECK(snapshot.observationCount() == 1);
        TASK_CHECK(snapshot.observationWeight() == 1.5);
        TASK_CHECK(snapshot.coreSubmesh().get() == coreSubmesh.get());

        // Mutate runtime state while the snapshot is alive.
        rig.runtimeSubmesh->clearMorphTargetScales();
        rigPose(rig, false);
    }
    // rig (skeleton, mixer, runtime CalSubmesh) is destroyed here.
    TASK_CHECK(coreSubmesh.get() != 0); // snapshot keeps the core mesh alive
    TASK_CHECK(snapshot.coreSubmesh().get() == coreSubmesh.get());

    ISTD::shared_ptr<CalCoreMorphTarget> target;
    BakeReport report;
    TASK_CHECK(bakeOne(coreSubmesh, std::vector<const Snapshot*>(1, &snapshot),
        "teardown.additive", 1e-4, target, report));
    TASK_CHECK(target != 0);
    TASK_CHECK(report.vertexReports.size() == 1);
    return 0;
}

// Snapshots may cover different vertices; the target holds their sorted
// union, zero increments are omitted, and an empty bake yields an empty
// target.
static int capture_pipeline_sparse_union_and_empty_target() {
    PipelineRig rig = buildPipelineRig(false);
    rigPose(rig, true);

    const double d0[1][3] = { { 0.05, 0.0, 0.0 } };
    const double d3[1][3] = { { 0.0, -0.04, 0.02 } };
    const double zeroD[1][3] = { { 0.0, 0.0, 0.0 } };

    Snapshot snapshotA;
    TASK_CHECK(captureKnownIncrement(rig, std::vector<size_t>(1, 0), d0, 1.0, snapshotA));
    Snapshot snapshotB;
    TASK_CHECK(captureKnownIncrement(rig, std::vector<size_t>(1, 3), d3, 1.0, snapshotB));
    Snapshot snapshotZero;
    TASK_CHECK(captureKnownIncrement(rig, std::vector<size_t>(1, 2), zeroD, 1.0, snapshotZero));

    std::vector<const Snapshot*> ptrs;
    ptrs.push_back(&snapshotA);
    ptrs.push_back(&snapshotB);
    ptrs.push_back(&snapshotZero);
    ISTD::shared_ptr<CalCoreMorphTarget> target;
    BakeReport report;
    TASK_CHECK(bakeOne(rig.coreSubmesh, ptrs, "union.additive", 1e-4, target, report));
    TASK_CHECK(target->vertexOffsets.size() == 2); // strict-zero increment omitted
    TASK_CHECK(target->vertexOffsets[0].vertexId == 0);
    TASK_CHECK(target->vertexOffsets[1].vertexId == 3);
    TASK_CHECK(report.vertexReports.size() == 3); // all observed vertices reported
    TASK_CHECK(report.vertexReports[0].vertexId == 0);
    TASK_CHECK(report.vertexReports[1].vertexId == 2);
    TASK_CHECK(report.vertexReports[2].vertexId == 3);

    // Empty input is a valid empty target.
    ISTD::shared_ptr<CalCoreMorphTarget> emptyTarget;
    BakeReport emptyReport;
    TASK_CHECK(bakeOne(rig.coreSubmesh, std::vector<const Snapshot*>(),
        "empty.additive", 1.0, emptyTarget, emptyReport));
    TASK_CHECK(emptyTarget != 0);
    TASK_CHECK(emptyTarget->vertexOffsets.size() == 0);
    TASK_CHECK(emptyReport.vertexReports.empty());
    TASK_CHECK(emptyReport.maxResidual == 0.0);

    // Empty observation capture is likewise valid.
    Snapshot emptySnapshot;
    TASK_CHECK(captureOne(*rig.skeleton, *rig.runtimeSubmesh,
        std::vector<Observation>(), 1.0, emptySnapshot));
    TASK_CHECK(emptySnapshot.empty());
    return 0;
}

// Validation failures must be refused, must leave inputs and previously
// returned results intact, and must never partially populate a target.
static int capture_pipeline_validation_and_failure_contracts() {
    PipelineRig rig = buildPipelineRig(true);
    rigPose(rig, true);

    const std::vector<size_t> ids(1, static_cast<size_t>(1));
    const double d[1][3] = { { 0.02, 0.03, -0.04 } };
    Snapshot good;
    TASK_CHECK(captureKnownIncrement(rig, ids, d, 1.0, good));
    const size_t goodRecordCount = good.observationCount();

    std::string error;

    // Non-positive or non-finite observation weight.
    Snapshot rejected;
    std::vector<Observation> obs;
    obs.push_back(Observation(1, CalPoint4(0, 0, 0)));
    TASK_CHECK(!captureSculptPose(*rig.skeleton, *rig.runtimeSubmesh, &obs[0], 1, 0.0, rejected, &error));
    TASK_CHECK(!error.empty());
    TASK_CHECK(!captureSculptPose(*rig.skeleton, *rig.runtimeSubmesh, &obs[0], 1, -1.0, rejected, &error));
    TASK_CHECK(!captureSculptPose(*rig.skeleton, *rig.runtimeSubmesh, &obs[0], 1,
        std::numeric_limits<double>::quiet_NaN(), rejected, &error));

    // Non-finite target position.
    const float nanFloat = std::numeric_limits<float>::quiet_NaN();
    std::vector<Observation> nanObs;
    nanObs.push_back(Observation(1, CalPoint4(nanFloat, 0.0f, 0.0f)));
    TASK_CHECK(!captureSculptPose(*rig.skeleton, *rig.runtimeSubmesh, &nanObs[0], 1, 1.0, rejected, &error));

    // Out-of-range and duplicated vertex ids.
    std::vector<Observation> badRange;
    badRange.push_back(Observation(99, CalPoint4(0, 0, 0)));
    TASK_CHECK(!captureSculptPose(*rig.skeleton, *rig.runtimeSubmesh, &badRange[0], 1, 1.0, rejected, &error));
    std::vector<Observation> duplicated;
    duplicated.push_back(Observation(1, CalPoint4(0, 0, 0)));
    duplicated.push_back(Observation(1, CalPoint4(1, 1, 1)));
    TASK_CHECK(!captureSculptPose(*rig.skeleton, *rig.runtimeSubmesh, &duplicated[0], 2, 1.0, rejected, &error));

    // A failed capture into the good snapshot leaves it untouched.
    TASK_CHECK(!captureSculptPose(*rig.skeleton, *rig.runtimeSubmesh, &obs[0], 1, 0.0, good, &error));
    TASK_CHECK(good.observationCount() == goodRecordCount);

    // Bad influence data: weights that do not sum to one, negative weight,
    // and a bone id outside the skeleton.  The library stores weights as
    // given, so nothing gets renormalized behind our backs.
    {
        auto badSubmeshCore = ISTD::make_shared<CalCoreSubmesh>(1, false, 0);
        CalCoreSubmesh::Vertex vertex;
        vertex.position = CalPoint4(0, 0, 0);
        vertex.normal = CalVector4(0, 0, 1);

        std::vector<CalCoreSubmesh::Influence> wrongSum;
        wrongSum.push_back(CalCoreSubmesh::Influence(0, 0.6f, false));
        wrongSum.push_back(CalCoreSubmesh::Influence(1, 0.6f, true));
        badSubmeshCore->addVertex(vertex, 0, wrongSum);
        CalSubmesh badSubmesh(badSubmeshCore);
        TASK_CHECK(!captureSculptPose(*rig.skeleton, badSubmesh, &obs[0], 1, 1.0, rejected, &error));

        auto negativeCore = ISTD::make_shared<CalCoreSubmesh>(1, false, 0);
        std::vector<CalCoreSubmesh::Influence> negative;
        negative.push_back(CalCoreSubmesh::Influence(0, 1.2f, false));
        negative.push_back(CalCoreSubmesh::Influence(1, -0.2f, true));
        negativeCore->addVertex(vertex, 0, negative);
        CalSubmesh negativeSubmesh(negativeCore);
        TASK_CHECK(!captureSculptPose(*rig.skeleton, negativeSubmesh, &obs[0], 1, 1.0, rejected, &error));

        auto badBoneCore = ISTD::make_shared<CalCoreSubmesh>(1, false, 0);
        std::vector<CalCoreSubmesh::Influence> badBone;
        badBone.push_back(CalCoreSubmesh::Influence(99, 1.0f, true));
        badBoneCore->addVertex(vertex, 0, badBone);
        CalSubmesh badBoneSubmesh(badBoneCore);
        TASK_CHECK(!captureSculptPose(*rig.skeleton, badBoneSubmesh, &obs[0], 1, 1.0, rejected, &error));
    }

    // Bake-time argument validation.
    ISTD::shared_ptr<CalCoreMorphTarget> target;
    BakeReport report;
    std::vector<const Snapshot*> goodPtrs(1, &good);
    TASK_CHECK(!bakeSculptMorphTarget(rig.coreSubmesh, &goodPtrs[0], 1,
        "sculpt.wrongSuffix", 1.0, target, report, &error));
    TASK_CHECK(target == 0);
    TASK_CHECK(!bakeSculptMorphTarget(rig.coreSubmesh, &goodPtrs[0], 1,
        "sculpt.additive", -1.0, target, report, &error));
    TASK_CHECK(target == 0);
    TASK_CHECK(!bakeSculptMorphTarget(rig.coreSubmesh, &goodPtrs[0], 1,
        "sculpt.additive", std::numeric_limits<double>::quiet_NaN(), target, report, &error));
    TASK_CHECK(target == 0);

    // Core mesh identity must match across snapshots.
    auto foreignCore = buildFourVertexSubmesh(rig.influences);
    TASK_CHECK(!bakeSculptMorphTarget(foreignCore, &goodPtrs[0], 1,
        "sculpt.additive", 1.0, target, report, &error));
    TASK_CHECK(target == 0);

    // Mutating a core vertex between capture and bake breaks the invariant.
    // Use a dedicated rig so the good snapshots above stay usable; the public
    // replaceMeshWithMorphTarget() path is the supported way to move a core
    // vertex in place.
    {
        PipelineRig mutatedRig = buildPipelineRig(false);
        rigPose(mutatedRig, true);
        Snapshot mutatedSnapshot;
        TASK_CHECK(captureKnownIncrement(mutatedRig, ids, d, 1.0, mutatedSnapshot));

        CalCoreMorphTarget::VertexOffsetArray perturbOffsets;
        perturbOffsets.push_back(VertexOffset(1,
            CalPoint4(9.0f, 9.0f, 9.0f, 1.0f),
            CalVector4(0.0f, 0.0f, 0.0f, 0.0f)));
        mutatedRig.coreSubmesh->addMorphTarget(
            ISTD::make_shared<CalCoreMorphTarget>("perturb.additive", 4, perturbOffsets));
        mutatedRig.coreSubmesh->replaceMeshWithMorphTarget("perturb.additive");

        ISTD::shared_ptr<CalCoreMorphTarget> mutatedTarget;
        BakeReport mutatedReport;
        TASK_CHECK(!bakeOne(mutatedRig.coreSubmesh,
            std::vector<const Snapshot*>(1, &mutatedSnapshot),
            "sculpt.additive", 1.0, mutatedTarget, mutatedReport));
        TASK_CHECK(mutatedTarget == 0);
    }

    // After restoring the mesh, the unchanged snapshots still bake, and the
    // previously returned target remains functional.
    TASK_CHECK(bakeOne(rig.coreSubmesh, goodPtrs, "sculpt.additive", 1e-4, target, report));
    TASK_CHECK(target != 0);
    ISTD::shared_ptr<CalCoreMorphTarget> priorTarget = target;
    TASK_CHECK(priorTarget->vertexOffsets.size() >= 1);
    TASK_CHECK(!bakeSculptMorphTarget(foreignCore, &goodPtrs[0], 1,
        "sculpt.additive", 1.0, target, report, &error));
    TASK_CHECK(priorTarget->vertexOffsets.size() >= 1);
    return 0;
}

// ===========================================================================
// Group: assets -- CMF save/load round trip keeps the new target playable
// through the unchanged production skinning path.
// ===========================================================================

static int assets_cmf_roundtrip_and_cpu_playback() {
    PipelineRig rig = buildPipelineRig(true);
    rigPose(rig, true);

    const std::vector<size_t> ids(kObservedVertices, kObservedVertices + 3);
    Snapshot snapshot;
    TASK_CHECK(captureKnownIncrement(rig, ids, kKnownIncrements, 1.0, snapshot));

    ISTD::shared_ptr<CalCoreMorphTarget> target;
    BakeReport report;
    TASK_CHECK(bakeOne(rig.coreSubmesh, std::vector<const Snapshot*>(1, &snapshot),
        "sculpt.additive", 1e-4, target, report));
    TASK_CHECK(target->vertexOffsets.size() != 0);
    rig.coreSubmesh->addMorphTarget(target);

    // Expected model-space positions with both morphs at weight 1.
    CalSubmesh expectedSubmesh(rig.coreSubmesh);
    expectedSubmesh.setMorphTargetWeight("known.additive", 1.0f);
    expectedSubmesh.setMorphTargetWeight("sculpt.additive", 1.0f);
    std::vector<CalVector4> expected;
    runProductionSkinning(*rig.skeleton, expectedSubmesh, expected);

    auto mesh = ISTD::make_shared<CalCoreMesh>();
    mesh->submeshes.push_back(rig.coreSubmesh);
    const std::string bytes = CalSaver::saveCoreMeshToBuffer(mesh);
    TASK_CHECK(!bytes.empty());

    CalBufferSource buffer(bytes.data(), bytes.size());
    ISTD::shared_ptr<CalCoreMesh> loadedMesh = CalLoader::loadCoreMesh(buffer);
    TASK_CHECK(loadedMesh != 0);
    TASK_CHECK(loadedMesh->submeshes.size() == 1);
    const CalCoreSubmesh::MorphTargetArray& loadedMorphs = loadedMesh->submeshes[0]->getMorphTargets();
    TASK_CHECK(loadedMorphs.size() == 2);
    bool foundSculpt = false;
    for (size_t i = 0; i < loadedMorphs.size(); ++i) {
        if (loadedMorphs[i]->name == "sculpt.additive") {
            foundSculpt = true;
            TASK_CHECK(loadedMorphs[i]->vertexOffsets.size() == target->vertexOffsets.size());
        }
    }
    TASK_CHECK(foundSculpt);

    // The loaded target still drives the ordinary CPU skinning output.
    CalSubmesh loadedSubmesh(loadedMesh->submeshes[0]);
    loadedSubmesh.setMorphTargetWeight("known.additive", 1.0f);
    loadedSubmesh.setMorphTargetWeight("sculpt.additive", 1.0f);
    std::vector<CalVector4> actual;
    runProductionSkinning(*rig.skeleton, loadedSubmesh, actual);
    TASK_CHECK(actual.size() == expected.size());
    for (size_t v = 0; v < rig.coreSubmesh->getVertexCount(); ++v) {
        for (int r = 0; r < 3; ++r) {
            TASK_CHECK(closeDouble((&actual[2 * v].x)[r], (&expected[2 * v].x)[r], 2e-4));
        }
    }

    // The untouched baseline path still produces identical output for the
    // pre-existing morph alone.
    CalSubmesh baselineOnly(loadedMesh->submeshes[0]);
    baselineOnly.setMorphTargetWeight("known.additive", 1.0f);
    CalSubmesh baselineExpected(rig.coreSubmesh);
    baselineExpected.setMorphTargetWeight("known.additive", 1.0f);
    std::vector<CalVector4> baselineActual, baselineWant;
    runProductionSkinning(*rig.skeleton, baselineOnly, baselineActual);
    runProductionSkinning(*rig.skeleton, baselineExpected, baselineWant);
    for (size_t i = 0; i < baselineActual.size(); ++i) {
        TASK_CHECK(closeDouble(baselineActual[i].x, baselineWant[i].x, 1e-6));
        TASK_CHECK(closeDouble(baselineActual[i].y, baselineWant[i].y, 1e-6));
        TASK_CHECK(closeDouble(baselineActual[i].z, baselineWant[i].z, 1e-6));
    }
    return 0;
}

// ===========================================================================
// Registration.
// ===========================================================================

const struct task_case task_cases[] = {
    { "observability", "rank_one_minimum_norm", observability_rank_one_minimum_norm },
    { "observability", "multiple_poses_full_rank", observability_multiple_poses_full_rank },
    { "observability", "still_rank_deficient_minimum_norm", observability_still_rank_deficient_minimum_norm },
    { "observability", "near_threshold_singular_values", observability_near_threshold_singular_values },
    { "observability", "contradictory_observations", observability_contradictory_observations },

    { "capture_pipeline", "end_to_end", capture_pipeline_end_to_end },
    { "capture_pipeline", "runtime_teardown_independence", capture_pipeline_runtime_teardown_independence },
    { "capture_pipeline", "sparse_union_and_empty_target", capture_pipeline_sparse_union_and_empty_target },
    { "capture_pipeline", "validation_and_failure_contracts", capture_pipeline_validation_and_failure_contracts },

    { "assets", "cmf_roundtrip_and_cpu_playback", assets_cmf_roundtrip_and_cpu_playback }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
