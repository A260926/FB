#include "Renderer.h"

#include "Scene.h"
#include "Drawing.h"
#include "Geometry.h"
#include "Light.h"
#include "Noise.h"
#include "Photon.h"
#include "Sky.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/glm.hpp>
#include <random>
#include <utility>
#include <vector>

// Signed area of triangle (a, b, p) x2 - the edge function used for barycentric tests.
static float edgeFunction(const CanvasPoint &a, const CanvasPoint &b, float px, float py) {
	return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}

// Sutherland-Hodgman clip a camera-space polygon against a z plane: keep the
// side where z < zBound (keepLess) or z > zBound. Used for the near/far planes
// (forward is -z, so "in front of near" is z < -near).
static std::vector<glm::vec3> clipCameraZ(const std::vector<glm::vec3> &poly, float zBound, bool keepLess) {
	std::vector<glm::vec3> out;
	for (size_t i = 0; i < poly.size(); i++) {
		const glm::vec3 &a = poly[i];
		const glm::vec3 &b = poly[(i + 1) % poly.size()];
		bool ai = keepLess ? a.z < zBound : a.z > zBound;
		bool bi = keepLess ? b.z < zBound : b.z > zBound;
		if (ai)
			out.push_back(a);
		if (ai != bi) {
			float t = (zBound - a.z) / (b.z - a.z);
			out.push_back(a + t * (b - a));
		}
	}
	return out;
}

// Clip a projected polygon against one screen edge (x or y, >= or <= bound),
// interpolating inverse depth so the new vertices stay perspective-correct.
static std::vector<CanvasPoint> clipScreenEdge(const std::vector<CanvasPoint> &poly, int axis, float bound,
                                               bool keepGreater) {
	std::vector<CanvasPoint> out;
	auto coord = [&](const CanvasPoint &p) { return axis == 0 ? p.x : p.y; };
	auto inside = [&](const CanvasPoint &p) { return keepGreater ? coord(p) >= bound : coord(p) <= bound; };
	for (size_t i = 0; i < poly.size(); i++) {
		const CanvasPoint &a = poly[i];
		const CanvasPoint &b = poly[(i + 1) % poly.size()];
		bool ai = inside(a), bi = inside(b);
		if (ai)
			out.push_back(a);
		if (ai != bi) {
			float t = (bound - coord(a)) / (coord(b) - coord(a));
			CanvasPoint p;
			p.x = a.x + t * (b.x - a.x);
			p.y = a.y + t * (b.y - a.y);
			float invDepth = 1.0f / a.depth + t * (1.0f / b.depth - 1.0f / a.depth);
			p.depth = 1.0f / invDepth;
			out.push_back(p);
		}
	}
	return out;
}

// Full frustum clipping: clip a triangle against the near and far planes in
// camera space, project, then clip against all four screen edges. Returns the
// clipped polygon fan-split into triangles (empty if fully outside).
static std::vector<std::array<CanvasPoint, 3>> clipAndProject(const ModelTriangle &tri, const Camera &camera, int W,
                                                              int H) {
	const float nearPlane = 0.1f;
	const float farPlane = 1000.0f;
	std::vector<glm::vec3> poly;
	poly.reserve(3);
	for (int i = 0; i < 3; i++)
		poly.push_back(camera.toCameraSpace(tri.vertices[i]));

	poly = clipCameraZ(poly, -nearPlane, true); // in front of the near plane
	if (poly.size() < 3)
		return {};
	poly = clipCameraZ(poly, -farPlane, false); // nearer than the far plane
	if (poly.size() < 3)
		return {};

	std::vector<CanvasPoint> proj;
	proj.reserve(poly.size());
	for (const glm::vec3 &v : poly)
		proj.push_back(camera.projectCameraPoint(v));

	proj = clipScreenEdge(proj, 0, 0.0f, true);                       // x >= 0
	proj = clipScreenEdge(proj, 0, static_cast<float>(W - 1), false); // x <= W-1
	proj = clipScreenEdge(proj, 1, 0.0f, true);                       // y >= 0
	proj = clipScreenEdge(proj, 1, static_cast<float>(H - 1), false); // y <= H-1
	if (proj.size() < 3)
		return {};

	std::vector<std::array<CanvasPoint, 3>> result;
	for (size_t i = 1; i + 1 < proj.size(); i++)
		result.push_back({proj[0], proj[i], proj[i + 1]});
	return result;
}

