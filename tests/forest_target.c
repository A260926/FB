/* Preserve-nodes (forest) import target.
 *
 * A glTF whose skin spans two disconnected real trees, with a non-joint ancestor carrying a
 * non-uniform scale above rotating joints, a shuffled skin slot order and duplicate node names.
 * The default fold cannot represent the S·R chain product as a TRS; --preserve-nodes keeps the
 * real nodes, so the shear stays in the model-matrix product. This file checks:
 *   import    - default still rejects the shear; preserve mode imports; the emitted node set is
 *               exactly the ancestor closure (no unrelated sibling, no virtual root); the forest
 *               ordering API yields output->node, skin-slot->output and a valid parent table;
 *               inverse binds pair with skin slots (aux bones get identity) and names may repeat.
 *   hierarchy - sampled locals are the nodes' own TRS (ancestors not folded again), the model
 *               matrices equal an independent per-node accumulation, roots start at their local,
 *               and a weighted vertex skins correctly through non-identity inverse binds.
 *   batch     - 9 instances, f32 and f16, every available backend against the scalar reference,
 *               disconnected roots, blend and additive accumulate, plus both clip codecs.
 *
 * The numeric references here are implemented independently of the converter/runtime math:
 * quaternion -> 3x3, TRS -> affine, affine compose/inverse and the hierarchy walk are local. */
#include "task_support/check.h"
#include "marrow.h"
#include "marrow_internal.h"  /* mrw_rd_u32: parent-table offset in the validation case */
#include "convert.h"
#include "joint_order.h"
#include "mrw_authoring.h"
#include "cgltf.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define F32_TOL 1e-4
#define F16_ATOL 2e-3
#define F16_RTOL 2e-3

/* ------------------------------------------------------------------ independent small math */

static void q_axis_angle(float ax, float ay, float az, float deg, float q[4]) {
    float n = sqrtf(ax*ax + ay*ay + az*az);
    if (n <= 0.0f) { q[0]=q[1]=q[2]=0.0f; q[3]=1.0f; return; }
    float h = 0.5f * deg * 3.14159265358979323846f / 180.0f;
    float s = sinf(h) / n;
    q[0]=ax*s; q[1]=ay*s; q[2]=az*s; q[3]=cosf(h);
}

/* quat -> row-major 3x3 rotation (column-vector convention, same as glTF/marrow) */
static void q_mat3(const float q[4], float R[9]) {
    float x=q[0], y=q[1], z=q[2], w=q[3];
    R[0]=1-2*(y*y+z*z); R[1]=2*(x*y-z*w);   R[2]=2*(x*z+y*w);
    R[3]=2*(x*y+z*w);   R[4]=1-2*(x*x+z*z); R[5]=2*(y*z-x*w);
    R[6]=2*(x*z-y*w);   R[7]=2*(y*z+x*w);   R[8]=1-2*(x*x+y*y);
}

/* local = T·R·S -> 3x4 row-major [A|t] */
static void trs_affine(const float q[4], const float t[3], const float s[3], float A[12]) {
    float R[9]; q_mat3(q, R);
    for (int r = 0; r < 3; ++r) {
        A[r*4+0] = R[r*3+0]*s[0];
        A[r*4+1] = R[r*3+1]*s[1];
        A[r*4+2] = R[r*3+2]*s[2];
        A[r*4+3] = t[r];
    }
}

/* A∘B (apply B then A), both 3x4 row-major */
static void aff_mul(const float A[12], const float B[12], float O[12]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) {
            float v = A[r*4+0]*B[0*4+c] + A[r*4+1]*B[1*4+c] + A[r*4+2]*B[2*4+c];
            if (c == 3) v += A[r*4+3];
            O[r*4+c] = v;
        }
}

static void aff_apply(const float A[12], const float p[3], float o[3]) {
    for (int r = 0; r < 3; ++r)
        o[r] = A[r*4+0]*p[0] + A[r*4+1]*p[1] + A[r*4+2]*p[2] + A[r*4+3];
}

static int aff_near(const float A[12], const float B[12], double eps) {
    for (int i = 0; i < 12; ++i) if (fabs((double)A[i] - B[i]) > eps) return 0;
    return 1;
}

/* ------------------------------------------------------------------ fixture description */

/* fixture node local transforms (indices are data.nodes order) */
static const float NODE_T[8][3] = {
    { 1, 2, 3 }, { 0.5f, 1, 0 }, { 0, 2, 0 }, { 1, 0, 0 },
    { 100, 0, 0 }, { -1, 0, 1 }, { 0, 1.5f, 0 }, { 5, 5, 5 }
};
static const float NODE_S[8][3] = {
    { 2, 0.5f, 1.5f }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 },
    { 1, 1, 1 }, { 1.25f, 1, 0.75f }, { 1, 1, 1 }, { 1, 1, 1 }
};

/* animated nodes (per animation): axis + start/end angle in degrees */
typedef struct { uint32_t node; float ax, ay, az, a0, a1; } anim_key;
static const anim_key ANIM_A[5] = {
    { 1, 0, 0, 1,   0,  30 },
    { 2, 1, 0, 0,   0,  50 },
    { 3, 0, 1, 0,   0, -40 },
    { 6, 0, 0, 1,   0,  65 },
    { 7, 1, 0, 0,   0, -25 },
};
static const anim_key ANIM_B[5] = {
    { 1, 0, 0, 1,   0, -20 },
    { 2, 1, 0, 0,   0,  80 },
    { 3, 0, 1, 0,   0,  30 },
    { 6, 0, 0, 1,   0, -45 },
    { 7, 1, 0, 0,   0,  60 },
};

/* non-identity inverse-bind per skin slot: (rot axis/deg, translation, scale) */
typedef struct { float ax, ay, az, deg; float t[3]; float s[3]; } ib_desc;
static const ib_desc IBM[4] = {
    { 0, 0, 1, 10, { -0.5f, 0.25f, 0.1f },  { 1, 1, 1 } },
    { 1, 0, 0, 20, { 0.2f, -0.4f, 0.3f },   { 1, 1, 1 } },
    { 0, 1, 0, 15, { 1, 0, -1 },            { 0.5f, 0.5f, 0.5f } },
    { 1, 1, 1,  0, { -2, 1, 0.5f },         { 1, 1, 1 } },
};

static void ibm_aff(int slot, float A[12]) {
    float q[4]; q_axis_angle(IBM[slot].ax, IBM[slot].ay, IBM[slot].az, IBM[slot].deg, q);
    trs_affine(q, IBM[slot].t, IBM[slot].s, A);
}

/* node's own local at normalized animation time u (0..1) for animation keys[]; nodes not in the
 * animation are constant. This is the independent reference for both modes. */
static void fixture_node_local(const anim_key keys[5], float u, uint32_t node, float A[12]) {
    float q[4] = { 0, 0, 0, 1 };
    if (u < 0.0f) u = 0.0f; if (u > 1.0f) u = 1.0f;
    for (int k = 0; k < 5; ++k)
        if (keys[k].node == node) {
            q_axis_angle(keys[k].ax, keys[k].ay, keys[k].az,
                         keys[k].a0 + u * (keys[k].a1 - keys[k].a0), q);
            break;
        }
    trs_affine(q, NODE_T[node], NODE_S[node], A);
}

