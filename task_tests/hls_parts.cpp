// Regression tests for optional HLS partial segments (HLSPartPublisher).
//
// Every check runs through the real MPEG2TransportStreamMultiplexor output
// and the real HLSSegmenter file-writing callbacks.  PES buffers and their
// SCR/PCR values are hand-built here; no TS clock is re-parsed by the
// production code.  Part callbacks additionally open the advertised file
// range and read the bytes they publish.

#include "task_support/check.h"

#include "HLSSegmenter.hh"
#include "MPEG2TransportStreamMultiplexor.hh"
#include "BasicUsageEnvironment.hh"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr unsigned TS_SIZE = 188;
constexpr uint64_t PCR_MOD = HLSPartPublisher::PCR_MODULUS;

// ---------- small byte/PCR builders ----------

std::vector<unsigned char> makePES(uint8_t streamId, unsigned size, uint32_t seed) {
  std::vector<unsigned char> p(size, 0);
  if (size >= 6) {
    p[0] = 0x00; p[1] = 0x00; p[2] = 0x01;
    p[3] = streamId;
    unsigned pesLength = size - 6;
    p[4] = (unsigned char)((pesLength >> 8) & 0xFF);
    p[5] = (unsigned char)(pesLength & 0xFF);
    if (size >= 9) {
      p[6] = 0x80; // '10' marker bits, MPEG-2 PES
      p[7] = 0x80; // PTS_DTS_flags = PTS only
      p[8] = 5;    // PES_header_data_length
    }
  }
  // Deterministic, non-trivial payload:
  for (unsigned i = 9; i < size; ++i) {
    p[i] = (unsigned char)((seed * 1103515245u + i * 12345u + 7u) >> 16);
  }
  return p;
}

// Build an SCR from an unwrapped 27 MHz time.  The 33-bit base / 9-bit
// extension split is done here, so callers can script across the wrap.
MPEG1or2Demux::SCR makeSCR(uint64_t unwrappedTicks, Boolean valid = True) {
  uint64_t raw = unwrappedTicks % PCR_MOD;
  uint64_t base = raw / 300; // 33 bits
  MPEG1or2Demux::SCR scr;
  scr.highBit = (uint8_t)((base >> 32) & 1u);
  scr.remainingBits = (uint32_t)(base & 0xFFFFFFFFu);
  scr.extension = (uint16_t)(raw % 300);
  scr.isValid = valid;
  return scr;
}

uint64_t usTicks(uint64_t us) { return us * 27; }

unsigned tsPID(unsigned char const* pkt) {
  return unsigned(((pkt[1] & 0x1F) << 8) | pkt[2]);
}

// Return the elementary-stream payload bytes of one 188-byte TS packet,
// correctly skipping any adaptation field.  Empty if payload-only absent.
std::vector<unsigned char> tsPayload(unsigned char const* pkt) {
  unsigned afc = (pkt[3] >> 4) & 0x3;
  unsigned start = 4;
  if (afc == 0x2 || afc == 0x3) {
    unsigned afl = pkt[4];
    start = 5 + afl;
  }
  if (afc == 0x2 || start > TS_SIZE) return {}; // adaptation only / malformed
  return std::vector<unsigned char>(pkt + start, pkt + TS_SIZE);
}

std::vector<unsigned char> readFileBytes(std::string const& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (f == NULL) return {};
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<unsigned char> out(n > 0 ? size_t(n) : 0);
  if (n > 0) {
    size_t rd = fread(out.data(), 1, out.size(), f);
    out.resize(rd);
  }
  fclose(f);
  return out;
}

// ---------- recording callbacks ----------

struct Recorder {
  std::vector<HLSPartInfo> parts;
  std::vector<std::pair<std::string, double>> segments;
  std::map<unsigned, uint64_t> nextOffset;

  unsigned openErrors = 0, rangeErrors = 0, readErrors = 0;
  unsigned alignmentErrors = 0, emptyErrors = 0, syncErrors = 0;
  unsigned continuityErrors = 0;
  unsigned checkedRanges = 0;

