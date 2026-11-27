#include "cppshader.h"
#include "task_support/helper.h"
#include "task_support/check.h"

#include <cmath>
#include <stdexcept>
#include <thread>

namespace
{
	simdmask make_float_mask(const bool* _flags)
	{
		simdmask mask;
		for (int32_t i = 0; i < SIMDWIDTH; ++i) mask.value[i] = _flags[i] ? 1 : 0;
		return mask;
	}

	simdimask make_int_mask(const bool* _flags)
	{
		simdimask mask;
		for (int32_t i = 0; i < SIMDWIDTH; ++i) mask.value[i] = _flags[i] ? 1 : 0;
		return mask;
	}

	bool same_float_lanes(const simdfloat& _v, const float* _ref, float _eps = 1e-6f)
	{
		float lanes[SIMDWIDTH];
		simd_store_float(lanes, _v);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			if (std::fabs(lanes[i] - _ref[i]) > _eps) return false;
		return true;
	}

	bool same_int_lanes(const simdint& _v, const int32_t* _ref)
	{
		int32_t lanes[SIMDWIDTH];
		simd_store_int(lanes, _v);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			if (lanes[i] != _ref[i]) return false;
		return true;
	}

	// Defined in this translation unit: the active domain must be visible to
	// ordinary helper functions, not only to the masked() call site.
	void bump(simdfloat& _value, vec2& _uv)
	{
		_value += 1.0f;
		_uv += vec2(1.0f, 2.0f);
	}

	// Raymarch-style loop: each lane terminates after its own iteration count.
	// Everything a finished lane has already written must stay untouched.
	void material_loop(const int32_t* _limits, const bool* _active,
	                   vec4& _color, ivec4& _iterations, vec2& _uv)
	{
		simdint limit = simd_load_int(_limits);
		simdint step(0);
		simdimask domain = make_int_mask(_active);
		for (int32_t pass = 0; pass < 8; ++pass)
		{
			simdimask running = step < limit;
			shader::masked(running, [&] {
				shader::masked(domain, [&] {
					_color += vec4(0.1f, 0.2f, 0.3f, 0.4f);
					_iterations += 1;
					++step;
					shader_step(_color.x, _uv);
				});
			});
		}
	}
}

// ---- lanes group: domain mechanics ----------------------------------------

static int case_lanes_mixed()
{
	bool flags[4] = { true, false, true, false };
	simdmask mask = make_float_mask(flags);

	simdfloat scalar(1.0f);
	simdint integer(10);
	vec3 vector(1.0f, 2.0f, 3.0f);
	ivec3 counters(0);

	shader::masked(mask, [&] {
		scalar = 5.0f;
		scalar += 2.0f;
		++scalar;
		integer *= 3;
		--integer;
		integer++; // postfix increment follows the same active-domain rule
		vector = vec3(7.0f, 8.0f, 9.0f);
		vector.y *= 0.5f;
		counters.z = counters.z + 4;
	});

	float expect_scalar[4];
	int32_t expect_int[4] = { 30, 10, 30, 10 };
	for (int32_t i = 0; i < SIMDWIDTH; ++i) expect_scalar[i] = flags[i] ? 8.0f : 1.0f;
	TASK_CHECK(same_float_lanes(scalar, expect_scalar));
	TASK_CHECK(same_int_lanes(integer, expect_int));

	float out[SIMDWIDTH];
	simd_store_float(out, vector.x);
	for (int32_t i = 0; i < SIMDWIDTH; ++i) TASK_CHECK(out[i] == (flags[i] ? 7.0f : 1.0f));
	simd_store_float(out, vector.y);
	for (int32_t i = 0; i < SIMDWIDTH; ++i) TASK_CHECK(out[i] == (flags[i] ? 4.0f : 2.0f));
	simd_store_float(out, vector.z);
	for (int32_t i = 0; i < SIMDWIDTH; ++i) TASK_CHECK(out[i] == (flags[i] ? 9.0f : 3.0f));

	int32_t iz[SIMDWIDTH];
	simd_store_int(iz, counters.z);
	for (int32_t i = 0; i < SIMDWIDTH; ++i) TASK_CHECK(iz[i] == (flags[i] ? 4 : 0));
	return 0;
}

