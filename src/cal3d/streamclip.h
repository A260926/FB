//****************************************************************************//
// streamclip.h                                                               //
// Time-blocked, randomly addressable animation files and a decoded-block     //
// cache shared by any number of characters.                                  //
//****************************************************************************//
//
// OVERVIEW
// --------
// A CalCoreAnimation can be written out as a set of fixed-duration blocks.
// Each block stores the raw keyframes that belong to the block's half-open
// time interval [start, end) plus the immediate predecessor and successor
// keyframes needed to interpolate the two ends of the interval.  Keyframes are
// never resampled at a block boundary, so sampling a streamed clip produces
// the same numbers as CalCoreTrack::getCurrentTransform() on the in-memory
// animation (to within ordinary floating point noise; the same operations are
// performed in the same order).
//
// The last block's end is the animation duration and it also contains every
// keyframe at or after the start of that block, including keyframes at the
// duration itself.  Times before the first keyframe or after the last
// keyframe clamp to the first/last keyframe exactly like
// CalCoreTrack::getCurrentTransform().
//
// An empty track (a CalCoreTrack with no keyframes) is written as a track
// with keyCount == 0 and samples to the identity local transform.  A bone that
// has no track at all is absent from the file and therefore keeps the bind
// pose when mixed.
//
// FILE FORMAT - VERSION 1
// -----------------------
// All multi-byte integer and floating point fields are fixed width and little
// endian.  Floats are the IEEE-754 binary32 bit patterns stored in a u32.
// Nothing is written as a native structure image; the layout below is exact
// and portable.  Tests may construct a file by hand using this table.
//
//   Header (64 bytes)
//     off  size field
//     0    8    magic: 'C','A','L','S','C','L','P','1'
//     8    4    u32 version                (must be 1)
//     12   4    u32 headerSize             (must be 64)
//     16   4    u32 flags                  (must be 0)
//     20   4    f32 blockDuration          (> 0, finite)
//     24   4    f32 duration               (>= 0, finite)
//     28   4    u32 trackCount
//     32   4    u32 blockCount             (>= 1)
//     36   4    u32 trackMetaOffset        (must be 64)
//     40   4    u32 trackMetaEntrySize     (must be 8)
//     44   4    u32 directoryOffset
//     48   4    u32 directoryEntrySize     (must be 32)
//     52   8    u64 fileSize               (must equal the real file size)
//     60   4    u32 reserved               (must be 0)
//
//   Track metadata (trackCount entries of 8 bytes, starting at trackMetaOffset)
//     off  size field
//     0    4    u32 boneId                 (0 .. 0x7fffffff, unique)
//     4    4    u32 reserved               (must be 0)
//
//   Directory (blockCount entries of 32 bytes, starting at directoryOffset)
//     off  size field
//     0    4    u32 blockIndex             (must equal the entry's index)
//     4    4    f32 start                  (inclusive; finite, >= 0)
//     8    4    f32 end                    (exclusive, except the last block)
//     12   8    u64 dataOffset             (start of the block data)
//     20   4    u32 byteLength             (<= 32 MiB)
//     24   4    u32 crc32                  (over exactly byteLength bytes)
//     28   4    u32 reserved               (must be 0)
//
//     Directory intervals must not overlap (entry[i].start >= entry[i-1].end)
//     and the first interval must start at 0.  Every interval must lie inside
//     [0, duration] and the last interval's end must equal duration.  Block
//     data offsets/lengths must lie inside the file and must not point into
//     the header, the track metadata or the directory.
//
//   Block data (at each directory entry's dataOffset, exactly byteLength bytes)
//     0    4    u32 trackCount             (must equal the header trackCount)
//     Then for each track in metadata order:
//       +0   4    u32 boneId               (must match the metadata entry)
//       +4   4    u32 keyCount
//       +8   keyCount * 32 bytes, each:
//            +0   4    f32 time
//            +4   4    f32 translation.x
//            +8   4    f32 translation.y
//            +12  4    f32 translation.z
//            +16  4    f32 rotation.x
//            +20  4    f32 rotation.y
//            +24  4    f32 rotation.z
//            +28  4    f32 rotation.w
//     There must be no trailing bytes.
//
//   Keyframe times within one track inside a block must be strictly
//   increasing.  Every value must be finite.  A keyframe list may contain the
//   predecessor of the block's start, the keys inside the block and the
//   successor of the block's end; borrowed keys across blocks are expected.
//
//   crc32 is the standard IEEE 802.3 CRC-32 (polynomial 0xEDB88320, initial
//   value 0xFFFFFFFF, final XOR 0xFFFFFFFF) over the block's bytes.
//
// OPENING
// -------
// CalStreamClip::open() only reads the header, the track metadata and the
// directory.  It deliberately does not touch block data.  Consequently
// keyframe-level problems (non-increasing keys, non-finite values, a block
// bone id that disagrees with the metadata) are reported when the block is
// first decoded, not when the file is opened.  Header/directory problems
// (bad magic/version, out-of-range offsets, overlapping intervals, duplicate
// track bone ids, illegal bone ids in metadata) are reported by open().
//
// The caller guarantees the file is not modified while a CalStreamClip is
// open.  open() validates the directory against the file size once.
//
// CACHE
// -----
// CalStreamClipCache stores fully decoded blocks and is shared between all
// characters/clips.  Capacity is measured in blocks (minimum 1); one block
// can contain many tracks.  For a frame the mixer:
//   * collects the distinct blocks needed by every active streaming animation,
//     in animation blend order (a repeated block counts once),
//   * fails before reading anything if that working set exceeds capacity,
//   * reads/decodes all missing blocks into temporaries; any read or checksum
//     failure aborts the frame without publishing anything,
//   * on success publishes all missing blocks together and evicts unused
//     least-recently-used blocks until the resident count is within capacity.
// The LRU position of a block is its *last* access in the frame's animation
// order.  Failures never change the skeleton, animation times, resident
// blocks, LRU order, hit count, peak or returned blocks; only readAttemptCount()
// may advance.
//
// A cache key is (CalStreamClip identity, block index).  The identity is a
// process-local serial number assigned when the file is opened.  It is not a
// path and not an address, so two different files with the same block index
// can never collide, and reopening a file after the old clip was destroyed
// gets a fresh identity and therefore a cache miss.
//
// No mmap, no external compression library and no network access is used.
//
//****************************************************************************//

