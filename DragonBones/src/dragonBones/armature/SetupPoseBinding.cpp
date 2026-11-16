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
#include "SetupPoseBinding.h"
#include "Armature.h"
#include "Bone.h"
#include "Slot.h"
#include "DeformVertices.h"

#include <cmath>

DRAGONBONES_NAMESPACE_BEGIN

static bool _isFiniteTransform(const Transform& value)
{
    return
        std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.skew) && std::isfinite(value.rotation) &&
        std::isfinite(value.scaleX) && std::isfinite(value.scaleY);
}

bool SetupPoseBinding::_validate(const Armature* armature, const std::map<std::string, Transform>& overrides) const
{
    if (armature == nullptr || armature->_armatureData == nullptr)
    {
        return false;
    }

    // Flipping changes the world matrix composition, it is outside the
    // supported binding contract.
    if (armature->getFlipX() || armature->getFlipY())
    {
        return false;
    }

    // Constraints drive bones outside the plain parent chain.
    if (!armature->_constraints.empty())
    {
        return false;
    }

    for (const auto bone : armature->getBones())
    {
        if (bone == nullptr || bone->origin == nullptr || bone->offsetMode != OffsetMode::Additive)
        {
            return false;
        }

        const auto boneData = bone->_boneData;
        if (
            boneData == nullptr ||
            !boneData->inheritTranslation || !boneData->inheritRotation ||
            !boneData->inheritScale || !boneData->inheritReflection
        )
        {
            return false;
        }

        if (!_isFiniteTransform(*bone->origin) || bone->origin->scaleX <= 0.0f || bone->origin->scaleY <= 0.0f)
        {
            return false;
        }
    }

    for (const auto& pair : overrides)
    {
        const auto bone = armature->getBone(pair.first);
        if (bone == nullptr)
        {
            return false; // Unknown bone name.
        }

        if (!_isFiniteTransform(pair.second) || pair.second.scaleX <= 0.0f || pair.second.scaleY <= 0.0f)
        {
            return false; // Non-finite or non-positive scale.
        }
    }

    return true;
}

void SetupPoseBinding::_computeMatrices(Armature* armature)
{
    // sortedBones is parent-first, so every parent matrix is ready when used.
    const auto& armatureData = armature->_armatureData;
    std::map<const BoneData*, Matrix> setupWorldMatrices;
    std::map<const BoneData*, Matrix> overrideWorldMatrices;

    for (const auto boneData : armatureData->sortedBones)
    {
        const auto bone = armature->getBone(boneData->name);
        if (bone == nullptr)
        {
            continue;
        }

        Transform setupOrigin = boneData->transform;
        Transform overrideOrigin = boneData->transform;
        const auto iterator = _bindings.find(boneData->name);
        if (iterator != _bindings.end())
        {
            overrideOrigin = iterator->second.originOverride;
        }

        Matrix setupLocal;
        setupOrigin.toMatrix(setupLocal);
        Matrix overrideLocal;
        overrideOrigin.toMatrix(overrideLocal);

        Matrix setupWorld = setupLocal;
        Matrix overrideWorld = overrideLocal;
        if (boneData->parent != nullptr)
        {
            setupWorld.concat(setupWorldMatrices[boneData->parent]);
            overrideWorld.concat(overrideWorldMatrices[boneData->parent]);
        }

        setupWorldMatrices[boneData] = setupWorld;
        overrideWorldMatrices[boneData] = overrideWorld;

        // DragonBones matrices use row-vector multiplication (a point is
        // p · M and Matrix::concat(A, B) is A · B). The column-notation
        // relation W · inverse(B1) · B0 therefore becomes
        // p · B0 · inverse(B1) · W here: the re-bind matrix applied to the
        // stored local point is R = B0 · inverse(B1), and the bone's current
        // world matrix W is applied afterwards.
        Matrix overrideInverse = overrideWorld;
        overrideInverse.invert();
        Matrix bind = setupWorld;
        bind.concat(overrideInverse);

        Matrix bindDelta = bind;
        bindDelta.tx = 0.0f;
        bindDelta.ty = 0.0f;

        if (iterator != _bindings.end())
        {
            iterator->second.bindMatrix = bind;
            iterator->second.bindDeltaMatrix = bindDelta;
        }
        else
        {
            // Unlisted bone: keep the shared origin, only store the relation
            // produced by the overridden ancestor chain.
            BoneBinding binding;
            binding.overridden = false;
            binding.bindMatrix = bind;
            binding.bindDeltaMatrix = bindDelta;
            _bindings[boneData->name] = binding;
        }
    }
}

