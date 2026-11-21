//****************************************************************************//
// streamclip.cpp                                                             //
// Time-blocked animation files and the shared decoded-block cache.           //
//****************************************************************************//
// The on-disk layout is documented in streamclip.h.  Everything here
// serialises field by field in little endian; no native struct is ever
// written or read as a blob.                                                 //
//****************************************************************************//

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

#include "cal3d/streamclip.h"
#include "cal3d/coreanimation.h"
#include "cal3d/coretrack.h"
#include "cal3d/corekeyframe.h"

namespace {

const char kMagic[8] = { 'C', 'A', 'L', 'S', 'C', 'L', 'P', '1' };

// ---------------------------------------------------------------------------
// Little-endian primitives
// ---------------------------------------------------------------------------

inline uint32_t floatToBits(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

inline float bitsToFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

inline void putU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

inline void putU64(uint8_t* p, uint64_t v) {
    putU32(p, static_cast<uint32_t>(v));
    putU32(p + 4, static_cast<uint32_t>(v >> 32));
}

inline void putF32(uint8_t* p, float f) {
    putU32(p, floatToBits(f));
}

inline uint32_t getU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t getU64(const uint8_t* p) {
    return static_cast<uint64_t>(getU32(p))
        | (static_cast<uint64_t>(getU32(p + 4)) << 32);
}

inline float getF32(const uint8_t* p) {
    return bitsToFloat(getU32(p));
}

// ---------------------------------------------------------------------------
// CRC-32 (IEEE 802.3)
// ---------------------------------------------------------------------------

// Function-local static: its initialisation is thread-safe under C++11, so
// clips opened concurrently on different threads cannot race on the table.
uint32_t crc32(const uint8_t* data, size_t size) {
    static const struct Table {
        uint32_t entries[256];
        Table() {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int j = 0; j < 8; ++j) {
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                entries[i] = c;
            }
        }
    } table;

    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        c = table.entries[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

std::atomic<uint64_t> g_nextIdentity(1);

uint64_t allocateIdentity() {
    return g_nextIdentity.fetch_add(1);
}

bool finiteVector(const CalVector& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finiteQuaternion(const CalQuaternion& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

bool readExact(std::ifstream& file, uint64_t offset, uint8_t* out, size_t size) {
    if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) {
        return false;
    }
    file.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size));
    return file.good() || (file.eof() && static_cast<size_t>(file.gcount()) == size);
}

// A block reduced to the raw keys it must keep, in time order.
struct BlockTrackPlan {
    uint32_t boneId;
    std::vector<CalStreamClip::StreamKey> keys;
};

} // namespace

// ---------------------------------------------------------------------------
// CalStreamClip
// ---------------------------------------------------------------------------

CalStreamClip::CalStreamClip()
    : m_open(false)
    , m_fileSize(0)
    , m_dataAreaStart(0)
    , m_identity(0)
    , m_duration(0.0f)
    , m_blockDuration(0.0f)
    , m_blockCount(0)
{}

ISTD::shared_ptr<CalStreamClip> CalStreamClip::open(const std::string& path, std::string* error) {
    ISTD::shared_ptr<CalStreamClip> clip(new CalStreamClip());
    if (!clip->initialize(path, error)) {
        return ISTD::shared_ptr<CalStreamClip>();
    }
    return clip;
}

bool CalStreamClip::initialize(const std::string& path, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) {
            *error = why;
        }
        return false;
    };

    m_file.open(path.c_str(), std::ios::in | std::ios::binary);
    if (!m_file) {
        return fail("streamclip: cannot open file");
    }

    m_file.seekg(0, std::ios::end);
    if (!m_file) {
        return fail("streamclip: cannot determine file size");
    }
    std::streamoff end = m_file.tellg();
    if (end < 0) {
        return fail("streamclip: cannot determine file size");
    }
    m_fileSize = static_cast<uint64_t>(end);

    if (m_fileSize < cal3d::streamclip::kHeaderSize) {
        return fail("streamclip: file shorter than header");
    }

    uint8_t header[cal3d::streamclip::kHeaderSize];
    if (!readExact(m_file, 0, header, sizeof(header))) {
        return fail("streamclip: cannot read header");
    }

    if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
        return fail("streamclip: bad magic");
    }
    const uint32_t version = getU32(header + 8);
    if (version != cal3d::streamclip::kVersion) {
        return fail("streamclip: unsupported version");
    }
    if (getU32(header + 12) != cal3d::streamclip::kHeaderSize) {
        return fail("streamclip: bad header size");
    }
    if (getU32(header + 16) != 0) {
        return fail("streamclip: unsupported flags");
    }
    const float blockDuration = getF32(header + 20);
    if (!std::isfinite(blockDuration) || blockDuration <= 0.0f) {
        return fail("streamclip: invalid block duration");
    }
    const float duration = getF32(header + 24);
    if (!std::isfinite(duration) || duration < 0.0f) {
        return fail("streamclip: invalid duration");
    }
    const uint32_t trackCount = getU32(header + 28);
    const uint32_t blockCount = getU32(header + 32);
    const uint32_t trackMetaOffset = getU32(header + 36);
    const uint32_t trackMetaEntrySize = getU32(header + 40);
    const uint32_t directoryOffset = getU32(header + 44);
    const uint32_t directoryEntrySize = getU32(header + 48);
    const uint64_t storedFileSize = getU64(header + 52);
    if (getU32(header + 60) != 0) {
        return fail("streamclip: reserved header field is not zero");
    }
    if (trackMetaEntrySize != cal3d::streamclip::kTrackMetaEntrySize) {
        return fail("streamclip: bad track metadata entry size");
    }
    if (directoryEntrySize != cal3d::streamclip::kDirectoryEntrySize) {
        return fail("streamclip: bad directory entry size");
    }
    if (trackMetaOffset != cal3d::streamclip::kHeaderSize) {
        return fail("streamclip: track metadata must follow the header");
    }
    if (blockCount < 1 || blockCount > cal3d::streamclip::kMaxBlocks) {
        return fail("streamclip: invalid block count");
    }
    if (storedFileSize != m_fileSize) {
        return fail("streamclip: header file size does not match the file");
    }

    // Guard the metadata/directory regions against overflow and against
    // extending past the end of the file.
    const uint64_t metaBytes = static_cast<uint64_t>(trackCount) * trackMetaEntrySize;
    const uint64_t metaEnd = static_cast<uint64_t>(trackMetaOffset) + metaBytes;
    if (metaEnd > m_fileSize) {
        return fail("streamclip: track metadata out of bounds");
    }
    const uint64_t dirBytes = static_cast<uint64_t>(blockCount) * directoryEntrySize;
    const uint64_t dirEnd = static_cast<uint64_t>(directoryOffset) + dirBytes;
    if (dirEnd > m_fileSize) {
        return fail("streamclip: directory out of bounds");
    }
    if (directoryOffset < metaEnd) {
        return fail("streamclip: directory overlaps track metadata");
    }
    m_dataAreaStart = std::max<uint64_t>(dirEnd, metaEnd);

    // Track metadata.
    std::vector<uint8_t> meta(static_cast<size_t>(metaBytes));
    if (metaBytes > 0 && !readExact(m_file, trackMetaOffset, meta.data(), meta.size())) {
        return fail("streamclip: cannot read track metadata");
    }
    m_tracks.resize(trackCount);
    std::vector<uint32_t> seenBones;
    seenBones.reserve(trackCount);
    for (uint32_t i = 0; i < trackCount; ++i) {
        const uint8_t* p = meta.data() + static_cast<size_t>(i) * trackMetaEntrySize;
        const uint32_t boneId = getU32(p);
        if (getU32(p + 4) != 0) {
            return fail("streamclip: reserved track metadata field is not zero");
        }
        if (boneId > cal3d::streamclip::kMaxBoneId) {
            return fail("streamclip: illegal bone id in track metadata");
        }
        m_tracks[i].boneId = boneId;
        seenBones.push_back(boneId);
    }
    std::sort(seenBones.begin(), seenBones.end());
    if (std::adjacent_find(seenBones.begin(), seenBones.end()) != seenBones.end()) {
        return fail("streamclip: duplicate track bone id");
    }

    // Directory.
    std::vector<uint8_t> dir(static_cast<size_t>(dirBytes));
    if (!readExact(m_file, directoryOffset, dir.data(), dir.size())) {
        return fail("streamclip: cannot read directory");
    }
    m_directory.resize(blockCount);
    for (uint32_t i = 0; i < blockCount; ++i) {
        const uint8_t* p = dir.data() + static_cast<size_t>(i) * directoryEntrySize;
        DirectoryEntry& entry = m_directory[i];
        entry.blockIndex = getU32(p);
        entry.start = getF32(p + 4);
        entry.end = getF32(p + 8);
        entry.dataOffset = getU64(p + 12);
        entry.byteLength = getU32(p + 20);
        entry.crc32 = getU32(p + 24);
        if (getU32(p + 28) != 0) {
            return fail("streamclip: reserved directory field is not zero");
        }
        if (entry.blockIndex != i) {
            return fail("streamclip: directory block index mismatch");
        }
        if (!std::isfinite(entry.start) || !std::isfinite(entry.end)) {
            return fail("streamclip: non-finite directory interval");
        }
        if (entry.start < 0.0f || entry.end < entry.start) {
            return fail("streamclip: invalid directory interval");
        }
        if (entry.byteLength > cal3d::streamclip::kMaxBlockBytes) {
            return fail("streamclip: block exceeds 32 MiB");
        }
        if (entry.dataOffset < m_dataAreaStart) {
            return fail("streamclip: block data overlaps metadata");
        }
        if (entry.dataOffset > m_fileSize
            || static_cast<uint64_t>(entry.byteLength) > m_fileSize - entry.dataOffset) {
            return fail("streamclip: block data out of bounds");
        }
        if (i == 0) {
            if (entry.start != 0.0f) {
                return fail("streamclip: first block must start at zero");
            }
        } else {
            if (entry.start < m_directory[i - 1].end) {
                return fail("streamclip: directory intervals overlap");
            }
        }
    }
    if (m_directory.back().end != duration) {
        return fail("streamclip: last block must end at the duration");
    }
    for (uint32_t i = 0; i < blockCount; ++i) {
        if (m_directory[i].end > duration) {
            return fail("streamclip: directory interval exceeds duration");
        }
    }

    m_duration = duration;
    m_blockDuration = blockDuration;
    m_blockCount = blockCount;
    m_identity = allocateIdentity();
    m_open = true;
    return true;
}

