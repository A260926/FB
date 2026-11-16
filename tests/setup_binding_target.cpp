// Target tests for per-instance setup pose overrides and mesh re-binding.
// No main(): task_support/driver.c provides it and links task_cases.
#include "task_support/check.h"
#include "task_support/headless.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace dragonBones;

namespace {

constexpr float EPS = 2e-4f;

struct V2 { float x, y; };
struct M2
{
    float a, b, c, d, tx, ty;
};

M2 mIdentity() { return M2{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f }; }

M2 mFromTransform(const Transform& t)
{
    Transform copy = t;
    Matrix m;
    copy.toMatrix(m);
    return M2{ m.a, m.b, m.c, m.d, m.tx, m.ty };
}

// Same multiplication order as Matrix::concat: result = A * B.
M2 mMul(const M2& A, const M2& B)
{
    return M2{
        A.a * B.a + A.b * B.c,
        A.a * B.b + A.b * B.d,
        A.c * B.a + A.d * B.c,
        A.c * B.b + A.d * B.d,
        B.a * A.tx + B.c * A.ty + B.tx,
        B.b * A.tx + B.d * A.ty + B.ty
    };
}

M2 mInverse(const M2& A)
{
    const auto n = A.a * A.d - A.b * A.c;
    return M2{
        A.d / n, -A.b / n, -A.c / n, A.a / n,
        (A.c * A.ty - A.d * A.tx) / n,
        -(A.a * A.ty - A.b * A.tx) / n
    };
}

V2 mPoint(const M2& m, const V2& p)
{
    return V2{ m.a * p.x + m.c * p.y + m.tx, m.b * p.x + m.d * p.y + m.ty };
}

V2 mDelta(const M2& m, const V2& p)
{
    return V2{ m.a * p.x + m.c * p.y, m.b * p.x + m.d * p.y };
}

bool nearF(float a, float b)
{
    return std::fabs(a - b) <= EPS;
}

bool nearV(const V2& a, const V2& b)
{
    return nearF(a.x, b.x) && nearF(a.y, b.y);
}

std::string fmt(float v)
{
    std::ostringstream os;
    os.precision(8);
    os << v;
    return os.str();
}

// Independent double precision matrix used only to emit bonePose arrays.
struct D2
{
    double a, b, c, d, tx, ty;
};

D2 dLocal(double x, double y, double rotDeg, double skewDeg, double sx, double sy)
{
    const auto r = rotDeg * 3.14159265358979323846 / 180.0;
    const auto k = skewDeg * 3.14159265358979323846 / 180.0;
    return D2{
        std::cos(r) * sx,
        std::sin(r) * sx,
        -std::sin(k + r) * sy,
        std::cos(k + r) * sy,
        x, y
    };
}

D2 dMul(const D2& A, const D2& B)
{
    return D2{
        A.a * B.a + A.b * B.c,
        A.a * B.b + A.b * B.d,
        A.c * B.a + A.d * B.c,
        A.c * B.b + A.d * B.d,
        B.a * A.tx + B.c * A.ty + B.tx,
        B.b * A.tx + B.d * A.ty + B.ty
    };
}

std::string dArray(const D2& m)
{
    std::ostringstream os;
    os.precision(10);
    os << m.a << ',' << m.b << ',' << m.c << ',' << m.d << ',' << m.tx << ',' << m.ty;
    return os.str();
}

// Program generated armature: three-bone chain with rotation, skew and
// non-uniform scaling, two skinned mesh displays (two-bone weights each), one
// unskinned mesh with a non-identity display transform, FFD and two animations.
std::string makeRigJson(const D2& rootW, const D2& midW, const D2& tipW)
{
    const auto slotPose = "0.965925826,-0.258819045,0.258819045,0.965925826,1.5,-0.7";

    std::ostringstream os;
    os << "{\"version\":\"5.5\",\"compatibleVersion\":\"5.5\",\"name\":\"fixture\",\"frameRate\":24,\"armature\":[{";
    os << "\"name\":\"rig\",\"type\":\"Armature\",\"frameRate\":24,";
    os << "\"bone\":[";
    os << "{\"name\":\"root\",\"transform\":{\"x\":1,\"y\":2,\"rotate\":20,\"scaleX\":1.2,\"scaleY\":0.9}},";
    os << "{\"name\":\"mid\",\"parent\":\"root\",\"transform\":{\"x\":3,\"y\":-1,\"rotate\":-15,\"skew\":10,\"scaleX\":0.8,\"scaleY\":1.3}},";
    os << "{\"name\":\"tip\",\"parent\":\"mid\",\"transform\":{\"x\":2,\"y\":0,\"rotate\":35,\"scaleX\":1.1,\"scaleY\":1}}],";
    os << "\"slot\":[{\"name\":\"slotA\",\"parent\":\"tip\",\"displayIndex\":0},{\"name\":\"slotB\",\"parent\":\"mid\",\"displayIndex\":0},{\"name\":\"slotC\",\"parent\":\"root\",\"displayIndex\":0}],";
    os << "\"skin\":[{\"name\":\"default\",\"slot\":[";
    // Slot A: two skinned mesh displays sharing the same bind poses.
    os << "{\"name\":\"slotA\",\"display\":[";
    os << "{\"name\":\"triA\",\"type\":\"mesh\","
       << "\"vertices\":[0,0,4,0,4,3,0,3],\"uvs\":[0,0,1,0,1,1,0,1],\"triangles\":[0,1,2,0,2,3],"
       << "\"slotPose\":[" << slotPose << "],"
       << "\"bonePose\":[0," << dArray(rootW) << ",1," << dArray(midW) << ",2," << dArray(tipW) << "],"
       << "\"weights\":[2,0,0.5,1,0.5, 2,1,0.5,2,0.5, 2,2,0.5,0,0.5, 2,0,0.5,2,0.5]},";
    os << "{\"name\":\"triB\",\"type\":\"mesh\","
       << "\"vertices\":[1,1,5,2,2,4],\"uvs\":[0,0,1,0,0,1],\"triangles\":[0,1,2],"
       << "\"slotPose\":[" << slotPose << "],"
       << "\"bonePose\":[0," << dArray(rootW) << ",1," << dArray(midW) << ",2," << dArray(tipW) << "],"
       << "\"weights\":[2,0,0.5,2,0.5, 2,1,0.6,2,0.4, 2,0,0.7,1,0.3]}]},";
    // Slot B: one unskinned mesh with a non-identity slot display transform.
    os << "{\"name\":\"slotB\",\"display\":[{"
       << "\"name\":\"plain\",\"type\":\"mesh\","
       << "\"vertices\":[0,0,2,0,0,2],\"uvs\":[0,0,1,0,0,1],\"triangles\":[0,1,2],"
       << "\"transform\":{\"x\":1,\"y\":1,\"rotate\":25,\"scaleX\":1.5,\"scaleY\":0.7}}]},";
    // Slot C: a non-mesh image display, which must not expose mesh vertices.
    os << "{\"name\":\"slotC\",\"display\":[{"
       << "\"name\":\"img\",\"type\":\"image\",\"transform\":{\"x\":0.5,\"y\":0.5}}]}]}],";
    os << "\"animation\":[";
    os << "{\"name\":\"move\",\"duration\":24,\"playTimes\":0,\"bone\":["
       << "{\"name\":\"root\",\"translateFrame\":[{\"duration\":12,\"tweenEasing\":0,\"x\":0,\"y\":0},{\"duration\":12,\"tweenEasing\":0,\"x\":5,\"y\":0}]},"
       << "{\"name\":\"tip\",\"rotateFrame\":[{\"duration\":12,\"tweenEasing\":0,\"rotate\":0},{\"duration\":12,\"tweenEasing\":0,\"rotate\":40}]}],"
       << "\"ffd\":[{\"skin\":\"default\",\"slot\":\"slotA\",\"name\":\"triA\",\"frame\":["
       << "{\"duration\":12,\"tweenEasing\":0,\"vertices\":[0,0,0,0,0,0,0,0]},"
       << "{\"duration\":12,\"tweenEasing\":0,\"vertices\":[0.3,-0.2,0.3,-0.2,0.3,-0.2,0.3,-0.2]}]}]},";
    os << "{\"name\":\"mix\",\"duration\":24,\"playTimes\":1,\"bone\":["
       << "{\"name\":\"mid\",\"translateFrame\":[{\"duration\":12,\"tweenEasing\":0,\"x\":0,\"y\":0},{\"duration\":12,\"tweenEasing\":0,\"x\":0,\"y\":6}]}]}";
    os << "]}]}";
    return os.str();
}

struct Rig
{
    task_support::Factory factory;
    DragonBonesData* data = nullptr;
    Armature* armA = nullptr;
    Armature* armB = nullptr;
    std::string json;

