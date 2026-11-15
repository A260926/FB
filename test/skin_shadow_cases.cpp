/*
 * skin_shadow_cases.cpp - task cases for the skin_shadow example.
 *
 * Registered through task_support/check.h in three groups:
 *   topology    : welding rules and manifold rejection
 *   deformation : morph/skin evaluation against an independent reference
 *   volume      : orientation, silhouette and finite/far edge identity
 *
 * The file has no main(); task_support/driver.c drives task_cases. Every
 * fixture is serialized to a real glTF/bin pair in TMPDIR, parsed, buffer
 * loaded and validated through cgltf before being handed to skin_shadow.
 */
#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "cgltf.h"
#include "examples/skin_shadow.h"
#include "examples/skin_shadow_demo.h"

namespace {

constexpr float kEps = 1e-5f;

using skin_shadow::EdgeSource;
using skin_shadow::Error;
using skin_shadow::FaceSource;
using skin_shadow::Occluder;
using skin_shadow::ShadowVolume;
using skin_shadow::Status;

/* ------------------------------------------------------------------ */
/* Small math helpers                                                  */
/* ------------------------------------------------------------------ */

struct V3 {
	float x = 0.f, y = 0.f, z = 0.f;
};

float vdot(const V3& a, const V3& b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

V3 vcross(const V3& a, const V3& b) {
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

V3 vsub(const V3& a, const V3& b) {
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

V3 vadd(const V3& a, const V3& b) {
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}

V3 vscale(const V3& a, float s) {
	return { a.x * s, a.y * s, a.z * s };
}

float vdist(const V3& a, const V3& b) {
	const V3 d = vsub(a, b);
	return std::sqrt(vdot(d, d));
}

void mat_identity(float m[16]) {
	std::memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.f;
}

void mat_mul(float out[16], const float a[16], const float b[16]) {
	float r[16];
	for (int col = 0; col < 4; ++col) {
		for (int row = 0; row < 4; ++row) {
			float s = 0.f;
			for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[col * 4 + k];
			r[col * 4 + row] = s;
		}
	}
	std::memcpy(out, r, sizeof(r));
}

V3 mat_point(const float m[16], const V3& p) {
	return {
		m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
		m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
		m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]
	};
}

bool all_finite(const V3& p) {
	return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

/* ------------------------------------------------------------------ */
/* Minimal glTF builder                                                */
/* ------------------------------------------------------------------ */

struct GView {
	cgltf_size offset = 0;
	cgltf_size size = 0;
	cgltf_size stride = 0;
	int target = 0;
};

struct GAcc {
	int view = -1;                 /* -1 => no base buffer view (zeros) */
	int comp = 5126;
	std::string type = "VEC3";
	cgltf_size count = 0;
	bool normalized = false;
	bool sparse = false;
	cgltf_size sparse_count = 0;
	int sparse_idx_view = -1;
	int sparse_idx_comp = 5121;
	int sparse_val_view = -1;
};

struct GAttr {
	std::string name;
	int acc = -1;
};

struct GTarget {
	std::vector<GAttr> attrs;
};

struct GPrim {
	std::vector<GAttr> attrs;
	std::vector<GTarget> targets;
	int indices = -1;
	int material = -1;
	int mode = 4; /* TRIANGLES */
};

struct GMesh {
	std::vector<GPrim> prims;
	std::vector<float> weights;
};

struct GNode {
	int mesh = -1;
	int skin = -1;
	bool has_matrix = false;
	float matrix[16];
	std::vector<float> weights;
};

struct GSkin {
	std::vector<int> joints;
	int ibm = -1; /* -1 => no inverse bind matrices */
};

class GltfBuilder {
public:
	std::vector<unsigned char> bin;
	std::vector<GView> views;
	std::vector<GAcc> accs;
	std::vector<GMesh> meshes;
	std::vector<GNode> nodes;
	std::vector<GSkin> skins;
	int material_count = 0;
	std::string buffer_uri_ = "a.bin";

	int add_view(const void* data, cgltf_size size, cgltf_size stride = 0, int target = 0) {
		align4();
		GView v;
		v.offset = bin.size();
		v.size = size;
		v.stride = stride;
		v.target = target;
		const unsigned char* p = static_cast<const unsigned char*>(data);
		bin.insert(bin.end(), p, p + size);
		views.push_back(v);
		return int(views.size()) - 1;
	}

	int add_acc(int view, int comp, std::string type, cgltf_size count,
			bool normalized = false) {
		GAcc a;
		a.view = view;
		a.comp = comp;
		a.type = std::move(type);
		a.count = count;
		a.normalized = normalized;
		accs.push_back(a);
		return int(accs.size()) - 1;
	}

	int add_sparse_acc(int base_view, int comp, std::string type, cgltf_size count,
			int idx_view, int idx_comp, int val_view, cgltf_size sparse_count) {
		GAcc a;
		a.view = base_view;
		a.comp = comp;
		a.type = std::move(type);
		a.count = count;
		a.sparse = true;
		a.sparse_count = sparse_count;
		a.sparse_idx_view = idx_view;
		a.sparse_idx_comp = idx_comp;
		a.sparse_val_view = val_view;
		accs.push_back(a);
		return int(accs.size()) - 1;
	}

	int add_mesh(GMesh m) {
		meshes.push_back(std::move(m));
		return int(meshes.size()) - 1;
	}

	int add_node(const GNode& n) {
		nodes.push_back(n);
		return int(nodes.size()) - 1;
	}

	int add_skin(const GSkin& s) {
		skins.push_back(s);
		return int(skins.size()) - 1;
	}

	std::string write_and_parse(const std::string& tag, cgltf_data** out_data) {
		const char* tmp = std::getenv("TMPDIR");
		std::string dir = tmp ? tmp : "/tmp/";
		if (!dir.empty() && dir.back() != '/') dir.push_back('/');
		const std::string base = dir + "sscase_" + tag + "_";
		const std::string gltf = base + "a.gltf";
		const std::string binpath = base + "a.bin";
		buffer_uri_ = "sscase_" + tag + "_a.bin";
		{
			std::ofstream bf(binpath, std::ios::binary);
			bf.write(reinterpret_cast<const char*>(bin.data()),
					std::streamsize(bin.size()));
		}
		{
			std::ofstream jf(gltf, std::ios::binary);
			jf << json();
		}
		cgltf_options opts{};
		cgltf_data* data = nullptr;
		if (cgltf_parse_file(&opts, gltf.c_str(), &data) != cgltf_result_success) {
			*out_data = nullptr;
			return gltf;
		}
		if (cgltf_load_buffers(&opts, data, gltf.c_str()) != cgltf_result_success ||
				cgltf_validate(data) != cgltf_result_success) {
			cgltf_free(data);
			*out_data = nullptr;
			return gltf;
		}
		*out_data = data;
		return gltf;
	}

private:
	void align4() {
		while (bin.size() % 4 != 0) bin.push_back(0);
	}

	static std::string fnum(float v) {
		char b[64];
		std::snprintf(b, sizeof(b), "%.9g", double(v));
		return std::string(b);
	}

	std::string json() const {
		std::string s = "{\"asset\":{\"version\":\"2.0\"}";
		s += ",\"buffers\":[{\"uri\":\"" + buffer_uri_ +
				"\",\"byteLength\":" + std::to_string(bin.size()) + "}]";
		s += ",\"bufferViews\":[";
		for (size_t i = 0; i < views.size(); ++i) {
			const GView& v = views[i];
			s += (i ? std::string(",") : std::string("")) +
					"{\"buffer\":0,\"byteOffset\":" +
					std::to_string(v.offset) + ",\"byteLength\":" +
					std::to_string(v.size);
			if (v.stride) s += ",\"byteStride\":" + std::to_string(v.stride);
			if (v.target) s += ",\"target\":" + std::to_string(v.target);
			s += "}";
		}
		s += "],\"accessors\":[";
		for (size_t i = 0; i < accs.size(); ++i) {
			const GAcc& a = accs[i];
			s += i ? "," : "";
			s += "{\"componentType\":" + std::to_string(a.comp) +
					",\"count\":" + std::to_string(a.count) + ",\"type\":\"" + a.type + "\"";
			if (a.normalized) s += ",\"normalized\":true";
			if (a.view >= 0) s += ",\"bufferView\":" + std::to_string(a.view);
			if (a.sparse) {
				s += ",\"sparse\":{\"count\":" + std::to_string(a.sparse_count) +
						",\"indices\":{\"bufferView\":" +
						std::to_string(a.sparse_idx_view) + ",\"componentType\":" +
						std::to_string(a.sparse_idx_comp) +
						"},\"values\":{\"bufferView\":" +
						std::to_string(a.sparse_val_view) + "}}";
			}
			s += "}";
		}
		s += "],\"materials\":[";
		for (int i = 0; i < material_count; ++i) {
			s += (i ? "," : "") + std::string("{\"name\":\"m") + std::to_string(i) + "\"}";
		}
		s += "],\"meshes\":[";
		for (size_t mi = 0; mi < meshes.size(); ++mi) {
			const GMesh& m = meshes[mi];
			s += mi ? "," : "";
			s += "{\"primitives\":[";
			for (size_t pi = 0; pi < m.prims.size(); ++pi) {
				const GPrim& p = m.prims[pi];
				s += pi ? "," : "";
				s += "{\"mode\":" + std::to_string(p.mode) + ",\"attributes\":{";
				for (size_t ai = 0; ai < p.attrs.size(); ++ai) {
					s += (ai ? std::string(",") : std::string("")) + "\"" +
							p.attrs[ai].name + "\":" +
							std::to_string(p.attrs[ai].acc);
				}
				s += "}";
				if (!p.targets.empty()) {
					s += ",\"targets\":[";
					for (size_t ti = 0; ti < p.targets.size(); ++ti) {
						const GTarget& t = p.targets[ti];
						s += ti ? "," : "";
						s += "{";
						for (size_t ai = 0; ai < t.attrs.size(); ++ai) {
							s += (ai ? std::string(",") : std::string("")) + "\"" +
									t.attrs[ai].name + "\":" +
									std::to_string(t.attrs[ai].acc);
						}
						s += "}";
					}
					s += "]";
				}
				if (p.indices >= 0) s += ",\"indices\":" + std::to_string(p.indices);
				if (p.material >= 0) s += ",\"material\":" + std::to_string(p.material);
				s += "}";
			}
			s += "]";
			if (!m.weights.empty()) {
				s += ",\"weights\":[";
				for (size_t i = 0; i < m.weights.size(); ++i) {
					s += (i ? "," : "") + fnum(m.weights[i]);
				}
				s += "]";
			}
			s += "}";
		}
		s += "],\"nodes\":[";
		for (size_t i = 0; i < nodes.size(); ++i) {
			const GNode& n = nodes[i];
			s += i ? "," : "";
			s += "{";
			bool first = true;
			if (n.mesh >= 0) {
				s += "\"mesh\":" + std::to_string(n.mesh);
				first = false;
			}
			if (n.skin >= 0) {
				if (!first) s += ",";
				s += "\"skin\":" + std::to_string(n.skin);
				first = false;
			}
			if (n.has_matrix) {
				if (!first) s += ",";
				s += "\"matrix\":[";
				for (int k = 0; k < 16; ++k) s += (k ? "," : "") + fnum(n.matrix[k]);
				s += "]";
				first = false;
			}
			if (!n.weights.empty()) {
				if (!first) s += ",";
				s += "\"weights\":[";
				for (size_t k = 0; k < n.weights.size(); ++k) {
					s += (k ? "," : "") + fnum(n.weights[k]);
				}
				s += "]";
			}
			s += "}";
		}
		s += "],\"skins\":[";
		for (size_t i = 0; i < skins.size(); ++i) {
			const GSkin& sk = skins[i];
			s += i ? "," : "";
			s += "{\"joints\":[";
			for (size_t j = 0; j < sk.joints.size(); ++j) {
				s += (j ? "," : "") + std::to_string(sk.joints[j]);
			}
			s += "]";
			if (sk.ibm >= 0) s += ",\"inverseBindMatrices\":" + std::to_string(sk.ibm);
			s += "}";
		}
		s += "],\"scenes\":[{\"nodes\":[";
		for (size_t i = 0; i < nodes.size(); ++i) s += (i ? "," : "") + std::to_string(i);
		s += "]}],\"scene\":0}";
		return s;
	}
};

struct ParsedFixture {
	cgltf_data* data = nullptr;
	std::string gltf_path;
	std::string bin_path;

	~ParsedFixture() {
		if (data) cgltf_free(data);
		if (!gltf_path.empty()) std::remove(gltf_path.c_str());
		if (!bin_path.empty()) std::remove(bin_path.c_str());
	}
};

bool parse_fixture(GltfBuilder& b, const std::string& tag, ParsedFixture& fx) {
	cgltf_data* data = nullptr;
	fx.gltf_path = b.write_and_parse(tag, &data);
	fx.data = data;
	if (!data) return false;
	fx.bin_path = fx.gltf_path.substr(0, fx.gltf_path.size() - 5) + "bin";
	return true;
}

/* ------------------------------------------------------------------ */
/* Canonical tetrahedron geometry (outward wound)                     */
/* ------------------------------------------------------------------ */

const V3 kTetra[4] = {
	{ 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.5f, 1.f, 0.f }, { 0.5f, 0.4f, 1.f }
};

/* Welded outward faces: (0,2,1),(0,1,3),(0,3,2),(1,2,3). */
const int kFaces[4][3] = {
	{ 0, 2, 1 }, { 0, 1, 3 }, { 0, 3, 2 }, { 1, 2, 3 }
};

int add_position_view(GltfBuilder& b, const std::vector<V3>& pts, cgltf_size stride) {
	std::vector<unsigned char> raw(stride * pts.size(), 0);
	for (size_t i = 0; i < pts.size(); ++i) {
		std::memcpy(raw.data() + i * stride, &pts[i], 3 * sizeof(float));
	}
	return b.add_view(raw.data(), raw.size(), stride);
}

int add_vec3_view(GltfBuilder& b, const std::vector<V3>& vecs) {
	return b.add_view(vecs.data(), vecs.size() * 3 * sizeof(float), 12);
}

int add_scaled_normals(GltfBuilder& b, int count, float nx, float ny, float nz) {
	std::vector<V3> n(count, { nx, ny, nz });
	return add_vec3_view(b, n);
}

int add_uvs(GltfBuilder& b, int count, float offset_u) {
	std::vector<float> uv(count * 2);
	for (int i = 0; i < count; ++i) {
		uv[2 * i] = offset_u + 0.1f * i;
		uv[2 * i + 1] = offset_u;
	}
	return b.add_view(uv.data(), uv.size() * sizeof(float), 8);
}

int add_ushort_indices(GltfBuilder& b, const std::vector<unsigned short>& idx) {
	return b.add_view(idx.data(), idx.size() * sizeof(unsigned short),
			sizeof(unsigned short), 34963);
}

/* Two-prim tetra with duplicated seam vertices and distinct materials. */
void build_two_material_tetra(GltfBuilder& b, int& mesh_idx,
		const std::vector<V3>* morph0 = nullptr,
		const std::vector<V3>* morph1 = nullptr,
		const std::vector<std::vector<float>>* weights0 = nullptr,
		const std::vector<std::vector<float>>* weights1 = nullptr,
		const std::vector<std::vector<int>>* joints0 = nullptr,
		const std::vector<std::vector<int>>* joints1 = nullptr,
		bool normalized_weights = false) {
	b.material_count = 2;
	/* Primitive 0 ordering A,B,C,D; primitive 1 ordering B,C,A,D. */
	const int reorder1[4] = { 1, 2, 0, 3 };
	std::vector<V3> p0(4), p1(4);
	for (int i = 0; i < 4; ++i) {
		p0[i] = kTetra[i];
		p1[i] = kTetra[reorder1[i]];
	}
	const int pos0v = add_position_view(b, p0, 12);
	const int pos1v = add_position_view(b, p1, 12);
	const int n0v = add_scaled_normals(b, 4, 0.f, 0.f, 1.f);
	const int n1v = add_scaled_normals(b, 4, 1.f, 0.f, 0.f);
	const int uv0v = add_uvs(b, 4, 0.0f);
	const int uv1v = add_uvs(b, 4, 0.5f);
	const int pos0a = b.add_acc(pos0v, 5126, "VEC3", 4);
	const int pos1a = b.add_acc(pos1v, 5126, "VEC3", 4);
	const int n0a = b.add_acc(n0v, 5126, "VEC3", 4);
	const int n1a = b.add_acc(n1v, 5126, "VEC3", 4);
	const int uv0a = b.add_acc(uv0v, 5126, "VEC2", 4);
	const int uv1a = b.add_acc(uv1v, 5126, "VEC2", 4);

	GMesh m;
	GPrim pr0, pr1;
	pr0.attrs = { { "POSITION", pos0a }, { "NORMAL", n0a }, { "TEXCOORD_0", uv0a } };
	pr1.attrs = { { "POSITION", pos1a }, { "NORMAL", n1a }, { "TEXCOORD_0", uv1a } };
	pr0.material = 0;
	pr1.material = 1;

	if (morph0 || morph1) {
		const std::vector<V3>& d0 = morph0 ? *morph0 : std::vector<V3>(4, { 0, 0, 0 });
		const std::vector<V3>& d1 = morph1 ? *morph1 : std::vector<V3>(4, { 0, 0, 0 });
		const int d0v = add_vec3_view(b, d0);
		const int d1v = add_vec3_view(b, d1);
		const int d0a = b.add_acc(d0v, 5126, "VEC3", 4);
		const int d1a = b.add_acc(d1v, 5126, "VEC3", 4);
		pr0.targets.push_back(GTarget{ { { "POSITION", d0a } } });
		pr1.targets.push_back(GTarget{ { { "POSITION", d1a } } });
	}

	auto add_skin = [&](GPrim& pr, const std::vector<std::vector<float>>& weights,
			const std::vector<std::vector<int>>& joints, int /*vertex_count*/) {
		const int n = 4;
		if (weights.empty()) return;
		for (int set = 0; set < 2; ++set) {
			bool any = false;
			for (int i = 0; i < n; ++i) {
				if (size_t(set) < joints[i].size() && joints[i][set] >= 0) any = true;
			}
			if (!any) continue;
			std::vector<uint16_t> jv(n * 4, 0);
			for (int i = 0; i < n; ++i) {
				for (int k = 0; k < 4; ++k) {
					const int idx = set * 4 + k;
					jv[i * 4 + k] = idx < int(joints[i].size()) && joints[i][idx] >= 0
							? uint16_t(joints[i][idx]) : 0;
				}
			}
			const int jview = b.add_view(jv.data(), jv.size() * 2, 8, 34962);
			const int jacc = b.add_acc(jview, 5123, "VEC4", n);
			pr.attrs.push_back({ set == 0 ? "JOINTS_0" : "JOINTS_1", jacc });

			if (normalized_weights) {
				std::vector<uint8_t> wv(n * 4, 0);
				for (int i = 0; i < n; ++i) {
					for (int k = 0; k < 4; ++k) {
						const int idx = set * 4 + k;
						const float w = idx < int(weights[i].size()) ? weights[i][idx] : 0.f;
						wv[i * 4 + k] = uint8_t(std::lround(w * 255.f));
					}
				}
				const int wview = b.add_view(wv.data(), wv.size(), 4, 34962);
				const int wacc = b.add_acc(wview, 5121, "VEC4", n, true);
				pr.attrs.push_back({ set == 0 ? "WEIGHTS_0" : "WEIGHTS_1", wacc });
			} else {
				std::vector<float> wv(n * 4, 0.f);
				for (int i = 0; i < n; ++i) {
					for (int k = 0; k < 4; ++k) {
						const int idx = set * 4 + k;
						wv[i * 4 + k] = idx < int(weights[i].size()) ? weights[i][idx] : 0.f;
					}
				}
				const int wview = b.add_view(wv.data(), wv.size() * 4, 16, 34962);
				const int wacc = b.add_acc(wview, 5126, "VEC4", n);
				pr.attrs.push_back({ set == 0 ? "WEIGHTS_0" : "WEIGHTS_1", wacc });
			}
		}
	};
	if (weights0 && joints0) add_skin(pr0, *weights0, *joints0, 4);
	if (weights1 && joints1) add_skin(pr1, *weights1, *joints1, 4);

	/* Faces 0,1 in primitive 0; faces 2,3 in primitive 1. */
	std::vector<unsigned short> idx0 = {
		uint16_t(kFaces[0][0]), uint16_t(kFaces[0][1]), uint16_t(kFaces[0][2]),
		uint16_t(kFaces[1][0]), uint16_t(kFaces[1][1]), uint16_t(kFaces[1][2])
	};
	/* Map welded ids into primitive-1 local ordering B,C,A,D. */
	const int weld_to_local1[4] = { 2, 0, 1, 3 };
	std::vector<unsigned short> idx1;
	for (int f = 2; f < 4; ++f) {
		for (int c = 0; c < 3; ++c) idx1.push_back(uint16_t(weld_to_local1[kFaces[f][c]]));
	}
	pr0.indices = b.add_acc(add_ushort_indices(b, idx0), 5123, "SCALAR", 6);
	pr1.indices = b.add_acc(add_ushort_indices(b, idx1), 5123, "SCALAR", 6);

	m.prims = { pr0, pr1 };
	mesh_idx = b.add_mesh(std::move(m));
}

void identity_worlds(cgltf_size node_count, std::vector<float>& worlds) {
	worlds.assign(node_count * 16, 0.f);
	for (cgltf_size i = 0; i < node_count; ++i) {
		float* m = worlds.data() + 16 * i;
		m[0] = m[5] = m[10] = m[15] = 1.f;
	}
}

/* ------------------------------------------------------------------ */
/* Independent reference deformation                                  */
/* ------------------------------------------------------------------ */

struct RefInfluence {
	int joint;
	float weight;
};

V3 read_v3(const cgltf_accessor* a, cgltf_size i) {
	float f[3] = { 0, 0, 0 };
	cgltf_accessor_read_float(a, i, f, 3);
	return { f[0], f[1], f[2] };
}

std::vector<RefInfluence> ref_influences(const cgltf_primitive& prim, cgltf_size v) {
	struct Pair {
		int joint;
		float weight;
	};
	std::vector<Pair> pairs;
	for (int set = 0; set < 2; ++set) {
		const cgltf_accessor* ja =
				cgltf_find_accessor(&prim, cgltf_attribute_type_joints, set);
		const cgltf_accessor* wa =
				cgltf_find_accessor(&prim, cgltf_attribute_type_weights, set);
		if (!ja || !wa) continue;
		uint32_t j[4];
		float w[4];
		cgltf_accessor_read_uint(ja, v, j, 4);
		cgltf_accessor_read_float(wa, v, w, 4);
		for (int k = 0; k < 4; ++k) {
			if (w[k] == 0.f) continue;
			pairs.push_back({ int(j[k]), w[k] });
		}
	}
	std::vector<RefInfluence> out;
	float sum = 0.f;
	for (const Pair& p : pairs) {
		bool merged = false;
		for (RefInfluence& r : out) {
			if (r.joint == p.joint) {
				r.weight += p.weight;
				merged = true;
				break;
			}
		}
		if (!merged) out.push_back({ p.joint, p.weight });
		sum += p.weight;
	}
	for (RefInfluence& r : out) r.weight /= sum;
	std::sort(out.begin(), out.end(),
			[](const RefInfluence& a, const RefInfluence& b) { return a.joint < b.joint; });
	return out;
}

/* Reference world position of one source vertex of one primitive. */
V3 reference_deform(const cgltf_data* data, const cgltf_node* node,
		const cgltf_primitive& prim, cgltf_size v, const float* worlds,
		const std::vector<float>& weights, const float* node_world) {
	const cgltf_accessor* pos =
			cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
	V3 local = read_v3(pos, v);
	for (cgltf_size t = 0; t < prim.targets_count; ++t) {
		const cgltf_accessor* d = nullptr;
		for (cgltf_size a = 0; a < prim.targets[t].attributes_count; ++a) {
			if (prim.targets[t].attributes[a].type == cgltf_attribute_type_position) {
				d = prim.targets[t].attributes[a].data;
			}
		}
		if (d) {
			const V3 delta = read_v3(d, v);
			local = vadd(local, vscale(delta, t < weights.size() ? weights[t] : 0.f));
		}
	}
	std::vector<RefInfluence> inf = ref_influences(prim, v);
	if (node->skin && !inf.empty()) {
		const cgltf_skin* skin = node->skin;
		V3 acc{ 0, 0, 0 };
		for (const RefInfluence& r : inf) {
			const cgltf_node* jn = skin->joints[r.joint];
			const cgltf_size jidx = cgltf_node_index(data, jn);
			float jm[16];
			std::memcpy(jm, worlds + 16 * jidx, sizeof(jm));
			if (skin->inverse_bind_matrices) {
				float ibm[16];
				cgltf_accessor_read_float(skin->inverse_bind_matrices, r.joint, ibm, 16);
				mat_mul(jm, jm, ibm);
			}
			acc = vadd(acc, vscale(mat_point(jm, local), r.weight));
		}
		return acc;
	}
	return mat_point(node_world, local);
}

/* Distinct reference points across every source vertex of every primitive. */
std::vector<V3> reference_points(const cgltf_data* data, const cgltf_mesh* mesh,
		const cgltf_node* node, const float* worlds,
		const std::vector<float>& weights, const float* node_world) {
	std::vector<V3> all;
	for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
		const cgltf_primitive& prim = mesh->primitives[p];
		const cgltf_accessor* pos =
				cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
		for (cgltf_size v = 0; v < pos->count; ++v) {
			all.push_back(reference_deform(data, node, prim, v, worlds,
					weights, node_world));
		}
	}
	std::vector<V3> uniq;
	for (const V3& p : all) {
		bool found = false;
		for (const V3& q : uniq) {
			if (vdist(p, q) < 1e-6f) {
				found = true;
				break;
			}
		}
		if (!found) uniq.push_back(p);
	}
	return uniq;
}

bool points_match(const std::vector<V3>& ref, const skin_shadow::ShadowVolume& vol) {
	if (ref.size() != vol.finite_count()) return false;
	const float* verts = vol.vertices();
	for (cgltf_size i = 0; i < vol.finite_count(); ++i) {
		const V3 p{ verts[4 * i], verts[4 * i + 1], verts[4 * i + 2] };
		if (!all_finite(p)) return false;
		bool found = false;
		for (const V3& q : ref) {
			if (vdist(p, q) < kEps) {
				found = true;
				break;
			}
		}
		if (!found) return false;
	}
	return true;
}

/* Every undirected output edge must occur once in each direction. */
bool edges_balanced(const skin_shadow::ShadowVolume& vol) {
	struct Dir {
		bool ab = false, ba = false;
	};
	std::unordered_map<uint64_t, Dir> edges;
	const cgltf_uint* idx = vol.indices();
	for (cgltf_size t = 0; t < vol.index_count(); t += 3) {
		const cgltf_uint tri[3] = { idx[t], idx[t + 1], idx[t + 2] };
		for (int c = 0; c < 3; ++c) {
			cgltf_uint u = tri[c], w = tri[(c + 1) % 3];
			cgltf_uint lo = std::min(u, w), hi = std::max(u, w);
			const uint64_t key = (uint64_t(lo) << 32) | hi;
			Dir& d = edges[key];
			if (u < w) {
				if (d.ab) return false;
				d.ab = true;
			} else {
				if (d.ba) return false;
				d.ba = true;
			}
		}
	}
	for (auto& kv : edges) {
		if (!kv.second.ab || !kv.second.ba) return false;
	}
	return true;
}

/* Independent lit source-face set for a single-component fixture. */
std::vector<FaceSource> expected_lit_faces(const cgltf_data* data,
		const cgltf_mesh* mesh, const cgltf_node* node, const float* worlds,
		const std::vector<float>& weights, const float* node_world,
		const V3& light) {
	std::vector<V3> corners;
	struct SrcTri {
		FaceSource src;
		int a, b, c; /* indices into corners */
	};
	std::vector<SrcTri> tris;
	for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
		const cgltf_primitive& prim = mesh->primitives[p];
		const cgltf_accessor* pos =
				cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
		const cgltf_size base = corners.size();
		for (cgltf_size v = 0; v < pos->count; ++v) {
			corners.push_back(reference_deform(data, node, prim, v, worlds,
					weights, node_world));
		}
		const cgltf_size tri_count = prim.indices ? prim.indices->count / 3 : pos->count / 3;
		for (cgltf_size t = 0; t < tri_count; ++t) {
			int local[3];
			for (int c = 0; c < 3; ++c) {
				local[c] = int(prim.indices
						? cgltf_accessor_read_index(prim.indices, t * 3 + c)
						: t * 3 + c);
			}
			tris.push_back({ { p, t }, int(base) + local[0],
					int(base) + local[1], int(base) + local[2] });
		}
	}
	double volume6 = 0.0;
	for (const SrcTri& tr : tris) {
		const V3& a = corners[tr.a];
		const V3& b = corners[tr.b];
		const V3& c = corners[tr.c];
		volume6 += vdot(a, vcross(b, c));
	}
	const bool flip = volume6 < 0.0;
	std::vector<FaceSource> lit;
	for (const SrcTri& tr : tris) {
		const V3& a = corners[tr.a];
		const V3 b = corners[flip ? tr.c : tr.b];
		const V3 c = corners[flip ? tr.b : tr.c];
		const V3 n = vcross(vsub(b, a), vsub(c, a));
		if (vdot(n, vsub(light, a)) > 0.f) lit.push_back(tr.src);
	}
	return lit;
}

bool has_source(const std::vector<FaceSource>& set, cgltf_size p, cgltf_size t) {
	for (const FaceSource& s : set) {
		if (s.primitive == p && s.triangle == t) return true;
	}
	return false;
}

/* One outward-wound tetrahedron primitive, translated by offset. */
void add_tetra_prim(GltfBuilder& b, const V3& offset, int material,
		int& mesh_idx) {
	std::vector<V3> pts(4);
	for (int i = 0; i < 4; ++i) pts[i] = vadd(kTetra[i], offset);
	const int pv = add_position_view(b, pts, 12);
	const int pa = b.add_acc(pv, 5126, "VEC3", 4);
	std::vector<unsigned char> idx;
	for (auto& f : kFaces) idx.push_back((unsigned char)f[0]),
			idx.push_back((unsigned char)f[1]), idx.push_back((unsigned char)f[2]);
	const int iv = b.add_view(idx.data(), idx.size(), 1, 34963);
	const int ia = b.add_acc(iv, 5121, "SCALAR", 12);
	GMesh m;
	GPrim pr;
	pr.attrs = { { "POSITION", pa } };
	pr.indices = ia;
	pr.material = material;
	m.prims = { pr };
	mesh_idx = b.add_mesh(std::move(m));
}

/* Near cap normals point out of, far cap normals point into, each component. */
bool caps_oriented_per_component(const ShadowVolume& vol) {
	const cgltf_size V = vol.finite_count();
	const float* verts = vol.vertices();
	auto vtx = [&](cgltf_size i) -> V3 {
		return { verts[4 * i], verts[4 * i + 1], verts[4 * i + 2] };
	};
	/* Cluster finite vertices into two components by their x coordinate. */
	std::vector<int> cluster(V, -1);
	V3 cen[2] = { { 0, 0, 0 }, { 0, 0, 0 } };
	int cnt[2] = { 0, 0 };
	for (cgltf_size i = 0; i < V; ++i) {
		const V3 p = vtx(i);
		const int c = p.x < 5.f ? 0 : 1;
		cluster[i] = c;
		cen[c] = vadd(cen[c], p);
		++cnt[c];
	}
	if (cnt[0] == 0 || cnt[1] == 0) return false;
	for (int c = 0; c < 2; ++c) cen[c] = vscale(cen[c], 1.f / float(cnt[c]));

	const cgltf_uint* idx = vol.indices();
	for (cgltf_size t = 0; t < vol.index_count(); t += 3) {
		const bool finite_face = idx[t] < V && idx[t + 1] < V && idx[t + 2] < V;
		const bool far_face = idx[t] >= V && idx[t + 1] >= V && idx[t + 2] >= V;
		if (!finite_face && !far_face) continue; /* side wall */
		/* Differences of far points equal differences of finite points, so
		 * the normal can be evaluated on the finite identity vertices. */
		const cgltf_size ia = finite_face ? idx[t] : idx[t] - V;
		const cgltf_size ib = finite_face ? idx[t + 1] : idx[t + 1] - V;
		const cgltf_size ic = finite_face ? idx[t + 2] : idx[t + 2] - V;
		const V3 a = vtx(ia);
		const V3 b = vtx(ib);
		const V3 c = vtx(ic);
		const int comp = cluster[ia];
		const V3 n = vcross(vsub(b, a), vsub(c, a));
		const V3 fcen = vscale(vadd(vadd(a, b), c), 1.f / 3.f);
		const float d = vdot(n, vsub(fcen, cen[comp]));
		if (finite_face) {
			if (!(d > 0.f)) return false; /* near cap outward */
		} else {
			if (!(d < 0.f)) return false; /* far cap reversed */
		}
	}
	return true;
}

/* ================================================================== */
/* topology cases                                                      */
/* ================================================================== */

int case_uv_material_split_closed() {
	GltfBuilder b;
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx);
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "topo_split", fx));

	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	TASK_CHECK(occ.vertex_count() == 4);
	TASK_CHECK(occ.face_count() == 4);
	TASK_CHECK(occ.component_count() == 1);
	TASK_CHECK(occ.face_sources()[0].primitive == 0);

	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	skin_shadow::ShadowVolume vol;
	const V3 light{ 3.f, 3.f, -2.f };
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[0],
			worlds.data(), &light.x, vol, err) == skin_shadow::Status::ok);
	TASK_CHECK(vol.finite_count() == 4);
	TASK_CHECK(vol.far_count() == 4);
	TASK_CHECK(vol.silhouette_edge_count() > 0);
	TASK_CHECK(edges_balanced(vol));
	return 0;
}

