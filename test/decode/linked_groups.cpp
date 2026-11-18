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

// Regression tests for the optional linked data group reassembly mode
// (Decoder::SetDataGroupReassemblyEnabled / aribcc_decoder_set_data_group_reassembly_enabled).
//
// All caption packets are handcrafted: each PES carries exactly one link fragment
// (see task_support/link_contract.md) with a proper CRC-16.

#include "task_support/check.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "aribcaption/aribcaption.hpp"
#include "aribcaption/caption.h"
#include "aribcaption/context.h"
#include "aribcaption/decoder.h"

using namespace aribcaption;

namespace {

// CRC-16/CCITT: polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR
uint16_t Crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

// Build a PES packet carrying exactly one link fragment (input for reassembly mode):
// 80 ff f0 | id(6)|version(2) | link | last | payload length(2, BE) | payload | CRC16(2, BE)
std::vector<uint8_t> MakeFragment(uint8_t group_id, uint8_t version, uint8_t link, uint8_t last,
                                  const std::vector<uint8_t>& payload, uint8_t data_identifier = 0x80) {
    std::vector<uint8_t> packet;
    packet.push_back(data_identifier);
    packet.push_back(0xFF);
    packet.push_back(0xF0);  // PES_data_packet_header_length = 0
    packet.push_back(static_cast<uint8_t>((group_id << 2) | (version & 0x03)));
    packet.push_back(link);
    packet.push_back(last);
    packet.push_back(static_cast<uint8_t>(payload.size() >> 8));
    packet.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    packet.insert(packet.end(), payload.begin(), payload.end());
    uint16_t crc = Crc16(packet.data() + 3, packet.size() - 3);
    packet.push_back(static_cast<uint8_t>(crc >> 8));
    packet.push_back(static_cast<uint8_t>(crc & 0xFF));
    return packet;
}

// Build a classic PES packet carrying one complete data group (for reference decoding
// through the original non-reassembling path)
std::vector<uint8_t> MakeClassic(uint8_t group_id, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> packet;
    packet.push_back(0x80);
    packet.push_back(0xFF);
    packet.push_back(0xF0);
    packet.push_back(static_cast<uint8_t>(group_id << 2));
    packet.push_back(0);
    packet.push_back(0);
    packet.push_back(static_cast<uint8_t>(body.size() >> 8));
    packet.push_back(static_cast<uint8_t>(body.size() & 0xFF));
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

std::vector<uint8_t> DataUnit(uint8_t parameter, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> unit;
    unit.push_back(0x1F);  // unit separator
    unit.push_back(parameter);
    unit.push_back(static_cast<uint8_t>(data.size() >> 16));
    unit.push_back(static_cast<uint8_t>(data.size() >> 8));
    unit.push_back(static_cast<uint8_t>(data.size()));
    unit.insert(unit.end(), data.begin(), data.end());
    return unit;
}

// Caption statement data body: TMD = 0, then a data unit loop
std::vector<uint8_t> StatementBodyUnits(const std::vector<std::vector<uint8_t>>& units) {
    size_t loop_length = 0;
    for (const auto& unit : units)
        loop_length += unit.size();
    std::vector<uint8_t> body;
    body.push_back(0x00);  // TMD
    body.push_back(static_cast<uint8_t>(loop_length >> 16));
    body.push_back(static_cast<uint8_t>(loop_length >> 8));
    body.push_back(static_cast<uint8_t>(loop_length));
    for (const auto& unit : units)
        body.insert(body.end(), unit.begin(), unit.end());
    return body;
}

std::vector<uint8_t> StatementBody(const std::vector<uint8_t>& statement_data) {
    return StatementBodyUnits({DataUnit(0x20, statement_data)});
}

// Caption management data body with a single language (language_tag 0), no data units
std::vector<uint8_t> ManagementBody(uint8_t format, const char* iso6392_3cc) {
    std::vector<uint8_t> body;
    body.push_back(0x00);  // TMD
    body.push_back(0x01);  // num_languages
    body.push_back(0x00);  // language_tag = 0, DMF = 0
    body.push_back(static_cast<uint8_t>(iso6392_3cc[0]));
    body.push_back(static_cast<uint8_t>(iso6392_3cc[1]));
    body.push_back(static_cast<uint8_t>(iso6392_3cc[2]));
    body.push_back(static_cast<uint8_t>((format << 4) | (1 << 2)));  // format, TCS = 1
    body.push_back(0x00);
    body.push_back(0x00);
    body.push_back(0x00);  // data_unit_loop_length = 0
    return body;
}

// Statement text: APS (active position set) to character cell (x, y), then UTF-8 text
std::vector<uint8_t> ApsText(int x, int y, const char* text) {
    std::vector<uint8_t> data;
    data.push_back(0x1C);  // C0::APS
    data.push_back(static_cast<uint8_t>(y));
    data.push_back(static_cast<uint8_t>(x));
    while (*text)
        data.push_back(static_cast<uint8_t>(*text++));
    return data;
}

// DRCS-2 data unit defining one 16x18 1-bit bitmap character
std::vector<uint8_t> Drcs2Unit(uint16_t character_code) {
    std::vector<uint8_t> data;
    data.push_back(0x01);  // number_of_code
    data.push_back(static_cast<uint8_t>(character_code >> 8));
    data.push_back(static_cast<uint8_t>(character_code & 0xFF));
    data.push_back(0x01);  // number_of_font
    data.push_back(0x00);  // font_id = 0, mode = 0 (bitmap)
    data.push_back(0x00);  // depth = 0 + 2 (1 bit per pixel)
    data.push_back(0x10);  // width = 16
    data.push_back(0x12);  // height = 18
    for (int i = 0; i < 36; i++)  // 16 * 18 * 1 / 8 = 36 bytes of bitmap
        data.push_back(0xAA);
    return DataUnit(0x31, data);
}

std::vector<uint8_t> Slice(const std::vector<uint8_t>& data, size_t begin, size_t end) {
    return std::vector<uint8_t>(data.begin() + begin, data.begin() + end);
}

DecodeStatus Decode(Decoder& decoder, const std::vector<uint8_t>& packet, int64_t pts, DecodeResult& result) {
    return decoder.Decode(packet.data(), packet.size(), pts, result);
}

DecodeStatus DecodeClassic(Decoder& decoder, uint8_t group_id, const std::vector<uint8_t>& body,
                           int64_t pts, DecodeResult& result) {
    return Decode(decoder, MakeClassic(group_id, body), pts, result);
}

// ---------------------------------------------------------------------------
// group: link_transport
// ---------------------------------------------------------------------------

// Three-fragment statement interleaved with a two-fragment management group and an
// unrelated language group; the completed caption must match direct decoding of the
// same assembled body, with the PTS of the first fragment.
int case_interleaved_three_fragments() {
    Context context;
    Decoder decoder(context);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));
    decoder.SetDataGroupReassemblyEnabled(true);

