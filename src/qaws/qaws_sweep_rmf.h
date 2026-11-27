#include "qaws_platform.h"   /* (QAWS_SCALAR_IS_FLOAT: the precision this pass declares) */
#if QAWS_SCALAR_IS_FLOAT
#ifndef QAWS_SWEEP_RMF_H_F32
#define QAWS_SWEEP_RMF_H_F32
#define QAWS_SWEEP_RMF_H_BODY
#endif
#else
#ifndef QAWS_SWEEP_RMF_H_F64
#define QAWS_SWEEP_RMF_H_F64
#define QAWS_SWEEP_RMF_H_BODY
#endif
#endif
#ifdef QAWS_SWEEP_RMF_H_BODY
#undef QAWS_SWEEP_RMF_H_BODY
#define QAWS_SWEEP_RMF_H

#include "qaws_types.h"
#include "qaws_status.h"
#include "qaws_allocator.h"

/* Shared rotation-minimizing (Bishop, parallel-transport) frame field of a 3D path curve, for the swept and the pipe
   surface. The field is built once, from one node per distinct knot span and adaptively refined inside each span until
   the fourth-order double-reflection step is within the angular tolerance; the nodes' normals are then transported
   along the curve, never blended axis by axis. Queries interpolate the transported normals with a cubic Hermite in the
   curve parameter (their analytic transport rates are the Hermite's endpoint derivatives) and renormalize, so the frame
   is continuous in value and first derivative across the nodes and across the cache's spans. The normal is free of
   rotation about the tangent (the transport rate has no binormal component), and a straight piece or an inflection is
   carried with the previous frame rather than a jumping Frenet frame.

   The field borrows the path curve; the caller keeps it alive. It is immutable after creation and safe for concurrent
   queries. Its memory comes from the allocator it is created with (the surface's), and qaws_sweep_rmf_destroy frees it
   with the same one. */

typedef struct qaws_sweep_rmf_field qaws_sweep_rmf_field;

/* Build the frame field of path over its whole parameter range.
   initial_normal (may be NULL) is projected onto the plane perpendicular to the tangent at the start; NULL takes the
   axis least aligned with that tangent, the same starting direction the pipe has always used.
   angular_tolerance is the target angular error in radians, in [1e-7, 1e-3]; tighter values place more nodes, never
   uniformly over the whole range (each knot span refines only where it needs to).
   QAWS_STATUS_OUT_OF_RANGE when the tolerance is outside that range, QAWS_STATUS_INVALID_ARGUMENT for a non-finite
   normal or tolerance, QAWS_STATUS_DEGENERATE_CURVE where the speed vanishes, QAWS_STATUS_INVALID_DIMENSION when the
   path is not 3D. On failure *out_field is left NULL. */
qaws_status qaws_sweep_rmf_create(
	qaws_sweep_rmf_field** out_field,
	qaws_curve const* path,
	qaws_vec3 const* initial_normal,
	qaws_scalar angular_tolerance,
	qaws_allocator const* allocator);

void qaws_sweep_rmf_destroy(
	qaws_sweep_rmf_field* field,
	qaws_allocator const* allocator);

/* The frame at parameter t (clamped to the field's range): the path point and first derivative, the unit tangent, the
   normal and binormal, and the normal's and binormal's derivatives with respect to the curve parameter. All of them
   come from the same field, so a position, its derivatives and its normal are consistent across the nodes. The normal
   and its rate are the field's interpolant and that interpolant's exact derivative. */
qaws_status qaws_sweep_rmf_frame(
	qaws_vec3* out_position,
	qaws_vec3* out_d1,
	qaws_vec3* out_normal,
	qaws_vec3* out_binormal,
	qaws_vec3* out_dnormal,
	qaws_vec3* out_dbinormal,
	qaws_sweep_rmf_field const* field,
	qaws_scalar parameter);

#endif /* QAWS_SWEEP_RMF_H */
