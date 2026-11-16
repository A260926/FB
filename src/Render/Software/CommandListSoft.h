/*
 * SoftGLRender
 * @author 	: keith@robot9.me
 *
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Base/Buffer.h"
#include "Base/HashUtils.h"
#include "Base/Logger.h"
#include "Render/Framebuffer.h"
#include "Render/RenderStates.h"
#include "Render/Software/FrozenSoft.h"
#include "Render/Software/RendererInternal.h"
#include "Render/Software/SamplerSoft.h"
#include "Render/Software/ShaderProgramSoft.h"
#include "Render/Software/ShaderSoft.h"
#include "Render/Software/VertexSoft.h"

namespace SoftGL {

class RendererSoft;
class SnapshotStore;

// Build a fresh sampler of the given type/format bound to a frozen image.
// Implemented in CommandListSoft.cpp so commands stay free of typed pointers.
std::shared_ptr<SamplerSoft> makeFrozenSampler(TextureType type,
                                              TextureFormat format,
                                              const std::shared_ptr<SnapshotRef> &image,
                                              const SamplerRuntimeState &state,
                                              const std::vector<uint8_t> &border);

// Immutable description of one recorded draw. Holds no pointer into any source
// resource: the source VAO, uniforms, program, textures and buffers, and even
// the recording Renderer / SnapshotStore, may all be destroyed.
class CommandSoft {
 public:
  // Build a replay-ready program: fresh shader clones (re-bound to their own
  // uniforms / builtin / samplers / derivative callbacks) plus frozen samplers.
  std::shared_ptr<ShaderProgramSoft> createProgram() const;

 public:
  // geometry: shared immutable snapshot of what was bound
  size_t vertexStride = 0;
  size_t vertexCnt = 0;
  size_t indicesCnt = 0;
  std::shared_ptr<const std::vector<uint8_t>> vertexes;
  std::shared_ptr<const std::vector<int32_t>> indices;

  Viewport viewport{};
  RenderStates renderStates{};
  bool earlyZ = true;

  // shader description (instances are cloned on every replay)
  std::vector<std::string> defines;
  std::shared_ptr<ShaderSoft> vertexShaderTemplate;
  std::shared_ptr<ShaderSoft> fragmentShaderTemplate;
  std::vector<uint8_t> uniformBytes;

  struct SamplerRecord {
    TextureType type = TextureType_2D;
    TextureFormat format = TextureFormat_RGBA8;
    SamplerRuntimeState state;
    std::vector<uint8_t> border;
    std::shared_ptr<SnapshotRef> image;
    int binding = 0;
  };
  std::vector<SamplerRecord> samplers;

  // every snapshot this command keeps alive (textures and the mesh)
  std::vector<std::shared_ptr<SnapshotRef>> heldSnapshots;
};

class CommandListSoft {
 public:
  // Append an immutable copy of the currently bound state of `renderer`.
  // Runs no vertex or fragment shader and stores no final image / transformed
  // vertices. Returns false (leaving the list and store untouched) when the
  // binding is incomplete or uses an unsupported resource.
  bool appendFrom(RendererSoft &renderer, SnapshotStore &snapshots);

  // Replay in recorded order onto `renderer` / `frameBuffer`.
  // The caller owns clearing and target selection; the previous bindings,
  // viewport, states, VAO and program are restored before returning.
  bool replay(RendererSoft &renderer, std::shared_ptr<FrameBuffer> &frameBuffer);

  inline size_t size() const { return commands_.size(); }
  inline bool empty() const { return commands_.empty(); }

 private:
  std::vector<std::shared_ptr<CommandSoft>> commands_;
};

// Snapshot pool shared by command lists: identical source content reuses the
// same read-only copy. The cache never keeps a copy alive on its own, so a
// snapshot disappears as soon as no command references it.
class SnapshotStore {
 public:
  class TextureEntry : public SnapshotRef {
   public:
    size_t bytes() const override { return byteSize; }

    std::string fingerprint;
    size_t byteSize = 0;
    std::shared_ptr<SnapshotRef> image;  // FrozenTextureImageSoft<T>
  };

  class MeshEntry : public SnapshotRef {
   public:
    size_t bytes() const override { return byteSize; }

    std::string fingerprint;
    size_t byteSize = 0;
    std::shared_ptr<const std::vector<uint8_t>> vertexes;
    std::shared_ptr<const std::vector<int32_t>> indices;
  };

 public:
  SnapshotStore() = default;

  // Number of distinct live image / mesh snapshots
  size_t imageSnapshotCount() const;
  size_t meshSnapshotCount() const;
  size_t snapshotCount() const { return imageSnapshotCount() + meshSnapshotCount(); }

  // Bytes held by live snapshots
  size_t heldBytes() const;
  // Cumulative bytes copied when creating snapshots (cache hits are free)
  size_t copiedBytes() const { return copiedBytes_; }

  std::shared_ptr<TextureEntry> findTexture(const std::string &fingerprint) const;
  std::shared_ptr<MeshEntry> findMesh(const std::string &fingerprint) const;

  // Insert entries; identical live entries are reused instead of re-copied
  std::shared_ptr<TextureEntry> storeTexture(const std::string &fingerprint,
                                             std::shared_ptr<SnapshotRef> image);
  std::shared_ptr<MeshEntry> storeMesh(const std::string &fingerprint,
                                       const std::vector<uint8_t> &vertexes,
                                       const std::vector<int32_t> &indices);

  // content fingerprints; no copy is made, only bytes are hashed
  static std::string meshFingerprint(size_t stride, const uint8_t *vertexes, size_t vertexBytes,
                                     const int32_t *indices, size_t indexCount);

 private:
  // weak: a snapshot disappears as soon as no command holds it any more
  mutable std::unordered_map<std::string, std::weak_ptr<TextureEntry>> textures_;
  mutable std::unordered_map<std::string, std::weak_ptr<MeshEntry>> meshes_;
  size_t copiedBytes_ = 0;
};

}