    void build()
    {
        const auto rootL = dLocal(1, 2, 20, 0, 1.2, 0.9);
        const auto midL = dLocal(3, -1, -15, 10, 0.8, 1.3);
        const auto tipL = dLocal(2, 0, 35, 0, 1.1, 1.0);
        const auto rootW = rootL;
        const auto midW = dMul(midL, rootW);
        const auto tipW = dMul(tipL, midW);
        json = makeRigJson(rootW, midW, tipW);

        data = factory.parseDragonBonesData(json.c_str());
        armA = factory.buildArmature("rig", "fixture");
        armB = factory.buildArmature("rig", "fixture");
        armA->advanceTime(0.0f);
        armB->advanceTime(0.0f);
    }

    void dispose()
    {
        if (armA != nullptr) { armA->dispose(); armA = nullptr; }
        if (armB != nullptr) { armB->dispose(); armB = nullptr; }
        factory.runtime.advanceTime(0.0f);
    }
};

// Pure setup world matrix of a bone, original (B0) or overridden (B1).
M2 setupWorld(const ArmatureData* ad, const std::string& name, const std::map<std::string, Transform>& overrides)
{
    const auto boneData = ad->getBone(name);
    Transform origin = boneData->transform;
    const auto it = overrides.find(name);
    if (it != overrides.end())
    {
        origin = it->second;
    }

    const auto local = mFromTransform(origin);
    if (boneData->parent == nullptr)
    {
        return local;
    }
    return mMul(local, setupWorld(ad, boneData->parent->name, overrides));
}

// Current expected bone world matrix using the live offset/animationPose and
// the same Additive combination rule as Bone::_updateGlobalTransformMatrix().
M2 expectedBoneWorld(Armature* arm, const std::string& name, const std::map<std::string, Transform>& overrides)
{
    const auto bone = arm->getBone(name);
    const auto boneData = bone->_boneData;
    const Transform* origin = &boneData->transform;
    const auto it = overrides.find(name);
    if (it != overrides.end())
    {
        origin = &it->second;
    }

    Transform local;
    local.x = origin->x + bone->offset.x + bone->animationPose.x;
    local.y = origin->y + bone->offset.y + bone->animationPose.y;
    local.skew = origin->skew + bone->offset.skew + bone->animationPose.skew;
    local.rotation = origin->rotation + bone->offset.rotation + bone->animationPose.rotation;
    local.scaleX = origin->scaleX * bone->offset.scaleX * bone->animationPose.scaleX;
    local.scaleY = origin->scaleY * bone->offset.scaleY * bone->animationPose.scaleY;

    const auto localMatrix = mFromTransform(local);
    if (boneData->parent == nullptr)
    {
        return localMatrix;
    }
    return mMul(localMatrix, expectedBoneWorld(arm, boneData->parent->name, overrides));
}

// Replicate the packed weight layout consumed by Slot::getDeformedVertices.
struct Influence { unsigned boneIndex; float weight; V2 point; };
struct SkinVertex { std::vector<Influence> influences; };

std::vector<SkinVertex> readSkinLayout(Slot* slot)
{
    const auto vd = slot->_deformVertices->verticesData;
    const auto wd = vd->weight;
    const auto data = vd->data;
    const auto intArray = data->intArray;
    const auto floatArray = data->floatArray;
    const auto vertexCount = (std::size_t)intArray[vd->offset + (unsigned)BinaryOffset::MeshVertexCount];
    int weightFloatOffset = intArray[wd->offset + (unsigned)BinaryOffset::WeigthFloatOffset];
    if (weightFloatOffset < 0) weightFloatOffset += 65536;

    std::vector<SkinVertex> result(vertexCount);
    std::size_t iB = wd->offset + (unsigned)BinaryOffset::WeigthBoneIndices + slot->_deformVertices->bones.size();
    std::size_t iV = (std::size_t)weightFloatOffset;
    const auto scale = slot->_armature->_armatureData->scale;
    for (std::size_t i = 0; i < vertexCount; ++i)
    {
        const auto boneCount = (std::size_t)intArray[iB++];
        for (std::size_t j = 0; j < boneCount; ++j)
        {
            Influence inf;
            inf.boneIndex = (unsigned)intArray[iB++];
            inf.weight = floatArray[iV++];
            inf.point = V2{ floatArray[iV++] * scale, floatArray[iV++] * scale };
            result[i].influences.push_back(inf);
        }
    }
    return result;
}

std::vector<V2> readPackedMeshPoints(Slot* slot)
{
    const auto vd = slot->_deformVertices->verticesData;
    const auto data = vd->data;
    const auto intArray = data->intArray;
    const auto floatArray = data->floatArray;
    const auto vertexCount = (std::size_t)intArray[vd->offset + (unsigned)BinaryOffset::MeshVertexCount];
    int vertexOffset = intArray[vd->offset + (unsigned)BinaryOffset::MeshFloatOffset];
    if (vertexOffset < 0) vertexOffset += 65536;
    const auto scale = slot->_armature->_armatureData->scale;

    std::vector<V2> result(vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i)
    {
        result[i] = V2{ floatArray[vertexOffset + i * 2] * scale, floatArray[vertexOffset + i * 2 + 1] * scale };
    }
    return result;
}

// Independent expectation for the current slot, mirroring
// W * inverse(B1) * B0 * (p + d) with the translation kept off the direction.
std::vector<V2> expectedVertices(Armature* arm, Slot* slot, const std::map<std::string, Transform>& overrides)
{
    const auto& bones = slot->_deformVertices->bones;
    const auto& deform = slot->_deformVertices->vertices;
    const auto hasFFD = !deform.empty();
    std::vector<V2> result;

    if (slot->_deformVertices->verticesData->weight != nullptr)
    {
        const auto layout = readSkinLayout(slot);
        result.resize(layout.size());
        std::size_t iD = 0;
        for (std::size_t i = 0; i < layout.size(); ++i)
        {
            V2 acc{ 0.0f, 0.0f };
            for (const auto& inf : layout[i].influences)
            {
                const auto bone = bones[inf.boneIndex];
                const auto b0 = setupWorld(arm->_armatureData, bone->getName(), {});
                const auto b1 = setupWorld(arm->_armatureData, bone->getName(), overrides);
                // Row-vector convention of the engine: R = B0 · inverse(B1).
                const auto r = mMul(b0, mInverse(b1));
                const auto rd = r;
                const auto linearOnly = M2{ rd.a, rd.b, rd.c, rd.d, 0.0f, 0.0f };

                V2 local = mPoint(r, inf.point);
                if (hasFFD)
                {
                    const V2 d{ deform[iD * 2], deform[iD * 2 + 1] };
                    const auto rp = mPoint(r, inf.point);
                    const auto ld = mDelta(linearOnly, d);
                    local = V2{ rp.x + ld.x, rp.y + ld.y };
                    ++iD;
                }

                const auto w = expectedBoneWorld(arm, bone->getName(), overrides);
                const auto world = mPoint(w, local);
                acc.x += world.x * inf.weight;
                acc.y += world.y * inf.weight;
            }
            result[i] = acc;
        }
    }
    else
    {
        const auto points = readPackedMeshPoints(slot);
        result.resize(points.size());
        const auto gm = slot->globalTransformMatrix;
        const M2 slotMatrix{ gm.a, gm.b, gm.c, gm.d, gm.tx, gm.ty };
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            V2 local = points[i];
            if (hasFFD)
            {
                local.x += deform[i * 2];
                local.y += deform[i * 2 + 1];
            }
            result[i] = mPoint(slotMatrix, local);
        }
    }
    return result;
}

