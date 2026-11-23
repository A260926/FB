// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#include "opentimelineio/pcmWave.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

namespace {

void
set_error(
    PcmWaveStatus* status,
    PcmWaveError    error,
    std::string     message)
{
    if (status)
    {
        status->error   = error;
        status->message = std::move(message);
    }
}

uint16_t
le_u16(uint8_t const* p) noexcept
{
    return uint16_t(uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8));
}

uint32_t
le_u32(uint8_t const* p) noexcept
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16)
           | (uint32_t(p[3]) << 24);
}

bool
read_exact(
    std::ifstream& file,
    uint64_t       offset,
    void*          buffer,
    size_t         bytes)
{
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file)
    {
        return false;
    }
    file.read(static_cast<char*>(buffer), static_cast<std::streamsize>(bytes));
    return size_t(file.gcount()) == bytes;
}

constexpr uint32_t kFmtChunkId  = 0x20746d66; // "fmt "
constexpr uint32_t kDataChunkId = 0x61746164; // "data"
constexpr uint32_t kRiffId      = 0x46464952; // "RIFF"
constexpr uint32_t kWaveId      = 0x45564157; // "WAVE"

} // namespace

struct PcmWaveReader::Impl
{
    std::ifstream file;
    std::string   path;
    double        sample_rate = 0;
    int           channels    = 0;
    int64_t       frames      = 0;
    uint64_t      data_offset = 0;
    int64_t       cache_limit = 4096;

    std::vector<float> cache;
    int64_t            cache_start  = 0;
    int64_t            cache_frames = 0;

    bool closed = true;

    void drop_cache() noexcept
    {
        cache_start  = 0;
        cache_frames = 0;
    }

    bool open(std::string const& in_path, PcmWaveStatus* status)
    {
        close();

        file.open(in_path, std::ios::binary);
        if (!file)
        {
            set_error(
                status,
                PcmWaveError::FileOpenFailed,
                "cannot open WAV file '" + in_path + "'");
            return false;
        }

        file.clear();
        file.seekg(0, std::ios::end);
        if (!file)
        {
            set_error(
                status,
                PcmWaveError::FileOpenFailed,
                "cannot determine the size of WAV file '" + in_path + "'");
            return false;
        }
        auto const end = file.tellg();
        if (end < 0)
        {
            set_error(
                status,
                PcmWaveError::FileOpenFailed,
                "cannot determine the size of WAV file '" + in_path + "'");
            return false;
        }
        uint64_t const file_size = uint64_t(end);

        if (file_size < 12)
        {
            set_error(
                status,
                PcmWaveError::MalformedFile,
                "file is too short to be a RIFF/WAVE file");
            return false;
        }

        uint8_t header[12];
        if (!read_exact(file, 0, header, sizeof(header)))
        {
            set_error(
                status,
                PcmWaveError::ReadFailed,
                "cannot read the RIFF header");
            return false;
        }
        if (le_u32(header) != kRiffId || le_u32(header + 8) != kWaveId)
        {
            set_error(
                status,
                PcmWaveError::UnsupportedFormat,
                "file is not a little-endian RIFF/WAVE file");
            return false;
        }

        bool     have_fmt      = false;
        bool     have_data     = false;
        uint16_t audio_format  = 0;
        uint16_t channel_count = 0;
        uint16_t block_align   = 0;
        uint16_t bits          = 0;
        uint32_t rate          = 0;
        uint64_t data_bytes    = 0;
        uint64_t payload_start = 0;

        uint64_t pos = 12;
        while (pos + 8 <= file_size)
        {
            uint8_t chunk_header[8];
            if (!read_exact(file, pos, chunk_header, sizeof(chunk_header)))
            {
                set_error(
                    status,
                    PcmWaveError::ReadFailed,
                    "cannot read a RIFF chunk header");
                return false;
            }
            uint32_t const id   = le_u32(chunk_header);
            uint64_t const size = le_u32(chunk_header + 4);
            pos += 8;

            uint64_t const available = file_size - pos;

            if (id == kFmtChunkId)
            {
                if (size < 16 || available < 16)
                {
                    set_error(
                        status,
                        PcmWaveError::MalformedFile,
                        "fmt chunk is truncated");
                    return false;
                }
                uint8_t fmt[16];
                if (!read_exact(file, pos, fmt, sizeof(fmt)))
                {
                    set_error(
                        status,
                        PcmWaveError::ReadFailed,
                        "cannot read the fmt chunk");
                    return false;
                }
                audio_format  = le_u16(fmt);
                channel_count = le_u16(fmt + 2);
                rate          = le_u32(fmt + 4);
                block_align   = le_u16(fmt + 12);
                bits          = le_u16(fmt + 14);
                have_fmt      = true;
            }
            else if (id == kDataChunkId && !have_data)
            {
                if (size > available)
                {
                    set_error(
                        status,
                        PcmWaveError::MalformedFile,
                        "data chunk is truncated");
                    return false;
                }
                data_bytes    = size;
                payload_start = pos;
                have_data     = true;
            }

            // RIFF chunks are padded to an even number of bytes.
            uint64_t const padded = size + (size & 1u);
            if (padded > available)
            {
                break;
            }
            pos += padded;
        }

        if (!have_fmt)
        {
            set_error(
                status,
                PcmWaveError::MalformedFile,
                "WAV file has no fmt chunk");
            return false;
        }
        if (!have_data)
        {
            set_error(
                status,
                PcmWaveError::MalformedFile,
                "WAV file has no data chunk");
            return false;
        }
        if (audio_format != 1)
        {
            set_error(
                status,
                PcmWaveError::UnsupportedFormat,
                "only uncompressed PCM WAV data is supported");
            return false;
        }
        if (bits != 16)
        {
            set_error(
                status,
                PcmWaveError::UnsupportedFormat,
                "only 16-bit PCM WAV data is supported");
            return false;
        }
        if (channel_count != 1 && channel_count != 2)
        {
            set_error(
                status,
                PcmWaveError::UnsupportedFormat,
                "only mono or stereo WAV media is supported");
            return false;
        }
        if (rate == 0)
        {
            set_error(
                status,
                PcmWaveError::UnsupportedFormat,
                "WAV sample rate must be a positive integer");
            return false;
        }
        if (block_align != channel_count * 2)
        {
            set_error(
                status,
                PcmWaveError::UnsupportedFormat,
                "WAV block alignment is inconsistent with 16-bit PCM");
            return false;
        }

        this->path        = in_path;
        this->sample_rate = double(rate);
        this->channels    = int(channel_count);
        this->frames      = int64_t(data_bytes / block_align);
        this->data_offset = payload_start;
        this->cache.assign(size_t(cache_limit) * 2, 0.0f);
        this->drop_cache();
        this->closed = false;
        return true;
    }

