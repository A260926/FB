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
// Optional gradual RTCP clock adjustment ("clock slewing")
// C++ header (internal implementation detail of "RTPSource")

#ifndef _RTP_CLOCK_SLEW_HH
#define _RTP_CLOCK_SLEW_HH

#ifndef _RTP_SOURCE_HH
#include "RTPSource.hh"
#endif

// One segment of the piecewise RTP-timestamp -> presentation-time mapping
// created when an RTCP SR is adopted in clock-slew mode.  For an RTP tick
// count "d >= 0" past the segment anchor, the mapped presentation time is:
//
//   P + floor(d*1000000/rate)
//     + sign(E)*min(|E|, floor(d*maxSlewPPM/rate))
//
// All arithmetic is done in 64 bits, so 32-bit RTP wrap-arounds (and the
// products above) cannot overflow for streams spanning less than one day.
class RTPClockSlewSegment {
public:
  RTPClockSlewSegment(u_int64_t anchorRTP, int64_t anchorPresentationUS,
		      int64_t correctionUS, unsigned maxSlewPPM,
		      unsigned timestampFrequency);
  ~RTPClockSlewSegment();

  RTPClockSlewSegment* next() const { return fNext; }
  void setNext(RTPClockSlewSegment* next) { fNext = next; }

  u_int64_t anchorRTP() const { return fAnchorRTP; }

  // The extended RTP sequence number of the most advanced packet seen
  // before the next segment began.  Any packet that could still map to
  // this (older) segment has an extended sequence number no larger than
  // this.  UINT_MAX means no newer segment exists yet.
  unsigned maxExtSeqBeforeNext() const { return fMaxExtSeqBeforeNext; }
  void setMaxExtSeqBeforeNext(unsigned seq) { fMaxExtSeqBeforeNext = seq; }

  // Maps the (extended, wrap-aware) RTP timestamp "rtp64" to a presentation
  // time in microseconds since the Unix epoch.  Timestamps before the anchor
  // (only possible for the first segment, whose correction is zero) are
  // extrapolated backwards along the plain rate mapping.
  int64_t mapToMicroseconds(u_int64_t rtp64) const;

private:
  u_int64_t fAnchorRTP;
  int64_t fAnchorPresentationUS; // "P": presentation time at the anchor
  int64_t fCorrectionUS; // signed, not-yet-consumed correction ("E")
  unsigned fMaxSlewPPM;
  unsigned fTimestampFrequency;
  unsigned fMaxExtSeqBeforeNext;
  RTPClockSlewSegment* fNext;
};

#endif
