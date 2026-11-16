// basisu_texture_job.h - pausable/cancellable ETC1S .basis video texture playback.
// Copyright (C) 2019-2026 Binomial LLC. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once

#include "basisu_transcoder.h"

#include <cstdint>

namespace basist
{
	// Result codes returned by the pausable texture player.
	//
	// kTextureJobRunning, kTextureJobFrameComplete and kTextureJobFrameStarted are successful outcomes. The rest are
	// definite rejections or decode failures that leave the player idle (or, for a failed open(), unchanged).
	enum class texture_job_result : int
	{
		kTextureJobRunning = 0,		// step() consumed its budget (or was given zero budget); the working frame is incomplete.
		kTextureJobFrameComplete,	// step() finished and published the working frame; front() now exposes it.
		kTextureJobFrameStarted,	// begin_frame() accepted the frame; work may begin.
		kTextureJobNotOpen,			// No file is open.
		kTextureJobBusy,			// begin_frame() rejected: another working frame is already in progress.
		kTextureJobIdle,			// step() called while no working frame exists (and the last frame wasn't just completed).
		kTextureJobInvalidFrame,	// begin_frame() rejected: image_index is not the next allowed frame.
		kTextureJobDecodeError		// A block decode or publish failed; the working frame was discarded.
	};

	// Progress counters for the player. All units are source 4x4 slice blocks, not output pixels.
	struct texture_job_progress
	{
		// Total source blocks decoded across all frames since open() succeeded (including cancelled/failed work).
		uint64_t m_total_source_blocks_decoded;

		// Source blocks fully completed in the current/last working frame. A block counts as complete when both its
		// alpha (if present) and RGB blocks have been decoded and expanded into the working frame.
		uint32_t m_frame_blocks_completed;

		// True if the working frame currently has an alpha block decoded but not yet its matching RGB block (i.e.
		// step() stopped between the alpha and RGB halves of one dual-slice position). Reset by begin_frame(); after
		// cancel()/failure it still reports the state left by that attempt so the work done can be inspected.
		bool m_half_block_pending;

		// Total source blocks completed in the working frame's alpha slice (0 when the file has no alpha slices).
		uint32_t m_frame_alpha_blocks_completed;

		// Number of working frames successfully published to front() since open().
		uint32_t m_published_frames;
	};

	// A borrowed, tightly packed RGBA32 view of the most recently published frame.
	//
	// Ownership: The pixel bytes and the frame number are owned by the player. The view stays valid until the next
	// successful frame publish or until close()/destruction; do not free or retain it past that point. Before the first
	// successful publish, front() returns a view with m_frame_index = 0, m_pixels == nullptr and zero dimensions.
	struct texture_job_frame
	{
		uint32_t m_frame_index;		// Image index of the published frame.
		const uint8_t* m_pixels;	// Row-major RGBA32 pixels, m_width * m_height * 4 bytes, rows contiguous.
		uint32_t m_width;			// Published frame width in pixels.
		uint32_t m_height;			// Published frame height in pixels.

		texture_job_frame() :
			m_frame_index(0), m_pixels(nullptr), m_width(0), m_height(0)
		{
		}
	};

