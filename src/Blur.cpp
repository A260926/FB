#include "Blur.h"

#include <algorithm>
#include <cmath>

void boxBlurH(std::vector<float> &img, int w, int h, int radius) {
	if (radius < 1 || w <= 0 || h <= 0)
		return;
	float norm = 1.0f / static_cast<float>(2 * radius + 1);
	std::vector<float> row(w);
	for (int y = 0; y < h; y++) {
		int base = y * w;
		// Prime the running sum over [-r, r] with clamped edge sampling.
		float sum = 0.0f;
		for (int i = -radius; i <= radius; i++) {
			int x = std::clamp(i, 0, w - 1);
			sum += img[base + x];
		}
		for (int x = 0; x < w; x++) {
			row[x] = sum * norm;
			// Slide the window: add the pixel entering on the right, drop the
			// one leaving on the left, both clamped at the edges.
			int xin = std::clamp(x + radius + 1, 0, w - 1);
			int xout = std::clamp(x - radius, 0, w - 1);
			sum += img[base + xin] - img[base + xout];
		}
		for (int x = 0; x < w; x++)
			img[base + x] = row[x];
	}
}

void boxBlurV(std::vector<float> &img, int w, int h, int radius) {
	if (radius < 1 || w <= 0 || h <= 0)
		return;
	float norm = 1.0f / static_cast<float>(2 * radius + 1);
	std::vector<float> col(h);
	for (int x = 0; x < w; x++) {
		// Prime the running sum over [-r, r] with clamped edge sampling.
		float sum = 0.0f;
		for (int i = -radius; i <= radius; i++) {
			int y = std::clamp(i, 0, h - 1);
			sum += img[y * w + x];
		}
		for (int y = 0; y < h; y++) {
			col[y] = sum * norm;
			// Slide the window: add the pixel entering below, drop the one
			// leaving above, both clamped at the edges.
			int yin = std::clamp(y + radius + 1, 0, h - 1);
			int yout = std::clamp(y - radius, 0, h - 1);
			sum += img[yin * w + x] - img[yout * w + x];
		}
		for (int y = 0; y < h; y++)
			img[y * w + x] = col[y];
	}
}

void gaussianBlur(std::vector<float> &img, int w, int h, float radius, int iterations) {
	int r = static_cast<int>(std::lround(radius));
	if (r < 1 || iterations < 1)
		return;
	for (int i = 0; i < iterations; i++) {
		boxBlurH(img, w, h, r);
		boxBlurV(img, w, h, r);
	}
}
