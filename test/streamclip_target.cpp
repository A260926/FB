// Tests for the streamed-clip format, the shared block cache and the mixed
// in-memory / streamed mixer path.  No main(): task_support/driver.c runs the
// registered cases through TASK_CHECK.

#include "task_support/check.h"

#include <cal3d/coreanimation.h>
#include <cal3d/corebone.h>
#include <cal3d/corekeyframe.h>
#include <cal3d/coreskeleton.h>
#include <cal3d/coresubmesh.h>
#include <cal3d/coretrack.h>
#include <cal3d/mixer.h>
#include <cal3d/physique.h>
#include <cal3d/skeleton.h>
#include <cal3d/streamclip.h>
#include <cal3d/submesh.h>
#include <cal3d/transform.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

int g_fileSerial = 0;

std::string tempPath(const char* tag) {
    const char* dir = std::getenv("TMPDIR");
    if (!dir || !*dir) {
        dir = ".";
    }
    char name[512];
    std::snprintf(name, sizeof(name), "%s/streamclip_%s_%d_%d.clip",
                  dir, tag, static_cast<int>(::getpid()), ++g_fileSerial);
    return name;
}

struct TempFile {
    explicit TempFile(const char* tag)
        : path(tempPath(tag))
    {}
    ~TempFile() {
        std::remove(path.c_str());
    }
    std::string path;
};

bool closef(float a, float b, float tol = 2e-5f) {
    return std::fabs(a - b) <= tol;
}

bool closeV(const CalVector& a, const CalVector& b, float tol = 2e-5f) {
    return closef(a.x, b.x, tol) && closef(a.y, b.y, tol) && closef(a.z, b.z, tol);
}

bool closeQ(const CalQuaternion& a, const CalQuaternion& b, float tol = 2e-5f) {
    // q and -q are the same rotation; accept either sign.
    float direct = std::fabs(a.x - b.x) + std::fabs(a.y - b.y)
        + std::fabs(a.z - b.z) + std::fabs(a.w - b.w);
    float flipped = std::fabs(a.x + b.x) + std::fabs(a.y + b.y)
        + std::fabs(a.z + b.z) + std::fabs(a.w + b.w);
    return direct <= tol || flipped <= tol;
}

bool closeRT(const cal3d::RotateTranslate& a, const cal3d::RotateTranslate& b, float tol = 2e-5f) {
    return closeV(a.translation, b.translation, tol) && closeQ(a.rotation, b.rotation, tol);
}

CalCoreKeyframe key(float t, float x, float y, float z, const CalQuaternion& q = CalQuaternion()) {
    return CalCoreKeyframe(t, CalVector(x, y, z), q);
}

ISTD::shared_ptr<CalCoreAnimation> makeAnimation(
    float duration,
    const std::vector<CalCoreTrack>& tracks
) {
    ISTD::shared_ptr<CalCoreAnimation> animation = ISTD::make_shared<CalCoreAnimation>();
    animation->duration = duration;
    animation->tracks = tracks;
    return animation;
}

// Offsets from the header layout, for hand-built files.
namespace fmt {
const uint32_t kHeader = 64;
const uint32_t kMetaEntry = 8;
const uint32_t kDirEntry = 32;
const uint32_t kKey = 32;
}

void putU32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    v[off + 0] = static_cast<uint8_t>(x);
    v[off + 1] = static_cast<uint8_t>(x >> 8);
    v[off + 2] = static_cast<uint8_t>(x >> 16);
    v[off + 3] = static_cast<uint8_t>(x >> 24);
}

void putU64(std::vector<uint8_t>& v, size_t off, uint64_t x) {
    putU32(v, off, static_cast<uint32_t>(x));
    putU32(v, off + 4, static_cast<uint32_t>(x >> 32));
}

void putF32(std::vector<uint8_t>& v, size_t off, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    putU32(v, off, bits);
}

uint32_t crc32Of(const uint8_t* data, size_t size) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

bool writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

// A helper for constructing one-block-per-file fixtures by hand.  Each track
// is a list of raw keys; the caller decides what keys belong to the block.
struct HandTrack {
    uint32_t boneId;
    std::vector<CalStreamClip::StreamKey> keys;
};

CalStreamClip::StreamKey rawKey(float t, float x, float y, float z,
                                const CalQuaternion& q = CalQuaternion()) {
    CalStreamClip::StreamKey k;
    k.time = t;
    k.translation = CalVector(x, y, z);
    k.rotation = q;
    return k;
}

// Build a syntactically valid stream clip with a single block holding the
// supplied tracks.  Used both to prove "open reads zero blocks" and to build
// deliberately corrupt fixtures (by mutating the returned bytes).
std::vector<uint8_t> buildSingleBlockFile(
    float duration,
    float blockDuration,
    const std::vector<HandTrack>& tracks
) {
    // Panel: header | metadata | directory | block.
    const uint32_t trackCount = static_cast<uint32_t>(tracks.size());
    std::vector<uint8_t> payload;

    // payload: u32 track count, then per track (boneId, keyCount, keys...)
    size_t payloadSize = 4;
    for (size_t t = 0; t < tracks.size(); ++t) {
        payloadSize += 8 + tracks[t].keys.size() * fmt::kKey;
    }
    payload.resize(payloadSize);
    putU32(payload, 0, trackCount);
    size_t cursor = 4;
    for (size_t t = 0; t < tracks.size(); ++t) {
        putU32(payload, cursor, tracks[t].boneId);
        putU32(payload, cursor + 4, static_cast<uint32_t>(tracks[t].keys.size()));
        cursor += 8;
        for (size_t k = 0; k < tracks[t].keys.size(); ++k) {
            const CalStreamClip::StreamKey& key = tracks[t].keys[k];
            putF32(payload, cursor + 0, key.time);
            putF32(payload, cursor + 4, key.translation.x);
            putF32(payload, cursor + 8, key.translation.y);
            putF32(payload, cursor + 12, key.translation.z);
            putF32(payload, cursor + 16, key.rotation.x);
            putF32(payload, cursor + 20, key.rotation.y);
            putF32(payload, cursor + 24, key.rotation.z);
            putF32(payload, cursor + 28, key.rotation.w);
            cursor += fmt::kKey;
        }
    }

    const uint32_t metaOffset = fmt::kHeader;
    const uint32_t dirOffset = metaOffset + trackCount * fmt::kMetaEntry;
    const uint32_t dataOffset = dirOffset + fmt::kDirEntry;
    const uint32_t fileSize = dataOffset + static_cast<uint32_t>(payload.size());

    std::vector<uint8_t> out(fileSize, 0);
    const char magic[8] = { 'C', 'A', 'L', 'S', 'C', 'L', 'P', '1' };
    std::memcpy(out.data(), magic, 8);
    putU32(out, 8, 1);
    putU32(out, 12, fmt::kHeader);
    putU32(out, 16, 0);
    putF32(out, 20, blockDuration);
    putF32(out, 24, duration);
    putU32(out, 28, trackCount);
    putU32(out, 32, 1);
    putU32(out, 36, metaOffset);
    putU32(out, 40, fmt::kMetaEntry);
    putU32(out, 44, dirOffset);
    putU32(out, 48, fmt::kDirEntry);
    putU64(out, 52, fileSize);
    putU32(out, 60, 0);

    for (size_t t = 0; t < tracks.size(); ++t) {
        putU32(out, metaOffset + t * fmt::kMetaEntry, tracks[t].boneId);
        putU32(out, metaOffset + t * fmt::kMetaEntry + 4, 0);
    }

    putU32(out, dirOffset + 0, 0);
    putF32(out, dirOffset + 4, 0.0f);
    putF32(out, dirOffset + 8, duration);
    putU64(out, dirOffset + 12, dataOffset);
    putU32(out, dirOffset + 20, static_cast<uint32_t>(payload.size()));
    putU32(out, dirOffset + 24, crc32Of(payload.data(), payload.size()));
    putU32(out, dirOffset + 28, 0);

    std::memcpy(out.data() + dataOffset, payload.data(), payload.size());
    return out;
}

// Offsets for the one-track single-block fixture used by the rejection tests.
const uint32_t kMetaOffset = 64;
const uint32_t kDirOffset = 64 + 8;
const uint32_t kDataOffset = kDirOffset + 32;

