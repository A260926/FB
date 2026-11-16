/*
 * SoftGLRender
 * @author 	: keith@robot9.me
 *
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>
#include "Base/UUID.h"
#include "Render/ShaderProgram.h"
#include "ShaderSoft.h"

namespace SoftGL {

class ShaderProgramSoft : public ShaderProgram {
 public:
  int getId() const override {
    return uuid_.get();
  }

  void addDefine(const std::string &def) override {
    defines_.emplace_back(def);
  }

  bool SetShaders(std::shared_ptr<ShaderSoft> vs, std::shared_ptr<ShaderSoft> fs) {
    vertexShader_ = std::move(vs);
    fragmentShader_ = std::move(fs);

    // defines
    auto &defineDesc = vertexShader_->getDefines();
    definesBuffer_ = MemoryUtils::makeBuffer<uint8_t>(defineDesc.size());
    vertexShader_->bindDefines(definesBuffer_.get());
    fragmentShader_->bindDefines(definesBuffer_.get());

    memset(definesBuffer_.get(), 0, sizeof(uint8_t) * defineDesc.size());
    for (auto &name : defines_) {
      for (int i = 0; i < defineDesc.size(); i++) {
        if (defineDesc[i] == name) {
          definesBuffer_.get()[i] = 1;
        }
      }
    }

    // builtin
    vertexShader_->bindBuiltin(&builtin_);
    fragmentShader_->bindBuiltin(&builtin_);

    // uniforms
    uniformBuffer_ = MemoryUtils::makeBuffer<uint8_t>(vertexShader_->getShaderUniformsSize());
    vertexShader_->bindShaderUniforms(uniformBuffer_.get());
    fragmentShader_->bindShaderUniforms(uniformBuffer_.get());

    return true;
  }

  inline void bindVertexAttributes(void *ptr) {
    vertexShader_->bindShaderAttributes(ptr);
  }

  inline void bindUniformBlockBuffer(void *data, size_t len, int binding) {
    int offset = vertexShader_->GetUniformOffset(binding);
    memcpy(uniformBuffer_.get() + offset, data, len);
  }

  inline void bindUniformSampler(std::shared_ptr<SamplerSoft> &sampler, int binding) {
    bindUniformSamplerPtr(sampler.get(), binding);
  }

  // bind a non-owning sampler pointer at the recorded location
  inline void bindUniformSamplerPtr(SamplerSoft *sampler, int binding) {
    if (!uniformBuffer_ || binding < 0 || binding >= (int) vertexShader_->getUniformsDesc().size()) {
      return;
    }
    int offset = vertexShader_->GetUniformOffset(binding);
    if (offset < 0) {
      return;
    }
    auto **ptr = reinterpret_cast<SamplerSoft **>(uniformBuffer_.get() + offset);
    *ptr = sampler;
    samplerPtrs_[binding] = sampler;
  }

  // binding -> sampler currently stored in the uniform buffer
  inline const std::map<int, SamplerSoft *> &getSamplerBindings() const {
    return samplerPtrs_;
  }

  // rebinding resources replaces the active sampler set; stale bindings from a
  // previous resources object must not be recorded
  void bindResources(ShaderResources &resources) override {
    samplerPtrs_.clear();
    ShaderProgram::bindResources(resources);
  }

  // keep replay-side objects (e.g. frozen samplers) alive with the program
  template<typename T>
  inline void keepAlive(std::shared_ptr<T> obj) {
    keepAlive_.push_back(std::static_pointer_cast<void>(std::move(obj)));
  }

  inline void bindVertexShaderVaryings(void *ptr) {
    vertexShader_->bindShaderVaryings(ptr);
  }

  inline void bindFragmentShaderVaryings(void *ptr) {
    fragmentShader_->bindShaderVaryings(ptr);
  }

  inline size_t getShaderVaryingsSize() {
    return vertexShader_->getShaderVaryingsSize();
  }

  inline int getUniformLocation(const std::string &name) {
    return vertexShader_->getUniformLocation(name);
  }

  inline int getUniformOffset(int location) {
    return vertexShader_->GetUniformOffset(location);
  }

  inline const std::vector<std::string> &getDefines() const {
    return defines_;
  }

  inline size_t getUniformBufferSize() const {
    return uniformBuffer_ ? vertexShader_->getShaderUniformsSize() : 0;
  }

  inline const uint8_t *getUniformBufferData() const {
    return uniformBuffer_.get();
  }

  // copy recorded uniform bytes (sizes must be compatible)
  inline bool copyUniformData(const void *data, size_t size) {
    size_t capacity = getUniformBufferSize();
    if (size == 0) {
      return true;
    }
    if (!uniformBuffer_ || size > capacity) {
      return false;
    }
    memset(uniformBuffer_.get(), 0, capacity);
    if (size > 0) {
      memcpy(uniformBuffer_.get(), data, size);
    }
    return true;
  }

  // snapshots of the shader instances, used only to produce independent clones
  inline std::shared_ptr<ShaderSoft> getVertexShader() const {
    return vertexShader_;
  }

  inline std::shared_ptr<ShaderSoft> getFragmentShader() const {
    return fragmentShader_;
  }

  inline ShaderBuiltin &getShaderBuiltin() {
    return builtin_;
  }

  inline void execVertexShader() {
    vertexShader_->shaderMain();
  }

  inline void prepareFragmentShader() {
    fragmentShader_->prepareExecMain();
  }

  inline void execFragmentShader() {
    fragmentShader_->setupSamplerDerivative();
    fragmentShader_->shaderMain();
  }

  inline std::shared_ptr<ShaderProgramSoft> clone() const {
    auto ret = std::make_shared<ShaderProgramSoft>(*this);

    ret->vertexShader_ = vertexShader_->clone();
    ret->fragmentShader_ = fragmentShader_->clone();

    // derivative callbacks are temporary: never carry the source / last replay
    ret->vertexShader_->texLodFunc = nullptr;
    ret->fragmentShader_->texLodFunc = nullptr;

    // own define buffer, re-bound to the clone's shader instances
    size_t defineCnt = vertexShader_->getDefines().size();
    ret->definesBuffer_ = MemoryUtils::makeBuffer<uint8_t>(defineCnt);
    if (defineCnt > 0 && definesBuffer_) {
      memcpy(ret->definesBuffer_.get(), definesBuffer_.get(), defineCnt);
    }
    ret->vertexShader_->bindDefines(ret->definesBuffer_.get());
    ret->fragmentShader_->bindDefines(ret->definesBuffer_.get());

    // own uniform buffer, re-bound to the clone's shader instances
    size_t uniformSize = vertexShader_->getShaderUniformsSize();
    ret->uniformBuffer_ = MemoryUtils::makeBuffer<uint8_t>(uniformSize);
    if (uniformSize > 0 && uniformBuffer_) {
      memcpy(ret->uniformBuffer_.get(), uniformBuffer_.get(), uniformSize);
    }
    ret->vertexShader_->bindShaderUniforms(ret->uniformBuffer_.get());
    ret->fragmentShader_->bindShaderUniforms(ret->uniformBuffer_.get());

    // own builtin
    ret->vertexShader_->bindBuiltin(&ret->builtin_);
    ret->fragmentShader_->bindBuiltin(&ret->builtin_);

    // independent sampler copies (own state + derivative callback) all sharing
    // the same read-only image; re-write their pointers into the clone buffer
    auto bindings = ret->samplerPtrs_;
    ret->samplerPtrs_.clear();
    ret->keepAlive_.clear();
    for (auto &kv : bindings) {
      if (kv.second == nullptr) {
        continue;
      }
      auto copy = kv.second->cloneSampler();
      ret->bindUniformSamplerPtr(copy.get(), kv.first);
      ret->keepAlive(std::move(copy));
    }

    return ret;
  }

 private:
  ShaderBuiltin builtin_;
  std::vector<std::string> defines_;

  std::shared_ptr<ShaderSoft> vertexShader_;
  std::shared_ptr<ShaderSoft> fragmentShader_;

  std::shared_ptr<uint8_t> definesBuffer_;  // 0->false; 1->true
  std::shared_ptr<uint8_t> uniformBuffer_;
  std::map<int, SamplerSoft *> samplerPtrs_;
  std::vector<std::shared_ptr<void>> keepAlive_;

 private:
  UUID<ShaderProgramSoft> uuid_;
};

}
