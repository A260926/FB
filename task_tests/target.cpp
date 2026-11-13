// target.cpp -- numerical regression tests for canvas_ity shadow rendering.
//
// These tests drive the public canvas_ity API only and compare the rendered
// pixels against an independent reference model (ref_shadow.hpp) that uses
// exact convex-quad coverage and direct double-precision convolution.  The
// production rendering logic is untouched; the legacy image-hash tests keep
// working as before.
//
// Groups:
//   kernel    -- the three-pass extended box filter: radii 0/1/2 with
//                nonzero tail weights, full-image alpha, separability of
//                the 2D kernel, and total shadow mass.
//   offscreen -- shadow sources that lie partly or fully outside the canvas
//                but whose offset/blurred shadows reach back inside, plus
//                interaction with (nested) clipping.
//   paint     -- how source paint alpha modulates the shadow: transparent
//                solid colors, gradients, patterns, device-pixel shadow
//                geometry under transforms, single global-alpha application,
//                and linear-premultiplied compositing.
//   state     -- save/restore of shadow parameters interleaved with
//                shadowed and plain drawing, checked through source_over
//                and destination_out compositing.
//
// Every scene is also re-rendered with shadows disabled and compared
// against the reference's main pass alone, so a wrong main image cannot be
// masked by a compensating shadow error.

#define CANVAS_ITY_IMPLEMENTATION
#include "canvas_ity.hpp"

#include "registry.hpp"
#include "ref_shadow.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// ======== scene drivers ========
//
// Each scene is a function void(Driver&) that issues high-level drawing
// commands.  Two drivers execute the same scene: one forwards to a real
// canvas_ity::canvas, the other to the independent reference model.

struct Driver {
    virtual ~Driver() {}
    virtual void fill_color(double r, double g, double b, double a) = 0;
    virtual void fill_gradient(double x0, double y0, double x1, double y1,
                               const std::vector<std::array<double, 5>> &stops) = 0;
    virtual void fill_pattern(const unsigned char *tex, int w, int h,
                              int repetition) = 0;
    virtual void shadow(double r, double g, double b, double a,
                        double ox, double oy, double blur) = 0;
    virtual void no_shadow() = 0;
    virtual void op(int operation) = 0;
    virtual void galpha(double a) = 0;
    virtual void transform(const double m[6]) = 0;
    virtual void fill_rect(double x, double y, double w, double h) = 0;
    virtual void clip_rect(double x, double y, double w, double h) = 0;
    virtual void save() = 0;
    virtual void restore() = 0;
};

struct LibDriver : Driver {
    canvas_ity::canvas c;
    bool disable_shadows;
    LibDriver(int w, int h, bool ds) : c(w, h), disable_shadows(ds) {}
    void fill_color(double r, double g, double b, double a) override {
        c.set_color(canvas_ity::fill_style, (float)r, (float)g, (float)b, (float)a);
    }
    void fill_gradient(double x0, double y0, double x1, double y1,
                       const std::vector<std::array<double, 5>> &stops) override {
        c.set_linear_gradient(canvas_ity::fill_style, (float)x0, (float)y0,
                              (float)x1, (float)y1);
        for (const auto &s : stops)
            c.add_color_stop(canvas_ity::fill_style, (float)s[0], (float)s[1],
                             (float)s[2], (float)s[3], (float)s[4]);
    }
    void fill_pattern(const unsigned char *tex, int w, int h,
                      int repetition) override {
        c.set_pattern(canvas_ity::fill_style, tex, w, h, w * 4,
                      (canvas_ity::repetition_style)repetition);
    }
    void shadow(double r, double g, double b, double a,
                double ox, double oy, double blur) override {
        if (disable_shadows)
            a = 0.0;
        c.set_shadow_color((float)r, (float)g, (float)b, (float)a);
        c.shadow_offset_x = (float)ox;
        c.shadow_offset_y = (float)oy;
        c.set_shadow_blur((float)blur);
    }
    void no_shadow() override { c.set_shadow_color(0, 0, 0, 0); }
    void op(int operation) override {
        c.global_composite_operation = (canvas_ity::composite_operation)operation;
    }
    void galpha(double a) override { c.set_global_alpha((float)a); }
    void transform(const double m[6]) override {
        c.set_transform((float)m[0], (float)m[1], (float)m[2],
                        (float)m[3], (float)m[4], (float)m[5]);
    }
    void fill_rect(double x, double y, double w, double h) override {
        c.fill_rectangle((float)x, (float)y, (float)w, (float)h);
    }
    void clip_rect(double x, double y, double w, double h) override {
        c.begin_path();
        c.rectangle((float)x, (float)y, (float)w, (float)h);
        c.clip();
    }
    void save() override { c.save(); }
    void restore() override { c.restore(); }
};

