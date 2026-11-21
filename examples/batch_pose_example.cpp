// Batch decompression example.
// Compresses a procedural clip, decompresses many sample times in one call
// with the batch entry point, cross-checks the poses with the reference
// decompression context, and writes the model space bone endpoints to a CSV
// file. The CSV file is regenerated on every run.
//
// Usage: batch_pose_example <output.csv>

#include <acl/compression/compress.h>
#include <acl/compression/track_array.h>
#include <acl/core/ansi_allocator.h>
#include <acl/core/track_writer.h>
#include <acl/decompression/batch_decompress.h>
#include <acl/decompression/decompress.h>

#include <rtm/quatf.h>
#include <rtm/qvvf.h>
#include <rtm/vector4f.h>
#include <rtm/vqvf.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace acl;

namespace
{
	struct pose_writer : track_writer
	{
		rtm::vqvf* pose;
		void RTM_SIMD_CALL write_rotation(uint32_t track_index, rtm::quatf_arg0 rotation) { pose[track_index].rotation = rotation; }
		void RTM_SIMD_CALL write_translation(uint32_t track_index, rtm::vector4f_arg0 translation) { pose[track_index].translation = translation; }
		void RTM_SIMD_CALL write_scale(uint32_t track_index, rtm::vector4f_arg0 scale) { pose[track_index].scale = scale; }
	};

	uint32_t bone_parent(uint32_t bone_index)
	{
		if (bone_index == 0)
			return k_invalid_track_index;
		return (bone_index % 4) == 0 ? bone_index / 2 : bone_index - 1;
	}

	rtm::vqvf make_bone_sample(uint32_t bone_index, float sample_time)
	{
		const float t = sample_time;
		rtm::quatf rotation;
		rtm::vector4f translation;
		switch (bone_index % 4)
		{
		case 0:
			rotation = rtm::quat_identity();
			translation = rtm::vector_zero();
			break;
		case 1:
			rotation = rtm::quat_from_euler(0.31F, -0.22F, 0.13F);
			translation = rtm::vector_set(0.27F, 0.14F, -0.05F);
			break;
		case 2:
			rotation = rtm::quat_from_euler(0.15F * t, 0.07F * t, 0.21F * t);
			translation = rtm::vector_set(0.06F * std::sin(t * 1.3F), 0.23F * t, 0.04F * std::cos(t));
			break;
		default:
			rotation = rtm::quat_from_euler(0.55F * std::sin(t * 0.7F), 0.23F, -0.17F * t);
			translation = rtm::vector_set(-0.19F, 0.33F, 0.08F);
			break;
		}
		return rtm::vqv_set(rotation, translation, rtm::vector_set(1.0F));
	}

	bool scalar_near(float lhs, float rhs)
	{
		return std::fabs(lhs - rhs) <= 1.0E-5F + 1.0E-5F * std::fabs(rhs);
	}

	bool vector_near3(rtm::vector4f_arg0 lhs, rtm::vector4f_arg0 rhs)
	{
		return scalar_near(rtm::vector_get_x(lhs), rtm::vector_get_x(rhs))
			&& scalar_near(rtm::vector_get_y(lhs), rtm::vector_get_y(rhs))
			&& scalar_near(rtm::vector_get_z(lhs), rtm::vector_get_z(rhs));
	}

