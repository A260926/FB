/*
 * Fragment bytecode shader example.
 *
 * Generates a brick texture procedurally, builds a material program with a
 * REPEAT loop, IF/ELSE branches, a DISCARD_IF pattern and DDY-based
 * shading, draws with it into an offscreen ZBuffer, and writes:
 *   <outdir>/surface.ppm  the rendered image
 *   <outdir>/trace.csv    inputs, register derivatives and write mask of
 *                         one executed quad
 *
 * Usage: shader_vm <output-directory>
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <TGL/gl.h>
#include <TGL/glshader.h>
#include "zbuffer.h"

#define W 128
#define H 128
#define TEXDIM 256

static unsigned char g_tex[TEXDIM * TEXDIM * 3];

/* ------------------------------------------------------------------ */
/* scene                                                                */
/* ------------------------------------------------------------------ */

static void make_texture(void)
{
    int x, y;
    for (y = 0; y < TEXDIM; y++)
        for (x = 0; x < TEXDIM; x++) {
            int row = y / 32;
            int col = (x + (row & 1) * 32) / 64;
            int mx = (x + (row & 1) * 32) % 64;
            int my = y % 32;
            int mortar = (mx < 4) || (my < 3);
            float base;
            if (mortar)
                base = 0.20f;
            else
                base = 0.55f + 0.35f * (float) ((row * 7 + col * 13) % 5) / 4.0f;
            /* speckle band that the material program discards */
            if (!mortar && (x * 31 + y * 17) % 23 == 0)
                base = 0.42f;
            g_tex[(y * TEXDIM + x) * 3 + 0] = (unsigned char) (base * 255.0f);
            g_tex[(y * TEXDIM + x) * 3 + 1] =
                (unsigned char) (base * 0.82f * 255.0f);
            g_tex[(y * TEXDIM + x) * 3 + 2] =
                (unsigned char) (base * 0.68f * 255.0f);
        }
}

/* brick material: coordinate warp loop, mortar branch, speckle discard */
static const TGLShaderInst g_material[] = {
    {TGL_SHADER_CONST, 8, 0, 0, 0, {4.0f, 4.0f, 4.0f, 4.0f}, 0},
    {TGL_SHADER_MUL, 3, 0, 8, 0, {0, 0, 0, 0}, 0}, /* p = uv * 4 */
    {TGL_SHADER_CONST, 9, 0, 0, 0, {0.5f, 0.5f, 0.5f, 0.5f}, 0},
    {TGL_SHADER_REPEAT, 0, 0, 0, 0, {0, 0, 0, 0}, 3},
    {TGL_SHADER_TEX, 4, 3, 0, 0, {0, 0, 0, 0}, 0}, /* warp sample */
    {TGL_SHADER_MUL, 4, 4, 9, 0, {0, 0, 0, 0}, 0},
    {TGL_SHADER_ADD, 3, 3, 4, 0, {0, 0, 0, 0}, 0}, /* p += 0.5 * tex */
    {TGL_SHADER_ENDREPEAT, 0, 0, 0, 0, {0, 0, 0, 0}, 0},
    {TGL_SHADER_TEX, 4, 3, 0, 0, {0, 0, 0, 0}, 0}, /* final sample */
    {TGL_SHADER_CONST, 10, 0, 0, 0, {0.35f, 0.35f, 0.35f, 0.35f}, 0},
    {TGL_SHADER_LT, 5, 4, 10, 0, {0, 0, 0, 0}, 0}, /* dark texel? */
    {TGL_SHADER_IF, 0, 5, 0, 0, {0, 0, 0, 0}, 0},
    {TGL_SHADER_CONST, 6, 0, 0, 0, {0.13f, 0.11f, 0.10f, 1.0f}, 0}, /* mortar */
    {TGL_SHADER_ELSE, 0, 0, 0, 0, {0, 0, 0, 0}, 0},
    {TGL_SHADER_MUL, 6, 4, 1, 0, {0, 0, 0, 0}, 0}, /* brick * gouraud tint */
    {TGL_SHADER_ENDIF, 0, 0, 0, 0, {0, 0, 0, 0}, 0},
    {TGL_SHADER_DDY, 7, 0, 0, 0, {0, 0, 0, 0}, 0}, /* dv/dy */
    {TGL_SHADER_MAD, 6, 7, 9, 6, {0, 0, 0, 0}, 0}, /* color += 0.5 * ddy */
    {TGL_SHADER_CONST, 11, 0, 0, 0, {0.45f, 0.45f, 0.45f, 0.45f}, 0},
    {TGL_SHADER_LT, 5, 4, 11, 0, {0, 0, 0, 0}, 0}, /* tex.r < 0.45 */
    {TGL_SHADER_CONST, 12, 0, 0, 0, {0.40f, 0.40f, 0.40f, 0.40f}, 0},
    {TGL_SHADER_LT, 7, 12, 4, 0, {0, 0, 0, 0}, 0}, /* 0.40 < tex.r */
    {TGL_SHADER_MUL, 5, 5, 7, 0, {0, 0, 0, 0}, 0}, /* speckle band */
    {TGL_SHADER_DISCARD_IF, 0, 5, 0, 0, {0, 0, 0, 0}, 0},
    {TGL_SHADER_OUTPUT, 0, 6, 0, 0, {0, 0, 0, 0}, 0},
};

