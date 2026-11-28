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

// Regression for the optional linked data group reassembly mode.
// See task_support/link_contract.md for the transport contract.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "aribcaption/aribcaption.hpp"
#include "aribcaption/aribcaption.h"

extern "C" {
#include "task_support/check.h"
}

using namespace aribcaption;

namespace {

// ---------------------------------------------------------------------------
// Packet construction (CRC is computed by hand, no external dependency)
// ---------------------------------------------------------------------------

uint16_t Crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < length; i++) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

void AppendBe24(std::vector<uint8_t>& out, size_t value) {
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

// One data unit inside a data unit loop.
std::vector<uint8_t> MakeDataUnit(uint8_t parameter, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> unit = {0x1f, parameter};
    AppendBe24(unit, body.size());
    unit.insert(unit.end(), body.begin(), body.end());
    return unit;
}

// Caption statement data group body wrapping the given data units.
std::vector<uint8_t> MakeStatementBody(const std::vector<std::vector<uint8_t>>& units) {
    std::vector<uint8_t> body = {0x00};  // TMD = 0
    size_t loop_length = 0;
    for (const auto& unit : units) {
        loop_length += unit.size();
    }
    AppendBe24(body, loop_length);
    for (const auto& unit : units) {
        body.insert(body.end(), unit.begin(), unit.end());
    }
    return body;
}

// Caption statement body carrying a single statement body unit.
std::vector<uint8_t> MakeStatementBody(const std::vector<uint8_t>& statement) {
    return MakeStatementBody(std::vector<std::vector<uint8_t>>{MakeDataUnit(0x20, statement)});
}

// Caption management data group body for two languages.
std::vector<uint8_t> MakeManagementBody(uint32_t lang1, uint32_t lang2) {
    std::vector<uint8_t> body = {0x00, 0x02};  // TMD = 0, num_languages = 2
    body.push_back(0x00);  // language tag 0, DMF = 0
    body.push_back(static_cast<uint8_t>((lang1 >> 16) & 0xff));
    body.push_back(static_cast<uint8_t>((lang1 >> 8) & 0xff));
    body.push_back(static_cast<uint8_t>(lang1 & 0xff));
    body.push_back(0x80);  // format = 8 (SWF 7), TCS = 0
    body.push_back(0x20);  // language tag 1, DMF = 0
    body.push_back(static_cast<uint8_t>((lang2 >> 16) & 0xff));
    body.push_back(static_cast<uint8_t>((lang2 >> 8) & 0xff));
    body.push_back(static_cast<uint8_t>(lang2 & 0xff));
    body.push_back(0x80);
    AppendBe24(body, 0);  // empty data unit loop
    return body;
}

// A valid 2-byte DRCS (code U+EC00), 16x18 2-bit, all-zero bitmap. Parameter 0x31.
std::vector<uint8_t> MakeDrcsUnit() {
    std::vector<uint8_t> drcs = {
        0x01,        // number_of_code = 1
        0xEC, 0x00,  // character code 0xEC00 (PUA)
        0x01,        // number_of_font = 1
        0x00,        // font_id = 0, mode = 0
        0x00,        // depth = 2
        16, 18       // width x height
    };
    drcs.resize(drcs.size() + 36, 0);  // 16 * 18 * 1 bit / 8 = 36 bytes bitmap
    return MakeDataUnit(0x31, drcs);
}

// Legacy PES carrying one complete data group (reassembly disabled).
std::vector<uint8_t> MakeDirectPacket(uint8_t group_id, uint8_t version,
                                      const std::vector<uint8_t>& group_body) {
    std::vector<uint8_t> packet = {0x80, 0xff, 0xf0};
    packet.push_back(static_cast<uint8_t>((group_id << 2) | (version & 0x03)));
    packet.push_back(0x00);  // link_number
    packet.push_back(0x00);  // last_link_number
    packet.push_back(static_cast<uint8_t>((group_body.size() >> 8) & 0xff));
    packet.push_back(static_cast<uint8_t>(group_body.size() & 0xff));
    packet.insert(packet.end(), group_body.begin(), group_body.end());
    return packet;
}

// PES carrying exactly one link fragment. CRC covers id/version .. payload end.
std::vector<uint8_t> MakeLinkPacket(uint8_t group_id, uint8_t version,
                                    uint8_t link_number, uint8_t last_link_number,
                                    const std::vector<uint8_t>& payload,
                                    bool corrupt_crc = false) {
    std::vector<uint8_t> packet = {0x80, 0xff, 0xf0};
    const size_t fragment_begin = packet.size();
    packet.push_back(static_cast<uint8_t>((group_id << 2) | (version & 0x03)));
    packet.push_back(link_number);
    packet.push_back(last_link_number);
    packet.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xff));
    packet.push_back(static_cast<uint8_t>(payload.size() & 0xff));
    packet.insert(packet.end(), payload.begin(), payload.end());
    uint16_t crc = Crc16(packet.data() + fragment_begin, packet.size() - fragment_begin);
    if (corrupt_crc) {
        crc = static_cast<uint16_t>(crc ^ 0x0001);
    }
    packet.push_back(static_cast<uint8_t>(crc >> 8));
    packet.push_back(static_cast<uint8_t>(crc & 0xff));
    return packet;
}

std::vector<std::vector<uint8_t>> SplitPayload(const std::vector<uint8_t>& body,
                                               const std::vector<size_t>& cuts) {
    std::vector<std::vector<uint8_t>> fragments;
    size_t begin = 0;
    for (size_t cut : cuts) {
        fragments.emplace_back(body.begin() + begin, body.begin() + cut);
        begin = cut;
    }
    fragments.emplace_back(body.begin() + begin, body.end());
    return fragments;
}