  void onPart(HLSPartInfo const& info) {
    parts.push_back(info); // owns its own std::string file name

    uint64_t& expected = nextOffset[info.segmentNumber]; // inserts 0 first time
    if (info.offset != expected) ++continuityErrors;
    expected = info.offset + info.length;

    if (info.offset % TS_SIZE != 0 || info.length % TS_SIZE != 0) ++alignmentErrors;
    if (info.length == 0) ++emptyErrors;

    FILE* f = fopen(info.fileName.c_str(), "rb");
    if (f == NULL) { ++openErrors; return; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (uint64_t(size < 0 ? 0 : size) < info.offset + info.length) ++rangeErrors;

    if (fseek(f, long(info.offset), SEEK_SET) == 0 && info.length > 0) {
      std::vector<unsigned char> buf(size_t(info.length));
      size_t rd = fread(buf.data(), 1, buf.size(), f);
      if (rd != buf.size()) ++readErrors;
      for (size_t i = 0; i + TS_SIZE <= buf.size(); i += TS_SIZE) {
        if (buf[i] != 0x47) ++syncErrors;
      }
    }
    fclose(f);
    ++checkedRanges;
  }

  static void partTrampoline(void* cd, HLSPartInfo const& info) {
    ((Recorder*)cd)->onPart(info);
  }

  static void segmentTrampoline(void* cd, char const* name, double d) {
    Recorder* r = (Recorder*)cd;
    r->segments.emplace_back(name != NULL ? name : "", d);
  }

  unsigned totalByteErrors() const {
    return openErrors + rangeErrors + readErrors + alignmentErrors
      + emptyErrors + syncErrors + continuityErrors;
  }
};

// ---------- scripted multiplexor source ----------

struct Action {
  std::vector<unsigned char> pes;
  MPEG1or2Demux::SCR scr;
  int mpegVersion = 5;
  enum Kind { BUFFER, CLOSE, BLOCK } kind = BUFFER;
};

Action videoAction(uint64_t ticks, unsigned pesSize = 220, uint32_t seed = 1,
                   Boolean valid = True) {
  Action a;
  a.pes = makePES(0xE0, pesSize, seed);
  a.scr = makeSCR(ticks, valid);
  a.kind = Action::BUFFER;
  return a;
}

Action audioAction(uint32_t seed = 9) {
  Action a;
  a.pes = makePES(0xC0, 170, seed);
  a.scr = makeSCR(0, False); // audio does not carry the clock
  a.mpegVersion = 1;
  a.kind = Action::BUFFER;
  return a;
}

class ScriptedSource: public MPEG2TransportStreamMultiplexor {
public:
  std::vector<Action> script;
  size_t pos = 0;
  bool blocked = false;

  ScriptedSource(UsageEnvironment& env): MPEG2TransportStreamMultiplexor(env) {}

protected:
  virtual void awaitNewBuffer(unsigned char* /*oldBuffer*/) override {
    if (pos >= script.size()) {
      FramedSource::handleClosure(this);
      return;
    }
    Action const& a = script[pos];
    ++pos;
    if (a.kind == Action::CLOSE) {
      FramedSource::handleClosure(this);
      return;
    }
    if (a.kind == Action::BLOCK) {
      blocked = true;
      return; // behave like a live source waiting for more input
    }
    // "a" lives in the stable script vector, which is not mutated while playing.
    handleNewBuffer(const_cast<unsigned char*>(a.pes.data()),
                    unsigned(a.pes.size()), a.mpegVersion, a.scr);
  }
};

// ---------- fault-injectable stdio file ops ----------

struct FaultState {
  int opens = 0, writes = 0, flushes = 0, closes = 0;
  int failWriteCall = -1;   // 1-indexed write call to make short
  int failFlushCall = -1;   // 1-indexed flush call to fail
  int failCloseCall = -1;   // 1-indexed close call to fail
  bool failWriteAfterFlush = false;
  bool writeFailed = false;
};

FILE* faultOpen(void* cd, char const* path, char const* mode) {
  FaultState* fs = (FaultState*)cd;
  ++fs->opens;
  return fopen(path, mode);
}
size_t faultWrite(void* cd, void const* ptr, size_t size, size_t nmemb, FILE* f) {
  FaultState* fs = (FaultState*)cd;
  ++fs->writes;
  bool failThis = (fs->writes == fs->failWriteCall)
    || (fs->failWriteAfterFlush && fs->flushes > 0 && !fs->writeFailed);
  if (failThis) {
    fs->writeFailed = true;
    // Actually push some bytes, then report a short nmemb result:
    if (nmemb > 1) (void)fwrite(ptr, size, nmemb / 2, f);
    return nmemb > 0 ? nmemb - 1 : 0;
  }
  return fwrite(ptr, size, nmemb, f);
}
int faultFlush(void* cd, FILE* f) {
  FaultState* fs = (FaultState*)cd;
  ++fs->flushes;
  if (fs->flushes == fs->failFlushCall) return EOF;
  return fflush(f);
}
int faultClose(void* cd, FILE* f) {
  FaultState* fs = (FaultState*)cd;
  ++fs->closes;
  int real = fclose(f); // always really close, to avoid an fd leak
  if (fs->closes == fs->failCloseCall) return EOF;
  return real;
}

HLSFileOps faultOps(FaultState& fs) {
  return HLSFileOps{&fs, faultOpen, faultWrite, faultFlush, faultClose};
}

// ---------- per-case harness ----------

struct Harness {
  TaskScheduler* sched;
  UsageEnvironment* env;
  Recorder rec;
  EventLoopWatchVariable done{0};
  TaskToken watchdogToken = NULL;
  bool timedOut = false;
  int afterCount = 0;
  std::string dir, prefix;
  HLSSegmenter* seg = nullptr;
  ScriptedSource* src = nullptr;

