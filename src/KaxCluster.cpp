// Copyright © 2002-2010 Steve Lhomme.
// SPDX-License-Identifier: LGPL-2.1-or-later

/*!
  \file
  \author Steve Lhomme     <robux4 @ users.sf.net>
*/
#include "matroska/KaxCluster.h"
#include "matroska/KaxBlock.h"
#include "matroska/KaxContexts.h"
#include "matroska/KaxSegment.h"
#include "matroska/KaxFrameStore.h"
#include "matroska/KaxDefines.h"

#include <utility>
#include <vector>

using namespace libebml;

// sub elements
namespace libmatroska {

KaxCluster::KaxCluster()
  :EbmlMaster(KaxCluster::ClassInfos)
{}

KaxCluster::~KaxCluster()
{
  // a failed spool may have detached buffers whose free callback threw; they
  // belong to nobody else. Retry the reclaim without letting an exception leave
  // the destructor, then drop the DataBuffer object.
  for (auto * buffer : myPendingFreeBuffers) {
    try {
      buffer->FreeBuffer(*buffer);
    } catch (...) {
      // the payload owner could not be notified; nothing else can be done
    }
    delete buffer;
  }
}

KaxCluster::KaxCluster(const KaxCluster & ElementToClone)
  :EbmlMaster(ElementToClone)
{
  // update the parent of each children
  for (const auto& child : *this) {
    if (EbmlId(*child) == EBML_ID(KaxBlockGroup)) {
      static_cast<KaxBlockGroup   *>(child)->SetParent(*this);
    } else if (EbmlId(*child) == EBML_ID(KaxBlock)) {
      static_cast<KaxBlock        *>(child)->SetParent(*this);
    } else if (EbmlId(*child) == EBML_ID(KaxBlockVirtual)) {
      static_cast<KaxBlockVirtual *>(child)->SetParent(*this);
    }
  }
}

bool KaxCluster::AddBlockBlob(KaxBlockBlob * NewBlob)
{
  Blobs.push_back(NewBlob);
  return true;
}

namespace {

//! return the internal block of a direct BlockGroup, or nullptr
KaxInternalBlock * InternalBlockOf(KaxBlockGroup & aBlockGroup)
{
  return FindChild<KaxBlock>(aBlockGroup);
}

//! return the internal block held by a blob, whichever flavour it is
KaxInternalBlock & InternalBlockOf(KaxBlockBlob & aBlob)
{
  return static_cast<KaxInternalBlock &>(aBlob);
}

} // namespace

bool KaxCluster::SpoolFrames(const std::shared_ptr<KaxFrameStore> & aFrameStore)
{
  if (!aFrameStore)
    // an empty store cannot back any frame
    return false;

  // collect every block still owning resident payloads, both the direct
  // BlockGroups and the blobs registered but not rendered yet
  struct PendingBlock {
    KaxInternalBlock * myBlock;
    std::vector<std::uint64_t> myOffsets;
  };
  std::vector<PendingBlock> pending;
  std::vector<KaxInternalBlock *> seen; // a block may be reachable both directly and through a blob

  const auto registerBlock = [&](KaxInternalBlock & aBlock) -> bool {
    if (std::find(seen.begin(), seen.end(), &aBlock) != seen.end())
      return true; // already accounted for
    seen.push_back(&aBlock);
    if (aBlock.HasSpooledFrames()) {
      if (aBlock.SpoolStore() != aFrameStore.get())
        // the cluster already belongs to another store: refuse everything
        return false;
      return true; // already stored in this store, nothing to do
    }
    if (aBlock.HasResidentFrames())
      pending.push_back(PendingBlock{&aBlock, {}});
    return true;
  };

  for (const auto& element : GetElementList()) {
    if (EbmlId(*element) == EBML_ID(KaxBlockGroup)) {
      auto * internal = InternalBlockOf(*static_cast<KaxBlockGroup *>(element));
      if (internal && !registerBlock(*internal))
        return false;
    }
  }
  for (const auto& blob : Blobs) {
    if (!registerBlock(InternalBlockOf(*blob)))
      return false;
  }

  if (pending.empty()) {
    // nothing resident to move; still make sure no new frame can join an old lace
    currentNewBlock = nullptr;
    currentNewBlob = nullptr;
    return true;
  }

  // first phase: write every resident payload to the backing stream. A failure
  // leaves all resident buffers in place; the stream may hold tail bytes that
  // no frame will ever reference.
  for (auto& entry : pending) {
    if (!entry.myBlock->SpoolFramesWrite(aFrameStore, entry.myOffsets))
      return false;
  }

  // second phase: detach every block before any free callback can run. At this
  // point no block modification can fail and the cluster is fully spooled.
  std::vector<DataBuffer *> detached;
  for (auto& entry : pending)
    entry.myBlock->SpoolFramesCommit(aFrameStore, std::move(entry.myOffsets), detached);

  // third phase: free the detached payloads, each callback runs exactly once.
  // a throwing callback fails the call but cannot touch the spooled blocks; the
  // buffer that threw and every remaining one are reclaimed at destruction.
  while (!detached.empty()) {
    auto * buffer = detached.back();
    detached.pop_back();
    try {
      buffer->FreeBuffer(*buffer);
      delete buffer;
    } catch (...) {
      myPendingFreeBuffers.push_back(buffer);
      myPendingFreeBuffers.insert(myPendingFreeBuffers.end(),
                                  detached.begin(), detached.end());
      return false;
    }
  }

  // new frames must start a new block, never extend a migrated lace
  currentNewBlock = nullptr;
  currentNewBlob = nullptr;
  return true;
}

bool KaxCluster::AddFrameInternal(const KaxTrackEntry & track, std::uint64_t timestamp, DataBuffer & buffer, KaxBlockGroup * & MyNewBlock, const KaxBlockGroup * PastBlock, const KaxBlockGroup * ForwBlock, LacingType lacing)
{
  if (!bFirstFrameInside) {
    bFirstFrameInside = true;
    MinTimestamp = MaxTimestamp = timestamp;
  } else {
    if (timestamp < MinTimestamp)
      MinTimestamp = timestamp;
    if (timestamp > MaxTimestamp)
      MaxTimestamp = timestamp;
  }

  MyNewBlock = nullptr;

  if (lacing == LACING_NONE || !track.LacingEnabled()) {
    currentNewBlock = nullptr;
  }

  // a lace whose payloads were spooled is sealed: a following frame must start
  // a fresh block rather than joining the migrated one
  const auto * currentInternal = currentNewBlock ? FindChild<KaxBlock>(*currentNewBlock) : nullptr;
  const bool currentIsSpooled = currentInternal && currentInternal->HasSpooledFrames();

  // force creation of a new block
  if (!currentNewBlock || currentIsSpooled || static_cast<std::uint32_t>(track.TrackNumber()) != static_cast<std::uint32_t>(currentNewBlock->TrackNumber()) || PastBlock || ForwBlock) {
    KaxBlockGroup & aNewBlock = GetNewBlock();
    MyNewBlock = currentNewBlock = &aNewBlock;
  }

  if (PastBlock) {
    if (ForwBlock) {
      if (currentNewBlock->AddFrame(track, timestamp, buffer, *PastBlock, *ForwBlock, lacing)) {
        // more data are allowed in this Block
        return true;
      }

      currentNewBlock = nullptr;
      return false;
    }
    if (currentNewBlock->AddFrame(track, timestamp, buffer, *PastBlock, lacing)) {
        // more data are allowed in this Block
        return true;
      }
    currentNewBlock = nullptr;
    return false;
  }

  if (currentNewBlock->AddFrame(track, timestamp, buffer, lacing)) {
    // more data are allowed in this Block
    return true;
  }

  currentNewBlock = nullptr;
  return false;
}

bool KaxCluster::AddFrame(const KaxTrackEntry & track, std::uint64_t timestamp, DataBuffer & buffer, KaxBlockGroup * & MyNewBlock, LacingType lacing)
{
  assert(Blobs.empty()); // mutually exclusive for the moment
  return AddFrameInternal(track, timestamp, buffer, MyNewBlock, nullptr, nullptr, lacing);
}

bool KaxCluster::AddFrame(const KaxTrackEntry & track, std::uint64_t timestamp, DataBuffer & buffer, KaxBlockGroup * & MyNewBlock, const KaxBlockGroup & PastBlock, LacingType lacing)
{
  assert(Blobs.empty()); // mutually exclusive for the moment
  return AddFrameInternal(track, timestamp, buffer, MyNewBlock, &PastBlock, nullptr, lacing);
}

bool KaxCluster::AddFrame(const KaxTrackEntry & track, std::uint64_t timestamp, DataBuffer & buffer, KaxBlockGroup * & MyNewBlock, const KaxBlockGroup & PastBlock, const KaxBlockGroup & ForwBlock, LacingType lacing)
{
  assert(Blobs.empty()); // mutually exclusive for the moment
  return AddFrameInternal(track, timestamp, buffer, MyNewBlock, &PastBlock, &ForwBlock, lacing);
}

/*!
  \todo only put the Blocks written in the cue entries
*/
filepos_t KaxCluster::Render(IOCallback & output, KaxCues & CueToUpdate, const ShouldWrite& writeFilter)
{
  filepos_t Result = 0;

  // update the timestamp of the Cluster before writing
  auto ClusterTimestamp = FindChild<KaxClusterTimestamp>(*this);
  ClusterTimestamp->SetValue(GlobalTimestamp() / GlobalTimestampScale());

  if (Blobs.empty()) {
    // old-school direct KaxBlockGroup

    Result = EbmlMaster::Render(output, writeFilter);
    // For all Blocks add their position on the CueEntry

    for (const auto& element : GetElementList()) {
      if (EbmlId(*element) == EBML_ID(KaxBlockGroup)) {
        CueToUpdate.PositionSet(*static_cast<const KaxBlockGroup *>(element));
      }
    }
  } else {
    // new school, using KaxBlockBlob
    for (const auto& blob : Blobs) {
      if (blob->IsSimpleBlock())
        PushElement( static_cast<KaxSimpleBlock&>(*blob));
      else
        PushElement( static_cast<KaxBlockGroup&>(*blob));
    }

    Result = EbmlMaster::Render(output, writeFilter);

    // For all Blocks add their position on the CueEntry
    for (const auto& blob : Blobs)
      CueToUpdate.PositionSet(*blob);

    Blobs.clear();
  }

  return Result;
}

/*!
  \todo automatically choose valid timestamp for the Cluster based on the previous cluster timestamp (must be incremental)
*/
std::uint64_t KaxCluster::GlobalTimestamp() const
{
  assert(bPreviousTimestampIsSet);
  std::uint64_t result = MinTimestamp;

  if (result < PreviousTimestamp)
    result = PreviousTimestamp + 1;

  return result;
}

/*!
  \brief retrieve the relative
  \todo !!! We need a way to know the TimestampScale
*/
std::int16_t KaxCluster::GetBlockLocalTimestamp(std::uint64_t aGlobalTimestamp) const
{
  const std::int64_t TimestampDelay = (static_cast<std::int64_t>(aGlobalTimestamp) - static_cast<std::int64_t>(GlobalTimestamp())) / static_cast<std::int64_t>(GlobalTimestampScale());
  assert(TimestampDelay >= std::int16_t(0x8000) && TimestampDelay <= std::int16_t(0x7FFF));
  return static_cast<std::int16_t>(TimestampDelay);
}

std::uint64_t KaxCluster::GetBlockGlobalTimestamp(std::int16_t LocalTimestamp)
{
  if (!bFirstFrameInside) {
    auto ClusterTimestamp = FindChild<KaxClusterTimestamp>(*this);
    assert (bFirstFrameInside); // use the InitTimestamp() hack for now
    MinTimestamp = MaxTimestamp = PreviousTimestamp = static_cast<std::uint64_t>(*static_cast<EbmlUInteger *>(ClusterTimestamp));
    bFirstFrameInside = true;
    bPreviousTimestampIsSet = true;
  }
  return static_cast<std::int64_t>(LocalTimestamp * GlobalTimestampScale()) + GlobalTimestamp();
}

KaxBlockGroup & KaxCluster::GetNewBlock()
{
  auto & MyBlock = AddNewChild<KaxBlockGroup>(*this);
  MyBlock.SetParent(*this);
  return MyBlock;
}

void KaxCluster::ReleaseFrames()
{
  for (const auto& element : GetElementList()) {
    if (EbmlId(*element) == EBML_ID(KaxBlockGroup)) {
      static_cast<KaxBlockGroup *>(element)->ReleaseFrames();
    }
  }
}

std::uint64_t KaxCluster::GetPosition() const
{
  assert(ParentSegment);
  return ParentSegment->GetRelativePosition(*this);
}

} // namespace libmatroska
