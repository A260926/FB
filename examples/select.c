/*
 * MyTinyGL - GL_SELECT selection example
 *
 * Runs one selection pass over a small scene and prints the hit
 * records, then renders normally and verifies that pixels were drawn.
 * Fully off-screen: no SDL, no display.
 */

#include "src/mytinygl.h"
#include <stdio.h>

int main(void)
{
    GLState *c = gl_create_context(64, 64);
    GLuint buf[32];
    GLint hits, h;
    GLuint *p;
    size_t i;
    size_t colored = 0;

    if (!c) return 1;
    gl_make_current(c);

    /* --- selection pass: pick geometry by name --- */
    glSelectBuffer(32, buf);
    glRenderMode(GL_SELECT);
    glInitNames();
    glPushName(1);
    /* visible triangle */
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f( 0.5f, -0.5f, 0.0f);
    glVertex3f( 0.0f,  0.5f, 0.0f);
    glEnd();
    glLoadName(2);
    /* off-screen triangle: no hit */
    glBegin(GL_TRIANGLES);
    glVertex3f(5.0f, 5.0f, 0.0f);
    glVertex3f(6.0f, 5.0f, 0.0f);
    glVertex3f(5.0f, 6.0f, 0.0f);
    glEnd();
    glLoadName(3);
    /* visible line */
    glBegin(GL_LINES);
    glVertex3f(-0.8f, 0.8f, -0.5f);
    glVertex3f( 0.8f, 0.8f, -0.5f);
    glEnd();
    glPopName();

    hits = glRenderMode(GL_RENDER);
    printf("selection hits: %d\n", (int)hits);
    p = buf;
    for (h = 0; h < hits; h++) {
        GLuint n = p[0];
        GLuint j;
        printf("  hit %d: names=%u zmin=%u zmax=%u stack=",
               (int)h, n, p[1], p[2]);
        for (j = 0; j < n; j++) {
            printf("%s%u", j ? "," : "", p[3 + j]);
        }
        printf("\n");
        p += 3 + n;
    }

    /* names 1 (triangle) and 3 (line) hit, name 2 was off-screen */
    if (hits != 2) return 1;
    if (buf[0] != 1 || buf[3] != 1) return 1;
    if (buf[4] != 1 || buf[7] != 3) return 1;

    /* --- normal rendering resumes afterwards --- */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(1.0f, 0.0f, 0.0f);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f( 0.5f, -0.5f, 0.0f);
    glVertex3f( 0.0f,  0.5f, 0.0f);
    glEnd();

    for (i = 0; i < 64 * 64; i++) {
        if (c->framebuffer.color[i] != 0xFF000000u) colored++;
    }
    printf("rendered colored pixels: %zu\n", colored);

    if (colored == 0) return 1;
    if (glGetError() != GL_NO_ERROR) return 1;

    gl_make_current(NULL);
    gl_destroy_context(c);
    return 0;
}