struct RefDriver : Driver {
    refshadow::Canvas rc;
    bool disable_shadows;
    RefDriver(int w, int h, bool ds) : rc(w, h), disable_shadows(ds) {}
    void fill_color(double r, double g, double b, double a) override {
        rc.paint = refshadow::Paint();
        rc.paint.type = refshadow::Paint::Color;
        rc.paint.color = refshadow::RGBA{r, g, b, a};
    }
    void fill_gradient(double x0, double y0, double x1, double y1,
                       const std::vector<std::array<double, 5>> &stops) override {
        rc.paint = refshadow::Paint();
        rc.paint.type = refshadow::Paint::Linear;
        rc.paint.x0 = x0; rc.paint.y0 = y0;
        rc.paint.x1 = x1; rc.paint.y1 = y1;
        rc.paint.stops = stops;
    }
    void fill_pattern(const unsigned char *tex, int w, int h,
                      int repetition) override {
        rc.paint = refshadow::Paint();
        rc.paint.type = refshadow::Paint::Pattern;
        rc.paint.tw = w; rc.paint.th = h;
        rc.paint.repetition = repetition;
        for (int i = 0; i < w * h; ++i) {
            refshadow::RGBA c{tex[4 * i + 0] / 255.0, tex[4 * i + 1] / 255.0,
                              tex[4 * i + 2] / 255.0, tex[4 * i + 3] / 255.0};
            rc.paint.texels.push_back(refshadow::premultiplied(
                refshadow::linearized(c)));
        }
    }
    void shadow(double r, double g, double b, double a,
                double ox, double oy, double blur) override {
        if (disable_shadows)
            a = 0.0;
        rc.sr = r; rc.sg = g; rc.sb = b; rc.sa = a;
        rc.sox = ox; rc.soy = oy; rc.sblur = blur;
    }
    void no_shadow() override { rc.sa = 0.0; }
    void op(int operation) override { rc.op = operation; }
    void galpha(double a) override { rc.galpha = a; }
    void transform(const double m[6]) override {
        rc.set_transform(refshadow::Mat{m[0], m[1], m[2], m[3], m[4], m[5]});
    }
    void fill_rect(double x, double y, double w, double h) override {
        rc.fill_rect(x, y, w, h);
    }
    void clip_rect(double x, double y, double w, double h) override {
        rc.clip_rect(x, y, w, h);
    }
    void save() override { rc.save(); }
    void restore() override { rc.restore(); }
};

// ======== comparison helpers ========

std::vector<unsigned char> lib_bytes(canvas_ity::canvas &c, int w, int h) {
    std::vector<unsigned char> out((size_t)w * h * 4);
    c.get_image_data(out.data(), w, h, w * 4, 0, 0);
    return out;
}

void fail_at(Case &C, const char *scene, int x, int y, const char *channel,
             int expected, int actual) {
    char msg[256];
    std::snprintf(msg, sizeof msg,
                  "scene '%s' pixel (%d,%d) %s: expected %d, actual %d",
                  scene, x, y, channel, expected, actual);
    C.check(false, msg);
}