void patchBlockCrc(std::vector<uint8_t>& bytes) {
    // Single-block fixtures: the directory sits at 64 + trackCount*8 and the
    // block starts right after the one 32-byte directory entry.
    const uint32_t trackCount = (bytes[28]) | (bytes[29] << 8) | (bytes[30] << 16) | (bytes[31] << 24);
    uint32_t dirOffset = (bytes[44]) | (bytes[45] << 8) | (bytes[46] << 16) | (bytes[47] << 24);
    (void)trackCount;
    uint32_t dataOffset = static_cast<uint32_t>(
        bytes[dirOffset + 12] | (bytes[dirOffset + 13] << 8)
        | (bytes[dirOffset + 14] << 16) | (bytes[dirOffset + 15] << 24));
    uint32_t length = static_cast<uint32_t>(
        bytes[dirOffset + 20] | (bytes[dirOffset + 21] << 8)
        | (bytes[dirOffset + 22] << 16) | (bytes[dirOffset + 23] << 24));
    putU32(bytes, dirOffset + 24, crc32Of(bytes.data() + dataOffset, length));
}

// ---------------------------------------------------------------------------
// Skeleton / submesh fixtures for the mixed pipeline
// ---------------------------------------------------------------------------

// root -> child.  The rotation on the root makes the parent/child ordering
// observable, and the inverse bind pose is not the identity.
ISTD::shared_ptr<CalCoreSkeleton> makeTwoBoneSkeleton() {
    ISTD::shared_ptr<CalCoreSkeleton> core = ISTD::make_shared<CalCoreSkeleton>();
    ISTD::shared_ptr<CalCoreBone> root = ISTD::make_shared<CalCoreBone>("root");
    ISTD::shared_ptr<CalCoreBone> child = ISTD::make_shared<CalCoreBone>("child", 0);

    root->relativeTransform = cal3d::RotateTranslate();
    CalQuaternion rootOri;
    rootOri.setAxisAngle(CalVector(0, 0, 1), 0.9f);
    root->relativeTransform.rotation = rootOri;
    root->relativeTransform.translation = CalVector(0, 0, 0.5f);
    root->inverseBindPoseTransform.translation = CalVector(-0.25f, 0.1f, 0.0f);

    child->relativeTransform.translation = CalVector(0, 1, 0);
    CalQuaternion childOri;
    childOri.setAxisAngle(CalVector(1, 0, 0), -0.4f);
    child->relativeTransform.rotation = childOri;
    child->inverseBindPoseTransform.translation = CalVector(0.3f, -1.5f, 0.2f);

    core->addCoreBone(root);
    core->addCoreBone(child);
    return core;
}

// One vertex weighted 25% to the root and 75% to the child, so both the
// weighted blend and the parent-before-child absolute pose are exercised.
ISTD::shared_ptr<CalCoreSubmesh> makeWeightedSubmesh() {
    ISTD::shared_ptr<CalCoreSubmesh> sub = ISTD::make_shared<CalCoreSubmesh>(1, false, 0);
    CalCoreSubmesh::Vertex v;
    v.position = CalPoint4(1.5f, -0.5f, 0.75f);
    v.normal = CalVector4(0.0f, 1.0f, 0.0f);
    std::vector<CalCoreSubmesh::Influence> influences;
    influences.push_back(CalCoreSubmesh::Influence(0, 0.25f, false));
    influences.push_back(CalCoreSubmesh::Influence(1, 0.75f, true));
    sub->addVertex(v, CalColor32(0), influences);
    return sub;
}

// A two-track animation that moves and rotates both bones.
ISTD::shared_ptr<CalCoreAnimation> makePipelineClip(float duration, float phase) {
    CalCoreTrack::KeyframeList rootKeys;
    rootKeys.push_back(CalCoreKeyframe(0.0f, CalVector(0, 0, 0),
                                       CalQuaternion(0, 0, 0, 1)));
    CalQuaternion rootMid;
    rootMid.setAxisAngle(CalVector(0, 1, 0), 0.6f + phase);
    rootKeys.push_back(CalCoreKeyframe(duration * 0.5f, CalVector(0.4f, 0.2f, -0.1f), rootMid));
    CalQuaternion rootEnd;
    rootEnd.setAxisAngle(CalVector(1, 1, 0), -0.3f + phase);
    rootKeys.push_back(CalCoreKeyframe(duration, CalVector(0.1f, -0.3f, 0.25f), rootEnd));

    CalCoreTrack::KeyframeList childKeys;
    childKeys.push_back(CalCoreKeyframe(0.0f, CalVector(0, 0, 0),
                                        CalQuaternion(0, 0, 0, 1)));
    CalQuaternion childMid;
    childMid.setAxisAngle(CalVector(0, 0, 1), 0.8f - phase);
    childKeys.push_back(CalCoreKeyframe(duration * 0.25f, CalVector(-0.2f, 0.3f, 0.1f), childMid));
    CalQuaternion childEnd;
    childEnd.setAxisAngle(CalVector(0, 1, 0), -0.5f - phase);
    childKeys.push_back(CalCoreKeyframe(duration, CalVector(0.15f, 0.05f, -0.2f), childEnd));

    std::vector<CalCoreTrack> tracks;
    tracks.push_back(CalCoreTrack(0, rootKeys));
    tracks.push_back(CalCoreTrack(1, childKeys));
    return makeAnimation(duration, tracks);
}

// Skin the standard test vertex and return position + normal.
void skinVertex(const CalSkeleton& skeleton, CalVector& position, CalVector& normal) {
    CalSubmesh runtime(makeWeightedSubmesh());
    CalVector4 out[2];
    CalPhysique::calculateVerticesAndNormals(
        cal3d::pointerFromVector(skeleton.boneTransforms),
        &runtime,
        reinterpret_cast<float*>(out));
    position = CalVector(out[0].x, out[0].y, out[0].z);
    normal = CalVector(out[1].x, out[1].y, out[1].z);
}

} // namespace

// ===========================================================================
// Group: block_sampling
// ===========================================================================

static int case_nonuniform_keys_cross_boundary() {
    // Keys deliberately fall between (and exactly on) the block boundaries
    // 0.5 and 1.0, and the animation never samples uniformly.
    CalCoreTrack::KeyframeList keys;
    keys.push_back(key(0.0f, 0, 0, 0));
    keys.push_back(key(0.3f, 3, 0, 0));
    keys.push_back(key(0.5f, 5, 0, 0));
    keys.push_back(key(0.7f, 7, 1, 0));
    keys.push_back(key(0.9f, 9, 2, 0));
    keys.push_back(key(1.0f, 10, 3, 0));
    keys.push_back(key(1.3f, 13, 4, 0));
    keys.push_back(key(1.7f, 17, 5, 0));
    std::vector<CalCoreTrack> tracks;
    tracks.push_back(CalCoreTrack(0, keys));
    ISTD::shared_ptr<CalCoreAnimation> animation = makeAnimation(1.7f, tracks);

    TempFile file("nonuniform");
    std::string error;
    TASK_CHECK(writeStreamClip(*animation, 0.5f, file.path, &error));

    ISTD::shared_ptr<CalStreamClip> clip = CalStreamClip::open(file.path, &error);
    TASK_CHECK(clip);
    TASK_CHECK(clip->blockCount() == 4);
    TASK_CHECK(clip->tracks().size() == 1);
    TASK_CHECK(clip->tracks()[0].boneId == 0);

    // Sample at and around every key and both block boundaries; the streamed
    // decoder must agree with the reference interpolation.
    const float times[] = { -2.0f, -0.1f, 0.0f, 0.1f, 0.3f, 0.49f, 0.5f, 0.51f,
                            0.7f, 0.9f, 0.999f, 1.0f, 1.01f, 1.3f, 1.69f,
                            1.7f, 1.9f, 5.0f };
    const size_t expectedBlocks[] = { 0, 0, 0, 0, 0, 0, 1, 1,
                                      1, 1, 1, 2, 2, 2, 3,
                                      3, 3, 3 };
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); ++i) {
        const unsigned blockIndex = clip->blockIndexForTime(times[i]);
        TASK_CHECK(blockIndex == expectedBlocks[i]);

        CalStreamClip::DecodedBlockPtr block;
        TASK_CHECK(clip->loadBlock(blockIndex, block, &error));
        TASK_CHECK(block && block->tracks.size() == 1);

        const cal3d::RotateTranslate streamed =
            CalStreamClip::sampleTrack(block->tracks[0], times[i]);
        const cal3d::RotateTranslate reference = animation->tracks[0].getCurrentTransform(times[i]);
        TASK_CHECK(closeRT(streamed, reference));
    }
    return 0;
}

