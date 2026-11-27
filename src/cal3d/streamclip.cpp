//****************************************************************************//
// streamclip.cpp                                                             //
// Blocked, read-only streaming animation clips with a shared block cache.    //
// On-disk layout is documented in streamclip_format.txt next to the header.  //
//****************************************************************************//

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cal3d/streamclip.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <utility>

#include "cal3d/animation.h"
#include "cal3d/coreanimation.h"
#include "cal3d/coretrack.h"
#include "cal3d/corekeyframe.h"
#include "cal3d/error.h"
#include "cal3d/mixer.h"
#include "cal3d/quaternion.h"
#include "cal3d/skeleton.h"
#include "cal3d/bone.h"

namespace cal3d {

namespace {

// ---------------------------------------------------------------------------
// fixed-width little-endian serialization (no native struct dumps)
// ---------------------------------------------------------------------------

void putU32(std::string& out, uint32_t v) {
    char b[4];
    b[0] = char(v & 0xFFu);
    b[1] = char((v >> 8) & 0xFFu);
    b[2] = char((v >> 16) & 0xFFu);
    b[3] = char((v >> 24) & 0xFFu);
    out.append(b, 4);
}

void putU64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(char((v >> (8 * i)) & 0xFFu));
    }
}

void putF32(std::string& out, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    putU32(out, bits);
}

bool getU32(const uint8_t*& p, const uint8_t* end, uint32_t& v) {
    if (p + 4 > end) {
        return false;
    }
    v = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    p += 4;
    return true;
}

bool getU64(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    if (p + 8 > end) {
        return false;
    }
    v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= uint64_t(p[i]) << (8 * i);
    }
    p += 8;
    return true;
}

bool getF32(const uint8_t*& p, const uint8_t* end, float& v) {
    uint32_t bits;
    if (!getU32(p, end, bits)) {
        return false;
    }
    std::memcpy(&v, &bits, 4);
    return true;
}

bool finiteFloat(float v) {
    return std::isfinite(v);
}

// ---------------------------------------------------------------------------
// CRC-32 (IEEE 802.3, reflected poly 0xEDB88320)
// ---------------------------------------------------------------------------

uint32_t crc32(const uint8_t* data, size_t length, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool initialized = false;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        initialized = true;
    }
    for (size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

const uint32_t FILE_MAGIC = 0x31435343u; // file bytes 'C','S','C','1'
const uint32_t MAX_BONE_ID = 0x7FFFFFFFu;
const size_t HEADER_BYTES = 40;
const size_t BLOCK_ENTRY_BYTES = 32;
const size_t TRACK_ENTRY_BYTES = 8;

struct BlockDirEntry {
    uint32_t blockIndex;
    float blockStart;
    float blockEnd;
    uint64_t offset;
    uint32_t length;
    uint32_t checksum;
};

struct TrackDirEntry {
    uint32_t coreBoneId;
    uint32_t keyCount;
};

uint64_t nextSourceId() {
    static std::atomic<uint64_t> counter{0};
    return ++counter;
}

} // namespace

// ---------------------------------------------------------------------------
// decoded block data (plain keyframes, never a CalCoreAnimation)
// ---------------------------------------------------------------------------

struct DecodedKey {
    float time;
    RotateTranslate transform;
};

struct DecodedBlock {
    // bone id -> strictly increasing decoded keys; a bone with an empty key
    // list samples as the identity local transform
    std::map<unsigned, std::vector<DecodedKey>> tracks;
};

struct BlockKey {
    uint64_t sourceId;
    unsigned blockIndex;

    bool operator<(const BlockKey& other) const {
        if (sourceId != other.sourceId) {
            return sourceId < other.sourceId;
        }
        return blockIndex < other.blockIndex;
    }

    bool operator==(const BlockKey& other) const {
        return sourceId == other.sourceId && blockIndex == other.blockIndex;
    }
};

// ---------------------------------------------------------------------------
// cache implementation
// ---------------------------------------------------------------------------

struct CalStreamBlockCache::Impl {
    struct Entry {
        DecodedBlock block;
        std::list<BlockKey>::iterator lruIt;
    };

    explicit Impl(size_t cap)
        : capacity(cap)
        , inTransaction(false)
        , hitCount(0)
        , readAttempts(0)
        , decodePeak(0)
    {}

    size_t capacity;
    std::map<BlockKey, Entry> resident;
    std::list<BlockKey> lru; // front = least recently used

    // frame transaction state
    bool inTransaction;
    std::vector<BlockKey> needed;
    std::set<BlockKey> preResident;
    std::vector<BlockKey> touchOrder;
    std::map<BlockKey, DecodedBlock> pending;
    std::vector<StreamBlockRequest> distinctRequests;

    uint64_t hitCount;
    uint64_t readAttempts;
    size_t decodePeak;

    BlockKey makeKey(const StreamBlockRequest& req) const {
        return BlockKey{req.clip->id(), req.blockIndex};
    }

    bool beginTransaction(const std::vector<StreamBlockRequest>& requests);
    const DecodedBlock* resolve(const BlockKey& key) const;
    void touch(const BlockKey& key);
    void commitTransaction();
    void abortTransaction();
};

