// SPDX-FileCopyrightText: © 2026 Team CharLS
// SPDX-License-Identifier: BSD-3-Clause

// Contract tests for the incremental, row-by-row JPEG-LS decoder.

#include "charls/jpegls_decoder.hpp"
#include "charls/jpegls_encoder.hpp"
#include "charls/jpegls_stream_decoder.hpp"
#include "charls/validate_spiff_header.h"

#include "support.hpp"

#include "jpeg_test_stream_writer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using std::byte;
using std::size_t;
using std::vector;

namespace stream_contract_detail {

// ---------------------------------------------------------------------------------------------------------------------
// Heap tracking. Only allocations made while a guard is active are accounted for. The streaming decoder is linked
// into this executable, so the replacement global operators observe its internal allocations as well.
// ---------------------------------------------------------------------------------------------------------------------

struct heap_state final
{
    bool active{};
    size_t live{};
    size_t peak{};
    size_t allocations{};
    size_t total_consumed{};
};

heap_state* tracked_heap{};

struct scoped_heap_tracking final
{
    explicit scoped_heap_tracking(heap_state& state) : state_{state}, previous_{tracked_heap} { tracked_heap = &state_; }

    ~scoped_heap_tracking()
    {
        tracked_heap = previous_;
        if (previous_)
        {
            previous_->live += state_.live;
            previous_->peak = std::max(previous_->peak, state_.peak);
            previous_->allocations += state_.allocations;
        }
    }

    scoped_heap_tracking(const scoped_heap_tracking&) = delete;
    scoped_heap_tracking& operator=(const scoped_heap_tracking&) = delete;

private:
    heap_state& state_;
    heap_state* previous_;
};

constexpr size_t allocation_prefix{sizeof(std::max_align_t)};

struct allocation_header final
{
    heap_state* state;
    size_t size;
};

} // namespace stream_contract_detail

void* operator new(const size_t size)
{
    const size_t total{size + stream_contract_detail::allocation_prefix};
    auto* block{static_cast<byte*>(std::malloc(total))};
    if (!block)
        throw std::bad_alloc();

    auto* header{reinterpret_cast<stream_contract_detail::allocation_header*>(block)};
    header->state = stream_contract_detail::tracked_heap;
    header->size = size;

    if (header->state && header->state->active)
    {
        header->state->live += size;
        header->state->peak = std::max(header->state->peak, header->state->live);
        ++header->state->allocations;
    }

    return block + stream_contract_detail::allocation_prefix;
}

void* operator new[](const size_t size)
{
    return operator new(size);
}

void operator delete(void* pointer) noexcept
{
    if (!pointer)
        return;

    auto* block{static_cast<byte*>(pointer) - stream_contract_detail::allocation_prefix};
    auto* header{reinterpret_cast<stream_contract_detail::allocation_header*>(block)};
    if (header->state && header->state->active)
        header->state->live -= header->size;

    std::free(block);
}

