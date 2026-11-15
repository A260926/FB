/*
 * skin_shadow.cpp - implementation of the occluder/point-light shadow
 * volume interface declared in skin_shadow.h. C++17 standard library only.
 */
#include "skin_shadow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skin_shadow {

namespace {

constexpr float kAreaEps = 1e-18f;

struct Vec3 {
	float x = 0.f, y = 0.f, z = 0.f;
};

struct Influ {
	int32_t joint = -1;
	float weight = 0.f;
};

struct SrcFace {
	FaceSource src;
	cgltf_size a = 0, b = 0, c = 0; /* welded vertex indices */
};

struct EdgeRec {
	cgltf_size face = (cgltf_size)-1;
	int corner = -1;
};

inline bool finite3(const float* v) {
	return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

inline bool finite16(const float* m) {
	for (int i = 0; i < 16; ++i) {
		if (!std::isfinite(m[i])) return false;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Accessor helpers (hand-written so sparse positions work everywhere) */
/* ------------------------------------------------------------------ */

const uint8_t* view_data(const cgltf_buffer_view* view) {
	if (view->data) return static_cast<const uint8_t*>(view->data);
	if (!view->buffer || !view->buffer->data) return nullptr;
	return static_cast<const uint8_t*>(view->buffer->data) + view->offset;
}

uint32_t read_index_raw(const uint8_t* p, cgltf_component_type type) {
	switch (type) {
		case cgltf_component_type_r_8u: return *p;
		case cgltf_component_type_r_16u: {
			uint16_t v;
			std::memcpy(&v, p, sizeof(v));
			return v;
		}
		case cgltf_component_type_r_32u: {
			uint32_t v;
			std::memcpy(&v, p, sizeof(v));
			return v;
		}
		default: return 0;
	}
}

float read_comp_float(const uint8_t* p, cgltf_component_type type, bool normalized) {
	if (type == cgltf_component_type_r_32f) {
		float v;
		std::memcpy(&v, p, sizeof(v));
		return v;
	}
	if (normalized) {
		switch (type) {
			case cgltf_component_type_r_8u: return *p / 255.f;
			case cgltf_component_type_r_16u: {
				uint16_t u;
				std::memcpy(&u, p, sizeof(u));
				return u / 65535.f;
			}
			case cgltf_component_type_r_8: return *reinterpret_cast<const int8_t*>(p) / 127.f;
			case cgltf_component_type_r_16: {
				int16_t s;
				std::memcpy(&s, p, sizeof(s));
				return s / 32767.f;
			}
			default: return 0.f;
		}
	}
	switch (type) {
		case cgltf_component_type_r_8u: return float(*p);
		case cgltf_component_type_r_16u: {
			uint16_t u;
			std::memcpy(&u, p, sizeof(u));
			return float(u);
		}
		default: return 0.f;
	}
}

/* Read a float VEC3 accessor element, honoring offset/stride and sparse. */
bool read_vec3(const cgltf_accessor* acc, cgltf_size index, float out[3]) {
	out[0] = out[1] = out[2] = 0.f;
	bool wrote = false;
	if (acc->is_sparse) {
		const cgltf_accessor_sparse* sp = &acc->sparse;
		const uint8_t* idx_base = view_data(sp->indices_buffer_view);
		const uint8_t* val_base = view_data(sp->values_buffer_view);
		if (!idx_base || !val_base) return false;
		idx_base += sp->indices_byte_offset;
		val_base += sp->values_byte_offset;
		cgltf_size istride = cgltf_component_size(sp->indices_component_type);
		for (cgltf_size k = 0; k < sp->count; ++k) {
			if (read_index_raw(idx_base + k * istride, sp->indices_component_type) == index) {
				const uint8_t* e = val_base + k * acc->stride;
				for (int c = 0; c < 3; ++c) {
					out[c] = read_comp_float(e + c * cgltf_component_size(acc->component_type),
							acc->component_type, acc->normalized != 0);
				}
				wrote = true;
				break;
			}
		}
	}
	if (!wrote) {
		if (!acc->buffer_view) { /* zero-filled accessor */
			return true;
		}
		const uint8_t* base = view_data(acc->buffer_view);
		if (!base) return false;
		const uint8_t* e = base + acc->offset + acc->stride * index;
		if (acc->component_type != cgltf_component_type_r_32f || acc->type != cgltf_type_vec3) {
			return false;
		}
		std::memcpy(out, e, 3 * sizeof(float));
	}
	/* Canonicalize signed zero so equal points share an exact key. */
	for (int c = 0; c < 3; ++c) {
		if (out[c] == 0.f) out[c] = 0.f;
	}
	return true;
}

/* Non-sparse scalar/vector uint read for JOINTS. */
bool read_uint4(const cgltf_accessor* acc, cgltf_size index, uint32_t out[4]) {
	out[0] = out[1] = out[2] = out[3] = 0;
	if (acc->is_sparse || !acc->buffer_view) return false;
	const uint8_t* base = view_data(acc->buffer_view);
	if (!base) return false;
	const uint8_t* e = base + acc->offset + acc->stride * index;
	cgltf_size n = cgltf_num_components(acc->type);
	if (n > 4) return false;
	for (cgltf_size c = 0; c < n; ++c) {
		out[c] = read_index_raw(e + c * cgltf_component_size(acc->component_type),
				acc->component_type);
	}
	return true;
}

bool read_float4(const cgltf_accessor* acc, cgltf_size index, float out[4], bool normalized) {
	out[0] = out[1] = out[2] = out[3] = 0.f;
	if (acc->is_sparse || !acc->buffer_view) return false;
	const uint8_t* base = view_data(acc->buffer_view);
	if (!base) return false;
	const uint8_t* e = base + acc->offset + acc->stride * index;
	cgltf_size n = cgltf_num_components(acc->type);
	if (n > 4) return false;
	cgltf_size cs = cgltf_component_size(acc->component_type);
	for (cgltf_size c = 0; c < n; ++c) {
		out[c] = read_comp_float(e + c * cs, acc->component_type, normalized);
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Welding key                                                         */
/* ------------------------------------------------------------------ */

struct WKey {
	Vec3 pos;
	std::vector<Vec3> morph;   /* target_count entries */
	std::vector<Influ> skin;   /* sorted, normalized    */

	bool operator==(const WKey& o) const {
		if (std::memcmp(&pos, &o.pos, sizeof(Vec3)) != 0) return false;
		if (morph.size() != o.morph.size()) return false;
		for (size_t i = 0; i < morph.size(); ++i) {
			if (std::memcmp(&morph[i], &o.morph[i], sizeof(Vec3)) != 0) return false;
		}
		if (skin.size() != o.skin.size()) return false;
		for (size_t i = 0; i < skin.size(); ++i) {
			if (skin[i].joint != o.skin[i].joint) return false;
			if (skin[i].weight != o.skin[i].weight) return false;
		}
		return true;
	}
};

struct WKeyHash {
	static void combine(size_t& h, const void* p, size_t n) {
		const uint8_t* b = static_cast<const uint8_t*>(p);
		for (size_t i = 0; i < n; ++i) {
			h ^= size_t(b[i]) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
		}
	}
	size_t operator()(const WKey& k) const {
		size_t h = 0xcbf29ce484222325ULL;
		combine(h, &k.pos, sizeof(Vec3));
		for (const Vec3& m : k.morph) combine(h, &m, sizeof(Vec3));
		for (const Influ& s : k.skin) combine(h, &s, sizeof(Influ));
		return h;
	}
};

/* ------------------------------------------------------------------ */
/* Resource implementation                                            */
/* ------------------------------------------------------------------ */

struct BuildVertex {
	Vec3 pos;
	std::vector<Vec3> morph;
	std::vector<Influ> skin;
};

} /* anonymous namespace */

/* Nested implementation types are defined at the enclosing namespace. */
struct Occluder::Impl {
	const cgltf_mesh* mesh = nullptr;
	std::vector<BuildVertex> verts;
	std::vector<SrcFace> faces;
	std::vector<FaceSource> face_sources;
	cgltf_size target_count = 0;
	std::vector<cgltf_size> face_component; /* per face */
	cgltf_size component_count = 0;
};

struct ShadowVolume::Impl {
	std::vector<float> vertices;       /* 4 floats each, finite then far */
	std::vector<cgltf_uint> indices;
	std::vector<FaceSource> tri_sources;
	std::vector<EdgeSource> silhouette;
	cgltf_size n = 0;
};

namespace {

const cgltf_accessor* find_attr(const cgltf_primitive& prim,
		cgltf_attribute_type type, cgltf_int index) {
	for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
		if (prim.attributes[i].type == type && prim.attributes[i].index == index) {
			return prim.attributes[i].data;
		}
	}
	return nullptr;
}

const cgltf_accessor* target_position(const cgltf_primitive& prim, cgltf_size target) {
	if (target >= prim.targets_count) return nullptr;
	for (cgltf_size i = 0; i < prim.targets[target].attributes_count; ++i) {
		if (prim.targets[target].attributes[i].type == cgltf_attribute_type_position) {
			return prim.targets[target].attributes[i].data;
		}
	}
	return nullptr;
}

/* Merge JOINTS/WEIGHTS sets 0 and 1, drop zero weights, sort, normalize. */
bool build_influence(const cgltf_primitive& prim, cgltf_size v,
		std::vector<Influ>& out, Error& error) {
	out.clear();
	struct Pair {
		uint32_t joint;
		float weight;
	};
	Pair pairs[8];
	cgltf_size count = 0;

	for (int set = 0; set < 2; ++set) {
		const cgltf_accessor* ja = find_attr(prim, cgltf_attribute_type_joints, set);
		const cgltf_accessor* wa = find_attr(prim, cgltf_attribute_type_weights, set);
		if (!ja && !wa) continue;
		if (!ja || !wa) {
			error.status = Status::bad_accessor;
			error.message = "JOINTS and WEIGHTS must appear as a pair";
			return false;
		}
		if (ja->is_sparse ||
				(ja->component_type != cgltf_component_type_r_8u &&
				 ja->component_type != cgltf_component_type_r_16u)) {
			error.status = Status::bad_accessor;
			error.message = "joints must be non-sparse UNSIGNED_BYTE or UNSIGNED_SHORT";
			return false;
		}
		const bool float_weights = wa->component_type == cgltf_component_type_r_32f;
		const bool norm_bytes =
				(wa->component_type == cgltf_component_type_r_8u ||
				 wa->component_type == cgltf_component_type_r_16u) &&
				wa->normalized != 0;
		if (wa->is_sparse || (!float_weights && !norm_bytes)) {
			error.status = Status::bad_accessor;
			error.message = "weights must be float or normalized UNSIGNED_BYTE/SHORT";
			return false;
		}
		uint32_t joints[4];
		float weights[4];
		if (!read_uint4(ja, v, joints) ||
				!read_float4(wa, v, weights, wa->normalized != 0)) {
			error.status = Status::bad_accessor;
			error.message = "failed to read skin influence";
			return false;
		}
		for (int i = 0; i < 4 && count < 8; ++i) {
			if (weights[i] == 0.f) continue; /* zero-weight slot never uses joint */
			if (!std::isfinite(weights[i])) {
				error.status = Status::bad_accessor;
				error.message = "non-finite skin weight";
				return false;
			}
			pairs[count++] = { joints[i], weights[i] };
		}
	}

	/* Merge duplicate joints. */
	for (cgltf_size i = 0; i < count; ++i) {
		if (pairs[i].weight == 0.f) continue;
		for (cgltf_size j = i + 1; j < count; ++j) {
			if (pairs[j].weight != 0.f && pairs[j].joint == pairs[i].joint) {
				pairs[i].weight += pairs[j].weight;
				pairs[j].weight = 0.f;
			}
		}
	}
	float sum = 0.f;
	for (cgltf_size i = 0; i < count; ++i) {
		if (pairs[i].weight != 0.f) {
			out.push_back({ int32_t(pairs[i].joint), pairs[i].weight });
			sum += pairs[i].weight;
		}
	}
	if (out.empty()) return true; /* unskinned vertex */
	if (!std::isfinite(sum) || sum == 0.f) {
		error.status = Status::bad_accessor;
		error.message = "skin weights cannot be normalized (zero sum)";
		return false;
	}
	std::sort(out.begin(), out.end(), [](const Influ& a, const Influ& b) {
		return a.joint < b.joint;
	});
	const float inv = 1.f / sum;
	for (Influ& inf : out) inf.weight *= inv;
	return true;
}

uint64_t edge_key(uint32_t a, uint32_t b) {
	if (a > b) std::swap(a, b);
	return (uint64_t(a) << 32) | uint64_t(b);
}

} /* anonymous namespace */

Status build_occluder_impl(const cgltf_data* data, const cgltf_mesh* mesh,
		Occluder& out, Error& error) {
	(void)data;
	auto fail = [&](Status s, const char* msg, cgltf_size p, cgltf_size t,
			cgltf_size v0 = 0, ErrorLocation* second_loc = nullptr) -> Status {
		error.status = s;
		error.message = msg;
		error.first = { p, t, v0 };
		if (second_loc) error.second = *second_loc;
		return s;
	};

	if (mesh->primitives_count == 0) {
		return fail(Status::invalid_argument, "mesh has no primitives", 0, 0);
	}

	cgltf_size target_count = mesh->primitives[0].targets_count;
	auto view_compressed = [](const cgltf_buffer_view* view) {
		return view && view->has_meshopt_compression;
	};
	for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
		const cgltf_primitive& prim = mesh->primitives[p];
		if (prim.has_draco_mesh_compression) {
			return fail(Status::unsupported_geometry, "Draco compression is not supported", p, 0);
		}
		if (view_compressed(prim.indices ? prim.indices->buffer_view : nullptr)) {
			return fail(Status::unsupported_geometry,
					"meshopt compression is not supported", p, 0);
		}
		for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
			if (view_compressed(prim.attributes[a].data->buffer_view)) {
				return fail(Status::unsupported_geometry,
						"meshopt compression is not supported", p, 0);
			}
		}
		for (cgltf_size t = 0; t < prim.targets_count; ++t) {
			for (cgltf_size a = 0; a < prim.targets[t].attributes_count; ++a) {
				if (view_compressed(prim.targets[t].attributes[a].data->buffer_view)) {
					return fail(Status::unsupported_geometry,
							"meshopt compression is not supported", p, 0);
				}
			}
		}
		if (prim.type != cgltf_primitive_type_triangles) {
			return fail(Status::unsupported_geometry,
					"only TRIANGLES primitives are supported", p, 0);
		}
		if (prim.targets_count != target_count) {
			return fail(Status::bad_accessor,
					"all primitives must expose the same target count", p, 0);
		}
		const cgltf_accessor* pos = find_attr(prim, cgltf_attribute_type_position, 0);
		if (!pos || pos->component_type != cgltf_component_type_r_32f ||
				pos->type != cgltf_type_vec3) {
			return fail(Status::bad_accessor,
					"POSITION must be a float VEC3 accessor", p, 0);
		}
		if (prim.indices) {
			const cgltf_component_type it = prim.indices->component_type;
			if (it != cgltf_component_type_r_8u &&
					it != cgltf_component_type_r_16u &&
					it != cgltf_component_type_r_32u) {
				return fail(Status::bad_accessor,
						"indices must be UNSIGNED_BYTE/SHORT/INT", p, 0);
			}
		}
	}

	auto impl = std::make_unique<Occluder::Impl>();
	impl->target_count = target_count;

	std::unordered_map<WKey, cgltf_size, WKeyHash> weld;
	weld.reserve(256);

	for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
		const cgltf_primitive& prim = mesh->primitives[p];
		const cgltf_accessor* pos = find_attr(prim, cgltf_attribute_type_position, 0);
		const cgltf_size vcount = pos->count;
		cgltf_size tri_count = prim.indices ? prim.indices->count / 3 : vcount / 3;

		/* Per-vertex welded ids for this primitive. */
		std::vector<cgltf_size> remap(vcount);

		for (cgltf_size v = 0; v < vcount; ++v) {
			WKey key;
			if (!read_vec3(pos, v, &key.pos.x)) {
				return fail(Status::bad_accessor, "failed to read POSITION", p, 0, v);
			}
			if (!finite3(&key.pos.x)) {
				return fail(Status::bad_accessor, "non-finite POSITION", p, 0, v);
			}
			key.morph.resize(target_count);
			for (cgltf_size t = 0; t < target_count; ++t) {
				const cgltf_accessor* dacc = target_position(prim, t);
				if (dacc) {
					if (dacc->component_type != cgltf_component_type_r_32f ||
							dacc->type != cgltf_type_vec3 ||
							!read_vec3(dacc, v, &key.morph[t].x)) {
						return fail(Status::bad_accessor,
								"bad POSITION morph target accessor", p, t, v);
					}
					if (!finite3(&key.morph[t].x)) {
						return fail(Status::bad_accessor,
								"non-finite morph delta", p, t, v);
					}
				} /* missing target => zero delta */
			}
			if (!build_influence(prim, v, key.skin, error)) {
				error.first = { p, 0, v };
				return error.status;
			}

			auto it = weld.find(key);
			if (it != weld.end()) {
				remap[v] = it->second;
			} else {
				BuildVertex bv;
				bv.pos = key.pos;
				bv.morph = key.morph;
				bv.skin = key.skin;
				cgltf_size id = impl->verts.size();
				impl->verts.push_back(std::move(bv));
				weld.emplace(std::move(key), id);
				remap[v] = id;
			}
		}

		for (cgltf_size t = 0; t < tri_count; ++t) {
			cgltf_size local[3];
			if (prim.indices) {
				for (int c = 0; c < 3; ++c) {
					cgltf_size raw = cgltf_accessor_read_index(prim.indices, t * 3 + c);
					if (raw >= vcount) {
						return fail(Status::bad_accessor, "index out of range", p, t, c);
					}
					local[c] = raw;
				}
			} else {
				local[0] = t * 3;
				local[1] = t * 3 + 1;
				local[2] = t * 3 + 2;
			}
			SrcFace f;
			f.src = { p, t };
			f.a = remap[local[0]];
			f.b = remap[local[1]];
			f.c = remap[local[2]];
			if (f.a == f.b || f.b == f.c || f.c == f.a) {
				return fail(Status::degenerate_face,
						"degenerate source triangle after welding", p, t);
			}
			const Vec3& pa = impl->verts[f.a].pos;
			const Vec3& pb = impl->verts[f.b].pos;
			const Vec3& pc = impl->verts[f.c].pos;
			const float abx = pb.x - pa.x, aby = pb.y - pa.y, abz = pb.z - pa.z;
			const float acx = pc.x - pa.x, acy = pc.y - pa.y, acz = pc.z - pa.z;
			const float n2 =
					(aby * acz - abz * acy) * (aby * acz - abz * acy) +
					(abz * acx - abx * acz) * (abz * acx - abx * acz) +
					(abx * acy - aby * acx) * (abx * acy - aby * acx);
			if (!(n2 > kAreaEps)) {
				return fail(Status::degenerate_face,
						"zero-area source triangle", p, t);
			}
			impl->faces.push_back(f);
			impl->face_sources.push_back(f.src);
		}
	}

	const cgltf_size F = impl->faces.size();
	const cgltf_size V = impl->verts.size();

	/* ---- Undirected edge manifold check: exactly two opposite faces ---- */
	struct UEdge {
		EdgeRec recs[2];
		int count = 0;
	};
	std::unordered_map<uint64_t, UEdge> edges;
	edges.reserve(F * 2);

	for (cgltf_size f = 0; f < F; ++f) {
		const SrcFace& fc = impl->faces[f];
		const cgltf_size corners[3] = { fc.a, fc.b, fc.c };
		for (int c = 0; c < 3; ++c) {
			uint32_t u = uint32_t(corners[c]);
			uint32_t w = uint32_t(corners[(c + 1) % 3]);
			uint64_t k = edge_key(u, w);
			UEdge& e = edges[k];
			if (e.count == 2) {
				ErrorLocation second = { fc.src.primitive, fc.src.triangle,
						static_cast<cgltf_size>(c) };
				return fail(Status::non_manifold,
						"edge shared by more than two faces (non-manifold)",
						fc.src.primitive, fc.src.triangle, c, &second);
			}
			e.recs[e.count++] = { f, c };
		}
	}
	for (auto& kv : edges) {
		if (kv.second.count != 2) {
			const EdgeRec& r = kv.second.recs[0];
			return fail(Status::non_manifold, "open edge with a single incident face",
					impl->faces[r.face].src.primitive,
					impl->faces[r.face].src.triangle, r.corner);
		}
		const EdgeRec& r0 = kv.second.recs[0];
		const EdgeRec& r1 = kv.second.recs[1];
		const SrcFace& f0 = impl->faces[r0.face];
		const SrcFace& f1 = impl->faces[r1.face];
		const cgltf_size c0[3] = { f0.a, f0.b, f0.c };
		const cgltf_size c1[3] = { f1.a, f1.b, f1.c };
		cgltf_size u0 = c0[r0.corner], w0 = c0[(r0.corner + 1) % 3];
		cgltf_size u1 = c1[r1.corner], w1 = c1[(r1.corner + 1) % 3];
		if (u0 == u1 && w0 == w1) {
			ErrorLocation second = { f1.src.primitive, f1.src.triangle,
					static_cast<cgltf_size>(r1.corner) };
			return fail(Status::non_manifold,
					"two faces traverse the same edge in the same direction",
					f0.src.primitive, f0.src.triangle, r0.corner, &second);
		}
	}

	/* ---- Duplicate face check ---- */
	std::vector<std::pair<uint64_t, cgltf_size>> face_keys;
	face_keys.reserve(F);
	for (cgltf_size f = 0; f < F; ++f) {
		const SrcFace& fc = impl->faces[f];
		uint32_t s[3] = { uint32_t(fc.a), uint32_t(fc.b), uint32_t(fc.c) };
		std::sort(s, s + 3);
		uint64_t hh = uint64_t(s[0]);
		hh = hh * 1000003ULL ^ uint64_t(s[1]);
		hh = hh * 1000003ULL ^ uint64_t(s[2]);
		face_keys.emplace_back(hh, f);
	}
	std::sort(face_keys.begin(), face_keys.end());
	for (cgltf_size i = 1; i < F; ++i) {
		if (face_keys[i].first != face_keys[i - 1].first) continue;
		const SrcFace& fa = impl->faces[face_keys[i - 1].second];
		const SrcFace& fb = impl->faces[face_keys[i].second];
		uint32_t sa[3] = { uint32_t(fa.a), uint32_t(fa.b), uint32_t(fa.c) };
		uint32_t sb[3] = { uint32_t(fb.a), uint32_t(fb.b), uint32_t(fb.c) };
		std::sort(sa, sa + 3);
		std::sort(sb, sb + 3);
		if (sa[0] == sb[0] && sa[1] == sb[1] && sa[2] == sb[2]) {
			ErrorLocation second = { fb.src.primitive, fb.src.triangle, 0 };
			return fail(Status::non_manifold, "duplicate face",
					fa.src.primitive, fa.src.triangle, 0, &second);
		}
	}

	/* ---- Vertex neighborhood: single face fan ---- */
	std::vector<std::vector<std::pair<uint32_t, uint32_t>>> incident(V);
	for (cgltf_size f = 0; f < F; ++f) {
		const SrcFace& fc = impl->faces[f];
		const cgltf_size corners[3] = { fc.a, fc.b, fc.c };
		for (int c = 0; c < 3; ++c) {
			incident[corners[c]].emplace_back(
					uint32_t(corners[(c + 1) % 3]), uint32_t(corners[(c + 2) % 3]));
		}
	}
	auto fan_fail = [&](const char* msg, cgltf_size v) -> Status {
		ErrorLocation loc{};
		loc.vertex = v;
		for (const SrcFace& fc : impl->faces) {
			if (fc.a == v || fc.b == v || fc.c == v) {
				loc.primitive = fc.src.primitive;
				loc.triangle = fc.src.triangle;
				break;
			}
		}
		return fail(Status::non_manifold, msg, loc.primitive, loc.triangle, v);
	};
	for (cgltf_size v = 0; v < V; ++v) {
		auto& nb = incident[v];
		const cgltf_size degree = nb.size();
		if (degree < 3) {
			return fan_fail("vertex neighborhood is not a closed fan", v);
		}
		std::unordered_map<uint32_t, uint32_t> next_map;
		next_map.reserve(degree * 2);
		for (auto& pr : nb) {
			if (!next_map.emplace(pr.first, pr.second).second) {
				return fan_fail("non-manifold vertex: duplicated outgoing edge", v);
			}
		}
		/* Walk the directed cycle and require it to visit every neighbor once. */
		uint32_t start = nb[0].first;
		uint32_t cur = start;
		cgltf_size steps = 0;
		for (;;) {
			auto it = next_map.find(cur);
			if (it == next_map.end()) {
				return fan_fail("vertex neighborhood splits into multiple fans", v);
			}
			cur = it->second;
			++steps;
			if (cur == start) break;
			if (steps > degree) {
				return fan_fail("vertex neighborhood is not a single face fan", v);
			}
		}
		if (steps != degree) {
			return fan_fail("vertex neighborhood does not form one fan", v);
		}
	}

	/* ---- Connected components via face adjacency ---- */
	impl->face_component.assign(F, (cgltf_size)-1);
	cgltf_size component_count = 0;
	std::vector<std::vector<cgltf_size>> adjacency(F);
	for (auto& kv : edges) {
		const cgltf_size f0 = kv.second.recs[0].face;
		const cgltf_size f1 = kv.second.recs[1].face;
		adjacency[f0].push_back(f1);
		adjacency[f1].push_back(f0);
	}
	std::vector<cgltf_size> stack;
	for (cgltf_size seed = 0; seed < F; ++seed) {
		if (impl->face_component[seed] != (cgltf_size)-1) continue;
		impl->face_component[seed] = component_count;
		stack.clear();
		stack.push_back(seed);
		while (!stack.empty()) {
			cgltf_size f = stack.back();
			stack.pop_back();
			for (cgltf_size other : adjacency[f]) {
				if (impl->face_component[other] == (cgltf_size)-1) {
					impl->face_component[other] = component_count;
					stack.push_back(other);
				}
			}
		}
		++component_count;
	}
	impl->component_count = component_count;
	impl->mesh = mesh;

	delete out.impl_;
	out.impl_ = impl.release();
	error.status = Status::ok;
	error.message = "ok";
	return Status::ok;
}

namespace {

/* ------------------------------------------------------------------ */
/* Matrix math (column-major, matching cgltf)                          */
/* ------------------------------------------------------------------ */

void mat4_identity(float m[16]) {
	std::memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.f;
}

bool is_affine(const float m[16]) {
	return m[3] == 0.f && m[7] == 0.f && m[11] == 0.f && m[15] == 1.f;
}

void mat4_multiply(float out[16], const float a[16], const float b[16]) {
	float r[16];
	for (int col = 0; col < 4; ++col) {
		for (int row = 0; row < 4; ++row) {
			float s = 0.f;
			for (int k = 0; k < 4; ++k) {
				s += a[k * 4 + row] * b[col * 4 + k];
			}
			r[col * 4 + row] = s;
		}
	}
	std::memcpy(out, r, sizeof(r));
}

void mat4_transform_point(const float m[16], const float p[3], float out[3]) {
	float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
	float y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
	float z = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
	out[0] = x;
	out[1] = y;
	out[2] = z;
}

/* ------------------------------------------------------------------ */
/* Shadow volume implementation                                       */
/* ------------------------------------------------------------------ */

} /* anonymous namespace */

Status generate_shadow_impl(const Occluder& resource, const cgltf_data* data,
		const cgltf_node* node, const float* world_matrices,
		const float* override, cgltf_size override_count,
		const float light[3], ShadowVolume& out, Error& error) {
	auto fail = [&](Status s, const char* msg) -> Status {
		error.status = s;
		error.message = msg;
		error.first = {};
		error.second = {};
		return s;
	};

	const Occluder::Impl* R = resource.impl_;
	if (!R) return fail(Status::invalid_argument, "occluder resource is empty");
	if (!data || !node || !world_matrices || !light) {
		return fail(Status::invalid_argument, "null required argument");
	}
	if (node->mesh == nullptr) {
		return fail(Status::invalid_argument, "node does not reference a mesh");
	}
	/* The node must reference the mesh the resource was built from. */
	if (node->mesh != R->mesh) {
		return fail(Status::invalid_argument,
				"node mesh does not match the occluder resource");
	}
	if (!finite3(light)) return fail(Status::bad_light, "non-finite light position");
	for (cgltf_size i = 0; i < data->nodes_count; ++i) {
		const float* m = world_matrices + 16 * i;
		if (!finite16(m) || !is_affine(m)) {
			return fail(Status::bad_matrix, "invalid (non-finite or non-affine) world matrix");
		}
	}
	const float* node_world = world_matrices + 16 * cgltf_node_index(data, node);

	/* ---- Resolve morph weights ---- */
	const cgltf_size T = R->target_count;
	std::vector<float> weights(T, 0.f);
	const float* src = nullptr;
	cgltf_size src_count = 0;
	if (override) {
		src = override;
		src_count = override_count;
	} else if (node->weights && node->weights_count) {
		src = node->weights;
		src_count = node->weights_count;
	} else if (node->mesh->weights && node->mesh->weights_count) {
		src = node->mesh->weights;
		src_count = node->mesh->weights_count;
	}
	if (src) {
		if (src_count != T) {
			return fail(Status::bad_weights, "morph weight count does not match target count");
		}
		for (cgltf_size t = 0; t < T; ++t) {
			if (!std::isfinite(src[t])) {
				return fail(Status::bad_weights, "non-finite morph weight");
			}
			weights[t] = src[t];
		}
	}

	/* ---- Skin matrices for this instance ---- */
	const cgltf_skin* skin = node->skin;
	std::vector<float> joint_matrices;
	bool skinned = false;
	if (skin) {
		skinned = true;
		joint_matrices.resize(skin->joints_count * 16);
		for (cgltf_size j = 0; j < skin->joints_count; ++j) {
			const cgltf_node* jn = skin->joints[j];
			if (!jn) return fail(Status::bad_matrix, "skin references a null joint node");
			cgltf_size jidx = cgltf_node_index(data, jn);
			const float* joint_world = world_matrices + 16 * jidx;
			float ibm[16];
			if (skin->inverse_bind_matrices) {
				float ib[16];
				if (!cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ib, 16)) {
					return fail(Status::bad_matrix, "failed to read inverse bind matrix");
				}
				if (!finite16(ib)) {
					return fail(Status::bad_matrix, "non-finite inverse bind matrix");
				}
				std::memcpy(ibm, ib, sizeof(ibm));
			} else {
				mat4_identity(ibm);
			}
			mat4_multiply(&joint_matrices[j * 16], joint_world, ibm);
			if (!finite16(&joint_matrices[j * 16])) {
				return fail(Status::bad_matrix, "non-finite skin matrix");
			}
		}
	}

