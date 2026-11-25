#ifndef ORDERED_TEST_FIXTURE_H
#define ORDERED_TEST_FIXTURE_H
// Shared fixture writer for test/ordered_reader.cpp.
//
// Every small file is produced with the legacy libmatroska/libebml writer API
// (KaxSegment / KaxTracks / KaxCluster / KaxChapters) and then written to disk
// and reopened, so the tests exercise real Matroska bytes rather than
// hand-assembled element streams.

#include <matroska/KaxSegment.h>
#include <matroska/KaxInfo.h>
#include <matroska/KaxInfoData.h>
#include <matroska/KaxChapters.h>
#include <matroska/KaxTracks.h>
#include <matroska/KaxTrackVideo.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxBlock.h>
#include <matroska/KaxBlockData.h>
#include <ebml/EbmlHead.h>
#include <ebml/EbmlSubHead.h>
#include <ebml/MemIOCallback.h>
#include <ebml/StdIOCallback.h>
#include <ebml/IOCallback.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ordered_test {

using libebml::IOCallback;
using libebml::MemIOCallback;
using libebml::StdIOCallback;
using namespace libmatroska;

using Bytes = std::vector<uint8_t>;

struct TrackSpec {
  uint64_t number = 1;
  uint64_t uid = 1;
  std::string codec_id = "V_THEORA";
  Bytes codec_private;
  uint64_t default_duration_ns = 0;  // 0 = omit
  uint64_t pixel_width = 32;
  uint64_t pixel_height = 32;
  bool content_encodings = false;
  double timecode_scale = 1.0;
  uint64_t codec_delay = 0;
  uint64_t seek_preroll = 0;
  bool video = true;
};

struct FrameSpec {
  bool keyframe = true;
  uint64_t source_ns = 0;
  Bytes payload;
  size_t track_index = 0;
  bool block_group = false;
  bool has_duration = false;
  uint64_t duration = 0;  // BlockDuration in raw timecode units
  bool has_reference = false;
  int64_t reference_timecode = 0;
  bool codec_state = false;
  int64_t discard_padding = 0;
  int force_simple = -1;  // -1 = per source, 0 = SimpleBlock, 1 = BlockGroup
};

struct ChapterSpec {
  uint64_t uid = 1;
  uint64_t begin_ns = 0;
  uint64_t end_ns = 0;
  bool enabled = true;
  int hidden = -1;  // -1 = omit
  bool has_segment_uid = false;
  std::array<uint8_t, 16> segment_uid{};
  bool has_segment_edition_uid = false;
  bool has_track = false;
  bool has_process = false;
  bool has_nested = false;
};

struct SourceSpec {
  std::array<uint8_t, 16> segment_uid{};
  uint64_t timecode_scale = 1000000;
  // Optional single cluster at an explicit raw timecode.  Used to exercise
  // negative local block timecodes.
  bool has_cluster_origin = false;
  uint64_t cluster_origin = 0;
  std::vector<TrackSpec> tracks;
  std::vector<FrameSpec> frames;
  // Cluster layout: (raw cluster timecode, frame indices).  Empty means one
  // cluster at timecode 0.
  std::vector<std::pair<uint64_t, std::vector<size_t>>> clusters;
  bool use_block_group = false;  // force every frame to a BlockGroup
};

struct EditionSpec {
  uint64_t uid = 1;
  bool ordered = true;
  std::vector<ChapterSpec> chapters;
};

struct FileSpec {
  SourceSpec source;
  bool has_chapters = false;
  std::vector<EditionSpec> editions;
  // Top-level ordering controls.  Metadata may follow the clusters.
  bool tracks_after_cluster = false;
  bool chapters_after_cluster = false;
};