int case_static_coincident_different_morph_rejected() {
	GltfBuilder b;
	const std::vector<V3> d0(4, { 0.f, 0.f, 1.f });
	const std::vector<V3> d1(4, { 0.f, 0.f, 2.f }); /* different deformation signature */
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx, &d0, &d1);
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "topo_morphsplit", fx));

	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	const skin_shadow::Status st =
			skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err);
	TASK_CHECK(st == skin_shadow::Status::non_manifold);
	TASK_CHECK(err.first.primitive <= 1);
	TASK_CHECK(err.message != nullptr && err.message[0] != '\0');
	return 0;
}

int case_equivalent_reordered_bone_weights_weld() {
	GltfBuilder b;
	/* Primitive 0: joints (0,1) weights (.5,.5). Primitive 1: joint 1 in
	 * set 0 (.5), joint 0 in set 1 (.5), plus a zero-weight junk slot. */
	std::vector<std::vector<float>> w0(4, { 0.5f, 0.5f, 0.f, 0.f });
	std::vector<std::vector<int>> j0(4, { 0, 1, 0, 0 });
	std::vector<std::vector<float>> w1(4, { 0.5f, 0.f, 0.f, 0.f, 0.5f, 0.f, 0.f, 0.f });
	std::vector<std::vector<int>> j1(4, { 1, 255, 0, 0, 0, 0, 0, 0 });
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx, nullptr, nullptr, &w0, &w1, &j0, &j1);
	GNode node;
	node.mesh = mesh_idx;
	mat_identity(node.matrix);
	node.has_matrix = true;
	const int node_idx = b.add_node(node);
	/* Two joint nodes, then the skin. */
	GNode jn0, jn1;
	mat_identity(jn0.matrix);
	mat_identity(jn1.matrix);
	jn0.has_matrix = jn1.has_matrix = true;
	const int j0_idx = b.add_node(jn0);
	const int j1_idx = b.add_node(jn1);
	GSkin skin;
	skin.joints = { j0_idx, j1_idx };
	const int skin_idx = b.add_skin(skin);
	b.nodes[node_idx].skin = skin_idx;

	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "topo_skinweld", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	TASK_CHECK(occ.vertex_count() == 4);
	TASK_CHECK(occ.face_count() == 4);

	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	skin_shadow::ShadowVolume vol;
	const V3 light{ 2.f, -3.f, 2.f };
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), &light.x, vol, err) == skin_shadow::Status::ok);
	TASK_CHECK(edges_balanced(vol));
	return 0;
}

