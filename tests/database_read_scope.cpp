// Read-scope residency protocol tests for the ACL database runtime.
// Registered through task_support/check.h; no main() is defined here.

#include "task_support/check.h"

#include <acl/compression/compress.h>
#include <acl/compression/transform_error_metrics.h>
#include <acl/core/ansi_allocator.h>
#include <acl/core/track_writer.h>
#include <acl/decompression/decompress.h>
#include <acl/decompression/database/database.h>
#include <acl/decompression/database/database_streamer.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace acl;

namespace
{
	constexpr uint32_t k_num_bones = 12;
	constexpr uint32_t k_num_frames = 128;
	constexpr float k_sample_rate = 30.0f;

	struct test_decompression_settings : default_transform_decompression_settings
	{
		using database_settings_type = debug_database_settings;
	};

	using test_database_context = database_context<debug_database_settings>;
	using test_decompression_context = decompression_context<test_decompression_settings>;

	//////////////////////////////////////////////////////////////////////////
	// A streamer whose completion timing is fully controlled by the test. In
	// synchronous mode complete()/cancel() is called from the stream call itself
	// (reentrancy). In asynchronous mode the request is parked until the test
	// completes or cancels it, potentially from another thread.
	//////////////////////////////////////////////////////////////////////////
	class controllable_streamer final : public database_streamer
	{
	public:
		controllable_streamer(iallocator& allocator, const uint8_t* source_bulk_data, uint32_t bulk_data_size)
			: database_streamer(m_requests, k_max_num_requests)
			, m_allocator(allocator)
			, m_source_bulk_data(source_bulk_data)
			, m_bulk_data_size(bulk_data_size)
		{}

		virtual ~controllable_streamer() override
		{
			if (m_streamed_bulk_data != nullptr)
				deallocate_type_array(m_allocator, m_streamed_bulk_data, m_bulk_data_size);
		}

		void set_asynchronous(bool is_asynchronous) { m_asynchronous = is_asynchronous; }

		virtual bool is_initialized() const override { return m_bulk_data_size == 0 || m_source_bulk_data != nullptr; }

		virtual const uint8_t* get_bulk_data(quality_tier) const override { return m_streamed_bulk_data; }

		uint32_t get_num_stream_in_calls() const { return m_num_stream_in_calls.load(std::memory_order_relaxed); }
		uint32_t get_num_stream_out_calls() const { return m_num_stream_out_calls.load(std::memory_order_relaxed); }
		uint32_t get_live_bulk_size() const { return m_live_bulk_size.load(std::memory_order_relaxed); }
		bool has_pending_request() const { return m_pending_request_id.is_valid(); }

		virtual void stream_in(uint32_t offset, uint32_t size, bool can_allocate_bulk_data, quality_tier tier, streaming_request_id request_id) override
		{
			ACL_ASSERT(offset + size <= m_bulk_data_size, "Stream range outside of bulk data");
			m_num_stream_in_calls.fetch_add(1, std::memory_order_relaxed);

			if (can_allocate_bulk_data)
			{
				ACL_ASSERT(m_streamed_bulk_data == nullptr, "Bulk data already allocated");
				m_streamed_bulk_data = allocate_type_array<uint8_t>(m_allocator, m_bulk_data_size);
				std::memset(m_streamed_bulk_data, 0xCD, m_bulk_data_size);
				m_live_bulk_size.store(m_bulk_data_size, std::memory_order_relaxed);
			}

			std::memcpy(m_streamed_bulk_data + offset, m_source_bulk_data + offset, size);

			if (m_asynchronous)
			{
				std::lock_guard<std::mutex> guard(m_pending_lock);
				m_pending_request_id = request_id;
				m_pending_tier = tier;
			}
			else
			{
				complete(request_id);	// Reentrant completion from the calling thread
			}
		}

		virtual void stream_out(uint32_t offset, uint32_t size, bool can_deallocate_bulk_data, quality_tier tier, streaming_request_id request_id) override
		{
			ACL_ASSERT(offset + size <= m_bulk_data_size, "Stream range outside of bulk data");
			m_num_stream_out_calls.fetch_add(1, std::memory_order_relaxed);

			std::memset(m_streamed_bulk_data + offset, 0xCD, size);

			if (can_deallocate_bulk_data)
			{
				ACL_ASSERT(m_streamed_bulk_data != nullptr, "Bulk data already deallocated");
				deallocate_type_array(m_allocator, m_streamed_bulk_data, m_bulk_data_size);
				m_streamed_bulk_data = nullptr;
				m_live_bulk_size.store(0, std::memory_order_relaxed);
			}

			if (m_asynchronous)
			{
				std::lock_guard<std::mutex> guard(m_pending_lock);
				m_pending_request_id = request_id;
				m_pending_tier = tier;
			}
			else
			{
				complete(request_id);	// Reentrant completion from the calling thread
			}
		}

		void complete_pending()
		{
			streaming_request_id request_id;
			{
				std::lock_guard<std::mutex> guard(m_pending_lock);
				ACL_ASSERT(m_pending_request_id.is_valid(), "No pending request to complete");
				request_id = m_pending_request_id;
				m_pending_request_id = k_invalid_streamer_request_id;
			}
			complete(request_id);
		}

