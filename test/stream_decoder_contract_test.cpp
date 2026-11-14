// SPDX-FileCopyrightText: © 2026 Team CharLS
// SPDX-License-Identifier: BSD-3-Clause

// Contract tests for the incremental (push based) JPEG-LS stream decoder.
//
// The tests are registered in four suites:
//   stream_input      - consumed accounting, every split point, byte-by-byte input, truncation
//   stream_prediction - NEAR, preset coding parameters, ILV_LINE and ILV_SAMPLE prediction and run mode
//   stream_restart    - DRI / RST markers (including the bundled restart images)
//   stream_lifecycle  - done / reset / interleaved objects / storage lifetime / allocation behaviour

#include "pch.hpp"

#include "charls/charls.hpp"
#include "charls/jpegls_stream_decoder.hpp"

#include "support.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>

// Allocation tracking for the "does not retain the whole image" evidence. All global allocation
// entry points are replaced with malloc/free based versions so that placement, replacement and
// sized/nothrow/aligned variants stay paired. Only the total number of allocated bytes inside a
// measurement region is recorded; live bytes are not tracked, so no per-block header is needed.
namespace {

std::atomic<std::size_t> allocation_total_bytes{};
std::atomic<bool> allocation_in_region{};

void* tracked_allocate(const std::size_t size, const std::size_t alignment) noexcept
{
    void* pointer{};
    if (alignment <= alignof(std::max_align_t))
    {
        pointer = std::malloc(size == 0 ? 1 : size);
    }
    else if (posix_memalign(&pointer, alignment, size == 0 ? 1 : size) != 0)
    {
        pointer = nullptr;
    }

    if (pointer && allocation_in_region.load(std::memory_order_relaxed))
        allocation_total_bytes.fetch_add(size, std::memory_order_relaxed);
    return pointer;
}

void tracked_deallocate(void* pointer) noexcept
{
    std::free(pointer);
}

} // namespace