int case_open_edge_rejected() {
	GltfBuilder b;
	b.material_count = 1;
	/* A single standalone triangle is necessarily open. */
	const std::vector<V3> pts = { kTetra[0], kTetra[1], kTetra[2] };
	const int pv = add_position_view(b, pts, 12);
	const int pa = b.add_acc(pv, 5126, "VEC3", 3);
	GMesh m;
	GPrim pr;
	pr.attrs = { { "POSITION", pa } };
	pr.material = 0;
	m.prims = { pr };
	const int mesh_idx = b.add_mesh(std::move(m));
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "topo_open", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::non_manifold);
	TASK_CHECK(err.status == skin_shadow::Status::non_manifold);
	return 0;
}

int case_non_triangle_rejected() {
	GltfBuilder b;
	b.material_count = 1;
	const std::vector<V3> pts = { kTetra[0], kTetra[1] };
	const int pv = add_position_view(b, pts, 12);
	const int pa = b.add_acc(pv, 5126, "VEC3", 2);
	GMesh m;
	GPrim pr;
	pr.attrs = { { "POSITION", pa } };
	pr.mode = 1; /* LINES */
	pr.material = 0;
	m.prims = { pr };
	const int mesh_idx = b.add_mesh(std::move(m));
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "topo_lines", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::unsupported_geometry);
	return 0;
}