	/* ---- Deform every welded vertex ---- */
	const cgltf_size V = R->verts.size();
	std::vector<Vec3> world(V);
	for (cgltf_size v = 0; v < V; ++v) {
		const BuildVertex& bv = R->verts[v];
		float local[3] = { bv.pos.x, bv.pos.y, bv.pos.z };
		for (cgltf_size t = 0; t < T; ++t) {
			const float w = weights[t];
			if (w != 0.f) {
				local[0] += w * bv.morph[t].x;
				local[1] += w * bv.morph[t].y;
				local[2] += w * bv.morph[t].z;
			}
		}
		if (!finite3(local)) {
			return fail(Status::degenerate_frame, "non-finite morphed position");
		}

		Vec3 wp;
		if (skinned && !bv.skin.empty()) {
			float acc[3] = { 0.f, 0.f, 0.f };
			for (const Influ& inf : bv.skin) {
				if (inf.joint < 0 || cgltf_size(inf.joint) >= skin->joints_count) {
					return fail(Status::bad_matrix, "joint index outside skin joint count");
				}
				const float* jm = &joint_matrices[inf.joint * 16];
				float tp[3];
				mat4_transform_point(jm, local, tp);
				acc[0] += inf.weight * tp[0];
				acc[1] += inf.weight * tp[1];
				acc[2] += inf.weight * tp[2];
			}
			wp.x = acc[0];
			wp.y = acc[1];
			wp.z = acc[2];
		} else {
			mat4_transform_point(node_world, local, &wp.x);
		}
		if (!finite3(&wp.x)) {
			return fail(Status::degenerate_frame, "non-finite deformed world position");
		}
		world[v] = wp;
	}

