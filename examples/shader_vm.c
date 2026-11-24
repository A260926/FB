/*
 * examples/shader_vm.c — fragment bytecode material preview.
 *
 * Renders a full-screen quad with a small bytecode material program
 * (texture fetch, DDX/DDY-based filtering, a REPEAT/BREAK_IF banding loop
 * and a DISCARD_IF seam) into surface.ppm, and dumps the inputs, register
 * derivatives and write mask of one executed quad to trace.csv.
 *
 * Usage: shader_vm_example [output-dir]
 *
 * Pure C99 + standard library; no window system or GPU required.
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

static TGLShaderInstr instr(GLint op, GLint dst, GLint a, GLint b, GLint c)
{
    TGLShaderInstr in;
    in.op = op;
    in.dst = dst;
    in.a = a;
    in.b = b;
    in.c = c;
    in.imm[0] = in.imm[1] = in.imm[2] = in.imm[3] = 0.0f;
    in.count = 0;
    return in;
}

static TGLShaderInstr iconst(GLint dst, GLfloat x, GLfloat y, GLfloat z,
                             GLfloat w)
{
    TGLShaderInstr in = instr(CONST, dst, 0, 0, 0);
    in.imm[0] = x;
    in.imm[1] = y;
    in.imm[2] = z;
    in.imm[3] = w;
    return in;
}

static TGLShaderInstr irepeat(GLuint count)
{
    TGLShaderInstr in = instr(REPEAT, 0, 0, 0, 0);
    in.count = count;
    return in;
}

/* the material program: texture * tint, derivative-based dimming, a
 * threshold-counting loop for brightness bands, and a discarded seam */
static GLuint build_material(TGLShaderInstr *code)
{
    GLuint n = 0;
    code[n++] = iconst(3, 0.85f, 0.75f, 0.55f, 1.0f); /* warm tint */
    code[n++] = instr(TEX, 4, 0, 0, 0);               /* r4 = texel(uv) */
    code[n++] = instr(MUL, 4, 4, 3, 0);               /* r4 *= tint */
    code[n++] = instr(DDX, 5, 0, 0, 0);               /* r5 = d(uv)/dx */
    code[n++] = instr(DDY, 6, 0, 0, 0);               /* r6 = d(uv)/dy */
    code[n++] = instr(MUL, 5, 5, 5, 0);
    code[n++] = instr(MAD, 5, 6, 6, 5);               /* |ddx|^2+|ddy|^2 */
    code[n++] = iconst(7, 0.0004f, 0.0004f, 0.0004f, 0.0004f);
    code[n++] = instr(LT, 8, 7, 5, 0);                /* high frequency? */
    code[n++] = instr(IF, 0, 8, 0, 0);
    code[n++] = iconst(9, 0.5f, 0.5f, 0.5f, 1.0f);
    code[n++] = instr(MUL, 4, 4, 9, 0); /* crude anti-alias dim */
    code[n++] = instr(ENDIF, 0, 0, 0, 0);
    code[n++] = iconst(10, 0, 0, 0, 0);             /* band accumulator */
    code[n++] = iconst(11, 0, 0, 0, 0);             /* threshold */
    code[n++] = iconst(12, 0.25f, 0.25f, 0.25f, 0.25f);
    code[n++] = iconst(13, 0.12f, 0.12f, 0.12f, 0.12f);
    code[n++] = irepeat(4);
    code[n++] = instr(ADD, 11, 11, 12, 0); /* threshold += 0.25 */
    code[n++] = instr(LT, 14, 0, 11, 0);   /* u < threshold? */
    code[n++] = instr(BREAK_IF, 0, 14, 0, 0);
    code[n++] = instr(ADD, 10, 10, 13, 0); /* band += 0.12 */
    code[n++] = instr(ENDREPEAT, 0, 0, 0, 0);
    code[n++] = instr(ADD, 4, 4, 10, 0); /* apply the band */
    code[n++] = iconst(15, 0.03f, 0.03f, 0.03f, 0.03f);
    code[n++] = instr(LT, 14, 0, 15, 0); /* u < 0.03? */
    code[n++] = instr(DISCARD_IF, 0, 14, 0, 0); /* cut the seam */
    code[n++] = instr(OUTPUT, 0, 4, 0, 0);
    return n;
}