int case_degenerate_face_rejected() {
	GltfBuilder b;
	b.material_count = 1;
	const std::vector<V3> pts = { kTetra[0], kTetra[0], kTetra[1], kTetra[2] };
	const int pv = add_position_view(b, pts, 12);
	const int pa = b.add_acc(pv, 5126, "VEC3", 4);
	const std::vector<unsigned short> idx = { 0, 0, 1, 0, 1, 2 };
	const int iv = add_ushort_indices(b, idx);
	const int ia = b.add_acc(iv, 5123, "SCALAR", 6);
	GMesh m;
	GPrim pr;
	pr.attrs = { { "POSITION", pa } };
	pr.indices = ia;
	pr.material = 0;
	m.prims = { pr };
	const int mesh_idx = b.add_mesh(std::move(m));
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "topo_degenerate", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::degenerate_face);
	TASK_CHECK(err.first.primitive == 0 && err.first.triangle == 0);
	return 0;
}

int case_same_winding_edge_rejected() {
	GltfBuilder b;
	b.material_count = 1;
	/* Two distinct triangles sharing edge (0,1) with the same winding. */
	const std::vector<V3> pts = { kTetra[0], kTetra[1], kTetra[2],
			{ 0.5f, 0.5f, -1.f } };
	const int pv = add_position_view(b, pts, 12);
	const int pa = b.add_acc(pv, 5126, "VEC3", 4);
	const std::vector<unsigned short> idx = { 0, 1, 2, 0, 1, 3 };
	const int iv = add_ushort_indices(b, idx);
	const int ia = b.add_acc(iv, 5123, "SCALAR", 6);
	GMesh m;
	GPrim pr;
	pr.attrs = { { "POSITION", pa } };
	pr.indices = ia;
	pr.material = 0;
	m.prims = { pr };
	const int mesh_idx = b.add_mesh(std::move(m));
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "topo_samewinding", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::non_manifold);
	return 0;
}

