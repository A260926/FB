//
// Skinned LOD example.
//
// Builds two discrete LOD levels and a joint-limited continuous LOD for a programmatically generated
// skinned strip, then exports lod0.obj / lod1.obj deformed by CPU linear blend skinning. Each output
// OBJ vertex carries the joint influences as extra coefficients so the skinning remains inspectable.
//
#include "../MeshDecimationTools.h"

#include <cmath>
#include <cstdio>
#include <cfloat>
#include <cstring>
#include <string>
#include <vector>

static MdtSkinInfluences MakeSkin(uint32_t joint_0, float weight_0, uint32_t joint_1 = 0, float weight_1 = 0.f) {
	MdtSkinInfluences skin = {};
	skin.joints[0] = joint_0; skin.weights[0] = weight_0;
	skin.joints[1] = joint_1; skin.weights[1] = weight_1;
	return skin;
}

struct SkinMatrix {
	float m[12];
};

static SkinMatrix MakeJointMatrix(uint32_t joint) {
	SkinMatrix matrix = {};
	float angle = 0.35f * float(joint) + 0.2f;
	float c = cosf(angle);
	float s = sinf(angle);
	float scale = 0.9f + 0.05f * float(joint % 3);
	matrix.m[0] = scale * c;  matrix.m[1] = -scale * s; matrix.m[2]  = 0.f; matrix.m[3]  = 0.12f * float(joint) - 0.1f;
	matrix.m[4] = scale * s;  matrix.m[5] = scale * c;  matrix.m[6]  = 0.f; matrix.m[7]  = 0.05f * float(joint);
	matrix.m[8] = 0.f;        matrix.m[9] = 0.f;        matrix.m[10] = scale; matrix.m[11] = 0.02f * float(joint);
	return matrix;
}

static void SkinPosition(const SkinMatrix& matrix, const float* position, double* result) {
	for (uint32_t axis = 0; axis < 3; axis += 1) {
		result[axis] = (double)matrix.m[axis * 4 + 0] * position[0] +
		               (double)matrix.m[axis * 4 + 1] * position[1] +
		               (double)matrix.m[axis * 4 + 2] * position[2] +
		               (double)matrix.m[axis * 4 + 3];
	}
}

// CPU linear blend skinning in double precision.
static void SkinVertex(const std::vector<SkinMatrix>& matrices, const MdtSkinInfluences& skin, const float* position, double* result) {
	double accumulated[3] = { 0.0, 0.0, 0.0 };
	double weight_sum = 0.0;
	for (uint32_t slot_index = 0; slot_index < 4; slot_index += 1) {
		float weight = skin.weights[slot_index];
		if (weight <= 0.f) continue;

		double transformed[3];
		SkinPosition(matrices[skin.joints[slot_index]], position, transformed);
		for (uint32_t axis = 0; axis < 3; axis += 1) accumulated[axis] += transformed[axis] * (double)weight;
		weight_sum += (double)weight;
	}

	double rcp_weight_sum = weight_sum > 0.0 ? 1.0 / weight_sum : 0.0;
	for (uint32_t axis = 0; axis < 3; axis += 1) result[axis] = accumulated[axis] * rcp_weight_sum;
}

struct SkinnedMesh {
	std::vector<float> vertices;      // Position + two UVs, stride 5 dwords.
	std::vector<uint32_t> indices;
	std::vector<MdtSkinInfluences> skins;
	uint32_t joint_count = 0;
};

static SkinnedMesh MakeMesh(uint32_t size_x, uint32_t size_y) {
	SkinnedMesh mesh;
	const uint32_t joint_count = size_y * 2;
	mesh.joint_count = joint_count;

	for (uint32_t y = 0; y < size_y; y += 1) {
		for (uint32_t x = 0; x < size_x; x += 1) {
			float u = float(x) / float(size_x - 1);
			float v = float(y) / float(size_y - 1);
			mesh.vertices.insert(mesh.vertices.end(), { u, v, 0.15f * u * v, u, v });

			// Each row blends between two joints. A slight vertical gradient makes the
			// welded seams and the collapse blend visible in the exported mesh.
			float t = 0.4f + 0.2f * float(x) / float(size_x - 1);
			mesh.skins.push_back(MakeSkin(y * 2, 1.f - t, y * 2 + 1, t));
		}
	}

	for (uint32_t y = 0; y + 1 < size_y; y += 1) {
		for (uint32_t x = 0; x + 1 < size_x; x += 1) {
			uint32_t a = y * size_x + x;
			uint32_t b = a + 1;
			uint32_t c = a + size_x;
			uint32_t d = c + 1;
			mesh.indices.insert(mesh.indices.end(), { a, b, d, a, d, c });
		}
	}

	return mesh;
}