unsigned CalStreamClip::blockIndexForTime(float time) const {
    // Negative, zero and NaN all clamp to the first block.
    if (!(time > 0.0f)) {
        return 0;
    }
    if (time >= m_duration) {
        return m_blockCount - 1;
    }

    // Contiguous (or at worst gapped) directory: find the last entry whose
    // start <= time.
    uint32_t lo = 0;
    uint32_t hi = m_blockCount;
    while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (m_directory[mid].start <= time) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

bool CalStreamClip::loadBlock(uint32_t blockIndex, DecodedBlockPtr& out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) {
            *error = why;
        }
        return false;
    };

    if (!m_open) {
        return fail("streamclip: clip is not open");
    }
    if (blockIndex >= m_blockCount) {
        return fail("streamclip: block index out of range");
    }

    const DirectoryEntry& entry = m_directory[blockIndex];
    if (entry.byteLength > cal3d::streamclip::kMaxBlockBytes) {
        return fail("streamclip: block exceeds 32 MiB");
    }

    std::vector<uint8_t> data(entry.byteLength);
    if (entry.byteLength > 0 && !readExact(m_file, entry.dataOffset, data.data(), data.size())) {
        return fail("streamclip: cannot read block data");
    }
    if (crc32(data.data(), data.size()) != entry.crc32) {
        return fail("streamclip: block checksum mismatch");
    }

    const uint8_t* p = data.data();
    size_t cursor = 0;
    size_t size = data.size();

    if (size < 4) {
        return fail("streamclip: truncated block");
    }
    const uint32_t blockTrackCount = getU32(p + cursor);
    cursor += 4;
    if (blockTrackCount != m_tracks.size()) {
        return fail("streamclip: block track count mismatch");
    }

    ISTD::shared_ptr<DecodedBlock> block = ISTD::make_shared<DecodedBlock>();
    block->tracks.resize(blockTrackCount);

    for (uint32_t t = 0; t < blockTrackCount; ++t) {
        if (size - cursor < 8) {
            return fail("streamclip: truncated track header");
        }
        const uint32_t boneId = getU32(p + cursor);
        const uint32_t keyCount = getU32(p + cursor + 4);
        cursor += 8;
        if (boneId != m_tracks[t].boneId) {
            return fail("streamclip: block bone id disagrees with metadata");
        }
        if (static_cast<uint64_t>(keyCount) * 32u > size - cursor) {
            return fail("streamclip: truncated keyframes");
        }

        DecodedTrack& track = block->tracks[t];
        track.boneId = boneId;
        track.keys.resize(keyCount);

        bool havePrevious = false;
        float previousTime = 0.0f;
        for (uint32_t k = 0; k < keyCount; ++k) {
            const uint8_t* kp = p + cursor;
            StreamKey& key = track.keys[k];
            key.time = getF32(kp + 0);
            key.translation = CalVector(getF32(kp + 4), getF32(kp + 8), getF32(kp + 12));
            key.rotation = CalQuaternion(
                getF32(kp + 16), getF32(kp + 20), getF32(kp + 24), getF32(kp + 28));
            cursor += 32;

            if (!std::isfinite(key.time)
                || !finiteVector(key.translation)
                || !finiteQuaternion(key.rotation)) {
                return fail("streamclip: non-finite keyframe value");
            }
            if (havePrevious && !(key.time > previousTime)) {
                return fail("streamclip: keyframe times are not increasing");
            }
            havePrevious = true;
            previousTime = key.time;
        }
    }

    if (cursor != size) {
        return fail("streamclip: trailing bytes in block");
    }

    out = block;
    return true;
}