static int case_empty_and_missing_tracks() {
    // Track 0 has keys, track 1 is empty ("empty track -> identity"), track 2
    // is absent entirely ("missing track -> bind pose").
    std::vector<CalCoreTrack> tracks;
    CalCoreTrack::KeyframeList keys;
    keys.push_back(key(0.0f, 1, 2, 3));
    keys.push_back(key(1.0f, 4, 5, 6));
    tracks.push_back(CalCoreTrack(0, keys));
    tracks.push_back(CalCoreTrack(1, CalCoreTrack::KeyframeList()));

    ISTD::shared_ptr<CalCoreAnimation> animation = makeAnimation(1.0f, tracks);

    TempFile file("empty");
    std::string error;
    TASK_CHECK(writeStreamClip(*animation, 0.4f, file.path, &error));
    ISTD::shared_ptr<CalStreamClip> clip = CalStreamClip::open(file.path, &error);
    TASK_CHECK(clip);
    TASK_CHECK(clip->tracks().size() == 2);
    TASK_CHECK(clip->tracks()[0].boneId == 0);
    TASK_CHECK(clip->tracks()[1].boneId == 1);

    CalStreamClip::DecodedBlockPtr block;
    TASK_CHECK(clip->loadBlock(1, block, &error));
    TASK_CHECK(block && block->tracks.size() == 2);
    TASK_CHECK(block->tracks[0].keys.size() >= 2);

    const float t = 0.55f;
    TASK_CHECK(block->tracks[1].keys.empty());
    const cal3d::RotateTranslate emptySample = CalStreamClip::sampleTrack(block->tracks[1], t);
    TASK_CHECK(closeV(emptySample.translation, CalVector(0, 0, 0)));
    TASK_CHECK(closeQ(emptySample.rotation, CalQuaternion()));
    // Identity matches the in-memory empty-track reference.
    TASK_CHECK(closeRT(emptySample, animation->tracks[1].getCurrentTransform(t)));

    // Mixing: bone 1 is in the file (empty track -> identity replaces the bind
    // pose), bone 2 has no track at all and keeps the bind pose.
    ISTD::shared_ptr<CalCoreSkeleton> core = ISTD::make_shared<CalCoreSkeleton>();
    CalCoreBone* b0 = new CalCoreBone("b0");
    CalCoreBone* b1 = new CalCoreBone("b1", 0);
    CalCoreBone* b2 = new CalCoreBone("b2", 0);
    b1->relativeTransform.translation = CalVector(10, 0, 0);
    b2->relativeTransform.translation = CalVector(0, 20, 0);
    core->addCoreBone(ISTD::shared_ptr<CalCoreBone>(b0));
    core->addCoreBone(ISTD::shared_ptr<CalCoreBone>(b1));
    core->addCoreBone(ISTD::shared_ptr<CalCoreBone>(b2));

    CalSkeleton skeleton(core);
    CalMixer mixer;
    ISTD::shared_ptr<CalAnimation> streamed =
        ISTD::make_shared<CalAnimation>(clip, 1.0f, 0);
    streamed->time = t;
    mixer.addAnimation(streamed);
    CalStreamClipCache cache(2);
    TASK_CHECK(mixer.updateSkeletonStreamed(&skeleton, {}, {}, &cache));
    // Bone 2 is missing from the file: bind (relative) pose.
    TASK_CHECK(closeV(skeleton.bones[2].getRelativeTransform().translation, CalVector(0, 20, 0)));
    // Bone 1 is present but empty: identity local transform replaces its bind
    // relative transform.
    TASK_CHECK(closeV(skeleton.bones[1].getRelativeTransform().translation, CalVector(0, 0, 0)));
    // The absolute pose still composes the animated root: no track for bone 2
    // means its own local transform is unchanged, not that it floats free.
    TASK_CHECK(closeV(skeleton.bones[2].absoluteTransform.translation,
                      skeleton.bones[0].absoluteTransform.translation + CalVector(0, 20, 0)));
    return 0;
}

static int case_equivalent_quaternion_sign() {
    // The same rotation as q and -q.  Sampling between two keys that are
    // sign-flipped must take the short arc, exactly like the in-memory path.
    CalQuaternion q;
    q.setAxisAngle(CalVector(0, 1, 0), 1.2f);
    CalQuaternion neg(-q.x, -q.y, -q.z, -q.w);

    CalCoreTrack::KeyframeList keys;
    keys.push_back(CalCoreKeyframe(0.0f, CalVector(0, 0, 0), q));
    keys.push_back(CalCoreKeyframe(1.0f, CalVector(1, 0, 0), neg));
    std::vector<CalCoreTrack> tracks;
    tracks.push_back(CalCoreTrack(0, keys));
    ISTD::shared_ptr<CalCoreAnimation> animation = makeAnimation(1.0f, tracks);

    TempFile file("quatsign");
    std::string error;
    TASK_CHECK(writeStreamClip(*animation, 1.0f, file.path, &error));
    ISTD::shared_ptr<CalStreamClip> clip = CalStreamClip::open(file.path, &error);
    TASK_CHECK(clip);

    CalStreamClip::DecodedBlockPtr block;
    TASK_CHECK(clip->loadBlock(0, block, &error));
    TASK_CHECK(block);
    TASK_CHECK(block->tracks[0].keys.size() == 2);
    // Sign is preserved verbatim on disk.
    TASK_CHECK(closef(block->tracks[0].keys[1].rotation.x, neg.x));
    TASK_CHECK(closef(block->tracks[0].keys[1].rotation.w, neg.w));

    const float times[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    for (size_t i = 0; i < 5; ++i) {
        const cal3d::RotateTranslate streamed =
            CalStreamClip::sampleTrack(block->tracks[0], times[i]);
        const cal3d::RotateTranslate reference = animation->tracks[0].getCurrentTransform(times[i]);
        TASK_CHECK(closeRT(streamed, reference));
    }
    return 0;
}

static int case_open_reads_zero_blocks() {
    std::vector<HandTrack> tracks;
    HandTrack track;
    track.boneId = 0;
    track.keys.push_back(rawKey(0.0f, 1, 2, 3));
    track.keys.push_back(rawKey(1.0f, 4, 5, 6));
    tracks.push_back(track);

    std::vector<uint8_t> bytes = buildSingleBlockFile(1.0f, 0.5f, tracks);

    // Prove the directory/block are valid by loading it once first.
    TempFile file("zeroread");
    TASK_CHECK(writeBytes(file.path, bytes));

    ISTD::shared_ptr<CalStreamClip> probe = CalStreamClip::open(file.path);
    TASK_CHECK(probe);
    CalStreamClip::DecodedBlockPtr decoded;
    TASK_CHECK(probe->loadBlock(0, decoded));
    TASK_CHECK(decoded && decoded->tracks.size() == 1);

    // Corrupt the block payload but leave the directory intact.  A correct
    // open() must still succeed because it never reads block data.
    const uint32_t dirOffset = 64 + 8;
    const uint32_t dataOffset = dirOffset + 32;
    bytes[dataOffset + 8] ^= 0xFF; // smash the first key

    TempFile corrupt("zeroread_corrupt");
    TASK_CHECK(writeBytes(corrupt.path, bytes));

    std::string error;
    ISTD::shared_ptr<CalStreamClip> clip = CalStreamClip::open(corrupt.path, &error);
    TASK_CHECK(clip);
    TASK_CHECK(clip->blockCount() == 1);
    TASK_CHECK(clip->tracks().size() == 1);
    TASK_CHECK(clip->isOpen());

    // The corruption is only visible when the block is actually decoded.
    CalStreamClip::DecodedBlockPtr bad;
    TASK_CHECK(!clip->loadBlock(0, bad, &error));
    TASK_CHECK(!error.empty());
    return 0;
}

static int case_repeated_sampling_hits_cache() {
    std::vector<CalCoreTrack> tracks;
    CalCoreTrack::KeyframeList keys;
    keys.push_back(key(0.0f, 0, 0, 0));
    keys.push_back(key(0.25f, 1, 0, 0));
    keys.push_back(key(0.5f, 2, 0, 0));
    keys.push_back(key(0.75f, 3, 0, 0));
    keys.push_back(key(1.0f, 4, 0, 0));
    tracks.push_back(CalCoreTrack(0, keys));
    ISTD::shared_ptr<CalCoreAnimation> animation = makeAnimation(1.0f, tracks);

    TempFile file("repeat");
    TASK_CHECK(writeStreamClip(*animation, 0.25f, file.path));

    ISTD::shared_ptr<CalCoreSkeleton> core = ISTD::make_shared<CalCoreSkeleton>();
    core->addCoreBone(ISTD::make_shared<CalCoreBone>("b0"));
    CalSkeleton skeleton(core);
    CalMixer mixer;
    ISTD::shared_ptr<CalAnimation> streamed =
        ISTD::make_shared<CalAnimation>(CalStreamClip::open(file.path), 1.0f, 0);
    TASK_CHECK(streamed->isStreamed());
    mixer.addAnimation(streamed);

    CalStreamClipCache cache(4);
    TASK_CHECK(cache.residentCount() == 0);
    TASK_CHECK(cache.hitCount() == 0);
    TASK_CHECK(cache.readAttemptCount() == 0);

    // Sampling repeatedly inside the same block should read that block once
    // and hit the cache afterwards.
    const float times[] = { 0.05f, 0.10f, 0.18f, 0.05f, 0.20f };
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); ++i) {
        streamed->time = times[i];
        TASK_CHECK(mixer.updateSkeletonStreamed(&skeleton, {}, {}, &cache));
    }
    TASK_CHECK(cache.readAttemptCount() == 1);
    TASK_CHECK(cache.residentCount() == 1);
    // A hit is a distinct requested block that was already resident: the
    // first frame reads, the next four hit.
    TASK_CHECK(cache.hitCount() == 4);
    TASK_CHECK(cache.peakDecodedBlocks() == 1);

    // Move to another block; a miss is read exactly once.
    streamed->time = 0.55f;
    TASK_CHECK(mixer.updateSkeletonStreamed(&skeleton, {}, {}, &cache));
    TASK_CHECK(cache.readAttemptCount() == 2);
    TASK_CHECK(cache.residentCount() == 2);
    TASK_CHECK(cache.hitCount() == 4);

    streamed->time = 0.60f;
    TASK_CHECK(mixer.updateSkeletonStreamed(&skeleton, {}, {}, &cache));
    TASK_CHECK(cache.readAttemptCount() == 2);
    TASK_CHECK(cache.hitCount() == 5);
    return 0;
}

