#pragma once

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

// Included only once from batch_decompress.h

#include "acl/version.h"
#include "acl/core/algorithm_types.h"
#include "acl/core/compressed_tracks_version.h"
#include "acl/core/interpolation_utils.h"
#include "acl/core/memory_utils.h"
#include "acl/core/track_formats.h"
#include "acl/core/impl/compressed_headers.h"
#include "acl/core/impl/compiler_utils.h"
#include "acl/math/quat_packing.h"
#include "acl/math/quatf.h"
#include "acl/math/vector4_packing.h"

#include <rtm/quatf.h>
#include <rtm/scalarf.h>
#include <rtm/vector4f.h>
#include <rtm/vqvf.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

ACL_IMPL_FILE_PRAGMA_PUSH

namespace acl
{
	ACL_IMPL_VERSION_NAMESPACE_BEGIN

	namespace acl_impl
	{
		//////////////////////////////////////////////////////////////////////////
		// Workspace records for the batch decompression context.
		// Every record size is a multiple of 16 bytes so that every section of
		// the workspace stays aligned to 16 bytes.
		//////////////////////////////////////////////////////////////////////////

		// The resolved sampling state of one request
		struct batch_request_info_t
		{
			uint32_t key_frame0;			// Clip keyframe index before the sample time
			uint32_t key_frame1;			// Clip keyframe index after the sample time
			float interpolation_alpha;		// Interpolation alpha after the rounding policy
			uint32_t endpoint0;				// Index of the first required frame in the frame table
			uint32_t endpoint1;				// Index of the second required frame in the frame table
			uint32_t padding0;
			uint32_t padding1;
			uint32_t padding2;
		};

		static_assert(sizeof(batch_request_info_t) == 32, "Unexpected size");

		// One required frame endpoint of one request, used to merge duplicates
		struct batch_endpoint_t
		{
			uint32_t clip_frame;			// Clip keyframe index
			uint32_t request_index;			// Request that requires this frame
			uint32_t request_slot;			// 0 for endpoint0, 1 for endpoint1
			uint32_t padding;
		};

		static_assert(sizeof(batch_endpoint_t) == 16, "Unexpected size");

		// One distinct frame required by the requests
		struct batch_frame_info_t
		{
			uint32_t clip_frame;			// Clip keyframe index
			uint32_t segment_pos;			// Index in the touched segment table
			uint32_t bit_offset;			// Bit offset of the frame in the segment animated data
			uint32_t padding;
		};

		static_assert(sizeof(batch_frame_info_t) == 16, "Unexpected size");

		// One segment touched by the requests, with its resolved data pointers
		// and the running bit offsets used to locate each animated sub-track
		struct batch_segment_info_t
		{
			const uint8_t* format_rotation;			// Rotation per sub-track metadata base
			const uint8_t* range_rotation;			// Rotation segment range data base
			const uint8_t* format_translation;		// Translation per sub-track metadata base
			const uint8_t* range_translation;		// Translation segment range data base
			const uint8_t* animated_data;			// Animated sample data base
			uint32_t segment_index;					// Segment index in the clip
			uint32_t pose_bit_size;					// Bits of one animated frame in this segment
			uint32_t rotation_bit_offset;			// Running bit offset of animated rotations
			uint32_t translation_bit_offset;		// Running bit offset of animated translations
			uint32_t rotation_current_offset;		// Bit offset of the current rotation sub-track
			uint32_t translation_current_offset;	// Bit offset of the current translation sub-track
			uint32_t rotation_current_num_bits;		// Bits per component of the current rotation
			uint32_t translation_current_num_bits;	// Bits per component of the current translation
			uint32_t padding;
		};

		static_assert(sizeof(batch_segment_info_t) == 80, "Unexpected size");

		// The workspace sections, carved out of the caller provided buffer
		struct batch_workspace_t
		{
			batch_request_info_t* requests;
			batch_endpoint_t* endpoints;
			batch_frame_info_t* frames;
			batch_segment_info_t* segments;
			rtm::vector4f* values;
		};

