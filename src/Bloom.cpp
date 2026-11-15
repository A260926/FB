#include "Bloom.h"

#include "Blur.h"

#include <algorithm>
#include <vector>

void applyBloom(Canvas &canvas, float threshold, float radius, float intensity) {
	int w = static_cast<int>(canvas.width);
	int h = static_cast<int>(canvas.height);
	if (w <= 0 || h <= 0)
		return;
	size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);

	// (1) Bright-pass: extract the highlight energy into planar float channels
	// on a 0..1 scale. A soft knee near the threshold fades highlights in
	// gently instead of clipping to a hard on/off mask.
	std::vector<float> br(n), bg(n), bb(n);
	const float knee = 0.1f;
	for (size_t i = 0; i < n; i++) {
		uint32_t p = canvas.pixels[i];
		float r = ((p >> 16) & 0xFF) / 255.0f;
		float g = ((p >> 8) & 0xFF) / 255.0f;
		float b = (p & 0xFF) / 255.0f;
		float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
		// Soft knee: 0 below (threshold - knee), ramping to 1 above threshold.
		float t = std::clamp((lum - (threshold - knee)) / (2.0f * knee), 0.0f, 1.0f);
		float weight = t * t * (3.0f - 2.0f * t);
		br[i] = r * weight;
		bg[i] = g * weight;
		bb[i] = b * weight;
	}

	// (2) Blur each highlight channel to spread its energy outwards.
	gaussianBlur(br, w, h, radius);
	gaussianBlur(bg, w, h, radius);
	gaussianBlur(bb, w, h, radius);

	// (3) Additively composite the scaled, blurred highlights back onto the
	// canvas, clamping each channel to 255.
	for (size_t i = 0; i < n; i++) {
		uint32_t p = canvas.pixels[i];
		int r = (p >> 16) & 0xFF;
		int g = (p >> 8) & 0xFF;
		int b = p & 0xFF;
		r = std::min(255, r + static_cast<int>(intensity * br[i] * 255.0f + 0.5f));
		g = std::min(255, g + static_cast<int>(intensity * bg[i] * 255.0f + 0.5f));
		b = std::min(255, b + static_cast<int>(intensity * bb[i] * 255.0f + 0.5f));
		canvas.pixels[i] = (p & 0xFF000000u) | (r << 16) | (g << 8) | b;
	}
}
