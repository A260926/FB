// Copyright © 2002-2010 Steve Lhomme.
// SPDX-License-Identifier: LGPL-2.1-or-later

/*!
  \file
  \todo add a PureBlock class to group functionalities between Block and BlockVirtual
  \author Steve Lhomme     <robux4 @ users.sf.net>
  \author Julien Coloos    <suiryc @ users.sf.net>
*/
#ifndef LIBMATROSKA_BLOCK_H
#define LIBMATROSKA_BLOCK_H

#include <vector>
#include <memory>
#include <stdexcept>

#include "matroska/KaxTypes.h"
#include <ebml/EbmlBinary.h>
#include <ebml/EbmlMaster.h>
#include "matroska/KaxTracks.h"
#include "matroska/KaxDefines.h"

namespace libmatroska {

class KaxCluster;
class KaxFrameStore;
class KaxReferenceBlock;
class KaxInternalBlock;
class KaxBlockBlob;

class MATROSKA_DLL_API DataBuffer {
  protected:
    libebml::binary  *myBuffer{nullptr};
    std::uint32_t mySize;
    bool     bValidValue{true};
    bool     (*myFreeBuffer)(const DataBuffer & aBuffer); // method to free the internal buffer
    bool     bInternalBuffer;

  public:
    DataBuffer(libebml::binary * aBuffer, std::uint32_t aSize, bool (*aFreeBuffer)(const DataBuffer & aBuffer) = nullptr, bool _bInternalBuffer = false)
      :mySize(aSize)
      ,myFreeBuffer(aFreeBuffer)
      ,bInternalBuffer(_bInternalBuffer)
    {
      if (bInternalBuffer)
      {
        try {
          myBuffer = new libebml::binary[mySize];
          memcpy(myBuffer, aBuffer, mySize);
        } catch (const std::bad_alloc &) {
          bValidValue = false;
        }
      }
      else
        myBuffer = aBuffer;
    }

    virtual ~DataBuffer() = default;
    virtual libebml::binary * Buffer() {assert(bValidValue); return myBuffer;}
    virtual std::uint32_t & Size() {return mySize;};
    virtual const libebml::binary * Buffer() const {assert(bValidValue); return myBuffer;}
    virtual std::uint32_t Size()   const {return mySize;};
    bool    FreeBuffer(const DataBuffer & aBuffer) {
      bool bResult = true;
      if (myBuffer && bValidValue) {
        if (myFreeBuffer)
          bResult = myFreeBuffer(aBuffer);
        if (bInternalBuffer)
          delete [] myBuffer;
        myBuffer = nullptr;
        mySize = 0;
        bValidValue = false;
      }
      return bResult;
    }

    virtual DataBuffer * Clone();
};

class MATROSKA_DLL_API SimpleDataBuffer : public DataBuffer {
  public:
    SimpleDataBuffer(libebml::binary * aBuffer, std::uint32_t aSize, std::uint32_t aOffset, bool (*aFreeBuffer)(const DataBuffer & aBuffer) = myFreeBuffer)
      :DataBuffer(aBuffer + aOffset, aSize, aFreeBuffer)
      ,Offset(aOffset)
      ,BaseBuffer(aBuffer)
    {}
    ~SimpleDataBuffer() override = default;

    DataBuffer * Clone() override {return new SimpleDataBuffer(*this);}

  protected:
    std::uint32_t Offset;
    libebml::binary * BaseBuffer;

    static bool myFreeBuffer(const DataBuffer & aBuffer)
    {
      auto _Buffer = static_cast<const SimpleDataBuffer*>(&aBuffer)->BaseBuffer;
      if (_Buffer)
        free(_Buffer);
      return true;
    }

    SimpleDataBuffer(const SimpleDataBuffer & ToClone);
};

/*!
  \note the data is copied locally, it can be freed right away
* /
class MATROSKA_DLL_API NotSoSimpleDataBuffer : public SimpleDataBuffer {
  public:
    NotSoSimpleDataBuffer(libebml::binary * aBuffer, std::uint32_t aSize, std::uint32_t aOffset)
      :SimpleDataBuffer(new libebml::binary[aSize - aOffset], aSize, 0)
    {
      memcpy(BaseBuffer, aBuffer + aOffset, aSize - aOffset);
    }
};
*/

DECLARE_MKX_MASTER(KaxBlockGroup)
  public:
    ~KaxBlockGroup() override = default;

