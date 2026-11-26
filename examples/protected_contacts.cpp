////////////////////////////////////////////////////////////////////////////////
// The MIT License (MIT)
//
// Copyright (c) 2026 Nicholas Frechette & Animation Compression Library contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
////////////////////////////////////////////////////////////////////////////////

// Protected keyframe example: mark foot contact samples so whole keyframe
// stripping keeps them, compress, decompress, and print the foot model space
// coordinates of every sample as CSV.

#include <acl/compression/compress.h>
#include <acl/compression/track_array.h>
#include <acl/compression/transform_error_metrics.h>
#include <acl/core/ansi_allocator.h>
#include <acl/core/impl/compressed_headers.h>
#include <acl/core/impl/compressed_tracks.impl.h>
#include <acl/core/impl/debug_track_writer.h>
#include <acl/decompression/decompress.h>

#include <rtm/scalarf.h>
#include <rtm/vqvf.h>
#include <rtm/vector4f.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace
{
	using namespace acl;

	constexpr uint32_t k_num_bones = 4;		// root, thigh, shin, foot
	constexpr uint32_t k_foot_bone = 3;
	constexpr uint32_t k_num_samples = 48;
	constexpr float k_sample_rate = 30.0F;

	const uint32_t k_parent_indices[k_num_bones] =
	{
		k_invalid_track_index,	// root
		0,						// thigh
		1,						// shin
		2,						// foot
	};

	// Frames at which the foot is planted on the ground. These are the moments
	// that must survive whole keyframe stripping.
	const uint32_t k_contact_frames[] = { 6, 22, 38 };
	constexpr uint32_t k_num_contact_frames = sizeof(k_contact_frames) / sizeof(k_contact_frames[0]);

	bool is_protected_sample(uint32_t sample_index)
	{
		for (uint32_t index = 0; index < k_num_contact_frames; ++index)
		{
			if (k_contact_frames[index] == sample_index)
				return true;
		}

		return false;
	}

	rtm::vqvf make_local_pose(uint32_t bone, uint32_t frame)
	{
		const float f = float(frame);

		float tx = 0.0F;
		float ty = 0.0F;
		float tz = 0.0F;

		switch (bone)
		{
		case 0:	// Root advances steadily.
			tx = 0.04F * f;
			ty = 0.9F;
			break;
		case 1:	// Thigh.
			tx = 0.10F;
			ty = -0.20F;
			break;
		case 2:	// Shin lifts with a gait cycle, lowest at the contact frames.
			tx = 0.10F;
			ty = -0.25F;
			tz = 0.08F * std::sin(f * 0.9F);
			break;
		case 3:	// Foot height follows an abs-sine so contacts sit on the floor.
			tx = 0.20F;
			ty = 0.05F + 0.12F * std::fabs(std::sin(f * 0.7F));
			break;
		default:
			break;
		}

		return rtm::vqv_set(rtm::quat_identity(), rtm::vector_set(tx, ty, tz), rtm::vector_set(1.0F));
	}

	track_array_vqvf build_clip(ansi_allocator& allocator)
	{
		track_array_vqvf tracks(allocator, k_num_bones);

		for (uint32_t bone = 0; bone < k_num_bones; ++bone)
		{
			track_desc_transformf desc;
			desc.output_index = bone;
			desc.parent_index = k_parent_indices[bone];
			desc.precision = 0.001F;
			desc.shell_distance = 3.0F;

			track_vqvf track = track_vqvf::make_reserve(desc, allocator, k_num_samples, k_sample_rate);
			for (uint32_t frame = 0; frame < k_num_samples; ++frame)
				track[frame] = make_local_pose(bone, frame);

			tracks[bone] = std::move(track);
		}

		return tracks;
	}

	// Multiplies the local chain to recover the foot model space transform.
	rtm::vqvf foot_model_pose(const rtm::vqvf* local_poses)
	{
		rtm::vqvf object_poses[k_num_bones];
		object_poses[0] = local_poses[0];

		for (uint32_t bone = 1; bone < k_num_bones; ++bone)
			object_poses[bone] = rtm::vqv_mul(local_poses[bone], object_poses[k_parent_indices[bone]]);

		return object_poses[k_foot_bone];
	}

	// Reads the per-segment hard keyframe bit masks and returns true for every
	// sample that remains in the compressed stream.
	std::vector<bool> retained_samples(const compressed_tracks& compressed)
	{
		const uint32_t num_samples = compressed.get_num_samples_per_track();
		if (!compressed.has_stripped_keyframes())
			return std::vector<bool>(num_samples, true);

		const acl_impl::transform_tracks_header& header = acl_impl::get_transform_tracks_header(compressed);
		const acl_impl::stripped_segment_header_t* segment_headers = header.get_stripped_segment_headers();
		const uint32_t* segment_start_indices = header.num_segments > 1 ? header.get_segment_start_indices() : nullptr;

		std::vector<bool> retained(num_samples, false);
		for (uint32_t segment_index = 0; segment_index < header.num_segments; ++segment_index)
		{
			const uint32_t start = segment_start_indices != nullptr ? segment_start_indices[segment_index] : 0U;
			const uint32_t end = (segment_start_indices != nullptr && segment_index + 1 < header.num_segments)
				? segment_start_indices[segment_index + 1]
				: num_samples;
			const uint32_t sample_indices = segment_headers[segment_index].sample_indices;

			for (uint32_t local_index = 0; local_index < (end - start); ++local_index)
				retained[start + local_index] = (sample_indices & (1U << (31 - local_index))) != 0;
		}

		return retained;
	}

	compressed_tracks* compress_clip(ansi_allocator& allocator, const track_array_vqvf& tracks,
		vqvf_transform_error_metric& metric, bool protect_contacts, const char* tag)
	{
		compression_settings settings = get_default_compression_settings();
		settings.error_metric = &metric;
		settings.optimize_loops = false;
		settings.keyframe_stripping.proportion = 0.6F;

		if (protect_contacts)
		{
			settings.keyframe_stripping.protected_keyframes = k_contact_frames;
			settings.keyframe_stripping.num_protected_keyframes = k_num_contact_frames;
		}

		output_stats stats;
		compressed_tracks* compressed = nullptr;
		const error_result result = compress_track_list(allocator, tracks, settings, compressed, stats);
		if (result.any())
		{
			std::fprintf(stderr, "compression failed (%s): %s\n", tag, result.c_str());
			return nullptr;
		}

		return compressed;
	}

	bool run(ansi_allocator& allocator)
	{
		vqvf_transform_error_metric metric;
		track_array_vqvf tracks = build_clip(allocator);

		compressed_tracks* unprotected = compress_clip(allocator, tracks, metric, false, "unprotected");
		compressed_tracks* protected_compressed = compress_clip(allocator, tracks, metric, true, "protected");

		if (unprotected == nullptr || protected_compressed == nullptr)
			return false;

		std::fprintf(stderr, "unprotected size: %u bytes, protected size: %u bytes\n",
			unprotected->get_size(), protected_compressed->get_size());

		const std::vector<bool> retained_unprotected = retained_samples(*unprotected);
		const std::vector<bool> retained_protected = retained_samples(*protected_compressed);
		for (uint32_t frame : k_contact_frames)
		{
			std::fprintf(stderr,
				"contact frame %u: retained without protection=%d, retained with protection=%d\n",
				frame,
				retained_unprotected[frame] ? 1 : 0,
				retained_protected[frame] ? 1 : 0);
		}

		decompression_context<default_transform_decompression_settings> context;
		if (!context.initialize(*protected_compressed))
		{
			std::fprintf(stderr, "decompression context failed to initialize\n");
			return false;
		}

		std::printf("sample_index,is_protected,foot_model_x,foot_model_y,foot_model_z\n");

		for (uint32_t frame = 0; frame < k_num_samples; ++frame)
		{
			acl_impl::debug_track_writer_constant_defaults writer(allocator, track_type8::vqvf, k_num_bones);
			context.seek(float(frame) / k_sample_rate, sample_rounding_policy::nearest);
			context.decompress_tracks(writer);

			rtm::vqvf local_poses[k_num_bones];
			for (uint32_t bone = 0; bone < k_num_bones; ++bone)
				local_poses[bone] = writer.read_vqv(bone);

			const rtm::vqvf foot = foot_model_pose(local_poses);
			std::printf("%u,%d,%.6f,%.6f,%.6f\n",
				frame,
				is_protected_sample(frame) ? 1 : 0,
				rtm::scalar_cast(rtm::vector_get_x(foot.translation)),
				rtm::scalar_cast(rtm::vector_get_y(foot.translation)),
				rtm::scalar_cast(rtm::vector_get_z(foot.translation)));
		}

		allocator.deallocate(unprotected, unprotected->get_size());
		allocator.deallocate(protected_compressed, protected_compressed->get_size());
		return true;
	}
}

int main()
{
	ansi_allocator allocator;
	return run(allocator) ? 0 : 1;
}
