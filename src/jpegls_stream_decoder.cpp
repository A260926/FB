// SPDX-FileCopyrightText: © 2026 Team CharLS
// SPDX-License-Identifier: BSD-3-Clause

#include "pch.hpp"

#include "charls/jpegls_stream_decoder.hpp"

#include "constants.hpp"
#include "default_traits.hpp"
#include "golomb_lut.hpp"
#include "jpeg_marker_code.hpp"
#include "jpegls_algorithm.hpp"
#include "jpegls_preset_coding_parameters.hpp"
#include "regular_mode_context.hpp"
#include "run_mode_context.hpp"
#include "scan_codec.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace charls {

namespace {

/// <summary>
/// Control flow signal used internally to stop the current decode step when more encoded input is required.
/// State that was committed before the current coding action remains valid; the action itself is either resumed
/// or rolled back to its start before the signal is thrown.
/// </summary>
struct need_input_signal final
{};

// Note: named 'throw_error' because the nested class 'impl' would otherwise hide the charls::impl namespace.
[[noreturn]]
inline void throw_error(const jpegls_errc error_value)
{
    charls::impl::throw_jpegls_error(error_value);
}

constexpr uint8_t to_byte(const std::byte value) noexcept
{
    return std::to_integer<uint8_t>(value);
}

constexpr uint16_t read_be16(const std::byte* data) noexcept
{
    return static_cast<uint16_t>((static_cast<uint16_t>(to_byte(data[0])) << 8U) | to_byte(data[1]));
}

constexpr uint32_t read_be32(const std::byte* data) noexcept
{
    return (static_cast<uint32_t>(to_byte(data[0])) << 24U) | (static_cast<uint32_t>(to_byte(data[1])) << 16U) |
           (static_cast<uint32_t>(to_byte(data[2])) << 8U) | to_byte(data[3]);
}

constexpr bool is_known_jpeg_sof_marker(const uint8_t marker_code) noexcept
{
    switch (marker_code)
    {
    case 0xC0: // SOF_0
    case 0xC1: // SOF_1
    case 0xC2: // SOF_2
    case 0xC3: // SOF_3
    case 0xC5: // SOF_5
    case 0xC6: // SOF_6
    case 0xC7: // SOF_7
    case 0xC9: // SOF_9
    case 0xCA: // SOF_10
    case 0xCB: // SOF_11
    case 0xF9: // SOF_57 (JPEG-LS extended, ISO/IEC 14495-2)
        return true;
    default:
        return false;
    }
}

constexpr bool is_restart_marker(const uint8_t marker_code) noexcept
{
    return marker_code >= jpeg_restart_marker_base && marker_code < jpeg_restart_marker_base + jpeg_restart_marker_range;
}

constexpr bool is_application_data(const uint8_t marker_code) noexcept
{
    return marker_code >= static_cast<uint8_t>(jpeg_marker_code::application_data0) &&
           marker_code <= static_cast<uint8_t>(jpeg_marker_code::application_data15);
}

} // namespace


struct jpegls_stream_decoder::impl final
{
    impl()
    {
        reset();
    }

    void reset() noexcept
    {
        input_ = nullptr;
        input_size_ = 0;
        input_pos_ = 0;
        final_ = false;
        failed_ = false;

        phase_ = phase::header;
        row_valid_ = false;

        header_state_ = header_parse_state::soi_ff;
        segment_kind_ = segment_kind::none;
        marker_code_ = 0;
        length_high_ = 0;
        body_len_ = 0;
        body_target_ = 0;
        skip_remaining_ = 0;
        std::vector<std::byte>().swap(segment_);
        in_spiff_directory_ = false;
        seen_start_of_frame_ = false;
        scan_started_ = false;

        frame_width_ = 0;
        frame_height_ = 0;
        bits_per_sample_ = 0;
        frame_component_count_ = 0;
        std::vector<uint8_t>().swap(component_ids_);

        has_preset_parameters_ = false;
        preset_parameters_ = {};
        maximum_sample_value_ = 0;
        threshold1_ = 0;
        threshold2_ = 0;
        threshold3_ = 0;
        reset_threshold_ = 0;
        restart_interval_ = 0;

        near_lossless_ = 0;
        interleave_ = interleave_mode::none;

        traits_.reset();
        std::vector<int8_t>().swap(quantization_lut_);
        quantization_ = nullptr;
        std::vector<int32_t>().swap(line_storage_);
        std::vector<std::byte>().swap(row_buffer_);
        for (auto& context : regular_mode_contexts_)
        {
            context = regular_mode_context{};
        }
        run_mode_contexts_[0] = run_mode_context{};
        run_mode_contexts_[1] = run_mode_context{};
        run_indexes_.fill(0);

        components_ = 0;
        samples_per_pixel_ = 1;
        line_count_ = 1;
        pixel_stride_ = 0;
        output_component_count_ = 0;
        bytes_per_sample_ = 1;
        row_ = 0;
        component_ = 0;
        index_ = 1;
        rows_since_restart_ = 0;
        restart_interval_rows_ = 0;
        restart_marker_counter_ = 0;
        restart_pending_ = false;
        restart_state_ = restart_parse_state::start_byte;

        cache_ = 0;
        valid_bits_ = 0;

        run_phase_ = run_phase::idle;
        run_total_ = 0;
        run_done_ = 0;
        run_ra_.fill(0);
        interruption_component_ = 0;

        eoi_state_ = eoi_parse_state::drain_padding;
    }

    // ------------------------------------------------------------------ input

    [[nodiscard]]
    bool has_byte() const noexcept
    {
        return input_pos_ < input_size_;
    }

    [[nodiscard]]
    std::byte peek_input_byte() const noexcept
    {
        return input_[input_pos_];
    }

    std::byte take_input_byte() noexcept
    {
        return input_[input_pos_++];
    }

    void require_byte(const jpegls_errc eof_error)
    {
        if (has_byte())
            return;

        if (final_)
            throw_error(eof_error);

        throw need_input_signal{};
    }

    // ------------------------------------------------------------------ header parsing

    enum class phase
    {
        header,
        scan,
        end_of_image,
        done
    };

    enum class header_parse_state
    {
        soi_ff,
        soi_code,
        marker_ff,
        marker_code,
        length_hi,
        length_lo,
        collect_body,
        skip_body
    };

    enum class segment_kind
    {
        none,
        start_of_frame,
        start_of_scan,
        preset_parameters,
        restart_interval,
        application_data8,
        spiff_entry,
        skip
    };

    void parse_header_until_scan()
    {
        for (;;)
        {
            step_header();
            if (scan_started_)
            {
                scan_started_ = false;
                phase_ = phase::scan;
                return;
            }
        }
    }

