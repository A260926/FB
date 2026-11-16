/*
 * SoftGLRender
 * @author 	: keith@robot9.me
 *
 */

#include "CommandListSoft.h"

#include <algorithm>
#include <cstring>

#include "FramebufferSoft.h"
#include "RendererSoft.h"
#include "TextureSoft.h"
#include "UniformSoft.h"
#include "VertexSoft.h"

namespace SoftGL {

namespace {

template<typename T>
void bindFrozenTyped(SamplerSoft &sampler,
                     const std::shared_ptr<SnapshotRef> &image,
                     const SamplerRuntimeState &state,
                     const std::vector<uint8_t> &border) {
  auto *typed = dynamic_cast<Sampler2DSoft<T> *>(&sampler);
  auto *frozen = dynamic_cast<FrozenTextureImageSoft<T> *>(image.get());
  if (!typed || !frozen) {
    return;
  }
  typed->setFrozenImage(frozen, state,
                        border.size() == sizeof(T) ? border.data() : nullptr, sizeof(T));
}

}  // namespace

std::shared_ptr<SamplerSoft> makeFrozenSampler(TextureType type,
                                              TextureFormat format,
                                              const std::shared_ptr<SnapshotRef> &image,
                                              const SamplerRuntimeState &state,
                                              const std::vector<uint8_t> &border) {
  std::shared_ptr<SamplerSoft> ret;
  switch (type) {
    case TextureType_2D:
      switch (format) {
        case TextureFormat_RGBA8: ret = std::make_shared<Sampler2DSoft<RGBA>>(); break;
        case TextureFormat_FLOAT32: ret = std::make_shared<Sampler2DSoft<float>>(); break;
      }
      break;
    case TextureType_CUBE:
      switch (format) {
        case TextureFormat_RGBA8: ret = std::make_shared<SamplerCubeSoft<RGBA>>(); break;
        case TextureFormat_FLOAT32: ret = std::make_shared<SamplerCubeSoft<float>>(); break;
      }
      break;
  }
  if (!ret) {
    return nullptr;
  }
  if (type == TextureType_2D) {
    switch (format) {
      case TextureFormat_RGBA8: bindFrozenTyped<RGBA>(*ret, image, state, border); break;
      case TextureFormat_FLOAT32: bindFrozenTyped<float>(*ret, image, state, border); break;
    }
  }
  return ret;
}

// ----------------------------------------------------------------------------
// CommandSoft
// ----------------------------------------------------------------------------

std::shared_ptr<ShaderProgramSoft> CommandSoft::createProgram() const {
  auto program = std::make_shared<ShaderProgramSoft>();
  // defines must be known before SetShaders builds the define buffer
  for (auto &def : defines) {
    program->addDefine(def);
  }
  program->SetShaders(vertexShaderTemplate->clone(), fragmentShaderTemplate->clone());
  program->copyUniformData(uniformBytes.data(), uniformBytes.size());

  for (auto &rec : samplers) {
    auto sampler = makeFrozenSampler(rec.type, rec.format, rec.image, rec.state, rec.border);
    if (!sampler) {
      continue;
    }
    program->bindUniformSamplerPtr(sampler.get(), rec.binding);
    program->keepAlive(std::move(sampler));
  }
  return program;
}

// ----------------------------------------------------------------------------
// SnapshotStore
// ----------------------------------------------------------------------------

size_t SnapshotStore::imageSnapshotCount() const {
  size_t count = 0;
  for (auto it = textures_.begin(); it != textures_.end();) {
    if (it->second.expired()) {
      it = textures_.erase(it);
    } else {
      ++count;
      ++it;
    }
  }
  return count;
}

size_t SnapshotStore::meshSnapshotCount() const {
  size_t count = 0;
  for (auto it = meshes_.begin(); it != meshes_.end();) {
    if (it->second.expired()) {
      it = meshes_.erase(it);
    } else {
      ++count;
      ++it;
    }
  }
  return count;
}

size_t SnapshotStore::heldBytes() const {
  size_t total = 0;
  for (auto &kv : textures_) {
    if (auto entry = kv.second.lock()) {
      total += entry->bytes();
    }
  }
  for (auto &kv : meshes_) {
    if (auto entry = kv.second.lock()) {
      total += entry->bytes();
    }
  }
  return total;
}

std::shared_ptr<SnapshotStore::TextureEntry> SnapshotStore::findTexture(const std::string &fingerprint) const {
  auto it = textures_.find(fingerprint);
  if (it == textures_.end()) {
    return nullptr;
  }
  return it->second.lock();
}

std::shared_ptr<SnapshotStore::MeshEntry> SnapshotStore::findMesh(const std::string &fingerprint) const {
  auto it = meshes_.find(fingerprint);
  if (it == meshes_.end()) {
    return nullptr;
  }
  return it->second.lock();
}

std::shared_ptr<SnapshotStore::TextureEntry> SnapshotStore::storeTexture(const std::string &fingerprint,
                                                                        std::shared_ptr<SnapshotRef> image) {
  auto existing = findTexture(fingerprint);
  if (existing) {
    return existing;
  }
  auto entry = std::make_shared<TextureEntry>();
  entry->fingerprint = fingerprint;
  entry->byteSize = image->bytes();
  entry->image = std::move(image);
  textures_[fingerprint] = entry;
  copiedBytes_ += entry->byteSize;
  return entry;
}

std::shared_ptr<SnapshotStore::MeshEntry> SnapshotStore::storeMesh(const std::string &fingerprint,
                                                                  const std::vector<uint8_t> &vertexes,
                                                                  const std::vector<int32_t> &indices) {
  auto existing = findMesh(fingerprint);
  if (existing) {
    return existing;
  }
  auto entry = std::make_shared<MeshEntry>();
  entry->fingerprint = fingerprint;
  entry->vertexes = std::make_shared<const std::vector<uint8_t>>(vertexes);
  entry->indices = std::make_shared<const std::vector<int32_t>>(indices);
  entry->byteSize = vertexes.size() + indices.size() * sizeof(int32_t);
  meshes_[fingerprint] = entry;
  copiedBytes_ += entry->byteSize;
  return entry;
}

std::string SnapshotStore::meshFingerprint(size_t stride, const uint8_t *vertexes, size_t vertexBytes,
                                           const int32_t *indices, size_t indexCount) {
  MD5_CTX ctx;
  MD5_Init(&ctx);
  uint32_t meta[2] = {(uint32_t) stride, (uint32_t) indexCount};
  MD5_Update(&ctx, (unsigned char *) meta, sizeof(meta));
  if (vertexes && vertexBytes) {
    MD5_Update(&ctx, (unsigned char *) vertexes, (unsigned int) vertexBytes);
  }
  if (indices && indexCount) {
    MD5_Update(&ctx, (unsigned char *) indices, (unsigned int) (indexCount * sizeof(int32_t)));
  }
  unsigned char digest[16] = {0};
  MD5_Final(digest, &ctx);
  static const char *hex = "0123456789abcdef";
  char str[33] = {0};
  for (int i = 0; i < 16; i++) {
    str[i * 2] = hex[(digest[i] >> 4) & 0xF];
    str[i * 2 + 1] = hex[digest[i] & 0xF];
  }
  return std::string(str, 32);
}

// ----------------------------------------------------------------------------
// CommandListSoft
// ----------------------------------------------------------------------------

bool CommandListSoft::appendFrom(RendererSoft &renderer, SnapshotStore &snapshots) {
  VertexArrayObjectSoft *vao = renderer.getVertexArrayObject();
  ShaderProgramSoft *program = renderer.getShaderProgram();
  const RenderStates *states = renderer.getRenderStates();

  // incomplete binding: reject without side effects
  if (!vao || !program || !states) {
    return false;
  }
  if (!program->getVertexShader() || !program->getFragmentShader()) {
    return false;
  }
  if (vao->vertexStride == 0 || vao->vertexCount() == 0 || vao->indexCount() == 0) {
    return false;
  }

  size_t vertexBytes = vao->vertexCount() * vao->vertexStride;
  size_t uniformBytes = program->getUniformBufferSize();
  if (vertexBytes == 0) {
    return false;
  }
  if (uniformBytes > 0 && !program->getUniformBufferData()) {
    return false;
  }

  // --- build everything in a local command first (all-or-nothing) ---
  auto cmd = std::make_shared<CommandSoft>();
  cmd->vertexStride = vao->vertexStride;
  cmd->vertexCnt = vao->vertexCount();
  cmd->indicesCnt = vao->indexCount();
  cmd->viewport = renderer.getViewport();
  cmd->renderStates = *states;
  cmd->earlyZ = renderer.isEarlyZEnabled();
  // independent shader templates; no source or last-replay pointer is kept
  cmd->vertexShaderTemplate = program->getVertexShader()->clone();
  cmd->fragmentShaderTemplate = program->getFragmentShader()->clone();
  cmd->vertexShaderTemplate->clearBoundPointers();
  cmd->fragmentShaderTemplate->clearBoundPointers();
  cmd->defines = program->getDefines();
  if (uniformBytes > 0) {
    cmd->uniformBytes.assign(program->getUniformBufferData(),
                             program->getUniformBufferData() + uniformBytes);
  }

  // sampler validation first, so an unsupported resource aborts before copies
  struct PendingSampler {
    int binding;
    int uniformOffset;   // -1 when the binding has no uniform slot
    SamplerImageDesc desc;
    SamplerRuntimeState state;
    std::vector<uint8_t> border;
    std::shared_ptr<SnapshotRef> frozen;  // set when a cache miss
  };
  std::vector<PendingSampler> pending;
  for (auto &kv : program->getSamplerBindings()) {
    SamplerSoft *sampler = kv.second;
    if (!sampler || !sampler->valid()) {
      return false;
    }
    PendingSampler p;
    p.binding = kv.first;
    p.uniformOffset = program->getUniformOffset(kv.first);
    if (!sampler->describeImage(p.desc)) {
      return false;  // unsupported / empty / multi sample / incomplete mip chain
    }
    p.state = sampler->runtimeState();
    p.border = sampler->borderBytes();

    // share the frozen image with an earlier sampler of the same content in
    // this append (two samplers may point at one texture)
    bool reusedInAppend = false;
    for (auto &other : pending) {
      if (other.desc.fingerprint == p.desc.fingerprint) {
        p.frozen = other.frozen;
        reusedInAppend = true;
        break;
      }
    }
    if (!reusedInAppend && !snapshots.findTexture(p.desc.fingerprint)) {
      p.frozen = sampler->freezeImage();
      if (!p.frozen) {
        return false;
      }
    }
    pending.push_back(std::move(p));
  }

  // never freeze a live sampler pointer: replay rebinds its own frozen samplers
  for (auto &p : pending) {
    if (p.uniformOffset >= 0 && (size_t) p.uniformOffset + sizeof(void *) <= cmd->uniformBytes.size()) {
      memset(cmd->uniformBytes.data() + p.uniformOffset, 0, sizeof(void *));
    }
  }

  // geometry snapshot: identical content reuses one read-only mesh
  auto fingerprint = SnapshotStore::meshFingerprint(vao->vertexStride, vao->vertexData(), vertexBytes,
                                                   vao->indexData(), vao->indexCount());
  auto mesh = snapshots.findMesh(fingerprint);
  std::shared_ptr<SnapshotStore::MeshEntry> newMesh;
  if (!mesh) {
    std::vector<uint8_t> vertexCopy(vao->vertexData(), vao->vertexData() + vertexBytes);
    std::vector<int32_t> indexCopy(vao->indexData(), vao->indexData() + vao->indexCount());
    newMesh = std::make_shared<SnapshotStore::MeshEntry>();
    newMesh->fingerprint = fingerprint;
    newMesh->vertexes = std::make_shared<const std::vector<uint8_t>>(std::move(vertexCopy));
    newMesh->indices = std::make_shared<const std::vector<int32_t>>(std::move(indexCopy));
    newMesh->byteSize = newMesh->vertexes->size() + newMesh->indices->size() * sizeof(int32_t);
    mesh = newMesh;
  }

  // --- commit: every check passed, the store and list now may change ---
  if (newMesh) {
    mesh = snapshots.storeMesh(fingerprint, *newMesh->vertexes, *newMesh->indices);
  }
  cmd->vertexes = mesh->vertexes;
  cmd->indices = mesh->indices;
  cmd->heldSnapshots.push_back(mesh);

  for (auto &p : pending) {
    std::shared_ptr<SnapshotStore::TextureEntry> entry;
    if (p.frozen) {
      entry = snapshots.storeTexture(p.desc.fingerprint, p.frozen);
    } else {
      entry = snapshots.findTexture(p.desc.fingerprint);
    }
    if (!entry) {
      return false;
    }
    CommandSoft::SamplerRecord rec;
    rec.type = p.desc.type;
    rec.format = p.desc.format;
    rec.state = p.state;
    rec.border = std::move(p.border);
    rec.image = entry->image;  // the frozen image, not the cache wrapper
    rec.binding = p.binding;
    cmd->samplers.push_back(std::move(rec));
    cmd->heldSnapshots.push_back(entry);
  }

  commands_.push_back(std::move(cmd));
  return true;
}

bool CommandListSoft::replay(RendererSoft &renderer, std::shared_ptr<FrameBuffer> &frameBuffer) {
  auto *fbo = dynamic_cast<FrameBufferSoft *>(frameBuffer.get());
  if (!fbo || commands_.empty()) {
    return false;
  }

  // save everything a draw can mutate and restore afterwards
  auto saved = renderer.saveBinding();

  for (auto &cmd : commands_) {
    auto program = cmd->createProgram();
    auto mesh = std::make_shared<FrozenVertexArraySoft>(cmd->vertexes, cmd->indices, cmd->vertexStride);
    renderer.applyBinding(fbo, mesh.get(), program.get(), &cmd->renderStates, cmd->viewport, cmd->earlyZ);
    renderer.draw();
  }

  renderer.restoreBinding(saved);
  return true;
}

}
