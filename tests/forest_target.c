/* forest_target - --preserve-nodes (real glTF node chain) coverage.
 *
 * Generates glTF fixtures in C (base64 data URIs), converts them with the library entry
 * point, and checks:
 *   import   - the default fold mode still rejects a shear-producing non-uniform ancestor,
 *              preserve mode accepts it and emits exactly the ancestor closure; the shared
 *              forest ordering API (mrw_g2m_forest_order_build) exposes both index maps and
 *              the parent table; disconnected real trees stay a forest.
 *   hierarchy- rest locals are the nodes' own TRS (nothing folded), sampled frames reproduce
 *              independently multiplied per-node model matrices, ancestor animation drives the
 *              result, inverse-bind pairs by skin slot, and weighted vertices skin correctly.
 *   batch    - mrw_local_to_model / mrw_clip_to_palette / batch sample / blend / accumulate,
 *              f32 and f16, nine instances, both codecs, every available backend (scalar must
 *              run; unavailable ones are reported).
 *
 * The file registers cases only; task_support/driver.c owns main(). */
#include "task_support/check.h"
#include "marrow.h"
#include "convert.h"
#include "joint_order.h"
#include "mrw_authoring.h"
#include "cgltf.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FX_FRAMES 31
#define FX_FPS    30.0f
#define FX_MAXN   16
#define FX_MAXJ   8

/* ------------------------------------------------------------------ fixture description */

typedef struct {
    const char *name;
    int         parent;
    double      t[3], q[4], s[3];
    int         rot_on[2];                 /* per animation */
    double      rot_axis[3], rot_base, rot_amp[2];
    int         tr_on[2];
    double      tr_dir[3], tr_amp[2];
} fx_node;

typedef struct {
    fx_node nodes[FX_MAXN];
    int     node_count;
    int     joints[FX_MAXJ];
    int     joint_count;
    int     anim_count;
    const char *anim_names[2];
} fixture;

static void node_defaults(fx_node *n, const char *name, int parent) {
    memset(n, 0, sizeof *n);
    n->name = name;
    n->parent = parent;
    n->q[3] = 1.0;
    n->s[0] = n->s[1] = n->s[2] = 1.0;
    n->rot_axis[1] = 1.0;
    n->tr_dir[0] = 1.0;
}

/* ------------------------------------------------------------------ double 3x4 affine math
 * Independent of the library: the oracle composes node locals with plain double arithmetic so
 * a bug in the runtime's matrix composition cannot hide inside the reference. */

typedef struct { double m[12]; } aff; /* row-major [R|t], column vectors */

static aff aff_ident(void) {
    aff a = {{1,0,0,0, 0,1,0,0, 0,0,1,0}};
    return a;
}
static aff aff_mul(aff A, aff B) {
    aff C;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            C.m[r*4+c] = A.m[r*4+0]*B.m[0*4+c] + A.m[r*4+1]*B.m[1*4+c] + A.m[r*4+2]*B.m[2*4+c];
        C.m[r*4+3] = A.m[r*4+0]*B.m[3] + A.m[r*4+1]*B.m[7] + A.m[r*4+2]*B.m[11] + A.m[r*4+3];
    }
    return C;
}
static aff aff_trs(const double q_in[4], const double t[3], const double s[3]) {
    double q[4] = { q_in[0], q_in[1], q_in[2], q_in[3] };
    double n = sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n > 0) for (int i = 0; i < 4; ++i) q[i] /= n;
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double R[3][3] = {
        { 1 - 2*(y*y + z*z),   2*(x*y - z*w),     2*(x*z + y*w) },
        { 2*(x*y + z*w),       1 - 2*(x*x + z*z), 2*(y*z - x*w) },
        { 2*(x*z - y*w),       2*(y*z + x*w),     1 - 2*(x*x + y*y) }
    };
    aff a;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) a.m[r*4+c] = R[r][c] * s[c];
        a.m[r*4+3] = t[r];
    }
    return a;
}
static int aff_inv(aff A, aff *out) {
    const double *m = A.m;
    double a = m[0], b = m[1], c = m[2];
    double d = m[4], e = m[5], f = m[6];
    double g = m[8], h = m[9], i = m[10];
    double det = a*(e*i - f*h) - b*(d*i - f*g) + c*(d*h - e*g);
    if (fabs(det) < 1e-14) return 0;
    double inv = 1.0 / det;
    double R[9];
    R[0] =  (e*i - f*h) * inv; R[1] = -(b*i - c*h) * inv; R[2] =  (b*f - c*e) * inv;
    R[3] = -(d*i - f*g) * inv; R[4] =  (a*i - c*g) * inv; R[5] = -(a*f - c*d) * inv;
    R[6] =  (d*h - e*g) * inv; R[7] = -(a*h - b*g) * inv; R[8] =  (a*e - b*d) * inv;
    double t[3] = { m[3], m[7], m[11] };
    for (int r = 0; r < 3; ++r) {
        out->m[r*4+0] = R[r*3+0];
        out->m[r*4+1] = R[r*3+1];
        out->m[r*4+2] = R[r*3+2];
        out->m[r*4+3] = -(R[r*3+0]*t[0] + R[r*3+1]*t[1] + R[r*3+2]*t[2]);
    }
    return 1;
}
static void aff_to_colmajor(const aff *A, float out16[16]) {
    memset(out16, 0, 16 * sizeof(float));
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out16[c*4 + r] = (float)A->m[r*4 + c];
        out16[12 + r] = (float)A->m[r*4 + 3];
    }
    out16[15] = 1.0f;
}
static void aff_xform_point(const aff *A, const double p[3], double out[3]) {
    for (int r = 0; r < 3; ++r)
        out[r] = A->m[r*4+0]*p[0] + A->m[r*4+1]*p[1] + A->m[r*4+2]*p[2] + A->m[r*4+3];
}

static void quat_axis_angle(const double axis[3], double angle, double q[4]) {
    double ax = axis[0], ay = axis[1], az = axis[2];
    double n = sqrt(ax*ax + ay*ay + az*az);
    if (n > 0) { ax /= n; ay /= n; az /= n; }
    double s = sin(angle * 0.5);
    q[0] = ax*s; q[1] = ay*s; q[2] = az*s; q[3] = cos(angle * 0.5);
}

/* node's local TRS at one resampled frame (anim < 0 = rest/default pose). */
static aff fx_local_d(const fixture *fx, int node, int anim, int frame) {
    const fx_node *n = &fx->nodes[node];
    double q[4] = { n->q[0], n->q[1], n->q[2], n->q[3] };
    double t[3] = { n->t[0], n->t[1], n->t[2] };
    double u = (double)frame / (double)(FX_FRAMES - 1);
    if (anim >= 0) {
        if (n->rot_on[anim])
            quat_axis_angle(n->rot_axis, n->rot_base + n->rot_amp[anim] * u, q);
        if (n->tr_on[anim])
            for (int k = 0; k < 3; ++k) t[k] += n->tr_dir[k] * n->tr_amp[anim] * u;
    }
    return aff_trs(q, t, n->s);
}

/* ------------------------------------------------------------------ raw buffer + JSON writer */

typedef struct { unsigned char *p; size_t n, cap; } rawbuf;

static int rb_init(rawbuf *r, size_t cap) { r->p = (unsigned char *)malloc(cap); r->n = 0; r->cap = cap; return r->p ? 0 : -1; }
static size_t rb_add(rawbuf *r, const void *v, size_t n) {
    size_t off = r->n;
    if (n > r->cap - off) return (size_t)-1;
    memcpy(r->p + off, v, n);
    r->n = off + n;
    return off;
}
static size_t rb_floats(rawbuf *r, const float *v, size_t count) {
    while (r->n & 3u) { if (r->n >= r->cap) return (size_t)-1; r->p[r->n++] = 0; }
    return rb_add(r, v, count * sizeof(float));
}

