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
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "aribcaption/context.hpp"
#include "aribcaption/decoder.hpp"
#include "aribcaption/encoder.hpp"
#include "task_support/check.h"

using namespace aribcaption;

namespace {

// The eight opaque foreground colors of CLUT palette 0 (STD-B24), duplicated
// here because the internal color table header is not on this test's include path.
const ColorRGBA kPalette0[8] = {
    ColorRGBA(0, 0, 0, 255),
    ColorRGBA(255, 0, 0, 255),
    ColorRGBA(0, 255, 0, 255),
    ColorRGBA(255, 255, 0, 255),
    ColorRGBA(0, 0, 255, 255),
    ColorRGBA(255, 0, 255, 255),
    ColorRGBA(0, 255, 255, 255),
    ColorRGBA(255, 255, 255, 255),
};

// ---------- builders ----------

CaptionChar MakeText(uint32_t codepoint, int x, int y, ColorRGBA color) {
    CaptionChar ch;
    ch.type = CaptionCharType::kText;
    ch.codepoint = codepoint;
    ch.x = x;
    ch.y = y;
    ch.char_width = 36;
    ch.char_height = 36;
    ch.char_horizontal_spacing = 4;
    ch.char_vertical_spacing = 24;
    ch.char_horizontal_scale = 1.0f;
    ch.char_vertical_scale = 1.0f;
    ch.text_color = color;
    ch.back_color = ColorRGBA(0, 0, 0, 0);
    return ch;
}

CaptionChar MakeDrcs(uint32_t drcs_code, int x, int y, ColorRGBA color,
                     CaptionCharType type = CaptionCharType::kDRCS) {
    CaptionChar ch;
    ch.type = type;
    ch.codepoint = type == CaptionCharType::kDRCSReplaced ? 0x3013 : 0;
    ch.drcs_code = drcs_code;
    ch.x = x;
    ch.y = y;
    ch.char_width = 36;
    ch.char_height = 36;
    ch.char_horizontal_spacing = 4;
    ch.char_vertical_spacing = 24;
    ch.char_horizontal_scale = 1.0f;
    ch.char_vertical_scale = 1.0f;
    ch.text_color = color;
    ch.back_color = ColorRGBA(0, 0, 0, 0);
    return ch;
}

DRCS MakePattern(int width, int height, int depth, uint8_t seed) {
    DRCS d;
    d.width = width;
    d.height = height;
    d.depth = depth;
    d.depth_bits = (depth == 2) ? 1 : 2;
    const size_t bytes = (static_cast<size_t>(width) * height * d.depth_bits + 7) / 8;
    d.pixels.resize(bytes);
    for (size_t i = 0; i < bytes; i++) {
        d.pixels[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i) * 7);
    }
    return d;
}

// Effective-pixel view: the unused low bits of the final packed byte carry no
// information and are ignored on the wire.
std::vector<uint8_t> EffectivePixels(const DRCS& d) {
    std::vector<uint8_t> pixels = d.pixels;
    const size_t total_bits =
        static_cast<size_t>(d.width) * d.height * d.depth_bits;
    const size_t unused = (8 - total_bits % 8) % 8;
    if (unused != 0 && !pixels.empty()) {
        pixels.back() &= static_cast<uint8_t>(0xFF << unused);
    }
    return pixels;
}

Caption OneCharCaption(const CaptionChar& ch, int64_t pts) {
    Caption cap;
    cap.pts = pts;
    cap.plane_width = 960;
    cap.plane_height = 540;
    cap.iso6392_language_code = ThreeCC("jpn");
    CaptionRegion region;
    region.chars.push_back(ch);
    cap.regions.push_back(region);
    return cap;
}

// Flatten every decoded character across regions, preserving order.
std::vector<CaptionChar> FlattenChars(const Caption& cap) {
    std::vector<CaptionChar> all;
    for (const CaptionRegion& region : cap.regions) {
        for (const CaptionChar& ch : region.chars) {
            all.push_back(ch);
        }
    }
    return all;
}

// Feeds every packet through a fresh JIS decoder and returns the last caption.
bool DecodePackets(const std::vector<EncodedPES>& packets,
                   std::unique_ptr<Caption>& out, uint32_t* language = nullptr) {
    Context ctx;
    Decoder decoder(ctx);
    if (!decoder.Initialize(EncodingScheme::kARIB_STD_B24_JIS)) {
        return false;
    }
    DecodeResult result;
    bool got = false;
    for (const EncodedPES& pes : packets) {
        DecodeStatus status = decoder.Decode(pes.data.data(), pes.data.size(),
                                             pes.pts, result);
        if (status == DecodeStatus::kError) {
            return false;
        }
        if (status == DecodeStatus::kGotCaption && result.caption) {
            out = std::move(result.caption);
            got = true;
        }
    }
    if (language) {
        *language = decoder.QueryISO6392LanguageCode(LanguageId::kFirst);
    }
    return got;
}

// ---------- independent wire interpretation ----------

uint16_t LocalCrc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(data[i]) << 8));
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

struct ParsedPacket {
    uint8_t group_id = 0;
    uint16_t declared_size = 0;
    std::vector<uint8_t> body;
    uint16_t crc = 0;
};

