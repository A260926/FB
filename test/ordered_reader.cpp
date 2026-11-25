// Tests for KaxOrderedReader: ordered-chapter linking, seeking and IO
// resilience.  Registered through the task_support driver; no main here.

#include "task_support/check.h"
#include "ordered_fixture.h"

#include <matroska/KaxOrderedReader.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace libmatroska;
using namespace ordered_test;
using Reader = KaxOrderedReader;

namespace {

const std::string kDir = ".task-build";

std::shared_ptr<TestIO> MakeSource(const Bytes& bytes, const std::string& name)
{
  auto path = kDir + "/" + name;
  std::shared_ptr<libebml::IOCallback> inner = WriteTemp(bytes, path);
  return std::make_shared<TestIO>(inner);
}

struct Opened {
  std::unique_ptr<Reader> reader;
  std::vector<std::shared_ptr<TestIO>> ios;
};

bool OpenMain(Opened& out, const MainFixture& fx,
              const std::vector<std::shared_ptr<TestIO>>& ios_override = {},
              size_t root = 0, uint64_t edition = 91, uint64_t track = 71)
{
  out.ios.clear();
  std::vector<std::shared_ptr<libebml::IOCallback>> sources;
  if (ios_override.empty()) {
    for (size_t i = 0; i < fx.file_bytes.size(); ++i) {
      auto io = MakeSource(fx.file_bytes[i], "main_" + std::to_string(i) + ".mkv");
      out.ios.push_back(io);
      sources.push_back(io);
    }
  } else {
    out.ios = ios_override;
    for (auto& io : out.ios)
      sources.push_back(io);
  }
  return Reader::Open(sources, root, edition, track, out.reader) && out.reader;
}

}  // namespace

// ---------------------------------------------------------------------------
// Linking
// ---------------------------------------------------------------------------

static int CheckMainEventSequence()
{
  MainFixture fx = MakeMainFixture();
  Opened opened;
  TASK_CHECK(OpenMain(opened, fx));
  Reader& reader = *opened.reader;
  TASK_CHECK(reader.DurationNS() == 8000000);

  std::vector<Reader::Packet> events;
  Reader::Packet packet;
  while (true) {
    auto status = reader.ReadNext(packet);
    if (status == Reader::Status::End)
      break;
    if (status != Reader::Status::Ok)
      return 1;
    events.push_back(packet);
  }

  // Chapter 0: Config + 2 frames, chapter index 0.
  TASK_CHECK(events.size() == 3 + 4 + 2 + 3);
  TASK_CHECK(events[0].kind == Reader::Kind::Config);
  TASK_CHECK(events[0].chapter_index == 0);
  TASK_CHECK(events[0].source_index == 0);
  TASK_CHECK(events[0].track_number == 7);
  TASK_CHECK(events[0].source_ns == 0);
  TASK_CHECK(events[0].program_ns == 0);
  TASK_CHECK(events[0].duration_ns == 0);

  // First frame of chapter 0.
  TASK_CHECK(events[1].kind == Reader::Kind::Frame);
  TASK_CHECK(events[1].source_index == 0);
  TASK_CHECK(events[1].source_ns == 0);
  TASK_CHECK(events[1].program_ns == 0);
  TASK_CHECK(events[1].duration_ns == 1000000);
  TASK_CHECK(events[1].visible_begin_ns == 0);
  TASK_CHECK(events[1].visible_duration_ns == 1000000);
  TASK_CHECK(events[1].keyframe);
  TASK_CHECK(events[1].data.size() == 12);

  // Second frame of chapter 0 comes from a BlockGroup carrying a reference.
  TASK_CHECK(events[2].source_ns == 1000000);
  TASK_CHECK(events[2].program_ns == 1000000);
  TASK_CHECK(!events[2].keyframe);
  TASK_CHECK(events[2].data.size() == 10);

  // Chapter 1 was disabled, so the next Config belongs to chapter index 2.
  TASK_CHECK(events[3].kind == Reader::Kind::Config);
  TASK_CHECK(events[3].chapter_index == 2);
  TASK_CHECK(events[3].source_index == 1);
  TASK_CHECK(events[3].track_number == 11);
  TASK_CHECK(events[3].source_ns == 1000000);
  TASK_CHECK(events[3].program_ns == 2000000);

  // Source B uses a 1us timebase: source_ns = ticks * 1000.
  TASK_CHECK(events[4].source_ns == 1000000);
  TASK_CHECK(events[4].program_ns == 2000000);
  TASK_CHECK(events[5].source_ns == 2000000);
  TASK_CHECK(events[5].program_ns == 3000000);
  TASK_CHECK(!events[5].keyframe);
  TASK_CHECK(events[5].duration_ns == 1000000);
  TASK_CHECK(events[6].source_ns == 3000000);
  TASK_CHECK(events[6].program_ns == 4000000);
  TASK_CHECK(events[6].keyframe);

  // Chapter 3 returns to source A for one frame starting at 2_000_000ns.
  TASK_CHECK(events[7].kind == Reader::Kind::Config);
  TASK_CHECK(events[7].chapter_index == 3);
  TASK_CHECK(events[7].source_index == 0);
  TASK_CHECK(events[7].source_ns == 2000000);
  TASK_CHECK(events[7].program_ns == 5000000);
  TASK_CHECK(events[8].kind == Reader::Kind::Frame);
  TASK_CHECK(events[8].source_ns == 2000000);
  TASK_CHECK(events[8].program_ns == 5000000);

  // Chapter 4 plays the metadata-after-cluster source C.
  TASK_CHECK(events[9].kind == Reader::Kind::Config);
  TASK_CHECK(events[9].chapter_index == 4);
  TASK_CHECK(events[9].source_index == 2);
  TASK_CHECK(events[9].track_number == 3);
  TASK_CHECK(events[10].source_ns == 0);
  TASK_CHECK(events[10].program_ns == 6000000);
  TASK_CHECK(events[11].source_ns == 1000000);
  TASK_CHECK(events[11].program_ns == 7000000);
  // Chapter ends at 2_000_000; the last frame is fully inside.
  TASK_CHECK(events[11].visible_duration_ns == 1000000);

  return 0;
}

