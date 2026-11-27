//
// Copyright (c) 2013 Mikko Mononen memon@inside.org
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

// CPU (off-screen, no window or GPU) rasterizer backend for NanoVG.
//
// NanoVG already tessellates paths, strokes and glyphs into triangles and
// hands them to the backend through the NVGparams callbacks. This backend
// rasterizes that exact geometry on the CPU:
//
//  * edgeAntiAlias is disabled, so coverage is evaluated at the centers of a
//    regular 4x4 sub-pixel grid (16 samples per physical pixel);
//  * fills combine all sub-paths with the non-zero winding rule, so concave
//    shapes, holes, multiple contours and self intersections work;
//  * strokes merge the triangle strips of one renderStroke call into a
//    coverage union (a sub-pixel is painted at most once per call), while
//    separate stroke calls still blend in call order;
//  * renderTriangles uses a half-open shared edge rule so the diagonals of
//    glyph quads are never covered twice;
//  * paints reproduce the nanovg_gl.h box-distance gradient, premultiplied
//    inner/outer colors and texture tint formulas.
//
// The borrowed target is only read at nvgBeginFrame() and written at
// nvgEndFrame(); everything in between accumulates in a private float buffer,
// so nvgCancelFrame() can discard a whole frame and texture updates or
// deletions cannot affect draws already rasterized.

#include "nanovg_cpu.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CPU_SUBS 4
#define CPU_NSAMP (CPU_SUBS * CPU_SUBS)

typedef struct CPUtexture CPUtexture;
struct CPUtexture {
	int id;
	int type;			// NVG_TEXTURE_ALPHA or NVG_TEXTURE_RGBA
	int width, height;
	int flags;			// NVG_IMAGE_*
	int used;
	unsigned char* data;	// owned copy, always 4 bytes per pixel
};

typedef struct CPUcontext CPUcontext;
struct CPUcontext {
	unsigned char* target;
	int width, height, stride;

	// Accumulated, premultiplied frame buffer, 4 floats per pixel.
	float* frame;

	// Rasterizer scratch storage, grown on demand to the largest bbox.
	int scratchCap;		// in samples (pixels * CPU_NSAMP)
	int* winding;		// signed winding per sample (fills)
	unsigned char* mask;	// coverage union per sample (strokes/tris)
	float* uv;			// barycentric u,v per sample (tris)

	// Textures owned by this context.
	CPUtexture* textures;
	int ntextures, ctextures;
	int nextId;
};

// ----------------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------------

static float cpu_clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float cpu_maxf(float a, float b) { return a > b ? a : b; }
static float cpu_minf(float a, float b) { return a < b ? a : b; }

static void cpu_premul(float c[4])
{
	c[0] *= c[3];
	c[1] *= c[3];
	c[2] *= c[3];
}

static CPUtexture* cpu_findTexture(CPUcontext* cpu, int id)
{
	int i;
	for (i = 0; i < cpu->ntextures; i++) {
		if (cpu->textures[i].used && cpu->textures[i].id == id)
			return &cpu->textures[i];
	}
	return NULL;
}

// Reload the accumulation buffer from the borrowed target bytes.
static void cpu_loadTarget(CPUcontext* cpu)
{
	int y, x;
	for (y = 0; y < cpu->height; y++) {
		const unsigned char* row = cpu->target + (size_t)y * cpu->stride;
		float* dst = cpu->frame + (size_t)y * cpu->width * 4;
		for (x = 0; x < cpu->width; x++) {
			dst[x*4+0] = row[x*4+0] / 255.0f;
			dst[x*4+1] = row[x*4+1] / 255.0f;
			dst[x*4+2] = row[x*4+2] / 255.0f;
			dst[x*4+3] = row[x*4+3] / 255.0f;
		}
	}
}

// Publish the accumulated float frame back into the borrowed target.
static void cpu_storeTarget(CPUcontext* cpu)
{
	int y, x, k;
	for (y = 0; y < cpu->height; y++) {
		unsigned char* row = cpu->target + (size_t)y * cpu->stride;
		const float* src = cpu->frame + (size_t)y * cpu->width * 4;
		for (x = 0; x < cpu->width; x++) {
			for (k = 0; k < 4; k++) {
				float v = cpu_clampf(src[x*4+k], 0.0f, 1.0f);
				row[x*4+k] = (unsigned char)(int)(v * 255.0f + 0.5f);
			}
		}
	}
}

static int cpu_ensureScratch(CPUcontext* cpu, int pixels)
{
	int cap = pixels * CPU_NSAMP;
	if (cap <= cpu->scratchCap) return 1;
	{
		int grown = cpu->scratchCap ? cpu->scratchCap : 256;
		void* p;
		while (grown < cap) grown *= 2;
		p = realloc(cpu->winding, sizeof(int) * (size_t)grown);
		if (p) { cpu->winding = (int*)p; } else return 0;
		p = realloc(cpu->mask, sizeof(unsigned char) * (size_t)grown);
		if (p) { cpu->mask = (unsigned char*)p; } else return 0;
		p = realloc(cpu->uv, sizeof(float) * 2 * (size_t)grown);
		if (p) { cpu->uv = (float*)p; } else return 0;
		cpu->scratchCap = grown;
	}
	return 1;
}

// ----------------------------------------------------------------------------
// Texture management
// ----------------------------------------------------------------------------

