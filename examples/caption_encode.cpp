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

// Example: encode a couple of captions into a simple container file, then
// decode that file in a separate process.
//
// Container record format (all integers big-endian):
//   int64_t  pts
//   uint32_t length
//   uint8_t  data[length]
//
// Usage:
//   caption_encode <output.pes>          write mode (removes old output first)
//   caption_encode --decode <input.pes>  decode mode (never re-encodes)

#include <aribcaption/context.hpp>
#include <aribcaption/decoder.hpp>
#include <aribcaption/encoder.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace aribcaption;

namespace {

constexpr int kSectionWidth = 40;
constexpr int kSectionHeight = 60;

ColorRGBA PaletteColor(int index) {
    static const ColorRGBA kPalette[8] = {
        ColorRGBA(0, 0, 0, 255),
        ColorRGBA(255, 0, 0, 255),
        ColorRGBA(0, 255, 0, 255),
        ColorRGBA(255, 255, 0, 255),
        ColorRGBA(0, 0, 255, 255),
        ColorRGBA(255, 0, 255, 255),
        ColorRGBA(0, 255, 255, 255),
        ColorRGBA(255, 255, 255, 255),
    };
    return kPalette[index];
}

// A simple 8x8, 4-bit glyph with a recognizable diagonal pattern.
DRCS MakeSampleGlyph() {
    DRCS drcs;
    drcs.width = 8;
    drcs.height = 8;
    drcs.depth = 4;
    drcs.depth_bits = 2;
    // depth_bits = 2, so four pixels are packed into each byte, MSB first.
    drcs.pixels.assign(8 * 8 * drcs.depth_bits / 8, 0);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            uint8_t value = (x == y) ? 0x3 : static_cast<uint8_t>((x + y) & 0x3);
            int index = y * 8 + x;
            int shift = 6 - (index % 4) * 2;
            drcs.pixels[static_cast<size_t>(index / 4)] |= static_cast<uint8_t>(value << shift);
        }
    }
    drcs.md5 = "example-glyph";
    return drcs;
}

void AddTextChar(CaptionRegion& region, uint32_t codepoint, int x, int y, int color) {
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
    ch.text_color = PaletteColor(color);
    ch.back_color = ColorRGBA(0, 0, 0, 0);
    region.chars.push_back(ch);
}

void AddDrcsChar(CaptionRegion& region, uint32_t drcs_code, int x, int y, int color) {
    CaptionChar ch;
    ch.type = CaptionCharType::kDRCS;
    ch.drcs_code = drcs_code;
    ch.x = x;
    ch.y = y;
    ch.char_width = 36;
    ch.char_height = 36;
    ch.char_horizontal_spacing = 4;
    ch.char_vertical_spacing = 24;
    ch.char_horizontal_scale = 1.0f;
    ch.char_vertical_scale = 1.0f;
    ch.text_color = PaletteColor(color);
    ch.back_color = ColorRGBA(0, 0, 0, 0);
    region.chars.push_back(ch);
}

Caption MakeCaptionOne(const DRCS& glyph) {
    Caption caption;
    caption.pts = 1000;
    caption.plane_width = 960;
    caption.plane_height = 540;
    caption.iso6392_language_code = ThreeCC("jpn");
    caption.drcs_map[0x2100] = glyph;

    CaptionRegion region;
    region.x = 0;
    region.y = 0;
    AddTextChar(region, 0xFF21, 0, 0, 7);       // Ａ
    AddTextChar(region, 0x65E5, 40, 0, 1);      // 日 (red)
    AddTextChar(region, 0x672C, 80, 0, 7);      // 本
    AddDrcsChar(region, 0x2100, 120, 0, 6);     // DRCS (cyan)
    caption.regions.push_back(region);
    return caption;
}

Caption MakeCaptionTwo(const DRCS& glyph) {
    Caption caption;
    caption.pts = 2000;
    caption.plane_width = 960;
    caption.plane_height = 540;
    caption.iso6392_language_code = ThreeCC("jpn");
    // A different input code and md5 for the same bitmap: the encoder should
    // reuse the cached DRCS-1 slot without redefining it.
    caption.drcs_map[0x4321] = glyph;

    CaptionRegion region;
    region.x = 0;
    region.y = 60;
    AddTextChar(region, 0x3042, 0, 60, 2);      // あ (green)
    AddTextChar(region, 0x30A2, 40, 60, 4);     // ア (blue)
    AddTextChar(region, 0x3000, 80, 60, 7);     // ideographic space
    AddDrcsChar(region, 0x4321, 120, 60, 3);    // same DRCS bitmap (yellow)
    caption.regions.push_back(region);
    return caption;
}

bool WriteAll(std::ofstream& file, const void* data, size_t size) {
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return file.good();
}

bool WriteBigEndianU64(std::ofstream& file, uint64_t value) {
    uint8_t buffer[8];
    for (int i = 7; i >= 0; i--) {
        buffer[i] = static_cast<uint8_t>(value & 0xFF);
        value >>= 8;
    }
    return WriteAll(file, buffer, sizeof(buffer));
}

bool WriteBigEndianU32(std::ofstream& file, uint32_t value) {
    uint8_t buffer[4];
    for (int i = 3; i >= 0; i--) {
        buffer[i] = static_cast<uint8_t>(value & 0xFF);
        value >>= 8;
    }
    return WriteAll(file, buffer, sizeof(buffer));
}