// Alpha is compared to within 1 byte everywhere; color channels to within
// 2 bytes wherever the reference alpha is not low (below that, the
// unpremultiplied color is dominated by quantization noise).
void compare_images(Case &C, const char *scene,
                    const std::vector<unsigned char> &got,
                    const std::vector<unsigned char> &want, int w, int h) {
    const int tol_alpha = 1, tol_color = 2;
    const int color_floor = 26; // ~0.1 * 255
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int i = 4 * (y * w + x);
            int da = std::abs((int)got[i + 3] - (int)want[i + 3]);
            if (da > tol_alpha)
                fail_at(C, scene, x, y, "alpha", want[i + 3], got[i + 3]);
            C.check(true);
            if (want[i + 3] < color_floor)
                continue;
            for (int ch = 0; ch < 3; ++ch)
                if (std::abs((int)got[i + ch] - (int)want[i + ch]) > tol_color) {
                    const char *names[3] = {"red", "green", "blue"};
                    fail_at(C, scene, x, y, names[ch], want[i + ch], got[i + ch]);
                }
            C.check(true);
        }
}

struct Extra {
    virtual ~Extra() {}
    virtual void check_scene(Case &C, const std::vector<unsigned char> &got,
                             const std::vector<unsigned char> &want,
                             int w, int h) const = 0;
};

// Runs one scene through both the library and the reference, once with
// shadows enabled and once disabled, and compares all four images.
void run_scene(Case &C, const char *name, int w, int h,
               void (*scene)(Driver &), const Extra *extra = nullptr) {
    std::string shadowed = std::string(name) + " [shadow]";
    std::string plain = std::string(name) + " [no shadow]";
    {
        LibDriver lib(w, h, false);
        RefDriver ref(w, h, false);
        scene(lib);
        scene(ref);
        std::vector<unsigned char> got = lib_bytes(lib.c, w, h);
        std::vector<unsigned char> want = ref.rc.image_bytes();
        compare_images(C, shadowed.c_str(), got, want, w, h);
        if (extra)
            extra->check_scene(C, got, want, w, h);
    }
    {
        LibDriver lib(w, h, true);
        RefDriver ref(w, h, true);
        scene(lib);
        scene(ref);
        compare_images(C, plain.c_str(), lib_bytes(lib.c, w, h),
                       ref.rc.image_bytes(), w, h);
    }
}

// Total alpha mass over the canvas must match the reference when the
// blurred shadow is fully inside the canvas (nothing clipped away).
struct MassExtra : Extra {
    void check_scene(Case &C, const std::vector<unsigned char> &got,
                     const std::vector<unsigned char> &want,
                     int w, int h) const override {
        long gs = 0, ws = 0;
        for (int i = 0; i < w * h; ++i) {
            gs += got[4 * i + 3];
            ws += want[4 * i + 3];
        }
        double tol = 0.015 * ws + 4.0 * std::sqrt((double)w * h) + 2.0;
        if (std::fabs((double)(gs - ws)) > tol) {
            char msg[256];
            std::snprintf(msg, sizeof msg,
                          "scene mass: expected alpha sum %ld, actual %ld",
                          ws, gs);
            C.check(false, msg);
        }
        C.check(true);
    }
};

