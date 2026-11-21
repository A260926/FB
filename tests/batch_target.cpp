// Batch decompression target tests.
// The cases are registered with the task driver in task_support/driver.c and
// are grouped in three groups: values, sharing, and contracts.

#include "task_support/check.h"

#include <acl/compression/compress.h>
#include <acl/compression/track_array.h>
#include <acl/core/ansi_allocator.h>
#include <acl/core/impl/compressed_headers.h>
#include <acl/core/track_writer.h>
#include <acl/decompression/batch_decompress.h>
#include <acl/decompression/decompress.h>

#include <rtm/quatf.h>
#include <rtm/qvvf.h>
#include <rtm/scalarf.h>
#include <rtm/vector4f.h>
#include <rtm/vqvf.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

using namespace acl;

//////////////////////////////////////////////////////////////////////////
// Heap allocation tracking. The batch context must not allocate or release
// heap memory while it serves requests. The operators below count every
// global new and delete while they are armed.
//////////////////////////////////////////////////////////////////////////

static std::atomic<uint32_t> g_num_heap_allocs{ 0 };
static std::atomic<uint32_t> g_num_heap_frees{ 0 };
static bool g_track_heap = false;

static void* tracked_allocate(std::size_t size)
{
	if (g_track_heap)
		g_num_heap_allocs.fetch_add(1, std::memory_order_relaxed);

	void* ptr = std::malloc(size != 0 ? size : 1);
	if (ptr == nullptr)
		throw std::bad_alloc();
	return ptr;
}

static void tracked_release(void* ptr) noexcept
{
	if (g_track_heap && ptr != nullptr)
		g_num_heap_frees.fetch_add(1, std::memory_order_relaxed);

	std::free(ptr);
}

