/* c-ray-fast - fused ball field groups (metaballs).
 * Copyright (C) 2006-2021 John Tsiombikas <nuclear@member.fsf.org>
 *
 * You are free to use, modify and redistribute this program under the terms
 * of the GNU General Public License v3 or later. See COPYING for details.
 * ---------------------------------------------------------------------
 * A field group is the sum of compactly supported weighted kernels
 *   f_i(p) = w_i * max(0, 1 - |p - c_i|^2 / R_i^2)^2
 * and its surface is F(p) = tau. Kernels are summed first (negative weights
 * included) and the isosurface is intersected analytically; the supporting
 * spheres only delimit where each kernel contributes. Nothing is meshed and
 * no fixed-step marching is performed.
 */
#ifndef CRay_METABALL_H
#define CRay_METABALL_H

#include "scene.h"

/* Conservative axis-aligned union of the supporting spheres. */
void metaball_bounds(struct metaball *mb, struct aabox *box);

/* Field value F(p) and world-space gradient grad F(p) at point p. */
double mb_field(struct metaball *mb, struct vec3 *p);
void mb_gradient(struct metaball *mb, struct vec3 *p, struct vec3 *grad);

/* Production ray intersection. Finds the earliest root with
 * ERR_MARGIN <= t <= 1 along ray orig + t * dir (dir keeps its length).
 * Returns 1 and fills sp (including the outward unit normal pointing toward
 * decreasing F) or 0 when the ray misses the isosurface. sp may be NULL for
 * occlusion queries; the same root finder is used in both cases. */
int ray_metaball(struct metaball *mb, struct ray *ray, struct spoint *sp);

#endif	/* CRay_METABALL_H */
