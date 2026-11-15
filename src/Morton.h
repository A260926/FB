#pragma once

// Morton.h
// Morton codes (also called Z-order or Z-curve codes) interleave the bits of
// two or three integer coordinates into a single scalar key. Points that are
// close together in space tend to map to nearby codes, so sorting primitives
// by Morton code lays them out along a space-filling curve. This locality is
// what makes Morton codes the backbone of linear BVH (LBVH) construction,
// spatial hashing, cache-friendly grid traversal and texture swizzling.
//
// The bit-spreading routines follow Fabian "ryg" Giesen's "Decoding Morton
// codes" article and its addendum: the classic magic-number masks that spread
// a set of low bits apart with fixed gaps, plus their inverse "compact"
// operations that gather the spread bits back together.
//
// Dependency free: only needs <cstdint>.

#include <cstdint>

// "Insert" one zero bit after each of the 16 low bits of x, spreading them
// into the even bit positions (bit i goes to bit 2i). Bits 16..31 of the
// input are ignored. This is the 2D interleave building block.
uint32_t part1By1(uint32_t x);

// Inverse of part1By1: gather the even-position bits of x back down into the
// low 16 bits (bit 2i goes to bit i), discarding the odd positions.
uint32_t compact1By1(uint32_t x);

// "Insert" two zero bits after each of the 10 low bits of x, spreading them
// with 2-bit gaps (bit i goes to bit 3i). Bits 10..31 of the input are
// ignored. This is the 3D interleave building block.
uint32_t part1By2(uint32_t x);

// Inverse of part1By2: gather every third bit of x back down into the low 10
// bits (bit 3i goes to bit i).
uint32_t compact1By2(uint32_t x);

// Encode a 2D point into a 32-bit Morton code by interleaving the bits of x
// (even positions) and y (odd positions). Each coordinate contributes 16 bits.
uint32_t encodeMorton2(uint16_t x, uint16_t y);

// Decode a 2D Morton code back into its x and y coordinates.
void decodeMorton2(uint32_t code, uint32_t &x, uint32_t &y);

// Encode a 3D point into a 32-bit Morton code by interleaving the bits of x,
// y and z with 2-bit gaps. Each coordinate contributes 10 bits (30 bits used).
uint32_t encodeMorton3(uint16_t x, uint16_t y, uint16_t z);

// Decode a 3D Morton code back into its x, y and z coordinates.
void decodeMorton3(uint32_t code, uint32_t &x, uint32_t &y, uint32_t &z);

// Encode a 3D point into a 63-bit Morton code stored in a uint64_t. Each of
// x, y and z contributes 21 bits, giving a much larger usable coordinate range
// than encodeMorton3. This is the form typically used when building an LBVH
// over scenes that need finer than 10-bit quantisation per axis.
uint64_t morton3D_64(uint32_t x, uint32_t y, uint32_t z);