static int CheckCodecPrivateRepublish()
{
  MainFixture fx = MakeMainFixture();
  Opened opened;
  TASK_CHECK(OpenMain(opened, fx));
  Reader& reader = *opened.reader;

  // The second chapter of source A republishes A's CodecPrivate even though
  // the source was already seen in chapter 0.
  Reader::Packet packet;
  for (int i = 0; i < 4; ++i) {
    TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  }
  TASK_CHECK(packet.kind == Reader::Kind::Config);
  TASK_CHECK(packet.chapter_index == 2);
  const Bytes privB = {5, 6, 7};
  TASK_CHECK(packet.data == privB);

  TASK_CHECK(reader.Seek(0));
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Config);
  const Bytes privA = {1, 2, 3, 4};
  TASK_CHECK(packet.data == privA);
  return 0;
}

static int CheckOpenRejectsMissingUid()
{
  MainFixture fx = MakeMainFixture();
  auto root = MakeSource(fx.file_bytes[0], "missing_uid_root.mkv");
  auto other = MakeSource(fx.file_bytes[1], "missing_uid_other.mkv");

  std::vector<std::shared_ptr<libebml::IOCallback>> sources{root, other};
  std::unique_ptr<Reader> reader;
  TASK_CHECK(!Reader::Open(sources, 0, 91, 71, reader));
  TASK_CHECK(!reader);

  // Root source without chapters.
  std::vector<std::shared_ptr<libebml::IOCallback>> one{other};
  TASK_CHECK(!Reader::Open(one, 0, 91, 71, reader));
  TASK_CHECK(!reader);

  // Unknown edition.
  TASK_CHECK(!Reader::Open(sources, 0, 999, 71, reader));

  // Root index out of range.
  TASK_CHECK(!Reader::Open(sources, 5, 91, 71, reader));
  return 0;
}

static int CheckAmbiguousSegmentUid()
{
  MainFixture fx = MakeMainFixture();
  auto a = MakeSource(fx.file_bytes[0], "amb_a.mkv");
  auto b = MakeSource(fx.file_bytes[1], "amb_b.mkv");
  // Duplicate SegmentUID: use A's bytes twice.
  auto dup = MakeSource(fx.file_bytes[0], "amb_dup.mkv");

  std::vector<std::shared_ptr<libebml::IOCallback>> sources{a, b, dup};
  std::unique_ptr<Reader> reader;
  TASK_CHECK(!Reader::Open(sources, 0, 91, 71, reader));
  TASK_CHECK(!reader);

  // Same UID set but the root's chapter reference cannot be resolved because
  // two sources share the referenced UID.
  FileSpec root_spec;
  root_spec.source.segment_uid = MakeUID(0x99);
  root_spec.source.timecode_scale = 1000000;
  TrackSpec t;
  t.number = 9;
  t.uid = 71;
  t.codec_private = {1};
  t.default_duration_ns = 1000000;
  root_spec.source.tracks = {t};
  root_spec.source.frames = {{true, 0, Bytes{9, 9}, 0, false, false, 0, false, 0}};
  ChapterSpec c;
  c.uid = 1;
  c.begin_ns = 0;
  c.end_ns = 1000000;
  c.has_segment_uid = true;
  c.segment_uid = MakeUID(0x20);
  root_spec.has_chapters = true;
  root_spec.editions.push_back(EditionSpec{5, true, {c}});
  auto root = MakeSource(BuildFile(root_spec), "amb_root.mkv");
  auto b1 = MakeSource(fx.file_bytes[1], "amb_b1.mkv");
  auto b2 = MakeSource(fx.file_bytes[1], "amb_b2.mkv");
  std::vector<std::shared_ptr<libebml::IOCallback>> sources2{root, b1, b2};
  TASK_CHECK(!Reader::Open(sources2, 0, 5, 71, reader));
  TASK_CHECK(!reader);
  return 0;
}

static int CheckDisabledChaptersSkipButKeepIndex()
{
  MainFixture fx = MakeMainFixture();
  Opened opened;
  TASK_CHECK(OpenMain(opened, fx));
  Reader& reader = *opened.reader;
  Reader::Packet packet;

  // Collect chapter indices of Config events.
  std::vector<size_t> chapters;
  while (reader.ReadNext(packet) == Reader::Status::Ok) {
    if (packet.kind == Reader::Kind::Config)
      chapters.push_back(packet.chapter_index);
  }
  TASK_CHECK(chapters.size() == 4);
  TASK_CHECK(chapters[0] == 0);
  TASK_CHECK(chapters[1] == 2);
  TASK_CHECK(chapters[2] == 3);
  TASK_CHECK(chapters[3] == 4);
  return 0;
}