	/* ---- Per-component outward orientation from signed volume ---- */
	const cgltf_size F = R->faces.size();
	std::vector<int> flip(R->component_count, 0);
	std::vector<double> volume(R->component_count, 0.0);
	for (cgltf_size f = 0; f < F; ++f) {
		const SrcFace& fc = R->faces[f];
		const Vec3& a = world[fc.a];
		const Vec3& b = world[fc.b];
		const Vec3& c = world[fc.c];
		double six = double(a.x) * (double(b.y) * c.z - double(b.z) * c.y)
				- double(a.y) * (double(b.x) * c.z - double(b.z) * c.x)
				+ double(a.z) * (double(b.x) * c.y - double(b.y) * c.x);
		volume[R->face_component[f]] += six;
	}
	for (cgltf_size k = 0; k < R->component_count; ++k) {
		if (!(std::abs(volume[k]) > 0.0)) {
			return fail(Status::degenerate_frame, "component has zero signed volume");
		}
		flip[k] = volume[k] < 0.0 ? 1 : 0;
	}

	/* ---- Per-face current geometry, area and lit flag ---- */
	struct FrameFace {
		cgltf_size a, b, c;
		Vec3 normal;
		bool lit;
	};
	std::vector<FrameFace> frame(F);
	for (cgltf_size f = 0; f < F; ++f) {
		const SrcFace& src_f = R->faces[f];
		cgltf_size a = src_f.a, b = src_f.b, c = src_f.c;
		if (flip[R->face_component[f]]) std::swap(b, c);
		const Vec3& pa = world[a];
		const Vec3& pb = world[b];
		const Vec3& pc = world[c];
		Vec3 ab = { pb.x - pa.x, pb.y - pa.y, pb.z - pa.z };
		Vec3 ac = { pc.x - pa.x, pc.y - pa.y, pc.z - pa.z };
		Vec3 n = {
			ab.y * ac.z - ab.z * ac.y,
			ab.z * ac.x - ab.x * ac.z,
			ab.x * ac.y - ab.y * ac.x
		};
		float area2 = n.x * n.x + n.y * n.y + n.z * n.z;
		if (!(area2 > kAreaEps)) {
			return fail(Status::degenerate_frame, "degenerate triangle in current pose");
		}
		Vec3 lp = { light[0] - pa.x, light[1] - pa.y, light[2] - pa.z };
		float dot = n.x * lp.x + n.y * lp.y + n.z * lp.z;
		bool lit = dot > 0.f; /* exact zero counts as back lit */
		frame[f] = { a, b, c, n, lit };
	}

