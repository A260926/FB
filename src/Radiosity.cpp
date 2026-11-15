#include "Radiosity.h"

#include "Scene.h"
#include <algorithm>
#include <cmath>
#include <random>

// Recursively split a triangle into 4 by edge midpoints, down to `level` 0.
static void subdivideTri(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c, const Colour &col, int level,
                         std::vector<ModelTriangle> &out) {
	if (level <= 0) {
		ModelTriangle t;
		t.vertices = {a, b, c};
		t.colour = col;
		glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
		t.normal = n;
		t.vertexNormals = {n, n, n};
		out.push_back(t);
		return;
	}
	glm::vec3 ab = (a + b) * 0.5f, bc = (b + c) * 0.5f, ca = (c + a) * 0.5f;
	subdivideTri(a, ab, ca, col, level - 1, out);
	subdivideTri(ab, b, bc, col, level - 1, out);
	subdivideTri(ca, bc, c, col, level - 1, out);
	subdivideTri(ab, bc, ca, col, level - 1, out);
}

// Cosine-weighted hemisphere sample about n (so averaging over samples estimates
// the (1/pi) integral of B cos(theta), i.e. the radiosity gather).
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

void renderRadiosity(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas, int subdivLevel,
                     int iterations, int samples, bool progressive) {
	canvas.clearPixels();

	// 1. Subdivide the scene into patches.
	std::vector<ModelTriangle> patches;
	for (const ModelTriangle &t : model)
		subdivideTri(t.vertices[0], t.vertices[1], t.vertices[2], t.colour, subdivLevel, patches);

	// 2. Add a downward-facing emitter panel just below the ceiling.
	int emitStart = static_cast<int>(patches.size());
	auto addEmitter = [&](const glm::vec3 &A, const glm::vec3 &B, const glm::vec3 &C) {
		ModelTriangle t;
		t.vertices = {A, B, C};
		t.colour = Colour(255, 255, 255);
		glm::vec3 n = glm::normalize(glm::cross(B - A, C - A));
		if (n.y > 0.0f) // force the normal to face down into the room
			n = -n;
		t.normal = n;
		t.vertexNormals = {n, n, n};
		patches.push_back(t);
	};
	const float ey = 0.32f;
	addEmitter({-0.3f, ey, -0.3f}, {0.3f, ey, -0.3f}, {0.3f, ey, 0.3f});
	addEmitter({-0.3f, ey, -0.3f}, {0.3f, ey, 0.3f}, {-0.3f, ey, 0.3f});

	int N = static_cast<int>(patches.size());
	std::vector<glm::vec3> B(N), E(N, glm::vec3(0.0f)), albedo(N), centroid(N), normal(N);
	for (int i = 0; i < N; i++) {
		const ModelTriangle &t = patches[i];
		albedo[i] = glm::vec3(t.colour.red, t.colour.green, t.colour.blue) / 255.0f;
		centroid[i] = (t.vertices[0] + t.vertices[1] + t.vertices[2]) / 3.0f;
		normal[i] = t.normal;
	}
	const float emission = 20.0f;
	for (int i = emitStart; i < N; i++) {
		E[i] = glm::vec3(emission);
		albedo[i] = glm::vec3(0.0f); // the light does not reflect
	}
	B = E;

	Primitives noPrims; // must outlive `scene` (Scene keeps a reference)
	Scene scene(patches, noPrims);

	// 3a. Progressive (shooting) radiosity: repeatedly shoot the patch holding the
	// most unshot energy to every other patch, weighted by a centroid-to-centroid
	// form factor (cos*cos / pi*d^2, visibility-tested). Converges progressively,
	// unlike solving the whole system - the classic Cohen-Greenberg method.
	if (progressive) {
		std::vector<glm::vec3> residual = E;
		const float patchArea = 420.0f / static_cast<float>(N); // rough uniform patch area
		int shoots = iterations * 60;
		for (int s = 0; s < shoots; s++) {
			int src = -1;
			float best = 1e-4f;
			for (int i = 0; i < N; i++) {
				float p = residual[i].r + residual[i].g + residual[i].b;
				if (p > best) {
					best = p;
					src = i;
				}
			}
			if (src < 0)
				break;
			glm::vec3 dB = residual[src];
			residual[src] = glm::vec3(0.0f);
#pragma omp parallel for schedule(dynamic, 64)
			for (int j = 0; j < N; j++) {
				if (j == src || j >= emitStart)
					continue;
				glm::vec3 d = centroid[j] - centroid[src];
				float dist = glm::length(d);
				if (dist < 1e-4f)
					continue;
				d /= dist;
				float cosS = glm::dot(normal[src], d), cosJ = glm::dot(normal[j], -d);
				if (cosS <= 0.0f || cosJ <= 0.0f)
					continue;
				if (scene.occluded(centroid[src] + normal[src] * 1e-3f, d, dist - 2e-3f, src))
					continue;
				float form = cosS * cosJ / (3.14159265f * dist * dist) * patchArea;
				glm::vec3 delta = albedo[j] * dB * form;
				B[j] += delta;
				residual[j] += delta;
			}
		}
	} else {

		// 3b. Jacobi gathering sweeps.
		for (int iter = 0; iter < iterations; iter++) {
			std::vector<glm::vec3> next = E;
#pragma omp parallel for schedule(dynamic, 16)
			for (int i = 0; i < emitStart; i++) {
				std::mt19937 rng(static_cast<unsigned>(i) * 2654435761u + static_cast<unsigned>(iter) * 40503u + 1u);
				glm::vec3 origin = centroid[i] + normal[i] * 1e-3f;
				glm::vec3 incoming(0.0f);
				for (int s = 0; s < samples; s++) {
					glm::vec3 dir = cosineHemisphere(normal[i], rng);
					RayTriangleIntersection h = scene.intersect(origin, dir, i);
					if (h.hit && h.triangleIndex < static_cast<size_t>(N))
						incoming += B[h.triangleIndex];
				}
				next[i] = E[i] + albedo[i] * (incoming / static_cast<float>(samples));
			}
			B = next;
		}
	}

	// 4. Render: each camera ray shows its hit patch's radiosity (Reinhard-mapped).
	int W = static_cast<int>(canvas.width);
	int H = static_cast<int>(canvas.height);
	float f = camera.focalLength * camera.scale;
#pragma omp parallel for schedule(dynamic, 4)
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			glm::vec3 dirCamera((x - W / 2.0f) / f, -(y - H / 2.0f) / f, -1.0f);
			glm::vec3 direction = glm::normalize(camera.orientation * dirCamera);
			RayTriangleIntersection h = scene.intersect(camera.position, direction);
			glm::vec3 col(0.0f);
			if (h.hit && h.triangleIndex < static_cast<size_t>(N)) {
				glm::vec3 b = B[h.triangleIndex];
				col = 255.0f * (b / (b + glm::vec3(1.0f))); // Reinhard tone-map
			}
			int r = std::min(255, static_cast<int>(col.r));
			int g = std::min(255, static_cast<int>(col.g));
			int bl = std::min(255, static_cast<int>(col.b));
			canvas.setPixelColour(x, y, (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (bl & 0xFF));
		}
	}
}
