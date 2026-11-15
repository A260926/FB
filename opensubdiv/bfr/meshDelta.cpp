//
//   Copyright 2021 Pixar
//
//   Licensed under the terms set forth in the LICENSE.txt file available at
//   https://opensubdiv.org/license.
//

#include "meshDelta.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Bfr {

namespace {

constexpr size_t HeaderSize = 8 + 6 * 8;          // magic + 6 uint64
constexpr size_t PositionRecordSize = 8 + 3 * 8;  // id + 3 doubles
constexpr size_t TriangleRecordSize =
    8 + 4 + 3 * 8 + 6 * 8 + 9 * 8;                // id, face, ids, uv, n

void
appendU64(std::vector<unsigned char> & out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFFu));
    }
}

void
appendU32(std::vector<unsigned char> & out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFFu));
    }
}

void
appendDouble(std::vector<unsigned char> & out, double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    appendU64(out, bits);
}

uint64_t
readU64(unsigned char const * p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return value;
}

uint32_t
readU32(unsigned char const * p) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(p[i]) << (8 * i);
    }
    return value;
}

double
readDouble(unsigned char const * p) {
    uint64_t bits = readU64(p);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool
allFinite(double const * values, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) return false;
    }
    return true;
}

bool
unitNormal(std::array<double, 3> const & n) {
    if (!allFinite(n.data(), 3)) return false;
    double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    return std::abs(length - 1.0) <= 1e-9;
}

//  Validate everything that is knowable without mirror state.
void
validateDelta(MeshDelta const & delta) {
    if (delta.IsEmpty()) {
        if (delta.fromRevision != delta.toRevision) {
            throw std::invalid_argument(
                "MeshDelta: empty delta must keep the same revision");
        }
    } else if (delta.toRevision != delta.fromRevision + 1) {
        throw std::invalid_argument(
            "MeshDelta: non-empty delta must advance revision by one");
    }

    auto checkCounts = [](size_t n) {
        if (n > MeshDeltaMaxRecordsPerGroup) {
            throw std::invalid_argument(
                "MeshDelta: record group exceeds 1000000 entries");
        }
    };
    checkCounts(delta.deletedTriangles.size());
    checkCounts(delta.deletedPositions.size());
    checkCounts(delta.positionUpserts.size());
    checkCounts(delta.triangleUpserts.size());

    auto checkSortedUnique = [](std::vector<uint64_t> const & ids,
                                char const * what) {
        for (size_t i = 0; i < ids.size(); ++i) {
            if (ids[i] == 0) {
                throw std::invalid_argument(
                    std::string("MeshDelta: zero ID in ") + what);
            }
            if (i && ids[i] <= ids[i - 1]) {
                throw std::invalid_argument(
                    std::string("MeshDelta: ") + what +
                    " must be strictly sorted by ID");
            }
        }
    };
    checkSortedUnique(delta.deletedTriangles, "deleted triangles");
    checkSortedUnique(delta.deletedPositions, "deleted positions");

    for (size_t i = 0; i < delta.positionUpserts.size(); ++i) {
        DeltaPosition const & rec = delta.positionUpserts[i];
        if (rec.id == 0) {
            throw std::invalid_argument("MeshDelta: zero position ID");
        }
        if (i && rec.id <= delta.positionUpserts[i - 1].id) {
            throw std::invalid_argument(
                "MeshDelta: position upserts must be strictly sorted by ID");
        }
        if (!allFinite(rec.position.data(), 3)) {
            throw std::invalid_argument(
                "MeshDelta: non-finite position component");
        }
    }
    for (size_t i = 0; i < delta.triangleUpserts.size(); ++i) {
        DeltaTriangle const & rec = delta.triangleUpserts[i];
        if (rec.id == 0) {
            throw std::invalid_argument("MeshDelta: zero triangle ID");
        }
        if (i && rec.id <= delta.triangleUpserts[i - 1].id) {
            throw std::invalid_argument(
                "MeshDelta: triangle upserts must be strictly sorted by ID");
        }
        if (rec.positionIds[0] == 0 || rec.positionIds[1] == 0 ||
            rec.positionIds[2] == 0) {
            throw std::invalid_argument("MeshDelta: zero referenced position");
        }
        if (rec.positionIds[0] == rec.positionIds[1] ||
            rec.positionIds[1] == rec.positionIds[2] ||
            rec.positionIds[0] == rec.positionIds[2]) {
            throw std::invalid_argument(
                "MeshDelta: triangle must reference three distinct positions");
        }
        for (auto const & uv : rec.cornerUV) {
            if (!allFinite(uv.data(), 2)) {
                throw std::invalid_argument("MeshDelta: non-finite UV");
            }
        }
        for (auto const & nrm : rec.cornerNormal) {
            if (!unitNormal(nrm)) {
                throw std::invalid_argument(
                    "MeshDelta: normal must be finite and unit length");
            }
        }
    }

    //  An ID must never be both deleted and upserted in the same delta.
    auto findPositionUpsert = [&](uint64_t id) {
        auto it = std::lower_bound(
            delta.positionUpserts.begin(), delta.positionUpserts.end(), id,
            [](DeltaPosition const & rec, uint64_t v) {
                return rec.id < v;
            });
        return it != delta.positionUpserts.end() && it->id == id;
    };
    auto findTriangleUpsert = [&](uint64_t id) {
        auto it = std::lower_bound(
            delta.triangleUpserts.begin(), delta.triangleUpserts.end(), id,
            [](DeltaTriangle const & rec, uint64_t v) {
                return rec.id < v;
            });
        return it != delta.triangleUpserts.end() && it->id == id;
    };
    for (uint64_t id : delta.deletedPositions) {
        if (findPositionUpsert(id)) {
            throw std::invalid_argument(
                "MeshDelta: position ID both deleted and upserted");
        }
    }
    for (uint64_t id : delta.deletedTriangles) {
        if (findTriangleUpsert(id)) {
            throw std::invalid_argument(
                "MeshDelta: triangle ID both deleted and upserted");
        }
    }
}

