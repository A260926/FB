/* Regression target for the explicit MODEL-SPACE mesh blend (marrow_mesh_blend.c).
 *
 * Three registered groups (see task_cases[] at the bottom):
 *   hierarchy  - the single-pose mrw_pose_mesh_blend recurrence: an independent DOUBLE-PRECISION
 *                reference composes the rotation chains, blends in model space, restores local
 *                rotations, and then composes the FULL local TRS affine hierarchy and inverse-bind.
 *                Covers non-coaxial parent/child rotations, parent/child mask divergence, non-uniform
 *                scale (shear in the affine), sign-flipped quaternions, exact output aliasing,
 *                endpoint/limit weights, and finiteness/capacity/alignment/no-partial-write.
 *   batch      - mrw_batch_mesh_blend_clips_to_palette(_f16): both codecs, 9/17 instances, every
 *                host backend run and reported by name (scalar is the reference), checked against
 *                the looped single-pose reference; plus requirements/overflow/alias/tail contracts.
 *   contracts  - the pre-existing local-space entry points keep their numerics, zero-count is a
 *                no-op, and the mesh entry never writes on error.
 *
 * The SIMD backends are compared against BOTH the scalar batch output and the looped single-pose
 * reference - never to the new scalar alone - so a shared scalar bug cannot hide.
 *
 * build: linked against task_support/driver.c (provides main + TASK_CHECK), no main here.
 */
#include "task_support/check.h"
#include "marrow.h"
#include "marrow_internal.h"   /* mrw_rd_* / MRW_* wire constants for eyeballing blob bytes */
#include "mrw_build.h"         /* mrw_build / mrw_alloc64 / mrw_free (authoring lib) */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================== shared rig helpers */

#define MAXJ 64u
#define MAXS 32u
#define FPS  30.0f

typedef struct { uint32_t s; } rng_t;
static uint32_t ru32(rng_t *r) { r->s = r->s * 1664525u + 1013904223u; return r->s; }
static float    rf(rng_t *r, float lo, float hi) {
    return lo + (hi - lo) * ((float)(ru32(r) >> 8) / (float)(1u << 24));
}
/* random unit quaternion over a FULL sphere (w may be negative ⇒ exercises the sign double cover) */
static void rquat_signed(rng_t *r, float q[4]) {
    float ax = rf(r, -1, 1), ay = rf(r, -1, 1), az = rf(r, -1, 1);
    float n = sqrtf(ax*ax + ay*ay + az*az);
    if (n < 1e-6f) { ax = 0; ay = 0; az = 1; n = 1; }
    float ang = rf(r, -3.14159265f, 3.14159265f), s = sinf(ang * 0.5f);
    q[0] = ax/n * s; q[1] = ay/n * s; q[2] = az/n * s; q[3] = cosf(ang * 0.5f);
}
/* canonical (w >= 0) random quaternion */
static void rquat(rng_t *r, float q[4]) {
    rquat_signed(r, q);
    if (q[3] < 0.0f) { q[0] = -q[0]; q[1] = -q[1]; q[2] = -q[2]; q[3] = -q[3]; }
}

/* Axis-angle unit quaternion, so tests can name exact non-coaxial rotations. */
static void axis_angle(const float ax[3], float ang, float q[4]) {
    float n = sqrtf(ax[0]*ax[0] + ax[1]*ax[1] + ax[2]*ax[2]);
    if (n < 1e-12f) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; return; }
    float s = sinf(ang * 0.5f);
    q[0] = ax[0]/n * s; q[1] = ax[1]/n * s; q[2] = ax[2]/n * s; q[3] = cosf(ang * 0.5f);
}

/* ===================================================================== double-precision reference
 *
 * Deliberately INDEPENDENT of the runtime's float implementation: it works entirely in `double`,
 * with its own quaternion multiply / nlerp / normalize and its own 3x4 affine chain. It reproduces
 * the documented recurrence exactly (same clamp/mask formula) and then builds the palette from the
 * FULL local TRS affine - so non-uniform scale (which becomes shear when composed) is exercised the
 * same way the runtime does it. Everything is compared to the runtime as rotations / affines to a
 * tolerance, never bit-for-bit (different precision, different order). */

typedef struct { double q[4]; } dq_t;

static dq_t dq_mul(dq_t a, dq_t b) {   /* Hamilton, (x,y,z,w) */
    dq_t o;
    o.q[0] = a.q[3]*b.q[0] + a.q[0]*b.q[3] + a.q[1]*b.q[2] - a.q[2]*b.q[1];
    o.q[1] = a.q[3]*b.q[1] - a.q[0]*b.q[2] + a.q[1]*b.q[3] + a.q[2]*b.q[0];
    o.q[2] = a.q[3]*b.q[2] + a.q[0]*b.q[1] - a.q[1]*b.q[0] + a.q[2]*b.q[3];
    o.q[3] = a.q[3]*b.q[3] - a.q[0]*b.q[0] - a.q[1]*b.q[1] - a.q[2]*b.q[2];
    return o;
}
static dq_t dq_conj(dq_t a) { dq_t o = {{ -a.q[0], -a.q[1], -a.q[2], a.q[3] }}; return o; }
static dq_t dq_norm(dq_t a) {
    double n = sqrt(a.q[0]*a.q[0] + a.q[1]*a.q[1] + a.q[2]*a.q[2] + a.q[3]*a.q[3]);
    if (n > 0.0) for (int k = 0; k < 4; ++k) a.q[k] /= n;
    return a;
}
static dq_t dq_nlerp(dq_t a, dq_t b, double u) {
    double dot = a.q[0]*b.q[0] + a.q[1]*b.q[1] + a.q[2]*b.q[2] + a.q[3]*b.q[3];
    double s = (dot < 0.0) ? -1.0 : 1.0;
    dq_t o;
    for (int k = 0; k < 4; ++k) o.q[k] = a.q[k] + u * (s*b.q[k] - a.q[k]);
    return dq_norm(o);
}
static dq_t dq_from(const float q[4]) { dq_t o; for (int k = 0; k < 4; ++k) o.q[k] = q[k]; return o; }

/* rotation difference between two quaternions as a rotation (sign-insensitive), radians. Uses the
 * chord (not acos of the dot) so it stays well-conditioned for near-identical rotations. */
static double dq_angle(dq_t a, dq_t b) {
    double dot = a.q[0]*b.q[0] + a.q[1]*b.q[1] + a.q[2]*b.q[2] + a.q[3]*b.q[3];
    double s = (dot < 0.0) ? -1.0 : 1.0;
    double c2 = 0.0;
    for (int k = 0; k < 4; ++k) { double d = a.q[k] - s*b.q[k]; c2 += d*d; }
    /* for unit quats, |a - s*b| = 2|sin(theta/2)| ⇒ theta = 2 asin(chord/2) */
    double h = 0.5 * sqrt(c2);
    if (h > 1.0) h = 1.0;
    return 2.0 * asin(h);
}

/* dq -> 3x3 rotation matrix, row-major, v' = R v (matches mrw_quat_to_mat3 in double). */
static void dq_mat3(dq_t q, double r[9]) {
    double x=q.q[0], y=q.q[1], z=q.q[2], w=q.q[3];
    double xx=x*x, yy=y*y, zz=z*z, xy=x*y, xz=x*z, yz=y*z, wx=w*x, wy=w*y, wz=w*z;
    r[0]=1-2*(yy+zz); r[1]=2*(xy-wz);   r[2]=2*(xz+wy);
    r[3]=2*(xy+wz);   r[4]=1-2*(xx+zz); r[5]=2*(yz-wx);
    r[6]=2*(xz-wy);   r[7]=2*(yz+wx);   r[8]=1-2*(xx+yy);
}
/* 3x4 affine from a local TRS (double precision) */
static void dtrs_affine(const mrw_trs *t, double m[12]) {
    dq_t q = dq_from(t->rot);
    double r[9]; dq_mat3(q, r);
    m[0]=r[0]*t->scale[0]; m[1]=r[1]*t->scale[1]; m[2]=r[2]*t->scale[2]; m[3]=t->trans[0];
    m[4]=r[3]*t->scale[0]; m[5]=r[4]*t->scale[1]; m[6]=r[5]*t->scale[2]; m[7]=t->trans[1];
    m[8]=r[6]*t->scale[0]; m[9]=r[7]*t->scale[1]; m[10]=r[8]*t->scale[2]; m[11]=t->trans[2];
}
static void daffine_mul(const double a[12], const double b[12], double o[12]) {
    double c[12];
    for (int r = 0; r < 3; ++r) {
        double a0=a[4*r+0], a1=a[4*r+1], a2=a[4*r+2], at=a[4*r+3];
        c[4*r+0] = a0*b[0] + a1*b[4] + a2*b[8];
        c[4*r+1] = a0*b[1] + a1*b[5] + a2*b[9];
        c[4*r+2] = a0*b[2] + a1*b[6] + a2*b[10];
        c[4*r+3] = a0*b[3] + a1*b[7] + a2*b[11] + at;
    }
    memcpy(o, c, sizeof c);
}