  explicit Harness(char const* name) {
    sched = BasicTaskScheduler::createNew();
    env = BasicUsageEnvironment::createNew(*sched);
    dir = std::string(".task-build/cases/") + name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    prefix = dir + "/seg-";
  }

  ~Harness() {
    if (watchdogToken != NULL) sched->unscheduleDelayedTask(watchdogToken);
    if (seg != nullptr) Medium::close(seg);
    if (src != nullptr) Medium::close(src);
    env->reclaim();
    delete sched;
  }

  static void afterPlaying(void* cd) {
    Harness* h = (Harness*)cd;
    ++h->afterCount;
    h->done = 1;
  }

  static void watchdogProc(void* cd) {
    Harness* h = (Harness*)cd;
    h->timedOut = true;
    h->done = 1;
  }

  void play(unsigned segmentationDuration = 10) {
    seg = HLSSegmenter::createNew(*env, segmentationDuration, prefix.c_str(),
                                  &Recorder::segmentTrampoline, &rec);
    src = new ScriptedSource(*env);
  }

  void start() {
    Boolean started = seg->startPlaying(*src, &Harness::afterPlaying, this);
    if (!started) done = 1;
  }

  void run() {
    watchdogToken = sched->scheduleDelayedTask(
      8000000, (TaskFunc*)&Harness::watchdogProc, this);
    sched->doEventLoop(&done);
  }

  static void setWatchTask(void* cd) {
    ((EventLoopWatchVariable*)cd)->store(1);
  }

  // Drain currently pending zero-delay tasks (e.g. deferred failure tasks)
  // without allowing the production chain to make new progress.
  void pump(int microseconds = 3000) {
    EventLoopWatchVariable pumpDone{0};
    TaskToken tok = sched->scheduleDelayedTask(
      microseconds, (TaskFunc*)&Harness::setWatchTask, &pumpDone);
    sched->doEventLoop(&pumpDone);
    sched->unscheduleDelayedTask(tok);
  }
};

// ---------- cross-check helpers ----------

std::string segmentPath(Harness const& h, unsigned number) {
  char buf[64];
  snprintf(buf, sizeof(buf), "seg-%03u.ts", number);
  return h.dir + "/" + buf;
}

// Validate one whole segment file against the events recorded for it.
// Returns an empty string on success, otherwise an error description.
std::string validateSegment(Harness const& h, unsigned segNumber,
                            unsigned expectedParts,
                            uint64_t expectedWholeBytes = 0) {
  std::vector<HLSPartInfo const*> ev;
  for (auto const& p : h.rec.parts) {
    if (p.segmentNumber == segNumber) ev.push_back(&p);
  }
  if (ev.size() != expectedParts)
    return "part count mismatch in segment " + std::to_string(segNumber);

  uint64_t cursor = 0;
  for (unsigned i = 0; i < ev.size(); ++i) {
    if (ev[i]->partNumber != i) return "part numbering not contiguous";
    if (ev[i]->offset != cursor) return "part offsets not contiguous";
    if (ev[i]->length == 0 || ev[i]->length % TS_SIZE != 0)
      return "bad part length/alignment";
    cursor += ev[i]->length;
  }
  if (expectedWholeBytes != 0 && cursor != expectedWholeBytes)
    return "part bytes do not sum to expected whole size";

  std::string path = segmentPath(h, segNumber);
  std::vector<unsigned char> bytes = readFileBytes(path);
  if (bytes.empty()) return "segment file missing/empty: " + path;
  if (cursor != bytes.size()) return "coverage does not equal file size";
  if (expectedWholeBytes != 0 && bytes.size() != expectedWholeBytes)
    return "unexpected whole byte size";

  // The segment must begin with its own PAT then PMT:
  if (bytes.size() < 2 * TS_SIZE) return "too few TS packets for PAT/PMT";
  if (bytes[0] != 0x47 || tsPID(bytes.data()) != 0x0000) return "first packet is not PAT";
  if (tsPID(bytes.data() + TS_SIZE) != 0x1000) return "second packet is not PMT";

  // Every TS packet in the complete file has sync 0x47:
  for (size_t i = 0; i + TS_SIZE <= bytes.size(); i += TS_SIZE) {
    if (bytes[i] != 0x47) return "TS sync byte broken";
  }
  return "";
}

// Reassemble all payload bytes of one PID across one segment file.
std::vector<unsigned char> reassemblePID(Harness const& h, unsigned segNumber, unsigned pid) {
  std::vector<unsigned char> bytes = readFileBytes(segmentPath(h, segNumber));
  std::vector<unsigned char> out;
  for (size_t i = 0; i + TS_SIZE <= bytes.size(); i += TS_SIZE) {
    unsigned char const* pkt = bytes.data() + i;
    if (tsPID(pkt) != pid) continue;
    std::vector<unsigned char> pl = tsPayload(pkt);
    out.insert(out.end(), pl.begin(), pl.end());
  }
  return out;
}

// ---------- stopPlaying tick helper (stop happens outside callbacks) ----------

struct StopTick {
  Harness* h;
  int tries = 0;
  TaskToken token = NULL;

