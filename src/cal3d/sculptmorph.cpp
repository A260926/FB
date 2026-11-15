//****************************************************************************//
// sculptmorph.cpp                                                            //
// Bake sculpted vertex positions, captured on posed runtime characters,     //
// into a single bind-space additive Cal3D morph target.                     //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

#include "cal3d/sculptmorph.h"
#include "cal3d/bonetransform.h"
#include "cal3d/coremorphtarget.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/memory.h"
#include "cal3d/physique.h"
#include "cal3d/skeleton.h"
#include "cal3d/submesh.h"

namespace CalSculptMorph {

namespace {

const double kSingularZeroScale = 1e-8;
const double kWeightSumTolerance = 1e-5;

bool isFinite(double v) {
    return std::isfinite(v);
}

bool isFinite(float v) {
    return std::isfinite(static_cast<double>(v));
}

bool setError(std::string* errorMessage, const std::string& message) {
    if (errorMessage) {
        *errorMessage = message;
    }
    return false;
}

// Walks the flat influence array, which is organized as one run per vertex,
// each run terminated by its lastInfluenceForThisVertex entry (the exact
// convention consumed by CalPhysique).  Returns false if the runs do not
// line up with the vertex count.
bool collectInfluenceRuns(
    const CalCoreSubmesh& coreSubmesh,
    std::vector<std::pair<size_t, size_t> >& runs)
{
    const CalCoreSubmesh::InfluenceVector& influences = coreSubmesh.getInfluences();
    const size_t vertexCount = coreSubmesh.getVertexCount();
    runs.clear();
    runs.reserve(vertexCount);

    size_t runStart = 0;
    for (size_t i = 0; i < influences.size(); ++i) {
        if (influences[i].lastInfluenceForThisVertex) {
            runs.push_back(std::make_pair(runStart, i + 1));
            runStart = i + 1;
        }
    }
    return runs.size() == vertexCount && runStart == influences.size();
}

// Validates every influence run exactly as production skinning interprets
// it: finite non-negative weights summing to 1 per vertex and bone ids in
// range of the skeleton.  Weights are never renormalized.
bool validateInfluencesAgainstSkeleton(
    const CalCoreSubmesh& coreSubmesh,
    size_t skeletonBoneCount,
    const std::vector<std::pair<size_t, size_t> >& runs,
    std::string* errorMessage)
{
    const CalCoreSubmesh::InfluenceVector& influences = coreSubmesh.getInfluences();
    for (size_t vertexId = 0; vertexId < runs.size(); ++vertexId) {
        float sum = 0.0f;
        for (size_t i = runs[vertexId].first; i < runs[vertexId].second; ++i) {
            const CalCoreSubmesh::Influence& influence = influences[i];
            if (!isFinite(influence.weight) || influence.weight < 0.0f) {
                return setError(errorMessage, "capture: influence weight must be finite and non-negative");
            }
            if (influence.boneId >= skeletonBoneCount) {
                return setError(errorMessage, "capture: influence bone id is out of range of the skeleton");
            }
            sum += influence.weight;
        }
        if (std::fabs(static_cast<double>(sum) - 1.0) > kWeightSumTolerance) {
            return setError(errorMessage, "capture: per-vertex influence weights must sum to 1");
        }
    }
    return true;
}

// Extracts the 3x3 linear part of a palette built by the shared
// CalPhysique palette routine, so the bake cannot diverge from production
// skinning on influence ordering or inverse-bind handling.
void paletteLinearPart(const BoneTransform& palette, double out[3][3]) {
    out[0][0] = palette.rowx.x;
    out[0][1] = palette.rowx.y;
    out[0][2] = palette.rowx.z;
    out[1][0] = palette.rowy.x;
    out[1][1] = palette.rowy.y;
    out[1][2] = palette.rowy.z;
    out[2][0] = palette.rowz.x;
    out[2][1] = palette.rowz.y;
    out[2][2] = palette.rowz.z;
}

bool matrix3Finite(const double m[3][3]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (!isFinite(m[r][c])) {
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// One-sided Jacobi SVD for an m x 3 matrix M.
//
// Repeated orthogonal column rotations turn M into M * V whose columns are
// mutually orthogonal; column j then has length sigma_j and direction u_j.
// Only 3 columns exist, so this is a small fixed amount of work and needs no
// external linear algebra package.  All arithmetic is double precision.
// ---------------------------------------------------------------------------
struct JacobiSVD3 {
    // m rows x 3 columns, row-major.
    std::vector<double> b;
    size_t rows;
    double v[3][3];
    double sigma[3];
    unsigned rank;

    JacobiSVD3()
        : rows(0)
        , rank(0) {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                v[i][j] = (i == j) ? 1.0 : 0.0;
            }
            sigma[i] = 0.0;
        }
    }

    void decompose(const std::vector<double>& matrix, size_t rowCount) {
        b = matrix;
        rows = rowCount;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                v[i][j] = (i == j) ? 1.0 : 0.0;
            }
        }

