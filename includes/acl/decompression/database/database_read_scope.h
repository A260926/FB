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

// Public RAII read scope used to pin a database residency snapshot while
// decompressing. See docs/database_support.md for the threading protocol.

#include "acl/version.h"
#include "acl/core/impl/compiler_utils.h"
#include "acl/decompression/database/impl/database_read_state.h"

#include <cstdint>

ACL_IMPL_FILE_PRAGMA_PUSH

namespace acl
{
	ACL_IMPL_VERSION_NAMESPACE_BEGIN

	//////////////////////////////////////////////////////////////////////////
	// RAII handle returned by database_context::try_acquire_read().
	//
	// While held, the database residency snapshot visible at acquisition time
	// remains valid: streamed in publications are delayed, streamed out evictions
	// are deferred, and the bulk data backing the resident chunks stays alive.
	// The handle is movable but not copyable. A default constructed or moved-from
	// scope is invalid.
	//////////////////////////////////////////////////////////////////////////
	class database_read_scope
	{
	public:
		//////////////////////////////////////////////////////////////////////////
		// Constructs an invalid scope.
		database_read_scope() noexcept
			: m_state(nullptr)
		{
		}

		//////////////////////////////////////////////////////////////////////////
		// Releases the read if still held.
		~database_read_scope()
		{
			release();
		}

		database_read_scope(database_read_scope&& other) noexcept
			: m_state(other.m_state)
		{
			other.m_state = nullptr;
		}

		database_read_scope& operator=(database_read_scope&& other) noexcept
		{
			if (this != &other)
			{
				release();
				m_state = other.m_state;
				other.m_state = nullptr;
			}
			return *this;
		}

		database_read_scope(const database_read_scope&) = delete;
		database_read_scope& operator=(const database_read_scope&) = delete;

		//////////////////////////////////////////////////////////////////////////
		// Returns true if this scope successfully acquired a read.
		bool is_valid() const noexcept { return m_state != nullptr; }
		explicit operator bool() const noexcept { return is_valid(); }

		//////////////////////////////////////////////////////////////////////////
		// Releases the read ahead of destruction. Releasing an invalid scope is a no-op.
		// When the last scope on a database is released, every held streaming operation
		// is committed before this call returns.
		void release();

	private:
		template<class database_settings_type>
		friend class database_context;

		explicit database_read_scope(acl_impl::database_read_state* state) noexcept
			: m_state(state)
		{
		}

		acl_impl::database_read_state* m_state;
	};

	ACL_IMPL_VERSION_NAMESPACE_END
}

ACL_IMPL_FILE_PRAGMA_POP
