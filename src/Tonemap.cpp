#include "Tonemap.h"

#include <algorithm>
#include <cmath>

glm::vec3 applyExposure(const glm::vec3 &linear, float ev) {
	return linear * std::pow(2.0f, ev);
}

glm::vec3 acesFilmic(const glm::vec3 &x) {
	// Narkowicz 2015 ACES approximation.
	const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
	glm::vec3 num = x * (a * x + glm::vec3(b));
	glm::vec3 den = x * (c * x + glm::vec3(d)) + glm::vec3(e);
	return glm::clamp(num / den, 0.0f, 1.0f);
}

static float l2s(float c) {
	c = std::clamp(c, 0.0f, 1.0f);
	return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
static float s2l(float c) {
	c = std::clamp(c, 0.0f, 1.0f);
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

glm::vec3 linearToSRGB(const glm::vec3 &c) {
	return glm::vec3(l2s(c.r), l2s(c.g), l2s(c.b));
}
glm::vec3 sRGBToLinear(const glm::vec3 &c) {
	return glm::vec3(s2l(c.r), s2l(c.g), s2l(c.b));
}

void gradeCanvas(Canvas &canvas, float ev, bool aces) {
	size_t n = canvas.width * canvas.height;
	for (size_t i = 0; i < n; i++) {
		uint32_t p = canvas.pixels[i];
		glm::vec3 lin((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
		lin /= 255.0f;
		lin = applyExposure(lin, ev);
		lin = aces ? acesFilmic(lin) : glm::clamp(lin, 0.0f, 1.0f);
		glm::vec3 srgb = linearToSRGB(lin) * 255.0f;
		int r = std::min(255, static_cast<int>(srgb.r + 0.5f));
		int g = std::min(255, static_cast<int>(srgb.g + 0.5f));
		int b = std::min(255, static_cast<int>(srgb.b + 0.5f));
		canvas.pixels[i] = (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
	}
}
