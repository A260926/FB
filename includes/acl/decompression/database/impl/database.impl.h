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

// Included only once from database.h

#include "acl/version.h"
#include "acl/core/bitset.h"
#include "acl/core/compressed_database.h"
#include "acl/core/compressed_tracks_version.h"
#include "acl/core/impl/atomic.impl.h"
#include "acl/core/impl/bit_cast.impl.h"
#include "acl/decompression/database/impl/database_context.h"

#include <cstdint>
#include <mutex>

ACL_IMPL_FILE_PRAGMA_PUSH

namespace acl
{
	ACL_IMPL_VERSION_NAMESPACE_BEGIN

	namespace acl_impl
	{
		inline uint32_t calculate_runtime_data_size(const compressed_database& database)
		{
			const database_header& header = get_database_header(database);

			const uint32_t num_medium_chunks = header.num_chunks[0];
			const uint32_t num_low_chunks = header.num_chunks[1];
			const uint32_t num_clips = header.num_clips;
			const uint32_t num_segments = header.num_segments;

			const bitset_description medium_desc = bitset_description::make_from_num_bits(num_medium_chunks);
			const bitset_description low_desc = bitset_description::make_from_num_bits(num_low_chunks);

			const uint32_t medium_bitset_size = medium_desc.get_num_bytes();
			const uint32_t low_bitset_size = low_desc.get_num_bytes();

			uint32_t runtime_data_size = 0;
			runtime_data_size += medium_bitset_size;			// Loaded chunks
			runtime_data_size += medium_bitset_size;			// Streaming chunks
			runtime_data_size += low_bitset_size;				// Loaded chunks
			runtime_data_size += low_bitset_size;				// Streaming chunks
			runtime_data_size = align_to(runtime_data_size, 8);	// Align runtime headers
			runtime_data_size += num_clips * sizeof(database_runtime_clip_header);
			runtime_data_size += num_segments * sizeof(database_runtime_segment_header);

			return runtime_data_size;
		}
	}

	template<class database_settings_type>
	inline database_context<database_settings_type>::database_context()
	{
		m_context.reset();

		static_assert(offsetof(database_context<database_settings_type>, m_context) == 0, "m_context must be the first data member!");
	}

	template<class database_settings_type>
	inline database_context<database_settings_type>::~database_context()
	{
		reset();
	}

	template<class database_settings_type>
	inline const compressed_database* database_context<database_settings_type>::get_compressed_database() const { return m_context.get_compressed_database(); }

