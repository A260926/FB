/*
 * Renders a small tensor-product cubic Bézier mesh patch with PlutoVG and
 * writes the result to a PPM file.
 *
 *   mesh_ppm <output.ppm>
 *
 * The patch is defined by a 4x4 control net in paint space. Corner colors
 * are interpolated bilinearly in the parameter domain after premultiplying
 * each corner RGB by its alpha. The patch is painted through an ordinary
 * canvas paint, so it participates in clipping, compositing and opacity like
 * any other paint.
 */

#include <plutovg.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int clamp_byte(int value)
{
    if(value < 0) return 0;
    if(value > 255) return 255;
    return value;
}

static int write_ppm(const plutovg_surface_t* surface, const char* filename)
{
    const int width = plutovg_surface_get_width(surface);
    const int height = plutovg_surface_get_height(surface);
    const int stride = plutovg_surface_get_stride(surface);
    const unsigned char* data = plutovg_surface_get_data(surface);
    FILE* file = fopen(filename, "wb");
    if(file == NULL)
        return 0;

    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for(int y = 0; y < height; ++y) {
        const uint32_t* row = (const uint32_t*)(data + y * stride);
        for(int x = 0; x < width; ++x) {
            uint32_t pixel = row[x];
            uint32_t a = (pixel >> 24) & 0xff;
            uint32_t r = (pixel >> 16) & 0xff;
            uint32_t g = (pixel >> 8) & 0xff;
            uint32_t b = (pixel >> 0) & 0xff;
            /* Un-premultiply and composite over an opaque white page. */
            int ur = a ? clamp_byte((int)r * 255 / (int)a) : 0;
            int ug = a ? clamp_byte((int)g * 255 / (int)a) : 0;
            int ub = a ? clamp_byte((int)b * 255 / (int)a) : 0;
            unsigned char rgb[3] = {
                (unsigned char)((ur * a + 255 * (255 - a)) / 255),
                (unsigned char)((ug * a + 255 * (255 - a)) / 255),
                (unsigned char)((ub * a + 255 * (255 - a)) / 255)
            };
            fwrite(rgb, 1, 3, file);
        }
    }

    fclose(file);
    return 1;
}

/*
 * Builds a control net whose interior bulges, so the internal color bands
 * curve rather than following the straight diagonal between the corners.
 */
static plutovg_mesh_patch_t make_patch(void)
{
    plutovg_mesh_patch_t patch;
    const float grid[4] = { 40.f, 90.f, 150.f, 200.f };
    for(int j = 0; j < 4; ++j) {
        for(int i = 0; i < 4; ++i) {
            float x = grid[i];
            float y = grid[j];
            if(i > 0 && i < 3) {
                x += (i == 1 ? -20.f : 20.f);
            }

            if(j > 0 && j < 3) {
                y += (j == 1 ? -20.f : 20.f);
            }

            patch.points[4 * j + i] = PLUTOVG_MAKE_POINT(x, y);
        }
    }

    /* (0,0), (1,0), (1,1), (0,1) corner colors. */
    patch.colors[0] = PLUTOVG_MAKE_COLOR(0.95f, 0.25f, 0.20f, 1.f);
    patch.colors[1] = PLUTOVG_MAKE_COLOR(0.20f, 0.80f, 0.45f, 0.85f);
    patch.colors[2] = PLUTOVG_MAKE_COLOR(0.20f, 0.35f, 0.95f, 1.f);
    patch.colors[3] = PLUTOVG_MAKE_COLOR(0.95f, 0.85f, 0.20f, 0.9f);
    return patch;
}

int main(int argc, char** argv)
{
    const char* filename = argc > 1 ? argv[1] : "mesh.ppm";
    const int width = 240;
    const int height = 220;

    plutovg_surface_t* surface = plutovg_surface_create(width, height);
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);

    /* A rounded white page so the patch edge is easy to see. */
    plutovg_canvas_set_rgb(canvas, 1, 1, 1);
    plutovg_canvas_paint(canvas);

    plutovg_mesh_patch_t patch = make_patch();
    plutovg_matrix_t matrix;
    plutovg_matrix_init_translate(&matrix, 10, 10);
    plutovg_paint_t* paint = plutovg_paint_create_mesh(&patch, 1, &matrix);
    if(paint == NULL) {
        fprintf(stderr, "failed to create mesh paint\n");
        plutovg_canvas_destroy(canvas);
        plutovg_surface_destroy(surface);
        return 1;
    }

    /* The paint flows through the normal fill path. */
    plutovg_canvas_set_paint(canvas, paint);
    plutovg_canvas_fill_rect(canvas, 0, 0, width, height);
    plutovg_paint_destroy(paint);

    /* A translucent patch on top shows ordinary compositing. */
    plutovg_canvas_set_opacity(canvas, 0.55f);
    plutovg_mesh_patch_t overlay = patch;
    plutovg_matrix_t overlay_matrix;
    plutovg_matrix_init_scale(&overlay_matrix, 0.9f, 0.9f);
    plutovg_matrix_translate(&overlay_matrix, 20, 10);
    plutovg_paint_t* overlay_paint = plutovg_paint_create_mesh(&overlay, 1, &overlay_matrix);
    if(overlay_paint == NULL) {
        fprintf(stderr, "failed to create overlay paint\n");
        plutovg_canvas_destroy(canvas);
        plutovg_surface_destroy(surface);
        return 1;
    }

    plutovg_canvas_set_paint(canvas, overlay_paint);
    plutovg_canvas_rect(canvas, 30, 30, 150, 150);
    plutovg_canvas_fill(canvas);
    plutovg_paint_destroy(overlay_paint);

    int ok = write_ppm(surface, filename);
    plutovg_canvas_destroy(canvas);
    plutovg_surface_destroy(surface);
    if(!ok) {
        fprintf(stderr, "failed to write %s\n", filename);
        return 1;
    }

    printf("wrote %s\n", filename);
    return 0;
}