std::vector<V2> collect(Slot* slot)
{
    std::vector<Point> points;
    if (!slot->getDeformedVertices(points))
    {
        return {};
    }
    std::vector<V2> out;
    out.reserve(points.size());
    for (const auto& p : points) out.push_back(V2{ p.x, p.y });
    return out;
}

bool matchVertices(const std::vector<V2>& got, const std::vector<V2>& want)
{
    if (got.size() != want.size()) return false;
    for (std::size_t i = 0; i < got.size(); ++i)
    {
        if (!nearV(got[i], want[i])) return false;
    }
    return true;
}

Transform tr(float x, float y, float rotDeg, float skewDeg, float sx, float sy)
{
    Transform t;
    t.x = x;
    t.y = y;
    t.rotation = rotDeg * Transform::DEG_RAD;
    t.skew = skewDeg * Transform::DEG_RAD;
    t.scaleX = sx;
    t.scaleY = sy;
    return t;
}

bool checkSharedPackedUnchanged(DragonBonesData* data, const std::vector<float>& packedSnapshot, const std::vector<int16_t>& intSnapshot,
                                std::size_t floatCount, std::size_t intCount)
{
    for (std::size_t i = 0; i < floatCount; ++i)
    {
        if (data->floatArray[i] != packedSnapshot[i]) return false;
    }
    for (std::size_t i = 0; i < intCount; ++i)
    {
        if (data->intArray[i] != intSnapshot[i]) return false;
    }
    return true;
}

std::size_t packedFloatSpan(DragonBonesData* data, Armature* arm)
{
    std::size_t span = 0;
    for (const auto slot : arm->getSlots())
    {
        const auto dv = slot->_deformVertices;
        if (dv == nullptr || dv->verticesData == nullptr) continue;
        const auto vd = dv->verticesData;
        const auto vertexCount = (std::size_t)data->intArray[vd->offset + (unsigned)BinaryOffset::MeshVertexCount];
        int vertexOffset = data->intArray[vd->offset + (unsigned)BinaryOffset::MeshFloatOffset];
        if (vertexOffset < 0) vertexOffset += 65536;
        span = std::max(span, (std::size_t)vertexOffset + vertexCount * 4);
        if (vd->weight != nullptr)
        {
            int wf = data->intArray[vd->weight->offset + (unsigned)BinaryOffset::WeigthFloatOffset];
            if (wf < 0) wf += 65536;
            span = std::max(span, (std::size_t)wf + vd->weight->count * 3);
        }
    }
    return span;
}

std::size_t packedIntSpan(DragonBonesData* data, Armature* arm)
{
    std::size_t span = 0;
    for (const auto slot : arm->getSlots())
    {
        const auto dv = slot->_deformVertices;
        if (dv == nullptr || dv->verticesData == nullptr) continue;
        const auto vd = dv->verticesData;
        span = std::max(span, (std::size_t)vd->offset);
        if (vd->weight != nullptr)
        {
            span = std::max(span, (std::size_t)vd->weight->offset);
        }
    }
    // Include the full mesh/weight header regions by a fixed margin.
    return span + 64;
}

// ---------------------------------------------------------------------------
// basis group
// ---------------------------------------------------------------------------

