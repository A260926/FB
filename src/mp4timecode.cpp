/*
* The contents of this file are subject to the Mozilla Public
* License Version 1.1 (the "License"); you may not use this file
* except in compliance with the License. You may obtain a copy of
* the License at http://www.mozilla.org/MPL/
*
* Software distributed under the License is distributed on an "AS
* IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
* implied. See the License for the specific language governing
* rights and limitations under the License.
*
* The Original Code is MP4v2.
*/

#include "src/impl.h"
#include <mp4v2/timecode.h>

#include <climits>
#include <cstdio>
#include <cstring>
#include <vector>

namespace mp4v2 { namespace impl {

        ///////////////////////////////////////////////////////////////////////////////

        namespace {

            // non-negative rational, denominator always strictly positive
            struct Rational {
                uint64_t num;
                uint64_t den;
            };

            static bool safeMulU(uint64_t a, uint64_t b, uint64_t& out)
            {
                if (a != 0 && b > UINT64_MAX / a) {
                    return false;
                }
                out = a * b;
                return true;
            }

            static bool safeAddU(uint64_t a, uint64_t b, uint64_t& out)
            {
                if (a > UINT64_MAX - b) {
                    return false;
                }
                out = a + b;
                return true;
            }

            static uint64_t gcd64(uint64_t a, uint64_t b)
            {
                while (b) {
                    uint64_t t = a % b;
                    a = b;
                    b = t;
                }
                return a;
            }

            // reduce a fraction in place so later cross products do not overflow on
            // values that are mathematically small
            static void reduceRat(uint64_t& num, uint64_t& den)
            {
                if (num == 0) {
                    den = 1;
                    return;
                }
                const uint64_t g = gcd64(num, den);
                if (g > 1) {
                    num /= g;
                    den /= g;
                }
            }

            // compare rational num/den against a whole value x:
            // -1 if num/den < x, 0 if equal, 1 if greater
            static int cmpRat(uint64_t num, uint64_t den, uint64_t x)
            {
                uint64_t scaled;
                if (!safeMulU(x, den, scaled)) {
                    // x*den overflows; the bounded rational cannot be that large
                    return -1;
                }
                if (num < scaled) return -1;
                if (num > scaled) return 1;
                return 0;
            }

            struct EditEntry {
                uint64_t segmentDuration; // movie timeline units
                int64_t  mediaTime;       // -1 marks an empty edit
                int16_t  mediaRate;
                uint16_t mediaRateFraction;
            };

            // Validate edit contents shared by both tracks. Only positive-duration,
            // unit-rate edits (plus empty edits with media time -1) are supported.
            static bool validateEdits(const std::vector<EditEntry>& edits)
            {
                for (uint32_t i = 0; i < edits.size(); i++) {
                    if (edits[i].segmentDuration == 0) {
                        return false;
                    }
                    if (edits[i].mediaRate != 1 || edits[i].mediaRateFraction != 0) {
                        return false;
                    }
                }
                return true;
            }

            struct TimecodeDescription {
                uint32_t flags;
                uint32_t timeScale;
                uint32_t frameDuration;
                uint8_t  numFrames;
            };

            // QuickTime timecode description flag bits
            const uint32_t TC_FLAG_DROP_FRAME  = 0x80000000u;
            const uint32_t TC_FLAG_24_HOUR_MAX = 0x40000000u;
            const uint32_t TC_FLAG_COUNTER     = 0x20000000u;

