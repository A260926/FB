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

// Standalone target test for protected keyframes in transform track stripping.
// It drives the real compress_track_list entry point and the real
// decompression_context. It also contains an independent, step by step greedy
// removal simulator so that the removal order is verified against a separate
// reconstruction instead of against a patched final bit mask.

#include <acl/compression/compress.h>
#include <acl/compression/track_array.h>
#include <acl/compression/transform_error_metrics.h>
#include <acl/core/ansi_allocator.h>
#include <acl/core/impl/compressed_headers.h>
#include <acl/core/impl/compressed_tracks.impl.h>
#include <acl/core/impl/debug_track_writer.h>
#include <acl/decompression/decompress.h>
#include <acl/math/quatf.h>

#include <rtm/quatf.h>
#include <rtm/scalarf.h>
#include <rtm/vector4f.h>
#include <rtm/vqvf.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace
{
	using namespace acl;
	using namespace acl::acl_impl;

	uint32_t g_failures = 0;

	void report_failure(const char* file, int line, const char* format, ...)
	{
		++g_failures;
		std::printf("FAIL %s:%d: ", file, line);
		va_list args;
		va_start(args, format);
		std::vprintf(format, args);
		va_end(args);
		std::printf("\n");
	}

	#define REQUIRE(condition, ...) \
		do { if (!(condition)) { report_failure(__FILE__, __LINE__, __VA_ARGS__); } } while (0)

	// A programmatic transform clip stored as flat local space vqvf samples.
	struct program_clip
	{
		uint32_t num_bones = 0;
		uint32_t num_frames = 0;
		float sample_rate = 30.0F;
		std::vector<rtm::vqvf> samples;				// [frame * num_bones + bone]
		std::vector<uint32_t> parents;				// num_bones entries
		std::vector<float> precision;				// num_bones entries
		std::vector<float> shell_distance;			// num_bones entries

		const rtm::vqvf& at(uint32_t frame, uint32_t bone) const { return samples[frame * num_bones + bone]; }
		rtm::vqvf& at(uint32_t frame, uint32_t bone) { return samples[frame * num_bones + bone]; }
	};

	rtm::vqvf make_pose(float angle, float tx, float ty, float tz)
	{
		const rtm::vector4f axis = rtm::vector_set(0.0F, 1.0F, 0.0F);
		const rtm::quatf rotation = rtm::quat_from_axis_angle(axis, angle);
		return rtm::vqv_set(rotation, rtm::vector_set(tx, ty, tz), rtm::vector_set(1.0F));
	}

	// Small, deliberately nonlinear clip. The changing curvature keeps the
	// reconstruction error and the removal order sensitive to the endpoints.
	program_clip make_nonlinear_clip(uint32_t num_frames, uint32_t num_bones)
	{
		program_clip clip;
		clip.num_bones = num_bones;
		clip.num_frames = num_frames;
		clip.samples.resize(size_t(num_frames) * num_bones);
		clip.parents.resize(num_bones);
		clip.precision.resize(num_bones, 0.01F);
		clip.shell_distance.resize(num_bones, 3.0F);

		for (uint32_t bone = 0; bone < num_bones; ++bone)
		{
			clip.parents[bone] = bone == 0 ? k_invalid_track_index : (bone - 1);

			const float phase = float(bone) * 1.7F;
			const float freq = 0.35F + float(bone) * 0.07F;
			for (uint32_t frame = 0; frame < num_frames; ++frame)
			{
				const float f = float(frame);
				const float angle = 0.20F * std::sin(f * freq + phase) + 0.08F * std::sin(f * 0.93F + phase * 0.5F);
				const float tx = 0.05F * f + 0.35F * std::sin(f * 0.81F + phase);
				const float ty = 0.25F * std::cos(f * 0.67F + phase) + 0.10F * std::sin(f * 1.37F);
				const float tz = 0.15F * std::sin(f * 0.53F + phase * 0.4F);
				clip.at(frame, bone) = make_pose(angle, tx + float(bone) * 0.5F, ty, tz);
			}
		}

		return clip;
	}

	// Smooth, mostly linear clip with many redundant frames so that stripping
	// really saves space.
	program_clip make_smooth_clip(uint32_t num_frames, uint32_t num_bones)
	{
		program_clip clip;
		clip.num_bones = num_bones;
		clip.num_frames = num_frames;
		clip.samples.resize(size_t(num_frames) * num_bones);
		clip.parents.resize(num_bones);
		clip.precision.resize(num_bones, 0.001F);
		clip.shell_distance.resize(num_bones, 3.0F);

		for (uint32_t bone = 0; bone < num_bones; ++bone)
		{
			clip.parents[bone] = bone == 0 ? k_invalid_track_index : (bone - 1);

			for (uint32_t frame = 0; frame < num_frames; ++frame)
			{
				const float f = float(frame);
				const float angle = 0.02F * f + 0.01F * std::sin(f * 0.2F + float(bone));
				const float tx = 0.04F * f + float(bone) * 0.2F;
				const float ty = 0.02F * std::sin(f * 0.15F + float(bone));
				const float tz = 0.01F * f;
				clip.at(frame, bone) = make_pose(angle, tx, ty, tz);
			}
		}

		return clip;
	}

	track_array_vqvf make_track_array(ansi_allocator& allocator, const program_clip& clip)
	{
		track_array_vqvf tracks(allocator, clip.num_bones);
		for (uint32_t bone = 0; bone < clip.num_bones; ++bone)
		{
			track_desc_transformf desc;
			desc.output_index = bone;
			desc.parent_index = clip.parents[bone];
			desc.precision = clip.precision[bone];
			desc.shell_distance = clip.shell_distance[bone];

			track_vqvf track = track_vqvf::make_reserve(desc, allocator, clip.num_frames, clip.sample_rate);
			for (uint32_t frame = 0; frame < clip.num_frames; ++frame)
				track[frame] = clip.at(frame, bone);

			tracks[bone] = std::move(track);
		}

		return tracks;
	}

	compression_settings make_base_settings(vqvf_transform_error_metric& metric, bool optimize_loops)
	{
		compression_settings settings = get_default_compression_settings();
		settings.error_metric = &metric;
		settings.optimize_loops = optimize_loops;
		settings.keyframe_stripping.proportion = 0.0F;
		settings.keyframe_stripping.threshold = 0.0F;
		settings.keyframe_stripping.strip_trivial = false;
		return settings;
	}

	compressed_tracks* compress_or_fail(ansi_allocator& allocator, const track_array_vqvf& tracks, const compression_settings& settings, const char* tag)
	{
		output_stats stats;
		compressed_tracks* out = nullptr;
		const error_result result = compress_track_list(allocator, tracks, settings, out, stats);
		REQUIRE(result.empty(), "compression (%s) returned an error: %s", tag, result.c_str());
		REQUIRE(out != nullptr, "compression (%s) produced no output", tag);
		return out;
	}

	bool expect_compression_error(ansi_allocator& allocator, const track_array& tracks, const compression_settings& settings, const char* tag)
	{
		output_stats stats;
		compressed_tracks* out = nullptr;
		const error_result result = compress_track_list(allocator, tracks, settings, out, stats);
		REQUIRE(result.any(), "compression (%s) was expected to fail but it succeeded", tag);
		REQUIRE(out == nullptr, "compression (%s) produced partial output on failure", tag);
		return result.any();
	}

	// Returns the retained (hard) bit of every segment sample in clip order.
	std::vector<bool> read_retained_mask(const compressed_tracks& tracks)
	{
		const uint32_t num_samples = tracks.get_num_samples_per_track();

		// Without stripping every sample is retained and the headers are plain.
		if (!tracks.has_stripped_keyframes())
			return std::vector<bool>(num_samples, true);

		const transform_tracks_header& header = get_transform_tracks_header(tracks);
		const uint32_t num_segments = header.num_segments;
		const stripped_segment_header_t* segment_headers = header.get_stripped_segment_headers();
		const uint32_t* segment_start_indices = num_segments > 1 ? header.get_segment_start_indices() : nullptr;

		std::vector<bool> retained(num_samples, false);
		for (uint32_t segment_index = 0; segment_index < num_segments; ++segment_index)
		{
			const uint32_t start = segment_start_indices != nullptr ? segment_start_indices[segment_index] : 0U;
			const uint32_t end = (segment_start_indices != nullptr && segment_index + 1 < num_segments)
				? segment_start_indices[segment_index + 1]
				: num_samples;
			const uint32_t sample_indices = segment_headers[segment_index].sample_indices;

			for (uint32_t local_index = 0; local_index < (end - start); ++local_index)
			{
				const uint32_t mask = 1U << (31 - local_index);
				retained[start + local_index] = (sample_indices & mask) != 0;
			}
		}

		return retained;
	}

	const keyframe_stripping_metadata_t* read_contributing_error(const compressed_tracks& tracks)
	{
		const optional_metadata_header& metadata_header = get_optional_metadata_header(tracks);
		const uint8_t* raw_entries = metadata_header.get_contributing_error(tracks);
		REQUIRE(raw_entries != nullptr, "contributing error metadata is missing");
		return reinterpret_cast<const keyframe_stripping_metadata_t*>(raw_entries);
	}

	rtm::vqvf decompress_local_pose(ansi_allocator& allocator, const compressed_tracks& tracks, uint32_t bone, uint32_t frame, float sample_rate)
	{
		decompression_context<default_transform_decompression_settings> context;
		REQUIRE(context.initialize(tracks), "decompression context failed to initialize");

		// The constant defaults writer materializes default sub-tracks (for example a
		// unit scale) with their identity default value. This matches the lossy poses that
		// the compressor bit rate database samples while it measures the contributing error.
		debug_track_writer_constant_defaults writer(allocator, track_type8::vqvf, tracks.get_num_tracks());
		context.seek(float(frame) / sample_rate, sample_rounding_policy::nearest);
		context.decompress_tracks(writer);
		return writer.read_vqv(bone);
	}

	//////////////////////////////////////////////////////////////////////////
	// Independent greedy removal simulator for a single segment clip.
	// It reconstructs every candidate from the current retained neighbors and
	// measures the error with the same public error metric as the compressor.
	//////////////////////////////////////////////////////////////////////////
	struct greedy_simulator
	{
		const program_clip& raw;
		const std::vector<rtm::vqvf>& lossy;			// Quantized local poses, [frame * bones + bone]
		const vqvf_transform_error_metric& metric;
		std::vector<rtm::vqvf> raw_object;				// Raw object space poses

		greedy_simulator(const program_clip& raw_, const std::vector<rtm::vqvf>& lossy_, const vqvf_transform_error_metric& metric_)
			: raw(raw_)
			, lossy(lossy_)
			, metric(metric_)
		{
			raw_object.resize(size_t(raw.num_frames) * raw.num_bones);
			std::vector<uint32_t> indices(raw.num_bones);
			for (uint32_t index = 0; index < raw.num_bones; ++index)
				indices[index] = index;

			itransform_error_metric::local_to_object_space_args args;
			args.dirty_transform_indices = indices.data();
			args.num_dirty_transforms = raw.num_bones;
			args.parent_transform_indices = raw.parents.data();
			args.num_transforms = raw.num_bones;

			for (uint32_t frame = 0; frame < raw.num_frames; ++frame)
			{
				args.local_transforms = &raw.samples[frame * raw.num_bones];
				metric.local_to_object_space(args, &raw_object[frame * raw.num_bones]);
			}
		}

		struct candidate_result
		{
			uint32_t frame = 0;
			uint32_t start_frame = 0;
			uint32_t end_frame = 0;
			float error = 0.0F;
			bool trivial = false;
		};

		candidate_result measure_candidate(const std::vector<bool>& retained, uint32_t frame) const
		{
			const uint32_t num_bones = raw.num_bones;
			const uint32_t num_frames = raw.num_frames;

			uint32_t start_frame = frame - 1;
			while (!retained[start_frame])
				--start_frame;

			uint32_t end_frame = frame + 1;
			while (!retained[end_frame])
				++end_frame;

			std::vector<rtm::vqvf> interpolated(num_bones);
			std::vector<rtm::vqvf> object_pose(num_bones);
			std::vector<uint32_t> indices(num_bones);
			for (uint32_t index = 0; index < num_bones; ++index)
				indices[index] = index;

			float max_error_sq = 0.0F;
			bool trivial = true;

			for (uint32_t interp_frame = start_frame + 1; interp_frame < end_frame; ++interp_frame)
			{
				const float alpha = float(interp_frame - start_frame) / float(end_frame - start_frame);

				for (uint32_t bone = 0; bone < num_bones; ++bone)
				{
					const rtm::vqvf& start = lossy[start_frame * num_bones + bone];
					const rtm::vqvf& end = lossy[end_frame * num_bones + bone];
					const rtm::quatf rotation = acl_impl::quat_lerp_stable(start.rotation, end.rotation, alpha);
					const rtm::vector4f translation = rtm::vector_lerp(start.translation, end.translation, alpha);
					const rtm::vector4f scale = rtm::vector_lerp(start.scale, end.scale, alpha);
					interpolated[bone] = rtm::vqv_set(rotation, translation, scale);
				}

				itransform_error_metric::local_to_object_space_args object_args;
				object_args.dirty_transform_indices = indices.data();
				object_args.num_dirty_transforms = num_bones;
				object_args.parent_transform_indices = raw.parents.data();
				object_args.local_transforms = interpolated.data();
				object_args.num_transforms = num_bones;
				metric.local_to_object_space(object_args, object_pose.data());

				for (uint32_t bone = 0; bone < num_bones; ++bone)
				{
					itransform_error_metric::calculate_error_args error_args;
					error_args.construct_sphere_shell(raw.shell_distance[bone]);
					error_args.transform0 = &raw_object[interp_frame * num_bones + bone];
					error_args.transform1 = &object_pose[bone];

					const float error_sq = rtm::scalar_cast(metric.calculate_error_squared(error_args));
					max_error_sq = std::max(max_error_sq, error_sq);
					trivial = trivial && (error_sq <= raw.precision[bone] * raw.precision[bone]);
				}
			}

			candidate_result result;
			result.frame = frame;
			result.start_frame = start_frame;
			result.end_frame = end_frame;
			result.error = std::sqrt(max_error_sq);
			result.trivial = trivial;
			(void)num_frames;
			return result;
		}

		// Runs the greedy removal. Protected frames and segment boundaries stay
		// retained. Returns the removal order and the per-iteration candidate data.
		std::vector<candidate_result> run(const std::vector<uint32_t>& protected_frames, std::vector<uint32_t>& out_order, bool use_protected_tie_break) const
		{
			const uint32_t num_frames = raw.num_frames;
			std::vector<bool> retained(num_frames, true);
			std::vector<bool> is_protected(num_frames, false);
			for (uint32_t frame : protected_frames)
				is_protected[frame] = true;

			std::vector<candidate_result> history;
			out_order.clear();

			const uint32_t first_removable = 1;
			const uint32_t last_removable = num_frames - 1;	// Exclusive upper bound

			while (true)
			{
				bool found = false;
				candidate_result best;
				best.error = std::numeric_limits<float>::infinity();

				for (uint32_t frame = first_removable; frame < last_removable; ++frame)
				{
					if (!retained[frame] || is_protected[frame])
						continue;

					const candidate_result candidate = measure_candidate(retained, frame);
					if (!found || candidate.error < best.error ||
						(use_protected_tie_break && candidate.error == best.error && frame < best.frame))
					{
						best = candidate;
						found = true;
					}
				}

				if (!found)
					break;

				retained[best.frame] = false;
				out_order.push_back(best.frame);
				history.push_back(best);
			}

			return history;
		}
	};

	// Builds the quantized local poses that the compressor measures during the
	// greedy removal: compress without stripping and read every exact sample.
	std::vector<rtm::vqvf> extract_lossy_poses(ansi_allocator& allocator, const track_array_vqvf& tracks, const program_clip& clip, vqvf_transform_error_metric& metric)
	{
		compression_settings settings = make_base_settings(metric, false);
		compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "lossy reference");

		std::vector<rtm::vqvf> lossy(size_t(clip.num_frames) * clip.num_bones);
		for (uint32_t frame = 0; frame < clip.num_frames; ++frame)
			for (uint32_t bone = 0; bone < clip.num_bones; ++bone)
				lossy[frame * clip.num_bones + bone] = decompress_local_pose(allocator, *compressed, bone, frame, clip.sample_rate);

		allocator.deallocate(compressed, compressed->get_size());
		return lossy;
	}

	void test_validation_errors(ansi_allocator& allocator, vqvf_transform_error_metric& metric)
	{
		constexpr uint32_t k_num_frames = 12;
		program_clip clip = make_nonlinear_clip(k_num_frames, 3);
		track_array_vqvf tracks = make_track_array(allocator, clip);

		const uint32_t unsorted[] = { 5, 2 };
		const uint32_t duplicate[] = { 3, 3, 8 };
		const uint32_t out_of_range[] = { k_num_frames };
		const uint32_t valid[] = { 2, 5, 9 };

		struct case_t
		{
			const char* tag;
			const uint32_t* list;
			uint32_t count;
		};

		const case_t cases[] =
		{
			{ "null with non-zero count", nullptr, 1 },
			{ "unsorted list", unsorted, 2 },
			{ "duplicate list", duplicate, 3 },
			{ "out of range list", out_of_range, 1 },
		};

		for (const case_t& test_case : cases)
		{
			compression_settings settings = make_base_settings(metric, false);
			settings.keyframe_stripping.proportion = 0.5F;
			settings.keyframe_stripping.protected_keyframes = test_case.list;
			settings.keyframe_stripping.num_protected_keyframes = test_case.count;
			expect_compression_error(allocator, tracks, settings, test_case.tag);
		}

		// A zero count ignores the pointer, even a non-null invalid one.
		{
			compression_settings settings = make_base_settings(metric, false);
			settings.keyframe_stripping.proportion = 0.5F;
			settings.keyframe_stripping.protected_keyframes = reinterpret_cast<const uint32_t*>(0x1);
			settings.keyframe_stripping.num_protected_keyframes = 0;
			compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "empty list ignores pointer");
			allocator.deallocate(compressed, compressed->get_size());
		}

		// A valid list passes validation and produces a decodable stream.
		{
			compression_settings settings = make_base_settings(metric, false);
			settings.keyframe_stripping.proportion = 0.5F;
			settings.keyframe_stripping.protected_keyframes = valid;
			settings.keyframe_stripping.num_protected_keyframes = 3;
			compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "valid list");
			REQUIRE(compressed->has_stripped_keyframes(), "valid list clip should still strip other frames");
			allocator.deallocate(compressed, compressed->get_size());
		}
	}

	void test_hash_and_byte_compatibility(ansi_allocator& allocator, vqvf_transform_error_metric& metric)
	{
		constexpr uint32_t k_num_frames = 24;
		program_clip clip = make_smooth_clip(k_num_frames, 4);
		track_array_vqvf tracks = make_track_array(allocator, clip);

		const uint32_t list_a[] = { 3, 7, 15, 20 };
		uint32_t list_b_storage[] = { 3, 7, 15, 20 };
		const uint32_t* list_b = list_b_storage;
		const uint32_t different_list[] = { 3, 7, 15, 21 };

		compression_settings settings_a = make_base_settings(metric, false);
		settings_a.keyframe_stripping.proportion = 0.5F;
		settings_a.keyframe_stripping.protected_keyframes = list_a;
		settings_a.keyframe_stripping.num_protected_keyframes = 4;

		compression_settings settings_b = settings_a;
		settings_b.keyframe_stripping.protected_keyframes = list_b;

		REQUIRE(settings_a.keyframe_stripping.get_hash() == settings_b.keyframe_stripping.get_hash(),
			"equal protected lists at different addresses must hash equally");
		REQUIRE(settings_a.get_hash() == settings_b.get_hash(), "full settings hash must ignore the list address");

		compression_settings settings_different = settings_a;
		settings_different.keyframe_stripping.protected_keyframes = different_list;
		REQUIRE(settings_a.keyframe_stripping.get_hash() != settings_different.keyframe_stripping.get_hash(),
			"different protected values must hash differently");

		// An empty list hashes exactly like the old settings without the feature.
		compression_settings settings_empty = make_base_settings(metric, false);
		settings_empty.keyframe_stripping.proportion = 0.5F;
		compression_settings settings_legacy = make_base_settings(metric, false);
		settings_legacy.keyframe_stripping.proportion = 0.5F;
		REQUIRE(settings_empty.keyframe_stripping.get_hash() == settings_legacy.keyframe_stripping.get_hash(),
			"empty protected list must match the legacy hash");
		REQUIRE(settings_empty.get_hash() == settings_legacy.get_hash(), "empty protected list must match the legacy full hash");

		compressed_tracks* compressed_empty = compress_or_fail(allocator, tracks, settings_empty, "empty protected bytes");
		compressed_tracks* compressed_legacy = compress_or_fail(allocator, tracks, settings_legacy, "legacy bytes");
		REQUIRE(compressed_empty->get_size() == compressed_legacy->get_size(),
			"empty protected list must not change the compressed size");
		REQUIRE(compressed_empty->get_hash() == compressed_legacy->get_hash(),
			"empty protected list must not change the compressed bytes");
		allocator.deallocate(compressed_empty, compressed_empty->get_size());
		allocator.deallocate(compressed_legacy, compressed_legacy->get_size());
	}

	void test_scalar_and_database_rejection(ansi_allocator& allocator, vqvf_transform_error_metric& metric)
	{
		// Scalar tracks reject a non-empty protected list explicitly.
		{
			track_desc_scalarf desc;
			desc.output_index = 0;
			desc.precision = 0.001F;

			track_float1f track = track_float1f::make_reserve(desc, allocator, 16, 30.0F);
			for (uint32_t frame = 0; frame < 16; ++frame)
				track[frame] = std::sin(float(frame) * 0.3F);

			track_array_float1f scalar_tracks(allocator, 1);
			scalar_tracks[0] = std::move(track);

			compression_settings settings = get_default_compression_settings();
			settings.error_metric = &metric;
			const uint32_t protected_frames[] = { 4, 9 };
			settings.keyframe_stripping.protected_keyframes = protected_frames;
			settings.keyframe_stripping.num_protected_keyframes = 2;

			expect_compression_error(allocator, scalar_tracks, settings, "scalar protected list");
		}

		// Database support rejects a non-empty protected list.
		{
			program_clip clip = make_smooth_clip(24, 3);
			track_array_vqvf tracks = make_track_array(allocator, clip);

			compression_settings settings = make_base_settings(metric, false);
			settings.enable_database_support = true;
			settings.keyframe_stripping.proportion = 0.0F;
			settings.keyframe_stripping.strip_trivial = false;
			const uint32_t protected_frames[] = { 5, 12 };
			settings.keyframe_stripping.protected_keyframes = protected_frames;
			settings.keyframe_stripping.num_protected_keyframes = 2;

			expect_compression_error(allocator, tracks, settings, "database protected list");

			// An empty list does not trigger the new limitation.
			settings.keyframe_stripping.protected_keyframes = nullptr;
			settings.keyframe_stripping.num_protected_keyframes = 0;
			compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "database empty list");
			allocator.deallocate(compressed, compressed->get_size());
		}
	}

	void test_greedy_order_and_endpoints(ansi_allocator& allocator, vqvf_transform_error_metric& metric)
	{
		constexpr uint32_t k_num_frames = 12;
		constexpr uint32_t k_num_bones = 3;
		program_clip clip = make_nonlinear_clip(k_num_frames, k_num_bones);
		track_array_vqvf tracks = make_track_array(allocator, clip);

		const std::vector<rtm::vqvf> lossy = extract_lossy_poses(allocator, tracks, clip, metric);
		const greedy_simulator simulator(clip, lossy, metric);

		const std::vector<uint32_t> no_protection;

		std::vector<uint32_t> unprotected_order;
		const std::vector<greedy_simulator::candidate_result> unprotected_history =
			simulator.run(no_protection, unprotected_order, false);

		REQUIRE(unprotected_order.size() == k_num_frames - 2, "unprotected run must remove every interior frame");

		// Protect frames that the unconstrained run removes early, but never its very
		// first frame. This keeps the first greedy step identical and forces a real
		// divergence immediately afterwards through recomputation.
		const uint32_t protected_a = unprotected_order[1];
		const uint32_t protected_b = unprotected_order[3];
		const std::vector<uint32_t> protected_frames = { protected_a, protected_b };

		std::vector<uint32_t> protected_order;
		const std::vector<greedy_simulator::candidate_result> protected_history =
			simulator.run(protected_frames, protected_order, true);

		REQUIRE(protected_order.size() == k_num_frames - 2 - uint32_t(protected_frames.size()),
			"protected run must leave every protected frame in place");
		REQUIRE(protected_order[0] == unprotected_order[0], "the first greedy step must stay identical");

		// The protected frames must never appear in the protected removal order.
		for (uint32_t frame : protected_frames)
		{
			const bool removed = std::find(protected_order.begin(), protected_order.end(), frame) != protected_order.end();
			REQUIRE(!removed, "protected frame %u was removed by the simulator", frame);
		}

		// Compress with the contributing error metadata retained and compare the
		// actual finite-error removal order with the independent simulator.
		compression_settings settings = make_base_settings(metric, false);
		settings.keyframe_stripping.proportion = 1.0F;
		settings.metadata.include_contributing_error = true;
		settings.keyframe_stripping.protected_keyframes = protected_frames.data();
		settings.keyframe_stripping.num_protected_keyframes = uint32_t(protected_frames.size());

		compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "greedy metadata");
		const keyframe_stripping_metadata_t* entries = read_contributing_error(*compressed);
		const uint32_t num_samples = compressed->get_num_samples_per_track();

		std::vector<keyframe_stripping_metadata_t> finite_entries;
		for (uint32_t index = 0; index < num_samples; ++index)
		{
			REQUIRE(entries[index].keyframe_index < num_samples, "uninitialized metadata entry reached the global sort");
			if (!std::isinf(entries[index].stripping_error))
				finite_entries.push_back(entries[index]);
		}

		std::sort(finite_entries.begin(), finite_entries.end(),
			[](const keyframe_stripping_metadata_t& lhs, const keyframe_stripping_metadata_t& rhs)
			{ return lhs.stripping_index < rhs.stripping_index; });

		REQUIRE(finite_entries.size() == protected_order.size(),
			"metadata removal count %u must match simulator count %u", uint32_t(finite_entries.size()), uint32_t(protected_order.size()));

		for (size_t index = 0; index < finite_entries.size() && index < protected_order.size(); ++index)
		{
			REQUIRE(finite_entries[index].keyframe_index == protected_order[index],
				"removal order mismatch at step %u: metadata %u simulator %u",
				uint32_t(index), finite_entries[index].keyframe_index, protected_order[index]);
		}

		// Protected entries sit in the non-removable tail together with the segment
		// boundaries: infinite error and no trivial flag. The global sort reassigns
		// every stripping index, so the tail position is the reliable marker.
		for (uint32_t frame : protected_frames)
		{
			bool found = false;
			for (uint32_t index = 0; index < num_samples; ++index)
			{
				if (entries[index].keyframe_index == frame)
				{
					found = true;
					REQUIRE(std::isinf(entries[index].stripping_error), "protected frame %u must have infinite error", frame);
					REQUIRE(!entries[index].is_keyframe_trivial, "protected frame %u must not be trivial", frame);
					REQUIRE(entries[index].stripping_index >= uint32_t(protected_order.size()),
						"protected frame %u must sort into the non-removable tail", frame);
					break;
				}
			}
			REQUIRE(found, "metadata for protected frame %u is missing", frame);
		}

		// Find the first divergence of the two removal orders.
		size_t divergence = 0;
		while (divergence < protected_order.size() &&
			divergence < unprotected_order.size() &&
			unprotected_order[divergence] == protected_order[divergence])
		{
			++divergence;
		}

		REQUIRE(divergence < protected_order.size(), "protection failed to change the removal order");

		// Directly prove the mechanism on the first protected frame. Start from one shared
		// deletion, then measure its removable interior neighbors twice: with the protected
		// frame retained as an endpoint and with it removed so the interpolation span jumps.
		const uint32_t protected_frame = protected_a;
		std::vector<bool> state_retained(k_num_frames, true);
		state_retained[unprotected_order[0]] = false;	// The first greedy deletion, identical in both runs

		// Nearest still-retained interior frame on each side. Boundaries are not candidates.
		uint32_t left_neighbor = k_num_frames;
		for (uint32_t frame = protected_frame; frame > 1; --frame)
		{
			if (state_retained[frame - 1])
			{
				left_neighbor = frame - 1;
				break;
			}
		}

		uint32_t right_neighbor = k_num_frames;
		for (uint32_t frame = protected_frame; frame + 2 < k_num_frames; ++frame)
		{
			if (state_retained[frame + 1])
			{
				right_neighbor = frame + 1;
				break;
			}
		}

		const bool has_left_neighbor = left_neighbor < k_num_frames;
		const bool has_right_neighbor = right_neighbor < k_num_frames;
		REQUIRE(has_left_neighbor || has_right_neighbor, "the protected frame has no removable interior neighbor");

		// World B removes the protected frame so the neighbor spans jump past it.
		std::vector<bool> state_without_protected = state_retained;
		state_without_protected[protected_frame] = false;

		if (has_left_neighbor)
		{
			const greedy_simulator::candidate_result left_locked = simulator.measure_candidate(state_retained, left_neighbor);
			const greedy_simulator::candidate_result left_removed = simulator.measure_candidate(state_without_protected, left_neighbor);
			REQUIRE(left_locked.end_frame == protected_frame, "the left neighbor must interpolate up to the protected frame");
			REQUIRE(left_removed.end_frame != left_locked.end_frame, "locking the protected frame must change the left endpoint");
			REQUIRE(std::fabs(left_removed.error - left_locked.error) > 1.0e-9F,
				"locking the protected frame must change the left reconstruction error");
		}

		if (has_right_neighbor)
		{
			const greedy_simulator::candidate_result right_locked = simulator.measure_candidate(state_retained, right_neighbor);
			const greedy_simulator::candidate_result right_removed = simulator.measure_candidate(state_without_protected, right_neighbor);
			REQUIRE(right_locked.start_frame == protected_frame, "the right neighbor must interpolate back to the protected frame");
			REQUIRE(right_removed.start_frame != right_locked.start_frame, "locking the protected frame must change the right endpoint");
			REQUIRE(std::fabs(right_removed.error - right_locked.error) > 1.0e-9F,
				"locking the protected frame must change the right reconstruction error");
		}

		allocator.deallocate(compressed, compressed->get_size());
	}

	void test_multi_segment_and_saturation(ansi_allocator& allocator, vqvf_transform_error_metric& metric)
	{
		constexpr uint32_t k_num_frames = 40;	// Splits into two segments of 20 samples
		constexpr uint32_t k_num_bones = 4;
		program_clip clip = make_nonlinear_clip(k_num_frames, k_num_bones);
		track_array_vqvf tracks = make_track_array(allocator, clip);

		// Interior frames, a segment boundary frame, and frames on both sides of it.
		const uint32_t protected_frames[] = { 7, 19, 20, 31 };

		compression_settings settings = make_base_settings(metric, false);
		settings.keyframe_stripping.proportion = 1.0F;	// Saturate the proportion cap
		settings.keyframe_stripping.protected_keyframes = protected_frames;
		settings.keyframe_stripping.num_protected_keyframes = 4;

		compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "multi segment saturated");
		REQUIRE(compressed->has_stripped_keyframes(), "saturated proportion must strip removable frames");
		REQUIRE(compressed->get_num_samples_per_track() == k_num_frames, "sample count must be unchanged by stripping");

		const transform_tracks_header& header = get_transform_tracks_header(*compressed);
		REQUIRE(header.num_segments >= 2, "the clip must span multiple segments");

		const std::vector<bool> retained = read_retained_mask(*compressed);
		REQUIRE(retained.size() == k_num_frames, "retained mask size mismatch");

		uint32_t num_retained = 0;
		for (bool is_retained : retained)
			num_retained += is_retained ? 1U : 0U;

		for (uint32_t frame : protected_frames)
			REQUIRE(retained[frame], "protected frame %u must remain in the bit mask", frame);

		// Segment boundary frames are never removable, verify each one is still present.
		const uint32_t* segment_start_indices = header.get_segment_start_indices();
		for (uint32_t segment_index = 0; segment_index < header.num_segments; ++segment_index)
		{
			const uint32_t first = segment_start_indices[segment_index];
			const uint32_t last = segment_index + 1 < header.num_segments
				? segment_start_indices[segment_index + 1] - 1
				: k_num_frames - 1;
			REQUIRE(retained[first], "segment boundary frame %u must remain", first);
			REQUIRE(retained[last], "segment boundary frame %u must remain", last);
		}

		// Saturation cannot remove every frame: boundaries and protected frames remain,
		// and the removable region must still lose frames under a proportion of 1.0.
		REQUIRE(num_retained < k_num_frames, "saturated stripping failed to remove a removable frame");

		// Every retained pose decompresses and stays close to the original motion.
		for (uint32_t frame = 0; frame < k_num_frames; ++frame)
		{
			for (uint32_t bone = 0; bone < k_num_bones; ++bone)
			{
				const rtm::vqvf pose = decompress_local_pose(allocator, *compressed, bone, frame, clip.sample_rate);
				const float delta_x = rtm::vector_get_x(pose.translation) - rtm::vector_get_x(clip.at(frame, bone).translation);
				const float delta_y = rtm::vector_get_y(pose.translation) - rtm::vector_get_y(clip.at(frame, bone).translation);
				REQUIRE(std::fabs(delta_x) < 2.0F && std::fabs(delta_y) < 2.0F,
					"decompressed frame %u bone %u drifted too far", frame, bone);
			}
		}

		allocator.deallocate(compressed, compressed->get_size());
	}

	void test_threshold_and_trivial(ansi_allocator& allocator, vqvf_transform_error_metric& metric)
	{
		constexpr uint32_t k_num_frames = 28;
		program_clip clip = make_smooth_clip(k_num_frames, 4);
		track_array_vqvf tracks = make_track_array(allocator, clip);
		const uint32_t protected_frames[] = { 6, 17, 23 };

		// A large threshold removes every removable frame below it but never a protected one.
		{
			compression_settings settings = make_base_settings(metric, false);
			settings.keyframe_stripping.threshold = 100.0F;
			settings.keyframe_stripping.protected_keyframes = protected_frames;
			settings.keyframe_stripping.num_protected_keyframes = 3;

			compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "large threshold");
			const std::vector<bool> retained = read_retained_mask(*compressed);
			for (uint32_t frame : protected_frames)
				REQUIRE(retained[frame], "threshold stripping removed protected frame %u", frame);
			allocator.deallocate(compressed, compressed->get_size());
		}

		// strip_trivial consumes only the legal removable prefix as well.
		{
			compression_settings settings = make_base_settings(metric, false);
			settings.keyframe_stripping.strip_trivial = true;
			settings.keyframe_stripping.protected_keyframes = protected_frames;
			settings.keyframe_stripping.num_protected_keyframes = 3;

			compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "strip trivial");
			const std::vector<bool> retained = read_retained_mask(*compressed);
			for (uint32_t frame : protected_frames)
				REQUIRE(retained[frame], "trivial stripping removed protected frame %u", frame);
			allocator.deallocate(compressed, compressed->get_size());
		}
	}

	void test_loop_optimization_mapping(ansi_allocator& allocator, vqvf_transform_error_metric& metric)
	{
		constexpr uint32_t k_num_frames = 20;
		program_clip clip = make_smooth_clip(k_num_frames, 3);

		// Force the last sample to repeat the first sample so loop optimization removes it.
		for (uint32_t bone = 0; bone < clip.num_bones; ++bone)
			clip.at(k_num_frames - 1, bone) = clip.at(0, bone);

		track_array_vqvf tracks = make_track_array(allocator, clip);

		// Protect the original last frame and an ordinary interior frame.
		const uint32_t protected_frames[] = { 5, k_num_frames - 1 };

		compression_settings settings = make_base_settings(metric, true);	// Loop optimization enabled
		settings.keyframe_stripping.proportion = 0.6F;
		settings.keyframe_stripping.protected_keyframes = protected_frames;
		settings.keyframe_stripping.num_protected_keyframes = 2;

		compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "loop wrap mapping");
		REQUIRE(compressed->get_num_samples_per_track() == k_num_frames - 1, "the repeating last sample must be removed");
		REQUIRE(compressed->get_looping_policy() == sample_looping_policy::wrap, "the clip must use the wrap policy");

		const std::vector<bool> retained = read_retained_mask(*compressed);
		REQUIRE(retained[0], "the original last frame must map to the retained first frame");
		REQUIRE(retained[5], "the ordinary protected frame keeps its number after the mapping");

		// The wrapped stream still decompresses at the first and last stored times.
		const rtm::vqvf first_pose = decompress_local_pose(allocator, *compressed, 0, 0, clip.sample_rate);
		const rtm::vqvf last_pose = decompress_local_pose(allocator, *compressed, 0, k_num_frames - 2, clip.sample_rate);
		REQUIRE(std::isfinite(rtm::vector_get_x(first_pose.translation)), "first wrapped pose is not finite");
		REQUIRE(std::isfinite(rtm::vector_get_x(last_pose.translation)), "last wrapped pose is not finite");

		allocator.deallocate(compressed, compressed->get_size());

		// Two protected indices that coincide after the wrap mapping: frame 0 and the
		// original last frame both map to retained frame 0. This is legal and must not
		// report a duplicate error.
		{
			const uint32_t coincident_frames[] = { 0, k_num_frames - 1 };
			compression_settings coincident_settings = make_base_settings(metric, true);
			coincident_settings.keyframe_stripping.proportion = 0.6F;
			coincident_settings.keyframe_stripping.protected_keyframes = coincident_frames;
			coincident_settings.keyframe_stripping.num_protected_keyframes = 2;

			compressed_tracks* coincident = compress_or_fail(allocator, tracks, coincident_settings, "coincident wrap mapping");
			REQUIRE(coincident->get_num_samples_per_track() == k_num_frames - 1, "the repeating last sample must be removed");
			const std::vector<bool> coincident_retained = read_retained_mask(*coincident);
			REQUIRE(coincident_retained[0], "both coincident indices must resolve to the retained first frame");

			// The settings list is still valid and the hash ignores the merged duplicate.
			REQUIRE(coincident_settings.keyframe_stripping.is_valid(k_num_frames).empty(),
				"the original space list stays valid after a coincident mapping");
			allocator.deallocate(coincident, coincident->get_size());
		}

		// Without an actual wrap removal, the last index does not map to frame 0.
		program_clip non_wrapping_clip = make_smooth_clip(k_num_frames, 3);
		track_array_vqvf non_wrapping_tracks = make_track_array(allocator, non_wrapping_clip);
		compression_settings non_wrapping_settings = make_base_settings(metric, true);
		non_wrapping_settings.keyframe_stripping.proportion = 0.6F;
		non_wrapping_settings.keyframe_stripping.protected_keyframes = protected_frames;
		non_wrapping_settings.keyframe_stripping.num_protected_keyframes = 2;

		compressed_tracks* non_wrapping = compress_or_fail(allocator, non_wrapping_tracks, non_wrapping_settings, "no wrap mapping");
		REQUIRE(non_wrapping->get_num_samples_per_track() == k_num_frames, "a non-wrapping clip keeps every sample");
		const std::vector<bool> non_wrapping_retained = read_retained_mask(*non_wrapping);
		REQUIRE(non_wrapping_retained[k_num_frames - 1], "the last frame must stay protected when no wrap removal happens");
		allocator.deallocate(non_wrapping, non_wrapping->get_size());
	}

	void test_edge_case_clips(ansi_allocator& allocator, vqvf_transform_error_metric& metric)
	{
		// Single frame clip with its only frame protected.
		{
			program_clip clip = make_nonlinear_clip(1, 2);
			track_array_vqvf tracks = make_track_array(allocator, clip);
			const uint32_t protected_frames[] = { 0 };

			compression_settings settings = make_base_settings(metric, false);
			settings.keyframe_stripping.proportion = 0.5F;
			settings.keyframe_stripping.protected_keyframes = protected_frames;
			settings.keyframe_stripping.num_protected_keyframes = 1;

			compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "single frame");
			const rtm::vqvf pose = decompress_local_pose(allocator, *compressed, 0, 0, clip.sample_rate);
			REQUIRE(std::isfinite(rtm::vector_get_x(pose.translation)), "single frame pose is not finite");
			allocator.deallocate(compressed, compressed->get_size());
		}

		// Every frame protected, including the boundaries.
		{
			constexpr uint32_t k_num_frames = 10;
			program_clip clip = make_nonlinear_clip(k_num_frames, 2);
			track_array_vqvf tracks = make_track_array(allocator, clip);
			std::vector<uint32_t> all_frames;
			for (uint32_t frame = 0; frame < k_num_frames; ++frame)
				all_frames.push_back(frame);

			compression_settings settings = make_base_settings(metric, false);
			settings.keyframe_stripping.proportion = 1.0F;
			settings.keyframe_stripping.protected_keyframes = all_frames.data();
			settings.keyframe_stripping.num_protected_keyframes = uint32_t(all_frames.size());

			compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "all frames protected");
			REQUIRE(!compressed->has_stripped_keyframes(), "a fully protected clip cannot strip frames");
			allocator.deallocate(compressed, compressed->get_size());
		}

		// Every movable frame protected, boundaries stay naturally retained.
		{
			constexpr uint32_t k_num_frames = 10;
			program_clip clip = make_nonlinear_clip(k_num_frames, 2);
			track_array_vqvf tracks = make_track_array(allocator, clip);
			std::vector<uint32_t> movable_frames;
			for (uint32_t frame = 1; frame + 1 < k_num_frames; ++frame)
				movable_frames.push_back(frame);

			compression_settings settings = make_base_settings(metric, false);
			settings.keyframe_stripping.proportion = 1.0F;
			settings.keyframe_stripping.threshold = 100.0F;
			settings.keyframe_stripping.strip_trivial = true;
			settings.keyframe_stripping.protected_keyframes = movable_frames.data();
			settings.keyframe_stripping.num_protected_keyframes = uint32_t(movable_frames.size());

			compressed_tracks* compressed = compress_or_fail(allocator, tracks, settings, "all movable protected");
			REQUIRE(!compressed->has_stripped_keyframes(), "protecting every movable frame must prevent stripping");
			const rtm::vqvf pose = decompress_local_pose(allocator, *compressed, 0, k_num_frames - 1, clip.sample_rate);
			REQUIRE(std::isfinite(rtm::vector_get_x(pose.translation)), "fully retained clip pose is not finite");
			allocator.deallocate(compressed, compressed->get_size());
		}
	}

	void test_storage_savings_survive_protection(ansi_allocator& allocator, vqvf_transform_error_metric& metric)
	{
		constexpr uint32_t k_num_frames = 48;
		program_clip clip = make_smooth_clip(k_num_frames, 5);
		track_array_vqvf tracks = make_track_array(allocator, clip);
		const uint32_t protected_frames[] = { 8, 24, 40 };

		compression_settings plain_settings = make_base_settings(metric, false);
		compressed_tracks* plain = compress_or_fail(allocator, tracks, plain_settings, "plain size");

		compression_settings stripped_settings = make_base_settings(metric, false);
		stripped_settings.keyframe_stripping.proportion = 0.8F;
		compressed_tracks* stripped = compress_or_fail(allocator, tracks, stripped_settings, "stripped size");

		compression_settings protected_settings = make_base_settings(metric, false);
		protected_settings.keyframe_stripping.proportion = 0.8F;
		protected_settings.keyframe_stripping.protected_keyframes = protected_frames;
		protected_settings.keyframe_stripping.num_protected_keyframes = 3;
		compressed_tracks* protected_compressed = compress_or_fail(allocator, tracks, protected_settings, "protected size");

		REQUIRE(stripped->get_size() < plain->get_size(), "unprotected stripping must save space");
		REQUIRE(protected_compressed->get_size() < plain->get_size(),
			"protected stripping must still remove unprotected redundant frames");

		const std::vector<bool> retained = read_retained_mask(*protected_compressed);
		for (uint32_t frame : protected_frames)
			REQUIRE(retained[frame], "protected frame %u missing from the reduced stream", frame);

		uint32_t num_retained = 0;
		for (bool is_retained : retained)
			num_retained += is_retained ? 1U : 0U;
		REQUIRE(num_retained < k_num_frames, "no redundant frame was removed despite protection");

		allocator.deallocate(plain, plain->get_size());
		allocator.deallocate(stripped, stripped->get_size());
		allocator.deallocate(protected_compressed, protected_compressed->get_size());
	}
}

int main()
{
	ansi_allocator allocator;
	vqvf_transform_error_metric metric;

	test_validation_errors(allocator, metric);
	test_hash_and_byte_compatibility(allocator, metric);
	test_scalar_and_database_rejection(allocator, metric);
	test_greedy_order_and_endpoints(allocator, metric);
	test_multi_segment_and_saturation(allocator, metric);
	test_threshold_and_trivial(allocator, metric);
	test_loop_optimization_mapping(allocator, metric);
	test_edge_case_clips(allocator, metric);
	test_storage_savings_survive_protection(allocator, metric);

	if (g_failures != 0)
	{
		std::printf("ACL_PROTECTED_TARGET_FAILED failures=%u\n", g_failures);
		return 1;
	}

	std::printf("ACL_PROTECTED_TARGET_PASS\n");
	return 0;
}