int basis_neutral_equals_independent_formula()
{
    Rig rig;
    rig.build();
    const std::map<std::string, Transform> none;

    auto* slotA = rig.armA->getSlot("slotA");
    auto* slotB = rig.armB->getSlot("slotA");
    TASK_CHECK(slotA != nullptr && slotB != nullptr);

    const auto wantA = expectedVertices(rig.armA, slotA, none);
    const auto wantB = expectedVertices(rig.armB, slotB, none);
    const auto gotA = collect(slotA);
    const auto gotB = collect(slotB);
    TASK_CHECK(matchVertices(gotA, wantA));
    TASK_CHECK(matchVertices(gotB, wantB));
    TASK_CHECK(matchVertices(gotA, gotB));

    // Bone world matrices at neutral equal B0.
    for (const auto name : { "root", "mid", "tip" })
    {
        const auto b0 = setupWorld(rig.armA->_armatureData, name, none);
        const auto& m = rig.armA->getBone(name)->globalTransformMatrix;
        TASK_CHECK(nearF(m.a, b0.a) && nearF(m.d, b0.d) && nearF(m.tx, b0.tx) && nearF(m.ty, b0.ty));
    }
    rig.dispose();
    return 0;
}

int basis_override_keeps_neutral_shape()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");
    auto* slotB = rig.armB->getSlot("slotA");

    const auto before = collect(slotA);
    const auto otherBefore = collect(slotB);

    std::map<std::string, Transform> overrides;
    overrides["root"] = tr(4, -3, 30, 0, 1.3, 0.8);
    overrides["tip"] = tr(2.5, 1.0, 50, 5, 1.05, 1.2);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(rig.armA->hasSetupPoseOverrides());

    // Neutral skinning result is invariant under the setup change, while the
    // overridden bone world matrices follow B1.
    const auto after = collect(slotA);
    TASK_CHECK(matchVertices(after, before));

    const auto b1Root = setupWorld(rig.armA->_armatureData, "root", overrides);
    const auto& rootMatrix = rig.armA->getBone("root")->globalTransformMatrix;
    TASK_CHECK(nearF(rootMatrix.tx, b1Root.tx) && nearF(rootMatrix.ty, b1Root.ty));

    // The other armature sharing the same data is completely untouched.
    TASK_CHECK(!rig.armB->hasSetupPoseOverrides());
    TASK_CHECK(matchVertices(collect(slotB), otherBefore));
    TASK_CHECK(rig.armB->getBone("root")->origin == &rig.armB->getBone("root")->_boneData->transform);
    rig.dispose();
    return 0;
}

int basis_animation_uses_new_setup_and_old_motion()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");
    auto* slotB = rig.armB->getSlot("slotA");

    std::map<std::string, Transform> overrides;
    overrides["root"] = tr(4, -3, 30, 0, 1.3, 0.8);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));

    TASK_CHECK(rig.armA->getAnimation()->play("move") != nullptr);
    TASK_CHECK(rig.armB->getAnimation()->play("move") != nullptr);
    rig.armA->advanceTime(0.5f);
    rig.armB->advanceTime(0.5f);

    // Overridden instance: old motion on top of the new setup.
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, overrides)));
    for (const auto name : { "root", "mid", "tip" })
    {
        const auto want = expectedBoneWorld(rig.armA, name, overrides);
        const auto& m = rig.armA->getBone(name)->globalTransformMatrix;
        TASK_CHECK(nearF(m.a, want.a) && nearF(m.b, want.b) && nearF(m.c, want.c) &&
                   nearF(m.d, want.d) && nearF(m.tx, want.tx) && nearF(m.ty, want.ty));
    }

    // Uncovered instance keeps the original setup plus the same motion.
    const std::map<std::string, Transform> none;
    TASK_CHECK(matchVertices(collect(slotB), expectedVertices(rig.armB, slotB, none)));
    rig.dispose();
    return 0;
}

int basis_unlisted_descendant_rebinds_through_chain()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");

    std::map<std::string, Transform> overrides;
    // Only the root is listed; mid and tip must still move through B1.
    overrides["root"] = tr(-2, 5, -25, 0, 0.9, 1.4);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(rig.armA->getAnimation()->play("move") != nullptr);
    rig.armA->advanceTime(0.3f);

    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, overrides)));
    const auto tipWant = expectedBoneWorld(rig.armA, "tip", overrides);
    const auto& tipMatrix = rig.armA->getBone("tip")->globalTransformMatrix;
    TASK_CHECK(nearF(tipMatrix.tx, tipWant.tx) && nearF(tipMatrix.ty, tipWant.ty));
    rig.dispose();
    return 0;
}

int basis_offset_still_combines_with_override()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");

    // A bone offset must keep combining with the overridden origin exactly by
    // the existing Additive rule (origin + offset + animationPose).
    auto* root = rig.armA->getBone("root");
    root->offset.x = 1.5f;
    root->offset.rotation = 12.0f * Transform::DEG_RAD;
    root->invalidUpdate();

    std::map<std::string, Transform> overrides;
    overrides["root"] = tr(4, -3, 30, 0, 1.3, 0.8);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(rig.armA->getAnimation()->play("move") != nullptr);
    rig.armA->advanceTime(0.35f);

    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, overrides)));
    // The offset itself is untouched by the override machinery.
    TASK_CHECK(nearF(root->offset.x, 1.5f) && nearF(root->offset.rotation, 12.0f * Transform::DEG_RAD));
    rig.dispose();
    return 0;
}

int basis_replacement_and_empty_map_semantics()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");

    std::map<std::string, Transform> first;
    first["root"] = tr(4, -3, 30, 0, 1.3, 0.8);
    first["tip"] = tr(2.5, 1.0, 50, 5, 1.05, 1.2);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(first));
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, first)));

    // A second complete mapping replaces the first one: tip falls back to its
    // original setup and only root stays overridden.
    std::map<std::string, Transform> second;
    second["root"] = tr(-1, 2, -10, 0, 0.9, 1.1);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(second));
    TASK_CHECK(rig.armA->hasSetupPoseOverrides());
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, second)));
    TASK_CHECK(rig.armA->getBone("tip")->origin == &rig.armA->getBone("tip")->_boneData->transform);
    TASK_CHECK(rig.armA->getBone("tip")->_setupBindMatrix != nullptr); // Chain relation remains.

    // An empty mapping is equivalent to clearSetupPoseOverrides().
    const std::map<std::string, Transform> empty;
    TASK_CHECK(rig.armA->setSetupPoseOverrides(empty));
    TASK_CHECK(!rig.armA->hasSetupPoseOverrides());
    const std::map<std::string, Transform> none;
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, none)));

    // Clearing when nothing is installed is a safe no-op returning normally.
    rig.armA->clearSetupPoseOverrides();
    TASK_CHECK(!rig.armA->hasSetupPoseOverrides());
    rig.dispose();
    return 0;
}

