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

// Lightweight streaming request types shared by the public streamer header and
// the internal read-scope bookkeeping, without pulling in the full streamer
// implementation and its include graph.

#include "acl/version.h"
#include "acl/core/quality_tiers.h"
#include "acl/core/impl/compiler_utils.h"

#include <cstdint>

ACL_IMPL_FILE_PRAGMA_PUSH

namespace acl
{
	ACL_IMPL_VERSION_NAMESPACE_BEGIN

	//////////////////////////////////////////////////////////////////////////
	// Enum representing a streaming action
	//////////////////////////////////////////////////////////////////////////
	enum class streaming_action
	{
		stream_in,
		stream_out,
	};

	//////////////////////////////////////////////////////////////////////////
	// A streaming request
	//////////////////////////////////////////////////////////////////////////
	struct streaming_request
	{
		streaming_action		action = streaming_action::stream_in;
		quality_tier			tier = quality_tier::highest_importance;	// Marks the request as invalid since we never stream the highest tier
		uint32_t				first_chunk_index = 0;
		uint32_t				num_streaming_chunks = 0;
		uint32_t				generation_id = 0;

		bool is_valid() const { return tier != quality_tier::highest_importance; }
		void reset() { tier = quality_tier::highest_importance; }
	};

	//////////////////////////////////////////////////////////////////////////
	// A streaming request ID
	//
	// The internal value is an implementation detail.
	//////////////////////////////////////////////////////////////////////////
	struct streaming_request_id
	{
		// Opaque value
		uint64_t value;

		bool is_valid() const { return value != ~0ULL; }
	};

	// An invalid streaming request ID
	constexpr streaming_request_id k_invalid_streamer_request_id = { ~0ULL };

	ACL_IMPL_VERSION_NAMESPACE_END
}

ACL_IMPL_FILE_PRAGMA_POP