static int case_lanes_nested()
{
	bool outer_flags[4] = { true, true, false, false };
	bool inner_flags[4] = { true, false, true, false };
	simdmask outer = make_float_mask(outer_flags);
	simdimask inner = make_int_mask(inner_flags);

	simdfloat value(0.0f);
	vec2 uv(0.0f);
	shader::masked(outer, [&] {
		value = 10.0f;
		bump(value, uv); // same-TU helper sees the outer domain
		shader::masked(inner, [&] {
			value = 20.0f;
			uv.x = 99.0f;
		});
		// inner domain is gone, outer domain is back
		value = 30.0f;
	});

	float expected[4] = { 30.0f, 30.0f, 0.0f, 0.0f };
	TASK_CHECK(same_float_lanes(value, expected));
	float ux[SIMDWIDTH], uy[SIMDWIDTH];
	simd_store_float(ux, uv.x);
	simd_store_float(uy, uv.y);
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
	{
		TASK_CHECK(ux[i] == (i == 0 ? 99.0f : (outer_flags[i] ? 1.0f : 0.0f)));
		TASK_CHECK(uy[i] == (outer_flags[i] ? 2.0f : 0.0f));
	}
	return 0;
}

static int case_lanes_all_false()
{
	simdmask none; // default-constructed: every lane inactive
	int32_t host_calls = 0;
	bool inner_ran_once = false;
	simdfloat value(4.0f);
	vec4 color(1.0f, 2.0f, 3.0f, 4.0f);

	shader::masked(none, [&] {
		++host_calls;          // plain host side effect runs exactly once
		int32_t local_calls = 0;
		shader::masked(none, [&] { ++local_calls; });
		inner_ran_once = (local_calls == 1);
		value = 99.0f;
		color = vec4(9.0f);
	});
	TASK_CHECK(host_calls == 1);
	TASK_CHECK(inner_ran_once);

	float ref_scalar[4];
	for (int32_t i = 0; i < SIMDWIDTH; ++i) ref_scalar[i] = 4.0f;
	TASK_CHECK(same_float_lanes(value, ref_scalar));
	float lanes[SIMDWIDTH];
	for (int32_t c = 0; c < 4; ++c)
	{
		simd_store_float(lanes, color[c]);
		for (int32_t i = 0; i < SIMDWIDTH; ++i) TASK_CHECK(lanes[i] == static_cast<float>(c + 1));
	}

	// after leaving the scope, ordinary writes are full-width again
	value = 7.0f;
	for (int32_t i = 0; i < SIMDWIDTH; ++i) ref_scalar[i] = 7.0f;
	TASK_CHECK(same_float_lanes(value, ref_scalar));
	return 0;
}

static int case_lanes_exception()
{
	bool flags[4] = { true, false, true, false };
	simdmask mask = make_float_mask(flags);
	simdfloat value(1.0f);
	bool caught = false;
	try
	{
		shader::masked(mask, [&] {
			value = 5.0f;
			throw std::runtime_error("abort scope");
		});
	}
	catch (const std::exception&)
	{
		caught = true;
	}
	TASK_CHECK(caught);

	// domain restored during stack unwinding: this write hits every lane
	value = 11.0f;
	float ref[4];
	for (int32_t i = 0; i < SIMDWIDTH; ++i) ref[i] = 11.0f;
	TASK_CHECK(same_float_lanes(value, ref));
	return 0;
}

