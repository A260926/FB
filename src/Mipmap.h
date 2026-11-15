#pragma once

#include <TextureMap.h>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

// A mip pyramid built from a TextureMap: successive box-filtered half-resolution
// levels down to 1x1. Sampling is bilinear within a level and linear across two
// adjacent levels (trilinear). A higher LOD selects coarser levels, which
// removes minification aliasing / shimmer on distant, minified surfaces.
struct MipTexture {
	std::vector<std::vector<uint32_t>> levels; // 0xAARRGGBB pixels per level
	std::vector<int> widths;
	std::vector<int> heights;

	explicit MipTexture(const TextureMap &tex);

	int levelCount() const { return static_cast<int>(levels.size()); }
	// Bilinear lookup within one level; (u, v) in [0,1]. Returns RGB 0..255.
	glm::vec3 sampleBilinear(int level, float u, float v) const;
	// Trilinear lookup: blends the two levels bracketing `lod` (0 = full res).
	glm::vec3 sampleTrilinear(float u, float v, float lod) const;
};