    void step_header()
    {
        switch (header_state_)
        {
        case header_parse_state::soi_ff: {
            require_byte(jpegls_errc::need_more_data);
            if (take_input_byte() != jpeg_marker_start_byte)
                throw_error(jpegls_errc::jpeg_marker_start_byte_not_found);
            header_state_ = header_parse_state::soi_code;
            return;
        }

        case header_parse_state::soi_code: {
            require_byte(jpegls_errc::need_more_data);
            const auto value{take_input_byte()};
            if (value == jpeg_marker_start_byte)
                return; // Skip fill bytes.
            if (to_byte(value) != static_cast<uint8_t>(jpeg_marker_code::start_of_image))
                throw_error(jpegls_errc::start_of_image_marker_not_found);
            header_state_ = header_parse_state::marker_ff;
            return;
        }

        case header_parse_state::marker_ff: {
            require_byte(jpegls_errc::need_more_data);
            if (take_input_byte() != jpeg_marker_start_byte)
                throw_error(jpegls_errc::jpeg_marker_start_byte_not_found);
            header_state_ = header_parse_state::marker_code;
            return;
        }

        case header_parse_state::marker_code: {
            require_byte(jpegls_errc::need_more_data);
            const auto value{take_input_byte()};
            if (value == jpeg_marker_start_byte)
                return; // Skip fill bytes.
            marker_code_ = to_byte(value);
            handle_marker_code();
            return;
        }

        case header_parse_state::length_hi:
            require_byte(jpegls_errc::need_more_data);
            length_high_ = to_byte(take_input_byte());
            header_state_ = header_parse_state::length_lo;
            return;

        case header_parse_state::length_lo: {
            require_byte(jpegls_errc::need_more_data);
            const uint16_t segment_length{
                static_cast<uint16_t>((static_cast<uint16_t>(length_high_) << 8U) | to_byte(take_input_byte()))};
            if (segment_length < segment_length_size)
                throw_error(jpegls_errc::invalid_marker_segment_size);
            body_len_ = static_cast<size_t>(segment_length) - segment_length_size;
            start_segment();
            return;
        }

        case header_parse_state::collect_body:
            while (segment_.size() < body_target_)
            {
                require_byte(jpegls_errc::need_more_data);
                segment_.push_back(take_input_byte());
            }

            process_segment();
            if (scan_started_)
                return;
            header_state_ = skip_remaining_ != 0 ? header_parse_state::skip_body : header_parse_state::marker_ff;
            return;

        case header_parse_state::skip_body:
            while (skip_remaining_ != 0 && has_byte())
            {
                take_input_byte();
                --skip_remaining_;
            }

            if (skip_remaining_ != 0)
            {
                if (final_)
                    throw_error(jpegls_errc::need_more_data);
                throw need_input_signal{};
            }

            header_state_ = header_parse_state::marker_ff;
            return;
        }
    }

    void handle_marker_code()
    {
        if (marker_code_ == static_cast<uint8_t>(jpeg_marker_code::end_of_image))
            throw_error(jpegls_errc::unexpected_end_of_image_marker);
        if (marker_code_ == static_cast<uint8_t>(jpeg_marker_code::start_of_image))
            throw_error(jpegls_errc::duplicate_start_of_image_marker);

        if (in_spiff_directory_)
        {
            // ISO/IEC 10918-3, F.2.2: only APP8 segments are allowed inside the SPIFF directory.
            if (marker_code_ != static_cast<uint8_t>(jpeg_marker_code::application_data8))
                throw_error(jpegls_errc::missing_end_of_spiff_directory);
            segment_kind_ = segment_kind::spiff_entry;
            header_state_ = header_parse_state::length_hi;
            return;
        }

        switch (marker_code_)
        {
        case static_cast<uint8_t>(jpeg_marker_code::start_of_frame_jpegls):
            if (seen_start_of_frame_)
                throw_error(jpegls_errc::duplicate_start_of_frame_marker);
            segment_kind_ = segment_kind::start_of_frame;
            break;

        case static_cast<uint8_t>(jpeg_marker_code::start_of_scan):
            if (!seen_start_of_frame_)
                throw_error(jpegls_errc::unexpected_start_of_scan_marker);
            segment_kind_ = segment_kind::start_of_scan;
            break;

        case static_cast<uint8_t>(jpeg_marker_code::define_restart_interval):
            segment_kind_ = segment_kind::restart_interval;
            break;

        case static_cast<uint8_t>(jpeg_marker_code::jpegls_preset_parameters):
            segment_kind_ = segment_kind::preset_parameters;
            break;

        case static_cast<uint8_t>(jpeg_marker_code::define_number_of_lines):
            // DNL is only allowed when the frame height is signaled as unknown, which is not supported.
            throw_error(jpegls_errc::unexpected_define_number_of_lines_marker);

        case static_cast<uint8_t>(jpeg_marker_code::comment):
            segment_kind_ = segment_kind::skip;
            break;

        case static_cast<uint8_t>(jpeg_marker_code::application_data8):
            segment_kind_ = segment_kind::application_data8;
            break;

        default:
            if (is_application_data(marker_code_))
                segment_kind_ = segment_kind::skip;
            else if (is_restart_marker(marker_code_))
                throw_error(jpegls_errc::unexpected_restart_marker);
            else if (is_known_jpeg_sof_marker(marker_code_))
                throw_error(jpegls_errc::encoding_not_supported);
            else
                throw_error(jpegls_errc::unknown_jpeg_marker_found);
            break;
        }

        header_state_ = header_parse_state::length_hi;
    }

    void start_segment()
    {
        skip_remaining_ = 0;
        switch (segment_kind_)
        {
        case segment_kind::skip:
            skip_remaining_ = body_len_;
            segment_.clear();
            header_state_ = body_len_ != 0 ? header_parse_state::skip_body : header_parse_state::marker_ff;
            return;

        case segment_kind::application_data8: {
            segment_.clear();
            if (body_len_ >= 30)
                body_target_ = 30;
            else if (body_len_ == 5)
                body_target_ = 5;
            else
            {
                skip_remaining_ = body_len_;
                header_state_ = body_len_ != 0 ? header_parse_state::skip_body : header_parse_state::marker_ff;
                return;
            }
            header_state_ = header_parse_state::collect_body;
            return;
        }

        case segment_kind::spiff_entry:
            if (body_len_ < 4)
                throw_error(jpegls_errc::invalid_marker_segment_size);
            segment_.clear();
            body_target_ = 4;
            header_state_ = header_parse_state::collect_body;
            return;

        case segment_kind::start_of_frame:
            // A SOF segment holds 6 fixed bytes plus 3 bytes per component (at most 255 components).
            if (body_len_ < 6 || body_len_ > 6 + 3 * maximum_component_count)
                throw_error(jpegls_errc::invalid_marker_segment_size);
            segment_.clear();
            body_target_ = body_len_;
            header_state_ = header_parse_state::collect_body;
            return;

        case segment_kind::start_of_scan:
            // An SOS segment holds 4 fixed bytes plus 2 bytes per scan component (at most 4 components).
            if (body_len_ < 1 || body_len_ > 4 + 2 * maximum_component_count_in_scan)
                throw_error(jpegls_errc::invalid_marker_segment_size);
            segment_.clear();
            body_target_ = body_len_;
            header_state_ = header_parse_state::collect_body;
            return;

        case segment_kind::preset_parameters:
            if (body_len_ < 1)
                throw_error(jpegls_errc::invalid_marker_segment_size);
            segment_.clear();
            // Only the type byte and a preset coding parameters payload are supported; other lengths are rejected
            // after the type byte has been read.
            body_target_ = std::min<size_t>(body_len_, 11);
            header_state_ = header_parse_state::collect_body;
            return;

        case segment_kind::restart_interval:
            if (body_len_ < 2 || body_len_ > 4)
                throw_error(jpegls_errc::invalid_marker_segment_size);
            segment_.clear();
            body_target_ = body_len_;
            header_state_ = header_parse_state::collect_body;
            return;

        case segment_kind::none:
            break;
        }

        throw_error(jpegls_errc::invalid_marker_segment_size);
    }

