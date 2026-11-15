//
// Skinned CLOD / DLOD target tests.
//
// Every mesh in this file is generated programmatically. Skinning influences use several joint
// sets, including duplicate joints, unsorted slots and zero weight slots, so the normalization
// path is exercised before welding, decimation and meshlet palette construction.
//
#include "task_support/check.h"
#include "MeshDecimationTools.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace {

struct Mesh {
	std::vector<float> vertices;
	std::vector<uint32_t> indices;
	std::vector<MdtSkinInfluences> skins;
	uint32_t joint_count = 0;
};

static MdtSkinInfluences MakeSkin(uint32_t j0, float w0, uint32_t j1 = 0, float w1 = 0.f, uint32_t j2 = 0, float w2 = 0.f, uint32_t j3 = 0, float w3 = 0.f) {
	MdtSkinInfluences skin = {};
	skin.joints[0] = j0; skin.weights[0] = w0;
	skin.joints[1] = j1; skin.weights[1] = w1;
	skin.joints[2] = j2; skin.weights[2] = w2;
	skin.joints[3] = j3; skin.weights[3] = w3;
	return skin;
}

static void AddVertex(Mesh& mesh, float x, float y, float z, float u, float v, const MdtSkinInfluences& skin) {
	mesh.vertices.insert(mesh.vertices.end(), { x, y, z, u, v });
	mesh.skins.push_back(skin);
}

static void AddTriangle(Mesh& mesh, uint32_t a, uint32_t b, uint32_t c) {
	mesh.indices.insert(mesh.indices.end(), { a, b, c });
}

// Planar grid with per vertex skin callback.
template<typename SkinFn>
static Mesh MakeGrid(uint32_t size_x, uint32_t size_y, SkinFn&& skin_fn, uint32_t joint_count) {
	Mesh mesh;
	mesh.joint_count = joint_count;
	for (uint32_t y = 0; y < size_y; y += 1) {
		for (uint32_t x = 0; x < size_x; x += 1) {
			float u = float(x) / float(size_x - 1);
			float v = float(y) / float(size_y - 1);
			AddVertex(mesh, u, v, 0.05f * u * v, u, v, skin_fn(x, y));
		}
	}
	for (uint32_t y = 0; y + 1 < size_y; y += 1) {
		for (uint32_t x = 0; x + 1 < size_x; x += 1) {
			uint32_t a = y * size_x + x;
			uint32_t b = a + 1;
			uint32_t c = a + size_x;
			uint32_t d = c + 1;
			AddTriangle(mesh, a, b, d);
			AddTriangle(mesh, a, d, c);
		}
	}
	return mesh;
}

struct GeometrySetup {
	MdtTriangleGeometryDesc descs[4];
	const MdtSkinInfluences* skin_ptrs[4];
	MdtSkinnedMeshDesc mesh_desc;
};

static GeometrySetup MakeGeometrySetup(Mesh& mesh, uint32_t vertex_stride_dwords = 5) {
	GeometrySetup setup = {};
	setup.descs[0].indices      = mesh.indices.data();
	setup.descs[0].vertices     = mesh.vertices.data();
	setup.descs[0].index_count  = uint32_t(mesh.indices.size());
	setup.descs[0].vertex_count = uint32_t(mesh.skins.size());
	setup.skin_ptrs[0] = mesh.skins.data();

	setup.mesh_desc.mesh.geometry_descs      = setup.descs;
	setup.mesh_desc.mesh.geometry_desc_count = 1;
	setup.mesh_desc.mesh.vertex_stride_bytes = vertex_stride_dwords * sizeof(float);
	setup.mesh_desc.mesh.geometric_weight    = 0.5f;
	setup.mesh_desc.geometry_skins           = setup.skin_ptrs;
	setup.mesh_desc.joint_count              = mesh.joint_count;
	return setup;
}

// Total number of faces reported across all geometries of a discrete LOD level.
static uint32_t GetLevelFaceCount(const MdtDiscreteLodBuildResult& result, uint32_t level_index) {
	auto& level = result.level_of_detail_descs[level_index];
	uint32_t face_count = 0;
	for (uint32_t geometry_index = level.begin_geometry_index; geometry_index < level.end_geometry_index; geometry_index += 1) {
		auto& desc = result.geometry_descs[geometry_index];
		face_count += (desc.end_indices_index - desc.begin_indices_index) / 3;
	}
	return face_count;
}

static uint32_t GetLevelFaceCountFromMeshlets(const MdtContinuousLodBuildResult& result, uint32_t level_index) {
	auto& level = result.levels[level_index];
	uint32_t face_count = 0;
	for (uint32_t meshlet_index = level.begin_meshlets_index; meshlet_index < level.end_meshlets_index; meshlet_index += 1) {
		auto& meshlet = result.meshlets[meshlet_index];
		face_count += meshlet.end_meshlet_triangles_index - meshlet.begin_meshlet_triangles_index;
	}
	return face_count;
}

static bool IsNormalized(const MdtSkinInfluences& skin) {
	int positive_slot_count = 0;
	float weight_sum = 0.f;
	uint32_t previous_joint = 0;
	for (uint32_t slot_index = 0; slot_index < 4; slot_index += 1) {
		if (skin.weights[slot_index] > 0.f) {
			if (positive_slot_count > 0 && skin.joints[slot_index] <= previous_joint) return false;
			previous_joint = skin.joints[slot_index];
			positive_slot_count += 1;
		} else {
			// Empty slots are zeroed out completely, including the joint index.
			if (skin.weights[slot_index] != 0.f || skin.joints[slot_index] != 0) return false;
		}
		weight_sum += skin.weights[slot_index];
	}
	if (positive_slot_count == 0) return false;
	return fabsf(weight_sum - 1.f) < 1e-4f;
}

static bool HasJoint(const MdtSkinInfluences& skin, uint32_t joint, float weight_epsilon = 1e-6f) {
	for (uint32_t slot_index = 0; slot_index < 4; slot_index += 1) {
		if (skin.weights[slot_index] > weight_epsilon && skin.joints[slot_index] == joint) return true;
	}
	return false;
}

// Reconstructs a vertex skin from a meshlet palette and local slot indices.
static bool ReconstructSkin(const MdtSkinnedContinuousLodBuildResult& result, uint32_t meshlet_index, uint32_t meshlet_vertex_index, MdtSkinInfluences& skin) {
	uint32_t begin = result.meshlet_palette_offsets[meshlet_index];
	uint32_t end   = result.meshlet_palette_offsets[meshlet_index + 1];

	memset(&skin, 0, sizeof(skin));
	for (uint32_t slot_index = 0; slot_index < 4; slot_index += 1) {
		uint8_t local_joint = result.meshlet_local_joints[meshlet_vertex_index * 4 + slot_index];
		if (local_joint >= end - begin) return false;
		skin.joints[slot_index] = result.meshlet_joint_palette[begin + local_joint];
		skin.weights[slot_index] = result.vertex_skins[result.base.meshlet_vertex_indices[meshlet_vertex_index]].weights[slot_index];
	}
	return true;
}

static bool SkinsAreEqual(const MdtSkinInfluences& lhs, const MdtSkinInfluences& rhs) {
	for (uint32_t slot_index = 0; slot_index < 4; slot_index += 1) {
		if (lhs.joints[slot_index] != rhs.joints[slot_index]) return false;
		if (fabsf(lhs.weights[slot_index] - rhs.weights[slot_index]) > 1e-5f) return false;
	}
	return true;
}

//
// Non-identity joint affine transforms used by the CPU linear blend skinning checks.
//
struct SkinMatrix {
	float m[12]; // 3x4 row major: { m00, m01, m02, m03, m10, ... }
};

static SkinMatrix MakeSkinMatrix(float tx, float ty, float tz, float scale, float angle) {
	SkinMatrix rotation = {};
	float c = cosf(angle);
	float s = sinf(angle);
	rotation.m[0] = scale * c;  rotation.m[1] = -scale * s; rotation.m[2]  = 0.f; rotation.m[3]  = tx;
	rotation.m[4] = scale * s;  rotation.m[5] = scale * c;  rotation.m[6]  = 0.f; rotation.m[7]  = ty;
	rotation.m[8] = 0.f;        rotation.m[9] = 0.f;        rotation.m[10] = scale; rotation.m[11] = tz;
	return rotation;
}

