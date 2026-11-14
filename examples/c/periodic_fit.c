/*
 * Fits a closed, periodic cubic spline to a set of 2D points.
 *
 * This example shows how to use ts_bspline_fit_periodic_cubic. In contrast to
 * ts_bspline_interpolate_cubic_natural and
 * ts_bspline_interpolate_catmull_rom, the fitted spline is periodic: its
 * position as well as its first and second derivative are continuous at the
 * seam. Repeating the first point is neither necessary nor allowed.
 *
 * Build and run this example with CMake (the target is named
 * `periodic_fit_c') and it will print the control points and a few evaluated
 * points of the resulting spline.
 */

#include <math.h>
#include <stdio.h>

#include "tinyspline.h"

int main()
{
	/* The points of a closed profile (the first point is not repeated).
	 * Each point is made up of two components (x, y). */
	tsReal points[] = {
		1.0,  0.0,
		1.4,  1.1,
		0.6,  1.7,
		-0.4, 1.5,
		-1.0, 0.6,
		-0.7, -0.4,
		0.1,  -0.9
	};
	/* One parameter per point plus one value that specifies the end of
	 * the period. The parameters are arbitrary and need not be uniform;
	 * they are not normalized. */
	tsReal parameters[] = { 0.0, 0.5, 1.7, 2.2, 3.9, 4.4, 5.8, 6.4 };
	/* Non-negative weights. A weight of 0 excludes the corresponding
	 * point from the error term (the point's parameter is still a knot,
	 * however). */
	tsReal weights[] = { 1.0, 1.0, 0.0, 2.0, 1.0, 1.0, 0.7 };
	/* Hard constraints: the sample is satisfied exactly. */
	int hard[] = { 1, 0, 0, 0, 1, 0, 0 };
	const tsReal lambda = 0.4;

	tsBSpline spline = ts_bspline_init();
	tsStatus status;
	tsError error = ts_bspline_fit_periodic_cubic(
		points, 7, 2, parameters, weights, hard, lambda,
		&spline, &status);
	if (error) {
		fprintf(stderr, "error %d: %s\n", error, status.message);
		return 1;
	}

	printf("degree:             %lu\n",
	       (unsigned long) ts_bspline_degree(&spline));
	printf("dimension:          %lu\n",
	       (unsigned long) ts_bspline_dimension(&spline));
	printf("num control points: %lu\n",
	       (unsigned long) ts_bspline_num_control_points(&spline));
	printf("num knots:          %lu\n",
	       (unsigned long) ts_bspline_num_knots(&spline));

	printf("\ncontrol points:\n");
	{
		const tsReal *ctrlp =
			ts_bspline_control_points_ptr(&spline);
		size_t i, num = ts_bspline_num_control_points(&spline);
		for (i = 0; i < num; ++i) {
			printf("  %2lu: (%f, %f)\n", (unsigned long) i,
			       ctrlp[i * 2 + 0], ctrlp[i * 2 + 1]);
		}
	}

	printf("\nsamples:\n");
	{
		tsDeBoorNet net = ts_deboornet_init();
		size_t i;
		for (i = 0; i < 7; ++i) {
			if (ts_bspline_eval(&spline, parameters[i], &net,
			                    &status)) {
				fprintf(stderr, "error %d: %s\n",
				        status.code, status.message);
				break;
			}
			printf("  t=%f -> (%f, %f)%s\n",
			       parameters[i],
			       ts_deboornet_result_ptr(&net)[0],
			       ts_deboornet_result_ptr(&net)[1],
			       hard[i] ? "  (hard)" : "");
		}
		ts_deboornet_free(&net);
	}

	/* The result is an ordinary spline; derive it to check the C2
	 * continuity at the seam. */
	printf("\nseam continuity:\n");
	{
		tsBSpline deriv = ts_bspline_init();
		size_t order;
		for (order = 0; order <= 2; ++order) {
			tsDeBoorNet first = ts_deboornet_init();
			tsDeBoorNet last = ts_deboornet_init();
			const tsBSpline *s = &spline;
			tsReal t0 = parameters[0];
			tsReal tm = parameters[7];
			tsReal gap = 0.0;
			size_t d;
			if (order > 0) {
				if (ts_bspline_derive(&spline, order, -1.f,
				                      &deriv, &status)) {
					fprintf(stderr, "error %d: %s\n",
					        status.code,
					        status.message);
					break;
				}
				s = &deriv;
			}
			ts_bspline_eval(s, t0, &first, &status);
			ts_bspline_eval(s, tm, &last, &status);
			for (d = 0;
			     d < ts_deboornet_len_result(&first); ++d) {
				tsReal diff =
					ts_deboornet_result_ptr(&first)[d] -
					ts_deboornet_result_ptr(&last)[d];
				gap += diff * diff;
			}
			printf("  derivative %lu: gap = %e\n",
			       (unsigned long) order,
			       (double) sqrt((double) gap));
			ts_deboornet_free(&first);
			ts_deboornet_free(&last);
			ts_bspline_free(&deriv);
		}
	}

	ts_bspline_free(&spline);
	return 0;
}