cal3d::RotateTranslate CalStreamClip::sampleTrack(const DecodedTrack& track, float time) {
    const std::vector<StreamKey>& keys = track.keys;
    if (keys.empty()) {
        return cal3d::RotateTranslate();
    }

    // upper_bound: first key strictly later than time.
    size_t lo = 0;
    size_t hi = keys.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (keys[mid].time <= time) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo == keys.size()) {
        const StreamKey& k = keys.back();
        return cal3d::RotateTranslate(k.rotation, k.translation);
    }
    if (lo == 0) {
        const StreamKey& k = keys.front();
        return cal3d::RotateTranslate(k.rotation, k.translation);
    }

    const StreamKey& before = keys[lo - 1];
    const StreamKey& after = keys[lo];

    float blendFactor = 0.0f;
    if (after.time != before.time) {
        blendFactor = (time - before.time) / (after.time - before.time);
    }

    return blend(
        blendFactor,
        cal3d::RotateTranslate(before.rotation, before.translation),
        cal3d::RotateTranslate(after.rotation, after.translation));
}

// ---------------------------------------------------------------------------
// CalStreamClipCache
// ---------------------------------------------------------------------------

CalStreamClipCache::CalStreamClipCache(size_t capacity)
    : m_capacity(capacity < 1 ? 1 : capacity)
    , m_hitCount(0)
    , m_readAttempts(0)
    , m_peakDecodedBlocks(0)
{}

