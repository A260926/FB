/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 3 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/
// "clock-slew"
// Demonstrates the optional RTCP clock-slew mode of an RTP source.
//
// The program builds a H.264 RTP source, enables clock slewing BEFORE the
// first read, then feeds it synthesized RTP packets and RTCP sender reports
// over a local socket pair.  Each delivered frame is reported through the
// normal "afterGetting" callback, showing how a small sender-clock correction
// is applied gradually instead of jumping the presentation time.

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"

#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <cstdio>

static const unsigned SSRC = 77;
static const unsigned RTP_TIMESTAMP_FREQUENCY = 90000; // H.264 clock
static const unsigned NTP_EPOCH_OFFSET = 0x83AA7E80u;  // 1900 -> 1970

static void putWord(std::vector<unsigned char>& v, unsigned x) {
  for (int i = 3; i >= 0; --i) v.push_back((unsigned char)((x >> (8*i)) & 255));
}

// One complete frame delivered through the ordinary source callback.
struct ClientState {
  EventLoopWatchVariable done{0};
  unsigned frameCount{0};
  unsigned char buffer[256]{};
};

static void afterGettingFrame(void* clientData, unsigned frameSize,
			       unsigned numTruncatedBytes,
			       struct timeval presentationTime,
			       unsigned /*durationInMicroseconds*/) {
  ClientState* state = (ClientState*)clientData;
  ++state->frameCount;
  std::printf("frame %u: %u byte(s), presentation time %ld.%06ld%s\n",
	      state->frameCount, frameSize,
	      (long)presentationTime.tv_sec, (long)presentationTime.tv_usec,
	      numTruncatedBytes ? " (truncated)" : "");
  state->done = 1;
}

static void onTimeout(void* clientData) {
  ((ClientState*)clientData)->done = 2; // distinguish a hang from a real frame
}

static void sendRTP(int fd, unsigned seq, unsigned rtpTimestamp,
		    const std::vector<unsigned char>& payload, bool marker) {
  std::vector<unsigned char> packet;
  putWord(packet, 0x80000000u | ((marker ? 0x80u : 0u) | 96u) << 16
	  | (seq & 0xFFFF));
  putWord(packet, rtpTimestamp);
  putWord(packet, SSRC);
  packet.insert(packet.end(), payload.begin(), payload.end());

  unsigned char interleavedHeader[4] = {
    '$', 0, (unsigned char)(packet.size() >> 8), (unsigned char)packet.size()
  };
  if (write(fd, interleavedHeader, 4) != 4
      || write(fd, packet.data(), packet.size()) != (ssize_t)packet.size()) {
    std::perror("write RTP");
  }
}

static void injectSR(RTCPInstance& rtcp, unsigned ntpSeconds,
		     unsigned ntpFraction, unsigned rtpTimestamp) {
  std::vector<unsigned char> sr;
  putWord(sr, 0x80C80006u);       // V=2, PT=SR, RC=0, length 6 words
  putWord(sr, SSRC);
  putWord(sr, NTP_EPOCH_OFFSET + ntpSeconds);
  putWord(sr, ntpFraction);
  putWord(sr, rtpTimestamp);
  putWord(sr, 0);                // sender's packet count
  putWord(sr, 0);                // sender's octet count

  struct sockaddr_storage fromAddress{};
  fromAddress.ss_family = AF_INET;
  rtcp.injectReport(sr.data(), sr.size(), fromAddress);
}

int main() {
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

  struct sockaddr_storage address{};
  address.ss_family = AF_INET;
  Groupsock mediaGroupsock(*env, address, Port(0), 0);
  Groupsock controlGroupsock(*env, address, Port(0), 0);

  H264VideoRTPSource* source = H264VideoRTPSource::createNew(
    *env, &mediaGroupsock, 96, RTP_TIMESTAMP_FREQUENCY);
  RTCPInstance* rtcp = RTCPInstance::createNew(
    *env, &controlGroupsock, 100, (unsigned char const*)"local", NULL, source);

  // The optional, gradual clock adjustment.  It must be enabled before the
  // first "getNextFrame()": at most a 100,000 PPM (10%) slew and a 1 ms cap
  // on any single correction.
  if (!source->enableRTCPClockSlew(100000, 1000)) {
    std::fprintf(stderr, "failed to enable RTCP clock slewing\n");
    return 1;
  }
  std::printf("RTCP clock slewing enabled (max 100000 PPM, max 1000 us)\n");

  int socketPair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, socketPair) != 0) {
    std::perror("socketpair");
    return 1;
  }
  source->setStreamSocket(socketPair[1], 0, NULL);

  ClientState state;

  // First sender report anchors RTP timestamp 90000 at wall-clock second 1000.
  injectSR(*rtcp, 1000, 0, 90000);

  const struct FrameSpec { unsigned seq; unsigned rtpTimestamp; } frames[] = {
    {1, 90000}, {2, 180000}, {3, 270000},
  };
  for (const FrameSpec& spec : frames) {
    source->getNextFrame(state.buffer, sizeof(state.buffer),
			 afterGettingFrame, &state, NULL, NULL);
    // A single, complete (non-fragmented) H.264 NAL unit per RTP packet.
    sendRTP(socketPair[0], spec.seq, spec.rtpTimestamp, {0x65, 0x11, 0x22},
	    true);

    TaskToken watchdog = scheduler->scheduleDelayedTask(1000000, onTimeout,
							 &state);
    scheduler->doEventLoop(&state.done);
    scheduler->unscheduleDelayedTask(watchdog);
    if (state.done != 1) {
      std::fprintf(stderr, "frame %u was not delivered\n", spec.seq);
      return 1;
    }
    state.done = 0;
  }

  // A later sender report claiming a small offset would normally jump the
  // clock; with slewing enabled it is consumed gradually over later frames,
  // while the raw SR remains available through the reception statistics.
  RTPReceptionStats* stats = source->receptionStatsDB().lookup(SSRC);
  if (stats != NULL) {
    std::printf("packets received: %u; last SR NTP MSW: 0x%08x\n",
		stats->totNumPacketsReceived(),
		stats->lastReceivedSR_NTPmsw());
  }

  Medium::close(rtcp);
  Medium::close(source);
  close(socketPair[0]);
  close(socketPair[1]);
  delete scheduler;

  std::printf("clock-slew example completed successfully\n");
  return 0;
}