static bool WriteObjGeometry(
	const char* file_name,
	const MdtDiscreteLodBuildResult& result,
	const MdtSkinInfluences* skins,
	const std::vector<SkinMatrix>& matrices,
	uint32_t level_index) {

	auto* file = fopen(file_name, "wb");
	if (file == nullptr) return false;

	auto& level = result.level_of_detail_descs[level_index];

	fprintf(file, "# CPU linear blend skinned LOD level %u\n", level_index);

	// Deduplicate output vertices across the level's geometries and remember their source index.
	std::vector<uint32_t> source_vertex_indices;
	std::vector<uint32_t> vertex_to_local_index(result.vertex_count, UINT32_MAX);
	uint32_t vertex_stride_dwords = 5;

	for (uint32_t geometry_index = level.begin_geometry_index; geometry_index < level.end_geometry_index; geometry_index += 1) {
		auto& desc = result.geometry_descs[geometry_index];
		for (uint32_t index_index = desc.begin_indices_index; index_index < desc.end_indices_index; index_index += 1) {
			uint32_t vertex_index = result.indices[index_index];
			if (vertex_to_local_index[vertex_index] == UINT32_MAX) {
				vertex_to_local_index[vertex_index] = uint32_t(source_vertex_indices.size());
				source_vertex_indices.push_back(vertex_index);
			}
		}
	}

	for (uint32_t local_index = 0; local_index < source_vertex_indices.size(); local_index += 1) {
		uint32_t vertex_index = source_vertex_indices[local_index];
		double position[3];
		SkinVertex(matrices, skins[vertex_index], &result.vertices[vertex_index * vertex_stride_dwords], position);
		fprintf(file, "v %.6f %.6f %.6f\n", position[0], position[1], position[2]);
	}
	for (uint32_t local_index = 0; local_index < source_vertex_indices.size(); local_index += 1) {
		uint32_t vertex_index = source_vertex_indices[local_index];
		fprintf(file, "vt %.6f %.6f\n", result.vertices[vertex_index * vertex_stride_dwords + 3], result.vertices[vertex_index * vertex_stride_dwords + 4]);
	}

	for (uint32_t geometry_index = level.begin_geometry_index; geometry_index < level.end_geometry_index; geometry_index += 1) {
		auto& desc = result.geometry_descs[geometry_index];
		for (uint32_t index_index = desc.begin_indices_index; index_index + 2 < desc.end_indices_index; index_index += 3) {
			uint32_t v0 = vertex_to_local_index[result.indices[index_index + 0]] + 1;
			uint32_t v1 = vertex_to_local_index[result.indices[index_index + 1]] + 1;
			uint32_t v2 = vertex_to_local_index[result.indices[index_index + 2]] + 1;
			fprintf(file, "f %u/%u %u/%u %u/%u\n", v0, v0, v1, v1, v2, v2);
		}
	}

	fclose(file);
	return true;
}

// Computes the maximum number of distinct positively weighted joints referenced by a meshlet.
static uint32_t ComputeMaxMeshletJointCount(const MdtSkinnedContinuousLodBuildResult& result) {
	uint32_t max_joint_count = 0;
	for (uint32_t meshlet_index = 0; meshlet_index < result.base.meshlet_count; meshlet_index += 1) {
		uint32_t palette_size = result.meshlet_palette_offsets[meshlet_index + 1] - result.meshlet_palette_offsets[meshlet_index];
		if (palette_size > max_joint_count) max_joint_count = palette_size;
	}
	return max_joint_count;
}

// Computes points, faces and the largest meshlet joint palette for a single continuous LOD level.
static void ComputeContinuousLevelStats(const MdtSkinnedContinuousLodBuildResult& result, uint32_t level_index, uint32_t& point_count, uint32_t& face_count, uint32_t& max_joint_count) {
	auto& level = result.base.levels[level_index];
	std::vector<uint8_t> seen(result.base.vertex_count, 0);
	point_count = 0;
	face_count = 0;
	max_joint_count = 0;

	for (uint32_t meshlet_index = level.begin_meshlets_index; meshlet_index < level.end_meshlets_index; meshlet_index += 1) {
		auto& meshlet = result.base.meshlets[meshlet_index];
		face_count += meshlet.end_meshlet_triangles_index - meshlet.begin_meshlet_triangles_index;
		for (uint32_t vertex_index = meshlet.begin_vertex_indices_index; vertex_index < meshlet.end_vertex_indices_index; vertex_index += 1) {
			uint32_t output_vertex_index = result.base.meshlet_vertex_indices[vertex_index];
			if (seen[output_vertex_index] == 0) { seen[output_vertex_index] = 1; point_count += 1; }
		}
		uint32_t palette_size = result.meshlet_palette_offsets[meshlet_index + 1] - result.meshlet_palette_offsets[meshlet_index];
		if (palette_size > max_joint_count) max_joint_count = palette_size;
	}
}