namespace detail {

inline void FillInfo(KaxSegment& segment, const SourceSpec& source)
{
  KaxInfo& info = GetChild<KaxInfo>(segment);
  GetChild<KaxTimecodeScale>(info).SetValue(source.timecode_scale);
  GetChild<KaxSegmentUID>(info).CopyBuffer(source.segment_uid.data(), 16);
  GetChild<KaxMuxingApp>(info).SetValueUTF8("ordered_test");
  GetChild<KaxWritingApp>(info).SetValueUTF8("ordered_test");
}

inline void FillTrack(KaxTrackEntry& entry, const TrackSpec& track,
                      uint64_t timecode_scale)
{
  GetChild<KaxTrackNumber>(entry).SetValue(track.number);
  GetChild<KaxTrackUID>(entry).SetValue(track.uid);
  GetChild<KaxTrackType>(entry).SetValue(track.video ? 1 : 2);
  entry.EnableLacing(false);
  GetChild<KaxCodecID>(entry).SetValue(track.codec_id.c_str());
  if (!track.codec_private.empty())
    GetChild<KaxCodecPrivate>(entry).CopyBuffer(
        track.codec_private.data(),
        static_cast<uint32>(track.codec_private.size()));
  if (track.default_duration_ns != 0)
    GetChild<KaxTrackDefaultDuration>(entry).SetValue(track.default_duration_ns);
  if (track.codec_delay != 0)
    GetChild<KaxCodecDelay>(entry).SetValue(track.codec_delay);
  if (track.seek_preroll != 0)
    GetChild<KaxSeekPreRoll>(entry).SetValue(track.seek_preroll);
  if (track.content_encodings) {
    KaxContentEncodings& encodings = GetChild<KaxContentEncodings>(entry);
    AddNewChild<KaxContentEncoding>(encodings);
  }
  if (track.video) {
    KaxTrackVideo& video = GetChild<KaxTrackVideo>(entry);
    GetChild<KaxVideoPixelWidth>(video).SetValue(track.pixel_width);
    GetChild<KaxVideoPixelHeight>(video).SetValue(track.pixel_height);
  }
  entry.SetGlobalTimecodeScale(timecode_scale);
}

inline void FillTracks(KaxTracks& tracks, const SourceSpec& source)
{
  for (const auto& track : source.tracks) {
    KaxTrackEntry& entry = AddNewChild<KaxTrackEntry>(tracks);
    FillTrack(entry, track, source.timecode_scale);
  }
}

inline void AddBlock(KaxCluster& cluster, KaxTrackEntry& track,
                     const FrameSpec& frame, uint64_t scale)
{
  const bool group = frame.force_simple == 0
                         ? false
                         : (frame.force_simple == 1 || frame.block_group);
  // The owning element deletes the DataBuffer in its destructor, so it must
  // outlive this call (heap allocation, as the legacy API expects).
  auto* buffer = new DataBuffer(const_cast<uint8_t*>(frame.payload.data()),
                                static_cast<uint32>(frame.payload.size()),
                                nullptr, true);

  if (!group) {
    KaxSimpleBlock& block = AddNewChild<KaxSimpleBlock>(cluster);
    block.SetParent(cluster);
    block.AddFrame(track, frame.source_ns, *buffer, LACING_NONE);
    block.SetKeyframe(frame.keyframe);
    return;
  }

  KaxBlockGroup* created = nullptr;
  cluster.AddFrame(track, frame.source_ns, *buffer, created, LACING_NONE);
  if (created == nullptr)
    throw std::runtime_error("failed to create BlockGroup");
  if (frame.has_duration)
    created->SetBlockDuration(frame.duration * scale);
  if (frame.has_reference)
    GetChild<KaxReferenceBlock>(*created).SetReferencedTimecode(
        frame.reference_timecode);
  if (frame.codec_state) {
    const uint8_t state[3] = {1, 2, 3};
    GetChild<KaxCodecState>(*created).CopyBuffer(state, sizeof(state));
  }
  if (frame.discard_padding != 0)
    GetChild<KaxDiscardPadding>(*created).SetValue(frame.discard_padding);
}

inline void FillCluster(KaxSegment& segment, const SourceSpec& source,
                        KaxTracks& tracks, uint64_t cluster_tc,
                        const std::vector<size_t>& indices)
{
  KaxCluster& cluster = AddNewChild<KaxCluster>(segment);
  cluster.InitTimecode(cluster_tc, static_cast<int64>(source.timecode_scale));
  GetChild<KaxClusterTimecode>(cluster).SetValue(cluster_tc);
  for (size_t index : indices) {
    if (index >= source.frames.size())
      throw std::runtime_error("cluster frame index out of range");
    const FrameSpec& frame = source.frames[index];
    if (frame.track_index >= source.tracks.size())
      throw std::runtime_error("frame track index out of range");

    KaxTrackEntry* track = nullptr;
    size_t seen = 0;
    for (auto* element : tracks) {
      if (EbmlId(*element) != EBML_ID(KaxTrackEntry))
        continue;
      if (seen++ == frame.track_index) {
        track = static_cast<KaxTrackEntry*>(element);
        break;
      }
    }
    if (track == nullptr)
      throw std::runtime_error("track entry lookup failed");
    AddBlock(cluster, *track, frame, source.timecode_scale);
  }
}

inline void FillClusters(KaxSegment& segment, const SourceSpec& source,
                         KaxTracks& tracks)
{
  if (source.has_cluster_origin) {
    std::vector<size_t> all;
    for (size_t i = 0; i < source.frames.size(); ++i)
      all.push_back(i);
    FillCluster(segment, source, tracks, source.cluster_origin, all);
  } else if (!source.clusters.empty()) {
    for (const auto& cluster : source.clusters)
      FillCluster(segment, source, tracks, cluster.first, cluster.second);
  } else {
    std::vector<size_t> all;
    for (size_t i = 0; i < source.frames.size(); ++i)
      all.push_back(i);
    FillCluster(segment, source, tracks, 0, all);
  }
}

inline void FillChapterAtom(KaxChapterAtom& atom, const ChapterSpec& chapter)
{
  GetChild<KaxChapterUID>(atom).SetValue(chapter.uid);
  GetChild<KaxChapterTimeStart>(atom).SetValue(chapter.begin_ns);
  GetChild<KaxChapterTimeEnd>(atom).SetValue(chapter.end_ns);
  if (chapter.hidden >= 0)
    GetChild<KaxChapterFlagHidden>(atom).SetValue(
        static_cast<uint64>(chapter.hidden));
  if (!chapter.enabled)
    GetChild<KaxChapterFlagEnabled>(atom).SetValue(0);
  if (chapter.has_segment_uid)
    GetChild<KaxChapterSegmentUID>(atom).CopyBuffer(chapter.segment_uid.data(), 16);
  if (chapter.has_segment_edition_uid)
    GetChild<KaxChapterSegmentEditionUID>(atom).SetValue(7);
  if (chapter.has_track) {
    KaxChapterTrack& track = GetChild<KaxChapterTrack>(atom);
    GetChild<KaxChapterTrackNumber>(track).SetValue(1);
  }
  if (chapter.has_process)
    GetChild<KaxChapterProcess>(atom);
  if (chapter.has_nested) {
    KaxChapterAtom& nested = GetChild<KaxChapterAtom>(atom);
    GetChild<KaxChapterUID>(nested).SetValue(999);
    GetChild<KaxChapterTimeStart>(nested).SetValue(0);
  }
}

inline void FillChapters(KaxChapters& chapters, const FileSpec& spec)
{
  bool first = true;
  for (const auto& edition : spec.editions) {
    KaxEditionEntry& entry = first ? GetChild<KaxEditionEntry>(chapters)
                                   : AddNewChild<KaxEditionEntry>(chapters);
    first = false;
    GetChild<KaxEditionUID>(entry).SetValue(edition.uid);
    GetChild<KaxEditionFlagOrdered>(entry).SetValue(edition.ordered ? 1 : 0);
    for (const auto& chapter : edition.chapters) {
      KaxChapterAtom& atom = AddNewChild<KaxChapterAtom>(entry);
      FillChapterAtom(atom, chapter);
    }
  }
}

}  // namespace detail

