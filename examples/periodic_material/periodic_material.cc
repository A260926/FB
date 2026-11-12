// Periodic material example for build_periodic_complex()
//
// Author   : Chris H. Rycroft (Harvard University / LBL)
// Email    : chr@alum.mit.edu
// Date     : August 30th 2011
//
// Builds a small periodic Voronoi material inside a sheared parallelepiped
// and prints the shared-face information of the resulting polyhedral
// complex. No windowing or online assets are used: everything is computed
// from the periodic container and printed to standard output.

#include "voro++.hh"
#include "periodic_complex.hh"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace voro;

int main() {
	// Lattice vectors A=(bx,0,0), B=(bxy,by,0), C=(bxz,byz,bz): a mildly
	// sheared periodic domain whose determinant is bx*by*bz.
	const double bx = 1.0, bxy = 0.25, by = 0.9,
	             bxz = 0.10, byz = 0.20, bz = 1.1;

	// A handful of asymmetric seeds in a 3x3x3 computational grid.
	container_periodic con(bx, bxy, by, bxz, byz, bz, 3, 3, 3, 8);
	con.put(0, 0.15, 0.20, 0.30);
	con.put(1, 0.70, 0.65, 0.25);
	con.put(2, 0.40, 0.75, 0.80);
	con.put(3, 0.85, 0.15, 0.60);
	con.put(4, 0.25, 0.50, 0.05);
	con.put(5, 0.55, 0.35, 0.45);

	// One editable complex: shared faces stored once, periodic vertices
	// shared across all cells.
	const periodic_complex pc = build_periodic_complex(con);

	std::printf("periodic material: %zu cells, %zu shared faces, %zu periodic vertices\n",
	            pc.cells.size(), pc.faces.size(), pc.vertices.size());
	std::printf("lattice determinant (domain volume) = %.12g\n", pc.domain_volume());
	std::printf("pairing tolerance = %.3g, minimum vertex separation = %.3g\n\n",
	            pc.pairing_tolerance, pc.minimum_vertex_separation);

	// Print every shared face exactly once, in canonical identity order.
	for (size_t f = 0; f < pc.faces.size(); f++) {
		const pc_face &face = pc.faces[f];
		std::printf("face %2zu: particles %d <-> %d  image_shift=(%d,%d,%d)  %zu-gon\n",
		            f, face.particle_a, face.particle_b,
		            face.image_shift.x, face.image_shift.y, face.image_shift.z,
		            face.corners.size());
		for (size_t t = 0; t < face.corners.size(); t++) {
			const pc_corner &corner = face.corners[t];
			const std::array<double, 3> w = pc.corner_world(corner);
			std::printf("    corner %zu: vertex %d lift=(%d,%d,%d) world=(%.6f,%.6f,%.6f)\n",
			            t, corner.vertex, corner.lift.x, corner.lift.y, corner.lift.z,
			            w[0], w[1], w[2]);
		}
	}

	// Per-cell directed face references and the volume reconstructed from
	// the faces, demonstrating that the complex is a closed tiling.
	std::printf("\ncells (sorted by particle ID):\n");
	double total = 0.0;
	for (size_t i = 0; i < pc.cells.size(); i++) {
		const pc_cell &cell = pc.cells[i];
		int self_faces = 0;
		for (const pc_face_ref &ref : cell.faces) {
			const pc_face &face = pc.faces[ref.face];
			if (face.particle_a == face.particle_b) self_faces++;
		}
		const double v = pc.cell_volume(static_cast<int>(i));
		total += v;
		std::printf("  particle %d: %2zu directed face refs (%d self), volume=%.12g\n",
		            cell.particle, cell.faces.size(), self_faces, v);
	}
	std::printf("sum of face-derived cell volumes = %.12g\n", total);
	return 0;
}
