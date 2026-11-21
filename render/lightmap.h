#pragma once

/**
 * Lightmap baking for static scenes.
 *
 * A BakeScene freezes everything the baker needs out of an Entity subtree:
 * the world-space triangles of every visible Object instance, a constant
 * linear-RGB reflectance/emission per instance, and an independent set of
 * per-corner lightmap UVs bound through BakeBinding. The scene owns deep
 * copies of all of it, so the source entities, meshes, bindings and
 * materials may be released right after bake_scene_build() returns.
 *
 * Instances are addressed by entity path: the sequence of child indices
 * from the root entity down to the entity holding the Object. The empty
 * path addresses the root entity itself. The same Object mounted twice is
 * two independent instances with two independent lightmaps.
 *
 * Baking itself (bake_step) gathers irradiance at every covered texel with
 * a fixed set of cosine-weighted hemisphere directions - the same
 * directions every round, so results are deterministic and repeated calls
 * with the same arguments compose exactly like one longer call.
 *
 * renderer_render_lightmapped() draws a live tree through the normal
 * transform/coverage/depth path, but shades each fragment from the baked
 * radiance instead of the fixed directional light.
 */

#include "entity.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "renderer.h"
#include "state.h"
#include <stddef.h>
#include <stdint.h>

/// Binding between one entity path and the bake inputs of that instance.
typedef struct BakeBinding {
  const size_t *path;   // child indices from the root; NULL with length 0
  size_t path_length;   // 0 addresses the root entity itself
  int width;            // lightmap size in texels, both must be > 0
  int height;
  const Vec2f *uvs;     // secondary (lightmap) UVs, each component in [0,1]
  size_t uv_count;
  const uint16_t *uv_indices; // one index per mesh corner (indexes_count)
  Vec3f reflectance;    // constant linear RGB, each component in [0,1)
  Vec3f emission;       // constant linear RGB, each component >= 0
} BakeBinding;

/// One world-space triangle of a baked instance.
typedef struct BakeTri {
  Vec3f p0, p1, p2;     // world-space positions
  Vec3f normal;         // unit world face normal from the transformed winding
  Vec2f uv0, uv1, uv2;  // bound secondary UVs, as submitted
  int instance;         // owning instance index
  int face;             // original face index (corner offset / 3)
} BakeTri;

/// One bound Object instance: frozen geometry plus its lightmap.
typedef struct BakeInstance {
  size_t *path;         // owned copy of the entity path
  size_t path_length;
  Mat4 world;           // frozen world transform
  int width, height;    // lightmap size in texels
  Vec3f reflectance, emission;

  int indexes_count;    // frozen mesh description, for render-time checks
  size_t position_count;
  uint16_t *pos_indices; // owned
  Vec3f *positions;      // owned local-space positions
  Vec2f *uvs;            // owned secondary UVs
  size_t uv_count;
  uint16_t *uv_indices;  // owned

  BakeTri *tris;         // owned, indexes_count/3 entries
  size_t tri_count;

  int *texel_face;       // width*height; owning face, -1 when uncovered
  Vec3f *texel_point;    // width*height; world surface point (covered only)
  Vec3f *radiance;       // width*height; current exit radiance L
  Vec3f *radiance_next;  // width*height; scratch for the next round
} BakeInstance;

/// Owning snapshot of a static scene ready for baking.
typedef struct BakeScene {
  BakeInstance *instances; // sorted by entity path, lexicographic
  size_t instance_count;
  unsigned long rounds;    // bake rounds completed since build/reset
} BakeScene;

/// Record returned for one lightmap texel.
typedef struct BakeTexel {
  int covered;   // 1 when a face covers the texel centre
  int face;      // original face index covering it, -1 when uncovered
  Vec3f radiance; // current exit radiance L (zero when uncovered)
  Vec3f point;    // world surface point at the texel centre (covered only)
  Vec3f normal;   // unit world face normal (covered only)
} BakeTexel;

extern void bake_scene_init(BakeScene *scene);

/**
 * Builds (or replaces) a bake scene from an Entity subtree.
 *
 * Every Object reachable through visible entities must be bound exactly
 * once; invisible subtrees are ignored entirely. On any validation or
 * coverage failure the previous contents of `scene` are kept and a
 * non-zero status is returned.
 */
extern int bake_scene_build(BakeScene *scene, const Entity *root,
                            const BakeBinding *bindings, size_t binding_count);

extern void bake_scene_free(BakeScene *scene);

/**
 * Advances `rounds` bounce rounds with `samples` fixed hemisphere
 * directions per covered texel. Rays start at P + epsilon*N and consider
 * hits with t > epsilon; misses see the constant `environment` radiance.
 * Invalid arguments leave the scene untouched and return non-zero.
 */
extern int bake_step(BakeScene *scene, int rounds, int samples, float epsilon,
                     Vec3f environment);

/// Restores every covered texel to its emission and the round count to 0.
extern void bake_reset(BakeScene *scene);

/// Index of the instance at `path`, or -1 when there is none.
extern int bake_scene_find(const BakeScene *scene, const size_t *path,
                           size_t path_length);

/// Reads one texel record of an instance. Returns non-zero on bad input.
extern int bake_scene_texel(const BakeScene *scene, int instance, int x, int y,
                            BakeTexel *out);

/**
 * Renders the live scene graph shaded by the baked lightmaps.
 *
 * The tree under renderer->root_renderable must still match the snapshot:
 * every visible Object instance is checked against the frozen path, world
 * transform and geometry. The material's base colour texture may be
 * swapped freely; the camera may move freely. renderer->clear must be
 * set. Drawing happens into private buffers first; only on success are
 * the backend callbacks invoked and the finished colour and depth copied
 * into the backend's current buffers, so a failure never publishes a
 * partial frame.
 */
extern int renderer_render_lightmapped(Renderer *renderer,
                                       const BakeScene *scene, float exposure);