            static bool formatLabel(
            uint64_t frameNumber,
            const TimecodeDescription& desc,
            char text[16])
            {
                if (desc.flags & TC_FLAG_COUNTER) {
                    return false;
                }

                const bool dropFrame = (desc.flags & TC_FLAG_DROP_FRAME) != 0;
                const bool wrap24Hour = (desc.flags & TC_FLAG_24_HOUR_MAX) != 0;

                uint32_t framesPerSecond = 0;
                uint32_t dropPerMinute = 0;

                if (dropFrame) {
                    if (desc.timeScale == 30000u &&
                    desc.frameDuration == 1001u &&
                    desc.numFrames == 30) {
                        framesPerSecond = 30;
                        dropPerMinute = 2;
                    } else if (desc.timeScale == 60000u &&
                    desc.frameDuration == 1001u &&
                    desc.numFrames == 60) {
                        framesPerSecond = 60;
                        dropPerMinute = 4;
                        } else {
                        return false;
                    }
                    } else {
                    static const uint32_t integerRates[] =
                    { 24, 25, 30, 50, 60 };
                    for (uint32_t i = 0;
                    i < sizeof(integerRates) / sizeof(integerRates[0]); i++) {
                        uint32_t rate = integerRates[i];
                        if (desc.numFrames == rate &&
                        desc.timeScale == rate * desc.frameDuration) {
                            framesPerSecond = rate;
                            break;
                        }
                    }
                    if (framesPerSecond == 0) {
                        return false;
                    }
                }

                // map the absolute frame number onto wall-clock frame labels
                uint64_t labelFrame = frameNumber;
                if (dropFrame) {
                    const uint64_t framesPerMinute =
                    (uint64_t)framesPerSecond * 60u - dropPerMinute;
                    const uint64_t framesPer10Minutes =
                    (uint64_t)framesPerSecond * 600u -
                    (uint64_t)dropPerMinute * 9u;
                    const uint64_t tens = labelFrame / framesPer10Minutes;
                    const uint64_t rest = labelFrame % framesPer10Minutes;

                    uint64_t adjusted;
                    if (!safeMulU(tens, (uint64_t)dropPerMinute * 9u, adjusted)) {
                        return false;
                    }
                    if (rest > (uint64_t)dropPerMinute) {
                        uint64_t extra;
                        if (!safeMulU(
                        dropPerMinute,
                        (uint32_t)((rest - dropPerMinute) / framesPerMinute),
                        extra) ||
                        !safeAddU(adjusted, extra, adjusted)) {
                            return false;
                        }
                    }
                    if (!safeAddU(labelFrame, adjusted, labelFrame)) {
                        return false;
                    }
                }

                const uint64_t framesPerHour = (uint64_t)framesPerSecond * 3600u;
                const uint64_t framesPerMinute = (uint64_t)framesPerSecond * 60u;

                uint64_t hour = labelFrame / framesPerHour;
                const uint32_t minute =
                (uint32_t)((labelFrame / framesPerMinute) % 60u);
                const uint32_t second =
                (uint32_t)((labelFrame / framesPerSecond) % 60u);
                const uint32_t frame =
                (uint32_t)(labelFrame % framesPerSecond);

                if (wrap24Hour) {
                    hour %= 24u;
                    } else if (hour > 99u) {
                    return false;
                }

                const char separator = dropFrame ? ';' : ':';
                int written = snprintf(
                text, 16, "%02llu:%02u:%02u%c%02u",
                (unsigned long long)hour, minute, second, separator, frame);
                if (written <= 0 || written >= 16) {
                    return false;
                }
                return true;
            }

        } // anonymous namespace

        ///////////////////////////////////////////////////////////////////////////////