  static void proc(void* cd) {
    StopTick* t = (StopTick*)cd;
    Harness* h = t->h;
    if (h->src != nullptr && h->src->blocked && h->rec.parts.size() >= 1) {
      h->seg->stopPlaying();
      h->done = 1;
      return; // do not reschedule
    }
    if (++t->tries > 5000) { // ~10 s at 2 ms
      h->timedOut = true;
      h->done = 1;
      return;
    }
    t->token = h->sched->scheduleDelayedTask(2000, (TaskFunc*)&StopTick::proc, t);
  }

  void arm() { token = h->sched->scheduleDelayedTask(0, (TaskFunc*)&StopTick::proc, this); }
};

} // namespace

// =====================================================================
// clock group
// =====================================================================

static int clock_wrap_accumulates() {
  Harness h("clock-wrap-accumulates");
  // First PCR sits 200 us before the 27 MHz wrap point.
  uint64_t t0 = PCR_MOD - usTicks(200);
  h.src = new ScriptedSource(*h.env);
  h.src->script = {
    videoAction(t0, 200, 1),
    videoAction(t0 + usTicks(100000), 200, 2),
    videoAction(t0 + usTicks(200000), 200, 3),
    videoAction(t0 + usTicks(300000), 200, 4),
  };
  h.seg = HLSSegmenter::createNew(*h.env, 10, h.prefix.c_str(),
                                  &Recorder::segmentTrampoline, &h.rec);
  TASK_CHECK(h.seg->enablePartialSegments(100000, &Recorder::partTrampoline, &h.rec));
  h.seg->startPlaying(*h.src, &Harness::afterPlaying, &h);
  h.run();

  TASK_CHECK(!h.timedOut);
  TASK_CHECK(h.rec.parts.size() == 4);
  uint64_t expected[] = {100000, 100000, 100000, 0};
  for (int i = 0; i < 4; ++i) {
    TASK_CHECK(h.rec.parts[i].durationUS == expected[i]);
    TASK_CHECK(h.rec.parts[i].segmentNumber == 1);
    TASK_CHECK(h.rec.parts[i].partNumber == unsigned(i));
  }
  TASK_CHECK(h.rec.totalByteErrors() == 0);
  TASK_CHECK(h.rec.checkedRanges == 4);
  return 0;
}

static int clock_zero_base_is_origin() {
  Harness h("clock-zero-base");
  h.src = new ScriptedSource(*h.env);
  h.src->script = {
    videoAction(0, 200, 11),                       // all-zero valid PCR
    videoAction(usTicks(50000), 200, 12),          // below 100 ms target
    videoAction(usTicks(150000), 200, 13),         // crosses once, big span
  };
  h.seg = HLSSegmenter::createNew(*h.env, 10, h.prefix.c_str(),
                                  &Recorder::segmentTrampoline, &h.rec);
  TASK_CHECK(h.seg->enablePartialSegments(100000, &Recorder::partTrampoline, &h.rec));
  h.seg->startPlaying(*h.src, &Harness::afterPlaying, &h);
  h.run();

  TASK_CHECK(!h.timedOut);
  // One boundary part (150 ms, ended only once despite the overshoot),
  // then a zero-duration EOF tail starting at the boundary PCR.
  TASK_CHECK(h.rec.parts.size() == 2);
  TASK_CHECK(h.rec.parts[0].durationUS == 150000);
  TASK_CHECK(h.rec.parts[1].durationUS == 0);
  TASK_CHECK(h.rec.totalByteErrors() == 0);
  return 0;
}

static int clock_invalid_scr_ignored() {
  Harness h("clock-invalid-scr");
  MPEG1or2Demux::SCR garbage = makeSCR(PCR_MOD / 3, False);
  Action bad;
  bad.pes = makePES(0xE0, 200, 21);
  bad.scr = garbage;
  h.src = new ScriptedSource(*h.env);
  h.src->script = {
    videoAction(0, 200, 20),
    bad,                                              // isValid == False
    videoAction(usTicks(100000), 200, 22),
  };
  h.seg = HLSSegmenter::createNew(*h.env, 10, h.prefix.c_str(),
                                  &Recorder::segmentTrampoline, &h.rec);
  TASK_CHECK(h.seg->enablePartialSegments(100000, &Recorder::partTrampoline, &h.rec));
  h.seg->startPlaying(*h.src, &Harness::afterPlaying, &h);
  h.run();

  TASK_CHECK(!h.timedOut);
  TASK_CHECK(h.rec.parts.size() == 2);
  TASK_CHECK(h.rec.parts[0].durationUS == 100000);
  TASK_CHECK(h.rec.parts[1].durationUS == 0);
  TASK_CHECK(h.rec.totalByteErrors() == 0);
  return 0;
}

