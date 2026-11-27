//****************************************************************************//
// streamclip_target.cpp                                                      //
// Task cases for blocked streaming clips, the shared block cache and the     //
// streamed mixer pipeline.  No main(): task_support/driver.c drives these.   //
//****************************************************************************//

#include "task_support/check.h"

#include "cal3d/streamclip.h"
#include "cal3d/coreanimation.h"
#include "cal3d/coretrack.h"
#include "cal3d/corekeyframe.h"
#include "cal3d/coreskeleton.h"
#include "cal3d/corebone.h"
#include "cal3d/animation.h"
#include "cal3d/mixer.h"
#include "cal3d/skeleton.h"
#include "cal3d/bone.h"
#include "cal3d/submesh.h"
#include "cal3d/physique.h"
#include "cal3d/bonetransform.h"
#include "cal3d/matrix.h"
#include "cal3d/quaternion.h"
#include "cal3d/error.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// small utilities
// ---------------------------------------------------------------------------

bool close(float a, float b) {
    return std::fabs(a - b) <= 2e-5f;
}

bool closeVec(const CalVector& a, const CalVector& b) {
    return close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z);
}

bool closeQuat(const CalQuaternion& a, const CalQuaternion& b) {
    // quaternions describe the same rotation up to a common sign
    const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    return close(std::fabs(d), 1.0f);
}

std::string tempPath(const char* name) {
    const char* base = std::getenv("TMPDIR");
    std::string dir = base ? base : ".";
    dir += "/streamclip_target_";
    // unique per process and per name
    static unsigned counter = 0;
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), "%d_%u", int(::getpid()), ++counter);
    dir += suffix;
    std::string command = "mkdir -p " + dir;
    if (std::system(command.c_str()) != 0) {
        return std::string();
    }
    return dir + "/" + name;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const std::string& path, const std::string& contents) {
    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(contents.data(), std::streamsize(contents.size()));
    return bool(f);
}

CalCoreKeyframe kf(float t, const CalVector& tr, const CalQuaternion& q) {
    return CalCoreKeyframe(t, tr, q);
}

CalQuaternion axisAngle(const CalVector& axis, float angle) {
    CalQuaternion q;
    q.setAxisAngle(axis, angle);
    return q;
}

// Independent CRC-32 used by the hand-built file writer (not the library one).
uint32_t manualCrc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
        }
    }
    return ~crc;
}

// Builds a .csc file byte by byte straight from the format document, so the
// on-disk layout is exercised independently of writeStreamClip().
class ManualFileBuilder {
public:
    struct Block {
        float start;
        float end;
        struct TrackRef {
            uint32_t boneId;
            struct Key {
                float time;
                float tx, ty, tz;
                float qx, qy, qz, qw;
            };
            std::vector<Key> keys;
        };
        std::vector<TrackRef> tracks;
    };

    std::vector<Block> blocks;
    std::vector<std::pair<uint32_t, uint32_t>> trackEntries; // bone id, key total
    float duration;
    float blockDuration;
    uint32_t magic;
    uint32_t version;
    uint32_t flags;

    ManualFileBuilder()
        : duration(1.0f)
        , blockDuration(0.5f)
        , magic(0x31435343u)
        , version(1)
        , flags(0)
    {}

    static void putU32(std::string& s, uint32_t v) {
        char b[4] = {
            char(v & 0xFFu), char((v >> 8) & 0xFFu),
            char((v >> 16) & 0xFFu), char((v >> 24) & 0xFFu)
        };
        s.append(b, 4);
    }
    static void putU64(std::string& s, uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            s.push_back(char((v >> (8 * i)) & 0xFFu));
        }
    }
    static void putF32(std::string& s, float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        putU32(s, bits);
    }

    std::string serialize() const {
        const uint32_t blockCount = uint32_t(blocks.size());
        const uint32_t trackCount = uint32_t(trackEntries.size());
        const uint64_t dataStart = 40 + uint64_t(32) * blockCount + uint64_t(8) * trackCount;

        std::string payloads;
        std::vector<std::pair<uint64_t, uint32_t>> layout; // offset, length
        std::vector<uint32_t> blockChecksums;
        for (size_t b = 0; b < blocks.size(); ++b) {
            std::string p;
            putU32(p, uint32_t(blocks[b].tracks.size()));
            for (size_t t = 0; t < blocks[b].tracks.size(); ++t) {
                const Block::TrackRef& tr = blocks[b].tracks[t];
                putU32(p, tr.boneId);
                putU32(p, uint32_t(tr.keys.size()));
                for (size_t k = 0; k < tr.keys.size(); ++k) {
                    const Block::TrackRef::Key& k0 = tr.keys[k];
                    putF32(p, k0.time);
                    putF32(p, k0.tx);
                    putF32(p, k0.ty);
                    putF32(p, k0.tz);
                    putF32(p, k0.qx);
                    putF32(p, k0.qy);
                    putF32(p, k0.qz);
                    putF32(p, k0.qw);
                }
            }
            layout.push_back(std::make_pair(dataStart + payloads.size(), uint32_t(p.size())));
            blockChecksums.push_back(manualCrc32(reinterpret_cast<const uint8_t*>(p.data()), p.size()));
            payloads += p;
        }

        std::string file;
        putU32(file, magic);
        putU32(file, version);
        putU32(file, flags);
        putF32(file, duration);
        putF32(file, blockDuration);
        putU32(file, blockCount);
        putU32(file, trackCount);
        putU64(file, dataStart);
        putU32(file, manualCrc32(reinterpret_cast<const uint8_t*>(payloads.data()), payloads.size()));

        for (size_t b = 0; b < blocks.size(); ++b) {
            putU32(file, uint32_t(b));
            putF32(file, blocks[b].start);
            putF32(file, blocks[b].end);
            putU64(file, layout[b].first);
            putU32(file, layout[b].second);
            putU32(file, blockChecksums[b]);
            putU32(file, 0);
        }
        for (size_t t = 0; t < trackEntries.size(); ++t) {
            putU32(file, trackEntries[t].first);
            putU32(file, trackEntries[t].second);
        }
        file += payloads;
        return file;
    }
};

// ---------------------------------------------------------------------------
// shared fixtures
// ---------------------------------------------------------------------------

// Non-uniform key times, several landing exactly on block boundaries.
ISTD::shared_ptr<CalCoreAnimation> makeBoundaryClip() {
    auto clip = ISTD::make_shared<CalCoreAnimation>();
    clip->duration = 1.0f;

    CalCoreTrack::KeyframeList keys;
    const CalVector axis(0, 0, 1);
    keys.push_back(kf(0.0f, CalVector(0, 0, 0), axisAngle(axis, 0.0f)));
    keys.push_back(kf(0.15f, CalVector(1, 0, 0), axisAngle(axis, 0.3f)));
    keys.push_back(kf(0.30f, CalVector(2, 0, 0), axisAngle(axis, 0.6f)));
    keys.push_back(kf(0.45f, CalVector(3, 1, 0), axisAngle(axis, 0.9f)));
    keys.push_back(kf(0.60f, CalVector(4, 1, 1), axisAngle(axis, 1.2f)));
    keys.push_back(kf(0.80f, CalVector(5, 2, 1), axisAngle(axis, 1.5f)));
    keys.push_back(kf(1.00f, CalVector(6, 2, 2), axisAngle(axis, 1.8f)));
    clip->tracks.push_back(CalCoreTrack(0, keys));

    CalCoreTrack::KeyframeList keys2;
    keys2.push_back(kf(0.0f, CalVector(0, 10, 0), CalQuaternion()));
    keys2.push_back(kf(1.0f, CalVector(0, 11, 0), CalQuaternion()));
    clip->tracks.push_back(CalCoreTrack(1, keys2));
    return clip;
}

