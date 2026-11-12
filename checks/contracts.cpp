// Contract tests for build_periodic_complex().
//
// The cases are registered in the three groups required by qa_main.cpp:
// lattice-faces, quotient-topology, and weighted-reuse. Every callback
// drives Check::require at least once and returns 0 on success; returning
// 77 would be reported as a skipped/failed case.

#include "qa.hpp"
#include "voro++.hh"
#include "periodic_complex.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace {

using voro::pc_corner;
using voro::pc_face_ref;
using voro::pc_ivec3;
using voro::periodic_complex;
using voro::periodic_complex_error;

constexpr double EPS = 1e-8;

double norm3(const std::array<double, 3> &v) {
	return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

double dist3(const std::array<double, 3> &a, const std::array<double, 3> &b) {
	return norm3({a[0] - b[0], a[1] - b[1], a[2] - b[2]});
}

template <class container_class>
std::map<int, double> library_volumes(container_class &con) {
	std::map<int, double> volumes;
	voro::voronoicell_neighbor cell(con);
	voro::c_loop_all_periodic loop(con);
	if (loop.start()) do {
		if (!con.compute_cell(cell, loop))
			throw periodic_complex_error("contract: library failed to compute a cell");
		volumes[loop.pid()] = cell.volume();
	} while (loop.inc());
	return volumes;
}

template <class container_class>
std::map<int, std::array<double, 3> > particle_positions(container_class &con) {
	std::map<int, std::array<double, 3> > positions;
	voro::c_loop_all_periodic loop(con);
	if (loop.start()) do {
		double x, y, z;
		loop.pos(x, y, z);
		positions[loop.pid()] = {x, y, z};
	} while (loop.inc());
	return positions;
}

int cell_index_of(const periodic_complex &pc, int particle) {
	for (int i = 0; i < static_cast<int>(pc.cells.size()); i++)
		if (pc.cells[i].particle == particle) return i;
	return -1;
}

// Structural equality of two independently built complexes. Vertex numbers
// are allowed to be permuted, so representatives are matched as periodic
// point sets (minimum-image distance) and rings are compared cyclically
// through the resulting bijection.
bool same_complex(const periodic_complex &a, const periodic_complex &b) {
	if (a.vertices.size() != b.vertices.size()) return false;
	if (a.faces.size() != b.faces.size()) return false;
	if (a.cells.size() != b.cells.size()) return false;
	const double match_tol = 1e-10;

	// Bijection between the two periodic vertex sets.
	std::vector<int> a_to_b(a.vertices.size(), -1);
	std::vector<int> b_to_a(b.vertices.size(), -1);
	for (size_t i = 0; i < a.vertices.size(); i++) {
		int found = -1;
		for (size_t j = 0; j < b.vertices.size(); j++) {
			std::array<double, 3> df;
			for (int c = 0; c < 3; c++) {
				df[c] = a.vertices[i][c] - b.vertices[j][c];
				df[c] -= std::round(df[c]);
			}
			const auto dw = a.fractional_to_world(df[0], df[1], df[2]);
			if (norm3(dw) <= match_tol) {
				if (found != -1) return false;
				found = static_cast<int>(j);
			}
		}
		if (found < 0 || b_to_a[found] != -1) return false;
		a_to_b[i] = found;
		b_to_a[found] = static_cast<int>(i);
	}

	auto ring_key = [](const std::vector<pc_corner> &ring, const std::vector<int> *map) {
		std::vector<std::tuple<int, int, int, int> > key;
		for (const auto &corner : ring) {
			const int v = map ? (*map)[corner.vertex] : corner.vertex;
			key.emplace_back(v, corner.lift.x, corner.lift.y, corner.lift.z);
		}
		return key;
	};
	for (size_t i = 0; i < a.faces.size(); i++) {
		const auto &fa = a.faces[i];
		const auto &fb = b.faces[i];
		if (fa.particle_a != fb.particle_a || fa.particle_b != fb.particle_b) return false;
		if (fa.image_shift != fb.image_shift) return false;
		if (fa.corners.size() != fb.corners.size()) return false;
		const auto ka = ring_key(fa.corners, &a_to_b);
		const auto kb = ring_key(fb.corners, nullptr);
		const size_t n = ka.size();
		bool cyclic = false;
		for (size_t rot = 0; rot < n && !cyclic; rot++) {
			bool ok = true;
			for (size_t t = 0; t < n; t++)
				if (ka[t] != kb[(t + rot) % n]) { ok = false; break; }
			cyclic = ok;
		}
		if (!cyclic) return false;
	}
	// Cell face references are compared as incidence sets, because the
	// per-cell face enumeration order is not part of the topology.
	for (size_t i = 0; i < a.cells.size(); i++) {
		if (a.cells[i].particle != b.cells[i].particle) return false;
		if (a.cells[i].faces.size() != b.cells[i].faces.size()) return false;
		// Compare on the full (identity, orientation, translation) tuple.
		std::set<std::tuple<int, int, int, int, int, int, int, int, int> > sa, sb;
		for (const auto &r : a.cells[i].faces) {
			const auto &f = a.faces[r.face];
			sa.emplace(f.particle_a, f.particle_b, f.image_shift.x, f.image_shift.y,
			           f.image_shift.z, r.orientation, r.lattice_translation.x,
			           r.lattice_translation.y, r.lattice_translation.z);
		}
		for (const auto &r : b.cells[i].faces) {
			const auto &f = b.faces[r.face];
			sb.emplace(f.particle_a, f.particle_b, f.image_shift.x, f.image_shift.y,
			           f.image_shift.z, r.orientation, r.lattice_translation.x,
			           r.lattice_translation.y, r.lattice_translation.z);
		}
		if (sa != sb) return false;
	}
	return true;
}

// Every shared face must appear on exactly its two sides, with reversed
// winding and matching lifted corner positions.
void check_face_opposites(Check &check, const periodic_complex &pc) {
	auto matches = [&](const pc_face_ref &r, const voro::pc_face &face) {
		const auto &g = pc.faces[r.face];
		return g.particle_a == face.particle_a && g.particle_b == face.particle_b &&
		       g.image_shift == face.image_shift;
	};
	for (const auto &face : pc.faces) {
		const int ia = cell_index_of(pc, face.particle_a);
		const int ib = cell_index_of(pc, face.particle_b);
		check.require(ia >= 0 && ib >= 0, "face references an unknown cell");
		std::vector<pc_face_ref> refs_a, refs_b;
		for (const auto &r : pc.cells[ia].faces) if (matches(r, face)) refs_a.push_back(r);
		if (ia != ib)
			for (const auto &r : pc.cells[ib].faces) if (matches(r, face)) refs_b.push_back(r);
		if (face.particle_a == face.particle_b) {
			check.require(refs_a.size() == 2, "self face must be referenced twice by its cell");
			refs_b = refs_a;
		} else {
			check.require(refs_a.size() == 1 && refs_b.size() == 1,
				"inter-particle face must be referenced once per cell");
		}
		const pc_face_ref *ra = nullptr, *rb = nullptr;
		for (const auto &r : refs_a) if (r.orientation == 1) ra = &r;
		for (const auto &r : refs_b) if (r.orientation == -1) rb = &r;
		check.require(ra != nullptr && rb != nullptr, "face missing a directed incidence");
		if (!ra || !rb) continue;
		check.require(ra->orientation == 1, "canonical first side uses the stored orientation");
		check.require(rb->orientation == -1, "opposite side reverses the ring");
		const auto ring_a = pc.oriented_ring(*ra);
		const auto ring_b = pc.oriented_ring(*rb);
		check.require(ring_a.size() == ring_b.size(), "opposite rings have different lengths");
		const size_t n = ring_a.size();
		for (size_t t = 0; t < n; t++) {
			const auto &ca = ring_a[t];
			const auto &cb = ring_b[n - 1 - t];
			check.require(ca.vertex == cb.vertex, "opposite rings use different periodic vertices");
			check.require(pc.vertices[ca.vertex] == pc.vertices[cb.vertex],
				"opposite ring vertex index mismatch");
			// The b-side ring sits in the frame translated by -k; shift
			// it back by +k before comparing geometric positions.
			const pc_corner cb_shifted{cb.vertex,
				pc_ivec3(cb.lift.x + face.image_shift.x,
				         cb.lift.y + face.image_shift.y,
				         cb.lift.z + face.image_shift.z)};
			const auto wa = pc.corner_world(ca);
			const auto wb = pc.corner_world(cb_shifted);
			check.require(dist3(wa, wb) < EPS, "opposite corners do not coincide after the frame shift");
			// Lift relationship across the face: b-side lift = a-side lift - k.
			const pc_ivec3 expected(ca.lift.x - face.image_shift.x,
			                        ca.lift.y - face.image_shift.y,
			                        ca.lift.z - face.image_shift.z);
			check.require(cb.lift == expected, "opposite corner lift is not shifted by -image_shift");
		}
	}
}

// Each cell is a closed orientable manifold: every directed edge occurs
// once and is paired with its reverse.
void check_closure(Check &check, const periodic_complex &pc) {
	struct corner_id {
		int vertex;
		pc_ivec3 lift;
		bool operator<(const corner_id &o) const {
			return std::tie(vertex, lift.x, lift.y, lift.z) <
			       std::tie(o.vertex, o.lift.x, o.lift.y, o.lift.z);
		}
	};
	struct edge_id {
		corner_id u, v;
		bool operator<(const edge_id &o) const {
			return std::tie(u, v) < std::tie(o.u, o.v);
		}
	};
	for (const auto &cell : pc.cells) {
		std::map<edge_id, int> edges;
		for (const auto &ref : cell.faces) {
			const auto ring = pc.oriented_ring(ref);
			check.require(ring.size() >= 3, "face has fewer than three corners");
			for (size_t t = 0; t < ring.size(); t++) {
				edge_id e{{ring[t].vertex, ring[t].lift},
				          {ring[(t + 1) % ring.size()].vertex,
				           ring[(t + 1) % ring.size()].lift}};
				check.require(++edges[e] == 1, "duplicate directed edge inside a cell");
			}
		}
		for (const auto &entry : edges) {
			edge_id rev{entry.first.v, entry.first.u};
			auto it = edges.find(rev);
			check.require(it != edges.end() && it->second == 1, "cell edge is not closed by its reverse");
		}
	}
}

// Stored rings follow the outward normal of the canonical first cell.
void check_outward_normals(Check &check, const periodic_complex &pc,
                           const std::map<int, std::array<double, 3> > &positions) {
	for (const auto &face : pc.faces) {
		std::array<double, 3> normal{0, 0, 0};
		for (size_t t = 0; t < face.corners.size(); t++) {
			const auto wa = pc.corner_world(face.corners[t]);
			const auto wb = pc.corner_world(face.corners[(t + 1) % face.corners.size()]);
			normal[0] += (wa[1] - wb[1]) * (wa[2] + wb[2]);
			normal[1] += (wa[2] - wb[2]) * (wa[0] + wb[0]);
			normal[2] += (wa[0] - wb[0]) * (wa[1] + wb[1]);
		}
		const double nl = norm3(normal);
		check.require(nl > 0, "degenerate face ring");
		for (int c = 0; c < 3; c++) normal[c] /= nl;
		const auto pa = positions.at(face.particle_a);
		const auto pb = positions.at(face.particle_b);
		const std::array<double, 3> image{
			pb[0] + face.image_shift.x * pc.bx + face.image_shift.y * pc.bxy + face.image_shift.z * pc.bxz,
			pb[1] + face.image_shift.y * pc.by + face.image_shift.z * pc.byz,
			pb[2] + face.image_shift.z * pc.bz
		};
		const std::array<double, 3> pv{image[0] - pa[0], image[1] - pa[1], image[2] - pa[2]};
		const double pl = norm3(pv);
		check.require(pl > 0, "zero-length image vector");
		const double dot = (pv[0] * normal[0] + pv[1] * normal[1] + pv[2] * normal[2]) / pl;
		check.require(dot > 1.0 - 1e-9, "stored ring is not ordered by the outward normal");
	}
}

void check_volumes(Check &check, const periodic_complex &pc,
                   const std::map<int, double> &library) {
	double total = 0;
	for (int i = 0; i < static_cast<int>(pc.cells.size()); i++) {
		const double v = pc.cell_volume(i);
		check.require(v > 0, "non-positive complex cell volume");
		auto it = library.find(pc.cells[i].particle);
		check.require(it != library.end(), "library volume missing for cell");
		if (it != library.end())
			check.require(std::abs(v - it->second) < EPS * std::max(1.0, it->second),
				"complex volume disagrees with the library cell");
		total += v;
	}
	check.require(std::abs(total - pc.domain_volume()) < EPS * std::max(1.0, pc.domain_volume()),
		"cell volumes do not tile the lattice determinant");
}

// Shared faces are stored once and obey the canonical identity ordering.
void check_face_identities(Check &check, const periodic_complex &pc) {
	std::set<std::tuple<int, int, int, int, int> > keys;
	for (const auto &f : pc.faces) {
		check.require(f.particle_a <= f.particle_b, "face IDs are not canonicalized");
		if (f.particle_a == f.particle_b) {
			const auto &k = f.image_shift;
			const int lead = k.x != 0 ? k.x : (k.y != 0 ? k.y : k.z);
			check.require(lead > 0, "self face image shift is not canonicalized");
		}
		auto key = std::make_tuple(f.particle_a, f.particle_b,
			f.image_shift.x, f.image_shift.y, f.image_shift.z);
		check.require(keys.insert(key).second, "duplicate face identity");
	}
	for (size_t i = 1; i < pc.faces.size(); i++)
		check.require(std::tie(pc.faces[i - 1].particle_a, pc.faces[i - 1].particle_b,
		                       pc.faces[i - 1].image_shift.x, pc.faces[i - 1].image_shift.y,
		                       pc.faces[i - 1].image_shift.z) <
		              std::tie(pc.faces[i].particle_a, pc.faces[i].particle_b,
		                       pc.faces[i].image_shift.x, pc.faces[i].image_shift.y,
		                       pc.faces[i].image_shift.z),
			"faces are not sorted by canonical identity");
	for (size_t i = 1; i < pc.cells.size(); i++)
		check.require(pc.cells[i - 1].particle < pc.cells[i].particle,
			"cells are not sorted by particle ID");
}

} // namespace