    void process_segment()
    {
        switch (segment_kind_)
        {
        case segment_kind::start_of_frame:
            process_start_of_frame();
            break;
        case segment_kind::start_of_scan:
            process_start_of_scan();
            break;
        case segment_kind::preset_parameters:
            process_preset_parameters();
            break;
        case segment_kind::restart_interval:
            process_restart_interval();
            break;
        case segment_kind::application_data8:
            process_application_data8();
            break;
        case segment_kind::spiff_entry:
            process_spiff_entry();
            break;
        case segment_kind::skip:
        case segment_kind::none:
            break;
        }
    }

    void process_start_of_frame()
    {
        ASSERT(segment_.size() >= 6);

        const auto bits_per_sample{to_byte(segment_[0])};
        if (bits_per_sample < minimum_bits_per_sample || bits_per_sample > maximum_bits_per_sample)
            throw_error(jpegls_errc::invalid_parameter_bits_per_sample);

        const auto height{static_cast<uint32_t>(read_be16(segment_.data() + 1))};
        const auto width{static_cast<uint32_t>(read_be16(segment_.data() + 3))};
        const auto component_count{to_byte(segment_[5])};
        if (component_count == 0)
            throw_error(jpegls_errc::invalid_parameter_component_count_sof);

        if (segment_.size() != static_cast<size_t>(component_count) * 3 + 6)
            throw_error(jpegls_errc::invalid_marker_segment_size);

        if (width > maximum_width)
            throw_error(jpegls_errc::invalid_parameter_width);
        if (height != 0 && (height < minimum_height || height > maximum_height))
            throw_error(jpegls_errc::invalid_parameter_height);

        bits_per_sample_ = static_cast<int32_t>(bits_per_sample);
        frame_height_ = height;
        frame_width_ = width;
        frame_component_count_ = static_cast<int32_t>(component_count);

        component_ids_.clear();
        for (size_t i{}; i != static_cast<size_t>(component_count); ++i)
        {
            const auto* component{segment_.data() + 6 + i * 3};
            const auto component_id{to_byte(component[0])};
            if (std::find(component_ids_.cbegin(), component_ids_.cend(), component_id) != component_ids_.cend())
                throw_error(jpegls_errc::duplicate_component_id_in_sof_segment);
            component_ids_.push_back(component_id);

            // Only 1x1 sampling factors are supported by JPEG-LS (no subsampled scans).
            if (to_byte(component[1]) != 0x11)
                throw_error(jpegls_errc::parameter_value_not_supported);
        }

        seen_start_of_frame_ = true;
    }

    void process_start_of_scan()
    {
        ASSERT(!segment_.empty());

        const auto scan_component_count{static_cast<int32_t>(to_byte(segment_[0]))};
        if (scan_component_count < 1 || scan_component_count > maximum_component_count_in_scan ||
            scan_component_count > frame_component_count_)
            throw_error(jpegls_errc::invalid_parameter_component_count_sos);

        if (segment_.size() != static_cast<size_t>(scan_component_count) * 2 + 4)
            throw_error(jpegls_errc::invalid_marker_segment_size);

        for (int32_t i{}; i != scan_component_count; ++i)
        {
            const auto component_id{to_byte(segment_[1 + static_cast<size_t>(i) * 2])};
            if (std::find(component_ids_.cbegin(), component_ids_.cend(), component_id) == component_ids_.cend())
                throw_error(jpegls_errc::unknown_component_id);

            // Mapping tables are not supported by the incremental decoder.
            if (to_byte(segment_[2 + static_cast<size_t>(i) * 2]) != 0)
                throw_error(jpegls_errc::parameter_value_not_supported);
        }

        const auto near_lossless{
            static_cast<int32_t>(to_byte(segment_[1 + static_cast<size_t>(scan_component_count) * 2]))};
        const auto signaled_maximum_sample_value{has_preset_parameters_ && preset_parameters_.maximum_sample_value != 0
                                                     ? preset_parameters_.maximum_sample_value
                                                     : calculate_maximum_bit_sample_value(bits_per_sample_)};
        if (near_lossless > compute_maximum_near_lossless(signaled_maximum_sample_value))
            throw_error(jpegls_errc::invalid_parameter_near_lossless);

        const auto interleave_value{
            static_cast<int32_t>(to_byte(segment_[2 + static_cast<size_t>(scan_component_count) * 2]))};
        if (interleave_value < static_cast<int32_t>(interleave_mode::none) ||
            interleave_value > static_cast<int32_t>(interleave_mode::sample))
            throw_error(jpegls_errc::invalid_parameter_interleave_mode);
        const auto interleave_mode_value{static_cast<interleave_mode>(interleave_value)};
        if (scan_component_count == 1 && interleave_mode_value != interleave_mode::none)
            throw_error(jpegls_errc::invalid_parameter_interleave_mode);

        // Point transform is not supported.
        if ((to_byte(segment_[3 + static_cast<size_t>(scan_component_count) * 2]) & 0x0FU) != 0)
            throw_error(jpegls_errc::parameter_value_not_supported);

        if (frame_width_ == 0)
            throw_error(jpegls_errc::invalid_parameter_width);

        // A frame height of 0 requires DNL, which is not supported.
        if (frame_height_ == 0)
            throw_error(jpegls_errc::parameter_value_not_supported);

        // Only a single scan that contains all frame components is supported.
        if (scan_component_count != frame_component_count_ || frame_component_count_ > maximum_component_count_in_scan)
            throw_error(jpegls_errc::parameter_value_not_supported);

        // Multi-component planar scans are not supported.
        if (interleave_mode_value == interleave_mode::none && scan_component_count > 1)
            throw_error(jpegls_errc::parameter_value_not_supported);

        jpegls_pc_parameters validated_parameters{};
        if (!is_valid(preset_parameters_, calculate_maximum_bit_sample_value(bits_per_sample_), near_lossless,
                      &validated_parameters))
            throw_error(jpegls_errc::invalid_parameter_jpegls_preset_parameters);

        near_lossless_ = near_lossless;
        interleave_ = interleave_mode_value;
        maximum_sample_value_ = validated_parameters.maximum_sample_value;
        threshold1_ = validated_parameters.threshold1;
        threshold2_ = validated_parameters.threshold2;
        threshold3_ = validated_parameters.threshold3;
        reset_threshold_ = validated_parameters.reset_value;

        setup_scan_decoder();
        scan_started_ = true;
    }

