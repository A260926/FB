#include "IrradianceCache.h"

#include <cmath>

// Ward's weighting: a record contributes where the point is close relative to
// the record's radius and the normals agree. w = 1 / (dist/radius + sqrt(1 - n.n)),
// and a record is usable when w exceeds the threshold.
bool IrradianceCache::lookup(const glm::vec3 &p, const glm::vec3 &n, glm::vec3 &out) const {
	glm::vec3 weighted(0.0f);
	float totalWeight = 0.0f;
	for (const IrradianceRecord &r : records_) {
		float nd = glm::dot(n, r.normal);
		if (nd <= 0.0f)
			continue; // opposing normals never share irradiance
		float dist = glm::length(p - r.position);
		float err = dist / r.radius + std::sqrt(std::max(0.0f, 1.0f - nd));
		if (err <= 0.0f)
			err = 1e-4f;
		float w = 1.0f / err;
		if (w < threshold_)
			continue;
		weighted += r.irradiance * w;
		totalWeight += w;
	}
	if (totalWeight <= 0.0f)
		return false;
	out = weighted / totalWeight;
	return true;
}

void IrradianceCache::store(const glm::vec3 &p, const glm::vec3 &n, const glm::vec3 &irradiance, float radius) {
	records_.push_back({p, n, irradiance, radius > 1e-4f ? radius : 1e-4f});
}