	template<class database_settings_type>
	inline bool database_context<database_settings_type>::initialize(iallocator& allocator, const compressed_database& database)
	{
		const bool is_valid = database.is_valid(false).empty();
		ACL_ASSERT(is_valid, "Invalid compressed database instance");
		if (!is_valid)
			return false;

		ACL_ASSERT(database.is_bulk_data_inline(), "Bulk data must be inline when initializing without a streamer");
		if (!database.is_bulk_data_inline())
			return false;

		ACL_ASSERT(!is_initialized(), "Cannot initialize database twice");
		if (is_initialized())
			return false;

		m_context.db = &database;
		m_context.db_hash = database.get_hash();
		m_context.allocator = &allocator;
		m_context.bulk_data[0] = database.get_bulk_data(quality_tier::medium_importance);
		m_context.bulk_data[1] = database.get_bulk_data(quality_tier::lowest_importance);
		m_context.streamers[0] = m_context.streamers[1] = nullptr;

		const acl_impl::database_header& header = acl_impl::get_database_header(database);

		const uint32_t num_medium_chunks = header.num_chunks[0];
		const uint32_t num_low_chunks = header.num_chunks[1];

		const bitset_description medium_desc = bitset_description::make_from_num_bits(num_medium_chunks);
		const bitset_description low_desc = bitset_description::make_from_num_bits(num_low_chunks);

		const uint32_t medium_bitset_size = medium_desc.get_num_bytes();
		const uint32_t low_bitset_size = low_desc.get_num_bytes();

		// Allocate a single buffer for everything we need. This is faster to allocate and it ensures better virtual
		// memory locality which should help reduce the cost of TLB misses.
		const uint32_t runtime_data_size = acl_impl::calculate_runtime_data_size(database);
		uint8_t* runtime_data_buffer = allocate_type_array_aligned<uint8_t>(allocator, runtime_data_size, 16);

		// Initialize everything to 0
		std::memset(runtime_data_buffer, 0, runtime_data_size);

		m_context.loaded_chunks[0] = acl_impl::bit_cast<uint32_t*>(runtime_data_buffer);
		runtime_data_buffer += medium_bitset_size;

		m_context.streaming_chunks[0] = acl_impl::bit_cast<uint32_t*>(runtime_data_buffer);
		runtime_data_buffer += medium_bitset_size;

		m_context.loaded_chunks[1] = acl_impl::bit_cast<uint32_t*>(runtime_data_buffer);
		runtime_data_buffer += low_bitset_size;

		m_context.streaming_chunks[1] = acl_impl::bit_cast<uint32_t*>(runtime_data_buffer);
		runtime_data_buffer += low_bitset_size;

		m_context.clip_segment_headers = align_to(runtime_data_buffer, 8);	// Align runtime headers

		// Copy our clip hashes to setup our headers
		const uint32_t num_clips = header.num_clips;
		const acl_impl::database_clip_metadata* clip_metadatas = header.get_clip_metadatas();
		for (uint32_t clip_index = 0; clip_index < num_clips; ++clip_index)
		{
			const acl_impl::database_clip_metadata& clip_metadata = clip_metadatas[clip_index];
			acl_impl::database_runtime_clip_header* clip_header = clip_metadata.get_clip_header(m_context.clip_segment_headers);
			clip_header->clip_hash = clip_metadata.clip_hash;
		}

		// Allocate the read-scope bookkeeping up front so acquiring a scope never races on its creation.
		// Initialization is single threaded by contract.
		m_context.read_state = acl_impl::create_read_state(m_context, allocator);

		// Bulk data is inline so stream everything in right away
		const acl_impl::database_chunk_description* chunk_descriptions = header.get_chunk_descriptions_medium();
		for (uint32_t chunk_index = 0; chunk_index < num_medium_chunks; ++chunk_index)
		{
			const acl_impl::database_chunk_description& chunk_description = chunk_descriptions[chunk_index];
			const acl_impl::database_chunk_header* chunk_header = chunk_description.get_chunk_header(m_context.bulk_data[0]);
			ACL_ASSERT(chunk_header->index == chunk_index, "Unexpected chunk index");

			const acl_impl::database_chunk_segment_header* chunk_segment_headers = chunk_header->get_segment_headers();
			const uint32_t num_segments = chunk_header->num_segments;
			for (uint32_t segment_index = 0; segment_index < num_segments; ++segment_index)
			{
				const acl_impl::database_chunk_segment_header& chunk_segment_header = chunk_segment_headers[segment_index];

#if defined(ACL_HAS_ASSERT_CHECKS)
				const acl_impl::database_runtime_clip_header* clip_header = chunk_segment_header.get_clip_header(m_context.clip_segment_headers);
				ACL_ASSERT(clip_header->clip_hash == chunk_segment_header.clip_hash, "Unexpected clip hash");
#endif

				acl_impl::database_runtime_segment_header* segment_header = chunk_segment_header.get_segment_header(m_context.clip_segment_headers);
				ACL_ASSERT(segment_header->tier_metadata[0].load(acl_impl::k_memory_order_relaxed) == 0, "Tier metadata should not be initialized");
				segment_header->tier_metadata[0].store((uint64_t(chunk_segment_header.samples_offset) << 32) | chunk_segment_header.sample_indices, acl_impl::k_memory_order_relaxed);
			}

			bitset_set(m_context.loaded_chunks[0], medium_desc, chunk_index, true);
		}

		chunk_descriptions = header.get_chunk_descriptions_low();
		for (uint32_t chunk_index = 0; chunk_index < num_low_chunks; ++chunk_index)
		{
			const acl_impl::database_chunk_description& chunk_description = chunk_descriptions[chunk_index];
			const acl_impl::database_chunk_header* chunk_header = chunk_description.get_chunk_header(m_context.bulk_data[1]);
			ACL_ASSERT(chunk_header->index == chunk_index, "Unexpected chunk index");

			const acl_impl::database_chunk_segment_header* chunk_segment_headers = chunk_header->get_segment_headers();
			const uint32_t num_segments = chunk_header->num_segments;
			for (uint32_t segment_index = 0; segment_index < num_segments; ++segment_index)
			{
				const acl_impl::database_chunk_segment_header& chunk_segment_header = chunk_segment_headers[segment_index];

#if defined(ACL_HAS_ASSERT_CHECKS)
				const acl_impl::database_runtime_clip_header* clip_header = chunk_segment_header.get_clip_header(m_context.clip_segment_headers);
				ACL_ASSERT(clip_header->clip_hash == chunk_segment_header.clip_hash, "Unexpected clip hash");
#endif

				acl_impl::database_runtime_segment_header* segment_header = chunk_segment_header.get_segment_header(m_context.clip_segment_headers);
				ACL_ASSERT(segment_header->tier_metadata[1].load(acl_impl::k_memory_order_relaxed) == 0, "Tier metadata should not be initialized");
				segment_header->tier_metadata[1].store((uint64_t(chunk_segment_header.samples_offset) << 32) | chunk_segment_header.sample_indices, acl_impl::k_memory_order_relaxed);
			}

			bitset_set(m_context.loaded_chunks[1], low_desc, chunk_index, true);
		}

		return true;
	}

