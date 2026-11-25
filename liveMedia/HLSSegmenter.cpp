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
// A media sink that takes - as input - a MPEG Transport Stream, and outputs a series
// of MPEG Transport Stream files, each representing a segment of the input stream,
// suitable for HLS (Apple's "HTTP Live Streaming").
// Implementation

#include "HLSSegmenter.hh"
#include "OutputFile.hh"
#include "MPEG2TransportStreamMultiplexor.hh"

#define TRANSPORT_PACKET_SIZE 188
#define OUTPUT_FILE_BUFFER_SIZE (TRANSPORT_PACKET_SIZE*100)

HLSSegmenter* HLSSegmenter
::createNew(UsageEnvironment& env,
	    unsigned segmentationDuration, char const* fileNamePrefix,
	    onEndOfSegmentFunc* onEndOfSegmentFunc, void* onEndOfSegmentClientData) {
  return new HLSSegmenter(env, segmentationDuration, fileNamePrefix,
			  onEndOfSegmentFunc, onEndOfSegmentClientData);
}

HLSSegmenter::HLSSegmenter(UsageEnvironment& env,
			   unsigned segmentationDuration, char const* fileNamePrefix,
			   onEndOfSegmentFunc* onEndOfSegmentFunc,
			   void* onEndOfSegmentClientData)
  : MediaSink(env),
    fSegmentationDuration(segmentationDuration),
    fFileNamePrefix(NULL), fOwnedFileNamePrefix(NULL),
    fOnEndOfSegmentFunc(onEndOfSegmentFunc),
    fOnEndOfSegmentClientData(onEndOfSegmentClientData),
    fHaveConfiguredUpstreamSource(False), fStartPlayingEver(False),
    fCurrentSegmentCounter(1),
    fOutputSegmentFileName(NULL), fOutFid(NULL),
    fOutputFileBuffer(NULL),
    fPartialEnabled(False), fPartPublisher(NULL),
    fPartFailed(False), fStandardClosureNotified(False) {
  // Keep our own copy of the file-name prefix:
  char const* const prefix = fileNamePrefix != NULL ? fileNamePrefix : "";
  fOwnedFileNamePrefix = new char[strlen(prefix) + 1];
  strcpy(fOwnedFileNamePrefix, prefix);
  fFileNamePrefix = fOwnedFileNamePrefix;

  // Allocate enough space for the segment file name:
  fOutputSegmentFileName = new char[strlen(prefix) + 20/*more than enough*/];

  // Allocate the output file buffer size:
  fOutputFileBuffer = new unsigned char[OUTPUT_FILE_BUFFER_SIZE];
}

HLSSegmenter::~HLSSegmenter() {
  if (fPartPublisher != NULL) {
    delete fPartPublisher; // aborts/closes any still-open handle exactly once
    fPartPublisher = NULL;
  }
  delete[] fOutputFileBuffer;
  delete[] fOutputSegmentFileName;
  delete[] fOwnedFileNamePrefix;
}

Boolean HLSSegmenter
::enablePartialSegments(unsigned partDurationUS,
			HLSPartCallback* partCallback, void* partCallbackData,
			HLSFileOps const* fileOps) {
  // Reject duplicate configuration and configuration after first play:
  if (fPartialEnabled || fStartPlayingEver) return False;

  // Validate everything before changing any state, so a rejected call
  // leaves the original configuration untouched:
  if (partDurationUS == 0 || partCallback == NULL) return False;
  if (fSegmentationDuration == 0) return False;

  uint64_t const wholeTargetUS
    = uint64_t(fSegmentationDuration) * 1000000ull; // no overflow in 64 bits
  if (uint64_t(partDurationUS) > wholeTargetUS) return False;

  if (fileOps != NULL
      && (fileOps->open == NULL || fileOps->write == NULL
	  || fileOps->flush == NULL || fileOps->close == NULL)) {
    return False;
  }

  fPartPublisher = new HLSPartPublisher(partDurationUS, wholeTargetUS,
					partCallback, partCallbackData,
					fileOps); // copies *fileOps
  fPartialEnabled = True;
  return True;
}

void HLSSegmenter::stopPlaying() {
  // stopPlaying()/destruction is an abort: close the handle without
  // publishing the unfinished range or sending a segment completion.
  if (fPartPublisher != NULL) fPartPublisher->abortSegment();
  MediaSink::stopPlaying();
}

