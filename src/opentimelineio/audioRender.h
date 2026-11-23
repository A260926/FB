// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#pragma once

#include "opentimelineio/export.h"
#include "opentimelineio/timeline.h"
#include "opentimelineio/version.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

/// @brief Error categories reported by AudioRenderer.
enum class AudioRenderError
{
    /// @brief No error.
    None = 0,
    /// @brief A function argument was invalid.
    InvalidArgument,
    /// @brief The timeline (or one of its branches) is not renderable.
    InvalidTimeline,
    /// @brief A transition, time effect or other graph feature is unsupported.
    UnsupportedFeature,
    /// @brief Referenced media cannot be found or opened.
    MissingMedia,
    /// @brief Media exists but its audio payload could not be read.
    MediaReadError,
    /// @brief A requested seek position is outside the render interval.
    InvalidSeek,
};

/// @brief Result of an AudioRenderer operation.
struct OTIO_API_TYPE AudioRenderStatus
{
    AudioRenderError error = AudioRenderError::None;
    std::string      message;

    bool ok() const noexcept { return error == AudioRenderError::None; }
    explicit operator bool() const noexcept { return ok(); }
};

/// @brief Renders the audio tracks of a Timeline as interleaved stereo float.
///
/// The renderer takes a private, deep snapshot of the timeline at
/// construction time: the input Timeline is never mutated and later edits to
/// it do not affect the renderer.
///
/// ### Time model
///
/// Output frame @c n corresponds to program time @c n / output_rate seconds,
/// retained as a fractional value.  That program time is mapped up the
/// composition chain: every layer's @c source_range start is the local source
/// coordinate of that object and the beginning of the object in its parent
/// corresponds to that coordinate.  The Timeline's @c global_start_time is a
/// timecode only and does not shift the render.
///
/// The render interval is the whole duration of the project, and the number
/// of output frames is @c ceil(duration_seconds * output_rate); it is not
/// rounded per segment.
///
/// ### Mixing
///
/// Under @c Timeline::tracks only audio tracks contribute; video tracks are
/// ignored.  Audio branches may nest Tracks and Stacks: Tracks are laid out
/// sequentially, Stack children are summed, and disabled branches are muted.
/// Gaps, and time beyond the end of a shorter track, are silence.
///
/// A SMPTE_Dissolve is evaluated over its half-open @c range_in_parent
/// interval by taking the two neighbouring Clips and blending them with
/// cos(pi*u/2) and sin(pi*u/2), where @c u is the full transition progress.
/// Source sampling extends into the transition handles on both sides, and a
/// nested trim that cuts into a transition keeps the original progress.
///
/// ### Media
///
/// Clips must reference local RIFF/WAVE PCM16 media (mono or stereo) through
/// an ExternalReference.  Paths are resolved relative to the media root.  Mono
/// media is duplicated to both channels.  Media frame @c k is at
/// @c k / sample_rate seconds, adjacent frames are linearly interpolated, and
/// positions outside the physical media are silence.  Media is read on demand
/// and never fully buffered.
///
/// The resulting float mix is not clamped.
///
/// ### Errors
///
/// TimeEffects, transition types other than SMPTE_Dissolve, transitions
/// without two adjacent Clips, intersecting transition intervals, invalid
/// times and unreadable visible media are reported as errors.  A failed read
/// leaves the output buffer and the read position untouched, so a partial
/// render is never produced.  A zero-duration project renders zero frames.
class OTIO_API_TYPE AudioRenderer
{
public:
    /// @brief Construct a renderer from a snapshot of @p timeline.
    ///
    /// @param timeline The timeline to render.  It is deep copied and never
    /// modified.
    /// @param media_root Directory against which relative media paths are
    /// resolved.
    /// @param output_rate Output sample rate in Hz.  Must be finite and
    /// greater than zero.
    /// @param status Optional error report.  On failure the renderer is left
    /// in an invalid state; is_valid() returns false.
    OTIO_API AudioRenderer(
        Timeline const*     timeline,
        std::string         media_root,
        double              output_rate,
        AudioRenderStatus*  status = nullptr);

    OTIO_API ~AudioRenderer();

    AudioRenderer(AudioRenderer const&)            = delete;
    AudioRenderer& operator=(AudioRenderer const&) = delete;

    OTIO_API AudioRenderer(AudioRenderer&&) noexcept;
    OTIO_API AudioRenderer& operator=(AudioRenderer&&) noexcept;

    /// @brief Return whether construction succeeded.
    OTIO_API bool is_valid() const noexcept;

    /// @brief Return the output sample rate in Hz.
    OTIO_API double output_rate() const noexcept;

    /// @brief Return the duration of the snapshotted project.
    OTIO_API RationalTime duration() const noexcept;

    /// @brief Return the total number of output frames.
    ///
    /// This is @c ceil(duration_seconds * output_rate).
    OTIO_API int64_t frame_count() const noexcept;

    /// @brief Return the current read position as a zero based output frame.
    OTIO_API int64_t tell() const noexcept;

    /// @brief Seek to the zero based output frame @p frame.
    ///
    /// Valid positions are @c [0, frame_count()].  An out-of-range seek
    /// reports InvalidSeek and leaves the position unchanged.
    OTIO_API bool seek(int64_t frame, AudioRenderStatus* status = nullptr);

    /// @brief Read up to @p frame_count frames of interleaved stereo float.
    ///
    /// @p stereo_out must hold at least @c 2*frame_count floats.  Fewer than
    /// @p frame_count frames are returned when the render interval ends; at
    /// the end zero frames are returned.
    ///
    /// @return The number of frames written, or -1 on error.  On error no
    /// frames are written and the position is unchanged.
    OTIO_API int64_t read(
        int64_t            frame_count,
        float*             stereo_out,
        AudioRenderStatus* status = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