int main(int argument_count, char** arguments) {
	const char* output_directory = argument_count > 1 ? arguments[1] : ".";

	SkinnedMesh mesh = MakeMesh(32, 8);

	// One geometry, five floats per vertex (position + UV), and per vertex skin influences.
	MdtTriangleGeometryDesc geometry_desc = {};
	geometry_desc.indices      = mesh.indices.data();
	geometry_desc.index_count  = uint32_t(mesh.indices.size());
	geometry_desc.vertices     = mesh.vertices.data();
	geometry_desc.vertex_count = uint32_t(mesh.skins.size());

	const MdtSkinInfluences* geometry_skins[1] = { mesh.skins.data() };

	MdtSkinnedMeshDesc mesh_desc = {};
	mesh_desc.mesh.geometry_descs      = &geometry_desc;
	mesh_desc.mesh.geometry_desc_count = 1;
	mesh_desc.mesh.vertex_stride_bytes = 5 * sizeof(float);
	mesh_desc.mesh.geometric_weight    = 0.5f;
	mesh_desc.geometry_skins           = geometry_skins;
	mesh_desc.joint_count              = mesh.joint_count;

	std::vector<SkinMatrix> matrices(mesh.joint_count);
	for (uint32_t joint = 0; joint < mesh.joint_count; joint += 1) matrices[joint] = MakeJointMatrix(joint);

	// Two discrete LOD levels.
	uint32_t source_face_count = uint32_t(mesh.indices.size() / 3);
	MdtLevelOfDetailTargetDesc levels[2] = {};
	levels[0].target_face_count  = source_face_count / 2;
	levels[0].target_error_limit = FLT_MAX;
	levels[1].target_face_count  = source_face_count / 4;
	levels[1].target_error_limit = FLT_MAX;

	MdtSkinnedDiscreteLodBuildInputs discrete_inputs = {};
	discrete_inputs.mesh                  = mesh_desc;
	discrete_inputs.level_of_detail_descs = levels;
	discrete_inputs.level_of_detail_count = 2;

	MdtSkinnedDiscreteLodBuildResult discrete_result = {};
	if (MdtBuildSkinnedDiscreteLod(&discrete_inputs, &discrete_result, nullptr) != 1) {
		fprintf(stderr, "Skinned discrete LOD build failed.\n");
		return 1;
	}

	// Continuous LOD limited to four joints per meshlet.
	MdtSkinnedContinuousLodBuildInputs continuous_inputs = {};
	continuous_inputs.mesh                            = mesh_desc;
	continuous_inputs.meshlet_target_triangle_count   = 64;
	continuous_inputs.meshlet_target_vertex_count     = 96;
	continuous_inputs.max_meshlet_joints              = 4;

	MdtSkinnedContinuousLodBuildResult continuous_result = {};
	if (MdtBuildSkinnedContinuousLod(&continuous_inputs, &continuous_result, nullptr) != 1) {
		fprintf(stderr, "Skinned continuous LOD build failed.\n");
		MdtFreeSkinnedDiscreteLodBuildResult(&discrete_result, nullptr);
		return 1;
	}

	for (uint32_t level_index = 0; level_index < discrete_result.base.level_of_detail_count; level_index += 1) {
		std::string file_name = std::string(output_directory) + "/lod";
		file_name += std::to_string(level_index);
		file_name += ".obj";

		if (WriteObjGeometry(file_name.c_str(), discrete_result.base, discrete_result.vertex_skins, matrices, level_index) == false) {
			fprintf(stderr, "Failed to write '%s'.\n", file_name.c_str());
			MdtFreeSkinnedContinuousLodBuildResult(&continuous_result, nullptr);
			MdtFreeSkinnedDiscreteLodBuildResult(&discrete_result, nullptr);
			return 1;
		}

		auto& level = discrete_result.base.level_of_detail_descs[level_index];
		uint32_t face_count = 0;
		uint32_t referenced_vertex_count = 0;
		std::vector<uint8_t> seen(discrete_result.base.vertex_count, 0);
		for (uint32_t geometry_index = level.begin_geometry_index; geometry_index < level.end_geometry_index; geometry_index += 1) {
			auto& desc = discrete_result.base.geometry_descs[geometry_index];
			face_count += (desc.end_indices_index - desc.begin_indices_index) / 3;
			for (uint32_t index_index = desc.begin_indices_index; index_index < desc.end_indices_index; index_index += 1) {
				uint32_t vertex_index = discrete_result.base.indices[index_index];
				if (seen[vertex_index] == 0) { seen[vertex_index] = 1; referenced_vertex_count += 1; }
			}
		}

		printf("lod%u: points=%u faces=%u\n", level_index, referenced_vertex_count, face_count);
	}

	printf("clod: levels=%u meshlets=%u max_meshlet_joints=%u\n",
		continuous_result.base.level_count,
		continuous_result.base.meshlet_count,
		ComputeMaxMeshletJointCount(continuous_result));

	for (uint32_t level_index = 0; level_index < continuous_result.base.level_count; level_index += 1) {
		uint32_t point_count = 0;
		uint32_t face_count = 0;
		uint32_t max_joint_count = 0;
		ComputeContinuousLevelStats(continuous_result, level_index, point_count, face_count, max_joint_count);
		printf("clod level %u: points=%u faces=%u max_meshlet_joints=%u\n", level_index, point_count, face_count, max_joint_count);
	}

	MdtFreeSkinnedContinuousLodBuildResult(&continuous_result, nullptr);
	MdtFreeSkinnedDiscreteLodBuildResult(&discrete_result, nullptr);
	return 0;
}