bool ParsePacket(const std::vector<uint8_t>& data, ParsedPacket& parsed) {
    if (data.size() < 10 || data[0] != 0x80 || data[1] != 0xFF ||
        data[2] != 0xF0) {
        return false;
    }
    parsed.group_id = data[3];
    parsed.declared_size = static_cast<uint16_t>((data[6] << 8) | data[7]);
    if (8u + parsed.declared_size + 2u != data.size()) {
        return false;
    }
    parsed.body.assign(data.begin() + 8, data.begin() + 8 + parsed.declared_size);
    parsed.crc = static_cast<uint16_t>((data[8 + parsed.declared_size] << 8) |
                                       data[8 + parsed.declared_size + 1]);
    return LocalCrc16(data.data() + 3, 5 + parsed.body.size()) == parsed.crc;
}

// Finds a data unit (1F parameter) inside a unit loop, returns its body.
bool FindUnit(const std::vector<uint8_t>& units, uint8_t parameter,
              std::vector<uint8_t>& unit_body, size_t* unit_offset = nullptr) {
    size_t offset = 0;
    while (offset + 5 <= units.size()) {
        if (units[offset] != 0x1F) {
            return false;
        }
        uint32_t size = (static_cast<uint32_t>(units[offset + 2]) << 16) |
                        (static_cast<uint32_t>(units[offset + 3]) << 8) |
                        units[offset + 4];
        if (offset + 5 + size > units.size()) {
            return false;
        }
        if (units[offset + 1] == parameter) {
            unit_body.assign(units.begin() + offset + 5,
                             units.begin() + offset + 5 + size);
            if (unit_offset) {
                *unit_offset = offset;
            }
            return true;
        }
        offset += 5 + size;
    }
    return false;
}

bool HasDrcsUnit(const EncodedPES& pes) {
    ParsedPacket parsed;
    if (!ParsePacket(pes.data, parsed)) {
        return false;
    }
    // body: TMD(1) + loop length(3) + units
    if (parsed.body.size() < 4) {
        return false;
    }
    std::vector<uint8_t> unit_body;
    return FindUnit(std::vector<uint8_t>(parsed.body.begin() + 4, parsed.body.end()),
                    0x30, unit_body);
}

std::string SelfDirectory() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return ".";
    }
    path.resize(std::strlen(path.c_str()));
#elif defined(__linux__)
    char buffer[4096];
    ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer));
    if (n <= 0) {
        return ".";
    }
    std::string path(buffer, static_cast<size_t>(n));
#else
    std::string path = ".";
#endif
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? "." : path.substr(0, pos);
}

}  // namespace

// ============================ mapping group ============================

static int test_mapping_mixed_chars() {
    Encoder encoder;
    Caption cap;
    cap.pts = 1200;
    cap.plane_width = 960;
    cap.plane_height = 540;

    CaptionRegion region;
    // Kanji plane, fullwidth alphanumeric, ideographic space, hiragana,
    // katakana and a fullwidth question mark, interleaved with colors.
    const uint32_t codepoints[] = {
        0x65E5, 0x672C, 0x8A9E, 0xFF21, 0x3000, 0x3042, 0x30A2, 0xFF1F
    };
    const int xs[] = {0, 40, 80, 120, 160, 200, 240, 53};
    const int ys[] = {0, 0, 0, 0, 0, 0, 0, 71};  // last one: discrete dots
    for (size_t i = 0; i < 8; i++) {
        region.chars.push_back(MakeText(codepoints[i], xs[i], ys[i],
                                        kPalette0[i]));
    }
    cap.regions.push_back(region);

    std::vector<EncodedPES> packets;
    TASK_CHECK(encoder.Encode(cap, packets));
    TASK_CHECK(packets.size() == 2);
    TASK_CHECK(packets[0].pts == 1200 && packets[1].pts == 1200);

    std::unique_ptr<Caption> decoded;
    uint32_t language = 0;
    TASK_CHECK(DecodePackets(packets, decoded, &language));
    TASK_CHECK(language == ThreeCC("jpn"));
    TASK_CHECK(decoded->plane_width == 960 && decoded->plane_height == 540);

    std::vector<CaptionChar> chars = FlattenChars(*decoded);
    TASK_CHECK(chars.size() == 8);
    for (size_t i = 0; i < 8; i++) {
        TASK_CHECK(chars[i].type == CaptionCharType::kText);
        TASK_CHECK(chars[i].codepoint == codepoints[i]);
        TASK_CHECK(chars[i].x == xs[i]);
        TASK_CHECK(chars[i].y == ys[i]);
        TASK_CHECK(chars[i].text_color.u32 == kPalette0[i].u32);
        TASK_CHECK(chars[i].back_color.u32 == ColorRGBA(0, 0, 0, 0).u32);
    }
    return 0;
}

static int test_mapping_eight_colors() {
    Encoder encoder;
    Caption cap;
    cap.pts = 300;
    CaptionRegion region;
    for (int i = 0; i < 8; i++) {
        region.chars.push_back(MakeText(0x65E5, i * 40, 0, kPalette0[i]));
    }
    cap.regions.push_back(region);

    std::vector<EncodedPES> packets;
    TASK_CHECK(encoder.Encode(cap, packets));

    std::unique_ptr<Caption> decoded;
    TASK_CHECK(DecodePackets(packets, decoded));
    std::vector<CaptionChar> chars = FlattenChars(*decoded);
    TASK_CHECK(chars.size() == 8);
    for (int i = 0; i < 8; i++) {
        TASK_CHECK(chars[i].text_color.u32 == kPalette0[i].u32);
    }
    return 0;
}