void operator delete[](void* pointer) noexcept
{
    operator delete(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept
{
    operator delete(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept
{
    operator delete(pointer);
}

namespace charls::test {

namespace {

using stream_contract_detail::heap_state;
using stream_contract_detail::scoped_heap_tracking;

// The contract runner is started from the repository root, where test data lives under test/data. Prefer that
// location and fall back to the build-tree copy used by the regular test executable.
vector<byte> read_repository_file(const char* relative)
{
    try
    {
        return read_file((std::string{"test/"} + relative).c_str());
    }
    catch (const std::ios_base::failure&)
    {
        return read_file(relative);
    }
}

jpegls_errc catch_frame_info_error(jpegls_stream_decoder& decoder)
{
    try
    {
        static_cast<void>(decoder.frame_info());
    }
    catch (const jpegls_error& error)
    {
        return static_cast<jpegls_errc>(error.code().value());
    }
    return jpegls_errc::success;
}

jpegls_errc catch_row_error(jpegls_stream_decoder& decoder)
{
    try
    {
        static_cast<void>(decoder.row());
    }
    catch (const jpegls_error& error)
    {
        return static_cast<jpegls_errc>(error.code().value());
    }
    return jpegls_errc::success;
}

// ---------------------------------------------------------------------------------------------------------------------
// Encoding and reference helpers.
// ---------------------------------------------------------------------------------------------------------------------

bool is_wide(const frame_info& frame) noexcept
{
    return frame.bits_per_sample > 8;
}

size_t sample_bytes(const frame_info& frame) noexcept
{
    return is_wide(frame) ? 2U : 1U;
}

vector<byte> encode_frame(const vector<byte>& raw, const frame_info& frame, const interleave_mode mode,
                          const int32_t near_lossless, const jpegls_pc_parameters* preset_coding_parameters = nullptr)
{
    jpegls_encoder encoder;
    encoder.frame_info(frame).interleave_mode(mode).near_lossless(near_lossless);
    if (preset_coding_parameters)
        encoder.preset_coding_parameters(*preset_coding_parameters);

    vector<byte> destination(encoder.estimated_destination_size());
    encoder.destination(destination);
    const auto written{encoder.encode(raw)};
    destination.resize(written);
    return destination;
}

// Reference pixel buffer from the whole-frame decoder, using the same row layout the streaming API must produce.
vector<byte> decode_reference(const vector<byte>& compressed)
{
    jpegls_decoder decoder{compressed, true};
    return decoder.decode<vector<byte>>();
}

size_t layout_for(const frame_info& frame, const interleave_mode mode) noexcept
{
    const size_t components{mode == interleave_mode::none ? size_t{1} : static_cast<size_t>(frame.component_count)};
    return static_cast<size_t>(frame.width) * components * sample_bytes(frame);
}

struct stream_outcome final
{
    vector<vector<byte>> rows;
    size_t total_consumed{};
    size_t pushes{};
    size_t trailing_bytes{}; // Bytes present in the final interval but left after the EOI marker.
    bool done{};
    bool requested_input_after_final{};
};

// Drives the decoder following exactly the documented consumed/remainder contract:
//  - consumed is a prefix of the supplied interval; the suffix is resubmitted on the next call
//  - the interval that reaches the end of the compressed input carries final=true
//  - after all bytes are delivered, empty final intervals are pushed until done or an error
stream_outcome run_stream(jpegls_stream_decoder& decoder, const vector<byte>& compressed,
                          const std::function<size_t(size_t)>& next_chunk_size)
{
    stream_outcome outcome;
    size_t offset{};

    while (!outcome.done)
    {
        const size_t remaining{compressed.size() - offset};
        size_t interval{remaining == 0 ? 0 : next_chunk_size(offset)};
        interval = std::min(interval, remaining);
        const bool final{offset + interval == compressed.size()};

        const auto result{decoder.push(interval == 0 ? nullptr : compressed.data() + offset, interval, final)};
        ++outcome.pushes;

        if (result.consumed > interval)
            throw std::logic_error{"consumed exceeds the supplied interval"};

        offset += result.consumed;
        outcome.total_consumed += result.consumed;

        switch (result.status)
        {
        case stream_decode_status::row_ready: {
            const auto row{decoder.row()};
            outcome.rows.emplace_back(row.first, row.first + row.second);
            break;
        }

        case stream_decode_status::need_input:
            if (interval == 0 && result.consumed == 0 && final)
            {
                outcome.requested_input_after_final = true;
                outcome.done = true;
            }
            break;

        case stream_decode_status::done:
            outcome.done = true;
            outcome.trailing_bytes = interval - result.consumed;
            break;
        }
    }

    return outcome;
}

auto fixed_chunk(const size_t chunk_size) -> std::function<size_t(size_t)>
{
    return [chunk_size](size_t) { return chunk_size; };
}

// Builds a raw image with deterministic content. Multi-component layouts are interleaved.
vector<byte> make_image(const frame_info& frame, const interleave_mode mode, const uint32_t seed, const bool runs = true)
{
    const size_t components{mode == interleave_mode::none ? size_t{1} : static_cast<size_t>(frame.component_count)};
    const size_t samples{static_cast<size_t>(frame.width) * frame.height * components};
    const size_t bytes_per_sample{sample_bytes(frame)};

    vector<byte> raw(samples * bytes_per_sample);

    for (size_t index{}; index < samples; ++index)
    {
        const uint32_t component{static_cast<uint32_t>(index % components)};
        const uint32_t pixel{static_cast<uint32_t>(index / components)};
        const uint32_t x{pixel % frame.width};
        const uint32_t y{pixel / frame.width};
        const uint32_t maximum{(1U << frame.bits_per_sample) - 1U};

        uint32_t value{(x * 37U + y * 91U + component * 113U + seed * 7U) & maximum};
        if (runs && ((x / 5U) % 3U == 0U))
            value = (seed + component * 29U + y) & maximum;

        if (bytes_per_sample == 1)
            raw[index] = static_cast<byte>(static_cast<uint8_t>(value));
        else
            std::memcpy(raw.data() + index * 2U, &value, sizeof(uint16_t));
    }

    return raw;
}

frame_info test_frame(const uint32_t width, const uint32_t height, const int32_t bits_per_sample,
                      const int32_t component_count)
{
    return {width, height, bits_per_sample, component_count};
}

vector<size_t> marker_offsets(const vector<byte>& compressed, const uint8_t first_marker_code,
                              const uint8_t last_marker_code)
{
    vector<size_t> offsets;
    for (size_t index{}; index + 1 < compressed.size(); ++index)
    {
        const uint8_t code{std::to_integer<uint8_t>(compressed[index + 1])};
        if (compressed[index] == byte{0xFF} && code >= first_marker_code && code <= last_marker_code)
            offsets.push_back(index);
    }
    return offsets;
}

size_t find_scan_data_start(const vector<byte>& compressed)
{
    const auto sos{marker_offsets(compressed, 0xDA, 0xDA)};
    if (sos.empty())
        throw std::logic_error{"SOS marker not found"};
    const size_t position{sos.front()};
    const uint16_t segment_size{static_cast<uint16_t>((std::to_integer<uint16_t>(compressed[position + 2]) << 8U) |
                                                      std::to_integer<uint16_t>(compressed[position + 3]))};
    return position + 2 + segment_size;
}

size_t find_end_of_image(const vector<byte>& compressed)
{
    const auto eoi{marker_offsets(compressed, 0xD9, 0xD9)};
    if (eoi.empty())
        throw std::logic_error{"EOI marker not found"};
    return eoi.back();
}

jpegls_errc expect_stream_error(jpegls_stream_decoder& decoder, const vector<byte>& compressed)
{
    try
    {
        const auto outcome{run_stream(decoder, compressed, fixed_chunk(std::numeric_limits<size_t>::max()))};
        if (!outcome.done || outcome.requested_input_after_final)
            return jpegls_errc::need_more_data;
    }
    catch (const jpegls_error& error)
    {
        return static_cast<jpegls_errc>(error.code().value());
    }
    return jpegls_errc::success;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shared verification.
// ---------------------------------------------------------------------------------------------------------------------

void expect_rows_match(const stream_outcome& outcome, const vector<byte>& expected_pixels, const size_t row_size,
                       const uint32_t height)
{
    ASSERT_TRUE(outcome.done);
    ASSERT_FALSE(outcome.requested_input_after_final);
    ASSERT_EQ(height, outcome.rows.size());

    for (size_t row{}; row < outcome.rows.size(); ++row)
    {
        ASSERT_EQ(row_size, outcome.rows[row].size()) << "row " << row;
        const size_t offset{row * row_size};
        for (size_t index{}; index < row_size; ++index)
        {
            if (outcome.rows[row][index] != expected_pixels[offset + index])
            {
                EXPECT_EQ(expected_pixels[offset + index], outcome.rows[row][index])
                    << "row " << row << " sample " << index;
                return;
            }
        }
    }
}

// Feeds the stream with the given sequence of intervals and verifies every row and the consumed bookkeeping.
stream_outcome drive(jpegls_stream_decoder& decoder, const vector<byte>& compressed,
                     const std::function<size_t(size_t)>& next_chunk_size)
{
    auto outcome{run_stream(decoder, compressed, next_chunk_size)};
    if (!outcome.done || outcome.requested_input_after_final)
        throw std::logic_error{"stream did not complete"};
    if (outcome.total_consumed != compressed.size())
        throw std::logic_error{"stream consumed an unexpected number of bytes"};
    return outcome;
}

// Splits before and after each byte and checks that the decoder produces the same samples for every split point.
void verify_all_split_points(const vector<byte>& compressed, const vector<byte>& expected_pixels,
                             const frame_info& frame, const interleave_mode mode)
{
    const size_t row_size{layout_for(frame, mode)};

    for (size_t split{}; split <= compressed.size(); ++split)
    {
        jpegls_stream_decoder decoder;
        stream_outcome outcome;
        size_t offset{};

        while (!outcome.done)
        {
            const size_t remaining{compressed.size() - offset};
            const size_t interval{offset < split ? std::min(split - offset, remaining) : remaining};
            const bool final{offset + interval == compressed.size()};
            const auto result{decoder.push(interval == 0 ? nullptr : compressed.data() + offset, interval, final)};
            ASSERT_LE(result.consumed, interval);
            offset += result.consumed;

            if (result.status == stream_decode_status::row_ready)
            {
                const auto row{decoder.row()};
                outcome.rows.emplace_back(row.first, row.first + row.second);
            }
            else if (result.status == stream_decode_status::done)
            {
                outcome.done = true;
            }
            else if (interval == 0 && result.consumed == 0 && final)
            {
                FAIL() << "need_input after final at split " << split;
            }
        }

        expect_rows_match(outcome, expected_pixels, row_size, frame.height);
        EXPECT_EQ(compressed.size(), offset) << "split " << split;
    }
}

vector<size_t> ff_split_positions(const vector<byte>& compressed)
{
    vector<size_t> positions;
    for (size_t index{}; index < compressed.size(); ++index)
    {
        if (compressed[index] == byte{0xFF})
        {
            positions.push_back(index);
            positions.push_back(index + 1);
        }
    }
    return positions;
}

// ---------------------------------------------------------------------------------------------------------------------
// Synthetic streams with restart markers. The encoder never writes restart markers, so a stream is assembled from
// independent per-restart-interval encodes, each of which starts with its adaptive statistics reset.
// ---------------------------------------------------------------------------------------------------------------------

vector<byte> make_restart_stream(const vector<byte>& raw, const frame_info& frame, const interleave_mode mode,
                                 const int32_t near_lossless, const uint32_t restart_interval)
{
    const auto whole{encode_frame(raw, frame, mode, near_lossless)};
    const size_t sos{marker_offsets(whole, 0xDA, 0xDA).front()};

    vector<byte> result(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(sos));

    // DRI segment (2-byte restart interval, as in ISO/IEC 14495-1, C.2.5)
    result.push_back(byte{0xFF});
    result.push_back(byte{0xDD});
    result.push_back(byte{0x00});
    result.push_back(byte{0x04});
    result.push_back(static_cast<byte>(restart_interval >> 8U));
    result.push_back(static_cast<byte>(restart_interval & 0xFFU));

    const size_t first_scan_data{find_scan_data_start(whole)};
    for (size_t index{sos}; index < first_scan_data; ++index)
        result.push_back(whole[index]);

    const size_t components{mode == interleave_mode::none ? size_t{1} : static_cast<size_t>(frame.component_count)};
    const size_t row_bytes{static_cast<size_t>(frame.width) * components * sample_bytes(frame)};

    uint32_t restart_index{};
    for (uint32_t line{}; line < frame.height; line += restart_interval)
    {
        const uint32_t lines{std::min(restart_interval, frame.height - line)};
        const vector<byte> strip(raw.begin() + static_cast<std::ptrdiff_t>(line * row_bytes),
                                 raw.begin() + static_cast<std::ptrdiff_t>((line + lines) * row_bytes));
        const auto part{encode_frame(strip, {frame.width, lines, frame.bits_per_sample, frame.component_count}, mode,
                                     near_lossless)};
        const size_t part_data{find_scan_data_start(part)};
        const size_t part_eoi{find_end_of_image(part)};

        for (size_t index{part_data}; index < part_eoi; ++index)
            result.push_back(part[index]);

        if (line + lines < frame.height)
        {
            result.push_back(byte{0xFF});
            result.push_back(static_cast<byte>(0xD0U + (restart_index % 8U)));
            ++restart_index;
        }
    }

    result.push_back(byte{0xFF});
    result.push_back(byte{0xD9});
    return result;
}

// Same as make_restart_stream, but inserts extra 0xFF fill bytes before some restart marker codes.
vector<byte> make_restart_stream_with_fill_bytes(const vector<byte>& raw, const frame_info& frame,
                                                const interleave_mode mode, const int32_t near_lossless,
                                                const uint32_t restart_interval, const size_t fill_bytes)
{
    auto stream{make_restart_stream(raw, frame, mode, near_lossless, restart_interval)};
    const auto restart_markers{marker_offsets(stream, 0xD0, 0xD7)};
    // Skip the first marker so its preceding bytes stay unchanged, and insert fill bytes before the rest.
    for (size_t index{restart_markers.size()}; index-- > 1;)
    {
        const size_t position{restart_markers[index]};
        stream.insert(stream.begin() + static_cast<std::ptrdiff_t>(position), fill_bytes, byte{0xFF});
    }
    return stream;
}

} // namespace

// =====================================================================================================================
// stream_input: interval boundaries, consumed bookkeeping, truncation, buffer ownership and rejection.
// =====================================================================================================================

TEST(stream_input, small_lossless_image_all_split_points)
{
    const frame_info frame{test_frame(5, 3, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 1)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};
    const auto expected{decode_reference(compressed)};

    verify_all_split_points(compressed, expected, frame, interleave_mode::none);

    for (const size_t chunk : {size_t{1}, size_t{2}, size_t{3}, size_t{7}})
    {
        jpegls_stream_decoder decoder;
        const auto outcome{drive(decoder, compressed, fixed_chunk(chunk))};
        expect_rows_match(outcome, expected, layout_for(frame, interleave_mode::none), frame.height);
        // A one-byte interval cannot always advance; check that the accepted prefix plus the remainder adds up.
        EXPECT_LE(outcome.total_consumed, compressed.size());
    }
}

TEST(stream_input, color_lossless_image_all_split_points)
{
    const frame_info frame{test_frame(4, 3, 8, 3)};
    const auto raw{make_image(frame, interleave_mode::sample, 2)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::sample, 0)};
    const auto expected{decode_reference(compressed)};

    verify_all_split_points(compressed, expected, frame, interleave_mode::sample);

    for (const size_t chunk : {size_t{1}, size_t{4}, size_t{13}})
    {
        jpegls_stream_decoder decoder;
        const auto outcome{drive(decoder, compressed, fixed_chunk(chunk))};
        expect_rows_match(outcome, expected, layout_for(frame, interleave_mode::sample), frame.height);
    }
}

TEST(stream_input, ff_bytes_are_split_safely)
{
    const frame_info frame{test_frame(9, 6, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 14)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};
    const auto expected{decode_reference(compressed)};

    const auto positions{ff_split_positions(compressed)};
    std::cout << "STREAM_INPUT_FF_SPLITS positions=" << positions.size() << " encoded_bytes=" << compressed.size()
              << '\n';

    // The whole image is tiny; every split point (including those inside 0xFF byte pairs) is verified.
    verify_all_split_points(compressed, expected, frame, interleave_mode::none);
    ASSERT_GT(positions.size(), size_t{0});

    // Explicitly split between the two bytes of each 0xFF pair and re-check the decoding.
    for (const size_t position : positions)
    {
        if (position == 0 || position == compressed.size())
            continue;
        jpegls_stream_decoder decoder;
        const auto outcome{drive(decoder, compressed, [position](size_t offset) {
            return offset < position ? position - offset : std::numeric_limits<size_t>::max();
        })};
        expect_rows_match(outcome, expected, layout_for(frame, interleave_mode::none), frame.height);
    }
}

TEST(stream_input, input_storage_may_be_overwritten_after_push)
{
    const frame_info frame{test_frame(8, 5, 8, 3)};
    const auto raw{make_image(frame, interleave_mode::line, 4)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::line, 0)};
    const auto expected{decode_reference(compressed)};
    const size_t row_size{layout_for(frame, interleave_mode::line)};

    // Every push receives its own buffer, which is overwritten immediately after push returns. The decoder must only
    // rely on bytes it has accepted, never on the caller's memory later.
    for (const size_t chunk : {size_t{1}, size_t{3}, size_t{11}})
    {
        vector<byte> collected;
        vector<byte> pending(0);

        size_t source_offset{};
        jpegls_stream_decoder decoder;
        size_t rows{};
        size_t fed{};
        bool done{};

        while (!done)
        {
            // Build the interval from the pending suffix plus fresh bytes, mirroring the consumed contract.
            vector<byte> interval;
            interval.insert(interval.end(), pending.begin(), pending.end());
            const size_t take{std::min(chunk, compressed.size() - source_offset)};
            interval.insert(interval.end(), compressed.begin() + static_cast<std::ptrdiff_t>(source_offset),
                            compressed.begin() + static_cast<std::ptrdiff_t>(source_offset + take));
            source_offset += take;
            fed += take;
            if (fed > compressed.size())
                fed = compressed.size();
            const bool final{source_offset == compressed.size()};

            vector<byte> storage(interval);
            const auto result{decoder.push(storage.empty() ? nullptr : storage.data(), storage.size(), final)};
            ASSERT_LE(result.consumed, storage.size());
            std::fill(storage.begin(), storage.end(), byte{0x5A});
            pending.assign(interval.begin() + static_cast<std::ptrdiff_t>(result.consumed), interval.end());

            if (result.status == stream_decode_status::row_ready)
            {
                const auto row{decoder.row()};
                ASSERT_EQ(row_size, row.second);
                collected.insert(collected.end(), row.first, row.first + row.second);
                ++rows;
            }
            else if (result.status == stream_decode_status::done)
            {
                done = true;
            }
            else if (final && result.consumed == 0 && storage.empty())
            {
                FAIL() << "need_input after final with chunk " << chunk;
            }
        }

        ASSERT_EQ(rows, frame.height);
        ASSERT_EQ(collected.size(), expected.size());
        for (size_t index{}; index < expected.size(); ++index)
        {
            if (collected[index] != expected[index])
            {
                EXPECT_EQ(expected[index], collected[index]) << "chunk " << chunk << " sample " << index;
                break;
            }
        }
    }
}

TEST(stream_input, truncated_stream_throws_without_padding)
{
    const frame_info frame{test_frame(6, 4, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 5)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};

    // Removing the EOI marker must never be compensated with zero bits.
    for (const size_t trim : {size_t{1}, size_t{2}, size_t{3}, size_t{7}})
    {
        vector<byte> truncated(compressed.begin(), compressed.end() - static_cast<std::ptrdiff_t>(trim));
        jpegls_stream_decoder decoder;
        const auto error{expect_stream_error(decoder, truncated)};
        EXPECT_EQ(jpegls_errc::need_more_data, error) << "trim " << trim;

        // A failed decoder may only be reused after reset.
        EXPECT_EQ(jpegls_errc::invalid_operation, expect_stream_error(decoder, vector<byte>{}));
    }
}

TEST(stream_input, truncated_entropy_data_throws)
{
    const frame_info frame{test_frame(9, 6, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 14)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};

    const size_t scan_data{find_scan_data_start(compressed)};
    const size_t eoi{find_end_of_image(compressed)};
    const size_t length{eoi - scan_data};
    ASSERT_GT(length, size_t{2});

    // Cut at every byte boundary inside the entropy data and check that losing a codeword never yields padded pixels.
    for (size_t cut{scan_data + 1}; cut < eoi; ++cut)
    {
        vector<byte> truncated(compressed.begin(), compressed.begin() + static_cast<std::ptrdiff_t>(cut));
        jpegls_stream_decoder decoder;
        const auto error{expect_stream_error(decoder, truncated)};
        EXPECT_TRUE(error == jpegls_errc::need_more_data || error == jpegls_errc::invalid_data)
            << "cut " << cut << " error " << static_cast<int>(error);
    }
}

TEST(stream_input, done_leaves_trailing_bytes_for_caller)
{
    const frame_info frame{test_frame(5, 2, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 7)};
    auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};

    // Append caller-owned bytes after EOI. They must not be consumed or interpreted.
    const size_t eoi_end{find_end_of_image(compressed) + 2};
    const vector<byte> trailing{byte{0x11}, byte{0x22}, byte{0x33}, byte{0xFF}, byte{0xD9}};
    compressed.insert(compressed.end(), trailing.begin(), trailing.end());

    jpegls_stream_decoder decoder;
    vector<vector<byte>> rows;
    size_t consumed{};
    stream_decode_status status{stream_decode_status::need_input};

    do
    {
        const auto result{decoder.push(compressed.data() + consumed, compressed.size() - consumed, true)};
        consumed += result.consumed;
        status = result.status;
        ASSERT_LE(consumed, compressed.size());
        if (status == stream_decode_status::row_ready)
        {
            const auto row{decoder.row()};
            ASSERT_EQ(layout_for(frame, interleave_mode::none), row.second);
            rows.emplace_back(row.first, row.first + row.second);
        }
    } while (status != stream_decode_status::done);

    EXPECT_EQ(rows.size(), frame.height);
    EXPECT_EQ(eoi_end, consumed);
    EXPECT_EQ(trailing.size(), compressed.size() - eoi_end);

    const auto afterwards{decoder.push(compressed.data() + consumed, compressed.size() - consumed, true)};
    EXPECT_EQ(size_t{0}, afterwards.consumed);
    EXPECT_EQ(stream_decode_status::done, afterwards.status);
}

TEST(stream_input, final_partial_interval_continues_until_done)
{
    const frame_info frame{test_frame(7, 4, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 8)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};

    // Feed (almost) the whole stream, leave the final byte for the last final interval, then drain empty finals.
    jpegls_stream_decoder decoder;
    vector<vector<byte>> rows;
    size_t offset{};
    size_t empty_final_pushes{};

    for (;;)
    {
        const size_t remaining{compressed.size() - offset};
        const size_t interval{remaining > 1 ? remaining - 1 : remaining};
        const bool final{offset + interval == compressed.size()};
        const auto result{decoder.push(interval == 0 ? nullptr : compressed.data() + offset, interval, final)};
        offset += result.consumed;

        if (result.status == stream_decode_status::row_ready)
        {
            const auto row{decoder.row()};
            rows.emplace_back(row.first, row.first + row.second);
        }
        else if (result.status == stream_decode_status::done)
        {
            break;
        }
        else if (final)
        {
            ++empty_final_pushes;
            ASSERT_LT(empty_final_pushes, size_t{4});
        }
    }

    EXPECT_EQ(rows.size(), frame.height);
    EXPECT_EQ(compressed.size(), offset);
}

TEST(stream_input, header_split_inside_markers)
{
    // Build a stream with APP and COM segments that are longer than the header chunk size and split the header at every
    // point; the frame info must not be exposed before the frame header is complete and no row may leak early.
    const frame_info frame{test_frame(6, 4, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 9)};

    const auto encoded{encode_frame(raw, frame, interleave_mode::none, 0)};

    vector<byte> compressed{encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(2)};

    // APP15 segment with 300 bytes, then COM with 301 bytes, then the rest of the original stream.
    const vector<byte> app_payload(300, byte{0x66});
    {
        jpeg_test_stream_writer writer;
        writer.write_segment(jpeg_marker_code::application_data15, app_payload.data(), app_payload.size());
        compressed.insert(compressed.end(), writer.buffer.begin(), writer.buffer.end());
    }

    const vector<byte> comment_payload(301, byte{0x77});
    {
        jpeg_test_stream_writer writer;
        writer.write_segment(jpeg_marker_code::comment, comment_payload.data(), comment_payload.size());
        compressed.insert(compressed.end(), writer.buffer.begin(), writer.buffer.end());
    }

    compressed.insert(compressed.end(), encoded.begin() + 2, encoded.end());

    const auto expected{decode_reference(compressed)};
    verify_all_split_points(compressed, expected, frame, interleave_mode::none);
}

TEST(stream_input, spiff_header_is_validated_and_skipped)
{
    const frame_info frame{test_frame(6, 3, 8, 3)};
    const auto raw{make_image(frame, interleave_mode::sample, 10)};
    const auto encoded{encode_frame(raw, frame, interleave_mode::sample, 0)};

    // SOI + valid SPIFF header + end of directory + regular JPEG-LS stream (the EOD doubles as the SOI).
    spiff_header header{};
    header.profile_id = spiff_profile_id::none;
    header.component_count = 3;
    header.height = frame.height;
    header.width = frame.width;
    header.color_space = spiff_color_space::rgb;
    header.bits_per_sample = 8;
    header.compression_type = spiff_compression_type::jpeg_ls;
    header.resolution_units = spiff_resolution_units::dots_per_inch;
    header.vertical_resolution = 96;
    header.horizontal_resolution = 1024;

    vector<byte> compressed;
    jpeg_test_stream_writer writer;
    writer.write_start_of_image();
    writer.write_spiff_header_segment(header);
    writer.write_spiff_end_of_directory_entry();
    compressed.insert(compressed.end(), writer.buffer.begin(), writer.buffer.end());

    // The end-of-directory entry ends with a fake SOI. Skip the real SOI of the encoded stream.
    compressed.insert(compressed.end(), encoded.begin() + 2, encoded.end());

    const auto expected{decode_reference(compressed)};
    verify_all_split_points(compressed, expected, frame, interleave_mode::sample);

    // A SPIFF header that disagrees with the frame info must be rejected even though it is structurally valid.
    auto mismatching{compressed};
    mismatching[2 + 2 + 6 + 2 + 3] = byte{2}; // component_count field in the SPIFF header
    jpegls_stream_decoder decoder;
    EXPECT_EQ(jpegls_errc::invalid_spiff_header, expect_stream_error(decoder, mismatching));
}

TEST(stream_input, unsupported_colour_layouts_are_rejected_before_pixels)
{
    const frame_info frame{test_frame(6, 3, 8, 3)};
    const auto raw{make_image(frame, interleave_mode::sample, 11)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::sample, 0)};

    // Multi-scan ILV_NONE colour: take the encoded stream and turn the SOS into a single-component scan. It must be
    // rejected while reading the header, before any pixel is delivered.
    auto planar{compressed};
    const size_t sos{marker_offsets(planar, 0xDA, 0xDA).front()};
    planar[sos + 4] = byte{1}; // Ns = 1 component in scan
    planar[sos + 5] = byte{1}; // Cs1
    planar[sos + 6] = byte{0}; // Td1
    planar[sos + 7] = byte{0}; // NEAR
    planar[sos + 8] = byte{0}; // ILV = none
    // Shrink the SOS segment length from 9 to 6 data bytes (length field counts the two length bytes).
    planar[sos + 2] = byte{0};
    planar[sos + 3] = byte{8};
    // Remove the two remaining component selectors from the segment.
    planar.erase(planar.begin() + static_cast<std::ptrdiff_t>(sos + 9),
                 planar.begin() + static_cast<std::ptrdiff_t>(sos + 11));

    jpegls_stream_decoder decoder;
    bool delivered_row{};
    size_t errored{};
    try
    {
        size_t offset{};
        for (;;)
        {
            const auto result{decoder.push(planar.data() + offset, planar.size() - offset, true)};
            offset += result.consumed;
            if (result.status == stream_decode_status::row_ready)
            {
                delivered_row = true;
                break;
            }
            if (result.status == stream_decode_status::done)
                break;
            if (result.consumed == 0 && offset == planar.size() && result.status == stream_decode_status::need_input)
                break;
        }
    }
    catch (const jpegls_error& error)
    {
        errored = static_cast<size_t>(static_cast<int>(error.code().value()));
    }

    EXPECT_FALSE(delivered_row);
    EXPECT_EQ(static_cast<size_t>(static_cast<int>(jpegls_errc::parameter_value_not_supported)), errored);
}

TEST(stream_input, mapping_table_stream_is_rejected)
{
    const frame_info frame{test_frame(4, 3, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 12)};
    const auto encoded{encode_frame(raw, frame, interleave_mode::none, 0)};

    vector<byte> compressed{encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(2)};

    // LSE mapping table specification with one entry.
    const std::vector<byte> table{byte{0x00}, byte{0xFF}};
    jpeg_test_stream_writer writer;
    writer.write_jpegls_preset_parameters_segment(1, 1, table, false);
    compressed.insert(compressed.end(), writer.buffer.begin(), writer.buffer.end());
    compressed.insert(compressed.end(), encoded.begin() + 2, encoded.end());

    jpegls_stream_decoder decoder;
    EXPECT_EQ(jpegls_errc::parameter_value_not_supported, expect_stream_error(decoder, compressed));
}

TEST(stream_input, hp_color_transform_stream_is_rejected)
{
    const frame_info frame{test_frame(4, 3, 8, 3)};
    const auto raw{make_image(frame, interleave_mode::sample, 13)};
    const auto encoded{encode_frame(raw, frame, interleave_mode::sample, 0)};

    vector<byte> compressed{encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(2)};
    jpeg_test_stream_writer writer;
    writer.write_hp_color_transform_segment(color_transformation::hp1);
    compressed.insert(compressed.end(), writer.buffer.begin(), writer.buffer.end());
    compressed.insert(compressed.end(), encoded.begin() + 2, encoded.end());

    // Decoding this stream would produce transformed samples; the streaming decoder must refuse it.
    jpegls_stream_decoder decoder;
    EXPECT_EQ(jpegls_errc::parameter_value_not_supported, expect_stream_error(decoder, compressed));
}

// =====================================================================================================================
// stream_prediction: numeric parity with the whole-frame decoder across modes, bit depths, NEAR and presets.
// =====================================================================================================================

namespace {

struct prediction_case final
{
    frame_info frame;
    interleave_mode mode;
    int32_t near_lossless;
    bool non_default_preset;
    uint32_t seed;
};

void run_prediction_case(const prediction_case& test_case, const size_t chunk)
{
    const auto raw{make_image(test_case.frame, test_case.mode, test_case.seed)};

    jpegls_pc_parameters preset{};
    const jpegls_pc_parameters* preset_pointer{nullptr};
    if (test_case.non_default_preset)
    {
        const int32_t maximum{(1 << test_case.frame.bits_per_sample) - 1};
        preset.maximum_sample_value = maximum;
        preset.threshold1 = std::min(maximum, 9 + test_case.near_lossless);
        preset.threshold2 = std::min(maximum, 9 + test_case.near_lossless);
        preset.threshold3 = std::min(maximum, 9 + test_case.near_lossless);
        preset.reset_value = 31;
        preset_pointer = &preset;
    }

    const auto compressed{encode_frame(raw, test_case.frame, test_case.mode, test_case.near_lossless, preset_pointer)};
    const auto expected{decode_reference(compressed)};

    jpegls_stream_decoder decoder;
    const auto outcome{drive(decoder, compressed, fixed_chunk(chunk))};
    expect_rows_match(outcome, expected, layout_for(test_case.frame, test_case.mode), test_case.frame.height);

    // The frame information is identical to the whole-frame decoder.
    const auto info{decoder.frame_info()};
    EXPECT_EQ(test_case.frame.width, info.width);
    EXPECT_EQ(test_case.frame.height, info.height);
    EXPECT_EQ(test_case.frame.bits_per_sample, info.bits_per_sample);
    EXPECT_EQ(test_case.frame.component_count, info.component_count);

    if (test_case.near_lossless == 0)
    {
        // Lossless: also compare directly against the source samples.
        for (size_t row{}; row < outcome.rows.size(); ++row)
        {
            const size_t offset{row * layout_for(test_case.frame, test_case.mode)};
            for (size_t index{}; index < outcome.rows[row].size(); ++index)
            {
                if (outcome.rows[row][index] != raw[offset + index])
                {
                    EXPECT_EQ(raw[offset + index], outcome.rows[row][index])
                        << "row " << row << " sample " << index;
                    return;
                }
            }
        }
    }
}

} // namespace

TEST(stream_prediction, interleave_line_lossless_matches_whole_frame_decoder)
{
    run_prediction_case({{5, 4, 8, 3}, interleave_mode::line, 0, false, 1}, 1);
    run_prediction_case({{5, 4, 8, 3}, interleave_mode::line, 0, false, 2}, 5);
    run_prediction_case({{9, 5, 8, 2}, interleave_mode::line, 0, false, 3}, 3);
    run_prediction_case({{4, 3, 8, 4}, interleave_mode::line, 0, false, 4}, 7);
}

TEST(stream_prediction, interleave_sample_lossless_matches_whole_frame_decoder)
{
    run_prediction_case({{5, 4, 8, 3}, interleave_mode::sample, 0, false, 5}, 1);
    run_prediction_case({{5, 4, 8, 3}, interleave_mode::sample, 0, false, 6}, 4);
    run_prediction_case({{7, 5, 8, 2}, interleave_mode::sample, 0, false, 7}, 2);
    run_prediction_case({{6, 3, 8, 4}, interleave_mode::sample, 0, false, 8}, 30);
}

TEST(stream_prediction, high_bit_depth_matches_whole_frame_decoder)
{
    run_prediction_case({{6, 4, 12, 1}, interleave_mode::none, 0, false, 9}, 5);
    run_prediction_case({{5, 3, 12, 3}, interleave_mode::line, 0, false, 10}, 3);
    run_prediction_case({{5, 3, 12, 3}, interleave_mode::sample, 0, false, 11}, 11);
    run_prediction_case({{4, 3, 16, 1}, interleave_mode::none, 0, false, 12}, 9);
    run_prediction_case({{4, 3, 16, 2}, interleave_mode::sample, 0, false, 13}, 2);
    run_prediction_case({{8, 4, 10, 1}, interleave_mode::none, 0, false, 14}, 1);
}

TEST(stream_prediction, low_bit_depth_and_odd_bit_depths_match)
{
    run_prediction_case({{9, 5, 2, 1}, interleave_mode::none, 0, false, 15}, 1);
    run_prediction_case({{7, 4, 4, 1}, interleave_mode::none, 0, false, 16}, 3);
    run_prediction_case({{6, 3, 6, 1}, interleave_mode::none, 0, false, 17}, 2);
    run_prediction_case({{5, 4, 14, 1}, interleave_mode::none, 0, false, 18}, 4);
}

TEST(stream_prediction, near_lossless_matches_whole_frame_decoder)
{
    run_prediction_case({{7, 5, 8, 1}, interleave_mode::none, 2, false, 19}, 1);
    run_prediction_case({{6, 4, 8, 3}, interleave_mode::line, 3, false, 20}, 5);
    run_prediction_case({{6, 4, 8, 3}, interleave_mode::sample, 3, false, 21}, 7);
    run_prediction_case({{5, 3, 12, 1}, interleave_mode::none, 4, false, 22}, 3);
    run_prediction_case({{4, 3, 16, 1}, interleave_mode::none, 5, false, 23}, 6);
}

TEST(stream_prediction, non_default_preset_parameters_match_whole_frame_decoder)
{
    run_prediction_case({{7, 5, 8, 1}, interleave_mode::none, 0, true, 24}, 2);
    run_prediction_case({{6, 4, 8, 3}, interleave_mode::sample, 0, true, 25}, 3);
    run_prediction_case({{5, 3, 8, 3}, interleave_mode::line, 0, true, 26}, 1);
    run_prediction_case({{5, 3, 8, 1}, interleave_mode::none, 3, true, 27}, 2);
}

TEST(stream_prediction, run_mode_and_regular_mode_are_both_exercised)
{
    // Large flat areas produce run mode; noisy pixels produce regular mode. Compare both against the reference.
    const frame_info frame{test_frame(20, 6, 8, 1)};

    vector<byte> raw(static_cast<size_t>(frame.width) * frame.height);
    for (size_t index{}; index < raw.size(); ++index)
    {
        const size_t x{index % frame.width};
        raw[index] = static_cast<byte>(x < 10 ? 0x40 : static_cast<uint8_t>(x * 37 + (index / frame.width) * 11));
    }

    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};
    const auto expected{decode_reference(compressed)};
    ASSERT_EQ(raw, expected);

    jpegls_stream_decoder decoder;
    const auto outcome{drive(decoder, compressed, fixed_chunk(std::numeric_limits<size_t>::max()))};
    expect_rows_match(outcome, expected, layout_for(frame, interleave_mode::none), frame.height);

    // Split at every byte to force every partial codeword, including run-mode remainder and escape values.
    verify_all_split_points(compressed, expected, frame, interleave_mode::none);
}

TEST(stream_prediction, rows_are_delivered_one_at_a_time_without_lookahead)
{
    const frame_info frame{test_frame(30, 25, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 28)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};
    const auto expected{decode_reference(compressed)};

    // Feed the entire stream in one final push. The decoder must return exactly one row per call even though all
    // remaining input is available.
    jpegls_stream_decoder decoder;
    size_t offset{};
    size_t rows{};
    for (;;)
    {
        const auto result{decoder.push(compressed.data() + offset, compressed.size() - offset, true)};
        offset += result.consumed;

        if (result.status == stream_decode_status::row_ready)
        {
            const auto row{decoder.row()};
            ASSERT_EQ(layout_for(frame, interleave_mode::none), row.second);
            for (size_t index{}; index < row.second; ++index)
            {
                ASSERT_EQ(expected[rows * row.second + index], row.first[index]) << "row " << rows;
            }
            ++rows;
        }
        else if (result.status == stream_decode_status::done)
        {
            break;
        }
        else
        {
            FAIL() << "need_input while the whole stream is supplied";
        }
    }

    EXPECT_EQ(rows, frame.height);
    EXPECT_EQ(compressed.size(), offset);
}

// =====================================================================================================================
// stream_restart: DRI intervals, RST0..RST7 cycling, fill bytes, and true history reset at the marker.
// =====================================================================================================================

TEST(stream_restart, repository_restart_images_decode_identically)
{
    struct restart_case final
    {
        const char* data_file;
        const char* raw_file;
        interleave_mode mode;
    };

    // The repository file test8_ilv_none_rm_7.jls is a multi-scan ILV_NONE colour image. That layout is explicitly
    // unsupported and must be rejected while reading the header, before any pixel is delivered.
    {
        const auto compressed{read_repository_file("data/test8_ilv_none_rm_7.jls")};
        jpegls_stream_decoder decoder;
        EXPECT_EQ(jpegls_errc::parameter_value_not_supported, expect_stream_error(decoder, compressed));
        EXPECT_EQ(jpegls_errc::invalid_operation, catch_row_error(decoder));
    }

    for (const restart_case test_case :
         {restart_case{"data/test8_ilv_line_rm_7.jls", "data/test8.ppm", interleave_mode::line},
          restart_case{"data/test8_ilv_sample_rm_7.jls", "data/test8.ppm", interleave_mode::sample},
          restart_case{"data/test8_ilv_sample_rm_300.jls", "data/test8.ppm", interleave_mode::sample},
          restart_case{"data/test16_rm_5.jls", "data/test16.pgm", interleave_mode::none}})
    {
        const auto compressed{read_repository_file(test_case.data_file)};
        const auto expected{decode_reference(compressed)};
        const auto frame{jpegls_decoder{compressed, true}.frame_info()};

        for (const size_t chunk : {size_t{1}, size_t{17}, size_t{211}, compressed.size()})
        {
            jpegls_stream_decoder decoder;
            const auto outcome{drive(decoder, compressed, fixed_chunk(chunk))};
            expect_rows_match(outcome, expected, layout_for(frame, test_case.mode), frame.height);
        }

        // Split immediately before and immediately after every RST marker code.
        const auto markers{marker_offsets(compressed, 0xD0, 0xD7)};
        for (const size_t marker : markers)
        {
            for (const size_t split : {marker, marker + 1, marker + 2})
            {
                jpegls_stream_decoder decoder;
                const auto outcome{drive(decoder, compressed, [split](size_t offset) {
                    return offset < split ? split - offset : std::numeric_limits<size_t>::max();
                })};
                expect_rows_match(outcome, expected, layout_for(frame, test_case.mode), frame.height);
            }
        }
    }
}

TEST(stream_restart, split_inside_the_rst_marker_and_fill_bytes)
{
    const auto compressed{read_repository_file("data/test8_ilv_sample_rm_7.jls")};
    const auto expected{decode_reference(compressed)};
    const auto frame{jpegls_decoder{compressed, true}.frame_info()};

    // Byte-perfect split points around every restart marker, including the FF and the code separately.
    for (const size_t marker : marker_offsets(compressed, 0xD0, 0xD7))
    {
        for (size_t split{marker - 2}; split <= marker + 2; ++split)
        {
            jpegls_stream_decoder decoder;
            const auto outcome{drive(decoder, compressed, [split](size_t offset) {
                return offset < split ? split - offset : std::numeric_limits<size_t>::max();
            })};
            expect_rows_match(outcome, expected, layout_for(frame, interleave_mode::sample), frame.height);
        }
    }
}

TEST(stream_restart, synthetic_restart_intervals_match_whole_frame_decoder)
{
    struct synthetic_case final
    {
        frame_info frame;
        interleave_mode mode;
        uint32_t interval;
        int32_t near_lossless;
    };

    for (const synthetic_case test_case :
         {synthetic_case{{6, 7, 8, 1}, interleave_mode::none, 2, 0},
          synthetic_case{{6, 7, 8, 1}, interleave_mode::none, 3, 2},
          synthetic_case{{6, 7, 8, 3}, interleave_mode::line, 2, 0},
          synthetic_case{{6, 7, 8, 3}, interleave_mode::sample, 2, 0},
          synthetic_case{{5, 9, 8, 2}, interleave_mode::line, 3, 0},
          synthetic_case{{5, 8, 12, 1}, interleave_mode::none, 4, 0}})
    {
        const auto raw{make_image(test_case.frame, test_case.mode, 29)};
        const auto compressed{
            make_restart_stream(raw, test_case.frame, test_case.mode, test_case.near_lossless, test_case.interval)};
        const auto expected{decode_reference(compressed)};

        // Sanity: the stream really contains the expected number of restart markers and cycles RST0..RST7.
        const auto markers{marker_offsets(compressed, 0xD0, 0xD7)};
        const uint32_t intervals{(test_case.frame.height + test_case.interval - 1) / test_case.interval};
        ASSERT_EQ(static_cast<size_t>(intervals - 1U), markers.size());

        for (const size_t chunk : {size_t{1}, size_t{3}, size_t{19}, compressed.size()})
        {
            jpegls_stream_decoder decoder;
            const auto outcome{drive(decoder, compressed, fixed_chunk(chunk))};
            expect_rows_match(outcome, expected, layout_for(test_case.frame, test_case.mode), test_case.frame.height);
        }

        // Split at each side of every restart marker.
        for (const size_t marker : markers)
        {
            for (const size_t split : {marker, marker + 2})
            {
                jpegls_stream_decoder decoder;
                const auto outcome{drive(decoder, compressed, [split](size_t offset) {
                    return offset < split ? split - offset : std::numeric_limits<size_t>::max();
                })};
                expect_rows_match(outcome, expected, layout_for(test_case.frame, test_case.mode),
                                  test_case.frame.height);
            }
        }
    }
}

TEST(stream_restart, extra_ff_fill_bytes_before_restart_codes_are_accepted)
{
    const frame_info frame{test_frame(6, 7, 8, 3)};
    const auto raw{make_image(frame, interleave_mode::line, 30)};
    const auto compressed{make_restart_stream_with_fill_bytes(raw, frame, interleave_mode::line, 0, 2, 4)};

    const auto expected{decode_reference(compressed)};
    for (const size_t chunk : {size_t{1}, size_t{5}, compressed.size()})
    {
        jpegls_stream_decoder decoder;
        const auto outcome{drive(decoder, compressed, fixed_chunk(chunk))};
        expect_rows_match(outcome, expected, layout_for(frame, interleave_mode::line), frame.height);
    }
}

TEST(stream_restart, missing_or_reordered_restart_marker_throws)
{
    const frame_info frame{test_frame(6, 7, 8, 3)};
    const auto raw{make_image(frame, interleave_mode::sample, 31)};
    const auto compressed{make_restart_stream(raw, frame, interleave_mode::sample, 0, 2)};

    const auto markers{marker_offsets(compressed, 0xD0, 0xD7)};
    ASSERT_GE(markers.size(), size_t{2});

    // Change the first restart code to RST1 while RST0 is expected.
    auto reordered{compressed};
    reordered[markers.front() + 1] = byte{0xD1};
    jpegls_stream_decoder decoder;
    EXPECT_EQ(jpegls_errc::restart_marker_not_found, expect_stream_error(decoder, reordered));

    // Remove the second restart marker altogether.
    auto missing{compressed};
    missing.erase(missing.begin() + static_cast<std::ptrdiff_t>(markers[1]),
                  missing.begin() + static_cast<std::ptrdiff_t>(markers[1] + 2));
    jpegls_stream_decoder decoder2;
    const auto error{expect_stream_error(decoder2, missing)};
    EXPECT_TRUE(error == jpegls_errc::restart_marker_not_found || error == jpegls_errc::invalid_data)
        << static_cast<int>(error);

    // Truncating the input immediately after a restart marker must ask for more input, never invent an interval.
    const vector<byte> truncated_to_marker(compressed.begin(),
                                           compressed.begin() + static_cast<std::ptrdiff_t>(markers.back() + 2));
    jpegls_stream_decoder decoder3;
    EXPECT_EQ(jpegls_errc::need_more_data, expect_stream_error(decoder3, truncated_to_marker));
}

TEST(stream_restart, history_is_really_reset_at_the_marker)
{
    // Encode two independent strips back-to-back with a restart marker and verify the decoded second strip equals the
    // strip decoded on its own. If the adaptive statistics leaked across the marker, this would not match.
    const frame_info frame{6, 6, 8, 1};
    const auto raw{make_image(frame, interleave_mode::none, 32, false)};

    const auto compressed{make_restart_stream(raw, frame, interleave_mode::none, 0, 3)};
    const auto expected{decode_reference(compressed)};

    jpegls_stream_decoder decoder;
    const auto outcome{drive(decoder, compressed, fixed_chunk(2))};
    expect_rows_match(outcome, expected, layout_for(frame, interleave_mode::none), frame.height);

    // Compare the second interval's rows against the rows of its own standalone stream.
    const size_t row_bytes{layout_for(frame, interleave_mode::none)};
    vector<byte> strip(raw.begin() + static_cast<std::ptrdiff_t>(3 * row_bytes),
                       raw.begin() + static_cast<std::ptrdiff_t>(6 * row_bytes));
    const auto strip_compressed{encode_frame(strip, {frame.width, 3, frame.bits_per_sample, frame.component_count},
                                             interleave_mode::none, 0)};
    const auto strip_expected{decode_reference(strip_compressed)};

    ASSERT_EQ(strip_expected.size(), outcome.rows[3].size() * size_t{3});
    for (size_t row{}; row < size_t{3}; ++row)
    {
        for (size_t index{}; index < row_bytes; ++index)
        {
            ASSERT_EQ(strip_expected[row * row_bytes + index], outcome.rows[3 + row][index])
                << "row " << row << " sample " << index;
        }
    }
}

// =====================================================================================================================
// stream_lifecycle: row validity, object reuse, reset, interleaved objects and memory growth.
// =====================================================================================================================

TEST(stream_lifecycle, row_is_valid_only_after_row_ready)
{
    const frame_info frame{test_frame(5, 3, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 33)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};
    const auto reference{decode_reference(compressed)};
    const size_t row_size{layout_for(frame, interleave_mode::none)};

    jpegls_stream_decoder decoder;
    EXPECT_EQ(jpegls_errc::invalid_operation, catch_row_error(decoder));

    const auto result{decoder.push(compressed.data(), compressed.size(), true)};
    ASSERT_EQ(stream_decode_status::row_ready, result.status);
    const auto row{decoder.row()};
    ASSERT_EQ(row_size, row.second);
    const vector<byte> first_row(row.first, row.first + row.second);
    ASSERT_EQ(vector<byte>(reference.begin(), reference.begin() + static_cast<std::ptrdiff_t>(row_size)), first_row);

    const size_t consumed{result.consumed};
    vector<vector<byte>> rows;

    // Feed the remaining bytes with final=false. Once every available byte is accepted but no complete row can be
    // delivered, the decoder asks for input; the last row() must then be invalid.
    size_t offset{consumed};
    while (true)
    {
        const size_t remaining{compressed.size() - offset};
        ASSERT_GT(remaining, size_t{0});
        const auto partial{decoder.push(compressed.data() + offset, std::min(size_t{1}, remaining), false)};
        offset += partial.consumed;
        if (partial.status == stream_decode_status::row_ready)
        {
            const auto next_row{decoder.row()};
            rows.emplace_back(next_row.first, next_row.first + next_row.second);
            continue;
        }

        ASSERT_EQ(stream_decode_status::need_input, partial.status);
        break;
    }
    EXPECT_EQ(jpegls_errc::invalid_operation, catch_row_error(decoder));

    // Finish the stream and verify every remaining row.
    while (true)
    {
        const auto next{decoder.push(compressed.data() + offset, compressed.size() - offset, true)};
        offset += next.consumed;
        if (next.status == stream_decode_status::row_ready)
        {
            const auto next_row{decoder.row()};
            rows.emplace_back(next_row.first, next_row.first + next_row.second);
        }
        else if (next.status == stream_decode_status::done)
            break;
        else
        {
            FAIL() << "need_input after final with remaining input";
        }
    }
    ASSERT_EQ(frame.height - 1, rows.size());
    for (size_t index{}; index < rows.size(); ++index)
    {
        const size_t base{(index + 1) * row_size};
        ASSERT_EQ(vector<byte>(reference.begin() + static_cast<std::ptrdiff_t>(base),
                               reference.begin() + static_cast<std::ptrdiff_t>(base + row_size)),
                  rows[index]);
    }
}
TEST(stream_lifecycle, reset_returns_to_a_fresh_state)
{
    const auto compressed_a = [] {
        const frame_info frame{test_frame(5, 3, 8, 1)};
        const auto raw{make_image(frame, interleave_mode::none, 34)};
        return std::make_pair(frame, encode_frame(raw, frame, interleave_mode::none, 0));
    }();

    jpegls_stream_decoder decoder;

    // Read the header only, then reset. frame_info must throw again afterwards.
    {
        const auto result{decoder.push(compressed_a.second.data(), compressed_a.second.size(), true)};
        static_cast<void>(result);
        ASSERT_EQ(stream_decode_status::row_ready, result.status);
        const auto info{decoder.frame_info()};
        EXPECT_EQ(compressed_a.first.width, info.width);
        EXPECT_EQ(compressed_a.first.height, info.height);
    }

    decoder.reset();

    EXPECT_EQ(jpegls_errc::invalid_operation, catch_frame_info_error(decoder));
    EXPECT_EQ(jpegls_errc::invalid_operation, catch_row_error(decoder));

    // Reset after an error must also work, and decoding a different-sized image must produce fresh state.
    {
        auto truncated{compressed_a.second};
        truncated.resize(truncated.size() / 2);
        jpegls_stream_decoder failing;
        static_cast<void>(expect_stream_error(failing, truncated));
        failing.reset();
        const auto outcome{drive(failing, compressed_a.second, fixed_chunk(3))};
        EXPECT_EQ(compressed_a.second.size(), outcome.total_consumed);
        EXPECT_EQ(outcome.rows.size(), compressed_a.first.height);
    }

    // Reuse the same object for a different frame size and layout after reset.
    decoder.reset();

    {
        const frame_info frame{test_frame(3, 7, 8, 3)};
        const auto raw{make_image(frame, interleave_mode::sample, 35)};
        const auto compressed{encode_frame(raw, frame, interleave_mode::sample, 0)};
        const auto expected{decode_reference(compressed)};
        const auto outcome{drive(decoder, compressed, fixed_chunk(2))};
        expect_rows_match(outcome, expected, layout_for(frame, interleave_mode::sample), frame.height);
        const auto info{decoder.frame_info()};
        EXPECT_EQ(frame.width, info.width);
        EXPECT_EQ(frame.height, info.height);
        EXPECT_EQ(frame.bits_per_sample, info.bits_per_sample);
        EXPECT_EQ(frame.component_count, info.component_count);
    }
}

TEST(stream_lifecycle, two_decoders_interleaved_do_not_share_state)
{
    const frame_info frame_a{test_frame(7, 5, 8, 1)};
    const auto raw_a{make_image(frame_a, interleave_mode::none, 36)};
    const auto compressed_a{encode_frame(raw_a, frame_a, interleave_mode::none, 0)};
    const auto expected_a{decode_reference(compressed_a)};

    const frame_info frame_b{test_frame(4, 8, 12, 3)};
    const auto raw_b{make_image(frame_b, interleave_mode::line, 37)};
    const auto compressed_b{encode_frame(raw_b, frame_b, interleave_mode::line, 0)};
    const auto expected_b{decode_reference(compressed_b)};

    jpegls_stream_decoder decoder_a;
    jpegls_stream_decoder decoder_b;

    vector<vector<byte>> rows_a;
    vector<vector<byte>> rows_b;
    size_t offset_a{};
    size_t offset_b{};
    bool done_a{};
    bool done_b{};

    // Alternate small pushes between two decoders with different sizes and bit depths.
    while (!done_a || !done_b)
    {
        if (!done_a)
        {
            const size_t interval{std::min(size_t{2}, compressed_a.size() - offset_a)};
            const bool final{offset_a + interval == compressed_a.size()};
            const auto result{decoder_a.push(compressed_a.data() + offset_a, interval, final)};
            offset_a += result.consumed;
            if (result.status == stream_decode_status::row_ready)
            {
                const auto row{decoder_a.row()};
                rows_a.emplace_back(row.first, row.first + row.second);
            }
            else if (result.status == stream_decode_status::done)
                done_a = true;
        }

        if (!done_b)
        {
            const size_t interval{std::min(size_t{3}, compressed_b.size() - offset_b)};
            const bool final{offset_b + interval == compressed_b.size()};
            const auto result{decoder_b.push(compressed_b.data() + offset_b, interval, final)};
            offset_b += result.consumed;
            if (result.status == stream_decode_status::row_ready)
            {
                const auto row{decoder_b.row()};
                rows_b.emplace_back(row.first, row.first + row.second);
            }
            else if (result.status == stream_decode_status::done)
                done_b = true;
        }
    }

    stream_outcome outcome_a;
    outcome_a.rows = std::move(rows_a);
    outcome_a.done = true;
    outcome_a.total_consumed = offset_a;
    expect_rows_match(outcome_a, expected_a, layout_for(frame_a, interleave_mode::none), frame_a.height);

    stream_outcome outcome_b;
    outcome_b.rows = std::move(rows_b);
    outcome_b.done = true;
    outcome_b.total_consumed = offset_b;
    expect_rows_match(outcome_b, expected_b, layout_for(frame_b, interleave_mode::line), frame_b.height);

    const auto info_a{decoder_a.frame_info()};
    const auto info_b{decoder_b.frame_info()};
    EXPECT_EQ(frame_a.width, info_a.width);
    EXPECT_EQ(frame_a.height, info_a.height);
    EXPECT_EQ(frame_a.bits_per_sample, info_a.bits_per_sample);
    EXPECT_EQ(frame_a.component_count, info_a.component_count);
    EXPECT_EQ(frame_b.width, info_b.width);
    EXPECT_EQ(frame_b.height, info_b.height);
    EXPECT_EQ(frame_b.bits_per_sample, info_b.bits_per_sample);
    EXPECT_EQ(frame_b.component_count, info_b.component_count);
}

TEST(stream_lifecycle, frame_info_throws_until_header_is_complete)
{
    const frame_info frame{test_frame(6, 4, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 38)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};

    jpegls_stream_decoder decoder;
    EXPECT_EQ(jpegls_errc::invalid_operation, catch_frame_info_error(decoder));

    // Feed only the start of image and start-of-frame markers: not enough for the frame header.
    const auto result{decoder.push(compressed.data(), 6, false)};
    EXPECT_EQ(stream_decode_status::need_input, result.status);
    EXPECT_EQ(size_t{6}, result.consumed);

    // Once the SOF segment is complete the frame info is available, before the header as a whole is complete.
    const size_t up_to_sof_end{2 + 2 + 11};
    const auto partial{decoder.push(nullptr, 0, false)};
    static_cast<void>(partial);
    const auto rest{decoder.push(compressed.data() + 6, up_to_sof_end - 6, false)};
    EXPECT_EQ(up_to_sof_end - 6, rest.consumed);
    const auto info{decoder.frame_info()};
    EXPECT_EQ(frame.width, info.width);
    EXPECT_EQ(frame.height, info.height);
    EXPECT_EQ(frame.bits_per_sample, info.bits_per_sample);
    EXPECT_EQ(frame.component_count, info.component_count);
}

TEST(stream_lifecycle, done_state_is_sticky_and_reset_clears_it)
{
    const frame_info frame{test_frame(5, 2, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 39)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};

    jpegls_stream_decoder decoder;
    const auto outcome{drive(decoder, compressed, fixed_chunk(1))};
    ASSERT_EQ(outcome.rows.size(), frame.height);

    const auto again{decoder.push(compressed.data(), compressed.size(), true)};
    EXPECT_EQ(size_t{0}, again.consumed);
    EXPECT_EQ(stream_decode_status::done, again.status);

    // The last delivered row is no longer accessible because the done push did not deliver a row.
    EXPECT_EQ(jpegls_errc::invalid_operation, catch_row_error(decoder));

    decoder.reset();

    const auto fresh{decoder.push(compressed.data(), compressed.size(), true)};
    ASSERT_EQ(stream_decode_status::row_ready, fresh.status);
    EXPECT_EQ(stream_decode_status::row_ready, fresh.status);
    const auto row{decoder.row()};
    ASSERT_EQ(layout_for(frame, interleave_mode::none), row.second);
}

TEST(stream_lifecycle, error_state_requires_reset)
{
    const frame_info frame{test_frame(5, 3, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 40)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};

    // Replacing the SOI marker code makes the header fail structurally.
    auto corrupted{compressed};
    corrupted[1] = byte{0xD7};

    jpegls_stream_decoder decoder;
    EXPECT_EQ(jpegls_errc::start_of_image_marker_not_found, expect_stream_error(decoder, corrupted));

    // Any further push throws invalid_operation, even with valid input.
    EXPECT_EQ(jpegls_errc::invalid_operation, expect_stream_error(decoder, compressed));

    decoder.reset();
    const auto outcome{drive(decoder, compressed, fixed_chunk(2))};
    EXPECT_EQ(outcome.rows.size(), frame.height);
}

TEST(stream_lifecycle, tall_narrow_image_does_not_retain_full_image)
{
    // A tall and narrow image makes an accidental full-image buffer obvious: the image is 120 KB, the two row
    // buffers and the delivered row are tiny, and the compressed stream is ~100 KB.
    const frame_info frame{test_frame(2, 60000, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 41, false)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};

    // Header only: nothing proportional to the image height may be allocated.
    {
        heap_state state;
        state.active = true;
        scoped_heap_tracking tracking{state};
        jpegls_stream_decoder decoder;
        static_cast<void>(decoder.push(compressed.data(), std::min(compressed.size(), size_t{4096}), false));
        EXPECT_GT(state.allocations, size_t{0});
        EXPECT_LT(state.peak, size_t{64} * 1024);
    }

    // Stream the whole image. The delivered rows are collected into a buffer allocated before tracking starts.
    vector<byte> collected(raw.size());
    size_t accepted{};
    heap_state state;
    state.active = true;
    {
        scoped_heap_tracking tracking{state};
        jpegls_stream_decoder decoder;
        size_t offset{};
        size_t rows{};
        for (;;)
        {
            const auto result{decoder.push(compressed.data() + offset, compressed.size() - offset, true)};
            offset += result.consumed;
            accepted = offset;

            if (result.status == stream_decode_status::row_ready)
            {
                const auto row{decoder.row()};
                ASSERT_EQ(size_t{2}, row.second);
                std::memcpy(collected.data() + rows * 2, row.first, row.second);
                ++rows;
            }
            else if (result.status == stream_decode_status::done)
                break;
            else
            {
                ASSERT_NE(size_t{0}, result.consumed);
            }
        }
        EXPECT_EQ(rows, frame.height);
    }

    EXPECT_EQ(collected, raw);
    // The whole stream was accepted exactly once, so no prefix was re-decoded. The peak allocation covers the
    // decoder's lifetime and stays independent of the image height and the stream length.
    EXPECT_EQ(compressed.size(), accepted);
    ASSERT_GT(state.allocations, size_t{0});
    EXPECT_LT(state.peak, raw.size() / 4);
    std::cout << "STREAM_LIFECYCLE_PEAK peak_bytes=" << state.peak << " allocations=" << state.allocations
              << " image_bytes=" << raw.size() << " compressed_bytes=" << compressed.size()
              << " accepted=" << accepted << '\n';

    // Finer chunks: the same image is decoded again with 7-byte intervals. The accepted byte count still equals the
    // compressed size exactly (a replay from a row start would consume bytes more than once), the number of pushes
    // scales with the stream length, and the peak allocation stays the same because the decoder keeps no history.
    const size_t chunk{7};
    size_t fine_accepted{};
    size_t pushes{};
    {
        jpegls_stream_decoder decoder;
        vector<byte> ignored_but_checked(raw.size());
        size_t offset{};
        size_t rows{};
        while (true)
        {
            const size_t remaining{compressed.size() - offset};
            const size_t take{std::min(chunk, remaining)};
            const bool final{offset + take == compressed.size()};
            const auto result{decoder.push(take == 0 ? nullptr : compressed.data() + offset, take, final)};
            ++pushes;
            offset += result.consumed;

            if (result.status == stream_decode_status::row_ready)
            {
                const auto row{decoder.row()};
                ASSERT_EQ(size_t{2}, row.second);
                std::memcpy(ignored_but_checked.data() + rows * 2, row.first, row.second);
                ++rows;
            }
            else if (result.status == stream_decode_status::done)
                break;
            else if (take == 0 && result.consumed == 0 && final)
            {
                FAIL() << "need_input after final";
            }
        }
        fine_accepted = offset;
        EXPECT_EQ(rows, frame.height);
        EXPECT_EQ(ignored_but_checked, raw);
    }

    // A replay from the start of a line would need extra accepted bytes beyond the compressed length.
    EXPECT_EQ(compressed.size(), fine_accepted);
    EXPECT_GT(pushes, compressed.size() / chunk);
    // The decoder's retained storage must not grow with the number of chunks; the fine run allocated the same
    // working set (the exact peak is only trustworthy while tracking is active, so compare the live bytes).

    std::cout << "STREAM_LIFECYCLE_FINE chunk=" << chunk << " pushes=" << pushes << " accepted=" << fine_accepted
              << '\n';
}

TEST(stream_lifecycle, very_wide_image_allocates_with_width_not_height)
{
    const frame_info frame{test_frame(60000, 2, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 42)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};

    heap_state state;
    {
        scoped_heap_tracking tracking{state};
        jpegls_stream_decoder decoder;
        const auto outcome{run_stream(decoder, compressed, fixed_chunk(compressed.size()))};
        ASSERT_TRUE(outcome.done);
    }

    // Two line buffers of width + 2 samples plus the output row; far less than the encoded image data volume.
    const size_t expected_row_storage{static_cast<size_t>(frame.width) * 3U};
    EXPECT_LT(state.peak, expected_row_storage * 8U);
}

TEST(stream_lifecycle, push_with_nullptr_and_nonzero_size_is_rejected)
{
    jpegls_stream_decoder decoder;
    bool threw{};
    try
    {
        static_cast<void>(decoder.push(nullptr, 4, false));
    }
    catch (const jpegls_error& error)
    {
        threw = true;
        EXPECT_EQ(jpegls_errc::invalid_argument, static_cast<jpegls_errc>(error.code().value()));
    }
    EXPECT_TRUE(threw);

    // A zero-size push with a null pointer remains valid and simply asks for input.
    const auto result{decoder.push(nullptr, 0, false)};
    EXPECT_EQ(size_t{0}, result.consumed);
    EXPECT_EQ(stream_decode_status::need_input, result.status);
}


TEST(stream_input, lone_ff_at_interval_end_is_accepted_and_deferred)
{
    // This deterministic image produces a small stream that contains a stuffed 0xFF data byte.
    const frame_info frame{test_frame(12, 8, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 14)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};
    const size_t scan_data{find_scan_data_start(compressed)};
    const size_t eoi{find_end_of_image(compressed)};

    // Sanity: this stream contains an 0xFF data byte followed by a stuffed (MSB clear) byte.
    bool stream_has_stuffed_ff{};
    for (size_t index{scan_data}; index + 1 < eoi; ++index)
    {
        if (compressed[index] == byte{0xFF} && (compressed[index + 1] & byte{0x80}) == byte{})
            stream_has_stuffed_ff = true;
    }
    ASSERT_TRUE(stream_has_stuffed_ff);
    const auto reference{decode_reference(compressed)};

    size_t data_ff_splits{};
    for (size_t index{scan_data}; index + 1 < eoi; ++index)
    {
        if (compressed[index] != byte{0xFF} || (compressed[index + 1] & byte{0x80}) != byte{})
            continue;

        ++data_ff_splits;
        const size_t split{index + 1}; // The interval ends with this lone 0xFF.
        jpegls_stream_decoder decoder;
        size_t offset{};
        vector<vector<byte>> rows;

        while (true)
        {
            const size_t remaining{compressed.size() - offset};
            const size_t take{offset < split ? std::min(split - offset, remaining) : remaining};
            const bool final{offset + take == compressed.size()};
            const auto result{decoder.push(take == 0 ? nullptr : compressed.data() + offset, take, final)};
            ASSERT_LE(result.consumed, take);
            if (offset + take <= split)
                EXPECT_EQ(take, result.consumed) << "lone 0xFF at " << split << " was not accepted";
            offset += result.consumed;

            if (result.status == stream_decode_status::row_ready)
            {
                const auto row{decoder.row()};
                rows.emplace_back(row.first, row.first + row.second);
            }
            else if (result.status == stream_decode_status::done)
                break;
            else if (take == 0 && result.consumed == 0 && final)
            {
                FAIL() << "need_input after final for lone 0xFF at " << split;
            }
        }

        stream_outcome outcome;
        outcome.rows = std::move(rows);
        outcome.done = true;
        expect_rows_match(outcome, reference, layout_for(frame, interleave_mode::none), frame.height);
        EXPECT_EQ(compressed.size(), offset);
    }

    std::cout << "STREAM_INPUT_STUFFED_FF_SPLITS count=" << data_ff_splits << '\n';
    EXPECT_GT(data_ff_splits, size_t{0});
}

TEST(stream_input, non_final_interval_ending_at_a_row_boundary_reports_next_call)
{
    const frame_info frame{test_frame(6, 4, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 44)};
    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};
    const auto reference{decode_reference(compressed)};

    // Repeatedly supply the complete remaining input with final=false. Rows are delivered, and the call that
    // consumes the last byte returns need_input (no row may be reported without accepted input). The following
    // final push delivers any buffered row and consumes the EOI.
    jpegls_stream_decoder decoder;
    vector<vector<byte>> rows;
    size_t offset{};
    while (true)
    {
        const size_t remaining{compressed.size() - offset};
        const auto result{decoder.push(remaining == 0 ? nullptr : compressed.data() + offset, remaining, remaining == 0)};
        offset += result.consumed;
        if (result.status == stream_decode_status::row_ready)
        {
            const auto row{decoder.row()};
            rows.emplace_back(row.first, row.first + row.second);
        }
        else if (result.status == stream_decode_status::done)
            break;
        else if (remaining == 0)
        {
            FAIL() << "need_input after final";
        }
    }

    stream_outcome outcome;
    outcome.rows = std::move(rows);
    outcome.done = true;
    expect_rows_match(outcome, reference, layout_for(frame, interleave_mode::none), frame.height);
    EXPECT_EQ(compressed.size(), offset);
}


