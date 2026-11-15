#pragma once

#include <cstdint>
#include <vector>

// Lanczos (windowed-sinc) image resampling for 0xAARRGGBB pixel buffers, as
// used by TextureMap and Canvas. Lanczos gives sharper results than a box or
// bilinear filter at the cost of ringing on high-contrast edges, so output is
// clamped back to [0,255]. See the ryg "sinc" and "half-pel" blog posts.

// Windowed sinc kernel L(x) = sinc(x) * sinc(x / a) for |x| < a, else 0.
// sinc(t) = sin(pi t) / (pi t) with the t == 0 -> 1 limit. The lobe count a is
// typically 2 or 3.
float lanczosKernel(float x, int a);

// Separable arbitrary-ratio resample of a 0xAARRGGBB image from sw*sh to dw*dh.
// A horizontal pass is followed by a vertical pass; each output sample gathers
// 2a input taps per axis, weighted by the normalized Lanczos kernel with edge
// clamping. Work is done in float and clamped to [0,255] on write.
std::vector<uint32_t> resampleLanczos(const std::vector<uint32_t> &src, int sw, int sh, int dw, int dh, int a = 3);

// Convenience 2x downsample (dw = sw/2, dh = sh/2) for sharper mip generation
// than a box filter.
std::vector<uint32_t> downsampleHalfLanczos(const std::vector<uint32_t> &src, int sw, int sh, int a = 2);