// For a rectangular source the blurred shadow alpha factorizes as
// f(x)*g(y); check the identity A(x1,y1)*A(x2,y2) == A(x1,y2)*A(x2,y1)
// on the library output at four pure-shadow pixels.
struct SeparableExtra : Extra {
    int x1, y1, x2, y2;
    SeparableExtra(int x1, int y1, int x2, int y2)
        : x1(x1), y1(y1), x2(x2), y2(y2) {}
    void check_scene(Case &C, const std::vector<unsigned char> &got,
                     const std::vector<unsigned char> &want,
                     int w, int) const override {
        (void)want;
        auto alpha = [&](int x, int y) {
            return got[4 * (y * w + x) + 3] / 255.0;
        };
        double lhs = alpha(x1, y1) * alpha(x2, y2);
        double rhs = alpha(x1, y2) * alpha(x2, y1);
        if (std::fabs(lhs - rhs) > 0.02) {
            char msg[256];
            std::snprintf(msg, sizeof msg,
                          "scene separability: A(%d,%d)*A(%d,%d)=%.4f != "
                          "A(%d,%d)*A(%d,%d)=%.4f",
                          x1, y1, x2, y2, lhs, x1, y2, x2, y1, rhs);
            C.check(false, msg);
        }
        C.check(true);
    }
};

// Asserts a specific pixel's alpha byte in the library output.
struct PixelExtra : Extra {
    int x, y, lo, hi;
    const char *what;
    PixelExtra(int x, int y, int lo, int hi, const char *what)
        : x(x), y(y), lo(lo), hi(hi), what(what) {}
    void check_scene(Case &C, const std::vector<unsigned char> &got,
                     const std::vector<unsigned char> &want,
                     int w, int) const override {
        (void)want;
        int a = got[4 * (y * w + x) + 3];
        if (a < lo || a > hi) {
            char msg[256];
            std::snprintf(msg, sizeof msg,
                          "scene pixel (%d,%d) %s: expected alpha in "
                          "[%d,%d], actual %d",
                          x, y, what, lo, hi, a);
            C.check(false, msg);
        }
        C.check(true);
    }
};

struct ChainExtra : Extra {
    std::vector<const Extra *> extras;
    ~ChainExtra() { for (const Extra *e : extras) delete e; }
    void add(const Extra *e) { extras.push_back(e); }
    void check_scene(Case &C, const std::vector<unsigned char> &got,
                     const std::vector<unsigned char> &want,
                     int w, int h) const override {
        for (const Extra *e : extras)
            e->check_scene(C, got, want, w, h);
    }
};

// ======== kernel scenes ========

void scene_kernel_control(Driver &d) {
    d.fill_color(0.15, 0.45, 0.95, 1.0);
    d.shadow(0.0, 0.0, 0.0, 0.6, 5.0, 3.0, 0.0);
    d.fill_rect(4, 3, 8, 6);
}

void scene_kernel_blur2(Driver &d) {
    d.fill_color(1.0, 1.0, 1.0, 1.0);
    d.shadow(0.0, 0.0, 0.0, 1.0, 7.0, 5.0, 2.0);
    d.fill_rect(3, 3, 7, 5);
}

void scene_kernel_blur4(Driver &d) {
    d.fill_color(1.0, 1.0, 1.0, 1.0);
    d.shadow(0.0, 0.0, 0.0, 1.0, 9.0, 7.0, 4.0);
    d.fill_rect(3, 3, 6, 5);
}

void scene_kernel_blur6(Driver &d) {
    d.fill_color(1.0, 1.0, 1.0, 1.0);
    d.shadow(0.0, 0.0, 0.0, 1.0, 11.0, 9.0, 6.0);
    d.fill_rect(2, 2, 6, 4);
}

void scene_kernel_mass(Driver &d) {
    d.fill_color(0.9, 0.9, 0.9, 0.8);
    d.shadow(0.0, 0.0, 0.0, 0.9, 5.0, 4.0, 4.0);
    d.fill_rect(4, 3, 9, 6);
}

void case_kernel_control(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(13, 10, 150, 156, "offset shadow core"));
    extra.add(new PixelExtra(5, 4, 254, 255, "opaque body"));
    extra.add(new PixelExtra(20, 15, 0, 0, "outside shadow"));
    run_scene(C, "kernel offset control", 30, 24, scene_kernel_control, &extra);
}

