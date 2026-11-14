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

// Included only once from database_streamer.impl.h.
// Implements the read-scope residency protocol on top of database_context_v0.
//
// Synchronization model:
//   - Every mutation of the residency bookkeeping (loaded/streaming bitsets, segment tier
//     metadata, bulk data pointers, deferred operation slots) is serialized by read_state.lock.
//   - The lock is never held while calling into the user streamer because a streamer may
//     reentrantly call complete()/cancel() from the same thread.
//   - Decompression threads do not take the lock. While at least one reader is active the
//     published residency cannot change, so their (atomic) reads stay valid for the whole scope.

#include "acl/version.h"
#include "acl/core/bitset.h"
#include "acl/core/iallocator.h"
#include "acl/core/impl/bit_cast.impl.h"
#include "acl/core/impl/compressed_headers.h"
#include "acl/decompression/database/database_read_scope.h"
#include "acl/decompression/database/impl/database_context.h"
#include "acl/decompression/database/impl/database_read_state.h"

#include <cstdint>
#include <mutex>

ACL_IMPL_FILE_PRAGMA_PUSH

namespace acl
{
	ACL_IMPL_VERSION_NAMESPACE_BEGIN

	namespace acl_impl
	{
		inline database_read_state* create_read_state(database_context_v0& context, iallocator& allocator)
		{
			database_read_state* state = allocate_type<database_read_state>(allocator);
			state->context = &context;
			return state;
		}

		inline void destroy_read_state(database_context_v0& context)
		{
			database_read_state* state = context.read_state;
			if (state == nullptr)
				return;	// Nothing to do

			iallocator& allocator = *context.allocator;
			context.read_state = nullptr;
			deallocate_type(allocator, state);
		}

		//////////////////////////////////////////////////////////////////////////
		// Reader acquisition. Fails (returns null) once any streaming operation is
		// pending; the gate stays closed until every pending operation commits.
		//////////////////////////////////////////////////////////////////////////
		inline database_read_state* acquire_read(database_context_v0& context)
		{
			database_read_state* state = context.read_state;
			if (state == nullptr)
				return nullptr;	// Not initialized

			std::lock_guard<std::mutex> guard(state->lock);

			for (const database_deferred_operation& operation : state->operations)
			{
				if (operation.is_used())
					return nullptr;
			}

			++state->num_readers;
			return state;
		}

		// Clears the segment tier metadata and drops the bulk pointer for a streamed out range.
		// The caller must hold read_state.lock and must only call this once no reader is active.
		inline void tear_down_streamed_out_chunks_locked(database_context_v0& context, quality_tier tier,
			uint32_t first_chunk_index, uint32_t num_streaming_chunks, bool can_deallocate_bulk_data)
		{
			const database_header& header = get_database_header(*context.db);
			const database_chunk_description* chunk_descriptions = tier == quality_tier::medium_importance
				? header.get_chunk_descriptions_medium()
				: header.get_chunk_descriptions_low();

			const uint32_t tier_index = uint32_t(tier) - 1;
			const uint8_t*& bulk_data_ref = context.bulk_data[tier_index];
			const uint8_t* bulk_data = bulk_data_ref;
			ACL_ASSERT(bulk_data != nullptr, "Bulk data should be allocated when we stream out");

			if (can_deallocate_bulk_data)
				bulk_data_ref = nullptr;

			const uint32_t end_chunk_index = first_chunk_index + num_streaming_chunks;
			for (uint32_t chunk_index = first_chunk_index; chunk_index < end_chunk_index; ++chunk_index)
			{
				const database_chunk_description& chunk_description = chunk_descriptions[chunk_index];
				const database_chunk_header* chunk_header = chunk_description.get_chunk_header(bulk_data);
				ACL_ASSERT(chunk_header->index == chunk_index, "Unexpected chunk index");

				const database_chunk_segment_header* chunk_segment_headers = chunk_header->get_segment_headers();
				const uint32_t num_segments = chunk_header->num_segments;
				for (uint32_t segment_index = 0; segment_index < num_segments; ++segment_index)
				{
					const database_chunk_segment_header& chunk_segment_header = chunk_segment_headers[segment_index];

#if defined(ACL_HAS_ASSERT_CHECKS)
					const database_runtime_clip_header* clip_header = chunk_segment_header.get_clip_header(context.clip_segment_headers);
					ACL_ASSERT(clip_header->clip_hash == chunk_segment_header.clip_hash, "Unexpected clip hash");
#endif

					database_runtime_segment_header* segment_header = chunk_segment_header.get_segment_header(context.clip_segment_headers);
					const uint64_t tier_metadata = (uint64_t(chunk_segment_header.samples_offset) << 32) | chunk_segment_header.sample_indices;
					ACL_ASSERT(segment_header->tier_metadata[tier_index].load(k_memory_order_relaxed) == tier_metadata, "Database tier metadata should have been initialized"); (void)tier_metadata;
					segment_header->tier_metadata[tier_index].store(0, k_memory_order_relaxed);
				}
			}
		}