		// Carves the workspace into its sections. Every section stays aligned
		// to 16 bytes because the base is aligned and every stride is a
		// multiple of 16 bytes.
		inline void batch_carve_workspace(void* workspace, uint32_t max_requests, batch_workspace_t& out)
		{
			uint8_t* ptr = static_cast<uint8_t*>(workspace);
			out.requests = reinterpret_cast<batch_request_info_t*>(ptr);
			ptr += sizeof(batch_request_info_t) * max_requests;
			out.endpoints = reinterpret_cast<batch_endpoint_t*>(ptr);
			ptr += sizeof(batch_endpoint_t) * 2 * max_requests;
			out.frames = reinterpret_cast<batch_frame_info_t*>(ptr);
			ptr += sizeof(batch_frame_info_t) * 2 * max_requests;
			out.segments = reinterpret_cast<batch_segment_info_t*>(ptr);
			ptr += sizeof(batch_segment_info_t) * 2 * max_requests;
			out.values = reinterpret_cast<rtm::vector4f*>(ptr);
		}

		// Returns the number of bits one animated rotation sample uses in a segment
		inline uint32_t batch_rotation_sample_bits(rotation_format8 rotation_format, uint32_t num_bits_at_bit_rate)
		{
			if (rotation_format == rotation_format8::quatf_drop_w_variable)
			{
				// A bit rate of 0 stores no sample, the raw bit rate stores 32 bits
				// per component and is stored as 31
				return num_bits_at_bit_rate == 0 ? 0 : (num_bits_at_bit_rate >= 31 ? 96 : num_bits_at_bit_rate * 3);
			}

			return rotation_format == rotation_format8::quatf_full ? 128 : 96;
		}

		// Returns the number of bits one animated translation sample uses in a segment
		inline uint32_t batch_translation_sample_bits(vector_format8 translation_format, uint32_t num_bits_at_bit_rate)
		{
			if (translation_format == vector_format8::vector3f_variable)
				return num_bits_at_bit_rate == 0 ? 0 : (num_bits_at_bit_rate >= 31 ? 96 : num_bits_at_bit_rate * 3);

			return 96;
		}

		// Unpacks one animated rotation sample and reconstructs the W component.
		// Rotations are normalized after the reconstruction, this matches the
		// rotation_normalization_policy_t::always policy of the reference decoder.
		inline rtm::quatf RTM_SIMD_CALL batch_unpack_rotation(
			rotation_format8 rotation_format,
			bool has_segments,
			const batch_segment_info_t& segment,
			const uint8_t* clip_range_rotations,
			uint32_t num_animated_rotations,
			uint32_t animated_index,
			uint32_t bit_offset)
		{
			rtm::vector4f sample;

			if (rotation_format == rotation_format8::quatf_drop_w_variable)
			{
				const uint32_t num_bits = segment.rotation_current_num_bits;
				if (num_bits == 0)
				{
					// Constant bit rate, the sample is stored in the segment range
					// data. The data is swizzled in groups of 4 sub-tracks.
					const uint8_t* range_data = segment.range_rotation + (animated_index & ~3U) * 6 + (animated_index & 3U);
					const uint32_t x = (uint32_t(range_data[0]) << 8) | range_data[4];
					const uint32_t y = (uint32_t(range_data[8]) << 8) | range_data[12];
					const uint32_t z = (uint32_t(range_data[16]) << 8) | range_data[20];
					sample = rtm::vector_mul(rtm::vector_set(float(x), float(y), float(z), 0.0F), 1.0F / 65535.0F);

					// Only the clip range applies
					const uint32_t group_size = std::min<uint32_t>(num_animated_rotations - (animated_index & ~3U), 4);
					const float* clip_range_data = bit_cast<const float*>(clip_range_rotations) + (animated_index & ~3U) * 6 + (animated_index & 3U);
					const rtm::vector4f clip_range_min = rtm::vector_set(clip_range_data[0], clip_range_data[group_size], clip_range_data[group_size * 2], 0.0F);
					const rtm::vector4f clip_range_extent = rtm::vector_set(clip_range_data[group_size * 3], clip_range_data[group_size * 4], clip_range_data[group_size * 5], 0.0F);
					sample = rtm::vector_mul_add(sample, clip_range_extent, clip_range_min);
				}
				else if (num_bits >= 31)
				{
					// Raw bit rate, the sample is stored with full precision, no ranges apply
					sample = unpack_vector3_96_unsafe(segment.animated_data, bit_offset);
				}
				else
				{
					sample = unpack_vector3_uXX_unsafe(num_bits, segment.animated_data, bit_offset);

					if (has_segments)
					{
						// The segment range data is swizzled in groups of 4 sub-tracks
						const uint8_t* range_data = segment.range_rotation + (animated_index & ~3U) * 6 + (animated_index & 3U);
						const rtm::vector4f segment_range_min = rtm::vector_mul(rtm::vector_set(float(range_data[0]), float(range_data[4]), float(range_data[8]), 0.0F), 1.0F / 255.0F);
						const rtm::vector4f segment_range_extent = rtm::vector_mul(rtm::vector_set(float(range_data[12]), float(range_data[16]), float(range_data[20]), 0.0F), 1.0F / 255.0F);
						sample = rtm::vector_mul_add(sample, segment_range_extent, segment_range_min);
					}

					const uint32_t group_size = std::min<uint32_t>(num_animated_rotations - (animated_index & ~3U), 4);
					const float* clip_range_data = bit_cast<const float*>(clip_range_rotations) + (animated_index & ~3U) * 6 + (animated_index & 3U);
					const rtm::vector4f clip_range_min = rtm::vector_set(clip_range_data[0], clip_range_data[group_size], clip_range_data[group_size * 2], 0.0F);
					const rtm::vector4f clip_range_extent = rtm::vector_set(clip_range_data[group_size * 3], clip_range_data[group_size * 4], clip_range_data[group_size * 5], 0.0F);
					sample = rtm::vector_mul_add(sample, clip_range_extent, clip_range_min);
				}

				const rtm::quatf rotation = quat_from_positive_w_stable(sample);
				return quat_normalize_stable(rotation);
			}
			else if (rotation_format == rotation_format8::quatf_full)
			{
				sample = unpack_vector4_128_unsafe(segment.animated_data, bit_offset);
				return rtm::vector_to_quat(sample);
			}
			else
			{
				// rotation_format8::quatf_drop_w_full
				sample = unpack_vector3_96_unsafe(segment.animated_data, bit_offset);
				const rtm::quatf rotation = quat_from_positive_w_stable(sample);
				return quat_normalize_stable(rotation);
			}
		}

