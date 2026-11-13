#include "registry.hpp"
#include "plutovg.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

const int kTolerance = 3;

struct RefPoint {
    double x;
    double y;
};

/* ------------------------------------------------------------------------- */
/* Independent reference implementation.                                     */
/*                                                                           */
/* This code evaluates tensor-product Bernstein patches directly and solves  */
/* for the parameter with its own analytic Newton iteration. It never calls  */
/* the mesh sampler exposed by PlutoVG.                                      */
/* ------------------------------------------------------------------------- */

double bern3(int i, double t)
{
    const double mt = 1.0 - t;
    switch(i) {
    case 0: return mt * mt * mt;
    case 1: return 3.0 * t * mt * mt;
    case 2: return 3.0 * t * t * mt;
    default: return t * t * t;
    }
}

double bern2(int i, double t)
{
    if(i < 0 || i > 2)
        return 0.0;
    const double mt = 1.0 - t;
    switch(i) {
    case 0: return mt * mt;
    case 1: return 2.0 * t * mt;
    default: return t * t;
    }
}

double bern3_derivative(int i, double t)
{
    return 3.0 * (bern2(i - 1, t) - bern2(i, t));
}

void ref_eval(const RefPoint points[16], double u, double v,
    RefPoint* position, RefPoint* derivative_u, RefPoint* derivative_v)
{
    RefPoint p = { 0.0, 0.0 };
    RefPoint du = { 0.0, 0.0 };
    RefPoint dv = { 0.0, 0.0 };
    for(int j = 0; j < 4; ++j) {
        for(int i = 0; i < 4; ++i) {
            const double bu = bern3(i, u);
            const double bv = bern3(j, v);
            const double dbu = bern3_derivative(i, u);
            const RefPoint& q = points[4 * j + i];
            p.x += q.x * bu * bv;
            p.y += q.y * bu * bv;
            du.x += q.x * dbu * bv;
            du.y += q.y * dbu * bv;
            dv.x += q.x * bu * bern3_derivative(j, v);
            dv.y += q.y * bu * bern3_derivative(j, v);
        }
    }

    *position = p;
    *derivative_u = du;
    *derivative_v = dv;
}

RefPoint ref_seed(const RefPoint points[16], double x, double y)
{
    RefPoint best = { 0.0, 0.0 };
    double best_distance = 1e300;
    for(int j = 0; j < 9; ++j) {
        for(int i = 0; i < 9; ++i) {
            double u = i / 8.0;
            double v = j / 8.0;
            RefPoint p, du, dv;
            ref_eval(points, u, v, &p, &du, &dv);
            double dx = p.x - x;
            double dy = p.y - y;
            double distance = dx * dx + dy * dy;
            if(distance < best_distance) {
                best_distance = distance;
                best.x = u;
                best.y = v;
            }
        }
    }

    return best;
}

bool ref_invert(const RefPoint points[16], double x, double y, double* u, double* v)
{
    RefPoint seed = ref_seed(points, x, y);
    double su = seed.x, sv = seed.y;
    bool converged = false;
    for(int iteration = 0; iteration < 60; ++iteration) {
        RefPoint p, du, dv;
        ref_eval(points, su, sv, &p, &du, &dv);
        double rx = p.x - x;
        double ry = p.y - y;
        double det = du.x * dv.y - du.y * dv.x;
        if(!std::isfinite(det) || std::fabs(det) < 1e-18)
            return false;
        double step_u = (rx * dv.y - ry * dv.x) / det;
        double step_v = (du.x * ry - du.y * rx) / det;
        su -= step_u;
        sv -= step_v;
        if(!std::isfinite(su) || !std::isfinite(sv) || su < -4.0 || su > 5.0 || sv < -4.0 || sv > 5.0)
            return false;
        if(std::fabs(rx) < 1e-9 && std::fabs(ry) < 1e-9 && std::fabs(step_u) < 1e-12 && std::fabs(step_v) < 1e-12) {
            converged = true;
            break;
        }
    }

    if(!converged) {
        RefPoint p, du, dv;
        ref_eval(points, su, sv, &p, &du, &dv);
        if(std::fabs(p.x - x) > 1e-6 || std::fabs(p.y - y) > 1e-6)
            return false;
    }

    if(su < -1e-6 || su > 1.0 + 1e-6 || sv < -1e-6 || sv > 1.0 + 1e-6)
        return false;
    *u = su;
    *v = sv;
    return true;
}

void ref_color(const plutovg_mesh_patch_t& patch, double u, double v, double color[4])
{
    const double w[4] = {
        (1.0 - u) * (1.0 - v),
        u * (1.0 - v),
        u * v,
        (1.0 - u) * v
    };

    double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
    for(int k = 0; k < 4; ++k) {
        const plutovg_color_t& c = patch.colors[k];
        r += c.r * c.a * w[k];
        g += c.g * c.a * w[k];
        b += c.b * c.a * w[k];
        a += c.a * w[k];
    }

    color[0] = r;
    color[1] = g;
    color[2] = b;
    color[3] = a;
}

RefPoint transform_point(const plutovg_matrix_t& matrix, double x, double y)
{
    RefPoint p;
    p.x = matrix.a * x + matrix.c * y + matrix.e;
    p.y = matrix.b * x + matrix.d * y + matrix.f;
    return p;
}

void transform_patch(const plutovg_mesh_patch_t& patch, const plutovg_matrix_t* paint, const plutovg_matrix_t* canvas, RefPoint points[16])
{
    for(int k = 0; k < 16; ++k) {
        RefPoint p = { patch.points[k].x, patch.points[k].y };
        if(paint)
            p = transform_point(*paint, p.x, p.y);
        if(canvas)
            p = transform_point(*canvas, p.x, p.y);
        points[k] = p;
    }
}

