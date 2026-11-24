#pragma once

// Read-only validation of native-endian cista offset object graphs.
//
// A validation_session is constructed from the memory regions the caller
// authorizes for reading and a per-call object expansion budget.  validate()
// walks the static-type graph reachable from a root with the same reflection
// and container state rules as serialization.h/deserialization and proves that
// every byte the graph can read stays inside one authorized region: a single
// object or container always has to fit into one region completely, and gaps
// between regions are never treated as readable memory.
//
// The walk is strictly read-only: offsets are decoded from their storage
// values, deserialize() is never used for pointer or endian conversion, and no
// pointer outside an authorized region is ever materialized.
//
// Successfully validated roots together with their full dependency closures
// are remembered as proofs, keyed by static type and object start address.
// Later calls reuse them: a hit on a committed object counts as reused and is
// never expanded again.  Proofs are committed only when the whole closure
// validates; on failure every counter reads zero and committed_objects() is
// unchanged.  The caller must keep every region's address and bytes unchanged
// for the lifetime of the session, or call clear() before modifying them.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include "cista/containers.h"
#include "cista/containers/offset_ptr.h"
#include "cista/decay.h"
#include "cista/indexed.h"
#include "cista/offset_t.h"
#include "cista/reflection/for_each_field.h"
#include "cista/serialized_size.h"
#include "cista/strong.h"
#include "cista/type_traits.h"

namespace cista {

enum class validation_error {
  none,
  invalid_regions,
  outside_region,
  invalid_state,
  budget_exhausted
};

struct validation_region {
  std::uint64_t id;
  const void* data;
  std::size_t size;
};

struct validation_report {
  validation_error error{validation_error::none};
  std::uint64_t region_id{0};
  std::size_t offset{0};
  std::size_t expanded{0};
  std::size_t reused{0};
};

namespace validation_detail {

struct region_info {
  std::uint64_t id;
  std::uintptr_t begin;
  std::uintptr_t end;
};

// Proofs are distinguished by static type and object start, exactly as the
// session semantics require.
struct object_key {
  void const* address;
  std::type_info const* type;

  bool operator<(object_key const& o) const noexcept {
    if (address != o.address) {
      return address < o.address;
    }
    return type->before(*o.type);
  }
};

template <typename>
struct is_offset_ptr_helper : std::false_type {};

template <typename T>
struct is_offset_ptr_helper<offset_ptr<T>> : std::true_type {};

template <typename T>
constexpr bool is_offset_ptr_v =
    is_offset_ptr_helper<std::remove_cv_t<T>>::value;

template <typename>
struct is_vector_helper : std::false_type {};

template <typename T, template <typename> typename Ptr, bool Indexed,
          typename SizeType>
struct is_vector_helper<basic_vector<T, Ptr, Indexed, SizeType>>
    : std::true_type {};

template <typename T>
constexpr bool is_vector_v =
    is_vector_helper<std::remove_cv_t<T>>::value;

template <typename>
struct is_unique_ptr_helper : std::false_type {};

template <typename T, typename Ptr>
struct is_unique_ptr_helper<basic_unique_ptr<T, Ptr>> : std::true_type {};

template <typename T>
constexpr bool is_unique_ptr_v =
    is_unique_ptr_helper<std::remove_cv_t<T>>::value;

template <typename>
struct is_variant_helper : std::false_type {};

template <typename... Ts>
struct is_variant_helper<variant<Ts...>> : std::true_type {};

template <typename T>
constexpr bool is_variant_v =
    is_variant_helper<std::remove_cv_t<T>>::value;

template <typename>
struct is_optional_helper : std::false_type {};

template <typename T>
struct is_optional_helper<optional<T>> : std::true_type {};

template <typename T>
constexpr bool is_optional_v =
    is_optional_helper<std::remove_cv_t<T>>::value;

// Read the raw offset value.  Resolving it through pointer arithmetic outside
// an authorized region would be undefined behavior, so validation only works
// with the stored integer until the target range is proven.
inline offset_t read_offset(void const* slot) noexcept {
  offset_t value;
  std::memcpy(&value, slot, sizeof(value));
  return value;
}

inline bool is_valid_bool_byte(bool const& b) noexcept {
  auto const byte = *reinterpret_cast<std::uint8_t const*>(&b);
  return byte == 0U || byte == 1U;
}

template <typename T>
inline bool mul_would_overflow(std::size_t const count) noexcept {
  return count != 0U &&
         count > std::numeric_limits<std::size_t>::max() / sizeof(T);
}

// Same field expansion serialization.h's for_each_ptr_field performs, but
// const-correct: validation never writes.
template <typename T, typename Fn>
void for_each_reflected_field(T const& t, Fn&& fn) {
  if constexpr (std::is_scalar_v<T>) {
    fn(t);
  } else {
    std::apply([&](auto&&... args) { (fn(args), ...); }, to_tuple(t));
  }
}

}  // namespace validation_detail

class validation_session {
public:
  using region = validation_region;

