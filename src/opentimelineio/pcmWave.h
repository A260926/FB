// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#pragma once

#include "opentimelineio/errorStatus.h"
#include "opentimelineio/version.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

/// @brief Error codes specific to PCM/WAVE audio I/O and rendering.
///
/// These extend the generic OTIO ErrorStatus outcomes so that the renderer can
/// report precise failures while still using the standard ErrorStatus channel.
enum class AudioRenderError
{
    /// No error.
    none = 0,
    /// An argument to a public entry point was invalid.
    invalid_argument,
    /// The timeline contains a structure the renderer does not support.
    unsupported_timeline,
    /// A time or time range in the timeline is invalid.
    invalid_time,
    /// Two transition coverage ranges overlap, which is ambiguous.
    overlapping_transitions,
    /// A transition does not have the two ordinary clips it needs.
    transition_without_neighbors,
    /// A media reference is not a readable local ExternalReference.
    invalid_media_reference,
    /// The WAVE file is missing, unreadable or malformed.
    unreadable_media,
    /// The requested seek position lies outside the render.
    seek_out_of_range,
    /// An I/O or allocation failure happened while streaming.
    io_failure
};

/// @brief Return a stable, human readable name for an AudioRenderError.
OTIO_API std::string
audio_render_error_name(AudioRenderError error) noexcept;

/// @brief Describe a decoded RIFF/WAVE PCM16 source file.
///
/// Only uncompressed PCM (format tag 1), 16-bit samples, one or two channels
/// and positive integer sample rates are accepted. The parser walks every
/// chunk, honoring the even-byte padding appended to odd sized chunks, so it
/// never assumes the canonical 44-byte layout.
class OTIO_API_TYPE PcmWaveSource
{
public:
    /// @brief Open and decode the header of a local RIFF/WAVE PCM16 file.
    ///
    /// The file is opened once and kept open for on-demand sample reads. Only
    /// the requested sample windows are ever loaded into memory.
    ///
    /// @param file_path Absolute path to the .wav file.
    /// @param error_status Set with a detailed message on failure.
    PcmWaveSource(
        std::string const& file_path,
        ErrorStatus*       error_status = nullptr);

    ~PcmWaveSource();

    PcmWaveSource(PcmWaveSource const&)            = delete;
    PcmWaveSource& operator=(PcmWaveSource const&) = delete;

    /// @brief Return the file's sample rate in Hz (a positive integer).
    int sample_rate() const noexcept { return _sample_rate; }

    /// @brief Return the channel count: 1 (mono) or 2 (stereo).
    int channels() const noexcept { return _channels; }

    /// @brief Return the total number of sample frames in the data chunk.
    int64_t frame_count() const noexcept { return _frame_count; }

    /// @brief Read a contiguous run of interleaved source frames.
    ///
    /// The returned samples are normalized to the [-1, 1] float range. A mono
    /// source yields one value per frame; a stereo source yields two. Frames
    /// beyond the physical end of the file are returned as silence, so callers
    /// never have to clamp the request themselves.
    ///
    /// @param first_frame Zero-based index of the first source frame.
    /// @param frame_count Number of frames to read.
    /// @param out Destination; resized to frame_count * channels().
    /// @param error_status Set on an I/O failure.
    OTIO_API bool read_frames(
        int64_t              first_frame,
        int64_t              frame_count,
        std::vector<float>&  out,
        ErrorStatus*         error_status = nullptr);

private:
    std::ifstream _stream;
    std::string   _path;
    int64_t       _data_offset;
    int64_t       _frame_count;
    int           _sample_rate;
    int           _channels;
};

/// @brief Helper for writing a 16-bit stereo PCM WAVE file incrementally.
///
/// The header is written up front with a placeholder data size and patched on
/// close(), so audio can be streamed in blocks without buffering the whole
/// mix. If close() is not reached, the caller is expected to delete the
/// partially written file itself.
class OTIO_API_TYPE PcmWaveWriter
{
public:
    /// @brief Create (or truncate) a stereo PCM16 WAVE file.
    ///
    /// @param file_path Destination path.
    /// @param sample_rate Positive integer output sample rate.
    /// @param error_status Set with a detailed message on failure.
    PcmWaveWriter(
        std::string const& file_path,
        int                sample_rate,
        ErrorStatus*       error_status = nullptr);

    ~PcmWaveWriter();

    PcmWaveWriter(PcmWaveWriter const&)            = delete;
    PcmWaveWriter& operator=(PcmWaveWriter const&) = delete;

    /// @brief Append interleaved stereo float frames.
    ///
    /// Samples are first clamped to [-1, 32767/32768], then rounded to the
    /// nearest integer. Exact half values round away from zero.
    ///
    /// @param frames Interleaved left/right floats; size must be even.
    OTIO_API bool write_frames(
        std::vector<float> const& frames,
        ErrorStatus*              error_status = nullptr);

    /// @brief Patch the RIFF/data sizes and flush the file.
    ///
    /// After a successful close() the file is a complete, playable WAVE.
    /// Returns false (and sets error_status) if any write or flush failed.
    OTIO_API bool close(ErrorStatus* error_status = nullptr);

    /// @brief Return the number of stereo frames written so far.
    int64_t frame_count() const noexcept { return _frames_written; }

private:
    std::ofstream _stream;
    std::string   _path;
    int64_t       _frames_written;
    bool          _closed;
};

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