int WriteMode(const std::string& path) {
    // Remove any previous output before writing.
    std::remove(path.c_str());

    DRCS glyph = MakeSampleGlyph();
    Encoder encoder;

    std::vector<EncodedPES> packets;
    std::vector<EncodedPES> first_packets;
    std::vector<EncodedPES> second_packets;
    if (!encoder.Encode(MakeCaptionOne(glyph), first_packets) ||
        !encoder.Encode(MakeCaptionTwo(glyph), second_packets)) {
        std::cerr << "encode failed\n";
        return 1;
    }
    packets.insert(packets.end(), first_packets.begin(), first_packets.end());
    packets.insert(packets.end(), second_packets.begin(), second_packets.end());

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::cerr << "cannot open output file: " << path << "\n";
        return 1;
    }

    for (const EncodedPES& packet : packets) {
        if (!WriteBigEndianU64(file, static_cast<uint64_t>(packet.pts)) ||
            !WriteBigEndianU32(file, static_cast<uint32_t>(packet.data.size())) ||
            !WriteAll(file, packet.data.data(), packet.data.size())) {
            std::cerr << "write failed\n";
            return 1;
        }
    }
    file.close();
    if (!file) {
        std::cerr << "close failed\n";
        return 1;
    }

    std::cout << "wrote " << packets.size() << " packets to " << path << "\n";
    return 0;
}

bool ReadBigEndianU64(std::ifstream& file, uint64_t& value) {
    uint8_t buffer[8];
    file.read(reinterpret_cast<char*>(buffer), sizeof(buffer));
    if (file.gcount() != static_cast<std::streamsize>(sizeof(buffer))) {
        return false;
    }
    value = 0;
    for (uint8_t byte : buffer) {
        value = (value << 8) | byte;
    }
    return true;
}

bool ReadBigEndianU32(std::ifstream& file, uint32_t& value) {
    uint8_t buffer[4];
    file.read(reinterpret_cast<char*>(buffer), sizeof(buffer));
    if (file.gcount() != static_cast<std::streamsize>(sizeof(buffer))) {
        return false;
    }
    value = 0;
    for (uint8_t byte : buffer) {
        value = (value << 8) | byte;
    }
    return true;
}

int DecodeMode(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "cannot open input file: " << path << "\n";
        return 1;
    }

    Context context;
    Decoder decoder(context);
    decoder.Initialize(EncodingScheme::kARIB_STD_B24_JIS);

    std::vector<Caption> captions;
    while (true) {
        uint64_t pts = 0;
        uint32_t length = 0;
        uint8_t peek = 0;
        file.read(reinterpret_cast<char*>(&peek), 1);
        if (file.gcount() == 0) {
            break;
        }
        file.putback(static_cast<char>(peek));
        if (!ReadBigEndianU64(file, pts) || !ReadBigEndianU32(file, length) || length == 0) {
            std::cerr << "malformed container\n";
            return 1;
        }
        std::vector<uint8_t> data(length);
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(length));
        if (file.gcount() != static_cast<std::streamsize>(length)) {
            std::cerr << "truncated packet\n";
            return 1;
        }

        DecodeResult result;
        DecodeStatus status = decoder.Decode(data.data(), data.size(), static_cast<int64_t>(pts), result);
        if (status == DecodeStatus::kGotCaption && result.caption) {
            captions.push_back(*result.caption);
        }
    }

    if (captions.size() < 2) {
        std::cerr << "expected at least 2 captions, got " << captions.size() << "\n";
        return 1;
    }

    // Verify the normal characters of the first two captions.
    std::vector<const CaptionChar*> first;
    for (const CaptionRegion& region : captions[0].regions) {
        for (const CaptionChar& ch : region.chars) {
            first.push_back(&ch);
        }
    }
    std::vector<const CaptionChar*> second;
    for (const CaptionRegion& region : captions[1].regions) {
        for (const CaptionChar& ch : region.chars) {
            second.push_back(&ch);
        }
    }

    if (first.size() < 4 || second.size() < 4) {
        std::cerr << "unexpected caption sizes\n";
        return 1;
    }
    if (first[0]->codepoint != 0xFF21 || first[1]->codepoint != 0x65E5 || first[2]->codepoint != 0x672C) {
        std::cerr << "first caption text mismatch\n";
        return 1;
    }
    if (second[0]->codepoint != 0x3042 || second[1]->codepoint != 0x30A2 || second[2]->codepoint != 0x3000) {
        std::cerr << "second caption text mismatch\n";
        return 1;
    }

    // Verify the same DRCS bitmap appears in both captions.
    const CaptionChar* first_drcs = first.back();
    const CaptionChar* second_drcs = second.back();
    if (first_drcs->type != CaptionCharType::kDRCS || second_drcs->type != CaptionCharType::kDRCS) {
        std::cerr << "expected DRCS characters\n";
        return 1;
    }
    auto it1 = captions[0].drcs_map.find(first_drcs->drcs_code);
    auto it2 = captions[1].drcs_map.find(second_drcs->drcs_code);
    if (it1 == captions[0].drcs_map.end() || it2 == captions[1].drcs_map.end()) {
        std::cerr << "missing DRCS bitmap\n";
        return 1;
    }
    if (it1->second.width != it2->second.width || it1->second.height != it2->second.height ||
        it1->second.depth != it2->second.depth || it1->second.pixels != it2->second.pixels) {
        std::cerr << "DRCS bitmap mismatch between captions\n";
        return 1;
    }

    std::cout << "decoded " << captions.size() << " captions, DRCS bitmap matches\n";
    return 0;
}

}  // namespace

int main(int argc, const char* argv[]) {
    if (argc == 2 && std::strcmp(argv[1], "--decode") != 0) {
        return WriteMode(argv[1]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--decode") == 0) {
        return DecodeMode(argv[2]);
    }

    std::cerr << "usage:\n"
              << "  " << argv[0] << " <output.pes>\n"
              << "  " << argv[0] << " --decode <input.pes>\n";
    return 2;
}