// Statement body used by most cases: APS(y=2,x=3) "AB", APR, APS(y=4,x=1) "CD".
// SWF 7 geometry: section 40x60 dots. Region 1 at (120,120), region 2 at (40,240).
std::vector<uint8_t> TwoLineStatement() {
    return std::vector<uint8_t>{
        0x1c, 2, 3, 'A', 'B', 0x0d, 0x1c, 4, 1, 'C', 'D'
    };
}

bool SameCaption(const Caption& a, const Caption& b) {
    if (a.text != b.text) return false;
    if (a.iso6392_language_code != b.iso6392_language_code) return false;
    if (a.pts != b.pts) return false;
    if (a.plane_width != b.plane_width || a.plane_height != b.plane_height) return false;
    if (a.regions.size() != b.regions.size()) return false;
    if (a.drcs_map.size() != b.drcs_map.size()) return false;

    for (size_t i = 0; i < a.regions.size(); i++) {
        const CaptionRegion& ra = a.regions[i];
        const CaptionRegion& rb = b.regions[i];
        if (ra.x != rb.x || ra.y != rb.y || ra.width != rb.width || ra.height != rb.height) return false;
        if (ra.chars.size() != rb.chars.size()) return false;
        for (size_t j = 0; j < ra.chars.size(); j++) {
            const CaptionChar& ca = ra.chars[j];
            const CaptionChar& cb = rb.chars[j];
            if (ca.x != cb.x || ca.y != cb.y) return false;
            if (ca.type != cb.type || ca.codepoint != cb.codepoint || ca.drcs_code != cb.drcs_code) return false;
            if (std::string(ca.u8str) != std::string(cb.u8str)) return false;
        }
    }
    return true;
}

Decoder MakeEnabledDecoder(Context& ctx) {
    Decoder decoder(ctx);
    decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8);
    decoder.SetDataGroupReassemblyEnabled(true);
    return decoder;
}

// ---------------------------------------------------------------------------
// link_transport
// ---------------------------------------------------------------------------