static int test_mapping_unmappable_rejected() {
    Encoder encoder;
    const uint32_t bad_codepoints[] = {
        0x0041,  // halfwidth ASCII 'A': only fullwidth Ａ exists in the JIS sets
        0x003F,  // halfwidth '?': must not be accepted as a fallback
        0x1F600, // emoji, outside every bundled table
        0x0E01,  // Thai character, not part of Japanese JIS
    };
    std::vector<EncodedPES> out;
    for (uint32_t cp : bad_codepoints) {
        Caption cap = OneCharCaption(MakeText(cp, 0, 0,
                                               ColorRGBA(255, 255, 255, 255)), 1);
        TASK_CHECK(!encoder.Encode(cap, out));
        TASK_CHECK(out.empty());  // failure never touches the output
    }

    // The fullwidth forms are reversible and must succeed.
    Caption ok = OneCharCaption(MakeText(0xFF21, 0, 0,
                                         ColorRGBA(255, 255, 255, 255)), 1);
    TASK_CHECK(encoder.Encode(ok, out));
    TASK_CHECK(out.size() == 2);
    return 0;
}

static int test_mapping_unsupported_attributes() {
    Encoder encoder;
    std::vector<EncodedPES> out;

    CaptionChar base = MakeText(0x65E5, 0, 0, ColorRGBA(255, 255, 255, 255));

    CaptionChar bold = base;
    bold.style = CharStyle::kCharStyleBold;
    TASK_CHECK(!encoder.Encode(OneCharCaption(bold, 1), out));

    CaptionChar enclosure = base;
    enclosure.enclosure_style = EnclosureStyle::kEnclosureStyleBottom;
    TASK_CHECK(!encoder.Encode(OneCharCaption(enclosure, 1), out));

    CaptionChar scaled = base;
    scaled.char_horizontal_scale = 0.5f;
    TASK_CHECK(!encoder.Encode(OneCharCaption(scaled, 1), out));

    CaptionChar spacing = base;
    spacing.char_horizontal_spacing = 12;
    TASK_CHECK(!encoder.Encode(OneCharCaption(spacing, 1), out));

    CaptionChar background = base;
    background.back_color = ColorRGBA(255, 255, 255, 255);
    TASK_CHECK(!encoder.Encode(OneCharCaption(background, 1), out));

    Caption ruby;
    ruby.pts = 1;
    CaptionRegion ruby_region;
    ruby_region.is_ruby = true;
    ruby_region.chars.push_back(base);
    ruby.regions.push_back(ruby_region);
    TASK_CHECK(!encoder.Encode(ruby, out));

    Caption sound;
    sound.pts = 1;
    sound.has_builtin_sound = true;
    sound.regions.emplace_back().chars.push_back(base);
    TASK_CHECK(!encoder.Encode(sound, out));

    TASK_CHECK(out.empty());
    return 0;
}

static int test_mapping_position_bounds() {
    Encoder encoder;
    std::vector<EncodedPES> out;
    // 40x60 section must lie fully inside the 960x540 plane.
    TASK_CHECK(encoder.Encode(OneCharCaption(
        MakeText(0x65E5, 920, 480, ColorRGBA(255, 255, 255, 255)), 1), out));
    TASK_CHECK(!encoder.Encode(OneCharCaption(
        MakeText(0x65E5, 921, 480, ColorRGBA(255, 255, 255, 255)), 1), out));
    TASK_CHECK(!encoder.Encode(OneCharCaption(
        MakeText(0x65E5, 920, 481, ColorRGBA(255, 255, 255, 255)), 1), out));
    TASK_CHECK(!encoder.Encode(OneCharCaption(
        MakeText(0x65E5, -1, 0, ColorRGBA(255, 255, 255, 255)), 1), out));
    return 0;
}

// ============================ dictionary group ============================

