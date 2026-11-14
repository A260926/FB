/*
examples/adaptive_refinement.cc
Walkthrough of trimesh::AdaptiveRefiner: local refinement, coarsening and
export of the current red-leaf mesh to an OBJ file.

Build (from the repository root):

    c++ -std=c++11 -O2 -Iinclude examples/adaptive_refinement.cc \
        libsrc/TriMesh_*.cc libsrc/subdiv.cc libsrc/KDtree.cc \
        libsrc/remove.cc libsrc/reorder_verts.cc libsrc/AdaptiveRefiner.cc \
        -o adaptive_refinement_example

Usage:

    ./adaptive_refinement_example [output.obj]

If no path is given, "adaptive_refinement.obj" is written next to the
executable's working directory.
*/

#include "AdaptiveRefiner.h"
#include "TriMesh.h"
#include "TriMesh_algo.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace trimesh;
typedef AdaptiveRefiner::NodeId NodeId;

// Build a 6x6 triangulated sheet with a gentle fold so the example is not a
// completely flat patch.  Colors and confidences follow the vertex positions,
// which makes the interpolation visible in the exported file.
static TriMesh make_sheet(int nx, int ny)
{
	TriMesh m;
	std::vector<int> vid((nx + 1) * (ny + 1));
	for (int j = 0; j <= ny; j++) {
		for (int i = 0; i <= nx; i++) {
			float x = 0.1f * i;
			float y = 0.1f * j;
			float z = 0.05f * std::sin(1.1f * x) * std::cos(0.9f * y);
			vid[j * (nx + 1) + i] = (int) m.vertices.size();
			m.vertices.push_back(point(x, y, z));
			m.colors.push_back(Color(x, y, 0.5f + 0.5f * z));
			m.confidences.push_back(x + y);
		}
	}
	for (int j = 0; j < ny; j++) {
		for (int i = 0; i < nx; i++) {
			int a = vid[j * (nx + 1) + i];
			int b = vid[j * (nx + 1) + i + 1];
			int c = vid[(j + 1) * (nx + 1) + i + 1];
			int d = vid[(j + 1) * (nx + 1) + i];
			m.faces.push_back(TriMesh::Face(a, b, c));
			m.faces.push_back(TriMesh::Face(a, c, d));
		}
	}
	return m;
}

static void print_summary(const char *label, const AdaptiveRefiner &r,
                          const AdaptiveRefiner::Snapshot &s)
{
	std::printf("%-22s leaves=%2d  output vertices=%3d  output faces=%3d\n",
	            label, (int) r.leaves().size(),
	            (int) s.mesh.vertices.size(), (int) s.mesh.faces.size());
}

int main(int argc, char **argv)
{
	const char *outfile = (argc > 1) ? argv[1] : "adaptive_refinement.obj";

	TriMesh input = make_sheet(6, 6);
	std::printf("input: %d vertices, %d faces, %d colors, %d confidences\n",
	            (int) input.vertices.size(), (int) input.faces.size(),
	            (int) input.colors.size(), (int) input.confidences.size());

	AdaptiveRefiner refiner(input);

	// Root nodes are simply the initial face indices.
	std::vector<NodeId> roots = refiner.leaves();
	std::printf("initial red leaves: %d (all roots, ids 0..%d)\n",
	            (int) roots.size(), (int) roots.size() - 1);

	// Refine one root face.  Its three edge neighbours are one level coarser
	// across the shared edges, so the closure refines them too.
	refiner.refine({ 0 });
	AdaptiveRefiner::Snapshot s1 = refiner.snapshot();
	print_summary("after refine(0)", refiner, s1);

	// children() reports the four fixed-template children of a refined node.
	std::vector<NodeId> kids = refiner.children(0);
	std::printf("children(0):");
	for (size_t i = 0; i < kids.size(); i++)
		std::printf(" %llu", (unsigned long long) kids[i]);
	std::printf("\n");

	// Refine one of those children again to show persistent IDs: existing
	// midpoint vertices are reused, only genuinely new midpoints are added.
	refiner.refine({ kids[0] });
	AdaptiveRefiner::Snapshot s2 = refiner.snapshot();
	print_summary("after refine(deeper)", refiner, s2);

	// A parent can only be coarsened when all four children are current red
	// leaves; a level gap that would be created by the retraction makes the
	// whole request fail with `false` and no state change.
	bool ok = refiner.coarsen({ kids[0] });
	std::printf("coarsen(child of 0): %s\n", ok ? "true" : "false (blocked)");

	// Coarsen the original root: legal now that the deeper child is gone.
	ok = refiner.coarsen({ 0 });
	std::printf("coarsen(0): %s\n", ok ? "true" : "false (blocked)");
	AdaptiveRefiner::Snapshot s3 = refiner.snapshot();
	print_summary("after coarsening", refiner, s3);

	// Export through the ordinary TriMesh operators.  Snapshot meshes contain
	// no derived data, so computing normals here exercises the regular path.
	AdaptiveRefiner::Snapshot out = refiner.snapshot();
	out.mesh.need_normals();
	out.mesh.need_pointareas();
	if (out.mesh.write(outfile))
		std::printf("wrote %s (%d faces, root_face/leaf_id parallel to faces)\n",
		            outfile, (int) out.mesh.faces.size());
	else
		std::printf("could not write %s\n", outfile);

	// Show the per-face provenance of the first few exported triangles.
	std::printf("first exported faces: root_face / leaf_id\n");
	for (int i = 0; i < (int) out.mesh.faces.size() && i < 8; i++)
		std::printf("  face %3d -> root %3d, leaf %llu\n", i,
		            out.root_face[i],
		            (unsigned long long) out.leaf_id[i]);

	return 0;
}
