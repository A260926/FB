// Offline pivot-aware FBX -> GLB export example.
//
// Loads task_support/rig.fbx, exports its first animation layer over [0, 1]
// seconds into .task-build/example.glb, parses the result back with cgltf and
// verifies one animated skinned vertex against the source production path.
#include "src/ofbx_gltf.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#define CGLTF_IMPLEMENTATION
#include "vendor/cgltf/cgltf.h"

namespace
{

bool readFile(const char* path, std::vector<unsigned char>& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	return true;
}

void mulMat4(const float a[16], const float b[16], float out[16])
{
	float r[16];
	for (int j = 0; j < 4; ++j)
		for (int i = 0; i < 4; ++i)
		{
			float s = 0;
			for (int k = 0; k < 4; ++k) s += a[i + k * 4] * b[k + j * 4];
			r[i + j * 4] = s;
		}
	for (int i = 0; i < 16; ++i) out[i] = r[i];
}

void worldMatrix(const cgltf_node* node, float out[16])
{
	float local[16];
	cgltf_node_transform_local(node, local);
	if (!node->parent)
	{
		for (int i = 0; i < 16; ++i) out[i] = local[i];
		return;
	}
	float parent[16];
	worldMatrix(node->parent, parent);
	mulMat4(parent, local, out);
}

// LINEAR sampler evaluation per the glTF 2.0 rules (lerp for scalars/vectors,
// shortest-arc slerp for quaternions), then application onto node TRS.
void slerp(const float a[4], const float b[4], float t, float out[4])
{
	float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	float bb[4] = {b[0], b[1], b[2], b[3]};
	if (d < 0) { d = -d; bb[0] = -bb[0]; bb[1] = -bb[1]; bb[2] = -bb[2]; bb[3] = -bb[3]; }
	if (d > 0.9995f)
	{
		for (int i = 0; i < 4; ++i) out[i] = a[i] + (bb[i] - a[i]) * t;
		return;
	}
	float theta = std::acos(std::fmin(1.0f, std::fmax(-1.0f, d)));
	float s0 = std::sin((1 - t) * theta) / std::sin(theta);
	float s1 = std::sin(t * theta) / std::sin(theta);
	for (int i = 0; i < 4; ++i) out[i] = s0 * a[i] + s1 * bb[i];
}

void applyAnimationAt(cgltf_data* data, float time)
{
	const cgltf_animation& anim = data->animations[0];
	for (cgltf_size s = 0; s < anim.samplers_count; ++s)
	{
		const cgltf_animation_sampler& sm = anim.samplers[s];
		const cgltf_size n = sm.input->count;
		float prev_t, next_t;
		cgltf_accessor_read_float(sm.input, 0, &prev_t, 1);
		cgltf_accessor_read_float(sm.input, n - 1, &next_t, 1);
		cgltf_size i0 = 0, i1 = n - 1;
		for (cgltf_size i = 1; i < n; ++i)
		{
			float t;
			cgltf_accessor_read_float(sm.input, i, &t, 1);
			if (t >= time) { i1 = i; i0 = i - 1; next_t = t; cgltf_accessor_read_float(sm.input, i0, &prev_t, 1); break; }
		}
		float alpha = next_t > prev_t ? (time - prev_t) / (next_t - prev_t) : 0.0f;
		const cgltf_size comps = cgltf_num_components(sm.output->type);
		float v0[16], v1[16], v[16];
		cgltf_accessor_read_float(sm.output, i0, v0, comps);
		cgltf_accessor_read_float(sm.output, i1, v1, comps);
		// Find the channel targeting this sampler.
		for (cgltf_size c = 0; c < anim.channels_count; ++c)
		{
			if (anim.channels[c].sampler != &sm) continue;
			cgltf_node* node = anim.channels[c].target_node;
			if (anim.channels[c].target_path == cgltf_animation_path_type_rotation && comps == 4)
			{
				slerp(v0, v1, alpha, v);
				for (int k = 0; k < 4; ++k) node->rotation[k] = v[k];
			}
			else
			{
				for (cgltf_size k = 0; k < comps; ++k) v[k] = v0[k] + (v1[k] - v0[k]) * alpha;
				if (anim.channels[c].target_path == cgltf_animation_path_type_translation)
					for (int k = 0; k < 3; ++k) node->translation[k] = v[k];
				else if (anim.channels[c].target_path == cgltf_animation_path_type_scale)
					for (int k = 0; k < 3; ++k) node->scale[k] = v[k];
			}
		}
	}
}

} // namespace