size_t
encodedSize(MeshDelta const & delta) {
    //  Saturating checked arithmetic; the 64 MiB cap is far below overflow.
    auto add = [](size_t total, size_t count, size_t recordSize) {
        if (count > (std::numeric_limits<size_t>::max() - total) /
                         recordSize) {
            throw std::invalid_argument(
                "MeshDelta: encoded size overflow");
        }
        return total + count * recordSize;
    };
    size_t size = HeaderSize;
    size = add(size, delta.deletedTriangles.size(), sizeof(uint64_t));
    size = add(size, delta.deletedPositions.size(), sizeof(uint64_t));
    size = add(size, delta.positionUpserts.size(), PositionRecordSize);
    size = add(size, delta.triangleUpserts.size(), TriangleRecordSize);
    if (size > MeshDeltaMaxFileBytes) {
        throw std::invalid_argument("MeshDelta: encoded stream exceeds 64 MiB");
    }
    return size;
}

} // unnamed namespace

std::vector<unsigned char>
EncodeMeshDelta(MeshDelta const & delta) {

    static_assert(sizeof(double) == 8,
                  "OSDM0001 requires IEEE-754 binary64 doubles");

    validateDelta(delta);
    size_t expectedSize = encodedSize(delta);

    std::vector<unsigned char> out;
    out.reserve(expectedSize);

    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<unsigned char>(MeshDeltaMagic[i]));
    }
    appendU64(out, delta.fromRevision);
    appendU64(out, delta.toRevision);
    appendU64(out, static_cast<uint64_t>(delta.deletedTriangles.size()));
    appendU64(out, static_cast<uint64_t>(delta.deletedPositions.size()));
    appendU64(out, static_cast<uint64_t>(delta.positionUpserts.size()));
    appendU64(out, static_cast<uint64_t>(delta.triangleUpserts.size()));

    for (uint64_t id : delta.deletedTriangles) appendU64(out, id);
    for (uint64_t id : delta.deletedPositions) appendU64(out, id);

    for (DeltaPosition const & rec : delta.positionUpserts) {
        appendU64(out, rec.id);
        appendDouble(out, rec.position[0]);
        appendDouble(out, rec.position[1]);
        appendDouble(out, rec.position[2]);
    }
    for (DeltaTriangle const & rec : delta.triangleUpserts) {
        appendU64(out, rec.id);
        appendU32(out, rec.face);
        appendU64(out, rec.positionIds[0]);
        appendU64(out, rec.positionIds[1]);
        appendU64(out, rec.positionIds[2]);
        for (auto const & uv : rec.cornerUV) {
            appendDouble(out, uv[0]);
            appendDouble(out, uv[1]);
        }
        for (auto const & nrm : rec.cornerNormal) {
            appendDouble(out, nrm[0]);
            appendDouble(out, nrm[1]);
            appendDouble(out, nrm[2]);
        }
    }

    if (out.size() != expectedSize) {
        throw std::runtime_error("MeshDelta: internal encoding size mismatch");
    }
    return out;
}

