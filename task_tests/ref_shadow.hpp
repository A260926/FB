// ref_shadow.hpp -- independent numerical reference for canvas_ity shadows.
//
// This is a from-scratch model of canvas_ity's shadow rendering pipeline,
// written only against the documented public behavior of the library.  It
// deliberately shares no code with the library internals: it does not call
// paint_pixel, lines_to_runs, or render_shadow, and it does not reuse the
// sliding-window box filter recursion.  Instead it evaluates, in double
// precision:
//
//   1. exact convex-quad pixel coverage (Sutherland-Hodgman clipping of the
//      device-space quad against each pixel square, then a shoelace area),
//   2. direct O(window^2) convolution with the three-pass extended box
//      kernel that the library approximates a Gaussian with (the kernel is
//      built by literally convolving the 1D kernel with itself three times),
//   3. Porter-Duff compositing in linear premultiplied space, and
//   4. conversion to unpremultiplied sRGB8 with the same ordered dither
//      that get_image_data applies.
//
// Stages 1-3 work on linear premultiplied colors; stage 4 is the only place
// where sRGB quantization happens, so the two layers never get conflated.
//
// The model covers what the tests exercise: fill_rectangle-style convex
// quads under affine transforms, solid/linear-gradient/pattern paints,
// rectangular (possibly nested) clips, shadow offset+blur in device pixels,
// global alpha, and the Porter-Duff operations.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace refshadow {

// Pixels with coverage or visibility below this are treated as empty,
// matching the library's sparse-run threshold.
constexpr double k_threshold = 1.0 / 8160.0;

