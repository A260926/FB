// Voro++, a 3D cell-based Voronoi library
//
// Author   : Chris H. Rycroft (Harvard University / LBL)
// Email    : chr@alum.mit.edu
// Date     : August 30th 2011

/** \file periodic_complex.hh
 * \brief Header file for the periodic_complex structure and the
 * build_periodic_complex() routines.
 *
 * A periodic_complex is an editable, quotient-space description of a
 * periodic Voronoi tessellation. Unlike the per-cell triangle soups that the
 * drawing routines emit, the complex stores every shared face exactly once,
 * keeps one global index per periodic vertex, and carries the integer
 * lattice image information (the "image shift") next to every incidence.
 *
 * The periodic domain is the one already used by container_periodic and
 * container_periodic_poly, defined by the lattice vectors
 *
 *   A = (bx, 0, 0), B = (bxy, by, 0), C = (bxz, byz, bz).
 *
 * Conventions used throughout this file:
 *
 * - A vertex representative stores lattice fractional coordinates in
 *   [0,1). Its world position is fx*A + fy*B + fz*C.
 * - A face corner stores (vertex index, lift). The actual world position of
 *   the corner is the world position of the vertex representative plus
 *   lift.x*A + lift.y*B + lift.z*C.
 * - A face between particle i and the k-th lattice image of particle j is
 *   canonically identified by (i, j, k). It is stored once. The same face
 *   seen from j is (j, i, -k). For i == j the two opposite self images are
 *   still distinct faces; different k values are never merged.
 * - Canonical orientation: for i != j the face is stored on the side of the
 *   smaller particle ID, with image_shift k pointing from that particle to
 *   the image of the other one. For i == j the direction whose first
 *   non-zero component is positive is used. The stored corner ring follows
 *   the outward normal of the canonical first cell, i.e. it is
 *   counterclockwise when viewed from outside that cell.
 * - A cell face reference therefore carries an orientation and a lattice
 *   translation. orientation == +1 means the stored ring is used as-is, and
 *   the lattice translation is (0,0,0). orientation == -1 means the ring
 *   must be traversed in reverse; corner lifts in that cell's local frame
 *   equal the stored lifts plus lattice_translation (which is -image_shift
 *   for the canonical second side).
 */

#ifndef VOROPP_PERIODIC_COMPLEX_HH
#define VOROPP_PERIODIC_COMPLEX_HH

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "container_prd.hh"

namespace voro {

/** Exception type thrown when a periodic complex cannot be built reliably:
 * invalid input, empty cells, or geometric pairings that are ambiguous at
 * the required tolerance. The original container seed data is never
 * modified when this is thrown. */
class periodic_complex_error : public std::runtime_error {
	public:
		explicit periodic_complex_error(const std::string &message)
			: std::runtime_error(message) {}
};

/** A triplet of integer lattice indices. */
struct pc_ivec3 {
	int x, y, z;
	pc_ivec3() : x(0), y(0), z(0) {}
	pc_ivec3(int x_, int y_, int z_) : x(x_), y(y_), z(z_) {}
	bool operator==(const pc_ivec3 &o) const {
		return x == o.x && y == o.y && z == o.z;
	}
	bool operator!=(const pc_ivec3 &o) const { return !(*this == o); }
	pc_ivec3 operator+(const pc_ivec3 &o) const {
		return pc_ivec3(x + o.x, y + o.y, z + o.z);
	}
	pc_ivec3 operator-(const pc_ivec3 &o) const {
		return pc_ivec3(x - o.x, y - o.y, z - o.z);
	}
	pc_ivec3 operator-() const { return pc_ivec3(-x, -y, -z); }
};

/** One corner of a face ring. The corner's world position is
 * world(vertex) + lift*lattice. */
struct pc_corner {
	/** Global index into periodic_complex::vertices. */
	int vertex;
	/** Integer lattice lift applied to the vertex representative. */
	pc_ivec3 lift;
};

/** A directed reference from a cell to one of its faces. */
struct pc_face_ref {
	/** Index into periodic_complex::faces. */
	int face;
	/** +1: the stored outward-normal ring belongs to this cell.
	 *  -1: the cell sees the same polygon with reversed winding, in
	 *  a translated periodic frame. */
	int orientation;
	/** Lattice translation that maps the stored face frame into this
	 * cell's local frame. Zero for the canonical first side. */
	pc_ivec3 lattice_translation;
};

/** One Voronoi cell, identified by its original particle ID. */
struct pc_cell {
	/** The particle ID supplied to container_periodic::put() (or the
	 * poly variant). It is never remapped to a dense index. */
	int particle;
	/** Directed face incidences, one per face of the cell. A periodic
	 * self face appears twice here (once per image direction). */
	std::vector<pc_face_ref> faces;
};

/** One shared face of the periodic tessellation. Stored exactly once. */
struct pc_face {
	/** Canonical first particle ID. For particle_a != particle_b this
	 * is the smaller of the two IDs; for self faces particle_a ==
	 * particle_b. */
	int particle_a;
	/** Canonical second particle ID. */
	int particle_b;
	/** Canonical image shift k: the face separates particle_a from
	 * the image of particle_b at particle_b's primary position plus
	 * k*lattice. The reverse incidence is (particle_b, particle_a,
	 * -image_shift). */
	pc_ivec3 image_shift;
	/** Corner ring ordered by the outward normal of the cell of
	 * particle_a. */
	std::vector<pc_corner> corners;
};

/** An editable polyhedral complex of a periodic Voronoi tessellation.
 *
 * cells are sorted by particle ID. faces are sorted by the canonical
 * identity (particle_a, particle_b, image_shift). Vertex numbering is
 * stable across repeated builds of identical input. Every periodic vertex
 * has exactly one entry in vertices and is shared by all cells touching
 * it. */
struct periodic_complex {
	/** @name Lattice geometry, identical to the container.
	 * A=(bx,0,0), B=(bxy,by,0), C=(bxz,byz,bz). */
	///@{
	double bx, bxy, by, bxz, byz, bz;
	///@}

