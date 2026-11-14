#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "../core/api.h"
#include "relief_shader.h"

/* ------------------------------------------------------------------ */
/* height field sampling                                               */
/* ------------------------------------------------------------------ */

static float grid_sample(const float *heights, int width, int i, int j) {
    return heights[j * width + i];
}

/* bilinear height and its derivatives over cell (cell_x, cell_y) */
static void cell_surface(
        const float *heights, int width, int cell_x, int cell_y,
        float s, float t, float *height, float *dh_ds, float *dh_dt) {
    float c00 = grid_sample(heights, width, cell_x,     cell_y);
    float c10 = grid_sample(heights, width, cell_x + 1, cell_y);
    float c01 = grid_sample(heights, width, cell_x,     cell_y + 1);
    float c11 = grid_sample(heights, width, cell_x + 1, cell_y + 1);
    float h00 = c00 * (1 - s) + c10 * s;
    float h01 = c01 * (1 - s) + c11 * s;
    *height = h00 * (1 - t) + h01 * t;
    *dh_ds = (c10 - c00) * (1 - t) + (c11 - c01) * t;
    *dh_dt = (c01 - c00) * (1 - s) + (c11 - c10) * s;
}

static void surface_normal(const relief_material_t *material,
                           float u, float v, vec3_t *normal) {
    int max_x = material->width - 2;
    int max_y = material->height - 2;
    int cell_x = (int)floor(u * (material->width - 1));
    int cell_y = (int)floor(v * (material->height - 1));
    float s, t, dh_ds, dh_dt, dh_du, dh_dv, height;
    if (cell_x < 0) {
        cell_x = 0;
    } else if (cell_x > max_x) {
        cell_x = max_x;
    }
    if (cell_y < 0) {
        cell_y = 0;
    } else if (cell_y > max_y) {
        cell_y = max_y;
    }
    s = u * (material->width - 1) - cell_x;
    t = v * (material->height - 1) - cell_y;
    if (s < 0) {
        s = 0;
    } else if (s > 1) {
        s = 1;
    }
    if (t < 0) {
        t = 0;
    } else if (t > 1) {
        t = 1;
    }
    cell_surface(material->heights, material->width, cell_x, cell_y,
                 s, t, &height, &dh_ds, &dh_dt);
    dh_du = dh_ds * (material->width - 1);
    dh_dv = dh_dt * (material->height - 1);
    *normal = vec3_normalize(vec3_new(material->scale * dh_du,
                                      material->scale * dh_dv, 1));
}

/* ------------------------------------------------------------------ */
/* ray / bilinear-cell intersection                                    */
/* ------------------------------------------------------------------ */

#define RELIEF_INF 1e30

/*
 * solve f(lambda) = oz + lambda * dz + scale * H(s(lambda), t(lambda)) = 0
 * inside one cell; H is bilinear in (s, t) while both are linear in lambda,
 * so f is at most quadratic. every admissible root is collected first and
 * the earliest one wins, which keeps tangent and double-root cases exact.
 */
