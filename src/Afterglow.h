#pragma once

#include "TemporalResponse.h"
#include <glm/glm.hpp>
#include <cstddef>
#include <random>
#include <string>
#include <vector>

struct Light;
struct ModelTriangle;
class Camera;
class Scene;

// One real radiance contribution produced by a path: its (unquantised) RGB,
// the geometric flight time (nanoseconds) of every segment up to the terminating
// light or surface, and the immutable, owned temporal-response prefix in force
// when it was made. Environment escapes set `environment` and carry no arrival
// time: background radiance is never assigned a fabricated finite timestamp.
struct AfterglowContribution {
	glm::dvec3 rgb{0.0};
	double flightNs = 0.0;
	TemporalResponse response;
	bool environment = false;
};

// Everything a single camera ray produced. `contributions` owns one entry per
// real contribution (direct lighting, emissive hits, background escape); the
// binned energies partition those contributions onto the caller's time edges,
// and `steady` is their unquantised sum in steady state:
//   steady == before + sum(bins) + after + environment
// up to floating-point rounding, independently of lifetimes and time edges.
struct AfterglowRayResult {
	std::vector<AfterglowContribution> contributions;
	std::vector<glm::dvec3> bins; // one RGB per half-open [edges[i], edges[i+1))
	glm::dvec3 beforeWindow{0.0};
	glm::dvec3 afterWindow{0.0};
	glm::dvec3 environment{0.0};
	glm::dvec3 steady{0.0};
};

// Per-pixel averaged film. Every channel is averaged over the full number of
// camera samples, never over hit or in-window counts.
struct AfterglowPixel {
	std::vector<glm::dvec3> bins;
	glm::dvec3 beforeWindow{0.0};
	glm::dvec3 afterWindow{0.0};
	glm::dvec3 environment{0.0};
	glm::dvec3 steady{0.0};
};

// Owned double-precision result of a full render, indexed [y * width + x].
struct AfterglowFilm {
	int width = 0;
	int height = 0;
	std::vector<double> timeEdges;
	std::vector<AfterglowPixel> pixels;

	const AfterglowPixel &at(int x, int y) const { return pixels[static_cast<std::size_t>(y) * width + x]; }
};

// Speed of light in metres per second; flight time is
// length(world units) * metersPerUnit / kSpeedOfLight * 1e9 nanoseconds.
inline constexpr double kSpeedOfLight = 299792458.0;

// Validate every render argument. Returns an empty string when the configuration
// is renderable, otherwise a human-readable reason. Validation runs before any
// RNG is constructed or any output storage allocated, so a rejected call leaves
// the caller's RNG and outputs untouched.
//
// The accepted scene is deliberately narrow: triangle meshes whose materials are
// Diffuse, Mirror or Emissive only (no analytic primitives), at least one finite
// Point light with radius 0 and finite non-negative colour/intensity, and a
// positive finite metric scale. A non-zero afterglow lifetime is legal only on a
// Diffuse triangle; its range is [1e-6, 1e6] ns and 0 means instantaneous.
std::string validateAfterglowInputs(const std::vector<ModelTriangle> &model, const std::vector<Light> &lights,
                                    const std::vector<double> &timeEdges, double metersPerUnit, int samples);

// Single-ray temporal transport over a prebuilt triangle Scene. The RNG stream,
// branch decisions and depth limit (4) are exactly those of the steady-state
// path tracer: flight-time bookkeeping consumes no random numbers and cannot
// change a termination branch. Throws std::invalid_argument on bad arguments.
AfterglowRayResult pathTraceAfterglowRay(const glm::vec3 &origin, const glm::vec3 &direction, const Scene &scene,
                                         const std::vector<Light> &lights, const std::vector<double> &timeEdges,
                                         double metersPerUnit, std::mt19937 &rng);

// Steady-state twin of the temporal ray: the same transport with the temporal
// bookkeeping disabled, returning the unquantised RGB (0..1-ish) the classic
// path tracer would return. Feeding both functions identically seeded RNGs
// leaves both RNGs in the same end state.
glm::dvec3 pathTraceSteadyRay(const glm::vec3 &origin, const glm::vec3 &direction, const Scene &scene,
                              const std::vector<Light> &lights, std::mt19937 &rng);

// Render a full afterglow film. The camera loop reuses the classic path
// tracer's per-pixel seed ((y * width + x) * 9781 + 1), jitter and RNG draws;
// each pixel is the mean over all `samples` camera samples. Throws
// std::invalid_argument on bad arguments, before any state is touched.
AfterglowFilm renderAfterglow(const std::vector<ModelTriangle> &model, const Camera &camera,
                              const std::vector<Light> &lights, int width, int height, int samples,
                              double metersPerUnit, const std::vector<double> &timeEdges);

// Write the film's double-precision values to `path` as CSV (one row per pixel,
// steady, pre-window, every time bin, post-window and environment channels),
// with enough precision to round-trip. Returns false on I/O failure.
bool writeAfterglowCsv(const AfterglowFilm &film, const std::string &path);

// Read a CSV written by writeAfterglowCsv back into an owned film. Returns false
// on a malformed or unreadable file.
bool readAfterglowCsv(const std::string &path, AfterglowFilm &film);

// Quantise the steady-state channel exactly like the classic path tracer
// (truncate after x255, clamp to 255) and write a P6 PPM preview. Returns false
// on I/O failure.
bool writeSteadyPpm(const AfterglowFilm &film, const std::string &path);
