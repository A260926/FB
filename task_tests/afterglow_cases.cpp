// Task cases for the afterglow path tracer. Registered with the C harness in
// task_support/check.h (which supplies main); this file defines no main.
extern "C" {
#include "task_support/check.h"
}

#include "Afterglow.h"
#include "Camera.h"
#include "Light.h"
#include "Renderer.h"
#include "Scene.h"
#include <Canvas.h>
#include <Colour.h>
#include <ModelTriangle.h>
#include <glm/glm.hpp>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr double kInf = std::numeric_limits<double>::infinity();

bool closeDouble(double a, double b, double tol = 1e-9) {
	return std::abs(a - b) <= tol * std::max(1.0, std::max(std::abs(a), std::abs(b)));
}
bool closeVec(const glm::dvec3 &a, const glm::dvec3 &b, double tol = 1e-9) {
	return closeDouble(a.x, b.x, tol) && closeDouble(a.y, b.y, tol) && closeDouble(a.z, b.z, tol);
}

ModelTriangle makeTri(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c, const Colour &colour,
                      Material material = Material::Diffuse, double tauNs = 0.0) {
	ModelTriangle tri(a, b, c, colour);
	tri.material = material;
	const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
	tri.normal = normal;
	tri.vertexNormals = {normal, normal, normal};
	tri.afterglowTauNs = tauNs;
	return tri;
}

void addQuad(std::vector<ModelTriangle> &mesh, const glm::vec3 &p0, const glm::vec3 &p1, const glm::vec3 &p2,
             const glm::vec3 &p3, const Colour &colour, Material material = Material::Diffuse, double tauNs = 0.0) {
	mesh.push_back(makeTri(p0, p1, p2, colour, material, tauNs));
	mesh.push_back(makeTri(p0, p2, p3, colour, material, tauNs));
}

Light pointLight(const glm::vec3 &position, const glm::vec3 &colour, float intensity) {
	Light L;
	L.type = LightType::Point;
	L.position = position;
	L.colour = colour;
	L.intensity = intensity;
	L.radius = 0.0f;
	return L;
}

// A closed diffuse cube centred on the origin, each face carrying its own tau.
std::vector<ModelTriangle> closedBox(double half, const Colour &colour, double tauNs) {
	std::vector<ModelTriangle> mesh;
	const double h = half;
	addQuad(mesh, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}, colour, Material::Diffuse, tauNs); // floor
	addQuad(mesh, {-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h}, colour, Material::Diffuse, tauNs);      // ceiling
	addQuad(mesh, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {h, -h, -h}, colour, Material::Diffuse, tauNs);  // back
	addQuad(mesh, {h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h}, colour, Material::Diffuse, tauNs);      // right
	addQuad(mesh, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, colour, Material::Diffuse, tauNs);      // front
	addQuad(mesh, {-h, h, -h}, {-h, -h, -h}, {-h, -h, h}, {-h, h, h}, colour, Material::Diffuse, tauNs);  // left
	return mesh;
}

const std::vector<double> kEdges = {0.5, 2.0, 8.0, 30.0};
} // namespace

// ============================= response group ==============================

static int responseSingleExponential() {
	const double tau = 3.5;
	const TemporalResponse r(std::vector<double>{tau});
	TASK_CHECK(r.size() == 1);
	TASK_CHECK(closeDouble(r.mean(), tau));
	for (double t : {0.0, 0.2, 1.0, 3.5, 9.0, 40.0}) {
		const double want = 1.0 - std::exp(-t / tau);
		TASK_CHECK(closeDouble(r.cdf(t), want, 1e-11));
	}
	TASK_CHECK(closeDouble(r.mass(-kInf, kInf), 1.0, 1e-12));
	TASK_CHECK(closeDouble(r.mass(1.0, 1.0), 0.0));
	TASK_CHECK(closeDouble(r.mass(-10.0, 0.0), 0.0));
	TASK_CHECK(closeDouble(r.mass(-kInf, 0.0), 0.0));
	return 0;
}

static int responseErlangRepeatedLifetimes() {
	const double tau = 2.0;
	const int k = 4;
	const TemporalResponse r(std::vector<double>(k, tau));
	TASK_CHECK(r.size() == static_cast<std::size_t>(k));
	for (double t : {0.3, 1.5, 4.0, 12.0, 40.0}) {
		const double x = t / tau;
		const double want = 1.0 - std::exp(-x) * (1.0 + x + x * x / 2.0 + x * x * x / 6.0);
		TASK_CHECK(closeDouble(r.cdf(t), want, 1e-10));
	}
	// Appending one at a time must build the same Erlang response.
	TemporalResponse stepwise;
	for (int i = 0; i < k; ++i)
		stepwise = stepwise.append(tau);
	for (double t : {1.0, 7.0, 20.0})
		TASK_CHECK(closeDouble(stepwise.cdf(t), r.cdf(t), 1e-12));
	return 0;
}

