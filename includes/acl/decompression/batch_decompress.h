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

#include "acl/version.h"
#include "acl/core/compressed_tracks.h"
#include "acl/core/sample_looping_policy.h"
#include "acl/core/sample_rounding_policy.h"
#include "acl/core/impl/compiler_utils.h"

#include <rtm/vqvf.h>

#include <cstddef>
#include <cstdint>

ACL_IMPL_FILE_PRAGMA_PUSH

namespace acl
{
	ACL_IMPL_VERSION_NAMESPACE_BEGIN

	//////////////////////////////////////////////////////////////////////////
	// A single decompression request for batch_decompression_context.
	// Each request samples the bound clip at one point in time.
	//////////////////////////////////////////////////////////////////////////
	struct batch_request
	{
		//////////////////////////////////////////////////////////////////////////
		// The time to sample at, in seconds.
		// The value must be finite. Values outside [0, clip duration] are clamped.
		float sample_time;

		//////////////////////////////////////////////////////////////////////////
		// How to round the sample time when it falls between two keyframes.
		// Only sample_rounding_policy::none, floor, ceil, and nearest are supported.
		sample_rounding_policy rounding_policy;
	};

	//////////////////////////////////////////////////////////////////////////
	// Counters that describe the work a batch_decompression_context performed
	// during one call to decompress(..). The counts are exact, they are
	// incremented where the measured operation takes place.
	//////////////////////////////////////////////////////////////////////////
	struct batch_decode_stats
	{
		//////////////////////////////////////////////////////////////////////////
		// The number of sub-tracks that were classified (default, constant, or
		// animated). At most 3 times the number of output bones. Sub-tracks that
		// exist only as padding for SIMD are not counted.
		uint32_t sub_track_visits;

		//////////////////////////////////////////////////////////////////////////
		// The number of (segment, animated sub-track) pairs whose per segment
		// metadata was processed. At most (number of animated sub-tracks) times
		// (number of distinct segments touched).
		uint32_t segment_metadata_visits;

		//////////////////////////////////////////////////////////////////////////
		// The number of (frame, animated sub-track) sample values unpacked from
		// the compressed stream. At most (number of animated sub-tracks) times
		// (number of distinct frames required by the requests).
		uint32_t endpoint_unpacks;

		//////////////////////////////////////////////////////////////////////////
		// The number of constant sub-track values decoded.
		// At most the number of constant sub-tracks in the clip.
		uint32_t constant_unpacks;
	};

	//////////////////////////////////////////////////////////////////////////
	// Decompresses many sample times of the same clip in one call.
	//
	// The context binds one read-only compressed_tracks instance and a maximum
	// number of requests per call. The compressed data is referenced, never
	// copied. The bound compressed_tracks instance and the workspace provided
	// at initialize(..) must outlive the context.
	//
	// Only a subset of the compressed format is supported:
	//    - transform tracks of type vqvf without scale, without an additive base
	//    - trivial default values (identity rotation, zero translation, unit scale)
	//    - default, constant, and animated sub-tracks
	//    - full precision and variable bit rate rotation/translation formats
	//    - one or more segments, including interpolation across segment boundaries
	// Clips that use a database, stripped keyframes, scalar tracks, vqf/vqsf
	// tracks, or scale are rejected by initialize(..).
	//
	// Usage:
	//    1. Call query_workspace_size(max_requests) to learn the workspace size.
	//    2. Provide a workspace buffer aligned to 16 bytes that you own.
	//    3. Call initialize(..) with the clip, the capacity, and the workspace.
	//    4. Optionally call set_looping_policy(..) to select clamp or wrap.
	//    5. Call decompress(..) with up to max_requests requests.
	//
	// The context performs no heap allocation after initialize(..) returns.
	// The caller must ensure that the requests, the workspace, and the output
	// buffer do not overlap. Two contexts can be used in an interleaved
	// fashion, they share no state.
	//////////////////////////////////////////////////////////////////////////
	class batch_decompression_context
	{
	public:
		//////////////////////////////////////////////////////////////////////////
		// Constructs an uninitialized context.
		batch_decompression_context();

		//////////////////////////////////////////////////////////////////////////
		// Destroys the context. The workspace is not released, the caller owns it.
		~batch_decompression_context();