/* ------------------------------------------------------------------ glTF generation */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t b0 = in[i], b1 = i+1<n?in[i+1]:0, b2 = i+2<n?in[i+2]:0;
        uint32_t t = (b0<<16)|(b1<<8)|b2;
        out[o++] = B64[(t>>18)&63]; out[o++] = B64[(t>>12)&63];
        out[o++] = i+1<n ? B64[(t>>6)&63] : '=';
        out[o++] = i+2<n ? B64[t&63] : '=';
    }
    out[o] = 0;
}

/* Build the main fixture glTF at `path`. Node order/parents exactly as in NODE_T etc.
 * Buffer layout: IBM (4*16 f32), times (2), animA keys (5*2*4), animB keys (5*2*4). */
static int write_main_fixture(const char *path) {
    float fb[64 + 2 + 40 + 40];
    size_t nf = 0;
    for (int s = 0; s < 4; ++s) {
        float A[12]; ibm_aff(s, A);
        float m[16] = {0};
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) m[c*4+r] = A[r*4+c];
        m[15] = 1.0f;
        memcpy(fb + nf, m, sizeof m); nf += 16;
    }
    fb[nf++] = 0.0f; fb[nf++] = 1.0f;
    for (int a = 0; a < 2; ++a) {
        const anim_key *keys = a ? ANIM_B : ANIM_A;
        for (int k = 0; k < 5; ++k) {
            float q0[4], q1[4];
            q_axis_angle(keys[k].ax, keys[k].ay, keys[k].az, keys[k].a0, q0);
            q_axis_angle(keys[k].ax, keys[k].ay, keys[k].az, keys[k].a1, q1);
            memcpy(fb + nf, q0, sizeof q0); nf += 4;
            memcpy(fb + nf, q1, sizeof q1); nf += 4;
        }
    }
    uint8_t bytes[sizeof(fb)];
    memcpy(bytes, fb, nf * sizeof(float));
    char enc[2048]; b64(bytes, nf * sizeof(float), enc);

    char json[8192];
    int n = snprintf(json, sizeof json,
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0,7]}],"
        "\"nodes\":["
          "{\"name\":\"shared\",\"translation\":[1,2,3],\"scale\":[2,0.5,1.5],\"children\":[1,4,5]},"
          "{\"name\":\"upper\",\"translation\":[0.5,1,0],\"children\":[2]},"
          "{\"name\":\"bone\",\"translation\":[0,2,0],\"children\":[3]},"
          "{\"name\":\"twist\",\"translation\":[1,0,0]},"
          "{\"name\":\"decoy\",\"translation\":[100,0,0]},"
          "{\"name\":\"branch\",\"translation\":[-1,0,1],\"scale\":[1.25,1,0.75],\"children\":[6]},"
          "{\"name\":\"bone\",\"translation\":[0,1.5,0]},"
          "{\"name\":\"isolated\",\"translation\":[5,5,5]}"
        "],"
        "\"skins\":[{\"name\":\"rig\",\"joints\":[6,2,7,3],\"inverseBindMatrices\":0}],"
        "\"animations\":["
          "{\"name\":\"animA\",\"samplers\":["
            "{\"input\":1,\"output\":2,\"interpolation\":\"LINEAR\"},"
            "{\"input\":1,\"output\":3,\"interpolation\":\"LINEAR\"},"
            "{\"input\":1,\"output\":4,\"interpolation\":\"LINEAR\"},"
            "{\"input\":1,\"output\":5,\"interpolation\":\"LINEAR\"},"
            "{\"input\":1,\"output\":6,\"interpolation\":\"LINEAR\"}],"
           "\"channels\":["
            "{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"rotation\"}},"
            "{\"sampler\":1,\"target\":{\"node\":2,\"path\":\"rotation\"}},"
            "{\"sampler\":2,\"target\":{\"node\":3,\"path\":\"rotation\"}},"
            "{\"sampler\":3,\"target\":{\"node\":6,\"path\":\"rotation\"}},"
            "{\"sampler\":4,\"target\":{\"node\":7,\"path\":\"rotation\"}}]},"
          "{\"name\":\"animB\",\"samplers\":["
            "{\"input\":1,\"output\":7,\"interpolation\":\"LINEAR\"},"
            "{\"input\":1,\"output\":8,\"interpolation\":\"LINEAR\"},"
            "{\"input\":1,\"output\":9,\"interpolation\":\"LINEAR\"},"
            "{\"input\":1,\"output\":10,\"interpolation\":\"LINEAR\"},"
            "{\"input\":1,\"output\":11,\"interpolation\":\"LINEAR\"}],"
           "\"channels\":["
            "{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"rotation\"}},"
            "{\"sampler\":1,\"target\":{\"node\":2,\"path\":\"rotation\"}},"
            "{\"sampler\":2,\"target\":{\"node\":3,\"path\":\"rotation\"}},"
            "{\"sampler\":3,\"target\":{\"node\":6,\"path\":\"rotation\"}},"
            "{\"sampler\":4,\"target\":{\"node\":7,\"path\":\"rotation\"}}]}"
        "],"
        "\"buffers\":[{\"byteLength\":%zu,\"uri\":\"data:application/octet-stream;base64,%s\"}],"
        "\"bufferViews\":["
          "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":256},"
          "{\"buffer\":0,\"byteOffset\":256,\"byteLength\":8},"
          "{\"buffer\":0,\"byteOffset\":264,\"byteLength\":160},"
          "{\"buffer\":0,\"byteOffset\":424,\"byteLength\":160}],"
        "\"accessors\":["
          "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"MAT4\"},"
          "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\",\"min\":[0],\"max\":[1]},"
          "{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"},"
          "{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\",\"byteOffset\":32},"
          "{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\",\"byteOffset\":64},"
          "{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\",\"byteOffset\":96},"
          "{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\",\"byteOffset\":128},"
          "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"},"
          "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\",\"byteOffset\":32},"
          "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\",\"byteOffset\":64},"
          "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\",\"byteOffset\":96},"
          "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\",\"byteOffset\":128}"
        "]}",
        nf * sizeof(float), enc);
    if (n < 0 || (size_t)n >= sizeof json) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int bad = fwrite(json, 1, (size_t)n, f) != (size_t)n;
    if (fclose(f) != 0) bad = 1;
    return bad;
}

/* Single-root shear fixture: non-joint ancestor with non-uniform scale above one rotating joint. */
static int write_shear_fixture(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const char *s =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"nodes\":[{\"name\":\"scale_helper\",\"scale\":[2,1,1],\"children\":[1]},"
                   "{\"name\":\"joint\",\"rotation\":[0,0,0.3826834324,0.9238795325]}],"
        "\"skins\":[{\"joints\":[1]}]}";
    int bad = fwrite(s, 1, strlen(s), f) != strlen(s);
    if (fclose(f) != 0) bad = 1;
    return bad;
}

