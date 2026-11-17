#include "../qaws_platform.h"
#include "../qaws_types.h"
#include "../qaws_core_types.h"
#include "../simd/qaws_simd.h"

/*
 * Maximum SIMD lane count across all supported ISAs (AVX2 float = 8).
 * Used to size stack buffers for SIMD store operations.
 */
#ifndef QAWS_SIMD_MAX_WIDTH
# define QAWS_SIMD_MAX_WIDTH 8
#endif

/* Portable alignment for SIMD store buffers */
#if defined(_MSC_VER)
# define QAWS_BATCH_ALIGN(n) __declspec(align(n))
#elif defined(__GNUC__) || defined(__clang__)
# define QAWS_BATCH_ALIGN(n) __attribute__((aligned(n)))
#else
# define QAWS_BATCH_ALIGN(n)
#endif

/* ===================================================================
 * Batch Bezier evaluation via De Casteljau (2D)
 *
 * Evaluates the same Bezier curve at N parameter values.
 * The SIMD path processes QAWS_SIMD_WIDTH values per iteration.
 *
 * control_points: flat array [x0,y0, x1,y1, ..., xN,yN]
 * degree:         polynomial degree (point_count - 1)
 * t_values:       array of count parameter values (must be
 *                 QAWS_SIMD_WIDTH-aligned for the SIMD path)
 * count:          number of parameter values
 * out_positions:  receives count * 2 scalars [x0,y0, x1,y1, ...]
 * =================================================================== */

void qaws_batch_bezier_eval_2d(
    qaws_scalar const* control_points,
    unsigned int degree,
    qaws_scalar const* t_values,
    unsigned int count,
    qaws_scalar* out_positions)
{
    unsigned int i;
    unsigned int w = QAWS_SIMD_WIDTH;
    unsigned int simd_count = count - (count % w);

    /* SIMD path: process w parameter values at a time */
    for (i = 0; i < simd_count; i += w) {
        qaws_simd t = qaws_simd_load(&t_values[i]);
        qaws_simd one = qaws_simd_set1(QAWS_ONE);
        qaws_simd u = qaws_simd_sub(one, t);

        /* Broadcast control points into SIMD registers */
        qaws_simd wx[QAWS_CORE_MAX_POINTS];
        qaws_simd wy[QAWS_CORE_MAX_POINTS];
        unsigned int k, r;

        for (k = 0; k <= degree; k++) {
            wx[k] = qaws_simd_set1(control_points[k * 2]);
            wy[k] = qaws_simd_set1(control_points[k * 2 + 1]);
        }

        /* De Casteljau triangular reduction */
        for (r = 1; r <= degree; r++) {
            for (k = 0; k <= degree - r; k++) {
                wx[k] = qaws_simd_add(qaws_simd_mul(u, wx[k]),
                                       qaws_simd_mul(t, wx[k + 1]));
                wy[k] = qaws_simd_add(qaws_simd_mul(u, wy[k]),
                                       qaws_simd_mul(t, wy[k + 1]));
            }
        }

        /* Store results with interleaved x,y layout */
        {
            QAWS_BATCH_ALIGN(32) qaws_scalar xbuf[QAWS_SIMD_MAX_WIDTH];
            QAWS_BATCH_ALIGN(32) qaws_scalar ybuf[QAWS_SIMD_MAX_WIDTH];
            unsigned int j;

            qaws_simd_store(xbuf, wx[0]);
            qaws_simd_store(ybuf, wy[0]);

            for (j = 0; j < w; j++) {
                out_positions[(i + j) * 2]     = xbuf[j];
                out_positions[(i + j) * 2 + 1] = ybuf[j];
            }
        }
    }

    /* Scalar tail for remaining elements */
    for (; i < count; i++) {
        qaws_scalar t_val = t_values[i];
        qaws_scalar u_s = QAWS_ONE - t_val;
        qaws_scalar wx_s[QAWS_CORE_MAX_POINTS];
        qaws_scalar wy_s[QAWS_CORE_MAX_POINTS];
        unsigned int k, r;

        for (k = 0; k <= degree; k++) {
            wx_s[k] = control_points[k * 2];
            wy_s[k] = control_points[k * 2 + 1];
        }

        for (r = 1; r <= degree; r++) {
            for (k = 0; k <= degree - r; k++) {
                wx_s[k] = u_s * wx_s[k] + t_val * wx_s[k + 1];
                wy_s[k] = u_s * wy_s[k] + t_val * wy_s[k + 1];
            }
        }

        out_positions[i * 2]     = wx_s[0];
        out_positions[i * 2 + 1] = wy_s[0];
    }
}