		void cancel_pending()
		{
			streaming_request_id request_id;
			{
				std::lock_guard<std::mutex> guard(m_pending_lock);
				ACL_ASSERT(m_pending_request_id.is_valid(), "No pending request to cancel");
				request_id = m_pending_request_id;
				m_pending_request_id = k_invalid_streamer_request_id;
			}
			cancel(request_id);
		}

	private:
		static constexpr uint32_t k_max_num_requests = 4;

		iallocator& m_allocator;
		const uint8_t* m_source_bulk_data;
		uint8_t* m_streamed_bulk_data = nullptr;
		uint32_t m_bulk_data_size;

		bool m_asynchronous = false;
		std::atomic<uint32_t> m_num_stream_in_calls{ 0 };
		std::atomic<uint32_t> m_num_stream_out_calls{ 0 };
		std::atomic<uint32_t> m_live_bulk_size{ 0 };

		std::mutex m_pending_lock;
		streaming_request_id m_pending_request_id = k_invalid_streamer_request_id;
		quality_tier m_pending_tier = quality_tier::highest_importance;

		streaming_request m_requests[k_max_num_requests];
	};

	//////////////////////////////////////////////////////////////////////////
	// Track writers
	//////////////////////////////////////////////////////////////////////////
	struct full_pose_writer : track_writer
	{
		explicit full_pose_writer(uint32_t num_bones) : pose(num_bones) {}

		void RTM_SIMD_CALL write_rotation(uint32_t i, rtm::quatf_arg0 v) { pose[i].rotation = v; }
		void RTM_SIMD_CALL write_translation(uint32_t i, rtm::vector4f_arg0 v) { pose[i].translation = v; }
		void RTM_SIMD_CALL write_scale(uint32_t i, rtm::vector4f_arg0 v) { pose[i].scale = v; }

		std::vector<rtm::vqvf> pose;
	};

	struct single_pose_writer : track_writer
	{
		void RTM_SIMD_CALL write_rotation(uint32_t, rtm::quatf_arg0 v) { pose.rotation = v; }
		void RTM_SIMD_CALL write_translation(uint32_t, rtm::vector4f_arg0 v) { pose.translation = v; }
		void RTM_SIMD_CALL write_scale(uint32_t, rtm::vector4f_arg0 v) { pose.scale = v; }

		rtm::vqvf pose = rtm::vqv_identity();
	};

	//////////////////////////////////////////////////////////////////////////
	// Compressed database fixture: many bones, many segments, two non-empty tiers.
	//////////////////////////////////////////////////////////////////////////
	struct database_fixture
	{
		ansi_allocator allocator;

		compressed_tracks* raw_clip = nullptr;			// Pre-database clip, kept for cleanup
		compressed_database* inline_database = nullptr;
		compressed_database* database = nullptr;		// Split database used at runtime
		compressed_tracks* database_clip = nullptr;
		uint8_t* medium_bulk = nullptr;
		uint8_t* low_bulk = nullptr;

		controllable_streamer* medium_streamer = nullptr;
		controllable_streamer* low_streamer = nullptr;
		test_database_context* context = nullptr;

		void setup()
		{
			track_array_vqvf tracks(allocator, k_num_bones);
			for (uint32_t bone = 0; bone < k_num_bones; ++bone)
			{
				track_desc_transformf desc;
				desc.output_index = bone;
				desc.parent_index = bone == 0 ? k_invalid_track_index : 0;
				desc.precision = 0.00001f;

				track_vqvf track = track_vqvf::make_reserve(desc, allocator, k_num_frames, k_sample_rate);
				for (uint32_t frame = 0; frame < k_num_frames; ++frame)
				{
					const float x = float(frame) * 0.2f;
					track[frame] = rtm::vqv_set(
						rtm::quat_identity(),
						rtm::vector_set(std::sin(x * float(bone + 1)) * 3.0f, float(bone) + std::cos(x * 0.5f), std::cos(x * float(bone + 2)) * 2.0f),
						rtm::vector_set(1.0f));
				}
				tracks[bone] = std::move(track);
			}

			compression_settings settings = get_default_compression_settings();
			vqvf_transform_error_metric metric;
			settings.error_metric = &metric;
			settings.enable_database_support = true;
			settings.keyframe_stripping = compression_keyframe_stripping_settings{};

			output_stats stats;
			error_result error = compress_track_list(allocator, tracks, settings, raw_clip, stats);
			ACL_ASSERT(error.empty(), "Compression failed: %s", error.c_str());

			compression_database_settings database_settings;
			database_settings.medium_importance_tier_proportion = 0.4f;
			database_settings.low_importance_tier_proportion = 0.4f;
			database_settings.max_chunk_size = 4096;

			const compressed_tracks* inputs[] = { raw_clip };
			compressed_tracks* database_clips[] = { nullptr };
			error = build_database(allocator, database_settings, inputs, 1, database_clips, inline_database);
			ACL_ASSERT(error.empty(), "Database build failed: %s", error.c_str());
			database_clip = database_clips[0];

			error = split_database_bulk_data(allocator, *inline_database, database, medium_bulk, low_bulk);
			ACL_ASSERT(error.empty(), "Bulk data split failed: %s", error.c_str());

			ACL_ASSERT(database->get_num_chunks(quality_tier::medium_importance) >= 2, "Need at least two medium chunks");
			ACL_ASSERT(database->get_num_chunks(quality_tier::lowest_importance) >= 2, "Need at least two low chunks");
			ACL_ASSERT(database->get_num_segments() >= 2, "Need multiple segments");

			medium_streamer = new controllable_streamer(allocator, medium_bulk, database->get_bulk_data_size(quality_tier::medium_importance));
			low_streamer = new controllable_streamer(allocator, low_bulk, database->get_bulk_data_size(quality_tier::lowest_importance));
			context = new test_database_context();

			const bool initialized = context->initialize(allocator, *database, *medium_streamer, *low_streamer);
			ACL_ASSERT(initialized, "Database context failed to initialize");
		}

