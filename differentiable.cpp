#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "differentiable.h"
#include "model.h"
#include "our_gl.h"
#include "raster_core.h"

extern mat<4,4> ModelView, Viewport, Perspective; // global camera state snapshot

namespace {

vec3 texel(const DiffForward &rec, const int idx) {
    return { rec.tex_rgb[3*idx+0], rec.tex_rgb[3*idx+1], rec.tex_rgb[3*idx+2] };
}

// Bilinear cell choice: X = u*(W-1); the cell origin is floor(X) clamped to
// [0, W-2], so an internal grid line takes the right/lower cell and the
// bottom-right endpoint uses the last legal cell. Fractions are computed
// against the (possibly clamped) origin.
struct Bilinear {
    int ix = 0, iy = 0;
    double fx = 0, fy = 0;
    double w[4] = {0,0,0,0};
    int idx[4] = {0,0,0,0};
};

Bilinear bilinear_setup(const int W, const int H, const double uc, const double vc) {
    Bilinear b;
    double X = uc * (W-1);
    double Y = vc * (H-1);
    b.ix = static_cast<int>(std::floor(X));
    if (b.ix < 0) b.ix = 0;
    if (b.ix > W-2) b.ix = W-2;
    b.iy = static_cast<int>(std::floor(Y));
    if (b.iy < 0) b.iy = 0;
    if (b.iy > H-2) b.iy = H-2;
    b.fx = X - b.ix;
    b.fy = Y - b.iy;
    b.w[0] = (1-b.fx)*(1-b.fy);
    b.w[1] = b.fx*(1-b.fy);
    b.w[2] = (1-b.fx)*b.fy;
    b.w[3] = b.fx*b.fy;
    b.idx[0] = b.ix     + b.iy*W;
    b.idx[1] = b.ix + 1 + b.iy*W;
    b.idx[2] = b.ix     + (b.iy+1)*W;
    b.idx[3] = b.ix + 1 + (b.iy+1)*W;
    return b;
}

} // namespace

DiffScene make_scene(const Model &model) {
    DiffScene s;
    s.positions.resize(model.nverts());
    for (int i=0; i<model.nverts(); ++i)
        s.positions[i] = model.vert(i).xyz();

    const int nf = model.nfaces();
    s.face_pos.resize(nf);
    s.corner_uv.resize(3*nf);
    for (int f=0; f<nf; ++f) {
        for (int k=0; k<3; ++k) {
            s.face_pos[f][k] = model.face_pos(f, k);
            s.corner_uv[3*f+k] = model.uv(f, k);
        }
    }

    const TGAImage &diffuse = model.diffuse();
    const int src_w = diffuse.width();
    const int src_h = diffuse.height();
    // The scene texture must be at least 2x2. A Model without a usable diffuse
    // map still yields a valid (zero) 2x2 texture so the scene stays editable.
    const bool usable = src_w >= 2 && src_h >= 2;
    const int W = usable ? src_w : 2;
    const int H = usable ? src_h : 2;
    s.tex_width = W;
    s.tex_height = H;
    s.tex_rgb.assign(3*W*H, 0.0);
    if (usable) {
        for (int y=0; y<H; ++y) {
            for (int x=0; x<W; ++x) {
                TGAColor c = diffuse.get(x, y); // TGA stores BGRA; R/G/B live at 2/1/0
                int i = 3*(x + y*W);
                s.tex_rgb[i+0] = c[2] / 255.0;  // R
                s.tex_rgb[i+1] = c[1] / 255.0;  // G
                s.tex_rgb[i+2] = c[0] / 255.0;  // B
            }
        }
    }
    return s;
}

