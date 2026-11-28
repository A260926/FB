// SPDX-FileCopyrightText: © 2026 Team CharLS
// SPDX-License-Identifier: BSD-3-Clause

#include "pch.hpp"

#include "charls/jpegls_stream_decoder.hpp"

#include "charls/validate_spiff_header.h"

#include "coding_parameters.hpp"
#include "constants.hpp"
#include "default_traits.hpp"
#include "jpeg_marker_code.hpp"
#include "jpeg_stream_reader.hpp"
#include "jpegls_algorithm.hpp"
#include "scan_codec.hpp"
#include "span.hpp"
#include "util.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace charls {

using impl::throw_jpegls_error;
using std::array;
using std::byte;
using std::size_t;

namespace {

constexpr size_t header_chunk_size = 512;

// The bit cache is a 64-bit word; the 8 most significant bits are the read window used for peeking.
constexpr int32_t bit_cache_bits = 64;
constexpr int32_t max_readable_cache_bits = bit_cache_bits - 8;

[[nodiscard]]
bool is_length_bearing_marker(const uint8_t marker_code) noexcept
{
    switch (marker_code)
    {
    case 0xDC: // DNL
    case 0xDD: // DRI
    case 0xDA: // SOS
    case 0xF7: // SOF55
    case 0xF8: // LSE
    case 0xFE: // COM
        return true;

    default:
        return marker_code >= 0xE0 && marker_code <= 0xEF; // APPn
    }
}

struct header_layout final
{
    bool complete{};
    size_t end{};
    bool mapping_table{};
    bool oversize_dimensions{};
    bool define_number_of_lines{};
    bool zero_height_sof{};
    bool have_sof{};
    charls::frame_info sof{};
};

// Walks the marker structure up to the end of the first SOS segment. This only determines how many bytes
// belong to the header and whether unsupported structural features are present. All semantic validation is
// still performed by jpeg_stream_reader.
[[nodiscard]]
header_layout scan_header_layout(const std::vector<byte>& buffer) noexcept
{
    header_layout result;

    size_t position{};
    if (position == buffer.size())
        return result;

    if (buffer[position] != jpeg_marker_start_byte)
    {
        result.complete = true;
        result.end = 1;
        return result;
    }

    while (position < buffer.size() && buffer[position] == jpeg_marker_start_byte)
        ++position;
    if (position == buffer.size())
        return result;

    if (std::to_integer<uint8_t>(buffer[position]) != static_cast<uint8_t>(jpeg_marker_code::start_of_image))
    {
        result.complete = true;
        result.end = position + 1;
        return result;
    }
    ++position;

    while (position < buffer.size())
    {
        if (buffer[position] != jpeg_marker_start_byte)
        {
            result.complete = true;
            result.end = position + 1;
            return result;
        }

        size_t code_position{position};
        while (code_position < buffer.size() && buffer[code_position] == jpeg_marker_start_byte)
            ++code_position;
        if (code_position == buffer.size())
            return result;

        const uint8_t marker_code{std::to_integer<uint8_t>(buffer[code_position])};
        if (!is_length_bearing_marker(marker_code))
        {
            result.complete = true;
            result.end = code_position + 1;
            return result;
        }

        if (code_position + 2 >= buffer.size())
            return result;

        const uint16_t segment_size{
            static_cast<uint16_t>((std::to_integer<uint16_t>(buffer[code_position + 1]) << 8U) |
                                  std::to_integer<uint16_t>(buffer[code_position + 2]))};

        if (segment_size < 2)
        {
            result.complete = true;
            result.end = code_position + 3;
            return result;
        }

        const size_t segment_end{code_position + 1 + segment_size};
        if (segment_end > buffer.size())
            return result;

        if (marker_code == static_cast<uint8_t>(jpeg_marker_code::start_of_frame_jpegls) && !result.have_sof)
        {
            const int32_t data_size{static_cast<int32_t>(segment_size) - 2};
            if (data_size >= 6)
            {
                const size_t data_position{code_position + 3};
                const int32_t bits_per_sample{std::to_integer<int32_t>(buffer[data_position])};
                const uint32_t height{static_cast<uint32_t>((std::to_integer<uint16_t>(buffer[data_position + 1]) << 8U) |
                                                            std::to_integer<uint16_t>(buffer[data_position + 2]))};
                const uint32_t width{static_cast<uint32_t>((std::to_integer<uint16_t>(buffer[data_position + 3]) << 8U) |
                                                           std::to_integer<uint16_t>(buffer[data_position + 4]))};
                const int32_t component_count{std::to_integer<int32_t>(buffer[data_position + 5])};

                if (height == 0)
                    result.zero_height_sof = true; // The height is defined later by a DNL segment.

                if (bits_per_sample >= minimum_bits_per_sample && bits_per_sample <= maximum_bits_per_sample &&
                    height >= minimum_height && width >= minimum_width && component_count > 0)
                {
                    result.have_sof = true;
                    result.sof = {width, height, bits_per_sample, component_count};
                }
            }
        }
        else if (marker_code == static_cast<uint8_t>(jpeg_marker_code::jpegls_preset_parameters) && segment_size >= 3)
        {
            switch (std::to_integer<uint8_t>(buffer[code_position + 3]))
            {
            case 0x2: // mapping table specification
            case 0x3: // mapping table continuation
                result.mapping_table = true;
                break;

            case 0x4: // oversize image dimension
                result.oversize_dimensions = true;
                break;

            default:
                break;
            }
        }
        else if (marker_code == static_cast<uint8_t>(jpeg_marker_code::define_number_of_lines))
        {
            result.define_number_of_lines = true;
        }

        if (marker_code == static_cast<uint8_t>(jpeg_marker_code::start_of_scan))
        {
            result.complete = true;
            result.end = segment_end;
            return result;
        }

        position = segment_end;
    }

    return result;
}

[[nodiscard]]
bool is_missing_define_number_of_lines_error(const std::error_code& code) noexcept
{
    return code == make_error_code(jpegls_errc::define_number_of_lines_marker_not_found) ||
           code == make_error_code(jpegls_errc::unexpected_define_number_of_lines_marker);
}

} // namespace


