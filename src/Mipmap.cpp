#include "Mipmap.h"

#include <algorithm>
#include <cmath>

static glm::vec3 unpack(uint32_t p) {
	return glm::vec3((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
}

MipTexture::MipTexture(const TextureMap &tex) {
	int w = static_cast<int>(tex.width), h = static_cast<int>(tex.height);
	levels.push_back(tex.pixels);
	widths.push_back(w);
	heights.push_back(h);
	// Box-filter down to 1x1.
	while (w > 1 || h > 1) {
		int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
		std::vector<uint32_t> next(static_cast<size_t>(nw) * nh);
		const std::vector<uint32_t> &prev = levels.back();
		int pw = w;
		for (int y = 0; y < nh; y++) {
			for (int x = 0; x < nw; x++) {
				int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
				int y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
				glm::vec3 c = (unpack(prev[y0 * pw + x0]) + unpack(prev[y0 * pw + x1]) + unpack(prev[y1 * pw + x0]) +
				               unpack(prev[y1 * pw + x1])) *
				              0.25f;
				int r = static_cast<int>(c.r), g = static_cast<int>(c.g), b = static_cast<int>(c.b);
				next[static_cast<size_t>(y) * nw + x] = (255u << 24) | (r << 16) | (g << 8) | b;
			}
		}
		levels.push_back(std::move(next));
		widths.push_back(nw);
		heights.push_back(nh);
		w = nw;
		h = nh;
	}
}

glm::vec3 MipTexture::sampleBilinear(int level, float u, float v) const {
	level = std::min(std::max(level, 0), levelCount() - 1);
	int w = widths[level], h = heights[level];
	const std::vector<uint32_t> &px = levels[level];
	float fx = std::min(std::max(u, 0.0f), 1.0f) * (w - 1);
	float fy = (1.0f - std::min(std::max(v, 0.0f), 1.0f)) * (h - 1);
	int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
	int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
	float tx = fx - x0, ty = fy - y0;
	glm::vec3 c00 = unpack(px[static_cast<size_t>(y0) * w + x0]);
	glm::vec3 c10 = unpack(px[static_cast<size_t>(y0) * w + x1]);
	glm::vec3 c01 = unpack(px[static_cast<size_t>(y1) * w + x0]);
	glm::vec3 c11 = unpack(px[static_cast<size_t>(y1) * w + x1]);
	return glm::mix(glm::mix(c00, c10, tx), glm::mix(c01, c11, tx), ty);
}

glm::vec3 MipTexture::sampleTrilinear(float u, float v, float lod) const {
	lod = std::min(std::max(lod, 0.0f), static_cast<float>(levelCount() - 1));
	int lo = static_cast<int>(lod);
	int hi = std::min(lo + 1, levelCount() - 1);
	float t = lod - lo;
	return glm::mix(sampleBilinear(lo, u, v), sampleBilinear(hi, u, v), t);
}
