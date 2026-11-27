#pragma once
#include "geometry.h"
#include "model.h"
#include <memory>
#include <vector>

// Owned, editable scene built from a const Model. Positions are shared by the
// original position index (one entry per Model vertex); UVs are stored per
// face corner and are never merged even when two corners share a position.
struct DiffScene {
    static std::unique_ptr<DiffScene> from_model(const Model &model, const int width, const int height);

    int width()  const { return out_w; }
    int height() const { return out_h; }
    int num_positions() const { return static_cast<int>(positions.size()); }
    int num_faces()     const { return static_cast<int>(face_pos.size()) / 3; }
    int num_corner_uvs() const { return static_cast<int>(corner_uv.size()); }

    vec4  position(const int i) const { return positions[i]; }
    vec4& position(const int i)       { return positions[i]; }
    vec2  uv(const int face, const int corner) const { return corner_uv[face*3+corner]; }
    vec2& uv(const int face, const int corner)       { return corner_uv[face*3+corner]; }
    int   face_position_index(const int face, const int corner) const { return face_pos[face*3+corner]; }

    int texture_width()  const { return tex_w; }
    int texture_height() const { return tex_h; }
    double  texel(const int x, const int y, const int channel) const { return texture[(y*tex_w+x)*3+channel]; }
    double& texel(const int x, const int y, const int channel)       { return texture[(y*tex_w+x)*3+channel]; }
    void resize_texture(const int w, const int h); // replaces the texture with zeros; w,h must be >= 2

    const mat<4,4>& model_view()  const { return mv; }
    const mat<4,4>& perspective() const { return persp; }
    const mat<4,4>& viewport()    const { return vp; }

    // factory fills these; users edit through the accessors above
    std::vector<vec4> positions;
    std::vector<vec2> corner_uv;
    std::vector<int>  face_pos;
    std::vector<double> texture; // tex_w*tex_h*3, row major, channels are linear R,G,B
    int tex_w = 0, tex_h = 0;
    mat<4,4> mv, persp, vp;
    int out_w = 0, out_h = 0;
};

// Linear (unclamped) RGB output, z/w depth and winning face id per pixel.
// Background pixels are rgb = 0, depth = 0, face = -1.
struct DiffImage {
    int width = 0, height = 0;
    std::vector<double> rgb;   // width*height*3, channels R,G,B
    std::vector<double> depth; // width*height
    std::vector<int>    face;  // width*height, -1 for background
};

// Self-owned forward record: it snapshots every forward value the adjoint
// needs. Destroying the Model, editing the scene, changing the global camera
// or rendering another frame leaves it untouched.
struct DiffPixelRecord {
    int x = 0, y = 0;
    int face = -1;
    double lam[3]  = {0,0,0}; // screen-space barycentric weights
    double beta[3] = {0,0,0}; // perspective-corrected weights
    double w[3]    = {0,0,0}; // clip-space w of the three corners
    double qx[3]   = {0,0,0}, qy[3] = {0,0,0}; // ndc x/y of the three corners
    double corner_u[3] = {0,0,0}, corner_v[3] = {0,0,0}; // face-corner UV snapshot
    double u = 0, v = 0;      // raw (pre-clamp) interpolated UV
    int i0 = 0, j0 = 0;       // bilinear base cell
    double fx = 0, fy = 0;    // bilinear fractions
    int tex_cell[4] = {0,0,0,0}; // flat texel base indices (times 3)
    double tex[4][3] = {};       // sampled texel values, record-owned snapshot
    double shade = 0;           // 0.2 + 0.8*max(0, N.L)
    bool lit = false;           // N.L > 0 (max derivative is zero at/under zero)
};

struct DiffFaceRecord {
    double b_col0[3] = {0,0,0}; // columns of abc.invert_transpose()
    double b_col1[3] = {0,0,0};
    double area = 0;           // screen-space triangle determinant
    double vscale_x = 0, vscale_y = 0;
    double e1[3] = {0,0,0}, e2[3] = {0,0,0}; // view-space p1-p0, p2-p0
    double normal_len = 0;
    double N[3] = {0,0,0};
    int pos_idx[3] = {0,0,0};
};

struct DiffRecord {
    int width = 0, height = 0;
    int tex_w = 0, tex_h = 0;
    int n_positions = 0;
    mat<4,4> mv{}, persp{}, vp{};
    double light[3] = {0,0,0};
    std::vector<DiffFaceRecord> faces;
    std::vector<DiffPixelRecord> pixels; // winning fragments only, in draw order
};

struct DiffGradients {
    std::vector<vec4> positions;   // one vec4 per scene position
    std::vector<vec2> uvs;         // one vec2 per face corner
    std::vector<double> texture;   // tex_w*tex_h*3
    int tex_w = 0, tex_h = 0;
};

// light_dir is a unit, view-space direction pointing toward the light.
std::unique_ptr<DiffRecord> diff_render(const DiffScene &scene, const vec3 &light_dir, DiffImage &image);

// g holds one RGB adjoint per pixel (width*height*3). The returned gradients
// are freshly zeroed and belong to the caller; the record is untouched, so the
// same record can be back-propagated with different g any number of times.
DiffGradients diff_backward(const DiffRecord &record, const std::vector<double> &g);
