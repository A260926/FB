#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <new>
#include <type_traits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "cista/strong.h"
#include "cista/verify.h"

namespace cista {

template <typename T, typename Key = std::uint32_t>
struct basic_anon_vec {
  static_assert(std::is_trivially_copyable_v<T> &&
                    std::is_trivially_destructible_v<T>,
                "basic_anon_vec: elements are copied and never destroyed");

  using size_type = base_t<Key>;
  using access_type = Key;
  using value_type = T;
  using iterator = T*;

  static constexpr auto kCommitStep = std::size_t{1U} << 27U;  // 128 MiB

  explicit basic_anon_vec(std::size_t const max_bytes)
      : max_bytes_{round_up(max_bytes)} {}

  ~basic_anon_vec() {
    if (addr_ == nullptr) {
      return;
    }
#ifdef _WIN32
    ::VirtualFree(addr_, 0U, MEM_RELEASE);
#else
    ::munmap(addr_, max_bytes_);
#endif
  }

  basic_anon_vec(basic_anon_vec const&) = delete;
  basic_anon_vec& operator=(basic_anon_vec const&) = delete;

  size_type size() const noexcept { return used_size_; }
  T* data() noexcept { return static_cast<T*>(addr_); }

  T* begin() noexcept { return data(); }

  T& operator[](access_type const i) noexcept { return data()[to_idx(i)]; }

  void reserve(size_type const n) {
    commit_up_to(static_cast<std::size_t>(n) * sizeof(T));
  }

  void resize(size_type const n) {
    verify(n >= used_size_, "basic_anon_vec: resize can only grow");
    reserve(n);
    used_size_ = n;
  }

private:
  static std::size_t round_up(std::size_t const bytes) {
    return (bytes + kCommitStep - 1U) / kCommitStep * kCommitStep;
  }

  void commit_up_to(std::size_t const total_bytes) {
    if (total_bytes <= committed_) {
      return;
    }
    if (total_bytes > max_bytes_) {
      throw std::bad_alloc{};
    }
    if (addr_ == nullptr) {
      reserve_address_space();
    }
    auto const target = std::min(round_up(total_bytes), max_bytes_);
    auto* const from = static_cast<std::uint8_t*>(addr_) + committed_;
#ifdef _WIN32
    verify(::VirtualAlloc(from, target - committed_, MEM_COMMIT,
                          PAGE_READWRITE) != nullptr,
           "basic_anon_vec: commit failed");
#else
    verify(::mprotect(from, target - committed_, PROT_READ | PROT_WRITE) == 0,
           "basic_anon_vec: commit failed");
#endif
    committed_ = target;
  }

  void reserve_address_space() {
#ifdef _WIN32
    addr_ = ::VirtualAlloc(nullptr, max_bytes_, MEM_RESERVE, PAGE_NOACCESS);
    verify(addr_ != nullptr, "basic_anon_vec: reserve failed");
#else
    auto flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    auto* const addr = ::mmap(nullptr, max_bytes_, PROT_NONE, flags, -1, 0);
    verify(addr != MAP_FAILED, "basic_anon_vec: reserve failed");
    addr_ = addr;
#endif
  }

  void* addr_{nullptr};
  std::size_t max_bytes_{0U};
  std::size_t committed_{0U};
  size_type used_size_{0U};
};

template <typename T>
using anon_vec = basic_anon_vec<T>;

}  // namespace cista