void case_kernel_blur2(Case &C) {
    ChainExtra extra;
    extra.add(new MassExtra());
    extra.add(new SeparableExtra(18, 14, 19, 15));
    extra.add(new PixelExtra(19, 10, 1, 4, "kernel tail at radius 3"));
    extra.add(new PixelExtra(17, 10, 70, 82, "kernel fringe at radius 1"));
    run_scene(C, "kernel blur 2 (radius 0, tail 0.25)", 34, 26,
              scene_kernel_blur2, &extra);
}

void case_kernel_blur4(Case &C) {
    ChainExtra extra;
    extra.add(new MassExtra());
    extra.add(new SeparableExtra(20, 17, 22, 19));
    extra.add(new PixelExtra(22, 12, 1, 5, "kernel tail at radius 5"));
    run_scene(C, "kernel blur 4 (radius 1, tail 0.375)", 40, 30,
              scene_kernel_blur4, &extra);
}

void case_kernel_blur6(Case &C) {
    ChainExtra extra;
    extra.add(new MassExtra());
    extra.add(new SeparableExtra(20, 16, 23, 19));
    run_scene(C, "kernel blur 6 (radius 2, tail 5/12)", 44, 34,
              scene_kernel_blur6, &extra);
}

void case_kernel_mass(Case &C) {
    ChainExtra extra;
    extra.add(new MassExtra());
    extra.add(new SeparableExtra(19, 14, 21, 16));
    run_scene(C, "kernel mass and separability", 36, 28,
              scene_kernel_mass, &extra);
}

// ======== offscreen scenes ========

void scene_off_left(Driver &d) {
    d.fill_color(1.0, 0.3, 0.1, 1.0);
    d.shadow(0.0, 0.0, 0.0, 0.75, 10.0, 2.0, 2.0);
    d.fill_rect(-14, 4, 8, 8);
}

void scene_off_right(Driver &d) {
    d.fill_color(0.2, 0.9, 0.4, 1.0);
    d.shadow(0.0, 0.0, 0.0, 0.8, -12.0, 3.0, 4.0);
    d.fill_rect(34, 3, 8, 9);
}

void scene_off_top(Driver &d) {
    d.fill_color(0.9, 0.8, 0.1, 1.0);
    d.shadow(0.0, 0.0, 0.0, 0.8, 0.0, 0.0, 6.0);
    d.fill_rect(3, -13, 10, 8);
}

void scene_off_bottom(Driver &d) {
    d.fill_color(0.1, 0.4, 0.9, 1.0);
    d.shadow(0.0, 0.0, 0.0, 0.85, 2.0, -9.0, 2.0);
    d.fill_rect(4, 26, 9, 7);
}

void scene_off_edge_bleed(Driver &d) {
    d.fill_color(0.95, 0.95, 0.95, 1.0);
    d.shadow(0.0, 0.0, 0.0, 0.9, 0.0, 0.0, 6.0);
    d.fill_rect(-8, 3, 14, 7);
}

void scene_off_nested_clip(Driver &d) {
    d.clip_rect(4, 2, 22, 18);
    d.clip_rect(8, 5, 14, 11);
    d.fill_color(0.8, 0.2, 0.5, 1.0);
    d.shadow(0.0, 0.0, 0.0, 0.8, 14.0, 3.0, 2.0);
    d.fill_rect(-12, 6, 8, 8);
}

void scene_off_clipped_source(Driver &d) {
    d.clip_rect(13, 0, 13, 18);
    d.fill_color(0.9, 0.6, 0.1, 1.0);
    d.shadow(0.0, 0.0, 0.0, 0.9, 12.0, 2.0, 2.0);
    d.fill_rect(2, 4, 6, 8);
}

void case_off_left(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(1, 8, 150, 200, "shadow from off-canvas left"));
    extra.add(new PixelExtra(12, 8, 0, 0, "beyond shadow reach"));
    run_scene(C, "offscreen source left", 28, 20, scene_off_left, &extra);
}

