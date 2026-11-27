#include "qaws_platform.h"   /* (QAWS_SCALAR_IS_FLOAT: the precision this pass declares) */
#if QAWS_SCALAR_IS_FLOAT
#ifndef QAWS_SURFACE_SWEPT_H_F32
#define QAWS_SURFACE_SWEPT_H_F32
#define QAWS_SURFACE_SWEPT_H_BODY
#endif
#else
#ifndef QAWS_SURFACE_SWEPT_H_F64
#define QAWS_SURFACE_SWEPT_H_F64
#define QAWS_SURFACE_SWEPT_H_BODY
#endif
#endif
#ifdef QAWS_SURFACE_SWEPT_H_BODY
#undef QAWS_SURFACE_SWEPT_H_BODY
#define QAWS_SURFACE_SWEPT_H

#include "qaws_surface_types.h"

/* Swept surface: cross-section profile swept along a 3D path curve.
   S(u,v) = path(u) + profile_x(v) * N(u) + profile_y(v) * B(u)
   where N, B are the Frenet normal and binormal of the path.

   u parameter follows the path curve domain.
   v parameter follows the profile curve domain.

   The path must be a 3D curve. The profile must be a 2D curve whose
   x,y coordinates map to the normal and binormal directions of the frame.

   The surface borrows (does not own) the path and profile curves.
   The caller must keep both curves alive for the lifetime of the surface. */

typedef struct qaws_surface_swept_desc
{
	qaws_curve const* path;       /* 3D path curve (borrowed) */
	qaws_curve const* profile;    /* 2D cross-section curve (borrowed) */
	qaws_scalar scale;            /* uniform scale of cross-section (0 = default 1.0) */
} qaws_surface_swept_desc;

qaws_status qaws_surface_create_swept(
	qaws_surface** out_surface,
	qaws_surface_swept_desc const* desc);

/* Choose the cross-section frame of an existing swept surface.
   angular_tolerance > 0 builds and installs the shared rotation-minimizing (Bishop) frame field
   (qaws_sweep_rmf.h) over the path, with the target angular error in radians (1e-3 .. 1e-7); initial_normal is
   projected onto the plane perpendicular to the path's start tangent, and NULL keeps the frame's default start.
   angular_tolerance == 0 releases the field and restores the surface's original Frenet frame behaviour.
   The surface's identity and its borrowed path and profile do not change; the field's memory comes from the
   allocator the surface was created with. Call it only when no thread is evaluating the surface. */
qaws_status qaws_surface_swept_set_rmf(
	qaws_surface* surface,
	qaws_vec3 const* initial_normal,
	qaws_scalar angular_tolerance);

#endif /* QAWS_SURFACE_SWEPT_H */
