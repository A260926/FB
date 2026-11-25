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

// Timecode example: print the source timecode recorded for a movie time.
//
// Usage: timecode <file> <video track id> <movie time>
//
// The movie time is expressed in the movie time scale (see
// MP4GetTimeScale()); it must not be derived from a player's playback
// position. The resolved timecode comes from the QuickTime timecode track
// referenced by the video track's tref.tmcd box.

#include <mp4v2/mp4v2.h>

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::fprintf(stderr,
            "usage: %s <file> <video-track-id> <movie-time>\n", argv[0]);
        return 2;
    }

    char* end = NULL;
    long videoTrackId = std::strtol(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' || videoTrackId <= 0) {
        std::fprintf(stderr, "invalid video track id: %s\n", argv[2]);
        return 2;
    }

    end = NULL;
    long long movieWhen = std::strtoll(argv[3], &end, 10);
    if (end == argv[3] || *end != '\0' || movieWhen < 0) {
        std::fprintf(stderr, "invalid movie time: %s\n", argv[3]);
        return 2;
    }

    MP4FileHandle file = MP4Read(argv[1]);
    if (file == MP4_INVALID_FILE_HANDLE) {
        std::fprintf(stderr, "cannot open file: %s\n", argv[1]);
        return 3;
    }

    MP4VideoTimeCode tc;
    const bool ok = MP4GetVideoTimeCode(
        file, (MP4TrackId)videoTrackId, (MP4Timestamp)movieWhen, &tc);

    if (!ok) {
        std::fprintf(stderr,
            "no source timecode for track %lu at movie time %lld\n",
            videoTrackId, movieWhen);
        MP4Close(file);
        return 4;
    }

    std::printf("timecode %s\n", tc.text);
    std::printf("videoSampleId %u\n", (unsigned)tc.videoSampleId);
    std::printf("timeCodeTrackId %u\n", (unsigned)tc.timeCodeTrackId);
    std::printf("timeCodeSampleId %u\n", (unsigned)tc.timeCodeSampleId);
    std::printf("frameNumber %llu\n",
                (unsigned long long)tc.frameNumber);

    MP4Close(file);
    return 0;
}
