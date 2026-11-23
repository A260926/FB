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
// "liveMedia"
// Copyright (c) 1996-2026 Live Networks, Inc.  All rights reserved.
// RTP Sources
// Implementation

#include "RTPSource.hh"
#include "RTCPClockSlew.hh"
#include "GroupsockHelper.hh"

////////// RTPSource //////////

Boolean RTPSource::lookupByName(UsageEnvironment& env,
				char const* sourceName,
				RTPSource*& resultSource) {
  resultSource = NULL; // unless we succeed

  MediaSource* source;
  if (!MediaSource::lookupByName(env, sourceName, source)) return False;

  if (!source->isRTPSource()) {
    env.setResultMsg(sourceName, " is not a RTP source");
    return False;
  }

  resultSource = (RTPSource*)source;
  return True;
}

Boolean RTPSource::hasBeenSynchronizedUsingRTCP() {
  return fCurPacketHasBeenSynchronizedUsingRTCP;
}

Boolean RTPSource::isRTPSource() const {
  return True;
}

Boolean RTPSource::enableRTCPClockSlew(unsigned maxSlewPPM,
				       unsigned maxCorrectionUS) {
  if (fReadStarted) return False; // configuration is fixed once reading begins
  if (maxSlewPPM == 0 || maxCorrectionUS == 0) return False;
  if (maxSlewPPM >= 1000000) return False; // must be a proper, sub-100% rate

  fClockSlewEnabled = True;
  fMaxSlewPPM = maxSlewPPM;
  fMaxCorrectionUS = maxCorrectionUS;
  return True;
}

RTPSource::RTPSource(UsageEnvironment& env, Groupsock* RTPgs,
		     unsigned char rtpPayloadFormat,
		     u_int32_t rtpTimestampFrequency)
  : FramedSource(env),
    fRTPInterface(this, RTPgs),
    fCurPacketHasBeenSynchronizedUsingRTCP(False), fLastReceivedSSRC(0),
    fRTCPInstanceForMultiplexedRTCPPackets(NULL), fCrypto(NULL),
    fRTPPayloadFormat(rtpPayloadFormat), fTimestampFrequency(rtpTimestampFrequency),
    fSSRC(our_random32()), fEnableRTCPReports(True),
    fReadStarted(False), fClockSlewEnabled(False),
    fMaxSlewPPM(0), fMaxCorrectionUS(0) {
  fReceptionStatsDB = new RTPReceptionStatsDB();
}

RTPSource::~RTPSource() {
  delete fReceptionStatsDB;
}

void RTPSource::getAttributes() const {
  envir().setResultMsg(""); // Fix later to get attributes from  header #####
}


////////// RTPReceptionStatsDB //////////

RTPReceptionStatsDB::RTPReceptionStatsDB()
  : fTable(HashTable::create(ONE_WORD_HASH_KEYS)), fTotNumPacketsReceived(0) {
  reset();
}

void RTPReceptionStatsDB::reset() {
  fNumActiveSourcesSinceLastReset = 0;

  Iterator iter(*this);
  RTPReceptionStats* stats;
  while ((stats = iter.next()) != NULL) {
    stats->reset();
  }
}

RTPReceptionStatsDB::~RTPReceptionStatsDB() {
  // First, remove and delete all stats records from the table:
  RTPReceptionStats* stats;
  while ((stats = (RTPReceptionStats*)fTable->RemoveNext()) != NULL) {
    delete stats;
  }

  // Then, delete the table itself:
  delete fTable;
}

