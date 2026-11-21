/*
 * Texture source selection example.
 *
 * Builds a small procedural mesh of two overlapping quads plus two source
 * textures that are generated in memory, runs the production selection and
 * observer draw, and writes the resulting image as binary PPM (P6).
 *
 * Usage: view_texture <output.ppm>
 *
 * Source textures are y-up: pixel (x, y) with y = 0 at the bottom row.
 */

#include "rasterizer/view_texture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

Texture<Vec4> make_source_texture(int width, int height, Vec4 base, Vec4 accent)
{
    Texture<Vec4> texture(static_cast<unsigned int>(width), static_cast<unsigned int>(height));

    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; x++)
        {
            float u = static_cast<float>(x) / static_cast<float>(std::max(1, width - 1));
            float v = static_cast<float>(y) / static_cast<float>(std::max(1, height - 1));

            bool checker = ((x / 4) + (y / 4)) % 2 == 0;
            Vec4 color = checker ? base : accent;

            /* gentle gradient so bilinear sampling is visible */
            color = color * Vec4(0.6f + 0.4f * u, 0.6f + 0.4f * v, 1.0f, 1.0f);
            texture(x, y) = color;
        }
    }

    return texture;
}

bool write_ppm(const std::string& path, const Texture<RGBA8>& color)
{
    FILE* file = std::fopen(path.c_str(), "wb");
    if(file == nullptr) return false;

    std::fprintf(file, "P6\n%d %d\n255\n", color.width(), color.height());

    /* The framebuffer origin is bottom left (y up); PPM expects rows from the
     * top, so store the last row first. */
    for(int y = color.height() - 1; y >= 0; y--)
    {
        for(int x = 0; x < color.width(); x++)
        {
            unsigned char rgb[3] = { color(x, y).x, color(x, y).y, color(x, y).z };
            if(std::fwrite(rgb, 1, 3, file) != 3)
            {
                std::fclose(file);
                return false;
            }
        }
    }

    return std::fclose(file) == 0;
}

Mat4 look_down_ortho(float left, float right, float bottom, float top)
{
    return Mat4::ortho(left, bottom, right, top, -1.0f, 1.0f);
}

} /* namespace */

int main(int argc, char** argv)
{
    const std::string output = argc > 1 ? argv[1] : "view_texture.ppm";
    const int texture_size = 32;

    /* Source A looks down -z.  Source B looks from the opposite side by
     * mirroring z before the same orthographic projection, so the depth
     * ordering of the two quads is reversed between the two sources. */
    Mat4 source_matrix_a = look_down_ortho(-1.1f, 1.1f, -1.1f, 1.1f);
    Mat4 source_matrix_b = source_matrix_a * Mat4::scale(1.0f, 1.0f, -1.0f);

    Texture<Vec4> texture_a = make_source_texture(texture_size, texture_size,
                                                  Vec4(0.85f, 0.25f, 0.20f, 1.0f),
                                                  Vec4(0.20f, 0.35f, 0.80f, 1.0f));
    Texture<Vec4> texture_b = make_source_texture(texture_size, texture_size,
                                                  Vec4(0.20f, 0.75f, 0.30f, 1.0f),
                                                  Vec4(0.90f, 0.85f, 0.25f, 1.0f));

    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{ source_matrix_a, &texture_a },
        view_texture::SourceCamera{ source_matrix_b, &texture_b },
    };

    /*
     * Two partially overlapping quads at different depths.  From source A the
     * quad at z = +0.3 is nearer; from source B (mirrored z) the quad at
     * z = -0.3 is nearer.  Each quad therefore wins every pixel it covers in
     * exactly one source, while the other source occludes it, so the exact
     * cut assigns one quad to each source.
     */
    view_texture::Mesh mesh;
    mesh.positions = {
        /* quad seen cleanly by source A (nearer from A) */
        { -0.85f, -0.6f, 0.3f },
        {  0.35f, -0.6f, 0.3f },
        {  0.35f,  0.6f, 0.3f },
        { -0.85f,  0.6f, 0.3f },
        /* quad seen cleanly by source B (nearer from B) */
        { -0.35f, -0.6f, -0.3f },
        {  0.85f, -0.6f, -0.3f },
        {  0.85f,  0.6f, -0.3f },
        { -0.35f,  0.6f, -0.3f },
    };
    mesh.indices = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };

    const int width = 320;
    const int height = 320;

    view_texture::Selection selection = view_texture::select(mesh, sources, 6);

    if(!selection.valid)
    {
        std::fprintf(stderr, "selection failed: %s\n", selection.error.c_str());
        return 1;
    }

    Renderer renderer(width, height);
    renderer.options().culling = false;
    renderer.framebuffer().clear(RGBA8(16, 16, 24, 255));

    const view_texture::ObserverCamera observer{ look_down_ortho(-1.0f, 1.0f, -1.0f, 1.0f) };

    const bool ok = view_texture::draw(renderer, mesh, selection, sources, observer,
                                       Vec4(0.55f, 0.55f, 0.60f, 1.0f));

    if(!ok)
    {
        std::fprintf(stderr, "draw failed\n");
        return 1;
    }

    if(!write_ppm(output, renderer.framebuffer().color()))
    {
        std::fprintf(stderr, "could not write %s\n", output.c_str());
        return 1;
    }

    int labelled = 0;
    for(int label : selection.labels)
    {
        if(label >= 0) labelled++;
    }

    std::printf("wrote %s (%dx%d), labelled faces %d/%zu, energy %lld\n",
                output.c_str(), width, height, labelled, selection.labels.size(),
                selection.energy);

    return 0;
}
