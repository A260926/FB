#include "Armature.h"
#include "../model/TextureAtlasData.h"
#include "../model/UserData.h"
#include "../animation/WorldClock.h"
#include "../animation/Animation.h"
#include "../event/EventObject.h"
#include "IArmatureProxy.h"
#include "Bone.h"
#include "Slot.h"
#include "DeformVertices.h"
#include "Constraint.h"

DRAGONBONES_NAMESPACE_BEGIN

bool Armature::_onSortSlots(Slot* a, Slot* b)
{
    return a->_zOrder < b->_zOrder ? true : false;
}

void Armature::_onClear()
{
    // Detach every per-instance binding pointer before the bones go back to the
    // pool; the binding itself is discarded as well.
    _setupPoseBinding.clear(this);
    _setupPoseBinding.reset();
    _setupPoseCacheBypass = false;
    _setupPoseForceFrame = false;

    if (_clock != nullptr) // Remove clock before slots clear.
    {
        _clock->remove(this);
    }

    for (const auto bone : _bones)
    {
        bone->returnToPool();
    }

    for (const auto slot : _slots)
    {
        slot->returnToPool();
    }

    for (const auto constraint : _constraints)
    {
        constraint->returnToPool();
    }

    for (const auto action : _actions)
    {
        action->returnToPool();
    }

    if(_animation != nullptr)
    {
        _animation->returnToPool();
    }

    if (_proxy != nullptr)
    {
        _proxy->dbClear();
    }

    if (_replaceTextureAtlasData != nullptr)
    {
        _replaceTextureAtlasData->returnToPool();
    }

    inheritAnimation = true;
    userData = nullptr;

    _debugDraw = false;
    _lockUpdate = false;
    _slotsDirty = false;
    _zOrderDirty = false;
    _flipX = false;
    _flipY = false;
    _cacheFrameIndex = -1;
    _bones.clear();
    _slots.clear();
    _constraints.clear();
    _actions.clear();
    _armatureData = nullptr;
    _animation = nullptr;
    _proxy = nullptr;
    _display = nullptr;
    _replaceTextureAtlasData = nullptr;
    _replacedTexture = nullptr;
    _dragonBones = nullptr;
    _clock = nullptr;
    _parent = nullptr;
}

void Armature::_sortZOrder(const int16_t* slotIndices, unsigned offset)
{
    const auto& slotDatas = _armatureData->sortedSlots;
    const auto isOriginal = slotIndices == nullptr;

    if (_zOrderDirty || !isOriginal) 
    {
        for (std::size_t i = 0, l = slotDatas.size(); i < l; ++i) 
        {
            const auto slotIndex = isOriginal ? i : (std::size_t)slotIndices[offset + i];
            if (slotIndex < 0 || slotIndex >= l)
            {
                continue;
            }

            const auto slotData = slotDatas[slotIndex];
            const auto slot = getSlot(slotData->name);
            if (slot != nullptr) 
            {
                slot->_setZorder(i);
            }
        }

        _slotsDirty = true;
        _zOrderDirty = !isOriginal;
    }
}

void Armature::_addBone(Bone* value)
{
    if (std::find(_bones.begin(), _bones.end(), value) == _bones.end())
    {
        _bones.push_back(value);
    }
}

void Armature::_addSlot(Slot* value)
{
    if (std::find(_slots.begin(), _slots.end(), value) == _slots.end())
    {
        _slots.push_back(value);
    }
}

void Armature::_addConstraint(Constraint* value)
{
    if (std::find(_constraints.cbegin(), _constraints.cend(), value) == _constraints.cend())
    {
        _constraints.push_back(value);
    }
}

void Armature::_bufferAction(EventObject* action, bool append)
{
    if (std::find(_actions.cbegin(), _actions.cend(), action) == _actions.cend()) 
    {
        if (append) 
        {
            _actions.push_back(action);
        }
        else 
        {
            _actions.insert(_actions.begin(), action);
        }
    }
}

void Armature::dispose()
{
    if (_armatureData != nullptr) 
    {
        _lockUpdate = true;
        _dragonBones->bufferObject(this);
    }
}

void Armature::init(ArmatureData *armatureData, IArmatureProxy* proxy, void* display, DragonBones* dragonBones)
{
    if (_armatureData != nullptr)
    {
        return;
    }

    _armatureData = armatureData;
    _animation = BaseObject::borrowObject<Animation>();
    _proxy = proxy;
    _display = display;
    _dragonBones = dragonBones;

    _proxy->dbInit(this);
    _animation->init(this);
    _animation->setAnimations(_armatureData->animations);
}

