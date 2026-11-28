/*
 * Copyright (C) 2021 magicxqq <xqq@xqq.im>. All rights reserved.
 *
 * This file is part of libaribcaption.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef ARIBCAPTION_ENCODER_HPP
#define ARIBCAPTION_ENCODER_HPP

#include <cstdint>
#include <memory>
#include <vector>
#include "aribcc_export.h"
#include "caption.hpp"

namespace aribcaption {

/**
 * Structure for holding an encoded caption packet.
 *
 * The packet data starts with the private caption header (0x80 0xFF 0xF0)
 * and does not contain any outer MPEG-TS or PES start code.
 */
struct EncodedPES {
    /**
     * Presentation timestamp inherited from the input Caption, in milliseconds.
     * PTS_NOPTS is preserved as-is.
     */
    int64_t pts = PTS_NOPTS;

    /**
     * Raw caption packet bytes.
     */
    std::vector<uint8_t> data;
};

/**
 * ARIB STD-B24 caption encoder.
 *
 * The encoder turns a decoded Caption back into ARIB STD-B24 caption packets.
 * It is intended to be reused: DRCS glyph definitions are cached across calls
 * until Reset() is invoked.
 *
 * Supported subset:
 *  - Japanese JIS (ARIB STD-B24 JIS), first language, Profile A
 *  - 960x540 full-screen captions
 *  - 36x36 characters, horizontal spacing 4, vertical spacing 24, scale 1.0
 *  - transparent background, opaque text colors from palette 0 (indexes 0-7)
 *  - DRCS-1 glyphs with width/height in 1..64, depth 2 or 4
 *
 * Unsupported properties cause Encode() to return false without modifying
 * either the output vector or the encoder state.
 */
class ARIBCC_API Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(Encoder&&) noexcept;
    Encoder& operator=(Encoder&&) noexcept;
public:
    /**
     * Encode a caption.
     *
     * On success, @p out is replaced with the generated packets (a management
     * packet on the first successful call after construction / Reset(), followed
     * by a statement packet) and true is returned.
     *
     * On failure, @p out and the encoder state are left untouched.
     *
     * @param caption Input caption
     * @param out     Output packets, replaced on success
     * @return true on success, false if the caption uses unsupported properties
     *         or cannot be represented within the packet size limit
     */
    bool Encode(const Caption& caption, std::vector<EncodedPES>& out);

    /**
     * Reset encoder state.
     *
     * Clears the DRCS glyph cache and makes the next successful Encode() emit
     * a caption management packet again.
     */
    void Reset();
public:
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aribcaption

#endif  // ARIBCAPTION_ENCODER_HPP