static double solve_cell(const relief_material_t *material,
                         int cell_x, int cell_y,
                         double ox, double oy, double oz,
                         double dx, double dy, double dz,
                         double lambda_lo, double lambda_hi) {
    int grid_w = material->width;
    double scale = material->scale;
    double du = 1.0 / (grid_w - 1);
    double dv = 1.0 / (material->height - 1);
    double c00 = grid_sample(material->heights, grid_w, cell_x,     cell_y);
    double c10 = grid_sample(material->heights, grid_w, cell_x + 1, cell_y);
    double c01 = grid_sample(material->heights, grid_w, cell_x,     cell_y + 1);
    double c11 = grid_sample(material->heights, grid_w, cell_x + 1, cell_y + 1);
    double a0 = ox / du - cell_x;
    double a1 = dx / du;
    double b0 = oy / dv - cell_y;
    double b1 = dy / dv;
    double h_a = c00;
    double h_b = c10 - c00;
    double h_c = c01 - c00;
    double h_e = c00 - c10 - c01 + c11;
    double h0 = h_a + h_b * a0 + h_c * b0 + h_e * a0 * b0;
    double h1 = h_b * a1 + h_c * b1 + h_e * (a1 * b0 + a0 * b1);
    double h2 = h_e * a1 * b1;
    double p = scale * h2;
    double q = dz + scale * h1;
    double r = oz + scale * h0;
    double roots[2];
    int num_roots = 0;
    int k;
    double best = -1;
    const double cell_eps = 1e-7;
    const double lambda_eps = 1e-10;

    if (fabs(p) < 1e-14) {
        if (fabs(q) > 1e-15) {
            roots[num_roots++] = -r / q;
        }
    } else {
        double discriminant = q * q - 4 * p * r;
        /*
         * rays arrive through float-rounded camera math, so features below
         * roughly float relative precision cannot be told apart from a
         * grazing contact: clamp such near-zero discriminants instead of
         * punching holes through tangent surfaces
         */
        double tolerance = 1e-7 * (1 + q * q + fabs(4 * p * r));
        if (discriminant < -tolerance) {
            return -1;
        }
        if (discriminant < 0) {
            discriminant = 0;                /* grazing contact */
        }
        {
            double sq = sqrt(discriminant);
            roots[num_roots++] = (-q - sq) / (2 * p);
            if (sq > 1e-15 * (1 + fabs(q))) {
                roots[num_roots++] = (-q + sq) / (2 * p);
            }
        }
    }

    for (k = 0; k < num_roots; k++) {
        double lambda = roots[k];
        double x, y, s, t, z;
        if (!(lambda >= lambda_lo - lambda_eps) ||
            !(lambda <= lambda_hi + lambda_eps)) {
            continue;
        }
        x = ox + lambda * dx;
        y = oy + lambda * dy;
        s = x / du - cell_x;
        t = y / dv - cell_y;
        z = oz + lambda * dz;
        if (s < -cell_eps || s > 1 + cell_eps ||
            t < -cell_eps || t > 1 + cell_eps) {
            continue;
        }
        if (z < -scale - cell_eps * (1 + scale) ||
            z > cell_eps * (1 + scale)) {
            continue;
        }
        if (best < 0 || lambda < best) {
            best = lambda;
        }
    }
    return best;
}

/* slab intersection against [lo, hi] on a single axis */
static int slab_interval(double o, double d, double lo, double hi,
                         double *tenter, double *tleave) {
    if (fabs(d) < 1e-15) {
        return o >= lo && o <= hi;
    } else {
        double t1 = (lo - o) / d;
        double t2 = (hi - o) / d;
        double tmin = t1 < t2 ? t1 : t2;
        double tmax = t1 < t2 ? t2 : t1;
        if (tmin > *tenter) {
            *tenter = tmin;
        }
        if (tmax < *tleave) {
            *tleave = tmax;
        }
        return 1;
    }
}

static int intersect_flat(double ox, double oy, double oz,
                          double dx, double dy, double dz,
                          relief_hit_t *hit) {
    double lambda;
    double x, y;
    if (fabs(dz) < 1e-15) {
        return 0;
    }
    lambda = -oz / dz;
    if (!(lambda >= 0)) {
        return 0;
    }
    x = ox + lambda * dx;
    y = oy + lambda * dy;
    if (x < -1e-7 || x > 1 + 1e-7 || y < -1e-7 || y > 1 + 1e-7) {
        return 0;
    }
    hit->hit = 1;
    hit->uv = vec2_new((float)x, (float)y);
    hit->position = vec3_new((float)x, (float)y, 0);
    hit->normal = vec3_new(0, 0, 1);
    return 1;
}

