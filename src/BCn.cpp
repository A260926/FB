// BCn.cpp - BC1 (DXT1) and BC3 (DXT5) block texture decoders.
#include "BCn.h"

#include <cstdint>
#include <vector>

namespace {

// Read a little-endian 16-bit value from two bytes.
uint16_t readU16(const uint8_t *p) {
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// Expand a 5-bit channel to 8 bits by replicating the high bits.
uint8_t expand5(uint32_t v) {
	return static_cast<uint8_t>((v << 3) | (v >> 2));
}

// Expand a 6-bit channel to 8 bits by replicating the high bits.
uint8_t expand6(uint32_t v) {
	return static_cast<uint8_t>((v << 2) | (v >> 4));
}

// Pack channels into a 0xAARRGGBB pixel.
uint32_t packRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
	       static_cast<uint32_t>(b);
}

// Decode an RGB565 endpoint into 8-bit R, G, B.
void decode565(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b) {
	r = expand5((c >> 11) & 0x1F);
	g = expand6((c >> 5) & 0x3F);
	b = expand5(c & 0x1F);
}

// Build a BC1 4-colour palette from an 8-byte block.
// When forceOpaque is true (used by BC3 colour blocks), always take the
// c0>c1 opaque interpolation path regardless of the endpoint ordering.
// Fills palette[4] with 0xAARRGGBB values.
void buildBC1Palette(const uint8_t block[8], bool forceOpaque, uint32_t palette[4]) {
	uint16_t c0 = readU16(block);
	uint16_t c1 = readU16(block + 2);

	uint8_t r0, g0, b0, r1, g1, b1;
	decode565(c0, r0, g0, b0);
	decode565(c1, r1, g1, b1);

	palette[0] = packRGBA(r0, g0, b0, 255);
	palette[1] = packRGBA(r1, g1, b1, 255);

	if (forceOpaque || c0 > c1) {
		// Two interpolated colours, all opaque.
		uint8_t r2 = static_cast<uint8_t>((2 * r0 + r1) / 3);
		uint8_t g2 = static_cast<uint8_t>((2 * g0 + g1) / 3);
		uint8_t b2 = static_cast<uint8_t>((2 * b0 + b1) / 3);
		uint8_t r3 = static_cast<uint8_t>((r0 + 2 * r1) / 3);
		uint8_t g3 = static_cast<uint8_t>((g0 + 2 * g1) / 3);
		uint8_t b3 = static_cast<uint8_t>((b0 + 2 * b1) / 3);
		palette[2] = packRGBA(r2, g2, b2, 255);
		palette[3] = packRGBA(r3, g3, b3, 255);
	} else {
		// One interpolated colour plus a transparent black.
		uint8_t r2 = static_cast<uint8_t>((r0 + r1) / 2);
		uint8_t g2 = static_cast<uint8_t>((g0 + g1) / 2);
		uint8_t b2 = static_cast<uint8_t>((b0 + b1) / 2);
		palette[2] = packRGBA(r2, g2, b2, 255);
		palette[3] = packRGBA(0, 0, 0, 0);
	}
}

// Decode the 32-bit index block (4 bytes at offset) into per-pixel palette
// indices, writing the chosen palette colour into out[16].
void applyColourIndices(const uint8_t indexBytes[4], const uint32_t palette[4], uint32_t out[16]) {
	uint32_t bits = static_cast<uint32_t>(indexBytes[0]) | (static_cast<uint32_t>(indexBytes[1]) << 8) |
	                (static_cast<uint32_t>(indexBytes[2]) << 16) | (static_cast<uint32_t>(indexBytes[3]) << 24);
	for (int i = 0; i < 16; i++) {
		uint32_t idx = (bits >> (i * 2)) & 0x3;
		out[i] = palette[idx];
	}
}

} // namespace

void decodeBC1Block(const uint8_t block[8], uint32_t outRGBA[16]) {
	uint32_t palette[4];
	buildBC1Palette(block, false, palette);
	applyColourIndices(block + 4, palette, outRGBA);
}

void decodeBC3Block(const uint8_t block[16], uint32_t outRGBA[16]) {
	// Alpha portion: two 8-bit endpoints plus 16 x 3-bit indices.
	uint8_t a0 = block[0];
	uint8_t a1 = block[1];

	uint8_t alpha[8];
	alpha[0] = a0;
	alpha[1] = a1;
	if (a0 > a1) {
		// Six interpolated alpha values.
		for (int i = 1; i <= 6; i++) {
			alpha[i + 1] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
		}
	} else {
		// Four interpolated alpha values, then explicit 0 and 255.
		for (int i = 1; i <= 4; i++) {
			alpha[i + 1] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
		}
		alpha[6] = 0;
		alpha[7] = 255;
	}

	// The 16 x 3-bit alpha indices are packed in the 6 bytes after a0,a1,
	// as two little-endian 24-bit groups of 8 indices each.
	uint8_t perPixelAlpha[16];
	for (int half = 0; half < 2; half++) {
		const uint8_t *p = block + 2 + half * 3;
		uint32_t bits =
		    static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
		for (int i = 0; i < 8; i++) {
			uint32_t idx = (bits >> (i * 3)) & 0x7;
			perPixelAlpha[half * 8 + i] = alpha[idx];
		}
	}

	// Colour portion: BC1-style block, always the opaque 4-colour path.
	uint32_t palette[4];
	buildBC1Palette(block + 8, true, palette);

	uint32_t colours[16];
	applyColourIndices(block + 12, palette, colours);

	// Combine colour RGB with the decoded per-pixel alpha.
	for (int i = 0; i < 16; i++) {
		uint32_t c = colours[i];
		uint8_t r = static_cast<uint8_t>((c >> 16) & 0xFF);
		uint8_t g = static_cast<uint8_t>((c >> 8) & 0xFF);
		uint8_t b = static_cast<uint8_t>(c & 0xFF);
		outRGBA[i] = packRGBA(r, g, b, perPixelAlpha[i]);
	}
}

std::vector<uint32_t> decodeBC1(const uint8_t *data, int width, int height) {
	std::vector<uint32_t> image(static_cast<size_t>(width) * height, 0);
	int blocksX = width / 4;
	int blocksY = height / 4;
	const uint8_t *ptr = data;
	for (int by = 0; by < blocksY; by++) {
		for (int bx = 0; bx < blocksX; bx++) {
			uint32_t pixels[16];
			decodeBC1Block(ptr, pixels);
			ptr += 8;
			for (int py = 0; py < 4; py++) {
				for (int px = 0; px < 4; px++) {
					int x = bx * 4 + px;
					int y = by * 4 + py;
					image[static_cast<size_t>(y) * width + x] = pixels[py * 4 + px];
				}
			}
		}
	}
	return image;
}

std::vector<uint32_t> decodeBC3(const uint8_t *data, int width, int height) {
	std::vector<uint32_t> image(static_cast<size_t>(width) * height, 0);
	int blocksX = width / 4;
	int blocksY = height / 4;
	const uint8_t *ptr = data;
	for (int by = 0; by < blocksY; by++) {
		for (int bx = 0; bx < blocksX; bx++) {
			uint32_t pixels[16];
			decodeBC3Block(ptr, pixels);
			ptr += 16;
			for (int py = 0; py < 4; py++) {
				for (int px = 0; px < 4; px++) {
					int x = bx * 4 + px;
					int y = by * 4 + py;
					image[static_cast<size_t>(y) * width + x] = pixels[py * 4 + px];
				}
			}
		}
	}
	return image;
}
