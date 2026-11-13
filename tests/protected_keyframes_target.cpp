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

// Standalone target test for protected keyframes in whole keyframe stripping.
// It calls the real compress_track_list entry and the real decompression_context.
// A failure prints a message and returns a non-zero process exit code.

#include <acl/compression/compress.h>
#include <acl/compression/track_array.h>
#include <acl/compression/transform_error_metrics.h>
#include <acl/core/ansi_allocator.h>
#include <acl/core/impl/compressed_headers.h>
#include <acl/core/impl/debug_track_writer.h>
#include <acl/decompression/decompress.h>

#include <rtm/scalarf.h>
#include <rtm/vector4f.h>
#include <rtm/vqvf.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace acl;
using namespace acl::acl_impl;

namespace
{
	constexpr float k_sample_rate = 30.0F;
	constexpr float k_precision = 0.0001F;

	int g_failures = 0;

	void check_failed(const char* expression, const char* file, int line, const char* format, ...)
	{
		std::printf("CHECK FAILED (%s:%d): %s\n", file, line, expression);
		if (format != nullptr)
		{
			va_list args;
			va_start(args, format);
			std::vprintf(format, args);
			va_end(args);
		}
		++g_failures;
	}

	#define CHECK(expression, ...) \
		do { if (!(expression)) { check_failed(#expression, __FILE__, __LINE__, __VA_ARGS__); } } while (0)

	// Builds a procedural transform clip. Every bone keeps the identity rotation
	// and a unit scale. The root advances on X. Child bones carry a nonlinear
	// vertical motion so that a linear reconstruction has a measurable error.
	track_array_vqvf make_procedural_clip(iallocator& allocator, uint32_t num_bones, uint32_t num_frames, float motion_seed)
	{
		track_array_vqvf tracks(allocator, num_bones);

		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
		{
			track_desc_transformf desc;
			desc.output_index = bone_index;
			desc.parent_index = bone_index == 0 ? k_invalid_track_index : (bone_index - 1);
			desc.precision = k_precision;
			desc.shell_distance = 3.0F;

			track_vqvf track = track_vqvf::make_reserve(desc, allocator, num_frames, k_sample_rate);

			for (uint32_t frame_index = 0; frame_index < num_frames; ++frame_index)
			{
				const float t = float(frame_index);
				const float x = t * 0.05F;
				// A nonlinear bump with a per bone phase so the motion is not a straight line.
				const float y = 0.6F * std::sin(t * 0.55F + float(bone_index) * 0.7F + motion_seed)
					+ 0.25F * std::sin(t * 1.37F + float(bone_index));
				const float z = 0.15F * std::cos(t * 0.41F + float(bone_index) * 1.3F);

				track[frame_index] = rtm::vqv_set(rtm::quat_identity(), rtm::vector_set(x, y, z), rtm::vector_set(1.0F));
			}

			tracks[bone_index] = std::move(track);
		}

		return tracks;
	}

	// Builds a small clip with a foot contact plant at the protected frame. The contact and its
	// two neighbors are perfectly flat, so removing the contact frame reconstructs it exactly.
	// It is therefore the cheapest early removal candidate. The other frames carry a nonlinear
	// lift so that removing any of them has a measurable, larger error. Keeping the contact frame
	// splits the interpolation span of its neighbors and changes their reconstruction endpoints.
	track_array_vqvf make_contact_clip(iallocator& allocator, uint32_t num_frames, uint32_t contact_frame)
	{
		constexpr uint32_t num_bones = 3;
		track_array_vqvf tracks(allocator, num_bones);

		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
		{
			track_desc_transformf desc;
			desc.output_index = bone_index;
			desc.parent_index = bone_index == 0 ? k_invalid_track_index : (bone_index - 1);
			desc.precision = k_precision;

			track_vqvf track = track_vqvf::make_reserve(desc, allocator, num_frames, k_sample_rate);

			for (uint32_t frame_index = 0; frame_index < num_frames; ++frame_index)
			{
				const float x = float(frame_index) * 0.02F;
				float y = 0.0F;
				float z = 0.0F;

				if (bone_index == num_bones - 1)
				{
					// A three frame flat contact plant around the protected frame.
					const bool in_contact_plant =
						frame_index == contact_frame - 1 ||
						frame_index == contact_frame ||
						frame_index == contact_frame + 1;

					if (in_contact_plant)
						y = 0.0F;	// The foot stays planted
					else
						y = 0.9F * std::sin(float(frame_index) * 0.9F + 0.4F) + 0.35F * float(frame_index);
				}

				track[frame_index] = rtm::vqv_set(rtm::quat_identity(), rtm::vector_set(x, y, z), rtm::vector_set(1.0F));
			}

			tracks[bone_index] = std::move(track);
		}

		return tracks;
	}

	compression_settings make_test_settings(vqvf_transform_error_metric& metric)
	{
		compression_settings settings = get_default_compression_settings();
		settings.error_metric = &metric;
		settings.optimize_loops = false;
		return settings;
	}

	compressed_tracks* compress_or_fail(iallocator& allocator, const track_array_vqvf& tracks, const compression_settings& settings, const char* label)
	{
		output_stats stats;
		compressed_tracks* compressed = nullptr;
		const error_result result = compress_track_list(allocator, tracks, settings, compressed, stats);
		CHECK(!result.any(), "label=%s error=%s\n", label, result.c_str());
		CHECK(compressed != nullptr, "label=%s produced no output\n", label);
		return compressed;
	}

	void free_tracks(iallocator& allocator, compressed_tracks* tracks)
	{
		if (tracks != nullptr)
			allocator.deallocate(tracks, tracks->get_size());
	}

	// Retained frame bookkeeping read directly from the compressed bit stream.
	struct retained_clip_info
	{
		uint32_t num_samples = 0;
		uint32_t num_segments = 0;
		std::vector<uint32_t> segment_offsets;
		std::vector<uint32_t> segment_counts;
		std::vector<uint32_t> hard_keyframe_masks;
	};

	retained_clip_info read_retained_info(const compressed_tracks& tracks)
	{
		retained_clip_info info;
		info.num_samples = tracks.get_num_samples_per_track();

		const transform_tracks_header& header = get_transform_tracks_header(tracks);
		info.num_segments = header.num_segments;
		info.segment_offsets.resize(info.num_segments);
		info.segment_counts.resize(info.num_segments);
		info.hard_keyframe_masks.resize(info.num_segments, ~0U);

		const uint32_t* segment_start_indices = info.num_segments > 1 ? header.get_segment_start_indices() : nullptr;

		for (uint32_t segment_index = 0; segment_index < info.num_segments; ++segment_index)
		{
			const uint32_t offset = info.num_segments > 1 ? segment_start_indices[segment_index] : 0U;
			const uint32_t next_offset = segment_index + 1 < info.num_segments
				? segment_start_indices[segment_index + 1]
				: info.num_samples;

			info.segment_offsets[segment_index] = offset;
			info.segment_counts[segment_index] = next_offset - offset;

			if (tracks.has_stripped_keyframes())
				info.hard_keyframe_masks[segment_index] = header.get_stripped_segment_headers()[segment_index].sample_indices;
		}

		return info;
	}

	bool is_frame_retained(const retained_clip_info& info, uint32_t clip_sample_index)
	{
		for (uint32_t segment_index = 0; segment_index < info.num_segments; ++segment_index)
		{
			const uint32_t offset = info.segment_offsets[segment_index];
			const uint32_t count = info.segment_counts[segment_index];

			if (clip_sample_index >= offset && clip_sample_index < offset + count)
			{
				const uint32_t local_index = clip_sample_index - offset;
				// ACL stores the hard keyframe bit set MSB-first: local frame N maps to bit 31-N.
				return (info.hard_keyframe_masks[segment_index] & (1U << (31 - local_index))) != 0;
			}
		}

		return false;
	}

	const keyframe_stripping_metadata_t* read_contributing_metadata(const compressed_tracks& tracks)
	{
		const optional_metadata_header& metadata_header = get_optional_metadata_header(tracks);
		const uint8_t* raw_metadata = metadata_header.get_contributing_error(tracks);
		CHECK(raw_metadata != nullptr, "contributing error metadata is missing\n");
		return bit_cast<const keyframe_stripping_metadata_t*>(raw_metadata);
	}

	// Decompresses every sample and confirms that every foot translation is finite.
	void check_decompression_roundtrip(iallocator& allocator, const compressed_tracks& tracks, uint32_t num_tracks, uint32_t num_samples)
	{
		decompression_context<default_transform_decompression_settings> context;
		CHECK(context.initialize(tracks), "decompression context failed to initialize\n");

		debug_track_writer writer(allocator, track_type8::vqvf, num_tracks);
		const float duration = tracks.get_duration();

		for (uint32_t frame_index = 0; frame_index < num_samples; ++frame_index)
		{
			const float sample_time = rtm::scalar_min(float(frame_index) / k_sample_rate, duration);
			context.seek(sample_time, sample_rounding_policy::none);
			context.decompress_tracks(writer);

			for (uint32_t bone_index = 0; bone_index < num_tracks; ++bone_index)
			{
				const rtm::vector4f& translation = writer.read_vqv(bone_index).translation;
				CHECK(std::isfinite(rtm::vector_get_x(translation)), "non-finite x at frame=%u bone=%u\n", frame_index, bone_index);
				CHECK(std::isfinite(rtm::vector_get_y(translation)), "non-finite y at frame=%u bone=%u\n", frame_index, bone_index);
				CHECK(std::isfinite(rtm::vector_get_z(translation)), "non-finite z at frame=%u bone=%u\n", frame_index, bone_index);
			}
		}
	}

	// A single greedy removal step recorded by the independent reference simulator.
	struct simulated_step
	{
		uint32_t removed_frame = 0;
		uint32_t start_frame = 0;
		uint32_t end_frame = 0;
		float error = 0.0F;
	};

	// Independent step-by-step reconstruction over the raw object space motion.
	// It mirrors the ACL greedy process: at every step, every retained candidate is
	// reconstructed from its retained neighbors with linear interpolation, and the
	// candidate with the worst reconstruction error below the best value is removed.
	// Ties use the smaller index only when a protected list is present.
	std::vector<simulated_step> simulate_greedy_stripping(
		const track_array_vqvf& tracks,
		const uint32_t* protected_list,
		uint32_t num_protected,
		bool has_protection)
	{
		const uint32_t num_frames = tracks.get_num_samples_per_track();
		const uint32_t num_bones = tracks.get_num_tracks();

		std::vector<bool> retained(num_frames, true);
		std::vector<bool> protected_frame(num_frames, false);
		for (uint32_t index = 0; index < num_protected; ++index)
			protected_frame[protected_list[index]] = true;

		// Cache object space translations of the raw clip along the parent chain.
		std::vector<std::vector<rtm::vector4f>> object_translation(num_frames, std::vector<rtm::vector4f>(num_bones));
		for (uint32_t frame_index = 0; frame_index < num_frames; ++frame_index)
		{
			for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
			{
				const rtm::vector4f local = tracks[bone_index][frame_index].translation;
				if (bone_index == 0)
				{
					object_translation[frame_index][bone_index] = local;
				}
				else
				{
					// Rotations are identity in these test clips, so object translation is the chain sum.
					object_translation[frame_index][bone_index] =
						rtm::vector_add(object_translation[frame_index][bone_index - 1], local);
				}
			}
		}

		auto reconstruction_error = [&](uint32_t start_frame, uint32_t end_frame) -> float
		{
			float worst_error_sq = 0.0F;
			for (uint32_t gap_frame = start_frame + 1; gap_frame < end_frame; ++gap_frame)
			{
				const float alpha = float(gap_frame - start_frame) / float(end_frame - start_frame);
				for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
				{
					const rtm::vector4f& a = object_translation[start_frame][bone_index];
					const rtm::vector4f& b = object_translation[end_frame][bone_index];
					const rtm::vector4f reconstructed = rtm::vector_lerp(a, b, alpha);
					const rtm::vector4f raw = object_translation[gap_frame][bone_index];
					const rtm::vector4f delta = rtm::vector_sub(raw, reconstructed);
					const float error_sq = rtm::vector_dot3(delta, delta);
					worst_error_sq = std::max(worst_error_sq, error_sq);
				}
			}
			return std::sqrt(worst_error_sq);
		};

		std::vector<simulated_step> steps;

		for (uint32_t iteration = 1; iteration < num_frames - 1; ++iteration)
		{
			bool found = false;
			simulated_step best;
			best.error = std::numeric_limits<float>::infinity();

			for (uint32_t candidate = 1; candidate < num_frames - 1; ++candidate)
			{
				if (!retained[candidate] || protected_frame[candidate])
					continue;

				uint32_t start_frame = candidate - 1;
				while (!retained[start_frame])
					--start_frame;

				uint32_t end_frame = candidate + 1;
				while (!retained[end_frame])
					++end_frame;

				const float error = reconstruction_error(start_frame, end_frame);

				const bool take_first = !found;
				const bool take_lower = error < best.error;
				const bool take_tie = has_protection && found && error == best.error && candidate < best.removed_frame;

				if (take_first || take_lower || take_tie)
				{
					found = true;
					best.removed_frame = candidate;
					best.start_frame = start_frame;
					best.end_frame = end_frame;
					best.error = error;
				}
			}

			if (!found)
				break;	// Only protected or boundary frames remain

			retained[best.removed_frame] = false;
			steps.push_back(best);
		}

		return steps;
	}

	void test_validation_errors(iallocator& allocator, vqvf_transform_error_metric& metric)
	{
		track_array_vqvf tracks = make_procedural_clip(allocator, 4, 12, 0.0F);

		const uint32_t good_list[] = { 2, 5, 9 };
		const uint32_t unsorted_list[] = { 5, 2, 9 };
		const uint32_t duplicate_list[] = { 2, 5, 5 };
		const uint32_t out_of_range_list[] = { 2, 12 };

		struct invalid_case
		{
			const uint32_t* list;
			uint32_t count;
			const char* label;
		};

		const invalid_case cases[] =
		{
			{ nullptr, 2, "null pointer with a non-zero count" },
			{ unsorted_list, 3, "an unsorted list" },
			{ duplicate_list, 3, "a duplicate entry" },
			{ out_of_range_list, 2, "an out of range entry" },
		};

		for (const invalid_case& test_case : cases)
		{
			compression_settings settings = make_test_settings(metric);
			settings.keyframe_stripping.proportion = 0.5F;
			settings.keyframe_stripping.protected_keyframes = test_case.list;
			settings.keyframe_stripping.num_protected_keyframes = test_case.count;

			output_stats stats;
			compressed_tracks* compressed = nullptr;
			const error_result result = compress_track_list(allocator, tracks, settings, compressed, stats);
			CHECK(result.any(), "expected an error for %s\n", test_case.label);
			CHECK(compressed == nullptr, "invalid input %s produced partial output\n", test_case.label);
		}

		// A zero count ignores the pointer, even when the pointer is null.
		compression_settings ignored_settings = make_test_settings(metric);
		ignored_settings.keyframe_stripping.proportion = 0.5F;
		ignored_settings.keyframe_stripping.protected_keyframes = nullptr;
		ignored_settings.keyframe_stripping.num_protected_keyframes = 0;
		compressed_tracks* ignored_output = compress_or_fail(allocator, tracks, ignored_settings, "zero count ignores pointer");
		free_tracks(allocator, ignored_output);

		// The valid list compresses and decompresses.
		compression_settings valid_settings = make_test_settings(metric);
		valid_settings.keyframe_stripping.proportion = 0.5F;
		valid_settings.keyframe_stripping.protected_keyframes = good_list;
		valid_settings.keyframe_stripping.num_protected_keyframes = 3;
		compressed_tracks* valid_output = compress_or_fail(allocator, tracks, valid_settings, "valid list");
		check_decompression_roundtrip(allocator, *valid_output, 4, 12);
		free_tracks(allocator, valid_output);
	}

	void test_empty_list_legacy_identity(iallocator& allocator, vqvf_transform_error_metric& metric)
	{
		track_array_vqvf tracks = make_procedural_clip(allocator, 5, 30, 9.0F);

		// The legacy configuration never touches the protected fields.
		compression_settings legacy_settings = make_test_settings(metric);
		legacy_settings.keyframe_stripping.proportion = 0.6F;
		legacy_settings.keyframe_stripping.strip_trivial = true;

		// A dangling non-null pointer with a zero count must be ignored completely.
		const uint32_t dangling_list[] = { 3, 11, 22 };
		compression_settings empty_settings = legacy_settings;
		empty_settings.keyframe_stripping.protected_keyframes = dangling_list;
		empty_settings.keyframe_stripping.num_protected_keyframes = 0;

		// The full settings hash stays identical.
		CHECK(legacy_settings.get_hash() == empty_settings.get_hash(),
			"an empty protected list changed the settings hash\n");

		compressed_tracks* legacy_clip = compress_or_fail(allocator, tracks, legacy_settings, "legacy no list");
		compressed_tracks* empty_clip = compress_or_fail(allocator, tracks, empty_settings, "empty list pointer");

		// The compressed byte stream stays identical byte for byte.
		CHECK(legacy_clip->get_size() == empty_clip->get_size(),
			"an empty protected list changed the compressed size\n");
		const bool bytes_identical =
			std::memcmp(legacy_clip, empty_clip, legacy_clip->get_size()) == 0;
		CHECK(bytes_identical, "an empty protected list changed the compressed bytes\n");

		free_tracks(allocator, legacy_clip);
		free_tracks(allocator, empty_clip);
	}

	void test_scalar_and_database_rejection(iallocator& allocator, vqvf_transform_error_metric& metric)
	{
		// Scalar tracks reject a non-empty protected list explicitly.
		{
			track_desc_scalarf desc;
			desc.output_index = 0;
			desc.precision = k_precision;

			track_float1f scalar_track = track_float1f::make_reserve(desc, allocator, 8, k_sample_rate);
			for (uint32_t frame_index = 0; frame_index < 8; ++frame_index)
				scalar_track[frame_index] = float(frame_index);

			track_array_float1f scalar_tracks(allocator, 1);
			scalar_tracks[0] = std::move(scalar_track);

			compression_settings settings = get_default_compression_settings();
			settings.error_metric = &metric;
			const uint32_t protected_list[] = { 3 };
			settings.keyframe_stripping.protected_keyframes = protected_list;
			settings.keyframe_stripping.num_protected_keyframes = 1;

			output_stats stats;
			compressed_tracks* compressed = nullptr;
			const error_result result = compress_track_list(allocator, scalar_tracks, settings, compressed, stats);
			CHECK(result.any(), "scalar tracks with protected keyframes must fail\n");
			CHECK(compressed == nullptr, "scalar rejection produced partial output\n");
		}

		// Database support rejects a non-empty protected list through is_valid and the entry point.
		{
			track_array_vqvf tracks = make_procedural_clip(allocator, 4, 16, 1.0F);
			const uint32_t protected_list[] = { 4 };

			compression_settings settings = make_test_settings(metric);
			settings.enable_database_support = true;
			settings.keyframe_stripping.protected_keyframes = protected_list;
			settings.keyframe_stripping.num_protected_keyframes = 1;
			CHECK(settings.is_valid().any(), "database support with protected keyframes must be invalid\n");

			output_stats stats;
			compressed_tracks* compressed = nullptr;
			const error_result result = compress_track_list(allocator, tracks, settings, compressed, stats);
			CHECK(result.any(), "database support with protected keyframes must fail at entry\n");
			CHECK(compressed == nullptr, "database rejection produced partial output\n");

			// An empty list does not add the database restriction. Disable ordinary stripping
			// because the pre-existing rule already forbids stripping with database support.
			compression_settings empty_settings = make_test_settings(metric);
			empty_settings.enable_database_support = true;
			empty_settings.keyframe_stripping.proportion = 0.0F;
			empty_settings.keyframe_stripping.threshold = 0.0F;
			empty_settings.keyframe_stripping.strip_trivial = false;
			empty_settings.keyframe_stripping.protected_keyframes = nullptr;
			empty_settings.keyframe_stripping.num_protected_keyframes = 0;
			CHECK(empty_settings.is_valid().empty(), "an empty protected list must not restrict database support\n");
			compressed_tracks* database_output = compress_or_fail(allocator, tracks, empty_settings, "empty list with database");
			if (database_output != nullptr)
				free_tracks(allocator, database_output);
		}
	}

	void test_hash_identity()
	{
		const uint32_t list_a[] = { 1, 7, 14, 22 };
		const uint32_t list_b[] = { 1, 7, 14, 22 };
		const uint32_t different_list[] = { 1, 7, 15, 22 };

		compression_keyframe_stripping_settings settings_a;
		settings_a.proportion = 0.4F;
		settings_a.protected_keyframes = list_a;
		settings_a.num_protected_keyframes = 4;

		compression_keyframe_stripping_settings settings_b;
		settings_b.proportion = 0.4F;
		settings_b.protected_keyframes = list_b;	// Same values, different address
		settings_b.num_protected_keyframes = 4;

		compression_keyframe_stripping_settings settings_different;
		settings_different.proportion = 0.4F;
		settings_different.protected_keyframes = different_list;
		settings_different.num_protected_keyframes = 4;

		CHECK(settings_a.get_hash() == settings_b.get_hash(), "equal content at different addresses must hash equally\n");
		CHECK(settings_a.get_hash() != settings_different.get_hash(), "different content must hash differently\n");

		// An empty list keeps the legacy hash regardless of the pointer.
		compression_keyframe_stripping_settings legacy_settings;
		legacy_settings.proportion = 0.4F;
		legacy_settings.protected_keyframes = nullptr;
		legacy_settings.num_protected_keyframes = 0;

		compression_keyframe_stripping_settings null_with_count_settings = legacy_settings;
		null_with_count_settings.protected_keyframes = list_a;	// Ignored because the count is zero
		null_with_count_settings.num_protected_keyframes = 0;

		CHECK(legacy_settings.get_hash() == null_with_count_settings.get_hash(), "an empty list must keep the legacy hash\n");
	}

	void test_multi_segment_retained_bitmap(iallocator& allocator, vqvf_transform_error_metric& metric)
	{
		// 48 frames exceed the 32 sample contributing-error limit and cross a segment junction.
		// The default segmenting balances them into two segments of 24 frames, split between
		// frame 23 (segment 0 last sample) and frame 24 (segment 1 first sample).
		constexpr uint32_t num_frames = 48;
		constexpr uint32_t num_bones = 6;
		track_array_vqvf tracks = make_procedural_clip(allocator, num_bones, num_frames, 2.0F);

		// Interior frames on both segments, both sides of the segment junction (23 and 24), and
		// an index past frame 32 are covered.
		const uint32_t protected_list[] = { 5, 16, 23, 24, 31, 40 };
		constexpr uint32_t num_protected = sizeof(protected_list) / sizeof(protected_list[0]);

		compression_settings plain_settings = make_test_settings(metric);
		compressed_tracks* plain = compress_or_fail(allocator, tracks, plain_settings, "multi-segment plain");

		compression_settings protected_settings = make_test_settings(metric);
		protected_settings.keyframe_stripping.proportion = 0.9F;	// Saturate the proportion cap
		protected_settings.keyframe_stripping.strip_trivial = true;
		protected_settings.keyframe_stripping.threshold = 0.01F;
		protected_settings.keyframe_stripping.protected_keyframes = protected_list;
		protected_settings.keyframe_stripping.num_protected_keyframes = num_protected;
		protected_settings.metadata.include_contributing_error = true;
		compressed_tracks* protected_clip = compress_or_fail(allocator, tracks, protected_settings, "multi-segment protected");

		compression_settings unprotected_settings = protected_settings;
		unprotected_settings.keyframe_stripping.protected_keyframes = nullptr;
		unprotected_settings.keyframe_stripping.num_protected_keyframes = 0;
		compressed_tracks* unprotected_clip = compress_or_fail(allocator, tracks, unprotected_settings, "multi-segment unprotected");

		const retained_clip_info protected_info = read_retained_info(*protected_clip);
		const retained_clip_info unprotected_info = read_retained_info(*unprotected_clip);

		// 48 frames exceed the 32 sample contributing-error limit and cross a segment junction.
		// The default segmenting balances them into two segments of 24 frames.
		CHECK(protected_info.num_segments >= 2, "expected multiple segments, got %u\n", protected_info.num_segments);
		CHECK(num_frames > 32, "the clip must contain more than 32 frames\n");

		// Every protected frame must be present in the actual hard keyframe bit set.
		for (uint32_t index = 0; index < num_protected; ++index)
		{
			const uint32_t frame = protected_list[index];
			CHECK(is_frame_retained(protected_info, frame), "protected frame %u was stripped\n", frame);
		}

		// Protection must retain at least one frame that the unconstrained run removes.
		bool protection_changed_bitmap = false;
		for (uint32_t frame = 0; frame < num_frames; ++frame)
		{
			if (is_frame_retained(protected_info, frame) && !is_frame_retained(unprotected_info, frame))
			{
				protection_changed_bitmap = true;
				break;
			}
		}
		CHECK(protection_changed_bitmap, "protection did not retain any frame that the plain strip removed\n");

		// The unconstrained run still removes redundant frames and saves space versus no stripping.
		uint32_t num_unprotected_retained = 0;
		for (uint32_t frame = 0; frame < num_frames; ++frame)
			if (is_frame_retained(unprotected_info, frame))
				++num_unprotected_retained;
		CHECK(num_unprotected_retained < num_frames, "unprotected redundant frames were not stripped\n");
		CHECK(unprotected_clip->get_size() < plain->get_size(), "stripping did not reduce the clip size\n");
		CHECK(protected_clip->get_size() >= unprotected_clip->get_size(), "protection cannot make the clip smaller\n");

		// The metadata keeps a complete entry per frame. The clip wide array is sorted in the
		// cross-segment stripping order, so every finite error entry comes before every infinite
		// error entry. Protected frames join the segment boundaries in the non-removable tail.
		const keyframe_stripping_metadata_t* metadata = read_contributing_metadata(*protected_clip);
		uint32_t num_finite_entries = 0;
		for (uint32_t frame = 0; frame < num_frames; ++frame)
		{
			const keyframe_stripping_metadata_t* entry = nullptr;
			for (uint32_t metadata_index = 0; metadata_index < num_frames; ++metadata_index)
			{
				if (metadata[metadata_index].keyframe_index == frame)
				{
					entry = &metadata[metadata_index];
					break;
				}
			}
			CHECK(entry != nullptr, "frame %u has no contributing error entry\n", frame);
			if (entry != nullptr && !std::isinf(entry->stripping_error))
				++num_finite_entries;
		}

		// The sorted array must keep all finite errors ahead of all infinite errors.
		for (uint32_t metadata_index = 1; metadata_index < num_frames; ++metadata_index)
		{
			const bool prev_infinite = std::isinf(metadata[metadata_index - 1].stripping_error);
			const bool current_finite = !std::isinf(metadata[metadata_index].stripping_error);
			CHECK(!(prev_infinite && current_finite), "a finite entry follows an infinite tail entry at index %u\n", metadata_index);
		}

		for (uint32_t index = 0; index < num_protected; ++index)
		{
			const uint32_t frame = protected_list[index];
			for (uint32_t metadata_index = 0; metadata_index < num_frames; ++metadata_index)
			{
				if (metadata[metadata_index].keyframe_index == frame)
				{
					// The protected frame stays in the non-removable tail: infinite error, not trivial,
					// and positioned after every removable finite-error entry.
					CHECK(metadata_index >= num_finite_entries, "protected frame %u is not in the non-removable tail\n", frame);
					CHECK(std::isinf(metadata[metadata_index].stripping_error), "protected frame %u does not have infinite error\n", frame);
					CHECK(metadata[metadata_index].is_keyframe_trivial == 0, "protected frame %u is marked trivial\n", frame);
					break;
				}
			}
		}

		check_decompression_roundtrip(allocator, *protected_clip, num_bones, num_frames);

		free_tracks(allocator, plain);
		free_tracks(allocator, protected_clip);
		free_tracks(allocator, unprotected_clip);
	}

	void test_proportion_threshold_and_trivial_caps(iallocator& allocator, vqvf_transform_error_metric& metric)
	{
		constexpr uint32_t num_frames = 40;
		constexpr uint32_t num_bones = 5;
		track_array_vqvf tracks = make_procedural_clip(allocator, num_bones, num_frames, 3.0F);

		const uint32_t protected_list[] = { 4, 12, 25, 33 };
		constexpr uint32_t num_protected = sizeof(protected_list) / sizeof(protected_list[0]);

		struct cap_case
		{
			float proportion;
			float threshold;
			bool strip_trivial;
			const char* label;
		};

		const cap_case cases[] =
		{
			{ 1.0F, 0.0F, false, "saturated proportion" },
			{ 0.0F, 0.005F, false, "threshold only" },
			{ 0.0F, 0.0F, true, "strip trivial only" },
		};

		for (const cap_case& test_case : cases)
		{
			compression_settings settings = make_test_settings(metric);
			settings.keyframe_stripping.proportion = test_case.proportion;
			settings.keyframe_stripping.threshold = test_case.threshold;
			settings.keyframe_stripping.strip_trivial = test_case.strip_trivial;
			settings.keyframe_stripping.protected_keyframes = protected_list;
			settings.keyframe_stripping.num_protected_keyframes = num_protected;

			compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, test_case.label);
			const retained_clip_info info = read_retained_info(*compressed);

			for (uint32_t index = 0; index < num_protected; ++index)
				CHECK(is_frame_retained(info, protected_list[index]), "%s stripped protected frame %u\n", test_case.label, protected_list[index]);

			check_decompression_roundtrip(allocator, *compressed, num_bones, num_frames);
			free_tracks(allocator, compressed);
		}
	}

	void test_single_frame_and_full_protection(iallocator& allocator, vqvf_transform_error_metric& metric)
	{
		// A single frame clip with its only frame protected.
		{
			track_array_vqvf tracks = make_procedural_clip(allocator, 3, 1, 4.0F);
			const uint32_t protected_list[] = { 0 };

			compression_settings settings = make_test_settings(metric);
			settings.keyframe_stripping.proportion = 0.8F;
			settings.keyframe_stripping.protected_keyframes = protected_list;
			settings.keyframe_stripping.num_protected_keyframes = 1;

			compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "single frame");
			check_decompression_roundtrip(allocator, *compressed, 3, 1);
			free_tracks(allocator, compressed);
		}

		// Every frame protected, including the non-movable boundary frames.
		{
			constexpr uint32_t num_frames = 14;
			track_array_vqvf tracks = make_procedural_clip(allocator, 4, num_frames, 5.0F);
			std::vector<uint32_t> protected_list;
			for (uint32_t frame = 0; frame < num_frames; ++frame)
				protected_list.push_back(frame);

			compression_settings settings = make_test_settings(metric);
			settings.keyframe_stripping.proportion = 0.9F;
			settings.keyframe_stripping.protected_keyframes = protected_list.data();
			settings.keyframe_stripping.num_protected_keyframes = uint32_t(protected_list.size());

			compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "all frames protected");
			const retained_clip_info info = read_retained_info(*compressed);
			for (uint32_t frame = 0; frame < num_frames; ++frame)
				CHECK(is_frame_retained(info, frame), "fully protected frame %u was stripped\n", frame);
			check_decompression_roundtrip(allocator, *compressed, 4, num_frames);
			free_tracks(allocator, compressed);
		}

		// Every movable (interior) frame protected. Boundaries are already non-movable.
		{
			constexpr uint32_t num_frames = 12;
			track_array_vqvf tracks = make_procedural_clip(allocator, 4, num_frames, 6.0F);
			std::vector<uint32_t> protected_list;
			for (uint32_t frame = 1; frame < num_frames - 1; ++frame)
				protected_list.push_back(frame);

			compression_settings settings = make_test_settings(metric);
			settings.keyframe_stripping.proportion = 0.9F;
			settings.keyframe_stripping.strip_trivial = true;
			settings.keyframe_stripping.protected_keyframes = protected_list.data();
			settings.keyframe_stripping.num_protected_keyframes = uint32_t(protected_list.size());

			compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "all movable frames protected");
			const retained_clip_info info = read_retained_info(*compressed);
			for (uint32_t frame = 0; frame < num_frames; ++frame)
				CHECK(is_frame_retained(info, frame), "movable-protected frame %u was stripped\n", frame);
			check_decompression_roundtrip(allocator, *compressed, 4, num_frames);
			free_tracks(allocator, compressed);
		}
	}

	void test_loop_optimization_mapping(iallocator& allocator, vqvf_transform_error_metric& metric)
	{
		constexpr uint32_t num_frames = 20;
		constexpr uint32_t num_bones = 4;

		// Build a clip whose last sample exactly repeats the first sample.
		track_array_vqvf looping_tracks(allocator, num_bones);
		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
		{
			track_desc_transformf desc;
			desc.output_index = bone_index;
			desc.parent_index = bone_index == 0 ? k_invalid_track_index : (bone_index - 1);
			desc.precision = k_precision;

			track_vqvf track = track_vqvf::make_reserve(desc, allocator, num_frames, k_sample_rate);
			for (uint32_t frame_index = 0; frame_index < num_frames; ++frame_index)
			{
				const uint32_t source_frame = frame_index == num_frames - 1 ? 0 : frame_index;
				const float y = 0.4F * std::sin(float(source_frame) * 0.6F + float(bone_index));
				track[frame_index] = rtm::vqv_set(rtm::quat_identity(),
					rtm::vector_set(float(source_frame) * 0.03F, y, 0.0F), rtm::vector_set(1.0F));
			}
			looping_tracks[bone_index] = std::move(track);
		}

		// The original last frame maps to the retained frame 0, and frame 0 maps there too.
		const uint32_t protected_list[] = { 0, 8, num_frames - 1 };
		constexpr uint32_t num_protected = sizeof(protected_list) / sizeof(protected_list[0]);

		compression_settings settings = make_test_settings(metric);
		settings.optimize_loops = true;
		settings.keyframe_stripping.proportion = 0.7F;
		settings.keyframe_stripping.protected_keyframes = protected_list;
		settings.keyframe_stripping.num_protected_keyframes = num_protected;

		compressed_tracks* looping_clip = compress_or_fail(allocator, looping_tracks, settings, "looping protected");

		// Loop optimization removed one sample.
		CHECK(looping_clip->get_num_samples_per_track() == num_frames - 1, "loop optimization did not remove the repeating sample\n");
		const retained_clip_info info = read_retained_info(*looping_clip);

		// Frame 0 holds both original frame 0 and the remapped original last frame.
		CHECK(is_frame_retained(info, 0), "remapped loop endpoint frame 0 was stripped\n");
		CHECK(is_frame_retained(info, 8), "protected interior loop frame was stripped\n");
		check_decompression_roundtrip(allocator, *looping_clip, num_bones, num_frames - 1);
		free_tracks(allocator, looping_clip);

		// A non-looping clip does not remap: its last frame stays a valid protected index.
		track_array_vqvf non_looping = make_procedural_clip(allocator, num_bones, num_frames, 7.0F);
		const uint32_t last_protected[] = { num_frames - 1 };
		compression_settings non_looping_settings = make_test_settings(metric);
		non_looping_settings.optimize_loops = true;
		non_looping_settings.keyframe_stripping.proportion = 0.7F;
		non_looping_settings.keyframe_stripping.protected_keyframes = last_protected;
		non_looping_settings.keyframe_stripping.num_protected_keyframes = 1;

		compressed_tracks* non_looping_clip = compress_or_fail(allocator, non_looping, non_looping_settings, "non looping last frame");
		CHECK(non_looping_clip->get_num_samples_per_track() == num_frames, "a non-looping clip must keep every sample\n");
		const retained_clip_info non_looping_info = read_retained_info(*non_looping_clip);
		CHECK(is_frame_retained(non_looping_info, num_frames - 1), "the non-looping last frame must be retained\n");
		check_decompression_roundtrip(allocator, *non_looping_clip, num_bones, num_frames);
		free_tracks(allocator, non_looping_clip);
	}

	void test_nonlinear_endpoint_change(iallocator& allocator, vqvf_transform_error_metric& metric)
	{
		constexpr uint32_t num_frames = 9;
		constexpr uint32_t contact_frame = 3;
		constexpr uint32_t num_bones = 3;
		constexpr float k_error_tolerance = 1.0e-6F;

		track_array_vqvf tracks = make_contact_clip(allocator, num_frames, contact_frame);

		// Independent reference reconstruction, without and with a protected contact frame.
		const uint32_t protected_list[] = { contact_frame };
		const std::vector<simulated_step> unconstrained_steps =
			simulate_greedy_stripping(tracks, nullptr, 0, false);
		const std::vector<simulated_step> protected_steps =
			simulate_greedy_stripping(tracks, protected_list, 1, true);

		CHECK(!unconstrained_steps.empty(), "the unconstrained simulator produced no steps\n");
		CHECK(!protected_steps.empty(), "the protected simulator produced no steps\n");

		// The protected simulator never removes the contact frame.
		for (const simulated_step& step : protected_steps)
			CHECK(step.removed_frame != contact_frame, "the simulator removed the protected frame\n");

		// The flat contact plant reconstructs exactly, so it is the cheapest first removal and
		// the unconstrained process removes it first.
		CHECK(unconstrained_steps.front().removed_frame == contact_frame,
			"the unconstrained first removal is %u not the contact frame %u\n",
			unconstrained_steps.front().removed_frame, contact_frame);
		CHECK(unconstrained_steps.front().error <= k_error_tolerance,
			"the contact frame reconstruction is not exact, error=%f\n", unconstrained_steps.front().error);

		// The protected process cannot remove the contact frame, so its first removal is a
		// different candidate with a strictly larger measured error.
		CHECK(protected_steps.front().removed_frame != contact_frame, "the protected simulator removed the contact frame first\n");
		CHECK(protected_steps.front().error > unconstrained_steps.front().error + k_error_tolerance,
			"protection did not raise the first removal error\n");

		// At the very first step the reconstruction endpoint pair and the measured error change.
		const simulated_step& unconstrained_first = unconstrained_steps.front();
		const simulated_step& protected_first = protected_steps.front();
		const bool endpoint_changed =
			unconstrained_first.start_frame != protected_first.start_frame ||
			unconstrained_first.end_frame != protected_first.end_frame;
		CHECK(endpoint_changed, "the first-step reconstruction endpoints did not change\n");
		CHECK(protected_first.error != unconstrained_first.error, "the first-step re-sequenced error did not change\n");

		// The contact frame acts as an interpolation endpoint for a neighbor candidate on each
		// side in the protected run. A left side candidate ends on it, a right side one starts on it.
		bool contact_is_left_endpoint = false;
		bool contact_is_right_endpoint = false;
		for (const simulated_step& step : protected_steps)
		{
			if (step.end_frame == contact_frame)
				contact_is_left_endpoint = true;
			if (step.start_frame == contact_frame)
				contact_is_right_endpoint = true;
		}
		CHECK(contact_is_left_endpoint, "the protected frame is never a right interpolation endpoint for a left neighbor\n");
		CHECK(contact_is_right_endpoint, "the protected frame is never a left interpolation endpoint for a right neighbor\n");

		// Cross-check the real first removal order against the independent simulator for both runs.
		compression_settings unprotected_settings = make_test_settings(metric);
		unprotected_settings.keyframe_stripping.proportion = 0.9F;
		unprotected_settings.metadata.include_contributing_error = true;
		compressed_tracks* unprotected_clip = compress_or_fail(allocator, tracks, unprotected_settings, "nonlinear unprotected");

		compression_settings protected_settings = make_test_settings(metric);
		protected_settings.keyframe_stripping.proportion = 0.9F;
		protected_settings.keyframe_stripping.protected_keyframes = protected_list;
		protected_settings.keyframe_stripping.num_protected_keyframes = 1;
		protected_settings.metadata.include_contributing_error = true;
		compressed_tracks* protected_clip = compress_or_fail(allocator, tracks, protected_settings, "nonlinear protected");

		const keyframe_stripping_metadata_t* unprotected_metadata =
			read_contributing_metadata(*unprotected_clip);
		const keyframe_stripping_metadata_t* protected_metadata =
			read_contributing_metadata(*protected_clip);

		// Removal order cross-check: the real unconstrained first strip must match the simulator.
		CHECK(unprotected_metadata[0].stripping_index == 0, "the first unprotected metadata entry is not the first strip\n");
		CHECK(unprotected_metadata[0].keyframe_index == unconstrained_steps.front().removed_frame,
			"real first removal %u does not match the simulator %u\n",
			unprotected_metadata[0].keyframe_index, unconstrained_steps.front().removed_frame);

		// The real protected first strip cannot be the contact frame, and it matches the
		// independent simulator first pick.
		CHECK(protected_metadata[0].stripping_index == 0, "the first protected metadata entry is not the first strip\n");
		CHECK(protected_metadata[0].keyframe_index != contact_frame, "the real protected run removed the contact frame\n");
		CHECK(protected_metadata[0].keyframe_index == protected_steps.front().removed_frame,
			"real protected first removal %u does not match the simulator %u\n",
			protected_metadata[0].keyframe_index, protected_steps.front().removed_frame);

		// The protected contact frame is in the non-removable tail of the real metadata:
		// it carries an infinite error, it is not trivial, and it follows every finite entry.
		const uint32_t protected_num_samples = protected_clip->get_num_samples_per_track();
		uint32_t protected_num_finite = 0;
		for (uint32_t index = 0; index < protected_num_samples; ++index)
			if (!std::isinf(protected_metadata[index].stripping_error))
				++protected_num_finite;

		bool found_protected_tail = false;
		for (uint32_t index = 0; index < protected_num_samples; ++index)
		{
			if (protected_metadata[index].keyframe_index == contact_frame)
			{
				found_protected_tail =
					index >= protected_num_finite &&
					std::isinf(protected_metadata[index].stripping_error) &&
					protected_metadata[index].is_keyframe_trivial == 0;
				break;
			}
		}
		CHECK(found_protected_tail, "the real metadata does not place the contact frame in the non-removable tail\n");

		const retained_clip_info protected_info = read_retained_info(*protected_clip);
		CHECK(is_frame_retained(protected_info, contact_frame), "the protected contact frame was stripped\n");
		check_decompression_roundtrip(allocator, *protected_clip, num_bones, num_frames);

		free_tracks(allocator, unprotected_clip);
		free_tracks(allocator, protected_clip);
	}
}

int main()
{
	ansi_allocator allocator;
	vqvf_transform_error_metric metric;

	test_hash_identity();
	test_validation_errors(allocator, metric);
	test_empty_list_legacy_identity(allocator, metric);
	test_scalar_and_database_rejection(allocator, metric);
	test_multi_segment_retained_bitmap(allocator, metric);
	test_proportion_threshold_and_trivial_caps(allocator, metric);
	test_single_frame_and_full_protection(allocator, metric);
	test_loop_optimization_mapping(allocator, metric);
	test_nonlinear_endpoint_change(allocator, metric);

	if (g_failures != 0)
	{
		std::printf("ACL_PROTECTED_TARGET_FAILED failures=%d\n", g_failures);
		return 1;
	}

	std::printf("ACL_PROTECTED_TARGET_PASS\n");
	return 0;
}