// Incremental JPEG-LS scan decoder. Decodes exactly one complete row per row_ready result.
// State is limited to two line buffers, the adaptive contexts, and a small bit cache, so it does not
// grow with the image height, the compressed length, or the number of delivered rows.
class stream_scan_decoder final : protected scan_codec
{
public:
    enum class step_result
    {
        row_ready,
        need_input,
        done
    };

    stream_scan_decoder(const charls::frame_info& source_frame_info, const jpegls_pc_parameters& preset_parameters,
                        const coding_parameters& parameters) :
        scan_codec{source_frame_info, preset_parameters, parameters},
        traits_{preset_parameters.maximum_sample_value, parameters.near_lossless},
        near_lossless_{parameters.near_lossless},
        component_count_{source_frame_info.component_count},
        interleave_mode_{parameters.interleave_mode},
        restart_interval_{parameters.restart_interval == 0 ? source_frame_info.height : parameters.restart_interval},
        pixel_stride_{static_cast<size_t>(source_frame_info.width) + 2U}
    {
        ASSERT(component_count_ >= 1 && component_count_ <= maximum_component_count_in_scan);
        ASSERT(interleave_mode_ != interleave_mode::none || component_count_ == 1);
        ASSERT(source_frame_info.bits_per_sample > 8 || traits_.maximum_sample_value <= 255);

        line_buffers_.assign(size_t{2} * static_cast<size_t>(component_count_) * pixel_stride_, 0);
        run_indexes_.fill(0);

        const size_t bytes_per_sample{source_frame_info.bits_per_sample > 8 ? 2U : 1U};
        row_bytes_.resize(static_cast<size_t>(source_frame_info.width) * static_cast<size_t>(component_count_) *
                          bytes_per_sample);

        initialize_parameters(traits_.range);
    }

    [[nodiscard]]
    step_result step(const byte*& position, const byte* end, const bool final)
    {
        for (;;)
        {
            switch (phase_)
            {
            case phase::decode:
                if (!row_started_)
                {
                    if (row_ >= frame_info().height)
                    {
                        phase_ = phase::scan_end;
                        continue;
                    }

                    if (restart_pending_)
                    {
                        phase_ = phase::restart;
                        restart_pending_ = false;
                        continue;
                    }

                    start_row();
                }

                return decode_row(position, end, final);

            case phase::restart:
                if (complete_restart_marker(position, end, final) == step_result::need_input)
                    return step_result::need_input;
                phase_ = phase::decode;
                continue;

            case phase::scan_end:
                return complete_end_of_image(position, end, final);

            case phase::done:
                return step_result::done;
            }
        }
    }

    [[nodiscard]]
    const byte* row_data() const noexcept
    {
        return row_bytes_.data();
    }

    [[nodiscard]]
    size_t row_size() const noexcept
    {
        return row_bytes_.size();
    }

private:
    // Reads entropy-coded bits. Bytes are copied into the cache as they are read, which allows the caller to reuse
    // or free the input buffer as soon as push() returns. A 0xFF byte is only consumed once the following byte is
    // known: its MSB tells entropy data (stuffed bit, clear) from a marker start (set). When 0xFF is the last byte
    // of an interval it is accepted and kept pending; no data bits are used until the next byte arrives.
    class bit_reader final
    {
    public:
        [[nodiscard]]
        bool read_bit(const byte*& position, const byte* end, uint32_t& value)
        {
            if (valid_bits_ <= 0 && !fill_to(1, position, end))
                return false;

            value = static_cast<uint32_t>(cache_ >> (bit_cache_bits - 1));
            skip(1);
            return true;
        }

        [[nodiscard]]
        bool read_value(const int32_t count, const byte*& position, const byte* end, int32_t& value)
        {
            ASSERT(count > 0 && count < 32);

            if (!fill_to(count, position, end))
                return false;

            value = static_cast<int32_t>(cache_ >> (bit_cache_bits - count));
            skip(count);
            return true;
        }

        // Discards the cache and any pending-but-undecided 0xFF. Used only when the caller already knows that the
        // next byte starts a marker (or that the scan has ended).
        void reset() noexcept
        {
            cache_ = 0;
            valid_bits_ = 0;
            marker_at_ = false;
            pending_ff_ = false;
            padding_byte_consumed_ = false;
        }

        [[nodiscard]]
        bool marker_at() const noexcept
        {
            return marker_at_;
        }

        [[nodiscard]]
        bool padding_byte_consumed() const noexcept
        {
            return padding_byte_consumed_;
        }

        // True when the 0xFF of a marker start (or a data byte that turned out to be a marker) was consumed but the
        // byte that decides its meaning is still missing.
        [[nodiscard]]
        bool has_nonzero_valid_bits() const noexcept
        {
            return valid_bits_ > 0 && (cache_ >> (bit_cache_bits - valid_bits_)) != 0;
        }

