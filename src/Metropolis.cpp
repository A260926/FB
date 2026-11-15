#include "Metropolis.h"

#include "Scene.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace {

const float kPi = 3.14159265f;
const float kEps = 1e-3f;

// Scene light (matches the bidirectional tracer's ceiling emitter).
const glm::vec3 kLightCenter(0.0f, 0.33f, 0.0f);
const glm::vec3 kLightNormal(0.0f, -1.0f, 0.0f);
const float kLightHalf = 0.18f;
const float kLightArea = (2.0f * kLightHalf) * (2.0f * kLightHalf);
const glm::vec3 kLe(30.0f);
const int kMaxDepth = 4;

float luminance(const glm::vec3 &c) {
	return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

// A lazily-extended stream of primary-space random numbers (the "sample").
struct Sampler {
	std::vector<float> *u;
	size_t idx = 0;
	std::mt19937 *rng;
	float next() {
		std::uniform_real_distribution<float> U(0.0f, 1.0f);
		if (idx >= u->size())
			u->push_back(U(*rng));
		return (*u)[idx++];
	}
};

glm::vec3 faceViewer(const glm::vec3 &n, const glm::vec3 &dir) {
	return glm::dot(n, dir) < 0.0f ? n : -n;
}

glm::vec3 cosineHemisphere(const glm::vec3 &n, float u1, float u2) {
	float r = std::sqrt(u1);
	float theta = 2.0f * kPi * u2;
	float x = r * std::cos(theta), y = r * std::sin(theta), z = std::sqrt(std::max(0.0f, 1.0f - u1));
	glm::vec3 up = std::abs(n.z) < 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
	glm::vec3 t = glm::normalize(glm::cross(up, n));
	glm::vec3 b = glm::cross(n, t);
	return glm::normalize(t * x + b * y + n * z);
}

// Diffuse path tracer with next-event estimation, driven by the sampler.
glm::vec3 tracePath(Sampler &smp, const Scene &scene, const glm::vec3 &rayOrigin, const glm::vec3 &rayDir) {
	glm::vec3 L(0.0f), beta(1.0f);
	glm::vec3 origin = rayOrigin, dir = rayDir;
	for (int depth = 0; depth < kMaxDepth; depth++) {
		RayTriangleIntersection hit = scene.intersect(origin, dir);
		if (!hit.hit)
			break;
		const ModelTriangle &tri = hit.intersectedTriangle;
		glm::vec3 n = faceViewer(tri.normal, dir);
		glm::vec3 albedo = glm::vec3(tri.colour.red, tri.colour.green, tri.colour.blue) / 255.0f;
		glm::vec3 point = hit.intersectionPoint;
		// Next-event estimation to the ceiling light.
		glm::vec3 lp =
		    kLightCenter + glm::vec3((smp.next() * 2 - 1) * kLightHalf, 0.0f, (smp.next() * 2 - 1) * kLightHalf);
		glm::vec3 toL = lp - point;
		float dist = glm::length(toL);
		toL /= dist;
		float cosS = glm::dot(n, toL), cosL = glm::dot(kLightNormal, -toL);
		if (cosS > 0.0f && cosL > 0.0f && !scene.occluded(point + n * kEps, toL, dist - 2.0f * kEps, -1)) {
			float G = cosS * cosL / (dist * dist);
			L += beta * (albedo / kPi) * G * kLe * kLightArea;
		}
		glm::vec3 nd = cosineHemisphere(n, smp.next(), smp.next());
		beta *= albedo;
		origin = point + kEps * nd;
		dir = nd;
	}
	return L;
}

} // namespace

