/** @file reduce.c
 *
 * Complete modeling example for knot removal and curve simplification.
 *
 * A modeling tool repeatedly inserts knots into a curve while editing it.
 * Before storing the result, the redundant interior knots are collapsed
 * again with ts_bspline_simplify, keeping the same parameter axis while
 * obtaining a certified upper bound on the geometric deviation.
 *
 * Build (standalone, against the TinySpline C library):
 *     cc -std=c99 -I<include-dir> reduce.c -ltinyspline -lm -o reduce
 */

#include "tinyspline.h"
#include "tinyspline_reduce.h"

#include <stdio.h>
#include <stdlib.h>

int
main(void)
{
	/* A planar cubic design curve. */
	const tsReal control[] = {
		0.0, 0.0,
		1.0, 2.0,
		2.5, -1.0,
		3.5, 0.5,
		5.0, 1.0
	};
	const size_t num_points = 5;
	const size_t dim = 2;
	const size_t degree = 3;
	const tsReal tolerance = 1e-6;

	tsStatus status;
	tsBSpline curve = ts_bspline_init();
	tsBSpline edited = ts_bspline_init();
	tsBSpline reduced = ts_bspline_init();
	tsBSpline tmp = ts_bspline_init();
	tsDeBoorNet net = ts_deboornet_init();
	tsReal *result = NULL;
	size_t k, i, removed;
	tsReal error_bound;

	TS_TRY(try, status.code, &status)
		TS_CALL(try, status.code, ts_bspline_new(
		        num_points, dim, degree, TS_CLAMPED,
		        &curve, &status))
		TS_CALL(try, status.code, ts_bspline_set_control_points(
		        &curve, control, &status))

		/* Simulate an interactive editing session that repeatedly
		 * inserts interior knots. Each insertion preserves the shape
		 * but adds one control point. */
		TS_CALL(try, status.code, ts_bspline_copy(
		        &curve, &edited, &status))
		{
			const tsReal inserted[] = {0.2, 0.4, 0.6, 0.8};
			for (i = 0; i < sizeof(inserted) / sizeof(tsReal);
			     i++) {
				TS_CALL(try, status.code, ts_bspline_insert_knot(
				        &edited, inserted[i], 1,
				        &tmp, &k, &status))
				ts_bspline_free(&edited);
				ts_bspline_move(&tmp, &edited);
			}
		}
		printf("after editing: %lu control points\n",
		       (unsigned long)
		       ts_bspline_num_control_points(&edited));

		/* Collapse the knots that are not needed to represent the
		 * geometry within the tolerance. The parameterization,
		 * degree, dimension, domain and endpoints stay unchanged. */
		TS_CALL(try, status.code, ts_bspline_simplify(
		        &edited, tolerance, &reduced,
		        &removed, &error_bound, &status))
		printf("simplified:    %lu control points "
		       "(%lu knots removed)\n",
		       (unsigned long)
		       ts_bspline_num_control_points(&reduced),
		       (unsigned long) removed);
		printf("certified deviation bound: %.3e "
		       "(tolerance %.3e)\n",
		       (double) error_bound, (double) tolerance);

		/* Evaluate the simplified curve at a few parameters. */
		for (i = 0; i <= 4; i++) {
			tsReal u = (tsReal) i / 4.0;
			TS_CALL(try, status.code, ts_bspline_eval(
			        &reduced, u, &net, &status))
			TS_CALL(try, status.code, ts_deboornet_result(
			        &net, &result, &status))
			printf("C(%.2f) = (%.6f, %.6f)\n",
			       (double) u,
			       (double) result[0], (double) result[1]);
			free(result);
			result = NULL;
		}
	TS_CATCH(status.code)
		puts(status.message);
	TS_FINALLY
		ts_bspline_free(&curve);
		ts_bspline_free(&edited);
		ts_bspline_free(&reduced);
		ts_bspline_free(&tmp);
		ts_deboornet_free(&net);
		if (result)
			free(result);
	TS_END_TRY

	return status.code ? 1 : 0;
}
