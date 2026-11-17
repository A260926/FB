#include "task_support/check.h"

#include "cppshader.h"
#include "task_support/helper.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <thread>
#include <vector>

namespace
{
	// ---------------------------------------------------------------------
	// Lane-array helpers (simd_load/simd_store keep their full-width meaning)
	// ---------------------------------------------------------------------

	simdmask make_float_mask(const int _bits[SIMDWIDTH])
	{
		simdmask mask;
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			mask.value[i] = _bits[i] ? 1 : 0;
		return mask;
	}

	simdimask make_int_mask(const int _bits[SIMDWIDTH])
	{
		simdimask mask;
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			mask.value[i] = _bits[i] ? 1 : 0;
		return mask;
	}

	void load_lanes(const float _src[SIMDWIDTH], simdfloat& _dst) { _dst = simd_load_float(_src); }
	void load_lanes(const int32_t _src[SIMDWIDTH], simdint& _dst) { _dst = simd_load_int(_src); }

	void store_lanes(const simdfloat& _src, float _dst[SIMDWIDTH]) { simd_store_float(_dst, _src); }
	void store_lanes(const simdint& _src, int32_t _dst[SIMDWIDTH]) { simd_store_int(_dst, _src); }
	void store_lanes(const simdimask& _src, int32_t _dst[SIMDWIDTH]) { simd_store_int(_dst, _src); }
	void store_lanes(const simdmask& _src, int32_t _dst[SIMDWIDTH])
	{
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			_dst[i] = _src.value[i];
	}

	bool close(float _a, float _b)
	{
		return std::fabs(_a - _b) <= 1e-5f * (1.0f + std::fabs(_b));
	}

	bool mask_equals(const simdmask& _mask, const int _bits[SIMDWIDTH])
	{
		int32_t got[SIMDWIDTH];
		store_lanes(_mask, got);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			if ((got[i] != 0) != (_bits[i] != 0))
				return false;
		return true;
	}

	bool mask_equals(const simdimask& _mask, const int _bits[SIMDWIDTH])
	{
		int32_t got[SIMDWIDTH];
		store_lanes(_mask, got);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			if ((got[i] != 0) != (_bits[i] != 0))
				return false;
		return true;
	}

	// Pattern lanes: even-indexed lanes active.
	void even_bits(int _bits[SIMDWIDTH])
	{
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			_bits[i] = (i % 2 == 0);
	}

	// Pattern lanes: odd-indexed lanes active.
	void odd_bits(int _bits[SIMDWIDTH])
	{
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			_bits[i] = (i % 2 != 0);
	}

	void none_bits(int _bits[SIMDWIDTH])
	{
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			_bits[i] = 0;
	}

	void all_bits(int _bits[SIMDWIDTH])
	{
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			_bits[i] = 1;
	}

	struct Boom : std::exception
	{
		const char* what() const noexcept override { return "masked boom"; }
	};

	// =====================================================================
	// Group "lanes": active-domain mechanics
	// =====================================================================

	int lanes_intersect_and_restore()
	{
		int outer[SIMDWIDTH], inner[SIMDWIDTH], none[SIMDWIDTH], full[SIMDWIDTH];
		even_bits(outer);
		odd_bits(inner);
		none_bits(none);
		all_bits(full);

		simdfloat value(1.0f);
		simdint counter(1);
		shader::masked(make_float_mask(outer), [&] {
			value = 5.0f;                 // outer-active lanes only
			counter = 9;
			shader::masked(make_int_mask(inner), [&] {
				value = 7.0f;             // outer AND inner: no lane on a 4-wide pack
				counter = 3;
			});
			// Back to the outer domain after the nested scope.
			value += 1.0f;
		});

		float got_f[SIMDWIDTH];
		int32_t got_i[SIMDWIDTH];
		store_lanes(value, got_f);
		store_lanes(counter, got_i);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			if (outer[i])
			{
				TASK_CHECK(close(got_f[i], 6.0f));
				TASK_CHECK(got_i[i] == 9);
			}
			else
			{
				TASK_CHECK(close(got_f[i], 1.0f));
				TASK_CHECK(got_i[i] == 1);
			}
		}

