// Copyright © 2026 Steve Lhomme.
// SPDX-License-Identifier: LGPL-2.1-or-later

/*!
  \file KaxFrameStore task tests: transactional spooling, streaming render and
  shared ownership lifetimes. No main() is provided, task_support/driver.c drives
  the registered cases.
*/
#include "task_support/check.h"

#include "matroska/KaxBlock.h"
#include "matroska/KaxBlockData.h"
#include "matroska/KaxCluster.h"
#include "matroska/KaxCues.h"
#include "matroska/KaxFrameStore.h"
#include "matroska/KaxSegment.h"
#include "matroska/KaxSemantic.h"
#include "matroska/KaxTracks.h"

#include <ebml/EbmlHead.h>
#include <ebml/MemIOCallback.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace libebml;
using namespace libmatroska;

namespace {

// ---------------------------------------------------------------------------
// deterministic frame payloads and free callback counting
// ---------------------------------------------------------------------------

int g_releaseCount = 0;
bool g_releaseThrows = false;

bool frameFreeCallback(const DataBuffer &)
{
  ++g_releaseCount;
  if (g_releaseThrows)
    throw std::runtime_error("synthetic free callback failure");
  return true;
}

std::vector<binary> frameBytes(std::uint32_t size, std::uint8_t seed)
{
  std::vector<binary> result(size);
  for (std::uint32_t i = 0; i < size; ++i)
    result[i] = static_cast<binary>((static_cast<unsigned>(seed) * 37u + i * 13u + 7u) & 0xFFu);
  return result;
}

DataBuffer * makeBuffer(std::uint32_t size, std::uint8_t seed)
{
  auto bytes = frameBytes(size, seed);
  // internal copy: the source vector is not retained, FreeBuffer runs the callback
  return new DataBuffer(bytes.data(), size, frameFreeCallback, true);
}

// ---------------------------------------------------------------------------
// fault injectable IOCallback with bounded request accounting
// ---------------------------------------------------------------------------

class FaultyIO : public IOCallback {
  public:
    std::vector<binary> storage;
    std::uint64_t pos{0};

    // -1 means the fault never triggers
    int failWritesAfter{-1};
    int failReadsAfter{-1};
    bool throwOnWrite{false};
    bool throwOnRead{false};
    bool throwOnSeek{false};

    std::uint64_t writeCount{0};
    std::uint64_t readCount{0};
    std::size_t maxWriteRequest{0};
    std::size_t maxReadRequest{0};

    std::size_t read(void * buffer, std::size_t size) override {
      ++readCount;
      maxReadRequest = std::max(maxReadRequest, size);
      if (throwOnRead)
        throw std::runtime_error("synthetic read failure");
      if (failReadsAfter == 0)
        return 0; // controlled short read
      if (failReadsAfter > 0)
        --failReadsAfter;
      if (pos >= storage.size())
        return 0;
      const auto chunk = std::min(size, static_cast<std::size_t>(storage.size() - pos));
      std::memcpy(buffer, storage.data() + pos, chunk);
      pos += chunk;
      return chunk;
    }

    void setFilePointer(std::int64_t offset, seek_mode mode = seek_beginning) override {
      if (throwOnSeek)
        throw std::runtime_error("synthetic seek failure");
      if (mode == seek_beginning)
        pos = offset;
      else if (mode == seek_current)
        pos += offset;
      else
        pos = storage.size() + offset;
    }

    std::size_t write(const void * buffer, std::size_t size) override {
      ++writeCount;
      maxWriteRequest = std::max(maxWriteRequest, size);
      if (throwOnWrite)
        throw std::runtime_error("synthetic write failure");
      if (failWritesAfter == 0) {
        // controlled short write, never a full one
        return size > 1 ? size / 2 : 0;
      }
      if (failWritesAfter > 0)
        --failWritesAfter;
      if (pos + size > storage.size())
        storage.resize(static_cast<std::size_t>(pos + size));
      std::memcpy(storage.data() + pos, buffer, size);
      pos += size;
      return size;
    }

