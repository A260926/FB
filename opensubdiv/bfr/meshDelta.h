//
//   Copyright 2021 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

#ifndef OPENSUBDIV3_BFR_MESH_DELTA_H
#define OPENSUBDIV3_BFR_MESH_DELTA_H

///
/// @file meshDelta.h
///
/// @brief Incremental mesh deltas, their on-disk protocol and the
///        topology-free MeshMirror that replays them.
///
/// This header is self contained: it depends only on the C++17 standard
/// library and must not include any other OpenSubdiv header. A consumer
/// process that never sees the subdivision control cage can rebuild an
/// OBJ-exportable triangle mesh solely from a sequence of MeshDelta
/// records encoded by WriteMeshDeltaFile().
///

#include "../version.h"

#include <cstdint>
#include <array>
#include <map>
#include <string>
#include <vector>

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Bfr {

///
/// @brief Maximum size (64 MiB) accepted for a single delta file.
///
constexpr uint64_t MeshDeltaMaxFileBytes = 64ull * 1024ull * 1024ull;

///
/// @brief Maximum number of records allowed in any one delta list.
///
constexpr uint64_t MeshDeltaMaxRecordsPerGroup = 1000000ull;

///
/// @brief Eight byte ASCII magic identifying an OSDM revision 1 stream.
///
constexpr char MeshDeltaMagic[8] = {
    'O', 'S', 'D', 'M', '0', '0', '0', '1'
};

///
/// @brief A position record: one shared location in the mesh.
///
/// Position IDs and triangle IDs live in independent ID spaces.
///
struct DeltaPosition {
    uint64_t id = 0;                          ///< Non-zero position ID
    std::array<double, 3> position = {0.0, 0.0, 0.0};
};

///
/// @brief A triangle record with per-corner UV and analytic unit normal.
///
/// Corner data is never merged into per-point data: two triangles may
/// reference the same position ID while carrying distinct UV/normal
/// triples.
///
struct DeltaTriangle {
    uint64_t id = 0;                          ///< Non-zero triangle ID
    uint32_t face = 0;                        ///< Originating base face index
    std::array<uint64_t, 3> positionIds = {0ull, 0ull, 0ull};

    /// Two local parametric coordinates per corner, in corner order.
    std::array<std::array<double, 2>, 3> cornerUV = {};

    /// One analytic unit normal per corner, in corner order.
    std::array<std::array<double, 3>, 3> cornerNormal = {};
};

///
/// @brief An owned, serializable change from revision @p fromRevision to
///        @p toRevision.
///
/// All four record lists are sorted by ID and contain no duplicates.
/// An ID never appears in both a delete list and the matching upsert
/// list of the same delta. A delta whose four lists are all empty is a
/// legal no-op only when fromRevision == toRevision.
///
struct MeshDelta {
    uint64_t fromRevision = 0;
    uint64_t toRevision   = 0;

    std::vector<uint64_t>      deletedTriangles;
    std::vector<uint64_t>      deletedPositions;
    std::vector<DeltaPosition> positionUpserts;
    std::vector<DeltaTriangle> triangleUpserts;

    /// @brief True when the delta carries no records of any kind.
    bool IsEmpty() const {
        return deletedTriangles.empty() && deletedPositions.empty() &&
               positionUpserts.empty() && triangleUpserts.empty();
    }
};

///
/// @brief An owned, ID-sorted snapshot of a complete triangle mesh.
///
/// Snapshots own their storage and stay unchanged when the session or
/// mirror that produced them is edited afterwards.
///
struct MeshSnapshot {
    uint64_t revision = 0;
    std::vector<DeltaPosition> positions;
    std::vector<DeltaTriangle> triangles;
};

///
/// @brief Encode a delta into the deterministic OSDM0001 byte layout.
///
/// Throws std::invalid_argument on malformed in-memory data. The output
/// contains no padding and no trailing bytes.
///
std::vector<unsigned char> EncodeMeshDelta(MeshDelta const & delta);

///
/// @brief Decode an OSDM0001 byte buffer, validating every record.
///
/// Throws std::invalid_argument on any truncation, bad magic, count
/// overflow, non-finite value, ordering violation or malformed record.
///
MeshDelta DecodeMeshDelta(unsigned char const * data, size_t size);

/// @brief Convenience overload over a byte vector.
MeshDelta DecodeMeshDelta(std::vector<unsigned char> const & data);

///
/// @brief Validate @p delta and write it to @p path deterministically.
///
/// Invalid data is rejected before the destination file is touched, so
/// an existing valid file is never truncated by a failed write.
///
void WriteMeshDeltaFile(std::string const & path, MeshDelta const & delta);

///
/// @brief Read, size-check and decode a delta file.
///
/// Throws std::invalid_argument when the file is missing, empty, larger
/// than MeshDeltaMaxFileBytes or otherwise malformed.
///
MeshDelta ReadMeshDeltaFile(std::string const & path);

///
/// @brief Replays MeshDelta streams without ever holding a refiner,
///        Surface or control point array.
///
/// The mirror starts at revision 0 with an empty mesh. A non-empty
/// delta must satisfy from == current revision and to == from + 1.
/// An empty delta is legal when its revision equals the current one.
/// Every candidate delta is fully validated on a scratch copy and is
/// committed in one step only; a rejected delta leaves the mirror
/// unchanged.
///
class MeshMirror {
public:
    MeshMirror();
    ~MeshMirror();

    /// @brief Current revision (0 for the empty initial mesh).
    uint64_t Revision() const { return _revision; }

    /// @brief Number of live positions.
    size_t NumPositions() const { return _positions.size(); }

    /// @brief Number of live triangles.
    size_t NumTriangles() const { return _triangles.size(); }

    /// @brief Atomically apply an in-memory delta (strong exception safety).
    void Apply(MeshDelta const & delta);

    /// @brief Read a file and atomically apply the decoded delta.
    void ApplyFile(std::string const & path);

    /// @brief Owning snapshot with positions and triangles sorted by ID.
    MeshSnapshot Snapshot() const;

private:
    struct PositionEntry {
        std::array<double, 3> value;
    };
    struct TriangleEntry {
        uint32_t face;
        std::array<uint64_t, 3> positionIds;
        std::array<std::array<double, 2>, 3> cornerUV;
        std::array<std::array<double, 3>, 3> cornerNormal;
    };

    uint64_t _revision;
    std::map<uint64_t, PositionEntry> _positions;
    std::map<uint64_t, TriangleEntry> _triangles;
};

} // end namespace Bfr

} // end namespace OPENSUBDIV_VERSION
using namespace OPENSUBDIV_VERSION;
} // end namespace OpenSubdiv

#endif /* OPENSUBDIV3_BFR_MESH_DELTA_H */