static int cpu_renderCreateTexture(void* uptr, int type, int w, int h,
								   int imageFlags, const unsigned char* data)
{
	CPUcontext* cpu = (CPUcontext*)uptr;
	CPUtexture* tex;
	int i, slot = -1, n;

	if (w <= 0 || h <= 0) return 0;

	// Reuse a slot left by a deleted texture before growing the array.
	for (i = 0; i < cpu->ntextures; i++) {
		if (!cpu->textures[i].used) { slot = i; break; }
	}
	if (slot < 0 && cpu->ntextures >= cpu->ctextures) {
		int ncap = cpu->ctextures ? cpu->ctextures * 2 : 4;
		CPUtexture* grown = (CPUtexture*)realloc(cpu->textures,
												 sizeof(CPUtexture) * (size_t)ncap);
		if (!grown) return 0;
		cpu->textures = grown;
		cpu->ctextures = ncap;
	}
	if (slot < 0) slot = cpu->ntextures++;
	tex = &cpu->textures[slot];
	memset(tex, 0, sizeof(*tex));
	tex->id = ++cpu->nextId;
	tex->type = type;
	tex->width = w;
	tex->height = h;
	tex->flags = imageFlags;
	tex->used = 1;
	n = w * h;
	tex->data = (unsigned char*)malloc((size_t)n * 4);
	if (!tex->data) { tex->used = 0; return 0; }

	if (type == NVG_TEXTURE_ALPHA) {
		// Expand the one channel coverage into a four channel premultiplied
		// (a,a,a,a) texel so sampling shares one code path with RGBA images.
		for (i = 0; i < n; i++) {
			unsigned char a = data ? data[i] : 0;
			tex->data[i*4+0] = a;
			tex->data[i*4+1] = a;
			tex->data[i*4+2] = a;
			tex->data[i*4+3] = a;
		}
	} else {
		if (data)
			memcpy(tex->data, data, (size_t)n * 4);
		else
			memset(tex->data, 0, (size_t)n * 4);
	}
	return tex->id;
}

static int cpu_renderDeleteTexture(void* uptr, int image)
{
	CPUcontext* cpu = (CPUcontext*)uptr;
	CPUtexture* tex = cpu_findTexture(cpu, image);
	if (!tex) return 0;
	free(tex->data);
	memset(tex, 0, sizeof(*tex));
	return 1;
}

static int cpu_renderUpdateTexture(void* uptr, int image, int x, int y, int w, int h,
								   const unsigned char* data)
{
	CPUcontext* cpu = (CPUcontext*)uptr;
	CPUtexture* tex = cpu_findTexture(cpu, image);
	int row, col;
	if (!tex || !data) return 0;
	// 'data' is the base address of the whole texture; only the given
	// rectangle is replaced.
	for (row = 0; row < h; row++) {
		int ty = y + row;
		if (ty < 0 || ty >= tex->height) continue;
		for (col = 0; col < w; col++) {
			int tx = x + col;
			if (tx < 0 || tx >= tex->width) continue;
			if (tex->type == NVG_TEXTURE_ALPHA) {
				unsigned char a = data[((size_t)ty * tex->width + tx)];
				unsigned char* dst = tex->data + ((size_t)ty * tex->width + tx) * 4;
				dst[0] = dst[1] = dst[2] = dst[3] = a;
			} else {
				const unsigned char* src =
					data + ((size_t)ty * tex->width + tx) * 4;
				unsigned char* dst =
					tex->data + ((size_t)ty * tex->width + tx) * 4;
				dst[0] = src[0]; dst[1] = src[1];
				dst[2] = src[2]; dst[3] = src[3];
			}
		}
	}
	return 1;
}

static int cpu_renderGetTextureSize(void* uptr, int image, int* w, int* h)
{
	CPUcontext* cpu = (CPUcontext*)uptr;
	CPUtexture* tex = cpu_findTexture(cpu, image);
	if (!tex) return 0;
	*w = tex->width;
	*h = tex->height;
	return 1;
}

// ----------------------------------------------------------------------------
// Frame lifecycle
// ----------------------------------------------------------------------------

static int cpu_renderCreate(void* uptr)
{
	(void)uptr;
	return 1;
}

static void cpu_renderDelete(void* uptr)
{
	CPUcontext* cpu = (CPUcontext*)uptr;
	int i;
	if (!cpu) return;
	for (i = 0; i < cpu->ntextures; i++)
		free(cpu->textures[i].data);
	free(cpu->textures);
	free(cpu->frame);
	free(cpu->winding);
	free(cpu->mask);
	free(cpu->uv);
}

static void cpu_renderViewport(void* uptr, float width, float height,
							   float devicePixelRatio)
{
	CPUcontext* cpu = (CPUcontext*)uptr;
	// The caller guarantees width * devicePixelRatio == physical target size.
	// Coordinates arrive in the same space as the physical pixels, the DPR is
	// never applied a second time.
	(void)width; (void)height; (void)devicePixelRatio;
	cpu_loadTarget(cpu);
}

static void cpu_renderCancel(void* uptr)
{
	CPUcontext* cpu = (CPUcontext*)uptr;
	// Throw away everything accumulated this frame and restore the target's
	// previous contents in the off-screen buffer.
	cpu_loadTarget(cpu);
}

