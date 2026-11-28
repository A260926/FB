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

#ifndef ARIBCAPTION_DECODER_LINK_REASSEMBLY_HPP
#define ARIBCAPTION_DECODER_LINK_REASSEMBLY_HPP

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "aribcaption/decoder.hpp"

namespace aribcaption::internal {

/**
 * Transport-only reassembler for data groups transmitted as linked fragments.
 *
 * Each PES carries exactly one link fragment:
 *   byte 0: high 6 bits = full data_group_id, low 2 bits = version
 *   byte 1: link_number
 *   byte 2: last_link_number
 *   byte 3-4: big-endian payload length
 *   following: payload, then 2 big-endian CRC-16 bytes
 *
 * The reassembler only concatenates payloads. It never parses caption bodies,
 * so language tables, DRCS and display instructions stay untouched until a
 * group is complete. See task_support/link_contract.md.
 */
class LinkReassembler {
public:
    LinkReassembler() = default;
    ~LinkReassembler() = default;

    /**
     * Feed one link fragment (pointing at the data_group_id/version byte).
     *
     * On @kGotCaption the group is complete: @p out_group_id identifies it,
     * @p out_body holds the concatenated payload and @p out_pts is the PTS
     * of link 0 (which may be PTS_NOPTS). The completed group is removed from
     * the in-flight table, so a later link 0 starts a brand new group.
     *
     * On @kError the affected in-flight group (if identifiable) is dropped;
     * groups with other data_group_id values keep accumulating.
     */
    DecodeStatus ProcessFragment(const uint8_t* fragment, size_t length, int64_t pts,
                                 uint8_t& out_group_id,
                                 std::vector<uint8_t>& out_body,
                                 int64_t& out_pts);

    /** Drop every in-flight group. Display state is not touched. */
    void Clear();

private:
    struct InFlightGroup {
        uint8_t version = 0;
        uint8_t last_link_number = 0;
        uint8_t next_link_number = 0;  // link_number expected on the next fragment
        int64_t first_pts = 0;
        std::vector<uint8_t> body;
        std::vector<uint8_t> last_fragment;  // header + payload of the most recently accepted fragment
    };

private:
    std::unordered_map<uint8_t, InFlightGroup> groups_;
};

}  // namespace aribcaption::internal

#endif  // ARIBCAPTION_DECODER_LINK_REASSEMBLY_HPP
