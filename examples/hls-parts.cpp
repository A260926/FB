// Example: generate an in-memory PES stream, mux it into Transport Stream
// through the real MPEG2TransportStreamMultiplexor, and let HLSSegmenter
// publish finalized byte ranges ("parts") inside ordinary segment .ts files.
//
// Usage:
//   hls-parts <output-directory>
//
// The program prints one line per published part and per whole segment.
// A player can read each part as the byte range [offset, offset+length) of
// the named segment file; every event is emitted only after fflush(3).

#include "HLSSegmenter.hh"
#include "MPEG2TransportStreamMultiplexor.hh"
#include "BasicUsageEnvironment.hh"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr uint64_t PCR_MOD = HLSPartPublisher::PCR_MODULUS;

MPEG1or2Demux::SCR scrAtUS(uint64_t us) {
  uint64_t raw = (us * 27) % PCR_MOD;
  uint64_t base = raw / 300;
  MPEG1or2Demux::SCR scr;
  scr.highBit = uint8_t((base >> 32) & 1u);
  scr.remainingBits = uint32_t(base & 0xFFFFFFFFu);
  scr.extension = uint16_t(raw % 300);
  scr.isValid = True;
  return scr;
}

std::vector<unsigned char> makeVideoPES(unsigned size, uint32_t seed) {
  std::vector<unsigned char> pes(size, 0);
  pes[0] = 0x00; pes[1] = 0x00; pes[2] = 0x01; pes[3] = 0xE0;
  unsigned pesLength = size - 6;
  pes[4] = uint8_t(pesLength >> 8);
  pes[5] = uint8_t(pesLength & 0xFF);
  pes[6] = 0x80; pes[7] = 0x80; pes[8] = 5;
  for (unsigned i = 9; i < size; ++i) {
    pes[i] = uint8_t((seed * 2654435761u + i * 40503u) >> 16);
  }
  return pes;
}

struct MemorySource: MPEG2TransportStreamMultiplexor {
  struct Chunk { std::vector<unsigned char> pes; MPEG1or2Demux::SCR scr; };
  std::vector<Chunk> chunks;
  size_t next = 0;

  explicit MemorySource(UsageEnvironment& env)
    : MPEG2TransportStreamMultiplexor(env) {
    // 14 video PES frames, PCR every 200 ms -> 2.8 s of stream.
    for (unsigned i = 0; i < 14; ++i) {
      chunks.push_back({makeVideoPES(420 + (i % 3) * 130, 100 + i),
                        scrAtUS(uint64_t(i) * 200000)});
    }
  }

protected:
  virtual void awaitNewBuffer(unsigned char* /*oldBuffer*/) override {
    if (next >= chunks.size()) {
      FramedSource::handleClosure(this);
      return;
    }
    Chunk const& c = chunks[next++];
    handleNewBuffer(const_cast<unsigned char*>(c.pes.data()),
                    unsigned(c.pes.size()), 5, c.scr);
  }
};

EventLoopWatchVariable finished{0};

void onPart(void* /*clientData*/, HLSPartInfo const& info) {
  std::printf("PART segment=%u part=%u file=%s offset=%llu length=%llu durationUS=%llu\n",
              info.segmentNumber, info.partNumber, info.fileName.c_str(),
              (unsigned long long)info.offset,
              (unsigned long long)info.length,
              (unsigned long long)info.durationUS);
  std::fflush(stdout);
}

void onSegment(void* /*clientData*/, char const* fileName, double durationSeconds) {
  std::printf("SEGMENT file=%s duration=%.6f\n",
              fileName != nullptr ? fileName : "(null)", durationSeconds);
  std::fflush(stdout);
}

void afterPlaying(void* /*clientData*/) {
  finished = 1;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
    return 2;
  }

  std::error_code ec;
  std::filesystem::path outDir = argv[1];
  std::filesystem::create_directories(outDir, ec);

  // Clean this example's own old products so repeated runs in one directory
  // never mix stale ranges with the fresh output.
  for (auto const& entry : std::filesystem::directory_iterator(outDir, ec)) {
    std::string name = entry.path().filename().string();
    if (name.rfind("hls-part-", 0) == 0 && entry.path().extension() == ".ts") {
      std::filesystem::remove(entry.path(), ec);
    }
  }

  std::string prefix = (outDir / "hls-part-").string();

  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

  HLSSegmenter* segmenter
    = HLSSegmenter::createNew(*env, 1 /*whole segment seconds*/, prefix.c_str(),
                             &onSegment, nullptr);
  if (segmenter == nullptr) return 1;

  // 500 ms parts inside 1 s segments:
  if (!segmenter->enablePartialSegments(500000, &onPart, nullptr, nullptr)) {
    std::fprintf(stderr, "enablePartialSegments failed: %s\n",
                 env->getResultMsg());
    Medium::close(segmenter);
    env->reclaim();
    delete scheduler;
    return 1;
  }

  MemorySource* source = new MemorySource(*env);
  segmenter->startPlaying(*source, &afterPlaying, nullptr);
  scheduler->doEventLoop(&finished);

  Medium::close(segmenter);
  Medium::close(source);
  env->reclaim();
  delete scheduler;

  std::printf("DONE output=%s\n", outDir.string().c_str());
  return 0;
}
