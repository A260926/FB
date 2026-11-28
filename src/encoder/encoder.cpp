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

#include "aribcaption/encoder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "decoder/b24_colors.hpp"
#include "decoder/b24_conv_tables.hpp"
#include "decoder/b24_gaiji_table.hpp"

namespace aribcaption {

namespace {

constexpr int kPlaneWidth = 960;
constexpr int kPlaneHeight = 540;
constexpr int kCharWidth = 36;
constexpr int kCharHeight = 36;
constexpr int kHorizontalSpacing = 4;
constexpr int kVerticalSpacing = 24;
constexpr int kSectionWidth = kCharWidth + kHorizontalSpacing;    // 40
constexpr int kSectionHeight = kCharHeight + kVerticalSpacing;    // 60
constexpr int kDRCS1SlotCount = 94;

constexpr uint8_t kDRCS1_F = 0x41;
constexpr uint8_t kDataUnitParameterStatement = 0x20;
constexpr uint8_t kDataUnitParameterDRCS = 0x30;

// Data group ids: 0b000000 (management), 0b000001 (statement for language #1)
constexpr uint8_t kDataGroupIdManagement = 0x00;
constexpr uint8_t kDataGroupIdStatementFirst = 0x04;

constexpr int kMaxDataGroupPayload = 0xFFFF;
constexpr size_t kMaxDataUnitPayload = 0xFFFFFF;

enum class TextSet : uint8_t {
    kKanji,
    kAlphanumeric,
    kHiragana,
    kKatakana,
    kAdditionalSymbols
};

struct TextCode {
    TextSet set = TextSet::kKanji;
    uint8_t first = 0;
    uint8_t second = 0;
    bool two_bytes = false;
};

// Build a reverse (Unicode codepoint -> JIS graphic code) map from the tables
// used by the decoder. Insertion order defines the preferred code when a
// codepoint can be represented by more than one set.
const std::unordered_map<uint32_t, TextCode>& GetTextMap() {
    static const std::unordered_map<uint32_t, TextCode> map = [] {
        std::unordered_map<uint32_t, TextCode> result;

        auto add_single = [&result](uint32_t cp, TextSet set, uint8_t code) {
            if (cp == 0xFFFD) {
                return;
            }
            TextCode text_code;
            text_code.set = set;
            text_code.first = code;
            text_code.two_bytes = false;
            result.emplace(cp, text_code);
        };

        auto add_double = [&result](uint32_t cp, TextSet set, uint8_t first, uint8_t second) {
            if (cp == 0xFFFD) {
                return;
            }
            TextCode text_code;
            text_code.set = set;
            text_code.first = first;
            text_code.second = second;
            text_code.two_bytes = true;
            result.emplace(cp, text_code);
        };

        // Fullwidth alphanumeric set (preferred for fullwidth Latin characters)
        for (size_t i = 0; i < 94; i++) {
            add_single(kAlphanumericTable_Fullwidth[i], TextSet::kAlphanumeric,
                       static_cast<uint8_t>(0x21 + i));
        }
        // Hiragana / Katakana sets
        for (size_t i = 0; i < 94; i++) {
            add_single(kHiraganaTable[i], TextSet::kHiragana, static_cast<uint8_t>(0x21 + i));
        }
        for (size_t i = 0; i < 94; i++) {
            add_single(kKatakanaTable[i], TextSet::kKatakana, static_cast<uint8_t>(0x21 + i));
        }
        // Additional symbols (ku >= 84)
        for (size_t i = 0; i < sizeof(kAdditionalSymbolsTable_Unicode) / sizeof(uint32_t); i++) {
            uint32_t cp = kAdditionalSymbolsTable_Unicode[i];
            size_t ku = 84 + i / 94;
            size_t ten = i % 94;
            if (ku > 94) {
                continue;
            }
            add_double(cp, TextSet::kAdditionalSymbols, static_cast<uint8_t>(0x21 + ku),
                       static_cast<uint8_t>(0x21 + ten));
        }
        // JIS X 0208 Kanji set (ku 1..84)
        constexpr size_t kKanjiTableEntries = 84 * 94;
        for (size_t i = 0; i < kKanjiTableEntries; i++) {
            uint32_t cp = kKanjiTable[i];
            size_t ku = i / 94;
            size_t ten = i % 94;
            add_double(cp, TextSet::kKanji, static_cast<uint8_t>(0x21 + ku),
                       static_cast<uint8_t>(0x21 + ten));
        }

        return result;
    }();
    return map;
}

int FindPalette0TextColor(const ColorRGBA& color) {
    if (color.a != 255) {
        return -1;
    }
    for (int i = 0; i < 8; i++) {
        const ColorRGBA& candidate = kB24ColorCLUT[0][i];
        if (candidate.r == color.r && candidate.g == color.g && candidate.b == color.b &&
            candidate.a == color.a) {
            return i;
        }
    }
    return -1;
}

struct Glyph {
    int width = 0;
    int height = 0;
    int depth = 0;
    int depth_bits = 0;
    std::vector<uint8_t> pixels;  // canonical, trailing unused bits zeroed
    std::string key;              // content identity, ignores code and md5
    int slot = -1;                // DRCS-1 slot (0..93), or -1 if not assigned
    bool fresh = false;           // needs a definition in the current packet

