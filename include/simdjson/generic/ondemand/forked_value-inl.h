#ifndef SIMDJSON_GENERIC_ONDEMAND_FORKED_VALUE_INL_H

#ifndef SIMDJSON_CONDITIONAL_INCLUDE
#define SIMDJSON_GENERIC_ONDEMAND_FORKED_VALUE_INL_H
#include "simdjson/generic/ondemand/base.h"
#include "simdjson/generic/ondemand/forked_value.h"
#include "simdjson/generic/ondemand/json_iterator-inl.h"
#include "simdjson/generic/ondemand/value-inl.h"
#include "simdjson/generic/ondemand/value_iterator-inl.h"
#endif // SIMDJSON_CONDITIONAL_INCLUDE

#include <new>

namespace simdjson {
namespace SIMDJSON_IMPLEMENTATION {
namespace ondemand {

//
// Special members (json_iterator is complete here).
//

simdjson_inline forked_value::forked_value(forked_value &&) noexcept = default;
simdjson_inline forked_value &forked_value::operator=(forked_value &&) noexcept = default;
simdjson_inline forked_value::~forked_value() noexcept = default;

//
// Construction
//

simdjson_warn_unused simdjson_inline simdjson_result<forked_value>
forked_value::make(json_iterator &src, token_position start, depth_t depth) noexcept {
  forked_value owner{};

  // Determine the sub-value boundary by walking only *this* value's structural
  // tokens (allowed: scanning the sub-value's own indexes). This never copies
  // the indexes or reruns stage 1, and it stops exactly at the value's closing
  // token so the fork cannot read into the next sibling.
  token_position fork_end;
  {
    const uint8_t first = *src.peek(start);
    token_position p = start;
    if (first == '{' || first == '[') {
      int open_depth = 0;
      const token_position hard_end = src.end_position();
      do {
        const uint8_t c = *src.peek(p);
        ++p;
        if (c == '{' || c == '[') {
          ++open_depth;
        } else if (c == '}' || c == ']') {
          --open_depth;
        }
      } while (open_depth > 0 && p < hard_end);
      fork_end = p; // one past the matching closing bracket
    } else {
      // A scalar is a single structural token.
      fork_end = start + 1;
    }
  }

  // Upper bound on the sub-value's byte length, used only to size private
  // storage. It is derived from the sub-value span, so it is independent of any
  // unrelated sibling's size. A container spans up to and including its closing
  // bracket; a scalar spans up to the next structural token (which is exactly
  // one index past its root, and always readable thanks to stage 1's sentinel).
  size_t byte_len;
  if (*src.peek(start) == '{' || *src.peek(start) == '[') {
    byte_len = size_t(*(fork_end - 1)) + 1 > size_t(*start)
                   ? size_t(*(fork_end - 1)) + 1 - size_t(*start)
                   : size_t(1);
  } else {
    byte_len = size_t(src.peek_length(start));
    if (byte_len == 0) { byte_len = 1; }
  }

  // Private decode space. Every decoded string produced inside the fork is no
  // larger than its raw JSON span, and those spans are disjoint parts of the
  // sub-value, so byte_len plus SIMD headroom bounds the accumulated output
  // without ever scaling to the whole document.
  const size_t decode_capacity =
      SIMDJSON_ROUNDUP_N(byte_len + 2 * SIMDJSON_PADDING, 64);
  std::unique_ptr<uint8_t[]> decode(new (std::nothrow) uint8_t[decode_capacity]);
  if (!decode) { return MEMALLOC; }

#if SIMDJSON_DEVELOPMENT_CHECKS
  // The deepest container the fork can open is its root plus one level per
  // structural token inside the sub-value: token_count + root_depth + slack.
  const size_t table_capacity =
      size_t(depth) + size_t(fork_end - start) + 2;
  std::unique_ptr<token_position[]> starts(
      new (std::nothrow) token_position[table_capacity]);
  if (!starts) { return MEMALLOC; }
#endif

  // The cursor starts as a private copy of the source cursor (same shared
  // parser/input/indexes), then gets its own position, bounds and storage.
  std::unique_ptr<json_iterator> cursor(
      new (std::nothrow) json_iterator(src));
  if (!cursor) { return MEMALLOC; }

  owner.string_buf = std::move(decode);
  owner.string_capacity = decode_capacity;
#if SIMDJSON_DEVELOPMENT_CHECKS
  owner.start_positions = std::move(starts);
  owner.start_positions_capacity = table_capacity;
#endif

  cursor->_fork_end = fork_end;
  cursor->_fork_root_depth = depth;
  cursor->_owned_string_buf = owner.string_buf.get();
  cursor->_owned_string_capacity = owner.string_capacity;
  cursor->_string_buf_loc = cursor->_owned_string_buf;
#if SIMDJSON_DEVELOPMENT_CHECKS
  cursor->_owned_start_positions = owner.start_positions.get();
  cursor->_owned_start_positions_capacity = owner.start_positions_capacity;
#endif
  // Reposition at the sub-value root with its depth, independent of where the
  // source cursor currently points, and start from a clean error state.
  cursor->_root = start;
  cursor->token.set_position(start);
  cursor->_depth = depth;
  cursor->error = SUCCESS;
  // A fork is always a complete, non-streaming sub-document, even when the
  // source iterator came from document_stream: its boundary comes from our own
  // token scan, not from the streaming batch logic.
  cursor->_streaming = false;
#ifdef SIMDJSON_EXPERIMENTAL_ALLOW_INCOMPLETE_JSON
  cursor->_allow_incomplete_json = false;
#endif

  owner.cursor = std::move(cursor);
  owner.root_position = start;
  owner.root_depth = depth;
  return owner;
}

//
// Public API
//

simdjson_inline simdjson_result<value> forked_value::get_value() noexcept {
  if (!cursor) { return UNINITIALIZED; }
  if (taken) { return OUT_OF_ORDER_ITERATION; }
  taken = true;
  return value(value_iterator(cursor.get(), root_depth, root_position));
}

} // namespace ondemand
} // namespace SIMDJSON_IMPLEMENTATION
} // namespace simdjson

namespace simdjson {

simdjson_inline simdjson_result<SIMDJSON_IMPLEMENTATION::ondemand::forked_value>::simdjson_result(
  SIMDJSON_IMPLEMENTATION::ondemand::forked_value &&value
) noexcept :
    implementation_simdjson_result_base<SIMDJSON_IMPLEMENTATION::ondemand::forked_value>(
      std::forward<SIMDJSON_IMPLEMENTATION::ondemand::forked_value>(value)
    )
{
}
simdjson_inline simdjson_result<SIMDJSON_IMPLEMENTATION::ondemand::forked_value>::simdjson_result(
  error_code error
) noexcept :
    implementation_simdjson_result_base<SIMDJSON_IMPLEMENTATION::ondemand::forked_value>(error)
{
}

simdjson_inline simdjson_result<SIMDJSON_IMPLEMENTATION::ondemand::value>
simdjson_result<SIMDJSON_IMPLEMENTATION::ondemand::forked_value>::get_value() noexcept {
  if (error()) { return error(); }
  return first.get_value();
}

} // namespace simdjson

#endif // SIMDJSON_GENERIC_ONDEMAND_FORKED_VALUE_INL_H
