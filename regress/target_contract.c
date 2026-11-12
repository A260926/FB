/* Contract tests for sft_render_into.
 *
 * The reference image is always produced by the old, tightly packed
 * sft_render entry point, so nothing here depends on how the glyph looks.
 * Allocation accounting is done by routing the library's allocator calls
 * through the counters below (see regress/check_target.sh). */

#include "schrift.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DIM 400
#define SENTINEL 0xA5

/* ---- allocation accounting ------------------------------------------- */

static size_t live_bytes, peak_bytes, largest_block, live_blocks;
static size_t allocation_limit, allocations_seen;
static int    fail_enabled;

typedef struct { size_t size; } Header;

/* The build remaps the standard allocator names to the hooks below, so the
 * hooks themselves must call the real ones. */
#define REAL_MALLOC  sft_real_malloc
#define REAL_CALLOC  sft_real_calloc
#define REAL_REALLOC sft_real_realloc
#define REAL_FREE    sft_real_free

/* Ground truth allocator entry points, unaffected by the macro remapping in
 * the instrumented build. */
void *sft_real_malloc(size_t n);
void *sft_real_calloc(size_t n, size_t s);
void *sft_real_realloc(void *p, size_t n);
void  sft_real_free(void *p);

static int should_fail(void)
{
	return fail_enabled && allocation_limit && allocations_seen + 1 == allocation_limit;
}

static void note_alloc(size_t n)
{
	live_bytes += n;
	if (live_bytes > peak_bytes) peak_bytes = live_bytes;
	if (n > largest_block) largest_block = n;
	live_blocks++;
	allocations_seen++;
}

void *test_malloc(size_t n)
{
	Header *h;
	if (should_fail()) return NULL;
	if (!(h = REAL_MALLOC(n + sizeof *h))) return NULL;
	h->size = n;
	note_alloc(n);
	return h + 1;
}

void *test_calloc(size_t n, size_t s)
{
	Header *h;
	size_t total;
	if (s != 0 && n > SIZE_MAX / s) return NULL;
	total = n * s;
	if (should_fail()) return NULL;
	if (!(h = REAL_CALLOC(1, total + sizeof *h))) return NULL;
	h->size = total;
	note_alloc(total);
	return h + 1;
}

void *test_realloc(void *p, size_t n)
{
	Header *h;
	size_t old;
	if (!p) return test_malloc(n);
	old = ((Header *) p - 1)->size;
	if (n == old) return p;
	if (should_fail()) return NULL;
	if (!(h = REAL_REALLOC((Header *) p - 1, n + sizeof *h))) return NULL;
	if (n > old) note_alloc(n - old); else live_bytes -= old - n;
	if (n > largest_block) largest_block = n;
	h->size = n;
	return h + 1;
}

void test_free(void *p)
{
	Header *h;
	if (!p) return;
	h = (Header *) p - 1;
	live_bytes -= h->size;
	live_blocks--;
	REAL_FREE(h);
}

static void accounting_reset(void)
{
	peak_bytes = largest_block = 0;
	allocation_limit = 0;
	allocations_seen = 0;
	fail_enabled = 0;
}

static void fail_at(size_t n)
{
	accounting_reset();
	allocation_limit = n;
	fail_enabled = 1;
}

/* ---- test helpers ----------------------------------------------------- */

static int failures;

static void fail(const char *what)
{
	printf("FAIL: %s\n", what);
	failures++;
}

static void ok(const char *what)
{
	printf("ok: %s\n", what);
}

static SFT_Font *font;
static SFT sft;
static SFT_Glyph glyph_a, glyph_space, glyph_i, glyph_oe, glyph_bad;

static unsigned char tight[MAX_DIM * MAX_DIM];

/* Renders the reference through the old entry point. */
static int reference(SFT_Glyph glyph, int *w, int *h)
{
	SFT_GMetrics m;
	if (sft_gmetrics(&sft, glyph, &m) < 0) return -1;
	*w = m.minWidth;
	*h = m.minHeight;
	if (*w <= 0 || *h <= 0) return 0;
	if (*w > MAX_DIM || *h > MAX_DIM) return -1;
	memset(tight, 0, (size_t) *w * *h);
	return sft_render(&sft, glyph, (SFT_Image){tight, *w, *h});
}