int relief_intersect(const relief_material_t *material,
                     vec3_t origin, vec3_t direction,
                     relief_hit_t *hit) {
    double ox = origin.x, oy = origin.y, oz = origin.z;
    double dx = direction.x, dy = direction.y, dz = direction.z;
    double scale = material->scale;
    double tenter = -RELIEF_INF, tleave = RELIEF_INF;
    double lambda;
    int cell_x, cell_y, guard;
    int grid_w = material->width;
    int grid_h = material->height;
    double du = 1.0 / (grid_w - 1);
    double dv = 1.0 / (grid_h - 1);

    hit->hit = 0;

    if (scale == 0) {
        return intersect_flat(ox, oy, oz, dx, dy, dz, hit);
    }

    /* trace only inside the local column [0, 1]^2 x [-scale, 0] */
    if (!slab_interval(ox, dx, 0, 1, &tenter, &tleave) ||
        !slab_interval(oy, dy, 0, 1, &tenter, &tleave) ||
        !slab_interval(oz, dz, -scale, 0, &tenter, &tleave)) {
        return 0;
    }
    lambda = tenter > 0 ? tenter : 0;
    if (lambda > tleave) {
        return 0;
    }

    /* grid traversal: one analytic solve per bilinear cell, no layering */
    for (guard = 0; guard < grid_w + grid_h + 4; guard++) {
        double x = ox + lambda * dx;
        double y = oy + lambda * dy;
        double tx = RELIEF_INF, ty = RELIEF_INF;
        double tstop, best, next_lambda;

        cell_x = (int)floor(x / du);
        cell_y = (int)floor(y / dv);
        if (cell_x < 0 || cell_x >= grid_w - 1 ||
            cell_y < 0 || cell_y >= grid_h - 1) {
            return 0;                       /* left through a side */
        }

        if (fabs(dx) > 1e-15) {
            int boundary = cell_x + (dx > 0 ? 1 : 0);
            tx = (boundary * du - ox) / dx;
            if (tx <= lambda) {
                tx = RELIEF_INF;
            }
        }
        if (fabs(dy) > 1e-15) {
            int boundary = cell_y + (dy > 0 ? 1 : 0);
            ty = (boundary * dv - oy) / dy;
            if (ty <= lambda) {
                ty = RELIEF_INF;
            }
        }
        tstop = tx < ty ? tx : ty;
        if (tleave < tstop) {
            tstop = tleave;
        }

        best = solve_cell(material, cell_x, cell_y,
                          ox, oy, oz, dx, dy, dz,
                          lambda, tstop);
        if (best >= 0) {
            double hx = ox + best * dx;
            double hy = oy + best * dy;
            double hz = oz + best * dz;
            vec3_t normal;
            if (hx < 0) {
                hx = 0;
            } else if (hx > 1) {
                hx = 1;
            }
            if (hy < 0) {
                hy = 0;
            } else if (hy > 1) {
                hy = 1;
            }
            surface_normal(material, (float)hx, (float)hy, &normal);
            hit->hit = 1;
            hit->position = vec3_new((float)hx, (float)hy, (float)hz);
            hit->uv = vec2_new((float)hx, (float)hy);
            hit->normal = normal;
            return 1;
        }

        /* advance to the next cell boundary, then re-derive the cell from
         * the position so rays running exactly on a shared edge cannot
         * wrap around or get stuck */
        if (tleave <= tx && tleave <= ty) {
            return 0;                       /* exited the column */
        }
        next_lambda = tx < ty ? tx : ty;
        if (!(next_lambda > lambda)) {
            return 0;
        }
        lambda = next_lambda + (tleave - next_lambda) * 1e-12 + 1e-15;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* gpu-side program                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    vec3_t local_position;
    vec2_t texcoord;
} relief_attribs_t;

typedef struct {
    vec3_t local_position;
    vec2_t texcoord;
} relief_varyings_t;

typedef struct {
    mat4_t model_matrix;
    mat4_t inv_model_matrix;
    mat3_t normal_matrix;
    mat4_t camera_vp_matrix;
    vec3_t camera_pos;
    vec3_t light_dir;
    vec3_t basecolor;
    const float *heights;
    int grid_width;
    int grid_height;
    float scale;
    float near_plane;
    float far_plane;
    float ambient_intensity;
    float punctual_intensity;
} relief_uniforms_t;

static vec4_t relief_vertex_shader(void *attribs_, void *varyings_,
                                   void *uniforms_) {
    relief_attribs_t *attribs = (relief_attribs_t*)attribs_;
    relief_varyings_t *varyings = (relief_varyings_t*)varyings_;
    relief_uniforms_t *uniforms = (relief_uniforms_t*)uniforms_;
    vec4_t local_position = vec4_from_vec3(attribs->local_position, 1);
    vec4_t world_position =
        mat4_mul_vec4(uniforms->model_matrix, local_position);
    vec4_t clip_position =
        mat4_mul_vec4(uniforms->camera_vp_matrix, world_position);
    varyings->local_position = attribs->local_position;
    varyings->texcoord = attribs->texcoord;
    return clip_position;
}

static fragment_output_t relief_fragment_shader(
        void *varyings_, void *uniforms_, int backface, float base_depth) {
    relief_varyings_t *varyings = (relief_varyings_t*)varyings_;
    relief_uniforms_t *uniforms = (relief_uniforms_t*)uniforms_;
    relief_material_t material;
    relief_hit_t hit;
    vec3_t local_camera, local_direction, world_normal;
    vec3_t light_dir, shaded;
    vec4_t world_point, clip_point;
    fragment_output_t output;
    float n_dot_l, window_depth;

    UNUSED_VAR(backface);
    UNUSED_VAR(base_depth);
    output.color = vec4_new(0, 0, 0, 0);
    output.depth = base_depth;
    output.discard = 1;

    /* bring the camera into local space and shoot away from it */
    world_point = mat4_mul_vec4(uniforms->inv_model_matrix,
                                vec4_from_vec3(uniforms->camera_pos, 1));
    local_camera = vec3_from_vec4(world_point);
    local_direction = vec3_normalize(
        vec3_sub(varyings->local_position, local_camera));

    material.width = uniforms->grid_width;
    material.height = uniforms->grid_height;
    material.heights = uniforms->heights;
    material.scale = uniforms->scale;
    material.basecolor = uniforms->basecolor;
    material.near_plane = uniforms->near_plane;
    material.far_plane = uniforms->far_plane;

    if (!relief_intersect(&material, local_camera, local_direction, &hit)) {
        return output;
    }

    /* project the true hit point, never offset the base window depth */
    world_point = mat4_mul_vec4(uniforms->model_matrix,
                                vec4_from_vec3(hit.position, 1));
    clip_point = mat4_mul_vec4(uniforms->camera_vp_matrix, world_point);
    if (!isfinite(clip_point.x) || !isfinite(clip_point.y) ||
        !isfinite(clip_point.z) || !isfinite(clip_point.w) ||
        clip_point.w <= EPSILON) {
        return output;
    }
    {
        float ndc_z = clip_point.z / clip_point.w;
        if (!isfinite(ndc_z) || ndc_z < -1 || ndc_z > 1) {
            return output;                   /* outside finite near/far */
        }
        window_depth = ndc_z * 0.5f + 0.5f;
        if (!isfinite(window_depth) ||
            window_depth < 0 || window_depth > 1) {
            return output;
        }
    }

    /* simple lambert lighting with the local height-field normal */
    world_normal = vec3_normalize(
        mat3_mul_vec3(uniforms->normal_matrix, hit.normal));
    light_dir = vec3_normalize(uniforms->light_dir);
    n_dot_l = vec3_dot(world_normal, vec3_negate(light_dir));
    if (n_dot_l < 0) {
        n_dot_l = 0;
    }
    shaded = vec3_mul(uniforms->basecolor,
                      uniforms->ambient_intensity +
                      uniforms->punctual_intensity * n_dot_l);

    output.color = vec4_from_vec3(shaded, 1);
    output.depth = window_depth;
    output.discard = 0;
    return output;
}

/* ------------------------------------------------------------------ */
/* model                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    model_t header;
    float *heights;
} relief_model_t;

static void relief_update_model(model_t *model, perframe_t *perframe) {
    relief_uniforms_t *uniforms =
        (relief_uniforms_t*)program_get_uniforms(model->program);
    mat4_t model_matrix = model->transform;
    uniforms->model_matrix = model_matrix;
    uniforms->inv_model_matrix = mat4_inverse(model_matrix);
    uniforms->normal_matrix =
        mat3_inverse_transpose(mat3_from_mat4(model_matrix));
    uniforms->camera_vp_matrix =
        mat4_mul_mat4(perframe->camera_proj_matrix,
                      perframe->camera_view_matrix);
    uniforms->camera_pos = perframe->camera_pos;
    uniforms->light_dir = perframe->light_dir;
    uniforms->ambient_intensity =
        float_clamp(perframe->ambient_intensity, 0, 5);
    uniforms->punctual_intensity =
        float_clamp(perframe->punctual_intensity, 0, 5);
}

static void relief_draw_model(model_t *model, framebuffer_t *framebuffer,
                              int shadow_pass) {
    mesh_t *mesh = model->mesh;
    int num_faces = mesh_get_num_faces(mesh);
    vertex_t *vertices = mesh_get_vertices(mesh);
    program_t *program = model->program;
    int i, j;

    if (shadow_pass) {
        return;                           /* no shadow pass support */
    }
    for (i = 0; i < num_faces; i++) {
        for (j = 0; j < 3; j++) {
            vertex_t vertex = vertices[i * 3 + j];
            relief_attribs_t *attribs =
                (relief_attribs_t*)program_get_attribs(program, j);
            attribs->local_position = vertex.position;
            attribs->texcoord = vertex.texcoord;
        }
        graphics_draw_triangle(framebuffer, program);
    }
}