    void process_preset_parameters()
    {
        switch (to_byte(segment_[0]))
        {
        case 0x1: // Preset coding parameters, ISO/IEC 14495-1, C.2.4.1.1
            if (body_len_ != 11 || segment_.size() != 11)
                throw_error(jpegls_errc::invalid_marker_segment_size);
            preset_parameters_ = {static_cast<int32_t>(read_be16(segment_.data() + 1)),
                                  static_cast<int32_t>(read_be16(segment_.data() + 3)),
                                  static_cast<int32_t>(read_be16(segment_.data() + 5)),
                                  static_cast<int32_t>(read_be16(segment_.data() + 7)),
                                  static_cast<int32_t>(read_be16(segment_.data() + 9))};
            has_preset_parameters_ = true;
            return;

        case 0x2: // Mapping table specification: not supported.
        case 0x3: // Mapping table continuation: not supported.
            throw_error(jpegls_errc::parameter_value_not_supported);

        case 0x4: // Oversize image dimensions: not supported.
            throw_error(jpegls_errc::parameter_value_not_supported);

        default:
            if (to_byte(segment_[0]) <= 0x0D)
                throw_error(jpegls_errc::jpegls_preset_extended_parameter_type_not_supported);
            throw_error(jpegls_errc::invalid_jpegls_preset_parameter_type);
        }
    }

    void process_restart_interval()
    {
        switch (body_len_)
        {
        case 2:
            restart_interval_ = read_be16(segment_.data());
            return;
        case 3:
            restart_interval_ = (static_cast<uint32_t>(to_byte(segment_[0])) << 16U) | read_be16(segment_.data() + 1);
            return;
        default:
            ASSERT(body_len_ == 4);
            restart_interval_ = read_be32(segment_.data());
            return;
        }
    }

    void process_application_data8()
    {
        if (body_len_ == 5)
        {
            // HP color transform segment: "mrfx" tag followed by the transformation (not part of JPEG-LS).
            constexpr std::array<uint8_t, 4> tag{'m', 'r', 'f', 'x'};
            if (std::equal(tag.cbegin(), tag.cend(), segment_.cbegin(),
                           [](uint8_t expected, std::byte actual) { return expected == to_byte(actual); }))
            {
                switch (to_byte(segment_[4]))
                {
                case 0: // none
                    break;
                case 1:
                case 2:
                case 3:
                case 4:
                case 5:
                    throw_error(jpegls_errc::color_transform_not_supported);
                default:
                    throw_error(jpegls_errc::invalid_parameter_color_transformation);
                }
            }
            skip_remaining_ = 0;
            return;
        }

        if (body_len_ >= 30)
        {
            constexpr std::array<uint8_t, 6> spiff_tag{'S', 'P', 'I', 'F', 'F', 0};
            if (std::equal(spiff_tag.cbegin(), spiff_tag.cend(), segment_.cbegin(),
                           [](uint8_t expected, std::byte actual) { return expected == to_byte(actual); }) &&
                to_byte(segment_[6]) <= spiff_major_revision_number)
            {
                // A SPIFF header is present; the following APP8 segments form the SPIFF directory.
                // Only directory recognition and termination are kept, the payload itself is not retained.
                in_spiff_directory_ = true;
            }
        }

        skip_remaining_ = body_len_ - segment_.size();
    }

    void process_spiff_entry()
    {
        ASSERT(segment_.size() == 4);

        if (read_be32(segment_.data()) == spiff_end_of_directory_entry_type)
        {
            if (body_len_ != 6)
                throw_error(jpegls_errc::invalid_marker_segment_size);
            in_spiff_directory_ = false;
        }

        skip_remaining_ = body_len_ - segment_.size();
    }

    // ------------------------------------------------------------------ scan setup

    void setup_scan_decoder()
    {
        components_ = static_cast<size_t>(frame_component_count_);
        samples_per_pixel_ = interleave_ == interleave_mode::sample ? components_ : 1U;
        line_count_ = interleave_ == interleave_mode::line ? components_ : 1U;
        pixel_stride_ = pixel_count_to_pixel_stride(frame_width_);
        output_component_count_ = interleave_ == interleave_mode::none ? 1U : components_;
        bytes_per_sample_ = bit_to_byte_count(bits_per_sample_);

        line_storage_.assign(2 * line_count_ * pixel_stride_ * samples_per_pixel_, 0);
        row_buffer_.resize(static_cast<size_t>(frame_width_) * output_component_count_ * bytes_per_sample_);

        traits_ = std::make_unique<default_traits<int32_t, int32_t>>(maximum_sample_value_, near_lossless_);
        quantization_ = initialize_quantization_lut(*traits_, threshold1_, threshold2_, threshold3_, quantization_lut_);
        initialize_parameters(traits_->range);

        row_ = 0;
        component_ = 0;
        index_ = 1;
        run_phase_ = run_phase::idle;
        rows_since_restart_ = 0;
        restart_marker_counter_ = 0;
        restart_pending_ = false;
        restart_interval_rows_ = restart_interval_ == 0 ? frame_height_ : restart_interval_;
        restart_state_ = restart_parse_state::start_byte;
        eoi_state_ = eoi_parse_state::drain_padding;
    }

    void initialize_parameters(const int32_t range)
    {
        const regular_mode_context initial_value{range};
        for (auto& context : regular_mode_contexts_)
        {
            context = initial_value;
        }

        run_mode_contexts_[0] = run_mode_context{0, range};
        run_mode_contexts_[1] = run_mode_context{1, range};
        run_indexes_.fill(0);
    }

    void reset_after_restart()
    {
        std::fill(line_storage_.begin(), line_storage_.end(), 0);
        initialize_parameters(traits_->range);
        cache_ = 0;
        valid_bits_ = 0;
        component_ = 0;
        index_ = 1;
        run_phase_ = run_phase::idle;
    }

    // ------------------------------------------------------------------ incremental bit reader

    enum class fill_status
    {
        data,
        full,
        marker,
        need_input,
        end_of_input
    };

    void append_byte(const std::byte value) noexcept
    {
        cache_ |= static_cast<std::uint64_t>(to_byte(value)) << (max_readable_cache_bits - valid_bits_);
        valid_bits_ += 8;
    }

    fill_status fill_one()
    {
        if (!has_byte())
            return final_ ? fill_status::end_of_input : fill_status::need_input;

        if (const auto value{to_byte(peek_input_byte())}; value != to_byte(jpeg_marker_start_byte))
        {
            take_input_byte();
            append_byte(std::byte{value});
            return fill_status::data;
        }

        // A 0xFF byte is only entropy data when the next byte has a zero most significant bit.
        if (input_pos_ + 1 >= input_size_)
            return final_ ? fill_status::marker : fill_status::need_input;
        if ((to_byte(input_[input_pos_ + 1]) & 0x80U) != 0)
            return fill_status::marker;

        take_input_byte();
        append_byte(jpeg_marker_start_byte);
        // ISO/IEC 14495-1, A.1: a single zero bit is inserted after 0xFF and is not data.
        --valid_bits_;
        return fill_status::data;
    }

    fill_status fill_read_cache()
    {
        while (valid_bits_ < max_readable_cache_bits)
        {
            const auto status{fill_one()};
            if (status != fill_status::data)
                return status;
        }
        return fill_status::full;
    }

    void handle_fill_status(const fill_status status)
    {
        if (status == fill_status::need_input)
            throw need_input_signal{};
        if (status == fill_status::end_of_input)
            throw_error(jpegls_errc::need_more_data);
    }