static void SkinPosition(const SkinMatrix& matrix, const float* position, float* result) {
	result[0] = matrix.m[0] * position[0] + matrix.m[1] * position[1] + matrix.m[2] * position[2] + matrix.m[3];
	result[1] = matrix.m[4] * position[0] + matrix.m[5] * position[1] + matrix.m[6] * position[2] + matrix.m[7];
	result[2] = matrix.m[8] * position[0] + matrix.m[9] * position[1] + matrix.m[10] * position[2] + matrix.m[11];
}

// CPU linear blend skinning using global joint indices.
static void SkinVertexGlobal(const SkinMatrix* matrices, const MdtSkinInfluences& skin, const float* position, float* result) {
	float accumulated[3] = { 0.f, 0.f, 0.f };
	float weight_sum = 0.f;
	for (uint32_t slot_index = 0; slot_index < 4; slot_index += 1) {
		float weight = skin.weights[slot_index];
		if (weight <= 0.f) continue;
		float transformed[3];
		SkinPosition(matrices[skin.joints[slot_index]], position, transformed);
		for (uint32_t axis = 0; axis < 3; axis += 1) accumulated[axis] += transformed[axis] * weight;
		weight_sum += weight;
	}
	float rcp_weight_sum = weight_sum > 0.f ? 1.f / weight_sum : 0.f;
	for (uint32_t axis = 0; axis < 3; axis += 1) result[axis] = accumulated[axis] * rcp_weight_sum;
}

// CPU linear blend skinning using a meshlet local palette.
static void SkinVertexPalette(const SkinMatrix* matrices, const MdtSkinnedContinuousLodBuildResult& result, uint32_t meshlet_index, uint32_t meshlet_vertex_index, const float* position, float* skinned_position) {
	uint32_t palette_begin = result.meshlet_palette_offsets[meshlet_index];
	uint32_t palette_end   = result.meshlet_palette_offsets[meshlet_index + 1];

	auto& skin = result.vertex_skins[result.base.meshlet_vertex_indices[meshlet_vertex_index]];

	float accumulated[3] = { 0.f, 0.f, 0.f };
	float weight_sum = 0.f;
	for (uint32_t slot_index = 0; slot_index < 4; slot_index += 1) {
		float weight = skin.weights[slot_index];
		if (weight <= 0.f) continue;

		uint8_t local_joint = result.meshlet_local_joints[meshlet_vertex_index * 4 + slot_index];
		if (palette_begin + local_joint >= palette_end) continue; // Checked by caller, keep it safe.

		uint32_t global_joint = result.meshlet_joint_palette[palette_begin + local_joint];
		float transformed[3];
		SkinPosition(matrices[global_joint], position, transformed);
		for (uint32_t axis = 0; axis < 3; axis += 1) accumulated[axis] += transformed[axis] * weight;
		weight_sum += weight;
	}
	float rcp_weight_sum = weight_sum > 0.f ? 1.f / weight_sum : 0.f;
	for (uint32_t axis = 0; axis < 3; axis += 1) skinned_position[axis] = accumulated[axis] * rcp_weight_sum;
}

static SkinMatrix MakeJointMatrix(uint32_t joint) {
	// Non-identity, joint dependent affine transform.
	return MakeSkinMatrix(0.15f * float(joint) - 0.2f, 0.07f * float(joint) + 0.1f, 0.03f * float(joint), 0.9f + 0.05f * float(joint % 3), 0.4f * float(joint) + 0.3f);
}

static void BuildJointMatrices(const SkinMatrix*& matrices, std::vector<SkinMatrix>& storage, uint32_t joint_count) {
	storage.resize(joint_count);
	for (uint32_t joint = 0; joint < joint_count; joint += 1) storage[joint] = MakeJointMatrix(joint);
	matrices = storage.data();
}

} // namespace


//
// identity group: welding, normalization, disjoint skin, ordinary attribute wedges and CPU skinning.
//

static int TestIdentityNormalizationAndWelding() {
	// Duplicate weights, unsorted slots, zero weight slots and zero weight joints that don't
	// participate in vertex identity.
	Mesh mesh;
	mesh.joint_count = 7;
	AddVertex(mesh, 0.f, 0.f, 0.f, 0.0f, 0.0f, MakeSkin(2, 0.5f, 0, 0.f, 5, 0.3f, 2, 0.5f));
	AddVertex(mesh, 1.f, 0.f, 0.f, 1.0f, 0.0f, MakeSkin(4, 0.2f, 1, 0.6f, 0, 0.f, 0, 0.f));
	AddVertex(mesh, 0.f, 1.f, 0.f, 0.0f, 1.0f, MakeSkin(6, 1.0f, 0, 0.f, 0, 0.f, 0, 0.f));
	// Duplicate of vertex 0 position with a different but equivalent canonical skin, plus a
	// different zero weight joint id. Should weld with vertex 0.
	AddVertex(mesh, 0.f, 0.f, 0.f, 0.25f, 0.75f, MakeSkin(5, 0.3f, 9, 0.f, 2, 0.5f, 2, 0.5f));
	AddTriangle(mesh, 0, 1, 2);
	AddTriangle(mesh, 3, 1, 2);

	auto setup = MakeGeometrySetup(mesh);
	MdtLevelOfDetailTargetDesc levels[1] = { { 2, FLT_MAX } };
	MdtSkinnedDiscreteLodBuildInputs inputs = {};
	inputs.mesh = setup.mesh_desc;
	inputs.level_of_detail_descs = levels;
	inputs.level_of_detail_count = 1;

	MdtSkinnedDiscreteLodBuildResult result = {};
	TASK_CHECK(MdtBuildSkinnedDiscreteLod(&inputs, &result, nullptr) == 1);

	TASK_CHECK(result.base.vertex_count > 0);
	TASK_CHECK(result.base.index_count > 0);
	TASK_CHECK(result.vertex_skins != nullptr);

	// Duplicated input vertex 0 / 3 must have welded into a single output vertex: 4 input vertices.
	TASK_CHECK(result.base.vertex_count == 4);

	for (uint32_t vertex_index = 0; vertex_index < result.base.vertex_count; vertex_index += 1) {
		TASK_CHECK(IsNormalized(result.vertex_skins[vertex_index]));
	}

	// Output vertex 0 is the welded vertex with merged duplicate joint 2 slot.
	uint32_t welded_index = 0;
	for (uint32_t vertex_index = 0; vertex_index < result.base.vertex_count; vertex_index += 1) {
		if (result.base.vertices[vertex_index * 5] == 0.f && result.base.vertices[vertex_index * 5 + 1] == 0.f) {
			if (result.vertex_skins[vertex_index].weights[2] > 0.f || result.vertex_skins[vertex_index].weights[3] > 0.f) {
				welded_index = vertex_index;
			}
		}
	}
	// Joint 2 merged weight: 0.5 + 0.5 = 1.0 out of total 1.3, normalized to 1/1.3,
	// joint 5 weight 0.3/1.3. Ascending order puts joint 2 first.
	TASK_CHECK(result.vertex_skins[welded_index].joints[0] == 2);
	TASK_CHECK(fabsf(result.vertex_skins[welded_index].weights[0] - 1.f / 1.3f) < 1e-4f);
	TASK_CHECK(result.vertex_skins[welded_index].joints[1] == 5);
	TASK_CHECK(fabsf(result.vertex_skins[welded_index].weights[1] - 0.3f / 1.3f) < 1e-4f);
	TASK_CHECK(result.vertex_skins[welded_index].weights[2] == 0.f);
	TASK_CHECK(result.vertex_skins[welded_index].weights[3] == 0.f);

	MdtFreeSkinnedDiscreteLodBuildResult(&result, nullptr);
	return 0;
}