    /*!
      \brief Addition of a frame without references
    */
    bool AddFrame(const KaxTrackEntry & track, std::uint64_t timestamp, DataBuffer & buffer, LacingType lacing = LACING_AUTO);
    /*!
      \brief Addition of a frame with a backward reference (P frame)
    */
    bool AddFrame(const KaxTrackEntry & track, std::uint64_t timestamp, DataBuffer & buffer, const KaxBlockGroup & PastBlock, LacingType lacing = LACING_AUTO);

    /*!
      \brief Addition of a frame with a backward+forward reference (B frame)
    */
    bool AddFrame(const KaxTrackEntry & track, std::uint64_t timestamp, DataBuffer & buffer, const KaxBlockGroup & PastBlock, const KaxBlockGroup & ForwBlock, LacingType lacing = LACING_AUTO);
    bool AddFrame(const KaxTrackEntry & track, std::uint64_t timestamp, DataBuffer & buffer, const KaxBlockBlob * PastBlock, const KaxBlockBlob * ForwBlock, LacingType lacing = LACING_AUTO);

    void SetParent(KaxCluster & aParentCluster);

    void SetParentTrack(const KaxTrackEntry & aParentTrack) {
      ParentTrack = &aParentTrack;
    }

    /*!
      \brief Set the duration of the contained frame(s) (for the total number of frames)
    */
    void SetBlockDuration(std::uint64_t TimeLength);
    bool GetBlockDuration(std::uint64_t &TheTimestamp) const;

    /*!
      \return the global timestamp of this Block (not just the delta to the Cluster)
    */
    std::uint64_t GlobalTimestamp() const;
    std::uint64_t GlobalTimestampScale() const {
      assert(ParentTrack);
      return ParentTrack->GlobalTimestampScale();
    }

    std::uint16_t TrackNumber() const;

    std::uint64_t ClusterPosition() const;

    /*!
      \return the number of references to other frames
    */
    unsigned int ReferenceCount() const;
    const KaxReferenceBlock & Reference(unsigned int Index) const;

    /*!
      \brief release all the frames of all Blocks
    */
    void ReleaseFrames();

    operator KaxInternalBlock &();

    const KaxCluster *GetParentCluster() const { return ParentCluster; }

  protected:
    KaxCluster * ParentCluster{nullptr};
    const KaxTrackEntry * ParentTrack{nullptr};
};

class MATROSKA_DLL_API KaxInternalBlock : public libebml::EbmlBinary {
  friend class KaxCluster;
  public:
    KaxInternalBlock(const libebml::EbmlCallbacks & classInfo)
      :libebml::EbmlBinary(classInfo)
    {}
    KaxInternalBlock(const KaxInternalBlock & ElementToClone);
    ~KaxInternalBlock() override;
    bool SizeIsValid(std::uint64_t size) const override
    {
      return size >= 4; /// for the moment
    }

    std::uint16_t TrackNum() const {return TrackNumber;}
    /*!
      \todo !!!! This method needs to be changes !
    */
    std::uint64_t GlobalTimestamp() const {return Timestamp;}

    /*!
      \note override this function to generate the Data/Size on the fly, unlike the usual binary elements
    */
    libebml::filepos_t UpdateSize(const ShouldWrite & writeFilter = WriteSkipDefault, bool bForceRender = false) override;
    libebml::filepos_t ReadData(libebml::IOCallback & input, libebml::ScopeMode ReadFully = libebml::SCOPE_ALL_DATA) override;

    /*!
      \brief Only read the head of the Block (not internal data)
      \note convenient when you are parsing the file quickly
    */
    std::uint64_t ReadInternalHead(libebml::IOCallback & input);

    unsigned int NumberFrames() const { return SizeList.size();}
    DataBuffer & GetBuffer(unsigned int iIndex) {
      if (mySpoolStore)
        throw std::logic_error("KaxInternalBlock::GetBuffer: frame payloads were moved to a KaxFrameStore");
      return *myBuffers[iIndex];
    }

