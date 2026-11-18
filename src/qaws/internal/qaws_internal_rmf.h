#ifndef QAWS_INTERNAL_RMF_H
#define QAWS_INTERNAL_RMF_H

/*
 * The span-adaptive Bishop (rotation-minimizing) frame field shared by the
 * swept and pipe backends (qaws_sweep_rmf.c, compiled once per precision).
 *
 * The field is built per knot span of the source path: every real span gets
 * its own sample count, refined until the transported normal meets the
 * angular target, and the frames carry on across the span boundaries from
 * the last frame of the previous span. A query takes one double reflection
 * from the sample at or below it, so the field is continuous (including over
 * straight pieces and curvature zeros) and does not depend on query order.
 */

#include "../qaws_types.h"
#include "../qaws_status.h"

/* pipes come out of creation with this adaptive precision (radians) */
#define QAWS_RMF_DEFAULT_TOLERANCE QAWS_LITERAL(1e-5)
/* the supported angular-error targets (radians); 0 switches the field off */
#define QAWS_RMF_TOLERANCE_MIN QAWS_LITERAL(1e-7)
#define QAWS_RMF_TOLERANCE_MAX QAWS_LITERAL(1e-3)

typedef struct qaws_rmf_field qaws_rmf_field;

/* OK for a positive tolerance inside [1e-7, 1e-3]; OUT_OF_RANGE for a finite
   one outside it (negative included); INVALID_ARGUMENT for NaN/inf or 0 is
   accepted by the caller as "switch off", not here */
qaws_status qaws_rmf_validate_tolerance(qaws_scalar angular_tolerance);

/* Build a field over path_range (the surface's normalized u maps onto it).
   initial_normal is projected onto the normal plane of the start tangent;
   NULL lets the builder pick one. DEGENERATE_CURVE where the speed vanishes,
   INVALID_ARGUMENT for a normal parallel to the tangent. All memory comes
   from allocator (NULL: the current allocator); *out_field stays untouched on
   failure, so a rejected rebuild leaves the previous mode in place. */
qaws_status qaws_rmf_field_build(
	qaws_rmf_field** out_field,
	qaws_curve const* path,
	qaws_range path_range,
	qaws_vec3 const* initial_normal,
	qaws_scalar angular_tolerance,
	qaws_allocator const* allocator);

void qaws_rmf_field_free(qaws_rmf_field* field, qaws_allocator const* allocator);

/* The frame at normalized u in [0, 1] (mapped onto the build range) and the
   path evaluated there (position, first and second derivatives). */
qaws_status qaws_rmf_field_frame(
	qaws_rmf_field const* field,
	qaws_scalar u,
	qaws_eval_result_3d* out_path,
	qaws_vec3* out_tangent,
	qaws_vec3* out_normal,
	qaws_vec3* out_binormal);

/* The frame's rates in the path's own parameter: a rotation-minimizing
   frame turns with the tangent only, N' = -(T' . N) T and B' = -(T' . B) T
   with T' = (C'' - (C'' . T) T) / |C'|. c holds D1 and D2 at the point;
   T, N, B the frame. The surface's normalized-u chain scale is applied by
   the caller, once. */
void qaws_rmf_frame_rates(
	qaws_eval_result_3d const* c,
	qaws_vec3 T,
	qaws_vec3 N,
	qaws_vec3 B,
	qaws_vec3* out_dn,
	qaws_vec3* out_db);

#endif /* QAWS_INTERNAL_RMF_H */