	/* ---- Build undirected edge -> the two incident frame faces ---- */
	struct EInfo {
		cgltf_size face[2] = { (cgltf_size)-1, (cgltf_size)-1 };
		int corner[2] = { -1, -1 };
		int count = 0;
	};
	std::unordered_map<uint64_t, EInfo> emap;
	emap.reserve(F * 2);
	for (cgltf_size f = 0; f < F; ++f) {
		const FrameFace& ff = frame[f];
		const cgltf_size cn[3] = { ff.a, ff.b, ff.c };
		for (int c = 0; c < 3; ++c) {
			EInfo& e = emap[edge_key(uint32_t(cn[c]), uint32_t(cn[(c + 1) % 3]))];
			e.face[e.count] = f;
			e.corner[e.count] = c;
			++e.count;
		}
	}

	auto impl = std::make_unique<ShadowVolume::Impl>();
	impl->n = V;
	impl->vertices.resize(V * 2 * 4);
	for (cgltf_size v = 0; v < V; ++v) {
		float* fin = &impl->vertices[v * 4];
		fin[0] = world[v].x;
		fin[1] = world[v].y;
		fin[2] = world[v].z;
		fin[3] = 1.f;
		float* far = &impl->vertices[(V + v) * 4];
		far[0] = world[v].x - light[0];
		far[1] = world[v].y - light[1];
		far[2] = world[v].z - light[2];
		far[3] = 0.f;
	}