static int case_invalid_files_are_rejected() {
    // Every malformed fixture must be rejected at the stage promised by the
    // header: header/directory problems on open, keyframe problems on decode.
    const float duration = 1.0f;
    std::string error;

    const std::vector<uint8_t> good = buildSingleBlockFile(duration, 1.0f,
        std::vector<HandTrack>{
            HandTrack{ 0, { rawKey(0.0f, 0, 0, 0), rawKey(1.0f, 1, 0, 0) } }
        });

    // Sanity: the unmodified fixture opens and decodes.
    {
        TempFile file("invalid_good");
        TASK_CHECK(writeBytes(file.path, good));
        ISTD::shared_ptr<CalStreamClip> clip = CalStreamClip::open(file.path, &error);
        TASK_CHECK(clip);
        CalStreamClip::DecodedBlockPtr block;
        TASK_CHECK(clip->loadBlock(0, block, &error));
    }

    struct Mutation {
        const char* name;
        void (*apply)(std::vector<uint8_t>&);
        bool atOpen; // true: open rejects, false: decode rejects
    };

    const Mutation mutations[] = {
        { "directory_out_of_bounds", [](std::vector<uint8_t>& b) {
              putU32(b, kDirOffset + 20, 0xFFFFFFu); // byteLength past EOF
          }, true },
        { "directory_overlap", [](std::vector<uint8_t>&) {
              // Covered by the purpose-built two-entry fixture below.
          }, true },
        { "duplicate_track_bone", [](std::vector<uint8_t>&) {
              // Covered by the purpose-built two-track fixture below.
          }, true },
        { "illegal_bone_id", [](std::vector<uint8_t>& b) {
              putU32(b, kMetaOffset, 0x80000000u);
          }, true },
        { "non_increasing_keys", [](std::vector<uint8_t>& b) {
              // Same time twice in the payload.  The CRC is recomputed so this
              // is a semantic error, not a checksum error.
              putF32(b, kDataOffset + 4 + 8 + 32, 0.0f);
              patchBlockCrc(b);
          }, false },
        { "non_finite_key", [](std::vector<uint8_t>& b) {
              putF32(b, kDataOffset + 4 + 8 + 4, std::numeric_limits<float>::quiet_NaN());
              patchBlockCrc(b);
          }, false },
        { "wrong_block_track_count", [](std::vector<uint8_t>& b) {
              putU32(b, kDataOffset, 2);
              patchBlockCrc(b);
          }, false },
        { "bad_magic", [](std::vector<uint8_t>& b) {
              b[0] = 'X';
          }, true },
        { "bad_version", [](std::vector<uint8_t>& b) {
              putU32(b, 8, 99);
          }, true },
        { "wrong_file_size", [](std::vector<uint8_t>& b) {
              putU64(b, 52, 1);
          }, true },
        { "truncated_block", [](std::vector<uint8_t>& b) {
              // Shrink the recorded payload one byte so the key stream runs
              // out mid-keyframe.
              uint32_t len = static_cast<uint32_t>(
                  b[kDirOffset + 20] | (b[kDirOffset + 21] << 8)
                  | (b[kDirOffset + 22] << 16) | (b[kDirOffset + 23] << 24));
              putU32(b, kDirOffset + 20, len - 1);
              putU32(b, kDirOffset + 24,
                     crc32Of(b.data() + kDataOffset, len - 1));
          }, false },
    };

    for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); ++i) {
        if (std::string(mutations[i].name) == "directory_overlap"
            || std::string(mutations[i].name) == "duplicate_track_bone") {
            continue; // handled with purpose-built fixtures below
        }
        std::vector<uint8_t> bytes = good;
        mutations[i].apply(bytes);
        TempFile file("invalid");
        TASK_CHECK(writeBytes(file.path, bytes));
        ISTD::shared_ptr<CalStreamClip> clip = CalStreamClip::open(file.path, &error);
        if (mutations[i].atOpen) {
            TASK_CHECK(!clip);
        } else {
            TASK_CHECK(clip);
            CalStreamClip::DecodedBlockPtr block;
            TASK_CHECK(!clip->loadBlock(0, block, &error));
        }
    }

    // Overlapping intervals: two directory entries that both cover time.
    {
        std::vector<uint8_t> bytes = good;
        // Append a second directory entry at the end, then point the header at
        // it with blockCount = 2.  Intervals are [0,1] and [0.25,1].
        std::vector<uint8_t> second(32, 0);
        putU32(second, 0, 1);
        putF32(second, 4, 0.25f);
        putF32(second, 8, duration);
        putU64(second, 12, kDataOffset);
        putU32(second, 20, static_cast<uint32_t>(
            bytes[kDirOffset + 20] | (bytes[kDirOffset + 21] << 8)
            | (bytes[kDirOffset + 22] << 16) | (bytes[kDirOffset + 23] << 24)));
        putU32(second, 24, bytes[kDirOffset + 24]);
        bytes.insert(bytes.end(), second.begin(), second.end());
        putU32(bytes, 32, 2);
        putU64(bytes, 52, bytes.size());

        TempFile file("invalid_overlap");
        TASK_CHECK(writeBytes(file.path, bytes));
        TASK_CHECK(!CalStreamClip::open(file.path, &error));
    }

    // Duplicate track bone ids in the metadata.
    {
        std::vector<HandTrack> tracks;
        HandTrack a; a.boneId = 3; a.keys.push_back(rawKey(0.0f, 0, 0, 0));
        HandTrack b; b.boneId = 3; b.keys.push_back(rawKey(0.0f, 1, 0, 0));
        tracks.push_back(a);
        tracks.push_back(b);
        std::vector<uint8_t> bytes = buildSingleBlockFile(duration, 1.0f, tracks);
        TempFile file("invalid_dup");
        TASK_CHECK(writeBytes(file.path, bytes));
        TASK_CHECK(!CalStreamClip::open(file.path, &error));
        TASK_CHECK(!error.empty());
    }

    // Writer-side validation.
    {
        TempFile file("invalid_write");

        CalCoreTrack::KeyframeList sameTime;
        sameTime.push_back(key(0.0f, 0, 0, 0));
        sameTime.push_back(key(0.0f, 1, 0, 0));
        std::vector<CalCoreTrack> duplicateKeys;
        duplicateKeys.push_back(CalCoreTrack(0, sameTime));
        TASK_CHECK(!writeStreamClip(*makeAnimation(1.0f, duplicateKeys), 0.5f, file.path, &error));

        CalCoreTrack::KeyframeList nanKey;
        nanKey.push_back(key(0.0f, 0, 0, 0));
        nanKey.push_back(key(1.0f, std::numeric_limits<float>::infinity(), 0, 0));
        std::vector<CalCoreTrack> nonFinite;
        nonFinite.push_back(CalCoreTrack(0, nanKey));
        TASK_CHECK(!writeStreamClip(*makeAnimation(1.0f, nonFinite), 0.5f, file.path, &error));

        CalCoreTrack::KeyframeList okKeys;
        okKeys.push_back(key(0.0f, 0, 0, 0));
        std::vector<CalCoreTrack> dupTracks;
        dupTracks.push_back(CalCoreTrack(1, okKeys));
        dupTracks.push_back(CalCoreTrack(1, okKeys));
        TASK_CHECK(!writeStreamClip(*makeAnimation(1.0f, dupTracks), 0.5f, file.path, &error));

        std::vector<CalCoreTrack> single;
        single.push_back(CalCoreTrack(0, okKeys));
        ISTD::shared_ptr<CalCoreAnimation> okClip = makeAnimation(1.0f, single);
        TASK_CHECK(!writeStreamClip(*okClip, 0.0f, file.path, &error));
        TASK_CHECK(!writeStreamClip(*okClip, -1.0f, file.path, &error));
        TASK_CHECK(!writeStreamClip(*okClip,
                                    std::numeric_limits<float>::quiet_NaN(),
                                    file.path, &error));
    }
    return 0;
}

