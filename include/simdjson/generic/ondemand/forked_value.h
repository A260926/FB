#ifndef SIMDJSON_GENERIC_ONDEMAND_FORKED_VALUE_H

#ifndef SIMDJSON_CONDITIONAL_INCLUDE
#define SIMDJSON_GENERIC_ONDEMAND_FORKED_VALUE_H
#include "simdjson/generic/ondemand/base.h"
#include "simdjson/generic/implementation_simdjson_result_base.h"
#endif // SIMDJSON_CONDITIONAL_INCLUDE

#include <memory>
#include <cstddef>

namespace simdjson {
namespace SIMDJSON_IMPLEMENTATION {
namespace ondemand {

class value;
class json_iterator;

/**
 * An independent, owner-side fork of an ondemand::value.
 *
 * A forked_value is produced by value::fork() while the source value is still
 * unconsumed. It owns a private iteration cursor (its own position, depth and
 * error state), a private string decode buffer and, when
 * SIMDJSON_DEVELOPMENT_CHECKS is enabled, its own container-start bookkeeping.
 * The read-only structural indexes, the input buffer and the parser remain
 * shared with the original document and are never copied.
 *
 * The fork only ever sees the sub-value it was created from: its token range is
 * bounded by a scan of that sub-value, so iteration can never run into the next
 * sibling. Several forks of the same value, and the parent iteration itself,
 * may be consumed in any interleaving order; a mistake inside one fork (a
 * missing field, a type error or a lazily detected format error) only poisons
 * that fork's own cursor and error state.
 *
 * forked_value is move-only. The cursor is kept in a stable, heap-held object:
 * moving the owner never relocates it, so every value/array/object/string_view
 * already handed out stays valid after the move. A default-constructed
 * instance holds no fork.
 *
 * Lifetime contract (the fork does NOT extend any of these):
 *   - The original input buffer, the ondemand::parser and the ondemand::document
 *     must outlive every forked_value, and every value/array/object/string_view
 *     obtained from one.
 *   - While any fork is alive you must not call parser.iterate() again, nor
 *     rewind()/move the original document or parser.
 *   - The parent path may keep iterating forward normally.
 *   - A nested fork only borrows the original input and structural indexes:
 *     once it has been created, the outer forked_value may be destroyed first,
 *     but the original parser, document and input must still be alive.
 *
 * Forks are single-threaded: interleaving is supported within one thread only.
 */
class forked_value {
public:
  /**
   * Create an empty owner that holds no fork. get_value() on it fails with
   * UNINITIALIZED.
   */
  simdjson_inline forked_value() noexcept = default;

  /** Move construction: views handed out before the move stay valid. */
  simdjson_inline forked_value(forked_value &&other) noexcept;
  /** Move assignment: views handed out before the move stay valid. */
  simdjson_inline forked_value &operator=(forked_value &&other) noexcept;

  /** Forks own mutable decode state and cannot be copied. */
  forked_value(const forked_value &) = delete;
  forked_value &operator=(const forked_value &) = delete;

  /** Defined in forked_value-inl.h, where json_iterator is complete. */
  simdjson_inline ~forked_value() noexcept;

  /**
   * Obtain the root value of this fork.
   *
   * The returned ondemand::value supports the usual get_object(), get_array(),
   * scalar reads, field access and may itself be forked again. It remains valid
   * for as long as this owner is alive, including after the owner has been
   * moved.
   *
   * get_value() may be called only once per forked_value: a second call returns
   * OUT_OF_ORDER_ITERATION.
   *
   * @returns The root value, or:
   *          - OUT_OF_ORDER_ITERATION if a value was already obtained,
   *          - UNINITIALIZED on a default-constructed (empty) owner.
   */
  simdjson_warn_unused simdjson_inline simdjson_result<value> get_value() noexcept;

private:
  /**
   * Heap-held cursor. Kept off the owner object so its address (referenced by
   * every value/object/array derived from the fork) is stable across moves of
   * the owner.
   */
  std::unique_ptr<json_iterator> cursor{};
  /** Structural index at which the forked sub-value starts. */
  token_position root_position{};
  /** Depth the forked sub-value lives at. */
  depth_t root_depth{};
  bool taken{false};

  // Private decode space, sized from the forked sub-value only so it never
  // scales with unrelated siblings. The cursor references this non-owningly.
  std::unique_ptr<uint8_t[]> string_buf{};
  size_t string_capacity{0};
#if SIMDJSON_DEVELOPMENT_CHECKS
  // Private per-depth container-start table, mirroring the parser's own
  // start_positions so the development checks stay fork-local.
  std::unique_ptr<token_position[]> start_positions{};
  size_t start_positions_capacity{0};
#endif

  /** Build a fork of the value currently at `src`/`start`/`depth`. */
  simdjson_warn_unused static simdjson_inline simdjson_result<forked_value>
  make(json_iterator &src, token_position start, depth_t depth) noexcept;

  friend class value;
  friend class json_iterator;
};

} // namespace ondemand
} // namespace SIMDJSON_IMPLEMENTATION
} // namespace simdjson

namespace simdjson {

template<>
struct simdjson_result<SIMDJSON_IMPLEMENTATION::ondemand::forked_value>
    : public SIMDJSON_IMPLEMENTATION::implementation_simdjson_result_base<
          SIMDJSON_IMPLEMENTATION::ondemand::forked_value> {
public:
  simdjson_inline simdjson_result(SIMDJSON_IMPLEMENTATION::ondemand::forked_value &&value) noexcept; ///< @private
  simdjson_inline simdjson_result(error_code error) noexcept; ///< @private
  simdjson_inline simdjson_result() noexcept {}

  /** @copydoc SIMDJSON_IMPLEMENTATION::ondemand::forked_value::get_value */
  simdjson_warn_unused simdjson_inline
      simdjson_result<SIMDJSON_IMPLEMENTATION::ondemand::value>
      get_value() noexcept;
};

} // namespace simdjson

#endif // SIMDJSON_GENERIC_ONDEMAND_FORKED_VALUE_H