static int responseTwoDistinctFormula() {
	const double t1 = 1.5, t2 = 3.7;
	const TemporalResponse r(std::vector<double>{t1, t2});
	for (double t : {0.3, 1.0, 2.0, 5.0, 20.0}) {
		const double want = 1.0 - (t2 * std::exp(-t / t2) - t1 * std::exp(-t / t1)) / (t2 - t1);
		TASK_CHECK(closeDouble(r.cdf(t), want, 1e-10));
	}
	// Order of kernels cannot matter (convolution commutes).
	const TemporalResponse swapped(std::vector<double>{t2, t1});
	for (double t : {0.7, 4.4, 17.0})
		TASK_CHECK(closeDouble(swapped.cdf(t), r.cdf(t), 1e-12));
	return 0;
}

static int responseNearEqualLifetimes() {
	// Relative difference exactly 1e-10: no negative mass, no NaN, no energy drift.
	const double tauA = 2.5;
	for (double rel : {1e-8, 1e-9, 1e-10, 1e-11}) {
		const TemporalResponse r(std::vector<double>{tauA, tauA * (1.0 + rel)});
		double total = 0.0;
		double prev = -kInf;
		for (int i = 0; i <= 400; ++i) {
			const double edge =
			    (i == 400) ? kInf : (i == 0 ? -kInf : std::pow(10.0, -3.0 + (std::log10(200.0) + 3.0) * (i - 1) / 398.0));
			const double m = r.mass(prev, edge);
			TASK_CHECK(!std::isnan(m));
			TASK_CHECK(m >= 0.0);
			TASK_CHECK(m <= 1.0 + 1e-12);
			total += m;
			prev = edge;
		}
		TASK_CHECK(closeDouble(total, 1.0, 1e-8));
	}
	return 0;
}

static int responseZeroAndNegativeTime() {
	// Zero lifetimes add no delay: the chain stays a unit atom at time 0.
	TemporalResponse r(std::vector<double>{0.0, 0.0, 5.0, 0.0});
	TASK_CHECK(r.size() == 4);
	TASK_CHECK(closeDouble(r.mean(), 5.0));
	const TemporalResponse impulse(std::vector<double>{0.0, 0.0});
	TASK_CHECK(closeDouble(impulse.mass(-kInf, 0.0), 0.0));
	TASK_CHECK(closeDouble(impulse.mass(0.0, 1.0), 1.0));
	TASK_CHECK(closeDouble(impulse.mass(-1.0, kInf), 1.0));
	TASK_CHECK(closeDouble(impulse.mass(0.0, 0.0), 0.0));
	// Causal support: nothing arrives before time 0.
	const TemporalResponse positive(std::vector<double>{1.0, 2.0});
	TASK_CHECK(closeDouble(positive.cdf(-1e-12), 0.0));
	TASK_CHECK(closeDouble(positive.mass(-kInf, -1.0), 0.0));
	return 0;
}

static int responseLongTailAndValidation() {
	const TemporalResponse small(std::vector<double>{1e-6});
	TASK_CHECK(closeDouble(small.cdf(1e-3), 1.0 - std::exp(-1000.0), 1e-12));
	const TemporalResponse big(std::vector<double>{1e6});
	TASK_CHECK(closeDouble(big.cdf(1e6), 1.0 - std::exp(-1.0), 1e-10));
	// A 64-kernel chain still conserves mass over its whole support.
	const TemporalResponse r64(std::vector<double>(64, 0.7));
	TASK_CHECK(r64.size() == 64);
	TASK_CHECK(closeDouble(r64.mass(-kInf, kInf), 1.0, 1e-8));
	TASK_CHECK(closeDouble(r64.mean(), 64 * 0.7, 1e-12));

	bool threw = false;
	try {
		TemporalResponse bad(std::vector<double>{1e-7});
	} catch (...) {
		threw = true;
	}
	TASK_CHECK(threw);
	threw = false;
	try {
		TemporalResponse bad(std::vector<double>{1.0e7});
	} catch (...) {
		threw = true;
	}
	TASK_CHECK(threw);
	threw = false;
	try {
		TemporalResponse bad(std::vector<double>(65, 1.0));
	} catch (...) {
		threw = true;
	}
	TASK_CHECK(threw);
	threw = false;
	try {
		TemporalResponse ok;
		ok.append(std::nan(""));
	} catch (...) {
		threw = true;
	}
	TASK_CHECK(threw);
	threw = false;
	try {
		TemporalResponse ok;
		ok.mass(1.0, 0.0);
	} catch (...) {
		threw = true;
	}
	TASK_CHECK(threw);
	threw = false;
	try {
		TemporalResponse ok;
		ok.mass(std::nan(""), 1.0);
	} catch (...) {
		threw = true;
	}
	TASK_CHECK(threw);

	// Prefix immutability: append returns a new response and leaves the base.
	TemporalResponse base;
	const TemporalResponse extended = base.append(3.0);
	TASK_CHECK(base.size() == 0);
	TASK_CHECK(extended.size() == 1);
	TASK_CHECK(extended.tauAt(0) == 3.0);
	return 0;
}

// ============================= transport group =============================