/* Verifies a placement and returns the number of bad target bytes. */
static long expect_placement(SFT_Glyph glyph, int px, int py, int tw, int th,
                             size_t stride, int fill)
{
	unsigned char *target;
	long bad = 0;
	int w, h, x, y;

	if (reference(glyph, &w, &h) < 0) return -1;
	if (!(target = REAL_MALLOC(stride * (size_t) th))) return -1;
	memset(target, fill, stride * (size_t) th);
	if (sft_render_into(&sft, glyph, (SFT_Target){target, tw, th, stride}, px, py) < 0) {
		REAL_FREE(target);
		return -1;
	}
	for (y = 0; y < th; y++) {
		for (x = 0; x < tw; x++) {
			int sx = x - px, sy = y - py;
			unsigned char want = (sx >= 0 && sx < w && sy >= 0 && sy < h)
			                     ? tight[(size_t) sy * w + sx] : fill;
			if (target[(size_t) y * stride + x] != want) bad++;
		}
		for (x = tw; (size_t) x < stride; x++)
			if (target[(size_t) y * stride + x] != fill) bad++;
	}
	REAL_FREE(target);
	return bad;
}

static void expect_ok(const char *what, SFT_Glyph glyph, int px, int py, int tw, int th,
                      size_t stride, int fill)
{
	long bad = expect_placement(glyph, px, py, tw, th, stride, fill);
	if (bad < 0) fail(what);
	else if (bad) { printf("FAIL: %s (%ld bytes differ)\n", what, bad); failures++; }
	else ok(what);
}

static void expect_error(const char *what, SFT_Target target, SFT_Glyph glyph, int px, int py)
{
	SFT_Target copy = target;
	if (copy.width > 0 && copy.height > 0 && copy.pixels)
		memset(copy.pixels, SENTINEL, (size_t) copy.width * copy.height);
	if (sft_render_into(&sft, glyph, target, px, py) != -1) { fail(what); return; }
	if (copy.pixels && copy.width > 0 && copy.height > 0) {
		size_t i;
		for (i = 0; i < (size_t) copy.width * copy.height; i++)
			if (((unsigned char *) copy.pixels)[i] != SENTINEL) {
				printf("FAIL: %s (target modified)\n", what);
				failures++;
				return;
			}
	}
	ok(what);
}

/* ---- cases ------------------------------------------------------------ */

static void test_stated_example(void)
{
	/* FiraGO 'A' at 24px with offsets (0.25, 0.5), downward Y:
	 * 15x19 covering rectangle with 129 non-zero pixels. */
	unsigned char tight_a[15 * 19];
	unsigned char target[6 * 13];
	SFT_GMetrics m;
	int ink = 0, i;

	sft.font = font; sft.xScale = 24; sft.yScale = 24;
	sft.xOffset = 0.25; sft.yOffset = 0.5; sft.flags = SFT_DOWNWARD_Y;
	if (sft_lookup(&sft, 'A', &glyph_a) < 0 || sft_gmetrics(&sft, glyph_a, &m) < 0) {
		fail("A metrics"); return;
	}
	if (m.minWidth != 15 || m.minHeight != 19) {
		printf("FAIL: A is %dx%d, expected 15x19\n", m.minWidth, m.minHeight); failures++;
	} else ok("A covering rectangle is 15x19");

	memset(tight_a, 0, sizeof tight_a);
	if (sft_render(&sft, glyph_a, (SFT_Image){tight_a, 15, 19}) < 0) { fail("A render"); return; }
	for (i = 0; i < 15 * 19; i++) ink += tight_a[i] != 0;
	if (ink != 129) {
		printf("FAIL: A has %d non-zero pixels, expected 129\n", ink); failures++;
	} else ok("A has 129 non-zero pixels");

	memset(target, SENTINEL, sizeof target);
	if (sft_render_into(&sft, glyph_a, (SFT_Target){target, 8, 6, 13}, -3, -4) < 0) {
		fail("A at (-3,-4)"); return;
	}
	if (target[0] != tight_a[4 * 15 + 3]) {
		printf("FAIL: target(0,0)=%u, tight(3,4)=%u\n", target[0], tight_a[4 * 15 + 3]);
		failures++;
	} else ok("target(0,0) equals the tight source at (3,4)");
	for (i = 0; i < 6; i++) {
		if (target[i * 13 + 8] != SENTINEL || target[i * 13 + 12] != SENTINEL) {
			fail("row padding untouched at (-3,-4)"); return;
		}
	}
	ok("row padding untouched at (-3,-4)");

	memset(target, SENTINEL, sizeof target);
	if (sft_render_into(&sft, glyph_a, (SFT_Target){target, 8, 6, 13}, 8, 0) < 0) {
		fail("A at (8,0)"); return;
	}
	for (i = 0; i < 6 * 13; i++) if (target[i] != SENTINEL) {
		fail("A at (8,0) writes nothing"); return;
	}
	ok("A at (8,0) writes nothing");
}

