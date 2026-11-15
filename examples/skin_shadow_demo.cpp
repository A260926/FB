/*
 * skin_shadow_demo.cpp - demonstration harness (no main()).
 *
 * Builds a closed tetrahedron split into two TRIANGLES primitives with two
 * materials. Vertices on the seam are duplicated with different normals
 * and UVs in each primitive, exactly the situation that makes raw index
 * shadow volumes leak. The mesh is serialized to a real .gltf/.bin pair,
 * then parsed, buffer-loaded and validated through cgltf before the
 * skin_shadow API rebuilds the closed occluder and emits two posed volumes.
 */
#include "skin_shadow_demo.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "cgltf.h"

namespace skin_shadow_demo {

namespace {

constexpr cgltf_size kPrimVerts = 4;
constexpr cgltf_size kBinSize = 280;

/* Offsets inside the single packed binary buffer. */
constexpr cgltf_size kOffPos0 = 0;
constexpr cgltf_size kOffPos1 = 48;
constexpr cgltf_size kOffNrm0 = 96;
constexpr cgltf_size kOffNrm1 = 144;
constexpr cgltf_size kOffUv0 = 192;
constexpr cgltf_size kOffUv1 = 224;
constexpr cgltf_size kOffIdx0 = 256;
constexpr cgltf_size kOffIdx1 = 268;

void put_vec3(float* dst, const float v[3]) {
	dst[0] = v[0];
	dst[1] = v[1];
	dst[2] = v[2];
}

void put_vec2(float* dst, float u, float v) {
	dst[0] = u;
	dst[1] = v;
}

void put_shorts(unsigned char* dst, const unsigned short idx[6]) {
	std::memcpy(dst, idx, 6 * sizeof(unsigned short));
}

std::string build_json() {
	return std::string(R"({
  "asset": {"version": "2.0"},
  "buffers": [{"uri": "skin_shadow_demo.bin", "byteLength": )") +
		std::to_string(kBinSize) + R"(}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": )" + std::to_string(kOffPos0) +
		R"(, "byteLength": 48},
    {"buffer": 0, "byteOffset": )" + std::to_string(kOffPos1) +
		R"(, "byteLength": 48},
    {"buffer": 0, "byteOffset": )" + std::to_string(kOffNrm0) +
		R"(, "byteLength": 48},
    {"buffer": 0, "byteOffset": )" + std::to_string(kOffNrm1) +
		R"(, "byteLength": 48},
    {"buffer": 0, "byteOffset": )" + std::to_string(kOffUv0) +
		R"(, "byteLength": 32},
    {"buffer": 0, "byteOffset": )" + std::to_string(kOffUv1) +
		R"(, "byteLength": 32},
    {"buffer": 0, "byteOffset": )" + std::to_string(kOffIdx0) +
		R"(, "byteLength": 12, "target": 34963},
    {"buffer": 0, "byteOffset": )" + std::to_string(kOffIdx1) +
		R"(, "byteLength": 12, "target": 34963}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC3"},
    {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC3"},
    {"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC2"},
    {"bufferView": 5, "componentType": 5126, "count": 4, "type": "VEC2"},
    {"bufferView": 6, "componentType": 5123, "count": 6, "type": "SCALAR"},
    {"bufferView": 7, "componentType": 5123, "count": 6, "type": "SCALAR"}
  ],
  "materials": [{"name": "seam_a"}, {"name": "seam_b"}],
  "meshes": [{
    "name": "two_material_tetrahedron",
    "primitives": [
      {
        "attributes": {"POSITION": 0, "NORMAL": 2, "TEXCOORD_0": 4},
        "indices": 6, "material": 0, "mode": 4
      },
      {
        "attributes": {"POSITION": 1, "NORMAL": 3, "TEXCOORD_0": 5},
        "indices": 7, "material": 1, "mode": 4
      }
    ]
  }],
  "nodes": [{"mesh": 0}],
  "scenes": [{"nodes": [0]}],
  "scene": 0
})";
}