    std::uint64_t getFilePointer() override {return pos;}
    void close() override {}
};

// ---------------------------------------------------------------------------
// cluster scaffolding
// ---------------------------------------------------------------------------

void setupTrack(KaxTrackEntry & track)
{
  track.SetGlobalTimestampScale(1000000);
  GetChild<KaxTrackNumber>(track).SetValue(1);
  track.EnableLacing(true);
}

void setupCluster(KaxCluster & cluster)
{
  cluster.SetPreviousTimestamp(0, 1000000);
  GetChild<KaxClusterTimestamp>(cluster).SetValue(1);
}

std::vector<binary> renderCluster(KaxCluster & cluster)
{
  MemIOCallback output;
  KaxSegment segment;
  KaxCues cues;
  cues.SetGlobalTimestampScale(1000000);
  segment.WriteHead(output, 5);
  cluster.SetParent(segment);
  cluster.Render(output, cues);
  return {output.GetDataBuffer(),
          output.GetDataBuffer() + output.getFilePointer()};
}

// ---------------------------------------------------------------------------
// independent EBML / Matroska block parser
// ---------------------------------------------------------------------------

struct VInt {
  std::uint64_t value;
  unsigned length;
};

VInt readVInt(const std::vector<binary> & data, std::size_t & pos, bool withMarker)
{
  if (pos >= data.size() || data[pos] == 0)
    throw std::runtime_error("invalid vint");
  unsigned n = 1;
  unsigned mask = 0x80;
  while ((data[pos] & mask) == 0) {
    mask >>= 1;
    ++n;
  }
  if (pos + n > data.size())
    throw std::runtime_error("truncated vint");
  std::uint64_t value = withMarker ? data[pos] : (data[pos] & (mask - 1));
  ++pos;
  for (unsigned i = 1; i < n; ++i)
    value = (value << 8) | data[pos++];
  return {value, n};
}

struct ElementHead {
  std::uint64_t id;
  std::size_t payloadStart;
  std::size_t payloadEnd;
};

ElementHead readElement(const std::vector<binary> & data, std::size_t & pos)
{
  const auto id = readVInt(data, pos, true);
  const auto size = readVInt(data, pos, false);
  const std::size_t start = pos;
  if (start + size.value > data.size())
    throw std::runtime_error("element past end");
  pos = start + static_cast<std::size_t>(size.value);
  return {id.value, start, pos};
}

std::uint64_t readUnsignedVintValue(const std::vector<binary> & data, std::size_t & pos)
{
  const auto v = readVInt(data, pos, false);
  return v.value;
}

std::int64_t readSignedVintValue(const std::vector<binary> & data, std::size_t & pos)
{
  const std::size_t start = pos;
  const auto raw = readVInt(data, pos, false);
  const auto shift = (std::int64_t(1) << (7 * (pos - start) - 1)) - 1;
  return static_cast<std::int64_t>(raw.value) - shift;
}

struct ParsedBlock {
  std::uint64_t track;
  std::int16_t localTimestamp;
  std::uint8_t flags;
  std::vector<std::vector<binary>> frames;
};

ParsedBlock parseBlock(const std::vector<binary> & data, std::size_t start, std::size_t end)
{
  ParsedBlock result;
  std::size_t pos = start;
  // track number vint: strip the length marker bit(s)
  const auto trackVint = readVInt(data, pos, true);
  result.track = trackVint.length == 1 ? (trackVint.value & 0x7Fu)
                                      : (trackVint.value & 0x3FFFu);
  if (pos + 3 > end)
    throw std::runtime_error("block head truncated");
  result.localTimestamp = static_cast<std::int16_t>((data[pos] << 8) | data[pos + 1]);
  pos += 2;
  result.flags = data[pos++];

  const auto lacing = (result.flags & 0x06) >> 1;
  if (lacing == 0) {
    result.frames.emplace_back(data.begin() + pos, data.begin() + end);
    return result;
  }

  const unsigned laceCount = data[pos++]; // frames minus one
  const unsigned frameCount = laceCount + 1;
  std::vector<std::uint32_t> sizes;
  sizes.reserve(frameCount);

  if (lacing == 1) { // Xiph
    for (unsigned i = 0; i < laceCount; ++i) {
      std::uint32_t size = 0;
      while (pos < end && data[pos] == 0xFF) {
        size += 0xFF;
        ++pos;
      }
      if (pos >= end)
        throw std::runtime_error("xiph lace truncated");
      size += data[pos++];
      sizes.push_back(size);
    }
  } else if (lacing == 3) { // EBML
    sizes.push_back(static_cast<std::uint32_t>(readUnsignedVintValue(data, pos)));
    for (unsigned i = 1; i < laceCount; ++i)
      sizes.push_back(sizes.back() + static_cast<std::uint32_t>(readSignedVintValue(data, pos)));
  }

  for (unsigned i = 0; i < frameCount; ++i) {
    std::uint32_t size;
    if (lacing == 2) // fixed
      size = static_cast<std::uint32_t>(end - pos) / (frameCount - i);
    else if (i < laceCount)
      size = sizes[i];
    else
      size = static_cast<std::uint32_t>(end - pos); // last frame takes the rest
    if (pos + size > end)
      throw std::runtime_error("frame past block end");
    result.frames.emplace_back(data.begin() + pos, data.begin() + pos + size);
    pos += size;
  }
  if (pos != end)
    throw std::runtime_error("block payload not fully consumed");
  return result;
}

std::vector<ParsedBlock> parseClusters(const std::vector<binary> & data)
{
  std::vector<ParsedBlock> blocks;
  std::size_t pos = 0;
  while (pos < data.size()) {
    const auto element = readElement(data, pos);
    if (element.id != 0x1F43B675) // KaxCluster
      continue;
    std::size_t cpos = element.payloadStart;
    while (cpos < element.payloadEnd) {
      const auto child = readElement(data, cpos);
      if (child.id == 0xA3) { // KaxSimpleBlock
        blocks.push_back(parseBlock(data, child.payloadStart, child.payloadEnd));
      } else if (child.id == 0xA0) { // KaxBlockGroup
        std::size_t gpos = child.payloadStart;
        while (gpos < child.payloadEnd) {
          const auto grand = readElement(data, gpos);
          if (grand.id == 0xA1) // KaxBlock
            blocks.push_back(parseBlock(data, grand.payloadStart, grand.payloadEnd));
        }
      }
    }
  }
  return blocks;
}

int expectFrames(const std::vector<ParsedBlock> & blocks,
                 const std::vector<std::vector<binary>> & expected,
                 const std::vector<std::int16_t> & expectedTimestamps)
{
  std::size_t frameIndex = 0;
  for (const auto& block : blocks) {
    for (const auto& frame : block.frames) {
      TASK_CHECK(frameIndex < expected.size());
      TASK_CHECK(frame == expected[frameIndex]);
      ++frameIndex;
    }
  }
  TASK_CHECK(frameIndex == expected.size());
  std::size_t blockIndex = 0;
  for (const auto& block : blocks) {
    TASK_CHECK(blockIndex < expectedTimestamps.size());
    TASK_CHECK(block.localTimestamp == expectedTimestamps[blockIndex]);
    ++blockIndex;
  }
  TASK_CHECK(blockIndex == expectedTimestamps.size());
  return 0;
}

std::shared_ptr<KaxFrameStore> makeStore(const std::shared_ptr<IOCallback> & io)
{
  return std::make_shared<KaxFrameStore>(io);
}

} // namespace

