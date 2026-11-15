#include "Resample.h"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Normalized sinc: sin(pi t) / (pi t), with the removable singularity at t == 0
// resolved to 1.
float sinc(float t) {
	if (t == 0.0f) {
		return 1.0f;
	}
	float pt = kPi * t;
	return std::sin(pt) / pt;
}

glm::vec4 unpack(uint32_t p) {
	return glm::vec4((p >> 24) & 0xFF, (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
}

uint32_t pack(const glm::vec4 &c) {
	// Lanczos overshoots on high-contrast edges, so clamp before rounding.
	int a = static_cast<int>(std::lround(glm::clamp(c.x, 0.0f, 255.0f)));
	int r = static_cast<int>(std::lround(glm::clamp(c.y, 0.0f, 255.0f)));
	int g = static_cast<int>(std::lround(glm::clamp(c.z, 0.0f, 255.0f)));
	int b = static_cast<int>(std::lround(glm::clamp(c.w, 0.0f, 255.0f)));
	return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
	       static_cast<uint32_t>(b);
}

int clampi(int v, int lo, int hi) {
	return std::max(lo, std::min(v, hi));
}

// Resample one axis of an image stored as a flat vector of float pixels (4
// channels interleaved). srcLen is the length of the axis being resampled;
// otherLen is the length of the untouched axis. If horizontal is true, x is the
// resampled axis; otherwise y is. The result has dstLen samples along the
// resampled axis.
std::vector<glm::vec4> resampleAxis(const std::vector<glm::vec4> &src, int srcLen, int otherLen, int dstLen, int a,
                                    bool horizontal) {
	std::vector<glm::vec4> dst(static_cast<size_t>(dstLen) * otherLen);

	// Scale from destination space back to source space. For downsampling
	// (ratio > 1) the kernel is widened to act as a low-pass filter; for
	// upsampling the kernel keeps its native width.
	float ratio = static_cast<float>(srcLen) / static_cast<float>(dstLen);
	float support = (ratio > 1.0f) ? ratio : 1.0f;
	float invSupport = 1.0f / support;

	for (int d = 0; d < dstLen; d++) {
		// Centre of destination sample d mapped into source coordinates.
		float centre = (static_cast<float>(d) + 0.5f) * ratio - 0.5f;
		int first = static_cast<int>(std::ceil(centre - support * static_cast<float>(a)));
		int last = static_cast<int>(std::floor(centre + support * static_cast<float>(a)));

		// Precompute normalized weights for this destination position.
		float weightSum = 0.0f;
		std::vector<float> weights(static_cast<size_t>(last - first + 1));
		for (int s = first; s <= last; s++) {
			float w = lanczosKernel((static_cast<float>(s) - centre) * invSupport, a);
			weights[static_cast<size_t>(s - first)] = w;
			weightSum += w;
		}
		if (weightSum == 0.0f) {
			weightSum = 1.0f;
		}

		for (int o = 0; o < otherLen; o++) {
			glm::vec4 acc(0.0f);
			for (int s = first; s <= last; s++) {
				int cs = clampi(s, 0, srcLen - 1);
				size_t idx = horizontal ? static_cast<size_t>(o) * srcLen + cs : static_cast<size_t>(cs) * otherLen + o;
				acc += src[idx] * weights[static_cast<size_t>(s - first)];
			}
			acc /= weightSum;
			size_t out = horizontal ? static_cast<size_t>(o) * dstLen + d : static_cast<size_t>(d) * otherLen + o;
			dst[out] = acc;
		}
	}
	return dst;
}

} // namespace

float lanczosKernel(float x, int a) {
	if (x <= -static_cast<float>(a) || x >= static_cast<float>(a)) {
		return 0.0f;
	}
	return sinc(x) * sinc(x / static_cast<float>(a));
}

std::vector<uint32_t> resampleLanczos(const std::vector<uint32_t> &src, int sw, int sh, int dw, int dh, int a) {
	std::vector<uint32_t> out(static_cast<size_t>(std::max(0, dw)) * std::max(0, dh));
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || a <= 0) {
		return out;
	}

	// Unpack to float once so both passes operate in linear-in-8-bit space.
	std::vector<glm::vec4> fsrc(static_cast<size_t>(sw) * sh);
	for (size_t i = 0; i < fsrc.size(); i++) {
		fsrc[i] = unpack(src[i]);
	}

	// Horizontal pass: sw -> dw, height unchanged. Result is dw wide, sh tall.
	std::vector<glm::vec4> horiz = resampleAxis(fsrc, sw, sh, dw, a, true);

	// Vertical pass: sh -> dh, width unchanged at dw.
	std::vector<glm::vec4> vert = resampleAxis(horiz, sh, dw, dh, a, false);

	for (size_t i = 0; i < out.size(); i++) {
		out[i] = pack(vert[i]);
	}
	return out;
}

std::vector<uint32_t> downsampleHalfLanczos(const std::vector<uint32_t> &src, int sw, int sh, int a) {
	int dw = std::max(1, sw / 2);
	int dh = std::max(1, sh / 2);
	return resampleLanczos(src, sw, sh, dw, dh, a);
}