        // Reuses the parsed track tables through friendship with MP4Track; it never
        // opens files or rescans boxes itself.
        class TimecodeDetails
        {
            public:
            static bool get(
            MP4File& file,
            MP4TrackId videoTrackId,
            uint64_t movieWhen,
            MP4VideoTimeCode& out)
            {
                const uint64_t movieTimeScale = file.GetTimeScale();
                if (movieTimeScale == 0) {
                    return false;
                }

                MP4Track* video = file.GetTrack(videoTrackId);
                if (!video ||
                !strequal(video->GetType(), MP4_VIDEO_TRACK_TYPE)) {
                    return false;
                }

                // resolve the unique tref.tmcd association
                MP4Atom* tref =
                video->GetTrakAtom().FindAtom("trak.tref");
                if (!tref) {
                    return false;
                }

                uint32_t tmcdRefs = 0;
                MP4Atom* tmcdRef = NULL;
                for (uint32_t i = 0; i < tref->GetNumberOfChildAtoms(); i++) {
                    MP4Atom* child = tref->GetChildAtom(i);
                    if (strequal(child->GetType(), "tmcd")) {
                        tmcdRefs++;
                        tmcdRef = child;
                    }
                }
                if (tmcdRefs != 1 || !tmcdRef) {
                    return false;
                }

                MP4Integer32Property* refCountProp = NULL;
                MP4Integer32Property* refIdProp = NULL;
                if (!tmcdRef->FindProperty(
                "tmcd.entryCount", (MP4Property**)&refCountProp) ||
                !tmcdRef->FindProperty(
                "tmcd.entries.trackId", (MP4Property**)&refIdProp) ||
                refCountProp->GetValue() != 1) {
                    return false;
                }

                const MP4TrackId timeCodeTrackId = refIdProp->GetValue(0);
                if (timeCodeTrackId == MP4_INVALID_TRACK_ID ||
                timeCodeTrackId == videoTrackId) {
                    return false;
                }

                MP4Track* timecode = file.GetTrack(timeCodeTrackId);
                if (!timecode || !strequal(timecode->GetType(), "tmcd")) {
                    return false;
                }

                std::vector<EditEntry> videoEdits;
                std::vector<EditEntry> timecodeEdits;
                if (!readEdits(*video, videoEdits) ||
                !readEdits(*timecode, timecodeEdits) ||
                !validateEdits(videoEdits) ||
                !validateEdits(timecodeEdits)) {
                    return false;
                }

                MP4SampleId videoSampleId = MP4_INVALID_SAMPLE_ID;
                Rational anchorMovie = { 0, 1 };
                if (!locateVideoFrame(
                *video, movieTimeScale, movieWhen, videoEdits,
                videoSampleId, anchorMovie)) {
                    return false;
                }

                MP4SampleId counterSampleId = MP4_INVALID_SAMPLE_ID;
                uint64_t counterDts = 0;
                Rational timecodeMedia = { 0, 1 };
                if (!locateCounter(
                *timecode, movieTimeScale, anchorMovie, timecodeEdits,
                counterSampleId, counterDts, timecodeMedia)) {
                    return false;
                }

                TimecodeDescription desc;
                if (!readCounterDescription(*timecode, counterSampleId, desc)) {
                    return false;
                }

                // read the counter payload through the ordinary sample path
                uint8_t* bytes = NULL;
                uint32_t numBytes = 0;
                MP4Timestamp sampleStart = 0;
                MP4Duration sampleDuration = 0;
                MP4Duration renderingOffset = 0;
                bool isSync = false;
                timecode->ReadSample(
                counterSampleId, &bytes, &numBytes,
                &sampleStart, &sampleDuration, &renderingOffset, &isSync);

                if (numBytes != 4 || bytes == NULL) {
                    MP4Free(bytes);
                    return false;
                }
                const uint64_t counterValue =
                ((uint64_t)bytes[0] << 24) |
                ((uint64_t)bytes[1] << 16) |
                ((uint64_t)bytes[2] << 8) |
                (uint64_t)bytes[3];
                MP4Free(bytes);

                // delta = timecodeMedia - counterDts, kept rational
                uint64_t dtsScaled;
                if (!safeMulU(counterDts, timecodeMedia.den, dtsScaled) ||
                timecodeMedia.num < dtsScaled) {
                    return false;
                }
                uint64_t deltaNum = timecodeMedia.num - dtsScaled;
                uint64_t deltaDen = timecodeMedia.den;
                reduceRat(deltaNum, deltaDen);

                // framesAdded = floor(delta * desc.timeScale /
                //                     (tcTimeScale * desc.frameDuration))
                const uint64_t tcTimeScale = timecode->GetTimeScale();

                // cancel common factors before multiplying so the intermediates
                // stay bounded regardless of the source time scales
                uint64_t leftNum = deltaNum;
                uint64_t leftDen = deltaDen;
                uint64_t rightNum = desc.timeScale;
                uint64_t rightDen = tcTimeScale;
                reduceRat(leftNum, leftDen);
                reduceRat(rightNum, rightDen);

                uint64_t g;
                g = gcd64(leftNum, rightDen); leftNum /= g; rightDen /= g;
                g = gcd64(rightNum, leftDen); rightNum /= g; leftDen /= g;

                uint64_t numerator, denominator;
                if (!safeMulU(leftNum, rightNum, numerator) ||
                !safeMulU(leftDen, rightDen, denominator) ||
                !safeMulU(denominator, desc.frameDuration, denominator)) {
                    return false;
                }
                const uint64_t framesAdded = numerator / denominator;

                uint64_t frameNumber;
                if (!safeAddU(counterValue, framesAdded, frameNumber)) {
                    return false;
                }

                MP4VideoTimeCode result;
                result.videoSampleId = videoSampleId;
                result.timeCodeTrackId = timeCodeTrackId;
                result.timeCodeSampleId = counterSampleId;
                result.frameNumber = frameNumber;
                std::memset(result.text, 0, sizeof(result.text));
                if (!formatLabel(frameNumber, desc, result.text)) {
                    return false;
                }

                out = result;
                return true;
            }