		//////////////////////////////////////////////////////////////////////////
		// Returns the number of bytes of workspace that a context requires to
		// serve at most max_requests requests per decompress(..) call.
		// The workspace does not depend on the clip contents.
		static size_t query_workspace_size(uint32_t max_requests);

		//////////////////////////////////////////////////////////////////////////
		// Returns the number of bytes of workspace that this context requires,
		// zero when it is not initialized.
		size_t get_workspace_size() const { return is_initialized() ? query_workspace_size(m_max_requests) : 0; }

		//////////////////////////////////////////////////////////////////////////
		// Initializes the context with a compressed tracks instance, a maximum
		// request capacity, and a caller owned workspace.
		// The workspace must be aligned to 16 bytes and must be at least
		// query_workspace_size(max_requests) bytes large.
		// No animation data is decoded by this call.
		// Returns whether initialization was successful or not.
		bool initialize(const compressed_tracks& tracks, uint32_t max_requests, void* workspace, size_t workspace_size);

		//////////////////////////////////////////////////////////////////////////
		// Returns true if this context is bound to a compressed tracks instance.
		bool is_initialized() const { return m_tracks != nullptr; }

		//////////////////////////////////////////////////////////////////////////
		// Resets the context to its default constructed state.
		void reset();

		//////////////////////////////////////////////////////////////////////////
		// Returns the compressed tracks bound to this context.
		const compressed_tracks* get_compressed_tracks() const { return m_tracks; }

		//////////////////////////////////////////////////////////////////////////
		// Returns the maximum number of requests per decompress(..) call.
		uint32_t get_max_requests() const { return m_max_requests; }

		//////////////////////////////////////////////////////////////////////////
		// Sets the looping policy. Only sample_looping_policy::clamp and
		// sample_looping_policy::wrap are supported. The policy
		// sample_looping_policy::as_compressed resolves to the policy stored in
		// the clip. The default is the policy stored in the clip.
		void set_looping_policy(sample_looping_policy policy);

		//////////////////////////////////////////////////////////////////////////
		// Gets the current looping policy, clamp or wrap.
		sample_looping_policy get_looping_policy() const { return m_looping_policy; }

		//////////////////////////////////////////////////////////////////////////
		// Decompresses num_requests poses in one call.
		//
		// requests: The requests to evaluate, num_requests entries. Can be null
		//           when num_requests is zero.
		// num_requests: The number of requests, at most get_max_requests().
		// out: The output poses, one pose of num_tracks elements per request.
		//      The pose of request i starts at out[i * pose_stride]. Bones are
		//      written in compressed output bone order. Elements in the gap
		//      between num_tracks and pose_stride are not modified. Can be null
		//      when num_requests is zero.
		// out_capacity: The number of rtm::vqvf elements in out. Must hold at
		//      least (num_requests - 1) * pose_stride + num_tracks elements.
		// pose_stride: The distance in elements between the start of two
		//      consecutive poses. Must be at least the number of output bones.
		// out_stats: Receives the work counters on success. Can be null. It is
		//      not modified when the call fails.
		//
		// Returns true on success. On failure, out and out_stats are not
		// modified. Failure happens when: the context is not initialized, a
		// capacity is exceeded, a request time is not finite, a rounding policy
		// is not supported, or a null pointer is provided for data that is needed.
		bool decompress(const batch_request* requests, uint32_t num_requests, rtm::vqvf* out, uint32_t out_capacity, uint32_t pose_stride, batch_decode_stats* out_stats);

	private:
		batch_decompression_context(const batch_decompression_context& other) = delete;
		batch_decompression_context& operator=(const batch_decompression_context& other) = delete;

		// Bound state, valid when m_tracks is not null
		const compressed_tracks* m_tracks;
		void* m_workspace;
		uint32_t m_max_requests;
		uint32_t m_num_tracks;
		uint32_t m_num_samples;
		float m_sample_rate;
		float m_clip_duration;
		sample_looping_policy m_looping_policy;
	};

	ACL_IMPL_VERSION_NAMESPACE_END
}

#include "acl/decompression/impl/batch_decompress.impl.h"

ACL_IMPL_FILE_PRAGMA_POP
