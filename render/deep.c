#include "deep.h"

#include "backend.h"
#include "deep_internal.h"
#include "math/mat4.h"
#include "render/renderer.h"
#include "render/state.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct DeepFragment {
  float depth;
  Vec3f color;   // local colour, linear RGB in [0, 1]
  Vec3f tint;    // only meaningful for a transmissive fragment
  Vec2f offset;  // already evaluated offset-map pixels
  float opacity; // 1 for an opaque fragment
  int opaque;
  int next; // next fragment for the same pixel, or -1
} DeepFragment;

struct DeepFrame {
  Vec2i size;
  DeepFragment *fragments;
  size_t count;
  size_t capacity;
  int *heads; // one per pixel, index into fragments, or -1
};

struct DeepCapture {
  DeepFrame *frame;
  int failed;
};

// ---------------------------------------------------------------------------
// Pixel channel access
//
// A Pixel's channels are a compile-time choice.  Deep capture needs the raw
// RGB bytes both to linearize a local colour and to build the offset map's
// signed displacement.
// ---------------------------------------------------------------------------

static void pixel_rgb_bytes(Pixel p, unsigned char *r, unsigned char *g,
                            unsigned char *b) {
#ifdef PINGO_PIXEL_UINT8
  *r = p.g;
  *g = p.g;
  *b = p.g;
#elif defined(PINGO_PIXEL_RGB565)
  *r = (unsigned char)(p.red << 3);
  *g = (unsigned char)(p.green << 2);
  *b = (unsigned char)(p.blue << 3);
#elif defined(PINGO_PIXEL_RGB888) || defined(PINGO_PIXEL_RGBA8888) ||          \
    defined(PINGO_PIXEL_BGRA8888)
  *r = p.r;
  *g = p.g;
  *b = p.b;
#endif
}

