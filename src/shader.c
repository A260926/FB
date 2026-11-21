/*
 * GL integration for fragment bytecode programs.
 *
 * Implements binding (immediate context state, never recorded into display
 * lists), the runtime error query, and the quad-based program-mode
 * rasterizer for GL_SMOOTH filled triangles. The rasterizer keeps the
 * existing clipping, culling, texture and depth toggles: triangles arrive
 * here already clipped and culled, TEX samples the texture bound at draw
 * time, and the depth test after the shader stage uses the original
 * zz >= zbuf comparison.
 */

#include "zgl.h"

/* ------------------------------------------------------------------ */
/* binding and error query                                              */
/* ------------------------------------------------------------------ */

void tglBindShader(TGLShaderProgram *prog)
{
    GLContext *c = gl_get_context();
    if (c->bound_shader == prog)
        return;
    if (c->bound_shader)
        tglShaderRelease(c->bound_shader);
    c->bound_shader = prog;
    if (prog)
        tglShaderRetain(prog);
}

TGLShaderProgram *tglGetBoundShader(void)
{
    return gl_get_context()->bound_shader;
}

int tglShaderGetError(void)
{
    GLContext *c = gl_get_context();
    int e = c->shader_error;
    c->shader_error = TGL_SHADER_RT_NONE;
    return e;
}

/* the first runtime error sticks until tglShaderGetError() is called */
static void shader_set_err(GLContext *c, int e)
{
    if (c->shader_error == TGL_SHADER_RT_NONE)
        c->shader_error = e;
}

/* called for polygon modes the program path does not support */
void tgl_shader_reject_state(GLContext *c)
{
    shader_set_err(c, TGL_SHADER_RT_STATE);
}

/* ------------------------------------------------------------------ */
/* interpolation planes                                                 */
/* ------------------------------------------------------------------ */

typedef struct FPlane {
    float a0, dx, dy, x0, y0;
} FPlane;

static void plane_setup(FPlane *pl,
                        GLfloat x0,
                        GLfloat y0,
                        GLfloat x1,
                        GLfloat y1,
                        GLfloat x2,
                        GLfloat y2,
                        GLfloat a0,
                        GLfloat a1,
                        GLfloat a2,
                        GLfloat inv_area)
{
    pl->dx = (a0 * (y1 - y2) + a1 * (y2 - y0) + a2 * (y0 - y1)) * inv_area;
    pl->dy = (a0 * (x2 - x1) + a1 * (x0 - x2) + a2 * (x1 - x0)) * inv_area;
    pl->a0 = a0;
    pl->x0 = x0;
    pl->y0 = y0;
}

static GLfloat plane_eval(const FPlane *pl, GLfloat x, GLfloat y)
{
    return pl->a0 + pl->dx * (x - pl->x0) + pl->dy * (y - pl->y0);
}

/* ------------------------------------------------------------------ */
/* texture sampling over the texture bound at draw time                 */
/* ------------------------------------------------------------------ */

static void shader_sample(void *ctx, int tx, int ty, float rgba[4])
{
    const PIXEL *tex = (const PIXEL *) ctx;
    PIXEL p = tex[(unsigned) ty * TGL_FEATURE_TEXTURE_DIM + (unsigned) tx];
    rgba[0] = (GLfloat) GET_RED(p) * (1.0f / 255.0f);
    rgba[1] = (GLfloat) GET_GREEN(p) * (1.0f / 255.0f);
    rgba[2] = (GLfloat) GET_BLUE(p) * (1.0f / 255.0f);
    rgba[3] = 1.0f;
}

static PIXEL shader_pixel(const float rgba[4])
{
    GLint r = (GLint) (clampf(rgba[0], 0.0f, 1.0f) * 255.0f + 0.5f);
    GLint g = (GLint) (clampf(rgba[1], 0.0f, 1.0f) * 255.0f + 0.5f);
    GLint b = (GLint) (clampf(rgba[2], 0.0f, 1.0f) * 255.0f + 0.5f);
#if TGL_FEATURE_RENDER_BITS == 32
    return (PIXEL) ((r << 16) | (g << 8) | b);
#else
    return (PIXEL) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
#endif
}

/* ------------------------------------------------------------------ */
/* program-mode triangle rasterization                                  */
/* ------------------------------------------------------------------ */

/* coverage of one pixel center by the normalized-winding triangle:
 * inside means every edge function is positive, or zero on a top-left
 * edge (half-open rule) */
static GLint shader_covered(const GLfloat edx[3],
                            const GLfloat edy[3],
                            const GLfloat ex0[3],
                            const GLfloat ey0[3],
                            const GLint topleft[3],
                            GLfloat cx,
                            GLfloat cy)
{
    GLint k;
    for (k = 0; k < 3; k++) {
        GLfloat f = edx[k] * (cy - ey0[k]) - edy[k] * (cx - ex0[k]);
        if (f < 0.0f)
            return 0;
        if (f == 0.0f && !topleft[k])
            return 0;
    }
    return 1;
}