		// Unpacks one animated translation sample
		inline rtm::vector4f RTM_SIMD_CALL batch_unpack_translation(
			vector_format8 translation_format,
			bool has_segments,
			const batch_segment_info_t& segment,
			const uint8_t* clip_range_translations,
			uint32_t animated_index,
			uint32_t bit_offset)
		{
			if (translation_format == vector_format8::vector3f_variable)
			{
				const uint32_t num_bits = segment.translation_current_num_bits;
				const uint8_t* range_data = segment.range_translation + animated_index * 6;

				rtm::vector4f sample;
				if (num_bits == 0)
				{
					// Constant bit rate, the sample is stored in the segment range data
					sample = unpack_vector3_u48_unsafe(range_data);

					// Only the clip range applies
					const rtm::vector4f clip_range_min = rtm::vector_load(clip_range_translations + animated_index * 24);
					const rtm::vector4f clip_range_extent = rtm::vector_load(clip_range_translations + animated_index * 24 + sizeof(rtm::float3f));
					sample = rtm::vector_mul_add(sample, clip_range_extent, clip_range_min);
				}
				else if (num_bits >= 31)
				{
					// Raw bit rate, the sample is stored with full precision, no ranges apply
					sample = unpack_vector3_96_unsafe(segment.animated_data, bit_offset);
				}
				else
				{
					sample = unpack_vector3_uXX_unsafe(num_bits, segment.animated_data, bit_offset);

					if (has_segments)
					{
						const rtm::vector4f segment_range_min = unpack_vector3_u24_unsafe(range_data);
						const rtm::vector4f segment_range_extent = unpack_vector3_u24_unsafe(range_data + 3);
						sample = rtm::vector_mul_add(sample, segment_range_extent, segment_range_min);
					}

					const rtm::vector4f clip_range_min = rtm::vector_load(clip_range_translations + animated_index * 24);
					const rtm::vector4f clip_range_extent = rtm::vector_load(clip_range_translations + animated_index * 24 + sizeof(rtm::float3f));
					sample = rtm::vector_mul_add(sample, clip_range_extent, clip_range_min);
				}

				return sample;
			}
			else
			{
				// vector_format8::vector3f_full
				return unpack_vector3_96_unsafe(segment.animated_data, bit_offset);
			}
		}