static int CheckRejectsNonOrderedEdition()
{
  MainFixture fx = MakeMainFixture();
  std::unique_ptr<Reader> reader;
  // Edition 91 has Ordered=0 in a rebuilt root below.
  FileSpec spec;
  spec.source = SourceSpec{};
  spec.source.segment_uid = MakeUID(0x55);
  TrackSpec t;
  t.number = 1;
  t.uid = 71;
  t.codec_private = {1};
  t.default_duration_ns = 1000000;
  spec.source.tracks = {t};
  spec.source.frames = {{true, 0, Bytes{1, 2, 3}, 0, false, false, 0, false, 0}};
  spec.has_chapters = true;
  ChapterSpec c;
  c.uid = 1;
  c.begin_ns = 0;
  c.end_ns = 1000000;
  spec.editions.push_back(EditionSpec{7, false, {c}});
  auto root2 = MakeSource(BuildFile(spec), "nonordered2.mkv");
  std::vector<std::shared_ptr<libebml::IOCallback>> sources2{root2};
  TASK_CHECK(!Reader::Open(sources2, 0, 7, 71, reader));

  // Duplicate edition UID is ambiguous.
  FileSpec dup;
  dup.source = spec.source;
  dup.has_chapters = true;
  dup.editions.push_back(EditionSpec{7, true, {c}});
  dup.editions.push_back(EditionSpec{7, true, {c}});
  auto root3 = MakeSource(BuildFile(dup), "nonordered3.mkv");
  std::vector<std::shared_ptr<libebml::IOCallback>> sources3{root3};
  TASK_CHECK(!Reader::Open(sources3, 0, 7, 71, reader));
  return 0;
}

static int CheckUnsupportedFeaturesRejected()
{
  ChapterSpec base;
  base.uid = 1;
  base.begin_ns = 0;
  base.end_ns = 1000000;

  auto try_open = [](const FileSpec& spec) {
    auto io = MakeSource(BuildFile(spec), "unsupported.mkv");
    std::vector<std::shared_ptr<libebml::IOCallback>> sources{io};
    std::unique_ptr<Reader> reader;
    return Reader::Open(sources, 0, 1, 71, reader) && reader;
  };

  auto make_base = []() {
    FileSpec spec;
    spec.source.segment_uid = MakeUID(0x77);
    TrackSpec t;
    t.number = 1;
    t.uid = 71;
    t.codec_private = {1};
    t.default_duration_ns = 1000000;
    spec.source.tracks = {t};
    spec.source.frames = {{true, 0, Bytes{1, 2, 3}, 0, true, false, 0, false, 0}};
    spec.has_chapters = true;
    ChapterSpec c;
    c.uid = 1;
    c.begin_ns = 0;
    c.end_ns = 1000000;
    spec.editions.push_back(EditionSpec{1, true, {c}});
    return spec;
  };

  TASK_CHECK(try_open(make_base()));

  {
    FileSpec spec = make_base();
    spec.editions[0].chapters[0].has_nested = true;
    TASK_CHECK(!try_open(spec));
  }
  {
    FileSpec spec = make_base();
    spec.editions[0].chapters[0].has_track = true;
    TASK_CHECK(!try_open(spec));
  }
  {
    FileSpec spec = make_base();
    spec.editions[0].chapters[0].has_process = true;
    TASK_CHECK(!try_open(spec));
  }
  {
    FileSpec spec = make_base();
    spec.editions[0].chapters[0].has_segment_edition_uid = true;
    TASK_CHECK(!try_open(spec));
  }
  {
    FileSpec spec = make_base();
    spec.source.frames[0].codec_state = true;
    TASK_CHECK(!try_open(spec));
  }
  {
    FileSpec spec = make_base();
    spec.source.frames[0].discard_padding = 3;
    TASK_CHECK(!try_open(spec));
  }
  {
    FileSpec spec = make_base();
    spec.source.tracks[0].codec_delay = 5;
    TASK_CHECK(!try_open(spec));
  }
  {
    FileSpec spec = make_base();
    spec.source.tracks[0].seek_preroll = 5;
    TASK_CHECK(!try_open(spec));
  }
  {
    // TrackTimecodeScale is not renderable through the legacy writer, so
    // inject it at the byte level.
    Bytes injected = raw::AppendToFirstTrackEntry(
        BuildFile(make_base()), 0x23314F, raw::FloatElement(0, 2.0));
    auto io = MakeSource(injected, "unsupported_tts.mkv");
    std::vector<std::shared_ptr<libebml::IOCallback>> sources{io};
    std::unique_ptr<Reader> reader;
    TASK_CHECK(!Reader::Open(sources, 0, 1, 71, reader));
  }
  {
    FileSpec spec = make_base();
    spec.source.tracks[0].content_encodings = true;
    TASK_CHECK(!try_open(spec));
  }
  return 0;
}