// ===========================================================================
// transaction group
// ===========================================================================

static int null_and_foreign_store_rejected()
{
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);
  KaxBlockGroup * block = nullptr;
  cluster.AddFrame(track, 1000000, *makeBuffer(10, 1), block, LACING_XIPH);

  TASK_CHECK(!cluster.SpoolFrames(nullptr));
  TASK_CHECK(g_releaseCount == 0);

  auto io1 = std::make_shared<FaultyIO>();
  auto store1 = makeStore(io1);
  TASK_CHECK(cluster.SpoolFrames(store1));
  TASK_CHECK(g_releaseCount == 1);

  auto io2 = std::make_shared<FaultyIO>();
  auto store2 = makeStore(io2);
  // the cluster already belongs to store1: store2 must be rejected wholesale
  TASK_CHECK(!cluster.SpoolFrames(store2));
  TASK_CHECK(io2->storage.empty());

  // the original store still serves the cluster
  const auto out = renderCluster(cluster);
  TASK_CHECK(!out.empty());
  const auto parsed = parseClusters(out);
  TASK_CHECK(parsed.size() == 1);
  TASK_CHECK(parsed[0].frames.size() == 1);
  return 0;
}

static int short_write_keeps_everything_resident()
{
  g_releaseCount = 0;
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);
  KaxBlockGroup * block = nullptr;
  cluster.AddFrame(track, 1000000, *makeBuffer(5, 1), block, LACING_XIPH);
  cluster.AddFrame(track, 2000000, *makeBuffer(300, 2), block, LACING_XIPH);
  const auto before = renderCluster(cluster);

  auto io = std::make_shared<FaultyIO>();
  io->failWritesAfter = 1; // the second write is short
  auto store = makeStore(io);
  TASK_CHECK(!cluster.SpoolFrames(store));
  TASK_CHECK(g_releaseCount == 0); // nothing resident was released
  TASK_CHECK(renderCluster(cluster) == before); // still fully exportable

  // repair the stream behaviour and retry with a fresh store: tail bytes left
  // in the failed stream must never be interpreted as frame data
  auto good = std::make_shared<FaultyIO>();
  auto goodStore = makeStore(good);
  TASK_CHECK(cluster.SpoolFrames(goodStore));
  TASK_CHECK(g_releaseCount == 2);
  TASK_CHECK(renderCluster(cluster) == before);
  return 0;
}

static int seek_failure_keeps_everything_resident()
{
  g_releaseCount = 0;
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);
  KaxBlockGroup * block = nullptr;
  cluster.AddFrame(track, 1000000, *makeBuffer(12, 3), block, LACING_NONE);
  const auto before = renderCluster(cluster);

  auto io = std::make_shared<FaultyIO>();
  io->throwOnSeek = true;
  TASK_CHECK(!cluster.SpoolFrames(makeStore(io)));
  TASK_CHECK(g_releaseCount == 0);
  TASK_CHECK(renderCluster(cluster) == before);

  auto good = std::make_shared<FaultyIO>();
  TASK_CHECK(cluster.SpoolFrames(makeStore(good)));
  TASK_CHECK(g_releaseCount == 1);
  TASK_CHECK(renderCluster(cluster) == before);
  return 0;
}

static int throwing_release_callback_is_safe()
{
  g_releaseCount = 0;
  g_releaseThrows = true;
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);
  KaxBlockGroup * block = nullptr;
  cluster.AddFrame(track, 1000000, *makeBuffer(7, 4), block, LACING_XIPH);
  cluster.AddFrame(track, 2000000, *makeBuffer(9, 5), block, LACING_XIPH);
  const auto before = renderCluster(cluster);

  auto io = std::make_shared<FaultyIO>();
  TASK_CHECK(!cluster.SpoolFrames(makeStore(io)));
  TASK_CHECK(g_releaseCount == 1); // the first callback threw, no frame was freed
  // the cluster remains exportable and destruction must not touch the payloads
  TASK_CHECK(renderCluster(cluster) == before);
  g_releaseThrows = false;
  return 0;
}