        const int maxSweeps = 64;
        for (int sweep = 0; sweep < maxSweeps; ++sweep) {
            bool converged = true;
            for (int p = 0; p < 3; ++p) {
                for (int q = p + 1; q < 3; ++q) {
                    double alpha = 0.0;
                    double beta = 0.0;
                    double gamma = 0.0;
                    for (size_t r = 0; r < rows; ++r) {
                        const double bp = b[r * 3 + p];
                        const double bq = b[r * 3 + q];
                        alpha += bp * bp;
                        beta += bq * bq;
                        gamma += bp * bq;
                    }
                    if (gamma == 0.0 || std::fabs(gamma) <= 1e-14 * std::sqrt(alpha * beta)) {
                        continue;
                    }
                    converged = false;

                    const double zeta = (beta - alpha) / (2.0 * gamma);
                    const double t = (zeta >= 0.0 ? 1.0 : -1.0)
                        / (std::fabs(zeta) + std::sqrt(1.0 + zeta * zeta));
                    const double c = 1.0 / std::sqrt(1.0 + t * t);
                    const double s = t * c;

                    for (size_t r = 0; r < rows; ++r) {
                        const double bp = b[r * 3 + p];
                        const double bq = b[r * 3 + q];
                        b[r * 3 + p] = c * bp - s * bq;
                        b[r * 3 + q] = s * bp + c * bq;
                    }
                    for (int r = 0; r < 3; ++r) {
                        const double vp = v[r][p];
                        const double vq = v[r][q];
                        v[r][p] = c * vp - s * vq;
                        v[r][q] = s * vp + c * vq;
                    }
                }
            }
            if (converged) {
                break;
            }
        }

        for (int j = 0; j < 3; ++j) {
            double normSquared = 0.0;
            for (size_t r = 0; r < rows; ++r) {
                const double value = b[r * 3 + j];
                normSquared += value * value;
            }
            sigma[j] = std::sqrt(normSquared);
        }

        // Jacobi makes no ordering guarantee; sort singular values descending.
        int order[3] = { 0, 1, 2 };
        if (sigma[order[0]] < sigma[order[1]]) {
            std::swap(order[0], order[1]);
        }
        if (sigma[order[1]] < sigma[order[2]]) {
            std::swap(order[1], order[2]);
        }
        if (sigma[order[0]] < sigma[order[1]]) {
            std::swap(order[0], order[1]);
        }
        const double savedSigma[3] = { sigma[0], sigma[1], sigma[2] };
        double savedV[3][3];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                savedV[r][c] = v[r][c];
            }
        }
        for (int j = 0; j < 3; ++j) {
            sigma[j] = savedSigma[order[j]];
            for (int r = 0; r < 3; ++r) {
                v[r][j] = savedV[r][order[j]];
            }
        }
        // Reorder the working columns to match the sorted singular values.
        for (size_t r = 0; r < rows; ++r) {
            double row[3];
            for (int j = 0; j < 3; ++j) {
                row[j] = b[r * 3 + order[j]];
            }
            for (int j = 0; j < 3; ++j) {
                b[r * 3 + j] = row[j];
            }
        }

        const double threshold = kSingularZeroScale * std::max(1.0, sigma[0]);
        rank = 0;
        for (int j = 0; j < 3; ++j) {
            if (sigma[j] > threshold) {
                ++rank;
            }
        }
    }

    // Minimum-norm least-squares solution of M * d = rhs for the decomposed
    // matrix.  Zero singular values are inverted as zero; no regularization
    // term is introduced.
    void solve(const std::vector<double>& rhs, double out[3]) const {
        for (int i = 0; i < 3; ++i) {
            out[i] = 0.0;
        }
        const double threshold = kSingularZeroScale * std::max(1.0, sigma[0]);
        for (int j = 0; j < 3; ++j) {
            if (sigma[j] <= threshold) {
                continue;
            }
            double utRhs = 0.0;
            for (size_t r = 0; r < rows; ++r) {
                utRhs += b[r * 3 + j] * rhs[r];
            }
            const double coefficient = utRhs / (sigma[j] * sigma[j]);
            for (int i = 0; i < 3; ++i) {
                out[i] += coefficient * v[i][j];
            }
        }
    }
};