	auto push_tri = [&](cgltf_uint i0, cgltf_uint i1, cgltf_uint i2, const FaceSource& s) {
		impl->indices.push_back(i0);
		impl->indices.push_back(i1);
		impl->indices.push_back(i2);
		impl->tri_sources.push_back(s);
	};

	/* ---- Near cap: lit faces keep outward winding ---- */
	for (cgltf_size f = 0; f < F; ++f) {
		if (!frame[f].lit) continue;
		const FrameFace& ff = frame[f];
		push_tri(cgltf_uint(ff.a), cgltf_uint(ff.b), cgltf_uint(ff.c),
				R->faces[f].src);
	}

	/* ---- Far cap: reversed copies at infinity ---- */
	for (cgltf_size f = 0; f < F; ++f) {
		if (!frame[f].lit) continue;
		const FrameFace& ff = frame[f];
		push_tri(cgltf_uint(V + ff.a), cgltf_uint(V + ff.c), cgltf_uint(V + ff.b),
				R->faces[f].src);
	}

	/* ---- Side walls on edges lit from exactly one side ---- */
	for (auto& kv : emap) {
		EInfo& e = kv.second;
		if (e.count != 2) continue; /* guaranteed closed by the resource build */
		const cgltf_size f0 = e.face[0];
		const cgltf_size f1 = e.face[1];
		if (frame[f0].lit == frame[f1].lit) continue;
		const cgltf_size lf = frame[f0].lit ? f0 : f1;
		const int lc = frame[f0].lit ? e.corner[0] : e.corner[1];
		const FrameFace& ff = frame[lf];
		const cgltf_size cn[3] = { ff.a, ff.b, ff.c };
		const cgltf_size u = cn[lc];
		const cgltf_size w = cn[(lc + 1) % 3];
		const FaceSource& s = R->faces[lf].src;
		/*
		 * The near cap contributes the directed edge u -> w (lit face
		 * winding); the wall must contribute the opposite w -> u edge.
		 * Both triangles are wound so the wall faces away from the light.
		 * T1: u, far(w), w ; T2: u, far(u), far(w).
		 */
		push_tri(cgltf_uint(u), cgltf_uint(V + w), cgltf_uint(w), s);
		push_tri(cgltf_uint(u), cgltf_uint(V + u), cgltf_uint(V + w), s);
		impl->silhouette.push_back({ s.primitive, s.triangle, lc });
	}