    void close() noexcept
    {
        if (file.is_open())
        {
            file.close();
        }
        path.clear();
        sample_rate = 0;
        channels    = 0;
        frames      = 0;
        data_offset = 0;
        drop_cache();
        closed = true;
    }

    bool fill_cache(int64_t start, PcmWaveStatus* status)
    {
        if (start < 0 || start >= frames)
        {
            cache_start  = std::max<int64_t>(start, 0);
            cache_frames = 0;
            return true;
        }

        int64_t const count = std::min<int64_t>(cache_limit, frames - start);
        size_t const  samples = size_t(count) * size_t(channels);
        std::vector<uint8_t> bytes(samples * 2);

        uint64_t const offset =
            data_offset + uint64_t(start) * uint64_t(channels) * 2u;
        if (!read_exact(file, offset, bytes.data(), bytes.size()))
        {
            set_error(
                status,
                PcmWaveError::ReadFailed,
                "cannot read the PCM payload from the WAV file");
            return false;
        }

        cache.assign(size_t(count) * 2, 0.0f);
        if (channels == 1)
        {
            for (int64_t i = 0; i < count; ++i)
            {
                int16_t const v =
                    int16_t(le_u16(bytes.data() + size_t(i) * 2));
                float const f = float(v) / 32768.0f;
                cache[size_t(i) * 2]     = f;
                cache[size_t(i) * 2 + 1] = f;
            }
        }
        else
        {
            for (int64_t i = 0; i < count; ++i)
            {
                int16_t const l =
                    int16_t(le_u16(bytes.data() + size_t(i) * 4));
                int16_t const r =
                    int16_t(le_u16(bytes.data() + size_t(i) * 4 + 2));
                cache[size_t(i) * 2]     = float(l) / 32768.0f;
                cache[size_t(i) * 2 + 1] = float(r) / 32768.0f;
            }
        }

        cache_start  = start;
        cache_frames = count;
        return true;
    }

    bool frame(int64_t index, float& left, float& right, PcmWaveStatus* status)
    {
        if (index < 0 || index >= frames)
        {
            left = right = 0.0f;
            return true;
        }
        if (index < cache_start || index >= cache_start + cache_frames)
        {
            if (!fill_cache(index, status))
            {
                return false;
            }
            if (cache_frames == 0)
            {
                left = right = 0.0f;
                return true;
            }
        }
        size_t const i = size_t(index - cache_start) * 2;
        left           = cache[i];
        right          = cache[i + 1];
        return true;
    }
};