    /*!
      \return true when the frame payloads live in a KaxFrameStore rather than in memory
    */
    bool HasSpooledFrames() const {return static_cast<bool>(mySpoolStore);}

    /*!
      \return true while the block still owns in-memory frame payloads
    */
    bool HasResidentFrames() const {return !mySpoolStore && !myBuffers.empty();}

    /*!
      \return the store owning the payloads, or nullptr when the frames are resident
    */
    const KaxFrameStore * SpoolStore() const {return mySpoolStore.get();}

    bool AddFrame(const KaxTrackEntry & track, std::uint64_t timestamp, DataBuffer & buffer, LacingType lacing = LACING_AUTO, bool invisible = false);

    /*!
      \brief move all resident frame payloads into the given backing store
      \return false and leave every resident frame untouched on failure
      \note blocks already attached to another store are rejected by the cluster
    */
    bool SpoolFrames(const std::shared_ptr<KaxFrameStore> & aStore);

    /*!
      \brief render the block head, lacing metadata and the streamed payloads

      Used instead of RenderData() once the payloads have been spooled. Payload
      bytes pass through a bounded 64 KiB working area regardless of the frame
      or lace size. Read failures or output failures are thrown to the caller
      and the backing store is never modified.
    */
    libebml::filepos_t RenderSpooledData(libebml::IOCallback & output);

    /*!
      \brief size of frame \a iIndex, valid for resident and spooled frames
    */
    std::uint32_t FramePayloadSize(std::size_t iIndex) const;

    /*!
      \brief number of frames, resident frames are tracked by myBuffers
    */
    std::size_t FrameCount() const {
      return mySpoolStore ? SizeList.size() : myBuffers.size();
    }

    /*!
      \brief release all the frames of all Blocks
    */
    void ReleaseFrames();

    void SetParent(KaxCluster & aParentCluster);

    /*!
      \return Returns the lacing type that produces the smallest footprint.
    */
    LacingType GetBestLacingType() const;

    /*!
      \param FrameNumber 0 for the first frame
      \return the position in the stream for a given frame
      \note return -1 if the position doesn't exist
    */
    std::int64_t GetDataPosition(std::size_t FrameNumber = 0);

    /*!
      \param FrameNumber 0 for the first frame
      \return the size of a given frame
      \note return -1 if the position doesn't exist
    */
    std::int64_t GetFrameSize(std::size_t FrameNumber = 0);

    bool IsInvisible() const { return mInvisible; }

    std::uint64_t ClusterPosition() const;

    /*!
     * \return Get the timestamp as written in the Block (not scaled).
     * \since LIBMATROSKA_VERSION >= 0x010700
     */
    std::int16_t GetRelativeTimestamp() const { return LocalTimestamp; }

    /*!
     * \return the lacing type read or used for writing
     */
    LacingType GetCurrentLacing() const { return mLacing; }

  protected:
    std::vector<DataBuffer *> myBuffers;
    std::vector<std::int32_t> SizeList;
    std::uint64_t             Timestamp; // temporary timestamp of the first frame, non scaled
    std::int16_t              LocalTimestamp;
    bool                      bLocalTimestampUsed{false};
    std::uint16_t               TrackNumber;
    LacingType                mLacing{LACING_AUTO};
    bool                      mInvisible{false};
    std::uint64_t             FirstFrameLocation;

    KaxCluster               *ParentCluster{nullptr};

    // frame payload backing store; when set, myBuffers is empty and the payload
    // bytes live at mySpoolOffsets in the store stream
    std::shared_ptr<KaxFrameStore> mySpoolStore;
    std::vector<std::uint64_t>    mySpoolOffsets;

    /*!
      \brief first spool phase: write all resident payloads to the store stream

      On failure the block keeps its resident buffers; the store may carry
      uncommitted tail bytes that are never referenced.
    */
    bool SpoolFramesWrite(const std::shared_ptr<KaxFrameStore> & aStore,
                          std::vector<std::uint64_t> & aOffsets);