/* The documented model-space recurrence, in double, for one pose pair. */
static void dref_mesh_blend(const mrw_skeleton_view *skel, const mrw_trs *a, const mrw_trs *b,
                            double weight, const float *mask,
                            dq_t *out_rot /*joint_count*/, mrw_trs *out_local /*joint_count*/) {
    uint32_t n = skel->joint_count;
    dq_t *QA = (dq_t *)malloc(sizeof(dq_t) * n);
    dq_t *QB = (dq_t *)malloc(sizeof(dq_t) * n);
    dq_t *QO = (dq_t *)malloc(sizeof(dq_t) * n);
    double wc = weight < 0.0 ? 0.0 : (weight > 1.0 ? 1.0 : weight);
    for (uint32_t j = 0; j < n; ++j) {
        uint16_t parent = 0;
        int root = (j == 0);
        if (!root) { mrw_skeleton_parent(skel, j, &parent); }
        dq_t la = dq_from(a[j].rot), lb = dq_from(b[j].rot);
        QA[j] = root ? dq_norm(la) : dq_norm(dq_mul(QA[parent], la));
        QB[j] = root ? dq_norm(lb) : dq_norm(dq_mul(QB[parent], lb));
        double mj = mask ? mask[j] : 1.0;
        if (mj < 0.0) mj = 0.0; if (mj > 1.0) mj = 1.0;
        double we = wc * mj; if (we < 0.0) we = 0.0; if (we > 1.0) we = 1.0;
        QO[j] = dq_nlerp(QA[j], QB[j], we);
        dq_t lo = root ? QO[j] : dq_norm(dq_mul(dq_conj(QO[parent]), QO[j]));
        out_rot[j] = QO[j];
        for (int k = 0; k < 4; ++k) out_local[j].rot[k] = (float)lo.q[k];
        for (int k = 0; k < 3; ++k) {
            out_local[j].trans[k] = (float)(a[j].trans[k] + we * ((double)b[j].trans[k] - a[j].trans[k]));
            out_local[j].scale[k] = (float)(a[j].scale[k] + we * ((double)b[j].scale[k] - a[j].scale[k]));
        }
    }
    free(QA); free(QB); free(QO);
}

/* Full local → model → inverse-bind palette, in double, from the given locals. */
static void dref_palette(const mrw_skeleton_view *skel, const mrw_trs *locals, double *pal) {
    uint32_t n = skel->joint_count;
    double *model = (double *)malloc(sizeof(double) * 12 * n);
    for (uint32_t j = 0; j < n; ++j) {
        double lm[12]; dtrs_affine(&locals[j], lm);
        if (j == 0) memcpy(model, lm, sizeof lm);
        else {
            uint16_t parent; mrw_skeleton_parent(skel, j, &parent);
            daffine_mul(model + (size_t)parent*12, lm, model + (size_t)j*12);
        }
    }
    for (uint32_t j = 0; j < n; ++j) {
        float ibf[12]; mrw_skeleton_inverse_bind(skel, j, ibf);
        double ib[12]; for (int k = 0; k < 12; ++k) ib[k] = ibf[k];
        daffine_mul(model + (size_t)j*12, ib, pal + (size_t)j*12);
    }
    free(model);
}

/* max |difference| over two 12-component affines */
static double aff_maxdiff(const float *a, const double *b) {
    double m = 0.0;
    for (int k = 0; k < 12; ++k) { double d = fabs((double)a[k] - b[k]); if (d > m) m = d; }
    return m;
}

/* ===================================================================== rig builder
 *
 * A bounded tree (a "torso" chain with a couple of arm branches) so parent/child mask divergence is
 * meaningful, with configurable non-uniform bind scale on some joints (⇒ shear in the composed
 * affine and inverse-bind) and non-coaxial parent/child rotations (two named joints get rotations
 * about different axes). Returns a validated blob; the caller keeps it alive. */
typedef struct {
    uint16_t parent[MAXJ];
    float    rest[MAXJ*10];
    float    ib[MAXJ*12];
    float    sA[MAXJ*MAXS*10], sB[MAXJ*MAXS*10];
    mrw_trs  pA[MAXJ], pB[MAXJ];   /* sample 0 of each clip, as local TRS (single-pose tests) */
    char     namebuf[MAXJ][8];
    const char *names[MAXJ];
} rig_t;

/* parent layering: 0 root; 1..3 torso; 4,5 left arm off joint 3; 6,7 right arm off joint 3.
 * Deeper indices chain. This is a helper topology, independent of the runtime's own layout. */
static void rig_parent(uint32_t j, uint16_t *p) {
    static const uint16_t fixed[] = { 0xFFFF, 0, 1, 2, 3, 3, 5, 6, 7, 3, 9, 10 };
    if (j < sizeof fixed / sizeof fixed[0]) *p = fixed[j];
    else *p = (uint16_t)(j - 1);
}