DiffForward diff_forward(const DiffScene &scene, const int width, const int height, const vec3 &light) {
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("diff_forward: non-positive output size");
    if (scene.tex_width < 2 || scene.tex_height < 2)
        throw std::invalid_argument("diff_forward: texture must be at least 2x2");

    DiffForward rec;
    rec.width = width;
    rec.height = height;
    rec.rgb.assign(3*width*height, 0.0);
    rec.depth.assign(width*height, 0.0);
    rec.face_id.assign(width*height, -1);
    rec.record_pixel.assign(width*height, -1);

    rec.modelview  = ::ModelView;
    rec.perspective = ::Perspective;
    rec.viewport    = ::Viewport;
    rec.light = light;

    rec.tex_width = scene.tex_width;
    rec.tex_height = scene.tex_height;
    rec.tex_rgb = scene.tex_rgb;
    rec.face_pos = scene.face_pos;
    rec.corner_uv = scene.corner_uv;
    rec.n_positions = static_cast<int>(scene.positions.size());

    const int nf = static_cast<int>(scene.face_pos.size());
    rec.clip.resize(3*nf);
    rec.screen.resize(3*nf);
    rec.view.resize(3*nf);
    rec.normal.assign(nf, vec3{});

    std::vector<double> zbuf(width*height, -1000.0); // same initial value as init_zbuffer()

    const int W = scene.tex_width;
    const int H = scene.tex_height;

    for (int f=0; f<nf; ++f) {
        vec4 world[3], view[3], clip[3];
        for (int k=0; k<3; ++k) {
            const vec3 &P = scene.positions[scene.face_pos[f][k]];
            world[k] = vec4{P.x, P.y, P.z, 1.0};
            view[k]  = rec.modelview * world[k];
            clip[k]  = rec.perspective * view[k];
        }
        Triangle tri = { clip[0], clip[1], clip[2] };
        raster_core::Prepared prep = raster_core::prepare(tri, rec.viewport);

        for (int k=0; k<3; ++k) {
            rec.clip[3*f+k]   = clip[k];
            rec.screen[3*f+k] = prep.screen[k];
            rec.view[3*f+k]   = view[k].xyz();
        }
        if (!prep.valid) continue; // fixed culling branch

        const vec3 edge1 = (view[1]-view[0]).xyz();
        const vec3 edge2 = (view[2]-view[0]).xyz();
        const vec3 n = normalized(cross(edge1, edge2));
        rec.normal[f] = n;
        const double shade = 0.2 + 0.8*std::max(0.0, n*light);

        raster_core::Bounds bb = raster_core::pixel_bounds(prep, width, height);
        for (int x=bb.x0; x<=bb.x1; ++x) {
            for (int y=bb.y0; y<=bb.y1; ++y) {
                vec3 bc = raster_core::bary_screen(prep, x, y);
                if (!raster_core::inside(bc)) continue;
                const double z = raster_core::depth(prep, bc);
                const int pi = x + y*width;
                if (!raster_core::depth_passes(z, zbuf[pi])) continue; // fixed selection
                zbuf[pi] = z;

                const vec3 bcc = raster_core::bary_clip(prep, bc);
                const vec2 uv_raw = scene.corner_uv[3*f+0]*bcc.x
                                  + scene.corner_uv[3*f+1]*bcc.y
                                  + scene.corner_uv[3*f+2]*bcc.z;
                const double uc = std::min(1.0, std::max(0.0, uv_raw.x));
                const double vc = std::min(1.0, std::max(0.0, uv_raw.y));
                const Bilinear b = bilinear_setup(W, H, uc, vc);
                const vec3 T = b.w[0]*texel(rec, b.idx[0]) + b.w[1]*texel(rec, b.idx[1])
                             + b.w[2]*texel(rec, b.idx[2]) + b.w[3]*texel(rec, b.idx[3]);
                const vec3 color = T * shade;

                rec.depth[pi] = z;
                rec.face_id[pi] = f;
                rec.rgb[3*pi+0] = color.x;
                rec.rgb[3*pi+1] = color.y;
                rec.rgb[3*pi+2] = color.z;

                int ri = rec.record_pixel[pi];
                if (ri < 0) {
                    ri = static_cast<int>(rec.records.size());
                    rec.record_pixel[pi] = ri;
                    rec.record_face.push_back(f);
                    rec.records.emplace_back();
                } else {
                    rec.record_face[ri] = f; // a nearer face replaces the earlier contribution
                }
                DiffForward::PixelRec &r = rec.records[ri];
                r.bc_screen = bc;
                r.bc_clip = bcc;
                r.uv_raw = uv_raw;
                r.uv = vec2{uc, vc};
                r.dclamp[0] = (uv_raw.x > 0.0 && uv_raw.x < 1.0) ? 1.0 : 0.0;
                r.dclamp[1] = (uv_raw.y > 0.0 && uv_raw.y < 1.0) ? 1.0 : 0.0;
                r.ix = b.ix; r.iy = b.iy; r.fx = b.fx; r.fy = b.fy;
                r.shade = shade;
                for (int k=0; k<4; ++k) { r.w4[k] = b.w[k]; r.tex[k] = b.idx[k]; }
            }
        }
    }
    return rec;
}