bool CalStreamBlockCache::Impl::beginTransaction(const std::vector<StreamBlockRequest>& requests) {
    if (inTransaction) {
        return false;
    }

    // collect the distinct blocks, keeping first-seen (blend) order
    std::set<BlockKey> seen;
    needed.clear();
    distinctRequests.clear();
    for (size_t i = 0; i < requests.size(); ++i) {
        const BlockKey key = makeKey(requests[i]);
        if (seen.insert(key).second) {
            needed.push_back(key);
            distinctRequests.push_back(requests[i]);
        }
    }

    // The distinct working set of a frame must fit the capacity.  An
    // over-capacity frame fails before any read and without disturbing
    // resident blocks or LRU order; unused resident blocks are only reclaimed
    // at commit time, never speculatively.
    if (needed.size() > capacity) {
        needed.clear();
        distinctRequests.clear();
        return false;
    }

    preResident.clear();
    for (std::map<BlockKey, Entry>::const_iterator it = resident.begin(); it != resident.end(); ++it) {
        preResident.insert(it->first);
    }

    pending.clear();
    touchOrder.clear();

    // Decode every missing block into temporary storage first; nothing is
    // published until all of them succeed.
    for (size_t i = 0; i < distinctRequests.size(); ++i) {
        const BlockKey key = makeKey(distinctRequests[i]);
        if (resident.find(key) != resident.end()) {
            continue;
        }

        ++readAttempts;
        DecodedBlock decoded;
        if (!distinctRequests[i].clip->decodeBlock(distinctRequests[i].blockIndex, decoded)) {
            // Failure: drop temporaries, keep every resident block and LRU
            // order untouched.  The read attempt above still counts.
            pending.clear();
            needed.clear();
            preResident.clear();
            touchOrder.clear();
            distinctRequests.clear();
            return false;
        }
        pending.insert(std::make_pair(key, std::move(decoded)));
        decodePeak = std::max(decodePeak, resident.size() + pending.size());
    }

    inTransaction = true;
    return true;
}

const DecodedBlock* CalStreamBlockCache::Impl::resolve(const BlockKey& key) const {
    if (inTransaction) {
        std::map<BlockKey, DecodedBlock>::const_iterator pendingIt = pending.find(key);
        if (pendingIt != pending.end()) {
            return &pendingIt->second;
        }
    }
    std::map<BlockKey, Entry>::const_iterator it = resident.find(key);
    if (it == resident.end()) {
        return 0;
    }
    return &it->second.block;
}

void CalStreamBlockCache::Impl::touch(const BlockKey& key) {
    if (!inTransaction) {
        return;
    }
    // Every access of a block that was resident before this frame began is a
    // hit; blocks loaded during the frame are not counted as hits, and a
    // block touched repeatedly still counts each time.
    if (preResident.find(key) != preResident.end()) {
        ++hitCount;
    }
    // repeated access of the same block: keep only the last position
    std::vector<BlockKey>::iterator it = std::find(touchOrder.begin(), touchOrder.end(), key);
    if (it != touchOrder.end()) {
        touchOrder.erase(it);
    }
    touchOrder.push_back(key);
}

void CalStreamBlockCache::Impl::commitTransaction() {
    if (!inTransaction) {
        return;
    }

    // publish the decoded blocks
    for (std::map<BlockKey, DecodedBlock>::iterator it = pending.begin(); it != pending.end(); ++it) {
        std::list<BlockKey>::iterator lruIt = lru.insert(lru.end(), it->first);
        resident.insert(std::make_pair(it->first, Entry{std::move(it->second), lruIt}));
    }
    pending.clear();

    // replay accesses in last-access order
    for (size_t i = 0; i < touchOrder.size(); ++i) {
        const BlockKey& key = touchOrder[i];
        std::map<BlockKey, Entry>::iterator entryIt = resident.find(key);
        if (entryIt != resident.end()) {
            lru.erase(entryIt->second.lruIt);
            entryIt->second.lruIt = lru.insert(lru.end(), key);
        }
    }

    const std::set<BlockKey> used(needed.begin(), needed.end());

    // evict blocks this frame did not use, least recently used first, until
    // the residence is back at capacity; used blocks are always retained
    std::list<BlockKey>::iterator lruIt = lru.begin();
    while (resident.size() > capacity && lruIt != lru.end()) {
        const BlockKey candidate = *lruIt;
        ++lruIt;
        if (used.find(candidate) != used.end()) {
            continue;
        }
        std::map<BlockKey, Entry>::iterator entryIt = resident.find(candidate);
        lru.erase(entryIt->second.lruIt);
        resident.erase(entryIt);
    }

    decodePeak = std::max(decodePeak, resident.size());

    inTransaction = false;
    needed.clear();
    preResident.clear();
    touchOrder.clear();
    distinctRequests.clear();
}

void CalStreamBlockCache::Impl::abortTransaction() {
    pending.clear();
    needed.clear();
    preResident.clear();
    touchOrder.clear();
    distinctRequests.clear();
    inTransaction = false;
}

CalStreamBlockCache::CalStreamBlockCache(size_t capacity) {
    m_impl = ISTD::make_shared<Impl>(capacity < 1 ? 1 : capacity);
}

CalStreamBlockCache::~CalStreamBlockCache() {}

