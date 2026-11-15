/*
split_sheets.cc

Example: build a small model made of several sheets that share indexed edges
(two crossing fins, a butterfly vertex, and a pair of sheets joined along an
whole strip), run the non-manifold sheet splitter on it, and write the repaired
mesh to PLY with freshly computed normals.

Usage: split_sheets [output.ply]

The splitter keeps every original face, corner order and vertex position, and
only duplicates the vertices that are needed to turn each vertex fan into a
single open chain or cycle.  It never flips faces, never welds vertices by
spatial position, and if the locally optimal edge pairing would make two
distinct input edges collapse onto the same output edge (a quotient-edge
conflict) it rejects the input instead of silently breaking a pairing.

Outputs one PLY file (default: split_sheets_example.ply) and prints a short
summary to stdout.
*/

#include "TriMesh.h"
#include "TriMesh_algo.h"

#include <cstdio>
#include <cstdlib>
#include <string>


using namespace trimesh;


// Build a model with several non-manifold junctions:
//  - two fins crossing on index edge (0,1),
//  - a butterfly vertex 2 where two triangles only touch,
//  - a folded strip that shares its edges with another strip.
static TriMesh make_example()
{
	TriMesh m;

	// Crossing fins over edge (0,1): two forward and two backward faces.
	m.vertices.push_back(point(0, 0, 0));     // 0
	m.vertices.push_back(point(1, 0, 0));     // 1
	m.vertices.push_back(point(0.1f, 1.2f, 0.4f));   // 2
	m.vertices.push_back(point(1.1f, -0.9f, 0.6f));  // 3
	m.vertices.push_back(point(0.2f, 1.6f, -0.5f));  // 4
	m.vertices.push_back(point(1.2f, -1.3f, 0.2f));  // 5

	// A butterfly: two triangles that only meet at vertex 6.
	m.vertices.push_back(point(0.5f, 0, 1.5f));      // 6
	m.vertices.push_back(point(1.4f, 0.2f, 2.1f));   // 7
	m.vertices.push_back(point(0.1f, 0.6f, 2.2f));   // 8
	m.vertices.push_back(point(-0.6f, -0.1f, 1.3f)); // 9
	m.vertices.push_back(point(0.3f, -0.8f, 1.1f));  // 10

	// A folded strip pair sharing a chain of edges.
	m.vertices.push_back(point(2.0f, 0.0f, 0.0f));   // 11
	m.vertices.push_back(point(3.0f, 0.0f, 0.0f));   // 12
	m.vertices.push_back(point(4.0f, 0.0f, 0.0f));   // 13
	m.vertices.push_back(point(5.0f, 0.0f, 0.0f));   // 14
	m.vertices.push_back(point(2.0f, 0.0f, 0.8f));   // 15
	m.vertices.push_back(point(3.0f, 0.0f, 0.8f));   // 16
	m.vertices.push_back(point(4.0f, 0.0f, 0.8f));   // 17
	m.vertices.push_back(point(5.0f, 0.0f, 0.8f));   // 18

	m.faces.push_back(TriMesh::Face(0, 1, 2));
	m.faces.push_back(TriMesh::Face(1, 0, 3));
	m.faces.push_back(TriMesh::Face(0, 1, 4));
	m.faces.push_back(TriMesh::Face(1, 0, 5));
	m.faces.push_back(TriMesh::Face(6, 7, 8));
	m.faces.push_back(TriMesh::Face(6, 9, 10));
	m.faces.push_back(TriMesh::Face(11, 12, 15));
	m.faces.push_back(TriMesh::Face(12, 16, 15));
	m.faces.push_back(TriMesh::Face(12, 13, 16));
	m.faces.push_back(TriMesh::Face(13, 17, 16));
	m.faces.push_back(TriMesh::Face(13, 14, 17));
	m.faces.push_back(TriMesh::Face(14, 18, 17));
	return m;
}


int main(int argc, char *argv[])
{
	const char *outfilename = (argc > 1) ? argv[1] : "split_sheets_example.ply";

	TriMesh mesh = make_example();
	printf("Input:  %d vertices, %d faces\n",
	       int(mesh.vertices.size()), int(mesh.faces.size()));

	SheetSplitResult result;
	if (!split_nonmanifold_sheets(&mesh, result)) {
		// A false return means the input was rejected (bad indices or
		// geometry, attribute length mismatch, too many faces on one
		// edge, or a quotient-edge conflict after the locally optimal
		// matching).  Nothing was modified.
		fprintf(stderr, "Input was rejected: no sheet split performed.\n");
		return 1;
	}

	int original_slots = 0;
	for (size_t i = 0; i < result.vertex_source.size(); ++i)
		if (result.vertex_source[i] == int(i))
			original_slots++;
	printf("Split:  %d vertices, %d faces (%d input slots retained, "
	       "%d duplicates appended)\n",
	       int(mesh.vertices.size()), int(mesh.faces.size()),
	       original_slots, int(result.vertex_source.size()) - original_slots);

	// All derived data was invalidated by the split, so rebuild normals from
	// the new topology.  Each split sheet now gets its own normal instead of
	// averaging across the cut.
	mesh.need_normals();

	if (!mesh.write(std::string("norm:") + outfilename)) {
		fprintf(stderr, "Couldn't write %s\n", outfilename);
		return 1;
	}

	// Report how many face edges were paired across all index edges.
	mesh.need_across_edge();
	int paired = 0;
	for (size_t f = 0; f < result.edge_partner.size(); ++f)
		for (int k = 0; k < 3; ++k)
			if (result.edge_partner[f][k] >= 0)
				paired++;
	printf("Selected %d paired face edges across all index edges.\n",
	       paired / 2);
	printf("Wrote %s\n", outfilename);
	return 0;
}