#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <list>
#include <map>
#include <string>
#include <vector>

#include "cal3d/BoostToStd.h"
#include "cal3d/global.h"
#include "cal3d/quaternion.h"
#include "cal3d/transform.h"
#include "cal3d/vector.h"

class CalCoreAnimation;

namespace cal3d {
    namespace streamclip {
        const uint32_t kVersion = 1;
        const uint32_t kHeaderSize = 64;
        const uint32_t kTrackMetaEntrySize = 8;
        const uint32_t kDirectoryEntrySize = 32;
        const uint32_t kMaxBlockBytes = 32u * 1024u * 1024u;
        const uint32_t kMaxBoneId = 0x7fffffffu;
        const uint32_t kMaxBlocks = 10000000u;
    }
}

class CAL3D_API CalStreamClip {
public:
    // A single raw keyframe as stored in / decoded from a block.
    struct StreamKey {
        float time;
        CalVector translation;
        CalQuaternion rotation;
    };

    struct DecodedTrack {
        uint32_t boneId;
        std::vector<StreamKey> keys;
    };

    struct DecodedBlock {
        std::vector<DecodedTrack> tracks;
    };

    typedef ISTD::shared_ptr<const DecodedBlock> DecodedBlockPtr;

    struct TrackInfo {
        uint32_t boneId;
    };

    // Read-only open.  On failure returns null and, when error is non-null,
    // sets it to a human-readable reason.
    static ISTD::shared_ptr<CalStreamClip> open(const std::string& path, std::string* error = 0);

    bool isOpen() const {
        return m_open;
    }

    // Process-local identity used in cache keys.  Never zero for an open clip.
    uint64_t identity() const {
        return m_identity;
    }

    float duration() const {
        return m_duration;
    }

    float blockDuration() const {
        return m_blockDuration;
    }

    uint32_t blockCount() const {
        return m_blockCount;
    }

    const std::vector<TrackInfo>& tracks() const {
        return m_tracks;
    }

    // The block that must be decoded to sample the given animation time.
    // Negative (and NaN) times select block 0; times at/after duration select
    // the last block.  Callers normally obtain this from the mixer, but it is
    // public so callers can schedule reads themselves.
    unsigned blockIndexForTime(float time) const;

    // Read, checksum and decode one block.  The cache calls this; it is public
    // so custom schedulers can reuse the format implementation.  Returns false
    // on any read/validation failure and leaves out untouched.
    bool loadBlock(uint32_t blockIndex, DecodedBlockPtr& out, std::string* error = 0);

    // Sample one track exactly like CalCoreTrack::getCurrentTransform(),
    // including clamping at both ends.  An empty key list yields the identity
    // local transform.
    static cal3d::RotateTranslate sampleTrack(const DecodedTrack& track, float time);

private:
    CalStreamClip();
    bool initialize(const std::string& path, std::string* error);