ISTD::shared_ptr<CalCoreSkeleton> makeTwoBoneSkeleton() {
    auto core = ISTD::make_shared<CalCoreSkeleton>();
    core->addCoreBone(ISTD::make_shared<CalCoreBone>("root"));
    core->addCoreBone(ISTD::make_shared<CalCoreBone>("child", 0));

    core->coreBones[0]->relativeTransform = cal3d::RotateTranslate(
        axisAngle(CalVector(0, 0, 1), 0.2f), CalVector(0.1f, 0.2f, 0.3f));
    core->coreBones[1]->relativeTransform = cal3d::RotateTranslate(
        axisAngle(CalVector(1, 0, 0), 0.4f), CalVector(0.5f, 0.0f, 0.0f));

    // deliberately non-identity inverse bind poses
    core->coreBones[0]->inverseBindPoseTransform = cal3d::RotateTranslate(
        axisAngle(CalVector(0, 1, 0), 0.15f), CalVector(-1.0f, -2.0f, -3.0f));
    core->coreBones[1]->inverseBindPoseTransform = cal3d::RotateTranslate(
        axisAngle(CalVector(0, 0, 1), -0.25f), CalVector(-1.5f, -2.5f, -3.5f));
    return core;
}

bool sameSkeleton(const CalSkeleton& a, const CalSkeleton& b) {
    if (a.bones.size() != b.bones.size()) {
        return false;
    }
    for (size_t i = 0; i < a.bones.size(); ++i) {
        const CalMatrix& ma = a.bones[i].absoluteTransform.basis;
        const CalMatrix& mb = b.bones[i].absoluteTransform.basis;
        const CalVector* va = &ma.cx;
        const CalVector* vb = &mb.cx;
        for (int c = 0; c < 3; ++c) {
            if (!closeVec(va[c], vb[c])) {
                return false;
            }
        }
        if (!closeVec(a.bones[i].absoluteTransform.translation,
                      b.bones[i].absoluteTransform.translation)) {
            return false;
        }
    }
    return true;
}

bool sameSkin(const CalVector4* a, const CalVector4* b, size_t vertexCount) {
    for (size_t i = 0; i < vertexCount * 2; ++i) {
        if (!close(a[i].x, b[i].x) || !close(a[i].y, b[i].y) || !close(a[i].z, b[i].z)) {
            return false;
        }
    }
    return true;
}

// ===========================================================================
// block_sampling
// ===========================================================================

int case_boundary_keys_exact_intervals() {
    const std::string path = tempPath("boundary.csc");
    auto core = makeBoundaryClip();
    TASK_CHECK(cal3d::writeStreamClip(path, *core, 0.30f));

    cal3d::CalStreamClipPtr clip = cal3d::openStreamClip(path);
    TASK_CHECK(clip);
    TASK_CHECK(clip->blockCount() == 4);
    TASK_CHECK(clip->trackCount() == 2);
    TASK_CHECK(clip->readAttempts() == 0);

    TASK_CHECK(close(clip->blockInfo(0).blockStart, 0.0f) && close(clip->blockInfo(0).blockEnd, 0.3f));
    TASK_CHECK(close(clip->blockInfo(1).blockStart, 0.3f) && close(clip->blockInfo(1).blockEnd, 0.6f));
    TASK_CHECK(close(clip->blockInfo(2).blockStart, 0.6f) && close(clip->blockInfo(2).blockEnd, 0.9f));
    TASK_CHECK(close(clip->blockInfo(3).blockStart, 0.9f) && close(clip->blockInfo(3).blockEnd, 1.0f));

    TASK_CHECK(clip->blockIndexForTime(-5.0f) == 0);
    TASK_CHECK(clip->blockIndexForTime(0.0f) == 0);
    TASK_CHECK(clip->blockIndexForTime(0.2999f) == 0);
    TASK_CHECK(clip->blockIndexForTime(0.3f) == 1);
    TASK_CHECK(clip->blockIndexForTime(0.5999f) == 1);
    TASK_CHECK(clip->blockIndexForTime(0.6f) == 2);
    TASK_CHECK(clip->blockIndexForTime(0.9f) == 3);
    TASK_CHECK(clip->blockIndexForTime(1.0f) == 3);
    TASK_CHECK(clip->blockIndexForTime(9.0f) == 3);

    cal3d::CalStreamBlockCache cache(4);
    const float sampleTimes[] = {-0.5f, 0.0f, 0.15f, 0.2999f, 0.3f, 0.45f, 0.6f,
                                 0.75f, 0.9f, 1.0f, 1.5f};
    for (size_t i = 0; i < sizeof(sampleTimes) / sizeof(sampleTimes[0]); ++i) {
        const float t = sampleTimes[i];
        cal3d::RotateTranslate got;
        TASK_CHECK(clip->sampleTrack(0, t, cache, got));
        const cal3d::RotateTranslate want = core->tracks[0].getCurrentTransform(t);
        TASK_CHECK(closeVec(got.translation, want.translation));
        TASK_CHECK(closeQuat(got.rotation, want.rotation));

        cal3d::RotateTranslate got2;
        TASK_CHECK(clip->sampleTrack(1, t, cache, got2));
        const cal3d::RotateTranslate want2 = core->tracks[1].getCurrentTransform(t);
        TASK_CHECK(closeVec(got2.translation, want2.translation));
    }

    // four distinct blocks were read; every block carries both tracks
    TASK_CHECK(clip->readAttempts() == 4);
    TASK_CHECK(cache.stats().residentBlocks == 4);
    return 0;
}

