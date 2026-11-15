#pragma once

#include <glm/glm.hpp>
#include <vector>

// A Ward-style irradiance cache: expensive indirect irradiance is computed at
// sparse points and reused (weighted-interpolated) for nearby points with a
// similar normal, rather than recomputed per pixel. This is the standard speedup
// for the diffuse indirect / final-gather term.
struct IrradianceRecord {
	glm::vec3 position{};
	glm::vec3 normal{};
	glm::vec3 irradiance{};
	float radius = 1.0f; // valid harmonic-mean distance to nearby geometry
};

class IrradianceCache {
public:
	// If enough valid records surround (p, n), fill `out` with their weighted
	// average and return true (a cache hit); otherwise return false so the caller
	// computes the irradiance and store()s it.
	bool lookup(const glm::vec3 &p, const glm::vec3 &n, glm::vec3 &out) const;
	void store(const glm::vec3 &p, const glm::vec3 &n, const glm::vec3 &irradiance, float radius);

	int size() const { return static_cast<int>(records_.size()); }
	void setThreshold(float t) { threshold_ = t; }

private:
	std::vector<IrradianceRecord> records_;
	float threshold_ = 1.5f; // higher = tighter reuse (Ward's `a`)
};
