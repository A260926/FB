#include "Worley.h"

#include <cmath>
#include <limits>

// Deterministic integer hash mixing two/three signed cell coordinates into a
// 32-bit value. This is a Wang/xxHash-style avalanche mix: multiply by large odd
// constants and xor-shift so that neighbouring cells produce well-scrambled,
// uncorrelated outputs. It replaces a permutation table lookup and works for the
// full integer range without wrapping into [0, 255].
static unsigned int hashInt(unsigned int x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

static unsigned int hashCell(int ix, int iy) {
	unsigned int h = hashInt(static_cast<unsigned int>(ix));
	h = hashInt(h ^ (static_cast<unsigned int>(iy) * 0x9e3779b9u));
	return h;
}

static unsigned int hashCell(int ix, int iy, int iz) {
	unsigned int h = hashInt(static_cast<unsigned int>(ix));
	h = hashInt(h ^ (static_cast<unsigned int>(iy) * 0x9e3779b9u));
	h = hashInt(h ^ (static_cast<unsigned int>(iz) * 0x85ebca6bu));
	return h;
}

// Map a 32-bit hash to a float in [0, 1). 2^-24 keeps us within the exactly
// representable mantissa range so the result never rounds up to 1.0f.
static float toUnitFloat(unsigned int h) {
	return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

// Deterministic feature-point offset in [0, 1)^2 for cell (ix, iy).
static glm::vec2 featurePoint(int ix, int iy) {
	unsigned int hx = hashCell(ix, iy);
	unsigned int hy = hashCell(ix + 0x1000, iy - 0x1000);
	return glm::vec2(toUnitFloat(hx), toUnitFloat(hy));
}

// Deterministic feature-point offset in [0, 1)^3 for cell (ix, iy, iz).
static glm::vec3 featurePoint(int ix, int iy, int iz) {
	unsigned int hx = hashCell(ix, iy, iz);
	unsigned int hy = hashCell(ix + 0x1000, iy - 0x1000, iz + 0x2000);
	unsigned int hz = hashCell(ix - 0x2000, iy + 0x2000, iz - 0x1000);
	return glm::vec3(toUnitFloat(hx), toUnitFloat(hy), toUnitFloat(hz));
}

// Fold a squared distance into the running nearest / second-nearest pair.
static void consider(float d2, float &f1, float &f2) {
	if (d2 < f1) {
		f2 = f1;
		f1 = d2;
	} else if (d2 < f2) {
		f2 = d2;
	}
}

WorleyResult worley(const glm::vec2 &p, float density) {
	glm::vec2 q = p * density;
	int cx = static_cast<int>(std::floor(q.x));
	int cy = static_cast<int>(std::floor(q.y));

	float f1 = std::numeric_limits<float>::max();
	float f2 = std::numeric_limits<float>::max();

	for (int oy = -1; oy <= 1; oy++) {
		for (int ox = -1; ox <= 1; ox++) {
			int gx = cx + ox;
			int gy = cy + oy;
			glm::vec2 site = glm::vec2(gx, gy) + featurePoint(gx, gy);
			glm::vec2 diff = site - q;
			consider(glm::dot(diff, diff), f1, f2);
		}
	}

	return {std::sqrt(f1), std::sqrt(f2)};
}

float worleyF1(const glm::vec2 &p, float density) {
	return worley(p, density).f1;
}

float worleyCells(const glm::vec2 &p, float density) {
	WorleyResult r = worley(p, density);
	float sum = r.f2 + r.f1;
	if (sum <= 0.0f) {
		return 0.0f;
	}
	return (r.f2 - r.f1) / sum;
}

WorleyResult worley(const glm::vec3 &p, float density) {
	glm::vec3 q = p * density;
	int cx = static_cast<int>(std::floor(q.x));
	int cy = static_cast<int>(std::floor(q.y));
	int cz = static_cast<int>(std::floor(q.z));

	float f1 = std::numeric_limits<float>::max();
	float f2 = std::numeric_limits<float>::max();

	for (int oz = -1; oz <= 1; oz++) {
		for (int oy = -1; oy <= 1; oy++) {
			for (int ox = -1; ox <= 1; ox++) {
				int gx = cx + ox;
				int gy = cy + oy;
				int gz = cz + oz;
				glm::vec3 site = glm::vec3(gx, gy, gz) + featurePoint(gx, gy, gz);
				glm::vec3 diff = site - q;
				consider(glm::dot(diff, diff), f1, f2);
			}
		}
	}

	return {std::sqrt(f1), std::sqrt(f2)};
}