int case_empty_track_and_missing_track() {
    const std::string path = tempPath("empty.csc");
    auto core = ISTD::make_shared<CalCoreAnimation>();
    core->duration = 1.0f;
    CalCoreTrack::KeyframeList keys;
    keys.push_back(kf(0.0f, CalVector(1, 0, 0), CalQuaternion()));
    keys.push_back(kf(1.0f, CalVector(2, 0, 0), CalQuaternion()));
    core->tracks.push_back(CalCoreTrack(0, keys));
    core->tracks.push_back(CalCoreTrack(1, CalCoreTrack::KeyframeList())); // empty

    TASK_CHECK(cal3d::writeStreamClip(path, *core, 0.5f));
    cal3d::CalStreamClipPtr clip = cal3d::openStreamClip(path);
    TASK_CHECK(clip);

    auto skeletonCore = makeTwoBoneSkeleton();
    // third bone exists in the skeleton but has no track at all
    skeletonCore->addCoreBone(ISTD::make_shared<CalCoreBone>("orphan", 1));

    auto streamedAnim = ISTD::make_shared<CalAnimation>(core, 1.0f, 0);
    cal3d::setAnimationStreamSource(*streamedAnim, clip);
    TASK_CHECK(cal3d::animationHasStreamSource(*streamedAnim));

    CalMixer mixer;
    mixer.addAnimation(streamedAnim);
    CalSkeleton skeleton(skeletonCore);
    cal3d::CalStreamBlockCache cache(2);
    streamedAnim->time = 0.5f;
    TASK_CHECK(mixer.updateSkeletonStreamed(&skeleton, {}, {}, cache));

    // animated bone moved
    TASK_CHECK(close(skeleton.bones[0].getRelativeTransform().translation.x, 1.5f));
    // empty track sampled to the identity local transform
    TASK_CHECK(closeVec(skeleton.bones[1].getRelativeTransform().translation, CalVector()));
    TASK_CHECK(closeQuat(skeleton.bones[1].getRelativeTransform().rotation, CalQuaternion()));
    // missing track keeps the bind (core relative) pose
    TASK_CHECK(closeVec(skeleton.bones[2].getRelativeTransform().translation,
                        skeletonCore->coreBones[2]->relativeTransform.translation));

    // direct empty-track sampling is identity too
    cal3d::RotateTranslate emptyPose;
    TASK_CHECK(clip->sampleTrack(1, 0.5f, cache, emptyPose));
    TASK_CHECK(closeVec(emptyPose.translation, CalVector()));
    TASK_CHECK(closeQuat(emptyPose.rotation, CalQuaternion()));
    return 0;
}

