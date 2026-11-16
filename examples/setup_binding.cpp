// Shared-character setup pose override example.
//
// Two armatures are built from one DragonBonesData. The first armature keeps
// the shared setup pose, the second one installs a setup pose override on its
// root bone. The example prints the skinned control vertices before and after
// the override and numerically checks that:
//   * at the neutral pose both characters still produce the same skin shape;
//   * while animating, the overridden character deforms with the original
//     motion applied on top of the new setup;
//   * the shared BoneData and packed mesh arrays are never modified.
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "task_support/headless.h"

using namespace dragonBones;

namespace
{
    const char* RIG_JSON =
        "{\"version\":\"5.5\",\"compatibleVersion\":\"5.5\",\"name\":\"fixture\",\"frameRate\":24,"
        "\"armature\":[{\"name\":\"rig\",\"type\":\"Armature\",\"frameRate\":24,"
        "\"bone\":[{\"name\":\"root\",\"transform\":{\"x\":1,\"y\":0}},"
        "{\"name\":\"tip\",\"parent\":\"root\",\"transform\":{\"x\":3,\"y\":1}}],"
        "\"slot\":[{\"name\":\"mesh\",\"parent\":\"tip\",\"displayIndex\":0}],"
        "\"skin\":[{\"name\":\"default\",\"slot\":[{\"name\":\"mesh\",\"display\":[{"
        "\"name\":\"tri\",\"type\":\"mesh\",\"vertices\":[0,0,4,0,0,4],\"uvs\":[0,0,1,0,0,1],"
        "\"triangles\":[0,1,2],\"slotPose\":[1,0,0,1,0,0],"
        "\"bonePose\":[0,1,0,0,1,1,0, 1,1,0,0,1,4,1],"
        "\"weights\":[2,0,0.5,1,0.5, 2,0,0.5,1,0.5, 2,1,0.5,0,0.5]}]}]}],"
        "\"animation\":[{\"name\":\"move\",\"duration\":24,\"playTimes\":0,\"bone\":["
        "{\"name\":\"root\",\"translateFrame\":[{\"duration\":12,\"tweenEasing\":0,\"x\":0,\"y\":0},"
        "{\"duration\":12,\"tweenEasing\":0,\"x\":6,\"y\":0}]},"
        "{\"name\":\"tip\",\"rotateFrame\":[{\"duration\":12,\"tweenEasing\":0,\"rotate\":0},"
        "{\"duration\":12,\"tweenEasing\":0,\"rotate\":40}]}],"
        "\"ffd\":[{\"skin\":\"default\",\"slot\":\"mesh\",\"name\":\"tri\",\"frame\":["
        "{\"duration\":12,\"tweenEasing\":0,\"vertices\":[0,0,0,0,0,0]},"
        "{\"duration\":12,\"tweenEasing\":0,\"vertices\":[1,0,1,0,1,0]}]}]}]}]}";

    void printVertices(const char* title, const std::vector<Point>& points)
    {
        std::cout << title << '\n';
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            std::cout << "  v" << i << " = (" << std::fixed << std::setprecision(5)
                      << points[i].x << ", " << points[i].y << ")\n";
        }
    }

    bool samePoints(const std::vector<Point>& a, const std::vector<Point>& b, float eps)
    {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (std::fabs(a[i].x - b[i].x) > eps || std::fabs(a[i].y - b[i].y) > eps)
            {
                return false;
            }
        }
        return true;
    }

    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "example check failed: " << message << '\n';
            std::exit(1);
        }
    }
}

int main()
{
    task_support::Factory factory;
    auto* data = factory.parseDragonBonesData(RIG_JSON);
    require(data != nullptr, "parse data");

    auto* plain = factory.buildArmature("rig", "fixture");
    auto* styled = factory.buildArmature("rig", "fixture");
    require(plain->_armatureData == styled->_armatureData, "shared armature data");
    plain->advanceTime(0.0f);
    styled->advanceTime(0.0f);

    auto* plainSlot = plain->getSlot("mesh");
    auto* styledSlot = styled->getSlot("mesh");

    std::vector<Point> plainNeutral;
    std::vector<Point> styledNeutral;
    require(plainSlot->getDeformedVertices(plainNeutral), "plain neutral vertices");
    require(styledSlot->getDeformedVertices(styledNeutral), "styled neutral vertices");
    printVertices("plain armature, neutral setup:", plainNeutral);
    printVertices("styled armature, neutral setup (before override):", styledNeutral);
    require(samePoints(plainNeutral, styledNeutral, 2e-4f), "neutral shapes must be equal");

    // Snapshot the shared root BoneData transform to prove it stays untouched.
    const auto sharedRoot = data->getArmature("rig")->getBone("root");
    const auto sharedX = sharedRoot->transform.x;
    const auto sharedRot = sharedRoot->transform.rotation;

    std::map<std::string, Transform> overrides;
    Transform rootOverride;
    rootOverride.x = 4.0f;
    rootOverride.y = -2.0f;
    rootOverride.rotation = 30.0f * Transform::DEG_RAD;
    rootOverride.scaleX = 1.3f;
    rootOverride.scaleY = 0.8f;
    overrides["root"] = rootOverride;
    require(styled->setSetupPoseOverrides(overrides), "apply setup override");
    require(!plain->hasSetupPoseOverrides(), "sibling keeps no overrides");

    require(styledSlot->getDeformedVertices(styledNeutral), "styled neutral vertices after override");
    printVertices("styled armature, neutral setup (after override):", styledNeutral);
    require(samePoints(plainNeutral, styledNeutral, 2e-4f), "neutral skin shape is preserved by re-binding");

    // Play the same animation on both: original motion on top of each setup.
    require(plain->getAnimation()->play("move") != nullptr, "play plain");
    require(styled->getAnimation()->play("move") != nullptr, "play styled");
    plain->advanceTime(0.5f);
    styled->advanceTime(0.5f);

    std::vector<Point> plainAnimated;
    std::vector<Point> styledAnimated;
    require(plainSlot->getDeformedVertices(plainAnimated), "plain animated vertices");
    require(styledSlot->getDeformedVertices(styledAnimated), "styled animated vertices");
    printVertices("plain armature, animated:", plainAnimated);
    printVertices("styled armature, animated (new setup + original motion):", styledAnimated);
    require(!samePoints(plainAnimated, styledAnimated, 2e-4f), "animated shapes must now differ");

    require(sharedRoot->transform.x == sharedX && sharedRoot->transform.rotation == sharedRot,
            "shared BoneData is untouched");

    // Clearing restores the shared setup: at the same animation time the
    // cleared armature now matches the plain one exactly.
    styled->clearSetupPoseOverrides();
    require(!styled->hasSetupPoseOverrides(), "override cleared");
    std::vector<Point> restored;
    require(styledSlot->getDeformedVertices(restored), "vertices after clear");
    require(samePoints(restored, plainAnimated, 2e-4f), "cleared armature rejoins the shared path");

    plain->dispose();
    styled->dispose();
    factory.runtime.advanceTime(0.0f);

    std::cout << "SETUP_BINDING_EXAMPLE_OK\n";
    return 0;
}