		void teardown()
		{
			delete context;
			delete low_streamer;
			delete medium_streamer;
			allocator.deallocate(low_bulk, database->get_bulk_data_size(quality_tier::lowest_importance));
			allocator.deallocate(medium_bulk, database->get_bulk_data_size(quality_tier::medium_importance));
			allocator.deallocate(database, database->get_size());
			allocator.deallocate(database_clip, database_clip->get_size());
			allocator.deallocate(inline_database, inline_database->get_size());
			allocator.deallocate(raw_clip, raw_clip->get_size());
		}

		void stream_everything_in_synchronously()
		{
			medium_streamer->set_asynchronous(false);
			low_streamer->set_asynchronous(false);
			const database_stream_request_result medium_result = context->stream_in(quality_tier::medium_importance);
			const database_stream_request_result low_result = context->stream_in(quality_tier::lowest_importance);
			ACL_ASSERT(medium_result == database_stream_request_result::dispatched, "Medium stream in failed");
			ACL_ASSERT(low_result == database_stream_request_result::dispatched, "Low stream in failed");
			ACL_ASSERT(context->is_streamed_in(quality_tier::medium_importance), "Medium tier not fully resident");
			ACL_ASSERT(context->is_streamed_in(quality_tier::lowest_importance), "Low tier not fully resident");
		}
	};

	//////////////////////////////////////////////////////////////////////////
	// Pose helpers
	//////////////////////////////////////////////////////////////////////////
	std::vector<rtm::vqvf> capture_full_pose(test_decompression_context& decompressor, uint32_t frame)
	{
		full_pose_writer writer(k_num_bones);
		decompressor.seek(float(frame) / k_sample_rate, sample_rounding_policy::none);
		decompressor.decompress_tracks(writer);
		return writer.pose;
	}

	rtm::vqvf capture_single_pose(test_decompression_context& decompressor, uint32_t frame, uint32_t bone)
	{
		single_pose_writer writer;
		decompressor.seek(float(frame) / k_sample_rate, sample_rounding_policy::none);
		decompressor.decompress_track(bone, writer);
		return writer.pose;
	}