int case_opposite_sign_quaternions() {
    const std::string pathA = tempPath("quat_a.csc");
    const std::string pathB = tempPath("quat_b.csc");

    auto make = [](bool flip) {
        auto clip = ISTD::make_shared<CalCoreAnimation>();
        clip->duration = 1.0f;
        CalCoreTrack::KeyframeList keys;
        CalQuaternion q0 = axisAngle(CalVector(0, 0, 1), 0.5f);
        CalQuaternion q1 = axisAngle(CalVector(0, 1, 0), 1.0f);
        if (flip) {
            q0 = CalQuaternion(-q0.x, -q0.y, -q0.z, -q0.w);
            q1 = CalQuaternion(-q1.x, -q1.y, -q1.z, -q1.w);
        }
        keys.push_back(kf(0.0f, CalVector(), q0));
        keys.push_back(kf(1.0f, CalVector(1, 1, 1), q1));
        clip->tracks.push_back(CalCoreTrack(0, keys));
        return clip;
    };

    auto coreA = make(false);
    auto coreB = make(true);
    TASK_CHECK(cal3d::writeStreamClip(pathA, *coreA, 0.5f));
    TASK_CHECK(cal3d::writeStreamClip(pathB, *coreB, 0.5f));
    auto clipA = cal3d::openStreamClip(pathA);
    auto clipB = cal3d::openStreamClip(pathB);
    TASK_CHECK(clipA && clipB);

    cal3d::CalStreamBlockCache cacheA(2), cacheB(2);
    const CalVector probes[3] = {CalVector(1, 0, 0), CalVector(0, 1, 0), CalVector(0, 0, 1)};
    const float times[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (float t : times) {
        cal3d::RotateTranslate a, b;
        TASK_CHECK(clipA->sampleTrack(0, t, cacheA, a));
        TASK_CHECK(clipB->sampleTrack(0, t, cacheB, b));
        for (const CalVector& v : probes) {
            TASK_CHECK(closeVec(a.rotation * v, b.rotation * v));
        }
        TASK_CHECK(closeVec(a.translation, b.translation));
    }
    return 0;
}

int case_open_zero_reads_and_repeat_hits() {
    const std::string path = tempPath("hits.csc");
    auto core = makeBoundaryClip();
    TASK_CHECK(cal3d::writeStreamClip(path, *core, 0.3f));

    // opening touches header/directory only
    auto clip = cal3d::openStreamClip(path);
    TASK_CHECK(clip);
    TASK_CHECK(clip->readAttempts() == 0);

    cal3d::CalStreamBlockCache cache(2);
    TASK_CHECK(cache.stats().residentBlocks == 0);
    TASK_CHECK(cache.stats().hitCount == 0);
    TASK_CHECK(cache.stats().readAttempts == 0);

    cal3d::RotateTranslate r;
    TASK_CHECK(clip->sampleTrack(0, 0.1f, cache, r));
    TASK_CHECK(clip->readAttempts() == 1);
    TASK_CHECK(cache.stats().readAttempts == 1);

    // repeated sampling of the same block never re-reads and counts as hits
    for (int i = 0; i < 5; ++i) {
        TASK_CHECK(clip->sampleTrack(0, 0.2f, cache, r));
    }
    TASK_CHECK(clip->readAttempts() == 1);
    TASK_CHECK(cache.stats().readAttempts == 1);
    TASK_CHECK(cache.stats().hitCount == 5);
    TASK_CHECK(cache.stats().residentBlocks == 1);

    // a second open of the same file has a fresh identity and zero reads
    auto clip2 = cal3d::openStreamClip(path);
    TASK_CHECK(clip2);
    TASK_CHECK(clip2->id() != clip->id());
    TASK_CHECK(clip2->readAttempts() == 0);
    return 0;
}

int case_handbuilt_file_and_invalid_rejections() {
    const std::string goodPath = tempPath("manual_good.csc");

    // hand-build a two-block clip with one animated and one empty track
    ManualFileBuilder builder;
    builder.duration = 1.0f;
    builder.blockDuration = 0.5f;
    builder.trackEntries.push_back(std::make_pair(0u, 4u));
    builder.trackEntries.push_back(std::make_pair(1u, 0u));

    ManualFileBuilder::Block b0;
    b0.start = 0.0f;
    b0.end = 0.5f;
    b0.tracks.push_back(ManualFileBuilder::Block::TrackRef());
    b0.tracks[0].boneId = 0;
    b0.tracks[0].keys.push_back({0.0f, 0, 0, 0, 0, 0, 0, 1});
    b0.tracks[0].keys.push_back({0.5f, 1, 0, 0, 0, 0, 0, 1});
    builder.blocks.push_back(b0);

    ManualFileBuilder::Block b1;
    b1.start = 0.5f;
    b1.end = 1.0f;
    b1.tracks.push_back(ManualFileBuilder::Block::TrackRef());
    b1.tracks[0].boneId = 0;
    b1.tracks[0].keys.push_back({0.5f, 1, 0, 0, 0, 0, 0, 1}); // predecessor
    b1.tracks[0].keys.push_back({1.0f, 2, 0, 0, 0, 0, 0, 1});
    builder.blocks.push_back(b1);

    TASK_CHECK(writeFile(goodPath, builder.serialize()));
    auto clip = cal3d::openStreamClip(goodPath);
    TASK_CHECK(clip);
    TASK_CHECK(clip->blockCount() == 2 && clip->trackCount() == 2);
    cal3d::CalStreamBlockCache cache(2);
    cal3d::RotateTranslate r;
    TASK_CHECK(clip->sampleTrack(0, 0.25f, cache, r));
    TASK_CHECK(close(r.translation.x, 0.5f));
    TASK_CHECK(clip->sampleTrack(0, 1.0f, cache, r));
    TASK_CHECK(close(r.translation.x, 2.0f));

    // bad magic
    {
        ManualFileBuilder bad = builder;
        bad.magic = 0xDEADBEEFu;
        const std::string p = tempPath("bad_magic.csc");
        TASK_CHECK(writeFile(p, bad.serialize()));
        TASK_CHECK(!cal3d::openStreamClip(p));
        TASK_CHECK(CalError::getLastErrorCode() == CalError::INVALID_FILE_FORMAT);
    }
    // unsupported version
    {
        ManualFileBuilder bad = builder;
        bad.version = 99;
        const std::string p = tempPath("bad_version.csc");
        TASK_CHECK(writeFile(p, bad.serialize()));
        TASK_CHECK(!cal3d::openStreamClip(p));
        TASK_CHECK(CalError::getLastErrorCode() == CalError::INCOMPATIBLE_FILE_VERSION);
    }
    // missing file
    TASK_CHECK(!cal3d::openStreamClip(tempPath("does_not_exist.csc")));
    TASK_CHECK(CalError::getLastErrorCode() == CalError::FILE_NOT_FOUND);

    // truncated file: present but shorter than the fixed header
    {
        const std::string p = tempPath("truncated.csc");
        TASK_CHECK(writeFile(p, builder.serialize().substr(0, 20)));
        TASK_CHECK(!cal3d::openStreamClip(p));
        TASK_CHECK(CalError::getLastErrorCode() == CalError::INVALID_FILE_FORMAT);
    }

    // block length above the 32 MiB cap is rejected from the directory alone
    {
        std::string bytes = builder.serialize();
        // block 0 length field: header 40 + 3 u32 (12) = byte 52
        const size_t lenPos = 52 + 8; // after the u64 offset
        const uint32_t tooBig = 32u * 1024u * 1024u + 1u;
        for (int i = 0; i < 4; ++i) {
            bytes[lenPos + i] = char((tooBig >> (8 * i)) & 0xFFu);
        }
        const std::string p = tempPath("big_block.csc");
        TASK_CHECK(writeFile(p, bytes));
        TASK_CHECK(!cal3d::openStreamClip(p));
        TASK_CHECK(CalError::getLastErrorCode() == CalError::INVALID_FILE_FORMAT);
    }

    // directory out of bounds: claim the payload lies past the end
    {
        std::string bytes = builder.serialize();
        // block 0 offset field lives at header(40) + 4+4+4 = byte 52
        const size_t offPos = 52;
        uint64_t huge = uint64_t(bytes.size()) + 4096;
        for (int i = 0; i < 8; ++i) {
            bytes[offPos + i] = char((huge >> (8 * i)) & 0xFFu);
        }
        const std::string p = tempPath("bad_offset.csc");
        TASK_CHECK(writeFile(p, bytes));
        TASK_CHECK(!cal3d::openStreamClip(p));
    }
    // overlapping block intervals
    {
        ManualFileBuilder bad = builder;
        bad.blocks[1].start = 0.4f; // now overlaps block 0
        const std::string p = tempPath("overlap.csc");
        TASK_CHECK(writeFile(p, bad.serialize()));
        TASK_CHECK(!cal3d::openStreamClip(p));
    }
    // duplicate track ids
    {
        ManualFileBuilder bad = builder;
        bad.trackEntries[1].first = 0;
        const std::string p = tempPath("dup_track.csc");
        TASK_CHECK(writeFile(p, bad.serialize()));
        TASK_CHECK(!cal3d::openStreamClip(p));
    }
    // non-increasing keys: opens fine (payloads are lazy), decode rejects
    {
        ManualFileBuilder bad = builder;
        bad.blocks[0].tracks[0].keys[1].time = 0.4f; // 0.0 then 0.4 is fine...
        bad.blocks[0].tracks[0].keys[0].time = 0.4f; // equal times -> not increasing
        const std::string p = tempPath("bad_keys.csc");
        TASK_CHECK(writeFile(p, bad.serialize()));
        auto badClip = cal3d::openStreamClip(p);
        TASK_CHECK(badClip);
        cal3d::CalStreamBlockCache badCache(1);
        cal3d::RotateTranslate rr;
        TASK_CHECK(!badClip->sampleTrack(0, 0.2f, badCache, rr));
    }
    // non-finite value: decode rejects
    {
        ManualFileBuilder bad = builder;
        const float inf = std::numeric_limits<float>::infinity();
        bad.blocks[0].tracks[0].keys[0].tx = inf;
        const std::string p = tempPath("bad_finite.csc");
        TASK_CHECK(writeFile(p, bad.serialize()));
        auto badClip = cal3d::openStreamClip(p);
        TASK_CHECK(badClip);
        cal3d::CalStreamBlockCache badCache(1);
        cal3d::RotateTranslate rr;
        TASK_CHECK(!badClip->sampleTrack(0, 0.2f, badCache, rr));
    }
    // illegal bone id inside a payload: decode rejects
    {
        ManualFileBuilder bad = builder;
        bad.blocks[0].tracks[0].boneId = 7;
        const std::string p = tempPath("bad_bone.csc");
        TASK_CHECK(writeFile(p, bad.serialize()));
        auto badClip = cal3d::openStreamClip(p);
        TASK_CHECK(badClip);
        cal3d::CalStreamBlockCache badCache(1);
        cal3d::RotateTranslate rr;
        TASK_CHECK(!badClip->sampleTrack(7, 0.2f, badCache, rr));
    }
    return 0;
}

int case_writer_validation_and_missing_retry() {
    auto valid = []() {
        auto c = ISTD::make_shared<CalCoreAnimation>();
        c->duration = 1.0f;
        CalCoreTrack::KeyframeList k;
        k.push_back(kf(0.0f, CalVector(), CalQuaternion()));
        k.push_back(kf(1.0f, CalVector(1, 0, 0), CalQuaternion()));
        c->tracks.push_back(CalCoreTrack(0, k));
        return c;
    };

    // blockDuration must be positive and finite
    TASK_CHECK(!cal3d::writeStreamClip(tempPath("w_zero.csc"), *valid(), 0.0f));
    TASK_CHECK(!cal3d::writeStreamClip(tempPath("w_neg.csc"), *valid(), -0.5f));
    TASK_CHECK(!cal3d::writeStreamClip(tempPath("w_nan.csc"), *valid(),
                                       std::numeric_limits<float>::quiet_NaN()));
    // a rejected write leaves no file behind
    const std::string rejected = tempPath("w_rejected.csc");
    TASK_CHECK(!cal3d::writeStreamClip(rejected, *valid(), 0.0f));
    std::string ignored;
    TASK_CHECK(!readFile(rejected, ignored));

    // duplicate track bone ids
    {
        auto c = valid();
        c->tracks.push_back(CalCoreTrack(0, CalCoreTrack::KeyframeList()));
        TASK_CHECK(!cal3d::writeStreamClip(tempPath("w_dup.csc"), *c, 0.5f));
    }
    // non-increasing key times: a duplicate time survives the track's sort
    // but must be rejected by the writer
    {
        auto c = valid();
        c->tracks[0].keyframes.push_back(kf(0.0f, CalVector(9, 9, 9), CalQuaternion()));
        TASK_CHECK(!cal3d::writeStreamClip(tempPath("w_keys.csc"), *c, 0.5f));
    }
    // non-finite keyframe value
    {
        auto c = valid();
        c->tracks[0].keyframes[1].transform.translation.y =
            std::numeric_limits<float>::infinity();
        TASK_CHECK(!cal3d::writeStreamClip(tempPath("w_inf.csc"), *c, 0.5f));
    }
    // illegal bone id
    {
        auto c = valid();
        c->tracks[0].coreBoneId = -3;
        TASK_CHECK(!cal3d::writeStreamClip(tempPath("w_bone.csc"), *c, 0.5f));
    }

    // zero-duration animation: exactly one block containing the key at t = 0
    {
        auto c = ISTD::make_shared<CalCoreAnimation>();
        c->duration = 0.0f;
        CalCoreTrack::KeyframeList k;
        k.push_back(kf(0.0f, CalVector(2, 3, 4), CalQuaternion()));
        c->tracks.push_back(CalCoreTrack(0, k));
        const std::string p = tempPath("w_zero_dur.csc");
        TASK_CHECK(cal3d::writeStreamClip(p, *c, 0.25f));
        auto clip = cal3d::openStreamClip(p);
        TASK_CHECK(clip && clip->blockCount() == 1);
        cal3d::CalStreamBlockCache cache(1);
        cal3d::RotateTranslate r;
        TASK_CHECK(clip->sampleTrack(0, 0.0f, cache, r));
        TASK_CHECK(closeVec(r.translation, CalVector(2, 3, 4)));
    }

    // missing file is retryable: failed open, then create, then open succeeds
    {
        const std::string p = tempPath("w_retry.csc");
        TASK_CHECK(!cal3d::openStreamClip(p));
        TASK_CHECK(CalError::getLastErrorCode() == CalError::FILE_NOT_FOUND);
        TASK_CHECK(cal3d::writeStreamClip(p, *valid(), 0.5f));
        auto clip = cal3d::openStreamClip(p);
        TASK_CHECK(clip && clip->blockCount() == 2);
    }
    return 0;
}

// ===========================================================================
// cache_transactions
// ===========================================================================

int case_shared_block_distinct_sources() {
    const std::string pathA = tempPath("src_a.csc");
    const std::string pathB = tempPath("src_b.csc");
    auto coreA = makeBoundaryClip();
    auto coreB = makeBoundaryClip();
    coreB->tracks[0].keyframes[1].transform.translation.x = 7.0f; // different content
    TASK_CHECK(cal3d::writeStreamClip(pathA, *coreA, 0.3f));
    TASK_CHECK(cal3d::writeStreamClip(pathB, *coreB, 0.3f));

    auto clipA = cal3d::openStreamClip(pathA);
    auto clipB = cal3d::openStreamClip(pathB);
    TASK_CHECK(clipA && clipB);
    TASK_CHECK(clipA->id() != clipB->id());

    cal3d::CalStreamBlockCache cache(4);
    cal3d::RotateTranslate ra, rb;
    TASK_CHECK(clipA->sampleTrack(0, 0.1f, cache, ra));
    TASK_CHECK(clipB->sampleTrack(0, 0.1f, cache, rb));

    // same block number, different sources: two reads and two residents
    TASK_CHECK(cache.stats().readAttempts == 2);
    TASK_CHECK(cache.stats().residentBlocks == 2);
    std::vector<std::pair<uint64_t, unsigned>> ids = cache.residentBlockIds();
    TASK_CHECK(ids.size() == 2);
    TASK_CHECK(ids[0].first == clipA->id() && ids[0].second == 0);
    TASK_CHECK(ids[1].first == clipB->id() && ids[1].second == 0);

    // the entries really hold different decoded content, no cross-hit
    TASK_CHECK(ra.translation.x != rb.translation.x);
    return 0;
}

int case_capacity_exact_fit_and_shortfall() {
    const std::string path = tempPath("capacity.csc");
    auto core = makeBoundaryClip();
    TASK_CHECK(cal3d::writeStreamClip(path, *core, 0.3f));
    auto clip = cal3d::openStreamClip(path);
    TASK_CHECK(clip);

    auto makeFrameAnims = [&]() {
        std::vector<ISTD::shared_ptr<CalAnimation>> anims;
        const float times[4] = {0.1f, 0.4f, 0.7f, 0.95f}; // one per block
        for (int i = 0; i < 4; ++i) {
            auto a = ISTD::make_shared<CalAnimation>(core, 1.0f, 0);
            cal3d::setAnimationStreamSource(*a, clip);
            a->time = times[i];
            anims.push_back(a);
        }
        return anims;
    };

    // capacity exactly equal to the four-block working set
    {
        cal3d::CalStreamBlockCache cache(4);
        CalMixer mixer;
        auto skeletonCore = makeTwoBoneSkeleton();
        CalSkeleton skeleton(skeletonCore);
        auto anims = makeFrameAnims();
        for (auto& a : anims) {
            mixer.addAnimation(a);
        }
        TASK_CHECK(mixer.updateSkeletonStreamed(&skeleton, {}, {}, cache));
        TASK_CHECK(cache.stats().residentBlocks == 4);
        TASK_CHECK(cache.stats().readAttempts == 4);
        TASK_CHECK(cache.stats().decodePeak == 4);
    }

    // capacity one short: the frame fails before any physical read
    {
        cal3d::CalStreamBlockCache cache(3);
        CalMixer mixer;
        auto skeletonCore = makeTwoBoneSkeleton();
        CalSkeleton before(skeletonCore);
        CalSkeleton skeleton(skeletonCore);
        auto anims = makeFrameAnims();
        for (auto& a : anims) {
            mixer.addAnimation(a);
        }
        TASK_CHECK(!mixer.updateSkeletonStreamed(&skeleton, {}, {}, cache));
        TASK_CHECK(cache.stats().readAttempts == 0);
        TASK_CHECK(cache.stats().residentBlocks == 0);
        TASK_CHECK(sameSkeleton(before, skeleton)); // skeleton untouched
        TASK_CHECK(CalError::getLastErrorCode() == CalError::BAD_DATA_SOURCE);

        // the same frame is retryable with a larger cache
        cal3d::CalStreamBlockCache bigger(4);
        TASK_CHECK(mixer.updateSkeletonStreamed(&skeleton, {}, {}, bigger));
        TASK_CHECK(bigger.stats().residentBlocks == 4);
    }
    return 0;
}

int case_corruption_failure_preserves_hot_blocks() {
    const std::string path = tempPath("corrupt.csc");
    auto core = makeBoundaryClip();
    TASK_CHECK(cal3d::writeStreamClip(path, *core, 0.3f));
    auto clip = cal3d::openStreamClip(path);
    TASK_CHECK(clip);

    cal3d::CalStreamBlockCache cache(2);
    cal3d::RotateTranslate r;
    // warm blocks 0 and 1
    TASK_CHECK(clip->sampleTrack(0, 0.1f, cache, r));
    TASK_CHECK(clip->sampleTrack(0, 0.4f, cache, r));
    TASK_CHECK(cache.stats().residentBlocks == 2);
    const std::vector<std::pair<uint64_t, unsigned>> before = cache.residentBlockIds();
    const uint64_t readsBefore = cache.stats().readAttempts;

    // corrupt block 2 payload on disk after the clip was opened
    std::string bytes;
    TASK_CHECK(readFile(path, bytes));
    const cal3d::StreamBlockInfo info2 = clip->blockInfo(2);
    (void)info2;
    // locate block 2 via the directory: header 40, entries 32 bytes each
    const size_t dirBase = 40 + 2 * 32;
    uint64_t offset2 = 0;
    uint32_t length2 = 0;
    for (int i = 0; i < 8; ++i) {
        offset2 |= uint64_t(uint8_t(bytes[dirBase + 12 + i])) << (8 * i);
    }
    for (int i = 0; i < 4; ++i) {
        length2 |= uint32_t(uint8_t(bytes[dirBase + 20 + i])) << (8 * i);
    }
    TASK_CHECK(length2 > 0);
    bytes[size_t(offset2)] ^= 0xFFu;
    TASK_CHECK(writeFile(path, bytes));

    // frame needing hot block 0 plus corrupt block 2: fits, then fails
    TASK_CHECK(!clip->sampleTrack(0, 0.7f, cache, r));
    TASK_CHECK(cache.stats().readAttempts == readsBefore + 1); // attempt counted
    TASK_CHECK(cache.stats().residentBlocks == 2);             // nothing evicted
    TASK_CHECK(cache.residentBlockIds() == before);            // LRU unchanged

    // repair the file and retry; block 0 stays hot and is never re-read
    bytes[size_t(offset2)] ^= 0xFFu;
    TASK_CHECK(writeFile(path, bytes));
    TASK_CHECK(clip->sampleTrack(0, 0.7f, cache, r));
    TASK_CHECK(cache.stats().readAttempts == readsBefore + 2);
    TASK_CHECK(cache.stats().residentBlocks == 2); // 0 retained, one of 0/1 evicted for 2
    return 0;
}

int case_reopen_releases_stale_identity() {
    const std::string path = tempPath("reopen.csc");
    auto core = makeBoundaryClip();
    TASK_CHECK(cal3d::writeStreamClip(path, *core, 0.3f));

    cal3d::CalStreamBlockCache cache(2);
    uint64_t oldId = 0;
    {
        auto clip = cal3d::openStreamClip(path);
        TASK_CHECK(clip);
        oldId = clip->id();
        cal3d::RotateTranslate r;
        TASK_CHECK(clip->sampleTrack(0, 0.1f, cache, r));
        TASK_CHECK(cache.stats().residentBlocks == 1);
        // clip dies here; the cache keeps the stale block entry
    }

    auto reopened = cal3d::openStreamClip(path);
    TASK_CHECK(reopened);
    TASK_CHECK(reopened->id() != oldId);

    // the stale entry cannot satisfy the new open object: this is a miss
    const uint64_t readsBefore = cache.stats().readAttempts;
    cal3d::RotateTranslate r2;
    TASK_CHECK(reopened->sampleTrack(0, 0.1f, cache, r2));
    TASK_CHECK(cache.stats().readAttempts == readsBefore + 1);
    TASK_CHECK(cache.stats().residentBlocks == 2);

    const std::vector<std::pair<uint64_t, unsigned>> ids = cache.residentBlockIds();
    bool hasOld = false;
    bool hasNew = false;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i].first == oldId) {
            hasOld = true;
        }
        if (ids[i].first == reopened->id()) {
            hasNew = true;
        }
    }
    TASK_CHECK(hasOld && hasNew);
    return 0;
}

