// SPDX-FileCopyrightText: © 2026 Team CharLS
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "api_abi.h"
#include "jpegls_error.hpp"
#include "public_types.h"

#ifndef CHARLS_BUILD_AS_CPP_MODULE
#include <cstddef>
#include <memory>
#include <utility>
#endif

#if defined(CHARLS_STATIC)
#define CHARLS_STREAM_DECODER_API
#elif defined(_WIN32)
#if defined(CHARLS_LIBRARY_BUILD)
#define CHARLS_STREAM_DECODER_API __declspec(dllexport)
#else
#define CHARLS_STREAM_DECODER_API __declspec(dllimport)
#endif
#elif defined(CHARLS_LIBRARY_BUILD)
#define CHARLS_STREAM_DECODER_API __attribute__((visibility("default")))
#else
#define CHARLS_STREAM_DECODER_API
#endif

CHARLS_EXPORT
namespace charls {

/// <summary>
/// Defines the state of an incremental JPEG-LS decode operation.
/// </summary>
enum class stream_decode_status
{
    /// <summary>
    /// All input of the last push call has been consumed, but no complete row is available yet.
    /// Call push again with more encoded bytes.
    /// </summary>
    need_input,

    /// <summary>
    /// A complete row is available through row(). Process the row and call push again to continue.
    /// </summary>
    row_ready,

    /// <summary>
    /// All rows have been decoded and the End Of Image (EOI) marker has been consumed and verified.
    /// </summary>
    done
};


/// <summary>
/// Result of a call to jpegls_stream_decoder::push.
/// </summary>
struct stream_decode_result final
{
    /// <summary>
    /// Number of encoded bytes from the start of the pushed buffer that have been accepted by the decoder.
    /// The caller may release or overwrite these bytes. A suffix that was not accepted needs to be pushed again.
    /// </summary>
    std::size_t consumed;

    /// <summary>
    /// The state of the decoder after the push call.
    /// </summary>
    stream_decode_status status;
};


/// <summary>
/// Incremental JPEG-LS decoder that decodes one row at a time from a sequence of small input buffers.
/// </summary>
/// <remarks>
/// The decoder supports single scan images with a known width and height, 2 to 16 bits per sample,
/// one component (ILV_NONE) or 2 to 4 components with line or sample interleave. NEAR, JPEG-LS preset
/// coding parameters and restart intervals are supported. Unsupported JPEG-LS constructs (multi scan
/// ILV_NONE color data, mapping tables, HP color transforms, DNL and oversize dimensions) are rejected
/// before any pixel data is delivered.
/// </remarks>
class CHARLS_STREAM_DECODER_API jpegls_stream_decoder final
{
public:
    jpegls_stream_decoder();
    ~jpegls_stream_decoder();

    jpegls_stream_decoder(const jpegls_stream_decoder&) = delete;
    jpegls_stream_decoder& operator=(const jpegls_stream_decoder&) = delete;
    jpegls_stream_decoder(jpegls_stream_decoder&&) noexcept;
    jpegls_stream_decoder& operator=(jpegls_stream_decoder&&) noexcept;

    /// <summary>
    /// Pushes a buffer with encoded JPEG-LS bytes into the decoder.
    /// </summary>
    /// <param name="input">Buffer with encoded bytes. Bytes that are not reported as consumed need to be pushed again.</param>
    /// <param name="size">Size of the input buffer in bytes.</param>
    /// <param name="final">
    /// True when the pushed buffer reaches the end of the encoded input. The caller keeps passing final=true,
    /// with the remaining suffix or an empty buffer, until done or an error is reported.
    /// </param>
    /// <exception cref="charls::jpegls_error">An error occurred while decoding the stream.</exception>
    /// <returns>The number of consumed bytes and the decoder status.</returns>
    CHARLS_ATTRIBUTE_ACCESS((access(read_only, 2, 3)))
    stream_decode_result push(const void* input, std::size_t size, bool final);

    /// <summary>
    /// Returns the frame information of the JPEG-LS stream.
    /// </summary>
    /// <exception cref="charls::jpegls_error">Thrown with jpegls_errc::invalid_operation when the frame header is not complete.</exception>
    [[nodiscard]]
    frame_info frame_info() const;

    /// <summary>
    /// Returns the most recently decoded row.
    /// </summary>
    /// <remarks>
    /// The row is only valid directly after push returned stream_decode_status::row_ready. The memory is owned by
    /// the decoder and remains valid until the next push or reset call.
    /// </remarks>
    /// <exception cref="charls::jpegls_error">Thrown with jpegls_errc::invalid_operation when no row is available.</exception>
    [[nodiscard]]
    std::pair<const std::byte*, std::size_t> row() const;

    /// <summary>
    /// Resets the decoder to the state it had directly after construction.
    /// </summary>
    void reset();

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace charls

#undef CHARLS_STREAM_DECODER_API
