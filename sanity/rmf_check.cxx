/* Driver for sanity/check_rmf.sh. Exercises ts_bspline_compute_rmf (C) and
 * BSpline::computeRMF (C++) with repeated knots, 2D curves, custom first
 * normals, and the rejection cases (zero/non-finite derivative, coincident
 * points with deviating tangents, invalid first normal). Prints
 * "rmf repeated samples PASS" and exits with 0 on success; any failed check
 * results in a non-zero exit code. */
#include "tinyspline.h"
#include "tinysplinecxx.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

static int failures = 0;

/* Verification tolerance: 1e-9 for the default (double precision)
 * configuration; relaxed for single precision builds. */
#ifdef TINYSPLINE_FLOAT_PRECISION
#define RMF_TOL 1e-4
#else
#define RMF_TOL 1e-9
#endif

#define CHECK(cond, msg)                                                  \
	do {                                                              \
		if (!(cond)) {                                            \
			std::fprintf(stderr, "FAIL [%s:%d]: %s\n",          \
			             __FILE__, __LINE__, msg);              \
			failures++;                                       \
		}                                                         \
	} while (0)

static double
dot3(const tsReal *a, const tsReal *b)
{
	return (double) a[0] * b[0] + (double) a[1] * b[1] +
	       (double) a[2] * b[2];
}

static double
mag3(const tsReal *a)
{
	return std::sqrt(dot3(a, a));
}

static double
dist3(const tsReal *a, const tsReal *b)
{
	tsReal d[3];
	ts_vec_sub(a, b, 3, d);
	return mag3(d);
}

/* Maximum absolute difference over all 12 components of two frames. */
static double
frame_dist(const tsFrame *a, const tsFrame *b)
{
	double m = 0.0, d;
	int i;
	for (i = 0; i < 3; i++) {
		d = std::fabs((double) a->position[i] - b->position[i]);
		if (d > m) m = d;
		d = std::fabs((double) a->tangent[i] - b->tangent[i]);
		if (d > m) m = d;
		d = std::fabs((double) a->normal[i] - b->normal[i]);
		if (d > m) m = d;
		d = std::fabs((double) a->binormal[i] - b->binormal[i]);
		if (d > m) m = d;
	}
	return m;
}

static int
frame_equal(const tsFrame *a, const tsFrame *b)
{
	int i;
	for (i = 0; i < 3; i++) {
		if (a->position[i] != b->position[i]) return 0;
		if (a->tangent[i] != b->tangent[i]) return 0;
		if (a->normal[i] != b->normal[i]) return 0;
		if (a->binormal[i] != b->binormal[i]) return 0;
	}
	return 1;
}

/* Verifies each frame against an independent evaluation of the spline and
 * its derivative, and checks finiteness, unit length, pairwise
 * orthogonality, and the right-handed relation binormal = tangent x
 * normal. */