static int CheckChapterBoundaries()
{
  auto make = [](uint64_t begin, uint64_t end, bool keyframe_start) {
    FileSpec spec;
    spec.source.segment_uid = MakeUID(0x11);
    TrackSpec t;
    t.number = 2;
    t.uid = 71;
    t.codec_private = {1};
    t.default_duration_ns = 1000000;
    spec.source.tracks = {t};
    spec.source.frames = {
        {keyframe_start, 0, Bytes{1}, 0, false, false, 0, false, 0},
        {true, 1000000, Bytes{2}, 0, false, false, 0, false, 0},
        {true, 2000000, Bytes{3}, 0, false, false, 0, false, 0},
    };
    spec.has_chapters = true;
    ChapterSpec c;
    c.uid = 1;
    c.begin_ns = begin;
    c.end_ns = end;
    spec.editions.push_back(EditionSpec{1, true, {c}});
    return BuildFile(spec);
  };

  auto open_bytes = [](const Bytes& bytes) {
    auto io = MakeSource(bytes, "boundary.mkv");
    std::vector<std::shared_ptr<libebml::IOCallback>> sources{io};
    std::unique_ptr<Reader> reader;
    return Reader::Open(sources, 0, 1, 71, reader);
  };

  // begin must be exactly on a keyframe.
  TASK_CHECK(open_bytes(make(0, 2000000, true)));
  TASK_CHECK(!open_bytes(make(0, 2000000, false)));
  // A begin inside a frame (not on its start) is invalid.
  TASK_CHECK(!open_bytes(make(500000, 2000000, true)));
  TASK_CHECK(open_bytes(make(1000000, 2000000, true)));
  // end must not exceed the selected track end (3_000_000).
  TASK_CHECK(!open_bytes(make(0, 3000001, true)));
  // begin >= end is invalid.
  TASK_CHECK(!open_bytes(make(2000000, 2000000, true)));
  return 0;
}

static int CheckTrackSelectionRules()
{
  // TrackNumber may differ per source but CodecID and dimensions must match.
  FileSpec spec;
  spec.source.segment_uid = MakeUID(0x22);
  TrackSpec a;
  a.number = 4;
  a.uid = 71;
  a.codec_private = {1};
  a.default_duration_ns = 1000000;
  a.codec_id = "V_VP8";
  TrackSpec other;
  other.number = 8;
  other.uid = 99;
  other.codec_id = "A_OPUS";
  other.video = false;
  other.codec_private = {9};
  other.default_duration_ns = 1000000;
  spec.source.tracks = {a, other};
  spec.source.frames = {
      {true, 0, Bytes{1, 1}, 0, false, false, 0, false, 0},
      {true, 1000000, Bytes{2, 2}, 0, false, false, 0, false, 0},
  };
  spec.has_chapters = true;
  ChapterSpec c;
  c.uid = 1;
  c.begin_ns = 0;
  c.end_ns = 2000000;
  spec.editions.push_back(EditionSpec{1, true, {c}});

  auto io = MakeSource(BuildFile(spec), "track_rules.mkv");
  std::vector<std::shared_ptr<libebml::IOCallback>> sources{io};
  std::unique_ptr<Reader> reader;
  TASK_CHECK(Reader::Open(sources, 0, 1, 71, reader));
  Reader::Packet packet;
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.track_number == 4);
  // Unselected track frames are skipped, and frame payloads come from track 4.
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.data.size() == 2);
  TASK_CHECK(packet.data[0] == 1);
  return 0;
}

static int CheckMixedTrackFramesSkipped()
{
  // Both tracks are interleaved in the same cluster.  Only the selected
  // track's frames must be indexed; the audio frames sit between them.
  FileSpec spec;
  spec.source.segment_uid = MakeUID(0x23);
  TrackSpec video;
  video.number = 1;
  video.uid = 71;
  video.codec_private = {1};
  video.default_duration_ns = 1000000;
  TrackSpec audio;
  audio.number = 2;
  audio.uid = 72;
  audio.codec_id = "A_OPUS";
  audio.video = false;
  audio.default_duration_ns = 500000;
  spec.source.tracks = {video, audio};
  spec.source.frames = {
      {true, 0, Bytes{0x10}, 0, false, false, 0, false, 0},
      {true, 0, Bytes{0xA0}, 1, false, false, 0, false, 0},
      {true, 1000000, Bytes{0x11}, 0, false, false, 0, false, 0},
      {true, 500000, Bytes{0xA1}, 1, false, false, 0, false, 0},
      {true, 1500000, Bytes{0xA2}, 1, false, false, 0, false, 0},
      {true, 2000000, Bytes{0x12}, 0, false, false, 0, false, 0},
  };
  spec.has_chapters = true;
  ChapterSpec c;
  c.uid = 1;
  c.begin_ns = 0;
  c.end_ns = 3000000;
  spec.editions.push_back(EditionSpec{1, true, {c}});

  auto io = MakeSource(BuildFile(spec), "mixed_tracks.mkv");
  std::vector<std::shared_ptr<libebml::IOCallback>> sources{io};
  std::unique_ptr<Reader> reader;
  TASK_CHECK(Reader::Open(sources, 0, 1, 71, reader));
  Reader::Packet packet;
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);  // config
  for (int i = 0; i < 3; ++i) {
    TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);
    TASK_CHECK(packet.kind == Reader::Kind::Frame);
    TASK_CHECK(packet.data.size() == 1);
    TASK_CHECK(packet.data[0] == static_cast<uint8_t>(0x10 + i));
    TASK_CHECK(packet.source_ns == static_cast<uint64_t>(i) * 1000000);
    TASK_CHECK(packet.track_number == 1);
  }
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::End);
  return 0;
}