static void rig_build(rig_t *R, uint32_t jc, uint32_t sc, uint32_t flags,
                      uint32_t seed, uint32_t codec, uint8_t **out_buf, size_t *out_sz) {
    rng_t r = { seed ? seed : 1u };
    for (uint32_t j = 0; j < jc; ++j) {
        rig_parent(j, &R->parent[j]);
        float q[4];
        /* Explicit NON-COAXIAL parent/child rotations: joint 1 about Y, joint 3 about Z (the
         * parent of joint 4), joint 4 about X. The rest random. */
        if (j == 1)      { float ay[3] = {0,1,0}; axis_angle(ay, 0.7f, q); }
        else if (j == 3) { float az[3] = {0,0,1}; axis_angle(az, 0.9f, q); }
        else if (j == 4) { float ax[3] = {1,0,0}; axis_angle(ax, 1.1f, q); }
        else rquat(&r, q);
        R->rest[j*10+0]=q[0]; R->rest[j*10+1]=q[1]; R->rest[j*10+2]=q[2]; R->rest[j*10+3]=q[3];
        R->rest[j*10+4]=(j==0)?0.0f:rf(&r,0.05f,0.18f);   /* character-scale bone offsets */
        R->rest[j*10+5]=rf(&r,-0.05f,0.05f);
        R->rest[j*10+6]=rf(&r,-0.05f,0.05f);
        /* every 3rd non-root joint: non-uniform bind scale (⇒ shear in composed affine) */
        if (j != 0 && (j % 3) == 1) {
            R->rest[j*10+7]=rf(&r,0.6f,1.7f);
            R->rest[j*10+8]=rf(&r,0.6f,1.7f);
            R->rest[j*10+9]=rf(&r,0.6f,1.7f);
        } else {
            R->rest[j*10+7]=R->rest[j*10+8]=R->rest[j*10+9]=1.0f;
        }
        snprintf(R->namebuf[j], sizeof R->namebuf[j], "j%u", j);
        R->names[j] = R->namebuf[j];
    }
    /* inverse-bind from the actual rest hierarchy (a real, non-identity bind) */
    {
        float model[MAXJ*12];
        for (uint32_t j = 0; j < jc; ++j) {
            mrw_trs t;
            memcpy(t.rot,   R->rest + j*10 + 0, 16);
            memcpy(t.trans, R->rest + j*10 + 4, 12);
            memcpy(t.scale, R->rest + j*10 + 7, 12);
            float lm[12]; mrw_trs_to_affine(&t, lm);
            if (j == 0) memcpy(model, lm, sizeof lm);
            else mrw_affine_mul(model + (size_t)R->parent[j]*12, lm, model + (size_t)j*12);
            /* inverse of the 3x4 affine, computed here in double for a real non-identity bind */
            const float *m = model + (size_t)j*12;
            double A[9] = { m[0],m[1],m[2], m[4],m[5],m[6], m[8],m[9],m[10] };
            double det = A[0]*(A[4]*A[8]-A[5]*A[7]) - A[1]*(A[3]*A[8]-A[5]*A[6]) + A[2]*(A[3]*A[7]-A[4]*A[6]);
            double inv[9];
            inv[0]=(A[4]*A[8]-A[5]*A[7])/det; inv[1]=(A[2]*A[7]-A[1]*A[8])/det; inv[2]=(A[1]*A[5]-A[2]*A[4])/det;
            inv[3]=(A[5]*A[6]-A[3]*A[8])/det; inv[4]=(A[0]*A[8]-A[2]*A[6])/det; inv[5]=(A[2]*A[3]-A[0]*A[5])/det;
            inv[6]=(A[3]*A[7]-A[4]*A[6])/det; inv[7]=(A[1]*A[6]-A[0]*A[7])/det; inv[8]=(A[0]*A[4]-A[1]*A[3])/det;
            double t3[3] = { m[3], m[7], m[11] };
            R->ib[j*12+0]=(float)inv[0]; R->ib[j*12+1]=(float)inv[1]; R->ib[j*12+2]=(float)inv[2];
            R->ib[j*12+3]=(float)-(inv[0]*t3[0]+inv[1]*t3[1]+inv[2]*t3[2]);
            R->ib[j*12+4]=(float)inv[3]; R->ib[j*12+5]=(float)inv[4]; R->ib[j*12+6]=(float)inv[5];
            R->ib[j*12+7]=(float)-(inv[3]*t3[0]+inv[4]*t3[1]+inv[5]*t3[2]);
            R->ib[j*12+8]=(float)inv[6]; R->ib[j*12+9]=(float)inv[7]; R->ib[j*12+10]=(float)inv[8];
            R->ib[j*12+11]=(float)-(inv[6]*t3[0]+inv[7]*t3[1]+inv[8]*t3[2]);
        }
    }
    for (uint32_t j = 0; j < jc; ++j) {
        for (uint32_t s = 0; s < sc; ++s) {
            float *pa = R->sA + ((size_t)j*sc + s)*10;
            float *pb = R->sB + ((size_t)j*sc + s)*10;
            float qa[4]; rquat_signed(&r, qa);
            float qb[4]; rquat_signed(&r, qb);
            /* make the two clips clearly different, and non-coaxial on the arm joints */
            if (j == 4) { float ay[3] = {0,1,0}; axis_angle(ay, (float)(s+1)*0.5f, qa); }
            if (j == 4) { float ax[3] = {1,0,0}; axis_angle(ax, (float)(s+1)*0.7f, qb); }
            for (int k=0;k<4;++k) { pa[k]=qa[k]; pb[k]=qb[k]; }
            pa[4]=rf(&r,-0.1f,0.1f); pa[5]=rf(&r,-0.1f,0.1f); pa[6]=rf(&r,-0.1f,0.1f);
            pb[4]=rf(&r,-0.1f,0.1f); pb[5]=rf(&r,-0.1f,0.1f); pb[6]=rf(&r,-0.1f,0.1f);
            if (codec == 0) {
                pa[7]=rf(&r,0.5f,1.6f); pa[8]=rf(&r,0.5f,1.6f); pa[9]=rf(&r,0.5f,1.6f);
                pb[7]=rf(&r,0.5f,1.6f); pb[8]=rf(&r,0.5f,1.6f); pb[9]=rf(&r,0.5f,1.6f);
            } else {
                pa[7]=pa[8]=pa[9]=1.0f; pb[7]=pb[8]=pb[9]=1.0f;
            }
        }
    }
    for (uint32_t j = 0; j < jc; ++j) {
        memcpy(R->pA[j].rot,  R->sA + (size_t)j*sc*10 + 0, 16);
        memcpy(R->pA[j].trans, R->sA + (size_t)j*sc*10 + 4, 12);
        memcpy(R->pA[j].scale, R->sA + (size_t)j*sc*10 + 7, 12);
        memcpy(R->pB[j].rot,  R->sB + (size_t)j*sc*10 + 0, 16);
        memcpy(R->pB[j].trans, R->sB + (size_t)j*sc*10 + 4, 12);
        memcpy(R->pB[j].scale, R->sB + (size_t)j*sc*10 + 7, 12);
    }
    mrw_skel skel = { jc, R->parent, R->rest, R->ib, R->names };
    mrw_clip clips[2] = {
        { FPS, sc, flags, R->sA, NULL, codec },
        { FPS, sc, flags, R->sB, NULL, codec },
    };
    *out_sz = mrw_build(&skel, clips, 2, NULL, out_buf);
}

/* ===================================================================== hierarchy group */

/* Non-coaxial parent/child rotation: with mask=1 everywhere, the output local rotation must equal the
 * model-space nlerp restored against the output parent - an independent double reference. */
static int case_noncoaxial(void) {
    rig_t R; uint8_t *buf = NULL; size_t sz = 0;
    rig_build(&R, 12, 3, 0, 101, 0, &buf, &sz);
    mrw_blob blob; TASK_CHECK(mrw_blob_open(buf, sz, &blob) == MRW_OK);
    mrw_skeleton_view sv; TASK_CHECK(mrw_blob_skeleton(&blob, &sv) == MRW_OK);

    uint32_t jc = sv.joint_count;
    mrw_trs a[MAXJ], b[MAXJ], got[MAXJ];
    for (uint32_t j = 0; j < jc; ++j) {
        a[j] = R.pA[j];
        b[j] = R.pB[j];
    }
    float mask[MAXJ]; for (uint32_t j = 0; j < jc; ++j) mask[j] = 1.0f;

    mrw_mem_req sr; TASK_CHECK(mrw_pose_mesh_blend_requirements(jc, &sr) == MRW_OK);
    void *scratch = mrw_alloc64(sr.size);
    TASK_CHECK(scratch != NULL);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 0.5f, mask, got, jc, jc, scratch, sr.size) == MRW_OK);

    dq_t *qo = (dq_t *)malloc(sizeof(dq_t) * jc);
    mrw_trs *ref = (mrw_trs *)malloc(sizeof(mrw_trs) * jc);
    dref_mesh_blend(&sv, a, b, 0.5, mask, qo, ref);

    /* parent 1 about Y, child 4 about X: the model-space blend must not just average the locals */
    for (uint32_t j = 0; j < jc; ++j) {
        dq_t g = dq_from(got[j].rot);
        TASK_CHECK(dq_angle(g, dq_from(ref[j].rot)) < 2e-5);
        for (int k = 0; k < 3; ++k) {
            TASK_CHECK(fabs((double)got[j].trans[k] - ref[j].trans[k]) < 1e-5);
            TASK_CHECK(fabs((double)got[j].scale[k] - ref[j].scale[k]) < 1e-5);
        }
    }
    /* And it must differ from the LOCAL-space blend (the whole point of the entry) on the child of a
     * non-coaxial moving parent: joint 4's parent (joint 3) rotates about Z in A vs B, so a local
     * child blend and a model-space child blend give materially different directions. */
    mrw_trs localblend[MAXJ];
    TASK_CHECK(mrw_pose_blend(a, b, 0.5f, mask, localblend, jc, jc) == MRW_OK);
    double child_local_vs_model = dq_angle(dq_from(localblend[4].rot), dq_from(got[4].rot));
    TASK_CHECK(child_local_vs_model > 0.02);
    /* In model space the mesh-blended child direction must be the nlerp of the two inputs' child
     * directions (parent accounted for) - which the local blend does not generally produce. */
    dq_t mA[MAXJ], mB[MAXJ], mL[MAXJ];
    for (uint32_t j = 0; j < jc; ++j) {
        uint16_t parent = 0; int root = (j == 0);
        if (!root) mrw_skeleton_parent(&sv, j, &parent);
        dq_t la = dq_from(a[j].rot), lb = dq_from(b[j].rot), ll = dq_from(localblend[j].rot);
        mA[j] = root ? dq_norm(la) : dq_norm(dq_mul(mA[parent], la));
        mB[j] = root ? dq_norm(lb) : dq_norm(dq_mul(mB[parent], lb));
        mL[j] = root ? dq_norm(ll) : dq_norm(dq_mul(mL[parent], ll));
    }
    dq_t expect = dq_nlerp(mA[4], mB[4], 0.5);
    /* got[] are LOCAL rotations - compose the OUTPUT up its chain to compare model-space direction */
    dq_t mo[MAXJ];
    for (uint32_t j = 0; j < jc; ++j) {
        uint16_t parent = 0; int root = (j == 0);
        if (!root) mrw_skeleton_parent(&sv, j, &parent);
        dq_t l = dq_from(got[j].rot);
        mo[j] = root ? dq_norm(l) : dq_norm(dq_mul(mo[parent], l));
    }
    TASK_CHECK(dq_angle(mo[4], expect) < 2e-5);   /* output model-space dir == nlerp */
    TASK_CHECK(dq_angle(mL[4], expect) > 0.02);   /* local blend does NOT match it */

    /* palette parity through the FULL local affine hierarchy + inverse-bind (double reference) */
    double refpal[MAXJ*12];
    dref_palette(&sv, ref, refpal);
    float model[MAXJ*12], pal[MAXJ*12];
    TASK_CHECK(mrw_local_to_model(&sv, got, model, jc) == MRW_OK);
    TASK_CHECK(mrw_model_to_palette(&sv, model, pal, jc) == MRW_OK);
    for (uint32_t j = 0; j < jc; ++j)
        TASK_CHECK(aff_maxdiff(pal + j*12, refpal + j*12) < 2e-5);

    free(qo); free(ref); mrw_free((uint8_t*)scratch); mrw_free(buf);
    return 0;
}