// ===========================================================================
// streaming group
// ===========================================================================

static int direct_blockgroup_xiph_roundtrip()
{
  g_releaseCount = 0;
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);
  KaxBlockGroup * block = nullptr;
  const std::uint32_t sizes[] = {5, 300, 17, 260};
  std::vector<std::vector<binary>> expected;
  std::vector<std::int16_t> timestamps;
  for (std::size_t i = 0; i < 4; ++i) {
    auto * buffer = makeBuffer(sizes[i], static_cast<std::uint8_t>(i + 1));
    expected.push_back(frameBytes(sizes[i], static_cast<std::uint8_t>(i + 1)));
    cluster.AddFrame(track, (1 + i) * 1000000, *buffer, block, LACING_XIPH);
    if (i == 0 || block != nullptr)
      timestamps.push_back(static_cast<std::int16_t>(i * 1000));
  }
  // the lace holds at most 8 frames: all four land in the same BlockGroup
  TASK_CHECK(cluster.ListSize() == 2); // timestamp + one block group
  const auto before = renderCluster(cluster);

  auto io = std::make_shared<FaultyIO>();
  auto store = makeStore(io);
  TASK_CHECK(cluster.SpoolFrames(store));
  TASK_CHECK(g_releaseCount == 4);
  const auto after = renderCluster(cluster);
  TASK_CHECK(after == before); // byte identical cluster output

  const auto parsed = parseClusters(after);
  TASK_CHECK(parsed.size() == 1);
  TASK_CHECK(parsed[0].frames.size() == 4);
  TASK_CHECK(expectFrames(parsed, expected, {0}) == 0);

  // a second export is identical and the store gained no new bytes
  const auto storeSize = io->storage.size();
  TASK_CHECK(renderCluster(cluster) == before);
  TASK_CHECK(io->storage.size() == storeSize);

  // re-spooling with the same store does not rewrite old frames
  TASK_CHECK(cluster.SpoolFrames(store));
  TASK_CHECK(io->storage.size() == storeSize);
  return 0;
}

static int simpleblock_blob_roundtrip_with_cue()
{
  g_releaseCount = 0;
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);
  // blob frames bypass AddFrameInternal, so establish the cluster time base explicitly
  cluster.InitTimestamp(1, 1000000);

  auto * blob = new KaxBlockBlob(BLOCK_BLOB_ALWAYS_SIMPLE);
  blob->SetParent(cluster);
  TASK_CHECK(cluster.AddBlockBlob(blob));
  const std::uint32_t sizes[] = {11, 23, 45};
  std::vector<std::vector<binary>> expected;
  for (std::uint32_t i = 0; i < 3; ++i) {
    auto * buffer = makeBuffer(sizes[i], static_cast<std::uint8_t>(10 + i));
    expected.push_back(frameBytes(sizes[i], static_cast<std::uint8_t>(10 + i)));
    TASK_CHECK(blob->AddFrameAuto(track, (1 + i) * 1000000, *buffer, LACING_EBML));
  }

  KaxCues cues;
  cues.SetGlobalTimestampScale(1000000);
  TASK_CHECK(cues.AddBlockBlob(*blob));

  auto io = std::make_shared<FaultyIO>();
  TASK_CHECK(cluster.SpoolFrames(makeStore(io)));
  TASK_CHECK(g_releaseCount == 3);

  MemIOCallback output;
  KaxSegment segment;
  segment.WriteHead(output, 5);
  cluster.SetParent(segment);
  cluster.Render(output, cues);
  const std::vector<binary> rendered(output.GetDataBuffer(),
                                     output.GetDataBuffer() + output.getFilePointer());
  const auto parsed = parseClusters(rendered);
  TASK_CHECK(parsed.size() == 1);
  TASK_CHECK(parsed[0].track == 1);
  TASK_CHECK((parsed[0].flags & 0x80) != 0); // keyframe flag kept
  TASK_CHECK(parsed[0].frames.size() == 3);
  TASK_CHECK(expectFrames(parsed, expected, {0}) == 0);
  TASK_CHECK(cues.ListSize() == 1); // registered cue relation still produced
  return 0;
}

static int blockgroup_blob_roundtrip()
{
  g_releaseCount = 0;
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);

  auto * blob = new KaxBlockBlob(BLOCK_BLOB_NO_SIMPLE);
  blob->SetParent(cluster);
  cluster.InitTimestamp(1, 1000000);
  cluster.AddBlockBlob(blob);
  auto * buffer = makeBuffer(64, 7);
  const auto expected = frameBytes(64, 7);
  TASK_CHECK(blob->AddFrameAuto(track, 3000000, *buffer, LACING_AUTO));

  TASK_CHECK(cluster.SpoolFrames(std::make_shared<KaxFrameStore>(std::make_shared<FaultyIO>())));
  TASK_CHECK(g_releaseCount == 1);

  const auto rendered = renderCluster(cluster);
  const auto parsed = parseClusters(rendered);
  TASK_CHECK(parsed.size() == 1);
  TASK_CHECK(parsed[0].frames.size() == 1);
  TASK_CHECK(parsed[0].frames[0] == expected);
  return 0;
}

