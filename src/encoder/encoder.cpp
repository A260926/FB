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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aribcaption/encoder.hpp"
#include "decoder/b24_colors.hpp"
#include "decoder/b24_conv_tables.hpp"
#include "decoder/b24_gaiji_table.hpp"

namespace aribcaption {

namespace {

// Profile A, 960x540 horizontal writing format
constexpr int kPlaneWidth = 960;
constexpr int kPlaneHeight = 540;
constexpr int kCharWidth = 36;
constexpr int kCharHeight = 36;
constexpr int kCharHorizontalSpacing = 4;
constexpr int kCharVerticalSpacing = 24;
constexpr int kSectionWidth = kCharWidth + kCharHorizontalSpacing;    // 40
constexpr int kSectionHeight = kCharHeight + kCharVerticalSpacing;   // 60
constexpr size_t kMaxGroupSize = 65535;
constexpr size_t kDrcsSlotCount = 94;

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout
uint16_t Crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(data[i]) << 8));
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

enum class GraphicKind : uint8_t {
    Kanji = 0,        // ESC 24 42, 2-byte JIS X 0208 kanji plane
    Additional = 1,   // ESC 24 3B, 2-byte ARIB additional symbols
    Alphanumeric = 2,// ESC 28 4A, 1-byte fullwidth alphanumeric
    Hiragana = 3,     // ESC 28 30, 1-byte hiragana
    Katakana = 4,     // ESC 28 31, 1-byte katakana
};

struct JisCode {
    GraphicKind kind = GraphicKind::Kanji;
    uint8_t first = 0;   // first GL byte, 0x21..0x7E
    uint8_t second = 0;  // second GL byte, 0 for 1-byte graphic sets
};

// Builds the Unicode -> JIS reverse lookup by inverting the exact tables the
// decoder uses, so every emitted code is guaranteed to round-trip.
class ReverseJisMap {
public:
    ReverseJisMap() {
        // JIS X 0208 kanji plane: the decoder reaches ku 0..83 through the
        // Kanji graphic set and the 10 additional-symbol ku (84..93) through
        // the Additional symbols graphic set.
        constexpr uint32_t kanji_begin_ku = 0;
        constexpr uint32_t kanji_ku_count = 84;
        constexpr uint32_t additional_ku_count = 10;  // 940 table entries
        for (uint32_t i = 0; i < kanji_ku_count * 94; i++) {
            Insert(kKanjiTable[i], JisCode{GraphicKind::Kanji,
                                           static_cast<uint8_t>(0x21 + kanji_begin_ku + i / 94),
                                           static_cast<uint8_t>(0x21 + i % 94)});
        }
        for (uint32_t i = 0; i < additional_ku_count * 94; i++) {
            Insert(kAdditionalSymbolsTable_Unicode[i],
                   JisCode{GraphicKind::Additional,
                           static_cast<uint8_t>(0x21 + kanji_ku_count + i / 94),
                           static_cast<uint8_t>(0x21 + i % 94)});
        }
        for (uint32_t i = 0; i < 94; i++) {
            const uint8_t byte = static_cast<uint8_t>(0x21 + i);
            Insert(kAlphanumericTable_Fullwidth[i],
                   JisCode{GraphicKind::Alphanumeric, byte, 0});
            Insert(kHiraganaTable[i],
                   JisCode{GraphicKind::Hiragana, byte, 0});
            Insert(kKatakanaTable[i],
                   JisCode{GraphicKind::Katakana, byte, 0});
        }
    }

    [[nodiscard]]
    const JisCode* Lookup(uint32_t codepoint) const {
        auto iter = map_.find(codepoint);
        if (iter == map_.end()) {
            return nullptr;
        }
        return &iter->second;
    }

private:
    void Insert(uint32_t codepoint, const JisCode& code) {
        // Unassigned table slots decode to U+FFFD and must never be emitted;
        // when several codes reach the same codepoint, keep any reversible one.
        if (codepoint == 0xFFFD || codepoint == 0) {
            return;
        }
        map_.emplace(codepoint, code);
    }

    std::unordered_map<uint32_t, JisCode> map_;
};

struct DrcsPattern {
    int width = 0;
    int height = 0;
    int depth = 0;
    std::vector<uint8_t> pixels;

    bool operator==(const DrcsPattern& other) const {
        return width == other.width && height == other.height &&
               depth == other.depth && pixels == other.pixels;
    }
};

