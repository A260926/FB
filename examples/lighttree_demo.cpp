/*
 * Light-tree demo.
 *
 * Builds a Cornell box containing several lights of very different strength
 * and type, constructs the static light-tree snapshot, and renders the scene
 * with both bidirectional path tracing (BPT) and vertex connection and
 * merging (VCM).  A PFM image named lighttree.pfm is written into the
 * directory supplied on the command line and the rendering work counts are
 * printed.  Uses only the C++ standard library and the NO_OMP build path.
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include "src/scene.hxx"
#include "src/framebuffer.hxx"
#include "src/pathtracer.hxx"
#include "src/vertexcm.hxx"

int main(int argc, const char *argv[])
{
    const char *outDir = (argc > 1) ? argv[1] : ".";

    Scene scene;
    // Ceiling area lights + a strong point light + sun + background gives a
    // wide spread of intensities and all four light implementations.
    const uint mask =
        Scene::kLightCeiling | Scene::kLightPoint |
        Scene::kLightSun | Scene::kLightBackground |
        Scene::kBothSmallSpheres;
    scene.LoadCornellBox(Vec2i(64, 64), mask);
    scene.BuildSceneSphere();

    if(!scene.RebuildLightTree())
    {
        std::fprintf(stderr, "light tree build failed\n");
        return 2;
    }
    scene.SetUseLightTree(true);

    const LightTree &tree = scene.GetLightTree();
    const int pixels = scene.mCamera.mResolution.x *
        scene.mCamera.mResolution.y;
    const int iterations = 3;

    std::printf("lights=%d nodes=%d depth=%d pixels=%d iterations=%d\n",
        tree.LightCount(), tree.NodeCount(), tree.Depth(),
        pixels, iterations);

    Framebuffer accumulated;
    accumulated.Setup(scene.mCamera.mResolution);

    // BPT pass.
    {
        VertexCM bpt(scene, VertexCM::kBpt, 0.003f, 0.75f, 1234);
        bpt.mMaxPathLength = 6;
        for(int i = 0; i < iterations; ++i)
            bpt.RunIteration(i);
        Framebuffer fb;
        bpt.GetFramebuffer(fb);
        accumulated.Add(fb);
    }

    // VCM pass.
    {
        VertexCM vcm(scene, VertexCM::kVcm, 0.003f, 0.75f, 1234);
        vcm.mMaxPathLength = 6;
        for(int i = 0; i < iterations; ++i)
            vcm.RunIteration(i);
        Framebuffer fb;
        vcm.GetFramebuffer(fb);
        accumulated.Add(fb);
    }
    accumulated.Scale(0.5f);

    const std::string outPath = std::string(outDir) + "/lighttree.pfm";
    accumulated.SavePFM(outPath.c_str());

    std::printf("work bpt_iterations=%d vcm_iterations=%d total_samples=%d "
        "tree_sample_queries=%lld tree_pdf_queries=%lld "
        "max_sample_visits=%d max_pdf_visits=%d image=%s\n",
        iterations, iterations, 2 * pixels * iterations,
        tree.SampleQueryCount(), tree.PdfQueryCount(),
        tree.MaxSampleNodeAccesses(), tree.MaxPdfNodeAccesses(),
        outPath.c_str());
    std::printf("total_luminance=%.6g\n", accumulated.TotalLuminance());
    return 0;
}
