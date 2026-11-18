#include "qaws_platform.h"   /* (QAWS_SCALAR_IS_FLOAT: the precision this pass declares) */
#if QAWS_SCALAR_IS_FLOAT
#ifndef QAWS_SURFACE_PIPE_H_F32
#define QAWS_SURFACE_PIPE_H_F32
#define QAWS_SURFACE_PIPE_H_BODY
#endif
#else
#ifndef QAWS_SURFACE_PIPE_H_F64
#define QAWS_SURFACE_PIPE_H_F64
#define QAWS_SURFACE_PIPE_H_BODY
#endif
#endif
#ifdef QAWS_SURFACE_PIPE_H_BODY
#undef QAWS_SURFACE_PIPE_H_BODY
#define QAWS_SURFACE_PIPE_H

#include "qaws_surface_types.h"
#include "qaws_curve.h"

/* Pipe surface: sweeps a circular or elliptical cross-section along a 3D path curve.
   S(u,v) = path(u) + radius_x * cos(2*pi*v) * N(u) + radius_y * sin(2*pi*v) * B(u)

   u parameterizes along the path [0,1], v parameterizes around the cross-section [0,1].
   N(u), B(u) are a rotation-minimizing (parallel transport) frame along the path curve, by double reflection.
   For circular pipe, set radius_x = radius_y. */

typedef struct qaws_surface_pipe_desc
{
	qaws_curve const* path;        /* 3D path curve (borrowed) */
	qaws_scalar radius_x;          /* cross-section radius in normal direction */
	qaws_scalar radius_y;          /* cross-section radius in binormal direction (0 = same as radius_x) */
} qaws_surface_pipe_desc;

qaws_status qaws_surface_create_pipe(
	qaws_surface** out_surface,
	qaws_surface_pipe_desc const* desc);

/* Rebuild the pipe's frame field at angular_tolerance (radians, 1e-3 .. 1e-7):
   a span-adaptive Bishop field that subdivides every knot span of the path on
   its own, so a narrow span cannot skip a turn (the fixed table a pipe used to
   carry did). A new pipe already runs such a field at 1e-5; this call replaces
   it. initial_normal is projected onto the normal plane of the start tangent;
   NULL keeps the pipe's original starting direction. angular_tolerance == 0
   releases the adaptive field and falls back to the pipe's original fixed
   table. A rejected call changes nothing. Only call when no other thread is
   evaluating the surface. */
qaws_status qaws_surface_pipe_set_rmf(
	qaws_surface* surface,
	qaws_vec3 const* initial_normal,
	qaws_scalar angular_tolerance);

#endif /* QAWS_SURFACE_PIPE_H */