static int CheckCodecMismatchRejected()
{
  // Two referenced sources whose CodecID differs must fail at Open.
  auto make = [](uint8_t uid_tag, const char* codec) {
    FileSpec spec;
    spec.source.segment_uid = MakeUID(uid_tag);
    TrackSpec t;
    t.number = 1;
    t.uid = 71;
    t.codec_id = codec;
    t.codec_private = {1};
    t.default_duration_ns = 1000000;
    spec.source.tracks = {t};
    spec.source.frames = {{true, 0, Bytes{1}, 0, false, false, 0, false, 0}};
    return spec;
  };

  auto source_b = make(0x41, "V_VP9");

  FileSpec root = make(0x51, "V_VP8");
  root.has_chapters = true;
  ChapterSpec c0;
  c0.uid = 1;
  c0.begin_ns = 0;
  c0.end_ns = 1000000;
  ChapterSpec c1;
  c1.uid = 2;
  c1.begin_ns = 0;
  c1.end_ns = 1000000;
  c1.has_segment_uid = true;
  c1.segment_uid = MakeUID(0x41);
  root.editions.push_back(EditionSpec{1, true, {c0, c1}});

  auto ra = MakeSource(BuildFile(root), "codec_a.mkv");
  auto rb = MakeSource(BuildFile(source_b), "codec_b.mkv");
  std::vector<std::shared_ptr<libebml::IOCallback>> sources{ra, rb};
  std::unique_ptr<Reader> reader;
  TASK_CHECK(!Reader::Open(sources, 0, 1, 71, reader));
  TASK_CHECK(!reader);
  return 0;
}

static int CheckEmptyEdition()
{
  // An ordered edition with no enabled chapters has zero duration and
  // immediately returns End.
  FileSpec spec;
  spec.source.segment_uid = MakeUID(0x61);
  TrackSpec t;
  t.number = 1;
  t.uid = 71;
  t.codec_private = {1};
  t.default_duration_ns = 1000000;
  spec.source.tracks = {t};
  spec.source.frames = {{true, 0, Bytes{1}, 0, false, false, 0, false, 0}};
  spec.has_chapters = true;
  ChapterSpec disabled;
  disabled.uid = 1;
  disabled.begin_ns = 0;
  disabled.end_ns = 1000000;
  disabled.enabled = false;
  spec.editions.push_back(EditionSpec{1, true, {disabled}});

  auto io = MakeSource(BuildFile(spec), "empty_edition.mkv");
  std::vector<std::shared_ptr<libebml::IOCallback>> sources{io};
  std::unique_ptr<Reader> reader;
  TASK_CHECK(Reader::Open(sources, 0, 1, 71, reader));
  TASK_CHECK(reader->DurationNS() == 0);
  Reader::Packet packet;
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::End);
  TASK_CHECK(reader->Seek(0));
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::End);
  return 0;
}

static int CheckEdgeSeekPositions()
{
  MainFixture fx = MakeMainFixture();
  Opened opened;
  TASK_CHECK(OpenMain(opened, fx));
  Reader& reader = *opened.reader;
  Reader::Packet packet;

  // Seek to the last program nanosecond before the end.
  const uint64_t duration = reader.DurationNS();
  TASK_CHECK(reader.Seek(duration - 1));
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Config);
  TASK_CHECK(packet.chapter_index == 4);

  // Walk to the final frame and check the visible window is clipped.
  Reader::Packet last;
  while (reader.ReadNext(packet) == Reader::Status::Ok)
    last = packet;
  TASK_CHECK(last.kind == Reader::Kind::Frame);
  TASK_CHECK(last.program_ns == duration - 1000000);
  // Visible fields are source-time: the final frame covers source
  // [1_000_000, 2_000_000), and the seek target is source 1_999_999.
  TASK_CHECK(last.source_ns == 1000000);
  TASK_CHECK(last.visible_begin_ns == 1999999);
  TASK_CHECK(last.visible_duration_ns == 1);
  return 0;
}


// ---------------------------------------------------------------------------
// Seek
// ---------------------------------------------------------------------------

static int CheckSeekIntraFrame()
{
  MainFixture fx = MakeMainFixture();
  Opened opened;
  TASK_CHECK(OpenMain(opened, fx));
  Reader& reader = *opened.reader;
  Reader::Packet packet;

  // 1_500_000ns falls inside chapter 0's second frame [1_000_000, 2_000_000).
  TASK_CHECK(reader.Seek(1500000));
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Config);
  TASK_CHECK(packet.chapter_index == 0);
  TASK_CHECK(packet.source_ns == 0);
  TASK_CHECK(packet.program_ns == 0);

  // Back up to the chapter keyframe (frame 0), which is fully hidden now.
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Frame);
  TASK_CHECK(packet.source_ns == 0);
  TASK_CHECK(packet.visible_begin_ns == 0);
  TASK_CHECK(packet.visible_duration_ns == 0);

  // The target frame is clipped at 1_500_000.
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.source_ns == 1000000);
  TASK_CHECK(packet.visible_begin_ns == 1500000);
  TASK_CHECK(packet.visible_duration_ns == 500000);

  // End of chapter 0.
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Config);
  TASK_CHECK(packet.chapter_index == 2);
  return 0;
}

