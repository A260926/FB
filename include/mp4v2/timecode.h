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
 *
 * The Initial Developer of the Original Code is the MP4v2 project.
 * Portions created by the MP4v2 project are
 * Copyright (C) the MP4v2 project. All Rights Reserved.
 */
#ifndef MP4V2_TIMECODE_H
#define MP4V2_TIMECODE_H

/**************************************************************************//**
 *
 *  @defgroup mp4_timecode MP4v2 Video Timecode
 *  @{
 *
 *****************************************************************************/

/** Video timecode query result.
 *
 *  This structure describes the source timecode (the value recorded in the
 *  QuickTime timecode track associated with a video track) for a single
 *  movie time.
 *
 *  All sample identifiers are 1-based; the recorded frame number is
 *  0-based. @c text is always NUL-terminated.
 */
typedef struct MP4VideoTimeCode_s {
    /** 1-based sample id of the visible video frame. */
    MP4SampleId videoSampleId;

    /** track id of the timecode track referenced by the video track. */
    MP4TrackId  timeCodeTrackId;

    /** 1-based sample id of the timecode (counter) sample in effect. */
    MP4SampleId timeCodeSampleId;

    /** 0-based absolute frame number recorded by the timecode sample. */
    uint64_t    frameNumber;

    /** NUL-terminated timecode label, e.g. "01:00:00:02" or "00:01:00;02". */
    char        text[16];
} MP4VideoTimeCode;

/** Get the source timecode for a video frame.
 *
 *  MP4GetVideoTimeCode resolves the movie time @p movieWhen to the visible
 *  video frame of track @p videoTrack using that track's edit list and the
 *  samples' presentation timestamps (DTS plus composition offset, never DTS
 *  alone), then follows the video track's @c tref.tmcd reference to its
 *  associated timecode track and reads the counter sample in effect for the
 *  left edge of that visible frame.
 *
 *  The timecode is computed entirely with integer rational arithmetic:
 *  the recorded four-byte counter value (the frame number at the start of
 *  the counter sample) plus the whole number of source frames between the
 *  counter sample's decode time and the timecode media time corresponding
 *  to the frame anchor, scaled by the timecode sample description's
 *  timeScale and frameDuration. Every query time falling inside the same
 *  visible video frame yields the identical result.
 *
 *  The label follows the QuickTime timecode sample description:
 *      @li non-drop-frame integer rates 24, 25, 30, 50 and 60 fps produce
 *          <tt>HH:MM:SS:FF</tt>;
 *      @li drop-frame is accepted only for 29.97 fps
 *          (timeScale/frameDuration 30000/1001, 30 frames) or 59.94 fps
 *          (60000/1001, 60 frames) and produces <tt>HH:MM:SS;FF</tt>,
 *          skipping the first 2 (or 4) frame numbers of every minute that
 *          is not a ten-minute boundary;
 *      @li the 24-hour-max flag only wraps the label back to zero after a
 *          full day while @c frameNumber keeps its complete, unwrapped
 *          value; without that flag hours above 99 are rejected.
 *
 *  Only ordinary, self-contained, non-fragmented, unencrypted files are
 *  supported. The timecode sample description must use the local data
 *  reference and contain no counter flag, negative timecodes are not
 *  supported and only non-negative composition offsets and unit-rate,
 *  positive-duration media edits (plus empty edits with media time -1) are
 *  accepted. The function never modifies the file: it does not reopen it,
 *  rescan boxes or bypass the existing track and sample reading code, and
 *  ordinary MP4ReadSample() results for pre-existing tracks are unchanged.
 *  Timecode tracks additionally become readable through MP4ReadSample(),
 *  which returns the raw counter payload.
 *
 *  @param hFile handle of file for the operation.
 *  @param videoTrack id of the video track carrying the @c tref.tmcd
 *      reference.
 *  @param movieWhen movie time, in the movie time scale, to resolve.
 *  @param out address of a caller-provided result structure. On failure the
 *      structure is left completely untouched.
 *
 *  @return <b>true</b> on success, <b>false</b> if the time falls in an
 *      empty edit or outside any frame or counter sample, the timecode
 *      association is missing or ambiguous, any required table is invalid
 *      or unsupported, a time base is zero, a rate other than 1.0 is
 *      encountered, an arithmetic overflow occurs, or the timecode
 *      description is unsupported.
 */
MP4V2_EXPORT
bool MP4GetVideoTimeCode(
    MP4FileHandle  hFile,
    MP4TrackId     videoTrack,
    MP4Timestamp   movieWhen,
    MP4VideoTimeCode* out );

/** @} ***********************************************************************/

#endif /* MP4V2_TIMECODE_H */