    size_t pixel_bytes() const {
        size_t bits = static_cast<size_t>(width) * static_cast<size_t>(height) *
                      static_cast<size_t>(depth_bits);
        return (bits + 7) / 8;
    }
};

bool MakeGlyph(const DRCS& drcs, Glyph& out) {
    if (drcs.width < 1 || drcs.width > 64 || drcs.height < 1 || drcs.height > 64) {
        return false;
    }
    if (drcs.depth != 2 && drcs.depth != 4) {
        return false;
    }
    int depth_bits = drcs.depth == 2 ? 1 : 2;
    if (drcs.depth_bits != depth_bits) {
        return false;
    }

    Glyph glyph;
    glyph.width = drcs.width;
    glyph.height = drcs.height;
    glyph.depth = drcs.depth;
    glyph.depth_bits = depth_bits;

    size_t bytes = glyph.pixel_bytes();
    if (drcs.pixels.size() < bytes) {
        return false;
    }
    glyph.pixels.assign(drcs.pixels.begin(), drcs.pixels.begin() + static_cast<std::ptrdiff_t>(bytes));

    size_t used_bits = static_cast<size_t>(glyph.width) * static_cast<size_t>(glyph.height) *
                       static_cast<size_t>(depth_bits);
    size_t tail_bits = used_bits % 8;
    if (tail_bits != 0 && !glyph.pixels.empty()) {
        glyph.pixels.back() &= static_cast<uint8_t>(0xFF << (8 - tail_bits));
    }

    glyph.key.reserve(4 + glyph.pixels.size());
    glyph.key.push_back(static_cast<char>(glyph.width));
    glyph.key.push_back(static_cast<char>(glyph.height));
    glyph.key.push_back(static_cast<char>(glyph.depth));
    glyph.key.push_back(static_cast<char>(glyph.depth_bits));
    glyph.key.append(reinterpret_cast<const char*>(glyph.pixels.data()), glyph.pixels.size());

    out = std::move(glyph);
    return true;
}

struct CharPlan {
    bool is_drcs = false;
    TextCode text;
    int glyph_index = -1;  // index into Work::glyphs
    int color = 7;
    int x = 0;
    int y = 0;
};

struct Work {
    std::vector<Glyph> glyphs;
    std::unordered_map<std::string, int> glyph_index;  // key -> index in glyphs
    std::array<int, kDRCS1SlotCount> slot_glyph{};     // slot -> index in glyphs, -1 if free
    std::vector<int> lru;                              // slots, least recently used first
    std::vector<int> new_slots;                        // slots needing definitions this packet
};

void AppendU16BE(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void AppendU24BE(std::vector<uint8_t>& out, size_t value) {
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

uint16_t CRC16_CCITT_FALSE(const uint8_t* data, size_t length) {
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

// Build a complete data group: private header, data group header, payload, CRC.
bool MakeDataGroup(uint8_t data_group_id, const std::vector<uint8_t>& payload,
                   std::vector<uint8_t>& out) {
    if (payload.size() > kMaxDataGroupPayload) {
        return false;
    }

    out.clear();
    out.reserve(3 + 5 + payload.size() + 2);

    out.push_back(0x80);  // data_identifier (caption)
    out.push_back(0xFF);  // private_stream_id
    out.push_back(0xF0);  // PES_data_packet_header_length = 0

    size_t crc_begin = out.size();
    out.push_back(data_group_id);
    out.push_back(0x00);
    out.push_back(0x00);
    AppendU16BE(out, static_cast<uint16_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());

    uint16_t crc = CRC16_CCITT_FALSE(out.data() + crc_begin, out.size() - crc_begin);
    AppendU16BE(out, crc);
    return true;
}

std::vector<uint8_t> MakeManagementPayload() {
    // TMD=0b00, one language, language tag #0 (first), DMF=0, "jpn",
    // format=8 (960x540 horizontal), TCS=0, no data units.
    return std::vector<uint8_t>{
        0x00,
        0x01,
        0x00,
        0x6A, 0x70, 0x6E,
        0x80,
        0x00, 0x00, 0x00,
    };
}

void AppendACS(std::vector<uint8_t>& body, int x, int y) {
    body.push_back(0x9B);  // CSI
    auto append_decimal = [&body](int value) {
        uint8_t digits[6];
        size_t count = 0;
        do {
            digits[count++] = static_cast<uint8_t>('0' + value % 10);
            value /= 10;
        } while (value > 0);
        while (count > 0) {
            body.push_back(digits[--count]);
        }
    };
    append_decimal(x);
    body.push_back(0x3B);  // ';'
    append_decimal(y);
    body.push_back(0x20);  // space terminator required by the CSI parser
    body.push_back(0x61);  // ACPS
}

enum class BodySet : uint8_t {
    kKanji,
    kAlphanumeric,
    kHiragana,
    kKatakana,
    kDRCS
};

BodySet MapTextSet(TextSet set) {
    switch (set) {
        case TextSet::kKanji:
        case TextSet::kAdditionalSymbols:
            return BodySet::kKanji;
        case TextSet::kAlphanumeric:
            return BodySet::kAlphanumeric;
        case TextSet::kHiragana:
            return BodySet::kHiragana;
        case TextSet::kKatakana:
            return BodySet::kKatakana;
    }
    return BodySet::kKanji;
}

void EmitCharsetSelection(std::vector<uint8_t>& body, BodySet set) {
    switch (set) {
        case BodySet::kKanji:
            body.push_back(0x1B);
            body.push_back(0x24);
            body.push_back(0x42);  // ESC $ B
            break;
        case BodySet::kAlphanumeric:
            body.push_back(0x1B);
            body.push_back(0x28);
            body.push_back(0x4A);  // ESC ( J
            break;
        case BodySet::kHiragana:
            body.push_back(0x1B);
            body.push_back(0x28);
            body.push_back(0x30);  // ESC ( 0
            break;
        case BodySet::kKatakana:
            body.push_back(0x1B);
            body.push_back(0x28);
            body.push_back(0x31);  // ESC ( 1
            break;
        case BodySet::kDRCS:
            body.push_back(0x1B);
            body.push_back(0x28);
            body.push_back(0x20);
            body.push_back(kDRCS1_F);  // ESC ( 0x20 0x41 (DRCS-1)
            break;
    }
}

bool BuildBody(const Work& work, const std::vector<CharPlan>& plans, std::vector<uint8_t>& body) {
    body.clear();
    body.push_back(0x0C);  // CS: clear screen before every caption

    int current_color = 7;         // decoder default after CS is white
    BodySet current_set = BodySet::kKanji;  // G0 default is Kanji set

    for (const CharPlan& plan : plans) {
        if (plan.color != current_color) {
            body.push_back(static_cast<uint8_t>(0x80 + plan.color));
            current_color = plan.color;
        }

        BodySet set = plan.is_drcs ? BodySet::kDRCS : MapTextSet(plan.text.set);
        if (set != current_set) {
            EmitCharsetSelection(body, set);
            current_set = set;
        }

        // ACPS takes dot coordinates; the decoder derives char.y = y - section height.
        AppendACS(body, plan.x, plan.y + kSectionHeight);

        if (plan.is_drcs) {
            int slot = work.glyphs[plan.glyph_index].slot;
            if (slot < 0 || slot >= kDRCS1SlotCount) {
                return false;
            }
            body.push_back(static_cast<uint8_t>(0x21 + slot));
        } else if (plan.text.two_bytes) {
            body.push_back(plan.text.first);
            body.push_back(plan.text.second);
        } else {
            body.push_back(plan.text.first);
        }
    }

    return true;
}

void AppendDataUnit(std::vector<uint8_t>& units, uint8_t parameter, const std::vector<uint8_t>& payload) {
    units.push_back(0x1F);
    units.push_back(parameter);
    AppendU24BE(units, payload.size());
    units.insert(units.end(), payload.begin(), payload.end());
}

bool BuildDRCSDefinitions(const Work& work, std::vector<uint8_t>& unit_payload) {
    if (work.new_slots.empty()) {
        return true;
    }
    if (work.new_slots.size() > 0xFF) {
        return false;
    }

    unit_payload.clear();
    unit_payload.push_back(static_cast<uint8_t>(work.new_slots.size()));  // number_of_code

    for (int slot : work.new_slots) {
        int glyph_index = work.slot_glyph[static_cast<size_t>(slot)];
        if (glyph_index < 0) {
            return false;
        }
        const Glyph& glyph = work.glyphs[static_cast<size_t>(glyph_index)];

        uint16_t character_code = static_cast<uint16_t>(kDRCS1_F << 8) |
                                  static_cast<uint16_t>(0x21 + slot);
        unit_payload.push_back(static_cast<uint8_t>((character_code >> 8) & 0xFF));
        unit_payload.push_back(static_cast<uint8_t>(character_code & 0xFF));
        unit_payload.push_back(0x01);  // number_of_font

        unit_payload.push_back(0x00);  // font_id=0, mode=0 (pixel)
        unit_payload.push_back(static_cast<uint8_t>(glyph.depth - 2));
        unit_payload.push_back(static_cast<uint8_t>(glyph.width));
        unit_payload.push_back(static_cast<uint8_t>(glyph.height));
        unit_payload.insert(unit_payload.end(), glyph.pixels.begin(), glyph.pixels.end());
    }

    if (unit_payload.size() > kMaxDataUnitPayload) {
        return false;
    }
    return true;
}

}  // namespace

class Encoder::Impl {
public:
    Impl() { Reset(); }

    bool Encode(const Caption& caption, std::vector<EncodedPES>& out);
    void Reset();

private:
    bool ValidateAndPlan(const Caption& caption, Work& work, std::vector<CharPlan>& plans) const;
    bool AssignSlots(Work& work, const std::vector<CharPlan>& plans) const;
    void TouchSlot(Work& work, int slot) const;
    void Commit(const Work& work);

private:
    std::vector<Glyph> glyphs_;
    std::unordered_map<std::string, int> glyph_index_;
    std::array<int, kDRCS1SlotCount> slot_glyph_{};
    std::vector<int> lru_;
    bool header_sent_ = false;
};

void Encoder::Impl::Reset() {
    glyphs_.clear();
    glyph_index_.clear();
    slot_glyph_.fill(-1);
    lru_.clear();
    lru_.reserve(kDRCS1SlotCount);
    for (int slot = 0; slot < kDRCS1SlotCount; slot++) {
        lru_.push_back(slot);
    }
    header_sent_ = false;
}

void Encoder::Impl::TouchSlot(Work& work, int slot) const {
    auto iter = std::find(work.lru.begin(), work.lru.end(), slot);
    if (iter != work.lru.end()) {
        work.lru.erase(iter);
    }
    work.lru.push_back(slot);
}

bool Encoder::Impl::ValidateAndPlan(const Caption& caption, Work& work,
                                    std::vector<CharPlan>& plans) const {
    if (caption.plane_width != kPlaneWidth || caption.plane_height != kPlaneHeight) {
        return false;
    }
    if (caption.has_builtin_sound) {
        return false;
    }

    work.glyphs = glyphs_;
    for (Glyph& glyph : work.glyphs) {
        glyph.fresh = false;
    }
    work.glyph_index = glyph_index_;
    work.slot_glyph = slot_glyph_;
    work.lru = lru_;
    work.new_slots.clear();

    plans.clear();

    for (const CaptionRegion& region : caption.regions) {
        if (region.is_ruby) {
            return false;
        }

        for (const CaptionChar& caption_char : region.chars) {
            if (caption_char.x < 0 || caption_char.y < 0) {
                return false;
            }
            if (caption_char.x + kSectionWidth > kPlaneWidth ||
                caption_char.y + kSectionHeight > kPlaneHeight) {
                return false;
            }
            if (caption_char.char_width != kCharWidth ||
                caption_char.char_height != kCharHeight ||
                caption_char.char_horizontal_spacing != kHorizontalSpacing ||
                caption_char.char_vertical_spacing != kVerticalSpacing) {
                return false;
            }
            if (caption_char.char_horizontal_scale != 1.0f ||
                caption_char.char_vertical_scale != 1.0f) {
                return false;
            }
            if (caption_char.style != kCharStyleDefault ||
                caption_char.enclosure_style != kEnclosureStyleNone) {
                return false;
            }
            if (caption_char.back_color.a != 0) {
                return false;
            }
            int color = FindPalette0TextColor(caption_char.text_color);
            if (color < 0) {
                return false;
            }

            CharPlan plan;
            plan.color = color;
            plan.x = caption_char.x;
            plan.y = caption_char.y;

            if (caption_char.type == CaptionCharType::kText) {
                auto iter = GetTextMap().find(caption_char.codepoint);
                if (iter == GetTextMap().end()) {
                    return false;
                }
                plan.is_drcs = false;
                plan.text = iter->second;
            } else if (caption_char.type == CaptionCharType::kDRCS ||
                       caption_char.type == CaptionCharType::kDRCSReplaced) {
                auto iter = caption.drcs_map.find(caption_char.drcs_code);
                if (iter == caption.drcs_map.end()) {
                    return false;
                }
                Glyph glyph;
                if (!MakeGlyph(iter->second, glyph)) {
                    return false;
                }
                auto glyph_iter = work.glyph_index.find(glyph.key);
                int glyph_index = -1;
                if (glyph_iter == work.glyph_index.end()) {
                    glyph.slot = -1;
                    glyph.fresh = true;
                    glyph_index = static_cast<int>(work.glyphs.size());
                    work.glyphs.push_back(std::move(glyph));
                    work.glyph_index.emplace(work.glyphs.back().key, glyph_index);
                } else {
                    glyph_index = glyph_iter->second;
                }
                plan.is_drcs = true;
                plan.glyph_index = glyph_index;
            } else {
                return false;
            }

            plans.push_back(plan);
        }
    }

    return true;
}

bool Encoder::Impl::AssignSlots(Work& work, const std::vector<CharPlan>& plans) const {
    // Unique glyphs required by this caption, preserving first-reference order.
    std::vector<int> needed;
    std::vector<bool> seen(work.glyphs.size(), false);
    for (const CharPlan& plan : plans) {
        if (!plan.is_drcs) {
            continue;
        }
        size_t index = static_cast<size_t>(plan.glyph_index);
        if (index >= seen.size()) {
            return false;
        }
        if (!seen[index]) {
            seen[index] = true;
            needed.push_back(plan.glyph_index);
        }
    }

    if (needed.size() > static_cast<size_t>(kDRCS1SlotCount)) {
        return false;
    }

    // Protect every cached glyph referenced by this caption from eviction.
    for (int glyph_index : needed) {
        int slot = work.glyphs[static_cast<size_t>(glyph_index)].slot;
        if (slot >= 0) {
            TouchSlot(work, slot);
        }
    }

    // Assign slots to new glyphs. Free slots are at the front of the LRU list;
    // otherwise the least recently used unreferenced slot is evicted.
    for (int glyph_index : needed) {
        Glyph& glyph = work.glyphs[static_cast<size_t>(glyph_index)];
        if (glyph.slot >= 0) {
            continue;
        }
        if (work.lru.empty()) {
            return false;
        }
        int slot = work.lru.front();
        work.lru.erase(work.lru.begin());

        int evicted = work.slot_glyph[static_cast<size_t>(slot)];
        if (evicted >= 0) {
            work.glyph_index.erase(work.glyphs[static_cast<size_t>(evicted)].key);
            work.glyphs[static_cast<size_t>(evicted)].slot = -1;
        }

        work.slot_glyph[static_cast<size_t>(slot)] = glyph_index;
        glyph.slot = slot;
        work.lru.push_back(slot);
        work.new_slots.push_back(slot);
    }

    return true;
}

void Encoder::Impl::Commit(const Work& work) {
    std::vector<Glyph> compact;
    std::unordered_map<std::string, int> compact_index;
    std::array<int, kDRCS1SlotCount> compact_slot_glyph{};
    compact_slot_glyph.fill(-1);

    for (int slot = 0; slot < kDRCS1SlotCount; slot++) {
        int glyph_index = work.slot_glyph[static_cast<size_t>(slot)];
        if (glyph_index < 0) {
            continue;
        }
        Glyph glyph = work.glyphs[static_cast<size_t>(glyph_index)];
        glyph.slot = slot;
        glyph.fresh = false;
        int new_index = static_cast<int>(compact.size());
        compact_index.emplace(glyph.key, new_index);
        compact_slot_glyph[static_cast<size_t>(slot)] = new_index;
        compact.push_back(std::move(glyph));
    }

    glyphs_ = std::move(compact);
    glyph_index_ = std::move(compact_index);
    slot_glyph_ = compact_slot_glyph;
    lru_ = work.lru;
}

bool Encoder::Impl::Encode(const Caption& caption, std::vector<EncodedPES>& out) {
    Work work;
    std::vector<CharPlan> plans;

    if (!ValidateAndPlan(caption, work, plans)) {
        return false;
    }
    if (!AssignSlots(work, plans)) {
        return false;
    }

    std::vector<uint8_t> body;
    if (!BuildBody(work, plans, body)) {
        return false;
    }

    std::vector<uint8_t> units;

    if (!work.new_slots.empty()) {
        std::vector<uint8_t> drcs_payload;
        if (!BuildDRCSDefinitions(work, drcs_payload)) {
            return false;
        }
        AppendDataUnit(units, kDataUnitParameterDRCS, drcs_payload);
    }
    if (body.size() > kMaxDataUnitPayload) {
        return false;
    }
    AppendDataUnit(units, kDataUnitParameterStatement, body);

    if (units.size() > kMaxDataUnitPayload) {
        return false;
    }

    std::vector<uint8_t> statement_payload;
    statement_payload.reserve(4 + units.size());
    statement_payload.push_back(0x00);  // TMD = 0b00, no OTM
    AppendU24BE(statement_payload, units.size());
    statement_payload.insert(statement_payload.end(), units.begin(), units.end());

    std::vector<uint8_t> statement_group;
    if (!MakeDataGroup(kDataGroupIdStatementFirst, statement_payload, statement_group)) {
        return false;
    }

    std::vector<EncodedPES> result;
    if (!header_sent_) {
        std::vector<uint8_t> management_group;
        if (!MakeDataGroup(kDataGroupIdManagement, MakeManagementPayload(), management_group)) {
            return false;
        }
        EncodedPES management_packet;
        management_packet.pts = caption.pts;
        management_packet.data = std::move(management_group);
        result.push_back(std::move(management_packet));
    }

    EncodedPES statement_packet;
    statement_packet.pts = caption.pts;
    statement_packet.data = std::move(statement_group);
    result.push_back(std::move(statement_packet));

    // Commit only after every packet has been built successfully.
    Commit(work);
    header_sent_ = true;

    out = std::move(result);
    return true;
}

Encoder::Encoder() : impl_(std::make_unique<Impl>()) {}

Encoder::~Encoder() = default;

Encoder::Encoder(Encoder&&) noexcept = default;

Encoder& Encoder::operator=(Encoder&&) noexcept = default;

bool Encoder::Encode(const Caption& caption, std::vector<EncodedPES>& out) {
    return impl_->Encode(caption, out);
}

void Encoder::Reset() {
    impl_->Reset();
}

}  // namespace aribcaption