    Decoder reference(context);
    TASK_CHECK(reference.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));

    DecodeResult result;

    auto body = StatementBody(ApsText(2, 1, "AB"));
    auto s0 = MakeFragment(1, 0, 0, 2, Slice(body, 0, 5));
    auto s1 = MakeFragment(1, 0, 1, 2, Slice(body, 5, 10));
    auto s2 = MakeFragment(1, 0, 2, 2, Slice(body, 10, body.size()));

    auto mgmt = ManagementBody(8, "jpn");
    auto m0 = MakeFragment(0, 0, 0, 1, Slice(mgmt, 0, 6));
    auto m1 = MakeFragment(0, 0, 1, 1, Slice(mgmt, 6, mgmt.size()));

    // single-fragment statement of the other language id, must be filtered out on completion
    auto other = MakeFragment(2, 0, 0, 0, StatementBody(ApsText(0, 0, "ZZ")));

    TASK_CHECK(Decode(decoder, m0, 5000, result) == DecodeStatus::kNoCaption && !result.caption);
    TASK_CHECK(Decode(decoder, s0, 1000, result) == DecodeStatus::kNoCaption && !result.caption);
    TASK_CHECK(Decode(decoder, other, 700, result) == DecodeStatus::kNoCaption && !result.caption);
    TASK_CHECK(Decode(decoder, m1, 5100, result) == DecodeStatus::kNoCaption && !result.caption);
    TASK_CHECK(decoder.QueryISO6392LanguageCode(LanguageId::kFirst) == ThreeCC("jpn"));
    TASK_CHECK(Decode(decoder, s1, 1100, result) == DecodeStatus::kNoCaption && !result.caption);
    TASK_CHECK(Decode(decoder, s2, 1200, result) == DecodeStatus::kGotCaption && result.caption);

    // reference: decode the same assembled bodies directly, without reassembly
    DecodeResult ref_mgmt;
    TASK_CHECK(DecodeClassic(reference, 0, mgmt, 5000, ref_mgmt) == DecodeStatus::kNoCaption);
    DecodeResult ref_result;
    TASK_CHECK(DecodeClassic(reference, 1, body, 1000, ref_result) == DecodeStatus::kGotCaption && ref_result.caption);

    Caption& caption = *result.caption;
    Caption& expected = *ref_result.caption;
    TASK_CHECK(caption.text == "AB");
    TASK_CHECK(caption.text == expected.text);
    TASK_CHECK(caption.pts == 1000);  // PTS comes from the first fragment
    TASK_CHECK(caption.pts == expected.pts);
    TASK_CHECK(caption.iso6392_language_code == ThreeCC("jpn"));
    TASK_CHECK(caption.iso6392_language_code == expected.iso6392_language_code);
    TASK_CHECK(caption.plane_width == 960 && caption.plane_height == 540);
    TASK_CHECK(caption.regions.size() == 1 && expected.regions.size() == 1);
    TASK_CHECK(caption.regions[0].x == 80 && caption.regions[0].y == 60);
    TASK_CHECK(caption.regions[0].width == 80 && caption.regions[0].height == 60);
    TASK_CHECK(caption.regions[0].x == expected.regions[0].x);
    TASK_CHECK(caption.regions[0].y == expected.regions[0].y);
    TASK_CHECK(caption.regions[0].width == expected.regions[0].width);
    TASK_CHECK(caption.regions[0].height == expected.regions[0].height);
    TASK_CHECK(caption.regions[0].chars.size() == 2);
    TASK_CHECK(caption.regions[0].chars[0].x == 80 && caption.regions[0].chars[0].y == 60);
    TASK_CHECK(caption.regions[0].chars[1].x == 120 && caption.regions[0].chars[1].y == 60);
    TASK_CHECK(caption.regions[0].chars[0].codepoint == 'A');
    TASK_CHECK(caption.regions[0].chars[1].codepoint == 'B');

    // an already returned caption owns its data and must not be modified by later decoding
    std::unique_ptr<Caption> kept = std::move(result.caption);
    auto more = MakeFragment(1, 0, 0, 0, StatementBody(ApsText(0, 0, "Q")));
    TASK_CHECK(Decode(decoder, more, 2000, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(kept->text == "AB" && kept->pts == 1000);
    TASK_CHECK(kept->regions[0].x == 80 && kept->regions[0].chars.size() == 2);
    return 0;
}

// Idempotent retransmission of the first and a middle fragment: no duplicated payload,
// and the first fragment's PTS is kept even if the retransmission arrives with a newer PTS.
int case_retransmit_first_and_middle() {
    Context context;
    Decoder decoder(context);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));
    decoder.SetDataGroupReassemblyEnabled(true);

    DecodeResult result;
    auto body = StatementBody(ApsText(1, 1, "ABC"));
    auto f0 = MakeFragment(1, 0, 0, 2, Slice(body, 0, 5));
    auto f1 = MakeFragment(1, 0, 1, 2, Slice(body, 5, 10));
    auto f2 = MakeFragment(1, 0, 2, 2, Slice(body, 10, body.size()));

    TASK_CHECK(Decode(decoder, f0, 1000, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f0, 5000, result) == DecodeStatus::kNoCaption);  // retransmitted first fragment
    TASK_CHECK(Decode(decoder, f1, 1100, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f1, 1200, result) == DecodeStatus::kNoCaption);  // retransmitted middle fragment
    TASK_CHECK(Decode(decoder, f2, 1300, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->text == "ABC");  // fragments were not appended twice
    TASK_CHECK(result.caption->regions[0].chars.size() == 3);
    TASK_CHECK(result.caption->pts == 1000);    // PTS of the first fragment, not of the retransmission
    return 0;
}