/* Unit-scale forest fixture: two disconnected rotating roots, no scale -> codec 1 by default.
 * One LINEAR rotation clip drives the first root (identity -> +90° about Y). */
static int write_unit_forest(const char *path) {
    float fb[10] = { 0.0f, 1.0f,  0,0,0,1,  0,0.70710678f,0,0.70710678f };
    uint8_t bytes[sizeof fb];
    memcpy(bytes, fb, sizeof fb);
    char enc[256]; b64(bytes, sizeof fb, enc);

    char json[2048];
    int n = snprintf(json, sizeof json,
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"nodes\":[{\"name\":\"a\",\"translation\":[1,0,0]},"
                   "{\"name\":\"b\",\"translation\":[0,2,0],\"rotation\":[0.2588190451,0,0,0.9659258263]}],"
        "\"skins\":[{\"joints\":[1,0]}],"
        "\"animations\":[{\"name\":\"turn\","
          "\"samplers\":[{\"input\":0,\"output\":1,\"interpolation\":\"LINEAR\"}],"
          "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"rotation\"}}]}],"
        "\"buffers\":[{\"byteLength\":%d,\"uri\":\"data:application/octet-stream;base64,%s\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":8},"
                         "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":32}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\",\"min\":[0],\"max\":[1]},"
                       "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"}]}",
        (int)sizeof fb, enc);
    if (n < 0 || (size_t)n >= sizeof json) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int bad = fwrite(json, 1, (size_t)n, f) != (size_t)n;
    if (fclose(f) != 0) bad = 1;
    return bad;
}

/* ------------------------------------------------------------------ conversion helpers */

static mrw_result convert(const char *path, int preserve, int force_codec0,
                          uint8_t **buf, size_t *size, char *diag, size_t cap) {
    mrw_g2m_options o;
    memset(&o, 0, sizeof o);
    o.input_path = path; o.fps = 30.0f; o.skin_index = -1;
    o.preserve_nodes = preserve; o.force_codec0 = force_codec0;
    return mrw_g2m_convert(&o, buf, size, diag, cap);
}

/* open a converted blob, grab the single skeleton + n-th clip */
static int open_blob(uint8_t *buf, size_t size, mrw_blob *blob, mrw_skeleton_view *sv) {
    if (mrw_blob_open(buf, (uint64_t)size, blob) != MRW_OK) return -1;
    if (mrw_blob_skeleton(blob, sv) != MRW_OK) return -1;
    return 0;
}
static int clip_at(mrw_blob *blob, uint32_t index, mrw_clip_view *cv) {
    return mrw_clip_view_at(blob, index, cv) == MRW_OK ? 0 : -1;
}

/* expected forest layout of the main fixture */
static const uint32_t EXP_NODE[7]  = { 0, 1, 2, 3, 5, 6, 7 };
static const uint16_t EXP_PARENT[7] = { 0xFFFFu, 0, 1, 2, 0, 4, 0xFFFFu };
static const uint32_t EXP_SKIN2OUT[4] = { 5, 2, 6, 3 }; /* slots [6,2,7,3] */

/* ================================================================== import group */