	float component_distance(const rtm::vector4f& lhs, const rtm::vector4f& rhs)
	{
		const float dx = rtm::vector_get_x(lhs) - rtm::vector_get_x(rhs);
		const float dy = rtm::vector_get_y(lhs) - rtm::vector_get_y(rhs);
		const float dz = rtm::vector_get_z(lhs) - rtm::vector_get_z(rhs);
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	float max_pose_distance(const std::vector<rtm::vqvf>& lhs, const std::vector<rtm::vqvf>& rhs)
	{
		float result = 0.0f;
		for (uint32_t bone = 0; bone < k_num_bones; ++bone)
		{
			result = std::fmax(result, component_distance(lhs[bone].translation, rhs[bone].translation));
			result = std::fmax(result, component_distance(lhs[bone].scale, rhs[bone].scale));
			const rtm::vector4f ql = rtm::quat_to_vector(lhs[bone].rotation);
			const rtm::vector4f qr = rtm::quat_to_vector(rhs[bone].rotation);
			result = std::fmax(result, component_distance(ql, qr));
		}
		return result;
	}

	void init_decompressor(test_decompression_context& decompressor, test_database_context& database_context, const compressed_tracks& clip)
	{
		const bool initialized = decompressor.initialize(clip, database_context);
		ACL_ASSERT(initialized, "Decompression context failed to initialize");
	}

	//////////////////////////////////////////////////////////////////////////
	// Shared frame indices spread across segment boundaries (7 segments / 128 frames).
	//////////////////////////////////////////////////////////////////////////
	constexpr uint32_t k_sample_frames[] = { 0, 5, 17, 33, 40, 63, 64, 90, 96, 127 };
	constexpr uint32_t k_num_sample_frames = sizeof(k_sample_frames) / sizeof(k_sample_frames[0]);
	constexpr float k_quality_change_epsilon = 1e-5f;

	std::vector<std::vector<rtm::vqvf>> capture_reference_poses(test_decompression_context& decompressor)
	{
		std::vector<std::vector<rtm::vqvf>> poses;
		poses.reserve(k_num_sample_frames);
		for (uint32_t frame : k_sample_frames)
			poses.push_back(capture_full_pose(decompressor, frame));
		return poses;
	}

	float distance_to_reference(test_decompression_context& decompressor, const std::vector<std::vector<rtm::vqvf>>& reference)
	{
		float distance = 0.0f;
		for (uint32_t index = 0; index < k_num_sample_frames; ++index)
		{
			const std::vector<rtm::vqvf> pose = capture_full_pose(decompressor, k_sample_frames[index]);
			distance = std::fmax(distance, max_pose_distance(pose, reference[index]));
		}
		return distance;
	}

	//////////////////////////////////////////////////////////////////////////
	// residency group
	//////////////////////////////////////////////////////////////////////////
	int run_residency_defers_full_eviction()
	{
		database_fixture fixture;
		fixture.setup();
		fixture.stream_everything_in_synchronously();

		test_decompression_context decompressor;
		init_decompressor(decompressor, *fixture.context, *fixture.database_clip);

		const std::vector<std::vector<rtm::vqvf>> full_reference = capture_reference_poses(decompressor);
		const uint32_t low_bulk_size = fixture.database->get_bulk_data_size(quality_tier::lowest_importance);

		database_read_scope scope = fixture.context->try_acquire_read();
		TASK_CHECK(scope.is_valid());

		// The eviction is recorded, not executed.
		const database_stream_request_result result = fixture.context->stream_out(quality_tier::lowest_importance);
		TASK_CHECK(result == database_stream_request_result::deferred);
		TASK_CHECK(fixture.low_streamer->get_num_stream_out_calls() == 0);
		TASK_CHECK(fixture.low_streamer->get_live_bulk_size() == low_bulk_size);
		TASK_CHECK(fixture.context->is_streaming(quality_tier::lowest_importance));

		// Full track and single track decompression keep returning the pinned snapshot.
		TASK_CHECK(distance_to_reference(decompressor, full_reference) == 0.0f);
		for (uint32_t index = 0; index < k_num_sample_frames; index += 2)
		{
			const uint32_t frame = k_sample_frames[index];
			for (uint32_t bone : { 0u, 3u, 7u, 11u })
			{
				const rtm::vqvf single = capture_single_pose(decompressor, frame, bone);
				const float translation_delta = component_distance(single.translation, full_reference[index][bone].translation);
				TASK_CHECK(translation_delta == 0.0f);
			}
		}

		// Repeated seeks while the scope is held cannot pick up a publication that lands in between.
		// The deferred eviction keeps the tier busy, so another request retains the old
		// streaming_in_progress meaning (and never reaches the streamer).
		TASK_CHECK(fixture.context->stream_in(quality_tier::lowest_importance) == database_stream_request_result::streaming_in_progress);
		for (uint32_t iteration = 0; iteration < 16; ++iteration)
			TASK_CHECK(distance_to_reference(decompressor, full_reference) == 0.0f);

		scope.release();
		TASK_CHECK(!scope.is_valid());

		// Last reader released: the deferred eviction ran and the bulk memory was actually reclaimed.
		TASK_CHECK(fixture.low_streamer->get_num_stream_out_calls() == 1);
		TASK_CHECK(fixture.low_streamer->get_live_bulk_size() == 0);
		TASK_CHECK(!fixture.context->is_streamed_in(quality_tier::lowest_importance));
		TASK_CHECK(distance_to_reference(decompressor, full_reference) > k_quality_change_epsilon);

		fixture.teardown();
		return 0;
	}

	int run_residency_partial_chunk_requests()
	{
		database_fixture fixture;
		fixture.setup();
		fixture.stream_everything_in_synchronously();

		test_decompression_context decompressor;
		init_decompressor(decompressor, *fixture.context, *fixture.database_clip);
		const std::vector<std::vector<rtm::vqvf>> full_reference = capture_reference_poses(decompressor);

		const uint32_t medium_bulk_size = fixture.database->get_bulk_data_size(quality_tier::medium_importance);
		const uint32_t low_bulk_size = fixture.database->get_bulk_data_size(quality_tier::lowest_importance);

		// One chunk per tier, both deferred while a reader pins the snapshot.
		{
			database_read_scope scope = fixture.context->try_acquire_read();
			TASK_CHECK(scope.is_valid());
			TASK_CHECK(fixture.context->stream_out(quality_tier::lowest_importance, 1) == database_stream_request_result::deferred);
			TASK_CHECK(fixture.context->stream_out(quality_tier::medium_importance, 1) == database_stream_request_result::deferred);
			TASK_CHECK(distance_to_reference(decompressor, full_reference) == 0.0f);
			scope.release();
		}

		// Partial evictions keep the bulk allocation alive while dropping residency.
		TASK_CHECK(fixture.low_streamer->get_live_bulk_size() == low_bulk_size);
		TASK_CHECK(fixture.medium_streamer->get_live_bulk_size() == medium_bulk_size);
		TASK_CHECK(!fixture.context->is_streamed_in(quality_tier::lowest_importance));
		TASK_CHECK(!fixture.context->is_streamed_in(quality_tier::medium_importance));
		TASK_CHECK(!fixture.context->is_streaming(quality_tier::lowest_importance));
		TASK_CHECK(!fixture.context->is_streaming(quality_tier::medium_importance));
		TASK_CHECK(distance_to_reference(decompressor, full_reference) > k_quality_change_epsilon);

		// Repeated requests on a tier still use the streaming_in_progress meaning. With an
		// asynchronous streamer the first request stays in flight, so the second is rejected.
		fixture.low_streamer->set_asynchronous(true);
		TASK_CHECK(fixture.context->stream_in(quality_tier::lowest_importance) == database_stream_request_result::dispatched);
		TASK_CHECK(fixture.context->stream_in(quality_tier::lowest_importance) == database_stream_request_result::streaming_in_progress);
		fixture.low_streamer->complete_pending();
		TASK_CHECK(fixture.context->is_streamed_in(quality_tier::lowest_importance));
		fixture.low_streamer->set_asynchronous(false);

		fixture.teardown();
		return 0;
	}

	int run_residency_scope_is_movable()
	{
		database_fixture fixture;
		fixture.setup();
		fixture.stream_everything_in_synchronously();

		database_read_scope first = fixture.context->try_acquire_read();
		TASK_CHECK(first.is_valid());

		database_read_scope moved(std::move(first));
		TASK_CHECK(!first.is_valid());
		TASK_CHECK(moved.is_valid());

		// While the moved-to scope is held, eviction is deferred.
		TASK_CHECK(fixture.context->stream_out(quality_tier::lowest_importance) == database_stream_request_result::deferred);

		database_read_scope assigned;
		TASK_CHECK(!assigned.is_valid());
		assigned = std::move(moved);
		TASK_CHECK(!moved.is_valid());
		TASK_CHECK(assigned.is_valid());

		assigned.release();
		TASK_CHECK(!assigned.is_valid());
		TASK_CHECK(fixture.low_streamer->get_num_stream_out_calls() == 1);

		fixture.teardown();
		return 0;
	}
}

namespace
{
	//////////////////////////////////////////////////////////////////////////
	// publication group
	//////////////////////////////////////////////////////////////////////////
	int run_publication_async_completion_held()
	{
		database_fixture fixture;
		fixture.setup();
		fixture.stream_everything_in_synchronously();

		test_decompression_context decompressor;
		init_decompressor(decompressor, *fixture.context, *fixture.database_clip);
		const std::vector<std::vector<rtm::vqvf>> full_reference = capture_reference_poses(decompressor);

		// Drop the low tier synchronously so we have something to stream back in.
		TASK_CHECK(fixture.context->stream_out(quality_tier::lowest_importance) == database_stream_request_result::dispatched);
		TASK_CHECK(!fixture.context->is_streamed_in(quality_tier::lowest_importance));
		const std::vector<std::vector<rtm::vqvf>> low_quality_reference = capture_reference_poses(decompressor);
		float low_tier_contribution = 0.0f;
		for (uint32_t index = 0; index < k_num_sample_frames; ++index)
			low_tier_contribution = std::fmax(low_tier_contribution, max_pose_distance(full_reference[index], low_quality_reference[index]));
		TASK_CHECK(low_tier_contribution > k_quality_change_epsilon);

		fixture.low_streamer->set_asynchronous(true);

		database_read_scope scope = fixture.context->try_acquire_read();
		TASK_CHECK(scope.is_valid());

		// The stream in is dispatched immediately and the IO parks inside the asynchronous streamer.
		TASK_CHECK(fixture.context->stream_in(quality_tier::lowest_importance) == database_stream_request_result::dispatched);
		TASK_CHECK(fixture.low_streamer->has_pending_request());

		// Until the completion publishes, the scope keeps seeing the old, lower quality snapshot.
		TASK_CHECK(distance_to_reference(decompressor, low_quality_reference) == 0.0f);
		TASK_CHECK(!fixture.context->is_streamed_in(quality_tier::lowest_importance));

		// The completion arrives from another thread while readers are still inside the scope.
		std::thread completer([&] { fixture.low_streamer->complete_pending(); });
		completer.join();

		// The completion record is held: the segment headers and loaded bits are not published yet.
		TASK_CHECK(!fixture.context->is_streamed_in(quality_tier::lowest_importance));
		TASK_CHECK(distance_to_reference(decompressor, low_quality_reference) == 0.0f);

		scope.release();

		// Once the last reader leaves, the held completion publishes and full quality is restored.
		TASK_CHECK(fixture.context->is_streamed_in(quality_tier::lowest_importance));
		TASK_CHECK(distance_to_reference(decompressor, full_reference) == 0.0f);

		fixture.teardown();
		return 0;
	}

