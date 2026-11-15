//
//  image_assets.c
//
//  Regression tests for shared image assets behind cr_cn_image nodes:
//  one decode per scene+path, a cached linear view for SRGB_TRANSFORM,
//  deterministic draining of async tasks at scene_destroy, and
//  serialize/deserialize fidelity for both the raw and linear views.
//

#include "task_support/check.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <v.h>

#include <c-ray/c-ray.h>
#include <c-ray/node.h>
#include <common/color.h>
#include <common/texture.h>
#include <datatypes/scene.h>
#include <nodes/colornode.h>
#include <nodes/shaders/background.h>
#include <accelerators/bvh.h>
#include <renderer/pathtrace.h>
#include <renderer/renderer.h>
#include <renderer/samplers/sampler.h>
#include <protocol/protocol.h>

// ---------- independent reference model ----------
// The checks below compare the production pipeline against this
// separately written model of the documented sampling semantics.

static float ref_srgb_to_linear(float c) {
	// Independently written from the sRGB spec (IEC 61966-2-1).
	if (c <= 0.04045f) return c / 12.92f;
	return powf((c + 0.055f) / 1.055f, 2.4f);
}

struct ref_img {
	unsigned char *px; // top-down rows, ch bytes per pixel
	int w, h, ch;
};

static struct color ref_texel(const struct ref_img *img, int x, int y, bool linear) {
	// Mirror tex_get_px(): out-of-range texels wrap, rows are flipped.
	x %= img->w; if (x < 0) x += img->w;
	y %= img->h; if (y < 0) y += img->h;
	const unsigned char *p = img->px + ((img->h - 1 - y) * img->w + x) * img->ch;
	struct color c = {
		p[0] / 255.0f,
		p[1] / 255.0f,
		p[2] / 255.0f,
		img->ch > 3 ? p[3] / 255.0f : 1.0f
	};
	if (linear) {
		c.red = ref_srgb_to_linear(c.red);
		c.green = ref_srgb_to_linear(c.green);
		c.blue = ref_srgb_to_linear(c.blue);
	}
	return c;
}

static float ref_lerp_channel(float a, float b, float t) {
	return (1.0f - t) * a + t * b;
}

static struct color ref_lerp(struct color a, struct color b, float t) {
	return (struct color){
		ref_lerp_channel(a.red, b.red, t),
		ref_lerp_channel(a.green, b.green, t),
		ref_lerp_channel(a.blue, b.blue, t),
		ref_lerp_channel(a.alpha, b.alpha, t),
	};
}

static struct color ref_sample(const struct ref_img *img, float u, float v, unsigned options) {
	bool linear = (options & SRGB_TRANSFORM) != 0;
	if (options & NO_BILINEAR)
		return ref_texel(img, (int)(u * img->w), (int)(v * img->h), linear);
	float xc = u * img->w - 0.5f;
	float yc = v * img->h - 0.5f;
	int xi = (int)xc, yi = (int)yc;
	struct color tl = ref_texel(img, xi, yi, linear);
	struct color tr = ref_texel(img, xi + 1, yi, linear);
	struct color bl = ref_texel(img, xi, yi + 1, linear);
	struct color br = ref_texel(img, xi + 1, yi + 1, linear);
	return ref_lerp(ref_lerp(tl, tr, xc - xi), ref_lerp(bl, br, xc - xi), yc - yi);
}

// ---------- small image writers ----------

static int write_ppm(const char *path, const unsigned char *rgb, int w, int h) {
	FILE *f = fopen(path, "wb");
	if (!f) return 1;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	size_t want = (size_t)w * (size_t)h * 3;
	int ok = fwrite(rgb, 1, want, f) == want;
	fclose(f);
	return ok ? 0 : 1;
}

static int write_tga(const char *path, const unsigned char *bgra, int w, int h) {
	FILE *f = fopen(path, "wb");
	if (!f) return 1;
	unsigned char header[18] = { 0 };
	header[2] = 2;                // uncompressed true-color
	header[12] = (unsigned char)(w & 0xff);
	header[13] = (unsigned char)((w >> 8) & 0xff);
	header[14] = (unsigned char)(h & 0xff);
	header[15] = (unsigned char)((h >> 8) & 0xff);
	header[16] = 32;              // 8 bits per channel, BGRA
	header[17] = 0x28;            // 8 alpha bits, top-left origin
	size_t want = (size_t)w * (size_t)h * 4;
	int ok = fwrite(header, 1, sizeof(header), f) == sizeof(header) &&
	         fwrite(bgra, 1, want, f) == want;
	fclose(f);
	return ok ? 0 : 1;
}