int case_three_fragment_interleave() {
    Context ctx;
    Decoder dec = MakeEnabledDecoder(ctx);

    const std::vector<uint8_t> body_x = MakeStatementBody(TwoLineStatement());
    const std::vector<uint8_t> body_y = MakeStatementBody(std::vector<uint8_t>{0x1c, 5, 0, 'Y', 'Z'});
    const auto fx = SplitPayload(body_x, {2, 5});   // 3 fragments
    const auto fy = SplitPayload(body_y, {3, 7});   // 3 fragments

    DecodeResult r;
    // Interleaved delivery of group 0x01 and group 0x21.
    TASK_CHECK(dec.Decode(MakeLinkPacket(1, 0, 0, 2, fx[0]).data(),
                          MakeLinkPacket(1, 0, 0, 2, fx[0]).size(), 1000, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(!r.caption);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x21, 0, 0, 2, fy[0]).data(),
                          MakeLinkPacket(0x21, 0, 0, 2, fy[0]).size(), 2000, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.Decode(MakeLinkPacket(1, 0, 1, 2, fx[1]).data(),
                          MakeLinkPacket(1, 0, 1, 2, fx[1]).size(), 1100, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x21, 0, 1, 2, fy[1]).data(),
                          MakeLinkPacket(0x21, 0, 1, 2, fy[1]).size(), 2100, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.Decode(MakeLinkPacket(1, 0, 2, 2, fx[2]).data(),
                          MakeLinkPacket(1, 0, 2, 2, fx[2]).size(), 1200, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr);
    TASK_CHECK(r.caption->text == "AB\nCD");
    TASK_CHECK(r.caption->pts == 1000);  // first fragment PTS wins
    TASK_CHECK(r.caption->regions.size() == 2);
    TASK_CHECK(r.caption->regions[0].x == 120 && r.caption->regions[0].y == 120);
    TASK_CHECK(r.caption->regions[0].chars.size() == 2);
    TASK_CHECK(r.caption->regions[0].chars[0].x == 120 && r.caption->regions[0].chars[0].y == 120);
    TASK_CHECK(r.caption->regions[0].chars[1].x == 160);
    TASK_CHECK(r.caption->regions[1].x == 40 && r.caption->regions[1].y == 240);
    TASK_CHECK(r.caption->regions[1].chars[1].x == 80 && r.caption->regions[1].chars[1].y == 240);

    TASK_CHECK(dec.Decode(MakeLinkPacket(0x21, 0, 2, 2, fy[2]).data(),
                          MakeLinkPacket(0x21, 0, 2, 2, fy[2]).size(), 2200, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr);
    TASK_CHECK(r.caption->text == "YZ");
    TASK_CHECK(r.caption->pts == 2000);
    TASK_CHECK(r.caption->regions.size() == 1);
    TASK_CHECK(r.caption->regions[0].x == 0 && r.caption->regions[0].y == 300);
    return 0;
}

int case_first_fragment_retransmit() {
    Context ctx;
    Decoder dec = MakeEnabledDecoder(ctx);

    const auto body = MakeStatementBody(TwoLineStatement());
    const auto fragments = SplitPayload(body, {4});  // 2 fragments
    DecodeResult r;

    auto first = MakeLinkPacket(0x1, 0, 0, 1, fragments[0]);
    TASK_CHECK(dec.Decode(first.data(), first.size(), 700, r) == DecodeStatus::kNoCaption);
    // Identical link 0 retransmission while only the first fragment is held.
    auto first_repeat = MakeLinkPacket(0x1, 0, 0, 1, fragments[0]);
    TASK_CHECK(dec.Decode(first_repeat.data(), first_repeat.size(), 750, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(!r.caption);

    auto last = MakeLinkPacket(0x1, 0, 1, 1, fragments[1]);
    TASK_CHECK(dec.Decode(last.data(), last.size(), 900, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr);
    TASK_CHECK(r.caption->text == "AB\nCD");
    TASK_CHECK(r.caption->pts == 700);  // retransmission must not change the first PTS
    return 0;
}

int case_middle_fragment_retransmit() {
    Context ctx;
    Decoder dec = MakeEnabledDecoder(ctx);

    const auto body = MakeStatementBody(TwoLineStatement());
    const auto fragments = SplitPayload(body, {2, 5});  // 3 fragments
    DecodeResult r;

    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 0, 2, fragments[0]).data(),
                          MakeLinkPacket(0x1, 0, 0, 2, fragments[0]).size(), 100, r) == DecodeStatus::kNoCaption);
    auto middle = MakeLinkPacket(0x1, 0, 1, 2, fragments[1]);
    TASK_CHECK(dec.Decode(middle.data(), middle.size(), 200, r) == DecodeStatus::kNoCaption);
    // Idempotent retransmission of the most recently accepted fragment.
    auto middle_repeat = MakeLinkPacket(0x1, 0, 1, 2, fragments[1]);
    TASK_CHECK(dec.Decode(middle_repeat.data(), middle_repeat.size(), 250, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 2, 2, fragments[2]).data(),
                          MakeLinkPacket(0x1, 0, 2, 2, fragments[2]).size(), 300, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr);
    TASK_CHECK(r.caption->text == "AB\nCD");  // no duplicated append
    TASK_CHECK(r.caption->pts == 100);

    // A content-mismatched retransmission of an accepted fragment is an error and drops the group.
    Decoder dec2 = MakeEnabledDecoder(ctx);
    std::vector<uint8_t> altered = fragments[1];
    altered[0] = static_cast<uint8_t>(altered[0] ^ 0x01);
    TASK_CHECK(dec2.Decode(MakeLinkPacket(0x21, 0, 0, 2, fragments[0]).data(),
                           MakeLinkPacket(0x21, 0, 0, 2, fragments[0]).size(), 1, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec2.Decode(MakeLinkPacket(0x21, 0, 1, 2, fragments[1]).data(),
                           MakeLinkPacket(0x21, 0, 1, 2, fragments[1]).size(), 2, r) == DecodeStatus::kNoCaption);
    // Correct CRC over the changed payload: a genuine content-mismatched retransmission.
    TASK_CHECK(dec2.Decode(MakeLinkPacket(0x21, 0, 1, 2, altered).data(),
                           MakeLinkPacket(0x21, 0, 1, 2, altered).size(), 3, r) == DecodeStatus::kError);
    // Group 0x21 is gone: its final fragment now lacks a predecessor.
    TASK_CHECK(dec2.Decode(MakeLinkPacket(0x21, 0, 2, 2, fragments[2]).data(),
                           MakeLinkPacket(0x21, 0, 2, 2, fragments[2]).size(), 4, r) == DecodeStatus::kError);
    return 0;
}

int case_new_version_replaces() {
    Context ctx;
    Decoder dec = MakeEnabledDecoder(ctx);

    const auto body_v0 = MakeStatementBody(std::vector<uint8_t>{'O', 'L', 'D'});
    const auto body_v1 = MakeStatementBody(std::vector<uint8_t>{'N', 'E', 'W'});
    const auto f_v0 = SplitPayload(body_v0, {2});
    const auto f_v1 = SplitPayload(body_v1, {2});
    DecodeResult r;

    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 0, 1, f_v0[0]).data(),
                          MakeLinkPacket(0x1, 0, 0, 1, f_v0[0]).size(), 10, r) == DecodeStatus::kNoCaption);
    // A legal link 0 with a new version replaces the unfinished group.
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 1, 0, 1, f_v1[0]).data(),
                          MakeLinkPacket(0x1, 1, 0, 1, f_v1[0]).size(), 20, r) == DecodeStatus::kNoCaption);
    // A late v0 continuation must not fit the v1 in-flight group (version mismatch -> error/drop).
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 1, 1, f_v0[1]).data(),
                          MakeLinkPacket(0x1, 0, 1, 1, f_v0[1]).size(), 30, r) == DecodeStatus::kError);
    // Restart v1 cleanly and complete it.
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 1, 0, 1, f_v1[0]).data(),
                          MakeLinkPacket(0x1, 1, 0, 1, f_v1[0]).size(), 40, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 1, 1, 1, f_v1[1]).data(),
                          MakeLinkPacket(0x1, 1, 1, 1, f_v1[1]).size(), 50, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr);
    TASK_CHECK(r.caption->text == "NEW");
    TASK_CHECK(r.caption->pts == 40);

    // A same-version link 0 arriving after later fragments were accepted replaces the
    // unfinished group: it is NOT treated as an idempotent single-fragment retransmit.
    Decoder dec2 = MakeEnabledDecoder(ctx);
    const auto body3 = MakeStatementBody(TwoLineStatement());
    const auto f3 = SplitPayload(body3, {2, 5});
    TASK_CHECK(dec2.Decode(MakeLinkPacket(0x11, 0, 0, 2, f3[0]).data(),
                           MakeLinkPacket(0x11, 0, 0, 2, f3[0]).size(), 100, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec2.Decode(MakeLinkPacket(0x11, 0, 1, 2, f3[1]).data(),
                           MakeLinkPacket(0x11, 0, 1, 2, f3[1]).size(), 110, r) == DecodeStatus::kNoCaption);
    // Restart the same group with a fresh link 0 and a new first PTS.
    auto restart = MakeLinkPacket(0x11, 0, 0, 2, f3[0]);
    TASK_CHECK(dec2.Decode(restart.data(), restart.size(), 200, r) == DecodeStatus::kNoCaption);
    // The old link 1 is now stale: the group was reset to hold only the new link 0.
    auto stale_middle = MakeLinkPacket(0x11, 0, 1, 2, f3[1]);
    // It is accepted as the first continuation after replacement (identical bytes), then link 2 completes.
    TASK_CHECK(dec2.Decode(stale_middle.data(), stale_middle.size(), 210, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec2.Decode(MakeLinkPacket(0x11, 0, 2, 2, f3[2]).data(),
                           MakeLinkPacket(0x11, 0, 2, 2, f3[2]).size(), 220, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr);
    TASK_CHECK(r.caption->text == "AB\nCD");
    TASK_CHECK(r.caption->pts == 200);  // first PTS comes from the replacing link 0
    return 0;
}

int case_error_isolation() {
    Context ctx;
    Decoder dec = MakeEnabledDecoder(ctx);
    DecodeResult r;

    const auto good_body = MakeStatementBody(std::vector<uint8_t>{0x1c, 1, 1, 'O', 'K'});
    const auto good = SplitPayload(good_body, {3});

    // Group 0x01: corrupt CRC on the first fragment.
    auto bad = MakeLinkPacket(0x1, 0, 0, 1, std::vector<uint8_t>{1, 2, 3}, true);
    TASK_CHECK(dec.Decode(bad.data(), bad.size(), 1, r) == DecodeStatus::kError);

    // Group 0x11 starts while group 0x01 is failing.
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x11, 0, 0, 1, good[0]).data(),
                          MakeLinkPacket(0x11, 0, 0, 1, good[0]).size(), 2, r) == DecodeStatus::kNoCaption);

    // Group 0x21: non-zero fragment without a predecessor.
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x21, 0, 1, 2, std::vector<uint8_t>{9}).data(),
                          MakeLinkPacket(0x21, 0, 1, 2, std::vector<uint8_t>{9}).size(), 3, r) == DecodeStatus::kError);

    // Group 0x21 again, now started from link 0: link 2 arriving before link 1 is a gap.
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x21, 0, 0, 2, std::vector<uint8_t>{0}).data(),
                          MakeLinkPacket(0x21, 0, 0, 2, std::vector<uint8_t>{0}).size(), 3, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x21, 0, 2, 2, std::vector<uint8_t>{2}).data(),
                          MakeLinkPacket(0x21, 0, 2, 2, std::vector<uint8_t>{2}).size(), 3, r) == DecodeStatus::kError);

    // Group 0x31: last_link_number changes between fragments.
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x31, 0, 0, 2, std::vector<uint8_t>{1}).data(),
                          MakeLinkPacket(0x31, 0, 0, 2, std::vector<uint8_t>{1}).size(), 4, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x31, 0, 1, 1, std::vector<uint8_t>{2}).data(),
                          MakeLinkPacket(0x31, 0, 1, 1, std::vector<uint8_t>{2}).size(), 5, r) == DecodeStatus::kError);

    // Unreadable group id (fragment area empty): error, but other groups survive.
    const std::vector<uint8_t> empty_fragment = {0x80, 0xff, 0xf0};
    TASK_CHECK(dec.Decode(empty_fragment.data(), empty_fragment.size(), 6, r) == DecodeStatus::kError);

    // Declared length does not match the actual packet.
    auto wrong_len = MakeLinkPacket(0x1, 0, 0, 1, std::vector<uint8_t>{1, 2});
    wrong_len[6] = static_cast<uint8_t>(wrong_len[6] + 1);  // corrupt payload length field
    TASK_CHECK(dec.Decode(wrong_len.data(), wrong_len.size(), 7, r) == DecodeStatus::kError);

    // Group 0x11 is unaffected by all the failures above.
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x11, 0, 1, 1, good[1]).data(),
                          MakeLinkPacket(0x11, 0, 1, 1, good[1]).size(), 8, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr);
    TASK_CHECK(r.caption->text == "OK");
    TASK_CHECK(r.caption->pts == 2);
    return 0;
}