            private:
            // Parse one track's edit list through the handles the track already
            // built. An empty vector means no edit list, i.e. media time zero.
            static bool readEdits(
            MP4Track& track, std::vector<EditEntry>& edits)
            {
                edits.clear();

                if (track.m_pElstCountProperty == NULL) {
                    return true;
                }

                MP4Atom* elst = track.GetTrakAtom().FindAtom("trak.edts.elst");
                if (!elst) {
                    return false;
                }

                MP4Integer8Property* versionProp = NULL;
                if (!elst->FindProperty(
                "elst.version", (MP4Property**)&versionProp)) {
                    return false;
                }
                const bool version1 = (versionProp->GetValue() == 1);
                const uint64_t emptyMarker =
                version1 ? UINT64_MAX : (uint64_t)0xFFFFFFFFu;

                uint32_t count = track.m_pElstCountProperty->GetValue();
                for (uint32_t i = 0; i < count; i++) {
                    EditEntry e;
                    e.segmentDuration =
                    track.m_pElstDurationProperty->GetValue(i);
                    uint64_t rawMediaTime =
                    track.m_pElstMediaTimeProperty->GetValue(i);
                    e.mediaRate =
                    (int16_t)track.m_pElstRateProperty->GetValue(i);
                    e.mediaRateFraction =
                    (uint16_t)track.m_pElstReservedProperty->GetValue(i);

                    if (rawMediaTime == emptyMarker) {
                        e.mediaTime = -1;
                    } else if (!version1 &&
                    rawMediaTime > (uint64_t)INT32_MAX) {
                        // any negative media time other than the empty edit is
                        // out of scope
                        return false;
                        } else {
                        e.mediaTime = (int64_t)rawMediaTime;
                    }

                    edits.push_back(e);
                }
                return true;
            }

            // Decode time (stts) and composition offset (ctts) of one sample.
            // Negative composition offsets are rejected.
            static bool sampleTiming(
            MP4Track& track,
            MP4SampleId sampleId,
            uint64_t& dts,
            uint64_t& duration,
            uint64_t& compositionOffset,
            bool wantOffset)
            {
                uint32_t numStts = track.m_pSttsCountProperty->GetValue();
                MP4SampleId sid = 1;
                uint64_t elapsed = 0;
                bool found = false;

                for (uint32_t i = 0; i < numStts; i++) {
                    uint32_t sampleCount =
                    track.m_pSttsSampleCountProperty->GetValue(i);
                    uint64_t delta =
                    track.m_pSttsSampleDeltaProperty->GetValue(i);

                    if (sampleCount == 0) {
                        return false;
                    }
                    if (sampleId < sid + sampleCount) {
                        uint64_t into;
                        if (!safeMulU((uint64_t)(sampleId - sid), delta, into)) {
                            return false;
                        }
                        if (!safeAddU(elapsed, into, dts)) {
                            return false;
                        }
                        duration = delta;
                        found = true;
                        break;
                    }
                    uint64_t span;
                    if (!safeMulU((uint64_t)sampleCount, delta, span) ||
                    !safeAddU(elapsed, span, elapsed)) {
                        return false;
                    }
                    sid += sampleCount;
                }
                if (!found) {
                    return false;
                }

                compositionOffset = 0;
                if (wantOffset &&
                track.m_pCttsCountProperty != NULL &&
                track.m_pCttsCountProperty->GetValue() > 0) {

                    uint32_t numCtts = track.m_pCttsCountProperty->GetValue();
                    MP4SampleId csid = 1;
                    for (uint32_t i = 0; i < numCtts; i++) {
                        uint32_t sampleCount =
                        track.m_pCttsSampleCountProperty->GetValue(i);
                        if (sampleCount == 0) {
                            return false;
                        }
                        if (sampleId < csid + sampleCount) {
                            uint64_t offset =
                            track.m_pCttsSampleOffsetProperty->GetValue(i);
                            if (offset > (uint64_t)INT32_MAX) {
                                return false; // negative ctts is out of scope
                            }
                            compositionOffset = offset;
                            return true;
                        }
                        csid += sampleCount;
                    }
                    return false; // ctts table did not cover the sample
                }
                return true;
            }

