#include "differentiable.h"
#include "our_gl.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

extern mat<4,4> ModelView, Viewport, Perspective; // camera state owned by our_gl.cpp

// ---------------------------------------------------------------------------
// Scene construction
// ---------------------------------------------------------------------------

std::unique_ptr<DiffScene> DiffScene::from_model(const Model &model, const int width, const int height) {
    auto scene = std::make_unique<DiffScene>();
    scene->out_w = width;
    scene->out_h = height;

    scene->positions.resize(model.nverts());
    for (int i = 0; i < model.nverts(); ++i) scene->positions[i] = model.vert(i);

    const int nfaces = model.nfaces();
    scene->face_pos.resize(nfaces * 3);
    scene->corner_uv.resize(nfaces * 3);
    for (int f = 0; f < nfaces; ++f) {
        for (int k = 0; k < 3; ++k) {
            scene->face_pos[f*3+k] = model.face_vert_index(f, k); // positions stay shared by index
            scene->corner_uv[f*3+k] = model.uv(f, k);            // one UV entry per face corner
        }
    }

    const TGAImage &diffuse = model.diffuse();
    scene->tex_w = diffuse.width();
    scene->tex_h = diffuse.height();
    if (scene->tex_w < 2 || scene->tex_h < 2)
        throw std::runtime_error("differentiable renderer requires a diffuse texture of at least 2x2");
    scene->texture.assign(scene->tex_w * scene->tex_h * 3, 0.0);
    for (int y = 0; y < scene->tex_h; ++y) {
        for (int x = 0; x < scene->tex_w; ++x) {
            const TGAColor c = diffuse.get(x, y); // TGA storage is BGRA; keep linear RGB order
            scene->texture[((y*scene->tex_w + x)*3 + 0)] = c[2] / 255.0; // R
            scene->texture[((y*scene->tex_w + x)*3 + 1)] = c[1] / 255.0; // G
            scene->texture[((y*scene->tex_w + x)*3 + 2)] = c[0] / 255.0; // B
        }
    }

    scene->mv    = ModelView;    // snapshot of the global camera state
    scene->persp = Perspective;
    scene->vp    = Viewport;
    return scene;
}

void DiffScene::resize_texture(const int w, const int h) {
    if (w < 2 || h < 2) throw std::runtime_error("texture dimensions must be at least 2x2");
    tex_w = w;
    tex_h = h;
    texture.assign(w * h * 3, 0.0);
}

// ---------------------------------------------------------------------------
// Small linear algebra helpers (double arrays, fixed sizes)
// ---------------------------------------------------------------------------

