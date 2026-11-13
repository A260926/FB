#ifndef PORTAL_H
#define PORTAL_H

#include "graphics.h"
#include "maths.h"
#include "scene.h"

/*
 * recursive planar portal rendering.
 *
 * a portal is a rectangle in its local frame: z = 0, spanning
 * [-half_size.x, +half_size.x] x [-half_size.y, +half_size.y].
 * the +Z side is the front face; only a camera placed in front of the
 * plane and farther away than clip_bias can look through it.
 *
 * local_to_world must be a rigid body transform (rotation + translation,
 * no scale and no reflection). portals come in pairs that point at each
 * other through their linked indices, and a linked pair shares the same
 * half size. fallback is the opaque color drawn when the recursion depth
 * limit is reached.
 */
typedef struct {
    mat4_t local_to_world;
    vec2_t half_size;
    int linked;
    vec4_t fallback;
} portal_t;

/*
 * draw the scene with depth-limited recursive portals.
 *
 * returns 0 on success. invalid arguments or unsupported scene
 * configurations return a non-zero value and leave dst untouched.
 * count may be 0 with a NULL array, in which case the scene is drawn
 * normally. max_depth is clamped to [0, 4].
 */
int scene_draw_portals(scene_t *scene, framebuffer_t *dst,
                       const perframe_t *frame,
                       const portal_t *portals, int count,
                       int max_depth, float clip_bias);

#endif