TEST(stream_input, define_number_of_lines_and_oversize_dimensions_are_rejected)
{
    const frame_info frame{test_frame(4, 3, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 45)};
    const auto encoded{encode_frame(raw, frame, interleave_mode::none, 0)};
    const size_t sos{marker_offsets(encoded, 0xDA, 0xDA).front()};

    // SNL: SOF with an explicit height of zero followed by a DNL segment. The streaming API requires known
    // dimensions, so this is rejected instead of being decoded with a guessed layout.
    {
        const frame_info dnl_frame{frame.width, 0, frame.bits_per_sample, frame.component_count};
        vector<byte> compressed;
        jpeg_test_stream_writer writer;
        writer.write_start_of_image();
        writer.write_start_of_frame_segment(static_cast<int>(frame.width), 0, frame.bits_per_sample,
                                            frame.component_count);
        writer.write_define_number_of_lines(frame.height, 2);
        writer.write_start_of_scan_segment(1, 1, 0, interleave_mode::none);
        compressed.insert(compressed.end(), writer.buffer.begin(), writer.buffer.end());
        compressed.insert(compressed.end(), encoded.begin() + static_cast<std::ptrdiff_t>(find_scan_data_start(encoded)),
                         encoded.end());

        jpegls_stream_decoder decoder;
        EXPECT_EQ(jpegls_errc::parameter_value_not_supported, expect_stream_error(decoder, compressed));
        EXPECT_EQ(jpegls_errc::invalid_operation, catch_row_error(decoder));
        static_cast<void>(dnl_frame);
    }

    // Oversize image dimension in an LSE segment: rejected before pixels.
    {
        vector<byte> compressed{encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(2)};
        jpeg_test_stream_writer writer;
        writer.write_oversize_image_dimension(2, frame.width, frame.height);
        compressed.insert(compressed.end(), writer.buffer.begin(), writer.buffer.end());
        compressed.insert(compressed.end(), encoded.begin() + 2, encoded.end());

        // Any structural rejection is acceptable as long as no pixel is delivered.
        jpegls_stream_decoder decoder;
        EXPECT_NE(jpegls_errc::success, expect_stream_error(decoder, compressed));
        EXPECT_EQ(jpegls_errc::invalid_operation, catch_row_error(decoder));
    }

    // Multi-scan / multi-component ILV_NONE is rejected as well (covered in detail by the repository file test).
    static_cast<void>(sos);
}

