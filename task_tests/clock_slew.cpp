// Task tests for the optional RTCP clock-slew mode of RTPSource.
// The tests synthesize RTP and RTCP SR bytes and drive them through the real
// RTPSource / MultiFramedRTPSource / H264VideoRTPSource receive chain over a
// local socketpair (interleaved TCP framing), without any sleeping.

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "../task_support/check.h"

#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <cstdio>
#include <cstring>
#include <stdexcept>

static const unsigned SSRC = 77;
static const unsigned RATE = 90000;
static const unsigned NTP_EPOCH_OFFSET = 0x83AA7E80u; // 1900 -> 1970

static void require(bool x) {
  if (!x) throw std::runtime_error("harness failure");
}

static void putWord(std::vector<unsigned char>& v, unsigned x) {
  for (int i = 3; i >= 0; --i) v.push_back((unsigned char)((x >> (8*i)) & 255));
}

// Smallest NTP fractional word whose floor-truncation back to microseconds
// yields exactly "microseconds" (the receiver truncates NTP fractions down).
static unsigned ntpFrac(unsigned microseconds) {
  return (unsigned)(((u_int64_t)microseconds << 26) / 15625u
		    + ((((u_int64_t)microseconds << 26) % 15625u) != 0));
}

// One received, reassembled frame.
struct Frame {
  EventLoopWatchVariable done{0};
  unsigned n{0};
  unsigned truncated{0};
  timeval tv{};
  unsigned char bytes[256]{};

  void reset() {
    done = 0;
    n = 0;
    truncated = 0;
    tv = timeval{};
    std::memset(bytes, 0, sizeof(bytes));
  }
};

static void afterGettingFrame(void* clientData, unsigned frameSize,
			       unsigned numTruncatedBytes,
			       struct timeval presentationTime,
			       unsigned /*durationInMicroseconds*/) {
  Frame* f = (Frame*)clientData;
  f->n = frameSize;
  f->truncated = numTruncatedBytes;
  f->tv = presentationTime;
  f->done = 1;
}

static void onWatchdog(void* clientData) {
  ((Frame*)clientData)->done = 2;
}

static void onStepTimer(void* clientData) {
  *(EventLoopWatchVariable*)clientData = 1;
}

struct Harness {
  TaskScheduler* scheduler;
  UsageEnvironment* env;
  Groupsock media;
  Groupsock control;
  H264VideoRTPSource* source;
  RTCPInstance* rtcp;
  int rtpFd[2]{-1, -1};
  int rtcpFd[2]{-1, -1};
  Frame frame;