void Armature::advanceTime(float passedTime)
{
    if (_lockUpdate)
    {
        return;
    }

    if (_armatureData == nullptr)
    {
        DRAGONBONES_ASSERT(false, "The armature has been disposed.");
        return;
    }
    else if(_armatureData->parent == nullptr)
    {
        DRAGONBONES_ASSERT(false, "The armature data has been disposed.\nPlease make sure dispose armature before call factory.clear().");
        return;
    }

    const auto prevCacheFrameIndex = _cacheFrameIndex;

    // Update animation.
    _animation->advanceTime(passedTime);

    // Sort slots.
    if (_slotsDirty)
    {
        _slotsDirty = false;
        std::sort(_slots.begin(), _slots.end(), Armature::_onSortSlots);
    }

    // An armature with setup pose overrides never consumes the shared cache
    // frames: pass -1 so bones and slots evaluate from the live timelines,
    // whatever cacheFrameRate the shared ArmatureData keeps for other
    // instances. A forced frame (set/clear boundary) also evaluates uncached
    // once so no stale or polluted cache frame is ever read.
    const auto forceFrame = _setupPoseForceFrame;
    const auto effectiveCacheFrameIndex = (_setupPoseCacheBypass || forceFrame) ? -1 : _cacheFrameIndex;

    // Update bones and slots.
    if (forceFrame || effectiveCacheFrameIndex < 0 || effectiveCacheFrameIndex != prevCacheFrameIndex)
    {
        for (const auto bone : _bones)
        {
            bone->update(effectiveCacheFrameIndex);
        }

        for (const auto slot : _slots)
        {
            slot->update(effectiveCacheFrameIndex);
        }
    }

    _setupPoseForceFrame = false;

    // Do actions.
    if (!_actions.empty()) 
    {
        _lockUpdate = true;

        for (const auto action : _actions)
        {
            const auto actionData = action->actionData;

            if (actionData != nullptr) 
            {
                if (actionData->type == ActionType::Play) 
                {
                    if (action->slot != nullptr)
                    {
                        const auto childArmature = action->slot->getChildArmature();
                        if (childArmature != nullptr) 
                        {
                            childArmature->getAnimation()->fadeIn(actionData->name);
                        }
                    }
                    else if (action->bone != nullptr) 
                    {
                        for (const auto slot : getSlots())
                        {
                            if (slot->getParent() == action->bone)
                            {
                                const auto childArmature = slot->getChildArmature();
                                if (childArmature != nullptr) 
                                {
                                    childArmature->getAnimation()->fadeIn(actionData->name);
                                }
                            }
                        }
                    }
                    else 
                    {
                        _animation->fadeIn(actionData->name);
                    }
                }
            }

            action->returnToPool();
        }

        _actions.clear();
        _lockUpdate = false;
    }

    _proxy->dbUpdate();
}

void Armature::invalidUpdate(const std::string& boneName, bool updateSlot)
{
    if (!boneName.empty())
    {
        const auto bone = getBone(boneName);
        if (bone != nullptr)
        {
            bone->invalidUpdate();

            if (updateSlot)
            {
                for (const auto slot : _slots)
                {
                    if (slot->getParent() == bone)
                    {
                        slot->invalidUpdate();
                    }
                }
            }
        }
    }
    else
    {
        for (const auto bone : _bones)
        {
            bone->invalidUpdate();
        }

        if (updateSlot)
        {
            for (const auto slot : _slots)
            {
                slot->invalidUpdate();
            }
        }
    }
}

Slot* Armature::containsPoint(float x, float y) const
{
    for(const auto slot : _slots)
    {
        if(slot->containsPoint(x,y)) {
            return slot;
        }
    }

    return nullptr;
}

Slot* Armature::intersectsSegment(
    float xA, float yA, float xB, float yB,
    Point* intersectionPointA,
    Point* intersectionPointB,
    Point* normalRadians
) const
{
    const auto isV = xA == xB;
    auto dMin = 0.0f;
    auto dMax = 0.0f;
    auto intXA = 0.0f;
    auto intYA = 0.0f;
    auto intXB = 0.0f;
    auto intYB = 0.0f;
    auto intAN = 0.0f;
    auto intBN = 0.0f;
    Slot* intSlotA = nullptr;
    Slot* intSlotB = nullptr;

    for (const auto & slot : _slots) 
    {
        auto intersectionCount = slot->intersectsSegment(xA, yA, xB, yB, intersectionPointA, intersectionPointB, normalRadians);
        if (intersectionCount > 0) 
        {
            if (intersectionPointA != nullptr || intersectionPointB != nullptr) 
            {
                if (intersectionPointA != nullptr) 
                {
                    float d = isV ? intersectionPointA->y - yA : intersectionPointA->x - xA;
                    if (d < 0.0f) 
                    {
                        d = -d;
                    }

                    if (intSlotA == nullptr || d < dMin) 
                    {
                        dMin = d;
                        intXA = intersectionPointA->x;
                        intYA = intersectionPointA->y;
                        intSlotA = slot;

                        if (normalRadians) {
                            intAN = normalRadians->x;
                        }
                    }
                }

                if (intersectionPointB != nullptr) 
                {
                    float d = intersectionPointB->x - xA;
                    if (d < 0.0f) 
                    {
                        d = -d;
                    }

                    if (intSlotB == nullptr || d > dMax) 
                    {
                        dMax = d;
                        intXB = intersectionPointB->x;
                        intYB = intersectionPointB->y;
                        intSlotB = slot;

                        if (normalRadians != nullptr) 
                        {
                            intBN = normalRadians->y;
                        }
                    }
                }
            }
            else 
            {
                intSlotA = slot;
                break;
            }
        }
    }

    if (intSlotA != nullptr && intersectionPointA != nullptr)
    {
        intersectionPointA->x = intXA;
        intersectionPointA->y = intYA;

        if (normalRadians != nullptr) 
        {
            normalRadians->x = intAN;
        }
    }

    if (intSlotB != nullptr && intersectionPointB != nullptr) 
    {
        intersectionPointB->x = intXB;
        intersectionPointB->y = intYB;

        if (normalRadians != nullptr) 
        {
            normalRadians->y = intBN;
        }
    }

    return intSlotA;
}