  validation_session(std::vector<region> regions, std::size_t budget)
      : budget_{budget} {
    std::set<std::uint64_t> ids;
    std::vector<validation_detail::region_info> parsed;

    for (auto const& r : regions) {
      if (r.data == nullptr || r.size == 0U || !ids.insert(r.id).second) {
        return;  // Empty descriptor or duplicate id.
      }
      auto const begin = reinterpret_cast<std::uintptr_t>(r.data);
      if (begin > std::numeric_limits<std::uintptr_t>::max() - r.size) {
        return;  // address + size would overflow.
      }
      parsed.push_back(
          validation_detail::region_info{r.id, begin, begin + r.size});
    }

    for (std::size_t i = 0U; i < parsed.size(); ++i) {
      for (std::size_t j = i + 1U; j < parsed.size(); ++j) {
        if (parsed[i].begin < parsed[j].end &&
            parsed[j].begin < parsed[i].end) {
          return;  // Overlapping descriptors.
        }
      }
    }

    regions_ = std::move(parsed);
    valid_regions_ = true;
  }

  void set_budget(std::size_t const budget) noexcept { budget_ = budget; }

  void clear() noexcept { proofs_.clear(); }

  std::size_t committed_objects() const noexcept { return proofs_.size(); }

  template <typename T>
  validation_report validate(T const* root) {
    validation_report report;

    if (!valid_regions_) {
      report.error = validation_error::invalid_regions;
      return report;
    }

    validator v{*this, report};

    // An unauthorized root is reported at the reserved coordinate {0, 0}.
    if (!v.check_contiguous(root, sizeof(decay_t<T>), alignof(decay_t<T>),
                            nullptr)) {
      return report;
    }

    auto const root_key = v.make_key(root);
    if (proofs_.find(root_key) != proofs_.end()) {
      // The whole root is already a committed proof.
      report.reused = 1U;
      return report;
    }

    if (!v.visit_chargeable(root, nullptr)) {
      // The walk failed before completing the closure.  Proofs are only
      // committed on full success, so prior proofs are intact and the failure
      // counters read zero.
      report.expanded = 0U;
      report.reused = 0U;
      return report;
    }

    for (auto const& key : v.charged_) {
      proofs_.insert(key);
    }
    return report;
  }

private:
  struct validator {
    explicit validator(validation_session& session, validation_report& report)
        : session_{session}, report_{report} {}

    template <typename T>
    validation_detail::object_key make_key(T const* p) const noexcept {
      return {static_cast<void const*>(p), &typeid(decay_t<T>)};
    }

    // Prove that the complete, aligned storage [ptr, ptr + size) lies within
    // a single authorized region.  On failure the coordinate points at the
    // source reference slot; a null source (the root) gives {0, 0}.
    bool check_contiguous(void const* ptr, std::size_t const size,
                          std::size_t const alignment,
                          void const* source) noexcept {
      auto const address = reinterpret_cast<std::uintptr_t>(ptr);
      if (address == 0U || size == 0U ||
          (alignment != 0U && (address & (alignment - 1U)) != 0U) ||
          address > std::numeric_limits<std::uintptr_t>::max() - size) {
        return fail(source, validation_error::outside_region);
      }
      auto const end = address + size;
      for (auto const& r : session_.regions_) {
        if (address >= r.begin && end <= r.end) {
          return true;
        }
      }
      return fail(source, validation_error::outside_region);
    }