  Harness()
    : scheduler(BasicTaskScheduler::createNew()),
      env(BasicUsageEnvironment::createNew(*scheduler)),
      media(*env, dummyAddr(), Port(0), 0),
      control(*env, dummyAddr(), Port(0), 0),
      source(H264VideoRTPSource::createNew(*env, &media, 96, RATE)),
      rtcp(RTCPInstance::createNew(*env, &control, 100,
				   (unsigned char const*)"local", NULL, source)) {
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, rtpFd) == 0);
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, rtcpFd) == 0);
    source->setStreamSocket(rtpFd[1], 0, NULL);
  }

  static sockaddr_storage dummyAddr() {
    sockaddr_storage addr{};
    addr.ss_family = AF_INET;
    return addr;
  }

  ~Harness() {
    Medium::close(rtcp);
    Medium::close(source);
    close(rtpFd[0]); close(rtpFd[1]);
    close(rtcpFd[0]); close(rtcpFd[1]);
    delete scheduler;
  }

  // Send one RTP packet (payload type 96) over the interleaved socket.
  void sendRTP(unsigned seq, unsigned rtpTs,
	       const std::vector<unsigned char>& payload, bool marker) {
    std::vector<unsigned char> p;
    putWord(p, 0x80000000u | ((marker ? 0x80u : 0u) | 96u) << 16 | (seq & 0xFFFF));
    putWord(p, rtpTs);
    putWord(p, SSRC);
    p.insert(p.end(), payload.begin(), payload.end());
    unsigned char hdr[4] = {'$', 0, (unsigned char)(p.size() >> 8),
			    (unsigned char)p.size()};
    require(write(rtpFd[0], hdr, 4) == 4);
    require(write(rtpFd[0], p.data(), p.size()) == (ssize_t)p.size());
  }

  // Inject an RTCP SR: NTP (sec, fractional-word), RTP timestamp.
  void sendSR(unsigned ntpSec, unsigned ntpLSW, unsigned rtpTs) {
    std::vector<unsigned char> sr;
    putWord(sr, 0x80C80006u); // V=2, PT=SR, RC=0, length 6
    putWord(sr, SSRC);
    putWord(sr, NTP_EPOCH_OFFSET + ntpSec);
    putWord(sr, ntpLSW);
    putWord(sr, rtpTs);
    putWord(sr, 0); // sender's packet count
    putWord(sr, 0); // sender's octet count
    sockaddr_storage from = dummyAddr();
    rtcp->injectReport(sr.data(), sr.size(), from);
  }

  void beginFrame() {
    frame.reset();
    source->getNextFrame(frame.bytes, sizeof(frame.bytes),
			 afterGettingFrame, &frame, NULL, NULL);
  }

  void pump() {
    TaskToken deadline = scheduler->scheduleDelayedTask(1000000, onWatchdog, &frame);
    scheduler->doEventLoop(&frame.done);
    scheduler->unscheduleDelayedTask(deadline);
    require(frame.done == 1);
    require(frame.truncated == 0);
  }

  // Process ready background I/O exactly once, without sleeping: a zero-delay
  // task makes the event loop return after handling the readable socket.
  void step() {
    EventLoopWatchVariable stop{0};
    TaskToken t = scheduler->scheduleDelayedTask(0, onStepTimer, &stop);
    scheduler->doEventLoop(&stop);
    scheduler->unscheduleDelayedTask(t);
  }

  long long frameUS() const {
    return (long long)frame.tv.tv_sec*1000000 + frame.tv.tv_usec;
  }

  RTPReceptionStats* stats() {
    return source->receptionStatsDB().lookup(SSRC);
  }
};

static std::vector<unsigned char> bytes(std::initializer_list<unsigned char> bs) {
  return std::vector<unsigned char>(bs);
}

// FU-A indicator/header helpers.
static std::vector<unsigned char> fuStart(unsigned char nalType,
					  std::initializer_list<unsigned char> payload) {
  std::vector<unsigned char> v{0x7c, (unsigned char)(0x80 | nalType)};
  v.insert(v.end(), payload.begin(), payload.end());
  return v;
}
static std::vector<unsigned char> fuCont(std::initializer_list<unsigned char> payload) {
  std::vector<unsigned char> v{0x7c, 0x05};
  v.insert(v.end(), payload.begin(), payload.end());
  return v;
}
static std::vector<unsigned char> fuEnd(std::initializer_list<unsigned char> payload) {
  std::vector<unsigned char> v{0x7c, 0x45};
  v.insert(v.end(), payload.begin(), payload.end());
  return v;
}

static long long us(long long sec, long long micro = 0) {
  return sec*1000000 + micro;
}

// ---------------------------------------------------------------------------
// Group "segments"
// ---------------------------------------------------------------------------

static int case_config_validation() {
  Harness h;
  // Both parameters must be positive; PPM must be below 1,000,000.
  TASK_CHECK(h.source->enableRTCPClockSlew(0, 1000) == False);
  TASK_CHECK(h.source->enableRTCPClockSlew(1000, 0) == False);
  TASK_CHECK(h.source->enableRTCPClockSlew(1000000, 1000) == False);
  TASK_CHECK(h.source->enableRTCPClockSlew(100000, 1000000) == True);

  // After the first getNextFrame() the configuration is frozen.
  h.beginFrame();
  TASK_CHECK(h.source->enableRTCPClockSlew(50, 50) == False);
  // A rejected re-configuration leaves the original mode in place:
  h.sendRTP(1, RATE, bytes({0x65, 0x11}), true);
  h.pump();
  TASK_CHECK(h.source->clockSlewEnabled() == True);
  TASK_CHECK(h.source->maxSlewPPM() == 100000);
  return 0;
}