// Write a file through the legacy writer API and return its bytes.
inline Bytes BuildFile(const FileSpec& spec)
{
  MemIOCallback out;

  libebml::EbmlHead head;
  GetChild<libebml::EDocType>(head).SetValue("matroska");
  GetChild<libebml::EDocTypeVersion>(head).SetValue(4);
  GetChild<libebml::EDocTypeReadVersion>(head).SetValue(2);
  head.Render(out);

  KaxSegment segment;
  segment.SetSizeInfinite(false);
  detail::FillInfo(segment, spec.source);

  std::unique_ptr<KaxTracks> late_tracks;
  KaxTracks* tracks = nullptr;
  if (spec.tracks_after_cluster) {
    late_tracks.reset(new KaxTracks());
    tracks = late_tracks.get();
  } else {
    tracks = &GetChild<KaxTracks>(segment);
  }
  detail::FillTracks(*tracks, spec.source);

  std::unique_ptr<KaxChapters> late_chapters;
  if (spec.has_chapters && !spec.chapters_after_cluster)
    detail::FillChapters(GetChild<KaxChapters>(segment), spec);

  detail::FillClusters(segment, spec.source, *tracks);

  if (late_tracks)
    segment.PushElement(*late_tracks.release());

  if (spec.has_chapters) {
    if (spec.chapters_after_cluster) {
      late_chapters.reset(new KaxChapters());
      detail::FillChapters(*late_chapters, spec);
      segment.PushElement(*late_chapters.release());
    }
  }

  segment.Render(out);

  const auto* begin = out.GetDataBuffer();
  const auto size = static_cast<size_t>(out.GetDataBufferSize());
  return Bytes(begin, begin + size);
}