void RTPReceptionStatsDB
::noteIncomingPacket(u_int32_t SSRC, u_int16_t seqNum,
		     u_int32_t rtpTimestamp, unsigned timestampFrequency,
		     Boolean useForJitterCalculation,
		     struct timeval& resultPresentationTime,
		     Boolean& resultHasBeenSyncedUsingRTCP,
		     unsigned packetSize,
		     Boolean clockSlewEnabled,
		     unsigned maxSlewPPM,
		     unsigned maxCorrectionUS) {
  ++fTotNumPacketsReceived;
  RTPReceptionStats* stats = lookup(SSRC);
  if (stats == NULL) {
    // This is the first time we've heard from this SSRC.
    // Create a new record for it:
    stats = new RTPReceptionStats(SSRC, seqNum);
    if (stats == NULL) return;
    add(SSRC, stats);
  }

  if (stats->numPacketsReceivedSinceLastReset() == 0) {
    ++fNumActiveSourcesSinceLastReset;
  }

  stats->noteIncomingPacket(seqNum, rtpTimestamp, timestampFrequency,
				    useForJitterCalculation,
				    resultPresentationTime,
				    resultHasBeenSyncedUsingRTCP, packetSize,
				    clockSlewEnabled,
				    maxSlewPPM,
				    maxCorrectionUS);
}

void RTPReceptionStatsDB
::noteIncomingSR(u_int32_t SSRC,
		 u_int32_t ntpTimestampMSW, u_int32_t ntpTimestampLSW,
		 u_int32_t rtpTimestamp,
		 Boolean clockSlewEnabled,
		 unsigned maxSlewPPM,
		 unsigned maxCorrectionUS,
		 unsigned timestampFrequency) {
  RTPReceptionStats* stats = lookup(SSRC);
  if (stats == NULL) {
    // This is the first time we've heard of this SSRC.
    // Create a new record for it (this covers SRs that arrive before any RTP
    // packet, so the slew configuration is applied to it as well):
    stats = new RTPReceptionStats(SSRC);
    if (stats == NULL) return;
    add(SSRC, stats);
  }

  stats->noteIncomingSR(ntpTimestampMSW, ntpTimestampLSW, rtpTimestamp,
			clockSlewEnabled, maxSlewPPM, maxCorrectionUS,
			timestampFrequency);
}

void RTPReceptionStatsDB::removeRecord(u_int32_t SSRC) {
  RTPReceptionStats* stats = lookup(SSRC);
  if (stats != NULL) {
    long SSRC_long = (long)SSRC;
    fTable->Remove((char const*)SSRC_long);
    delete stats;
  }
}

RTPReceptionStatsDB::Iterator
::Iterator(RTPReceptionStatsDB& receptionStatsDB)
  : fIter(HashTable::Iterator::create(*(receptionStatsDB.fTable))) {
}

RTPReceptionStatsDB::Iterator::~Iterator() {
  delete fIter;
}

RTPReceptionStats*
RTPReceptionStatsDB::Iterator::next(Boolean includeInactiveSources) {
  char const* key; // dummy

  // If asked, skip over any sources that haven't been active
  // since the last reset:
  RTPReceptionStats* stats;
  do {
    stats = (RTPReceptionStats*)(fIter->next(key));
  } while (stats != NULL && !includeInactiveSources
	   && stats->numPacketsReceivedSinceLastReset() == 0);

  return stats;
}

RTPReceptionStats* RTPReceptionStatsDB::lookup(u_int32_t SSRC) const {
  long SSRC_long = (long)SSRC;
  return (RTPReceptionStats*)(fTable->Lookup((char const*)SSRC_long));
}

void RTPReceptionStatsDB::add(u_int32_t SSRC, RTPReceptionStats* stats) {
  long SSRC_long = (long)SSRC;
  fTable->Add((char const*)SSRC_long, stats);
}

////////// RTPReceptionStats //////////

RTPReceptionStats::RTPReceptionStats(u_int32_t SSRC, u_int16_t initialSeqNum) {
  initSeqNum(initialSeqNum);
  init(SSRC);
}

RTPReceptionStats::RTPReceptionStats(u_int32_t SSRC) {
  init(SSRC);
}

RTPReceptionStats::~RTPReceptionStats() {
  releaseSlewSegments();
}