	template<class database_settings_type>
	inline bool database_context<database_settings_type>::initialize(iallocator& allocator, const compressed_database& database, database_streamer& medium_tier_streamer, database_streamer& low_tier_streamer)
	{
		const bool is_valid = database.is_valid(false).empty();
		ACL_ASSERT(is_valid, "Invalid compressed database instance");
		if (!is_valid)
			return false;

		ACL_ASSERT(medium_tier_streamer.is_initialized(), "Medium importance tier streamer must be initialized");
		if (!medium_tier_streamer.is_initialized())
			return false;

		ACL_ASSERT(low_tier_streamer.is_initialized(), "Low importance tier streamer must be initialized");
		if (!low_tier_streamer.is_initialized())
			return false;

		ACL_ASSERT(!is_initialized(), "Cannot initialize database twice");
		if (is_initialized())
			return false;

		m_context.db = &database;
		m_context.db_hash = database.get_hash();
		m_context.allocator = &allocator;
		m_context.bulk_data[0] = m_context.bulk_data[1] = nullptr;	// Will be set during the first stream in request
		m_context.streamers[0] = &medium_tier_streamer;
		m_context.streamers[1] = &low_tier_streamer;

		medium_tier_streamer.bind(m_context);
		low_tier_streamer.bind(m_context);

		const acl_impl::database_header& header = acl_impl::get_database_header(database);

		const uint32_t num_medium_chunks = header.num_chunks[0];
		const uint32_t num_low_chunks = header.num_chunks[1];

		const bitset_description medium_desc = bitset_description::make_from_num_bits(num_medium_chunks);
		const bitset_description low_desc = bitset_description::make_from_num_bits(num_low_chunks);

		const uint32_t medium_bitset_size = medium_desc.get_num_bytes();
		const uint32_t low_bitset_size = low_desc.get_num_bytes();

		// Allocate a single buffer for everything we need. This is faster to allocate and it ensures better virtual
		// memory locality which should help reduce the cost of TLB misses.
		const uint32_t runtime_data_size = acl_impl::calculate_runtime_data_size(database);
		uint8_t* runtime_data_buffer = allocate_type_array_aligned<uint8_t>(allocator, runtime_data_size, 16);

		// Initialize everything to 0
		std::memset(runtime_data_buffer, 0, runtime_data_size);

		m_context.loaded_chunks[0] = acl_impl::bit_cast<uint32_t*>(runtime_data_buffer);
		runtime_data_buffer += medium_bitset_size;

		m_context.streaming_chunks[0] = acl_impl::bit_cast<uint32_t*>(runtime_data_buffer);
		runtime_data_buffer += medium_bitset_size;

		m_context.loaded_chunks[1] = acl_impl::bit_cast<uint32_t*>(runtime_data_buffer);
		runtime_data_buffer += low_bitset_size;

		m_context.streaming_chunks[1] = acl_impl::bit_cast<uint32_t*>(runtime_data_buffer);
		runtime_data_buffer += low_bitset_size;

		m_context.clip_segment_headers = align_to(runtime_data_buffer, 8);	// Align runtime headers

		// Copy our clip hashes to setup our headers
		const uint32_t num_clips = header.num_clips;
		const acl_impl::database_clip_metadata* clip_metadatas = header.get_clip_metadatas();
		for (uint32_t clip_index = 0; clip_index < num_clips; ++clip_index)
		{
			const acl_impl::database_clip_metadata& clip_metadata = clip_metadatas[clip_index];
			acl_impl::database_runtime_clip_header* clip_header = clip_metadata.get_clip_header(m_context.clip_segment_headers);
			clip_header->clip_hash = clip_metadata.clip_hash;
		}

		// Allocate the read-scope bookkeeping up front so acquiring a scope never races on its creation.
		// Initialization is single threaded by contract.
		m_context.read_state = acl_impl::create_read_state(m_context, allocator);

		return true;
	}