	int run_publication_cancel_follows_old_rules()
	{
		database_fixture fixture;
		fixture.setup();
		fixture.stream_everything_in_synchronously();

		test_decompression_context decompressor;
		init_decompressor(decompressor, *fixture.context, *fixture.database_clip);
		const std::vector<std::vector<rtm::vqvf>> full_reference = capture_reference_poses(decompressor);

		TASK_CHECK(fixture.context->stream_out(quality_tier::lowest_importance) == database_stream_request_result::dispatched);
		const std::vector<std::vector<rtm::vqvf>> low_quality_reference = capture_reference_poses(decompressor);

		fixture.low_streamer->set_asynchronous(true);

		database_read_scope scope = fixture.context->try_acquire_read();
		TASK_CHECK(scope.is_valid());

		TASK_CHECK(fixture.context->stream_in(quality_tier::lowest_importance) == database_stream_request_result::dispatched);
		TASK_CHECK(fixture.low_streamer->has_pending_request());

		// Cancellation follows the old rules: a canceled stream in does not publish any chunk.
		// It is held until the readers leave, so the scope keeps the low quality snapshot.
		fixture.low_streamer->cancel_pending();
		TASK_CHECK(distance_to_reference(decompressor, low_quality_reference) == 0.0f);

		scope.release();

		TASK_CHECK(!fixture.context->is_streamed_in(quality_tier::lowest_importance));
		TASK_CHECK(!fixture.context->is_streaming(quality_tier::lowest_importance));
		TASK_CHECK(distance_to_reference(decompressor, low_quality_reference) == 0.0f);

		// The tier can be loaded again through the normal synchronous path.
		fixture.low_streamer->set_asynchronous(false);
		TASK_CHECK(fixture.context->stream_in(quality_tier::lowest_importance) == database_stream_request_result::dispatched);
		TASK_CHECK(fixture.context->is_streamed_in(quality_tier::lowest_importance));
		TASK_CHECK(distance_to_reference(decompressor, full_reference) == 0.0f);

		fixture.teardown();
		return 0;
	}