static void relief_release_model(model_t *model) {
    relief_model_t *relief = (relief_model_t*)model;
    program_release(model->program);
    mesh_release(model->mesh);
    free(relief->heights);
    free(relief);
}

static int valid_material(const relief_material_t *material) {
    int count, i;
    if (material == NULL || material->heights == NULL) {
        return 0;
    }
    if (material->width < 2 || material->height < 2) {
        return 0;
    }
    if (!isfinite(material->scale) || material->scale < 0) {
        return 0;
    }
    if (!isfinite(material->near_plane) || !isfinite(material->far_plane) ||
        !(material->near_plane > 0) ||
        !(material->far_plane > material->near_plane)) {
        return 0;
    }
    if (!isfinite(material->basecolor.x) ||
        !isfinite(material->basecolor.y) ||
        !isfinite(material->basecolor.z)) {
        return 0;
    }
    count = material->width * material->height;
    for (i = 0; i < count; i++) {
        float height = material->heights[i];
        if (!isfinite(height) || height < 0 || height > 1) {
            return 0;
        }
    }
    return 1;
}

static mesh_t *create_base_mesh(void) {
    vertex_t *vertices = (vertex_t*)malloc(sizeof(vertex_t) * 6);
    vec3_t positions[6] = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0},
        {0, 0, 0}, {1, 1, 0}, {0, 1, 0},
    };
    vec2_t texcoords[6] = {
        {0, 0}, {1, 0}, {1, 1},
        {0, 0}, {1, 1}, {0, 1},
    };
    int i;
    for (i = 0; i < 6; i++) {
        vertices[i].position = positions[i];
        vertices[i].texcoord = texcoords[i];
        vertices[i].normal = vec3_new(0, 0, 1);
        vertices[i].tangent = vec4_new(1, 0, 0, 1);
        vertices[i].joint = vec4_new(0, 0, 0, 0);
        vertices[i].weight = vec4_new(0, 0, 0, 0);
    }
    return mesh_create(vertices, 2);
}

