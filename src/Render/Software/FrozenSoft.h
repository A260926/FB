/*
 * SoftGLRender
 * @author 	: keith@robot9.me
 *
 */

#pragma once

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Base/HashUtils.h"
#include "TextureSoft.h"

namespace SoftGL {

// Untyped handle to an immutable snapshot, used for cache entries and command
// lifetime accounting. A snapshot holds no pointer back to its source.
class SnapshotRef {
 public:
  virtual ~SnapshotRef() = default;
  virtual size_t bytes() const = 0;
};

// Immutable snapshot of one texture layer image, including every mip level.
// Multi sample images are not supported: they have no single sample copy path.
template<typename T>
class FrozenTextureImageSoft : public TextureImageSoft<T>, public SnapshotRef {
 public:
  static inline std::shared_ptr<FrozenTextureImageSoft<T>> freeze(const TextureImageSoft<T> *src) {
    if (!src || src->levels.empty()) {
      return nullptr;
    }
    auto frozen = std::make_shared<FrozenTextureImageSoft<T>>();
    frozen->levels.reserve(src->levels.size());
    for (auto &level : src->levels) {
      if (!level || level->multiSample || !level->buffer) {
        return nullptr;
      }
      auto copy = std::make_shared<ImageBufferSoft<T>>();
      copy->width = level->width;
      copy->height = level->height;
      copy->multiSample = false;
      copy->sampleCnt = 1;
      copy->buffer = Buffer<T>::makeDefault(level->buffer->getWidth(), level->buffer->getHeight());
      if (!copy->buffer) {
        return nullptr;
      }
      // copy through the accessor so any storage layout is handled correctly
      for (size_t y = 0; y < copy->buffer->getHeight(); y++) {
        for (size_t x = 0; x < copy->buffer->getWidth(); x++) {
          T *dst = copy->buffer->get(x, y);
          const T *srcPtr = level->buffer->get(x, y);
          if (dst && srcPtr) {
            *dst = *srcPtr;
          }
        }
      }
      frozen->levels.emplace_back(std::move(copy));
    }
    return frozen;
  }

  size_t bytes() const override {
    size_t total = 0;
    for (auto &level : this->levels) {
      total += level->buffer->getRawDataBytesSize();
    }
    return total;
  }
};

// Validate a source image and produce a content fingerprint over every mip
// level (sizes, level count, element size and pixels). Returns false when the
// image is missing, multi sample or has an incomplete mip chain.
template<typename T>
inline bool describeTextureImage(TextureImageSoft<T> *image, bool useMipmaps, TextureFormat format,
                                 std::string &fingerprint, size_t &bytes) {
  if (!image || image->levels.empty()) {
    return false;
  }
  size_t levelCount = image->levels.size();
  size_t baseWidth = (size_t) image->levels[0]->width;
  size_t baseHeight = (size_t) image->levels[0]->height;
  if (baseWidth == 0 || baseHeight == 0) {
    return false;
  }
  if (useMipmaps) {
    size_t expected = (size_t) std::floor(std::log2((double) std::max(baseWidth, baseHeight))) + 1;
    if (levelCount < expected) {
      return false;
    }
  }

  MD5_CTX ctx;
  MD5_Init(&ctx);
  uint32_t layout = image->levels[0]->buffer ? (uint32_t) image->levels[0]->buffer->getLayout() : 0u;
  uint32_t meta[6] = {(uint32_t) baseWidth, (uint32_t) baseHeight, (uint32_t) levelCount,
                      (uint32_t) format, layout, 0x534f4654u};
  MD5_Update(&ctx, (unsigned char *) meta, sizeof(meta));

  bytes = 0;
  for (size_t level = 0; level < levelCount; level++) {
    auto &img = image->levels[level];
    if (!img || img->multiSample || !img->buffer) {
      return false;
    }
    uint32_t dims[2] = {(uint32_t) img->width, (uint32_t) img->height};
    MD5_Update(&ctx, (unsigned char *) dims, sizeof(dims));
    size_t dataBytes = img->buffer->getRawDataBytesSize();
    if (dataBytes > 0) {
      MD5_Update(&ctx, (unsigned char *) img->buffer->getRawDataPtr(), (unsigned int) dataBytes);
    }
    bytes += dataBytes;
  }

  unsigned char digest[16] = {0};
  MD5_Final(digest, &ctx);
  static const char *hex = "0123456789abcdef";
  char str[33] = {0};
  for (int i = 0; i < 16; i++) {
    str[i * 2] = hex[(digest[i] >> 4) & 0xF];
    str[i * 2 + 1] = hex[digest[i] & 0xF];
  }
  fingerprint.assign(str, 32);
  return true;
}

}
