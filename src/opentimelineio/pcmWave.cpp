// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#include "opentimelineio/pcmWave.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

namespace {

uint16_t
read_u16(unsigned char const* p)
{
    return uint16_t(p[0] | (uint16_t(p[1]) << 8));
}

uint32_t
read_u32(unsigned char const* p)
{
    return uint32_t(
        p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16)
        | (uint32_t(p[3]) << 24));
}

void
write_u16(std::ostream& stream, uint16_t value)
{
    char bytes[2] = {
        char(value & 0xff),
        char((value >> 8) & 0xff)
    };
    stream.write(bytes, 2);
}

void
write_u32(std::ostream& stream, uint32_t value)
{
    char bytes[4] = {
        char(value & 0xff),
        char((value >> 8) & 0xff),
        char((value >> 16) & 0xff),
        char((value >> 24) & 0xff)
    };
    stream.write(bytes, 4);
}

bool
read_exact(std::istream& stream, char* buffer, size_t size)
{
    stream.read(buffer, std::streamsize(size));
    return stream.gcount() == std::streamsize(size);
}

} // namespace

std::string
audio_render_error_name(AudioRenderError error) noexcept
{
    switch (error)
    {
        case AudioRenderError::none:
            return "none";
        case AudioRenderError::invalid_argument:
            return "invalid_argument";
        case AudioRenderError::unsupported_timeline:
            return "unsupported_timeline";
        case AudioRenderError::invalid_time:
            return "invalid_time";
        case AudioRenderError::overlapping_transitions:
            return "overlapping_transitions";
        case AudioRenderError::transition_without_neighbors:
            return "transition_without_neighbors";
        case AudioRenderError::invalid_media_reference:
            return "invalid_media_reference";
        case AudioRenderError::unreadable_media:
            return "unreadable_media";
        case AudioRenderError::seek_out_of_range:
            return "seek_out_of_range";
        case AudioRenderError::io_failure:
            return "io_failure";
    }
    return "unknown";
}

