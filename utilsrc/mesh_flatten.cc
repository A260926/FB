/*
Flatten a triangulated topological disk into the plane with a discrete
conformal (angle-preserving) map.

Usage: mesh_flatten <input> <output>

The boundary curvature targets default to the uniform value 2*pi/n for the
n boundary vertices.  The result is written with the library's usual
TriMesh::write, so the output format follows the output file extension.
On any failure the program exits non-zero and does not leave a partially
written output file behind.
*/

#include "ConformalMap.h"
#include "TriMesh.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

using namespace trimesh;

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTwoPi = 2.0 * kPi;

int usage(const char *argv0)
{
	std::fprintf(stderr, "Usage: %s <input> <output>\n", argv0);
	return 2;
}

} // anonymous namespace

int main(int argc, char **argv)
{
	if (argc != 3)
		return usage(argv[0]);
	TriMesh::set_verbose(0);

	std::unique_ptr<TriMesh> mesh(TriMesh::read(argv[1]));
	if (!mesh) {
		std::fprintf(stderr, "mesh_flatten: could not read '%s'\n", argv[1]);
		return 1;
	}

	try {
		ConformalMap cm(*mesh);
		const std::vector<int> &loop = cm.boundary_loop();
		if (loop.empty()) {
			std::fprintf(stderr, "mesh_flatten: no boundary loop\n");
			return 1;
		}
		std::vector<double> targets(mesh->vertices.size(), 0.0);
		const double uniform = kTwoPi / (double) loop.size();
		for (int v : loop)
			targets[v] = uniform;

		ConformalMap::Result res =
			cm.solve(targets, 1e-10, 500, 200000);
		if (!res.ok()) {
			std::fprintf(stderr,
			             "mesh_flatten: solve failed (%s): %s\n",
			             ConformalMap::status_name(res.status),
			             res.diagnostics.last_error.c_str());
			return 1;
		}

		// Write to a temporary sibling and rename only on success, so a
		// failed write never leaves a half-finished output file.
		const std::string out = argv[2];
		const std::string tmp = out + ".tmp-flatten";
		if (!res.flat.write(tmp)) {
			std::remove(tmp.c_str());
			std::fprintf(stderr, "mesh_flatten: could not write '%s'\n",
			             tmp.c_str());
			return 1;
		}
		if (std::rename(tmp.c_str(), out.c_str()) != 0) {
			std::remove(tmp.c_str());
			std::fprintf(stderr, "mesh_flatten: could not rename to '%s'\n",
			             out.c_str());
			return 1;
		}
		std::fprintf(stderr,
		             "mesh_flatten: %d vertices, %d faces, %d steps, "
		             "%d flips, max residual %.3g\n",
		             (int) res.flat.vertices.size(),
		             (int) res.flat.faces.size(),
		             res.diagnostics.steps, res.diagnostics.flips,
		             res.diagnostics.max_residual);
	} catch (const std::exception &e) {
		std::fprintf(stderr, "mesh_flatten: %s\n", e.what());
		return 1;
	}
	return 0;
}