static int clock_floors_to_microseconds() {
  Harness h("clock-floor");
  uint64_t delta = usTicks(100) + 13; // 100 us plus 13/27 us remainder
  h.src = new ScriptedSource(*h.env);
  h.src->script = {
    videoAction(0, 200, 31),
    videoAction(delta, 200, 32), // target 100 us -> boundary; floor must be 100
  };
  h.seg = HLSSegmenter::createNew(*h.env, 10, h.prefix.c_str(),
                                  &Recorder::segmentTrampoline, &h.rec);
  TASK_CHECK(h.seg->enablePartialSegments(100, &Recorder::partTrampoline, &h.rec));
  h.seg->startPlaying(*h.src, &Harness::afterPlaying, &h);
  h.run();

  TASK_CHECK(!h.timedOut);
  TASK_CHECK(h.rec.parts.size() == 2);
  TASK_CHECK(h.rec.parts[0].durationUS == 100);
  TASK_CHECK(h.rec.parts[1].durationUS == 0);
  return 0;
}

static int clock_no_valid_pcr_gives_zero_tail() {
  Harness h("clock-no-valid-pcr");
  Action noPCR;
  noPCR.pes = makePES(0xE0, 220, 35);
  noPCR.scr = makeSCR(usTicks(1234567), False); // present but invalid
  h.src = new ScriptedSource(*h.env);
  h.src->script = { noPCR };
  h.seg = HLSSegmenter::createNew(*h.env, 10, h.prefix.c_str(),
                                  &Recorder::segmentTrampoline, &h.rec);
  TASK_CHECK(h.seg->enablePartialSegments(100000, &Recorder::partTrampoline, &h.rec));
  h.seg->startPlaying(*h.src, &Harness::afterPlaying, &h);
  h.run();

  TASK_CHECK(!h.timedOut);
  TASK_CHECK(h.rec.parts.size() == 1); // non-empty tail is still published
  TASK_CHECK(h.rec.parts[0].durationUS == 0);
  TASK_CHECK(h.rec.parts[0].length > 0);
  TASK_CHECK(h.rec.segments.size() == 1);
  TASK_CHECK(h.rec.totalByteErrors() == 0);
  return 0;
}

// =====================================================================
// ranges group
// =====================================================================

static int ranges_contiguous_whole_boundary() {
  Harness h("ranges-contiguous");
  // 250 ms parts, 1 s whole segments; PCR exactly on every boundary.
  h.src = new ScriptedSource(*h.env);
  h.src->script = {
    videoAction(usTicks(0),       210, 41),
    videoAction(usTicks(250000),  210, 42),
    videoAction(usTicks(500000),  210, 43),
    videoAction(usTicks(750000),  210, 44),
    videoAction(usTicks(1000000), 210, 45), // part+whole due at once
    videoAction(usTicks(1250000), 210, 46),
  };
  h.seg = HLSSegmenter::createNew(*h.env, 1, h.prefix.c_str(),
                                  &Recorder::segmentTrampoline, &h.rec);
  TASK_CHECK(h.seg->enablePartialSegments(250000, &Recorder::partTrampoline, &h.rec));
  h.seg->startPlaying(*h.src, &Harness::afterPlaying, &h);
  h.run();

  TASK_CHECK(!h.timedOut);
  // Segment 1: four 250 ms parts.  Segment 2: one 250 ms part + zero tail.
  TASK_CHECK(h.rec.parts.size() == 6);
  TASK_CHECK(validateSegment(h, 1, 4).empty());
  TASK_CHECK(validateSegment(h, 2, 2).empty());

  for (unsigned i = 0; i < 4; ++i) TASK_CHECK(h.rec.parts[i].durationUS == 250000);
  TASK_CHECK(h.rec.parts[4].segmentNumber == 2);
  TASK_CHECK(h.rec.parts[4].partNumber == 0);
  TASK_CHECK(h.rec.parts[4].durationUS == 250000);
  TASK_CHECK(h.rec.parts[5].durationUS == 0);

  // Exactly one whole-segment callback at the 1 s boundary plus one at EOF.
  TASK_CHECK(h.rec.segments.size() == 2);
  TASK_CHECK(std::abs(h.rec.segments[0].second - 1.0) < 1e-9);
  TASK_CHECK(std::abs(h.rec.segments[1].second - 0.25) < 1e-9);
  TASK_CHECK(h.rec.totalByteErrors() == 0);
  return 0;
}

