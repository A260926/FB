//****************************************************************************//
// streamclip.cpp example                                                     //
// Offline-writes a blocked animation, releases the in-memory source, then    //
// drives two characters from a small shared block cache and skins them on    //
// the CPU.  All files live in a self-created temporary directory.            //
//****************************************************************************//

#include "cal3d/streamclip.h"
#include "cal3d/coreanimation.h"
#include "cal3d/coretrack.h"
#include "cal3d/corekeyframe.h"
#include "cal3d/coreskeleton.h"
#include "cal3d/corebone.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/animation.h"
#include "cal3d/mixer.h"
#include "cal3d/skeleton.h"
#include "cal3d/submesh.h"
#include "cal3d/physique.h"
#include "cal3d/quaternion.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

CalQuaternion rotZ(float angle) {
    CalQuaternion q;
    q.setAxisAngle(CalVector(0, 0, 1), angle);
    return q;
}

std::string makeTempDir() {
    const char* base = std::getenv("TMPDIR");
    std::string dir = (base ? base : ".");
    dir += "/streamclip_example_XXXXXX";
    std::vector<char> buffer(dir.begin(), dir.end());
    buffer.push_back('\0');
    if (!mkdtemp(&buffer[0])) {
        return std::string();
    }
    return std::string(&buffer[0]);
}

} // namespace

