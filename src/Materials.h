#pragma once

#include <Colour.h>
#include <ModelTriangle.h>
#include <string>

// A named material preset resolved to this renderer's surface model: a base
// colour plus a Material (metals map to Material::Metal with a low roughness,
// everything else to Diffuse).
struct MaterialPreset {
	Colour colour;
	Material material = Material::Diffuse;
	float roughness = 0.2f;
};

// Look up one of the classic real-world material presets by name (case-sensitive,
// e.g. "gold", "chrome", "ruby", "green plastic"). Unknown names return a
// neutral grey diffuse.
MaterialPreset materialPreset(const std::string &name);
