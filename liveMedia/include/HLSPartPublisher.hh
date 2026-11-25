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
// Optional partial-segment ("part") publishing state for HLSSegmenter.
//
// A "part" is a finished, byte-aligned range inside the segment's own .ts
// file.  Parts are never muxed separately: this object only tracks the
// producer's byte offsets, the single shared 27 MHz PCR expansion, and the
// flush/publish/close ordering.  It is driven from the
// MPEG2TransportStreamMultiplexor PES-start boundary hook through
// HLSSegmenter, so no second TS clock is parsed out of the output.
// C++ header

#ifndef _HLS_PART_PUBLISHER_HH
#define _HLS_PART_PUBLISHER_HH

#include <cstdint>
#include <cstdio>
#include <string>

struct HLSPartInfo {
  unsigned segmentNumber; // 沿用HLSSegmenter，从1开始
  unsigned partNumber; // 每段从0开始
  std::string fileName; // 此次事件自己拥有文件名
  uint64_t offset, length, durationUS;
};

typedef void HLSPartCallback(void* clientData, const HLSPartInfo& info);

struct HLSFileOps {
  void* context;
  FILE* (*open)(void* context, const char* path, const char* mode);
  size_t (*write)(void* context, const void* ptr, size_t size, size_t nmemb, FILE* stream);
  int (*flush)(void* context, FILE* stream);
  int (*close)(void* context, FILE* stream);
};

// Internal state machine used by HLSSegmenter in partial mode.
// All timing is integer 27 MHz PCR time (33 bit base * 300 + 9 bit ext),
// expanded monotonically across the 2^33*300 wrap.
class HLSPartPublisher {
public:
  static constexpr uint64_t PCR_MODULUS = (uint64_t(1) << 33) * 300; // 2^33 * 300
  static constexpr uint64_t TICKS_PER_MICROSECOND = 27; // 27 MHz

  HLSPartPublisher(uint64_t partTargetUS, uint64_t wholeTargetUS,
		   HLSPartCallback* callback, void* callbackData,
		   HLSFileOps const* fileOps);
      // If "fileOps" is NULL, plain stdio is used.  If non-NULL, all four
      // functions must be non-NULL (validated by HLSSegmenter).
  ~HLSPartPublisher();

  void beginSegment(unsigned segmentNumber, char const* segmentFileName);
      // Starts a fresh segment range.  The underlying file is opened lazily,
      // on the first byte write, so an input that closes before producing
      // anything leaves no empty .ts file behind.

  struct PESStartResult {
    int action; // 0 = continue; 1 = a whole segment just ended, emit PAT/PMT next
    bool ioFailed;
    uint64_t endedWholeDurationUS; // valid when action == 1
  };
  PESStartResult handlePESStart(bool scrValid,
				uint8_t highBit, uint32_t remainingBits,
				uint16_t extension);
      // Must be called exactly once for each new PCR-PID PES, before any of
      // its TS packets are produced.  Runs part/whole boundaries.

  bool writeBytes(void const* data, size_t byteCount);
      // Writes exactly one produced frame (always a multiple of 188 bytes
      // for the Transport Stream multiplexor).  A short "write" op is a
      // failure even if some bytes went out.

  struct FinishResult {
    enum Kind { Empty, Settled, Failed };
    Kind kind;
    uint64_t wholeDurationUS; // valid when kind == Settled
  };
  FinishResult finishEOF();
      // Normal source closure: publishes the non-empty tail part, then
      // closes the file.  Empty when nothing at all was produced.

  void abortSegment();
      // stopPlaying()/destruction: close the handle at most once, without
      // publishing the unfinished range or calling any segment callback.

  bool hasFailed() const { return fFailed; }

private:
  bool ensureOpen();
  bool flushHandle();
  bool closeHandle(); // calls the close op at most once per handle
  void failAndClose();
  bool settlePart(uint64_t durationTicks);
      // flush + publish the current non-empty part and advance part state

  static HLSFileOps defaultStdioOps();

private:
  HLSFileOps fOps;

  uint64_t fPartTargetTicks, fWholeTargetTicks;
  HLSPartCallback* fCallback;
  void* fCallbackData;

  // Shared expanded PCR clock:
  bool fHaveFirstPCR;
  uint64_t fLastRawPCR; // raw 27 MHz value of the previous valid PCR
  uint64_t fClockTicks; // monotonically expanded time of fLastRawPCR
  uint64_t fLastPCRTicks; // expanded time of the most recent valid PCR
  uint64_t fPartStartTicks, fWholeStartTicks;

  // Current segment / part byte state:
  unsigned fSegmentNumber;
  std::string fSegmentFileName;
  FILE* fFid;
  uint64_t fSegmentBytes; // total bytes successfully written in this segment
  uint64_t fPartStartOffset;
  unsigned fPartNumber;

  bool fFailed;
};

#endif
