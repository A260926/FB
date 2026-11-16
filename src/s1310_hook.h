/*
 * Internal tracing hook for the SISL intersection marcher s1310.
 *
 * The hook is deliberately private: it lets the native grid-event marcher
 * observe the final, ordered sequence of stations that s1310 accepted after
 * its own step acceptance, step retreat and parameter-boundary clipping
 * logic has run.  When no hook is installed (the normal public s1310 path),
 * s1310 behaves exactly as before.
 */
#ifndef SISL_S1310_HOOK_H
#define SISL_S1310_HOOK_H

#include "sisl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct S1310StationSink
{
    void *user;

    /* Emit the final accepted station sequence of one s1310 branch.
       egeo3d  : 10 doubles per station (position, unit tangent,
                 curvature vector, radius of curvature).
       egeop1  : 7 doubles per station in the parameter plane of surface 1
                 (position pair, unit tangent pair, curvature pair, radius).
       egeop2  : likewise for surface 2.
       aclosed : non-zero when the march returned to its start point.
       The callback must copy the data; the arrays are owned by s1310. */
    int (*emit)(void *user, int acount, const double *egeo3d,
                const double *egeop1, const double *egeop2, int aclosed);
} S1310StationSink;

/* Installed by the native grid-event marcher around a single s1310 call.
   s1310.c defines this symbol; it is SISL_NULL by default. */
extern S1310StationSink *s1310_station_sink;

#ifdef __cplusplus
}
#endif

#endif
