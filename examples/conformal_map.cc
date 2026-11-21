/*
Example: flatten a disk mesh into the plane with ConformalMap and write the
result as flat.ply into a caller-supplied output directory.

The output triangulation generally differs from the input: edges may have
been flipped so that the result is Delaunay in the final discrete conformal
metric.  Vertex numbers, the boundary loop, colors and confidences are
preserved; the 3D geometry is replaced by (x, y, 0) plane coordinates.
*/

#include "ConformalMap.h"
#include "TriMesh.h"

#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <vector>

using namespace trimesh;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559005768;

// A simple closed umbrella-like disk: n boundary vertices around a circle
// in 3D plus one apex, triangulated as a fan.  The surface is deliberately
// non-planar so the flattening is non-trivial.
TriMesh make_example_disk(int n)
{
	TriMesh m;
	m.vertices.push_back(point(0.0f, 0.0f, 1.4f)); // apex, vertex 0
	for (int i = 0; i < n; i++) {
		double a = kTwoPi * (double) i / (double) n;
		float r = 1.0f + 0.15f * (float) std::sin(3.0 * a);
		m.vertices.push_back(point((float) (r * std::cos(a)),
		                           (float) (r * std::sin(a)),
		                           0.2f * (float) std::sin(2.0 * a)));
	}
	// Fan from the apex, wound counter-clockwise seen from +Z.
	for (int i = 0; i < n; i++)
		m.faces.push_back(TriMesh::Face(0, 1 + i, 1 + (i + 1) % n));
	return m;
}

} // anonymous namespace

int main(int argc, char **argv)
{
	if (argc != 2) {
		std::fprintf(stderr, "Usage: %s <output-directory>\n", argv[0]);
		return 2;
	}
	TriMesh::set_verbose(0);
	const std::string outdir = argv[1];
	const std::string outpath = outdir + "/flat.ply";

	try {
		TriMesh mesh = make_example_disk(17);
		ConformalMap cm(mesh);
		std::vector<double> targets(mesh.vertices.size(), 0.0);
		double uniform = kTwoPi / (double) cm.boundary_loop().size();
		for (int v : cm.boundary_loop())
			targets[v] = uniform;

		ConformalMap::Result res = cm.solve(targets, 1e-10, 500, 200000);
		if (!res.ok()) {
			std::fprintf(stderr, "conformal_map: %s (residual %.3g)\n",
			             ConformalMap::status_name(res.status),
			             res.diagnostics.max_residual);
			return 1;
		}
		if (!res.flat.write(outpath)) {
			std::fprintf(stderr, "conformal_map: could not write %s\n",
			             outpath.c_str());
			return 1;
		}
		std::printf("conformal_map: wrote %s\n", outpath.c_str());
		std::printf("  input : %d vertices, %d faces\n",
		            (int) mesh.vertices.size(), (int) mesh.faces.size());
		std::printf("  output: %d vertices, %d faces, %d flips\n",
		            (int) res.flat.vertices.size(),
		            (int) res.flat.faces.size(),
		            res.diagnostics.flips);
		std::printf("  max angle-defect residual: %.3g\n",
		            res.diagnostics.max_residual);
		std::printf(
			"  Note: the output connectivity is the Delaunay triangulation "
			"of the final intrinsic conformal metric.  It shares vertex\n"
			"  numbers and the boundary loop with the input, but edges may "
			"differ where the solve flipped them; the output is the flat\n"
			"  (x, y, 0) chart of that metric, not a per-triangle texture "
			"map of the original surface.\n");
	} catch (const std::exception &e) {
		std::fprintf(stderr, "conformal_map: %s\n", e.what());
		return 1;
	}
	return 0;
}
