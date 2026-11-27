/*
 * User clipping plane example.
 *
 * Draws a colored square and cuts it with two arbitrary user clipping
 * planes (GL_CLIP_PLANE0/1), then writes the result to clip_planes_demo.ppm
 * using only the C standard library and TinyGL - no SDL or image helpers.
 *
 * Build: cc -std=c99 -Iinclude -I. examples/raw/clip_planes.c \
 *          lib/libTinyGL.a -lm -o clip_planes_demo
 */
#include <TGL/gl.h>
#include "zbuffer.h"

#include <stdio.h>
#include <stdlib.h>

#define W 128
#define H 128

int main(void)
{
    ZBuffer *zb = ZB_open(W, H, ZB_MODE_RGBA, NULL);
    FILE *f;
    if (!zb)
        return 1;

    glInit(zb);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 0);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Keep the intersection of two object-space half spaces:
     *   plane 0: x      >= 0   (equation 1,0,0,0)
     *   plane 1: x + y  >= -0.2 (equation 1,1,0,0.2)
     * Equations are frozen to eye space with the inverse transpose of the
     * MODELVIEW matrix active at glClipPlane time. */
    {
        GLdouble right_half[4] = {1.0, 0.0, 0.0, 0.0};
        GLdouble oblique[4] = {1.0, 1.0, 0.0, 0.2};
        glClipPlane(GL_CLIP_PLANE0, right_half);
        glClipPlane(GL_CLIP_PLANE1, oblique);
        glEnable(GL_CLIP_PLANE0);
        glEnable(GL_CLIP_PLANE1);
    }

    /* a smoothly shaded square spanning the whole normalized view */
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex3f(-1.0f, -1.0f, 0.0f);
    glColor3f(0.0f, 1.0f, 0.0f);
    glVertex3f(1.0f, -1.0f, 0.0f);
    glColor3f(0.0f, 0.0f, 1.0f);
    glVertex3f(1.0f, 1.0f, 0.0f);

    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex3f(-1.0f, -1.0f, 0.0f);
    glColor3f(0.0f, 0.0f, 1.0f);
    glVertex3f(1.0f, 1.0f, 0.0f);
    glColor3f(1.0f, 1.0f, 0.0f);
    glVertex3f(-1.0f, 1.0f, 0.0f);
    glEnd();

    f = fopen("clip_planes_demo.ppm", "wb");
    if (!f) {
        glClose();
        ZB_close(zb);
        return 2;
    }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char rgb[3] = {
            (unsigned char) GET_RED(zb->pbuf[i]),
            (unsigned char) GET_GREEN(zb->pbuf[i]),
            (unsigned char) GET_BLUE(zb->pbuf[i]),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);

    glClose();
    ZB_close(zb);
    return 0;
}