int case_non_indexed_welds() {
	GltfBuilder b;
	b.material_count = 1;
	/* 12 non-indexed corners describing the four outward tetra faces. */
	std::vector<V3> pts;
	for (auto& f : kFaces) {
		pts.push_back(kTetra[f[0]]);
		pts.push_back(kTetra[f[1]]);
		pts.push_back(kTetra[f[2]]);
	}
	const int pv = add_position_view(b, pts, 12);
	const int pa = b.add_acc(pv, 5126, "VEC3", 12);
	GMesh m;
	GPrim pr;
	pr.attrs = { { "POSITION", pa } };
	pr.material = 0;
	m.prims = { pr };
	const int mesh_idx = b.add_mesh(std::move(m));
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "topo_nonindexed", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	TASK_CHECK(occ.vertex_count() == 4);
	TASK_CHECK(occ.face_count() == 4);
	return 0;
}

/* ================================================================== */
/* deformation cases                                                   */
/* ================================================================== */

int case_negative_morph_matches_reference() {
	GltfBuilder b;
	const std::vector<V3> delta(4, { 0.f, 0.f, -1.f });
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx, &delta, &delta);
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	const int node_idx = b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "defm_negativemorph", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	TASK_CHECK(occ.target_count() == 1);

	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	const std::vector<float> weights = { -0.7f };
	skin_shadow::ShadowVolume vol;
	const V3 light{ 3.f, 3.f, -2.f };
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), weights.data(), weights.size(), &light.x,
			vol, err) == skin_shadow::Status::ok);
	const float* node_world = worlds.data() + 16 * node_idx;
	std::vector<V3> ref = reference_points(fx.data, &fx.data->meshes[0],
			&fx.data->nodes[node_idx], worlds.data(), weights, node_world);
	TASK_CHECK(points_match(ref, vol));
	return 0;
}

int case_missing_position_target_treated_as_zero() {
	GltfBuilder b;
	b.material_count = 2;
	const int reorder1[4] = { 1, 2, 0, 3 };
	std::vector<V3> p0(4), p1(4);
	for (int i = 0; i < 4; ++i) { p0[i] = kTetra[i]; p1[i] = kTetra[reorder1[i]]; }
	const int pos0a = b.add_acc(add_position_view(b, p0, 12), 5126, "VEC3", 4);
	const int pos1a = b.add_acc(add_position_view(b, p1, 12), 5126, "VEC3", 4);
	/* Primitive 0 target: NORMAL only (POSITION delta is therefore missing). */
	const int nrm_view = add_scaled_normals(b, 4, 0.f, 1.f, 0.f);
	const int nrm_acc = b.add_acc(nrm_view, 5126, "VEC3", 4);
	/* Primitive 1 target: explicit zero POSITION delta. */
	const std::vector<V3> zero_delta(4, { 0.f, 0.f, 0.f });
	const int zero_acc = b.add_acc(add_vec3_view(b, zero_delta), 5126, "VEC3", 4);

	GMesh m;
	GPrim pr0, pr1;
	pr0.attrs = { { "POSITION", pos0a } };
	pr1.attrs = { { "POSITION", pos1a } };
	pr0.material = 0;
	pr1.material = 1;
	pr0.targets.push_back(GTarget{ { { "NORMAL", nrm_acc } } });
	pr1.targets.push_back(GTarget{ { { "POSITION", zero_acc } } });

	std::vector<unsigned short> idx0 = {
		uint16_t(kFaces[0][0]), uint16_t(kFaces[0][1]), uint16_t(kFaces[0][2]),
		uint16_t(kFaces[1][0]), uint16_t(kFaces[1][1]), uint16_t(kFaces[1][2])
	};
	const int weld_to_local1[4] = { 2, 0, 1, 3 };
	std::vector<unsigned short> idx1;
	for (int f = 2; f < 4; ++f)
		for (int c = 0; c < 3; ++c)
			idx1.push_back(uint16_t(weld_to_local1[kFaces[f][c]]));
	pr0.indices = b.add_acc(add_ushort_indices(b, idx0), 5123, "SCALAR", 6);
	pr1.indices = b.add_acc(add_ushort_indices(b, idx1), 5123, "SCALAR", 6);
	m.prims = { pr0, pr1 };
	const int mesh_idx = b.add_mesh(std::move(m));
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	const int node_idx = b.add_node(n);

	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "defm_missingtarget", fx));
	Occluder occ;
	Error err;
	/* Equal (zero) deformation signatures must still weld the seam. */
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== Status::ok);
	TASK_CHECK(occ.vertex_count() == 4);
	TASK_CHECK(occ.target_count() == 1);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	const std::vector<float> weights = { 1.f };
	ShadowVolume vol;
	const V3 light{ 2.f, 2.f, 3.f };
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), weights.data(), weights.size(), &light.x,
			vol, err) == Status::ok);
	std::vector<V3> ref = reference_points(fx.data, &fx.data->meshes[0],
			&fx.data->nodes[node_idx], worlds.data(), weights,
			worlds.data() + 16 * node_idx);
	TASK_CHECK(points_match(ref, vol));
	return 0;
}