        // Verifies the trailing padding bits and resolves a pending 0xFF that could not be classified (it was the
        // last byte of the entropy data). When this returns true, marker_at() tells whether that 0xFF started a
        // marker (the code byte is next in the input); otherwise position has been advanced past the padding.
        // Throws jpegls_errc::invalid_data when non-zero padding is present.
        [[nodiscard]]
        bool resolve_trailing_padding(const byte*& position, const byte* end)
        {
            if (has_nonzero_valid_bits())
                impl::throw_jpegls_error(jpegls_errc::invalid_data);

            if (!pending_ff_)
                return true;

            if (position == end)
                return false;

            const byte value{*position};
            if ((value & byte{0x80}) == byte{})
            {
                // The 0xFF was entropy data; the next byte is its 7 remaining bits, which are trailing padding.
                if ((value & byte{0x7F}) != byte{})
                    impl::throw_jpegls_error(jpegls_errc::invalid_data);
                ++position;
            }
            else
            {
                marker_at_ = true; // The code byte of the marker is next; it is not part of the entropy data.
            }

            pending_ff_ = false;
            return true;
        }

    private:
        [[nodiscard]]
        bool fill_to(const int32_t target, const byte*& position, const byte* end)
        {
            while (valid_bits_ < target)
            {
                if (pending_ff_)
                {
                    if (position == end)
                        return false;

                    if ((*position & byte{0x80}) != byte{})
                    {
                        marker_at_ = true;
                        return false; // The code byte is not part of the entropy data.
                    }

                    // 0xFF was entropy data: add its 8 bits and skip the stuffed bit of the next byte.
                    cache_ |= static_cast<uint64_t>(0xFF) << (max_readable_cache_bits - valid_bits_);
                    valid_bits_ += 8;
                    --valid_bits_;
                    pending_ff_ = false;
                    continue;
                }

                if (position == end)
                    return false;

                const byte value{*position};
                ++position;
                if (value == jpeg_marker_start_byte)
                {
                    pending_ff_ = true;
                    continue;
                }

                cache_ |= static_cast<uint64_t>(std::to_integer<uint8_t>(value))
                          << (max_readable_cache_bits - valid_bits_);
                valid_bits_ += 8;
            }

            return true;
        }

        void skip(const int32_t count) noexcept
        {
            cache_ <<= count;
            valid_bits_ -= count;
        }

        uint64_t cache_{};
        int32_t valid_bits_{};
        bool marker_at_{};
        bool pending_ff_{};
        bool padding_byte_consumed_{};
    };

    enum class phase
    {
        decode,
        restart,
        scan_end,
        done
    };

    enum class operation
    {
        none,
        regular,
        run_pixels,
        run_remainder,
        run_interruption
    };

    enum class op_phase
    {
        unary,
        remainder,
        escape
    };

    enum class pixel_run_result
    {
        line_complete,
        continue_decoding,
        need_input
    };

    using sample_type = uint16_t;

    [[nodiscard]]
    sample_type& current(const int32_t component, const size_t index) noexcept
    {
        return current_line_[static_cast<size_t>(component) * pixel_stride_ + index];
    }

    [[nodiscard]]
    sample_type& previous(const int32_t component, const size_t index) noexcept
    {
        return previous_line_[static_cast<size_t>(component) * pixel_stride_ + index];
    }

    [[nodiscard]]
    int32_t current_value(const int32_t component, const size_t index) const noexcept
    {
        return current_line_[static_cast<size_t>(component) * pixel_stride_ + index];
    }

    [[nodiscard]]
    int32_t previous_value(const int32_t component, const size_t index) const noexcept
    {
        return previous_line_[static_cast<size_t>(component) * pixel_stride_ + index];
    }

    void start_row() noexcept
    {
        const size_t block_size{static_cast<size_t>(component_count_) * pixel_stride_};
        const size_t previous_block{static_cast<size_t>(row_ & 1U)};
        previous_line_ = line_buffers_.data() + previous_block * block_size;
        current_line_ = line_buffers_.data() + (size_t{1} - previous_block) * block_size;

        row_started_ = true;
        unit_ = 0;
        start_unit();
    }

    void start_unit() noexcept
    {
        index_ = 1;

        if (interleave_mode_ == interleave_mode::sample)
        {
            run_index_ = run_indexes_[0];
            for (int32_t component{}; component < component_count_; ++component)
                initialize_edge_pixels(current_line_pointer(component), previous_line_pointer(component));
        }
        else
        {
            run_index_ = run_indexes_[static_cast<size_t>(unit_)];
            initialize_edge_pixels(current_line_pointer(unit_), previous_line_pointer(unit_));
        }
    }

    [[nodiscard]]
    sample_type* previous_line_pointer(const int32_t component) noexcept
    {
        return previous_line_ + static_cast<size_t>(component) * pixel_stride_;
    }

    [[nodiscard]]
    sample_type* current_line_pointer(const int32_t component) noexcept
    {
        return current_line_ + static_cast<size_t>(component) * pixel_stride_;
    }

    void initialize_edge_pixels(sample_type* current_line, sample_type* previous_line) const noexcept
    {
        previous_line[width_ + 1] = previous_line[width_];
        current_line[0] = previous_line[1];
    }

    [[nodiscard]]
    int32_t quantize_gradient(const int32_t di) const noexcept
    {
        return quantize_gradient_org(di, near_lossless_);
    }

    [[nodiscard]]
    int32_t compute_gradient_context(const int32_t component, const size_t index) const noexcept
    {
        const int32_t ra{current_value(component, index - 1)};
        const int32_t rc{previous_value(component, index - 1)};
        const int32_t rb{previous_value(component, index)};
        const int32_t rd{previous_value(component, index + 1)};

        return compute_context_id(quantize_gradient(rd - rb), quantize_gradient(rb - rc), quantize_gradient(rc - ra));
    }