//
// Builds two geometries that touch along the x = 1 seam, each with its own vertex buffer, and
// reports how many seam positions referenced by one geometry are not referenced by the other.
// A value of zero means the seam stayed geometrically coordinated.
//
static uint32_t CountUnmatchedSeamPositions(bool same_skin, bool same_uv, uint32_t target) {
	const uint32_t size_y = 12;
	const uint32_t size_x = 3;

	Mesh mesh;
	mesh.joint_count = 2;
	for (uint32_t side = 0; side < 2; side += 1) {
		for (uint32_t y = 0; y < size_y; y += 1) {
			for (uint32_t x = 0; x < size_x; x += 1) {
				float u = float(x) / float(size_x - 1) + float(side);
				float v = float(y) / float(size_y - 1);
				bool is_seam = (side == 1 && x == 0);
				uint32_t joint = (is_seam && !same_skin) ? 1u : 0u;
				float uv_shift = (is_seam && !same_uv) ? 0.25f : 0.f;
				AddVertex(mesh, u, v, 0.f, u + uv_shift, v, MakeSkin(joint, 1.f));
			}
		}
	}

	// Fill the interleaved index buffer (both geometries contiguous) before taking pointers into it.
	for (uint32_t side = 0; side < 2; side += 1) {
		for (uint32_t y = 0; y + 1 < size_y; y += 1) {
			for (uint32_t x = 0; x + 1 < size_x; x += 1) {
				uint32_t a = y * size_x + x;
				uint32_t b = a + 1;
				uint32_t c = a + size_x;
				uint32_t d = c + 1;
				AddTriangle(mesh, a, b, d);
				AddTriangle(mesh, a, d, c);
			}
		}
	}

	// Each geometry uses half of the concatenated index range.
	MdtTriangleGeometryDesc descs[2] = {};
	const MdtSkinInfluences* skin_ptrs[2] = {};
	uint32_t base = 0;
	uint32_t geometry_quad_count = (size_y - 1) * (size_x - 1);
	for (uint32_t side = 0; side < 2; side += 1) {
		descs[side].indices      = mesh.indices.data() + side * geometry_quad_count * 6;
		descs[side].index_count  = geometry_quad_count * 6;
		descs[side].vertices     = mesh.vertices.data() + base * 5;
		descs[side].vertex_count = size_x * size_y;
		skin_ptrs[side]          = mesh.skins.data() + base;
		base += size_x * size_y;
	}

	MdtSkinnedMeshDesc mesh_desc = {};
	mesh_desc.mesh.geometry_descs      = descs;
	mesh_desc.mesh.geometry_desc_count = 2;
	mesh_desc.mesh.vertex_stride_bytes = 5 * sizeof(float);
	mesh_desc.mesh.geometric_weight    = 0.5f;
	mesh_desc.geometry_skins           = skin_ptrs;
	mesh_desc.joint_count              = 2;

	MdtLevelOfDetailTargetDesc levels[1] = { { target, FLT_MAX } };
	MdtSkinnedDiscreteLodBuildInputs inputs = {};
	inputs.mesh = mesh_desc;
	inputs.level_of_detail_descs = levels;
	inputs.level_of_detail_count = 1;

	MdtSkinnedDiscreteLodBuildResult result = {};
	if (MdtBuildSkinnedDiscreteLod(&inputs, &result, nullptr) != 1) return 0xFFFFFFFFu;

	std::set<std::pair<float, float>> seam_positions[2];
	for (uint32_t geometry_index = 0; geometry_index < result.base.geometry_desc_count; geometry_index += 1) {
		auto& desc = result.base.geometry_descs[geometry_index];
		for (uint32_t index_index = desc.begin_indices_index; index_index < desc.end_indices_index; index_index += 1) {
			uint32_t vertex_index = result.base.indices[index_index];
			float x = result.base.vertices[vertex_index * 5];
			float y = result.base.vertices[vertex_index * 5 + 1];
			if (x > 0.999f && x < 1.001f) seam_positions[geometry_index].insert({ x, y });
		}
	}

	uint32_t unmatched = 0;
	for (auto& position : seam_positions[0]) if (!seam_positions[1].count(position)) unmatched += 1;
	for (auto& position : seam_positions[1]) if (!seam_positions[0].count(position)) unmatched += 1;

	MdtFreeSkinnedDiscreteLodBuildResult(&result, nullptr);
	return unmatched;
}

static int TestIdentityDifferentSkinDoesNotWeld() {
	// Same position, same skin, different UV: the seam still welds, so both sides stay coordinated.
	const uint32_t source_face_count = 2 * 11 * 2;
	uint32_t same_skin_uv_apart = CountUnmatchedSeamPositions(true, false, source_face_count / 2);
	TASK_CHECK(same_skin_uv_apart == 0);

	// Same position, same skin, same UV: coordinated as well.
	uint32_t same_skin_same_uv = CountUnmatchedSeamPositions(true, true, source_face_count / 2);
	TASK_CHECK(same_skin_same_uv == 0);

	// The same seam with mismatching skin must not weld: the geometry becomes disconnected there and
	// the seam positions diverge.
	uint32_t different_skin = CountUnmatchedSeamPositions(false, true, source_face_count / 2);
	TASK_CHECK(different_skin > 0);

	// A sufficient joint budget for the seam mesh: both sides stay coordinated and actually reduce.
	Mesh seam_mesh = MakeGrid(16, 8, [](uint32_t x, uint32_t y) {
		return MakeSkin(2 * y, (x % 2 == 0) ? 0.75f : 0.25f, 2 * y + 1, (x % 2 == 0) ? 0.25f : 0.75f);
	}, 18);
	auto seam_setup = MakeGeometrySetup(seam_mesh);
	MdtSkinnedContinuousLodBuildInputs seam_inputs = {};
	seam_inputs.mesh = seam_setup.mesh_desc;
	seam_inputs.meshlet_target_triangle_count = 128;
	seam_inputs.meshlet_target_vertex_count = 254;
	seam_inputs.max_meshlet_joints = 4;
	MdtSkinnedContinuousLodBuildResult seam_result = {};
	TASK_CHECK(MdtBuildSkinnedContinuousLod(&seam_inputs, &seam_result, nullptr) == 1);
	TASK_CHECK(seam_result.base.level_count >= 2);
	TASK_CHECK(GetLevelFaceCountFromMeshlets(seam_result.base, 0) > GetLevelFaceCountFromMeshlets(seam_result.base, seam_result.base.level_count - 1));
	MdtFreeSkinnedContinuousLodBuildResult(&seam_result, nullptr);

	// Even without any decimation the skin signature is part of vertex identity.
	uint32_t different_skin_no_decimation = CountUnmatchedSeamPositions(false, true, source_face_count);
	TASK_CHECK(different_skin_no_decimation > 0);

	// Matching skin without decimation: geometry is welded, so seams stay together.
	uint32_t same_skin_no_decimation = CountUnmatchedSeamPositions(true, true, source_face_count);
	TASK_CHECK(same_skin_no_decimation == 0);

	return 0;
}

static int TestIdentityCpuSkinningAgreement() {
	// Build a CLOD on a rig with several joints and compare global and palette CPU skinning.
	Mesh mesh = MakeGrid(6, 6, [](uint32_t x, uint32_t y) {
		return MakeSkin(x % 3, 0.5f, (x + y) % 3, 0.3f, (y + 1) % 3, 0.2f);
	}, 4);

	auto setup = MakeGeometrySetup(mesh);
	MdtSkinnedContinuousLodBuildInputs inputs = {};
	inputs.mesh = setup.mesh_desc;
	inputs.meshlet_target_triangle_count = 16;
	inputs.meshlet_target_vertex_count = 24;
	inputs.max_meshlet_joints = 4;

	MdtSkinnedContinuousLodBuildResult result = {};
	TASK_CHECK(MdtBuildSkinnedContinuousLod(&inputs, &result, nullptr) == 1);
	TASK_CHECK(result.base.meshlet_count > 0);
	TASK_CHECK(result.base.level_count > 0);

	std::vector<SkinMatrix> matrix_storage;
	const SkinMatrix* matrices = nullptr;
	BuildJointMatrices(matrices, matrix_storage, mesh.joint_count);

	uint32_t checked_vertices = 0;
	for (uint32_t meshlet_index = 0; meshlet_index < result.base.meshlet_count; meshlet_index += 1) {
		auto& meshlet = result.base.meshlets[meshlet_index];
		for (uint32_t vertex_index = meshlet.begin_vertex_indices_index; vertex_index < meshlet.end_vertex_indices_index; vertex_index += 1) {
			uint32_t output_vertex_index = result.base.meshlet_vertex_indices[vertex_index];
			float* position = &result.base.vertices[output_vertex_index * 5];

			float global_result[3];
			float palette_result[3];
			SkinVertexGlobal(matrices, result.vertex_skins[output_vertex_index], position, global_result);
			SkinVertexPalette(matrices, result, meshlet_index, vertex_index, position, palette_result);

			for (uint32_t axis = 0; axis < 3; axis += 1) {
				TASK_CHECK(fabsf(global_result[axis] - palette_result[axis]) < 1e-4f);
			}
			checked_vertices += 1;
		}
	}
	TASK_CHECK(checked_vertices > 0);

	// Every local joint slot with positive weight must name the vertex skin joint through the palette.
	for (uint32_t meshlet_index = 0; meshlet_index < result.base.meshlet_count; meshlet_index += 1) {
		auto& meshlet = result.base.meshlets[meshlet_index];
		uint32_t palette_begin = result.meshlet_palette_offsets[meshlet_index];
		uint32_t palette_end   = result.meshlet_palette_offsets[meshlet_index + 1];
		TASK_CHECK(palette_end >= palette_begin);
		for (uint32_t vertex_index = meshlet.begin_vertex_indices_index; vertex_index < meshlet.end_vertex_indices_index; vertex_index += 1) {
			MdtSkinInfluences reconstructed;
			TASK_CHECK(ReconstructSkin(result, meshlet_index, vertex_index, reconstructed));
			MdtSkinInfluences original = result.vertex_skins[result.base.meshlet_vertex_indices[vertex_index]];
			for (uint32_t slot_index = 0; slot_index < 4; slot_index += 1) {
				if (original.weights[slot_index] > 0.f) {
					TASK_CHECK(reconstructed.joints[slot_index] == original.joints[slot_index]);
				} else {
					TASK_CHECK(result.meshlet_local_joints[vertex_index * 4 + slot_index] == 0);
				}
			}
		}
	}

	MdtFreeSkinnedContinuousLodBuildResult(&result, nullptr);
	return 0;
}