void* operator new(std::size_t size) { return tracked_allocate(size); }
void* operator new[](std::size_t size) { return tracked_allocate(size); }
void* operator new(std::size_t size, std::align_val_t) { return tracked_allocate(size); }
void* operator new[](std::size_t size, std::align_val_t) { return tracked_allocate(size); }
void operator delete(void* ptr) noexcept { tracked_release(ptr); }
void operator delete[](void* ptr) noexcept { tracked_release(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { tracked_release(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { tracked_release(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { tracked_release(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { tracked_release(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { tracked_release(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { tracked_release(ptr); }

//////////////////////////////////////////////////////////////////////////
// Test helpers
//////////////////////////////////////////////////////////////////////////

namespace
{
	constexpr float k_tolerance = 1.0E-5F;

	struct pose_writer : track_writer
	{
		rtm::vqvf* pose;
		void RTM_SIMD_CALL write_rotation(uint32_t track_index, rtm::quatf_arg0 rotation) { pose[track_index].rotation = rotation; }
		void RTM_SIMD_CALL write_translation(uint32_t track_index, rtm::vector4f_arg0 translation) { pose[track_index].translation = translation; }
		void RTM_SIMD_CALL write_scale(uint32_t track_index, rtm::vector4f_arg0 scale) { pose[track_index].scale = scale; }
	};

	bool scalar_near(float lhs, float rhs)
	{
		return std::fabs(lhs - rhs) <= k_tolerance + k_tolerance * std::fabs(rhs);
	}

	bool vector_near3(rtm::vector4f_arg0 lhs, rtm::vector4f_arg0 rhs)
	{
		return scalar_near(rtm::vector_get_x(lhs), rtm::vector_get_x(rhs))
			&& scalar_near(rtm::vector_get_y(lhs), rtm::vector_get_y(rhs))
			&& scalar_near(rtm::vector_get_z(lhs), rtm::vector_get_z(rhs));
	}

	// Rotations are compared by direction: q and -q describe the same rotation
	bool quat_near_direction(rtm::quatf_arg0 lhs, rtm::quatf_arg0 rhs)
	{
		const float dot = rtm::scalar_cast(rtm::quat_dot_as_scalar(lhs, rhs));
		return (1.0F - std::fabs(dot)) <= 2.0F * k_tolerance;
	}

	bool pose_near(const rtm::vqvf& lhs, const rtm::vqvf& rhs)
	{
		return quat_near_direction(lhs.rotation, rhs.rotation)
			&& vector_near3(lhs.translation, rhs.translation)
			&& vector_near3(lhs.scale, rhs.scale);
	}

	// The parent of each bone, always lower than the bone index
	uint32_t bone_parent(uint32_t bone_index)
	{
		if (bone_index == 0)
			return k_invalid_track_index;
		return (bone_index % 4) == 0 ? bone_index / 2 : bone_index - 1;
	}

	// One sample of the procedural clip. The bone index selects the kind of
	// sub-tracks it produces once compressed:
	//    bone % 6 == 0: default rotation and translation
	//    bone % 6 == 1: constant rotation and translation
	//    bone % 6 == 2: animated rotation and translation
	//    bone % 6 == 3: animated rotation, constant translation
	//    bone % 6 == 4: constant rotation, animated translation
	//    bone % 6 == 5: animated rotation and translation, static outside [1, 4]
	rtm::vqvf make_bone_sample(uint32_t bone_index, float sample_time)
	{
		const float t = sample_time;
		rtm::quatf rotation;
		rtm::vector4f translation;
		switch (bone_index % 6)
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
			rotation = rtm::quat_from_euler(0.15F * t, 0.07F * t * float(bone_index % 7 + 1), 0.21F * t);
			translation = rtm::vector_set(0.11F * float(bone_index % 5) + 0.06F * std::sin(t * 1.3F), 0.23F * t, 0.04F * std::cos(t));
			break;
		case 3:
			rotation = rtm::quat_from_euler(0.55F * std::sin(t * 0.7F), 0.23F, -0.17F * t);
			translation = rtm::vector_set(-0.19F, 0.33F, 0.08F);
			break;
		case 4:
			rotation = rtm::quat_from_euler(-0.41F, 0.29F, 0.37F);
			translation = rtm::vector_set(0.05F * float(bone_index % 6), 0.16F * std::sin(t * 0.9F), 0.31F * t);
			break;
		default:
		{
			// A smooth window that keeps the bone static outside [1, 4] so that
			// some segments see a constant bit rate for these sub-tracks
			const float x = t < 1.0F ? 0.0F : (t > 4.0F ? 1.0F : (t - 1.0F) / 3.0F);
			const float w = x * x * (3.0F - 2.0F * x);
			rotation = rtm::quat_from_euler(0.8F * w, -0.5F * w, 0.3F * w);
			translation = rtm::vector_set(0.4F * w, 0.2F * w, -0.1F * w);
			break;
		}
		}
		return rtm::vqv_set(rotation, translation, rtm::vector_set(1.0F));
	}

	struct clip_params
	{
		uint32_t num_bones = 70;
		uint32_t num_samples = 200;
		float sample_rate = 30.0F;
		rotation_format8 rotation_format = rotation_format8::quatf_drop_w_variable;
		vector_format8 translation_format = vector_format8::vector3f_variable;
		bool optimize_loops = false;
		bool match_last_to_first = false;
	};

	compressed_tracks* build_clip(ansi_allocator& allocator, const clip_params& params)
	{
		track_array_vqvf raw(allocator, params.num_bones);
		for (uint32_t bone_index = 0; bone_index < params.num_bones; ++bone_index)
		{
			track_desc_transformf desc;
			desc.output_index = bone_index;
			desc.parent_index = bone_parent(bone_index);
			desc.precision = 0.001F;
			desc.shell_distance = 1.0F;
			track_vqvf track = track_vqvf::make_reserve(desc, allocator, params.num_samples, params.sample_rate);
			for (uint32_t sample_index = 0; sample_index < params.num_samples; ++sample_index)
			{
				const float sample_time = float(sample_index) / params.sample_rate;
				track[sample_index] = make_bone_sample(bone_index, sample_time);
			}
			if (params.match_last_to_first)
				track[params.num_samples - 1] = track[0];
			raw[bone_index] = std::move(track);
		}

		compression_settings settings = get_default_compression_settings();
		settings.rotation_format = params.rotation_format;
		settings.translation_format = params.translation_format;
		settings.scale_format = vector_format8::vector3f_full;
		settings.optimize_loops = params.optimize_loops;
		settings.keyframe_stripping.strip_trivial = false;

		compressed_tracks* packed = nullptr;
		output_stats stats;
		const error_result error = compress_track_list(allocator, raw, settings, packed, stats);
		if (error.any())
			return nullptr;
		return packed;
	}

	compressed_tracks* build_scale_clip(ansi_allocator& allocator)
	{
		const uint32_t num_bones = 8;
		const uint32_t num_samples = 40;
		track_array_vqvf raw(allocator, num_bones);
		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
		{
			track_desc_transformf desc;
			desc.output_index = bone_index;
			desc.parent_index = bone_parent(bone_index);
			desc.precision = 0.001F;
			desc.shell_distance = 1.0F;
			track_vqvf track = track_vqvf::make_reserve(desc, allocator, num_samples, 30.0F);
			for (uint32_t sample_index = 0; sample_index < num_samples; ++sample_index)
			{
				const float t = float(sample_index) / 30.0F;
				const rtm::vector4f scale = (bone_index % 3) == 2 ? rtm::vector_set(1.0F + 0.03F * t, 1.0F, 1.0F - 0.01F * t) : rtm::vector_set(1.0F);
				track[sample_index] = rtm::vqv_set(rtm::quat_from_euler(0.1F * t, 0.0F, 0.0F), rtm::vector_set(0.1F * t, 0.0F, 0.0F), scale);
			}
			raw[bone_index] = std::move(track);
		}

		compression_settings settings = get_default_compression_settings();
		settings.keyframe_stripping.strip_trivial = false;
		settings.optimize_loops = false;

		compressed_tracks* packed = nullptr;
		output_stats stats;
		const error_result error = compress_track_list(allocator, raw, settings, packed, stats);
		if (error.any())
			return nullptr;
		return packed;
	}

	compressed_tracks* build_scalar_clip(ansi_allocator& allocator)
	{
		track_array_float1f raw(allocator, 4);
		for (uint32_t track_index = 0; track_index < 4; ++track_index)
		{
			track_desc_scalarf desc;
			desc.output_index = track_index;
			desc.precision = 0.001F;
			track_float1f track = track_float1f::make_reserve(desc, allocator, 16, 30.0F);
			for (uint32_t sample_index = 0; sample_index < 16; ++sample_index)
				track[sample_index] = std::sin(float(sample_index) * 0.5F + float(track_index));
			raw[track_index] = std::move(track);
		}

		compression_settings settings = get_default_compression_settings();
		compressed_tracks* packed = nullptr;
		output_stats stats;
		const error_result error = compress_track_list(allocator, raw, settings, packed, stats);
		if (error.any())
			return nullptr;
		return packed;
	}

	compressed_tracks* build_vqf_clip(ansi_allocator& allocator)
	{
		track_array_vqf raw(allocator, 6);
		for (uint32_t bone_index = 0; bone_index < 6; ++bone_index)
		{
			track_desc_transformf desc;
			desc.output_index = bone_index;
			desc.parent_index = bone_parent(bone_index);
			desc.precision = 0.001F;
			desc.shell_distance = 1.0F;
			track_vqf track = track_vqf::make_reserve(desc, allocator, 16, 30.0F);
			for (uint32_t sample_index = 0; sample_index < 16; ++sample_index)
			{
				const float t = float(sample_index) / 30.0F;
				track[sample_index] = rtm::vq_set(rtm::quat_from_euler(0.1F * t, 0.2F * t, 0.0F), rtm::vector_set(0.1F * t, 0.0F, 0.0F));
			}
			raw[bone_index] = std::move(track);
		}

		compression_settings settings = get_default_compression_settings();
		settings.keyframe_stripping.strip_trivial = false;
		compressed_tracks* packed = nullptr;
		output_stats stats;
		const error_result error = compress_track_list(allocator, raw, settings, packed, stats);
		if (error.any())
			return nullptr;
		return packed;
	}

	compressed_tracks* build_vqsf_clip(ansi_allocator& allocator)
	{
		track_array_vqsf raw(allocator, 6);
		for (uint32_t bone_index = 0; bone_index < 6; ++bone_index)
		{
			track_desc_transformf desc;
			desc.output_index = bone_index;
			desc.parent_index = bone_parent(bone_index);
			desc.precision = 0.001F;
			desc.shell_distance = 1.0F;
			track_vqsf track = track_vqsf::make_reserve(desc, allocator, 16, 30.0F);
			for (uint32_t sample_index = 0; sample_index < 16; ++sample_index)
			{
				const float t = float(sample_index) / 30.0F;
				track[sample_index] = rtm::vqs_set(rtm::quat_from_euler(0.1F * t, 0.0F, 0.2F * t), rtm::vector_set(0.0F, 0.1F * t, 0.0F), 1.0F + 0.01F * t);
			}
			raw[bone_index] = std::move(track);
		}

		compression_settings settings = get_default_compression_settings();
		settings.keyframe_stripping.strip_trivial = false;
		compressed_tracks* packed = nullptr;
		output_stats stats;
		const error_result error = compress_track_list(allocator, raw, settings, packed, stats);
		if (error.any())
			return nullptr;
		return packed;
	}

	compressed_tracks* build_stripped_clip(ansi_allocator& allocator)
	{
		clip_params params;
		params.num_bones = 12;
		params.num_samples = 120;

		track_array_vqvf raw(allocator, params.num_bones);
		for (uint32_t bone_index = 0; bone_index < params.num_bones; ++bone_index)
		{
			track_desc_transformf desc;
			desc.output_index = bone_index;
			desc.parent_index = bone_parent(bone_index);
			desc.precision = 0.001F;
			desc.shell_distance = 1.0F;
			track_vqvf track = track_vqvf::make_reserve(desc, allocator, params.num_samples, params.sample_rate);
			for (uint32_t sample_index = 0; sample_index < params.num_samples; ++sample_index)
				track[sample_index] = make_bone_sample(bone_index, float(sample_index) / params.sample_rate);
			raw[bone_index] = std::move(track);
		}

		compression_settings settings = get_default_compression_settings();
		settings.optimize_loops = false;
		settings.keyframe_stripping.strip_trivial = true;
		settings.keyframe_stripping.proportion = 0.5F;

		compressed_tracks* packed = nullptr;
		output_stats stats;
		const error_result error = compress_track_list(allocator, raw, settings, packed, stats);
		if (error.any())
			return nullptr;
		return packed;
	}

	compressed_tracks* build_additive_clip(ansi_allocator& allocator)
	{
		const uint32_t num_bones = 8;
		track_array_vqvf raw(allocator, num_bones);
		track_array_vqvf base(allocator, num_bones);
		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
		{
			track_desc_transformf desc;
			desc.output_index = bone_index;
			desc.parent_index = bone_parent(bone_index);
			desc.precision = 0.001F;
			desc.shell_distance = 1.0F;
			track_vqvf track = track_vqvf::make_reserve(desc, allocator, 40, 30.0F);
			for (uint32_t sample_index = 0; sample_index < 40; ++sample_index)
				track[sample_index] = make_bone_sample(bone_index, float(sample_index) / 30.0F);
			raw[bone_index] = std::move(track);

			track_vqvf base_track = track_vqvf::make_reserve(desc, allocator, 1, 30.0F);
			base_track[0] = make_bone_sample(bone_index, 0.0F);
			base[bone_index] = std::move(base_track);
		}

		compression_settings settings = get_default_compression_settings();
		settings.keyframe_stripping.strip_trivial = false;
		settings.optimize_loops = false;

		transform_base_desc_t base_desc;
		base_desc.format = additive_clip_format8::additive1;
		base_desc.tracks = &base;

		compressed_tracks* packed = nullptr;
		output_stats stats;
		const error_result error = compress_track_list(allocator, raw, settings, base_desc, packed, stats);
		if (error.any())
			return nullptr;
		return packed;
	}

	compressed_tracks* build_database_clip(ansi_allocator& allocator, compressed_database*& out_database)
	{
		clip_params params;
		params.num_bones = 12;
		params.num_samples = 120;

		track_array_vqvf raw(allocator, params.num_bones);
		for (uint32_t bone_index = 0; bone_index < params.num_bones; ++bone_index)
		{
			track_desc_transformf desc;
			desc.output_index = bone_index;
			desc.parent_index = bone_parent(bone_index);
			desc.precision = 0.001F;
			desc.shell_distance = 1.0F;
			track_vqvf track = track_vqvf::make_reserve(desc, allocator, params.num_samples, params.sample_rate);
			for (uint32_t sample_index = 0; sample_index < params.num_samples; ++sample_index)
				track[sample_index] = make_bone_sample(bone_index, float(sample_index) / params.sample_rate);
			raw[bone_index] = std::move(track);
		}

		compression_settings settings = get_default_compression_settings();
		settings.optimize_loops = false;
		settings.keyframe_stripping.strip_trivial = false;
		settings.enable_database_support = true;

		compressed_tracks* packed_with_metadata = nullptr;
		output_stats stats;
		const error_result error = compress_track_list(allocator, raw, settings, packed_with_metadata, stats);
		if (error.any())
			return nullptr;

		compression_database_settings database_settings;
		compressed_tracks* database_tracks[1] = { nullptr };
		const compressed_tracks* input_tracks[1] = { packed_with_metadata };
		compressed_database* database = nullptr;
		const error_result database_error = build_database(allocator, database_settings, input_tracks, 1, database_tracks, database);
		allocator.deallocate(packed_with_metadata, packed_with_metadata->get_size());
		if (database_error.any() || database_tracks[0] == nullptr)
		{
			if (database != nullptr)
				allocator.deallocate(database, database->get_size());
			return nullptr;
		}

		out_database = database;
		return database_tracks[0];
	}

	void reference_decompress(decompression_context<debug_transform_decompression_settings>& context, float sample_time, sample_rounding_policy rounding_policy, rtm::vqvf* out_pose)
	{
		pose_writer writer;
		writer.pose = out_pose;
		context.seek(sample_time, rounding_policy);
		context.decompress_tracks(writer);
	}

	void compute_model_space(const rtm::vqvf* local_poses, uint32_t num_bones, rtm::vqvf* model_poses)
	{
		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
		{
			const uint32_t parent_index = bone_parent(bone_index);
			model_poses[bone_index] = parent_index == k_invalid_track_index
				? local_poses[bone_index]
				: rtm::vqv_mul(model_poses[parent_index], local_poses[bone_index]);
		}
	}

	rtm::vqvf bone_inverse_bind(uint32_t bone_index)
	{
		const float j = float(bone_index);
		return rtm::vqv_set(
			rtm::quat_from_euler(0.11F * j, -0.07F * j, 0.05F * j),
			rtm::vector_set(0.03F * j, -0.02F * j, 0.01F * j),
			rtm::vector_set(1.0F));
	}

	struct scoped_tracks
	{
		ansi_allocator& allocator;
		compressed_tracks* tracks;
		explicit scoped_tracks(ansi_allocator& allocator_, compressed_tracks* tracks_) : allocator(allocator_), tracks(tracks_) {}
		~scoped_tracks() { if (tracks != nullptr) allocator.deallocate(tracks, tracks->get_size()); }
		operator const compressed_tracks*() const { return tracks; }
	};

	struct scoped_workspace
	{
		ansi_allocator& allocator;
		size_t size;
		void* ptr;
		scoped_workspace(ansi_allocator& allocator_, uint32_t max_requests)
			: allocator(allocator_)
			, size(batch_decompression_context::query_workspace_size(max_requests))
			, ptr(allocator.allocate(size, 16))
		{}
		~scoped_workspace() { allocator.deallocate(ptr, size); }
	};
}

//////////////////////////////////////////////////////////////////////////
// values: the decompressed TRS values match the reference decoder
//////////////////////////////////////////////////////////////////////////

static int test_values_mixed_subtracks()
{
	ansi_allocator allocator;
	clip_params params;
	scoped_tracks tracks(allocator, build_clip(allocator, params));
	TASK_CHECK(tracks.tracks != nullptr);

	const acl_impl::transform_tracks_header& transform_header = acl_impl::get_transform_tracks_header(*tracks.tracks);
	const uint32_t num_bones = params.num_bones;

	// The clip must exercise the feature mix: many segments, every sub-track
	// kind, and a partial last group of animated sub-tracks
	TASK_CHECK(transform_header.num_segments > 1);
	TASK_CHECK(transform_header.num_animated_rotation_sub_tracks % 4 != 0);
	TASK_CHECK(transform_header.num_animated_translation_sub_tracks % 4 != 0);
	TASK_CHECK(transform_header.num_constant_rotation_samples != 0);
	TASK_CHECK(transform_header.num_constant_translation_samples != 0);
	TASK_CHECK(transform_header.num_animated_rotation_sub_tracks != 0);
	TASK_CHECK(transform_header.num_animated_translation_sub_tracks != 0);

	const float duration = tracks.tracks->get_finite_duration(sample_looping_policy::clamp);
	const uint32_t* segment_start_indices = transform_header.get_segment_start_indices();

	// Build a request set that mixes: out of order times, duplicate times,
	// several alphas inside the same frame interval, exact keyframe times,
	// times that straddle segment boundaries, and every rounding policy
	batch_request requests[96];
	uint32_t num_requests = 0;
	const sample_rounding_policy policies[4] = { sample_rounding_policy::none, sample_rounding_policy::floor, sample_rounding_policy::ceil, sample_rounding_policy::nearest };

	uint32_t lcg_state = 12345;
	for (uint32_t index = 0; index < 24; ++index)
	{
		lcg_state = lcg_state * 1664525 + 1013904223;
		const float t = float(lcg_state >> 8) * (duration / 16777216.0F);
		requests[num_requests] = batch_request{ t, policies[num_requests % 4] };
		++num_requests;
	}

	// Duplicate and out of order times
	requests[num_requests++] = batch_request{ 2.0F, sample_rounding_policy::none };
	requests[num_requests++] = batch_request{ 0.5F, sample_rounding_policy::floor };
	requests[num_requests++] = batch_request{ 2.0F, sample_rounding_policy::none };
	requests[num_requests++] = batch_request{ 2.0F, sample_rounding_policy::ceil };

	// Same frame interval, different alphas
	for (uint32_t index = 0; index < 6; ++index)
		requests[num_requests++] = batch_request{ 1.500F + 0.005F * float(index), policies[index % 4] };

	// Exact keyframe times with every rounding policy
	for (uint32_t key = 0; key <= 8; ++key)
	{
		const float t = float(key * 23) / params.sample_rate;
		requests[num_requests++] = batch_request{ t, policies[key % 4] };
	}

	// Times that straddle each segment boundary
	for (uint32_t segment_index = 1; segment_index < transform_header.num_segments; ++segment_index)
	{
		const float boundary = float(segment_start_indices[segment_index]) / params.sample_rate;
		requests[num_requests++] = batch_request{ boundary - 0.5F / params.sample_rate, sample_rounding_policy::none };
		requests[num_requests++] = batch_request{ boundary - 0.25F / params.sample_rate, sample_rounding_policy::nearest };
	}

	// Clamp behavior at and past the clip bounds
	requests[num_requests++] = batch_request{ 0.0F, sample_rounding_policy::none };
	requests[num_requests++] = batch_request{ -1.0F, sample_rounding_policy::none };
	requests[num_requests++] = batch_request{ duration, sample_rounding_policy::none };
	requests[num_requests++] = batch_request{ duration + 1.0F, sample_rounding_policy::ceil };

	TASK_CHECK(num_requests <= 96);

	scoped_workspace workspace(allocator, 96);
	batch_decompression_context context;
	TASK_CHECK(context.initialize(*tracks.tracks, 96, workspace.ptr, workspace.size));

	rtm::vqvf output[96 * 70];
	batch_decode_stats stats;
	TASK_CHECK(context.decompress(requests, num_requests, output, num_requests * num_bones, num_bones, &stats));

	// Every classified sub-track is visited once per call
	TASK_CHECK(stats.sub_track_visits == 2 * num_bones);

	decompression_context<debug_transform_decompression_settings> reference;
	TASK_CHECK(reference.initialize(*tracks.tracks));

	rtm::vqvf expected[70];
	for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
	{
		reference_decompress(reference, requests[request_index].sample_time, requests[request_index].rounding_policy, expected);
		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
		{
			if (!pose_near(output[request_index * num_bones + bone_index], expected[bone_index]))
				std::printf("mismatch request=%u time=%f policy=%d bone=%u\n", request_index, double(requests[request_index].sample_time), int(requests[request_index].rounding_policy), bone_index);
			TASK_CHECK(pose_near(output[request_index * num_bones + bone_index], expected[bone_index]));
		}
	}
	return 0;
}

static int test_values_hierarchy_skinning()
{
	ansi_allocator allocator;
	clip_params params;
	params.num_bones = 24;
	params.num_samples = 120;
	scoped_tracks tracks(allocator, build_clip(allocator, params));
	TASK_CHECK(tracks.tracks != nullptr);

	const uint32_t num_bones = params.num_bones;
	const float duration = tracks.tracks->get_finite_duration(sample_looping_policy::clamp);

	const float times[] = { 0.0F, 0.77F, 1.53F, 2.31F, 3.69F, duration };
	const sample_rounding_policy policies[] = { sample_rounding_policy::none, sample_rounding_policy::floor, sample_rounding_policy::ceil, sample_rounding_policy::nearest, sample_rounding_policy::none, sample_rounding_policy::none };
	constexpr uint32_t num_requests = 6;

	batch_request requests[num_requests];
	for (uint32_t index = 0; index < num_requests; ++index)
		requests[index] = batch_request{ times[index], policies[index] };

	scoped_workspace workspace(allocator, num_requests);
	batch_decompression_context context;
	TASK_CHECK(context.initialize(*tracks.tracks, num_requests, workspace.ptr, workspace.size));

	rtm::vqvf batch_poses[num_requests * 24];
	batch_decode_stats stats;
	TASK_CHECK(context.decompress(requests, num_requests, batch_poses, num_requests * num_bones, num_bones, &stats));

	decompression_context<debug_transform_decompression_settings> reference;
	TASK_CHECK(reference.initialize(*tracks.tracks));

	// Non-identity inverse bind transforms, one per bone
	rtm::vqvf inverse_bind[24];
	for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
		inverse_bind[bone_index] = bone_inverse_bind(bone_index);

	// Weighted vertices: each vertex follows two bones
	constexpr uint32_t num_vertices = 16;
	rtm::vector4f vertices[num_vertices];
	uint32_t vertex_bones[num_vertices][2];
	float vertex_weights[num_vertices][2];
	for (uint32_t vertex_index = 0; vertex_index < num_vertices; ++vertex_index)
	{
		const float angle = float(vertex_index) * 0.7F;
		vertices[vertex_index] = rtm::vector_set(0.4F * std::cos(angle), 0.4F * std::sin(angle), 0.25F * float(vertex_index % 5));
		vertex_bones[vertex_index][0] = (vertex_index * 3) % num_bones;
		vertex_bones[vertex_index][1] = (vertex_index * 3 + 7) % num_bones;
		vertex_weights[vertex_index][0] = 0.25F + 0.5F * float(vertex_index % 3) / 2.0F;
		vertex_weights[vertex_index][1] = 1.0F - vertex_weights[vertex_index][0];
	}

	rtm::vqvf reference_local[24];
	rtm::vqvf batch_model[24];
	rtm::vqvf reference_model[24];
	rtm::vqvf batch_skin[24];
	rtm::vqvf reference_skin[24];

	for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
	{
		reference_decompress(reference, times[request_index], policies[request_index], reference_local);

		// Parent-child affine composition in model space
		compute_model_space(batch_poses + request_index * num_bones, num_bones, batch_model);
		compute_model_space(reference_local, num_bones, reference_model);

		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
		{
			TASK_CHECK(quat_near_direction(batch_model[bone_index].rotation, reference_model[bone_index].rotation));
			TASK_CHECK(vector_near3(batch_model[bone_index].translation, reference_model[bone_index].translation));
		}

		// Skin the weighted vertices with the non-identity inverse bind
		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
		{
			batch_skin[bone_index] = rtm::vqv_mul(batch_model[bone_index], inverse_bind[bone_index]);
			reference_skin[bone_index] = rtm::vqv_mul(reference_model[bone_index], inverse_bind[bone_index]);
		}

		for (uint32_t vertex_index = 0; vertex_index < num_vertices; ++vertex_index)
		{
			rtm::vector4f batch_vertex = rtm::vector_zero();
			rtm::vector4f reference_vertex = rtm::vector_zero();
			for (uint32_t weight_index = 0; weight_index < 2; ++weight_index)
			{
				const uint32_t bone_index = vertex_bones[vertex_index][weight_index];
				const float weight = vertex_weights[vertex_index][weight_index];
				batch_vertex = rtm::vector_add(batch_vertex, rtm::vector_mul(rtm::vqv_mul_point3(vertices[vertex_index], batch_skin[bone_index]), weight));
				reference_vertex = rtm::vector_add(reference_vertex, rtm::vector_mul(rtm::vqv_mul_point3(vertices[vertex_index], reference_skin[bone_index]), weight));
			}
			TASK_CHECK(vector_near3(batch_vertex, reference_vertex));
		}
	}
	return 0;
}

static int test_values_loop_wrap_clamp()
{
	ansi_allocator allocator;
	clip_params params;
	params.num_bones = 24;
	params.num_samples = 97;
	params.optimize_loops = true;
	params.match_last_to_first = true;
	scoped_tracks tracks(allocator, build_clip(allocator, params));
	TASK_CHECK(tracks.tracks != nullptr);
	TASK_CHECK(tracks.tracks->get_looping_policy() == sample_looping_policy::wrap);

	const uint32_t num_bones = params.num_bones;
	const float clamp_duration = tracks.tracks->get_finite_duration(sample_looping_policy::clamp);
	const float wrap_duration = tracks.tracks->get_finite_duration(sample_looping_policy::wrap);
	TASK_CHECK(wrap_duration > clamp_duration);

	// Times inside the loop tail interval, at the wrap duration, and beyond
	const float times[] = { 0.0F, 1.25F, clamp_duration - 0.01F, clamp_duration + 0.01F, wrap_duration - 0.001F, wrap_duration, wrap_duration + 0.5F };
	const sample_rounding_policy policies[] = { sample_rounding_policy::none, sample_rounding_policy::nearest, sample_rounding_policy::none, sample_rounding_policy::none, sample_rounding_policy::floor, sample_rounding_policy::ceil, sample_rounding_policy::none };
	constexpr uint32_t num_requests = 7;

	batch_request requests[num_requests];
	for (uint32_t index = 0; index < num_requests; ++index)
		requests[index] = batch_request{ times[index], policies[index] };

	rtm::vqvf batch_poses[num_requests * 24];
	rtm::vqvf reference_pose[24];
	batch_decode_stats stats;

	decompression_context<debug_transform_decompression_settings> reference;
	TASK_CHECK(reference.initialize(*tracks.tracks));

	// Wrap policy: the tail interval interpolates between the last and first frames
	{
		scoped_workspace workspace(allocator, num_requests);
		batch_decompression_context context;
		TASK_CHECK(context.initialize(*tracks.tracks, num_requests, workspace.ptr, workspace.size));
		context.set_looping_policy(sample_looping_policy::wrap);
		TASK_CHECK(context.decompress(requests, num_requests, batch_poses, num_requests * num_bones, num_bones, &stats));

		reference.set_looping_policy(sample_looping_policy::wrap);
		for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
		{
			reference_decompress(reference, times[request_index], policies[request_index], reference_pose);
			for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
				TASK_CHECK(pose_near(batch_poses[request_index * num_bones + bone_index], reference_pose[bone_index]));
		}
	}

	// Clamp policy on the same clip: the tail interval clamps to the last frame
	{
		scoped_workspace workspace(allocator, num_requests);
		batch_decompression_context context;
		TASK_CHECK(context.initialize(*tracks.tracks, num_requests, workspace.ptr, workspace.size));
		context.set_looping_policy(sample_looping_policy::clamp);
		TASK_CHECK(context.decompress(requests, num_requests, batch_poses, num_requests * num_bones, num_bones, &stats));

		reference.set_looping_policy(sample_looping_policy::clamp);
		for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
		{
			reference_decompress(reference, times[request_index], policies[request_index], reference_pose);
			for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
				TASK_CHECK(pose_near(batch_poses[request_index * num_bones + bone_index], reference_pose[bone_index]));
		}
	}
	return 0;
}

static int test_values_full_precision()
{
	ansi_allocator allocator;

	const rotation_format8 rotation_formats[2] = { rotation_format8::quatf_full, rotation_format8::quatf_drop_w_full };
	for (uint32_t format_index = 0; format_index < 2; ++format_index)
	{
		clip_params params;
		params.num_bones = 20;
		params.num_samples = 100;
		params.rotation_format = rotation_formats[format_index];
		params.translation_format = vector_format8::vector3f_full;
		scoped_tracks tracks(allocator, build_clip(allocator, params));
		TASK_CHECK(tracks.tracks != nullptr);

		const uint32_t num_bones = params.num_bones;
		const float duration = tracks.tracks->get_finite_duration(sample_looping_policy::clamp);

		batch_request requests[8];
		const sample_rounding_policy policies[4] = { sample_rounding_policy::none, sample_rounding_policy::floor, sample_rounding_policy::ceil, sample_rounding_policy::nearest };
		for (uint32_t index = 0; index < 8; ++index)
			requests[index] = batch_request{ duration * float(index) / 7.0F, policies[index % 4] };

		scoped_workspace workspace(allocator, 8);
		batch_decompression_context context;
		TASK_CHECK(context.initialize(*tracks.tracks, 8, workspace.ptr, workspace.size));

		rtm::vqvf batch_poses[8 * 20];
		batch_decode_stats stats;
		TASK_CHECK(context.decompress(requests, 8, batch_poses, 8 * num_bones, num_bones, &stats));

		decompression_context<debug_transform_decompression_settings> reference;
		TASK_CHECK(reference.initialize(*tracks.tracks));

		rtm::vqvf reference_pose[20];
		for (uint32_t request_index = 0; request_index < 8; ++request_index)
		{
			reference_decompress(reference, requests[request_index].sample_time, requests[request_index].rounding_policy, reference_pose);
			for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
				TASK_CHECK(pose_near(batch_poses[request_index * num_bones + bone_index], reference_pose[bone_index]));
		}
	}
	return 0;
}

static int test_values_single_frame()
{
	ansi_allocator allocator;
	clip_params params;
	params.num_bones = 5;
	params.num_samples = 1;
	scoped_tracks tracks(allocator, build_clip(allocator, params));
	TASK_CHECK(tracks.tracks != nullptr);

	const uint32_t num_bones = params.num_bones;

	batch_request requests[4];
	requests[0] = batch_request{ 0.0F, sample_rounding_policy::none };
	requests[1] = batch_request{ 0.5F, sample_rounding_policy::none };
	requests[2] = batch_request{ 1.0F, sample_rounding_policy::nearest };
	requests[3] = batch_request{ -0.5F, sample_rounding_policy::ceil };

	scoped_workspace workspace(allocator, 4);
	batch_decompression_context context;
	TASK_CHECK(context.initialize(*tracks.tracks, 4, workspace.ptr, workspace.size));

	rtm::vqvf batch_poses[4 * 5];
	batch_decode_stats stats;
	TASK_CHECK(context.decompress(requests, 4, batch_poses, 4 * num_bones, num_bones, &stats));

	// A single frame clip has no animated sub-track, the unpack counts reflect it
	TASK_CHECK(stats.endpoint_unpacks == 0);
	TASK_CHECK(stats.segment_metadata_visits == 0);
	TASK_CHECK(stats.sub_track_visits == 2 * num_bones);

	decompression_context<debug_transform_decompression_settings> reference;
	TASK_CHECK(reference.initialize(*tracks.tracks));

	rtm::vqvf reference_pose[5];
	for (uint32_t request_index = 0; request_index < 4; ++request_index)
	{
		reference_decompress(reference, requests[request_index].sample_time, requests[request_index].rounding_policy, reference_pose);
		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
			TASK_CHECK(pose_near(batch_poses[request_index * num_bones + bone_index], reference_pose[bone_index]));
	}
	return 0;
}

static int test_values_stride_gaps()
{
	ansi_allocator allocator;
	clip_params params;
	params.num_bones = 20;
	params.num_samples = 90;
	scoped_tracks tracks(allocator, build_clip(allocator, params));
	TASK_CHECK(tracks.tracks != nullptr);

	const uint32_t num_bones = params.num_bones;
	const uint32_t pose_stride = num_bones + 7;
	constexpr uint32_t num_requests = 6;

	batch_request requests[num_requests];
	const sample_rounding_policy policies[3] = { sample_rounding_policy::none, sample_rounding_policy::floor, sample_rounding_policy::nearest };
	for (uint32_t index = 0; index < num_requests; ++index)
		requests[index] = batch_request{ 0.4F + 0.37F * float(index), policies[index % 3] };

	scoped_workspace workspace(allocator, num_requests);
	batch_decompression_context context;
	TASK_CHECK(context.initialize(*tracks.tracks, num_requests, workspace.ptr, workspace.size));

	// Fill the whole output buffer with a sentinel pattern
	rtm::vqvf output[num_requests * 27];
	const rtm::vqvf sentinel = rtm::vqv_set(rtm::quat_set(7.0F, 8.0F, 9.0F, 10.0F), rtm::vector_set(11.0F), rtm::vector_set(12.0F));
	for (uint32_t index = 0; index < num_requests * pose_stride; ++index)
		output[index] = sentinel;

	batch_decode_stats stats;
	TASK_CHECK(context.decompress(requests, num_requests, output, num_requests * pose_stride, pose_stride, &stats));

	decompression_context<debug_transform_decompression_settings> reference;
	TASK_CHECK(reference.initialize(*tracks.tracks));

	rtm::vqvf reference_pose[20];
	for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
	{
		reference_decompress(reference, requests[request_index].sample_time, requests[request_index].rounding_policy, reference_pose);
		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
			TASK_CHECK(pose_near(output[request_index * pose_stride + bone_index], reference_pose[bone_index]));

		// The gap between the poses must be untouched
		for (uint32_t gap_index = num_bones; gap_index < pose_stride; ++gap_index)
		{
			const rtm::vqvf& element = output[request_index * pose_stride + gap_index];
			TASK_CHECK(rtm::vector_get_x(element.translation) == 11.0F);
			TASK_CHECK(rtm::vector_get_x(element.scale) == 12.0F);
		}
	}
	return 0;
}

//////////////////////////////////////////////////////////////////////////
// sharing: duplicate endpoints are merged and the statistics are exact
//////////////////////////////////////////////////////////////////////////

static int test_sharing_same_interval_endpoints()
{
	ansi_allocator allocator;
	clip_params params;
	TASK_CHECK(params.num_bones >= 64);
	scoped_tracks tracks(allocator, build_clip(allocator, params));
	TASK_CHECK(tracks.tracks != nullptr);

	const acl_impl::transform_tracks_header& transform_header = acl_impl::get_transform_tracks_header(*tracks.tracks);
	const uint32_t num_bones = params.num_bones;
	const uint32_t num_animated = transform_header.num_animated_rotation_sub_tracks + transform_header.num_animated_translation_sub_tracks;
	const uint32_t num_constant = transform_header.num_constant_rotation_samples + transform_header.num_constant_translation_samples;

	// 64 distinct requests that all land inside the same sampling interval
	// [frame 45, frame 46]: only two distinct frame endpoints are needed
	constexpr uint32_t num_requests = 64;
	batch_request requests[num_requests];
	for (uint32_t index = 0; index < num_requests; ++index)
		requests[index] = batch_request{ 1.5000F + 0.0003F * float(index), sample_rounding_policy::none };
	TASK_CHECK(1.5000F + 0.0003F * float(num_requests - 1) < 46.0F / 30.0F);

	scoped_workspace workspace(allocator, num_requests);
	batch_decompression_context context;
	TASK_CHECK(context.initialize(*tracks.tracks, num_requests, workspace.ptr, workspace.size));

	rtm::vqvf output[64 * 70];
	batch_decode_stats stats;
	TASK_CHECK(context.decompress(requests, num_requests, output, num_requests * num_bones, num_bones, &stats));

	// The unpack work corresponds to exactly two distinct frame endpoints.
	// The animated sub-track count comes from the compressed stream itself.
	TASK_CHECK(stats.endpoint_unpacks == 2 * num_animated);
	TASK_CHECK(stats.segment_metadata_visits == num_animated);
	TASK_CHECK(stats.sub_track_visits == 2 * num_bones);
	TASK_CHECK(stats.constant_unpacks == num_constant);

	// Every pose still matches the reference decoder
	decompression_context<debug_transform_decompression_settings> reference;
	TASK_CHECK(reference.initialize(*tracks.tracks));

	rtm::vqvf reference_pose[70];
	for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
	{
		reference_decompress(reference, requests[request_index].sample_time, requests[request_index].rounding_policy, reference_pose);
		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
			TASK_CHECK(pose_near(output[request_index * num_bones + bone_index], reference_pose[bone_index]));
	}
	return 0;
}

static int test_sharing_duplicate_times()
{
	ansi_allocator allocator;
	clip_params params;
	scoped_tracks tracks(allocator, build_clip(allocator, params));
	TASK_CHECK(tracks.tracks != nullptr);

	const acl_impl::transform_tracks_header& transform_header = acl_impl::get_transform_tracks_header(*tracks.tracks);
	const uint32_t num_bones = params.num_bones;
	const uint32_t num_animated = transform_header.num_animated_rotation_sub_tracks + transform_header.num_animated_translation_sub_tracks;

	scoped_workspace workspace(allocator, 32);
	batch_decompression_context context;
	TASK_CHECK(context.initialize(*tracks.tracks, 32, workspace.ptr, workspace.size));

	rtm::vqvf output[32 * 70];
	batch_decode_stats stats;

	// 32 identical requests on an exact keyframe time: one distinct frame
	{
		batch_request requests[32];
		for (uint32_t index = 0; index < 32; ++index)
			requests[index] = batch_request{ 1.5F, sample_rounding_policy::none };
		TASK_CHECK(context.decompress(requests, 32, output, 32 * num_bones, num_bones, &stats));
		TASK_CHECK(stats.endpoint_unpacks == num_animated);
		TASK_CHECK(stats.segment_metadata_visits == num_animated);
	}

	// 32 identical requests between two keyframes: two distinct frames
	{
		batch_request requests[32];
		for (uint32_t index = 0; index < 32; ++index)
			requests[index] = batch_request{ 1.51F, sample_rounding_policy::none };
		TASK_CHECK(context.decompress(requests, 32, output, 32 * num_bones, num_bones, &stats));
		TASK_CHECK(stats.endpoint_unpacks == 2 * num_animated);
	}

	// 32 requests on 32 distinct exact keyframe times: 32 distinct frames
	{
		batch_request requests[32];
		for (uint32_t index = 0; index < 32; ++index)
			requests[index] = batch_request{ float(index * 6) / 30.0F, sample_rounding_policy::floor };
		TASK_CHECK(context.decompress(requests, 32, output, 32 * num_bones, num_bones, &stats));
		TASK_CHECK(stats.endpoint_unpacks == 32 * num_animated);
		TASK_CHECK(stats.segment_metadata_visits <= num_animated * transform_header.num_segments);
		TASK_CHECK(stats.sub_track_visits == 2 * num_bones);
	}
	return 0;
}

static int test_sharing_stats_bounds()
{
	ansi_allocator allocator;
	clip_params params;
	scoped_tracks tracks(allocator, build_clip(allocator, params));
	TASK_CHECK(tracks.tracks != nullptr);

	const acl_impl::transform_tracks_header& transform_header = acl_impl::get_transform_tracks_header(*tracks.tracks);
	const uint32_t num_bones = params.num_bones;
	const uint32_t num_animated = transform_header.num_animated_rotation_sub_tracks + transform_header.num_animated_translation_sub_tracks;
	const uint32_t num_constant = transform_header.num_constant_rotation_samples + transform_header.num_constant_translation_samples;
	const float duration = tracks.tracks->get_finite_duration(sample_looping_policy::clamp);

	// Requests spread over the whole clip so that many segments are touched
	constexpr uint32_t num_requests = 48;
	batch_request requests[num_requests];
	const sample_rounding_policy policies[4] = { sample_rounding_policy::none, sample_rounding_policy::floor, sample_rounding_policy::ceil, sample_rounding_policy::nearest };
	for (uint32_t index = 0; index < num_requests; ++index)
		requests[index] = batch_request{ duration * float(index) / float(num_requests - 1), policies[index % 4] };

	scoped_workspace workspace(allocator, num_requests);
	batch_decompression_context context;
	TASK_CHECK(context.initialize(*tracks.tracks, num_requests, workspace.ptr, workspace.size));

	rtm::vqvf output[48 * 70];
	batch_decode_stats stats;
	TASK_CHECK(context.decompress(requests, num_requests, output, num_requests * num_bones, num_bones, &stats));

	// The counters stay within their documented bounds
	TASK_CHECK(stats.sub_track_visits == 2 * num_bones);
	TASK_CHECK(stats.sub_track_visits <= 3 * num_bones);
	TASK_CHECK(stats.constant_unpacks == num_constant);
	TASK_CHECK(stats.segment_metadata_visits <= num_animated * transform_header.num_segments);
	TASK_CHECK(stats.endpoint_unpacks <= num_animated * 2 * num_requests);

	// With A animated sub-tracks, S touched segments, and U distinct frames:
	// segment_metadata_visits == A * S and endpoint_unpacks == A * U
	TASK_CHECK(stats.segment_metadata_visits % num_animated == 0);
	TASK_CHECK(stats.endpoint_unpacks % num_animated == 0);
	TASK_CHECK(stats.segment_metadata_visits / num_animated >= 1);
	TASK_CHECK(stats.endpoint_unpacks / num_animated >= stats.segment_metadata_visits / num_animated);
	return 0;
}

//////////////////////////////////////////////////////////////////////////
// contracts: the API contract and the failure behavior
//////////////////////////////////////////////////////////////////////////

static int test_contracts_failure_atomicity()
{
	ansi_allocator allocator;
	clip_params params;
	params.num_bones = 20;
	params.num_samples = 60;
	scoped_tracks tracks(allocator, build_clip(allocator, params));
	TASK_CHECK(tracks.tracks != nullptr);

	const uint32_t num_bones = params.num_bones;
	constexpr uint32_t max_requests = 8;
	constexpr uint32_t out_capacity = max_requests * 20;

	scoped_workspace workspace(allocator, max_requests);
	batch_decompression_context context;
	TASK_CHECK(context.initialize(*tracks.tracks, max_requests, workspace.ptr, workspace.size));

	batch_request valid_requests[2];
	valid_requests[0] = batch_request{ 0.25F, sample_rounding_policy::none };
	valid_requests[1] = batch_request{ 0.75F, sample_rounding_policy::nearest };

	rtm::vqvf output[out_capacity];
	batch_decode_stats stats;

	const rtm::vqvf sentinel = rtm::vqv_set(rtm::quat_set(3.0F, 4.0F, 5.0F, 6.0F), rtm::vector_set(7.0F), rtm::vector_set(8.0F));
	const batch_decode_stats stats_sentinel{ 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF };

	auto output_and_stats_untouched = [&]() -> bool
	{
		for (uint32_t index = 0; index < out_capacity; ++index)
		{
			if (rtm::quat_get_x(output[index].rotation) != 3.0F
				|| rtm::vector_get_x(output[index].translation) != 7.0F
				|| rtm::vector_get_x(output[index].scale) != 8.0F)
				return false;
		}
		return stats.sub_track_visits == 0xDEADBEEF && stats.segment_metadata_visits == 0xDEADBEEF
			&& stats.endpoint_unpacks == 0xDEADBEEF && stats.constant_unpacks == 0xDEADBEEF;
	};

	auto expect_failure = [&](const batch_request* reqs, uint32_t count, rtm::vqvf* out, uint32_t capacity, uint32_t stride) -> bool
	{
		for (uint32_t index = 0; index < out_capacity; ++index)
			output[index] = sentinel;
		stats = stats_sentinel;
		if (context.decompress(reqs, count, out, capacity, stride, &stats))
			return false;
		return output_and_stats_untouched();
	};

	// An uninitialized context fails
	{
		batch_decompression_context empty_context;
		for (uint32_t index = 0; index < out_capacity; ++index)
			output[index] = sentinel;
		stats = stats_sentinel;
		TASK_CHECK(!empty_context.decompress(valid_requests, 2, output, out_capacity, num_bones, &stats));
		TASK_CHECK(output_and_stats_untouched());
	}

	// Too many requests for the context capacity
	TASK_CHECK(expect_failure(valid_requests, max_requests + 1, output, out_capacity + num_bones * 2, num_bones));

	// Output capacity too small by one element
	TASK_CHECK(expect_failure(valid_requests, 2, output, num_bones + num_bones - 1, num_bones));

	// Stride smaller than the bone count
	TASK_CHECK(expect_failure(valid_requests, 2, output, out_capacity, num_bones - 1));

	// Null request and output pointers with a non-zero request count
	TASK_CHECK(expect_failure(nullptr, 2, output, out_capacity, num_bones));
	TASK_CHECK(expect_failure(valid_requests, 2, nullptr, out_capacity, num_bones));

	// Non finite sample times
	{
		batch_request bad_requests[1];
		bad_requests[0] = batch_request{ std::nanf(""), sample_rounding_policy::none };
		TASK_CHECK(expect_failure(bad_requests, 1, output, out_capacity, num_bones));
		bad_requests[0] = batch_request{ std::numeric_limits<float>::infinity(), sample_rounding_policy::none };
		TASK_CHECK(expect_failure(bad_requests, 1, output, out_capacity, num_bones));
		bad_requests[0] = batch_request{ -std::numeric_limits<float>::infinity(), sample_rounding_policy::none };
		TASK_CHECK(expect_failure(bad_requests, 1, output, out_capacity, num_bones));
	}

	// Unsupported rounding policies
	{
		batch_request bad_requests[1];
		bad_requests[0] = batch_request{ 0.5F, sample_rounding_policy::per_track };
		TASK_CHECK(expect_failure(bad_requests, 1, output, out_capacity, num_bones));
		bad_requests[0] = batch_request{ 0.5F, static_cast<sample_rounding_policy>(77) };
		TASK_CHECK(expect_failure(bad_requests, 1, output, out_capacity, num_bones));
	}

	// A valid call succeeds after the failures and produces the right values
	for (uint32_t index = 0; index < out_capacity; ++index)
		output[index] = sentinel;
	TASK_CHECK(context.decompress(valid_requests, 2, output, out_capacity, num_bones, &stats));

	decompression_context<debug_transform_decompression_settings> reference;
	TASK_CHECK(reference.initialize(*tracks.tracks));
	rtm::vqvf reference_pose[20];
	for (uint32_t request_index = 0; request_index < 2; ++request_index)
	{
		reference_decompress(reference, valid_requests[request_index].sample_time, valid_requests[request_index].rounding_policy, reference_pose);
		for (uint32_t bone_index = 0; bone_index < num_bones; ++bone_index)
			TASK_CHECK(pose_near(output[request_index * num_bones + bone_index], reference_pose[bone_index]));
	}
	return 0;
}

static int test_contracts_initialize()
{
	ansi_allocator allocator;
	clip_params params;
	params.num_bones = 12;
	params.num_samples = 40;
	scoped_tracks tracks(allocator, build_clip(allocator, params));
	TASK_CHECK(tracks.tracks != nullptr);

	const uint32_t max_requests = 4;
	const size_t workspace_size = batch_decompression_context::query_workspace_size(max_requests);
	TASK_CHECK(workspace_size != 0);

	void* workspace = allocator.allocate(workspace_size + 64, 32);
	TASK_CHECK(workspace != nullptr);

	batch_decompression_context context;
	TASK_CHECK(context.get_workspace_size() == 0);

	// Null workspace
	TASK_CHECK(!context.initialize(*tracks.tracks, max_requests, nullptr, workspace_size));

	// Misaligned workspace: 8 bytes past a 32 byte aligned address
	void* misaligned = static_cast<uint8_t*>(workspace) + 8;
	TASK_CHECK(!context.initialize(*tracks.tracks, max_requests, misaligned, workspace_size));

	// Workspace too small
	TASK_CHECK(!context.initialize(*tracks.tracks, max_requests, workspace, workspace_size - 16));

	// Zero request capacity
	TASK_CHECK(!context.initialize(*tracks.tracks, 0, workspace, workspace_size));

	// A larger workspace than required is accepted
	TASK_CHECK(context.initialize(*tracks.tracks, max_requests, workspace, workspace_size + 64));
	context.reset();

	// A 16 byte aligned workspace is accepted
	TASK_CHECK(context.initialize(*tracks.tracks, max_requests, workspace, workspace_size));
	TASK_CHECK(context.get_workspace_size() == workspace_size);
	context.reset();

	allocator.deallocate(workspace, workspace_size + 64);

	// Unsupported clips are rejected
	{
		scoped_tracks scalar_clip(allocator, build_scalar_clip(allocator));
		TASK_CHECK(scalar_clip.tracks != nullptr);
		scoped_workspace scalar_workspace(allocator, max_requests);
		TASK_CHECK(!context.initialize(*scalar_clip.tracks, max_requests, scalar_workspace.ptr, scalar_workspace.size));
	}
	{
		scoped_tracks vqf_clip(allocator, build_vqf_clip(allocator));
		TASK_CHECK(vqf_clip.tracks != nullptr);
		scoped_workspace vqf_workspace(allocator, max_requests);
		TASK_CHECK(!context.initialize(*vqf_clip.tracks, max_requests, vqf_workspace.ptr, vqf_workspace.size));
	}
	{
		scoped_tracks vqsf_clip(allocator, build_vqsf_clip(allocator));
		TASK_CHECK(vqsf_clip.tracks != nullptr);
		scoped_workspace vqsf_workspace(allocator, max_requests);
		TASK_CHECK(!context.initialize(*vqsf_clip.tracks, max_requests, vqsf_workspace.ptr, vqsf_workspace.size));
	}
	{
		scoped_tracks scale_clip(allocator, build_scale_clip(allocator));
		TASK_CHECK(scale_clip.tracks != nullptr);
		TASK_CHECK(acl_impl::get_tracks_header(*scale_clip.tracks).get_has_scale());
		scoped_workspace scale_workspace(allocator, max_requests);
		TASK_CHECK(!context.initialize(*scale_clip.tracks, max_requests, scale_workspace.ptr, scale_workspace.size));
	}
	{
		scoped_tracks stripped_clip(allocator, build_stripped_clip(allocator));
		TASK_CHECK(stripped_clip.tracks != nullptr);
		TASK_CHECK(stripped_clip.tracks->has_stripped_keyframes());
		scoped_workspace stripped_workspace(allocator, max_requests);
		TASK_CHECK(!context.initialize(*stripped_clip.tracks, max_requests, stripped_workspace.ptr, stripped_workspace.size));
	}
	{
		scoped_tracks additive_clip(allocator, build_additive_clip(allocator));
		TASK_CHECK(additive_clip.tracks != nullptr);
		TASK_CHECK(additive_clip.tracks->get_default_scale() != 1);
		scoped_workspace additive_workspace(allocator, max_requests);
		TASK_CHECK(!context.initialize(*additive_clip.tracks, max_requests, additive_workspace.ptr, additive_workspace.size));
	}
	{
		compressed_database* database = nullptr;
		scoped_tracks database_clip(allocator, build_database_clip(allocator, database));
		TASK_CHECK(database_clip.tracks != nullptr);
		TASK_CHECK(database_clip.tracks->has_database());
		scoped_workspace database_workspace(allocator, max_requests);
		TASK_CHECK(!context.initialize(*database_clip.tracks, max_requests, database_workspace.ptr, database_workspace.size));
		if (database != nullptr)
			allocator.deallocate(database, database->get_size());
	}

	// A supported clip is accepted as a control
	{
		scoped_workspace control_workspace(allocator, max_requests);
		TASK_CHECK(context.initialize(*tracks.tracks, max_requests, control_workspace.ptr, control_workspace.size));
	}
	return 0;
}

static int test_contracts_zero_requests()
{
	ansi_allocator allocator;
	clip_params params;
	params.num_bones = 12;
	params.num_samples = 40;
	scoped_tracks tracks(allocator, build_clip(allocator, params));
	TASK_CHECK(tracks.tracks != nullptr);

	scoped_workspace workspace(allocator, 4);
	batch_decompression_context context;
	TASK_CHECK(context.initialize(*tracks.tracks, 4, workspace.ptr, workspace.size));

	// Zero requests allow null pointers and succeed with zero statistics
	batch_decode_stats stats{ 1, 2, 3, 4 };
	TASK_CHECK(context.decompress(nullptr, 0, nullptr, 0, 0, &stats));
	TASK_CHECK(stats.sub_track_visits == 0 && stats.segment_metadata_visits == 0 && stats.endpoint_unpacks == 0 && stats.constant_unpacks == 0);

	// Null statistics are allowed
	TASK_CHECK(context.decompress(nullptr, 0, nullptr, 0, 0, nullptr));

	// Zero requests with valid buffers leave the output untouched
	rtm::vqvf output[4 * 12];
	const rtm::vqvf sentinel = rtm::vqv_set(rtm::quat_set(1.0F, 2.0F, 3.0F, 4.0F), rtm::vector_set(5.0F), rtm::vector_set(6.0F));
	for (uint32_t index = 0; index < 4 * 12; ++index)
		output[index] = sentinel;
	batch_request requests[1];
	requests[0] = batch_request{ 0.5F, sample_rounding_policy::none };
	stats = batch_decode_stats{ 9, 9, 9, 9 };
	TASK_CHECK(context.decompress(requests, 0, output, 4 * 12, 12, &stats));
	TASK_CHECK(stats.endpoint_unpacks == 0);
	TASK_CHECK(rtm::vector_get_x(output[0].translation) == 5.0F);
	return 0;
}

static int test_contracts_no_heap_allocation()
{
	ansi_allocator allocator;
	clip_params params;
	scoped_tracks tracks(allocator, build_clip(allocator, params));
	TASK_CHECK(tracks.tracks != nullptr);

	const uint32_t num_bones = params.num_bones;
	constexpr uint32_t num_requests = 64;

	scoped_workspace workspace(allocator, num_requests);

	g_num_heap_allocs.store(0);
	g_num_heap_frees.store(0);
	g_track_heap = true;

	batch_decompression_context context;
	const bool initialized = context.initialize(*tracks.tracks, num_requests, workspace.ptr, workspace.size);

	batch_request requests[num_requests];
	for (uint32_t index = 0; index < num_requests; ++index)
		requests[index] = batch_request{ 0.1F + 0.09F * float(index), sample_rounding_policy::none };

	rtm::vqvf output[num_requests * 70];
	batch_decode_stats stats;
	const bool decompressed = initialized && context.decompress(requests, num_requests, output, num_requests * num_bones, num_bones, &stats);

	g_track_heap = false;

	TASK_CHECK(initialized);
	TASK_CHECK(decompressed);
	TASK_CHECK(g_num_heap_allocs.load() == 0);
	TASK_CHECK(g_num_heap_frees.load() == 0);
	return 0;
}

static int test_contracts_interleaved_contexts()
{
	ansi_allocator allocator;
	clip_params params_a;
	params_a.num_bones = 24;
	params_a.num_samples = 90;
	scoped_tracks tracks_a(allocator, build_clip(allocator, params_a));

	clip_params params_b;
	params_b.num_bones = 16;
	params_b.num_samples = 64;
	params_b.rotation_format = rotation_format8::quatf_drop_w_full;
	params_b.translation_format = vector_format8::vector3f_full;
	scoped_tracks tracks_b(allocator, build_clip(allocator, params_b));
	TASK_CHECK(tracks_a.tracks != nullptr && tracks_b.tracks != nullptr);

	constexpr uint32_t num_requests = 8;
	batch_request requests_a[num_requests];
	batch_request requests_b[num_requests];
	for (uint32_t index = 0; index < num_requests; ++index)
	{
		requests_a[index] = batch_request{ 0.13F + 0.21F * float(index), sample_rounding_policy::none };
		requests_b[index] = batch_request{ 0.07F + 0.19F * float(index), sample_rounding_policy::nearest };
	}

	scoped_workspace workspace_a(allocator, num_requests);
	scoped_workspace workspace_b(allocator, num_requests);
	batch_decompression_context context_a;
	batch_decompression_context context_b;
	TASK_CHECK(context_a.initialize(*tracks_a.tracks, num_requests, workspace_a.ptr, workspace_a.size));
	TASK_CHECK(context_b.initialize(*tracks_b.tracks, num_requests, workspace_b.ptr, workspace_b.size));

	rtm::vqvf output_a[num_requests * 24];
	rtm::vqvf output_b[num_requests * 16];
	batch_decode_stats stats_a;
	batch_decode_stats stats_b;

	// Interleave the calls of the two contexts
	TASK_CHECK(context_a.decompress(requests_a, 4, output_a, 4 * 24, 24, &stats_a));
	TASK_CHECK(context_b.decompress(requests_b, 4, output_b, 4 * 16, 16, &stats_b));
	TASK_CHECK(context_a.decompress(requests_a + 4, 4, output_a + 4 * 24, 4 * 24, 24, &stats_a));
	TASK_CHECK(context_b.decompress(requests_b + 4, 4, output_b + 4 * 16, 4 * 16, 16, &stats_b));

	// The interleaved results match the reference decoder
	decompression_context<debug_transform_decompression_settings> reference_a;
	decompression_context<debug_transform_decompression_settings> reference_b;
	TASK_CHECK(reference_a.initialize(*tracks_a.tracks));
	TASK_CHECK(reference_b.initialize(*tracks_b.tracks));

	rtm::vqvf reference_pose[24];
	for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
	{
		reference_decompress(reference_a, requests_a[request_index].sample_time, requests_a[request_index].rounding_policy, reference_pose);
		for (uint32_t bone_index = 0; bone_index < 24; ++bone_index)
			TASK_CHECK(pose_near(output_a[request_index * 24 + bone_index], reference_pose[bone_index]));

		reference_decompress(reference_b, requests_b[request_index].sample_time, requests_b[request_index].rounding_policy, reference_pose);
		for (uint32_t bone_index = 0; bone_index < 16; ++bone_index)
			TASK_CHECK(pose_near(output_b[request_index * 16 + bone_index], reference_pose[bone_index]));
	}
	return 0;
}

//////////////////////////////////////////////////////////////////////////
// Test registration
//////////////////////////////////////////////////////////////////////////

const task_case task_cases[] =
{
	{ "values", "mixed_subtracks", &test_values_mixed_subtracks },
	{ "values", "hierarchy_skinning", &test_values_hierarchy_skinning },
	{ "values", "loop_wrap_clamp", &test_values_loop_wrap_clamp },
	{ "values", "full_precision", &test_values_full_precision },
	{ "values", "single_frame", &test_values_single_frame },
	{ "values", "stride_gaps", &test_values_stride_gaps },
	{ "sharing", "same_interval_endpoints", &test_sharing_same_interval_endpoints },
	{ "sharing", "duplicate_times", &test_sharing_duplicate_times },
	{ "sharing", "stats_bounds", &test_sharing_stats_bounds },
	{ "contracts", "failure_atomicity", &test_contracts_failure_atomicity },
	{ "contracts", "initialize", &test_contracts_initialize },
	{ "contracts", "zero_requests", &test_contracts_zero_requests },
	{ "contracts", "no_heap_allocation", &test_contracts_no_heap_allocation },
	{ "contracts", "interleaved_contexts", &test_contracts_interleaved_contexts },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
