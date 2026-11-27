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

/* Choose the cross-section frame of an existing pipe.
   angular_tolerance > 0 rebuilds the pipe's frame field as the shared rotation-minimizing (Bishop) field
   (qaws_sweep_rmf.h) over the path, with the target angular error in radians (1e-3 .. 1e-7); initial_normal is
   projected onto the plane perpendicular to the path's start tangent, and NULL keeps the pipe's original start
   direction. angular_tolerance == 0 releases the field and restores the pipe's original fixed-table
   rotation-minimizing (parallel-transport) frames.
   A pipe created without a call here uses the adaptive field at 1e-5. The pipe's identity and its borrowed path do
   not change; the field's memory comes from the allocator the pipe was created with. Call it only when no thread is
   evaluating the pipe. */
qaws_status qaws_surface_pipe_set_rmf(
	qaws_surface* surface,
	qaws_vec3 const* initial_normal,
	qaws_scalar angular_tolerance);

#endif /* QAWS_SURFACE_PIPE_H */
