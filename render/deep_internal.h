#pragma once

/**
 * Bridge between the scene graph (Entity/Object) and the deep capture.
 *
 * The deep frame is built by walking the same renderables and running the same
 * Object triangle setup, then emitting every covered fragment instead of
 * letting the depth buffer discard it.  Nothing here is part of the public
 * API.
 */

#include "entity.h"
#include "fwd.h"
#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "material.h"
#include "object.h"
#include "pixel.h"
#include "renderable.h"

typedef struct DeepCapture DeepCapture;

// Render entry points set by object_init and entity_init.  The capture
// dispatches by comparing the renderable's render function against these.
extern int object_render(void *this, Mat4 transform, Renderer *renderer);
extern int entity_render(void *this, Mat4 transform, Renderer *renderer);

/**
 * Records one covered fragment.  The colour is already evaluated (texture
 * sample darkened by the existing diffuse term) and the transmission
 * description is borrowed only for the duration of the call; the current
 * perspective UV is passed so the optional offset map can be evaluated now.
 */
int deep_capture_emit(DeepCapture *capture, int x, int y, float depth,
                      Vec3f color, const ScreenTransmission *transmission,
                      Vec2f uv, int has_uv);

/** Visits an Entity subtree with the same transform composition as render. */
int entity_capture_layers(Entity *entity, Mat4 transform, Renderer *renderer,
                          DeepCapture *capture);

/** Rasterizes every covered fragment of an Object using its render path. */
int object_capture_layers(Object *object, Mat4 transform, Renderer *renderer,
                          DeepCapture *capture);

/** Dispatches on the renderable's own render function. */
int deep_capture_renderable(Renderable *renderable, Mat4 transform,
                            Renderer *renderer, DeepCapture *capture);

/** Local colour convention: a Pixel's RGB bytes divided by 255. */
Vec3f deep_pixel_to_linear(Pixel pixel);
