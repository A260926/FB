#include "Clouds.h"

#include "Noise.h"

#include <algorithm>
#include <cmath>

namespace {

// Vertical extent of the cloud slab, in world units.
constexpr float kSlabMin = 1.0f;
constexpr float kSlabMax = 3.0f;

// Volumetric extinction coefficient (Beer's law) and the Henyey-Greenstein
// anisotropy that biases scattering forward along the light direction.
constexpr float kExtinction = 6.0f;
constexpr float kAnisotropy = 0.35f;

// Sun and ambient sky radiance used by the single-scatter integrator.
const glm::vec3 kSunColour(1.0f, 0.95f, 0.85f);
const glm::vec3 kAmbient(0.35f, 0.42f, 0.55f);

// Smooth blue-to-white sky gradient used as the background behind the clouds.
glm::vec3 skyGradient(const glm::vec3 &dir) {
	float t = glm::clamp(dir.y * 0.5f + 0.5f, 0.0f, 1.0f);
	const glm::vec3 horizon(0.72f, 0.80f, 0.92f);
	const glm::vec3 zenith(0.22f, 0.42f, 0.82f);
	glm::vec3 sky = glm::mix(horizon, zenith, t);
	return sky * 255.0f;
}

// Henyey-Greenstein phase function; describes how much light scatters into the
// view direction given the angle between the view ray and the sun.
float henyeyGreenstein(float cosTheta, float g) {
	float gg = g * g;
	float denom = 1.0f + gg - 2.0f * g * cosTheta;
	denom = std::max(denom, 1e-4f);
	return (1.0f - gg) / (4.0f * 3.14159265358979323846f * std::pow(denom, 1.5f));
}

// Estimate transmittance from a sample point toward the sun with a short march.
float lightTransmittance(const glm::vec3 &pos, const glm::vec3 &sunDir, float time) {
	constexpr int kLightSteps = 6;
	constexpr float kLightStep = 0.18f;
	float opticalDepth = 0.0f;
	for (int i = 1; i <= kLightSteps; ++i) {
		glm::vec3 lp = pos + sunDir * (kLightStep * static_cast<float>(i));
		opticalDepth += cloudDensity(lp, time) * kLightStep;
	}
	return std::exp(-kExtinction * opticalDepth);
}

} // namespace

float cloudDensity(const glm::vec3 &p, float time) {
	// Advect the sample point so the field drifts like wind-blown vapour.
	const glm::vec3 wind(0.18f, 0.03f, 0.11f);
	glm::vec3 q = p * 0.55f + wind * time;

	float n = fractalNoise(q, 5); // fBm, roughly [0, 1]

	// Coverage/threshold shaping: subtract a coverage floor, renormalise to keep
	// the result in [0, 1], and taper density toward the slab edges so clouds
	// fade out rather than ending abruptly.
	constexpr float kCoverage = 0.48f;
	float d = glm::clamp((n - kCoverage) / (1.0f - kCoverage), 0.0f, 1.0f);

	float mid = 0.5f * (kSlabMin + kSlabMax);
	float half = 0.5f * (kSlabMax - kSlabMin);
	float edge = glm::clamp(1.0f - std::abs(p.y - mid) / half, 0.0f, 1.0f);
	edge = edge * edge; // soft, rounded vertical falloff

	return d * edge;
}

glm::vec3 raymarchClouds(const glm::vec3 &origin, const glm::vec3 &dir, const glm::vec3 &sunDir, float time) {
	glm::vec3 sky = skyGradient(dir);

	// Intersect the view ray with the horizontal slab y in [kSlabMin, kSlabMax].
	float t0;
	float t1;
	if (std::abs(dir.y) < 1e-4f) {
		if (origin.y < kSlabMin || origin.y > kSlabMax)
			return sky; // parallel to slab and outside it: pure sky
		t0 = 0.0f;
		t1 = 24.0f;
	} else {
		float ta = (kSlabMin - origin.y) / dir.y;
		float tb = (kSlabMax - origin.y) / dir.y;
		t0 = std::min(ta, tb);
		t1 = std::max(ta, tb);
		if (t1 <= 0.0f)
			return sky; // slab is entirely behind the camera
		t0 = std::max(t0, 0.0f);
	}

	constexpr int kSteps = 96;
	float dt = (t1 - t0) / static_cast<float>(kSteps);
	float cosTheta = glm::dot(glm::normalize(dir), glm::normalize(sunDir));
	float phase = henyeyGreenstein(cosTheta, kAnisotropy);

	float transmittance = 1.0f;
	glm::vec3 scattered(0.0f);

	for (int i = 0; i < kSteps; ++i) {
		float t = t0 + (static_cast<float>(i) + 0.5f) * dt;
		glm::vec3 pos = origin + dir * t;
		float d = cloudDensity(pos, time);
		if (d <= 0.001f)
			continue;

		// Fraction of light that interacts within this segment (energy-conserving
		// front-to-back compositing).
		float segT = std::exp(-kExtinction * d * dt);
		float lightT = lightTransmittance(pos, sunDir, time);

		glm::vec3 luminance = kSunColour * (lightT * phase * 6.0f) + kAmbient;
		scattered += transmittance * (1.0f - segT) * luminance;
		transmittance *= segT;

		if (transmittance < 0.01f)
			break; // fully opaque; nothing behind contributes
	}

	// Composite accumulated in-scattering over the remaining sky background.
	glm::vec3 colour = scattered * 255.0f + sky * transmittance;
	return glm::clamp(colour, glm::vec3(0.0f), glm::vec3(255.0f));
}