	template<class database_settings_type>
	inline bool database_context<database_settings_type>::is_initialized() const { return m_context.is_initialized(); }

	template<class database_settings_type>
	inline void database_context<database_settings_type>::reset()
	{
		if (!is_initialized())
			return;	// Nothing to do

		ACL_ASSERT(!is_streaming(quality_tier::medium_importance), "Behavior is undefined if context is reset while streaming is in progress");
		ACL_ASSERT(!is_streaming(quality_tier::lowest_importance), "Behavior is undefined if context is reset while streaming is in progress");
		ACL_ASSERT(!acl_impl::has_active_readers(m_context), "Behavior is undefined if context is reset while a read scope is held");
		bool has_pending_operation = false;
		{
			// No deferred or in-flight operation may survive a reset; the caller must let every
			// request finish (including any completion held by a read scope) before resetting.
			std::lock_guard<std::mutex> guard(m_context.read_state->lock);
			for (const acl_impl::database_deferred_operation& operation : m_context.read_state->operations)
				has_pending_operation = has_pending_operation || operation.is_used();
		}
		ACL_ASSERT(!has_pending_operation, "Behavior is undefined if context is reset while a streaming request is pending");
		(void)has_pending_operation;

		// Tear down the read-scope bookkeeping first. The caller guarantees that every reader and
		// every streaming request has finished by this point.
		acl_impl::destroy_read_state(m_context);

		const uint32_t runtime_data_size = acl_impl::calculate_runtime_data_size(*m_context.db);
		deallocate_type_array(*m_context.allocator, acl_impl::bit_cast<uint8_t*>(m_context.loaded_chunks[0]), runtime_data_size);

		// Just reset the DB pointer, this will mark us as no longer initialized indicating everything is stale
		m_context.db = nullptr;
	}

	template<class database_settings_type>
	inline database_read_scope database_context<database_settings_type>::try_acquire_read()
	{
		if (!is_initialized())
			return database_read_scope();	// Invalid scope

		acl_impl::database_read_state* state = acl_impl::acquire_read(m_context);
		return database_read_scope(state);
	}

