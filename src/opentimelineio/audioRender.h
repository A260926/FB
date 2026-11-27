// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#pragma once

#include "opentimelineio/errorStatus.h"
#include "opentimelineio/pcmWave.h"
#include "opentimelineio/version.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

class Timeline;

/// @brief Render an OTIO Timeline's audio tracks into interleaved stereo float.
///
/// The renderer takes an independent deep snapshot of the timeline at
/// construction time, so later edits to the supplied Timeline never affect an
/// existing renderer. Media is read on demand from local RIFF/WAVE PCM16
/// files referenced by ExternalReference; neither the whole media nor the
/// whole mix is ever resident in memory.
///
/// Mixing rules:
///  * Only Audio tracks under Timeline.tracks() contribute; Video tracks are
///    ignored. Tracks are summed.
///  * Tracks lay out their children sequentially; Stacks sum their audio
///    children. Gaps and disabled branches are silence.
///  * The render covers the timeline's full duration. Output frame n sits at
///    program time t = n / output_rate, kept as a fractional time and mapped
///    through the parent chain to source seconds.
///  * Source sample k sits at k / source_rate; values between adjacent source
///    frames are linearly interpolated, and anything past the physical end of
///    the media is silence. Mono media is duplicated to both channels.
///  * A SMPTE_Dissolve covers the half-open range_in_parent interval and
///    crossfades its two neighboring ordinary clips using
///    cos(pi*u/2)/sin(pi*u/2), where u is the full, un-rebased transition
///    progress. The clips' source times extend into both handles, and no
///    ordinary segment is added inside the transition.
///  * global_start_time is a timecode label only and never offsets output.
///
/// Any unsupported construct (time effects, non-dissolve transitions,
/// unreadable or non-PCM visible media, overlapping transitions, invalid
/// times, ...) makes construction or the next read() fail with an error rather
/// than producing a partial render. A zero-duration timeline is valid.
class OTIO_API_TYPE AudioRenderer
{
public:
    /// @brief Construct a renderer from a timeline.
    ///
    /// @param timeline The timeline to render; it is deep-copied, so the
    ///     renderer does not retain or mutate the passed object.
    /// @param media_root Directory used to resolve relative ExternalReference
    ///     target URLs. Absolute targets are used as-is.
    /// @param output_sample_rate Positive integer output rate in Hz.
    /// @param error_status Set when the timeline cannot be rendered at all;
    ///     the resulting renderer must not be used when an error is reported.
    AudioRenderer(
        Timeline const&        timeline,
        std::string const&     media_root,
        int                    output_sample_rate,
        ErrorStatus*           error_status = nullptr);

    ~AudioRenderer();

    AudioRenderer(AudioRenderer const&)            = delete;
    AudioRenderer& operator=(AudioRenderer const&) = delete;

    /// @brief Return the output sample rate the renderer was built with.
    int output_sample_rate() const noexcept;

    /// @brief Return the total number of stereo frames in the render.
    ///
    /// This is ceil(duration_seconds * output_rate), computed once from the
    /// whole timeline rather than per segment.
    int64_t frame_count() const noexcept;

    /// @brief Return the current zero-based read position in output frames.
    int64_t position() const noexcept;

    /// @brief Reposition the read cursor.
    ///
    /// @param frame Zero-based output frame. Seeking to frame_count() is
    ///     legal and makes the next read() return zero frames; seeking past
    ///     it (or below zero) is an error and leaves the position unchanged.
    /// @param error_status Set to seek_out_of_range on a bad position.
    bool seek(int64_t frame, ErrorStatus* error_status = nullptr);

    /// @brief Read up to requested_frame_count interleaved stereo frames.
    ///
    /// Fewer frames (down to zero at end of stream) may be returned. The
    /// output is independent of the block size: reading in chunks, re-reading
    /// after seek(), or reading everything at once yields identical samples.
    ///
    /// On failure the error is reported and no guarantee is made about the
    /// cursor or partial contents; the caller must treat the render as failed.
    ///
    /// @param requested_frame_count Maximum frames to produce.
    /// @param out Interleaved L/R floats; cleared then filled.
    /// @param error_status Detailed failure.
    /// @return True on success (including a clean end-of-stream), false on a
    ///     render or media error.
    bool read(
        int64_t             requested_frame_count,
        std::vector<float>& out,
        ErrorStatus*        error_status = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