	bool quat_near_direction(rtm::quatf_arg0 lhs, rtm::quatf_arg0 rhs)
	{
		const float dot = rtm::scalar_cast(rtm::quat_dot_as_scalar(lhs, rhs));
		return (1.0F - std::fabs(dot)) <= 2.0E-5F;
	}
}

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::fprintf(stderr, "usage: batch_pose_example <output.csv>\n");
		return 2;
	}

	ansi_allocator allocator;

	// Build a small skeleton and animate it procedurally
	const uint32_t num_bones = 16;
	const uint32_t num_samples = 120;
	const float sample_rate = 30.0F;

	track_array_vqvf raw_tracks(allocator, num_bones);
	for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
	{
		track_desc_transformf desc;
		desc.output_index = bone_index;
		desc.parent_index = bone_parent(bone_index);
		desc.precision = 0.001F;
		desc.shell_distance = 1.0F;
		track_vqvf track = track_vqvf::make_reserve(desc, allocator, num_samples, sample_rate);
		for (uint32_t sample_index = 0; sample_index < num_samples; ++sample_index)
			track[sample_index] = make_bone_sample(bone_index, float(sample_index) / sample_rate);
		raw_tracks[bone_index] = std::move(track);
	}

	compression_settings settings = get_default_compression_settings();
	settings.optimize_loops = false;
	settings.keyframe_stripping.strip_trivial = false;

	compressed_tracks* packed_tracks = nullptr;
	output_stats compress_stats;
	const error_result compress_error = compress_track_list(allocator, raw_tracks, settings, packed_tracks, compress_stats);
	if (compress_error.any() || packed_tracks == nullptr)
	{
		std::fprintf(stderr, "failed to compress the clip\n");
		return 3;
	}

	// Decompress every request in one batch call
	constexpr uint32_t num_requests = 33;
	const float duration = packed_tracks->get_finite_duration(sample_looping_policy::clamp);

	batch_request requests[num_requests];
	for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
		requests[request_index] = batch_request{ duration * float(request_index) / float(num_requests - 1), sample_rounding_policy::none };

	const size_t workspace_size = batch_decompression_context::query_workspace_size(num_requests);
	void* workspace = allocator.allocate(workspace_size, 16);

	batch_decompression_context batch_context;
	if (!batch_context.initialize(*packed_tracks, num_requests, workspace, workspace_size))
	{
		std::fprintf(stderr, "failed to initialize the batch context\n");
		return 4;
	}

	rtm::vqvf batch_poses[num_requests * 16];
	batch_decode_stats batch_stats;
	if (!batch_context.decompress(requests, num_requests, batch_poses, num_requests * num_bones, num_bones, &batch_stats))
	{
		std::fprintf(stderr, "batch decompression failed\n");
		return 5;
	}

	// Cross-check the batch poses with the reference decompression context
	decompression_context<debug_transform_decompression_settings> reference_context;
	if (!reference_context.initialize(*packed_tracks))
	{
		std::fprintf(stderr, "failed to initialize the reference context\n");
		return 6;
	}

	rtm::vqvf reference_pose[16];
	for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
	{
		pose_writer writer;
		writer.pose = reference_pose;
		reference_context.seek(requests[request_index].sample_time, requests[request_index].rounding_policy);
		reference_context.decompress_tracks(writer);

		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
		{
			const rtm::vqvf& batch_pose = batch_poses[request_index * num_bones + bone_index];
			if (!quat_near_direction(batch_pose.rotation, reference_pose[bone_index].rotation)
				|| !vector_near3(batch_pose.translation, reference_pose[bone_index].translation)
				|| !vector_near3(batch_pose.scale, reference_pose[bone_index].scale))
			{
				std::fprintf(stderr, "batch pose mismatch at request %u bone %u\n", request_index, bone_index);
				return 7;
			}
		}
	}

	// Compose the poses in model space and write the bone endpoints
	FILE* output_file = std::fopen(argv[1], "w");
	if (output_file == nullptr)
	{
		std::fprintf(stderr, "failed to open %s for writing\n", argv[1]);
		return 8;
	}

	std::fprintf(output_file, "request,time,bone,parent,x,y,z\n");
	rtm::vqvf model_poses[16];
	for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
	{
		const rtm::vqvf* local_poses = batch_poses + request_index * num_bones;
		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
		{
			const uint32_t parent_index = bone_parent(bone_index);
			model_poses[bone_index] = parent_index == k_invalid_track_index
				? local_poses[bone_index]
				: rtm::vqv_mul(model_poses[parent_index], local_poses[bone_index]);
		}

		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
		{
			const rtm::vector4f endpoint = model_poses[bone_index].translation;
			const int parent_index = bone_parent(bone_index) == k_invalid_track_index ? -1 : int(bone_parent(bone_index));
			std::fprintf(output_file, "%u,%.6f,%u,%d,%.6f,%.6f,%.6f\n",
				request_index, double(requests[request_index].sample_time), bone_index, parent_index,
				double(rtm::vector_get_x(endpoint)), double(rtm::vector_get_y(endpoint)), double(rtm::vector_get_z(endpoint)));
		}
	}

	std::fclose(output_file);

	std::printf("BATCH_EXAMPLE_OK bones=%u requests=%u endpoint_unpacks=%u csv=%s\n",
		num_bones, num_requests, batch_stats.endpoint_unpacks, argv[1]);

	allocator.deallocate(workspace, workspace_size);
	allocator.deallocate(packed_tracks, packed_tracks->get_size());
	return 0;
}