	template<class database_settings_type>
	inline bool database_context<database_settings_type>::relocated(const compressed_database& database)
	{
		if (!m_context.is_initialized())
			return false;	// Not initialized, cannot be relocated

		const bool is_valid = database.is_valid(false).empty();
		ACL_ASSERT(is_valid, "Invalid compressed database instance");
		if (!is_valid)
			return false;

		if (m_context.db_hash != database.get_hash())
			return false;	// Hash is different, this instance did not relocate, it is different

		// The instances are identical and might have relocated, update our metadata
		m_context.db = &database;
		m_context.bulk_data[0] = database.get_bulk_data(quality_tier::medium_importance);
		m_context.bulk_data[1] = database.get_bulk_data(quality_tier::lowest_importance);

		return true;
	}

	template<class database_settings_type>
	inline bool database_context<database_settings_type>::relocated(const compressed_database& database, database_streamer& medium_tier_streamer, database_streamer& low_tier_streamer)
	{
		if (!m_context.is_initialized())
			return false;	// Not initialized, cannot be relocated

		const bool is_valid = database.is_valid(false).empty();
		ACL_ASSERT(is_valid, "Invalid compressed database instance");
		if (!is_valid)
			return false;

		if (m_context.db_hash != database.get_hash())
			return false;	// Hash is different, this instance did not relocate, it is different

		// The instances are identical and might have relocated, update our metadata
		m_context.db = &database;
		m_context.streamers[0] = &medium_tier_streamer;
		m_context.streamers[1] = &low_tier_streamer;

		medium_tier_streamer.bind(m_context);
		low_tier_streamer.bind(m_context);

		return true;
	}

	template<class database_settings_type>
	inline bool database_context<database_settings_type>::is_bound_to(const compressed_database& database) const
	{
		if (m_context.db != &database)
			return false;	// Different pointer, no guarantees

		if (m_context.db_hash != database.get_hash())
			return false;	// Different hash

		// Must be bound to it!
		return true;
	}

	template<class database_settings_type>
	inline bool database_context<database_settings_type>::contains(const compressed_tracks& tracks) const
	{
		ACL_ASSERT(is_initialized(), "Database isn't initialized");
		if (!is_initialized())
			return false;

		if (!tracks.has_database())
			return false;	// Clip not bound to anything

		const acl_impl::transform_tracks_header& transform_header = acl_impl::get_transform_tracks_header(tracks);
		const acl_impl::tracks_database_header* tracks_db_header = transform_header.get_database_header();
		ACL_ASSERT(tracks_db_header != nullptr, "Expected a 'tracks_database_header'");

		if (!tracks_db_header->clip_header_offset.is_valid())
			return false;	// Invalid clip header offset

		const uint32_t num_clips = m_context.db->get_num_clips();
		const uint32_t num_segments = m_context.db->get_num_segments();

		uint32_t largest_offset = 0;
		largest_offset += num_clips * sizeof(acl_impl::database_runtime_clip_header);
		largest_offset += num_segments * sizeof(acl_impl::database_runtime_segment_header);

		// We can't read past the end of the last entry
		largest_offset -= sizeof(acl_impl::database_runtime_segment_header);

		if (tracks_db_header->clip_header_offset > largest_offset)
			return false;	// Invalid clip header offset

		const acl_impl::database_runtime_clip_header* db_clip_header = tracks_db_header->get_clip_header(m_context.clip_segment_headers);
		if (db_clip_header->clip_hash != tracks.get_hash())
			return false;	// Clip not bound to this database instance

		// All good
		return true;
	}

	template<class database_settings_type>
	inline bool database_context<database_settings_type>::is_streamed_in(quality_tier tier) const
	{
		ACL_ASSERT(tier != quality_tier::highest_importance, "The database does not contain data for the high importance tier, it lives inside compressed_tracks");
		ACL_ASSERT(is_initialized(), "Database isn't initialized");
		if (!is_initialized() || tier == quality_tier::highest_importance)
			return false;

		const uint32_t num_chunks = m_context.db->get_num_chunks(tier);
		const bitset_description desc = bitset_description::make_from_num_bits(num_chunks);
		const uint32_t tier_index = uint32_t(tier) - 1;

		const uint32_t* loaded_chunks = m_context.loaded_chunks[tier_index];
		const uint32_t num_loaded_chunks = bitset_count_set_bits(loaded_chunks, desc);

		return num_loaded_chunks == num_chunks;
	}