void RTPReceptionStats::init(u_int32_t SSRC) {
  fSSRC = SSRC;
  fTotNumPacketsReceived = 0;
  fTotBytesReceived_hi = fTotBytesReceived_lo = 0;
  fBaseExtSeqNumReceived = 0;
  fHighestExtSeqNumReceived = 0;
  fHaveSeenInitialSequenceNumber = False;
  fLastTransit = ~0;
  fPreviousPacketRTPTimestamp = 0;
  fJitter = 0.0;
  fLastReceivedSR_NTPmsw = fLastReceivedSR_NTPlsw = 0;
  fLastReceivedSR_time.tv_sec = fLastReceivedSR_time.tv_usec = 0;
  fLastPacketReceptionTime.tv_sec = fLastPacketReceptionTime.tv_usec = 0;
  fMinInterPacketGapUS = 0x7FFFFFFF;
  fMaxInterPacketGapUS = 0;
  fTotalInterPacketGaps.tv_sec = fTotalInterPacketGaps.tv_usec = 0;
  fHasBeenSynchronized = False;
  fSyncTime.tv_sec = fSyncTime.tv_usec = 0;
  fSlewSegments = NULL;
  fSlewExtRTP = 0;
  fSlewLastSR_NTPmsw = fSlewLastSR_NTPlsw = 0;
  fSlewLastSR_RTP = 0;
  fSlewHaveAdoptedSR = False;
  reset();
}

void RTPReceptionStats::initSeqNum(u_int16_t initialSeqNum) {
    fBaseExtSeqNumReceived = 0x10000 | initialSeqNum;
    fHighestExtSeqNumReceived = 0x10000 | initialSeqNum;
    fHaveSeenInitialSequenceNumber = True;
}

#ifndef MILLION
#define MILLION 1000000
#endif

// Convert a (possibly wrapped) 32-bit RTP timestamp to a monotonic 64-bit
// tick count, using "previousExt" as the count at the most recent packet.
// The signed 32-bit difference is assumed to span less than half of the
// 32-bit cycle, as required for a normal I/P stream.
static u_int64_t extendRTPTimestamp(u_int32_t rtpTimestamp,
				    u_int64_t previousExt) {
  int32_t diff = (int32_t)(rtpTimestamp - (u_int32_t)(previousExt & 0xFFFFFFFFu));
  return (u_int64_t)((int64_t)previousExt + (int64_t)diff);
}