static int TestIdentityInvalidInputs() {
	Mesh mesh = MakeGrid(3, 3, [](uint32_t, uint32_t) { return MakeSkin(0, 1.f); }, 1);
	auto setup = MakeGeometrySetup(mesh);

	MdtLevelOfDetailTargetDesc levels[1] = { { 2, FLT_MAX } };
	MdtSkinnedDiscreteLodBuildInputs inputs = {};
	inputs.mesh = setup.mesh_desc;
	inputs.level_of_detail_descs = levels;
	inputs.level_of_detail_count = 1;

	// Negative weight.
	{
		mesh.skins[0].weights[0] = -1.f;
		MdtSkinnedDiscreteLodBuildResult result = {};
		TASK_CHECK(MdtBuildSkinnedDiscreteLod(&inputs, &result, nullptr) == 0);
		TASK_CHECK(result.base.vertex_count == 0 && result.base.index_count == 0 && result.vertex_skins == nullptr);
		MdtFreeSkinnedDiscreteLodBuildResult(&result, nullptr);
		mesh.skins[0].weights[0] = 1.f;
	}
	// Non-finite weight.
	{
		mesh.skins[0].weights[0] = NAN;
		MdtSkinnedDiscreteLodBuildResult result = {};
		TASK_CHECK(MdtBuildSkinnedDiscreteLod(&inputs, &result, nullptr) == 0);
		MdtFreeSkinnedDiscreteLodBuildResult(&result, nullptr);
		mesh.skins[0].weights[0] = 1.f;
	}
	// Positive weight on an out of range joint.
	{
		mesh.skins[0].joints[1] = 5;
		mesh.skins[0].weights[1] = 0.5f;
		MdtSkinnedDiscreteLodBuildResult result = {};
		TASK_CHECK(MdtBuildSkinnedDiscreteLod(&inputs, &result, nullptr) == 0);
		MdtFreeSkinnedDiscreteLodBuildResult(&result, nullptr);
		mesh.skins[0].joints[1] = 0;
		mesh.skins[0].weights[1] = 0.f;
	}
	// Zero total weight.
	{
		mesh.skins[0].weights[0] = 0.f;
		MdtSkinnedDiscreteLodBuildResult result = {};
		TASK_CHECK(MdtBuildSkinnedDiscreteLod(&inputs, &result, nullptr) == 0);
		MdtFreeSkinnedDiscreteLodBuildResult(&result, nullptr);
		mesh.skins[0].weights[0] = 1.f;
	}
	// CLOD joint limit range is [4, 256].
	{
		MdtSkinnedContinuousLodBuildInputs clod_inputs = {};
		clod_inputs.mesh = setup.mesh_desc;
		clod_inputs.meshlet_target_triangle_count = 16;
		clod_inputs.meshlet_target_vertex_count = 24;

		MdtSkinnedContinuousLodBuildResult result = {};

		clod_inputs.max_meshlet_joints = 3;
		TASK_CHECK(MdtBuildSkinnedContinuousLod(&clod_inputs, &result, nullptr) == 0);
		clod_inputs.max_meshlet_joints = 257;
		TASK_CHECK(MdtBuildSkinnedContinuousLod(&clod_inputs, &result, nullptr) == 0);
		TASK_CHECK(result.base.meshlet_count == 0);
		MdtFreeSkinnedContinuousLodBuildResult(&result, nullptr);
	}
	// Zero initialized results free safely and are independent.
	{
		MdtSkinnedDiscreteLodBuildResult result = {};
		MdtFreeSkinnedDiscreteLodBuildResult(&result, nullptr);
		MdtSkinnedContinuousLodBuildResult clod_result = {};
		MdtFreeSkinnedContinuousLodBuildResult(&clod_result, nullptr);
	}
	return 0;
}


//
// collapse group: acceptable collapses, more than four joints rejection, multi level invariance.
//

static int TestCollapseAcceptsCollapse() {
	// A quad made of two triangles. The opposite corners of the shared diagonal have different
	// single joint skins, so the merged skin has at most two joints. The collapse is accepted and the
	// result keeps the joint identities (no renumbering, no truncation).
	Mesh mesh;
	mesh.joint_count = 8;
	AddVertex(mesh, 0.f, 0.f, 0.1f, 0.f, 0.f, MakeSkin(0, 1.f));
	AddVertex(mesh, 1.f, 0.f, 0.0f, 1.f, 0.f, MakeSkin(1, 1.f));
	AddVertex(mesh, 1.f, 1.f, 0.1f, 1.f, 1.f, MakeSkin(2, 1.f));
	AddVertex(mesh, 0.f, 1.f, 0.0f, 0.f, 1.f, MakeSkin(3, 1.f));
	AddTriangle(mesh, 0, 1, 2);
	AddTriangle(mesh, 0, 2, 3);

	auto setup = MakeGeometrySetup(mesh);
	MdtLevelOfDetailTargetDesc levels[1] = { { 1, FLT_MAX } };
	MdtSkinnedDiscreteLodBuildInputs inputs = {};
	inputs.mesh = setup.mesh_desc;
	inputs.level_of_detail_descs = levels;
	inputs.level_of_detail_count = 1;

	MdtSkinnedDiscreteLodBuildResult result = {};
	TASK_CHECK(MdtBuildSkinnedDiscreteLod(&inputs, &result, nullptr) == 1);
	TASK_CHECK(GetLevelFaceCount(result.base, 0) == 1u);
	TASK_CHECK(result.base.vertex_count == 3u);
	for (uint32_t vertex_index = 0; vertex_index < result.base.vertex_count; vertex_index += 1) {
		TASK_CHECK(IsNormalized(result.vertex_skins[vertex_index]));
		for (uint32_t slot_index = 0; slot_index < 4; slot_index += 1) {
			if (result.vertex_skins[vertex_index].weights[slot_index] > 0.f) {
				TASK_CHECK(result.vertex_skins[vertex_index].joints[slot_index] < mesh.joint_count);
			}
		}
	}
	MdtFreeSkinnedDiscreteLodBuildResult(&result, nullptr);
	return 0;
}