            // Read and validate the tmcd sample description actually referenced by
            // the stsc entry of the given counter sample. Description switches
            // inside one track are therefore honoured per sample.
            static bool readCounterDescription(
            MP4Track& track,
            MP4SampleId sampleId,
            TimecodeDescription& desc)
            {
                uint32_t numStsc = track.m_pStscCountProperty->GetValue();
                if (numStsc == 0) {
                    return false;
                }

                uint32_t stscIndex;
                for (stscIndex = 0; stscIndex < numStsc; stscIndex++) {
                    if (sampleId <
                    track.m_pStscFirstSampleProperty->GetValue(stscIndex)) {
                        if (stscIndex == 0) {
                            return false;
                        }
                        stscIndex -= 1;
                        break;
                    }
                }
                if (stscIndex == numStsc) {
                    stscIndex -= 1;
                }

                uint32_t stsdIndex =
                track.m_pStscSampleDescrIndexProperty->GetValue(stscIndex);
                if (stsdIndex == 0) {
                    return false;
                }

                MP4Atom* stsd =
                track.GetTrakAtom().FindAtom("trak.mdia.minf.stbl.stsd");
                if (!stsd || stsdIndex > stsd->GetNumberOfChildAtoms()) {
                    return false;
                }

                MP4Atom* entry = stsd->GetChildAtom(stsdIndex - 1);
                if (!entry || !strequal(entry->GetType(), "tmcd")) {
                    return false;
                }

                MP4Integer16Property* drefIndexProp = NULL;
                MP4BytesProperty* flagsProp = NULL;
                MP4Integer32Property* timeScaleProp = NULL;
                MP4Integer32Property* frameDurationProp = NULL;
                MP4Integer8Property* numFramesProp = NULL;

                if (!entry->FindProperty(
                "*.dataReferenceIndex", (MP4Property**)&drefIndexProp) ||
                !entry->FindProperty("*.flags", (MP4Property**)&flagsProp) ||
                !entry->FindProperty("*.timeScale", (MP4Property**)&timeScaleProp) ||
                !entry->FindProperty(
                "*.frameDuration", (MP4Property**)&frameDurationProp) ||
                !entry->FindProperty("*.numFrames", (MP4Property**)&numFramesProp)) {
                    return false;
                }

                if (flagsProp->GetValueSize() != 4) {
                    return false;
                }
                uint8_t flagBytes[4];
                flagsProp->CopyValue(flagBytes, 0);
                desc.flags = ((uint32_t)flagBytes[0] << 24) |
                ((uint32_t)flagBytes[1] << 16) |
                ((uint32_t)flagBytes[2] << 8) |
                (uint32_t)flagBytes[3];

                desc.timeScale = timeScaleProp->GetValue();
                desc.frameDuration = frameDurationProp->GetValue();
                desc.numFrames = (uint8_t)numFramesProp->GetValue();

                if (desc.timeScale == 0 || desc.frameDuration == 0 ||
                desc.numFrames == 0) {
                    return false;
                }

                // only the local, self-contained data reference is in scope
                MP4Atom* dref =
                track.GetTrakAtom().FindAtom("trak.mdia.minf.dinf.dref");
                if (!dref) {
                    return false;
                }
                uint32_t drefIndex = drefIndexProp->GetValue();
                if (drefIndex == 0 || drefIndex > dref->GetNumberOfChildAtoms()) {
                    return false;
                }
                MP4Atom* urlAtom = dref->GetChildAtom(drefIndex - 1);
                if (!urlAtom || !strequal(urlAtom->GetType(), "url ") ||
                !(urlAtom->GetFlags() & 1)) {
                    return false;
                }

                return true;
            }