void write_bin(const std::string& path) {
	unsigned char bin[kBinSize];
	std::memset(bin, 0, sizeof(bin));

	/* Shared tetrahedron positions: A, B, C, D. */
	const float A[3] = { 0.f, 0.f, 0.f };
	const float B[3] = { 1.f, 0.f, 0.f };
	const float C[3] = { 0.5f, 1.f, 0.f };
	const float D[3] = { 0.5f, 0.4f, 1.f };
	float* pos0 = reinterpret_cast<float*>(bin + kOffPos0);
	put_vec3(pos0 + 0, A);
	put_vec3(pos0 + 3, B);
	put_vec3(pos0 + 6, C);
	put_vec3(pos0 + 9, D);
	/* Primitive 1 reorders the same points: B, C, A, D. */
	float* pos1 = reinterpret_cast<float*>(bin + kOffPos1);
	put_vec3(pos1 + 0, B);
	put_vec3(pos1 + 3, C);
	put_vec3(pos1 + 6, A);
	put_vec3(pos1 + 9, D);

	/* Different hard normals per primitive make the seam explicit. */
	const float n0[3] = { 0.f, 0.f, 1.f };
	const float n1[3] = { 1.f, 0.f, 0.f };
	float* nrm0 = reinterpret_cast<float*>(bin + kOffNrm0);
	float* nrm1p = reinterpret_cast<float*>(bin + kOffNrm1);
	for (cgltf_size i = 0; i < kPrimVerts; ++i) {
		put_vec3(nrm0 + i * 3, n0);
		put_vec3(nrm1p + i * 3, n1);
	}

	/* Different UV islands per primitive. */
	float* uv0 = reinterpret_cast<float*>(bin + kOffUv0);
	float* uv1 = reinterpret_cast<float*>(bin + kOffUv1);
	for (int i = 0; i < (int)kPrimVerts; ++i) {
		put_vec2(uv0 + i * 2, float(i) * 0.25f, 0.f);
		put_vec2(uv1 + i * 2, float(i) * 0.25f + 0.5f, 1.f);
	}

	/*
	 * Outward-wound tetrahedron (A,B,C base in z=0, apex D above). The four
	 * faces are (A,C,B), (A,B,D), (A,D,C), (B,C,D); every shared edge is
	 * traversed in opposite directions. Split across two materials:
	 * Primitive 0 (pos0 A,B,C,D): (A,C,B) and (A,B,D).
	 */
	const unsigned short idx0[6] = { 0, 2, 1, 0, 1, 3 };
	/* Primitive 1 ordering B,C,A,D: (A,D,C) = 2,3,1 and (B,C,D) = 0,1,3. */
	const unsigned short idx1[6] = { 2, 3, 1, 0, 1, 3 };
	put_shorts(bin + kOffIdx0, idx0);
	put_shorts(bin + kOffIdx1, idx1);

	std::ofstream f(path, std::ios::binary);
	f.write(reinterpret_cast<const char*>(bin), sizeof(bin));
}

std::string temp_dir() {
	const char* env = std::getenv("TMPDIR");
	std::string dir = env ? env : "/tmp/";
	if (!dir.empty() && dir.back() != '/') dir.push_back('/');
	return dir;
}

} /* anonymous namespace */

bool run(skin_shadow::ShadowVolume& first,
		skin_shadow::ShadowVolume& second, DemoReport& report) {
	first = skin_shadow::ShadowVolume();
	second = skin_shadow::ShadowVolume();
	report = DemoReport{};

	const std::string dir = temp_dir();
	const std::string gltf_path = dir + "skin_shadow_demo.gltf";
	const std::string bin_path = dir + "skin_shadow_demo.bin";

	{
		std::ofstream jf(gltf_path, std::ios::binary);
		jf << build_json();
	}
	write_bin(bin_path);

	cgltf_options opts{};
	cgltf_data* data = nullptr;
	if (cgltf_parse_file(&opts, gltf_path.c_str(), &data) != cgltf_result_success) {
		return false;
	}
	bool ok = false;
	do {
		if (cgltf_load_buffers(&opts, data, gltf_path.c_str()) != cgltf_result_success) {
			break;
		}
		if (cgltf_validate(data) != cgltf_result_success) {
			break;
		}

		skin_shadow::Occluder occluder;
		skin_shadow::Error error;
		if (skin_shadow::build(data, &data->meshes[0], occluder, error)
				!= skin_shadow::Status::ok) {
			break;
		}

		/* Eight source corner vertices must weld back to four points. */
		if (occluder.vertex_count() != 4 || occluder.face_count() != 4 ||
				occluder.component_count() != 1) {
			break;
		}
		report.source_vertices = 2 * kPrimVerts;
		report.welded_vertices = occluder.vertex_count();
		report.welded_faces = occluder.face_count();
		report.components = occluder.component_count();

		float identity[16] = {
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			0, 0, 0, 1
		};
		/* Second pose: translate the tetrahedron up by four units. */
		float translated[16] = {
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			0, 0, 4, 1
		};

		const float light[3] = { 3.f, 3.f, -2.f };
		if (skin_shadow::generate(occluder, data, &data->nodes[0], identity,
				light, first, error) != skin_shadow::Status::ok) {
			break;
		}
		if (skin_shadow::generate(occluder, data, &data->nodes[0], translated,
				light, second, error) != skin_shadow::Status::ok) {
			break;
		}
		if (first.finite_count() != 4 || second.finite_count() != 4 ||
				first.index_count() == 0 ||
				first.index_count() != second.index_count()) {
			break;
		}
		ok = true;
	} while (false);

	cgltf_free(data);
	std::remove(gltf_path.c_str());
	std::remove(bin_path.c_str());

	report.ok = ok;
	if (!ok) {
		first = skin_shadow::ShadowVolume();
		second = skin_shadow::ShadowVolume();
	}
	return ok;
}

} /* namespace skin_shadow_demo */