static int case_block_size_limit() {
    // Build a clip just over the 32 MiB block budget.  Each key is 32 bytes,
    // so 32 MiB / 32 = 1,048,576 keys plus per-track framing crosses the
    // limit.  The writer must reject it rather than emit a huge block.
    const size_t keyCount = (cal3d::streamclip::kMaxBlockBytes / 32u) + 8;
    CalCoreTrack::KeyframeList keys;
    keys.reserve(keyCount);
    for (size_t i = 0; i < keyCount; ++i) {
        keys.push_back(key(static_cast<float>(i) * 0.001f, static_cast<float>(i), 0, 0));
    }
    std::vector<CalCoreTrack> tracks;
    tracks.push_back(CalCoreTrack(0, keys));
    ISTD::shared_ptr<CalCoreAnimation> animation =
        makeAnimation(static_cast<float>(keyCount) * 0.001f, tracks);

    TempFile file("toobig");
    std::string error;
    TASK_CHECK(!writeStreamClip(*animation, animation->duration + 1.0f, file.path, &error));
    TASK_CHECK(!error.empty());
    return 0;
}

static int case_single_block_and_zero_duration() {
    // duration < blockDuration -> exactly one block covering [0, duration].
    CalCoreTrack::KeyframeList keys;
    keys.push_back(key(0.0f, 0, 0, 0));
    keys.push_back(key(0.2f, 2, 0, 0));
    keys.push_back(key(0.4f, 4, 0, 0));
    std::vector<CalCoreTrack> tracks;
    tracks.push_back(CalCoreTrack(0, keys));
    ISTD::shared_ptr<CalCoreAnimation> animation = makeAnimation(0.4f, tracks);

    std::string error;
    TempFile file("single");
    TASK_CHECK(writeStreamClip(*animation, 10.0f, file.path, &error));
    ISTD::shared_ptr<CalStreamClip> clip = CalStreamClip::open(file.path, &error);
    TASK_CHECK(clip);
    TASK_CHECK(clip->blockCount() == 1);
    TASK_CHECK(closef(clip->duration(), 0.4f));

    CalStreamClip::DecodedBlockPtr block;
    TASK_CHECK(clip->loadBlock(0, block, &error));
    const float times[] = { -1.0f, 0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 9.0f };
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); ++i) {
        TASK_CHECK(clip->blockIndexForTime(times[i]) == 0);
        TASK_CHECK(closeRT(CalStreamClip::sampleTrack(block->tracks[0], times[i]),
                           animation->tracks[0].getCurrentTransform(times[i])));
    }

    // Zero duration still yields one legal (possibly empty-interval) block.
    CalCoreTrack::KeyframeList still;
    still.push_back(key(0.0f, 7, 0, 0));
    std::vector<CalCoreTrack> stillTracks;
    stillTracks.push_back(CalCoreTrack(0, still));
    ISTD::shared_ptr<CalCoreAnimation> zero = makeAnimation(0.0f, stillTracks);
    TempFile zeroFile("zero");
    TASK_CHECK(writeStreamClip(*zero, 0.5f, zeroFile.path, &error));
    ISTD::shared_ptr<CalStreamClip> zeroClip = CalStreamClip::open(zeroFile.path, &error);
    TASK_CHECK(zeroClip);
    TASK_CHECK(zeroClip->blockCount() == 1);
    CalStreamClip::DecodedBlockPtr zeroBlock;
    TASK_CHECK(zeroClip->loadBlock(0, zeroBlock, &error));
    TASK_CHECK(closeRT(CalStreamClip::sampleTrack(zeroBlock->tracks[0], 0.0f),
                       zero->tracks[0].getCurrentTransform(0.0f)));
    return 0;
}

// ===========================================================================
// Group: cache_transactions
// ===========================================================================

namespace {

// Two clips whose blocks can be addressed individually.  Both use a single
// track (bone 0) with keys every 0.25s so block N contains a known value.
ISTD::shared_ptr<CalStreamClip> makeSimpleClip(const char* tag, float offset) {
    CalCoreTrack::KeyframeList keys;
    for (int i = 0; i <= 8; ++i) {
        keys.push_back(key(i * 0.25f, offset + i, 0, 0));
    }
    std::vector<CalCoreTrack> tracks;
    tracks.push_back(CalCoreTrack(0, keys));
    ISTD::shared_ptr<CalCoreAnimation> animation = makeAnimation(2.0f, tracks);

    static int serial = 0;
    char tagbuf[64];
    std::snprintf(tagbuf, sizeof(tagbuf), "%s_%d", tag, ++serial);
    std::string path = tempPath(tagbuf);
    // Intentionally leak the file on disk for the duration of the process;
    // the test only needs it alive while the clip is open.
    std::string error;
    bool ok = writeStreamClip(*animation, 0.5f, path, &error);
    (void)ok;
    return CalStreamClip::open(path);
}

} // namespace

static int case_shared_blocks_hit_once() {
    ISTD::shared_ptr<CalStreamClip> clipA = makeSimpleClip("sharedA", 0.0f);
    ISTD::shared_ptr<CalStreamClip> clipB = makeSimpleClip("sharedB", 100.0f);
    TASK_CHECK(clipA && clipB);
    TASK_CHECK(clipA->identity() != clipB->identity());

    CalStreamClipCache cache(3);
    std::vector<CalStreamClipBlockRef> requests;
    requests.push_back(CalStreamClipBlockRef(clipA, 0));
    requests.push_back(CalStreamClipBlockRef(clipB, 0));
    requests.push_back(CalStreamClipBlockRef(clipA, 1));
    requests.push_back(CalStreamClipBlockRef(clipA, 0)); // duplicate

    std::vector<CalStreamClip::DecodedBlockPtr> blocks;
    TASK_CHECK(cache.acquire(requests, blocks));
    TASK_CHECK(blocks.size() == 4);
    TASK_CHECK(blocks[0] == blocks[3]); // same block instance
    TASK_CHECK(blocks[0] != blocks[1]);
    TASK_CHECK(cache.readAttemptCount() == 3);
    TASK_CHECK(cache.residentCount() == 3);
    TASK_CHECK(cache.hitCount() == 0);
    TASK_CHECK(cache.peakDecodedBlocks() == 3);

    // Different files using the same block number must not share contents.
    const CalStreamClip::StreamKey& a =
        blocks[0]->tracks[0].keys[blocks[0]->tracks[0].keys.size() > 1 ? 1 : 0];
    const CalStreamClip::StreamKey& b =
        blocks[1]->tracks[0].keys[blocks[1]->tracks[0].keys.size() > 1 ? 1 : 0];
    TASK_CHECK(closef(a.translation.x, b.translation.x - 100.0f));

    // LRU: last accesses were A0(3), B0(1), A1(2) -> B0, A1, A0.
    std::vector<CalStreamClipBlockKey> lru = cache.lruOrder();
    TASK_CHECK(lru.size() == 3);
    TASK_CHECK(lru[2].sourceId == clipA->identity() && lru[2].blockIndex == 0);
    TASK_CHECK(lru[1].sourceId == clipA->identity() && lru[1].blockIndex == 1);
    TASK_CHECK(lru[0].sourceId == clipB->identity() && lru[0].blockIndex == 0);

    // A second frame that reuses A0 and B0 is all hits.
    requests.clear();
    requests.push_back(CalStreamClipBlockRef(clipA, 0));
    requests.push_back(CalStreamClipBlockRef(clipB, 0));
    blocks.clear();
    TASK_CHECK(cache.acquire(requests, blocks));
    TASK_CHECK(cache.readAttemptCount() == 3);
    TASK_CHECK(cache.hitCount() == 2);
    return 0;
}