int basis_failed_input_rolls_back()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");
    const auto before = collect(slotA);
    const auto& rootMatrixBefore = rig.armA->getBone("root")->globalTransformMatrix;

    std::map<std::string, Transform> good;
    good["root"] = tr(1, 1, 10, 0, 1.1, 1.1);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(good));

    std::map<std::string, Transform> unknown;
    unknown["ghost"] = tr(0, 0, 0, 0, 1, 1);
    TASK_CHECK(!rig.armA->setSetupPoseOverrides(unknown));
    TASK_CHECK(rig.armA->hasSetupPoseOverrides()); // Previous override intact.

    auto badScale = good;
    badScale["mid"] = tr(0, 0, 0, 0, -1.0f, 1.0f);
    TASK_CHECK(!rig.armA->setSetupPoseOverrides(badScale));

    auto nonFinite = good;
    nonFinite["tip"] = tr(0, 0, 0, 0, std::nan(""), 1.0f);
    TASK_CHECK(!rig.armA->setSetupPoseOverrides(nonFinite));
    TASK_CHECK(rig.armA->hasSetupPoseOverrides()); // Good mapping still intact.

    // Pose, output and the good override are all unchanged.
    const auto& rootMatrixAfter = rig.armA->getBone("root")->globalTransformMatrix;
    TASK_CHECK(nearF(rootMatrixAfter.tx, rootMatrixBefore.tx) && nearF(rootMatrixAfter.ty, rootMatrixBefore.ty));
    TASK_CHECK(matchVertices(collect(slotA), before));
    rig.dispose();
    return 0;
}

int basis_unsupported_configuration_rejected()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");
    const auto before = collect(slotA);

    std::map<std::string, Transform> overrides;
    overrides["root"] = tr(1, 1, 10, 0, 1.1, 1.1);

    // Non-Additive offset mode is outside the supported binding contract.
    rig.armA->getBone("mid")->offsetMode = OffsetMode::Override;
    TASK_CHECK(!rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(!rig.armA->hasSetupPoseOverrides());
    rig.armA->getBone("mid")->offsetMode = OffsetMode::Additive;

    // A disabled inheritance flag is rejected as well (test-local mutation,
    // restored immediately since the BoneData is shared).
    const auto tipData = const_cast<BoneData*>(rig.armA->getBone("tip")->_boneData);
    tipData->inheritScale = false;
    TASK_CHECK(!rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(!rig.armA->hasSetupPoseOverrides());
    tipData->inheritScale = true;

    // Flipped armatures are rejected.
    rig.armA->setFlipX(true);
    TASK_CHECK(!rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(!rig.armA->hasSetupPoseOverrides());
    rig.armA->setFlipX(false);

    // Nothing was installed and every output stays on the original path.
    TASK_CHECK(rig.armA->getBone("root")->_setupBindMatrix == nullptr);
    TASK_CHECK(matchVertices(collect(slotA), before));
    rig.dispose();
    return 0;
}

int basis_pool_reuse_clears_binding()
{
    Rig rig;
    rig.build();
    std::map<std::string, Transform> overrides;
    overrides["root"] = tr(4, -3, 30, 0, 1.3, 0.8);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    rig.dispose(); // Returns every Armature/Bone/Slot to the object pool.

    auto* reborn = rig.factory.buildArmature("rig", "fixture");
    reborn->advanceTime(0.0f);
    TASK_CHECK(!reborn->hasSetupPoseOverrides());
    TASK_CHECK(reborn->getBone("root")->origin == &reborn->getBone("root")->_boneData->transform);
    TASK_CHECK(reborn->getBone("root")->_setupBindMatrix == nullptr);

    const std::map<std::string, Transform> none;
    auto* slotA = reborn->getSlot("slotA");
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(reborn, slotA, none)));
    reborn->dispose();
    rig.factory.runtime.advanceTime(0.0f);
    return 0;
}

// ---------------------------------------------------------------------------
// deform group
// ---------------------------------------------------------------------------

int deform_ffd_direction_skips_bind_translation()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");

    std::map<std::string, Transform> overrides;
    // A large translation makes any accidental application to the FFD
    // direction obviously wrong.
    overrides["root"] = tr(10, 0, 0, 0, 1, 1);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(rig.armA->getAnimation()->play("move") != nullptr);
    rig.armA->advanceTime(0.5f);

    const auto got = collect(slotA);
    const auto want = expectedVertices(rig.armA, slotA, overrides);
    TASK_CHECK(matchVertices(got, want));

    // The deliberately wrong formula (full affine on the direction) must not
    // match, proving the translation is excluded from the direction path.
    const auto& deform = slotA->_deformVertices->vertices;
    TASK_CHECK(!deform.empty());
    const auto layout = readSkinLayout(slotA);
    const auto& bones = slotA->_deformVertices->bones;
    V2 wrong{ 0.0f, 0.0f };
    V2 right{ 0.0f, 0.0f };
    std::size_t iD = 0;
    for (std::size_t i = 0; i < layout.size(); ++i)
    {
        for (const auto& inf : layout[i].influences)
        {
            const auto bone = bones[inf.boneIndex];
            const auto b0 = setupWorld(rig.armA->_armatureData, bone->getName(), {});
            const auto b1 = setupWorld(rig.armA->_armatureData, bone->getName(), overrides);
            const auto r = mMul(b0, mInverse(b1));
            const V2 d{ deform[iD * 2], deform[iD * 2 + 1] };
            wrong.x += inf.weight * (mPoint(r, d).x);
            right.x += inf.weight * (mDelta(r, d).x);
            ++iD;
        }
    }
    TASK_CHECK(std::fabs(wrong.x - right.x) > 1.0f);
    rig.dispose();
    return 0;
}

int deform_clear_restores_shared_setup()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");
    auto* slotB = rig.armB->getSlot("slotA");

    std::map<std::string, Transform> overrides;
    overrides["mid"] = tr(0, 4, 20, 0, 1.2, 0.7);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(rig.armA->getAnimation()->play("move") != nullptr);
    TASK_CHECK(rig.armB->getAnimation()->play("move") != nullptr);
    rig.armA->advanceTime(0.4f);
    rig.armB->advanceTime(0.4f);

    rig.armA->clearSetupPoseOverrides();
    TASK_CHECK(!rig.armA->hasSetupPoseOverrides());
    rig.armA->advanceTime(0.0f);

    const std::map<std::string, Transform> none;
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, none)));
    // Same shared data, same animation time, same vertices as the sibling.
    TASK_CHECK(matchVertices(collect(slotA), collect(slotB)));
    rig.dispose();
    return 0;
}

