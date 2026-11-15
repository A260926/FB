#include "BDPT.h"

#include "Scene.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace {

const float kPi = 3.14159265f;
const float kEps = 1e-3f;

// A path vertex on a diffuse surface (or the light).
struct Vertex {
	glm::vec3 position;
	glm::vec3 normal;
	glm::vec3 albedo;     // 0 on the light
	glm::vec3 throughput; // beta arriving at this vertex
	bool isLight = false;
};

glm::vec3 cosineHemisphere(const glm::vec3 &n, std::mt19937 &rng) {
	std::uniform_real_distribution<float> U(0.0f, 1.0f);
	float u1 = U(rng), u2 = U(rng);
	float r = std::sqrt(u1);
	float theta = 2.0f * kPi * u2;
	float x = r * std::cos(theta), y = r * std::sin(theta), z = std::sqrt(std::max(0.0f, 1.0f - u1));
	glm::vec3 up = std::abs(n.z) < 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
	glm::vec3 t = glm::normalize(glm::cross(up, n));
	glm::vec3 b = glm::cross(n, t);
	return glm::normalize(t * x + b * y + n * z);
}

glm::vec3 faceViewer(const glm::vec3 &n, const glm::vec3 &dir) {
	return glm::dot(n, dir) < 0.0f ? n : -n;
}

glm::vec3 albedoOf(const ModelTriangle &tri) {
	return glm::vec3(tri.colour.red, tri.colour.green, tri.colour.blue) / 255.0f;
}

// Extend a diffuse subpath, appending surface vertices as the ray bounces.
void tracePath(const Scene &scene, glm::vec3 origin, glm::vec3 dir, glm::vec3 beta, int maxDepth,
               std::vector<Vertex> &out, std::mt19937 &rng) {
	for (int depth = 0; depth < maxDepth; depth++) {
		RayTriangleIntersection hit = scene.intersect(origin, dir);
		if (!hit.hit)
			return;
		const ModelTriangle &tri = hit.intersectedTriangle;
		Vertex v;
		v.position = hit.intersectionPoint;
		v.normal = faceViewer(tri.normal, dir);
		v.albedo = albedoOf(tri);
		v.throughput = beta;
		out.push_back(v);
		glm::vec3 nd = cosineHemisphere(v.normal, rng);
		beta *= v.albedo; // cosine sampling cancels cos/pdf
		origin = v.position + kEps * nd;
		dir = nd;
	}
}

} // namespace

void renderBidirectional(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas, int samples,
                         float exposure) {
	canvas.clearPixels();
	int W = static_cast<int>(canvas.width);
	int H = static_cast<int>(canvas.height);
	float f = camera.focalLength * camera.scale;

	Primitives noPrims;
	Scene scene(model, noPrims);

	// Ceiling area light (disk sampled as a square patch), facing down.
	const glm::vec3 lightCenter(0.0f, 0.33f, 0.0f);
	const glm::vec3 lightNormal(0.0f, -1.0f, 0.0f);
	const float lightHalf = 0.18f;
	const float lightArea = (2.0f * lightHalf) * (2.0f * lightHalf);
	const glm::vec3 Le(30.0f); // emitted radiance
	const int maxDepth = 4;

#pragma omp parallel for schedule(dynamic, 4)
	for (int y = 0; y < H; y++) {
		std::mt19937 rng(9871 + y);
		std::uniform_real_distribution<float> U(0.0f, 1.0f);
		for (int x = 0; x < W; x++) {
			glm::vec3 accum(0.0f);
			for (int s = 0; s < samples; s++) {
				// Camera subpath.
				glm::vec3 dirCamera((x + U(rng) - W / 2.0f) / f, -(y + U(rng) - H / 2.0f) / f, -1.0f);
				glm::vec3 direction = glm::normalize(camera.orientation * dirCamera);
				std::vector<Vertex> camPath;
				tracePath(scene, camera.position, direction, glm::vec3(1.0f), maxDepth, camPath, rng);

				// Light subpath, starting from a sampled point on the emitter.
				std::vector<Vertex> lightPath;
				glm::vec3 lp =
				    lightCenter + glm::vec3((U(rng) * 2 - 1) * lightHalf, 0.0f, (U(rng) * 2 - 1) * lightHalf);
				Vertex l0;
				l0.position = lp;
				l0.normal = lightNormal;
				l0.albedo = glm::vec3(0.0f);
				l0.throughput = Le * lightArea; // emitted importance for connections
				l0.isLight = true;
				lightPath.push_back(l0);
				glm::vec3 ldir = cosineHemisphere(lightNormal, rng);
				tracePath(scene, lp + kEps * ldir, ldir, Le * lightArea, maxDepth, lightPath, rng);

				// Connect every camera vertex to every light vertex.
				for (size_t i = 0; i < camPath.size(); i++) {
					const Vertex &c = camPath[i];
					for (size_t j = 0; j < lightPath.size(); j++) {
						const Vertex &l = lightPath[j];
						glm::vec3 d = l.position - c.position;
						float dist2 = glm::dot(d, d);
						float dist = std::sqrt(dist2);
						d /= dist;
						float cosC = glm::dot(c.normal, d);
						float cosL = glm::dot(l.normal, -d);
						if (cosC <= 0.0f || cosL <= 0.0f)
							continue;
						if (scene.occluded(c.position + c.normal * kEps, d, dist - 2.0f * kEps, -1))
							continue;
						float G = cosC * cosL / dist2;
						glm::vec3 fC = c.albedo / kPi;
						glm::vec3 contrib = c.throughput * fC * G * l.throughput;
						if (!l.isLight)
							contrib *= l.albedo / kPi;
						// Uniform MIS weight over the strategies for this path length.
						float weight = 1.0f / static_cast<float>(i + j + 2);
						accum += contrib * weight;
					}
				}
			}
			glm::vec3 col = accum * (exposure / static_cast<float>(samples));
			col = 255.0f * (col / (col + glm::vec3(1.0f))); // Reinhard
			int r = std::min(255, static_cast<int>(col.r));
			int g = std::min(255, static_cast<int>(col.g));
			int b = std::min(255, static_cast<int>(col.b));
			canvas.setPixelColour(x, y, (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF));
		}
	}
}