		// Invokes the user streamer. The lock is acquired only for the bookkeeping around the call
		// and released across the streamer invocation itself so synchronous completion can re-lock.
		inline void dispatch_operation_unlocked(database_read_state& state, database_deferred_operation& operation)
		{
			database_context_v0& context = *state.context;
			const uint32_t tier_index = uint32_t(operation.tier) - 1;
			database_streamer* streamer = context.streamers[tier_index];

			if (operation.action == streaming_action::stream_out)
			{
				{
					std::lock_guard<std::mutex> guard(state.lock);
					tear_down_streamed_out_chunks_locked(context, operation.tier, operation.first_chunk_index,
						operation.num_streaming_chunks, operation.can_free_bulk_data);
				}

				streamer->stream_out(operation.stream_offset, operation.stream_size,
					operation.can_free_bulk_data, operation.tier, operation.request_id);
			}
			else
			{
				streamer->stream_in(operation.stream_offset, operation.stream_size,
					operation.can_reallocate_bulk_data, operation.tier, operation.request_id);
			}
		}

		// Commits held publications/cancellations and dispatches queued requests. Runs only when the
		// last reader has just left, so the published residency is safe to mutate.
		inline void pump_operations(database_read_state& state)
		{
			bool made_progress = true;
			while (made_progress)
			{
				made_progress = false;
				database_deferred_operation* operation_to_dispatch = nullptr;

				{
					std::lock_guard<std::mutex> guard(state.lock);

					for (database_deferred_operation& operation : state.operations)
					{
						if (!operation.is_used())
							continue;

						if (operation.request_phase == database_deferred_operation::phase::completion_held
							|| operation.request_phase == database_deferred_operation::phase::cancel_held)
						{
							streaming_request request;
							request.action = operation.action;
							request.tier = operation.tier;
							request.first_chunk_index = operation.first_chunk_index;
							request.num_streaming_chunks = operation.num_streaming_chunks;
							request.generation_id = get_generation_id(operation.request_id);

							execute_request(operation.completion_success, *state.context, request);

							operation.request_phase = database_deferred_operation::phase::unused;
							made_progress = true;
						}
						else if (operation.request_phase == database_deferred_operation::phase::waiting_dispatch
							&& operation_to_dispatch == nullptr)
						{
							// Move to in-flight before releasing the lock so a reentrant completion
							// finds the operation in the waiting_completion phase.
							operation.request_phase = database_deferred_operation::phase::waiting_completion;
							operation_to_dispatch = &operation;
							made_progress = true;
						}
					}
				}

				if (operation_to_dispatch != nullptr)
					dispatch_operation_unlocked(state, *operation_to_dispatch);
			}
		}

		inline void release_read(database_read_state& state)
		{
			bool is_last_reader = false;
			{
				std::lock_guard<std::mutex> guard(state.lock);
				ACL_ASSERT(state.num_readers > 0, "Releasing a read scope that is not held");
				--state.num_readers;
				is_last_reader = state.num_readers == 0;
			}

			if (is_last_reader)
				pump_operations(state);	// Other readers kept the snapshot pinned; now it can change.
		}