Vec3f deep_pixel_to_linear(Pixel p) {
  unsigned char r = 0, g = 0, b = 0;
  pixel_rgb_bytes(p, &r, &g, &b);
  return (Vec3f){r / 255.0f, g / 255.0f, b / 255.0f};
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

static DeepFrame *deep_frame_create(Vec2i size) {
  if (size.x <= 0 || size.y <= 0)
    return 0;

  DeepFrame *frame = calloc(1, sizeof(*frame));
  if (frame == 0)
    return 0;

  frame->size = size;

  const size_t pixels = (size_t)size.x * (size_t)size.y;
  frame->heads = malloc(pixels * sizeof(*frame->heads));
  if (frame->heads == 0) {
    free(frame);
    return 0;
  }
  for (size_t i = 0; i < pixels; i++)
    frame->heads[i] = -1;

  return frame;
}

static int deep_frame_add(DeepFrame *frame, int x, int y, float depth, Vec3f color,
                          const ScreenTransmission *transmission, Vec2f uv,
                          int has_uv) {
  if (frame->count == frame->capacity) {
    size_t capacity = frame->capacity == 0 ? 256 : frame->capacity * 2;
    DeepFragment *grown =
        realloc(frame->fragments, capacity * sizeof(*frame->fragments));
    if (grown == 0)
      return 1;
    frame->fragments = grown;
    frame->capacity = capacity;
  }

  DeepFragment *fragment = &frame->fragments[frame->count];
  fragment->depth = depth;
  fragment->color = color;
  fragment->opaque = 1;
  fragment->opacity = 1.0f;
  fragment->tint = (Vec3f){0, 0, 0};
  fragment->offset = (Vec2f){0, 0};

  if (transmission != 0) {
    fragment->opaque = 0;
    fragment->opacity = transmission->opacity;
    fragment->tint = transmission->tint;

    Vec2f offset = transmission->offset;
    if (transmission->offset_map != 0 && has_uv) {
      // The same perspective UV and the same repeat lookup the colour texture
      // uses.  Red and green select a signed displacement multiplier.
      Pixel map =
          texture_readF(transmission->offset_map, (Vec2f){uv.x, uv.y});
      unsigned char r = 0, g = 0, b = 0;
      pixel_rgb_bytes(map, &r, &g, &b);
      (void)b;
      const float mx = 2.0f * (float)r / 255.0f - 1.0f;
      const float my = 2.0f * (float)g / 255.0f - 1.0f;
      offset.x *= mx;
      offset.y *= my;
    }
    fragment->offset = offset;
  }

  const size_t pixel = (size_t)x + (size_t)y * (size_t)frame->size.x;
  fragment->next = frame->heads[pixel];
  frame->heads[pixel] = (int)frame->count;
  frame->count++;

  return 0;
}

int deep_capture_emit(DeepCapture *capture, int x, int y, float depth,
                      Vec3f color, const ScreenTransmission *transmission,
                      Vec2f uv, int has_uv) {
  IF_NULL_RETURN(capture, RENDER_ERROR);

  DeepFrame *frame = capture->frame;
  if (frame == 0)
    return RENDER_ERROR;

  if (capture->failed)
    return RENDER_ERROR;

  if (x < 0 || y < 0 || x >= frame->size.x || y >= frame->size.y)
    return OK;

  // The input meshes are assumed non-degenerate, in the frustum and with
  // positive clip w; a fragment whose interpolated depth does not land in the
  // supported (0, 1] range is simply not a layer.
  if (!isfinite(depth) || depth <= 0.0f || depth > 1.0f)
    return OK;

  if (deep_frame_add(frame, x, y, depth, color, transmission, uv, has_uv)) {
    capture->failed = 1;
    return RENDER_ERROR;
  }

  return OK;
}

int deep_capture_renderable(Renderable *renderable, Mat4 transform,
                            Renderer *renderer, DeepCapture *capture) {
  IF_NULL_RETURN(renderable, RENDER_ERROR);
  IF_NULL_RETURN(renderable->render, RENDER_ERROR);
  IF_NULL_RETURN(capture, RENDER_ERROR);

  // Entity and Object contribute the geometry a deep frame can hold.  Other
  // renderables (sprites) carry no depth-bearing layer and are ignored, which
  // leaves the ordinary renderer's output untouched.
  if (renderable->render == &entity_render)
    return entity_capture_layers((Entity *)renderable, transform, renderer,
                                 capture);

  if (renderable->render == &object_render)
    return object_capture_layers((Object *)renderable, transform, renderer,
                                 capture);

  return OK;
}

int renderer_capture_deep(Renderer *renderer, DeepFrame **out) {
  IF_NULL_RETURN(renderer, RENDER_ERROR);
  IF_NULL_RETURN(out, RENDER_ERROR);
  IF_NULL_RETURN(renderer->root_renderable, RENDER_ERROR);

  DeepFrame *frame = deep_frame_create(renderer->framebuffer.size);
  if (frame == 0)
    return RENDER_ERROR;

  DeepCapture capture;
  capture.frame = frame;
  capture.failed = 0;

  const int status = deep_capture_renderable(renderer->root_renderable,
                                             mat4Identity(), renderer, &capture);

  if (status != OK || capture.failed) {
    deep_frame_destroy(frame);
    return RENDER_ERROR;
  }

  // Nothing has been written to the renderer or the backend: only on success
  // does the caller's output pointer change.
  *out = frame;
  return OK;
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

// The heads are pushed on capture, so walking a pixel's list visits the last
// submitted fragment first.  d = -NDC z, so the nearest surface is the one
// with the largest d; among layers strictly behind `upper`, the nearest is the
// largest d below it.  Traversing last-submitted first means keeping the first
// one seen on a tie is exactly "last submitted wins".
static const DeepFragment *deep_nearest(const DeepFrame *frame, int x, int y,
                                        float upper) {
  if (x < 0 || y < 0 || x >= frame->size.x || y >= frame->size.y)
    return 0;

  const size_t pixel = (size_t)x + (size_t)y * (size_t)frame->size.x;
  const DeepFragment *best = 0;
  for (int index = frame->heads[pixel]; index >= 0;
       index = frame->fragments[index].next) {
    const DeepFragment *fragment = &frame->fragments[index];
    if (!(fragment->depth < upper))
      continue;
    if (best == 0 || fragment->depth > best->depth)
      best = fragment;
  }
  return best;
}

static int deep_sample_impl(const DeepFrame *frame, int x, int y, float upper,
                            Vec3f *out) {
  const DeepFragment *layer = deep_nearest(frame, x, y, upper);
  if (layer == 0) {
    *out = (Vec3f){0, 0, 0};
    return OK;
  }

  if (layer->opaque) {
    *out = layer->color;
    return OK;
  }

  // Bilinear lookup behind this layer.  Each of the four integer pixels runs
  // its own descent with this layer's depth as the upper bound; weights that
  // are zero are skipped, out-of-image taps are black, and nothing is clamped
  // or renormalized.  Because every descent uses a strictly smaller bound, the
  // chain itself is the termination condition - no fixed layer count.
  const float fx = (float)x + layer->offset.x;
  const float fy = (float)y + layer->offset.y;
  const float baseX = floorf(fx);
  const float baseY = floorf(fy);
  const float wx = fx - baseX;
  const float wy = fy - baseY;

  const float cornerX[4] = {baseX, baseX + 1.0f, baseX, baseX + 1.0f};
  const float cornerY[4] = {baseY, baseY, baseY + 1.0f, baseY + 1.0f};
  const float weights[4] = {(1.0f - wx) * (1.0f - wy), wx * (1.0f - wy),
                            (1.0f - wx) * wy, wx * wy};

  Vec3f behind = {0, 0, 0};
  for (int i = 0; i < 4; i++) {
    if (weights[i] == 0.0f)
      continue;
    // A tap outside the image is black and its weight is not redistributed.
    // Comparing in floating point also keeps the conversion to int in range
    // for extreme (but finite) offsets.
    if (cornerX[i] < 0.0f || cornerY[i] < 0.0f ||
        cornerX[i] >= (float)frame->size.x ||
        cornerY[i] >= (float)frame->size.y)
      continue;
    Vec3f tap = {0, 0, 0};
    if (deep_sample_impl(frame, (int)cornerX[i], (int)cornerY[i], layer->depth,
                         &tap) != OK)
      return RENDER_ERROR;
    behind.x += weights[i] * tap.x;
    behind.y += weights[i] * tap.y;
    behind.z += weights[i] * tap.z;
  }

  const float opacity = layer->opacity;
  out->x = opacity * layer->color.x +
           (1.0f - opacity) * layer->tint.x * behind.x;
  out->y = opacity * layer->color.y +
           (1.0f - opacity) * layer->tint.y * behind.y;
  out->z = opacity * layer->color.z +
           (1.0f - opacity) * layer->tint.z * behind.z;

  return OK;
}

int deep_frame_sample(const DeepFrame *frame, int x, int y, float upper,
                      Vec3f *out) {
  IF_NULL_RETURN(frame, RENDER_ERROR);
  IF_NULL_RETURN(out, RENDER_ERROR);

  // upper == +infinity is legal; NaN, negative infinity and non-positive
  // bounds are not.
  if (isnan(upper) || upper <= 0.0f)
    return RENDER_ERROR;

  return deep_sample_impl(frame, x, y, upper, out);
}

// ---------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------

static unsigned char quantize_channel(float value) {
  if (!(value > 0.0f))
    return 0;
  if (value >= 1.0f)
    return 255;
  const double scaled = floor(255.0 * (double)value + 0.5);
  if (scaled <= 0.0)
    return 0;
  if (scaled >= 255.0)
    return 255;
  return (unsigned char)scaled;
}

static uint32_t encode_depth(float depth) {
  const double scaled = floor((double)depth * (double)UINT32_MAX);
  if (!(scaled > 0.0))
    return 0;
  if (scaled >= (double)UINT32_MAX)
    return UINT32_MAX;
  return (uint32_t)scaled;
}

int deep_frame_resolve(const DeepFrame *frame, Texture *target,
                       PingoDepth *depth) {
  IF_NULL_RETURN(frame, RENDER_ERROR);
  IF_NULL_RETURN(target, RENDER_ERROR);
  IF_NULL_RETURN(target->frameBuffer, RENDER_ERROR);
  IF_NULL_RETURN(depth, RENDER_ERROR);

  if (target->size.x != frame->size.x || target->size.y != frame->size.y)
    return RENDER_ERROR;

  for (int y = 0; y < frame->size.y; y++) {
    for (int x = 0; x < frame->size.x; x++) {
      const int pixel = x + y * frame->size.x;

      Vec3f color = {0, 0, 0};
      if (deep_sample_impl(frame, x, y, INFINITY, &color) != OK)
        return RENDER_ERROR;

      // The colour is clamped exactly once, here.
      const unsigned char r = quantize_channel(color.x);
      const unsigned char g = quantize_channel(color.y);
      const unsigned char b = quantize_channel(color.z);
      target->frameBuffer[pixel] = pixelFromRGBA(r, g, b, 255);

      const DeepFragment *front = deep_nearest(frame, x, y, INFINITY);
      depth[pixel].d = front != 0 ? encode_depth(front->depth) : 0;
    }
  }

  return OK;
}

void deep_frame_destroy(DeepFrame *frame) {
  if (frame == 0)
    return;
  free(frame->fragments);
  free(frame->heads);
  free(frame);
}

// ---------------------------------------------------------------------------
// Publishing render
// ---------------------------------------------------------------------------

int renderer_render_deep(Renderer *renderer) {
  IF_NULL_RETURN(renderer, RENDER_ERROR);
  IF_NULL_RETURN(renderer->root_renderable, RENDER_ERROR);
  IF_NULL_RETURN(renderer->backend, RENDER_ERROR);

  if (!renderer->clear)
    return RENDER_ERROR;

  Backend *backend = renderer->backend;
  const Vec2i size = renderer->framebuffer.size;
  const size_t pixels = (size_t)size.x * (size_t)size.y;

  DeepFrame *frame = 0;
  if (renderer_capture_deep(renderer, &frame) != OK)
    return RENDER_ERROR;

  Pixel *color = malloc(pixels * sizeof(*color));
  PingoDepth *depth = malloc(pixels * sizeof(*depth));
  if (color == 0 || depth == 0) {
    free(color);
    free(depth);
    deep_frame_destroy(frame);
    return RENDER_ERROR;
  }

  Texture private_color;
  const int texture_status = texture_init(&private_color, size, color);
  if (texture_status != OK ||
      deep_frame_resolve(frame, &private_color, depth) != OK) {
    free(color);
    free(depth);
    deep_frame_destroy(frame);
    return RENDER_ERROR;
  }

  // Only now, with a complete frame in private buffers, are the backend
  // callbacks involved.  The backend may hand out different buffers here, so
  // they are fetched after beforeRender and the sizes are checked before
  // publishing anything.
  backend->beforeRender(renderer, backend);

  Pixel *backend_color = backend->getFrameBuffer(renderer, backend);
  PingoDepth *backend_depth = backend->getZetaBuffer(renderer, backend);
  if (backend_color == 0 || backend_depth == 0) {
    free(color);
    free(depth);
    deep_frame_destroy(frame);
    return RENDER_ERROR;
  }

  memcpy(backend_color, color, pixels * sizeof(*color));
  memcpy(backend_depth, depth, pixels * sizeof(*depth));
  renderer->framebuffer.frameBuffer = backend_color;

  backend->afterRender(renderer, backend);

  free(color);
  free(depth);
  deep_frame_destroy(frame);
  return OK;
}