		// The outer scope restored the full domain: later writes hit every lane.
		value = 11.0f;
		counter = 4;
		store_lanes(value, got_f);
		store_lanes(counter, got_i);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			TASK_CHECK(close(got_f[i], 11.0f));
			TASK_CHECK(got_i[i] == 4);
		}
		TASK_CHECK(mask_equals(shader::detail::active_float_mask(), full));
		TASK_CHECK(mask_equals(shader::detail::active_int_mask(), full));
		return 0;
	}

	int lanes_all_false_still_runs_once()
	{
		int none[SIMDWIDTH], full[SIMDWIDTH];
		none_bits(none);
		all_bits(full);

		int host_calls = 0;
		simdfloat value(2.0f);
		simdint counter(2);
		shader::masked(make_float_mask(none), [&] {
			++host_calls;                 // ordinary host side effect, run exactly once
			shader::masked(make_int_mask(none), [&] {
				++host_calls;
				value = 99.0f;
				counter = 99;
			});
			value = 42.0f;
			counter = 42;
		});
		TASK_CHECK(host_calls == 2);

		float got_f[SIMDWIDTH];
		int32_t got_i[SIMDWIDTH];
		store_lanes(value, got_f);
		store_lanes(counter, got_i);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			TASK_CHECK(close(got_f[i], 2.0f));
			TASK_CHECK(got_i[i] == 2);
		}

		// Leaving the all-false scope restores an all-lane domain.
		value = 8.0f;
		store_lanes(value, got_f);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			TASK_CHECK(close(got_f[i], 8.0f));
		TASK_CHECK(mask_equals(shader::detail::active_float_mask(), full));
		return 0;
	}

	int lanes_exception_restores_domain()
	{
		int outer[SIMDWIDTH], inner[SIMDWIDTH], full[SIMDWIDTH];
		even_bits(outer);
		odd_bits(inner);
		all_bits(full);

		bool caught = false;
		simdfloat value(1.0f);
		try
		{
			shader::masked(make_float_mask(outer), [&] {
				shader::masked(make_int_mask(inner), [&] {
					throw Boom();
				});
			});
		}
		catch (const Boom&)
		{
			caught = true;
		}
		TASK_CHECK(caught);

		// Both levels of scope were unwound: the domain is fully active again.
		TASK_CHECK(mask_equals(shader::detail::active_float_mask(), full));
		TASK_CHECK(mask_equals(shader::detail::active_int_mask(), full));
		value = 6.0f;
		float got[SIMDWIDTH];
		store_lanes(value, got);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			TASK_CHECK(close(got[i], 6.0f));
		return 0;
	}

	int lanes_either_mask_type_drives_both_domains()
	{
		int even[SIMDWIDTH];
		even_bits(even);

		// An integer mask gates float stores too.
		simdfloat fvalue(1.0f);
		vec3 color(1.0f);
		shader::masked(make_int_mask(even), [&] {
			fvalue = 3.0f;
			color = vec3(2.0f, 4.0f, 6.0f);
		});
		float got_f[SIMDWIDTH];
		store_lanes(fvalue, got_f);
		float got_c[SIMDWIDTH];
		store_lanes(color.g, got_c);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			TASK_CHECK(close(got_f[i], even[i] ? 3.0f : 1.0f));
			TASK_CHECK(close(got_c[i], even[i] ? 4.0f : 1.0f));
		}

		// A float mask gates integer stores too.
		simdint ivalue(1);
		ivec4 counts(1);
		shader::masked(make_float_mask(even), [&] {
			ivalue = 5;
			counts = ivec4(7, 8, 9, 10);
		});
		int32_t got_i[SIMDWIDTH];
		int32_t got_b[SIMDWIDTH];
		store_lanes(ivalue, got_i);
		store_lanes(counts.b, got_b);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			TASK_CHECK(got_i[i] == (even[i] ? 5 : 1));
			TASK_CHECK(got_b[i] == (even[i] ? 9 : 1));
		}
		return 0;
	}

	struct ThreadResult
	{
		alignas(SIMDWIDTH * 4) float value[SIMDWIDTH];
		alignas(SIMDWIDTH * 4) int32_t counter[SIMDWIDTH];
		alignas(SIMDWIDTH * 4) float uvx[SIMDWIDTH];
		alignas(SIMDWIDTH * 4) float uvy[SIMDWIDTH];
		alignas(SIMDWIDTH * 4) float after[SIMDWIDTH];
	};

	void thread_work(const int _bits[SIMDWIDTH], ThreadResult& _out)
	{
		simdfloat value(1.0f);
		simdint counter(1);
		vec2 uv(0.0f);
		shader::masked(make_float_mask(_bits), [&] {
			value += 10.0f;
			counter += 5;
			shader_step(value, uv);                 // helper living in another TU
		});
		simdfloat later(3.0f);
		later = 7.0f;                               // full domain again inside this thread
		store_lanes(value, _out.value);
		store_lanes(counter, _out.counter);
		store_lanes(uv.x, _out.uvx);
		store_lanes(uv.y, _out.uvy);
		store_lanes(later, _out.after);
	}

	int lanes_two_threads_independent_domains()
	{
		int even[SIMDWIDTH], odd[SIMDWIDTH];
		even_bits(even);
		odd_bits(odd);

		ThreadResult ra, rb;
		std::thread ta(thread_work, even, std::ref(ra));
		std::thread tb(thread_work, odd, std::ref(rb));
		ta.join();
		tb.join();

		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			// Thread A: even lanes ran the body, thread B: odd lanes ran it.
			TASK_CHECK(close(ra.value[i], even[i] ? 12.25f : 1.0f));
			TASK_CHECK(ra.counter[i] == (even[i] ? 6 : 1));
			TASK_CHECK(close(ra.uvx[i], even[i] ? 0.25f : 0.0f));
			TASK_CHECK(close(ra.uvy[i], even[i] ? 0.75f : 0.0f));
			TASK_CHECK(close(rb.value[i], odd[i] ? 12.25f : 1.0f));
			TASK_CHECK(rb.counter[i] == (odd[i] ? 6 : 1));
			TASK_CHECK(close(rb.uvx[i], odd[i] ? 0.25f : 0.0f));
			TASK_CHECK(close(rb.uvy[i], odd[i] ? 0.75f : 0.0f));
			TASK_CHECK(close(ra.after[i], 7.0f));
			TASK_CHECK(close(rb.after[i], 7.0f));
		}
		return 0;
	}

	// =====================================================================
	// Group "values": per-type write semantics
	// =====================================================================

	int values_scalar_assign_compound_incdec()
	{
		int even[SIMDWIDTH];
		even_bits(even);

		alignas(SIMDWIDTH * 4) float seed_f[SIMDWIDTH];
		alignas(SIMDWIDTH * 4) int32_t seed_i[SIMDWIDTH];
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			seed_f[i] = static_cast<float>(i + 1);
			seed_i[i] = i + 1;
		}

		simdfloat fv;
		simdint iv;
		load_lanes(seed_f, fv);
		load_lanes(seed_i, iv);

		shader::masked(make_float_mask(even), [&] {
			fv = 10.0f;                    // plain assignment of a broadcast
			iv = 20;
			fv += 1.0f;                    // compound assignment, scalar RHS
			fv -= simdfloat(2.0f);         // compound assignment, SIMD RHS
			fv *= 3.0f;
			fv /= simdfloat(3.0f);
			iv += 4;
			iv -= simdint(1);
			iv *= 2;
			iv /= 3;
			iv %= 7;
			++fv;
			--iv;
		});

		float got_f[SIMDWIDTH];
		int32_t got_i[SIMDWIDTH];
		store_lanes(fv, got_f);
		store_lanes(iv, got_i);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			if (even[i])
			{
				// 10 +1 -2 = 9; *3 /3 = 9; ++ -> 10
				TASK_CHECK(close(got_f[i], 10.0f));
				// 20 +4 -1 = 23; *2 = 46; /3 = 15; %7 = 1; -- -> 0
				TASK_CHECK(got_i[i] == 0);
			}
			else
			{
				TASK_CHECK(close(got_f[i], seed_f[i]));
				TASK_CHECK(got_i[i] == seed_i[i]);
			}
		}

		// Post-increment/decrement return the pre-write, full-width value.
		simdfloat pv(4.0f);
		simdfloat before_f = pv++;
		simdint pi(4);
		simdint before_i = pi--;
		float bf[SIMDWIDTH];
		int32_t bi[SIMDWIDTH], af[SIMDWIDTH], ai[SIMDWIDTH];
		store_lanes(before_f, bf);
		store_lanes(before_i, bi);
		store_lanes(pv, af);
		store_lanes(pi, ai);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			TASK_CHECK(close(bf[i], 4.0f));
			TASK_CHECK(bi[i] == 4);
			TASK_CHECK(close(af[i], 5.0f));
			TASK_CHECK(ai[i] == 3);
		}

		// Cross-type scalar stores also respect the domain.
		simdfloat cross_f(0.0f);
		simdint cross_i(0);
		shader::masked(make_int_mask(even), [&] {
			cross_f = simdint(6);
			cross_i = simdfloat(7.5f);
		});
		float cf[SIMDWIDTH];
		int32_t ci[SIMDWIDTH];
		store_lanes(cross_f, cf);
		store_lanes(cross_i, ci);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			TASK_CHECK(close(cf[i], even[i] ? 6.0f : 0.0f));
			TASK_CHECK(ci[i] == (even[i] ? 7 : 0));
		}

		// Post-increment/decrement inside a partial mask: the write hits only
		// active lanes while the returned value stays a full-width snapshot.
		alignas(SIMDWIDTH * 4) float post_seed[SIMDWIDTH];
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			post_seed[i] = static_cast<float>(10 + i);
		simdfloat post_v;
		load_lanes(post_seed, post_v);
		simdfloat post_return(-1.0f);
		shader::masked(make_float_mask(even), [&] {
			post_return = post_v++;
		});
		float pv_now[SIMDWIDTH], pv_ret[SIMDWIDTH];
		store_lanes(post_v, pv_now);
		store_lanes(post_return, pv_ret);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			// The write only touches active lanes...
			TASK_CHECK(close(pv_now[i], even[i] ? post_seed[i] + 1.0f : post_seed[i]));
			// ...and assigning the full-width return value back is masked too,
			// so inactive lanes of the destination keep their old value.
			TASK_CHECK(close(pv_ret[i], even[i] ? post_seed[i] : -1.0f));
		}
		return 0;
	}

	int values_vector_components_aliases_subscript()
	{
		int even[SIMDWIDTH], odd[SIMDWIDTH];
		even_bits(even);
		odd_bits(odd);

		// vec3 / vec4: plain assignment, rgba and stpq aliases, subscript.
		vec3 color(0.1f, 0.2f, 0.3f);
		vec4 pixel(1.0f, 2.0f, 3.0f, 4.0f);
		shader::masked(make_float_mask(even), [&] {
			color = vec3(0.5f, 0.6f, 0.7f);
			color.b += 0.25f;                 // alias write, compound
			pixel[0] = 9.0f;                  // subscript write
			pixel.a = 8.0f;                   // rgba alias
			pixel.q += 1.0f;                  // stpq alias of the same component
		});
		float channels[3][SIMDWIDTH];
		store_lanes(color.r, channels[0]);
		store_lanes(color.g, channels[1]);
		store_lanes(color.p, channels[2]);
		float px[4][SIMDWIDTH];
		store_lanes(pixel.x, px[0]);
		store_lanes(pixel.g, px[1]);
		store_lanes(pixel.b, px[2]);
		store_lanes(pixel.a, px[3]);
		const float base_c[3] = { 0.1f, 0.2f, 0.3f };
		const float new_c[3] = { 0.5f, 0.6f, 0.95f };
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			for (int c = 0; c < 3; ++c)
				TASK_CHECK(close(channels[c][i], even[i] ? new_c[c] : base_c[c]));
			TASK_CHECK(close(px[0][i], even[i] ? 9.0f : 1.0f));
			TASK_CHECK(close(px[1][i], even[i] ? 2.0f : 2.0f));
			TASK_CHECK(close(px[2][i], even[i] ? 3.0f : 3.0f));
			TASK_CHECK(close(px[3][i], even[i] ? 9.0f : 4.0f)); // 8 then +1
		}

		// ivec2 / ivec3 / ivec4 with integer masks.
		ivec2 pair(1, 2);
		ivec3 triple(3, 4, 5);
		ivec4 quad(6, 7, 8, 9);
		shader::masked(make_int_mask(odd), [&] {
			pair = ivec2(10, 20);
			triple.g += 100;
			quad[2] = 80;
			quad.s = 60;
		});
		int32_t p[2][SIMDWIDTH], t3[3][SIMDWIDTH], q4[4][SIMDWIDTH];
		store_lanes(pair.x, p[0]);
		store_lanes(pair.t, p[1]);
		store_lanes(triple.x, t3[0]);
		store_lanes(triple.y, t3[1]);
		store_lanes(triple.z, t3[2]);
		store_lanes(quad.r, q4[0]);
		store_lanes(quad.g, q4[1]);
		store_lanes(quad.b, q4[2]);
		store_lanes(quad.a, q4[3]);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			TASK_CHECK(p[0][i] == (odd[i] ? 10 : 1));
			TASK_CHECK(p[1][i] == (odd[i] ? 20 : 2));
			TASK_CHECK(t3[0][i] == (odd[i] ? 3 : 3));
			TASK_CHECK(t3[1][i] == (odd[i] ? 104 : 4));
			TASK_CHECK(t3[2][i] == 5);
			TASK_CHECK(q4[0][i] == (odd[i] ? 60 : 6));
			TASK_CHECK(q4[1][i] == 7);
			TASK_CHECK(q4[2][i] == (odd[i] ? 80 : 8));
			TASK_CHECK(q4[3][i] == 9);
		}

		// Vector compound assignments.
		vec3 va(1.0f), vb(2.0f);
		ivec3 ia(10), ib(2);
		shader::masked(make_float_mask(even), [&] {
			va += vb;
			va *= 0.5f;
			ia += ivec3(1);
			ia /= ib;
		});
		float vout[3][SIMDWIDTH];
		int32_t iout[3][SIMDWIDTH];
		store_lanes(va.x, vout[0]);
		store_lanes(va.y, vout[1]);
		store_lanes(va.z, vout[2]);
		store_lanes(ia.x, iout[0]);
		store_lanes(ia.y, iout[1]);
		store_lanes(ia.z, iout[2]);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			for (int32_t c = 0; c < 3; ++c)
			{
				TASK_CHECK(close(vout[c][i], even[i] ? 1.5f : 1.0f)); // (1+2)*0.5
				TASK_CHECK(iout[c][i] == (even[i] ? 5 : 10));        // (10+1)/2
			}
		return 0;
	}

	int values_overlapping_swizzle_writes()
	{
		int even[SIMDWIDTH];
		even_bits(even);

		// vec2 swap through the vector itself and through component arguments.
		vec2 v2(1.0f, 2.0f);
		shader::masked(make_float_mask(even), [&] {
			v2.yx(v2);
		});
		float got2[2][SIMDWIDTH];
		store_lanes(v2.x, got2[0]);
		store_lanes(v2.y, got2[1]);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			TASK_CHECK(close(got2[0][i], even[i] ? 2.0f : 1.0f));
			TASK_CHECK(close(got2[1][i], even[i] ? 1.0f : 2.0f));
		}

		vec2 w2(3.0f, 6.0f);
		shader::masked(make_float_mask(even), [&] {
			w2.xy(w2.y, w2.x);
		});
		store_lanes(w2.x, got2[0]);
		store_lanes(w2.y, got2[1]);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			TASK_CHECK(close(got2[0][i], even[i] ? 6.0f : 3.0f));
			TASK_CHECK(close(got2[1][i], even[i] ? 3.0f : 6.0f));
		}

		// vec3 reversed permutation with repeated components in the source.
		vec3 v3(1.0f, 2.0f, 3.0f);
		shader::masked(make_float_mask(even), [&] {
			v3.zyx(v3);
		});
		float got3[3][SIMDWIDTH];
		store_lanes(v3.x, got3[0]);
		store_lanes(v3.y, got3[1]);
		store_lanes(v3.z, got3[2]);
		const float base3[3] = { 1.0f, 2.0f, 3.0f };
		const float rev3[3] = { 3.0f, 2.0f, 1.0f };
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			for (int32_t c = 0; c < 3; ++c)
				TASK_CHECK(close(got3[c][i], even[i] ? rev3[c] : base3[c]));

		// vec4 rgba reversal and an integer overlap write.
		vec4 v4(1.0f, 2.0f, 3.0f, 4.0f);
		shader::masked(make_float_mask(even), [&] {
			v4.abgr(v4);
		});
		float got4[4][SIMDWIDTH];
		store_lanes(v4.x, got4[0]);
		store_lanes(v4.y, got4[1]);
		store_lanes(v4.z, got4[2]);
		store_lanes(v4.w, got4[3]);
		const float base4[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
		const float rev4[4] = { 4.0f, 3.0f, 2.0f, 1.0f };
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			for (int32_t c = 0; c < 4; ++c)
				TASK_CHECK(close(got4[c][i], even[i] ? rev4[c] : base4[c]));

		ivec2 iv(7, 9);
		shader::masked(make_int_mask(even), [&] {
			iv.yx(iv);
		});
		int32_t goti[2][SIMDWIDTH];
		store_lanes(iv.x, goti[0]);
		store_lanes(iv.y, goti[1]);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			TASK_CHECK(goti[0][i] == (even[i] ? 9 : 7));
			TASK_CHECK(goti[1][i] == (even[i] ? 7 : 9));
		}

		// Broadcast swizzle setters (one scalar RHS) and multi-argument
		// setters also honor the domain.
		vec4 bcast(1.0f);
		ivec3 bcast_i(2);
		shader::masked(make_float_mask(even), [&] {
			bcast.bg(simdfloat(5.0f));
			bcast.rgba(1.0f, 2.0f, 3.0f, 4.0f);
		});
		shader::masked(make_int_mask(even), [&] {
			bcast_i.xyz(8, 7, 6);
		});
		float bc[4][SIMDWIDTH];
		int32_t bci[3][SIMDWIDTH];
		store_lanes(bcast.r, bc[0]);
		store_lanes(bcast.g, bc[1]);
		store_lanes(bcast.b, bc[2]);
		store_lanes(bcast.a, bc[3]);
		store_lanes(bcast_i.x, bci[0]);
		store_lanes(bcast_i.y, bci[1]);
		store_lanes(bcast_i.z, bci[2]);
		const float bc_new[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
		const int32_t bci_new[3] = { 8, 7, 6 };
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			for (int32_t c = 0; c < 4; ++c)
			{
				TASK_CHECK(close(bc[c][i], even[i] ? bc_new[c] : 1.0f));
				if (c < 3)
					TASK_CHECK(bci[c][i] == (even[i] ? bci_new[c] : 2));
			}
		return 0;
	}

	int values_temporaries_keep_full_width()
	{
		int none[SIMDWIDTH], even[SIMDWIDTH];
		none_bits(none);
		even_bits(even);

		// Temporaries built inside an all-false branch are still complete
		// full-width values; explicit stores keep their full-width behavior,
		// so they can be observed after the scope ends.
		alignas(SIMDWIDTH * 4) float fs[SIMDWIDTH];
		alignas(SIMDWIDTH * 4) float v3s[3][SIMDWIDTH];
		alignas(SIMDWIDTH * 4) int32_t i2s[2][SIMDWIDTH];
		alignas(SIMDWIDTH * 4) int32_t cmp[SIMDWIDTH];
		alignas(SIMDWIDTH * 4) float sw[2][SIMDWIDTH];
		bool temp_full_width = true;
		shader::masked(make_float_mask(none), [&] {
			simdfloat local_f = simdfloat(7.0f) * 2.0f + 1.0f;
			vec3 local_v = vec3(1.0f, 2.0f, 3.0f) + vec3(0.5f);
			ivec2 local_i = ivec2(4, 5) * ivec2(2);
			simdmask local_m = local_f > 0.0f;
			vec2 local_s = local_v.yx();
			simd_store_float(fs, local_f);
			simd_store_float(v3s[0], local_v.r);
			simd_store_float(v3s[1], local_v.g);
			simd_store_float(v3s[2], local_v.b);
			simd_store_int(i2s[0], local_i.x);
			simd_store_int(i2s[1], local_i.y);
			store_lanes(local_m, cmp);
			simd_store_float(sw[0], local_s.x);
			simd_store_float(sw[1], local_s.y);
		});
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			temp_full_width &= close(fs[i], 15.0f);
			temp_full_width &= close(v3s[0][i], 1.5f);
			temp_full_width &= close(v3s[1][i], 2.5f);
			temp_full_width &= close(v3s[2][i], 3.5f);
			temp_full_width &= (i2s[0][i] == 8);
			temp_full_width &= (i2s[1][i] == 10);
			temp_full_width &= (cmp[i] != 0);
			temp_full_width &= close(sw[0][i], 2.5f);
			temp_full_width &= close(sw[1][i], 1.5f);
		}
		TASK_CHECK(temp_full_width);

		// An existing object assigned inside the all-false branch is not
		// touched at all and keeps its previous value.
		simdfloat untouched(4.0f);
		vec3 untouched_v(8.0f);
		shader::masked(make_float_mask(none), [&] {
			untouched = simdfloat(7.0f) * 2.0f + 1.0f;
			untouched_v = vec3(1.0f, 2.0f, 3.0f) + vec3(0.5f);
		});
		float us[SIMDWIDTH];
		float uvs[3][SIMDWIDTH];
		simd_store_float(us, untouched);
		simd_store_float(uvs[0], untouched_v.x);
		simd_store_float(uvs[1], untouched_v.y);
		simd_store_float(uvs[2], untouched_v.z);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			TASK_CHECK(close(us[i], 4.0f));
			TASK_CHECK(close(uvs[0][i], 8.0f));
			TASK_CHECK(close(uvs[1][i], 8.0f));
			TASK_CHECK(close(uvs[2][i], 8.0f));
		}

		// The full-width temporary carried out of the scope is only blended
		// into an existing object according to the new active domain.
		vec3 full_temp(1.5f, 2.5f, 3.5f);
		vec3 target(9.0f);
		shader::masked(make_float_mask(even), [&] {
			target = full_temp;
		});
		float ts[3][SIMDWIDTH];
		store_lanes(target.x, ts[0]);
		store_lanes(target.y, ts[1]);
		store_lanes(target.z, ts[2]);
		const float expected[3] = { 1.5f, 2.5f, 3.5f };
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			for (int32_t c = 0; c < 3; ++c)
				TASK_CHECK(close(ts[c][i], even[i] ? expected[c] : 9.0f));
		return 0;
	}

	int values_copies_own_their_aliases()
	{
		vec4 src(1.0f, 2.0f, 3.0f, 4.0f);
		vec4 dst = src;
		dst.r = 10.0f;
		dst.g = 20.0f;
		dst.b = 30.0f;
		dst.a = 40.0f;
		// stpq must address the same copy, not the source.
		dst.s += 1.0f;
		dst.t += 1.0f;
		dst.p += 1.0f;
		dst.q += 1.0f;
		float src_lanes[4][SIMDWIDTH];
		float dst_lanes[4][SIMDWIDTH];
		store_lanes(src.r, src_lanes[0]);
		store_lanes(src.g, src_lanes[1]);
		store_lanes(src.b, src_lanes[2]);
		store_lanes(src.a, src_lanes[3]);
		store_lanes(dst.x, dst_lanes[0]);
		store_lanes(dst.y, dst_lanes[1]);
		store_lanes(dst.z, dst_lanes[2]);
		store_lanes(dst.w, dst_lanes[3]);
		const float src_init[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
		const float dst_final[4] = { 11.0f, 21.0f, 31.0f, 41.0f };
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			for (int32_t c = 0; c < 4; ++c)
			{
				TASK_CHECK(close(src_lanes[c][i], src_init[c]));
				TASK_CHECK(close(dst_lanes[c][i], dst_final[c]));
			}

		ivec3 isrc(5, 6, 7);
		ivec3 idst = isrc;
		idst.s = 50;
		idst.t = 60;
		idst.p = 70;
		int32_t isrc_lanes[3][SIMDWIDTH];
		int32_t idst_lanes[3][SIMDWIDTH];
		store_lanes(isrc.r, isrc_lanes[0]);
		store_lanes(isrc.g, isrc_lanes[1]);
		store_lanes(isrc.b, isrc_lanes[2]);
		store_lanes(idst.x, idst_lanes[0]);
		store_lanes(idst.y, idst_lanes[1]);
		store_lanes(idst.z, idst_lanes[2]);
		const int32_t isrc_init[3] = { 5, 6, 7 };
		const int32_t idst_final[3] = { 50, 60, 70 };
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			for (int32_t c = 0; c < 3; ++c)
			{
				TASK_CHECK(isrc_lanes[c][i] == isrc_init[c]);
				TASK_CHECK(idst_lanes[c][i] == idst_final[c]);
			}

		// Construction inside a masked region is still full-width, and an
		// alias write performed afterwards remains subject to the domain.
		int even2[SIMDWIDTH];
		even_bits(even2);
		alignas(SIMDWIDTH * 4) float built[4][SIMDWIDTH];
		bool alias_write_ok = false;
		shader::masked(make_float_mask(even2), [&] {
			vec4 fresh(src);
			simd_store_float(built[0], fresh.x);
			simd_store_float(built[1], fresh.y);
			simd_store_float(built[2], fresh.z);
			simd_store_float(built[3], fresh.w);
			fresh.a = 0.0f;
			alias_write_ok = close(fresh.q.value[0], 0.0f); // q aliases a
		});
		TASK_CHECK(alias_write_ok);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			for (int32_t c = 0; c < 4; ++c)
				TASK_CHECK(close(built[c][i], src_init[c]));
		return 0;
	}

	int values_load_store_and_value_array_stay_full_width()
	{
		int none[SIMDWIDTH];
		none_bits(none);

		alignas(SIMDWIDTH * 4) float in[SIMDWIDTH];
		alignas(SIMDWIDTH * 4) float out[SIMDWIDTH];
		alignas(SIMDWIDTH * 4) int32_t out_i[SIMDWIDTH];
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			in[i] = 0.5f + static_cast<float>(i);
			out[i] = -1.0f;
			out_i[i] = -1;
		}

		bool direct_array_write_ok = false;

		shader::masked(make_float_mask(none), [&] {
			// Explicit load/store ignore the active domain by design.
			simdfloat loaded = simd_load_float(in);
			simd_store_float(out, loaded + 1.0f);
			simdint integers = simd_load_int(reinterpret_cast<const int32_t*>(in));
			simd_store_int(out_i, integers);
			// Direct value[] storage access keeps its original behavior.
			loaded.value[0] = 77.0f;
			direct_array_write_ok = close(loaded.value[0], 77.0f);
		});
		TASK_CHECK(direct_array_write_ok);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			TASK_CHECK(close(out[i], in[i] + 1.0f));
			TASK_CHECK(out_i[i] != -1);
		}
		return 0;
	}

	int values_cross_tu_helper_respects_domain()
	{
		int even[SIMDWIDTH];
		even_bits(even);

		simdfloat value(2.0f);
		vec2 uv(1.0f, 2.0f);

		// Outside any scope the helper updates every lane.
		shader_step(value, uv);
		float vf[SIMDWIDTH], ux[SIMDWIDTH], uy[SIMDWIDTH];
		store_lanes(value, vf);
		store_lanes(uv.x, ux);
		store_lanes(uv.y, uy);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			TASK_CHECK(close(vf[i], 3.25f));
			TASK_CHECK(close(ux[i], 1.25f));
			TASK_CHECK(close(uy[i], 2.75f));
		}

		// Inside a scope only active lanes see the helper's writes.
		shader::masked(make_float_mask(even), [&] {
			shader_step(value, uv);
		});
		store_lanes(value, vf);
		store_lanes(uv.x, ux);
		store_lanes(uv.y, uy);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			TASK_CHECK(close(vf[i], even[i] ? 4.5f : 3.25f));
			TASK_CHECK(close(ux[i], even[i] ? 1.5f : 1.25f));
			TASK_CHECK(close(uy[i], even[i] ? 3.5f : 2.75f));
		}
		return 0;
	}

	// =====================================================================
	// Group "shader": material-style per-pixel loop with early termination
	// =====================================================================

	// SIMD version of the material. A lane that reaches the stopping condition
	// writes its final color and iteration count once; later iterations must
	// leave the finished pixel untouched.
	void run_material(const simdfloat& _seed, vec3& _color, simdint& _iterations)
	{
		simdfloat z = _seed;
		simdfloat red(0.0f), green(0.0f), blue(0.0f);
		simdint iteration(0);
		simdint done(0);
		const simdint limit(8);

		for (int step = 0; step < 8; ++step)
		{
			simdimask running = iteration < limit && done == simdint(0);
			shader::masked(running, [&] {
				simdfloat next_z = z * 1.6f + 0.25f;       // full-width temporary
				red += z * 0.1f;
				green += z * z * 0.02f;
				blue += 0.05f;

				simdmask stop = next_z > 3.0f;
				shader::masked(stop, [&] {
					_iterations = iteration;
					_color = vec3(red, green, blue);
					done = simdint(1);
				});

				z = next_z;
				iteration += 1;
			});
		}
	}

	int shader_material_loop_matches_scalar_reference()
	{
		alignas(SIMDWIDTH * 4) float seeds[SIMDWIDTH];
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			seeds[i] = 0.2f + 0.6f * static_cast<float>(i); // 0.2, 0.8, 1.4, 2.0

		vec3 color(9.0f);
		simdint iterations(-1);
		run_material(simd_load_float(seeds), color, iterations);

		float got_c[3][SIMDWIDTH];
		int32_t got_it[SIMDWIDTH];
		store_lanes(color.r, got_c[0]);
		store_lanes(color.g, got_c[1]);
		store_lanes(color.b, got_c[2]);
		store_lanes(iterations, got_it);

		for (int32_t lane = 0; lane < SIMDWIDTH; ++lane)
		{
			// Plain per-pixel scalar reference: a finished pixel keeps its
			// final color and iteration count, like the masked SIMD version.
			float z = seeds[lane];
			float red = 0.0f, green = 0.0f, blue = 0.0f;
			int reference_it = 8;
			bool finished = false;
			for (int step = 0; step < 8; ++step)
			{
				float next_z = z * 1.6f + 0.25f;
				if (!finished)
				{
					red += z * 0.1f;
					green += z * z * 0.02f;
					blue += 0.05f;
					if (next_z > 3.0f)
					{
						reference_it = step;
						finished = true;
					}
				}
				z = next_z;
			}
			TASK_CHECK(finished); // every chosen seed terminates in range
			TASK_CHECK(got_it[lane] == reference_it);
			TASK_CHECK(close(got_c[0][lane], red));
			TASK_CHECK(close(got_c[1][lane], green));
			TASK_CHECK(close(got_c[2][lane], blue));
		}
		return 0;
	}

	// Same material driven from two threads on disjoint coordinate rows, with
	// different entry masks, to mirror parallel image-row processing.
	int shader_material_loop_on_two_threads()
	{
		struct RowResult
		{
			alignas(SIMDWIDTH * 4) float color[3][SIMDWIDTH];
			alignas(SIMDWIDTH * 4) int32_t iterations[SIMDWIDTH];
			alignas(SIMDWIDTH * 4) int32_t final_mask[SIMDWIDTH];
		};

		auto row_work = [](bool _even_row, RowResult& _out) {
			alignas(SIMDWIDTH * 4) float seeds[SIMDWIDTH];
			int entry[SIMDWIDTH];
			for (int32_t i = 0; i < SIMDWIDTH; ++i)
			{
				seeds[i] = 0.2f + 0.6f * static_cast<float>(i);
				entry[i] = _even_row ? (i % 2 == 0) : (i % 2 != 0);
			}
			vec3 color(9.0f);
			simdint iterations(-1);
			simdimask entry_mask = make_int_mask(entry);
			shader::masked(entry_mask, [&] {
				run_material(simd_load_float(seeds), color, iterations);
			});
			store_lanes(color.r, _out.color[0]);
			store_lanes(color.g, _out.color[1]);
			store_lanes(color.b, _out.color[2]);
			store_lanes(iterations, _out.iterations);
			store_lanes(shader::detail::active_int_mask(), _out.final_mask);
		};

		RowResult even_row, odd_row;
		std::thread t1(row_work, true, std::ref(even_row));
		std::thread t2(row_work, false, std::ref(odd_row));
		t1.join();
		t2.join();

		for (int32_t lane = 0; lane < SIMDWIDTH; ++lane)
		{
			float z = 0.2f + 0.6f * static_cast<float>(lane);
			float red = 0.0f, green = 0.0f, blue = 0.0f;
			int reference_it = 8;
			bool finished = false;
			for (int step = 0; step < 8; ++step)
			{
				float next_z = z * 1.6f + 0.25f;
				if (!finished)
				{
					red += z * 0.1f;
					green += z * z * 0.02f;
					blue += 0.05f;
					if (next_z > 3.0f)
					{
						reference_it = step;
						finished = true;
					}
				}
				z = next_z;
			}

			const bool even_active = (lane % 2 == 0);
			const bool odd_active = (lane % 2 != 0);
			TASK_CHECK(even_row.iterations[lane] == (even_active ? reference_it : -1));
			TASK_CHECK(odd_row.iterations[lane] == (odd_active ? reference_it : -1));
			TASK_CHECK(close(even_row.color[0][lane], even_active ? red : 9.0f));
			TASK_CHECK(close(even_row.color[1][lane], even_active ? green : 9.0f));
			TASK_CHECK(close(odd_row.color[2][lane], odd_active ? blue : 9.0f));
			TASK_CHECK(even_row.final_mask[lane] != 0); // scope restored in each thread
			TASK_CHECK(odd_row.final_mask[lane] != 0);
		}
		return 0;
	}
}

