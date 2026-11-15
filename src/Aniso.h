#pragma once

#include <TextureMap.h>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

// Anisotropic texture filtering, following ryg's "A trip through the Graphics
// Pipeline" part 4 (texture samplers). A stretched-on-screen surface has a
// texture footprint that is long along one screen axis and short along the
// other. Isotropic trilinear filtering must pick a single LOD for the whole
// footprint, so it blurs along the short axis to avoid aliasing along the long
// one. Anisotropic filtering instead takes several trilinear taps stepped along
// the long (major) axis, each at the finer LOD implied by the short (minor)
// axis, and averages them. That keeps detail along the minor axis while still
// suppressing aliasing along the major axis.

// Build a box-filtered mip chain as a plain vector of TextureMap levels, level
// 0 being the base and the last level 1x1. A separable Lanczos downsample could
// replace the box filter later for sharper coarse levels.
std::vector<TextureMap> buildMipChain(const TextureMap &base);

// Trilinear lookup across a mip chain: bilinearly sample the two levels
// bracketing `lod` (0 = full resolution) and blend by frac(lod). (u, v) in
// [0,1]; returns RGBA in 0..255 floats.
glm::vec4 sampleTrilinear(const std::vector<TextureMap> &mips, float u, float v, float lod);

// Anisotropic lookup. dudx = (du/dx, dv/dx) and dudy = (du/dy, dv/dy) are the
// screen-space gradients of the texture coordinates in [0,1] units (change in
// (u, v) per one pixel step in screen x and y respectively). The number of taps
// grows with the anisotropy ratio, capped at maxAniso. (u, v) in [0,1]; returns
// RGBA in 0..255 floats.
glm::vec4 sampleAnisotropic(const std::vector<TextureMap> &mips, float u, float v, glm::vec2 dudx, glm::vec2 dudy,
                            int maxAniso = 8);
