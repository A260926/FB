#include "Afterglow.h"

#include "Camera.h"
#include "Light.h"
#include "Scene.h"
#include <ModelTriangle.h>
#include <RayTriangleIntersection.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kRayBias = 1e-4f;
constexpr int kMaxDepth = 4;

// Mirror of the classic path tracer's cosine-weighted hemisphere sample, kept
// bit-for-bit in the same two-Draw form so both transport twins consume RNG
// identically.
glm::vec3 cosineHemisphere(const glm::vec3 &n, std::mt19937 &rng) {
	std::uniform_real_distribution<float> U(0.0f, 1.0f);
	const float u1 = U(rng), u2 = U(rng);
	const float r = std::sqrt(u1);
	const float theta = 2.0f * kPi * u2;
	const float x = r * std::cos(theta), y = r * std::sin(theta);
	const float z = std::sqrt(std::max(0.0f, 1.0f - u1));
	const glm::vec3 up = std::abs(n.z) < 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
	const glm::vec3 t = glm::normalize(glm::cross(up, n));
	const glm::vec3 b = glm::cross(n, t);
	return glm::normalize(t * x + b * y + n * z);
}

glm::vec3 faceViewer(glm::vec3 normal, const glm::vec3 &rayDirection) {
	return (glm::dot(normal, rayDirection) > 0.0f) ? -normal : normal;
}