/* Parent/child MASK divergence: parent fully blended, child mask=0. The child must keep input A's
 * model-space direction, so its local rotation compensates for the moved parent. */
static int case_mask_divergence(void) {
    rig_t R; uint8_t *buf = NULL; size_t sz = 0;
    rig_build(&R, 12, 3, 0, 202, 0, &buf, &sz);
    mrw_blob blob; TASK_CHECK(mrw_blob_open(buf, sz, &blob) == MRW_OK);
    mrw_skeleton_view sv; TASK_CHECK(mrw_blob_skeleton(&blob, &sv) == MRW_OK);
    uint32_t jc = sv.joint_count;

    mrw_trs a[MAXJ], b[MAXJ], got[MAXJ];
    for (uint32_t j = 0; j < jc; ++j) {
        a[j] = R.pA[j];
        b[j] = R.pB[j];
    }
    uint16_t arm = 4;              /* arm joint, child of joint 3 */
    uint16_t torso = 3;           /* its parent */
    float mask[MAXJ];
    for (uint32_t j = 0; j < jc; ++j) mask[j] = 1.0f;
    mask[arm] = 0.0f;             /* child masked out; ancestor blended */

    mrw_mem_req sr; TASK_CHECK(mrw_pose_mesh_blend_requirements(jc, &sr) == MRW_OK);
    void *scratch = mrw_alloc64(sr.size);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 0.65f, mask, got, jc, jc, scratch, sr.size) == MRW_OK);

    /* Independent double reference for model-space directions of the OUTPUT pose. */
    dq_t *qo = (dq_t *)malloc(sizeof(dq_t) * jc);
    mrw_trs *ref = (mrw_trs *)malloc(sizeof(mrw_trs) * jc);
    dref_mesh_blend(&sv, a, b, 0.65, mask, qo, ref);
    for (uint32_t j = 0; j < jc; ++j) {
        TASK_CHECK(dq_angle(dq_from(got[j].rot), dq_from(ref[j].rot)) < 2e-5);
    }

    /* Compose the model-space rotation of the OUTPUT locals in double (independent of runtime
     * helpers): the masked arm's model-space rotation must equal input A's model-space rotation. */
    dq_t *ma = (dq_t *)malloc(sizeof(dq_t) * jc);
    dq_t *mo = (dq_t *)malloc(sizeof(dq_t) * jc);
    for (uint32_t j = 0; j < jc; ++j) {
        uint16_t parent = 0; int root = (j == 0);
        if (!root) mrw_skeleton_parent(&sv, j, &parent);
        dq_t la = dq_from(a[j].rot), lo = dq_from(got[j].rot);
        ma[j] = root ? dq_norm(la) : dq_norm(dq_mul(ma[parent], la));
        mo[j] = root ? dq_norm(lo) : dq_norm(dq_mul(mo[parent], lo));
    }
    TASK_CHECK(dq_angle(ma[arm], mo[arm]) < 2e-5);          /* masked child: A's model-space direction */
    TASK_CHECK(dq_angle(mo[torso], ma[torso]) > 0.02);      /* ...while the ancestor moved from A */
    /* trans/scale of the masked joint still take the SAME effective weight (0) ⇒ exactly A */
    for (int k = 0; k < 3; ++k) {
        TASK_CHECK(got[arm].trans[k] == a[arm].trans[k]);
        TASK_CHECK(got[arm].scale[k] == a[arm].scale[k]);
    }
    free(ma); free(mo); free(qo); free(ref); mrw_free((uint8_t*)scratch); mrw_free(buf);
    return 0;
}

/* Non-uniform scale / shear survives into the palette: compare the full composed affine to the
 * double reference, which builds it from the actual local TRS (scale columns), not a stripped R. */
static int case_nonuniform_shear(void) {
    rig_t R; uint8_t *buf = NULL; size_t sz = 0;
    rig_build(&R, 16, 3, 0, 303, 0, &buf, &sz);
    mrw_blob blob; TASK_CHECK(mrw_blob_open(buf, sz, &blob) == MRW_OK);
    mrw_skeleton_view sv; TASK_CHECK(mrw_blob_skeleton(&blob, &sv) == MRW_OK);
    uint32_t jc = sv.joint_count;

    mrw_trs a[MAXJ], b[MAXJ], got[MAXJ];
    for (uint32_t j = 0; j < jc; ++j) {
        a[j] = R.pA[j];
        b[j] = R.pB[j];
    }
    float mask[MAXJ];
    for (uint32_t j = 0; j < jc; ++j) mask[j] = (j % 2) ? 0.3f : 0.9f;

    mrw_mem_req sr; TASK_CHECK(mrw_pose_mesh_blend_requirements(jc, &sr) == MRW_OK);
    void *scratch = mrw_alloc64(sr.size);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 0.7f, mask, got, jc, jc, scratch, sr.size) == MRW_OK);

    dq_t *qo = (dq_t *)malloc(sizeof(dq_t) * jc);
    mrw_trs *ref = (mrw_trs *)malloc(sizeof(mrw_trs) * jc);
    dref_mesh_blend(&sv, a, b, 0.7, mask, qo, ref);
    double refpal[MAXJ*12];
    dref_palette(&sv, ref, refpal);
    float model[MAXJ*12], pal[MAXJ*12];
    TASK_CHECK(mrw_local_to_model(&sv, got, model, jc) == MRW_OK);
    TASK_CHECK(mrw_model_to_palette(&sv, model, pal, jc) == MRW_OK);
    for (uint32_t j = 0; j < jc; ++j) TASK_CHECK(aff_maxdiff(pal + j*12, refpal + j*12) < 2e-5);

    /* a joint with non-uniform scale ⇒ matrix has shear (off-diagonal asymmetry) somewhere */
    int nonuniform = 0;
    for (uint32_t j = 0; j < jc; ++j) {
        float s0 = got[j].scale[0], s1 = got[j].scale[1], s2 = got[j].scale[2];
        if (fabsf(s0 - s1) > 0.05f || fabsf(s1 - s2) > 0.05f) nonuniform = 1;
    }
    TASK_CHECK(nonuniform);
    free(qo); free(ref); mrw_free((uint8_t*)scratch); mrw_free(buf);
    return 0;
}

/* Quaternion sign invariance: negating an input rotation must not change the result rotation (q≡−q),
 * for both a fully blended joint and a masked-out child. */
static int case_quat_sign(void) {
    rig_t R; uint8_t *buf = NULL; size_t sz = 0;
    rig_build(&R, 12, 3, 0, 404, 0, &buf, &sz);
    mrw_blob blob; TASK_CHECK(mrw_blob_open(buf, sz, &blob) == MRW_OK);
    mrw_skeleton_view sv; TASK_CHECK(mrw_blob_skeleton(&blob, &sv) == MRW_OK);
    uint32_t jc = sv.joint_count;

    mrw_trs a[MAXJ], b[MAXJ], an[MAXJ], got[MAXJ], gotn[MAXJ];
    for (uint32_t j = 0; j < jc; ++j) {
        a[j] = R.pA[j];
        b[j] = R.pB[j];
        memcpy(&an[j], &a[j], sizeof a[j]);
        for (int k = 0; k < 4; ++k) an[j].rot[k] = -an[j].rot[k];  /* sign-flip input A */
    }
    float mask[MAXJ]; for (uint32_t j = 0; j < jc; ++j) mask[j] = (j == 4) ? 0.0f : 0.6f;
    mrw_mem_req sr; TASK_CHECK(mrw_pose_mesh_blend_requirements(jc, &sr) == MRW_OK);
    void *scratch = mrw_alloc64(sr.size);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 0.6f, mask, got, jc, jc, scratch, sr.size) == MRW_OK);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, an, b, 0.6f, mask, gotn, jc, jc, scratch, sr.size) == MRW_OK);
    for (uint32_t j = 0; j < jc; ++j)
        TASK_CHECK(dq_angle(dq_from(got[j].rot), dq_from(gotn[j].rot)) < 2e-5);
    mrw_free((uint8_t*)scratch); mrw_free(buf);
    return 0;
}