int case_lru_order_and_repeated_access() {
    const std::string path = tempPath("lru.csc");
    auto core = makeBoundaryClip();
    TASK_CHECK(cal3d::writeStreamClip(path, *core, 0.3f));
    auto clip = cal3d::openStreamClip(path);
    TASK_CHECK(clip);

    auto singleFrame = [&](cal3d::CalStreamBlockCache& cache, float time) {
        CalMixer mixer;
        auto skeletonCore = makeTwoBoneSkeleton();
        CalSkeleton skeleton(skeletonCore);
        auto a = ISTD::make_shared<CalAnimation>(core, 1.0f, 0);
        cal3d::setAnimationStreamSource(*a, clip);
        a->time = time;
        mixer.addAnimation(a);
        return mixer.updateSkeletonStreamed(&skeleton, {}, {}, cache);
    };

    cal3d::CalStreamBlockCache cache(2);
    TASK_CHECK(singleFrame(cache, 0.1f)); // block 0
    TASK_CHECK(cache.residentBlockIds().size() == 1);
    TASK_CHECK(cache.residentBlockIds()[0].second == 0);

    TASK_CHECK(singleFrame(cache, 0.4f)); // block 1
    TASK_CHECK(cache.residentBlockIds()[0].second == 0);
    TASK_CHECK(cache.residentBlockIds()[1].second == 1);

    TASK_CHECK(singleFrame(cache, 0.7f)); // block 2 evicts least recently used 0
    TASK_CHECK(cache.residentBlockIds().size() == 2);
    TASK_CHECK(cache.residentBlockIds()[0].second == 1);
    TASK_CHECK(cache.residentBlockIds()[1].second == 2);

    TASK_CHECK(singleFrame(cache, 0.4f)); // re-touch block 1: it becomes newest
    TASK_CHECK(cache.residentBlockIds()[0].second == 2);
    TASK_CHECK(cache.residentBlockIds()[1].second == 1);

    TASK_CHECK(singleFrame(cache, 0.1f)); // block 0 evicts least recently used 2
    TASK_CHECK(cache.residentBlockIds()[0].second == 1);
    TASK_CHECK(cache.residentBlockIds()[1].second == 0);
    TASK_CHECK(cache.stats().readAttempts == 4); // blocks 0,1,2,0 again

    // repeated access of one block in a frame: one read, one resident
    {
        cal3d::CalStreamBlockCache tiny(1);
        CalMixer mixer;
        auto skeletonCore = makeTwoBoneSkeleton();
        CalSkeleton skeleton(skeletonCore);
        for (int i = 0; i < 3; ++i) {
            auto a = ISTD::make_shared<CalAnimation>(core, 1.0f, 0);
            cal3d::setAnimationStreamSource(*a, clip);
            a->time = 0.35f + 0.05f * i; // all inside block 1
            mixer.addAnimation(a);
        }
        TASK_CHECK(mixer.updateSkeletonStreamed(&skeleton, {}, {}, tiny));
        TASK_CHECK(tiny.stats().residentBlocks == 1);
        TASK_CHECK(tiny.stats().readAttempts == 1);
        TASK_CHECK(tiny.residentBlockIds()[0].second == 1);
    }

    // last-access positioning: access order 0, 1, 0 in one frame means block
    // 1, not block 0, is the least recently used and gets evicted next
    {
        cal3d::CalStreamBlockCache c2(2);
        CalMixer mixer;
        auto skeletonCore = makeTwoBoneSkeleton();
        CalSkeleton skeleton(skeletonCore);
        const float order[3] = {0.1f, 0.4f, 0.2f}; // blocks 0, 1, 0
        for (int i = 0; i < 3; ++i) {
            auto a = ISTD::make_shared<CalAnimation>(core, 1.0f, 0);
            cal3d::setAnimationStreamSource(*a, clip);
            a->time = order[i];
            mixer.addAnimation(a);
        }
        TASK_CHECK(mixer.updateSkeletonStreamed(&skeleton, {}, {}, c2));
        TASK_CHECK(c2.residentBlockIds()[0].second == 1); // LRU
        TASK_CHECK(c2.residentBlockIds()[1].second == 0); // last touched

        TASK_CHECK(singleFrame(c2, 0.7f)); // bring block 2 in; block 1 must go
        TASK_CHECK(c2.residentBlockIds().size() == 2);
        TASK_CHECK(c2.residentBlockIds()[0].second == 0);
        TASK_CHECK(c2.residentBlockIds()[1].second == 2);
    }
    return 0;
}

