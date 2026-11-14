#pragma once

////////////////////////////////////////////////////////////////////////////////
// The MIT License (MIT)
//
// Copyright (c) 2020 Nicholas Frechette & Animation Compression Library contributors
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

// Internal read-scope bookkeeping shared by the database context and the streamer.

#include "acl/version.h"
#include "acl/core/iallocator.h"
#include "acl/core/quality_tiers.h"
#include "acl/core/impl/compiler_utils.h"
#include "acl/decompression/database/impl/database_streamer_types.h"

#include <cstdint>
#include <mutex>

ACL_IMPL_FILE_PRAGMA_PUSH

namespace acl
{
	ACL_IMPL_VERSION_NAMESPACE_BEGIN

	namespace acl_impl
	{
		struct database_context_v0;

		//////////////////////////////////////////////////////////////////////////
		// A streaming request whose effect cannot be published while readers are
		// active. At most one request can exist per quality tier, mirroring the
		// rule that only one stream in/out request can be in flight per tier.
		//////////////////////////////////////////////////////////////////////////
		struct database_deferred_operation
		{
			enum class phase : uint8_t
			{
				// Slot is free
				unused,

				// Request was recorded but the user streamer has not been called yet.
				// Evictions queued while the gate is closed wait here.
				waiting_dispatch,

				// The user streamer was called and we are waiting for complete/cancel.
				// This phase does not block new readers: nothing is ready to publish.
				waiting_completion,

				// complete() arrived while readers were active, publication is held.
				completion_held,

				// cancel() arrived while readers were active, cancellation is held.
				cancel_held,
			};

			streaming_action			action = streaming_action::stream_in;
			quality_tier				tier = quality_tier::highest_importance;
			uint32_t					first_chunk_index = 0;
			uint32_t					num_streaming_chunks = 0;
			streaming_request_id		request_id = k_invalid_streamer_request_id;

			// Cached streamer arguments captured when the request was built
			uint32_t					stream_offset = 0;
			uint32_t					stream_size = 0;
			bool						can_reallocate_bulk_data = false;
			bool						can_free_bulk_data = false;
			bool						completion_success = false;

			phase						request_phase = phase::unused;

			bool is_used() const { return request_phase != phase::unused; }
		};

		//////////////////////////////////////////////////////////////////////////
		// Shared residency snapshot state for a database context. Allocated lazily
		// the first time a read scope is acquired and destroyed when the context resets.
		//
		// The internal mutex only guards bookkeeping. User streamer calls are never
		// performed while holding it because a streamer may reentrantly call
		// complete()/cancel() from the same thread.
		//////////////////////////////////////////////////////////////////////////
		struct database_read_state
		{
			database_context_v0*						context = nullptr;
			std::mutex									lock;
			int32_t										num_readers = 0;
			database_deferred_operation					operations[k_num_database_tiers];
		};

		// Read-scope protocol entry points. Definitions live in
		// database_read_state.impl.h, pulled in by database_streamer.impl.h.
		database_read_state* create_read_state(database_context_v0& context, iallocator& allocator);
		void destroy_read_state(database_context_v0& context);
		database_read_state* acquire_read(database_context_v0& context);
		void release_read(database_read_state& state);
		bool handle_streamer_completion(database_context_v0& context, const streaming_request& request, bool success);

		// The following helpers must be called with read_state.lock held.
		void register_dispatched_operation_locked(database_context_v0& context, streaming_action action,
			quality_tier tier, uint32_t first_chunk_index, uint32_t num_streaming_chunks,
			streaming_request_id request_id, uint32_t stream_offset, uint32_t stream_size,
			bool can_reallocate_bulk_data);
		bool should_defer_stream_out_locked(const database_read_state& state);
		bool defer_stream_out_locked(database_context_v0& context, quality_tier tier,
			uint32_t first_chunk_index, uint32_t num_streaming_chunks, streaming_request_id request_id,
			uint32_t stream_offset, uint32_t stream_size, bool can_deallocate_bulk_data);
		bool has_active_readers(database_context_v0& context);
	}

	ACL_IMPL_VERSION_NAMESPACE_END
}

ACL_IMPL_FILE_PRAGMA_POP