PcmWaveReader::PcmWaveReader()
    : _impl(new Impl)
{}

PcmWaveReader::~PcmWaveReader() = default;

PcmWaveReader::PcmWaveReader(PcmWaveReader&&) noexcept = default;

PcmWaveReader&
PcmWaveReader::operator=(PcmWaveReader&&) noexcept = default;

bool
PcmWaveReader::open(std::string const& path, PcmWaveStatus* status)
{
    if (status)
    {
        *status = PcmWaveStatus();
    }
    return _impl->open(path, status);
}

void
PcmWaveReader::close() noexcept
{
    _impl->close();
}

bool
PcmWaveReader::is_open() const noexcept
{
    return !_impl->closed;
}

std::string const&
PcmWaveReader::path() const noexcept
{
    return _impl->path;
}

double
PcmWaveReader::sample_rate() const noexcept
{
    return _impl->sample_rate;
}

int
PcmWaveReader::channel_count() const noexcept
{
    return _impl->channels;
}

int64_t
PcmWaveReader::frame_count() const noexcept
{
    return _impl->frames;
}

void
PcmWaveReader::set_cache_frame_count(int64_t frames) noexcept
{
    _impl->cache_limit = std::max<int64_t>(1, frames);
    _impl->drop_cache();
}

bool
PcmWaveReader::read_frames(
    int64_t        start_frame,
    int64_t        frame_count,
    float*         stereo_out,
    PcmWaveStatus* status)
{
    if (status)
    {
        *status = PcmWaveStatus();
    }
    if (_impl->closed)
    {
        set_error(
            status,
            PcmWaveError::InvalidArgument,
            "cannot read from a closed WAV reader");
        return false;
    }
    if (!stereo_out || start_frame < 0 || frame_count < 0)
    {
        set_error(
            status,
            PcmWaveError::InvalidArgument,
            "invalid frame range for a WAV read");
        return false;
    }

    int64_t remaining = frame_count;
    int64_t cursor    = start_frame;
    float*  out       = stereo_out;
    while (remaining > 0)
    {
        if (cursor >= _impl->frames)
        {
            std::fill(out, out + size_t(remaining) * 2, 0.0f);
            break;
        }
        if (cursor < _impl->cache_start
            || cursor >= _impl->cache_start + _impl->cache_frames)
        {
            if (!_impl->fill_cache(cursor, status))
            {
                return false;
            }
        }
        int64_t const available =
            _impl->cache_start + _impl->cache_frames - cursor;
        if (available <= 0)
        {
            std::fill(out, out + size_t(remaining) * 2, 0.0f);
            break;
        }
        int64_t const take = std::min(remaining, available);
        float const*  src =
            _impl->cache.data() + size_t(cursor - _impl->cache_start) * 2;
        std::copy(src, src + size_t(take) * 2, out);
        out += size_t(take) * 2;
        cursor += take;
        remaining -= take;
    }
    return true;
}

bool
PcmWaveReader::sample_at(
    double         frame_position,
    float&         left,
    float&         right,
    PcmWaveStatus* status)
{
    if (status)
    {
        *status = PcmWaveStatus();
    }
    if (_impl->closed)
    {
        set_error(
            status,
            PcmWaveError::InvalidArgument,
            "cannot sample a closed WAV reader");
        return false;
    }
    if (!std::isfinite(frame_position))
    {
        set_error(
            status,
            PcmWaveError::InvalidArgument,
            "media sample position is not finite");
        return false;
    }

    // Media frame k is located at k / sample_rate.  Positions between two
    // frames are linearly interpolated; a frame index outside the physical
    // media is treated as silence, so padding beyond the material is zero.
    double const span = double(_impl->frames);
    if (frame_position < 0.0 || frame_position >= span)
    {
        left = right = 0.0f;
        return true;
    }

    double const floor_pos = std::floor(frame_position);
    double const frac      = frame_position - floor_pos;
    int64_t const base     = int64_t(floor_pos);

    float l0 = 0.0f, r0 = 0.0f, l1 = 0.0f, r1 = 0.0f;
    if (!_impl->frame(base, l0, r0, status))
    {
        return false;
    }
    if (frac == 0.0)
    {
        left  = l0;
        right = r0;
        return true;
    }
    if (!_impl->frame(base + 1, l1, r1, status))
    {
        return false;
    }

    left  = float(double(l0) + (double(l1) - double(l0)) * frac);
    right = float(double(r0) + (double(r1) - double(r0)) * frac);
    return true;
}

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