/* ===================================================================
 * Batch Bezier evaluation via De Casteljau (3D)
 *
 * control_points: flat array [x0,y0,z0, x1,y1,z1, ...]
 * out_positions:  receives count * 3 scalars [x0,y0,z0, ...]
 * =================================================================== */

void qaws_batch_bezier_eval_3d(
    qaws_scalar const* control_points,
    unsigned int degree,
    qaws_scalar const* t_values,
    unsigned int count,
    qaws_scalar* out_positions)
{
    unsigned int i;
    unsigned int w = QAWS_SIMD_WIDTH;
    unsigned int simd_count = count - (count % w);

    /* SIMD path */
    for (i = 0; i < simd_count; i += w) {
        qaws_simd t = qaws_simd_load(&t_values[i]);
        qaws_simd one = qaws_simd_set1(QAWS_ONE);
        qaws_simd u = qaws_simd_sub(one, t);

        qaws_simd wx[QAWS_CORE_MAX_POINTS];
        qaws_simd wy[QAWS_CORE_MAX_POINTS];
        qaws_simd wz[QAWS_CORE_MAX_POINTS];
        unsigned int k, r;

        for (k = 0; k <= degree; k++) {
            wx[k] = qaws_simd_set1(control_points[k * 3]);
            wy[k] = qaws_simd_set1(control_points[k * 3 + 1]);
            wz[k] = qaws_simd_set1(control_points[k * 3 + 2]);
        }

        for (r = 1; r <= degree; r++) {
            for (k = 0; k <= degree - r; k++) {
                wx[k] = qaws_simd_add(qaws_simd_mul(u, wx[k]),
                                       qaws_simd_mul(t, wx[k + 1]));
                wy[k] = qaws_simd_add(qaws_simd_mul(u, wy[k]),
                                       qaws_simd_mul(t, wy[k + 1]));
                wz[k] = qaws_simd_add(qaws_simd_mul(u, wz[k]),
                                       qaws_simd_mul(t, wz[k + 1]));
            }
        }

        {
            QAWS_BATCH_ALIGN(32) qaws_scalar xbuf[QAWS_SIMD_MAX_WIDTH];
            QAWS_BATCH_ALIGN(32) qaws_scalar ybuf[QAWS_SIMD_MAX_WIDTH];
            QAWS_BATCH_ALIGN(32) qaws_scalar zbuf[QAWS_SIMD_MAX_WIDTH];
            unsigned int j;

            qaws_simd_store(xbuf, wx[0]);
            qaws_simd_store(ybuf, wy[0]);
            qaws_simd_store(zbuf, wz[0]);

            for (j = 0; j < w; j++) {
                out_positions[(i + j) * 3]     = xbuf[j];
                out_positions[(i + j) * 3 + 1] = ybuf[j];
                out_positions[(i + j) * 3 + 2] = zbuf[j];
            }
        }
    }

    /* Scalar tail */
    for (; i < count; i++) {
        qaws_scalar t_val = t_values[i];
        qaws_scalar u_s = QAWS_ONE - t_val;
        qaws_scalar wx_s[QAWS_CORE_MAX_POINTS];
        qaws_scalar wy_s[QAWS_CORE_MAX_POINTS];
        qaws_scalar wz_s[QAWS_CORE_MAX_POINTS];
        unsigned int k, r;

        for (k = 0; k <= degree; k++) {
            wx_s[k] = control_points[k * 3];
            wy_s[k] = control_points[k * 3 + 1];
            wz_s[k] = control_points[k * 3 + 2];
        }

        for (r = 1; r <= degree; r++) {
            for (k = 0; k <= degree - r; k++) {
                wx_s[k] = u_s * wx_s[k] + t_val * wx_s[k + 1];
                wy_s[k] = u_s * wy_s[k] + t_val * wy_s[k + 1];
                wz_s[k] = u_s * wz_s[k] + t_val * wz_s[k + 1];
            }
        }

        out_positions[i * 3]     = wx_s[0];
        out_positions[i * 3 + 1] = wy_s[0];
        out_positions[i * 3 + 2] = wz_s[0];
    }
}