// ---- Minimal EBML byte-level surgery -------------------------------------
//
// A few rejection paths (e.g. a non-1 TrackTimecodeScale) cannot be produced
// through the legacy writer because the corresponding element refuses to
// render.  These helpers re-encode an element tree just enough to inject a
// child element into a TrackEntry.

namespace raw {

inline size_t VintWidth(uint64_t value)
{
  size_t width = 1;
  while (width < 8 && value >= (uint64_t(1) << (7 * width)))
    ++width;
  return width;
}

inline void PutId(Bytes& out, uint64_t id)
{
  size_t width = 1;
  while (width < 4 && (id >> (8 * width)) != 0)
    ++width;
  for (size_t i = 0; i < width; ++i)
    out.push_back(static_cast<uint8_t>((id >> (8 * (width - 1 - i))) & 0xFF));
}

inline void PutVint(Bytes& out, uint64_t value)
{
  const size_t width = VintWidth(value);
  for (size_t i = 0; i < width; ++i) {
    uint8_t byte = static_cast<uint8_t>((value >> (8 * (width - 1 - i))) & 0xFF);
    if (i == 0)
      byte |= static_cast<uint8_t>(0x80 >> (width - 1));
    out.push_back(byte);
  }
}

inline Bytes Element(uint64_t id, const Bytes& payload)
{
  Bytes out;
  PutId(out, id);
  PutVint(out, payload.size());
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

inline Bytes UIntElement(uint64_t id, uint64_t value)
{
  Bytes payload;
  if (value == 0) {
    payload.push_back(0);
  } else {
    uint8_t buf[8];
    size_t n = 0;
    while (value != 0) {
      buf[n++] = static_cast<uint8_t>(value & 0xFF);
      value >>= 8;
    }
    for (size_t i = 0; i < n; ++i)
      payload.push_back(buf[n - 1 - i]);
  }
  return Element(id, payload);
}

struct RawElem {
  uint64_t id = 0;
  uint64_t data = 0;  // offset of payload in buffer
  uint64_t size = 0;
  uint64_t end = 0;
};

inline bool ParseElem(const Bytes& buffer, size_t pos, size_t limit, RawElem& elem)
{
  if (pos >= limit || buffer[pos] == 0)
    return false;
  size_t id_len = 1;
  uint8_t mask = 0x80;
  while (id_len <= 4 && (buffer[pos] & mask) == 0) {
    ++id_len;
    mask >>= 1;
  }
  if (id_len > 4 || pos + id_len >= limit)
    return false;
  uint64_t id = buffer[pos];
  for (size_t i = 1; i < id_len; ++i)
    id = (id << 8) | buffer[pos + i];

  const size_t size_pos = pos + id_len;
  uint8_t first = buffer[size_pos];
  if (first == 0)
    return false;
  size_t size_len = 1;
  mask = 0x80;
  while (size_len <= 8 && (first & mask) == 0) {
    ++size_len;
    mask >>= 1;
  }
  if (size_len > 8 || size_pos + size_len > limit)
    return false;
  uint64_t size = first & (mask - 1);
  for (size_t i = 1; i < size_len; ++i)
    size = (size << 8) | buffer[size_pos + i];

  const uint64_t data = size_pos + size_len;
  if (data > limit || size > limit - data)
    return false;
  elem.id = id;
  elem.data = data;
  elem.size = size;
  elem.end = data + size;
  return true;
}

inline Bytes FloatElement(uint64_t id, double value)
{
  Bytes be(8);
  std::memcpy(be.data(), &value, 8);
  std::reverse(be.begin(), be.end());
  return Element(id, be);
}

// Append an extra child (id/payload) to the first TrackEntry of the file,
// re-encoding the enclosing TrackEntry/Tracks/Segment sizes.
inline Bytes AppendToFirstTrackEntry(const Bytes& file, uint64_t id,
                                     const Bytes& payload)
{
  const uint64_t kSegment = 0x18538067;
  const uint64_t kTracks = 0x1654AE6B;
  const uint64_t kTrackEntry = 0xAE;

  RawElem head;
  if (!ParseElem(file, 0, file.size(), head))
    throw std::runtime_error("raw: no EBML head");
  RawElem segment;
  if (!ParseElem(file, static_cast<size_t>(head.end), file.size(), segment) ||
      segment.id != kSegment)
    throw std::runtime_error("raw: no segment");

  const size_t seg_data = static_cast<size_t>(segment.data);
  const size_t seg_end = static_cast<size_t>(segment.end);

  RawElem tracks;
  bool found_tracks = false;
  size_t scan = seg_data;
  while (scan < seg_end) {
    RawElem child;
    if (!ParseElem(file, scan, seg_end, child))
      throw std::runtime_error("raw: bad segment child");
    if (child.id == kTracks) {
      tracks = child;
      found_tracks = true;
      break;
    }
    scan = static_cast<size_t>(child.end);
  }
  if (!found_tracks)
    throw std::runtime_error("raw: no tracks");

  const size_t tracks_data = static_cast<size_t>(tracks.data);
  size_t entry_pos = 0;
  RawElem entry;
  bool found_entry = false;
  size_t tpos = tracks_data;
  while (tpos < tracks.end) {
    RawElem child;
    if (!ParseElem(file, tpos, static_cast<size_t>(tracks.end), child))
      throw std::runtime_error("raw: bad tracks child");
    if (child.id == kTrackEntry) {
      entry = child;
      entry_pos = tpos;
      found_entry = true;
      break;
    }
    tpos = static_cast<size_t>(child.end);
  }
  if (!found_entry)
    throw std::runtime_error("raw: no track entry");

  Bytes entry_payload(file.begin() + entry.data, file.begin() + entry.end);
  Bytes extra = Element(id, payload);
  entry_payload.insert(entry_payload.end(), extra.begin(), extra.end());
  Bytes entry_encoded = Element(kTrackEntry, entry_payload);

  Bytes tracks_payload;
  size_t wpos = tracks_data;
  while (wpos < tracks.end) {
    RawElem child;
    ParseElem(file, wpos, static_cast<size_t>(tracks.end), child);
    if (wpos == entry_pos)
      tracks_payload.insert(tracks_payload.end(), entry_encoded.begin(),
                            entry_encoded.end());
    else
      tracks_payload.insert(tracks_payload.end(), file.begin() + wpos,
                            file.begin() + child.end);
    wpos = static_cast<size_t>(child.end);
  }
  Bytes tracks_encoded = Element(kTracks, tracks_payload);

  Bytes segment_payload;
  size_t spos = seg_data;
  while (spos < seg_end) {
    RawElem child;
    ParseElem(file, spos, seg_end, child);
    if (child.id == kTracks)
      segment_payload.insert(segment_payload.end(), tracks_encoded.begin(),
                             tracks_encoded.end());
    else
      segment_payload.insert(segment_payload.end(), file.begin() + spos,
                             file.begin() + child.end);
    spos = static_cast<size_t>(child.end);
  }
  Bytes segment_encoded = Element(kSegment, segment_payload);

  Bytes out(file.begin(), file.begin() + seg_data);
  out.insert(out.end(), segment_encoded.begin(), segment_encoded.end());
  return out;
}

}  // namespace raw

// ---- IO wrapper that can observe and inject failures ----------------------

class TestIO : public IOCallback {
public:
  explicit TestIO(std::shared_ptr<IOCallback> inner) : inner_(std::move(inner)) {}

