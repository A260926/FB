/****************************************************************************
** libmatroska : parse Matroska files, see http://www.matroska.org/
**
** Ordered-chapter reader: event playback over pre-scanned sources.
**
** This file is part of libmatroska.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License as published by the Free Software Foundation; either
** version 2.1 of the License, or (at your option) any later version.
**
**********************************************************************/

#include "matroska/KaxOrderedReader.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "KaxOrderedInternal.h"

namespace libmatroska {
namespace {

using ordered_detail::ChapterRec;
using ordered_detail::FrameRef;
using ordered_detail::SourceMeta;

constexpr uint64_t kNotFound = std::numeric_limits<uint64_t>::max();

/*!
  Read exactly \p size bytes at \p offset into \p data.  Any short read or IO
  exception is reported as false; the caller keeps its logical state.
*/
bool ReadPayload(libebml::IOCallback& io, uint64_t offset, uint64_t size,
                 std::vector<uint8_t>& data)
{
  if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    return false;
  if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    return false;

  data.resize(static_cast<size_t>(size));
  if (size == 0)
    return true;

  try {
    io.setFilePointer(static_cast<int64_t>(offset), libebml::seek_beginning);
  } catch (...) {
    return false;
  }

  size_t done = 0;
  while (done < data.size()) {
    uint32_t got = 0;
    try {
      got = io.read(data.data() + done, data.size() - done);
    } catch (...) {
      return false;
    }
    if (got == 0)
      return false;
    done += got;
  }
  return true;
}

uint64_t FindChapter(const std::vector<ChapterRec>& chapters, uint64_t program_ns)
{
  auto it = std::upper_bound(
      chapters.begin(), chapters.end(), program_ns,
      [](uint64_t value, const ChapterRec& chapter) {
        return value < chapter.program_start_ns;
      });
  if (it == chapters.begin())
    return kNotFound;
  --it;
  if (program_ns >= it->ProgramEndNS())
    return kNotFound;
  return static_cast<uint64_t>(it - chapters.begin());
}

bool FrameVisibleWindow(const ChapterRec& chapter, const FrameRef& frame,
                        uint64_t clip_ns, uint64_t& visible_begin,
                        uint64_t& visible_duration)
{
  const uint64_t frame_begin = frame.source_ns;
  const uint64_t frame_end = frame.source_ns + frame.duration_ns;

  uint64_t lo = std::max(frame_begin, chapter.begin_ns);
  lo = std::max(lo, clip_ns);
  const uint64_t hi = std::min(frame_end, chapter.end_ns);

  if (lo >= hi) {
    visible_begin = 0;
    visible_duration = 0;
    return false;
  }
  visible_begin = lo;
  visible_duration = hi - lo;
  return true;
}

} // namespace

KaxOrderedReader::KaxOrderedReader() : impl_(new KaxOrderedReaderImpl()) {}

KaxOrderedReader::~KaxOrderedReader() = default;

bool KaxOrderedReader::Open(
    const std::vector<std::shared_ptr<libebml::IOCallback>>& sources,
    size_t rootSource,
    uint64_t editionUID,
    uint64_t trackUID,
    std::unique_ptr<KaxOrderedReader>& out)
{
  out.reset();

  auto impl = std::unique_ptr<KaxOrderedReaderImpl>(new KaxOrderedReaderImpl());
  if (!ordered_detail::BuildOrderedIndex(sources, rootSource, editionUID,
                                         trackUID, *impl))
    return false;

  impl->ended = impl->chapters.empty();
  impl->chapter_index = 0;
  impl->frame_index = impl->chapters.empty() ? 0 : impl->chapters.front().first_frame;
  impl->need_config = !impl->chapters.empty();
  impl->clip_ns = impl->chapters.empty() ? 0 : impl->chapters.front().begin_ns;

  auto reader = std::unique_ptr<KaxOrderedReader>(new KaxOrderedReader());
  reader->impl_ = std::move(impl);
  out = std::move(reader);
  return true;
}

uint64_t KaxOrderedReader::DurationNS() const
{
  return impl_->duration_ns;
}

bool KaxOrderedReader::Seek(uint64_t programNS)
{
  KaxOrderedReaderImpl& impl = *impl_;
  if (programNS > impl.duration_ns)
    return false;

  if (programNS == impl.duration_ns || impl.chapters.empty()) {
    impl.ended = true;
    return true;
  }

  const uint64_t index = FindChapter(impl.chapters, programNS);
  if (index == kNotFound)
    return false;
  const ChapterRec& chapter = impl.chapters[index];
  const SourceMeta& source = impl.sources[chapter.source_index];
  const uint64_t target_ns =
      chapter.begin_ns + (programNS - chapter.program_start_ns);

  // Back up to the closest keyframe in this chapter that does not start
  // after the target.  The chapter start is guaranteed to be a keyframe.
  size_t keyframe = chapter.first_frame;
  bool found = false;
  for (size_t i = chapter.first_frame; i < chapter.end_frame; ++i) {
    const FrameRef& frame = source.frames[i];
    if (frame.source_ns > target_ns)
      break;
    if (frame.keyframe) {
      keyframe = i;
      found = true;
    }
  }
  if (!found)
    return false;

  impl.ended = false;
  impl.chapter_index = static_cast<size_t>(index);
  impl.frame_index = keyframe;
  impl.need_config = true;
  impl.clip_ns = target_ns;
  return true;
}

KaxOrderedReader::Status KaxOrderedReader::ReadNext(Packet& out)
{
  KaxOrderedReaderImpl& impl = *impl_;
  if (impl.ended || impl.chapters.empty()) {
    impl.ended = true;
    return Status::End;
  }

  if (impl.chapter_index >= impl.chapters.size()) {
    impl.ended = true;
    return Status::End;
  }

  const size_t chapter_index = impl.chapter_index;
  const ChapterRec& chapter = impl.chapters[chapter_index];
  const SourceMeta& source = impl.sources[chapter.source_index];

  if (impl.need_config) {
    Packet packet;
    packet.kind = Kind::Config;
    packet.source_index = chapter.source_index;
    packet.chapter_index = chapter.original_index;
    packet.track_number = source.track_number;
    packet.source_ns = chapter.begin_ns;
    packet.program_ns = chapter.program_start_ns;
    packet.duration_ns = 0;
    packet.visible_begin_ns = 0;
    packet.visible_duration_ns = 0;
    packet.keyframe = false;
    packet.data = source.codec_private;

    out = std::move(packet);
    impl.need_config = false;
    return Status::Ok;
  }

  if (impl.frame_index >= chapter.end_frame) {
    // Defensive: advance to the next chapter.
    impl.need_config = true;
    ++impl.chapter_index;
    if (impl.chapter_index >= impl.chapters.size()) {
      impl.ended = true;
      return Status::End;
    }
    impl.frame_index = impl.chapters[impl.chapter_index].first_frame;
    impl.clip_ns = impl.chapters[impl.chapter_index].begin_ns;
    return ReadNext(out);
  }

  if (impl.frame_index >= source.frames.size())
    return Status::Error;

  const FrameRef& frame = source.frames[impl.frame_index];

  std::vector<uint8_t> data;
  if (!ReadPayload(*source.io, frame.payload_offset, frame.payload_size, data))
    return Status::Error;

  Packet packet;
  packet.kind = Kind::Frame;
  packet.source_index = chapter.source_index;
  packet.chapter_index = chapter.original_index;
  packet.track_number = source.track_number;
  packet.source_ns = frame.source_ns;
  packet.program_ns = chapter.program_start_ns + (frame.source_ns - chapter.begin_ns);
  packet.duration_ns = frame.duration_ns;
  packet.keyframe = frame.keyframe;
  FrameVisibleWindow(chapter, frame, impl.clip_ns, packet.visible_begin_ns,
                     packet.visible_duration_ns);
  packet.data = std::move(data);

  // Commit the cursor only after the payload was read successfully so an IO
  // failure leaves the same event pending for retry.
  ++impl.frame_index;
  if (impl.frame_index >= chapter.end_frame) {
    impl.need_config = true;
    ++impl.chapter_index;
    if (impl.chapter_index >= impl.chapters.size()) {
      impl.ended = true;
    } else {
      impl.frame_index = impl.chapters[impl.chapter_index].first_frame;
      impl.clip_ns = impl.chapters[impl.chapter_index].begin_ns;
    }
  }

  out = std::move(packet);
  return Status::Ok;
}

} // namespace libmatroska