typedef struct { char *p; size_t n, cap; } jb;
static void jb_init(jb *b, size_t cap) { b->p = (char *)malloc(cap); b->n = 0; b->cap = cap; if (b->p) b->p[0] = 0; }
static void jb_printf(jb *b, const char *fmt, ...) {
    if (!b->p || b->n + 1 >= b->cap) return;
    size_t rem = b->cap - b->n;
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(b->p + b->n, rem, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    b->n += ((size_t)w < rem) ? (size_t)w : rem - 1;
}

static int add_acc(jb *accs, int *count, size_t off, size_t cnt, const char *type,
                   int has_mm, double mn, double mx) {
    int idx = *count;
    if (has_mm)
        jb_printf(accs, "%s{\"bufferView\":0,\"byteOffset\":%zu,\"componentType\":5126,"
                       "\"count\":%zu,\"type\":\"%s\",\"min\":[%.9g],\"max\":[%.9g]}",
                  idx ? "," : "", off, cnt, type, mn, mx);
    else
        jb_printf(accs, "%s{\"bufferView\":0,\"byteOffset\":%zu,\"componentType\":5126,"
                       "\"count\":%zu,\"type\":\"%s\"}",
                  idx ? "," : "", off, cnt, type);
    *count = idx + 1;
    return idx;
}

static const char B64C[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char *base64_alloc(const unsigned char *in, size_t n) {
    size_t olen = 4 * ((n + 2) / 3);
    char *out = (char *)malloc(olen + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t b0 = in[i];
        uint32_t b1 = (i + 1 < n) ? in[i+1] : 0;
        uint32_t b2 = (i + 2 < n) ? in[i+2] : 0;
        uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out[o++] = B64C[(v >> 18) & 63];
        out[o++] = B64C[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64C[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64C[v & 63] : '=';
    }
    out[o] = 0;
    return out;
}

/* bind model of every node from the default pose (parents precede in our fixtures); used only to
 * author non-identity inverse-bind matrices that invert the bind pose exactly. */
static void fx_bind_models(const fixture *fx, aff out[FX_MAXN]) {
    int done[FX_MAXN] = {0};
    for (int pass = 0; pass < fx->node_count; ++pass) {
        for (int i = 0; i < fx->node_count; ++i) {
            if (done[i]) continue;
            int p = fx->nodes[i].parent;
            if (p >= 0 && !done[p]) continue;
            aff l = aff_trs(fx->nodes[i].q, fx->nodes[i].t, fx->nodes[i].s);
            out[i] = (p < 0) ? l : aff_mul(out[p], l);
            done[i] = 1;
        }
    }
}

static int write_fixture(const char *path, const fixture *fx) {
    rawbuf raw;
    if (rb_init(&raw, 1u << 20)) return 1;

    aff bindm[FX_MAXN];
    fx_bind_models(fx, bindm);

    /* inverse-bind matrices, one per ORIGINAL skin slot (slot order = fx->joints[]) */
    float ibm[FX_MAXJ * 16];
    for (int s = 0; s < fx->joint_count; ++s) {
        aff inv;
        if (!aff_inv(bindm[fx->joints[s]], &inv)) { free(raw.p); return 1; }
        float m16[16];
        aff_to_colmajor(&inv, m16);
        memcpy(ibm + s * 16, m16, sizeof m16);
    }
    size_t ibm_off = rb_floats(&raw, ibm, (size_t)fx->joint_count * 16);

    float times[FX_FRAMES];
    for (int i = 0; i < FX_FRAMES; ++i) times[i] = (float)i / (float)(FX_FRAMES - 1);
    size_t times_off = rb_floats(&raw, times, FX_FRAMES);

    jb accs; jb_init(&accs, 1u << 17);
    int acc_count = 0;
    int ibm_acc   = add_acc(&accs, &acc_count, ibm_off, (size_t)fx->joint_count, "MAT4", 0, 0, 0);
    int times_acc = add_acc(&accs, &acc_count, times_off, FX_FRAMES, "SCALAR", 1, 0.0, 1.0);

    /* channel tables, one per animation */
    enum { FX_MAXCH = FX_MAXN * 2 };
    int ch_node[2][FX_MAXCH];
    const char *ch_path[2][FX_MAXCH];
    int ch_acc[2][FX_MAXCH];
    int ch_count[2] = {0, 0};

    for (int a = 0; a < fx->anim_count; ++a) {
        for (int i = 0; i < fx->node_count; ++i) {
            const fx_node *n = &fx->nodes[i];
            if (n->rot_on[a]) {
                float vals[FX_FRAMES * 4];
                for (int f = 0; f < FX_FRAMES; ++f) {
                    double q[4];
                    double u = (double)f / (double)(FX_FRAMES - 1);
                    quat_axis_angle(n->rot_axis, n->rot_base + n->rot_amp[a] * u, q);
                    for (int k = 0; k < 4; ++k) vals[f*4+k] = (float)q[k];
                }
                size_t off = rb_floats(&raw, vals, (size_t)FX_FRAMES * 4);
                int ai = add_acc(&accs, &acc_count, off, FX_FRAMES, "VEC4", 0, 0, 0);
                ch_node[a][ch_count[a]] = i; ch_path[a][ch_count[a]] = "rotation"; ch_acc[a][ch_count[a]] = ai;
                ++ch_count[a];
            }
            if (n->tr_on[a]) {
                float vals[FX_FRAMES * 3];
                for (int f = 0; f < FX_FRAMES; ++f) {
                    double u = (double)f / (double)(FX_FRAMES - 1);
                    for (int k = 0; k < 3; ++k)
                        vals[f*3+k] = (float)(n->t[k] + n->tr_dir[k] * n->tr_amp[a] * u);
                }
                size_t off = rb_floats(&raw, vals, (size_t)FX_FRAMES * 3);
                int ai = add_acc(&accs, &acc_count, off, FX_FRAMES, "VEC3", 0, 0, 0);
                ch_node[a][ch_count[a]] = i; ch_path[a][ch_count[a]] = "translation"; ch_acc[a][ch_count[a]] = ai;
                ++ch_count[a];
            }
        }
    }

    /* nodes */
    jb nodes; jb_init(&nodes, 1u << 15);
    for (int i = 0; i < fx->node_count; ++i) {
        const fx_node *n = &fx->nodes[i];
        jb_printf(&nodes, "%s{\"name\":\"%s\",\"translation\":[%.9g,%.9g,%.9g],"
                          "\"rotation\":[%.9g,%.9g,%.9g,%.9g],\"scale\":[%.9g,%.9g,%.9g]",
                  i ? "," : "", n->name, n->t[0], n->t[1], n->t[2],
                  n->q[0], n->q[1], n->q[2], n->q[3], n->s[0], n->s[1], n->s[2]);
        int first = 1;
        for (int c = 0; c < fx->node_count; ++c) {
            if (fx->nodes[c].parent != i) continue;
            jb_printf(&nodes, "%s%d", first ? ",\"children\":[" : ",", c);
            first = 0;
        }
        if (!first) jb_printf(&nodes, "]");
        jb_printf(&nodes, "}");
    }

    /* animations */
    jb anims; jb_init(&anims, 1u << 15);
    for (int a = 0; a < fx->anim_count; ++a) {
        jb_printf(&anims, "%s{\"name\":\"%s\",\"samplers\":[", a ? "," : "", fx->anim_names[a]);
        for (int c = 0; c < ch_count[a]; ++c)
            jb_printf(&anims, "%s{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"}",
                      c ? "," : "", times_acc, ch_acc[a][c]);
        jb_printf(&anims, "],\"channels\":[");
        for (int c = 0; c < ch_count[a]; ++c)
            jb_printf(&anims, "%s{\"sampler\":%d,\"target\":{\"node\":%d,\"path\":\"%s\"}}",
                      c ? "," : "", c, ch_node[a][c], ch_path[a][c]);
        jb_printf(&anims, "]}");
    }

    /* scene roots */
    jb roots; jb_init(&roots, 1024);
    int rfirst = 1;
    for (int i = 0; i < fx->node_count; ++i) {
        if (fx->nodes[i].parent >= 0) continue;
        jb_printf(&roots, "%s%d", rfirst ? "" : ",", i);
        rfirst = 0;
    }

    jb joints; jb_init(&joints, 256);
    for (int j = 0; j < fx->joint_count; ++j) jb_printf(&joints, "%s%d", j ? "," : "", fx->joints[j]);

    char *b64 = base64_alloc(raw.p, raw.n);
    if (!b64 || !nodes.p || !anims.p || !roots.p || !joints.p || !accs.p) {
        free(raw.p); free(b64); free(nodes.p); free(anims.p); free(roots.p); free(joints.p); free(accs.p);
        return 1;
    }

    jb doc; jb_init(&doc, 1u << 18);
    jb_printf(&doc,
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[%s]}],"
        "\"nodes\":[%s],"
        "\"skins\":[{\"name\":\"forest_rig\",\"joints\":[%s],\"inverseBindMatrices\":%d}],",
        roots.p, nodes.p, joints.p, ibm_acc);
    jb_printf(&doc, "\"animations\":[%s],", anims.p);
    jb_printf(&doc,
        "\"buffers\":[{\"byteLength\":%zu,\"uri\":\"data:application/octet-stream;base64,%s\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%zu}],"
        "\"accessors\":[%s]}",
        raw.n, b64, raw.n, accs.p);

    int fail = (!doc.p || doc.n == 0);
    if (!fail) {
        FILE *f = fopen(path, "wb");
        if (!f) fail = 1;
        else {
            fail = (fwrite(doc.p, 1, doc.n, f) != doc.n);
            if (fclose(f) != 0) fail = 1;
        }
    }
    free(doc.p); free(raw.p); free(b64); free(nodes.p); free(anims.p); free(roots.p); free(joints.p); free(accs.p);
    return fail;
}

/* ------------------------------------------------------------------ fixture definitions */

static void fixture_shear(fixture *fx) {
    memset(fx, 0, sizeof *fx);
    fx->node_count = 7;
    /* 0: single non-joint root with a NON-UNIFORM scale; joints below it rotate -> the folded
     *    local of joint 1 would need a sheared TRS in the default mode. */
    node_defaults(&fx->nodes[0], "Armature", -1);
    fx->nodes[0].t[0] = 1.0; fx->nodes[0].s[0] = 2.0; fx->nodes[0].s[1] = 1.0; fx->nodes[0].s[2] = 0.5;
    fx->nodes[0].tr_on[0] = fx->nodes[0].tr_on[1] = 1;
    fx->nodes[0].tr_dir[0] = 1.0; fx->nodes[0].tr_dir[1] = 0.25; fx->nodes[0].tr_dir[2] = 0.0;
    fx->nodes[0].tr_amp[0] = 0.5; fx->nodes[0].tr_amp[1] = 0.9;
    /* 1: joint J0 (rotation about X) */
    node_defaults(&fx->nodes[1], "J0", 0);
    fx->nodes[1].t[1] = 1.0;
    fx->nodes[1].rot_on[0] = fx->nodes[1].rot_on[1] = 1;
    fx->nodes[1].rot_axis[0] = 1.0; fx->nodes[1].rot_base = 0.2;
    fx->nodes[1].rot_amp[0] = 1.1; fx->nodes[1].rot_amp[1] = 0.6;
    /* 2: non-joint mid, ANOTHER non-uniform scale + rotation about Z (ancestor animation) */
    node_defaults(&fx->nodes[2], "mid", 1);
    fx->nodes[2].t[2] = 2.0; fx->nodes[2].s[0] = 1.0; fx->nodes[2].s[1] = 2.0; fx->nodes[2].s[2] = 0.5;
    fx->nodes[2].rot_on[0] = fx->nodes[2].rot_on[1] = 1;
    fx->nodes[2].rot_axis[2] = 1.0; fx->nodes[2].rot_base = -0.3;
    fx->nodes[2].rot_amp[0] = 0.9; fx->nodes[2].rot_amp[1] = 1.3;
    /* 3: joint J0 - DUPLICATE NAME of node 1, rotation about Y */
    node_defaults(&fx->nodes[3], "J0", 2);
    fx->nodes[3].t[0] = 3.0;
    fx->nodes[3].rot_on[0] = fx->nodes[3].rot_on[1] = 1;
    fx->nodes[3].rot_axis[1] = 1.0; fx->nodes[3].rot_base = 0.1;
    fx->nodes[3].rot_amp[0] = 1.4; fx->nodes[3].rot_amp[1] = 0.8;
    /* 4: unrelated sibling (animated, must NOT reach the output) */
    node_defaults(&fx->nodes[4], "side", 0);
    fx->nodes[4].t[0] = 5.0; fx->nodes[4].t[1] = 5.0; fx->nodes[4].t[2] = 5.0;
    fx->nodes[4].tr_on[0] = fx->nodes[4].tr_on[1] = 1;
    fx->nodes[4].tr_dir[2] = 1.0; fx->nodes[4].tr_amp[0] = 2.0; fx->nodes[4].tr_amp[1] = 1.0;
    /* 5: second related branch under joint 1 (joint J2, rotation about Z) */
    node_defaults(&fx->nodes[5], "J2", 1);
    fx->nodes[5].t[2] = -1.0;
    fx->nodes[5].rot_on[0] = fx->nodes[5].rot_on[1] = 1;
    fx->nodes[5].rot_axis[2] = 1.0; fx->nodes[5].rot_base = 0.4;
    fx->nodes[5].rot_amp[0] = 0.7; fx->nodes[5].rot_amp[1] = 1.6;
    /* 6: unrelated disconnected root */
    node_defaults(&fx->nodes[6], "apart", -1);
    fx->nodes[6].t[0] = 9.0; fx->nodes[6].t[1] = 9.0; fx->nodes[6].t[2] = 9.0;

    /* SHUFFLED skin slots (vertex JOINTS_0 space) */
    fx->joints[0] = 3; fx->joints[1] = 5; fx->joints[2] = 1;
    fx->joint_count = 3;
    fx->anim_count = 2;
    fx->anim_names[0] = "spinA"; fx->anim_names[1] = "spinB";
}

static void fixture_forest(fixture *fx) {
    memset(fx, 0, sizeof *fx);
    fx->node_count = 6;
    /* tree A: 0 -> 1(non-joint) -> 2 ; tree B: 3 -> 4 ; 5 unrelated root. All unit scale so the
     * auto codec is codec 1. */
    node_defaults(&fx->nodes[0], "A", -1);
    fx->nodes[0].t[0] = 1.0;
    fx->nodes[0].rot_on[0] = fx->nodes[0].rot_on[1] = 1;
    fx->nodes[0].rot_axis[0] = 1.0; fx->nodes[0].rot_base = 0.1;
    fx->nodes[0].rot_amp[0] = 1.2; fx->nodes[0].rot_amp[1] = 0.8;
    node_defaults(&fx->nodes[1], "midA", 0);
    fx->nodes[1].t[1] = 2.0;
    fx->nodes[1].rot_on[0] = fx->nodes[1].rot_on[1] = 1;
    fx->nodes[1].rot_axis[1] = 1.0; fx->nodes[1].rot_base = 0.2;
    fx->nodes[1].rot_amp[0] = 0.5; fx->nodes[1].rot_amp[1] = 1.1;
    node_defaults(&fx->nodes[2], "A", 1);
    fx->nodes[2].t[2] = 1.0;
    fx->nodes[2].rot_on[0] = fx->nodes[2].rot_on[1] = 1;
    fx->nodes[2].rot_axis[2] = 1.0; fx->nodes[2].rot_base = 0.0;
    fx->nodes[2].rot_amp[0] = 0.9; fx->nodes[2].rot_amp[1] = 0.4;
    node_defaults(&fx->nodes[3], "B", -1);
    fx->nodes[3].t[0] = -2.0;
    fx->nodes[3].rot_on[0] = fx->nodes[3].rot_on[1] = 1;
    fx->nodes[3].rot_axis[1] = 1.0; fx->nodes[3].rot_base = 0.3;
    fx->nodes[3].rot_amp[0] = 1.5; fx->nodes[3].rot_amp[1] = 0.7;
    node_defaults(&fx->nodes[4], "B", 3);
    fx->nodes[4].t[1] = 1.0;
    fx->nodes[4].rot_on[0] = fx->nodes[4].rot_on[1] = 1;
    fx->nodes[4].rot_axis[0] = 1.0; fx->nodes[4].rot_base = 0.0;
    fx->nodes[4].rot_amp[0] = 0.6; fx->nodes[4].rot_amp[1] = 1.3;
    node_defaults(&fx->nodes[5], "sideB", -1);
    fx->nodes[5].t[2] = 3.0;
    fx->nodes[5].tr_on[0] = fx->nodes[5].tr_on[1] = 1;
    fx->nodes[5].tr_dir[1] = 1.0; fx->nodes[5].tr_amp[0] = 1.0; fx->nodes[5].tr_amp[1] = 1.5;

    fx->joints[0] = 2; fx->joints[1] = 4; fx->joints[2] = 0; fx->joints[3] = 3;
    fx->joint_count = 4;
    fx->anim_count = 2;
    fx->anim_names[0] = "spinA"; fx->anim_names[1] = "spinB";
}

/* single-root regression fixture (hand-written, mirrors the production baseline) */
static int write_single_tree(const char *path) {
    const char *json =
        "{\"asset\":{\"version\":\"2.0\"},\"nodes\":["
        "{\"name\":\"A\",\"translation\":[2,0,0],\"children\":[1]},"
        "{\"name\":\"B\",\"translation\":[0,3,0]}],"
        "\"skins\":[{\"joints\":[1,0]}]}";
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    size_t n = strlen(json);
    int fail = (fwrite(json, 1, n, f) != n);
    if (fclose(f) != 0) fail = 1;
    return fail;
}

/* ------------------------------------------------------------------ conversion helpers */

typedef struct {
    uint8_t *buf;
    size_t size;
    mrw_blob blob;
    mrw_skeleton_view skel;
    mrw_g2m_forest_order forest;
    int have_forest;
    cgltf_data *data;
    uint32_t clip_count;
} converted;

static int convert_fixture(const fixture *fx, const char *path, int preserve, int force_codec0,
                           converted *out, char diag[256]) {
    memset(out, 0, sizeof *out);
    if (write_fixture(path, fx)) return 1;
    mrw_g2m_options opt = {0};
    opt.input_path = path;
    opt.fps = FX_FPS;
    opt.skin_index = -1;
    opt.preserve_nodes = preserve;
    opt.force_codec0 = force_codec0;
    diag[0] = 0;
    mrw_result rc = mrw_g2m_convert(&opt, &out->buf, &out->size, diag, 256);
    if (rc != MRW_OK) return (int)rc + 1;
    if (mrw_blob_open(out->buf, (uint64_t)out->size, &out->blob) != MRW_OK) return 1001;
    if (mrw_blob_skeleton(&out->blob, &out->skel) != MRW_OK) return 1002;
    for (uint32_t i = 0; i < out->blob.section_count; ++i) {
        uint32_t ty = 0;
        if (mrw_blob_section_type(&out->blob, i, &ty) == MRW_OK && ty == MRW_SECTION_CLIP) ++out->clip_count;
    }
    return 0;
}

static void converted_free(converted *c) {
    if (c->buf) mrw_authoring_free(c->buf);
    mrw_g2m_forest_order_free(&c->forest);
    if (c->data) cgltf_free(c->data);
    memset(c, 0, sizeof *c);
}

static int load_forest(converted *c, const char *path) {
    cgltf_options co = {0};
    if (cgltf_parse_file(&co, path, &c->data) != cgltf_result_success) return 1;
    if (cgltf_load_buffers(&co, c->data, path) != cgltf_result_success) return 1;
    if (cgltf_validate(c->data) != cgltf_result_success) return 1;
    if (c->data->skins_count != 1) return 1;
    char diag[256] = {0};
    if (mrw_g2m_forest_order_build(&c->data->skins[0], c->data->nodes, c->data->nodes_count,
                                   &c->forest, diag, sizeof diag) != MRW_OK) return 1;
    c->have_forest = 1;
    return 0;
}

static mrw_result clip_view_n(converted *c, uint32_t n, mrw_clip_view *cv) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < c->blob.section_count; ++i) {
        uint32_t ty = 0;
        if (mrw_blob_section_type(&c->blob, i, &ty) != MRW_OK || ty != MRW_SECTION_CLIP) continue;
        if (k == n) return mrw_clip_view_at(&c->blob, i, cv);
        ++k;
    }
    return MRW_E_RANGE;
}