MeshDelta
DecodeMeshDelta(unsigned char const * data, size_t size) {

    if (data == nullptr) {
        throw std::invalid_argument("MeshDelta: null input buffer");
    }
    if (size == 0) {
        throw std::invalid_argument("MeshDelta: empty stream");
    }
    if (size > MeshDeltaMaxFileBytes) {
        throw std::invalid_argument("MeshDelta: stream exceeds 64 MiB");
    }
    if (size < HeaderSize) {
        throw std::invalid_argument("MeshDelta: truncated header");
    }
    if (std::memcmp(data, MeshDeltaMagic, 8) != 0) {
        throw std::invalid_argument("MeshDelta: bad magic");
    }

    MeshDelta delta;
    delta.fromRevision = readU64(data + 8);
    delta.toRevision   = readU64(data + 16);

    uint64_t nDelTri  = readU64(data + 24);
    uint64_t nDelPos  = readU64(data + 32);
    uint64_t nPosUp   = readU64(data + 40);
    uint64_t nTriUp   = readU64(data + 48);

    if (nDelTri > MeshDeltaMaxRecordsPerGroup ||
        nDelPos > MeshDeltaMaxRecordsPerGroup ||
        nPosUp  > MeshDeltaMaxRecordsPerGroup ||
        nTriUp  > MeshDeltaMaxRecordsPerGroup) {
        throw std::invalid_argument(
            "MeshDelta: record group exceeds 1000000 entries");
    }

    //  Verify the declared body size with checked arithmetic and require
    //  the file to end exactly there (no trailing bytes).
    uint64_t bodySize = 0;
    auto addBody = [&bodySize](uint64_t count, uint64_t recordSize) {
        if (count > (std::numeric_limits<uint64_t>::max() - bodySize) /
                         recordSize) {
            throw std::invalid_argument("MeshDelta: count overflow");
        }
        bodySize += count * recordSize;
    };
    addBody(nDelTri, 8);
    addBody(nDelPos, 8);
    addBody(nPosUp, PositionRecordSize);
    addBody(nTriUp, TriangleRecordSize);

    if (bodySize > std::numeric_limits<uint64_t>::max() - HeaderSize ||
        HeaderSize + static_cast<size_t>(bodySize) != size) {
        throw std::invalid_argument(
            "MeshDelta: size mismatch (truncated or trailing bytes)");
    }

    unsigned char const * p = data + HeaderSize;

    delta.deletedTriangles.resize(static_cast<size_t>(nDelTri));
    for (uint64_t i = 0; i < nDelTri; ++i) {
        delta.deletedTriangles[static_cast<size_t>(i)] = readU64(p);
        p += 8;
    }
    delta.deletedPositions.resize(static_cast<size_t>(nDelPos));
    for (uint64_t i = 0; i < nDelPos; ++i) {
        delta.deletedPositions[static_cast<size_t>(i)] = readU64(p);
        p += 8;
    }

    delta.positionUpserts.resize(static_cast<size_t>(nPosUp));
    for (uint64_t i = 0; i < nPosUp; ++i) {
        DeltaPosition & rec = delta.positionUpserts[static_cast<size_t>(i)];
        rec.id = readU64(p);
        p += 8;
        rec.position[0] = readDouble(p);
        rec.position[1] = readDouble(p + 8);
        rec.position[2] = readDouble(p + 16);
        p += 24;
    }

    delta.triangleUpserts.resize(static_cast<size_t>(nTriUp));
    for (uint64_t i = 0; i < nTriUp; ++i) {
        DeltaTriangle & rec = delta.triangleUpserts[static_cast<size_t>(i)];
        rec.id = readU64(p);
        p += 8;
        rec.face = readU32(p);
        p += 4;
        rec.positionIds[0] = readU64(p);
        rec.positionIds[1] = readU64(p + 8);
        rec.positionIds[2] = readU64(p + 16);
        p += 24;
        for (auto & uv : rec.cornerUV) {
            uv[0] = readDouble(p);
            uv[1] = readDouble(p + 8);
            p += 16;
        }
        for (auto & nrm : rec.cornerNormal) {
            nrm[0] = readDouble(p);
            nrm[1] = readDouble(p + 8);
            nrm[2] = readDouble(p + 16);
            p += 24;
        }
    }

    //  Full semantic validation, identical to the encoding contract.
    validateDelta(delta);
    return delta;
}

MeshDelta
DecodeMeshDelta(std::vector<unsigned char> const & data) {
    return DecodeMeshDelta(data.data(), data.size());
}

void
WriteMeshDeltaFile(std::string const & path, MeshDelta const & delta) {
    //  Validate and encode fully before touching the destination so that
    //  an existing valid output file is never truncated on bad input.
    std::vector<unsigned char> bytes = EncodeMeshDelta(delta);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::invalid_argument(
            "MeshDelta: cannot open output file: " + path);
    }
    out.write(reinterpret_cast<char const *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw std::runtime_error(
            "MeshDelta: failed writing output file: " + path);
    }
}