	template<class database_settings_type>
	inline bool database_context<database_settings_type>::is_streaming(quality_tier tier) const
	{
		ACL_ASSERT(tier != quality_tier::highest_importance, "The database does not contain data for the high importance tier, it lives inside compressed_tracks");
		ACL_ASSERT(is_initialized(), "Database isn't initialized");
		if (!is_initialized() || tier == quality_tier::highest_importance)
			return false;

		const uint32_t num_chunks = m_context.db->get_num_chunks(tier);
		const bitset_description desc = bitset_description::make_from_num_bits(num_chunks);
		const uint32_t tier_index = uint32_t(tier) - 1;

		const uint32_t* streaming_chunks = m_context.streaming_chunks[tier_index];
		const uint32_t num_streaming_chunks = bitset_count_set_bits(streaming_chunks, desc);

		return num_streaming_chunks != 0;
	}

	template<class database_settings_type>
	inline database_stream_request_result database_context<database_settings_type>::stream_in(quality_tier tier, uint32_t num_chunks_to_stream)
	{
		ACL_ASSERT(is_initialized(), "Database isn't initialized");
		if (!is_initialized())
			return database_stream_request_result::context_not_initialized;

		ACL_ASSERT(tier != quality_tier::highest_importance, "The database does not contain data for the high importance tier, it lives inside compressed_tracks");
		if (tier == quality_tier::highest_importance)
			return database_stream_request_result::invalid_database_tier;

		const acl_impl::database_header& header = acl_impl::get_database_header(*m_context.db);
		const acl_impl::database_chunk_description* chunk_descriptions = tier == quality_tier::medium_importance ? header.get_chunk_descriptions_medium() : header.get_chunk_descriptions_low();

		const uint32_t tier_index = uint32_t(tier) - 1;

		const uint32_t num_chunks = header.num_chunks[tier_index];
		const bitset_description desc = bitset_description::make_from_num_bits(num_chunks);
		const uint32_t max_chunk_size = header.max_chunk_size;

		database_streamer* streamer = m_context.streamers[tier_index];

		// Every residency decision and bookkeeping mutation is serialized. The lock is released only
		// across the user streamer call so a reentrant complete()/cancel() can re-acquire it.
		std::unique_lock<std::mutex> read_lock(m_context.read_state->lock);

		if (is_streaming(tier))
			return database_stream_request_result::streaming_in_progress;	// Can't stream while we are streaming

		uint32_t first_chunk_index = ~0U;

		// Clamp the total number of chunks we can stream
		num_chunks_to_stream = std::min<uint32_t>(num_chunks_to_stream, num_chunks);

		// Look for chunks that aren't loaded yet and aren't streaming yet. Chunk index i maps to
		// bit (31 - i), so the first unloaded chunk is the count of consecutive set bits starting
		// at the most significant bit. Counting leading zeros of the inverted word handles holes
		// left by a partial stream out; a fully loaded word inverts to zero and is skipped.
		const uint32_t num_entries = desc.get_size();
		const uint32_t* loaded_chunks = m_context.loaded_chunks[tier_index];
		for (uint32_t entry_index = 0; entry_index < num_entries; ++entry_index)
		{
			const uint32_t maybe_loaded = loaded_chunks[entry_index];
			const uint32_t num_loaded_prefix_chunks = count_leading_zeros(~maybe_loaded);
			if (num_loaded_prefix_chunks != 32)
			{
				first_chunk_index = (entry_index * 32) + num_loaded_prefix_chunks;
				break;
			}
		}

		if (first_chunk_index == ~0U)
			return database_stream_request_result::done;	// Everything is streamed in or streaming, nothing to do

		// Calculate and clamp our last chunk index (and handle wrapping for safety)
		const uint64_t last_chunk_index64 = uint64_t(first_chunk_index) + uint64_t(num_chunks_to_stream) - 1;
		const uint32_t last_chunk_index = last_chunk_index64 >= uint64_t(num_chunks) ? (num_chunks - 1) : uint32_t(last_chunk_index64);

		// Calculate our stream size and account for the fact that the last chunk doesn't have the same size
		const uint32_t num_streaming_chunks = last_chunk_index - first_chunk_index + 1;

		if (num_streaming_chunks == 0)
			return database_stream_request_result::done;	// Nothing more to stream

		const streaming_request_id request_id = streamer->build_request(streaming_action::stream_in, tier, first_chunk_index, num_streaming_chunks);
		if (!request_id.is_valid())
			return database_stream_request_result::no_free_streaming_requests;

		// Find the stream start offset from our first chunk's offset
		const acl_impl::database_chunk_description& first_chunk_description = chunk_descriptions[first_chunk_index];
		const uint32_t stream_start_offset = first_chunk_description.offset;

		const acl_impl::database_chunk_description& last_chunk_description = chunk_descriptions[last_chunk_index];
		const uint32_t stream_size = ((num_streaming_chunks - 1) * max_chunk_size) + last_chunk_description.size;

		// We can allocate our bulk data if we haven't already
		const uint8_t* bulk_data = m_context.bulk_data[tier_index];
		const bool can_allocate_bulk_data = bulk_data == nullptr;

		// Mark chunks as in-streaming
		uint32_t* streaming_chunks = m_context.streaming_chunks[tier_index];
		bitset_set_range(streaming_chunks, desc, first_chunk_index, num_streaming_chunks, true);

		// Record the request so a completion arriving on another thread is held while readers are active.
		acl_impl::register_dispatched_operation_locked(m_context, streaming_action::stream_in, tier,
			first_chunk_index, num_streaming_chunks, request_id, stream_start_offset, stream_size, can_allocate_bulk_data);

		read_lock.unlock();

		// Fire the stream in request and let the streamer handle it (sync/async)
		streamer->stream_in(stream_start_offset, stream_size, can_allocate_bulk_data, tier, request_id);

		return database_stream_request_result::dispatched;
	}