// ===========================================================================
// mixed_pipeline
// ===========================================================================

ISTD::shared_ptr<CalCoreSubmesh> makeSkinSubmesh() {
    auto sub = ISTD::make_shared<CalCoreSubmesh>(1, false, 0);
    CalCoreSubmesh::Vertex v;
    v.position = CalPoint4(0.3f, 0.7f, -0.2f);
    v.normal = CalVector4(0.2f, 0.4f, 1.0f);
    std::vector<CalCoreSubmesh::Influence> inf;
    inf.push_back(CalCoreSubmesh::Influence(0, 0.25f, false));
    inf.push_back(CalCoreSubmesh::Influence(1, 0.75f, true));
    sub->addVertex(v, CalColor32(0), inf);
    return sub;
}

int case_mixed_priority_skin_matches_memory() {
    const std::string path = tempPath("mixed.csc");
    auto skeletonCore = makeTwoBoneSkeleton();

    // low priority streamed clip animating both bones
    auto streamedCore = ISTD::make_shared<CalCoreAnimation>();
    streamedCore->duration = 1.0f;
    CalCoreTrack::KeyframeList s0;
    s0.push_back(kf(0.0f, CalVector(0.2f, 0.0f, 0.0f), axisAngle(CalVector(0, 0, 1), 0.1f)));
    s0.push_back(kf(1.0f, CalVector(0.8f, 0.4f, 0.1f), axisAngle(CalVector(0, 0, 1), 0.9f)));
    streamedCore->tracks.push_back(CalCoreTrack(0, s0));
    CalCoreTrack::KeyframeList s1;
    s1.push_back(kf(0.0f, CalVector(0.0f, 0.5f, 0.0f), axisAngle(CalVector(1, 0, 0), 0.2f)));
    s1.push_back(kf(1.0f, CalVector(0.3f, 0.9f, 0.2f), axisAngle(CalVector(1, 0, 0), 0.8f)));
    streamedCore->tracks.push_back(CalCoreTrack(1, s1));

    TASK_CHECK(cal3d::writeStreamClip(path, *streamedCore, 0.25f));
    auto clip = cal3d::openStreamClip(path);
    TASK_CHECK(clip);

    // high priority in-memory clip replacing bone 0
    auto memoryCore = ISTD::make_shared<CalCoreAnimation>();
    memoryCore->duration = 1.0f;
    CalCoreTrack::KeyframeList h0;
    h0.push_back(kf(0.0f, CalVector(-0.4f, 0.1f, 0.2f), axisAngle(CalVector(0, 1, 0), 0.3f)));
    h0.push_back(kf(1.0f, CalVector(-0.9f, 0.6f, 0.5f), axisAngle(CalVector(0, 1, 0), 0.7f)));
    memoryCore->tracks.push_back(CalCoreTrack(0, h0));

    std::vector<BoneTransformAdjustment> adjustments;
    BoneTransformAdjustment ba;
    ba.boneId = 1;
    ba.localOri = axisAngle(CalVector(0, 0, 1), 0.2f);
    ba.rampValue = 0.4f;
    adjustments.push_back(ba);
    std::vector<BoneScaleAdjustment> scales;
    scales.push_back(BoneScaleAdjustment(0, CalVector(1.1f, 0.9f, 1.0f)));

    auto sub = makeSkinSubmesh();
    auto runtimeSub = ISTD::make_shared<CalSubmesh>(sub);

    const float times[] = {0.0f, 0.13f, 0.37f, 0.66f, 1.0f};
    for (float t : times) {
        // reference: everything in memory through the legacy entry point
        CalMixer refMixer;
        auto refLow = ISTD::make_shared<CalAnimation>(streamedCore, 0.8f, 0);
        auto refHigh = ISTD::make_shared<CalAnimation>(memoryCore, 1.0f, 1);
        refLow->time = t;
        refHigh->time = t;
        refHigh->rampValue = 0.6f;
        refMixer.addAnimation(refLow);
        refMixer.addAnimation(refHigh);
        CalSkeleton refSkeleton(skeletonCore);
        refMixer.updateSkeleton(&refSkeleton, adjustments, scales);
        CalVector4 refSkin[2];
        CalPhysique::calculateVerticesAndNormals(
            cal3d::pointerFromVector(refSkeleton.boneTransforms),
            runtimeSub.get(),
            reinterpret_cast<float*>(refSkin));

        // streamed entry: low priority clip comes from the cache
        CalMixer mixer;
        auto low = ISTD::make_shared<CalAnimation>(streamedCore, 0.8f, 0);
        cal3d::setAnimationStreamSource(*low, clip);
        auto high = ISTD::make_shared<CalAnimation>(memoryCore, 1.0f, 1);
        low->time = t;
        high->time = t;
        high->rampValue = 0.6f;
        mixer.addAnimation(low);
        mixer.addAnimation(high);
        CalSkeleton skeleton(skeletonCore);
        cal3d::CalStreamBlockCache cache(2);
        TASK_CHECK(mixer.updateSkeletonStreamed(&skeleton, adjustments, scales, cache));
        CalVector4 gotSkin[2];
        CalPhysique::calculateVerticesAndNormals(
            cal3d::pointerFromVector(skeleton.boneTransforms),
            runtimeSub.get(),
            reinterpret_cast<float*>(gotSkin));

        TASK_CHECK(sameSkeleton(refSkeleton, skeleton));
        TASK_CHECK(sameSkin(refSkin, gotSkin, 1));
    }
    return 0;
}