static int every_lacing_mode_is_legal_and_identical()
{
  static const LacingType modes[] = {LACING_NONE, LACING_XIPH, LACING_EBML, LACING_FIXED, LACING_AUTO};
  for (const auto mode : modes) {
    g_releaseCount = 0;
    KaxTrackEntry track; setupTrack(track);
    KaxCluster cluster; setupCluster(cluster);
    KaxBlockGroup * block = nullptr;
    std::vector<std::vector<binary>> expected;
    for (std::uint32_t i = 0; i < 3; ++i) {
      const std::uint32_t size = (mode == LACING_FIXED) ? 40u : (20u + i * 37u);
      const auto seed = static_cast<std::uint8_t>(20 + i);
      expected.push_back(frameBytes(size, seed));
      cluster.AddFrame(track, (1 + i) * 1000000, *makeBuffer(size, seed), block, mode);
    }
    const auto before = renderCluster(cluster);
    TASK_CHECK(cluster.SpoolFrames(std::make_shared<KaxFrameStore>(std::make_shared<FaultyIO>())));
    TASK_CHECK(g_releaseCount == 3);
    const auto after = renderCluster(cluster);
    TASK_CHECK(after == before);
    const auto parsed = parseClusters(after);
    // LACING_NONE seals every frame in its own block; the other modes lace them
    TASK_CHECK(parsed.size() == (mode == LACING_NONE ? 3u : 1u));
    std::size_t totalFrames = 0;
    for (const auto& parsedBlock : parsed)
      totalFrames += parsedBlock.frames.size();
    TASK_CHECK(totalFrames == 3);
    const std::vector<std::int16_t> timestamps = (mode == LACING_NONE)
      ? std::vector<std::int16_t>{0, 1, 2}
      : std::vector<std::int16_t>{0};
    TASK_CHECK(expectFrames(parsed, expected, timestamps) == 0);
  }
  return 0;
}

static int references_duration_and_identity_survive()
{
  g_releaseCount = 0;
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);

  KaxBlockGroup * keyBlock = nullptr;
  cluster.AddFrame(track, 1000000, *makeBuffer(40, 1), keyBlock, LACING_NONE);
  KaxBlockGroup * pBlock = nullptr;
  cluster.AddFrame(track, 2000000, *makeBuffer(60, 2), pBlock, LACING_NONE);
  pBlock->SetBlockDuration(500000);
  // a pre-resolved reference keeps its value without owning another block
  auto & reference = AddNewChild<KaxReferenceBlock>(*pBlock);
  reference.SetReferencedTimestamp(-1);

  // capture the block identities and metadata before the migration
  const auto * keyInternal = &static_cast<KaxInternalBlock &>(*keyBlock);
  const auto * pInternal = &static_cast<KaxInternalBlock &>(*pBlock);
  TASK_CHECK(pBlock->ReferenceCount() == 1);
  std::uint64_t duration = 0;
  TASK_CHECK(pBlock->GetBlockDuration(duration));
  const auto before = renderCluster(cluster);

  TASK_CHECK(cluster.SpoolFrames(std::make_shared<KaxFrameStore>(std::make_shared<FaultyIO>())));
  TASK_CHECK(g_releaseCount == 2);

  // the very same block objects survive, now spooled, metadata intact
  TASK_CHECK(&static_cast<KaxInternalBlock &>(*keyBlock) == keyInternal);
  TASK_CHECK(&static_cast<KaxInternalBlock &>(*pBlock) == pInternal);
  TASK_CHECK(keyInternal->HasSpooledFrames());
  TASK_CHECK(pInternal->HasSpooledFrames());
  TASK_CHECK(pBlock->ReferenceCount() == 1);
  std::uint64_t durationAfter = 0;
  TASK_CHECK(pBlock->GetBlockDuration(durationAfter));
  TASK_CHECK(durationAfter == duration);

  // references and duration are part of the byte-identical output
  TASK_CHECK(renderCluster(cluster) == before);
  return 0;
}

static int large_frames_respect_the_working_area()
{
  g_releaseCount = 0;
  constexpr std::uint32_t big = 200000; // well over the 64 KiB working area
  constexpr std::uint32_t other = 70000;
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);
  KaxBlockGroup * block = nullptr;
  std::vector<std::vector<binary>> expected;
  // LACING_NONE keeps each oversized payload in its own block, exercising the
  // chunked transfer without the lace size limits
  expected.push_back(frameBytes(big, 9));
  cluster.AddFrame(track, 1000000, *makeBuffer(big, 9), block, LACING_NONE);
  expected.push_back(frameBytes(other, 11));
  cluster.AddFrame(track, 2000000, *makeBuffer(other, 11), block, LACING_NONE);
  expected.push_back(frameBytes(33u, 12));
  cluster.AddFrame(track, 3000000, *makeBuffer(33, 12), block, LACING_NONE);

  auto backing = std::make_shared<FaultyIO>();
  TASK_CHECK(cluster.SpoolFrames(makeStore(backing)));
  TASK_CHECK(g_releaseCount == 3);
  TASK_CHECK(backing->maxWriteRequest <= KaxFrameStore::WorkBufferSize);
  TASK_CHECK(backing->maxWriteRequest == KaxFrameStore::WorkBufferSize);
  TASK_CHECK(backing->storage.size() == big + other + 33u);

  // render while observing the backing stream read requests
  MemIOCallback output;
  KaxSegment segment;
  KaxCues cues;
  cues.SetGlobalTimestampScale(1000000);
  segment.WriteHead(output, 5);
  cluster.SetParent(segment);
  cluster.Render(output, cues);
  TASK_CHECK(backing->maxReadRequest <= KaxFrameStore::WorkBufferSize);
  TASK_CHECK(backing->maxReadRequest == KaxFrameStore::WorkBufferSize);
  TASK_CHECK(backing->readCount >= 4); // the big frame needed several chunks

  const std::vector<binary> rendered(output.GetDataBuffer(),
                                     output.GetDataBuffer() + output.getFilePointer());
  const auto parsed = parseClusters(rendered);
  TASK_CHECK(parsed.size() == 3);
  std::size_t totalFrames = 0;
  for (const auto& parsedBlock : parsed)
    totalFrames += parsedBlock.frames.size();
  TASK_CHECK(totalFrames == 3);
  TASK_CHECK(expectFrames(parsed, expected, {0, 1, 2}) == 0);
  return 0;
}