static int test_drcs_roundtrip() {
    Encoder encoder;
    Caption cap;
    cap.pts = 500;
    cap.drcs_map[10] = MakePattern(4, 4, 2, 0x11);   // 2-tone, 2 bytes
    cap.drcs_map[20] = MakePattern(3, 3, 4, 0x22);   // 4-tone, 3 bytes
    cap.drcs_map[30] = MakePattern(64, 64, 2, 0x33); // maximum size

    CaptionRegion region;
    region.chars.push_back(MakeDrcs(10, 0, 0, ColorRGBA(255, 0, 0, 255)));
    // DRCSReplaced must follow the bitmap, not the alternative codepoint.
    region.chars.push_back(MakeDrcs(20, 40, 0, ColorRGBA(0, 255, 0, 255),
                                    CaptionCharType::kDRCSReplaced));
    region.chars.push_back(MakeDrcs(30, 80, 60, ColorRGBA(0, 0, 255, 255)));
    cap.regions.push_back(region);

    std::vector<EncodedPES> packets;
    TASK_CHECK(encoder.Encode(cap, packets));
    TASK_CHECK(HasDrcsUnit(packets.back()));

    std::unique_ptr<Caption> decoded;
    TASK_CHECK(DecodePackets(packets, decoded));
    std::vector<CaptionChar> chars = FlattenChars(*decoded);
    TASK_CHECK(chars.size() == 3);
    TASK_CHECK(decoded->drcs_map.size() == 3);

    const DRCS& d2 = decoded->drcs_map.at(chars[0].drcs_code);
    TASK_CHECK(d2.width == 4 && d2.height == 4 && d2.depth == 2 &&
               d2.depth_bits == 1 &&
               EffectivePixels(d2) == EffectivePixels(cap.drcs_map.at(10)));
    const DRCS& d4 = decoded->drcs_map.at(chars[1].drcs_code);
    TASK_CHECK(d4.width == 3 && d4.height == 3 && d4.depth == 4 &&
               d4.depth_bits == 2 &&
               EffectivePixels(d4) == EffectivePixels(cap.drcs_map.at(20)));
    const DRCS& dmax = decoded->drcs_map.at(chars[2].drcs_code);
    TASK_CHECK(dmax.width == 64 && dmax.height == 64 &&
               EffectivePixels(dmax) == EffectivePixels(cap.drcs_map.at(30)));
    TASK_CHECK(chars[0].x == 0 && chars[1].x == 40 && chars[2].y == 60);
    return 0;
}

static int test_drcs_cache_and_renumber() {
    Encoder encoder;

    // First caption defines bitmap X under drcs_code 11.
    Caption first;
    first.pts = 1;
    first.drcs_map[11] = MakePattern(5, 5, 2, 0x44);
    first.regions.emplace_back().chars.push_back(
        MakeDrcs(11, 0, 0, ColorRGBA(255, 255, 255, 255)));
    std::vector<EncodedPES> p1;
    TASK_CHECK(encoder.Encode(first, p1));
    TASK_CHECK(p1.size() == 2 && HasDrcsUnit(p1[1]));

    // Same bitmap, different drcs_code: cache hit, no new definition unit.
    Caption second;
    second.pts = 2;
    second.drcs_map[99] = MakePattern(5, 5, 2, 0x44);
    second.regions.emplace_back().chars.push_back(
        MakeDrcs(99, 0, 0, ColorRGBA(255, 255, 255, 255)));
    std::vector<EncodedPES> p2;
    TASK_CHECK(encoder.Encode(second, p2));
    TASK_CHECK(p2.size() == 1);
    TASK_CHECK(!HasDrcsUnit(p2[0]));

    // A real decoder keeps the definition across packets, so the stream is
    // decoded with one persistent decoder (p1 then p2, then p3).
    Context ctx;
    Decoder decoder(ctx);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    DecodeResult result;
    for (const EncodedPES& pes : p1) {
        TASK_CHECK(decoder.Decode(pes.data.data(), pes.data.size(), pes.pts,
                                  result) != DecodeStatus::kError);
    }
    TASK_CHECK(decoder.Decode(p2[0].data.data(), p2[0].data.size(), p2[0].pts,
                              result) == DecodeStatus::kGotCaption);
    TASK_CHECK(result.caption->drcs_map.size() == 1);
    const DRCS& reused = result.caption->drcs_map.begin()->second;
    TASK_CHECK(EffectivePixels(reused) ==
               EffectivePixels(first.drcs_map.at(11)));

    // Same drcs_code 11, but a changed bitmap: a definition must be re-sent.
    Caption third;
    third.pts = 3;
    third.drcs_map[11] = MakePattern(5, 5, 2, 0x55);
    third.regions.emplace_back().chars.push_back(
        MakeDrcs(11, 0, 0, ColorRGBA(255, 255, 255, 255)));
    std::vector<EncodedPES> p3;
    TASK_CHECK(encoder.Encode(third, p3));
    TASK_CHECK(HasDrcsUnit(p3[0]));

    TASK_CHECK(decoder.Decode(p3[0].data.data(), p3[0].data.size(), p3[0].pts,
                              result) == DecodeStatus::kGotCaption);
    TASK_CHECK(result.caption->drcs_map.size() == 1);
    const DRCS& replaced = result.caption->drcs_map.begin()->second;
    TASK_CHECK(EffectivePixels(replaced) ==
               EffectivePixels(third.drcs_map.at(11)));
    TASK_CHECK(EffectivePixels(replaced) !=
               EffectivePixels(first.drcs_map.at(11)));
    return 0;
}