    [[nodiscard]]
    int32_t compute_prediction(const int32_t component, const size_t index) const noexcept
    {
        return compute_predicted_value(current_value(component, index - 1), previous_value(component, index),
                                       previous_value(component, index - 1));
    }

    void start_regular(const int32_t component, const size_t index)
    {
        const int32_t qs{compute_gradient_context(component, index)};
        const int32_t prediction{compute_prediction(component, index)};

        operation_ = operation::regular;
        op_component_ = component;
        op_sign_ = bit_wise_sign(qs);
        op_context_ = apply_sign_for_index(qs, op_sign_);
        op_corrected_prediction_ =
            traits_.correct_prediction(prediction + apply_sign(regular_mode_contexts_[op_context_].c(), op_sign_));
        op_k_ = regular_mode_contexts_[op_context_].compute_golomb_coding_parameter();
        op_phase_ = op_phase::unary;
        op_unary_ = 0;
    }

    void start_run(const int32_t run_component_count, const int32_t first_component, const size_t start_index) noexcept
    {
        operation_ = operation::run_pixels;
        op_run_component_count_ = run_component_count;
        if (run_component_count == 1)
        {
            op_components_[0] = first_component;
        }
        else
        {
            for (int32_t component{}; component < run_component_count; ++component)
                op_components_[component] = component;
        }

        op_run_start_ = start_index;
        op_run_end_ = start_index;
        for (int32_t i{}; i < run_component_count; ++i)
            op_run_value_[i] = current(op_components_[i], start_index - 1);
    }

    void fill_run(const size_t from, const size_t count) noexcept
    {
        for (int32_t i{}; i < op_run_component_count_; ++i)
        {
            for (size_t index{}; index < count; ++index)
                current(op_components_[i], from + index) = op_run_value_[i];
        }
    }

    // Decodes the run-mode pixel run. On line_complete the run covered the remainder of the line.
    // On continue_decoding a run interruption pixel was started and needs to be decoded.
    [[nodiscard]]
    pixel_run_result complete_pixel_run(const byte*& position, const byte* end, const bool final)
    {
        while (operation_ == operation::run_pixels)
        {
            uint32_t bit{};
            if (!bit_reader_.read_bit(position, end, bit))
            {
                handle_read_failure(position, end, final);
                return pixel_run_result::need_input;
            }

            const size_t run_shift{size_t{1} << j[run_index_]};
            const size_t pixels_in_line{static_cast<size_t>(width_) - (op_run_start_ - 1)};
            const size_t already{op_run_end_ - op_run_start_};

            if (bit != 0)
            {
                const size_t count{std::min(run_shift, pixels_in_line - already)};
                fill_run(op_run_end_, count);
                op_run_end_ += count;

                if (count == run_shift)
                    increment_run_index();

                if (op_run_end_ == static_cast<size_t>(width_) + 1U)
                    return pixel_run_result::line_complete;
                continue;
            }

            if (j[run_index_] > 0)
            {
                operation_ = operation::run_remainder;
                op_remainder_bits_ = j[run_index_];
                break;
            }

            begin_run_interruption();
            return pixel_run_result::continue_decoding;
        }

        if (operation_ == operation::run_remainder)
        {
            int32_t value{};
            if (!bit_reader_.read_value(op_remainder_bits_, position, end, value))
            {
                handle_read_failure(position, end, final);
                return pixel_run_result::need_input;
            }

            const size_t old_end{op_run_end_};
            op_run_end_ += static_cast<size_t>(value);
            if (op_run_end_ > static_cast<size_t>(width_) + 1U)
                throw_jpegls_error(jpegls_errc::invalid_data);

            fill_run(old_end, op_run_end_ - old_end);

            if (op_run_end_ == static_cast<size_t>(width_) + 1U)
            {
                operation_ = operation::none;
                return pixel_run_result::line_complete;
            }

            begin_run_interruption();
        }

        return pixel_run_result::continue_decoding;
    }

    void begin_run_interruption()
    {
        operation_ = operation::run_interruption;
        op_interrupt_component_ = 0;
        op_interrupt_end_ = op_run_end_;
        op_k_ = 0;

        for (int32_t i{}; i < op_run_component_count_; ++i)
        {
            const int32_t component{op_components_[i]};
            const int32_t ra{current_value(component, op_interrupt_end_ - 1)};
            const int32_t rb{previous_value(component, op_interrupt_end_)};

            if (interleave_mode_ == interleave_mode::sample)
            {
                op_interrupt_context_[i] = 0;
                op_interrupt_prediction_[i] = rb;
                op_interrupt_multiplier_[i] = sign(rb - ra);
            }
            else if (std::abs(ra - rb) <= near_lossless_)
            {
                op_interrupt_context_[i] = 1;
                op_interrupt_prediction_[i] = ra;
                op_interrupt_multiplier_[i] = 1;
            }
            else
            {
                op_interrupt_context_[i] = 0;
                op_interrupt_prediction_[i] = rb;
                op_interrupt_multiplier_[i] = sign(rb - ra);
            }
        }

        begin_run_interruption_component();
    }

    void begin_run_interruption_component()
    {
        const int32_t context_index{op_interrupt_context_[op_interrupt_component_]};
        run_mode_context& context{run_mode_contexts_[context_index]};
        op_k_ = context.compute_golomb_coding_parameter_checked();
        op_limit_ = traits_.limit - j[run_index_] - 1;
        op_interrupt_context_index_ = context_index;
        op_phase_ = op_phase::unary;
        op_unary_ = 0;
    }