int deform_switch_mesh_rebinds()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");

    std::map<std::string, Transform> overrides;
    overrides["root"] = tr(3, 3, 25, 0, 1.1, 0.9);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(rig.armA->getAnimation()->play("move") != nullptr);
    rig.armA->advanceTime(0.25f);

    TASK_CHECK(slotA->getDisplayIndex() == 0);
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, overrides)));
    TASK_CHECK(collect(slotA).size() == 4);

    slotA->setDisplayIndex(1);
    slotA->update(-1);
    TASK_CHECK(slotA->getDisplayIndex() == 1);
    TASK_CHECK(collect(slotA).size() == 3);
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, overrides)));

    slotA->setDisplayIndex(0);
    slotA->update(-1);
    TASK_CHECK(collect(slotA).size() == 4);
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, overrides)));
    rig.dispose();
    return 0;
}

int deform_empty_and_non_mesh_queries()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");

    std::vector<Point> sentinel(2);
    sentinel[0].x = 123.0f;
    sentinel[0].y = 456.0f;
    sentinel[1].x = 789.0f;
    sentinel[1].y = -12.0f;
    auto copy = sentinel;

    slotA->setDisplayIndex(-1);
    slotA->update(-1);
    TASK_CHECK(!slotA->getDeformedVertices(copy));
    TASK_CHECK(copy.size() == sentinel.size());
    TASK_CHECK(nearF(copy[0].x, 123.0f) && nearF(copy[1].y, -12.0f));

    // Restore and confirm normal query behavior again.
    slotA->setDisplayIndex(0);
    slotA->update(-1);
    const std::map<std::string, Transform> none;
    TASK_CHECK(slotA->getDeformedVertices(copy));
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, none)));

    // A non-mesh (image) display has no packed vertices: the query fails and
    // leaves its output untouched as well.
    auto* slotC = rig.armA->getSlot("slotC");
    TASK_CHECK(slotC != nullptr);
    auto imageCopy = copy;
    const auto imageSize = imageCopy.size();
    TASK_CHECK(!slotC->getDeformedVertices(imageCopy));
    TASK_CHECK(imageCopy.size() == imageSize);
    TASK_CHECK(nearF(imageCopy[0].x, copy[0].x));
    rig.dispose();
    return 0;
}

int deform_output_is_independent_copy()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");

    std::map<std::string, Transform> overrides;
    overrides["tip"] = tr(1, 1, 15, 0, 1.2, 1.2);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(rig.armA->getAnimation()->play("move") != nullptr);
    rig.armA->advanceTime(0.2f);

    std::vector<Point> first;
    TASK_CHECK(slotA->getDeformedVertices(first));
    const auto snapshot = first;

    // More sampling and a display switch must not mutate the old array.
    rig.armA->advanceTime(0.2f);
    slotA->setDisplayIndex(1);
    slotA->update(-1);
    TASK_CHECK(first.size() == snapshot.size());
    for (std::size_t i = 0; i < snapshot.size(); ++i)
    {
        TASK_CHECK(first[i].x == snapshot[i].x && first[i].y == snapshot[i].y);
    }

    // It must not alias the shared packed array storage.
    const auto vd = slotA->_deformVertices->verticesData;
    const auto packedBegin = reinterpret_cast<const char*>(&vd->data->floatArray[0]);
    const auto packedEnd = packedBegin +
                           static_cast<std::ptrdiff_t>(packedFloatSpan(rig.data, rig.armA)) * static_cast<std::ptrdiff_t>(sizeof(float));
    for (const auto& p : first)
    {
        const auto address = reinterpret_cast<const char*>(&p);
        TASK_CHECK(address < packedBegin || address >= packedEnd);
    }

    // Disposal must not invalidate the old array either.
    rig.dispose();
    TASK_CHECK(first.size() == snapshot.size());
    TASK_CHECK(nearF(first[0].x, snapshot[0].x) && nearF(first[1].y, snapshot[1].y));
    return 0;
}

int deform_unskinned_mesh_uses_slot_transform()
{
    Rig rig;
    rig.build();
    auto* slotB = rig.armA->getSlot("slotB");
    const std::map<std::string, Transform> none;
    TASK_CHECK(matchVertices(collect(slotB), expectedVertices(rig.armA, slotB, none)));

    std::map<std::string, Transform> overrides;
    overrides["mid"] = tr(2, -2, 10, 0, 1.1, 0.9);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    rig.armA->advanceTime(0.0f);
    // Unskinned output keeps following the slot (parent bone) transform.
    TASK_CHECK(matchVertices(collect(slotB), expectedVertices(rig.armA, slotB, overrides)));
    rig.dispose();
    return 0;
}

int deform_ffd_plus_offset_both_rebind()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");

    rig.armA->getBone("tip")->offset.y = 2.0f;
    rig.armA->getBone("tip")->invalidUpdate();

    std::map<std::string, Transform> overrides;
    overrides["root"] = tr(6, 4, -20, 0, 1.2, 0.8);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(rig.armA->getAnimation()->play("move") != nullptr);
    rig.armA->advanceTime(0.4f); // FFD keys are active here.

    // One independent expectation covers point re-bind, FFD direction re-bind
    // (linear only) and the surviving bone offset together.
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, overrides)));
    rig.dispose();
    return 0;
}

int deform_mixed_animations_still_match_formula()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");

    std::map<std::string, Transform> overrides;
    overrides["root"] = tr(1, -1, 12, 0, 1.05, 0.95);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(rig.armA->getAnimation()->play("move") != nullptr);
    TASK_CHECK(rig.armA->getAnimation()->fadeIn("mix", 0.2f) != nullptr);
    rig.armA->advanceTime(0.1f);

    // Two blended AnimationStates feed animationPose; the core relation only
    // consumes the resulting pose, so it still matches the independent formula.
    TASK_CHECK(rig.armA->getAnimation()->getStates().size() == 2);
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, overrides)));
    rig.dispose();
    return 0;
}

// ---------------------------------------------------------------------------
// cache group
// ---------------------------------------------------------------------------