	/* Only replace the previous result after a fully successful generation. */
	delete out.impl_;
	out.impl_ = impl.release();
	error.status = Status::ok;
	error.message = "ok";
	return Status::ok;
}

/* ------------------------------------------------------------------ */
/* Public Occluder                                                    */
/* ------------------------------------------------------------------ */

Occluder::~Occluder() { delete impl_; }

Occluder::Occluder(Occluder&& other) noexcept : impl_(other.impl_) {
	other.impl_ = nullptr;
}

Occluder& Occluder::operator=(Occluder&& other) noexcept {
	if (this != &other) {
		delete impl_;
		impl_ = other.impl_;
		other.impl_ = nullptr;
	}
	return *this;
}

cgltf_size Occluder::vertex_count() const {
	return impl_ ? impl_->verts.size() : 0;
}

cgltf_size Occluder::face_count() const {
	return impl_ ? impl_->faces.size() : 0;
}

cgltf_size Occluder::target_count() const {
	return impl_ ? impl_->target_count : 0;
}

cgltf_size Occluder::component_count() const {
	return impl_ ? impl_->component_count : 0;
}

const FaceSource* Occluder::face_sources() const {
	return impl_ ? impl_->face_sources.data() : nullptr;
}

/* ------------------------------------------------------------------ */
/* Public ShadowVolume                                                */
/* ------------------------------------------------------------------ */