static void cpu_renderFlush(void* uptr)
{
	CPUcontext* cpu = (CPUcontext*)uptr;
	cpu_storeTarget(cpu);
}

// ----------------------------------------------------------------------------
// Geometry / coverage
// ----------------------------------------------------------------------------

// Half-open triangle containment. Edge functions are normalized so the
// interior is always positive; a sample landing exactly on a shared edge is
// assigned to the triangle whose directed edge is a top/left edge. A shared
// edge appears as opposite directions in the two adjacent triangles, so
// exactly one of the two tie-breaks succeeds: the diagonal of a glyph quad is
// never covered twice and there are no gaps. Works for either winding.
static int cpu_inTriangle(float px, float py,
						  float ax, float ay, float bx, float by,
						  float cx, float cy)
{
	float area = (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
	float s;
	if (area == 0.0f) return 0;
	s = area > 0.0f ? 1.0f : -1.0f;

	// Edge a->b
	{
		float w = ((bx-ax)*(py-ay) - (by-ay)*(px-ax)) * s;
		if (w < 0.0f) return 0;
		if (w == 0.0f) {
			int topLeft = (ay == by) ? (ax > bx) : (ay < by);
			if (!topLeft) return 0;
		}
	}
	// Edge b->c
	{
		float w = ((cx-bx)*(py-by) - (cy-by)*(px-bx)) * s;
		if (w < 0.0f) return 0;
		if (w == 0.0f) {
			int topLeft = (by == cy) ? (bx > cx) : (by < cy);
			if (!topLeft) return 0;
		}
	}
	// Edge c->a
	{
		float w = ((ax-cx)*(py-cy) - (ay-cy)*(px-cx)) * s;
		if (w < 0.0f) return 0;
		if (w == 0.0f) {
			int topLeft = (cy == ay) ? (cx > ax) : (cy < ay);
			if (!topLeft) return 0;
		}
	}
	return 1;
}

// Signed crossing contribution of one oriented edge to the winding number.
// The geometry lives in NanoVG's y-down screen space, so the textbook y-up
// crossing rule has its signs flipped. The triangle fans keep their contour's
// orientation, hence the summed winding already distinguishes solid (CCW)
// contours from holes (CW); no extra winding multiplier is applied.
static void cpu_edgeWinding(float ax, float ay, float bx, float by,
							float px, float py, int* wn)
{
	float cross = (bx-ax)*(py-ay) - (by-ay)*(px-ax);
	if (ay <= py && by > py) {
		// Downward pointing edge crossing the sample's horizontal line.
		if (cross > 0.0f) (*wn)--;
	} else if (ay > py && by <= py) {
		// Upward pointing edge crossing the sample's horizontal line.
		if (cross < 0.0f) (*wn)++;
	}
}

typedef struct {
	float x0, y0, x1, y1;
} CPUbbox;

static void cpu_unionBbox(CPUbbox* bb, float x, float y)
{
	if (x < bb->x0) bb->x0 = x;
	if (y < bb->y0) bb->y0 = y;
	if (x > bb->x1) bb->x1 = x;
	if (y > bb->y1) bb->y1 = y;
}

// ----------------------------------------------------------------------------
// Paints
// ----------------------------------------------------------------------------

typedef struct {
	int isImage;
	float invxform[6];
	float extent[2];
	float radius, feather;
	float inner[4];		// premultiplied
	float outer[4];		// premultiplied
	CPUtexture* tex;
	int texType;		// 0 = premult RGBA, 1 = straight RGBA, 2 = alpha
	int hasScissor;
	float scissorInv[6];
	float scissorExt[2];
} CPUpaint;

static float cpu_sdRoundRect(float px, float py, float extx, float exty, float rad)
{
	float dx = fabsf(px) - (extx - rad);
	float dy = fabsf(py) - (exty - rad);
	float ox = cpu_maxf(dx, 0.0f);
	float oy = cpu_maxf(dy, 0.0f);
	// Signed distance: negative (interior) term plus the outside offset,
	// matching sdroundrect() in nanovg_gl.h.
	float inside = cpu_minf(cpu_maxf(dx, dy), 0.0f);
	return inside + sqrtf(ox*ox + oy*oy) - rad;
}

static int cpu_scissorInside(const CPUpaint* p, float x, float y)
{
	float qx, qy;
	if (!p->hasScissor) return 1;
	qx = p->scissorInv[0]*x + p->scissorInv[2]*y + p->scissorInv[4];
	qy = p->scissorInv[1]*x + p->scissorInv[3]*y + p->scissorInv[5];
	return fabsf(qx) <= p->scissorExt[0] && fabsf(qy) <= p->scissorExt[1];
}

// Wrap a normalized coordinate into [0,1) for repeat or clamp it to [0,1].
static float cpu_wrapCoord(float u, int repeat)
{
	if (repeat) {
		float f = u - floorf(u);
		// Guard against rounding landing exactly on 1.0.
		if (f >= 1.0f) f = 0.0f;
		return f;
	}
	return cpu_clampf(u, 0.0f, 1.0f);
}

static int cpu_wrapIndex(int i, int size, int repeat)
{
	if (repeat) {
		int m = i % size;
		if (m < 0) m += size;
		return m;
	}
	return cpu_clampf((float)i, 0, (float)size - 1);
}

// Fetch one premultiplied RGBA texel (components in [0,1]).
static void cpu_texel(const CPUtexture* tex, int ix, int iy, float out[4])
{
	const unsigned char* p = tex->data +
		((size_t)iy * tex->width + ix) * 4;
	out[0] = p[0] / 255.0f;
	out[1] = p[1] / 255.0f;
	out[2] = p[2] / 255.0f;
	out[3] = p[3] / 255.0f;
}

static void cpu_lerp4(float out[4], const float a[4], const float b[4], float t)
{
	int k;
	for (k = 0; k < 4; k++) out[k] = a[k] + (b[k] - a[k]) * t;
}

// Sample a texture at normalized uv, honoring nearest / texel-center bilinear
// filtering and REPEATX / REPEATY. Non power of two sizes are fine; mipmaps are
// not supported (NVG_IMAGE_GENERATE_MIPMAPS is ignored).
static void cpu_sampleTexture(const CPUtexture* tex, float u, float v,
							  float out[4])
{
	int rx = (tex->flags & NVG_IMAGE_REPEATX) != 0;
	int ry = (tex->flags & NVG_IMAGE_REPEATY) != 0;
	int w = tex->width, h = tex->height;

	if (tex->flags & NVG_IMAGE_NEAREST) {
		int ix, iy;
		u = cpu_wrapCoord(u, rx);
		v = cpu_wrapCoord(v, ry);
		ix = cpu_wrapIndex((int)floorf(u * w), w, rx);
		iy = cpu_wrapIndex((int)floorf(v * h), h, ry);
		cpu_texel(tex, ix, iy, out);
	} else {
		float gx, gy, fx, fy, c00[4], c01[4], c10[4], c11[4], top[4], bot[4];
		int x0, y0, x1, y1;
		u = cpu_wrapCoord(u, rx);
		v = cpu_wrapCoord(v, ry);
		// Bilinear weighting around texel centers.
		gx = u * w - 0.5f;
		gy = v * h - 0.5f;
		x0 = (int)floorf(gx); fx = gx - x0;
		y0 = (int)floorf(gy); fy = gy - y0;
		x1 = x0 + 1; y1 = y0 + 1;
		cpu_texel(tex, cpu_wrapIndex(x0,w,rx), cpu_wrapIndex(y0,h,ry), c00);
		cpu_texel(tex, cpu_wrapIndex(x1,w,rx), cpu_wrapIndex(y0,h,ry), c10);
		cpu_texel(tex, cpu_wrapIndex(x0,w,rx), cpu_wrapIndex(y1,h,ry), c01);
		cpu_texel(tex, cpu_wrapIndex(x1,w,rx), cpu_wrapIndex(y1,h,ry), c11);
		cpu_lerp4(top, c00, c10, fx);
		cpu_lerp4(bot, c01, c11, fx);
		cpu_lerp4(out, top, bot, fy);
	}

	// Stored RGBA textures may carry straight (non premultiplied) alpha;
	// alpha textures were expanded to (a,a,a,a) and need no conversion.
	if (tex->type == NVG_TEXTURE_RGBA &&
		(tex->flags & NVG_IMAGE_PREMULTIPLIED) == 0) {
		out[0] *= out[3];
		out[1] *= out[3];
		out[2] *= out[3];
	}
}

// Evaluate the paint at logical position (x,y); uv is used by textured
// triangles and ignored for fills and strokes.
static void cpu_paintColor(const CPUpaint* p, float x, float y,
						   float u, float v, float out[4])
{
	if (!p->isImage) {
		float px, py, d, t;
		px = p->invxform[0]*x + p->invxform[2]*y + p->invxform[4];
		py = p->invxform[1]*x + p->invxform[3]*y + p->invxform[5];
		if (p->feather > 0.0f) {
			d = (cpu_sdRoundRect(px, py, p->extent[0], p->extent[1],
								 p->radius) + p->feather * 0.5f) / p->feather;
			t = cpu_clampf(d, 0.0f, 1.0f);
		} else {
			t = cpu_sdRoundRect(px, py, p->extent[0], p->extent[1],
								p->radius) < 0.0f ? 0.0f : 1.0f;
		}
		out[0] = p->inner[0] + (p->outer[0] - p->inner[0]) * t;
		out[1] = p->inner[1] + (p->outer[1] - p->inner[1]) * t;
		out[2] = p->inner[2] + (p->outer[2] - p->inner[2]) * t;
		out[3] = p->inner[3] + (p->outer[3] - p->inner[3]) * t;
	} else {
		float px, py, tex[4];
		int k;
		if (p->texType == 2) {
			// Textured triangles carry texture coordinates directly.
			cpu_sampleTexture(p->tex, u, v, tex);
		} else {
			px = p->invxform[0]*x + p->invxform[2]*y + p->invxform[4];
			py = p->invxform[1]*x + p->invxform[3]*y + p->invxform[5];
			cpu_sampleTexture(p->tex,
							  px / p->extent[0], py / p->extent[1], tex);
		}
		// Texture tint (premultiplied inner color).
		for (k = 0; k < 4; k++) out[k] = tex[k] * p->inner[k];
	}
}

// ----------------------------------------------------------------------------
// Blending
// ----------------------------------------------------------------------------

static float cpu_blendFactor(int factor, int isAlpha,
							 const float src[4], const float dst[4])
{
	switch (factor) {
	case NVG_ZERO: return 0.0f;
	case NVG_ONE: return 1.0f;
	case NVG_SRC_COLOR:
		return isAlpha ? src[3] : src[0];
	case NVG_ONE_MINUS_SRC_COLOR:
		return 1.0f - (isAlpha ? src[3] : src[0]);
	case NVG_DST_COLOR:
		return isAlpha ? dst[3] : dst[0];
	case NVG_ONE_MINUS_DST_COLOR:
		return 1.0f - (isAlpha ? dst[3] : dst[0]);
	case NVG_SRC_ALPHA: return src[3];
	case NVG_ONE_MINUS_SRC_ALPHA: return 1.0f - src[3];
	case NVG_DST_ALPHA: return dst[3];
	case NVG_ONE_MINUS_DST_ALPHA: return 1.0f - dst[3];
	case NVG_SRC_ALPHA_SATURATE:
		// Only defined for the RGB source factor; alpha uses ONE.
		return isAlpha ? 1.0f : cpu_minf(src[3], 1.0f - dst[3]);
	default: return 1.0f;
	}
}

static void cpu_blend(CPUcontext* cpu, int px, int py,
					  const float src[4],
					  const NVGcompositeOperationState* op)
{
	float* dst = cpu->frame + ((size_t)py * cpu->width + px) * 4;
	float s[4], d[4], out[4];
	float sR, dR, sA, dA;
	int k;
	for (k = 0; k < 4; k++) {
		s[k] = cpu_clampf(src[k], 0.0f, 1.0f);
		d[k] = cpu_clampf(dst[k], 0.0f, 1.0f);
	}
	// RGB and alpha factors are evaluated separately.
	sR = cpu_blendFactor(op->srcRGB, 0, s, d);
	dR = cpu_blendFactor(op->dstRGB, 0, s, d);
	sA = cpu_blendFactor(op->srcAlpha, 1, s, d);
	dA = cpu_blendFactor(op->dstAlpha, 1, s, d);
	out[0] = cpu_clampf(s[0]*sR + d[0]*dR, 0.0f, 1.0f);
	out[1] = cpu_clampf(s[1]*sR + d[1]*dR, 0.0f, 1.0f);
	out[2] = cpu_clampf(s[2]*sR + d[2]*dR, 0.0f, 1.0f);
	out[3] = cpu_clampf(s[3]*sA + d[3]*dA, 0.0f, 1.0f);
	for (k = 0; k < 4; k++) dst[k] = out[k];
}

// ----------------------------------------------------------------------------
// Paint setup
// ----------------------------------------------------------------------------

static void cpu_initPaint(CPUcontext* cpu, CPUpaint* pe, const NVGpaint* paint,
						  const NVGscissor* scissor)
{
	memset(pe, 0, sizeof(*pe));
	pe->inner[0] = paint->innerColor.r;
	pe->inner[1] = paint->innerColor.g;
	pe->inner[2] = paint->innerColor.b;
	pe->inner[3] = paint->innerColor.a;
	pe->outer[0] = paint->outerColor.r;
	pe->outer[1] = paint->outerColor.g;
	pe->outer[2] = paint->outerColor.b;
	pe->outer[3] = paint->outerColor.a;
	cpu_premul(pe->inner);
	cpu_premul(pe->outer);
	pe->extent[0] = paint->extent[0];
	pe->extent[1] = paint->extent[1];
	pe->radius = paint->radius;
	pe->feather = paint->feather;

	if (scissor && scissor->extent[0] >= -0.5f && scissor->extent[1] >= -0.5f) {
		pe->hasScissor = 1;
		pe->scissorExt[0] = scissor->extent[0];
		pe->scissorExt[1] = scissor->extent[1];
		nvgTransformInverse(pe->scissorInv, scissor->xform);
	}

	if (paint->image != 0) {
		CPUtexture* tex = cpu_findTexture(cpu, paint->image);
		pe->isImage = 1;
		pe->tex = tex;
		if (tex) {
			if (tex->type == NVG_TEXTURE_RGBA)
				pe->texType = (tex->flags & NVG_IMAGE_PREMULTIPLIED) ? 0 : 1;
			else
				pe->texType = 2;
			if (tex->flags & NVG_IMAGE_FLIPY) {
				// Match nanovg_gl.h: fold the Y flip into the paint matrix.
				float m1[6], m2[6];
				nvgTransformTranslate(m1, 0.0f, pe->extent[1] * 0.5f);
				nvgTransformMultiply(m1, paint->xform);
				nvgTransformScale(m2, 1.0f, -1.0f);
				nvgTransformMultiply(m2, m1);
				nvgTransformTranslate(m1, 0.0f, -pe->extent[1] * 0.5f);
				nvgTransformMultiply(m1, m2);
				nvgTransformInverse(pe->invxform, m1);
			} else {
				nvgTransformInverse(pe->invxform, paint->xform);
			}
		}
	} else {
		pe->isImage = 0;
		nvgTransformInverse(pe->invxform, paint->xform);
	}
}

// Composite the covered samples of one rasterized callback into the frame.
static void cpu_composite(CPUcontext* cpu, const CPUpaint* pe,
						  const NVGcompositeOperationState* op,
						  int bx, int by, int bw, int bh,
						  int useUV)
{
	int py, px, si;
	for (py = 0; py < bh; py++) {
		int wy = by + py;
		if (wy < 0 || wy >= cpu->height) continue;
		for (px = 0; px < bw; px++) {
			int wx = bx + px;
			int base = (py * bw + px) * CPU_NSAMP;
			int count = 0;
			float acc[4] = {0,0,0,0};
			if (wx < 0 || wx >= cpu->width) continue;
			for (si = 0; si < CPU_NSAMP; si++) {
				float sx, sy, col[4];
				if (!cpu->mask[base + si]) continue;
				sx = (float)wx + ((si % CPU_SUBS) + 0.5f) / CPU_SUBS;
				sy = (float)wy + ((si / CPU_SUBS) + 0.5f) / CPU_SUBS;
				if (!cpu_scissorInside(pe, sx, sy)) continue;
				if (useUV)
					cpu_paintColor(pe, sx, sy,
								   cpu->uv[(base+si)*2+0],
								   cpu->uv[(base+si)*2+1], col);
				else
					cpu_paintColor(pe, sx, sy, 0.0f, 0.0f, col);
				acc[0] += col[0]; acc[1] += col[1];
				acc[2] += col[2]; acc[3] += col[3];
				count++;
			}
			if (count) {
				float src[4];
				// Uncovered samples contribute zero, so dividing by the full
				// 16-sample grid folds the coverage (count/16) into the
				// premultiplied source color and alpha.
				src[0] = acc[0] / CPU_NSAMP;
				src[1] = acc[1] / CPU_NSAMP;
				src[2] = acc[2] / CPU_NSAMP;
				src[3] = acc[3] / CPU_NSAMP;
				cpu_blend(cpu, wx, wy, src, op);
			}
		}
	}
}

// ----------------------------------------------------------------------------
// Backend callbacks
// ----------------------------------------------------------------------------

static void cpu_renderFill(void* uptr, NVGpaint* paint,
						   NVGcompositeOperationState compositeOperation,
						   NVGscissor* scissor, float fringe,
						   const float* bounds, const NVGpath* paths, int npaths)
{
	CPUcontext* cpu = (CPUcontext*)uptr;
	CPUpaint pe;
	CPUbbox bb;
	int bx, by, bw, bh, np, pi, py, px, si;
	(void)fringe; (void)bounds;

	cpu_initPaint(cpu, &pe, paint, scissor);
	if (paint->image != 0 && !pe.tex) return;

	bb.x0 = bb.y0 = 1e30f; bb.x1 = bb.y1 = -1e30f;
	for (np = 0; np < npaths; np++) {
		const NVGpath* path = &paths[np];
		for (pi = 0; pi < path->nfill; pi++)
			cpu_unionBbox(&bb, path->fill[pi].x, path->fill[pi].y);
	}
	if (bb.x0 > 1e29f) return; // no fill geometry
	bx = (int)floorf(bb.x0); by = (int)floorf(bb.y0);
	bw = (int)ceilf(bb.x1) - bx; bh = (int)ceilf(bb.y1) - by;
	if (bw <= 0 || bh <= 0) return;
	bx = (int)cpu_clampf((float)bx, 0, (float)cpu->width);
	by = (int)cpu_clampf((float)by, 0, (float)cpu->height);
	bw = (int)cpu_minf((float)bw, (float)(cpu->width - bx));
	bh = (int)cpu_minf((float)bh, (float)(cpu->height - by));
	if (bw <= 0 || bh <= 0) return;
	if (!cpu_ensureScratch(cpu, bw * bh)) return;
	memset(cpu->winding, 0, sizeof(int) * (size_t)bw * bh * CPU_NSAMP);

	// Non-zero winding over every sub-path together. The triangle fans keep
	// the orientation of their contour (CCW solid, CW hole) and the crossing
	// rule below is expressed for y-down screen coordinates.
	for (np = 0; np < npaths; np++) {
		const NVGpath* path = &paths[np];
		const NVGvertex* v = path->fill;
		int n = path->nfill, t;
		if (n < 3) continue;
		for (t = 0; t < n - 2; t++) {
			const NVGvertex* a = &v[0];
			const NVGvertex* b = &v[t+1];
			const NVGvertex* c = &v[t+2];
			int tbx = (int)cpu_maxf(0.0f, floorf(cpu_minf(a->x, cpu_minf(b->x, c->x))));
			int tby = (int)cpu_maxf(0.0f, floorf(cpu_minf(a->y, cpu_minf(b->y, c->y))));
			int tex_ = (int)cpu_minf((float)cpu->width - 1, ceilf(cpu_maxf(a->x, cpu_maxf(b->x, c->x))));
			int tey = (int)cpu_minf((float)cpu->height - 1, ceilf(cpu_maxf(a->y, cpu_maxf(b->y, c->y))));
			for (py = tby; py <= tey; py++) {
				if (py < by || py >= by + bh) continue;
				for (px = tbx; px <= tex_; px++) {
					int base = ((py-by) * bw + (px-bx)) * CPU_NSAMP;
					if (px < bx || px >= bx + bw) continue;
					for (si = 0; si < CPU_NSAMP; si++) {
						float sx = (float)px + ((si % CPU_SUBS) + 0.5f) / CPU_SUBS;
						float sy = (float)py + ((si / CPU_SUBS) + 0.5f) / CPU_SUBS;
						int wn = 0;
						cpu_edgeWinding(a->x,a->y,b->x,b->y,sx,sy,&wn);
						cpu_edgeWinding(b->x,b->y,c->x,c->y,sx,sy,&wn);
						cpu_edgeWinding(c->x,c->y,a->x,a->y,sx,sy,&wn);
						cpu->winding[base+si] += wn;
					}
				}
			}
		}
	}

	for (pi = 0; pi < bw * bh * CPU_NSAMP; pi++)
		cpu->mask[pi] = cpu->winding[pi] != 0;

	cpu_composite(cpu, &pe, &compositeOperation, bx, by, bw, bh, 0);
}

static void cpu_renderStroke(void* uptr, NVGpaint* paint,
							 NVGcompositeOperationState compositeOperation,
							 NVGscissor* scissor, float fringe,
							 float strokeWidth, const NVGpath* paths, int npaths)
{
	CPUcontext* cpu = (CPUcontext*)uptr;
	CPUpaint pe;
	CPUbbox bb;
	int bx, by, bw, bh, np, pi, py, px, si;
	(void)fringe; (void)strokeWidth;
	// The core already folded sub-pixel stroke widths into the paint alpha;
	// that alpha must not be scaled a second time here.

	cpu_initPaint(cpu, &pe, paint, scissor);
	if (paint->image != 0 && !pe.tex) return;

	bb.x0 = bb.y0 = 1e30f; bb.x1 = bb.y1 = -1e30f;
	for (np = 0; np < npaths; np++) {
		const NVGpath* path = &paths[np];
		for (pi = 0; pi < path->nstroke; pi++)
			cpu_unionBbox(&bb, path->stroke[pi].x, path->stroke[pi].y);
	}
	if (bb.x0 > 1e29f) return; // no stroke geometry
	bx = (int)floorf(bb.x0); by = (int)floorf(bb.y0);
	bw = (int)ceilf(bb.x1) - bx; bh = (int)ceilf(bb.y1) - by;
	if (bw <= 0 || bh <= 0) return;
	bx = (int)cpu_clampf((float)bx, 0, (float)cpu->width);
	by = (int)cpu_clampf((float)by, 0, (float)cpu->height);
	bw = (int)cpu_minf((float)bw, (float)(cpu->width - bx));
	bh = (int)cpu_minf((float)bh, (float)(cpu->height - by));
	if (bw <= 0 || bh <= 0) return;
	if (!cpu_ensureScratch(cpu, bw * bh)) return;
	memset(cpu->mask, 0, sizeof(unsigned char) * (size_t)bw * bh * CPU_NSAMP);

	// All strips of this call share one coverage union: overlapping joins
	// and self intersections paint each sub-pixel at most once.
	for (np = 0; np < npaths; np++) {
		const NVGpath* path = &paths[np];
		const NVGvertex* v = path->stroke;
		int n = path->nstroke, t;
		if (n < 3) continue;
		for (t = 0; t < n - 2; t++) {
			const NVGvertex* a = &v[t];
			const NVGvertex* b = &v[t+1];
			const NVGvertex* c = &v[t+2];
			int tbx = (int)cpu_maxf(0.0f, floorf(cpu_minf(a->x, cpu_minf(b->x, c->x))));
			int tby = (int)cpu_maxf(0.0f, floorf(cpu_minf(a->y, cpu_minf(b->y, c->y))));
			int tex_ = (int)cpu_minf((float)cpu->width - 1, ceilf(cpu_maxf(a->x, cpu_maxf(b->x, c->x))));
			int tey = (int)cpu_minf((float)cpu->height - 1, ceilf(cpu_maxf(a->y, cpu_maxf(b->y, c->y))));
			for (py = tby; py <= tey; py++) {
				if (py < by || py >= by + bh) continue;
				for (px = tbx; px <= tex_; px++) {
					int base = ((py-by) * bw + (px-bx)) * CPU_NSAMP;
					if (px < bx || px >= bx + bw) continue;
					for (si = 0; si < CPU_NSAMP; si++) {
						float sx = (float)px + ((si % CPU_SUBS) + 0.5f) / CPU_SUBS;
						float sy = (float)py + ((si / CPU_SUBS) + 0.5f) / CPU_SUBS;
						if (cpu_inTriangle(sx,sy,a->x,a->y,b->x,b->y,c->x,c->y))
							cpu->mask[base+si] = 1;
					}
				}
			}
		}
	}

	cpu_composite(cpu, &pe, &compositeOperation, bx, by, bw, bh, 0);
}

static void cpu_renderTriangles(void* uptr, NVGpaint* paint,
								NVGcompositeOperationState compositeOperation,
								NVGscissor* scissor, const NVGvertex* verts,
								int nverts, float fringe)
{
	CPUcontext* cpu = (CPUcontext*)uptr;
	CPUpaint pe;
	CPUbbox bb;
	int bx, by, bw, bh, vi, py, px, si;
	(void)fringe;

	cpu_initPaint(cpu, &pe, paint, scissor);
	if (paint->image != 0 && !pe.tex) return;
	if (nverts < 3) return;

	bb.x0 = bb.y0 = 1e30f; bb.x1 = bb.y1 = -1e30f;
	for (vi = 0; vi < nverts; vi++)
		cpu_unionBbox(&bb, verts[vi].x, verts[vi].y);
	bx = (int)floorf(bb.x0); by = (int)floorf(bb.y0);
	bw = (int)ceilf(bb.x1) - bx; bh = (int)ceilf(bb.y1) - by;
	if (bw <= 0 || bh <= 0) return;
	bx = (int)cpu_clampf((float)bx, 0, (float)cpu->width);
	by = (int)cpu_clampf((float)by, 0, (float)cpu->height);
	bw = (int)cpu_minf((float)bw, (float)(cpu->width - bx));
	bh = (int)cpu_minf((float)bh, (float)(cpu->height - by));
	if (bw <= 0 || bh <= 0) return;
	if (!cpu_ensureScratch(cpu, bw * bh)) return;
	memset(cpu->mask, 0, sizeof(unsigned char) * (size_t)bw * bh * CPU_NSAMP);
	memset(cpu->uv, 0, sizeof(float) * 2 * (size_t)bw * bh * CPU_NSAMP);

	// Triangle list with the half-open shared edge rule; UVs come from
	// barycentric interpolation of the vertices NanoVG provided.
	for (vi = 0; vi + 2 < nverts; vi += 3) {
		const NVGvertex* a = &verts[vi];
		const NVGvertex* b = &verts[vi+1];
		const NVGvertex* c = &verts[vi+2];
		float area = (b->x-a->x)*(c->y-a->y) - (b->y-a->y)*(c->x-a->x);
		int tbx = (int)cpu_maxf(0.0f, floorf(cpu_minf(a->x, cpu_minf(b->x, c->x))));
		int tby = (int)cpu_maxf(0.0f, floorf(cpu_minf(a->y, cpu_minf(b->y, c->y))));
		int tex_ = (int)cpu_minf((float)cpu->width - 1, ceilf(cpu_maxf(a->x, cpu_maxf(b->x, c->x))));
		int tey = (int)cpu_minf((float)cpu->height - 1, ceilf(cpu_maxf(a->y, cpu_maxf(b->y, c->y))));
		if (area == 0.0f) continue;
		for (py = tby; py <= tey; py++) {
			if (py < by || py >= by + bh) continue;
			for (px = tbx; px <= tex_; px++) {
				int base = ((py-by) * bw + (px-bx)) * CPU_NSAMP;
				if (px < bx || px >= bx + bw) continue;
				for (si = 0; si < CPU_NSAMP; si++) {
					float sx = (float)px + ((si % CPU_SUBS) + 0.5f) / CPU_SUBS;
					float sy = (float)py + ((si / CPU_SUBS) + 0.5f) / CPU_SUBS;
					if (cpu_inTriangle(sx,sy,a->x,a->y,b->x,b->y,c->x,c->y)) {
						float w0, w1, w2;
						w0 = ((b->x-sx)*(c->y-sy) - (b->y-sy)*(c->x-sx)) / area;
						w1 = ((c->x-sx)*(a->y-sy) - (c->y-sy)*(a->x-sx)) / area;
						w2 = 1.0f - w0 - w1;
						cpu->mask[base+si] = 1;
						cpu->uv[(base+si)*2+0] = w0*a->u + w1*b->u + w2*c->u;
						cpu->uv[(base+si)*2+1] = w0*a->v + w1*b->v + w2*c->v;
					}
				}
			}
		}
	}

	cpu_composite(cpu, &pe, &compositeOperation, bx, by, bw, bh, 1);
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

NVGcontext* nvgCreateCPU(unsigned char* rgba, int width, int height, int stride)
{
	NVGparams params;
	NVGcontext* ctx;
	CPUcontext* cpu;

	if (rgba == NULL || width <= 0 || height <= 0 || stride < 4 * width)
		return NULL;

	cpu = (CPUcontext*)calloc(1, sizeof(CPUcontext));
	if (!cpu) return NULL;
	cpu->target = rgba;
	cpu->width = width;
	cpu->height = height;
	cpu->stride = stride;
	cpu->frame = (float*)malloc(sizeof(float) * 4 * (size_t)width * (size_t)height);
	if (!cpu->frame) {
		free(cpu);
		return NULL;
	}
	cpu_loadTarget(cpu);

	memset(&params, 0, sizeof(params));
	params.userPtr = cpu;
	params.edgeAntiAlias = 0;
	params.renderCreate = cpu_renderCreate;
	params.renderCreateTexture = cpu_renderCreateTexture;
	params.renderDeleteTexture = cpu_renderDeleteTexture;
	params.renderUpdateTexture = cpu_renderUpdateTexture;
	params.renderGetTextureSize = cpu_renderGetTextureSize;
	params.renderViewport = cpu_renderViewport;
	params.renderCancel = cpu_renderCancel;
	params.renderFlush = cpu_renderFlush;
	params.renderFill = cpu_renderFill;
	params.renderStroke = cpu_renderStroke;
	params.renderTriangles = cpu_renderTriangles;
	params.renderDelete = cpu_renderDelete;

	ctx = nvgCreateInternal(&params);
	if (!ctx) {
		// nvgDeleteInternal() already ran renderDelete, which freed every
		// CPUcontext member; only the wrapper struct itself remains.
		free(cpu);
		return NULL;
	}
	return ctx;
}

void nvgDeleteCPU(NVGcontext* ctx)
{
	CPUcontext* cpu;
	if (!ctx) return;
	cpu = (CPUcontext*)nvgInternalParams(ctx)->userPtr;
	nvgDeleteInternal(ctx);
	free(cpu);
}
