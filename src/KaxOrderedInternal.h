/****************************************************************************
** libmatroska : parse Matroska files, see http://www.matroska.org/
**
** Internal support for KaxOrderedReader.  Not a public header.
**
** This file is part of libmatroska.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License as published by the Free Software Foundation; either
** version 2.1 of the License, or (at your option) any later version.
**
**********************************************************************/

#ifndef LIBMATROSKA_ORDERED_INTERNAL_H
#define LIBMATROSKA_ORDERED_INTERNAL_H

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ebml/IOCallback.h"

namespace libmatroska {

namespace ordered_detail {

struct Range {
  uint64_t data_pos{0};
  uint64_t size{0};
  uint64_t end{0};
};

struct FrameRef {
  uint64_t payload_offset{0};  // absolute file offset of the frame payload
  uint64_t payload_size{0};
  uint64_t source_ns{0};
  uint64_t duration_ns{0};
  bool keyframe{false};
};

struct TrackMeta {
  bool has_number{false};
  uint64_t number{0};
  bool has_uid{false};
  uint64_t uid{0};
  bool has_type{false};
  uint64_t type{0};
  bool has_codec_id{false};
  std::string codec_id;
  bool has_codec_private{false};
  std::vector<uint8_t> codec_private;
  uint64_t codec_delay{0};
  uint64_t seek_preroll{0};
  bool has_default_duration{false};
  uint64_t default_duration{0};
  bool has_timecode_scale{false};
  double timecode_scale{1.0};
  bool has_video{false};
  bool has_pixel_width{false};
  uint64_t pixel_width{0};
  bool has_pixel_height{false};
  uint64_t pixel_height{0};
  bool has_content_encodings{false};
};

struct SourceMeta {
  std::shared_ptr<libebml::IOCallback> io;
  std::array<uint8_t, 16> segment_uid{};
  bool has_segment_uid{false};
  uint64_t timecode_scale{1000000};

  bool has_info{false};
  Range info;
  bool has_tracks{false};
  Range tracks_range;
  bool has_chapters{false};
  Range chapters_range;
  std::vector<Range> clusters;

  std::vector<TrackMeta> tracks;

  bool selected{false};
  uint64_t track_number{0};
  std::string codec_id;
  std::vector<uint8_t> codec_private;
  uint64_t pixel_width{0};
  uint64_t pixel_height{0};
  bool has_default_duration{false};
  uint64_t default_duration_ns{0};

  std::vector<FrameRef> frames;
};

struct RawChapter {
  size_t original_index{0};
  bool enabled{true};
  bool has_begin{false};
  bool has_end{false};
  uint64_t begin_ns{0};
  uint64_t end_ns{0};
  bool has_segment_uid{false};
  std::array<uint8_t, 16> segment_uid{};
};

struct ChapterRec {
  size_t original_index{0};
  size_t source_index{0};
  uint64_t begin_ns{0};
  uint64_t end_ns{0};
  uint64_t program_start_ns{0};
  size_t first_frame{0};
  size_t end_frame{0};  // exclusive

  uint64_t ProgramEndNS() const { return program_start_ns + (end_ns - begin_ns); }
};

} // namespace ordered_detail

class KaxOrderedReaderImpl {
public:
  std::vector<ordered_detail::SourceMeta> sources;
  std::vector<ordered_detail::ChapterRec> chapters;
  uint64_t duration_ns{0};

  // Logical play cursor.  Sequence number identifies the pending event so
  // failed IO can be retried without consuming the event.
  bool ended{false};
  size_t chapter_index{0};
  size_t frame_index{0};
  bool need_config{true};
  uint64_t clip_ns{0};
};

namespace ordered_detail {

/*!
  Scan every source, resolve the ordered edition and build the compact
  play index.  Returns false on any malformed or unsupported input.
*/
bool BuildOrderedIndex(const std::vector<std::shared_ptr<libebml::IOCallback>>& sources,
                       size_t root_source,
                       uint64_t edition_uid,
                       uint64_t track_uid,
                       KaxOrderedReaderImpl& impl);

} // namespace ordered_detail

} // namespace libmatroska

#endif // LIBMATROSKA_ORDERED_INTERNAL_H