            // Locate the visible video sample whose presentation interval contains
            // the movie time, and express the left edge of that visible interval as
            // an exact rational movie time (the query anchor).
            static bool locateVideoFrame(
            MP4Track& video,
            uint64_t movieTimeScale,
            uint64_t movieWhen,
            const std::vector<EditEntry>& edits,
            MP4SampleId& videoSampleId,
            Rational& anchorMovie)
            {
                const uint64_t trackTimeScale = video.GetTimeScale();
                if (trackTimeScale == 0) {
                    return false;
                }

                int64_t editMediaTime = 0;
                uint64_t editStartMovie = 0;
                const bool inEdit = !edits.empty();

                if (inEdit) {
                    uint64_t elapsed = 0;
                    bool covered = false;
                    for (uint32_t i = 0; i < edits.size(); i++) {
                        uint64_t next;
                        if (!safeAddU(
                        elapsed, edits[i].segmentDuration, next)) {
                            return false;
                        }
                        if (movieWhen < next) {
                            if (edits[i].mediaTime < 0) {
                                return false; // empty edit
                            }
                            editStartMovie = elapsed;
                            editMediaTime = edits[i].mediaTime;
                            covered = true;
                            break;
                        }
                        elapsed = next;
                    }
                    if (!covered) {
                        return false; // movie time is not covered
                    }
                }

                const uint32_t numSamples = video.GetNumberOfSamples();
                if (numSamples == 0) {
                    return false;
                }

                for (MP4SampleId sid = 1; sid <= numSamples; sid++) {
                    uint64_t dts, duration, ctts;
                    if (!sampleTiming(
                    video, sid, dts, duration, ctts, true)) {
                        return false;
                    }
                    uint64_t pts;
                    if (!safeAddU(dts, ctts, pts) || duration == 0) {
                        return false;
                    }
                    uint64_t sampleEnd;
                    if (!safeAddU(pts, duration, sampleEnd)) {
                        return false;
                    }

                    if (inEdit) {
                        // mediaWhen = editMediaTime +
                        //   (movieWhen - editStartMovie) *
                        //   trackTimeScale / movieTimeScale
                        const uint64_t localMovie = movieWhen - editStartMovie;
                        uint64_t num, den;
                        if (!safeMulU(localMovie, trackTimeScale, num) ||
                        !safeMulU(movieTimeScale, 1u, den)) {
                            return false;
                        }

                        uint64_t baseNum;
                        if (!safeMulU(
                        (uint64_t)editMediaTime, den, baseNum) ||
                        !safeAddU(baseNum, num, baseNum)) {
                            return false;
                        }
                        uint64_t startScaled, endScaled;
                        if (!safeMulU(pts, den, startScaled) ||
                        !safeMulU(sampleEnd, den, endScaled)) {
                            return false;
                        }
                        if (baseNum < startScaled || baseNum >= endScaled) {
                            continue;
                        }

                        // left edge of the visible interval, mapped back to
                        // movie time
                        const uint64_t mediaStart =
                        pts > (uint64_t)editMediaTime
                        ? pts : (uint64_t)editMediaTime;
                        const uint64_t intoMedia =
                        mediaStart - (uint64_t)editMediaTime;

                        uint64_t anchorNum, anchorDen;
                        if (!safeMulU(intoMedia, movieTimeScale, anchorNum) ||
                        !safeMulU(
                        editStartMovie, trackTimeScale, anchorDen) ||
                        !safeAddU(anchorNum, anchorDen, anchorNum) ||
                        !safeMulU(trackTimeScale, 1u, anchorDen)) {
                            return false;
                        }
                        anchorMovie.num = anchorNum;
                        anchorMovie.den = anchorDen;
                        reduceRat(anchorMovie.num, anchorMovie.den);
                        videoSampleId = sid;
                        return true;
                    }

                    // no edit list: media time derives from movie time zero
                    uint64_t num, den;
                    if (!safeMulU(movieWhen, trackTimeScale, num)) {
                        return false;
                    }
                    den = movieTimeScale;

                    uint64_t ptsScaled, endScaled;
                    if (!safeMulU(pts, den, ptsScaled) ||
                    !safeMulU(sampleEnd, den, endScaled)) {
                        return false;
                    }
                    if (num < ptsScaled || num >= endScaled) {
                        continue;
                    }

                    if (!safeMulU(pts, movieTimeScale, anchorMovie.num)) {
                        return false;
                    }
                    anchorMovie.den = trackTimeScale;
                    reduceRat(anchorMovie.num, anchorMovie.den);
                    videoSampleId = sid;
                    return true;
                }

                return false;
            }