void RTPReceptionStats
::noteIncomingPacket(u_int16_t seqNum, u_int32_t rtpTimestamp,
		     unsigned timestampFrequency,
		     Boolean useForJitterCalculation,
		     struct timeval& resultPresentationTime,
		     Boolean& resultHasBeenSyncedUsingRTCP,
		     unsigned packetSize,
		     Boolean clockSlewEnabled,
		     unsigned maxSlewPPM,
		     unsigned maxCorrectionUS) {
  if (!fHaveSeenInitialSequenceNumber) initSeqNum(seqNum);

  ++fNumPacketsReceivedSinceLastReset;
  ++fTotNumPacketsReceived;
  u_int32_t prevTotBytesReceived_lo = fTotBytesReceived_lo;
  fTotBytesReceived_lo += packetSize;
  if (fTotBytesReceived_lo < prevTotBytesReceived_lo) { // wrap-around
    ++fTotBytesReceived_hi;
  }

  // Check whether the new sequence number is the highest yet seen:
  unsigned oldSeqNum = (fHighestExtSeqNumReceived&0xFFFF);
  unsigned seqNumCycle = (fHighestExtSeqNumReceived&0xFFFF0000);
  unsigned seqNumDifference = (unsigned)((int)seqNum-(int)oldSeqNum);
  unsigned newSeqNum = 0;
  if (seqNumLT((u_int16_t)oldSeqNum, seqNum)) {
    // This packet was not an old packet received out of order, so check it:

    if (seqNumDifference >= 0x8000) {
      // The sequence number wrapped around, so start a new cycle:
      seqNumCycle += 0x10000;
    }

    newSeqNum = seqNumCycle|seqNum;
    if (newSeqNum > fHighestExtSeqNumReceived) {
      fHighestExtSeqNumReceived = newSeqNum;
    }
  } else if (fTotNumPacketsReceived > 1) {
    // This packet was an old packet received out of order

    if ((int)seqNumDifference >= 0x8000) {
      // The sequence number wrapped around, so switch to an old cycle:
      seqNumCycle -= 0x10000;
    }

    newSeqNum = seqNumCycle|seqNum;
    if (newSeqNum < fBaseExtSeqNumReceived) {
      fBaseExtSeqNumReceived = newSeqNum;
    }
  }

  // The extended sequence number of this very packet:
  unsigned packetExtSeqNum = (newSeqNum != 0) ? newSeqNum
					      : fHighestExtSeqNumReceived;

  // Record the inter-packet delay
  struct timeval timeNow;
  gettimeofday(&timeNow, NULL);
  if (fLastPacketReceptionTime.tv_sec != 0
      || fLastPacketReceptionTime.tv_usec != 0) {
    unsigned gap
      = (timeNow.tv_sec - fLastPacketReceptionTime.tv_sec)*MILLION
      + timeNow.tv_usec - fLastPacketReceptionTime.tv_usec;
    if (gap > fMaxInterPacketGapUS) {
      fMaxInterPacketGapUS = gap;
    }
    if (gap < fMinInterPacketGapUS) {
      fMinInterPacketGapUS = gap;
    }
    fTotalInterPacketGaps.tv_usec += gap;
    if (fTotalInterPacketGaps.tv_usec >= MILLION) {
      ++fTotalInterPacketGaps.tv_sec;
      fTotalInterPacketGaps.tv_usec -= MILLION;
    }
  }
  fLastPacketReceptionTime = timeNow;

  // Compute the current 'jitter' using the received packet's RTP timestamp,
  // and the RTP timestamp that would correspond to the current time.
  // (Use the code from appendix A.8 in the RTP spec.)
  // Note, however, that we don't use this packet if its timestamp is
  // the same as that of the previous packet (this indicates a multi-packet
  // fragment), or if we've been explicitly told not to use this packet.
  if (useForJitterCalculation
      && rtpTimestamp != fPreviousPacketRTPTimestamp) {
    unsigned arrival = (timestampFrequency*timeNow.tv_sec);
    arrival += (unsigned)
      ((2.0*timestampFrequency*timeNow.tv_usec + 1000000.0)/2000000);
            // note: rounding
    int transit = arrival - rtpTimestamp;
    if (fLastTransit == (~0)) fLastTransit = transit; // hack for first time
    int d = transit - fLastTransit;
    fLastTransit = transit;
    if (d < 0) d = -d;
    fJitter += (1.0/16.0) * ((double)d - fJitter);
  }

  if (clockSlewEnabled) {
    slewNoteIncomingPacket(rtpTimestamp, timestampFrequency, packetExtSeqNum,
			   resultPresentationTime,
			   resultHasBeenSyncedUsingRTCP);
    // The reorder window advances with every received packet, so segments
    // that can no longer be needed may be released:
    pruneSlewSegments();
    fPreviousPacketRTPTimestamp = rtpTimestamp;
    return;
  }

  // Return the 'presentation time' that corresponds to "rtpTimestamp":
  if (fSyncTime.tv_sec == 0 && fSyncTime.tv_usec == 0) {
    // This is the first timestamp that we've seen, so use the current
    // 'wall clock' time as the synchronization time.  (This will be
    // corrected later when we receive RTCP SRs.)
    fSyncTimestamp = rtpTimestamp;
    fSyncTime = timeNow;
  }

  int timestampDiff = rtpTimestamp - fSyncTimestamp;
      // Note: This works even if the timestamp wraps around
      // (as long as "int" is 32 bits)

  // Divide this by the timestamp frequency to get real time:
  double timeDiff = timestampDiff/(double)timestampFrequency;

  // Add this to the 'sync time' to get our result:
  unsigned const million = 1000000;
  unsigned seconds, uSeconds;
  if (timeDiff >= 0.0) {
    seconds = fSyncTime.tv_sec + (unsigned)(timeDiff);
    uSeconds = fSyncTime.tv_usec
      + (unsigned)((timeDiff - (unsigned)timeDiff)*million);
    if (uSeconds >= million) {
      uSeconds -= million;
      ++seconds;
    }
  } else {
    timeDiff = -timeDiff;
    seconds = fSyncTime.tv_sec - (unsigned)(timeDiff);
    uSeconds = fSyncTime.tv_usec
      - (unsigned)((timeDiff - (unsigned)timeDiff)*million);
    if ((int)uSeconds < 0) {
      uSeconds += million;
      --seconds;
    }
  }
  resultPresentationTime.tv_sec = seconds;
  resultPresentationTime.tv_usec = uSeconds;
  resultHasBeenSyncedUsingRTCP = fHasBeenSynchronized;

  // Save these as the new synchronization timestamp & time:
  fSyncTimestamp = rtpTimestamp;
  fSyncTime = resultPresentationTime;

  fPreviousPacketRTPTimestamp = rtpTimestamp;
}