void HLSSegmenter::ourEndOfSegmentHandler(void* clientData, double segmentDuration) {
  ((HLSSegmenter*)clientData)->ourEndOfSegmentHandler(segmentDuration);
}

void HLSSegmenter::ourEndOfSegmentHandler(double segmentDuration) {
  // Note the end of the current segment:
  if (fOnEndOfSegmentFunc != NULL) {
    (*fOnEndOfSegmentFunc)(fOnEndOfSegmentClientData, fOutputSegmentFileName, segmentDuration);
  }

  // Begin the next segment:
  ++fCurrentSegmentCounter;
  openNextOutputSegment();
}

Boolean HLSSegmenter::openNextOutputSegment() {
  CloseOutputFile(fOutFid);

  sprintf(fOutputSegmentFileName, "%s%03u.ts", fFileNamePrefix, fCurrentSegmentCounter);
  fOutFid = OpenOutputFile(envir(), fOutputSegmentFileName);

  return fOutFid != NULL;
}

void HLSSegmenter::afterGettingFrame(void* clientData, unsigned frameSize,
				     unsigned numTruncatedBytes,
				     struct timeval /*presentationTime*/,
				     unsigned /*durationInMicroseconds*/) {
  ((HLSSegmenter*)clientData)->afterGettingFrame(frameSize, numTruncatedBytes);
}

void HLSSegmenter::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes) {
  if (numTruncatedBytes > 0) { // Shouldn't happen
    fprintf(stderr, "HLSSegmenter::afterGettingFrame(frameSize %d, numTruncatedBytes %d)\n", frameSize, numTruncatedBytes);
  }

  if (fPartialEnabled) {
    if (fPartFailed || fPartPublisher == NULL) return; // reading has stopped

    if (!fPartPublisher->writeBytes(fOutputFileBuffer, frameSize)) {
      notePartIOFailure("write");
      return; // do not ask for another frame
    }

    // Then try getting the next frame:
    continuePlaying();
    return;
  }

  // Classic mode: write the data to our output segment file:
  fwrite(fOutputFileBuffer, 1, frameSize, fOutFid);

  // Then try getting the next frame:
  continuePlaying();
}

int HLSSegmenter::ourPESStartHook(void* clientData,
				  MPEG1or2Demux::SCR const& scr) {
  return ((HLSSegmenter*)clientData)->handlePESStart(scr);
}

int HLSSegmenter::handlePESStart(MPEG1or2Demux::SCR const& scr) {
  if (!fPartialEnabled || fPartPublisher == NULL || fPartFailed) return 0;

  HLSPartPublisher::PESStartResult const result
    = fPartPublisher->handlePESStart(scr.isValid == True,
				     scr.highBit, scr.remainingBits,
				     scr.extension);
  if (result.ioFailed) {
    notePartIOFailure("flush or close");
    return 0;
  }

  if (result.action == 1) {
    // The publisher has already published the final part (after a successful
    // flush) and successfully closed the old segment file.  Only now send
    // the old whole-segment completion callback:
    if (fOnEndOfSegmentFunc != NULL) {
      double const segmentDuration
	= double(result.endedWholeDurationUS) / 1000000.0;
      (*fOnEndOfSegmentFunc)(fOnEndOfSegmentClientData,
			     fOutputSegmentFileName, segmentDuration);
    }

    // Advance to the next segment file.  The multiplexor will emit PAT then
    // PMT before this PES's media, and the new first part contains them.
    ++fCurrentSegmentCounter;
    sprintf(fOutputSegmentFileName, "%s%03u.ts",
	    fFileNamePrefix, fCurrentSegmentCounter);
    fPartPublisher->beginSegment(fCurrentSegmentCounter,
				 fOutputSegmentFileName);
    return 1;
  }

  return 0;
}

void HLSSegmenter::notePartIOFailure(char const* what) {
  if (fPartFailed) return;
  fPartFailed = True;

  envir().setResultMsg("HLSPartSegmenter file operation failed: ", what);

  // Notify standard closure from a fresh event-loop task, so we never run
  // the downstream "after playing" callback re-entrantly inside the
  // multiplexor production chain.  Replace any stale task token first.
  envir().taskScheduler().unscheduleDelayedTask(nextTask());
  nextTask()
    = envir().taskScheduler().scheduleDelayedTask(0, (TaskFunc*)ourFailureTask, this);
}

