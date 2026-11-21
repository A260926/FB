/* C11 smoke test for the oriented-BVH C API. This is a genuine C translation unit: it must be
 * compiled with a C compiler and only uses the public header. */

#include <bvh/v2/c_api/bvh.h>

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "obb_c_smoke FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

struct tri {
    struct bvh_vec3f v[3];
};

struct query {
    const struct tri* tris;
    size_t tri_count;
    const struct bvh_obb3f* bvh;
    struct bvh_vec3f org;
    struct bvh_vec3f dir;
    size_t best;
    float best_t;
};

static struct bvh_vec3f vec3(float x, float y, float z) {
    struct bvh_vec3f v = { x, y, z };
    return v;
}

static struct bvh_vec3f add(struct bvh_vec3f a, struct bvh_vec3f b) {
    return vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static struct bvh_vec3f sub(struct bvh_vec3f a, struct bvh_vec3f b) {
    return vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static float dot3(struct bvh_vec3f a, struct bvh_vec3f b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static struct bvh_vec3f cross3(struct bvh_vec3f a, struct bvh_vec3f b) {
    return vec3(a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x);
}

/* Moller-Trumbore, mirroring the C++ `PrecomputedTri` test exactly (same edge definitions). */
static bool intersect_tri(const struct tri* tri, struct bvh_vec3f org, struct bvh_vec3f dir,
                          float tmin, float tmax, float* out_t) {
    struct bvh_vec3f e1 = sub(tri->v[0], tri->v[1]);
    struct bvh_vec3f e2 = sub(tri->v[2], tri->v[0]);
    struct bvh_vec3f n = cross3(e1, e2);
    struct bvh_vec3f c = sub(tri->v[0], org);
    struct bvh_vec3f r = cross3(dir, c);
    float det = dot3(n, dir);
    if (det == 0.0f)
        return false;
    float inv = 1.0f / det;
    float u = dot3(r, e2) * inv;
    float v = dot3(r, e1) * inv;
    float w = 1.0f - u - v;
    if (u >= 0.0f && v >= 0.0f && w >= 0.0f) {
        float t = dot3(n, c) * inv;
        if (t >= tmin && t <= tmax) {
            *out_t = t;
            return true;
        }
    }
    return false;
}

static bool user_fn(void* user_data, float* t, size_t begin, size_t end) {
    struct query* q = (struct query*)user_data;
    bool found = false;
    size_t i;
    for (i = begin; i < end; ++i) {
        size_t prim = bvh_obb3f_get_prim_id(q->bvh, i);
        float hit_t;
        if (prim < q->tri_count &&
            intersect_tri(&q->tris[prim], q->org, q->dir, 0.0f, *t, &hit_t)) {
            *t = hit_t;
            q->best = prim;
            q->best_t = hit_t;
            found = true;
        }
    }
    return found;
}

static bool brute_force(const struct tri* tris, size_t count, struct bvh_vec3f org,
                        struct bvh_vec3f dir, size_t* out_id, float* out_t) {
    size_t i;
    size_t best = SIZE_MAX;
    float best_t = 1e30f;
    for (i = 0; i < count; ++i) {
        float t;
        if (intersect_tri(&tris[i], org, dir, 0.0f, best_t, &t)) {
            best = i;
            best_t = t;
        }
    }
    if (out_id)
        *out_id = best;
    if (out_t)
        *out_t = best_t;
    return best != SIZE_MAX;
}

static int make_scene(struct tri* tris) {
    struct bvh_vec3f n1 = vec3(0.3f, 0.2f, 0.93f);
    struct bvh_vec3f n2 = vec3(-0.7f, 0.5f, 0.5f);
    int i, j;
    size_t k = 0;
    for (j = 0; j < 2; ++j) {
        for (i = 0; i < 2; ++i) {
            struct bvh_vec3f c = vec3(-1.4f + 0.8f * (float)i, -1.3f + 0.8f * (float)j, 0.2f);
            struct bvh_vec3f u = vec3(0.3f, 0.06f, 0.0f);
            struct bvh_vec3f v = vec3(0.0f, 0.3f, 0.1f);
            struct bvh_vec3f a = sub(sub(c, u), v);
            struct bvh_vec3f b = sub(add(c, u), v);
            struct bvh_vec3f d = add(add(c, u), v);
            struct bvh_vec3f e = add(sub(c, u), v);
            tris[k].v[0] = a; tris[k].v[1] = b; tris[k].v[2] = d; ++k;
            tris[k].v[0] = a; tris[k].v[1] = d; tris[k].v[2] = e; ++k;
        }
    }
    for (j = 0; j < 2; ++j) {
        for (i = 0; i < 2; ++i) {
            struct bvh_vec3f c = vec3(1.2f + 0.8f * (float)i, -1.3f + 0.8f * (float)j, -0.1f);
            struct bvh_vec3f u = vec3(0.3f, 0.0f, 0.12f);
            struct bvh_vec3f v = vec3(0.0f, 0.3f, 0.05f);
            struct bvh_vec3f a = sub(sub(c, u), v);
            struct bvh_vec3f b = sub(add(c, u), v);
            struct bvh_vec3f d = add(add(c, u), v);
            struct bvh_vec3f e = add(sub(c, u), v);
            tris[k].v[0] = a; tris[k].v[1] = b; tris[k].v[2] = d; ++k;
            tris[k].v[0] = a; tris[k].v[1] = d; tris[k].v[2] = e; ++k;
        }
    }
    (void)n1;
    (void)n2;
    return (int)k;
}

static void tri_bbox(const struct tri* tri, struct bvh_bbox3f* bbox) {
    int i;
    bbox->min = tri->v[0];
    bbox->max = tri->v[0];
    for (i = 1; i < 3; ++i) {
        if (tri->v[i].x < bbox->min.x) bbox->min.x = tri->v[i].x;
        if (tri->v[i].y < bbox->min.y) bbox->min.y = tri->v[i].y;
        if (tri->v[i].z < bbox->min.z) bbox->min.z = tri->v[i].z;
        if (tri->v[i].x > bbox->max.x) bbox->max.x = tri->v[i].x;
        if (tri->v[i].y > bbox->max.y) bbox->max.y = tri->v[i].y;
        if (tri->v[i].z > bbox->max.z) bbox->max.z = tri->v[i].z;
    }
}

static void tri_center(const struct tri* tri, struct bvh_vec3f* center) {
    center->x = (tri->v[0].x + tri->v[1].x + tri->v[2].x) * (1.0f / 3.0f);
    center->y = (tri->v[0].y + tri->v[1].y + tri->v[2].y) * (1.0f / 3.0f);
    center->z = (tri->v[0].z + tri->v[1].z + tri->v[2].z) * (1.0f / 3.0f);
}

int main(void) {
    enum { TRI_COUNT = 16 };
    struct tri tris[TRI_COUNT];
    struct bvh_bbox3f boxes[TRI_COUNT];
    struct bvh_vec3f centers[TRI_COUNT];
    struct bvh_vec3f verts[TRI_COUNT * 3];
    struct bvh3f* source;
    struct bvh_obb3f* obb;
    size_t i;
    int count = make_scene(tris);

    CHECK(count == TRI_COUNT);
    for (i = 0; i < TRI_COUNT; ++i) {
        tri_bbox(&tris[i], &boxes[i]);
        tri_center(&tris[i], &centers[i]);
        verts[i * 3 + 0] = tris[i].v[0];
        verts[i * 3 + 1] = tris[i].v[1];
        verts[i * 3 + 2] = tris[i].v[2];
    }

    source = bvh3f_build(NULL, boxes, centers, TRI_COUNT, NULL);
    CHECK(source != NULL);
    CHECK(bvh3f_get_node_count(source) > 0);

    obb = bvh_obb3f_from_bvh(source, verts, TRI_COUNT);
    CHECK(obb != NULL);
    CHECK(bvh_obb3f_get_node_count(obb) == bvh3f_get_node_count(source));
    CHECK(bvh_obb3f_get_prim_count(obb) == TRI_COUNT);

    /* The source tree can be destroyed immediately after conversion. */
    bvh3f_destroy(source);

    {
        size_t seen[TRI_COUNT];
        memset(seen, 0, sizeof(seen));
        for (i = 0; i < TRI_COUNT; ++i) {
            size_t id = bvh_obb3f_get_prim_id(obb, i);
            CHECK(id < TRI_COUNT);
            seen[id] += 1;
        }
        for (i = 0; i < TRI_COUNT; ++i)
            CHECK(seen[i] == 1);
    }

    /* Nodes expose their own orthonormal frames. */
    {
        struct bvh_vec3f axes0[3], axes1[3], center0, half0;
        size_t node_count = bvh_obb3f_get_node_count(obb);
        int different = 0;
        CHECK(node_count >= 3);
        CHECK(bvh_obb3f_get_node_obb(obb, 0, axes0, &center0, &half0));
        CHECK(bvh_obb3f_get_node_obb(obb, 1, axes1, NULL, NULL));
        for (i = 0; i < 3; ++i) {
            float len = sqrtf(dot3(axes0[i], axes0[i]));
            CHECK(fabsf(len - 1.0f) < 1e-5f);
            CHECK(half0.x >= 0.0f && half0.y >= 0.0f && half0.z >= 0.0f);
        }
        for (i = 0; i < 3; ++i) {
            int j;
            for (j = 0; j < 3; ++j) {
                if (axes0[i].x != axes1[i].x || axes0[i].y != axes1[i].y ||
                    axes0[i].z != axes1[i].z)
                    different = 1;
            }
        }
        CHECK(different);
        CHECK(!bvh_obb3f_node_is_leaf(obb, 0));
        CHECK(!bvh_obb3f_get_node_obb(obb, 100000, axes0, NULL, NULL));
    }

    /* Closest-hit queries agree with the brute-force world-space loop. */
    {
        int hits = 0;
        for (i = 0; i < 24; ++i) {
            struct bvh_ray3f ray;
            struct query q;
            struct bvh_intersect_callbackf callback;
            size_t expected_id;
            float expected_t;

            ray.org = vec3(-2.2f + 0.19f * (float)i, -1.9f + 0.16f * (float)(i % 9), 5.0f);
            ray.dir = vec3(0.02f * (float)(i % 3), -0.01f * (float)(i % 4), -1.0f);
            ray.tmin = 0.0f;
            ray.tmax = 30.0f;

            q.tris = tris;
            q.tri_count = TRI_COUNT;
            q.bvh = obb;
            q.org = ray.org;
            q.dir = ray.dir;
            q.best = SIZE_MAX;
            q.best_t = 1e30f;
            callback.user_data = &q;
            callback.user_fn = user_fn;

            bvh_obb3f_intersect_ray(obb, &ray, &callback);

            if (brute_force(tris, TRI_COUNT, ray.org, ray.dir, &expected_id, &expected_t))
                ++hits;
            CHECK(q.best == expected_id);
            if (expected_id != SIZE_MAX)
                CHECK(fabsf(q.best_t - expected_t) <= 1e-4f);
        }
        CHECK(hits > 3);
    }

    /* Any-hit queries honor a finite distance window. */
    {
        struct bvh_ray3f ray;
        float windows[3] = { 1.0f, 3.0f, 6.0f };
        int w;
        for (w = 0; w < 3; ++w) {
            struct query q;
            struct bvh_intersect_callbackf callback;
            size_t expected_id;
            ray.org = vec3(-0.9f, 0.1f, 5.0f);
            ray.dir = vec3(0.0f, 0.0f, -1.0f);
            ray.tmin = 0.0f;
            ray.tmax = windows[w];
            q.tris = tris;
            q.tri_count = TRI_COUNT;
            q.bvh = obb;
            q.org = ray.org;
            q.dir = ray.dir;
            q.best = SIZE_MAX;
            q.best_t = 1e30f;
            callback.user_data = &q;
            callback.user_fn = user_fn;
            bvh_obb3f_intersect_ray_any(obb, &ray, &callback);
            CHECK((q.best != SIZE_MAX) ==
                  brute_force(tris, TRI_COUNT, ray.org, ray.dir, &expected_id, NULL));
        }
    }

    /* Refit after moving all geometry beyond the previous bounds. */
    {
        struct bvh_vec3f moved[TRI_COUNT * 3];
        for (i = 0; i < TRI_COUNT; ++i) {
            tris[i].v[0].z += 2.5f;
            tris[i].v[1].z += 2.5f;
            tris[i].v[2].z += 2.5f;
            moved[i * 3 + 0] = tris[i].v[0];
            moved[i * 3 + 1] = tris[i].v[1];
            moved[i * 3 + 2] = tris[i].v[2];
        }
        bvh_obb3f_refit(obb, moved, TRI_COUNT);
        CHECK(bvh_obb3f_get_prim_count(obb) == TRI_COUNT);
    }

    /* Save, load, and keep working without the source tree. */
    {
        const char* path = "obb_c_smoke.bin";
        FILE* file = fopen(path, "wb");
        struct bvh_obb3f* loaded;
        CHECK(file != NULL);
        if (!file)
            return 1;
        bvh_obb3f_save(obb, file);
        fclose(file);

        file = fopen(path, "rb");
        CHECK(file != NULL);
        if (!file)
            return 1;
        loaded = bvh_obb3f_load(file);
        fclose(file);
        remove(path);
        CHECK(loaded != NULL);
        if (!loaded)
            return 1;

        CHECK(bvh_obb3f_get_node_count(loaded) == bvh_obb3f_get_node_count(obb));
        CHECK(bvh_obb3f_get_prim_count(loaded) == TRI_COUNT);
        for (i = 0; i < TRI_COUNT; ++i)
            CHECK(bvh_obb3f_get_prim_id(loaded, i) == bvh_obb3f_get_prim_id(obb, i));

        {
            int hits = 0;
            for (i = 0; i < 16; ++i) {
                struct bvh_ray3f ray;
                struct query q;
                struct bvh_intersect_callbackf callback;
                size_t expected_id;
                ray.org = vec3(-2.0f + 0.24f * (float)i, -1.4f + 0.19f * (float)(i % 7), 8.0f);
                ray.dir = vec3(0.0f, 0.0f, -1.0f);
                ray.tmin = 0.0f;
                ray.tmax = 40.0f;
                q.tris = tris;
                q.tri_count = TRI_COUNT;
                q.bvh = loaded;
                q.org = ray.org;
                q.dir = ray.dir;
                q.best = SIZE_MAX;
                q.best_t = 1e30f;
                callback.user_data = &q;
                callback.user_fn = user_fn;
                bvh_obb3f_intersect_ray(loaded, &ray, &callback);
                if (brute_force(tris, TRI_COUNT, ray.org, ray.dir, &expected_id, NULL))
                    ++hits;
                CHECK(q.best == expected_id);
            }
            CHECK(hits > 1);
        }

        /* Refit the loaded tree as well; the frames survived the file round-trip. */
        {
            struct bvh_vec3f moved[TRI_COUNT * 3];
            for (i = 0; i < TRI_COUNT; ++i) {
                moved[i * 3 + 0] = tris[i].v[0];
                moved[i * 3 + 1] = tris[i].v[1];
                moved[i * 3 + 2] = tris[i].v[2];
            }
            bvh_obb3f_refit(loaded, moved, TRI_COUNT);
            CHECK(bvh_obb3f_get_node_count(loaded) == bvh_obb3f_get_node_count(obb));
        }
        bvh_obb3f_destroy(loaded);
    }

    bvh_obb3f_destroy(obb);
    printf("obb_c_smoke OK triangles=%d\n", TRI_COUNT);
    return 0;
}
