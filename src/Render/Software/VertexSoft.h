/*
 * SoftGLRender
 * @author 	: keith@robot9.me
 *
 */

#pragma once

#include <cstring>
#include <memory>
#include <vector>
#include "Base/UUID.h"
#include "Render/Vertex.h"

namespace SoftGL {

class VertexArrayObjectSoft : public VertexArrayObject {
 public:
  explicit VertexArrayObjectSoft(const VertexArray &vertexArray) {
    // init vertexes
    vertexStride = vertexArray.vertexesDesc[0].stride;
    vertexCnt = vertexArray.vertexesBufferLength / vertexStride;
    vertexes.resize(vertexCnt * vertexStride);
    memcpy(vertexes.data(), vertexArray.vertexesBuffer, vertexArray.vertexesBufferLength);

    // init indices
    indicesCnt = vertexArray.indexBufferLength / sizeof(int32_t);
    indices.resize(indicesCnt);
    memcpy(indices.data(), vertexArray.indexBuffer, vertexArray.indexBufferLength);
  }

  void updateVertexData(void *data, size_t length) override {
    memcpy(vertexes.data(), data, std::min(length, vertexes.size()));
  }

  int getId() const override {
    return uuid_.get();
  }

  // storage access; frozen meshes override these to share a snapshot
  virtual const uint8_t *vertexData() const { return vertexes.data(); }
  virtual const int32_t *indexData() const { return indices.data(); }
  virtual size_t vertexCount() const { return vertexCnt; }
  virtual size_t indexCount() const { return indicesCnt; }
  virtual size_t indexStride() const { return vertexStride; }

 public:
  size_t vertexStride = 0;
  size_t vertexCnt = 0;
  size_t indicesCnt = 0;
  std::vector<uint8_t> vertexes;
  std::vector<int32_t> indices;

 protected:
  VertexArrayObjectSoft() = default;

 private:
  UUID<VertexArrayObjectSoft> uuid_;
};

// Replay-side mesh view that shares an immutable snapshot, never copying it.
class FrozenVertexArraySoft : public VertexArrayObjectSoft {
 public:
  FrozenVertexArraySoft(std::shared_ptr<const std::vector<uint8_t>> vertexes,
                        std::shared_ptr<const std::vector<int32_t>> indices,
                        size_t stride)
      : vertexes_(std::move(vertexes)), indices_(std::move(indices)) {
    vertexStride = stride;
    vertexCnt = vertexStride > 0 ? vertexes_->size() / vertexStride : 0;
    indicesCnt = indices_->size();
  }

  const uint8_t *vertexData() const override { return vertexes_->data(); }
  const int32_t *indexData() const override { return indices_->data(); }
  size_t vertexCount() const override { return vertexCnt; }
  size_t indexCount() const override { return indicesCnt; }
  size_t indexStride() const override { return vertexStride; }

  void updateVertexData(void *, size_t) override {}

 private:
  std::shared_ptr<const std::vector<uint8_t>> vertexes_;
  std::shared_ptr<const std::vector<int32_t>> indices_;
};

}