    // Returns true when the complete run interruption pixel was reconstructed.
    [[nodiscard]]
    bool complete_run_interruption(const byte*& position, const byte* end, const bool final)
    {
        while (operation_ != operation::none)
        {
            switch (op_phase_)
            {
            case op_phase::unary: {
                uint32_t bit{};
                if (!bit_reader_.read_bit(position, end, bit))
                {
                    handle_read_failure(position, end, final);
                    return false;
                }

                if (bit == 0)
                {
                    ++op_unary_;
                    continue;
                }

                if (op_unary_ < op_limit_ - traits_.quantized_bits_per_sample - 1)
                {
                    if (op_k_ == 0)
                    {
                        if (finish_run_interruption_component(op_unary_))
                            break;
                        continue;
                    }
                    op_phase_ = op_phase::remainder;
                    continue;
                }

                op_phase_ = op_phase::escape;
                continue;
            }

            case op_phase::remainder: {
                int32_t value{};
                if (!bit_reader_.read_value(op_k_, position, end, value))
                {
                    handle_read_failure(position, end, final);
                    return false;
                }

                if (finish_run_interruption_component((op_unary_ << op_k_) + value))
                    break;
                continue;
            }

            case op_phase::escape: {
                int32_t value{};
                if (!bit_reader_.read_value(traits_.quantized_bits_per_sample, position, end, value))
                {
                    handle_read_failure(position, end, final);
                    return false;
                }

                if (finish_run_interruption_component(value + 1))
                    break;
                continue;
            }
            }

            for (int32_t i{}; i < op_run_component_count_; ++i)
                current(op_components_[i], op_interrupt_end_) = op_interrupt_reconstruction_[i];

            decrement_run_index();
            index_ = op_interrupt_end_ + 1;
            return true;
        }

        return false;
    }

    // Returns true when all components of the run interruption are reconstructed.
    [[nodiscard]]
    bool finish_run_interruption_component(const int32_t mapped_value)
    {
        run_mode_context& context{run_mode_contexts_[op_interrupt_context_index_]};
        const int32_t error_value{context.compute_error_value(mapped_value + context.run_interruption_type(), op_k_)};
        context.update_variables(error_value, mapped_value, reset_threshold_);

        op_interrupt_reconstruction_[op_interrupt_component_] = traits_.compute_reconstructed_sample(
            op_interrupt_prediction_[op_interrupt_component_], error_value * op_interrupt_multiplier_[op_interrupt_component_]);

        ++op_interrupt_component_;
        if (op_interrupt_component_ == op_run_component_count_)
        {
            operation_ = operation::none;
            return true;
        }

        begin_run_interruption_component();
        return false;
    }

    void finish_regular()
    {
        int32_t error_value{unmap_error_value(op_mapped_)};
        if (std::abs(error_value) > 65535)
            throw_jpegls_error(jpegls_errc::invalid_data);

        if (op_k_ == 0)
            error_value = error_value ^ regular_mode_contexts_[op_context_].get_error_correction(near_lossless_);

        regular_mode_contexts_[op_context_].update_variables_and_bias(error_value, near_lossless_, reset_threshold_);
        error_value = apply_sign(error_value, op_sign_);

        current(op_component_, index_) = traits_.compute_reconstructed_sample(op_corrected_prediction_, error_value);
    }

    [[nodiscard]]
    bool complete_regular(const byte*& position, const byte* end, const bool final)
    {
        for (;;)
        {
            switch (op_phase_)
            {
            case op_phase::unary: {
                uint32_t bit{};
                if (!bit_reader_.read_bit(position, end, bit))
                {
                    handle_read_failure(position, end, final);
                    return false;
                }

                if (bit == 0)
                {
                    ++op_unary_;
                    break;
                }

                if (op_unary_ < traits_.limit - traits_.quantized_bits_per_sample - 1)
                {
                    if (op_k_ == 0)
                    {
                        op_mapped_ = op_unary_;
                        finish_regular();
                        return true;
                    }
                    op_phase_ = op_phase::remainder;
                    break;
                }
                op_phase_ = op_phase::escape;
                break;
            }

            case op_phase::remainder: {
                int32_t value{};
                if (!bit_reader_.read_value(op_k_, position, end, value))
                {
                    handle_read_failure(position, end, final);
                    return false;
                }
                op_mapped_ = (op_unary_ << op_k_) + value;
                finish_regular();
                return true;
            }

            case op_phase::escape: {
                int32_t value{};
                if (!bit_reader_.read_value(traits_.quantized_bits_per_sample, position, end, value))
                {
                    handle_read_failure(position, end, final);
                    return false;
                }
                op_mapped_ = value + 1;
                finish_regular();
                return true;
            }
            }
        }
    }

    [[nodiscard]]
    bool finish_unit()
    {
        const bool single_unit_mode{interleave_mode_ != interleave_mode::line || component_count_ == 1};
        if (single_unit_mode)
        {
            run_indexes_[0] = run_index_;
            finish_row();
            return true;
        }

        run_indexes_[static_cast<size_t>(unit_)] = run_index_;
        ++unit_;
        if (unit_ < component_count_)
        {
            start_unit();
            return false;
        }

        finish_row();
        return true;
    }

    void finish_row() noexcept
    {
        const bool wide{frame_info().bits_per_sample > 8};
        size_t destination{};
        for (size_t index{}; index < width_; ++index)
        {
            for (int32_t component{}; component < component_count_; ++component)
            {
                const sample_type value{current(component, index + 1)};
                if (wide)
                {
                    std::memcpy(row_bytes_.data() + destination, &value, sizeof(sample_type));
                    destination += sizeof(sample_type);
                }
                else
                {
                    row_bytes_[destination] = static_cast<byte>(static_cast<uint8_t>(value));
                    ++destination;
                }
            }
        }

        ++row_;
        row_started_ = false;
        restart_pending_ = row_ < frame_info().height && restart_interval_ != 0 && row_ % restart_interval_ == 0;
    }