/* ------------------------------------------------------------------ backends */

typedef struct { mrw_dispatch d; const char *name; int simd; } backend_t;

/* scalar is mandatory; every other implemented backend that the host supports is exercised.
 * Unavailable backends are reported explicitly rather than silently skipped. */
static int host_backends(backend_t *b, int cap) {
    int n = 0;
    mrw_dispatch t;
    if (mrw_dispatch_scalar(&t) != MRW_OK) return 0;
    if (n < cap) b[n++] = (backend_t){ t, "scalar", 0 };
    if (mrw_dispatch_sse2(&t) == MRW_OK) { if (n < cap) b[n++] = (backend_t){ t, "sse2", 1 }; }
    else printf("NOTE: backend sse2 unavailable on this host (scalar still exercised)\n");
    if (mrw_dispatch_avx2(&t) == MRW_OK) {
        if (n < cap) b[n++] = (backend_t){ t, "avx2", 1 };
        if (t.features & MRW_FEAT_FMA) {
            mrw_dispatch nf = t; nf.features &= ~(uint32_t)MRW_FEAT_FMA;
            if (n < cap) b[n++] = (backend_t){ nf, "avx2-nofma", 1 };
        }
        if (t.features & MRW_FEAT_F16C) {
            mrw_dispatch n16 = t; n16.features &= ~(uint32_t)MRW_FEAT_F16C;
            if (n < cap) b[n++] = (backend_t){ n16, "avx2-nof16c", 1 };
        }
    } else printf("NOTE: backend avx2 unavailable on this host (scalar still exercised)\n");
    return n;
}