// Nearest-neighbour texture lookup, identical to the classic renderer's path.
glm::vec3 sampleTexture(const TextureMap &tex, float u, float v) {
	const int tw = static_cast<int>(tex.width);
	const int th = static_cast<int>(tex.height);
	const int tx = std::min(std::max(static_cast<int>(u * (tw - 1)), 0), tw - 1);
	const int ty = std::min(std::max(static_cast<int>((1.0f - v) * (th - 1)), 0), th - 1);
	const uint32_t p = tex.pixels[static_cast<std::size_t>(ty) * tw + tx];
	return glm::vec3((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
}

// Flat or textured albedo in 0..1. The accepted material set excludes the
// procedural/parallax/normal-map variants, so only these two branches exist.
glm::vec3 baseColour01(const RayTriangleIntersection &hit) {
	const ModelTriangle &tri = hit.intersectedTriangle;
	if (tri.texture) {
		const float w0 = 1.0f - hit.u - hit.v;
		const float tu = w0 * tri.texturePoints[0].x + hit.u * tri.texturePoints[1].x + hit.v * tri.texturePoints[2].x;
		const float tv = w0 * tri.texturePoints[0].y + hit.u * tri.texturePoints[1].y + hit.v * tri.texturePoints[2].y;
		return sampleTexture(*tri.texture, tu, tv) / 255.0f;
	}
	return glm::vec3(tri.colour.red, tri.colour.green, tri.colour.blue) / 255.0f;
}

// The default procedural sky used by the classic path tracer when the
// physically-based sky model is switched off (its default state).
glm::vec3 sky01(const glm::vec3 &direction) {
	const float t = 0.5f * (direction.y + 1.0f);
	const glm::vec3 c = glm::mix(glm::vec3(200.0f, 210.0f, 235.0f), glm::vec3(70.0f, 120.0f, 205.0f), t);
	return c / 255.0f;
}

bool finiteVec3(const glm::vec3 &v) {
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// One light's unclipped diffuse contribution (all components non-negative),
// alongside the point-to-light distance used for temporal placement.
struct LightTerm {
	glm::vec3 rgb;
	float distance;
};

LightTerm directTerm(const Light &L, const glm::vec3 &point, const glm::vec3 &normal, const Scene &scene,
                     int ignore) {
	const glm::vec3 to = L.position - point;
	const float dist = glm::length(to);
	const glm::vec3 lightDir = to / dist;
	const float denom = L.attenConstant + L.attenLinear * dist + L.attenQuadratic * dist * dist;
	const float attenuation = L.intensity / denom;
	const float visible = scene.occluded(point, lightDir, dist, ignore) ? 0.0f : 1.0f;
	const float incidence = std::max(0.0f, glm::dot(normal, lightDir));
	return {L.colour * (visible * attenuation * incidence), dist};
}

// Temporal sink: every real contribution, with the immutable response prefix in
// force when it was made and its own geometric flight time.
struct TemporalSink {
	std::vector<AfterglowContribution> items;

	void add(const glm::vec3 &rgb01, double flightNs, const TemporalResponse &response, bool environment) {
		AfterglowContribution c;
		c.rgb = glm::dvec3(rgb01);
		c.flightNs = flightNs;
		c.response = response; // owned copy; later bounces cannot mutate it
		c.environment = environment;
		items.push_back(std::move(c));
	}
};

// Steady-state sink: the same events folded into one RGB value, exactly as the
// classic return-value recursion would compose them.
struct SteadySink {
	glm::dvec3 total{0.0};

	void add(const glm::vec3 &rgb01, double, const TemporalResponse &, bool) { total += glm::dvec3(rgb01); }
};

double toNanoseconds(double lengthUnits, double metersPerUnit) {
	return lengthUnits * metersPerUnit / kSpeedOfLight * 1e9;
}

// The single transport used by both the temporal ray and its steady-state twin.
// The branch order, RNG draws, bias offset and depth limit mirror pathTrace in
// Renderer.cpp; flight-time bookkeeping is pure geometry over true hit points
// (never the biased ray origins) and never touches the RNG or any termination
// test.
//
// `prevPoint` is the point the current ray really left (the camera origin, or
// the previous true surface point). Segment lengths are measured directly
// between true points, so the self-intersection bias neither adds nor removes
// any distance. `throughput` is the product of the diffuse albedos above this
// node, exactly the weight the classic return-value recursion applies to every
// deeper contribution; response prefixes are owned per contribution, keeping
// earlier prefixes immutable.
template <typename Sink>
void tracePath(const glm::vec3 &origin, const glm::vec3 &direction, const glm::vec3 &prevPoint, const Scene &scene,
               const std::vector<Light> &lights, int depth, const TemporalResponse &prefix, const glm::vec3 &throughput,
               double flightUnits, double metersPerUnit, std::mt19937 &rng, Sink &sink) {
	const RayTriangleIntersection hit = scene.intersect(origin, direction);
	if (!hit.hit) {
		// Background escape: no finite arrival time exists, so it is filed as an
		// environment contribution rather than stamped onto the time axis.
		sink.add(throughput * sky01(direction), 0.0, prefix, true);
		return;
	}
	const ModelTriangle &tri = hit.intersectedTriangle;
	const glm::vec3 point = hit.intersectionPoint;
	const int ignore = static_cast<int>(hit.triangleIndex);

	// True geometric length travelled since the previous real point. The ray
	// origin may be biased off `point`'s predecessor, but the reported
	// intersection point lies on the surface itself, so this distance is
	// unaffected by the bias.
	const double arrivedUnits = flightUnits + static_cast<double>(glm::distance(prevPoint, point));

	if (tri.material == Material::Emissive) {
		sink.add(throughput * glm::vec3(tri.colour.red, tri.colour.green, tri.colour.blue) / 255.0f * 4.0f,
		         toNanoseconds(arrivedUnits, metersPerUnit), prefix, false);
		return;
	}

	if (tri.material == Material::Mirror && depth > 0) {
		const glm::vec3 n = faceViewer(tri.normal, direction);
		const glm::vec3 reflected = glm::reflect(direction, n);
		const glm::vec3 biased = point + kRayBias * reflected;
		// Mirrors carry no albedo weight, so throughput is passed through.
		tracePath(biased, reflected, point, scene, lights, depth - 1, prefix, throughput, arrivedUnits, metersPerUnit,
		          rng, sink);
		return;
	}

	// Diffuse tail. A Mirror that has run out of depth lands here too, matching
	// the classic tracer's depth-exhausted behaviour; it adds no dwell kernel.
	const glm::vec3 n = faceViewer(tri.normal, direction);
	const glm::vec3 albedo = baseColour01(hit);

	TemporalResponse atHit = prefix;
	if (tri.material == Material::Diffuse)
		atHit = prefix.append(tri.afterglowTauNs); // exactly one append per diffuse hit

	// Direct lighting. The classic tracer forms S = sum of per-light non-negative
	// RGB then applies per-channel min(S,1) and multiplies by albedo; to keep
	// per-light time placement while reproducing that exact colour, the shared
	// channel clip is redistributed back over the lights in their own ratios.
	std::vector<LightTerm> terms;
	terms.reserve(lights.size());
	glm::vec3 sum(0.0f);
	for (const Light &L : lights) {
		terms.push_back(directTerm(L, point, n, scene, ignore));
		sum += terms.back().rgb;
	}
	const glm::vec3 clipped = glm::min(sum, glm::vec3(1.0f));
	for (std::size_t i = 0; i < lights.size(); ++i) {
		glm::vec3 light01(0.0f);
		for (int c = 0; c < 3; ++c) {
			const float s = sum[c];
			if (s > 0.0f) {
				const float share = clipped[c] * terms[i].rgb[c] / s; // min(S,1)/S times this light
				light01[c] = albedo[c] * share;
			}
		}
		const double flight = toNanoseconds(arrivedUnits + static_cast<double>(terms[i].distance), metersPerUnit);
		sink.add(throughput * light01, flight, atHit, false);
	}

	if (depth <= 0)
		return; // depth-exhausted diffuse keeps its direct term, adds no bounce

	const glm::vec3 bounce = cosineHemisphere(n, rng);
	const glm::vec3 biased = point + kRayBias * bounce;
	// The child measures the segment from this true point, so the bias affects
	// only the ray origin, never the accumulated geometric length. Everything
	// beneath this bounce inherits this surface's albedo.
	tracePath(biased, bounce, point, scene, lights, depth - 1, atHit, throughput * albedo, arrivedUnits, metersPerUnit,
	          rng, sink);
}

// Distribute one finite-time contribution over the caller's edges using its own
// flight shift and response prefix.
void binContribution(const AfterglowContribution &c, const std::vector<double> &edges, AfterglowRayResult &out) {
	if (c.environment) {
		out.environment += c.rgb;
		return;
	}
	const std::size_t binCount = edges.size() - 1;
	const double first = edges.front() - c.flightNs;
	const double preMass = c.response.mass(-std::numeric_limits<double>::infinity(), first);
	out.beforeWindow += c.rgb * preMass;
	double prevEdge = first;
	for (std::size_t i = 0; i < binCount; ++i) {
		const double nextEdge = edges[i + 1] - c.flightNs;
		const double m = c.response.mass(prevEdge, nextEdge);
		out.bins[i] += c.rgb * m;
		prevEdge = nextEdge;
	}
	const double postMass = c.response.mass(prevEdge, std::numeric_limits<double>::infinity());
	out.afterWindow += c.rgb * postMass;
}
} // namespace

std::string validateAfterglowInputs(const std::vector<ModelTriangle> &model, const std::vector<Light> &lights,
                                    const std::vector<double> &timeEdges, double metersPerUnit, int samples) {
	if (samples <= 0)
		return "afterglow: sample count must be positive";
	if (!std::isfinite(metersPerUnit) || metersPerUnit <= 0.0)
		return "afterglow: metersPerUnit must be a positive finite number";
	if (timeEdges.size() < 2)
		return "afterglow: at least two time edges are required";
	for (double e : timeEdges) {
		if (!std::isfinite(e))
			return "afterglow: time edges must be finite";
	}
	for (std::size_t i = 1; i < timeEdges.size(); ++i) {
		if (!(timeEdges[i] > timeEdges[i - 1]))
			return "afterglow: time edges must be strictly increasing";
	}
	if (lights.empty())
		return "afterglow: at least one light is required";
	for (const Light &L : lights) {
		if (L.type != LightType::Point)
			return "afterglow: only finite Point lights are supported";
		if (L.radius != 0.0f)
			return "afterglow: Point lights must have radius 0";
		if (!finiteVec3(L.position))
			return "afterglow: light position must be finite";
		if (!finiteVec3(L.colour) || L.colour.x < 0.0f || L.colour.y < 0.0f || L.colour.z < 0.0f)
			return "afterglow: light colour must be finite and non-negative";
		if (!std::isfinite(L.intensity) || L.intensity < 0.0f)
			return "afterglow: light intensity must be finite and non-negative";
		if (!std::isfinite(L.attenConstant) || !std::isfinite(L.attenLinear) || !std::isfinite(L.attenQuadratic))
			return "afterglow: attenuation coefficients must be finite";
	}
	for (const ModelTriangle &tri : model) {
		if (tri.material != Material::Diffuse && tri.material != Material::Mirror &&
		    tri.material != Material::Emissive)
			return "afterglow: only Diffuse, Mirror and Emissive triangle materials are supported";
		const double tau = tri.afterglowTauNs;
		if (std::isnan(tau) || !std::isfinite(tau) || tau < 0.0)
			return "afterglow: afterglowTauNs must be 0 or a finite positive lifetime";
		if (tau > 0.0) {
			if (tri.material != Material::Diffuse)
				return "afterglow: non-zero lifetimes are only allowed on Diffuse triangles";
			if (tau < TemporalResponse::kMinTau || tau > TemporalResponse::kMaxTau)
				return "afterglow: diffuse lifetimes must lie in [1e-6, 1e6] ns";
		}
		if (!finiteVec3(tri.normal))
			return "afterglow: triangle normals must be finite";
		for (const glm::vec3 &v : tri.vertices)
			if (!finiteVec3(v))
				return "afterglow: triangle vertices must be finite";
	}
	// The distance attenuation denominator kc + kl*d + kq*d^2 must be strictly
	// positive for every point-to-light distance d > 0. This is guaranteed at the
	// polynomial level rather than sampled at vertices: with non-negative
	// coefficients, a positive quadratic or linear coefficient makes the
	// polynomial positive for all d > 0; otherwise the constant alone must be.
	for (const Light &L : lights) {
		const float kc = L.attenConstant, kl = L.attenLinear, kq = L.attenQuadratic;
		if (!std::isfinite(kc) || !std::isfinite(kl) || !std::isfinite(kq) || kc < 0.0f || kl < 0.0f || kq < 0.0f)
			return "afterglow: attenuation coefficients must be finite and non-negative";
		const bool positiveForAllPositiveD = (kq > 0.0f) || (kl > 0.0f) || (kc > 0.0f);
		if (!positiveForAllPositiveD)
			return "afterglow: light distance attenuation denominator must be positive";
	}
	return {};
}

AfterglowRayResult pathTraceAfterglowRay(const glm::vec3 &origin, const glm::vec3 &direction, const Scene &scene,
                                         const std::vector<Light> &lights, const std::vector<double> &timeEdges,
                                         double metersPerUnit, std::mt19937 &rng) {
	if (!std::isfinite(metersPerUnit) || metersPerUnit <= 0.0)
		throw std::invalid_argument("afterglow ray: metersPerUnit must be positive and finite");
	if (timeEdges.size() < 2)
		throw std::invalid_argument("afterglow ray: at least two time edges are required");
	for (std::size_t i = 0; i < timeEdges.size(); ++i) {
		if (!std::isfinite(timeEdges[i]))
			throw std::invalid_argument("afterglow ray: time edges must be finite");
		if (i > 0 && !(timeEdges[i] > timeEdges[i - 1]))
			throw std::invalid_argument("afterglow ray: time edges must be strictly increasing");
	}
	if (lights.empty())
		throw std::invalid_argument("afterglow ray: at least one light is required");
	for (const Light &L : lights) {
		if (L.type != LightType::Point || L.radius != 0.0f || !std::isfinite(L.intensity) || L.intensity < 0.0f
		    || !finiteVec3(L.colour) || L.colour.x < 0.0f || L.colour.y < 0.0f || L.colour.z < 0.0f)
			throw std::invalid_argument("afterglow ray: unsupported light configuration");
	}

	AfterglowRayResult result;
	result.bins.assign(timeEdges.size() - 1, glm::dvec3(0.0));
	TemporalSink sink;
	const TemporalResponse initial;
	tracePath(origin, direction, origin, scene, lights, kMaxDepth, initial, glm::vec3(1.0f), 0.0, metersPerUnit, rng,
	          sink);
	for (const AfterglowContribution &c : sink.items) {
		result.contributions.push_back(c);
		result.steady += c.rgb;
		binContribution(c, timeEdges, result);
	}
	return result;
}

glm::dvec3 pathTraceSteadyRay(const glm::vec3 &origin, const glm::vec3 &direction, const Scene &scene,
                              const std::vector<Light> &lights, std::mt19937 &rng) {
	SteadySink sink;
	const TemporalResponse initial;
	tracePath(origin, direction, origin, scene, lights, kMaxDepth, initial, glm::vec3(1.0f), 0.0, 1.0, rng, sink);
	return sink.total;
}

AfterglowFilm renderAfterglow(const std::vector<ModelTriangle> &model, const Camera &camera,
                              const std::vector<Light> &lights, int width, int height, int samples,
                              double metersPerUnit, const std::vector<double> &timeEdges) {
	if (width <= 0 || height <= 0)
		throw std::invalid_argument("afterglow: width and height must be positive");
	const std::string problem = validateAfterglowInputs(model, lights, timeEdges, metersPerUnit, samples);
	if (!problem.empty())
		throw std::invalid_argument(problem);
	if (!finiteVec3(camera.position))
		throw std::invalid_argument("afterglow: camera position must be finite");

	const Primitives noPrimitives; // analytic primitives are intentionally excluded
	const Scene scene(model, noPrimitives);
	const float f = camera.focalLength * camera.scale;
	const std::size_t binCount = timeEdges.size() - 1;

	AfterglowFilm film;
	film.width = width;
	film.height = height;
	film.timeEdges = timeEdges;
	film.pixels.resize(static_cast<std::size_t>(width) * height);
	for (AfterglowPixel &px : film.pixels)
		px.bins.assign(binCount, glm::dvec3(0.0));

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			// Same per-pixel seed as renderPathTraced.
			std::mt19937 rng(static_cast<unsigned>((y * width + x) * 9781 + 1));
			std::uniform_real_distribution<float> jitter(-0.5f, 0.5f);
			std::uniform_real_distribution<float> U(0.0f, 1.0f);
			AfterglowPixel &pixel = film.pixels[static_cast<std::size_t>(y) * width + x];
			for (int s = 0; s < samples; ++s) {
				// Same jitter draws as the classic loop, plus its motion-blur
				// draw (no motion in this mode), so RNG consumption matches.
				const float jx = jitter(rng);
				const float jy = jitter(rng);
				(void)U(rng);
				glm::vec3 rayO, rayDirection;
				camera.primaryRay(x + 0.5f + jx - width / 2.0f, -(y + 0.5f + jy - height / 2.0f), f, rayO,
				                  rayDirection);
				const AfterglowRayResult ray =
				    pathTraceAfterglowRay(rayO, rayDirection, scene, lights, timeEdges, metersPerUnit, rng);
				pixel.steady += ray.steady;
				pixel.beforeWindow += ray.beforeWindow;
				pixel.afterWindow += ray.afterWindow;
				pixel.environment += ray.environment;
				for (std::size_t b = 0; b < binCount; ++b)
					pixel.bins[b] += ray.bins[b];
			}
			const double inv = 1.0 / static_cast<double>(samples); // mean over camera samples only
			pixel.steady *= inv;
			pixel.beforeWindow *= inv;
			pixel.afterWindow *= inv;
			pixel.environment *= inv;
			for (glm::dvec3 &b : pixel.bins)
				b *= inv;
		}
	}
	return film;
}