void renderMetropolis(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas, int mutations,
                      float exposure) {
	canvas.clearPixels();
	int W = static_cast<int>(canvas.width);
	int H = static_cast<int>(canvas.height);
	float f = camera.focalLength * camera.scale;
	Primitives noPrims;
	Scene scene(model, noPrims);

	// Evaluate F(u): choose a pixel from the first two randoms, trace, return
	// the pixel index and radiance.
	auto eval = [&](std::vector<float> &u, std::mt19937 &rng, int &outPixel) -> glm::vec3 {
		Sampler smp{&u, 0, &rng};
		float fx = smp.next(), fy = smp.next();
		int px = std::min(W - 1, static_cast<int>(fx * W));
		int py = std::min(H - 1, static_cast<int>(fy * H));
		outPixel = py * W + px;
		glm::vec3 dirCamera((px + 0.5f - W / 2.0f) / f, -(py + 0.5f - H / 2.0f) / f, -1.0f);
		glm::vec3 direction = glm::normalize(camera.orientation * dirCamera);
		return tracePath(smp, scene, camera.position, direction);
	};

	// Bootstrap: estimate the normalisation b = average scalar contribution.
	double bSum = 0.0;
	const int bootstrap = 100000;
	{
		std::mt19937 rng(12345);
		for (int i = 0; i < bootstrap; i++) {
			std::vector<float> u;
			int pixel;
			bSum += luminance(eval(u, rng, pixel));
		}
	}
	float b = static_cast<float>(bSum / bootstrap);
	if (b <= 0.0f)
		return;

	const int numChains = 16;
	const float sigma = 0.02f; // small-step size
	const float pLarge = 0.3f; // large-step probability
	std::vector<glm::vec3> image(static_cast<size_t>(W) * H, glm::vec3(0.0f));

#pragma omp parallel for schedule(dynamic, 1)
	for (int chain = 0; chain < numChains; chain++) {
		std::mt19937 rng(777 + chain * 2654435761u);
		std::uniform_real_distribution<float> U(0.0f, 1.0f);
		std::normal_distribution<float> Nrm(0.0f, 1.0f);
		std::vector<glm::vec3> local(static_cast<size_t>(W) * H, glm::vec3(0.0f));

		// Seed the chain in a nonzero-contribution state.
		std::vector<float> cur;
		int curPixel = 0;
		glm::vec3 curL(0.0f);
		float curLum = 0.0f;
		for (int tries = 0; tries < 256 && curLum <= 0.0f; tries++) {
			cur.clear();
			curL = eval(cur, rng, curPixel);
			curLum = luminance(curL);
		}

		int steps = mutations / numChains;
		for (int s = 0; s < steps; s++) {
			bool large = U(rng) < pLarge;
			std::vector<float> prop = cur;
			if (large) {
				for (float &v : prop)
					v = U(rng);
			} else {
				for (float &v : prop) {
					v += sigma * Nrm(rng);
					v -= std::floor(v); // wrap into [0,1)
				}
			}
			int propPixel;
			glm::vec3 propL = eval(prop, rng, propPixel);
			float propLum = luminance(propL);

			float a = (curLum > 0.0f) ? std::min(1.0f, propLum / curLum) : 1.0f;
			if (propLum > 0.0f)
				local[propPixel] += propL * (a / propLum);
			if (curLum > 0.0f)
				local[curPixel] += curL * ((1.0f - a) / curLum);
			if (U(rng) < a) {
				cur = prop;
				curPixel = propPixel;
				curL = propL;
				curLum = propLum;
			}
		}
#pragma omp critical
		for (size_t i = 0; i < image.size(); i++)
			image[i] += local[i];
	}

	// Normalise: each step deposits one unit of luminance mass, so scaling by
	// b * numPixels / totalSteps makes the average image luminance equal b.
	float scale = b * static_cast<float>(W) * H / static_cast<float>(mutations);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			glm::vec3 c = image[static_cast<size_t>(y) * W + x] * scale * exposure;
			c = 255.0f * (c / (c + glm::vec3(1.0f))); // Reinhard
			int r = std::min(255, static_cast<int>(c.r));
			int g = std::min(255, static_cast<int>(c.g));
			int bl = std::min(255, static_cast<int>(c.b));
			canvas.setPixelColour(x, y, (255u << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (bl & 0xFF));
		}
	}
}