static void
check_frames(const tsBSpline *spline,
             const tsBSpline *deriv,
             const tsReal *knots,
             size_t num,
             const tsFrame *frames,
             const char *label)
{
	tsStatus st;
	tsDeBoorNet net;
	tsReal *result = NULL;
	tsReal p3[3], b3[3];
	size_t i;
	int c, j;
	char msg[160];

	for (i = 0; i < num; i++) {
		const tsReal *comps[4];
		comps[0] = frames[i].position;
		comps[1] = frames[i].tangent;
		comps[2] = frames[i].normal;
		comps[3] = frames[i].binormal;
		for (c = 0; c < 4; c++) {
			for (j = 0; j < 3; j++) {
				std::snprintf(msg, sizeof(msg),
				              "%s: frame %lu not finite",
				              label, (unsigned long) i);
				CHECK(std::isfinite((double) comps[c][j]),
				      msg);
			}
		}

		/* Position (independent evaluation). */
		net = ts_deboornet_init();
		std::snprintf(msg, sizeof(msg),
		              "%s: eval failed", label);
		CHECK(ts_bspline_eval(spline, knots[i], &net, &st)
		      == TS_SUCCESS, msg);
		CHECK(ts_deboornet_result(&net, &result, &st)
		      == TS_SUCCESS, msg);
		ts_vec3_set(p3, result, ts_bspline_dimension(spline));
		ts_deboornet_free(&net);
		std::free(result);
		result = NULL;
		std::snprintf(msg, sizeof(msg),
		              "%s: position of frame %lu", label,
		              (unsigned long) i);
		CHECK(dist3(p3, frames[i].position) < RMF_TOL, msg);

		/* Tangent (independent evaluation). */
		net = ts_deboornet_init();
		CHECK(ts_bspline_eval(deriv, knots[i], &net, &st)
		      == TS_SUCCESS, msg);
		CHECK(ts_deboornet_result(&net, &result, &st)
		      == TS_SUCCESS, msg);
		ts_vec3_set(p3, result, ts_bspline_dimension(deriv));
		ts_deboornet_free(&net);
		std::free(result);
		result = NULL;
		ts_vec_norm(p3, 3, p3);
		std::snprintf(msg, sizeof(msg),
		              "%s: tangent of frame %lu", label,
		              (unsigned long) i);
		CHECK(dist3(p3, frames[i].tangent) < RMF_TOL, msg);

		/* Unit length. */
		std::snprintf(msg, sizeof(msg),
		              "%s: unit vectors of frame %lu", label,
		              (unsigned long) i);
		CHECK(std::fabs(mag3(frames[i].tangent) - 1.0) < RMF_TOL,
		      msg);
		CHECK(std::fabs(mag3(frames[i].normal) - 1.0) < RMF_TOL,
		      msg);
		CHECK(std::fabs(mag3(frames[i].binormal) - 1.0) < RMF_TOL,
		      msg);

		/* Pairwise orthogonality. */
		std::snprintf(msg, sizeof(msg),
		              "%s: orthogonality of frame %lu", label,
		              (unsigned long) i);
		CHECK(std::fabs(dot3(frames[i].tangent,
		                     frames[i].normal)) < RMF_TOL, msg);
		CHECK(std::fabs(dot3(frames[i].tangent,
		                     frames[i].binormal)) < RMF_TOL, msg);
		CHECK(std::fabs(dot3(frames[i].normal,
		                     frames[i].binormal)) < RMF_TOL, msg);

		/* Right-handedness: binormal = tangent x normal. */
		ts_vec3_cross(frames[i].tangent, frames[i].normal, b3);
		std::snprintf(msg, sizeof(msg),
		              "%s: binormal of frame %lu", label,
		              (unsigned long) i);
		CHECK(dist3(b3, frames[i].binormal) < RMF_TOL, msg);
	}
}

static void
make_std_spline(tsBSpline *spline, tsBSpline *deriv)
{
	tsStatus st;
	CHECK(ts_bspline_new_with_control_points(
		      5, 3, 3, TS_CLAMPED, spline, &st,
		      100., 200., 0.,
		      150., 220., 10.,
		      190., 120., 50.,
		      260., 70.,  30.,
		      300., 200., 20.) == TS_SUCCESS,
	      "setup: standard spline");
	CHECK(ts_bspline_derive(spline, 1, 0, deriv, &st) == TS_SUCCESS,
	      "setup: derivative");
}

/* Removes the duplicates of `knots` (exactly equal consecutive entries). */
static size_t
dedup(const tsReal *knots, size_t num, tsReal *out)
{
	size_t i, n = 0;
	for (i = 0; i < num; i++) {
		if (i > 0 && knots[i] == knots[i-1])
			continue;
		out[n++] = knots[i];
	}
	return n;
}

/* Compares the frames computed with repeated knots against the frames
 * computed from the deduplicated knot sequence. */