int case_single_fragment_group() {
    Context ctx;
    Decoder dec = MakeEnabledDecoder(ctx);
    DecodeResult r;

    const auto body = MakeStatementBody(std::vector<uint8_t>{0x1c, 0, 0, 'Q'});
    // link_number == last_link_number == 0: CRC checked, then parsed immediately.
    auto single = MakeLinkPacket(0x1, 0, 0, 0, body);
    TASK_CHECK(dec.Decode(single.data(), single.size(), 555, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr);
    TASK_CHECK(r.caption->text == "Q");
    TASK_CHECK(r.caption->pts == 555);
    TASK_CHECK(r.caption->regions[0].x == 0 && r.caption->regions[0].y == 0);

    auto single_bad = MakeLinkPacket(0x1, 0, 0, 0, body, true);
    TASK_CHECK(dec.Decode(single_bad.data(), single_bad.size(), 556, r) == DecodeStatus::kError);
    TASK_CHECK(!r.caption);
    return 0;
}

int case_assembled_size_limit() {
    Context ctx;
    Decoder dec = MakeEnabledDecoder(ctx);
    DecodeResult r;

    constexpr size_t kOneMiB = 1u << 20;
    // The per-fragment length is a 16-bit field, so the 1 MiB group cap is reached
    // by accumulating many maximum-size fragments.
    const std::vector<uint8_t> chunk(0xFFFF, 0x41);  // 65535 bytes each
    constexpr int kFullChunks = 16;                  // 16 * 65535 = 1048560, still under 1 MiB
    static_assert(kFullChunks * 0xFFFF < kOneMiB, "precondition");

    for (int i = 0; i < kFullChunks; i++) {
        TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, static_cast<uint8_t>(i), kFullChunks, chunk).data(),
                              MakeLinkPacket(0x1, 0, static_cast<uint8_t>(i), kFullChunks, chunk).size(),
                              i, r) == DecodeStatus::kNoCaption);
    }

    // One more append pushes the assembled body past the 1 MiB cap: drop only this group.
    const std::vector<uint8_t> tail(64, 0x42);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, kFullChunks, kFullChunks, tail).data(),
                          MakeLinkPacket(0x1, 0, kFullChunks, kFullChunks, tail).size(),
                          kFullChunks, r) == DecodeStatus::kError);

    // Other groups are unaffected by the dropped oversized group.
    const auto ok_body = MakeStatementBody(std::vector<uint8_t>{'O', 'K'});
    const auto ok = SplitPayload(ok_body, {1});
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x11, 0, 0, 1, ok[0]).data(),
                          MakeLinkPacket(0x11, 0, 0, 1, ok[0]).size(), 3, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x11, 0, 1, 1, ok[1]).data(),
                          MakeLinkPacket(0x11, 0, 1, 1, ok[1]).size(), 4, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr && r.caption->text == "OK");
    return 0;
}