static int CheckSeekAcrossChapters()
{
  MainFixture fx = MakeMainFixture();
  Opened opened;
  TASK_CHECK(OpenMain(opened, fx));
  Reader& reader = *opened.reader;
  Reader::Packet packet;

  // Program 3_500_000 is inside chapter 2 (source B): program 2_000_000 offset
  // into that chapter, source time 2_500_000.  The nearest keyframe at or
  // before it is the 1_000_000ns frame.
  TASK_CHECK(reader.Seek(3500000));
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Config);
  TASK_CHECK(packet.chapter_index == 2);
  TASK_CHECK(packet.source_index == 1);
  TASK_CHECK(packet.source_ns == 1000000);
  TASK_CHECK(packet.program_ns == 2000000);

  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.source_ns == 1000000);
  TASK_CHECK(packet.program_ns == 2000000);
  TASK_CHECK(packet.visible_duration_ns == 0);  // before target

  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.source_ns == 2000000);
  TASK_CHECK(packet.program_ns == 3000000);
  TASK_CHECK(packet.visible_begin_ns == 2500000);
  TASK_CHECK(packet.visible_duration_ns == 500000);

  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.source_ns == 3000000);
  TASK_CHECK(packet.program_ns == 4000000);
  TASK_CHECK(packet.visible_begin_ns == 3000000);
  TASK_CHECK(packet.visible_duration_ns == 1000000);

  // Seeking to the exact program end is valid but enters End.
  TASK_CHECK(reader.Seek(reader.DurationNS()));
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::End);
  return 0;
}

static int CheckSeekBounds()
{
  MainFixture fx = MakeMainFixture();
  Opened opened;
  TASK_CHECK(OpenMain(opened, fx));
  Reader& reader = *opened.reader;
  Reader::Packet packet;

  // Establish a position mid-chapter 2.
  TASK_CHECK(reader.Seek(2500000));
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Config);
  const auto before = packet;

  // Out-of-range seek fails and does not change state.
  TASK_CHECK(!reader.Seek(reader.DurationNS() + 1));
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Frame);
  TASK_CHECK(packet.chapter_index == before.chapter_index);
  TASK_CHECK(packet.program_ns == before.program_ns);
  TASK_CHECK(packet.source_ns == 1000000);

  // Consecutive End calls stay End and leave the packet untouched.
  TASK_CHECK(reader.Seek(reader.DurationNS()));
  packet.kind = Reader::Kind::Frame;
  packet.program_ns = 12345;
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::End);
  TASK_CHECK(packet.kind == Reader::Kind::Frame);
  TASK_CHECK(packet.program_ns == 12345);
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::End);
  TASK_CHECK(packet.program_ns == 12345);
  return 0;
}

static int CheckSeekToChapterStart()
{
  MainFixture fx = MakeMainFixture();
  Opened opened;
  TASK_CHECK(OpenMain(opened, fx));
  Reader& reader = *opened.reader;
  Reader::Packet packet;

  // Program 2_000_000 is exactly the start of chapter 2.
  TASK_CHECK(reader.Seek(2000000));
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Config);
  TASK_CHECK(packet.chapter_index == 2);
  TASK_CHECK(packet.source_ns == 1000000);
  TASK_CHECK(packet.program_ns == 2000000);
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.source_ns == 1000000);
  TASK_CHECK(packet.visible_begin_ns == 1000000);
  TASK_CHECK(packet.visible_duration_ns == 1000000);
  return 0;
}

static int CheckSeekHiddenChapter()
{
  MainFixture fx = MakeMainFixture();
  Opened opened;
  TASK_CHECK(OpenMain(opened, fx));
  Reader& reader = *opened.reader;
  Reader::Packet packet;

  // Chapter 2 in storage has ChapterFlagHidden=1; it must remain seekable.
  TASK_CHECK(reader.Seek(2500000));
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.chapter_index == 2);
  return 0;
}

// ---------------------------------------------------------------------------
// IO
// ---------------------------------------------------------------------------

static int CheckOpenDoesNotReadPayloadTail()
{
  MainFixture fx = MakeMainFixture();
  auto root = MakeSource(fx.file_bytes[0], "io_open_root.mkv");
  auto b = MakeSource(fx.file_bytes[1], "io_open_b.mkv");
  auto c = MakeSource(fx.file_bytes[2], "io_open_c.mkv");
  root->ResetStats();
  b->ResetStats();
  c->ResetStats();

  std::vector<std::shared_ptr<TestIO>> ios{root, b, c};
  Opened opened;
  TASK_CHECK(OpenMain(opened, fx, ios));

  // Our fixture frame payloads are small; Open must not have read the whole
  // media.  It is allowed to read metadata, but must stay well short of the
  // total file size.
  for (size_t i = 0; i < fx.file_bytes.size(); ++i) {
    const uint64_t read = ios[i]->bytes_requested();
    TASK_CHECK(read < fx.file_bytes[i].size());
    TASK_CHECK(ios[i]->bytes_requested() < 1024);
  }
  return 0;
}