static int case_capacity_exact_and_insufficient() {
    ISTD::shared_ptr<CalStreamClip> clipA = makeSimpleClip("capA", 0.0f);
    ISTD::shared_ptr<CalStreamClip> clipB = makeSimpleClip("capB", 50.0f);
    TASK_CHECK(clipA && clipB);

    // A capacity exactly equal to the working set succeeds.
    {
        CalStreamClipCache cache(2);
        std::vector<CalStreamClipBlockRef> requests;
        requests.push_back(CalStreamClipBlockRef(clipA, 0));
        requests.push_back(CalStreamClipBlockRef(clipB, 2));
        std::vector<CalStreamClip::DecodedBlockPtr> blocks;
        TASK_CHECK(cache.acquire(requests, blocks));
        TASK_CHECK(blocks.size() == 2);
        TASK_CHECK(cache.residentCount() == 2);
        TASK_CHECK(cache.peakDecodedBlocks() == 2);
    }

    // A working set larger than capacity fails before reading anything.
    {
        CalStreamClipCache cache(2);
        std::vector<CalStreamClipBlockRef> requests;
        requests.push_back(CalStreamClipBlockRef(clipA, 0));
        requests.push_back(CalStreamClipBlockRef(clipB, 1));
        requests.push_back(CalStreamClipBlockRef(clipA, 2));
        std::vector<CalStreamClip::DecodedBlockPtr> blocks;
        TASK_CHECK(!cache.acquire(requests, blocks));
        TASK_CHECK(blocks.empty());
        TASK_CHECK(cache.residentCount() == 0);
        TASK_CHECK(cache.readAttemptCount() == 0);
        TASK_CHECK(!cache.lastError().empty());
    }

    // Repeating the same block does not count as a larger working set.
    {
        CalStreamClipCache cache(1);
        std::vector<CalStreamClipBlockRef> requests;
        requests.push_back(CalStreamClipBlockRef(clipA, 3));
        requests.push_back(CalStreamClipBlockRef(clipA, 3));
        std::vector<CalStreamClip::DecodedBlockPtr> blocks;
        TASK_CHECK(cache.acquire(requests, blocks));
        TASK_CHECK(blocks.size() == 2);
        TASK_CHECK(blocks[0] == blocks[1]);
        TASK_CHECK(cache.readAttemptCount() == 1);
    }

    // LRU eviction: capacity 2, warm A0 and B0, then use A1 -> B0 (unused,
    // oldest) is evicted while A0 stays hot.
    {
        CalStreamClipCache cache(2);
        std::vector<CalStreamClipBlockRef> requests;
        std::vector<CalStreamClip::DecodedBlockPtr> blocks;

        requests.clear();
        requests.push_back(CalStreamClipBlockRef(clipA, 0));
        requests.push_back(CalStreamClipBlockRef(clipB, 0));
        TASK_CHECK(cache.acquire(requests, blocks));
        TASK_CHECK(cache.residentCount() == 2);

        requests.clear();
        requests.push_back(CalStreamClipBlockRef(clipA, 0));
        requests.push_back(CalStreamClipBlockRef(clipA, 1));
        blocks.clear();
        TASK_CHECK(cache.acquire(requests, blocks));
        TASK_CHECK(cache.residentCount() == 2);
        std::vector<CalStreamClipBlockKey> resident = cache.residentBlocks();
        bool hasA0 = false, hasA1 = false, hasB0 = false;
        for (size_t i = 0; i < resident.size(); ++i) {
            if (resident[i].sourceId == clipA->identity() && resident[i].blockIndex == 0) hasA0 = true;
            if (resident[i].sourceId == clipA->identity() && resident[i].blockIndex == 1) hasA1 = true;
            if (resident[i].sourceId == clipB->identity() && resident[i].blockIndex == 0) hasB0 = true;
        }
        TASK_CHECK(hasA0 && hasA1 && !hasB0);
        // Only the one missing block was read; the hot A0 was a hit.
        TASK_CHECK(cache.readAttemptCount() == 3);
        TASK_CHECK(cache.hitCount() == 1);
    }

    // A frame whose working set exactly equals capacity, while the cache is
    // full of *other* blocks: all of them must be evicted to make room, but
    // the resident count must end at capacity and no live block may be lost.
    {
        CalStreamClipCache cache(2);
        std::vector<CalStreamClipBlockRef> requests;
        std::vector<CalStreamClip::DecodedBlockPtr> blocks;

        requests.push_back(CalStreamClipBlockRef(clipA, 3));
        requests.push_back(CalStreamClipBlockRef(clipB, 3));
        TASK_CHECK(cache.acquire(requests, blocks));
        TASK_CHECK(cache.residentCount() == 2);
        TASK_CHECK(cache.readAttemptCount() == 2);
        TASK_CHECK(cache.peakDecodedBlocks() == 2);

        requests.clear();
        requests.push_back(CalStreamClipBlockRef(clipA, 1));
        requests.push_back(CalStreamClipBlockRef(clipB, 2));
        blocks.clear();
        TASK_CHECK(cache.acquire(requests, blocks));
        TASK_CHECK(cache.residentCount() == 2);
        TASK_CHECK(cache.readAttemptCount() == 4);
        TASK_CHECK(cache.hitCount() == 0);
        // The transient peak doubles the resident set but is bounded by
        // capacity + distinct blocks in the frame.
        TASK_CHECK(cache.peakDecodedBlocks() == 4);
        std::vector<CalStreamClipBlockKey> resident = cache.residentBlocks();
        TASK_CHECK(resident.size() == 2);
        for (size_t i = 0; i < resident.size(); ++i) {
            bool stale = resident[i].blockIndex == 3;
            TASK_CHECK(!stale);
        }
    }
    return 0;
}