// ---------------------------------------------------------------------------
// publish_barrier
// ---------------------------------------------------------------------------

int case_no_publish_before_complete() {
    Context ctx;
    Decoder dec = MakeEnabledDecoder(ctx);

    const auto body = MakeStatementBody(TwoLineStatement());
    const auto fragments = SplitPayload(body, {2, 5});
    DecodeResult r;

    for (int i = 0; i < 3; i++) {
        TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, static_cast<uint8_t>(i), 2, fragments[i]).data(),
                              MakeLinkPacket(0x1, 0, static_cast<uint8_t>(i), 2, fragments[i]).size(),
                              100 + i, r) == (i == 2 ? DecodeStatus::kGotCaption : DecodeStatus::kNoCaption));
        if (i < 2) {
            // Nothing is published and no partial parsing state escapes.
            TASK_CHECK(!r.caption);
        }
    }
    TASK_CHECK(r.caption != nullptr);
    TASK_CHECK(r.caption->text == "AB\nCD");
    TASK_CHECK(r.caption->regions.size() == 2);
    TASK_CHECK(r.caption->regions[0].chars[0].x == 120 && r.caption->regions[0].chars[0].y == 120);
    TASK_CHECK(r.caption->regions[1].chars[0].x == 40 && r.caption->regions[1].chars[0].y == 240);
    return 0;
}

int case_equivalence_with_direct_decode() {
    Context ctx;

    // One body exercising DRCS + positioned text.
    std::vector<uint8_t> statement = {
        0x1c, 2, 3,
        0xEE, 0xB0, 0x80,  // U+EC00 -> the DRCS defined in the first data unit
        'Z',
        0x0d,
        0x1c, 4, 1, 'E', 'N', 'D'
    };
    const auto body = MakeStatementBody({MakeDrcsUnit(), MakeDataUnit(0x20, statement)});

    Decoder direct(ctx);
    direct.Initialize(EncodingScheme::kARIB_STD_B24_UTF8);
    DecodeResult rd;
    auto direct_packet = MakeDirectPacket(0x1, 0, body);
    TASK_CHECK(direct.Decode(direct_packet.data(), direct_packet.size(), 4242, rd) == DecodeStatus::kGotCaption);
    TASK_CHECK(rd.caption != nullptr);

    Decoder linked = MakeEnabledDecoder(ctx);
    const auto fragments = SplitPayload(body, {7, 19});  // 3 fragments, cuts cross data units
    DecodeResult rl;
    TASK_CHECK(linked.Decode(MakeLinkPacket(0x1, 0, 0, 2, fragments[0]).data(),
                             MakeLinkPacket(0x1, 0, 0, 2, fragments[0]).size(), 4242, rl) == DecodeStatus::kNoCaption);
    TASK_CHECK(linked.Decode(MakeLinkPacket(0x1, 0, 1, 2, fragments[1]).data(),
                             MakeLinkPacket(0x1, 0, 1, 2, fragments[1]).size(), 9999, rl) == DecodeStatus::kNoCaption);
    TASK_CHECK(linked.Decode(MakeLinkPacket(0x1, 0, 2, 2, fragments[2]).data(),
                             MakeLinkPacket(0x1, 0, 2, 2, fragments[2]).size(), 8888, rl) == DecodeStatus::kGotCaption);
    TASK_CHECK(rl.caption != nullptr);

    TASK_CHECK(SameCaption(*rd.caption, *rl.caption));
    TASK_CHECK(rl.caption->drcs_map.size() == 1);
    TASK_CHECK(rl.caption->regions[0].chars[0].type == CaptionCharType::kDRCS);
    TASK_CHECK(rl.caption->regions[0].chars[0].drcs_code == 0xEC00);
    TASK_CHECK(rl.caption->pts == 4242);  // later, newer PTS values must not override
    return 0;
}