static int case_lanes_threads()
{
	struct row_result { float value[4]; float ux[4]; float uy[4]; int32_t iters[4]; };
	auto worker = [](const bool* _pattern, const int32_t* _limits, row_result* _out) {
		vec4 color(0.0f);
		ivec4 iterations(0);
		vec2 uv(0.0f);
		material_loop(_limits, _pattern, color, iterations, uv);
		simd_store_float(_out->value, color.x);
		simd_store_float(_out->ux, uv.x);
		simd_store_float(_out->uy, uv.y);
		simd_store_int(_out->iters, iterations.x);
	};

	bool pattern_a[4] = { true, true, false, true };
	bool pattern_b[4] = { false, true, true, false };
	int32_t limits_a[4] = { 2, 4, 3, 1 };
	int32_t limits_b[4] = { 5, 2, 1, 3 };
	row_result ra, rb;

	std::thread first(worker, pattern_a, limits_a, &ra);
	std::thread second(worker, pattern_b, limits_b, &rb);
	first.join();
	second.join();

	for (int32_t i = 0; i < SIMDWIDTH; ++i)
	{
		int32_t na = pattern_a[i] ? limits_a[i] : 0;
		TASK_CHECK(ra.iters[i] == na);
		TASK_CHECK(std::fabs(ra.value[i] - na * 1.35f) < 1e-5f);
		TASK_CHECK(std::fabs(ra.ux[i] - na * 0.25f) < 1e-6f);
		TASK_CHECK(std::fabs(ra.uy[i] - na * 0.75f) < 1e-6f);

		int32_t nb = pattern_b[i] ? limits_b[i] : 0;
		TASK_CHECK(rb.iters[i] == nb);
		TASK_CHECK(std::fabs(rb.value[i] - nb * 1.35f) < 1e-5f);
		TASK_CHECK(std::fabs(rb.ux[i] - nb * 0.25f) < 1e-6f);
		TASK_CHECK(std::fabs(rb.uy[i] - nb * 0.75f) < 1e-6f);
	}
	return 0;
}

// ---- values group: value semantics ----------------------------------------

static int case_values_full_width_temps()
{
	simdmask none;
	float arithmetic_out[4], sum_x[4], sum_y[4], sum_z[4], stored_out[4];
	bool comparisons_true = true;
	bool swizzle_ok = true;

	shader::masked(none, [&] {
		// construction, arithmetic and swizzle reads stay full-width
		simdfloat arithmetic = simdfloat(2.0f) * simdfloat(3.0f) + 1.0f;
		simd_store_float(arithmetic_out, arithmetic);

		vec3 fresh(1.0f, 2.0f, 3.0f);
		vec3 sum = fresh + vec3(0.5f);
		simdmask comparison = fresh.x > 0.0f;
		vec2 swizzle = fresh.yz();
		simd_store_float(sum_x, sum.x);
		simd_store_float(sum_y, sum.y);
		simd_store_float(sum_z, sum.z);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
		{
			if (comparison.value[i] == 0) comparisons_true = false;
			if (swizzle.x.value[i] != 2.0f || swizzle.y.value[i] != 3.0f) swizzle_ok = false;
		}
	});

	for (int32_t i = 0; i < SIMDWIDTH; ++i)
	{
		TASK_CHECK(arithmetic_out[i] == 7.0f);
		TASK_CHECK(sum_x[i] == 1.5f && sum_y[i] == 2.5f && sum_z[i] == 3.5f);
	}
	TASK_CHECK(comparisons_true);
	TASK_CHECK(swizzle_ok);

	// explicit load/store keep their original full-width behavior in a dead branch
	simdfloat loaded(0.0f);
	float source[4];
	for (int32_t i = 0; i < SIMDWIDTH; ++i) source[i] = static_cast<float>(i) + 0.5f;
	shader::masked(none, [&] {
		loaded = simd_load_float(source); // masked: existing object untouched
		simd_store_float(stored_out, simd_load_float(source)); // store still full-width
	});
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
	{
		TASK_CHECK(stored_out[i] == source[i]);
		TASK_CHECK(loaded.value[i] == 0.0f);
	}

	// direct access to the underlying value array bypasses the active domain,
	// exactly like simd_load/simd_store: it remains an ordinary full-width store
	simdfloat raw(0.0f);
	shader::masked(none, [&] {
		for (int32_t i = 0; i < SIMDWIDTH; ++i) raw.value[i] = source[i];
	});
	for (int32_t i = 0; i < SIMDWIDTH; ++i) TASK_CHECK(raw.value[i] == source[i]);
	return 0;
}