static void build_texture(GLubyte *rgb)
{
    GLint x, y;
    for (y = 0; y < 256; y++)
        for (x = 0; x < 256; x++) {
            GLint checker = ((x >> 5) ^ (y >> 5)) & 1;
            rgb[(y * 256 + x) * 3 + 0] = (GLubyte) (checker ? x : 255 - x);
            rgb[(y * 256 + x) * 3 + 1] = (GLubyte) (checker ? y : 255 - y);
            rgb[(y * 256 + x) * 3 + 2] = (GLubyte) (checker ? 255 : 40);
        }
}

static int write_ppm(const char *path, const ZBuffer *zb)
{
    FILE *f = fopen(path, "wb");
    GLint x, y;
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            PIXEL p = zb->pbuf[y * W + x];
            fputc((int) GET_RED(p), f);
            fputc((int) GET_GREEN(p), f);
            fputc((int) GET_BLUE(p), f);
        }
    fclose(f);
    return 0;
}

/* Inputs for the quad at pixel (qx,qy) of the scene below, in closed
 * form: with the identity matrices, u = ndc.x + 1, v = ndc.y + 1, the
 * vertex colors are linear ramps, ndc.z = 0.3 and clip.w = 1. */
static void build_quad_inputs(GLint qx, GLint qy,
                              GLfloat in[4][TGL_SHADER_INPUT_REGS][4])
{
    static const GLfloat vp = (128 - 0.5f) / 2.0f; /* viewport scale/trans */
    GLint lane;
    for (lane = 0; lane < 4; lane++) {
        GLfloat cx = (GLfloat) (qx + (lane & 1)) + 0.5f;
        GLfloat cy = (GLfloat) (qy + (lane >> 1)) + 0.5f;
        GLfloat ndcx = (cx - vp) / vp;
        GLfloat ndcy = (vp - cy) / vp;
        in[lane][0][0] = ndcx + 1.0f; /* u */
        in[lane][0][1] = ndcy + 1.0f; /* v */
        in[lane][0][2] = 0.0f;
        in[lane][0][3] = 1.0f;
        in[lane][1][0] = (ndcx + 1.0f) * 0.4f + 0.1f; /* gouraud r */
        in[lane][1][1] = (ndcy + 1.0f) * 0.4f + 0.1f; /* gouraud g */
        in[lane][1][2] = 0.5f;                        /* gouraud b */
        in[lane][1][3] = 1.0f;                        /* gouraud a */
        in[lane][2][0] = cx;
        in[lane][2][1] = cy;
        in[lane][2][2] = (0.3f + 1.0f) * 0.5f; /* [0,1] depth */
        in[lane][2][3] = 1.0f;                 /* 1/clip.w */
    }
}