		//////////////////////////////////////////////////////////////////////////
		// Streamer completion/cancellation routing. Called from database_streamer::complete/cancel.
		//////////////////////////////////////////////////////////////////////////
		inline bool handle_streamer_completion(database_context_v0& context, const streaming_request& request, bool success)
		{
			database_read_state* state = context.read_state;
			if (state == nullptr)
			{
				execute_request(success, context, request);
				return true;
			}

			std::lock_guard<std::mutex> guard(state->lock);

			database_deferred_operation* matching_operation = nullptr;
			for (database_deferred_operation& operation : state->operations)
			{
				if (!operation.is_used() || operation.tier != request.tier)
					continue;

				if (operation.request_phase != database_deferred_operation::phase::waiting_completion)
					continue;

				if (get_generation_id(operation.request_id) != request.generation_id)
					continue;	// Stale request; the generation protection handles it like before

				matching_operation = &operation;
				break;
			}

			if (matching_operation == nullptr)
			{
				// Unknown to the read-state protocol (e.g. dispatched before read state existed):
				// preserve the original synchronous behavior.
				execute_request(success, context, request);
				return true;
			}

			if (state->num_readers > 0)
			{
				// Keep the completion record; publish only once every reader has left.
				matching_operation->completion_success = success;
				matching_operation->request_phase = success
					? database_deferred_operation::phase::completion_held
					: database_deferred_operation::phase::cancel_held;
				return false;
			}

			execute_request(success, context, request);
			matching_operation->request_phase = database_deferred_operation::phase::unused;
			return true;
		}

		//////////////////////////////////////////////////////////////////////////
		// Locked bookkeeping helpers used by database_context::stream_in/stream_out.
		// The caller must hold read_state.lock.
		//////////////////////////////////////////////////////////////////////////
		inline void register_dispatched_operation_locked(database_context_v0& context, streaming_action action,
			quality_tier tier, uint32_t first_chunk_index, uint32_t num_streaming_chunks,
			streaming_request_id request_id, uint32_t stream_offset, uint32_t stream_size,
			bool can_reallocate_bulk_data)
		{
			database_read_state* state = context.read_state;
			database_deferred_operation& operation = state->operations[uint32_t(tier) - 1];
			ACL_ASSERT(!operation.is_used(), "A streaming request is already in flight for this tier");

			operation.action = action;
			operation.tier = tier;
			operation.first_chunk_index = first_chunk_index;
			operation.num_streaming_chunks = num_streaming_chunks;
			operation.request_id = request_id;
			operation.stream_offset = stream_offset;
			operation.stream_size = stream_size;
			operation.can_reallocate_bulk_data = can_reallocate_bulk_data;
			operation.can_free_bulk_data = false;
			operation.completion_success = false;
			operation.request_phase = database_deferred_operation::phase::waiting_completion;
		}

		inline bool should_defer_stream_out_locked(const database_read_state& state)
		{
			return state.num_readers > 0;
		}

		inline bool defer_stream_out_locked(database_context_v0& context, quality_tier tier,
			uint32_t first_chunk_index, uint32_t num_streaming_chunks, streaming_request_id request_id,
			uint32_t stream_offset, uint32_t stream_size, bool can_deallocate_bulk_data)
		{
			database_read_state* state = context.read_state;
			database_deferred_operation& operation = state->operations[uint32_t(tier) - 1];
			if (operation.is_used())
				return false;

			operation.action = streaming_action::stream_out;
			operation.tier = tier;
			operation.first_chunk_index = first_chunk_index;
			operation.num_streaming_chunks = num_streaming_chunks;
			operation.request_id = request_id;
			operation.stream_offset = stream_offset;
			operation.stream_size = stream_size;
			operation.can_reallocate_bulk_data = false;
			operation.can_free_bulk_data = can_deallocate_bulk_data;
			operation.completion_success = false;
			operation.request_phase = database_deferred_operation::phase::waiting_dispatch;
			return true;
		}

		inline bool has_active_readers(database_context_v0& context)
		{
			database_read_state* state = context.read_state;
			if (state == nullptr)
				return false;

			std::lock_guard<std::mutex> guard(state->lock);
			return state->num_readers > 0;
		}
	}

	inline void database_read_scope::release()
	{
		if (m_state != nullptr)
		{
			acl_impl::release_read(*m_state);
			m_state = nullptr;
		}
	}

	ACL_IMPL_VERSION_NAMESPACE_END
}

ACL_IMPL_FILE_PRAGMA_POP