static int case_values_copy_aliases()
{
	simdmask none;
	vec4 source(1.0f, 2.0f, 3.0f, 4.0f);
	vec4 copy(0.0f);

	shader::masked(none, [&] {
		copy = source; // dead branch: copy keeps its zeroed data
	});
	float lanes[4];
	for (int32_t c = 0; c < 4; ++c)
	{
		simd_store_float(lanes, copy[c]);
		for (int32_t i = 0; i < SIMDWIDTH; ++i) TASK_CHECK(lanes[i] == 0.0f);
	}

	// copy construction is never domain-controlled: even in a dead branch the new
	// vector is fully populated, and its rgba/stpq aliases address its own data
	vec4 independent(0.0f);
	bool fresh_complete = true;
	shader::masked(none, [&] {
		vec4 fresh = source;
		independent = fresh; // dead write to an existing object: stays zeroed
		fresh.b = 77.0f;     // masked write to the temporary; reads still see all lanes
		for (int32_t c = 0; c < 4; ++c)
			if (fresh[c].value[0] != (c == 2 ? 3.0f : static_cast<float>(c + 1))) fresh_complete = false;
		// rgba and stpq aliases read the copy's own components, not source's
		if (fresh.r.value[0] != fresh.x.value[0] || fresh.g.value[0] != fresh.y.value[0] ||
		    fresh.b.value[0] != fresh.z.value[0] || fresh.a.value[0] != fresh.w.value[0] ||
		    fresh.s.value[0] != fresh.x.value[0] || fresh.t.value[0] != fresh.y.value[0] ||
		    fresh.p.value[0] != fresh.z.value[0] || fresh.q.value[0] != fresh.w.value[0])
			fresh_complete = false;
	});
	TASK_CHECK(fresh_complete);
	for (int32_t c = 0; c < 4; ++c)
	{
		simd_store_float(lanes, independent[c]);
		for (int32_t i = 0; i < SIMDWIDTH; ++i) TASK_CHECK(lanes[i] == 0.0f);
	}
	simd_store_float(lanes, source.b);
	for (int32_t i = 0; i < SIMDWIDTH; ++i) TASK_CHECK(lanes[i] == 3.0f);

	copy = source; // outside the scope: full-width copy
	copy.r = 9.0f;
	copy.g = 8.0f;
	copy.b = 7.0f;
	copy.a = 6.0f;
	copy.s += 1.0f;
	copy.t -= 1.0f;
	copy.p *= 2.0f;
	copy.q /= 2.0f;

	// rgba/stpq alias the copy's own components, the source vector is untouched
	TASK_CHECK(copy.x.value[0] == 10.0f && copy.y.value[0] == 7.0f);
	TASK_CHECK(copy.z.value[0] == 14.0f && copy.w.value[0] == 3.0f);
	TASK_CHECK(source.r.value[0] == 1.0f && source.a.value[0] == 4.0f);
	TASK_CHECK(source.s.value[0] == 1.0f && source.q.value[0] == 4.0f);
	return 0;
}