MeshDelta
ReadMeshDeltaFile(std::string const & path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::invalid_argument(
            "MeshDelta: cannot open input file: " + path);
    }
    std::streamoff signedSize = in.tellg();
    if (signedSize <= 0) {
        throw std::invalid_argument("MeshDelta: empty or unreadable file");
    }
    uint64_t fileSize = static_cast<uint64_t>(signedSize);
    if (fileSize > MeshDeltaMaxFileBytes) {
        throw std::invalid_argument("MeshDelta: file exceeds 64 MiB");
    }
    std::vector<unsigned char> bytes(static_cast<size_t>(fileSize));
    in.seekg(0, std::ios::beg);
    if (!in.read(reinterpret_cast<char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()))) {
        throw std::invalid_argument("MeshDelta: short read");
    }
    return DecodeMeshDelta(bytes);
}

//
//  MeshMirror
//
MeshMirror::MeshMirror() : _revision(0) { }
MeshMirror::~MeshMirror() = default;

void
MeshMirror::Apply(MeshDelta const & delta) {

    //  Full record-level validation on a scratch path, identical to the
    //  file contract, before any mirror state is touched.
    validateDelta(delta);

    if (delta.IsEmpty()) {
        if (delta.fromRevision != delta.toRevision ||
            delta.fromRevision != _revision) {
            throw std::invalid_argument(
                "MeshMirror: empty delta revision mismatch");
        }
        return;
    }
    if (delta.fromRevision != _revision) {
        throw std::invalid_argument(
            "MeshMirror: delta does not start at current revision");
    }
    if (delta.toRevision != delta.fromRevision + 1) {
        throw std::invalid_argument(
            "MeshMirror: delta must advance revision by exactly one");
    }

    //  Build the candidate state on copies; nothing is committed until
    //  every check has passed.
    std::map<uint64_t, PositionEntry> candidatePositions = _positions;
    std::map<uint64_t, TriangleEntry> candidateTriangles = _triangles;

    for (uint64_t id : delta.deletedTriangles) {
        if (candidateTriangles.erase(id) != 1) {
            throw std::invalid_argument(
                "MeshMirror: deleted triangle does not exist");
        }
    }
    for (uint64_t id : delta.deletedPositions) {
        if (candidatePositions.erase(id) != 1) {
            throw std::invalid_argument(
                "MeshMirror: deleted position does not exist");
        }
    }

    for (DeltaPosition const & rec : delta.positionUpserts) {
        candidatePositions[rec.id] = PositionEntry{rec.position};
    }
    for (DeltaTriangle const & rec : delta.triangleUpserts) {
        TriangleEntry entry;
        entry.face = rec.face;
        entry.positionIds = rec.positionIds;
        entry.cornerUV = rec.cornerUV;
        entry.cornerNormal = rec.cornerNormal;
        candidateTriangles[rec.id] = entry;
    }

    //  Every surviving triangle must reference surviving positions.
    for (auto const & kv : candidateTriangles) {
        TriangleEntry const & tri = kv.second;
        for (uint64_t posId : tri.positionIds) {
            if (candidatePositions.find(posId) == candidatePositions.end()) {
                throw std::invalid_argument(
                    "MeshMirror: triangle references missing position");
            }
        }
    }

    //  All checks passed: single commit.
    _positions = std::move(candidatePositions);
    _triangles = std::move(candidateTriangles);
    _revision = delta.toRevision;
}

void
MeshMirror::ApplyFile(std::string const & path) {
    MeshDelta delta = ReadMeshDeltaFile(path);
    Apply(delta);
}

MeshSnapshot
MeshMirror::Snapshot() const {
    MeshSnapshot snapshot;
    snapshot.revision = _revision;
    snapshot.positions.reserve(_positions.size());
    for (auto const & kv : _positions) {
        DeltaPosition rec;
        rec.id = kv.first;
        rec.position = kv.second.value;
        snapshot.positions.push_back(rec);
    }
    snapshot.triangles.reserve(_triangles.size());
    for (auto const & kv : _triangles) {
        DeltaTriangle rec;
        rec.id = kv.first;
        rec.face = kv.second.face;
        rec.positionIds = kv.second.positionIds;
        rec.cornerUV = kv.second.cornerUV;
        rec.cornerNormal = kv.second.cornerNormal;
        snapshot.triangles.push_back(rec);
    }
    return snapshot;
}

} // end namespace Bfr

} // end namespace OPENSUBDIV_VERSION
using namespace OPENSUBDIV_VERSION;
} // end namespace OpenSubdiv
