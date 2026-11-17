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

// Thread-local active-lane domain for the scalar CPU backends
// (USE_PACKED_SCALAR / USE_SCALAR) and the shader::masked() scope helper.
//
// The active domain is held per thread in two shapes: a simdmask gates float
// writes (simdfloat, vec2/3/4) and a simdimask gates integer writes
// (simdint, ivec2/3/4). Both always describe the same per-lane booleans, so
// entering shader::masked() intersects and restores both of them.

#include "simd_definitions.h"

#include <utility>

namespace shader
{
	namespace detail
	{
		// The outermost domain is every lane active. thread_local keeps the
		// domains of threads processing different image rows independent, and
		// the inline accessor gives every translation unit the same storage.
		inline simdmask& active_float_mask()
		{
			thread_local simdmask current = simdmask(1);
			return current;
		}

		inline simdimask& active_int_mask()
		{
			thread_local simdimask current = simdimask(1);
			return current;
		}

#if defined(USE_PACKED_SCALAR) || defined(USE_SCALAR)

		inline simdmask intersect_masks(const simdmask& _a, const simdmask& _b)
		{
			simdmask result;
			for (int32_t i = 0; i < SIMDWIDTH; ++i)
				result.value[i] = (_a.value[i] != 0 && _b.value[i] != 0) ? 1 : 0;
			return result;
		}

		inline simdimask intersect_masks(const simdimask& _a, const simdimask& _b)
		{
			simdimask result;
			for (int32_t i = 0; i < SIMDWIDTH; ++i)
				result.value[i] = (_a.value[i] != 0 && _b.value[i] != 0) ? 1 : 0;
			return result;
		}

		inline simdimask mask_as_int(const simdmask& _mask)
		{
			simdimask result;
			for (int32_t i = 0; i < SIMDWIDTH; ++i)
				result.value[i] = _mask.value[i] != 0 ? 1 : 0;
			return result;
		}

		inline simdmask mask_as_float(const simdimask& _mask)
		{
			simdmask result;
			for (int32_t i = 0; i < SIMDWIDTH; ++i)
				result.value[i] = _mask.value[i] != 0 ? 1 : 0;
			return result;
		}

#elif defined(USE_AVX512)

		inline simdmask intersect_masks(const simdmask& _a, const simdmask& _b)
		{
			return simdmask(static_cast<int32_t>(_a.value & _b.value));
		}

		inline simdimask intersect_masks(const simdimask& _a, const simdimask& _b)
		{
			return simdimask(static_cast<int32_t>(_a.value & _b.value));
		}

		inline simdimask mask_as_int(const simdmask& _mask) { return simdimask(static_cast<int32_t>(_mask.value)); }
		inline simdmask mask_as_float(const simdimask& _mask) { return simdmask(static_cast<int32_t>(_mask.value)); }

#elif defined(USE_NEON)

		inline simdmask intersect_masks(const simdmask& _a, const simdmask& _b)
		{
			return simdmask(simd_and_float(_a, _b));
		}

		inline simdimask intersect_masks(const simdimask& _a, const simdimask& _b)
		{
			return simdimask(simd_and_int(_a, _b));
		}

		inline simdimask mask_as_int(const simdmask& _mask) { return simdimask(_mask.value); }
		inline simdmask mask_as_float(const simdimask& _mask) { return simdmask(_mask.value); }

#else

		// SSE / AVX2: float and integer masks live in different register files.
		inline simdmask intersect_masks(const simdmask& _a, const simdmask& _b)
		{
			return simdmask(simd_and_float(_a, _b));
		}

		inline simdimask intersect_masks(const simdimask& _a, const simdimask& _b)
		{
			return simdimask(simd_and_int(_a, _b));
		}

		inline simdimask mask_as_int(const simdmask& _mask)
		{
#ifdef USE_AVX2
			return simdimask(_mm256_castps_si256(_mask.value));
#else
			return simdimask(_mm_castps_si128(_mask.value));
#endif
		}

		inline simdmask mask_as_float(const simdimask& _mask)
		{
#ifdef USE_AVX2
			return simdmask(_mm256_castsi256_ps(_mask.value));
#else
			return simdmask(_mm_castsi128_ps(_mask.value));
#endif
		}

#endif

		// Vector construction always produces a complete value: while a
		// constructor body runs, every lane is treated as active, regardless
		// of the surrounding masked() scope. The enclosing domain is restored
		// when the constructor returns or throws.
		class construction_full_scope
		{
		public:
			construction_full_scope()
				: m_savedFloat(active_float_mask()), m_savedInt(active_int_mask())
			{
				active_float_mask() = simdmask(1);
				active_int_mask() = simdimask(1);
			}