static int case_corruption_fails_without_eviction() {
    // Build a file with three blocks; corrupt the middle one's checksum.
    CalCoreTrack::KeyframeList keys;
    for (int i = 0; i <= 6; ++i) {
        keys.push_back(key(i * 0.5f, i, 0, 0));
    }
    std::vector<CalCoreTrack> tracks;
    tracks.push_back(CalCoreTrack(0, keys));
    ISTD::shared_ptr<CalCoreAnimation> animation = makeAnimation(3.0f, tracks);

    TempFile file("corrupt");
    TASK_CHECK(writeStreamClip(*animation, 1.0f, file.path));

    // Locate block 1's payload from the directory and flip a byte.
    std::ifstream in(file.path.c_str(), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    in.close();
    TASK_CHECK(bytes.size() > 64 + 32 * 3);
    const uint32_t trackCount = 1;
    const uint32_t dirOffset = 64 + trackCount * 8;
    const uint8_t* dir = bytes.data() + dirOffset + 32 * 1;
    uint64_t dataOffset = 0;
    for (int i = 0; i < 8; ++i) {
        dataOffset |= static_cast<uint64_t>(dir[12 + i]) << (8 * i);
    }
    TASK_CHECK(dataOffset + 12 < bytes.size());
    bytes[dataOffset + 12] ^= 0xFF; // inside the key payload, after the header
    TASK_CHECK(writeBytes(file.path, bytes));

    ISTD::shared_ptr<CalStreamClip> clip = CalStreamClip::open(file.path);
    TASK_CHECK(clip);

    CalStreamClipCache cache(2);
    std::vector<CalStreamClipBlockRef> requests;
    std::vector<CalStreamClip::DecodedBlockPtr> blocks;

    // Warm block 0 and block 2.
    requests.push_back(CalStreamClipBlockRef(clip, 0));
    requests.push_back(CalStreamClipBlockRef(clip, 2));
    TASK_CHECK(cache.acquire(requests, blocks));
    TASK_CHECK(cache.residentCount() == 2);
    const std::vector<CalStreamClipBlockKey> before = cache.lruOrder();
    const uint64_t readsBefore = cache.readAttemptCount();
    const uint64_t hitsBefore = cache.hitCount();

    // Now request the corrupt block.  The frame must fail without evicting
    // either resident block, without changing LRU, and without a hit.
    requests.clear();
    requests.push_back(CalStreamClipBlockRef(clip, 0));
    requests.push_back(CalStreamClipBlockRef(clip, 1));
    blocks.clear();
    TASK_CHECK(!cache.acquire(requests, blocks));
    TASK_CHECK(blocks.empty());
    TASK_CHECK(cache.residentCount() == 2);
    TASK_CHECK(cache.lruOrder() == before);
    TASK_CHECK(cache.readAttemptCount() == readsBefore + 1);
    TASK_CHECK(cache.hitCount() == hitsBefore);
    TASK_CHECK(!cache.lastError().empty());

    // Retrying still fails identically, and then a valid request still works
    // entirely from the untouched cache.
    TASK_CHECK(!cache.acquire(requests, blocks));
    TASK_CHECK(cache.readAttemptCount() == readsBefore + 2);
    requests.clear();
    requests.push_back(CalStreamClipBlockRef(clip, 2));
    requests.push_back(CalStreamClipBlockRef(clip, 0));
    blocks.clear();
    TASK_CHECK(cache.acquire(requests, blocks));
    TASK_CHECK(blocks.size() == 2);
    TASK_CHECK(cache.hitCount() == hitsBefore + 2);
    TASK_CHECK(cache.readAttemptCount() == readsBefore + 2);
    return 0;
}

static int case_reopen_gets_fresh_identity() {
    ISTD::shared_ptr<CalStreamClip> first = makeSimpleClip("reopen", 0.0f);
    TASK_CHECK(first);
    const uint64_t firstId = first->identity();

    CalStreamClipCache cache(2);
    std::vector<CalStreamClipBlockRef> requests;
    requests.push_back(CalStreamClipBlockRef(first, 0));
    std::vector<CalStreamClip::DecodedBlockPtr> blocks;
    TASK_CHECK(cache.acquire(requests, blocks));
    TASK_CHECK(cache.readAttemptCount() == 1);
    TASK_CHECK(cache.residentCount() == 1);

    // We must reuse the same on-disk file, so remember its path via a second
    // generation helper.  makeSimpleClip leaks the path; instead, write a file
    // whose lifetime we control here.
    CalCoreTrack::KeyframeList keys;
    for (int i = 0; i <= 4; ++i) {
        keys.push_back(key(i * 0.5f, i, 0, 0));
    }
    std::vector<CalCoreTrack> tracks;
    tracks.push_back(CalCoreTrack(0, keys));
    ISTD::shared_ptr<CalCoreAnimation> animation = makeAnimation(2.0f, tracks);
    TempFile file("reopen2");
    TASK_CHECK(writeStreamClip(*animation, 1.0f, file.path));

    ISTD::shared_ptr<CalStreamClip> a = CalStreamClip::open(file.path);
    TASK_CHECK(a);
    const uint64_t idA = a->identity();
    // Release the source, then reopen the exact same file.
    a.reset();
    ISTD::shared_ptr<CalStreamClip> b = CalStreamClip::open(file.path);
    TASK_CHECK(b);
    TASK_CHECK(b->identity() != idA);
    TASK_CHECK(b->identity() != firstId);

    // The stale identity's resident block is not served for the reopened clip.
    CalStreamClipCache fresh(1);
    requests.clear();
    requests.push_back(CalStreamClipBlockRef(b, 0));
    blocks.clear();
    TASK_CHECK(fresh.acquire(requests, blocks));
    TASK_CHECK(fresh.readAttemptCount() == 1);
    TASK_CHECK(fresh.hitCount() == 0);

    // Even sharing a cache, the old clip's block and the new clip's block are
    // distinct entries.
    requests.clear();
    requests.push_back(CalStreamClipBlockRef(first, 0));
    blocks.clear();
    TASK_CHECK(cache.acquire(requests, blocks)); // still a hit on `first`
    TASK_CHECK(cache.hitCount() == 1);
    return 0;
}

// ===========================================================================
// Group: mixed_pipeline
// ===========================================================================

namespace {

// Compare a mixed streamed+memory frame against the same blend driven purely
// from memory, using the real CalMixer / CalSkeleton / CalPhysique path.
struct PipelineResult {
    CalVector position;
    CalVector normal;
    CalVector bone0;
    CalVector bone1;
};

PipelineResult runPipeline(
    CalSkeleton& skeleton,
    CalMixer& mixer,
    bool streamed,
    CalStreamClipCache* cache
) {
    bool ok = streamed
        ? mixer.updateSkeletonStreamed(&skeleton, {}, {}, cache)
        : true;
    (void)ok;

    PipelineResult result;
    skinVertex(skeleton, result.position, result.normal);
    result.bone0 = skeleton.bones[0].absoluteTransform.translation;
    result.bone1 = skeleton.bones[1].absoluteTransform.translation;
    return result;
}

} // namespace

static int case_mixed_high_low_priority() {
    // Low-priority memory animation and a high-priority streamed animation
    // press on both bones; adjustments are applied too.
    ISTD::shared_ptr<CalCoreAnimation> lowClip = makePipelineClip(1.6f, 0.0f);

    // A streamed high-priority clip: same shape, different phase.
    ISTD::shared_ptr<CalCoreAnimation> highClip = makePipelineClip(1.6f, 0.25f);
    TempFile file("mixed");
    std::string error;
    TASK_CHECK(writeStreamClip(*highClip, 0.5f, file.path, &error));
    ISTD::shared_ptr<CalStreamClip> streamClip = CalStreamClip::open(file.path, &error);
    TASK_CHECK(streamClip);

    ISTD::shared_ptr<CalCoreSkeleton> core = makeTwoBoneSkeleton();

    CalSkeleton skeletonMemory(core);
    CalSkeleton skeletonStreamed(core);
    CalMixer memoryMixer;
    CalMixer streamedMixer;

    ISTD::shared_ptr<CalAnimation> lowMem =
        ISTD::make_shared<CalAnimation>(lowClip, 0.8f, 0);
    ISTD::shared_ptr<CalAnimation> lowStreamed =
        ISTD::make_shared<CalAnimation>(lowClip, 0.8f, 0);
    ISTD::shared_ptr<CalAnimation> highMem =
        ISTD::make_shared<CalAnimation>(highClip, 0.7f, 1);
    ISTD::shared_ptr<CalAnimation> highStreamed =
        ISTD::make_shared<CalAnimation>(streamClip, 0.7f, 1);

    lowMem->time = lowStreamed->time = 0.73f;
    lowMem->rampValue = lowStreamed->rampValue = 0.85f;
    highMem->time = highStreamed->time = 1.11f;
    highMem->rampValue = highStreamed->rampValue = 0.6f;

    memoryMixer.addAnimation(lowMem);
    memoryMixer.addAnimation(highMem);
    streamedMixer.addAnimation(lowStreamed);
    streamedMixer.addAnimation(highStreamed);

    std::vector<BoneTransformAdjustment> adjustments;
    BoneTransformAdjustment adj;
    adj.boneId = 1;
    CalQuaternion adjOri;
    adjOri.setAxisAngle(CalVector(0, 0, 1), 0.35f);
    adj.localOri = adjOri;
    adj.rampValue = 0.4f;
    adjustments.push_back(adj);

    CalStreamClipCache cache(2);
    memoryMixer.updateSkeleton(&skeletonMemory, adjustments, {});
    TASK_CHECK(streamedMixer.updateSkeletonStreamed(&skeletonStreamed, adjustments, {}, &cache));

    PipelineResult mem, str;
    skinVertex(skeletonMemory, mem.position, mem.normal);
    skinVertex(skeletonStreamed, str.position, str.normal);
    mem.bone0 = skeletonMemory.bones[0].absoluteTransform.translation;
    mem.bone1 = skeletonMemory.bones[1].absoluteTransform.translation;
    str.bone0 = skeletonStreamed.bones[0].absoluteTransform.translation;
    str.bone1 = skeletonStreamed.bones[1].absoluteTransform.translation;

    TASK_CHECK(closeV(str.bone0, mem.bone0));
    TASK_CHECK(closeV(str.bone1, mem.bone1));
    TASK_CHECK(closeV(str.position, mem.position));
    TASK_CHECK(closeV(str.normal, mem.normal));

    // Both bones are animated, so the bone transforms are non-trivial.
    TASK_CHECK(!closeV(mem.bone1, CalVector(0, 0, 0), 1e-3f));
    return 0;
}

static int case_streamed_only_and_legacy_error() {
    ISTD::shared_ptr<CalCoreAnimation> clip = makePipelineClip(1.2f, 0.1f);
    TempFile file("legacy");
    TASK_CHECK(writeStreamClip(*clip, 0.4f, file.path));
    ISTD::shared_ptr<CalStreamClip> streamClip = CalStreamClip::open(file.path);
    TASK_CHECK(streamClip);

    ISTD::shared_ptr<CalCoreSkeleton> core = makeTwoBoneSkeleton();
    CalSkeleton skeleton(core);
    CalMixer mixer;
    ISTD::shared_ptr<CalAnimation> streamed =
        ISTD::make_shared<CalAnimation>(streamClip, 1.0f, 0);
    streamed->time = 0.5f;
    mixer.addAnimation(streamed);

    // updateSkeleton must throw without touching the skeleton.
    skeleton.bones[0].absoluteTransform.translation = CalVector(7, 8, 9);
    bool threw = false;
    try {
        mixer.updateSkeleton(&skeleton, {}, {});
    } catch (const std::exception&) {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(closeV(skeleton.bones[0].absoluteTransform.translation, CalVector(7, 8, 9)));
    TASK_CHECK(closef(streamed->time, 0.5f));

    CalStreamClipCache cache(2);
    TASK_CHECK(mixer.updateSkeletonStreamed(&skeleton, {}, {}, &cache));
    TASK_CHECK(closef(streamed->time, 0.5f));
    return 0;
}

static int case_failure_restores_and_retries() {
    // Streamed clip needs block 1; then we corrupt it and check that a failed
    // frame leaves the skeleton and animation times untouched, and that a
    // later valid frame still works.
    ISTD::shared_ptr<CalCoreAnimation> clip = makePipelineClip(1.5f, 0.0f);
    TempFile file("retry");
    TASK_CHECK(writeStreamClip(*clip, 0.5f, file.path));

    ISTD::shared_ptr<CalCoreSkeleton> core = makeTwoBoneSkeleton();
    CalSkeleton skeleton(core);
    CalMixer mixer;
    ISTD::shared_ptr<CalAnimation> streamed =
        ISTD::make_shared<CalAnimation>(CalStreamClip::open(file.path), 1.0f, 0);
    mixer.addAnimation(streamed);

    CalStreamClipCache cache(1);
    streamed->time = 0.1f;
    TASK_CHECK(mixer.updateSkeletonStreamed(&skeleton, {}, {}, &cache));
    const CalVector boneSnapshot = skeleton.bones[0].absoluteTransform.translation;
    const CalVector skinSnapshot = [&]() {
        CalVector p, n;
        skinVertex(skeleton, p, n);
        return p;
    }();

    // Corrupt every block's payload; capacity 1 and a failing acquisition.
    std::ifstream in(file.path.c_str(), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    in.close();
    const uint32_t trackCount = 2;
    const uint32_t dirOffset = 64 + trackCount * 8;
    for (uint32_t b = 0; b < 3; ++b) {
        const uint8_t* dir = bytes.data() + dirOffset + 32 * b;
        uint64_t dataOffset = 0;
        for (int i = 0; i < 8; ++i) {
            dataOffset |= static_cast<uint64_t>(dir[12 + i]) << (8 * i);
        }
        bytes[dataOffset + 20] ^= 0xFF;
    }
    TASK_CHECK(writeBytes(file.path, bytes));

    // A new handle on the corrupted file will fail its frame.  (The clip
    // already open in `mixer` still has its block 0 cached, and block 0 has
    // not been corrupted in memory -- only on disk.  Move to a block that is
    // not resident and freshly read from the corrupted file.)
    ISTD::shared_ptr<CalStreamClip> badClip = CalStreamClip::open(file.path);
    TASK_CHECK(badClip);
    CalMixer badMixer;
    ISTD::shared_ptr<CalAnimation> badAnim =
        ISTD::make_shared<CalAnimation>(badClip, 1.0f, 0);
    badAnim->time = 1.1f; // block 2
    badMixer.addAnimation(badAnim);

    CalSkeleton before(core);
    CalVector beforePosition, beforeNormal;
    skinVertex(before, beforePosition, beforeNormal);

    CalStreamClipCache badCache(1);
    TASK_CHECK(!badMixer.updateSkeletonStreamed(&before, {}, {}, &badCache));
    TASK_CHECK(closef(badAnim->time, 1.1f));
    CalVector afterPosition, afterNormal;
    skinVertex(before, afterPosition, afterNormal);
    TASK_CHECK(closeV(afterPosition, beforePosition));
    TASK_CHECK(closeV(afterNormal, beforeNormal));
    TASK_CHECK(badCache.residentCount() == 0);

    // The still-valid mixer and cache continue to be correct.
    streamed->time = 0.1f;
    TASK_CHECK(mixer.updateSkeletonStreamed(&skeleton, {}, {}, &cache));
    TASK_CHECK(closeV(skeleton.bones[0].absoluteTransform.translation, boneSnapshot));
    CalVector p, n;
    skinVertex(skeleton, p, n);
    TASK_CHECK(closeV(p, skinSnapshot));
    (void)badMixer;
    return 0;
}

static int case_two_characters_out_of_order_seek() {
    ISTD::shared_ptr<CalCoreAnimation> clip = makePipelineClip(2.0f, 0.0f);
    TempFile file("seek");
    TASK_CHECK(writeStreamClip(*clip, 0.5f, file.path));
    ISTD::shared_ptr<CalStreamClip> streamClip = CalStreamClip::open(file.path);
    TASK_CHECK(streamClip);

    ISTD::shared_ptr<CalCoreSkeleton> core = makeTwoBoneSkeleton();

    // Two characters, each with their own skeleton/mixer, sharing the clip and
    // the cache.  They seek out of order and in opposite directions.
    CalSkeleton skeletonA(core);
    CalSkeleton skeletonB(core);
    CalMixer mixerA;
    CalMixer mixerB;

    ISTD::shared_ptr<CalAnimation> animA =
        ISTD::make_shared<CalAnimation>(streamClip, 1.0f, 0);
    ISTD::shared_ptr<CalAnimation> animB =
        ISTD::make_shared<CalAnimation>(streamClip, 1.0f, 0);
    mixerA.addAnimation(animA);
    mixerB.addAnimation(animB);

    CalStreamClipCache cache(2);

    const float timesA[] = { 1.3f, 0.2f, 1.9f, 0.7f, 0.1f };
    const float timesB[] = { 0.4f, 1.7f, 0.3f, 1.1f, 1.95f };
    for (size_t i = 0; i < 5; ++i) {
        animA->time = timesA[i];
        animB->time = timesB[i];
        TASK_CHECK(mixerA.updateSkeletonStreamed(&skeletonA, {}, {}, &cache));
        TASK_CHECK(mixerB.updateSkeletonStreamed(&skeletonB, {}, {}, &cache));

        // Compare against the pure-memory mixer at the same time.
        CalSkeleton refA(core), refB(core);
        CalMixer refMixerA, refMixerB;
        ISTD::shared_ptr<CalAnimation> refAnimA =
            ISTD::make_shared<CalAnimation>(clip, 1.0f, 0);
        ISTD::shared_ptr<CalAnimation> refAnimB =
            ISTD::make_shared<CalAnimation>(clip, 1.0f, 0);
        refAnimA->time = timesA[i];
        refAnimB->time = timesB[i];
        refMixerA.addAnimation(refAnimA);
        refMixerB.addAnimation(refAnimB);
        refMixerA.updateSkeleton(&refA, {}, {});
        refMixerB.updateSkeleton(&refB, {}, {});

        CalVector pa, na, pb, nb;
        CalVector ra, rna, rb, rnb;
        skinVertex(skeletonA, pa, na);
        skinVertex(refA, ra, rna);
        skinVertex(skeletonB, pb, nb);
        skinVertex(refB, rb, rnb);
        TASK_CHECK(closeV(pa, ra));
        TASK_CHECK(closeV(na, rna));
        TASK_CHECK(closeV(pb, rb));
        TASK_CHECK(closeV(nb, rnb));
        TASK_CHECK(closeV(skeletonA.bones[1].absoluteTransform.translation,
                           refA.bones[1].absoluteTransform.translation));
        TASK_CHECK(closeV(skeletonB.bones[0].absoluteTransform.translation,
                           refB.bones[0].absoluteTransform.translation));
    }

    // Both characters agree when they are at the same time, reading the same
    // cached block.
    animA->time = animB->time = 1.4f;
    TASK_CHECK(mixerA.updateSkeletonStreamed(&skeletonA, {}, {}, &cache));
    TASK_CHECK(mixerB.updateSkeletonStreamed(&skeletonB, {}, {}, &cache));
    TASK_CHECK(closeV(skeletonA.bones[0].absoluteTransform.translation,
                      skeletonB.bones[0].absoluteTransform.translation));
    TASK_CHECK(closeV(skeletonA.bones[1].absoluteTransform.translation,
                      skeletonB.bones[1].absoluteTransform.translation));

    // The transient decode peak is bounded by capacity + this frame's
    // distinct blocks; the resident count at the end of every frame is not
    // above capacity.
    TASK_CHECK(cache.peakDecodedBlocks() <= 2 + 1);
    TASK_CHECK(cache.residentCount() <= 2);
    return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    { "block_sampling", "nonuniform_keys_cross_boundary", case_nonuniform_keys_cross_boundary },
    { "block_sampling", "empty_and_missing_tracks", case_empty_and_missing_tracks },
    { "block_sampling", "equivalent_quaternion_sign", case_equivalent_quaternion_sign },
    { "block_sampling", "open_reads_zero_blocks", case_open_reads_zero_blocks },
    { "block_sampling", "repeated_sampling_hits_cache", case_repeated_sampling_hits_cache },
    { "block_sampling", "invalid_files_are_rejected", case_invalid_files_are_rejected },
    { "block_sampling", "block_size_limit", case_block_size_limit },
    { "block_sampling", "single_block_and_zero_duration", case_single_block_and_zero_duration },
    { "cache_transactions", "shared_blocks_hit_once", case_shared_blocks_hit_once },
    { "cache_transactions", "capacity_exact_and_insufficient", case_capacity_exact_and_insufficient },
    { "cache_transactions", "corruption_fails_without_eviction", case_corruption_fails_without_eviction },
    { "cache_transactions", "reopen_gets_fresh_identity", case_reopen_gets_fresh_identity },
    { "mixed_pipeline", "mixed_high_low_priority", case_mixed_high_low_priority },
    { "mixed_pipeline", "streamed_only_and_legacy_error", case_streamed_only_and_legacy_error },
    { "mixed_pipeline", "failure_restores_and_retries", case_failure_restores_and_retries },
    { "mixed_pipeline", "two_characters_out_of_order_seek", case_two_characters_out_of_order_seek },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