    void skip_bits(const int32_t bit_count) noexcept
    {
        ASSERT(bit_count != 0);
        valid_bits_ -= bit_count;
        cache_ = cache_ << bit_count;
    }

    int32_t read_bit()
    {
        if (valid_bits_ <= 0)
        {
            handle_fill_status(fill_read_cache());
            if (valid_bits_ <= 0)
                throw_error(jpegls_errc::invalid_data);
        }

        const auto bit{static_cast<int32_t>(cache_ >> (cache_t_bit_count - 1))};
        skip_bits(1);
        return bit;
    }

    int32_t peek_0_bits()
    {
        if (valid_bits_ < 16)
        {
            handle_fill_status(fill_read_cache());
            if (valid_bits_ <= 0)
                throw_error(jpegls_errc::invalid_data);
        }

        const auto count{countl_zero(cache_)};
        return count < 16 ? count : -1;
    }

    size_t peek_byte_forward()
    {
        if (valid_bits_ < 8)
        {
            handle_fill_status(fill_read_cache());
            if (valid_bits_ <= 0)
                throw_error(jpegls_errc::invalid_data);
        }

        return static_cast<size_t>(cache_ >> max_readable_cache_bits);
    }

    int32_t read_value(const int32_t bit_count)
    {
        ASSERT(0 < bit_count && bit_count < 32);

        if (valid_bits_ < bit_count)
        {
            handle_fill_status(fill_read_cache());
            if (valid_bits_ < bit_count)
                throw_error(jpegls_errc::invalid_data);
        }

        const auto result{static_cast<int32_t>(cache_ >> (cache_t_bit_count - bit_count))};
        skip_bits(bit_count);
        return result;
    }

    int32_t read_unary_code()
    {
        if (const int32_t count{peek_0_bits()}; count >= 0)
        {
            skip_bits(count + 1);
            return count;
        }

        skip_bits(15);
        for (int32_t zero_bit_count{15};; ++zero_bit_count)
        {
            if (read_bit())
                return zero_bit_count;
        }
    }

    int32_t decode_mapped_error_value(const int32_t k, const int32_t limit)
    {
        const auto quantized_bits_per_sample{traits_->quantized_bits_per_sample};
        if (const int32_t unary_code{read_unary_code()}; unary_code < limit - quantized_bits_per_sample - 1)
        {
            return k == 0 ? unary_code : (unary_code << k) + read_value(k);
        }

        return read_value(quantized_bits_per_sample) + 1;
    }

    struct reader_snapshot final
    {
        std::uint64_t cache;
        int32_t valid_bits;
        size_t input_pos;
    };

    [[nodiscard]]
    reader_snapshot capture_reader() const noexcept
    {
        return {cache_, valid_bits_, input_pos_};
    }

    void restore_reader(const reader_snapshot& snapshot) noexcept
    {
        cache_ = snapshot.cache;
        valid_bits_ = snapshot.valid_bits;
        input_pos_ = snapshot.input_pos;
    }

    // ------------------------------------------------------------------ JPEG-LS entropy decoding

    [[nodiscard]]
    int32_t quantize_gradient(const int32_t di) const noexcept
    {
        return *(quantization_ + di);
    }

    [[nodiscard]]
    int32_t decode_regular(const int32_t qs, const int32_t predicted)
    {
        const int32_t sign_value{bit_wise_sign(qs)};
        regular_mode_context& context{regular_mode_contexts_[apply_sign_for_index(qs, sign_value)]};
        const int32_t corrected_prediction{
            traits_->correct_prediction(predicted + apply_sign(context.c(), sign_value))};
        const int32_t k{context.compute_golomb_coding_parameter()};

        int32_t error_value;
        if (const golomb_code_match code{golomb_lut[static_cast<size_t>(k)].get(peek_byte_forward())};
            code.bit_count != 0)
        {
            skip_bits(code.bit_count);
            error_value = code.error_value;
        }
        else
        {
            error_value = unmap_error_value(decode_mapped_error_value(k, traits_->limit));
            if (std::abs(error_value) > 65535)
                throw_error(jpegls_errc::invalid_data);
        }

        if (k == 0)
        {
            error_value = error_value ^ context.get_error_correction(traits_->near_lossless);
        }

        context.update_variables_and_bias(error_value, traits_->near_lossless, reset_threshold_);
        error_value = apply_sign(error_value, sign_value);
        return traits_->compute_reconstructed_sample(corrected_prediction, error_value);
    }

    [[nodiscard]]
    int32_t decode_run_interruption_error(run_mode_context& context, const uint32_t run_index)
    {
        const int32_t k{context.compute_golomb_coding_parameter_checked()};
        const int32_t e_mapped_error_value{decode_mapped_error_value(k, traits_->limit - j[run_index] - 1)};
        const int32_t error_value{
            context.compute_error_value(e_mapped_error_value + context.run_interruption_type(), k)};
        context.update_variables(error_value, e_mapped_error_value, reset_threshold_);
        return error_value;
    }

    [[nodiscard]]
    int32_t decode_run_interruption_pixel(const int32_t ra, const int32_t rb, const uint32_t run_index)
    {
        if (std::abs(ra - rb) <= traits_->near_lossless)
        {
            const int32_t error_value{decode_run_interruption_error(run_mode_contexts_[1], run_index)};
            return traits_->compute_reconstructed_sample(ra, error_value);
        }

        const int32_t error_value{decode_run_interruption_error(run_mode_contexts_[0], run_index)};
        return traits_->compute_reconstructed_sample(rb, error_value * sign(rb - ra));
    }

    [[nodiscard]]
    int32_t decode_run_interruption_component(const int32_t ra, const int32_t rb, const uint32_t run_index)
    {
        const int32_t error_value{decode_run_interruption_error(run_mode_contexts_[0], run_index)};
        return traits_->compute_reconstructed_sample(rb, error_value * sign(rb - ra));
    }

    static void increment_run_index(uint32_t& run_index) noexcept
    {
        if (run_index < 31U)
            ++run_index;
    }

    static void decrement_run_index(uint32_t& run_index) noexcept
    {
        if (run_index > 0)
            --run_index;
    }

    // ------------------------------------------------------------------ line buffers

    [[nodiscard]]
    int32_t* row_base(const size_t which) noexcept
    {
        return line_storage_.data() + which * row_set_sample_count();
    }

    [[nodiscard]]
    size_t row_set_sample_count() const noexcept
    {
        return line_count_ * pixel_stride_ * samples_per_pixel_;
    }

    [[nodiscard]]
    int32_t* previous_row_base() noexcept
    {
        return row_base((row_ & 1U) == 0 ? 0U : 1U);
    }

    [[nodiscard]]
    int32_t* current_row_base() noexcept
    {
        return row_base((row_ & 1U) == 0 ? 1U : 0U);
    }

    void initialize_scalar_edge(int32_t* previous, int32_t* current) const noexcept
    {
        previous[frame_width_ + 1] = previous[frame_width_];
        current[0] = previous[1];
    }

    void initialize_block_edge(int32_t* previous, int32_t* current) const noexcept
    {
        const auto count{static_cast<size_t>(components_)};
        const auto width{static_cast<size_t>(frame_width_)};
        std::copy(previous + width * count, previous + (width + 1) * count, previous + (width + 1) * count);
        std::copy(previous + count, previous + 2 * count, current);
    }

