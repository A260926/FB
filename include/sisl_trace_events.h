/*
 * Native SISL surface/surface intersection tracing with dual-parameter
 * grid events.
 *
 * The entry point sisl_trace_grid_events() reuses the real SISL marching
 * kernel: branch discovery is done by s1859 and every branch is marched by
 * the ordinary s1310 code path (its accepted steps, step retreat and
 * parameter-boundary clipping).  While such a branch is marched, s1310
 * publishes the final, ordered sequence of accepted stations through an
 * internal tracing hook.  The tracer then inserts the events of the caller
 * supplied U/V grid lines of *both* surfaces, jointly corrected on both
 * original surfaces, and refines the station chain until the chord error
 * against the true intersection curve is within the requested geometric
 * tolerance.
 *
 * Everything is plain C99; no geometry library beyond the packaged SISL is
 * used and the input surfaces are never modified (they are copied before
 * the marching kernel sees them).
 */

#ifndef SISL_TRACE_EVENTS_H
#define SISL_TRACE_EVENTS_H

#include <stddef.h>

#include "sisl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Grid line identity bits used by SislTraceStation.grid_mask. */
#define SISL_GRID_U1 0x1u /* first surface, first parameter direction  */
#define SISL_GRID_V1 0x2u /* first surface, second parameter direction */
#define SISL_GRID_U2 0x4u /* second surface, first parameter direction  */
#define SISL_GRID_V2 0x8u /* second surface, second parameter direction */

/* Return codes.  Every non-zero code means that the call failed as a
 * whole: no partial branch is ever returned. */
#define SISL_TRACE_OK 0
#define SISL_TRACE_INVALID_ARGUMENT 1 /* bad surface / grid / option       */
#define SISL_TRACE_REJECTED 2         /* tangent, singular, coincident,    */
                                      /* or intersection along a grid line */
#define SISL_TRACE_NUMERIC 3          /* iteration / correction failure    */
#define SISL_TRACE_BUDGET 4           /* max_stations exceeded             */
#define SISL_TRACE_INTERSECTION 5     /* s1859 warning or branch failure   */

/* One station of an intersection branch.
 *
 * xyz   : point on the intersection, jointly corrected on both surfaces.
 * uv1   : parameter pair on the first surface.
 * uv2   : parameter pair on the second surface.
 * grid_mask : OR of SISL_GRID_* bits for every grid line hit exactly by
 *             this station.  A point may hit several lines, including both
 *             parameter directions of one or both surfaces.
 * grid_index : index into the corresponding input grid array for each of
 *             the four grid families, in the order u1, v1, u2, v2.  The
 *             entry is -1 when that family is not hit.
 */
typedef struct SislTraceStation
{
    double xyz[3];
    double uv1[2];
    double uv2[2];
    unsigned int grid_mask;
    int grid_index[4];
} SislTraceStation;

/* One traced branch.  The stations are ordered along the branch and are
 * owned by the result.  For a closed branch `closed` is non-zero and the
 * last station does not repeat the first one. */
typedef struct SislTraceBranch
{
    SislTraceStation *stations;
    size_t count;
    int closed;
} SislTraceBranch;

/* Owned result of one call.  Release with sisl_trace_result_free(). */
typedef struct SislTraceResult
{
    SislTraceBranch *branches;
    size_t branch_count;
    size_t station_count; /* total over all branches */
} SislTraceResult;

/* Options.
 *
 * geometric_tolerance : positive, finite length tolerance.  Every station
 *                       back-substituted into either original surface must
 *                       be within this distance, and the chord error of the
 *                       intersection polyline between consecutive stations
 *                       (measured at the half and quarter points of the true
 *                       intersection curve) is kept within it as well.
 * max_stations        : positive upper bound on the total number of
 *                       stations of the whole call.  While a branch is
 *                       traced the limit is the remaining budget of the
 *                       call.
 */
typedef struct SislTraceOptions
{
    double geometric_tolerance;
    int max_stations;
} SislTraceOptions;

/*
 * Trace all intersection branches of surf1 and surf2, splitting the result
 * at every grid line crossing or touch of either surface.
 *
 * surf1, surf2 : 3-D, open, non-periodic clamped NURBS surfaces of order
 *                2..4 in both directions (degree 1..3).  Rational surfaces
 *                with positive weights are accepted.  The surfaces are
 *                embedded and regular and the intersection is transversal.
 * grid1_u, grid1_v, grid2_u, grid2_v :
 *                strictly increasing parameter grid lines.  Both ends of
 *                the corresponding parameter domain and every interior
 *                knot must be present.  Count is the array length.
 * options      : see above; may not be NULL.
 * result       : receives the owned result on success.  Set to NULL on
 *                every failure.
 *
 * Returns SISL_TRACE_OK on success.  On failure the temporary result is
 * released, *result is NULL and a SISL_TRACE_* error code is returned.
 */
int sisl_trace_grid_events(const SISLSurf *surf1,
                           const double *grid1_u, int grid1_u_count,
                           const double *grid1_v, int grid1_v_count,
                           const SISLSurf *surf2,
                           const double *grid2_u, int grid2_u_count,
                           const double *grid2_v, int grid2_v_count,
                           const SislTraceOptions *options,
                           SislTraceResult **result);

/* Release a result returned by sisl_trace_grid_events().  NULL is a no-op. */
void sisl_trace_result_free(SislTraceResult *result);

#ifdef __cplusplus
}
#endif

#endif /* SISL_TRACE_EVENTS_H */
