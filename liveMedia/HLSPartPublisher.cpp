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
// Implementation of optional partial-segment publishing state.

#include "HLSPartPublisher.hh"

#include <algorithm>

namespace {
uint64_t scrToRawPCR(bool highBit, uint32_t remainingBits, uint16_t extension) {
  // PCR_base is 33 bits: highBit is bit 32, remainingBits are bits 31..0.
  uint64_t const base = (uint64_t(highBit ? 1 : 0) << 32) | uint64_t(remainingBits);
  return base * 300 + uint64_t(extension);
}
} // namespace

FILE* hlsp_defaultOpen(void*, char const* path, char const* mode) {
  return fopen(path, mode);
}
size_t hlsp_defaultWrite(void*, void const* ptr, size_t size, size_t nmemb, FILE* fid) {
  return fwrite(ptr, size, nmemb, fid);
}
int hlsp_defaultFlush(void*, FILE* fid) {
  return fflush(fid);
}
int hlsp_defaultClose(void*, FILE* fid) {
  return fclose(fid);
}

HLSFileOps HLSPartPublisher::defaultStdioOps() {
  return HLSFileOps{nullptr, hlsp_defaultOpen, hlsp_defaultWrite,
		    hlsp_defaultFlush, hlsp_defaultClose};
}

HLSPartPublisher
::HLSPartPublisher(uint64_t partTargetUS, uint64_t wholeTargetUS,
		   HLSPartCallback* callback, void* callbackData,
		   HLSFileOps const* fileOps)
  : fOps(fileOps != nullptr ? *fileOps : defaultStdioOps()),
    fPartTargetTicks(partTargetUS * TICKS_PER_MICROSECOND),
    fWholeTargetTicks(wholeTargetUS * TICKS_PER_MICROSECOND),
    fCallback(callback), fCallbackData(callbackData),
    fHaveFirstPCR(false), fLastRawPCR(0), fClockTicks(0), fLastPCRTicks(0),
    fPartStartTicks(0), fWholeStartTicks(0),
    fSegmentNumber(0), fFid(nullptr),
    fSegmentBytes(0), fPartStartOffset(0), fPartNumber(0), fFailed(false) {
}

HLSPartPublisher::~HLSPartPublisher() {
  abortSegment();
}

void HLSPartPublisher::beginSegment(unsigned segmentNumber,
				    char const* segmentFileName) {
  // Any previous handle must already have been finished/aborted; make sure
  // we never leak it even if called unexpectedly.
  abortSegment();

  fSegmentNumber = segmentNumber;
  fSegmentFileName = segmentFileName != nullptr ? segmentFileName : "";
  fFid = nullptr; // abortSegment() closed and cleared the previous handle
  fSegmentBytes = 0;
  fPartStartOffset = 0;
  fPartNumber = 0;

  // The expanded clock itself is stream-global across segments; only the
  // per-range anchor is established by the PCR that starts the segment.
  fPartStartTicks = fClockTicks;
  fWholeStartTicks = fClockTicks;
}

bool HLSPartPublisher::ensureOpen() {
  if (fFid != nullptr) return true;
  if (fFailed || fOps.open == nullptr) return false;

  fFid = fOps.open(fOps.context, fSegmentFileName.c_str(), "wb");
  if (fFid == nullptr) {
    fFailed = true;
    return false;
  }
  return true;
}

bool HLSPartPublisher::flushHandle() {
  if (fFid == nullptr || fOps.flush == nullptr) return true; // nothing buffered
  if (fOps.flush(fOps.context, fFid) != 0) {
    failAndClose();
    return false;
  }
  return true;
}

bool HLSPartPublisher::closeHandle() {
  if (fFid == nullptr) return true; // each handle is closed at most once

  FILE* fid = fFid;
  fFid = nullptr; // clear first: the close op runs exactly once either way
  int const result = fOps.close != nullptr ? fOps.close(fOps.context, fid) : 0;
  if (result != 0) {
    fFailed = true;
    return false;
  }
  return true;
}

void HLSPartPublisher::failAndClose() {
  fFailed = true;
  if (fFid != nullptr) {
    FILE* fid = fFid;
    fFid = nullptr;
    if (fOps.close != nullptr) (void)fOps.close(fOps.context, fid);
  }
}

void HLSPartPublisher::abortSegment() {
  // Stop/teardown path: close the handle without flushing/publishing the
  // unpublished range.  closeHandle() guarantees a single close call.
  (void)closeHandle();
}

bool HLSPartPublisher::writeBytes(void const* data, size_t byteCount) {
  if (fFailed) return false;
  if (byteCount == 0) return true;
  if (!ensureOpen()) return false;

  size_t const written
    = fOps.write(fOps.context, data, 1, byteCount, fFid);
  if (written < byteCount) {
    // Short write is failure even if "written" bytes hit the file.
    failAndClose();
    return false;
  }

  fSegmentBytes += uint64_t(byteCount);
  return true;
}