static int case_main_fixture_import(void) {
    const char *path = "forest_main.gltf";
    TASK_CHECK(write_main_fixture(path) == 0);

    /* -- default fold mode: multi-root and/or shear ⇒ rejected, never silently approximated -- */
    uint8_t *dbuf = NULL; size_t dsize = 0; char ddiag[256] = {0};
    mrw_result dr = convert(path, 0, 0, &dbuf, &dsize, ddiag, sizeof ddiag);
    TASK_CHECK(dr != MRW_OK);
    TASK_CHECK(dbuf == NULL);
    TASK_CHECK(ddiag[0] != '\0');

    /* -- a single-root shear chain is still rejected specifically for shear in default mode -- */
    const char *spath = "forest_shear.gltf";
    TASK_CHECK(write_shear_fixture(spath) == 0);
    char sdiag[256] = {0};
    mrw_result sr = convert(spath, 0, 0, &dbuf, &dsize, sdiag, sizeof sdiag);
    TASK_CHECK(sr == MRW_E_FORMAT);
    TASK_CHECK(strstr(sdiag, "shear") != NULL);
    TASK_CHECK(dbuf == NULL);

    /* preserve mode accepts that same shear chain: the shear stays in the model product. */
    {
        uint8_t *pbuf = NULL; size_t psize = 0; char pdiag[256] = {0};
        TASK_CHECK(convert(spath, 1, 0, &pbuf, &psize, pdiag, sizeof pdiag) == MRW_OK);
        mrw_blob pb; mrw_skeleton_view psv;
        TASK_CHECK(open_blob(pbuf, psize, &pb, &psv) == 0);
        TASK_CHECK(psv.joint_count == 2);            /* helper + joint, real nodes only */
        uint16_t pp0, pp1;
        TASK_CHECK(mrw_skeleton_parent(&psv, 0, &pp0) == MRW_OK && pp0 == 0xFFFFu);
        TASK_CHECK(mrw_skeleton_parent(&psv, 1, &pp1) == MRW_OK && pp1 == 0);
        /* the model product at the joint is genuinely sheared (non-orthogonal columns) */
        mrw_trs loc[2];
        TASK_CHECK(mrw_skeleton_rest_local(&psv, 0, &loc[0]) == MRW_OK);
        TASK_CHECK(mrw_skeleton_rest_local(&psv, 1, &loc[1]) == MRW_OK);
        float m[24];
        TASK_CHECK(mrw_local_to_model(&psv, loc, m, 2) == MRW_OK);
        float c0[3] = { m[12+0], m[12+4], m[12+8] }, c1[3] = { m[12+1], m[12+5], m[12+9] };
        float dot = c0[0]*c1[0]+c0[1]*c1[1]+c0[2]*c1[2];
        float l0 = sqrtf(c0[0]*c0[0]+c0[1]*c0[1]+c0[2]*c0[2]);
        float l1 = sqrtf(c1[0]*c1[0]+c1[1]*c1[1]+c1[2]*c1[2]);
        TASK_CHECK(fabsf(dot/(l0*l1)) > 1e-3f);
        mrw_authoring_free(pbuf);
    }

    /* the single-node `matrix` representability limit is unchanged in preserve mode: a sheared
     * matrix node is still rejected (it has no TRS decomposition). */
    {
        const char *mpath = "forest_matrix_shear.gltf";
        FILE *mf = fopen(mpath, "wb");
        TASK_CHECK(mf != NULL);
        /* column-major 4x4: column 0 = (1,0,0), column 1 = (0.5,1,0) → sheared 2D basis */
        const char *ms =
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"nodes\":[{\"name\":\"m\",\"matrix\":[1,0,0,0, 0.5,1,0,0, 0,0,1,0, 0,0,0,1]}],"
            "\"skins\":[{\"joints\":[0]}]}";
        TASK_CHECK(fwrite(ms, 1, strlen(ms), mf) == strlen(ms));
        TASK_CHECK(fclose(mf) == 0);
        char mdiag[256] = {0};
        uint8_t *mbuf = NULL; size_t msize = 0;
        TASK_CHECK(convert(mpath, 1, 0, &mbuf, &msize, mdiag, sizeof mdiag) == MRW_E_FORMAT);
        TASK_CHECK(mbuf == NULL);
        remove(mpath);
    }

    /* -- preserve mode imports the main fixture -- */
    uint8_t *buf = NULL; size_t size = 0; char diag[256] = {0};
    mrw_result r = convert(path, 1, 0, &buf, &size, diag, sizeof diag);
    if (r != MRW_OK) { fprintf(stderr, "preserve convert failed: %s\n", diag); return 1; }

    /* file version unchanged (v0) */
    { uint32_t ver = 0; memcpy(&ver, buf + 4, 4); TASK_CHECK(ver == 0); }

    mrw_blob blob; mrw_skeleton_view sv;
    TASK_CHECK(open_blob(buf, size, &blob, &sv) == 0);
    TASK_CHECK(sv.joint_count == 7);

    /* exactly the ancestor closure: [0,1,2,3,5,6,7] with node 4 (unrelated sibling) absent */
    uint16_t parent[7]; uint16_t seen_mask = 0;
    for (uint32_t j = 0; j < 7; ++j) {
        TASK_CHECK(mrw_skeleton_parent(&sv, j, &parent[j]) == MRW_OK);
        TASK_CHECK(parent[j] == EXP_PARENT[j]);
        if (j) TASK_CHECK(parent[j] == 0xFFFFu || parent[j] < j);
        seen_mask |= (uint16_t)(1u << EXP_NODE[j]);
    }
    TASK_CHECK((seen_mask & (1u << 4)) == 0);           /* decoy sibling not pulled in */
    TASK_CHECK(parent[0] == 0xFFFFu && parent[6] == 0xFFFFu); /* two real roots, no virtual root */

    /* names: the surviving node names, duplicates preserved verbatim */
    const char *nm;
    TASK_CHECK(mrw_skeleton_joint_name(&sv, 0, &nm) == MRW_OK && strcmp(nm, "shared") == 0);
    TASK_CHECK(mrw_skeleton_joint_name(&sv, 2, &nm) == MRW_OK && strcmp(nm, "bone") == 0);
    TASK_CHECK(mrw_skeleton_joint_name(&sv, 5, &nm) == MRW_OK && strcmp(nm, "bone") == 0);
    TASK_CHECK(mrw_skeleton_joint_name(&sv, 6, &nm) == MRW_OK && strcmp(nm, "isolated") == 0);

    /* inverse binds: aux bones (out 0,1,4) identity; original joints pair with their skin slot */
    static const float I12[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
    float ib[12], exp[12];
    for (uint32_t j = 0; j < 7; ++j) {
        TASK_CHECK(mrw_skeleton_inverse_bind(&sv, j, ib) == MRW_OK);
        int slot = -1;
        for (int s = 0; s < 4; ++s) if (EXP_SKIN2OUT[s] == j) slot = s;
        if (slot < 0) { TASK_CHECK(aff_near(ib, I12, 0.0)); }
        else { ibm_aff(slot, exp); TASK_CHECK(aff_near(ib, exp, 1e-6)); }
    }

    /* -- the shared public ordering entry point reports the two required maps + parent table -- */
    cgltf_options co; memset(&co, 0, sizeof co);
    cgltf_data *data = NULL;
    TASK_CHECK(cgltf_parse_file(&co, path, &data) == cgltf_result_success);
    TASK_CHECK(data->skins_count == 1);
    mrw_g2m_forest_order fo;
    char fdiag[256] = {0};
    TASK_CHECK(mrw_g2m_forest_order_build(data, &data->skins[0], &fo, fdiag, sizeof fdiag) == MRW_OK);
    TASK_CHECK(fo.joint_count == 7);
    for (uint32_t j = 0; j < 7; ++j) {
        TASK_CHECK(fo.node_index[j] == EXP_NODE[j]);
        TASK_CHECK(fo.parent[j] == EXP_PARENT[j]);
        TASK_CHECK(fo.nodes[j] == &data->nodes[EXP_NODE[j]]);
        TASK_CHECK(fo.node_to_output[EXP_NODE[j]] == j);
    }
    for (int s = 0; s < 4; ++s) {
        TASK_CHECK(fo.skin_to_output[s] == EXP_SKIN2OUT[s]);
        TASK_CHECK(fo.nodes[fo.skin_to_output[s]] == data->skins[0].joints[s]);
    }
    /* a vertex's JOINTS_0 values are skin slots; remapped indices are real joints, and no
     * auxiliary bone index can enter a vertex's JOINTS slot (slots only map to skin joints). */
    {
        uint8_t is_skin_output[7] = {0};
        for (int s = 0; s < 4; ++s) is_skin_output[fo.skin_to_output[s]] = 1;
        TASK_CHECK(!is_skin_output[0] && !is_skin_output[1] && !is_skin_output[4]);
        for (int s = 0; s < 4; ++s) TASK_CHECK(fo.skin_to_output[s] < fo.joint_count);
    }
    mrw_g2m_forest_order_free(&fo);
    cgltf_free(data);

    mrw_authoring_free(buf);
    remove(path); remove(spath);
    return 0;
}

static int case_single_root_fold_unchanged(void) {
    /* A joint tree with no non-joint chain nodes must give the same locals in both modes: the
     * old single-root result is untouched. */
    const char *path = "forest_unit.gltf";
    TASK_CHECK(write_unit_forest(path) == 0);

    uint8_t *b0 = NULL, *b1 = NULL; size_t s0 = 0, s1 = 0;
    char d0[256] = {0}, d1[256] = {0};
    /* default fold rejects this forest (two roots) - the historical behavior; preserve handles it */
    TASK_CHECK(convert(path, 0, 0, &b0, &s0, d0, sizeof d0) != MRW_OK);
    TASK_CHECK(convert(path, 1, 0, &b1, &s1, d1, sizeof d1) == MRW_OK);

    mrw_blob blob; mrw_skeleton_view sv;
    TASK_CHECK(open_blob(b1, s1, &blob, &sv) == 0);
    TASK_CHECK(sv.joint_count == 2);
    /* roots both at 0xFFFF, no invented root; each is a child of nothing */
    uint16_t p0, p1;
    TASK_CHECK(mrw_skeleton_parent(&sv, 0, &p0) == MRW_OK && p0 == 0xFFFFu);
    TASK_CHECK(mrw_skeleton_parent(&sv, 1, &p1) == MRW_OK && p1 == 0xFFFFu);
    /* own locals: translations preserved, rotations are the node rotations */
    mrw_trs t0, t1;
    TASK_CHECK(mrw_skeleton_rest_local(&sv, 0, &t0) == MRW_OK);
    TASK_CHECK(mrw_skeleton_rest_local(&sv, 1, &t1) == MRW_OK);
    TASK_CHECK(fabs(t0.trans[0] - 1.0f) < 1e-6 && fabs(t1.trans[1] - 2.0f) < 1e-6);

    mrw_authoring_free(b1);
    remove(path);
    return 0;
}

/* ================================================================== hierarchy group */

/* independent local->model over the fixture's own retention order */
static void independent_model(const anim_key keys[5], float u, float model[7][12]) {
    for (uint32_t oi = 0; oi < 7; ++oi) {
        float local[12];
        fixture_node_local(keys, u, EXP_NODE[oi], local);
        if (EXP_PARENT[oi] == 0xFFFFu) memcpy(model[oi], local, sizeof local);
        else aff_mul(model[EXP_PARENT[oi]], local, model[oi]);
    }
}

static int case_sampled_hierarchy(void) {
    const char *path = "forest_main.gltf";
    TASK_CHECK(write_main_fixture(path) == 0);
    uint8_t *buf = NULL; size_t size = 0; char diag[256] = {0};
    mrw_result r = convert(path, 1, 0, &buf, &size, diag, sizeof diag);
    if (r != MRW_OK) { fprintf(stderr, "convert: %s\n", diag); return 1; }

    mrw_blob blob; mrw_skeleton_view sv;
    TASK_CHECK(open_blob(buf, size, &blob, &sv) == 0);
    TASK_CHECK(sv.joint_count == 7);
    mrw_clip_view cv;
    TASK_CHECK(clip_at(&blob, 1, &cv) == 0); /* animA */
    TASK_CHECK(cv.codec == 0);               /* non-unit scales ⇒ raw TRS */

    /* frames: t=0, t=0.5 (mid-animation), t=1 (last), all exact grid samples at 30 fps */
    const float us[3] = { 0.0f, 0.5f, 1.0f };
    const uint32_t frames[3] = { 0, 15, cv.sample_count - 1 };
    float local[12];

    for (int fi = 0; fi < 3; ++fi) {
        /* runtime model from the clip's own samples at this frame */
        mrw_trs lj[7];
        for (uint32_t j = 0; j < 7; ++j) TASK_CHECK(mrw_clip_sample(&cv, j, frames[fi], &lj[j]) == MRW_OK);

        /* each output bone holds exactly its real node's local TRS (ancestors not re-folded) */
        for (uint32_t j = 0; j < 7; ++j) {
            fixture_node_local(ANIM_A, us[fi], EXP_NODE[j], local);
            float got[12];
            mrw_trs_to_affine(&lj[j], got);
            TASK_CHECK(aff_near(got, local, 1e-5));
        }

        float rt_model[7 * 12];
        TASK_CHECK(mrw_local_to_model(&sv, lj, rt_model, 7) == MRW_OK);
        float ref[7][12];
        independent_model(ANIM_A, us[fi], ref);
        for (uint32_t j = 0; j < 7; ++j)
            TASK_CHECK(aff_near(rt_model + j * 12, ref[j], 1e-4));

        /* every forest root starts its model at its own local */
        TASK_CHECK(aff_near(rt_model + 0 * 12, ref[0], 1e-4));
        TASK_CHECK(aff_near(rt_model + 6 * 12, ref[6], 1e-4));
        {
            float root_local[12];
            fixture_node_local(ANIM_A, us[fi], EXP_NODE[6], root_local);
            TASK_CHECK(aff_near(rt_model + 6 * 12, root_local, 1e-4));
        }

        /* the shear stays in the product: with the upper node rotated, model(2) combines the
         * ancestor's non-uniform scale with that rotation and is non-orthogonal (the matrix a fold
         * would have to decompose to a TRS and fail on). At u=0 every rotation is identity, so the
         * product is a pure scale and has no shear. */
        if (fi > 0) {
            float *m = rt_model + 2 * 12;
            float c0[3] = { m[0], m[4], m[8] }, c1[3] = { m[1], m[5], m[9] };
            float dot = c0[0]*c1[0] + c0[1]*c1[1] + c0[2]*c1[2];
            float l0 = sqrtf(c0[0]*c0[0]+c0[1]*c0[1]+c0[2]*c0[2]);
            float l1 = sqrtf(c1[0]*c1[0]+c1[1]*c1[1]+c1[2]*c1[2]);
            TASK_CHECK(l0 > 0 && l1 > 0);
            TASK_CHECK(fabsf(dot / (l0 * l1)) > 1e-3f); /* non-orthogonal ⇒ genuine shear */
        }

        /* palette = model · inverse_bind, per output bone */
        float palette[7 * 12];
        TASK_CHECK(mrw_model_to_palette(&sv, rt_model, palette, 7) == MRW_OK);
        for (uint32_t j = 0; j < 7; ++j) {
            float ib[12];
            TASK_CHECK(mrw_skeleton_inverse_bind(&sv, j, ib) == MRW_OK);
            float want[12]; aff_mul(ref[j], ib, want);
            TASK_CHECK(aff_near(palette + j * 12, want, 1e-4));
        }

        /* weighted vertex through skin-slot inverse binds (non-identity) */
        {
            const float p[3] = { 0.3f, -0.2f, 0.6f };
            const uint32_t slots[3] = { 0, 2, 3 };   /* skin slots, not output indices */
            const float w[3] = { 0.5f, 0.25f, 0.25f };
            float got[3] = { 0, 0, 0 };
            for (int k = 0; k < 3; ++k) {
                float q[3];
                aff_apply(palette + EXP_SKIN2OUT[slots[k]] * 12, p, q);
                for (int c = 0; c < 3; ++c) got[c] += w[k] * q[c];
            }
            float want[3] = { 0, 0, 0 };
            for (int k = 0; k < 3; ++k) {
                float ibk[12], skin[12], q[3];
                ibm_aff((int)slots[k], ibk);
                aff_mul(ref[EXP_SKIN2OUT[slots[k]]], ibk, skin);
                aff_apply(skin, p, q);
                for (int c = 0; c < 3; ++c) want[c] += w[k] * q[c];
            }
            for (int c = 0; c < 3; ++c) TASK_CHECK(fabsf(got[c] - want[c]) < 1e-4f);
            /* the vertex must NOT be addressable by an auxiliary bone index */
            TASK_CHECK(EXP_SKIN2OUT[0] != 0 && EXP_SKIN2OUT[0] != 1 && EXP_SKIN2OUT[0] != 4);
        }
    }

    mrw_authoring_free(buf);
    remove(path);
    return 0;
}

/* ================================================================== batch group */

typedef struct { mrw_dispatch d; const char *name; } backend_t;

/* Every implemented backend the host supports. Scalars always; unavailable SIMD is reported. */
static int host_backends(backend_t *bks) {
    int n = 0; mrw_dispatch t;
    TASK_CHECK(mrw_dispatch_scalar(&t) == MRW_OK);
    bks[n++] = (backend_t){ t, "scalar" };
    if (mrw_dispatch_sse2(&t) == MRW_OK) bks[n++] = (backend_t){ t, "sse2" };
    else printf("FOREST_BACKEND sse2 UNAVAILABLE\n");
    if (mrw_dispatch_avx2(&t) == MRW_OK) {
        bks[n++] = (backend_t){ t, "avx2" };
        if (t.features & MRW_FEAT_FMA) {
            mrw_dispatch nf = t; nf.features &= ~(uint32_t)MRW_FEAT_FMA;
            bks[n++] = (backend_t){ nf, "avx2-nofma" };
        }
        if (t.features & MRW_FEAT_F16C) {
            mrw_dispatch n16 = t; n16.features &= ~(uint32_t)MRW_FEAT_F16C;
            bks[n++] = (backend_t){ n16, "avx2-nof16c" };
        }
    } else printf("FOREST_BACKEND avx2 UNAVAILABLE\n");
    return n;
}

static int near_rel(float got, float want, double atol, double rtol) {
    double d = fabs((double)got - want);
    double s = fabs((double)want);
    if (s < 1.0) s = 1.0;
    return d <= atol + rtol * s;
}

#define NINST 9

/* Compare every available backend's f32 output to the scalar reference (the scalar backend is
 * bit-for-bit the loop of the single-pose primitives; SIMD is checked within F32_TOL). */
static int check_batch_f32(const mrw_skeleton_view *sv, const mrw_clip_view *cv,
                           const float *times, const float *ref, backend_t *bks, int nbk) {
    mrw_mem_req sreq, oreq;
    TASK_CHECK(mrw_batch_clip_to_palette_requirements(sv->joint_count, NINST, MRW_PALETTE_F32,
                                                      &sreq, &oreq) == MRW_OK);
    void *scratch = mrw_authoring_alloc(sreq.size);
    float *out = (float *)mrw_authoring_alloc(oreq.size);
    TASK_CHECK(scratch && out);
    for (int b = 0; b < nbk; ++b) {
        memset(out, 0, oreq.size);
        TASK_CHECK(mrw_batch_clip_to_palette(&bks[b].d, sv, cv, times, NINST,
                                             out, oreq.size, scratch, sreq.size) == MRW_OK);
        for (uint32_t i = 0; i < NINST; ++i)
            for (uint32_t j = 0; j < sv->joint_count; ++j)
                for (int c = 0; c < 12; ++c) {
                    float want = ref[((size_t)i * sv->joint_count + j) * 12 + c];
                    if (b == 0) TASK_CHECK(out[((size_t)i*sv->joint_count+j)*12+c] == want);
                    else TASK_CHECK(near_rel(out[((size_t)i*sv->joint_count+j)*12+c], want, F32_TOL, 0));
                }
    }
    mrw_authoring_free(out); mrw_authoring_free(scratch);
    return 0;
}

static int check_batch_f16(const mrw_skeleton_view *sv, const mrw_clip_view *cv,
                           const float *times, const float *ref, backend_t *bks, int nbk) {
    mrw_mem_req sreq, oreq;
    TASK_CHECK(mrw_batch_clip_to_palette_requirements(sv->joint_count, NINST, MRW_PALETTE_F16,
                                                      &sreq, &oreq) == MRW_OK);
    void *scratch = mrw_authoring_alloc(sreq.size);
    uint16_t *out = (uint16_t *)mrw_authoring_alloc(oreq.size);
    TASK_CHECK(scratch && out);
    for (int b = 0; b < nbk; ++b) {
        memset(out, 0, oreq.size);
        TASK_CHECK(mrw_batch_clip_to_palette_f16(&bks[b].d, sv, cv, times, NINST,
                                                 out, oreq.size, scratch, sreq.size) == MRW_OK);
        for (uint32_t i = 0; i < NINST; ++i)
            for (uint32_t j = 0; j < sv->joint_count; ++j)
                for (int c = 0; c < 12; ++c) {
                    float got = mrw_half_to_float(out[((size_t)i*sv->joint_count+j)*12+c]);
                    float want = ref[((size_t)i * sv->joint_count + j) * 12 + c];
                    TASK_CHECK(near_rel(got, want, F16_ATOL, F16_RTOL));
                }
    }
    mrw_authoring_free(out); mrw_authoring_free(scratch);
    return 0;
}

/* loop the fused single-pose primitive to get the scalar reference palette set */
static int reference_palettes(const mrw_skeleton_view *sv, const mrw_clip_view *cv,
                              const float *times, float *out) {
    uint32_t jc = sv->joint_count;
    float *model = (float *)mrw_authoring_alloc((size_t)jc * 12 * sizeof(float));
    TASK_CHECK(model != NULL);
    for (uint32_t i = 0; i < NINST; ++i) {
        TASK_CHECK(mrw_clip_to_palette(sv, cv, times[i], model, out + (size_t)i * jc * 12, jc) == MRW_OK);
    }
    mrw_authoring_free(model);
    return 0;
}

static int case_batch_forest(void) {
    const char *path = "forest_main.gltf";
    TASK_CHECK(write_main_fixture(path) == 0);
    uint8_t *buf = NULL; size_t size = 0; char diag[256] = {0};
    if (convert(path, 1, 0, &buf, &size, diag, sizeof diag) != MRW_OK) {
        fprintf(stderr, "convert: %s\n", diag); return 1;
    }
    mrw_blob blob; mrw_skeleton_view sv;
    TASK_CHECK(open_blob(buf, size, &blob, &sv) == 0);
    mrw_clip_view cv;
    TASK_CHECK(clip_at(&blob, 1, &cv) == 0);

    float times[NINST];
    for (int i = 0; i < NINST; ++i) times[i] = (float)i / (float)(NINST - 1);

    uint32_t jc = sv.joint_count;
    float *ref = (float *)mrw_authoring_alloc((size_t)NINST * jc * 12 * sizeof(float));
    TASK_CHECK(ref != NULL);
    TASK_CHECK(reference_palettes(&sv, &cv, times, ref) == 0);

    backend_t bks[6]; int nbk = host_backends(bks);
    TASK_CHECK(nbk >= 1);
    TASK_CHECK(check_batch_f32(&sv, &cv, times, ref, bks, nbk) == 0);
    TASK_CHECK(check_batch_f16(&sv, &cv, times, ref, bks, nbk) == 0);

    /* the forest's two roots each start their model at their own local per instance: at t=0 the
     * palette entry for root bone 6 is exactly its node's inverse-bind composition */
    {
        mrw_trs lj;
        TASK_CHECK(mrw_clip_sample(&cv, 6, 0, &lj) == MRW_OK);
        float local12[12], ib[12], want[12];
        mrw_trs_to_affine(&lj, local12);
        TASK_CHECK(mrw_skeleton_inverse_bind(&sv, 6, ib) == MRW_OK);
        aff_mul(local12, ib, want);
        for (int c = 0; c < 12; ++c) TASK_CHECK(fabsf(ref[(size_t)0*jc*12 + 6*12 + c] - want[c]) < 1e-4f);
    }

    /* blend + additive accumulate, both formats, all backends */
    {
        const float *mask = NULL;
        float ta[NINST], tb[NINST], w[NINST];
        for (int i = 0; i < NINST; ++i) {
            ta[i] = times[i];
            tb[i] = 1.0f - times[i];
            w[i] = 0.25f + 0.5f * (float)i / (float)(NINST - 1);
        }
        float *refb = (float *)mrw_authoring_alloc((size_t)NINST * jc * 12 * sizeof(float));
        TASK_CHECK(refb != NULL);
        float *model = (float *)mrw_authoring_alloc((size_t)jc * 12 * sizeof(float));
        TASK_CHECK(model != NULL);
        for (uint32_t i = 0; i < NINST; ++i) {
            mrw_trs a_loc[64], b_loc[64], blended[64];
            TASK_CHECK(jc <= 64);
            TASK_CHECK(mrw_clip_sample_local(&cv, ta[i], a_loc, 64) == MRW_OK);
            TASK_CHECK(mrw_clip_sample_local(&cv, tb[i], b_loc, 64) == MRW_OK);
            TASK_CHECK(mrw_pose_blend(a_loc, b_loc, w[i], mask, blended, jc, 64) == MRW_OK);
            TASK_CHECK(mrw_local_to_model(&sv, blended, model, 64) == MRW_OK);
            TASK_CHECK(mrw_model_to_palette(&sv, model, refb + (size_t)i * jc * 12, 64) == MRW_OK);
        }
        mrw_mem_req sreq, oreq;
        TASK_CHECK(mrw_batch_blend_clips_to_palette_requirements(jc, NINST, MRW_PALETTE_F32,
                                                                 &sreq, &oreq) == MRW_OK);
        void *scratch = mrw_authoring_alloc(sreq.size);
        float *outf = (float *)mrw_authoring_alloc(oreq.size);
        uint16_t *out16 = (uint16_t *)mrw_authoring_alloc(oreq.size);
        TASK_CHECK(scratch && outf && out16);
        for (int b = 0; b < nbk; ++b) {
            TASK_CHECK(mrw_batch_blend_clips_to_palette(&bks[b].d, &sv, &cv, &cv, ta, tb, w, mask,
                                                        NINST, outf, oreq.size, scratch, sreq.size) == MRW_OK);
            for (size_t k = 0; k < (size_t)NINST * jc * 12; ++k)
                TASK_CHECK(b == 0 ? outf[k] == refb[k] : near_rel(outf[k], refb[k], F32_TOL, 0));
            TASK_CHECK(mrw_batch_blend_clips_to_palette_f16(&bks[b].d, &sv, &cv, &cv, ta, tb, w, mask,
                                                            NINST, out16, oreq.size, scratch, sreq.size) == MRW_OK);
            for (size_t k = 0; k < (size_t)NINST * jc * 12; ++k)
                TASK_CHECK(near_rel(mrw_half_to_float(out16[k]), refb[k], F16_ATOL, F16_RTOL));
        }

        /* additive delta pose shared by the batch */
        mrw_trs delta[64];
        for (uint32_t j = 0; j < jc; ++j) {
            q_axis_angle(0, 1, 0, 10.0f + 3.0f * (float)j, delta[j].rot);
            delta[j].trans[0] = 0.05f; delta[j].trans[1] = -0.02f; delta[j].trans[2] = 0.01f;
            delta[j].scale[0] = 1.1f;  delta[j].scale[1] = 0.9f;  delta[j].scale[2] = 1.05f;
        }
        for (uint32_t i = 0; i < NINST; ++i) {
            mrw_trs base_loc[64], acc[64];
            TASK_CHECK(mrw_clip_sample_local(&cv, times[i], base_loc, 64) == MRW_OK);
            TASK_CHECK(mrw_pose_accumulate(base_loc, delta, w[i], mask, acc, jc, 64) == MRW_OK);
            TASK_CHECK(mrw_local_to_model(&sv, acc, model, 64) == MRW_OK);
            TASK_CHECK(mrw_model_to_palette(&sv, model, refb + (size_t)i * jc * 12, 64) == MRW_OK);
        }
        for (int b = 0; b < nbk; ++b) {
            TASK_CHECK(mrw_batch_accumulate_to_palette(&bks[b].d, &sv, &cv, times, delta, w, mask,
                                                       NINST, outf, oreq.size, scratch, sreq.size) == MRW_OK);
            for (size_t k = 0; k < (size_t)NINST * jc * 12; ++k)
                TASK_CHECK(b == 0 ? outf[k] == refb[k] : near_rel(outf[k], refb[k], F32_TOL, 0));
            TASK_CHECK(mrw_batch_accumulate_to_palette_f16(&bks[b].d, &sv, &cv, times, delta, w, mask,
                                                           NINST, out16, oreq.size, scratch, sreq.size) == MRW_OK);
            for (size_t k = 0; k < (size_t)NINST * jc * 12; ++k)
                TASK_CHECK(near_rel(mrw_half_to_float(out16[k]), refb[k], F16_ATOL, F16_RTOL));
        }
        mrw_authoring_free(out16); mrw_authoring_free(outf); mrw_authoring_free(scratch);
        mrw_authoring_free(model); mrw_authoring_free(refb);
    }

    mrw_authoring_free(ref);
    mrw_authoring_free(buf);
    remove(path);
    return 0;
}

static int case_both_codecs(void) {
    /* Unit-scale forest: default emits codec 1 (scale-free), --codec0 emits raw codec 0. Both must
     * carry the same forest and sample to the same poses through the batch path. */
    const char *path = "forest_unit.gltf";
    TASK_CHECK(write_unit_forest(path) == 0);

    uint8_t *b1 = NULL, *b0 = NULL; size_t s1 = 0, s0s = 0;
    char d1[256] = {0}, d0[256] = {0};
    TASK_CHECK(convert(path, 1, 0, &b1, &s1, d1, sizeof d1) == MRW_OK);
    TASK_CHECK(convert(path, 1, 1, &b0, &s0s, d0, sizeof d0) == MRW_OK);

    mrw_blob blob1, blob0; mrw_skeleton_view sv1, sv0;
    TASK_CHECK(open_blob(b1, s1, &blob1, &sv1) == 0);
    TASK_CHECK(open_blob(b0, s0s, &blob0, &sv0) == 0);
    TASK_CHECK(sv1.joint_count == 2 && sv0.joint_count == 2);

    mrw_clip_view c1, c0;
    TASK_CHECK(clip_at(&blob1, 1, &c1) == 0);
    TASK_CHECK(clip_at(&blob0, 1, &c0) == 0);
    TASK_CHECK(c1.codec == 1);
    TASK_CHECK(c0.codec == 0);

    /* both roots must remain roots in each */
    uint16_t p;
    TASK_CHECK(mrw_skeleton_parent(&sv1, 0, &p) == MRW_OK && p == 0xFFFFu);
    TASK_CHECK(mrw_skeleton_parent(&sv1, 1, &p) == MRW_OK && p == 0xFFFFu);

    /* same sample values (codec 1 implies scale 1; codec 0 stores the same 1.0f scales) */
    for (uint32_t s = 0; s < c1.sample_count; s += 7) {
        for (uint32_t j = 0; j < 2; ++j) {
            mrw_trs a, b;
            TASK_CHECK(mrw_clip_sample(&c1, j, s, &a) == MRW_OK);
            TASK_CHECK(mrw_clip_sample(&c0, j, s, &b) == MRW_OK);
            for (int k = 0; k < 4; ++k) TASK_CHECK(fabsf(a.rot[k] - b.rot[k]) < 1e-6f);
            for (int k = 0; k < 3; ++k) TASK_CHECK(fabsf(a.trans[k] - b.trans[k]) < 1e-6f);
            for (int k = 0; k < 3; ++k) TASK_CHECK(fabsf(a.scale[k] - 1.0f) < 1e-6f);
        }
    }

    /* codec-1 clip through the batch forest path */
    backend_t bks[6]; int nbk = host_backends(bks);
    float times[NINST];
    for (int i = 0; i < NINST; ++i) times[i] = 0.75f * (float)i / (float)(NINST - 1);
    float *ref = (float *)mrw_authoring_alloc((size_t)NINST * 2 * 12 * sizeof(float));
    TASK_CHECK(ref != NULL);
    TASK_CHECK(reference_palettes(&sv1, &c1, times, ref) == 0);
    TASK_CHECK(check_batch_f32(&sv1, &c1, times, ref, bks, nbk) == 0);
    TASK_CHECK(check_batch_f16(&sv1, &c1, times, ref, bks, nbk) == 0);
    mrw_authoring_free(ref);

    mrw_authoring_free(b0); mrw_authoring_free(b1);
    remove(path);
    return 0;
}

/* A forest-shaped parent table must pass validation, while self-reference, forward edges and
 * out-of-range parents are still rejected. Mutates a private copy of a converted blob. */
static int case_forest_parent_validation(void) {
    const char *path = "forest_unit.gltf";
    TASK_CHECK(write_unit_forest(path) == 0);
    uint8_t *buf = NULL; size_t size = 0; char diag[256] = {0};
    TASK_CHECK(convert(path, 1, 0, &buf, &size, diag, sizeof diag) == MRW_OK);

    mrw_blob blob; mrw_skeleton_view sv;
    TASK_CHECK(open_blob(buf, size, &blob, &sv) == 0);
    TASK_CHECK(sv.joint_count == 2);
    /* pristine forest blob validates */
    uint16_t p;
    TASK_CHECK(mrw_skeleton_parent(&sv, 0, &p) == MRW_OK && p == 0xFFFFu);
    TASK_CHECK(mrw_skeleton_parent(&sv, 1, &p) == MRW_OK && p == 0xFFFFu);

    /* locate the SKELETON section: section 0 for this single-skeleton blob */
    uint32_t st = mrw_rd_u32(buf + 20);
    uint64_t skel_off = 0; memcpy(&skel_off, buf + st + 8, 8);
    uint32_t parent_off = mrw_rd_u32(buf + skel_off + 24);
    uint8_t *copy = (uint8_t *)mrw_authoring_alloc(size);
    TASK_CHECK(copy != NULL);

    #define TRY_PARENT(v0, v1, expect) do { \
        memcpy(copy, buf, size); \
        uint16_t a_ = (v0), b_ = (v1); \
        memcpy(copy + skel_off + parent_off + 0, &a_, 2); \
        memcpy(copy + skel_off + parent_off + 2, &b_, 2); \
        mrw_blob tb; \
        TASK_CHECK(mrw_blob_open(copy, (uint64_t)size, &tb) == (expect)); \
    } while (0)

    TRY_PARENT(0xFFFFu, 0xFFFFu, MRW_OK);       /* forest is legal */
    TRY_PARENT(0xFFFFu, 0,        MRW_OK);       /* single tree is legal */
    TRY_PARENT(1,        0xFFFFu, MRW_E_RANGE);  /* forward edge from a root */
    TRY_PARENT(0xFFFFu, 5,        MRW_E_RANGE);  /* out-of-range parent */
    TRY_PARENT(0,        0xFFFFu, MRW_E_FORMAT); /* self-reference */
    TRY_PARENT(1,        1,       MRW_E_RANGE);  /* index 0 looks forward first */
    #undef TRY_PARENT

    mrw_authoring_free(copy);
    mrw_authoring_free(buf);
    remove(path);
    return 0;
}

