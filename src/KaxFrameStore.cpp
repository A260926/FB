// Copyright © 2026 Steve Lhomme.
// SPDX-License-Identifier: LGPL-2.1-or-later

/*!
  \file
  \brief Frame payload backing store for deferred Matroska rendering.
*/
#include "matroska/KaxFrameStore.h"

#include <stdexcept>
#include <utility>

namespace libmatroska {

KaxFrameStore::KaxFrameStore(std::shared_ptr<libebml::IOCallback> aBackingStream)
  :myBackingStream(std::move(aBackingStream))
{
  if (!myBackingStream)
    throw std::invalid_argument("KaxFrameStore requires a non-null backing stream");
}

std::uint64_t KaxFrameStore::BeginFrameWrite(std::uint32_t aFrameSize)
{
  // payloads are appended one after another at the end of the committed area;
  // a failing seek/write leaves uncommitted tail bytes that no frame references
  myBackingStream->setFilePointer(static_cast<std::int64_t>(myWriteEnd),
                                  libebml::seek_beginning);
  const auto frameOffset = myWriteEnd;
  myWriteEnd += aFrameSize;
  return frameOffset;
}

} // namespace libmatroska
