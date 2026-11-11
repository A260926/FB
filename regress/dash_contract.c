/*
 * dash_contract.c - regression contract tests for plutovg dashed stroking.
 *
 * What is covered (three public entry points, one shared contract):
 *   1. plutovg_path_traverse_dashed  (path-level, callback segments)
 *   2. plutovg_path_clone_dashed     (path-level, cloned geometry)
 *   3. plutovg_canvas_stroke_*       (pixel-level, dashed vs. cloned vs. solid)
 *
 * The expected visible intervals are derived here, outside the library,
 * straight from the dash contract:
 *   - the on/off pattern is the dash array repeated; for an odd number of
 *     elements the array is walked twice before the pattern repeats
 *     (period = sum(dashes), doubled when ndashes is odd);
 *   - the offset is normalized into [0, period), so offsets that differ by
 *     a whole period cover the same region;
 *   - phase continues across collinear LINE_TO segments of a subpath and
 *     restarts from the same initial offset at every MOVE_TO;
 *   - even pattern indices are "on", odd ones are "off";
 *   - dash lengths are measured in path coordinates, before any canvas
 *     transform is applied.
 *
 * Both library traversal entries share one implementation, so they are never
 * compared against each other -- each is compared against the independent
 * model below.  At the pixel level a dashed stroke, a clone_dashed+plain
 * stroke, and a stroke of the model's solid intervals must agree.
 *
 * Callback quirks that are explicitly tolerated: redundant MOVE_TOs and
 * zero-length segments at dash boundaries carry no coverage and are dropped
 * before comparison.  Coordinates are compared with 1e-4 tolerance.
 *
 * Inputs are restricted to: finite coordinates, strictly positive finite
 * dash elements, finite offsets, non-degenerate polylines.
 *
 * Run via: sh regress/check_dash.sh
 * Mutation self-check: sh regress/check_dash.sh --mutations
 */
#include "plutovg.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOL 1e-4f        /* allowed coordinate error for coverage comparison */
#define MERGE_TOL 1e-6f  /* gap below which touching runs are joined */

static int failures;
static const char* current_test;