static int ranges_audio_interleaved() {
  Harness h("ranges-audio");
  h.src = new ScriptedSource(*h.env);
  h.src->script = {
    videoAction(0, 200, 51),
    audioAction(52),
    videoAction(usTicks(100000), 200, 53),
    audioAction(54),
    videoAction(usTicks(200000), 200, 55),
    audioAction(56),
  };
  h.seg = HLSSegmenter::createNew(*h.env, 10, h.prefix.c_str(),
                                  &Recorder::segmentTrampoline, &h.rec);
  TASK_CHECK(h.seg->enablePartialSegments(100000, &Recorder::partTrampoline, &h.rec));
  h.seg->startPlaying(*h.src, &Harness::afterPlaying, &h);
  h.run();

  TASK_CHECK(!h.timedOut);
  TASK_CHECK(h.rec.parts.size() == 3); // 100 ms, 100 ms, zero tail
  TASK_CHECK(validateSegment(h, 1, 3).empty());
  TASK_CHECK(h.rec.totalByteErrors() == 0);

  // Audio PID 0x00C0 packets must actually be present in the file:
  std::vector<unsigned char> audio = reassemblePID(h, 1, 0x00C0);
  TASK_CHECK(audio.size() >= 3u * (170 - 9) / 2); // substantial audio payload
  bool hasC0Sync = false;
  std::vector<unsigned char> raw = readFileBytes(segmentPath(h, 1));
  for (size_t i = 0; i + TS_SIZE <= raw.size(); i += TS_SIZE) {
    if (tsPID(raw.data() + i) == 0x00C0) { hasC0Sync = true; break; }
  }
  TASK_CHECK(hasC0Sync);
  return 0;
}

static int ranges_pes_spans_many_packets() {
  Harness h("ranges-big-pes");
  // One ~2000 byte PES is ~11 TS packets; the cut at the next PCR PES start
  // must leave all of it inside the first part, never split mid-PES.
  std::vector<unsigned char> big0 = makePES(0xE0, 2000, 61);
  std::vector<unsigned char> big1 = makePES(0xE0, 1800, 62);
  h.src = new ScriptedSource(*h.env);
  h.src->script = {
    videoAction(0, 2000, 61),
    videoAction(usTicks(100000), 1800, 62),
  };
  (void)big0; (void)big1;
  h.seg = HLSSegmenter::createNew(*h.env, 10, h.prefix.c_str(),
                                  &Recorder::segmentTrampoline, &h.rec);
  TASK_CHECK(h.seg->enablePartialSegments(100000, &Recorder::partTrampoline, &h.rec));
  h.seg->startPlaying(*h.src, &Harness::afterPlaying, &h);
  h.run();

  TASK_CHECK(!h.timedOut);
  TASK_CHECK(h.rec.parts.size() == 2);
  TASK_CHECK(validateSegment(h, 1, 2).empty());
  TASK_CHECK(h.rec.totalByteErrors() == 0);

  // End-to-end payload reconstruction of the video PID must equal the two
  // source PES buffers concatenated (no bytes lost or moved to a new segment).
  std::vector<unsigned char> expected;
  for (auto const& a : h.src->script) expected.insert(expected.end(), a.pes.begin(), a.pes.end());
  std::vector<unsigned char> got = reassemblePID(h, 1, 0x00E0);
  TASK_CHECK(got == expected);
  return 0;
}

static int ranges_no_input_no_file() {
  Harness h("ranges-empty");
  h.src = new ScriptedSource(*h.env); // empty script -> immediate closure
  h.seg = HLSSegmenter::createNew(*h.env, 10, h.prefix.c_str(),
                                  &Recorder::segmentTrampoline, &h.rec);
  TASK_CHECK(h.seg->enablePartialSegments(100000, &Recorder::partTrampoline, &h.rec));
  h.seg->startPlaying(*h.src, &Harness::afterPlaying, &h);
  h.run();

  TASK_CHECK(!h.timedOut);
  TASK_CHECK(h.afterCount == 1);
  TASK_CHECK(h.rec.parts.empty());
  TASK_CHECK(h.rec.segments.empty());
  // No .ts artifact may have been created:
  unsigned tsFiles = 0;
  for (auto& e : std::filesystem::directory_iterator(h.dir)) {
    if (e.path().extension() == ".ts") ++tsFiles;
  }
  TASK_CHECK(tsFiles == 0);
  return 0;
}

// =====================================================================
// lifecycle group
// =====================================================================