/* derivative probes used for the trace: out = DDX r0 / DDY r0 */
static const TGLShaderInst g_probe_ddx[] = {
    {TGL_SHADER_DDX, 3, 0, 0, 0, {0, 0, 0, 0}, 0},
    {TGL_SHADER_OUTPUT, 0, 3, 0, 0, {0, 0, 0, 0}, 0},
};
static const TGLShaderInst g_probe_ddy[] = {
    {TGL_SHADER_DDY, 3, 0, 0, 0, {0, 0, 0, 0}, 0},
    {TGL_SHADER_OUTPUT, 0, 3, 0, 0, {0, 0, 0, 0}, 0},
};

static void background(void)
{
    /* fixed-pipeline gradient, visible through the discarded speckles */
    glShadeModel(GL_SMOOTH);
    glBegin(GL_TRIANGLES);
    glColor3f(0.05f, 0.10f, 0.25f);
    glVertex3f(-1.0f, -1.0f, 0.9f);
    glColor3f(0.10f, 0.35f, 0.45f);
    glVertex3f(1.0f, -1.0f, 0.9f);
    glColor3f(0.45f, 0.55f, 0.20f);
    glVertex3f(1.0f, 1.0f, 0.9f);
    glEnd();
    glBegin(GL_TRIANGLES);
    glColor3f(0.05f, 0.10f, 0.25f);
    glVertex3f(-1.0f, -1.0f, 0.9f);
    glColor3f(0.45f, 0.55f, 0.20f);
    glVertex3f(1.0f, 1.0f, 0.9f);
    glColor3f(0.20f, 0.45f, 0.50f);
    glVertex3f(-1.0f, 1.0f, 0.9f);
    glEnd();
}

static void material_triangles(void)
{
    /* first slab, partially clipped by the right edge of the view */
    glColor3f(1.0f, 0.85f, 0.70f);
    glBegin(GL_TRIANGLES);
    glTexCoord2f(0.05f, 0.05f);
    glVertex3f(-0.9f, -0.75f, 0.2f);
    glTexCoord2f(0.95f, 0.10f);
    glVertex3f(1.35f, -0.55f, 0.2f);
    glTexCoord2f(0.80f, 0.90f);
    glVertex3f(0.95f, 0.85f, 0.2f);
    glEnd();
    /* second slab in front */
    glColor3f(0.75f, 0.85f, 1.0f);
    glBegin(GL_TRIANGLES);
    glTexCoord2f(0.10f, 0.20f);
    glVertex3f(-0.95f, -0.95f, -0.2f);
    glTexCoord2f(0.60f, 0.15f);
    glVertex3f(0.35f, -0.80f, -0.2f);
    glTexCoord2f(0.25f, 0.85f);
    glVertex3f(-0.60f, 0.60f, -0.2f);
    glEnd();
}

static int write_ppm(const char *path, ZBuffer *zb)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            PIXEL p = zb->pbuf[y * W + x];
            unsigned char rgb[3];
            rgb[0] = (unsigned char) GET_RED(p);
            rgb[1] = (unsigned char) GET_GREEN(p);
            rgb[2] = (unsigned char) GET_BLUE(p);
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    return 0;
}

/* sampler over the generated texture for the independent quad executor */
static void trace_sampler(void *ctx, int tx, int ty, float rgba[4])
{
    const unsigned char *t = (const unsigned char *) ctx + (ty * TEXDIM + tx) * 3;
    rgba[0] = t[0] / 255.0f;
    rgba[1] = t[1] / 255.0f;
    rgba[2] = t[2] / 255.0f;
    rgba[3] = 1.0f;
}