static int unequal_oversized_lace_respects_the_working_area()
{
  g_releaseCount = 0;
  constexpr std::uint32_t sizes[] = {200000, 33, 70000}; // deliberately unequal
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);
  KaxBlockGroup * block = nullptr;
  std::vector<std::vector<binary>> expected;
  for (std::uint32_t i = 0; i < 3; ++i) {
    expected.push_back(frameBytes(sizes[i], static_cast<std::uint8_t>(30 + i)));
    // EBML lacing has no 1530 byte per-frame limit
    cluster.AddFrame(track, (1 + i) * 1000000, *makeBuffer(sizes[i], static_cast<std::uint8_t>(30 + i)), block, LACING_EBML);
  }

  auto backing = std::make_shared<FaultyIO>();
  TASK_CHECK(cluster.SpoolFrames(makeStore(backing)));
  TASK_CHECK(g_releaseCount == 3);
  TASK_CHECK(backing->maxWriteRequest == KaxFrameStore::WorkBufferSize);

  const auto before = renderCluster(cluster);
  TASK_CHECK(backing->maxReadRequest == KaxFrameStore::WorkBufferSize);
  const auto parsed = parseClusters(before);
  TASK_CHECK(parsed.size() == 1);
  TASK_CHECK(parsed[0].frames.size() == 3);
  TASK_CHECK(expectFrames(parsed, expected, {0}) == 0);

  // re-export on a fresh output stream keeps reading within the working area
  backing->readCount = 0;
  TASK_CHECK(renderCluster(cluster) == before);
  TASK_CHECK(backing->maxReadRequest <= KaxFrameStore::WorkBufferSize);
  TASK_CHECK(backing->readCount >= 4);
  return 0;
}

static int short_read_during_render_throws_and_retries()
{
  g_releaseCount = 0;
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);
  KaxBlockGroup * block = nullptr;
  const auto expected = frameBytes(100000, 6);
  cluster.AddFrame(track, 1000000, *makeBuffer(100000, 6), block, LACING_NONE);
  const auto goodOutput = renderCluster(cluster);

  auto backing = std::make_shared<FaultyIO>();
  TASK_CHECK(cluster.SpoolFrames(makeStore(backing)));

  backing->failReadsAfter = 1; // the second read (last chunk) is short
  MemIOCallback brokenOutput;
  KaxSegment segment;
  KaxCues cues;
  cues.SetGlobalTimestampScale(1000000);
  segment.WriteHead(brokenOutput, 5);
  cluster.SetParent(segment);
  bool threw = false;
  try {
    cluster.Render(brokenOutput, cues);
  } catch (const std::exception &) {
    threw = true;
  }
  TASK_CHECK(threw); // no silently truncated frame

  // the backing content is untouched and a fresh export succeeds
  backing->failReadsAfter = -1;
  TASK_CHECK(renderCluster(cluster) == goodOutput);
  const auto parsed = parseClusters(goodOutput);
  TASK_CHECK(parsed[0].frames[0] == expected);
  return 0;
}

static int failed_spool_tail_is_never_a_frame()
{
  g_releaseCount = 0;
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);
  KaxBlockGroup * block = nullptr;
  cluster.AddFrame(track, 1000000, *makeBuffer(1000, 1), block, LACING_XIPH);
  cluster.AddFrame(track, 2000000, *makeBuffer(2000, 2), block, LACING_XIPH);
  const auto goodOutput = renderCluster(cluster);

  auto io = std::make_shared<FaultyIO>();
  io->failWritesAfter = 1; // the second frame write is short, leaving a tail
  auto store = makeStore(io);
  TASK_CHECK(!cluster.SpoolFrames(store));
  TASK_CHECK(!io->storage.empty()); // tail bytes are present
  const auto tailSize = io->storage.size();

  // fixing the fault and retrying on the same store appends after the tail
  io->failWritesAfter = -1;
  TASK_CHECK(cluster.SpoolFrames(store));
  TASK_CHECK(io->storage.size() > tailSize); // the tail remains, unreferenced
  TASK_CHECK(renderCluster(cluster) == goodOutput);
  const auto parsed = parseClusters(renderCluster(cluster));
  TASK_CHECK(parsed[0].frames.size() == 2);
  TASK_CHECK(parsed[0].frames[0] == frameBytes(1000, 1));
  TASK_CHECK(parsed[0].frames[1] == frameBytes(2000, 2));
  return 0;
}

