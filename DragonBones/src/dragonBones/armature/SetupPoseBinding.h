/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2012-2018 DragonBones team and other contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef DRAGONBONES_SETUP_POSE_BINDING_H
#define DRAGONBONES_SETUP_POSE_BINDING_H

#include "../core/DragonBones.h"
#include "../geom/Matrix.h"
#include "../geom/Transform.h"

DRAGONBONES_NAMESPACE_BEGIN

class Armature;
class Bone;

/**
 * - Per-armature setup pose override binding.
 * The instance data (BoneData, animation timelines, packed mesh arrays, FFD
 * keys) is never modified: overrides only replace the setup origin seen by this
 * armature's bones, and every skinned point is re-bound through the relation
 * between the original pure setup world matrix B0 and the overridden pure setup
 * world matrix B1. DragonBones matrices use row-vector multiplication (a point
 * is p · M), so the column-notation relation
 *
 *     world = W · inverse(B1) · B0 · (p + d)
 *
 * is evaluated as
 *
 *     R(bone) = B0 · inverse(B1)
 *     world   = (R · (p + d)) · W
 *
 * where p is the stored bone local vertex, d the current frame FFD direction in
 * the same bone local space and W the bone's current world matrix (which keeps
 * combining offset and animationPose exactly as before). R is a full affine for
 * points; its translation is never applied to directions.
 *
 * Only skeletons without constraints, with every bone using Additive offset
 * mode and all four inherit flags enabled, positive finite setup/override scales
 * and no armature flip are supported. A failing validation leaves every
 * previously existing state untouched.
 * @version DragonBones 5.6
 * @language en_US
 */
/**
 * - 骨架实例级别的 setup pose 覆盖绑定。
 * 共享数据（BoneData、动画时间轴、网格 packed 数组、FFD 键值）不会被修改：
 * 覆盖仅替换本骨架骨骼看到的 setup origin，并通过原始纯 setup 世界矩阵 B0 与
 * 覆盖后纯 setup 世界矩阵 B1 之间的关系重新绑定每个蒙皮点：
 *
 *     R(骨骼) = inverse(B1) * B0
 *     世界点  = W * R * (p + d)
 *
 * 其中 p 为存储的骨骼局部顶点，d 为同一骨骼局部空间内本帧的 FFD 方向，
 * W 为该骨骼当前的世界矩阵，offset 与 animationPose 的组合规则保持不变。
 * R 对点使用完整仿射，其平移分量不会作用于方向。
 *
 * 仅支持无约束、全部骨骼为 Additive offset 模式、四个继承开关均开启、
 * setup/覆盖缩放有限且严格为正、骨架未翻转的情况。校验失败时所有既有状态
 * 保持不变。
 * @version DragonBones 5.6
 * @language zh_CN
 */
class SetupPoseBinding
{
public:
    struct BoneBinding
    {
        /**
         * - The setup origin that replaces BoneData::transform for this bone.
         * Only meaningful when overridden is true.
         */
        Transform originOverride;
        /**
         * - Whether this bone was explicitly named in the override mapping.
         * Unlisted bones keep the shared BoneData origin but still carry a
         * re-bind matrix because ancestor setup changes move their B1.
         */
        bool overridden;
        /**
         * - The re-bind matrix B0 * inverse(B1), full affine for points.
         */
        Matrix bindMatrix;
        /**
         * - The linear part of the re-bind matrix, used for FFD directions.
         */
        Matrix bindDeltaMatrix;

        BoneBinding() :
            overridden(false)
        {}
    };

private:
    bool _active;
    /**
     * - Node addresses stay valid while the map is alive; bone pointers are
     * refreshed on every successful apply/clear.
     */
    std::map<std::string, BoneBinding> _bindings;

    bool _validate(const Armature* armature, const std::map<std::string, Transform>& overrides) const;
    void _computeMatrices(Armature* armature);
    void _applyToBones(Armature* armature);
    void _detachFromBones(Armature* armature);

public:
    SetupPoseBinding() :
        _active(false)
    {}
    ~SetupPoseBinding() {}

    /**
     * - Validate and install a complete override mapping. An empty mapping is
     * equivalent to clearSetupPoseOverrides(). Returns false (without changing
     * any state) for unknown bone names, non-finite values or unsupported
     * skeleton configurations.
     */
    bool apply(Armature* armature, const std::map<std::string, Transform>& overrides);
    /**
     * - Remove all overrides of this armature and restore the shared setup.
     */
    void clear(Armature* armature);
    /**
     * - Hard reset used by the object pool; never touches shared data.
     */
    void reset();
    /**
     * - Re-run the binding after the current mesh display changed. The bind
     * matrices only depend on the bone setup chain, but every active bone
     * pointer is re-asserted so the new mesh goes through the same relation.
     */
    void notifyMeshChanged(Armature* armature);

    inline bool isActive() const
    {
        return _active;
    }
    inline const std::map<std::string, BoneBinding>& getBindings() const
    {
        return _bindings;
    }
};

DRAGONBONES_NAMESPACE_END
#endif // DRAGONBONES_SETUP_POSE_BINDING_H