int case_management_half_packet() {
    Context ctx;
    Decoder dec = MakeEnabledDecoder(ctx);
    DecodeResult r;

    const auto mgmt = MakeManagementBody(ThreeCC("jpn"), ThreeCC("por"));
    const auto halves = SplitPayload(mgmt, {4});  // 2 fragments

    // Only the management first fragment: must not parse language tables nor mark A/B processed.
    TASK_CHECK(dec.Decode(MakeLinkPacket(0, 0, 0, 1, halves[0]).data(),
                          MakeLinkPacket(0, 0, 0, 1, halves[0]).size(), 10, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.QueryISO6392LanguageCode(LanguageId::kFirst) == 0);
    TASK_CHECK(dec.QueryISO6392LanguageCode(LanguageId::kSecond) == 0);

    // A complete management group (group bit 0) is now accepted because the half packet
    // was never acknowledged as processed.
    auto complete = MakeLinkPacket(0, 0, 0, 0, mgmt);
    TASK_CHECK(dec.Decode(complete.data(), complete.size(), 11, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.QueryISO6392LanguageCode(LanguageId::kFirst) == ThreeCC("jpn"));
    TASK_CHECK(dec.QueryISO6392LanguageCode(LanguageId::kSecond) == ThreeCC("por"));

    // Retransmission of the same complete management group is deduplicated (A/B group bit unchanged).
    TASK_CHECK(dec.Decode(complete.data(), complete.size(), 12, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.QueryISO6392LanguageCode(LanguageId::kFirst) == ThreeCC("jpn"));

    // A half management group with different languages must not touch the active language/DRCS state.
    const auto other = MakeManagementBody(ThreeCC("eng"), ThreeCC("spa"));
    const auto other_halves = SplitPayload(other, {4});

    // First establish persistent glyph (DRCS) state with a complete statement group.
    const std::vector<uint8_t> drcs_statement = {0x1c, 1, 1, 0xEE, 0xB0, 0x80, 'G'};
    const auto drcs_body = MakeStatementBody({MakeDrcsUnit(), MakeDataUnit(0x20, drcs_statement)});
    auto drcs_packet = MakeLinkPacket(0x1, 0, 0, 0, drcs_body);
    TASK_CHECK(dec.Decode(drcs_packet.data(), drcs_packet.size(), 12, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr);
    TASK_CHECK(r.caption->drcs_map.size() == 1);
    TASK_CHECK(r.caption->regions[0].chars[0].type == CaptionCharType::kDRCS);

    TASK_CHECK(dec.Decode(MakeLinkPacket(0, 1, 0, 1, other_halves[0]).data(),
                          MakeLinkPacket(0, 1, 0, 1, other_halves[0]).size(), 13, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.QueryISO6392LanguageCode(LanguageId::kFirst) == ThreeCC("jpn"));

    // The previously defined glyph still resolves: the half packet did not clear DRCS state.
    const auto reuse_body = MakeStatementBody(std::vector<uint8_t>{0x1c, 1, 1, 0xEE, 0xB0, 0x80, 'H'});
    auto reuse_packet = MakeLinkPacket(0x1, 0, 0, 0, reuse_body);
    TASK_CHECK(dec.Decode(reuse_packet.data(), reuse_packet.size(), 13, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr);
    TASK_CHECK(r.caption->drcs_map.size() == 1);
    TASK_CHECK(r.caption->regions[0].chars[0].type == CaptionCharType::kDRCS);
    TASK_CHECK(dec.QueryISO6392LanguageCode(LanguageId::kFirst) == ThreeCC("jpn"));

    // Completing a management group with the other A/B group bit applies the new languages.
    auto other_group = MakeLinkPacket(0x20, 0, 0, 0, other);  // id 0x20 -> dgi_group bit 1
    TASK_CHECK(dec.Decode(other_group.data(), other_group.size(), 14, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.QueryISO6392LanguageCode(LanguageId::kFirst) == ThreeCC("eng"));
    TASK_CHECK(dec.QueryISO6392LanguageCode(LanguageId::kSecond) == ThreeCC("spa"));
    return 0;
}

int case_language_filter_after_completion() {
    Context ctx;
    Decoder dec = MakeEnabledDecoder(ctx);  // listens on LanguageId::kFirst
    DecodeResult r;

    // Statement group whose low nibble is 2 (second language), split over two fragments.
    const auto body = MakeStatementBody(std::vector<uint8_t>{'L', '2'});
    const auto fragments = SplitPayload(body, {2});

    TASK_CHECK(dec.Decode(MakeLinkPacket(0x02, 0, 0, 1, fragments[0]).data(),
                          MakeLinkPacket(0x02, 0, 0, 1, fragments[0]).size(), 1, r) == DecodeStatus::kNoCaption);
    // Filtering runs only once the whole group is assembled: completion yields NoCaption, no caption.
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x02, 0, 1, 1, fragments[1]).data(),
                          MakeLinkPacket(0x02, 0, 1, 1, fragments[1]).size(), 2, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(!r.caption);

    // The decoder still serves the selected first language normally afterwards.
    const auto ok = MakeStatementBody(std::vector<uint8_t>{'L', '1'});
    const auto ok_fragments = SplitPayload(ok, {1});
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x01, 0, 0, 1, ok_fragments[0]).data(),
                          MakeLinkPacket(0x01, 0, 0, 1, ok_fragments[0]).size(), 3, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x01, 0, 1, 1, ok_fragments[1]).data(),
                          MakeLinkPacket(0x01, 0, 1, 1, ok_fragments[1]).size(), 4, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr && r.caption->text == "L1" && r.caption->pts == 3);
    return 0;
}

int case_pts_first_fragment_and_nopts() {
    Context ctx;
    Decoder dec = MakeEnabledDecoder(ctx);
    DecodeResult r;

    const auto body = MakeStatementBody(std::vector<uint8_t>{'H', 'I'});
    const auto fragments = SplitPayload(body, {1});

    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).data(),
                          MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).size(), PTS_NOPTS, r)
               == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).data(),
                          MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).size(), 33333, r)
               == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr);
    TASK_CHECK(r.caption->pts == PTS_NOPTS);  // unknown first PTS stays unknown, no time addition

    // The caller owns the returned caption; keep it alive across the next Decode calls.
    std::unique_ptr<Caption> first_caption = std::move(r.caption);
    const auto body2 = MakeStatementBody(std::vector<uint8_t>{'B', 'Y', 'E'});
    const auto fragments2 = SplitPayload(body2, {1});
    auto g16_first = MakeLinkPacket(0x11, 0, 0, 1, fragments2[0]);
    TASK_CHECK(dec.Decode(g16_first.data(), g16_first.size(), 1, r) == DecodeStatus::kNoCaption);
    auto g16_last = MakeLinkPacket(0x11, 0, 1, 1, fragments2[1]);
    TASK_CHECK(dec.Decode(g16_last.data(), g16_last.size(), 2, r) == DecodeStatus::kGotCaption);
    // The previously returned caption is never modified by later packets.
    TASK_CHECK(first_caption->pts == PTS_NOPTS && first_caption->text == "HI");
    TASK_CHECK(r.caption->text == "BYE" && r.caption->pts == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

int case_flush_clears_inflight() {
    Context ctx;
    Decoder dec = MakeEnabledDecoder(ctx);
    DecodeResult r;

    const auto body = MakeStatementBody(TwoLineStatement());
    const auto fragments = SplitPayload(body, {3});
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).data(),
                          MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).size(), 1, r) == DecodeStatus::kNoCaption);

    dec.Flush();

    // The continuation now has no predecessor.
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).data(),
                          MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).size(), 2, r) == DecodeStatus::kError);

    // A fresh group still decodes normally after Flush.
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x11, 0, 0, 1, fragments[0]).data(),
                          MakeLinkPacket(0x11, 0, 0, 1, fragments[0]).size(), 3, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x11, 0, 1, 1, fragments[1]).data(),
                          MakeLinkPacket(0x11, 0, 1, 1, fragments[1]).size(), 4, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr && r.caption->text == "AB\nCD");
    return 0;
}