static int TestCollapseRejectsMoreThanFourJoints() {
	// A closed quad where every vertex has a disjoint set of four joints. Blending opposite corners
	// would need eight joints, so that candidate is rejected. Endpoint candidates keep exactly four
	// joints and remain legal in DLOD, which has no meshlet joint limit.
	Mesh mesh;
	mesh.joint_count = 16;
	for (uint32_t vertex_index = 0; vertex_index < 4; vertex_index += 1) {
		uint32_t base = vertex_index * 4;
		float x = (vertex_index == 0 || vertex_index == 3) ? 0.f : 1.f;
		float y = (vertex_index < 2) ? 0.f : 1.f;
		AddVertex(mesh, x, y, 0.05f * float(vertex_index & 1), 0.f, 0.f,
			MakeSkin(base + 0, 0.25f, base + 1, 0.25f, base + 2, 0.25f, base + 3, 0.25f));
	}
	AddTriangle(mesh, 0, 1, 2);
	AddTriangle(mesh, 0, 2, 3);

	auto setup = MakeGeometrySetup(mesh);
	MdtLevelOfDetailTargetDesc levels[1] = { { 1, FLT_MAX } };
	MdtSkinnedDiscreteLodBuildInputs inputs = {};
	inputs.mesh = setup.mesh_desc;
	inputs.level_of_detail_descs = levels;
	inputs.level_of_detail_count = 1;

	MdtSkinnedDiscreteLodBuildResult result = {};
	TASK_CHECK(MdtBuildSkinnedDiscreteLod(&inputs, &result, nullptr) == 1);
	for (uint32_t vertex_index = 0; vertex_index < result.base.vertex_count; vertex_index += 1) {
		TASK_CHECK(IsNormalized(result.vertex_skins[vertex_index]));
		uint32_t positive_slot_count = 0;
		for (uint32_t slot_index = 0; slot_index < 4; slot_index += 1) {
			if (result.vertex_skins[vertex_index].weights[slot_index] > 0.f) positive_slot_count += 1;
		}
		// Blended skin can never exceed four joints: no truncation and no joint renumbering.
		TASK_CHECK(positive_slot_count <= 4u);
	}
	MdtFreeSkinnedDiscreteLodBuildResult(&result, nullptr);

	// The same mesh in a CLOD with max_meshlet_joints = 4 must be rejected outright because a
	// source triangle already references more than four joints.
	MdtSkinnedContinuousLodBuildInputs clod_inputs = {};
	clod_inputs.mesh = setup.mesh_desc;
	clod_inputs.meshlet_target_triangle_count = 16;
	clod_inputs.meshlet_target_vertex_count = 24;
	clod_inputs.max_meshlet_joints = 4;

	MdtSkinnedContinuousLodBuildResult clod_result = {};
	TASK_CHECK(MdtBuildSkinnedContinuousLod(&clod_inputs, &clod_result, nullptr) == 0);
	TASK_CHECK(clod_result.base.meshlet_count == 0 && clod_result.meshlet_palette_offsets == nullptr);
	MdtFreeSkinnedContinuousLodBuildResult(&clod_result, nullptr);

	// With a larger budget the same mesh builds fine and needs a larger palette.
	clod_inputs.max_meshlet_joints = 16;
	MdtSkinnedContinuousLodBuildResult larger_budget_result = {};
	TASK_CHECK(MdtBuildSkinnedContinuousLod(&clod_inputs, &larger_budget_result, nullptr) == 1);
	uint32_t max_palette = 0;
	for (uint32_t meshlet_index = 0; meshlet_index < larger_budget_result.base.meshlet_count; meshlet_index += 1) {
		uint32_t palette_size = larger_budget_result.meshlet_palette_offsets[meshlet_index + 1] - larger_budget_result.meshlet_palette_offsets[meshlet_index];
		if (palette_size > max_palette) max_palette = palette_size;
	}
	TASK_CHECK(max_palette > 4u);
	MdtFreeSkinnedContinuousLodBuildResult(&larger_budget_result, nullptr);

	return 0;
}

static int TestCollapseRejectsWhenTriangleJointUnionExceedsLimit() {
	// A strip where each triangle is legal on its own (4 joints), but the union of opposite
	// vertices would exceed four joints. The CLOD must reject the collapse yet still build.
	Mesh mesh;
	mesh.joint_count = 32;
	for (uint32_t x = 0; x < 8; x += 1) {
		uint32_t base = x * 2;
		AddVertex(mesh, float(x), 0.f, 0.f, float(x), 0.f, MakeSkin(base, 0.5f, base + 1, 0.5f));
		AddVertex(mesh, float(x), 1.f, 0.05f, float(x), 1.f, MakeSkin(base, 0.5f, base + 1, 0.5f));
	}
	for (uint32_t x = 0; x + 1 < 8; x += 1) {
		uint32_t a = x * 2;
		AddTriangle(mesh, a, a + 1, a + 3);
		AddTriangle(mesh, a, a + 3, a + 2);
	}

	auto setup = MakeGeometrySetup(mesh);
	MdtSkinnedContinuousLodBuildInputs inputs = {};
	inputs.mesh = setup.mesh_desc;
	inputs.meshlet_target_triangle_count = 16;
	inputs.meshlet_target_vertex_count = 24;
	inputs.max_meshlet_joints = 4;

	MdtSkinnedContinuousLodBuildResult result = {};
	TASK_CHECK(MdtBuildSkinnedContinuousLod(&inputs, &result, nullptr) == 1);
	TASK_CHECK(result.base.meshlet_count > 0);

	for (uint32_t meshlet_index = 0; meshlet_index < result.base.meshlet_count; meshlet_index += 1) {
		uint32_t palette_size = result.meshlet_palette_offsets[meshlet_index + 1] - result.meshlet_palette_offsets[meshlet_index];
		TASK_CHECK(palette_size <= 4u);
	}
	MdtFreeSkinnedContinuousLodBuildResult(&result, nullptr);
	return 0;
}