// Conflicting retransmission, version/last_link_number changes and out-of-order fragments
// are errors and drop the affected group; the group can be restarted afterwards.
int case_conflicting_retransmit_drops_group() {
    Context context;
    Decoder decoder(context);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));
    decoder.SetDataGroupReassemblyEnabled(true);

    DecodeResult result;
    auto body = StatementBody(ApsText(1, 1, "AB"));
    auto f0 = MakeFragment(1, 0, 0, 2, Slice(body, 0, 5));
    auto f1 = MakeFragment(1, 0, 1, 2, Slice(body, 5, 10));
    auto f2 = MakeFragment(1, 0, 2, 2, Slice(body, 10, body.size()));

    // same link number as the most recent fragment but different payload
    auto f1_conflicting = MakeFragment(1, 0, 1, 2, std::vector<uint8_t>{0x55, 0x66});

    TASK_CHECK(Decode(decoder, f0, 100, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f1, 200, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f1_conflicting, 300, result) == DecodeStatus::kError && !result.caption);
    TASK_CHECK(Decode(decoder, f2, 400, result) == DecodeStatus::kError);  // group was dropped
    TASK_CHECK(Decode(decoder, f0, 500, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f1, 600, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f2, 700, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->text == "AB" && result.caption->pts == 500);

    // version change on a non-first fragment
    TASK_CHECK(Decode(decoder, f0, 800, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, MakeFragment(1, 1, 1, 2, Slice(body, 5, 10)), 900, result) == DecodeStatus::kError);
    TASK_CHECK(Decode(decoder, f1, 950, result) == DecodeStatus::kError);  // group was dropped

    // last_link_number change on a non-first fragment
    TASK_CHECK(Decode(decoder, f0, 1000, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 1, 3, Slice(body, 5, 10)), 1100, result) == DecodeStatus::kError);

    // out-of-order fragment (link 2 while link 1 is expected)
    TASK_CHECK(Decode(decoder, f0, 1200, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f2, 1300, result) == DecodeStatus::kError);

    // non-first fragment without any in-flight group
    TASK_CHECK(Decode(decoder, f1, 1400, result) == DecodeStatus::kError);
    return 0;
}