    bool fail(void const* source, validation_error const error) noexcept {
      report_.error = error;
      if (source == nullptr) {
        report_.region_id = 0U;
        report_.offset = 0U;
        return false;
      }
      auto const slot = reinterpret_cast<std::uintptr_t>(source);
      for (auto const& r : session_.regions_) {
        if (slot >= r.begin && slot < r.end) {
          report_.region_id = r.id;
          report_.offset = slot - r.begin;
          return false;
        }
      }
      report_.region_id = 0U;
      report_.offset = 0U;
      return false;
    }

    bool state_fail(void const* where) noexcept {
      return fail(where, validation_error::invalid_state);
    }

    // Entry point for a root or a non-null pointer target.  A target that is
    // already a committed proof or has already been encountered in this walk
    // (including embedded storage) is a reuse hit: it is counted and never
    // walked again.  Every other object consumes one expansion slot and
    // becomes a proof identity.  A reference target always gets an identity,
    // even when its static type is a scalar.
    template <typename T>
    bool visit_chargeable(T const* el, void const* source) noexcept {
      auto const key = make_key(el);
      if (session_.proofs_.find(key) != session_.proofs_.end() ||
          seen_.find(key) != seen_.end()) {
        ++report_.reused;
        return true;
      }
      if (report_.expanded >= session_.budget_) {
        return fail(source, validation_error::budget_exhausted);
      }
      ++report_.expanded;
      seen_.insert(key);
      charged_.insert(key);
      if constexpr (is_indexed_v<T>) {
        // The indexed wrapper shares the storage of its value type.
        return visit(static_cast<typename T::value_type const*>(el));
      } else {
        return dispatch(el);
      }
    }

    // Entry point for embedded storage (aggregate fields, container
    // elements).  Embedded objects never consume the expansion budget and
    // never become proof identities, but they still participate in traversal
    // dedup so shared inline storage is walked once.  Scalar leaves and
    // pointer slots carry no object identity.
    template <typename T>
    bool visit(T const* el) noexcept {
      using Type = decay_t<T>;
      if constexpr (is_indexed_v<Type>) {
        // The indexed wrapper shares the storage of its value type.
        return visit(static_cast<typename Type::value_type const*>(el));
      } else if constexpr (std::is_scalar_v<Type> ||
                           validation_detail::is_offset_ptr_v<Type> ||
                           std::is_pointer_v<Type>) {
        return dispatch(el);
      } else {
        auto const key = make_key(el);
        if (session_.proofs_.find(key) != session_.proofs_.end() ||
            seen_.find(key) != seen_.end()) {
          return true;
        }
        seen_.insert(key);
        return dispatch(el);
      }
    }

    template <typename T>
    bool dispatch(T const* el) noexcept {
      using Type = decay_t<T>;

      if constexpr (validation_detail::is_offset_ptr_v<Type>) {
        return visit_offset_pointer(el);
      } else if constexpr (std::is_pointer_v<Type>) {
        return visit_raw_pointer(el);
      } else if constexpr (is_string_v<Type>) {
        return visit_string(el);
      } else if constexpr (validation_detail::is_vector_v<Type>) {
        return visit_vector(el);
      } else if constexpr (validation_detail::is_unique_ptr_v<Type>) {
        return visit_unique_ptr(el);
      } else if constexpr (validation_detail::is_optional_v<Type>) {
        return visit_optional(el);
      } else if constexpr (validation_detail::is_variant_v<Type>) {
        return visit_variant(el);
      } else if constexpr (is_tuple_v<Type>) {
        bool ok = true;
        auto const check = [&](auto const& member) {
          if (ok) {
            ok = visit(std::addressof(member));
          }
        };
        cista::apply([&](auto const&... member) { (check(member), ...); },
                     *el);
        return ok;
      } else if constexpr (is_strong_v<Type>) {
        return visit(std::addressof(el->v_));
      } else if constexpr (to_tuple_works_v<Type>) {
        // Reflectable aggregate or cista_members() type: declaration-order
        // field traversal, identical to the serialization walk.
        bool ok = true;
        validation_detail::for_each_reflected_field(
            *el, [&](auto const& member) {
              if (ok) {
                ok = visit(std::addressof(member));
              }
            });
        return ok;
      } else if constexpr (std::is_scalar_v<Type>) {
        // Its bytes were authorized together with the enclosing object.
        return true;
      } else {
        static_assert(std::is_same_v<Type, Type> && false,
                      "type is not supported by validation_session");
      }
    }