ShadowVolume::~ShadowVolume() { delete impl_; }

ShadowVolume::ShadowVolume(ShadowVolume&& other) noexcept : impl_(other.impl_) {
	other.impl_ = nullptr;
}

ShadowVolume& ShadowVolume::operator=(ShadowVolume&& other) noexcept {
	if (this != &other) {
		delete impl_;
		impl_ = other.impl_;
		other.impl_ = nullptr;
	}
	return *this;
}

cgltf_size ShadowVolume::finite_count() const {
	return impl_ ? impl_->n : 0;
}

cgltf_size ShadowVolume::far_count() const {
	return impl_ ? impl_->n : 0;
}

const float* ShadowVolume::vertices() const {
	return impl_ ? impl_->vertices.data() : nullptr;
}

cgltf_size ShadowVolume::index_count() const {
	return impl_ ? impl_->indices.size() : 0;
}

const cgltf_uint* ShadowVolume::indices() const {
	return impl_ ? impl_->indices.data() : nullptr;
}

cgltf_size ShadowVolume::triangle_source_count() const {
	return impl_ ? impl_->tri_sources.size() : 0;
}

const FaceSource* ShadowVolume::triangle_sources() const {
	return impl_ ? impl_->tri_sources.data() : nullptr;
}

cgltf_size ShadowVolume::silhouette_edge_count() const {
	return impl_ ? impl_->silhouette.size() : 0;
}