// A legal first fragment replaces an incomplete group of the same id, including a first
// fragment carrying a new version; the replacement's PTS becomes the group's PTS.
int case_new_version_replaces_inflight() {
    Context context;
    Decoder decoder(context);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));
    decoder.SetDataGroupReassemblyEnabled(true);

    DecodeResult result;
    auto body_ab = StatementBody(ApsText(1, 1, "AB"));
    auto body_cd = StatementBody(ApsText(1, 1, "CD"));
    auto body_ef = StatementBody(ApsText(1, 1, "EF"));

    // split after the first text character, so that first fragments of different
    // bodies never share the same payload (which would be an idempotent retransmission)
    const size_t cut = 13;

    // first fragment with version 0, then a new-version first fragment replaces it
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 0, 1, Slice(body_ab, 0, cut)), 100, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, MakeFragment(1, 1, 0, 1, Slice(body_cd, 0, cut)), 200, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, MakeFragment(1, 1, 1, 1, Slice(body_cd, cut, body_cd.size())), 300, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->text == "CD");
    TASK_CHECK(result.caption->pts == 200);  // PTS of the replacement first fragment

    // a same-version first fragment also replaces an incomplete group
    TASK_CHECK(Decode(decoder, MakeFragment(1, 1, 0, 1, Slice(body_ab, 0, cut)), 400, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, MakeFragment(1, 1, 0, 1, Slice(body_ef, 0, cut)), 500, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, MakeFragment(1, 1, 1, 1, Slice(body_ef, cut, body_ef.size())), 600, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->text == "EF");
    TASK_CHECK(result.caption->pts == 500);
    return 0;
}

// Transport errors drop only the affected group; other in-flight groups continue,
// and a bad header without a readable group id keeps every group.
int case_error_isolates_other_groups() {
    Context context;
    Decoder decoder(context);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));
    decoder.SetDataGroupReassemblyEnabled(true);

    DecodeResult result;
    auto body = StatementBody(ApsText(1, 1, "AB"));
    auto mgmt = ManagementBody(8, "jpn");

    // statement group (id 1) and management group (id 0) in flight together
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 0, 1, Slice(body, 0, 5)), 100, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, MakeFragment(0, 0, 0, 1, Slice(mgmt, 0, 6)), 200, result) == DecodeStatus::kNoCaption);

    // CRC mismatch on group 1 drops only that group
    auto bad_crc = MakeFragment(1, 0, 1, 1, Slice(body, 5, body.size()));
    bad_crc.back() ^= 0xFF;
    TASK_CHECK(Decode(decoder, bad_crc, 300, result) == DecodeStatus::kError);
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 1, 1, Slice(body, 5, body.size())), 310, result) == DecodeStatus::kError);

    // the management group is unaffected and completes
    TASK_CHECK(Decode(decoder, MakeFragment(0, 0, 1, 1, Slice(mgmt, 6, mgmt.size())), 400, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(decoder.QueryISO6392LanguageCode(LanguageId::kFirst) == ThreeCC("jpn"));

    // a bad header without a readable group id keeps the in-flight group
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 0, 1, Slice(body, 0, 5)), 500, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, std::vector<uint8_t>{0x80, 0xFF, 0xF0}, 600, result) == DecodeStatus::kError);
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 1, 1, Slice(body, 5, body.size())), 700, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->text == "AB" && result.caption->pts == 500);

    // a truncated link header drops only the addressed group
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 0, 1, Slice(body, 0, 5)), 800, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, MakeFragment(0x20, 0, 0, 1, Slice(mgmt, 0, 6)), 900, result) == DecodeStatus::kNoCaption);
    std::vector<uint8_t> truncated = {0x80, 0xFF, 0xF0, static_cast<uint8_t>(1 << 2), 0x01};
    TASK_CHECK(Decode(decoder, truncated, 1000, result) == DecodeStatus::kError);
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 1, 1, Slice(body, 5, body.size())), 1100, result) == DecodeStatus::kError);
    TASK_CHECK(Decode(decoder, MakeFragment(0x20, 0, 1, 1, Slice(mgmt, 6, mgmt.size())), 1200, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(decoder.QueryISO6392LanguageCode(LanguageId::kFirst) == ThreeCC("jpn"));
    return 0;
}

// The first fragment may carry an unknown PTS; the completed caption keeps PTS_NOPTS
// instead of adopting a later fragment's timestamp.
int case_first_fragment_nopts() {
    Context context;
    Decoder decoder(context);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));
    decoder.SetDataGroupReassemblyEnabled(true);

    DecodeResult result;
    auto body = StatementBody(ApsText(1, 1, "AB"));
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 0, 2, Slice(body, 0, 5)), PTS_NOPTS, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 1, 2, Slice(body, 5, 10)), 100, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 2, 2, Slice(body, 10, body.size())), 200, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->text == "AB");
    TASK_CHECK(result.caption->pts == PTS_NOPTS);
    return 0;
}