static int new_frames_start_a_new_block_after_spool()
{
  g_releaseCount = 0;
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);
  KaxBlockGroup * block = nullptr;
  cluster.AddFrame(track, 1000000, *makeBuffer(10, 1), block, LACING_XIPH);
  cluster.AddFrame(track, 2000000, *makeBuffer(20, 2), block, LACING_XIPH);

  auto io = std::make_shared<FaultyIO>();
  auto store = makeStore(io);
  TASK_CHECK(cluster.SpoolFrames(store));
  const std::size_t afterFirstSpool = io->storage.size();
  TASK_CHECK(g_releaseCount == 2);

  // a further frame must create a fresh block, never join the migrated lace
  cluster.AddFrame(track, 3000000, *makeBuffer(30, 3), block, LACING_XIPH);
  TASK_CHECK(block != nullptr);
  TASK_CHECK(cluster.ListSize() == 3); // timestamp + old group + new group

  // the new frame is still resident while the old block stays spooled
  TASK_CHECK(g_releaseCount == 2);
  const auto storeSizeBeforeSecond = io->storage.size();
  TASK_CHECK(cluster.SpoolFrames(store));
  TASK_CHECK(g_releaseCount == 3);
  TASK_CHECK(io->storage.size() == storeSizeBeforeSecond + 30);
  TASK_CHECK(io->storage.size() > afterFirstSpool);

  const auto rendered = renderCluster(cluster);
  const auto parsed = parseClusters(rendered);
  TASK_CHECK(parsed.size() == 2);
  TASK_CHECK(parsed[0].frames.size() == 2);
  TASK_CHECK(parsed[1].frames.size() == 1);
  TASK_CHECK(parsed[0].frames[0] == frameBytes(10, 1));
  TASK_CHECK(parsed[0].frames[1] == frameBytes(20, 2));
  TASK_CHECK(parsed[1].frames[0] == frameBytes(30, 3));
  TASK_CHECK(parsed[1].localTimestamp == 2);
  return 0;
}

// ===========================================================================
// lifetime group
// ===========================================================================

static int shared_store_outlives_the_callers_handle()
{
  g_releaseCount = 0;
  KaxTrackEntry track; setupTrack(track);

  KaxCluster first; setupCluster(first);
  KaxCluster second; setupCluster(second);
  KaxBlockGroup * block = nullptr;
  first.AddFrame(track, 1000000, *makeBuffer(15, 1), block, LACING_XIPH);
  second.AddFrame(track, 1000000, *makeBuffer(25, 2), block, LACING_NONE);
  const auto expectedFirst = renderCluster(first);
  const auto expectedSecond = renderCluster(second);

  auto io = std::make_shared<FaultyIO>();
  {
    auto store = makeStore(io);
    TASK_CHECK(first.SpoolFrames(store));
    TASK_CHECK(second.SpoolFrames(store)); // one store shared by two clusters
  } // the caller drops every handle; the blocks keep the store alive

  TASK_CHECK(g_releaseCount == 2);
  TASK_CHECK(renderCluster(first) == expectedFirst);
  TASK_CHECK(renderCluster(second) == expectedSecond);

  io.reset(); // the stream handle is gone too, the blocks still own it
  TASK_CHECK(renderCluster(first) == expectedFirst);
  TASK_CHECK(renderCluster(second) == expectedSecond);
  return 0;
}

static int release_frames_is_safe_in_both_states()
{
  g_releaseCount = 0;
  KaxTrackEntry track; setupTrack(track);

  KaxCluster resident; setupCluster(resident);
  KaxBlockGroup * block = nullptr;
  resident.AddFrame(track, 1000000, *makeBuffer(8, 1), block, LACING_NONE);
  resident.ReleaseFrames();
  TASK_CHECK(g_releaseCount == 1);
  resident.ReleaseFrames(); // again, must stay safe
  TASK_CHECK(g_releaseCount == 1);

  KaxCluster spooled; setupCluster(spooled);
  spooled.AddFrame(track, 1000000, *makeBuffer(8, 2), block, LACING_NONE);
  TASK_CHECK(spooled.SpoolFrames(std::make_shared<KaxFrameStore>(std::make_shared<FaultyIO>())));
  TASK_CHECK(g_releaseCount == 2);
  spooled.ReleaseFrames(); // no callback, no access to freed input, no crash
  TASK_CHECK(g_releaseCount == 2);
  spooled.ReleaseFrames();
  return 0;
}