	/** Absolute geometric pairing tolerance: 1e-9 times the longest
	 * lattice vector. */
	double pairing_tolerance;
	/** Guaranteed minimum separation of distinct periodic vertices:
	 * 1e-6 times the longest lattice vector. */
	double minimum_vertex_separation;

	/** Vertex representatives as lattice fractional coordinates,
	 * each component in [0,1). */
	std::vector<std::array<double, 3> > vertices;

	/** Shared faces, sorted by canonical identity. */
	std::vector<pc_face> faces;

	/** Cells, sorted by particle ID. */
	std::vector<pc_cell> cells;

	/** Converts lattice fractional coordinates to world coordinates. */
	std::array<double, 3> fractional_to_world(double fx, double fy, double fz) const;
	/** Converts world coordinates to lattice fractional coordinates. */
	std::array<double, 3> world_to_fractional(double wx, double wy, double wz) const;
	/** World position of a vertex representative. */
	std::array<double, 3> vertex_world(int vertex) const;
	/** World position of a face corner (representative plus lift). */
	std::array<double, 3> corner_world(const pc_corner &corner) const;
	/** World position of a corner seen through a cell face reference
	 * (applies the reference's lattice translation). */
	std::array<double, 3> corner_world(const pc_corner &corner, const pc_face_ref &ref) const;

	/** Returns the oriented corner ring of a cell as seen from that
	 * cell: reversed when the reference orientation is -1, with lifts
	 * shifted by the reference's lattice translation. */
	std::vector<pc_corner> oriented_ring(const pc_face_ref &ref) const;

	/** Signed volume of one cell, computed by triangulating its
	 * oriented face rings in the frame of the cell's primary
	 * particle. */
	double cell_volume(int cell_index) const;

	/** Absolute value of the lattice determinant, i.e. the domain
	 * volume. */
	double domain_volume() const { return bx * by * bz; }
};

/** Builds the periodic complex for an unweighted (single-radius)
 * tessellation. Throws periodic_complex_error on invalid seed data, an
 * empty cell, or an unreliable geometric pairing. The container's seed
 * particles are never modified; its dynamic periodic-image cache may be
 * populated as part of the normal cell computation. */
periodic_complex build_periodic_complex(container_periodic &con);

/** Builds the periodic complex for a radical (radius-weighted)
 * tessellation. Same failure and determinism guarantees as the
 * unweighted overload. */
periodic_complex build_periodic_complex(container_periodic_poly &con);

}

#endif