void RTPReceptionStats::noteIncomingSR(u_int32_t ntpTimestampMSW,
				       u_int32_t ntpTimestampLSW,
				       u_int32_t rtpTimestamp,
				       Boolean clockSlewEnabled,
				       unsigned maxSlewPPM,
				       unsigned maxCorrectionUS,
				       unsigned timestampFrequency) {
  // The raw, most-recently-received SR information is always recorded,
  // even when the report is rejected for clock-slew purposes, so LSR/DLSR
  // and the reception statistics keep reflecting the actual input:
  fLastReceivedSR_NTPmsw = ntpTimestampMSW;
  fLastReceivedSR_NTPlsw = ntpTimestampLSW;

  gettimeofday(&fLastReceivedSR_time, NULL);

  if (clockSlewEnabled) {
    slewNoteIncomingSR(ntpTimestampMSW, ntpTimestampLSW, rtpTimestamp,
		       timestampFrequency, maxSlewPPM, maxCorrectionUS);
    return;
  }

  // Use this SR to update time synchronization information:
  fSyncTimestamp = rtpTimestamp;
  fSyncTime.tv_sec = ntpTimestampMSW - 0x83AA7E80; // 1/1/1900 -> 1/1/1970
  double microseconds = (ntpTimestampLSW*15625.0)/0x04000000; // 10^6/2^32
  fSyncTime.tv_usec = (unsigned)(microseconds+0.5);
  fHasBeenSynchronized = True;
}

double RTPReceptionStats::totNumKBytesReceived() const {
  double const hiMultiplier = 0x20000000/125.0; // == (2^32)/(10^3)
  return fTotBytesReceived_hi*hiMultiplier + fTotBytesReceived_lo/1000.0;
}

unsigned RTPReceptionStats::jitter() const {
  return (unsigned)fJitter;
}

void RTPReceptionStats::reset() {
  fNumPacketsReceivedSinceLastReset = 0;
  fLastResetExtSeqNumReceived = fHighestExtSeqNumReceived;
}

Boolean seqNumLT(u_int16_t s1, u_int16_t s2) {
  // a 'less-than' on 16-bit sequence numbers
  int diff = s2-s1;
  if (diff > 0) {
    return (diff < 0x8000);
  } else if (diff < 0) {
    return (diff < -0x8000);
  } else { // diff == 0
    return False;
  }
}


////////// Clock-slew mode implementation //////////

// The NTP fractional (least-significant) word, truncated downwards to whole
// microseconds (10^6/2^32 == 15625/2^26):
static unsigned ntpFractionToMicroseconds(u_int32_t ntpTimestampLSW) {
  return (unsigned)(((u_int64_t)ntpTimestampLSW*15625u) >> 26);
}