static int lifecycle_eof_settles_once() {
  Harness h("life-eof");
  h.src = new ScriptedSource(*h.env);
  h.src->script = {
    videoAction(0, 200, 71),
    videoAction(usTicks(100000), 200, 72),
    videoAction(usTicks(200000), 200, 73),
  };
  h.seg = HLSSegmenter::createNew(*h.env, 10, h.prefix.c_str(),
                                  &Recorder::segmentTrampoline, &h.rec);
  TASK_CHECK(h.seg->enablePartialSegments(100000, &Recorder::partTrampoline, &h.rec));
  h.seg->startPlaying(*h.src, &Harness::afterPlaying, &h);
  h.run();

  TASK_CHECK(!h.timedOut);
  TASK_CHECK(h.afterCount == 1);
  // part at 100 ms, part at 200 ms, zero tail:
  TASK_CHECK(h.rec.parts.size() == 3);
  TASK_CHECK(h.rec.segments.size() == 1); // exactly one EOF whole callback
  TASK_CHECK(validateSegment(h, 1, 3).empty());
  TASK_CHECK(h.rec.totalByteErrors() == 0);

  // All part durations telescope to the EOF whole duration:
  uint64_t sumUS = 0;
  for (auto const& p : h.rec.parts) sumUS += p.durationUS;
  TASK_CHECK(sumUS == 200000);
  TASK_CHECK(std::abs(h.rec.segments[0].second - 0.2) < 1e-9);
  return 0;
}

static int lifecycle_abort_does_not_publish_tail() {
  Harness h("life-abort");
  FaultState fs;
  HLSFileOps ops = faultOps(fs); // counting stdio ops

  h.src = new ScriptedSource(*h.env);
  Action block; block.kind = Action::BLOCK;
  h.src->script = {
    videoAction(0, 200, 81),
    videoAction(usTicks(100000), 200, 82), // publishes part 0
    block,
  };
  h.seg = HLSSegmenter::createNew(*h.env, 10, h.prefix.c_str(),
                                  &Recorder::segmentTrampoline, &h.rec);
  TASK_CHECK(h.seg->enablePartialSegments(100000, &Recorder::partTrampoline,
                                          &h.rec, &ops));
  h.seg->startPlaying(*h.src, &Harness::afterPlaying, &h);

  StopTick tick{&h};
  tick.arm();
  h.run();

  TASK_CHECK(!h.timedOut);
  TASK_CHECK(h.src->blocked);
  TASK_CHECK(h.rec.parts.size() == 1);
  unsigned partsAtStop = unsigned(h.rec.parts.size());
  unsigned segsAtStop = unsigned(h.rec.segments.size());
  TASK_CHECK(segsAtStop == 0); // no whole completion for the aborted segment
  TASK_CHECK(fs.closes == 1);   // stopPlaying closed the handle exactly once

  // Let any pending deferred tasks run; nothing new may be published.
  h.pump();
  TASK_CHECK(h.rec.parts.size() == partsAtStop);
  TASK_CHECK(h.rec.segments.size() == segsAtStop);

  // Closing the object must not close the handle a second time.
  Medium::close(h.seg);
  h.seg = nullptr;
  TASK_CHECK(fs.closes == 1);
  return 0;
}

static int lifecycle_write_failure_stops_publication() {
  Harness h("life-write-fail");
  FaultState fs;
  fs.failWriteAfterFlush = true;
  HLSFileOps ops = faultOps(fs);

  h.src = new ScriptedSource(*h.env);
  h.src->script = {
    videoAction(0, 200, 91),
    videoAction(usTicks(100000), 200, 92), // flush+publish part 0, then writes fail
    videoAction(usTicks(200000), 200, 93),
  };
  h.seg = HLSSegmenter::createNew(*h.env, 10, h.prefix.c_str(),
                                  &Recorder::segmentTrampoline, &h.rec);
  TASK_CHECK(h.seg->enablePartialSegments(100000, &Recorder::partTrampoline,
                                          &h.rec, &ops));
  h.seg->startPlaying(*h.src, &Harness::afterPlaying, &h);
  h.run();

  TASK_CHECK(!h.timedOut);
  h.pump();
  TASK_CHECK(h.afterCount == 1); // standard closure notification exactly once
  TASK_CHECK(h.rec.parts.size() == 1); // already published part is retained
  TASK_CHECK(h.rec.segments.empty());  // failed segment gets no completion event
  TASK_CHECK(fs.writeFailed);
  TASK_CHECK(fs.closes == 1);          // failed handle closed exactly once
  TASK_CHECK(strlen(h.env->getResultMsg()) > 0); // error stays readable
  return 0;
}

static int lifecycle_flush_failure_publishes_nothing_failed() {
  Harness h("life-flush-fail");
  FaultState fs;
  fs.failFlushCall = 1; // the first part flush fails
  HLSFileOps ops = faultOps(fs);

  h.src = new ScriptedSource(*h.env);
  h.src->script = {
    videoAction(0, 200, 101),
    videoAction(usTicks(100000), 200, 102),
  };
  h.seg = HLSSegmenter::createNew(*h.env, 10, h.prefix.c_str(),
                                  &Recorder::segmentTrampoline, &h.rec);
  TASK_CHECK(h.seg->enablePartialSegments(100000, &Recorder::partTrampoline,
                                          &h.rec, &ops));
  h.seg->startPlaying(*h.src, &Harness::afterPlaying, &h);
  h.run();

  TASK_CHECK(!h.timedOut);
  h.pump();
  TASK_CHECK(h.afterCount == 1);
  TASK_CHECK(h.rec.parts.empty()); // failed range not advertised
  TASK_CHECK(h.rec.segments.empty());
  TASK_CHECK(fs.flushes >= 1);
  TASK_CHECK(fs.closes == 1);
  TASK_CHECK(strlen(h.env->getResultMsg()) > 0);
  return 0;
}