            // Walk the timecode track edits with the rational movie-time anchor and
            // produce the corresponding rational timecode media time, then select
            // the counter sample covering it.
            static bool locateCounter(
            MP4Track& timecode,
            uint64_t movieTimeScale,
            const Rational& anchorMovie,
            const std::vector<EditEntry>& edits,
            MP4SampleId& counterSampleId,
            uint64_t& counterDts,
            Rational& timecodeMedia)
            {
                const uint64_t tcTimeScale = timecode.GetTimeScale();
                if (tcTimeScale == 0) {
                    return false;
                }

                uint64_t mediaNum = 0;
                uint64_t mediaDen = 1;

                if (edits.empty()) {
                    if (!safeMulU(anchorMovie.num, tcTimeScale, mediaNum) ||
                    !safeMulU(
                    anchorMovie.den, movieTimeScale, mediaDen)) {
                        return false;
                    }
                    } else {
                    uint64_t elapsed = 0;
                    bool found = false;
                    int64_t editMediaTime = 0;

                    for (uint32_t i = 0; i < edits.size(); i++) {
                        uint64_t next;
                        if (!safeAddU(
                        elapsed, edits[i].segmentDuration, next)) {
                            return false;
                        }
                        // elapsed <= anchorMovie < next, compared exactly
                        if (cmpRat(
                        anchorMovie.num, anchorMovie.den, next) >= 0) {
                            elapsed = next;
                            continue;
                        }
                        if (cmpRat(
                        anchorMovie.num, anchorMovie.den,
                        elapsed) < 0) {
                            return false;
                        }
                        if (edits[i].mediaTime < 0) {
                            return false; // anchor falls in an empty edit
                        }
                        editMediaTime = edits[i].mediaTime;
                        found = true;
                        break;
                    }
                    if (!found) {
                        return false;
                    }

                    uint64_t startScaled;
                    if (!safeMulU(elapsed, anchorMovie.den, startScaled) ||
                    anchorMovie.num < startScaled) {
                        return false;
                    }
                    const uint64_t localNum = anchorMovie.num - startScaled;
                    const uint64_t localDen = anchorMovie.den;

                    uint64_t scaledNum, scaledDen;
                    if (!safeMulU(localNum, tcTimeScale, scaledNum) ||
                    !safeMulU(localDen, movieTimeScale, scaledDen)) {
                        return false;
                    }
                    if (!safeMulU(
                    (uint64_t)editMediaTime, scaledDen, mediaNum) ||
                    !safeAddU(mediaNum, scaledNum, mediaNum)) {
                        return false;
                    }
                    mediaDen = scaledDen;
                }

                timecodeMedia.num = mediaNum;
                timecodeMedia.den = mediaDen;
                reduceRat(timecodeMedia.num, timecodeMedia.den);

                const uint32_t numSamples = timecode.GetNumberOfSamples();
                if (numSamples == 0) {
                    return false;
                }

                for (MP4SampleId sid = 1; sid <= numSamples; sid++) {
                    uint64_t dts, duration, unused;
                    if (!sampleTiming(
                    timecode, sid, dts, duration, unused, false)) {
                        return false;
                    }
                    if (duration == 0) {
                        return false;
                    }
                    uint64_t sampleEnd;
                    if (!safeAddU(dts, duration, sampleEnd)) {
                        return false;
                    }
                    uint64_t startScaled, endScaled;
                    if (!safeMulU(dts, mediaDen, startScaled) ||
                    !safeMulU(sampleEnd, mediaDen, endScaled)) {
                        return false;
                    }
                    if (mediaNum >= startScaled && mediaNum < endScaled) {
                        counterSampleId = sid;
                        counterDts = dts;
                        return true;
                    }
                }
                return false;
            }
        };

        ///////////////////////////////////////////////////////////////////////////////

        extern "C" MP4V2_EXPORT
        bool MP4GetVideoTimeCode(
        MP4FileHandle hFile,
        MP4TrackId videoTrack,
        MP4Timestamp movieWhen,
        MP4VideoTimeCode* out)
        {
            if (!MP4_IS_VALID_FILE_HANDLE(hFile) || out == NULL) {
                return false;
            }
            try {
                return TimecodeDetails::get(
                *static_cast<MP4File*>(hFile),
                videoTrack, (uint64_t)movieWhen, *out);
            }
            catch (Exception* x) {
                log.errorf(*x);
                delete x;
            }
            catch (...) {
                log.errorf("%s: failed", __FUNCTION__);
            }
            return false;
        }

        ///////////////////////////////////////////////////////////////////////////////

}} // namespace mp4v2::impl