static int case_positive_slew() {
  Harness h;
  TASK_CHECK(h.source->enableRTCPClockSlew(100000, 1000000) == True); // 10%
  h.sendSR(1000, 0, RATE); // first SR anchors rtp 90000 -> 1000.000000

  h.beginFrame();
  h.sendRTP(1, RATE, bytes({0x65, 0x01}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(1000));
  TASK_CHECK(h.source->hasBeenSynchronizedUsingRTCP() == True);

  h.beginFrame();
  h.sendRTP(2, 2*RATE, bytes({0x65, 0x02}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(1001));

  h.beginFrame();
  h.sendRTP(3, 3*RATE, bytes({0x65, 0x03}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(1002));

  // SR at rtp 270000 claims 1002.009000: a +9000 us error, slewed at 10%.
  h.sendSR(1002, ntpFrac(9000), 3*RATE);
  h.sendSR(1002, ntpFrac(9000), 3*RATE); // identical NTP: not advancing -> rejected

  h.beginFrame();
  h.sendRTP(4, 4*RATE, bytes({0x65, 0x04}), true);
  h.pump();
  // d = 90000 ticks; slew budget = 100000 us, capped at the 9000 us error.
  TASK_CHECK(h.frameUS() == us(1003, 9000));

  h.beginFrame();
  h.sendRTP(5, 5*RATE, bytes({0x65, 0x05}), true);
  h.pump();
  // The error is exhausted; subsequent times keep the 9000 us offset.
  TASK_CHECK(h.frameUS() == us(1004, 9000));
  return 0;
}

static int case_negative_slew() {
  Harness h;
  TASK_CHECK(h.source->enableRTCPClockSlew(100000, 1000000) == True);
  h.sendSR(2000, 0, RATE);

  h.beginFrame();
  h.sendRTP(1, RATE, bytes({0x65, 0x01}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(2000));

  h.beginFrame();
  h.sendRTP(2, 2*RATE, bytes({0x65, 0x02}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(2001));

  h.beginFrame();
  h.sendRTP(3, 3*RATE, bytes({0x65, 0x03}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(2002));

  // SR at rtp 270000 claims 2001.991000: a -9000 us error.
  h.sendSR(2001, ntpFrac(991000), 3*RATE);

  h.beginFrame();
  h.sendRTP(4, 4*RATE, bytes({0x65, 0x04}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(2002, 991000)); // 2003.000000 - 9000

  h.beginFrame();
  h.sendRTP(5, 5*RATE, bytes({0x65, 0x05}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(2003, 991000));
  return 0;
}

static int case_error_replaced_not_stacked() {
  Harness h;
  TASK_CHECK(h.source->enableRTCPClockSlew(10000, 1000000) == True); // 1%
  h.sendSR(3000, 0, RATE);

  h.beginFrame();
  h.sendRTP(1, RATE, bytes({0x65}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(3000));

  h.beginFrame();
  h.sendRTP(2, 2*RATE, bytes({0x65}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(3001));

  // First correction: +10000 us at rtp 180000.
  h.sendSR(3001, ntpFrac(10000), 2*RATE);

  // Half a second later (d=45000) half of the error is consumed (+5000).
  h.beginFrame();
  h.sendRTP(3, 225000, bytes({0x65}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(3001, 505000));

  // A new SR at the same RTP point replaces the unconsumed +5000 with a
  // fresh +10000 error (NTP = plain base 3001.500000 + 10000).
  h.sendSR(3001, ntpFrac(510000), 225000);

  h.beginFrame();
  h.sendRTP(4, 315000, bytes({0x65}), true);
  h.pump();
  // Replacement: base 3002.500000 + 10000.  Stacking would have given
  // 3002.515000 (the old +5000 plus the new +10000).
  TASK_CHECK(h.frameUS() == us(3002, 510000));
  return 0;
}

static int case_rejected_reports() {
  Harness h;
  TASK_CHECK(h.source->enableRTCPClockSlew(100000, 1000000) == True);
  h.sendSR(4000, 0, RATE);

  h.beginFrame();
  h.sendRTP(1, RATE, bytes({0x65}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(4000));

  h.beginFrame();
  h.sendRTP(2, 2*RATE, bytes({0x65}), true);
  h.pump();
  h.beginFrame();
  h.sendRTP(3, 3*RATE, bytes({0x65}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(4002));

  // NTP not advancing over the adopted SR: rejected.
  h.sendSR(4000, 0, 3*RATE);
  // RTP not advancing relative to the adopted SR's rtp 90000: rejected.
  h.sendSR(4004, 0, RATE);

  // SR whose RTP point is later than the highest media time received so far:
  // rejected (only a pre-first-packet SR may sit ahead of the media stream).
  h.sendSR(4005, 0, 6*RATE); // no packet beyond rtp 270000 has been seen yet

  // Oversized correction: the honest time at rtp 360000 is 4003.000000, but
  // the SR claims 4010.000000 (~7 ms), beyond the 1 ms bound.
  h.beginFrame();
  h.sendRTP(4, 4*RATE, bytes({0x65}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(4003));
  h.sendSR(4010, 0, 4*RATE);

  // The playback mapping never changed.
  h.beginFrame();
  h.sendRTP(5, 5*RATE, bytes({0x65}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(4004));

  // The raw SR fields still track the most recent (rejected) report.
  RTPReceptionStats* st = h.stats();
  require(st != NULL);
  TASK_CHECK(st->lastReceivedSR_NTPmsw() == NTP_EPOCH_OFFSET + 4010);
  TASK_CHECK(st->lastReceivedSR_NTPlsw() == 0);
  return 0;
}

static int case_cross_segment_out_of_order() {
  Harness h;
  TASK_CHECK(h.source->enableRTCPClockSlew(10000, 1000000) == True); // 1%

  // First SR anchors the plain curve (no correction yet).
  h.sendSR(5000, 0, RATE);
  h.beginFrame();
  h.sendRTP(1, RATE, bytes({0x65, 0x01}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(5000));

  // Queue seq 3 while seq 2 is still missing, then adopt a new SR carrying a
  // +10000 us correction at rtp 180000.
  h.beginFrame();
  h.sendRTP(3, 225000, bytes({0x65, 0x03}), true); // waits for the late seq 2
  h.sendSR(5001, ntpFrac(10000), 180000); // E = +10000 us at rtp 180000

  // Seq 2 arrives late, with a media time BEFORE the new segment anchor.  It
  // must be evaluated on the old (zero-correction) segment, not extrapolated
  // backwards from the new segment's corrected anchor:
  h.sendRTP(2, 135000, bytes({0x65, 0x02}), false);
  h.pump();
  TASK_CHECK(h.frameUS() == us(5000, 500000)); // old segment: plain rate only
  TASK_CHECK(h.source->curPacketRTPSeqNum() == 2);

  // Seq 3 keeps the snapshot it was given at receive time: a later SR never
  // rewrites an already-buffered packet.
  h.beginFrame();
  h.pump();
  TASK_CHECK(h.source->curPacketRTPSeqNum() == 3);
  TASK_CHECK(h.frameUS() == us(5001, 500000));
  return 0;
}

static int case_double_wrap() {
  Harness h;
  TASK_CHECK(h.source->enableRTCPClockSlew(100000, 1000000) == True);

  // Anchor the first SR just below the 32-bit RTP wrap, and the sequence
  // numbers just below the 16-bit wrap, so a handful of consecutive packets
  // crosses BOTH boundaries at once.
  const u_int32_t anchor = 0xFFFF0000u;
  h.sendSR(5100, 0, anchor);
  const unsigned short seqs[] = {65534, 65535, 0, 1, 2, 3};
  u_int32_t ts = anchor;
  long long expected = us(5100);
  for (unsigned i = 0; i < sizeof(seqs)/sizeof(seqs[0]); ++i) {
    h.beginFrame();
    h.sendRTP(seqs[i], ts, bytes({0x65, (unsigned char)i}), true);
    h.pump();
    TASK_CHECK(h.frameUS() == expected);
    TASK_CHECK(h.source->curPacketRTPSeqNum() == seqs[i]);
    ts += RATE; // wraps the 32-bit RTP space naturally
    expected += 1000000;
  }
  RTPReceptionStats* st = h.stats();
  require(st != NULL);
  TASK_CHECK(st->highestExtSeqNumReceived() >= 0x10001); // the seq wrap happened
  return 0;
}

static int case_pre_packet_sr() {
  Harness h;
  TASK_CHECK(h.source->enableRTCPClockSlew(100000, 1000000) == True);

  // An SR that arrives before any RTP packet must be accepted as the first
  // (NTP-anchored) segment, including when its stats record did not exist.
  TASK_CHECK(h.stats() == NULL);
  h.sendSR(5500, ntpFrac(250000), RATE); // 5500.250000 -> rtp 90000
  require(h.stats() != NULL);

  h.beginFrame();
  h.sendRTP(1, RATE, bytes({0x65}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(5500, 250000));
  TASK_CHECK(h.source->hasBeenSynchronizedUsingRTCP() == True);

  h.beginFrame();
  h.sendRTP(2, 2*RATE, bytes({0x65}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(5501, 250000));
  return 0;
}

// ---------------------------------------------------------------------------
// Group "fragment"
// ---------------------------------------------------------------------------

static int case_fu_across_sr() {
  Harness h;
  TASK_CHECK(h.source->enableRTCPClockSlew(100000, 1000000) == True);
  h.sendSR(6000, 0, RATE);

  // FU-A: start (seq 10), then an SR lands before the end fragment.  All
  // fragments of one NAL share rtp 90000, so the mid-NAL SR (same RTP point)
  // is rejected; the NAL time stays fixed to the start fragment.
  h.beginFrame();
  h.sendRTP(10, RATE, fuStart(5, {0xAA}), false);
  h.sendRTP(11, RATE, fuCont({0xBB}), false);
  h.sendSR(6010, 0, RATE); // rtpDiff == 0 -> rejected, mapping unchanged
  h.sendRTP(12, RATE, fuEnd({0xCC}), true);
  h.pump();

  // Reassembled NAL: reconstructed header 0x65 plus the three payload bytes.
  TASK_CHECK(h.frame.n == 4);
  TASK_CHECK(h.frame.bytes[0] == 0x65);
  TASK_CHECK(h.frame.bytes[1] == 0xAA);
  TASK_CHECK(h.frame.bytes[2] == 0xBB);
  TASK_CHECK(h.frame.bytes[3] == 0xCC);
  TASK_CHECK(h.frameUS() == us(6000)); // start-fragment snapshot, not 6010
  TASK_CHECK(h.source->curPacketMarkerBit() == True); // from the end fragment
  TASK_CHECK(h.source->curPacketRTPSeqNum() == 12);

  // The rejected SR still updated the raw report fields.
  RTPReceptionStats* st = h.stats();
  require(st != NULL);
  TASK_CHECK(st->lastReceivedSR_NTPmsw() == NTP_EPOCH_OFFSET + 6010);

  // The next, complete NAL is unaffected by the abandoned state.
  h.beginFrame();
  h.sendRTP(13, 2*RATE, bytes({0x67, 0xDD}), true);
  h.pump();
  TASK_CHECK(h.frame.n == 2 && h.frame.bytes[0] == 0x67);
  TASK_CHECK(h.frameUS() == us(6001));
  return 0;
}

static int case_fu_loss_recovery() {
  Harness h;
  TASK_CHECK(h.source->enableRTCPClockSlew(100000, 1000000) == True);
  static_cast<RTPSource*>(h.source)->setPacketReorderingThresholdTime(0);
  h.sendSR(7000, 0, RATE);

  // Start a FU-A NAL, then lose every remaining fragment.
  h.beginFrame();
  h.sendRTP(20, RATE, fuStart(5, {0xAA}), false);
  // A later, complete NAL (seq 22); seq 21 is gone, so the fragmented NAL is
  // abandoned and this one must be delivered cleanly with its own time.
  h.sendRTP(22, 2*RATE, bytes({0x65, 0xEE}), true);
  h.pump();

  TASK_CHECK(h.frame.n == 2);
  TASK_CHECK(h.frame.bytes[0] == 0x65 && h.frame.bytes[1] == 0xEE);
  TASK_CHECK(h.frameUS() == us(7001));
  TASK_CHECK(h.source->curPacketRTPSeqNum() == 22);
  TASK_CHECK(h.source->curPacketMarkerBit() == True);

  // A following NAL must not inherit anything from the abandoned FU.
  h.beginFrame();
  h.sendRTP(23, 3*RATE, bytes({0x41, 0xFF}), true);
  h.pump();
  TASK_CHECK(h.frame.n == 2 && h.frame.bytes[0] == 0x41);
  TASK_CHECK(h.frameUS() == us(7002));
  return 0;
}

static int case_stop_and_reread() {
  Harness h;
  TASK_CHECK(h.source->enableRTCPClockSlew(100000, 1000000) == True);
  h.sendSR(8000, 0, RATE);

  // Begin a read and ingest the start of a fragmented NAL (no end fragment,
  // so nothing is delivered), then stop while that start fragment is still
  // held inside the reordering pipeline.
  h.beginFrame();
  h.sendRTP(31, 2*RATE, fuStart(5, {0xAA}), false);
  h.step(); // read it into the pipeline; the NAL stays open
  TASK_CHECK(h.frame.done == 0);
  h.source->stopGettingFrames();

  // Re-reading keeps the established clock mapping but drops the undelivered
  // fragment and never lets its residual start-fragment state leak into the
  // next frame.
  h.beginFrame();
  h.sendRTP(32, 3*RATE, bytes({0x65, 0x32}), true);
  h.pump();
  TASK_CHECK(h.frame.n == 2 && h.frame.bytes[1] == 0x32);
  TASK_CHECK(h.frameUS() == us(8002)); // mapping from the earlier SR survives
  TASK_CHECK(h.source->curPacketRTPSeqNum() == 32);
  TASK_CHECK(h.source->curPacketMarkerBit() == True);
  // Both packets were counted, but only the second was ever delivered.
  TASK_CHECK(h.stats()->totNumPacketsReceived() == 2);
  return 0;
}

static int case_stap_a() {
  Harness h; // default (non-slew) behavior must keep working
  h.sendSR(9000, 0, RATE);

  // STAP-A: type byte 24, then one aggregated NAL (2-byte size + 0x65 0x11).
  std::vector<unsigned char> stap{24, 0x00, 0x02, 0x65, 0x11};
  h.beginFrame();
  h.sendRTP(40, RATE, stap, true);
  h.pump();
  TASK_CHECK(h.frame.n == 2);
  TASK_CHECK(h.frame.bytes[0] == 0x65 && h.frame.bytes[1] == 0x11);
  TASK_CHECK(h.frameUS() == us(9000));
  return 0;
}

// ---------------------------------------------------------------------------
// Group "reports"
// ---------------------------------------------------------------------------

static int case_rejected_sr_keeps_report_fields() {
  Harness h;
  TASK_CHECK(h.source->enableRTCPClockSlew(100000, 1000) == True); // 1 ms cap
  h.sendSR(100, 0, RATE);

  h.beginFrame();
  h.sendRTP(1, RATE, bytes({0x65}), true);
  h.pump();
  h.beginFrame();
  h.sendRTP(2, 2*RATE, bytes({0x65}), true);
  h.pump();

  // A validly-formatted but oversized correction: rejected for the mapping.
  h.sendSR(105, 0, 2*RATE); // ~4 s late at rtp 180000

  RTPReceptionStats* st = h.stats();
  require(st != NULL);
  TASK_CHECK(st->totNumPacketsReceived() == 2);
  TASK_CHECK(st->lastReceivedSR_NTPmsw() == NTP_EPOCH_OFFSET + 105);
  TASK_CHECK(st->lastReceivedSR_NTPlsw() == 0);
  require(st->lastReceivedSR_time().tv_sec != 0
	  || st->lastReceivedSR_time().tv_usec != 0);

  // Another packet keeps the unmodified mapping and still counts.
  h.beginFrame();
  h.sendRTP(3, 3*RATE, bytes({0x65}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(102));
  TASK_CHECK(st->totNumPacketsReceived() == 3);
  return 0;
}

static unsigned readNet32(const unsigned char* p) {
  return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16)
       | ((unsigned)p[2] << 8) | (unsigned)p[3];
}

static int case_rr_lsr_dlsr() {
  Harness h;
  TASK_CHECK(h.source->enableRTCPClockSlew(100000, 1000000) == True);
  h.rtcp->setStreamSocket(h.rtcpFd[1], 1, NULL);

  const unsigned msw = NTP_EPOCH_OFFSET + 200;
  const unsigned lsw = 0x12345678u;
  h.sendSR(200, lsw, RATE); // adopted/rejected is irrelevant to the RR fields

  // The record must be active (have received a packet since the last reset)
  // for an RR report block to be emitted.
  h.beginFrame();
  h.sendRTP(1, RATE, bytes({0x65}), true);
  h.pump();

  auto readRRBlock = [&](unsigned& outLSR, unsigned& outDLSR) {
    // Force an outgoing compound RTCP report over the interleaved socket.
    h.rtcp->sendReport();

    unsigned char hdr[4];
    require(read(h.rtcpFd[0], hdr, 4) == 4);
    require(hdr[0] == '$' && hdr[1] == 1);
    unsigned len = ((unsigned)hdr[2] << 8) | hdr[3];
    require(len >= 28 && len <= 1024);
    std::vector<unsigned char> pkt(len);
    size_t got = 0;
    while (got < len) {
      ssize_t n = read(h.rtcpFd[0], pkt.data() + got, len - got);
      require(n > 0);
      got += (size_t)n;
    }

    // Walk the compound packet's RTCP subpackets to find an RR report block
    // describing SSRC 77.  Block layout: SSRC(0), loss(4), highest seq(8),
    // jitter(12), LSR(16), DLSR(20).
    bool foundBlock = false;
    unsigned off = 0;
    while (off + 4 <= len) {
      unsigned char pt = pkt[off + 1];
      unsigned words = pkt[off + 3];
      unsigned subLen = 4*(words + 1);
      require(off + subLen <= len);
      if (pt == 201 /* RR */ && subLen >= 4 + 24) {
	unsigned rc = pkt[off] & 0x1F;
	unsigned block = off + 8; // skip header + SSRC
	for (unsigned i = 0; i < rc; ++i, block += 24) {
	  if (readNet32(&pkt[block]) == SSRC) {
	    outLSR = readNet32(&pkt[block + 16]);
	    outDLSR = readNet32(&pkt[block + 20]);
	    foundBlock = true;
	  }
	}
      }
      off += subLen;
    }
    require(foundBlock);
  };

  unsigned lsr1 = 0, dlsr1 = 0, lsr2 = 0, dlsr2 = 0;
  readRRBlock(lsr1, dlsr1);
  unsigned expectLSR = ((msw & 0xFFFF) << 16) | (lsw >> 16);
  TASK_CHECK(lsr1 == expectLSR); // the raw (even if rejected) SR feeds the RR

  // Emitting a report resets the per-cycle activity counters, so receive one
  // more frame before requesting another report block.
  h.beginFrame();
  h.sendRTP(2, 2*RATE, bytes({0x65}), true);
  h.pump();

  // A second report keeps the same LSR and a DLSR that never decreases as
  // more time passes since the SR.
  readRRBlock(lsr2, dlsr2);
  TASK_CHECK(lsr2 == expectLSR);
  TASK_CHECK(dlsr2 >= dlsr1);
  return 0;
}

static int case_ssrc_rebuild_waits_for_first_sr() {
  Harness h;
  TASK_CHECK(h.source->enableRTCPClockSlew(100000, 1000000) == True);
  h.sendSR(300, 0, RATE);

  h.beginFrame();
  h.sendRTP(1, RATE, bytes({0x65}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(300));
  require(h.stats() != NULL);

  // Remove the reception record (e.g. on a BYE); the same SSRC later returns.
  h.rtcp->removeSSRC(SSRC, True /*also remove stats*/);
  TASK_CHECK(h.stats() == NULL);

  // The rebuilt record starts unsynchronized and re-anchors on its first SR,
  // which is allowed to arrive before the first RTP packet of the new life.
  h.sendSR(400, 0, RATE);
  require(h.stats() != NULL);

  h.beginFrame();
  h.sendRTP(2, RATE, bytes({0x65}), true);
  h.pump();
  TASK_CHECK(h.frameUS() == us(400)); // fresh anchor, not the old 300 mapping
  TASK_CHECK(h.source->hasBeenSynchronizedUsingRTCP() == True);
  return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
  {"segments", "config_validation", case_config_validation},
  {"segments", "positive_slew", case_positive_slew},
  {"segments", "negative_slew", case_negative_slew},
  {"segments", "error_replaced_not_stacked", case_error_replaced_not_stacked},
  {"segments", "rejected_reports", case_rejected_reports},
  {"segments", "cross_segment_out_of_order", case_cross_segment_out_of_order},
  {"segments", "double_wrap", case_double_wrap},
  {"segments", "pre_packet_sr", case_pre_packet_sr},

  {"fragment", "fu_across_sr", case_fu_across_sr},
  {"fragment", "fu_loss_recovery", case_fu_loss_recovery},
  {"fragment", "stop_and_reread", case_stop_and_reread},
  {"fragment", "stap_a", case_stap_a},

  {"reports", "rejected_sr_keeps_report_fields", case_rejected_sr_keeps_report_fields},
  {"reports", "rr_lsr_dlsr", case_rr_lsr_dlsr},
  {"reports", "ssrc_rebuild", case_ssrc_rebuild_waits_for_first_sr},
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