int main() {
    const std::string dir = makeTempDir();
    if (dir.empty()) {
        std::fprintf(stderr, "could not create temporary directory\n");
        return 1;
    }
    const std::string clipPath = dir + "/wave.csc";
    std::printf("writing stream clip to %s\n", clipPath.c_str());

    // ---- offline build of the source animation -----------------------------
    {
        ISTD::shared_ptr<CalCoreAnimation> wave = ISTD::make_shared<CalCoreAnimation>();
        wave->duration = 2.0f;

        CalCoreTrack::KeyframeList rootKeys;
        rootKeys.push_back(CalCoreKeyframe(0.0f, CalVector(0, 0, 0), rotZ(0.0f)));
        rootKeys.push_back(CalCoreKeyframe(0.5f, CalVector(1, 0, 0), rotZ(0.4f)));
        rootKeys.push_back(CalCoreKeyframe(1.0f, CalVector(2, 1, 0), rotZ(0.8f)));
        rootKeys.push_back(CalCoreKeyframe(1.5f, CalVector(1, 2, 0), rotZ(1.2f)));
        rootKeys.push_back(CalCoreKeyframe(2.0f, CalVector(0, 2, 1), rotZ(1.6f)));
        wave->tracks.push_back(CalCoreTrack(0, rootKeys));

        CalCoreTrack::KeyframeList childKeys;
        childKeys.push_back(CalCoreKeyframe(0.0f, CalVector(0, 1, 0), CalQuaternion()));
        childKeys.push_back(CalCoreKeyframe(2.0f, CalVector(0, 3, 0), rotZ(-0.6f)));
        wave->tracks.push_back(CalCoreTrack(1, childKeys));

        if (!cal3d::writeStreamClip(clipPath, *wave, 0.5f)) {
            std::fprintf(stderr, "writeStreamClip failed\n");
            return 2;
        }
        // the in-memory animation is released on scope exit; sampling later
        // depends only on the file and the directory
    }

    // ---- skeleton, mesh and shared small cache ------------------------------
    ISTD::shared_ptr<CalCoreSkeleton> coreSkeleton = ISTD::make_shared<CalCoreSkeleton>();
    coreSkeleton->addCoreBone(ISTD::make_shared<CalCoreBone>("root"));
    coreSkeleton->addCoreBone(ISTD::make_shared<CalCoreBone>("child", 0));
    coreSkeleton->coreBones[0]->relativeTransform.translation = CalVector(0, 0, 0);
    coreSkeleton->coreBones[1]->relativeTransform.translation = CalVector(0, 1, 0);
    coreSkeleton->coreBones[0]->inverseBindPoseTransform.translation = CalVector(-1, 0, 0);
    coreSkeleton->coreBones[1]->inverseBindPoseTransform.translation = CalVector(-1, -1, 0);

    ISTD::shared_ptr<CalCoreSubmesh> coreSubmesh = ISTD::make_shared<CalCoreSubmesh>(1, false, 0);
    CalCoreSubmesh::Vertex vertex;
    vertex.position = CalPoint4(0.4f, 0.6f, 0.0f);
    vertex.normal = CalVector4(0, 1, 0);
    std::vector<CalCoreSubmesh::Influence> influences;
    influences.push_back(CalCoreSubmesh::Influence(0, 0.25f, false));
    influences.push_back(CalCoreSubmesh::Influence(1, 0.75f, true));
    coreSubmesh->addVertex(vertex, CalColor32(0), influences);
    ISTD::shared_ptr<CalSubmesh> submesh = ISTD::make_shared<CalSubmesh>(coreSubmesh);

    cal3d::CalStreamClipPtr clip = cal3d::openStreamClip(clipPath);
    if (!clip) {
        std::fprintf(stderr, "openStreamClip failed\n");
        return 3;
    }
    std::printf("opened clip: %u blocks, %u tracks, block reads so far: %llu\n",
                clip->blockCount(), clip->trackCount(),
                static_cast<unsigned long long>(clip->readAttempts()));

    // capacity 2 while each frame may touch two different blocks
    cal3d::CalStreamBlockCache cache(2);

    struct Character {
        CalMixer mixer;
        CalSkeleton skeleton;
        ISTD::shared_ptr<CalAnimation> animation;
        explicit Character(const ISTD::shared_ptr<CalCoreSkeleton>& core,
                           const cal3d::CalStreamClipPtr& streamClip,
                           float weight, unsigned priority)
            : skeleton(core)
        {
            animation = ISTD::make_shared<CalAnimation>(ISTD::shared_ptr<CalCoreAnimation>(), weight, priority);
            cal3d::setAnimationStreamSource(*animation, streamClip);
            mixer.addAnimation(animation);
        }
    };

    Character first(coreSkeleton, clip, 1.0f, 0);
    Character second(coreSkeleton, clip, 0.75f, 1);

    const float frameTimes[][2] = {
        {0.1f, 1.9f}, {0.6f, 1.4f}, {1.1f, 0.9f}, {1.7f, 0.3f}, {0.2f, 1.6f}
    };

    for (size_t frame = 0; frame < sizeof(frameTimes) / sizeof(frameTimes[0]); ++frame) {
        first.animation->time = frameTimes[frame][0];
        second.animation->time = frameTimes[frame][1];

        if (!first.mixer.updateSkeletonStreamed(&first.skeleton, {}, {}, cache)
            || !second.mixer.updateSkeletonStreamed(&second.skeleton, {}, {}, cache)) {
            std::fprintf(stderr, "frame %zu failed to load blocks\n", frame);
            return 4;
        }

        CalVector4 skin[2];
        CalPhysique::calculateVerticesAndNormals(
            cal3d::pointerFromVector(first.skeleton.boneTransforms),
            submesh.get(),
            reinterpret_cast<float*>(skin));

        const cal3d::StreamCacheStats stats = cache.stats();
        std::printf(
            "frame %zu: times=(%.2f,%.2f) skinned=(%.4f,%.4f,%.4f) "
            "resident=%zu hits=%llu reads=%llu peak=%zu\n",
            frame, frameTimes[frame][0], frameTimes[frame][1],
            skin[0].x, skin[0].y, skin[0].z,
            stats.residentBlocks,
            static_cast<unsigned long long>(stats.hitCount),
            static_cast<unsigned long long>(stats.readAttempts),
            stats.decodePeak);
    }

    std::printf("clip physical block reads: %llu\n",
                static_cast<unsigned long long>(clip->readAttempts()));
    std::puts("STREAMCLIP_EXAMPLE_OK");
    return 0;
}
