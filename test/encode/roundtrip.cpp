// Round-trip tests for the ARIB STD-B24 caption encoder.
//
// These tests deliberately avoid trusting the encoder's own bookkeeping:
//  - the wire format is re-parsed independently (data group length, CRC, data
//    units, charset escape sequences, ACPS positions),
//  - decoded captions are compared against an independent expectation list,
//  - the example program is exercised across two processes.

#include "task_support/check.h"

#include <aribcaption/caption.hpp>
#include <aribcaption/color.hpp>
#include <aribcaption/context.hpp>
#include <aribcaption/decoder.hpp>
#include <aribcaption/encoder.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace aribcaption;

namespace {

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

constexpr int kSectionWidth = 40;
constexpr int kSectionHeight = 60;

ColorRGBA PaletteColor(int index) {
    static const ColorRGBA kPalette[8] = {
        ColorRGBA(0, 0, 0, 255),        // black
        ColorRGBA(255, 0, 0, 255),      // red
        ColorRGBA(0, 255, 0, 255),      // green
        ColorRGBA(255, 255, 0, 255),    // yellow
        ColorRGBA(0, 0, 255, 255),      // blue
        ColorRGBA(255, 0, 255, 255),    // magenta
        ColorRGBA(0, 255, 255, 255),    // cyan
        ColorRGBA(255, 255, 255, 255),  // white
    };
    return kPalette[index];
}

int PixelBytes(int w, int h, int depth_bits) {
    return (w * h * depth_bits + 7) / 8;
}

DRCS MakeGlyphData(int w, int h, int depth, uint32_t seed) {
    DRCS d;
    d.width = w;
    d.height = h;
    d.depth = depth;
    d.depth_bits = depth == 2 ? 1 : 2;
    int bytes = PixelBytes(w, h, d.depth_bits);
    d.pixels.resize(bytes);
    for (int i = 0; i < bytes; i++) {
        d.pixels[i] = static_cast<uint8_t>((seed * 131u + static_cast<uint32_t>(i) * 29u + 7u) & 0xFF);
    }
    d.md5 = "ignored-" + std::to_string(seed);
    return d;
}

// Canonical pixel buffer: trailing unused bits are ignored / zeroed.
std::vector<uint8_t> CanonicalPixels(const DRCS& d) {
    int bits = d.width * d.height * d.depth_bits;
    int bytes = (bits + 7) / 8;
    std::vector<uint8_t> out(bytes, 0);
    for (int i = 0; i < bytes && i < static_cast<int>(d.pixels.size()); i++) {
        out[i] = d.pixels[i];
    }
    int tail = bits % 8;
    if (tail != 0 && !out.empty()) {
        out.back() &= static_cast<uint8_t>(0xFF << (8 - tail));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Caption construction
// ---------------------------------------------------------------------------

struct SpecChar {
    bool drcs = false;
    uint32_t codepoint = 0;
    uint32_t drcs_code = 0;
    int x = 0;
    int y = 0;
    int color = 7;
};

Caption MakeCaption(const std::vector<SpecChar>& specs,
                    const std::unordered_map<uint32_t, DRCS>& drcs_map,
                    int64_t pts = PTS_NOPTS) {
    Caption c;
    c.pts = pts;
    c.plane_width = 960;
    c.plane_height = 540;
    c.iso6392_language_code = ThreeCC("jpn");
    c.drcs_map = drcs_map;

    CaptionRegion region;
    for (const SpecChar& spec : specs) {
        CaptionChar ch;
        if (spec.drcs) {
            ch.type = CaptionCharType::kDRCS;
            ch.drcs_code = spec.drcs_code;
            ch.codepoint = 0;
        } else {
            ch.type = CaptionCharType::kText;
            ch.codepoint = spec.codepoint;
        }
        ch.x = spec.x;
        ch.y = spec.y;
        ch.char_width = 36;
        ch.char_height = 36;
        ch.char_horizontal_spacing = 4;
        ch.char_vertical_spacing = 24;
        ch.char_horizontal_scale = 1.0f;
        ch.char_vertical_scale = 1.0f;
        ch.text_color = PaletteColor(spec.color);
        ch.back_color = ColorRGBA(0, 0, 0, 0);
        region.chars.push_back(ch);
    }
    if (!specs.empty()) {
        region.x = specs.front().x;
        region.y = specs.front().y;
        region.width = static_cast<int>(specs.size()) * kSectionWidth;
        region.height = kSectionHeight;
        c.regions.push_back(region);
    }
    return c;
}

std::vector<SpecChar> TextSpecs(const std::vector<uint32_t>& codepoints) {
    std::vector<SpecChar> specs;
    for (size_t i = 0; i < codepoints.size(); i++) {
        SpecChar s;
        s.codepoint = codepoints[i];
        s.x = static_cast<int>(i % 24) * kSectionWidth;
        s.y = static_cast<int>(i / 24) * kSectionHeight;
        s.color = static_cast<int>(i % 8);
        specs.push_back(s);
    }
    return specs;
}

// ---------------------------------------------------------------------------
// Independent wire parsing
// ---------------------------------------------------------------------------

uint16_t ComputeCRC16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
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

struct ParsedGroup {
    bool ok = false;
    uint8_t data_group_id = 0;
    size_t declared_size = 0;
    uint16_t crc_field = 0;
    uint16_t crc_calc = 0;
    std::vector<uint8_t> payload;
};

ParsedGroup ParseGroup(const std::vector<uint8_t>& data) {
    ParsedGroup g;
    if (data.size() < 10) {
        return g;
    }
    if (data[0] != 0x80 || data[1] != 0xFF || data[2] != 0xF0) {
        return g;
    }
    g.data_group_id = data[3];
    g.declared_size = (static_cast<size_t>(data[6]) << 8) | data[7];
    if (8 + g.declared_size + 2 != data.size()) {
        return g;
    }
    g.payload.assign(data.begin() + 8, data.begin() + 8 + static_cast<std::ptrdiff_t>(g.declared_size));
    g.crc_field = static_cast<uint16_t>((data[8 + g.declared_size] << 8) | data[9 + g.declared_size]);
    g.crc_calc = ComputeCRC16(data.data() + 3, 5 + g.declared_size);
    g.ok = (g.crc_field == g.crc_calc);
    return g;
}

struct DataUnit {
    uint8_t parameter = 0;
    std::vector<uint8_t> payload;
};

bool ParseDataUnits(const std::vector<uint8_t>& data, std::vector<DataUnit>& units) {
    units.clear();
    size_t offset = 0;
    while (offset < data.size()) {
        if (offset + 5 > data.size()) {
            return false;
        }
        if (data[offset] != 0x1F) {
            return false;
        }
        uint8_t parameter = data[offset + 1];
        size_t size = (static_cast<size_t>(data[offset + 2]) << 16) |
                      (static_cast<size_t>(data[offset + 3]) << 8) |
                      static_cast<size_t>(data[offset + 4]);
        if (offset + 5 + size > data.size()) {
            return false;
        }
        DataUnit unit;
        unit.parameter = parameter;
        unit.payload.assign(data.begin() + offset + 5,
                            data.begin() + offset + 5 + static_cast<std::ptrdiff_t>(size));
        units.push_back(std::move(unit));
        offset += 5 + size;
    }
    return true;
}

enum class WireSet : int {
    kKanji = 0,
    kAlphanumeric,
    kHiragana,
    kKatakana,
    kDRCS
};

struct WireChar {
    WireSet set = WireSet::kKanji;
    uint8_t first = 0;
    uint8_t second = 0;
    bool two_bytes = false;
    int x = 0;
    int y = 0;
    int color = 7;
};

struct WirePlan {
    bool ok = false;
    bool begins_with_clear = false;
    bool has_drcs_unit = false;
    std::vector<WireChar> chars;
    int drcs_definition_count = 0;
};

// Parse a CSI parameter list as emitted by the encoder: decimal parameters
// separated by ';', a space terminator, then the F byte.
bool ParseCSI(const std::vector<uint8_t>& body, size_t* offset, int* param1, int* param2) {
    std::vector<int> params;
    int current = 0;
    bool has_current = false;
    size_t i = *offset;
    while (i < body.size()) {
        uint8_t b = body[i];
        if (b >= '0' && b <= '9') {
            current = current * 10 + (b - '0');
            has_current = true;
            i++;
        } else if (b == ';') {
            params.push_back(has_current ? current : 0);
            current = 0;
            has_current = false;
            i++;
        } else if (b == 0x20) {
            if (has_current) {
                params.push_back(current);
            }
            i++;
            if (i >= body.size()) {
                return false;
            }
            *param1 = params.size() > 0 ? params[0] : 0;
            *param2 = params.size() > 1 ? params[1] : 0;
            *offset = i + 1;  // consume the F byte
            return true;
        } else {
            return false;
        }
    }
    return false;
}

WirePlan ReadStatementPlan(const std::vector<uint8_t>& data) {
    WirePlan plan;
    ParsedGroup group = ParseGroup(data);
    if (!group.ok || group.data_group_id != 0x04 || group.payload.size() < 4 || group.payload[0] != 0x00) {
        return plan;
    }
    size_t units_length = (static_cast<size_t>(group.payload[1]) << 16) |
                          (static_cast<size_t>(group.payload[2]) << 8) |
                          static_cast<size_t>(group.payload[3]);
    if (4 + units_length != group.payload.size()) {
        return plan;
    }
    std::vector<uint8_t> units_data(group.payload.begin() + 4, group.payload.end());
    std::vector<DataUnit> units;
    if (!ParseDataUnits(units_data, units)) {
        return plan;
    }
    const std::vector<uint8_t>* body = nullptr;
    for (const DataUnit& unit : units) {
        if (unit.parameter == 0x30) {
            if (unit.payload.empty()) {
                return plan;
            }
            plan.has_drcs_unit = true;
            plan.drcs_definition_count = unit.payload[0];
        } else if (unit.parameter == 0x20) {
            body = &unit.payload;
        }
    }
    if (!body) {
        return plan;
    }

    const std::vector<uint8_t>& b = *body;
    size_t i = 0;
    if (!b.empty() && b[0] == 0x0C) {
        plan.begins_with_clear = true;
        i = 1;
    }

    WireSet current_set = WireSet::kKanji;
    int current_color = 7;
    bool have_position = false;
    int pos_x = 0;
    int pos_y = 0;

    while (i < b.size()) {
        uint8_t byte = b[i];
        if (byte == 0x0C) {
            i++;
        } else if (byte >= 0x80 && byte <= 0x87) {
            current_color = byte - 0x80;
            i++;
        } else if (byte == 0x9B) {
            int p1 = 0;
            int p2 = 0;
            i++;
            if (!ParseCSI(b, &i, &p1, &p2)) {
                return plan;
            }
            pos_x = p1;
            pos_y = p2;
            have_position = true;
        } else if (byte == 0x1B) {
            if (i + 2 >= b.size()) {
                return plan;
            }
            if (b[i + 1] == 0x24 && b[i + 2] == 0x42) {
                current_set = WireSet::kKanji;
                i += 3;
            } else if (b[i + 1] == 0x28) {
                uint8_t designator = b[i + 2];
                if (designator == 0x4A) {
                    current_set = WireSet::kAlphanumeric;
                    i += 3;
                } else if (designator == 0x30) {
                    current_set = WireSet::kHiragana;
                    i += 3;
                } else if (designator == 0x31) {
                    current_set = WireSet::kKatakana;
                    i += 3;
                } else if (designator == 0x20) {
                    if (i + 3 >= b.size()) {
                        return plan;
                    }
                    current_set = WireSet::kDRCS;
                    i += 4;
                } else {
                    return plan;
                }
            } else {
                return plan;
            }
        } else if (byte >= 0x21 && byte <= 0x7E) {
            bool two_bytes = (current_set == WireSet::kKanji);
            if (two_bytes && i + 1 >= b.size()) {
                return plan;
            }
            WireChar ch;
            ch.set = current_set;
            ch.first = byte;
            ch.two_bytes = two_bytes;
            if (two_bytes) {
                ch.second = b[i + 1];
                i += 2;
            } else {
                i += 1;
            }
            ch.color = current_color;
            ch.x = have_position ? pos_x : 0;
            ch.y = have_position ? (pos_y - kSectionHeight) : 0;
            plan.chars.push_back(ch);
            have_position = false;
        } else {
            return plan;
        }
    }

    plan.ok = true;
    return plan;
}

// ---------------------------------------------------------------------------
// Decoder harness
// ---------------------------------------------------------------------------

std::vector<Caption> DecodePackets(const std::vector<EncodedPES>& packets) {
    Context context;
    Decoder decoder(context);
    decoder.Initialize(EncodingScheme::kARIB_STD_B24_JIS);

    std::vector<Caption> captions;
    for (const EncodedPES& packet : packets) {
        DecodeResult result;
        DecodeStatus status = decoder.Decode(packet.data.data(), packet.data.size(), packet.pts, result);
        if (status == DecodeStatus::kGotCaption && result.caption) {
            captions.push_back(*result.caption);
        }
    }
    return captions;
}

std::vector<const CaptionChar*> Flatten(const Caption& caption) {
    std::vector<const CaptionChar*> chars;
    for (const CaptionRegion& region : caption.regions) {
        for (const CaptionChar& ch : region.chars) {
            chars.push_back(&ch);
        }
    }
    return chars;
}

bool SamePackets(const std::vector<EncodedPES>& a, const std::vector<EncodedPES>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].pts != b[i].pts || a[i].data != b[i].data) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// mapping group
// ---------------------------------------------------------------------------

int CaseJisMappingRoundtrip() {
    // Kanji, fullwidth Latin, hiragana, katakana, ideographic space, symbols.
    std::vector<uint32_t> codepoints = {
        0xFF21, 0xFF22, 0xFF5A, 0x65E5, 0x672C, 0x8A9E,
        0x3042, 0x3044, 0x30A2, 0x30A4, 0x3000, 0x3001,
        0x3013, 0x3005, 0x4E00, 0xFF01,
    };
    Caption caption = MakeCaption(TextSpecs(codepoints), {}, 4242);

    Encoder encoder;
    std::vector<EncodedPES> packets;
    TASK_CHECK(encoder.Encode(caption, packets));
    TASK_CHECK(packets.size() == 2);
    TASK_CHECK(packets[0].pts == 4242);
    TASK_CHECK(packets[1].pts == 4242);

    std::vector<Caption> decoded = DecodePackets(packets);
    TASK_CHECK(decoded.size() == 1);
    std::vector<const CaptionChar*> chars = Flatten(decoded[0]);
    TASK_CHECK(chars.size() == codepoints.size());
    for (size_t i = 0; i < codepoints.size(); i++) {
        TASK_CHECK(chars[i]->type == CaptionCharType::kText);
        TASK_CHECK(chars[i]->codepoint == codepoints[i]);
        TASK_CHECK(chars[i]->x == static_cast<int>(i % 24) * kSectionWidth);
        TASK_CHECK(chars[i]->y == static_cast<int>(i / 24) * kSectionHeight);
        TASK_CHECK(chars[i]->text_color.u32 == PaletteColor(static_cast<int>(i % 8)).u32);
    }
    return 0;
}

int CaseUnsupportedCodepointRejected() {
    Encoder encoder;
    std::vector<EncodedPES> out;

    // Plain ASCII is not representable in the JIS graphic sets used here.
    std::vector<uint32_t> codepoints = {0x0041};
    TASK_CHECK(!encoder.Encode(MakeCaption(TextSpecs(codepoints), {}), out));
    TASK_CHECK(out.empty());

    // Same for UTF-8-only scripts.
    std::vector<uint32_t> philippine = {0x1F600};
    TASK_CHECK(!encoder.Encode(MakeCaption(TextSpecs(philippine), {}), out));
    TASK_CHECK(out.empty());
    return 0;
}

int CaseUnsupportedAttributesRejected() {
    Encoder encoder;
    std::vector<EncodedPES> out;
    out.push_back(EncodedPES{123, {0xAA, 0xBB}});

    auto expect_failure = [&](const Caption& caption) -> int {
        std::vector<EncodedPES> before = out;
        TASK_CHECK(!encoder.Encode(caption, out));
        TASK_CHECK(SamePackets(out, before));
        return 0;
    };

    Caption base = MakeCaption(TextSpecs({0xFF21}), {});
    // Sanity check: the unmodified baseline is accepted.
    {
        std::vector<EncodedPES> baseline;
        TASK_CHECK(encoder.Encode(base, baseline));
    }

    // Each mutation below must be rejected.
    {
        Caption c = base;
        c.plane_width = 720;
        expect_failure(c);
    }
    {
        Caption c = base;
        c.has_builtin_sound = true;
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].is_ruby = true;
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].chars[0].char_width = 18;
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].chars[0].char_horizontal_spacing = 12;
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].chars[0].char_vertical_spacing = 16;
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].chars[0].char_horizontal_scale = 0.5f;
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].chars[0].char_vertical_scale = 2.0f;
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].chars[0].style = kCharStyleBold;
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].chars[0].enclosure_style = kEnclosureStyleTop;
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].chars[0].back_color = ColorRGBA(0, 0, 0, 255);
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].chars[0].text_color = ColorRGBA(1, 2, 3, 255);
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].chars[0].text_color = ColorRGBA(255, 0, 0, 128);
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].chars[0].x = -1;
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].chars[0].x = 960 - kSectionWidth + 1;
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].chars[0].y = 540 - kSectionHeight + 1;
        expect_failure(c);
    }
    {
        Caption c = base;
        c.regions[0].chars[0].type = CaptionCharType::kDRCS;  // missing drcs_map entry
        expect_failure(c);
    }
    return 0;
}