// Split a microsecond-since-Unix-epoch value into a timeval, keeping the
// microsecond field in [0, 1000000) even for (unusual) negative values:
static struct timeval microsecondsToTimeval(int64_t us) {
  struct timeval tv;
  int64_t seconds = us/MILLION; // truncates toward zero
  int64_t micros = us%MILLION;
  if (micros < 0) {
    micros += MILLION;
    --seconds;
  }
  tv.tv_sec = (long)seconds;
  tv.tv_usec = (int)micros;
  return tv;
}

void RTPReceptionStats
::slewNoteIncomingPacket(u_int32_t rtpTimestamp,
			 unsigned timestampFrequency,
			 unsigned extSeqNum,
			 struct timeval& resultPresentationTime,
			 Boolean& resultHasBeenSyncedUsingRTCP) {
  // Maintain the wrap-aware 64-bit RTP tick count.  Before the first SR (and
  // first packet) the anchor tick is arbitrary, so start the count at the
  // first packet's own tick.
  if (!fSlewHaveAdoptedSR && fSlewSegments == NULL
      && fTotNumPacketsReceived == 1) {
    fSlewExtRTP = (u_int64_t)rtpTimestamp;
  } else {
    fSlewExtRTP = extendRTPTimestamp(rtpTimestamp, fSlewExtRTP);
  }

  if (fSlewHaveAdoptedSR) {
    resultPresentationTime = slewMapPacket(rtpTimestamp, timestampFrequency,
					   extSeqNum);
    resultHasBeenSyncedUsingRTCP = True;
  } else {
    // No SR yet: there is no absolute mapping until the first report.
    resultPresentationTime.tv_sec = 0;
    resultPresentationTime.tv_usec = 0;
    resultHasBeenSyncedUsingRTCP = False;
  }
}

struct timeval RTPReceptionStats
::slewMapPacket(u_int32_t rtpTimestamp, unsigned timestampFrequency,
		unsigned extSeqNum) {
  u_int64_t rtp64 = extendRTPTimestamp(rtpTimestamp, fSlewExtRTP);

  // The segments' extended-sequence ranges partition the stream in order,
  // each bounded by "maxExtSeqBeforeNext()" (the tail segment is unbounded).
  // Pick the unique historical segment this packet belongs to, so a late
  // packet is never evaluated against a segment that did not exist when it
  // arrived:
  RTPClockSlewSegment* chosen = fSlewSegments;
  for (RTPClockSlewSegment* seg = fSlewSegments; seg != NULL;
       seg = seg->next()) {
    if (extSeqNum <= seg->maxExtSeqBeforeNext()) {
      chosen = seg;
      break;
    }
  }

  int64_t us = chosen->mapToMicroseconds(rtp64);
  return microsecondsToTimeval(us);
}

