// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#pragma once

#include "opentimelineio/export.h"
#include "opentimelineio/version.h"

#include <cstdint>
#include <memory>
#include <string>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

/// @brief Error categories reported by PcmWaveReader.
enum class PcmWaveError
{
    /// @brief No error.
    None = 0,
    /// @brief A function argument was invalid (negative frame index, ...).
    InvalidArgument,
    /// @brief The file could not be opened or stat'ed.
    FileOpenFailed,
    /// @brief The file is RIFF/WAVE but not PCM16 mono/stereo.
    UnsupportedFormat,
    /// @brief The RIFF/chunk structure is truncated or malformed.
    MalformedFile,
    /// @brief Reading audio payload failed after a successful open.
    ReadFailed,
};

/// @brief Result of a PcmWaveReader operation.
struct OTIO_API_TYPE PcmWaveStatus
{
    PcmWaveError error = PcmWaveError::None;
    std::string  message;

    bool ok() const noexcept { return error == PcmWaveError::None; }
    explicit operator bool() const noexcept { return ok(); }
};

/// @brief Streaming reader for RIFF/WAVE PCM16 media.
///
/// The reader parses the complete RIFF chunk list (fmt, data and any unknown
/// chunks, honouring the even-byte padding of odd sized chunks), so files are
/// not required to use a canonical 44 byte header.
///
/// Audio data is read on demand.  The only memory held by the reader is a
/// small read-through cache of decoded stereo float frames, so a renderer
/// never has to hold a whole media file (or a whole render) in memory.  Mono
/// media is expanded to stereo by duplicating the channel.  Frames that lie
/// outside the physical media are reported as silence.
class OTIO_API_TYPE PcmWaveReader
{
public:
    /// @brief Create a closed reader.
    OTIO_API PcmWaveReader();

    OTIO_API ~PcmWaveReader();

    PcmWaveReader(PcmWaveReader const&)            = delete;
    PcmWaveReader& operator=(PcmWaveReader const&) = delete;

    OTIO_API PcmWaveReader(PcmWaveReader&&) noexcept;
    OTIO_API PcmWaveReader& operator=(PcmWaveReader&&) noexcept;

    /// @brief Open @p path and parse its RIFF/WAVE header.
    ///
    /// The audio payload is not loaded here; it is read lazily by
    /// sample_at() and read_frames().
    OTIO_API bool
    open(std::string const& path, PcmWaveStatus* status = nullptr);

    /// @brief Close the file and discard the cache.
    OTIO_API void close() noexcept;

    /// @brief Return whether a file is open.
    OTIO_API bool is_open() const noexcept;

    /// @brief Return the path passed to open().
    OTIO_API std::string const& path() const noexcept;

    /// @brief Return the media sample rate in Hz (0 when closed).
    OTIO_API double sample_rate() const noexcept;

    /// @brief Return the number of channels (1 or 2, 0 when closed).
    OTIO_API int channel_count() const noexcept;

    /// @brief Return the number of complete frames present in the data chunk.
    OTIO_API int64_t frame_count() const noexcept;

    /// @brief Set the number of frames decoded per cache fill (default 4096).
    OTIO_API void set_cache_frame_count(int64_t frames) noexcept;

    /// @brief Read @p frame_count stereo frames starting at @p start_frame.
    ///
    /// @p stereo_out must hold 2 * frame_count floats.  Frames at or past the
    /// end of the media are written as zero.
    OTIO_API bool read_frames(
        int64_t        start_frame,
        int64_t        frame_count,
        float*         stereo_out,
        PcmWaveStatus* status = nullptr);

    /// @brief Sample the media at a fractional frame position.
    ///
    /// Media frame k is located at time k / sample_rate().  Positions between
    /// two existing frames are linearly interpolated.  Positions outside the
    /// physical frame span, or past a truncated media, are reported as zero.
    OTIO_API bool sample_at(
        double         frame_position,
        float&         left,
        float&         right,
        PcmWaveStatus* status = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
