/* RedNoise - a CPU software renderer. Stable C ABI.
 *
 * This is the public, language-agnostic surface of the library: it exposes no
 * C++ or glm types, so it binds cleanly from C, Rust, Python, Go, etc. Link the
 * `rednoise` library and include this header.
 */
#ifndef REDNOISE_H
#define REDNOISE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RN_VERSION_MAJOR 0
#define RN_VERSION_MINOR 1
#define RN_VERSION_PATCH 2

/* An opaque loaded scene. */
typedef struct rn_scene rn_scene;

/* Which renderer to run. */
typedef enum { RN_WIREFRAME = 0, RN_RASTERISED = 1, RN_RAYTRACED = 2, RN_PATHTRACED = 3 } rn_render_mode;

/* Load a Wavefront OBJ (and its .mtl) as a scene, scaling vertices by `scale`.
 * Returns NULL on failure. Free the result with rn_scene_free. */
rn_scene *rn_scene_load_obj(const char *path, float scale);

/* Release a scene returned by rn_scene_load_obj (NULL is ignored). */
void rn_scene_free(rn_scene *scene);

/* Number of triangles in the scene (0 if scene is NULL). */
int rn_scene_triangle_count(const rn_scene *scene);

/* Render `scene` into `rgba` (a caller-owned buffer of width*height*4 bytes,
 * RGBA8) using a camera at (0, 0, cam_z) aimed at the origin. `samples` is the
 * per-pixel sample count for RN_PATHTRACED (ignored by the other modes).
 * Returns 1 on success, 0 on invalid arguments. */
int rn_render(const rn_scene *scene, rn_render_mode mode, int width, int height, float cam_z, int samples,
              unsigned char *rgba);

/* Write an RGBA8 buffer (width*height*4 bytes, as filled by rn_render) to a PNG
 * file. Returns 1 on success, 0 on failure. */
int rn_save_png(const char *path, int width, int height, const unsigned char *rgba);

/* Library version as a string, e.g. "0.1.0". */
const char *rn_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* REDNOISE_H */