    /*!
      \brief second spool phase: detach the resident buffers and keep the store references

      The DataBuffer objects are handed back untouched so the caller can free
      them once every block of the transaction is detached. The block becomes
      fully spooled and exportable without throwing itself.
    */
    void SpoolFramesCommit(const std::shared_ptr<KaxFrameStore> & aStore,
                           std::vector<std::uint64_t> && aOffsets,
                           std::vector<DataBuffer *> & aDetachedBuffers);

    libebml::filepos_t RenderData(libebml::IOCallback & output, bool bForceRender, const ShouldWrite & writeFilter = WriteSkipDefault) override;
};

DECLARE_xxx_BASE(KaxBlock, MATROSKA_DLL_API, KaxInternalBlock)
    MATROSKA_CLASS_BODY(KaxBlock)
};

DECLARE_xxx_BASE(KaxSimpleBlock, MATROSKA_DLL_API, KaxInternalBlock)
  private:
    bool                      bIsKeyframe{true};
    bool                      bIsDiscardable{false};
  public:
    void SetKeyframe(bool b_keyframe) { bIsKeyframe = b_keyframe; }
    void SetDiscardable(bool b_discard) { bIsDiscardable = b_discard; }

    bool IsKeyframe() const    { return bIsKeyframe; }
    bool IsDiscardable() const { return bIsDiscardable; }

    void SetParent(KaxCluster & aParentCluster);

    MATROSKA_CLASS_BODY(KaxSimpleBlock)
};

/// Placeholder class for either a BlockGroup or a SimpleBlock
class MATROSKA_DLL_API KaxBlockBlob {
public:
  KaxBlockBlob(BlockBlobType sblock_mode) : SimpleBlockMode(sblock_mode) {
    bUseSimpleBlock = (sblock_mode != BLOCK_BLOB_NO_SIMPLE);
    Block.group = nullptr;
  }

  ~KaxBlockBlob() {
    if (bUseSimpleBlock)
      delete Block.simpleblock;
    else
      delete Block.group;
  }

  operator KaxBlockGroup &() const;
  operator KaxSimpleBlock &() const;
  operator KaxInternalBlock &() const;

  void SetBlockGroup( KaxBlockGroup &BlockRef );

  void SetBlockDuration(std::uint64_t TimeLength);

  void SetParent(KaxCluster & aParentCluster);
  bool AddFrameAuto(const KaxTrackEntry & track, std::uint64_t timestamp, DataBuffer & buffer, LacingType lacing = LACING_AUTO, const KaxBlockBlob * PastBlock = nullptr, const KaxBlockBlob * ForwBlock = nullptr);

  bool IsSimpleBlock() const {return bUseSimpleBlock;}

  bool ReplaceSimpleByGroup();
protected:
  KaxCluster *ParentCluster{nullptr};
  union {
    KaxBlockGroup *group;
    KaxSimpleBlock *simpleblock;
  } Block;
  bool bUseSimpleBlock;
  BlockBlobType SimpleBlockMode;
};

DECLARE_MKX_BINARY_CONS(KaxBlockVirtual)
  public:
    ~KaxBlockVirtual() override;

    /*!
      \note override this function to generate the Data/Size on the fly, unlike the usual binary elements
    */
    libebml::filepos_t UpdateSize(const ShouldWrite & writeFilter = WriteSkipDefault, bool bForceRender = false) override;

    void SetParent(const KaxCluster & aParentCluster) {ParentCluster = &aParentCluster;}

    libebml::filepos_t RenderData(libebml::IOCallback & output, bool bForceRender, const ShouldWrite & writeFilter) override;

    libebml::filepos_t ReadData(libebml::IOCallback & input, libebml::ScopeMode ReadFully = libebml::SCOPE_ALL_DATA) override;

  protected:
    std::uint64_t Timestamp; // temporary timestamp of the first frame if there are more than one
    std::uint16_t TrackNumber;
    libebml::binary DataBlock[5];

    const KaxCluster * ParentCluster{nullptr};
};

} // namespace libmatroska

#endif // LIBMATROSKA_BLOCK_H