/* Endpoint / limit weights: weight 0 ⇒ local rotations restore input A exactly; weight 1 ⇒ input B;
 * out-of-range weights clamp. Trans/scale lerp endpoints are exact. */
static int case_weight_endpoints(void) {
    rig_t R; uint8_t *buf = NULL; size_t sz = 0;
    rig_build(&R, 12, 3, 0, 505, 0, &buf, &sz);
    mrw_blob blob; TASK_CHECK(mrw_blob_open(buf, sz, &blob) == MRW_OK);
    mrw_skeleton_view sv; TASK_CHECK(mrw_blob_skeleton(&blob, &sv) == MRW_OK);
    uint32_t jc = sv.joint_count;

    mrw_trs a[MAXJ], b[MAXJ], got[MAXJ];
    for (uint32_t j = 0; j < jc; ++j) {
        a[j] = R.pA[j];
        b[j] = R.pB[j];
    }
    mrw_mem_req sr; TASK_CHECK(mrw_pose_mesh_blend_requirements(jc, &sr) == MRW_OK);
    void *scratch = mrw_alloc64(sr.size);

    /* weight 0 ⇒ equals input A (rotation up to sign, trans/scale exact) */
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 0.0f, NULL, got, jc, jc, scratch, sr.size) == MRW_OK);
    for (uint32_t j = 0; j < jc; ++j) {
        TASK_CHECK(dq_angle(dq_from(a[j].rot), dq_from(got[j].rot)) < 2e-5);
        for (int k = 0; k < 3; ++k) {
            TASK_CHECK(got[j].trans[k] == a[j].trans[k]);
            TASK_CHECK(got[j].scale[k] == a[j].scale[k]);
        }
    }
    /* weight 1 ⇒ equals input B */
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 1.0f, NULL, got, jc, jc, scratch, sr.size) == MRW_OK);
    for (uint32_t j = 0; j < jc; ++j) {
        TASK_CHECK(dq_angle(dq_from(b[j].rot), dq_from(got[j].rot)) < 2e-5) ;
        for (int k = 0; k < 3; ++k) {
            TASK_CHECK(fabs((double)got[j].trans[k] - b[j].trans[k]) < 1e-5);
            TASK_CHECK(fabs((double)got[j].scale[k] - b[j].scale[k]) < 1e-5);
        }
    }
    /* clamp: weight -3 ≡ 0, weight +5 ≡ 1 (compare to endpoint results) */
    mrw_trs lo[MAXJ], hi[MAXJ];
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 0.0f, NULL, lo, jc, jc, scratch, sr.size) == MRW_OK);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, -3.0f, NULL, got, jc, jc, scratch, sr.size) == MRW_OK);
    for (uint32_t j = 0; j < jc; ++j) TASK_CHECK(dq_angle(dq_from(lo[j].rot), dq_from(got[j].rot)) < 2e-6);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 1.0f, NULL, hi, jc, jc, scratch, sr.size) == MRW_OK);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 5.0f, NULL, got, jc, jc, scratch, sr.size) == MRW_OK);
    for (uint32_t j = 0; j < jc; ++j) TASK_CHECK(dq_angle(dq_from(hi[j].rot), dq_from(got[j].rot)) < 2e-6);

    mrw_free((uint8_t*)scratch); mrw_free(buf);
    return 0;
}

/* Out aliasing: out may EXACTLY alias a or b; must equal the non-aliased result. */
static int case_alias(void) {
    rig_t R; uint8_t *buf = NULL; size_t sz = 0;
    rig_build(&R, 12, 3, 0, 606, 0, &buf, &sz);
    mrw_blob blob; TASK_CHECK(mrw_blob_open(buf, sz, &blob) == MRW_OK);
    mrw_skeleton_view sv; TASK_CHECK(mrw_blob_skeleton(&blob, &sv) == MRW_OK);
    uint32_t jc = sv.joint_count;

    mrw_trs a[MAXJ], b[MAXJ], ref[MAXJ], aliased[MAXJ];
    for (uint32_t j = 0; j < jc; ++j) {
        a[j] = R.pA[j];
        b[j] = R.pB[j];
    }
    float mask[MAXJ]; for (uint32_t j = 0; j < jc; ++j) mask[j] = (j % 3) ? 0.4f : 0.8f;
    mrw_mem_req sr; TASK_CHECK(mrw_pose_mesh_blend_requirements(jc, &sr) == MRW_OK);
    void *scratch = mrw_alloc64(sr.size);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 0.5f, mask, ref, jc, jc, scratch, sr.size) == MRW_OK);

    /* alias A */
    memcpy(aliased, a, sizeof(mrw_trs) * jc);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, aliased, b, 0.5f, mask, aliased, jc, jc, scratch, sr.size) == MRW_OK);
    for (uint32_t j = 0; j < jc; ++j) TASK_CHECK(dq_angle(dq_from(ref[j].rot), dq_from(aliased[j].rot)) < 2e-6);
    /* alias B */
    memcpy(aliased, b, sizeof(mrw_trs) * jc);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, aliased, 0.5f, mask, aliased, jc, jc, scratch, sr.size) == MRW_OK);
    for (uint32_t j = 0; j < jc; ++j) TASK_CHECK(dq_angle(dq_from(ref[j].rot), dq_from(aliased[j].rot)) < 2e-6);
    mrw_free((uint8_t*)scratch); mrw_free(buf);
    return 0;
}

/* ===================================================================== contracts group */

static int case_single_contracts(void) {
    rig_t R; uint8_t *buf = NULL; size_t sz = 0;
    rig_build(&R, 10, 2, 0, 707, 0, &buf, &sz);
    mrw_blob blob; TASK_CHECK(mrw_blob_open(buf, sz, &blob) == MRW_OK);
    mrw_skeleton_view sv; TASK_CHECK(mrw_blob_skeleton(&blob, &sv) == MRW_OK);
    uint32_t jc = sv.joint_count;
    mrw_trs a[MAXJ], b[MAXJ], out[MAXJ];
    for (uint32_t j = 0; j < jc; ++j) {
        a[j] = R.pA[j];
        b[j] = R.pB[j];
    }
    mrw_mem_req sr; TASK_CHECK(mrw_pose_mesh_blend_requirements(jc, &sr) == MRW_OK);
    TASK_CHECK(sr.align == 64);
    TASK_CHECK(sr.size > 0);
    void *scratch = mrw_alloc64(sr.size);

    /* zero count is a clean no-op regardless of pointers */
    TASK_CHECK(mrw_pose_mesh_blend(&sv, NULL, NULL, 0.5f, NULL, NULL, 0, 0, NULL, 0) == MRW_OK);
    /* capacity */
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 0.5f, NULL, out, jc, jc - 1, scratch, sr.size) == MRW_E_CAPACITY);
    /* scratch capacity + alignment */
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 0.5f, NULL, out, jc, jc, scratch, sr.size - 1) == MRW_E_CAPACITY);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 0.5f, NULL, out, jc, jc, (char*)scratch + 8, sr.size) == MRW_E_ALIGN);
    /* joint count / skeleton mismatch */
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 0.5f, NULL, out, jc - 1, jc, scratch, sr.size) == MRW_E_INCOMPATIBLE);
    /* NULL inputs */
    TASK_CHECK(mrw_pose_mesh_blend(&sv, NULL, b, 0.5f, NULL, out, jc, jc, scratch, sr.size) == MRW_E_RANGE);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, NULL, 0.5f, NULL, out, jc, jc, scratch, sr.size) == MRW_E_RANGE);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 0.5f, NULL, NULL, jc, jc, scratch, sr.size) == MRW_E_RANGE);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 0.5f, NULL, out, jc, jc, NULL, 0) == MRW_E_RANGE);

    /* non-finite weight / pose / mask ⇒ RANGE with NO write (canary) */
    unsigned char canary[sizeof(mrw_trs) * MAXJ];
    memset(canary, 0xAB, sizeof canary);
    memcpy(out, canary, sizeof out);
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, (float)NAN, NULL, out, jc, jc, scratch, sr.size) == MRW_E_RANGE);
    for (size_t k = 0; k < sizeof out; ++k) TASK_CHECK(((unsigned char*)out)[k] == 0xAB);

    mrw_trs bad[MAXJ]; memcpy(bad, a, sizeof(mrw_trs) * jc);
    bad[jc/2].trans[1] = (float)INFINITY;
    TASK_CHECK(mrw_pose_mesh_blend(&sv, bad, b, 0.5f, NULL, out, jc, jc, scratch, sr.size) == MRW_E_RANGE);
    for (size_t k = 0; k < sizeof out; ++k) TASK_CHECK(((unsigned char*)out)[k] == 0xAB);

    float mask[MAXJ]; for (uint32_t j = 0; j < jc; ++j) mask[j] = 1.0f;
    mask[3] = (float)NAN;
    TASK_CHECK(mrw_pose_mesh_blend(&sv, a, b, 0.5f, mask, out, jc, jc, scratch, sr.size) == MRW_E_RANGE);
    for (size_t k = 0; k < sizeof out; ++k) TASK_CHECK(((unsigned char*)out)[k] == 0xAB);

    /* zero-joint requirements query is fine */
    mrw_mem_req zr; TASK_CHECK(mrw_pose_mesh_blend_requirements(0, &zr) == MRW_OK);
    TASK_CHECK(zr.size == 0);
    TASK_CHECK(mrw_pose_mesh_blend_requirements(4, NULL) == MRW_E_RANGE);

    mrw_free((uint8_t*)scratch); mrw_free(buf);
    return 0;
}