/* ------------------------------------------------------------------ oracle helpers */

/* oracle model matrices in the preserve ORDER, built as an independent per-node product. */
static void oracle_models(const fixture *fx, const mrw_g2m_forest_order *fo, int anim, int frame,
                          aff out[FX_MAXN]) {
    for (uint32_t o = 0; o < fo->joint_count; ++o) {
        uint32_t node = fo->node_index[o];
        aff l = fx_local_d(fx, (int)node, anim, frame);
        out[o] = (fo->parent[o] < 0) ? l : aff_mul(out[fo->parent[o]], l);
    }
}

static double dabs(double v) { return v < 0 ? -v : v; }

static int aff_near12(const float *a, const aff *b, double tol) {
    for (int k = 0; k < 12; ++k)
        if (dabs((double)a[k] - b->m[k]) > tol) return 0;
    return 1;
}

static int pal_entry_near(const float *pal, const aff *want, double tol) {
    for (int k = 0; k < 12; ++k)
        if (dabs((double)pal[k] - want->m[k]) > tol) return 0;
    return 1;
}

/* a weighted, two-influence vertex; JOINTS_0 uses ORIGINAL skin-slot indices. */
typedef struct { int slot[4]; double w[4]; double pos[3]; } vertex;

static void skin_vertex(const float *palette, uint32_t *slot_to_out, uint32_t slots,
                        const vertex *v, double out[3]) {
    out[0] = out[1] = out[2] = 0;
    for (int k = 0; k < 4; ++k) {
        if (v->w[k] == 0.0) continue;
        if (v->slot[k] < 0 || (uint32_t)v->slot[k] >= slots) continue;
        uint32_t o = slot_to_out[v->slot[k]];
        const float *m = palette + (size_t)o * 12;
        double p[3];
        for (int r = 0; r < 3; ++r)
            p[r] = m[r*4+0]*v->pos[0] + m[r*4+1]*v->pos[1] + m[r*4+2]*v->pos[2] + m[r*4+3];
        for (int r = 0; r < 3; ++r) out[r] += v->w[k] * p[r];
    }
}

/* ================================================================== import group */