bool CalStreamClipCache::fail(const std::string& message) {
    m_lastError = message;
    return false;
}

std::vector<CalStreamClipBlockKey> CalStreamClipCache::residentBlocks() const {
    std::vector<CalStreamClipBlockKey> keys;
    keys.reserve(m_index.size());
    for (auto i = m_index.begin(); i != m_index.end(); ++i) {
        keys.push_back(i->first);
    }
    return keys;
}

std::vector<CalStreamClipBlockKey> CalStreamClipCache::lruOrder() const {
    std::vector<CalStreamClipBlockKey> keys;
    keys.reserve(m_lru.size());
    for (auto i = m_lru.begin(); i != m_lru.end(); ++i) {
        keys.push_back(i->key);
    }
    return keys;
}

bool CalStreamClipCache::acquire(
    const std::vector<CalStreamClipBlockRef>& requests,
    std::vector<CalStreamClip::DecodedBlockPtr>& outBlocks
) {
    m_lastError.clear();
    outBlocks.clear();

    // Resolve the distinct working set and the last-access order for LRU.
    // lastUse records each distinct key's *last* position in the frame.
    std::map<CalStreamClipBlockKey, size_t> lastUse;
    std::vector<CalStreamClipBlockKey> distinct;
    for (size_t i = 0; i < requests.size(); ++i) {
        const CalStreamClipBlockRef& ref = requests[i];
        if (!ref.clip) {
            return fail("streamclip: null clip in cache request");
        }
        CalStreamClipBlockKey key(ref.clip->identity(), ref.blockIndex);
        if (lastUse.find(key) == lastUse.end()) {
            distinct.push_back(key);
        }
        lastUse[key] = i;
    }

    if (distinct.size() > m_capacity) {
        return fail("streamclip: working set exceeds cache capacity");
    }

    // Count hits: one per distinct resident block used this frame.
    size_t hits = 0;
    for (size_t d = 0; d < distinct.size(); ++d) {
        if (m_index.find(distinct[d]) != m_index.end()) {
            ++hits;
        }
    }

    // Determine which are resident and which must be read.  This does not
    // mutate anything: every miss is decoded into a temporary first.
    std::vector<CalStreamClip::DecodedBlockPtr> missing;
    std::vector<CalStreamClipBlockKey> missingKeys;
    for (size_t d = 0; d < distinct.size(); ++d) {
        const CalStreamClipBlockKey& key = distinct[d];
        if (m_index.find(key) != m_index.end()) {
            continue;
        }
        ++m_readAttempts;

        // Locate the corresponding request to get the clip handle.
        CalStreamClipPtr clip;
        for (size_t i = 0; i < requests.size(); ++i) {
            if (requests[i].clip->identity() == key.sourceId) {
                clip = requests[i].clip;
                break;
            }
        }
        if (!clip) {
            return fail("streamclip: could not resolve clip for block");
        }

        CalStreamClip::DecodedBlockPtr decoded;
        std::string error;
        if (!clip->loadBlock(key.blockIndex, decoded, &error)) {
            return fail(error);
        }
        missing.push_back(decoded);
        missingKeys.push_back(key);
    }

    // Success: publish the new blocks.
    const size_t residentBefore = m_lru.size();
    const size_t peak = residentBefore + missing.size();
    if (peak > m_peakDecodedBlocks) {
        m_peakDecodedBlocks = peak;
    }
    m_hitCount += hits;

    for (size_t i = 0; i < missingKeys.size(); ++i) {
        Entry entry;
        entry.key = missingKeys[i];
        entry.block = missing[i];
        m_lru.push_back(entry);
        m_index[entry.key] = --m_lru.end();
    }

    // Touch every requested distinct block in last-use order.  A block
    // accessed several times in the frame moves to MRU only at its last use.
    std::vector<std::pair<size_t, CalStreamClipBlockKey> > touchOrder;
    touchOrder.reserve(distinct.size());
    for (auto i = lastUse.begin(); i != lastUse.end(); ++i) {
        touchOrder.push_back(std::make_pair(i->second, i->first));
    }
    std::sort(touchOrder.begin(), touchOrder.end());
    for (size_t i = 0; i < touchOrder.size(); ++i) {
        auto found = m_index.find(touchOrder[i].second);
        if (found == m_index.end()) {
            continue; // defensive: cannot happen
        }
        m_lru.splice(m_lru.end(), m_lru, found->second);
    }

    // Evict only blocks that were not used this frame, oldest first.
    while (m_lru.size() > m_capacity) {
        const CalStreamClipBlockKey& oldest = m_lru.front().key;
        if (lastUse.find(oldest) != lastUse.end()) {
            // Should not happen (used keys are at the back and fit the
            // capacity), but never evict a live block.
            break;
        }
        m_index.erase(oldest);
        m_lru.pop_front();
    }

    // Build the aligned result.
    outBlocks.reserve(requests.size());
    for (size_t i = 0; i < requests.size(); ++i) {
        CalStreamClipBlockKey key(requests[i].clip->identity(), requests[i].blockIndex);
        auto found = m_index.find(key);
        if (found == m_index.end()) {
            return fail("streamclip: internal error resolving cached block");
        }
        outBlocks.push_back(found->second->block);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

namespace {

struct PendingBlock {
    float start;
    float end;
    std::vector<BlockTrackPlan> tracks;
    std::vector<uint8_t> payload;
    uint32_t crc;
    uint64_t offset;
};

bool buildBlockPayload(const std::vector<BlockTrackPlan>& tracks, std::vector<uint8_t>& payload) {
    size_t total = 4;
    for (size_t i = 0; i < tracks.size(); ++i) {
        total += 8 + tracks[i].keys.size() * 32u;
    }
    if (total > cal3d::streamclip::kMaxBlockBytes) {
        return false;
    }
    payload.resize(total);
    uint8_t* p = payload.data();
    putU32(p, static_cast<uint32_t>(tracks.size()));
    size_t cursor = 4;
    for (size_t i = 0; i < tracks.size(); ++i) {
        putU32(p + cursor, tracks[i].boneId);
        putU32(p + cursor + 4, static_cast<uint32_t>(tracks[i].keys.size()));
        cursor += 8;
        for (size_t k = 0; k < tracks[i].keys.size(); ++k) {
            const CalStreamClip::StreamKey& key = tracks[i].keys[k];
            putF32(p + cursor + 0, key.time);
            putF32(p + cursor + 4, key.translation.x);
            putF32(p + cursor + 8, key.translation.y);
            putF32(p + cursor + 12, key.translation.z);
            putF32(p + cursor + 16, key.rotation.x);
            putF32(p + cursor + 20, key.rotation.y);
            putF32(p + cursor + 24, key.rotation.z);
            putF32(p + cursor + 28, key.rotation.w);
            cursor += 32;
        }
    }
    return true;
}

} // namespace

bool writeStreamClip(
    const CalCoreAnimation& animation,
    float blockDuration,
    const std::string& path,
    std::string* error
) {
    auto fail = [&](const std::string& why) {
        if (error) {
            *error = why;
        }
        return false;
    };

    if (!std::isfinite(blockDuration) || blockDuration <= 0.0f) {
        return fail("streamclip: block duration must be finite and positive");
    }
    const float duration = animation.duration;
    if (!std::isfinite(duration) || duration < 0.0f) {
        return fail("streamclip: animation duration must be finite and non-negative");
    }

    // Validate track metadata up front.
    std::vector<uint32_t> boneIds;
    boneIds.reserve(animation.tracks.size());
    for (size_t i = 0; i < animation.tracks.size(); ++i) {
        const CalCoreTrack& track = animation.tracks[i];
        if (track.coreBoneId > cal3d::streamclip::kMaxBoneId) {
            return fail("streamclip: illegal bone id");
        }
        boneIds.push_back(track.coreBoneId);
    }
    {
        std::vector<uint32_t> sorted = boneIds;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
            return fail("streamclip: duplicate track bone id");
        }
    }

    // Validate keys and copy them into a plain representation.
    struct SourceTrack {
        uint32_t boneId;
        std::vector<CalStreamClip::StreamKey> keys;
    };
    std::vector<SourceTrack> sourceTracks(animation.tracks.size());
    for (size_t i = 0; i < animation.tracks.size(); ++i) {
        const CalCoreTrack& track = animation.tracks[i];
        SourceTrack& dst = sourceTracks[i];
        dst.boneId = track.coreBoneId;
        dst.keys.reserve(track.keyframes.size());
        bool havePrevious = false;
        float previousTime = 0.0f;
        for (size_t k = 0; k < track.keyframes.size(); ++k) {
            const CalCoreKeyframe& kf = track.keyframes[k];
            if (!std::isfinite(kf.time)
                || !finiteVector(kf.transform.translation)
                || !finiteQuaternion(kf.transform.rotation)) {
                return fail("streamclip: non-finite keyframe value");
            }
            if (havePrevious && !(kf.time > previousTime)) {
                return fail("streamclip: keyframe times are not increasing");
            }
            havePrevious = true;
            previousTime = kf.time;

            CalStreamClip::StreamKey key;
            key.time = kf.time;
            key.translation = kf.transform.translation;
            key.rotation = kf.transform.rotation;
            dst.keys.push_back(key);
        }
    }

    // Compute contiguous block boundaries.
    const double bd = static_cast<double>(blockDuration);
    double rawCount = std::ceil(static_cast<double>(duration) / bd);
    if (!std::isfinite(rawCount) || rawCount < 1.0) {
        rawCount = 1.0;
    }
    if (rawCount > static_cast<double>(cal3d::streamclip::kMaxBlocks)) {
        return fail("streamclip: too many blocks");
    }
    const uint32_t blockCount = static_cast<uint32_t>(rawCount);

    std::vector<float> boundaries(blockCount + 1);
    boundaries[0] = 0.0f;
    for (uint32_t i = 1; i <= blockCount; ++i) {
        double raw = (i == blockCount) ? static_cast<double>(duration)
                                       : static_cast<double>(i) * bd;
        if (raw > static_cast<double>(duration)) {
            raw = static_cast<double>(duration);
        }
        float boundary = static_cast<float>(raw);
        if (boundary < boundaries[i - 1]) {
            boundary = boundaries[i - 1];
        }
        boundaries[i] = boundary;
    }

    // Plan each block.
    std::vector<PendingBlock> blocks(blockCount);
    for (uint32_t b = 0; b < blockCount; ++b) {
        PendingBlock& block = blocks[b];
        block.start = boundaries[b];
        block.end = boundaries[b + 1];
        block.tracks.resize(sourceTracks.size());

        for (size_t t = 0; t < sourceTracks.size(); ++t) {
            const SourceTrack& src = sourceTracks[t];
            BlockTrackPlan& plan = block.tracks[t];
            plan.boneId = src.boneId;

            // Determine the inclusive range of source keys that live in this
            // block.  The first block claims everything before its successor
            // boundary so that negative-time keys still clamp correctly.
            size_t firstIncluded;
            size_t lastIncluded; // one past
            if (blockCount == 1) {
                firstIncluded = 0;
                lastIncluded = src.keys.size();
            } else if (b == 0) {
                firstIncluded = 0;
                size_t i = 0;
                while (i < src.keys.size() && src.keys[i].time < boundaries[1]) {
                    ++i;
                }
                lastIncluded = i;
            } else if (b + 1 == blockCount) {
                size_t i = 0;
                while (i < src.keys.size() && src.keys[i].time < boundaries[b]) {
                    ++i;
                }
                firstIncluded = i;
                lastIncluded = src.keys.size();
            } else {
                size_t i = 0;
                while (i < src.keys.size() && src.keys[i].time < boundaries[b]) {
                    ++i;
                }
                firstIncluded = i;
                while (i < src.keys.size() && src.keys[i].time < boundaries[b + 1]) {
                    ++i;
                }
                lastIncluded = i;
            }

            // Predecessor key (only meaningful when the block does not start
            // at the beginning of the animation).
            size_t predecessor = firstIncluded;
            if (b > 0 && blockCount > 1) {
                size_t i = 0;
                while (i < src.keys.size() && src.keys[i].time < boundaries[b]) {
                    ++i;
                }
                if (i > 0) {
                    predecessor = i - 1;
                } else {
                    predecessor = firstIncluded;
                }
            }

            // Successor key (the first key at/after the next boundary), except
            // for the last block, which already holds everything remaining.
            size_t successor = lastIncluded;
            if (b + 1 < blockCount) {
                size_t i = 0;
                while (i < src.keys.size() && src.keys[i].time < boundaries[b + 1]) {
                    ++i;
                }
                successor = i;
            }

            std::vector<CalStreamClip::StreamKey> keys;
            if (b > 0 && blockCount > 1 && predecessor < firstIncluded) {
                keys.push_back(src.keys[predecessor]);
            }
            for (size_t i = firstIncluded; i < lastIncluded; ++i) {
                keys.push_back(src.keys[i]);
            }
            if (b + 1 < blockCount && successor < src.keys.size()
                && (keys.empty() || src.keys[successor].time > keys.back().time)) {
                keys.push_back(src.keys[successor]);
            }
            plan.keys.swap(keys);
        }

        if (!buildBlockPayload(block.tracks, block.payload)) {
            return fail("streamclip: block exceeds 32 MiB");
        }
        block.crc = crc32(block.payload.data(), block.payload.size());
    }

    // Lay out the file.
    const uint64_t trackMetaOffset = cal3d::streamclip::kHeaderSize;
    const uint64_t trackMetaBytes = static_cast<uint64_t>(sourceTracks.size())
        * cal3d::streamclip::kTrackMetaEntrySize;
    const uint64_t directoryOffset = trackMetaOffset + trackMetaBytes;
    const uint64_t directoryBytes = static_cast<uint64_t>(blockCount)
        * cal3d::streamclip::kDirectoryEntrySize;
    uint64_t cursor = directoryOffset + directoryBytes;

    for (uint32_t b = 0; b < blockCount; ++b) {
        blocks[b].offset = cursor;
        cursor += blocks[b].payload.size();
    }
    const uint64_t fileSize = cursor;

    if (directoryOffset > 0xFFFFFFFFull || fileSize > 0xFFFFFFFFull) {
        // Offsets in the durable format are 32 bit; block offsets are 64 bit,
        // but the directory/track metadata offsets are u32.
        return fail("streamclip: file too large");
    }

    std::vector<uint8_t> out(static_cast<size_t>(fileSize));
    uint8_t* header = out.data();
    std::memcpy(header, kMagic, sizeof(kMagic));
    putU32(header + 8, cal3d::streamclip::kVersion);
    putU32(header + 12, cal3d::streamclip::kHeaderSize);
    putU32(header + 16, 0);
    putF32(header + 20, blockDuration);
    putF32(header + 24, duration);
    putU32(header + 28, static_cast<uint32_t>(sourceTracks.size()));
    putU32(header + 32, blockCount);
    putU32(header + 36, static_cast<uint32_t>(trackMetaOffset));
    putU32(header + 40, cal3d::streamclip::kTrackMetaEntrySize);
    putU32(header + 44, static_cast<uint32_t>(directoryOffset));
    putU32(header + 48, cal3d::streamclip::kDirectoryEntrySize);
    putU64(header + 52, fileSize);
    putU32(header + 60, 0);

    uint8_t* meta = out.data() + trackMetaOffset;
    for (size_t i = 0; i < sourceTracks.size(); ++i) {
        putU32(meta + i * cal3d::streamclip::kTrackMetaEntrySize, sourceTracks[i].boneId);
        putU32(meta + i * cal3d::streamclip::kTrackMetaEntrySize + 4, 0);
    }

    uint8_t* dir = out.data() + directoryOffset;
    for (uint32_t b = 0; b < blockCount; ++b) {
        uint8_t* p = dir + static_cast<size_t>(b) * cal3d::streamclip::kDirectoryEntrySize;
        putU32(p, b);
        putF32(p + 4, blocks[b].start);
        putF32(p + 8, blocks[b].end);
        putU64(p + 12, blocks[b].offset);
        putU32(p + 20, static_cast<uint32_t>(blocks[b].payload.size()));
        putU32(p + 24, blocks[b].crc);
        putU32(p + 28, 0);
    }

    for (uint32_t b = 0; b < blockCount; ++b) {
        if (!blocks[b].payload.empty()) {
            std::memcpy(out.data() + blocks[b].offset,
                        blocks[b].payload.data(),
                        blocks[b].payload.size());
        }
    }

    std::ofstream file(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file) {
        return fail("streamclip: cannot create output file");
    }
    file.write(reinterpret_cast<const char*>(out.data()),
               static_cast<std::streamsize>(out.size()));
    if (!file) {
        return fail("streamclip: cannot write output file");
    }
    return true;
}

bool writeStreamClip(
    const ISTD::shared_ptr<CalCoreAnimation>& animation,
    float blockDuration,
    const std::string& path,
    std::string* error
) {
    if (!animation) {
        if (error) {
            *error = "streamclip: null animation";
        }
        return false;
    }
    return writeStreamClip(*animation, blockDuration, path, error);
}