static int test_drcs_slots_and_eviction() {
    Encoder encoder;

    // 4x4 2-tone packs into exactly 16 bits, so every seed yields a distinct
    // effective bitmap (no unused trailing bits to collapse patterns).
    // 94 distinct patterns fit; a 95th in the same caption is rejected.
    Caption crowded;
    crowded.pts = 1;
    CaptionRegion region;
    for (int i = 0; i < 95; i++) {
        crowded.drcs_map[1000 + i] = MakePattern(4, 4, 2, static_cast<uint8_t>(0x10 + i));
        region.chars.push_back(MakeDrcs(1000 + i, (i % 24) * 40, (i / 24) * 60,
                                        ColorRGBA(255, 255, 255, 255)));
    }
    crowded.regions.push_back(region);
    std::vector<EncodedPES> out;
    TASK_CHECK(!encoder.Encode(crowded, out));
    TASK_CHECK(out.empty());

    Caption full;
    full.pts = 1;
    CaptionRegion full_region;
    for (int i = 0; i < 94; i++) {
        full.drcs_map[i] = MakePattern(4, 4, 2, static_cast<uint8_t>(0x20 + i));
        full_region.chars.push_back(MakeDrcs(i, (i % 24) * 40, (i / 24) * 60,
                                             ColorRGBA(255, 255, 255, 255)));
    }
    full.regions.push_back(full_region);
    std::vector<EncodedPES> p_full;
    TASK_CHECK(encoder.Encode(full, p_full));

    // A later caption keeps the slot-5 bitmap and introduces one unique new
    // pattern: an occupied slot is replaced, but never the still-referenced one.
    Caption next;
    next.pts = 2;
    next.drcs_map[5] = MakePattern(4, 4, 2, 0x25);   // same bitmap as full slot 5
    next.drcs_map[777] = MakePattern(4, 4, 2, 0xF0); // brand new
    CaptionRegion next_region;
    next_region.chars.push_back(MakeDrcs(5, 0, 0, ColorRGBA(255, 255, 255, 255)));
    next_region.chars.push_back(MakeDrcs(777, 40, 0, ColorRGBA(255, 255, 255, 255)));
    next_region.chars.push_back(MakeDrcs(5, 80, 0, ColorRGBA(255, 255, 255, 255)));
    next.regions.push_back(next_region);
    std::vector<EncodedPES> p_next;
    TASK_CHECK(encoder.Encode(next, p_next));
    TASK_CHECK(HasDrcsUnit(p_next[0]));

    std::unique_ptr<Caption> decoded;
    Context ctx;
    Decoder decoder(ctx);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    DecodeResult result;
    for (const EncodedPES& pes : p_full) {
        TASK_CHECK(decoder.Decode(pes.data.data(), pes.data.size(), pes.pts,
                                  result) != DecodeStatus::kError);
    }
    TASK_CHECK(decoder.Decode(p_next[0].data.data(), p_next[0].data.size(),
                              p_next[0].pts, result) == DecodeStatus::kGotCaption);
    std::vector<CaptionChar> chars = FlattenChars(*result.caption);
    TASK_CHECK(chars.size() == 3);
    TASK_CHECK(EffectivePixels(result.caption->drcs_map.at(chars[0].drcs_code)) ==
               EffectivePixels(next.drcs_map.at(5)));
    TASK_CHECK(EffectivePixels(result.caption->drcs_map.at(chars[2].drcs_code)) ==
               EffectivePixels(next.drcs_map.at(5)));
    TASK_CHECK(EffectivePixels(result.caption->drcs_map.at(chars[1].drcs_code)) ==
               EffectivePixels(next.drcs_map.at(777)));
    return 0;
}

static int test_drcs_forward_reference_protected() {
    Encoder encoder;

    // Fill all 94 slots with 4x4 patterns (16 effective bits, no trailing bits);
    // seed 0x20 lands in slot 0 (lowest free slot first).
    Caption fill;
    fill.pts = 1;
    CaptionRegion fill_region;
    for (int i = 0; i < 94; i++) {
        fill.drcs_map[i] = MakePattern(4, 4, 2, static_cast<uint8_t>(0x20 + i));
        fill_region.chars.push_back(MakeDrcs(i, (i % 24) * 40, (i / 24) * 60,
                                             ColorRGBA(255, 255, 255, 255)));
    }
    fill.regions.push_back(fill_region);
    std::vector<EncodedPES> p_fill;
    TASK_CHECK(encoder.Encode(fill, p_fill));

    // 93 brand-new patterns are needed, and the resident slot-0 pattern is only
    // referenced at the very end. It must survive every preceding eviction.
    Caption next;
    next.pts = 2;
    CaptionRegion next_region;
    for (int i = 0; i < 93; i++) {
        next.drcs_map[2000 + i] = MakePattern(4, 4, 2, static_cast<uint8_t>(0x80 + i));
        next_region.chars.push_back(MakeDrcs(2000 + i, (i % 24) * 40, (i / 24) * 60,
                                             ColorRGBA(255, 255, 255, 255)));
    }
    next.drcs_map[0] = MakePattern(4, 4, 2, 0x20);  // identical to cached slot 0
    next_region.chars.push_back(MakeDrcs(0, (93 % 24) * 40, (93 / 24) * 60,
                                         ColorRGBA(255, 255, 255, 255)));
    next.regions.push_back(next_region);

    std::vector<EncodedPES> p_next;
    TASK_CHECK(encoder.Encode(next, p_next));

    Context ctx;
    Decoder decoder(ctx);
    TASK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    DecodeResult result;
    for (const EncodedPES& pes : p_fill) {
        TASK_CHECK(decoder.Decode(pes.data.data(), pes.data.size(), pes.pts,
                                  result) != DecodeStatus::kError);
    }
    TASK_CHECK(decoder.Decode(p_next[0].data.data(), p_next[0].data.size(),
                              p_next[0].pts, result) == DecodeStatus::kGotCaption);
    std::vector<CaptionChar> chars = FlattenChars(*result.caption);
    TASK_CHECK(chars.size() == 94);
    // The last character still resolves to the original slot-0 bitmap.
    const DRCS& last = result.caption->drcs_map.at(chars.back().drcs_code);
    TASK_CHECK(EffectivePixels(last) == EffectivePixels(next.drcs_map.at(0)));
    // The first new pattern round-trips too, proving evictions still happened.
    const DRCS& first_new = result.caption->drcs_map.at(chars.front().drcs_code);
    TASK_CHECK(EffectivePixels(first_new) ==
               EffectivePixels(next.drcs_map.at(2000)));
    return 0;
}