		// Unpacks one constant rotation sample
		inline rtm::quatf RTM_SIMD_CALL batch_unpack_constant_rotation(
			rotation_format8 rotation_format,
			const uint8_t* constant_rotations,
			uint32_t num_constant_rotations,
			uint32_t constant_index)
		{
			if (rotation_format == rotation_format8::quatf_full)
				return unpack_quat_128(constant_rotations + constant_index * sizeof(rtm::float4f));

			// The data is swizzled in groups of 4 sub-tracks, the last group is packed tight
			const uint32_t group_offset = constant_index & ~3U;
			const uint32_t group_size = std::min<uint32_t>(num_constant_rotations - group_offset, 4);
			const float* constant_data = bit_cast<const float*>(constant_rotations) + group_offset * 3 + (constant_index & 3U);
			const float x = constant_data[0];
			const float y = constant_data[group_size];
			const float z = constant_data[group_size * 2];
			const rtm::quatf rotation = quat_from_positive_w_stable(rtm::vector_set(x, y, z, 0.0F));
			return quat_normalize_stable(rotation);
		}
	}

	//////////////////////////////////////////////////////////////////////////
	// batch_decompression_context implementation
	//////////////////////////////////////////////////////////////////////////

	inline batch_decompression_context::batch_decompression_context()
		: m_tracks(nullptr)
		, m_workspace(nullptr)
		, m_max_requests(0)
		, m_num_tracks(0)
		, m_num_samples(0)
		, m_sample_rate(0.0F)
		, m_clip_duration(0.0F)
		, m_looping_policy(sample_looping_policy::clamp)
	{
	}

	inline batch_decompression_context::~batch_decompression_context()
	{
	}

	inline size_t batch_decompression_context::query_workspace_size(uint32_t max_requests)
	{
		const size_t num_requests = max_requests;
		return sizeof(acl_impl::batch_request_info_t) * num_requests
			+ sizeof(acl_impl::batch_endpoint_t) * 2 * num_requests
			+ sizeof(acl_impl::batch_frame_info_t) * 2 * num_requests
			+ sizeof(acl_impl::batch_segment_info_t) * 2 * num_requests
			+ sizeof(rtm::vector4f) * 2 * num_requests;
	}

	inline bool batch_decompression_context::initialize(const compressed_tracks& tracks, uint32_t max_requests, void* workspace, size_t workspace_size)
	{
		reset();

		if (max_requests == 0)
			return false;	// A context must serve at least one request per call

		if (workspace == nullptr || !is_aligned_to(workspace, 16))
			return false;	// The workspace must be present and aligned to 16 bytes

		if (workspace_size < query_workspace_size(max_requests))
			return false;	// The workspace is too small

		if (tracks.get_algorithm_type() != algorithm_type8::uniformly_sampled)
			return false;	// Only the uniformly sampled algorithm is supported

		if (tracks.get_track_type() != track_type8::vqvf)
			return false;	// Only vqvf transform tracks are supported

		const compressed_tracks_version16 version = tracks.get_version();
		if (version < compressed_tracks_version16::v02_01_99_1 || version > compressed_tracks_version16::latest)
			return false;	// Only the current binary format is supported

		if (tracks.has_database())
			return false;	// Databases are not supported

		if (tracks.has_stripped_keyframes())
			return false;	// Stripped keyframes are not supported

		const acl_impl::tracks_header& header = acl_impl::get_tracks_header(tracks);
		if (header.get_has_scale())
			return false;	// Scale is not supported

		if (tracks.get_default_scale() != 1 || !tracks.has_trivial_default_values())
			return false;	// Additive clips and non trivial default values are not supported

		const rotation_format8 rotation_format = header.get_rotation_format();
		if (rotation_format != rotation_format8::quatf_full
			&& rotation_format != rotation_format8::quatf_drop_w_full
			&& rotation_format != rotation_format8::quatf_drop_w_variable)
			return false;	// Unknown rotation format

		const vector_format8 translation_format = header.get_translation_format();
		if (translation_format != vector_format8::vector3f_full
			&& translation_format != vector_format8::vector3f_variable)
			return false;	// Unknown translation format

		m_tracks = &tracks;
		m_workspace = workspace;
		m_max_requests = max_requests;
		m_num_tracks = header.num_tracks;
		m_num_samples = header.num_samples;
		m_sample_rate = header.sample_rate;
		m_looping_policy = tracks.get_looping_policy();
		m_clip_duration = tracks.get_finite_duration(m_looping_policy);
		return true;
	}