int cache_other_instance_keeps_using_shared_cache()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");
    auto* slotB = rig.armB->getSlot("slotA");

    rig.armB->setCacheFrameRate(24);
    TASK_CHECK(rig.armB->getCacheFrameRate() == 24);
    TASK_CHECK(rig.armA->getCacheFrameRate() == 24); // Shared ArmatureData.

    TASK_CHECK(rig.armB->getAnimation()->play("move") != nullptr);
    rig.armB->advanceTime(0.5f);
    TASK_CHECK(rig.armB->getBone("root")->_cachedFrameIndices != nullptr);

    const auto animationData = rig.data->getArmature("rig")->getAnimation("move");
    auto cachedFramesBefore = 0;
    for (const auto flag : animationData->cachedFrames) if (flag) ++cachedFramesBefore;
    TASK_CHECK(cachedFramesBefore > 0);

    // The overridden instance must evaluate uncached without disturbing the
    // shared cache rate or any cache frame produced by the sibling.
    std::map<std::string, Transform> overrides;
    overrides["root"] = tr(3, 0, 20, 0, 1.2, 0.8);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(rig.armA->getAnimation()->play("move") != nullptr);
    rig.armA->advanceTime(0.5f);
    TASK_CHECK(rig.armA->getBone("root")->_cachedFrameIndices == nullptr);
    TASK_CHECK(rig.armB->getBone("root")->_cachedFrameIndices != nullptr);
    TASK_CHECK(rig.armA->getCacheFrameRate() == 24);

    auto cachedFramesAfter = 0;
    for (const auto flag : animationData->cachedFrames) if (flag) ++cachedFramesAfter;
    TASK_CHECK(cachedFramesAfter == cachedFramesBefore);
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, overrides)));

    // The sibling keeps reading its cached mapping and stays correct.
    const std::map<std::string, Transform> none;
    TASK_CHECK(matchVertices(collect(slotB), expectedVertices(rig.armB, slotB, none)));
    rig.dispose();
    return 0;
}

int cache_same_frame_repeated_set_and_clear()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");

    rig.armA->setCacheFrameRate(24);
    TASK_CHECK(rig.armA->getAnimation()->play("move") != nullptr);
    rig.armA->advanceTime(0.3f);
    const auto state = rig.armA->getAnimation()->getState("move");
    TASK_CHECK(state != nullptr);
    const auto timeBefore = state->getCurrentTime();
    const auto weightBefore = state->weight;
    // One cache quantization step: currentTime = floor(t * 2 * rate) / (2 * rate).
    const auto cacheStep = 1.0f / (2.0f * 24.0f) + 1e-6f;

    std::map<std::string, Transform> overrides;
    overrides["mid"] = tr(0, 2, 30, 5, 0.9, 1.2);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    rig.armA->advanceTime(0.0f); // Same cache frame, immediate refresh.
    const auto first = collect(slotA);
    rig.armA->advanceTime(0.0f); // Again, still the same animation frame.
    TASK_CHECK(matchVertices(collect(slotA), first));
    TASK_CHECK(matchVertices(first, expectedVertices(rig.armA, slotA, overrides)));

    // Zero-time calls must not advance the state: the uncached readout only
    // removes the shared-cache quantization (at most one step), and repeated
    // zero-time evaluations stay exactly stable.
    const auto timeOverridden = state->getCurrentTime();
    TASK_CHECK(std::fabs(timeOverridden - timeBefore) <= cacheStep);
    rig.armA->advanceTime(0.0f);
    TASK_CHECK(std::fabs(state->getCurrentTime() - timeOverridden) < 1e-6f);
    TASK_CHECK(nearF(state->weight, weightBefore));

    rig.armA->clearSetupPoseOverrides();
    TASK_CHECK(nearF(state->weight, weightBefore));
    // Back on the cached path: the quantized readout matches the original one.
    TASK_CHECK(std::fabs(state->getCurrentTime() - timeBefore) <= 1e-6f);

    const std::map<std::string, Transform> none;
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, none)));
    TASK_CHECK(rig.armA->getCacheFrameRate() == 24);

    // A real time step afterwards still advances the same state normally.
    rig.armA->advanceTime(0.1f);
    TASK_CHECK(state->getCurrentTime() > timeBefore);
    rig.dispose();
    return 0;
}

int cache_clear_returns_to_safe_cached_path()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");

    rig.armA->setCacheFrameRate(24);
    TASK_CHECK(rig.armA->getAnimation()->play("move") != nullptr);
    rig.armA->advanceTime(0.2f);

    std::map<std::string, Transform> overrides;
    overrides["root"] = tr(2, 2, 15, 0, 1.3, 0.7);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    rig.armA->advanceTime(0.2f);
    TASK_CHECK(rig.armA->getBone("root")->_cachedFrameIndices == nullptr);

    rig.armA->clearSetupPoseOverrides();
    rig.armA->advanceTime(0.0f);
    // Cached frame indices are re-linked for this instance after clearing.
    TASK_CHECK(rig.armA->getBone("root")->_cachedFrameIndices != nullptr);

    rig.armA->advanceTime(0.2f);
    const std::map<std::string, Transform> none;
    TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, none)));
    rig.dispose();
    return 0;
}

int cache_shared_bytes_and_bonedata_unchanged()
{
    Rig rig;
    rig.build();

    std::vector<Transform> boneSnapshot;
    for (const auto boneData : rig.data->getArmature("rig")->sortedBones)
    {
        boneSnapshot.push_back(boneData->transform);
    }
    const auto floatSpan = packedFloatSpan(rig.data, rig.armA);
    const auto intSpan = packedIntSpan(rig.data, rig.armA);
    std::vector<float> floatSnapshot(rig.data->floatArray, rig.data->floatArray + floatSpan);
    std::vector<int16_t> intSnapshot(rig.data->intArray, rig.data->intArray + intSpan);
    const auto* floatPointer = rig.data->floatArray;

    // Snapshot the FFD deform produced by the shared keys before any override
    // activity on the sibling.
    TASK_CHECK(rig.armB->getAnimation()->play("move") != nullptr);
    rig.armB->advanceTime(0.25f);
    const auto ffdSnapshot = rig.armB->getSlot("slotA")->_deformVertices->vertices;
    TASK_CHECK(!ffdSnapshot.empty());

    std::map<std::string, Transform> overrides;
    overrides["root"] = tr(5, -5, 40, 0, 1.4, 0.6);
    overrides["tip"] = tr(0, 0, 20, 10, 0.8, 1.5);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(rig.armA->getAnimation()->play("move") != nullptr);
    rig.armA->advanceTime(0.5f);
    rig.armA->clearSetupPoseOverrides();
    rig.armA->advanceTime(0.5f);

    TASK_CHECK(rig.data->floatArray == floatPointer);
    TASK_CHECK(checkSharedPackedUnchanged(rig.data, floatSnapshot, intSnapshot, floatSpan, intSpan));

    std::size_t k = 0;
    for (const auto boneData : rig.data->getArmature("rig")->sortedBones)
    {
        const auto& t0 = boneSnapshot[k++];
        const auto& t1 = boneData->transform;
        TASK_CHECK(nearF(t0.x, t1.x) && nearF(t0.y, t1.y) && nearF(t0.rotation, t1.rotation) &&
                   nearF(t0.skew, t1.skew) && nearF(t0.scaleX, t1.scaleX) && nearF(t0.scaleY, t1.scaleY));
    }

    // A brand new armature on the same shared data must read identical FFD
    // keys at the same animation time, proving the keys were never modified.
    auto* armC = rig.factory.buildArmature("rig", "fixture");
    TASK_CHECK(armC->getAnimation()->play("move") != nullptr);
    armC->advanceTime(0.25f);
    const auto& ffdAgain = armC->getSlot("slotA")->_deformVertices->vertices;
    TASK_CHECK(ffdAgain.size() == ffdSnapshot.size());
    for (std::size_t i = 0; i < ffdSnapshot.size(); ++i)
    {
        TASK_CHECK(nearF(ffdAgain[i], ffdSnapshot[i]));
    }
    armC->dispose();
    rig.factory.runtime.advanceTime(0.0f);
    rig.dispose();
    return 0;
}