    void fill_run_pixels(int32_t* base, const size_t pixel_offset, const size_t pixel_count) const noexcept
    {
        const auto sample_count{static_cast<size_t>(samples_per_pixel_)};
        for (size_t p{}; p != pixel_count; ++p)
        {
            std::copy(run_ra_.begin(), run_ra_.begin() + sample_count, base + (pixel_offset + p) * sample_count);
        }
    }

    // ------------------------------------------------------------------ run mode state machine

    enum class run_phase
    {
        idle,
        bits,
        interruption
    };

    void begin_run(const int32_t* ra, const size_t sample_count, const size_t total)
    {
        std::copy(ra, ra + sample_count, run_ra_.begin());
        run_total_ = total;
        run_done_ = 0;
        interruption_component_ = 0;
        run_phase_ = run_phase::bits;
    }

    // Decodes (or resumes) the run-length bits of the current run. Returns true when the run covers the
    // remainder of the line (no run interruption sample).
    void advance_run_bits(int32_t* target, uint32_t& run_index)
    {
        while (run_phase_ == run_phase::bits)
        {
            const reader_snapshot snapshot{capture_reader()};
            int32_t bit;
            try
            {
                bit = read_bit();
            }
            catch (const need_input_signal&)
            {
                restore_reader(snapshot);
                throw;
            }

            if (bit != 0)
            {
                const auto chunk{size_t{1} << j[run_index]};
                const auto count{std::min(chunk, run_total_ - run_done_)};
                fill_run_pixels(target, run_done_, count);
                run_done_ += count;

                if (count == chunk)
                    increment_run_index(run_index);

                if (run_done_ == run_total_)
                {
                    run_phase_ = run_phase::idle;
                    return;
                }
                continue;
            }

            size_t extra{};
            if (const int32_t bit_count{j[run_index]}; bit_count > 0)
            {
                try
                {
                    extra = static_cast<size_t>(read_value(bit_count));
                }
                catch (const need_input_signal&)
                {
                    // The zero bit and the remainder form one coding action: retry both from the savepoint.
                    restore_reader(snapshot);
                    throw;
                }
            }

            if (run_done_ + extra > run_total_)
                throw_error(jpegls_errc::invalid_data);

            fill_run_pixels(target, run_done_, extra);
            run_done_ += extra;

            if (run_done_ == run_total_)
            {
                run_phase_ = run_phase::idle;
                return;
            }

            run_phase_ = run_phase::interruption;
            interruption_component_ = 0;
            return;
        }
    }

    // ------------------------------------------------------------------ unit decoding

    void decode_scalar_unit(int32_t* previous, int32_t* current, uint32_t& run_index, size_t& index)
    {
        if (run_phase_ != run_phase::idle)
        {
            if (run_phase_ == run_phase::bits)
            {
                advance_run_bits(current + index, run_index);
                if (run_phase_ == run_phase::idle)
                {
                    index = frame_width_ + 1;
                    return;
                }
            }

            const auto end_index{static_cast<size_t>(index + run_done_)};
            const reader_snapshot snapshot{capture_reader()};
            int32_t value;
            try
            {
                value = decode_run_interruption_pixel(run_ra_[0], previous[end_index], run_index);
            }
            catch (const need_input_signal&)
            {
                restore_reader(snapshot);
                throw;
            }

            current[end_index] = value;
            decrement_run_index(run_index);
            index = end_index + 1;
            run_phase_ = run_phase::idle;
            return;
        }

        const int32_t ra{current[index - 1]};
        const int32_t rc{previous[index - 1]};
        const int32_t rb{previous[index]};
        const int32_t rd{previous[index + 1]};

        const int32_t qs{compute_context_id(quantize_gradient(rd - rb), quantize_gradient(rb - rc),
                                            quantize_gradient(rc - ra))};

        if (qs != 0)
        {
            const reader_snapshot snapshot{capture_reader()};
            int32_t value;
            try
            {
                value = decode_regular(qs, compute_predicted_value(ra, rb, rc));
            }
            catch (const need_input_signal&)
            {
                restore_reader(snapshot);
                throw;
            }

            current[index] = value;
            ++index;
            return;
        }

        begin_run(&ra, 1, frame_width_ - (index - 1));
        advance_run_bits(current + index, run_index);
        if (run_phase_ == run_phase::idle)
        {
            index = frame_width_ + 1;
            return;
        }

        const auto end_index{static_cast<size_t>(index + run_done_)};
        const reader_snapshot snapshot{capture_reader()};
        int32_t value;
        try
        {
            value = decode_run_interruption_pixel(run_ra_[0], previous[end_index], run_index);
        }
        catch (const need_input_signal&)
        {
            restore_reader(snapshot);
            throw;
        }

        current[end_index] = value;
        decrement_run_index(run_index);
        index = end_index + 1;
        run_phase_ = run_phase::idle;
    }

    struct context_snapshot final
    {
        size_t index;
        regular_mode_context context;
    };

