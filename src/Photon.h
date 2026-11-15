#pragma once

#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

// A photon deposited on a diffuse surface: where it landed, the power it carries,
// and the direction it arrived from.
struct Photon {
	glm::vec3 position{};
	glm::vec3 power{};
	glm::vec3 direction{};
};

// A photon map with a uniform spatial-hash grid for radius gathering.
class PhotonMap {
public:
	std::vector<Photon> photons;

	// Bucket the photons into cells of side `radius`.
	void build(float radius);

	// Estimate irradiance at `point`: sum the power of photons within `radius`
	// that arrived on the lit side of `normal`, divided by the disk area.
	glm::vec3 gather(const glm::vec3 &point, const glm::vec3 &normal, float radius) const;

private:
	float cellSize_ = 0.1f;
	std::unordered_map<long long, std::vector<int>> grid_;
	static long long cellKey(int x, int y, int z);
};