/* The pre-existing local-space entry points keep their numerics: the mesh entry must not perturb
 * mrw_pose_blend / mrw_batch_blend_clips_to_palette. Spot-check against their documented semantics. */
static int case_legacy_unchanged(void) {
    rig_t R; uint8_t *buf = NULL; size_t sz = 0;
    rig_build(&R, 10, 4, 0, 808, 0, &buf, &sz);
    mrw_blob blob; TASK_CHECK(mrw_blob_open(buf, sz, &blob) == MRW_OK);
    mrw_skeleton_view sv; TASK_CHECK(mrw_blob_skeleton(&blob, &sv) == MRW_OK);
    mrw_clip_view cA, cB;
    TASK_CHECK(mrw_clip_view_at(&blob, 1, &cA) == MRW_OK);
    TASK_CHECK(mrw_clip_view_at(&blob, 2, &cB) == MRW_OK);
    uint32_t jc = sv.joint_count;

    mrw_trs a[MAXJ], b[MAXJ], out[MAXJ];
    for (uint32_t j = 0; j < jc; ++j) {
        a[j] = R.pA[j];
        b[j] = R.pB[j];
    }
    /* local blend: w=0 ⇒ trans/scale exactly A */
    TASK_CHECK(mrw_pose_blend(a, b, 0.0f, NULL, out, jc, jc) == MRW_OK);
    for (uint32_t j = 0; j < jc; ++j) for (int k = 0; k < 3; ++k) {
        TASK_CHECK(out[j].trans[k] == a[j].trans[k]);
        TASK_CHECK(out[j].scale[k] == a[j].scale[k]);
    }
    /* batch blend clips still works and matches a hand-rolled loop of mrw_pose_blend for one lane
     * (spot check instance 3 only, to stay independent of test_blend_batch). */
    mrw_mem_req sreq, oreq;
    TASK_CHECK(mrw_batch_blend_clips_to_palette_requirements(jc, 9, MRW_PALETTE_F32, &sreq, &oreq) == MRW_OK);
    void *scr = mrw_alloc64(sreq.size); float *pal = (float *)mrw_alloc64(oreq.size);
    float tA[9], tB[9], w[9];
    for (int i = 0; i < 9; ++i) { tA[i] = 0.05f * i; tB[i] = 0.04f * i; w[i] = 0.1f * i; }
    mrw_dispatch d; TASK_CHECK(mrw_dispatch_scalar(&d) == MRW_OK);
    TASK_CHECK(mrw_batch_blend_clips_to_palette(&d, &sv, &cA, &cB, tA, tB, w, NULL, 9,
                                                pal, oreq.size, scr, sreq.size) == MRW_OK);
    /* instance 3: sample the reference through the single-pose local path */
    mrw_trs la[MAXJ], lb[MAXJ], bl[MAXJ];
    TASK_CHECK(mrw_clip_sample_local(&cA, tA[3], la, jc) == MRW_OK);
    TASK_CHECK(mrw_clip_sample_local(&cB, tB[3], lb, jc) == MRW_OK);
    TASK_CHECK(mrw_pose_blend(la, lb, w[3], NULL, bl, jc, jc) == MRW_OK);
    float model[MAXJ*12], refpal[MAXJ*12];
    TASK_CHECK(mrw_local_to_model(&sv, bl, model, jc) == MRW_OK);
    TASK_CHECK(mrw_model_to_palette(&sv, model, refpal, jc) == MRW_OK);
    for (uint32_t k = 0; k < jc*12u; ++k) TASK_CHECK(pal[(size_t)3*jc*12u + k] == refpal[k]);

    mrw_free((uint8_t*)scr); mrw_free((uint8_t*)pal); mrw_free(buf);
    return 0;
}

/* ===================================================================== batch group */

/* host backends in the same shape the existing tests use; each carries a display name */
typedef struct { mrw_dispatch d; const char *name; int simd; } backend_t;

static int host_backends(backend_t *bks) {
    int n = 0; mrw_dispatch t;
    if (mrw_dispatch_scalar(&t) == MRW_OK) bks[n++] = (backend_t){ t, "scalar", 0 };
    if (mrw_dispatch_sse2(&t)  == MRW_OK) bks[n++] = (backend_t){ t, "sse2", 1 };
    if (mrw_dispatch_avx2(&t)  == MRW_OK) {
        bks[n++] = (backend_t){ t, "avx2", 1 };
        if (t.features & MRW_FEAT_FMA) {
            mrw_dispatch nf = t; nf.features &= ~(uint32_t)MRW_FEAT_FMA;
            bks[n++] = (backend_t){ nf, "avx2-nofma", 1 };
        }
        if (t.features & MRW_FEAT_F16C) {
            mrw_dispatch n16 = t; n16.features &= ~(uint32_t)MRW_FEAT_F16C;
            bks[n++] = (backend_t){ n16, "avx2-nof16c", 1 };
        }
    }
    return n;
}

/* Looped single-pose reference: sample both clips, mesh-blend via the single-pose entry, then
 * local→model→palette. NOTE: this is ONE reference; the SIMD kernels are also compared to the
 * scalar batch output (which is checked against this loop), so SIMD never validates against the new
 * scalar alone. Returns 0 on success (the caller TASK_CHECKs it). */
static int oracle_instance(const mrw_skeleton_view *sv, const mrw_clip_view *cA, const mrw_clip_view *cB,
                           float tA, float tB, float w, const float *mask,
                           float *outpal, void *scratch, size_t scratch_sz, uint32_t jc) {
    static mrw_trs la[MAXJ], lb[MAXJ], bl[MAXJ];
    static float model[MAXJ*12];
    if (mrw_clip_sample_local(cA, tA, la, jc) != MRW_OK) return 1;
    if (mrw_clip_sample_local(cB, tB, lb, jc) != MRW_OK) return 1;
    if (mrw_pose_mesh_blend(sv, la, lb, w, mask, bl, jc, jc, scratch, scratch_sz) != MRW_OK) return 1;
    if (mrw_local_to_model(sv, bl, model, jc) != MRW_OK) return 1;
    if (mrw_model_to_palette(sv, model, outpal, jc) != MRW_OK) return 1;
    return 0;
}

static int report_backends(void) {
    backend_t bk[6]; int n = host_backends(bk);
    printf("MESH_BLEND_BACKENDS:");
    for (int i = 0; i < n; ++i) printf(" %s", bk[i].name);
    printf("\n");
    /* scalar MUST be available */
    TASK_CHECK(n >= 1);
    TASK_CHECK(strcmp(bk[0].name, "scalar") == 0);
    return 0;
}