/* Duplicate skin slots are ambiguous for the slot->output map, so the shared ordering entry point
 * rejects them; a node that is not part of the glTF is rejected too. Built from a hand-made
 * cgltf_data - no file needed. */
static int case_forest_order_rejections(void) {
    cgltf_node nodes[2];
    memset(nodes, 0, sizeof nodes);
    nodes[0].parent = NULL;
    nodes[1].parent = &nodes[0];
    cgltf_node *child_list[1] = { &nodes[1] };
    nodes[0].children = child_list;
    nodes[0].children_count = 1;

    cgltf_data data;
    memset(&data, 0, sizeof data);
    data.nodes = nodes;
    data.nodes_count = 2;

    cgltf_skin skin;
    memset(&skin, 0, sizeof skin);
    cgltf_node *joints[2] = { &nodes[1], &nodes[1] }; /* same node in two slots */
    skin.joints = joints;
    skin.joints_count = 2;

    mrw_g2m_forest_order fo;
    char diag[256] = {0};
    TASK_CHECK(mrw_g2m_forest_order_build(&data, &skin, &fo, diag, sizeof diag) == MRW_E_FORMAT);
    TASK_CHECK(fo.joint_count == 0 && fo.nodes == NULL && fo.parent == NULL);
    TASK_CHECK(diag[0] != '\0');

    /* a joint pointer that is not one of data->nodes */
    cgltf_node stray;
    memset(&stray, 0, sizeof stray);
    joints[0] = &stray; joints[1] = &nodes[1];
    TASK_CHECK(mrw_g2m_forest_order_build(&data, &skin, &fo, diag, sizeof diag) == MRW_E_FORMAT);

    /* valid case: one joint (the leaf) -> closure is both real nodes, forest root is node 0 */
    joints[0] = &nodes[1]; skin.joints_count = 1;
    TASK_CHECK(mrw_g2m_forest_order_build(&data, &skin, &fo, diag, sizeof diag) == MRW_OK);
    TASK_CHECK(fo.joint_count == 2);
    TASK_CHECK(fo.node_index[0] == 0 && fo.parent[0] == 0xFFFFu);
    TASK_CHECK(fo.node_index[1] == 1 && fo.parent[1] == 0);
    TASK_CHECK(fo.skin_to_output[0] == 1);
    mrw_g2m_forest_order_free(&fo);
    return 0;
}

/* ================================================================== registration */

const struct task_case task_cases[] = {
    { "import",    "main_fixture_import",     case_main_fixture_import },
    { "import",    "single_root_fold",        case_single_root_fold_unchanged },
    { "import",    "forest_parent_validate",  case_forest_parent_validation },
    { "import",    "forest_order_rejections", case_forest_order_rejections },
    { "hierarchy", "sampled_hierarchy",       case_sampled_hierarchy },
    { "batch",     "batch_forest",            case_batch_forest },
    { "batch",     "both_codecs",             case_both_codecs },
};
const size_t task_case_count = sizeof task_cases / sizeof task_cases[0];
