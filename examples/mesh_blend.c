/*
 * marrow mesh_blend example - MODEL-SPACE rotation blending for a same-skeleton pair.
 *
 * Builds a tiny two-clip rig in code, produces a palette with the batch model-space mesh-blend entry
 * (mrw_batch_mesh_blend_clips_to_palette), then skins a small weighted mesh with that palette and
 * writes a Wavefront OBJ to <out_dir>/mesh_blend.obj. The mesh deliberately straddles a masked joint
 * (a mask=0 child under a blended parent), which is exactly the case where model-space blending and
 * the classic local-space blend give different directions.
 *
 *   mesh_blend <out_dir>
 *
 * No graphics device, no asset downloads: the rig is authored in memory through the checked
 * authoring library and the output is a plain OBJ text file.
 */
#include "marrow.h"
#include "mrw_authoring.h" /* mrw_authoring_build / _alloc / _free (offline .mrw emitter) */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JC 6u       /* joints: root, spine, chest, upper arm, forearm, hand */
#define SC 5u       /* clip samples */
#define FPS 30.0f
#define NINST 4u
#define NVERT 400

/* deterministic LCG so the example is bit-identical across runs */
typedef struct { uint32_t s; } rng;
static uint32_t ru32(rng *r) { r->s = r->s * 1664525u + 1013904223u; return r->s; }
static float rf(rng *r, float lo, float hi) {
    return lo + (hi - lo) * ((float)(ru32(r) >> 8) / (float)(1u << 24));
}
static void quat_axis_angle(const float ax[3], float ang, float q[4]) {
    float n = sqrtf(ax[0]*ax[0] + ax[1]*ax[1] + ax[2]*ax[2]);
    float s = sinf(ang * 0.5f);
    q[0] = ax[0]/n*s; q[1] = ax[1]/n*s; q[2] = ax[2]/n*s; q[3] = cosf(ang * 0.5f);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <out_dir>\n", argv[0]); return 1; }
    const char *out_dir = argv[1];

    /* ---- build a small character-scale rig (non-uniform bind scale => shear in the affine) ---- */
    static uint16_t parent[JC] = { 0xFFFFu, 0, 1, 2, 3, 4 };
    static float rest[JC*10];
    static const char *names[JC] = { "root", "spine", "chest", "upper_arm", "forearm", "hand" };
    static float ib[JC*12];
    {
        float y[3] = {0,1,0}, x[3] = {1,0,0};
        for (uint32_t j = 0; j < JC; ++j) {
            float q[4];
            if (j == 0) quat_axis_angle(y, 0.05f, q);
            else if (j == 3) quat_axis_angle(x, 0.6f, q);   /* arm off the chest, about X */
            else quat_axis_angle(y, 0.1f * (float)j, q);
            float sc = (j == 4) ? 1.4f : 1.0f;             /* forearm: non-uniform rest scale */
            rest[j*10+0] = q[0]; rest[j*10+1] = q[1]; rest[j*10+2] = q[2]; rest[j*10+3] = q[3];
            rest[j*10+4] = (j == 0) ? 0.0f : 0.22f;
            rest[j*10+5] = (j == 0) ? 0.0f : 0.03f;
            rest[j*10+6] = 0.0f;
            rest[j*10+7] = (j == 4) ? sc : 1.0f;
            rest[j*10+8] = (j == 4) ? 0.75f : 1.0f;
            rest[j*10+9] = 1.0f;
        }
    }
    /* inverse-bind from the rest pose: fully in the example (no runtime helper needed) */
    {
        static float model[JC*12];
        for (uint32_t j = 0; j < JC; ++j) {
            mrw_trs t;
            memcpy(t.rot,   rest + j*10 + 0, 16);
            memcpy(t.trans, rest + j*10 + 4, 12);
            memcpy(t.scale, rest + j*10 + 7, 12);
            float lm[12]; mrw_trs_to_affine(&t, lm);
            if (j == 0) memcpy(model, lm, sizeof lm);
            else mrw_affine_mul(model + (size_t)parent[j]*12, lm, model + (size_t)j*12);
            /* invert the 3x4 affine */
            const float *m = model + (size_t)j*12;
            float a[9] = { m[0],m[1],m[2], m[4],m[5],m[6], m[8],m[9],m[10] };
            double det = (double)a[0]*(a[4]*a[8]-a[5]*a[7])
                       - (double)a[1]*(a[3]*a[8]-a[5]*a[6])
                       + (double)a[2]*(a[3]*a[7]-a[4]*a[6]);
            float cof[9]; mrw_cofactor3(a, cof);
            float inv[9];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) inv[3*r+c] = (float)(cof[3*c+r] / det);
            float tt[3] = { m[3], m[7], m[11] };
            for (int r = 0; r < 3; ++r) {
                ib[j*12 + r*4 + 0] = inv[r*3+0];
                ib[j*12 + r*4 + 1] = inv[r*3+1];
                ib[j*12 + r*4 + 2] = inv[r*3+2];
                ib[j*12 + r*4 + 3] = -(inv[r*3+0]*tt[0] + inv[r*3+1]*tt[1] + inv[r*3+2]*tt[2]);
            }
        }
    }

    /* ---- two small clips (identity-ish and a raised arm), same skeleton ---- */
    static float sA[JC*SC*10], sB[JC*SC*10];
    {
        rng r = { 4242u };
        float y[3] = {0,1,0}, x[3] = {1,0,0};
        for (uint32_t j = 0; j < JC; ++j) {
            for (uint32_t s = 0; s < SC; ++s) {
                float *pa = sA + ((size_t)j*SC + s)*10;
                float *pb = sB + ((size_t)j*SC + s)*10;
                float qa[4], qb[4];
                if (j == 1) { quat_axis_angle(y, 0.15f * (float)s, qa); quat_axis_angle(y, 0.05f * (float)s, qb); }
                else if (j == 3) { quat_axis_angle(x, 0.05f * (float)s, qa); quat_axis_angle(x, 0.55f * (float)s, qb); }
                else { quat_axis_angle(y, 0.02f * (float)s, qa); quat_axis_angle(x, 0.02f * (float)s, qb); }
                for (int k = 0; k < 4; ++k) { pa[k] = qa[k]; pb[k] = qb[k]; }
                pa[4] = rf(&r, -0.02f, 0.02f); pa[5] = 0.0f; pa[6] = 0.0f;
                pb[4] = rf(&r, -0.02f, 0.02f); pb[5] = 0.0f; pb[6] = 0.0f;
                pa[7] = pa[8] = pa[9] = 1.0f;
                pb[7] = pb[8] = pb[9] = 1.0f;
            }
        }
    }

    mrw_skel skel = { JC, parent, rest, ib, names };
    mrw_clip clips[2] = { { FPS, SC, 0, sA, NULL, 0 }, { FPS, SC, 0, sB, NULL, 0 } };
    uint8_t *blob_buf = NULL;
    size_t blob_sz = 0;
    if (mrw_authoring_build(&skel, clips, 2, NULL, &blob_buf, &blob_sz) != MRW_OK) {
        fprintf(stderr, "example: could not author the rig blob\n");
        return 1;
    }

    mrw_blob blob;
    if (mrw_blob_open(blob_buf, blob_sz, &blob) != MRW_OK) {
        fprintf(stderr, "example: blob failed to open\n");
        mrw_authoring_free(blob_buf);
        return 1;
    }
    mrw_skeleton_view sv;
    mrw_clip_view cA, cB;
    if (mrw_blob_skeleton(&blob, &sv) != MRW_OK ||
        mrw_clip_view_at(&blob, 1, &cA) != MRW_OK ||
        mrw_clip_view_at(&blob, 2, &cB) != MRW_OK) {
        fprintf(stderr, "example: missing skeleton/clips\n");
        mrw_authoring_free(blob_buf);
        return 1;
    }

    /* ---- batch model-space mesh blend: NINST instances, mask zeroes the forearm so it keeps clip
     * A's model-space direction while the chest (its ancestor) blends toward B ---- */
    mrw_dispatch disp;
    if (mrw_dispatch_detect(&disp) != MRW_OK) mrw_dispatch_scalar(&disp);

    mrw_mem_req screq, palreq;
    if (mrw_batch_mesh_blend_clips_to_palette_requirements(JC, NINST, MRW_PALETTE_F32,
                                                           &screq, &palreq) != MRW_OK) {
        fprintf(stderr, "example: requirements failed\n"); mrw_authoring_free(blob_buf); return 1;
    }
    void *scratch = mrw_authoring_alloc(screq.size);
    float *palette = (float *)mrw_authoring_alloc(palreq.size);
    float timesA[NINST], timesB[NINST], weights[NINST], mask[JC];
    float duration = (SC > 1) ? (float)(SC - 1) / FPS : 0.0f;
    for (uint32_t i = 0; i < NINST; ++i) {
        timesA[i] = duration * ((float)i / (float)(NINST - 1));
        timesB[i] = duration * (1.0f - (float)i / (float)(NINST - 1));
        weights[i] = (float)i / (float)(NINST - 1);
    }
    for (uint32_t j = 0; j < JC; ++j) mask[j] = (j == 4) ? 0.0f : 1.0f;  /* forearm masked out */

    mrw_result rc = mrw_batch_mesh_blend_clips_to_palette(
        &disp, &sv, &cA, &cB, timesA, timesB, weights, mask, NINST,
        palette, palreq.size, scratch, screq.size);
    if (rc != MRW_OK) {
        fprintf(stderr, "example: mesh blend failed (%u)\n", rc);
        mrw_authoring_free(palette); mrw_authoring_free(scratch); mrw_authoring_free(blob_buf);
        return 1;
    }

    /* ---- a small weighted mesh: a limb capsule around the arm chain, 1–2 influences ---- */
    static float vx[NVERT*3], vn[NVERT*3], vw[NVERT*2], vinst[NVERT];
    static uint32_t vj[NVERT*2];
    {
        rng r = { 777u };
        for (int i = 0; i < NVERT; ++i) {
            float u = rf(&r, 0.0f, 1.0f);              /* along the arm */
            float ang = rf(&r, 0.0f, 6.2831853f), rad = rf(&r, 0.05f, 0.12f);
            vx[i*3+0] = 0.22f + 0.55f * u + rad * cosf(ang);
            vx[i*3+1] = rad * sinf(ang);
            vx[i*3+2] = 0.05f * sinf(ang * 0.5f);
            float nx = 0.0f, ny = cosf(ang), nz = sinf(ang);
            float nl = sqrtf(nx*nx + ny*ny + nz*nz); if (nl < 1e-6f) nl = 1;
            vn[i*3+0] = nx/nl; vn[i*3+1] = ny/nl; vn[i*3+2] = nz/nl;
            /* weight the upper arm / forearm / hand by position along the limb */
            uint32_t j0 = (u < 0.5f) ? 3u : 4u;
            uint32_t j1 = (u < 0.5f) ? 4u : 5u;
            float w0 = 1.0f - fabsf(u - ((u < 0.5f) ? 0.25f : 0.75f)) * 1.2f;
            if (w0 < 0.2f) w0 = 0.2f; if (w0 > 0.9f) w0 = 0.9f;
            vj[i*2+0] = j0; vj[i*2+1] = j1;
            vw[i*2+0] = w0;  vw[i*2+1] = 1.0f - w0;
            vinst[i] = (float)(i % NINST);           /* each vertex reads one instance's palette */
        }
    }

    /* ---- LBS-skin every vertex with its instance palette, write an OBJ (the instance chosen by
     * the radius sweep above is folded into z so the four poses are visible side by side) ---- */
    char path[4096];
    snprintf(path, sizeof path, "%s/mesh_blend.obj", out_dir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "example: cannot write %s\n", path);
        mrw_authoring_free(palette); mrw_authoring_free(scratch); mrw_authoring_free(blob_buf);
        return 1;
    }
    fprintf(f, "# marrow model-space mesh blend example\n");
    fprintf(f, "# joints=%u instances=%u vertices=%d\n", sv.joint_count, NINST, NVERT);
    for (int i = 0; i < NVERT; ++i) {
        uint32_t inst = (uint32_t)vinst[i];
        const float *pal = palette + (size_t)inst * JC * 12u;
        double px = 0, py = 0, pz = 0;
        for (int k = 0; k < 2; ++k) {
            const float *M = pal + (size_t)vj[i*2+k] * 12u;
            double w = vw[i*2+k];
            px += w * (M[0]*vx[i*3+0] + M[1]*vx[i*3+1] + M[2]*vx[i*3+2] + M[3]);
            py += w * (M[4]*vx[i*3+0] + M[5]*vx[i*3+1] + M[6]*vx[i*3+2] + M[7]);
            pz += w * (M[8]*vx[i*3+0] + M[9]*vx[i*3+1] + M[10]*vx[i*3+2] + M[11]);
        }
        /* place the four instances apart along +Z so the OBJ shows all of them */
        pz += 1.2 * (double)inst;
        fprintf(f, "v %.6f %.6f %.6f\n", px, py, pz);
    }
    for (int i = 0; i < NVERT; ++i) {
        uint32_t inst = (uint32_t)vinst[i];
        const float *pal = palette + (size_t)inst * JC * 12u;
        double b[9] = {0};
        for (int k = 0; k < 2; ++k) {
            const float *M = pal + (size_t)vj[i*2+k] * 12u;
            double w = vw[i*2+k];
            b[0]+=w*M[0]; b[1]+=w*M[1]; b[2]+=w*M[2];
            b[3]+=w*M[4]; b[4]+=w*M[5]; b[5]+=w*M[6];
            b[6]+=w*M[8]; b[7]+=w*M[9]; b[8]+=w*M[10];
        }
        double nx = b[0]*vn[i*3+0]+b[1]*vn[i*3+1]+b[2]*vn[i*3+2];
        double ny = b[3]*vn[i*3+0]+b[4]*vn[i*3+1]+b[5]*vn[i*3+2];
        double nz = b[6]*vn[i*3+0]+b[7]*vn[i*3+1]+b[8]*vn[i*3+2];
        double nl = sqrt(nx*nx+ny*ny+nz*nz); if (nl < 1e-12) nl = 1;
        fprintf(f, "vn %.6f %.6f %.6f\n", nx/nl, ny/nl, nz/nl);
    }
    /* a simple triangle strip along the capsule for a viewable surface */
    for (int i = 0; i + 1 < NVERT; ++i) {
        if (i % 2 == 0) fprintf(f, "f %d//%d %d//%d %d//%d\n", i+1, i+1, i+2, i+2, i+3, i+3);
    }
    fclose(f);

    printf("backends available:");
    {
        mrw_dispatch t;
        if (mrw_dispatch_scalar(&t) == MRW_OK) printf(" scalar");
        if (mrw_dispatch_sse2(&t)  == MRW_OK) printf(" sse2");
        if (mrw_dispatch_avx2(&t)  == MRW_OK) printf(" avx2");
    }
    printf("\n");
    printf("wrote %s (joints=%u, instances=%u, vertices=%d, scratch=%zu B, palette=%zu B)\n",
           path, sv.joint_count, NINST, NVERT, screq.size, palreq.size);

    mrw_authoring_free(palette);
    mrw_authoring_free(scratch);
    mrw_authoring_free(blob_buf);
    return 0;
}