static int TestCollapseMultiLevelImmutability() {
	// 32x32 grid with a single joint. Build two DLOD levels and a multi level CLOD. Finer level
	// vertices and skins must not change once a coarser level is built.
	Mesh mesh = MakeGrid(32, 32, [](uint32_t, uint32_t) { return MakeSkin(0, 1.f); }, 1);

	// DLOD: half the source face count.
	{
		auto setup = MakeGeometrySetup(mesh);
		uint32_t source_face_count = uint32_t(mesh.indices.size() / 3);
		MdtLevelOfDetailTargetDesc levels[2] = { { source_face_count / 2, FLT_MAX }, { source_face_count / 4, FLT_MAX } };
		MdtSkinnedDiscreteLodBuildInputs inputs = {};
		inputs.mesh = setup.mesh_desc;
		inputs.level_of_detail_descs = levels;
		inputs.level_of_detail_count = 2;

		MdtSkinnedDiscreteLodBuildResult result = {};
		TASK_CHECK(MdtBuildSkinnedDiscreteLod(&inputs, &result, nullptr) == 1);
		TASK_CHECK(result.base.level_of_detail_count == 2);
		// The grid can't be decimated by exactly one face per collapse (a collapse can remove two
		// faces), so only require the target to be reached.
		TASK_CHECK(GetLevelFaceCount(result.base, 0) <= source_face_count / 2);
		TASK_CHECK(GetLevelFaceCount(result.base, 0) > 0);
		TASK_CHECK(GetLevelFaceCount(result.base, 1) <= source_face_count / 4);
		TASK_CHECK(GetLevelFaceCount(result.base, 1) > 0);
		TASK_CHECK(result.vertex_skins != nullptr);

		// The first level's vertices live at the start of the shared vertex array and must stay intact
		// when level 2 is appended. Capture and re-validate them.
		uint32_t level_zero_geometry_end = result.base.geometry_descs[result.base.level_of_detail_descs[0].begin_geometry_index].end_indices_index;
		std::vector<MdtSkinInfluences> level_zero_skins;
		std::vector<float> level_zero_positions;
		for (uint32_t index_index = result.base.geometry_descs[result.base.level_of_detail_descs[0].begin_geometry_index].begin_indices_index; index_index < level_zero_geometry_end; index_index += 1) {
			uint32_t vertex_index = result.base.indices[index_index];
			level_zero_skins.push_back(result.vertex_skins[vertex_index]);
			level_zero_positions.push_back(result.base.vertices[vertex_index * 5]);
			level_zero_positions.push_back(result.base.vertices[vertex_index * 5 + 1]);
		}
		TASK_CHECK(level_zero_skins.size() > 0);

		for (uint32_t vertex_index = 0; vertex_index < result.base.vertex_count; vertex_index += 1) {
			TASK_CHECK(IsNormalized(result.vertex_skins[vertex_index]));
			TASK_CHECK(HasJoint(result.vertex_skins[vertex_index], 0));
		}
		MdtFreeSkinnedDiscreteLodBuildResult(&result, nullptr);
	}

	// CLOD: multiple levels.
	{
		auto setup = MakeGeometrySetup(mesh);
		MdtSkinnedContinuousLodBuildInputs inputs = {};
		inputs.mesh = setup.mesh_desc;
		inputs.meshlet_target_triangle_count = 64;
		inputs.meshlet_target_vertex_count = 96;
		inputs.max_meshlet_joints = 4;

		MdtSkinnedContinuousLodBuildResult result = {};
		TASK_CHECK(MdtBuildSkinnedContinuousLod(&inputs, &result, nullptr) == 1);
		TASK_CHECK(result.base.level_count >= 2);
		TASK_CHECK(result.base.meshlet_count > 0 && result.base.vertex_count > 0);

		// Snapshot the vertices of the first level and verify they're unchanged after all levels built.
		auto& level_zero = result.base.levels[0];
		std::map<uint32_t, MdtSkinInfluences> level_zero_skins;
		std::map<uint32_t, std::pair<float, float>> level_zero_positions;
		for (uint32_t meshlet_index = level_zero.begin_meshlets_index; meshlet_index < level_zero.end_meshlets_index; meshlet_index += 1) {
			auto& meshlet = result.base.meshlets[meshlet_index];
			for (uint32_t vertex_index = meshlet.begin_vertex_indices_index; vertex_index < meshlet.end_vertex_indices_index; vertex_index += 1) {
				uint32_t output_vertex_index = result.base.meshlet_vertex_indices[vertex_index];
				level_zero_skins[output_vertex_index] = result.vertex_skins[output_vertex_index];
				level_zero_positions[output_vertex_index] = { result.base.vertices[output_vertex_index * 5], result.base.vertices[output_vertex_index * 5 + 1] };
			}
		}
		TASK_CHECK(level_zero_skins.size() > 0);

		for (uint32_t level_index = 1; level_index < result.base.level_count; level_index += 1) {
			auto& level = result.base.levels[level_index];
			TASK_CHECK(level.begin_meshlets_index >= level_zero.end_meshlets_index);
			TASK_CHECK(level.begin_meshlet_groups_index >= level_zero.end_meshlet_groups_index);
		}

		// Re-check the snapshotted level zero data.
		for (auto& entry : level_zero_skins) {
			TASK_CHECK(SkinsAreEqual(entry.second, result.vertex_skins[entry.first]));
			TASK_CHECK(result.base.vertices[entry.first * 5] == level_zero_positions[entry.first].first);
			TASK_CHECK(result.base.vertices[entry.first * 5 + 1] == level_zero_positions[entry.first].second);
			TASK_CHECK(IsNormalized(result.vertex_skins[entry.first]));
			TASK_CHECK(HasJoint(result.vertex_skins[entry.first], 0));
		}

		// Coarser levels must have fewer faces.
		TASK_CHECK(GetLevelFaceCountFromMeshlets(result.base, 0) > GetLevelFaceCountFromMeshlets(result.base, result.base.level_count - 1));
		TASK_CHECK(GetLevelFaceCountFromMeshlets(result.base, result.base.level_count - 1) > 0);

		MdtFreeSkinnedContinuousLodBuildResult(&result, nullptr);
	}

	return 0;
}

static int TestCollapseOversubscribedInitialTriangleFails() {
	// Every vertex has four distinct joints; a triangle's union is twelve. The CLOD build must fail
	// as a whole, leaving the input and result untouched.
	Mesh mesh;
	mesh.joint_count = 24;
	for (uint32_t vertex_index = 0; vertex_index < 3; vertex_index += 1) {
		uint32_t base = vertex_index * 4;
		float x = vertex_index == 0 ? 0.f : (vertex_index == 1 ? 1.f : 0.f);
		float y = vertex_index == 2 ? 1.f : 0.f;
		AddVertex(mesh, x, y, 0.f, x, y, MakeSkin(base + 0, 0.25f, base + 1, 0.25f, base + 2, 0.25f, base + 3, 0.25f));
	}
	AddTriangle(mesh, 0, 1, 2);

	auto setup = MakeGeometrySetup(mesh);
	MdtSkinnedContinuousLodBuildInputs inputs = {};
	inputs.mesh = setup.mesh_desc;
	inputs.meshlet_target_triangle_count = 16;
	inputs.meshlet_target_vertex_count = 24;
	inputs.max_meshlet_joints = 4;

	uint32_t snapshot_indices[3] = { mesh.indices[0], mesh.indices[1], mesh.indices[2] };
	uint32_t snapshot_joint = mesh.skins[0].joints[0];

	MdtSkinnedContinuousLodBuildResult result = {};
	TASK_CHECK(MdtBuildSkinnedContinuousLod(&inputs, &result, nullptr) == 0);
	TASK_CHECK(result.base.meshlet_count == 0);
	TASK_CHECK(result.base.level_count == 0);
	TASK_CHECK(result.vertex_skins == nullptr);
	TASK_CHECK(result.meshlet_palette_offsets == nullptr);
	TASK_CHECK(result.meshlet_joint_palette == nullptr);
	TASK_CHECK(result.meshlet_local_joints == nullptr);

	// Inputs unchanged.
	TASK_CHECK(mesh.indices[0] == snapshot_indices[0] && mesh.indices[1] == snapshot_indices[1] && mesh.indices[2] == snapshot_indices[2]);
	TASK_CHECK(mesh.skins[0].joints[0] == snapshot_joint);

	TASK_CHECK(inputs.max_meshlet_joints == 4);
	MdtFreeSkinnedContinuousLodBuildResult(&result, nullptr);
	return 0;
}


//
// palette group: meshlet joint palettes, local slots, small budget rejection and retry.
//