static int transportEnvironmentEscape() {
	// A ray that misses everything yields one environment contribution with no
	// finite arrival timestamp and zero in-window energy.
	std::vector<ModelTriangle> mesh = {makeTri({-1, -1, 0}, {1, -1, 0}, {0, 1, 0}, Colour(100, 100, 100))};
	Primitives prim;
	const Scene scene(mesh, prim);
	const std::vector<Light> lights = {pointLight({0, 0, 3}, {1, 1, 1}, 10.0f)};
	std::mt19937 rng(7);
	const AfterglowRayResult ray = pathTraceAfterglowRay({0, 0, 2}, {0, 1, 0}, scene, lights, kEdges, 1.0, rng);
	TASK_CHECK(ray.contributions.size() == 1);
	TASK_CHECK(ray.contributions[0].environment);
	TASK_CHECK(closeVec(ray.environment, ray.steady));
	for (const auto &bin : ray.bins)
		TASK_CHECK(closeVec(bin, glm::dvec3(0.0)));
	TASK_CHECK(closeVec(ray.beforeWindow + ray.afterWindow, glm::dvec3(0.0)));
	return 0;
}

static int transportFlightTimeAndImpulse() {
	// Wall at z = 0, camera at z = 4, light at z = 2 on the ray axis. With an
	// instantaneous coating the direct contribution lands at exactly
	// (4 + 2) * mpu / c * 1e9 ns.
	std::vector<ModelTriangle> mesh = {
	    makeTri({-2, -2, 0}, {2, -2, 0}, {2, 2, 0}, Colour(255, 255, 255)),
	    makeTri({-2, -2, 0}, {2, 2, 0}, {-2, 2, 0}, Colour(255, 255, 255))};
	Primitives prim;
	const Scene scene(mesh, prim);
	const double mpu = 0.5;
	const Light light = pointLight({0, 0, 2}, {1, 1, 1}, 40.0f);
	const double expectedNs = 6.0 * mpu / kSpeedOfLight * 1e9;
	const std::vector<double> edges = {expectedNs * 0.25, expectedNs * 0.75, expectedNs * 1.25, expectedNs * 4.0};
	std::mt19937 rng(11);
	const AfterglowRayResult ray = pathTraceAfterglowRay({0, 0, 4}, {0, 0, -1}, scene, {light}, edges, mpu, rng);
	const AfterglowContribution *direct = nullptr;
	for (const auto &c : ray.contributions)
		if (!c.environment && c.response.size() == 1)
			direct = &c;
	TASK_CHECK(direct != nullptr);
	TASK_CHECK(closeDouble(direct->flightNs, expectedNs, 1e-10));
	// Impulse response: all of this contribution sits in the bin straddling it.
	TASK_CHECK(closeDouble(ray.bins[1].r, direct->rgb.r, 1e-12));
	TASK_CHECK(closeVec(ray.beforeWindow, glm::dvec3(0.0), 1e-12));
	return 0;
}

static int transportMirrorTurnAndEmissive() {
	// Mirror at the origin bounces the ray straight back to an emissive panel
	// behind the camera. The path is deterministic and consumes no RNG.
	std::vector<ModelTriangle> mesh;
	addQuad(mesh, {-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}, Colour(220, 220, 220), Material::Mirror);
	addQuad(mesh, {-1, -1, 6}, {-1, 1, 6}, {1, 1, 6}, {1, -1, 6}, Colour(255, 128, 0), Material::Emissive);
	Primitives prim;
	const Scene scene(mesh, prim);
	const std::vector<Light> lights = {pointLight({0, 0, 3}, {1, 1, 1}, 5.0f)};
	std::mt19937 a(3), b(3);
	const AfterglowRayResult ray = pathTraceAfterglowRay({0, 0, 4}, {0, 0, -1}, scene, lights, kEdges, 1.0, a);
	const glm::dvec3 steady = pathTraceSteadyRay({0, 0, 4}, {0, 0, -1}, scene, lights, b);
	std::stringstream sa, sb;
	sa << a;
	sb << b;
	TASK_CHECK(sa.str() == sb.str()); // temporal bookkeeping draws no RNG
	TASK_CHECK(ray.contributions.size() == 1);
	TASK_CHECK(!ray.contributions[0].environment);
	TASK_CHECK(ray.contributions[0].response.size() == 0); // mirrors add no dwell kernel
	// The engine shades in float, so evaluate the expectation in float too.
	const glm::dvec3 want(glm::vec3(255, 128, 0) / 255.0f * 4.0f);
	TASK_CHECK(closeVec(ray.steady, want, 1e-6));
	TASK_CHECK(closeVec(steady, want, 1e-6));
	// Total geometric length is 4 (camera->mirror) + 2 (mirror->panel) + the
	// panel lies at z=6: length 10 units, converted through metersPerUnit/c.
	TASK_CHECK(closeDouble(ray.contributions[0].flightNs, 10.0 / kSpeedOfLight * 1e9, 1e-10));
	return 0;
}