PcmWaveSource::PcmWaveSource(
    std::string const& file_path,
    ErrorStatus*       error_status)
    : _data_offset(0)
    , _frame_count(0)
    , _sample_rate(0)
    , _channels(0)
{
    _path = file_path;
    _stream.open(file_path, std::ios::binary | std::ios::in);
    if (!_stream)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::FILE_OPEN_FAILED,
                "cannot open PCM WAVE source: " + file_path);
        }
        return;
    }

    char riff[12];
    if (!read_exact(_stream, riff, 12) || std::memcmp(riff, "RIFF", 4) != 0
        || std::memcmp(riff + 8, "WAVE", 4) != 0)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::MALFORMED_SCHEMA,
                "not a RIFF/WAVE file: " + file_path);
        }
        return;
    }

    bool     have_fmt = false;
    uint16_t format_tag = 0, bits_per_sample = 0, channels = 0;
    uint32_t sample_rate = 0;
    bool     have_data = false;

    // Walk every chunk. Chunks with an odd payload size are followed by a
    // single padding byte, so the header cannot be assumed to be 44 bytes.
    while (true)
    {
        char header[8];
        if (!read_exact(_stream, header, 8))
        {
            // A clean end-of-file after all chunks is fine.
            _stream.clear();
            break;
        }
        uint32_t chunk_size = read_u32(
            reinterpret_cast<unsigned char*>(header) + 4);
        std::streamoff chunk_start = _stream.tellg();
        if (chunk_start < 0)
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::MALFORMED_SCHEMA,
                    "malformed WAVE chunk in: " + file_path);
            }
            return;
        }

        if (std::memcmp(header, "fmt ", 4) == 0)
        {
            if (chunk_size < 16)
            {
                if (error_status)
                {
                    *error_status = ErrorStatus(
                        ErrorStatus::MALFORMED_SCHEMA,
                        "WAVE fmt chunk too small: " + file_path);
                }
                return;
            }
            char fmt[16];
            if (!read_exact(_stream, fmt, 16))
            {
                if (error_status)
                {
                    *error_status = ErrorStatus(
                        ErrorStatus::MALFORMED_SCHEMA,
                        "truncated WAVE fmt chunk: " + file_path);
                }
                return;
            }
            unsigned char const* p =
                reinterpret_cast<unsigned char*>(fmt);
            format_tag      = read_u16(p);
            channels        = read_u16(p + 2);
            sample_rate     = read_u32(p + 4);
            bits_per_sample = read_u16(p + 14);
            have_fmt        = true;
        }
        else if (std::memcmp(header, "data", 4) == 0)
        {
            if (have_data)
            {
                if (error_status)
                {
                    *error_status = ErrorStatus(
                        ErrorStatus::MALFORMED_SCHEMA,
                        "WAVE file contains multiple data chunks: " + file_path);
                }
                return;
            }
            _data_offset = int64_t(chunk_start);
            _frame_count = int64_t(chunk_size); // frame count derived once the
                                                // block alignment is known
            have_data = true;
        }

        // Skip the remainder of the payload plus the even-byte pad.
        std::streamoff next =
            chunk_start + std::streamoff(chunk_size) + (chunk_size & 1u);
        _stream.clear();
        _stream.seekg(next, std::ios::beg);
        if (!_stream)
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::MALFORMED_SCHEMA,
                    "WAVE chunk extends past end of file: " + file_path);
            }
            return;
        }
    }

    if (!have_fmt || !have_data)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::MALFORMED_SCHEMA,
                "WAVE file is missing fmt and/or data chunk: " + file_path);
        }
        return;
    }
    if (format_tag != 1)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::MALFORMED_SCHEMA,
                "only uncompressed PCM (format 1) is supported: " + file_path);
        }
        return;
    }
    if (bits_per_sample != 16)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::MALFORMED_SCHEMA,
                "only 16-bit PCM is supported: " + file_path);
        }
        return;
    }
    if (channels != 1 && channels != 2)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::MALFORMED_SCHEMA,
                "only mono or stereo PCM is supported: " + file_path);
        }
        return;
    }
    if (sample_rate == 0)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::MALFORMED_SCHEMA,
                "WAVE file has a non-positive sample rate: " + file_path);
        }
        return;
    }

    uint32_t block_align = uint32_t(channels) * (bits_per_sample / 8);
    if (uint32_t(_frame_count) % block_align != 0)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::MALFORMED_SCHEMA,
                "WAVE data chunk is not a whole number of frames: " + file_path);
        }
        return;
    }
    _frame_count   = int64_t(uint32_t(_frame_count) / block_align);
    _sample_rate   = int(sample_rate);
    _channels      = int(channels);
}

PcmWaveSource::~PcmWaveSource() = default;

bool
PcmWaveSource::read_frames(
    int64_t             first_frame,
    int64_t             frame_count,
    std::vector<float>& out,
    ErrorStatus*        error_status)
{
    if (first_frame < 0 || frame_count < 0)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::INVALID_TIME_RANGE,
                "negative frame request on PCM source: " + _path);
        }
        return false;
    }

    out.assign(size_t(frame_count) * size_t(_channels), 0.0f);
    if (frame_count == 0)
    {
        return true;
    }

    // Everything at or past the physical end of the media is silence.
    int64_t available = _frame_count - first_frame;
    if (available <= 0)
    {
        return true;
    }
    int64_t to_read = std::min(frame_count, available);

    int const block_align = _channels * 2;
    _stream.clear();
    _stream.seekg(
        _data_offset + first_frame * int64_t(block_align),
        std::ios::beg);
    if (!_stream)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::INTERNAL_ERROR,
                "seek failed on PCM source: " + _path);
        }
        return false;
    }

    std::vector<unsigned char> raw(size_t(to_read) * block_align);
    if (!read_exact(
            _stream,
            reinterpret_cast<char*>(raw.data()),
            raw.size()))
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::INTERNAL_ERROR,
                "short read from PCM source (truncated file?): " + _path);
        }
        return false;
    }

    for (int64_t i = 0; i < to_read; ++i)
    {
        for (int c = 0; c < _channels; ++c)
        {
            unsigned char const* p =
                raw.data() + (size_t(i) * block_align + size_t(c) * 2);
            int16_t value = int16_t(read_u16(p));
            out[size_t(i) * _channels + c] = float(value) / 32768.0f;
        }
    }
    return true;
}