/* Full parity for one rig shape and tail count, over both codecs and both output formats. */
static int run_batch_shape(uint32_t jc, uint32_t sc, uint32_t flags, uint32_t codec,
                           uint32_t seed, uint32_t N) {
    rig_t R; uint8_t *buf = NULL; size_t sz = 0;
    rig_build(&R, jc, sc, flags, seed, codec, &buf, &sz);
    mrw_blob blob; TASK_CHECK(mrw_blob_open(buf, sz, &blob) == MRW_OK);
    mrw_skeleton_view sv; TASK_CHECK(mrw_blob_skeleton(&blob, &sv) == MRW_OK);
    mrw_clip_view cA, cB;
    TASK_CHECK(mrw_clip_view_at(&blob, 1, &cA) == MRW_OK);
    TASK_CHECK(mrw_clip_view_at(&blob, 2, &cB) == MRW_OK);

    backend_t bk[6]; int nbk = host_backends(bk);
    float dur = (sc > 1) ? (float)(sc - 1) / FPS : 0.0f;
    rng_t r = { seed * 2654435761u + 7u };

    float *tA = (float *)malloc(sizeof(float) * N);
    float *tB = (float *)malloc(sizeof(float) * N);
    float *w  = (float *)malloc(sizeof(float) * N);
    for (uint32_t i = 0; i < N; ++i) {
        tA[i] = (sc > 1) ? rf(&r, -1.5f*dur, 1.5f*dur) : rf(&r, -5.0f, 5.0f);  /* looping/clamp */
        tB[i] = (sc > 1) ? rf(&r, -1.5f*dur, 1.5f*dur) : rf(&r, -5.0f, 5.0f);
        w[i]  = rf(&r, -0.2f, 1.2f);                                            /* clamped weights */
    }
    float mask[MAXJ];
    for (uint32_t j = 0; j < jc; ++j)
        mask[j] = (j == 4) ? 0.0f : (j == 3 ? 1.0f : rf(&r, 0.0f, 1.0f));

    mrw_mem_req sreq, oreq, oreq16;
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette_requirements(jc, N, MRW_PALETTE_F32, &sreq, &oreq) == MRW_OK);
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette_requirements(jc, N, MRW_PALETTE_F16, NULL, &oreq16) == MRW_OK);
    void *scratch = mrw_alloc64(sreq.size);
    /* scratch working set is O(joint_count·LANES), NOT per-instance */
    TASK_CHECK(sreq.size <= (size_t)jc * 24u * MRW_LANES * sizeof(float) + 128u);

    /* single-pose scratch for the looped oracle */
    mrw_mem_req psr; TASK_CHECK(mrw_pose_mesh_blend_requirements(jc, &psr) == MRW_OK);
    void *pscratch = mrw_alloc64(psr.size);

    static float ref[MAXJ*12*32];

    const float *masks[2] = { NULL, mask };
    for (int mi = 0; mi < 2; ++mi) {
        const float *mk = masks[mi];
        /* the looped single-pose oracle for THIS mask variant */
        for (uint32_t i = 0; i < N; ++i)
            TASK_CHECK(oracle_instance(&sv, &cA, &cB, tA[i], tB[i], w[i], mk,
                                       ref + (size_t)i*jc*12u, pscratch, psr.size, jc) == 0);
        for (int b = 0; b < nbk; ++b) {
            size_t cap = oreq.size + 64;
            uint8_t *ob = mrw_alloc64(cap); memset(ob, 0xCD, cap);
            float *out = (float *)ob;
            TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&bk[b].d, &sv, &cA, &cB, tA, tB, w, mk, N,
                          out, oreq.size, scratch, sreq.size) == MRW_OK);
            /* compare to the looped single-pose reference */
            for (uint32_t i = 0; i < N; ++i) for (uint32_t k = 0; k < jc*12u; ++k) {
                size_t idx = (size_t)i*jc*12u + k;
                double a = out[idx], e = ref[idx], d = fabs(a - e);
                double tol = 1e-4 + 1e-4*fabs(e);   /* short-skeleton f32 rel+abs bound (all backends) */

                TASK_CHECK(d <= tol);
            }
            for (size_t k = oreq.size; k < cap; ++k) TASK_CHECK(ob[k] == 0xCD);  /* no overrun */

            /* f16: consistency with the narrowed f32 (same backend) + decode error bound 2e-3 */
            uint8_t *o16 = mrw_alloc64(oreq16.size + 64); memset(o16, 0xCD, oreq16.size + 64);
            uint16_t *h = (uint16_t *)o16;
            TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette_f16(&bk[b].d, &sv, &cA, &cB, tA, tB, w, mk, N,
                          h, oreq16.size, scratch, sreq.size) == MRW_OK);
            for (uint32_t i = 0; i < N; ++i) for (uint32_t k = 0; k < jc*12u; ++k) {
                size_t idx = (size_t)i*jc*12u + k;
                TASK_CHECK(h[idx] == mrw_f32_to_f16(out[idx]));
                double d = fabs(mrw_half_to_float(h[idx]) - ref[idx]);
                TASK_CHECK(d <= 2e-3);
            }
            mrw_free(o16); mrw_free(ob);
        }
    }

    mrw_free((uint8_t*)pscratch); mrw_free((uint8_t*)scratch);
    free(tA); free(tB); free(w); mrw_free(buf);
    return 0;
}

static int case_batch_both_codecs(void) {
    TASK_CHECK(report_backends() == 0);
    /* codec 0 and 1, 9 and 17 instances, static/non-looping/looping */
    TASK_CHECK(run_batch_shape(12, 1, 0, 0, 11, 9) == 0);
    TASK_CHECK(run_batch_shape(16, 6, 0, 0, 12, 9) == 0);
    TASK_CHECK(run_batch_shape(16, 8, MRW_CLIP_LOOPING, 0, 13, 17) == 0);
    TASK_CHECK(run_batch_shape(12, 1, 0, 1, 14, 9) == 0);
    TASK_CHECK(run_batch_shape(16, 6, 0, 1, 15, 17) == 0);
    TASK_CHECK(run_batch_shape(16, 8, MRW_CLIP_LOOPING, 1, 16, 17) == 0);
    return 0;
}