static int transportOcclusion() {
	// Wall at z = 0 lit by an off-axis light. The occluder straddles the
	// point-to-light segment but sits off the camera ray, so both scenes shade
	// the same wall point and only the shadow term changes.
	std::vector<ModelTriangle> lit = {makeTri({-2, -2, 0}, {2, -2, 0}, {2, 2, 0}, Colour(200, 200, 200)),
	                                  makeTri({-2, -2, 0}, {2, 2, 0}, {-2, 2, 0}, Colour(200, 200, 200))};
	const Light light = pointLight({1.2f, 0.0f, 1.6f}, {1, 1, 1}, 40.0f);

	std::vector<ModelTriangle> blocked = lit;
	const glm::vec3 mid(0.6f, 0.0f, 0.8f); // midpoint of the (0,0,0)->light segment
	const glm::vec3 d = glm::normalize(glm::vec3(1.2f, 0.0f, 1.6f));
	const glm::vec3 u(0.0f, 1.0f, 0.0f);
	const glm::vec3 v = glm::normalize(glm::cross(d, u));
	const float half = 0.5f;
	addQuad(blocked, mid - half * u - half * v, mid + half * u - half * v, mid + half * u + half * v,
	        mid - half * u + half * v, Colour(50, 50, 50));

	Primitives prim;
	const Scene sceneLit(lit, prim), sceneBlocked(blocked, prim);
	std::mt19937 rngLit(5), rngBlocked(5);
	const AfterglowRayResult open = pathTraceAfterglowRay({0, 0, 4}, {0, 0, -1}, sceneLit, {light}, kEdges, 1.0, rngLit);
	const AfterglowRayResult dark =
	    pathTraceAfterglowRay({0, 0, 4}, {0, 0, -1}, sceneBlocked, {light}, kEdges, 1.0, rngBlocked);
	const AfterglowContribution *openDirect = nullptr, *darkDirect = nullptr;
	for (const auto &c : open.contributions)
		if (!c.environment && c.response.size() == 1)
			openDirect = &c;
	for (const auto &c : dark.contributions)
		if (!c.environment && c.response.size() == 1)
			darkDirect = &c;
	TASK_CHECK(openDirect != nullptr && darkDirect != nullptr);
	TASK_CHECK(openDirect->rgb.r > 0.0);
	TASK_CHECK(closeVec(darkDirect->rgb, glm::dvec3(0.0), 1e-12)); // shadow ray fully blocked
	// The blocked scene's first hit is still the wall (same flight time).
	TASK_CHECK(closeDouble(darkDirect->flightNs, openDirect->flightNs, 1e-12));
	return 0;
}

static int transportPerChannelClipping() {
	// Two coloured lights at different distances drive S above 1 on the red
	// channel. The shared per-channel clip must be redistributed across the
	// lights in their own ratios, never clipped per light.
	std::vector<ModelTriangle> mesh = closedBox(3.0, Colour(255, 255, 255), 5.0);
	Primitives prim;
	const Scene scene(mesh, prim);
	const Light A = pointLight({1.5f, 0.5f, 1.5f}, {1.0f, 0.2f, 0.1f}, 1200.0f);
	const Light B = pointLight({-1.8f, -0.4f, -1.0f}, {0.8f, 0.3f, 0.2f}, 900.0f);
	std::mt19937 rng(99);
	const AfterglowRayResult ray =
	    pathTraceAfterglowRay({0, 0, 0}, {0.3f, 0.7f, -0.6f}, scene, {A, B}, kEdges, 1.0, rng);
	glm::dvec3 directSum(0.0);
	int directCount = 0;
	for (const auto &c : ray.contributions) {
		if (!c.environment && c.response.size() == 1) { // first-hit direct terms only
			directSum += c.rgb;
			++directCount;
		}
	}
	TASK_CHECK(directCount == 2); // one contribution per light, split before clipping
	// Independently recompute the clipped result at the first hit.
	const RayTriangleIntersection hit = scene.intersect({0, 0, 0}, glm::normalize(glm::vec3(0.3f, 0.7f, -0.6f)));
	TASK_CHECK(hit.hit);
	const glm::vec3 point = hit.intersectionPoint;
	const glm::vec3 n = glm::dot(hit.intersectedTriangle.normal, glm::vec3(0.3f, 0.7f, -0.6f)) > 0
	                        ? -hit.intersectedTriangle.normal
	                        : hit.intersectedTriangle.normal;
	glm::vec3 S(0.0f);
	for (const Light &L : {A, B}) {
		const glm::vec3 to = L.position - point;
		const float d = glm::length(to);
		const float atten = L.intensity / (L.attenConstant + L.attenLinear * d + L.attenQuadratic * d * d);
		const float inc = std::max(0.0f, glm::dot(n, to / d));
		S += L.colour * (atten * inc); // both visible inside the closed box
	}
	const glm::dvec3 expect(S.r > 1.0 ? 1.0 : S.r, S.g > 1.0 ? 1.0 : S.g, S.b > 1.0 ? 1.0 : S.b);
	TASK_CHECK(closeVec(directSum, expect, 1e-6));
	TASK_CHECK(S.r > 1.0); // the red clip actually fired
	return 0;
}