static void test_placements(void)
{
	int w, h;
	if (reference(glyph_a, &w, &h) < 0) { fail("reference"); return; }
	expect_ok("negative placement clips", glyph_a, -3, -4, 8, 6, 13, SENTINEL);
	expect_ok("fully outside on the right", glyph_a, 8, 0, 8, 6, 13, SENTINEL);
	expect_ok("fully outside below", glyph_a, 0, 6, 8, 6, 13, SENTINEL);
	expect_ok("fully outside on the left", glyph_a, -w - 1, 2, 8, 6, 13, SENTINEL);
	expect_ok("fully outside above", glyph_a, 2, -h - 1, 8, 6, 13, SENTINEL);
	expect_ok("positive placement inside", glyph_a, 2, 1, 8, 6, 13, SENTINEL);
	expect_ok("exactly filling a larger target", glyph_a, 0, 0, w, h, (size_t) w, SENTINEL);
	expect_ok("straddling all edges", glyph_a, -2, -3, 4, 4, 9, SENTINEL);
}

static void test_directions_and_offsets(void)
{
	int flags[] = { 0, SFT_DOWNWARD_Y };
	int fi, n;

	for (fi = 0; fi < 2; fi++)
	for (n = 0; n < 4; n++) {
		char what[80];
		const char *dir = flags[fi] ? "downward" : "upward";
		sft.flags = flags[fi];
		sft.xOffset = (n == 0) ? 0.0 : 0.25 + 0.125 * n;
		sft.yOffset = (n == 0) ? 0.0 : 0.5 - 0.125 * n;
		strcpy(what, "Y direction ");
		strcat(what, dir);
		strcat(what, " with a fractional offset");
		expect_ok(what, glyph_a, -2, -3, 9, 8, 13, SENTINEL);
	}
	sft.xOffset = 0.25; sft.yOffset = 0.5; sft.flags = SFT_DOWNWARD_Y;
}

static void test_compound_and_space(void)
{
	sft.font = font; sft.xScale = 24; sft.yScale = 24;
	sft.xOffset = 0.25; sft.yOffset = 0.5; sft.flags = SFT_DOWNWARD_Y;
	if (sft_lookup(&sft, 0x00C4, &glyph_oe) < 0) fail("lookup A-umlaut");
	else {
		/* A-umlaut and i are built from other outlines in FiraGO. */
		expect_ok("compound glyph, downward Y", glyph_oe, -2, -3, 10, 8, 15, SENTINEL);
		sft.flags = 0;
		expect_ok("compound glyph, upward Y", glyph_oe, -2, -3, 10, 8, 15, SENTINEL);
		expect_ok("compound glyph i, upward Y", glyph_i, -1, -2, 8, 7, 11, SENTINEL);
		sft.flags = SFT_DOWNWARD_Y;
	}
	if (sft_lookup(&sft, ' ', &glyph_space) < 0) fail("lookup space"); else {
		unsigned char target[4 * 4];
		memset(target, SENTINEL, sizeof target);
		if (sft_render_into(&sft, glyph_space, (SFT_Target){target, 4, 4, 4}, 1, 1) != 0)
			fail("space returns 0");
		else if (target[0] != SENTINEL || target[15] != SENTINEL)
			fail("space writes nothing");
		else ok("space writes nothing");
	}
}