static int case_values_overlap_swizzle()
{
	bool flags[4] = { true, false, true, false };
	simdmask mask = make_float_mask(flags);

	float init_x[4], init_y[4], init_z[4], init_w[4];
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
	{
		init_x[i] = 1.0f + i;
		init_y[i] = 10.0f + i;
		init_z[i] = 20.0f + i;
		init_w[i] = 30.0f + i;
	}
	vec3 v3(simd_load_float(init_x), simd_load_float(init_y), simd_load_float(init_z));
	shader::masked(mask, [&] {
		v3.bgr(v3); // all right-hand values are snapshotted before any write
	});
	float lx[4], ly[4], lz[4];
	simd_store_float(lx, v3.x);
	simd_store_float(ly, v3.y);
	simd_store_float(lz, v3.z);
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
	{
		if (flags[i]) { TASK_CHECK(lx[i] == init_z[i] && ly[i] == init_y[i] && lz[i] == init_x[i]); }
		else { TASK_CHECK(lx[i] == init_x[i] && ly[i] == init_y[i] && lz[i] == init_z[i]); }
	}

	vec2 v2(simd_load_float(init_x), simd_load_float(init_y));
	shader::masked(mask, [&] {
		v2.yx(v2); // single-argument self swap, snapshots the pre-write vector
	});
	simd_store_float(lx, v2.x);
	simd_store_float(ly, v2.y);
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
	{
		if (flags[i]) { TASK_CHECK(lx[i] == init_y[i] && ly[i] == init_x[i]); }
		else { TASK_CHECK(lx[i] == init_x[i] && ly[i] == init_y[i]); }
	}

	vec2 v2b(simd_load_float(init_x), simd_load_float(init_y));
	shader::masked(mask, [&] {
		v2b.xy(v2b.y, v2b.x); // explicit multi-argument overlap
	});
	simd_store_float(lx, v2b.x);
	simd_store_float(ly, v2b.y);
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
	{
		if (flags[i]) { TASK_CHECK(lx[i] == init_y[i] && ly[i] == init_x[i]); }
		else { TASK_CHECK(lx[i] == init_x[i] && ly[i] == init_y[i]); }
	}

	vec4 v4(simd_load_float(init_x), simd_load_float(init_y),
	        simd_load_float(init_z), simd_load_float(init_w));
	shader::masked(mask, [&] {
		v4.argb(v4);
	});
	float lw[4];
	simd_store_float(lx, v4.x);
	simd_store_float(ly, v4.y);
	simd_store_float(lz, v4.z);
	simd_store_float(lw, v4.w);
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
	{
		if (flags[i]) { TASK_CHECK(lx[i] == init_y[i] && ly[i] == init_z[i] && lz[i] == init_w[i] && lw[i] == init_x[i]); }
		else { TASK_CHECK(lx[i] == init_x[i] && ly[i] == init_y[i] && lz[i] == init_z[i] && lw[i] == init_w[i]); }
	}

	// integer vectors use the same snapshot rule
	int32_t ix[4] = { 1, 2, 3, 4 }, iy[4] = { 5, 6, 7, 8 };
	ivec2 iv(simd_load_int(ix), simd_load_int(iy));
	simdimask imask = make_int_mask(flags);
	shader::masked(imask, [&] { iv.yx(iv); });
	int32_t ox[4], oy[4];
	simd_store_int(ox, iv.x);
	simd_store_int(oy, iv.y);
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
	{
		if (flags[i]) { TASK_CHECK(ox[i] == iy[i] && oy[i] == ix[i]); }
		else { TASK_CHECK(ox[i] == ix[i] && oy[i] == iy[i]); }
	}
	return 0;
}

static int case_values_index_and_aliases()
{
	bool flags[4] = { false, true, false, true };
	simdmask mask = make_float_mask(flags);
	vec3 v(0.0f);

	shader::masked(mask, [&] {
		v[0] = 1.0f; // component subscript write
		v.g = 2.0f;  // color alias write
		v.p = 3.0f;  // stpq alias write
	});
	float lanes[4];
	for (int32_t c = 0; c < 3; ++c)
	{
		simd_store_float(lanes, v[c]);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			TASK_CHECK(lanes[i] == (flags[i] ? static_cast<float>(c + 1) : 0.0f));
	}

	ivec3 iv(0);
	simdimask imask = make_int_mask(flags);
	shader::masked(imask, [&] {
		iv[1] = 42;
		iv.t = 43;
	});
	int32_t il[4];
	simd_store_int(il, iv.x);
	for (int32_t i = 0; i < SIMDWIDTH; ++i) TASK_CHECK(il[i] == 0);
	simd_store_int(il, iv.y);
	for (int32_t i = 0; i < SIMDWIDTH; ++i) TASK_CHECK(il[i] == (flags[i] ? 43 : 0));
	simd_store_int(il, iv.z);
	for (int32_t i = 0; i < SIMDWIDTH; ++i) TASK_CHECK(il[i] == 0);
	return 0;
}

static int case_values_temp_outside_scope()
{
	bool flags[4] = { true, false, true, false };
	simdmask mask = make_float_mask(flags);
	vec3 kept(7.0f);

	shader::masked(mask, [&] {
		vec3 local(4.0f, 5.0f, 6.0f);
		kept = local; // only active lanes are overwritten, no partial initialization
	});
	float lanes[4];
	for (int32_t c = 0; c < 3; ++c)
	{
		simd_store_float(lanes, kept[c]);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			TASK_CHECK(lanes[i] == (flags[i] ? static_cast<float>(c + 4) : 7.0f));
	}

	// scope exited: the surviving value can be consumed with full-width writes
	vec3 consumed(0.0f);
	consumed = kept * 2.0f;
	for (int32_t c = 0; c < 3; ++c)
	{
		simd_store_float(lanes, consumed[c]);
		for (int32_t i = 0; i < SIMDWIDTH; ++i)
			TASK_CHECK(lanes[i] == (flags[i] ? static_cast<float>(2 * (c + 4)) : 14.0f));
	}
	return 0;
}

