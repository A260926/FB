#include "EdgeRaster.h"

#include <algorithm>
#include <cmath>
#include <limits>

int64_t orient2d(int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t cx, int64_t cy) {
	// (b - a) x (c - a), computed in 64 bits so sub-pixel fixed-point products
	// never overflow.
	return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

void clearDepth(std::vector<float> &depth, std::vector<float> &hiZ, int w, int h) {
	const float kFar = std::numeric_limits<float>::infinity();
	depth.assign(static_cast<size_t>(w) * static_cast<size_t>(h), kFar);
	int tilesW = (w + EDGE_RASTER_TILE - 1) / EDGE_RASTER_TILE;
	int tilesH = (h + EDGE_RASTER_TILE - 1) / EDGE_RASTER_TILE;
	hiZ.assign(static_cast<size_t>(tilesW) * static_cast<size_t>(tilesH), kFar);
}

namespace {

// Round a floating screen coordinate to sub-pixel fixed point (units of 1/256).
inline int64_t toFixed(float v) {
	return static_cast<int64_t>(std::lround(v * static_cast<float>(EDGE_RASTER_SUBPIXEL_ONE)));
}

// Top-left fill rule for a counter-clockwise winding in y-down screen space.
// An edge from (x0,y0) to (x1,y1) is a "top" edge when it is horizontal and
// points left (dx < 0), or a "left" edge when it points downward (dy > 0).
// Such edges keep bias 0 (a sample exactly on them counts as inside); all other
// edges get bias -1 so a shared edge is owned by exactly one triangle.
inline int edgeBias(int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
	int64_t dx = x1 - x0;
	int64_t dy = y1 - y0;
	bool topLeft = (dy > 0) || (dy == 0 && dx < 0);
	return topLeft ? 0 : -1;
}

// Recompute one tile's hierarchical-Z maximum from the depth buffer. Keeping
// hiZ equal to the true tile maximum (never below it) makes the less-than
// rejection both correct and tight.
inline void refreshTileHiZ(const std::vector<float> &depth, std::vector<float> &hiZ, int tileX, int tileY, int tilesW,
                           int w, int h) {
	int px0 = tileX * EDGE_RASTER_TILE;
	int py0 = tileY * EDGE_RASTER_TILE;
	int px1 = std::min(px0 + EDGE_RASTER_TILE, w);
	int py1 = std::min(py0 + EDGE_RASTER_TILE, h);
	float m = -std::numeric_limits<float>::infinity();
	for (int y = py0; y < py1; y++) {
		const float *row = &depth[static_cast<size_t>(y) * w];
		for (int x = px0; x < px1; x++)
			m = std::max(m, row[x]);
	}
	hiZ[static_cast<size_t>(tileY) * tilesW + tileX] = m;
}

} // namespace

void rasterizeTriangle(Canvas &canvas, std::vector<float> &depth, std::vector<float> &hiZ, glm::vec3 p0, glm::vec3 p1,
                       glm::vec3 p2, uint32_t colour) {
	int w = static_cast<int>(canvas.width);
	int h = static_cast<int>(canvas.height);
	if (w <= 0 || h <= 0)
		return;

	// Snap vertices to sub-pixel fixed point.
	int64_t X0 = toFixed(p0.x), Y0 = toFixed(p0.y);
	int64_t X1 = toFixed(p1.x), Y1 = toFixed(p1.y);
	int64_t X2 = toFixed(p2.x), Y2 = toFixed(p2.y);
	float z0 = p0.z, z1 = p1.z, z2 = p2.z;

	// Total signed area (twice). Fix the winding to counter-clockwise so the
	// coverage test is a uniform "all edges >= 0"; swapping two vertices flips
	// the winding while leaving the triangle (and its per-vertex depth) intact.
	int64_t area = orient2d(X0, Y0, X1, Y1, X2, Y2);
	if (area == 0)
		return; // degenerate, zero coverage
	if (area < 0) {
		std::swap(X1, X2);
		std::swap(Y1, Y2);
		std::swap(z1, z2);
		area = -area;
	}

	// Bounding box in whole pixels, clipped to the canvas.
	int64_t minXf = std::min({X0, X1, X2});
	int64_t maxXf = std::max({X0, X1, X2});
	int64_t minYf = std::min({Y0, Y1, Y2});
	int64_t maxYf = std::max({Y0, Y1, Y2});
	int minX = static_cast<int>(minXf >> EDGE_RASTER_SUBPIXEL_BITS);
	int maxX = static_cast<int>(maxXf >> EDGE_RASTER_SUBPIXEL_BITS);
	int minY = static_cast<int>(minYf >> EDGE_RASTER_SUBPIXEL_BITS);
	int maxY = static_cast<int>(maxYf >> EDGE_RASTER_SUBPIXEL_BITS);
	minX = std::max(minX, 0);
	minY = std::max(minY, 0);
	maxX = std::min(maxX, w - 1);
	maxY = std::min(maxY, h - 1);
	if (minX > maxX || minY > maxY)
		return;

	// Edge coefficients. Edge i is opposite vertex i, so its value is the
	// barycentric weight of vertex i:
	//   w0 <- edge V1->V2, w1 <- edge V2->V0, w2 <- edge V0->V1.
	// The edge function is A*sx + B*sy + C; per-pixel it steps by A*256 in x and
	// B*256 in y (one pixel is 256 sub-pixel units).
	int64_t A0 = Y1 - Y2, B0 = X2 - X1, C0 = X1 * Y2 - X2 * Y1;
	int64_t A1 = Y2 - Y0, B1 = X0 - X2, C1 = X2 * Y0 - X0 * Y2;
	int64_t A2 = Y0 - Y1, B2 = X1 - X0, C2 = X0 * Y1 - X1 * Y0;
	int bias0 = edgeBias(X1, Y1, X2, Y2);
	int bias1 = edgeBias(X2, Y2, X0, Y0);
	int bias2 = edgeBias(X0, Y0, X1, Y1);
	int64_t stepX0 = A0 * EDGE_RASTER_SUBPIXEL_ONE, stepY0 = B0 * EDGE_RASTER_SUBPIXEL_ONE;
	int64_t stepX1 = A1 * EDGE_RASTER_SUBPIXEL_ONE, stepY1 = B1 * EDGE_RASTER_SUBPIXEL_ONE;
	int64_t stepX2 = A2 * EDGE_RASTER_SUBPIXEL_ONE, stepY2 = B2 * EDGE_RASTER_SUBPIXEL_ONE;

	// Nearest point of the triangle (barycentric interpolation is convex, so the
	// interpolated depth never drops below the minimum vertex depth). This is the
	// conservative bound the hierarchical-Z rejection needs.
	float triMinDepth = std::min({z0, z1, z2});
	double invArea = 1.0 / static_cast<double>(area);

	int tilesW = (w + EDGE_RASTER_TILE - 1) / EDGE_RASTER_TILE;
	int tileMinX = minX / EDGE_RASTER_TILE, tileMaxX = maxX / EDGE_RASTER_TILE;
	int tileMinY = minY / EDGE_RASTER_TILE, tileMaxY = maxY / EDGE_RASTER_TILE;

	for (int ty = tileMinY; ty <= tileMaxY; ty++) {
		for (int tx = tileMinX; tx <= tileMaxX; tx++) {
			size_t tileIdx = static_cast<size_t>(ty) * tilesW + tx;
			// Hierarchical-Z reject: if the triangle's nearest point is still
			// farther than everything already stored in this tile, no fragment
			// here can pass a less-than test, so skip the whole 8x8 block.
			if (triMinDepth > hiZ[tileIdx])
				continue;

			int x0 = std::max(minX, tx * EDGE_RASTER_TILE);
			int x1 = std::min(maxX, tx * EDGE_RASTER_TILE + EDGE_RASTER_TILE - 1);
			int y0 = std::max(minY, ty * EDGE_RASTER_TILE);
			int y1 = std::min(maxY, ty * EDGE_RASTER_TILE + EDGE_RASTER_TILE - 1);

			// Evaluate the three edge functions once at the tile's first pixel
			// centre, then advance them incrementally across the tile.
			int64_t sx = static_cast<int64_t>(x0) * EDGE_RASTER_SUBPIXEL_ONE + EDGE_RASTER_SUBPIXEL_ONE / 2;
			int64_t sy = static_cast<int64_t>(y0) * EDGE_RASTER_SUBPIXEL_ONE + EDGE_RASTER_SUBPIXEL_ONE / 2;
			int64_t w0Row = A0 * sx + B0 * sy + C0;
			int64_t w1Row = A1 * sx + B1 * sy + C1;
			int64_t w2Row = A2 * sx + B2 * sy + C2;

			for (int py = y0; py <= y1; py++) {
				int64_t e0 = w0Row, e1 = w1Row, e2 = w2Row;
				size_t rowBase = static_cast<size_t>(py) * w;
				for (int px = x0; px <= x1; px++) {
					// Top-left biased half-space test. All three inside -> covered.
					if ((e0 + bias0) >= 0 && (e1 + bias1) >= 0 && (e2 + bias2) >= 0) {
						// Area-normalised barycentric weights (unbiased edge
						// values) interpolate the per-vertex depth.
						double l0 = static_cast<double>(e0) * invArea;
						double l1 = static_cast<double>(e1) * invArea;
						double l2 = static_cast<double>(e2) * invArea;
						float d = static_cast<float>(l0 * z0 + l1 * z1 + l2 * z2);
						size_t idx = rowBase + px;
						if (d < depth[idx]) {
							depth[idx] = d;
							canvas.pixels[idx] = colour;
						}
					}
					e0 += stepX0;
					e1 += stepX1;
					e2 += stepX2;
				}
				w0Row += stepY0;
				w1Row += stepY1;
				w2Row += stepY2;
			}

			// This tile's depths may have shrunk; retighten its hierarchical-Z.
			refreshTileHiZ(depth, hiZ, tx, ty, tilesW, w, h);
		}
	}
}

QuadDerivatives quadDerivatives(glm::vec2 topLeft, glm::vec2 topRight, glm::vec2 bottomLeft, glm::vec2 bottomRight) {
	// Finite differences across the 2x2 quad. Average the two available rows /
	// columns so the estimate is stable even near a triangle edge where one
	// helper lane may be off the primitive.
	QuadDerivatives d;
	d.ddx = 0.5f * ((topRight - topLeft) + (bottomRight - bottomLeft));
	d.ddy = 0.5f * ((bottomLeft - topLeft) + (bottomRight - topRight));
	return d;
}