void case_off_right(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(25, 8, 150, 210, "shadow from off-canvas right"));
    extra.add(new PixelExtra(10, 8, 0, 0, "beyond shadow reach"));
    run_scene(C, "offscreen source right", 28, 20, scene_off_right, &extra);
}

void case_off_top(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(7, 0, 5, 15, "blur fringe from off-canvas top"));
    extra.add(new PixelExtra(7, 12, 0, 0, "beyond blur reach"));
    run_scene(C, "offscreen source top", 24, 22, scene_off_top, &extra);
}

void case_off_bottom(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(8, 19, 150, 220, "shadow from off-canvas bottom"));
    extra.add(new PixelExtra(8, 8, 0, 0, "beyond shadow reach"));
    run_scene(C, "offscreen source bottom", 24, 22, scene_off_bottom, &extra);
}

void case_off_edge_bleed(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(2, 12, 35, 50, "bleed from off-canvas half"));
    run_scene(C, "edge bleed keeps off-canvas source", 30, 22,
              scene_off_edge_bleed, &extra);
}

void case_off_nested_clip(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(9, 12, 100, 210, "shadow inside nested clip"));
    extra.add(new PixelExtra(6, 12, 0, 0, "clipped away column"));
    extra.add(new PixelExtra(15, 19, 0, 0, "clipped away row"));
    extra.add(new PixelExtra(24, 10, 0, 0, "outside inner clip"));
    run_scene(C, "offscreen source with nested clip", 30, 22,
              scene_off_nested_clip, &extra);
}

void case_off_clipped_source(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(16, 9, 150, 235, "shadow cast into allowed half"));
    extra.add(new PixelExtra(4, 7, 0, 0, "source body clipped away"));
    run_scene(C, "clipped source still casts", 26, 18,
              scene_off_clipped_source, &extra);
}

// ======== paint scenes ========

void scene_paint_solid(Driver &d) {
    d.fill_color(1.0, 0.0, 0.0, 0.5);
    d.shadow(0.0, 0.0, 1.0, 0.75, 6.0, 4.0, 2.0);
    d.fill_rect(3, 3, 8, 6);
}

void scene_paint_gradient(Driver &d) {
    d.fill_gradient(0, 4, 0, 10,
                    {{0.0, 1.0, 0.0, 0.0, 0.1}, {1.0, 0.0, 0.0, 1.0, 0.9}});
    d.shadow(0.0, 0.0, 0.0, 0.8, 0.0, 8.0, 2.0);
    d.fill_rect(3, 4, 10, 6);
}

void scene_paint_pattern(Driver &d) {
    unsigned char tex[5 * 4 * 4];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 5; ++x) {
            int i = 4 * (y * 5 + x);
            tex[i + 0] = (unsigned char)((40 + x * 37 + y * 11) % 256);
            tex[i + 1] = (unsigned char)((90 + x * 13 + y * 53) % 256);
            tex[i + 2] = (unsigned char)((10 + x * 71 + y * 29) % 256);
            tex[i + 3] = (unsigned char)(255 - ((x * 7 + y * 19) % 9) * 28);
        }
    d.fill_pattern(tex, 5, 4, 0);
    d.shadow(0.0, 0.0, 0.0, 0.85, 7.0, 6.0, 2.0);
    d.fill_rect(4, 3, 12, 8);
}

void scene_paint_transform_scale(Driver &d) {
    double m[6] = {2.0, 0.0, 0.0, 2.0, 3.0, 2.0};
    d.transform(m);
    d.fill_color(0.2, 0.7, 0.9, 0.9);
    d.shadow(0.0, 0.0, 0.0, 0.8, 4.0, 3.0, 4.0);
    d.fill_rect(2, 2, 5, 4);
}

void scene_paint_transform_rotate(Driver &d) {
    double cs = std::cos(0.5), sn = std::sin(0.5);
    double m[6] = {cs, sn, -sn, cs, 18.0, 15.0};
    d.transform(m);
    d.fill_color(0.9, 0.8, 0.2, 0.95);
    d.shadow(0.1, 0.0, 0.3, 0.85, 5.0, 3.0, 2.0);
    d.fill_rect(-5, -3, 10, 6);
}