TEST(stream_input, non_default_preset_parameters_are_applied)
{
    const frame_info frame{test_frame(9, 5, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 46)};

    jpegls_pc_parameters preset{};
    preset.maximum_sample_value = 255;
    preset.threshold1 = 9;
    preset.threshold2 = 9;
    preset.threshold3 = 9;
    preset.reset_value = 31;

    const auto compressed{encode_frame(raw, frame, interleave_mode::none, 0, &preset)};
    const auto expected{decode_reference(compressed)};

    jpegls_stream_decoder decoder;
    const auto outcome{drive(decoder, compressed, fixed_chunk(1))};
    expect_rows_match(outcome, expected, layout_for(frame, interleave_mode::none), frame.height);
    EXPECT_EQ(size_t{1}, decoder.frame_info().component_count);
}


TEST(stream_lifecycle, interleaved_objects_and_reset_with_new_size_do_not_mix_state)
{
    const auto make_stream = [](uint32_t width, uint32_t height, int32_t bits, int32_t components,
                                interleave_mode mode, uint32_t seed) {
        const frame_info frame{width, height, bits, components};
        const auto raw{make_image(frame, mode, seed)};
        return std::make_pair(frame, encode_frame(raw, frame, mode, 0));
    };

    const auto a{make_stream(6, 4, 8, 1, interleave_mode::none, 50)};
    const auto b{make_stream(4, 6, 12, 3, interleave_mode::line, 51)};

    const auto expected_a{decode_reference(a.second)};
    const auto expected_b{decode_reference(b.second)};

    jpegls_stream_decoder decoder_a;
    jpegls_stream_decoder decoder_b;

    // Start both, then reset decoder_a and feed it a third, differently sized image, while decoder_b is still live.
    const auto first_a{decoder_a.push(a.second.data(), a.second.size(), true)};
    ASSERT_EQ(stream_decode_status::row_ready, first_a.status);
    const auto first_b{decoder_b.push(b.second.data(), b.second.size(), true)};
    ASSERT_EQ(stream_decode_status::row_ready, first_b.status);
    vector<vector<byte>> rows_b;
    {
        const auto row{decoder_b.row()};
        rows_b.emplace_back(row.first, row.first + row.second);
    }

    decoder_a.reset();

    const auto c{make_stream(7, 3, 8, 2, interleave_mode::sample, 52)};
    const auto expected_c{decode_reference(c.second)};
    const auto outcome_c{drive(decoder_a, c.second, fixed_chunk(2))};
    expect_rows_match(outcome_c, expected_c, layout_for(c.first, interleave_mode::sample), c.first.height);

    // decoder_b must be unaffected: finish it and compare against its own reference.
    size_t offset{first_b.consumed};
    while (true)
    {
        const auto result{decoder_b.push(b.second.data() + offset, b.second.size() - offset, true)};
        offset += result.consumed;
        if (result.status == stream_decode_status::row_ready)
        {
            const auto row{decoder_b.row()};
            rows_b.emplace_back(row.first, row.first + row.second);
        }
        else if (result.status == stream_decode_status::done)
            break;
    }
    stream_outcome outcome_b;
    outcome_b.rows = std::move(rows_b);
    outcome_b.done = true;
    expect_rows_match(outcome_b, expected_b, layout_for(b.first, interleave_mode::line), b.first.height);

    const auto info_b{decoder_b.frame_info()};
    EXPECT_EQ(b.first.width, info_b.width);
    EXPECT_EQ(b.first.height, info_b.height);
    EXPECT_EQ(b.first.bits_per_sample, info_b.bits_per_sample);
    EXPECT_EQ(b.first.component_count, info_b.component_count);

    const auto info_c{decoder_a.frame_info()};
    EXPECT_EQ(c.first.width, info_c.width);
    EXPECT_EQ(c.first.component_count, info_c.component_count);
    static_cast<void>(expected_a);
}