int CaseEmptyRegionsIsClear() {
    Caption caption;
    caption.pts = 7;
    caption.plane_width = 960;
    caption.plane_height = 540;

    Encoder encoder;
    std::vector<EncodedPES> packets;
    TASK_CHECK(encoder.Encode(caption, packets));
    TASK_CHECK(packets.size() == 2);

    WirePlan plan = ReadStatementPlan(packets[1].data);
    TASK_CHECK(plan.ok);
    TASK_CHECK(plan.begins_with_clear);
    TASK_CHECK(plan.chars.empty());
    TASK_CHECK(plan.drcs_definition_count == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// dictionary group
// ---------------------------------------------------------------------------

int CaseSharedGlyphSingleDefinition() {
    DRCS glyph = MakeGlyphData(8, 8, 4, 3);
    std::unordered_map<uint32_t, DRCS> map;
    map[0x100] = glyph;
    map[0x200] = glyph;  // same bitmap, different input code / md5

    std::vector<SpecChar> specs;
    SpecChar a;
    a.drcs = true;
    a.drcs_code = 0x100;
    a.x = 0;
    a.y = 0;
    a.color = 6;
    specs.push_back(a);
    SpecChar b = a;
    b.drcs_code = 0x200;
    b.x = 40;
    specs.push_back(b);

    Encoder encoder;
    std::vector<EncodedPES> packets;
    TASK_CHECK(encoder.Encode(MakeCaption(specs, map), packets));

    WirePlan plan = ReadStatementPlan(packets[1].data);
    TASK_CHECK(plan.ok);
    TASK_CHECK(plan.drcs_definition_count == 1);
    TASK_CHECK(plan.chars.size() == 2);
    TASK_CHECK(plan.chars[0].set == WireSet::kDRCS);
    TASK_CHECK(plan.chars[1].set == WireSet::kDRCS);
    // Both references resolve to the same DRCS-1 slot.
    TASK_CHECK(plan.chars[0].first == plan.chars[1].first);

    std::vector<Caption> decoded = DecodePackets(packets);
    TASK_CHECK(decoded.size() == 1);
    std::vector<const CaptionChar*> chars = Flatten(decoded[0]);
    TASK_CHECK(chars.size() == 2);
    TASK_CHECK(chars[0]->type == CaptionCharType::kDRCS);
    TASK_CHECK(chars[1]->type == CaptionCharType::kDRCS);
    auto it0 = decoded[0].drcs_map.find(chars[0]->drcs_code);
    auto it1 = decoded[0].drcs_map.find(chars[1]->drcs_code);
    TASK_CHECK(it0 != decoded[0].drcs_map.end());
    TASK_CHECK(it1 != decoded[0].drcs_map.end());
    TASK_CHECK(CanonicalPixels(it0->second) == CanonicalPixels(glyph));
    TASK_CHECK(CanonicalPixels(it1->second) == CanonicalPixels(glyph));
    TASK_CHECK(chars[0]->drcs_code == chars[1]->drcs_code);
    return 0;
}

int CaseDrcsReplacedUsesBitmap() {
    DRCS glyph = MakeGlyphData(16, 18, 2, 9);
    glyph.alternative_text = "Z";
    glyph.alternative_ucs4 = 0x5A;

    std::unordered_map<uint32_t, DRCS> map;
    map[0x77] = glyph;

    std::vector<SpecChar> specs;
    SpecChar s;
    s.drcs = true;
    s.drcs_code = 0x77;
    s.x = 120;
    s.y = 240;
    s.color = 4;
    specs.push_back(s);

    Caption caption = MakeCaption(specs, map);
    caption.regions[0].chars[0].type = CaptionCharType::kDRCSReplaced;
    caption.regions[0].chars[0].codepoint = 0x5A;

    Encoder encoder;
    std::vector<EncodedPES> packets;
    TASK_CHECK(encoder.Encode(caption, packets));

    std::vector<Caption> decoded = DecodePackets(packets);
    TASK_CHECK(decoded.size() == 1);
    std::vector<const CaptionChar*> chars = Flatten(decoded[0]);
    TASK_CHECK(chars.size() == 1);
    // The transmitted glyph is the bitmap, not "Z"; whether the decoder labels
    // it as kDRCS or kDRCSReplaced depends on its replacement table.
    TASK_CHECK(chars[0]->type == CaptionCharType::kDRCS || chars[0]->type == CaptionCharType::kDRCSReplaced);
    auto it = decoded[0].drcs_map.find(chars[0]->drcs_code);
    TASK_CHECK(it != decoded[0].drcs_map.end());
    TASK_CHECK(CanonicalPixels(it->second) == CanonicalPixels(glyph));
    TASK_CHECK(chars[0]->x == 120);
    TASK_CHECK(chars[0]->y == 240);
    TASK_CHECK(chars[0]->text_color.u32 == PaletteColor(4).u32);
    return 0;
}

int CaseSameCodeDifferentBitmapReplacesSlot() {
    // Same input drcs_code: the second call must replace the cached glyph in
    // its slot and emit a fresh definition, not reuse the stale bitmap.
    DRCS first_bitmap = MakeGlyphData(8, 8, 4, 61);
    DRCS second_bitmap = MakeGlyphData(10, 6, 2, 62);

    std::vector<SpecChar> specs;
    SpecChar s;
    s.drcs = true;
    s.drcs_code = 7;
    s.x = 0;
    s.y = 0;
    s.color = 7;
    specs.push_back(s);

    Encoder encoder;
    std::vector<EncodedPES> packets;
    std::vector<EncodedPES> session;

    {
        std::unordered_map<uint32_t, DRCS> map;
        map[7] = first_bitmap;
        TASK_CHECK(encoder.Encode(MakeCaption(specs, map), packets));
        TASK_CHECK(ReadStatementPlan(packets[1].data).drcs_definition_count == 1);
        session.insert(session.end(), packets.begin(), packets.end());
    }
    {
        std::unordered_map<uint32_t, DRCS> map;
        map[7] = second_bitmap;
        TASK_CHECK(encoder.Encode(MakeCaption(specs, map), packets));
        TASK_CHECK(packets.size() == 1);
        // The changed bitmap must be redefined in this packet.
        TASK_CHECK(ReadStatementPlan(packets[0].data).drcs_definition_count == 1);
        session.insert(session.end(), packets.begin(), packets.end());
    }

    std::vector<Caption> decoded = DecodePackets(session);
    TASK_CHECK(decoded.size() == 2);
    std::vector<const CaptionChar*> second_chars = Flatten(decoded[1]);
    TASK_CHECK(second_chars.size() == 1);
    auto it = decoded[1].drcs_map.find(second_chars[0]->drcs_code);
    TASK_CHECK(it != decoded[1].drcs_map.end());
    TASK_CHECK(it->second.width == second_bitmap.width);
    TASK_CHECK(it->second.height == second_bitmap.height);
    TASK_CHECK(it->second.depth == second_bitmap.depth);
    TASK_CHECK(CanonicalPixels(it->second) == CanonicalPixels(second_bitmap));
    TASK_CHECK(CanonicalPixels(it->second) != CanonicalPixels(first_bitmap));
    return 0;
}

int CaseTrailingBitsIgnoredForIdentity() {
    // Two glyphs identical in effective pixels but differing in unused tail bits.
    DRCS a = MakeGlyphData(3, 3, 2, 1);  // 9 bits -> 2 bytes, 7 tail bits
    DRCS b = a;
    b.pixels.back() ^= 0x7F;
    b.md5 = "different";
    TASK_CHECK(CanonicalPixels(a) == CanonicalPixels(b));

    std::unordered_map<uint32_t, DRCS> map;
    map[1] = a;
    map[2] = b;

    std::vector<SpecChar> specs;
    SpecChar s;
    s.drcs = true;
    s.drcs_code = 1;
    s.x = 0;
    s.y = 0;
    specs.push_back(s);
    s.drcs_code = 2;
    s.x = 40;
    specs.push_back(s);

    Encoder encoder;
    std::vector<EncodedPES> packets;
    TASK_CHECK(encoder.Encode(MakeCaption(specs, map), packets));
    TASK_CHECK(ReadStatementPlan(packets[1].data).drcs_definition_count == 1);
    return 0;
}

int CaseSlotExhaustion() {
    std::unordered_map<uint32_t, DRCS> map;
    std::vector<SpecChar> specs;
    for (int i = 0; i < 95; i++) {
        DRCS g = MakeGlyphData(4, 4, 2, static_cast<uint32_t>(i) + 100);
        map[static_cast<uint32_t>(i)] = g;
        SpecChar s;
        s.drcs = true;
        s.drcs_code = static_cast<uint32_t>(i);
        s.x = (i % 24) * kSectionWidth;
        s.y = (i / 24) * kSectionHeight;
        s.color = i % 8;
        specs.push_back(s);
    }

    Encoder encoder;
    std::vector<EncodedPES> out;
    TASK_CHECK(!encoder.Encode(MakeCaption(specs, map), out));
    TASK_CHECK(out.empty());

    // 94 fits.
    std::vector<SpecChar> specs94(specs.begin(), specs.begin() + 94);
    std::vector<EncodedPES> packets;
    TASK_CHECK(encoder.Encode(MakeCaption(specs94, map), packets));
    TASK_CHECK(packets.size() == 2);
    TASK_CHECK(ReadStatementPlan(packets[1].data).drcs_definition_count == 94);

    std::vector<Caption> decoded = DecodePackets(packets);
    TASK_CHECK(decoded.size() == 1);
    std::vector<const CaptionChar*> chars = Flatten(decoded[0]);
    TASK_CHECK(chars.size() == 94);
    for (int i = 0; i < 94; i++) {
        auto it = decoded[0].drcs_map.find(chars[static_cast<size_t>(i)]->drcs_code);
        TASK_CHECK(it != decoded[0].drcs_map.end());
        TASK_CHECK(CanonicalPixels(it->second) == CanonicalPixels(map[static_cast<uint32_t>(i)]));
    }
    return 0;
}

int CaseEvictionKeepsReferenced() {
    // Prime the cache with 94 distinct glyphs.
    std::unordered_map<uint32_t, DRCS> map;
    std::vector<DRCS> glyphs;
    std::vector<SpecChar> first_specs;
    for (int i = 0; i < 94; i++) {
        DRCS g = MakeGlyphData(5, 5, 4, static_cast<uint32_t>(i) + 500);
        glyphs.push_back(g);
        map[static_cast<uint32_t>(i)] = g;
        SpecChar s;
        s.drcs = true;
        s.drcs_code = static_cast<uint32_t>(i);
        s.x = (i % 24) * kSectionWidth;
        s.y = (i / 24) * kSectionHeight;
        s.color = i % 8;
        first_specs.push_back(s);
    }
    // A brand-new glyph to force an eviction.
    DRCS fresh = MakeGlyphData(6, 6, 2, 999);
    map[1000] = fresh;

    Encoder encoder;
    std::vector<EncodedPES> session;
    std::vector<EncodedPES> packets;

    TASK_CHECK(encoder.Encode(MakeCaption(first_specs, map), packets));
    TASK_CHECK(packets.size() == 2);
    session.insert(session.end(), packets.begin(), packets.end());

    // Second caption references glyphs 1..93 (dropping glyph 0) plus the new one.
    std::vector<SpecChar> second_specs;
    for (int i = 1; i < 94; i++) {
        SpecChar s;
        s.drcs = true;
        s.drcs_code = static_cast<uint32_t>(i);
        s.x = ((i - 1) % 24) * kSectionWidth;
        s.y = ((i - 1) / 24) * kSectionHeight;
        s.color = i % 8;
        second_specs.push_back(s);
    }
    SpecChar fresh_spec;
    fresh_spec.drcs = true;
    fresh_spec.drcs_code = 1000;
    fresh_spec.x = (93 % 24) * kSectionWidth;
    fresh_spec.y = (93 / 24) * kSectionHeight;
    fresh_spec.color = 7;
    second_specs.push_back(fresh_spec);

    TASK_CHECK(encoder.Encode(MakeCaption(second_specs, map), packets));
    TASK_CHECK(packets.size() == 1);  // no management retransmission
    WirePlan plan = ReadStatementPlan(packets[0].data);
    TASK_CHECK(plan.ok);
    // Only the new glyph needs a definition; the 93 still-referenced cached
    // glyphs must not be evicted or redefined.
    TASK_CHECK(plan.drcs_definition_count == 1);
    session.insert(session.end(), packets.begin(), packets.end());

    // Decode the whole session so cached definitions remain visible.
    std::vector<Caption> decoded = DecodePackets(session);
    TASK_CHECK(decoded.size() == 2);
    std::vector<const CaptionChar*> chars = Flatten(decoded[1]);
    TASK_CHECK(chars.size() == 94);
    for (int i = 1; i < 94; i++) {
        auto it = decoded[1].drcs_map.find(chars[static_cast<size_t>(i - 1)]->drcs_code);
        TASK_CHECK(it != decoded[1].drcs_map.end());
        TASK_CHECK(CanonicalPixels(it->second) == CanonicalPixels(glyphs[static_cast<size_t>(i)]));
    }
    auto fresh_it = decoded[1].drcs_map.find(chars[93]->drcs_code);
    TASK_CHECK(fresh_it != decoded[1].drcs_map.end());
    TASK_CHECK(CanonicalPixels(fresh_it->second) == CanonicalPixels(fresh));
    return 0;
}

int CaseCachePersistsUntilReset() {
    DRCS glyph = MakeGlyphData(8, 8, 4, 21);
    std::unordered_map<uint32_t, DRCS> map;
    map[5] = glyph;
    std::vector<SpecChar> specs;
    SpecChar s;
    s.drcs = true;
    s.drcs_code = 5;
    s.x = 0;
    s.y = 0;
    specs.push_back(s);
    Caption caption = MakeCaption(specs, map);

    Encoder encoder;
    std::vector<EncodedPES> packets;

    TASK_CHECK(encoder.Encode(caption, packets));
    TASK_CHECK(packets.size() == 2);
    WirePlan first = ReadStatementPlan(packets[1].data);
    TASK_CHECK(first.drcs_definition_count == 1);

    // Repeated identical caption: no management, no redefinition.
    TASK_CHECK(encoder.Encode(caption, packets));
    TASK_CHECK(packets.size() == 1);
    WirePlan second = ReadStatementPlan(packets[0].data);
    TASK_CHECK(second.ok);
    TASK_CHECK(second.drcs_definition_count == 0);
    TASK_CHECK(!second.has_drcs_unit);

    // After Reset: management returns and the glyph is defined again.
    encoder.Reset();
    TASK_CHECK(encoder.Encode(caption, packets));
    TASK_CHECK(packets.size() == 2);
    WirePlan third = ReadStatementPlan(packets[1].data);
    TASK_CHECK(third.ok);
    TASK_CHECK(third.drcs_definition_count == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// rollback group
// ---------------------------------------------------------------------------

Caption MakeOverlongCaption() {
    // Two-byte kanji plus a per-character ACPS of roughly 12 bytes each keeps
    // each character around 15 bytes, so the total body exceeds the 65535-byte
    // data group payload limit.
    std::vector<SpecChar> specs;
    for (int i = 0; i < 8000; i++) {
        SpecChar s;
        s.codepoint = 0x65E5;
        s.x = (i % 24) * kSectionWidth;
        s.y = ((i / 24) % 9) * kSectionHeight;
        s.color = 7;
        specs.push_back(s);
    }
    return MakeCaption(specs, {}, 55);
}

int CaseOverlongFailsThenRetryMatchesVirgin() {
    Encoder encoder;
    std::vector<EncodedPES> out;
    out.push_back(EncodedPES{1, {0x11, 0x22}});
    std::vector<EncodedPES> before = out;

    TASK_CHECK(!encoder.Encode(MakeOverlongCaption(), out));
    TASK_CHECK(SamePackets(out, before));

    // Retry with a valid caption; must match a never-failed encoder exactly.
    DRCS glyph = MakeGlyphData(8, 8, 4, 44);
    std::unordered_map<uint32_t, DRCS> map;
    map[9] = glyph;
    std::vector<SpecChar> specs;
    SpecChar s;
    s.drcs = true;
    s.drcs_code = 9;
    s.x = 80;
    s.y = 120;
    s.color = 3;
    specs.push_back(s);
    specs.push_back(SpecChar{false, 0x65E5, 0, 0, 0, 7});
    Caption caption = MakeCaption(specs, map, 777);

    std::vector<EncodedPES> retried;
    TASK_CHECK(encoder.Encode(caption, retried));

    Encoder virgin;
    std::vector<EncodedPES> expected;
    TASK_CHECK(virgin.Encode(caption, expected));

    TASK_CHECK(SamePackets(retried, expected));
    return 0;
}

int CaseUnsupportedFailsThenRetryMatchesVirgin() {
    Encoder encoder;
    std::vector<EncodedPES> out;
    out.push_back(EncodedPES{PTS_NOPTS, {0x33}});
    std::vector<EncodedPES> before = out;

    Caption bad = MakeCaption(TextSpecs({0xFF21}), {});
    bad.regions[0].chars[0].style = kCharStyleItalic;
    TASK_CHECK(!encoder.Encode(bad, out));
    TASK_CHECK(SamePackets(out, before));

    // A failed DRCS slot overflow must likewise leave state untouched.
    std::unordered_map<uint32_t, DRCS> map;
    std::vector<SpecChar> over_specs;
    for (int i = 0; i < 95; i++) {
        map[static_cast<uint32_t>(i)] = MakeGlyphData(4, 4, 2, static_cast<uint32_t>(i) + 1);
        SpecChar s;
        s.drcs = true;
        s.drcs_code = static_cast<uint32_t>(i);
        s.x = (i % 24) * kSectionWidth;
        s.y = (i / 24) * kSectionHeight;
        over_specs.push_back(s);
    }
    TASK_CHECK(!encoder.Encode(MakeCaption(over_specs, map), out));
    TASK_CHECK(SamePackets(out, before));

    Caption good = MakeCaption(TextSpecs({0x3042, 0x30A2}), {}, PTS_NOPTS);
    std::vector<EncodedPES> retried;
    TASK_CHECK(encoder.Encode(good, retried));

    Encoder virgin;
    std::vector<EncodedPES> expected;
    TASK_CHECK(virgin.Encode(good, expected));
    TASK_CHECK(SamePackets(retried, expected));
    return 0;
}

int CaseInputAndReturnedPacketsStable() {
    Encoder encoder;
    std::vector<EncodedPES> out;

    DRCS glyph = MakeGlyphData(9, 7, 4, 123);
    std::unordered_map<uint32_t, DRCS> map;
    map[42] = glyph;
    std::vector<SpecChar> specs;
    SpecChar s;
    s.drcs = true;
    s.drcs_code = 42;
    s.x = 0;
    s.y = 0;
    s.color = 2;
    specs.push_back(s);
    Caption caption = MakeCaption(specs, map, 100);

    TASK_CHECK(encoder.Encode(caption, out));
    TASK_CHECK(out.size() == 2);
    std::vector<EncodedPES> snapshot = out;

    // The input caption must be untouched.
    TASK_CHECK(caption.regions.size() == 1);
    TASK_CHECK(caption.regions[0].chars.size() == 1);
    TASK_CHECK(caption.regions[0].chars[0].x == 0);
    TASK_CHECK(caption.regions[0].chars[0].drcs_code == 42);
    TASK_CHECK(caption.drcs_map.size() == 1);
    TASK_CHECK(caption.pts == 100);

    // Subsequent calls must not mutate previously returned packets.
    Caption second = MakeCaption(TextSpecs({0x65E5, 0x672C}), {}, 200);
    TASK_CHECK(encoder.Encode(second, out));
    TASK_CHECK(SamePackets(out, out));
    TASK_CHECK(snapshot.size() == 2);
    TASK_CHECK(snapshot[0].data != snapshot[1].data);

    // Re-encoding the first caption emits only a statement (the glyph is still
    // cached). Slot assignment may legitimately differ after other calls, so we
    // verify the decoded payload rather than comparing raw bytes.
    std::vector<EncodedPES> again;
    TASK_CHECK(encoder.Encode(caption, again));
    TASK_CHECK(again.size() == 1);
    TASK_CHECK(again[0].pts == 100);
    // A cache hit must not re-emit the glyph definition.
    TASK_CHECK(ReadStatementPlan(again[0].data).drcs_definition_count == 0);

    // Decode the whole session so the cached definition is available to the
    // statement-only re-encode; its content must match the original caption.
    std::vector<EncodedPES> session = snapshot;
    session.insert(session.end(), again.begin(), again.end());
    std::vector<Caption> decoded_session = DecodePackets(session);
    TASK_CHECK(decoded_session.size() == 2);
    std::vector<const CaptionChar*> re_chars = Flatten(decoded_session.back());
    TASK_CHECK(re_chars.size() == 1);
    TASK_CHECK(re_chars[0]->x == 0 && re_chars[0]->y == 0);
    TASK_CHECK(re_chars[0]->text_color.u32 == PaletteColor(2).u32);
    auto re_it = decoded_session.back().drcs_map.find(re_chars[0]->drcs_code);
    TASK_CHECK(re_it != decoded_session.back().drcs_map.end());
    TASK_CHECK(CanonicalPixels(re_it->second) == CanonicalPixels(glyph));

    // The earlier snapshot packets were never mutated by later calls.
    std::vector<Caption> decoded_snapshot = DecodePackets(snapshot);
    TASK_CHECK(decoded_snapshot.size() == 1);
    TASK_CHECK(Flatten(decoded_snapshot[0]).size() == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// wire group
// ---------------------------------------------------------------------------

int CaseHeaderSizeAndCRC() {
    DRCS glyph = MakeGlyphData(8, 8, 4, 31);
    std::unordered_map<uint32_t, DRCS> map;
    map[1] = glyph;
    std::vector<SpecChar> specs;
    SpecChar s;
    s.drcs = true;
    s.drcs_code = 1;
    s.x = 0;
    s.y = 0;
    specs.push_back(s);
    specs.push_back(SpecChar{false, 0x65E5, 0, 40, 0, 7});

    Encoder encoder;
    std::vector<EncodedPES> packets;
    TASK_CHECK(encoder.Encode(MakeCaption(specs, map), packets));
    TASK_CHECK(packets.size() == 2);

    for (const EncodedPES& packet : packets) {
        TASK_CHECK(packet.data.size() >= 10);
        TASK_CHECK(packet.data[0] == 0x80);
        TASK_CHECK(packet.data[1] == 0xFF);
        TASK_CHECK(packet.data[2] == 0xF0);
        ParsedGroup group = ParseGroup(packet.data);
        TASK_CHECK(group.ok);
        // CRC is computed over group header through body, excluding itself.
        TASK_CHECK(group.crc_field == ComputeCRC16(packet.data.data() + 3, 5 + group.declared_size));
        // Group length never exceeds 65535.
        TASK_CHECK(group.declared_size <= 0xFFFF);
    }

    TASK_CHECK(packets[0].data[3] == 0x00);  // management
    TASK_CHECK(packets[1].data[3] == 0x04);  // statement, first language

    ParsedGroup mgmt = ParseGroup(packets[0].data);
    TASK_CHECK(mgmt.payload.size() == 10);
    TASK_CHECK(mgmt.payload[0] == 0x00);  // TMD
    TASK_CHECK(mgmt.payload[1] == 0x01);  // one language
    TASK_CHECK(mgmt.payload[2] == 0x00);  // language tag #0
    TASK_CHECK(mgmt.payload[3] == 0x6A && mgmt.payload[4] == 0x70 && mgmt.payload[5] == 0x6E);
    TASK_CHECK((mgmt.payload[6] & 0xF0) == 0x80);  // format 8: 960x540
    TASK_CHECK((mgmt.payload[6] & 0x0C) == 0x00);  // TCS 0
    return 0;
}

int CaseManagementOncePerSession() {
    Encoder encoder;
    std::vector<EncodedPES> packets;
    Caption caption = MakeCaption(TextSpecs({0x65E5}), {}, 10);

    TASK_CHECK(encoder.Encode(caption, packets));
    TASK_CHECK(packets.size() == 2);
    TASK_CHECK(packets[0].data[3] == 0x00);
    TASK_CHECK(packets[1].data[3] == 0x04);
    TASK_CHECK(packets[0].pts == 10 && packets[1].pts == 10);

    TASK_CHECK(encoder.Encode(caption, packets));
    TASK_CHECK(packets.size() == 1);
    TASK_CHECK(packets[0].data[3] == 0x04);

    encoder.Reset();
    TASK_CHECK(encoder.Encode(caption, packets));
    TASK_CHECK(packets.size() == 2);
    TASK_CHECK(packets[0].data[3] == 0x00);
    return 0;
}

int CasePtsPassthrough() {
    Encoder encoder;
    std::vector<EncodedPES> packets;

    TASK_CHECK(encoder.Encode(MakeCaption(TextSpecs({0x65E5}), {}, PTS_NOPTS), packets));
    TASK_CHECK(packets.size() == 2);
    TASK_CHECK(packets[0].pts == PTS_NOPTS);
    TASK_CHECK(packets[1].pts == PTS_NOPTS);

    TASK_CHECK(encoder.Encode(MakeCaption(TextSpecs({0x672C}), {}, 987654321), packets));
    TASK_CHECK(packets.size() == 1);
    TASK_CHECK(packets[0].pts == 987654321);
    return 0;
}

int CaseInterleavedWireRoundtrip() {
    // Mixed: kanji, fullwidth Latin, ideographic space, colors, discrete
    // positions and DRCS, interleaved in one caption.
    std::unordered_map<uint32_t, DRCS> map;
    DRCS glyph_a = MakeGlyphData(12, 9, 4, 11);
    DRCS glyph_b = MakeGlyphData(4, 4, 2, 12);
    map[0xA] = glyph_a;
    map[0xB] = glyph_b;

    std::vector<SpecChar> specs = {
        {false, 0xFF21, 0, 0, 0, 7},
        {false, 0x65E5, 0, 40, 0, 1},
        {true, 0, 0xA, 80, 0, 6},
        {false, 0x3000, 0, 120, 0, 7},
        {false, 0x3042, 0, 160, 0, 2},
        {true, 0, 0xB, 200, 0, 5},
        {false, 0x30A2, 0, 240, 0, 4},
        {false, 0x672C, 0, 400, 180, 3},
        {true, 0, 0xA, 440, 180, 0},
    };
    Caption caption = MakeCaption(specs, map, 31415);

    Encoder encoder;
    std::vector<EncodedPES> packets;
    TASK_CHECK(encoder.Encode(caption, packets));

    // Independent wire interpretation.
    WirePlan plan = ReadStatementPlan(packets[1].data);
    TASK_CHECK(plan.ok);
    TASK_CHECK(plan.begins_with_clear);
    TASK_CHECK(plan.chars.size() == specs.size());
    TASK_CHECK(plan.drcs_definition_count == 2);
    for (size_t i = 0; i < specs.size(); i++) {
        TASK_CHECK(plan.chars[i].x == specs[i].x);
        TASK_CHECK(plan.chars[i].y == specs[i].y);
        TASK_CHECK(plan.chars[i].color == specs[i].color);
        if (specs[i].drcs) {
            TASK_CHECK(plan.chars[i].set == WireSet::kDRCS);
            TASK_CHECK(!plan.chars[i].two_bytes);
        } else if (specs[i].codepoint >= 0xFF01 && specs[i].codepoint <= 0xFF5E) {
            TASK_CHECK(plan.chars[i].set == WireSet::kAlphanumeric);
        } else if (specs[i].codepoint >= 0x3041 && specs[i].codepoint <= 0x309F) {
            TASK_CHECK(plan.chars[i].set == WireSet::kHiragana);
        } else if (specs[i].codepoint >= 0x30A1 && specs[i].codepoint <= 0x30FF) {
            TASK_CHECK(plan.chars[i].set == WireSet::kKatakana);
        } else if (specs[i].codepoint == 0x3000) {
            // Ideographic space is valid in the hiragana set used here.
            TASK_CHECK(plan.chars[i].set == WireSet::kHiragana);
        } else {
            TASK_CHECK(plan.chars[i].set == WireSet::kKanji);
            TASK_CHECK(plan.chars[i].two_bytes);
        }
    }

    // Decoder round-trip.
    std::vector<Caption> decoded = DecodePackets(packets);
    TASK_CHECK(decoded.size() == 1);
    std::vector<const CaptionChar*> chars = Flatten(decoded[0]);
    TASK_CHECK(chars.size() == specs.size());
    for (size_t i = 0; i < specs.size(); i++) {
        TASK_CHECK(chars[i]->x == specs[i].x);
        TASK_CHECK(chars[i]->y == specs[i].y);
        TASK_CHECK(chars[i]->text_color.u32 == PaletteColor(specs[i].color).u32);
        if (specs[i].drcs) {
            TASK_CHECK(chars[i]->type == CaptionCharType::kDRCS);
            const DRCS& expected = specs[i].drcs_code == 0xA ? glyph_a : glyph_b;
            auto it = decoded[0].drcs_map.find(chars[i]->drcs_code);
            TASK_CHECK(it != decoded[0].drcs_map.end());
            TASK_CHECK(CanonicalPixels(it->second) == CanonicalPixels(expected));
            TASK_CHECK(it->second.width == expected.width);
            TASK_CHECK(it->second.height == expected.height);
            TASK_CHECK(it->second.depth == expected.depth);
            TASK_CHECK(it->second.depth_bits == expected.depth_bits);
        } else {
            TASK_CHECK(chars[i]->type == CaptionCharType::kText);
            TASK_CHECK(chars[i]->codepoint == specs[i].codepoint);
        }
    }
    return 0;
}

int CaseDrcsDimensionsAndDepths() {
    // Boundary dimensions and both depths.
    struct Case {
        int w;
        int h;
        int depth;
    };
    std::vector<Case> cases = {
        {1, 1, 2}, {64, 1, 2}, {1, 64, 4}, {64, 64, 4}, {36, 36, 2}, {33, 37, 4},
    };

    for (const Case& c : cases) {
        std::unordered_map<uint32_t, DRCS> map;
        map[1] = MakeGlyphData(c.w, c.h, c.depth, static_cast<uint32_t>(c.w * 7 + c.h));
        std::vector<SpecChar> specs;
        SpecChar s;
        s.drcs = true;
        s.drcs_code = 1;
        s.x = 0;
        s.y = 0;
        specs.push_back(s);

        Encoder encoder;
        std::vector<EncodedPES> packets;
        TASK_CHECK(encoder.Encode(MakeCaption(specs, map), packets));

        std::vector<Caption> decoded = DecodePackets(packets);
        TASK_CHECK(decoded.size() == 1);
        std::vector<const CaptionChar*> chars = Flatten(decoded[0]);
        TASK_CHECK(chars.size() == 1);
        auto it = decoded[0].drcs_map.find(chars[0]->drcs_code);
        TASK_CHECK(it != decoded[0].drcs_map.end());
        TASK_CHECK(it->second.width == c.w);
        TASK_CHECK(it->second.height == c.h);
        TASK_CHECK(it->second.depth == c.depth);
        TASK_CHECK(it->second.depth_bits == (c.depth == 2 ? 1 : 2));
        TASK_CHECK(CanonicalPixels(it->second) == CanonicalPixels(map[1]));
    }

    // Invalid: zero size, oversize, unsupported depth, mismatched depth_bits.
    auto rejects = [](DRCS glyph) {
        std::unordered_map<uint32_t, DRCS> map;
        map[1] = glyph;
        std::vector<SpecChar> specs;
        SpecChar s;
        s.drcs = true;
        s.drcs_code = 1;
        specs.push_back(s);
        Encoder encoder;
        std::vector<EncodedPES> out;
        return !encoder.Encode(MakeCaption(specs, map), out) && out.empty();
    };

    DRCS bad = MakeGlyphData(4, 4, 2, 1);
    bad.width = 0;
    TASK_CHECK(rejects(bad));
    bad = MakeGlyphData(4, 4, 2, 1);
    bad.height = 65;
    TASK_CHECK(rejects(bad));
    bad = MakeGlyphData(4, 4, 2, 1);
    bad.depth = 8;
    TASK_CHECK(rejects(bad));
    bad = MakeGlyphData(4, 4, 4, 1);
    bad.depth_bits = 1;
    TASK_CHECK(rejects(bad));
    bad = MakeGlyphData(4, 4, 2, 1);
    bad.pixels.resize(1);
    TASK_CHECK(rejects(bad));
    return 0;
}

int CaseCrossProcessExampleRoundtrip() {
    // The example binary is produced by task_support/run.sh in .task-build/.
    const char* tmpdir = std::getenv("TMPDIR");
    std::string path = std::string(tmpdir ? tmpdir : ".") + "/aribcc_encode_roundtrip.pes";
    std::remove(path.c_str());

    std::string write_cmd = "./.task-build/caption_encode " + path + " >/dev/null 2>&1";
    TASK_CHECK(std::system(write_cmd.c_str()) == 0);

    std::ifstream file(path, std::ios::binary);
    TASK_CHECK(file.good());
    TASK_CHECK(static_cast<bool>(file));

    // Parse the container: 8-byte big-endian signed pts, 4-byte big-endian length.
    std::vector<EncodedPES> records;
    while (true) {
        uint8_t header[12];
        file.read(reinterpret_cast<char*>(header), sizeof(header));
        if (file.gcount() == 0) {
            break;
        }
        TASK_CHECK(file.gcount() == static_cast<std::streamsize>(sizeof(header)));
        uint64_t raw_pts = 0;
        for (int i = 0; i < 8; i++) {
            raw_pts = (raw_pts << 8) | header[i];
        }
        uint32_t length = (static_cast<uint32_t>(header[8]) << 24) |
                          (static_cast<uint32_t>(header[9]) << 16) |
                          (static_cast<uint32_t>(header[10]) << 8) |
                          static_cast<uint32_t>(header[11]);
        TASK_CHECK(length > 0);
        TASK_CHECK(length <= (1u << 20));
        EncodedPES packet;
        packet.pts = static_cast<int64_t>(raw_pts);
        packet.data.resize(length);
        file.read(reinterpret_cast<char*>(packet.data.data()), length);
        TASK_CHECK(file.gcount() == static_cast<std::streamsize>(length));
        records.push_back(std::move(packet));
    }
    TASK_CHECK(records.size() >= 3);

    // In-process verification of the same file.
    std::vector<Caption> captions = DecodePackets(records);
    TASK_CHECK(captions.size() >= 2);

    std::vector<const CaptionChar*> first = Flatten(captions[0]);
    std::vector<const CaptionChar*> second = Flatten(captions[1]);
    TASK_CHECK(first.size() >= 3);
    TASK_CHECK(second.size() >= 3);

    // Normal characters of the first two captions.
    TASK_CHECK(first[0]->type == CaptionCharType::kText && first[0]->codepoint == 0xFF21);
    TASK_CHECK(first[1]->type == CaptionCharType::kText && first[1]->codepoint == 0x65E5);
    TASK_CHECK(second[0]->type == CaptionCharType::kText && second[0]->codepoint == 0x3042);
    TASK_CHECK(second[1]->type == CaptionCharType::kText && second[1]->codepoint == 0x30A2);

    // Same DRCS bitmap in both captions.
    TASK_CHECK(first.back()->type == CaptionCharType::kDRCS);
    TASK_CHECK(second.back()->type == CaptionCharType::kDRCS);
    auto it1 = captions[0].drcs_map.find(first.back()->drcs_code);
    auto it2 = captions[1].drcs_map.find(second.back()->drcs_code);
    TASK_CHECK(it1 != captions[0].drcs_map.end());
    TASK_CHECK(it2 != captions[1].drcs_map.end());
    TASK_CHECK(it1->second.width == it2->second.width);
    TASK_CHECK(it1->second.height == it2->second.height);
    TASK_CHECK(CanonicalPixels(it1->second) == CanonicalPixels(it2->second));
    TASK_CHECK(!it1->second.pixels.empty());

    // Decode mode must agree, in a separate process.
    std::string decode_cmd = "./.task-build/caption_encode --decode " + path + " >/dev/null 2>&1";
    TASK_CHECK(std::system(decode_cmd.c_str()) == 0);

    std::remove(path.c_str());
    return 0;
}

}  // namespace

extern const struct task_case task_cases[] = {
    {"mapping", "jis_mapping_roundtrip", CaseJisMappingRoundtrip},
    {"mapping", "unsupported_codepoint", CaseUnsupportedCodepointRejected},
    {"mapping", "unsupported_attributes", CaseUnsupportedAttributesRejected},
    {"mapping", "empty_regions_clear", CaseEmptyRegionsIsClear},
    {"dictionary", "shared_glyph_single_definition", CaseSharedGlyphSingleDefinition},
    {"dictionary", "drcs_replaced_uses_bitmap", CaseDrcsReplacedUsesBitmap},
    {"dictionary", "same_code_different_bitmap", CaseSameCodeDifferentBitmapReplacesSlot},
    {"dictionary", "trailing_bits_ignored", CaseTrailingBitsIgnoredForIdentity},
    {"dictionary", "slot_exhaustion", CaseSlotExhaustion},
    {"dictionary", "eviction_keeps_referenced", CaseEvictionKeepsReferenced},
    {"dictionary", "cache_persists_until_reset", CaseCachePersistsUntilReset},
    {"rollback", "overlong_then_retry", CaseOverlongFailsThenRetryMatchesVirgin},
    {"rollback", "unsupported_then_retry", CaseUnsupportedFailsThenRetryMatchesVirgin},
    {"rollback", "input_and_packets_stable", CaseInputAndReturnedPacketsStable},
    {"wire", "header_size_crc", CaseHeaderSizeAndCRC},
    {"wire", "management_once", CaseManagementOncePerSession},
    {"wire", "pts_passthrough", CasePtsPassthrough},
    {"wire", "interleaved_roundtrip", CaseInterleavedWireRoundtrip},
    {"wire", "drcs_dimensions_depths", CaseDrcsDimensionsAndDepths},
    {"wire", "cross_process_example", CaseCrossProcessExampleRoundtrip},
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