/* Batch error / alias / tail / zero-count contracts. */
static int case_batch_contracts(void) {
    rig_t R; uint8_t *buf = NULL; size_t sz = 0;
    rig_build(&R, 12, 5, 0, 909, 0, &buf, &sz);
    mrw_blob blob; TASK_CHECK(mrw_blob_open(buf, sz, &blob) == MRW_OK);
    mrw_skeleton_view sv; TASK_CHECK(mrw_blob_skeleton(&blob, &sv) == MRW_OK);
    mrw_clip_view cA, cB;
    TASK_CHECK(mrw_clip_view_at(&blob, 1, &cA) == MRW_OK);
    TASK_CHECK(mrw_clip_view_at(&blob, 2, &cB) == MRW_OK);
    uint32_t jc = sv.joint_count;

    mrw_dispatch d; TASK_CHECK(mrw_dispatch_scalar(&d) == MRW_OK);
    mrw_mem_req sreq, oreq;
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette_requirements(jc, 9, MRW_PALETTE_F32, &sreq, &oreq) == MRW_OK);
    void *scratch = mrw_alloc64(sreq.size);
    uint8_t *ob = mrw_alloc64(oreq.size);
    float *out = (float *)ob;
    float t[9], tt[9], w[9];
    for (int i = 0; i < 9; ++i) { t[i] = 0.05f * i; tt[i] = 0.03f * i; w[i] = 0.1f * i; }

    /* zero instance is a no-op with NULL data */
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&d, &sv, &cA, &cB, NULL, NULL, NULL, NULL, 0,
                NULL, 0, NULL, 0) == MRW_OK);
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette_f16(&d, &sv, &cA, &cB, NULL, NULL, NULL, NULL, 0,
                NULL, 0, NULL, 0) == MRW_OK);
    /* NULL handles / data */
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(NULL, &sv, &cA, &cB, t, tt, w, NULL, 9,
                out, oreq.size, scratch, sreq.size) == MRW_E_RANGE);
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&d, &sv, NULL, &cB, t, tt, w, NULL, 9,
                out, oreq.size, scratch, sreq.size) == MRW_E_RANGE);
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&d, &sv, &cA, &cB, NULL, tt, w, NULL, 9,
                out, oreq.size, scratch, sreq.size) == MRW_E_RANGE);
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&d, &sv, &cA, &cB, t, tt, NULL, NULL, 9,
                out, oreq.size, scratch, sreq.size) == MRW_E_RANGE);
    /* bad dispatch */
    mrw_dispatch dbad = { MRW_BACKEND_AVX2, 0 };
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&dbad, &sv, &cA, &cB, t, tt, w, NULL, 9,
                out, oreq.size, scratch, sreq.size) == MRW_E_UNSUPPORTED);
    /* capacity / alignment */
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&d, &sv, &cA, &cB, t, tt, w, NULL, 9,
                out, oreq.size - 1, scratch, sreq.size) == MRW_E_CAPACITY);
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&d, &sv, &cA, &cB, t, tt, w, NULL, 9,
                out, oreq.size, scratch, sreq.size - 1) == MRW_E_CAPACITY);
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&d, &sv, &cA, &cB, t, tt, w, NULL, 9,
                (float*)((char*)ob + 8), oreq.size, scratch, sreq.size) == MRW_E_ALIGN);
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&d, &sv, &cA, &cB, t, tt, w, NULL, 9,
                out, oreq.size, (char*)scratch + 8, sreq.size) == MRW_E_ALIGN);
    /* non-finite time / weight / mask ⇒ RANGE, no write (canary) */
    memset(ob, 0xAB, oreq.size);
    float bt[9]; memcpy(bt, t, sizeof bt); bt[4] = (float)NAN;
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&d, &sv, &cA, &cB, bt, tt, w, NULL, 9,
                out, oreq.size, scratch, sreq.size) == MRW_E_RANGE);
    for (size_t k = 0; k < oreq.size; ++k) TASK_CHECK(ob[k] == 0xAB);
    float bw[9]; memcpy(bw, w, sizeof bw); bw[2] = (float)INFINITY;
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&d, &sv, &cA, &cB, t, tt, bw, NULL, 9,
                out, oreq.size, scratch, sreq.size) == MRW_E_RANGE);
    for (size_t k = 0; k < oreq.size; ++k) TASK_CHECK(ob[k] == 0xAB);
    float bm[MAXJ]; for (uint32_t j = 0; j < jc; ++j) bm[j] = 1.0f; bm[5] = (float)NAN;
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&d, &sv, &cA, &cB, t, tt, w, bm, 9,
                out, oreq.size, scratch, sreq.size) == MRW_E_RANGE);
    for (size_t k = 0; k < oreq.size; ++k) TASK_CHECK(ob[k] == 0xAB);

    /* requirements: NULL/NULL ⇒ RANGE; bad format ⇒ RANGE; overflow-checked sizes */
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette_requirements(jc, 9, MRW_PALETTE_F32, NULL, NULL) == MRW_E_RANGE);
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette_requirements(jc, 9, 7, &sreq, &oreq) == MRW_E_RANGE);
    mrw_mem_req so, oo;
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette_requirements(0xFFFFFFFFu, 0xFFFFFFFFu, MRW_PALETTE_F32, &so, &oo) == MRW_E_OVERFLOW);
    /* scratch size is independent of instance_count (tile-reused) */
    mrw_mem_req s1, s2, o1, o2;
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette_requirements(jc, 1, MRW_PALETTE_F32, &s1, &o1) == MRW_OK);
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette_requirements(jc, 1000, MRW_PALETTE_F32, &s2, &o2) == MRW_OK);
    TASK_CHECK(s1.size == s2.size);
    TASK_CHECK(o2.size > o1.size);

    /* incompatible: a clip from a different skeleton */
    rig_t R2; uint8_t *bx = NULL; size_t sx = 0;
    rig_build(&R2, 14, 5, 0, 910, 0, &bx, &sx);
    mrw_blob blx; TASK_CHECK(mrw_blob_open(bx, sx, &blx) == MRW_OK);
    mrw_clip_view cx; TASK_CHECK(mrw_clip_view_at(&blx, 1, &cx) == MRW_OK);
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&d, &sv, &cx, &cB, t, tt, w, NULL, 9,
                out, oreq.size, scratch, sreq.size) == MRW_E_INCOMPATIBLE);
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&d, &sv, &cA, &cx, t, tt, w, NULL, 9,
                out, oreq.size, scratch, sreq.size) == MRW_E_INCOMPATIBLE);

    /* tail instances (N=17) must match the oracle exactly on the last partial tile */
    mrw_free(ob); mrw_free((uint8_t*)scratch); mrw_free(bx); mrw_free(buf);
    TASK_CHECK(run_batch_shape(14, 5, 0, 0, 911, 17) == 0);
    return 0;
}

/* The single-pose mesh entry and the scalar batch path agree (bit-for-bit): batch instance i equals
 * looped single-pose for the same inputs. This cross-checks the two scalar implementations. */
static int case_scalar_batch_matches_single(void) {
    rig_t R; uint8_t *buf = NULL; size_t sz = 0;
    rig_build(&R, 12, 5, 0, 1010, 0, &buf, &sz);
    mrw_blob blob; TASK_CHECK(mrw_blob_open(buf, sz, &blob) == MRW_OK);
    mrw_skeleton_view sv; TASK_CHECK(mrw_blob_skeleton(&blob, &sv) == MRW_OK);
    mrw_clip_view cA, cB;
    TASK_CHECK(mrw_clip_view_at(&blob, 1, &cA) == MRW_OK);
    TASK_CHECK(mrw_clip_view_at(&blob, 2, &cB) == MRW_OK);
    uint32_t jc = sv.joint_count;

    mrw_dispatch d; TASK_CHECK(mrw_dispatch_scalar(&d) == MRW_OK);
    mrw_mem_req sreq, oreq;
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette_requirements(jc, 9, MRW_PALETTE_F32, &sreq, &oreq) == MRW_OK);
    void *scratch = mrw_alloc64(sreq.size);
    float *pal = (float *)mrw_alloc64(oreq.size);
    float tA[9], tB[9], w[9];
    rng_t r = { 55u };
    for (int i = 0; i < 9; ++i) { tA[i] = rf(&r,0,0.1f); tB[i] = rf(&r,0,0.1f); w[i] = rf(&r,-0.2f,1.2f); }
    float mask[MAXJ]; for (uint32_t j = 0; j < jc; ++j) mask[j] = (j == 4) ? 0.0f : rf(&r,0,1);
    TASK_CHECK(mrw_batch_mesh_blend_clips_to_palette(&d, &sv, &cA, &cB, tA, tB, w, mask, 9,
                pal, oreq.size, scratch, sreq.size) == MRW_OK);

    mrw_mem_req psr; TASK_CHECK(mrw_pose_mesh_blend_requirements(jc, &psr) == MRW_OK);
    void *ps = mrw_alloc64(psr.size);
    float *refp = (float *)mrw_alloc64(oreq.size);
    for (int i = 0; i < 9; ++i)
        TASK_CHECK(oracle_instance(&sv, &cA, &cB, tA[i], tB[i], w[i], mask, refp + (size_t)i*jc*12u, ps, psr.size, jc) == 0);
    for (size_t k = 0; k < (size_t)9*jc*12u; ++k) TASK_CHECK(pal[k] == refp[k]);

    mrw_free((uint8_t*)ps); mrw_free((uint8_t*)refp); mrw_free((uint8_t*)scratch);
    mrw_free((uint8_t*)pal); mrw_free(buf);
    return 0;
}

/* ===================================================================== registration */

const struct task_case task_cases[] = {
    { "hierarchy", "noncoaxial",        case_noncoaxial },
    { "hierarchy", "mask_divergence",   case_mask_divergence },
    { "hierarchy", "nonuniform_shear",  case_nonuniform_shear },
    { "hierarchy", "quat_sign",         case_quat_sign },
    { "hierarchy", "weight_endpoints",  case_weight_endpoints },
    { "hierarchy", "alias",             case_alias },
    { "batch",     "both_codecs",       case_batch_both_codecs },
    { "batch",     "contracts",         case_batch_contracts },
    { "batch",     "scalar_vs_single",  case_scalar_batch_matches_single },
    { "contracts", "single_contracts",  case_single_contracts },
    { "contracts", "legacy_unchanged",  case_legacy_unchanged },
};
const size_t task_case_count = sizeof task_cases / sizeof task_cases[0];