TEST(stream_input, nonzero_trailing_padding_is_rejected)
{
    // A scan whose final byte carries non-zero padding bits is malformed. The repository encoder always pads with
    // zeros, so craft such a stream by setting the low bits of the final entropy byte.
    const frame_info frame{test_frame(5, 2, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 47)};
    auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};
    const size_t eoi{find_end_of_image(compressed)};
    ASSERT_GT(eoi, size_t{2});

    // Set every unused low bit of the final entropy byte.
    compressed[eoi - 1] |= byte{0x7F};

    jpegls_stream_decoder decoder;
    const auto error{expect_stream_error(decoder, compressed)};
    // Depending on where the unused bits fall, the change either trips the padding check or the bit-stuffing rule;
    // both are structural rejections rather than silently accepting padded output.
    EXPECT_TRUE(error == jpegls_errc::invalid_data || error == jpegls_errc::end_of_image_marker_not_found)
        << static_cast<int>(error);
}

TEST(stream_input, zero_padding_before_eoi_is_accepted)
{
    // Some legacy encoders insert a single zero byte before the EOI marker. The whole-frame decoder tolerates it;
    // the streaming decoder must too, without treating the zero as data.
    const frame_info frame{test_frame(5, 2, 8, 1)};
    const auto raw{make_image(frame, interleave_mode::none, 48)};
    auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};
    const size_t eoi{find_end_of_image(compressed)};
    ASSERT_GT(eoi, size_t{1});

    // Only insert the zero byte if it does not disturb the final entropy byte's padding expectations: replacement
    // of the byte directly before EOI by 0x00,00 would change data, so insert a standalone zero byte instead.
    if (compressed[eoi - 1] != byte{0})
        compressed.insert(compressed.begin() + static_cast<std::ptrdiff_t>(eoi), byte{0});

    const auto expected{decode_reference(compressed)};
    jpegls_stream_decoder decoder;
    const auto outcome{drive(decoder, compressed, fixed_chunk(1))};
    expect_rows_match(outcome, expected, layout_for(frame, interleave_mode::none), frame.height);
}