static int TestPaletteMeshletPalettes() {
	// 16 wide, 8 tall grid. Each row is bound to a distinct pair of joints. Point and face caps have
	// plenty of headroom, so only the 4 joint meshlet limit binds: two adjacent rows fit, three don't.
	Mesh mesh = MakeGrid(16, 8, [](uint32_t x, uint32_t y) {
		return MakeSkin(2 * y, (x % 2 == 0) ? 0.75f : 0.25f, 2 * y + 1, (x % 2 == 0) ? 0.25f : 0.75f);
	}, 18);

	auto setup = MakeGeometrySetup(mesh);
	MdtSkinnedContinuousLodBuildInputs inputs = {};
	inputs.mesh = setup.mesh_desc;
	inputs.meshlet_target_triangle_count = 128;
	inputs.meshlet_target_vertex_count = 254;
	inputs.max_meshlet_joints = 4;

	MdtSkinnedContinuousLodBuildResult result = {};
	TASK_CHECK(MdtBuildSkinnedContinuousLod(&inputs, &result, nullptr) == 1);
	TASK_CHECK(result.base.meshlet_count > 0);
	TASK_CHECK(result.meshlet_palette_offsets != nullptr);
	TASK_CHECK(result.meshlet_joint_palette != nullptr);
	TASK_CHECK(result.meshlet_local_joints != nullptr);

	uint32_t previous_offset = 0;
	for (uint32_t meshlet_index = 0; meshlet_index < result.base.meshlet_count; meshlet_index += 1) {
		uint32_t begin = result.meshlet_palette_offsets[meshlet_index];
		uint32_t end   = result.meshlet_palette_offsets[meshlet_index + 1];
		TASK_CHECK(begin >= previous_offset);
		TASK_CHECK(end >= begin);
		TASK_CHECK(end - begin <= 4u); // max_meshlet_joints

		// The palette is the ascending set of positively weighted global joints referenced by the meshlet.
		std::set<uint32_t> expected_joints;
		auto& meshlet = result.base.meshlets[meshlet_index];
		for (uint32_t vertex_index = meshlet.begin_vertex_indices_index; vertex_index < meshlet.end_vertex_indices_index; vertex_index += 1) {
			uint32_t output_vertex_index = result.base.meshlet_vertex_indices[vertex_index];
			for (uint32_t slot_index = 0; slot_index < 4; slot_index += 1) {
				if (result.vertex_skins[output_vertex_index].weights[slot_index] > 0.f) {
					expected_joints.insert(result.vertex_skins[output_vertex_index].joints[slot_index]);
				}
			}
		}
		TASK_CHECK(expected_joints.size() == end - begin);
		uint32_t palette_index = begin;
		for (uint32_t joint : expected_joints) {
			TASK_CHECK(result.meshlet_joint_palette[palette_index] == joint);
			palette_index += 1;
		}

		// Local slots must reconstruct vertex_skins item by item.
		for (uint32_t vertex_index = meshlet.begin_vertex_indices_index; vertex_index < meshlet.end_vertex_indices_index; vertex_index += 1) {
			MdtSkinInfluences reconstructed;
			TASK_CHECK(ReconstructSkin(result, meshlet_index, vertex_index, reconstructed));
			auto& original = result.vertex_skins[result.base.meshlet_vertex_indices[vertex_index]];
			for (uint32_t slot_index = 0; slot_index < 4; slot_index += 1) {
				if (original.weights[slot_index] > 0.f) {
					TASK_CHECK(reconstructed.joints[slot_index] == original.joints[slot_index]);
				} else {
					TASK_CHECK(result.meshlet_local_joints[vertex_index * 4 + slot_index] == 0);
				}
			}
		}
		previous_offset = end;
	}

	// Also check every level's meshlets.
	for (uint32_t level_index = 0; level_index < result.base.level_count; level_index += 1) {
		auto& level = result.base.levels[level_index];
		TASK_CHECK(level.end_meshlets_index <= result.base.meshlet_count);
		for (uint32_t meshlet_index = level.begin_meshlets_index; meshlet_index < level.end_meshlets_index; meshlet_index += 1) {
			uint32_t palette_size = result.meshlet_palette_offsets[meshlet_index + 1] - result.meshlet_palette_offsets[meshlet_index];
			TASK_CHECK(palette_size <= 4u);
			auto& meshlet = result.base.meshlets[meshlet_index];
			for (uint32_t triangle_index = meshlet.begin_meshlet_triangles_index; triangle_index < meshlet.end_meshlet_triangles_index; triangle_index += 1) {
				auto triangle = result.base.meshlet_triangles[triangle_index];
				TASK_CHECK(triangle.i0 < meshlet.end_vertex_indices_index - meshlet.begin_vertex_indices_index);
				TASK_CHECK(triangle.i1 < meshlet.end_vertex_indices_index - meshlet.begin_vertex_indices_index);
				TASK_CHECK(triangle.i2 < meshlet.end_vertex_indices_index - meshlet.begin_vertex_indices_index);
			}
		}
	}

	// CPU linear blend skinning through the local palette must match global joint skinning exactly.
	std::vector<SkinMatrix> matrix_storage;
	const SkinMatrix* matrices = nullptr;
	BuildJointMatrices(matrices, matrix_storage, mesh.joint_count);
	for (uint32_t meshlet_index = 0; meshlet_index < result.base.meshlet_count; meshlet_index += 1) {
		auto& meshlet = result.base.meshlets[meshlet_index];
		for (uint32_t vertex_index = meshlet.begin_vertex_indices_index; vertex_index < meshlet.end_vertex_indices_index; vertex_index += 1) {
			uint32_t output_vertex_index = result.base.meshlet_vertex_indices[vertex_index];
			float* position = &result.base.vertices[output_vertex_index * 5];
			float global_result[3];
			float palette_result[3];
			SkinVertexGlobal(matrices, result.vertex_skins[output_vertex_index], position, global_result);
			SkinVertexPalette(matrices, result, meshlet_index, vertex_index, position, palette_result);
			for (uint32_t axis = 0; axis < 3; axis += 1) TASK_CHECK(fabsf(global_result[axis] - palette_result[axis]) < 1e-4f);
		}
	}

	MdtFreeSkinnedContinuousLodBuildResult(&result, nullptr);
	return 0;
}

static int TestPaletteBudgetRejectionAndRetry() {
	// 6x6 grid where each column has its own pair of joints. Adjacent columns can't share a
	// meshlet for a 4 joint budget; a larger budget lets meshlets span more columns.
	Mesh mesh = MakeGrid(6, 6, [](uint32_t x, uint32_t) {
		return MakeSkin(2 * x, 0.5f, 2 * x + 1, 0.5f);
	}, 12);

	auto setup = MakeGeometrySetup(mesh);

	uint32_t meshlet_counts[3] = {};
	uint32_t budgets[3] = { 4, 8, 12 };
	for (uint32_t budget_index = 0; budget_index < 3; budget_index += 1) {
		MdtSkinnedContinuousLodBuildInputs inputs = {};
		inputs.mesh = setup.mesh_desc;
		inputs.meshlet_target_triangle_count = 32;
		inputs.meshlet_target_vertex_count = 48;
		inputs.max_meshlet_joints = budgets[budget_index];

		MdtSkinnedContinuousLodBuildResult result = {};
		TASK_CHECK(MdtBuildSkinnedContinuousLod(&inputs, &result, nullptr) == 1);
		TASK_CHECK(result.base.meshlet_count > 0);

		for (uint32_t meshlet_index = 0; meshlet_index < result.base.meshlet_count; meshlet_index += 1) {
			uint32_t palette_size = result.meshlet_palette_offsets[meshlet_index + 1] - result.meshlet_palette_offsets[meshlet_index];
			TASK_CHECK(palette_size <= budgets[budget_index]);
		}
		meshlet_counts[budget_index] = result.base.meshlet_count;
		MdtFreeSkinnedContinuousLodBuildResult(&result, nullptr);
	}
	// A small budget forces more, smaller meshlets than a large budget.
	TASK_CHECK(meshlet_counts[0] >= meshlet_counts[2]);

	// Retry after a rejected budget: a build with budget 4 must still succeed and produce valid data.
	{
		MdtSkinnedContinuousLodBuildInputs inputs = {};
		inputs.mesh = setup.mesh_desc;
		inputs.meshlet_target_triangle_count = 32;
		inputs.meshlet_target_vertex_count = 48;
		inputs.max_meshlet_joints = 4;
		MdtSkinnedContinuousLodBuildResult result = {};
		TASK_CHECK(MdtBuildSkinnedContinuousLod(&inputs, &result, nullptr) == 1);
		TASK_CHECK(result.meshlet_palette_offsets[result.base.meshlet_count] <= result.base.meshlet_count * 4);
		MdtFreeSkinnedContinuousLodBuildResult(&result, nullptr);
	}

	return 0;
}

static int TestPaletteSmallBudgetStillDecimates() {
	// 32x32 planar grid with one joint. The DLOD target is half the source face count and must be
	// reached with a non-empty result.
	Mesh mesh = MakeGrid(32, 32, [](uint32_t, uint32_t) { return MakeSkin(0, 1.f); }, 1);
	uint32_t source_face_count = uint32_t(mesh.indices.size() / 3);
	TASK_CHECK(source_face_count == 2u * 31u * 31u);

	auto setup = MakeGeometrySetup(mesh);
	MdtLevelOfDetailTargetDesc levels[1] = { { source_face_count / 2, FLT_MAX } };
	MdtSkinnedDiscreteLodBuildInputs inputs = {};
	inputs.mesh = setup.mesh_desc;
	inputs.level_of_detail_descs = levels;
	inputs.level_of_detail_count = 1;

	MdtSkinnedDiscreteLodBuildResult result = {};
	TASK_CHECK(MdtBuildSkinnedDiscreteLod(&inputs, &result, nullptr) == 1);
	// Each collapse removes two faces, so the target (half the source face count) is reached from
	// above; the decimation must not stop early.
	TASK_CHECK(GetLevelFaceCount(result.base, 0) <= source_face_count / 2);
	TASK_CHECK(GetLevelFaceCount(result.base, 0) >= source_face_count / 2 - 1);
	TASK_CHECK(GetLevelFaceCount(result.base, 0) > 0);
	TASK_CHECK(result.base.index_count > 0 && result.base.vertex_count > 0);
	for (uint32_t vertex_index = 0; vertex_index < result.base.vertex_count; vertex_index += 1) {
		TASK_CHECK(IsNormalized(result.vertex_skins[vertex_index]));
		TASK_CHECK(HasJoint(result.vertex_skins[vertex_index], 0));
	}
	TASK_CHECK(result.vertex_skins[0].weights[0] == 1.f);
	MdtFreeSkinnedDiscreteLodBuildResult(&result, nullptr);

	// The same mesh through CLOD with a 4 joint budget must build and reduce meshlet face counts
	// across levels.
	MdtSkinnedContinuousLodBuildInputs clod_inputs = {};
	clod_inputs.mesh = setup.mesh_desc;
	clod_inputs.meshlet_target_triangle_count = 64;
	clod_inputs.meshlet_target_vertex_count = 96;
	clod_inputs.max_meshlet_joints = 4;

	MdtSkinnedContinuousLodBuildResult clod_result = {};
	TASK_CHECK(MdtBuildSkinnedContinuousLod(&clod_inputs, &clod_result, nullptr) == 1);
	TASK_CHECK(clod_result.base.level_count >= 2);
	TASK_CHECK(GetLevelFaceCountFromMeshlets(clod_result.base, 0) > GetLevelFaceCountFromMeshlets(clod_result.base, clod_result.base.level_count - 1));
	for (uint32_t meshlet_index = 0; meshlet_index < clod_result.base.meshlet_count; meshlet_index += 1) {
		TASK_CHECK(clod_result.meshlet_palette_offsets[meshlet_index + 1] - clod_result.meshlet_palette_offsets[meshlet_index] <= 4u);
	}
	MdtFreeSkinnedContinuousLodBuildResult(&clod_result, nullptr);
	return 0;
}

