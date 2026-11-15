// Afterglow path-tracer example. Builds a small room entirely in C++: diffuse
// surfaces carrying two different exponential lifetimes, a mirror panel, an
// emissive ceiling panel and two coloured point lights at different distances.
// Renders the temporal film with renderAfterglow and writes the double-precision
// CSV plus a steady-state PPM preview into the directory given on the command
// line:  example <output-dir>  ->  <dir>/afterglow.csv, <dir>/steady.ppm
#include "Afterglow.h"
#include "Camera.h"
#include "Light.h"
#include <Colour.h>
#include <ModelTriangle.h>
#include <filesystem>
#include <glm/glm.hpp>
#include <iostream>
#include <vector>

namespace {
ModelTriangle makeTriangle(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c, const Colour &colour,
                           Material material, double tauNs = 0.0) {
	ModelTriangle tri(a, b, c, colour);
	tri.material = material;
	const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
	tri.normal = normal;
	tri.vertexNormals = {normal, normal, normal};
	tri.afterglowTauNs = tauNs;
	return tri;
}

// A rectangle split into two triangles with consistent winding.
void addQuad(std::vector<ModelTriangle> &mesh, const glm::vec3 &p0, const glm::vec3 &p1, const glm::vec3 &p2,
             const glm::vec3 &p3, const Colour &colour, Material material, double tauNs = 0.0) {
	mesh.push_back(makeTriangle(p0, p1, p2, colour, material, tauNs));
	mesh.push_back(makeTriangle(p0, p2, p3, colour, material, tauNs));
}
} // namespace

int main(int argc, char **argv) {
	if (argc != 2) {
		std::cerr << "usage: " << argv[0] << " <output-directory>\n";
		return 2;
	}
	const std::filesystem::path outDir = argv[1];
	std::error_code ec;
	std::filesystem::create_directories(outDir, ec);
	if (ec) {
		std::cerr << "cannot create output directory: " << ec.message() << '\n';
		return 2;
	}

	constexpr double kShortTauNs = 2.0;   // fast-decaying diffuse coating
	constexpr double kLongTauNs = 25.0;   // long-persistence diffuse coating

	std::vector<ModelTriangle> mesh;
	const Colour floorColour(170, 160, 150);
	const Colour wallColour(150, 170, 190);
	const Colour backColour(190, 160, 170);

	// Floor and back wall carry the short lifetime; the side walls the long one.
	addQuad(mesh, {-2.0f, -1.0f, -2.0f}, {2.0f, -1.0f, -2.0f}, {2.0f, -1.0f, 2.0f},
	        {-2.0f, -1.0f, 2.0f}, floorColour, Material::Diffuse, kShortTauNs);
	addQuad(mesh, {-2.0f, -1.0f, -2.0f}, {-2.0f, 2.0f, -2.0f}, {2.0f, 2.0f, -2.0f},
	        {2.0f, -1.0f, -2.0f}, backColour, Material::Diffuse, kShortTauNs);
	addQuad(mesh, {-2.0f, -1.0f, 2.0f}, {-2.0f, -1.0f, -2.0f}, {-2.0f, 2.0f, -2.0f},
	        {-2.0f, 2.0f, 2.0f}, wallColour, Material::Diffuse, kLongTauNs);
	addQuad(mesh, {2.0f, -1.0f, -2.0f}, {2.0f, -1.0f, 2.0f}, {2.0f, 2.0f, 2.0f},
	        {2.0f, 2.0f, -2.0f}, wallColour, Material::Diffuse, kLongTauNs);

	// Emissive ceiling panel: every path that reaches it records an instantaneous
	// emission contribution at the panel's geometric flight time.
	addQuad(mesh, {-1.2f, 2.0f, -1.2f}, {1.2f, 2.0f, -1.2f}, {1.2f, 2.0f, 0.4f},
	        {-1.2f, 2.0f, 0.4f}, Colour(255, 240, 210), Material::Emissive);

	// A mirror panel on the back wall, lifetime fixed at 0 as required.
	addQuad(mesh, {-0.7f, -0.6f, -1.98f}, {0.7f, -0.6f, -1.98f}, {0.7f, 1.2f, -1.98f},
	        {-0.7f, 1.2f, -1.98f}, Colour(230, 230, 235), Material::Mirror);

	// Two finite, radius-0 point lights: a warm near one and a cooler far one.
	Light nearLight;
	nearLight.position = {1.2f, 1.0f, 1.0f};
	nearLight.colour = {1.0f, 0.45f, 0.35f};
	nearLight.intensity = 20.0f;
	Light farLight;
	farLight.position = {-1.3f, 0.8f, -1.2f};
	farLight.colour = {0.35f, 0.5f, 1.0f};
	farLight.intensity = 30.0f;

	constexpr int kWidth = 48;
	constexpr int kHeight = 36;
	constexpr int kSamples = 8;
	Camera camera(kWidth, kHeight, 2.0f, glm::vec3(0.0f, 0.2f, 4.0f));
	camera.scale = 40.0f;
	camera.lookAt(glm::vec3(0.0f, -0.2f, -1.0f));

	// Finite, strictly increasing, deliberately non-uniform time edges (ns).
	// With the metric scale below, geometric flights are sub-nanosecond, so the
	// two coatings (means 2 ns and 25 ns) and the instantaneous emissive panel
	// separate clearly across the bins.
	const std::vector<double> edges = {0.0, 1.0, 5.0, 20.0, 60.0};

	// One world unit is 2 cm: a 10-unit path takes well under a nanosecond of
	// flight time, leaving the exponential dwell kernels to dominate timing.
	constexpr double kMetersPerUnit = 0.02;
	const AfterglowFilm film =
	    renderAfterglow(mesh, camera, {nearLight, farLight}, kWidth, kHeight, kSamples, kMetersPerUnit, edges);

	const auto csvPath = outDir / "afterglow.csv";
	const auto ppmPath = outDir / "steady.ppm";
	if (!writeAfterglowCsv(film, csvPath.string())) {
		std::cerr << "failed to write " << csvPath << '\n';
		return 1;
	}
	if (!writeSteadyPpm(film, ppmPath.string())) {
		std::cerr << "failed to write " << ppmPath << '\n';
		return 1;
	}
	std::cout << "afterglow render: " << kWidth << 'x' << kHeight << " spp=" << kSamples << " bins=" << edges.size() - 1
	          << "\nwrote " << csvPath << "\nwrote " << ppmPath << '\n';
	return 0;
}