static int transportMultipleDiffusePrefixes() {
	// Inside a closed box every bounce stays inside: prefixes must grow by one
	// owned tau per diffuse interaction, up to the depth-4 cap (5 hits max), and
	// earlier prefixes must remain intact when later bounces append.
	std::vector<ModelTriangle> mesh = closedBox(3.0, Colour(220, 180, 120), 7.5);
	Primitives prim;
	const Scene scene(mesh, prim);
	const std::vector<Light> lights = {pointLight({0, 1.5f, 0}, {1, 1, 1}, 25.0f)};
	std::mt19937 rng(21);
	const AfterglowRayResult ray = pathTraceAfterglowRay({0, 0, 0}, {0.1f, 0.2f, -1}, scene, lights, kEdges, 1.0, rng);
	std::size_t maxChain = 0;
	const TemporalResponse *firstPrefix = nullptr;
	for (const auto &c : ray.contributions) {
		maxChain = std::max(maxChain, c.response.size());
		for (std::size_t i = 0; i < c.response.size(); ++i)
			TASK_CHECK(closeDouble(c.response.tauAt(i), 7.5)); // every coating in the box shares this tau
		if (firstPrefix == nullptr && !c.environment)
			firstPrefix = &c.response;
	}
	TASK_CHECK(maxChain >= 2);     // at least one multi-bounce path occurred
	TASK_CHECK(maxChain <= 5);     // primary hit plus four bounces
	TASK_CHECK(firstPrefix != nullptr);
	const std::size_t firstSize = firstPrefix->size();
	// Ownership: appending to a copy cannot mutate the stored prefix.
	TemporalResponse copy = *firstPrefix;
	const TemporalResponse longer = copy.append(2.0);
	TASK_CHECK(firstPrefix->size() == firstSize);
	TASK_CHECK(copy.size() == firstSize);
	TASK_CHECK(longer.size() == firstSize + 1);
	return 0;
}

static int transportEnergyReorganizationAndSteadyParity() {
	// Bins + before + after + environment must rebuild the unquantised steady
	// RGB for the same spatial sample, regardless of edges or lifetimes.
	std::vector<ModelTriangle> box = closedBox(3.0, Colour(200, 150, 90), 4.0);
	Primitives prim;
	const Scene scene(box, prim);
	const std::vector<Light> lights = {pointLight({1.0f, 1.0f, 1.0f}, {1, 0.8f, 0.6f}, 30.0f)};
	const std::vector<std::vector<double>> edgeSets = {
	    {0.01, 0.1, 1.0, 10.0, 100.0}, {5.0, 6.0, 7.0}, {1e-6, 1e6}, {0.5, 2.0, 8.0, 30.0}};
	for (const auto &edges : edgeSets) {
		std::mt19937 rng(1234);
		const AfterglowRayResult ray =
		    pathTraceAfterglowRay({0, 0, 0}, {0.4f, -0.3f, -0.8f}, scene, lights, edges, 1.0, rng);
		glm::dvec3 rebuilt = ray.beforeWindow + ray.afterWindow + ray.environment;
		for (const auto &bin : ray.bins)
			rebuilt += bin;
		TASK_CHECK(closeVec(rebuilt, ray.steady, 1e-9));
	}
	// Change the coating lifetime: conservation must still hold exactly.
	for (double tau : {0.0, 1e-6, 13.0, 1e6}) {
		std::vector<ModelTriangle> retau = closedBox(3.0, Colour(200, 150, 90), tau);
		const Scene scene2(retau, prim);
		std::mt19937 rng(4321);
		const AfterglowRayResult ray =
		    pathTraceAfterglowRay({0, 0, 0}, {0.4f, -0.3f, -0.8f}, scene2, lights, edgeSets.front(), 1.0, rng);
		glm::dvec3 rebuilt = ray.beforeWindow + ray.afterWindow + ray.environment;
		for (const auto &bin : ray.bins)
			rebuilt += bin;
		TASK_CHECK(closeVec(rebuilt, ray.steady, 1e-9));
	}
	// The steady twin and the classic renderer agree on the same pixel, which
	// also pins down the depth-exhausted branch behaviour.
	const int W = 12, H = 10;
	Camera camera(W, H, 2.0f, glm::vec3(0.0f, 0.0f, 2.5f));
	camera.scale = 30.0f;
	camera.lookAt(glm::vec3(0.0f, -0.5f, -1.0f));
	Canvas canvas(W, H);
	renderPathTraced(box, camera, canvas, 1, lights);
	const AfterglowFilm film = renderAfterglow(box, camera, lights, W, H, 1, 1.0, edgeSets.front());
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			const uint32_t px = canvas.getPixelColour(x, y);
			const glm::dvec3 classic((px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF);
			const glm::dvec3 ours = film.at(x, y).steady * 255.0;
			TASK_CHECK(std::abs(classic.r - ours.r) <= 2.0);
			TASK_CHECK(std::abs(classic.g - ours.g) <= 2.0);
			TASK_CHECK(std::abs(classic.b - ours.b) <= 2.0);
		}
	return 0;
}