static int lifecycle_close_failure_suppresses_segment_event() {
  Harness h("life-close-fail");
  FaultState fs;
  fs.failCloseCall = 1; // EOF tail publishes, then close reports failure
  HLSFileOps ops = faultOps(fs);

  h.src = new ScriptedSource(*h.env);
  h.src->script = {
    videoAction(0, 200, 111),
    videoAction(usTicks(100000), 200, 112),
  };
  h.seg = HLSSegmenter::createNew(*h.env, 10, h.prefix.c_str(),
                                  &Recorder::segmentTrampoline, &h.rec);
  TASK_CHECK(h.seg->enablePartialSegments(100000, &Recorder::partTrampoline,
                                          &h.rec, &ops));
  h.seg->startPlaying(*h.src, &Harness::afterPlaying, &h);
  h.run();

  TASK_CHECK(!h.timedOut);
  h.pump();
  TASK_CHECK(h.afterCount == 1);
  // The tail part had a successful flush and was published before close:
  TASK_CHECK(h.rec.parts.size() == 2);
  TASK_CHECK(h.rec.segments.empty()); // close failed: no whole completion event
  TASK_CHECK(fs.closes == 1);
  TASK_CHECK(strlen(h.env->getResultMsg()) > 0);

  Medium::close(h.seg);
  h.seg = nullptr;
  TASK_CHECK(fs.closes == 1); // still exactly one close attempt
  return 0;
}

static int lifecycle_config_validation() {
  Harness h("life-config");
  HLSSegmenter* a = HLSSegmenter::createNew(*h.env, 2, h.prefix.c_str());
  TASK_CHECK(a->enablePartialSegments(0, &Recorder::partTrampoline, &h.rec) == False);
  TASK_CHECK(a->enablePartialSegments(500000, &Recorder::partTrampoline, &h.rec) == True);
  TASK_CHECK(a->enablePartialSegments(500000, &Recorder::partTrampoline, &h.rec) == False);
  Medium::close(a);

  // An ops table with a missing function is rejected and leaves state clean:
  HLSSegmenter* b = HLSSegmenter::createNew(*h.env, 2, h.prefix.c_str());
  HLSFileOps bad{nullptr, faultOpen, nullptr, faultFlush, faultClose};
  TASK_CHECK(b->enablePartialSegments(500000, &Recorder::partTrampoline,
                                      &h.rec, &bad) == False);
  TASK_CHECK(b->enablePartialSegments(500000, &Recorder::partTrampoline, &h.rec) == True);
  Medium::close(b);

  // part target larger than the whole segment is rejected without overflow:
  HLSSegmenter* c = HLSSegmenter::createNew(*h.env, 1, h.prefix.c_str());
  TASK_CHECK(c->enablePartialSegments(2000000, &Recorder::partTrampoline, &h.rec) == False);
  TASK_CHECK(c->enablePartialSegments(1000000, &Recorder::partTrampoline, &h.rec) == True);
  Medium::close(c);
  h.seg = nullptr; // harness destructor must not double-close
  return 0;
}

// ---------- registration ----------

const task_case task_cases[] = {
  {"clock", "wrap_accumulates", clock_wrap_accumulates},
  {"clock", "zero_base_is_origin", clock_zero_base_is_origin},
  {"clock", "invalid_scr_ignored", clock_invalid_scr_ignored},
  {"clock", "floors_to_microseconds", clock_floors_to_microseconds},
  {"clock", "no_valid_pcr_zero_tail", clock_no_valid_pcr_gives_zero_tail},

  {"ranges", "contiguous_whole_boundary", ranges_contiguous_whole_boundary},
  {"ranges", "audio_interleaved", ranges_audio_interleaved},
  {"ranges", "pes_spans_many_packets", ranges_pes_spans_many_packets},
  {"ranges", "no_input_no_file", ranges_no_input_no_file},

  {"lifecycle", "eof_settles_once", lifecycle_eof_settles_once},
  {"lifecycle", "abort_does_not_publish_tail", lifecycle_abort_does_not_publish_tail},
  {"lifecycle", "write_failure_stops_publication", lifecycle_write_failure_stops_publication},
  {"lifecycle", "flush_failure_no_failed_event", lifecycle_flush_failure_publishes_nothing_failed},
  {"lifecycle", "close_failure_no_segment_event", lifecycle_close_failure_suppresses_segment_event},
  {"lifecycle", "config_validation", lifecycle_config_validation},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
