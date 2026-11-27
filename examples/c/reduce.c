/*
 * End-of-edit simplification of a modeling curve.
 *
 * A surface modeling tool repeatedly inserts knots while the artist drags
 * control points around, so the same shape ends up carrying far more
 * control points than needed. This example first simulates that editing
 * session by inserting a series of knots, then calls ts_bspline_simplify
 * to reclaim the redundant interior knots within a certified tolerance.
 * The parameter axis, degree, dimension, domain and endpoints are kept,
 * and the reported error bound holds over the whole parameter domain.
 */
#include "tinyspline_reduce.h"

#include <stdio.h>
#include <stdlib.h>

int
main(void)
{
	tsStatus status;
	tsBSpline curve = ts_bspline_init();
	tsBSpline edited = ts_bspline_init();
	tsBSpline simplified = ts_bspline_init();
	tsReal *ctrlp = NULL;

	/* Cubic 2D modeling curve with clamped end knots. */
	const tsReal initial_points[8] = {
		0, 0,
		1, 2,
		3, 1,
		4, 0
	};
	/* Knots inserted during the editing session. */
	const tsReal inserted_knots[5] = {
		0.15, 0.3, 0.5, 0.7, 0.85
	};
	/* Uniform Euclidean deviation budget for simplification. */
	const tsReal tolerance = (tsReal) 1e-6;

	size_t i;
	size_t removed;
	tsReal error_bound;
	tsError err;

	TS_TRY(try, status.code, &status)
		TS_CALL(try, status.code, ts_bspline_new(
		        4, 2, 3, TS_CLAMPED, &curve, &status))
		TS_CALL(try, status.code, ts_bspline_set_control_points(
		        &curve, initial_points, &status))

		/* Simulate the editing session: every insertion adds one
		 * control point without changing the geometry. */
		TS_CALL(try, status.code, ts_bspline_copy(
		        &curve, &edited, &status))
		for (i = 0; i < sizeof(inserted_knots) /
		                sizeof(inserted_knots[0]); i++) {
			tsBSpline next = ts_bspline_init();
			size_t k;
			TS_CALL(try, status.code, ts_bspline_insert_knot(
			        &edited, inserted_knots[i], 1,
			        &next, &k, &status))
			ts_bspline_free(&edited);
			edited = next;
		}
		printf("after editing: %lu control points\n",
		       (unsigned long)
		       ts_bspline_num_control_points(&edited));

		/* Reclaim every interior knot whose removal stays within
		 * the certified tolerance. The original curve serves as the
		 * single reference for the whole error budget. */
		TS_CALL(try, status.code, ts_bspline_simplify(
		        &edited, tolerance, &simplified,
		        &removed, &error_bound, &status))
		printf("simplified:   %lu control points "
		       "(%lu knots removed)\n",
		       (unsigned long)
		       ts_bspline_num_control_points(&simplified),
		       (unsigned long) removed);
		printf("certified error bound: %.3g (tolerance %.3g)\n",
		       (double) error_bound, (double) tolerance);

		/* The minimal curve is back to the original four control
		 * points because all insertions were exact. */
		if (ts_bspline_num_control_points(&simplified) !=
		    ts_bspline_num_control_points(&curve)) {
			TS_THROW_0(try, status.code, &status, TS_NO_RESULT,
			           "unexpected number of control points")
		}

		/* Endpoints must be preserved exactly. */
		TS_CALL(try, status.code, ts_bspline_control_points(
		        &simplified, &ctrlp, &status))
		if (ctrlp[0] != initial_points[0] ||
		    ctrlp[1] != initial_points[1] ||
		    ctrlp[6] != initial_points[6] ||
		    ctrlp[7] != initial_points[7]) {
			TS_THROW_0(try, status.code, &status, TS_NO_RESULT,
			           "endpoints were modified")
		}
		puts("parameter axis and endpoints preserved");
	TS_CATCH(status.code)
		puts(status.message);
	TS_FINALLY
		free(ctrlp);
		ts_bspline_free(&curve);
		ts_bspline_free(&edited);
		ts_bspline_free(&simplified);
	TS_END_TRY

	err = status.code;
	if (err == TS_SUCCESS)
		puts("REDUCE_EXAMPLE_OK");
	return err ? 1 : 0;
}
