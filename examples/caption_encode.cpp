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

// Usage:
//   caption_encode <path>             Encode two captions (reusing one DRCS
//                                     bitmap) and write their packets to <path>.
//                                     Any previous file at <path> is removed.
//   caption_encode --decode <path>    In a separate process, read <path> and
//                                     verify the stream with a Decoder. The
//                                     Encoder is never used in this mode.
//
// On-disk record format (all integers big-endian):
//   int64_t  pts  (8 bytes, signed)
//   uint32_t size (4 bytes)
//   uint8_t  packet[size]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "aribcaption/context.hpp"
#include "aribcaption/decoder.hpp"

#if !defined(ARIBCC_DECODE_ONLY)
#include "aribcaption/encoder.hpp"
#endif

using namespace aribcaption;

namespace {

constexpr uint32_t kSharedDrcsCodeA = 100;
constexpr uint32_t kSharedDrcsCodeB = 200;  // different code, identical bitmap

DRCS MakeSharedBitmap() {
    // 5x5, 2-tone (1 bit/pixel), high bit first: 25 bits -> 4 bytes.
    DRCS drcs;
    drcs.width = 5;
    drcs.height = 5;
    drcs.depth = 2;
    drcs.depth_bits = 1;
    drcs.pixels = {0x55, 0xAA, 0x55, 0xA0};  // last 7 bits are ignored
    return drcs;
}

// Effective-pixel view used for comparison: the unused low bits of the final
// packed byte carry no information and are ignored on the wire.
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

CaptionChar MakeDrcs(uint32_t code, int x, int y, ColorRGBA color) {
    CaptionChar ch;
    ch.type = CaptionCharType::kDRCS;
    ch.drcs_code = code;
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

void WriteU64BE(std::ofstream& file, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        file.put(static_cast<char>((value >> shift) & 0xFF));
    }
}

void WriteU32BE(std::ofstream& file, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        file.put(static_cast<char>((value >> shift) & 0xFF));
    }
}

bool ReadExact(std::ifstream& file, void* buffer, size_t length) {
    file.read(static_cast<char*>(buffer), static_cast<std::streamsize>(length));
    return static_cast<size_t>(file.gcount()) == length;
}

uint64_t ReadU64BE(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value = (value << 8) | p[i];
    }
    return value;
}

uint32_t ReadU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

#if !defined(ARIBCC_DECODE_ONLY)

int RunWrite(const std::string& path) {
    std::remove(path.c_str());  // every run starts from an empty output

    Encoder encoder;

    // First caption: kanji, an ideographic space, a color and a DRCS bitmap.
    Caption first;
    first.pts = 1000;
    first.plane_width = 960;
    first.plane_height = 540;
    first.iso6392_language_code = ThreeCC("jpn");
    first.drcs_map[kSharedDrcsCodeA] = MakeSharedBitmap();
    CaptionRegion first_region;
    first_region.chars.push_back(MakeText(0x65E5, 0, 0, ColorRGBA(255, 255, 255, 255)));
    first_region.chars.push_back(MakeText(0x672C, 40, 0, ColorRGBA(255, 0, 0, 255)));
    first_region.chars.push_back(MakeText(0x3000, 80, 0, ColorRGBA(255, 255, 0, 255)));
    first_region.chars.push_back(MakeDrcs(kSharedDrcsCodeA, 120, 60,
                                          ColorRGBA(0, 255, 255, 255)));
    first.regions.push_back(first_region);

    // Second caption: fullwidth letters, hiragana, and the *same* DRCS bitmap
    // transmitted under a different drcs_code, exercising the encoder cache.
    Caption second;
    second.pts = 2000;
    second.plane_width = 960;
    second.plane_height = 540;
    second.iso6392_language_code = ThreeCC("jpn");
    second.drcs_map[kSharedDrcsCodeB] = MakeSharedBitmap();
    CaptionRegion second_region;
    second_region.chars.push_back(MakeText(0xFF21, 0, 0, ColorRGBA(0, 255, 0, 255)));
    second_region.chars.push_back(MakeText(0xFF22, 40, 0, ColorRGBA(255, 255, 0, 255)));
    second_region.chars.push_back(MakeText(0x3042, 80, 0, ColorRGBA(0, 0, 255, 255)));
    second_region.chars.push_back(MakeDrcs(kSharedDrcsCodeB, 120, 60,
                                           ColorRGBA(255, 0, 255, 255)));
    second.regions.push_back(second_region);

    const Caption* captions[] = {&first, &second};
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::cerr << "cannot open output: " << path << "\n";
        return 1;
    }

    size_t packet_count = 0;
    for (const Caption* caption : captions) {
        std::vector<EncodedPES> packets;
        if (!encoder.Encode(*caption, packets)) {
            std::cerr << "encoding failed\n";
            return 1;
        }
        for (const EncodedPES& pes : packets) {
            WriteU64BE(file, static_cast<uint64_t>(pes.pts));
            WriteU32BE(file, static_cast<uint32_t>(pes.data.size()));
            file.write(reinterpret_cast<const char*>(pes.data.data()),
                       static_cast<std::streamsize>(pes.data.size()));
            packet_count++;
        }
    }
    if (!file) {
        std::cerr << "writing failed\n";
        return 1;
    }
    std::cout << "wrote " << packet_count << " packets to " << path << "\n";
    return 0;
}

#endif  // !ARIBCC_DECODE_ONLY

