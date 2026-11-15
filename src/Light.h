#pragma once

#include <glm/glm.hpp>

// Point/Spot with radius > 0 is a flat (disk) area light; Volume samples a whole
// sphere of radius `radius`; Rectangle samples a uAxis-by-vAxis rectangle for a
// soft rectangular penumbra (e.g. a softbox / window light); Triangle samples the
// triangle (position, position + uAxis, position + vAxis) for a soft triangular
// penumbra (reuses the uAxis/vAxis fields as the two edge vectors).
enum class LightType { Point, Directional, Spot, Volume, Rectangle, Triangle };

// A light source. A point light with radius > 0 is an area light (soft shadows).
struct Light {
	LightType type = LightType::Point;
	glm::vec3 position{0.0f, 0.95f, 0.0f};
	glm::vec3 direction{0.0f, -1.0f, 0.0f}; // for Directional / Spot
	glm::vec3 colour{1.0f, 1.0f, 1.0f};     // 0..1 tint
	float intensity = 40.0f;
	float radius = 0.0f;  // area/volume-light size; 0 = a hard point light
	float coneCos = 0.9f; // Spot cone, cosine of the half-angle
	// Distance attenuation 1 / (kc + kl*d + kq*d^2). The defaults reproduce the
	// physical inverse-square falloff; set kc/kl for the classic OpenGL model.
	float attenConstant = 0.0f;
	float attenLinear = 0.0f;
	float attenQuadratic = 12.566370f; // 4*pi
	// Half-extent vectors of a Rectangle light (spanning position +- uAxis +- vAxis).
	glm::vec3 uAxis{0.3f, 0.0f, 0.0f};
	glm::vec3 vAxis{0.0f, 0.0f, 0.3f};
};