static int transportSteadyTwinRngParity() {
	// A scene whose paths actually draw hemisphere samples: the temporal ray and
	// its steady twin must consume the same RNG, take the same branches and
	// return the same RGB, regardless of the lifetime chain.
	std::vector<ModelTriangle> mesh = closedBox(3.0, Colour(200, 150, 90), 9.0);
	Primitives prim;
	const Scene scene(mesh, prim);
	const std::vector<Light> lights = {pointLight({1.0f, 1.0f, 1.0f}, {1, 0.8f, 0.6f}, 30.0f),
	                                  pointLight({-1.0f, 0.0f, -1.0f}, {0.6f, 0.8f, 1.0f}, 18.0f)};
	for (unsigned seed : {1u, 42u, 777u}) {
		const glm::vec3 direction = glm::normalize(glm::vec3(0.2f, -0.4f, -0.9f));
		std::mt19937 rngA(seed), rngB(seed);
		const AfterglowRayResult temporal =
		    pathTraceAfterglowRay({0, 0, 0}, direction, scene, lights, kEdges, 1.0, rngA);
		const glm::dvec3 steady = pathTraceSteadyRay({0, 0, 0}, direction, scene, lights, rngB);
		std::stringstream sa, sb;
		sa << rngA;
		sb << rngB;
		TASK_CHECK(sa.str() == sb.str()); // identical RNG end state
		TASK_CHECK(closeVec(temporal.steady, steady, 1e-12));
	}
	return 0;
}

// =============================== film group =================================

static int filmRejectionAndRngUntouched() {
	std::vector<ModelTriangle> good = closedBox(2.0, Colour(180, 180, 180), 1.0);
	const std::vector<Light> lights = {pointLight({0, 1, 0}, {1, 1, 1}, 20.0f)};
	const std::vector<double> edges = {1.0, 2.0, 3.0};
	Camera camera(8, 8, 2.0f, glm::vec3(0, 0, 1.5f));
	camera.scale = 20.0f;
	camera.lookAt(glm::vec3(0, 0, 0));

	auto rejected = [&](const std::vector<ModelTriangle> &model, const std::vector<Light> &ls,
	                    const std::vector<double> &es, double mpu, int samples, int w = 8, int h = 8) {
		try {
			(void)renderAfterglow(model, camera, ls, w, h, samples, mpu, es);
			return false;
		} catch (...) {
			return true;
		}
	};
	TASK_CHECK(rejected(good, {}, edges, 1.0, 4));                                  // no lights
	TASK_CHECK(rejected(good, lights, {1.0}, 1.0, 4));                              // too few edges
	TASK_CHECK(rejected(good, lights, {1.0, 1.0}, 1.0, 4));                         // not strictly increasing
	TASK_CHECK(rejected(good, lights, {1.0, std::nan("")}, 1.0, 4));                // non-finite edge
	TASK_CHECK(rejected(good, lights, edges, 0.0, 4));                              // bad metric scale
	TASK_CHECK(rejected(good, lights, edges, -1.0, 4));                             // negative metric scale
	TASK_CHECK(rejected(good, lights, edges, 1.0, 0));                              // bad sample count
	TASK_CHECK(rejected(good, lights, edges, 1.0, 4, 0, 8));                        // bad width
	TASK_CHECK(!rejected(good, lights, edges, 1.0, 4));                             // the baseline is valid

	std::vector<ModelTriangle> badTau = good;
	badTau[0].afterglowTauNs = 5.0;
	badTau[0].material = Material::Mirror; // non-zero lifetime off a diffuse surface
	TASK_CHECK(rejected(badTau, lights, edges, 1.0, 4));
	std::vector<ModelTriangle> badRange = good;
	badRange[0].afterglowTauNs = 1e-9;
	TASK_CHECK(rejected(badRange, lights, edges, 1.0, 4));
	std::vector<ModelTriangle> badMaterial = good;
	badMaterial[0].material = Material::Glass;
	TASK_CHECK(rejected(badMaterial, lights, edges, 1.0, 4));

	Light area = lights[0];
	area.radius = 0.25f;
	TASK_CHECK(rejected(good, {area}, edges, 1.0, 4)); // radius must be 0
	Light directional = lights[0];
	directional.type = LightType::Directional;
	TASK_CHECK(rejected(good, {directional}, edges, 1.0, 4));
	Light negative = lights[0];
	negative.intensity = -1.0f;
	TASK_CHECK(rejected(good, {negative}, edges, 1.0, 4));
	Light zero = lights[0];
	zero.intensity = 0.0f; // zero intensity is explicitly allowed
	TASK_CHECK(!rejected(good, {zero}, edges, 1.0, 4));
	Light zeroDenom = lights[0];
	zeroDenom.attenConstant = 0.0f;
	zeroDenom.attenLinear = 0.0f;
	zeroDenom.attenQuadratic = 0.0f; // denominator collapses to zero
	TASK_CHECK(rejected(good, {zeroDenom}, edges, 1.0, 4));

	// A rejected call must not advance the caller's RNG.
	std::mt19937 rng(777);
	std::stringstream before;
	before << rng;
	Primitives prim;
	const Scene scene(good, prim);
	try {
		(void)pathTraceAfterglowRay({0, 0, 0}, {0, 0, -1}, scene, lights, {1.0}, 1.0, rng);
		TASK_CHECK(false);
	} catch (...) {
		TASK_CHECK(true);
	}
	std::stringstream after;
	after << rng;
	TASK_CHECK(before.str() == after.str());
	return 0;
}