  uint32_t read(void* buffer, size_t size) override {
    const uint64_t start = inner_->getFilePointer();
    if (fail_reads_ > 0 && reads_seen_ >= fail_after_) {
      --fail_reads_;
      throw std::runtime_error("injected read failure");
    }
    ++reads_seen_;
    const uint32_t got = inner_->read(buffer, size);
    ranges_.emplace_back(start, got);
    observed_ += size;
    return got;
  }

  void setFilePointer(int64_t offset, libebml::seek_mode mode) override {
    inner_->setFilePointer(offset, mode);
  }
  size_t write(const void*, size_t) override { return 0; }
  uint64_t getFilePointer() override { return inner_->getFilePointer(); }
  void close() override {}

  void FailNextReads(size_t count) {
    fail_after_ = reads_seen_;
    fail_reads_ = count;
  }
  void ClearFailures() { fail_reads_ = 0; }
  void ResetStats() {
    observed_ = 0;
    ranges_.clear();
  }

  uint64_t bytes_requested() const { return observed_; }
  const std::vector<std::pair<uint64_t, uint32_t>>& ranges() const {
    return ranges_;
  }

  std::shared_ptr<IOCallback> inner() const { return inner_; }

private:
  std::shared_ptr<IOCallback> inner_;
  std::vector<std::pair<uint64_t, uint32_t>> ranges_;
  uint64_t observed_ = 0;
  size_t reads_seen_ = 0;
  size_t fail_after_ = 0;
  size_t fail_reads_ = 0;
};

inline std::shared_ptr<IOCallback> WriteTemp(const Bytes& bytes,
                                             const std::string& path)
{
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
      throw std::runtime_error("cannot write " + path);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  return std::make_shared<StdIOCallback>(path.c_str(), MODE_READ);
}

inline Bytes ReadFileBytes(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), {});
}