static int test_drcs_trailing_bits_ignored() {
    Encoder encoder;
    // 5x5 2-tone = 25 bits, the final byte has 7 unused low bits.
    DRCS a = MakePattern(5, 5, 2, 0x40);
    DRCS b = a;
    b.pixels.back() = static_cast<uint8_t>(b.pixels.back() | 0x03);  // unused bits only

    Caption first;
    first.pts = 1;
    first.drcs_map[1] = a;
    first.regions.emplace_back().chars.push_back(
        MakeDrcs(1, 0, 0, ColorRGBA(255, 255, 255, 255)));
    std::vector<EncodedPES> p1;
    TASK_CHECK(encoder.Encode(first, p1));
    TASK_CHECK(HasDrcsUnit(p1.back()));

    Caption second;
    second.pts = 2;
    second.drcs_map[2] = b;  // identical effective pixels, different raw last byte
    second.regions.emplace_back().chars.push_back(
        MakeDrcs(2, 0, 0, ColorRGBA(255, 255, 255, 255)));
    std::vector<EncodedPES> p2;
    TASK_CHECK(encoder.Encode(second, p2));
    TASK_CHECK(p2.size() == 1);
    TASK_CHECK(!HasDrcsUnit(p2[0]));  // shared slot, no redefinition
    return 0;
}

static int test_drcs_invalid_rejected() {
    Encoder encoder;
    std::vector<EncodedPES> out;

    auto attempt = [&](const DRCS& d) {
        Caption cap;
        cap.pts = 1;
        cap.drcs_map[1] = d;
        cap.regions.emplace_back().chars.push_back(
            MakeDrcs(1, 0, 0, ColorRGBA(255, 255, 255, 255)));
        return encoder.Encode(cap, out);
    };

    DRCS d = MakePattern(4, 4, 2, 0x10);
    DRCS bad_width = d;
    bad_width.width = 65;
    TASK_CHECK(!attempt(bad_width));

    DRCS bad_depth = d;
    bad_depth.depth = 3;
    bad_depth.depth_bits = 2;
    TASK_CHECK(!attempt(bad_depth));

    DRCS bad_bits = d;
    bad_bits.depth_bits = 2;  // depth 2 requires exactly 1 bit
    TASK_CHECK(!attempt(bad_bits));

    DRCS bad_pixels = d;
    bad_pixels.pixels.push_back(0x00);  // size no longer matches w*h*bits
    TASK_CHECK(!attempt(bad_pixels));

    DRCS zero = d;
    zero.width = 0;
    TASK_CHECK(!attempt(zero));

    Caption missing;
    missing.pts = 1;
    missing.regions.emplace_back().chars.push_back(
        MakeDrcs(42, 0, 0, ColorRGBA(255, 255, 255, 255)));  // no drcs_map entry
    TASK_CHECK(!encoder.Encode(missing, out));
    TASK_CHECK(out.empty());
    return 0;
}

// ============================ rollback group ============================

static int test_rollback_failure_retry() {
    Encoder encoder;
    Caption good_a = OneCharCaption(
        MakeText(0x65E5, 0, 0, ColorRGBA(255, 255, 255, 255)), 100);
    std::vector<EncodedPES> ref_a;
    TASK_CHECK(encoder.Encode(good_a, ref_a));

    // Oversized group: many large 4-tone patterns exceed 65535 bytes.
    Caption oversized;
    oversized.pts = 200;
    CaptionRegion region;
    for (int i = 0; i < 64; i++) {
        DRCS d;
        d.width = 64;
        d.height = 64;
        d.depth = 4;
        d.depth_bits = 2;
        d.pixels.resize(1024, static_cast<uint8_t>(i));
        oversized.drcs_map[100 + i] = d;
        region.chars.push_back(MakeDrcs(100 + i, (i % 24) * 40, (i / 24) * 60,
                                        ColorRGBA(255, 255, 255, 255)));
    }
    oversized.regions.push_back(region);
    std::vector<EncodedPES> out;
    TASK_CHECK(!encoder.Encode(oversized, out));
    TASK_CHECK(out.empty());

    // After the failure the encoder must behave exactly like one that never
    // saw the oversized caption.
    Caption good_b = OneCharCaption(
        MakeText(0x672C, 0, 0, ColorRGBA(255, 0, 0, 255)), 300);
    std::vector<EncodedPES> after_b;
    TASK_CHECK(encoder.Encode(good_b, after_b));

    Encoder reference;
    std::vector<EncodedPES> ref_b_a, ref_b_b;
    TASK_CHECK(reference.Encode(good_a, ref_b_a));
    TASK_CHECK(reference.Encode(good_b, ref_b_b));
    TASK_CHECK(after_b.size() == 1 && ref_b_b.size() == 1);
    TASK_CHECK(after_b[0].data == ref_b_b[0].data);
    TASK_CHECK(after_b[0].pts == ref_b_b[0].pts);
    return 0;
}