static void test_shared_atlas(void)
{
	/* Two glyphs written into distinct cells of the same padded atlas. The
	 * padding and the gap between cells are pre-existing content that must
	 * survive both renders. */
	unsigned char atlas[32 * 19];
	unsigned char ref_a[15 * 19], ref_b[13 * 19];
	SFT_GMetrics ma, mb;
	int x, y;

	sft.font = font; sft.xScale = 24; sft.yScale = 24;
	sft.xOffset = 0.25; sft.yOffset = 0.5; sft.flags = SFT_DOWNWARD_Y;
	if (sft_gmetrics(&sft, glyph_a, &ma) < 0) { fail("atlas metrics A"); return; }
	if (sft_lookup(&sft, 'B', &glyph_i) < 0) { fail("lookup B"); return; }
	if (sft_gmetrics(&sft, glyph_i, &mb) < 0) { fail("atlas metrics B"); return; }
	if (ma.minWidth != 15 || ma.minHeight != 19 || mb.minWidth != 13 || mb.minHeight != 19) {
		printf("FAIL: unexpected cell sizes %dx%d / %dx%d\n",
		       ma.minWidth, ma.minHeight, mb.minWidth, mb.minHeight);
		failures++;
		return;
	}
	memset(ref_a, 0, sizeof ref_a);
	memset(ref_b, 0, sizeof ref_b);
	if (sft_render(&sft, glyph_a, (SFT_Image){ref_a, 15, 19}) < 0) { fail("atlas render A"); return; }
	if (sft_render(&sft, glyph_i, (SFT_Image){ref_b, 13, 19}) < 0) { fail("atlas render B"); return; }

	memset(atlas, 0xA5, sizeof atlas);
	/* A at cell column 0, B at column 15; four bytes of padding per row. */
	if (sft_render_into(&sft, glyph_a, (SFT_Target){atlas, 28, 19, 32}, 0, 0) < 0 ||
	    sft_render_into(&sft, glyph_i, (SFT_Target){atlas, 28, 19, 32}, 15, 0) < 0) {
		fail("atlas renders");
		return;
	}
	for (y = 0; y < 19; y++) {
		for (x = 0; x < 15; x++)
			if (atlas[y * 32 + x] != ref_a[y * 15 + x]) { fail("atlas cell A"); return; }
		for (x = 15; x < 28; x++)
			if (atlas[y * 32 + x] != ref_b[y * 13 + (x - 15)]) { fail("atlas cell B"); return; }
		for (x = 28; x < 32; x++)
			if (atlas[y * 32 + x] != SENTINEL) { fail("atlas padding untouched"); return; }
	}
	ok("shared atlas cells and padding");
}

static void test_errors(void)
{
	unsigned char pixels[64];
	SFT_Target t;

	sft.font = font; sft.xScale = 24; sft.yScale = 24;
	sft.xOffset = 0.25; sft.yOffset = 0.5; sft.flags = SFT_DOWNWARD_Y;

	t = (SFT_Target){pixels, -1, 4, 0};
	expect_error("negative width", t, glyph_a, 0, 0);
	t = (SFT_Target){pixels, 4, -1, 4};
	expect_error("negative height", t, glyph_a, 0, 0);
	t = (SFT_Target){pixels, 4, 4, 3};
	expect_error("stride smaller than width", t, glyph_a, 0, 0);
	t = (SFT_Target){NULL, 4, 4, 4};
	expect_error("NULL pixels in non-empty target", t, glyph_a, 0, 0);
	t = (SFT_Target){pixels, 4, 4, SIZE_MAX / 2};
	expect_error("accessible span overflows", t, glyph_a, 0, 0);

	/* Order: negative dimensions before glyph validation. */
	t = (SFT_Target){pixels, -4, 4, 0};
	if (sft_render_into(&sft, glyph_bad, t, 0, 0) != -1) fail("negative dimensions precede glyph validation");
	else ok("negative dimensions precede glyph validation");

	/* Invalid glyph is -1 even with an empty target. */
	t = (SFT_Target){NULL, 0, 0, 0};
	if (sft_render_into(&sft, glyph_bad, t, 0, 0) != -1) fail("invalid glyph with empty target");
	else ok("invalid glyph with empty target");
	t = (SFT_Target){NULL, 4, 4, 4};
	if (sft_render_into(&sft, glyph_bad, t, 0, 0) != -1) fail("invalid glyph with NULL target");
	else ok("invalid glyph with NULL target");

	/* Zero extent is a successful no-op with NULL pointer and odd stride. */
	t = (SFT_Target){NULL, 0, 4, 0};
	if (sft_render_into(&sft, glyph_a, t, 0, 0) != 0) fail("zero width no-op");
	else ok("zero width is a successful no-op");
	t = (SFT_Target){NULL, 4, 0, 12345};
	if (sft_render_into(&sft, glyph_a, t, 0, 0) != 0) fail("zero height no-op");
	else ok("zero height is a successful no-op");

	/* Extreme placements must not overflow; nothing lands in this target. */
	{
		unsigned char big[6 * 13];
		int i;
		memset(big, SENTINEL, sizeof big);
		t = (SFT_Target){big, 8, 6, 13};
		if (sft_render_into(&sft, glyph_a, t, INT_MIN, INT_MIN) != 0) fail("INT_MIN placement");
		else {
			for (i = 0; i < 6 * 13; i++)
				if (big[i] != SENTINEL) { fail("INT_MIN placement writes nothing"); break; }
			if (i == 6 * 13) ok("INT_MIN placement clips without overflow");
		}
		memset(big, SENTINEL, sizeof big);
		if (sft_render_into(&sft, glyph_a, t, INT_MAX, INT_MAX) != 0) fail("INT_MAX placement");
		else ok("INT_MAX placement clips without overflow");
	}
}