static void
check_dedup(const tsBSpline *spline,
            const tsReal *knots,
            size_t num,
            const tsFrame *frames,
            const char *label)
{
	tsStatus st;
	tsReal dknots[64];
	tsFrame dframes[64];
	size_t dnum, i, k;
	char msg[160];

	dnum = dedup(knots, num, dknots);
	CHECK(ts_bspline_compute_rmf(spline, dknots, dnum, 0, dframes, &st)
	      == TS_SUCCESS, "dedup: compute");
	k = 0;
	for (i = 0; i < num; i++) {
		if (i > 0 && knots[i] == knots[i-1])
			continue;
		std::snprintf(msg, sizeof(msg),
		              "%s: dedup mismatch at %lu", label,
		              (unsigned long) i);
		CHECK(frame_dist(&frames[i], &dframes[k]) < RMF_TOL, msg);
		k++;
	}
}

static void
test_c_api(void)
{
	tsStatus st;
	tsBSpline spline = ts_bspline_init();
	tsBSpline deriv = ts_bspline_init();
	size_t i;

	make_std_spline(&spline, &deriv);

	/* Case: repeated endpoint (.2 sampled twice). */
	{
		tsReal knots[] = {0, .2, .2, .4, .7, 1};
		tsFrame frames[6];
		CHECK(ts_bspline_compute_rmf(&spline, knots, 6, 0,
		                             frames, &st) == TS_SUCCESS,
		      "repeated endpoint: compute");
		check_frames(&spline, &deriv, knots, 6, frames,
		             "repeated endpoint");
		CHECK(frame_equal(&frames[1], &frames[2]),
		      "repeated endpoint: duplicate frame not carried over");
		check_dedup(&spline, knots, 6, frames,
		            "repeated endpoint");
	}

	/* Case: multiple consecutive duplicates. */
	{
		tsReal knots[] = {0, .3, .3, .3, .55, .55, 1};
		tsFrame frames[7];
		CHECK(ts_bspline_compute_rmf(&spline, knots, 7, 0,
		                             frames, &st) == TS_SUCCESS,
		      "multi repeat: compute");
		check_frames(&spline, &deriv, knots, 7, frames,
		             "multi repeat");
		CHECK(frame_equal(&frames[1], &frames[2]) &&
		      frame_equal(&frames[2], &frames[3]),
		      "multi repeat: duplicate frames not carried over");
		CHECK(frame_equal(&frames[4], &frames[5]),
		      "multi repeat: duplicate frames not carried over");
		check_dedup(&spline, knots, 7, frames, "multi repeat");
	}

	/* Case: 2D straight line (positions and vectors are embedded into
	 * 3D; also exercises the vanishing second reflection vector). */
	{
		tsBSpline line = ts_bspline_init();
		tsBSpline lderiv = ts_bspline_init();
		tsReal knots[] = {0, .25, .5, .75, 1};
		tsFrame frames[5];
		tsReal x_unit[3] = {1, 0, 0};
		CHECK(ts_bspline_new_with_control_points(
		      2, 2, 1, TS_CLAMPED, &line, &st,
		      0., 0., 10., 0.) == TS_SUCCESS,
		      "2d line: setup");
		CHECK(ts_bspline_derive(&line, 1, 0, &lderiv, &st)
		      == TS_SUCCESS, "2d line: derive");
		CHECK(ts_bspline_compute_rmf(&line, knots, 5, 0,
		                             frames, &st) == TS_SUCCESS,
		      "2d line: compute");
		check_frames(&line, &lderiv, knots, 5, frames, "2d line");
		for (i = 0; i < 5; i++) {
			CHECK(dist3(frames[i].tangent, x_unit) < RMF_TOL,
			      "2d line: tangent");
			CHECK(std::fabs((double) frames[i].position[2])
			      < RMF_TOL, "2d line: z must be 0");
		}
		ts_bspline_free(&line);
		ts_bspline_free(&lderiv);
	}

	/* Case: non-planar curve, frames must rotate smoothly. */
	{
		tsReal knots[21];
		tsFrame frames[21];
		for (i = 0; i < 21; i++)
			knots[i] = (tsReal) i / (tsReal) 20;
		CHECK(ts_bspline_compute_rmf(&spline, knots, 21, 0,
		                             frames, &st) == TS_SUCCESS,
		      "non-planar: compute");
		check_frames(&spline, &deriv, knots, 21, frames,
		             "non-planar");
		for (i = 1; i < 21; i++) {
			CHECK(dot3(frames[i-1].normal,
			           frames[i].normal) > 0.0,
			      "non-planar: normal flips direction");
		}
	}

	/* Case: custom first normal with a slight tangential component. */
	{
		tsDeBoorNet net = ts_deboornet_init();
		tsReal *result = NULL;
		tsReal t0[3], n0[3], knots[] = {0, .2, .4, .7, 1};
		tsFrame frames[5];
		tsReal d;

		CHECK(ts_bspline_eval(&deriv, 0, &net, &st)
		      == TS_SUCCESS, "first normal: eval");
		CHECK(ts_deboornet_result(&net, &result, &st)
		      == TS_SUCCESS, "first normal: result");
		ts_vec3_set(t0, result, ts_bspline_dimension(&deriv));
		ts_vec_norm(t0, 3, t0);
		ts_deboornet_free(&net);
		std::free(result);

		/* n0: an arbitrary vector perpendicular to t0. */
		ts_vec3_init(n0, (tsReal) 0.3, (tsReal) -0.7,
		             (tsReal) 1.0);
		d = ts_vec_dot(t0, n0, 3);
		ts_vec_mul(t0, 3, d, frames[0].normal);
		ts_vec_sub(n0, frames[0].normal, 3, n0);
		ts_vec_norm(n0, 3, n0);

		/* Supply n0 plus a tangential component (scaled, i.e.,
		 * not normalized). */
		ts_vec3_init(frames[0].normal,
		             (tsReal) (2.0 * (n0[0] + 0.25 * t0[0])),
		             (tsReal) (2.0 * (n0[1] + 0.25 * t0[1])),
		             (tsReal) (2.0 * (n0[2] + 0.25 * t0[2])));
		CHECK(ts_bspline_compute_rmf(&spline, knots, 5, 1,
		                             frames, &st) == TS_SUCCESS,
		      "first normal: compute");
		CHECK(dist3(frames[0].normal, n0) < RMF_TOL,
		      "first normal: not projected onto normal plane");
		CHECK(dot3(frames[0].normal, n0) > 0.0,
		      "first normal: orientation not maintained");
		check_frames(&spline, &deriv, knots, 5, frames,
		             "first normal");

		/* Rejection: zero first normal. */
		ts_vec3_init(frames[0].normal, 0, 0, 0);
		CHECK(ts_bspline_compute_rmf(&spline, knots, 5, 1,
		                             frames, &st) == TS_NO_RESULT,
		      "first normal: zero vector not rejected");

		/* Rejection: first normal parallel to tangent. */
		ts_vec3_init(frames[0].normal,
		             (tsReal) (2.0 * t0[0]),
		             (tsReal) (2.0 * t0[1]),
		             (tsReal) (2.0 * t0[2]));
		CHECK(ts_bspline_compute_rmf(&spline, knots, 5, 1,
		                             frames, &st) == TS_NO_RESULT,
		      "first normal: parallel vector not rejected");

		/* Rejection: non-finite first normal. */
		ts_vec3_init(frames[0].normal,
		             (tsReal) std::nan(""), 0, 0);
		CHECK(ts_bspline_compute_rmf(&spline, knots, 5, 1,
		                             frames, &st) == TS_NO_RESULT,
		      "first normal: non-finite vector not rejected");
	}

	/* Case: zero derivative (at the first and at a later knot). */
	{
		tsBSpline z0 = ts_bspline_init();
		tsBSpline z1 = ts_bspline_init();
		tsReal ka[] = {0, (tsReal) 0.5};
		tsReal kb[] = {(tsReal) 0.5, 1};
		tsFrame frames[2];
		/* derivative vanishes at u = 0 */
		CHECK(ts_bspline_new_with_control_points(
		      4, 3, 3, TS_CLAMPED, &z0, &st,
		      0., 0., 0., 0., 0., 0.,
		      1., 0., 0., 2., 0., 0.) == TS_SUCCESS,
		      "zero derivative: setup z0");
		CHECK(ts_bspline_compute_rmf(&z0, ka, 2, 0, frames, &st)
		      == TS_NO_RESULT,
		      "zero derivative: not rejected (first knot)");
		/* derivative vanishes at u = 1 */
		CHECK(ts_bspline_new_with_control_points(
		      4, 3, 3, TS_CLAMPED, &z1, &st,
		      0., 0., 0., 1., 0., 0.,
		      2., 0., 0., 2., 0., 0.) == TS_SUCCESS,
		      "zero derivative: setup z1");
		CHECK(ts_bspline_compute_rmf(&z1, kb, 2, 0, frames, &st)
		      == TS_NO_RESULT,
		      "zero derivative: not rejected (later knot)");
		ts_bspline_free(&z0);
		ts_bspline_free(&z1);
	}

	/* Case: non-finite derivative. */
	{
		tsBSpline s = ts_bspline_init();
		tsReal knots[] = {0, 1};
		tsFrame frames[2];
		double nan = std::nan("");
		CHECK(ts_bspline_new_with_control_points(
		      2, 3, 1, TS_CLAMPED, &s, &st,
		      0., 0., 0., nan, 0., 0.) == TS_SUCCESS,
		      "non-finite derivative: setup");
		CHECK(ts_bspline_compute_rmf(&s, knots, 2, 0, frames, &st)
		      == TS_NO_RESULT,
		      "non-finite derivative: not rejected");
		ts_bspline_free(&s);
	}

	/* Case: coincident points with deviating tangents. */
	{
		tsBSpline loop = ts_bspline_init();
		tsReal knots[] = {0, 1};
		tsFrame frames[2];
		CHECK(ts_bspline_new_with_control_points(
		      5, 3, 3, TS_CLAMPED, &loop, &st,
		      0., 0., 0., 1., 0., 0., 1., 1., 0.,
		      0., 1., 0., 0., 0., 0.) == TS_SUCCESS,
		      "coincident: setup");
		CHECK(ts_bspline_compute_rmf(&loop, knots, 2, 0,
		                             frames, &st) == TS_NO_RESULT,
		      "coincident: deviating tangents not rejected");
		CHECK(std::strstr(st.message, "propagation direction")
		      != NULL,
		      "coincident: message does not explain the cause");
		ts_bspline_free(&loop);
	}

	/* Case: coincident points at different knots with equally
	 * directed tangents continue the previous frame. */
	{
		tsBSpline s = ts_bspline_init();
		tsBSpline d = ts_bspline_init();
		tsReal knots[] = {0, 1};
		tsFrame frames[2];
		CHECK(ts_bspline_new_with_control_points(
		      7, 3, 3, TS_CLAMPED, &s, &st,
		      0., 0., 0.,  1., 0., 0.,  1., 1., 0.,  0., 1., 0.,
		      -1., 1., 0.,  -1., 0., 0.,  0., 0., 0.)
		      == TS_SUCCESS, "coincident same dir: setup");
		CHECK(ts_bspline_derive(&s, 1, 0, &d, &st) == TS_SUCCESS,
		      "coincident same dir: derive");
		CHECK(ts_bspline_compute_rmf(&s, knots, 2, 0, frames, &st)
		      == TS_SUCCESS, "coincident same dir: compute");
		CHECK(frame_equal(&frames[0], &frames[1]),
		      "coincident same dir: frame not carried over");
		check_frames(&s, &d, knots, 2, frames,
		             "coincident same dir");
		ts_bspline_free(&s);
		ts_bspline_free(&d);
	}

	/* Case: num = 0 succeeds. */
	CHECK(ts_bspline_compute_rmf(&spline, NULL, 0, 0, NULL, &st)
	      == TS_SUCCESS, "num = 0: compute");

	ts_bspline_free(&spline);
	ts_bspline_free(&deriv);
}

