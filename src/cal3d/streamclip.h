//****************************************************************************//
// streamclip.h                                                               //
// Blocked, read-only streaming animation clips with a shared block cache.    //
// On-disk layout is documented in streamclip_format.txt next to this file.   //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cal3d/BoostToStd.h"
#include "cal3d/global.h"
#include "cal3d/transform.h"

class CalCoreAnimation;
class CalAnimation;
class CalSkeleton;
struct BoneTransformAdjustment;
struct BoneScaleAdjustment;

namespace cal3d {

static const unsigned STREAMCLIP_FORMAT_VERSION = 1;
static const unsigned STREAMCLIP_MAX_BLOCK_BYTES = 32u * 1024u * 1024u;
static const unsigned STREAMCLIP_KEY_BYTES = 32u;

struct StreamBlockInfo {
    unsigned blockIndex;
    float blockStart;
    float blockEnd;
};

struct StreamCacheStats {
    size_t capacity;          // maximum resident decoded blocks
    size_t residentBlocks;    // blocks currently held
    uint64_t hitCount;        // frame requests served from resident blocks
    uint64_t readAttempts;    // physical block read attempts (failures count)
    size_t decodePeak;        // high-water mark of simultaneously decoded blocks
};

class CalStreamClip;
using CalStreamClipPtr = ISTD::shared_ptr<CalStreamClip>;

// One (source, block) pair needed by a frame.
struct StreamBlockRequest {
    const CalStreamClip* clip;
    unsigned blockIndex;
};

// Writes a blocked clip for animation into path.  blockDuration must be
// positive and finite.  Returns false (and sets CalError) on failure; any
// partial file at path is removed on failure.
CAL3D_API bool writeStreamClip(
    const std::string& path,
    const CalCoreAnimation& animation,
    float blockDuration);

// Opens a blocked clip read-only.  Only the header, the directory and track
// metadata are read and validated.  No block payload is read or decoded.
// Returns a null pointer (and sets CalError) if the file is missing or
// invalid.  The caller guarantees the file does not change while open.
CAL3D_API CalStreamClipPtr openStreamClip(const std::string& path);

// Shared, capacity-bounded cache of decoded animation blocks.  One block may
// carry several tracks; capacity counts blocks and is clamped to at least 1.
class CAL3D_API CalStreamBlockCache {
public:
    explicit CalStreamBlockCache(size_t capacity);
    ~CalStreamBlockCache();

    CalStreamBlockCache(const CalStreamBlockCache&) = delete;
    CalStreamBlockCache& operator=(const CalStreamBlockCache&) = delete;

    StreamCacheStats stats() const;

    // Ids of the resident blocks as (source id, block index) pairs, oldest
    // first.  The source id is the identity reported by CalStreamClip::id().
    std::vector<std::pair<uint64_t, unsigned>> residentBlockIds() const;

    // Frame transaction.  beginTransaction collects all distinct requested
    // blocks, fails (without reading or evicting anything) if the working set
    // exceeds the capacity, and otherwise loads every missing block into
    // temporary storage first.  A read/checksum failure likewise changes
    // nothing.  On success the mixer samples, then commitTransaction publishes
    // the new blocks, replays LRU accesses and evicts unused blocks.
    bool beginTransaction(const std::vector<StreamBlockRequest>& requests);
    void commitTransaction();
    void abortTransaction();

private:
    friend class CalStreamClip;
    struct Impl;
    ISTD::shared_ptr<Impl> m_impl;
};

// One open blocked clip: directory, file handle and shared cache ownership.
// It never retains the CalCoreAnimation used to write the file and never
// reconstructs a full CalCoreAnimation or a full decoded keyframe set.
class CAL3D_API CalStreamClip {
public:
    ~CalStreamClip();

    // Stable identity of this open object.  Unique for the object's lifetime;
    // reopening a file allocates a fresh id so stale cache entries can never
    // match a different open object.  Not derived from the path or an address.
    uint64_t id() const;

    unsigned blockCount() const;
    unsigned trackCount() const;
    float duration() const;
    float blockDuration() const;

    // Directory view (no block payloads are touched).
    StreamBlockInfo blockInfo(unsigned blockIndex) const;
    // Bone ids animated by this clip; absent ids use the bind pose.
    std::vector<unsigned> trackBoneIds() const;
    // Block that owns time, clamped to the first/last block outside [0,d].
    unsigned blockIndexForTime(float time) const;

    // Samples one track at time using only decoded block data.  An empty
    // track (or a track with no keys in the resident block) returns the
    // identity local transform; missing tracks are the mixer's responsibility
    // (bind pose).  End times clamp like CalCoreTrack::getCurrentTransform.
    // Returns false on a read or checksum failure without mutating cache
    // residence or LRU order.
    bool sampleTrack(
        unsigned coreBoneId,
        float time,
        CalStreamBlockCache& cache,
        RotateTranslate& result) const;

    // Physical block reads performed through this open object.
    uint64_t readAttempts() const;

private:
    CalStreamClip();
    CalStreamClip(const CalStreamClip&) = delete;
    CalStreamClip& operator=(const CalStreamClip&) = delete;

    friend CalStreamClipPtr openStreamClip(const std::string& path);
    friend struct CalStreamBlockCache::Impl;

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    // Reads, verifies and decodes one block.  Increments readAttempts even on
    // failure and never touches cache state.
    bool decodeBlock(unsigned blockIndex, struct DecodedBlock& decoded) const;
};

// Attaches a streamed source to an existing in-memory CalAnimation.  The
// animation keeps its weight, priority, time and rampValue; its
// CalCoreAnimation pointer is cleared, so the legacy updateSkeleton entry
// point rejects it explicitly instead of touching the skeleton.
CAL3D_API void setAnimationStreamSource(
    CalAnimation& animation,
    const CalStreamClipPtr& clip);

CAL3D_API bool animationHasStreamSource(const CalAnimation& animation);

} // namespace cal3d
