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

/* Replace the swept surface's Frenet frame with a span-adaptive Bishop
   (rotation-minimizing, parallel-transport) frame field built along its
   borrowed path. Every real knot span is subdivided on its own until the
   transported normal meets angular_tolerance (radians, 1e-3 .. 1e-7), so a
   turn compressed into a narrow span is not skipped; the normal has no spin
   about the tangent and stays defined through straight pieces and curvature
   zeros. initial_normal is projected onto the normal plane of the start
   tangent; NULL keeps the frame the surface currently starts from. A positive
   tolerance rebuilds (or first enables) the field, replacing any earlier one.
   angular_tolerance == 0 releases the field and restores the original Frenet
   behavior. The surface keeps its identity and its borrowed path/profile;
   the field's cache is allocated and freed through the surface's allocator.
   A rejected call (an out-of-range or non-finite tolerance, a zero-speed
   path, a normal parallel to the tangent) changes nothing: the previous mode
   stays in force. Only call when no other thread is evaluating the surface. */
qaws_status qaws_surface_swept_set_rmf(
	qaws_surface* surface,
	qaws_vec3 const* initial_normal,
	qaws_scalar angular_tolerance);

#endif /* QAWS_SURFACE_SWEPT_H */