int case_non_identity_inverse_bind() {
	GltfBuilder b;
	std::vector<std::vector<float>> w(4, { 0.5f, 0.5f, 0.f, 0.f });
	std::vector<std::vector<int>> j(4, { 0, 1, 0, 0 });
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx, nullptr, nullptr, &w, &w, &j, &j);
	GNode node;
	node.mesh = mesh_idx;
	mat_identity(node.matrix);
	node.has_matrix = true;
	const int node_idx = b.add_node(node);
	GNode jn0, jn1;
	mat_identity(jn0.matrix);
	mat_identity(jn1.matrix);
	jn0.has_matrix = jn1.has_matrix = true;
	const int j0_idx = b.add_node(jn0);
	const int j1_idx = b.add_node(jn1);
	/* IBM0 translates +10 on x, IBM1 translates +20 on x. */
	std::vector<float> ibm(32, 0.f);
	for (int k = 0; k < 2; ++k) {
		float* m = ibm.data() + 16 * k;
		m[0] = m[5] = m[10] = m[15] = 1.f;
		m[12] = k == 0 ? 10.f : 20.f;
	}
	const int ibm_view = b.add_view(ibm.data(), ibm.size() * sizeof(float), 64);
	const int ibm_acc = b.add_acc(ibm_view, 5126, "MAT4", 2);
	GSkin skin;
	skin.joints = { j0_idx, j1_idx };
	skin.ibm = ibm_acc;
	const int skin_idx = b.add_skin(skin);
	b.nodes[node_idx].skin = skin_idx;

	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "defm_ibm", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	skin_shadow::ShadowVolume vol;
	const V3 light{ 2.f, 2.f, 3.f };
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), &light.x, vol, err) == skin_shadow::Status::ok);
	const std::vector<float> no_weights;
	const float* node_world = worlds.data() + 16 * node_idx;
	std::vector<V3> ref = reference_points(fx.data, &fx.data->meshes[0],
			&fx.data->nodes[node_idx], worlds.data(), no_weights, node_world);
	TASK_CHECK(points_match(ref, vol));
	/* Every skinned point must shift by +15 on x. */
	const float* verts = vol.vertices();
	for (cgltf_size i = 0; i < vol.finite_count(); ++i) {
		TASK_CHECK(std::fabs(verts[4 * i] - (kTetra[i % 4].x + 15.f)) < kEps);
	}
	return 0;
}

int case_same_mesh_different_skin() {
	GltfBuilder b;
	std::vector<std::vector<float>> w(4, { 1.f, 0.f, 0.f, 0.f });
	std::vector<std::vector<int>> j(4, { 0, 0, 0, 0 });
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx, nullptr, nullptr, &w, &w, &j, &j);
	/* Two mesh nodes, four joint nodes. */
	GNode na, nb;
	na.mesh = nb.mesh = mesh_idx;
	mat_identity(na.matrix);
	mat_identity(nb.matrix);
	na.has_matrix = nb.has_matrix = true;
	const int na_idx = b.add_node(na);
	const int nb_idx = b.add_node(nb);
	GNode joints[4];
	int joint_idx[4];
	for (int k = 0; k < 4; ++k) {
		mat_identity(joints[k].matrix);
		joints[k].has_matrix = true;
		joint_idx[k] = b.add_node(joints[k]);
	}
	auto make_ibm = [&](float tx) {
		std::vector<float> ibm(16, 0.f);
		ibm[0] = ibm[5] = ibm[10] = ibm[15] = 1.f;
		ibm[12] = tx;
		const int v = b.add_view(ibm.data(), ibm.size() * sizeof(float), 64);
		return b.add_acc(v, 5126, "MAT4", 1);
	};
	GSkin sa, sb;
	sa.joints = { joint_idx[0] };
	sb.joints = { joint_idx[1] };
	sa.ibm = make_ibm(5.f);
	sb.ibm = make_ibm(-7.f);
	const int sa_idx = b.add_skin(sa);
	const int sb_idx = b.add_skin(sb);
	b.nodes[na_idx].skin = sa_idx;
	b.nodes[nb_idx].skin = sb_idx;

	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "defm_twoskin", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	const V3 light{ 2.f, 2.f, 3.f };
	skin_shadow::ShadowVolume va, vb;
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[na_idx],
			worlds.data(), &light.x, va, err) == skin_shadow::Status::ok);
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[nb_idx],
			worlds.data(), &light.x, vb, err) == skin_shadow::Status::ok);
	const std::vector<float> no_weights;
	std::vector<V3> ra = reference_points(fx.data, &fx.data->meshes[0],
			&fx.data->nodes[na_idx], worlds.data(), no_weights,
			worlds.data() + 16 * na_idx);
	std::vector<V3> rb = reference_points(fx.data, &fx.data->meshes[0],
			&fx.data->nodes[nb_idx], worlds.data(), no_weights,
			worlds.data() + 16 * nb_idx);
	TASK_CHECK(points_match(ra, va));
	TASK_CHECK(points_match(rb, vb));
	/* The two instances must not collapse onto the same points. */
	TASK_CHECK(vdist({ va.vertices()[0], va.vertices()[1], va.vertices()[2] },
			{ vb.vertices()[0], vb.vertices()[1], vb.vertices()[2] }) > 1.f);
	return 0;
}

int case_weight_precedence_explicit_zero() {
	GltfBuilder b;
	const std::vector<V3> delta(4, { 0.f, 1.f, 0.f });
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx, &delta, &delta);
	GMesh& m = b.meshes[mesh_idx];
	m.weights = { 1.f }; /* mesh default would apply the full delta */
	GNode n;
	n.mesh = mesh_idx;
	n.weights = { 0.25f }; /* node default overrides mesh default */
	mat_identity(n.matrix);
	n.has_matrix = true;
	const int node_idx = b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "defm_precedence", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	const V3 light{ 2.f, 2.f, 3.f };
	skin_shadow::ShadowVolume vol;

	/* Explicit all-zero override must win and leave the base pose intact. */
	const std::vector<float> zero = { 0.f };
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), zero.data(), zero.size(), &light.x, vol, err)
			== skin_shadow::Status::ok);
	std::vector<V3> base_ref = reference_points(fx.data, &fx.data->meshes[0],
			&fx.data->nodes[node_idx], worlds.data(), zero,
			worlds.data() + 16 * node_idx);
	TASK_CHECK(points_match(base_ref, vol));

	/* No override: node weights (0.25) take over. */
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), &light.x, vol, err) == skin_shadow::Status::ok);
	const std::vector<float> node_w = { 0.25f };
	std::vector<V3> node_ref = reference_points(fx.data, &fx.data->meshes[0],
			&fx.data->nodes[node_idx], worlds.data(), node_w,
			worlds.data() + 16 * node_idx);
	TASK_CHECK(points_match(node_ref, vol));

	/* Wrong override count is rejected, the previous result stays valid. */
	const cgltf_size saved = vol.index_count();
	const std::vector<float> bad = { 0.1f, 0.2f };
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), bad.data(), bad.size(), &light.x, vol, err)
			== skin_shadow::Status::bad_weights);
	TASK_CHECK(vol.index_count() == saved);

	/* Non-finite weight rejected. */
	const std::vector<float> nan_w = { std::nanf("") };
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), nan_w.data(), nan_w.size(), &light.x, vol, err)
			== skin_shadow::Status::bad_weights);
	return 0;
}

int case_sparse_stride_positions() {
	GltfBuilder b;
	b.material_count = 1;
	constexpr cgltf_size stride = 24;
	std::vector<V3> pts(4);
	for (int i = 0; i < 4; ++i) pts[i] = kTetra[i];
	const int base_view = add_position_view(b, pts, stride);
	/* Sparse replacement of vertex 2: move C upward by 0.2 on y. */
	const unsigned char sparse_idx = 2;
	const int si_view = b.add_view(&sparse_idx, 1, 1);
	const V3 replacement = { kTetra[2].x, kTetra[2].y + 0.2f, kTetra[2].z };
	const int sv_view = add_vec3_view(b, { replacement });
	const int pa = b.add_sparse_acc(base_view, 5126, "VEC3", 4,
			si_view, 5121, sv_view, 1);
	/* Build a closed tetra around the replaced point using the same accessor. */
	const std::vector<unsigned short> idx = {
		uint16_t(kFaces[0][0]), uint16_t(kFaces[0][1]), uint16_t(kFaces[0][2]),
		uint16_t(kFaces[1][0]), uint16_t(kFaces[1][1]), uint16_t(kFaces[1][2]),
		uint16_t(kFaces[2][0]), uint16_t(kFaces[2][1]), uint16_t(kFaces[2][2]),
		uint16_t(kFaces[3][0]), uint16_t(kFaces[3][1]), uint16_t(kFaces[3][2])
	};
	const int iv = add_ushort_indices(b, idx);
	const int ia = b.add_acc(iv, 5123, "SCALAR", 12);
	GMesh m;
	GPrim pr;
	pr.attrs = { { "POSITION", pa } };
	pr.indices = ia;
	pr.material = 0;
	m.prims = { pr };
	const int mesh_idx = b.add_mesh(std::move(m));
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	const int node_idx = b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "defm_sparse", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	skin_shadow::ShadowVolume vol;
	const V3 light{ 2.f, 2.f, 3.f };
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), &light.x, vol, err) == skin_shadow::Status::ok);
	const std::vector<float> no_weights;
	std::vector<V3> ref = reference_points(fx.data, &fx.data->meshes[0],
			&fx.data->nodes[node_idx], worlds.data(), no_weights,
			worlds.data() + 16 * node_idx);
	TASK_CHECK(points_match(ref, vol));
	TASK_CHECK(edges_balanced(vol));
	return 0;
}

