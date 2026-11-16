/*
 * SoftGLRender
 * @author 	: keith@robot9.me
 *
 */

#pragma once

#include <atomic>

namespace SoftGL {

template<typename T>
class UUID {
 public:
  UUID() : uuid_(uuidCounter_.fetch_add(1)) {}

  inline int get() const {
    return uuid_;
  }

 private:
  int uuid_ = -1;
  static std::atomic<int> uuidCounter_;
};

template<typename T>
std::atomic<int> UUID<T>::uuidCounter_{0};

}