static void
test_cxx_api(void)
{
	using namespace tinyspline;

	/* Repeated knots via the C++ wrapper. */
	{
		BSpline spline(5, 3, 3, BSpline::Type::Clamped);
		spline.setControlPoints({
			100, 200, 0,  150, 220, 10,  190, 120, 50,
			260, 70, 30,  300, 200, 20
		});
		FrameSeq frames = spline.computeRMF(
			std::vector<real>({0, .2, .2, .4, .7, 1}));
		CHECK(frames.size() == 6, "cxx: frame count mismatch");
		Frame f1 = frames.at(1), f2 = frames.at(2);
		CHECK(f1.position().distance(f2.position()) == 0 &&
		      f1.tangent().distance(f2.tangent()) == 0 &&
		      f1.normal().distance(f2.normal()) == 0 &&
		      f1.binormal().distance(f2.binormal()) == 0,
		      "cxx: duplicate frame not carried over");
		for (size_t i = 0; i < frames.size(); i++) {
			Frame f = frames.at(i);
			CHECK(std::isfinite(f.normal().x()) &&
			      std::isfinite(f.normal().y()) &&
			      std::isfinite(f.normal().z()),
			      "cxx: normal not finite");
		}

		/* num = 0 succeeds. */
		FrameSeq empty = spline.computeRMF(std::vector<real>());
		CHECK(empty.size() == 0, "cxx: num = 0");

		/* Custom first normal (with tangential component). */
		Vec3 t0 = frames.at(0).tangent();
		Vec3 n0 = frames.at(0).normal();
		Vec3 fn = n0.add(t0.multiply(0.25));
		FrameSeq custom = spline.computeRMF(
			std::vector<real>({0, .2, .4, .7, 1}), &fn);
		CHECK(custom.at(0).normal().distance(n0) < RMF_TOL,
		      "cxx: first normal not projected");
		CHECK(custom.at(0).normal().dot(n0) > 0,
		      "cxx: first normal orientation not maintained");

		/* Rejection: first normal parallel to tangent. */
		Vec3 par = t0.multiply(2.0);
		bool threw = false;
		try {
			spline.computeRMF(
				std::vector<real>({0, .5, 1}), &par);
		} catch (const std::runtime_error &) {
			threw = true;
		}
		CHECK(threw, "cxx: parallel first normal not rejected");
	}

	/* Repeated failures followed by a success must not corrupt the
	 * wrapper (and must not leak the temporary frame buffer). */
	{
		BSpline bad(4, 3, 3, BSpline::Type::Clamped);
		bad.setControlPoints({
			0, 0, 0,  1, 0, 0,  2, 0, 0,  2, 0, 0
		});
		for (int i = 0; i < 3; i++) {
			bool threw = false;
			try {
				bad.computeRMF(
					std::vector<real>({0.5, 1.0}));
			} catch (const std::runtime_error &) {
				threw = true;
			}
			CHECK(threw, "cxx: zero derivative not rejected");
		}

		BSpline good(5, 3, 3, BSpline::Type::Clamped);
		good.setControlPoints({
			100, 200, 0,  150, 220, 10,  190, 120, 50,
			260, 70, 30,  300, 200, 20
		});
		FrameSeq frames = good.computeRMF(
			std::vector<real>({0, .2, .2, .4, .7, 1}));
		CHECK(frames.size() == 6, "cxx: failed after failures");

		/* Copy and move behavior. */
		FrameSeq copy(frames);
		CHECK(copy.size() == frames.size(), "cxx: copy ctor");
		FrameSeq assigned;
		assigned = frames;
		CHECK(assigned.size() == frames.size(),
		      "cxx: copy assignment");
		FrameSeq moved(std::move(assigned));
		CHECK(moved.size() == frames.size() &&
		      assigned.size() == 0, "cxx: move ctor");
		assigned = std::move(copy);
		CHECK(assigned.size() == frames.size() &&
		      copy.size() == 0, "cxx: move assignment");
		CHECK(assigned.at(2).normal().distance(
		      frames.at(2).normal()) == 0,
		      "cxx: frame content corrupted");
	}
}

int
main(void)
{
	test_c_api();
	test_cxx_api();
	if (failures) {
		std::fprintf(stderr, "rmf repeated samples FAILED (%d)\n",
		             failures);
		return 1;
	}
	std::puts("rmf repeated samples PASS");
	return 0;
}
