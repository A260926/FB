////////////////////////////////////////////////////////////////////////////////
// The MIT License (MIT)
//
// Copyright (c) 2025 Nicholas Frechette & Animation Compression Library contributors
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

// Protected keyframe example for foot contacts.
//
// This program builds a short procedural walk cycle, marks the foot contact
// moments as protected keyframes, compresses the transform tracks with whole
// keyframe stripping, decompresses the result, and writes a CSV file. Each row
// lists the sample index, whether that sample was protected, and the foot model
// (object) space coordinates after decompression.
//
// Build:
//   c++ -std=c++17 -O2 -Iincludes -Iexternal/rtm/includes examples/protected_contacts.cpp -o protected_contacts
//
// Run:
//   ./protected_contacts > contacts.csv

#include <acl/compression/compress.h>
#include <acl/compression/track_array.h>
#include <acl/compression/transform_error_metrics.h>
#include <acl/core/ansi_allocator.h>
#include <acl/core/impl/debug_track_writer.h>
#include <acl/decompression/decompress.h>

#include <rtm/vector4f.h>
#include <rtm/vqvf.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace acl;

namespace
{
	constexpr float k_sample_rate = 30.0F;
	constexpr uint32_t k_num_bones = 3;		// root, leg, foot
	constexpr uint32_t k_foot_bone_index = 2;
	constexpr uint32_t k_num_frames = 24;

	// The foot is planted (a contact) at these input sample indices.
	const uint32_t k_contact_frames[] = { 4, 12, 20 };

	track_array_vqvf build_walk_cycle(iallocator& allocator)
	{
		track_array_vqvf tracks(allocator, k_num_bones);

		for (uint32_t bone_index = 0; bone_index < k_num_bones; ++bone_index)
		{
			track_desc_transformf desc;
			desc.output_index = bone_index;
			desc.parent_index = bone_index == 0 ? k_invalid_track_index : (bone_index - 1);
			desc.precision = 0.0001F;
			desc.shell_distance = 3.0F;

			track_vqvf track = track_vqvf::make_reserve(desc, allocator, k_num_frames, k_sample_rate);

			for (uint32_t frame_index = 0; frame_index < k_num_frames; ++frame_index)
			{
				rtm::vector4f translation;

				if (bone_index == 0)
				{
					// The root advances steadily along X.
					translation = rtm::vector_set(float(frame_index) * 0.05F, 0.0F, 0.0F);
				}
				else if (bone_index == 1)
				{
					// The leg swings through a smooth vertical arc.
					const float y = 0.4F + 0.25F * std::sin(float(frame_index) * 0.7F);
					translation = rtm::vector_set(0.0F, y, 0.0F);
				}
				else
				{
					// The foot lifts between contacts and stays planted on them.
					float lift = 0.3F * std::sin(float(frame_index) * 0.9F);
					for (uint32_t contact_index = 0; contact_index < sizeof(k_contact_frames) / sizeof(k_contact_frames[0]); ++contact_index)
					{
						if (frame_index == k_contact_frames[contact_index])
							lift = 0.0F;	// The foot touches the ground.
					}
					translation = rtm::vector_set(0.0F, lift, 0.0F);
				}

				track[frame_index] = rtm::vqv_set(rtm::quat_identity(), translation, rtm::vector_set(1.0F));
			}

			tracks[bone_index] = std::move(track);
		}

		return tracks;
	}

	bool is_protected_sample(uint32_t frame_index)
	{
		for (uint32_t contact_index = 0; contact_index < sizeof(k_contact_frames) / sizeof(k_contact_frames[0]); ++contact_index)
		{
			if (k_contact_frames[contact_index] == frame_index)
				return true;
		}
		return false;
	}

	// Walks the parent chain to convert the local foot pose into model (object) space.
	rtm::vector4f compute_foot_model_translation(const acl_impl::debug_track_writer& writer)
	{
		rtm::vqvf object_transforms[k_num_bones];
		for (uint32_t bone_index = 0; bone_index < k_num_bones; ++bone_index)
		{
			const rtm::vqvf local = writer.read_vqv(bone_index);
			if (bone_index == 0)
				object_transforms[bone_index] = local;
			else
				object_transforms[bone_index] = rtm::vqv_normalize(rtm::vqv_mul(local, object_transforms[bone_index - 1]));
		}

		return object_transforms[k_foot_bone_index].translation;
	}
}

int main()
{
	ansi_allocator allocator;
	track_array_vqvf tracks = build_walk_cycle(allocator);

	vqvf_transform_error_metric metric;
	compression_settings settings = get_default_compression_settings();
	settings.error_metric = &metric;
	settings.optimize_loops = false;

	// Aggressive whole keyframe stripping, but the contact samples must stay in the bit stream.
	settings.keyframe_stripping.proportion = 0.7F;
	settings.keyframe_stripping.threshold = 0.001F;
	settings.keyframe_stripping.strip_trivial = true;
	settings.keyframe_stripping.protected_keyframes = k_contact_frames;
	settings.keyframe_stripping.num_protected_keyframes = uint32_t(sizeof(k_contact_frames) / sizeof(k_contact_frames[0]));

	output_stats stats;
	compressed_tracks* compressed_tracks_ = nullptr;
	const error_result result = compress_track_list(allocator, tracks, settings, compressed_tracks_, stats);
	if (result.any())
	{
		std::fprintf(stderr, "compression failed: %s\n", result.c_str());
		return 1;
	}

	decompression_context<default_transform_decompression_settings> context;
	if (!context.initialize(*compressed_tracks_))
	{
		std::fprintf(stderr, "decompression context initialization failed\n");
		return 2;
	}

	acl_impl::debug_track_writer writer(allocator, track_type8::vqvf, k_num_bones);
	// Default (skipped) sub-tracks keep their description values through every decompress call.
	writer.initialize_with_defaults(tracks);
	const float duration = compressed_tracks_->get_duration();

	std::printf("sample_index,is_protected,foot_model_x,foot_model_y,foot_model_z\n");
	for (uint32_t frame_index = 0; frame_index < k_num_frames; ++frame_index)
	{
		const float sample_time = rtm::scalar_min(float(frame_index) / k_sample_rate, duration);
		context.seek(sample_time, sample_rounding_policy::none);
		context.decompress_tracks(writer);

		const rtm::vector4f foot = compute_foot_model_translation(writer);
		std::printf("%u,%d,%.6f,%.6f,%.6f\n",
			frame_index,
			is_protected_sample(frame_index) ? 1 : 0,
			static_cast<float>(rtm::vector_get_x(foot)),
			static_cast<float>(rtm::vector_get_y(foot)),
			static_cast<float>(rtm::vector_get_z(foot)));
	}

	allocator.deallocate(compressed_tracks_, compressed_tracks_->get_size());
	return 0;
}