    // --- offset pointers ---------------------------------------------------
    template <typename T>
    bool visit_offset_pointer(offset_ptr<T> const* slot) noexcept {
      auto const raw = validation_detail::read_offset(slot);
      if (raw == NULLPTR_OFFSET) {
        return true;  // Null references need no target.
      }

      auto const slot_addr = reinterpret_cast<std::uintptr_t>(slot);
      std::uintptr_t target_addr = 0U;
      if (raw >= 0) {
        auto const forward = static_cast<std::uintptr_t>(raw);
        if (slot_addr > std::numeric_limits<std::uintptr_t>::max() - forward) {
          return fail(slot, validation_error::outside_region);
        }
        target_addr = slot_addr + forward;
      } else {
        // Magnitude of the negative offset computed in unsigned arithmetic,
        // so the most negative offset_t never triggers signed negation UB.
        auto const backward = static_cast<std::uintptr_t>(~raw) + 1U;
        if (slot_addr < backward) {
          return fail(slot, validation_error::outside_region);
        }
        target_addr = slot_addr - backward;
      }

      using target_type = decay_t<T>;
      if constexpr (std::is_same_v<target_type, void>) {
        // A void target has no static layout; prove the pointed-to byte is
        // authorized.  It cannot be expanded.
        return check_contiguous(reinterpret_cast<void const*>(target_addr), 1U,
                                1U, slot);
      } else {
        T const* target = nullptr;
        std::memcpy(&target, &target_addr, sizeof(target));
        if (!check_contiguous(target, sizeof(target_type),
                              alignof(target_type), slot)) {
          return false;
        }
        // Shared targets and cycle back-edges count as reused and stop here.
        return visit_chargeable(target, slot);
      }
    }

    // Raw pointer graphs are out of scope: a non-null raw pointer cannot be
    // resolved against offset-based authorization.
    template <typename T>
    bool visit_raw_pointer(T* const* slot) noexcept {
      if (*slot == nullptr) {
        return true;
      }
      return state_fail(slot);
    }

    // --- vectors -----------------------------------------------------------
    template <typename T, template <typename> typename Ptr, bool Indexed,
              typename SizeType>
    bool visit_vector(
        basic_vector<T, Ptr, Indexed, SizeType> const* el) noexcept {
      static_assert(
          validation_detail::is_offset_ptr_v<Ptr<T>>,
          "only native-endian offset vectors are supported");

      // State rules mirror check_state() for basic_vector.
      if (!validation_detail::is_valid_bool_byte(el->self_allocated_)) {
        return state_fail(&el->self_allocated_);
      }
      if (el->self_allocated_) {
        return state_fail(&el->self_allocated_);
      }
      if (el->allocated_size_ != el->used_size_) {
        return state_fail(&el->allocated_size_);
      }
      if ((el->used_size_ == 0U) != (el->el_ == nullptr)) {
        return state_fail(&el->el_);
      }
      if (validation_detail::mul_would_overflow<T>(
              static_cast<std::size_t>(el->allocated_size_))) {
        return state_fail(&el->el_);
      }
      if (el->el_ == nullptr) {
        return true;
      }

      // Arithmetic and the complete element block are proven before the
      // container is traversed, and the block has to fit one region.
      auto const count = static_cast<std::size_t>(el->used_size_);
      if (!check_contiguous(static_cast<T const*>(el->el_),
                            count * sizeof(T), alignof(T), &el->el_)) {
        return false;
      }

      // Embedded elements are not separate expansions; visit them in order.
      bool ok = true;
      for (std::size_t i = 0U; i < count && ok; ++i) {
        ok = visit(std::addressof(el->el_[i]));
      }
      return ok;
    }

