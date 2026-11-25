/****************************************************************************
** libmatroska : parse Matroska files, see http://www.matroska.org/
**
** Ordered-chapter index builder.  Pre-scans sources, resolves the ordered
** edition and records the compact per-frame play index.
**
** This file is part of libmatroska.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License as published by the Free Software Foundation; either
** version 2.1 of the License, or (at your option) any later version.
**
**********************************************************************/

#include "KaxOrderedInternal.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace libmatroska {
namespace ordered_detail {
namespace {

const uint64_t ID_SEGMENT = 0x18538067;

const uint64_t ID_INFO = 0x1549A966;
const uint64_t ID_TIMECODE_SCALE = 0x2AD7B1;
const uint64_t ID_SEGMENT_UID = 0x73A4;

const uint64_t ID_TRACKS = 0x1654AE6B;
const uint64_t ID_TRACK_ENTRY = 0xAE;
const uint64_t ID_TRACK_NUMBER = 0xD7;
const uint64_t ID_TRACK_UID = 0x73C5;
const uint64_t ID_TRACK_TYPE = 0x83;
const uint64_t ID_CODEC_ID = 0x86;
const uint64_t ID_CODEC_PRIVATE = 0x63A2;
const uint64_t ID_CODEC_DELAY = 0x56AA;
const uint64_t ID_SEEK_PRE_ROLL = 0x56BB;
const uint64_t ID_TRACK_DEFAULT_DURATION = 0x23E383;
const uint64_t ID_TRACK_TIMECODE_SCALE = 0x23314F;
const uint64_t ID_TRACK_VIDEO = 0xE0;
const uint64_t ID_VIDEO_PIXEL_WIDTH = 0xB0;
const uint64_t ID_VIDEO_PIXEL_HEIGHT = 0xBA;
const uint64_t ID_CONTENT_ENCODINGS = 0x6D80;

const uint64_t ID_CHAPTERS = 0x1043A770;
const uint64_t ID_EDITION_ENTRY = 0x45B9;
const uint64_t ID_EDITION_UID = 0x45BC;
const uint64_t ID_EDITION_FLAG_ORDERED = 0x45DD;
const uint64_t ID_CHAPTER_ATOM = 0xB6;
const uint64_t ID_CHAPTER_TIME_START = 0x91;
const uint64_t ID_CHAPTER_TIME_END = 0x92;
const uint64_t ID_CHAPTER_FLAG_ENABLED = 0x4598;
const uint64_t ID_CHAPTER_FLAG_HIDDEN = 0x98;
const uint64_t ID_CHAPTER_SEGMENT_UID = 0x6E67;
const uint64_t ID_CHAPTER_SEGMENT_EDITION_UID = 0x6EBC;
const uint64_t ID_CHAPTER_TRACK = 0x8F;
const uint64_t ID_CHAPTER_PROCESS = 0x6944;

const uint64_t ID_CLUSTER = 0x1F43B675;
const uint64_t ID_CLUSTER_TIMECODE = 0xE7;
const uint64_t ID_SIMPLE_BLOCK = 0xA3;
const uint64_t ID_BLOCK_GROUP = 0xA0;
const uint64_t ID_BLOCK = 0xA1;
const uint64_t ID_BLOCK_DURATION = 0x9B;
const uint64_t ID_REFERENCE_BLOCK = 0xFB;
const uint64_t ID_CODEC_STATE = 0xA4;
const uint64_t ID_DISCARD_PADDING = 0x75A2;

struct Elem {
  uint64_t id{0};
  uint64_t pos{0};
  uint64_t data_pos{0};
  uint64_t size{0};
};

bool ReadAt(libebml::IOCallback& io, uint64_t pos, void* buffer, size_t count)
{
  if (count == 0)
    return true;
  if (pos > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    return false;
  try {
    io.setFilePointer(static_cast<int64_t>(pos), libebml::seek_beginning);
  } catch (...) {
    return false;
  }
  auto* dst = static_cast<uint8_t*>(buffer);
  size_t done = 0;
  while (done < count) {
    uint32_t got = 0;
    try {
      got = io.read(dst + done, count - done);
    } catch (...) {
      return false;
    }
    if (got == 0)
      return false;
    done += got;
  }
  return true;
}

bool ReadByte(libebml::IOCallback& io, uint64_t pos, uint8_t& value)
{
  return ReadAt(io, pos, &value, 1);
}

bool ReadElementAt(libebml::IOCallback& io, uint64_t pos, uint64_t limit, Elem& elem)
{
  if (pos >= limit)
    return false;

  uint8_t first = 0;
  if (!ReadByte(io, pos, first) || first == 0) {
    return false;
  }

  unsigned id_len = 1;
  uint8_t mask = 0x80;
  while (id_len <= 4 && (first & mask) == 0) {
    ++id_len;
    mask >>= 1;
  }
  if (id_len > 4)
    return false;

  uint64_t id = first;
  for (unsigned i = 1; i < id_len; ++i) {
    uint8_t b = 0;
    if (!ReadByte(io, pos + i, b))
      return false;
    id = (id << 8) | b;
  }

  const uint64_t size_pos = pos + id_len;
  if (size_pos >= limit)
    return false;

  uint8_t size_first = 0;
  if (!ReadByte(io, size_pos, size_first) || size_first == 0)
    return false;

  unsigned size_len = 1;
  mask = 0x80;
  while (size_len <= 8 && (size_first & mask) == 0) {
    ++size_len;
    mask >>= 1;
  }
  if (size_len > 8)
    return false;

  uint64_t size = size_first & (mask - 1);
  for (unsigned i = 1; i < size_len; ++i) {
    uint8_t b = 0;
    if (!ReadByte(io, size_pos + i, b))
      return false;
    size = (size << 8) | b;
  }

  // All value bits set means "unknown size"; the supported inputs have
  // finite sizes only.
  const uint64_t all_ones = (size_len >= 8)
                                ? ((uint64_t(1) << 56) - 1)
                                : ((uint64_t(1) << (7 * size_len)) - 1);
  if (size == all_ones)
    return false;

  const uint64_t data_pos = size_pos + size_len;
  if (data_pos > limit)
    return false;
  if (size > limit - data_pos)
    return false;

  elem.id = id;
  elem.pos = pos;
  elem.data_pos = data_pos;
  elem.size = size;
  return true;
}

bool NextChild(libebml::IOCallback& io, uint64_t& pos, uint64_t end, Elem& elem)
{
  if (!ReadElementAt(io, pos, end, elem))
    return false;
  pos = elem.data_pos + elem.size;
  return true;
}

bool ReadUInt(libebml::IOCallback& io, const Elem& elem, uint64_t& value)
{
  if (elem.size > 8)
    return false;
  uint8_t buf[8] = {0};
  if (!ReadAt(io, elem.data_pos, buf, static_cast<size_t>(elem.size)))
    return false;
  uint64_t v = 0;
  for (uint64_t i = 0; i < elem.size; ++i)
    v = (v << 8) | buf[i];
  value = v;
  return true;
}

bool ReadSInt(libebml::IOCallback& io, const Elem& elem, int64_t& value)
{
  if (elem.size > 8)
    return false;
  uint8_t buf[8] = {0};
  if (!ReadAt(io, elem.data_pos, buf, static_cast<size_t>(elem.size)))
    return false;
  uint64_t v = 0;
  for (uint64_t i = 0; i < elem.size; ++i)
    v = (v << 8) | buf[i];
  if (elem.size > 0 && elem.size < 8 && (buf[0] & 0x80))
    v |= (~uint64_t(0)) << (8 * elem.size);
  value = static_cast<int64_t>(v);
  return true;
}

bool ReadFloat(libebml::IOCallback& io, const Elem& elem, double& value)
{
  uint8_t buf[8] = {0};
  if (elem.size == 0) {
    value = 0.0;
    return true;
  }
  if (elem.size != 4 && elem.size != 8)
    return false;
  if (!ReadAt(io, elem.data_pos, buf, static_cast<size_t>(elem.size)))
    return false;
  if (elem.size == 4) {
    uint32_t bits = (uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) |
                    (uint32_t(buf[2]) << 8) | uint32_t(buf[3]);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    value = static_cast<double>(f);
  } else {
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i)
      bits = (bits << 8) | buf[i];
    std::memcpy(&value, &bits, sizeof(value));
  }
  return true;
}

bool ReadBytes(libebml::IOCallback& io, const Elem& elem, std::vector<uint8_t>& out)
{
  out.resize(static_cast<size_t>(elem.size));
  if (elem.size == 0)
    return true;
  return ReadAt(io, elem.data_pos, out.data(), out.size());
}

bool ReadString(libebml::IOCallback& io, const Elem& elem, std::string& out)
{
  std::vector<uint8_t> bytes;
  if (!ReadBytes(io, elem, bytes))
    return false;
  out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

Range ToRange(const Elem& elem)
{
  Range r;
  r.data_pos = elem.data_pos;
  r.size = elem.size;
  r.end = elem.data_pos + elem.size;
  return r;
}

bool ParseInfo(libebml::IOCallback& io, const Range& range, SourceMeta& source)
{
  uint64_t pos = range.data_pos;
  bool saw_first_uid = false;
  while (pos < range.end) {
    Elem elem;
    if (!NextChild(io, pos, range.end, elem))
      return false;
    if (elem.id == ID_TIMECODE_SCALE) {
      uint64_t value = 0;
      if (!ReadUInt(io, elem, value))
        return false;
      source.timecode_scale = value;
    } else if (elem.id == ID_SEGMENT_UID) {
      if (elem.size != 16)
        return false;
      if (saw_first_uid)
        continue;
      uint8_t buf[16];
      if (!ReadAt(io, elem.data_pos, buf, sizeof(buf)))
        return false;
      std::memcpy(source.segment_uid.data(), buf, sizeof(buf));
      source.has_segment_uid = true;
      saw_first_uid = true;
    }
  }
  return source.has_segment_uid;
}

bool ParseTrack(libebml::IOCallback& io, const Elem& entry, TrackMeta& track)
{
  uint64_t pos = entry.data_pos;
  const uint64_t end = entry.data_pos + entry.size;
  while (pos < end) {
    Elem elem;
    if (!NextChild(io, pos, end, elem))
      return false;
    switch (elem.id) {
      case ID_TRACK_NUMBER:
        if (!ReadUInt(io, elem, track.number))
          return false;
        track.has_number = true;
        break;
      case ID_TRACK_UID:
        if (!ReadUInt(io, elem, track.uid))
          return false;
        track.has_uid = true;
        break;
      case ID_TRACK_TYPE:
        if (!ReadUInt(io, elem, track.type))
          return false;
        track.has_type = true;
        break;
      case ID_CODEC_ID:
        if (!ReadString(io, elem, track.codec_id))
          return false;
        track.has_codec_id = true;
        break;
      case ID_CODEC_PRIVATE:
        if (!ReadBytes(io, elem, track.codec_private))
          return false;
        track.has_codec_private = true;
        break;
      case ID_CODEC_DELAY:
        if (!ReadUInt(io, elem, track.codec_delay))
          return false;
        break;
      case ID_SEEK_PRE_ROLL:
        if (!ReadUInt(io, elem, track.seek_preroll))
          return false;
        break;
      case ID_TRACK_DEFAULT_DURATION:
        if (!ReadUInt(io, elem, track.default_duration))
          return false;
        track.has_default_duration = true;
        break;
      case ID_TRACK_TIMECODE_SCALE:
        if (!ReadFloat(io, elem, track.timecode_scale))
          return false;
        track.has_timecode_scale = true;
        break;
      case ID_CONTENT_ENCODINGS:
        track.has_content_encodings = true;
        break;
      case ID_TRACK_VIDEO: {
        track.has_video = true;
        uint64_t vpos = elem.data_pos;
        const uint64_t vend = elem.data_pos + elem.size;
        while (vpos < vend) {
          Elem child;
          if (!NextChild(io, vpos, vend, child))
            return false;
          if (child.id == ID_VIDEO_PIXEL_WIDTH) {
            if (!ReadUInt(io, child, track.pixel_width))
              return false;
            track.has_pixel_width = true;
          } else if (child.id == ID_VIDEO_PIXEL_HEIGHT) {
            if (!ReadUInt(io, child, track.pixel_height))
              return false;
            track.has_pixel_height = true;
          }
        }
        break;
      }
      default:
        break;
    }
  }
  return true;
}

bool ParseTracks(libebml::IOCallback& io, const Range& range, SourceMeta& source)
{
  uint64_t pos = range.data_pos;
  while (pos < range.end) {
    Elem elem;
    if (!NextChild(io, pos, range.end, elem))
      return false;
    if (elem.id == ID_TRACK_ENTRY) {
      TrackMeta track;
      if (!ParseTrack(io, elem, track))
        return false;
      source.tracks.push_back(std::move(track));
    }
  }
  return true;
}

bool ParseChapterAtom(libebml::IOCallback& io, const Elem& atom, size_t index,
                      RawChapter& chapter)
{
  chapter.original_index = index;
  chapter.enabled = true;

  uint64_t pos = atom.data_pos;
  const uint64_t end = atom.data_pos + atom.size;

  // A disabled chapter is skipped entirely, so only its enabled flag matters.
  while (pos < end) {
    Elem elem;
    if (!NextChild(io, pos, end, elem))
      return false;
    if (elem.id == ID_CHAPTER_FLAG_ENABLED) {
      uint64_t value = 0;
      if (!ReadUInt(io, elem, value))
        return false;
      chapter.enabled = value != 0;
    }
  }
  if (!chapter.enabled)
    return true;

  pos = atom.data_pos;
  bool saw_segment_uid = false;
  while (pos < end) {
    Elem elem;
    if (!NextChild(io, pos, end, elem))
      return false;
    switch (elem.id) {
      case ID_CHAPTER_ATOM:
      case ID_CHAPTER_TRACK:
      case ID_CHAPTER_PROCESS:
      case ID_CHAPTER_SEGMENT_EDITION_UID:
        return false;
      case ID_CHAPTER_TIME_START:
        if (!ReadUInt(io, elem, chapter.begin_ns))
          return false;
        chapter.has_begin = true;
        break;
      case ID_CHAPTER_TIME_END:
        if (!ReadUInt(io, elem, chapter.end_ns))
          return false;
        chapter.has_end = true;
        break;
      case ID_CHAPTER_FLAG_ENABLED:
        break;
      case ID_CHAPTER_FLAG_HIDDEN:
        break;
      case ID_CHAPTER_SEGMENT_UID: {
        if (elem.size != 16)
          return false;
        uint8_t buf[16];
        if (!ReadAt(io, elem.data_pos, buf, sizeof(buf)))
          return false;
        if (!saw_segment_uid) {
          std::memcpy(chapter.segment_uid.data(), buf, sizeof(buf));
          chapter.has_segment_uid = true;
          saw_segment_uid = true;
        }
        break;
      }
      default:
        break;
    }
  }
  return true;
}

bool ParseChapters(libebml::IOCallback& io, const Range& range, uint64_t edition_uid,
                   std::vector<RawChapter>& chapters)
{
  uint64_t pos = range.data_pos;
  bool found = false;
  while (pos < range.end) {
    Elem entry;
    if (!NextChild(io, pos, range.end, entry))
      return false;
    if (entry.id != ID_EDITION_ENTRY)
      continue;

    uint64_t ed_pos = entry.data_pos;
    const uint64_t ed_end = entry.data_pos + entry.size;
    bool has_uid = false;
    uint64_t uid = 0;
    bool ordered = false;
    std::vector<Elem> atoms;
    uint64_t scan = ed_pos;
    while (scan < ed_end) {
      Elem child;
      if (!NextChild(io, scan, ed_end, child))
        return false;
      if (child.id == ID_EDITION_UID) {
        if (!ReadUInt(io, child, uid))
          return false;
        has_uid = true;
      } else if (child.id == ID_EDITION_FLAG_ORDERED) {
        uint64_t value = 0;
        if (!ReadUInt(io, child, value))
          return false;
        ordered = value == 1;
      } else if (child.id == ID_CHAPTER_ATOM) {
        atoms.push_back(child);
      }
    }
    if (!has_uid || uid != edition_uid)
      continue;
    if (found)
      return false;  // duplicate EditionUID
    found = true;
    if (!ordered)
      return false;

    chapters.clear();
    size_t index = 0;
    for (const auto& atom : atoms) {
      RawChapter chapter;
      if (!ParseChapterAtom(io, atom, index, chapter))
        return false;
      ++index;
      if (!chapter.enabled)
        continue;
      if (!chapter.has_begin || !chapter.has_end)
        return false;
      if (!(chapter.begin_ns < chapter.end_ns))
        return false;
      chapters.push_back(chapter);
    }
  }
  return found;
}

bool GetFileStructure(libebml::IOCallback& io, SourceMeta& source)
{
  const uint64_t unlimited = std::numeric_limits<uint64_t>::max();
  uint64_t pos = 0;
  bool saw_segment = false;
  Elem segment;

  for (int guard = 0; guard < 16; ++guard) {
    Elem elem;
    if (!ReadElementAt(io, pos, unlimited, elem))
      return false;
    if (elem.id == ID_SEGMENT) {
      segment = elem;
      saw_segment = true;
      break;
    }
    if (elem.size > std::numeric_limits<uint64_t>::max() - elem.data_pos)
      return false;
    pos = elem.data_pos + elem.size;
  }
  if (!saw_segment)
    return false;

  if (segment.size > std::numeric_limits<uint64_t>::max() - segment.data_pos)
    return false;
  const uint64_t segment_end = segment.data_pos + segment.size;

  // Exactly one Segment per input.
  {
    uint64_t after = segment_end;
    while (after < unlimited) {
      Elem extra;
      if (!ReadElementAt(io, after, unlimited, extra))
        break;
      if (extra.id == ID_SEGMENT)
        return false;
      if (extra.size > std::numeric_limits<uint64_t>::max() - extra.data_pos)
        break;
      after = extra.data_pos + extra.size;
    }
  }

  pos = segment.data_pos;
  while (pos < segment_end) {
    Elem elem;
    if (!ReadElementAt(io, pos, segment_end, elem))
      return false;
    switch (elem.id) {
      case ID_INFO:
        if (!source.has_info) {
          source.info = ToRange(elem);
          source.has_info = true;
        }
        break;
      case ID_TRACKS:
        if (!source.has_tracks) {
          source.tracks_range = ToRange(elem);
          source.has_tracks = true;
        }
        break;
      case ID_CHAPTERS:
        if (!source.has_chapters) {
          source.chapters_range = ToRange(elem);
          source.has_chapters = true;
        }
        break;
      case ID_CLUSTER:
        source.clusters.push_back(ToRange(elem));
        break;
      default:
        break;
    }
    pos = elem.data_pos + elem.size;
  }
  return source.has_info && ParseInfo(io, source.info, source);
}

struct BlockHead {
  uint64_t track_number{0};
  int16_t local_timecode{0};
  uint8_t flags{0};
  uint64_t header_size{0};
};

bool ReadBlockHead(libebml::IOCallback& io, uint64_t data_pos, uint64_t size, BlockHead& head)
{
  if (size < 4)
    return false;
  if (data_pos > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    return false;
  try {
    io.setFilePointer(static_cast<int64_t>(data_pos), libebml::seek_beginning);
  } catch (...) {
    return false;
  }

  auto read_one = [&io](uint8_t& b) -> bool {
    try {
      return io.read(&b, 1) == 1;
    } catch (...) {
      return false;
    }
  };

  uint8_t b0 = 0;
  if (!read_one(b0))
    return false;

  uint64_t consumed = 0;
  if (b0 & 0x80) {
    head.track_number = b0 & 0x7F;
    consumed = 1;
  } else if (b0 & 0x40) {
    uint8_t b1 = 0;
    if (!read_one(b1))
      return false;
    head.track_number = (uint64_t(b0 & 0x3F) << 8) | b1;
    consumed = 2;
  } else {
    return false;
  }

  uint8_t tc_hi = 0, tc_lo = 0, flags = 0;
  if (!read_one(tc_hi) || !read_one(tc_lo) || !read_one(flags))
    return false;

  const uint16_t raw_tc = (uint16_t(tc_hi) << 8) | tc_lo;
  head.local_timecode = static_cast<int16_t>(raw_tc);
  head.flags = flags;
  head.header_size = consumed + 3;
  if (head.header_size > size)
    return false;
  return true;
}

bool MultiplyChecked(uint64_t a, uint64_t b, uint64_t& out)
{
  if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)
    return false;
  out = a * b;
  return true;
}

bool BlockTimeToNS(uint64_t cluster_timecode, int16_t local, uint64_t scale, uint64_t& ns)
{
  if (cluster_timecode > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    return false;
  uint64_t units = 0;
  if (local >= 0) {
    const uint64_t bump = static_cast<uint64_t>(local);
    if (cluster_timecode > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - bump)
      return false;
    units = cluster_timecode + bump;
  } else {
    const uint64_t drop = static_cast<uint64_t>(-static_cast<int64_t>(local));
    if (drop > cluster_timecode)
      return false;
    units = cluster_timecode - drop;
  }
  return MultiplyChecked(units, scale, ns);
}

struct GroupInfo {
  bool has_block{false};
  Elem block;
  bool has_duration{false};
  uint64_t duration{0};
  bool has_reference{false};
  bool has_codec_state{false};
  bool has_discard_padding{false};
  int64_t discard_padding{0};
};

bool ParseBlockGroup(libebml::IOCallback& io, const Elem& group, GroupInfo& info)
{
  uint64_t pos = group.data_pos;
  const uint64_t end = group.data_pos + group.size;
  while (pos < end) {
    Elem elem;
    if (!NextChild(io, pos, end, elem))
      return false;
    switch (elem.id) {
      case ID_BLOCK:
        if (!info.has_block) {
          info.block = elem;
          info.has_block = true;
        }
        break;
      case ID_BLOCK_DURATION:
        if (!ReadUInt(io, elem, info.duration))
          return false;
        info.has_duration = true;
        break;
      case ID_REFERENCE_BLOCK:
        info.has_reference = true;
        break;
      case ID_CODEC_STATE:
        info.has_codec_state = true;
        break;
      case ID_DISCARD_PADDING:
        if (!ReadSInt(io, elem, info.discard_padding))
          return false;
        info.has_discard_padding = true;
        break;
      default:
        break;
    }
  }
  return true;
}

bool AppendFrame(SourceMeta& source, const FrameRef& frame);

bool ScanClusters(libebml::IOCallback& io, SourceMeta& source)
{
  for (const auto& cluster : source.clusters) {
    bool has_timecode = false;
    uint64_t cluster_timecode = 0;
    std::vector<Elem> blocks;

    uint64_t pos = cluster.data_pos;
    while (pos < cluster.end) {
      Elem elem;
      if (!NextChild(io, pos, cluster.end, elem))
        return false;
      if (elem.id == ID_CLUSTER_TIMECODE) {
        if (!has_timecode) {
          if (!ReadUInt(io, elem, cluster_timecode))
            return false;
          has_timecode = true;
        }
      } else if (elem.id == ID_SIMPLE_BLOCK || elem.id == ID_BLOCK_GROUP) {
        blocks.push_back(elem);
      }
    }
    if (!has_timecode)
      return false;

    for (const auto& block_elem : blocks) {
      if (block_elem.id == ID_SIMPLE_BLOCK) {
        BlockHead head;
        if (!ReadBlockHead(io, block_elem.data_pos, block_elem.size, head))
          return false;
        if (head.track_number != source.track_number)
          continue;
        if (((head.flags >> 1) & 0x03) != 0)
          return false;  // lacing unsupported

        uint64_t ns = 0;
        if (!BlockTimeToNS(cluster_timecode, head.local_timecode, source.timecode_scale, ns))
          return false;

        uint64_t duration = 0;
        if (!source.has_default_duration || source.default_duration_ns == 0)
          return false;
        duration = source.default_duration_ns;

        FrameRef frame;
        frame.payload_offset = block_elem.data_pos + head.header_size;
        frame.payload_size = block_elem.size - head.header_size;
        frame.source_ns = ns;
        frame.duration_ns = duration;
        frame.keyframe = (head.flags & 0x80) != 0;
        if (!AppendFrame(source, frame))
          return false;
      } else {
        GroupInfo info;
        if (!ParseBlockGroup(io, block_elem, info))
          return false;
        if (!info.has_block)
          continue;

        BlockHead head;
        if (!ReadBlockHead(io, info.block.data_pos, info.block.size, head))
          return false;
        if (head.track_number != source.track_number)
          continue;
        if (((head.flags >> 1) & 0x03) != 0)
          return false;  // lacing unsupported
        if (info.has_codec_state)
          return false;
        if (info.has_discard_padding && info.discard_padding != 0)
          return false;

        uint64_t ns = 0;
        if (!BlockTimeToNS(cluster_timecode, head.local_timecode, source.timecode_scale, ns))
          return false;

        uint64_t duration = 0;
        if (info.has_duration) {
          if (!MultiplyChecked(info.duration, source.timecode_scale, duration) ||
              duration == 0)
            return false;
        } else if (source.has_default_duration && source.default_duration_ns > 0) {
          duration = source.default_duration_ns;
        } else {
          return false;
        }

        FrameRef frame;
        frame.payload_offset = info.block.data_pos + head.header_size;
        frame.payload_size = info.block.size - head.header_size;
        frame.source_ns = ns;
        frame.duration_ns = duration;
        frame.keyframe = !info.has_reference;
        if (!AppendFrame(source, frame))
          return false;
      }
    }
  }
  return true;
}

bool AppendFrame(SourceMeta& source, const FrameRef& frame)
{
  if (frame.duration_ns == 0)
    return false;
  if (frame.source_ns > std::numeric_limits<uint64_t>::max() - frame.duration_ns)
    return false;
  if (!source.frames.empty()) {
    const FrameRef& prev = source.frames.back();
    if (!(frame.source_ns > prev.source_ns))
      return false;
    if (prev.source_ns + prev.duration_ns != frame.source_ns)
      return false;
  }
  source.frames.push_back(frame);
  return true;
}

bool ResolveSelectedTrack(SourceMeta& source, uint64_t track_uid)
{
  const TrackMeta* selected = nullptr;
  size_t matches = 0;
  for (const auto& track : source.tracks) {
    if (track.has_uid && track.uid == track_uid && track.has_type && track.type == 1) {
      ++matches;
      selected = &track;
    }
  }
  if (matches != 1 || selected == nullptr)
    return false;

  const TrackMeta& t = *selected;
  if (!t.has_number || !t.has_codec_id || !t.has_video ||
      !t.has_pixel_width || !t.has_pixel_height)
    return false;
  if (t.has_timecode_scale && t.timecode_scale != 1.0)
    return false;
  if (t.codec_delay != 0 || t.seek_preroll != 0)
    return false;
  if (t.has_content_encodings)
    return false;

  source.selected = true;
  source.track_number = t.number;
  source.codec_id = t.codec_id;
  source.codec_private = t.codec_private;
  source.pixel_width = t.pixel_width;
  source.pixel_height = t.pixel_height;
  source.has_default_duration = t.has_default_duration;
  source.default_duration_ns = t.default_duration;
  return true;
}

} // namespace

bool BuildOrderedIndex(const std::vector<std::shared_ptr<libebml::IOCallback>>& sources,
                       size_t root_source,
                       uint64_t edition_uid,
                       uint64_t track_uid,
                       KaxOrderedReaderImpl& impl)
{
  if (sources.empty() || root_source >= sources.size())
    return false;

  impl.sources.clear();
  impl.sources.resize(sources.size());
  impl.chapters.clear();
  impl.duration_ns = 0;

  for (size_t i = 0; i < sources.size(); ++i) {
    if (!sources[i])
      return false;
    impl.sources[i].io = sources[i];
    if (!GetFileStructure(*sources[i], impl.sources[i]))
      return false;
  }

  // SegmentUIDs must be present, 16 bytes and unique across the source set.
  for (size_t i = 0; i < impl.sources.size(); ++i) {
    if (!impl.sources[i].has_segment_uid)
      return false;
    for (size_t j = 0; j < i; ++j) {
      if (impl.sources[j].segment_uid == impl.sources[i].segment_uid)
        return false;
    }
  }

  if (!impl.sources[root_source].has_chapters)
    return false;

  std::vector<RawChapter> raw_chapters;
  if (!ParseChapters(*impl.sources[root_source].io,
                     impl.sources[root_source].chapters_range,
                     edition_uid, raw_chapters))
    return false;

  auto uid_to_source = [&impl](const std::array<uint8_t, 16>& uid, size_t& index) -> bool {
    size_t found = 0;
    for (size_t i = 0; i < impl.sources.size(); ++i) {
      if (impl.sources[i].segment_uid == uid) {
        index = i;
        ++found;
      }
    }
    return found == 1;
  };

  std::vector<size_t> required;
  std::vector<size_t> chapter_sources;
  chapter_sources.reserve(raw_chapters.size());
  for (const auto& raw : raw_chapters) {
    size_t source_index = root_source;
    if (raw.has_segment_uid && !uid_to_source(raw.segment_uid, source_index))
      return false;
    chapter_sources.push_back(source_index);
    if (std::find(required.begin(), required.end(), source_index) == required.end())
      required.push_back(source_index);
  }

  // Resolve tracks and build frame indexes for every referenced source.
  for (size_t index : required) {
    SourceMeta& source = impl.sources[index];
    if (!source.has_tracks)
      return false;
    if (!ParseTracks(*source.io, source.tracks_range, source)) {
      return false;
    }
    if (!ResolveSelectedTrack(source, track_uid)) {
      return false;
    }
    if (!ScanClusters(*source.io, source)) {
      return false;
    }
  }

  // Codec and picture dimensions must agree across referenced sources.
  bool have_reference = false;
  std::string codec_id;
  uint64_t width = 0, height = 0;
  for (size_t index : required) {
    const SourceMeta& source = impl.sources[index];
    if (!have_reference) {
      have_reference = true;
      codec_id = source.codec_id;
      width = source.pixel_width;
      height = source.pixel_height;
    } else if (source.codec_id != codec_id || source.pixel_width != width ||
               source.pixel_height != height) {
      return false;
    }
  }

  uint64_t program = 0;
  for (size_t i = 0; i < raw_chapters.size(); ++i) {
    const RawChapter& raw = raw_chapters[i];
    const size_t source_index = chapter_sources[i];
    SourceMeta& source = impl.sources[source_index];

    if (source.frames.empty())
      return false;
    // The chapter must begin exactly on a keyframe: a frame whose source
    // time equals the chapter begin.
    auto begin_it = std::lower_bound(
        source.frames.begin(), source.frames.end(), raw.begin_ns,
        [](const FrameRef& frame, uint64_t value) { return frame.source_ns < value; });
    if (begin_it == source.frames.end() || begin_it->source_ns != raw.begin_ns ||
        !begin_it->keyframe)
      return false;
    const size_t first_frame = static_cast<size_t>(begin_it - source.frames.begin());

    const uint64_t track_end =
        source.frames.back().source_ns + source.frames.back().duration_ns;
    if (raw.end_ns > track_end)
      return false;

    auto end_it = std::lower_bound(
        source.frames.begin() + first_frame, source.frames.end(), raw.end_ns,
        [](const FrameRef& frame, uint64_t value) { return frame.source_ns < value; });
    const size_t end_frame = static_cast<size_t>(end_it - source.frames.begin());
    if (end_frame <= first_frame)
      return false;

    const uint64_t chapter_duration = raw.end_ns - raw.begin_ns;
    if (program > std::numeric_limits<uint64_t>::max() - chapter_duration)
      return false;

    ChapterRec rec;
    rec.original_index = raw.original_index;
    rec.source_index = source_index;
    rec.begin_ns = raw.begin_ns;
    rec.end_ns = raw.end_ns;
    rec.program_start_ns = program;
    rec.first_frame = first_frame;
    rec.end_frame = end_frame;
    impl.chapters.push_back(rec);

    program += chapter_duration;
  }

  impl.duration_ns = program;
  return true;
}

} // namespace ordered_detail
} // namespace libmatroska