void register_contracts(Registry &registry) {
	// ------------------------------------------------------------------
	// lattice-faces
	// ------------------------------------------------------------------
	registry.add("lattice-faces", "single-cube", [](Check &check) -> int {
		voro::container_periodic con(1, 0, 1, 0, 0, 1, 2, 2, 2, 8);
		con.put(17, .2, .3, .4);
		const periodic_complex pc = build_periodic_complex(con);
		check.require(pc.vertices.size() == 1, "single cube must have one periodic vertex");
		check.require(pc.faces.size() == 3, "single cube must have three shared faces");
		check.require(pc.cells.size() == 1, "single cube must have one cell");
		check.require(pc.cells[0].faces.size() == 6, "single cube cell needs six directed refs");
		std::set<std::tuple<int, int, int> > shifts;
		for (const auto &f : pc.faces) {
			check.require(f.corners.size() == 4, "self faces of a cube are quadrilaterals");
			check.require(f.particle_a == 17 && f.particle_b == 17, "self face keeps the seed ID");
			shifts.insert(std::make_tuple(f.image_shift.x, f.image_shift.y, f.image_shift.z));
			for (const auto &corner : f.corners) check.require(corner.vertex == 0, "one shared vertex");
		}
		check.require(shifts.count(std::make_tuple(1, 0, 0)) == 1, "missing +x self image");
		check.require(shifts.count(std::make_tuple(0, 1, 0)) == 1, "missing +y self image");
		check.require(shifts.count(std::make_tuple(0, 0, 1)) == 1, "missing +z self image");
		int plus = 0, minus = 0;
		for (const auto &r : pc.cells[0].faces) {
			if (r.orientation == 1) {
				plus++;
				check.require(r.lattice_translation == pc_ivec3(0, 0, 0), "positive side is untranslated");
			} else {
				minus++;
				const auto &f = pc.faces[r.face];
				check.require(r.lattice_translation ==
				              pc_ivec3(-f.image_shift.x, -f.image_shift.y, -f.image_shift.z),
					"negative side translates by -image_shift");
			}
		}
		check.require(plus == 3 && minus == 3, "each self face is seen from both sides");
		check.require(std::abs(pc.cell_volume(0) - 1.0) < EPS, "cube cell volume");
		return 0;
	});

	registry.add("lattice-faces", "sheared-asymmetric", [](Check &check) -> int {
		voro::container_periodic con(1.0, 0.3, 0.8, 0.1, 0.2, 0.9, 3, 3, 3, 8);
		con.put(0, 0.1, 0.2, 0.3);
		con.put(1, 0.7, 0.6, 0.2);
		con.put(2, 0.4, 0.75, 0.8);
		con.put(3, 0.85, 0.15, 0.55);
		con.put(4, 0.25, 0.45, 0.05);
		const periodic_complex pc = build_periodic_complex(con);
		check.require(pc.cells.size() == 5, "one complex cell per seed");
		check.require(pc.pairing_tolerance == 1e-9 * 1.0, "pairing tolerance follows the longest vector");
		check.require(pc.minimum_vertex_separation == 1e-6 * 1.0, "minimum separation follows the longest vector");
		check_face_identities(check, pc);
		check_face_opposites(check, pc);
		check_closure(check, pc);
		check_outward_normals(check, pc, particle_positions(con));
		check_volumes(check, pc, library_volumes(con));
		return 0;
	});

	registry.add("lattice-faces", "boundary-seams", [](Check &check) -> int {
		voro::container_periodic con(1, 0, 1, 0, 0, 1, 2, 2, 2, 8);
		con.put(0, 0.02, 0.5, 0.5);
		con.put(1, 0.98, 0.5, 0.5);
		con.put(2, 0.5, 0.02, 0.5);
		con.put(3, 0.5, 0.98, 0.5);
		const periodic_complex pc = build_periodic_complex(con);
		for (const auto &v : pc.vertices)
			for (int c = 0; c < 3; c++)
				check.require(v[c] >= 0.0 && v[c] < 1.0, "vertex representative outside [0,1)");
		check_face_identities(check, pc);
		check_face_opposites(check, pc);
		check_closure(check, pc);
		check_outward_normals(check, pc, particle_positions(con));
		check_volumes(check, pc, library_volumes(con));
		return 0;
	});

	// ------------------------------------------------------------------
	// quotient-topology
	// ------------------------------------------------------------------
	registry.add("quotient-topology", "rings-and-closure", [](Check &check) -> int {
		voro::container_periodic con(1.2, -0.25, 0.9, 0.15, 0.35, 1.1, 4, 4, 4, 8);
		con.put(10, 0.15, 0.7, 0.2);
		con.put(20, 0.9, 0.2, 0.6);
		con.put(30, 0.45, 0.55, 0.9);
		con.put(40, 1.05, 0.8, 0.35);
		con.put(50, 0.3, 0.15, 0.75);
		con.put(60, 0.7, 0.4, 0.1);
		const periodic_complex pc = build_periodic_complex(con);
		check.require(pc.cells.size() == 6, "all seeds produce cells");
		check_face_identities(check, pc);
		check_face_opposites(check, pc);
		check_closure(check, pc);
		check_outward_normals(check, pc, particle_positions(con));
		check_volumes(check, pc, library_volumes(con));
		// Every corner references a valid global vertex.
		for (const auto &f : pc.faces)
			for (const auto &corner : f.corners)
				check.require(corner.vertex >= 0 &&
				              corner.vertex < static_cast<int>(pc.vertices.size()),
					"corner references an invalid vertex index");
		return 0;
	});

	registry.add("quotient-topology", "integer-lattice-translation", [](Check &check) -> int {
		struct seed { int id; double x, y, z; };
		const seed seeds[] = {
			{1, .15, .2, .3}, {2, .7, .65, .25}, {3, .4, .75, .8},
			{4, .85, .15, .6}, {5, .25, .5, .05}
		};
		voro::container_periodic base(1.0, 0.2, 0.7, 0.1, 0.15, 0.9, 3, 3, 3, 8);
		voro::container_periodic shifted(1.0, 0.2, 0.7, 0.1, 0.15, 0.9, 3, 3, 3, 8);
		for (const auto &s : seeds) {
			base.put(s.id, s.x, s.y, s.z);
			// Translate by the lattice vector 2A - 3B + C; the container
			// remaps the points back into the primary domain.
			shifted.put(s.id,
				s.x + 2 * 1.0 - 3 * 0.2 + 1 * 0.1,
				s.y - 3 * 0.7 + 1 * 0.15,
				s.z + 1 * 0.9);
		}
		const periodic_complex a = build_periodic_complex(base);
		const periodic_complex b = build_periodic_complex(shifted);
		check.require(same_complex(a, b), "integer lattice translation changes the quotient topology");
		check_volumes(check, a, library_volumes(base));
		return 0;
	});

	registry.add("quotient-topology", "reorder-and-image-cache", [](Check &check) -> int {
		struct seed { int id; double x, y, z; };
		const seed seeds[] = {
			{7, .1, .2, .3}, {3, .7, .6, .2}, {11, .4, .75, .8},
			{1, .85, .15, .55}, {5, .25, .45, .05}
		};
		voro::container_periodic cold(1.0, 0.3, 0.8, 0.1, 0.2, 0.9, 3, 3, 3, 8);
		voro::container_periodic hot(1.0, 0.3, 0.8, 0.1, 0.2, 0.9, 3, 3, 3, 8);
		for (const auto &s : seeds) cold.put(s.id, s.x, s.y, s.z);
		for (auto it = seeds; it != seeds + 5; ++it) hot.put(it->id, it->x, it->y, it->z);
		// Different insertion order on a third container.
		voro::container_periodic reordered(1.0, 0.3, 0.8, 0.1, 0.2, 0.9, 3, 3, 3, 8);
		for (int i = 4; i >= 0; i--) reordered.put(seeds[i].id, seeds[i].x, seeds[i].y, seeds[i].z);

		const periodic_complex pc_cold = build_periodic_complex(cold);
		// Warm the dynamic image cache, compute one cell explicitly, then
		// rebuild: topology must not depend on cache state.
		hot.create_all_images();
		{
			voro::voronoicell_neighbor probe(hot);
			voro::c_loop_all_periodic loop(hot);
			loop.start();
			check.require(hot.compute_cell(probe, loop), "probe cell computation failed");
		}
		const periodic_complex pc_hot = build_periodic_complex(hot);
		const periodic_complex pc_reordered = build_periodic_complex(reordered);
		const periodic_complex pc_repeat = build_periodic_complex(cold);
		check.require(same_complex(pc_cold, pc_hot), "image cache state changes the complex");
		check.require(same_complex(pc_cold, pc_reordered), "insertion order changes the complex");
		check.require(same_complex(pc_cold, pc_repeat), "repeated build is not deterministic");
		return 0;
	});

	registry.add("quotient-topology", "high-order-vertices", [](Check &check) -> int {
		// FCC lattice: the rhombic-dodecahedron cell has vertices of order
		// higher than three, exercising the non-simplicial path.
		voro::container_periodic con(1, 0, 1, 0, 0, 1, 3, 3, 3, 8);
		con.put(0, 0.0, 0.0, 0.0);
		con.put(1, 0.5, 0.5, 0.0);
		con.put(2, 0.5, 0.0, 0.5);
		con.put(3, 0.0, 0.5, 0.5);
		const periodic_complex pc = build_periodic_complex(con);
		check.require(pc.cells.size() == 4, "FCC seeds produce four cells");
		check_closure(check, pc);
		check_face_opposites(check, pc);
		check_volumes(check, pc, library_volumes(con));
		// No face is assumed to be a triangle.
		for (const auto &f : pc.faces) check.require(f.corners.size() >= 3, "degenerate face");
		return 0;
	});

	// ------------------------------------------------------------------
	// weighted-reuse
	// ------------------------------------------------------------------
	registry.add("weighted-reuse", "radical-unequal-radii", [](Check &check) -> int {
		voro::container_periodic_poly con(1, 0, 1, 0, 0, 1, 3, 3, 3, 8);
		con.put(0, .2, .2, .2, .15);
		con.put(1, .7, .3, .6, .08);
		con.put(2, .5, .8, .3, .12);
		con.put(3, .3, .5, .85, .10);
		con.put(4, .85, .85, .15, .06);
		const periodic_complex pc = build_periodic_complex(con);
		check.require(pc.cells.size() == 5, "weighted cells built");
		check_face_identities(check, pc);
		check_face_opposites(check, pc);
		check_closure(check, pc);
		check_outward_normals(check, pc, particle_positions(con));
		check_volumes(check, pc, library_volumes(con));
		return 0;
	});

	registry.add("weighted-reuse", "equal-radii-matches-mono", [](Check &check) -> int {
		struct seed { int id; double x, y, z; };
		const seed seeds[] = {
			{0, .2, .2, .2}, {1, .7, .3, .6}, {2, .5, .8, .3},
			{3, .3, .5, .85}, {4, .85, .85, .15}
		};
		voro::container_periodic mono(1, 0, 1, 0, 0, 1, 3, 3, 3, 8);
		voro::container_periodic_poly poly(1, 0, 1, 0, 0, 1, 3, 3, 3, 8);
		for (const auto &s : seeds) {
			mono.put(s.id, s.x, s.y, s.z);
			poly.put(s.id, s.x, s.y, s.z, 0.37);
		}
		const periodic_complex a = build_periodic_complex(mono);
		const periodic_complex b = build_periodic_complex(poly);
		check.require(a.vertices.size() == b.vertices.size(),
			"equal radii must reproduce the unweighted vertex count");
		check.require(a.faces.size() == b.faces.size(),
			"equal radii must reproduce the unweighted face count");
		check.require(same_complex(a, b), "equal-radius radical tessellation differs from mono");
		return 0;
	});

	registry.add("weighted-reuse", "weighted-sheared-determinism", [](Check &check) -> int {
		voro::container_periodic_poly a(1.1, 0.25, 0.95, -0.1, 0.2, 1.05, 4, 4, 4, 8);
		a.put(0, .1, .2, .3, .12);
		a.put(1, .8, .4, .7, .07);
		a.put(2, .45, .85, .25, .15);
		a.put(3, .65, .15, .8, .09);
		a.put(4, .25, .55, .6, .11);
		voro::container_periodic_poly b(1.1, 0.25, 0.95, -0.1, 0.2, 1.05, 4, 4, 4, 8);
		b.put(4, .25, .55, .6, .11);
		b.put(3, .65, .15, .8, .09);
		b.put(2, .45, .85, .25, .15);
		b.put(1, .8, .4, .7, .07);
		b.put(0, .1, .2, .3, .12);
		const periodic_complex pca = build_periodic_complex(a);
		const periodic_complex pcb = build_periodic_complex(b);
		const periodic_complex pca2 = build_periodic_complex(a);
		check.require(same_complex(pca, pcb), "weighted topology depends on insertion order");
		check.require(same_complex(pca, pca2), "weighted rebuild is not deterministic");
		check_face_opposites(check, pca);
		check_closure(check, pca);
		check_volumes(check, pca, library_volumes(a));
		return 0;
	});

	registry.add("weighted-reuse", "invalid-input-fails-loudly", [](Check &check) -> int {
		auto expect_throw = [](voro::container_periodic &con) {
			bool threw = false;
			try { build_periodic_complex(con); }
			catch (const periodic_complex_error &) { threw = true; }
			return threw;
		};
		{
			voro::container_periodic con(1, 0, 1, 0, 0, 1, 2, 2, 2, 8);
			check.require(expect_throw(con), "empty container must throw");
		}
		{
			voro::container_periodic con(1, 0, 1, 0, 0, 1, 2, 2, 2, 8);
			con.put(-3, .5, .5, .5);
			check.require(expect_throw(con), "negative particle ID must throw");
		}
		{
			voro::container_periodic con(1, 0, 1, 0, 0, 1, 2, 2, 2, 8);
			con.put(5, .5, .5, .5);
			con.put(5, .2, .2, .2);
			check.require(expect_throw(con), "duplicate particle ID must throw");
		}
		{
			voro::container_periodic_poly con(1, 0, 1, 0, 0, 1, 2, 2, 2, 8);
			con.put(9, .1, .1, .1, .1);
			const periodic_complex pc = build_periodic_complex(con);
			check.require(pc.cells.size() == 1, "valid weighted single cell still builds");
		}
		return 0;
	});
}
