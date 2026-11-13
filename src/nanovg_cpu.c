//
// CPU (off-screen, software) back-end for NanoVG.
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
//    claim that you wrote the original software. If you use a software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

#include "nanovg_cpu.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Physical pixels are sampled on a regular 4x4 sub-pixel grid. A pixel's
// coverage is the fraction of the 16 sub-pixel centers covered by geometry.
#define NVGCPU_SUBSHIFT 2
#define NVGCPU_NSUBS (1 << (NVGCPU_SUBSHIFT * 2))

typedef struct { float x, y; } nvgcpuVec2;

typedef struct {
	int id;          // public image handle (0 == no image)
	int version;     // bumped on every rectangular update
	int live;        // non-zero while this id has not been deleted
	int width, height;
	int type;        // NVG_TEXTURE_RGBA / NVG_TEXTURE_ALPHA
	int flags;       // NVG_IMAGE_*
	unsigned char* data; // immutable copy for this version (RGBA 4 bpp, alpha 1 bpp)
} nvgcpuTexVersion;

// Resolved, immutable reference stored by every recorded draw. It keeps the
// exact texture content seen at record time, so later updates or deletes can
// never change already recorded draws.
typedef struct {
	const unsigned char* data;
	int width, height, type, flags;
} nvgcpuSnap;

typedef struct {
	int srcRGB, dstRGB, srcAlpha, dstAlpha;
} nvgcpuBlend;

typedef struct {
	float x, y, u, v;
} nvgcpuVtx;

enum { NVGCPU_FILL = 0, NVGCPU_STROKE = 1, NVGCPU_TRIANGLES = 2 };

typedef struct {
	int kind;
	NVGpaint paint;
	nvgcpuBlend blend;
	int hasScissor;
	NVGscissor scissor;
	nvgcpuSnap snap;      // resolved texture snapshot (empty when paint.image == 0)
	nvgcpuVtx* verts;     // deep copy of the geometry this draw owns
	int nverts;
	// FILL: polygon sub-paths (indices into verts) and their lengths.
	// STROKE: per-path triangle strips (indices into verts) and their lengths.
	int* fillOffsets;
	int* fillCounts;
	int npaths;
	float bounds[4];      // xmin,ymin,xmax,ymax in physical pixels
} nvgcpuCmd;

typedef struct {
	// Borrowed render target.
	unsigned char* rgba;
	int width, height, stride;

	// Viewport reported by the core (logical size + DPR). The caller
	// guarantees logical size * DPR matches the target dimensions.
	float viewW, viewH, dpr;

	// Texture versions, retained for the life of the context.
	nvgcpuTexVersion* tex;
	int ntex, ctex;
	int nextTexId;

	// Recorded draws for the frame in progress.
	nvgcpuCmd* cmds;
	int ncmds, ccmds;

	int inFrame;
} nvgcpuContext;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static float nvgcpu_clampf(float a, float mn, float mx)
{
	return a < mn ? mn : (a > mx ? mx : a);
}

static int nvgcpu_mini(int a, int b) { return a < b ? a : b; }
static int nvgcpu_maxi(int a, int b) { return a > b ? a : b; }
static int nvgcpu_clampi_(int a, int mn, int mx) { return a < mn ? mn : (a > mx ? mx : a); }
static float nvgcpu_minf_(float a, float b) { return a < b ? a : b; }
static float nvgcpu_maxf_(float a, float b) { return a > b ? a : b; }
static float nvgcpu_max0f(float a) { return a > 0.0f ? a : 0.0f; }

