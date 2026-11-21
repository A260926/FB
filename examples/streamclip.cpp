// Example: write an animation to a time-blocked file, release the original
// in-memory animation, then drive two characters through a small shared
// decoded-block cache and skin the result on the CPU.
//
// Everything is written under a self-created temporary directory; nothing is
// left behind.

#include <cal3d/coreanimation.h>
#include <cal3d/corebone.h>
#include <cal3d/corekeyframe.h>
#include <cal3d/coreskeleton.h>
#include <cal3d/coresubmesh.h>
#include <cal3d/coretrack.h>
#include <cal3d/mixer.h>
#include <cal3d/physique.h>
#include <cal3d/skeleton.h>
#include <cal3d/streamclip.h>
#include <cal3d/submesh.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

std::string makeTempDir() {
    std::string base;
    const char* tmp = std::getenv("TMPDIR");
    if (!tmp || !*tmp) {
        tmp = ".";
    }
    base = tmp;
    if (!base.empty() && base[base.size() - 1] == '/') {
        base.erase(base.size() - 1);
    }

    std::string pattern = base + "/cal3d_streamclip_example_XXXXXX";
    std::string buffer = pattern;
#if !defined(_WIN32)
    if (::mkdtemp(&buffer[0])) {
        return buffer;
    }
#endif
    // Fallback: a fixed-ish name unique per process.
    char fallback[512];
    std::snprintf(fallback, sizeof(fallback), "%s/cal3d_streamclip_example_%d",
                  base.c_str(), static_cast<int>(::getpid()));
#if !defined(_WIN32)
    ::mkdir(fallback, 0700);
#endif
    return fallback;
}

cal3d::RotateTranslate quatRT(float angle, const CalVector& axis, const CalVector& translation) {
    CalQuaternion q;
    q.setAxisAngle(axis, angle);
    return cal3d::RotateTranslate(q, translation);
}

ISTD::shared_ptr<CalCoreAnimation> buildAnimation() {
    ISTD::shared_ptr<CalCoreAnimation> animation = ISTD::make_shared<CalCoreAnimation>();
    animation->duration = 2.0f;

    CalCoreTrack::KeyframeList rootKeys;
    rootKeys.push_back(CalCoreKeyframe(0.0f, CalVector(0, 0, 0), CalQuaternion()));
    rootKeys.push_back(CalCoreKeyframe(0.6f, CalVector(0.5f, 0.1f, 0.0f),
                                       quatRT(0.7f, CalVector(0, 1, 0), CalVector()).rotation));
    rootKeys.push_back(CalCoreKeyframe(1.4f, CalVector(0.1f, 0.4f, 0.2f),
                                       quatRT(-0.4f, CalVector(1, 0, 1), CalVector()).rotation));
    rootKeys.push_back(CalCoreKeyframe(2.0f, CalVector(0, 0.2f, 0.0f),
                                       quatRT(0.2f, CalVector(0, 0, 1), CalVector()).rotation));

    CalCoreTrack::KeyframeList childKeys;
    childKeys.push_back(CalCoreKeyframe(0.0f, CalVector(0, 0, 0), CalQuaternion()));
    childKeys.push_back(CalCoreKeyframe(0.8f, CalVector(0.2f, 0.3f, 0.0f),
                                        quatRT(0.9f, CalVector(0, 0, 1), CalVector()).rotation));
    childKeys.push_back(CalCoreKeyframe(2.0f, CalVector(-0.2f, 0.1f, 0.15f),
                                        quatRT(-0.6f, CalVector(0, 1, 0), CalVector()).rotation));

    animation->tracks.push_back(CalCoreTrack(0, rootKeys));
    animation->tracks.push_back(CalCoreTrack(1, childKeys));
    return animation;
}

ISTD::shared_ptr<CalCoreSkeleton> buildSkeleton() {
    ISTD::shared_ptr<CalCoreSkeleton> core = ISTD::make_shared<CalCoreSkeleton>();
    ISTD::shared_ptr<CalCoreBone> root = ISTD::make_shared<CalCoreBone>("root");
    ISTD::shared_ptr<CalCoreBone> child = ISTD::make_shared<CalCoreBone>("child", 0);

    root->relativeTransform = cal3d::RotateTranslate();
    root->inverseBindPoseTransform.translation = CalVector(-0.1f, 0.0f, 0.0f);

    child->relativeTransform.translation = CalVector(0, 1.0f, 0);
    child->inverseBindPoseTransform.translation = CalVector(0.0f, -1.0f, 0.1f);

    core->addCoreBone(root);
    core->addCoreBone(child);
    return core;
}

ISTD::shared_ptr<CalCoreSubmesh> buildSubmesh() {
    ISTD::shared_ptr<CalCoreSubmesh> submesh = ISTD::make_shared<CalCoreSubmesh>(2, false, 0);

    CalCoreSubmesh::Vertex a;
    a.position = CalPoint4(1.0f, 0.5f, 0.0f);
    a.normal = CalVector4(0.0f, 1.0f, 0.0f);
    std::vector<CalCoreSubmesh::Influence> infA;
    infA.push_back(CalCoreSubmesh::Influence(0, 1.0f, true));
    submesh->addVertex(a, CalColor32(0), infA);

    CalCoreSubmesh::Vertex b;
    b.position = CalPoint4(-0.5f, 0.25f, 0.5f);
    b.normal = CalVector4(1.0f, 0.0f, 0.0f);
    std::vector<CalCoreSubmesh::Influence> infB;
    infB.push_back(CalCoreSubmesh::Influence(0, 0.25f, false));
    infB.push_back(CalCoreSubmesh::Influence(1, 0.75f, true));
    submesh->addVertex(b, CalColor32(0), infB);

    return submesh;
}

} // namespace