static int case_values_cross_tu_helper()
{
	bool flags[4] = { true, false, true, false };
	simdmask mask = make_float_mask(flags);
	simdfloat value(0.0f);
	vec2 uv(0.0f);

	// shader_step lives in task_support/helper.cpp (another translation unit)
	shader::masked(mask, [&] { shader_step(value, uv); });

	float v[4], ux[4], uy[4];
	simd_store_float(v, value);
	simd_store_float(ux, uv.x);
	simd_store_float(uy, uv.y);
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
	{
		TASK_CHECK(v[i] == (flags[i] ? 1.25f : 0.0f));
		TASK_CHECK(ux[i] == (flags[i] ? 0.25f : 0.0f));
		TASK_CHECK(uy[i] == (flags[i] ? 0.75f : 0.0f));
	}
	return 0;
}

// ---- shader group: material loop with per-lane termination ----------------

static int case_shader_material_loop()
{
	int32_t limits[4] = { 1, 4, 2, 3 };
	bool active[4] = { true, true, true, true };
	vec4 color(0.0f);
	ivec4 iterations(0);
	vec2 uv(0.0f);
	material_loop(limits, active, color, iterations, uv);

	float cr[4], cg[4], cb[4], ca[4], ux[4], uy[4];
	simd_store_float(cr, color.r);
	simd_store_float(cg, color.g);
	simd_store_float(cb, color.b);
	simd_store_float(ca, color.a);
	simd_store_float(ux, uv.s);
	simd_store_float(uy, uv.t);
	int32_t ir[4];
	simd_store_int(ir, iterations.r);

	for (int32_t i = 0; i < SIMDWIDTH; ++i)
	{
		const int32_t n = limits[i]; // plain per-pixel scalar reference
		TASK_CHECK(ir[i] == n);
		TASK_CHECK(std::fabs(cr[i] - n * 1.35f) < 1e-5f);
		TASK_CHECK(std::fabs(cg[i] - n * 0.2f) < 1e-6f);
		TASK_CHECK(std::fabs(cb[i] - n * 0.3f) < 1e-6f);
		TASK_CHECK(std::fabs(ca[i] - n * 0.4f) < 1e-6f);
		TASK_CHECK(std::fabs(ux[i] - n * 0.25f) < 1e-6f);
		TASK_CHECK(std::fabs(uy[i] - n * 0.75f) < 1e-6f);
	}

	// same loop behind a mixed outer mask: inactive lanes never start, and a
	// lane that terminates early keeps its final color for the remaining passes
	bool mixed[4] = { false, true, false, true };
	vec4 color2(0.0f);
	ivec4 iterations2(0);
	vec2 uv2(0.0f);
	material_loop(limits, mixed, color2, iterations2, uv2);
	simd_store_float(cr, color2.r);
	simd_store_int(ir, iterations2.x);
	for (int32_t i = 0; i < SIMDWIDTH; ++i)
	{
		const int32_t n = mixed[i] ? limits[i] : 0;
		TASK_CHECK(ir[i] == n);
		TASK_CHECK(std::fabs(cr[i] - n * 1.35f) < 1e-5f);
	}
	return 0;
}

const task_case task_cases[] = {
	{ "lanes", "mixed", case_lanes_mixed },
	{ "lanes", "nested", case_lanes_nested },
	{ "lanes", "all_false", case_lanes_all_false },
	{ "lanes", "exception", case_lanes_exception },
	{ "lanes", "threads", case_lanes_threads },
	{ "values", "full_width_temps", case_values_full_width_temps },
	{ "values", "copy_aliases", case_values_copy_aliases },
	{ "values", "overlap_swizzle", case_values_overlap_swizzle },
	{ "values", "index_and_aliases", case_values_index_and_aliases },
	{ "values", "temp_outside_scope", case_values_temp_outside_scope },
	{ "values", "cross_tu_helper", case_values_cross_tu_helper },
	{ "shader", "material_loop", case_shader_material_loop },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