const EdgeSource* ShadowVolume::silhouette_edges() const {
	return impl_ ? impl_->silhouette.data() : nullptr;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                */
/* ------------------------------------------------------------------ */

Status build(const cgltf_data* data, const cgltf_mesh* mesh,
		Occluder& out, Error& error) {
	error = Error{};
	if (!data || !mesh) {
		error.status = Status::invalid_argument;
		error.message = "null data or mesh";
		return error.status;
	}
	bool owned = false;
	for (cgltf_size i = 0; i < data->meshes_count; ++i) {
		if (&data->meshes[i] == mesh) { owned = true; break; }
	}
	if (!owned) {
		error.status = Status::invalid_argument;
		error.message = "mesh is not part of the supplied cgltf_data";
		return error.status;
	}
	try {
		return build_occluder_impl(data, mesh, out, error);
	} catch (const std::bad_alloc&) {
		error.status = Status::out_of_memory;
		error.message = status_message(Status::out_of_memory);
		return Status::out_of_memory;
	}
}

Status generate(const Occluder& resource, const cgltf_data* data,
		const cgltf_node* node, const float* world_matrices,
		const float* override, cgltf_size override_count,
		const float light[3], ShadowVolume& out, Error& error) {
	error = Error{};
	try {
		return generate_shadow_impl(resource, data, node, world_matrices,
				override, override_count, light, out, error);
	} catch (const std::bad_alloc&) {
		error.status = Status::out_of_memory;
		error.message = status_message(Status::out_of_memory);
		return Status::out_of_memory;
	}
}

Status generate(const Occluder& resource, const cgltf_data* data,
		const cgltf_node* node, const float* world_matrices,
		const float light[3], ShadowVolume& out, Error& error) {
	return generate(resource, data, node, world_matrices, nullptr, 0,
			light, out, error);
}

const char* status_message(Status status) {
	switch (status) {
		case Status::ok: return "ok";
		case Status::invalid_argument: return "invalid argument";
		case Status::unsupported_geometry: return "unsupported geometry";
		case Status::bad_accessor: return "bad accessor";
		case Status::non_manifold: return "non-manifold topology";
		case Status::degenerate_face: return "degenerate source face";
		case Status::bad_matrix: return "bad matrix";
		case Status::bad_weights: return "bad morph weights";
		case Status::bad_light: return "bad light";
		case Status::degenerate_frame: return "degenerate frame";
		case Status::out_of_memory: return "out of memory";
	}
	return "unknown";
}

} /* namespace skin_shadow */