	inline void batch_decompression_context::reset()
	{
		m_tracks = nullptr;
		m_workspace = nullptr;
		m_max_requests = 0;
		m_num_tracks = 0;
		m_num_samples = 0;
		m_sample_rate = 0.0F;
		m_clip_duration = 0.0F;
		m_looping_policy = sample_looping_policy::clamp;
	}

	inline void batch_decompression_context::set_looping_policy(sample_looping_policy policy)
	{
		if (!is_initialized())
			return;

		if (policy == sample_looping_policy::as_compressed)
			policy = m_tracks->get_looping_policy();

		if (policy != sample_looping_policy::clamp && policy != sample_looping_policy::wrap)
			return;	// Only clamp and wrap are supported

		if (m_looping_policy != policy)
		{
			m_looping_policy = policy;
			m_clip_duration = m_tracks->get_finite_duration(policy);
		}
	}

	inline bool batch_decompression_context::decompress(const batch_request* requests, uint32_t num_requests, rtm::vqvf* out, uint32_t out_capacity, uint32_t pose_stride, batch_decode_stats* out_stats)
	{
		if (!is_initialized())
			return false;

		if (num_requests > m_max_requests)
			return false;	// The request count exceeds the context capacity

		const uint32_t num_tracks = m_num_tracks;

		if (num_requests != 0)
		{
			if (requests == nullptr || out == nullptr)
				return false;

			if (pose_stride < num_tracks)
				return false;	// The stride cannot hold one pose

			// Use 64 bit arithmetic to avoid overflow
			const uint64_t required_capacity = uint64_t(num_requests - 1) * pose_stride + num_tracks;
			if (required_capacity > uint64_t(out_capacity))
				return false;	// The output buffer cannot hold every pose
		}

		for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
		{
			const batch_request& request = requests[request_index];
			if (!rtm::scalar_is_finite(request.sample_time))
				return false;	// The sample time must be finite

			const sample_rounding_policy rounding_policy = request.rounding_policy;
			if (rounding_policy != sample_rounding_policy::none
				&& rounding_policy != sample_rounding_policy::floor
				&& rounding_policy != sample_rounding_policy::ceil
				&& rounding_policy != sample_rounding_policy::nearest)
				return false;	// The rounding policy is not supported
		}

		if (num_requests == 0 || num_tracks == 0)
		{
			// Nothing to write, succeed with zero statistics
			if (out_stats != nullptr)
				*out_stats = batch_decode_stats{ 0, 0, 0, 0 };
			return true;
		}

		const compressed_tracks& tracks = *m_tracks;
		const acl_impl::tracks_header& header = acl_impl::get_tracks_header(tracks);
		const acl_impl::transform_tracks_header& transform_header = acl_impl::get_transform_tracks_header(tracks);

		const rotation_format8 rotation_format = header.get_rotation_format();
		const vector_format8 translation_format = header.get_translation_format();
		const bool has_segments = transform_header.has_multiple_segments();

		acl_impl::batch_workspace_t workspace;
		acl_impl::batch_carve_workspace(m_workspace, m_max_requests, workspace);

		acl_impl::batch_request_info_t* request_infos = workspace.requests;
		acl_impl::batch_endpoint_t* endpoints = workspace.endpoints;
		acl_impl::batch_frame_info_t* frames = workspace.frames;
		acl_impl::batch_segment_info_t* segments = workspace.segments;
		rtm::vector4f* endpoint_values = workspace.values;

		// Resolve the sampling state of every request and collect the frame
		// endpoints that each request needs. A request needs one frame when its
		// alpha selects a single keyframe, two frames otherwise.
		for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
		{
			const batch_request& request = requests[request_index];

			// Clamp for safety, matches the reference decoder
			const float sample_time = rtm::scalar_clamp(request.sample_time, 0.0F, m_clip_duration);

			uint32_t key_frame0;
			uint32_t key_frame1;
			float interpolation_alpha;
			find_linear_interpolation_samples_with_sample_rate(m_num_samples, m_sample_rate, sample_time, request.rounding_policy, m_looping_policy, key_frame0, key_frame1, interpolation_alpha);

			acl_impl::batch_request_info_t& info = request_infos[request_index];
			info.key_frame0 = key_frame0;
			info.key_frame1 = key_frame1;
			info.interpolation_alpha = interpolation_alpha;

			acl_impl::batch_endpoint_t& endpoint0 = endpoints[request_index * 2];
			acl_impl::batch_endpoint_t& endpoint1 = endpoints[request_index * 2 + 1];
			if (interpolation_alpha <= 0.0F)
			{
				endpoint0 = acl_impl::batch_endpoint_t{ key_frame0, request_index, 0, 0 };
				endpoint1 = acl_impl::batch_endpoint_t{ key_frame0, request_index, 1, 0 };
			}
			else if (interpolation_alpha >= 1.0F)
			{
				endpoint0 = acl_impl::batch_endpoint_t{ key_frame1, request_index, 0, 0 };
				endpoint1 = acl_impl::batch_endpoint_t{ key_frame1, request_index, 1, 0 };
			}
			else
			{
				endpoint0 = acl_impl::batch_endpoint_t{ key_frame0, request_index, 0, 0 };
				endpoint1 = acl_impl::batch_endpoint_t{ key_frame1, request_index, 1, 0 };
			}
		}

		// Merge the duplicate endpoints: sort the endpoints per frame so that
		// each distinct frame is unpacked once per animated sub-track
		const uint32_t num_endpoints = num_requests * 2;
		std::sort(endpoints, endpoints + num_endpoints, [](const acl_impl::batch_endpoint_t& lhs, const acl_impl::batch_endpoint_t& rhs)
		{
			if (lhs.clip_frame != rhs.clip_frame)
				return lhs.clip_frame < rhs.clip_frame;
			if (lhs.request_index != rhs.request_index)
				return lhs.request_index < rhs.request_index;
			return lhs.request_slot < rhs.request_slot;
		});

		uint32_t num_frames = 0;
		for (uint32_t endpoint_index = 0; endpoint_index < num_endpoints; ++endpoint_index)
		{
			const acl_impl::batch_endpoint_t& endpoint = endpoints[endpoint_index];
			if (endpoint_index == 0 || endpoint.clip_frame != endpoints[endpoint_index - 1].clip_frame)
			{
				frames[num_frames].clip_frame = endpoint.clip_frame;
				++num_frames;
			}

			acl_impl::batch_request_info_t& info = request_infos[endpoint.request_index];
			if (endpoint.request_slot == 0)
				info.endpoint0 = num_frames - 1;
			else
				info.endpoint1 = num_frames - 1;
		}

		// Locate the segment of every distinct frame. The frames are sorted, so
		// the segment indices are monotonic and the touched segments are
		// contiguous groups.
		const acl_impl::segment_header* segment_headers = transform_header.get_segment_headers();
		const uint32_t* segment_start_indices = has_segments ? transform_header.get_segment_start_indices() : nullptr;
		const uint32_t num_segments = transform_header.num_segments;

		uint32_t num_touched_segments = 0;
		uint32_t segment_index = 0;
		for (uint32_t frame_index = 0; frame_index < num_frames; ++frame_index)
		{
			const uint32_t clip_frame = frames[frame_index].clip_frame;
			if (has_segments)
			{
				while (segment_index + 1 < num_segments && segment_start_indices[segment_index + 1] <= clip_frame)
					++segment_index;
			}

			if (num_touched_segments == 0 || segments[num_touched_segments - 1].segment_index != segment_index)
			{
				// Resolve the data pointers of the newly touched segment
				const acl_impl::segment_header& segment_header = segment_headers[segment_index];
				const uint8_t* format_per_track_data;
				const uint8_t* segment_range_data;
				const uint8_t* animated_data;
				transform_header.get_segment_data(header, segment_header, format_per_track_data, segment_range_data, animated_data);

				const uint32_t num_animated_rotations_padded = align_to(transform_header.num_animated_rotation_sub_tracks, 4U);
				const bool are_rotations_variable = rotation_format == rotation_format8::quatf_drop_w_variable;

				acl_impl::batch_segment_info_t& segment = segments[num_touched_segments];
				segment.segment_index = segment_index;
				segment.pose_bit_size = segment_header.animated_pose_bit_size;
				segment.format_rotation = format_per_track_data;
				segment.range_rotation = segment_range_data;
				segment.format_translation = format_per_track_data + (are_rotations_variable ? num_animated_rotations_padded : 0);
				segment.range_translation = segment_range_data + (are_rotations_variable ? num_animated_rotations_padded * 6 : 0);
				segment.animated_data = animated_data;
				segment.rotation_bit_offset = 0;
				segment.translation_bit_offset = segment_header.animated_rotation_bit_size;
				++num_touched_segments;
			}

			const uint32_t segment_start_index = has_segments ? segment_start_indices[segment_index] : 0;
			frames[frame_index].segment_pos = num_touched_segments - 1;
			frames[frame_index].bit_offset = (clip_frame - segment_start_index) * segments[num_touched_segments - 1].pose_bit_size;
		}

		// Clip level data bases
		const uint8_t* const constant_track_data = transform_header.get_constant_track_data();
		const uint32_t packed_rotation_size = get_packed_rotation_size(get_highest_variant_precision(get_rotation_variant(rotation_format)));
		const uint8_t* const constant_rotations = constant_track_data;
		const uint8_t* const constant_translations = constant_rotations + packed_rotation_size * transform_header.num_constant_rotation_samples;

		const uint8_t* const clip_range_data = transform_header.get_clip_range_data();
		const uint8_t* const clip_range_rotations = clip_range_data;
		const uint8_t* const clip_range_translations = clip_range_rotations
			+ (rotation_format == rotation_format8::quatf_drop_w_variable ? transform_header.num_animated_rotation_sub_tracks * sizeof(rtm::float3f) * 2 : 0);

		const acl_impl::packed_sub_track_types* sub_track_types = transform_header.get_sub_track_types();
		const uint32_t num_sub_track_entries = (num_tracks + acl_impl::k_num_sub_tracks_per_packed_entry - 1) / acl_impl::k_num_sub_tracks_per_packed_entry;
		const acl_impl::packed_sub_track_types* rotation_sub_track_types = sub_track_types;
		const acl_impl::packed_sub_track_types* translation_sub_track_types = rotation_sub_track_types + num_sub_track_entries;

		uint32_t sub_track_visits = 0;
		uint32_t segment_metadata_visits = 0;
		uint32_t endpoint_unpacks = 0;
		uint32_t constant_unpacks = 0;

		const uint32_t num_animated_rotations = transform_header.num_animated_rotation_sub_tracks;
		const uint32_t num_constant_rotations = transform_header.num_constant_rotation_samples;

		uint32_t constant_rotation_index = 0;
		uint32_t animated_rotation_index = 0;

		// Unpack the rotation sub-tracks. Each sub-track is classified once,
		// each touched segment contributes its metadata once, and each distinct
		// frame is unpacked once. The per request interpolation follows.
		for (uint32_t track_index = 0; track_index < num_tracks; ++track_index)
		{
			const uint32_t sub_track_type = (rotation_sub_track_types[track_index / acl_impl::k_num_sub_tracks_per_packed_entry].types >> ((acl_impl::k_num_sub_tracks_per_packed_entry - 1 - (track_index % acl_impl::k_num_sub_tracks_per_packed_entry)) * 2)) & 0x3;
			++sub_track_visits;

			if (sub_track_type == 0)
			{
				// Default rotation
				for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
					out[request_index * pose_stride + track_index].rotation = rtm::quat_identity();
			}
			else if (sub_track_type == 1)
			{
				// Constant rotation, decoded once for every request
				const rtm::quatf rotation = acl_impl::batch_unpack_constant_rotation(rotation_format, constant_rotations, num_constant_rotations, constant_rotation_index);
				++constant_rotation_index;
				++constant_unpacks;

				for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
					out[request_index * pose_stride + track_index].rotation = rotation;
			}
			else
			{
				// Animated rotation
				const uint32_t animated_index = animated_rotation_index++;

				// Process the per segment metadata of this sub-track and record
				// where its samples live in every touched segment
				for (uint32_t segment_pos = 0; segment_pos < num_touched_segments; ++segment_pos)
				{
					acl_impl::batch_segment_info_t& segment = segments[segment_pos];
					const uint32_t num_bits = rotation_format == rotation_format8::quatf_drop_w_variable ? uint32_t(segment.format_rotation[animated_index]) : 0;
					segment.rotation_current_num_bits = num_bits;
					segment.rotation_current_offset = segment.rotation_bit_offset;
					segment.rotation_bit_offset += acl_impl::batch_rotation_sample_bits(rotation_format, num_bits);
					++segment_metadata_visits;
				}

				// Unpack the sample of this sub-track for every distinct frame
				for (uint32_t frame_index = 0; frame_index < num_frames; ++frame_index)
				{
					const acl_impl::batch_frame_info_t& frame = frames[frame_index];
					const acl_impl::batch_segment_info_t& segment = segments[frame.segment_pos];
					const rtm::quatf rotation = acl_impl::batch_unpack_rotation(rotation_format, has_segments, segment, clip_range_rotations, num_animated_rotations, animated_index, frame.bit_offset + segment.rotation_current_offset);
					endpoint_values[frame_index] = rtm::quat_to_vector(rotation);
					++endpoint_unpacks;
				}

				// Interpolate per request and scatter the result in output order
				for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
				{
					const acl_impl::batch_request_info_t& info = request_infos[request_index];
					const rtm::quatf value0 = rtm::vector_to_quat(endpoint_values[info.endpoint0]);
					const rtm::quatf value1 = rtm::vector_to_quat(endpoint_values[info.endpoint1]);
					out[request_index * pose_stride + track_index].rotation = acl_impl::quat_lerp_stable(value0, value1, info.interpolation_alpha);
				}
			}
		}

		uint32_t constant_translation_index = 0;
		uint32_t animated_translation_index = 0;

		// Unpack the translation sub-tracks
		for (uint32_t track_index = 0; track_index < num_tracks; ++track_index)
		{
			const uint32_t sub_track_type = (translation_sub_track_types[track_index / acl_impl::k_num_sub_tracks_per_packed_entry].types >> ((acl_impl::k_num_sub_tracks_per_packed_entry - 1 - (track_index % acl_impl::k_num_sub_tracks_per_packed_entry)) * 2)) & 0x3;
			++sub_track_visits;

			if (sub_track_type == 0)
			{
				// Default translation
				for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
					out[request_index * pose_stride + track_index].translation = rtm::vector_zero();
			}
			else if (sub_track_type == 1)
			{
				// Constant translation, decoded once for every request
				const rtm::vector4f translation = rtm::vector_load(constant_translations + constant_translation_index * sizeof(rtm::float3f));
				++constant_translation_index;
				++constant_unpacks;

				for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
					out[request_index * pose_stride + track_index].translation = translation;
			}
			else
			{
				// Animated translation
				const uint32_t animated_index = animated_translation_index++;

				for (uint32_t segment_pos = 0; segment_pos < num_touched_segments; ++segment_pos)
				{
					acl_impl::batch_segment_info_t& segment = segments[segment_pos];
					const uint32_t num_bits = translation_format == vector_format8::vector3f_variable ? uint32_t(segment.format_translation[animated_index]) : 0;
					segment.translation_current_num_bits = num_bits;
					segment.translation_current_offset = segment.translation_bit_offset;
					segment.translation_bit_offset += acl_impl::batch_translation_sample_bits(translation_format, num_bits);
					++segment_metadata_visits;
				}

				for (uint32_t frame_index = 0; frame_index < num_frames; ++frame_index)
				{
					const acl_impl::batch_frame_info_t& frame = frames[frame_index];
					const acl_impl::batch_segment_info_t& segment = segments[frame.segment_pos];
					endpoint_values[frame_index] = acl_impl::batch_unpack_translation(translation_format, has_segments, segment, clip_range_translations, animated_index, frame.bit_offset + segment.translation_current_offset);
					++endpoint_unpacks;
				}

				for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
				{
					const acl_impl::batch_request_info_t& info = request_infos[request_index];
					out[request_index * pose_stride + track_index].translation = rtm::vector_lerp(endpoint_values[info.endpoint0], endpoint_values[info.endpoint1], info.interpolation_alpha);
				}
			}
		}

		// Clips without scale have a unit scale on every bone
		for (uint32_t request_index = 0; request_index < num_requests; ++request_index)
		{
			rtm::vqvf* pose = out + request_index * pose_stride;
			for (uint32_t track_index = 0; track_index < num_tracks; ++track_index)
				pose[track_index].scale = rtm::vector_set(1.0F);
		}

		if (out_stats != nullptr)
		{
			out_stats->sub_track_visits = sub_track_visits;
			out_stats->segment_metadata_visits = segment_metadata_visits;
			out_stats->endpoint_unpacks = endpoint_unpacks;
			out_stats->constant_unpacks = constant_unpacks;
		}

		return true;
	}

	ACL_IMPL_VERSION_NAMESPACE_END
}

ACL_IMPL_FILE_PRAGMA_POP