static int filmConservationAndSampleAveraging() {
	std::vector<ModelTriangle> mesh = closedBox(2.5, Colour(190, 160, 130), 6.0);
	const std::vector<Light> lights = {pointLight({0.8f, 0.8f, 0.8f}, {1, 0.9f, 0.7f}, 25.0f),
	                                  pointLight({-0.9f, 0.2f, -0.8f}, {0.5f, 0.7f, 1.0f}, 18.0f)};
	const std::vector<double> edges = {0.2, 1.0, 5.0, 20.0, 80.0};
	const int W = 10, H = 8, spp = 4;
	Camera camera(W, H, 2.0f, glm::vec3(0.0f, 0.0f, 2.0f));
	camera.scale = 26.0f;
	camera.lookAt(glm::vec3(0.0f, -0.4f, -0.8f));
	const AfterglowFilm film = renderAfterglow(mesh, camera, lights, W, H, spp, 1.0, edges);

	Primitives prim;
	const Scene scene(mesh, prim);
	const float f = camera.focalLength * camera.scale;
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			const AfterglowPixel &px = film.at(x, y);
			glm::dvec3 rebuilt = px.beforeWindow + px.afterWindow + px.environment;
			for (const auto &bin : px.bins)
				rebuilt += bin;
			TASK_CHECK(closeVec(rebuilt, px.steady, 1e-9));

			// Replay the exact per-pixel camera loop: the film mean must equal
			// the mean of the individual camera samples (not hit/window counts).
			std::mt19937 rng(static_cast<unsigned>((y * W + x) * 9781 + 1));
			std::uniform_real_distribution<float> jitter(-0.5f, 0.5f);
			std::uniform_real_distribution<float> U(0.0f, 1.0f);
			glm::dvec3 manualSteady(0.0), manualEnv(0.0);
			for (int s = 0; s < spp; ++s) {
				const float jx = jitter(rng), jy = jitter(rng);
				(void)U(rng);
				glm::vec3 rayO, rayD;
				camera.primaryRay(x + 0.5f + jx - W / 2.0f, -(y + 0.5f + jy - H / 2.0f), f, rayO, rayD);
				const AfterglowRayResult ray =
				    pathTraceAfterglowRay(rayO, rayD, scene, lights, edges, 1.0, rng);
				manualSteady += ray.steady;
				manualEnv += ray.environment;
			}
			TASK_CHECK(closeVec(px.steady, manualSteady / static_cast<double>(spp), 1e-14));
			TASK_CHECK(closeVec(px.environment, manualEnv / static_cast<double>(spp), 1e-14));
		}
	return 0;
}

static int filmCsvRoundTripAndPpm() {
	std::vector<ModelTriangle> mesh = closedBox(2.0, Colour(170, 170, 200), 3.5);
	const std::vector<Light> lights = {pointLight({0, 1, 0}, {1, 1, 1}, 20.0f)};
	const std::vector<double> edges = {0.1, 0.7, 3.3, 12.0, 40.0, 90.0};
	const int W = 6, H = 5;
	Camera camera(W, H, 2.0f, glm::vec3(0, 0, 1.5f));
	camera.scale = 18.0f;
	camera.lookAt(glm::vec3(0, -0.3f, -0.5f));
	const AfterglowFilm film = renderAfterglow(mesh, camera, lights, W, H, 3, 0.8, edges);

	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "rednoise_afterglow_tests";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	const std::string csv = (dir / "roundtrip.csv").string();
	const std::string ppm = (dir / "steady.ppm").string();
	TASK_CHECK(writeAfterglowCsv(film, csv));
	TASK_CHECK(writeSteadyPpm(film, ppm));

	AfterglowFilm read;
	TASK_CHECK(readAfterglowCsv(csv, read));
	TASK_CHECK(read.width == film.width && read.height == film.height);
	TASK_CHECK(read.timeEdges.size() == film.timeEdges.size());
	for (std::size_t i = 0; i < edges.size(); ++i)
		TASK_CHECK(closeDouble(read.timeEdges[i], film.timeEdges[i], 1e-16));
	for (int i = 0; i < W * H; ++i) {
		const AfterglowPixel &a = film.pixels[i];
		const AfterglowPixel &b = read.pixels[i];
		TASK_CHECK(closeVec(a.steady, b.steady, 1e-15));
		TASK_CHECK(closeVec(a.beforeWindow, b.beforeWindow, 1e-15));
		TASK_CHECK(closeVec(a.afterWindow, b.afterWindow, 1e-15));
		TASK_CHECK(closeVec(a.environment, b.environment, 1e-15));
		TASK_CHECK(a.bins.size() == b.bins.size());
		for (std::size_t k = 0; k < a.bins.size(); ++k)
			TASK_CHECK(closeVec(a.bins[k], b.bins[k], 1e-15));
	}
	// The PPM is a P6 preview quantised by the classic truncate-then-clamp rule.
	std::ifstream in(ppm, std::ios::binary);
	std::string magic;
	int ppmW = 0, ppmH = 0, ppmMax = 0;
	in >> magic >> ppmW >> ppmH >> ppmMax;
	in.get();
	TASK_CHECK(magic == "P6");
	TASK_CHECK(ppmW == W && ppmH == H && ppmMax == 255);
	std::vector<unsigned char> rgb(static_cast<std::size_t>(W) * H * 3);
	in.read(reinterpret_cast<char *>(rgb.data()), rgb.size());
	TASK_CHECK(in.gcount() == static_cast<std::streamsize>(rgb.size()));
	for (int i = 0; i < W * H; ++i)
		for (int c = 0; c < 3; ++c) {
			const int expected = std::clamp(static_cast<int>(film.pixels[i].steady[c] * 255.0), 0, 255);
			TASK_CHECK(rgb[i * 3 + c] == static_cast<unsigned char>(expected));
		}
	// A missing file must fail cleanly rather than throw.
	AfterglowFilm missing;
	TASK_CHECK(!readAfterglowCsv((dir / "does_not_exist.csv").string(), missing));
	return 0;
}