static int get_buffer_throws_once_spooled()
{
  g_releaseCount = 0;
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);
  KaxBlockGroup * block = nullptr;
  cluster.AddFrame(track, 1000000, *makeBuffer(8, 1), block, LACING_NONE);
  auto & internal = static_cast<KaxInternalBlock &>(*block);
  TASK_CHECK(internal.FrameCount() == 1);
  TASK_CHECK(internal.GetBuffer(0).Size() == 8); // resident service is unchanged

  TASK_CHECK(cluster.SpoolFrames(std::make_shared<KaxFrameStore>(std::make_shared<FaultyIO>())));
  bool threw = false;
  try {
    (void)internal.GetBuffer(0);
  } catch (const std::logic_error &) {
    threw = true;
  }
  TASK_CHECK(threw); // no silent re-residency of the whole frame
  return 0;
}

static int spooled_destruction_and_pending_free_are_safe()
{
  g_releaseCount = 0;
  g_releaseThrows = true;
  {
    KaxTrackEntry track; setupTrack(track);
    KaxCluster cluster; setupCluster(cluster);
    KaxBlockGroup * block = nullptr;
    cluster.AddFrame(track, 1000000, *makeBuffer(6, 1), block, LACING_XIPH);
    cluster.AddFrame(track, 2000000, *makeBuffer(6, 2), block, LACING_XIPH);
    auto io = std::make_shared<FaultyIO>();
    TASK_CHECK(!cluster.SpoolFrames(makeStore(io)));
    TASK_CHECK(renderCluster(cluster).size() > 0);
    // destruction reclaims the detached buffers without invoking the callback
  }
  g_releaseThrows = false;

  {
    // a normally spooled cluster destroyed before its store
    KaxTrackEntry track; setupTrack(track);
    KaxCluster cluster; setupCluster(cluster);
    KaxBlockGroup * block = nullptr;
    cluster.AddFrame(track, 1000000, *makeBuffer(6, 3), block, LACING_NONE);
    auto store = std::make_shared<KaxFrameStore>(std::make_shared<FaultyIO>());
    TASK_CHECK(cluster.SpoolFrames(store));
    store.reset();
  }
  return 0;
}

static int copying_a_spooled_block_is_rejected()
{
  g_releaseCount = 0;
  KaxTrackEntry track; setupTrack(track);
  KaxCluster cluster; setupCluster(cluster);
  KaxBlockGroup * block = nullptr;
  cluster.AddFrame(track, 1000000, *makeBuffer(6, 1), block, LACING_NONE);
  TASK_CHECK(cluster.SpoolFrames(std::make_shared<KaxFrameStore>(std::make_shared<FaultyIO>())));

  bool blockThrew = false;
  try {
    KaxBlockGroup copy(*block);
    (void)copy;
  } catch (const std::logic_error &) {
    blockThrew = true;
  }
  TASK_CHECK(blockThrew);

  bool clusterThrew = false;
  try {
    KaxCluster copy(cluster);
    (void)copy;
  } catch (const std::logic_error &) {
    clusterThrew = true;
  }
  TASK_CHECK(clusterThrew);
  return 0;
}

static int constructor_rejects_a_null_stream()
{
  bool threw = false;
  try {
    KaxFrameStore store(nullptr);
    (void)store;
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  TASK_CHECK(threw);
  return 0;
}

const struct task_case task_cases[] = {
  // transaction
  {"transaction", "null_and_foreign_store_rejected", null_and_foreign_store_rejected},
  {"transaction", "short_write_keeps_everything_resident", short_write_keeps_everything_resident},
  {"transaction", "seek_failure_keeps_everything_resident", seek_failure_keeps_everything_resident},
  {"transaction", "throwing_release_callback_is_safe", throwing_release_callback_is_safe},
  // streaming
  {"streaming", "direct_blockgroup_xiph_roundtrip", direct_blockgroup_xiph_roundtrip},
  {"streaming", "simpleblock_blob_roundtrip_with_cue", simpleblock_blob_roundtrip_with_cue},
  {"streaming", "blockgroup_blob_roundtrip", blockgroup_blob_roundtrip},
  {"streaming", "references_duration_and_identity_survive", references_duration_and_identity_survive},
  {"streaming", "every_lacing_mode_is_legal_and_identical", every_lacing_mode_is_legal_and_identical},
  {"streaming", "large_frames_respect_the_working_area", large_frames_respect_the_working_area},
  {"streaming", "unequal_oversized_lace_respects_the_working_area", unequal_oversized_lace_respects_the_working_area},
  {"streaming", "short_read_during_render_throws_and_retries", short_read_during_render_throws_and_retries},
  {"streaming", "failed_spool_tail_is_never_a_frame", failed_spool_tail_is_never_a_frame},
  {"streaming", "new_frames_start_a_new_block_after_spool", new_frames_start_a_new_block_after_spool},
  // lifetime
  {"lifetime", "shared_store_outlives_the_callers_handle", shared_store_outlives_the_callers_handle},
  {"lifetime", "release_frames_is_safe_in_both_states", release_frames_is_safe_in_both_states},
  {"lifetime", "get_buffer_throws_once_spooled", get_buffer_throws_once_spooled},
  {"lifetime", "spooled_destruction_and_pending_free_are_safe", spooled_destruction_and_pending_free_are_safe},
  {"lifetime", "copying_a_spooled_block_is_rejected", copying_a_spooled_block_is_rejected},
  {"lifetime", "constructor_rejects_a_null_stream", constructor_rejects_a_null_stream},
};

const std::size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