void RTPReceptionStats
::slewNoteIncomingSR(u_int32_t ntpTimestampMSW, u_int32_t ntpTimestampLSW,
		     u_int32_t rtpTimestamp,
		     unsigned timestampFrequency,
		     unsigned maxSlewPPM, unsigned maxCorrectionUS) {
  // NTP time in microseconds since the Unix epoch; the fractional word is
  // truncated downwards to whole microseconds:
  int64_t ntpUS = (int64_t)(u_int64_t)(ntpTimestampMSW - 0x83AA7E80u)*MILLION
		+ (int64_t)ntpFractionToMicroseconds(ntpTimestampLSW);

  u_int64_t srRTP64;
  if (!fSlewHaveAdoptedSR) {
    // First SR: keep the original NTP anchoring behavior.  Any packets seen
    // before this point (which had no absolute time) may now switch to it.
    if (fTotNumPacketsReceived == 0) {
      // The SR arrived before any RTP packet: anchor the tick count directly.
      srRTP64 = (u_int64_t)rtpTimestamp;
    } else {
      srRTP64 = extendRTPTimestamp(rtpTimestamp, fSlewExtRTP);
    }
    fSlewExtRTP = srRTP64;
    RTPClockSlewSegment* seg
      = new RTPClockSlewSegment(srRTP64, ntpUS, 0/*no correction yet*/,
				maxSlewPPM, timestampFrequency);
    releaseSlewSegments();
    fSlewSegments = seg;
    fSlewHaveAdoptedSR = True;
  } else {
    // Subsequent SRs must be strictly newer than the last adopted one:
    u_int64_t prevNTP = ((u_int64_t)fSlewLastSR_NTPmsw << 32)
		      | (u_int64_t)fSlewLastSR_NTPlsw;
    u_int64_t thisNTP = ((u_int64_t)ntpTimestampMSW << 32)
		      | (u_int64_t)ntpTimestampLSW;
    int32_t rtpDiff = (int32_t)(rtpTimestamp - fSlewLastSR_RTP);

    if (!(thisNTP > prevNTP) || rtpDiff <= 0) {
      // Non-advancing NTP, or no positive signed-32-bit RTP advance:
      // reject the report and leave the playback mapping unchanged.
      return;
    }

    srRTP64 = extendRTPTimestamp(rtpTimestamp, fSlewExtRTP);

    // The SR's RTP point must not be later than the most advanced media
    // time received so far (a pre-first-packet SR is the branch above):
    if (srRTP64 > fSlewExtRTP) return;

    // P: the presentation time the old mapping gives this RTP point.
    struct timeval ptv = slewMapPacket(rtpTimestamp, timestampFrequency,
				       fHighestExtSeqNumReceived);
    int64_t P = (int64_t)ptv.tv_sec*MILLION + ptv.tv_usec;
    int64_t E = ntpUS - P;
    int64_t absE = (E < 0) ? -E : E;
    if (absE > (int64_t)maxCorrectionUS) {
      // The correction exceeds the configured bound: reject the report and
      // leave the playback mapping unchanged.
      return;
    }

    // Adopt the report: a new segment begins at this point.  Its correction
    // replaces whatever error the previous segment had not yet consumed;
    // the old (tail) segment is bounded to the packets already received.
    RTPClockSlewSegment* tail = fSlewSegments;
    while (tail != NULL && tail->next() != NULL) tail = tail->next();
    if (tail != NULL) tail->setMaxExtSeqBeforeNext(fHighestExtSeqNumReceived);

    RTPClockSlewSegment* seg
      = new RTPClockSlewSegment(srRTP64, P, E, maxSlewPPM,
				timestampFrequency);
    if (tail == NULL) {
      fSlewSegments = seg;
    } else {
      tail->setNext(seg);
    }
  }

  fSlewLastSR_NTPmsw = ntpTimestampMSW;
  fSlewLastSR_NTPlsw = ntpTimestampLSW;
  fSlewLastSR_RTP = rtpTimestamp;

  // Retain only the segments still needed by packets inside the reordering
  // window (bounded by the 16-bit sequence space):
  pruneSlewSegments();
}

void RTPReceptionStats::pruneSlewSegments() {
  // Any sequence number older than this can no longer arrive within the
  // 16-bit reordering window, so segments that end before it are obsolete:
  unsigned neededExtSeq = (fHighestExtSeqNumReceived >= 0x10000)
			? fHighestExtSeqNumReceived - 0xFFFFu : 0;
  while (fSlewSegments != NULL && fSlewSegments->next() != NULL) {
    if (fSlewSegments->maxExtSeqBeforeNext() >= neededExtSeq) break; // still needed
    RTPClockSlewSegment* old = fSlewSegments;
    fSlewSegments = old->next();
    old->setNext(NULL);
    delete old;
  }
}

void RTPReceptionStats::releaseSlewSegments() {
  RTPClockSlewSegment* seg = fSlewSegments;
  while (seg != NULL) {
    RTPClockSlewSegment* next = seg->next();
    delete seg;
    seg = next;
  }
  fSlewSegments = NULL;
}