int case_reinitialize_clears_inflight() {
    Context ctx;
    Decoder dec = MakeEnabledDecoder(ctx);
    DecodeResult r;

    const auto body = MakeStatementBody(std::vector<uint8_t>{'R', 'I'});
    const auto fragments = SplitPayload(body, {1});
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).data(),
                          MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).size(), 1, r) == DecodeStatus::kNoCaption);

    dec.Initialize(EncodingScheme::kARIB_STD_B24_UTF8);

    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).data(),
                          MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).size(), 2, r) == DecodeStatus::kError);

    // The switch state survives re-Initialize; only the in-flight groups were dropped,
    // so a fresh group can be assembled again without calling the setter.
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).data(),
                          MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).size(), 3, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).data(),
                          MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).size(), 4, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr && r.caption->text == "RI");
    return 0;
}

int case_config_changes_clear_inflight() {
    Context ctx;
    DecodeResult r;

    const auto body = MakeStatementBody(std::vector<uint8_t>{'C', 'F', 'G'});
    const auto fragments = SplitPayload(body, {1});

    // Same-value calls keep the in-flight group.
    {
        Decoder dec = MakeEnabledDecoder(ctx);
        TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).data(),
                              MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).size(), 1, r) == DecodeStatus::kNoCaption);
        dec.SetProfile(Profile::kProfileA);            // unchanged
        dec.SetEncodingScheme(EncodingScheme::kARIB_STD_B24_UTF8);  // unchanged
        dec.SwitchLanguage(LanguageId::kFirst);        // unchanged
        dec.SetCaptionType(CaptionType::kCaption);     // unchanged
        TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).data(),
                              MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).size(), 2, r) == DecodeStatus::kGotCaption);
        TASK_CHECK(r.caption != nullptr && r.caption->text == "CFG");
    }

    // Actual changes each drop the in-flight group.
    {
        Decoder dec = MakeEnabledDecoder(ctx);
        TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).data(),
                              MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).size(), 1, r) == DecodeStatus::kNoCaption);
        dec.SetProfile(Profile::kProfileC);
        TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).data(),
                              MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).size(), 2, r) == DecodeStatus::kError);
    }
    {
        Decoder dec = MakeEnabledDecoder(ctx);
        TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).data(),
                              MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).size(), 1, r) == DecodeStatus::kNoCaption);
        dec.SwitchLanguage(LanguageId::kSecond);
        TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).data(),
                              MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).size(), 2, r) == DecodeStatus::kError);
    }
    {
        Decoder dec = MakeEnabledDecoder(ctx);
        TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).data(),
                              MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).size(), 1, r) == DecodeStatus::kNoCaption);
        dec.SetEncodingScheme(EncodingScheme::kARIB_STD_B24_JIS);
        TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).data(),
                              MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).size(), 2, r) == DecodeStatus::kError);
    }
    {
        Decoder dec = MakeEnabledDecoder(ctx);
        TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).data(),
                              MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).size(), 1, r) == DecodeStatus::kNoCaption);
        dec.SetCaptionType(CaptionType::kSuperimpose);  // actual change clears in-flight groups
        dec.SetCaptionType(CaptionType::kCaption);      // back to the matching data_identifier
        // The continuation now fails for lack of a predecessor, proving the group was dropped.
        TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).data(),
                              MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).size(), 2, r) == DecodeStatus::kError);
    }
    return 0;
}

int case_toggle_switch_and_legacy() {
    Context ctx;
    Decoder dec(ctx);
    dec.Initialize(EncodingScheme::kARIB_STD_B24_UTF8);
    DecodeResult r;

    const auto body = MakeStatementBody(std::vector<uint8_t>{'T', 'G'});
    const auto fragments = SplitPayload(body, {1});

    // Disabled by default: a link fragment is rejected by the legacy parser (existing behavior).
    auto legacy_link = MakeLinkPacket(0x1, 0, 0, 1, fragments[0]);
    TASK_CHECK(dec.Decode(legacy_link.data(), legacy_link.size(), 1, r) == DecodeStatus::kError);

    dec.SetDataGroupReassemblyEnabled(true);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).data(),
                          MakeLinkPacket(0x1, 0, 0, 1, fragments[0]).size(), 2, r) == DecodeStatus::kNoCaption);

    // Same setting repeated: in-flight group is retained.
    dec.SetDataGroupReassemblyEnabled(true);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).data(),
                          MakeLinkPacket(0x1, 0, 1, 1, fragments[1]).size(), 3, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr && r.caption->text == "TG");

    // Off -> on cannot restore groups dropped while switching.
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x21, 0, 0, 1, fragments[0]).data(),
                          MakeLinkPacket(0x21, 0, 0, 1, fragments[0]).size(), 4, r) == DecodeStatus::kNoCaption);
    dec.SetDataGroupReassemblyEnabled(false);
    dec.SetDataGroupReassemblyEnabled(true);
    TASK_CHECK(dec.Decode(MakeLinkPacket(0x21, 0, 1, 1, fragments[1]).data(),
                          MakeLinkPacket(0x21, 0, 1, 1, fragments[1]).size(), 5, r) == DecodeStatus::kError);

    // Legacy complete packets keep working while the switch is off (Decode stays unchanged).
    dec.SetDataGroupReassemblyEnabled(false);
    auto direct = MakeDirectPacket(0x1, 0, body);
    TASK_CHECK(dec.Decode(direct.data(), direct.size(), 6, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption != nullptr && r.caption->text == "TG" && r.caption->pts == 6);
    return 0;
}