static void fail(const char* entry, const char* fmt, ...)
{
    va_list ap;
    failures++;
    printf("FAIL [%s] entry=%s: ", current_test, entry);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* ------------------------------------------------------------------ */
/* Visible runs: the coverage of a dashed polyline as point pairs.     */

typedef struct { float x, y; } pt_t;
typedef struct { pt_t a, b; } run_t;
typedef struct { run_t* v; int n, cap; } run_list_t;

static float pt_dist(pt_t a, pt_t b)
{
    return hypotf(a.x - b.x, a.y - b.y);
}

static void runs_push(run_list_t* l, pt_t a, pt_t b)
{
    if(l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = realloc(l->v, (size_t)l->cap * sizeof(run_t));
        if(!l->v) { fprintf(stderr, "out of memory\n"); exit(2); }
    }
    l->v[l->n].a = a;
    l->v[l->n].b = b;
    l->n++;
}

/* Join consecutive runs whose endpoints touch (within MERGE_TOL). */
static void runs_merge(run_list_t* l)
{
    int w = 0;
    for(int i = 0; i < l->n; ++i) {
        if(w > 0 && pt_dist(l->v[w - 1].b, l->v[i].a) <= MERGE_TOL)
            l->v[w - 1].b = l->v[i].b;
        else
            l->v[w++] = l->v[i];
    }
    l->n = w;
}

static void runs_free(run_list_t* l)
{
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Independent reference model, derived from the dash contract only.   */

typedef struct { const pt_t* pts; int n; } subpath_t;

static void model_runs(run_list_t* out, const subpath_t* subs, int nsubs,
                       float offset, const float* dashes, int ndashes)
{
    float period = 0.f;
    for(int i = 0; i < ndashes; ++i)
        period += dashes[i];
    if(ndashes % 2 == 1)
        period *= 2.f; /* odd arrays need two passes for a full on/off cycle */

    float o = fmodf(offset, period);
    if(o < 0.f)
        o += period; /* negative offsets wrap into [0, period) */

    for(int s = 0; s < nsubs; ++s) {
        /* every subpath restarts from the same normalized offset */
        int idx = 0, on = 1;
        float phase = o;
        for(;;) {
            float d = dashes[idx % ndashes];
            if(phase < d)
                break;
            phase -= d;
            on = !on;
            idx++;
        }
        for(int i = 0; i + 1 < subs[s].n; ++i) {
            pt_t p0 = subs[s].pts[i], p1 = subs[s].pts[i + 1];
            float len = pt_dist(p0, p1);
            if(len == 0.f)
                continue;
            float t = 0.f;
            while(t < len) {
                float d = dashes[idx % ndashes] - phase;
                float step = len - t;
                if(step > d)
                    step = d;
                if(on && step > 0.f) {
                    float t0 = t / len, t1 = (t + step) / len;
                    pt_t a = { p0.x + t0 * (p1.x - p0.x), p0.y + t0 * (p1.y - p0.y) };
                    pt_t b = { p0.x + t1 * (p1.x - p0.x), p0.y + t1 * (p1.y - p0.y) };
                    runs_push(out, a, b);
                }
                t += step;
                if(step < d) {
                    phase += step;
                } else {
                    phase = 0.f;
                    on = !on;
                    idx++;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Collect effective coverage from a path traversal callback.          */
/* Redundant MOVE_TOs and zero-length segments are ignored.            */

typedef struct {
    run_list_t runs;
    pt_t cur;
    int have_cur;
    pt_t run_end;
    int open;
} collect_t;

static void collect_cb(void* closure, plutovg_path_command_t command,
                       const plutovg_point_t* points, int npoints)
{
    collect_t* c = (collect_t*)(closure);
    (void)npoints;
    if(command == PLUTOVG_PATH_COMMAND_MOVE_TO) {
        c->cur.x = points[0].x;
        c->cur.y = points[0].y;
        c->have_cur = 1;
        c->open = 0;
        return;
    }
    if(command != PLUTOVG_PATH_COMMAND_LINE_TO || !c->have_cur)
        return; /* the test inputs flatten to lines only */
    pt_t p = { points[0].x, points[0].y };
    pt_t a = c->cur;
    c->cur = p;
    if(pt_dist(a, p) <= MERGE_TOL)
        return; /* zero-length segment: no visible coverage */
    if(c->open && pt_dist(c->run_end, a) <= MERGE_TOL) {
        c->runs.v[c->runs.n - 1].b = p; /* extend the current run */
    } else {
        runs_push(&c->runs, a, p);
    }
    c->run_end = p;
    c->open = 1;
}

static void collect_reset(collect_t* c)
{
    memset(c, 0, sizeof(*c));
}

/* ------------------------------------------------------------------ */
/* Comparisons with first-difference reporting.                        */

static void compare_runs(const char* entry, const run_list_t* got, const run_list_t* exp)
{
    int n = got->n < exp->n ? got->n : exp->n;
    for(int i = 0; i < n; ++i) {
        if(pt_dist(got->v[i].a, exp->v[i].a) > TOL || pt_dist(got->v[i].b, exp->v[i].b) > TOL) {
            fail(entry, "run %d differs: got (%.6f,%.6f)->(%.6f,%.6f), expected (%.6f,%.6f)->(%.6f,%.6f)",
                 i, got->v[i].a.x, got->v[i].a.y, got->v[i].b.x, got->v[i].b.y,
                 exp->v[i].a.x, exp->v[i].a.y, exp->v[i].b.x, exp->v[i].b.y);
            return;
        }
    }
    if(got->n != exp->n)
        fail(entry, "run count %d, expected %d (first divergence at run %d)", got->n, exp->n, n);
}

static plutovg_path_t* make_path(const subpath_t* subs, int nsubs)
{
    plutovg_path_t* path = plutovg_path_create();
    for(int s = 0; s < nsubs; ++s) {
        plutovg_path_move_to(path, subs[s].pts[0].x, subs[s].pts[0].y);
        for(int i = 1; i < subs[s].n; ++i)
            plutovg_path_line_to(path, subs[s].pts[i].x, subs[s].pts[i].y);
    }
    return path;
}

/* ------------------------------------------------------------------ */
/* Path-level contract: both public entries vs. the independent model. */

static void check_path_case(const char* name, const subpath_t* subs, int nsubs,
                            float offset, const float* dashes, int ndashes)
{
    current_test = name;

    run_list_t exp = { 0 };
    model_runs(&exp, subs, nsubs, offset, dashes, ndashes);
    runs_merge(&exp);

    plutovg_path_t* path = make_path(subs, nsubs);

    collect_t c1;
    collect_reset(&c1);
    plutovg_path_traverse_dashed(path, offset, dashes, ndashes, collect_cb, &c1);
    runs_merge(&c1.runs);
    compare_runs("plutovg_path_traverse_dashed", &c1.runs, &exp);

    plutovg_path_t* clone = plutovg_path_clone_dashed(path, offset, dashes, ndashes);
    collect_t c2;
    collect_reset(&c2);
    plutovg_path_traverse(clone, collect_cb, &c2);
    runs_merge(&c2.runs);
    compare_runs("plutovg_path_clone_dashed", &c2.runs, &exp);

    collect_reset(&c1);
    runs_free(&c1.runs);
    collect_reset(&c2);
    runs_free(&c2.runs);
    runs_free(&exp);
    plutovg_path_destroy(clone);
    plutovg_path_destroy(path);
}

/* ------------------------------------------------------------------ */
/* Pixel-level contract: dashed stroke vs. clone_dashed+stroke vs.     */
/* stroke of the model's solid intervals, all on small butt-cap canvas.*/

typedef struct {
    plutovg_surface_t* surface;
    plutovg_canvas_t* canvas;
    unsigned char* data; /* copied out so surfaces can be freed */
    int stride;
} bitmap_t;

static bitmap_t render_stroke(int w, int h, float sx, float sy,
                              const plutovg_path_t* path,
                              int use_dash, float offset, const float* dashes, int ndashes)
{
    bitmap_t r;
    r.surface = plutovg_surface_create(w, h);
    r.canvas = plutovg_canvas_create(r.surface);
    plutovg_canvas_set_rgba(r.canvas, 1, 0, 0, 1);
    plutovg_canvas_set_line_width(r.canvas, 1);
    plutovg_canvas_set_line_cap(r.canvas, PLUTOVG_LINE_CAP_BUTT);
    if(sx != 1.f || sy != 1.f)
        plutovg_canvas_scale(r.canvas, sx, sy);
    if(use_dash)
        plutovg_canvas_set_dash(r.canvas, offset, dashes, ndashes);
    plutovg_canvas_stroke_path(r.canvas, path);
    r.stride = plutovg_surface_get_stride(r.surface);
    r.data = malloc((size_t)r.stride * h);
    if(!r.data) { fprintf(stderr, "out of memory\n"); exit(2); }
    memcpy(r.data, plutovg_surface_get_data(r.surface), (size_t)r.stride * h);
    return r;
}

static void bitmap_free(bitmap_t* r)
{
    free(r->data);
    plutovg_canvas_destroy(r->canvas);
    plutovg_surface_destroy(r->surface);
}

static void compare_bitmaps(const char* entry_a, const bitmap_t* a,
                            const char* entry_b, const bitmap_t* b,
                            int w, int h)
{
    for(int y = 0; y < h; ++y) {
        for(int x = 0; x < w; ++x) {
            const unsigned char* pa = a->data + y * a->stride + x * 4;
            const unsigned char* pb = b->data + y * b->stride + x * 4;
            if(memcmp(pa, pb, 4) != 0) {
                char entry[160];
                snprintf(entry, sizeof(entry), "%s vs %s", entry_a, entry_b);
                fail(entry, "first pixel diff at (%d,%d): %s=%02x%02x%02x%02x, %s=%02x%02x%02x%02x",
                     x, y, entry_a, pa[0], pa[1], pa[2], pa[3],
                     entry_b, pb[0], pb[1], pb[2], pb[3]);
                return;
            }
        }
    }
}

static void check_pixel_case(const char* name, int w, int h, float sx, float sy,
                             const subpath_t* subs, int nsubs,
                             float offset, const float* dashes, int ndashes)
{
    current_test = name;

    run_list_t exp = { 0 };
    model_runs(&exp, subs, nsubs, offset, dashes, ndashes);
    runs_merge(&exp);
    if(exp.n == 0) {
        fail("model", "reference produced no visible runs; test would be vacuous");
        runs_free(&exp);
        return;
    }

    /* reference geometry: the model's visible intervals as solid segments */
    plutovg_path_t* solid = plutovg_path_create();
    for(int i = 0; i < exp.n; ++i) {
        plutovg_path_move_to(solid, exp.v[i].a.x, exp.v[i].a.y);
        plutovg_path_line_to(solid, exp.v[i].b.x, exp.v[i].b.y);
    }

    plutovg_path_t* path = make_path(subs, nsubs);
    plutovg_path_t* clone = plutovg_path_clone_dashed(path, offset, dashes, ndashes);

    bitmap_t dashed = render_stroke(w, h, sx, sy, path, 1, offset, dashes, ndashes);
    bitmap_t cloned = render_stroke(w, h, sx, sy, clone, 0, 0, NULL, 0);
    bitmap_t reference = render_stroke(w, h, sx, sy, solid, 0, 0, NULL, 0);

    int nonzero = 0;
    for(int i = 0; i < reference.stride * h; ++i)
        nonzero += reference.data[i] != 0;
    if(!nonzero)
        fail("solid-interval reference", "reference rendered nothing; test would be vacuous");

    compare_bitmaps("canvas dashed stroke", &dashed, "solid-interval reference", &reference, w, h);
    compare_bitmaps("clone_dashed+plain stroke", &cloned, "solid-interval reference", &reference, w, h);

    bitmap_free(&dashed);
    bitmap_free(&cloned);
    bitmap_free(&reference);
    plutovg_path_destroy(solid);
    plutovg_path_destroy(clone);
    plutovg_path_destroy(path);
    runs_free(&exp);
}

/* ------------------------------------------------------------------ */
/* Individual contract rules.                                          */

static void test_odd_array_period(void)
{
    /* [3,2,1] has period 12: on3 off2 on1 off3 on2 off1.
     * Visible on (0,0)->(13,0): [0,3] [5,6] [9,11] [12,13]. */
    const pt_t pts[] = { {0, 0}, {13, 0} };
    const subpath_t subs[] = { { pts, 2 } };
    const float dashes[] = { 3, 2, 1 };
    check_path_case("odd_array_period", subs, 1, 0.f, dashes, 3);
}

static void test_offset_period_equivalence(void)
{
    /* Offsets that differ by a whole period (12 for [3,2,1]) must cover the
     * same region; offset -1 wraps to 11. Known answer on (1,2)->(13,2):
     * [2,5] [7,8] [11,13]. */
    const pt_t pts[] = { {1, 2}, {13, 2} };
    const subpath_t subs[] = { { pts, 2 } };
    const float dashes[] = { 3, 2, 1 };
    check_path_case("offset_negative_wraps", subs, 1, -1.f, dashes, 3);
    check_path_case("offset_plus_period", subs, 1, 11.f, dashes, 3);
    check_path_case("offset_minus_period", subs, 1, -13.f, dashes, 3);

    /* Even array [4,2], period 6: offsets -3 and 3 agree. */
    const pt_t pts2[] = { {0, 0}, {14, 0} };
    const subpath_t subs2[] = { { pts2, 2 } };
    const float dashes2[] = { 4, 2 };
    check_path_case("offset_even_negative", subs2, 1, -3.f, dashes2, 2);
    check_path_case("offset_even_positive", subs2, 1, 3.f, dashes2, 2);
}

static void test_collinear_lineto_continuation(void)
{
    /* Splitting a straight line with collinear LINE_TOs must not restart
     * the dash phase: same coverage as the single-segment case. */
    const pt_t pts[] = { {0, 0}, {5, 0}, {9, 0}, {13, 0} };
    const subpath_t subs[] = { { pts, 4 } };
    const float dashes[] = { 3, 2, 1 };
    check_path_case("collinear_lineto_continuation", subs, 1, -1.f, dashes, 3);
}

static void test_moveto_restarts_phase(void)
{
    /* Each subpath restarts from the same initial offset: both rows get
     * identical coverage, shifted in y. */
    const pt_t row1[] = { {0, 0}, {8, 0} };
    const pt_t row2[] = { {0, 4}, {8, 4} };
    const subpath_t subs[] = { { row1, 2 }, { row2, 2 } };
    const float dashes[] = { 3, 2, 1 };
    check_path_case("moveto_restarts_phase", subs, 2, -1.f, dashes, 3);
}

static void test_exact_dash_boundary(void)
{
    /* Endpoints landing exactly on dash boundaries: no zero-length segment
     * may count as visible length. [3,2] on (0,0)->(3,0): exactly [0,3]. */
    const float dashes[] = { 3, 2 };
    const pt_t a[] = { {0, 0}, {3, 0} };
    const subpath_t sa[] = { { a, 2 } };
    check_path_case("boundary_at_line_end", sa, 1, 0.f, dashes, 2);

    /* (0,0)->(10,0): [0,3] [5,8]; boundaries at 3, 5, 8 hit exactly. */
    const pt_t b[] = { {0, 0}, {10, 0} };
    const subpath_t sb[] = { { b, 2 } };
    check_path_case("boundary_multiple_hits", sb, 1, 0.f, dashes, 2);

    /* Offset exactly on an on/off boundary: offset 3 consumes the on3,
     * so coverage starts after the off2: [2,5] on (0,0)->(5,0). */
    const pt_t c[] = { {0, 0}, {5, 0} };
    const subpath_t sc[] = { { c, 2 } };
    check_path_case("boundary_offset_exact", sc, 1, 3.f, dashes, 2);
}

static void test_pixels_baseline(void)
{
    /* The original probe case, now checked against the independent model:
     * (1,2)->(13,2), [3,2,1], offset -1 == solid [2,5] [7,8] [11,13]. */
    const pt_t pts[] = { {1, 2}, {13, 2} };
    const subpath_t subs[] = { { pts, 2 } };
    const float dashes[] = { 3, 2, 1 };
    check_pixel_case("pixels_baseline", 16, 5, 1.f, 1.f, subs, 1, -1.f, dashes, 3);
}

static void test_pixels_odd_negative_second_subpath(void)
{
    /* Odd array + negative offset + a second subpath in one case. */
    const pt_t row1[] = { {1, 2}, {13, 2} };
    const pt_t row2[] = { {2, 5}, {12, 5} };
    const subpath_t subs[] = { { row1, 2 }, { row2, 2 } };
    const float dashes[] = { 3, 2, 1 };
    check_pixel_case("pixels_odd_negative_second_subpath", 16, 8, 1.f, 1.f, subs, 2, -1.f, dashes, 3);
}

static void test_pixels_nonuniform_scale(void)
{
    /* Dash lengths are measured in path coordinates, then transformed.
     * scale(2,1): [2,1] on (1,3)->(7,3) covers path x [1,3] [4,6],
     * i.e. device x [2,6] [8,12] -- not a re-dash of the stretched line. */
    const pt_t h[] = { {1, 3}, {7, 3} };
    const subpath_t sh[] = { { h, 2 } };
    const float dashes[] = { 2, 1 };
    check_pixel_case("pixels_scale_x2", 24, 7, 2.f, 1.f, sh, 1, 0.f, dashes, 2);

    /* scale(1,3) with a fractional offset: path y [1,2.5] [3.5,5.5] [6.5,7],
     * device y [3,7.5] [10.5,16.5] [19.5,21]. */
    const pt_t v[] = { {3, 1}, {3, 7} };
    const subpath_t sv[] = { { v, 2 } };
    check_pixel_case("pixels_scale_y3_offset", 7, 24, 1.f, 3.f, sv, 1, 0.5f, dashes, 2);
}

static void test_canvas_save_restore(void)
{
    current_test = "canvas_save_restore";
    const int w = 16, h = 5;
    float arr[3] = { 3, 2, 1 };

    const pt_t pts[] = { {1, 2}, {13, 2} };
    const subpath_t subs[] = { { pts, 2 } };
    plutovg_path_t* path = make_path(subs, 1);

    /* Reference rendering with the original style. */
    bitmap_t ref = render_stroke(w, h, 1.f, 1.f, path, 1, -1.f, arr, 3);

    /* The caller rewrites the array after set_dash: the canvas must have
     * copied it, so the rendering and the getters stay unchanged. */
    plutovg_surface_t* surface = plutovg_surface_create(w, h);
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
    plutovg_canvas_set_rgba(canvas, 1, 0, 0, 1);
    plutovg_canvas_set_line_width(canvas, 1);
    plutovg_canvas_set_line_cap(canvas, PLUTOVG_LINE_CAP_BUTT);
    plutovg_canvas_set_dash(canvas, -1.f, arr, 3);
    arr[0] = arr[1] = arr[2] = 100.f;

    plutovg_canvas_stroke_path(canvas, path);
    bitmap_t got;
    got.stride = plutovg_surface_get_stride(surface);
    got.data = malloc((size_t)got.stride * h);
    if(!got.data) { fprintf(stderr, "out of memory\n"); exit(2); }
    memcpy(got.data, plutovg_surface_get_data(surface), (size_t)got.stride * h);
    compare_bitmaps("canvas after caller mutated dash array", &got, "reference", &ref, w, h);

    if(plutovg_canvas_get_dash_offset(canvas) != -1.f)
        fail("plutovg_canvas_get_dash_offset", "got %g, expected -1",
             (double)plutovg_canvas_get_dash_offset(canvas));
    const float* stored = NULL;
    int nstored = plutovg_canvas_get_dash_array(canvas, &stored);
    if(nstored != 3 || stored[0] != 3.f || stored[1] != 2.f || stored[2] != 1.f)
        fail("plutovg_canvas_get_dash_array", "got n=%d [%g,%g,%g], expected 3 [3,2,1]",
             nstored, nstored > 0 ? (double)stored[0] : -1.0,
             nstored > 1 ? (double)stored[1] : -1.0, nstored > 2 ? (double)stored[2] : -1.0);

    /* Inner save modifies offset and array; restore must bring back both
     * the values and the original rendering. */
    plutovg_canvas_save(canvas);
    float inner[2] = { 1, 1 };
    plutovg_canvas_set_dash_offset(canvas, 0.f);
    plutovg_canvas_set_dash_array(canvas, inner, 2);
    inner[0] = 50.f; /* must not leak into the stored inner state either */
    if(plutovg_canvas_get_dash_offset(canvas) != 0.f)
        fail("inner dash offset", "got %g, expected 0",
             (double)plutovg_canvas_get_dash_offset(canvas));
    plutovg_canvas_restore(canvas);

    if(plutovg_canvas_get_dash_offset(canvas) != -1.f)
        fail("plutovg_canvas_restore(offset)", "got %g, expected -1",
             (double)plutovg_canvas_get_dash_offset(canvas));
    stored = NULL;
    nstored = plutovg_canvas_get_dash_array(canvas, &stored);
    if(nstored != 3 || stored[0] != 3.f || stored[1] != 2.f || stored[2] != 1.f)
        fail("plutovg_canvas_restore(array)", "got n=%d, expected 3 [3,2,1]", nstored);

    memset(plutovg_surface_get_data(surface), 0, (size_t)got.stride * h);
    plutovg_canvas_stroke_path(canvas, path);
    memcpy(got.data, plutovg_surface_get_data(surface), (size_t)got.stride * h);
    compare_bitmaps("canvas render after restore", &got, "reference", &ref, w, h);

    free(got.data);
    plutovg_canvas_destroy(canvas);
    plutovg_surface_destroy(surface);
    bitmap_free(&ref);
    plutovg_path_destroy(path);
}

int main(void)
{
    test_odd_array_period();
    test_offset_period_equivalence();
    test_collinear_lineto_continuation();
    test_moveto_restarts_phase();
    test_exact_dash_boundary();
    test_pixels_baseline();
    test_pixels_odd_negative_second_subpath();
    test_pixels_nonuniform_scale();
    test_canvas_save_restore();

    if(failures) {
        printf("dash_contract: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS dash_contract: all dash contract tests passed\n");
    return 0;
}