static int write_trace(const char *path, const TGLShaderProgram *prog,
                       const GLuint *texels, GLint qx, GLint qy)
{
    TGLShaderInstr probe_code[2];
    TGLShaderProgram *probe_x = NULL, *probe_y = NULL;
    GLfloat in[4][TGL_SHADER_INPUT_REGS][4];
    GLfloat out[4][4], dx_out[4][4], dy_out[4][4];
    GLuint mask = 0, mx = 0, my = 0;
    FILE *f;
    GLint lane, err;

    probe_code[0] = instr(DDX, 3, 0, 0, 0);
    probe_code[1] = instr(OUTPUT, 0, 3, 0, 0);
    probe_x = tglShaderCreate(probe_code, 2, NULL);
    probe_code[0] = instr(DDY, 3, 0, 0, 0);
    probe_y = tglShaderCreate(probe_code, 2, NULL);
    if (!probe_x || !probe_y)
        return -1;

    build_quad_inputs(qx, qy, in);
    err = tglShaderExecuteQuad(prog, in, out, 0xF, &mask, texels);
    if (err != TGL_SHADER_EXEC_OK)
        return -1;
    if (tglShaderExecuteQuad(probe_x, in, dx_out, 0xF, &mx, texels) !=
        TGL_SHADER_EXEC_OK)
        return -1;
    if (tglShaderExecuteQuad(probe_y, in, dy_out, 0xF, &my, texels) !=
        TGL_SHADER_EXEC_OK)
        return -1;

    f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "# quad origin (%d,%d) write_mask 0x%x\n", qx, qy, mask);
    fprintf(f, "lane,pixel_x,pixel_y,covered,r0_u,r0_v,r1_r,r1_g,r1_b,"
               "r1_a,r2_x,r2_y,r2_z,r2_w,ddx_u,ddx_v,ddy_u,ddy_v,"
               "out_r,out_g,out_b,out_a,write_bit\n");
    for (lane = 0; lane < 4; lane++) {
        fprintf(f, "%d,%d,%d,1,%.6f,%.6f,%.4f,%.4f,%.4f,%.4f,"
                   "%.1f,%.1f,%.4f,%.4f,%.6f,%.6f,%.6f,%.6f,"
                   "%.4f,%.4f,%.4f,%.4f,%d\n",
                lane, qx + (lane & 1), qy + (lane >> 1), in[lane][0][0],
                in[lane][0][1], in[lane][1][0], in[lane][1][1],
                in[lane][1][2], in[lane][1][3], in[lane][2][0],
                in[lane][2][1], in[lane][2][2], in[lane][2][3],
                dx_out[lane][0], dx_out[lane][1], dy_out[lane][0],
                dy_out[lane][1], out[lane][0], out[lane][1], out[lane][2],
                out[lane][3], (mask >> lane) & 1);
    }
    fclose(f);
    tglShaderRelease(probe_x);
    tglShaderRelease(probe_y);
    return 0;
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : ".";
    static GLubyte tex_rgb[256 * 256 * 3];
    TGLShaderInstr code[32];
    TGLShaderProgram *prog;
    TGLShaderError err;
    ZBuffer *zb;
    GLuint n, tn = 0;
    GLint xs = 0, ys = 0;
    GLuint *texels;
    char path[1024];
    int rc = 1;

    zb = ZB_open(W, H, ZB_MODE_RGBA, NULL);
    if (!zb) {
        fprintf(stderr, "shader_vm_example: cannot open zbuffer\n");
        return 1;
    }
    glInit(zb);
    glViewport(0, 0, W, H);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_LIGHTING);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);

    /* procedural 256x256 texture */
    build_texture(tex_rgb);
    glGenTextures(1, &tn);
    glBindTexture(GL_TEXTURE_2D, tn);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE,
                 tex_rgb);

    /* material program with loop, branches and derivatives */
    n = build_material(code);
    prog = tglShaderCreate(code, n, &err);
    if (!prog) {
        fprintf(stderr, "shader_vm_example: create failed: code=%d index=%d\n",
                err.code, err.index);
        goto cleanup;
    }
    tglShaderBind(prog);
    tglShaderRelease(prog); /* the binding keeps the program alive */

    /* full-screen quad: UV ramp 0..2, linear color ramps, z = 0.3 */
    glClearColor(0.05f, 0.05f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_QUADS);
    glColor4f(0.1f, 0.1f, 0.5f, 1.0f);
    glTexCoord2f(0, 0);
    glVertex3f(-1, -1, 0.3f);
    glColor4f(0.9f, 0.1f, 0.5f, 1.0f);
    glTexCoord2f(2, 0);
    glVertex3f(1, -1, 0.3f);
    glColor4f(0.9f, 0.9f, 0.5f, 1.0f);
    glTexCoord2f(2, 2);
    glVertex3f(1, 1, 0.3f);
    glColor4f(0.1f, 0.9f, 0.5f, 1.0f);
    glTexCoord2f(0, 2);
    glVertex3f(-1, 1, 0.3f);
    glEnd();
    if (tglShaderGetError() != TGL_SHADER_EXEC_OK) {
        fprintf(stderr, "shader_vm_example: draw error\n");
        goto cleanup;
    }

    snprintf(path, sizeof path, "%s/surface.ppm", outdir);
    if (write_ppm(path, zb) != 0) {
        fprintf(stderr, "shader_vm_example: cannot write %s\n", path);
        goto cleanup;
    }

    /* trace one fully covered quad of the drawn surface */
    texels = (GLuint *) glGetTexturePixmap((GLint) tn, 0, &xs, &ys);
    snprintf(path, sizeof path, "%s/trace.csv", outdir);
    if (!texels || write_trace(path, prog, texels, 64, 64) != 0) {
        fprintf(stderr, "shader_vm_example: cannot write %s\n", path);
        goto cleanup;
    }

    printf("shader_vm_example: wrote %s/surface.ppm and %s/trace.csv\n",
           outdir, outdir);
    rc = 0;

cleanup:
    tglShaderBind(NULL);
    glClose();
    ZB_close(zb);
    return rc;
}