    [[nodiscard]]
    step_result decode_row(const byte*& position, const byte* end, const bool final)
    {
        for (;;)
        {
            if (operation_ == operation::none)
            {
                if (index_ > width_)
                {
                    if (finish_unit())
                        return step_result::row_ready;
                    continue;
                }

                if (interleave_mode_ == interleave_mode::sample)
                {
                    bool all_zero{true};
                    for (int32_t component{}; component < component_count_; ++component)
                    {
                        if (compute_gradient_context(component, index_) != 0)
                            all_zero = false;
                    }

                    if (all_zero)
                    {
                        regular_samples_ = false;
                        start_run(component_count_, 0, index_);
                    }
                    else
                    {
                        regular_samples_ = true;
                        op_sample_component_ = 0;
                        start_regular(0, index_);
                    }
                }
                else
                {
                    regular_samples_ = false;
                    if (const int32_t qs{compute_gradient_context(unit_, index_)}; qs != 0)
                        start_regular(unit_, index_);
                    else
                        start_run(1, unit_, index_);
                }
            }

            switch (operation_)
            {
            case operation::regular:
                if (!complete_regular(position, end, final))
                    return step_result::need_input;

                operation_ = operation::none;
                if (regular_samples_ && op_sample_component_ + 1 < component_count_)
                {
                    ++op_sample_component_;
                    start_regular(op_sample_component_, index_);
                }
                else
                {
                    regular_samples_ = false;
                    ++index_;
                }
                break;

            case operation::run_pixels:
            case operation::run_remainder: {
                const auto result{complete_pixel_run(position, end, final)};
                if (result == pixel_run_result::need_input)
                    return step_result::need_input;

                operation_ = result == pixel_run_result::line_complete ? operation::none : operation_;

                if (result == pixel_run_result::line_complete)
                {
                    if (finish_unit())
                        return step_result::row_ready;
                }
                break;            }

            case operation::run_interruption:
                if (!complete_run_interruption(position, end, final))
                    return step_result::need_input;
                break;

            case operation::none:
                break;
            }
        }
    }

    // Called when the bit cache could not supply the requested bits. A marker in place of data is a structural
    // error; otherwise the supplied interval has ended (this includes a trailing 0xFF whose meaning depends on the
    // next byte). At the end of the input, a missing codeword is a format error, never zero padding.
    static void handle_read_failure(const bit_reader& reader, const bool final)
    {
        if (reader.marker_at())
            throw_jpegls_error(jpegls_errc::invalid_data);

        if (final)
            throw_jpegls_error(jpegls_errc::need_more_data);
    }

    void handle_read_failure(const byte*, const byte*, const bool final)
    {
        handle_read_failure(bit_reader_, final);
    }

    [[nodiscard]]
    step_result complete_restart_marker(const byte*& position, const byte* end, const bool final)
    {
        if (!marker_started_)
        {
            // The entropy data of the interval ends here. Verify its padding and resolve a possible pending 0xFF that
            // was actually the start of the restart marker.
            if (!bit_reader_.resolve_trailing_padding(position, end))
            {
                if (final)
                    throw_jpegls_error(jpegls_errc::restart_marker_not_found);
                return step_result::need_input;
            }

            restart_code_next_ = bit_reader_.marker_at();
            bit_reader_.reset();
            marker_started_ = true;
        }

        const bool code_next{restart_code_next_};
        restart_code_next_ = false;

        if (code_next)
        {
            if (position == end)
            {
                if (final)
                    throw_jpegls_error(jpegls_errc::restart_marker_not_found);
                return step_result::need_input;
            }

            const byte marker{*position};
            ++position;
            if (marker == jpeg_marker_start_byte)
            {
                restart_code_next_ = true; // Fill byte: the actual marker code follows.
                return step_result::need_input;
            }

            const uint32_t expected{jpeg_restart_marker_base + restart_counter_};
            if (UNLIKELY(std::to_integer<uint32_t>(marker) != expected))
                throw_jpegls_error(jpegls_errc::restart_marker_not_found);

            restart_counter_ = (restart_counter_ + 1) % jpeg_restart_marker_range;
            reset_after_restart();
            marker_started_ = false;
            return step_result::done;
        }

        if (position == end)
        {
            if (final)
                throw_jpegls_error(jpegls_errc::restart_marker_not_found);
            return step_result::need_input;
        }

        if (*position != jpeg_marker_start_byte)
            throw_jpegls_error(jpegls_errc::restart_marker_not_found);

        ++position;
        restart_code_next_ = true;
        return step_result::need_input;
    }

    void reset_after_restart() noexcept
    {
        run_indexes_.fill(0);
        std::fill(line_buffers_.begin(), line_buffers_.end(), sample_type{});
        initialize_parameters(traits_.range);
        bit_reader_.reset();
        row_started_ = false;
        restart_pending_ = false;
    }

