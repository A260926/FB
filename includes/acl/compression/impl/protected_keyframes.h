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
#include "acl/core/bitset.h"
#include "acl/core/error_result.h"
#include "acl/core/iallocator.h"
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
		// Validates the borrowed protected keyframe list against the original
		// number of samples per track. The indices use the input track list sample
		// numbering, before loop optimization.
		inline error_result validate_protected_keyframes(
			const compression_keyframe_stripping_settings& stripping_settings,
			uint32_t num_input_samples)
		{
			return stripping_settings.is_valid(num_input_samples);
		}

		//////////////////////////////////////////////////////////////////////////
		// Copies the borrowed protected keyframe list into clip owned memory and
		// maps the indices into the sample space that remains after loop
		// optimization. When loop optimization removes the last sample because it
		// repeats the first, the original last index maps to sample 0. No other
		// index moves. Two indices that coincide after the mapping are legal, the
		// output list keeps one entry. The output list stays sorted and contains
		// no duplicate.
		// The caller frees the output array with deallocate_type_array.
		inline error_result resolve_protected_keyframes(
			iallocator& allocator,
			const compression_keyframe_stripping_settings& stripping_settings,
			uint32_t num_input_samples,
			bool loop_wrapped,
			uint32_t*& out_indices,
			uint32_t& out_num_indices)
		{
			out_indices = nullptr;
			out_num_indices = 0;

			const uint32_t num_protected = stripping_settings.num_protected_keyframes;
			if (num_protected == 0)
				return error_result();	// An empty list needs no storage

			const error_result validation_result = validate_protected_keyframes(stripping_settings, num_input_samples);
			if (validation_result.any())
				return validation_result;

			// Map every index first and count the unique entries. The mapping can
			// only turn the original last index into 0, so the mapped sequence stays
			// sorted and the only possible duplicate is a leading pair of zeroes.
			uint32_t num_unique = 0;
			uint32_t previous_index = 0xFFFFFFFF;
			for (uint32_t index = 0; index < num_protected; ++index)
			{
				uint32_t keyframe_index = stripping_settings.protected_keyframes[index];

				// The removed repeating last sample lives on through the retained first sample.
				if (loop_wrapped && keyframe_index == (num_input_samples - 1))
					keyframe_index = 0;

				if (index == 0 || keyframe_index != previous_index)
					num_unique++;

				previous_index = keyframe_index;
			}

			uint32_t* indices = allocate_type_array<uint32_t>(allocator, num_unique);

			uint32_t output_index = 0;
			previous_index = 0xFFFFFFFF;
			for (uint32_t index = 0; index < num_protected; ++index)
			{
				uint32_t keyframe_index = stripping_settings.protected_keyframes[index];

				// The removed repeating last sample lives on through the retained first sample.
				if (loop_wrapped && keyframe_index == (num_input_samples - 1))
					keyframe_index = 0;

				if (index != 0 && keyframe_index == previous_index)
					continue;	// Two inputs map to the same retained frame, keep one entry

				indices[output_index++] = keyframe_index;
				previous_index = keyframe_index;
			}

			ACL_ASSERT(output_index == num_unique, "Unexpected number of unique protected keyframes");

			out_indices = indices;
			out_num_indices = num_unique;
			return error_result();
		}

		//////////////////////////////////////////////////////////////////////////
		// Builds the protected keyframe bit mask for one segment in its local
		// sample space. Set bits identify frames that keyframe stripping must keep.
		inline uint32_t build_protected_segment_mask(const segment_context& segment, const clip_context& clip)
		{
			const bitset_description desc = bitset_description::make_from_num_bits<32>();
			uint32_t protected_mask = 0;

			for (uint32_t index = 0; index < clip.num_protected_keyframes; ++index)
			{
				const uint32_t clip_sample_index = clip.protected_keyframes[index];
				if (clip_sample_index < segment.clip_sample_offset)
					continue;	// This frame belongs to an earlier segment

				const uint32_t segment_sample_index = clip_sample_index - segment.clip_sample_offset;
				if (segment_sample_index >= segment.num_samples)
					continue;	// This frame belongs to a later segment

				bitset_set(&protected_mask, desc, segment_sample_index, true);
			}

			return protected_mask;
		}

		//////////////////////////////////////////////////////////////////////////
		// Counts the protected frames that fall inside the movable region of the
		// clip. Segment boundary frames are not movable on their own and the
		// movable count already excludes them.
		inline uint32_t count_protected_movable_keyframes(const clip_context& clip)
		{
			uint32_t num_protected_movable = 0;

			for (uint32_t index = 0; index < clip.num_protected_keyframes; ++index)
			{
				const uint32_t clip_sample_index = clip.protected_keyframes[index];

				for (const segment_context& segment : clip.segment_iterator())
				{
					const bool is_segment_boundary =
						clip_sample_index == segment.clip_sample_offset ||
						clip_sample_index == (segment.clip_sample_offset + segment.num_samples - 1);

					if (!is_segment_boundary &&
						clip_sample_index >= segment.clip_sample_offset &&
						clip_sample_index < (segment.clip_sample_offset + segment.num_samples))
					{
						num_protected_movable++;
						break;
					}
				}
			}

			return num_protected_movable;
		}
	}

	ACL_IMPL_VERSION_NAMESPACE_END
}

ACL_IMPL_FILE_PRAGMA_POP
