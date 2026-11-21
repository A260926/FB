// example/async_pose.cpp - plays two characters of one IQM asset in
// parallel on the background pose service, checks that their poses differ,
// and prints ASYNC_POSE_DEMO_OK.  CPU-only: no GL, no display.
//
// The example builds a small IQM file programmatically (so it is fully
// self-contained), loads it for real through asyncpose::load_pose_asset,
// then steps both characters through the animation with a frame offset
// between them.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "demo/async_pose.h"
#include "tests/iqm_fixture.h"

namespace
{

bool poses_differ(const asyncpose::PoseSnapshot &a, const asyncpose::PoseSnapshot &b)
{
    if(a.joints.size() != b.joints.size() || a.positions.size() != b.positions.size())
        return true;
    for(size_t i = 0; i < a.positions.size(); i++)
        for(int k = 0; k < 3; k++)
            if(std::fabs(a.positions[i][k] - b.positions[i][k]) > 2e-5f) return true;
    for(size_t i = 0; i < a.joints.size(); i++)
        for(int k = 0; k < 4; k++)
            if(std::fabs(a.joints[i].a[k] - b.joints[i].a[k]) > 2e-5f ||
               std::fabs(a.joints[i].b[k] - b.joints[i].b[k]) > 2e-5f ||
               std::fabs(a.joints[i].c[k] - b.joints[i].c[k]) > 2e-5f) return true;
    return false;
}

} // namespace

int main()
{
    // Build a small animated model and write it to a temp file.
    fixture::BuiltRig built = fixture::build_rig(fixture::make_rig_alpha());
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if(ec) dir = ".";
    const std::string path = (dir / "async_pose_example.iqm").string();
    if(!fixture::write_file(path, built.filebytes))
    {
        std::fprintf(stderr, "failed to write %s\n", path.c_str());
        return 1;
    }

    // Really load the asset from disk, then play two characters in parallel.
    std::shared_ptr<const asyncpose::PoseAsset> asset = asyncpose::load_pose_asset(path);
    std::remove(path.c_str());
    if(!asset)
    {
        std::fprintf(stderr, "failed to load %s\n", path.c_str());
        return 1;
    }

    asyncpose::PoseService service(2);
    asyncpose::CharacterId first = service.create_character(asset);
    asyncpose::CharacterId second = service.create_character(asset);
    if(!first.valid() || !second.valid())
    {
        std::fprintf(stderr, "failed to create characters\n");
        return 1;
    }

    // Step both characters through the animation; the second runs 1.5
    // frames ahead, so their poses must differ at every step.
    const int steps = 8;
    int differing = 0;
    for(int step = 0; step < steps; step++)
    {
        const float fa = 0.25f * step;
        const float fb = 0.25f * step + 1.5f;
        asyncpose::Ticket ta = service.submit(first, fa);
        asyncpose::Ticket tb = service.submit(second, fb);
        if(!ta || !tb)
        {
            std::fprintf(stderr, "submit failed at step %d\n", step);
            return 1;
        }
        std::shared_ptr<const asyncpose::PoseSnapshot> snapa, snapb;
        if(service.wait(first, ta, &snapa) != asyncpose::RequestStatus::published || !snapa ||
           service.wait(second, tb, &snapb) != asyncpose::RequestStatus::published || !snapb)
        {
            std::fprintf(stderr, "wait failed at step %d\n", step);
            return 1;
        }
        if(poses_differ(*snapa, *snapb)) differing++;
    }

    service.unregister_character(first);
    service.unregister_character(second);
    service.shutdown();

    if(differing != steps)
    {
        std::fprintf(stderr, "poses unexpectedly identical at %d of %d steps\n", steps - differing, steps);
        return 1;
    }
    std::printf("ASYNC_POSE_DEMO_OK joints=%d frames=%d verts=%d steps=%d\n",
                asset->numjoints(), asset->numframes(), asset->numverts(), steps);
    return 0;
}