int case_c_api_consistency() {
    aribcc_context_t* ctx = aribcc_context_alloc();
    TASK_CHECK(ctx != nullptr);
    aribcc_decoder_t* dec = aribcc_decoder_alloc(ctx);
    TASK_CHECK(dec != nullptr);
    TASK_CHECK(aribcc_decoder_initialize(dec, ARIBCC_ENCODING_SCHEME_ARIB_STD_B24_UTF8,
                                         ARIBCC_CAPTIONTYPE_CAPTION, ARIBCC_PROFILE_A,
                                         ARIBCC_LANGUAGEID_FIRST));
    aribcc_decoder_set_data_group_reassembly_enabled(dec, true);

    const auto body = MakeStatementBody(TwoLineStatement());
    const auto fragments = SplitPayload(body, {2, 5});
    aribcc_caption_t caption;

    TASK_CHECK(aribcc_decoder_decode(dec, MakeLinkPacket(0x1, 0, 0, 2, fragments[0]).data(),
                                     MakeLinkPacket(0x1, 0, 0, 2, fragments[0]).size(),
                                     900, &caption) == ARIBCC_DECODE_STATUS_NO_CAPTION);
    TASK_CHECK(caption.text == nullptr);
    TASK_CHECK(aribcc_decoder_decode(dec, MakeLinkPacket(0x1, 0, 1, 2, fragments[1]).data(),
                                     MakeLinkPacket(0x1, 0, 1, 2, fragments[1]).size(),
                                     950, &caption) == ARIBCC_DECODE_STATUS_NO_CAPTION);
    TASK_CHECK(caption.text == nullptr);

    // Idempotent middle retransmission through the C API.
    TASK_CHECK(aribcc_decoder_decode(dec, MakeLinkPacket(0x1, 0, 1, 2, fragments[1]).data(),
                                     MakeLinkPacket(0x1, 0, 1, 2, fragments[1]).size(),
                                     960, &caption) == ARIBCC_DECODE_STATUS_NO_CAPTION);

    TASK_CHECK(aribcc_decoder_decode(dec, MakeLinkPacket(0x1, 0, 2, 2, fragments[2]).data(),
                                     MakeLinkPacket(0x1, 0, 2, 2, fragments[2]).size(),
                                     999, &caption) == ARIBCC_DECODE_STATUS_GOT_CAPTION);
    TASK_CHECK(caption.text != nullptr);
    TASK_CHECK(std::string(caption.text) == "AB\nCD");
    TASK_CHECK(caption.pts == 900);  // first fragment PTS
    TASK_CHECK(caption.region_count == 2);
    TASK_CHECK(caption.regions[0].x == 120 && caption.regions[0].y == 120);
    TASK_CHECK(caption.regions[0].char_count == 2);
    TASK_CHECK(caption.regions[0].chars[1].x == 160);
    TASK_CHECK(caption.regions[1].x == 40 && caption.regions[1].y == 240);
    aribcc_caption_cleanup(&caption);

    // Flush drops in-flight groups through the C API as well.
    TASK_CHECK(aribcc_decoder_decode(dec, MakeLinkPacket(0x11, 0, 0, 1, fragments[0]).data(),
                                     MakeLinkPacket(0x11, 0, 0, 1, fragments[0]).size(),
                                     1, &caption) == ARIBCC_DECODE_STATUS_NO_CAPTION);
    aribcc_decoder_flush(dec);
    TASK_CHECK(aribcc_decoder_decode(dec, MakeLinkPacket(0x11, 0, 1, 1, fragments[1]).data(),
                                     MakeLinkPacket(0x11, 0, 1, 1, fragments[1]).size(),
                                     2, &caption) == ARIBCC_DECODE_STATUS_ERROR);

    aribcc_decoder_free(dec);
    aribcc_context_free(ctx);
    return 0;
}

}  // namespace

// C linkage registry consumed by task_support/driver.c
extern "C" {

const struct task_case task_cases[] = {
    {"link_transport", "three_fragment_interleave", case_three_fragment_interleave},
    {"link_transport", "first_fragment_retransmit", case_first_fragment_retransmit},
    {"link_transport", "middle_fragment_retransmit", case_middle_fragment_retransmit},
    {"link_transport", "new_version_replaces", case_new_version_replaces},
    {"link_transport", "error_isolation", case_error_isolation},
    {"link_transport", "single_fragment_group", case_single_fragment_group},
    {"link_transport", "assembled_size_limit", case_assembled_size_limit},

    {"publish_barrier", "no_publish_before_complete", case_no_publish_before_complete},
    {"publish_barrier", "equivalence_with_direct_decode", case_equivalence_with_direct_decode},
    {"publish_barrier", "management_half_packet", case_management_half_packet},
    {"publish_barrier", "language_filter_after_completion", case_language_filter_after_completion},
    {"publish_barrier", "pts_first_fragment_and_nopts", case_pts_first_fragment_and_nopts},

    {"lifecycle", "flush_clears_inflight", case_flush_clears_inflight},
    {"lifecycle", "reinitialize_clears_inflight", case_reinitialize_clears_inflight},
    {"lifecycle", "config_changes_clear_inflight", case_config_changes_clear_inflight},
    {"lifecycle", "toggle_switch_and_legacy", case_toggle_switch_and_legacy},
    {"lifecycle", "c_api_consistency", case_c_api_consistency},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

}  // extern "C"