			~construction_full_scope()
			{
				active_float_mask() = m_savedFloat;
				active_int_mask() = m_savedInt;
			}

			construction_full_scope(const construction_full_scope&) = delete;
			construction_full_scope& operator=(const construction_full_scope&) = delete;

		private:
			simdmask m_savedFloat;
			simdimask m_savedInt;
		};

		// RAII scope: intersects on entry, restores on exit (also when the body
		// throws). Both domains are always saved so either mask argument type
		// drives float and integer writes alike.
		class mask_scope
		{
		public:
			explicit mask_scope(const simdmask& _mask)
				: m_savedFloat(active_float_mask()), m_savedInt(active_int_mask())
			{
				active_float_mask() = intersect_masks(active_float_mask(), _mask);
				active_int_mask() = intersect_masks(active_int_mask(), mask_as_int(_mask));
			}

			explicit mask_scope(const simdimask& _mask)
				: m_savedFloat(active_float_mask()), m_savedInt(active_int_mask())
			{
				active_int_mask() = intersect_masks(active_int_mask(), _mask);
				active_float_mask() = intersect_masks(active_float_mask(), mask_as_float(_mask));
			}

			~mask_scope()
			{
				active_float_mask() = m_savedFloat;
				active_int_mask() = m_savedInt;
			}

			mask_scope(const mask_scope&) = delete;
			mask_scope& operator=(const mask_scope&) = delete;

		private:
			simdmask m_savedFloat;
			simdimask m_savedInt;
		};
	}

	// Run _body once with the active domain intersected with _mask. The host
	// always calls _body exactly once, even when every lane is inactive. The
	// previous domain is restored when the scope ends, and exceptions propagate
	// to the caller after the restore.
	template<typename Callable>
	inline void masked(const simdmask& _mask, Callable&& _body)
	{
		detail::mask_scope scope(_mask);
		std::forward<Callable>(_body)();
	}

	template<typename Callable>
	inline void masked(const simdimask& _mask, Callable&& _body)
	{
		detail::mask_scope scope(_mask);
		std::forward<Callable>(_body)();
	}
}

#if defined(USE_PACKED_SCALAR) || defined(USE_SCALAR)

namespace shader
{
	namespace detail
	{
		// Narrow, befriended accessors used by the masked stores below. The
		// public value[] array keeps its original full-width semantics for
		// direct access and simd_load/simd_store.
		class simdfloat_lane_writer
		{
		public:
			static void set(simdfloat& _dst, int32_t _lane, float _v) { _dst.value[_lane] = _v; }
		};

		class simdint_lane_writer
		{
		public:
			static void set(simdint& _dst, int32_t _lane, int32_t _v) { _dst.value[_lane] = _v; }
		};
	}
}

inline void masked_assign_f(simdfloat& _dst, const float& _val)
{
	const simdmask& active = shader::detail::active_float_mask();
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
		if (active.value[i] != 0)
			shader::detail::simdfloat_lane_writer::set(_dst, i, _val);
}

inline void masked_assign_f(simdfloat& _dst, const int32_t& _val)
{
	masked_assign_f(_dst, static_cast<float>(_val));
}

inline void masked_assign_f(simdfloat& _dst, const double& _val)
{
	masked_assign_f(_dst, static_cast<float>(_val));
}

inline void masked_assign_f(simdfloat& _dst, const simdfloat& _val)
{
	const simdmask& active = shader::detail::active_float_mask();
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
		if (active.value[i] != 0)
			shader::detail::simdfloat_lane_writer::set(_dst, i, _val.value[i]);
}

inline void masked_assign_f(simdfloat& _dst, const simdint& _val)
{
	const simdmask& active = shader::detail::active_float_mask();
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
		if (active.value[i] != 0)
			shader::detail::simdfloat_lane_writer::set(_dst, i, static_cast<float>(_val.value[i]));
}

inline void masked_assign_i(simdint& _dst, const int32_t& _val)
{
	const simdimask& active = shader::detail::active_int_mask();
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
		if (active.value[i] != 0)
			shader::detail::simdint_lane_writer::set(_dst, i, _val);
}

inline void masked_assign_i(simdint& _dst, const simdint& _val)
{
	const simdimask& active = shader::detail::active_int_mask();
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
		if (active.value[i] != 0)
			shader::detail::simdint_lane_writer::set(_dst, i, _val.value[i]);
}

inline void masked_assign_i(simdint& _dst, const simdfloat& _val)
{
	const simdimask& active = shader::detail::active_int_mask();
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
		if (active.value[i] != 0)
			shader::detail::simdint_lane_writer::set(_dst, i, static_cast<int32_t>(_val.value[i]));
}

#endif // USE_PACKED_SCALAR || USE_SCALAR

#endif // SIMD_MASKED_H