	int run_publication_synchronous_reentrant_completion()
	{
		database_fixture fixture;
		fixture.setup();
		fixture.stream_everything_in_synchronously();

		test_decompression_context decompressor;
		init_decompressor(decompressor, *fixture.context, *fixture.database_clip);
		const std::vector<std::vector<rtm::vqvf>> full_reference = capture_reference_poses(decompressor);

		// Synchronous streamers call complete() reentrantly from stream_out/stream_in.
		// With no reader, the old synchronous behavior must be preserved.
		TASK_CHECK(fixture.context->stream_out(quality_tier::lowest_importance) == database_stream_request_result::dispatched);
		TASK_CHECK(fixture.low_streamer->get_num_stream_out_calls() == 1);
		TASK_CHECK(!fixture.context->is_streamed_in(quality_tier::lowest_importance));
		TASK_CHECK(distance_to_reference(decompressor, full_reference) > k_quality_change_epsilon);

		TASK_CHECK(fixture.context->stream_in(quality_tier::lowest_importance) == database_stream_request_result::dispatched);
		TASK_CHECK(fixture.context->is_streamed_in(quality_tier::lowest_importance));
		TASK_CHECK(distance_to_reference(decompressor, full_reference) == 0.0f);

		// With a reader, a deferred eviction is dispatched on release and completes reentrantly
		// inside the release call, without deadlocking on the internal lock.
		{
			database_read_scope scope = fixture.context->try_acquire_read();
			TASK_CHECK(scope.is_valid());
			TASK_CHECK(fixture.context->stream_out(quality_tier::lowest_importance) == database_stream_request_result::deferred);
			scope.release();
		}

		TASK_CHECK(fixture.low_streamer->get_num_stream_out_calls() == 2);
		TASK_CHECK(!fixture.context->is_streamed_in(quality_tier::lowest_importance));

		fixture.teardown();
		return 0;
	}