void SetupPoseBinding::_detachFromBones(Armature* armature)
{
    for (const auto bone : armature->getBones())
    {
        if (bone == nullptr || bone->_boneData == nullptr)
        {
            continue;
        }

        bone->origin = &(bone->_boneData->transform);
        bone->_setupBindMatrix = nullptr;
        bone->_setupBindDeltaMatrix = nullptr;
    }
}

void SetupPoseBinding::_applyToBones(Armature* armature)
{
    for (const auto bone : armature->getBones())
    {
        if (bone == nullptr || bone->_boneData == nullptr)
        {
            continue;
        }

        const auto iterator = _bindings.find(bone->getName());
        if (iterator == _bindings.end())
        {
            continue;
        }

        // std::map node addresses stay valid until the whole mapping is
        // replaced. Unlisted bones keep the shared BoneData origin; explicitly
        // overridden ones use their instance-local replacement.
        if (iterator->second.overridden)
        {
            bone->origin = &(iterator->second.originOverride);
        }
        else
        {
            bone->origin = &(bone->_boneData->transform);
        }
        bone->_setupBindMatrix = &(iterator->second.bindMatrix);
        bone->_setupBindDeltaMatrix = &(iterator->second.bindDeltaMatrix);
    }
}

bool SetupPoseBinding::apply(Armature* armature, const std::map<std::string, Transform>& overrides)
{
    if (overrides.empty())
    {
        clear(armature);
        return true;
    }

    if (!_validate(armature, overrides))
    {
        return false; // Every existing state is left untouched.
    }

    std::map<std::string, BoneBinding> next;
    for (const auto& pair : overrides)
    {
        BoneBinding binding;
        binding.overridden = true;
        binding.originOverride = pair.second;
        next[pair.first] = binding;
    }

    // Build the full relation (B0/B1 for every bone, including unlisted ones)
    // off the live object first; only swap it in once everything is ready.
    SetupPoseBinding staging;
    staging._bindings = std::move(next);
    staging._computeMatrices(armature);

    // Positive scale alone does not guarantee an invertible affine when skew is
    // allowed (e.g. a 90 degree skew collapses the basis). Reject any relation
    // whose matrices stopped being finite rather than committing NaNs.
    for (const auto& pair : staging._bindings)
    {
        const auto& m = pair.second.bindMatrix;
        if (
            !std::isfinite(m.a) || !std::isfinite(m.b) ||
            !std::isfinite(m.c) || !std::isfinite(m.d) ||
            !std::isfinite(m.tx) || !std::isfinite(m.ty)
        )
        {
            return false;
        }
    }

    _detachFromBones(armature);
    _bindings = std::move(staging._bindings);
    _active = true;
    _applyToBones(armature);

    return true;
}

void SetupPoseBinding::clear(Armature* armature)
{
    if (!_active)
    {
        return;
    }

    if (armature != nullptr)
    {
        _detachFromBones(armature);
    }

    _bindings.clear();
    _active = false;
}

void SetupPoseBinding::reset()
{
    // Pool reset: bones are cleared independently through their own _onClear(),
    // so this must not dereference any armature or bone pointer.
    _bindings.clear();
    _active = false;
}

void SetupPoseBinding::notifyMeshChanged(Armature* armature)
{
    if (!_active || armature == nullptr)
    {
        return;
    }

    // The relation is purely bone based and does not depend on the mesh.
    // Re-assert every bone pointer so a freshly built DeformVertices goes
    // through the same binding, and force mesh re-evaluation.
    _applyToBones(armature);

    for (const auto slot : armature->getSlots())
    {
        if (slot->_deformVertices != nullptr)
        {
            slot->_deformVertices->verticesDirty = true;
        }

        slot->invalidUpdate();
    }
}

DRAGONBONES_NAMESPACE_END
