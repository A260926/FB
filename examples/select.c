/*
 * select.c - Offscreen name-based picking example for MyTinyGL.
 *
 * The program runs entirely headless (no SDL, no display):
 *   1. Enters GL_SELECT and picks several primitives by name, printing
 *      the raw hit records written to the selection buffer.
 *   2. Returns to GL_RENDER and draws the scene in color, verifying that
 *      ordinary rasterization still produces colored pixels afterwards.
 */

#include "src/mytinygl.h"
#include <stdio.h>
#include <string.h>

#define WIDTH  64
#define HEIGHT 64

static void draw_scene(void)
{
    /* Three stacked triangles plus a line and a point. */
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f, -0.8f,  0.20f);
    glVertex3f( 0.0f, -0.8f,  0.20f);
    glVertex3f(-0.4f,  0.2f,  0.20f);

    glVertex3f( 0.0f, -0.8f,  0.20f);
    glVertex3f( 0.8f, -0.8f,  0.20f);
    glVertex3f( 0.4f,  0.2f,  0.20f);

    glVertex3f(-0.3f,  0.1f, -0.30f);
    glVertex3f( 0.3f,  0.1f, -0.30f);
    glVertex3f( 0.0f,  0.8f, -0.30f);
    glEnd();

    glBegin(GL_LINES);
    glVertex3f(-0.9f, 0.0f, 0.0f);
    glVertex3f( 0.9f, 0.0f, 0.0f);
    glEnd();

    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();
}

int main(void)
{
    GLState *ctx = gl_create_context(WIDTH, HEIGHT);
    if (!ctx) {
        fprintf(stderr, "failed to create context\n");
        return 1;
    }
    gl_make_current(ctx);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* ---- selection pass ------------------------------------------------ */
    GLuint select_buf[256];
    memset(select_buf, 0, sizeof(select_buf));
    glSelectBuffer(256, select_buf);

    if (glRenderMode(GL_SELECT) != GL_RENDER) {
        fprintf(stderr, "unexpected previous render mode\n");
        return 2;
    }
    glInitNames();

    /* Names 1 and 2: the two near triangles; name 3: the far triangle.
     * The line and point are drawn on an empty name stack. */
    glPushName(1);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f, -0.8f, 0.20f);
    glVertex3f( 0.0f, -0.8f, 0.20f);
    glVertex3f(-0.4f,  0.2f, 0.20f);
    glEnd();
    glLoadName(2);
    glBegin(GL_TRIANGLES);
    glVertex3f( 0.0f, -0.8f, 0.20f);
    glVertex3f( 0.8f, -0.8f, 0.20f);
    glVertex3f( 0.4f,  0.2f, 0.20f);
    glEnd();
    glPopName();

    glPushName(3);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.3f, 0.1f, -0.30f);
    glVertex3f( 0.3f, 0.1f, -0.30f);
    glVertex3f( 0.0f, 0.8f, -0.30f);
    glEnd();
    glPopName();

    glInitNames();  /* empty stack for the remaining primitives */
    glBegin(GL_LINES);
    glVertex3f(-0.9f, 0.0f, 0.0f);
    glVertex3f( 0.9f, 0.0f, 0.0f);
    glEnd();
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();

    GLint hits = glRenderMode(GL_RENDER);
    if (hits < 0) {
        fprintf(stderr, "selection buffer overflow\n");
        return 3;
    }

    printf("SELECT pass: %d hit record(s)\n", (int)hits);
    GLsizei i = 0;
    for (GLint h = 0; h < hits; h++) {
        GLuint name_count = select_buf[i];
        GLuint min_depth = select_buf[i + 1];
        GLuint max_depth = select_buf[i + 2];
        printf("  record %d: names=%u min=%u max=%u stack=[",
               h + 1, name_count, min_depth, max_depth);
        for (GLuint n = 0; n < name_count; n++) {
            printf("%s%u", n ? ", " : "", select_buf[i + 3 + n]);
        }
        printf("]\n");
        i += (GLsizei)(3 + name_count);
    }
    if (hits != 4) {
        fprintf(stderr, "expected 4 hits (3 named + 1 empty-stack), got %d\n",
                (int)hits);
        return 4;
    }

    /* ---- ordinary render pass ------------------------------------------ */
    /* Opaque black is stored with its alpha channel set. */
    const pixel_t clear_pixel = color_to_rgba32(color(0.0f, 0.0f, 0.0f, 1.0f));
    size_t colored_before = 0;
    size_t total = (size_t)WIDTH * HEIGHT;
    for (size_t p = 0; p < total; p++) {
        if (ctx->framebuffer.color[p] != clear_pixel) colored_before++;
    }
    if (colored_before != 0) {
        fprintf(stderr, "selection pass wrote %zu color pixels\n",
                colored_before);
        return 5;
    }

    glColor3f(1.0f, 0.0f, 0.0f);
    draw_scene();

    size_t colored_after = 0;
    for (size_t p = 0; p < total; p++) {
        if (ctx->framebuffer.color[p] != clear_pixel) colored_after++;
    }
    printf("RENDER pass: %zu colored pixel(s)\n", colored_after);
    if (colored_after < 100) {
        fprintf(stderr, "render pass produced too few colored pixels\n");
        return 6;
    }

    if (glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "unexpected GL error\n");
        return 7;
    }

    gl_make_current(NULL);
    gl_destroy_context(ctx);
    printf("select example OK\n");
    return 0;
}