bool HLSPartPublisher::settlePart(uint64_t durationTicks) {
  uint64_t const length = fSegmentBytes - fPartStartOffset;
  if (length == 0) {
    // Never publish an empty part; just re-anchor the next one.
    return true;
  }

  if (!flushHandle()) return false; // failed range is not published

  HLSPartInfo info;
  info.segmentNumber = fSegmentNumber;
  info.partNumber = fPartNumber;
  info.fileName = fSegmentFileName; // event owns its own copy
  info.offset = fPartStartOffset;
  info.length = length;
  info.durationUS = durationTicks / TICKS_PER_MICROSECOND; // floor to us

  if (fCallback != nullptr) (*fCallback)(fCallbackData, info);

  fPartStartOffset = fSegmentBytes;
  ++fPartNumber;
  return true;
}

HLSPartPublisher::PESStartResult
HLSPartPublisher::handlePESStart(bool scrValid,
				 uint8_t highBit, uint32_t remainingBits,
				 uint16_t extension) {
  PESStartResult result{0, false, 0};
  if (fFailed) { result.ioFailed = true; return result; }

  if (scrValid) {
    uint64_t const raw = scrToRawPCR(highBit != 0, remainingBits, extension);
    if (!fHaveFirstPCR) {
      // The first valid PCR establishes time zero; an all-zero base counts.
      fHaveFirstPCR = true;
      fLastRawPCR = raw;
      fClockTicks = 0;
      fLastPCRTicks = 0;
      fPartStartTicks = 0;
      fWholeStartTicks = 0;
    } else {
      // Forward difference on the modulo-2^33*300 circle.  Real activity
      // jumps are < half a circle, so the modular forward delta resolves a
      // normal wrap unambiguously and keeps the clock monotonic.
      uint64_t delta;
      if (raw >= fLastRawPCR) {
	delta = raw - fLastRawPCR;
      } else {
	delta = PCR_MODULUS - fLastRawPCR + raw;
      }
      fClockTicks += delta;
      fLastRawPCR = raw;
      fLastPCRTicks = fClockTicks;
    }
  }
  // An invalid SCR ("no PCR") neither advances time nor starts a boundary.

  if (!fHaveFirstPCR) return result;

  uint64_t const wholeElapsed = fClockTicks - fWholeStartTicks;
  uint64_t const partElapsed = fClockTicks - fPartStartTicks;

  if (wholeElapsed >= fWholeTargetTicks) {
    // Whole boundary has priority.  Settle the old (non-empty) final part
    // with the current boundary PCR, then close the file.
    uint64_t const partDurationTicks = fClockTicks - fPartStartTicks;
    if (!settlePart(partDurationTicks)) {
      result.ioFailed = true;
      return result;
    }

    uint64_t const wholeDurationUS
      = (fClockTicks - fWholeStartTicks) / TICKS_PER_MICROSECOND;

    if (!closeHandle()) {
      result.ioFailed = true;
      result.endedWholeDurationUS = wholeDurationUS;
      return result;
    }

    result.action = 1;
    result.endedWholeDurationUS = wholeDurationUS;

    // The new segment's part/whole anchors are this same boundary PCR.
    fWholeStartTicks = fClockTicks;
    fPartStartTicks = fClockTicks;
    // Byte/part number state for the new segment is installed by
    // HLSSegmenter calling beginSegment() after its old-segment callback.
    return result;
  }

  if (partElapsed >= fPartTargetTicks) {
    uint64_t const partDurationTicks = fClockTicks - fPartStartTicks;
    if (!settlePart(partDurationTicks)) {
      result.ioFailed = true;
      return result;
    }
    fPartStartTicks = fClockTicks;
  }

  return result;
}

HLSPartPublisher::FinishResult HLSPartPublisher::finishEOF() {
  FinishResult result{FinishResult::Empty, 0};
  if (fFailed) { result.kind = FinishResult::Failed; return result; }

  if (fSegmentBytes == 0 && fFid == nullptr) {
    // No input at all: no file was opened, no event/callback.
    return result;
  }

  // Settle the unpublished non-empty tail.  With no following boundary PCR,
  // time it from the last valid PCR; a tail that starts at that PCR has an
  // allowed duration of zero.
  uint64_t tailDurationTicks = 0;
  if (fHaveFirstPCR && fLastPCRTicks >= fPartStartTicks) {
    tailDurationTicks = fLastPCRTicks - fPartStartTicks;
  }
  if (!settlePart(tailDurationTicks)) {
    result.kind = FinishResult::Failed;
    return result;
  }

  uint64_t const wholeDurationUS
    = fHaveFirstPCR && fLastPCRTicks >= fWholeStartTicks
      ? (fLastPCRTicks - fWholeStartTicks) / TICKS_PER_MICROSECOND
      : 0;

  if (!closeHandle()) {
    result.kind = FinishResult::Failed;
    result.wholeDurationUS = wholeDurationUS;
    return result;
  }

  result.kind = FinishResult::Settled;
  result.wholeDurationUS = wholeDurationUS;
  return result;
}
