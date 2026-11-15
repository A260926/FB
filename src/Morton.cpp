// Morton.cpp
// Implementation of Morton / Z-order code interleaving and de-interleaving.
// See Morton.h for the public interface and rationale. The magic-number masks
// and shift chains are taken from Fabian "ryg" Giesen's "Decoding Morton
// codes" article and its addendum.

#include "Morton.h"

#include <cstdint>

// Spread the 16 low bits of x into the even bit positions. Each masking step
// halves the block size while doubling the gap, so after four steps every
// original bit sits two positions apart from its neighbour.
uint32_t part1By1(uint32_t x) {
	x &= 0x0000ffff;                 // keep only the low 16 bits
	x = (x ^ (x << 8)) & 0x00ff00ff; // spread bytes:   0000abcd -> 00ab00cd
	x = (x ^ (x << 4)) & 0x0f0f0f0f; // spread nibbles
	x = (x ^ (x << 2)) & 0x33333333; // spread pairs
	x = (x ^ (x << 1)) & 0x55555555; // spread single bits (one gap each)
	return x;
}

// Inverse of part1By1: gather the even-position bits back into the low 16.
// This runs the part1By1 shift chain in reverse, right-shifting and merging.
uint32_t compact1By1(uint32_t x) {
	x &= 0x55555555; // keep only the even-position bits
	x = (x ^ (x >> 1)) & 0x33333333;
	x = (x ^ (x >> 2)) & 0x0f0f0f0f;
	x = (x ^ (x >> 4)) & 0x00ff00ff;
	x = (x ^ (x >> 8)) & 0x0000ffff;
	return x;
}

// Spread the 10 low bits of x with 2-bit gaps (bit i lands at bit 3i). The
// mask chain follows ryg's article; 0x09249249 is the ...001001001 pattern
// that isolates every third bit.
uint32_t part1By2(uint32_t x) {
	x &= 0x000003ff; // keep only the low 10 bits
	x = (x ^ (x << 16)) & 0xff0000ff;
	x = (x ^ (x << 8)) & 0x0300f00f;
	x = (x ^ (x << 4)) & 0x030c30c3;
	x = (x ^ (x << 2)) & 0x09249249;
	return x;
}

// Inverse of part1By2: gather every third bit back into the low 10 bits.
uint32_t compact1By2(uint32_t x) {
	x &= 0x09249249; // keep only the bits at positions 3i
	x = (x ^ (x >> 2)) & 0x030c30c3;
	x = (x ^ (x >> 4)) & 0x0300f00f;
	x = (x ^ (x >> 8)) & 0xff0000ff;
	x = (x ^ (x >> 16)) & 0x000003ff;
	return x;
}

// Interleave x into the even bits and y into the odd bits.
uint32_t encodeMorton2(uint16_t x, uint16_t y) {
	return (part1By1(y) << 1) | part1By1(x);
}

// Pull the even bits into x and the odd bits into y.
void decodeMorton2(uint32_t code, uint32_t &x, uint32_t &y) {
	x = compact1By1(code);
	y = compact1By1(code >> 1);
}

// Interleave x, y and z with 2-bit gaps: x at 3i, y at 3i+1, z at 3i+2.
uint32_t encodeMorton3(uint16_t x, uint16_t y, uint16_t z) {
	return (part1By2(z) << 2) | (part1By2(y) << 1) | part1By2(x);
}

// Pull each of the three interleaved bit planes back out.
void decodeMorton3(uint32_t code, uint32_t &x, uint32_t &y, uint32_t &z) {
	x = compact1By2(code);
	y = compact1By2(code >> 1);
	z = compact1By2(code >> 2);
}

// Spread the 21 low bits of a coordinate into every third bit of a 64-bit
// word (bit i lands at bit 3i). This is the 64-bit analogue of part1By2, with
// one extra masking step to cover the wider 63-bit output range.
static uint64_t splitBy3_64(uint32_t a) {
	uint64_t x = static_cast<uint64_t>(a) & 0x1fffff; // keep only the low 21 bits
	x = (x | (x << 32)) & 0x1f00000000ffffULL;
	x = (x | (x << 16)) & 0x1f0000ff0000ffULL;
	x = (x | (x << 8)) & 0x100f00f00f00f00fULL;
	x = (x | (x << 4)) & 0x10c30c30c30c30c3ULL;
	x = (x | (x << 2)) & 0x1249249249249249ULL;
	return x;
}

// Encode a 3D point into a 63-bit Morton code by interleaving 21 bits from
// each of x, y and z.
uint64_t morton3D_64(uint32_t x, uint32_t y, uint32_t z) {
	return splitBy3_64(x) | (splitBy3_64(y) << 1) | (splitBy3_64(z) << 2);
}