int cache_repeated_set_clear_keeps_every_path_correct()
{
    Rig rig;
    rig.build();
    auto* slotA = rig.armA->getSlot("slotA");
    auto* slotB = rig.armB->getSlot("slotA");
    rig.armA->setCacheFrameRate(24); // Shared ArmatureData cache stays enabled.

    TASK_CHECK(rig.armA->getAnimation()->play("move") != nullptr);
    TASK_CHECK(rig.armB->getAnimation()->play("move") != nullptr);

    const std::map<std::string, Transform> none;
    std::map<std::string, Transform> a;
    a["root"] = tr(2, 2, 15, 0, 1.2, 0.9);
    std::map<std::string, Transform> b;
    b["mid"] = tr(0, -3, 25, 8, 0.8, 1.3);

    // Two full set/clear cycles while the sibling keeps using shared caches.
    for (int cycle = 0; cycle < 2; ++cycle)
    {
        const auto& active = cycle == 0 ? a : b;
        TASK_CHECK(rig.armA->setSetupPoseOverrides(active));
        TASK_CHECK(rig.armA->_isSetupPoseCacheBypassed());
        rig.armA->advanceTime(0.2f);
        rig.armB->advanceTime(0.2f);
        TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, active)));
        TASK_CHECK(matchVertices(collect(slotB), expectedVertices(rig.armB, slotB, none)));
        TASK_CHECK(rig.armB->getBone("root")->_cachedFrameIndices != nullptr);

        rig.armA->clearSetupPoseOverrides();
        TASK_CHECK(!rig.armA->_isSetupPoseCacheBypassed());
        rig.armA->advanceTime(0.1f);
        rig.armB->advanceTime(0.1f);
        TASK_CHECK(matchVertices(collect(slotA), expectedVertices(rig.armA, slotA, none)));
        // Normal cached production path is re-armed for this instance.
        TASK_CHECK(rig.armA->getBone("root")->_cachedFrameIndices != nullptr);
        TASK_CHECK(rig.armA->getCacheFrameRate() == 24);
    }
    rig.dispose();
    return 0;
}

int cache_pool_reuse_does_not_restore_override()
{
    Rig rig;
    rig.build();
    rig.armA->setCacheFrameRate(24);
    std::map<std::string, Transform> overrides;
    overrides["root"] = tr(1, 1, 5, 0, 1.1, 0.9);
    TASK_CHECK(rig.armA->setSetupPoseOverrides(overrides));
    TASK_CHECK(rig.armA->hasSetupPoseOverrides());
    rig.dispose();

    auto* reborn = rig.factory.buildArmature("rig", "fixture");
    reborn->advanceTime(0.0f);
    TASK_CHECK(!reborn->hasSetupPoseOverrides());
    TASK_CHECK(!reborn->_isSetupPoseCacheBypassed());
    TASK_CHECK(reborn->getCacheFrameRate() == 24); // Shared setting survives.
    reborn->dispose();
    rig.factory.runtime.advanceTime(0.0f);
    return 0;
}

} // namespace

extern const struct task_case task_cases[] = {
    { "basis", "neutral_equals_independent_formula", basis_neutral_equals_independent_formula },
    { "basis", "override_keeps_neutral_shape", basis_override_keeps_neutral_shape },
    { "basis", "animation_uses_new_setup_and_old_motion", basis_animation_uses_new_setup_and_old_motion },
    { "basis", "unlisted_descendant_rebinds_through_chain", basis_unlisted_descendant_rebinds_through_chain },
    { "basis", "offset_still_combines_with_override", basis_offset_still_combines_with_override },
    { "basis", "replacement_and_empty_map_semantics", basis_replacement_and_empty_map_semantics },
    { "basis", "failed_input_rolls_back", basis_failed_input_rolls_back },
    { "basis", "unsupported_configuration_rejected", basis_unsupported_configuration_rejected },
    { "basis", "pool_reuse_clears_binding", basis_pool_reuse_clears_binding },
    { "deform", "ffd_direction_skips_bind_translation", deform_ffd_direction_skips_bind_translation },
    { "deform", "clear_restores_shared_setup", deform_clear_restores_shared_setup },
    { "deform", "switch_mesh_rebinds", deform_switch_mesh_rebinds },
    { "deform", "empty_and_non_mesh_queries", deform_empty_and_non_mesh_queries },
    { "deform", "output_is_independent_copy", deform_output_is_independent_copy },
    { "deform", "unskinned_mesh_uses_slot_transform", deform_unskinned_mesh_uses_slot_transform },
    { "deform", "ffd_plus_offset_both_rebind", deform_ffd_plus_offset_both_rebind },
    { "deform", "mixed_animations_still_match_formula", deform_mixed_animations_still_match_formula },
    { "cache", "other_instance_keeps_using_shared_cache", cache_other_instance_keeps_using_shared_cache },
    { "cache", "same_frame_repeated_set_and_clear", cache_same_frame_repeated_set_and_clear },
    { "cache", "clear_returns_to_safe_cached_path", cache_clear_returns_to_safe_cached_path },
    { "cache", "shared_bytes_and_bonedata_unchanged", cache_shared_bytes_and_bonedata_unchanged },
    { "cache", "repeated_set_clear_keeps_every_path_correct", cache_repeated_set_clear_keeps_every_path_correct },
    { "cache", "pool_reuse_does_not_restore_override", cache_pool_reuse_does_not_restore_override },
};
extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