    // --- strings -----------------------------------------------------------
    template <typename Ptr>
    bool visit_string(generic_string<Ptr> const* el) noexcept {
      static_assert(validation_detail::is_offset_ptr_v<Ptr>,
                    "only native-endian offset strings are supported");
      if (!validation_detail::is_valid_bool_byte(el->s_.is_short_)) {
        return state_fail(&el->s_.is_short_);
      }
      if (el->is_short()) {
        return true;  // Inline bytes are part of the string object.
      }
      if (!validation_detail::is_valid_bool_byte(el->h_.self_allocated_)) {
        return state_fail(&el->h_.self_allocated_);
      }
      if (el->h_.self_allocated_) {
        return state_fail(&el->h_.self_allocated_);
      }
      using char_t = typename generic_string<Ptr>::CharT;
      if (validation_detail::mul_would_overflow<char_t>(el->h_.size_)) {
        return state_fail(&el->h_.ptr_);
      }
      if ((el->h_.size_ == 0U) != (el->h_.ptr_ == nullptr)) {
        return state_fail(&el->h_.ptr_);
      }
      if (el->h_.ptr_ == nullptr) {
        return true;
      }
      return check_contiguous(el->data(),
                              static_cast<std::size_t>(el->h_.size_) *
                                  sizeof(char_t),
                              alignof(char_t), &el->h_.ptr_);
    }

    template <typename Ptr>
    bool visit_string(generic_cstring<Ptr> const* el) noexcept {
      static_assert(validation_detail::is_offset_ptr_v<Ptr>,
                    "only native-endian offset cstrings are supported");
      if (el->is_short()) {
        return true;
      }
      if (!validation_detail::is_valid_bool_byte(el->h_.self_allocated_)) {
        return state_fail(&el->h_.self_allocated_);
      }
      if (el->h_.self_allocated_) {
        return state_fail(&el->h_.self_allocated_);
      }
      if (el->h_.minus_one_ != -1) {
        return state_fail(&el->h_.minus_one_);
      }
      if (el->h_.ptr_ == nullptr) {
        return state_fail(&el->h_.ptr_);
      }
      // The payload plus the terminating NUL byte is one continuous block.
      auto const bytes = static_cast<std::size_t>(el->h_.size_) + 1U;
      return check_contiguous(el->data(), bytes, alignof(char), &el->h_.ptr_);
    }

    // --- unique_ptr --------------------------------------------------------
    template <typename T, typename Ptr>
    bool visit_unique_ptr(basic_unique_ptr<T, Ptr> const* el) noexcept {
      static_assert(validation_detail::is_offset_ptr_v<Ptr>,
                    "only native-endian offset unique_ptr is supported");
      if (!validation_detail::is_valid_bool_byte(el->self_allocated_)) {
        return state_fail(&el->self_allocated_);
      }
      if (el->self_allocated_) {
        return state_fail(&el->self_allocated_);
      }
      if (el->el_ == nullptr) {
        return true;
      }
      auto const target = static_cast<T const*>(el->el_);
      if (!check_contiguous(target, sizeof(T), alignof(T), &el->el_)) {
        return false;
      }
      return visit_chargeable(target, &el->el_);
    }

    // --- optional ----------------------------------------------------------
    template <typename T>
    bool visit_optional(optional<T> const* el) noexcept {
      if (!validation_detail::is_valid_bool_byte(el->valid_)) {
        return state_fail(&el->valid_);
      }
      if (!el->valid_) {
        return true;
      }
      return visit(std::addressof(el->value()));
    }

    // --- variant -----------------------------------------------------------
    template <typename... Ts>
    bool visit_variant(variant<Ts...> const* el) noexcept {
      if (el->index() >= sizeof...(Ts)) {
        return state_fail(&el->idx_);
      }
      bool ok = true;
      el->apply([&](auto const& active) {
        if (ok) {
          ok = visit(std::addressof(active));
        }
      });
      return ok;
    }

    using visited_set = std::set<validation_detail::object_key>;

    validation_session& session_;
    validation_report& report_;
    // Traversal dedup: every object dispatched once, including embedded
    // storage, so back-edges and shared inline storage stop the walk.
    visited_set seen_;
    // Roots and non-null reference targets only.  These consume the budget
    // and become the committed proof set on full success.
    visited_set charged_;
  };

  std::size_t budget_;
  bool valid_regions_{false};
  std::vector<validation_detail::region_info> regions_;
  std::set<validation_detail::object_key> proofs_;
};

}  // namespace cista