static void test_allocation_bound(void)
{
	SFT_GMetrics m;
	SFT_Target t;
	unsigned char target[9];
	int w, h;
	unsigned long long full;

	sft.font = font; sft.xScale = 4096; sft.yScale = 4096;
	sft.xOffset = 0; sft.yOffset = 0; sft.flags = SFT_DOWNWARD_Y;
	if (sft_gmetrics(&sft, glyph_a, &m) < 0) { fail("4096 metrics"); return; }
	w = m.minWidth; h = m.minHeight;

	/* A huge glyph of which only a 3x2 sliver is visible. */
	accounting_reset();
	t = (SFT_Target){target, 3, 2, 3};
	if (sft_render_into(&sft, glyph_a, t, -(w - 3), -(h - 2)) < 0) { fail("sliver render"); return; }
	full = (unsigned long long) w * (unsigned long long) h;
	printf("ok: sliver of a %dx%d glyph (%llu pixels) peaks at %zu bytes, largest block %zu\n",
	       w, h, full, peak_bytes, largest_block);
	/* The intersection is six pixels. Allow a generous constant for the
	 * decoded outline and per-pixel cell storage (16 bytes) plus byte output,
	 * but nowhere near the full bitmap (which would be > 100 MB). */
	if (largest_block > 64 * 1024) {
		printf("FAIL: largest single allocation %zu is not area-bound\n", largest_block);
		failures++;
	}
	if (full > 100000 && peak_bytes > 64 * 1024) {
		printf("FAIL: peak %zu is not area-bound for a 6-pixel intersection\n", peak_bytes);
		failures++;
	}

	/* A larger intersection may cost more, but still only in proportion to
	 * the visible area and never to the full glyph. */
	{
		unsigned char wide[64 * 48];
		accounting_reset();
		t = (SFT_Target){wide, 64, 48, 64};
		if (sft_render_into(&sft, glyph_a, t, -(w - 64), -(h - 48)) < 0) {
			fail("wide sliver render");
			return;
		}
		printf("ok: 64x48 visible area peaks at %zu bytes (full bitmap remains %llu)\n",
		       peak_bytes, full);
		if (peak_bytes > 64 * 48 * 100) {
			printf("FAIL: peak %zu exceeds a constant factor of the visible area\n",
			       peak_bytes);
			failures++;
		}
	}
}