static int filmEnvironmentSeparateAndEdgesInvariant() {
	// A camera aimed at the sky stores background energy only in environment;
	// shifting the time window cannot move that energy into bins.
	std::vector<ModelTriangle> mesh = {makeTri({-1, -1, 0}, {1, -1, 0}, {0, 1, 0}, Colour(120, 120, 120))};
	const std::vector<Light> lights = {pointLight({0, 0, 3}, {1, 1, 1}, 10.0f)};
	const int W = 4, H = 4;
	Camera camera(W, H, 2.0f, glm::vec3(0, 0, 2));
	camera.scale = 10.0f;
	camera.lookAt(glm::vec3(0, 10.0f, 0)); // aim well above the only triangle
	const AfterglowFilm narrow = renderAfterglow(mesh, camera, lights, W, H, 2, 1.0, {1.0, 2.0});
	const AfterglowFilm wide = renderAfterglow(mesh, camera, lights, W, H, 2, 1.0, {1e-6, 1e6});
	for (int i = 0; i < W * H; ++i) {
		for (const auto &bin : narrow.pixels[i].bins)
			TASK_CHECK(closeVec(bin, glm::dvec3(0.0), 1e-15));
		TASK_CHECK(closeVec(narrow.pixels[i].beforeWindow, glm::dvec3(0.0), 1e-15));
		TASK_CHECK(closeVec(narrow.pixels[i].afterWindow, glm::dvec3(0.0), 1e-15));
		TASK_CHECK(narrow.pixels[i].environment.r >= 0.0);
		TASK_CHECK(closeVec(narrow.pixels[i].environment, wide.pixels[i].environment, 1e-15));
		TASK_CHECK(closeVec(narrow.pixels[i].steady, wide.pixels[i].steady, 1e-15));
	}
	return 0;
}

extern "C" const struct task_case task_cases[] = {
    {"response", "single_exponential", responseSingleExponential},
    {"response", "erlang_repeated_lifetimes", responseErlangRepeatedLifetimes},
    {"response", "two_distinct_formula", responseTwoDistinctFormula},
    {"response", "near_equal_lifetimes", responseNearEqualLifetimes},
    {"response", "zero_and_negative_time", responseZeroAndNegativeTime},
    {"response", "long_tail_and_validation", responseLongTailAndValidation},
    {"transport", "environment_escape", transportEnvironmentEscape},
    {"transport", "flight_time_and_impulse", transportFlightTimeAndImpulse},
    {"transport", "mirror_turn_and_emissive", transportMirrorTurnAndEmissive},
    {"transport", "occlusion", transportOcclusion},
    {"transport", "per_channel_clipping", transportPerChannelClipping},
    {"transport", "multiple_diffuse_prefixes", transportMultipleDiffusePrefixes},
    {"transport", "steady_twin_rng_parity", transportSteadyTwinRngParity},
    {"transport", "energy_reorganization_and_parity", transportEnergyReorganizationAndSteadyParity},
    {"film", "rejection_and_rng_untouched", filmRejectionAndRngUntouched},
    {"film", "conservation_and_sample_averaging", filmConservationAndSampleAveraging},
    {"film", "csv_roundtrip_and_ppm", filmCsvRoundTripAndPpm},
    {"film", "environment_separate_and_edges_invariant", filmEnvironmentSeparateAndEdgesInvariant},
};

extern "C" const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