PcmWaveWriter::PcmWaveWriter(
    std::string const& file_path,
    int                sample_rate,
    ErrorStatus*       error_status)
    : _frames_written(0)
    , _closed(false)
{
    _path = file_path;
    if (sample_rate <= 0)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::INVALID_TIME_RANGE,
                "output sample rate must be a positive integer");
        }
        return;
    }

    _stream.open(file_path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!_stream)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::FILE_OPEN_FAILED,
                "cannot open output WAVE for writing: " + file_path);
        }
        return;
    }

    // Standard 44-byte header; RIFF and data sizes are patched in close().
    _stream.write("RIFF", 4);
    write_u32(_stream, 0);
    _stream.write("WAVE", 4);
    _stream.write("fmt ", 4);
    write_u32(_stream, 16);
    write_u16(_stream, 1);             // PCM
    write_u16(_stream, 2);             // stereo
    write_u32(_stream, uint32_t(sample_rate));
    write_u32(_stream, uint32_t(sample_rate) * 4); // bytes per second
    write_u16(_stream, 4);             // block align
    write_u16(_stream, 16);            // bits per sample
    _stream.write("data", 4);
    write_u32(_stream, 0);
    if (!_stream)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::FILE_WRITE_FAILED,
                "failed writing WAVE header: " + file_path);
        }
    }
}

PcmWaveWriter::~PcmWaveWriter()
{
    if (!_closed && _stream.is_open())
    {
        // Best effort; destructors must not throw.
        _stream.close();
    }
}

bool
PcmWaveWriter::write_frames(
    std::vector<float> const& frames,
    ErrorStatus*              error_status)
{
    if (!_stream)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::FILE_WRITE_FAILED,
                "output WAVE stream is not open: " + _path);
        }
        return false;
    }
    if (frames.size() % 2 != 0)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::INVALID_TIME_RANGE,
                "stereo write requires an even number of samples");
        }
        return false;
    }

    for (float sample: frames)
    {
        // Clamp first to the asymmetric PCM16 float range.
        if (sample > 32767.0f / 32768.0f)
        {
            sample = 32767.0f / 32768.0f;
        }
        else if (sample < -1.0f)
        {
            sample = -1.0f;
        }

        // Quantize to the nearest integer, with exact halves rounding away
        // from zero.
        double scaled = double(sample) * 32768.0;
        int    quantized;
        if (scaled >= 0.0)
        {
            quantized = int(std::floor(scaled + 0.5));
        }
        else
        {
            quantized = int(std::ceil(scaled - 0.5));
        }
        if (quantized > 32767)
        {
            quantized = 32767;
        }
        else if (quantized < -32768)
        {
            quantized = -32768;
        }
        write_u16(_stream, uint16_t(quantized));
    }

    if (!_stream)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::FILE_WRITE_FAILED,
                "failed writing WAVE samples: " + _path);
        }
        return false;
    }
    _frames_written += int64_t(frames.size()) / 2;
    return true;
}

bool
PcmWaveWriter::close(ErrorStatus* error_status)
{
    if (_closed)
    {
        return true;
    }
    if (!_stream)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::FILE_WRITE_FAILED,
                "output WAVE stream is not open: " + _path);
        }
        return false;
    }

    uint32_t data_size = uint32_t(_frames_written) * 4u;
    _stream.seekp(4, std::ios::beg);
    write_u32(_stream, 36u + data_size);
    _stream.seekp(40, std::ios::beg);
    write_u32(_stream, data_size);
    _stream.flush();
    bool ok = bool(_stream);
    _stream.close();
    _closed = true;
    if (!ok && error_status)
    {
        *error_status = ErrorStatus(
            ErrorStatus::FILE_WRITE_FAILED,
            "failed finalizing WAVE file: " + _path);
    }
    return ok;
}

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
