// Standalone PPM rendering example for the libtess2 geometry tests.
//
// It builds one fixed scene that contains all three features the task cares
// about:
//   * a square with a square hole;
//   * a 45-degree diamond crossing an axis-aligned rectangle;
//   * a disjoint filled square;
// then tesselates it and writes a binary PPM (P6) in which every output
// triangle gets its own deterministic color. The library's boundary contours
// are overlaid in black and the original input edges in dark blue, so holes,
// crossings and boundaries are all visibly distinct.
//
// Pure C++ and the standard library: no image or other dependencies.
//
// Build (from the repository root, after the Source/*.c objects are built):
//   c++ -std=c++17 -O1 -IInclude -ISource task_tests/ppm_example.cpp \
//       build/*.o -lm -o ppm_example
// Run:
//   ./ppm_example [out.ppm]      (default: task_tests/sample.ppm)

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "tesselator.h"

namespace {

struct Pt { float x, y; };
struct Contour { std::vector<Pt> pts; bool reversed; };

constexpr int kW = 720;
constexpr int kH = 720;
constexpr float kMargin = 24.0f;      // pixels
constexpr float kGrid = 32.0f;       // input spans [0,32] on both axes

struct RGB { unsigned char r, g, b; };

float mapX(float x) { return kMargin + x * (kW - 2 * kMargin) / kGrid; }
float mapY(float y) { return kH - kMargin - y * (kH - 2 * kMargin) / kGrid; }

// Deterministic, saturated per-triangle color (no randomness).
RGB triColor(int index) {
    unsigned h = (unsigned)index * 2654435761u + 0x9e3779b9u;
    float hue = (h % 360) / 60.0f;
    float c = 0.85f, x = c * (1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    int sector = (int)hue;
    switch (sector) {
        case 0: r = c; g = x; break;
        case 1: r = x; g = c; break;
        case 2: g = c; b = x; break;
        case 3: g = x; b = c; break;
        case 4: r = x; b = c; break;
        default: r = c; b = x; break;
    }
    return {(unsigned char)(r * 255), (unsigned char)(g * 255),
            (unsigned char)(b * 255)};
}

class Canvas {
public:
    Canvas() : px(kW * kH * 3, 245) {}  // light background shows through holes

    void setPx(int x, int y, RGB c) {
        if (x < 0 || y < 0 || x >= kW || y >= kH) return;
        size_t i = (size_t)(y * kW + x) * 3;
        px[i] = c.r; px[i + 1] = c.g; px[i + 2] = c.b;
    }
    RGB getPx(int x, int y) const {
        size_t i = (size_t)(y * kW + x) * 3;
        return {px[i], px[i + 1], px[i + 2]};
    }

    void fillTri(Pt a, Pt b, Pt c, RGB color) {
        float ax = mapX(a.x), ay = mapY(a.y);
        float bx = mapX(b.x), by = mapY(b.y);
        float cx = mapX(c.x), cy = mapY(c.y);
        int y0 = (int)std::floor(std::min(ay, std::min(by, cy)));
        int y1 = (int)std::ceil(std::max(ay, std::max(by, cy)));
        for (int y = y0; y <= y1; ++y) {
            float fy = y + 0.5f;
            float xs[3] = {ax, bx, cx};
            float ys[3] = {ay, by, cy};
            float xmin = 1e9f, xmax = -1e9f;
            int hits = 0;
            for (int i = 0; i < 3; ++i) {
                float ya = ys[i], yb = ys[(i + 1) % 3];
                float xa = xs[i], xb = xs[(i + 1) % 3];
                if (std::fmin(ya, yb) <= fy && fy < std::fmax(ya, yb)) {
                    float t = (fy - ya) / (yb - ya);
                    float xx = xa + t * (xb - xa);
                    xmin = std::min(xmin, xx);
                    xmax = std::max(xmax, xx);
                    ++hits;
                }
            }
            if (hits != 2) continue;
            for (int x = (int)std::floor(xmin); x <= (int)std::ceil(xmax); ++x)
                setPx(x, y, color);
        }
    }

    void line(Pt a, Pt b, RGB color, int thickness) {
        float ax = mapX(a.x), ay = mapY(a.y), bx = mapX(b.x), by = mapY(b.y);
        int steps = (int)std::ceil(std::hypot(bx - ax, by - ay));
        for (int i = 0; i <= steps; ++i) {
            float t = steps ? (float)i / steps : 0.0f;
            int x = (int)std::lround(ax + t * (bx - ax));
            int y = (int)std::lround(ay + t * (by - ay));
            for (int dy = -thickness; dy <= thickness; ++dy)
                for (int dx = -thickness; dx <= thickness; ++dx)
                    setPx(x + dx, y + dy, color);
        }
    }

    bool save(const std::string& path) const {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::fprintf(f, "P6\n%d %d\n255\n", kW, kH);
        if (std::fwrite(px.data(), 1, px.size(), f) != px.size()) {
            std::fclose(f);
            return false;
        }
        std::fclose(f);
        return true;
    }

private:
    std::vector<unsigned char> px;
};

void addContour(TESStesselator* t, const Contour& c) {
    tessSetOption(t, TESS_REVERSE_CONTOURS, c.reversed ? 1 : 0);
    tessAddContour(t, 2, c.pts.data(), 2 * (int)sizeof(float),
                   (int)c.pts.size());
    tessSetOption(t, TESS_REVERSE_CONTOURS, 0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string out = argc > 1 ? argv[1] : "task_tests/sample.ppm";

    // Square with a square hole (left half of the canvas).
    Contour outer = {{{2, 2}, {16, 2}, {16, 16}, {2, 16}}, false};
    Contour hole = {{{6, 6}, {12, 6}, {12, 12}, {6, 12}}, true};
    // Diamond crossing an axis-aligned rectangle (center).
    Contour diamond = {{{20, 4}, {26, 10}, {20, 16}, {14, 10}}, false};
    Contour rect = {{{17, 7}, {25, 7}, {25, 13}, {17, 13}}, false};
    // Fully disjoint filled square (bottom-right).
    Contour disjoint = {{{24, 20}, {30, 20}, {30, 26}, {24, 26}}, false};
    std::vector<Contour> contours = {outer, hole, diamond, rect, disjoint};

    TESStesselator* t = tessNewTess(nullptr);
    if (!t) return 2;
    for (const Contour& c : contours) addContour(t, c);
    if (!tessTesselate(t, TESS_WINDING_NONZERO, TESS_POLYGONS, 3, 2, nullptr)) {
        std::fprintf(stderr, "tessellation failed: %d\n", tessGetStatus(t));
        tessDeleteTess(t);
        return 3;
    }

    Canvas canvas;
    const float* v = tessGetVertices(t);
    const TESSindex* e = tessGetElements(t);
    int triCount = tessGetElementCount(t);
    for (int f = 0; f < triCount; ++f) {
        Pt a = {v[2 * e[3 * f]], v[2 * e[3 * f] + 1]};
        Pt b = {v[2 * e[3 * f + 1]], v[2 * e[3 * f + 1] + 1]};
        Pt c = {v[2 * e[3 * f + 2]], v[2 * e[3 * f + 2] + 1]};
        canvas.fillTri(a, b, c, triColor(f));
    }

    // Original input edges in dark blue make the crossing geometry explicit.
    const RGB blue = {20, 40, 160};
    for (const Contour& c : contours)
        for (size_t i = 0; i < c.pts.size(); ++i)
            canvas.line(c.pts[i], c.pts[(i + 1) % c.pts.size()], blue, 1);

    // Library boundary contours in black: outer loops, hole rims and the
    // boundaries around the crossed regions.
    TESStesselator* tb = tessNewTess(nullptr);
    for (const Contour& c : contours) addContour(tb, c);
    if (tessTesselate(tb, TESS_WINDING_NONZERO, TESS_BOUNDARY_CONTOURS, 3, 2,
                      nullptr)) {
        const float* bv = tessGetVertices(tb);
        const TESSindex* be = tessGetElements(tb);
        const RGB black = {0, 0, 0};
        for (int f = 0; f < tessGetElementCount(tb); ++f) {
            int base = be[2 * f], count = be[2 * f + 1];
            for (int k = 0; k < count; ++k) {
                Pt a = {bv[2 * (base + k)], bv[2 * (base + k) + 1]};
                Pt b = {bv[2 * (base + (k + 1) % count)],
                        bv[2 * (base + (k + 1) % count) + 1]};
                canvas.line(a, b, black, 1);
            }
        }
    }
    tessDeleteTess(tb);
    tessDeleteTess(t);

    if (!canvas.save(out)) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 4;
    }
    std::printf("wrote %s (%dx%d P6, %d colored triangles)\n", out.c_str(), kW,
                kH, triCount);
    return 0;
}