    struct DirectoryEntry {
        uint32_t blockIndex;
        float start;
        float end;
        uint64_t dataOffset;
        uint32_t byteLength;
        uint32_t crc32;
    };

    bool m_open;
    std::ifstream m_file;
    uint64_t m_fileSize;
    uint64_t m_dataAreaStart;
    uint64_t m_identity;
    float m_duration;
    float m_blockDuration;
    uint32_t m_blockCount;
    std::vector<TrackInfo> m_tracks;
    std::vector<DirectoryEntry> m_directory;
};

CAL3D_PTR(CalStreamClip);

// A request for (clip, block).  clip keeps the file alive for the duration of
// the acquisition.
struct CAL3D_API CalStreamClipBlockRef {
    CalStreamClipPtr clip;
    uint32_t blockIndex;

    CalStreamClipBlockRef()
        : blockIndex(0)
    {}

    CalStreamClipBlockRef(const CalStreamClipPtr& c, uint32_t b)
        : clip(c)
        , blockIndex(b)
    {}
};

struct CAL3D_API CalStreamClipBlockKey {
    uint64_t sourceId;
    uint32_t blockIndex;

    CalStreamClipBlockKey()
        : sourceId(0)
        , blockIndex(0)
    {}

    CalStreamClipBlockKey(uint64_t s, uint32_t b)
        : sourceId(s)
        , blockIndex(b)
    {}

    bool operator==(const CalStreamClipBlockKey& rhs) const {
        return sourceId == rhs.sourceId && blockIndex == rhs.blockIndex;
    }

    bool operator!=(const CalStreamClipBlockKey& rhs) const {
        return !(*this == rhs);
    }

    bool operator<(const CalStreamClipBlockKey& rhs) const {
        if (sourceId != rhs.sourceId) {
            return sourceId < rhs.sourceId;
        }
        return blockIndex < rhs.blockIndex;
    }
};

class CAL3D_API CalStreamClipCache {
public:
    // Capacity is measured in decoded blocks.  Values below 1 are clamped to
    // 1.  The cache is not internally synchronised; callers serialise use.
    explicit CalStreamClipCache(size_t capacity);

    // Acquire every distinct block referenced by requests.  outBlocks is
    // aligned with requests (duplicate requests return the same block
    // pointer).  Returns true only if every requested block could be read,
    // checksummed, decoded and published.  On false, outBlocks is empty and
    // no observable cache state other than readAttemptCount() has changed.
    bool acquire(
        const std::vector<CalStreamClipBlockRef>& requests,
        std::vector<CalStreamClip::DecodedBlockPtr>& outBlocks);

    size_t capacity() const {
        return m_capacity;
    }

    size_t residentCount() const {
        return m_lru.size();
    }

    // Resident identities sorted by (sourceId, blockIndex).
    std::vector<CalStreamClipBlockKey> residentBlocks() const;

    // Resident identities in LRU order: index 0 is the least recently used,
    // the last element is the most recently used.
    std::vector<CalStreamClipBlockKey> lruOrder() const;

    // Number of distinct block requests served from the cache by successful
    // acquisitions (several animations asking for the same resident block in
    // one frame count once).
    uint64_t hitCount() const {
        return m_hitCount;
    }

    // Number of physical block read attempts, including attempts that failed.
    uint64_t readAttemptCount() const {
        return m_readAttempts;
    }

    // Largest number of decoded blocks held at once (resident before the frame
    // plus the frame's newly decoded temporaries) across successful
    // acquisitions.
    size_t peakDecodedBlocks() const {
        return m_peakDecodedBlocks;
    }

    const std::string& lastError() const {
        return m_lastError;
    }

private:
    struct Entry {
        CalStreamClipBlockKey key;
        CalStreamClip::DecodedBlockPtr block;
    };

    bool fail(const std::string& message);

    size_t m_capacity;
    std::list<Entry> m_lru; // front = least recently used
    std::map<CalStreamClipBlockKey, std::list<Entry>::iterator> m_index;
    uint64_t m_hitCount;
    uint64_t m_readAttempts;
    size_t m_peakDecodedBlocks;
    std::string m_lastError;
};

// Offline writer.  blockDuration must be finite and > 0.  Returns false on
// invalid input (non-finite values, duplicate/non-increasing keys, duplicate
// track bone ids, illegal bone ids, a block larger than 32 MiB) or on I/O
// failure.  The animation is not retained or modified.
CAL3D_API bool writeStreamClip(
    const CalCoreAnimation& animation,
    float blockDuration,
    const std::string& path,
    std::string* error = 0);

CAL3D_API bool writeStreamClip(
    const ISTD::shared_ptr<CalCoreAnimation>& animation,
    float blockDuration,
    const std::string& path,
    std::string* error = 0);
