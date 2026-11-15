#pragma once

#include <Canvas.h>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

// Hardware-style half-space / edge-function triangle rasteriser.
//
// Follows Fabian "ryg" Giesen's notes ("Triangle rasterization in practice",
// "Optimizing the basic rasterizer", "Depth buffers done quick") and the
// GPU-pipeline series parts 6/7/8: integer edge functions with sub-pixel
// fixed point, a top-left fill rule for watertight shared edges, hierarchical-Z
// tile rejection, and 2x2-quad attribute derivatives for texture LOD.

// Coarse tile size (in pixels) for the hierarchical-Z pyramid.
constexpr int EDGE_RASTER_TILE = 8;
// Sub-pixel precision: coordinates are snapped to 1/256 of a pixel.
constexpr int EDGE_RASTER_SUBPIXEL_BITS = 8;
constexpr int EDGE_RASTER_SUBPIXEL_ONE = 1 << EDGE_RASTER_SUBPIXEL_BITS; // 256

// Integer edge function evaluated as a 64-bit determinant. Returns twice the
// signed area of triangle (a, b, c); positive for a counter-clockwise winding
// in a y-down screen space. Also serves as an incremental point-in-triangle
// half-space test.
int64_t orient2d(int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t cx, int64_t cy);

// Reset the depth buffer to "far" and the hierarchical-Z tile maxima to "far".
// Sizes both vectors from the canvas dimensions. Depth uses a less-than test,
// so "far" is +infinity.
void clearDepth(std::vector<float> &depth, std::vector<float> &hiZ, int w, int h);

// Rasterise one triangle. Each vertex is (screen_x, screen_y, depth). Coverage
// uses integer edge functions with sub-pixel snapping and incremental stepping;
// the top-left fill rule keeps shared edges watertight (drawn exactly once).
// Depth is interpolated with area-normalised barycentric weights and resolved
// with a less-than z-test. Hierarchical-Z skips whole 8x8 tiles that the
// triangle cannot possibly bring nearer.
void rasterizeTriangle(Canvas &canvas, std::vector<float> &depth, std::vector<float> &hiZ, glm::vec3 p0, glm::vec3 p1,
                       glm::vec3 p2, uint32_t colour);

// Screen-space partial derivatives of a 2x2 pixel quad's interpolated attribute
// (for example a uv coordinate), by finite difference: ddx = right - left,
// ddy = bottom - top. Handy for choosing a texture mip level.
struct QuadDerivatives {
	glm::vec2 ddx{};
	glm::vec2 ddy{};
};
QuadDerivatives quadDerivatives(glm::vec2 topLeft, glm::vec2 topRight, glm::vec2 bottomLeft, glm::vec2 bottomRight);