StreamCacheStats CalStreamBlockCache::stats() const {
    StreamCacheStats s;
    s.capacity = m_impl->capacity;
    s.residentBlocks = m_impl->resident.size();
    s.hitCount = m_impl->hitCount;
    s.readAttempts = m_impl->readAttempts;
    s.decodePeak = m_impl->decodePeak;
    return s;
}

std::vector<std::pair<uint64_t, unsigned>> CalStreamBlockCache::residentBlockIds() const {
    std::vector<std::pair<uint64_t, unsigned>> result;
    for (std::list<BlockKey>::const_iterator it = m_impl->lru.begin(); it != m_impl->lru.end(); ++it) {
        result.push_back(std::make_pair(it->sourceId, it->blockIndex));
    }
    return result;
}

bool CalStreamBlockCache::beginTransaction(const std::vector<StreamBlockRequest>& requests) {
    return m_impl->beginTransaction(requests);
}

void CalStreamBlockCache::commitTransaction() {
    m_impl->commitTransaction();
}

void CalStreamBlockCache::abortTransaction() {
    m_impl->abortTransaction();
}

// ---------------------------------------------------------------------------
// open clip implementation
// ---------------------------------------------------------------------------

struct CalStreamClip::Impl {
    uint64_t id;
    mutable std::ifstream file;
    std::string path;
    float duration;
    float blockDuration;
    std::vector<BlockDirEntry> blocks;
    std::vector<TrackDirEntry> tracks;
    mutable uint64_t readAttempts;

    Impl()
        : id(0)
        , duration(0.0f)
        , blockDuration(0.0f)
        , readAttempts(0)
    {}
};

CalStreamClip::CalStreamClip()
    : m_impl(new Impl())
{}

CalStreamClip::~CalStreamClip() {}

uint64_t CalStreamClip::id() const {
    return m_impl->id;
}

unsigned CalStreamClip::blockCount() const {
    return static_cast<unsigned>(m_impl->blocks.size());
}

unsigned CalStreamClip::trackCount() const {
    return static_cast<unsigned>(m_impl->tracks.size());
}

float CalStreamClip::duration() const {
    return m_impl->duration;
}

float CalStreamClip::blockDuration() const {
    return m_impl->blockDuration;
}

StreamBlockInfo CalStreamClip::blockInfo(unsigned blockIndex) const {
    const BlockDirEntry& e = m_impl->blocks[blockIndex];
    StreamBlockInfo info;
    info.blockIndex = e.blockIndex;
    info.blockStart = e.blockStart;
    info.blockEnd = e.blockEnd;
    return info;
}

std::vector<unsigned> CalStreamClip::trackBoneIds() const {
    std::vector<unsigned> result;
    result.reserve(m_impl->tracks.size());
    for (size_t i = 0; i < m_impl->tracks.size(); ++i) {
        result.push_back(m_impl->tracks[i].coreBoneId);
    }
    return result;
}

unsigned CalStreamClip::blockIndexForTime(float time) const {
    const unsigned count = blockCount();
    if (!finiteFloat(time) || time <= 0.0f || count == 1) {
        return 0;
    }
    if (time >= m_impl->duration) {
        return count - 1;
    }
    unsigned index = static_cast<unsigned>(time / m_impl->blockDuration);
    if (index >= count) {
        index = count - 1;
    }
    // Float division/grid products can straddle a boundary (e.g. 0.9f is a
    // hair below 3 * 0.3f); resolve against the stored boundaries with a
    // relative epsilon so half-open interval membership stays consistent.
    const float epsilon = 1e-6f * std::max(1.0f, std::fabs(time));
    while (index + 1 < count && time + epsilon >= m_impl->blocks[index + 1].blockStart) {
        ++index;
    }
    return index;
}

uint64_t CalStreamClip::readAttempts() const {
    return m_impl->readAttempts;
}