	int run_publication_gate_reopens_after_commit()
	{
		database_fixture fixture;
		fixture.setup();
		fixture.stream_everything_in_synchronously();

		test_decompression_context decompressor;
		init_decompressor(decompressor, *fixture.context, *fixture.database_clip);
		const std::vector<std::vector<rtm::vqvf>> full_reference = capture_reference_poses(decompressor);

		fixture.low_streamer->set_asynchronous(true);

		// An in-flight request closes the acquisition gate even when no reader is active.
		TASK_CHECK(fixture.context->stream_out(quality_tier::lowest_importance, 1) == database_stream_request_result::dispatched);
		TASK_CHECK(fixture.low_streamer->has_pending_request());

		database_read_scope blocked = fixture.context->try_acquire_read();
		TASK_CHECK(!blocked.is_valid());

		fixture.low_streamer->complete_pending();

		// After the pending operation commits, acquisition is allowed again immediately.
		database_read_scope scope = fixture.context->try_acquire_read();
		TASK_CHECK(scope.is_valid());
		TASK_CHECK(std::isfinite(distance_to_reference(decompressor, full_reference)));
		scope.release();

		// And the gate stays open with nothing pending.
		database_read_scope second_scope = fixture.context->try_acquire_read();
		TASK_CHECK(second_scope.is_valid());
		second_scope.release();

		fixture.teardown();
		return 0;
	}
}

namespace
{
	//////////////////////////////////////////////////////////////////////////
	// threads group
	//////////////////////////////////////////////////////////////////////////
	int run_threads_two_readers_release_in_order()
	{
		database_fixture fixture;
		fixture.setup();
		fixture.stream_everything_in_synchronously();

		test_decompression_context first_decompressor;
		init_decompressor(first_decompressor, *fixture.context, *fixture.database_clip);
		test_decompression_context second_decompressor;
		init_decompressor(second_decompressor, *fixture.context, *fixture.database_clip);

		const std::vector<std::vector<rtm::vqvf>> full_reference = capture_reference_poses(first_decompressor);

		database_read_scope first_scope = fixture.context->try_acquire_read();
		database_read_scope second_scope = fixture.context->try_acquire_read();
		TASK_CHECK(first_scope.is_valid() && second_scope.is_valid());

		std::atomic<bool> reader_checked(false);
		std::atomic<bool> release_reader(false);
		std::atomic<bool> reader_done(false);
		std::atomic<uint32_t> reader_failures(0);

		std::thread reader([&]
		{
			// This thread owns second_decompressor exclusively.
			while (!release_reader.load(std::memory_order_acquire))
			{
				if (distance_to_reference(second_decompressor, full_reference) != 0.0f)
					reader_failures.fetch_add(1, std::memory_order_relaxed);
				reader_checked.store(true, std::memory_order_release);
				std::this_thread::yield();
			}
			second_scope.release();
			reader_done.store(true, std::memory_order_release);
		});

		TASK_CHECK(fixture.context->stream_out(quality_tier::lowest_importance) == database_stream_request_result::deferred);
		TASK_CHECK(fixture.low_streamer->get_num_stream_out_calls() == 0);

		// The main thread keeps reading through its own decompressor while the worker reads too.
		for (uint32_t iteration = 0; iteration < 32; ++iteration)
			TASK_CHECK(distance_to_reference(first_decompressor, full_reference) == 0.0f);

		// Wait for the worker to confirm it has sampled the pinned snapshot at least once.
		while (!reader_checked.load(std::memory_order_acquire))
			std::this_thread::yield();

		// First reader leaves: the second reader still pins the snapshot.
		release_reader.store(true, std::memory_order_release);
		while (!reader_done.load(std::memory_order_acquire))
			std::this_thread::yield();
		reader.join();

		TASK_CHECK(reader_checked.load());
		TASK_CHECK(reader_failures.load() == 0);
		TASK_CHECK(fixture.low_streamer->get_num_stream_out_calls() == 0);
		TASK_CHECK(fixture.context->is_streamed_in(quality_tier::lowest_importance));
		TASK_CHECK(distance_to_reference(first_decompressor, full_reference) == 0.0f);

		// Last reader leaves: the deferred eviction commits.
		first_scope.release();
		TASK_CHECK(fixture.low_streamer->get_num_stream_out_calls() == 1);
		TASK_CHECK(!fixture.context->is_streamed_in(quality_tier::lowest_importance));
		TASK_CHECK(distance_to_reference(first_decompressor, full_reference) > k_quality_change_epsilon);

		fixture.teardown();
		return 0;
	}

	int run_threads_acquire_eviction_handshake()
	{
		database_fixture fixture;
		fixture.setup();
		fixture.stream_everything_in_synchronously();

		test_decompression_context reader_decompressor;
		init_decompressor(reader_decompressor, *fixture.context, *fixture.database_clip);
		const std::vector<std::vector<rtm::vqvf>> full_reference = capture_reference_poses(reader_decompressor);

		constexpr uint32_t k_num_rounds = 50;

		std::atomic<bool> stop(false);
		std::atomic<uint32_t> scopes_acquired(0);
		std::atomic<uint32_t> scopes_rejected(0);
		std::atomic<uint32_t> reader_failures(0);

		// Monotonic round counters. Each side waits until the other reaches the current round,
		// which avoids the reset race that plain boolean flags would have across round boundaries.
		std::atomic<uint32_t> reader_holds(0);			// Reader has acquired its scope for round r
		std::atomic<uint32_t> streamer_issued_round(0);	// Streamer has issued its requests for round r
		std::atomic<uint32_t> reader_released_round(0);	// Reader has released its scope for round r
		std::atomic<uint32_t> streamer_complete_round(0);	// Streamer has finished round r

		auto wait_until = [](const std::atomic<uint32_t>& counter, uint32_t threshold)
		{
			while (counter.load(std::memory_order_acquire) < threshold)
				std::this_thread::yield();
		};

		// The reader thread only ever touches its own decompression context and its own scopes.
		std::thread reader([&]
		{
			uint32_t frame_selector = 0;
			for (uint32_t round = 0; round < k_num_rounds && !stop.load(std::memory_order_acquire); ++round)
			{
				// The gate is open at the start of every round because the previous round reloaded
				// everything and cleared every pending operation. Retry defensively anyway.
				database_read_scope scope = fixture.context->try_acquire_read();
				while (!scope.is_valid())
				{
					scopes_rejected.fetch_add(1, std::memory_order_relaxed);
					std::this_thread::yield();
					scope = fixture.context->try_acquire_read();
				}

				scopes_acquired.fetch_add(1, std::memory_order_relaxed);

				// Invariant: whatever residency was visible when the scope started stays visible
				// for its whole lifetime, even though the streamer defers an eviction meanwhile.
				const uint32_t frame = k_sample_frames[frame_selector % k_num_sample_frames];
				const std::vector<rtm::vqvf> first_pose = capture_full_pose(reader_decompressor, frame);
				reader_holds.store(round + 1, std::memory_order_release);

				wait_until(streamer_issued_round, round + 1);

				const std::vector<rtm::vqvf> second_pose = capture_full_pose(reader_decompressor, frame);
				if (max_pose_distance(first_pose, second_pose) != 0.0f)
					reader_failures.fetch_add(1, std::memory_order_relaxed);
				++frame_selector;

				scope.release();
				reader_released_round.store(round + 1, std::memory_order_release);

				wait_until(streamer_complete_round, round + 1);
			}
		});

		uint32_t deferred_results = 0;
		uint32_t in_progress_results = 0;

		for (uint32_t round = 0; round < k_num_rounds; ++round)
		{
			wait_until(reader_holds, round + 1);

			// A reader is active: the eviction must be deferred rather than touch the snapshot.
			const database_stream_request_result out_result = fixture.context->stream_out(quality_tier::lowest_importance);
			TASK_CHECK(out_result == database_stream_request_result::deferred);
			++deferred_results;

			// The deferred request keeps the tier busy, so another request retains the
			// streaming_in_progress meaning and never reaches the streamer.
			const database_stream_request_result busy_result = fixture.context->stream_in(quality_tier::lowest_importance);
			TASK_CHECK(busy_result == database_stream_request_result::streaming_in_progress);
			++in_progress_results;

			streamer_issued_round.store(round + 1, std::memory_order_release);
			wait_until(reader_released_round, round + 1);

			// The last reader's release pumps the deferred eviction, so the tier is no longer fully resident.
			TASK_CHECK(!fixture.context->is_streamed_in(quality_tier::lowest_importance));

			// No reader now: reload restores full residency through the ordinary dispatched path.
			database_stream_request_result in_result = fixture.context->stream_in(quality_tier::lowest_importance);
			while (in_result == database_stream_request_result::streaming_in_progress)
			{
				std::this_thread::yield();
				in_result = fixture.context->stream_in(quality_tier::lowest_importance);
			}
			TASK_CHECK(in_result == database_stream_request_result::dispatched || in_result == database_stream_request_result::done);
			TASK_CHECK(fixture.context->is_streamed_in(quality_tier::lowest_importance));

			streamer_complete_round.store(round + 1, std::memory_order_release);
		}

		stop.store(true, std::memory_order_release);
		reader.join();

		TASK_CHECK(scopes_acquired.load() == k_num_rounds);
		TASK_CHECK(deferred_results == k_num_rounds);
		TASK_CHECK(in_progress_results == k_num_rounds);
		TASK_CHECK(reader_failures.load() == 0);
		TASK_CHECK(fixture.context->is_streamed_in(quality_tier::lowest_importance));
		TASK_CHECK(distance_to_reference(reader_decompressor, full_reference) == 0.0f);

		fixture.teardown();
		return 0;
	}