int case_normalized_u8_weights() {
	GltfBuilder b;
	std::vector<std::vector<float>> w(4, { 0.5f, 0.5f, 0.f, 0.f });
	std::vector<std::vector<int>> j(4, { 0, 1, 0, 0 });
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx, nullptr, nullptr, &w, &w, &j, &j, true);
	GNode node;
	node.mesh = mesh_idx;
	mat_identity(node.matrix);
	node.has_matrix = true;
	const int node_idx = b.add_node(node);
	GNode jn0, jn1;
	mat_identity(jn0.matrix);
	mat_identity(jn1.matrix);
	jn0.has_matrix = jn1.has_matrix = true;
	const int j0_idx = b.add_node(jn0);
	const int j1_idx = b.add_node(jn1);
	GSkin skin;
	skin.joints = { j0_idx, j1_idx }; /* no IBM => identity */
	const int skin_idx = b.add_skin(skin);
	b.nodes[node_idx].skin = skin_idx;
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "defm_normu8", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	skin_shadow::ShadowVolume vol;
	const V3 light{ 2.f, 2.f, 3.f };
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), &light.x, vol, err) == skin_shadow::Status::ok);
	const std::vector<float> no_weights;
	std::vector<V3> ref = reference_points(fx.data, &fx.data->meshes[0],
			&fx.data->nodes[node_idx], worlds.data(), no_weights,
			worlds.data() + 16 * node_idx);
	TASK_CHECK(points_match(ref, vol));
	return 0;
}

int case_failure_then_retry() {
	GltfBuilder b;
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx);
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	const int node_idx = b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "defm_retry", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	const V3 light{ 2.f, 2.f, 3.f };
	skin_shadow::ShadowVolume vol;
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), &light.x, vol, err) == skin_shadow::Status::ok);
	const cgltf_size saved_indices = vol.index_count();

	std::vector<float> bad = worlds;
	bad[5] = std::nanf("");
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			bad.data(), &light.x, vol, err) == skin_shadow::Status::bad_matrix);
	/* Previously handed-out result must remain valid and untouched. */
	TASK_CHECK(vol.index_count() == saved_indices);
	TASK_CHECK(std::isfinite(vol.vertices()[0]));

	/* A subsequent legal generation still succeeds. */
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), &light.x, vol, err) == skin_shadow::Status::ok);
	TASK_CHECK(vol.index_count() == saved_indices);
	TASK_CHECK(edges_balanced(vol));
	return 0;
}

/* ================================================================== */
/* volume cases                                                        */
/* ================================================================== */

int case_reflection_reorients_outward() {
	GltfBuilder b;
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx);
	GNode n;
	n.mesh = mesh_idx;
	/* Reflect z; determinant negative. */
	std::memset(n.matrix, 0, sizeof(n.matrix));
	n.matrix[0] = 1.f;
	n.matrix[5] = 1.f;
	n.matrix[10] = -1.f;
	n.matrix[15] = 1.f;
	n.has_matrix = true;
	const int node_idx = b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "vol_reflect", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	/* Override the node's own world matrix with the reflection. */
	std::memcpy(worlds.data() + 16 * node_idx, n.matrix, sizeof(n.matrix));
	const V3 light{ 2.f, 2.f, 3.f };
	skin_shadow::ShadowVolume vol;
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), &light.x, vol, err) == skin_shadow::Status::ok);

	const std::vector<float> no_weights;
	std::vector<FaceSource> expect = expected_lit_faces(fx.data,
			&fx.data->meshes[0], &fx.data->nodes[node_idx], worlds.data(),
			no_weights, worlds.data() + 16 * node_idx, light);
	/* Near cap triangles are exactly the expected lit source faces. */
	cgltf_size near_count = 0;
	const cgltf_uint* idx = vol.indices();
	for (cgltf_size t = 0; t < vol.index_count(); t += 3) {
		if (idx[t] < vol.finite_count() && idx[t + 1] < vol.finite_count() &&
				idx[t + 2] < vol.finite_count()) {
			const FaceSource& s = vol.triangle_sources()[t / 3];
			TASK_CHECK(has_source(expect, s.primitive, s.triangle));
			++near_count;
		}
	}
	TASK_CHECK(near_count == expect.size());
	TASK_CHECK(edges_balanced(vol));
	return 0;
}

int case_silhouette_tracks_light() {
	GltfBuilder b;
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx);
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	const int node_idx = b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "vol_silhouette", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	const std::vector<float> no_weights;
	const float* node_world = worlds.data() + 16 * node_idx;

	const V3 lights[2] = { { 3.f, 3.f, -2.f }, { -3.f, -1.f, 2.f } };
	skin_shadow::ShadowVolume vols[2];
	cgltf_size sil_counts[2];
	for (int k = 0; k < 2; ++k) {
		TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
				worlds.data(), &lights[k].x, vols[k], err) == skin_shadow::Status::ok);
		sil_counts[k] = vols[k].silhouette_edge_count();
		TASK_CHECK(sil_counts[k] > 0);
		/* Every silhouette edge belongs to a currently lit face. */
		std::vector<FaceSource> lit = expected_lit_faces(fx.data,
				&fx.data->meshes[0], &fx.data->nodes[node_idx], worlds.data(),
				no_weights, node_world, lights[k]);
		for (cgltf_size e = 0; e < sil_counts[k]; ++e) {
			const EdgeSource& es = vols[k].silhouette_edges()[e];
			TASK_CHECK(has_source(lit, es.primitive, es.triangle));
			TASK_CHECK(es.corner >= 0 && es.corner < 3);
		}
		/* Side walls account for two triangles per silhouette edge. */
		const cgltf_size near_far = vols[k].index_count() / 3 - 2 * sil_counts[k];
		TASK_CHECK(near_far % 2 == 0);
		TASK_CHECK(near_far / 2 == lit.size());
		TASK_CHECK(edges_balanced(vols[k]));
	}
	/* Moving the light to the other side changes the silhouette set. */
	std::vector<FaceSource> s0, s1;
	for (cgltf_size e = 0; e < sil_counts[0]; ++e) {
		const EdgeSource& a = vols[0].silhouette_edges()[e];
		s0.push_back({ a.primitive, a.triangle });
	}
	for (cgltf_size e = 0; e < sil_counts[1]; ++e) {
		const EdgeSource& a = vols[1].silhouette_edges()[e];
		s1.push_back({ a.primitive, a.triangle });
	}
	bool identical = s0.size() == s1.size();
	if (identical) {
		for (size_t i = 0; i < s0.size(); ++i) {
			if (!has_source(s1, s0[i].primitive, s0[i].triangle)) identical = false;
		}
	}
	TASK_CHECK(!identical);
	return 0;
}

int case_far_vertex_identity_and_w() {
	GltfBuilder b;
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx);
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	const int node_idx = b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "vol_faridentity", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	const V3 light{ 1.5f, 0.7f, -2.5f };
	skin_shadow::ShadowVolume vol;
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), &light.x, vol, err) == skin_shadow::Status::ok);
	const cgltf_size V = vol.finite_count();
	TASK_CHECK(V == vol.far_count());
	const float* verts = vol.vertices();
	for (cgltf_size i = 0; i < V; ++i) {
		const float* fin = verts + 4 * i;
		const float* far = verts + 4 * (V + i);
		TASK_CHECK(fin[3] == 1.f);
		TASK_CHECK(far[3] == 0.f);
		TASK_CHECK(std::fabs(far[0] - (fin[0] - light.x)) < kEps);
		TASK_CHECK(std::fabs(far[1] - (fin[1] - light.y)) < kEps);
		TASK_CHECK(std::fabs(far[2] - (fin[2] - light.z)) < kEps);
	}
	/* Indices stay inside the finite/far identity ranges. */
	for (cgltf_size i = 0; i < vol.index_count(); ++i) {
		TASK_CHECK(vol.indices()[i] < 2 * V);
	}
	TASK_CHECK(edges_balanced(vol));
	return 0;
}

int case_zero_volume_failure_and_retry() {
	GltfBuilder b;
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx);
	GNode n;
	n.mesh = mesh_idx;
	/* Collapse z onto a plane: zero signed volume. */
	std::memset(n.matrix, 0, sizeof(n.matrix));
	n.matrix[0] = 1.f;
	n.matrix[5] = 1.f;
	n.matrix[10] = 0.f;
	n.matrix[15] = 1.f;
	n.has_matrix = true;
	const int node_idx = b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "vol_zerovol", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	std::memcpy(worlds.data() + 16 * node_idx, n.matrix, sizeof(n.matrix));
	const V3 light{ 2.f, 2.f, 3.f };
	skin_shadow::ShadowVolume vol;
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), &light.x, vol, err) == skin_shadow::Status::degenerate_frame);

	/* Restore a non-degenerate pose and generation must succeed again. */
	float fixed[16];
	mat_identity(fixed);
	std::memcpy(worlds.data() + 16 * node_idx, fixed, sizeof(fixed));
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), &light.x, vol, err) == skin_shadow::Status::ok);
	TASK_CHECK(vol.finite_count() == 4);
	TASK_CHECK(edges_balanced(vol));
	return 0;
}

