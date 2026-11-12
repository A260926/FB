// Voro++, a 3D cell-based Voronoi library
//
// Author   : Chris H. Rycroft (Harvard University / LBL)
// Email    : chr@alum.mit.edu
// Date     : August 30th 2011

/** \file periodic_complex.cc
 * \brief Function implementations for build_periodic_complex(). */

#include "periodic_complex.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace voro {

namespace {

/** Integer lattice shift with lexicographic ordering, used as a map key. */
struct face_key {
	int a, b, kx, ky, kz;
	bool operator<(const face_key &o) const {
		if (a != o.a) return a < o.a;
		if (b != o.b) return b < o.b;
		if (kx != o.kx) return kx < o.kx;
		if (ky != o.ky) return ky < o.ky;
		return kz < o.kz;
	}
};

/** A face incidence collected while walking one cell. */
struct raw_ref {
	face_key key;
	int orientation;
	pc_ivec3 translation;
};

/** Result of putting a face identity into canonical form. */
struct canon_face {
	face_key key;
	/** True when the observing cell is the canonical second side, so
	 * its local ring has to be reversed and re-lifted. */
	bool reversed;
};

canon_face canonicalize(int i, int j, const pc_ivec3 &k) {
	canon_face out;
	if (i == j) {
		const int lead = k.x != 0 ? k.x : (k.y != 0 ? k.y : k.z);
		if (lead > 0) {
			out.key = {i, j, k.x, k.y, k.z};
			out.reversed = false;
		} else {
			out.key = {i, j, -k.x, -k.y, -k.z};
			out.reversed = true;
		}
	} else if (i < j) {
		out.key = {i, j, k.x, k.y, k.z};
		out.reversed = false;
	} else {
		out.key = {j, i, -k.x, -k.y, -k.z};
		out.reversed = true;
	}
	return out;
}

double vec3_norm(const std::array<double, 3> &v) {
	return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

double vec3_dist(const std::array<double, 3> &a, const std::array<double, 3> &b) {
	double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

int round_to_int(double v) {
	return static_cast<int>(std::llround(v));
}

/** Shared driver for the regular and radical container overloads. */
template <class container_class>
periodic_complex build_complex(container_class &con) {
	if (!(con.bx > 0.0 && con.by > 0.0 && con.bz > 0.0))
		throw periodic_complex_error("build_periodic_complex: degenerate lattice vectors");

	const std::array<double, 3> A = {con.bx, 0.0, 0.0};
	const std::array<double, 3> B = {con.bxy, con.by, 0.0};
	const std::array<double, 3> C = {con.bxz, con.byz, con.bz};
	const double max_len = std::max(std::max(vec3_norm(A), vec3_norm(B)), vec3_norm(C));
	if (!(max_len > 0.0))
		throw periodic_complex_error("build_periodic_complex: zero-length lattice vector");
	const bool weighted = (con.ps == 4);

	periodic_complex out;
	out.bx = con.bx;
	out.bxy = con.bxy;
	out.by = con.by;
	out.bxz = con.bxz;
	out.byz = con.byz;
	out.bz = con.bz;
	out.pairing_tolerance = 1e-9 * max_len;
	out.minimum_vertex_separation = 1e-6 * max_len;
	const double tol = out.pairing_tolerance;
	// Tolerance for the (squared-distance) plane identity test. The true
	// planes hold to machine precision; this bound is deliberately far
	// tighter than any geometric signal but above floating point noise.
	const double plane_tol_sq = 50.0 * tol * max_len;
	// World-space spatial hash used to recognize the same periodic
	// vertex across cells and across the [0,1) seam. Representatives are
	// stored together with their 27 nearest lattice images, so lookups
	// are independent of lattice shear and anisotropy.
	const double hash_cell = std::max(4.0 * tol, 1e-300);
	struct world_key {
		long long x, y, z;
		bool operator==(const world_key &o) const {
			return x == o.x && y == o.y && z == o.z;
		}
	};
	struct world_hash {
		size_t operator()(const world_key &h) const {
			const size_t cx = std::hash<long long>()(h.x);
			const size_t cy = std::hash<long long>()(h.y);
			const size_t cz = std::hash<long long>()(h.z);
			return cx ^ (cy * 1000003ULL) ^ (cz * 1000033ULL);
		}
	};
	auto cell_of = [&](double v) {
		return static_cast<long long>(std::floor(v / hash_cell));
	};
	// Each bin entry is a periodic vertex together with one concrete
	// lattice image of its representative; matching is done against that
	// image's actual world position.
	struct world_entry {
		int vertex;
		std::array<double, 3> image_world;
	};
	std::unordered_map<world_key, std::vector<world_entry>, world_hash> world_bins;

	struct seed_info {
		int id, ijk, q;
		double x, y, z, radius;
		double library_volume;
		std::vector<raw_ref> refs;
	};
	std::vector<seed_info> seeds;

	// Primary-domain particles only. The periodic loop never visits
	// dynamically created image blocks, so original seed data is all that
	// is read, and nothing here mutates the container's particle arrays.
	c_loop_all_periodic loop(con);
	if (!loop.start())
		throw periodic_complex_error("build_periodic_complex: container has no particles");
	do {
		seed_info s;
		s.id = loop.pid();
		if (s.id < 0) {
			std::ostringstream msg;
			msg << "build_periodic_complex: particle IDs must be non-negative (got " << s.id << ")";
			throw periodic_complex_error(msg.str());
		}
		loop.pos(s.x, s.y, s.z);
		s.ijk = loop.ijk;
		s.q = loop.q;
		s.radius = weighted ? con.p[s.ijk][4 * s.q + 3] : 0.0;
		s.library_volume = 0.0;
		seeds.push_back(s);
	} while (loop.inc());

	{
		std::set<int> seen;
		for (const seed_info &s : seeds) {
			if (!seen.insert(s.id).second) {
				std::ostringstream msg;
				msg << "build_periodic_complex: duplicate particle ID " << s.id;
				throw periodic_complex_error(msg.str());
			}
		}
	}

	std::map<int, size_t> seed_by_id;
	for (size_t t = 0; t < seeds.size(); t++) seed_by_id[seeds[t].id] = t;

	// Global periodic-vertex table is the world-space spatial hash
	// (world_bins) built above; out.vertices holds the [0,1)
	// representatives. Key collisions are always verified geometrically so
	// an ambiguous pairing fails loudly instead of welding two distinct
	// vertices together.

	struct face_record {
		face_key key;
		pc_face face;
	};
	std::map<face_key, int> face_slot;
	std::vector<face_record> face_records;

	for (seed_info &s : seeds) {
		voronoicell_neighbor cell(con);
		if (!con.compute_cell(cell, s.ijk, s.q)) {
			std::ostringstream msg;
			msg << "build_periodic_complex: empty Voronoi cell for particle " << s.id;
			throw periodic_complex_error(msg.str());
		}
		s.library_volume = cell.volume();
		if (!(s.library_volume > 0.0)) {
			std::ostringstream msg;
			msg << "build_periodic_complex: non-positive cell volume for particle " << s.id;
			throw periodic_complex_error(msg.str());
		}

		// Voro++ stores vertex coordinates at double precision in pts;
		// every public accessor multiplies them by 0.5.
		auto local_world = [&](int t) {
			return std::array<double, 3>{
				s.x + 0.5 * cell.pts[4 * t],
				s.y + 0.5 * cell.pts[4 * t + 1],
				s.z + 0.5 * cell.pts[4 * t + 2]
			};
		};

		std::vector<int> face_verts, neighbor_ids;
		cell.face_vertices(face_verts);
		cell.neighbors(neighbor_ids);

		// Both accessors enumerate faces in the same edge-traversal
		// order; require them to agree.
		std::vector<std::vector<int> > rings;
		rings.reserve(neighbor_ids.size());
		for (size_t pos = 0; pos < face_verts.size();) {
			int count = face_verts[pos++];
			if (count < 3 || pos + static_cast<size_t>(count) > face_verts.size())
				throw periodic_complex_error("build_periodic_complex: malformed face vertex table");
			std::vector<int> ring(count);
			for (int t = 0; t < count; t++) ring[t] = face_verts[pos++];
			rings.push_back(ring);
		}
		if (rings.size() != neighbor_ids.size())
			throw periodic_complex_error("build_periodic_complex: face/neighbor enumeration mismatch");

		// Resolve every local vertex to a shared periodic vertex,
		// recording its lift relative to the stored [0,1)
		// representative. The lift is always computed against the
		// representative actually matched, so a point evaluated just
		// below 1 (representative stored just above 0) keeps the
		// correct lift instead of borrowing the local wrap.
		std::vector<int> local_to_global(cell.p, -1);
		std::vector<pc_ivec3> local_lift(cell.p);
		for (int t = 0; t < cell.p; t++) {
			const std::array<double, 3> world = local_world(t);
			const std::array<double, 3> frac = out.world_to_fractional(world[0], world[1], world[2]);
			std::array<double, 3> q;
			for (int c = 0; c < 3; c++) {
				const double m = std::floor(frac[c]);
				q[c] = frac[c] - m;
				if (q[c] < 0.0) q[c] = 0.0;
				if (q[c] >= 1.0) q[c] = 0.0;
			}

			// World-space lookup. Every stored representative is
			// indexed together with its 27 nearest lattice images, so
			// a seam point finds the same periodic vertex through the
			// image nearest to it. Candidates within one bin of the
			// point are checked directly against the pairing
			// tolerance; two distinct vertices inside that radius is a
			// hard pairing failure.
			const world_key center{cell_of(world[0]), cell_of(world[1]), cell_of(world[2])};
			int match = -1;
			for (long long dx = -1; dx <= 1; dx++)
				for (long long dy = -1; dy <= 1; dy++)
					for (long long dz = -1; dz <= 1; dz++) {
						auto bit = world_bins.find({center.x + dx, center.y + dy, center.z + dz});
						if (bit == world_bins.end()) continue;
						for (const world_entry &entry : bit->second) {
							if (vec3_dist(world, entry.image_world) <= tol && match != entry.vertex) {
								if (match != -1) {
									std::ostringstream msg;
									msg << "build_periodic_complex: periodic vertex pairing is ambiguous at "
									    << "tolerance " << tol << " (particle " << s.id << ")";
									throw periodic_complex_error(msg.str());
								}
								match = entry.vertex;
							}
						}
					}
			if (match == -1) {
				match = static_cast<int>(out.vertices.size());
				out.vertices.push_back(q);
				const std::array<double, 3> wrep = out.vertex_world(match);
				for (int da = -1; da <= 1; da++)
					for (int db = -1; db <= 1; db++)
						for (int dc = -1; dc <= 1; dc++) {
							const std::array<double, 3> wp = {
								wrep[0] + da * A[0] + db * B[0] + dc * C[0],
								wrep[1] + da * A[1] + db * B[1] + dc * C[1],
								wrep[2] + da * A[2] + db * B[2] + dc * C[2]
							};
							world_bins[{cell_of(wp[0]), cell_of(wp[1]), cell_of(wp[2])}]
								.push_back({match, wp});
						}
			}
			local_to_global[t] = match;
			// Lift relative to the representative that was actually
			// stored, so seam points keep the correct integer lift.
			for (int c = 0; c < 3; c++) {
				const double delta = frac[c] - out.vertices[match][c];
				const int lift_c = round_to_int(delta);
				if (c == 0) local_lift[t].x = lift_c;
				if (c == 1) local_lift[t].y = lift_c;
				if (c == 2) local_lift[t].z = lift_c;
			}
			// Reconstructed position must reproduce the input point.
			if (vec3_dist(world, out.corner_world(pc_corner{local_to_global[t], local_lift[t]})) > tol)
				throw periodic_complex_error(
					"build_periodic_complex: vertex lift does not reconstruct the cell corner");
		}

		for (size_t f = 0; f < rings.size(); f++) {
			const int j = neighbor_ids[f];
			if (j < 0 || seed_by_id.find(j) == seed_by_id.end()) {
				std::ostringstream msg;
				msg << "build_periodic_complex: face of particle " << s.id
				    << " refers to unknown neighbor ID " << j;
				throw periodic_complex_error(msg.str());
			}
			const seed_info &other = seeds[seed_by_id[j]];

			// Voro++'s edge walk yields an inward-pointing ring; the
			// complex stores rings ordered by the outward normal, so
			// work with the reversed traversal.
			std::vector<int> outward = rings[f];
			std::reverse(outward.begin(), outward.end());

			// Recover the lattice image k of neighbor j whose cutting
			// plane made this face. Two independent conditions pin k
			// down:
			//
			// 1. Plane identity at every corner x:
			//    unweighted: |x-p_i|^2 = |x-(p_j+kL)|^2
			//    radical:    |x-p_i|^2-r_i^2 = |x-(p_j+kL)|^2-r_j^2
			// 2. The cutting-plane normal is the center-to-image
			//    vector (p_j+kL)-p_i, which has to agree with the
			//    face's outward normal. This disambiguates high-order
			//    vertices where one corner alone lies on several
			//    planes (e.g. a cube corner is on three bisectors).
			//
			// The identity is evaluated at the actual face corners,
			// not by guessing the nearest particle at a face center.
			std::array<double, 3> face_normal = {0.0, 0.0, 0.0};
			for (size_t t = 0; t < outward.size(); t++) {
				const std::array<double, 3> wa =
					out.corner_world(pc_corner{local_to_global[outward[t]], local_lift[outward[t]]});
				const std::array<double, 3> wb = out.corner_world(
					pc_corner{local_to_global[outward[(t + 1) % outward.size()]],
					          local_lift[outward[(t + 1) % outward.size()]]});
				face_normal[0] += (wa[1] - wb[1]) * (wa[2] + wb[2]);
				face_normal[1] += (wa[2] - wb[2]) * (wa[0] + wb[0]);
				face_normal[2] += (wa[0] - wb[0]) * (wa[1] + wb[1]);
			}
			const double fnorm = vec3_norm(face_normal);
			if (!(fnorm > 0.0)) {
				std::ostringstream msg;
				msg << "build_periodic_complex: degenerate face on particle " << s.id;
				throw periodic_complex_error(msg.str());
			}
			face_normal[0] /= fnorm;
			face_normal[1] /= fnorm;
			face_normal[2] /= fnorm;
			// Distinct cutting planes meeting at a vertex have
			// separated normals; a match this close has to be the same
			// plane or the geometry is too tight to pair reliably.
			const double normal_eps = 1e-9;

			bool have_shift = false;
			pc_ivec3 kshift;
			for (int local : outward) {
				const int g = local_to_global[local];
				const std::array<double, 3> cw = out.corner_world(pc_corner{g, local_lift[local]});
				const std::array<double, 3> df =
					out.world_to_fractional(cw[0] - other.x, cw[1] - other.y, cw[2] - other.z);

				std::vector<pc_ivec3> valid;
				// The rounded candidate first, then a bounded scan to
				// stay correct even if a corner's fractional offset from
				// the neighbor image leaves the (-.5,.5) cell.
				const pc_ivec3 guess(round_to_int(df[0]), round_to_int(df[1]), round_to_int(df[2]));
				for (int ak = guess.x - 3; ak <= guess.x + 3; ak++)
					for (int bk = guess.y - 3; bk <= guess.y + 3; bk++)
						for (int ck = guess.z - 3; ck <= guess.z + 3; ck++) {
							const pc_ivec3 cand(ak, bk, ck);
							const std::array<double, 3> image_pos = {
								other.x + cand.x * A[0] + cand.y * B[0] + cand.z * C[0],
								other.y + cand.y * B[1] + cand.z * C[1],
								other.z + cand.z * C[2]
							};
							// Condition 1: bisector identity at this corner.
							const double di2 = vec3_dist(cw, {s.x, s.y, s.z});
							const double dj2 = vec3_dist(cw, image_pos);
							const double lhs = di2 * di2 - (weighted ? s.radius * s.radius : 0.0);
							const double rhs = dj2 * dj2 - (weighted ? other.radius * other.radius : 0.0);
							if (std::abs(lhs - rhs) > plane_tol_sq) continue;
							// Condition 2: cutting-plane normal alignment.
							const std::array<double, 3> pv = {
								image_pos[0] - s.x, image_pos[1] - s.y, image_pos[2] - s.z
							};
							const double pl = vec3_norm(pv);
							if (!(pl > 0.0)) continue;
							const double dot = (pv[0] * face_normal[0] + pv[1] * face_normal[1]
							                    + pv[2] * face_normal[2]) / pl;
							if (dot > 1.0 - normal_eps) valid.push_back(cand);
						}
				if (valid.size() != 1) {
					std::ostringstream msg;
					msg << "build_periodic_complex: " << (valid.empty() ? "cannot identify" : "ambiguous")
					    << " lattice image of neighbor " << j << " for particle " << s.id
					    << " at tolerance " << tol;
					throw periodic_complex_error(msg.str());
				}
				if (!have_shift) {
					kshift = valid[0];
					have_shift = true;
				} else if (kshift != valid[0]) {
					std::ostringstream msg;
					msg << "build_periodic_complex: inconsistent lattice image across face "
					    << "between particles " << s.id << " and " << j;
					throw periodic_complex_error(msg.str());
				}
			}
			if (!have_shift)
				throw periodic_complex_error("build_periodic_complex: face without corners");
			if (s.id == j && kshift == pc_ivec3(0, 0, 0))
				throw periodic_complex_error(
					"build_periodic_complex: degenerate self face with a zero image shift");

			const canon_face cf = canonicalize(s.id, j, kshift);

			// Build the ring in the canonical frame.
			std::vector<pc_corner> canonical_ring;
			canonical_ring.reserve(outward.size());
			for (int local : outward) {
				canonical_ring.push_back(
					pc_corner{local_to_global[local], local_lift[local]});
			}
			if (cf.reversed) {
				std::reverse(canonical_ring.begin(), canonical_ring.end());
				const pc_ivec3 mk(-kshift.x, -kshift.y, -kshift.z);
				for (pc_corner &corner : canonical_ring) corner.lift = corner.lift + mk;
			}

			auto slot_it = face_slot.find(cf.key);
			if (slot_it == face_slot.end()) {
				face_record rec;
				rec.key = cf.key;
				rec.face.particle_a = cf.key.a;
				rec.face.particle_b = cf.key.b;
				rec.face.image_shift = pc_ivec3(cf.key.kx, cf.key.ky, cf.key.kz);
				rec.face.corners = canonical_ring;
				face_slot[cf.key] = static_cast<int>(face_records.size());
				face_records.push_back(std::move(rec));
			} else {
				// The same geometric face seen from the other side.
				// The two edge walks start at different corners, so the
				// rings must agree up to a cyclic rotation; every corner
				// must also reproduce the stored world position.
				const pc_face &stored = face_records[slot_it->second].face;
				const size_t n = stored.corners.size();
				if (n != canonical_ring.size()) {
					std::ostringstream msg;
					msg << "build_periodic_complex: face between particles " << cf.key.a
					    << " and " << cf.key.b << " has mismatched ring sizes";
					throw periodic_complex_error(msg.str());
				}
				size_t rotation = 0;
				bool found = false;
				for (; rotation < n; rotation++) {
					bool ok = true;
					for (size_t t = 0; t < n; t++) {
						const pc_corner &a = stored.corners[t];
						const pc_corner &b = canonical_ring[(t + rotation) % n];
						if (a.vertex != b.vertex || a.lift != b.lift) { ok = false; break; }
					}
					if (ok) { found = true; break; }
				}
				if (!found) {
					std::ostringstream msg;
					msg << "build_periodic_complex: shared face between particles "
					    << cf.key.a << " and " << cf.key.b
					    << " is not geometrically identical on both sides";
					throw periodic_complex_error(msg.str());
				}
				for (size_t t = 0; t < n; t++) {
					const pc_corner &a = stored.corners[t];
					const pc_corner &b = canonical_ring[(t + rotation) % n];
					if (vec3_dist(out.corner_world(a), out.corner_world(b)) > tol)
						throw periodic_complex_error(
							"build_periodic_complex: shared face corner positions disagree beyond tolerance");
				}
			}

			raw_ref ref;
			ref.key = cf.key;
			ref.orientation = cf.reversed ? -1 : +1;
			ref.translation = cf.reversed ? pc_ivec3(-cf.key.kx, -cf.key.ky, -cf.key.kz)
			                              : pc_ivec3(0, 0, 0);
			s.refs.push_back(ref);
		}
	}

	// Distinct periodic vertices must respect the declared minimum
	// separation; otherwise the material is too tight for reliable pairing
	// and the build fails rather than risk a wrong weld. The check works
	// in world space and inserts the 27 nearest lattice images of every
	// representative so that vertices split across the [0,1) seam are
	// still measured at their true periodic distance.
	{
		const double min_sep = out.minimum_vertex_separation;
		struct wkey {
			long long x, y, z;
			bool operator==(const wkey &o) const {
				return x == o.x && y == o.y && z == o.z;
			}
		};
		struct whash {
			size_t operator()(const wkey &h) const {
				const size_t cx = std::hash<long long>()(h.x);
				const size_t cy = std::hash<long long>()(h.y);
				const size_t cz = std::hash<long long>()(h.z);
				return cx ^ (cy * 1000003ULL) ^ (cz * 1000033ULL);
			}
		};
		struct sample {
			int vertex;
			std::array<double, 3> world;
		};
		std::unordered_map<wkey, std::vector<sample>, whash> bins;
		for (int vi = 0; vi < static_cast<int>(out.vertices.size()); vi++) {
			const std::array<double, 3> base = out.vertex_world(vi);
			for (int da = -1; da <= 1; da++)
				for (int db = -1; db <= 1; db++)
					for (int dc = -1; dc <= 1; dc++) {
						const std::array<double, 3> wp = {
							base[0] + da * A[0] + db * B[0] + dc * C[0],
							base[1] + da * A[1] + db * B[1] + dc * C[1],
							base[2] + da * A[2] + db * B[2] + dc * C[2]
						};
						const wkey h{
							static_cast<long long>(std::floor(wp[0] / min_sep)),
							static_cast<long long>(std::floor(wp[1] / min_sep)),
							static_cast<long long>(std::floor(wp[2] / min_sep))
						};
						bins[h].push_back({vi, wp});
					}
		}
		bool too_close = false;
		int bad_a = -1, bad_b = -1;
		double bad_dist = 0.0;
		for (const auto &entry : bins) {
			const wkey &h = entry.first;
			for (int dx = -1; dx <= 1 && !too_close; dx++)
				for (int dy = -1; dy <= 1 && !too_close; dy++)
					for (int dz = -1; dz <= 1 && !too_close; dz++) {
						auto nit = bins.find({h.x + dx, h.y + dy, h.z + dz});
						if (nit == bins.end()) continue;
						for (const sample &sa : entry.second)
							for (const sample &sb : nit->second) {
								if (sa.vertex == sb.vertex) continue;
								const double d = vec3_dist(sa.world, sb.world);
								if (d < min_sep) {
									too_close = true;
									bad_a = sa.vertex;
									bad_b = sb.vertex;
									bad_dist = d;
									break;
								}
							}
					}
		}
		if (too_close) {
			std::ostringstream msg;
			msg << "build_periodic_complex: distinct periodic vertices " << bad_a
			    << " and " << bad_b << " are separated by " << bad_dist
			    << " < minimum separation " << min_sep;
			throw periodic_complex_error(msg.str());
		}
	}

	// Deterministic face ordering by canonical identity.
	std::sort(face_records.begin(), face_records.end(),
	          [](const face_record &a, const face_record &b) { return a.key < b.key; });
	out.faces.reserve(face_records.size());
	std::map<face_key, int> final_face_index;
	for (size_t t = 0; t < face_records.size(); t++) {
		final_face_index[face_records[t].key] = static_cast<int>(t);
		out.faces.push_back(face_records[t].face);
	}

	// Cells sorted by original particle ID, with directed face references.
	std::sort(seeds.begin(), seeds.end(),
	          [](const seed_info &a, const seed_info &b) { return a.id < b.id; });
	out.cells.reserve(seeds.size());
	double total_volume = 0.0;
	for (const seed_info &s : seeds) {
		pc_cell cell;
		cell.particle = s.id;
		cell.faces.reserve(s.refs.size());
		for (const raw_ref &r : s.refs) {
			auto it = final_face_index.find(r.key);
			if (it == final_face_index.end())
				throw periodic_complex_error("build_periodic_complex: internal face index error");
			pc_face_ref ref;
			ref.face = it->second;
			ref.orientation = r.orientation;
			ref.lattice_translation = r.translation;
			cell.faces.push_back(ref);
		}
		const int cell_index = static_cast<int>(out.cells.size());
		out.cells.push_back(cell);

		// Manifold closure: in the cell's local frame every directed
		// edge of every oriented ring must occur exactly once and its
		// reverse must occur exactly once.
		struct corner_id {
			int vertex;
			pc_ivec3 lift;
			bool operator<(const corner_id &o) const {
				if (vertex != o.vertex) return vertex < o.vertex;
				if (lift.x != o.lift.x) return lift.x < o.lift.x;
				if (lift.y != o.lift.y) return lift.y < o.lift.y;
				return lift.z < o.lift.z;
			}
			bool operator==(const corner_id &o) const {
				return vertex == o.vertex && lift == o.lift;
			}
		};
		struct edge_id {
			corner_id u, v;
			bool operator<(const edge_id &o) const {
				if (u < o.u) return true;
				if (o.u < u) return false;
				return v < o.v;
			}
		};
		std::map<edge_id, int> directed_edges;
		for (const pc_face_ref &ref : out.cells.back().faces) {
			const std::vector<pc_corner> ring = out.oriented_ring(ref);
			if (ring.size() < 3)
				throw periodic_complex_error("build_periodic_complex: face with fewer than three corners");
			for (size_t t = 0; t < ring.size(); t++) {
				const pc_corner &a = ring[t];
				const pc_corner &b = ring[(t + 1) % ring.size()];
				edge_id e{{a.vertex, a.lift}, {b.vertex, b.lift}};
				if (++directed_edges[e] != 1)
					throw periodic_complex_error(
						"build_periodic_complex: cell is not a closed orientable manifold (duplicate edge)");
			}
		}
		for (const auto &entry : directed_edges) {
			edge_id rev{entry.first.v, entry.first.u};
			auto rit = directed_edges.find(rev);
			if (rit == directed_edges.end() || rit->second != 1) {
				std::ostringstream msg;
				msg << "build_periodic_complex: cell of particle " << s.id << " is not closed";
				throw periodic_complex_error(msg.str());
			}
		}

		// The reconstructed cell must reproduce the library volume.
		const double rebuilt = out.cell_volume(cell_index);
		const double scale = std::max(1.0, max_len * max_len * max_len);
		if (std::abs(rebuilt - s.library_volume) > 1e-8 * scale) {
			std::ostringstream msg;
			msg << "build_periodic_complex: volume mismatch for particle " << s.id
			    << " (library " << s.library_volume << ", complex " << rebuilt << ")";
			throw periodic_complex_error(msg.str());
		}
		total_volume += rebuilt;
	}

	if (out.cells.size() != seeds.size())
		throw periodic_complex_error("build_periodic_complex: cell count mismatch");
	const double domain = out.domain_volume();
	if (std::abs(total_volume - domain) > 1e-8 * std::max(1.0, domain)) {
		std::ostringstream msg;
		msg << "build_periodic_complex: total volume " << total_volume
		    << " does not tile the lattice volume " << domain;
		throw periodic_complex_error(msg.str());
	}

	return out;
}

} // anonymous namespace

periodic_complex build_periodic_complex(container_periodic &con) {
	return build_complex(con);
}

periodic_complex build_periodic_complex(container_periodic_poly &con) {
	return build_complex(con);
}

std::array<double, 3> periodic_complex::fractional_to_world(double fx, double fy, double fz) const {
	return {
		fx * bx + fy * bxy + fz * bxz,
		fy * by + fz * byz,
		fz * bz
	};
}

std::array<double, 3> periodic_complex::world_to_fractional(double wx, double wy, double wz) const {
	const double fz = wz / bz;
	const double fy = (wy - byz * fz) / by;
	const double fx = (wx - bxy * fy - bxz * fz) / bx;
	return {fx, fy, fz};
}

std::array<double, 3> periodic_complex::vertex_world(int vertex) const {
	const std::array<double, 3> &f = vertices[vertex];
	return fractional_to_world(f[0], f[1], f[2]);
}

std::array<double, 3> periodic_complex::corner_world(const pc_corner &corner) const {
	const std::array<double, 3> base = vertex_world(corner.vertex);
	const pc_ivec3 &m = corner.lift;
	return {
		base[0] + m.x * bx + m.y * bxy + m.z * bxz,
		base[1] + m.y * by + m.z * byz,
		base[2] + m.z * bz
	};
}

std::array<double, 3> periodic_complex::corner_world(const pc_corner &corner, const pc_face_ref &ref) const {
	pc_corner shifted = corner;
	shifted.lift = shifted.lift + ref.lattice_translation;
	return corner_world(shifted);
}

std::vector<pc_corner> periodic_complex::oriented_ring(const pc_face_ref &ref) const {
	std::vector<pc_corner> ring = faces[ref.face].corners;
	if (ref.orientation < 0)
		std::reverse(ring.begin(), ring.end());
	for (pc_corner &corner : ring)
		corner.lift = corner.lift + ref.lattice_translation;
	return ring;
}

double periodic_complex::cell_volume(int cell_index) const {
	const pc_cell &c = cells[cell_index];
	double volume = 0.0;
	for (const pc_face_ref &ref : c.faces) {
		const std::vector<pc_corner> ring = oriented_ring(ref);
		if (ring.size() < 3) continue;
		const std::array<double, 3> a = corner_world(ring[0]);
		for (size_t t = 1; t + 1 < ring.size(); t++) {
			const std::array<double, 3> b = corner_world(ring[t]);
			const std::array<double, 3> d = corner_world(ring[t + 1]);
			// Signed tetrahedron contribution det(a,b,d)/6 for the
			// outward-oriented fan.
			const double cx = b[1] * d[2] - b[2] * d[1];
			const double cy = b[2] * d[0] - b[0] * d[2];
			const double cz = b[0] * d[1] - b[1] * d[0];
			volume += (a[0] * cx + a[1] * cy + a[2] * cz) / 6.0;
		}
	}
	return volume;
}

}