	template<class database_settings_type>
	inline database_stream_request_result database_context<database_settings_type>::stream_out(quality_tier tier, uint32_t num_chunks_to_stream)
	{
		ACL_ASSERT(is_initialized(), "Database isn't initialized");
		if (!is_initialized())
			return database_stream_request_result::context_not_initialized;

		ACL_ASSERT(tier != quality_tier::highest_importance, "The database does not contain data for the high importance tier, it lives inside compressed_tracks");
		if (tier == quality_tier::highest_importance)
			return database_stream_request_result::invalid_database_tier;

		const acl_impl::database_header& header = acl_impl::get_database_header(*m_context.db);
		const acl_impl::database_chunk_description* chunk_descriptions = tier == quality_tier::medium_importance ? header.get_chunk_descriptions_medium() : header.get_chunk_descriptions_low();

		const uint32_t tier_index = uint32_t(tier) - 1;

		const uint32_t num_chunks = header.num_chunks[tier_index];
		const bitset_description desc = bitset_description::make_from_num_bits(num_chunks);
		const uint32_t max_chunk_size = header.max_chunk_size;

		database_streamer* streamer = m_context.streamers[tier_index];

		// Every residency decision and bookkeeping mutation is serialized. The lock is released only
		// across the user streamer call so a reentrant complete()/cancel() can re-acquire it.
		std::unique_lock<std::mutex> read_lock(m_context.read_state->lock);

		if (is_streaming(tier))
			return database_stream_request_result::streaming_in_progress;	// Can't stream while we are streaming

		// Clamp the total number of chunks we can stream
		num_chunks_to_stream = std::min<uint32_t>(num_chunks_to_stream, num_chunks);

		uint32_t first_chunk_index = ~0U;

		// Look for chunks that aren't unloaded yet and aren't streaming yet
		const uint32_t num_entries = desc.get_size();
		const uint32_t* loaded_chunks = m_context.loaded_chunks[tier_index];
		for (uint32_t entry_index = 0; entry_index < num_entries; ++entry_index)
		{
			const uint32_t maybe_loaded = loaded_chunks[entry_index];
			const uint32_t num_pending_chunks = count_leading_zeros(maybe_loaded);
			if (num_pending_chunks != 32)
			{
				first_chunk_index = (entry_index * 32) + num_pending_chunks;
				break;
			}
		}

		if (first_chunk_index == ~0U)
			return database_stream_request_result::done;	// Everything is streamed out or streaming, nothing to do

		// Calculate and clamp our last chunk index (and handle wrapping for safety)
		const uint64_t last_chunk_index64 = uint64_t(first_chunk_index) + uint64_t(num_chunks_to_stream) - 1;
		const uint32_t last_chunk_index = last_chunk_index64 >= uint64_t(num_chunks) ? (num_chunks - 1) : uint32_t(last_chunk_index64);

		// Calculate our stream size and account for the fact that the last chunk doesn't have the same size
		const uint32_t num_streaming_chunks = last_chunk_index - first_chunk_index + 1;

		if (num_streaming_chunks == 0)
			return database_stream_request_result::done;	// Nothing more to stream

		const streaming_request_id request_id = streamer->build_request(streaming_action::stream_out, tier, first_chunk_index, num_streaming_chunks);
		if (!request_id.is_valid())
			return database_stream_request_result::no_free_streaming_requests;

		// Find the stream start offset from our first chunk's offset
		const acl_impl::database_chunk_description& first_chunk_description = chunk_descriptions[first_chunk_index];
		const uint32_t stream_start_offset = first_chunk_description.offset;

		const acl_impl::database_chunk_description& last_chunk_description = chunk_descriptions[last_chunk_index];
		const uint32_t stream_size = ((num_streaming_chunks - 1) * max_chunk_size) + last_chunk_description.size;

		// We can deallocate our bulk data if we are streaming out the last chunks
		const uint32_t num_loaded_chunks = bitset_count_set_bits(loaded_chunks, desc);
		const bool can_deallocate_bulk_data = num_streaming_chunks == num_loaded_chunks;

		// Mark chunks as in-streaming right away so repeated requests keep returning
		// streaming_in_progress, whether or not the actual eviction has to wait for readers.
		uint32_t* streaming_chunks = m_context.streaming_chunks[tier_index];
		bitset_set_range(streaming_chunks, desc, first_chunk_index, num_streaming_chunks, true);

		if (acl_impl::should_defer_stream_out_locked(*m_context.read_state))
		{
			// Readers are pinning this residency snapshot. Record the request without touching the
			// segment headers, the chunks, or the bulk data. The last reader dispatches it.
			const bool queued = acl_impl::defer_stream_out_locked(m_context, tier, first_chunk_index, num_streaming_chunks,
				request_id, stream_start_offset, stream_size, can_deallocate_bulk_data);
			ACL_ASSERT(queued, "Failed to queue a deferred stream out request");
			if (!queued)
			{
				// Should never happen: roll back the in-streaming bits and report we ran out of requests.
				bitset_set_range(streaming_chunks, desc, first_chunk_index, num_streaming_chunks, false);
				return database_stream_request_result::no_free_streaming_requests;
			}

			return database_stream_request_result::deferred;
		}

		// No reader is active: tear down the residency snapshot now, while still holding the lock.
		acl_impl::tear_down_streamed_out_chunks_locked(m_context, tier, first_chunk_index,
			num_streaming_chunks, can_deallocate_bulk_data);

		// Track the request so a completion arriving from another thread follows the publication rules.
		acl_impl::register_dispatched_operation_locked(m_context, streaming_action::stream_out, tier,
			first_chunk_index, num_streaming_chunks, request_id, stream_start_offset, stream_size, false);

		read_lock.unlock();

		// Fire the stream out request and let the streamer handle it (sync/async)
		streamer->stream_out(stream_start_offset, stream_size, can_deallocate_bulk_data, tier, request_id);

		return database_stream_request_result::dispatched;
	}

	ACL_IMPL_VERSION_NAMESPACE_END
}

ACL_IMPL_FILE_PRAGMA_POP