model_t *relief_create_model(const relief_material_t *material,
                             mat4_t transform) {
    relief_model_t *relief;
    relief_uniforms_t *uniforms;
    program_t *program;
    int count;

    if (!valid_material(material)) {
        return NULL;
    }

    program = program_create_deep(relief_vertex_shader,
                                  relief_fragment_shader,
                                  sizeof(relief_attribs_t),
                                  sizeof(relief_varyings_t),
                                  sizeof(relief_uniforms_t),
                                  0, 0);

    relief = (relief_model_t*)malloc(sizeof(relief_model_t));
    count = material->width * material->height;
    relief->heights = (float*)malloc(sizeof(float) * count);
    memcpy(relief->heights, material->heights, sizeof(float) * count);

    uniforms = (relief_uniforms_t*)program_get_uniforms(program);
    uniforms->heights = relief->heights;
    uniforms->grid_width = material->width;
    uniforms->grid_height = material->height;
    uniforms->scale = material->scale;
    uniforms->near_plane = material->near_plane;
    uniforms->far_plane = material->far_plane;
    uniforms->basecolor = material->basecolor;

    relief->header.mesh = create_base_mesh();
    relief->header.program = program;
    relief->header.transform = transform;
    relief->header.skeleton = NULL;
    relief->header.attached = -1;
    relief->header.opaque = 1;
    relief->header.distance = 0;
    relief->header.update = relief_update_model;
    relief->header.draw = relief_draw_model;
    relief->header.release = relief_release_model;
    return &relief->header;
}