// ---------------------------------------------------------------------------
// group: publish_barrier
// ---------------------------------------------------------------------------

// A half-received management group must not change the current language table or
// writing format; the A/B dedup of complete management groups still applies, and a
// management first fragment alone does not mark its group as processed.
int case_management_half_keeps_language_and_format() {
    Context context;
    Decoder decoder(context);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));
    decoder.SetDataGroupReassemblyEnabled(true);

    DecodeResult result;
    auto mgmt_a_jpn = ManagementBody(8, "jpn");    // format 8 => 960x540, 40x60 cells
    auto mgmt_a_eng = ManagementBody(10, "eng");   // same group A id, different content
    auto mgmt_b_eng = ManagementBody(10, "eng");   // group B: format 10 => 720x480, 40x52 cells

    // complete management group A takes effect
    TASK_CHECK(Decode(decoder, MakeFragment(0, 0, 0, 0, mgmt_a_jpn), 100, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(decoder.QueryISO6392LanguageCode(LanguageId::kFirst) == ThreeCC("jpn"));

    // A/B dedup: another complete group A counts as retransmission and is ignored
    TASK_CHECK(Decode(decoder, MakeFragment(0, 0, 0, 0, mgmt_a_eng), 200, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(decoder.QueryISO6392LanguageCode(LanguageId::kFirst) == ThreeCC("jpn"));

    // only the first fragment of management group B arrives (it carries the whole
    // language entry prefix, but must not be interpreted yet)
    TASK_CHECK(Decode(decoder, MakeFragment(0x20, 0, 0, 1, Slice(mgmt_b_eng, 0, 7)), 300, result) == DecodeStatus::kNoCaption && !result.caption);

    // language and writing format are still the old ones
    TASK_CHECK(decoder.QueryISO6392LanguageCode(LanguageId::kFirst) == ThreeCC("jpn"));
    auto st1 = StatementBody(ApsText(2, 1, "A"));
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 0, 0, st1), 400, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->iso6392_language_code == ThreeCC("jpn"));
    TASK_CHECK(result.caption->plane_width == 960 && result.caption->plane_height == 540);
    TASK_CHECK(result.caption->regions[0].x == 80 && result.caption->regions[0].y == 60);
    TASK_CHECK(result.caption->regions[0].height == 60);

    // the half-received group B was not marked as processed; group A is still the last
    // completed management group, so another complete A is still deduped
    TASK_CHECK(Decode(decoder, MakeFragment(0, 0, 0, 0, mgmt_a_eng), 500, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(decoder.QueryISO6392LanguageCode(LanguageId::kFirst) == ThreeCC("jpn"));

    // completing group B applies the new language and writing format
    TASK_CHECK(Decode(decoder, MakeFragment(0x20, 0, 1, 1, Slice(mgmt_b_eng, 7, mgmt_b_eng.size())), 600, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(decoder.QueryISO6392LanguageCode(LanguageId::kFirst) == ThreeCC("eng"));
    auto st2 = StatementBody(ApsText(2, 1, "B"));
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 0, 0, st2), 700, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->iso6392_language_code == ThreeCC("eng"));
    TASK_CHECK(result.caption->plane_width == 720 && result.caption->plane_height == 480);
    TASK_CHECK(result.caption->regions[0].x == 80 && result.caption->regions[0].y == 52);
    TASK_CHECK(result.caption->regions[0].height == 52);
    return 0;
}

// A half-received statement group must not register DRCS glyphs or apply display
// controls; both take effect only once the group completes.
int case_statement_half_no_side_effects() {
    Context context;
    Decoder decoder(context);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));
    decoder.SetDataGroupReassemblyEnabled(true);

    DecodeResult result;

    // statement group: DRCS-2 unit for U+EC01, then a statement unit "RDF + AB"
    std::vector<uint8_t> text = {0xC2, 0x81, 'A', 'B'};  // RDF (red foreground) in UTF-8 mode, then "AB"
    auto body = StatementBodyUnits({Drcs2Unit(0xEC01), DataUnit(0x20, text)});

    // cut the body right after the RDF control inside the statement unit
    const size_t cut = 1 + 3 + (5 + 44) + 5 + 2;
    auto f0 = MakeFragment(1, 0, 0, 1, Slice(body, 0, cut));
    auto f1 = MakeFragment(1, 0, 1, 1, Slice(body, cut, body.size()));

    TASK_CHECK(Decode(decoder, f0, 100, result) == DecodeStatus::kNoCaption && !result.caption);

    // probe with a complete single-fragment statement referencing U+EC01:
    // the half group's DRCS and red foreground must not be visible
    std::vector<uint8_t> probe = {0xEE, 0xB0, 0x81, 'Z'};  // U+EC01 in UTF-8, then 'Z'
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 0, 0, StatementBody(probe)), 200, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->text == "\xE3\x80\x93Z");  // Geta mark (U+3013) + Z
    TASK_CHECK(result.caption->drcs_map.empty());
    TASK_CHECK(result.caption->regions[0].chars.size() == 2);
    TASK_CHECK(result.caption->regions[0].chars[0].type == CaptionCharType::kText);
    TASK_CHECK(result.caption->regions[0].chars[0].codepoint == 0x3013);
    TASK_CHECK(result.caption->regions[0].chars[0].text_color.u32 == ColorRGBA(255, 255, 255, 255).u32);
    TASK_CHECK(result.caption->regions[0].chars[1].text_color.u32 == ColorRGBA(255, 255, 255, 255).u32);

    // deliver the split group for real; the red foreground applies to the whole caption
    TASK_CHECK(Decode(decoder, f0, 300, result) == DecodeStatus::kNoCaption && !result.caption);
    TASK_CHECK(Decode(decoder, f1, 400, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->text == "AB");
    TASK_CHECK(result.caption->regions[0].chars.size() == 2);
    TASK_CHECK(result.caption->regions[0].chars[0].text_color.u32 == ColorRGBA(255, 0, 0, 255).u32);
    TASK_CHECK(result.caption->regions[0].chars[1].text_color.u32 == ColorRGBA(255, 0, 0, 255).u32);

    // the DRCS glyph was registered only after the group completed
    TASK_CHECK(Decode(decoder, MakeFragment(1, 0, 0, 0, StatementBody(probe)), 500, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->drcs_map.count(0xEC01) == 1);
    TASK_CHECK(result.caption->regions[0].chars[0].type != CaptionCharType::kText);
    return 0;
}

// ---------------------------------------------------------------------------
// group: lifecycle
// ---------------------------------------------------------------------------

// Flush and toggling the switch drop in-flight groups; while disabled, Decode keeps
// its original behavior.
int case_flush_and_toggle() {
    Context context;
    Decoder decoder(context);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));
    decoder.SetDataGroupReassemblyEnabled(true);

    DecodeResult result;
    auto body = StatementBody(ApsText(1, 1, "AB"));
    auto f0 = MakeFragment(1, 0, 0, 2, Slice(body, 0, 5));
    auto f1 = MakeFragment(1, 0, 1, 2, Slice(body, 5, 10));
    auto f2 = MakeFragment(1, 0, 2, 2, Slice(body, 10, body.size()));

    // Flush drops the in-flight group
    TASK_CHECK(Decode(decoder, f0, 100, result) == DecodeStatus::kNoCaption);
    decoder.Flush();
    TASK_CHECK(Decode(decoder, f1, 200, result) == DecodeStatus::kError);
    TASK_CHECK(Decode(decoder, f0, 300, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f1, 400, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f2, 500, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->text == "AB" && result.caption->pts == 300);

    // switching off and on again does not restore old groups
    TASK_CHECK(Decode(decoder, f0, 600, result) == DecodeStatus::kNoCaption);
    decoder.SetDataGroupReassemblyEnabled(false);
    decoder.SetDataGroupReassemblyEnabled(true);
    TASK_CHECK(Decode(decoder, f1, 700, result) == DecodeStatus::kError);

    // while disabled, Decode expects one complete data group per PES
    decoder.SetDataGroupReassemblyEnabled(false);
    TASK_CHECK(DecodeClassic(decoder, 1, body, 800, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->text == "AB" && result.caption->pts == 800);
    TASK_CHECK(Decode(decoder, f0, 900, result) == DecodeStatus::kError);  // a lone fragment is not a whole group

    // re-enabling starts from a clean transport state again
    decoder.SetDataGroupReassemblyEnabled(true);
    TASK_CHECK(Decode(decoder, f0, 1000, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f1, 1100, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f2, 1200, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->text == "AB" && result.caption->pts == 1000);
    return 0;
}

// Repeating the same settings must keep in-flight groups.
int case_same_settings_keep_inflight() {
    Context context;
    Decoder decoder(context);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));
    decoder.SetDataGroupReassemblyEnabled(true);

    DecodeResult result;
    auto body = StatementBody(ApsText(1, 1, "AB"));
    auto f0 = MakeFragment(1, 0, 0, 2, Slice(body, 0, 5));
    auto f1 = MakeFragment(1, 0, 1, 2, Slice(body, 5, 10));
    auto f2 = MakeFragment(1, 0, 2, 2, Slice(body, 10, body.size()));

    TASK_CHECK(Decode(decoder, f0, 100, result) == DecodeStatus::kNoCaption);
    decoder.SetDataGroupReassemblyEnabled(true);
    decoder.SetProfile(Profile::kProfileA);
    decoder.SetCaptionType(CaptionType::kCaption);
    decoder.SwitchLanguage(LanguageId::kFirst);
    decoder.SetEncodingScheme(EncodingScheme::kARIB_STD_B24_UTF8);
    TASK_CHECK(Decode(decoder, f1, 200, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f2, 300, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->text == "AB" && result.caption->pts == 100);
    return 0;
}

// Actual changes of profile, language, caption type, encoding scheme, and re-Initialize
// drop in-flight groups.
int case_config_changes_clear_inflight() {
    Context context;
    Decoder decoder(context);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));
    decoder.SetDataGroupReassemblyEnabled(true);

    DecodeResult result;
    auto body = StatementBody(ApsText(1, 1, "AB"));
    auto f0 = MakeFragment(1, 0, 0, 1, Slice(body, 0, 5));
    auto f1 = MakeFragment(1, 0, 1, 1, Slice(body, 5, body.size()));

    // profile change
    TASK_CHECK(Decode(decoder, f0, 100, result) == DecodeStatus::kNoCaption);
    decoder.SetProfile(Profile::kProfileC);
    TASK_CHECK(Decode(decoder, f1, 200, result) == DecodeStatus::kError);
    decoder.SetProfile(Profile::kProfileA);
    TASK_CHECK(Decode(decoder, f0, 300, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f1, 400, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->text == "AB");

    // language switch
    TASK_CHECK(Decode(decoder, f0, 500, result) == DecodeStatus::kNoCaption);
    decoder.SwitchLanguage(LanguageId::kSecond);
    TASK_CHECK(Decode(decoder, f1, 600, result) == DecodeStatus::kError);
    decoder.SwitchLanguage(LanguageId::kFirst);
    TASK_CHECK(Decode(decoder, f0, 700, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f1, 800, result) == DecodeStatus::kGotCaption && result.caption);

    // caption type change (the follow-up fragment uses the matching data_identifier,
    // so the error must come from the dropped group, not a type mismatch)
    TASK_CHECK(Decode(decoder, f0, 900, result) == DecodeStatus::kNoCaption);
    decoder.SetCaptionType(CaptionType::kSuperimpose);
    auto f1_superimpose = MakeFragment(1, 0, 1, 1, Slice(body, 5, body.size()), 0x81);
    TASK_CHECK(Decode(decoder, f1_superimpose, 1000, result) == DecodeStatus::kError);
    decoder.SetCaptionType(CaptionType::kCaption);
    TASK_CHECK(Decode(decoder, f0, 1100, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f1, 1200, result) == DecodeStatus::kGotCaption && result.caption);

    // encoding scheme change
    TASK_CHECK(Decode(decoder, f0, 1300, result) == DecodeStatus::kNoCaption);
    decoder.SetEncodingScheme(EncodingScheme::kARIB_STD_B24_JIS);
    TASK_CHECK(Decode(decoder, f1, 1400, result) == DecodeStatus::kError);
    decoder.SetEncodingScheme(EncodingScheme::kARIB_STD_B24_UTF8);
    TASK_CHECK(Decode(decoder, f0, 1500, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f1, 1600, result) == DecodeStatus::kGotCaption && result.caption);

    // re-Initialize
    TASK_CHECK(Decode(decoder, f0, 1700, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));
    TASK_CHECK(Decode(decoder, f1, 1800, result) == DecodeStatus::kError);
    TASK_CHECK(Decode(decoder, f0, 1900, result) == DecodeStatus::kNoCaption);
    TASK_CHECK(Decode(decoder, f1, 2000, result) == DecodeStatus::kGotCaption && result.caption);
    TASK_CHECK(result.caption->text == "AB" && result.caption->pts == 1900);
    return 0;
}

