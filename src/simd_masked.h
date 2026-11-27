/*
 * MIT License
 *
 * Copyright 2026 Pierre GEISSLER
 *
 * Permission is hereby granted, free of charge, to any person obtaining _a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to USE, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR _a PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef SIMD_MASKED_H
#define SIMD_MASKED_H

// Lane activity domain for the scalar CPU backends (USE_PACKED_SCALAR / USE_SCALAR).
// shader::masked(mask, body) intersects the current thread's active domain with
// mask while body runs, and restores the previous domain afterwards. Writes to
// existing simdfloat/simdint (and vec/ivec) objects only touch active lanes;
// construction, reads and arithmetic keep producing full-width values.
#if defined(USE_PACKED_SCALAR) || defined(USE_SCALAR)

namespace shader
{
	namespace detail
	{
		template<typename Mask>
		inline Mask make_all_mask()
		{
			Mask result;
			for (int32_t i = 0; i < SIMDWIDTH; ++i) result.value[i] = 1;
			return result;
		}

		inline simdmask to_float_mask(const simdmask& _mask) { return _mask; }
		inline simdmask to_float_mask(const simdimask& _mask)
		{
			simdmask result;
			for (int32_t i = 0; i < SIMDWIDTH; ++i) result.value[i] = _mask.value[i];
			return result;
		}
		inline simdimask to_int_mask(const simdimask& _mask) { return _mask; }
		inline simdimask to_int_mask(const simdmask& _mask)
		{
			simdimask result;
			for (int32_t i = 0; i < SIMDWIDTH; ++i) result.value[i] = _mask.value[i];
			return result;
		}

		template<typename Mask>
		inline Mask intersect_masks(const Mask& _a, const Mask& _b)
		{
			Mask result;
			for (int32_t i = 0; i < SIMDWIDTH; ++i) result.value[i] = _a.value[i] && _b.value[i];
			return result;
		}

		// Per-thread active domains. The outermost scope is fully active, and
		// threads processing different image rows never share this state.
		inline simdmask& active_float_mask()
		{
			thread_local simdmask current = make_all_mask<simdmask>();
			return current;
		}
		inline simdimask& active_int_mask()
		{
			thread_local simdimask current = make_all_mask<simdimask>();
			return current;
		}

		// Full-width store used by constructors: building a value never depends
		// on the active domain, so fresh objects always hold complete data.
		inline void full_store(simdfloat& _dst, const simdfloat& _src)
		{
			for (int32_t i = 0; i < SIMDWIDTH; ++i) _dst.value[i] = _src.value[i];
		}
		inline void full_store(simdint& _dst, const simdint& _src)
		{
			for (int32_t i = 0; i < SIMDWIDTH; ++i) _dst.value[i] = _src.value[i];
		}

		// Masked store used by assignments to existing objects: inactive lanes
		// keep the value they already held.
		inline void masked_store(simdfloat& _dst, const simdfloat& _src)
		{
			const simdmask& active = active_float_mask();
			for (int32_t i = 0; i < SIMDWIDTH; ++i)
				if (active.value[i]) _dst.value[i] = _src.value[i];
		}
		inline void masked_store(simdint& _dst, const simdint& _src)
		{
			const simdimask& active = active_int_mask();
			for (int32_t i = 0; i < SIMDWIDTH; ++i)
				if (active.value[i]) _dst.value[i] = _src.value[i];
		}

		// RAII scope: intersect on entry, restore on leave, even when the body
		// throws (the exception itself still propagates to the caller).
		struct mask_scope
		{
			simdmask _previous_float;
			simdimask _previous_int;

			template<typename Mask>
			explicit mask_scope(const Mask& _mask)
				: _previous_float(active_float_mask()), _previous_int(active_int_mask())
			{
				const simdmask float_mask = intersect_masks(_previous_float, to_float_mask(_mask));
				active_float_mask() = float_mask;
				active_int_mask() = intersect_masks(_previous_int, to_int_mask(float_mask));
			}

			mask_scope(const mask_scope&) = delete;
			mask_scope& operator=(const mask_scope&) = delete;

			~mask_scope()
			{
				active_float_mask() = _previous_float;
				active_int_mask() = _previous_int;
			}
		};
	}

	// Runs _body once on the host with the active domain intersected by _mask.
	// The body is invoked unconditionally, even when every lane is inactive.
	template<typename Body>
	inline void masked(const simdmask& _mask, Body&& _body)
	{
		detail::mask_scope scope(_mask);
		_body();
	}
	template<typename Body>
	inline void masked(const simdimask& _mask, Body&& _body)
	{
		detail::mask_scope scope(_mask);
		_body();
	}
}

#endif // USE_PACKED_SCALAR || USE_SCALAR

#if !defined(USE_PACKED_SCALAR) && !defined(USE_SCALAR)
// Intrinsic backends keep their original unconditional stores: the per-lane
// activity domain is a scalar-backend feature. Vector constructors and swizzles
// still reference these helpers, so provide plain full-width fallbacks.
namespace shader
{
	namespace detail
	{
		inline void full_store(simdfloat& _dst, const simdfloat& _src) { _dst = _src; }
		inline void full_store(simdint& _dst, const simdint& _src) { _dst = _src; }
		inline void masked_store(simdfloat& _dst, const simdfloat& _src) { _dst = _src; }
		inline void masked_store(simdint& _dst, const simdint& _src) { _dst = _src; }
	}
}
#endif

#endif // SIMD_MASKED_H