void scene_paint_global_alpha(Driver &d) {
    d.galpha(0.6);
    d.fill_color(0.0, 1.0, 0.0, 0.8);
    d.shadow(1.0, 0.0, 0.0, 0.7, 5.0, 4.0, 2.0);
    d.fill_rect(3, 3, 7, 5);
}

void scene_paint_linear_compose(Driver &d) {
    d.no_shadow();
    d.fill_color(0.2, 0.8, 0.3, 1.0);
    d.fill_rect(0, 0, 28, 22);
    d.fill_color(1.0, 1.0, 1.0, 1.0);
    d.shadow(1.0, 0.0, 0.0, 0.5, 4.0, 3.0, 2.0);
    d.fill_rect(5, 4, 9, 6);
}

void case_paint_solid(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(13, 10, 90, 100, "blue shadow alpha"));
    extra.add(new PixelExtra(5, 5, 125, 130, "red body alpha"));
    run_scene(C, "paint transparent solid", 30, 24, scene_paint_solid, &extra);
}

void case_paint_gradient(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(7, 12, 25, 40, "shadow of low-alpha source row"));
    extra.add(new PixelExtra(7, 17, 100, 120, "shadow of high-alpha source row"));
    run_scene(C, "paint gradient alpha", 20, 26, scene_paint_gradient, &extra);
}

void case_paint_pattern(Case &C) {
    run_scene(C, "paint pattern alpha", 30, 24, scene_paint_pattern);
}

void case_paint_transform_scale(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(17, 11, 150, 165, "device-pixel offset shadow"));
    run_scene(C, "paint device-pixel shadow under scale", 34, 28,
              scene_paint_transform_scale, &extra);
}

void case_paint_transform_rotate(Case &C) {
    run_scene(C, "paint device-pixel shadow under rotation", 36, 30,
              scene_paint_transform_rotate);
}

void case_paint_global_alpha(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(9, 8, 70, 82, "global alpha applied once"));
    run_scene(C, "paint global alpha once", 26, 20,
              scene_paint_global_alpha, &extra);
}

void case_paint_linear_compose(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(16, 11, 250, 255, "red shadow over green bg"));
    run_scene(C, "paint linear-space compositing", 28, 22,
              scene_paint_linear_compose, &extra);
}

// ======== state scenes ========

void scene_state_interleave(Driver &d) {
    d.no_shadow();
    d.fill_color(0.1, 0.2, 0.9, 1.0);
    d.fill_rect(1, 1, 8, 20);
    d.save();
    d.shadow(0.0, 0.0, 0.0, 0.5, 3.0, 2.0, 2.0);
    d.fill_color(1.0, 0.5, 0.0, 0.9);
    d.fill_rect(12, 3, 7, 6);
    d.restore();
    d.fill_color(0.0, 0.7, 0.4, 0.8);
    d.fill_rect(24, 2, 6, 8);
    d.save();
    d.shadow(0.2, 0.0, 0.4, 0.8, -4.0, 3.0, 4.0);
    d.save();
    d.fill_color(0.9, 0.9, 0.1, 1.0);
    d.fill_rect(30, 12, 8, 7);
    d.restore();
    d.fill_color(0.6, 0.1, 0.7, 0.7);
    d.fill_rect(14, 14, 6, 5);
    d.restore();
    d.fill_color(0.3, 0.3, 0.3, 1.0);
    d.fill_rect(36, 2, 4, 18);
}

