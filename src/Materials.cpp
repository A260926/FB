#include "Materials.h"

#include <unordered_map>

MaterialPreset materialPreset(const std::string &name) {
	static const std::unordered_map<std::string, MaterialPreset> table = {
	    // Metals: tinted reflectors (Material::Metal), roughness sets the polish.
	    {"gold", {Colour(255, 215, 0), Material::Metal, 0.08f}},
	    {"silver", {Colour(230, 230, 235), Material::Metal, 0.05f}},
	    {"chrome", {Colour(200, 200, 210), Material::Metal, 0.03f}},
	    {"copper", {Colour(200, 110, 70), Material::Metal, 0.12f}},
	    {"brass", {Colour(200, 160, 60), Material::Metal, 0.15f}},
	    {"bronze", {Colour(150, 100, 50), Material::Metal, 0.18f}},
	    // Gemstones and organics: saturated diffuse.
	    {"emerald", {Colour(20, 150, 60), Material::Diffuse, 0.4f}},
	    {"jade", {Colour(85, 180, 120), Material::Diffuse, 0.4f}},
	    {"obsidian", {Colour(30, 30, 42), Material::Diffuse, 0.3f}},
	    {"pearl", {Colour(230, 215, 220), Material::Diffuse, 0.5f}},
	    {"ruby", {Colour(200, 30, 40), Material::Diffuse, 0.3f}},
	    {"turquoise", {Colour(60, 200, 190), Material::Diffuse, 0.4f}},
	    // Plastics: bright diffuse.
	    {"black plastic", {Colour(20, 20, 20), Material::Diffuse, 0.3f}},
	    {"cyan plastic", {Colour(0, 130, 130), Material::Diffuse, 0.3f}},
	    {"green plastic", {Colour(20, 130, 20), Material::Diffuse, 0.3f}},
	    {"red plastic", {Colour(180, 20, 20), Material::Diffuse, 0.3f}},
	    {"white plastic", {Colour(220, 220, 220), Material::Diffuse, 0.3f}},
	    {"yellow plastic", {Colour(200, 200, 20), Material::Diffuse, 0.3f}},
	    // Rubbers: muted diffuse.
	    {"black rubber", {Colour(30, 30, 30), Material::Diffuse, 0.8f}},
	    {"cyan rubber", {Colour(60, 120, 120), Material::Diffuse, 0.8f}},
	    {"green rubber", {Colour(60, 120, 60), Material::Diffuse, 0.8f}},
	    {"red rubber", {Colour(140, 50, 50), Material::Diffuse, 0.8f}},
	    {"white rubber", {Colour(200, 200, 200), Material::Diffuse, 0.8f}},
	    {"yellow rubber", {Colour(170, 170, 60), Material::Diffuse, 0.8f}},
	};
	auto it = table.find(name);
	if (it != table.end())
		return it->second;
	return {Colour(160, 160, 160), Material::Diffuse, 0.2f};
}
