#pragma once
#include <array>
#include <vector>
#include "geometry.h"

class Model; // build a scene from a Model with make_scene()

// A owning, editable scene built from a Model.
//
// Positions are shared by original position index (like Model::vert(i)); UVs
// are stored per face corner, three per triangle, and are never merged even
// when two corners reference the same position index.
struct DiffScene {
    std::vector<vec3> positions;              // mesh positions, indexed like Model::vert(i)
    std::vector<vec2> corner_uv;              // 3*nfaces UVs, in Model::uv(face, corner) order
    std::vector<std::array<int,3>> face_pos;  // 3*nfaces position indices
    int tex_width = 0, tex_height = 0;        // at least 2x2
    std::vector<double> tex_rgb;              // width*height*3 linear RGB in [0,1], row-major
};

// Build a scene equivalent to the given Model: geometry, per-corner UVs and the
// diffuse texture converted from TGA BGR storage to linear RGB (R,G,B)/255.
// Row/column order matches TGAImage::get (index x + y*width).
DiffScene make_scene(const Model &model);

// Forward record. It owns every forward value needed by backward(); it stays
// valid after the source Model/Scene is destroyed or edited, after the global
// camera changes, and after other frames are rendered.
//
// The colour model is linear diffuse only:
//     C(u,v) = T(u,v) * (0.2 + 0.8*max(0, N.L))
// with N = normalize(cross(P1-P0, P2-P0)) in view space and L the unit
// view-space direction towards the light supplied by the caller. Output is
// unclamped double RGB.
struct DiffForward {
    int width = 0, height = 0;
    std::vector<double> rgb;      // 3*W*H linear RGB, not clamped
    std::vector<double> depth;    // W*H, z/w of the winning fragment, 0 for background
    std::vector<int> face_id;     // W*H, -1 for background

    struct PixelRec {
        vec3   bc_screen;          // screen-space barycentric weights
        vec3   bc_clip;            // perspective-corrected weights (used for UV)
        vec2   uv_raw;             // interpolated (unclamped) UV
        vec2   uv;                 // UV after clamping to [0,1]
        double dclamp[2] = {0,0};  // 1 where clamp is inactive, 0 outside/on endpoints
        int    ix = 0, iy = 0;     // bilinear cell origin (right/bottom choice)
        double fx = 0, fy = 0;     // bilinear fractions
        double shade = 0;          // 0.2 + 0.8*max(0, N.L)
        double w4[4] = {0,0,0,0};  // bilinear weights (ix,iy),(ix+1,iy),(ix,iy+1),(ix+1,iy+1)
        int    tex[4] = {0,0,0,0}; // corresponding texel indices
    };

    std::vector<int> record_pixel;     // W*H -> compact index, -1 for background
    std::vector<int> record_face;      // compact -> face id
    std::vector<PixelRec> records;     // compact forward values

    // Owned per-face snapshots (3 entries per face).
    std::vector<vec4> clip;            // clip-space corners
    std::vector<vec2> screen;          // screen-space corners
    std::vector<vec3> view;            // view-space corners
    std::vector<vec3> normal;          // unit view-space face normal
    std::vector<vec2> corner_uv;       // per-corner UVs
    std::vector<std::array<int,3>> face_pos; // position indices per corner

    mat<4,4> modelview{};              // camera snapshot
    mat<4,4> perspective{};
    mat<4,4> viewport{};
    vec3 light;                        // unit view-space light direction

    int tex_width = 0, tex_height = 0;
    std::vector<double> tex_rgb;       // owned texture snapshot
    int n_positions = 0;
};

// Render one frame using the current global ModelView / Perspective / Viewport
// as a snapshot. Pixel sampling, area culling, depth selection and the
// strict '>' depth comparison follow rasterize(); equal depths keep the face
// drawn first and background pixels are zero.
DiffForward diff_forward(const DiffScene &scene, int width, int height, const vec3 &light);

// Gradients of the scalar loss sum_p g[p] . C[p] w.r.t. scene parameters.
struct DiffGrad {
    std::vector<vec3> positions;   // indexed like DiffScene::positions
    std::vector<vec2> corner_uv;   // 3*nfaces per-corner UVs
    std::vector<double> tex_rgb;   // texture-size RGB, summed per texel
};

// One backward pass. Coverage, culling and face selection are fixed forward
// branches. May be called repeatedly with different g; each call starts from
// zeroed gradients. The record is not modified.
DiffGrad diff_backward(const DiffForward &record, const std::vector<double> &g_rgb);