// The C API behaves the same as the C++ API.
int case_c_api() {
    aribcc_context_t* context = aribcc_context_alloc();
    TASK_CHECK(context != nullptr);
    aribcc_decoder_t* decoder = aribcc_decoder_alloc(context);
    TASK_CHECK(decoder != nullptr);
    TASK_CHECK(aribcc_decoder_initialize(decoder,
                                         ARIBCC_ENCODING_SCHEME_ARIB_STD_B24_UTF8,
                                         ARIBCC_CAPTIONTYPE_DEFAULT,
                                         ARIBCC_PROFILE_DEFAULT,
                                         ARIBCC_LANGUAGEID_DEFAULT));

    auto body = StatementBody(ApsText(2, 1, "AB"));
    auto f0 = MakeFragment(1, 0, 0, 2, Slice(body, 0, 5));
    auto f1 = MakeFragment(1, 0, 1, 2, Slice(body, 5, 10));
    auto f2 = MakeFragment(1, 0, 2, 2, Slice(body, 10, body.size()));

    aribcc_caption_t caption;

    // reassembly is disabled by default: a lone fragment is an error
    TASK_CHECK(aribcc_decoder_decode(decoder, f0.data(), f0.size(), 100, &caption) == ARIBCC_DECODE_STATUS_ERROR);
    TASK_CHECK(caption.text == nullptr && caption.regions == nullptr);

    aribcc_decoder_set_data_group_reassembly_enabled(decoder, true);
    TASK_CHECK(aribcc_decoder_decode(decoder, f0.data(), f0.size(), 777, &caption) == ARIBCC_DECODE_STATUS_NO_CAPTION);
    TASK_CHECK(caption.text == nullptr && caption.region_count == 0);
    TASK_CHECK(aribcc_decoder_decode(decoder, f1.data(), f1.size(), 888, &caption) == ARIBCC_DECODE_STATUS_NO_CAPTION);
    TASK_CHECK(caption.text == nullptr && caption.region_count == 0);
    TASK_CHECK(aribcc_decoder_decode(decoder, f2.data(), f2.size(), 999, &caption) == ARIBCC_DECODE_STATUS_GOT_CAPTION);
    TASK_CHECK(caption.text != nullptr && std::strcmp(caption.text, "AB") == 0);
    TASK_CHECK(caption.pts == 777);  // PTS of the first fragment
    TASK_CHECK(caption.plane_width == 960 && caption.plane_height == 540);
    TASK_CHECK(caption.region_count == 1);
    TASK_CHECK(caption.regions[0].x == 80 && caption.regions[0].y == 60);
    TASK_CHECK(caption.regions[0].width == 80 && caption.regions[0].height == 60);
    TASK_CHECK(caption.regions[0].char_count == 2);
    TASK_CHECK(caption.regions[0].chars[0].x == 80 && caption.regions[0].chars[0].y == 60);
    TASK_CHECK(caption.regions[0].chars[1].x == 120 && caption.regions[0].chars[1].y == 60);
    aribcc_caption_cleanup(&caption);

    // flush via the C API drops in-flight groups
    TASK_CHECK(aribcc_decoder_decode(decoder, f0.data(), f0.size(), 100, &caption) == ARIBCC_DECODE_STATUS_NO_CAPTION);
    aribcc_decoder_flush(decoder);
    TASK_CHECK(aribcc_decoder_decode(decoder, f1.data(), f1.size(), 200, &caption) == ARIBCC_DECODE_STATUS_ERROR);

    aribcc_decoder_free(decoder);
    aribcc_context_free(context);
    return 0;
}

}  // anonymous namespace

extern "C" {

const struct task_case task_cases[] = {
    {"link_transport", "interleaved_three_fragments", &case_interleaved_three_fragments},
    {"link_transport", "retransmit_first_and_middle", &case_retransmit_first_and_middle},
    {"link_transport", "conflicting_retransmit_drops_group", &case_conflicting_retransmit_drops_group},
    {"link_transport", "new_version_replaces_inflight", &case_new_version_replaces_inflight},
    {"link_transport", "error_isolates_other_groups", &case_error_isolates_other_groups},
    {"link_transport", "first_fragment_nopts", &case_first_fragment_nopts},
    {"publish_barrier", "management_half_keeps_language_and_format", &case_management_half_keeps_language_and_format},
    {"publish_barrier", "statement_half_no_side_effects", &case_statement_half_no_side_effects},
    {"lifecycle", "flush_and_toggle", &case_flush_and_toggle},
    {"lifecycle", "same_settings_keep_inflight", &case_same_settings_keep_inflight},
    {"lifecycle", "config_changes_clear_inflight", &case_config_changes_clear_inflight},
    {"lifecycle", "c_api", &case_c_api},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

}  // extern "C"