struct DrcsPatternHash {
    size_t operator()(const DrcsPattern& p) const {
        size_t h = static_cast<size_t>(p.width) * 1000003u;
        h ^= static_cast<size_t>(p.height) * 9176u;
        h ^= static_cast<size_t>(p.depth) * 131u;
        for (uint8_t byte : p.pixels) {
            h = h * 1315423911u ^ byte;
        }
        return h;
    }
};

struct CacheSlot {
    DrcsPattern pattern;
    bool occupied = false;
    uint64_t last_used = 0;  // monotonic sequence of the last referencing caption
};

void AppendU16BE(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void AppendU24BE(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

// Appends a non-negative integer as ASCII decimal digits (CSI parameter syntax).
void AppendDecimal(std::vector<uint8_t>& out, int value) {
    char digits[16];
    int count = 0;
    int v = value;
    do {
        digits[count++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v > 0);
    while (count > 0) {
        out.push_back(static_cast<uint8_t>(digits[--count]));
    }
}

void AppendEscDesignation(std::vector<uint8_t>& body, GraphicKind kind) {
    body.push_back(0x1B);
    switch (kind) {
        case GraphicKind::Kanji:
            body.push_back(0x24);
            body.push_back(0x42);
            break;
        case GraphicKind::Additional:
            body.push_back(0x24);
            body.push_back(0x3B);
            break;
        case GraphicKind::Alphanumeric:
            body.push_back(0x28);
            body.push_back(0x4A);
            break;
        case GraphicKind::Hiragana:
            body.push_back(0x28);
            body.push_back(0x30);
            break;
        case GraphicKind::Katakana:
            body.push_back(0x28);
            body.push_back(0x31);
            break;
    }
}

// Wraps a data group (group header + body + CRC) into a private caption packet
// beginning with 80 FF F0. The size field counts the body without the CRC-16,
// matching STD-B24 and the decoder's framing. Returns false on > 65535 bodies.
bool BuildPacket(int64_t pts, uint8_t group_id_byte,
                 const std::vector<uint8_t>& group_body, EncodedPES& out) {
    if (group_body.size() > kMaxGroupSize) {
        return false;
    }

    std::vector<uint8_t> packet;
    packet.reserve(3 + 5 + group_body.size() + 2);
    packet.push_back(0x80);
    packet.push_back(0xFF);
    packet.push_back(0xF0);

    // 5-byte data group header: id, two link/reserved bytes, 16-bit group size
    packet.push_back(group_id_byte);
    packet.push_back(0x00);
    packet.push_back(0x00);
    AppendU16BE(packet, static_cast<uint16_t>(group_body.size()));
    packet.insert(packet.end(), group_body.begin(), group_body.end());

    // CRC covers the group header through the body, but not the CRC itself.
    const uint16_t crc = Crc16(packet.data() + 3, 5 + group_body.size());
    AppendU16BE(packet, crc);

    out.pts = pts;
    out.data = std::move(packet);
    return true;
}

}  // namespace

namespace internal {

class EncoderImpl {
public:
    struct PlacedChar {
        bool is_drcs = false;
        JisCode jis;
        int drcs_slot = -1;
        int pattern_index = -1;  // index into distinct_patterns for DRCS chars
        int pos_x = 0;   // active position in dots, equals the char box x
        int pos_y = 0;   // active position in dots, equals the char box y + 60
        uint8_t color_index = 7;
    };

    EncoderImpl() = default;
    ~EncoderImpl() = default;

    bool Encode(const Caption& caption, std::vector<EncodedPES>& out) {
        // Everything is built into locals; the output and the persistent state
        // are touched only after every step has succeeded.
        std::vector<EncodedPES> built;
        std::array<CacheSlot, kDrcsSlotCount> planned = slots_;

        if (caption.has_builtin_sound) {
            return false;
        }

        // ---- First pass: validate every character, collect distinct DRCS ----
        std::vector<PlacedChar> placed;
        std::vector<DrcsPattern> distinct_patterns;
        std::unordered_map<DrcsPattern, int, DrcsPatternHash> pattern_index;

        for (const CaptionRegion& region : caption.regions) {
            if (region.is_ruby) {
                return false;
            }
            for (const CaptionChar& ch : region.chars) {
                if (!ValidateCommonAttributes(ch)) {
                    return false;
                }

                PlacedChar placed_char;
                // Non-negative integer positions; the 40x60 section (36x36 glyph
                // box plus spacing) must lie completely inside the 960x540 plane.
                if (ch.x < 0 || ch.y < 0 ||
                    ch.x + kSectionWidth > kPlaneWidth ||
                    ch.y + kSectionHeight > kPlaneHeight) {
                    return false;
                }
                // ACPS addresses the active position in dots. The decoder derives
                // CaptionChar::y = active_pos_y - section_height, so emit y + 60.
                placed_char.pos_x = ch.x;
                placed_char.pos_y = ch.y + kSectionHeight;

                const ColorRGBA& color = ch.text_color;
                int color_index = -1;
                for (int i = 0; i < 8; i++) {
                    if (color.u32 == kB24ColorCLUT[0][i].u32) {
                        color_index = i;
                        break;
                    }
                }
                if (color_index < 0) {
                    return false;
                }
                placed_char.color_index = static_cast<uint8_t>(color_index);

                if (ch.type == CaptionCharType::kText) {
                    const JisCode* code = reverse_map_.Lookup(ch.codepoint);
                    if (code == nullptr) {
                        return false;  // not reversibly representable in JIS
                    }
                    placed_char.is_drcs = false;
                    placed_char.jis = *code;
                } else if (ch.type == CaptionCharType::kDRCS ||
                           ch.type == CaptionCharType::kDRCSReplaced) {
                    auto drcs_iter = caption.drcs_map.find(ch.drcs_code);
                    if (drcs_iter == caption.drcs_map.end()) {
                        return false;
                    }
                    const DRCS& drcs = drcs_iter->second;
                    if (!ValidateDrcs(drcs)) {
                        return false;
                    }

                    DrcsPattern pattern;
                    pattern.width = drcs.width;
                    pattern.height = drcs.height;
                    pattern.depth = drcs.depth;
                    pattern.pixels = drcs.pixels;
                    // Only effective pixels participate in identity: clear the
                    // unused low bits of the final packed byte.
                    const size_t total_bits =
                        static_cast<size_t>(drcs.width) * drcs.height *
                        drcs.depth_bits;
                    const size_t unused = (8 - total_bits % 8) % 8;
                    if (unused != 0 && !pattern.pixels.empty()) {
                        pattern.pixels.back() &=
                            static_cast<uint8_t>(0xFF << unused);
                    }

                    auto found = pattern_index.find(pattern);
                    if (found != pattern_index.end()) {
                        placed_char.pattern_index = found->second;
                    } else {
                        placed_char.pattern_index =
                            static_cast<int>(distinct_patterns.size());
                        pattern_index.emplace(pattern, placed_char.pattern_index);
                        distinct_patterns.push_back(std::move(pattern));
                    }
                    placed_char.is_drcs = true;
                } else {
                    return false;
                }

                placed.push_back(placed_char);
            }
        }

        // ---- Second pass: assign DRCS slots ----
        std::unordered_map<int, int> slot_by_pattern;  // pattern index -> slot
        std::vector<int> defined_slots;                // slots redefined here
        std::vector<bool> protected_slot(kDrcsSlotCount, false);

        // Every resident slot holding a pattern this caption needs is protected,
        // so a glyph referenced later can never be evicted.
        for (const DrcsPattern& pattern : distinct_patterns) {
            int resident = FindResidentSlot(pattern, planned);
            if (resident >= 0) {
                protected_slot[resident] = true;
            }
        }

        for (int p = 0; p < static_cast<int>(distinct_patterns.size()); p++) {
            const DrcsPattern& pattern = distinct_patterns[p];
            int resident = FindResidentSlot(pattern, planned);
            if (resident >= 0) {
                // Cache hit: already defined, no definition unit is needed.
                slot_by_pattern.emplace(p, resident);
                continue;
            }
            int slot = AcquireSlot(pattern, planned, defined_slots,
                                   protected_slot);
            if (slot < 0) {
                return false;  // more than 94 distinct patterns
            }
            protected_slot[slot] = true;  // now referenced by this caption
            slot_by_pattern.emplace(p, slot);
        }

        for (PlacedChar& placed_char : placed) {
            if (placed_char.is_drcs) {
                placed_char.drcs_slot =
                    slot_by_pattern.at(placed_char.pattern_index);
            }
        }

        // ---- Second pass: emit packets ----
        if (!session_started_) {
            EncodedPES management;
            if (!BuildManagementPacket(caption.pts, management)) {
                return false;
            }
            built.push_back(std::move(management));
        }
        EncodedPES statement;
        if (!BuildStatementPacket(caption.pts, placed, planned,
                                  defined_slots, statement)) {
            return false;
        }
        built.push_back(std::move(statement));

        // ---- Commit ----
        const uint64_t sequence = ++sequence_;
        for (const auto& item : slot_by_pattern) {
            planned[item.second].last_used = sequence;
        }
        slots_ = std::move(planned);
        session_started_ = true;
        out = std::move(built);
        return true;
    }

    void Reset() {
        for (CacheSlot& slot : slots_) {
            slot = CacheSlot{};
        }
        session_started_ = false;
        sequence_ = 0;
    }

private:
    static bool ValidateCommonAttributes(const CaptionChar& ch) {
        if (ch.char_width != kCharWidth || ch.char_height != kCharHeight) {
            return false;
        }
        if (ch.char_horizontal_spacing != kCharHorizontalSpacing ||
            ch.char_vertical_spacing != kCharVerticalSpacing) {
            return false;
        }
        if (ch.char_horizontal_scale != 1.0f ||
            ch.char_vertical_scale != 1.0f) {
            return false;
        }
        if (ch.style != CharStyle::kCharStyleDefault) {
            return false;
        }
        if (ch.enclosure_style != EnclosureStyle::kEnclosureStyleNone) {
            return false;
        }
        // Transparent background: palette 0 index 8 is (0, 0, 0, alpha 0).
        const ColorRGBA transparent(0, 0, 0, 0);
        if (ch.back_color.u32 != transparent.u32) {
            return false;
        }
        return true;
    }

    static bool ValidateDrcs(const DRCS& drcs) {
        if (drcs.width < 1 || drcs.width > 64 ||
            drcs.height < 1 || drcs.height > 64) {
            return false;
        }
        if (drcs.depth != 2 && drcs.depth != 4) {
            return false;
        }
        const int expected_depth_bits = (drcs.depth == 2) ? 1 : 2;
        if (drcs.depth_bits != expected_depth_bits) {
            return false;
        }
        const size_t expected_size =
            (static_cast<size_t>(drcs.width) * drcs.height *
             expected_depth_bits + 7) / 8;
        return drcs.pixels.size() == expected_size;
    }

    // Finds a slot already holding the pattern inside the planned cache.
    static int FindResidentSlot(
            const DrcsPattern& pattern,
            const std::array<CacheSlot, kDrcsSlotCount>& planned) {
        for (size_t i = 0; i < kDrcsSlotCount; i++) {
            if (planned[i].occupied && planned[i].pattern == pattern) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // Reserves a slot for a brand-new pattern. Returns -1 if every slot is
    // occupied by a pattern the current caption still needs (more than 94
    // distinct patterns).
    static int AcquireSlot(const DrcsPattern& pattern,
                           std::array<CacheSlot, kDrcsSlotCount>& planned,
                           std::vector<int>& defined_slots,
                           const std::vector<bool>& protected_slot) {
        // Prefer an unused slot, lowest index first for stable output.
        for (size_t i = 0; i < kDrcsSlotCount; i++) {
            if (!planned[i].occupied) {
                planned[i].pattern = pattern;
                planned[i].occupied = true;
                defined_slots.push_back(static_cast<int>(i));
                return static_cast<int>(i);
            }
        }

        // Evict the least recently used unprotected slot. Every slot holding a
        // pattern referenced anywhere in this caption is protected, so a glyph
        // needed later can never be displaced.
        int victim = -1;
        uint64_t victim_last_used = 0;
        for (size_t i = 0; i < kDrcsSlotCount; i++) {
            if (protected_slot[i]) {
                continue;
            }
            if (victim < 0 || planned[i].last_used < victim_last_used ||
                (planned[i].last_used == victim_last_used &&
                 static_cast<int>(i) < victim)) {
                victim = static_cast<int>(i);
                victim_last_used = planned[i].last_used;
            }
        }
        if (victim < 0) {
            return -1;
        }

        planned[victim] = CacheSlot{};
        planned[victim].pattern = pattern;
        planned[victim].occupied = true;
        defined_slots.push_back(victim);
        return victim;
    }

    static bool BuildManagementPacket(int64_t pts, EncodedPES& out) {
        std::vector<uint8_t> body;
        body.reserve(10);
        body.push_back(0x00);  // TMD = 0, no OTM
        body.push_back(0x01);  // number of languages
        body.push_back(0x00);  // language tag 0 (first language), DMF = 0
        // ISO 639-2 "jpn"
        body.push_back(0x6A);
        body.push_back(0x70);
        body.push_back(0x6E);
        body.push_back(0x80);  // format = 8 (SWF 7, 960x540 horizontal), TCS = 0 (JIS)
        AppendU24BE(body, 0);  // data unit loop length = 0
        return BuildPacket(pts, 0x00, body, out);
    }

    bool BuildStatementPacket(
            int64_t pts,
            const std::vector<PlacedChar>& placed,
            const std::array<CacheSlot, kDrcsSlotCount>& planned,
            const std::vector<int>& defined_slots,
            EncodedPES& out) const {
        std::vector<uint8_t> units;

        // ---- DRCS definition unit (0x30), emitted before any reference ----
        if (!defined_slots.empty()) {
            std::vector<int> slots = defined_slots;
            std::sort(slots.begin(), slots.end());

            std::vector<uint8_t> drcs_body;
            drcs_body.push_back(static_cast<uint8_t>(slots.size()));  // number_of_code

            for (int slot : slots) {
                const DrcsPattern& pattern = planned[slot].pattern;
                // DRCS-1: high byte 0x41 selects F=0x41, low byte is the
                // 1-byte character code 0x21..0x7E.
                AppendU16BE(drcs_body, static_cast<uint16_t>(0x4100 + 0x21 + slot));
                drcs_body.push_back(0x01);  // number_of_font = 1
                drcs_body.push_back(0x00);  // font_id = 0, mode = 0 (bitmap)
                drcs_body.push_back(static_cast<uint8_t>(pattern.depth - 2));
                drcs_body.push_back(static_cast<uint8_t>(pattern.width));
                drcs_body.push_back(static_cast<uint8_t>(pattern.height));
                drcs_body.insert(drcs_body.end(),
                                 pattern.pixels.begin(), pattern.pixels.end());
            }

            units.push_back(0x1F);
            units.push_back(0x30);
            AppendU24BE(units, static_cast<uint32_t>(drcs_body.size()));
            units.insert(units.end(), drcs_body.begin(), drcs_body.end());
        }

        // ---- Statement body unit (0x20) ----
        std::vector<uint8_t> statement;
        statement.push_back(0x0C);  // CS: every statement starts with clear screen
        // CS resets the decoder's graphic sets, so each statement body begins
        // with Kanji (G0) and no DRCS designation, regardless of history.
        GraphicKind current_graphic = GraphicKind::Kanji;
        bool drcs_designated = false;
        int current_color = 7;                             // decoder default foreground

        for (const PlacedChar& ch : placed) {
            if (ch.color_index != current_color) {
                statement.push_back(static_cast<uint8_t>(0x80 + ch.color_index));
                current_color = ch.color_index;
            }

            // CSI ACPS: set the active position in dots, supporting arbitrary
            // integer coordinates instead of only the 40x60 character grid.
            // Syntax: CSI P1 ';' P2 SP 'a'
            statement.push_back(0x9B);
            AppendDecimal(statement, ch.pos_x);
            statement.push_back(0x3B);
            AppendDecimal(statement, ch.pos_y);
            statement.push_back(0x20);
            statement.push_back(0x61);  // final byte: ACPS ('a')

            if (ch.is_drcs) {
                if (!drcs_designated) {
                    statement.push_back(0x1B);
                    statement.push_back(0x28);
                    statement.push_back(0x20);
                    statement.push_back(0x41);  // designate DRCS-1 into G0
                    drcs_designated = true;
                }
                statement.push_back(static_cast<uint8_t>(0x21 + ch.drcs_slot));
            } else {
                if (drcs_designated || current_graphic != ch.jis.kind) {
                    AppendEscDesignation(statement, ch.jis.kind);
                    current_graphic = ch.jis.kind;
                    drcs_designated = false;
                }
                statement.push_back(ch.jis.first);
                if (ch.jis.second != 0) {
                    statement.push_back(ch.jis.second);
                }
            }
        }

        units.push_back(0x1F);
        units.push_back(0x20);
        AppendU24BE(units, static_cast<uint32_t>(statement.size()));
        units.insert(units.end(), statement.begin(), statement.end());

        std::vector<uint8_t> group_body;
        group_body.push_back(0x00);  // TMD = 0, no OTM (group id 0x04 marks statement)
        AppendU24BE(group_body, static_cast<uint32_t>(units.size()));
        group_body.insert(group_body.end(), units.begin(), units.end());
        // Group id 0x04: caption statement, language id 1, group bit 0
        return BuildPacket(pts, 0x04, group_body, out);
    }

private:
    ReverseJisMap reverse_map_;
    std::array<CacheSlot, kDrcsSlotCount> slots_;
    uint64_t sequence_ = 0;
    bool session_started_ = false;
};

}  // namespace internal

Encoder::Encoder() : pimpl_(std::make_unique<internal::EncoderImpl>()) {}

Encoder::~Encoder() = default;

Encoder::Encoder(Encoder&&) noexcept = default;

Encoder& Encoder::operator=(Encoder&&) noexcept = default;

bool Encoder::Encode(const Caption& caption, std::vector<EncodedPES>& out) {
    return pimpl_->Encode(caption, out);
}

void Encoder::Reset() {
    pimpl_->Reset();
}

}  // namespace aribcaption