    [[nodiscard]]
    step_result complete_end_of_image(const byte*& position, const byte* end, const bool final)
    {
        if (!scan_end_started_)
        {
            scan_end_started_ = true;
            eoi_padding_byte_skipped_ = false;
            eoi_code_next_ = false;
        }

        if (!bit_reader_.resolve_trailing_padding(position, end))
        {
            if (final)
                throw_jpegls_error(jpegls_errc::need_more_data);
            return step_result::need_input;
        }

        if (bit_reader_.marker_at())
        {
            eoi_code_next_ = true; // The 0xFF of the marker was already consumed as a pending byte.
        }
        else if (bit_reader_.padding_byte_consumed())
        {
            // The final data byte has already been consumed and its padding verified.
            eoi_padding_byte_skipped_ = true;
        }

        for (;;)
        {
            if (position == end)
            {
                if (final)
                    throw_jpegls_error(jpegls_errc::need_more_data);
                return step_result::need_input;
            }

            if (!eoi_code_next_)
            {
                const byte value{*position};
                if (!eoi_padding_byte_skipped_ && value != jpeg_marker_start_byte)
                {
                    // The scan end consumed one bit of this byte; the remaining 7 bits must be zero padding.
                    // This matches the whole-frame decoder's end-of-scan rule and tolerates the single padding
                    // byte that some encoders emit after an 0xFF data byte.
                    if ((value & byte{0x7F}) != byte{})
                        throw_jpegls_error(jpegls_errc::invalid_data);
                    eoi_padding_byte_skipped_ = true;
                    ++position;
                    continue;
                }

                ++position;
                eoi_code_next_ = true;
                continue;
            }

            const byte value{*position};
            ++position;
            if (value == jpeg_marker_start_byte)
                continue; // 0xFF fill bytes before the marker code.

            if (std::to_integer<uint8_t>(value) != static_cast<uint8_t>(jpeg_marker_code::end_of_image))
                throw_jpegls_error(jpegls_errc::end_of_image_marker_not_found);

            phase_ = phase::done;
            return step_result::done;
        }
    }

    default_traits<uint16_t, uint16_t> traits_;
    bit_reader bit_reader_;
    std::vector<sample_type> line_buffers_;
    std::vector<byte> row_bytes_;
    sample_type* previous_line_{};
    sample_type* current_line_{};
    array<uint32_t, maximum_component_count_in_scan> run_indexes_{};
    int32_t near_lossless_{};
    int32_t component_count_{};
    interleave_mode interleave_mode_{};
    uint32_t restart_interval_{};
    size_t pixel_stride_{};
    uint32_t row_{};
    bool row_started_{};
    bool restart_pending_{};
    phase phase_{phase::decode};

    size_t index_{1};
    int32_t unit_{};
    operation operation_{operation::none};
    bool regular_samples_{};
    int32_t op_sample_component_{};
    int32_t op_component_{};
    int32_t op_sign_{};
    size_t op_context_{};
    int32_t op_corrected_prediction_{};
    int32_t op_k_{};
    int32_t op_limit_{};
    int32_t op_unary_{};
    int32_t op_mapped_{};
    op_phase op_phase_{op_phase::unary};

    int32_t op_run_component_count_{};
    array<int32_t, maximum_component_count_in_scan> op_components_{};
    array<sample_type, maximum_component_count_in_scan> op_run_value_{};
    size_t op_run_start_{};
    size_t op_run_end_{};
    int32_t op_remainder_bits_{};

    int32_t op_interrupt_component_{};
    size_t op_interrupt_end_{};
    array<int32_t, maximum_component_count_in_scan> op_interrupt_context_{};
    array<int32_t, maximum_component_count_in_scan> op_interrupt_prediction_{};
    array<int32_t, maximum_component_count_in_scan> op_interrupt_multiplier_{};
    array<sample_type, maximum_component_count_in_scan> op_interrupt_reconstruction_{};
    int32_t op_interrupt_context_index_{};

    bool marker_started_{};
    bool restart_code_next_{};
    uint32_t restart_counter_{};

    bool scan_end_started_{};
    bool eoi_code_next_{};
    bool eoi_padding_byte_skipped_{};
};


struct jpegls_stream_decoder::impl final
{
    [[nodiscard]]
    stream_decode_result push(const void* input, const size_t size, const bool final)
    {
        if (failed_)
            throw_jpegls_error(jpegls_errc::invalid_operation);

        row_valid_ = false;

        if (done_)
            return {0, stream_decode_status::done};

        if (size != 0 && input == nullptr)
            throw_jpegls_error(jpegls_errc::invalid_argument);

        try
        {
            return push_impl(static_cast<const byte*>(input), size, final);
        }
        catch (...)
        {
            failed_ = true;
            row_valid_ = false;
            throw;
        }
    }

    [[nodiscard]]
    charls::frame_info get_frame_info() const
    {
        if (!frame_info_ready_)
            throw_jpegls_error(jpegls_errc::invalid_operation);
        return frame_info_;
    }

    [[nodiscard]]
    std::pair<const byte*, size_t> get_row() const
    {
        if (!row_valid_)
            throw_jpegls_error(jpegls_errc::invalid_operation);

        return {scan_decoder_->row_data(), scan_decoder_->row_size()};
    }

private:
    [[nodiscard]]
    stream_decode_result push_impl(const byte* input, const size_t size, const bool final)
    {
        const byte* position{input};
        const byte* const end{input + size};

        if (!header_parsed_)
        {
            if (!consume_header(input, position, end, final))
            {
                return {size, stream_decode_status::need_input};
            }
        }

        if (position == end)
        {
            if (!final)
                return {size, stream_decode_status::need_input};

            // The input ended exactly at a row boundary. The decoder must still consume and verify the EOI marker.
        }

        const auto result{scan_decoder_->step(position, end, final)};

        switch (result)
        {
        case stream_scan_decoder::step_result::row_ready:
            row_valid_ = true;
            return {static_cast<size_t>(position - input), stream_decode_status::row_ready};

        case stream_scan_decoder::step_result::need_input:
            return {static_cast<size_t>(position - input), stream_decode_status::need_input};

        case stream_scan_decoder::step_result::done:
            done_ = true;
            return {static_cast<size_t>(position - input), stream_decode_status::done};
        }

        unreachable();
    }