static int CheckOpenSkipsLargePayload()
{
  // A source with several large frames.  Open must index them without
  // pulling the payloads into memory: its reads should stay tiny compared
  // to the media size.
  const size_t kFrameSize = 200000;
  Bytes big(kFrameSize, 0x5A);
  FileSpec spec;
  spec.source.segment_uid = MakeUID(0x71);
  TrackSpec t;
  t.number = 1;
  t.uid = 71;
  t.codec_private = {1, 2};
  t.default_duration_ns = 1000000;
  spec.source.tracks = {t};
  for (uint64_t i = 0; i < 12; ++i)
    spec.source.frames.push_back({true, i * 1000000, big, 0, false, false, 0, false, 0});
  spec.has_chapters = true;
  ChapterSpec c;
  c.uid = 1;
  c.begin_ns = 0;
  c.end_ns = 12000000;
  spec.editions.push_back(EditionSpec{1, true, {c}});

  Bytes file = BuildFile(spec);
  auto io = MakeSource(file, "io_large.mkv");
  io->ResetStats();
  std::vector<std::shared_ptr<TestIO>> ios{io};
  std::vector<std::shared_ptr<libebml::IOCallback>> sources{io};
  std::unique_ptr<Reader> reader;
  TASK_CHECK(Reader::Open(sources, 0, 1, 71, reader));
  TASK_CHECK(reader->DurationNS() == 12000000);

  const uint64_t media = kFrameSize * 12;
  TASK_CHECK(io->bytes_requested() < media / 8);
  TASK_CHECK(io->bytes_requested() < 4096);

  // Reading a frame returns the full payload, and only the payload.
  Reader::Packet packet;
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);  // config
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.data.size() == kFrameSize);
  TASK_CHECK(packet.data[0] == 0x5A);
  return 0;
}

static int CheckIoFailureRetry()
{
  MainFixture fx = MakeMainFixture();
  Opened opened;
  TASK_CHECK(OpenMain(opened, fx));
  Reader& reader = *opened.reader;
  Reader::Packet packet;

  // Consume Config then frame 0 of chapter 0.
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Frame);
  TASK_CHECK(packet.source_ns == 0);
  const auto frame0 = packet;

  // Break the IO, attempt the next frame, and confirm Error with no state
  // change.
  opened.ios[0]->FailNextReads(10);
  auto status = reader.ReadNext(packet);
  TASK_CHECK(status == Reader::Status::Error);
  TASK_CHECK(packet.source_ns == frame0.source_ns);
  TASK_CHECK(packet.kind == frame0.kind);

  // Same call after clearing the failure must retry exactly the same event.
  opened.ios[0]->ClearFailures();
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Frame);
  TASK_CHECK(packet.source_ns == 1000000);
  TASK_CHECK(packet.program_ns == 1000000);
  TASK_CHECK(!packet.keyframe);
  return 0;
}

static int CheckIoFailureOnChapterTransition()
{
  MainFixture fx = MakeMainFixture();
  Opened opened;
  TASK_CHECK(OpenMain(opened, fx));
  Reader& reader = *opened.reader;
  Reader::Packet packet;

  // Walk to the last frame of chapter 0, then fail the next source read.
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);  // config
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);  // frame 0
  opened.ios[0]->FailNextReads(80);
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Error);

  opened.ios[0]->ClearFailures();
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Frame);
  TASK_CHECK(packet.source_ns == 1000000);
  TASK_CHECK(packet.program_ns == 1000000);
  // Next event must be chapter 2's Config, not boundary-crossing End.
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Config);
  TASK_CHECK(packet.chapter_index == 2);
  return 0;
}

static int CheckIoFailureOnSeekReplay()
{
  MainFixture fx = MakeMainFixture();
  Opened opened;
  TASK_CHECK(OpenMain(opened, fx));
  Reader& reader = *opened.reader;
  Reader::Packet packet;

  TASK_CHECK(reader.Seek(1500000));
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);  // config
  opened.ios[0]->FailNextReads(10);
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Error);
  opened.ios[0]->ClearFailures();

  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Frame);
  TASK_CHECK(packet.source_ns == 0);
  TASK_CHECK(packet.visible_duration_ns == 0);
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.source_ns == 1000000);
  TASK_CHECK(packet.visible_begin_ns == 1500000);
  TASK_CHECK(packet.visible_duration_ns == 500000);
  return 0;
}

static int CheckNoSharedBuffer()
{
  MainFixture fx = MakeMainFixture();
  Opened opened;
  TASK_CHECK(OpenMain(opened, fx));
  Reader& reader = *opened.reader;
  Reader::Packet packet;

  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);  // config
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);  // frame 0
  Bytes first = packet.data;
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);  // frame 1
  TASK_CHECK(packet.data != first);

  // Mutating the first copy must not affect the reader's future data.
  first.assign(first.size(), 0xEE);
  TASK_CHECK(reader.Seek(0));
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);  // config
  TASK_CHECK(reader.ReadNext(packet) == Reader::Status::Ok);  // frame 0
  TASK_CHECK(packet.data.size() == 12);
  TASK_CHECK(packet.data[0] != 0xEE);
  return 0;
}

static int CheckNegativeLocalTimecode()
{
  // Cluster timecode 50 units, frame at 10 units -> local timecode -40.
  FileSpec spec;
  spec.source.segment_uid = MakeUID(0x44);
  spec.source.timecode_scale = 1000000;
  spec.source.has_cluster_origin = true;
  spec.source.cluster_origin = 50;
  TrackSpec t;
  t.number = 1;
  t.uid = 71;
  t.codec_private = {1};
  t.default_duration_ns = 1000000;
  spec.source.tracks = {t};
  spec.source.frames = {
      {true, 10000000, Bytes{1}, 0, false, false, 0, false, 0},
      {true, 11000000, Bytes{2}, 0, false, false, 0, false, 0},
  };
  spec.has_chapters = true;
  ChapterSpec c;
  c.uid = 1;
  c.begin_ns = 10000000;
  c.end_ns = 12000000;
  spec.editions.push_back(EditionSpec{1, true, {c}});

  auto io = MakeSource(BuildFile(spec), "negative_tc.mkv");
  std::vector<std::shared_ptr<libebml::IOCallback>> sources{io};
  std::unique_ptr<Reader> reader;
  TASK_CHECK(Reader::Open(sources, 0, 1, 71, reader));
  Reader::Packet packet;
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.kind == Reader::Kind::Config);
  TASK_CHECK(packet.source_ns == 10000000);
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.source_ns == 10000000);
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.source_ns == 11000000);
  return 0;
}

