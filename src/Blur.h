#pragma once

#include <vector>

// Separable box and Gaussian blur on planar single-channel float buffers,
// following Fabian "ryg" Giesen's "Fast Blurs 1 & 2". Each buffer is w*h
// floats in row-major order; blurs run in place. A true Gaussian is
// approximated by repeated box passes (central limit theorem): three passes of
// a box already give a good bell shape.

// Horizontal box blur with a (2r+1)-wide window. Uses a running sum so each
// output pixel costs O(1) regardless of radius: prime the sum over [-r,r] with
// clamped edge sampling, then slide adding the incoming and dropping the
// outgoing sample. Divides by the window width to preserve total energy.
void boxBlurH(std::vector<float> &img, int w, int h, int radius);

// Vertical counterpart to boxBlurH, sliding the window down each column.
void boxBlurV(std::vector<float> &img, int w, int h, int radius);

// Approximate a Gaussian of the given radius by running several separable box
// passes (horizontal then vertical) over the buffer. The radius is rounded to
// the nearest whole pixel; the default of three passes converges to a smooth
// bell by the central limit theorem.
void gaussianBlur(std::vector<float> &img, int w, int h, float radius, int iterations = 3);
