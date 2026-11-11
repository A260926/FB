#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include "mkvmuxer/mkvmuxer.h"
#include "mkvparser/mkvparser.h"

static void Check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
class MemoryWriter : public mkvmuxer::IMkvWriter {
 public:
  std::vector<unsigned char> bytes;
  mkvmuxer::int32 Write(const void* data, mkvmuxer::uint32 size) override {
    if (pos_ + size > bytes.size()) bytes.resize(pos_ + size);
    std::memcpy(bytes.data() + pos_, data, size);
    pos_ += size;
    return 0;
  }
  mkvmuxer::int64 Position() const override { return pos_; }
  mkvmuxer::int32 Position(mkvmuxer::int64 pos) override {
    if (pos < 0) return -1;
    pos_ = static_cast<size_t>(pos);
    return 0;
  }
  bool Seekable() const override { return true; }
  void ElementStartNotify(mkvmuxer::uint64, mkvmuxer::int64) override {}
 private:
  size_t pos_ = 0;
};
class MemoryReader : public mkvparser::IMkvReader {
 public:
  explicit MemoryReader(const std::vector<unsigned char>& bytes) : bytes_(bytes) {}
  int Read(long long pos, long len, unsigned char* out) override {
    if (pos < 0 || len < 0 || static_cast<unsigned long long>(pos) > bytes_.size() ||
        static_cast<unsigned long long>(len) > bytes_.size() - pos) return -1;
    std::memcpy(out, bytes_.data() + pos, len);
    return 0;
  }
  int Length(long long* total, long long* available) override {
    *total = *available = bytes_.size(); return 0;
  }
 private:
  const std::vector<unsigned char>& bytes_;
};
struct Input {
  unsigned track;
  unsigned ms;
  std::vector<unsigned char> bytes;
  bool operator==(const Input& other) const {
    return track == other.track && ms == other.ms && bytes == other.bytes;
  }
};
static bool Order(const Input& a, const Input& b) {
  return a.ms == b.ms ? a.track < b.track : a.ms < b.ms;
}
static void Run(bool video) {
  const unsigned long long ms = 1000000;
  MemoryWriter writer;
  mkvmuxer::Segment mux;
  Check(mux.Init(&writer), "Segment Init");
  Check(mux.AddAudioTrack(48000, 2, 1) == 1, "audio track");
  mkvmuxer::Track* audio = mux.GetTrackByNumber(1);
  audio->set_default_duration(20 * ms);
  audio->set_uid(1);
  if (video) {
    Check(mux.AddVideoTrack(16, 16, 2) == 2, "video track");
    mux.GetTrackByNumber(2)->set_uid(2);
  }
  std::vector<Input> input;
  const size_t sizes[] = {1, 254, 255, 256, 510, 2, 3};
  for (unsigned i = 0; i < 7; ++i) {
    if (video && i % 2 == 0) input.push_back({2, i * 20, {0x56, static_cast<unsigned char>(i)}});
    std::vector<unsigned char> bytes(sizes[i]);
    for (size_t j = 0; j < bytes.size(); ++j) bytes[j] = static_cast<unsigned char>(i * 37 + j);
    input.push_back({1, i * 20, bytes});
  }
  for (const Input& in : input) {
    Check(mux.AddFrame(in.bytes.data(), in.bytes.size(), in.track, in.ms * ms, true), "AddFrame");
  }
  Check(mux.Finalize(), "Finalize");
  MemoryReader reader(writer.bytes);
  mkvparser::EBMLHeader header;
  long long pos = 0;
  Check(header.Parse(&reader, pos) == 0, "EBML header");
  mkvparser::Segment* raw = nullptr;
  Check(mkvparser::Segment::CreateInstance(&reader, pos, raw) == 0, "parser create");
  std::unique_ptr<mkvparser::Segment> parsed(raw);
  Check(parsed->Load() == 0, "parser load");
  Check(parsed->GetTracks()->GetTrackByNumber(1)->GetDefaultDuration() == 20 * ms, "default duration");
  std::vector<Input> output;
  int blocks = 0, laced = 0, clusters = 0;
  for (const mkvparser::Cluster* c = parsed->GetFirst(); c && !c->EOS(); c = parsed->GetNext(c)) {
    ++clusters;
    const mkvparser::BlockEntry* e = nullptr;
    Check(c->GetFirst(e) == 0, "first block");
    for (; e && !e->EOS();) {
      const mkvparser::Block* b = e->GetBlock();
      ++blocks;
      laced += b->GetLacing() != mkvparser::Block::kLacingNone;
      Check(b->GetFrameCount() == 1, "baseline expected one frame per block");
      const mkvparser::Block::Frame& f = b->GetFrame(0);
      std::vector<unsigned char> bytes(f.len);
      Check(f.Read(&reader, bytes.data()) == 0, "read frame");
      output.push_back({static_cast<unsigned>(b->GetTrackNumber()), static_cast<unsigned>(b->GetTime(c) / ms), bytes});
      const mkvparser::BlockEntry* next = nullptr;
      Check(c->GetNext(e, next) == 0, "next block");
      e = next;
    }
  }
  std::sort(input.begin(), input.end(), Order);
  std::sort(output.begin(), output.end(), Order);
  Check(input == output, "frame bytes/track/timestamp mismatch");
  Check(blocks == static_cast<int>(input.size()) && laced == 0, "baseline block counts");
  std::printf("BASELINE %s frames=%zu blocks=%d laced=%d clusters=%d bytes_preserved=yes timestamps_preserved=yes\n",
              video ? "av" : "audio", output.size(), blocks, laced, clusters);
}
int main() {
  try { Run(false); Run(true); return 0; }
  catch (const std::exception& e) { std::fprintf(stderr, "BASELINE FAIL: %s\n", e.what()); return 1; }
}