void tgl_shader_fill_triangle(GLContext *c,
                              GLVertex *v0,
                              GLVertex *v1,
                              GLVertex *v2)
{
    ZBuffer *zb = c->zb;
    const TGLShaderProgram *prog = c->bound_shader;
    GLVertex *vs[3];
    GLfloat x[3], y[3], zv[3], invw[3], uw[3], vw[3];
    GLfloat cr[3], cg[3], cb[3], ca[3];
    GLfloat nx[3], ny[3];
    GLfloat edx[3], edy[3], ex0[3], ey0[3];
    GLint topleft[3];
    FPlane p_invw, p_uw, p_vw, p_cr, p_cg, p_cb, p_ca, p_z;
    GLfloat area, inv_area, minx, maxx, miny, maxy;
    GLint bx0, bx1, by0, by1, qx0, qy0, qx, qy;
    GLint i, k, o1, o2;
    int uses_tex;
    void *texctx;

    if (!prog)
        return;

    /* program mode supports GL_SMOOTH filled triangles only; blending is
     * rejected for now */
    if (c->current_shade_model != GL_SMOOTH || zb->enable_blend) {
        shader_set_err(c, TGL_SHADER_RT_STATE);
        return;
    }
    uses_tex = tglShaderUsesTexture(prog);
    texctx = NULL;
    if (uses_tex) {
        if (!c->texture_2d_enabled || !c->current_texture) {
            shader_set_err(c, TGL_SHADER_RT_NO_TEXTURE);
            return;
        }
        texctx = c->current_texture->images[0].pixmap;
    }

    vs[0] = v0;
    vs[1] = v1;
    vs[2] = v2;
    for (i = 0; i < 3; i++) {
        GLfloat w = vs[i]->pc.W;
        GLfloat iw, u, v;
        x[i] = (GLfloat) vs[i]->zp.x;
        y[i] = (GLfloat) vs[i]->zp.y;
        zv[i] = (GLfloat) vs[i]->zp.z;
        if (w == 0.0f) {
            shader_set_err(c, TGL_SHADER_RT_ZERO_W);
            return;
        }
        iw = 1.0f / w;
        if (!isfinite(iw)) {
            shader_set_err(c, TGL_SHADER_RT_NONFINITE);
            return;
        }
        invw[i] = iw;
        u = vs[i]->tex_coord.X;
        v = vs[i]->tex_coord.Y;
        uw[i] = u * iw;
        vw[i] = v * iw;
        cr[i] = vs[i]->color.v[0];
        cg[i] = vs[i]->color.v[1];
        cb[i] = vs[i]->color.v[2];
        ca[i] = vs[i]->color.v[3];
    }

    area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    if (area == 0.0f || !isfinite(area))
        return; /* degenerate triangle covers nothing */
    inv_area = 1.0f / area;

    plane_setup(&p_invw, x[0], y[0], x[1], y[1], x[2], y[2], invw[0], invw[1],
                invw[2], inv_area);
    plane_setup(&p_uw, x[0], y[0], x[1], y[1], x[2], y[2], uw[0], uw[1],
                uw[2], inv_area);
    plane_setup(&p_vw, x[0], y[0], x[1], y[1], x[2], y[2], vw[0], vw[1],
                vw[2], inv_area);
    plane_setup(&p_cr, x[0], y[0], x[1], y[1], x[2], y[2], cr[0], cr[1],
                cr[2], inv_area);
    plane_setup(&p_cg, x[0], y[0], x[1], y[1], x[2], y[2], cg[0], cg[1],
                cg[2], inv_area);
    plane_setup(&p_cb, x[0], y[0], x[1], y[1], x[2], y[2], cb[0], cb[1],
                cb[2], inv_area);
    plane_setup(&p_ca, x[0], y[0], x[1], y[1], x[2], y[2], ca[0], ca[1],
                ca[2], inv_area);
    plane_setup(&p_z, x[0], y[0], x[1], y[1], x[2], y[2], zv[0], zv[1],
                zv[2], inv_area);

    /* coverage edges use a normalized (positive area) winding */
    o1 = 1;
    o2 = 2;
    if (area < 0.0f) {
        o1 = 2;
        o2 = 1;
    }
    nx[0] = x[0];
    ny[0] = y[0];
    nx[1] = x[o1];
    ny[1] = y[o1];
    nx[2] = x[o2];
    ny[2] = y[o2];
    for (k = 0; k < 3; k++) {
        GLint k2 = (k + 1) % 3;
        ex0[k] = nx[k];
        ey0[k] = ny[k];
        edx[k] = nx[k2] - nx[k];
        edy[k] = ny[k2] - ny[k];
        topleft[k] = (edy[k] < 0.0f) || (edy[k] == 0.0f && edx[k] > 0.0f);
    }

    /* bounding box of pixel centers that can be covered, clipped to the
     * framebuffer; quads align to even screen coordinates */
    minx = x[0];
    maxx = x[0];
    miny = y[0];
    maxy = y[0];
    for (i = 1; i < 3; i++) {
        if (x[i] < minx)
            minx = x[i];
        if (x[i] > maxx)
            maxx = x[i];
        if (y[i] < miny)
            miny = y[i];
        if (y[i] > maxy)
            maxy = y[i];
    }
    bx0 = (GLint) ceilf(minx - 0.5f);
    bx1 = (GLint) floorf(maxx - 0.5f);
    by0 = (GLint) ceilf(miny - 0.5f);
    by1 = (GLint) floorf(maxy - 0.5f);
    if (bx0 < 0)
        bx0 = 0;
    if (by0 < 0)
        by0 = 0;
    if (bx1 > zb->xsize - 1)
        bx1 = zb->xsize - 1;
    if (by1 > zb->ysize - 1)
        by1 = zb->ysize - 1;
    if (bx0 > bx1 || by0 > by1)
        return;
    qx0 = bx0 & ~1;
    qy0 = by0 & ~1;

    for (qy = qy0; qy <= by1; qy += 2) {
        for (qx = qx0; qx <= bx1; qx += 2) {
            float inq[4][TGL_SHADER_NUM_INPUTS][4];
            float outq[4][4];
            GLushort zz[4];
            unsigned coverage = 0, wmask = 0;
            GLint lane, bad = TGL_SHADER_RT_NONE;

            for (lane = 0; lane < 4; lane++) {
                GLint px = qx + (lane & 1);
                GLint py = qy + (lane >> 1);
                GLfloat cx = (GLfloat) px + 0.5f;
                GLfloat cy = (GLfloat) py + 0.5f;
                GLfloat iw, u, v, zf, zzf;
                GLfloat r, g, b, a;

                /* helper lanes (not covered by this clipped triangle) get
                 * their inputs extrapolated from this triangle's own
                 * attribute planes, never copied from a neighbour */
                if (px < zb->xsize && py < zb->ysize &&
                    shader_covered(edx, edy, ex0, ey0, topleft, cx, cy))
                    coverage |= (unsigned) (1u << lane);

                iw = plane_eval(&p_invw, cx, cy);
                if (iw == 0.0f) {
                    bad = TGL_SHADER_RT_ZERO_W;
                    break;
                }
                u = plane_eval(&p_uw, cx, cy) / iw;
                v = plane_eval(&p_vw, cx, cy) / iw;
                r = plane_eval(&p_cr, cx, cy);
                g = plane_eval(&p_cg, cx, cy);
                b = plane_eval(&p_cb, cx, cy);
                a = plane_eval(&p_ca, cx, cy);
                zf = plane_eval(&p_z, cx, cy);
                if (!isfinite(u) || !isfinite(v) || !isfinite(r) ||
                    !isfinite(g) || !isfinite(b) || !isfinite(a) ||
                    !isfinite(zf) || !isfinite(iw)) {
                    bad = TGL_SHADER_RT_NONFINITE;
                    break;
                }
                zzf = zf * (1.0f / 16384.0f);
                if (zzf < 0.0f)
                    zzf = 0.0f;
                if (zzf > 65535.0f)
                    zzf = 65535.0f;
                zz[lane] = (GLushort) zzf;

                inq[lane][0][0] = u;
                inq[lane][0][1] = v;
                inq[lane][0][2] = 0.0f;
                inq[lane][0][3] = 1.0f;
                inq[lane][1][0] = r;
                inq[lane][1][1] = g;
                inq[lane][1][2] = b;
                inq[lane][1][3] = a;
                inq[lane][2][0] = cx;
                inq[lane][2][1] = cy;
                inq[lane][2][2] = (GLfloat) zz[lane] * (1.0f / 65535.0f);
                inq[lane][2][3] = iw;
            }
            if (bad != TGL_SHADER_RT_NONE) {
                /* the whole quad is dropped, nothing is committed */
                shader_set_err(c, bad);
                continue;
            }
            if (!coverage)
                continue; /* no real lane in this quad: do not execute */

            bad = tglShaderExecQuad(prog, inq, outq, coverage, &wmask,
                                    uses_tex ? shader_sample : NULL, texctx);
            if (bad != TGL_SHADER_RT_NONE) {
                shader_set_err(c, bad);
                continue;
            }

            /* depth test after the shader stage; only lanes inside the
             * coverage, not discarded and passing the original depth
             * comparison may write color or depth */
            for (lane = 0; lane < 4; lane++) {
                GLint px, py, off;
                if (!(wmask & (1u << lane)))
                    continue;
                px = qx + (lane & 1);
                py = qy + (lane >> 1);
                off = py * zb->xsize + px;
                if (!zb->depth_test || zz[lane] >= zb->zbuf[off]) {
                    zb->pbuf[off] = shader_pixel(outq[lane]);
                    if (zb->depth_write)
                        zb->zbuf[off] = zz[lane];
                }
            }
        }
    }
}
