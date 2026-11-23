// Copyright © 2026 Steve Lhomme.
// SPDX-License-Identifier: LGPL-2.1-or-later

/*!
  \file
  \brief Frame payload backing store for deferred Matroska rendering.

  A muxer usually accumulates compressed frames in memory while it is not yet
  known which Matroska output they will end up in. A KaxFrameStore lets a
  KaxCluster move the frame payloads out of the in-memory DataBuffer objects
  into a caller provided stream once a batch is complete. The block objects,
  timestamps, references and lacing metadata stay in memory; only the payload
  bytes are externalised. The cluster can later be rendered through
  KaxCluster::Render() without decoding any media, streaming the payloads back
  from the store through a bounded temporary buffer.
*/
#ifndef LIBMATROSKA_FRAME_STORE_H
#define LIBMATROSKA_FRAME_STORE_H

#include "matroska/KaxConfig.h"

#include <ebml/IOCallback.h>

#include <cstdint>
#include <memory>

namespace libmatroska {

class KaxCluster;

/*!
  \class KaxFrameStore
  \brief Owns the backing stream that receives spooled frame payloads.

  The store and its stream are held through shared ownership. Every spooled
  block keeps a reference to the store, so both stay alive until the last block
  using them has been released, even if the caller drops its own copy.

  The backing stream is supplied by the caller and must start empty; it has to
  be readable, writable and seekable. It must be a different stream than the
  one passed to KaxCluster::Render().
*/
class MATROSKA_DLL_API KaxFrameStore {
  public:
    //! Maximum size of a single payload read/write request while rendering
    static constexpr std::size_t WorkBufferSize = 64u * 1024u;

    /*!
      \brief Build a store around a caller owned backing stream.
      \param aBackingStream a readable, writable and seekable stream, initially empty
      \throws std::invalid_argument when \a aBackingStream is null
    */
    explicit KaxFrameStore(std::shared_ptr<libebml::IOCallback> aBackingStream);

    KaxFrameStore(const KaxFrameStore &) = delete;
    KaxFrameStore & operator=(const KaxFrameStore &) = delete;

    /*!
      \brief the stream backing the spooled payloads
    */
    libebml::IOCallback & BackingStream() const {return *myBackingStream;}

    /*!
      \brief number of payload bytes already committed to the store

      Uncommitted tail bytes left behind by a failed spool are counted as part
      of the stream position but never referenced as frame data.
    */
    std::uint64_t SpooledBytes() const {return myWriteEnd;}

  protected:
    /*!
      \brief Reserve \a aFrameSize bytes at the end of the store.

      Positions the backing stream at the first byte of the reservation and
      returns the absolute offset of that reservation. The caller then writes
      exactly \a aFrameSize bytes. On failure nothing about the reservation is
      remembered as frame data.
    */
    std::uint64_t BeginFrameWrite(std::uint32_t aFrameSize);

  private:
    friend class KaxInternalBlock;
    friend class KaxCluster;

    std::shared_ptr<libebml::IOCallback> myBackingStream;
    std::uint64_t myWriteEnd{0}; ///< end of the committed payload area
};

} // namespace libmatroska

#endif // LIBMATROSKA_FRAME_STORE_H
