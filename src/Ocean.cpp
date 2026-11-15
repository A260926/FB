#include "Ocean.h"

#include <array>
#include <cmath>
#include <glm/glm.hpp>

namespace {
struct Wave {
	glm::vec2 dir;
	float length;
	float amplitude;
	float speed;
	float steepness;
};

// A few directional waves of decreasing scale give a natural-looking swell.
const std::array<Wave, 4> kWaves = {{
    {{1.0f, 0.0f}, 1.6f, 0.10f, 0.9f, 0.8f},
    {{0.6f, 0.8f}, 0.9f, 0.055f, 1.2f, 0.7f},
    {{-0.7f, 0.5f}, 0.5f, 0.03f, 1.6f, 0.6f},
    {{0.2f, -1.0f}, 0.28f, 0.015f, 2.1f, 0.5f},
}};

// Gerstner displacement of a base point (x, z) at time t: waves push crests
// forward (horizontal) as well as up (vertical).
glm::vec3 gerstner(float x, float z, float t) {
	glm::vec3 p(x, 0.0f, z);
	for (const Wave &w : kWaves) {
		glm::vec2 d = glm::normalize(w.dir);
		float k = 2.0f * 3.14159265f / w.length;
		float phase = k * glm::dot(d, glm::vec2(x, z)) + w.speed * t;
		float q = w.steepness / (k * w.amplitude * static_cast<float>(kWaves.size()));
		p.x += q * w.amplitude * d.x * std::cos(phase);
		p.z += q * w.amplitude * d.y * std::cos(phase);
		p.y += w.amplitude * std::sin(phase);
	}
	return p;
}
} // namespace

std::vector<ModelTriangle> generateOcean(int gridN, float size, float time) {
	int n = gridN;
	std::vector<std::vector<glm::vec3>> pos(n + 1, std::vector<glm::vec3>(n + 1));
	for (int i = 0; i <= n; i++) {
		for (int j = 0; j <= n; j++) {
			float x = (static_cast<float>(i) / n - 0.5f) * size;
			float z = (static_cast<float>(j) / n - 0.5f) * size;
			pos[i][j] = gerstner(x, z, time);
		}
	}
	// Smooth normals from displaced-grid finite differences.
	std::vector<std::vector<glm::vec3>> nrm(n + 1, std::vector<glm::vec3>(n + 1, glm::vec3(0, 1, 0)));
	for (int i = 0; i <= n; i++) {
		for (int j = 0; j <= n; j++) {
			glm::vec3 dx = pos[std::min(i + 1, n)][j] - pos[std::max(i - 1, 0)][j];
			glm::vec3 dz = pos[i][std::min(j + 1, n)] - pos[i][std::max(j - 1, 0)];
			glm::vec3 nn = glm::normalize(glm::cross(dz, dx));
			if (nn.y < 0.0f)
				nn = -nn;
			nrm[i][j] = nn;
		}
	}

	std::vector<ModelTriangle> tris;
	tris.reserve(static_cast<size_t>(n) * n * 2);
	Colour water(70, 120, 180);
	auto quad = [&](int i0, int j0, int i1, int j1, int i2, int j2) {
		ModelTriangle t;
		t.vertices = {pos[i0][j0], pos[i1][j1], pos[i2][j2]};
		t.vertexNormals = {nrm[i0][j0], nrm[i1][j1], nrm[i2][j2]};
		t.colour = water;
		t.material = Material::Glass;
		t.normal = glm::normalize(glm::cross(t.vertices[1] - t.vertices[0], t.vertices[2] - t.vertices[0]));
		if (t.normal.y < 0.0f)
			t.normal = -t.normal;
		tris.push_back(t);
	};
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < n; j++) {
			quad(i, j, i + 1, j, i + 1, j + 1);
			quad(i, j, i + 1, j + 1, i, j + 1);
		}
	}
	return tris;
}