int case_shear_non_uniform_scale() {
	GltfBuilder b;
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx);
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	/* x' = x + 0.6 y, y' = 2 y, z' = 3 z (shear + non-uniform scale). */
	n.matrix[0] = 1.f;
	n.matrix[4] = 0.6f;
	n.matrix[5] = 2.f;
	n.matrix[10] = 3.f;
	n.has_matrix = true;
	const int node_idx = b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "vol_shear", fx));
	skin_shadow::Occluder occ;
	skin_shadow::Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== skin_shadow::Status::ok);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	std::memcpy(worlds.data() + 16 * node_idx, n.matrix, sizeof(n.matrix));
	const V3 light{ 2.f, 2.f, 3.f };
	skin_shadow::ShadowVolume vol;
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), &light.x, vol, err) == skin_shadow::Status::ok);
	const std::vector<float> no_weights;
	std::vector<V3> ref = reference_points(fx.data, &fx.data->meshes[0],
			&fx.data->nodes[node_idx], worlds.data(), no_weights,
			worlds.data() + 16 * node_idx);
	TASK_CHECK(points_match(ref, vol));
	TASK_CHECK(edges_balanced(vol));
	return 0;
}

int case_two_components_independent_orientation() {
	GltfBuilder b;
	b.material_count = 2;
	GMesh m;
	for (int part = 0; part < 2; ++part) {
		const V3 off = part == 0 ? V3{ 0, 0, 0 } : V3{ 10.f, 0.f, 0.f };
		std::vector<V3> pts(4);
		for (int i = 0; i < 4; ++i) pts[i] = vadd(kTetra[i], off);
		const int pv = add_position_view(b, pts, 12);
		const int pa = b.add_acc(pv, 5126, "VEC3", 4);
		std::vector<unsigned char> idx;
		for (auto& f : kFaces) idx.push_back((unsigned char)f[0]),
				idx.push_back((unsigned char)f[1]),
				idx.push_back((unsigned char)f[2]);
		const int iv = b.add_view(idx.data(), idx.size(), 1, 34963);
		const int ia = b.add_acc(iv, 5121, "SCALAR", 12);
		GPrim pr;
		pr.attrs = { { "POSITION", pa } };
		pr.indices = ia;
		pr.material = part;
		m.prims.push_back(pr);
	}
	const int mesh_idx = b.add_mesh(std::move(m));
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	const int node_idx = b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "vol_twocomp", fx));
	Occluder occ;
	Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== Status::ok);
	TASK_CHECK(occ.vertex_count() == 8);
	TASK_CHECK(occ.face_count() == 8);
	TASK_CHECK(occ.component_count() == 2);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	const V3 light{ 5.f, 2.f, 4.f };
	ShadowVolume vol;
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), &light.x, vol, err) == Status::ok);
	TASK_CHECK(edges_balanced(vol));
	TASK_CHECK(caps_oriented_per_component(vol));
	return 0;
}

int case_u8_indices_closed() {
	GltfBuilder b;
	int mesh_idx = 0;
	add_tetra_prim(b, V3{ 0, 0, 0 }, -1, mesh_idx);
	b.material_count = 0;
	GNode n;
	n.mesh = mesh_idx;
	mat_identity(n.matrix);
	n.has_matrix = true;
	b.add_node(n);
	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "vol_u8", fx));
	Occluder occ;
	Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== Status::ok);
	TASK_CHECK(occ.vertex_count() == 4);
	TASK_CHECK(occ.face_count() == 4);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	ShadowVolume vol;
	const V3 light{ 2.f, 2.f, 3.f };
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[0],
			worlds.data(), &light.x, vol, err) == Status::ok);
	TASK_CHECK(edges_balanced(vol));
	return 0;
}

int case_example_demo_generates_two_volumes() {
	/* Exercises the example's own no-main entry point end to end. */
	skin_shadow::ShadowVolume first, second;
	skin_shadow_demo::DemoReport report;
	TASK_CHECK(skin_shadow_demo::run(first, second, report));
	TASK_CHECK(report.ok);
	TASK_CHECK(report.welded_vertices == 4);
	TASK_CHECK(report.welded_faces == 4);
	TASK_CHECK(report.components == 1);
	TASK_CHECK(report.source_vertices == 8);
	TASK_CHECK(first.finite_count() == second.finite_count());
	TASK_CHECK(first.index_count() == second.index_count());
	TASK_CHECK(first.index_count() > 0);
	TASK_CHECK(edges_balanced(first));
	TASK_CHECK(edges_balanced(second));
	return 0;
}

int case_positive_weight_out_of_range_joint() {
	GltfBuilder b;
	std::vector<std::vector<float>> w(4, { 1.f, 0.f, 0.f, 0.f });
	/* Positive weight references joint 5, but the skin only has one joint. */
	std::vector<std::vector<int>> j(4, { 5, 0, 0, 0 });
	int mesh_idx = 0;
	build_two_material_tetra(b, mesh_idx, nullptr, nullptr, &w, &w, &j, &j);
	GNode node;
	node.mesh = mesh_idx;
	mat_identity(node.matrix);
	node.has_matrix = true;
	const int node_idx = b.add_node(node);
	GNode jn;
	mat_identity(jn.matrix);
	jn.has_matrix = true;
	const int j_idx = b.add_node(jn);
	GSkin skin;
	skin.joints = { j_idx };
	const int skin_idx = b.add_skin(skin);
	b.nodes[node_idx].skin = skin_idx;

	ParsedFixture fx;
	TASK_CHECK(parse_fixture(b, "vol_badjoint", fx));
	Occluder occ;
	Error err;
	TASK_CHECK(skin_shadow::build(fx.data, &fx.data->meshes[0], occ, err)
			== Status::ok);
	std::vector<float> worlds;
	identity_worlds(fx.data->nodes_count, worlds);
	ShadowVolume vol;
	const V3 light{ 2.f, 2.f, 3.f };
	TASK_CHECK(skin_shadow::generate(occ, fx.data, &fx.data->nodes[node_idx],
			worlds.data(), &light.x, vol, err) == Status::bad_matrix);
	TASK_CHECK(err.message != nullptr && err.message[0] != '\0');
	return 0;
}

/* ================================================================== */
/* Registration                                                        */
/* ================================================================== */

} /* anonymous namespace */

extern "C" const task_case task_cases[] = {
	{ "topology", "uv_material_split_still_closed", case_uv_material_split_closed },
	{ "topology", "static_coincident_different_morph_rejected",
			case_static_coincident_different_morph_rejected },
	{ "topology", "equivalent_reordered_bone_weights_weld",
			case_equivalent_reordered_bone_weights_weld },
	{ "topology", "open_edge_rejected", case_open_edge_rejected },
	{ "topology", "non_triangle_primitive_rejected", case_non_triangle_rejected },
	{ "topology", "degenerate_face_rejected", case_degenerate_face_rejected },
	{ "topology", "same_winding_edge_rejected", case_same_winding_edge_rejected },
	{ "topology", "non_indexed_welds", case_non_indexed_welds },

	{ "deformation", "negative_morph_matches_reference",
			case_negative_morph_matches_reference },
	{ "deformation", "missing_position_target_treated_as_zero",
			case_missing_position_target_treated_as_zero },
	{ "deformation", "non_identity_inverse_bind", case_non_identity_inverse_bind },
	{ "deformation", "same_mesh_different_skin", case_same_mesh_different_skin },
	{ "deformation", "weight_precedence_explicit_zero",
			case_weight_precedence_explicit_zero },
	{ "deformation", "sparse_stride_positions", case_sparse_stride_positions },
	{ "deformation", "normalized_u8_weights", case_normalized_u8_weights },
	{ "deformation", "failure_then_retry", case_failure_then_retry },
	{ "deformation", "positive_weight_out_of_range_joint",
			case_positive_weight_out_of_range_joint },

	{ "volume", "reflection_reorients_outward", case_reflection_reorients_outward },
	{ "volume", "silhouette_tracks_light", case_silhouette_tracks_light },
	{ "volume", "far_vertex_identity_and_w", case_far_vertex_identity_and_w },
	{ "volume", "zero_volume_failure_and_retry", case_zero_volume_failure_and_retry },
	{ "volume", "shear_non_uniform_scale", case_shear_non_uniform_scale },
	{ "volume", "two_components_independent_orientation",
			case_two_components_independent_orientation },
	{ "volume", "u8_indices_closed", case_u8_indices_closed },
	{ "volume", "example_demo_generates_two_volumes",
			case_example_demo_generates_two_volumes }
};

extern "C" const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
