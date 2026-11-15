// BCn.h - BC1 (DXT1) and BC3 (DXT5) block texture decoders.
// Decodes S3TC / DXT compressed textures into 0xAARRGGBB pixels.
// Reference: Fabian Giesen's (ryg's) "Texture formats" / GPU BCn decoding notes.
#ifndef BCN_H
#define BCN_H

#include <cstdint>
#include <vector>

// Decode a single 4x4 BC1/DXT1 block (8 bytes) into 16 pixels (0xAARRGGBB).
void decodeBC1Block(const uint8_t block[8], uint32_t outRGBA[16]);

// Decode a single 4x4 BC3/DXT5 block (16 bytes) into 16 pixels (0xAARRGGBB).
void decodeBC3Block(const uint8_t block[16], uint32_t outRGBA[16]);

// Decode a full BC1 image. width and height must be multiples of 4.
std::vector<uint32_t> decodeBC1(const uint8_t *data, int width, int height);

// Decode a full BC3 image. width and height must be multiples of 4.
std::vector<uint32_t> decodeBC3(const uint8_t *data, int width, int height);

#endif