int main() {
    const std::string dir = makeTempDir();
    const std::string clipPath = dir + "/walk.clip";

    // 1. Write the clip, then drop the only reference to the in-memory
    //    animation.  The streaming clip keeps only its directory and handle.
    {
        ISTD::shared_ptr<CalCoreAnimation> animation = buildAnimation();
        std::string error;
        if (!writeStreamClip(*animation, 0.5f, clipPath, &error)) {
            std::fprintf(stderr, "write failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("WROTE %s duration=%.2f block=%.2f\n",
                    clipPath.c_str(), animation->duration, 0.5f);
    }

    ISTD::shared_ptr<CalStreamClip> clip = CalStreamClip::open(clipPath);
    if (!clip) {
        std::fprintf(stderr, "open failed\n");
        return 1;
    }
    std::printf("OPENED blocks=%u tracks=%u identity=%llu\n",
                clip->blockCount(),
                static_cast<unsigned>(clip->tracks().size()),
                static_cast<unsigned long long>(clip->identity()));

    ISTD::shared_ptr<CalCoreSkeleton> core = buildSkeleton();
    ISTD::shared_ptr<CalCoreSubmesh> coreSubmesh = buildSubmesh();

    CalSkeleton skeletonA(core);
    CalSkeleton skeletonB(core);
    CalMixer mixerA;
    CalMixer mixerB;

    // Two characters share one small cache (only two blocks resident).
    CalStreamClipCache cache(2);

    ISTD::shared_ptr<CalAnimation> animA = ISTD::make_shared<CalAnimation>(clip, 1.0f, 0);
    ISTD::shared_ptr<CalAnimation> animB = ISTD::make_shared<CalAnimation>(clip, 1.0f, 0);
    mixerA.addAnimation(animA);
    mixerB.addAnimation(animB);

    const float timesA[] = { 0.1f, 0.7f, 1.3f, 1.9f, 0.3f };
    const float timesB[] = { 1.1f, 0.2f, 1.6f, 0.4f, 1.2f };

    for (int frame = 0; frame < 5; ++frame) {
        animA->time = timesA[frame];
        animB->time = timesB[frame];

        if (!mixerA.updateSkeletonStreamed(&skeletonA, {}, {}, &cache)
            || !mixerB.updateSkeletonStreamed(&skeletonB, {}, {}, &cache)) {
            std::fprintf(stderr, "frame %d failed: %s\n", frame, cache.lastError().c_str());
            return 1;
        }

        CalSubmesh meshA(coreSubmesh);
        CalSubmesh meshB(coreSubmesh);
        CalVector4 outA[4];
        CalVector4 outB[4];
        CalPhysique::calculateVerticesAndNormals(
            cal3d::pointerFromVector(skeletonA.boneTransforms),
            &meshA,
            reinterpret_cast<float*>(outA));
        CalPhysique::calculateVerticesAndNormals(
            cal3d::pointerFromVector(skeletonB.boneTransforms),
            &meshB,
            reinterpret_cast<float*>(outB));

        std::printf(
            "FRAME %d A(t=%.2f) bone0=(%.3f,%.3f,%.3f) v1=(%.3f,%.3f,%.3f)"
            " B(t=%.2f) v0=(%.3f,%.3f,%.3f)"
            " resident=%u reads=%llu hits=%llu peak=%u\n",
            frame,
            animA->time,
            skeletonA.bones[0].absoluteTransform.translation.x,
            skeletonA.bones[0].absoluteTransform.translation.y,
            skeletonA.bones[0].absoluteTransform.translation.z,
            outA[2].x, outA[2].y, outA[2].z,
            animB->time,
            outB[0].x, outB[0].y, outB[0].z,
            static_cast<unsigned>(cache.residentCount()),
            static_cast<unsigned long long>(cache.readAttemptCount()),
            static_cast<unsigned long long>(cache.hitCount()),
            static_cast<unsigned>(cache.peakDecodedBlocks()));
    }

    std::printf("CACHE resident=%u reads=%llu hits=%llu peak=%u capacity=%u\n",
                static_cast<unsigned>(cache.residentCount()),
                static_cast<unsigned long long>(cache.readAttemptCount()),
                static_cast<unsigned long long>(cache.hitCount()),
                static_cast<unsigned>(cache.peakDecodedBlocks()),
                static_cast<unsigned>(cache.capacity()));

    std::printf("LRU");
    std::vector<CalStreamClipBlockKey> order = cache.lruOrder();
    for (size_t i = 0; i < order.size(); ++i) {
        std::printf(" [%llu:%u]",
                    static_cast<unsigned long long>(order[i].sourceId),
                    order[i].blockIndex);
    }
    std::printf("\n");

    // Clean up the temporary directory we created.
    std::remove(clipPath.c_str());
#if !defined(_WIN32)
    ::rmdir(dir.c_str());
#endif
    std::puts("EXAMPLE_COMPLETE");
    return 0;
}