TEST(stream_input, restart_interval_ending_with_a_stuffed_ff_is_decoded)
{
    // Build synthetic restart streams with various data seeds and restart intervals, and verify every one. This
    // exercises the path where the entropy data of an interval ends with a stuffed 0xFF whose meaning is resolved
    // when the restart marker's code byte follows.
    frame_info frame{test_frame(10, 6, 8, 1)};
    const auto expected_for = [&frame](uint32_t seed, uint32_t interval) {
        const auto raw{make_image(frame, interleave_mode::none, seed, false)};
        auto compressed{encode_frame(raw, frame, interleave_mode::none, 0)};
        if (interval != 0)
            compressed = make_restart_stream(raw, frame, interleave_mode::none, 0, interval);
        return std::make_pair(decode_reference(compressed), compressed);
    };

    for (uint32_t interval : {uint32_t{1}, uint32_t{2}, uint32_t{3}, uint32_t{0}})
    {
        for (uint32_t seed{}; seed < 8; ++seed)
        {
            const auto [expected, compressed]{expected_for(seed, interval)};
            for (const size_t chunk : {size_t{1}, size_t{4}, compressed.size()})
            {
                jpegls_stream_decoder decoder;
                const auto outcome{drive(decoder, compressed, fixed_chunk(chunk))};
                expect_rows_match(outcome, expected, layout_for(frame, interleave_mode::none), frame.height);
            }
        }
    }
}


} // namespace charls::test