// One observed row of one vertex: the snapshot's palette, target delta and
// observation weight.
struct VertexRow {
    double a[3][3];
    double b[3];
    double weight;
};

bool rowFinite(const VertexRow& row) {
    if (!isFinite(row.weight) || row.weight <= 0.0) {
        return false;
    }
    for (int r = 0; r < 3; ++r) {
        if (!isFinite(row.b[r])) {
            return false;
        }
        for (int c = 0; c < 3; ++c) {
            if (!isFinite(row.a[r][c])) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

Snapshot::Record::Record()
    : vertexId(0) {
    for (int i = 0; i < 3; ++i) {
        target[i] = 0.0;
        skinnedBase[i] = 0.0;
        corePosition[i] = 0.0;
        coreNormal[i] = 0.0;
        for (int j = 0; j < 3; ++j) {
            palette[i][j] = 0.0;
        }
    }
}

Snapshot::Snapshot()
    : m_vertexCount(0)
    , m_faceCount(0)
    , m_weight(0.0)
{}

Snapshot::~Snapshot()
{}

Snapshot::Snapshot(Snapshot&& other)
    : m_coreSubmesh(std::move(other.m_coreSubmesh))
    , m_vertexCount(other.m_vertexCount)
    , m_faceCount(other.m_faceCount)
    , m_weight(other.m_weight)
    , m_records(std::move(other.m_records))
{}

Snapshot& Snapshot::operator=(Snapshot&& other) {
    if (this != &other) {
        m_coreSubmesh = std::move(other.m_coreSubmesh);
        m_vertexCount = other.m_vertexCount;
        m_faceCount = other.m_faceCount;
        m_weight = other.m_weight;
        m_records = std::move(other.m_records);
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

bool captureSculptPose(
    const CalSkeleton& skeleton,
    const CalSubmesh& submesh,
    const Observation* observations,
    size_t observationCount,
    double observationWeight,
    Snapshot& outSnapshot,
    std::string* errorMessage)
{
    if (!isFinite(observationWeight) || observationWeight <= 0.0) {
        return setError(errorMessage, "capture: observation weight must be a positive finite number");
    }
    if (observationCount > 0 && observations == 0) {
        return setError(errorMessage, "capture: observation array is null but non-empty");
    }

    const ISTD::shared_ptr<CalCoreSubmesh>& coreSubmesh = submesh.coreSubmesh;
    if (!coreSubmesh) {
        return setError(errorMessage, "capture: submesh has no core submesh");
    }

    const size_t vertexCount = coreSubmesh->getVertexCount();
    for (size_t i = 0; i < observationCount; ++i) {
        if (observations[i].vertexId >= vertexCount) {
            return setError(errorMessage, "capture: observation vertex id is out of range");
        }
        if (!isFinite(observations[i].targetPosition.x)
            || !isFinite(observations[i].targetPosition.y)
            || !isFinite(observations[i].targetPosition.z)) {
            return setError(errorMessage, "capture: target position must be finite");
        }
    }
    {
        std::set<size_t> seen;
        for (size_t i = 0; i < observationCount; ++i) {
            if (!seen.insert(observations[i].vertexId).second) {
                return setError(errorMessage, "capture: duplicate vertex id within one observation set");
            }
        }
    }

    std::vector<std::pair<size_t, size_t> > runs;
    if (!collectInfluenceRuns(*coreSubmesh, runs)) {
        return setError(errorMessage, "capture: influence runs do not match the vertex count");
    }
    if (!validateInfluencesAgainstSkeleton(*coreSubmesh, skeleton.boneTransforms.size(), runs, errorMessage)) {
        return false;
    }

    // Run the production CPU skinning path once.  Its output already includes
    // every morph target currently weighted on the CalSubmesh, so the
    // baseline matches what the game displays this frame.
    cal3d::SSEArray<CalVector4> skinned(2 * vertexCount);
    CalPhysique::calculateVerticesAndNormals(
        cal3d::pointerFromVector(skeleton.boneTransforms),
        &submesh,
        &skinned[0].x);

    const CalCoreSubmesh::InfluenceVector& influences = coreSubmesh->getInfluences();
    const CalCoreSubmesh::VectorVertex& coreVertices = coreSubmesh->getVectorVertex();

    Snapshot result;
    result.m_coreSubmesh = coreSubmesh; // shared ownership keeps the core mesh alive
    result.m_vertexCount = vertexCount;
    result.m_faceCount = coreSubmesh->getFaceCount();
    result.m_weight = observationWeight;
    result.m_records.reserve(observationCount);

    for (size_t i = 0; i < observationCount; ++i) {
        const size_t vertexId = observations[i].vertexId;

        Snapshot::Record record;
        record.vertexId = vertexId;
        record.target[0] = observations[i].targetPosition.x;
        record.target[1] = observations[i].targetPosition.y;
        record.target[2] = observations[i].targetPosition.z;

        const CalVector4& skinnedPosition = skinned[2 * vertexId];
        record.skinnedBase[0] = skinnedPosition.x;
        record.skinnedBase[1] = skinnedPosition.y;
        record.skinnedBase[2] = skinnedPosition.z;

        // Same palette interpretation and weighted affine blend as the
        // scalar CalPhysique path, including its handling of the final
        // influence and the inverse-bind pose baked into each BoneTransform.
        const BoneTransform palette = CalPhysique::calculateVertexPalette(
            cal3d::pointerFromVector(skeleton.boneTransforms),
            &influences[runs[vertexId].first],
            runs[vertexId].second - runs[vertexId].first);
        paletteLinearPart(palette, record.palette);
        if (!matrix3Finite(record.palette)) {
            return setError(errorMessage, "capture: blended palette contains non-finite entries");
        }

        for (size_t k = runs[vertexId].first; k < runs[vertexId].second; ++k) {
            record.influences.push_back(std::make_pair(influences[k].boneId, influences[k].weight));
        }

        const CalCoreSubmesh::Vertex& coreVertex = coreVertices[vertexId];
        record.corePosition[0] = coreVertex.position.x;
        record.corePosition[1] = coreVertex.position.y;
        record.corePosition[2] = coreVertex.position.z;
        record.coreNormal[0] = coreVertex.normal.x;
        record.coreNormal[1] = coreVertex.normal.y;
        record.coreNormal[2] = coreVertex.normal.z;

        result.m_records.push_back(std::move(record));
    }

    outSnapshot = std::move(result);
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Bake
// ---------------------------------------------------------------------------

// Re-checks the capture-to-bake invariant: core vertices, normals, influence
// bone ids/weights and topology (vertex and face counts) are unchanged.
bool Snapshot::verifyForBake(const Snapshot& snapshot, std::string* errorMessage) {
    const ISTD::shared_ptr<CalCoreSubmesh>& coreSubmesh = snapshot.m_coreSubmesh;
    if (!coreSubmesh) {
        return setError(errorMessage, "bake: snapshot holds no core submesh");
    }
    if (!isFinite(snapshot.m_weight) || snapshot.m_weight <= 0.0) {
        return setError(errorMessage, "bake: snapshot weight must be a positive finite number");
    }
    if (coreSubmesh->getVertexCount() != snapshot.m_vertexCount
        || coreSubmesh->getFaceCount() != snapshot.m_faceCount) {
        return setError(errorMessage, "bake: core mesh topology changed between capture and bake");
    }

    std::vector<std::pair<size_t, size_t> > runs;
    if (!collectInfluenceRuns(*coreSubmesh, runs)) {
        return setError(errorMessage, "bake: influence runs do not match the vertex count");
    }
    const CalCoreSubmesh::InfluenceVector& influences = coreSubmesh->getInfluences();
    const CalCoreSubmesh::VectorVertex& coreVertices = coreSubmesh->getVectorVertex();

    std::set<size_t> seen;
    for (size_t i = 0; i < snapshot.m_records.size(); ++i) {
        const Snapshot::Record& record = snapshot.m_records[i];
        if (record.vertexId >= coreSubmesh->getVertexCount()) {
            return setError(errorMessage, "bake: snapshot vertex id out of range");
        }
        if (!seen.insert(record.vertexId).second) {
            return setError(errorMessage, "bake: duplicate vertex id inside one snapshot");
        }
        const CalCoreSubmesh::Vertex& coreVertex = coreVertices[record.vertexId];
        if (coreVertex.position.x != record.corePosition[0]
            || coreVertex.position.y != record.corePosition[1]
            || coreVertex.position.z != record.corePosition[2]
            || coreVertex.normal.x != record.coreNormal[0]
            || coreVertex.normal.y != record.coreNormal[1]
            || coreVertex.normal.z != record.coreNormal[2]) {
            return setError(errorMessage, "bake: core vertex changed between capture and bake");
        }
        const std::pair<size_t, size_t>& run = runs[record.vertexId];
        if (record.influences.size() != run.second - run.first) {
            return setError(errorMessage, "bake: skinning weights changed between capture and bake");
        }
        for (size_t k = 0; k < record.influences.size(); ++k) {
            const CalCoreSubmesh::Influence& influence = influences[run.first + k];
            if (influence.boneId != record.influences[k].first
                || influence.weight != record.influences[k].second) {
                return setError(errorMessage, "bake: skinning weights changed between capture and bake");
            }
        }
    }

    // Whole-mesh weight sanity: weights stay finite, non-negative and unit
    // sum; this never renormalizes anything.
    for (size_t vertexId = 0; vertexId < runs.size(); ++vertexId) {
        double sum = 0.0;
        for (size_t k = runs[vertexId].first; k < runs[vertexId].second; ++k) {
            const float weight = influences[k].weight;
            if (!isFinite(weight) || weight < 0.0f) {
                return setError(errorMessage, "bake: influence weight must be finite and non-negative");
            }
            sum += weight;
        }
        if (std::fabs(sum - 1.0) > kWeightSumTolerance) {
            return setError(errorMessage, "bake: per-vertex influence weights must sum to 1");
        }
    }
    return true;
}

bool bakeSculptMorphTarget(
    const ISTD::shared_ptr<CalCoreSubmesh>& coreSubmesh,
    const Snapshot* const* snapshots,
    size_t snapshotCount,
    const std::string& targetName,
    double residualLimit,
    ISTD::shared_ptr<CalCoreMorphTarget>& outMorphTarget,
    BakeReport& outReport,
    std::string* errorMessage)
{
    outMorphTarget.reset();
    outReport = BakeReport();

    const std::string suffix = ".additive";
    if (targetName.size() < suffix.size()
        || targetName.compare(targetName.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return setError(errorMessage, "bake: target name must end in \".additive\"");
    }
    if (!isFinite(residualLimit) || residualLimit < 0.0) {
        return setError(errorMessage, "bake: residual limit must be finite and non-negative");
    }
    if (!coreSubmesh) {
        return setError(errorMessage, "bake: core submesh is null");
    }
    if (snapshotCount > 0 && snapshots == 0) {
        return setError(errorMessage, "bake: snapshot array is null but non-empty");
    }

    // vertex id -> rows contributed by every snapshot covering that vertex.
    std::map<size_t, std::vector<VertexRow> > rowsByVertex;

    for (size_t snapshotIndex = 0; snapshotIndex < snapshotCount; ++snapshotIndex) {
        const Snapshot* snapshot = snapshots[snapshotIndex];
        if (snapshot == 0) {
            return setError(errorMessage, "bake: snapshot entry is null");
        }
        if (snapshot->m_coreSubmesh.get() != coreSubmesh.get()) {
            return setError(errorMessage, "bake: all snapshots must share the same core submesh identity");
        }
        if (!Snapshot::verifyForBake(*snapshot, errorMessage)) {
            return false;
        }

        for (size_t i = 0; i < snapshot->m_records.size(); ++i) {
            const Snapshot::Record& record = snapshot->m_records[i];
            VertexRow row;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    row.a[r][c] = record.palette[r][c];
                }
                row.b[r] = record.target[r] - record.skinnedBase[r];
            }
            row.weight = snapshot->m_weight;
            if (!rowFinite(row)) {
                return setError(errorMessage, "bake: snapshot contains non-finite capture data");
            }
            rowsByVertex[record.vertexId].push_back(row);
        }
    }

    CalCoreMorphTarget::VertexOffsetArray offsets;
    outReport.snapshotCount = snapshotCount;
    outReport.vertexReports.reserve(rowsByVertex.size());
    double globalMaxResidual = 0.0;
    bool residualLimitExceeded = false;

    for (std::map<size_t, std::vector<VertexRow> >::const_iterator it = rowsByVertex.begin();
        it != rowsByVertex.end();
        ++it)
    {
        const size_t vertexId = it->first;
        const std::vector<VertexRow>& rows = it->second;

        // Stack sqrt(weight) * A and sqrt(weight) * b, turning the weighted
        // least-squares problem into an ordinary one.  Only the linear part
        // of each palette participates; bone translations never multiply the
        // bind-space increment.
        std::vector<double> matrix(3 * rows.size() * 3);
        std::vector<double> rhs(3 * rows.size());
        for (size_t k = 0; k < rows.size(); ++k) {
            const double scale = std::sqrt(rows[k].weight);
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    matrix[(3 * k + r) * 3 + c] = scale * rows[k].a[r][c];
                }
                rhs[3 * k + r] = scale * rows[k].b[r];
            }
        }

        JacobiSVD3 svd;
        svd.decompose(matrix, 3 * rows.size());

        double d[3];
        svd.solve(rhs, d);

        // Residuals are judged from the float increment that will actually be
        // stored, not from the double-precision solution.
        const float df[3] = {
            static_cast<float>(d[0]),
            static_cast<float>(d[1]),
            static_cast<float>(d[2])
        };

        double vertexMaxResidual = 0.0;
        for (size_t k = 0; k < rows.size(); ++k) {
            double residualSquared = 0.0;
            for (int r = 0; r < 3; ++r) {
                double predicted = 0.0;
                for (int c = 0; c < 3; ++c) {
                    predicted += rows[k].a[r][c] * static_cast<double>(df[c]);
                }
                const double residual = predicted - rows[k].b[r];
                residualSquared += residual * residual;
            }
            vertexMaxResidual = std::max(vertexMaxResidual, std::sqrt(residualSquared));
        }

        VertexReport report;
        report.vertexId = vertexId;
        report.effectiveRank = svd.rank;
        report.maxResidual = vertexMaxResidual;
        outReport.vertexReports.push_back(report);
        globalMaxResidual = std::max(globalMaxResidual, vertexMaxResidual);
        if (vertexMaxResidual > residualLimit) {
            residualLimitExceeded = true;
        }

        // Strictly zero float increments create no sparse entry.
        if (df[0] != 0.0f || df[1] != 0.0f || df[2] != 0.0f) {
            offsets.push_back(VertexOffset(
                vertexId,
                CalPoint4(df[0], df[1], df[2], 1.0f),
                CalVector4(0.0f, 0.0f, 0.0f, 0.0f)));
        }
    }

    outReport.maxResidual = globalMaxResidual;

    if (residualLimitExceeded) {
        return setError(errorMessage, "bake: stored float residual exceeds the requested limit");
    }

    // Offsets are already in ascending vertexId because rowsByVertex is a
    // sorted std::map and zero entries merely drop out.
    outMorphTarget = ISTD::make_shared<CalCoreMorphTarget>(
        targetName,
        coreSubmesh->getVertexCount(),
        offsets);
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

} // namespace CalSculptMorph