static int test_rollback_reset() {
    Encoder encoder;
    Caption cap = OneCharCaption(
        MakeText(0x65E5, 0, 0, ColorRGBA(255, 255, 255, 255)), 100);

    std::vector<EncodedPES> first;
    TASK_CHECK(encoder.Encode(cap, first));
    TASK_CHECK(first.size() == 2);  // management + statement

    std::vector<EncodedPES> second;
    TASK_CHECK(encoder.Encode(cap, second));
    TASK_CHECK(second.size() == 1);  // session continues, no management

    encoder.Reset();
    std::vector<EncodedPES> after_reset;
    TASK_CHECK(encoder.Encode(cap, after_reset));
    TASK_CHECK(after_reset.size() == 2);  // management is sent again
    TASK_CHECK(after_reset[0].data == first[0].data);
    TASK_CHECK(after_reset[1].data == first[1].data);

    // After Reset the DRCS cache is empty: a pattern gets redefined.
    Encoder drcs_encoder;
    Caption drcs_cap;
    drcs_cap.pts = 1;
    drcs_cap.drcs_map[7] = MakePattern(4, 4, 2, 0x10);
    drcs_cap.regions.emplace_back().chars.push_back(
        MakeDrcs(7, 0, 0, ColorRGBA(255, 255, 255, 255)));
    std::vector<EncodedPES> d1, d2;
    TASK_CHECK(drcs_encoder.Encode(drcs_cap, d1));
    TASK_CHECK(drcs_encoder.Encode(drcs_cap, d2));
    TASK_CHECK(!HasDrcsUnit(d2[0]));
    drcs_encoder.Reset();
    std::vector<EncodedPES> d3;
    TASK_CHECK(drcs_encoder.Encode(drcs_cap, d3));
    TASK_CHECK(d3.size() == 2 && HasDrcsUnit(d3[1]));
    return 0;
}

static int test_rollback_input_and_output_immutable() {
    Encoder encoder;
    Caption cap;
    cap.pts = PTS_NOPTS;
    cap.drcs_map[7] = MakePattern(4, 4, 2, 0x10);
    CaptionRegion region;
    region.chars.push_back(MakeDrcs(7, 0, 0, ColorRGBA(255, 255, 255, 255)));
    region.chars.push_back(MakeText(0x65E5, 40, 0,
                                    ColorRGBA(0, 255, 0, 255)));
    cap.regions.push_back(region);

    std::vector<EncodedPES> packets;
    TASK_CHECK(encoder.Encode(cap, packets));
    std::vector<uint8_t> snapshot = packets.back().data;
    TASK_CHECK(packets.back().pts == PTS_NOPTS);

    // Mutating the input caption after Encode must not change returned packets.
    cap.regions[0].chars[0].x = 120;
    cap.drcs_map[7].pixels[0] = static_cast<uint8_t>(~cap.drcs_map[7].pixels[0]);
    cap.pts = 999;
    TASK_CHECK(packets.back().data == snapshot);
    TASK_CHECK(packets.back().pts == PTS_NOPTS);

    // Encoding the same logical caption twice yields identical bytes.
    Caption again;
    again.pts = PTS_NOPTS;
    again.drcs_map[7] = MakePattern(4, 4, 2, 0x10);
    CaptionRegion again_region;
    again_region.chars.push_back(MakeDrcs(7, 0, 0, ColorRGBA(255, 255, 255, 255)));
    again_region.chars.push_back(MakeText(0x65E5, 40, 0,
                                          ColorRGBA(0, 255, 0, 255)));
    again.regions.push_back(again_region);
    Encoder other;
    std::vector<EncodedPES> repeat;
    TASK_CHECK(other.Encode(again, repeat));
    TASK_CHECK(repeat.size() == packets.size());
    for (size_t i = 0; i < packets.size(); i++) {
        TASK_CHECK(repeat[i].data == packets[i].data);
        TASK_CHECK(repeat[i].pts == packets[i].pts);
    }
    return 0;
}

// ============================ wire group ============================