	// Pausable, cancellable ETC1S .basis video texture player.
	//
	// Scope and supported input:
	// - Only .basis containers whose texture type is cBASISTexTypeVideoFrames and whose texture format is ETC1S.
	// - Only mip level 0 is supported; files with additional mip levels are rejected by open().
	// - Output is always CPU RGBA32 (cTFRGBA32 order: R,G,B,A bytes).
	// - Files without alpha (one slice per image) and files with independent alpha slices (two slices per image,
	//   RGB then alpha) are both supported. Without alpha, A is set to 255.
	//
	// Decoding is resumable at source-block granularity: step(budget) decodes at most `budget` source 4x4 blocks
	// (an alpha+RGB pair counts as two), in spatial raster order, decoding alpha before RGB at each position.
	// Entropy state (bit cursor, selector history/RLE, endpoint prediction and the two prediction rows) persists
	// across step() calls, so a budget of 1 can stop between the alpha and RGB halves of a dual-slice block.
	//
	// Pixel output, edge clipping and per-block color expansion match calling the existing whole-frame
	// transcode_image_level(..., cTFRGBA32, ...) on the same file, frame by frame, in order.
	//
	// Typical use:
	//   basisu_transcoder_init();
	//   etc1s_texture_player player;
	//   if (!player.open(data, size)) { ... }
	//   while (player.begin_frame(frame_index) == texture_job_result::kTextureJobFrameStarted) {
	//       for (;;) {
	//           texture_job_result r = player.step(256);
	//           if (r == texture_job_result::kTextureJobFrameComplete) break;
	//           if (r != texture_job_result::kTextureJobRunning) { ...decode error... }
	//       }
	//       const texture_job_frame& f = player.front();
	//       // use f.m_pixels while it remains borrowed
	//   }
	//   player.close();
	//
	// Ordering rules:
	// - open() may be called repeatedly. A successful open() replaces the file and clears all playback state
	//   (published frame, references, progress). A failed open() leaves the previous instance state intact.
	// - begin_frame(image_index) may start only image 0 the first time, and afterwards only the image immediately
	//   following the last successfully published frame. Any index that skips forward, goes backward, repeats an
	//   already-published frame, or is invalid is rejected with kTextureJobInvalidFrame and changes no state.
	//   Calling begin_frame() while a working frame exists is rejected with kTextureJobBusy and changes no state.
	// - After cancel() or a decode failure, begin_frame() on the same image index is allowed again: the frame is
	//   restarted from its beginning, never silently skipped. Cancelling while idle is a no-op.
	// - close() is idempotent.
	//
	// Instances do not share mutable decode state with each other; each owns a copy of the compressed file and its
	// own codebooks. Concurrent use of one instance is not supported.
	//
	// Memory: the player retains only its copy of the compressed file, the decoded codebooks, two RGBA32 frames
	// (published + working), the committed and working previous-frame index planes for RGB and alpha, the two
	// endpoint prediction rows, and the per-slice entropy state. No whole-level expansion or frame cache is kept.
	class etc1s_texture_player
	{
	public:
		etc1s_texture_player();
		~etc1s_texture_player();

		etc1s_texture_player(const etc1s_texture_player&) = delete;
		etc1s_texture_player& operator= (const etc1s_texture_player&) = delete;

		// Validates and takes ownership of a copy of the complete compressed .basis file. On success, resets all
		// playback state (as if freshly constructed) and returns true. On failure, the previous state is untouched
		// and false is returned. Checks the container, unpacks the ETC1S codebooks, and verifies VideoFrames type,
		// mip 0 only, and consistent frame dimensions.
		bool open(const void* pData, uint32_t data_size);

		// Starts a working frame for image_index. Returns kTextureJobFrameStarted on success. Otherwise returns
		// kTextureJobNotOpen, kTextureJobBusy (a working frame already exists), or kTextureJobInvalidFrame (the
		// index isn't the allowed next frame), leaving the player unchanged.
		texture_job_result begin_frame(uint32_t image_index);

		// Decodes up to budget source blocks of the working frame. Zero budget does not advance. Returns
		// kTextureJobRunning if more work remains, kTextureJobFrameComplete once the frame has been published,
		// kTextureJobIdle if there is no working frame (e.g. already complete, or never begun), and
		// kTextureJobDecodeError if the compressed stream was rejected. After a complete frame, repeated step()
		// calls return kTextureJobFrameComplete without consuming anything until the next begin_frame().
		texture_job_result step(uint32_t budget);

		// Abandons the working frame, keeping the published texture and both committed reference planes. Safe to
		// call while idle. After cancel(), the same frame index may be begun again.
		void cancel();

		// Returns the most recently published frame (empty/borrowed view if none). See texture_job_frame ownership.
		const texture_job_frame& front() const;

		// Returns progress counters for the current/last working frame and cumulative totals. Counters reflect
		// actual per-block execution; begin_frame() clears the per-frame counters, cancel()/failure retain the work
		// done by that frame until the next begin_frame().
		const texture_job_progress& progress() const;

		// Releases the file, codebooks, published frame and all decode state. Idempotent.
		void close();

	private:
		struct impl;
		impl* m_pImpl;
	};
}