    // Collects header bytes until the end of the first SOS segment is known.
    // Returns false when all supplied input was accepted but the header is still incomplete.
    // When it returns true, position is updated to the first byte after the header, which is the
    // first entropy coded byte. Bytes beyond that point are not accepted by this push.
    [[nodiscard]]
    bool consume_header(const byte* input, const byte*& position, const byte* end, const bool final)
    {
        const size_t header_size_at_push_start{header_bytes_.size()};

        while (position != end)
        {
            const size_t take{std::min(header_chunk_size, static_cast<size_t>(end - position))};
            header_bytes_.insert(header_bytes_.end(), position, position + take);
            position += take;

            header_layout_ = scan_header_layout(header_bytes_);
            update_frame_info_from_layout();

            if (!header_layout_.complete)
                continue;

            const size_t accepted_this_push{header_layout_.end - header_size_at_push_start};
            position = input + accepted_this_push;
            if (header_layout_.end < header_bytes_.size())
                header_bytes_.resize(header_layout_.end);

            parse_header();
            header_parsed_ = true;
            return true;
        }

        header_layout_ = scan_header_layout(header_bytes_);
        update_frame_info_from_layout();

        if (!final)
            return false;

        // At the end of the input the complete buffer is the header, and the failure is a format error.
        parse_header();
        header_parsed_ = true;
        return true;
    }

    // The frame information becomes available as soon as the complete SOF segment has been read, even if the
    // remaining header segments (and the start of scan) have not arrived yet.
    void update_frame_info_from_layout() noexcept
    {
        if (header_layout_.have_sof && !frame_info_ready_)
        {
            frame_info_ = header_layout_.sof;
            frame_info_ready_ = true;
        }
    }

    void parse_header()
    {
        jpeg_stream_reader reader;
        reader.source({header_bytes_.data(), header_bytes_.size()});

        spiff_header spiff{};
        bool spiff_found{};

        try
        {
            reader.read_header(&spiff, &spiff_found);
            if (spiff_found)
                reader.read_header();
        }
        catch (const jpegls_error& error)
        {
            if (is_missing_define_number_of_lines_error(error.code()))
                throw_jpegls_error(jpegls_errc::parameter_value_not_supported);
            throw;
        }

        if (spiff_found &&
            charls_validate_spiff_header(&spiff, &reader.frame_info()) != jpegls_errc::success)
            throw_jpegls_error(jpegls_errc::invalid_spiff_header);

        if (reader.end_of_image() || reader.mapping_table_count() != 0 || header_layout_.mapping_table ||
            header_layout_.oversize_dimensions || header_layout_.define_number_of_lines ||
            header_layout_.zero_height_sof)
            throw_jpegls_error(jpegls_errc::parameter_value_not_supported);

        if (reader.parameters().transformation != color_transformation::none)
            throw_jpegls_error(jpegls_errc::parameter_value_not_supported);

        const auto component_count{static_cast<int32_t>(reader.component_count())};
        if (component_count > maximum_component_count_in_scan ||
            reader.scan_component_count() != static_cast<uint32_t>(component_count))
            throw_jpegls_error(jpegls_errc::parameter_value_not_supported);

        for (int32_t component{}; component < component_count; ++component)
        {
            if (reader.get_mapping_table_id(static_cast<size_t>(component)) != 0)
                throw_jpegls_error(jpegls_errc::parameter_value_not_supported);
        }

        if (reader.scan_interleave_mode() == interleave_mode::none && component_count > 1)
            throw_jpegls_error(jpegls_errc::parameter_value_not_supported);

        if (const charls::frame_info& frame{reader.frame_info()};
            frame.width > std::numeric_limits<uint16_t>::max() || frame.height > std::numeric_limits<uint16_t>::max())
            throw_jpegls_error(jpegls_errc::parameter_value_not_supported);

        const auto preset_parameters{reader.get_validated_preset_coding_parameters()};
        const coding_parameters parameters{reader.parameters().near_lossless, reader.parameters().restart_interval,
                                           reader.scan_interleave_mode(), color_transformation::none};

        scan_decoder_ = std::make_unique<stream_scan_decoder>(reader.frame_info(), preset_parameters, parameters);

        frame_info_ = reader.frame_info();
        frame_info_ready_ = true;

        std::vector<byte>().swap(header_bytes_);
    }

    std::vector<byte> header_bytes_;
    header_layout header_layout_;
    std::unique_ptr<stream_scan_decoder> scan_decoder_;
    charls::frame_info frame_info_{};
    bool frame_info_ready_{};
    bool header_parsed_{};
    bool row_valid_{};
    bool done_{};
    bool failed_{};
};

} // namespace charls


charls::jpegls_stream_decoder::jpegls_stream_decoder() : impl_{std::make_unique<impl>()}
{
}

charls::jpegls_stream_decoder::~jpegls_stream_decoder() = default;

charls::jpegls_stream_decoder::jpegls_stream_decoder(jpegls_stream_decoder&&) noexcept = default;

charls::jpegls_stream_decoder& charls::jpegls_stream_decoder::operator=(jpegls_stream_decoder&&) noexcept = default;

charls::stream_decode_result charls::jpegls_stream_decoder::push(const void* input, const size_t size,
                                                                 const bool final)
{
    return impl_->push(input, size, final);
}

charls::frame_info charls::jpegls_stream_decoder::frame_info() const
{
    return impl_->get_frame_info();
}

std::pair<const std::byte*, size_t> charls::jpegls_stream_decoder::row() const
{
    return impl_->get_row();
}

void charls::jpegls_stream_decoder::reset()
{
    impl_ = std::make_unique<impl>();
}