namespace {

struct V3 { double x, y, z; };

V3 sub3(const double a[3], const double b[3]) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
V3 cross3(const V3 &a, const V3 &b) {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
double dot3(const V3 &a, const V3 &b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
double len3(const V3 &a) { return std::sqrt(dot3(a, a)); }

void mat_vec4(const mat<4,4> &m, const vec4 &v, double out[4]) {
    for (int i = 0; i < 4; ++i) {
        double s = 0;
        for (int j = 0; j < 4; ++j) s += m[i][j] * v[j];
        out[i] = s;
    }
}

void mat_vec4(const mat<4,4> &m, const vec4 &v, vec4 &out) {
    double tmp[4];
    mat_vec4(m, v, tmp);
    for (int i = 0; i < 4; ++i) out[i] = tmp[i];
}

// d(out) for a matrix-vector product, given the output adjoint.
void adj_mat_vec4(const mat<4,4> &m, const double gout[4], double gv[4]) {
    for (int j = 0; j < 4; ++j) {
        double s = 0;
        for (int i = 0; i < 4; ++i) s += m[i][j] * gout[i];
        gv[j] += s;
    }
}

// ---------------------------------------------------------------------------
// Clamped bilinear texture sampling; returns the four texel base indices and
// blend weights. Clamping follows the forward contract: u,v clamped to [0,1],
// mapped onto [0, W-1] x [0, H-1], floor selects the right/lower cell on grid
// lines, the right/bottom edge reuses the last legal cell.
// ---------------------------------------------------------------------------

struct TextureSample {
    int cell[4];      // flat texel index (x + y*W) for BL, BR, TL, TR
    double wgt[4];    // corresponding bilinear weights
    int i0, j0;
    double fx, fy;
};

TextureSample sample_locations(double u, double v, const int W, const int H) {
    TextureSample s;
    const double uc = std::min(1.0, std::max(0.0, u));
    const double vc = std::min(1.0, std::max(0.0, v));
    double gx = uc * (W - 1.0);
    double gy = vc * (H - 1.0);
    int i0 = static_cast<int>(std::floor(gx));
    int j0 = static_cast<int>(std::floor(gy));
    i0 = std::min(i0, W - 2);
    j0 = std::min(j0, H - 2);
    double fx = gx - i0;
    double fy = gy - j0;
    s.i0 = i0; s.j0 = j0; s.fx = fx; s.fy = fy;
    const int bl = i0   + j0*W;
    const int br = i0+1 + j0*W;
    const int tl = i0   + (j0+1)*W;
    const int tr = i0+1 + (j0+1)*W;
    s.cell[0] = bl; s.cell[1] = br; s.cell[2] = tl; s.cell[3] = tr;
    s.wgt[0] = (1-fx)*(1-fy);
    s.wgt[1] =     fx *(1-fy);
    s.wgt[2] = (1-fx)*   fy;
    s.wgt[3] =     fx *   fy;
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Forward pass
// ---------------------------------------------------------------------------

std::unique_ptr<DiffRecord> diff_render(const DiffScene &scene, const vec3 &light_dir, DiffImage &image) {
    auto record = std::make_unique<DiffRecord>();
    record->width  = scene.out_w;
    record->height = scene.out_h;
    record->tex_w  = scene.tex_w;
    record->tex_h  = scene.tex_h;
    record->n_positions = scene.num_positions();
    record->mv    = scene.mv;
    record->persp = scene.persp;
    record->vp    = scene.vp;
    record->light[0] = light_dir.x;
    record->light[1] = light_dir.y;
    record->light[2] = light_dir.z;

    const int W = scene.out_w, H = scene.out_h;
    image.width = W;
    image.height = H;
    image.rgb.assign(W * H * 3, 0.0);
    image.depth.assign(W * H, 0.0);
    image.face.assign(W * H, -1);

    std::vector<double> depth_buffer(W * H, gl_depth_clear());
    std::vector<int> pixel_slot(W * H, -1); // pixel -> index in record->pixels
    const int nfaces = scene.num_faces();
    record->faces.resize(nfaces);

    // Per-face forward values, reused for every pixel the face wins.
    struct FaceGeom {
        vec4 clip[3];
        vec4 view[3];
        GlRasterUnit unit;
        bool culled;
        V3 N;
        double ndotl;
        bool lit;
        double shade;
    };
    std::vector<FaceGeom> geom(nfaces);

    for (int f = 0; f < nfaces; ++f) {
        FaceGeom &g = geom[f];
        DiffFaceRecord &fr = record->faces[f];
        for (int k = 0; k < 3; ++k)
            fr.pos_idx[k] = scene.face_position_index(f, k);

        for (int k = 0; k < 3; ++k) {
            const vec4 pos = scene.position(fr.pos_idx[k]);
            mat_vec4(scene.mv, pos, g.view[k]);
            mat_vec4(scene.persp, g.view[k], g.clip[k]);
        }
        Triangle tri = { g.clip[0], g.clip[1], g.clip[2] };
        g.unit = gl_make_raster_unit(tri);
        g.culled = gl_unit_culled(g.unit);
        if (g.culled) { fr.normal_len = 0; continue; }

        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                fr.b_col0[c] = g.unit.abc_inv_t[c][0];
                fr.b_col1[c] = g.unit.abc_inv_t[c][1];
            }
        fr.area = g.unit.area;
        fr.vscale_x = scene.vp[0][0];
        fr.vscale_y = scene.vp[1][1];

        const V3 p0 = {g.view[0].x, g.view[0].y, g.view[0].z};
        const V3 p1 = {g.view[1].x, g.view[1].y, g.view[1].z};
        const V3 p2 = {g.view[2].x, g.view[2].y, g.view[2].z};
        const V3 e1 = sub3(&p1.x, &p0.x);
        const V3 e2 = sub3(&p2.x, &p0.x);
        fr.e1[0] = e1.x; fr.e1[1] = e1.y; fr.e1[2] = e1.z;
        fr.e2[0] = e2.x; fr.e2[1] = e2.y; fr.e2[2] = e2.z;
        g.N = cross3(e1, e2);
        const double nlen = len3(g.N);
        fr.normal_len = nlen;
        g.N = {g.N.x / nlen, g.N.y / nlen, g.N.z / nlen};
        fr.N[0] = g.N.x; fr.N[1] = g.N.y; fr.N[2] = g.N.z;
        const V3 L = {record->light[0], record->light[1], record->light[2]};
        g.ndotl = dot3(g.N, L);
        g.lit = g.ndotl > 0.0;
        g.shade = 0.2 + 0.8 * std::max(0.0, g.ndotl);
    }

    // Faces are drawn in Model order; the depth test is strict so equal depth
    // keeps the face drawn first, exactly like rasterize().
    for (int f = 0; f < nfaces; ++f) {
        FaceGeom &g = geom[f];
        if (g.culled) continue;
        const int x0 = std::max<int>(g.unit.bbminx, 0);
        const int x1 = std::min<int>(g.unit.bbmaxx, W - 1);
        const int y0 = std::max<int>(g.unit.bbminy, 0);
        const int y1 = std::min<int>(g.unit.bbmaxy, H - 1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                vec3 bc_screen, bc_clip;
                double z;
                if (!gl_evaluate_pixel(g.unit, x, y, bc_screen, bc_clip, z)) continue;
                const int pi = x + y * W;
                if (!gl_depth_passes(z, depth_buffer[pi])) continue;
                depth_buffer[pi] = z;
                image.depth[pi] = z;
                image.face[pi] = f;

                DiffPixelRecord pr;
                pr.x = x; pr.y = y; pr.face = f;                for (int k = 0; k < 3; ++k) {
                    pr.lam[k]  = bc_screen[k];
                    pr.beta[k] = bc_clip[k];
                    pr.w[k]    = g.unit.clip[k].w;
                    pr.qx[k]   = g.unit.ndc[k].x;
                    pr.qy[k]   = g.unit.ndc[k].y;
                    const vec2 uv = scene.uv(f, k);
                    pr.corner_u[k] = uv.x;
                    pr.corner_v[k] = uv.y;
                }
                pr.u = pr.beta[0]*pr.corner_u[0] + pr.beta[1]*pr.corner_u[1] + pr.beta[2]*pr.corner_u[2];
                pr.v = pr.beta[0]*pr.corner_v[0] + pr.beta[1]*pr.corner_v[1] + pr.beta[2]*pr.corner_v[2];

                const TextureSample ts = sample_locations(pr.u, pr.v, scene.tex_w, scene.tex_h);
                pr.i0 = ts.i0; pr.j0 = ts.j0; pr.fx = ts.fx; pr.fy = ts.fy;
                double color[3] = {0,0,0};
                for (int t = 0; t < 4; ++t) {
                    pr.tex_cell[t] = ts.cell[t];
                    for (int c = 0; c < 3; ++c) {
                        const double val = scene.texture[ts.cell[t]*3 + c];
                        pr.tex[t][c] = val;
                        color[c] += ts.wgt[t] * val;
                    }
                }
                pr.shade = g.shade;
                pr.lit   = g.lit;
                for (int c = 0; c < 3; ++c)
                    image.rgb[pi*3 + c] = color[c] * g.shade; // linear double output, no clamping
                // Each pixel keeps only its final winner: an earlier fragment
                // that lost the depth test is overwritten in place and leaves
                // no adjoint contribution.
                if (pixel_slot[pi] < 0) {
                    pixel_slot[pi] = static_cast<int>(record->pixels.size());
                    record->pixels.push_back(pr);
                } else {
                    record->pixels[pixel_slot[pi]] = pr;
                }
            }
        }
    }
    return record;
}

// ---------------------------------------------------------------------------
// Hand-written adjoint
// ---------------------------------------------------------------------------

DiffGradients diff_backward(const DiffRecord &record, const std::vector<double> &g) {
    DiffGradients grad;
    grad.tex_w = record.tex_w;
    grad.tex_h = record.tex_h;
    grad.positions.assign(record.n_positions, vec4{});
    const int n_corners = static_cast<int>(record.faces.size()) * 3;
    grad.uvs.assign(n_corners, vec2{});
    grad.texture.assign(record.tex_w * record.tex_h * 3, 0.0);

    const V3 L = {record.light[0], record.light[1], record.light[2]};

    for (const DiffPixelRecord &pr : record.pixels) {
        const int pi = pr.x + pr.y * record.width;
        double gC[3] = { g[pi*3+0], g[pi*3+1], g[pi*3+2] }; // adjoint of the pixel color

        // C_c = shade * T_c  =>  gT_c = shade * gC_c, g_shade = sum gC*T
        double gT[3] = {0,0,0};
        double g_shade = 0;
        for (int c = 0; c < 3; ++c) {
            gT[c] = pr.shade * gC[c];
            g_shade += gC[c] *
                (pr.tex[0][c]*(1-pr.fx)*(1-pr.fy) + pr.tex[1][c]*pr.fx*(1-pr.fy) +
                 pr.tex[2][c]*(1-pr.fx)*pr.fy     + pr.tex[3][c]*pr.fx*pr.fy);
        }

        // Texture adjoint: weights fixed by the frozen sampling branch.
        const double wb[4] = {(1-pr.fx)*(1-pr.fy), pr.fx*(1-pr.fy),
                              (1-pr.fx)*pr.fy,     pr.fx*pr.fy};
        for (int t = 0; t < 4; ++t)
            for (int c = 0; c < 3; ++c)
                grad.texture[pr.tex_cell[t]*3 + c] += wb[t] * gT[c];

        // gfx, gfy from the bilinear blend (only propagated when the clamp
        // leaves the coordinate inside [0,1]; endpoints give zero).
        double gfx = 0, gfy = 0;
        for (int c = 0; c < 3; ++c) {
            const double t00 = pr.tex[0][c], t10 = pr.tex[1][c], t01 = pr.tex[2][c], t11 = pr.tex[3][c];
            gfx += gT[c] * ((1-pr.fy)*(t10-t00) + pr.fy*(t11-t01));
            gfy += gT[c] * ((1-pr.fx)*(t01-t00) + pr.fx*(t11-t10));
        }
        double gu = 0, gv = 0;
        const bool u_inside = (pr.u > 0.0 && pr.u < 1.0);
        const bool v_inside = (pr.v > 0.0 && pr.v < 1.0);
        if (u_inside) gu = gfx * (record.tex_w - 1);
        if (v_inside) gv = gfy * (record.tex_h - 1);

        // Shading adjoint through the unit face normal.
        const DiffFaceRecord &fr = record.faces[pr.face];
        double g_ndotl = 0;
        if (pr.lit) g_ndotl = 0.8 * g_shade;
        V3 gN = {g_ndotl*L.x, g_ndotl*L.y, g_ndotl*L.z};

        const V3 N  = {fr.N[0], fr.N[1], fr.N[2]};
        const double nlen = fr.normal_len;
        const V3 e1 = {fr.e1[0], fr.e1[1], fr.e1[2]};
        const V3 e2 = {fr.e2[0], fr.e2[1], fr.e2[2]};
        // N = c/nlen, c = e1 x e2
        V3 gc = { (gN.x - N.x*dot3(gN, N)) / nlen,
                  (gN.y - N.y*dot3(gN, N)) / nlen,
                  (gN.z - N.z*dot3(gN, N)) / nlen };
        // d(e1 x e2): g_e1 = e2 x gc, g_e2 = gc x e1
        V3 g_e1 = cross3(e2, gc);
        V3 g_e2 = cross3(gc, e1);
        const double ge1[3] = {g_e1.x, g_e1.y, g_e1.z};
        const double ge2[3] = {g_e2.x, g_e2.y, g_e2.z};
        // Accumulate edge adjoints onto the three view vertices.
        double gview[3][3] = {{0,0,0},{0,0,0},{0,0,0}};
        for (int a = 0; a < 3; ++a) {
            gview[0][a] += -ge1[a] - ge2[a];
            gview[1][a] +=  ge1[a];
            gview[2][a] +=  ge2[a];
        }

        // Perspective UV interpolation: u = sum beta_k * U_k with
        // beta_k = (lam_k/w_k) / s, s = sum lam_j/w_j.
        double gbeta[3] = {0,0,0};
        for (int k = 0; k < 3; ++k) gbeta[k] = gu * pr.corner_u[k] + gv * pr.corner_v[k];
        double g_uv_corner[3][2] = {{0,0},{0,0},{0,0}};
        for (int k = 0; k < 3; ++k) {
            g_uv_corner[k][0] = pr.beta[k] * gu;
            g_uv_corner[k][1] = pr.beta[k] * gv;
        }
        double s = 0;
        for (int k = 0; k < 3; ++k) s += pr.lam[k] / pr.w[k];
        double gh[3] = {0,0,0}; // adjoint of h_k = lam_k / w_k
        for (int k = 0; k < 3; ++k) {
            double acc = 0;
            for (int j = 0; j < 3; ++j)
                acc += gbeta[j] * ((j == k ? 1.0 : 0.0) - pr.beta[j]) / s;
            gh[k] = acc;
        }
        double glam[3] = {0,0,0};
        double gw[3]   = {0,0,0};
        for (int k = 0; k < 3; ++k) {
            glam[k] += gh[k] / pr.w[k];
            gw[k]   += -gh[k] * pr.lam[k] / (pr.w[k] * pr.w[k]);
        }

        // Screen barycentrics satisfy A^T lam = p, with A rows (sx_k, sy_k, 1)
        // and the integer pixel p fixed, hence dp = 0. Differentiating:
        //   dlam = -B (dA)^T lam,  B = A^{-T}.
        // For vertex k, ((dA)^T lam) = (lam_k dsx_k, lam_k dsy_k, 0), so
        //   g_sx_k = -lam_k sum_i glam[i] B[i][0], likewise for sy.
        double g_sx[3] = {0,0,0}, g_sy[3] = {0,0,0};
        double sum_b0 = 0, sum_b1 = 0;
        for (int i = 0; i < 3; ++i) {
            sum_b0 += glam[i] * fr.b_col0[i];
            sum_b1 += glam[i] * fr.b_col1[i];
        }
        for (int k = 0; k < 3; ++k) {
            g_sx[k] = -pr.lam[k] * sum_b0;
            g_sy[k] = -pr.lam[k] * sum_b1;
        }
        double gqx[3] = {0,0,0}, gqy[3] = {0,0,0};
        for (int k = 0; k < 3; ++k) {
            gqx[k] = fr.vscale_x * g_sx[k];
            gqy[k] = fr.vscale_y * g_sy[k];
        }

        // q = clip / w, clip = persp * view. The w adjoint has two sources:
        // the division itself (-sum gq*q/w) and the UV chain's 1/w term.
        for (int k = 0; k < 3; ++k) {
            const double gw_div = -(gqx[k]*pr.qx[k] + gqy[k]*pr.qy[k]) / pr.w[k];
            const double gw_total = gw_div + gw[k];
            double gclip[4] = {gqx[k]/pr.w[k], gqy[k]/pr.w[k], 0.0, gw_total};
            double gv4[4] = {0,0,0,0};
            adj_mat_vec4(record.persp, gclip, gv4);
            for (int a = 0; a < 3; ++a) gview[k][a] += gv4[a];
        }

        // Back-propagate the full view-space adjoint (shading + projection)
        // through the rigid ModelView to shared positions.
        for (int k = 0; k < 3; ++k) {
            const double gv4[4] = {gview[k][0], gview[k][1], gview[k][2], 0.0};
            double gpos4[4] = {0,0,0,0};
            adj_mat_vec4(record.mv, gv4, gpos4);
            vec4 &gp = grad.positions[fr.pos_idx[k]];
            for (int a = 0; a < 4; ++a) gp[a] += gpos4[a]; // shared positions: contributions add
        }

        // Per-corner UV adjoints land on face corners (never merged).
        for (int k = 0; k < 3; ++k) {
            vec2 &guv = grad.uvs[pr.face*3 + k];
            guv.x += g_uv_corner[k][0];
            guv.y += g_uv_corner[k][1];
        }
    }
    return grad;
}