	int run_threads_async_completion_races_release()
	{
		database_fixture fixture;
		fixture.setup();
		fixture.stream_everything_in_synchronously();

		test_decompression_context decompressor;
		init_decompressor(decompressor, *fixture.context, *fixture.database_clip);
		const std::vector<std::vector<rtm::vqvf>> full_reference = capture_reference_poses(decompressor);

		TASK_CHECK(fixture.context->stream_out(quality_tier::lowest_importance) == database_stream_request_result::dispatched);
		TASK_CHECK(!fixture.context->is_streamed_in(quality_tier::lowest_importance));

		fixture.low_streamer->set_asynchronous(true);

		database_read_scope scope = fixture.context->try_acquire_read();
		TASK_CHECK(scope.is_valid());

		// The stream in is in flight inside the asynchronous streamer.
		TASK_CHECK(fixture.context->stream_in(quality_tier::lowest_importance) == database_stream_request_result::dispatched);
		TASK_CHECK(fixture.low_streamer->has_pending_request());

		// The completion races the release from another thread. Whichever wins, the held completion
		// must publish exactly once and end with the full tier resident.
		std::thread completer([&] { fixture.low_streamer->complete_pending(); });
		scope.release();
		completer.join();

		TASK_CHECK(fixture.low_streamer->get_num_stream_in_calls() == 2);	// Initial load plus this reload
		TASK_CHECK(fixture.context->is_streamed_in(quality_tier::lowest_importance));
		TASK_CHECK(!fixture.context->is_streaming(quality_tier::lowest_importance));
		TASK_CHECK(distance_to_reference(decompressor, full_reference) == 0.0f);

		fixture.teardown();
		return 0;
	}
}

//////////////////////////////////////////////////////////////////////////
// Registration
//////////////////////////////////////////////////////////////////////////
const task_case task_cases[] =
{
	{ "residency", "defers_full_eviction", run_residency_defers_full_eviction },
	{ "residency", "partial_chunk_requests", run_residency_partial_chunk_requests },
	{ "residency", "scope_is_movable", run_residency_scope_is_movable },

	{ "publication", "async_completion_held", run_publication_async_completion_held },
	{ "publication", "cancel_follows_old_rules", run_publication_cancel_follows_old_rules },
	{ "publication", "synchronous_reentrant_completion", run_publication_synchronous_reentrant_completion },
	{ "publication", "gate_reopens_after_commit", run_publication_gate_reopens_after_commit },

	{ "threads", "two_readers_release_in_order", run_threads_two_readers_release_in_order },
	{ "threads", "acquire_eviction_handshake", run_threads_acquire_eviction_handshake },
	{ "threads", "async_completion_races_release", run_threads_async_completion_races_release },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