int case_two_characters_seek_and_recovery() {
    const std::string path = tempPath("twochar.csc");
    auto skeletonCore = makeTwoBoneSkeleton();
    auto core = makeBoundaryClip();
    TASK_CHECK(cal3d::writeStreamClip(path, *core, 0.3f));
    auto clip = cal3d::openStreamClip(path);
    TASK_CHECK(clip);

    cal3d::CalStreamBlockCache cache(2);

    struct Character {
        CalMixer mixer;
        CalSkeleton skeleton;
        ISTD::shared_ptr<CalAnimation> anim;
        explicit Character(const ISTD::shared_ptr<CalCoreSkeleton>& skel,
                           const ISTD::shared_ptr<CalCoreAnimation>& src,
                           const cal3d::CalStreamClipPtr& c)
            : skeleton(skel)
        {
            anim = ISTD::make_shared<CalAnimation>(src, 1.0f, 0);
            cal3d::setAnimationStreamSource(*anim, c);
            mixer.addAnimation(anim);
        }
    };

    Character a(skeletonCore, core, clip);
    Character b(skeletonCore, core, clip);

    // reference mixer kept fully in memory
    CalMixer refMixer;
    auto refAnim = ISTD::make_shared<CalAnimation>(core, 1.0f, 0);
    refMixer.addAnimation(refAnim);

    const float seekA[] = {0.95f, 0.1f, 0.45f, -0.3f, 0.7f, 0.3f};
    const float seekB[] = {0.2f, 0.85f, 0.5f, 0.99f, 0.05f, 0.6f};
    for (size_t step = 0; step < sizeof(seekA) / sizeof(seekA[0]); ++step) {
        a.anim->time = seekA[step];
        b.anim->time = seekB[step];

        // deliberately interleave out of order, including reverse seeks
        TASK_CHECK(b.mixer.updateSkeletonStreamed(&b.skeleton, {}, {}, cache));
        TASK_CHECK(a.mixer.updateSkeletonStreamed(&a.skeleton, {}, {}, cache));

        {
            CalSkeleton refSkeleton(skeletonCore);
            refAnim->time = seekA[step];
            refMixer.updateSkeleton(&refSkeleton, {}, {});
            TASK_CHECK(sameSkeleton(refSkeleton, a.skeleton));
        }
        {
            CalSkeleton refSkeleton(skeletonCore);
            refAnim->time = seekB[step];
            refMixer.updateSkeleton(&refSkeleton, {}, {});
            TASK_CHECK(sameSkeleton(refSkeleton, b.skeleton));
        }
    }

    // force a capacity failure, then recover: three distinct blocks in one
    // character's frame do not fit a capacity-2 cache
    CalMixer crowded;
    CalSkeleton crowdedSkeleton(skeletonCore);
    std::vector<ISTD::shared_ptr<CalAnimation>> anims;
    const float crowdedTimes[3] = {0.1f, 0.45f, 0.95f};
    for (int i = 0; i < 3; ++i) {
        auto an = ISTD::make_shared<CalAnimation>(core, 1.0f, 0);
        cal3d::setAnimationStreamSource(*an, clip);
        an->time = crowdedTimes[i];
        crowded.addAnimation(an);
        anims.push_back(an);
    }
    const CalVector untouched = a.skeleton.bones[0].absoluteTransform.translation;
    CalSkeleton crowdedBefore(skeletonCore);
    TASK_CHECK(!crowded.updateSkeletonStreamed(&crowdedSkeleton, {}, {}, cache));
    TASK_CHECK(sameSkeleton(crowdedBefore, crowdedSkeleton)); // failed frame changed nothing
    TASK_CHECK(closeVec(a.skeleton.bones[0].absoluteTransform.translation, untouched));

    // existing characters and their hot blocks keep working after the failure
    TASK_CHECK(a.mixer.updateSkeletonStreamed(&a.skeleton, {}, {}, cache));
    TASK_CHECK(b.mixer.updateSkeletonStreamed(&b.skeleton, {}, {}, cache));

    // recover the crowded frame with an adequately sized cache
    cal3d::CalStreamBlockCache big(4);
    TASK_CHECK(crowded.updateSkeletonStreamed(&crowdedSkeleton, {}, {}, big));
    TASK_CHECK(big.stats().residentBlocks == 3);
    (void)untouched;
    return 0;
}

