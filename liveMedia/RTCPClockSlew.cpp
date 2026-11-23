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
// Implementation

#include "RTCPClockSlew.hh"

RTPClockSlewSegment::RTPClockSlewSegment(u_int64_t anchorRTP,
					 int64_t anchorPresentationUS,
					 int64_t correctionUS,
					 unsigned maxSlewPPM,
					 unsigned timestampFrequency)
  : fAnchorRTP(anchorRTP), fAnchorPresentationUS(anchorPresentationUS),
    fCorrectionUS(correctionUS), fMaxSlewPPM(maxSlewPPM),
    fTimestampFrequency(timestampFrequency),
    fMaxExtSeqBeforeNext(0xFFFFFFFFu), fNext(NULL) {
}

RTPClockSlewSegment::~RTPClockSlewSegment() {
}

int64_t RTPClockSlewSegment::mapToMicroseconds(u_int64_t rtp64) const {
  // Signed tick distance from the anchor.  A negative value is possible only
  // for the zero-correction first segment (packets older than the first SR);
  // later segments are always evaluated at or after their anchor.
  int64_t sd = (int64_t)(rtp64 - fAnchorRTP);

  int64_t base;
  if (sd >= 0) {
    // P + floor(d*1000000/rate).  Every product is 64-bit, so a 32-bit RTP
    // tick multiplication cannot overflow for a sub-day stream.
    base = fAnchorPresentationUS
	 + (int64_t)((u_int64_t)sd*1000000ull/(u_int64_t)fTimestampFrequency);
  } else {
    u_int64_t ad = (u_int64_t)(-sd);
    base = fAnchorPresentationUS
	 - (int64_t)(ad*1000000ull/(u_int64_t)fTimestampFrequency);
  }

  if (fCorrectionUS == 0 || sd <= 0) return base;

  // Slew term: sign(E)*min(|E|, floor(d*maxSlewPPM/rate)), with d >= 0:
  u_int64_t d = (u_int64_t)sd;
  u_int64_t cap = (fCorrectionUS < 0)
		? (u_int64_t)(-fCorrectionUS)
		: (u_int64_t)fCorrectionUS;
  u_int64_t consumed = d*(u_int64_t)fMaxSlewPPM
		     / (u_int64_t)fTimestampFrequency;
  if (consumed > cap) consumed = cap;

  return (fCorrectionUS >= 0) ? base + (int64_t)consumed
			      : base - (int64_t)consumed;
}
