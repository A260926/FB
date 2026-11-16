/*
 * Compilable example for sisl_curve_on_surface_exact().
 *
 * Builds a bilinear tensor-product surface and a 2-D clamped parameter
 * curve, forms the exact 3-D composition C(t)=S(u(t),v(t)), and checks
 * it against pointwise surface evaluation with s1421.  The result is an
 * ordinary SISLCurve that is evaluated with s1221 and released with
 * freeCurve.
 *
 * Compile:
 *   cc -std=c99 -Iinclude examples/exact_composition_example.c \
 *      <path-to>/libsisl.a -lm -o exact_composition_example
 */

#include "sisl.h"
#include <math.h>
#include <stdio.h>

int main(void)
{
  /* Bilinear surface on [0,1]^2: S(u,v) = (u, v, u*v). */
  double surf_knots[] = {0.0, 0.0, 1.0, 1.0};
  double surf_coef[] = {
      0.0, 0.0, 0.0,
      1.0, 0.0, 0.0,
      0.0, 1.0, 0.0,
      1.0, 1.0, 1.0
  };
  SISLSurf *surface =
      newSurf(2, 2, 2, 2, surf_knots, surf_knots, surf_coef, 1, 3, 1);

  /* Quadratic 2-D parameter curve, clamped, wholly inside [0,1]^2. */
  double uv_knots[] = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
  double uv_coef[] = {
      0.10, 0.20,
      0.55, 0.80,
      0.90, 0.30
  };
  SISLCurve *uv = newCurve(3, 3, uv_knots, uv_coef, 1, 2, 1);
  if (surface == 0 || uv == 0)
    {
      fprintf(stderr, "object construction failed\n");
      return 1;
    }

  SISLCurve *result = 0;
  int stat = 0;
  sisl_curve_on_surface_exact(surface, uv, &result, &stat);
  if (stat != 0 || result == 0)
    {
      fprintf(stderr, "composition failed, status = %d\n", stat);
      freeCurve(uv);
      freeSurf(surface);
      return 2;
    }

  printf("parameter interval [%g,%g], output degree %d\n",
         result->et[result->ik - 1], result->et[result->in],
         result->ik - 1);

  int stat_eval = 0, left_curve = 0, left_u = 0, left_v = 0;
  double worst = 0.0;
  int i;
  for (i = 0; i <= 40; ++i)
    {
      double t = i / 40.0;
      double uv_value[8];
      double surface_derivatives[30], normal[3];
      double exact_point[3];

      /* Evaluate (u(t),v(t)). */
      left_curve = 0;
      s1221(uv, 1, t, &left_curve, uv_value, &stat_eval);
      if (stat_eval < 0) return 3;

      /* Reference point S(u(t),v(t)). */
      left_u = left_v = 0;
      s1421(surface, 1, uv_value, &left_u, &left_v,
            surface_derivatives, normal, &stat_eval);
      if (stat_eval < 0) return 4;

      /* The exact composition. */
      left_curve = 0;
      s1221(result, 0, t, &left_curve, exact_point, &stat_eval);
      if (stat_eval < 0) return 5;

      {
        int d;
        for (d = 0; d < 3; ++d)
          {
            double error = fabs(exact_point[d] - surface_derivatives[d]);
            if (error > worst) worst = error;
          }
      }
    }

  printf("maximum deviation from pointwise evaluation = %.3e\n", worst);

  freeCurve(result);
  freeCurve(uv);
  freeSurf(surface);

  if (worst > 1.0e-12)
    {
      fprintf(stderr, "example check failed\n");
      return 6;
    }
  printf("exact composition example OK\n");
  return 0;
}
