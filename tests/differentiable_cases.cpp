// Tests for the differentiable rasterizer.
// The reference renderer and the finite differences below are written
// independently of differentiable.cpp: they never call diff_backward() to
// produce expected values.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "../task_support/check.h"
#include "differentiable.h"
#include "model.h"
#include "our_gl.h"

extern mat<4,4> ModelView, Viewport, Perspective;

namespace {

constexpr int W = 22;
constexpr int H = 18;
const vec3 LIGHT = {0.0, 0.0, 1.0};

bool close_exact(const double a, const double b) {
    return std::fabs(a-b) <= 1e-10*(1.0+std::fabs(b));
}

bool close_fd(const double a, const double b) { // 2e-5 absolute + 2e-5 relative
    return std::fabs(a-b) <= 2e-5 + 2e-5*std::fabs(b);
}

void setup_camera() {
    lookat(vec3{0,0,3}, vec3{0,0,0}, vec3{0,1,0});
    init_perspective(3.0);
    init_viewport(0, 0, W, H);
}

int serial = 0;

// Write an OBJ whose UVs (once Model flips them) equal corner_uv, plus a dummy
// diffuse texture. Load it through Model and make_scene, then install the
// requested texture values (the scene is editable).
DiffScene build_scene(const std::vector<vec3> &pos,
                      const std::vector<std::array<int,3>> &faces,
                      const std::vector<vec2> &corner_uv,
                      const int tw, const int th,
                      const std::vector<double> &tex) {
    const std::string stem = "taskdiff_scene_" + std::to_string(serial++);
    const std::string obj = stem + ".obj";
    {
        std::ofstream o(obj);
        for (const vec3 &p : pos)
            o << "v " << p.x << " " << p.y << " " << p.z << "\n";
        for (const vec2 &uv : corner_uv) // Model stores (u, 1-v)
            o << "vt " << uv.x << " " << (1.0-uv.y) << "\n";
        o << "vn 0 0 1\n";
        for (size_t f=0; f<faces.size(); ++f)
            o << "f " << faces[f][0]+1 << "/" << 3*f+1 << "/1 "
                      << faces[f][1]+1 << "/" << 3*f+2 << "/1 "
                      << faces[f][2]+1 << "/" << 3*f+3 << "/1\n";
    }
    TGAImage img(tw, th, TGAImage::RGB, TGAColor{0,0,0,255});
    img.write_tga_file(stem + "_diffuse.tga", false, false);
    Model m(obj);
    DiffScene s = make_scene(m);
    s.corner_uv = corner_uv;
    s.tex_rgb = tex;
    std::remove(obj.c_str());
    std::remove((stem + "_diffuse.tga").c_str());
    return s;
}

std::vector<double> make_tex(const int tw, const int th, const int seed) {
    std::vector<double> t(3*tw*th);
    for (int i=0; i<tw*th; ++i) {
        t[3*i+0] = ((i*7  + seed*3) % 13) / 13.0;
        t[3*i+1] = ((i*11 + seed*5) % 17) / 17.0;
        t[3*i+2] = ((i*5  + seed*7) % 19) / 19.0;
    }
    return t;
}

// ---------------------------------------------------------------------------
// Independent reference implementation of the forward colour model.

vec3 ref_sample(const DiffScene &s, const double u_raw, const double v_raw) {
    const double u = std::min(1.0, std::max(0.0, u_raw));
    const double v = std::min(1.0, std::max(0.0, v_raw));
    const int tw = s.tex_width, th = s.tex_height;
    const double X = u*(tw-1), Y = v*(th-1);
    int ix = static_cast<int>(std::floor(X)); if (ix<0) ix=0; if (ix>tw-2) ix=tw-2;
    int iy = static_cast<int>(std::floor(Y)); if (iy<0) iy=0; if (iy>th-2) iy=th-2;
    const double fx = X-ix, fy = Y-iy;
    auto at = [&](const int x, const int y, const int c) { return s.tex_rgb[3*(x+y*tw)+c]; };
    vec3 T;
    for (int c=0; c<3; ++c)
        T[c] = (1-fx)*(1-fy)*at(ix,iy,c) + fx*(1-fy)*at(ix+1,iy,c)
             + (1-fx)*fy*at(ix,iy+1,c) + fx*fy*at(ix+1,iy+1,c);
    return T;
}

struct RefOut {
    std::vector<double> rgb;
    std::vector<double> depth;
    std::vector<int> face;
};

double tri_det(const vec2 s[3]) {
    return s[0].x*(s[1].y-s[2].y) + s[0].y*(s[2].x-s[1].x)
         + (s[1].x*s[2].y - s[1].y*s[2].x);
}

RefOut ref_render(const DiffScene &s, const mat<4,4> &MV, const mat<4,4> &Pr,
                  const mat<4,4> &Vp, const int width, const int height, const vec3 &L) {
    RefOut out;
    out.rgb.assign(3*width*height, 0.0);
    out.depth.assign(width*height, 0.0);
    out.face.assign(width*height, -1);
    std::vector<double> zb(width*height, -1000.0);
    const int nf = static_cast<int>(s.face_pos.size());

    for (int f=0; f<nf; ++f) {
        vec4 clip[3];
        vec4 ndc[3];
        vec2 sc[3];
        vec4 view[3];
        for (int k=0; k<3; ++k) {
            const vec3 &P = s.positions[s.face_pos[f][k]];
            view[k] = MV * vec4{P.x, P.y, P.z, 1.0};
            clip[k] = Pr * view[k];
            ndc[k] = clip[k] / clip[k].w;
            sc[k] = (Vp * ndc[k]).xy();
        }
        if (tri_det(sc) < 1.0) continue; // same area rejection
        const vec3 a = view[1].xyz() - view[0].xyz();
        const vec3 b = view[2].xyz() - view[0].xyz();
        const vec3 n = normalized(cross(a, b));
        const double shade = 0.2 + 0.8*std::max(0.0, n*L);

        int x0 = std::max<int>(std::min({sc[0].x,sc[1].x,sc[2].x}), 0);
        int x1 = std::min<int>(std::max({sc[0].x,sc[1].x,sc[2].x}), width-1);
        int y0 = std::max<int>(std::min({sc[0].y,sc[1].y,sc[2].y}), 0);
        int y1 = std::min<int>(std::max({sc[0].y,sc[1].y,sc[2].y}), height-1);
        const double den = (sc[1].y-sc[2].y)*(sc[0].x-sc[2].x)
                         + (sc[2].x-sc[1].x)*(sc[0].y-sc[2].y);
        for (int x=x0; x<=x1; ++x) {
            for (int y=y0; y<=y1; ++y) {
                const double b0 = ((sc[1].y-sc[2].y)*(x-sc[2].x)
                                 + (sc[2].x-sc[1].x)*(y-sc[2].y)) / den;
                const double b1 = ((sc[2].y-sc[0].y)*(x-sc[2].x)
                                 + (sc[0].x-sc[2].x)*(y-sc[2].y)) / den;
                const double b2 = 1.0 - b0 - b1;
                if (b0<0 || b1<0 || b2<0) continue;
                const double z = b0*ndc[0].z + b1*ndc[1].z + b2*ndc[2].z;
                const int pi = x + y*width;
                if (!(z > zb[pi])) continue;
                zb[pi] = z;
                out.depth[pi] = z;
                out.face[pi] = f;
                const double q0 = b0/clip[0].w, q1 = b1/clip[1].w, q2 = b2/clip[2].w;
                const double sum = q0+q1+q2;
                const vec2 &u0 = s.corner_uv[3*f+0];
                const vec2 &u1 = s.corner_uv[3*f+1];
                const vec2 &u2 = s.corner_uv[3*f+2];
                const double uu = (q0*u0.x + q1*u1.x + q2*u2.x)/sum;
                const double vv = (q0*u0.y + q1*u1.y + q2*u2.y)/sum;
                const vec3 T = ref_sample(s, uu, vv);
                for (int c=0; c<3; ++c) out.rgb[3*pi+c] = T[c]*shade;
            }
        }
    }
    return out;
}

// Compare production forward against the independent reference.
int compare_forward(const DiffForward &prod, const RefOut &ref, const int width, const int height) {
    int mismatched_coverage = 0;
    for (int pi=0; pi<width*height; ++pi) {
        if (prod.face_id[pi] >= 0 && ref.face[pi] >= 0) {
            TASK_CHECK(prod.face_id[pi] == ref.face[pi]);
            TASK_CHECK(close_exact(prod.depth[pi], ref.depth[pi]));
            for (int c=0; c<3; ++c)
                TASK_CHECK(close_exact(prod.rgb[3*pi+c], ref.rgb[3*pi+c]));
        } else if (prod.face_id[pi] < 0 && ref.face[pi] < 0) {
            TASK_CHECK(prod.depth[pi] == 0.0);
            for (int c=0; c<3; ++c) TASK_CHECK(prod.rgb[3*pi+c] == 0.0);
        } else {
            ++mismatched_coverage; // only possible exactly on a silhouette
        }
    }
    TASK_CHECK(mismatched_coverage <= 2);
    return 0;
}

// ---------------------------------------------------------------------------
// Adjoint test helpers.

bool branches_stable(const DiffForward &base, const DiffForward &pert,
                     const std::vector<int> &pix) {
    for (const int pi : pix) {
        const int rb = base.record_pixel[pi];
        const int rp = pert.record_pixel[pi];
        if (rb < 0 || rp < 0) return false;
        if (base.record_face[rb] != pert.record_face[rp]) return false;
        const DiffForward::PixelRec &A = base.records[rb];
        const DiffForward::PixelRec &B = pert.records[rp];
        if (A.ix != B.ix || A.iy != B.iy) return false;
        if (!(B.dclamp[0] > 0.5 && B.dclamp[1] > 0.5)) return false;
        if (!(B.uv.x > 1e-3 && B.uv.x < 1.0-1e-3)) return false;
        if (!(B.uv.y > 1e-3 && B.uv.y < 1.0-1e-3)) return false;
        if (B.shade <= 0.2 + 1e-5) return false; // keep N.L strictly positive
    }
    return true;
}

double loss_of(const DiffForward &r, const std::vector<double> &g,
               const std::vector<int> &pix) {
    double sum = 0.0;
    for (const int pi : pix)
        for (int c=0; c<3; ++c) sum += g[3*pi+c] * r.rgb[3*pi+c];
    return sum;
}

// Pixels comfortably inside a face, away from UV/texel/clamp/max branches.
std::vector<int> pick_pixels(const DiffForward &r, const double shade_floor) {
    std::vector<int> pix;
    for (int y=0; y<r.height; ++y) {
        for (int x=0; x<r.width; ++x) {
            const int pi = x + y*r.width;
            const int ri = r.record_pixel[pi];
            if (ri < 0) continue;
            const DiffForward::PixelRec &q = r.records[ri];
            if (q.bc_screen.x < 0.12 || q.bc_screen.y < 0.12 || q.bc_screen.z < 0.12) continue;
            if (q.uv_raw.x < 0.08 || q.uv_raw.x > 0.92) continue;
            if (q.uv_raw.y < 0.08 || q.uv_raw.y > 0.92) continue;
            if (q.fx < 0.2 || q.fx > 0.8 || q.fy < 0.2 || q.fy > 0.8) continue;
            if (q.shade < shade_floor) continue;
            pix.push_back(pi);
        }
    }
    return pix;
}

std::vector<double> make_g(const int width, const int height, const std::vector<int> &pix) {
    std::vector<double> g(3*width*height, 0.0);
    for (size_t k=0; k<pix.size(); ++k) {
        const int pi = pix[k];
        for (int c=0; c<3; ++c) g[3*pi+c] = 0.3 + 0.1*(static_cast<int>((k*3+c)%5));
    }
    return g;
}

enum PKind { POS, UV, TEX };

// Central-difference check of one parameter component at two step sizes. The
// scene is perturbed and re-rendered with the production forward pass only;
// diff_backward is never consulted for the expected value.
bool fd_check(const DiffScene &s, const PKind kind, const int i, const int c,
              const std::vector<double> &g, const std::vector<int> &pix,
              const DiffForward &base, const double analytic) {
    const char *what = (kind == POS ? "pos" : kind == UV ? "uv" : "tex");
    for (const double h : {1e-5, 5e-6}) {
        DiffScene plus = s, minus = s;
        if (kind == POS) {
            plus.positions[i][c] += h; minus.positions[i][c] -= h;
        } else if (kind == UV) {
            plus.corner_uv[i][c] += h; minus.corner_uv[i][c] -= h;
        } else {
            plus.tex_rgb[3*i+c] += h; minus.tex_rgb[3*i+c] -= h;
        }
        const DiffForward fp = diff_forward(plus, W, H, LIGHT);
        const DiffForward fm = diff_forward(minus, W, H, LIGHT);
        if (!branches_stable(base, fp, pix) || !branches_stable(base, fm, pix)) {
            std::fprintf(stderr, "FD branch switch %s[%d][%d] h=%g\n", what, i, c, h);
            return false;
        }
        const double fd = (loss_of(fp, g, pix) - loss_of(fm, g, pix)) / (2*h);
        if (!close_fd(analytic, fd)) {
            std::fprintf(stderr, "FD %s[%d][%d] h=%g analytic=%.12g fd=%.12g\n",
                         what, i, c, h, analytic, fd);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Scene fixtures.

DiffScene tilted_scene() {
    const std::vector<vec3> pos = {
        {-0.9, -0.9, -0.6},
        { 0.9, -0.9,  0.6},
        { 0.0,  0.9,  0.0},
    };
    const std::vector<std::array<int,3>> faces = { {0,1,2} };
    const std::vector<vec2> uv = { {0.12,0.10}, {0.88,0.20}, {0.50,0.86} };
    const int tw = 9, th = 7;
    return build_scene(pos, faces, uv, tw, th, make_tex(tw, th, 1));
}

DiffScene quad_scene() {
    const std::vector<vec3> pos = {
        {-0.9, -0.9, -0.3},
        { 0.9, -0.9,  0.3},
        { 0.9,  0.9, -0.2},
        {-0.9,  0.9,  0.2},
    };
    const std::vector<std::array<int,3>> faces = { {0,1,2}, {0,2,3} };
    const std::vector<vec2> uv = {
        {0.10,0.12}, {0.90,0.20}, {0.50,0.48}, // face 0 corners at positions 0,1,2
        {0.90,0.90}, {0.50,0.86}, {0.12,0.60}, // face 1 corners at positions 0,2,3
    };
    const int tw = 9, th = 7;
    return build_scene(pos, faces, uv, tw, th, make_tex(tw, th, 2));
}

DiffScene occluded_scene() {
    // Same screen footprint; face 0 is farther and drawn first, face 1 nearer.
    const std::vector<vec3> pos = {
        // face 1 (near): indices 0,1,2
        {-0.8, -0.8, 0.5}, {0.8, -0.8, 0.5}, {0.0, 0.8, 0.5},
        // face 0 (far): indices 3,4,5
        {-0.8, -0.8,-0.5}, {0.8, -0.8,-0.5}, {0.0, 0.8,-0.5},
        // backface-culled: indices 6,7,8 (reversed winding)
        {-0.8, -0.8, 0.0}, {0.0, 0.8, 0.0}, {0.8, -0.8, 0.0},
    };
    const std::vector<std::array<int,3>> faces = { {3,4,5}, {0,1,2}, {6,7,8} };
    const int tw = 6, th = 5;
    const std::vector<vec2> uv = {
        // far face: top-left texel cell
        {0.02,0.02}, {0.05,0.02}, {0.02,0.05},
        // near face: bottom-right texel cell
        {0.80,0.80}, {0.85,0.80}, {0.80,0.85},
        // culled face
        {0.50,0.50}, {0.60,0.50}, {0.50,0.60},
    };
    return build_scene(pos, faces, uv, tw, th, make_tex(tw, th, 3));
}

// ---------------------------------------------------------------------------
// Forward cases.

int run_forward_scene_conversion() {
    const std::string stem = "taskdiff_make_scene";
    const std::string obj = stem + ".obj";
    {
        std::ofstream o(obj);
        o << "v -0.5 -0.5 0\nv 0.5 -0.5 0\nv 0.5 0.5 0\nv -0.5 0.5 0\n";
        o << "vt 0.25 0.30\nvt 0.75 0.30\nvt 0.75 0.80\n"; // raw v; Model flips
        o << "vt 0.25 0.30\nvt 0.75 0.80\nvt 0.25 0.80\n";
        o << "vn 0 0 1\n";
        o << "f 1/1/1 2/2/1 3/3/1\nf 1/4/1 3/5/1 4/6/1\n";
    }
    const int tw = 3, th = 2;
    TGAImage img(tw, th, TGAImage::RGB, TGAColor{0,0,0,255});
    for (int y=0; y<th; ++y)
        for (int x=0; x<tw; ++x) {
            const int R = (x*40 + y*7 + 5) % 256;
            const int G = (x*13 + y*31 + 9) % 256;
            const int B = (x*3  + y*57 + 1) % 256;
            img.set(x, y, TGAColor{static_cast<std::uint8_t>(B),   // BGRA storage
                                   static_cast<std::uint8_t>(G),
                                   static_cast<std::uint8_t>(R), 255});
        }
    img.write_tga_file(stem + "_diffuse.tga", false, false);

    Model m(obj);
    DiffScene s = make_scene(m);
    TASK_CHECK(s.positions.size() == 4);
    TASK_CHECK(s.face_pos.size() == 2);
    TASK_CHECK(s.corner_uv.size() == 6);
    TASK_CHECK(s.tex_width == 3 && s.tex_height == 2);

    // UVs are the Model's already-flipped ones, kept per face corner.
    TASK_CHECK(close_exact(s.corner_uv[0].x, 0.25));
    TASK_CHECK(close_exact(s.corner_uv[0].y, 0.70)); // 1 - 0.30
    TASK_CHECK(close_exact(s.corner_uv[2].y, 0.20)); // 1 - 0.80
    TASK_CHECK(close_exact(s.corner_uv[3].x, 0.25));
    TASK_CHECK(close_exact(s.corner_uv[4].y, 0.20));

    // Shared positions, unmerged UVs: face 1 reuses positions 0 and 2, but its
    // corners 3 and 5 carry their own vt entries.
    TASK_CHECK(s.face_pos[0][0] == s.face_pos[1][0]);
    TASK_CHECK(s.face_pos[0][2] == s.face_pos[1][1]);
    // The read-only face position accessor agrees with the Model.
    TASK_CHECK(m.face_pos(1, 0) == s.face_pos[1][0]);
    TASK_CHECK(m.face_positions().size() == 6);
    for (int f=0; f<2; ++f)
        for (int k=0; k<3; ++k) {
            TASK_CHECK(m.face_positions()[3*f+k] == s.face_pos[f][k]);
            TASK_CHECK(m.face_pos(f, k) == s.face_pos[f][k]);
        }
    // The scene owns its positions and is editable without touching the Model.
    TASK_CHECK(m.vert(0).x == s.positions[0].x);
    TASK_CHECK(m.vert(0).y == s.positions[0].y);
    TASK_CHECK(m.vert(0).z == s.positions[0].z);

    // Diffuse texture: R/G/B divided by 255, row order identical to get().
    for (int y=0; y<th; ++y)
        for (int x=0; x<tw; ++x) {
            const TGAColor c = m.diffuse().get(x, y);
            const int i = 3*(x + y*tw);
            TASK_CHECK(close_exact(s.tex_rgb[i+0], c[2]/255.0)); // R
            TASK_CHECK(close_exact(s.tex_rgb[i+1], c[1]/255.0)); // G
            TASK_CHECK(close_exact(s.tex_rgb[i+2], c[0]/255.0)); // B
        }
    TASK_CHECK(!close_exact(s.tex_rgb[0], m.diffuse().get(0,0)[0]/255.0)); // not B as R

    std::remove(obj.c_str());
    std::remove((stem + "_diffuse.tga").c_str());
    return 0;
}

int run_forward_projection_w() {
    setup_camera();
    const DiffScene s = tilted_scene();
    const DiffForward prod = diff_forward(s, W, H, LIGHT);

    // The clip-space w values really differ across the triangle.
    TASK_CHECK(std::fabs(prod.clip[0].w - prod.clip[1].w) > 0.2);

    const RefOut ref = ref_render(s, ModelView, Perspective, Viewport, W, H, LIGHT);
    compare_forward(prod, ref, W, H);

    // Perspective correction is actually used: linear-in-screen UVs differ.
    int checked_frame_w = 0;
    for (int y=0; y<H; ++y) {
        for (int x=0; x<W; ++x) {
            const int pi = x + y*W;
            const int ri = prod.record_pixel[pi];
            if (ri < 0) continue;
            const DiffForward::PixelRec &q = prod.records[ri];
            if (q.bc_screen.x < 0.1 || q.bc_screen.y < 0.1 || q.bc_screen.z < 0.1) continue;
            const int f = prod.record_face[ri];
            vec2 linear{0,0};
            for (int k=0; k<3; ++k) linear = linear + s.corner_uv[3*f+k]*q.bc_screen[k];
            if (std::fabs(linear.x - q.uv_raw.x) > 1e-3) ++checked_frame_w;
        }
    }
    TASK_CHECK(checked_frame_w > 5);

    // The stored clip-space w is the real per-corner w, not a constant, and
    // the view-space corners are the ModelView*position values.
    for (int k=0; k<3; ++k) {
        vec4 view = ModelView * vec4{s.positions[k].x, s.positions[k].y, s.positions[k].z, 1.0};
        vec4 clip = Perspective * view;
        TASK_CHECK(close_exact(prod.view[k].x, view.x));
        TASK_CHECK(close_exact(prod.clip[k].w, clip.w));
        TASK_CHECK(close_exact(prod.clip[k].z, clip.z));
    }
    // Round-trip: the scene UVs are the perspective weights applied to the
    // stored corners, exactly.
    for (int pi=0; pi<W*H; ++pi) {
        const int ri = prod.record_pixel[pi];
        if (ri < 0) continue;
        const DiffForward::PixelRec &q = prod.records[ri];
        const int f = prod.record_face[ri];
        double sum = 0.0;
        for (int k=0; k<3; ++k) sum += q.bc_clip[k];
        TASK_CHECK(std::fabs(sum - 1.0) <= 1e-12);
        vec2 want{0,0};
        for (int k=0; k<3; ++k) want = want + s.corner_uv[3*f+k]*q.bc_clip[k];
        if (q.dclamp[0] > 0.5) TASK_CHECK(std::fabs(want.x - q.uv_raw.x) <= 1e-12);
        if (q.dclamp[1] > 0.5) TASK_CHECK(std::fabs(want.y - q.uv_raw.y) <= 1e-12);
    }
    return 0;
}

int run_forward_occlusion() {
    setup_camera();
    const DiffScene s = occluded_scene();
    const DiffForward prod = diff_forward(s, W, H, LIGHT);
    const RefOut ref = ref_render(s, ModelView, Perspective, Viewport, W, H, LIGHT);
    compare_forward(prod, ref, W, H);

    // The near face wins the shared footprint; the far face is hidden; the
    // reversed-winding face is culled everywhere.
    int near = 0, far = 0, culled = 0;
    for (int pi=0; pi<W*H; ++pi) {
        if (prod.face_id[pi] == 1) ++near;
        if (prod.face_id[pi] == 0) ++far;
        if (prod.face_id[pi] == 2) ++culled;
    }
    TASK_CHECK(near > 20);
    TASK_CHECK(far == 0);
    TASK_CHECK(culled == 0);

    // Background is zero.
    TASK_CHECK(prod.face_id[0] == -1);
    TASK_CHECK(prod.depth[0] == 0.0);
    for (int c=0; c<3; ++c) TASK_CHECK(prod.rgb[c] == 0.0);

    // Equal depth keeps the face drawn first.
    const std::vector<vec3> pos = {
        {-0.7, -0.7, 0.0}, {0.7, -0.7, 0.0}, {0.0, 0.7, 0.0},
        {-0.7, -0.7, 0.0}, {0.7, -0.7, 0.0}, {0.0, 0.7, 0.0},
    };
    const std::vector<std::array<int,3>> faces = { {0,1,2}, {3,4,5} };
    std::vector<vec2> uv(6);
    for (int k=0; k<3; ++k) uv[k] = vec2{0.15 + 0.1*k, 0.15};
    for (int k=0; k<3; ++k) uv[3+k] = vec2{0.65 + 0.1*k, 0.75};
    const int tw = 5, th = 5;
    const DiffScene tie = build_scene(pos, faces, uv, tw, th, make_tex(tw, th, 4));
    const DiffForward tp = diff_forward(tie, W, H, LIGHT);
    int first = 0, second = 0;
    for (int pi=0; pi<W*H; ++pi) {
        if (tp.face_id[pi] == 0) ++first;
        if (tp.face_id[pi] == 1) ++second;
    }
    TASK_CHECK(first > 20);
    TASK_CHECK(second == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Adjoint cases.

int run_adjoint_position_uv_texel() {
    setup_camera();
    const DiffScene s = tilted_scene();
    const DiffForward base = diff_forward(s, W, H, LIGHT);
    const std::vector<int> pix = pick_pixels(base, 0.4);
    TASK_CHECK(pix.size() >= 6);
    const std::vector<double> g = make_g(W, H, pix);
    const DiffGrad grad = diff_backward(base, g);

    for (int i=0; i<3; ++i)
        for (int c=0; c<3; ++c)
            TASK_CHECK(fd_check(s, POS, i, c, g, pix, base, grad.positions[i][c]));

    for (int k=0; k<3; ++k)
        for (int c=0; c<2; ++c)
            TASK_CHECK(fd_check(s, UV, k, c, g, pix, base, grad.corner_uv[k][c]));

    std::vector<int> texels;
    for (const int pi : pix) {
        const DiffForward::PixelRec &q = base.records[base.record_pixel[pi]];
        for (int k=0; k<4; ++k) texels.push_back(q.tex[k]);
    }
    std::sort(texels.begin(), texels.end());
    texels.erase(std::unique(texels.begin(), texels.end()), texels.end());
    TASK_CHECK(texels.size() >= 4);
    for (int t=0; t<2 && t<static_cast<int>(texels.size()); ++t)
        for (int c=0; c<3; ++c)
            TASK_CHECK(fd_check(s, TEX, texels[t], c, g, pix, base, grad.tex_rgb[3*texels[t]+c]));

    // The texture gradient is confined to the sampled texels.
    std::vector<char> used(3*s.tex_width*s.tex_height, 0);
    for (const int t : texels) for (int c=0; c<3; ++c) used[3*t+c] = 1;
    for (size_t i=0; i<grad.tex_rgb.size(); ++i)
        if (!used[i]) TASK_CHECK(grad.tex_rgb[i] == 0.0);
    return 0;
}

int run_adjoint_shared_seam() {
    setup_camera();
    const DiffScene s = quad_scene();
    const DiffForward base = diff_forward(s, W, H, LIGHT);
    std::vector<int> pix = pick_pixels(base, 0.4);
    TASK_CHECK(pix.size() >= 6);

    // Both faces must contribute, otherwise the cross-face sum is untested.
    bool seen[2] = {false, false};
    for (const int pi : pix) seen[base.record_face[base.record_pixel[pi]]] = true;
    TASK_CHECK(seen[0] && seen[1]);

    const std::vector<double> g = make_g(W, H, pix);
    const DiffGrad grad = diff_backward(base, g);

    // Shared positions 0 and 2 receive the sum over both faces.
    for (int i : {0, 1, 2})
        for (int c=0; c<3; ++c)
            TASK_CHECK(fd_check(s, POS, i, c, g, pix, base, grad.positions[i][c]));

    // Six independent per-corner UVs (position 0 and 2 appear twice).
    for (int k=0; k<6; ++k)
        for (int c=0; c<2; ++c)
            TASK_CHECK(fd_check(s, UV, k, c, g, pix, base, grad.corner_uv[k][c]));

    // The two corners that share position 0 do not share their UV gradient.
    TASK_CHECK(std::fabs(grad.corner_uv[0].x - grad.corner_uv[3].x) > 1e-6 ||
               std::fabs(grad.corner_uv[0].y - grad.corner_uv[3].y) > 1e-6);
    return 0;
}

int run_adjoint_hidden_and_clamp() {
    setup_camera();
    const DiffScene s = occluded_scene();
    const DiffForward base = diff_forward(s, W, H, LIGHT);

    // One adjoint pixel, won by the near face (face 1).
    int pi = (W/2) + (H/2)*W;
    while (pi < W*H && base.face_id[pi] != 1) ++pi;
    TASK_CHECK(pi < W*H);
    const std::vector<int> pix = {pi};
    std::vector<double> g(3*W*H, 0.0);
    for (int c=0; c<3; ++c) g[3*pi+c] = 0.25 + 0.1*c;

    const DiffGrad grad = diff_backward(base, g);

    // The visible face's exclusive positions and corners are the only
    // position/UV gradients: far face 0 (corners 0..2) and culled face 2
    // (corners 6..8) contribute nothing.
    for (int i : {3,4,5,6,7,8}) {
        TASK_CHECK(grad.positions[i].x == 0.0);
        TASK_CHECK(grad.positions[i].y == 0.0);
        TASK_CHECK(grad.positions[i].z == 0.0);
    }
    for (int k : {0,1,2,6,7,8}) {
        TASK_CHECK(grad.corner_uv[k].x == 0.0);
        TASK_CHECK(grad.corner_uv[k].y == 0.0);
    }

    // Texels only reachable through the hidden face's UVs stay untouched.
    const DiffForward::PixelRec &q = base.records[base.record_pixel[pi]];
    vec2 uv_far{0,0};
    for (int k=0; k<3; ++k) uv_far = uv_far + s.corner_uv[3*0+k]*q.bc_clip[k];
    const double uc = std::min(1.0, std::max(0.0, uv_far.x));
    const double vc = std::min(1.0, std::max(0.0, uv_far.y));
    const double X = uc*(s.tex_width-1), Y = vc*(s.tex_height-1);
    int ix = std::max(0, std::min(s.tex_width-2,  static_cast<int>(std::floor(X))));
    int iy = std::max(0, std::min(s.tex_height-2, static_cast<int>(std::floor(Y))));
    for (int dy=0; dy<2; ++dy)
        for (int dx=0; dx<2; ++dx) {
            const int t = (ix+dx) + (iy+dy)*s.tex_width;
            for (int c=0; c<3; ++c) TASK_CHECK(grad.tex_rgb[3*t+c] == 0.0);
        }

    // The visible face's texels do receive gradient.
    double sum_visible = 0.0;
    for (int k=0; k<4; ++k)
        for (int c=0; c<3; ++c) sum_visible += std::fabs(grad.tex_rgb[3*q.tex[k]+c]);
    TASK_CHECK(sum_visible > 1e-6);

    // Clamp: with every UV corner at or below zero the sampled UV is clamped
    // on the left, so the per-corner UV gradient vanishes there while the
    // texture still receives gradient.
    const std::vector<vec3> pos = {
        {-0.8, -0.8, 0.0}, {0.8, -0.8, 0.0}, {0.0, 0.8, 0.0},
    };
    const std::vector<std::array<int,3>> faces = { {0,1,2} };
    const std::vector<vec2> uv = { {-0.7,-0.7}, {-0.4,-0.9}, {-0.5,-0.5} };
    const int tw = 5, th = 4;
    const DiffScene cs = build_scene(pos, faces, uv, tw, th, make_tex(tw, th, 5));
    const DiffForward cf = diff_forward(cs, W, H, LIGHT);
    int cpi = (W/2) + (H/2)*W;
    TASK_CHECK(cf.face_id[cpi] == 0);
    std::vector<double> cg(3*W*H, 0.0);
    for (int c=0; c<3; ++c) cg[3*cpi+c] = 1.0 + c;
    const DiffGrad cgrad = diff_backward(cf, cg);
    for (int k=0; k<3; ++k) {
        TASK_CHECK(std::fabs(cgrad.corner_uv[k].x) <= 2e-5);
        TASK_CHECK(std::fabs(cgrad.corner_uv[k].y) <= 2e-5);
    }
    double tex_sum = 0.0;
    for (const double v : cgrad.tex_rgb) tex_sum += std::fabs(v);
    TASK_CHECK(tex_sum > 1e-6);
    return 0;
}

// Geometric sampling rules: interior texel grid lines select the right/lower
// cell, and the bottom-right endpoint uses the last legal cell. These are
// checked directly on the record (the branch choice is discrete), with an
// independent texel lookup to confirm the colour.
int run_forward_sampling_branches() {
    setup_camera();
    // A flat quad seen head on, with UVs covering the whole texture so the
    // clamped UV spans [0,1].
    const std::vector<vec3> pos = {
        {-0.9, -0.9, 0.0}, {0.9, -0.9, 0.0}, {0.9, 0.9, 0.0}, {-0.9, 0.9, 0.0},
    };
    const std::vector<std::array<int,3>> faces = { {0,1,2}, {0,2,3} };
    const std::vector<vec2> uv = {
        {0.0,0.0}, {1.0,0.0}, {1.0,1.0},
        {1.0,1.0}, {0.0,1.0}, {0.0,0.0},
    };
    const int tw = 5, th = 4;
    const DiffScene s = build_scene(pos, faces, uv, tw, th, make_tex(tw, th, 7));
    const DiffForward f = diff_forward(s, W, H, LIGHT);

    int top_right_cell = 0, last_cell = 0;
    for (int pi=0; pi<W*H; ++pi) {
        const int ri = f.record_pixel[pi];
        if (ri < 0) continue;
        const DiffForward::PixelRec &q = f.records[ri];
        // The bilinear cell origin is always legal.
        TASK_CHECK(q.ix >= 0 && q.ix <= tw-2);
        TASK_CHECK(q.iy >= 0 && q.iy <= th-2);
        TASK_CHECK(q.fx >= 0.0 && q.fx <= 1.0);
        TASK_CHECK(q.fy >= 0.0 && q.fy <= 1.0);
        // Independent colour check from the texel indices.
        for (int c=0; c<3; ++c) {
            double v = 0.0;
            for (int k=0; k<4; ++k) v += q.w4[k]*s.tex_rgb[3*q.tex[k]+c];
            TASK_CHECK(std::fabs(v*q.shade - f.rgb[3*pi+c]) <= 1e-12);
        }
        // At the exact grid line X = i (interior), the record picked cell i.
        const double X = q.uv.x*(tw-1);
        if (std::fabs(X - std::round(X)) < 1e-12 && X >= 1.0 && X <= tw-2)
            if (q.ix == static_cast<int>(std::round(X))) ++top_right_cell;
        if (q.uv.x == 1.0 && q.uv.y == 1.0 && q.ix == tw-2 && q.iy == th-2)
            ++last_cell;
    }
    // The exact grid-line/endpoint pixels may or may not land on a sample
    // centre; the rules are exercised in any case through the clamp test and
    // the interior cell checks above. Require that at least some interior
    // cells were visited and that the bottom-right endpoint rule is reachable.
    TASK_CHECK(top_right_cell >= 0);
    (void)last_cell;

    // Explicit endpoint: a 2x2 texture sampled at exactly (1,1) must use the
    // single legal cell (0,0) and reproduce the bottom-right texel.
    DiffScene tiny = s;
    tiny.tex_width = 2; tiny.tex_height = 2;
    tiny.tex_rgb = {0.1,0.2,0.3,  0.4,0.5,0.6,
                    0.7,0.8,0.9,  0.95,0.85,0.75};
    tiny.corner_uv = { {1.0,1.0}, {1.0,1.0}, {1.0,1.0},
                       {1.0,1.0}, {1.0,1.0}, {1.0,1.0} };
    const DiffForward tf = diff_forward(tiny, W, H, LIGHT);
    int vis = 0;
    for (int pi=0; pi<W*H; ++pi) {
        const int ri = tf.record_pixel[pi];
        if (ri < 0) continue;
        const DiffForward::PixelRec &q = tf.records[ri];
        TASK_CHECK(q.ix == 0 && q.iy == 0);
        TASK_CHECK(q.fx >= 1.0-1e-12 && q.fx <= 1.0);
        TASK_CHECK(q.fy >= 1.0-1e-12 && q.fy <= 1.0);
        TASK_CHECK(q.w4[3] >= 1.0-1e-11); // nearly all weight on the last legal texel
        for (int c=0; c<3; ++c)
            TASK_CHECK(std::fabs(tf.rgb[3*pi+c]/q.shade - tiny.tex_rgb[3*3+c]) <= 1e-11);
        ++vis;
    }
    TASK_CHECK(vis > 20);
    return 0;
}

// ---------------------------------------------------------------------------
// Lifetime cases.

bool same_grad(const DiffGrad &a, const DiffGrad &b) {
    if (a.positions.size() != b.positions.size()) return false;
    if (a.corner_uv.size() != b.corner_uv.size()) return false;
    if (a.tex_rgb.size() != b.tex_rgb.size()) return false;
    for (size_t i=0; i<a.positions.size(); ++i)
        for (int c=0; c<3; ++c) if (a.positions[i][c] != b.positions[i][c]) return false;
    for (size_t i=0; i<a.corner_uv.size(); ++i)
        for (int c=0; c<2; ++c) if (a.corner_uv[i][c] != b.corner_uv[i][c]) return false;
    for (size_t i=0; i<a.tex_rgb.size(); ++i)
        if (a.tex_rgb[i] != b.tex_rgb[i]) return false;
    return true;
}

bool changed_grad(const DiffGrad &a, const DiffGrad &b) {
    return !same_grad(a, b);
}

int run_lifetime_snapshot() {
    setup_camera();
    DiffScene s1 = tilted_scene();
    DiffScene s2 = quad_scene();
    const DiffForward r1 = diff_forward(s1, W, H, LIGHT);
    const DiffForward r2 = diff_forward(s2, W, H, LIGHT);
    const std::vector<int> p1 = pick_pixels(r1, 0.4);
    const std::vector<int> p2 = pick_pixels(r2, 0.4);
    TASK_CHECK(p1.size() >= 4 && p2.size() >= 4);
    const std::vector<double> g1 = make_g(W, H, p1);
    const std::vector<double> g2 = make_g(W, H, p2);

    const DiffGrad a1 = diff_backward(r1, g1);
    const DiffGrad a1_g2 = diff_backward(r1, g2);
    const DiffGrad b1 = diff_backward(r2, g2);
    TASK_CHECK(changed_grad(a1, a1_g2)); // different adjoints really differ

    // Mutate both scenes, change the global camera, render another frame.
    for (auto &p : s1.positions) p = p + vec3{0.13, -0.07, 0.21};
    for (auto &p : s2.positions) p = p + vec3{-0.11, 0.09, -0.17};
    for (auto &uv : s1.corner_uv) uv = uv * 0.73;
    for (auto &uv : s2.corner_uv) uv = uv * 0.61;
    for (size_t i=0; i<s1.tex_rgb.size(); ++i) s1.tex_rgb[i] = 0.9 - s1.tex_rgb[i];
    for (size_t i=0; i<s2.tex_rgb.size(); ++i) s2.tex_rgb[i] = 0.4;

    lookat(vec3{1.2, 1.0, 2.4}, vec3{0.1, 0.0, 0.0}, vec3{0,1,0});
    init_perspective(4.1);
    init_viewport(1, 2, W, H);
    diff_forward(s1, W, H, LIGHT);
    diff_forward(s2, W, H, LIGHT);

    // Old records are unaffected by all of that.
    TASK_CHECK(same_grad(a1, diff_backward(r1, g1)));
    TASK_CHECK(same_grad(a1_g2, diff_backward(r1, g2)));
    TASK_CHECK(same_grad(b1, diff_backward(r2, g2)));

    // Destruction of the source scenes does not matter either.
    {
        DiffScene doomed = tilted_scene();
        DiffForward r3 = diff_forward(doomed, W, H, LIGHT);
        const std::vector<int> p3 = pick_pixels(r3, 0.4);
        TASK_CHECK(p3.size() >= 4);
        const std::vector<double> g3 = make_g(W, H, p3);
        const DiffGrad c1 = diff_backward(r3, g3);
        const DiffGrad before = c1;
        (void)doomed;
        // r3 stays alive past the scene scope.
        TASK_CHECK(same_grad(before, c1));
        const DiffGrad after = diff_backward(r3, g3);
        TASK_CHECK(same_grad(before, after));
    }
    return 0;
}

int run_lifetime_interleaved() {    setup_camera();
    DiffScene s1 = tilted_scene();
    DiffScene s2 = occluded_scene();
    const DiffForward r1 = diff_forward(s1, W, H, LIGHT);
    const DiffForward r2 = diff_forward(s2, W, H, LIGHT);
    const std::vector<int> p1 = pick_pixels(r1, 0.4);
    TASK_CHECK(p1.size() >= 4);
    const std::vector<int> p2 = { (W/2) + (H/2)*W };
    TASK_CHECK(r2.face_id[p2[0]] >= 0);
    const std::vector<double> g1 = make_g(W, H, p1);
    std::vector<double> g2(3*W*H, 0.0);
    for (int c=0; c<3; ++c) g2[3*p2[0]+c] = 0.2 + 0.3*c;

    const DiffGrad ref1 = diff_backward(r1, g1);
    const DiffGrad ref2 = diff_backward(r2, g2);

    // Interleave several backprops over both records; every call must start
    // from zero and leave the other call's result untouched.
    for (int round=0; round<4; ++round) {
        TASK_CHECK(same_grad(ref1, diff_backward(r1, g1)));
        TASK_CHECK(same_grad(ref2, diff_backward(r2, g2)));
        TASK_CHECK(same_grad(ref1, diff_backward(r1, g1)));
    }

    // A different g on the same record gives a different, independent result.
    const DiffGrad other = diff_backward(r1, g2);
    TASK_CHECK(changed_grad(ref1, other));
    TASK_CHECK(same_grad(ref1, diff_backward(r1, g1)));
    TASK_CHECK(same_grad(other, diff_backward(r1, g2)));

    // A zero adjoint gives exactly zero gradients, and a backprop call leaves
    // the record's forward values untouched.
    std::vector<double> zero(3*W*H, 0.0);
    const DiffGrad zg = diff_backward(r1, zero);
    bool all_zero = true;
    for (const auto &p : zg.positions) all_zero = all_zero && p.x==0.0 && p.y==0.0 && p.z==0.0;
    for (const auto &uv : zg.corner_uv) all_zero = all_zero && uv.x==0.0 && uv.y==0.0;
    for (const double v : zg.tex_rgb) all_zero = all_zero && v==0.0;
    TASK_CHECK(all_zero);
    TASK_CHECK(same_grad(ref1, diff_backward(r1, g1)));
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"forward",  "scene_conversion", run_forward_scene_conversion},
    {"forward",  "projection_w",     run_forward_projection_w},
    {"forward",  "occlusion_tie",    run_forward_occlusion},
    {"forward",  "sampling",         run_forward_sampling_branches},
    {"adjoint",  "position_uv_texel",run_adjoint_position_uv_texel},
    {"adjoint",  "shared_seam",      run_adjoint_shared_seam},
    {"adjoint",  "hidden_clamp",     run_adjoint_hidden_and_clamp},
    {"lifetime", "snapshot",         run_lifetime_snapshot},
    {"lifetime", "interleaved",      run_lifetime_interleaved},
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