void scene_state_composite(Driver &d) {
    d.no_shadow();
    d.fill_color(0.9, 0.85, 0.7, 1.0);
    d.fill_rect(0, 0, 32, 22);
    d.op(canvas_ity::source_over);
    d.shadow(0.0, 0.0, 0.0, 0.5, 4.0, 3.0, 2.0);
    d.fill_color(0.0, 0.0, 1.0, 0.6);
    d.fill_rect(4, 4, 10, 7);
    d.op(canvas_ity::destination_out);
    d.shadow(0.0, 0.0, 0.0, 0.8, -3.0, 2.0, 4.0);
    d.fill_color(0.0, 0.0, 0.0, 0.9);
    d.fill_rect(10, 8, 12, 8);
    d.op(canvas_ity::source_over);
    d.no_shadow();
    d.fill_color(0.0, 1.0, 0.5, 0.5);
    d.fill_rect(24, 14, 6, 6);
}

void scene_state_save_restore(Driver &d) {
    d.shadow(0.0, 0.0, 0.0, 0.6, 2.0, 2.0, 2.0);
    d.fill_color(1.0, 0.0, 0.0, 1.0);
    d.fill_rect(2, 2, 6, 5);
    d.save();
    d.shadow(0.0, 0.0, 1.0, 0.8, 6.0, 4.0, 4.0);
    d.fill_color(0.0, 1.0, 0.0, 1.0);
    d.fill_rect(12, 3, 6, 5);
    d.restore();
    d.fill_color(0.0, 0.0, 1.0, 1.0);
    d.fill_rect(22, 2, 6, 5);
    d.save();
    d.no_shadow();
    d.fill_color(1.0, 1.0, 0.0, 1.0);
    d.fill_rect(30, 3, 5, 5);
    d.restore();
    d.fill_color(0.5, 0.0, 0.5, 1.0);
    d.fill_rect(6, 11, 6, 5);
}

void case_state_interleave(Case &C) {
    run_scene(C, "state interleaved shadows", 42, 24, scene_state_interleave);
}

void case_state_composite(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(14, 12, 0, 30, "erased by destination_out body"));
    extra.add(new PixelExtra(2, 2, 250, 255, "untouched background"));
    run_scene(C, "state composite operations", 32, 22,
              scene_state_composite, &extra);
}

void case_state_save_restore(Case &C) {
    ChainExtra extra;
    extra.add(new PixelExtra(28, 7, 128, 145, "restored shadow A"));
    extra.add(new PixelExtra(31, 5, 250, 255, "no-shadow body"));
    run_scene(C, "state save/restore of shadow params", 36, 20,
              scene_state_save_restore, &extra);
}

} // namespace

void register_task_tests(Registry &R) {
    R.add("kernel", "offset_control", case_kernel_control);
    R.add("kernel", "blur2_radius0", case_kernel_blur2);
    R.add("kernel", "blur4_radius1", case_kernel_blur4);
    R.add("kernel", "blur6_radius2", case_kernel_blur6);
    R.add("kernel", "mass_separability", case_kernel_mass);
    R.add("offscreen", "left", case_off_left);
    R.add("offscreen", "right", case_off_right);
    R.add("offscreen", "top", case_off_top);
    R.add("offscreen", "bottom", case_off_bottom);
    R.add("offscreen", "edge_bleed", case_off_edge_bleed);
    R.add("offscreen", "nested_clip", case_off_nested_clip);
    R.add("offscreen", "clipped_source", case_off_clipped_source);
    R.add("paint", "solid_alpha", case_paint_solid);
    R.add("paint", "gradient_alpha", case_paint_gradient);
    R.add("paint", "pattern_alpha", case_paint_pattern);
    R.add("paint", "transform_scale", case_paint_transform_scale);
    R.add("paint", "transform_rotate", case_paint_transform_rotate);
    R.add("paint", "global_alpha_once", case_paint_global_alpha);
    R.add("paint", "linear_compose", case_paint_linear_compose);
    R.add("state", "interleave", case_state_interleave);
    R.add("state", "composite_ops", case_state_composite);
    R.add("state", "save_restore", case_state_save_restore);
}