// Scanline fill of a projected triangle with an inverse-depth z-buffer.
static void fillTriangle(const CanvasPoint p[3], uint32_t colour, std::vector<float> &depthBuffer, int W, int H,
                         Canvas &canvas) {
	float area = edgeFunction(p[0], p[1], p[2].x, p[2].y);
	if (std::abs(area) < 1e-6f)
		return;
	int minX = std::max(0, static_cast<int>(std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
	int maxX = std::min(W - 1, static_cast<int>(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
	int minY = std::max(0, static_cast<int>(std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
	int maxY = std::min(H - 1, static_cast<int>(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
	float inv0 = 1.0f / p[0].depth;
	float inv1 = 1.0f / p[1].depth;
	float inv2 = 1.0f / p[2].depth;
	for (int y = minY; y <= maxY; y++) {
		for (int x = minX; x <= maxX; x++) {
			float px = x + 0.5f;
			float py = y + 0.5f;
			float w0 = edgeFunction(p[1], p[2], px, py) / area;
			float w1 = edgeFunction(p[2], p[0], px, py) / area;
			float w2 = edgeFunction(p[0], p[1], px, py) / area;
			if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
				continue;
			float invDepth = w0 * inv0 + w1 * inv1 + w2 * inv2;
			size_t idx = static_cast<size_t>(y) * W + x;
			if (invDepth > depthBuffer[idx]) {
				depthBuffer[idx] = invDepth;
				canvas.setPixelColour(x, y, colour);
			}
		}
	}
}

void renderWireframe(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas) {
	canvas.clearPixels();
	int W = static_cast<int>(canvas.width);
	int H = static_cast<int>(canvas.height);
	for (const ModelTriangle &tri : model) {
		for (const std::array<CanvasPoint, 3> &p : clipAndProject(tri, camera, W, H)) {
			drawLine(p[0], p[1], tri.colour, canvas);
			drawLine(p[1], p[2], tri.colour, canvas);
			drawLine(p[2], p[0], tri.colour, canvas);
		}
	}
}

void renderRasterised(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas,
                      bool backfaceCull) {
	canvas.clearPixels();
	int W = static_cast<int>(canvas.width);
	int H = static_cast<int>(canvas.height);
	std::vector<float> depthBuffer(static_cast<size_t>(W) * H, 0.0f); // inverse depth, 0 = far

	for (const ModelTriangle &tri : model) {
		for (const std::array<CanvasPoint, 3> &p : clipAndProject(tri, camera, W, H)) {
			// Backface culling: a back-facing triangle projects with negative
			// screen-space winding. Optional (needs consistent winding).
			if (backfaceCull && edgeFunction(p[0], p[1], p[2].x, p[2].y) < 0.0f)
				continue;
			fillTriangle(p.data(), tri.colour.toUint32(), depthBuffer, W, H, canvas);
		}
	}
}

// Rasterise the model, but shadow it with a depth map rendered from the light
// (classic shadow mapping): pass 1 stores nearest depth from the light's view;
// pass 2 reprojects each fragment's world position into that map to test
// occlusion, and adds simple diffuse lighting.
void renderShadowMapped(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas,
                        const glm::vec3 &lightPos) {
	const int SW = 512, SH = 512;
	Camera lightCam(SW, SH, 2.0f, lightPos);
	lightCam.scale = 140.0f;
	lightCam.lookAt(glm::vec3(0.0f, -1.0f, 0.0f)); // look down over the scene
	std::vector<float> shadowMap(static_cast<size_t>(SW) * SH, 1e30f);

	// Pass 1: depth from the light.
	for (const ModelTriangle &tri : model) {
		CanvasPoint lp[3];
		bool ok = true;
		for (int i = 0; i < 3; i++) {
			lp[i] = lightCam.projectVertex(tri.vertices[i]);
			if (lp[i].depth <= 0.0f)
				ok = false;
		}
		if (!ok)
			continue;
		float area = edgeFunction(lp[0], lp[1], lp[2].x, lp[2].y);
		if (std::abs(area) < 1e-6f)
			continue;
		int minX = std::max(0, static_cast<int>(std::floor(std::min({lp[0].x, lp[1].x, lp[2].x}))));
		int maxX = std::min(SW - 1, static_cast<int>(std::ceil(std::max({lp[0].x, lp[1].x, lp[2].x}))));
		int minY = std::max(0, static_cast<int>(std::floor(std::min({lp[0].y, lp[1].y, lp[2].y}))));
		int maxY = std::min(SH - 1, static_cast<int>(std::ceil(std::max({lp[0].y, lp[1].y, lp[2].y}))));
		for (int y = minY; y <= maxY; y++) {
			for (int x = minX; x <= maxX; x++) {
				float w0 = edgeFunction(lp[1], lp[2], x + 0.5f, y + 0.5f) / area;
				float w1 = edgeFunction(lp[2], lp[0], x + 0.5f, y + 0.5f) / area;
				float w2 = edgeFunction(lp[0], lp[1], x + 0.5f, y + 0.5f) / area;
				if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
					continue;
				float depth = w0 * lp[0].depth + w1 * lp[1].depth + w2 * lp[2].depth;
				size_t idx = static_cast<size_t>(y) * SW + x;
				if (depth < shadowMap[idx])
					shadowMap[idx] = depth;
			}
		}
	}

	// Pass 2: camera view, shadow-tested and diffuse-lit.
	canvas.clearPixels();
	int W = static_cast<int>(canvas.width);
	int H = static_cast<int>(canvas.height);
	std::vector<float> depthBuffer(static_cast<size_t>(W) * H, 0.0f);
	const float bias = 0.03f, ambient = 0.25f;
	for (const ModelTriangle &tri : model) {
		CanvasPoint p[3];
		bool ok = true;
		for (int i = 0; i < 3; i++) {
			p[i] = camera.projectVertex(tri.vertices[i]);
			if (p[i].depth <= 0.0f)
				ok = false;
		}
		if (!ok)
			continue;
		float area = edgeFunction(p[0], p[1], p[2].x, p[2].y);
		if (std::abs(area) < 1e-6f)
			continue;
		int minX = std::max(0, static_cast<int>(std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
		int maxX = std::min(W - 1, static_cast<int>(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
		int minY = std::max(0, static_cast<int>(std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
		int maxY = std::min(H - 1, static_cast<int>(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
		float inv0 = 1.0f / p[0].depth, inv1 = 1.0f / p[1].depth, inv2 = 1.0f / p[2].depth;
		for (int y = minY; y <= maxY; y++) {
			for (int x = minX; x <= maxX; x++) {
				float w0 = edgeFunction(p[1], p[2], x + 0.5f, y + 0.5f) / area;
				float w1 = edgeFunction(p[2], p[0], x + 0.5f, y + 0.5f) / area;
				float w2 = edgeFunction(p[0], p[1], x + 0.5f, y + 0.5f) / area;
				if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
					continue;
				float invDepth = w0 * inv0 + w1 * inv1 + w2 * inv2;
				size_t idx = static_cast<size_t>(y) * W + x;
				if (invDepth <= depthBuffer[idx])
					continue;
				depthBuffer[idx] = invDepth;
				// Perspective-correct world position of this fragment.
				glm::vec3 wp =
				    (w0 * tri.vertices[0] * inv0 + w1 * tri.vertices[1] * inv1 + w2 * tri.vertices[2] * inv2) /
				    invDepth;
				bool shadow = false;
				CanvasPoint slp = lightCam.projectVertex(wp);
				if (slp.depth > 0.0f) {
					int sx = static_cast<int>(slp.x), sy = static_cast<int>(slp.y);
					if (sx >= 0 && sx < SW && sy >= 0 && sy < SH && slp.depth > shadowMap[sy * SW + sx] + bias)
						shadow = true;
				}
				glm::vec3 L = glm::normalize(lightPos - wp);
				float diffuse = std::max(0.0f, glm::dot(tri.normal, L));
				float b = std::min(1.0f, ambient + (shadow ? 0.0f : diffuse));
				const Colour &c = tri.colour;
				int r = std::min(255, static_cast<int>(c.red * b));
				int g = std::min(255, static_cast<int>(c.green * b));
				int bl = std::min(255, static_cast<int>(c.blue * b));
				canvas.setPixelColour(x, y, (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (bl & 0xFF));
			}
		}
	}
}

// Rasterise the scene, then shadow it with stencil shadow volumes (z-fail /
// Carmack's reverse): each occluder triangle is extruded away from the light
// into a closed volume; the volume's faces are rasterised into a stencil buffer,
// counting +1 for back faces and -1 for front faces that lie behind the scene
// depth. Pixels with a non-zero stencil are inside a shadow volume.
void renderStencilShadowVolumes(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas,
                                const glm::vec3 &lightPos) {
	canvas.clearPixels();
	int W = static_cast<int>(canvas.width);
	int H = static_cast<int>(canvas.height);
	std::vector<float> depthBuffer(static_cast<size_t>(W) * H, 0.0f); // inverse depth, 0 = far
	std::vector<glm::vec3> colourBuffer(static_cast<size_t>(W) * H, glm::vec3(0.0f));
	std::vector<int> stencil(static_cast<size_t>(W) * H, 0);

	// Pass 1: rasterise the lit scene into colour + depth.
	for (const ModelTriangle &tri : model) {
		CanvasPoint p[3];
		bool ok = true;
		for (int i = 0; i < 3; i++) {
			p[i] = camera.projectVertex(tri.vertices[i]);
			if (p[i].depth <= 0.0f)
				ok = false;
		}
		if (!ok)
			continue;
		float area = edgeFunction(p[0], p[1], p[2].x, p[2].y);
		if (std::abs(area) < 1e-6f)
			continue;
		int minX = std::max(0, static_cast<int>(std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
		int maxX = std::min(W - 1, static_cast<int>(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
		int minY = std::max(0, static_cast<int>(std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
		int maxY = std::min(H - 1, static_cast<int>(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
		float inv0 = 1.0f / p[0].depth, inv1 = 1.0f / p[1].depth, inv2 = 1.0f / p[2].depth;
		glm::vec3 n = tri.normal;
		glm::vec3 albedo = glm::vec3(tri.colour.red, tri.colour.green, tri.colour.blue) / 255.0f;
		for (int y = minY; y <= maxY; y++) {
			for (int x = minX; x <= maxX; x++) {
				float w0 = edgeFunction(p[1], p[2], x + 0.5f, y + 0.5f) / area;
				float w1 = edgeFunction(p[2], p[0], x + 0.5f, y + 0.5f) / area;
				float w2 = edgeFunction(p[0], p[1], x + 0.5f, y + 0.5f) / area;
				if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
					continue;
				float invDepth = w0 * inv0 + w1 * inv1 + w2 * inv2;
				size_t idx = static_cast<size_t>(y) * W + x;
				if (invDepth <= depthBuffer[idx])
					continue;
				depthBuffer[idx] = invDepth;
				glm::vec3 wp =
				    (w0 * tri.vertices[0] * inv0 + w1 * tri.vertices[1] * inv1 + w2 * tri.vertices[2] * inv2) /
				    invDepth;
				glm::vec3 L = glm::normalize(lightPos - wp);
				float diff = std::max(0.0f, glm::dot(n, L));
				colourBuffer[idx] = albedo * (0.25f + 0.75f * diff) * 255.0f;
			}
		}
	}

	// Pass 2: rasterise each occluder's shadow-volume faces into the stencil.
	auto stencilPass = [&](const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c) {
		CanvasPoint p[3] = {camera.projectVertex(a), camera.projectVertex(b), camera.projectVertex(c)};
		if (p[0].depth <= 0.0f || p[1].depth <= 0.0f || p[2].depth <= 0.0f)
			return;
		float area = edgeFunction(p[0], p[1], p[2].x, p[2].y);
		if (std::abs(area) < 1e-6f)
			return;
		int dir = area > 0.0f ? +1 : -1; // screen winding = front/back face
		int minX = std::max(0, static_cast<int>(std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
		int maxX = std::min(W - 1, static_cast<int>(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
		int minY = std::max(0, static_cast<int>(std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
		int maxY = std::min(H - 1, static_cast<int>(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
		float inv0 = 1.0f / p[0].depth, inv1 = 1.0f / p[1].depth, inv2 = 1.0f / p[2].depth;
		for (int y = minY; y <= maxY; y++) {
			for (int x = minX; x <= maxX; x++) {
				float w0 = edgeFunction(p[1], p[2], x + 0.5f, y + 0.5f) / area;
				float w1 = edgeFunction(p[2], p[0], x + 0.5f, y + 0.5f) / area;
				float w2 = edgeFunction(p[0], p[1], x + 0.5f, y + 0.5f) / area;
				if ((w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) && (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f))
					continue; // outside the triangle (either winding)
				float invDepth = w0 * inv0 + w1 * inv1 + w2 * inv2;
				size_t idx = static_cast<size_t>(y) * W + x;
				if (invDepth < depthBuffer[idx]) // z-fail: this face is behind the scene
					stencil[idx] += dir;
			}
		}
	};
	const float farDist = 40.0f;
	for (const ModelTriangle &tri : model) {
		glm::vec3 v0 = tri.vertices[0], v1 = tri.vertices[1], v2 = tri.vertices[2];
		glm::vec3 f0 = v0 + glm::normalize(v0 - lightPos) * farDist;
		glm::vec3 f1 = v1 + glm::normalize(v1 - lightPos) * farDist;
		glm::vec3 f2 = v2 + glm::normalize(v2 - lightPos) * farDist;
		stencilPass(v0, v1, v2); // near cap
		stencilPass(f0, f2, f1); // far cap (reversed)
		stencilPass(v0, v1, f1); // side quads
		stencilPass(v0, f1, f0); //
		stencilPass(v1, v2, f2); //
		stencilPass(v1, f2, f1); //
		stencilPass(v2, v0, f0); //
		stencilPass(v2, f0, f2); //
	}

	// Composite: darken pixels inside a shadow volume (stencil != 0).
	for (int i = 0; i < W * H; i++) {
		glm::vec3 c = colourBuffer[static_cast<size_t>(i)];
		if (stencil[static_cast<size_t>(i)] != 0)
			c *= 0.3f;
		int r = std::min(255, static_cast<int>(c.r));
		int g = std::min(255, static_cast<int>(c.g));
		int b = std::min(255, static_cast<int>(c.b));
		canvas.pixels[static_cast<size_t>(i)] = (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
	}
}

// Fraction of a light visible from `point` (1 = lit, 0 = shadowed). Point and
// directional lights are binary; an area light (radius > 0) is sampled over a
// fixed disk for soft shadows (deterministic pattern, so no noise).
static float visibility(const Light &L, const glm::vec3 &point, const Scene &scene, int ignoreIndex) {
	if (L.type == LightType::Directional) {
		glm::vec3 dir = -glm::normalize(L.direction);
		return scene.occluded(point, dir, 1e6f, ignoreIndex) ? 0.0f : 1.0f;
	}
	if (L.type == LightType::Rectangle) {
		// Sample a fixed 3x3 grid over the rectangle for a soft rectangular penumbra.
		static const glm::vec2 kGrid[9] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {0, 0},
		                                   {1, 0},   {-1, 1}, {0, 1},  {1, 1}};
		int unoccluded = 0;
		for (const glm::vec2 &s : kGrid) {
			glm::vec3 samplePos = L.position + L.uAxis * s.x + L.vAxis * s.y;
			glm::vec3 to = samplePos - point;
			float dist = glm::length(to);
			if (!scene.occluded(point, to / dist, dist, ignoreIndex))
				unoccluded++;
		}
		return unoccluded / 9.0f;
	}
	if (L.type == LightType::Triangle) {
		// A triangular emitter with corners position, position + uAxis, position + vAxis.
		// Sample a fixed set of 9 barycentric weights (b1, b2) that all satisfy
		// b1 + b2 <= 1 (a 3x3-style grid inside the triangle plus the centroid) so
		// every sample lands on the emitter, giving a soft triangular penumbra.
		static const glm::vec2 kBary[9] = {{0.0f, 0.0f}, // corner at position
		                                   {1.0f, 0.0f}, // corner at position + uAxis
		                                   {0.0f, 1.0f}, // corner at position + vAxis
		                                   {0.5f, 0.0f}, // edge midpoints
		                                   {0.0f, 0.5f},   {0.5f, 0.5f},
		                                   {0.25f, 0.25f},                              // interior points
		                                   {0.5f, 0.25f},  {1.0f / 3.0f, 1.0f / 3.0f}}; // centroid
		int unoccluded = 0;
		for (const glm::vec2 &b : kBary) {
			glm::vec3 samplePos = L.position + L.uAxis * b.x + L.vAxis * b.y;
			glm::vec3 to = samplePos - point;
			float dist = glm::length(to);
			if (!scene.occluded(point, to / dist, dist, ignoreIndex))
				unoccluded++;
		}
		return unoccluded / 9.0f;
	}
	if (L.radius <= 0.0f) {
		glm::vec3 to = L.position - point;
		float dist = glm::length(to);
		return scene.occluded(point, to / dist, dist, ignoreIndex) ? 0.0f : 1.0f;
	}
	if (L.type == LightType::Volume) {
		// A spherical emitter, sampled by SOLID ANGLE (cone importance sampling).
		// The sphere of radius L.radius at L.position subtends a cone as seen from
		// the shaded point; we shoot a fixed deterministic set of directions inside
		// that cone (no RNG, so no noise). This is the Scratchapixel spherical-light
		// cone-sampling technique and is more correct than sampling surface points.
		const float kPi = 3.14159265358979323846f;
		glm::vec3 toCentre = L.position - point;
		float d = glm::length(toCentre);
		glm::vec3 dir = toCentre / d; // cone axis: point -> sphere centre
		float sinThetaMax = std::min(1.0f, L.radius / d);
		// If the shaded point is inside the sphere the cone opens past a hemisphere,
		// so treat the light as fully visible (surrounded by the emitter).
		if (sinThetaMax >= 1.0f)
			return 1.0f;
		float cosThetaMax = std::sqrt(std::max(0.0f, 1.0f - sinThetaMax * sinThetaMax));
		// Orthonormal basis about the cone axis.
		glm::vec3 up = std::abs(dir.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
		glm::vec3 tangent = glm::normalize(glm::cross(up, dir));
		glm::vec3 bitangent = glm::cross(dir, tangent);
		// Fixed cone pattern: the axis plus two rings (inner and outer) of azimuths,
		// giving 1 + 5 + 6 = 12 deterministic directions inside the cone. cosTheta is
		// interpolated in [cosThetaMax, 1] so every direction stays within the cone.
		static const float kRingCos[2] = {0.85f, 0.4f}; // fraction toward cosThetaMax per ring
		static const int kRingCount[2] = {5, 6};
		static const float kRingOffset[2] = {0.0f, 0.5f}; // stagger the outer ring azimuths
		int unoccluded = 0;
		int total = 1;
		// Central sample straight along the axis.
		if (!scene.occluded(point, dir, d, ignoreIndex))
			unoccluded++;
		for (int ring = 0; ring < 2; ring++) {
			float cosTheta = 1.0f - kRingCos[ring] * (1.0f - cosThetaMax);
			float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
			int count = kRingCount[ring];
			for (int i = 0; i < count; i++) {
				float phi = 2.0f * kPi * ((static_cast<float>(i) + kRingOffset[ring]) / static_cast<float>(count));
				glm::vec3 sampleDir = dir * cosTheta + (tangent * std::cos(phi) + bitangent * std::sin(phi)) * sinTheta;
				sampleDir = glm::normalize(sampleDir);
				// Occlusion up to the sphere centre distance.
				if (!scene.occluded(point, sampleDir, d, ignoreIndex))
					unoccluded++;
				total++;
			}
		}
		return static_cast<float>(unoccluded) / static_cast<float>(total);
	}
	static const glm::vec2 kSamples[9] = {{0, 0},       {1, 0},        {-1, 0},       {0, 1},        {0, -1},
	                                      {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
	glm::vec3 axis = glm::normalize(point - L.position);
	glm::vec3 up = std::abs(axis.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
	glm::vec3 tangent = glm::normalize(glm::cross(up, axis));
	glm::vec3 bitangent = glm::cross(axis, tangent);
	int unoccluded = 0;
	for (const glm::vec2 &s : kSamples) {
		glm::vec3 samplePos = L.position + (tangent * s.x + bitangent * s.y) * L.radius;
		glm::vec3 to = samplePos - point;
		float dist = glm::length(to);
		if (!scene.occluded(point, to / dist, dist, ignoreIndex))
			unoccluded++;
	}
	return unoccluded / 9.0f;
}

// Multi-light shading of a surface point: proximity/incidence diffuse + specular
// + soft-shadow visibility summed over every light, on an ambient floor. `base`
// is the surface's own colour (0..255).
static glm::vec3 shadeSurface(const glm::vec3 &point, const glm::vec3 &normal, const glm::vec3 &viewDir,
                              const std::vector<Light> &lights, const Scene &scene, int ignoreIndex,
                              const glm::vec3 &base) {
	const float ambient = 0.2f;
	glm::vec3 diffuseAccum(0.0f);
	float specAccum = 0.0f;
	for (const Light &L : lights) {
		glm::vec3 lightDir;
		float attenuation;
		if (L.type == LightType::Directional) {
			lightDir = -glm::normalize(L.direction);
			attenuation = L.intensity / 40.0f;
		} else {
			glm::vec3 to = L.position - point;
			float dist = glm::length(to);
			lightDir = to / dist;
			attenuation = L.intensity / (L.attenConstant + L.attenLinear * dist + L.attenQuadratic * dist * dist);
		}
		float cone = 1.0f;
		if (L.type == LightType::Spot)
			cone = (glm::dot(-lightDir, glm::normalize(L.direction)) > L.coneCos) ? 1.0f : 0.0f;
		float vis = visibility(L, point, scene, ignoreIndex);
		float incidence = std::max(0.0f, glm::dot(normal, lightDir));
		float brightness = vis * cone * attenuation * incidence;
		diffuseAccum += L.colour * brightness;
		if (brightness > 0.0f) {
			glm::vec3 reflectDir = 2.0f * glm::dot(normal, lightDir) * normal - lightDir;
			specAccum += vis * cone * std::pow(std::max(0.0f, glm::dot(reflectDir, viewDir)), 64.0f);
		}
	}
	glm::vec3 lit = glm::min(glm::vec3(1.0f), diffuseAccum + glm::vec3(ambient));
	return base * lit + glm::vec3(255.0f) * std::min(1.0f, specAccum);
}

// Flip a normal to face the viewer (rayDirection points camera -> surface).
static glm::vec3 faceViewer(glm::vec3 normal, const glm::vec3 &rayDirection) {
	return (glm::dot(normal, rayDirection) > 0.0f) ? -normal : normal;
}

// A simple procedural sky: rays that escape the scene sample this instead of black.
// Optional physically-based sky (Rayleigh + Mie); off by default, enabled via
// setSkyModel. Its radiance has a huge dynamic range, so we expose + tone-map it.
static bool g_skyEnabled = false;
static glm::vec3 g_skySun = glm::normalize(glm::vec3(0.3f, 0.45f, -1.0f));
static float g_skyExposure = 16.0f;

void setSkyModel(bool enabled, const glm::vec3 &sunDir) {
	g_skyEnabled = enabled;
	if (glm::length(sunDir) > 1e-4f)
		g_skySun = glm::normalize(sunDir);
}

static glm::vec3 environment(const glm::vec3 &direction) {
	if (g_skyEnabled) {
		glm::vec3 c = skyColour(direction, g_skySun) * g_skyExposure;
		c = c / (c + glm::vec3(1.0f)); // Reinhard over the wide sky/sun range
		return c * 255.0f;
	}
	float t = 0.5f * (direction.y + 1.0f);
	return glm::mix(glm::vec3(200.0f, 210.0f, 235.0f), glm::vec3(70.0f, 120.0f, 205.0f), t);
}

// Procedural wood grain from fBm-perturbed rings (Material::Procedural).
static glm::vec3 proceduralColour(const glm::vec3 &p) {
	glm::vec3 q = p * 3.0f;
	float n = fractalNoise(q, 4);
	float rings = 0.5f + 0.5f * std::sin((q.x + q.y) * 3.0f + n * 8.0f);
	return glm::mix(glm::vec3(60.0f, 30.0f, 12.0f), glm::vec3(205.0f, 150.0f, 90.0f), rings);
}

// Perturb a normal by the gradient of a noise field (Material::Bump).
static glm::vec3 bumpNormal(const glm::vec3 &normal, const glm::vec3 &p) {
	glm::vec3 q = p * 6.0f;
	float e = 0.15f;
	glm::vec3 gradient(perlin(q.x + e, q.y, q.z) - perlin(q.x - e, q.y, q.z),
	                   perlin(q.x, q.y + e, q.z) - perlin(q.x, q.y - e, q.z),
	                   perlin(q.x, q.y, q.z + e) - perlin(q.x, q.y, q.z - e));
	return glm::normalize(normal + 2.0f * gradient);
}

// Nearest-neighbour texel lookup at normalised (u, v); v is flipped for image origin.
static glm::vec3 sampleTexture(const TextureMap &tex, float u, float v) {
	int tw = static_cast<int>(tex.width);
	int th = static_cast<int>(tex.height);
	int tx = std::min(std::max(static_cast<int>(u * (tw - 1)), 0), tw - 1);
	int ty = std::min(std::max(static_cast<int>((1.0f - v) * (th - 1)), 0), th - 1);
	uint32_t p = tex.pixels[static_cast<size_t>(ty) * tw + tx];
	return glm::vec3((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
}

// Offset a texture coordinate along the view direction (in the triangle's tangent
// frame) by a height read from the texture, giving simple parallax mapping.
static glm::vec2 parallaxUV(const ModelTriangle &tri, glm::vec2 uv, const glm::vec3 &viewDir) {
	glm::vec3 e1 = tri.vertices[1] - tri.vertices[0];
	glm::vec3 e2 = tri.vertices[2] - tri.vertices[0];
	glm::vec2 d1(tri.texturePoints[1].x - tri.texturePoints[0].x, tri.texturePoints[1].y - tri.texturePoints[0].y);
	glm::vec2 d2(tri.texturePoints[2].x - tri.texturePoints[0].x, tri.texturePoints[2].y - tri.texturePoints[0].y);
	float det = d1.x * d2.y - d2.x * d1.y;
	if (std::abs(det) < 1e-8f)
		return uv;
	float f = 1.0f / det;
	glm::vec3 T = glm::normalize(f * (d2.y * e1 - d1.y * e2));
	glm::vec3 B = glm::normalize(f * (-d2.x * e1 + d1.x * e2));
	glm::vec3 vTS(glm::dot(viewDir, T), glm::dot(viewDir, B), glm::dot(viewDir, tri.normal));
	if (std::abs(vTS.z) < 1e-4f)
		return uv;
	float height = sampleTexture(*tri.texture, uv.x, uv.y).x / 255.0f; // red channel as height
	glm::vec2 offset = glm::vec2(vTS.x, vTS.y) / vTS.z * (height * 0.05f);
	return uv - offset;
}

// Tangent-space normal mapping: build the triangle's tangent frame from its
// positions + UVs, read a normal from the texture's height gradient, and rotate
// it into world space. Perturbs the shading normal per texel from a texture.
static glm::vec3 tangentNormal(const ModelTriangle &tri, float u, float v, const glm::vec3 &geomN) {
	if (!tri.texture)
		return geomN;
	glm::vec3 e1 = tri.vertices[1] - tri.vertices[0];
	glm::vec3 e2 = tri.vertices[2] - tri.vertices[0];
	glm::vec2 d1(tri.texturePoints[1].x - tri.texturePoints[0].x, tri.texturePoints[1].y - tri.texturePoints[0].y);
	glm::vec2 d2(tri.texturePoints[2].x - tri.texturePoints[0].x, tri.texturePoints[2].y - tri.texturePoints[0].y);
	float det = d1.x * d2.y - d2.x * d1.y;
	if (std::abs(det) < 1e-8f)
		return geomN;
	float f = 1.0f / det;
	glm::vec3 T = glm::normalize(f * (d2.y * e1 - d1.y * e2));
	glm::vec3 B = glm::normalize(f * (-d2.x * e1 + d1.x * e2));
	float e = 1.0f / static_cast<float>(tri.texture->width);
	auto lum = [&](float uu, float vv) {
		glm::vec3 c = sampleTexture(*tri.texture, uu, vv);
		return (c.r + c.g + c.b) / 765.0f;
	};
	float hu = lum(u + e, v) - lum(u - e, v);
	float hv = lum(u, v + e) - lum(u, v - e);
	glm::vec3 tn = glm::normalize(glm::vec3(-hu * 4.0f, -hv * 4.0f, 1.0f));
	return glm::normalize(tn.x * T + tn.y * B + tn.z * geomN);
}

// The surface's own colour (0..255) at a hit: sampled texture, procedural, or flat.
static glm::vec3 surfaceBaseColour(const RayTriangleIntersection &hit, const glm::vec3 &viewDir) {
	const ModelTriangle &tri = hit.intersectedTriangle;
	float w0 = 1.0f - hit.u - hit.v;
	float w1 = hit.u;
	float w2 = hit.v;
	if (tri.texture) {
		float tu = w0 * tri.texturePoints[0].x + w1 * tri.texturePoints[1].x + w2 * tri.texturePoints[2].x;
		float tv = w0 * tri.texturePoints[0].y + w1 * tri.texturePoints[1].y + w2 * tri.texturePoints[2].y;
		if (tri.material == Material::Parallax) {
			glm::vec2 uv = parallaxUV(tri, glm::vec2(tu, tv), viewDir);
			tu = uv.x;
			tv = uv.y;
		}
		return sampleTexture(*tri.texture, tu, tv);
	}
	if (tri.material == Material::Procedural)
		return proceduralColour(hit.intersectionPoint);
	return glm::vec3(tri.colour.red, tri.colour.green, tri.colour.blue);
}

static glm::vec3 shadeDiffuse(const RayTriangleIntersection &hit, const glm::vec3 &rayDirection,
                              const std::vector<Light> &lights, const Scene &scene, ShadingModel shading) {
	const ModelTriangle &tri = hit.intersectedTriangle;
	glm::vec3 point = hit.intersectionPoint;
	glm::vec3 viewDir = -rayDirection;
	int ignore = static_cast<int>(hit.triangleIndex);

	// Barycentric weights (vertex 0 gets 1-u-v).
	float w0 = 1.0f - hit.u - hit.v;
	float w1 = hit.u;
	float w2 = hit.v;

	glm::vec3 base = surfaceBaseColour(hit, viewDir);

	if (shading == ShadingModel::Gouraud) {
		// Shade each vertex, then interpolate the resulting colour.
		glm::vec3 c0 = shadeSurface(tri.vertices[0], faceViewer(tri.vertexNormals[0], rayDirection), viewDir, lights,
		                            scene, ignore, base);
		glm::vec3 c1 = shadeSurface(tri.vertices[1], faceViewer(tri.vertexNormals[1], rayDirection), viewDir, lights,
		                            scene, ignore, base);
		glm::vec3 c2 = shadeSurface(tri.vertices[2], faceViewer(tri.vertexNormals[2], rayDirection), viewDir, lights,
		                            scene, ignore, base);
		return w0 * c0 + w1 * c1 + w2 * c2;
	}

	glm::vec3 normal =
	    (shading == ShadingModel::Phong)
	        ? glm::normalize(w0 * tri.vertexNormals[0] + w1 * tri.vertexNormals[1] + w2 * tri.vertexNormals[2])
	        : tri.normal;
	normal = faceViewer(normal, rayDirection);
	if (tri.material == Material::Bump)
		normal = bumpNormal(normal, point);
	if (tri.material == Material::NormalMap && tri.texture) {
		float tu = w0 * tri.texturePoints[0].x + w1 * tri.texturePoints[1].x + w2 * tri.texturePoints[2].x;
		float tv = w0 * tri.texturePoints[0].y + w1 * tri.texturePoints[1].y + w2 * tri.texturePoints[2].y;
		normal = tangentNormal(tri, tu, tv, normal);
	}
	return shadeSurface(point, normal, viewDir, lights, scene, ignore, base);
}

// Recursively trace a ray. Diffuse surfaces are shaded directly; mirrors reflect
// and glass reflects + refracts (Fresnel-weighted), bounded by `depth`.
static glm::vec3 traceRay(const glm::vec3 &origin, const glm::vec3 &direction, const Scene &scene,
                          const std::vector<Light> &lights, ShadingModel shading, int depth);

// Reflect + refract a glass surface for a single index of refraction, blended by
// Fresnel. Tracing this per-wavelength (with slightly different `ior`) gives
// chromatic dispersion (prisms, rainbow fringing).
static glm::vec3 glassTrace(const glm::vec3 &point, const glm::vec3 &direction, const glm::vec3 &triNormal,
                            const Scene &scene, const std::vector<Light> &lights, ShadingModel shading, int depth,
                            float ior) {
	glm::vec3 n = triNormal;
	float cosi = glm::dot(direction, n);
	float etai = 1.0f, etat = ior;
	if (cosi < 0.0f) {
		cosi = -cosi;
	} else {
		std::swap(etai, etat);
		n = -n;
	}
	glm::vec3 reflected = glm::reflect(direction, n);
	glm::vec3 reflectionColour = traceRay(point + 1e-4f * reflected, reflected, scene, lights, shading, depth - 1);
	glm::vec3 refracted = glm::refract(direction, n, etai / etat);
	if (glm::length(refracted) < 1e-6f)
		return reflectionColour; // total internal reflection
	float r0 = (etai - etat) / (etai + etat);
	r0 *= r0;
	float fresnel = r0 + (1.0f - r0) * std::pow(1.0f - cosi, 5.0f);
	glm::vec3 refractionColour = traceRay(point + 1e-4f * refracted, refracted, scene, lights, shading, depth - 1);
	return fresnel * reflectionColour + (1.0f - fresnel) * refractionColour;
}

static glm::vec3 traceRay(const glm::vec3 &origin, const glm::vec3 &direction, const Scene &scene,
                          const std::vector<Light> &lights, ShadingModel shading, int depth) {
	RayTriangleIntersection hit = scene.intersect(origin, direction);
	if (!hit.hit)
		return environment(direction); // sky
	const ModelTriangle &tri = hit.intersectedTriangle;
	glm::vec3 point = hit.intersectionPoint;

	if (tri.material == Material::Emissive)
		return glm::vec3(tri.colour.red, tri.colour.green, tri.colour.blue) * 1.3f;

	if (depth > 0 && tri.material == Material::Mirror) {
		glm::vec3 n = faceViewer(tri.normal, direction);
		glm::vec3 reflected = glm::reflect(direction, n);
		return traceRay(point + 1e-4f * reflected, reflected, scene, lights, shading, depth - 1);
	}

	if (depth > 0 && tri.material == Material::Glass) {
		return glassTrace(point, direction, tri.normal, scene, lights, shading, depth, 1.5f);
	}
	if (depth > 0 && tri.material == Material::Dispersive) {
		// Trace each colour channel with a slightly different index of refraction
		// (red bends least, blue most), so white light fans into a spectrum.
		glm::vec3 r = glassTrace(point, direction, tri.normal, scene, lights, shading, depth, 1.50f);
		glm::vec3 g = glassTrace(point, direction, tri.normal, scene, lights, shading, depth, 1.53f);
		glm::vec3 b = glassTrace(point, direction, tri.normal, scene, lights, shading, depth, 1.56f);
		return glm::vec3(r.r, g.g, b.b);
	}

	if (depth > 0 && tri.material == Material::Metal) {
		// Metal reflects, tinted by its albedo (roughness ignored in the Whitted
		// path, which is deterministic; the path tracer blurs by roughness).
		glm::vec3 n = faceViewer(tri.normal, direction);
		glm::vec3 reflected = glm::reflect(direction, n);
		glm::vec3 refl = traceRay(point + 1e-4f * reflected, reflected, scene, lights, shading, depth - 1);
		return glm::vec3(tri.colour.red, tri.colour.green, tri.colour.blue) / 255.0f * refl;
	}

	return shadeDiffuse(hit, direction, lights, scene, shading);
}

// --- Monte-Carlo path tracer ------------------------------------------------

// Cosine-weighted hemisphere direction around a normal.
static glm::vec3 cosineHemisphere(const glm::vec3 &n, std::mt19937 &rng) {
	std::uniform_real_distribution<float> U(0.0f, 1.0f);
	float u1 = U(rng), u2 = U(rng);
	float r = std::sqrt(u1);
	float theta = 2.0f * 3.14159265f * u2;
	float x = r * std::cos(theta), y = r * std::sin(theta), z = std::sqrt(std::max(0.0f, 1.0f - u1));
	glm::vec3 up = std::abs(n.z) < 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
	glm::vec3 t = glm::normalize(glm::cross(up, n));
	glm::vec3 b = glm::cross(n, t);
	return glm::normalize(t * x + b * y + n * z);
}

// Direct lighting (0..1) at a diffuse point, summed over the lights.
static glm::vec3 directLight(const glm::vec3 &point, const glm::vec3 &normal, const glm::vec3 &albedo,
                             const std::vector<Light> &lights, const Scene &scene, int ignore) {
	glm::vec3 sum(0.0f);
	for (const Light &L : lights) {
		glm::vec3 lightDir;
		float attenuation;
		if (L.type == LightType::Directional) {
			lightDir = -glm::normalize(L.direction);
			attenuation = L.intensity / 40.0f;
		} else {
			glm::vec3 to = L.position - point;
			float dist = glm::length(to);
			lightDir = to / dist;
			attenuation = L.intensity / (L.attenConstant + L.attenLinear * dist + L.attenQuadratic * dist * dist);
		}
		float cone = 1.0f;
		if (L.type == LightType::Spot)
			cone = (glm::dot(-lightDir, glm::normalize(L.direction)) > L.coneCos) ? 1.0f : 0.0f;
		float vis = visibility(L, point, scene, ignore);
		float incidence = std::max(0.0f, glm::dot(normal, lightDir));
		sum += L.colour * (vis * cone * attenuation * incidence);
	}
	return albedo * glm::min(sum, glm::vec3(1.0f));
}

// Trace one path: direct light at each diffuse bounce plus a recursive
// cosine-weighted indirect bounce (this is what produces colour bleeding /
// global illumination). Mirror reflects; glass reflects or refracts
// stochastically by Fresnel.
static glm::vec3 pathTrace(const glm::vec3 &origin, const glm::vec3 &direction, const Scene &scene,
                           const std::vector<Light> &lights, int depth, std::mt19937 &rng) {
	RayTriangleIntersection hit = scene.intersect(origin, direction);
	if (!hit.hit)
		return environment(direction) / 255.0f;
	const ModelTriangle &tri = hit.intersectedTriangle;
	glm::vec3 point = hit.intersectionPoint;
	int ignore = static_cast<int>(hit.triangleIndex);

	if (tri.material == Material::Emissive) {
		// Glowing geometry: emit its colour. A bounce ray landing here picks up
		// the emission, so the surface lights the scene as an area light.
		return glm::vec3(tri.colour.red, tri.colour.green, tri.colour.blue) / 255.0f * 4.0f;
	}
	if (tri.material == Material::Mirror && depth > 0) {
		glm::vec3 n = faceViewer(tri.normal, direction);
		glm::vec3 reflected = glm::reflect(direction, n);
		return pathTrace(point + 1e-4f * reflected, reflected, scene, lights, depth - 1, rng);
	}
	if ((tri.material == Material::Glass || tri.material == Material::Dispersive) && depth > 0) {
		const float ior = 1.5f;
		glm::vec3 n = tri.normal;
		float cosi = glm::dot(direction, n);
		float etai = 1.0f, etat = ior;
		if (cosi < 0.0f) {
			cosi = -cosi;
		} else {
			std::swap(etai, etat);
			n = -n;
		}
		float r0 = (etai - etat) / (etai + etat);
		r0 *= r0;
		float fresnel = r0 + (1.0f - r0) * std::pow(1.0f - cosi, 5.0f);
		glm::vec3 refracted = glm::refract(direction, n, etai / etat);
		std::uniform_real_distribution<float> U(0.0f, 1.0f);
		glm::vec3 dir = (glm::length(refracted) < 1e-6f || U(rng) < fresnel) ? glm::reflect(direction, n) : refracted;
		return pathTrace(point + 1e-4f * dir, dir, scene, lights, depth - 1, rng);
	}
	if (tri.material == Material::Metal && depth > 0) {
		// Metallic/roughness: reflect the environment, tinted by the albedo and
		// blurred by roughness (mix the mirror direction toward a diffuse sample).
		glm::vec3 n = faceViewer(tri.normal, direction);
		glm::vec3 reflected = glm::reflect(direction, n);
		glm::vec3 diffuseDir = cosineHemisphere(n, rng);
		float r2 = tri.roughness * tri.roughness;
		glm::vec3 dir = glm::normalize(glm::mix(reflected, diffuseDir, r2));
		if (glm::dot(dir, n) <= 0.0f)
			dir = reflected;
		glm::vec3 albedo = surfaceBaseColour(hit, -direction) / 255.0f;
		return albedo * pathTrace(point + 1e-4f * dir, dir, scene, lights, depth - 1, rng);
	}

	if (tri.material == Material::Subsurface) {
		// A cheap BSSRDF approximation: wrap-around diffuse (light bleeds past the
		// terminator for a soft, waxy look) plus translucency - light entering the
		// far side and diffusing out through thin regions, attenuated by the
		// distance through the object (its "thickness" toward the light).
		glm::vec3 n = faceViewer(tri.normal, direction);
		glm::vec3 albedo = surfaceBaseColour(hit, -direction) / 255.0f;
		glm::vec3 shade(0.0f);
		const float wrap = 0.5f;
		for (const Light &L : lights) {
			glm::vec3 to = L.position - point;
			float dist = glm::length(to);
			glm::vec3 lightDir = to / dist;
			float atten = L.intensity / (L.attenConstant + L.attenLinear * dist + L.attenQuadratic * dist * dist);
			float wrapDiff = std::max(0.0f, (glm::dot(n, lightDir) + wrap) / (1.0f + wrap));
			shade += albedo * L.colour * (visibility(L, point, scene, ignore) * atten * wrapDiff);
			RayTriangleIntersection exit = scene.intersect(point - n * 1e-3f, lightDir, ignore);
			float thickness = exit.hit ? exit.distanceFromCamera : 0.0f;
			shade += albedo * L.colour * (atten * std::exp(-3.0f * thickness) * 0.6f);
		}
		glm::vec3 direct = glm::min(shade, glm::vec3(1.5f));
		if (depth <= 0)
			return direct;
		glm::vec3 sb = cosineHemisphere(n, rng);
		return direct + albedo * pathTrace(point + 1e-4f * sb, sb, scene, lights, depth - 1, rng);
	}

	glm::vec3 n = faceViewer(tri.normal, direction);
	glm::vec3 albedo = surfaceBaseColour(hit, -direction) / 255.0f;
	glm::vec3 direct = directLight(point, n, albedo, lights, scene, ignore);
	if (depth <= 0)
		return direct;
	glm::vec3 bounce = cosineHemisphere(n, rng);
	glm::vec3 indirect = albedo * pathTrace(point + 1e-4f * bounce, bounce, scene, lights, depth - 1, rng);
	return direct + indirect;
}

void renderRaytraced(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas,
                     ShadingModel shading, const std::vector<Light> &lights, const Primitives &prims, float fogDensity,
                     const glm::vec3 &fogColour) {
	canvas.clearPixels();
	int W = static_cast<int>(canvas.width);
	int H = static_cast<int>(canvas.height);
	float f = camera.focalLength * camera.scale;
	const int maxDepth = 4;
	Scene scene(model, prims); // triangles (BVH) + analytic primitives

	// Default to one soft (area) light so shadows have a penumbra out of the box.
	std::vector<Light> used = lights;
	if (used.empty()) {
		Light d;
		d.radius = 0.15f;
		used.push_back(d);
	}

	// Each pixel is independent, so the scanline loop parallelises cleanly.
#pragma omp parallel for schedule(dynamic, 4)
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			// Primary ray through this pixel (perspective, or parallel if the
			// camera is orthographic).
			glm::vec3 rayO, direction;
			camera.primaryRay(x - W / 2.0f, -(y - H / 2.0f), f, rayO, direction);
			glm::vec3 colour = traceRay(rayO, direction, scene, used, shading, maxDepth);
			// Depth fog: blend toward the fog colour by 1 - exp(-density * distance).
			if (fogDensity > 0.0f) {
				RayTriangleIntersection h = scene.intersect(rayO, direction);
				float dist = h.hit ? h.distanceFromCamera : 30.0f;
				colour = glm::mix(colour, fogColour, 1.0f - std::exp(-fogDensity * dist));
			}
			int r = std::min(255, static_cast<int>(colour.r));
			int g = std::min(255, static_cast<int>(colour.g));
			int b = std::min(255, static_cast<int>(colour.b));
			canvas.setPixelColour(x, y, (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF));
		}
	}
}

void renderVolumetric(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas,
                      const std::vector<Light> &lights, const Primitives &prims, float density, int steps) {
	canvas.clearPixels();
	int W = static_cast<int>(canvas.width);
	int H = static_cast<int>(canvas.height);
	float f = camera.focalLength * camera.scale;
	Scene scene(model, prims);
	std::vector<Light> used = lights;
	if (used.empty()) {
		Light d;
		d.radius = 0.15f;
		used.push_back(d);
	}

#pragma omp parallel for schedule(dynamic, 4)
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			glm::vec3 dirCamera((x - W / 2.0f) / f, -(y - H / 2.0f) / f, -1.0f);
			glm::vec3 direction = glm::normalize(camera.orientation * dirCamera);
			RayTriangleIntersection hit = scene.intersect(camera.position, direction);
			float surfaceDist = hit.hit ? hit.distanceFromCamera : 20.0f;
			glm::vec3 surfaceCol = traceRay(camera.position, direction, scene, used, ShadingModel::Phong, 4);
			// March the fog: accumulate single-scattered, shadow-tested light so
			// beams form visible shafts where the light is not occluded.
			glm::vec3 inscatter(0.0f);
			float transmittance = 1.0f;
			float dt = surfaceDist / steps;
			for (int i = 0; i < steps; i++) {
				glm::vec3 p = camera.position + direction * ((i + 0.5f) * dt);
				for (const Light &L : used) {
					glm::vec3 to = L.position - p;
					float d = glm::length(to);
					if (!scene.occluded(p, to / d, d, -1)) {
						float atten = L.intensity / (L.attenConstant + L.attenLinear * d + L.attenQuadratic * d * d);
						inscatter += transmittance * (density * dt) * L.colour * atten;
					}
				}
				transmittance *= std::exp(-density * dt);
			}
			glm::vec3 colour = surfaceCol * transmittance + inscatter * 55.0f;
			int r = std::min(255, static_cast<int>(colour.r));
			int g = std::min(255, static_cast<int>(colour.g));
			int b = std::min(255, static_cast<int>(colour.b));
			canvas.setPixelColour(x, y, (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF));
		}
	}
}

// --- Stochastic (delta-tracking) volume rendering --------------------------
// Density of a Perlin-modulated spherical cloud at world point p (0 outside).
static float mcCloudDensity(const glm::vec3 &p, const glm::vec3 &centre, float R) {
	float r = glm::length(p - centre);
	if (r > R)
		return 0.0f;
	float edge = 1.0f - r / R; // soft falloff to the boundary
	float n = 0.5f + 0.5f * fractalNoise(p * 2.4f + glm::vec3(4.7f), 5);
	return std::max(0.0f, (n - 0.42f) * 2.4f * edge);
}

// The t-interval [t0,t1] the ray (ro,rd) spends inside the bounding sphere.
static bool raySphereSpan(const glm::vec3 &ro, const glm::vec3 &rd, const glm::vec3 &c, float R, float &t0, float &t1) {
	glm::vec3 oc = ro - c;
	float b = glm::dot(oc, rd), disc = b * b - (glm::dot(oc, oc) - R * R);
	if (disc < 0.0f)
		return false;
	float s = std::sqrt(disc);
	t0 = std::max(0.0f, -b - s);
	t1 = -b + s;
	return t1 > t0;
}

// Ratio-tracking transmittance from p along dir through the cloud to its exit.
static float mcTransmittance(const glm::vec3 &p, const glm::vec3 &dir, const glm::vec3 &c, float R, float sigmaMax,
                             std::mt19937 &rng) {
	float t0, t1;
	if (!raySphereSpan(p, dir, c, R, t0, t1))
		return 1.0f;
	std::uniform_real_distribution<float> U(0.0f, 1.0f);
	float t = t0, Tr = 1.0f;
	while (true) {
		t -= std::log(1.0f - U(rng)) / sigmaMax;
		if (t >= t1)
			break;
		Tr *= 1.0f - mcCloudDensity(p + dir * t, c, R) / sigmaMax;
		if (Tr < 0.02f)
			break;
	}
	return Tr;
}

void renderVolumetricMC(const std::vector<ModelTriangle> &, const Camera &camera, Canvas &canvas,
                        const std::vector<Light> &lights, const Primitives &, float densityScale, int samples) {
	canvas.clearPixels();
	int W = static_cast<int>(canvas.width), H = static_cast<int>(canvas.height);
	float f = camera.focalLength * camera.scale;
	const glm::vec3 centre(0.0f), sunCol(1.7f, 1.55f, 1.35f);
	const float R = 1.3f, sigmaMax = densityScale;
	glm::vec3 sunDir = glm::normalize(glm::vec3(0.5f, 0.7f, 0.4f));
	if (!lights.empty() && lights[0].type == LightType::Directional)
		sunDir = glm::normalize(-lights[0].direction);

#pragma omp parallel for schedule(dynamic, 4)
	for (int y = 0; y < H; y++) {
		std::mt19937 rng(static_cast<unsigned>(y) * 9781u + 17u);
		std::uniform_real_distribution<float> U(0.0f, 1.0f);
		for (int x = 0; x < W; x++) {
			glm::vec3 accum(0.0f);
			for (int s = 0; s < samples; s++) {
				glm::vec3 ro, rd;
				camera.primaryRay(x + U(rng) - W / 2.0f, -(y + U(rng) - H / 2.0f), f, ro, rd);
				glm::vec3 sky = glm::mix(glm::vec3(0.75f, 0.85f, 1.0f), glm::vec3(0.35f, 0.55f, 0.9f),
				                         0.5f * (rd.y + 1.0f)); // background
				float t0, t1;
				if (!raySphereSpan(ro, rd, centre, R, t0, t1)) {
					accum += sky;
					continue;
				}
				// Delta tracking: step to a candidate collision, accept as a real
				// scatter with probability density/sigmaMax.
				float t = t0;
				bool scattered = false;
				glm::vec3 sp(0.0f);
				while (true) {
					t -= std::log(1.0f - U(rng)) / sigmaMax;
					if (t >= t1)
						break;
					glm::vec3 xp = ro + rd * t;
					if (U(rng) < mcCloudDensity(xp, centre, R) / sigmaMax) {
						scattered = true;
						sp = xp;
						break;
					}
				}
				if (!scattered) {
					accum += sky;
					continue;
				}
				// Single scatter to the sun (isotropic phase 1/4pi), plus a little
				// ambient sky so shadowed cloud is not pure black.
				float Tr = mcTransmittance(sp + sunDir * 1e-3f, sunDir, centre, R, sigmaMax, rng);
				glm::vec3 inscatter = sunCol * Tr * (1.0f / (4.0f * 3.14159265f)) * 12.0f;
				accum += inscatter + glm::vec3(0.28f, 0.33f, 0.42f);
			}
			glm::vec3 c = accum / static_cast<float>(samples) * 255.0f;
			int r = std::min(255, static_cast<int>(c.r)), g = std::min(255, static_cast<int>(c.g)),
			    b = std::min(255, static_cast<int>(c.b));
			canvas.setPixelColour(x, y, (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF));
		}
	}
}

// Sample a point in a unit lens aperture: a disk when blades < 3, else a regular
// polygon with `blades` sides (shaped bokeh). u1,u2 are uniform in [0,1).
static glm::vec2 sampleAperture(int blades, float u1, float u2) {
	if (blades < 3) {
		float ang = 2.0f * 3.14159265f * u1, rad = std::sqrt(u2);
		return glm::vec2(std::cos(ang), std::sin(ang)) * rad;
	}
	int wedge = std::min(blades - 1, static_cast<int>(u1 * blades)); // pick a triangular wedge
	float a0 = 2.0f * 3.14159265f * wedge / blades, a1 = 2.0f * 3.14159265f * (wedge + 1) / blades;
	glm::vec2 v0(std::cos(a0), std::sin(a0)), v1(std::cos(a1), std::sin(a1));
	float s = u1 * blades - wedge, t = u2; // uniform in the wedge triangle (origin, v0, v1)
	if (s + t > 1.0f) {
		s = 1.0f - s;
		t = 1.0f - t;
	}
	return v0 * s + v1 * t;
}

void renderPathTraced(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas, int samples,
                      const std::vector<Light> &lights, float aperture, float focusDistance,
                      const glm::vec3 &cameraMotion, const Primitives &prims, int apertureBlades) {
	canvas.clearPixels();
	int W = static_cast<int>(canvas.width);
	int H = static_cast<int>(canvas.height);
	float f = camera.focalLength * camera.scale;
	const int maxDepth = 4;
	Scene scene(model, prims);

	std::vector<Light> used = lights;
	if (used.empty()) {
		Light d;
		d.radius = 0.15f;
		used.push_back(d);
	}

#pragma omp parallel for schedule(dynamic, 4)
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			std::mt19937 rng(static_cast<unsigned>((y * W + x) * 9781 + 1)); // per-pixel seed = reproducible
			std::uniform_real_distribution<float> jitter(-0.5f, 0.5f);
			std::uniform_real_distribution<float> U(0.0f, 1.0f);
			glm::vec3 sum(0.0f);
			for (int s = 0; s < samples; s++) {
				// Jitter within the pixel: supersampling anti-aliasing for free.
				float jx = jitter(rng), jy = jitter(rng);
				// Primary ray (perspective, or parallel if the camera is orthographic).
				glm::vec3 rayO, direction;
				camera.primaryRay(x + 0.5f + jx - W / 2.0f, -(y + 0.5f + jy - H / 2.0f), f, rayO, direction);
				// Motion blur: jitter the camera along its motion vector over the shutter.
				glm::vec3 camPos = rayO + cameraMotion * U(rng);
				glm::vec3 origin = camPos;
				// Depth of field: sample a lens disk and re-aim at the focal point.
				if (aperture > 0.0f && !camera.orthographic) {
					glm::vec3 focalPoint = camPos + direction * focusDistance;
					glm::vec2 lens = sampleAperture(apertureBlades, U(rng), U(rng)) * aperture; // shaped bokeh
					glm::vec3 right = camera.orientation[0];
					glm::vec3 up = camera.orientation[1];
					origin = camPos + (right * lens.x + up * lens.y);
					direction = glm::normalize(focalPoint - origin);
				}
				sum += pathTrace(origin, direction, scene, used, maxDepth, rng);
			}
			glm::vec3 colour = sum / static_cast<float>(samples) * 255.0f;
			int r = std::min(255, static_cast<int>(colour.r));
			int g = std::min(255, static_cast<int>(colour.g));
			int b = std::min(255, static_cast<int>(colour.b));
			canvas.setPixelColour(x, y, (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF));
		}
	}
}

// --- Photon mapping ---------------------------------------------------------

// Trace one photon: reflect through mirrors/metal, reflect-or-refract through
// glass (which focuses caustics), and deposit it on the first diffuse surface,
// then optionally bounce (Russian roulette by albedo) to spread indirect light.
static void tracePhoton(const Scene &scene, const glm::vec3 &origin, const glm::vec3 &direction, const glm::vec3 &power,
                        int depth, std::vector<Photon> &out, std::mt19937 &rng) {
	if (depth < 0)
		return;
	RayTriangleIntersection hit = scene.intersect(origin, direction);
	if (!hit.hit)
		return;
	const ModelTriangle &tri = hit.intersectedTriangle;
	glm::vec3 point = hit.intersectionPoint;
	glm::vec3 n = faceViewer(tri.normal, direction);
	std::uniform_real_distribution<float> U(0.0f, 1.0f);

	if (tri.material == Material::Mirror) {
		glm::vec3 r = glm::reflect(direction, n);
		tracePhoton(scene, point + 1e-4f * r, r, power, depth - 1, out, rng);
		return;
	}
	if (tri.material == Material::Metal) {
		glm::vec3 albedo = surfaceBaseColour(hit, -direction) / 255.0f;
		glm::vec3 r = glm::reflect(direction, n);
		tracePhoton(scene, point + 1e-4f * r, r, power * albedo, depth - 1, out, rng);
		return;
	}
	if (tri.material == Material::Glass) {
		const float ior = 1.5f;
		glm::vec3 nn = tri.normal;
		float cosi = glm::dot(direction, nn);
		float etai = 1.0f, etat = ior;
		if (cosi < 0.0f)
			cosi = -cosi;
		else {
			std::swap(etai, etat);
			nn = -nn;
		}
		float r0 = (etai - etat) / (etai + etat);
		r0 *= r0;
		float fresnel = r0 + (1.0f - r0) * std::pow(1.0f - cosi, 5.0f);
		glm::vec3 refr = glm::refract(direction, nn, etai / etat);
		glm::vec3 d = (glm::length(refr) < 1e-6f || U(rng) < fresnel) ? glm::reflect(direction, nn) : refr;
		tracePhoton(scene, point + 1e-4f * d, d, power, depth - 1, out, rng);
		return;
	}

	out.push_back({point, power, direction}); // deposit on the diffuse surface
	glm::vec3 albedo = surfaceBaseColour(hit, -direction) / 255.0f;
	float p = std::max({albedo.r, albedo.g, albedo.b});
	if (depth > 0 && U(rng) < p) {
		glm::vec3 d = cosineHemisphere(n, rng);
		tracePhoton(scene, point + 1e-4f * d, d, power * albedo / p, depth - 1, out, rng);
	}
}

// Shade a camera ray using a prebuilt photon map: direct lighting plus the
// gathered photon irradiance (which carries indirect light and caustics). When
// gatherRays > 0 the indirect term uses a final gather (average the photon
// density one bounce away) instead of sampling the map at the visible point,
// which removes low-frequency photon-map blotches.
static glm::vec3 photonShade(const glm::vec3 &origin, const glm::vec3 &direction, const Scene &scene,
                             const std::vector<Light> &lights, const PhotonMap &map, float radius, int depth,
                             int gatherRays, std::mt19937 &rng) {
	RayTriangleIntersection hit = scene.intersect(origin, direction);
	if (!hit.hit)
		return environment(direction) / 255.0f;
	const ModelTriangle &tri = hit.intersectedTriangle;
	glm::vec3 point = hit.intersectionPoint;
	glm::vec3 n = faceViewer(tri.normal, direction);

	if (depth > 0 && tri.material == Material::Mirror) {
		glm::vec3 r = glm::reflect(direction, n);
		return photonShade(point + 1e-4f * r, r, scene, lights, map, radius, depth - 1, gatherRays, rng);
	}
	if (depth > 0 && tri.material == Material::Metal) {
		glm::vec3 albedo = surfaceBaseColour(hit, -direction) / 255.0f;
		glm::vec3 r = glm::reflect(direction, n);
		return albedo * photonShade(point + 1e-4f * r, r, scene, lights, map, radius, depth - 1, gatherRays, rng);
	}
	if (depth > 0 && tri.material == Material::Glass) {
		const float ior = 1.5f;
		glm::vec3 nn = tri.normal;
		float cosi = glm::dot(direction, nn);
		float etai = 1.0f, etat = ior;
		if (cosi < 0.0f)
			cosi = -cosi;
		else {
			std::swap(etai, etat);
			nn = -nn;
		}
		float r0 = (etai - etat) / (etai + etat);
		r0 *= r0;
		float fresnel = r0 + (1.0f - r0) * std::pow(1.0f - cosi, 5.0f);
		glm::vec3 reflected = glm::reflect(direction, nn);
		glm::vec3 reflCol =
		    photonShade(point + 1e-4f * reflected, reflected, scene, lights, map, radius, depth - 1, gatherRays, rng);
		glm::vec3 refr = glm::refract(direction, nn, etai / etat);
		if (glm::length(refr) < 1e-6f)
			return reflCol;
		glm::vec3 refrCol =
		    photonShade(point + 1e-4f * refr, refr, scene, lights, map, radius, depth - 1, gatherRays, rng);
		return fresnel * reflCol + (1.0f - fresnel) * refrCol;
	}

	glm::vec3 albedo = surfaceBaseColour(hit, -direction) / 255.0f;
	glm::vec3 direct = directLight(point, n, albedo, lights, scene, static_cast<int>(hit.triangleIndex));
	glm::vec3 indirect;
	if (gatherRays > 0) {
		// Final gather: bounce a batch of cosine-weighted rays and gather the
		// photon density where they land (or the sky), then average.
		glm::vec3 gathered(0.0f);
		for (int i = 0; i < gatherRays; i++) {
			glm::vec3 gdir = cosineHemisphere(n, rng);
			RayTriangleIntersection gh = scene.intersect(point + 1e-4f * gdir, gdir);
			if (gh.hit)
				gathered += map.gather(gh.intersectionPoint, faceViewer(gh.intersectedTriangle.normal, gdir), radius);
			else
				gathered += environment(gdir) / 255.0f;
		}
		indirect = albedo * gathered / static_cast<float>(gatherRays);
	} else {
		indirect = albedo * map.gather(point, n, radius);
	}
	return direct + indirect;
}

void renderPhotonMapped(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas, int numPhotons,
                        const std::vector<Light> &lights, const Primitives &prims, int gatherRays) {
	canvas.clearPixels();
	int W = static_cast<int>(canvas.width);
	int H = static_cast<int>(canvas.height);
	float f = camera.focalLength * camera.scale;
	const float radius = 0.25f;
	Scene scene(model, prims);

	std::vector<Light> used = lights;
	if (used.empty()) {
		Light d;
		d.radius = 0.15f;
		used.push_back(d);
	}

	// Pass 1: emit photons from the first light in all directions and trace them.
	std::vector<Photon> photons;
	std::mt19937 rng(777);
	std::uniform_real_distribution<float> U(0.0f, 1.0f);
	const Light &L = used[0];
	const float emitScale = 1.8f;
	for (int i = 0; i < numPhotons; i++) {
		float z = 1.0f - 2.0f * U(rng);
		float a = 2.0f * 3.14159265f * U(rng);
		float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
		glm::vec3 dir(r * std::cos(a), z, r * std::sin(a));
		glm::vec3 power = L.colour * (L.intensity * emitScale / numPhotons);
		tracePhoton(scene, L.position, dir, power, 5, photons, rng);
	}
	PhotonMap map;
	map.photons = std::move(photons);
	map.build(radius);

	// Pass 2: gather at each camera ray's diffuse hit (optionally final-gather).
#pragma omp parallel for schedule(dynamic, 4)
	for (int y = 0; y < H; y++) {
		std::mt19937 rng(1000 + y);
		for (int x = 0; x < W; x++) {
			glm::vec3 dirCamera((x - W / 2.0f) / f, -(y - H / 2.0f) / f, -1.0f);
			glm::vec3 direction = glm::normalize(camera.orientation * dirCamera);
			glm::vec3 colour =
			    photonShade(camera.position, direction, scene, used, map, radius, 4, gatherRays, rng) * 255.0f;
			int r = std::min(255, static_cast<int>(colour.r));
			int g = std::min(255, static_cast<int>(colour.g));
			int b = std::min(255, static_cast<int>(colour.b));
			canvas.setPixelColour(x, y, (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF));
		}
	}
}

// Post-process the rendered canvas: Reinhard tone-map (compresses highlights)
// followed by gamma correction.
void toneMap(Canvas &canvas, float exposure, float gamma) {
	for (uint32_t &pixel : canvas.pixels) {
		glm::vec3 c(static_cast<float>((pixel >> 16) & 0xFF), static_cast<float>((pixel >> 8) & 0xFF),
		            static_cast<float>(pixel & 0xFF));
		c = c / 255.0f * exposure;
		c = c / (c + glm::vec3(1.0f));                     // Reinhard
		c = glm::pow(c, glm::vec3(1.0f / gamma)) * 255.0f; // gamma
		int r = std::min(255, static_cast<int>(c.r));
		int g = std::min(255, static_cast<int>(c.g));
		int b = std::min(255, static_cast<int>(c.b));
		pixel = (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
	}
}

// Bloom: extract pixels brighter than `threshold` (0..1 of full white), blur
// that bright pass (repeated box blur ~ Gaussian), and add it back scaled by
// `intensity` so highlights glow into their surroundings.
AccumBuffer::AccumBuffer(int width, int height)
    : w_(width), h_(height), count_(0), sum_(static_cast<size_t>(width) * height, glm::vec3(0.0f)) {}

void AccumBuffer::add(const Canvas &canvas) {
	int n = w_ * h_;
	for (int i = 0; i < n; i++) {
		uint32_t p = canvas.pixels[static_cast<size_t>(i)];
		sum_[static_cast<size_t>(i)] += glm::vec3((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
	}
	count_++;
}

void AccumBuffer::resolve(Canvas &out) const {
	float inv = count_ > 0 ? 1.0f / count_ : 1.0f;
	for (int i = 0; i < w_ * h_; i++) {
		glm::vec3 c = sum_[static_cast<size_t>(i)] * inv;
		int r = std::min(255, static_cast<int>(c.r));
		int g = std::min(255, static_cast<int>(c.g));
		int b = std::min(255, static_cast<int>(c.b));
		out.pixels[static_cast<size_t>(i)] = (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
	}
}

void applyBloom(Canvas &canvas, float threshold, float intensity) {
	int W = static_cast<int>(canvas.width);
	int H = static_cast<int>(canvas.height);
	size_t n = static_cast<size_t>(W) * H;
	std::vector<glm::vec3> bright(n, glm::vec3(0.0f));
	for (size_t i = 0; i < n; i++) {
		uint32_t p = canvas.pixels[i];
		glm::vec3 c((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
		float luma = (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / 255.0f;
		if (luma > threshold)
			bright[i] = c * (luma - threshold) / (1.0f - threshold);
	}
	// Separable box blur, repeated 3x to approximate a wide Gaussian.
	const int radius = 6;
	std::vector<glm::vec3> tmp(n);
	auto blur = [&](std::vector<glm::vec3> &src, std::vector<glm::vec3> &dst, bool horizontal) {
		for (int y = 0; y < H; y++) {
			for (int x = 0; x < W; x++) {
				glm::vec3 sum(0.0f);
				int count = 0;
				for (int k = -radius; k <= radius; k++) {
					int sx = horizontal ? x + k : x;
					int sy = horizontal ? y : y + k;
					if (sx < 0 || sx >= W || sy < 0 || sy >= H)
						continue;
					sum += src[static_cast<size_t>(sy) * W + sx];
					count++;
				}
				dst[static_cast<size_t>(y) * W + x] = sum / static_cast<float>(count);
			}
		}
	};
	for (int pass = 0; pass < 3; pass++) {
		blur(bright, tmp, true);
		blur(tmp, bright, false);
	}
	for (size_t i = 0; i < n; i++) {
		uint32_t p = canvas.pixels[i];
		glm::vec3 c((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
		c += bright[i] * intensity;
		int r = std::min(255, static_cast<int>(c.r));
		int g = std::min(255, static_cast<int>(c.g));
		int b = std::min(255, static_cast<int>(c.b));
		canvas.pixels[i] = (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
	}
}

// A lightweight FXAA: blur pixels along high-contrast luma edges to soften
// jaggies without a full extra render pass.
void applyFXAA(Canvas &canvas) {
	const float edgeThreshold = 0.12f;
	std::vector<uint32_t> src = canvas.pixels;
	int W = static_cast<int>(canvas.width);
	int H = static_cast<int>(canvas.height);
	auto at = [&](int x, int y) {
		uint32_t p = src[static_cast<size_t>(y) * W + x];
		return glm::vec3((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
	};
	auto luma = [](const glm::vec3 &c) { return (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / 255.0f; };
	for (int y = 1; y < H - 1; y++) {
		for (int x = 1; x < W - 1; x++) {
			float lC = luma(at(x, y));
			float lN = luma(at(x, y - 1)), lS = luma(at(x, y + 1));
			float lW = luma(at(x - 1, y)), lE = luma(at(x + 1, y));
			float contrast = std::max({lC, lN, lS, lW, lE}) - std::min({lC, lN, lS, lW, lE});
			if (contrast < edgeThreshold)
				continue;
			glm::vec3 avg = (at(x, y - 1) + at(x, y + 1) + at(x - 1, y) + at(x + 1, y)) * 0.25f;
			float blend = std::min(0.5f, (contrast - edgeThreshold) * 2.0f);
			glm::vec3 c = glm::mix(at(x, y), avg, blend);
			int r = std::min(255, static_cast<int>(c.r));
			int g = std::min(255, static_cast<int>(c.g));
			int b = std::min(255, static_cast<int>(c.b));
			canvas.pixels[static_cast<size_t>(y) * W + x] =
			    (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
		}
	}
}
