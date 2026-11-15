#pragma once
#include <memory>
#include <vector>
#include "dragonBones/DragonBonesHeaders.h"
namespace task_support {
using namespace dragonBones;
struct Events: IEventDispatcher {
 bool hasDBEventListener(const std::string&)const override{return false;}
 void dispatchDBEvent(const std::string&,EventObject*)override{}
 void addDBEventListener(const std::string&,const std::function<void(EventObject*)>&)override{}
 void removeDBEventListener(const std::string&,const std::function<void(EventObject*)>&)override{}
};
struct Proxy: IArmatureProxy {
 Armature* armature=nullptr;
 void dbInit(Armature* a)override{armature=a;} void dbClear()override{armature=nullptr;}void dbUpdate()override{}
 void dispose(bool)override{if(armature)armature->dispose();}
 Armature* getArmature()const override{return armature;}
 Animation* getAnimation()const override{return armature?armature->getAnimation():nullptr;}
 bool hasDBEventListener(const std::string&)const override{return false;}
 void dispatchDBEvent(const std::string&,EventObject*)override{}
 void addDBEventListener(const std::string&,const std::function<void(EventObject*)>&)override{}
 void removeDBEventListener(const std::string&,const std::function<void(EventObject*)>&)override{}
};
struct Slot: dragonBones::Slot {
 BIND_CLASS_TYPE_A(Slot);
public:
 unsigned meshUpdates=0;
 void _updateVisible()override{}void _updateBlendMode()override{}void _updateColor()override{}
protected:
 void _onClear()override{dragonBones::Slot::_onClear();meshUpdates=0;}
 void _initDisplay(void*,bool)override{}void _disposeDisplay(void*,bool)override{}void _onUpdateDisplay()override{}
 void _addDisplay()override{}void _replaceDisplay(void*,bool)override{}void _removeDisplay()override{}
 void _updateZOrder()override{}void _updateFrame()override{}void _updateMesh()override{++meshUpdates;}
 void _updateTransform()override{}void _identityTransform()override{}
};
struct Texture: TextureData {
 BIND_CLASS_TYPE_A(Texture);
};
struct Atlas: TextureAtlasData {
 BIND_CLASS_TYPE_A(Atlas);
public:
 TextureData* createTexture()const override{return BaseObject::borrowObject<Texture>();}
};
struct Factory: BaseFactory {
 Events events;DragonBones runtime{&events};mutable std::vector<std::unique_ptr<Proxy>> proxies;
 Factory(){_dragonBones=&runtime;}
 ~Factory()override{clear();}
protected:
 TextureAtlasData* _buildTextureAtlasData(TextureAtlasData* value,void*)const override{return value?value:BaseObject::borrowObject<Atlas>();}
 Armature* _buildArmature(const BuildArmaturePackage& p)const override{
  auto arm=BaseObject::borrowObject<Armature>();auto proxy=std::make_unique<Proxy>();arm->init(p.armature,proxy.get(),proxy.get(),_dragonBones);proxies.push_back(std::move(proxy));return arm;
 }
 dragonBones::Slot* _buildSlot(const BuildArmaturePackage&,const SlotData* data,Armature* arm)const override{
  auto slot=BaseObject::borrowObject<Slot>();slot->init(data,arm,slot,slot);return slot;
 }
};
}