static void nvgcpu_xformPoint(const float* t, float sx, float sy, float* dx, float* dy)
{
	// Same convention as nvgTransformPoint(): [a c e; b d f].
	*dx = sx * t[0] + sy * t[2] + t[4];
	*dy = sx * t[1] + sy * t[3] + t[5];
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

static int nvgcpu_bpp(int type)
{
	return type == NVG_TEXTURE_RGBA ? 4 : 1;
}

static nvgcpuTexVersion* nvgcpu_findTex(nvgcpuContext* c, int id)
{
	int i;
	// Newest live version of an id is the last matching entry.
	for (i = c->ntex - 1; i >= 0; --i)
		if (c->tex[i].id == id && c->tex[i].live)
			return &c->tex[i];
	return NULL;
}

static int nvgcpu_renderCreateTexture(void* uptr, int type, int w, int h,
									  int imageFlags, const unsigned char* data)
{
	nvgcpuContext* c = (nvgcpuContext*)uptr;
	nvgcpuTexVersion* v;
	int bytes;

	if (w <= 0 || h <= 0)
		return 0;
	if (c->ntex + 1 > c->ctex) {
		int cap = c->ctex ? c->ctex + (c->ctex >> 1) : 8;
		nvgcpuTexVersion* p = (nvgcpuTexVersion*)realloc(c->tex, sizeof(*p) * cap);
		if (!p) return 0;
		c->tex = p;
		c->ctex = cap;
	}

	bytes = w * h * nvgcpu_bpp(type);
	v = &c->tex[c->ntex++];
	memset(v, 0, sizeof(*v));
	v->id = ++c->nextTexId;
	v->version = 0;
	v->live = 1;
	v->width = w;
	v->height = h;
	v->type = type;
	v->flags = imageFlags;
	v->data = (unsigned char*)malloc((size_t)bytes);
	if (!v->data) { c->ntex--; return 0; }
	if (data)
		memcpy(v->data, data, (size_t)bytes);
	else
		memset(v->data, 0, (size_t)bytes); // NULL data == all-zero texture
	return v->id;
}

static int nvgcpu_renderDeleteTexture(void* uptr, int image)
{
	nvgcpuContext* c = (nvgcpuContext*)uptr;
	int i, found = 0;
	// Invalidate the handle. Version buffers are retained while recorded
	// draws may still snapshot them; they are released with the context.
	for (i = 0; i < c->ntex; ++i)
		if (c->tex[i].id == image) { c->tex[i].live = 0; found = 1; }
	return found;
}

static int nvgcpu_renderUpdateTexture(void* uptr, int image, int x, int y,
									  int w, int h, const unsigned char* data)
{
	nvgcpuContext* c = (nvgcpuContext*)uptr;
	nvgcpuTexVersion* cur = nvgcpu_findTex(c, image);
	nvgcpuTexVersion* nv;
	int bpp, row, bytes;

	if (!cur || w <= 0 || h <= 0 || !data)
		return 0;

	// Copy-on-write: clone the current version, then patch the rectangle.
	// Earlier versions stay alive for draws already recorded.
	if (c->ntex + 1 > c->ctex) {
		int cap = c->ctex ? c->ctex + (c->ctex >> 1) : 8;
		nvgcpuTexVersion* p = (nvgcpuTexVersion*)realloc(c->tex, sizeof(*p) * cap);
		if (!p) return 0;
		c->tex = p;
		c->ctex = cap;
	}
	bpp = nvgcpu_bpp(cur->type);
	bytes = cur->width * cur->height * bpp;
	nv = &c->tex[c->ntex++];
	memset(nv, 0, sizeof(*nv));
	nv->id = cur->id;
	nv->version = cur->version + 1;
	nv->live = 1;
	nv->width = cur->width;
	nv->height = cur->height;
	nv->type = cur->type;
	nv->flags = cur->flags;
	nv->data = (unsigned char*)malloc((size_t)bytes);
	if (!nv->data) { c->ntex--; return 0; }
	memcpy(nv->data, cur->data, (size_t)bytes);

	// 'data' is the base address of the whole source image; only the given
	// rectangle is refreshed.
	for (row = 0; row < h; ++row) {
		const unsigned char* src = data + ((size_t)(y + row) * cur->width + x) * bpp;
		unsigned char* dst = nv->data + ((size_t)(y + row) * cur->width + x) * bpp;
		memcpy(dst, src, (size_t)w * bpp);
	}
	return 1;
}

static int nvgcpu_renderGetTextureSize(void* uptr, int image, int* w, int* h)
{
	nvgcpuContext* c = (nvgcpuContext*)uptr;
	nvgcpuTexVersion* v = nvgcpu_findTex(c, image);
	if (!v) return 0;
	*w = v->width;
	*h = v->height;
	return 1;
}

// Resolve an image handle to its current immutable snapshot at record time.
static void nvgcpu_resolveSnap(nvgcpuContext* c, int image, nvgcpuSnap* out)
{
	nvgcpuTexVersion* v;
	memset(out, 0, sizeof(*out));
	if (image == 0) return;
	v = nvgcpu_findTex(c, image);
	if (!v) return;
	out->data = v->data;
	out->width = v->width;
	out->height = v->height;
	out->type = v->type;
	out->flags = v->flags;
}

// ---------------------------------------------------------------------------
// View / frame lifecycle
// ---------------------------------------------------------------------------

static int nvgcpu_renderCreate(void* uptr)
{
	(void)uptr;
	return 1;
}

static void nvgcpu_renderViewport(void* uptr, float width, float height,
								  float devicePixelRatio)
{
	nvgcpuContext* c = (nvgcpuContext*)uptr;
	c->viewW = width;
	c->viewH = height;
	c->dpr = devicePixelRatio;
	c->inFrame = 1;
}

static void nvgcpu_freeCmd(nvgcpuCmd* cmd)
{
	free(cmd->verts);
	free(cmd->fillOffsets);
	free(cmd->fillCounts);
	cmd->verts = NULL;
	cmd->fillOffsets = NULL;
	cmd->fillCounts = NULL;
}

static void nvgcpu_clearCommands(nvgcpuContext* c)
{
	int i;
	for (i = 0; i < c->ncmds; ++i)
		nvgcpu_freeCmd(&c->cmds[i]);
	c->ncmds = 0;
}

// Reclaim texture version buffers. Recorded draws only live for one frame and
// are cleared before this runs, so only the newest live version of each id is
// still reachable; older versions and deleted ids can be released. Without
// this, frequent font-atlas updates would accumulate buffers forever.
static void nvgcpu_gcTextures(nvgcpuContext* c)
{
	int i, w;
	// Pass 1: decide keep/free on the untouched array (last live version of
	// each id is the one findTex returns).
	for (i = 0; i < c->ntex; ++i) {
		nvgcpuTexVersion* v = &c->tex[i];
		if (!(v->live && nvgcpu_findTex(c, v->id) == v)) {
			free(v->data);
			v->data = NULL;
		}
	}
	// Pass 2: compact the surviving versions while preserving order.
	w = 0;
	for (i = 0; i < c->ntex; ++i) {
		nvgcpuTexVersion* v = &c->tex[i];
		if (v->live && v->data != NULL) {
			if (w != i) c->tex[w] = *v;
			++w;
		}
	}
	c->ntex = w;
}

static void nvgcpu_renderCancel(void* uptr)
{
	nvgcpuContext* c = (nvgcpuContext*)uptr;
	// Drop this frame's draws; the borrowed target is left untouched.
	nvgcpu_clearCommands(c);
	nvgcpu_gcTextures(c);
	c->inFrame = 0;
}

// ---------------------------------------------------------------------------
// Geometry / rasterization math
// ---------------------------------------------------------------------------

static float nvgcpu_edge(nvgcpuVec2 a, nvgcpuVec2 b, nvgcpuVec2 p)
{
	return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

// Inclusive point-in-triangle test (boundary samples are inside). Combined
// with the per-draw "claim once" mask this gives half-open shared edges:
// adjacent triangles (e.g. the two halves of a glyph quad) never cover the
// same sub-pixel twice.
static int nvgcpu_inTri(nvgcpuVec2 a, nvgcpuVec2 b, nvgcpuVec2 d, nvgcpuVec2 p)
{
	float c0 = nvgcpu_edge(b, d, p);
	float c1 = nvgcpu_edge(d, a, p);
	float c2 = nvgcpu_edge(a, b, p);
	return (c0 >= 0.0f && c1 >= 0.0f && c2 >= 0.0f) ||
		   (c0 <= 0.0f && c1 <= 0.0f && c2 <= 0.0f);
}

// Signed winding contribution of all fill polygons at sample p using the
// non-zero rule over every sub-path together (holes, multiple contours and
// self-intersections all follow from the signed edge crossings).
static int nvgcpu_winding(const nvgcpuCmd* cmd, nvgcpuVec2 p)
{
	int wind = 0;
	int pi;
	for (pi = 0; pi < cmd->npaths; ++pi) {
		const nvgcpuVtx* poly = cmd->verts + cmd->fillOffsets[pi];
		int n = cmd->fillCounts[pi];
		int i;
		for (i = 0; i < n; ++i) {
			const nvgcpuVtx* a = &poly[i];
			const nvgcpuVtx* b = &poly[(i + 1) % n];
			// Half-open straddle so a vertex shared by two edges counts once.
			if ((a->y <= p.y && b->y > p.y) || (b->y <= p.y && a->y > p.y)) {
				float xint = a->x + (p.y - a->y) * (b->x - a->x) / (b->y - a->y);
				if (xint > p.x)
					wind += (b->y > a->y) ? 1 : -1;
			}
		}
	}
	return wind;
}

// ---------------------------------------------------------------------------
// Scissor (hard rectangle in the scissor's local space, no fringe)
// ---------------------------------------------------------------------------

static int nvgcpu_scissorInside(const nvgcpuCmd* cmd, nvgcpuVec2 p)
{
	float inv[6], lx, ly;
	if (!cmd->hasScissor)
		return 1;
	if (nvgTransformInverse(inv, cmd->scissor.xform) == 0)
		return 1;
	nvgcpu_xformPoint(inv, p.x, p.y, &lx, &ly);
	return fabsf(lx) <= cmd->scissor.extent[0] &&
		   fabsf(ly) <= cmd->scissor.extent[1];
}

// ---------------------------------------------------------------------------
// Paint / shader (mirrors the fillFragShader in nanovg_gl.h)
// ---------------------------------------------------------------------------

typedef struct { float r, g, b, a; } nvgcpuColor; // premultiplied

static nvgcpuColor nvgcpu_premul(NVGcolor c)
{
	nvgcpuColor o;
	o.r = c.r * c.a;
	o.g = c.g * c.a;
	o.b = c.b * c.a;
	o.a = c.a;
	return o;
}

static nvgcpuColor nvgcpu_lerpColor(nvgcpuColor a, nvgcpuColor b, float t)
{
	nvgcpuColor o;
	o.r = a.r + (b.r - a.r) * t;
	o.g = a.g + (b.g - a.g) * t;
	o.b = a.b + (b.b - a.b) * t;
	o.a = a.a + (b.a - a.a) * t;
	return o;
}

static float nvgcpu_sdroundrect(nvgcpuVec2 pt, nvgcpuVec2 ext, float rad)
{
	nvgcpuVec2 ext2, d;
	float len;
	ext2.x = ext.x - rad;
	ext2.y = ext.y - rad;
	d.x = fabsf(pt.x) - ext2.x;
	d.y = fabsf(pt.y) - ext2.y;
	len = sqrtf(nvgcpu_max0f(d.x) * nvgcpu_max0f(d.x) +
				nvgcpu_max0f(d.y) * nvgcpu_max0f(d.y));
	return nvgcpu_minf_(nvgcpu_maxf_(d.x, d.y), 0.0f) + len - rad;
}

// Texture sampling -----------------------------------------------------------

static int nvgcpu_wrapIndex(int idx, int dim, int repeat)
{
	if (repeat) {
		int m = idx % dim;
		if (m < 0) m += dim;
		return m;
	}
	return nvgcpu_clampi_(idx, 0, dim - 1);
}

// Sample a texture version into straight (non-premultiplied) float RGBA.
static nvgcpuColor nvgcpu_sample(const nvgcpuSnap* s, float u, float v)
{
	nvgcpuColor col;
	int bpp, repX, repY, nearest;
	float gx, gy, fx, fy;
	int x0, y0, x1, y1;
	const unsigned char *p00, *p10, *p01, *p11;
	nvgcpuColor c00, c10, c01, c11, top, bot;

	memset(&col, 0, sizeof(col));
	if (!s->data)
		return col;

	bpp = s->type == NVG_TEXTURE_RGBA ? 4 : 1;
	repX = (s->flags & NVG_IMAGE_REPEATX) != 0;
	repY = (s->flags & NVG_IMAGE_REPEATY) != 0;
	nearest = (s->flags & NVG_IMAGE_NEAREST) != 0;

	if (nearest) {
		int ix, iy;
		const unsigned char* px;
		if (repX) { float t = u - floorf(u); ix = (int)(t * s->width); if (ix >= s->width) ix = s->width - 1; }
		else ix = nvgcpu_clampi_((int)(u * s->width), 0, s->width - 1);
		if (repY) { float t = v - floorf(v); iy = (int)(t * s->height); if (iy >= s->height) iy = s->height - 1; }
		else iy = nvgcpu_clampi_((int)(v * s->height), 0, s->height - 1);
		px = s->data + ((size_t)iy * s->width + ix) * bpp;
		if (bpp == 4) { col.r = px[0] / 255.0f; col.g = px[1] / 255.0f; col.b = px[2] / 255.0f; col.a = px[3] / 255.0f; }
		else { col.r = col.g = col.b = 0.0f; col.a = px[0] / 255.0f; }
		return col;
	}

	// Bilinear around texel centers: texel i center is at (i + 0.5) / dim.
	gx = u * s->width - 0.5f;
	gy = v * s->height - 0.5f;
	if (repX) {
		x0 = (int)floorf(gx);
		fx = gx - x0;
		x0 = nvgcpu_wrapIndex(x0, s->width, 1);
		x1 = nvgcpu_wrapIndex(x0 + 1, s->width, 1);
	} else {
		float c = nvgcpu_clampf(gx, 0.0f, (float)s->width - 1.0f);
		x0 = (int)floorf(c);
		fx = c - x0;
		x0 = nvgcpu_clampi_(x0, 0, s->width - 1);
		x1 = nvgcpu_mini(x0 + 1, s->width - 1);
	}
	if (repY) {
		y0 = (int)floorf(gy);
		fy = gy - y0;
		y0 = nvgcpu_wrapIndex(y0, s->height, 1);
		y1 = nvgcpu_wrapIndex(y0 + 1, s->height, 1);
	} else {
		float c = nvgcpu_clampf(gy, 0.0f, (float)s->height - 1.0f);
		y0 = (int)floorf(c);
		fy = c - y0;
		y0 = nvgcpu_clampi_(y0, 0, s->height - 1);
		y1 = nvgcpu_mini(y0 + 1, s->height - 1);
	}

#define NVGCPU_TEX(px, out)                                                                 \
	do {                                                                                    \
		if (bpp == 4) { (out).r = (px)[0] / 255.0f; (out).g = (px)[1] / 255.0f;            \
			(out).b = (px)[2] / 255.0f; (out).a = (px)[3] / 255.0f; }                      \
		else { (out).r = (out).g = (out).b = 0.0f; (out).a = (px)[0] / 255.0f; }           \
	} while (0)

	p00 = s->data + ((size_t)y0 * s->width + x0) * bpp;
	p10 = s->data + ((size_t)y0 * s->width + x1) * bpp;
	p01 = s->data + ((size_t)y1 * s->width + x0) * bpp;
	p11 = s->data + ((size_t)y1 * s->width + x1) * bpp;
	NVGCPU_TEX(p00, c00);
	NVGCPU_TEX(p10, c10);
	NVGCPU_TEX(p01, c01);
	NVGCPU_TEX(p11, c11);
#undef NVGCPU_TEX

	top.r = c00.r + (c10.r - c00.r) * fx;
	top.g = c00.g + (c10.g - c00.g) * fx;
	top.b = c00.b + (c10.b - c00.b) * fx;
	top.a = c00.a + (c10.a - c00.a) * fx;
	bot.r = c01.r + (c11.r - c01.r) * fx;
	bot.g = c01.g + (c11.g - c01.g) * fx;
	bot.b = c01.b + (c11.b - c01.b) * fx;
	bot.a = c01.a + (c11.a - c01.a) * fx;
	col.r = top.r + (bot.r - top.r) * fy;
	col.g = top.g + (bot.g - top.g) * fy;
	col.b = top.b + (bot.b - top.b) * fy;
	col.a = top.a + (bot.a - top.a) * fy;
	return col;
}

// Convert a straight sampled texture color into the premultiplied shader
// color, matching texType 0/1/2 in nanovg_gl.h.
static nvgcpuColor nvgcpu_texAdjust(const nvgcpuSnap* s, nvgcpuColor c)
{
	nvgcpuColor o = c;
	if (s->type == NVG_TEXTURE_RGBA) {
		if (!(s->flags & NVG_IMAGE_PREMULTIPLIED)) {
			o.r = c.r * c.a; // straight alpha -> premultiplied (texType 1)
			o.g = c.g * c.a;
			o.b = c.b * c.a;
		}
		// texType 0: data already premultiplied, use as-is.
	} else {
		// Alpha texture -> four channel coverage (texType 2).
		o.r = o.g = o.b = o.a = c.a;
	}
	return o;
}

// Inverse paint matrix, including the FLIPY adjustment used by the GL backend.
static void nvgcpu_paintInverse(const nvgcpuCmd* cmd, float inv[6])
{
	if (cmd->snap.data && (cmd->snap.flags & NVG_IMAGE_FLIPY)) {
		float m1[6], m2[6];
		nvgTransformTranslate(m1, 0.0f, cmd->paint.extent[1] * 0.5f);
		nvgTransformMultiply(m1, cmd->paint.xform);
		nvgTransformScale(m2, 1.0f, -1.0f);
		nvgTransformMultiply(m2, m1);
		nvgTransformTranslate(m1, 0.0f, -cmd->paint.extent[1] * 0.5f);
		nvgTransformMultiply(m1, m2);
		nvgTransformInverse(inv, m1);
	} else {
		nvgTransformInverse(inv, cmd->paint.xform);
	}
}

// Gradient / image-pattern color for a fill or stroke at physical point p.
static nvgcpuColor nvgcpu_paintColor(const nvgcpuCmd* cmd, nvgcpuVec2 p)
{
	nvgcpuColor inner = nvgcpu_premul(cmd->paint.innerColor);
	nvgcpuColor outer = nvgcpu_premul(cmd->paint.outerColor);
	float inv[6];

	if (cmd->paint.image == 0 || !cmd->snap.data) {
		nvgcpuVec2 pt;
		float d;
		nvgcpuVec2 ext;
		nvgTransformInverse(inv, cmd->paint.xform);
		nvgcpu_xformPoint(inv, p.x, p.y, &pt.x, &pt.y);
		ext.x = cmd->paint.extent[0];
		ext.y = cmd->paint.extent[1];
		d = nvgcpu_sdroundrect(pt, ext, cmd->paint.radius);
		d = nvgcpu_clampf((d + cmd->paint.feather * 0.5f) / cmd->paint.feather, 0.0f, 1.0f);
		return nvgcpu_lerpColor(inner, outer, d);
	} else {
		nvgcpuVec2 pt, uv;
		nvgcpuColor tex;
		nvgcpu_paintInverse(cmd, inv);
		nvgcpu_xformPoint(inv, p.x, p.y, &pt.x, &pt.y);
		uv.x = pt.x / cmd->paint.extent[0];
		uv.y = pt.y / cmd->paint.extent[1];
		tex = nvgcpu_sample(&cmd->snap, uv.x, uv.y);
		tex = nvgcpu_texAdjust(&cmd->snap, tex);
		// Apply tint / pattern alpha (premultiplied inner color).
		tex.r *= inner.r; tex.g *= inner.g; tex.b *= inner.b; tex.a *= inner.a;
		return tex;
	}
}

// Textured triangle color at a UV coordinate (shader type 3).
static nvgcpuColor nvgcpu_triColor(const nvgcpuCmd* cmd, float u, float v)
{
	nvgcpuColor inner = nvgcpu_premul(cmd->paint.innerColor);
	nvgcpuColor tex = nvgcpu_sample(&cmd->snap, u, v);
	tex = nvgcpu_texAdjust(&cmd->snap, tex);
	tex.r *= inner.r; tex.g *= inner.g; tex.b *= inner.b; tex.a *= inner.a;
	return tex;
}

// ---------------------------------------------------------------------------
// Blending
// ---------------------------------------------------------------------------

// Resolve one blend factor. sa/da are source/destination alpha; sComp/dComp
// are the channel values for the RGB factors. RGB and alpha are evaluated
// independently by the caller through alphaChannel.
static float nvgcpu_blendFactor(int factor, float sa, float da, float sComp,
								float dComp, int alphaChannel)
{
	switch (factor) {
	case NVG_ZERO: return 0.0f;
	case NVG_ONE: return 1.0f;
	case NVG_SRC_COLOR: return sComp;
	case NVG_ONE_MINUS_SRC_COLOR: return 1.0f - sComp;
	case NVG_DST_COLOR: return dComp;
	case NVG_ONE_MINUS_DST_COLOR: return 1.0f - dComp;
	case NVG_SRC_ALPHA: return sa;
	case NVG_ONE_MINUS_SRC_ALPHA: return 1.0f - sa;
	case NVG_DST_ALPHA: return da;
	case NVG_ONE_MINUS_DST_ALPHA: return 1.0f - da;
	case NVG_SRC_ALPHA_SATURATE:
		// Only valid as an RGB source factor; for the alpha channel OpenGL
		// defines it as ONE.
		return alphaChannel ? 1.0f : nvgcpu_minf_(sa, 1.0f - da);
	default:
		// Unknown factor; the GL back-end would fall back to source-over.
		return 1.0f;
	}
}

static void nvgcpu_blend(const nvgcpuBlend* b, nvgcpuColor* dst, nvgcpuColor src)
{
	nvgcpuColor out;
	float fs, fd;

	fs = nvgcpu_blendFactor(b->srcRGB, src.a, dst->a, src.r, dst->r, 0);
	fd = nvgcpu_blendFactor(b->dstRGB, src.a, dst->a, src.r, dst->r, 0);
	out.r = nvgcpu_clampf(src.r * fs + dst->r * fd, 0.0f, 1.0f);
	fs = nvgcpu_blendFactor(b->srcRGB, src.a, dst->a, src.g, dst->g, 0);
	fd = nvgcpu_blendFactor(b->dstRGB, src.a, dst->a, src.g, dst->g, 0);
	out.g = nvgcpu_clampf(src.g * fs + dst->g * fd, 0.0f, 1.0f);
	fs = nvgcpu_blendFactor(b->srcRGB, src.a, dst->a, src.b, dst->b, 0);
	fd = nvgcpu_blendFactor(b->dstRGB, src.a, dst->a, src.b, dst->b, 0);
	out.b = nvgcpu_clampf(src.b * fs + dst->b * fd, 0.0f, 1.0f);
	fs = nvgcpu_blendFactor(b->srcAlpha, src.a, dst->a, src.a, dst->a, 1);
	fd = nvgcpu_blendFactor(b->dstAlpha, src.a, dst->a, src.a, dst->a, 1);
	out.a = nvgcpu_clampf(src.a * fs + dst->a * fd, 0.0f, 1.0f);
	*dst = out;
}

// ---------------------------------------------------------------------------
// Rasterization of one recorded command into the float framebuffer
// ---------------------------------------------------------------------------

static void nvgcpu_cmdBounds(const nvgcpuCmd* cmd, int* x0, int* y0, int* x1, int* y1)
{
	float minx = cmd->bounds[0], miny = cmd->bounds[1];
	float maxx = cmd->bounds[2], maxy = cmd->bounds[3];
	int i;
	for (i = 0; i < cmd->nverts; ++i) {
		float x = cmd->verts[i].x, y = cmd->verts[i].y;
		if (x < minx) minx = x;
		if (y < miny) miny = y;
		if (x > maxx) maxx = x;
		if (y > maxy) maxy = y;
	}
	*x0 = nvgcpu_maxi((int)floorf(minx), 0);
	*y0 = nvgcpu_maxi((int)floorf(miny), 0);
	*x1 = nvgcpu_mini((int)ceilf(maxx), 0x7fffffff);
	*y1 = nvgcpu_mini((int)ceilf(maxy), 0x7fffffff);
}

static void nvgcpu_raster(nvgcpuContext* c, const nvgcpuCmd* cmd, nvgcpuColor* fb)
{
	int bx0, by0, bx1, by1, W, H;
	int px, py;
	nvgcpu_cmdBounds(cmd, &bx0, &by0, &bx1, &by1);
	W = c->width; H = c->height;
	bx1 = nvgcpu_mini(bx1, W);
	by1 = nvgcpu_mini(by1, H);

	for (py = by0; py < by1; ++py) {
		for (px = bx0; px < bx1; ++px) {
			nvgcpuColor acc;
			int covered = 0;
			int sy, sx;
			acc.r = acc.g = acc.b = acc.a = 0.0f;

			for (sy = 0; sy < (1 << NVGCPU_SUBSHIFT); ++sy) {
				for (sx = 0; sx < (1 << NVGCPU_SUBSHIFT); ++sx) {
					nvgcpuVec2 sp;
					int inside = 0;
					sp.x = px + (2.0f * sx + 1.0f) / (2.0f * (1 << NVGCPU_SUBSHIFT));
					sp.y = py + (2.0f * sy + 1.0f) / (2.0f * (1 << NVGCPU_SUBSHIFT));
					if (!nvgcpu_scissorInside(cmd, sp))
						continue;

					if (cmd->kind == NVGCPU_FILL) {
						inside = nvgcpu_winding(cmd, sp) != 0;
						if (inside) {
							nvgcpuColor col = nvgcpu_paintColor(cmd, sp);
							acc.r += col.r; acc.g += col.g; acc.b += col.b; acc.a += col.a;
							covered++;
						}
					} else if (cmd->kind == NVGCPU_STROKE) {
						int pi;
						// Union over every strip of this renderStroke: a
						// sub-pixel covered by more than one triangle is
						// painted only once, so overlapping joins and
						// self-intersections do not darken inside one stroke.
						for (pi = 0; pi < cmd->npaths && !inside; ++pi) {
							int base = cmd->fillOffsets[pi];
							int cnt = cmd->fillCounts[pi];
							int t;
							for (t = 0; t + 2 < cnt; ++t) {
								nvgcpuVec2 a, b, d;
								int i0, i1, i2;
								// Triangle strip winding.
								if ((t & 1) == 0) { i0 = base + t; i1 = base + t + 1; i2 = base + t + 2; }
								else { i0 = base + t; i1 = base + t + 2; i2 = base + t + 1; }
								a.x = cmd->verts[i0].x; a.y = cmd->verts[i0].y;
								b.x = cmd->verts[i1].x; b.y = cmd->verts[i1].y;
								d.x = cmd->verts[i2].x; d.y = cmd->verts[i2].y;
								if (nvgcpu_edge(a, b, d) == 0.0f)
									continue; // degenerate triangle, covers nothing
								if (nvgcpu_inTri(a, b, d, sp)) { inside = 1; break; }
							}
						}
						if (inside) {
							nvgcpuColor col = nvgcpu_paintColor(cmd, sp);
							acc.r += col.r; acc.g += col.g; acc.b += col.b; acc.a += col.a;
							covered++;
						}
					} else { // NVGCPU_TRIANGLES
						int t;
						for (t = 0; t + 2 < cmd->nverts; t += 3) {
							nvgcpuVec2 a, b, d;
							float area, w0, w1, w2, u, v;
							a.x = cmd->verts[t].x;     a.y = cmd->verts[t].y;
							b.x = cmd->verts[t + 1].x; b.y = cmd->verts[t + 1].y;
							d.x = cmd->verts[t + 2].x; d.y = cmd->verts[t + 2].y;
							area = nvgcpu_edge(a, b, d);
							if (area == 0.0f)
								continue; // degenerate triangle, covers nothing
							if (!nvgcpu_inTri(a, b, d, sp))
								continue;
							w0 = nvgcpu_edge(b, d, sp) / area;
							w1 = nvgcpu_edge(d, a, sp) / area;
							w2 = nvgcpu_edge(a, b, sp) / area;
							u = w0 * cmd->verts[t].u + w1 * cmd->verts[t + 1].u + w2 * cmd->verts[t + 2].u;
							v = w0 * cmd->verts[t].v + w1 * cmd->verts[t + 1].v + w2 * cmd->verts[t + 2].v;
							// First covering triangle claims the sub-pixel, so
							// glyph quad diagonals are counted once.
							{
								nvgcpuColor col = nvgcpu_triColor(cmd, u, v);
								acc.r += col.r; acc.g += col.g; acc.b += col.b; acc.a += col.a;
								covered++;
							}
							inside = 1;
							break;
						}
					}
				}
			}

			if (covered) {
				nvgcpuColor src;
				nvgcpuColor* dst = &fb[py * W + px];
				// Average the per-sub-pixel colors: folds coverage into one
				// premultiplied source for this draw.
				src.r = acc.r / NVGCPU_NSUBS;
				src.g = acc.g / NVGCPU_NSUBS;
				src.b = acc.b / NVGCPU_NSUBS;
				src.a = acc.a / NVGCPU_NSUBS;
				nvgcpu_blend(&cmd->blend, dst, src);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Flush / delete
// ---------------------------------------------------------------------------

static void nvgcpu_renderFlush(void* uptr)
{
	nvgcpuContext* c = (nvgcpuContext*)uptr;
	nvgcpuColor* fb;
	int i, n;

	if (!c->inFrame) {
		nvgcpu_clearCommands(c);
		return;
	}

	n = c->width * c->height;
	fb = (nvgcpuColor*)malloc(sizeof(nvgcpuColor) * (size_t)n);
	if (!fb) { nvgcpu_clearCommands(c); c->inFrame = 0; return; }

	// Load the current target (premultiplied RGBA8) into float, leaving the
	// borrowed bytes untouched until the whole frame has been evaluated.
	for (i = 0; i < n; ++i) {
		int row = i / c->width;
		int col = i % c->width;
		const unsigned char* px = c->rgba + (size_t)row * c->stride + (size_t)col * 4;
		fb[i].r = px[0] / 255.0f;
		fb[i].g = px[1] / 255.0f;
		fb[i].b = px[2] / 255.0f;
		fb[i].a = px[3] / 255.0f;
	}

	for (i = 0; i < c->ncmds; ++i)
		nvgcpu_raster(c, &c->cmds[i], fb);

	// Publish once, rounding only here; padding and out-of-range bytes are
	// never touched.
	for (i = 0; i < n; ++i) {
		int row = i / c->width;
		int col = i % c->width;
		unsigned char* px = c->rgba + (size_t)row * c->stride + (size_t)col * 4;
		px[0] = (unsigned char)nvgcpu_clampi_((int)lroundf(nvgcpu_clampf(fb[i].r, 0.0f, 1.0f) * 255.0f), 0, 255);
		px[1] = (unsigned char)nvgcpu_clampi_((int)lroundf(nvgcpu_clampf(fb[i].g, 0.0f, 1.0f) * 255.0f), 0, 255);
		px[2] = (unsigned char)nvgcpu_clampi_((int)lroundf(nvgcpu_clampf(fb[i].b, 0.0f, 1.0f) * 255.0f), 0, 255);
		px[3] = (unsigned char)nvgcpu_clampi_((int)lroundf(nvgcpu_clampf(fb[i].a, 0.0f, 1.0f) * 255.0f), 0, 255);
	}

	free(fb);
	nvgcpu_clearCommands(c);
	nvgcpu_gcTextures(c);
	c->inFrame = 0;
}

static void nvgcpu_renderDelete(void* uptr)
{
	nvgcpuContext* c = (nvgcpuContext*)uptr;
	int i;
	if (!c) return;
	nvgcpu_clearCommands(c);
	for (i = 0; i < c->ntex; ++i)
		free(c->tex[i].data);
	free(c->tex);
	c->tex = NULL;
	c->ntex = c->ctex = 0;
	free(c->cmds);
	c->cmds = NULL;
	c->ncmds = c->ccmds = 0;
}

// ---------------------------------------------------------------------------
// Recording callbacks
// ---------------------------------------------------------------------------

static nvgcpuCmd* nvgcpu_allocCmd(nvgcpuContext* c)
{
	nvgcpuCmd* cmd;
	if (c->ncmds + 1 > c->ccmds) {
		int cap = c->ccmds ? c->ccmds + (c->ccmds >> 1) : 16;
		nvgcpuCmd* p = (nvgcpuCmd*)realloc(c->cmds, sizeof(*p) * cap);
		if (!p) return NULL;
		c->cmds = p;
		c->ccmds = cap;
	}
	cmd = &c->cmds[c->ncmds++];
	memset(cmd, 0, sizeof(*cmd));
	return cmd;
}

static nvgcpuVtx* nvgcpu_copyVerts(const NVGvertex* src, int n)
{
	nvgcpuVtx* dst;
	int i;
	if (n <= 0)
		return NULL;
	dst = (nvgcpuVtx*)malloc(sizeof(nvgcpuVtx) * (size_t)n);
	if (!dst)
		return NULL;
	for (i = 0; i < n; ++i) {
		dst[i].x = src[i].x;
		dst[i].y = src[i].y;
		dst[i].u = src[i].u;
		dst[i].v = src[i].v;
	}
	return dst;
}

static void nvgcpu_common(nvgcpuCmd* cmd, NVGpaint* paint,
						  NVGcompositeOperationState op, NVGscissor* scissor,
						  nvgcpuContext* c)
{
	cmd->paint = *paint;
	cmd->blend.srcRGB = op.srcRGB;
	cmd->blend.dstRGB = op.dstRGB;
	cmd->blend.srcAlpha = op.srcAlpha;
	cmd->blend.dstAlpha = op.dstAlpha;
	if (scissor && scissor->extent[0] >= -0.5f && scissor->extent[1] >= -0.5f) {
		cmd->hasScissor = 1;
		cmd->scissor = *scissor;
	}
	nvgcpu_resolveSnap(c, paint->image, &cmd->snap);
}

static void nvgcpu_renderFill(void* uptr, NVGpaint* paint,
							  NVGcompositeOperationState compositeOperation,
							  NVGscissor* scissor, float fringe,
							  const float* bounds, const NVGpath* paths,
							  int npaths)
{
	nvgcpuContext* c = (nvgcpuContext*)uptr;
	nvgcpuCmd* cmd = nvgcpu_allocCmd(c);
	int i, total = 0, pi;
	(void)fringe;

	if (!cmd) return;
	cmd->kind = NVGCPU_FILL;
	nvgcpu_common(cmd, paint, compositeOperation, scissor, c);
	if (bounds) {
		cmd->bounds[0] = bounds[0]; cmd->bounds[1] = bounds[1];
		cmd->bounds[2] = bounds[2]; cmd->bounds[3] = bounds[3];
	} else {
		cmd->bounds[0] = cmd->bounds[1] = 1e6f;
		cmd->bounds[2] = cmd->bounds[3] = -1e6f;
	}

	for (i = 0; i < npaths; ++i)
		total += paths[i].nfill;
	cmd->verts = total ? (nvgcpuVtx*)malloc(sizeof(nvgcpuVtx) * (size_t)total) : NULL;
	cmd->fillOffsets = (int*)malloc(sizeof(int) * (size_t)(npaths > 0 ? npaths : 1));
	cmd->fillCounts = (int*)malloc(sizeof(int) * (size_t)(npaths > 0 ? npaths : 1));
	if ((total && !cmd->verts) || !cmd->fillOffsets || !cmd->fillCounts) {
		nvgcpu_freeCmd(cmd); c->ncmds--; return;
	}
	cmd->npaths = npaths;
	pi = 0;
	for (i = 0; i < npaths; ++i) {
		int j;
		cmd->fillOffsets[i] = pi;
		cmd->fillCounts[i] = paths[i].nfill;
		for (j = 0; j < paths[i].nfill; ++j) {
			cmd->verts[pi].x = paths[i].fill[j].x;
			cmd->verts[pi].y = paths[i].fill[j].y;
			cmd->verts[pi].u = paths[i].fill[j].u;
			cmd->verts[pi].v = paths[i].fill[j].v;
			pi++;
		}
	}
	cmd->nverts = pi;
}

static void nvgcpu_renderStroke(void* uptr, NVGpaint* paint,
								NVGcompositeOperationState compositeOperation,
								NVGscissor* scissor, float fringe,
								float strokeWidth, const NVGpath* paths,
								int npaths)
{
	nvgcpuContext* c = (nvgcpuContext*)uptr;
	nvgcpuCmd* cmd = nvgcpu_allocCmd(c);
	int i, total = 0, pi;
	(void)fringe; (void)strokeWidth;

	if (!cmd) return;
	cmd->kind = NVGCPU_STROKE;
	nvgcpu_common(cmd, paint, compositeOperation, scissor, c);
	cmd->bounds[0] = cmd->bounds[1] = 1e6f;
	cmd->bounds[2] = cmd->bounds[3] = -1e6f;

	for (i = 0; i < npaths; ++i)
		total += paths[i].nstroke;
	cmd->verts = total ? (nvgcpuVtx*)malloc(sizeof(nvgcpuVtx) * (size_t)total) : NULL;
	// Keep each path's triangle strip separate so the strip winding restarts
	// per path instead of joining two unrelated strips into bad triangles.
	cmd->fillOffsets = (int*)malloc(sizeof(int) * (size_t)(npaths > 0 ? npaths : 1));
	cmd->fillCounts = (int*)malloc(sizeof(int) * (size_t)(npaths > 0 ? npaths : 1));
	if ((total && !cmd->verts) || !cmd->fillOffsets || !cmd->fillCounts) {
		nvgcpu_freeCmd(cmd); c->ncmds--; return;
	}
	cmd->npaths = npaths;
	pi = 0;
	for (i = 0; i < npaths; ++i) {
		int j;
		cmd->fillOffsets[i] = pi;
		cmd->fillCounts[i] = paths[i].nstroke;
		for (j = 0; j < paths[i].nstroke; ++j) {
			cmd->verts[pi].x = paths[i].stroke[j].x;
			cmd->verts[pi].y = paths[i].stroke[j].y;
			cmd->verts[pi].u = paths[i].stroke[j].u;
			cmd->verts[pi].v = paths[i].stroke[j].v;
			pi++;
		}
	}
	cmd->nverts = pi;
}

static void nvgcpu_renderTriangles(void* uptr, NVGpaint* paint,
								   NVGcompositeOperationState compositeOperation,
								   NVGscissor* scissor, const NVGvertex* verts,
								   int nverts, float fringe)
{
	nvgcpuContext* c = (nvgcpuContext*)uptr;
	nvgcpuCmd* cmd = nvgcpu_allocCmd(c);
	(void)fringe;

	if (!cmd) return;
	cmd->kind = NVGCPU_TRIANGLES;
	nvgcpu_common(cmd, paint, compositeOperation, scissor, c);
	cmd->bounds[0] = cmd->bounds[1] = 1e6f;
	cmd->bounds[2] = cmd->bounds[3] = -1e6f;
	cmd->verts = nvgcpu_copyVerts(verts, nverts);
	cmd->nverts = nverts;
	if (nverts > 0 && !cmd->verts) { nvgcpu_freeCmd(cmd); c->ncmds--; return; }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

NVGcontext* nvgCreateCPU(unsigned char* rgba, int width, int height, int stride)
{
	nvgcpuContext* c;
	NVGparams params;
	NVGcontext* ctx;

	if (rgba == NULL || width <= 0 || height <= 0 || stride < 4 * width)
		return NULL;

	c = (nvgcpuContext*)calloc(1, sizeof(nvgcpuContext));
	if (!c)
		return NULL;
	c->rgba = rgba;
	c->width = width;
	c->height = height;
	c->stride = stride;
	c->nextTexId = 0;
	c->inFrame = 0;

	memset(&params, 0, sizeof(params));
	params.userPtr = c;
	params.edgeAntiAlias = 0; // use the exact tessellated geometry directly
	params.renderCreate = nvgcpu_renderCreate;
	params.renderCreateTexture = nvgcpu_renderCreateTexture;
	params.renderDeleteTexture = nvgcpu_renderDeleteTexture;
	params.renderUpdateTexture = nvgcpu_renderUpdateTexture;
	params.renderGetTextureSize = nvgcpu_renderGetTextureSize;
	params.renderViewport = nvgcpu_renderViewport;
	params.renderCancel = nvgcpu_renderCancel;
	params.renderFlush = nvgcpu_renderFlush;
	params.renderFill = nvgcpu_renderFill;
	params.renderStroke = nvgcpu_renderStroke;
	params.renderTriangles = nvgcpu_renderTriangles;
	params.renderDelete = nvgcpu_renderDelete;

	ctx = nvgCreateInternal(&params);
	if (!ctx) {
		free(c);
		return NULL;
	}
	return ctx;
}

void nvgDeleteCPU(NVGcontext* ctx)
{
	nvgcpuContext* c;
	if (!ctx)
		return;
	c = (nvgcpuContext*)nvgInternalParams(ctx)->userPtr;
	nvgDeleteInternal(ctx); // invokes nvgcpu_renderDelete() on our user ptr
	free(c);
}