namespace {
std::string formatDouble(double v) {
	std::ostringstream oss;
	oss << std::setprecision(17) << v;
	return oss.str();
}

void appendVec3(std::ostringstream &oss, const glm::dvec3 &v) {
	oss << ',' << formatDouble(v.x) << ',' << formatDouble(v.y) << ',' << formatDouble(v.z);
}
} // namespace

bool writeAfterglowCsv(const AfterglowFilm &film, const std::string &path) {
	std::ofstream out(path, std::ios::trunc);
	if (!out)
		return false;
	out << "# dims," << film.width << ',' << film.height << ',' << film.timeEdges.size() << '\n';
	out << "# edges";
	for (double e : film.timeEdges)
		out << ',' << formatDouble(e);
	out << '\n';
	out << "x,y,steady_r,steady_g,steady_b,before_r,before_g,before_b";
	for (std::size_t b = 0; b < film.timeEdges.size() - 1; ++b)
		out << ",bin" << b << "_r,bin" << b << "_g,bin" << b << "_b";
	out << ",after_r,after_g,after_b,environment_r,environment_g,environment_b\n";
	for (int y = 0; y < film.height; ++y) {
		for (int x = 0; x < film.width; ++x) {
			const AfterglowPixel &p = film.at(x, y);
			std::ostringstream line;
			line << x << ',' << y;
			appendVec3(line, p.steady);
			appendVec3(line, p.beforeWindow);
			for (const glm::dvec3 &b : p.bins)
				appendVec3(line, b);
			appendVec3(line, p.afterWindow);
			appendVec3(line, p.environment);
			out << line.str() << '\n';
			if (!out)
				return false;
		}
	}
	return static_cast<bool>(out);
}