inline double linearized(double v) {
    return v < 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
inline double delinearized(double v) {
    return v < 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}
inline double clamp01(double v) { return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; }

struct RGBA { double r = 0.0, g = 0.0, b = 0.0, a = 0.0; };
inline RGBA premultiplied(const RGBA &c) {
    return {c.r * c.a, c.g * c.a, c.b * c.a, c.a};
}
inline RGBA linearized(const RGBA &c) {
    return {linearized(c.r), linearized(c.g), linearized(c.b), c.a};
}

struct Pt { double x, y; };
struct Mat { double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; };
inline Pt operator*(const Mat &m, Pt p) {
    return {m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f};
}
inline Mat inverse(const Mat &m) {
    double det = m.a * m.d - m.b * m.c;
    double s = det != 0.0 ? 1.0 / det : 0.0;
    return {s * m.d,         s * -m.b,
            s * -m.c,        s * m.a,
            s * (m.c * m.f - m.d * m.e), s * (m.b * m.e - m.a * m.f)};
}

// ======== paint model ========
//
// Colors are given as unpremultiplied sRGB (as through the public API) and
// evaluated to premultiplied linear colors at a device-space point.

struct Paint {
    enum Type { Color, Linear, Pattern } type = Color;
    RGBA color{0, 0, 0, 1};                        // solid, sRGB unpremultiplied
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;         // gradient line, user space
    std::vector<std::array<double, 5>> stops;      // offset, sRGB r, g, b, a
    std::vector<RGBA> texels;                      // pattern, premultiplied linear
    int tw = 0, th = 0, repetition = 0;            // repeat=0,x=1,y=2,none=3
};

inline RGBA sample_linear_gradient(const Paint &p, const Mat &inv,
                                   double dx, double dy) {
    if (p.stops.empty())
        return {};
    Pt u = inv * Pt{dx, dy};
    double lx = p.x1 - p.x0, ly = p.y1 - p.y0;
    double span = lx * lx + ly * ly;
    if (span == 0.0)
        return {};
    double offset = ((u.x - p.x0) * lx + (u.y - p.y0) * ly) / span;
    size_t i = 0;
    while (i < p.stops.size() && p.stops[i][0] <= offset)
        ++i;  // first stop strictly past offset (upper_bound)
    if (i == 0)
        return premultiplied(linearized(
            RGBA{p.stops[0][1], p.stops[0][2], p.stops[0][3], p.stops[0][4]}));
    if (i == p.stops.size())
        return premultiplied(linearized(
            RGBA{p.stops[i-1][1], p.stops[i-1][2], p.stops[i-1][3],
                 p.stops[i-1][4]}));
    double mix = (offset - p.stops[i-1][0]) / (p.stops[i][0] - p.stops[i-1][0]);
    RGBA c0{linearized(p.stops[i-1][1]), linearized(p.stops[i-1][2]),
            linearized(p.stops[i-1][3]), p.stops[i-1][4]};
    RGBA c1{linearized(p.stops[i][1]), linearized(p.stops[i][2]),
            linearized(p.stops[i][3]), p.stops[i][4]};
    RGBA c{c0.r + mix * (c1.r - c0.r), c0.g + mix * (c1.g - c0.g),
           c0.b + mix * (c1.b - c0.b), c0.a + mix * (c1.a - c0.a)};
    return premultiplied(c);
}

inline double keys_cubic(double t) {
    return t < 1.0 ? (1.5 * t - 2.5) * t * t + 1.0
                   : ((-0.5 * t + 2.5) * t - 4.0) * t + 2.0;
}

inline RGBA sample_pattern(const Paint &p, const Mat &inv,
                           double dx, double dy) {
    Pt u = inv * Pt{dx, dy};
    if ((p.repetition & 2) && (u.x < 0.0 || u.x >= p.tw))
        return {};
    if ((p.repetition & 1) && (u.y < 0.0 || u.y >= p.th))
        return {};
    double sx = std::max(1.0, std::min(std::fabs(inv.a) + std::fabs(inv.c),
                                       p.tw * 0.25));
    double sy = std::max(1.0, std::min(std::fabs(inv.b) + std::fabs(inv.d),
                                       p.th * 0.25));
    double px = u.x - 0.5, py = u.y - 0.5;
    int left = (int)std::ceil(px - 2.0 * sx), right = (int)std::ceil(px + 2.0 * sx);
    int top = (int)std::ceil(py - 2.0 * sy), bottom = (int)std::ceil(py + 2.0 * sy);
    RGBA sum{};
    double wsum = 0.0;
    for (int gy = top; gy < bottom; ++gy) {
        double wy = keys_cubic(std::fabs((gy - py) / sy));
        int yy = gy % p.th;
        if (yy < 0) yy += p.th;
        for (int gx = left; gx < right; ++gx) {
            double wx = keys_cubic(std::fabs((gx - px) / sx));
            int xx = gx % p.tw;
            if (xx < 0) xx += p.tw;
            double w = wx * wy;
            const RGBA &t = p.texels[(size_t)yy * p.tw + xx];
            sum.r += w * t.r;
            sum.g += w * t.g;
            sum.b += w * t.b;
            sum.a += w * t.a;
            wsum += w;
        }
    }
    double s = 1.0 / wsum;
    return {sum.r * s, sum.g * s, sum.b * s, sum.a * s};
}

inline RGBA paint_at(const Paint &p, const Mat &inv, double dx, double dy) {
    if (p.type == Paint::Color)
        return premultiplied(linearized(p.color));
    if (p.type == Paint::Linear)
        return sample_linear_gradient(p, inv, dx, dy);
    return sample_pattern(p, inv, dx, dy);
}

// ======== convex polygon helpers ========

inline std::vector<Pt> clip_halfplane(const std::vector<Pt> &in,
                                      double nx, double ny, double c) {
    std::vector<Pt> out;
    size_t n = in.size();
    for (size_t i = 0; i < n; ++i) {
        Pt from = in[(i + n - 1) % n], to = in[i];
        double fs = nx * from.x + ny * from.y + c;
        double ts = nx * to.x + ny * to.y + c;
        if (fs * ts < 0.0) {
            double t = fs / (fs - ts);
            out.push_back({from.x + t * (to.x - from.x),
                           from.y + t * (to.y - from.y)});
        }
        if (ts >= 0.0)
            out.push_back(to);
    }
    return out;
}

inline std::vector<Pt> clip_to_rect(const std::vector<Pt> &poly,
                                    double x0, double y0, double x1, double y1) {
    std::vector<Pt> q = clip_halfplane(poly, 1, 0, -x0);
    q = clip_halfplane(q, 0, 1, -y0);
    q = clip_halfplane(q, -1, 0, x1);
    q = clip_halfplane(q, 0, -1, y1);
    return q;
}

inline double polygon_area(const std::vector<Pt> &p) {
    double s = 0.0;
    for (size_t i = 0, n = p.size(); i < n; ++i) {
        Pt a = p[i], b = p[(i + 1) % n];
        s += a.x * b.y - b.x * a.y;
    }
    return std::fabs(s) * 0.5;
}

// Exact area of the intersection of a convex polygon with one pixel square.
inline double coverage(const std::vector<Pt> &poly, int px, int py) {
    if (poly.empty())
        return 0.0;
    double x0 = poly[0].x, x1 = poly[0].x, y0 = poly[0].y, y1 = poly[0].y;
    for (const Pt &p : poly) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    if (x1 <= px || x0 >= px + 1.0 || y1 <= py || y0 >= py + 1.0)
        return 0.0;
    return clamp01(polygon_area(clip_to_rect(poly, px, py, px + 1.0, py + 1.0)));
}

// ======== extended box filter kernel (three passes, direct convolution) ========

struct Kernel {
    int radius = 0;
    int border = 3;              // support of the 3-pass kernel: 3*(radius+1)
    double tail = 0.0;           // weight at |k| = radius+1
    double interior = 1.0;       // weight at |k| <= radius
    std::vector<double> k3;      // 2*border+1 entries, k3[border] is the center
};

inline Kernel make_kernel(double blur) {
    double s = 0.25 * blur * blur;
    int r = (int)std::floor(0.5 * (std::sqrt(4.0 * s + 1.0) - 1.0));
    double a = (2.0 * r + 1.0) * (r * (r + 1.0) - s) /
               (2.0 * s - 6.0 * (r + 1.0) * (r + 1.0));
    double divisor = 2.0 * (a + r) + 1.0;
    Kernel k;
    k.radius = r;
    k.tail = a / divisor;
    k.interior = 1.0 / divisor;
    k.border = 3 * (r + 1);
    int sup = r + 1;
    std::vector<double> one(2 * sup + 1, 0.0);
    for (int d = -r; d <= r; ++d)
        one[sup + d] = k.interior;
    one[0] = one[2 * sup] = k.tail;
    std::vector<double> two(4 * sup + 1, 0.0), three(6 * sup + 1, 0.0);
    for (size_t i = 0; i < one.size(); ++i)
        for (size_t j = 0; j < one.size(); ++j)
            two[i + j] += one[i] * one[j];
    for (size_t i = 0; i < two.size(); ++i)
        for (size_t j = 0; j < one.size(); ++j)
            three[i + j] += two[i] * one[j];
    k.k3 = three;
    return k;
}

// ======== reference canvas ========

class Canvas {
  public:
    int W, H;
    std::vector<double> R, G, B, A;      // linear premultiplied
    Mat fwd, inv;
    Paint paint;
    double sr = 0, sg = 0, sb = 0, sa = 0;  // shadow color, sRGB unpremultiplied
    double sox = 0, soy = 0, sblur = 0;
    int op = 14;                            // source_over
    double galpha = 1.0;
    std::vector<std::vector<Pt>> clips;     // device-space convex quads

    struct State {
        Mat fwd, inv;
        Paint paint;
        double sr, sg, sb, sa, sox, soy, sblur;
        int op;
        double galpha;
        std::vector<std::vector<Pt>> clips;
    };
    std::vector<State> stack;

    Canvas(int w, int h) : W(w), H(h),
        R((size_t)w * h, 0.0), G((size_t)w * h, 0.0),
        B((size_t)w * h, 0.0), A((size_t)w * h, 0.0) {}

    void set_transform(const Mat &m) { fwd = m; inv = inverse(m); }
    void save() {
        stack.push_back(State{fwd, inv, paint, sr, sg, sb, sa,
                              sox, soy, sblur, op, galpha, clips});
    }
    void restore() {
        if (stack.empty())
            return;
        const State &s = stack.back();
        fwd = s.fwd; inv = s.inv; paint = s.paint;
        sr = s.sr; sg = s.sg; sb = s.sb; sa = s.sa;
        sox = s.sox; soy = s.soy; sblur = s.sblur;
        op = s.op; galpha = s.galpha; clips = s.clips;
        stack.pop_back();
    }

    void clip_rect(double x, double y, double w, double h) {
        clips.push_back({fwd * Pt{x, y}, fwd * Pt{x + w, y},
                         fwd * Pt{x + w, y + h}, fwd * Pt{x, y + h}});
    }

    double visibility(int px, int py) const {
        double v = 1.0;
        for (const auto &q : clips)
            v *= coverage(q, px, py);
        return v;
    }

    void blend_into(int px, int py, const RGBA &fore, double vis) {
        size_t i = (size_t)py * W + px;
        double mix_f = (op & 1) ? A[i] : 0.0;
        if (op & 2)
            mix_f = 1.0 - mix_f;
        double mix_b = (op & 4) ? fore.a : 0.0;
        if (op & 8)
            mix_b = 1.0 - mix_b;
        double ba = std::min(mix_f * fore.a + mix_b * A[i], 1.0);
        double br = mix_f * fore.r + mix_b * R[i];
        double bg = mix_f * fore.g + mix_b * G[i];
        double bb = mix_f * fore.b + mix_b * B[i];
        R[i] = vis * br + (1.0 - vis) * R[i];
        G[i] = vis * bg + (1.0 - vis) * G[i];
        B[i] = vis * bb + (1.0 - vis) * B[i];
        A[i] = vis * ba + (1.0 - vis) * A[i];
    }

    void fill_rect(double x, double y, double w, double h) {
        std::vector<Pt> quad{fwd * Pt{x, y}, fwd * Pt{x + w, y},
                             fwd * Pt{x + w, y + h}, fwd * Pt{x, y + h}};
        if (sa > 0.0 && (sblur > 0.0 || sox != 0.0 || soy != 0.0))
            shadow_pass(quad);
        main_pass(quad);
    }

    // Blur the shifted, padded-clip-bounded source alpha with the direct
    // 2D convolution, then composite it through the clip mask.
    void shadow_pass(const std::vector<Pt> &quad) {
        Kernel k = make_kernel(sblur);
        int b = k.border;
        std::vector<Pt> shifted;
        for (const Pt &q : quad)
            shifted.push_back({q.x + sox, q.y + soy});
        // The library scan-converts the shifted shape clipped to the canvas
        // padded by 2*border on every side, i.e. device coords from -border
        // to size+border.  Anything farther out cannot reach the canvas.
        std::vector<Pt> src = clip_to_rect(shifted, -b, -b, W + b, H + b);
        if (src.empty())
            return;
        double x0 = src[0].x, x1 = src[0].x, y0 = src[0].y, y1 = src[0].y;
        for (const Pt &p : src) {
            x0 = std::min(x0, p.x);
            x1 = std::max(x1, p.x);
            y0 = std::min(y0, p.y);
            y1 = std::max(y1, p.y);
        }
        int tx0 = (int)std::floor(x0), tx1 = (int)std::ceil(x1) - 1;
        int ty0 = (int)std::floor(y0), ty1 = (int)std::ceil(y1) - 1;
        int sw = tx1 - tx0 + 1, sh = ty1 - ty0 + 1;
        std::vector<double> S((size_t)sw * sh, 0.0);
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) {
                double cov = coverage(src, tx, ty);
                if (cov < k_threshold)
                    continue;
                // Source alpha is sampled in the pre-offset device position.
                RGBA p = paint_at(paint, inv, tx - sox + 0.5, ty - soy + 0.5);
                S[(size_t)(ty - ty0) * sw + (tx - tx0)] = cov * p.a;
            }
        // The library only composites the shadow where its working buffer
        // reaches: run extents padded by border, with the bottom row of the
        // fringe (at distance exactly border below the last source row)
        // dropped because the row bound gets no +1 unlike the column bound.
        int min_run_x = (int)std::floor(x0) + b, max_run_x = (int)std::ceil(x1) + b;
        int min_run_y = (int)std::floor(y0) + b, max_run_y = (int)std::ceil(y1) + b - 1;
        int left = std::max(min_run_x - b, 0);
        int right = std::min(max_run_x + b, W + 2 * b) + 1;
        int top = std::max(min_run_y - b, 0);
        int bottom = std::min(max_run_y + b, H + 2 * b);
        int cx0 = std::max(left - b, 0), cx1 = std::min(right - b, W);
        int cy0 = std::max(top - b, 0), cy1 = std::min(bottom - b, H);
        RGBA sc = premultiplied(linearized(RGBA{sr, sg, sb, sa}));
        for (int cy = cy0; cy < cy1; ++cy)
            for (int cx = cx0; cx < cx1; ++cx) {
                int lo_x = std::max(cx - b, tx0), hi_x = std::min(cx + b, tx1);
                int lo_y = std::max(cy - b, ty0), hi_y = std::min(cy + b, ty1);
                if (lo_x > hi_x || lo_y > hi_y)
                    continue;
                double acc = 0.0;
                for (int ty = lo_y; ty <= hi_y; ++ty) {
                    double ky = k.k3[cy - ty + b];
                    if (ky == 0.0)
                        continue;
                    for (int tx = lo_x; tx <= hi_x; ++tx)
                        acc += k.k3[cx - tx + b] * ky *
                               S[(size_t)(ty - ty0) * sw + (tx - tx0)];
                }
                if (acc == 0.0)
                    continue;
                double vis = visibility(cx, cy);
                if (vis < k_threshold)
                    continue;
                RGBA fore{galpha * acc * sc.r, galpha * acc * sc.g,
                          galpha * acc * sc.b, galpha * acc * sc.a};
                blend_into(cx, cy, fore, vis);
            }
    }

    void main_pass(const std::vector<Pt> &quad) {
        std::vector<Pt> shape = clip_to_rect(quad, 0, 0, W, H);
        if (shape.empty())
            return;
        double x0 = shape[0].x, x1 = shape[0].x, y0 = shape[0].y, y1 = shape[0].y;
        for (const Pt &p : shape) {
            x0 = std::min(x0, p.x);
            x1 = std::max(x1, p.x);
            y0 = std::min(y0, p.y);
            y1 = std::max(y1, p.y);
        }
        int px0 = std::max(0, (int)std::floor(x0));
        int px1 = std::min(W - 1, (int)std::ceil(x1) - 1);
        int py0 = std::max(0, (int)std::floor(y0));
        int py1 = std::min(H - 1, (int)std::ceil(y1) - 1);
        for (int py = py0; py <= py1; ++py)
            for (int px = px0; px <= px1; ++px) {
                double cov = coverage(shape, px, py);
                if (cov < k_threshold && (op & 8))
                    continue;
                double vis = visibility(px, py);
                if (vis < k_threshold)
                    continue;
                RGBA p = paint_at(paint, inv, px + 0.5, py + 0.5);
                double f = cov * galpha;
                RGBA fore{f * p.r, f * p.g, f * p.b, f * p.a};
                blend_into(px, py, fore, vis);
            }
    }

    // Unpremultiplied sRGB8 with the same ordered dither as get_image_data.
    std::vector<unsigned char> image_bytes() const {
        static const double bayer[4][4] = {
            {0.03125, 0.53125, 0.15625, 0.65625},
            {0.78125, 0.28125, 0.90625, 0.40625},
            {0.21875, 0.71875, 0.09375, 0.59375},
            {0.96875, 0.46875, 0.84375, 0.34375}};
        std::vector<unsigned char> out((size_t)W * H * 4);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                size_t i = (size_t)y * W + x;
                double t = bayer[y & 3][x & 3];
                RGBA c{R[i], G[i], B[i], A[i]};
                RGBA u{0, 0, 0, 0};
                if (c.a >= k_threshold)
                    u = RGBA{c.r / c.a, c.g / c.a, c.b / c.a, c.a};
                double ch[4] = {delinearized(clamp01(u.r)),
                                delinearized(clamp01(u.g)),
                                delinearized(clamp01(u.b)),
                                clamp01(u.a)};
                for (int k = 0; k < 4; ++k)
                    out[i * 4 + k] = (unsigned char)(t + 255.0 * ch[k]);
            }
        return out;
    }
};

} // namespace refshadow