void* operator new(std::size_t size)
{
    if (void* pointer{tracked_allocate(size, alignof(std::max_align_t))})
        return pointer;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    return tracked_allocate(size, alignof(std::max_align_t));
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
{
    return ::operator new(size, tag);
}

void operator delete(void* pointer) noexcept
{
    tracked_deallocate(pointer);
}

void operator delete[](void* pointer) noexcept
{
    tracked_deallocate(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept
{
    tracked_deallocate(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept
{
    tracked_deallocate(pointer);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept
{
    tracked_deallocate(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept
{
    tracked_deallocate(pointer);
}

#if defined(__cpp_aligned_new)
void* operator new(std::size_t size, std::align_val_t alignment)
{
    if (void* pointer{tracked_allocate(size, static_cast<std::size_t>(alignment))})
        return pointer;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return tracked_allocate(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& tag) noexcept
{
    return ::operator new(size, alignment, tag);
}

void operator delete(void* pointer, std::align_val_t) noexcept
{
    tracked_deallocate(pointer);
}

void operator delete[](void* pointer, std::align_val_t) noexcept
{
    tracked_deallocate(pointer);
}

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept
{
    tracked_deallocate(pointer);
}

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept
{
    tracked_deallocate(pointer);
}

void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept
{
    tracked_deallocate(pointer);
}

void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept
{
    tracked_deallocate(pointer);
}
#endif

namespace charls::test {

namespace {

using std::byte;
using std::vector;

// ---------------------------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------------------------

constexpr size_t bytes_per_sample(const int32_t bits_per_sample) noexcept
{
    return static_cast<size_t>((bits_per_sample + 7) / 8);
}

// Reads a repository test data file. Works from the repository root and from the build tree.
// Build trees without a conformance/ subdirectory also find the files after the first copy step.
vector<byte> read_test_file(const char* name)
{
    const char* prefixes[]{"test/", "", ".check-build/test/", "../test/"};
    for (const char* prefix : prefixes)
    {
        std::ifstream input;
        input.open(std::string(prefix) + name, std::ios::in | std::ios::binary);
        if (!input && std::string(name).rfind("data/conformance/", 0) == 0)
        {
            // The build tree may keep the conformance files flat next to the other test data.
            char flat_name[256]{};
            std::snprintf(flat_name, sizeof(flat_name), "data/%s", name + 16);
            input.clear();
            input.open(std::string(prefix) + flat_name, std::ios::in | std::ios::binary);
        }
        if (!input)
            continue;

        input.seekg(0, std::ios::end);
        const auto byte_count{static_cast<size_t>(input.tellg())};
        input.seekg(0, std::ios::beg);
        vector<byte> buffer(byte_count);
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(byte_count));
        return buffer;
    }

    ADD_FAILURE() << "test data file not found: " << name;
    return {};
}

vector<byte> make_image_bytes(const frame_info& frame, const interleave_mode mode, const int32_t seed)
{
    const size_t sample_size{bytes_per_sample(frame.bits_per_sample)};
    const size_t components_in_line{mode == interleave_mode::none ? 1U : static_cast<size_t>(frame.component_count)};
    const size_t sample_count{static_cast<size_t>(frame.width) * components_in_line * frame.height};

    vector<byte> image(sample_count * sample_size);

    if (sample_size == 1)
    {
        for (size_t i{}; i != sample_count; ++i)
        {
            // Correlated data so that both regular mode and run mode are exercised.
            const auto x{static_cast<int32_t>(i % frame.width)};
            const auto y{static_cast<int32_t>(i / frame.width)};
            const auto value{((x * 3 + y * 11 + seed) ^ (x * y)) & ((1 << frame.bits_per_sample) - 1)};
            image[i] = static_cast<byte>(static_cast<uint8_t>(value));
        }
    }
    else
    {
        for (size_t i{}; i != sample_count; ++i)
        {
            const auto x{static_cast<int32_t>(i % frame.width)};
            const auto y{static_cast<int32_t>(i / frame.width)};
            const auto value{static_cast<uint16_t>((x * 37 + y * 101 + seed) & ((1U << frame.bits_per_sample) - 1U))};
            std::memcpy(image.data() + i * sizeof(uint16_t), &value, sizeof(value));
        }
    }

    return image;
}

struct encode_options final
{
    int32_t near_lossless = 0;
    jpegls_pc_parameters preset_parameters{};
    bool has_preset_parameters = false;
};

vector<byte> encode_image(const vector<byte>& source, const frame_info& frame, const interleave_mode mode,
                          const encode_options& options = {}, const int32_t restart_interval = 0)
{
    jpegls_encoder encoder;
    encoder.frame_info(frame).interleave_mode(mode).near_lossless(options.near_lossless);
    if (options.has_preset_parameters)
        encoder.preset_coding_parameters(options.preset_parameters);

    vector<byte> destination(encoder.estimated_destination_size());
    encoder.destination(destination);
    destination.resize(encoder.encode(source));

    if (restart_interval == 0)
        return destination;

    // Inject a DRI segment directly after the SOI marker. The bundled reference restart images are
    // produced the same way.
    static constexpr byte dri_marker[]{byte{0xFF}, byte{0xDD}, byte{0x00}, byte{0x04}};
    vector<byte> with_restarts;
    with_restarts.reserve(destination.size() + 8);
    with_restarts.push_back(byte{0xFF});
    with_restarts.push_back(byte{0xD8});
    with_restarts.insert(with_restarts.end(), std::begin(dri_marker), std::end(dri_marker));
    with_restarts.push_back(static_cast<byte>((restart_interval >> 8) & 0xFF));
    with_restarts.push_back(static_cast<byte>(restart_interval & 0xFF));
    with_restarts.insert(with_restarts.end(), destination.cbegin() + 2, destination.cend());
    return with_restarts;
}

// Reference decode using the existing whole-frame decoder.
vector<byte> reference_decode(const vector<byte>& encoded, frame_info* frame = nullptr,
                              interleave_mode* mode = nullptr)
{
    jpegls_decoder decoder{encoded, true};
    if (frame)
        *frame = decoder.frame_info();
    if (mode)
        *mode = decoder.get_interleave_mode();

    vector<byte> destination(decoder.get_destination_size());
    decoder.decode(destination);
    return destination;
}

// The incremental decoder rejects multi-scan planar color images before delivering pixels.
bool stream_decoder_supports(const vector<byte>& encoded)
{
    const jpegls_decoder probe{encoded, true};
    return !(probe.frame_info().component_count > 1 && probe.get_interleave_mode() == interleave_mode::none);
}

struct stream_run final
{
    vector<byte> rows;
    size_t consumed{};
    size_t pushes{};
    bool done{};
};

// Runs the decoder with increasing input windows. The caller always retains the bytes from the last
// accepted offset to the current window end; every push buffer is overwritten directly after the call
// to prove that the decoder does not keep references into it.
//
// chunk_sizes values must be positive. After the window reaches the end of the stream every further
// push is marked final, exactly as the contract requires.
stream_run decode_stream(const vector<byte>& encoded, const vector<size_t>& chunk_sizes)
{
    jpegls_stream_decoder decoder;
    stream_run run;

    size_t offset{};
    size_t chunk_index{};
    size_t available{};
    size_t idle_pushes{};
    vector<byte> scratch;

    while (!run.done)
    {
        if (++run.pushes > encoded.size() + static_cast<size_t>(encoded.front()) + 1000000)
        {
            ADD_FAILURE() << "stream decoder does not make progress";
            break;
        }

        const size_t wanted{std::max<size_t>(1, chunk_sizes[chunk_index++ % chunk_sizes.size()])};
        available = std::min(encoded.size(), std::max(available + wanted, offset));
        const bool final{available == encoded.size()};

        scratch.assign(encoded.cbegin() + static_cast<std::ptrdiff_t>(offset),
                       encoded.cbegin() + static_cast<std::ptrdiff_t>(available));
        const stream_decode_result result{decoder.push(scratch.data(), scratch.size(), final)};
        std::fill(scratch.begin(), scratch.end(), byte{0xA5});

        if (result.consumed > available - offset)
        {
            ADD_FAILURE() << "consumed exceeds the pushed size";
            break;
        }

        offset += result.consumed;
        run.consumed = offset;

        if (result.status == stream_decode_status::row_ready)
        {
            const auto [data, size]{decoder.row()};
            if (data == nullptr)
            {
                ADD_FAILURE() << "row() returned null";
                break;
            }
            run.rows.insert(run.rows.end(), data, data + size);
            idle_pushes = 0;
            continue;
        }

        if (result.status == stream_decode_status::done)
        {
            run.done = true;
            break;
        }

        if (result.consumed == 0 && final)
        {
            ADD_FAILURE() << "decoder needs input although the complete stream was supplied";
            break;
        }

        if (result.consumed == 0 && offset >= available)
        {
            if (++idle_pushes > 8)
            {
                ADD_FAILURE() << "decoder stopped making progress at offset " << offset;
                break;
            }
        }
        else
        {
            idle_pushes = 0;
        }
    }

    return run;
}

stream_run decode_stream_split_at(const vector<byte>& encoded, const size_t split)
{
    return decode_stream(encoded, {split == 0 ? size_t{1} : split});
}

// ---------------------------------------------------------------------------------------------
// stream_input: consumed accounting, splitting and truncation
// ---------------------------------------------------------------------------------------------

TEST(stream_input, decodes_with_every_two_way_split)
{
    struct image_spec final
    {
        frame_info frame;
        interleave_mode mode;
        encode_options options;
    };

    const image_spec specs[] = {
        {{17, 9, 8, 1}, interleave_mode::none, {}},
        {{11, 7, 8, 3}, interleave_mode::line, {}},
        {{13, 6, 8, 3}, interleave_mode::sample, {}},
        {{9, 5, 16, 2}, interleave_mode::line, {}},
        {{7, 4, 12, 1}, interleave_mode::none, {}},
        {{10, 8, 8, 2}, interleave_mode::sample, {.near_lossless = 2}},
    };

    for (const auto& spec : specs)
    {
        const vector<byte> source{make_image_bytes(spec.frame, spec.mode, static_cast<int32_t>(spec.frame.width))};
        const vector<byte> encoded{encode_image(source, spec.frame, spec.mode, spec.options)};

        frame_info decoded_frame;
        interleave_mode decoded_mode;
        const vector<byte> reference{reference_decode(encoded, &decoded_frame, &decoded_mode)};
        ASSERT_EQ(spec.frame.width, decoded_frame.width);
        ASSERT_EQ(spec.frame.height, decoded_frame.height);

        for (size_t split{}; split <= encoded.size(); ++split)
        {
            const auto run{decode_stream_split_at(encoded, split)};
            ASSERT_TRUE(run.done) << "split=" << split;
            EXPECT_EQ(reference, run.rows) << "split=" << split;
            EXPECT_EQ(encoded.size(), run.consumed) << "split=" << split;
        }

        // Every single split point with a one byte input window.
        {
            const auto run{decode_stream(encoded, {1})};
            ASSERT_TRUE(run.done);
            EXPECT_EQ(reference, run.rows);
            EXPECT_EQ(encoded.size(), run.consumed);
        }

        // Arbitrary interleaved chunk pattern.
        {
            const auto run{decode_stream(encoded, {1, 2, 3, 5, 8, 13})};
            ASSERT_TRUE(run.done);
            EXPECT_EQ(reference, run.rows);
        }
    }
}

TEST(stream_input, split_at_every_entropy_ff_byte)
{
    const auto encoded{read_test_file("data/tulips-gray-8bit-512-512-hp-encoder.jls")};
    const vector<byte> reference{reference_decode(encoded)};

    size_t tested{};
    for (size_t i{}; i + 1 < encoded.size() && tested < 200; ++i)
    {
        // 0xFF is part of the entropy data when the next byte has a zero MSB (bit stuffing).
        if (std::to_integer<int>(encoded[i]) != 0xFF || (std::to_integer<int>(encoded[i + 1]) & 0x80) != 0)
            continue;

        for (const size_t extra : {size_t{0}, size_t{1}})
        {
            const auto run{decode_stream_split_at(encoded, i + extra)};
            ASSERT_TRUE(run.done) << "split=" << i + extra;
            EXPECT_EQ(reference, run.rows) << "split=" << i + extra;
        }
        ++tested;
    }

    EXPECT_GT(tested, size_t{10});
}

TEST(stream_input, consumed_prefix_is_accepted_data_only)
{
    const frame_info frame{23, 11, 8, 1};
    const vector<byte> source{make_image_bytes(frame, interleave_mode::none, 7)};
    const vector<byte> encoded{encode_image(source, frame, interleave_mode::none)};
    const vector<byte> reference{reference_decode(encoded)};

    jpegls_stream_decoder decoder;

    const stream_decode_result first{decoder.push(encoded.data(), 4, false)};
    EXPECT_EQ(stream_decode_status::need_input, first.status);
    EXPECT_LE(first.consumed, size_t{4});

    assert_expect_exception(jpegls_errc::invalid_operation, [&decoder] { (void)decoder.frame_info(); });

    // Only the unaccepted suffix needs to be submitted again; the accepted prefix may be overwritten.
    vector<byte> tail(encoded.cbegin() + static_cast<std::ptrdiff_t>(first.consumed), encoded.cend());
    vector<byte> rows;

    for (;;)
    {
        const stream_decode_result result{decoder.push(tail.data(), tail.size(), true)};
        if (result.status == stream_decode_status::row_ready)
        {
            const auto [data, size]{decoder.row()};
            rows.insert(rows.end(), data, data + size);
            tail.erase(tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(result.consumed));
        }
        else if (result.status == stream_decode_status::done)
        {
            break;
        }
        else
        {
            ADD_FAILURE() << "unexpected need_input";
            break;
        }
    }

    EXPECT_EQ(reference, rows);
}

TEST(stream_input, empty_pushes_are_allowed_before_any_data)
{
    const frame_info frame{11, 6, 8, 1};
    const vector<byte> source{make_image_bytes(frame, interleave_mode::none, 13)};
    const vector<byte> encoded{encode_image(source, frame, interleave_mode::none)};
    const vector<byte> reference{reference_decode(encoded)};

    jpegls_stream_decoder decoder;
    for (int i{}; i != 3; ++i)
    {
        const stream_decode_result empty{decoder.push(nullptr, 0, false)};
        EXPECT_EQ(stream_decode_status::need_input, empty.status);
        EXPECT_EQ(0U, empty.consumed);
    }

    vector<byte> rows;
    size_t offset{};
    for (;;)
    {
        const stream_decode_result result{decoder.push(encoded.data() + offset, encoded.size() - offset, true)};
        offset += result.consumed;
        if (result.status == stream_decode_status::row_ready)
        {
            const auto [data, size]{decoder.row()};
            rows.insert(rows.end(), data, data + size);
        }
        else if (result.status == stream_decode_status::done)
        {
            break;
        }
    }

    EXPECT_EQ(reference, rows);
}

TEST(stream_input, truncated_stream_throws_need_more_data)
{
    const frame_info frame{19, 13, 8, 1};
    const vector<byte> source{make_image_bytes(frame, interleave_mode::none, 3)};
    const vector<byte> encoded{encode_image(source, frame, interleave_mode::none)};

    const size_t cut_points[]{size_t{3}, size_t{8}, encoded.size() / 2, encoded.size() - 1};

    for (const size_t cut : cut_points)
    {
        const vector<byte> truncated(encoded.cbegin(), encoded.cbegin() + static_cast<std::ptrdiff_t>(cut));

        jpegls_stream_decoder decoder;
        assert_expect_exception(jpegls_errc::need_more_data, [&decoder, &truncated] {
            size_t offset{};
            for (;;)
            {
                const stream_decode_result result{decoder.push(truncated.data() + offset, truncated.size() - offset, true)};
                offset += result.consumed;
                if (result.status == stream_decode_status::done)
                    return;
            }
        });
    }
}

TEST(stream_input, truncated_restart_stream_throws_need_more_data)
{
    const auto encoded{read_test_file("data/test16_rm_5.jls")};

    // Drop the last restart interval and the EOI marker.
    const vector<byte> truncated(encoded.cbegin(), encoded.cend() - 3);

    jpegls_stream_decoder decoder;
    assert_expect_exception(jpegls_errc::need_more_data, [&decoder, &truncated] {
        size_t offset{};
        for (;;)
        {
            const stream_decode_result result{decoder.push(truncated.data() + offset, truncated.size() - offset, true)};
            offset += result.consumed;
            if (result.status == stream_decode_status::done)
                return;
        }
    });
}

// ---------------------------------------------------------------------------------------------
// stream_prediction: NEAR, preset parameters, line/sample interleave prediction and run mode
// ---------------------------------------------------------------------------------------------

TEST(stream_prediction, lossless_line_and_sample_interleave_match_reference)
{
    const frame_info frame{29, 17, 8, 3};
    for (const auto mode : {interleave_mode::line, interleave_mode::sample})
    {
        const vector<byte> source{make_image_bytes(frame, mode, 5)};
        const vector<byte> encoded{encode_image(source, frame, mode)};
        const vector<byte> reference{reference_decode(encoded)};

        EXPECT_EQ(source, reference) << "self generated lossless image must round trip";
        const auto whole{decode_stream(encoded, {encoded.size()})};
        EXPECT_TRUE(whole.done);
        EXPECT_EQ(reference, whole.rows);

        for (const size_t split : {size_t{1}, size_t{3}, encoded.size() / 3, encoded.size() / 2, encoded.size() - 1})
        {
            const auto run{decode_stream_split_at(encoded, split)};
            ASSERT_TRUE(run.done);
            EXPECT_EQ(reference, run.rows);
        }
    }
}

TEST(stream_prediction, four_components_sample_interleave_match_reference)
{
    const frame_info frame{21, 9, 8, 4};
    const vector<byte> source{make_image_bytes(frame, interleave_mode::sample, 2)};
    const vector<byte> encoded{encode_image(source, frame, interleave_mode::sample)};
    const vector<byte> reference{reference_decode(encoded)};

    EXPECT_EQ(source, reference);
    const auto run{decode_stream(encoded, {1})};
    ASSERT_TRUE(run.done);
    EXPECT_EQ(reference, run.rows);
}

TEST(stream_prediction, two_component_line_interleave_matches_reference)
{
    const frame_info frame{15, 11, 8, 2};
    const vector<byte> source{make_image_bytes(frame, interleave_mode::line, 9)};
    const vector<byte> encoded{encode_image(source, frame, interleave_mode::line)};
    const vector<byte> reference{reference_decode(encoded)};

    EXPECT_EQ(source, reference);
    const auto run{decode_stream(encoded, {2, 1, 3})};
    ASSERT_TRUE(run.done);
    EXPECT_EQ(reference, run.rows);
}

TEST(stream_prediction, near_lossless_matches_reference_and_stays_within_bound)
{
    for (const auto mode : {interleave_mode::none, interleave_mode::line, interleave_mode::sample})
    {
        const int32_t components{mode == interleave_mode::none ? 1 : 3};
        const frame_info frame{31, 13, 8, components};
        const vector<byte> source{make_image_bytes(frame, mode, 4)};

        encode_options options;
        options.near_lossless = 3;

        const vector<byte> encoded{encode_image(source, frame, mode, options)};
        const vector<byte> reference{reference_decode(encoded)};

        for (const size_t chunk : {size_t{1}, size_t{2}, size_t{5}})
        {
            const auto run{decode_stream(encoded, {chunk})};
            ASSERT_TRUE(run.done);
            EXPECT_EQ(reference, run.rows);
            ASSERT_EQ(source.size(), run.rows.size());
            for (size_t i{}; i != source.size(); ++i)
            {
                if (bytes_per_sample(frame.bits_per_sample) == 1)
                {
                    EXPECT_LE(std::abs(static_cast<int>(source[i]) - static_cast<int>(run.rows[i])),
                              options.near_lossless);
                }
            }
        }
    }
}

TEST(stream_prediction, preset_coding_parameters_match_reference)
{
    const frame_info frame{17, 9, 8, 3};
    const vector<byte> source{make_image_bytes(frame, interleave_mode::sample, 8)};

    jpegls_pc_parameters preset{};
    preset.maximum_sample_value = 255;
    preset.threshold1 = 9;
    preset.threshold2 = 9;
    preset.threshold3 = 9;
    preset.reset_value = 31;

    encode_options options;
    options.preset_parameters = preset;
    options.has_preset_parameters = true;

    const vector<byte> encoded{encode_image(source, frame, interleave_mode::sample, options)};

    const jpegls_decoder probe{encoded, true};
    EXPECT_EQ(9, probe.preset_coding_parameters().threshold1);
    EXPECT_EQ(31, probe.preset_coding_parameters().reset_value);

    const vector<byte> reference{reference_decode(encoded)};
    EXPECT_EQ(source, reference);

    for (const size_t split : {size_t{1}, size_t{2}, size_t{11}, encoded.size() / 2})
    {
        const auto run{decode_stream_split_at(encoded, split)};
        ASSERT_TRUE(run.done);
        EXPECT_EQ(reference, run.rows);
    }
}

TEST(stream_prediction, low_bit_depth_images_match_reference)
{
    // 2, 4 and 10 bits exercise the dynamic quantization table and the masked row layout.
    for (const int32_t bits : {2, 4, 10})
    {
        const frame_info frame{23, 11, bits, 1};
        const vector<byte> source{make_image_bytes(frame, interleave_mode::none, bits)};
        const vector<byte> encoded{encode_image(source, frame, interleave_mode::none)};
        const vector<byte> reference{reference_decode(encoded)};

        EXPECT_EQ(source, reference) << "bits=" << bits;
        const auto run{decode_stream(encoded, {1, 2, 7})};
        ASSERT_TRUE(run.done) << "bits=" << bits;
        EXPECT_EQ(reference, run.rows) << "bits=" << bits;
    }
}

TEST(stream_prediction, sixteen_bit_prediction_matches_reference)
{
    const frame_info frame{27, 15, 16, 1};
    const vector<byte> source{make_image_bytes(frame, interleave_mode::none, 6)};
    const vector<byte> encoded{encode_image(source, frame, interleave_mode::none)};
    const vector<byte> reference{reference_decode(encoded)};

    EXPECT_EQ(source, reference);
    for (const size_t split : {size_t{1}, size_t{7}, size_t{64}, encoded.size() / 2})
    {
        const auto run{decode_stream_split_at(encoded, split)};
        ASSERT_TRUE(run.done);
        EXPECT_EQ(reference, run.rows);
    }
}

TEST(stream_prediction, run_mode_heavy_image_matches_reference)
{
    // A flat image with a few impulses forces long runs and run interruption samples.
    const frame_info frame{64, 33, 8, 1};
    vector<byte> source(static_cast<size_t>(frame.width) * frame.height, byte{0x40});
    source[static_cast<size_t>(frame.width) * 5 + 17] = byte{0x41};
    source[1] = byte{0x3F};
    source[source.size() - 1] = byte{0x41};

    const vector<byte> encoded{encode_image(source, frame, interleave_mode::none)};
    const vector<byte> reference{reference_decode(encoded)};
    EXPECT_EQ(source, reference);

    for (const size_t split : {size_t{1}, size_t{2}, size_t{5}, encoded.size() / 2, encoded.size() - 1})
    {
        const auto run{decode_stream_split_at(encoded, split)};
        ASSERT_TRUE(run.done);
        EXPECT_EQ(reference, run.rows);
    }
}

TEST(stream_prediction, bundled_conformance_images_match_reference)
{
    const char* files[]{"data/conformance/t8c1e0.jls", "data/conformance/t8c2e0.jls", "data/conformance/t8c1e3.jls",
                        "data/conformance/t8c2e3.jls", "data/conformance/t8nde0.jls", "data/conformance/t16e3.jls"};

    for (const char* file : files)
    {
        const auto encoded{read_test_file(file)};
        if (encoded.empty() || !stream_decoder_supports(encoded))
            continue;

        const vector<byte> reference{reference_decode(encoded)};
        const auto run{decode_stream(encoded, {1, 2, 3, 5, 8, 13})};
        EXPECT_TRUE(run.done) << file;
        EXPECT_EQ(reference, run.rows) << file;
    }
}

// ---------------------------------------------------------------------------------------------
// stream_restart: DRI and RST marker handling
// ---------------------------------------------------------------------------------------------

TEST(stream_restart, bundled_restart_images_match_reference)
{
    const char* files[]{"data/test8_ilv_line_rm_7.jls", "data/test8_ilv_sample_rm_7.jls",
                        "data/test8_ilv_sample_rm_300.jls", "data/test16_rm_5.jls"};

    for (const char* file : files)
    {
        const auto encoded{read_test_file(file)};
        if (encoded.empty() || !stream_decoder_supports(encoded))
            continue;

        const vector<byte> reference{reference_decode(encoded)};

        for (const size_t chunk : {size_t{1}, size_t{2}, size_t{7}, size_t{300}})
        {
            const auto run{decode_stream(encoded, {chunk})};
            EXPECT_TRUE(run.done) << file << " chunk=" << chunk;
            EXPECT_EQ(reference, run.rows) << file << " chunk=" << chunk;
        }
    }
}

TEST(stream_restart, splits_around_restart_markers_match_reference)
{
    const auto encoded{read_test_file("data/test8_ilv_line_rm_7.jls")};
    const vector<byte> reference{reference_decode(encoded)};

    vector<size_t> marker_positions;
    for (size_t i{}; i + 1 < encoded.size(); ++i)
    {
        const auto first{std::to_integer<int>(encoded[i])};
        const auto second{std::to_integer<int>(encoded[i + 1])};
        if (first == 0xFF && second >= 0xD0 && second <= 0xD7)
        {
            marker_positions.push_back(i);
            ++i;
        }
    }
    ASSERT_FALSE(marker_positions.empty());

    for (const size_t position : marker_positions)
    {
        for (const size_t delta : {size_t{0}, size_t{1}, size_t{2}})
        {
            const auto run{decode_stream_split_at(encoded, position + delta)};
            ASSERT_TRUE(run.done) << "split=" << position + delta;
            EXPECT_EQ(reference, run.rows) << "split=" << position + delta;
        }
    }
}

TEST(stream_restart, dri_segment_that_spans_the_whole_image_is_accepted)
{
    // A DRI value that is at least the image height means no restart marker is inserted. The segment must
    // still be parsed and must not disturb the decode (the encoder cannot emit RST markers itself).
    for (const auto mode : {interleave_mode::line, interleave_mode::sample})
    {
        const frame_info frame{23, 17, 8, 3};
        const vector<byte> source{make_image_bytes(frame, mode, 4)};
        const vector<byte> encoded{encode_image(source, frame, mode, {}, static_cast<int32_t>(frame.height))};
        const vector<byte> reference{reference_decode(encoded)};

        const auto run{decode_stream(encoded, {1, 2, 3})};
        ASSERT_TRUE(run.done);
        EXPECT_EQ(reference, run.rows);
    }
}

TEST(stream_restart, wrong_restart_marker_sequence_throws)
{
    auto encoded{read_test_file("data/test8_ilv_sample_rm_7.jls")};
    ASSERT_FALSE(encoded.empty());

    bool modified{};
    for (size_t i{}; i + 1 < encoded.size(); ++i)
    {
        if (std::to_integer<int>(encoded[i]) == 0xFF && std::to_integer<int>(encoded[i + 1]) == 0xD0)
        {
            encoded[i + 1] = byte{0xD5};
            modified = true;
            break;
        }
    }
    ASSERT_TRUE(modified);

    jpegls_stream_decoder decoder;
    assert_expect_exception(jpegls_errc::restart_marker_not_found, [&decoder, &encoded] {
        size_t offset{};
        for (;;)
        {
            const stream_decode_result result{decoder.push(encoded.data() + offset, encoded.size() - offset, true)};
            offset += result.consumed;
            if (result.status == stream_decode_status::done)
                return;
        }
    });
}

TEST(stream_restart, restart_marker_mismatch_after_reset_is_rejected)
{
    auto encoded{read_test_file("data/test8_ilv_sample_rm_7.jls")};
    ASSERT_FALSE(encoded.empty());

    // Remove the first restart marker entirely: the decoder must not treat the following bytes as a marker.
    bool removed{};
    for (size_t i{}; i + 1 < encoded.size(); ++i)
    {
        if (std::to_integer<int>(encoded[i]) == 0xFF && std::to_integer<int>(encoded[i + 1]) == 0xD0)
        {
            encoded.erase(encoded.begin() + static_cast<std::ptrdiff_t>(i),
                          encoded.begin() + static_cast<std::ptrdiff_t>(i + 2));
            removed = true;
            break;
        }
    }
    ASSERT_TRUE(removed);

    jpegls_stream_decoder decoder;
    assert_expect_exception(jpegls_errc::restart_marker_not_found, [&decoder, &encoded] {
        size_t offset{};
        for (;;)
        {
            const stream_decode_result result{decoder.push(encoded.data() + offset, encoded.size() - offset, true)};
            offset += result.consumed;
            if (result.status == stream_decode_status::done)
                return;
        }
    });
}

// ---------------------------------------------------------------------------------------------
// stream_lifecycle: done, reset, interleaving, storage lifetime and allocation behaviour
// ---------------------------------------------------------------------------------------------

TEST(stream_lifecycle, done_is_terminal_and_does_not_consume_the_suffix_after_eoi)
{
    const frame_info frame{9, 5, 8, 1};
    const vector<byte> source{make_image_bytes(frame, interleave_mode::none, 1)};
    const vector<byte> encoded{encode_image(source, frame, interleave_mode::none)};

    vector<byte> with_suffix{encoded};
    with_suffix.push_back(byte{0x12});
    with_suffix.push_back(byte{0x34});

    jpegls_stream_decoder decoder;
    size_t offset{};
    size_t rows{};
    for (;;)
    {
        const stream_decode_result result{decoder.push(with_suffix.data() + offset, with_suffix.size() - offset, true)};
        offset += result.consumed;
        if (result.status == stream_decode_status::row_ready)
        {
            ++rows;
            continue;
        }
        EXPECT_EQ(stream_decode_status::done, result.status);
        break;
    }

    EXPECT_EQ(static_cast<size_t>(frame.height), rows);
    EXPECT_EQ(encoded.size(), offset); // the two bytes after EOI stay with the caller.

    const stream_decode_result after{decoder.push(with_suffix.data() + offset, 2, true)};
    EXPECT_EQ(0U, after.consumed);
    EXPECT_EQ(stream_decode_status::done, after.status);

    assert_expect_exception(jpegls_errc::invalid_operation, [&decoder] { (void)decoder.row(); });
}

TEST(stream_lifecycle, row_is_only_valid_directly_after_row_ready)
{
    const frame_info frame{8, 4, 8, 1};
    const vector<byte> source{make_image_bytes(frame, interleave_mode::none, 2)};
    const vector<byte> encoded{encode_image(source, frame, interleave_mode::none)};

    jpegls_stream_decoder decoder;
    assert_expect_exception(jpegls_errc::invalid_operation, [&decoder] { (void)decoder.row(); });

    const stream_decode_result first{decoder.push(encoded.data(), encoded.size(), true)};
    ASSERT_EQ(stream_decode_status::row_ready, first.status);
    const auto [data, size]{decoder.row()};
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(static_cast<size_t>(frame.width), size);

    // The row size matches the documented layout: width * components * bytes per sample.
    EXPECT_EQ(static_cast<size_t>(frame.width) * bytes_per_sample(frame.bits_per_sample), size);

    decoder.reset();
    assert_expect_exception(jpegls_errc::invalid_operation, [&decoder] { (void)decoder.row(); });
    assert_expect_exception(jpegls_errc::invalid_operation, [&decoder] { (void)decoder.frame_info(); });
}

TEST(stream_lifecycle, reset_allows_reuse_with_a_different_frame_size)
{
    jpegls_stream_decoder decoder;

    auto decode_with = [](jpegls_stream_decoder& target, const vector<byte>& encoded) {
        vector<byte> rows;
        size_t offset{};
        for (;;)
        {
            const stream_decode_result result{target.push(encoded.data() + offset, encoded.size() - offset, true)};
            offset += result.consumed;
            if (result.status == stream_decode_status::row_ready)
            {
                const auto [data, size]{target.row()};
                rows.insert(rows.end(), data, data + size);
                continue;
            }
            if (result.status == stream_decode_status::done)
                break;
            ADD_FAILURE() << "unexpected need_input";
            break;
        }
        return rows;
    };

    const frame_info first_frame{9, 5, 8, 1};
    const vector<byte> first_source{make_image_bytes(first_frame, interleave_mode::none, 4)};
    const vector<byte> first_encoded{encode_image(first_source, first_frame, interleave_mode::none)};
    EXPECT_EQ(reference_decode(first_encoded), decode_with(decoder, first_encoded));

    decoder.reset();
    assert_expect_exception(jpegls_errc::invalid_operation, [&decoder] { (void)decoder.frame_info(); });

    const frame_info second_frame{13, 11, 8, 3};
    const vector<byte> second_source{make_image_bytes(second_frame, interleave_mode::sample, 3)};
    const vector<byte> second_encoded{encode_image(second_source, second_frame, interleave_mode::sample)};
    EXPECT_EQ(reference_decode(second_encoded), decode_with(decoder, second_encoded));
    EXPECT_EQ(13U, decoder.frame_info().width);
    EXPECT_EQ(11U, decoder.frame_info().height);
    EXPECT_EQ(3, decoder.frame_info().component_count);
}

TEST(stream_lifecycle, interleaved_objects_keep_separate_state)
{
    const frame_info frame_a{17, 7, 8, 1};
    const vector<byte> source_a{make_image_bytes(frame_a, interleave_mode::none, 4)};
    const vector<byte> encoded_a{encode_image(source_a, frame_a, interleave_mode::none)};

    const frame_info frame_b{9, 13, 16, 3};
    const vector<byte> source_b{make_image_bytes(frame_b, interleave_mode::sample, 5)};
    const vector<byte> encoded_b{encode_image(source_b, frame_b, interleave_mode::sample)};

    const vector<byte> reference_a{reference_decode(encoded_a)};
    const vector<byte> reference_b{reference_decode(encoded_b)};

    jpegls_stream_decoder decoder_a;
    jpegls_stream_decoder decoder_b;

    vector<byte> rows_a;
    vector<byte> rows_b;
    size_t offset_a{};
    size_t offset_b{};
    size_t available_a{};
    size_t available_b{};
    bool done_a{};
    bool done_b{};

    while (!done_a || !done_b)
    {
        if (!done_a)
        {
            available_a = std::min(encoded_a.size(), std::max(available_a + 3, offset_a));
            const stream_decode_result result{decoder_a.push(encoded_a.data() + offset_a, available_a - offset_a,
                                                            available_a == encoded_a.size())};
            offset_a += result.consumed;
            if (result.status == stream_decode_status::row_ready)
            {
                const auto [data, size]{decoder_a.row()};
                rows_a.insert(rows_a.end(), data, data + size);
            }
            else if (result.status == stream_decode_status::done)
            {
                done_a = true;
            }
            else if (result.consumed == 0 && offset_a >= available_a && available_a == encoded_a.size())
            {
                ADD_FAILURE() << "decoder_a stalled";
                break;
            }
        }

        if (!done_b)
        {
            available_b = std::min(encoded_b.size(), std::max(available_b + 2, offset_b));
            const stream_decode_result result{decoder_b.push(encoded_b.data() + offset_b, available_b - offset_b,
                                                            available_b == encoded_b.size())};
            offset_b += result.consumed;
            if (result.status == stream_decode_status::row_ready)
            {
                const auto [data, size]{decoder_b.row()};
                rows_b.insert(rows_b.end(), data, data + size);
            }
            else if (result.status == stream_decode_status::done)
            {
                done_b = true;
            }
            else if (result.consumed == 0 && offset_b >= available_b && available_b == encoded_b.size())
            {
                ADD_FAILURE() << "decoder_b stalled";
                break;
            }
        }
    }

    EXPECT_EQ(reference_a, rows_a);
    EXPECT_EQ(reference_b, rows_b);
}

TEST(stream_lifecycle, error_requires_reset_before_reuse)
{
    const vector<byte> garbage{byte{0xFF}, byte{0xD8}, byte{0xFF}, byte{0xE0}, byte{0x00}, byte{0x02},
                               byte{0xFF}, byte{0x01}, byte{0x00}, byte{0x00}};

    jpegls_stream_decoder decoder;
    assert_expect_exception(jpegls_errc::unknown_jpeg_marker_found,
                            [&decoder, &garbage] { (void)decoder.push(garbage.data(), garbage.size(), true); });

    assert_expect_exception(jpegls_errc::invalid_operation,
                            [&decoder, &garbage] { (void)decoder.push(garbage.data(), garbage.size(), true); });
    assert_expect_exception(jpegls_errc::invalid_operation, [&decoder] { (void)decoder.frame_info(); });

    decoder.reset();

    const frame_info frame{7, 3, 8, 1};
    const vector<byte> source{make_image_bytes(frame, interleave_mode::none, 0)};
    const vector<byte> encoded{encode_image(source, frame, interleave_mode::none)};
    const auto run{decode_stream(encoded, {1})};
    EXPECT_TRUE(run.done);
    EXPECT_EQ(reference_decode(encoded), run.rows);
}

TEST(stream_lifecycle, row_bytes_use_the_same_layout_as_the_whole_frame_decoder)
{
    const frame_info frame{12, 6, 8, 3};
    const vector<byte> source{make_image_bytes(frame, interleave_mode::sample, 31)};
    const vector<byte> encoded{encode_image(source, frame, interleave_mode::sample)};
    const vector<byte> reference{reference_decode(encoded)};

    const auto run{decode_stream(encoded, {1, 4})};
    ASSERT_TRUE(run.done);

    // Rows are delivered in order and concatenated equal the whole-frame output.
    EXPECT_EQ(reference, run.rows);
    EXPECT_EQ(static_cast<size_t>(frame.height) * frame.width * frame.component_count, run.rows.size());

    // Layout check: the first row must contain the first row of the reference buffer.
    const size_t row_bytes{static_cast<size_t>(frame.width) * frame.component_count * bytes_per_sample(frame.bits_per_sample)};
    EXPECT_EQ(0, std::memcmp(run.rows.data(), reference.data(), row_bytes));
}

TEST(stream_lifecycle, high_narrow_image_does_not_retain_the_whole_image)
{
    // Tall and narrow: one sample per line. The internal storage may grow with the row width and the
    // sample precision, but must not grow with the image height.
    auto measure = [](const uint32_t height) {
        const frame_info frame{1, height, 8, 1};
        vector<byte> source(height);
        for (size_t i{}; i != source.size(); ++i)
        {
            source[i] = static_cast<byte>(i & 0xFFU);
        }

        const vector<byte> encoded{encode_image(source, frame, interleave_mode::none)};

        // Warm up the shared paths once so the measurement itself does not include one-time library
        // allocations (the framing buffer of the test executable is deliberately excluded).
        {
            jpegls_stream_decoder warmup;
            (void)warmup.push(encoded.data(), 1, false);
        }

        struct measurement final
        {
            size_t total_bytes{};
            size_t rows{};
            size_t pushes{};
            size_t consumed{};
        } result{};

        allocation_total_bytes.store(0);
        allocation_in_region.store(true);

        jpegls_stream_decoder decoder;
        size_t offset{};

        for (;;)
        {
            const stream_decode_result r{decoder.push(encoded.data() + offset, encoded.size() - offset, true)};
            offset += r.consumed;
            ++result.pushes;
            if (r.status == stream_decode_status::row_ready)
            {
                const auto [data, size]{decoder.row()};
                (void)data;
                (void)size;
                ++result.rows;
                continue;
            }
            if (r.status == stream_decode_status::done)
                break;
        }

        allocation_in_region.store(false);
        result.total_bytes = allocation_total_bytes.load();
        result.consumed = offset;
        return result;
    };

    const auto short_image{measure(10000)};
    const auto tall_image{measure(40000)};

    std::printf("[stream_lifecycle] allocation evidence: height=10000 rows=%zu pushes=%zu consumed=%zu "
                "total_alloc=%zu bytes\n",
                short_image.rows, short_image.pushes, short_image.consumed, short_image.total_bytes);
    std::printf("[stream_lifecycle] allocation evidence: height=40000 rows=%zu pushes=%zu consumed=%zu "
                "total_alloc=%zu bytes\n",
                tall_image.rows, tall_image.pushes, tall_image.consumed, tall_image.total_bytes);

    EXPECT_EQ(10000U, short_image.rows);
    EXPECT_EQ(40000U, tall_image.rows);
    EXPECT_GE(short_image.pushes, short_image.rows);
    EXPECT_GE(tall_image.pushes, tall_image.rows);
    // Each row requires its own push (no look-ahead buffering of rows).
    EXPECT_LE(short_image.pushes, short_image.rows + 8);
    EXPECT_LE(tall_image.pushes, tall_image.rows + 8);

    // The decoder must not allocate in proportion to the image height: allocating a whole image buffer
    // would show up as a total allocation increase of 30000 bytes between the two measurements.
    EXPECT_LE(tall_image.total_bytes, short_image.total_bytes + 4096);
    EXPECT_LT(tall_image.total_bytes, 1024U * 1024U);
}

TEST(stream_lifecycle, chunk_size_does_not_change_decoded_rows)
{
    const frame_info frame{31, 19, 8, 1};
    const vector<byte> source{make_image_bytes(frame, interleave_mode::none, 21)};
    const vector<byte> encoded{encode_image(source, frame, interleave_mode::none)};
    const vector<byte> reference{reference_decode(encoded)};

    for (const size_t chunk : {size_t{1}, size_t{2}, size_t{4}, size_t{8}, size_t{16}, size_t{64}})
    {
        const auto run{decode_stream(encoded, {chunk})};
        ASSERT_TRUE(run.done);
        EXPECT_EQ(reference, run.rows) << "chunk=" << chunk;
        EXPECT_EQ(encoded.size(), run.consumed) << "chunk=" << chunk;
    }
}

} // namespace

} // namespace charls::test
