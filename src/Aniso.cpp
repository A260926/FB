#include "Aniso.h"

#include <algorithm>
#include <cmath>

static glm::vec4 unpack(uint32_t p) {
	return glm::vec4((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF, (p >> 24) & 0xFF);
}

static uint32_t pack(glm::vec4 c) {
	int r = static_cast<int>(std::min(std::max(c.r, 0.0f), 255.0f));
	int g = static_cast<int>(std::min(std::max(c.g, 0.0f), 255.0f));
	int b = static_cast<int>(std::min(std::max(c.b, 0.0f), 255.0f));
	int a = static_cast<int>(std::min(std::max(c.a, 0.0f), 255.0f));
	return (static_cast<uint32_t>(a) << 24) | (r << 16) | (g << 8) | b;
}

// Bilinear lookup within a single TextureMap level. (u, v) in [0,1] are clamped
// to the level's edge; returns RGBA in 0..255 floats.
static glm::vec4 sampleBilinear(const TextureMap &tex, float u, float v) {
	int w = static_cast<int>(tex.width), h = static_cast<int>(tex.height);
	if (w <= 0 || h <= 0)
		return glm::vec4(0.0f);
	float fx = std::min(std::max(u, 0.0f), 1.0f) * (w - 1);
	float fy = (1.0f - std::min(std::max(v, 0.0f), 1.0f)) * (h - 1);
	int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
	int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
	float tx = fx - x0, ty = fy - y0;
	glm::vec4 c00 = unpack(tex.pixels[static_cast<size_t>(y0) * w + x0]);
	glm::vec4 c10 = unpack(tex.pixels[static_cast<size_t>(y0) * w + x1]);
	glm::vec4 c01 = unpack(tex.pixels[static_cast<size_t>(y1) * w + x0]);
	glm::vec4 c11 = unpack(tex.pixels[static_cast<size_t>(y1) * w + x1]);
	return glm::mix(glm::mix(c00, c10, tx), glm::mix(c01, c11, tx), ty);
}

std::vector<TextureMap> buildMipChain(const TextureMap &base) {
	std::vector<TextureMap> mips;
	mips.push_back(base);
	int w = static_cast<int>(base.width), h = static_cast<int>(base.height);
	// Box-filter down to 1x1.
	while (w > 1 || h > 1) {
		int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
		const TextureMap &prev = mips.back();
		TextureMap next;
		next.width = static_cast<size_t>(nw);
		next.height = static_cast<size_t>(nh);
		next.pixels.resize(static_cast<size_t>(nw) * nh);
		for (int y = 0; y < nh; y++) {
			for (int x = 0; x < nw; x++) {
				int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
				int y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
				glm::vec4 c = (unpack(prev.pixels[static_cast<size_t>(y0) * w + x0]) +
				               unpack(prev.pixels[static_cast<size_t>(y0) * w + x1]) +
				               unpack(prev.pixels[static_cast<size_t>(y1) * w + x0]) +
				               unpack(prev.pixels[static_cast<size_t>(y1) * w + x1])) *
				              0.25f;
				next.pixels[static_cast<size_t>(y) * nw + x] = pack(c);
			}
		}
		mips.push_back(std::move(next));
		w = nw;
		h = nh;
	}
	return mips;
}

glm::vec4 sampleTrilinear(const std::vector<TextureMap> &mips, float u, float v, float lod) {
	if (mips.empty())
		return glm::vec4(0.0f);
	int maxLevel = static_cast<int>(mips.size()) - 1;
	lod = std::min(std::max(lod, 0.0f), static_cast<float>(maxLevel));
	int lo = static_cast<int>(lod);
	int hi = std::min(lo + 1, maxLevel);
	float t = lod - lo;
	return glm::mix(sampleBilinear(mips[lo], u, v), sampleBilinear(mips[hi], u, v), t);
}

glm::vec4 sampleAnisotropic(const std::vector<TextureMap> &mips, float u, float v, glm::vec2 dudx, glm::vec2 dudy,
                            int maxAniso) {
	if (mips.empty())
		return glm::vec4(0.0f);
	// Convert the [0,1] uv gradients to texel-space gradients so that axis
	// lengths and LODs are measured in texels of the base level.
	float w = static_cast<float>(mips[0].width), h = static_cast<float>(mips[0].height);
	glm::vec2 gx(dudx.x * w, dudx.y * h);
	glm::vec2 gy(dudy.x * w, dudy.y * h);
	float lenx = glm::length(gx), leny = glm::length(gy);

	// The longer screen-space gradient is the major axis; the shorter is the
	// minor axis. Step samples along the major axis in uv units.
	glm::vec2 majorUV;
	float majorLen, minorLen;
	if (lenx >= leny) {
		majorUV = dudx;
		majorLen = lenx;
		minorLen = leny;
	} else {
		majorUV = dudy;
		majorLen = leny;
		minorLen = lenx;
	}

	const float epsilon = 1e-6f;
	minorLen = std::max(minorLen, epsilon);
	majorLen = std::max(majorLen, epsilon);

	// Number of taps is the anisotropy ratio, clamped to [1, maxAniso].
	int n = static_cast<int>(std::ceil(majorLen / minorLen));
	n = std::min(std::max(n, 1), std::max(maxAniso, 1));

	// LOD comes from the minor axis so detail is preserved along it while the
	// spread of taps along the major axis suppresses aliasing there.
	float lod = std::log2(minorLen);

	glm::vec4 sum(0.0f);
	for (int i = 0; i < n; i++) {
		// Offsets spread evenly across the major axis footprint, centred on
		// (u, v): t in (-0.5, 0.5).
		float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(n) - 0.5f;
		float su = u + t * majorUV.x;
		float sv = v + t * majorUV.y;
		sum += sampleTrilinear(mips, su, sv, lod);
	}
	return sum / static_cast<float>(n);
}
