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

#include <cstring>
#include "decoder/link_reassembly.hpp"

namespace aribcaption::internal {

static constexpr size_t kLinkHeaderSize = 5;   // id/version, link_number, last_link_number, length(2)
static constexpr size_t kCrcSize = 2;
static constexpr size_t kMaxAssembledPayload = 1u << 20;  // 1 MiB per data group

static uint16_t Crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < length; i++) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc = static_cast<uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

DecodeStatus LinkReassembler::ProcessFragment(const uint8_t* fragment, size_t length, int64_t pts,
                                              uint8_t& out_group_id,
                                              std::vector<uint8_t>& out_body,
                                              int64_t& out_pts) {
    if (fragment == nullptr || length == 0) {
        // No data_group_id can be read: report the error but keep every other group.
        return DecodeStatus::kError;
    }

    const uint8_t group_id = (fragment[0] & 0b11111100) >> 2;
    const uint8_t version = fragment[0] & 0b00000011;

    auto fail_group = [&]() -> DecodeStatus {
        // Drop only the affected group; other data_group_id values keep accumulating.
        groups_.erase(group_id);
        return DecodeStatus::kError;
    };

    if (length < kLinkHeaderSize + kCrcSize) {
        return fail_group();
    }

    const uint8_t link_number = fragment[1];
    const uint8_t last_link_number = fragment[2];
    const size_t payload_length = (static_cast<size_t>(fragment[3]) << 8) | fragment[4];

    if (length != kLinkHeaderSize + payload_length + kCrcSize) {
        // The declared payload length must match the packet exactly.
        return fail_group();
    }

    const size_t crc_covered = kLinkHeaderSize + payload_length;
    const uint16_t expected_crc = static_cast<uint16_t>((fragment[crc_covered] << 8) |
                                                        fragment[crc_covered + 1]);
    if (Crc16(fragment, crc_covered) != expected_crc) {
        return fail_group();
    }

    if (link_number > last_link_number) {
        return fail_group();
    }

    const uint8_t* payload = fragment + kLinkHeaderSize;
    auto iter = groups_.find(group_id);

    if (link_number == 0) {
        if (iter != groups_.end()) {
            // A retransmitted link 0 is idempotent only while the group still holds
            // just that single fragment and the whole header/payload is identical.
            InFlightGroup& group = iter->second;
            if (group.next_link_number == 1 &&
                group.last_fragment.size() == crc_covered &&
                std::memcmp(group.last_fragment.data(), fragment, crc_covered) == 0) {
                // Do not re-append and do not change the first fragment's PTS.
                return DecodeStatus::kNoCaption;
            }
            // Otherwise a legal link 0 (including a new version) replaces the unfinished group.
            groups_.erase(iter);
        }

        InFlightGroup group;
        group.version = version;
        group.last_link_number = last_link_number;
        group.next_link_number = 1;
        group.first_pts = pts;
        group.body.assign(payload, payload + payload_length);
        group.last_fragment.assign(fragment, fragment + crc_covered);

        if (group.body.size() > kMaxAssembledPayload) {
            return fail_group();
        }

        if (last_link_number == 0) {
            // Single-fragment group: validate CRC first, then publish immediately.
            out_group_id = group_id;
            out_body = std::move(group.body);
            out_pts = group.first_pts;
            return DecodeStatus::kGotCaption;
        }

        groups_.emplace(group_id, std::move(group));
        return DecodeStatus::kNoCaption;
    }

    // Non-zero link: an in-flight group with matching predecessor is required.
    if (iter == groups_.end()) {
        return DecodeStatus::kError;  // Nothing identifiable to drop, other groups stay.
    }

    InFlightGroup& group = iter->second;

    if (version != group.version || last_link_number != group.last_link_number) {
        return fail_group();
    }

    if (link_number < group.next_link_number) {
        // Only the most recently accepted fragment may be retransmitted, and it
        // must carry the identical header/payload.
        if (link_number == group.next_link_number - 1 &&
            group.last_fragment.size() == crc_covered &&
            std::memcmp(group.last_fragment.data(), fragment, crc_covered) == 0) {
            return DecodeStatus::kNoCaption;
        }
        return fail_group();  // Out-of-order or content-mismatched retransmission.
    }

    if (link_number != group.next_link_number) {
        return fail_group();  // Gap: a predecessor link is missing.
    }

    if (group.body.size() + payload_length > kMaxAssembledPayload) {
        return fail_group();
    }

    group.body.insert(group.body.end(), payload, payload + payload_length);
    group.last_fragment.assign(fragment, fragment + crc_covered);
    group.next_link_number = static_cast<uint8_t>(link_number + 1);

    if (link_number != last_link_number) {
        return DecodeStatus::kNoCaption;
    }

    // Last fragment arrived: publish the concatenated body with the first PTS.
    out_group_id = group_id;
    out_body = std::move(group.body);
    out_pts = group.first_pts;
    groups_.erase(iter);
    return DecodeStatus::kGotCaption;
}

void LinkReassembler::Clear() {
    groups_.clear();
}

}  // namespace aribcaption::internal
