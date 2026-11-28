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

// Export the class when it is implemented in the shared library.
#if defined(_WIN32) && defined(CHARLS_LIBRARY_BUILD) && !defined(CHARLS_STATIC)
#define CHARLS_STREAM_DECODER_EXPORT __declspec(dllexport)
#elif defined(_WIN32) && !defined(CHARLS_STATIC) && !defined(CHARLS_LIBRARY_BUILD)
#define CHARLS_STREAM_DECODER_EXPORT __declspec(dllimport)
#elif defined(__GNUC__) && !defined(_WIN32) && defined(CHARLS_LIBRARY_BUILD)
#define CHARLS_STREAM_DECODER_EXPORT __attribute__((visibility("default")))
#else
#define CHARLS_STREAM_DECODER_EXPORT
#endif

CHARLS_EXPORT
namespace charls {

/// <summary>
/// The status of an incremental JPEG-LS decoding operation.
/// </summary>
enum class stream_decode_status
{
    /// <summary>
    /// All supplied input was consumed, but a complete row is not available yet.
    /// </summary>
    need_input,

    /// <summary>
    /// A complete row is available. Call row() before calling push() again.
    /// </summary>
    row_ready,

    /// <summary>
    /// The end of image has been reached and the EOI marker has been verified.
    /// </summary>
    done
};


/// <summary>
/// Result of an incremental push operation.
/// </summary>
struct stream_decode_result
{
    /// <summary>
    /// Number of bytes accepted from the supplied input. The caller may advance by this amount,
    /// overwrite or release those bytes and resubmit the remaining suffix on the next call.
    /// </summary>
    std::size_t consumed{};

    /// <summary>
    /// Status of the decoder after this push.
    /// </summary>
    stream_decode_status status{stream_decode_status::need_input};
};


/// <summary>
/// Incremental row-by-row JPEG-LS decoder.
/// Input is supplied in arbitrary chunks with push(). Each row_ready result exposes exactly one
/// complete row through row() and the decoder will not decode rows beyond the next delivered row.
/// </summary>
class CHARLS_STREAM_DECODER_EXPORT jpegls_stream_decoder final
{
public:
    /// <summary>
    /// Constructs a decoder in its initial state.
    /// </summary>
    jpegls_stream_decoder();

    ~jpegls_stream_decoder();

    jpegls_stream_decoder(jpegls_stream_decoder&&) noexcept;
    jpegls_stream_decoder& operator=(jpegls_stream_decoder&&) noexcept;

    jpegls_stream_decoder(const jpegls_stream_decoder&) = delete;
    jpegls_stream_decoder& operator=(const jpegls_stream_decoder&) = delete;

    /// <summary>
    /// Supplies the next interval of compressed input bytes.
    /// </summary>
    /// <param name="input">Start of the compressed bytes.</param>
    /// <param name="size">Number of compressed bytes available in input.</param>
    /// <param name="final">True when input ends at the end of the compressed stream.</param>
    /// <returns>The number of accepted bytes and the resulting status.</returns>
    /// <exception cref="charls::jpegls_error">An error occurred during the operation.</exception>
    [[nodiscard]]
    stream_decode_result push(const void* input, std::size_t size, bool final);

    /// <summary>
    /// Returns the frame information after the frame header has been read.
    /// </summary>
    /// <exception cref="charls::jpegls_error">jpegls_errc::invalid_operation when the frame header is not complete.</exception>
    [[nodiscard]]
    frame_info frame_info() const;

    /// <summary>
    /// Returns the most recently delivered row.
    /// The returned memory is owned by the decoder and remains valid until the next push() or reset().
    /// </summary>
    /// <exception cref="charls::jpegls_error">jpegls_errc::invalid_operation when the last result was not row_ready.</exception>
    [[nodiscard]]
    std::pair<const std::byte*, std::size_t> row() const;

    /// <summary>
    /// Returns the decoder to the state of a newly constructed instance.
    /// </summary>
    void reset();

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace charls

#undef CHARLS_STREAM_DECODER_EXPORT