int case_legacy_entry_rejects_streamed() {
    const std::string path = tempPath("legacy.csc");
    auto skeletonCore = makeTwoBoneSkeleton();
    auto core = makeBoundaryClip();
    TASK_CHECK(cal3d::writeStreamClip(path, *core, 0.3f));
    auto clip = cal3d::openStreamClip(path);
    TASK_CHECK(clip);

    auto anim = ISTD::make_shared<CalAnimation>(core, 1.0f, 0);
    cal3d::setAnimationStreamSource(*anim, clip);
    anim->time = 0.5f;

    CalMixer mixer;
    mixer.addAnimation(anim);
    CalSkeleton skeleton(skeletonCore);
    CalSkeleton before(skeletonCore);

    mixer.updateSkeleton(&skeleton, {}, {});
    TASK_CHECK(CalError::getLastErrorCode() == CalError::BAD_DATA_SOURCE);
    TASK_CHECK(sameSkeleton(before, skeleton)); // skeleton not modified

    // the streamed entry point on the same mixer works normally
    cal3d::CalStreamBlockCache cache(2);
    TASK_CHECK(mixer.updateSkeletonStreamed(&skeleton, {}, {}, cache));
    TASK_CHECK(!sameSkeleton(before, skeleton));

    // an in-memory-only mixer still uses the legacy entry point unchanged
    CalError::setLastError(CalError::OK, __FILE__, __LINE__, "");
    CalMixer plain;
    auto memAnim = ISTD::make_shared<CalAnimation>(core, 1.0f, 0);
    memAnim->time = 0.5f;
    plain.addAnimation(memAnim);
    CalSkeleton plainSkeleton(skeletonCore);
    plain.updateSkeleton(&plainSkeleton, {}, {});
    TASK_CHECK(CalError::getLastErrorCode() == CalError::OK);
    return 0;
}

// ===========================================================================
// registration (external linkage required by task_support/driver.c)
// ===========================================================================

} // namespace

const struct task_case task_cases[] = {
    {"block_sampling", "boundary_keys_exact_intervals", case_boundary_keys_exact_intervals},
    {"block_sampling", "empty_track_and_missing_track", case_empty_track_and_missing_track},
    {"block_sampling", "opposite_sign_quaternions", case_opposite_sign_quaternions},
    {"block_sampling", "open_zero_reads_and_repeat_hits", case_open_zero_reads_and_repeat_hits},
    {"block_sampling", "handbuilt_file_and_invalid_rejections", case_handbuilt_file_and_invalid_rejections},
    {"block_sampling", "writer_validation_and_missing_retry", case_writer_validation_and_missing_retry},

    {"cache_transactions", "shared_block_distinct_sources", case_shared_block_distinct_sources},
    {"cache_transactions", "capacity_exact_fit_and_shortfall", case_capacity_exact_fit_and_shortfall},
    {"cache_transactions", "corruption_failure_preserves_hot_blocks", case_corruption_failure_preserves_hot_blocks},
    {"cache_transactions", "reopen_releases_stale_identity", case_reopen_releases_stale_identity},
    {"cache_transactions", "lru_order_and_repeated_access", case_lru_order_and_repeated_access},

    {"mixed_pipeline", "mixed_priority_skin_matches_memory", case_mixed_priority_skin_matches_memory},
    {"mixed_pipeline", "two_characters_seek_and_recovery", case_two_characters_seek_and_recovery},
    {"mixed_pipeline", "legacy_entry_rejects_streamed", case_legacy_entry_rejects_streamed},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
