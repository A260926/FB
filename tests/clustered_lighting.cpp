#include "task_support/check.h"

#include "ClusteredLighting.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace swr;

namespace {

constexpr float kEps = 1e-5f;

float dot3(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void sub3(const float a[3], const float b[3], float out[3])
{
	out[0] = a[0] - b[0];
	out[1] = a[1] - b[1];
	out[2] = a[2] - b[2];
}

bool closeVec(const std::array<float, 3> &a, const std::array<float, 3> &b, float tol = 1e-5f)
{
	for (int i = 0; i < 3; ++i) {
		if (std::fabs(a[i] - b[i]) > tol * (1.0f + std::fabs(b[i])))
			return false;
	}
	return true;
}

GBufferVertex makeVertex(
	const ClusterConfig &config,
	float x, float y, float depth,
	const float normal[3],
	const float albedo[3])
{
	GBufferVertex v;
	const float view[3] = {x, y, -depth};
	projectPerspective(config, view, v.clip);
	v.position[0] = x;
	v.position[1] = y;
	v.position[2] = -depth;
	v.normal[0] = normal[0];
	v.normal[1] = normal[1];
	v.normal[2] = normal[2];
	v.albedo[0] = albedo[0];
	v.albedo[1] = albedo[1];
	v.albedo[2] = albedo[2];
	return v;
}

// Axis-aligned view-space quad at fixed depth, two triangles. The x/y bounds
// are near-plane slopes (view coordinate per unit depth) so screen coverage is
// independent of the quad's depth.
void makeQuad(
	const ClusterConfig &config,
	float sx0, float sy0, float sx1, float sy1, float depth,
	const float normal[3],
	const float albedo[3],
	std::vector<GBufferVertex> &verts,
	std::vector<int> &indices)
{
	const float x0 = sx0 * depth;
	const float x1 = sx1 * depth;
	const float y0 = sy0 * depth;
	const float y1 = sy1 * depth;
	const int base = (int)verts.size();
	verts.push_back(makeVertex(config, x0, y0, depth, normal, albedo));
	verts.push_back(makeVertex(config, x1, y0, depth, normal, albedo));
	verts.push_back(makeVertex(config, x1, y1, depth, normal, albedo));
	verts.push_back(makeVertex(config, x0, y1, depth, normal, albedo));
	const int ids[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
	indices.insert(indices.end(), ids, ids + 6);
}

ClusteredLighting makeContext(const ClusterConfig &config, RasterMode mode)
{
	ClusteredLighting context(config);
	context.setRasterMode(mode);
	context.beginGeometry();
	return context;
}

void drawScene(ClusteredLighting &context, const std::vector<GBufferVertex> &verts,
	const std::vector<int> &indices)
{
	context.vertexProcessor().setVertexAttribPointer(0, sizeof(GBufferVertex), verts.data());
	std::vector<int> ids = indices;
	context.drawElements(DrawMode::Triangle, ids.size(), ids.data());
}

float expectedWindowDepth(const ClusterConfig &c, float depth)
{
	const float n = c.nearPlane;
	const float f = c.farPlane;
	return 0.5f * ((f + n) / (f - n) - 2.0f * f * n / ((f - n) * depth)) + 0.5f;
}

// ---------------------------------------------------------------------------
// Independent reference cluster construction (cross-product based, no reuse
// of the production plane code).
// ---------------------------------------------------------------------------

struct RefPlane {
	float n[3];
	float d;
};

void refNormalize(float v[3])
{
	const float len = std::sqrt(dot3(v, v));
	if (len > 0.0f) {
		const float inv = 1.0f / len;
		v[0] *= inv;
		v[1] *= inv;
		v[2] *= inv;
	}
}

// Build a side plane through the camera origin from two near-plane edge
// points, then orient it toward an independently supplied interior point of
// the same tile (the tile's near-plane center). The orientation never assumes
// the tile contains the view axis, so corner tiles stay correct.
void refSidePlane(
	float xa, float ya, float xb, float yb, float n,
	float interiorX, float interiorY, RefPlane &out)
{
	const float pa[3] = {xa, ya, -n};
	const float pb[3] = {xb, yb, -n};
	out.n[0] = pa[1] * pb[2] - pa[2] * pb[1];
	out.n[1] = pa[2] * pb[0] - pa[0] * pb[2];
	out.n[2] = pa[0] * pb[1] - pa[1] * pb[0];
	out.d = 0.0f;
	const float interior[3] = {interiorX, interiorY, -n};
	if (dot3(out.n, interior) < 0.0f) {
		out.n[0] = -out.n[0];
		out.n[1] = -out.n[1];
		out.n[2] = -out.n[2];
	}
	refNormalize(out.n);
}

void refClusterPlanes(
	const ClusterConfig &c,
	const ClusterGrid &grid,
	int tx, int ty, int k,
	RefPlane out[6])
{
	const int px0 = c.viewportX + tx * c.tileWidth;
	const int px1 = std::min(c.viewportX + (tx + 1) * c.tileWidth,
		c.viewportX + c.viewportWidth);
	const int py0 = c.viewportY + ty * c.tileHeight;
	const int py1 = std::min(c.viewportY + (ty + 1) * c.tileHeight,
		c.viewportY + c.viewportHeight);
	const float fx0 = float(px0 - c.viewportX) / float(c.viewportWidth);
	const float fx1 = float(px1 - c.viewportX) / float(c.viewportWidth);
	const float fy0 = float(py0 - c.viewportY) / float(c.viewportHeight);
	const float fy1 = float(py1 - c.viewportY) / float(c.viewportHeight);

	const float L = c.left + (c.right - c.left) * fx0;
	const float R = c.left + (c.right - c.left) * fx1;
	const float B = c.bottom + (c.top - c.bottom) * fy0;
	const float T = c.bottom + (c.top - c.bottom) * fy1;
	const float n = c.nearPlane;
	const float midX = 0.5f * (L + R);
	const float midY = 0.5f * (B + T);

	refSidePlane(L, B, L, T, n, midX, midY, out[0]); // left
	refSidePlane(R, T, R, B, n, midX, midY, out[1]); // right
	refSidePlane(R, B, L, B, n, midX, midY, out[2]); // bottom
	refSidePlane(L, T, R, T, n, midX, midY, out[3]); // top

	out[4] = {{0.0f, 0.0f, -1.0f}, -grid.depthBoundaries[k]};
	out[5] = {{0.0f, 0.0f, 1.0f}, grid.depthBoundaries[k + 1]};
}

bool refSphereHits(const PointLight &light, const RefPlane &plane)
{
	const float distance = dot3(plane.n, light.position) + plane.d;
	return distance + light.radius >= -kEps;
}

std::vector<int> referenceCandidates(
	const ClusterConfig &c,
	const ClusterGrid &grid,
	const std::vector<PointLight> &lights,
	int tx, int ty, int k)
{
	RefPlane planes[6];
	refClusterPlanes(c, grid, tx, ty, k, planes);

	std::vector<int> result;
	for (int li = 0; li < (int)lights.size(); ++li) {
		bool inside = true;
		for (int p = 0; p < 6; ++p) {
			if (!refSphereHits(lights[li], planes[p])) {
				inside = false;
				break;
			}
		}
		if (inside)
			result.push_back(li);
	}
	return result;
}

// Reference lighting: every valid pixel against every light independently.
// Returns {1, 0, 0} on a full match, otherwise {-1, x, y} of the first mismatch.
std::array<float, 3> referenceShade(
	const ClusteredLighting &context,
	const std::vector<PointLight> &lights,
	const std::array<float, 3> &ambient,
	const std::vector<std::array<float, 3>> &image)
{
	const ClusterConfig &c = context.config();
	for (int ly = 0; ly < c.viewportHeight; ++ly) {
		for (int lx = 0; lx < c.viewportWidth; ++lx) {
			const int wx = lx + c.viewportX;
			const int wy = ly + c.viewportY;
			if (!context.isValid(wx, wy))
				continue;

			const float *P = context.viewPosition(wx, wy);
			const float *N = context.normal(wx, wy);
			const float *albedo = context.albedo(wx, wy);

			std::array<float, 3> expected = {{
				ambient[0] * albedo[0],
				ambient[1] * albedo[1],
				ambient[2] * albedo[2]
			}};

			for (const PointLight &light : lights) {
				float V[3];
				sub3(light.position, P, V);
				const float dist2 = dot3(V, V);
				const float dist = std::sqrt(dist2);
				if (!(dist > 0.0f) || !(dist < light.radius))
					continue;
				const float invDist = 1.0f / dist;
				const float dir[3] = {V[0] * invDist, V[1] * invDist, V[2] * invDist};
				const float ndl = std::max(0.0f, dot3(N, dir));
				const float falloff = 1.0f - dist2 / (light.radius * light.radius);
				const float attenuation = falloff * falloff / std::max(dist2, 1e-6f);
				for (int ch = 0; ch < 3; ++ch)
					expected[ch] += albedo[ch] * light.intensity[ch] * ndl * attenuation;
			}

			const auto &got = image[ly * c.viewportWidth + lx];
			if (!closeVec(expected, got, 1e-5f))
				return {{-1.0f, float(lx), float(ly)}};
		}
	}
	return {{1.0f, 0.0f, 0.0f}};
}

PointLight makeLight(float x, float y, float depth, float radius,
	float r, float g, float b)
{
	PointLight light;
	light.position[0] = x;
	light.position[1] = y;
	light.position[2] = -depth;
	light.radius = radius;
	light.intensity[0] = r;
	light.intensity[1] = g;
	light.intensity[2] = b;
	return light;
}

ClusterConfig baseConfig()
{
	ClusterConfig c;
	c.nearPlane = 1.0f;
	c.farPlane = 16.0f;
	c.left = -1.0f;
	c.right = 1.0f;
	c.bottom = -1.0f;
	c.top = 1.0f;
	c.viewportX = 0;
	c.viewportY = 0;
	c.viewportWidth = 16;
	c.viewportHeight = 16;
	c.tileWidth = 8;
	c.tileHeight = 8;
	c.depthLayers = 4;
	return c;
}

// ---------------------------------------------------------------------------
// capture group
// ---------------------------------------------------------------------------

int captureQuadModes()
{
	const ClusterConfig config = baseConfig();
	const float normal[3] = {0.0f, 0.0f, -1.0f};
	const float albedo[3] = {0.25f, 0.5f, 0.75f};

	std::vector<std::vector<std::uint8_t>> valids(3);
	std::vector<std::vector<float>> depths(3);

	for (int mode = 0; mode < 3; ++mode) {
		ClusteredLighting context = makeContext(config, RasterMode(mode));
		std::vector<GBufferVertex> verts;
		std::vector<int> indices;
		makeQuad(config, -0.5f, -0.5f, 0.5f, 0.5f, 4.0f, normal, albedo, verts, indices);
		drawScene(context, verts, indices);

		int validCount = 0;
		std::vector<std::uint8_t> valid(config.viewportWidth * config.viewportHeight, 0);
		std::vector<float> depth(config.viewportWidth * config.viewportHeight, 0.0f);
		for (int y = 0; y < config.viewportHeight; ++y) {
			for (int x = 0; x < config.viewportWidth; ++x) {
				const bool ok = context.isValid(x, y);
				valid[y * config.viewportWidth + x] = ok ? 1 : 0;
				depth[y * config.viewportWidth + x] = context.depth(x, y);
				if (!ok)
					continue;
				++validCount;

				const float *P = context.viewPosition(x, y);
				const float *N = context.normal(x, y);
				const float *A = context.albedo(x, y);

				TASK_CHECK(P[2] < 0.0f);
				TASK_CHECK(std::fabs(P[2] + 4.0f) < 1e-4f);
				TASK_CHECK(std::fabs(N[2] + 1.0f) < 1e-5f);
				TASK_CHECK(std::fabs(N[0]) < 1e-5f && std::fabs(N[1]) < 1e-5f);
				TASK_CHECK(std::fabs(A[0] - 0.25f) < 1e-5f);
				TASK_CHECK(std::fabs(A[1] - 0.5f) < 1e-5f);
				TASK_CHECK(std::fabs(A[2] - 0.75f) < 1e-5f);
				TASK_CHECK(context.depth(x, y) >= 0.0f && context.depth(x, y) <= 1.0f);
				TASK_CHECK(std::fabs(context.depth(x, y) - expectedWindowDepth(config, 4.0f)) < 1e-5f);
			}
		}
		TASK_CHECK(validCount > 10);
		// Background stays invalid.
		TASK_CHECK(!context.isValid(0, 0));
		valids[mode] = std::move(valid);
		depths[mode] = std::move(depth);
	}

	// All three rasterization modes produce the same pixel results.
	for (int mode = 1; mode < 3; ++mode) {
		TASK_CHECK(valids[0] == valids[mode]);
		for (size_t i = 0; i < depths[0].size(); ++i)
			TASK_CHECK(std::fabs(depths[0][i] - depths[mode][i]) < 1e-6f
				|| (!valids[0][i] && !valids[mode][i]));
	}
	return 0;
}

int captureOcclusion()
{
	const ClusterConfig config = baseConfig();
	const float normal[3] = {0.0f, 0.0f, -1.0f};
	const float backAlbedo[3] = {0.1f, 0.1f, 0.1f};
	const float frontAlbedo[3] = {0.9f, 0.9f, 0.9f};

	for (int mode = 0; mode < 3; ++mode) {
		ClusteredLighting context = makeContext(config, RasterMode(mode));
		std::vector<GBufferVertex> verts;
		std::vector<int> indices;
		makeQuad(config, -0.9f, -0.9f, 0.9f, 0.9f, 8.0f, normal, backAlbedo, verts, indices);
		makeQuad(config, -0.4f, -0.4f, 0.4f, 0.4f, 3.0f, normal, frontAlbedo, verts, indices);
		drawScene(context, verts, indices);

		const int cx = config.viewportWidth / 2;
		const int cy = config.viewportHeight / 2;
		TASK_CHECK(context.isValid(cx, cy));
		TASK_CHECK(std::fabs(context.albedo(cx, cy)[0] - 0.9f) < 1e-5f);
		TASK_CHECK(std::fabs(context.depth(cx, cy) - expectedWindowDepth(config, 3.0f)) < 1e-5f);

		// A corner only the back quad covers.
		TASK_CHECK(context.isValid(1, 1));
		TASK_CHECK(std::fabs(context.albedo(1, 1)[0] - 0.1f) < 1e-5f);
		TASK_CHECK(std::fabs(context.depth(1, 1) - expectedWindowDepth(config, 8.0f)) < 1e-5f);
	}
	return 0;
}

bool discardLeftHalf(int x, int, const float[3], const float[3], const float[3], float)
{
	return x < 8;
}

int captureDiscard()
{
	const ClusterConfig config = baseConfig();
	const float normal[3] = {0.0f, 0.0f, -1.0f};
	const float backAlbedo[3] = {0.2f, 0.3f, 0.4f};
	const float frontAlbedo[3] = {0.8f, 0.7f, 0.6f};

	ClusteredLighting context = makeContext(config, RasterMode::Span);

	std::vector<GBufferVertex> backVerts;
	std::vector<int> backIndices;
	makeQuad(config, -0.9f, -0.9f, 0.9f, 0.9f, 8.0f, normal, backAlbedo, backVerts, backIndices);
	std::vector<GBufferVertex> frontVerts;
	std::vector<int> frontIndices;
	makeQuad(config, -0.9f, -0.9f, 0.9f, 0.9f, 3.0f, normal, frontAlbedo, frontVerts, frontIndices);

	// Back surface is drawn without a discard callback.
	drawScene(context, backVerts, backIndices);
	// Only the front surface discards its left-half fragments.
	context.setDiscardFunction(discardLeftHalf);
	drawScene(context, frontVerts, frontIndices);

	for (int y = 2; y < 14; ++y) {
		// Discarded front fragments do not write geometry or depth: the back
		// quad remains visible on the left half.
		TASK_CHECK(context.isValid(3, y));
		TASK_CHECK(std::fabs(context.albedo(3, y)[0] - 0.2f) < 1e-5f);
		TASK_CHECK(std::fabs(context.depth(3, y) - expectedWindowDepth(config, 8.0f)) < 1e-5f);
		// Right half shows the surviving front quad.
		TASK_CHECK(context.isValid(12, y));
		TASK_CHECK(std::fabs(context.albedo(12, y)[0] - 0.8f) < 1e-5f);
		TASK_CHECK(std::fabs(context.depth(12, y) - expectedWindowDepth(config, 3.0f)) < 1e-5f);
	}
	return 0;
}

// Non-equal w: triangle vertices at three different depths; compare against
// an independently evaluated perspective-correct barycentric reference.
int capturePerspectiveW()
{
	ClusterConfig config = baseConfig();
	config.viewportWidth = 32;
	config.viewportHeight = 32;

	struct VSpec {
		float x, y, depth;
		float albedo[3];
	};
	const VSpec specs[3] = {
		{-1.5f, 1.2f, 3.0f, {1.0f, 0.0f, 0.0f}},
		{1.8f, 1.0f, 6.0f, {0.0f, 1.0f, 0.0f}},
		{0.0f, -1.5f, 12.0f, {0.0f, 0.0f, 1.0f}}
	};
	const float normal[3] = {0.0f, 0.0f, -1.0f};

	std::vector<GBufferVertex> verts;
	for (const VSpec &s : specs)
		verts.push_back(makeVertex(config, s.x, s.y, s.depth, normal, s.albedo));
	const std::vector<int> indices = {0, 1, 2};

	for (int mode = 0; mode < 3; ++mode) {
		ClusteredLighting context = makeContext(config, RasterMode(mode));
		drawScene(context, verts, indices);

		// Screen positions after the viewport transform.
		float sx[3], sy[3], sw[3];
		for (int i = 0; i < 3; ++i) {
			sw[i] = verts[i].clip[3];
			sx[i] = config.viewportWidth * 0.5f * (verts[i].clip[0] / sw[i])
				+ config.viewportWidth * 0.5f;
			sy[i] = config.viewportHeight * 0.5f * -(verts[i].clip[1] / sw[i])
				+ config.viewportHeight * 0.5f;
		}

		int compared = 0;
		for (int py = 1; py < 31 && compared < 8; ++py) {
			for (int px = 1; px < 31 && compared < 8; ++px) {
				if (!context.isValid(px, py))
					continue;
				const float fx = px + 0.5f;
				const float fy = py + 0.5f;

				const float denom = (sy[1] - sy[2]) * (sx[0] - sx[2])
					+ (sx[2] - sx[1]) * (sy[0] - sy[2]);
				const float b0 = ((sy[1] - sy[2]) * (fx - sx[2])
					+ (sx[2] - sx[1]) * (fy - sy[2])) / denom;
				const float b1 = ((sy[2] - sy[0]) * (fx - sx[2])
					+ (sx[0] - sx[2]) * (fy - sy[2])) / denom;
				const float b2 = 1.0f - b0 - b1;
				// Only strictly interior samples avoid rasterizer edge ties.
				if (!(b0 > 1e-4f && b1 > 1e-4f && b2 > 1e-4f))
					continue;

				const float w0 = b0 / sw[0], w1 = b1 / sw[1], w2 = b2 / sw[2];
				const float wsum = w0 + w1 + w2;
				for (int ch = 0; ch < 3; ++ch) {
					const float expected =
						(w0 * specs[0].albedo[ch] + w1 * specs[1].albedo[ch]
							+ w2 * specs[2].albedo[ch]) / wsum;
					TASK_CHECK(std::fabs(context.albedo(px, py)[ch] - expected) < 2e-5f);
				}
				++compared;
			}
		}
		TASK_CHECK(compared == 8);
	}
	return 0;
}

int captureDepthPrecedence()
{
	const ClusterConfig config = baseConfig();
	const float normal[3] = {0.0f, 0.0f, -1.0f};
	const float red[3] = {0.8f, 0.1f, 0.1f};
	const float green[3] = {0.1f, 0.8f, 0.1f};
	const float blue[3] = {0.1f, 0.1f, 0.8f};

	ClusteredLighting context = makeContext(config, RasterMode::Span);

	// Closer surface drawn FIRST: a later, farther surface must not replace it.
	std::vector<GBufferVertex> closeVerts;
	std::vector<int> closeIndices;
	makeQuad(config, -0.4f, -0.4f, 0.4f, 0.4f, 3.0f, normal, blue, closeVerts, closeIndices);
	drawScene(context, closeVerts, closeIndices);

	// Farther surface covering everything: rejected at the close region.
	std::vector<GBufferVertex> farVertsA;
	std::vector<int> farIndicesA;
	makeQuad(config, -0.9f, -0.9f, 0.9f, 0.9f, 6.0f, normal, red, farVertsA, farIndicesA);
	drawScene(context, farVertsA, farIndicesA);

	// Same depth as the red surface, drawn later: exact tie goes to the later
	// fragment, so green replaces red but never the closer blue center.
	std::vector<GBufferVertex> farVertsB;
	std::vector<int> farIndicesB;
	makeQuad(config, -0.9f, -0.9f, 0.9f, 0.9f, 6.0f, normal, green, farVertsB, farIndicesB);
	drawScene(context, farVertsB, farIndicesB);

	const int cx = config.viewportWidth / 2;
	const int cy = config.viewportHeight / 2;
	// Smaller depth wins regardless of draw order.
	TASK_CHECK(std::fabs(context.albedo(cx, cy)[2] - 0.8f) < 1e-5f);
	TASK_CHECK(std::fabs(context.depth(cx, cy) - expectedWindowDepth(config, 3.0f)) < 1e-5f);
	// Equal depth: the later fragment wins.
	TASK_CHECK(std::fabs(context.albedo(1, 1)[1] - 0.8f) < 1e-5f);
	TASK_CHECK(std::fabs(context.depth(1, 1) - expectedWindowDepth(config, 6.0f)) < 1e-5f);
	return 0;
}

// ---------------------------------------------------------------------------
// clusters group
// ---------------------------------------------------------------------------

int clustersGridBounds()
{
	ClusterConfig c = baseConfig();
	c.viewportWidth = 20;
	c.viewportHeight = 13;
	c.tileWidth = 8;
	c.tileHeight = 5;
	c.depthLayers = 4;

	ClusteredLighting context(c);
	const ClusterGrid &grid = context.grid();

	TASK_CHECK(grid.tilesX == 3);
	TASK_CHECK(grid.tilesY == 3);
	TASK_CHECK(grid.layers == 4);
	TASK_CHECK((int)grid.depthBoundaries.size() == 5);

	// Non-square final tiles are clipped to the viewport.
	const Cluster &last = context.clusters()[context.clusterIndex(2, 2, 0)];
	TASK_CHECK(last.pixelX0 == 16 && last.pixelX1 == 20);
	TASK_CHECK(last.pixelY0 == 10 && last.pixelY1 == 13);
	const Cluster &first = context.clusters()[context.clusterIndex(0, 0, 0)];
	TASK_CHECK(first.pixelX0 == 0 && first.pixelX1 == 8);
	TASK_CHECK(first.pixelY0 == 0 && first.pixelY1 == 5);

	// Logarithmic depth boundaries near*(far/near)^(k/K).
	for (int k = 0; k <= 4; ++k) {
		const float expected = c.nearPlane
			* std::pow(c.farPlane / c.nearPlane, float(k) / 4.0f);
		TASK_CHECK(std::fabs(grid.depthBoundaries[k] - expected) < 1e-5f);
	}
	TASK_CHECK(std::fabs(grid.depthBoundaries[0] - c.nearPlane) < 1e-6f);
	TASK_CHECK(std::fabs(grid.depthBoundaries[4] - c.farPlane) < 1e-6f);

	// Depth layer placement. Tiny spheres keep a bounded layer span, so the
	// near/far boundary conventions can be checked at the central tile.
	std::vector<PointLight> lights;
	lights.push_back(makeLight(0.0f, 0.0f, c.farPlane, 0.01f, 1.0f, 1.0f, 1.0f)); // on far
	lights.push_back(makeLight(0.0f, 0.0f, c.nearPlane, 0.01f, 1.0f, 0.0f, 0.0f)); // on near
	lights.push_back(makeLight(0.0f, 0.0f, 2.0f, 0.01f, 0.0f, 1.0f, 0.0f));        // on internal boundary
	context.setLights(lights);
	context.buildClusters();

	const int centerTx = grid.tilesX / 2;
	const int centerTy = grid.tilesY / 2;
	auto centerLights = [&](int layer) -> const std::vector<int> & {
		return context.clusters()[context.clusterIndex(centerTx, centerTy, layer)].lights;
	};
	auto has = [](const std::vector<int> &list, int id) {
		return std::find(list.begin(), list.end(), id) != list.end();
	};

	// Far plane belongs to the last layer and is not dropped.
	TASK_CHECK(has(centerLights(grid.layers - 1), 0));
	for (int k = 0; k < grid.layers - 1; ++k)
		TASK_CHECK(!has(centerLights(k), 0));

	// Near plane belongs to the first layer.
	TASK_CHECK(has(centerLights(0), 1));
	for (int k = 1; k < grid.layers; ++k)
		TASK_CHECK(!has(centerLights(k), 1));

	// A sphere straddling an internal boundary is kept by both adjacent layers
	// (closed half spaces) and reaches no other layer.
	TASK_CHECK(has(centerLights(0), 2));
	TASK_CHECK(has(centerLights(1), 2));
	for (int k = 2; k < grid.layers; ++k)
		TASK_CHECK(!has(centerLights(k), 2));
	return 0;
}

int clustersExhaustive()
{
	const std::array<ClusterConfig, 2> configs = [] {
		ClusterConfig symmetric = baseConfig();
		symmetric.viewportX = 3;
		symmetric.viewportY = 2;
		symmetric.viewportWidth = 17;
		symmetric.viewportHeight = 11;
		symmetric.tileWidth = 5;
		symmetric.tileHeight = 4;

		ClusterConfig asymmetric = baseConfig();
		asymmetric.left = -0.5f;
		asymmetric.right = 1.5f;
		asymmetric.bottom = -1.2f;
		asymmetric.top = 0.8f;
		asymmetric.viewportX = 4;
		asymmetric.viewportY = 1;
		asymmetric.viewportWidth = 19;
		asymmetric.viewportHeight = 13;
		asymmetric.tileWidth = 6;
		asymmetric.tileHeight = 5;
		return std::array<ClusterConfig, 2>{symmetric, asymmetric};
	}();

	std::vector<PointLight> lights;
	lights.push_back(makeLight(0.0f, 0.0f, 4.0f, 0.5f, 1.0f, 1.0f, 1.0f));
	lights.push_back(makeLight(0.7f, -0.6f, 2.2f, 0.3f, 0.5f, 0.2f, 0.1f));
	lights.push_back(makeLight(-0.8f, 0.8f, 12.0f, 1.5f, 0.2f, 0.7f, 0.9f));
	lights.push_back(makeLight(0.0f, 0.0f, 1.5f, 1.0f, 1.0f, 0.0f, 0.0f));   // crosses near plane
	lights.push_back(makeLight(0.0f, 0.0f, -1.0f, 3.0f, 0.0f, 1.0f, 0.0f));  // center behind camera, reaches view
	lights.push_back(makeLight(0.3f, 0.3f, 8.0f, 0.05f, 0.1f, 0.1f, 0.1f));

	for (const ClusterConfig &config : configs) {
		ClusteredLighting context(config);
		context.setLights(lights);
		context.buildClusters();
		const ClusterGrid &grid = context.grid();

		for (int ty = 0; ty < grid.tilesY; ++ty) {
			for (int tx = 0; tx < grid.tilesX; ++tx) {
				for (int k = 0; k < grid.layers; ++k) {
					const std::vector<int> expected =
						referenceCandidates(config, grid, lights, tx, ty, k);
					const std::vector<int> &got =
						context.clusters()[context.clusterIndex(tx, ty, k)].lights;
					TASK_CHECK(got == expected);
					// Sorted, unique, input order.
					TASK_CHECK(std::is_sorted(got.begin(), got.end()));
					TASK_CHECK(std::adjacent_find(got.begin(), got.end()) == got.end());
				}
			}
		}
	}
	return 0;
}

int clustersTangentAndCrossings()
{
	ClusterConfig c = baseConfig();
	ClusteredLighting context(c);
	const ClusterGrid &grid = context.grid();

	// Side tangency: sphere tangent to the x=0 tile boundary at depth 4.
	// Boundary slope is 0, so the plane is x=0; offset center by exactly r.
	const float depth = 4.0f;
	const float radius = 0.2f;
	std::vector<PointLight> lights;
	lights.push_back(makeLight(radius, 0.0f, depth, radius, 1.0f, 1.0f, 1.0f));

	// Near-plane crossing: sphere straddles d = near.
	lights.push_back(makeLight(0.0f, 0.0f, 1.5f, 1.0f, 1.0f, 0.0f, 0.0f));
	// Eye-plane crossing: center behind the camera, sphere enters the frustum.
	lights.push_back(makeLight(0.0f, 0.0f, -1.0f, 3.0f, 0.0f, 1.0f, 0.0f));
	// Fully behind the camera, no part in front.
	lights.push_back(makeLight(0.0f, 0.0f, -10.0f, 2.0f, 0.0f, 0.0f, 1.0f));
	// Tiny light wholly inside one tile/layer.
	lights.push_back(makeLight(0.6f, 0.6f, 5.0f, 0.1f, 0.3f, 0.3f, 0.3f));

	context.setLights(lights);
	context.buildClusters();

	int tangentHits = 0;
	int nearHits = 0;
	int eyeHits = 0;
	int behindHits = 0;
	for (int ty = 0; ty < grid.tilesY; ++ty) {
		for (int tx = 0; tx < grid.tilesX; ++tx) {
			for (int k = 0; k < grid.layers; ++k) {
				const std::vector<int> expected =
					referenceCandidates(c, grid, lights, tx, ty, k);
				const std::vector<int> &got =
					context.clusters()[context.clusterIndex(tx, ty, k)].lights;
				TASK_CHECK(got == expected);

				const auto has = [&](int id) {
					return std::find(got.begin(), got.end(), id) != got.end();
				};
				if (has(0))
					++tangentHits;
				if (has(1))
					++nearHits;
				if (has(2))
					++eyeHits;
				if (has(3))
					++behindHits;
			}
		}
	}

	// Tangent sphere is kept by the closed half spaces on both sides.
	TASK_CHECK(tangentHits >= 2);
	// Near and eye crossings produce candidates somewhere in the grid.
	TASK_CHECK(nearHits > 0);
	TASK_CHECK(eyeHits > 0);
	// A sphere entirely behind the camera is rejected everywhere.
	TASK_CHECK(behindHits == 0);
	return 0;
}

int clustersManyLights()
{
	const ClusterConfig config = baseConfig();
	ClusteredLighting context(config);

	const int lightCount = 250; // well beyond any small fixed per-cluster list
	std::vector<PointLight> lights;
	for (int i = 0; i < lightCount; ++i) {
		const float shade = 0.1f + 0.001f * float(i);
		lights.push_back(makeLight(0.0f, 0.0f, 4.0f, 0.4f, shade, shade, shade));
	}
	context.setLights(lights);
	context.buildClusters();

	const ClusterGrid &grid = context.grid();
	int populated = 0;
	for (int ty = 0; ty < grid.tilesY; ++ty) {
		for (int tx = 0; tx < grid.tilesX; ++tx) {
			for (int k = 0; k < grid.layers; ++k) {
				const std::vector<int> expected =
					referenceCandidates(config, grid, lights, tx, ty, k);
				const std::vector<int> &got =
					context.clusters()[context.clusterIndex(tx, ty, k)].lights;
				if (expected.empty())
					continue;
				++populated;
				TASK_CHECK(got.size() == (size_t)lightCount);
				TASK_CHECK(got == expected);
				for (int i = 0; i < lightCount; ++i)
					TASK_CHECK(got[i] == i); // input order, no duplicates or drops
			}
		}
	}
	TASK_CHECK(populated > 0);
	return 0;
}

int clustersBuildStats()
{
	const ClusterConfig config = baseConfig();
	ClusteredLighting context(config);
	const size_t clustersTotal =
		(size_t)context.grid().tilesX * context.grid().tilesY * context.grid().layers;

	// One small local light that crosses neither the eye nor the near plane, so
	// it exercises the bounded tangent-cone path. Its span must be a small
	// fraction of the grid; a wrongly-always-full tile range would fail here.
	std::vector<PointLight> one;
	one.push_back(makeLight(0.6f, 0.6f, 4.0f, 0.08f, 1.0f, 1.0f, 1.0f));
	context.setLights(one);
	context.buildClusters();
	const std::uint64_t singleLightTests = context.stats().clusterSphereTests;
	TASK_CHECK(singleLightTests > 0);
	TASK_CHECK(singleLightTests <= 4);
	TASK_CHECK(singleLightTests < clustersTotal);

	// Twelve identical local lights: work scales with the real per-light span,
	// not the cartesian product, and the counters are exact.
	std::vector<PointLight> lights;
	for (int i = 0; i < 12; ++i)
		lights.push_back(makeLight(0.6f, 0.6f, 4.0f, 0.08f, 1.0f, 1.0f, 1.0f));
	context.setLights(lights);
	context.buildClusters();

	const size_t cartesian = clustersTotal * lights.size();
	TASK_CHECK(context.stats().clusterSphereTests > 0);
	TASK_CHECK(context.stats().clusterSphereTests < cartesian);
	TASK_CHECK(context.stats().clusterSphereTests
		== singleLightTests * (std::uint64_t)lights.size());

	// Rebuilding resets the counter instead of accumulating stale work.
	const std::uint64_t first = context.stats().clusterSphereTests;
	context.buildClusters();
	TASK_CHECK(context.stats().clusterSphereTests == first);
	return 0;
}

// Degenerate control 1: project the sphere CENTER only and drop anything that
// does not project. Must compile/run and disagree with the conservative build.
std::vector<std::vector<int>> degenerateCenterOnly(
	const ClusterConfig &c,
	const ClusterGrid &grid,
	const std::vector<PointLight> &lights)
{
	const size_t clusterCount = (size_t)grid.tilesX * grid.tilesY * grid.layers;
	std::vector<std::vector<int>> lists(clusterCount);
	for (int li = 0; li < (int)lights.size(); ++li) {
		const float d = -lights[li].position[2];
		if (d <= 0.0f)
			continue; // bug: behind-camera light is lost
		const float fx = (c.nearPlane * lights[li].position[0] / d - c.left)
			/ (c.right - c.left);
		const float fy = (c.nearPlane * lights[li].position[1] / d - c.bottom)
			/ (c.top - c.bottom);
		if (fx < 0.0f || fx > 1.0f || fy < 0.0f || fy > 1.0f)
			continue;
		const int tx = std::min(int(fx * c.viewportWidth / c.tileWidth), grid.tilesX - 1);
		const int ty = std::min(int(fy * c.viewportHeight / c.tileHeight), grid.tilesY - 1);
		const float f = std::log(d / c.nearPlane) / std::log(c.farPlane / c.nearPlane);
		const int k = std::max(0, std::min(int(f * c.depthLayers), grid.layers - 1));
		const size_t idx = ((size_t)ty * grid.tilesX + tx) * grid.layers + k;
		lists[idx].push_back(li);
	}
	return lists;
}

// Degenerate control 2: treat window depth z (linear in NDC) as if it were the
// logarithmic layer parameter.
int degenerateWindowZLayer(const ClusterConfig &c, float depth)
{
	const float n = c.nearPlane;
	const float f = c.farPlane;
	const float z = (f + n) / (f - n) - 2.0f * f * n / ((f - n) * depth);
	return std::max(0, std::min(int(std::floor(z * c.depthLayers)), c.depthLayers - 1));
}

int degenerateControls()
{
	const ClusterConfig c = baseConfig();
	ClusteredLighting context(c);

	std::vector<PointLight> lights;
	lights.push_back(makeLight(0.0f, 0.0f, -1.0f, 3.0f, 1.0f, 1.0f, 1.0f)); // eye crossing
	lights.push_back(makeLight(0.0f, 0.0f, 1.5f, 1.0f, 1.0f, 1.0f, 1.0f));  // near crossing
	lights.push_back(makeLight(0.0f, 0.0f, 2.2f, 0.05f, 1.0f, 1.0f, 1.0f)); // layer mismatch probe
	context.setLights(lights);
	context.buildClusters();

	const auto bad = degenerateCenterOnly(c, context.grid(), lights);
	TASK_CHECK(bad.size() == context.clusters().size());

	bool centerOnlyDiffers = false;
	for (size_t i = 0; i < context.clusters().size(); ++i) {
		if (bad[i] != context.clusters()[i].lights)
			centerOnlyDiffers = true;
	}
	TASK_CHECK(centerOnlyDiffers);

	// The eye-crossing light must be missing entirely from the center-only build.
	bool eyeLightKept = false;
	for (const auto &list : bad) {
		if (std::find(list.begin(), list.end(), 0) != list.end())
			eyeLightKept = true;
	}
	TASK_CHECK(!eyeLightKept);

	// Window-z layering disagrees with the logarithmic layers at depth 2.2:
	// correct layer is 1 (boundaries 1, 2, 4, 8, 16) while the linear map
	// yields layer 0.
	const int correctLayer = [&] {
		const float f = std::log(2.2f / c.nearPlane) / std::log(c.farPlane / c.nearPlane);
		return std::max(0, std::min(int(f * c.depthLayers), c.depthLayers - 1));
	}();
	TASK_CHECK(correctLayer == 1);
	TASK_CHECK(degenerateWindowZLayer(c, 2.2f) != correctLayer);
	return 0;
}

// ---------------------------------------------------------------------------
// lighting group
// ---------------------------------------------------------------------------

std::vector<PointLight> lightingRig()
{
	// The lit quads face the camera with normal (0, 0, -1), so a positive
	// N dot V requires lights behind the surface (greater view depth).
	std::vector<PointLight> lights;
	lights.push_back(makeLight(0.3f, 0.3f, 5.5f, 3.0f, 2.0f, 0.5f, 0.25f));
	lights.push_back(makeLight(-0.6f, -0.4f, 6.0f, 2.5f, 0.4f, 1.5f, 0.8f));
	lights.push_back(makeLight(0.0f, 0.8f, 5.0f, 1.5f, 0.7f, 0.7f, 2.0f));
	lights.push_back(makeLight(0.2f, 0.2f, 4.5f, 0.8f, 30.0f, 30.0f, 30.0f)); // bright, finite radius
	return lights;
}

int lightingReference()
{
	const ClusterConfig config = baseConfig();
	const float normal[3] = {0.0f, 0.0f, -1.0f};
	const float albedo[3] = {0.6f, 0.4f, 0.2f};
	const std::array<float, 3> ambient = {{0.05f, 0.07f, 0.09f}};
	const std::vector<PointLight> lights = lightingRig();

	ClusteredLighting context(config);
	context.setAmbient(ambient[0], ambient[1], ambient[2]);
	context.beginGeometry();

	std::vector<GBufferVertex> verts;
	std::vector<int> indices;
	makeQuad(config, -0.8f, -0.8f, 0.8f, 0.8f, 4.0f, normal, albedo, verts, indices);
	drawScene(context, verts, indices);

	context.setLights(lights);
	context.buildClusters();
	const std::vector<std::array<float, 3>> image = context.shade();

	TASK_CHECK(image.size() == (size_t)(config.viewportWidth * config.viewportHeight));
	const std::array<float, 3> status = referenceShade(context, lights, ambient, image);
	TASK_CHECK(status[0] == 1.0f);

	// Invalid background pixels are zero.
	TASK_CHECK(!context.isValid(0, 15));
	const auto &bg = image[15 * config.viewportWidth];
	TASK_CHECK(bg[0] == 0.0f && bg[1] == 0.0f && bg[2] == 0.0f);

	// Values are kept as float, never clamped to the [0, 1] display range.
	bool foundAboveOne = false;
	for (const auto &pixel : image) {
		if (pixel[0] > 1.0f || pixel[1] > 1.0f || pixel[2] > 1.0f)
			foundAboveOne = true;
	}
	TASK_CHECK(foundAboveOne);

	// Ambient alone on an unlit pixel: light rig does not cover the far corner
	// of the quad strongly, so verify the ambient term directly on one pixel.
	const int px = 2, py = 2;
	if (context.isValid(px, py)) {
		std::array<float, 3> expectedAmbient = {{
			ambient[0] * albedo[0], ambient[1] * albedo[1], ambient[2] * albedo[2]}};
		const auto &got = image[py * config.viewportWidth + px];
		TASK_CHECK(got[0] >= expectedAmbient[0] - 1e-6f);
	}
	return 0;
}

int lightingSceneModes()
{
	const ClusterConfig config = baseConfig();
	const float normal[3] = {0.0f, 0.0f, -1.0f};
	const float backAlbedo[3] = {0.3f, 0.5f, 0.7f};
	const float frontAlbedo[3] = {0.9f, 0.2f, 0.4f};
	const std::array<float, 3> ambient = {{0.03f, 0.03f, 0.03f}};
	const std::vector<PointLight> lights = lightingRig();

	std::vector<std::vector<std::array<float, 3>>> images(3);
	std::vector<std::vector<std::uint8_t>> validity(3);

	for (int mode = 0; mode < 3; ++mode) {
		ClusteredLighting context(config);
		context.setAmbient(ambient[0], ambient[1], ambient[2]);
		context.setRasterMode(RasterMode(mode));
		context.beginGeometry();

		std::vector<GBufferVertex> verts;
		std::vector<int> indices;
		// Non-equal w across the quads via different depths, real drawElements.
		makeQuad(config, -0.9f, -0.9f, 0.9f, 0.9f, 9.0f, normal, backAlbedo, verts, indices);
		drawScene(context, verts, indices);
		// The front surface discards its left half, exposing the back quad.
		context.setDiscardFunction(discardLeftHalf);
		std::vector<GBufferVertex> frontVerts;
		std::vector<int> frontIndices;
		makeQuad(config, -0.7f, -0.7f, 0.7f, 0.7f, 3.5f, normal, frontAlbedo, frontVerts, frontIndices);
		drawScene(context, frontVerts, frontIndices);

		context.setLights(lights);
		context.buildClusters();
		images[mode] = context.shade();

		const std::array<float, 3> status = referenceShade(context, lights, ambient, images[mode]);
		TASK_CHECK(status[0] == 1.0f);

		validity[mode].resize(images[mode].size(), 0);
		for (int ly = 0; ly < config.viewportHeight; ++ly) {
			for (int lx = 0; lx < config.viewportWidth; ++lx) {
				const int idx = ly * config.viewportWidth + lx;
				validity[mode][idx] = context.isValid(
					lx + config.viewportX, ly + config.viewportY) ? 1 : 0;
			}
		}
	}

	// The three rasterizers agree on shared interior pixels; only legacy edge
	// coverage differences are tolerated, and they must stay a small minority.
	int shared = 0;
	int spanPixels = 0;
	for (size_t i = 0; i < images[0].size(); ++i) {
		if (validity[0][i])
			++spanPixels;
		if (!validity[0][i] || !validity[1][i] || !validity[2][i])
			continue;
		++shared;
		TASK_CHECK(closeVec(images[0][i], images[1][i], 2e-5f));
		TASK_CHECK(closeVec(images[0][i], images[2][i], 2e-5f));
	}
	TASK_CHECK(shared > 0);
	TASK_CHECK(shared * 10 >= spanPixels * 9); // at least 90% overlap
	return 0;
}

int lightingStats()
{
	const ClusterConfig config = baseConfig();
	const float normal[3] = {0.0f, 0.0f, -1.0f};
	const float albedo[3] = {0.5f, 0.5f, 0.5f};

	ClusteredLighting context(config);
	context.beginGeometry();
	std::vector<GBufferVertex> verts;
	std::vector<int> indices;
	makeQuad(config, -0.9f, -0.9f, 0.9f, 0.9f, 4.0f, normal, albedo, verts, indices);
	drawScene(context, verts, indices);

	std::vector<PointLight> lights;
	for (int i = 0; i < 10; ++i)
		lights.push_back(makeLight(0.6f, 0.0f, 4.0f, 0.15f, 1.0f, 1.0f, 1.0f));
	context.setLights(lights);
	context.buildClusters();
	context.shade();

	std::uint64_t expectedEvaluations = 0;
	int validPixels = 0;
	for (int ly = 0; ly < config.viewportHeight; ++ly) {
		for (int lx = 0; lx < config.viewportWidth; ++lx) {
			const int wx = lx + config.viewportX;
			const int wy = ly + config.viewportY;
			if (!context.isValid(wx, wy))
				continue;
			++validPixels;
			const float *P = context.viewPosition(wx, wy);
			const int tx = lx / config.tileWidth;
			const int ty = ly / config.tileHeight;
			const float f = std::log(-P[2] / config.nearPlane)
				/ std::log(config.farPlane / config.nearPlane);
			const int k = std::max(0, std::min(int(f * config.depthLayers), config.depthLayers - 1));
			expectedEvaluations +=
				context.clusters()[context.clusterIndex(tx, ty, k)].lights.size();
		}
	}
	TASK_CHECK(validPixels > 0);
	TASK_CHECK(context.stats().lightingEvaluations == expectedEvaluations);
	TASK_CHECK(context.stats().lightingEvaluations < std::uint64_t(validPixels) * lights.size());

	// Counter resets on every shade.
	context.shade();
	TASK_CHECK(context.stats().lightingEvaluations == expectedEvaluations);
	return 0;
}

int lightingIsolation()
{
	const ClusterConfig config = baseConfig();
	const float normal[3] = {0.0f, 0.0f, -1.0f};
	const float albedoA[3] = {0.8f, 0.1f, 0.1f};
	const float albedoB[3] = {0.1f, 0.1f, 0.8f};
	const std::vector<PointLight> lights = lightingRig();

	ClusteredLighting ctxA(config);
	ClusteredLighting ctxB(config);
	ctxA.beginGeometry();
	ctxB.beginGeometry();

	std::vector<GBufferVertex> vertsA;
	std::vector<int> indicesA;
	makeQuad(config, -0.8f, -0.8f, 0.8f, 0.8f, 4.0f, normal, albedoA, vertsA, indicesA);
	drawScene(ctxA, vertsA, indicesA);

	std::vector<GBufferVertex> vertsB;
	std::vector<int> indicesB;
	makeQuad(config, -0.8f, -0.8f, 0.8f, 0.8f, 4.0f, normal, albedoB, vertsB, indicesB);
	drawScene(ctxB, vertsB, indicesB);

	ctxA.setLights(lights);
	ctxA.buildClusters();
	ctxB.setLights(lights);
	ctxB.buildClusters();

	const auto imageA1 = ctxA.shade();
	const auto imageB1 = ctxB.shade();
	const auto imageA2 = ctxA.shade(); // interleaved reuse keeps ctxA stable
	TASK_CHECK(imageA1 == imageA2);
	TASK_CHECK(imageA1 != imageB1);

	// Clearing leaves no previous geometry: the shaded image is all zero.
	ctxA.clear();
	const auto cleared = ctxA.shade();
	TASK_CHECK(cleared.size() == imageA1.size());
	for (const auto &pixel : cleared)
		TASK_CHECK(pixel[0] == 0.0f && pixel[1] == 0.0f && pixel[2] == 0.0f);

	// Resolution change reallocates everything; old pixels cannot linger.
	ClusterConfig smaller = config;
	smaller.viewportWidth = 8;
	smaller.viewportHeight = 8;
	ctxA.configure(smaller);
	ctxA.beginGeometry();
	drawScene(ctxA, vertsA, indicesA);
	ctxA.setLights(lights);
	ctxA.buildClusters();
	const auto resized = ctxA.shade();
	TASK_CHECK(resized.size() == 64u);
	TASK_CHECK(ctxA.isValid(4, 4));
	TASK_CHECK(!ctxA.isValid(12, 12));

	// Shrinking the light table invalidates every stale cluster entry.
	std::vector<PointLight> oneLight(lights.begin(), lights.begin() + 1);
	ctxA.setLights(oneLight);
	ctxA.buildClusters();
	for (const Cluster &cluster : ctxA.clusters()) {
		for (int id : cluster.lights)
			TASK_CHECK(id == 0);
	}
	return 0;
}

} // namespace

const task_case task_cases[] = {
	{"capture", "quad_modes", &captureQuadModes},
	{"capture", "occlusion", &captureOcclusion},
	{"capture", "discard", &captureDiscard},
	{"capture", "perspective_w", &capturePerspectiveW},
	{"capture", "depth_precedence", &captureDepthPrecedence},
	{"clusters", "grid_bounds", &clustersGridBounds},
	{"clusters", "exhaustive", &clustersExhaustive},
	{"clusters", "tangent_crossings", &clustersTangentAndCrossings},
	{"clusters", "many_lights", &clustersManyLights},
	{"clusters", "build_stats", &clustersBuildStats},
	{"clusters", "degenerate_controls", &degenerateControls},
	{"lighting", "reference", &lightingReference},
	{"lighting", "scene_modes", &lightingSceneModes},
	{"lighting", "stats", &lightingStats},
	{"lighting", "isolation", &lightingIsolation},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);