static int CheckDurationPreferredOverDefault()
{
  // Two frames: one with BlockDuration (2 units of 1ms) and one falling back
  // to DefaultDuration.
  FileSpec spec;
  spec.source.segment_uid = MakeUID(0x66);
  spec.source.timecode_scale = 1000000;
  TrackSpec t;
  t.number = 1;
  t.uid = 71;
  t.codec_private = {1};
  t.default_duration_ns = 500000;
  spec.source.tracks = {t};
  spec.source.frames = {
      {true, 0, Bytes{1}, 0, true, true, 2, false, 0},
      {true, 2000000, Bytes{2}, 0, false, false, 0, false, 0},
  };
  spec.has_chapters = true;
  ChapterSpec c;
  c.uid = 1;
  c.begin_ns = 0;
  c.end_ns = 2500000;
  spec.editions.push_back(EditionSpec{1, true, {c}});

  auto io = MakeSource(BuildFile(spec), "duration.mkv");
  std::vector<std::shared_ptr<libebml::IOCallback>> sources{io};
  std::unique_ptr<Reader> reader;
  TASK_CHECK(Reader::Open(sources, 0, 1, 71, reader));
  Reader::Packet packet;
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);  // config
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.duration_ns == 2000000);
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.duration_ns == 500000);
  return 0;
}

static int CheckZeroReferenceIsNonKeyframe()
{
  // A BlockGroup with a ReferenceBlock whose value is 0 is still a reference.
  FileSpec spec;
  spec.source.segment_uid = MakeUID(0x88);
  spec.source.timecode_scale = 1000000;
  TrackSpec t;
  t.number = 1;
  t.uid = 71;
  t.codec_private = {1};
  t.default_duration_ns = 1000000;
  spec.source.tracks = {t};
  spec.source.frames = {
      {true, 0, Bytes{1}, 0, false, false, 0, false, 0},
      {false, 1000000, Bytes{2}, 0, true, true, 1, true, 0, false, 0},
  };
  spec.has_chapters = true;
  ChapterSpec c;
  c.uid = 1;
  c.begin_ns = 0;
  c.end_ns = 2000000;
  spec.editions.push_back(EditionSpec{1, true, {c}});

  auto io = MakeSource(BuildFile(spec), "zero_ref.mkv");
  std::vector<std::shared_ptr<libebml::IOCallback>> sources{io};
  std::unique_ptr<Reader> reader;
  TASK_CHECK(Reader::Open(sources, 0, 1, 71, reader));
  Reader::Packet packet;
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);  // config
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(packet.keyframe);
  TASK_CHECK(reader->ReadNext(packet) == Reader::Status::Ok);
  TASK_CHECK(!packet.keyframe);
  return 0;
}

// Table of registered cases.
const struct task_case task_cases[] = {
    {"linking", "main_event_sequence", CheckMainEventSequence},
    {"linking", "codec_private_republish", CheckCodecPrivateRepublish},
    {"linking", "open_rejects_missing_uid", CheckOpenRejectsMissingUid},
    {"linking", "ambiguous_segment_uid", CheckAmbiguousSegmentUid},
    {"linking", "disabled_chapters_keep_index", CheckDisabledChaptersSkipButKeepIndex},
    {"linking", "non_ordered_edition_rejected", CheckRejectsNonOrderedEdition},
    {"linking", "unsupported_features_rejected", CheckUnsupportedFeaturesRejected},
    {"linking", "chapter_boundaries", CheckChapterBoundaries},
    {"linking", "track_selection_rules", CheckTrackSelectionRules},
    {"linking", "mixed_track_frames_skipped", CheckMixedTrackFramesSkipped},
    {"linking", "codec_mismatch_rejected", CheckCodecMismatchRejected},
    {"linking", "empty_edition", CheckEmptyEdition},
    {"linking", "negative_local_timecode", CheckNegativeLocalTimecode},
    {"linking", "duration_preferred", CheckDurationPreferredOverDefault},
    {"linking", "zero_reference_non_keyframe", CheckZeroReferenceIsNonKeyframe},
    {"seek", "intra_frame", CheckSeekIntraFrame},
    {"seek", "across_chapters", CheckSeekAcrossChapters},
    {"seek", "bounds_and_end", CheckSeekBounds},
    {"seek", "chapter_start", CheckSeekToChapterStart},
    {"seek", "hidden_chapter", CheckSeekHiddenChapter},
    {"seek", "edge_positions", CheckEdgeSeekPositions},
    {"io", "open_reads_little", CheckOpenDoesNotReadPayloadTail},
    {"io", "open_skips_large_payload", CheckOpenSkipsLargePayload},
    {"io", "failure_retry", CheckIoFailureRetry},
    {"io", "failure_at_chapter_transition", CheckIoFailureOnChapterTransition},
    {"io", "failure_on_seek_replay", CheckIoFailureOnSeekReplay},
    {"io", "no_shared_buffer", CheckNoSharedBuffer},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
