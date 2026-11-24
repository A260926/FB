#pragma once

/**
 * Offline diffuse indirect-light baking for static pingo scenes.
 *
 * A BakeScene is an owned, frozen snapshot of an Entity subtree: world-space
 * triangles, per-corner secondary lightmap UVs, constant linear-RGB
 * reflectance/emission and the entity path of every instance. Once built it
 * keeps working after the source tree, meshes, bindings and materials have
 * been released.
 *
 * Paths are sequences of child indices taken from the root Entity; an empty
 * sequence addresses the root itself. The same Object attached in two places
 * is two independent instances: instances are never merged by Object pointer.
 */

#include "entity.h"
#include "fwd.h"
#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct BakeScene BakeScene;

/**
 * One bindable Object, addressed by its entity path. The binding carries the
 * lightmap size, the secondary UV array (with its own count and corner
 * indices) and constant linear-RGB material terms.
 */
typedef struct {
  const size_t *path;
  size_t path_len;

  int width;
  int height;

  const Vec2f *uv;
  size_t uv_count;
  // One index per mesh corner, so uv_indices has the same length as the
  // mesh position index list.
  const uint16_t *uv_indices;

  // Linear RGB. Reflectance components must lie in [0, 1), emission
  // components must be non-negative and finite.
  Vec3f reflectance;
  Vec3f emission;
} BakeBinding;

/**
 * Read-only record of one baked instance. The pointers stay valid for the
 * life of the BakeScene and reflect its current round.
 */
typedef struct {
  const size_t *path;
  size_t path_len;

  int width;
  int height;

  // width*height entries; true where a positive-area face covers the texel.
  const bool *covered;
  // width*height entries; original face number of the covering face, -1 on
  // uncovered texels.
  const int *face_owner;
  // width*height entries; outgoing radiance L, zero on uncovered texels.
  const Vec3f *radiance;
} BakeInstanceView;

/**
 * Freezes a visible Entity subtree into a new owned BakeScene.
 *
 * Every visible Object must be bound exactly once; invisible subtrees are
 * ignored. Only Entity and Object renderables are supported. On failure
 * *out is left untouched and nothing is allocated through it.
 *
 * Returns 0 on success, non-zero on any validation or allocation failure.
 */
int bake_scene_build(BakeScene **out, Entity *root,
                     const BakeBinding *bindings, size_t binding_count);

void bake_scene_free(BakeScene *scene);

/**
 * Runs `rounds` fixed-sample diffuse transport rounds.
 *
 * Each covered texel shoots S cosine-weighted hemisphere rays built from the
 * specified radical-inverse sequence; misses take the constant environment
 * radiance, back-face hits absorb and front-face hits sample the previous
 * round's L with a clamp-to-edge nearest texel lookup. The whole round reads
 * a single immutable copy of L, so stepping 1+1 rounds is identical to
 * stepping 2 at once. The environment only affects later rounds and never
 * resets the scene.
 *
 * Invalid arguments (null scene, rounds < 1, S < 1, non-positive/non-finite
 * epsilon or non-finite/negative environment) advance no round. Returns 0 on
 * success.
 */
int bake_step(BakeScene *scene, int rounds, int S, float epsilon,
              Vec3f environment);

/** Restores every covered texel's L to its emission and the round count to 0. */
int bake_reset(BakeScene *scene);

/** Number of rounds applied since the last reset. */
int bake_rounds(const BakeScene *scene);

size_t bake_instance_count(const BakeScene *scene);

/** Returns the view of instance `index`, or NULL when out of range. */
const BakeInstanceView *bake_instance_view(const BakeScene *scene,
                                           size_t index);

/**
 * Draws the live Entity/Object tree through the renderer, shading it with the
 * baked radiance instead of the fixed directional light.
 *
 * The live tree must match the snapshot path for path, face for face and
 * transform for transform; material base textures may be replaced. Secondary
 * UVs are interpolated with the original perspective weights and sampled
 * nearest. Final RGB is base/255 * L * exposure, clamped to [0,1] and
 * truncated to bytes; alpha follows the base texture. Changing cameras needs
 * no re-baking.
 *
 * The renderer must have clear == true. All validation and drawing happens on
 * private buffers; only on success is the backend's beforeRender called, the
 * current backend buffers fetched, color and depth copied, and afterRender
 * called. A failed call never publishes a half frame and never calls back
 * into the backend. Returns 0 on success.
 */
int renderer_render_lightmapped(Renderer *renderer, const BakeScene *scene,
                                float exposure);