std::vector<CaptionChar> FlattenChars(const Caption& cap) {
    std::vector<CaptionChar> chars;
    for (const CaptionRegion& region : cap.regions) {
        for (const CaptionChar& ch : region.chars) {
            chars.push_back(ch);
        }
    }
    return chars;
}

// Returns the pixels of the caption's single DRCS bitmap, or nullptr.
const DRCS* FindSingleDrcs(const Caption& cap) {
    const DRCS* found = nullptr;
    for (const auto& entry : cap.drcs_map) {
        if (found) {
            return nullptr;  // expected exactly one distinct bitmap
        }
        found = &entry.second;
    }
    return found;
}

int RunDecode(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "cannot open input: " << path << "\n";
        return 1;
    }

    Context context;
    Decoder decoder(context);
    if (!decoder.Initialize(EncodingScheme::kARIB_STD_B24_JIS)) {
        std::cerr << "decoder init failed\n";
        return 1;
    }

    std::vector<std::unique_ptr<Caption>> captions;
    while (true) {
        uint8_t header[12];
        if (!ReadExact(file, header, sizeof(header))) {
            break;  // clean EOF
        }
        const int64_t pts = static_cast<int64_t>(ReadU64BE(header));
        const uint32_t size = ReadU32BE(header + 8);
        if (size == 0 || size > 1024 * 1024) {
            std::cerr << "implausible packet size: " << size << "\n";
            return 1;
        }
        std::vector<uint8_t> packet(size);
        if (!ReadExact(file, packet.data(), size)) {
            std::cerr << "truncated packet\n";
            return 1;
        }
        DecodeResult result;
        DecodeStatus status = decoder.Decode(packet.data(), packet.size(), pts, result);
        if (status == DecodeStatus::kError) {
            std::cerr << "decoder error\n";
            return 1;
        }
        if (status == DecodeStatus::kGotCaption && result.caption) {
            captions.push_back(std::move(result.caption));
        }
    }

    if (captions.size() != 2) {
        std::cerr << "expected 2 captions, got " << captions.size() << "\n";
        return 1;
    }

    const DRCS expected = MakeSharedBitmap();

    // First caption: 日 本 (space) plus the shared DRCS.
    {
        const Caption& cap = *captions[0];
        if (cap.pts != 1000 || cap.plane_width != 960 || cap.plane_height != 540) {
            std::cerr << "first caption metadata mismatch\n";
            return 1;
        }
        const auto chars = FlattenChars(cap);
        const uint32_t expected_cp[] = {0x65E5, 0x672C, 0x3000};
        const int expected_x[] = {0, 40, 80};
        if (chars.size() != 4) {
            std::cerr << "first caption char count mismatch\n";
            return 1;
        }
        for (size_t i = 0; i < 3; i++) {
            if (chars[i].type != CaptionCharType::kText ||
                chars[i].codepoint != expected_cp[i] ||
                chars[i].x != expected_x[i] || chars[i].y != 0) {
                std::cerr << "first caption text char " << i << " mismatch\n";
                return 1;
            }
        }
        if (chars[3].type != CaptionCharType::kDRCS || chars[3].x != 120 ||
            chars[3].y != 60) {
            std::cerr << "first caption DRCS placement mismatch\n";
            return 1;
        }
        const DRCS* drcs = FindSingleDrcs(cap);
        if (!drcs || drcs->width != expected.width ||
            drcs->height != expected.height || drcs->depth != expected.depth ||
            drcs->depth_bits != expected.depth_bits ||
            EffectivePixels(*drcs) != EffectivePixels(expected)) {
            std::cerr << "first caption DRCS bitmap mismatch\n";
            return 1;
        }
    }

    // Second caption: Ａ Ｂ あ plus the same reused DRCS bitmap.
    {
        const Caption& cap = *captions[1];
        if (cap.pts != 2000) {
            std::cerr << "second caption pts mismatch\n";
            return 1;
        }
        const auto chars = FlattenChars(cap);
        const uint32_t expected_cp[] = {0xFF21, 0xFF22, 0x3042};
        const int expected_x[] = {0, 40, 80};
        if (chars.size() != 4) {
            std::cerr << "second caption char count mismatch\n";
            return 1;
        }
        for (size_t i = 0; i < 3; i++) {
            if (chars[i].type != CaptionCharType::kText ||
                chars[i].codepoint != expected_cp[i] ||
                chars[i].x != expected_x[i] || chars[i].y != 0) {
                std::cerr << "second caption text char " << i << " mismatch\n";
                return 1;
            }
        }
        if (chars[3].type != CaptionCharType::kDRCS || chars[3].x != 120 ||
            chars[3].y != 60) {
            std::cerr << "second caption DRCS placement mismatch\n";
            return 1;
        }
        const DRCS* drcs = FindSingleDrcs(cap);
        if (!drcs || EffectivePixels(*drcs) != EffectivePixels(expected)) {
            std::cerr << "second caption DRCS bitmap mismatch\n";
            return 1;
        }
    }

    std::cout << "cross-process roundtrip verified: 2 captions, shared DRCS bitmap\n";
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 3 && std::strcmp(argv[1], "--decode") == 0) {
        return RunDecode(argv[2]);
    }
#if !defined(ARIBCC_DECODE_ONLY)
    if (argc == 2) {
        return RunWrite(argv[1]);
    }
#endif
    std::cerr << "usage: " << argv[0] << " <output-path>\n"
              << "       " << argv[0] << " --decode <input-path>\n";
    return 1;
}
