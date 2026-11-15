#include "Photon.h"

#include <cmath>

long long PhotonMap::cellKey(int x, int y, int z) {
	auto f = [](int v) -> long long { return static_cast<long long>((v + (1 << 20)) & 0x1FFFFF); };
	return (f(x) << 42) | (f(y) << 21) | f(z);
}

void PhotonMap::build(float radius) {
	cellSize_ = radius;
	grid_.clear();
	for (int i = 0; i < static_cast<int>(photons.size()); i++) {
		glm::vec3 c = photons[i].position / cellSize_;
		grid_[cellKey(static_cast<int>(std::floor(c.x)), static_cast<int>(std::floor(c.y)),
		              static_cast<int>(std::floor(c.z)))]
		    .push_back(i);
	}
}

glm::vec3 PhotonMap::gather(const glm::vec3 &point, const glm::vec3 &normal, float radius) const {
	glm::vec3 c = point / cellSize_;
	int cx = static_cast<int>(std::floor(c.x));
	int cy = static_cast<int>(std::floor(c.y));
	int cz = static_cast<int>(std::floor(c.z));
	glm::vec3 sum(0.0f);
	float r2 = radius * radius;
	for (int dx = -1; dx <= 1; dx++) {
		for (int dy = -1; dy <= 1; dy++) {
			for (int dz = -1; dz <= 1; dz++) {
				auto it = grid_.find(cellKey(cx + dx, cy + dy, cz + dz));
				if (it == grid_.end())
					continue;
				for (int idx : it->second) {
					const Photon &p = photons[idx];
					glm::vec3 d = p.position - point;
					if (glm::dot(d, d) > r2)
						continue;
					if (glm::dot(normal, -p.direction) <= 0.0f)
						continue; // arrived on the back side
					sum += p.power;
				}
			}
		}
	}
	return sum / (3.14159265f * r2); // irradiance = power / area
}