Bone* Armature::getBone(const std::string& name) const
{
    for (const auto& bone : _bones)
    {
        if (bone->getName() == name)
        {
            return bone;
        }
    }

    return nullptr;
}

Bone* Armature::getBoneByDisplay(void* display) const
{
    const auto slot = getSlotByDisplay(display);

    return slot != nullptr ? slot->getParent() : nullptr;
}

Slot* Armature::getSlot(const std::string& name) const
{
    for (const auto slot : _slots)
    {
        if (slot->getName() == name)
        {
            return slot;
        }
    }

    return nullptr;
}

Slot* Armature::getSlotByDisplay(void* display) const
{
    if (display != nullptr)
    {
        for (const auto slot : _slots)
        {
            if (slot->getDisplay() == display)
            {
                return slot;
            }
        }
    }

    return nullptr;
}

void Armature::setCacheFrameRate(unsigned value)
{
    if (_armatureData->cacheFrameRate != value)
    {
        _armatureData->cacheFrames(value);

        for (const auto & slot : _slots)
        {
            const auto childArmature = slot->getChildArmature();
            if (childArmature != nullptr && childArmature->getCacheFrameRate() == 0)
            {
                childArmature->setCacheFrameRate(value);
            }
        }
    }
}

bool Armature::setSetupPoseOverrides(const std::map<std::string, Transform>& overrides)
{
    if (overrides.empty())
    {
        clearSetupPoseOverrides();
        return true;
    }

    if (!_setupPoseBinding.apply(this, overrides))
    {
        return false; // Existing overrides, pose and cached relations untouched.
    }

    _setupPoseCacheBypass = true;
    _setupPoseForceFrame = true;

    // The shared ArmatureData cache must stay available for other instances:
    // only this instance stops reading/writing it.
    for (const auto bone : _bones)
    {
        bone->_invalidateCachedFrame();
    }

    for (const auto slot : _slots)
    {
        slot->_invalidateCachedFrame();
        slot->invalidUpdate();
        if (slot->_deformVertices != nullptr)
        {
            slot->_deformVertices->verticesDirty = true;
        }
    }

    // The next advanceTime(0) recomputes bones and meshes immediately; no
    // animation time, weight, fade, display index or FFD value is changed.
    advanceTime(0.0f);

    return true;
}

void Armature::clearSetupPoseOverrides()
{
    if (!_setupPoseBinding.isActive())
    {
        _setupPoseCacheBypass = false;
        return;
    }

    _setupPoseBinding.clear(this);
    _setupPoseCacheBypass = false;
    _setupPoseForceFrame = true;

    // Re-arm the normal cached/uncached production path. Cache frames produced
    // by other instances are shared data and remain valid; this instance never
    // wrote any while overrides were active.
    _animation->_armatureCacheReset();

    for (const auto bone : _bones)
    {
        bone->_invalidateCachedFrame();
    }

    for (const auto slot : _slots)
    {
        slot->_invalidateCachedFrame();
        slot->invalidUpdate();
        if (slot->_deformVertices != nullptr)
        {
            slot->_deformVertices->verticesDirty = true;
        }
    }

    advanceTime(0.0f);
}

void Armature::_notifyMeshDisplayChanged()
{
    if (_setupPoseBinding.isActive())
    {
        _setupPoseBinding.notifyMeshChanged(this);
    }
}

void Armature::setClock(WorldClock* value)
{
    if(_clock == value)
    {
        return;
    }

    if(_clock)
    {
        _clock->remove(this);
    }

    _clock = value;

    if(_clock)
    {
        _clock->add(this);
    }

    // Update childArmature clock.
    for (const auto& slot : _slots)
    {
        const auto childArmature = slot->getChildArmature();
        if (childArmature != nullptr) 
        {
            childArmature->setClock(_clock);
        }
    }
}

void Armature::setReplacedTexture(void* value)
{
    if (_replacedTexture == value) 
    {
        return;
    }

    if (_replaceTextureAtlasData != nullptr) 
    {
        _replaceTextureAtlasData->returnToPool();
        _replaceTextureAtlasData = nullptr;
    }

    _replacedTexture = value;

    for (const auto &slot : _slots) 
    {
        slot->invalidUpdate();
        slot->update(-1);
    }
}

DRAGONBONES_NAMESPACE_END