DiffGrad diff_backward(const DiffForward &rec, const std::vector<double> &g_rgb) {
    if (static_cast<int>(g_rgb.size()) != 3*rec.width*rec.height)
        throw std::invalid_argument("diff_backward: adjoint size must be 3*W*H");

    DiffGrad grad;
    grad.positions.assign(rec.n_positions, vec3{});
    grad.corner_uv.assign(rec.corner_uv.size(), vec2{});
    grad.tex_rgb.assign(rec.tex_rgb.size(), 0.0);

    const int W = rec.width;
    const int H = rec.height;
    const int nf = static_cast<int>(rec.normal.size());
    std::vector<vec3> dLdN(nf, vec3{});

    const mat<4,4> &M = rec.modelview;
    const mat<4,4> &Pm = rec.perspective;
    const mat<4,4> &Vp = rec.viewport;

    for (int pi=0; pi<W*H; ++pi) {
        const int ri = rec.record_pixel[pi];
        if (ri < 0) continue; // background: no gradient
        const DiffForward::PixelRec &r = rec.records[ri];
        const int f = rec.record_face[ri];
        const vec3 g = { g_rgb[3*pi+0], g_rgb[3*pi+1], g_rgb[3*pi+2] };

        const vec3 T[4] = { texel(rec, r.tex[0]), texel(rec, r.tex[1]),
                            texel(rec, r.tex[2]), texel(rec, r.tex[3]) };
        const vec3 Ts = T[0]*r.w4[0] + T[1]*r.w4[1] + T[2]*r.w4[2] + T[3]*r.w4[3];
        const double dLdshade = g * Ts;

        // Texture: C = shade * sum_k w_k T_k
        for (int k=0; k<4; ++k) {
            const double s = r.shade * r.w4[k];
            for (int c=0; c<3; ++c)
                grad.tex_rgb[3*r.tex[k]+c] += s * g[c];
        }

        // Bilinear derivatives w.r.t. the clamped UV.
        const vec3 dSdX = (T[1]-T[0])*(1-r.fy) + (T[3]-T[2])*r.fy;
        const vec3 dSdY = (T[2]-T[0])*(1-r.fx) + (T[3]-T[1])*r.fx;
        const double dLdX = r.shade*(g*dSdX)*(rec.tex_width-1)*r.dclamp[0];
        const double dLdY = r.shade*(g*dSdY)*(rec.tex_height-1)*r.dclamp[1];
        const vec2 dLduv = { dLdX, dLdY };

        // Per-corner UVs.
        vec3 dLdbc_clip;
        for (int k=0; k<3; ++k) {
            const vec2 &t = rec.corner_uv[3*f+k];
            grad.corner_uv[3*f+k] = grad.corner_uv[3*f+k] + dLduv * r.bc_clip[k];
            dLdbc_clip[k] = dLduv.x*t.x + dLduv.y*t.y;
        }

        // Perspective-corrected weights: bc_clip_i = q_i / S, q_i = bc_screen_i / w_i.
        const double w0 = rec.clip[3*f+0].w;
        const double w1 = rec.clip[3*f+1].w;
        const double w2 = rec.clip[3*f+2].w;
        const vec3 q = { r.bc_screen.x/w0, r.bc_screen.y/w1, r.bc_screen.z/w2 };
        const double S = q.x + q.y + q.z;
        const double common = dLdbc_clip.x*q.x + dLdbc_clip.y*q.y + dLdbc_clip.z*q.z;
        double dLdq[3], dLdbc_screen[3], dLdw[3];
        const double wv[3] = {w0,w1,w2};
        const double bcv[3] = {r.bc_screen.x, r.bc_screen.y, r.bc_screen.z};
        for (int k=0; k<3; ++k) {
            dLdq[k] = dLdbc_clip[k]/S - common/(S*S);
            dLdbc_screen[k] = dLdq[k]/wv[k];
            dLdw[k] = -dLdq[k]*bcv[k]/(wv[k]*wv[k]);
        }

        // Screen-space barycentrics: bc = A^{-T} (x,y,1), A rows are screen points.
        mat<3,3> A;
        for (int k=0; k<3; ++k) {
            const vec2 &s = rec.screen[3*f+k];
            A[k] = vec3{s.x, s.y, 1.0};
        }
        const mat<3,3> Ainv = A.invert();
        const vec3 xvec = { dLdbc_screen[0], dLdbc_screen[1], dLdbc_screen[2] };
        const vec3 yvec = Ainv * xvec;

        for (int k=0; k<3; ++k) {
            // dL/dA[j][i] = -bc_j*y_i  =>  dL/dscreen_j = -bc_j*(y0,y1)
            const vec2 dLds = { -r.bc_screen[k]*yvec.x, -r.bc_screen[k]*yvec.y };

            const vec4 C = rec.clip[3*f+k];
            const double cw = C.w;

            // screen = Viewport * (c/w) is the only route through ndc.
            vec4 dLdndc;
            for (int m=0; m<4; ++m)
                dLdndc[m] = dLds.x*Vp[0][m] + dLds.y*Vp[1][m];

            // ndc_m = c_m / c_w, c_w = c4[3].
            vec4 dLdc;
            const double sum = dLdndc * C;
            for (int j=0; j<4; ++j) dLdc[j] = dLdndc[j]/cw;
            dLdc[3] -= sum/(cw*cw);
            dLdc[3] += dLdw[k]; // clip w also drives the perspective-correct UV weights

            // c = Perspective * view
            vec4 dLdview;
            for (int m=0; m<4; ++m) {
                double acc = 0;
                for (int j=0; j<4; ++j) acc += dLdc[j]*Pm[j][m];
                dLdview[m] = acc;
            }

            // view = ModelView * (position,1)
            vec3 &gp = grad.positions[rec.face_pos[f][k]];
            for (int c=0; c<3; ++c)
                gp[c] += dLdview.x*M[0][c] + dLdview.y*M[1][c]
                       + dLdview.z*M[2][c] + dLdview.w*M[3][c];
        }

        // Shading: shade = 0.2 + 0.8*max(0, N.L)
        const double m = rec.normal[f] * rec.light;
        const double dLdm = (m > 0.0) ? 0.8*dLdshade : 0.0;
        dLdN[f] = dLdN[f] + rec.light * dLdm;
    }

    // Face normals: N = normalize(cross(V1-V0, V2-V0)) in view space.
    for (int f=0; f<nf; ++f) {
        const vec3 &x = dLdN[f];
        if (x.x == 0.0 && x.y == 0.0 && x.z == 0.0) continue;
        const vec3 V0 = rec.view[3*f+0], V1 = rec.view[3*f+1], V2 = rec.view[3*f+2];
        const vec3 a = V1 - V0, b = V2 - V0;
        const vec3 e = cross(a, b);
        const double ne = norm(e);
        const vec3 N = rec.normal[f];
        const vec3 dLde = (x - N*(N*x)) / ne;
        const vec3 dLda = cross(b, dLde);
        const vec3 dLdb = cross(dLde, a);
        const vec3 dV[3] = { vec3{0,0,0} - (dLda + dLdb), dLda, dLdb };
        for (int k=0; k<3; ++k) {
            vec3 &gp = grad.positions[rec.face_pos[f][k]];
            for (int c=0; c<3; ++c)
                gp[c] += dV[k].x*M[0][c] + dV[k].y*M[1][c] + dV[k].z*M[2][c];
        }
    }

    return grad;
}