void HLSSegmenter::ourFailureTask(void* clientData) {
  HLSSegmenter* segmenter = (HLSSegmenter*)clientData;
  segmenter->nextTask() = NULL; // this task has now run
  segmenter->notifyStandardClosureOnce();
}

void HLSSegmenter::notifyStandardClosureOnce() {
  if (fStandardClosureNotified) return;
  fStandardClosureNotified = True;

  // On the IO-failure path, actively cancel any pending source delivery;
  // on a normal EOF the source has already closed itself.
  if (fPartFailed && fSource != NULL) fSource->stopGettingFrames();

  // MediaSink::onSourceClosure() unschedules our task token, clears fSource,
  // and invokes the standard after-playing callback once.
  onSourceClosure();
}

void HLSSegmenter::ourOnSourceClosure(void* clientData) {
  ((HLSSegmenter*)clientData)->ourOnSourceClosure();
}

void HLSSegmenter::ourOnSourceClosure() {
  if (fPartialEnabled) {
    if (fStandardClosureNotified) return;

    if (!fPartFailed && fPartPublisher != NULL) {
      HLSPartPublisher::FinishResult const finished
	= fPartPublisher->finishEOF();

      if (finished.kind == HLSPartPublisher::FinishResult::Failed) {
	notePartIOFailure("flush or close at EOF");
	return; // the failure task performs the standard closure notification
      }

      if (finished.kind == HLSPartPublisher::FinishResult::Settled
	  && fOnEndOfSegmentFunc != NULL) {
	// Normal EOF: the tail part was published and the file closed
	// successfully before this whole-segment completion event.
	double const segmentDuration
	  = double(finished.wholeDurationUS) / 1000000.0;
	(*fOnEndOfSegmentFunc)(fOnEndOfSegmentClientData,
			       fOutputSegmentFileName, segmentDuration);
      }
      // FinishResult::Empty: no input at all, so no file/event/callback.
    }

    notifyStandardClosureOnce();
    return;
  }

  // Classic mode: note the end of the final segment (currently being written):
  if (fOnEndOfSegmentFunc != NULL) {
    // We know that the source is a "MPEG2TransportStreamMultiplexor":
    MPEG2TransportStreamMultiplexor* multiplexorSource = (MPEG2TransportStreamMultiplexor*)fSource;
    double segmentDuration = multiplexorSource->currentSegmentDuration();

    (*fOnEndOfSegmentFunc)(fOnEndOfSegmentClientData, fOutputSegmentFileName, segmentDuration);
  }

  // Handle the closure for real:
  onSourceClosure();
}

Boolean HLSSegmenter::sourceIsCompatibleWithUs(MediaSource& source) {
  // Our source must be a Transport Stream Multiplexor:
  return source.isMPEG2TransportStreamMultiplexor();
}

Boolean HLSSegmenter::continuePlaying() {
  if (fSource == NULL) return False;
  fStartPlayingEver = True;

  if (!fHaveConfiguredUpstreamSource) {
    // We know that the source is a "MPEG2TransportStreamMultiplexor":
    MPEG2TransportStreamMultiplexor* multiplexorSource = (MPEG2TransportStreamMultiplexor*)fSource;

    if (fPartialEnabled) {
      // Precise PES-start boundaries drive parts and whole segments; do not
      // also enable the classic floating-point timed segmentation.
      multiplexorSource->setPESStartHook(&ourPESStartHook, this);

      sprintf(fOutputSegmentFileName, "%s%03u.ts",
	      fFileNamePrefix, fCurrentSegmentCounter);
      fPartPublisher->beginSegment(fCurrentSegmentCounter,
				   fOutputSegmentFileName);
    } else {
      // Tell our upstream multiplexor to call our 'end of segment handler' at the end of
      // each timed segment:
      multiplexorSource->setTimedSegmentation(fSegmentationDuration, ourEndOfSegmentHandler, this);
    }

    fHaveConfiguredUpstreamSource = True; // from now on
  }

  if (!fPartialEnabled) {
    if (fOutFid == NULL && !openNextOutputSegment()) return False;
  }

  fSource->getNextFrame(fOutputFileBuffer, OUTPUT_FILE_BUFFER_SIZE,
			afterGettingFrame, this,
			ourOnSourceClosure, this);

  return True;
}