static int case_import_shear(void) {
    fixture fx; fixture_shear(&fx);
    const char *path = ".task-build/forest_shear.gltf";
    char diag[256];

    /* default mode: the non-uniform ancestors + rotating joints fold to a sheared TRS */
    {
        converted c; int r = convert_fixture(&fx, path, 0, 0, &c, diag);
        TASK_CHECK(r != 0);                              /* rejected */
        TASK_CHECK(strstr(diag, "shear") != NULL);       /* specifically the shear diagnostic */
        if (c.buf) converted_free(&c);
    }

    /* preserve mode: accepted; every output joint is a real node from the ancestor closure */
    converted c; int r = convert_fixture(&fx, path, 1, 0, &c, diag);
    TASK_CHECK(r == 0);
    TASK_CHECK(c.skel.joint_count == 5);                 /* {0,1,2,3,5} */
    TASK_CHECK(c.clip_count == 2);
    TASK_CHECK(load_forest(&c, path) == 0);
    const mrw_g2m_forest_order *fo = &c.forest;
    TASK_CHECK(fo->joint_count == 5);
    TASK_CHECK(fo->root_count == 1 && fo->roots[0] == 0);
    /* exact ancestor closure in the specified order (roots by node index; DFS, children ascending) */
    const uint32_t want_nodes[5] = {0, 1, 2, 3, 5};
    for (uint32_t i = 0; i < 5; ++i) TASK_CHECK(fo->node_index[i] == want_nodes[i]);
    TASK_CHECK(fo->out_of_node[4] == -1);               /* unrelated sibling absent */
    TASK_CHECK(fo->out_of_node[6] == -1);               /* unrelated root absent */
    /* ORIGINAL skin slot -> output index (skin.joints = [3,5,1]) */
    TASK_CHECK(fo->slot_to_out[0] == 3);
    TASK_CHECK(fo->slot_to_out[1] == 4);
    TASK_CHECK(fo->slot_to_out[2] == 1);
    /* parent table: forest marker 0xFFFF, otherwise a strictly smaller index */
    {
        /* output order {0,1,2,3,5}: node 5 (J2) is the second child of node 1 */
        static const uint16_t want_parent[5] = {0xFFFFu, 0, 1, 2, 1};
        for (uint32_t i = 0; i < 5; ++i) {
            uint16_t p = 0;
            TASK_CHECK(mrw_skeleton_parent(&c.skel, i, &p) == MRW_OK);
            TASK_CHECK(p == want_parent[i]);
        }
        TASK_CHECK(fo->parent[0] == -1 && fo->parent[1] == 0 && fo->parent[2] == 1 &&
                   fo->parent[3] == 2 && fo->parent[4] == 1);
    }
    /* duplicate names are preserved, and both entries exist */
    {
        const char *n1 = NULL, *n3 = NULL;
        TASK_CHECK(mrw_skeleton_joint_name(&c.skel, 1, &n1) == MRW_OK);
        TASK_CHECK(mrw_skeleton_joint_name(&c.skel, 3, &n3) == MRW_OK);
        TASK_CHECK(n1 && n3 && strcmp(n1, "J0") == 0 && strcmp(n3, "J0") == 0);
        const char *nm = NULL;
        TASK_CHECK(mrw_skeleton_joint_name(&c.skel, 0, &nm) == MRW_OK && strcmp(nm, "Armature") == 0);
    }
    /* file version unchanged (v0) */
    TASK_CHECK(c.buf[0] == 'M' && c.buf[1] == 'R' && c.buf[2] == 'R' && c.buf[3] == 'W');
    {
        uint32_t ver; memcpy(&ver, c.buf + 4, 4);
        TASK_CHECK(ver == 0);
    }

    /* loader validation still rejects malformed parent tables: self, forward, out-of-range */
    {
        size_t hdr = (size_t)(c.skel.base - c.buf);
        uint8_t *copy = (uint8_t *)mrw_authoring_alloc(c.size);
        TASK_CHECK(copy != NULL);
        static const uint16_t bad[3] = { 1, 3, 0xFFFEu }; /* at joint 1: self, forward, >=count */
        for (int b = 0; b < 3; ++b) {
            memcpy(copy, c.buf, c.size);
            uint8_t *p = copy + hdr + c.skel.parent_off + 1 * 2;
            memcpy(p, &bad[b], 2);
            mrw_blob tmp;
            TASK_CHECK(mrw_blob_open(copy, (uint64_t)c.size, &tmp) == MRW_E_RANGE);
        }
        mrw_authoring_free(copy);
    }
    converted_free(&c);
    return 0;
}

static int case_import_forest_api(void) {
    fixture fx; fixture_shear(&fx);
    const char *path = ".task-build/forest_api.gltf";
    TASK_CHECK(write_fixture(path, &fx) == 0);

    cgltf_options co = {0};
    cgltf_data *data = NULL;
    TASK_CHECK(cgltf_parse_file(&co, path, &data) == cgltf_result_success);
    TASK_CHECK(cgltf_load_buffers(&co, data, path) == cgltf_result_success);
    TASK_CHECK(cgltf_validate(data) == cgltf_result_success);
    TASK_CHECK(data->skins_count == 1);

    char diag[256] = {0};
    mrw_g2m_forest_order fo = {0};
    TASK_CHECK(mrw_g2m_forest_order_build(&data->skins[0], data->nodes, data->nodes_count,
                                          &fo, diag, sizeof diag) == MRW_OK);
    TASK_CHECK(fo.node_count == (uint32_t)fx.node_count);
    /* both required maps + parent table are present and consistent */
    TASK_CHECK(fo.node_index && fo.parent && fo.slot_to_out && fo.out_of_node);
    for (uint32_t o = 0; o < fo.joint_count; ++o) {
        TASK_CHECK(fo.out_of_node[fo.node_index[o]] == (int32_t)o);
        TASK_CHECK(fo.parent[o] < (int32_t)o);
    }
    for (cgltf_size s = 0; s < data->skins[0].joints_count; ++s) {
        int64_t ni = data->skins[0].joints[s] - data->nodes;
        TASK_CHECK(ni >= 0 && fo.out_of_node[ni] == fo.slot_to_out[s]);
    }
    /* the old single-tree entry point remains available and still classifies this as one tree */
    mrw_g2m_joint_order jo = {0};
    TASK_CHECK(mrw_g2m_joint_order_build(&data->skins[0], &jo, diag, sizeof diag) == MRW_OK);
    TASK_CHECK(jo.joint_count == 3 && jo.root_js >= 0);
    mrw_g2m_joint_order_free(&jo);
    mrw_g2m_forest_order_free(&fo);
    cgltf_free(data);
    return 0;
}

static int case_import_disconnected(void) {
    fixture fx; fixture_forest(&fx);
    const char *path = ".task-build/forest_roots.gltf";
    char diag[256];

    /* default import requires exactly one skeleton root -> rejected */
    {
        converted c; int r = convert_fixture(&fx, path, 0, 0, &c, diag);
        TASK_CHECK(r != 0);
        TASK_CHECK(strstr(diag, "root") != NULL);
        if (c.buf) converted_free(&c);
    }

    converted c; int r = convert_fixture(&fx, path, 1, 0, &c, diag);
    TASK_CHECK(r == 0);
    TASK_CHECK(c.skel.joint_count == 5);

    TASK_CHECK(load_forest(&c, path) == 0);
    const mrw_g2m_forest_order *fo = &c.forest;
    TASK_CHECK(fo->joint_count == 5);
    TASK_CHECK(fo->root_count == 2);
    TASK_CHECK(fo->roots[0] == 0 && fo->roots[1] == 3);
    const uint32_t want_nodes[5] = {0, 1, 2, 3, 4};
    for (uint32_t i = 0; i < 5; ++i) TASK_CHECK(fo->node_index[i] == want_nodes[i]);
    TASK_CHECK(fo->out_of_node[5] == -1);               /* unrelated root excluded */
    {
        static const uint16_t want_parent[5] = {0xFFFFu, 0, 1, 0xFFFFu, 3};
        for (uint32_t i = 0; i < 5; ++i) {
            uint16_t p = 0;
            TASK_CHECK(mrw_skeleton_parent(&c.skel, i, &p) == MRW_OK);
            TASK_CHECK(p == want_parent[i]);
        }
    }
    /* skin slots [2,4,0,3] map into the forest independently of node order */
    TASK_CHECK(fo->slot_to_out[0] == 2);
    TASK_CHECK(fo->slot_to_out[1] == 4);
    TASK_CHECK(fo->slot_to_out[2] == 0);
    TASK_CHECK(fo->slot_to_out[3] == 3);
    converted_free(&c);
    return 0;
}

/* ================================================================== hierarchy group */