static int write_hdr(const char *path, const unsigned char *rgbe, int w, int h) {
	FILE *f = fopen(path, "wb");
	if (!f) return 1;
	fprintf(f, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n", h, w);
	size_t want = (size_t)w * (size_t)h * 4;
	int ok = fwrite(rgbe, 1, want, f) == want;
	fclose(f);
	return ok ? 0 : 1;
}

// ---------- shared helpers ----------

static struct color eval_at(const struct colorNode *n, float u, float v) {
	struct hitRecord h = { 0 };
	h.uv = (struct coord){ u, v };
	return n->eval(n, NULL, &h);
}

static bool close_color(struct color a, struct color b, float tol) {
	return fabsf(a.red - b.red) < tol &&
	       fabsf(a.green - b.green) < tol &&
	       fabsf(a.blue - b.blue) < tol &&
	       fabsf(a.alpha - b.alpha) < tol;
}

static bool close_rgb(struct color a, struct color b, float tol) {
	return fabsf(a.red - b.red) < tol &&
	       fabsf(a.green - b.green) < tol &&
	       fabsf(a.blue - b.blue) < tol;
}

static bool is_pink(struct color c) {
	return colorEquals(c, g_pink_color);
}

static struct texture_asset *find_asset(struct world *w, const char *name) {
	for (size_t i = 0; i < v_arr_len(w->textures); ++i) {
		if (strstr(w->textures[i]->path, name)) return w->textures[i];
	}
	return NULL;
}

static struct cr_color_node image_desc(char *path, unsigned options) {
	struct cr_color_node n = { .type = cr_cn_image };
	n.arg.image.full_path = path;
	n.arg.image.options = (uint8_t)options;
	return n;
}

// Distinct texel colors so filtering and view mix-ups are observable.
static const unsigned char g_rgb[4][3] = {
	{  10,  60, 128 },
	{ 200,  30,  90 },
	{ 255, 128,   0 },
	{  40, 220, 180 },
};

static const float g_uvs[][2] = {
	{ 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.25f, 0.75f }, { 0.75f, 0.75f },
	{ 0.5f, 0.5f }, { 0.1f, 0.9f }, { 0.9f, 0.1f }, { 0.4f, 0.6f },
};
#define NUVS (sizeof(g_uvs) / sizeof(g_uvs[0]))

static void make_ppm(unsigned char out[12]) {
	for (int i = 0; i < 4; ++i) memcpy(&out[i * 3], g_rgb[i], 3);
}

// ---------- controlled thread pool ----------
// A gate task parks the (single) pool worker until the test releases it,
// giving deterministic control over queued decode/view tasks. No sleeps.

struct gate {
	v_mutex *mu;
	v_cond *cv;
	bool started;
	bool release;
};

static void gate_init(struct gate *g) {
	g->mu = v_mutex_create();
	g->cv = v_cond_create();
	g->started = false;
	g->release = false;
}

static void gate_destroy(struct gate *g) {
	v_cond_destroy(g->cv);
	v_mutex_destroy(g->mu);
}

static void blocker_task(void *arg) {
	struct gate *g = arg;
	v_mutex_lock(g->mu);
	g->started = true;
	v_cond_broadcast(g->cv);
	while (!g->release) v_cond_wait(g->cv, g->mu);
	v_mutex_release(g->mu);
}

static void gate_wait_started(struct gate *g) {
	v_mutex_lock(g->mu);
	while (!g->started) v_cond_wait(g->cv, g->mu);
	v_mutex_release(g->mu);
}

static void gate_release(struct gate *g) {
	v_mutex_lock(g->mu);
	g->release = true;
	v_cond_broadcast(g->cv);
	v_mutex_release(g->mu);
}

static void *release_fn(void *arg) {
	gate_release(arg);
	return NULL;
}

static void use_single_worker(struct world *w) {
	v_threadpool_destroy(w->bg_worker);
	w->bg_worker = v_threadpool_create(1);
}

// ---------- variants ----------

static char path_variants[] = ".task-build/task_variants.ppm";

static int case_request_orders(void) {
	unsigned char ppm[12];
	make_ppm(ppm);
	TASK_CHECK(write_ppm(path_variants, ppm, 2, 2) == 0);
	struct ref_img img = { ppm, 2, 2, 3 };
	static const unsigned opts[4] = { 0, SRGB_TRANSFORM, NO_BILINEAR, SRGB_TRANSFORM | NO_BILINEAR };
	for (int order = 0; order < 2; ++order) {
		struct world *w = scene_new();
		const struct colorNode *nodes[4] = { 0 };
		// All four option combinations; round two asks for the
		// SRGB_TRANSFORM views before the raw ones.
		for (int step = 0; step < 4; ++step) {
			int idx = order == 0 ? step : 3 - step;
			struct cr_color_node d = image_desc(path_variants, opts[idx]);
			nodes[idx] = build_color_node((struct cr_scene *)w, &d);
			TASK_CHECK(nodes[idx] != NULL);
		}
		// Identical repeat requests reuse the hash-consed nodes.
		for (int i = 0; i < 4; ++i) {
			struct cr_color_node d = image_desc(path_variants, opts[i]);
			TASK_CHECK(build_color_node((struct cr_scene *)w, &d) == nodes[i]);
		}
		// Full options still distinguish node identity.
		for (int i = 0; i < 4; ++i)
			for (int j = i + 1; j < 4; ++j)
				TASK_CHECK(nodes[i] != nodes[j]);
		v_threadpool_wait(w->bg_worker);
		// One shared asset for the path: NO_BILINEAR did not split storage.
		TASK_CHECK(v_arr_len(w->textures) == 1);
		struct texture_asset *a = find_asset(w, "task_variants.ppm");
		TASK_CHECK(a != NULL);
		TASK_CHECK(a->decode_count == 1);
		TASK_CHECK(a->state == tex_asset_ready);
		// Raw source untouched, linear view cached separately.
		TASK_CHECK(a->t != NULL && a->t->colorspace == sRGB);
		TASK_CHECK(a->t->precision == char_p);
		TASK_CHECK(a->linear_ready && a->linear != NULL);
		TASK_CHECK(a->linear != a->t);
		TASK_CHECK(a->linear->precision == float_p);
		TASK_CHECK(a->linear->colorspace == linear);
		for (size_t s = 0; s < NUVS; ++s) {
			for (int i = 0; i < 4; ++i) {
				struct color got = eval_at(nodes[i], g_uvs[s][0], g_uvs[s][1]);
				struct color want = ref_sample(&img, g_uvs[s][0], g_uvs[s][1], opts[i]);
				TASK_CHECK(close_color(got, want, 1e-5f));
			}
		}
		scene_destroy(w);
	}
	return 0;
}

static char path_pending[] = ".task-build/task_pending.ppm";

static int case_pending_views(void) {
	unsigned char ppm[12];
	make_ppm(ppm);
	TASK_CHECK(write_ppm(path_pending, ppm, 2, 2) == 0);
	struct ref_img img = { ppm, 2, 2, 3 };
	struct world *w = scene_new();
	use_single_worker(w);
	struct gate g;
	gate_init(&g);
	v_threadpool_enqueue(w->bg_worker, blocker_task, &g);
	gate_wait_started(&g);
	// Both views requested while the source decode is still queued.
	struct cr_color_node dl = image_desc(path_pending, SRGB_TRANSFORM | NO_BILINEAR);
	struct cr_color_node dr = image_desc(path_pending, NO_BILINEAR);
	const struct colorNode *lin = build_color_node((struct cr_scene *)w, &dl);
	const struct colorNode *raw = build_color_node((struct cr_scene *)w, &dr);
	TASK_CHECK(lin != NULL && raw != NULL);
	// Not decoded yet: both views still report the pink fallback.
	TASK_CHECK(is_pink(eval_at(raw, 0.25f, 0.25f)));
	TASK_CHECK(is_pink(eval_at(lin, 0.25f, 0.25f)));
	struct texture_asset *a = find_asset(w, "task_pending.ppm");
	TASK_CHECK(a != NULL);
	TASK_CHECK(a->decode_count == 0);
	TASK_CHECK(a->linear_requested);
	gate_release(&g);
	v_threadpool_wait(w->bg_worker);
	// Views requested while pending converge to the same cached data.
	TASK_CHECK(a->decode_count == 1);
	TASK_CHECK(a->linear_ready && a->linear != NULL && a->linear != a->t);
	for (size_t s = 0; s < NUVS; ++s) {
		float u = g_uvs[s][0], v = g_uvs[s][1];
		TASK_CHECK(close_color(eval_at(raw, u, v), ref_sample(&img, u, v, NO_BILINEAR), 1e-5f));
		TASK_CHECK(close_color(eval_at(lin, u, v), ref_sample(&img, u, v, SRGB_TRANSFORM | NO_BILINEAR), 1e-5f));
	}
	gate_destroy(&g);
	scene_destroy(w);
	return 0;
}

static char path_done[] = ".task-build/task_done.ppm";

static int case_completed_then_views(void) {
	unsigned char ppm[12];
	make_ppm(ppm);
	TASK_CHECK(write_ppm(path_done, ppm, 2, 2) == 0);
	struct ref_img img = { ppm, 2, 2, 3 };
	struct world *w = scene_new();
	struct cr_color_node dr = image_desc(path_done, NO_BILINEAR);
	const struct colorNode *raw = build_color_node((struct cr_scene *)w, &dr);
	TASK_CHECK(raw != NULL);
	v_threadpool_wait(w->bg_worker);
	struct texture_asset *a = find_asset(w, "task_done.ppm");
	TASK_CHECK(a != NULL);
	TASK_CHECK(a->state == tex_asset_ready);
	TASK_CHECK(!a->linear_ready && a->linear == NULL);
	// Snapshot raw samples before the linear view exists.
	struct color before[NUVS];
	for (size_t s = 0; s < NUVS; ++s)
		before[s] = eval_at(raw, g_uvs[s][0], g_uvs[s][1]);
	// Append the linear view after the source already decoded.
	struct cr_color_node dl = image_desc(path_done, SRGB_TRANSFORM | NO_BILINEAR);
	const struct colorNode *lin = build_color_node((struct cr_scene *)w, &dl);
	TASK_CHECK(lin != NULL);
	TASK_CHECK(a->linear_ready && a->linear != NULL && a->linear != a->t);
	// View generation did not rewrite the raw pixels.
	TASK_CHECK(a->t->colorspace == sRGB);
	for (size_t s = 0; s < NUVS; ++s) {
		struct color after = eval_at(raw, g_uvs[s][0], g_uvs[s][1]);
		TASK_CHECK(colorEquals(before[s], after));
		TASK_CHECK(close_color(after, ref_sample(&img, g_uvs[s][0], g_uvs[s][1], NO_BILINEAR), 1e-6f));
		TASK_CHECK(close_color(eval_at(lin, g_uvs[s][0], g_uvs[s][1]),
			ref_sample(&img, g_uvs[s][0], g_uvs[s][1], SRGB_TRANSFORM | NO_BILINEAR), 1e-5f));
	}
	TASK_CHECK(a->decode_count == 1);
	scene_destroy(w);
	// Reverse order on a fresh scene: raw view appended after the linear one.
	struct world *w2 = scene_new();
	struct cr_color_node dl2 = image_desc(path_done, SRGB_TRANSFORM);
	const struct colorNode *lin2 = build_color_node((struct cr_scene *)w2, &dl2);
	TASK_CHECK(lin2 != NULL);
	v_threadpool_wait(w2->bg_worker);
	struct texture_asset *a2 = find_asset(w2, "task_done.ppm");
	TASK_CHECK(a2 != NULL);
	TASK_CHECK(a2->linear_ready && a2->linear != NULL);
	struct cr_color_node dr2 = image_desc(path_done, 0);
	const struct colorNode *raw2 = build_color_node((struct cr_scene *)w2, &dr2);
	TASK_CHECK(raw2 != NULL);
	TASK_CHECK(a2->decode_count == 1);
	for (size_t s = 0; s < NUVS; ++s) {
		float u = g_uvs[s][0], v = g_uvs[s][1];
		TASK_CHECK(close_color(eval_at(raw2, u, v), ref_sample(&img, u, v, 0), 1e-6f));
		TASK_CHECK(close_color(eval_at(lin2, u, v), ref_sample(&img, u, v, SRGB_TRANSFORM), 1e-5f));
	}
	scene_destroy(w2);
	return 0;
}

static char path_tga[] = ".task-build/task_alpha.tga";

static int case_alpha_tga(void) {
	// BGRA on disk, distinct alpha per texel.
	unsigned char tga[16];
	unsigned char rgba[16];
	static const unsigned char alphas[4] = { 255, 128, 64, 0 };
	for (int i = 0; i < 4; ++i) {
		tga[i * 4 + 0] = g_rgb[i][2];
		tga[i * 4 + 1] = g_rgb[i][1];
		tga[i * 4 + 2] = g_rgb[i][0];
		tga[i * 4 + 3] = alphas[i];
		rgba[i * 4 + 0] = g_rgb[i][0];
		rgba[i * 4 + 1] = g_rgb[i][1];
		rgba[i * 4 + 2] = g_rgb[i][2];
		rgba[i * 4 + 3] = alphas[i];
	}
	TASK_CHECK(write_tga(path_tga, tga, 2, 2) == 0);
	struct ref_img img = { rgba, 2, 2, 4 };
	struct world *w = scene_new();
	struct cr_color_node dr = image_desc(path_tga, NO_BILINEAR);
	struct cr_color_node dl = image_desc(path_tga, SRGB_TRANSFORM | NO_BILINEAR);
	struct cr_color_node db = image_desc(path_tga, SRGB_TRANSFORM);
	const struct colorNode *raw = build_color_node((struct cr_scene *)w, &dr);
	const struct colorNode *lin = build_color_node((struct cr_scene *)w, &dl);
	const struct colorNode *linb = build_color_node((struct cr_scene *)w, &db);
	TASK_CHECK(raw != NULL && lin != NULL && linb != NULL);
	v_threadpool_wait(w->bg_worker);
	struct texture_asset *a = find_asset(w, "task_alpha.tga");
	TASK_CHECK(a != NULL);
	TASK_CHECK(a->decode_count == 1);
	TASK_CHECK(a->t->channels == 4);
	TASK_CHECK(a->linear != NULL && a->linear->channels == 4);
	// Per-texel checks: alpha preserved exactly, RGB converted per view.
	static const float centers[4][2] = {
		{ 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.25f, 0.75f }, { 0.75f, 0.75f }
	};
	for (int s = 0; s < 4; ++s) {
		float u = centers[s][0], v = centers[s][1];
		struct color want_raw = ref_sample(&img, u, v, NO_BILINEAR);
		struct color want_lin = ref_sample(&img, u, v, SRGB_TRANSFORM | NO_BILINEAR);
		struct color got_raw = eval_at(raw, u, v);
		struct color got_lin = eval_at(lin, u, v);
		TASK_CHECK(close_color(got_raw, want_raw, 1e-6f));
		TASK_CHECK(close_color(got_lin, want_lin, 1e-5f));
		// Alpha rides along untouched by the sRGB curve.
		TASK_CHECK(got_lin.alpha == got_raw.alpha);
		TASK_CHECK(got_lin.alpha == want_raw.alpha);
	}
	// Bilinear over the linear view filters the float copy, alpha included.
	for (size_t s = 0; s < NUVS; ++s) {
		struct color got = eval_at(linb, g_uvs[s][0], g_uvs[s][1]);
		struct color want = ref_sample(&img, g_uvs[s][0], g_uvs[s][1], SRGB_TRANSFORM);
		TASK_CHECK(close_color(got, want, 1e-5f));
	}
	scene_destroy(w);
	return 0;
}

static char path_hdr[] = ".task-build/task_float.hdr";

static int case_hdr_float_source(void) {
	// Flat RGBE with E=128: channel value = byte / 256.
	unsigned char rgbe[16];
	static const unsigned char vals[4][3] = {
		{ 128, 64, 32 }, { 64, 128, 64 }, { 32, 64, 128 }, { 128, 128, 128 }
	};
	for (int i = 0; i < 4; ++i) {
		rgbe[i * 4 + 0] = vals[i][0];
		rgbe[i * 4 + 1] = vals[i][1];
		rgbe[i * 4 + 2] = vals[i][2];
		rgbe[i * 4 + 3] = 128;
	}
	TASK_CHECK(write_hdr(path_hdr, rgbe, 2, 2) == 0);
	struct world *w = scene_new();
	struct cr_color_node dr = image_desc(path_hdr, NO_BILINEAR);
	struct cr_color_node dl = image_desc(path_hdr, SRGB_TRANSFORM | NO_BILINEAR);
	const struct colorNode *raw = build_color_node((struct cr_scene *)w, &dr);
	const struct colorNode *lin = build_color_node((struct cr_scene *)w, &dl);
	TASK_CHECK(raw != NULL && lin != NULL);
	v_threadpool_wait(w->bg_worker);
	struct texture_asset *a = find_asset(w, "task_float.hdr");
	TASK_CHECK(a != NULL);
	TASK_CHECK(a->decode_count == 1);
	TASK_CHECK(a->t->precision == float_p);
	TASK_CHECK(a->t->colorspace == linear);
	// Already linear float data: no separate view, no conversion.
	TASK_CHECK(a->linear_ready);
	TASK_CHECK(a->linear == NULL);
	static const float centers[4][2] = {
		{ 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.25f, 0.75f }, { 0.75f, 0.75f }
	};
	for (int s = 0; s < 4; ++s) {
		int x = s & 1, y = s >> 1;
		int file_row = 1 - y; // texture rows are flipped relative to the file
		struct color want = {
			vals[file_row * 2 + x][0] / 256.0f,
			vals[file_row * 2 + x][1] / 256.0f,
			vals[file_row * 2 + x][2] / 256.0f,
			1.0f
		};
		TASK_CHECK(close_color(eval_at(raw, centers[s][0], centers[s][1]), want, 1e-6f));
		TASK_CHECK(close_color(eval_at(lin, centers[s][0], centers[s][1]), want, 1e-6f));
	}
	scene_destroy(w);
	return 0;
}

static char path_missing[] = ".task-build/task_missing.ppm";

static int case_failure_reuse(void) {
	unlink(path_missing);
	struct world *w = scene_new();
	struct cr_color_node dr = image_desc(path_missing, NO_BILINEAR);
	const struct colorNode *raw = build_color_node((struct cr_scene *)w, &dr);
	TASK_CHECK(raw != NULL);
	v_threadpool_wait(w->bg_worker);
	struct texture_asset *a = find_asset(w, "task_missing.ppm");
	TASK_CHECK(a != NULL);
	TASK_CHECK(a->state == tex_asset_failed);
	TASK_CHECK(a->decode_count == 1);
	TASK_CHECK(is_pink(eval_at(raw, 0.25f, 0.25f)));
	// Later requests for the same path reuse the failed asset: no retry.
	struct cr_color_node dl = image_desc(path_missing, SRGB_TRANSFORM | NO_BILINEAR);
	const struct colorNode *lin = build_color_node((struct cr_scene *)w, &dl);
	TASK_CHECK(lin != NULL);
	TASK_CHECK(is_pink(eval_at(lin, 0.5f, 0.5f)));
	struct cr_color_node dr2 = image_desc(path_missing, NO_BILINEAR);
	TASK_CHECK(build_color_node((struct cr_scene *)w, &dr2) == raw);
	v_threadpool_wait(w->bg_worker);
	TASK_CHECK(a->decode_count == 1);
	TASK_CHECK(v_arr_len(w->textures) == 1);
	scene_destroy(w);
	return 0;
}

// ---------- lifecycle ----------

struct probe_ctx {
	struct world *w;
	const char *name;
	bool ran;
	bool saw_ready;
};

static void probe_task(void *arg) {
	struct probe_ctx *p = arg;
	struct texture_asset *a = find_asset(p->w, p->name);
	p->saw_ready = a && a->state == tex_asset_ready;
	p->ran = true;
}

static void *destroy_fn(void *arg) {
	scene_destroy(arg);
	return NULL;
}

static char path_drain[] = ".task-build/task_drain.ppm";

static int case_destroy_drains_tasks(void) {
	unsigned char ppm[12];
	make_ppm(ppm);
	TASK_CHECK(write_ppm(path_drain, ppm, 2, 2) == 0);
	struct world *w = scene_new();
	use_single_worker(w);
	struct gate g;
	gate_init(&g);
	v_threadpool_enqueue(w->bg_worker, blocker_task, &g);
	gate_wait_started(&g);
	// Queue a decode (and its linear view) behind the parked worker.
	struct cr_color_node d = image_desc(path_drain, SRGB_TRANSFORM | NO_BILINEAR);
	TASK_CHECK(build_color_node((struct cr_scene *)w, &d) != NULL);
	// Probe queued after the decode records what it observes.
	struct probe_ctx p = { w, "task_drain.ppm", false, false };
	v_threadpool_enqueue(w->bg_worker, probe_task, &p);
	v_thread *dt = v_thread_spawn(destroy_fn, w, v_thread_type_joinable);
	TASK_CHECK(dt != NULL);
	// The single worker is parked in the blocker: the decode and the
	// probe are still queued, and scene_destroy cannot get past them.
	TASK_CHECK(!p.ran);
	struct texture_asset *a = find_asset(w, "task_drain.ppm");
	TASK_CHECK(a != NULL);
	TASK_CHECK(a->decode_count == 0);
	gate_release(&g);
	v_thread_wait(dt);
	// scene_destroy only returned after every queued task ran to
	// completion: the probe saw the decoded, ready asset.
	TASK_CHECK(p.ran);
	TASK_CHECK(p.saw_ready);
	gate_destroy(&g);
	return 0;
}

static char path_single[] = ".task-build/task_single.ppm";

static int case_single_worker_no_deadlock(void) {
	unsigned char ppm[12];
	make_ppm(ppm);
	TASK_CHECK(write_ppm(path_single, ppm, 2, 2) == 0);
	struct ref_img img = { ppm, 2, 2, 3 };
	struct world *w = scene_new();
	use_single_worker(w);
	struct gate g;
	gate_init(&g);
	v_threadpool_enqueue(w->bg_worker, blocker_task, &g);
	gate_wait_started(&g);
	// Linear view requested first, raw view second, all behind the blocker.
	struct cr_color_node dl = image_desc(path_single, SRGB_TRANSFORM | NO_BILINEAR);
	struct cr_color_node dr = image_desc(path_single, NO_BILINEAR);
	const struct colorNode *lin = build_color_node((struct cr_scene *)w, &dl);
	const struct colorNode *raw = build_color_node((struct cr_scene *)w, &dr);
	TASK_CHECK(lin != NULL && raw != NULL);
	gate_release(&g);
	// A single worker must be enough: no background task may occupy the
	// only worker thread while waiting for another queued task.
	v_threadpool_wait(w->bg_worker);
	struct texture_asset *a = find_asset(w, "task_single.ppm");
	TASK_CHECK(a != NULL);
	TASK_CHECK(a->decode_count == 1);
	TASK_CHECK(a->linear_ready && a->linear != NULL);
	for (size_t s = 0; s < NUVS; ++s) {
		float u = g_uvs[s][0], v = g_uvs[s][1];
		TASK_CHECK(close_color(eval_at(raw, u, v), ref_sample(&img, u, v, NO_BILINEAR), 1e-5f));
		TASK_CHECK(close_color(eval_at(lin, u, v), ref_sample(&img, u, v, SRGB_TRANSFORM | NO_BILINEAR), 1e-5f));
	}
	gate_destroy(&g);
	scene_destroy(w);
	return 0;
}

// ---------- transport ----------

static char path_transport[] = ".task-build/task_transport.ppm";
static char path_transport_missing[] = ".task-build/task_transport_missing.ppm";

static int case_transport_roundtrip(void) {
	unsigned char ppm[12];
	make_ppm(ppm);
	TASK_CHECK(write_ppm(path_transport, ppm, 2, 2) == 0);
	unlink(path_transport_missing);
	struct ref_img img = { ppm, 2, 2, 3 };
	static const unsigned opts[4] = { 0, SRGB_TRANSFORM, NO_BILINEAR, SRGB_TRANSFORM | NO_BILINEAR };
	struct world *w = scene_new();
	use_single_worker(w);
	struct gate g;
	gate_init(&g);
	v_threadpool_enqueue(w->bg_worker, blocker_task, &g);
	gate_wait_started(&g);
	// The decode is provably still queued while we serialize below.
	const struct colorNode *nodes[4];
	for (int i = 0; i < 4; ++i) {
		struct cr_color_node d = image_desc(path_transport, opts[i]);
		nodes[i] = build_color_node((struct cr_scene *)w, &d);
		TASK_CHECK(nodes[i] != NULL);
	}
	struct cr_color_node dm = image_desc(path_transport_missing, NO_BILINEAR);
	TASK_CHECK(build_color_node((struct cr_scene *)w, &dm) != NULL);
	struct renderer r = { 0 };
	r.scene = w;
	r.prefs = default_prefs();
	// serialize_renderer must wait for the queued decode to publish
	// before packing the textures; the helper lets the worker proceed
	// while this thread is blocked inside the serialize call.
	v_thread *helper = v_thread_spawn(release_fn, &g, v_thread_type_joinable);
	TASK_CHECK(helper != NULL);
	char *json = serialize_renderer(&r);
	v_thread_wait(helper);
	gate_destroy(&g);
	TASK_CHECK(json != NULL);
	// Source scene evaluates correctly after the serialize barrier.
	for (int i = 0; i < 4; ++i) {
		struct color got = eval_at(nodes[i], 0.25f, 0.25f);
		struct color want = ref_sample(&img, 0.25f, 0.25f, opts[i]);
		TASK_CHECK(close_color(got, want, 1e-5f));
	}
	// Delete the source file and destroy the source renderer's scene.
	TASK_CHECK(unlink(path_transport) == 0);
	scene_destroy(w);
	struct renderer *r2 = deserialize_renderer(json);
	free(json);
	TASK_CHECK(r2 != NULL);
	struct world *w2 = r2->scene;
	struct texture_asset *a = find_asset(w2, "task_transport.ppm");
	TASK_CHECK(a != NULL);
	TASK_CHECK(a->state == tex_asset_ready);
	TASK_CHECK(a->decode_count == 0);       // nothing re-read from disk
	TASK_CHECK(a->t->colorspace == sRGB);   // raw pixels, not pre-converted
	TASK_CHECK(a->t->precision == char_p);
	TASK_CHECK(a->linear_requested);        // view requirement carried over
	// Rebuild the node mapping on the deserialized scene.
	const struct colorNode *nodes2[4];
	for (int i = 0; i < 4; ++i) {
		struct cr_color_node d = image_desc(path_transport, opts[i]);
		nodes2[i] = build_color_node((struct cr_scene *)w2, &d);
		TASK_CHECK(nodes2[i] != NULL);
	}
	// The linear view was regenerated from the raw pixels, not re-converted.
	TASK_CHECK(a->linear_ready && a->linear != NULL && a->linear != a->t);
	TASK_CHECK(a->decode_count == 0);
	for (size_t s = 0; s < NUVS; ++s) {
		for (int i = 0; i < 4; ++i) {
			struct color got = eval_at(nodes2[i], g_uvs[s][0], g_uvs[s][1]);
			struct color want = ref_sample(&img, g_uvs[s][0], g_uvs[s][1], opts[i]);
			TASK_CHECK(close_color(got, want, 1e-5f));
		}
	}
	// The failed asset made the trip too: still pink, still no retry.
	struct texture_asset *f = find_asset(w2, "task_transport_missing.ppm");
	TASK_CHECK(f != NULL);
	TASK_CHECK(f->state == tex_asset_failed);
	TASK_CHECK(f->decode_count == 0);
	struct cr_color_node dm2 = image_desc(path_transport_missing, SRGB_TRANSFORM | NO_BILINEAR);
	const struct colorNode *bad = build_color_node((struct cr_scene *)w2, &dm2);
	TASK_CHECK(bad != NULL);
	TASK_CHECK(is_pink(eval_at(bad, 0.25f, 0.25f)));
	renderer_destroy(r2);
	return 0;
}

static char path_trace_ppm[] = ".task-build/task_trace.ppm";

static int case_transport_path_trace(void) {
	// Uniform image: background sampling is uv-independent.
	static const unsigned char uni[3] = { 96, 160, 224 };
	unsigned char ppm[12];
	for (int i = 0; i < 4; ++i) memcpy(&ppm[i * 3], uni, 3);
	static const unsigned opts[2] = { 0, SRGB_TRANSFORM };
	for (int round = 0; round < 2; ++round) {
		unsigned o = opts[round];
		TASK_CHECK(write_ppm(path_trace_ppm, ppm, 2, 2) == 0);
		struct world *w = scene_new();
		struct cr_color_node imgd = image_desc(path_trace_ppm, o);
		struct cr_shader_node bg = { .type = cr_bsdf_background };
		bg.arg.background.color = &imgd;
		bg.arg.background.pose = NULL;
		bg.arg.background.strength = NULL;
		TASK_CHECK(cr_scene_set_background((struct cr_scene *)w, &bg));
		v_threadpool_wait(w->bg_worker);
		w->topLevel = build_top_level_bvh(w->instances);
		sampler *s = sampler_new();
		sampler_init(s, Halton, 0, 1, 0);
		struct lightRay ray = { .direction = { 0, 0, -1 }, .type = rt_camera };
		struct color before = path_trace(ray, w, 0, s);
		sampler_destroy(s);
		struct renderer r = { 0 };
		r.scene = w;
		r.prefs = default_prefs();
		char *json = serialize_renderer(&r);
		TASK_CHECK(json != NULL);
		TASK_CHECK(unlink(path_trace_ppm) == 0);
		scene_destroy(w);
		struct renderer *r2 = deserialize_renderer(json);
		free(json);
		TASK_CHECK(r2 != NULL);
		TASK_CHECK(r2->scene->background != NULL);
		r2->scene->topLevel = build_top_level_bvh(r2->scene->instances);
		sampler *s2 = sampler_new();
		sampler_init(s2, Halton, 0, 1, 0);
		struct color after = path_trace(ray, r2->scene, 0, s2);
		sampler_destroy(s2);
		struct color want = { uni[0] / 255.0f, uni[1] / 255.0f, uni[2] / 255.0f, 1.0f };
		if (o & SRGB_TRANSFORM) {
			want.red = ref_srgb_to_linear(want.red);
			want.green = ref_srgb_to_linear(want.green);
			want.blue = ref_srgb_to_linear(want.blue);
		}
		// The old non-converted path (options without SRGB_TRANSFORM)
		// keeps its raw values across the roundtrip, and the converted
		// path lands on the same linearized result on both sides.
		// (path_trace accumulates alpha from g_black_color, so only
		// the color channels are compared against the reference.)
		TASK_CHECK(close_rgb(before, want, 1e-5f));
		TASK_CHECK(close_rgb(after, want, 1e-5f));
		TASK_CHECK(close_color(before, after, 1e-6f));
		renderer_destroy(r2);
	}
	return 0;
}

// ---------- registration ----------

const struct task_case task_cases[] = {
	{ "variants", "request_orders", case_request_orders },
	{ "variants", "pending_views", case_pending_views },
	{ "variants", "completed_then_views", case_completed_then_views },
	{ "variants", "alpha_tga", case_alpha_tga },
	{ "variants", "hdr_float_source", case_hdr_float_source },
	{ "variants", "failure_reuse", case_failure_reuse },
	{ "lifecycle", "destroy_drains_tasks", case_destroy_drains_tasks },
	{ "lifecycle", "single_worker_no_deadlock", case_single_worker_no_deadlock },
	{ "transport", "roundtrip_views", case_transport_roundtrip },
	{ "transport", "path_trace_roundtrip", case_transport_path_trace },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