int main()
{
	std::vector<unsigned char> fbx;
	if (!readFile("task_support/rig.fbx", fbx))
	{
		std::fprintf(stderr, "cannot open task_support/rig.fbx\n");
		return 1;
	}

	ofbx::IScene* scene = ofbx::load(fbx.data(), fbx.size(), 0);
	if (!scene)
	{
		std::fprintf(stderr, "FBX load failed: %s\n", ofbx::getError());
		return 1;
	}
	if (scene->getAnimationStackCount() < 1 || !scene->getAnimationStack(0)->getLayer(0))
	{
		std::fprintf(stderr, "rig.fbx has no animation layer\n");
		return 1;
	}
	const ofbx::AnimationLayer* layer = scene->getAnimationStack(0)->getLayer(0);

	ofbx::GltfResult result;
	const ofbx::GltfError err = ofbx::exportGltf(*scene, *layer, 0.0, 1.0, result);
	if (err != ofbx::GltfError::OK)
	{
		std::fprintf(stderr, "export failed: %s\n", result.message ? result.message : "?");
		return 1;
	}
	if (result.node_map.empty())
	{
		std::fprintf(stderr, "export produced no node mapping\n");
		return 1;
	}

	std::ofstream out(".task-build/example.glb", std::ios::binary);
	if (!out)
	{
		std::fprintf(stderr, "cannot write .task-build/example.glb\n");
		return 1;
	}
	out.write(reinterpret_cast<const char*>(result.glb.data()), (std::streamsize)result.glb.size());
	out.close();

	// Read the written file back from disk and parse/validate it with cgltf.
	std::vector<unsigned char> glb_on_disk;
	if (!readFile(".task-build/example.glb", glb_on_disk) || glb_on_disk != result.glb)
	{
		std::fprintf(stderr, "failed to read back .task-build/example.glb\n");
		return 1;
	}
	cgltf_options options = {};
	cgltf_data* data = nullptr;
	if (cgltf_parse(&options, glb_on_disk.data(), glb_on_disk.size(), &data) != cgltf_result_success)
	{
		std::fprintf(stderr, "cgltf_parse failed\n");
		return 1;
	}
	if (cgltf_validate(data) != cgltf_result_success)
	{
		std::fprintf(stderr, "cgltf_validate failed\n");
		return 1;
	}
	if (cgltf_load_buffers(&options, data, nullptr) != cgltf_result_success)
	{
		std::fprintf(stderr, "cgltf_load_buffers failed\n");
		return 1;
	}
	if (data->meshes_count != 1 || data->skins_count != 1 || data->animations_count != 1)
	{
		std::fprintf(stderr, "unexpected GLB contents\n");
		return 1;
	}

	// Sample the skin at t = 0.5 output seconds and check the first vertex.
	const cgltf_skin* skin = &data->skins[0];
	const cgltf_primitive& prim = data->meshes[0].primitives[0];
	const cgltf_accessor* positions = nullptr;
	const cgltf_accessor* joints = nullptr;
	const cgltf_accessor* weights = nullptr;
	for (cgltf_size i = 0; i < prim.attributes_count; ++i)
	{
		if (prim.attributes[i].type == cgltf_attribute_type_position) positions = prim.attributes[i].data;
		if (prim.attributes[i].type == cgltf_attribute_type_joints) joints = prim.attributes[i].data;
		if (prim.attributes[i].type == cgltf_attribute_type_weights) weights = prim.attributes[i].data;
	}
	if (!positions || !joints || !weights || !skin->inverse_bind_matrices)
	{
		std::fprintf(stderr, "missing skin attributes\n");
		return 1;
	}

	// Control point 1 sits at (1,0,0); with identity Transform/TransformLink
	// its skinned position equals the bone's animated world applied to (1,0,0),
	// the same value task_support/baseline.cpp derives from the source scene.
	float p[3], j[4], w[4];
	cgltf_accessor_read_float(positions, 1, p, 3);
	cgltf_accessor_read_float(joints, 1, j, 4);
	cgltf_accessor_read_float(weights, 1, w, 4);

	applyAnimationAt(data, 0.5f); // output time starts at zero

	float skinned[3] = {0, 0, 0};
	for (int k = 0; k < 4; ++k)
	{
		if (w[k] == 0.0f) continue;
		float ibm[16], world[16], combined[16];
		cgltf_accessor_read_float(skin->inverse_bind_matrices, (cgltf_size)j[k], ibm, 16);
		worldMatrix(skin->joints[(cgltf_size)j[k]], world);
		mulMat4(world, ibm, combined);
		for (int r = 0; r < 3; ++r)
			skinned[r] += w[k] * (combined[r] * p[0] + combined[4 + r] * p[1]
				+ combined[8 + r] * p[2] + combined[12 + r]);
	}

	// Source truth at source time 0.5: the rig's bone rotates 0 -> 90 deg and
	// task_support/baseline.cpp derives the expected point (3 - 0.5*sqrt(.5),
	// 3 + sqrt(.5)) from the source production path.
	const double k = std::sqrt(0.5);
	const double expected_x = 3.0 - 0.5 * k;
	const double expected_y = 3.0 + k;
	if (std::abs(skinned[0] - expected_x) > 2e-5 || std::abs(skinned[1] - expected_y) > 2e-5)
	{
		std::fprintf(stderr, "skinned point mismatch: got (%g,%g,%g) expected (%g,%g)\n",
			skinned[0], skinned[1], skinned[2], expected_x, expected_y);
		return 1;
	}

	std::printf("EXAMPLE_OK glb=%zu bytes node_map=%zu skinned=(%.6f,%.6f,%.6f)\n",
		result.glb.size(), result.node_map.size(), skinned[0], skinned[1], skinned[2]);
	cgltf_free(data);
	scene->destroy();
	return 0;
}
