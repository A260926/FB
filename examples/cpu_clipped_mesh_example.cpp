// Minimal C++ example for buildClippedMesh().
//
// It builds a small armature from inline DragonBones JSON, advances one
// animation frame, defines a convex mask range and prints every surviving
// clipped triangle. No window, texture or graphics adapter is involved.
//
// Build with the same flags as the headless test runner:
//   c++ -std=c++17 -O1 -IDragonBones/src -I3rdParty -I. -Itask_support \
//       examples/cpu_clipped_mesh_example.cpp $(find DragonBones/src -name '*.cpp') -o clip_example
#include <cstdio>
#include <string>
#include <vector>

#include "headless.h"
#include "dragonBones/armature/CpuClippedMesh.h"

using namespace dragonBones;

namespace
{
    // One unskinned quad mesh behind an empty mask marker slot.
    const char* kRigJson = R"JSON(
    {
      "version": "5.5", "compatibleVersion": "5.5", "name": "exampledata", "frameRate": 24,
      "armature": [{
        "name": "rig", "type": "Armature", "frameRate": 24,
        "bone": [{ "name": "root" }],
        "slot": [
          { "name": "mask", "parent": "root", "displayIndex": 0 },
          { "name": "mesh", "parent": "root", "displayIndex": 0 },
          { "name": "end", "parent": "root", "displayIndex": 0 }
        ],
        "skin": [{
          "name": "default",
          "slot": [{ "name": "mesh", "display": [{
            "name": "grid", "type": "mesh", "path": "material/grid.png",
            "vertices": [0, 0, 4, 0, 4, 4, 0, 4],
            "uvs": [0, 0, 1, 0, 1, 1, 0, 1],
            "triangles": [0, 1, 2, 0, 2, 3]
          }]}]
        }],
        "animation": [{
          "name": "slide", "duration": 24, "playTimes": 1,
          "bone": [{ "name": "root", "translateFrame": [
            { "duration": 12, "tweenEasing": 0, "x": 0, "y": 0 },
            { "duration": 12, "x": 4, "y": 0 }
          ]}]
        }]
      }]
    })JSON";
}

int main()
{
    task_support::Factory factory;
    if (!factory.parseDragonBonesData(kRigJson))
    {
        std::fprintf(stderr, "failed to parse armature data\n");
        return 1;
    }

    auto armature = factory.buildArmature("rig", "exampledata");
    if (armature == nullptr)
    {
        std::fprintf(stderr, "failed to build armature\n");
        return 1;
    }

    armature->getAnimation()->play("slide");
    armature->advanceTime(0.25f); // Frame 6: root translated to x = 2.

    // Square in the empty mask slot's local coordinates. The range covers
    // every slot drawn after "mask" up to and including "end".
    CpuMaskRange range;
    range.maskSlot = "mask";
    range.endSlot = "end";
    range.polygon = {
        { 1.0f, -1.0f }, { 5.0f, -1.0f }, { 5.0f, 5.0f }, { 1.0f, 5.0f }
    };

    std::vector<CpuSlotMesh> packages;
    if (!buildClippedMesh(*armature, { range }, packages))
    {
        std::fprintf(stderr, "buildClippedMesh rejected the frame\n");
        armature->dispose();
        factory.runtime.advanceTime(0.0f);
        return 1;
    }

    for (const auto& package : packages)
    {
        std::printf("slot=%s display=%s material=%s triangles=%zu\n",
            package.slotName.c_str(), package.displayName.c_str(),
            package.materialPath.c_str(), package.triangles.size());

        for (const auto& triangle : package.triangles)
        {
            std::printf("  source=%u winding=", triangle.sourceTriangle);
            for (const auto& vertex : triangle.vertices)
            {
                std::printf(" (%.3f,%.3f uv=%.3f,%.3f)", vertex.x, vertex.y, vertex.u, vertex.v);
            }
            std::printf("\n");
        }
    }

    // packages owns every copy; disposing the armature cannot invalidate it.
    armature->dispose();
    factory.runtime.advanceTime(0.0f);
    std::printf("cpu clipped mesh example finished with %zu slot package(s)\n", packages.size());
    return 0;
}