const task_case task_cases[] = {
	{ "lanes", "intersect_and_restore", lanes_intersect_and_restore },
	{ "lanes", "all_false_still_runs_once", lanes_all_false_still_runs_once },
	{ "lanes", "exception_restores_domain", lanes_exception_restores_domain },
	{ "lanes", "either_mask_type_drives_both_domains", lanes_either_mask_type_drives_both_domains },
	{ "lanes", "two_threads_independent_domains", lanes_two_threads_independent_domains },

	{ "values", "scalar_assign_compound_incdec", values_scalar_assign_compound_incdec },
	{ "values", "vector_components_aliases_subscript", values_vector_components_aliases_subscript },
	{ "values", "overlapping_swizzle_writes", values_overlapping_swizzle_writes },
	{ "values", "temporaries_keep_full_width", values_temporaries_keep_full_width },
	{ "values", "copies_own_their_aliases", values_copies_own_their_aliases },
	{ "values", "load_store_and_value_array_stay_full_width", values_load_store_and_value_array_stay_full_width },
	{ "values", "cross_tu_helper_respects_domain", values_cross_tu_helper_respects_domain },

	{ "shader", "material_loop_matches_scalar_reference", shader_material_loop_matches_scalar_reference },
	{ "shader", "material_loop_on_two_threads", shader_material_loop_on_two_threads },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