static void test_failure_reuse(void)
{
	/* Force every allocation to fail in turn and verify that the target is
	 * either fully written or untouched, that nothing leaks, and that the
	 * same buffer can be reused afterwards with an identical result. */
	SFT_Glyph g = glyph_a;
	SFT_Target t;
	unsigned char target[16 * 16];
	unsigned char expected[16 * 16];
	size_t n, baseline_blocks;
	int saw_failure = 0, saw_success = 0;

	sft.font = font; sft.xScale = 24; sft.yScale = 24;
	sft.xOffset = 0.25; sft.yOffset = 0.5; sft.flags = SFT_DOWNWARD_Y;
	t = (SFT_Target){target, 16, 16, 16};

	baseline_blocks = live_blocks; /* the loaded font is one of them */
	accounting_reset();
	memset(target, SENTINEL, sizeof target);
	if (sft_render_into(&sft, g, t, 4, 4) != 0) { fail("baseline for reuse"); return; }
	memcpy(expected, target, sizeof expected);

	for (n = 1; n <= 80; n++) {
		int rc;
		size_t i;

		memset(target, SENTINEL, sizeof target);
		fail_at(n);
		rc = sft_render_into(&sft, g, t, 4, 4);
		accounting_reset();

		if (rc == -1) {
			saw_failure = 1;
			for (i = 0; i < sizeof target; i++)
				if (target[i] != SENTINEL) {
					printf("FAIL: failed render left partial byte at %zu\n", i);
					failures++;
					break;
				}
		} else if (rc == 0) {
			saw_success = 1;
			if (memcmp(target, expected, sizeof target) != 0) {
				printf("FAIL: render after a late injection point differs at n=%zu\n", n);
				failures++;
			}
		} else {
			fail("unexpected return code");
		}

		/* Reuse the buffer, with injection disabled, after every failure. */
		memset(target, SENTINEL, sizeof target);
		if (sft_render_into(&sft, g, t, 4, 4) != 0) {
			printf("FAIL: buffer unusable after failure at n=%zu\n", n);
			failures++;
			break;
		}
		if (memcmp(target, expected, sizeof target) != 0) {
			printf("FAIL: reused buffer differs after failure at n=%zu\n", n);
			failures++;
			break;
		}
	}
	if (!saw_failure || !saw_success) fail("failure injection did not exercise both paths");
	else ok("failed renders leave no partial glyph and the buffer stays usable");
	if (live_blocks != baseline_blocks) {
		printf("FAIL: %zu blocks leaked\n", live_blocks - baseline_blocks);
		failures++;
	} else ok("no allocations leaked");
}

static void test_large_span(void)
{
	/* The overflow check must be done without reading the target. */
	unsigned char pixels[8];
	SFT_Target t = { pixels, 4, 4, SIZE_MAX / 2 };
	if (sft_render_into(&sft, glyph_a, t, 0, 0) != -1) fail("SIZE_MAX stride span");
	else ok("SIZE_MAX stride span is rejected before any access");
	t = (SFT_Target){ pixels, 4, 4, 4 };
	if (sft_render_into(&sft, glyph_bad, t, 0, 0) != -1) fail("invalid glyph after valid target checks");
	else ok("invalid glyph is rejected after target validation");
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "resources/FiraGO-Regular.ttf";

	if (!(font = sft_loadfile(path))) {
		printf("cannot load %s\n", path);
		return 2;
	}

	sft.font = font; sft.xScale = 24; sft.yScale = 24;
	sft.xOffset = 0.25; sft.yOffset = 0.5; sft.flags = SFT_DOWNWARD_Y;
	if (sft_lookup(&sft, 'A', &glyph_a) < 0) { printf("no A\n"); return 2; }
	if (sft_lookup(&sft, 'i', &glyph_i) < 0) { printf("no i\n"); return 2; } /* compound glyph */
	glyph_bad = (SFT_Glyph) 0xfffffffeu; /* out of range for this font */

	test_stated_example();
	test_placements();
	test_directions_and_offsets();
	test_compound_and_space();
	test_errors();
	test_large_span();
	test_shared_atlas();

	/* Memory tests only make sense when the allocator is instrumented. */

#ifdef SFT_TEST_TRACK_ALLOCATIONS
	test_allocation_bound();
	test_failure_reuse();
#else
	(void)test_allocation_bound;
	(void)test_failure_reuse;
	printf("skip: allocation tracking disabled\n");
#endif

	sft_freefont(font);

	printf(failures ? "FAILED: %d failure(s)\n" : "PASS: target contract\n", failures);
	return failures ? 1 : 0;
}
