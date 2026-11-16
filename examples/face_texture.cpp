#include "renderer.h"
#include "sampler.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

/*
 * Per-face texture example.
 *
 * Two connected quads share one edge; each face carries an independent
 * procedural float image at its own resolution. The scene is drawn with
 * the production quad entry (Renderer::drawQuads) and sampled with the
 * production face_texture_lod routine. After writing the first PPM, one
 * face is replaced through the owning FaceMaterial, the new snapshot is
 * drawn, and the original (now superseded) snapshot is replayed to prove
 * that snapshots keep working independently of the live material.
 */

struct FaceVarying
{
    Vec4 position;
    Vec2 uv;
    float face;
    VARYING(position, uv, face);
};

struct FaceUniforms
{
    std::shared_ptr<const FaceSnapshot> snapshot;
    float lod = 0.0f;
};

static Vec4 clamp_color(const Vec4& color)
{
    return Vec4(std::clamp(color.x, 0.0f, 1.0f),
                std::clamp(color.y, 0.0f, 1.0f),
                std::clamp(color.z, 0.0f, 1.0f),
                1.0f);
}

static FaceImageSpec stripes(int width, int height, float phase, int face)
{
    FaceImageSpec image;
    image.width = width;
    image.height = height;
    image.texels.resize(static_cast<size_t>(width) * height);
    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; x++)
        {
            const float u = static_cast<float>(x) / width;
            const float v = static_cast<float>(y) / height;
            const float wave = 0.5f + 0.5f * std::sin(8.0f * (u + v) + phase);
            image.texels[static_cast<size_t>(y) * width + x] =
                clamp_color(Vec4(0.15f + 0.55f * u + 0.1f * face,
                                 0.2f * wave + 0.1f * v,
                                 0.25f + 0.55f * v,
                                 1.0f));
        }
    }
    return image;
}

static FaceImageSpec checker(int width, int height)
{
    FaceImageSpec image;
    image.width = width;
    image.height = height;
    image.texels.resize(static_cast<size_t>(width) * height);
    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; x++)
        {
            const bool dark = ((x / 2) + (y / 2)) % 2 == 0;
            image.texels[static_cast<size_t>(y) * width + x] =
                dark ? Vec4(0.08f, 0.12f, 0.35f, 1.0f) : Vec4(0.85f, 0.75f, 0.25f, 1.0f);
        }
    }
    return image;
}

static void write_ppm(Renderer& renderer, const std::string& path)
{
    const auto& color = renderer.framebuffer().color();
    FILE* file = std::fopen(path.c_str(), "wb");
    if(file == nullptr)
    {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(1);
    }
    std::fprintf(file, "P6\n%d %d\n255\n", color.width(), color.height());
    for(int y = color.height() - 1; y >= 0; y--) /* PPM rows bottom to top */
    {
        for(int x = 0; x < color.width(); x++)
        {
            const RGBA8& pixel = color(x, y);
            const unsigned char rgb[3] = { pixel.x, pixel.y, pixel.z };
            std::fwrite(rgb, 1, 3, file);
        }
    }
    std::fclose(file);
}

static std::unique_ptr<Renderer> draw_material(const FaceSnapshot& snapshot,
                                               const std::vector<Vec4>& positions,
                                               float lod, int size)
{
    auto renderer = std::make_unique<Renderer>(size, size);
    renderer->options().culling = false;

    Program<FaceMeshVertex, FaceVarying, FaceUniforms> program;
    program.onVertex([](const FaceUniforms&, const FaceMeshVertex& in, FaceVarying& out)
    {
        out.position = in.position;
        out.uv = in.uv;
        out.face = in.face;
    });
    program.onFragment([](const FaceUniforms& uniforms, const FaceVarying& in, Vec4& out)
    {
        const unsigned int face = static_cast<unsigned int>(in.face + 0.5f);
        out = face_texture_lod(*uniforms.snapshot, face, in.uv, uniforms.lod);
    });

    /* non-owning handle: the snapshot lifetime is managed by the caller */
    program.uniforms().snapshot = std::shared_ptr<const FaceSnapshot>(&snapshot, [](const FaceSnapshot*) {});
    program.uniforms().lod = lod;

    FaceQuadMesh mesh;
    mesh.snapshot = program.uniforms().snapshot;
    mesh.positions = positions;
    renderer->drawQuads(program, mesh);
    return renderer;
}

int main(int argc, char** argv)
{
    const std::string outputDir = (argc > 1) ? argv[1] : ".";

    /* two quads sharing edge (1,2) in opposite directions; rectangular,
     * independently sized float images */
    std::vector<FaceQuad> quads = {
        FaceQuad{{0, 1, 2, 3}},
        FaceQuad{{1, 4, 5, 2}},
    };
    std::vector<FaceImageSpec> images = {
        stripes(8, 4, 0.0f, 0),
        stripes(4, 8, 1.7f, 1),
    };

    FaceMaterial material(quads, 6, images);
    const std::shared_ptr<const FaceSnapshot> firstSnapshot = material.snapshot();

    const std::vector<Vec4> positions = {
        Vec4(-0.8f, -0.8f, 0.0f, 1.0f), Vec4(0.0f, -0.8f, 0.0f, 1.0f),
        Vec4( 0.0f,  0.8f, 0.0f, 1.0f), Vec4(-0.8f, 0.8f, 0.0f, 1.0f),
        Vec4( 0.8f, -0.8f, 0.0f, 1.0f), Vec4(0.8f, 0.8f, 0.0f, 1.0f),
    };

    auto before = draw_material(*firstSnapshot, positions, 0.0f, 96);
    write_ppm(*before, outputDir + "/face_texture_before.ppm");

    /* replace the base level of face 0 (and change its density) */
    const FaceImageSpec replacement = checker(4, 4);
    if(!material.replace_face(0, replacement.width, replacement.height, replacement.texels))
    {
        std::fprintf(stderr, "face replacement rejected\n");
        return 1;
    }
    std::printf("rebuilds after update: %llu\n", material.rebuild_count());

    auto after = draw_material(*material.snapshot(), positions, 0.0f, 96);
    write_ppm(*after, outputDir + "/face_texture_after.ppm");

    /* the old snapshot still renders the original procedural face even
     * though the material has moved on to a new snapshot */
    auto replayed = draw_material(*firstSnapshot, positions, 0.0f, 96);
    write_ppm(*replayed, outputDir + "/face_texture_old_snapshot.ppm");

    /* also exercise the cross-level filtering entry point */
    const FaceTexel filtered = face_texture_lod(*material.snapshot(), 1, Vec2(0.63f, 0.27f), 1.3f);
    std::printf("face 1 lod=1.3 sample: %.4f %.4f %.4f %.4f\n", filtered.x, filtered.y, filtered.z, filtered.w);
    std::printf("wrote %s/face_texture_{before,after,old_snapshot}.ppm\n", outputDir.c_str());
    return 0;
}