bool readAfterglowCsv(const std::string &path, AfterglowFilm &film) {
	std::ifstream in(path);
	if (!in)
		return false;
	film = AfterglowFilm{};
	std::string line;
	int width = 0, height = 0, edgeCount = 0;
	std::vector<double> edges;
	auto split = [](const std::string &s) {
		std::vector<std::string> fields;
		std::string cur;
		for (char ch : s) {
			if (ch == ',') {
				fields.push_back(cur);
				cur.clear();
			} else
				cur.push_back(ch);
		}
		fields.push_back(cur);
		return fields;
	};
	auto toInt = [](const std::string &s, int &out) {
		try {
			std::size_t consumed = 0;
			out = std::stoi(s, &consumed);
			return consumed == s.size();
		} catch (...) {
			return false;
		}
	};
	auto toDouble = [](const std::string &s, double &out) {
		try {
			std::size_t consumed = 0;
			out = std::stod(s, &consumed);
			return consumed == s.size();
		} catch (...) {
			return false;
		}
	};
	while (std::getline(in, line)) {
		if (line.empty())
			continue;
		if (line.rfind("# dims,", 0) == 0) {
			const auto f = split(line);
			if (f.size() != 4 || !toInt(f[1], width) || !toInt(f[2], height) || !toInt(f[3], edgeCount))
				return false;
			continue;
		}
		if (line.rfind("# edges,", 0) == 0) {
			const auto f = split(line);
			if (static_cast<int>(f.size()) != edgeCount + 1)
				return false;
			edges.clear();
			for (std::size_t i = 1; i < f.size(); ++i) {
				double e = 0.0;
				if (!toDouble(f[i], e))
					return false;
				edges.push_back(e);
			}
			continue;
		}
		if (line[0] == '#')
			continue;
		break; // column-header or first data row, parsed below
	}
	if (width <= 0 || height <= 0 || edgeCount < 2 || static_cast<int>(edges.size()) != edgeCount)
		return false;
	// Skip the human-readable column header ("x,y,steady_r,...").
	if (line.rfind("x,y", 0) == 0) {
		if (!std::getline(in, line))
			return false; // header with no data rows is malformed for a non-empty film
	}
	film.width = width;
	film.height = height;
	film.timeEdges = edges;
	const std::size_t binCount = edgeCount - 1;
	film.pixels.resize(static_cast<std::size_t>(width) * height);
	for (AfterglowPixel &p : film.pixels)
		p.bins.assign(binCount, glm::dvec3(0.0));

	const std::size_t expectedCols = 2 + 3 * (4 + binCount);
	auto parseRow = [&](const std::string &row) {
		const auto f = split(row);
		if (f.size() != expectedCols)
			return false;
		int x = 0, y = 0;
		if (!toInt(f[0], x) || !toInt(f[1], y) || x < 0 || x >= width || y < 0 || y >= height)
			return false;
		AfterglowPixel &p = film.pixels[static_cast<std::size_t>(y) * width + x];
		std::size_t k = 2;
		auto readVec = [&] {
			double a = 0.0, b = 0.0, c = 0.0;
			if (!toDouble(f[k], a) || !toDouble(f[k + 1], b) || !toDouble(f[k + 2], c))
				throw std::runtime_error("bad field");
			k += 3;
			return glm::dvec3(a, b, c);
		};
		try {
			p.steady = readVec();
			p.beforeWindow = readVec();
			for (std::size_t b = 0; b < binCount; ++b)
				p.bins[b] = readVec();
			p.afterWindow = readVec();
			p.environment = readVec();
		} catch (...) {
			return false;
		}
		return true;
	};

	if (!parseRow(line)) // the first non-comment line already fetched
		return false;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		if (!parseRow(line))
			return false;
	}
	return true;
}

bool writeSteadyPpm(const AfterglowFilm &film, const std::string &path) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
	out << "P6\n" << film.width << " " << film.height << "\n255\n";
	for (const AfterglowPixel &p : film.pixels) {
		const glm::dvec3 colour = p.steady * 255.0;
		for (int c = 0; c < 3; ++c) {
			int v = static_cast<int>(colour[c]); // truncate, exactly like the classic loop
			v = std::clamp(v, 0, 255);
			out.put(static_cast<char>(v));
		}
	}
	return static_cast<bool>(out);
}