// ---- Canonical multi-source fixture -------------------------------------

struct MainFixture {
  std::vector<Bytes> file_bytes;
  std::array<uint8_t, 16> uidA{};
  std::array<uint8_t, 16> uidB{};
  std::array<uint8_t, 16> uidC{};
};

inline std::array<uint8_t, 16> MakeUID(uint8_t tag)
{
  std::array<uint8_t, 16> uid{};
  for (size_t i = 0; i < uid.size(); ++i)
    uid[i] = static_cast<uint8_t>(tag + i);
  return uid;
}

/*
  Program (edition 91, TrackUID 71):
    chapter 0 (storage index 0): source A [0, 2_000_000)        frames 0,1
    chapter 2 (storage index 2): source B [1_000_000, 4_000_000) frames 1..3
    chapter 3 (storage index 3): source A [2_000_000, 3_000_000) frame 2
    chapter 4 (storage index 4): source C [0, 2_000_000)        frames 0,1

  Storage index 1 is a disabled chapter and is skipped, but keeps its
  zero-based index.  Chapter 3 refers backwards to source A.  Source B has a
  different timebase, track number and CodecPrivate; source C writes both its
  Tracks and Chapters metadata after the cluster.  Source A mixes a
  SimpleBlock and a BlockGroup.
*/
inline MainFixture MakeMainFixture()
{
  MainFixture fx;
  fx.uidA = MakeUID(0x10);
  fx.uidB = MakeUID(0x20);
  fx.uidC = MakeUID(0x30);

  Bytes privA = {1, 2, 3, 4};
  Bytes privB = {5, 6, 7};
  Bytes privC = {8, 9};

  auto frame = [](uint8_t tag, size_t size) {
    Bytes payload(size);
    for (size_t i = 0; i < size; ++i)
      payload[i] = static_cast<uint8_t>(tag + i);
    return payload;
  };

  // Source A: three 1ms frames, track 7, 1ms timebase.  Frame 1 is a
  // BlockGroup with a reference and an explicit BlockDuration.
  FileSpec fa;
  fa.source.segment_uid = fx.uidA;
  fa.source.timecode_scale = 1000000;
  TrackSpec ta;
  ta.number = 7;
  ta.uid = 71;
  ta.codec_private = privA;
  ta.default_duration_ns = 1000000;
  fa.source.tracks = {ta};
  fa.source.frames = {
      {true, 0, frame(0x40, 12), 0, false, false, 0, false, 0},
      {false, 1000000, frame(0x50, 10), 0, true, true, 1, true, -1},
      {true, 2000000, frame(0x60, 14), 0, false, false, 0, false, 0},
  };
  fa.source.clusters = {{0, {0, 1}}, {2, {2}}};

  // Source B: four 1ms frames, track 11, 1us timebase.
  FileSpec fb;
  fb.source.segment_uid = fx.uidB;
  fb.source.timecode_scale = 1000;
  TrackSpec tb;
  tb.number = 11;
  tb.uid = 71;
  tb.codec_private = privB;
  tb.default_duration_ns = 1000000;
  fb.source.tracks = {tb};
  fb.source.frames = {
      {true, 0, frame(0x70, 9), 0, false, false, 0, false, 0},
      {true, 1000000, frame(0x80, 11), 0, false, false, 0, false, 0},
      {false, 2000000, frame(0x90, 13), 0, true, true, 1000, true, -1},
      {true, 3000000, frame(0xA0, 15), 0, false, false, 0, false, 0},
  };
  fb.source.clusters = {{0, {0, 1}}, {1000, {2, 3}}};

  // Source C: two frames, track 3; Tracks come after Clusters.
  FileSpec fc;
  fc.source.segment_uid = fx.uidC;
  fc.source.timecode_scale = 1000000;
  TrackSpec tc;
  tc.number = 3;
  tc.uid = 71;
  tc.codec_private = privC;
  tc.default_duration_ns = 1000000;
  fc.source.tracks = {tc};
  fc.source.frames = {
      {true, 0, frame(0xB0, 8), 0, false, false, 0, false, 0},
      {true, 1000000, frame(0xC0, 6), 0, false, false, 0, false, 0},
  };
  fc.source.clusters = {{0, {0, 1}}};
  fc.tracks_after_cluster = true;

  FileSpec root;
  root.source = fa.source;
  root.has_chapters = true;
  root.chapters_after_cluster = true;

  ChapterSpec c0;
  c0.uid = 100;
  c0.begin_ns = 0;
  c0.end_ns = 2000000;
  ChapterSpec c1;
  c1.uid = 101;
  c1.begin_ns = 0;
  c1.end_ns = 1000000;
  c1.enabled = false;
  ChapterSpec c2;
  c2.uid = 102;
  c2.begin_ns = 1000000;
  c2.end_ns = 4000000;
  c2.has_segment_uid = true;
  c2.segment_uid = fx.uidB;
  c2.hidden = 1;  // hidden chapters still play
  ChapterSpec c3;
  c3.uid = 103;
  c3.begin_ns = 2000000;
  c3.end_ns = 3000000;
  ChapterSpec c4;
  c4.uid = 104;
  c4.begin_ns = 0;
  c4.end_ns = 2000000;
  c4.has_segment_uid = true;
  c4.segment_uid = fx.uidC;
  root.editions.push_back(EditionSpec{91, true, {c0, c1, c2, c3, c4}});

  fx.file_bytes.push_back(BuildFile(root));
  fx.file_bytes.push_back(BuildFile(fb));
  fx.file_bytes.push_back(BuildFile(fc));
  return fx;
}

}  // namespace ordered_test

#endif  // ORDERED_TEST_FIXTURE_H
