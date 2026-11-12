#include "tinyspline.h"
#include "tinyspline_projection.h"
#include <stdio.h>

/*
 * Global point projection ("snapping") in C.
 *
 * Uniform sampling only finds the nearest of a finite set of samples and
 * ts_bspline_bisect assumes a monotone component, so neither can find the
 * nearest point of a spline that doubles back. A projection index instead
 * solves the global nearest-point problem and can be reused for many queries.
 * The index is an independent snapshot: the source spline may be changed or
 * freed afterwards.
 */
int main(int argc, char **argv)
{
	tsBSpline spline = ts_bspline_init();
	tsProjectionIndex index = ts_projection_index_init();
	tsStatus status;
	tsError err;

	/* A single cubic Bezier segment in 2D whose x coordinate rises and
	 * then returns to zero: a genuine fold-back. */
	tsReal ctrlp[8] = {
		0.0f, 0.0f,   /* P0 */
		10.0f, 5.0f,  /* P1 */
		10.0f, 15.0f, /* P2 */
		0.0f, 20.0f   /* P3 */
	};
	tsReal query[2] = {5.0f, 10.0f};
	tsProjection result;
	size_t d;

	(void) argc;
	(void) argv;

	err = ts_bspline_new(4, 2, 3, TS_BEZIERS, &spline, &status);
	if (err) {
		fprintf(stderr, "%s\n", status.message);
		return err;
	}
	ts_bspline_set_control_points(&spline, ctrlp, NULL);

	/* Build the reusable, owning snapshot of the spline. */
	err = ts_projection_index_new(&spline, &index, &status);
	if (err) {
		fprintf(stderr, "%s\n", status.message);
		goto cleanup;
	}

	/* The tolerance is a distance in curve space. Use at least 1e-10 times
	 * the control-point bounding-box diagonal (1e-10 here, the floor). */
	err = ts_projection_index_query(&index, query, 2, 1e-7,
	                                &result, &status);
	if (err) {
		fprintf(stderr, "%s\n", status.message);
		goto cleanup;
	}

	printf("query point:        (%g, %g)\n", query[0], query[1]);
	printf("nearest knot:       %g\n", result.knot);
	/* span_index is the left index, in the original knot vector, of the
	 * non-zero interval that contains the nearest point. At a break knot
	 * with full multiplicity it disambiguates the left vs. right one-sided
	 * limit instead of arbitrarily picking one of DeBoorNet's two results. */
	printf("span index:         %zu\n", result.span_index);
	printf("nearest curve point:(");
	for (d = 0; d < 2; d++) {
		printf("%g%s", result.point[d], d + 1 < 2 ? ", " : "");
	}
	printf(")\n");
	/* Certified bracket of the true shortest distance of the whole curve:
	 * lower <= true minimum <= upper, with upper - lower <= epsilon. */
	printf("distance bracket:   [%g, %g] (width %g)\n",
	       result.distance_lower, result.distance_upper,
	       result.distance_upper - result.distance_lower);
	/* Number of original knot intervals that needed local refinement. The
	 * other intervals were pruned from their global distance bounds. */
	printf("refined pieces:     %zu\n", result.visited_pieces);

	/* The same index answers further queries without rebuilding. */
	query[0] = -4.0f;
	query[1] = 2.0f;
	err = ts_projection_index_query(&index, query, 2, 1e-7,
	                                &result, &status);
	if (err) {
		fprintf(stderr, "%s\n", status.message);
		goto cleanup;
	}
	printf("second query -> knot %g, distance %g\n",
	       result.knot, result.distance_upper);

cleanup:
	/* The index owns all of its data and frees independently of the
	 * spline. */
	ts_projection_index_free(&index);
	ts_bspline_free(&spline);
	return err;
}