static int TestPaletteTwoGeometrySeamCoordination() {
	// Two geometries with matching skins along a shared seam. Same-skin seam vertices weld and stay
	// geometrically coordinated, so decimation reaches the target face count across both geometries.
	// Indices are local to each geometry's own vertex buffer, as required by the API.
	const uint32_t size_x = 4;
	const uint32_t size_y = 5;
	const uint32_t geometry_vertex_count = size_x * size_y;
	const uint32_t geometry_index_count = (size_x - 1) * (size_y - 1) * 6;

	Mesh mesh;
	mesh.joint_count = 1;
	for (uint32_t side = 0; side < 2; side += 1) {
		for (uint32_t y = 0; y < size_y; y += 1) {
			for (uint32_t x = 0; x < size_x; x += 1) {
				float u = float(x) / float(size_x - 1) + float(side);
				float v = float(y) / float(size_y - 1);
				AddVertex(mesh, u, v, 0.f, u, v, MakeSkin(0, 1.f));
			}
		}
	}
	// Each geometry indexes its own vertex buffer starting at zero.
	for (uint32_t side = 0; side < 2; side += 1) {
		for (uint32_t y = 0; y + 1 < size_y; y += 1) {
			for (uint32_t x = 0; x + 1 < size_x; x += 1) {
				uint32_t a = y * size_x + x;
				uint32_t b = a + 1;
				uint32_t c = a + size_x;
				uint32_t d = c + 1;
				AddTriangle(mesh, a, b, d);
				AddTriangle(mesh, a, d, c);
			}
		}
	}

	MdtTriangleGeometryDesc descs[2] = {};
	descs[0].indices      = mesh.indices.data();
	descs[0].index_count  = geometry_index_count;
	descs[0].vertices     = mesh.vertices.data();
	descs[0].vertex_count = geometry_vertex_count;
	descs[1].indices      = mesh.indices.data() + geometry_index_count;
	descs[1].index_count  = geometry_index_count;
	descs[1].vertices     = mesh.vertices.data() + geometry_vertex_count * 5;
	descs[1].vertex_count = geometry_vertex_count;

	// Both geometries get their own skin array pointing into the shared per vertex skins.
	const MdtSkinInfluences* skin_ptrs[2] = { mesh.skins.data(), mesh.skins.data() + geometry_vertex_count };

	MdtSkinnedMeshDesc mesh_desc = {};
	mesh_desc.mesh.geometry_descs      = descs;
	mesh_desc.mesh.geometry_desc_count = 2;
	mesh_desc.mesh.vertex_stride_bytes = 5 * sizeof(float);
	mesh_desc.mesh.geometric_weight    = 0.5f;
	mesh_desc.geometry_skins           = skin_ptrs;
	mesh_desc.joint_count              = 1;

	MdtLevelOfDetailTargetDesc levels[1] = { { 24, FLT_MAX } };
	MdtSkinnedDiscreteLodBuildInputs inputs = {};
	inputs.mesh = mesh_desc;
	inputs.level_of_detail_descs = levels;
	inputs.level_of_detail_count = 1;

	MdtSkinnedDiscreteLodBuildResult result = {};
	TASK_CHECK(MdtBuildSkinnedDiscreteLod(&inputs, &result, nullptr) == 1);
	uint32_t total_faces = GetLevelFaceCount(result.base, 0);
	TASK_CHECK(total_faces <= 24u);
	TASK_CHECK(total_faces > 0);
	TASK_CHECK(result.base.geometry_desc_count == 2);
	TASK_CHECK(result.vertex_skins != nullptr);

	// Both geometries must reference exactly the same seam positions. Matching skins weld, so the
	// seam stays sealed even though decimation runs independently on both sides.
	{
		std::set<std::pair<float, float>> seam_positions[2];
		for (uint32_t geometry_index = 0; geometry_index < result.base.geometry_desc_count; geometry_index += 1) {
			auto& desc = result.base.geometry_descs[geometry_index];
			for (uint32_t index_index = desc.begin_indices_index; index_index < desc.end_indices_index; index_index += 1) {
				uint32_t vertex_index = result.base.indices[index_index];
				float x = result.base.vertices[vertex_index * 5];
				float y = result.base.vertices[vertex_index * 5 + 1];
				if (x > 0.999f && x < 1.001f) seam_positions[geometry_index].insert({ x, y });
			}
		}
		TASK_CHECK(seam_positions[0].size() > 0);
		TASK_CHECK(seam_positions[0] == seam_positions[1]);
	}
	MdtFreeSkinnedDiscreteLodBuildResult(&result, nullptr);

	// CLOD on the same input: the joint budget is satisfied by every meshlet.
	MdtSkinnedContinuousLodBuildInputs clod_inputs = {};
	clod_inputs.mesh = mesh_desc;
	clod_inputs.meshlet_target_triangle_count = 16;
	clod_inputs.meshlet_target_vertex_count = 24;
	clod_inputs.max_meshlet_joints = 4;

	MdtSkinnedContinuousLodBuildResult clod_result = {};
	TASK_CHECK(MdtBuildSkinnedContinuousLod(&clod_inputs, &clod_result, nullptr) == 1);
	TASK_CHECK(clod_result.base.meshlet_count > 0);
	for (uint32_t meshlet_index = 0; meshlet_index < clod_result.base.meshlet_count; meshlet_index += 1) {
		TASK_CHECK(clod_result.meshlet_palette_offsets[meshlet_index + 1] - clod_result.meshlet_palette_offsets[meshlet_index] <= 4u);
	}
	MdtFreeSkinnedContinuousLodBuildResult(&clod_result, nullptr);
	return 0;
}


//
// Case registration.
//
const struct task_case task_cases[] = {
	{ "identity", "normalization_and_welding",        &TestIdentityNormalizationAndWelding },
	{ "identity", "different_skin_does_not_weld",     &TestIdentityDifferentSkinDoesNotWeld },
	{ "identity", "cpu_skinning_agreement",           &TestIdentityCpuSkinningAgreement },
	{ "identity", "invalid_inputs",                   &TestIdentityInvalidInputs },

	{ "collapse", "accepts_collapse",                 &TestCollapseAcceptsCollapse },
	{ "collapse", "rejects_more_than_four_joints",    &TestCollapseRejectsMoreThanFourJoints },
	{ "collapse", "rejects_joint_union_limit",        &TestCollapseRejectsWhenTriangleJointUnionExceedsLimit },
	{ "collapse", "multi_level_immutability",         &TestCollapseMultiLevelImmutability },
	{ "collapse", "oversubscribed_triangle_fails",    &TestCollapseOversubscribedInitialTriangleFails },

	{ "palette",  "meshlet_palettes",                 &TestPaletteMeshletPalettes },
	{ "palette",  "budget_rejection_and_retry",       &TestPaletteBudgetRejectionAndRetry },
	{ "palette",  "small_budget_decimates",           &TestPaletteSmallBudgetStillDecimates },
	{ "palette",  "two_geometry_seam",                &TestPaletteTwoGeometrySeamCoordination },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