static int write_trace(const char *path, TGLShaderProgram *material)
{
    FILE *f = fopen(path, "w");
    TGLShaderProgram *pdx, *pdy;
    TGLShaderError err;
    float in[4][3][4];
    float out[4][4], odx[4][4], ody[4][4];
    unsigned coverage = 0xF, wmask = 0, wdx = 0, wdy = 0;
    int lane, k, rc;

    if (!f)
        return -1;

    /* one representative quad of the second slab, lanes TL/TR/BL/BR */
    for (lane = 0; lane < 4; lane++) {
        float px = 24.0f + (float) (lane & 1);
        float py = 30.0f + (float) (lane >> 1);
        in[lane][0][0] = 0.30f + 0.02f * (float) (lane & 1);  /* u */
        in[lane][0][1] = 0.40f + 0.03f * (float) (lane >> 1); /* v */
        in[lane][0][2] = 0.0f;
        in[lane][0][3] = 1.0f;
        in[lane][1][0] = 0.75f; /* gouraud tint */
        in[lane][1][1] = 0.85f;
        in[lane][1][2] = 1.0f;
        in[lane][1][3] = 1.0f;
        in[lane][2][0] = px + 0.5f; /* pixel center */
        in[lane][2][1] = py + 0.5f;
        in[lane][2][2] = 0.6f; /* window depth */
        in[lane][2][3] = 1.0f; /* 1/clip.w */
    }

    rc = tglShaderExecQuad(material, in, out, coverage, &wmask, trace_sampler,
                           g_tex);
    if (rc != 0) {
        fclose(f);
        return -1;
    }
    pdx = tglShaderCreate(g_probe_ddx, 2, &err);
    pdy = tglShaderCreate(g_probe_ddy, 2, &err);
    if (!pdx || !pdy) {
        fclose(f);
        return -1;
    }
    if (tglShaderExecQuad(pdx, in, odx, coverage, &wdx, NULL, NULL) != 0 ||
        tglShaderExecQuad(pdy, in, ody, coverage, &wdy, NULL, NULL) != 0) {
        fclose(f);
        return -1;
    }

    fprintf(f, "section,lane,reg,v0,v1,v2,v3\n");
    for (lane = 0; lane < 4; lane++) {
        static const char *names[3] = {"r0", "r1", "r2"};
        for (k = 0; k < 3; k++)
            fprintf(f, "input,%d,%s,%.6f,%.6f,%.6f,%.6f\n", lane, names[k],
                    (double) in[lane][k][0], (double) in[lane][k][1],
                    (double) in[lane][k][2], (double) in[lane][k][3]);
        fprintf(f, "output,%d,out,%.6f,%.6f,%.6f,%.6f\n", lane,
                (double) out[lane][0], (double) out[lane][1],
                (double) out[lane][2], (double) out[lane][3]);
        fprintf(f, "ddx,%d,r0,%.6f,%.6f,%.6f,%.6f\n", lane,
                (double) odx[lane][0], (double) odx[lane][1],
                (double) odx[lane][2], (double) odx[lane][3]);
        fprintf(f, "ddy,%d,r0,%.6f,%.6f,%.6f,%.6f\n", lane,
                (double) ody[lane][0], (double) ody[lane][1],
                (double) ody[lane][2], (double) ody[lane][3]);
    }
    fprintf(f, "mask,coverage,%u,%u,%u,%u\n", coverage & 1, (coverage >> 1) & 1,
            (coverage >> 2) & 1, (coverage >> 3) & 1);
    fprintf(f, "mask,write,%u,%u,%u,%u\n", wmask & 1, (wmask >> 1) & 1,
            (wmask >> 2) & 1, (wmask >> 3) & 1);
    fprintf(f, "mask,discard,%u,%u,%u,%u\n", (coverage & ~wmask) & 1,
            ((coverage & ~wmask) >> 1) & 1, ((coverage & ~wmask) >> 2) & 1,
            ((coverage & ~wmask) >> 3) & 1);

    tglShaderRelease(pdx);
    tglShaderRelease(pdy);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    char path[1024];
    ZBuffer *zb;
    TGLShaderProgram *material;
    TGLShaderError err;
    GLuint tex;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
        return 1;
    }

    make_texture();

    material = tglShaderCreate(g_material,
                               sizeof(g_material) / sizeof(g_material[0]),
                               &err);
    if (!material) {
        fprintf(stderr, "material program rejected: code=%d index=%d\n",
                err.code, err.index);
        return 1;
    }

    zb = ZB_open(W, H, ZB_MODE_RGBA, NULL);
    if (!zb) {
        fprintf(stderr, "ZB_open failed\n");
        return 1;
    }
    glInit(zb);
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glShadeModel(GL_SMOOTH);
    glClearColor(0.02f, 0.02f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, TEXDIM, TEXDIM, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, g_tex);
    glEnable(GL_TEXTURE_2D);

    background();
    tglBindShader(material);
    material_triangles();
    tglBindShader(NULL);

    {
        int shader_err = tglShaderGetError();
        if (shader_err != TGL_SHADER_RT_NONE)
            fprintf(stderr, "shader runtime error %d\n", shader_err);
        /* keep going: still write the outputs */
    }

    snprintf(path, sizeof path, "%s/surface.ppm", argv[1]);
    if (write_ppm(path, zb) != 0) {
        fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    snprintf(path, sizeof path, "%s/trace.csv", argv[1]);
    if (write_trace(path, material) != 0) {
        fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }

    printf("shader_vm: wrote %s/surface.ppm and %s/trace.csv\n", argv[1],
           argv[1]);

    tglShaderRelease(material);
    glClose();
    ZB_close(zb);
    return 0;
}
