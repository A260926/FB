// shadow_ppm.cpp -- visual example for the shadow reference model.
//
// Renders a small deterministic scene that combines an off-canvas shadow
// source with nested clipping, once with canvas_ity and once with the
// independent reference model, then writes a PPM (P6) image with two
// magnified panels:
//
//   left  -- alpha channel of the library output (grayscale)
//   right -- absolute difference between library and reference, with
//            alpha differences in red (x32) and color differences in
//            blue (x8); a correct build shows this panel essentially black
//
// Usage:  c++ -std=c++17 -Isrc -Itask_tests task_tests/shadow_ppm.cpp -o shadow_ppm
//         ./shadow_ppm [output.ppm]

#define CANVAS_ITY_IMPLEMENTATION
#include "canvas_ity.hpp"

#include "ref_shadow.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

const int W = 30, H = 22, SCALE = 8;

// The same deterministic scene as the offscreen/nested_clip regression
// case: a source rectangle entirely left of the canvas whose offset,
// blurred shadow falls back inside a doubly clipped region.
void scene(canvas_ity::canvas &c) {
    c.begin_path();
    c.rectangle(4, 2, 22, 18);
    c.clip();
    c.begin_path();
    c.rectangle(8, 5, 14, 11);
    c.clip();
    c.set_color(canvas_ity::fill_style, 0.8f, 0.2f, 0.5f, 1.0f);
    c.set_shadow_color(0.0f, 0.0f, 0.0f, 0.8f);
    c.shadow_offset_x = 14.0f;
    c.shadow_offset_y = 3.0f;
    c.set_shadow_blur(2.0f);
    c.fill_rectangle(-12, 6, 8, 8);
}

void scene(refshadow::Canvas &rc) {
    rc.clip_rect(4, 2, 22, 18);
    rc.clip_rect(8, 5, 14, 11);
    rc.paint.type = refshadow::Paint::Color;
    rc.paint.color = refshadow::RGBA{0.8, 0.2, 0.5, 1.0};
    rc.sr = 0.0; rc.sg = 0.0; rc.sb = 0.0; rc.sa = 0.8;
    rc.sox = 14.0; rc.soy = 3.0; rc.sblur = 2.0;
    rc.fill_rect(-12, 6, 8, 8);
}

} // namespace

int main(int argc, char **argv) {
    std::string path = argc > 1 ? argv[1] : "shadow_example.ppm";

    canvas_ity::canvas lib(W, H);
    scene(lib);
    std::vector<unsigned char> got((size_t)W * H * 4);
    lib.get_image_data(got.data(), W, H, W * 4, 0, 0);

    refshadow::Canvas ref(W, H);
    scene(ref);
    std::vector<unsigned char> want = ref.image_bytes();

    int max_alpha = 0, max_color = 0;
    std::vector<unsigned char> ppm;
    ppm.reserve((size_t)W * 2 * SCALE * H * SCALE * 3);
    for (int y = 0; y < H * SCALE; ++y)
        for (int x = 0; x < W * 2 * SCALE; ++x) {
            int px = x / SCALE, py = y / SCALE;
            bool diff_panel = px >= W;
            if (diff_panel)
                px -= W;
            int i = 4 * (py * W + px);
            int da = std::abs((int)got[i + 3] - (int)want[i + 3]);
            int dc = 0;
            for (int ch = 0; ch < 3; ++ch)
                dc = std::max(dc, std::abs((int)got[i + ch] - (int)want[i + ch]));
            max_alpha = std::max(max_alpha, da);
            max_color = std::max(max_color, dc);
            unsigned char rgb[3];
            if (!diff_panel) {
                rgb[0] = rgb[1] = rgb[2] = got[i + 3];
            } else {
                rgb[0] = (unsigned char)std::min(255, da * 32);
                rgb[1] = 0;
                rgb[2] = (unsigned char)std::min(255, dc * 8);
            }
            ppm.insert(ppm.end(), rgb, rgb + 3);
        }

    FILE *out = std::fopen(path.c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return 1;
    }
    std::fprintf(out, "P6\n%d %d\n255\n", W * 2 * SCALE, H * SCALE);
    std::fwrite(ppm.data(), 1, ppm.size(), out);
    std::fclose(out);
    std::printf("scene 'offscreen source with nested clip': max alpha diff %d, "
                "max color diff %d (bytes); wrote %s\n",
                max_alpha, max_color, path.c_str());
    return 0;
}