    void decode_block_unit(int32_t* previous, int32_t* current, uint32_t& run_index, size_t& index)
    {
        const auto component_count{static_cast<size_t>(components_)};

        if (run_phase_ != run_phase::idle)
        {
            if (run_phase_ == run_phase::bits)
            {
                advance_run_bits(current + index * component_count, run_index);
                if (run_phase_ == run_phase::idle)
                {
                    index = frame_width_ + 1;
                    return;
                }
            }

            const auto end_index{static_cast<size_t>(index + run_done_)};
            while (interruption_component_ < component_count)
            {
                const size_t c{interruption_component_};
                const reader_snapshot snapshot{capture_reader()};
                const auto saved_context{run_mode_contexts_[0]};

                int32_t value;
                try
                {
                    value = decode_run_interruption_component(run_ra_[c], previous[end_index * component_count + c],
                                                              run_index);
                }
                catch (const need_input_signal&)
                {
                    run_mode_contexts_[0] = saved_context;
                    restore_reader(snapshot);
                    throw;
                }

                current[end_index * component_count + c] = value;
                ++interruption_component_;
            }

            decrement_run_index(run_index);
            index = end_index + 1;
            run_phase_ = run_phase::idle;
            return;
        }

        int32_t qs[4]{};
        bool all_zero{true};
        for (size_t c{}; c != component_count; ++c)
        {
            const int32_t ra{current[(index - 1) * component_count + c]};
            const int32_t rc{previous[(index - 1) * component_count + c]};
            const int32_t rb{previous[index * component_count + c]};
            const int32_t rd{previous[(index + 1) * component_count + c]};
            qs[c] = compute_context_id(quantize_gradient(rd - rb), quantize_gradient(rb - rc), quantize_gradient(rc - ra));
            all_zero = all_zero && qs[c] == 0;
        }

        if (!all_zero)
        {
            std::array<context_snapshot, 4> contexts{};
            size_t context_count{};
            const reader_snapshot reader{capture_reader()};

            for (size_t c{}; c != component_count; ++c)
            {
                const size_t context_index{apply_sign_for_index(qs[c], bit_wise_sign(qs[c]))};
                const auto existing{
                    std::find_if(contexts.begin(), contexts.begin() + static_cast<ptrdiff_t>(context_count),
                                 [context_index](const context_snapshot& item) { return item.index == context_index; })};
                if (existing == contexts.begin() + static_cast<ptrdiff_t>(context_count))
                {
                    contexts[context_count++] = {context_index, regular_mode_contexts_[context_index]};
                }
            }

            int32_t values[4]{};
            try
            {
                for (size_t c{}; c != component_count; ++c)
                {
                    const int32_t ra{current[(index - 1) * component_count + c]};
                    const int32_t rc{previous[(index - 1) * component_count + c]};
                    const int32_t rb{previous[index * component_count + c]};
                    values[c] = decode_regular(qs[c], compute_predicted_value(ra, rb, rc));
                }
            }
            catch (const need_input_signal&)
            {
                for (size_t i{}; i != context_count; ++i)
                {
                    regular_mode_contexts_[contexts[i].index] = contexts[i].context;
                }
                restore_reader(reader);
                throw;
            }

            for (size_t c{}; c != component_count; ++c)
            {
                current[index * component_count + c] = values[c];
            }
            ++index;
            return;
        }

        int32_t ra_block[4]{};
        std::copy(current + (index - 1) * component_count, current + index * component_count, ra_block);
        begin_run(ra_block, component_count, frame_width_ - (index - 1));
        advance_run_bits(current + index * component_count, run_index);
        if (run_phase_ == run_phase::idle)
        {
            index = frame_width_ + 1;
            return;
        }

        const auto end_index{static_cast<size_t>(index + run_done_)};
        while (interruption_component_ < component_count)
        {
            const size_t c{interruption_component_};
            const reader_snapshot snapshot{capture_reader()};
            const auto saved_context{run_mode_contexts_[0]};

            int32_t value;
            try
            {
                value = decode_run_interruption_component(run_ra_[c], previous[end_index * component_count + c],
                                                          run_index);
            }
            catch (const need_input_signal&)
            {
                run_mode_contexts_[0] = saved_context;
                restore_reader(snapshot);
                throw;
            }

            current[end_index * component_count + c] = value;
            ++interruption_component_;
        }

        decrement_run_index(run_index);
        index = end_index + 1;
        run_phase_ = run_phase::idle;
    }

    void decode_until_row_ready()
    {
        if (interleave_ == interleave_mode::line)
        {
            while (component_ < components_)
            {
                int32_t* previous{previous_row_base() + component_ * pixel_stride_};
                int32_t* current{current_row_base() + component_ * pixel_stride_};

                if (index_ == 1)
                    initialize_scalar_edge(previous, current);

                while (index_ <= frame_width_)
                {
                    decode_scalar_unit(previous, current, run_indexes_[component_], index_);
                }

                ++component_;
                index_ = 1;
            }

            finish_row();
            return;
        }

        int32_t* previous{previous_row_base()};
        int32_t* current{current_row_base()};

        if (index_ == 1)
        {
            if (interleave_ == interleave_mode::sample)
                initialize_block_edge(previous, current);
            else
                initialize_scalar_edge(previous, current);
        }

        while (index_ <= frame_width_)
        {
            if (interleave_ == interleave_mode::sample)
                decode_block_unit(previous, current, run_indexes_[0], index_);
            else
                decode_scalar_unit(previous, current, run_indexes_[0], index_);
        }

        finish_row();
    }

    void write_row_sample(const size_t output_index, const int32_t value) noexcept
    {
        const size_t offset{output_index * bytes_per_sample_};
        if (bytes_per_sample_ == 1)
        {
            row_buffer_[offset] = static_cast<std::byte>(static_cast<uint8_t>(value));
        }
        else
        {
            const auto sample{static_cast<uint16_t>(value)};
            std::memcpy(row_buffer_.data() + offset, &sample, sizeof(sample));
        }
    }

    void finish_row()
    {
        if (interleave_ == interleave_mode::sample)
        {
            const auto component_count{static_cast<size_t>(components_)};
            for (size_t x{}; x != frame_width_; ++x)
            {
                for (size_t c{}; c != component_count; ++c)
                {
                    write_row_sample(x * component_count + c, current_row_base()[(x + 1) * component_count + c]);
                }
            }
        }
        else if (interleave_ == interleave_mode::line)
        {
            for (size_t x{}; x != frame_width_; ++x)
            {
                for (size_t c{}; c != output_component_count_; ++c)
                {
                    write_row_sample(x * output_component_count_ + c, current_row_base()[c * pixel_stride_ + x + 1]);
                }
            }
        }
        else
        {
            const int32_t* line{current_row_base()};
            for (size_t x{}; x != frame_width_; ++x)
            {
                write_row_sample(x, line[x + 1]);
            }
        }

        ++row_;
        component_ = 0;
        index_ = 1;
        run_phase_ = run_phase::idle;
        row_valid_ = true;

        ++rows_since_restart_;
        if (row_ < frame_height_ && rows_since_restart_ == restart_interval_rows_)
        {
            restart_pending_ = true;
            rows_since_restart_ = 0;
        }

        if (row_ == frame_height_)
            phase_ = phase::end_of_image;
    }

    // ------------------------------------------------------------------ restart markers

    enum class restart_parse_state
    {
        start_byte,
        marker_code
    };

    void parse_restart_marker()
    {
        while (restart_pending_)
        {
            require_byte(jpegls_errc::need_more_data);

            if (restart_state_ == restart_parse_state::start_byte)
            {
                if (take_input_byte() != jpeg_marker_start_byte)
                    throw_error(jpegls_errc::restart_marker_not_found);
                restart_state_ = restart_parse_state::marker_code;
                continue;
            }

            const auto value{to_byte(take_input_byte())};
            if (value == to_byte(jpeg_marker_start_byte))
                continue; // Skip fill bytes.

            const auto expected{static_cast<uint8_t>(jpeg_restart_marker_base + restart_marker_counter_)};
            if (value != expected)
                throw_error(jpegls_errc::restart_marker_not_found);

            restart_marker_counter_ = (restart_marker_counter_ + 1) % jpeg_restart_marker_range;
            restart_state_ = restart_parse_state::start_byte;
            restart_pending_ = false;
            reset_after_restart();
        }
    }

    // ------------------------------------------------------------------ end of image

    enum class eoi_parse_state
    {
        drain_padding,
        marker_start,
        marker_code
    };