static int test_wire_packet_structure() {
    Encoder encoder;
    Caption cap;
    cap.pts = 1234;
    cap.drcs_map[7] = MakePattern(4, 4, 2, 0x10);
    CaptionRegion region;
    region.chars.push_back(MakeDrcs(7, 0, 0, ColorRGBA(255, 255, 255, 255)));
    region.chars.push_back(MakeText(0x65E5, 40, 0,
                                    ColorRGBA(255, 255, 0, 255)));
    cap.regions.push_back(region);

    std::vector<EncodedPES> packets;
    TASK_CHECK(encoder.Encode(cap, packets));
    TASK_CHECK(packets.size() == 2);

    ParsedPacket management, statement;
    TASK_CHECK(ParsePacket(packets[0].data, management));
    TASK_CHECK(ParsePacket(packets[1].data, statement));

    // Independent interpretation of the headers and size fields.
    TASK_CHECK(management.group_id == 0x00);
    TASK_CHECK(management.declared_size == management.body.size());
    TASK_CHECK(management.declared_size <= 65535);
    TASK_CHECK(statement.group_id == 0x04);
    TASK_CHECK(statement.declared_size == statement.body.size());
    TASK_CHECK(statement.declared_size <= 65535);

    // Management group: one Japanese first-language entry, SWF 7 format.
    TASK_CHECK(management.body[0] == 0x00);
    TASK_CHECK(management.body[1] == 0x01);
    TASK_CHECK(management.body[2] == 0x00);
    TASK_CHECK(management.body[3] == 0x6A);
    TASK_CHECK(management.body[4] == 0x70);
    TASK_CHECK(management.body[5] == 0x6E);
    TASK_CHECK(management.body[6] == 0x80);

    // Statement group: TMD 0 and a unit loop covering the rest.
    TASK_CHECK(statement.body[0] == 0x00);
    uint32_t loop_length = (static_cast<uint32_t>(statement.body[1]) << 16) |
                           (static_cast<uint32_t>(statement.body[2]) << 8) |
                           statement.body[3];
    TASK_CHECK(loop_length == statement.body.size() - 4);

    std::vector<uint8_t> units(statement.body.begin() + 4, statement.body.end());
    std::vector<uint8_t> drcs_unit, body_unit;
    size_t drcs_offset = 0, body_offset = 0;
    TASK_CHECK(FindUnit(units, 0x30, drcs_unit, &drcs_offset));
    TASK_CHECK(FindUnit(units, 0x20, body_unit, &body_offset));
    // The definition unit is transmitted before the statement body referencing it.
    TASK_CHECK(drcs_offset < body_offset);
    // Every statement body starts with clear screen (CS).
    TASK_CHECK(!body_unit.empty() && body_unit[0] == 0x0C);
    return 0;
}

static int test_wire_clear_screen_empty() {
    Encoder encoder;
    Caption empty;
    empty.pts = 42;
    std::vector<EncodedPES> packets;
    TASK_CHECK(encoder.Encode(empty, packets));
    TASK_CHECK(packets.size() == 2);

    ParsedPacket statement;
    TASK_CHECK(ParsePacket(packets[1].data, statement));
    std::vector<uint8_t> units(statement.body.begin() + 4, statement.body.end());
    std::vector<uint8_t> body_unit;
    TASK_CHECK(FindUnit(units, 0x20, body_unit));
    TASK_CHECK(body_unit.size() == 1 && body_unit[0] == 0x0C);  // pure clear screen
    TASK_CHECK(!FindUnit(units, 0x30, body_unit));              // no DRCS unit

    std::unique_ptr<Caption> decoded;
    TASK_CHECK(DecodePackets(packets, decoded));
    TASK_CHECK(decoded->regions.empty());
    TASK_CHECK(decoded->flags & CaptionFlags::kCaptionFlagsClearScreen);
    TASK_CHECK(decoded->wait_duration == DURATION_INDEFINITE);
    return 0;
}

static int test_wire_cross_process_roundtrip() {
    const std::string dir = SelfDirectory();
    const std::string binary = dir + "/caption_encode";
    const char* tmpdir = std::getenv("TMPDIR");
    const std::string output =
        std::string(tmpdir ? tmpdir : "/tmp") + "/aribcc_roundtrip.pes";
    std::remove(output.c_str());

    const std::string write_cmd = "\"" + binary + "\" \"" + output + "\"";
    const std::string decode_cmd =
        "\"" + binary + "\" --decode \"" + output + "\"";
    // Separate processes: one writes, a read-only decoder process verifies.
    TASK_CHECK(std::system(write_cmd.c_str()) == 0);
    TASK_CHECK(std::system(decode_cmd.c_str()) == 0);
    std::remove(output.c_str());
    return 0;
}

// ============================ registration ============================

const struct task_case task_cases[] = {
    {"mapping", "mixed_chars", test_mapping_mixed_chars},
    {"mapping", "eight_colors", test_mapping_eight_colors},
    {"mapping", "unmappable_rejected", test_mapping_unmappable_rejected},
    {"mapping", "unsupported_attributes", test_mapping_unsupported_attributes},
    {"mapping", "position_bounds", test_mapping_position_bounds},

    {"dictionary", "drcs_roundtrip", test_drcs_roundtrip},
    {"dictionary", "cache_and_renumber", test_drcs_cache_and_renumber},
    {"dictionary", "slots_and_eviction", test_drcs_slots_and_eviction},
    {"dictionary", "forward_reference_protected", test_drcs_forward_reference_protected},
    {"dictionary", "trailing_bits_ignored", test_drcs_trailing_bits_ignored},
    {"dictionary", "invalid_rejected", test_drcs_invalid_rejected},

    {"rollback", "failure_retry", test_rollback_failure_retry},
    {"rollback", "reset", test_rollback_reset},
    {"rollback", "immutability", test_rollback_input_and_output_immutable},

    {"wire", "packet_structure", test_wire_packet_structure},
    {"wire", "clear_screen_empty", test_wire_clear_screen_empty},
    {"wire", "cross_process_roundtrip", test_wire_cross_process_roundtrip},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