bool CalStreamClip::decodeBlock(unsigned blockIndex, DecodedBlock& decoded) const {
    if (blockIndex >= m_impl->blocks.size()) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__, "block index out of range");
        return false;
    }

    const BlockDirEntry& dir = m_impl->blocks[blockIndex];

    ++m_impl->readAttempts;

    std::string payload;
    payload.resize(dir.length);
    m_impl->file.clear();
    m_impl->file.seekg(static_cast<std::streamoff>(dir.offset));
    if (!m_impl->file || !m_impl->file.read(&payload[0], std::streamsize(dir.length))) {
        CalError::setLastError(CalError::BAD_DATA_SOURCE, __FILE__, __LINE__, "block read failed");
        return false;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(payload.data());
    if (crc32(bytes, dir.length) != dir.checksum) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__, "block checksum mismatch");
        return false;
    }

    const uint8_t* p = bytes;
    const uint8_t* end = bytes + dir.length;

    uint32_t trackCountInBlock;
    if (!getU32(p, end, trackCountInBlock)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__, "truncated block header");
        return false;
    }
    if (trackCountInBlock > m_impl->tracks.size()) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__, "too many tracks in block");
        return false;
    }

    std::set<unsigned> seenTracks;

    for (uint32_t ti = 0; ti < trackCountInBlock; ++ti) {
        uint32_t coreBoneId;
        uint32_t keyCount;
        if (!getU32(p, end, coreBoneId) || !getU32(p, end, keyCount)) {
            CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__, "truncated track ref");
            return false;
        }
        if (coreBoneId > MAX_BONE_ID || !seenTracks.insert(coreBoneId).second) {
            CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__, "illegal or duplicate track in block");
            return false;
        }

        bool knownTrack = false;
        for (size_t k = 0; k < m_impl->tracks.size(); ++k) {
            if (m_impl->tracks[k].coreBoneId == coreBoneId) {
                knownTrack = true;
                break;
            }
        }
        if (!knownTrack) {
            CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__, "block references unknown track");
            return false;
        }

        std::vector<DecodedKey> keys;
        keys.resize(keyCount);
        float previousTime = -std::numeric_limits<float>::infinity();
        for (uint32_t ki = 0; ki < keyCount; ++ki) {
            float t, tx, ty, tz, qx, qy, qz, qw;
            if (!getF32(p, end, t) || !getF32(p, end, tx) || !getF32(p, end, ty) || !getF32(p, end, tz)
                || !getF32(p, end, qx) || !getF32(p, end, qy) || !getF32(p, end, qz) || !getF32(p, end, qw)) {
                CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__, "truncated keyframe");
                return false;
            }
            if (!finiteFloat(t) || !finiteFloat(tx) || !finiteFloat(ty) || !finiteFloat(tz)
                || !finiteFloat(qx) || !finiteFloat(qy) || !finiteFloat(qz) || !finiteFloat(qw)) {
                CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__, "non-finite keyframe value");
                return false;
            }
            if (!(t > previousTime)) {
                CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__, "keys not strictly increasing");
                return false;
            }
            previousTime = t;
            keys[ki].time = t;
            keys[ki].transform.translation = CalVector(tx, ty, tz);
            keys[ki].transform.rotation = CalQuaternion(qx, qy, qz, qw);
        }
        decoded.tracks.insert(std::make_pair(coreBoneId, std::move(keys)));
    }

    if (p != end) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__, "trailing bytes in block");
        return false;
    }

    return true;
}

