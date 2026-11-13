#pragma once

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

#include "acl/version.h"
#include "acl/core/bitset.h"
#include "acl/core/error_result.h"
#include "acl/core/impl/compiler_utils.h"
#include "acl/compression/compression_settings.h"
#include "acl/compression/impl/clip_context.h"
#include "acl/compression/impl/segment_context.h"

#include <cstdint>

ACL_IMPL_FILE_PRAGMA_PUSH

namespace acl
{
	ACL_IMPL_VERSION_NAMESPACE_BEGIN

	namespace acl_impl
	{
		//////////////////////////////////////////////////////////////////////////
		// Validates the protected keyframe list against the original number of
		// samples per input track. The list must be strictly increasing, it must
		// not contain duplicate entries, and every entry must be smaller than the
		// number of samples. An empty list is always valid.
		//////////////////////////////////////////////////////////////////////////
		inline error_result validate_protected_keyframes(
			const uint32_t* protected_keyframes,
			uint32_t num_protected_keyframes,
			uint32_t num_samples_per_track)
		{
			if (num_protected_keyframes == 0)
				return error_result();	// An empty list disables the feature

			if (protected_keyframes == nullptr)
				return error_result("protected_keyframes cannot be NULL when num_protected_keyframes is not zero");

			for (uint32_t index = 0; index < num_protected_keyframes; ++index)
			{
				const uint32_t keyframe_index = protected_keyframes[index];

				if (keyframe_index >= num_samples_per_track)
					return error_result("protected_keyframes entries must be smaller than the number of samples per track");

				if (index != 0 && keyframe_index <= protected_keyframes[index - 1])
					return error_result("protected_keyframes must be strictly increasing and contain no duplicate entries");
			}

			return error_result();
		}

		//////////////////////////////////////////////////////////////////////////
		// Remaps the protected list after loop optimization removes the last
		// sample. A protected entry that points at the removed last sample maps
		// to the retained frame 0. Every other entry keeps its index. Two entries
		// may end up pointing at the same frame, this is legal.
		// The input list uses original input sample indices and the output list
		// uses clip relative sample indices after loop optimization.
		//////////////////////////////////////////////////////////////////////////
		inline void remap_protected_keyframes_for_loop(
			const uint32_t* protected_keyframes,
			uint32_t num_protected_keyframes,
			uint32_t num_input_samples,
			bool did_remove_last_sample,
			uint32_t* out_mapped_keyframes)
		{
			for (uint32_t index = 0; index < num_protected_keyframes; ++index)
			{
				uint32_t keyframe_index = protected_keyframes[index];

				if (did_remove_last_sample && keyframe_index == (num_input_samples - 1))
					keyframe_index = 0;	// The removed last sample repeats the retained first sample

				out_mapped_keyframes[index] = keyframe_index;
			}
		}

		//////////////////////////////////////////////////////////////////////////
		// Builds the bit set of protected keyframes for a segment, in segment
		// relative sample index space. The clip context list uses clip relative
		// indices after loop optimization. Entries that fall outside the segment
		// do not set any bit. Duplicate mapped entries simply set the same bit.
		// The result uses the ACL bitset convention (bit N maps to position 31-N).
		//////////////////////////////////////////////////////////////////////////
		inline uint32_t build_segment_protected_keyframe_mask(const clip_context& clip, const segment_context& segment)
		{
			const bitset_description desc = bitset_description::make_from_num_bits<32>();
			uint32_t protected_mask = 0;

			for (uint32_t index = 0; index < clip.num_protected_keyframes; ++index)
			{
				const uint32_t clip_sample_index = clip.protected_keyframes[index];

				if (clip_sample_index >= segment.clip_sample_offset &&
					clip_sample_index < (segment.clip_sample_offset + segment.num_samples))
				{
					const uint32_t segment_sample_index = clip_sample_index - segment.clip_sample_offset;
					bitset_set(&protected_mask, desc, segment_sample_index, true);
				}
			}

			return protected_mask;
		}

		//////////////////////////////////////////////////////////////////////////
		// Counts how many protected frames are movable candidates. Segment first
		// and last frames are never movable, they are excluded here as well.
		//////////////////////////////////////////////////////////////////////////
		inline uint32_t count_protected_movable_keyframes(const clip_context& clip)
		{
			const bitset_description desc = bitset_description::make_from_num_bits<32>();
			uint32_t num_protected_movable = 0;

			for (const segment_context& segment : clip.segment_iterator())
			{
				uint32_t protected_mask = build_segment_protected_keyframe_mask(clip, segment);

				if (segment.num_samples >= 2)
				{
					// The first and last sample of every segment cannot be removed
					uint32_t boundary_mask = 0;
					bitset_set(&boundary_mask, desc, 0, true);
					bitset_set(&boundary_mask, desc, segment.num_samples - 1, true);
					protected_mask &= ~boundary_mask;
				}
				else
				{
					// A single sample segment has no movable frame
					protected_mask = 0;
				}

				num_protected_movable += bitset_count_set_bits(&protected_mask, desc);
			}

			return num_protected_movable;
		}
	}

	ACL_IMPL_VERSION_NAMESPACE_END
}

ACL_IMPL_FILE_PRAGMA_POP
