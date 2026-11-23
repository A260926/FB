/*
 * Copyright (C) 2024 magicxqq <xqq@xqq.im>. All rights reserved.
 *
 * This file is part of libaribcaption.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE AND TITLE.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING
 * FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
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
 * An encoded ARIB private caption data packet.
 *
 * The payload starts with the 3-byte private caption header (0x80 0xFF 0xF0)
 * and contains neither an outer MPEG-TS framing nor a PES start code.
 */
struct EncodedPES {
    /**
     * Presentation timestamp in milliseconds, inherited from the source Caption.
     * Equals PTS_NOPTS if the source Caption's pts was PTS_NOPTS.
     */
    int64_t pts = PTS_NOPTS;

    /**
     * Raw packet bytes, including the private caption header and the CRC-16.
     */
    std::vector<uint8_t> data;
public:
    EncodedPES() = default;
    EncodedPES(const EncodedPES&) = default;
    EncodedPES(EncodedPES&&) noexcept = default;
    EncodedPES& operator=(const EncodedPES&) = default;
    EncodedPES& operator=(EncodedPES&&) noexcept = default;
};

namespace internal { class EncoderImpl; }

/**
 * ARIB STD-B24 caption encoder (Japanese JIS, first language, Profile A, 960x540).
 *
 * The encoder turns Caption objects back into private caption data packets which
 * can be fed into the Decoder. On the first successful Encode() (and again after
 * Reset()) a Japanese caption management group is emitted before the caption
 * statement group. DRCS pattern definitions are cached across Encode() calls and
 * only transmitted when a pattern is used for the first time or when its slot is
 * replaced by a new pattern.
 *
 * Encode() never mutates its output parameter or the encoder state on failure:
 * a failed call can be retried and produces exactly the same packets as an
 * encoder which never saw the failed call.
 *
 * Supported scope:
 *  - Japanese JIS encoding, first language, Profile A, 960x540 full screen
 *  - 36x36 characters, horizontal spacing 4, vertical spacing 24, scale 1.0
 *  - no ruby / decoration / enclosure / built-in sound, transparent background
 *  - the eight opaque foreground colors of CLUT palette 0
 *  - text characters whose Unicode codepoint is reversibly representable in the
 *    JIS graphic sets of the bundled decoder conversion tables
 *  - DRCS patterns of 1..64 width/height, 2 or 4 tones, DRCS-1 (94 slots)
 *
 * Any attribute outside this scope makes Encode() return false without touching
 * the output or the internal state.
 */
class Encoder {
public:
    ARIBCC_API Encoder();
    ARIBCC_API ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    Encoder(Encoder&&) noexcept;
    Encoder& operator=(Encoder&&) noexcept;

    /**
     * Encode one Caption into one or more private caption packets.
     *
     * A caption management packet is prepended when a new session starts (the
     * first call after construction or Reset()). Every statement packet begins
     * with a clear screen and presents an indefinite-duration full screen; an
     * empty region list yields a pure clear-screen picture.
     *
     * On success the previous contents of @p out are replaced. On failure @p out
     * and the encoder state are left completely untouched.
     *
     * @param caption Source caption, it is never modified.
     * @param out     Destination for the encoded packets, replaced on success.
     * @return true on success, false if the caption is outside the supported
     *         scope, it needs more than 94 distinct DRCS patterns, or a data
     *         group would exceed 65535 bytes.
     */
    ARIBCC_API bool Encode(const Caption& caption, std::vector<EncodedPES>& out);

    /**
     * Clear the DRCS pattern cache and restart the session.
     *
     * The next Encode() emits a Japanese caption management group again.
     */
    ARIBCC_API void Reset();
private:
    std::unique_ptr<internal::EncoderImpl> pimpl_;
};

}  // namespace aribcaption

#endif  // ARIBCAPTION_ENCODER_HPP