namespace {

// Sampling identical to CalCoreTrack::getCurrentTransform but operating on a
// decoded block.  Stored floats are bit-identical to the original keys, so the
// resulting transforms match the in-memory path bit for bit.
cal3d::RotateTranslate sampleKeys(const std::vector<DecodedKey>& keyframes, float time) {
    if (keyframes.empty()) {
        return cal3d::RotateTranslate();
    }

    size_t after = 0;
    while (after < keyframes.size() && !(keyframes[after].time > time)) {
        ++after;
    }

    if (after == keyframes.size()) {
        return keyframes.back().transform;
    }
    if (after == 0) {
        return keyframes.front().transform;
    }

    const DecodedKey& before = keyframes[after - 1];
    const DecodedKey& afterKey = keyframes[after];

    float blendFactor = 0.0f;
    if (afterKey.time != before.time) {
        blendFactor = (time - before.time) / (afterKey.time - before.time);
    }
    return cal3d::blend(blendFactor, before.transform, afterKey.transform);
}

// Returns 0 on success, -1 if the file cannot be opened, -2 if it is too
// short or truncated.  Only the header and directory are ever read.
int readHeaderAndDirectory(const std::string& path, std::string& headerAndDirectory, uint64_t& fileSize, std::ifstream& file) {
    file.open(path.c_str(), std::ios::binary | std::ios::in);
    if (!file) {
        return -1;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff end = file.tellg();
    if (end < 0) {
        return -2;
    }
    fileSize = static_cast<uint64_t>(end);
    if (fileSize < HEADER_BYTES) {
        return -2;
    }

    // read only the fixed header first; the directory size comes from it
    headerAndDirectory.resize(HEADER_BYTES);
    file.seekg(0, std::ios::beg);
    if (!file.read(&headerAndDirectory[0], HEADER_BYTES)) {
        return -2;
    }

    const uint8_t* hp = reinterpret_cast<const uint8_t*>(headerAndDirectory.data());
    const uint8_t* hpEnd = hp + HEADER_BYTES;
    const uint8_t* q = hp;
    uint32_t magic, version, flags, blockCount, trackCount;
    float duration, blockDuration;
    uint64_t dataStart;
    uint32_t contentCrc;
    if (!getU32(q, hpEnd, magic) || !getU32(q, hpEnd, version) || !getU32(q, hpEnd, flags)
        || !getF32(q, hpEnd, duration) || !getF32(q, hpEnd, blockDuration)
        || !getU32(q, hpEnd, blockCount) || !getU32(q, hpEnd, trackCount)
        || !getU64(q, hpEnd, dataStart) || !getU32(q, hpEnd, contentCrc)) {
        return -2;
    }

    const uint64_t directoryBytes = uint64_t(BLOCK_ENTRY_BYTES) * blockCount
        + uint64_t(TRACK_ENTRY_BYTES) * trackCount;
    if (directoryBytes > fileSize - HEADER_BYTES) {
        return -2;
    }
    headerAndDirectory.resize(size_t(HEADER_BYTES + directoryBytes));
    if (!file.read(&headerAndDirectory[HEADER_BYTES], std::streamsize(directoryBytes))) {
        return -2;
    }
    // Block payloads are deliberately never read or decoded here.
    return 0;
}

} // namespace

bool CalStreamClip::sampleTrack(
    unsigned coreBoneId,
    float time,
    CalStreamBlockCache& cache,
    RotateTranslate& result) const
{
    const unsigned blockIndex = blockIndexForTime(time);
    const StreamBlockRequest request{this, blockIndex};
    const BlockKey key{m_impl->id, blockIndex};

    const bool ownTransaction = !cache.m_impl->inTransaction;
    if (ownTransaction) {
        std::vector<StreamBlockRequest> requests(1, request);
        if (!cache.beginTransaction(requests)) {
            return false;
        }
    }

    const DecodedBlock* block = cache.m_impl->resolve(key);
    if (!block) {
        if (ownTransaction) {
            cache.abortTransaction();
        }
        CalError::setLastError(CalError::BAD_DATA_SOURCE, __FILE__, __LINE__, "block not resident");
        return false;
    }

    cache.m_impl->touch(key);

    std::map<unsigned, std::vector<DecodedKey>>::const_iterator it = block->tracks.find(coreBoneId);
    if (it == block->tracks.end() || it->second.empty()) {
        // empty track or track with no keys in this block: identity local pose
        result = cal3d::RotateTranslate();
    } else {
        result = sampleKeys(it->second, time);
    }

    if (ownTransaction) {
        cache.commitTransaction();
    }
    return true;
}

// ---------------------------------------------------------------------------
// offline writer
// ---------------------------------------------------------------------------

bool writeStreamClip(
    const std::string& path,
    const CalCoreAnimation& animation,
    float blockDuration)
{
    auto fail = [&](CalError::Code code, const char* text) -> bool {
        CalError::setLastError(code, __FILE__, __LINE__, text);
        std::remove(path.c_str());
        return false;
    };

    if (!finiteFloat(blockDuration) || !(blockDuration > 0.0f)) {
        return fail(CalError::INVALID_ANIMATION_DURATION, "blockDuration must be positive and finite");
    }
    if (!finiteFloat(animation.duration) || animation.duration < 0.0f) {
        return fail(CalError::INVALID_ANIMATION_DURATION, "animation duration must be finite and non-negative");
    }
    if (animation.tracks.empty()) {
        return fail(CalError::INVALID_KEYFRAME_COUNT, "animation has no tracks");
    }

    std::set<unsigned> usedBoneIds;
    for (size_t ti = 0; ti < animation.tracks.size(); ++ti) {
        const CalCoreTrack& track = animation.tracks[ti];
        if (track.coreBoneId < 0 || uint32_t(track.coreBoneId) > MAX_BONE_ID) {
            return fail(CalError::INVALID_ATTRIBUTE_VALUE, "illegal bone id");
        }
        if (!usedBoneIds.insert(unsigned(track.coreBoneId)).second) {
            return fail(CalError::INVALID_ATTRIBUTE_VALUE, "duplicate track bone id");
        }
        float previous = -std::numeric_limits<float>::infinity();
        for (size_t ki = 0; ki < track.keyframes.size(); ++ki) {
            const CalCoreKeyframe& kf = track.keyframes[ki];
            const CalVector& tr = kf.transform.translation;
            const CalQuaternion& ro = kf.transform.rotation;
            if (!finiteFloat(kf.time) || !finiteFloat(tr.x) || !finiteFloat(tr.y) || !finiteFloat(tr.z)
                || !finiteFloat(ro.x) || !finiteFloat(ro.y) || !finiteFloat(ro.z) || !finiteFloat(ro.w)) {
                return fail(CalError::INVALID_ATTRIBUTE_VALUE, "non-finite keyframe value");
            }
            if (!(kf.time > previous)) {
                return fail(CalError::INVALID_KEYFRAME_COUNT, "keyframe times must be strictly increasing");
            }
            previous = kf.time;
        }
    }

    const float duration = animation.duration;
    const uint32_t blockCount = std::max(
        1u,
        static_cast<uint32_t>(std::ceil(duration / blockDuration)));

    // block time ranges: half-open intervals of exactly blockDuration, except
    // the final block which is closed at duration.
    std::vector<std::pair<float, float>> ranges;
    ranges.reserve(blockCount);
    for (uint32_t b = 0; b < blockCount; ++b) {
        const float start = b * blockDuration;
        const float end = (b + 1 == blockCount) ? duration : (b + 1) * blockDuration;
        ranges.push_back(std::make_pair(start, end));
    }

    std::vector<TrackDirEntry> trackEntries;
    trackEntries.reserve(animation.tracks.size());
    for (size_t ti = 0; ti < animation.tracks.size(); ++ti) {
        TrackDirEntry e;
        e.coreBoneId = uint32_t(animation.tracks[ti].coreBoneId);
        e.keyCount = 0;
        trackEntries.push_back(e);
    }

    std::vector<std::string> payloads(blockCount);
    for (uint32_t b = 0; b < blockCount; ++b) {
        const float start = ranges[b].first;
        const float end = ranges[b].second;
        const bool lastBlock = (b + 1 == blockCount);

        struct TrackPayload {
            uint32_t boneId;
            std::vector<const CalCoreKeyframe*> keys;
        };
        std::vector<TrackPayload> blockTracks;

        for (size_t ti = 0; ti < animation.tracks.size(); ++ti) {
            const CalCoreTrack& track = animation.tracks[ti];
            const CalCoreTrack::KeyframeList& kfs = track.keyframes;

            // predecessor: last key strictly before the interval start
            const CalCoreKeyframe* predecessor = 0;
            // successor: first key at/after the interval end (strictly after
            // for the last block, whose interior already includes end)
            const CalCoreKeyframe* successor = 0;

            for (size_t ki = 0; ki < kfs.size(); ++ki) {
                if (kfs[ki].time < start) {
                    predecessor = &kfs[ki];
                }
                if (!successor) {
                    if (lastBlock ? (kfs[ki].time > end) : (kfs[ki].time >= end)) {
                        successor = &kfs[ki];
                    }
                }
            }

            TrackPayload tp;
            tp.boneId = uint32_t(track.coreBoneId);
            if (predecessor) {
                tp.keys.push_back(predecessor);
            }
            for (size_t ki = 0; ki < kfs.size(); ++ki) {
                const bool inside = lastBlock
                    ? (kfs[ki].time >= start && kfs[ki].time <= end)
                    : (kfs[ki].time >= start && kfs[ki].time < end);
                if (inside) {
                    tp.keys.push_back(&kfs[ki]);
                }
            }
            if (successor) {
                tp.keys.push_back(successor);
            }

            if (!tp.keys.empty()) {
                blockTracks.push_back(tp);
            }
        }

        std::string& payload = payloads[b];
        putU32(payload, uint32_t(blockTracks.size()));
        for (size_t ti = 0; ti < blockTracks.size(); ++ti) {
            putU32(payload, blockTracks[ti].boneId);
            putU32(payload, uint32_t(blockTracks[ti].keys.size()));
            for (size_t ki = 0; ki < blockTracks[ti].keys.size(); ++ki) {
                const CalCoreKeyframe* kf = blockTracks[ti].keys[ki];
                putF32(payload, kf->time);
                putF32(payload, kf->transform.translation.x);
                putF32(payload, kf->transform.translation.y);
                putF32(payload, kf->transform.translation.z);
                putF32(payload, kf->transform.rotation.x);
                putF32(payload, kf->transform.rotation.y);
                putF32(payload, kf->transform.rotation.z);
                putF32(payload, kf->transform.rotation.w);
            }
            for (size_t te = 0; te < trackEntries.size(); ++te) {
                if (trackEntries[te].coreBoneId == blockTracks[ti].boneId) {
                    trackEntries[te].keyCount += uint32_t(blockTracks[ti].keys.size());
                    break;
                }
            }
        }

        if (payload.size() > STREAMCLIP_MAX_BLOCK_BYTES) {
            return fail(CalError::INVALID_KEYFRAME_COUNT, "block payload exceeds 32 MiB");
        }
    }

    const size_t directoryBytes = BLOCK_ENTRY_BYTES * blockCount + TRACK_ENTRY_BYTES * trackEntries.size();
    const uint64_t dataStart = HEADER_BYTES + directoryBytes;

    std::vector<BlockDirEntry> blockEntries(blockCount);
    uint64_t offset = dataStart;
    for (uint32_t b = 0; b < blockCount; ++b) {
        blockEntries[b].blockIndex = b;
        blockEntries[b].blockStart = ranges[b].first;
        blockEntries[b].blockEnd = ranges[b].second;
        blockEntries[b].offset = offset;
        blockEntries[b].length = uint32_t(payloads[b].size());
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(payloads[b].data());
        blockEntries[b].checksum = crc32(bytes, payloads[b].size());
        offset += payloads[b].size();
    }

    std::string file;
    file.reserve(size_t(offset));

    // header (content crc written last)
    putU32(file, FILE_MAGIC);
    putU32(file, STREAMCLIP_FORMAT_VERSION);
    putU32(file, 0); // flags
    putF32(file, duration);
    putF32(file, blockDuration);
    putU32(file, blockCount);
    putU32(file, uint32_t(trackEntries.size()));
    putU64(file, dataStart);
    putU32(file, 0); // content crc placeholder
    assert(file.size() == HEADER_BYTES);

    for (uint32_t b = 0; b < blockCount; ++b) {
        putU32(file, blockEntries[b].blockIndex);
        putF32(file, blockEntries[b].blockStart);
        putF32(file, blockEntries[b].blockEnd);
        putU64(file, blockEntries[b].offset);
        putU32(file, blockEntries[b].length);
        putU32(file, blockEntries[b].checksum);
        putU32(file, 0); // reserved padding to the 32-byte entry width
    }
    for (size_t ti = 0; ti < trackEntries.size(); ++ti) {
        putU32(file, trackEntries[ti].coreBoneId);
        putU32(file, trackEntries[ti].keyCount);
    }
    assert(file.size() == dataStart);

    for (uint32_t b = 0; b < blockCount; ++b) {
        file.append(payloads[b]);
    }

    const uint32_t contentCrc = crc32(
        reinterpret_cast<const uint8_t*>(file.data()) + dataStart,
        file.size() - size_t(dataStart));
    file[HEADER_BYTES - 4] = char(contentCrc & 0xFFu);
    file[HEADER_BYTES - 3] = char((contentCrc >> 8) & 0xFFu);
    file[HEADER_BYTES - 2] = char((contentCrc >> 16) & 0xFFu);
    file[HEADER_BYTES - 1] = char((contentCrc >> 24) & 0xFFu);

    {
        std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail(CalError::FILE_CREATION_FAILED, "cannot create stream clip file");
        }
        out.write(file.data(), std::streamsize(file.size()));
        if (!out) {
            return fail(CalError::FILE_WRITING_FAILED, "cannot write stream clip file");
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// read-only opener
// ---------------------------------------------------------------------------

CalStreamClipPtr openStreamClip(const std::string& path) {
    auto fail = [&](CalError::Code code, const char* text) -> CalStreamClipPtr {
        CalError::setLastError(code, __FILE__, __LINE__, text);
        return CalStreamClipPtr();
    };

    ISTD::unique_ptr<CalStreamClip::Impl> impl(new CalStreamClip::Impl());

    // Only the header and directory are read; no block payload is touched.
    std::string headerAndDirectory;
    uint64_t fileSize = 0;
    const int readStatus = readHeaderAndDirectory(path, headerAndDirectory, fileSize, impl->file);
    if (readStatus == -1) {
        return fail(CalError::FILE_NOT_FOUND, "cannot open stream clip file");
    }
    if (readStatus == -2) {
        return fail(CalError::INVALID_FILE_FORMAT, "truncated stream clip header or directory");
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(headerAndDirectory.data());
    const uint8_t* p = base;
    const uint8_t* end = base + headerAndDirectory.size();

    uint32_t magic, version, flags, blockCount, trackCount;
    float duration, blockDuration;
    uint64_t dataStart;
    uint32_t contentCrc;

    if (!getU32(p, end, magic) || !getU32(p, end, version) || !getU32(p, end, flags)
        || !getF32(p, end, duration) || !getF32(p, end, blockDuration)
        || !getU32(p, end, blockCount) || !getU32(p, end, trackCount)
        || !getU64(p, end, dataStart) || !getU32(p, end, contentCrc)) {
        return fail(CalError::INVALID_FILE_FORMAT, "truncated header");
    }
    // contentCrc covers the payload region for offline validation tools; the
    // runtime path never reads all payloads at open and instead verifies each
    // block lazily via its directory checksum.
    (void)contentCrc;

    if (magic != FILE_MAGIC) {
        return fail(CalError::INVALID_FILE_FORMAT, "bad magic");
    }
    if (version != STREAMCLIP_FORMAT_VERSION) {
        return fail(CalError::INCOMPATIBLE_FILE_VERSION, "unsupported stream clip version");
    }
    if (flags != 0) {
        return fail(CalError::INVALID_FILE_FORMAT, "unsupported flags");
    }
    if (!finiteFloat(duration) || duration < 0.0f) {
        return fail(CalError::INVALID_ANIMATION_DURATION, "bad duration");
    }
    if (!finiteFloat(blockDuration) || !(blockDuration > 0.0f)) {
        return fail(CalError::INVALID_ANIMATION_DURATION, "bad block duration");
    }
    if (blockCount < 1 || trackCount < 1) {
        return fail(CalError::INVALID_KEYFRAME_COUNT, "directory must be non-empty");
    }

    const size_t directoryBytes = size_t(BLOCK_ENTRY_BYTES) * blockCount + size_t(TRACK_ENTRY_BYTES) * trackCount;
    if (dataStart != HEADER_BYTES + directoryBytes || dataStart > fileSize) {
        return fail(CalError::INVALID_FILE_FORMAT, "dataStart out of range");
    }

    std::vector<BlockDirEntry> blocks(blockCount);
    for (uint32_t b = 0; b < blockCount; ++b) {
        BlockDirEntry& e = blocks[b];
        uint32_t reserved;
        if (!getU32(p, end, e.blockIndex) || !getF32(p, end, e.blockStart) || !getF32(p, end, e.blockEnd)
            || !getU64(p, end, e.offset) || !getU32(p, end, e.length) || !getU32(p, end, e.checksum)
            || !getU32(p, end, reserved)) {
            return fail(CalError::INVALID_FILE_FORMAT, "truncated block directory");
        }
        if (reserved != 0) {
            return fail(CalError::INVALID_FILE_FORMAT, "non-zero reserved block directory field");
        }
        if (e.blockIndex != b) {
            return fail(CalError::INVALID_FILE_FORMAT, "block directory not in order");
        }
        if (!finiteFloat(e.blockStart) || !finiteFloat(e.blockEnd) || e.blockEnd < e.blockStart) {
            return fail(CalError::INVALID_FILE_FORMAT, "bad block interval");
        }
        if (e.length > STREAMCLIP_MAX_BLOCK_BYTES) {
            return fail(CalError::INVALID_FILE_FORMAT, "block exceeds 32 MiB");
        }
        if (e.offset < dataStart || e.offset > fileSize || uint64_t(e.length) > fileSize - e.offset) {
            return fail(CalError::INVALID_FILE_FORMAT, "block payload out of file bounds");
        }
    }

    // interval topology: aligned to the block duration grid, contiguous and
    // disjoint; the first block starts at zero and the last one ends at duration
    if (blocks.front().blockStart != 0.0f) {
        return fail(CalError::INVALID_FILE_FORMAT, "first block must start at 0");
    }
    if (blocks.back().blockEnd != duration) {
        return fail(CalError::INVALID_FILE_FORMAT, "last block must end at duration");
    }
    for (uint32_t b = 0; b < blockCount; ++b) {
        const float gridStart = b * blockDuration;
        if (blocks[b].blockStart != gridStart) {
            return fail(CalError::INVALID_FILE_FORMAT, "block start off the duration grid");
        }
        if (b + 1 < blockCount) {
            if (blocks[b].blockEnd != (b + 1) * blockDuration) {
                return fail(CalError::INVALID_FILE_FORMAT, "block end off the duration grid");
            }
            if (blocks[b].blockEnd <= blocks[b].blockStart) {
                return fail(CalError::INVALID_FILE_FORMAT, "block interval must be a positive half-open range");
            }
        }
        if (b > 0 && blocks[b].blockStart != blocks[b - 1].blockEnd) {
            return fail(CalError::INVALID_FILE_FORMAT, "block intervals must be contiguous");
        }
    }

    std::vector<TrackDirEntry> tracks(trackCount);
    std::set<uint32_t> usedBoneIds;
    for (uint32_t t = 0; t < trackCount; ++t) {
        if (!getU32(p, end, tracks[t].coreBoneId) || !getU32(p, end, tracks[t].keyCount)) {
            return fail(CalError::INVALID_FILE_FORMAT, "truncated track directory");
        }
        if (tracks[t].coreBoneId > MAX_BONE_ID) {
            return fail(CalError::INVALID_ATTRIBUTE_VALUE, "illegal bone id");
        }
        if (!usedBoneIds.insert(tracks[t].coreBoneId).second) {
            return fail(CalError::INVALID_ATTRIBUTE_VALUE, "duplicate track bone id");
        }
    }

    if (p != end) {
        return fail(CalError::INVALID_FILE_FORMAT, "directory size mismatch");
    }

    // payloads must tile the data region in directory order
    for (uint32_t b = 0; b < blockCount; ++b) {
        const uint64_t expectedOffset = (b == 0)
            ? dataStart
            : blocks[b - 1].offset + blocks[b - 1].length;
        if (blocks[b].offset != expectedOffset) {
            return fail(CalError::INVALID_FILE_FORMAT, "blocks must tile the data region in order");
        }
    }
    if (blocks.back().offset + blocks.back().length != fileSize) {
        return fail(CalError::INVALID_FILE_FORMAT, "trailing data after last block");
    }

    // impl->file was opened by readHeaderAndDirectory() and stays open for
    // lazy, seek-based block reads; no payload was decoded.
    impl->id = nextSourceId();
    impl->path = path;
    impl->duration = duration;
    impl->blockDuration = blockDuration;
    impl->blocks = std::move(blocks);
    impl->tracks = std::move(tracks);

    CalStreamClipPtr clip(new CalStreamClip());
    clip->m_impl = std::move(impl);
    return clip;
}

// ---------------------------------------------------------------------------
// CalAnimation attachment
// ---------------------------------------------------------------------------

void setAnimationStreamSource(CalAnimation& animation, const CalStreamClipPtr& clip) {
    animation.streamClip = clip;
    animation.coreAnimation.reset();
}

bool animationHasStreamSource(const CalAnimation& animation) {
    return static_cast<bool>(animation.streamClip);
}

} // namespace cal3d

// ---------------------------------------------------------------------------
// streaming mixer entry point
// ---------------------------------------------------------------------------

bool CalMixer::updateSkeletonStreamed(
    CalSkeleton* skeleton,
    const std::vector<BoneTransformAdjustment>& boneTransformAdjustments,
    const std::vector<BoneScaleAdjustment>& boneScaleAdjustments,
    cal3d::CalStreamBlockCache& cache)
{
    // Phase 1: collect every distinct block this frame will need, in blend
    // (first-access) order, before touching the skeleton.
    std::vector<cal3d::StreamBlockRequest> requests;
    for (AnimationList::iterator it = activeAnimations.begin(); it != activeAnimations.end(); ++it) {
        CalAnimation& animation = **it;
        if (!animation.streamClip) {
            continue;
        }
        cal3d::StreamBlockRequest request;
        request.clip = animation.streamClip.get();
        request.blockIndex = animation.streamClip->blockIndexForTime(animation.time);
        requests.push_back(request);
    }

    if (!cache.beginTransaction(requests)) {
        CalError::setLastError(
            CalError::BAD_DATA_SOURCE,
            __FILE__,
            __LINE__,
            "stream block working set could not be loaded");
        return false;
    }

    // Phase 2: run the exact same blending implementation as the legacy path.
    struct Provider : cal3d::IStreamPoseProvider {
        cal3d::CalStreamBlockCache& cache;
        explicit Provider(cal3d::CalStreamBlockCache& c)
            : cache(c)
        {}

        bool getTrackTransform(
            CalAnimation& animation,
            unsigned coreBoneId,
            float time,
            cal3d::RotateTranslate& result) override
        {
            return animation.streamClip->sampleTrack(coreBoneId, time, cache, result);
        }
    } provider(cache);

    const bool ok = blendSkeleton(skeleton, boneTransformAdjustments, boneScaleAdjustments, &provider);
    if (!ok) {
        cache.abortTransaction();
        return false;
    }

    // Phase 3: publish decoded blocks and evict only after a successful frame.
    cache.commitTransaction();
    return true;
}