static int case_hierarchy_rest(void) {
    fixture fx; fixture_shear(&fx);
    const char *path = ".task-build/forest_rest.gltf";
    char diag[256];
    converted c;
    TASK_CHECK(convert_fixture(&fx, path, 1, 0, &c, diag) == 0);
    TASK_CHECK(load_forest(&c, path) == 0);
    const mrw_g2m_forest_order *fo = &c.forest;

    /* rest locals are each node's OWN TRS - no ancestor fold */
    static const struct { uint32_t o; double t[3]; double s[3]; } want[] = {
        {0, {1,0,0},   {2,1,0.5}},
        {1, {0,1,0},   {1,1,1}},
        {2, {0,0,2},   {1,2,0.5}},
        {3, {3,0,0},   {1,1,1}},
        {4, {0,0,-1},  {1,1,1}},
    };
    mrw_trs locals[5];
    for (int i = 0; i < 5; ++i) {
        TASK_CHECK(mrw_skeleton_rest_local(&c.skel, want[i].o, &locals[want[i].o]) == MRW_OK);
        for (int k = 0; k < 3; ++k) {
            TASK_CHECK(fabs((double)locals[want[i].o].trans[k] - want[i].t[k]) < 1e-6);
            TASK_CHECK(fabs((double)locals[want[i].o].scale[k] - want[i].s[k]) < 1e-6);
        }
    }

    /* inverse bind pairs by ORIGINAL skin slot; auxiliary bones get identity */
    aff bindm[FX_MAXN];
    fx_bind_models(&fx, bindm);
    for (int s = 0; s < fx.joint_count; ++s) {
        aff inv; TASK_CHECK(aff_inv(bindm[fx.joints[s]], &inv));
        float ib[12];
        TASK_CHECK(mrw_skeleton_inverse_bind(&c.skel, (uint32_t)fo->slot_to_out[s], ib) == MRW_OK);
        TASK_CHECK(aff_near12(ib, &inv, 2e-6));
    }
    static const float ident12[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
    /* output 0 (Armature) and 2 (mid) are auxiliary, not skin joints -> identity */
    for (uint32_t o = 0; o < 5; ++o) {
        int is_slot = 0;
        for (int s = 0; s < fx.joint_count; ++s) if (fo->slot_to_out[s] == (int32_t)o) is_slot = 1;
        if (is_slot) continue;
        float ib[12];
        TASK_CHECK(mrw_skeleton_inverse_bind(&c.skel, o, ib) == MRW_OK);
        for (int k = 0; k < 12; ++k) TASK_CHECK(fabsf(ib[k] - ident12[k]) < 1e-7f);
    }
    /* auxiliary bones are absent from every skin.joints slot mapping to a vertex joint */
    for (uint32_t o = 0; o < 5; ++o) {
        int is_slot = 0;
        for (int s = 0; s < fx.joint_count; ++s) if (fo->slot_to_out[s] == (int32_t)o) is_slot = 1;
        TASK_CHECK(is_slot == ((o == 1 || o == 3 || o == 4) ? 1 : 0));
    }

    /* at the bind/rest pose every skin joint's palette entry is identity */
    float model[5 * 12], pal[5 * 12];
    TASK_CHECK(mrw_local_to_model(&c.skel, locals, model, 5) == MRW_OK);
    TASK_CHECK(mrw_model_to_palette(&c.skel, model, pal, 5) == MRW_OK);
    for (int s = 0; s < fx.joint_count; ++s) {
        const float *m = pal + (size_t)fo->slot_to_out[s] * 12;
        for (int k = 0; k < 12; ++k) TASK_CHECK(fabsf(m[k] - ident12[k]) < 2e-5f);
    }

    /* clip-to-palette (the fused single-pose path) matches the separate stages at every sample */
    {
        mrw_clip_view cv;
        TASK_CHECK(clip_view_n(&c, 0, &cv) == MRW_OK);
        for (uint32_t s = 0; s < cv.sample_count; s += 3) {
            float t = (float)s / (float)(cv.sample_count - 1);
            float m1[5 * 12], p1[5 * 12], p2[5 * 12];
            TASK_CHECK(mrw_clip_to_palette(&c.skel, &cv, t, m1, p1, 5) == MRW_OK);
            mrw_trs sl[5];
            TASK_CHECK(mrw_clip_sample_local(&cv, t, sl, 5) == MRW_OK);
            TASK_CHECK(mrw_local_to_model(&c.skel, sl, m1, 5) == MRW_OK);
            TASK_CHECK(mrw_model_to_palette(&c.skel, m1, p2, 5) == MRW_OK);
            for (int k = 0; k < 5 * 12; ++k) TASK_CHECK(fabsf(p1[k] - p2[k]) < 1e-5f);
        }
    }
    converted_free(&c);
    return 0;
}

static int case_hierarchy_frames(void) {
    fixture fx; fixture_shear(&fx);
    const char *path = ".task-build/forest_frames.gltf";
    char diag[256];
    converted c;
    TASK_CHECK(convert_fixture(&fx, path, 1, 0, &c, diag) == 0);
    TASK_CHECK(load_forest(&c, path) == 0);
    const mrw_g2m_forest_order *fo = &c.forest;
    mrw_clip_view cv;
    TASK_CHECK(clip_view_n(&c, 0, &cv) == MRW_OK);
    TASK_CHECK(cv.sample_count == FX_FRAMES);

    aff bindm[FX_MAXN]; fx_bind_models(&fx, bindm);

    /* non-identity IBM + weighted vertex in ORIGINAL skin-slot space */
    vertex vtx;
    vtx.slot[0] = 0; vtx.w[0] = 0.6;   /* slot 0 = node 3 */
    vtx.slot[1] = 2; vtx.w[1] = 0.4;   /* slot 2 = node 1 */
    vtx.pos[0] = 0.5; vtx.pos[1] = -1.0; vtx.pos[2] = 2.0;

    const int frames[] = { 0, 5, 13, 21, 30 };
    double model0_joint1[12] = {0}, modelN_joint1[12] = {0};
    for (size_t fi = 0; fi < sizeof frames / sizeof frames[0]; ++fi) {
        int f = frames[fi];
        aff want[FX_MAXN];
        oracle_models(&fx, fo, 0, f, want);

        float locals[5 * 10], model[5 * 12], pal[5 * 12];
        for (uint32_t o = 0; o < 5; ++o) {
            mrw_trs s;
            TASK_CHECK(mrw_clip_sample(&cv, o, (uint32_t)f, &s) == MRW_OK);
            memcpy(locals + o * 10 + 0, s.rot, 16);
            memcpy(locals + o * 10 + 4, s.trans, 12);
            memcpy(locals + o * 10 + 7, s.scale, 12);
        }
        TASK_CHECK(mrw_local_to_model(&c.skel, (const mrw_trs *)locals, model, 5) == MRW_OK);
        TASK_CHECK(mrw_model_to_palette(&c.skel, model, pal, 5) == MRW_OK);

        /* independent per-node product == runtime model, for every output member */
        for (uint32_t o = 0; o < 5; ++o) {
            TASK_CHECK(aff_near12(model + o * 12, &want[o], 2e-4));
        }
        /* original joints specifically (skin slots -> output) */
        for (int s = 0; s < fx.joint_count; ++s) {
            uint32_t o = (uint32_t)fo->slot_to_out[s];
            /* expected palette = model * inverse(bind) */
            aff ib; TASK_CHECK(aff_inv(bindm[fx.joints[s]], &ib));
            aff pw = aff_mul(want[o], ib);
            TASK_CHECK(pal_entry_near(pal + (size_t)o * 12, &pw, 3e-4));
        }
        /* weighted vertex vs the oracle */
        {
            double expect[3];
            expect[0] = expect[1] = expect[2] = 0;
            for (int k = 0; k < 2; ++k) {
                int slot = vtx.slot[k];
                uint32_t o = (uint32_t)fo->slot_to_out[slot];
                aff ib; TASK_CHECK(aff_inv(bindm[fx.joints[slot]], &ib));
                aff pw = aff_mul(want[o], ib);
                double p[3]; aff_xform_point(&pw, vtx.pos, p);
                for (int r = 0; r < 3; ++r) expect[r] += vtx.w[k] * p[r];
            }
            uint32_t map[FX_MAXJ];
            for (int s = 0; s < fx.joint_count; ++s) map[s] = (uint32_t)fo->slot_to_out[s];
            double got[3];
            skin_vertex(pal, map, (uint32_t)fx.joint_count, &vtx, got);
            for (int r = 0; r < 3; ++r) TASK_CHECK(fabs(got[r] - expect[r]) < 3e-4);
        }
        /* ancestor animation must reach the output: node 0's translation moves joint 1 */
        {
            uint32_t oj = (uint32_t)fo->slot_to_out[2]; /* slot 2 = node 1 */
            for (int k = 0; k < 12; ++k) {
                if (fi == 0) model0_joint1[k] = model[oj * 12 + k];
                else if (fi == sizeof frames / sizeof frames[0] - 1) modelN_joint1[k] = model[oj * 12 + k];
            }
        }
    }
    {
        double d = 0;
        for (int k = 0; k < 12; ++k) d += dabs(modelN_joint1[k] - model0_joint1[k]);
        TASK_CHECK(d > 1e-3); /* the animated ancestor changed the joint's model matrix */
    }
    converted_free(&c);
    return 0;
}

static int case_hierarchy_forest_roots(void) {
    fixture fx; fixture_forest(&fx);
    const char *path = ".task-build/forest_rootcheck.gltf";
    char diag[256];
    converted c;
    TASK_CHECK(convert_fixture(&fx, path, 1, 0, &c, diag) == 0);
    TASK_CHECK(load_forest(&c, path) == 0);
    const mrw_g2m_forest_order *fo = &c.forest;
    mrw_clip_view cv;
    TASK_CHECK(clip_view_n(&c, 0, &cv) == MRW_OK);

    aff want[FX_MAXN];
    oracle_models(&fx, fo, 0, 17, want);
    float locals[5 * 10], model[5 * 12];
    for (uint32_t o = 0; o < 5; ++o) {
        mrw_trs s;
        TASK_CHECK(mrw_clip_sample(&cv, o, 17, &s) == MRW_OK);
        memcpy(locals + o * 10 + 0, s.rot, 16);
        memcpy(locals + o * 10 + 4, s.trans, 12);
        memcpy(locals + o * 10 + 7, s.scale, 12);
    }
    TASK_CHECK(mrw_local_to_model(&c.skel, (const mrw_trs *)locals, model, 5) == MRW_OK);
    for (uint32_t o = 0; o < 5; ++o) TASK_CHECK(aff_near12(model + o * 12, &want[o], 2e-4));

    /* tree B is independent: move root A's local and only tree A responds. */
    float moved[5 * 10];
    memcpy(moved, locals, sizeof moved);
    moved[0 * 10 + 4] += 7.0f;              /* output 0 = root of tree A (translation x) */
    float model2[5 * 12];
    TASK_CHECK(mrw_local_to_model(&c.skel, (const mrw_trs *)moved, model2, 5) == MRW_OK);
    for (uint32_t o = 0; o < 3; ++o) {
        double d = 0;
        for (int k = 0; k < 12; ++k) d += dabs((double)model2[o*12+k] - model[o*12+k]);
        TASK_CHECK(d > 1.0);
    }
    for (uint32_t o = 3; o < 5; ++o) TASK_CHECK(aff_near12(model2 + o * 12, &want[o], 1e-6));

    /* the second real root is at model == local (no synthetic parent) */
    {
        double q[4], t[3], s[3];
        for (int k = 0; k < 4; ++k) q[k] = (double)locals[3 * 10 + k];
        for (int k = 0; k < 3; ++k) { t[k] = (double)locals[3 * 10 + 4 + k]; s[k] = (double)locals[3 * 10 + 7 + k]; }
        aff l = aff_trs(q, t, s);
        TASK_CHECK(aff_near12(model + 3 * 12, &l, 2e-5));
    }
    converted_free(&c);
    return 0;
}

/* ================================================================== batch group */

#define NINST 9

static double f16_tol(double ref) { return 2e-3 + 2e-3 * dabs(ref); }
static double f32_tol(void) { return 1e-4; }

static int check_batch_clip_one(converted *c, uint32_t jc, backend_t *bks, int nbk,
                                const float *times, int N) {
    mrw_clip_view cv;
    TASK_CHECK(clip_view_n(c, 0, &cv) == MRW_OK);
    mrw_mem_req sreq, oreq32, oreq16;
    TASK_CHECK(mrw_batch_clip_to_palette_requirements(jc, (uint32_t)N, MRW_PALETTE_F32, &sreq, &oreq32) == MRW_OK);
    TASK_CHECK(mrw_batch_clip_to_palette_requirements(jc, (uint32_t)N, MRW_PALETTE_F16, NULL, &oreq16) == MRW_OK);

    for (int b = 0; b < nbk; ++b) {
        void *scratch = mrw_authoring_alloc(sreq.size);
        float *out32 = (float *)mrw_authoring_alloc(oreq32.size);
        uint16_t *out16 = (uint16_t *)mrw_authoring_alloc(oreq16.size);
        TASK_CHECK(scratch && out32 && out16);
        TASK_CHECK(mrw_batch_clip_to_palette(&bks[b].d, &c->skel, &cv, times, (uint32_t)N,
                                             out32, oreq32.size, scratch, sreq.size) == MRW_OK);
        TASK_CHECK(mrw_batch_clip_to_palette_f16(&bks[b].d, &c->skel, &cv, times, (uint32_t)N,
                                                 out16, oreq16.size, scratch, sreq.size) == MRW_OK);
        for (int i = 0; i < N; ++i) {
            float mscratch[FX_MAXJ * 12], ref[FX_MAXJ * 12];
            TASK_CHECK(mrw_clip_to_palette(&c->skel, &cv, times[i], mscratch, ref, jc) == MRW_OK);
            for (uint32_t k = 0; k < jc * 12; ++k) {
                double a = (double)out32[(size_t)i * jc * 12 + k];
                TASK_CHECK(dabs(a - (double)ref[k]) <= f32_tol());
                double h = mrw_half_to_float(out16[(size_t)i * jc * 12 + k]);
                TASK_CHECK(dabs(h - (double)ref[k]) <= f16_tol((double)ref[k]));
            }
        }
        mrw_authoring_free(scratch); mrw_authoring_free(out32); mrw_authoring_free(out16);
    }
    return 0;
}

static int case_batch_clip(void) {
    backend_t bks[8];
    int nbk = host_backends(bks, 8);
    TASK_CHECK(nbk >= 1 && strcmp(bks[0].name, "scalar") == 0); /* scalar always runs */
    printf("batch backends:"); for (int i = 0; i < nbk; ++i) printf(" %s", bks[i].name); printf("\n");

    float times[NINST];
    for (int i = 0; i < NINST; ++i) times[i] = (float)(i * 3) / 29.0f; /* spans [0, ~0.83] */

    /* codec 1: unit-scale forest fixture, default codec snap */
    {
        fixture fx; fixture_forest(&fx);
        const char *path = ".task-build/forest_batch1.gltf";
        char diag[256];
        converted c;
        TASK_CHECK(convert_fixture(&fx, path, 1, 0, &c, diag) == 0);
        mrw_clip_view cv;
        TASK_CHECK(clip_view_n(&c, 0, &cv) == MRW_OK);
        TASK_CHECK(cv.codec == 1);
        TASK_CHECK(check_batch_clip_one(&c, c.skel.joint_count, bks, nbk, times, NINST) == 0);
        converted_free(&c);
    }
    /* codec 0: non-unit scales in the shear fixture force raw TRS */
    {
        fixture fx; fixture_shear(&fx);
        const char *path = ".task-build/forest_batch0.gltf";
        char diag[256];
        converted c;
        TASK_CHECK(convert_fixture(&fx, path, 1, 0, &c, diag) == 0);
        mrw_clip_view cv;
        TASK_CHECK(clip_view_n(&c, 0, &cv) == MRW_OK);
        TASK_CHECK(cv.codec == 0);
        TASK_CHECK(check_batch_clip_one(&c, c.skel.joint_count, bks, nbk, times, NINST) == 0);
        converted_free(&c);
    }
    return 0;
}

static int case_batch_codec(void) {
    fixture fx; fixture_forest(&fx);
    const char *path = ".task-build/forest_codec.gltf";

    /* default -> codec 1 (all scales are 1); --codec0 forces raw codec 0 */
    for (int force = 0; force < 2; ++force) {
        char diag[256];
        converted c;
        TASK_CHECK(convert_fixture(&fx, path, 1, force, &c, diag) == 0);
        mrw_clip_view cv;
        TASK_CHECK(clip_view_n(&c, 0, &cv) == MRW_OK);
        TASK_CHECK(cv.codec == (uint32_t)(force ? 0 : 1));
        TASK_CHECK(cv.joint_count == c.skel.joint_count);
        mrw_id128 sid;
        TASK_CHECK(mrw_skeleton_view_id(&c.skel, &sid) == MRW_OK);
        TASK_CHECK(mrw_id_equal(&cv.skeleton_id, &sid));
        converted_free(&c);
    }
    /* unavailable backends were reported by host_backends; scalar must still be constructible */
    mrw_dispatch d;
    TASK_CHECK(mrw_dispatch_scalar(&d) == MRW_OK && d.backend == MRW_BACKEND_SCALAR);
    return 0;
}

static int case_batch_blend_accum(void) {
    backend_t bks[8];
    int nbk = host_backends(bks, 8);
    TASK_CHECK(nbk >= 1);

    fixture fx; fixture_shear(&fx);
    const char *path = ".task-build/forest_blend.gltf";
    char diag[256];
    converted c;
    TASK_CHECK(convert_fixture(&fx, path, 1, 0, &c, diag) == 0);
    uint32_t jc = c.skel.joint_count;
    mrw_clip_view cvA, cvB;
    TASK_CHECK(clip_view_n(&c, 0, &cvA) == MRW_OK);
    TASK_CHECK(clip_view_n(&c, 1, &cvB) == MRW_OK);

    float timesA[NINST], timesB[NINST], weights[NINST];
    for (int i = 0; i < NINST; ++i) {
        timesA[i] = (float)(i * 2) / 29.0f;
        timesB[i] = (float)(NINST - 1 - i) * 3.0f / 29.0f;
        weights[i] = (float)i / (float)(NINST - 1);
    }
    float mask[FX_MAXJ];
    for (uint32_t j = 0; j < jc; ++j) mask[j] = (j % 2) ? 0.8f : 0.55f;

    /* shared additive delta over ALL output joints */
    mrw_trs delta[FX_MAXJ];
    for (uint32_t j = 0; j < jc; ++j) {
        double q[4]; double ax[3] = {0.0, 1.0, 0.0};
        quat_axis_angle(ax, 0.31 + 0.05 * j, q);
        for (int k = 0; k < 4; ++k) delta[j].rot[k] = (float)q[k];
        delta[j].trans[0] = 0.11f * (float)(j + 1);
        delta[j].trans[1] = -0.07f;
        delta[j].trans[2] = 0.05f;
        delta[j].scale[0] = 1.05f; delta[j].scale[1] = 0.97f; delta[j].scale[2] = 1.02f;
    }

    mrw_mem_req sreq, o32, o16;
    TASK_CHECK(mrw_batch_blend_clips_to_palette_requirements(jc, NINST, MRW_PALETTE_F32, &sreq, &o32) == MRW_OK);
    TASK_CHECK(mrw_batch_blend_clips_to_palette_requirements(jc, NINST, MRW_PALETTE_F16, NULL, &o16) == MRW_OK);
    mrw_mem_req asreq, ao32, ao16;
    TASK_CHECK(mrw_batch_accumulate_to_palette_requirements(jc, NINST, MRW_PALETTE_F32, &asreq, &ao32) == MRW_OK);
    TASK_CHECK(mrw_batch_accumulate_to_palette_requirements(jc, NINST, MRW_PALETTE_F16, NULL, &ao16) == MRW_OK);

    for (int b = 0; b < nbk; ++b) {
        void *scratch = mrw_authoring_alloc(sreq.size > asreq.size ? sreq.size : asreq.size);
        float *out32 = (float *)mrw_authoring_alloc(o32.size);
        uint16_t *out16 = (uint16_t *)mrw_authoring_alloc(o16.size);
        float *aout32 = (float *)mrw_authoring_alloc(ao32.size);
        uint16_t *aout16 = (uint16_t *)mrw_authoring_alloc(ao16.size);
        TASK_CHECK(scratch && out32 && out16 && aout32 && aout16);

        TASK_CHECK(mrw_batch_blend_clips_to_palette(&bks[b].d, &c.skel, &cvA, &cvB,
                   timesA, timesB, weights, mask, NINST, out32, o32.size, scratch, sreq.size) == MRW_OK);
        TASK_CHECK(mrw_batch_blend_clips_to_palette_f16(&bks[b].d, &c.skel, &cvA, &cvB,
                   timesA, timesB, weights, mask, NINST, out16, o16.size, scratch, sreq.size) == MRW_OK);
        TASK_CHECK(mrw_batch_accumulate_to_palette(&bks[b].d, &c.skel, &cvA, timesA, delta,
                   weights, mask, NINST, aout32, ao32.size, scratch, asreq.size) == MRW_OK);
        TASK_CHECK(mrw_batch_accumulate_to_palette_f16(&bks[b].d, &c.skel, &cvA, timesA, delta,
                   weights, mask, NINST, aout16, ao16.size, scratch, asreq.size) == MRW_OK);

        for (int i = 0; i < NINST; ++i) {
            /* blend reference: pose_blend sampled locals, then compose */
            mrw_trs la[FX_MAXJ], lb[FX_MAXJ], mixed[FX_MAXJ];
            TASK_CHECK(mrw_clip_sample_local(&cvA, timesA[i], la, jc) == MRW_OK);
            TASK_CHECK(mrw_clip_sample_local(&cvB, timesB[i], lb, jc) == MRW_OK);
            TASK_CHECK(mrw_pose_blend(la, lb, weights[i], mask, mixed, jc, jc) == MRW_OK);
            float ms[FX_MAXJ * 12], ref[FX_MAXJ * 12];
            TASK_CHECK(mrw_local_to_model(&c.skel, mixed, ms, jc) == MRW_OK);
            TASK_CHECK(mrw_model_to_palette(&c.skel, ms, ref, jc) == MRW_OK);
            for (uint32_t k = 0; k < jc * 12; ++k) {
                TASK_CHECK(dabs((double)out32[(size_t)i * jc * 12 + k] - ref[k]) <= f32_tol());
                TASK_CHECK(dabs(mrw_half_to_float(out16[(size_t)i * jc * 12 + k]) - ref[k])
                           <= f16_tol(ref[k]));
            }
            /* accumulate reference */
            mrw_trs base[FX_MAXJ], acc[FX_MAXJ];
            TASK_CHECK(mrw_clip_sample_local(&cvA, timesA[i], base, jc) == MRW_OK);
            TASK_CHECK(mrw_pose_accumulate(base, delta, weights[i], mask, acc, jc, jc) == MRW_OK);
            TASK_CHECK(mrw_local_to_model(&c.skel, acc, ms, jc) == MRW_OK);
            TASK_CHECK(mrw_model_to_palette(&c.skel, ms, ref, jc) == MRW_OK);
            for (uint32_t k = 0; k < jc * 12; ++k) {
                TASK_CHECK(dabs((double)aout32[(size_t)i * jc * 12 + k] - ref[k]) <= f32_tol());
                TASK_CHECK(dabs(mrw_half_to_float(aout16[(size_t)i * jc * 12 + k]) - ref[k])
                           <= f16_tol(ref[k]));
            }
        }
        mrw_authoring_free(scratch); mrw_authoring_free(out32); mrw_authoring_free(out16);
        mrw_authoring_free(aout32); mrw_authoring_free(aout16);
    }
    converted_free(&c);
    return 0;
}

/* ================================================================== registration */

const struct task_case task_cases[] = {
    { "import",    "shear_rejected_preserve_closure", case_import_shear },
    { "import",    "forest_order_api_maps",           case_import_forest_api },
    { "import",    "disconnected_roots",              case_import_disconnected },
    { "hierarchy", "rest_and_inverse_bind",           case_hierarchy_rest },
    { "hierarchy", "sampled_model_and_skin",          case_hierarchy_frames },
    { "hierarchy", "forest_roots_independent",        case_hierarchy_forest_roots },
    { "batch",     "nine_instance_clip_f32_f16",      case_batch_clip },
    { "batch",     "dual_codec",                      case_batch_codec },
    { "batch",     "blend_accumulate_f32_f16",        case_batch_blend_accum },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
