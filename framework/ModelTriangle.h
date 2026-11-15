#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <array>
#include "Colour.h"
#include "TextureMap.h"
#include "TexturePoint.h"

// Surface response used by the ray tracer.
enum class Material {
	Diffuse,
	Mirror,
	Glass,
	Procedural,
	Bump,
	Parallax,
	Metal,
	Dispersive,
	Subsurface,
	NormalMap,
	Emissive
};

struct ModelTriangle {
	std::array<glm::vec3, 3> vertices{};
	std::array<TexturePoint, 3> texturePoints{}; // normalised uv in (x, y)
	std::array<glm::vec3, 3> vertexNormals{};
	Colour colour{};
	glm::vec3 normal{};
	Material material = Material::Diffuse;
	float roughness = 0.2f;              // for Material::Metal (0 = mirror, 1 = diffuse)
	std::shared_ptr<TextureMap> texture; // null unless the material had a map_Kd
	// Afterglow mean lifetime in nanoseconds, used only by the temporal
	// (afterglow) path tracer and only on Material::Diffuse surfaces. 0 = an
	// instantaneous response; a positive value adds one normalised exponential
	// dwell kernel of mean tau per diffuse interaction. Range when positive:
	// [1e-6, 1e6] ns. Non-diffuse materials must leave this at 0.
	double afterglowTauNs = 0.0;

	ModelTriangle();
	ModelTriangle(const glm::vec3 &v0, const glm::vec3 &v1, const glm::vec3 &v2, Colour trigColour);
	friend std::ostream &operator<<(std::ostream &os, const ModelTriangle &triangle);
};