uint32_t ref_pixel(const plutovg_mesh_patch_t* patches, int count,
    const plutovg_matrix_t* paint, const plutovg_matrix_t* canvas,
    int px, int py, float opacity)
{
    RefPoint transformed[4][16];
    for(int i = 0; i < count; ++i)
        transform_patch(patches[i], paint, canvas, transformed[i]);

    double red = 0.0, green = 0.0, blue = 0.0, alpha = 0.0;
    for(int j = 0; j < 8; ++j) {
        for(int i = 0; i < 8; ++i) {
            double x = px + (i + 0.5) / 8.0;
            double y = py + (j + 0.5) / 8.0;
            for(int k = count - 1; k >= 0; --k) {
                double u = 0.0, v = 0.0;
                if(ref_invert(transformed[k], x, y, &u, &v)) {
                    if(u < 0.0) u = 0.0;
                    if(v < 0.0) v = 0.0;
                    if(u > 1.0) u = 1.0;
                    if(v > 1.0) v = 1.0;
                    double color[4];
                    ref_color(patches[k], u, v, color);
                    red += color[0];
                    green += color[1];
                    blue += color[2];
                    alpha += color[3];
                    break;
                }
            }
        }
    }

    red = red * opacity / 64.0;
    green = green * opacity / 64.0;
    blue = blue * opacity / 64.0;
    alpha = alpha * opacity / 64.0;

    uint32_t a = (uint32_t)lround(fmax(0.0, fmin(1.0, alpha)) * 255.0);
    uint32_t r = (uint32_t)lround(fmax(0.0, fmin(1.0, red)) * 255.0);
    uint32_t g = (uint32_t)lround(fmax(0.0, fmin(1.0, green)) * 255.0);
    uint32_t b = (uint32_t)lround(fmax(0.0, fmin(1.0, blue)) * 255.0);
    if(r > a) r = a;
    if(g > a) g = a;
    if(b > a) b = a;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

int channel(uint32_t color, int shift)
{
    return (int)((color >> shift) & 0xffu);
}

bool colors_match(uint32_t a, uint32_t b)
{
    return std::abs(channel(a, 24) - channel(b, 24)) <= kTolerance
        && std::abs(channel(a, 16) - channel(b, 16)) <= kTolerance
        && std::abs(channel(a, 8) - channel(b, 8)) <= kTolerance
        && std::abs(channel(a, 0) - channel(b, 0)) <= kTolerance;
}

uint32_t pixel_at(plutovg_surface_t* surface, int x, int y)
{
    const unsigned char* data = plutovg_surface_get_data(surface);
    return ((const uint32_t*)(data + y * plutovg_surface_get_stride(surface)))[x];
}

void clear_surface(plutovg_surface_t* surface)
{
    memset(plutovg_surface_get_data(surface), 0, plutovg_surface_get_stride(surface) * plutovg_surface_get_height(surface));
}

void fail(Case& c, const char* what, int x, int y, uint32_t got, uint32_t expected)
{
    char message[256];
    std::snprintf(message, sizeof(message), "%s at (%d,%d): got 0x%08x expected 0x%08x", what, x, y, got, expected);
    c.check(false, message);
}

/* Checks a rectangle of pixels against the independent reference. */
void check_region(Case& c, plutovg_surface_t* surface, const plutovg_mesh_patch_t* patches, int count,
    const plutovg_matrix_t* paint, const plutovg_matrix_t* canvas,
    int x0, int y0, int x1, int y1, const char* what)
{
    c.check(plutovg_surface_get_width(surface) >= x1 && plutovg_surface_get_height(surface) >= y1, "region inside surface");
    for(int y = y0; y < y1; ++y) {
        for(int x = x0; x < x1; ++x) {
            uint32_t got = pixel_at(surface, x, y);
            uint32_t expected = ref_pixel(patches, count, paint, canvas, x, y, 1.0f);
            if(!colors_match(got, expected))
                fail(c, what, x, y, got, expected);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Patch constructors used by the tests.                                      */
/* ------------------------------------------------------------------------- */

plutovg_color_t color4(float r, float g, float b, float a)
{
    return PLUTOVG_MAKE_COLOR(r, g, b, a);
}

plutovg_mesh_patch_t flat_patch(float x0, float y0, float x1, float y1,
    plutovg_color_t c00, plutovg_color_t c10, plutovg_color_t c11, plutovg_color_t c01)
{
    plutovg_mesh_patch_t patch;
    std::memset(&patch, 0, sizeof(patch));
    for(int j = 0; j < 4; ++j) {
        for(int i = 0; i < 4; ++i) {
            patch.points[4 * j + i].x = x0 + (x1 - x0) * (i / 3.0f);
            patch.points[4 * j + i].y = y0 + (y1 - y0) * (j / 3.0f);
        }
    }

    patch.colors[0] = c00;
    patch.colors[1] = c10;
    patch.colors[2] = c11;
    patch.colors[3] = c01;
    return patch;
}

plutovg_mesh_patch_t curved_patch(plutovg_color_t c00, plutovg_color_t c10, plutovg_color_t c11, plutovg_color_t c01)
{
    const float sx[4] = { 20.f, 45.f, 75.f, 100.f };
    const float sy[4] = { 20.f, 45.f, 75.f, 100.f };
    plutovg_mesh_patch_t patch;
    std::memset(&patch, 0, sizeof(patch));
    for(int j = 0; j < 4; ++j) {
        for(int i = 0; i < 4; ++i) {
            float x = sx[i];
            float y = sy[j];
            if(i > 0 && i < 3 && j > 0 && j < 3) {
                x += (i == 1 ? -1.f : 1.f) * 14.f;
                y += (j == 1 ? -1.f : 1.f) * 14.f;
            }

            patch.points[4 * j + i].x = x;
            patch.points[4 * j + i].y = y;
        }
    }

    patch.colors[0] = c00;
    patch.colors[1] = c10;
    patch.colors[2] = c11;
    patch.colors[3] = c01;
    return patch;
}

/* Mirrors the parameterization along u, producing a patch with a reversed */
/* (negative) orientation while keeping the same surface.                   */
plutovg_mesh_patch_t reflect_patch_u(const plutovg_mesh_patch_t& source)
{
    plutovg_mesh_patch_t patch = source;
    for(int j = 0; j < 4; ++j) {
        for(int i = 0; i < 4; ++i)
            patch.points[4 * j + i] = source.points[4 * j + (3 - i)];
    }

    patch.colors[0] = source.colors[1];
    patch.colors[1] = source.colors[0];
    patch.colors[2] = source.colors[3];
    patch.colors[3] = source.colors[2];
    return patch;
}

/* Splits a patch into two in parameter direction 0 (u) or 1 (v). */
void split_curve(const plutovg_point_t c[4], double t, plutovg_point_t left[4], plutovg_point_t right[4])
{
    plutovg_point_t p01 = PLUTOVG_MAKE_POINT((float)(c[0].x + (c[1].x - c[0].x) * t), (float)(c[0].y + (c[1].y - c[0].y) * t));
    plutovg_point_t p12 = PLUTOVG_MAKE_POINT((float)(c[1].x + (c[2].x - c[1].x) * t), (float)(c[1].y + (c[2].y - c[1].y) * t));
    plutovg_point_t p23 = PLUTOVG_MAKE_POINT((float)(c[2].x + (c[3].x - c[2].x) * t), (float)(c[2].y + (c[3].y - c[2].y) * t));
    plutovg_point_t p012 = PLUTOVG_MAKE_POINT((float)(p01.x + (p12.x - p01.x) * t), (float)(p01.y + (p12.y - p01.y) * t));
    plutovg_point_t p123 = PLUTOVG_MAKE_POINT((float)(p12.x + (p23.x - p12.x) * t), (float)(p12.y + (p23.y - p12.y) * t));
    plutovg_point_t p0123 = PLUTOVG_MAKE_POINT((float)(p012.x + (p123.x - p012.x) * t), (float)(p012.y + (p123.y - p012.y) * t));
    left[0] = c[0];
    left[1] = p01;
    left[2] = p012;
    left[3] = p0123;
    right[0] = p0123;
    right[1] = p123;
    right[2] = p23;
    right[3] = c[3];
}

void split_patch(const plutovg_mesh_patch_t& patch, int direction, double t,
    plutovg_mesh_patch_t* first, plutovg_mesh_patch_t* second)
{
    *first = patch;
    *second = patch;
    if(direction == 0) {
        for(int j = 0; j < 4; ++j) {
            plutovg_point_t c[4] = { patch.points[4 * j + 0], patch.points[4 * j + 1], patch.points[4 * j + 2], patch.points[4 * j + 3] };
            plutovg_point_t left[4], right[4];
            split_curve(c, t, left, right);
            for(int i = 0; i < 4; ++i) {
                first->points[4 * j + i] = left[i];
                second->points[4 * j + i] = right[i];
            }
        }
    } else {
        for(int i = 0; i < 4; ++i) {
            plutovg_point_t c[4] = { patch.points[4 * 0 + i], patch.points[4 * 1 + i], patch.points[4 * 2 + i], patch.points[4 * 3 + i] };
            plutovg_point_t left[4], right[4];
            split_curve(c, t, left, right);
            for(int j = 0; j < 4; ++j) {
                first->points[4 * j + i] = left[j];
                second->points[4 * j + i] = right[j];
            }
        }
    }
}

/* Bilinear RGB interpolation at a parameter location for an opaque patch. */
plutovg_color_t bilinear_color(const plutovg_mesh_patch_t& patch, double u, double v)
{
    const double w[4] = {
        (1.0 - u) * (1.0 - v),
        u * (1.0 - v),
        u * v,
        (1.0 - u) * v
    };

    double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
    for(int k = 0; k < 4; ++k) {
        r += patch.colors[k].r * w[k];
        g += patch.colors[k].g * w[k];
        b += patch.colors[k].b * w[k];
        a += patch.colors[k].a * w[k];
    }

    return PLUTOVG_MAKE_COLOR((float)r, (float)g, (float)b, (float)a);
}

/* ------------------------------------------------------------------------- */
/* Tests.                                                                    */
/* ------------------------------------------------------------------------- */

void register_geometry(Registry& registry)
{
    registry.add("geometry", "curved-interior", [](Case& c) {
        plutovg_mesh_patch_t patch = curved_patch(
            color4(1, 0, 0, 1), color4(0, 1, 0, 1), color4(0, 0, 1, 1), color4(1, 1, 0, 1));
        const int width = 128, height = 128;
        plutovg_surface_t* surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
        plutovg_paint_t* paint = plutovg_paint_create_mesh(&patch, 1, NULL);
        c.check(paint != NULL, "mesh paint created");
        plutovg_canvas_set_paint(canvas, paint);
        plutovg_canvas_paint(canvas);
        check_region(c, surface, &patch, 1, NULL, NULL, 24, 24, 100, 100, "curved interior");
        c.check(pixel_at(surface, 2, 2) == 0, "transparent outside the patch");
        plutovg_paint_destroy(paint);
        plutovg_canvas_destroy(canvas);
        plutovg_surface_destroy(surface);
    });

    registry.add("geometry", "reflected-patch", [](Case& c) {
        plutovg_mesh_patch_t base = curved_patch(
            color4(0.9f, 0.1f, 0.2f, 1), color4(0.1f, 0.8f, 0.3f, 1), color4(0.2f, 0.3f, 0.9f, 1), color4(0.8f, 0.7f, 0.1f, 1));
        plutovg_mesh_patch_t patch = reflect_patch_u(base);
        const int width = 128, height = 128;
        plutovg_surface_t* surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
        plutovg_paint_t* paint = plutovg_paint_create_mesh(&patch, 1, NULL);
        c.check(paint != NULL, "reflected paint created");
        plutovg_canvas_set_paint(canvas, paint);
        plutovg_canvas_paint(canvas);
        check_region(c, surface, &patch, 1, NULL, NULL, 24, 24, 100, 100, "reflected interior");
        plutovg_paint_destroy(paint);
        plutovg_canvas_destroy(canvas);
        plutovg_surface_destroy(surface);
    });

    registry.add("geometry", "matrix-composition", [](Case& c) {
        plutovg_mesh_patch_t patch = curved_patch(
            color4(1, 0, 0, 1), color4(0, 1, 0, 1), color4(0, 0, 1, 1), color4(1, 1, 0, 1));
        /* Non-commutative pair: paint translates, canvas scales. */
        plutovg_matrix_t paint_matrix;
        plutovg_matrix_init_translate(&paint_matrix, 13, 7);
        plutovg_matrix_t canvas_matrix;
        plutovg_matrix_init_scale(&canvas_matrix, 1.5f, 0.75f);

        const int width = 200, height = 160;
        plutovg_surface_t* surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
        plutovg_canvas_set_matrix(canvas, &canvas_matrix);
        plutovg_paint_t* paint = plutovg_paint_create_mesh(&patch, 1, &paint_matrix);
        c.check(paint != NULL, "matrix paint created");
        plutovg_canvas_set_paint(canvas, paint);
        plutovg_canvas_paint(canvas);
        check_region(c, surface, &patch, 1, &paint_matrix, &canvas_matrix, 60, 40, 150, 110, "paint then canvas");

        /* The opposite composition order must be visibly different. */
        bool different = false;
        for(int y = 30; y < 120 && !different; ++y) {
            for(int x = 40; x < 150 && !different; ++x) {
                uint32_t actual = pixel_at(surface, x, y);
                uint32_t reversed = ref_pixel(&patch, 1, &canvas_matrix, &paint_matrix, x, y, 1.0f);
                different = !colors_match(actual, reversed);
            }
        }
        c.check(different, "matrix order affects the result");

        plutovg_paint_destroy(paint);
        plutovg_canvas_destroy(canvas);
        plutovg_surface_destroy(surface);
    });

    registry.add("geometry", "invalid-inputs", [](Case& c) {
        plutovg_mesh_patch_t patch = flat_patch(0, 0, 10, 10, color4(1, 0, 0, 1), color4(1, 0, 0, 1), color4(1, 0, 0, 1), color4(1, 0, 0, 1));
        c.check(plutovg_paint_create_mesh(NULL, 1, NULL) == NULL, "null patch array rejected");
        c.check(plutovg_paint_create_mesh(&patch, 0, NULL) == NULL, "zero count rejected");
        c.check(plutovg_paint_create_mesh(&patch, -3, NULL) == NULL, "negative count rejected");

        plutovg_mesh_patch_t bad = patch;
        bad.points[5].x = NAN;
        c.check(plutovg_paint_create_mesh(&bad, 1, NULL) == NULL, "non-finite point rejected");

        bad = patch;
        bad.colors[2].a = INFINITY;
        c.check(plutovg_paint_create_mesh(&bad, 1, NULL) == NULL, "non-finite color rejected");

        plutovg_matrix_t singular;
        plutovg_matrix_init(&singular, 1, 2, 2, 4, 0, 0);
        c.check(plutovg_paint_create_mesh(&patch, 1, &singular) == NULL, "singular matrix rejected");

        plutovg_matrix_t nonfinite;
        plutovg_matrix_init(&nonfinite, 1, 0, 0, 1, NAN, 0);
        c.check(plutovg_paint_create_mesh(&patch, 1, &nonfinite) == NULL, "non-finite matrix rejected");

        /* Valid input still succeeds. */
        plutovg_matrix_t valid;
        plutovg_matrix_init_scale(&valid, 0.5f, 0.5f);
        plutovg_paint_t* paint = plutovg_paint_create_mesh(&patch, 1, &valid);
        c.check(paint != NULL, "valid input accepted");
        c.check(plutovg_paint_get_reference_count(paint) == 1, "initial reference count");
        plutovg_paint_destroy(paint);
    });
}

void register_seams(Registry& registry)
{
    registry.add("seams", "shared-curved-edge", [](Case& c) {
        /* Two patches whose shared edge is the same cubic curve; the colors */
        /* along the edge agree. Both are translucent red.                  */
        plutovg_mesh_patch_t patches[2];
        std::memset(patches, 0, sizeof(patches));
        const float edge_x[4] = { 70, 95, 45, 70 };
        const float edge_y[4] = { 15, 45, 80, 115 };
        for(int j = 0; j < 4; ++j) {
            patches[0].points[4 * j + 0] = PLUTOVG_MAKE_POINT(edge_x[j] - 60, edge_y[j]);
            patches[0].points[4 * j + 1] = PLUTOVG_MAKE_POINT(edge_x[j] - 40, edge_y[j]);
            patches[0].points[4 * j + 2] = PLUTOVG_MAKE_POINT(edge_x[j] - 20, edge_y[j]);
            patches[0].points[4 * j + 3] = PLUTOVG_MAKE_POINT(edge_x[j], edge_y[j]);

            patches[1].points[4 * j + 0] = PLUTOVG_MAKE_POINT(edge_x[j], edge_y[j]);
            patches[1].points[4 * j + 1] = PLUTOVG_MAKE_POINT(edge_x[j] + 20, edge_y[j]);
            patches[1].points[4 * j + 2] = PLUTOVG_MAKE_POINT(edge_x[j] + 40, edge_y[j]);
            patches[1].points[4 * j + 3] = PLUTOVG_MAKE_POINT(edge_x[j] + 60, edge_y[j]);
        }

        plutovg_color_t translucent = color4(1, 0, 0, 0.5f);
        for(int k = 0; k < 2; ++k)
            patches[k].colors[0] = patches[k].colors[1] = patches[k].colors[2] = patches[k].colors[3] = translucent;

        const int width = 180, height = 140;
        plutovg_surface_t* surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
        plutovg_paint_t* paint = plutovg_paint_create_mesh(patches, 2, NULL);
        c.check(paint != NULL, "seam paint created");
        plutovg_canvas_set_paint(canvas, paint);
        plutovg_canvas_paint(canvas);

        /* Walk the shared curve and inspect the pixel containing each point. */
        RefPoint control[16];
        transform_patch(patches[0], NULL, NULL, control);
        int checked = 0;
        for(int step = 2; step <= 98; ++step) {
            double v = step / 100.0;
            RefPoint point = { 0.0, 0.0 }, temp;
            ref_eval(control, 1.0, v, &point, &temp, &temp);
            int px = (int)std::floor(point.x);
            int py = (int)std::floor(point.y);
            if(px < 1 || px >= width - 1 || py < 1 || py >= height - 1)
                continue;
            uint32_t value = pixel_at(surface, px, py);
            int a = channel(value, 24);
            int r = channel(value, 16);
            int g = channel(value, 8);
            int b = channel(value, 0);
            c.check(a >= 125 && a <= 131, "shared edge has no transparent crack");
            c.check(r >= 125 && r <= 131, "shared edge is not darkened");
            c.check(g <= 3 && b <= 3, "shared edge keeps the expected hue");
            checked++;
        }
        c.check(checked > 20, "shared edge sampled");
        plutovg_paint_destroy(paint);
        plutovg_canvas_destroy(canvas);
        plutovg_surface_destroy(surface);
    });

    registry.add("seams", "patch-order", [](Case& c) {
        plutovg_mesh_patch_t patches[2];
        patches[0] = flat_patch(10, 10, 70, 70, color4(1, 0, 0, 0.5f), color4(1, 0, 0, 0.5f), color4(1, 0, 0, 0.5f), color4(1, 0, 0, 0.5f));
        patches[1] = flat_patch(30, 30, 50, 50, color4(0, 0, 1, 0.5f), color4(0, 0, 1, 0.5f), color4(0, 0, 1, 0.5f), color4(0, 0, 1, 0.5f));

        const int width = 90, height = 90;
        plutovg_surface_t* forward = plutovg_surface_create(width, height);
        plutovg_canvas_t* fcanvas = plutovg_canvas_create(forward);
        plutovg_paint_t* fpaint = plutovg_paint_create_mesh(patches, 2, NULL);
        c.check(fpaint != NULL, "ordered paint created");
        plutovg_canvas_set_paint(fcanvas, fpaint);
        plutovg_canvas_paint(fcanvas);

        /* Only the last patch contributes in the overlap, with a single */
        /* SRC_OVER operation: premultiplied blue at half alpha.           */
        uint32_t inside = pixel_at(forward, 40, 40);
        c.check(channel(inside, 24) == 128, "last patch alpha in overlap");
        c.check(channel(inside, 0) >= 125 && channel(inside, 0) <= 131, "last patch blue in overlap");
        c.check(channel(inside, 16) <= 3, "no red survives in overlap");
        c.check(channel(pixel_at(forward, 20, 20), 16) >= 125, "first patch visible outside overlap");

        plutovg_mesh_patch_t reversed[2] = { patches[1], patches[0] };
        plutovg_surface_t* backward = plutovg_surface_create(width, height);
        plutovg_canvas_t* bcanvas = plutovg_canvas_create(backward);
        plutovg_paint_t* bpaint = plutovg_paint_create_mesh(reversed, 2, NULL);
        c.check(bpaint != NULL, "reversed paint created");
        plutovg_canvas_set_paint(bcanvas, bpaint);
        plutovg_canvas_paint(bcanvas);
        uint32_t reverse_inside = pixel_at(backward, 40, 40);
        c.check(channel(reverse_inside, 16) >= 125, "reversing the array reverses the winner");
        c.check(channel(reverse_inside, 0) <= 3, "reversed winner has no blue");

        plutovg_paint_destroy(bpaint);
        plutovg_paint_destroy(fpaint);
        plutovg_canvas_destroy(bcanvas);
        plutovg_canvas_destroy(fcanvas);
        plutovg_surface_destroy(backward);
        plutovg_surface_destroy(forward);
    });

    registry.add("seams", "exact-subdivision", [](Case& c) {
        plutovg_mesh_patch_t original = curved_patch(
            color4(1, 0, 0, 1), color4(0, 1, 0, 1), color4(0, 0, 1, 1), color4(1, 1, 0, 1));

        plutovg_mesh_patch_t halves[4];
        plutovg_mesh_patch_t discarded[2];
        split_patch(original, 0, 0.5, &discarded[0], &discarded[1]);
        split_patch(discarded[0], 1, 0.5, &halves[0], &halves[1]);
        split_patch(discarded[1], 1, 0.5, &halves[2], &halves[3]);

        halves[0].colors[0] = bilinear_color(original, 0.0, 0.0);
        halves[0].colors[1] = bilinear_color(original, 0.5, 0.0);
        halves[0].colors[2] = bilinear_color(original, 0.5, 0.5);
        halves[0].colors[3] = bilinear_color(original, 0.0, 0.5);

        halves[1].colors[0] = bilinear_color(original, 0.0, 0.5);
        halves[1].colors[1] = bilinear_color(original, 0.5, 0.5);
        halves[1].colors[2] = bilinear_color(original, 0.5, 1.0);
        halves[1].colors[3] = bilinear_color(original, 0.0, 1.0);

        halves[2].colors[0] = bilinear_color(original, 0.5, 0.0);
        halves[2].colors[1] = bilinear_color(original, 1.0, 0.0);
        halves[2].colors[2] = bilinear_color(original, 1.0, 0.5);
        halves[2].colors[3] = bilinear_color(original, 0.5, 0.5);

        halves[3].colors[0] = bilinear_color(original, 0.5, 0.5);
        halves[3].colors[1] = bilinear_color(original, 1.0, 0.5);
        halves[3].colors[2] = bilinear_color(original, 1.0, 1.0);
        halves[3].colors[3] = bilinear_color(original, 0.5, 1.0);

        const int width = 128, height = 128;
        plutovg_surface_t* whole_surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* whole_canvas = plutovg_canvas_create(whole_surface);
        plutovg_paint_t* whole_paint = plutovg_paint_create_mesh(&original, 1, NULL);
        c.check(whole_paint != NULL, "original paint");
        plutovg_canvas_set_paint(whole_canvas, whole_paint);
        plutovg_canvas_paint(whole_canvas);

        plutovg_surface_t* parts_surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* parts_canvas = plutovg_canvas_create(parts_surface);
        plutovg_paint_t* parts_paint = plutovg_paint_create_mesh(halves, 4, NULL);
        c.check(parts_paint != NULL, "subdivided paint");
        plutovg_canvas_set_paint(parts_canvas, parts_paint);
        plutovg_canvas_paint(parts_canvas);

        for(int y = 20; y < 104; ++y) {
            for(int x = 20; x < 104; ++x) {
                uint32_t a = pixel_at(whole_surface, x, y);
                uint32_t b = pixel_at(parts_surface, x, y);
                if(!colors_match(a, b))
                    fail(c, "subdivision invariance", x, y, b, a);
            }
        }

        plutovg_paint_destroy(parts_paint);
        plutovg_paint_destroy(whole_paint);
        plutovg_canvas_destroy(parts_canvas);
        plutovg_canvas_destroy(whole_canvas);
        plutovg_surface_destroy(parts_surface);
        plutovg_surface_destroy(whole_surface);
    });
}

void register_compositing(Registry& registry)
{
    registry.add("compositing", "translucent-over-background", [](Case& c) {
        plutovg_mesh_patch_t patch = flat_patch(20, 20, 80, 80,
            color4(1, 0.25f, 0, 0.5f), color4(1, 0.25f, 0, 0.5f),
            color4(1, 0.25f, 0, 0.5f), color4(1, 0.25f, 0, 0.5f));

        const int width = 100, height = 100;
        plutovg_surface_t* mesh_surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* mesh_canvas = plutovg_canvas_create(mesh_surface);
        plutovg_canvas_set_rgba(mesh_canvas, 0, 0, 1, 1);
        plutovg_canvas_fill_rect(mesh_canvas, 0, 0, width, height);
        plutovg_paint_t* paint = plutovg_paint_create_mesh(&patch, 1, NULL);
        c.check(paint != NULL, "translucent mesh paint");
        plutovg_canvas_set_paint(mesh_canvas, paint);
        plutovg_canvas_fill_rect(mesh_canvas, 20, 20, 60, 60);

        plutovg_surface_t* solid_surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* solid_canvas = plutovg_canvas_create(solid_surface);
        plutovg_canvas_set_rgba(solid_canvas, 0, 0, 1, 1);
        plutovg_canvas_fill_rect(solid_canvas, 0, 0, width, height);
        plutovg_canvas_set_rgba(solid_canvas, 1, 0.25f, 0, 0.5f);
        plutovg_canvas_fill_rect(solid_canvas, 20, 20, 60, 60);

        for(int y = 25; y < 75; y += 5) {
            for(int x = 25; x < 75; x += 5) {
                uint32_t got = pixel_at(mesh_surface, x, y);
                uint32_t expected = pixel_at(solid_surface, x, y);
                if(!colors_match(got, expected))
                    fail(c, "translucent compositing", x, y, got, expected);
            }
        }
        c.check(pixel_at(mesh_surface, 10, 10) == pixel_at(solid_surface, 10, 10), "background outside the mesh");

        plutovg_paint_destroy(paint);
        plutovg_canvas_destroy(solid_canvas);
        plutovg_canvas_destroy(mesh_canvas);
        plutovg_surface_destroy(solid_surface);
        plutovg_surface_destroy(mesh_surface);
    });

    registry.add("compositing", "clip", [](Case& c) {
        plutovg_mesh_patch_t patch = flat_patch(10, 10, 90, 90, color4(0, 1, 0, 1), color4(0, 1, 0, 1), color4(0, 1, 0, 1), color4(0, 1, 0, 1));
        const int width = 100, height = 100;
        plutovg_surface_t* surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
        plutovg_canvas_clip_rect(canvas, 30, 30, 40, 40);
        plutovg_paint_t* paint = plutovg_paint_create_mesh(&patch, 1, NULL);
        c.check(paint != NULL, "clipped paint");
        plutovg_canvas_set_paint(canvas, paint);
        plutovg_canvas_fill_rect(canvas, 0, 0, width, height);

        c.check(pixel_at(surface, 50, 50) == 0xff00ff00u, "paint inside the clip");
        c.check(pixel_at(surface, 20, 20) == 0, "paint clipped away on the left");
        c.check(pixel_at(surface, 80, 20) == 0, "paint clipped away on the right");
        c.check(pixel_at(surface, 50, 75) == 0, "paint clipped away below");

        plutovg_paint_destroy(paint);
        plutovg_canvas_destroy(canvas);
        plutovg_surface_destroy(surface);
    });

    registry.add("compositing", "stroke", [](Case& c) {
        plutovg_mesh_patch_t patch = flat_patch(0, 0, 100, 100, color4(1, 1, 0, 1), color4(1, 1, 0, 1), color4(1, 1, 0, 1), color4(1, 1, 0, 1));
        const int width = 100, height = 100;
        plutovg_surface_t* surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
        plutovg_paint_t* paint = plutovg_paint_create_mesh(&patch, 1, NULL);
        c.check(paint != NULL, "stroked paint");
        plutovg_canvas_set_paint(canvas, paint);
        plutovg_canvas_set_line_width(canvas, 6);
        plutovg_canvas_stroke_rect(canvas, 20, 20, 60, 60);

        uint32_t on_stroke = pixel_at(surface, 20, 50);
        uint32_t inside = pixel_at(surface, 40, 50);
        c.check(channel(on_stroke, 16) >= 250 && channel(on_stroke, 8) >= 250 && channel(on_stroke, 24) >= 250, "mesh painted on the stroke");
        c.check(inside == 0, "path interior is not filled by a stroke");

        plutovg_paint_destroy(paint);
        plutovg_canvas_destroy(canvas);
        plutovg_surface_destroy(surface);
    });

    registry.add("compositing", "operator", [](Case& c) {
        plutovg_mesh_patch_t patch = flat_patch(30, 30, 70, 70, color4(0, 0, 1, 1), color4(0, 0, 1, 1), color4(0, 0, 1, 1), color4(0, 0, 1, 1));
        const int width = 100, height = 100;

        /* SRC ignores the destination: the result equals the paint alone. */
        plutovg_surface_t* op_surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* op_canvas = plutovg_canvas_create(op_surface);
        plutovg_canvas_set_rgba(op_canvas, 1, 0, 0, 1);
        plutovg_canvas_fill_rect(op_canvas, 0, 0, width, height);
        plutovg_paint_t* paint = plutovg_paint_create_mesh(&patch, 1, NULL);
        c.check(paint != NULL, "operator paint");
        plutovg_canvas_set_paint(op_canvas, paint);
        plutovg_canvas_set_operator(op_canvas, PLUTOVG_OPERATOR_SRC);
        plutovg_canvas_fill_rect(op_canvas, 0, 0, width, height);

        plutovg_surface_t* plain_surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* plain_canvas = plutovg_canvas_create(plain_surface);
        plutovg_paint_t* plain_paint = plutovg_paint_create_mesh(&patch, 1, NULL);
        c.check(plain_paint != NULL, "plain paint");
        plutovg_canvas_set_paint(plain_canvas, plain_paint);
        plutovg_canvas_fill_rect(plain_canvas, 0, 0, width, height);

        c.check(pixel_at(op_surface, 50, 50) == pixel_at(plain_surface, 50, 50), "SRC keeps the source");
        c.check(pixel_at(op_surface, 10, 10) == 0, "SRC clears the destination outside the mesh");
        c.check(pixel_at(plain_surface, 10, 10) == 0, "plain source is transparent outside");

        plutovg_paint_destroy(plain_paint);
        plutovg_paint_destroy(paint);
        plutovg_canvas_destroy(plain_canvas);
        plutovg_canvas_destroy(op_canvas);
        plutovg_surface_destroy(plain_surface);
        plutovg_surface_destroy(op_surface);
    });

    registry.add("compositing", "canvas-opacity", [](Case& c) {
        plutovg_mesh_patch_t patch = flat_patch(0, 0, 100, 100, color4(0, 1, 0, 1), color4(0, 1, 0, 1), color4(0, 1, 0, 1), color4(0, 1, 0, 1));
        const int width = 100, height = 100;

        plutovg_surface_t* mesh_surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* mesh_canvas = plutovg_canvas_create(mesh_surface);
        plutovg_canvas_set_rgba(mesh_canvas, 0, 0, 1, 1);
        plutovg_canvas_fill_rect(mesh_canvas, 0, 0, width, height);
        plutovg_canvas_set_opacity(mesh_canvas, 0.5f);
        plutovg_paint_t* paint = plutovg_paint_create_mesh(&patch, 1, NULL);
        c.check(paint != NULL, "opacity paint");
        plutovg_canvas_set_paint(mesh_canvas, paint);
        plutovg_canvas_paint(mesh_canvas);

        plutovg_surface_t* solid_surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* solid_canvas = plutovg_canvas_create(solid_surface);
        plutovg_canvas_set_rgba(solid_canvas, 0, 0, 1, 1);
        plutovg_canvas_fill_rect(solid_canvas, 0, 0, width, height);
        plutovg_canvas_set_opacity(solid_canvas, 0.5f);
        plutovg_canvas_set_rgba(solid_canvas, 0, 1, 0, 1);
        plutovg_canvas_fill_rect(solid_canvas, 0, 0, width, height);

        for(int y = 10; y < 90; y += 20) {
            for(int x = 10; x < 90; x += 20) {
                uint32_t got = pixel_at(mesh_surface, x, y);
                uint32_t expected = pixel_at(solid_surface, x, y);
                if(!colors_match(got, expected))
                    fail(c, "canvas opacity", x, y, got, expected);
            }
        }

        plutovg_paint_destroy(paint);
        plutovg_canvas_destroy(solid_canvas);
        plutovg_canvas_destroy(mesh_canvas);
        plutovg_surface_destroy(solid_surface);
        plutovg_surface_destroy(mesh_surface);
    });
}

void register_ownership(Registry& registry)
{
    registry.add("ownership", "input-copy", [](Case& c) {
        plutovg_mesh_patch_t* patches = (plutovg_mesh_patch_t*)std::malloc(sizeof(plutovg_mesh_patch_t));
        c.check(patches != NULL, "test allocation");
        *patches = curved_patch(color4(1, 0, 0, 1), color4(0, 1, 0, 1), color4(0, 0, 1, 1), color4(1, 1, 0, 1));
        plutovg_paint_t* paint = plutovg_paint_create_mesh(patches, 1, NULL);
        c.check(paint != NULL, "copied paint created");

        const int width = 128, height = 128;
        plutovg_surface_t* first = plutovg_surface_create(width, height);
        plutovg_canvas_t* first_canvas = plutovg_canvas_create(first);
        plutovg_canvas_set_paint(first_canvas, paint);
        plutovg_canvas_paint(first_canvas);

        /* Mutate and free the caller-owned input. */
        patches->points[0].x = -500;
        patches->colors[0] = color4(0, 0, 0, 0);
        std::free(patches);

        plutovg_surface_t* second = plutovg_surface_create(width, height);
        plutovg_canvas_t* second_canvas = plutovg_canvas_create(second);
        plutovg_canvas_set_paint(second_canvas, paint);
        plutovg_canvas_paint(second_canvas);

        for(int y = 20; y < 104; y += 7) {
            for(int x = 20; x < 104; x += 7) {
                uint32_t a = pixel_at(first, x, y);
                uint32_t b = pixel_at(second, x, y);
                if(a != b)
                    fail(c, "input copy isolation", x, y, b, a);
            }
        }
        c.check(true, "paint independent of caller input");

        plutovg_paint_destroy(paint);
        plutovg_canvas_destroy(second_canvas);
        plutovg_canvas_destroy(first_canvas);
        plutovg_surface_destroy(second);
        plutovg_surface_destroy(first);
    });

    registry.add("ownership", "nested-save-restore", [](Case& c) {
        plutovg_mesh_patch_t patch = curved_patch(color4(1, 0, 0, 1), color4(0, 1, 0, 1), color4(0, 0, 1, 1), color4(1, 1, 0, 1));
        const int width = 128, height = 128;
        plutovg_surface_t* plain_surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* plain_canvas = plutovg_canvas_create(plain_surface);
        plutovg_paint_t* plain_paint = plutovg_paint_create_mesh(&patch, 1, NULL);
        c.check(plain_paint != NULL, "plain paint");
        plutovg_canvas_set_paint(plain_canvas, plain_paint);
        plutovg_canvas_paint(plain_canvas);

        plutovg_surface_t* surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
        plutovg_paint_t* paint = plutovg_paint_create_mesh(&patch, 1, NULL);
        c.check(paint != NULL, "nested paint");
        c.check(plutovg_paint_get_reference_count(paint) == 1, "reference count after create");
        plutovg_canvas_set_paint(canvas, paint);
        c.check(plutovg_paint_get_reference_count(paint) == 2, "reference count after set_paint");
        plutovg_paint_destroy(paint);
        c.check(plutovg_paint_get_reference_count(plain_paint) == 2, "canvas holds paint after caller release");

        plutovg_canvas_save(canvas);
        /* Paint green only in a corner so the mesh comparison stays valid. */
        plutovg_canvas_clip_rect(canvas, 0, 0, 6, 6);
        plutovg_canvas_set_rgba(canvas, 0, 1, 0, 1);
        plutovg_canvas_paint(canvas);
        plutovg_canvas_save(canvas);
        plutovg_canvas_set_rgba(canvas, 0, 0, 0, 1);
        plutovg_canvas_restore(canvas);
        c.check(plutovg_canvas_get_paint(canvas, NULL) == NULL, "inner restore returns to the solid color");
        plutovg_canvas_restore(canvas);

        plutovg_paint_t* restored = plutovg_canvas_get_paint(canvas, NULL);
        c.check(restored != NULL, "outer restore returns the mesh paint");
        c.check(plutovg_paint_get_reference_count(restored) >= 1, "restored paint is referenced");
        plutovg_canvas_paint(canvas);

        for(int y = 20; y < 104; y += 9) {
            for(int x = 20; x < 104; x += 9) {
                uint32_t a = pixel_at(plain_surface, x, y);
                uint32_t b = pixel_at(surface, x, y);
                if(!colors_match(a, b))
                    fail(c, "save/restore paint", x, y, b, a);
            }
        }

        plutovg_canvas_destroy(canvas);
        plutovg_canvas_destroy(plain_canvas);
        plutovg_paint_destroy(plain_paint);
        plutovg_surface_destroy(surface);
        plutovg_surface_destroy(plain_surface);
    });

    registry.add("ownership", "reuse-across-transforms", [](Case& c) {
        plutovg_mesh_patch_t patch = curved_patch(color4(1, 0, 0, 1), color4(0, 1, 0, 1), color4(0, 0, 1, 1), color4(1, 1, 0, 1));
        const int width = 200, height = 200;
        plutovg_surface_t* surface = plutovg_surface_create(width, height);
        plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
        plutovg_paint_t* paint = plutovg_paint_create_mesh(&patch, 1, NULL);
        c.check(paint != NULL, "reused paint");
        c.check(plutovg_paint_get_reference_count(paint) == 1, "reference count before sharing");

        plutovg_canvas_set_paint(canvas, paint);
        c.check(plutovg_paint_get_reference_count(paint) == 2, "reference count after set_paint");

        plutovg_matrix_t identity;
        plutovg_matrix_init_identity(&identity);
        plutovg_matrix_t shifted;
        plutovg_matrix_init_translate(&shifted, 40, 15);
        plutovg_matrix_t scaled;
        plutovg_matrix_init_scale(&scaled, 1.25f, 0.8f);

        plutovg_matrix_t matrices[3] = { identity, shifted, scaled };
        for(int pass = 0; pass < 6; ++pass) {
            const plutovg_matrix_t& matrix = matrices[pass % 3];
            plutovg_canvas_set_matrix(canvas, &matrix);
            clear_surface(surface);
            plutovg_canvas_paint(canvas);
            for(int y = 20; y < 180; y += 23) {
                for(int x = 20; x < 180; x += 23) {
                    uint32_t got = pixel_at(surface, x, y);
                    uint32_t expected = ref_pixel(&patch, 1, NULL, &matrix, x, y, 1.0f);
                    if(!colors_match(got, expected))
                        fail(c, "paint reuse under changing transforms", x, y, got, expected);
                }
            }
        }

        c.check(plutovg_paint_get_reference_count(paint) == 2, "reference count stable across reuse");
        plutovg_canvas_destroy(canvas);
        c.check(plutovg_paint_get_reference_count(paint) == 1, "canvas destroy releases its reference");
        plutovg_paint_destroy(paint);
        plutovg_surface_destroy(surface);
    });
}

} // namespace

void register_task_tests(Registry& registry)
{
    register_geometry(registry);
    register_seams(registry);
    register_compositing(registry);
    register_ownership(registry);
}