    void parse_end_of_image()
    {
        while (phase_ == phase::end_of_image)
        {
            switch (eoi_state_)
            {
            case eoi_parse_state::drain_padding: {
                const reader_snapshot snapshot{capture_reader()};
                try
                {
                    require_byte(jpegls_errc::need_more_data);

                    // Mirror the whole-frame end_scan() check. The entropy decoder stops at the first byte that
                    // cannot be entropy data; the next byte must be the 0xFF of the marker. The remaining cache
                    // bits (the bit padding of the final byte) must all be zero.
                    if (to_byte(peek_input_byte()) != to_byte(jpeg_marker_start_byte))
                    {
                        // A legacy encoder may insert a zero padding byte before EOI; accept a single zero bit
                        // followed by the marker, exactly like the whole-frame decoder's end_scan().
                        read_bit();
                        require_byte(jpegls_errc::need_more_data);
                        if (to_byte(peek_input_byte()) != to_byte(jpeg_marker_start_byte))
                            throw_error(jpegls_errc::invalid_data);
                    }

                    if (cache_ != 0)
                        throw_error(jpegls_errc::invalid_data);
                }
                catch (const need_input_signal&)
                {
                    restore_reader(snapshot);
                    throw;
                }

                eoi_state_ = eoi_parse_state::marker_start;
                return;
            }

            case eoi_parse_state::marker_start:
                require_byte(jpegls_errc::need_more_data);
                if (take_input_byte() != jpeg_marker_start_byte)
                    throw_error(jpegls_errc::end_of_image_marker_not_found);
                eoi_state_ = eoi_parse_state::marker_code;
                return;

            case eoi_parse_state::marker_code: {
                require_byte(jpegls_errc::need_more_data);
                const auto value{to_byte(take_input_byte())};
                if (value == to_byte(jpeg_marker_start_byte))
                    return; // Skip fill bytes.

                if (value != static_cast<uint8_t>(jpeg_marker_code::end_of_image))
                    throw_error(jpegls_errc::end_of_image_marker_not_found);
                phase_ = phase::done;
                return;
            }
            }
        }
    }

    // ------------------------------------------------------------------ push entry point

    stream_decode_result push(const void* input, const size_t size, const bool final)
    {
        if (failed_)
            throw_error(jpegls_errc::invalid_operation);
        if (size != 0 && input == nullptr)
            throw_error(jpegls_errc::invalid_argument);

        row_valid_ = false;

        if (phase_ == phase::done)
            return {0, stream_decode_status::done};

        input_ = static_cast<const std::byte*>(input);
        input_size_ = size;
        input_pos_ = 0;
        final_ = final;

        try
        {
            for (;;)
            {
                switch (phase_)
                {
                case phase::header:
                    parse_header_until_scan();
                    break;

                case phase::scan:
                    if (restart_pending_)
                    {
                        parse_restart_marker();
                        break;
                    }

                    decode_until_row_ready();
                    return {input_pos_, stream_decode_status::row_ready};

                case phase::end_of_image:
                    parse_end_of_image();
                    if (phase_ == phase::done)
                        return {input_pos_, stream_decode_status::done};
                    break;

                case phase::done:
                    return {0, stream_decode_status::done};
                }
            }
        }
        catch (const need_input_signal&)
        {
            return {input_pos_, stream_decode_status::need_input};
        }
        catch (...)
        {
            failed_ = true;
            throw;
        }
    }

    // ------------------------------------------------------------------ state

    // Input view for the current push call. Bytes are never referenced after the call returns.
    const std::byte* input_{};
    size_t input_size_{};
    size_t input_pos_{};
    bool final_{};
    bool failed_{};

    phase phase_{phase::header};
    bool row_valid_{false};

    header_parse_state header_state_{header_parse_state::soi_ff};
    segment_kind segment_kind_{segment_kind::none};
    uint8_t marker_code_{};
    uint8_t length_high_{};
    size_t body_len_{};
    size_t body_target_{};
    size_t skip_remaining_{};
    std::vector<std::byte> segment_;
    bool in_spiff_directory_{false};
    bool seen_start_of_frame_{false};
    bool scan_started_{false};

    uint32_t frame_width_{};
    uint32_t frame_height_{};
    int32_t bits_per_sample_{};
    int32_t frame_component_count_{};
    std::vector<uint8_t> component_ids_;

    bool has_preset_parameters_{false};
    jpegls_pc_parameters preset_parameters_{};
    int32_t maximum_sample_value_{};
    int32_t threshold1_{};
    int32_t threshold2_{};
    int32_t threshold3_{};
    int32_t reset_threshold_{};
    uint32_t restart_interval_{};

    int32_t near_lossless_{};
    interleave_mode interleave_{interleave_mode::none};

    // Scan decoder state; storage grows with the row width and component count only.
    std::unique_ptr<default_traits<int32_t, int32_t>> traits_;
    std::vector<int8_t> quantization_lut_;
    const int8_t* quantization_{};
    std::array<regular_mode_context, 365> regular_mode_contexts_{};
    std::array<run_mode_context, 2> run_mode_contexts_{};
    std::array<uint32_t, 4> run_indexes_{};
    std::vector<int32_t> line_storage_;
    std::vector<std::byte> row_buffer_;

    size_t components_{};
    size_t samples_per_pixel_{1};
    size_t line_count_{1};
    size_t pixel_stride_{};
    size_t output_component_count_{};
    size_t bytes_per_sample_{1};
    uint32_t row_{};
    size_t component_{};
    size_t index_{1};
    uint32_t rows_since_restart_{};
    uint32_t restart_interval_rows_{};
    uint32_t restart_marker_counter_{};
    bool restart_pending_{false};
    restart_parse_state restart_state_{restart_parse_state::start_byte};

    // Resumable run mode state, bounded by the component count.
    run_phase run_phase_{run_phase::idle};
    size_t run_total_{};
    size_t run_done_{};
    std::array<int32_t, 4> run_ra_{};
    size_t interruption_component_{};

    // Incremental bit cache. Valid bits are stored in the most significant bits of the cache.
    static constexpr int32_t max_readable_cache_bits{56};
    static constexpr int32_t cache_t_bit_count{64};
    std::uint64_t cache_{};
    int32_t valid_bits_{};

    eoi_parse_state eoi_state_{eoi_parse_state::drain_padding};
};


jpegls_stream_decoder::jpegls_stream_decoder() : impl_{std::make_unique<impl>()}
{
}

jpegls_stream_decoder::~jpegls_stream_decoder() = default;
jpegls_stream_decoder::jpegls_stream_decoder(jpegls_stream_decoder&&) noexcept = default;
jpegls_stream_decoder& jpegls_stream_decoder::operator=(jpegls_stream_decoder&&) noexcept = default;

stream_decode_result jpegls_stream_decoder::push(const void* input, const std::size_t size, const bool final)
{
    if (!impl_)
        throw_error(jpegls_errc::invalid_operation);
    return impl_->push(input, size, final);
}

frame_info jpegls_stream_decoder::frame_info() const
{
    if (!impl_ || impl_->failed_)
        throw_error(jpegls_errc::invalid_operation);
    // The frame information is complete once the SOF segment has been read and it signaled a known size.
    check_operation(impl_->seen_start_of_frame_ && impl_->frame_width_ != 0 && impl_->frame_height_ != 0);
    return {impl_->frame_width_, impl_->frame_height_, impl_->bits_per_sample_, impl_->frame_component_count_};
}

std::pair<const std::byte*, std::size_t> jpegls_stream_decoder::row() const
{
    if (!impl_ || impl_->failed_ || !impl_->row_valid_)
        throw_error(jpegls_errc::invalid_operation);
    return {impl_->row_buffer_.data(), impl_->row_buffer_.size()};
}

void jpegls_stream_decoder::reset()
{
    if (!impl_)
        impl_ = std::make_unique<impl>();
    else
        impl_->reset();
}

} // namespace charls
