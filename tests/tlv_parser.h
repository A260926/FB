#pragma once

// Bounded-length TLV (type-length-value) parser material for the isolated
// checking contract tests. This material is self-contained: it only ever
// operates on its own byte buffers and never reads external files.
//
// Format: each node is [type:1][length:2 little-endian][payload:length].
// A type with the high bit (0x80) set denotes a container whose payload is
// itself a sequence of nodes; any other type is a leaf with raw payload
// bytes. The document is a sequence of top-level nodes filling the whole
// buffer. Inputs are bounded (kMaxInputBytes) and so is the nesting depth
// (kMaxDepth).
//
// Two parser versions share the exact same detection walk:
//  - parseV1 (legacy): treats a nested declared length exceeding the
//    remaining bytes of its parent container as an impossible condition and
//    triggers an explicit bounds-guard abort (SIGABRT).
//  - parseV2 (fixed): returns a format error at the very same position.
// An independent checker (firstBoundaryViolation) separately computes the
// first position where a declared length crosses its parent container
// boundary.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace tlv {

constexpr std::size_t kMaxInputBytes = 4096;
constexpr int kMaxDepth = 16;
constexpr std::size_t kHeaderSize = 3;

/// Returned by firstBoundaryViolation when no declared length crosses its
/// parent container boundary.
constexpr std::size_t kNoViolation = ~static_cast<std::size_t>(0);

inline bool isContainer(std::uint8_t type) { return (type & 0x80u) != 0; }

struct ParseReport {
  enum class Status { Ok, FormatError };

  Status status = Status::Ok;
  /// Offset of the first problem, when status is FormatError.
  std::size_t offset = 0;
  std::string message;
};

namespace detail {

enum class WalkKind { Ok, Violation, FormatError };

struct WalkOutcome {
  WalkKind kind;
  std::size_t offset;
  std::string message;
};

/// Shared detection walk over the region [begin, end), depth-first in
/// document order. A "violation" is a node whose declared payload length
/// exceeds the remaining bytes of its parent container (the region).
inline WalkOutcome walkRegion(const std::vector<std::uint8_t> &buffer,
                              std::size_t begin,
                              std::size_t end,
                              int depth) {
  std::size_t pos = begin;
  while (pos < end) {
    if (end - pos < kHeaderSize) {
      return {WalkKind::FormatError, pos, "truncated node header"};
    }
    if (depth > kMaxDepth) {
      return {WalkKind::FormatError, pos, "nesting depth exceeded"};
    }

    const std::uint8_t type = buffer[pos];
    const std::uint32_t length = static_cast<std::uint32_t>(buffer[pos + 1]) |
        (static_cast<std::uint32_t>(buffer[pos + 2]) << 8);
    const std::size_t remaining = end - pos - kHeaderSize;
    if (length > remaining) {
      return {WalkKind::Violation,
              pos,
              "nested length exceeds parent container remaining bytes"};
    }

    if (isContainer(type)) {
      // The region passed here is always contained in the parent's region,
      // which is inductively contained in the buffer, so the walk never
      // reads out of bounds.
      const auto sub = walkRegion(
          buffer, pos + kHeaderSize, pos + kHeaderSize + length, depth + 1);
      if (sub.kind != WalkKind::Ok) {
        return sub;
      }
    }
    pos += kHeaderSize + length;
  }
  return {WalkKind::Ok, 0, ""};
}

inline WalkOutcome walkDocument(const std::vector<std::uint8_t> &buffer) {
  if (buffer.size() > kMaxInputBytes) {
    return {WalkKind::FormatError, 0, "input exceeds maximum input length"};
  }
  return walkRegion(buffer, 0, buffer.size(), 1);
}

} // namespace detail

/// Legacy parser version: the boundary guard aborts the process when a
/// nested declared length exceeds the remaining bytes of its parent
/// container. Ordinary format problems are reported normally.
inline ParseReport parseV1(const std::vector<std::uint8_t> &buffer) {
  const auto outcome = detail::walkDocument(buffer);
  if (outcome.kind == detail::WalkKind::Violation) {
    // Explicit bounds guard of the old version: the condition was considered
    // impossible by construction, so the old code aborts (SIGABRT).
    std::abort();
  }

  ParseReport report;
  report.status = outcome.kind == detail::WalkKind::Ok
      ? ParseReport::Status::Ok
      : ParseReport::Status::FormatError;
  report.offset = outcome.offset;
  report.message = outcome.message;
  return report;
}

/// Fixed parser version: the same boundary condition is a graceful format
/// error reported at the same position.
inline ParseReport parseV2(const std::vector<std::uint8_t> &buffer) {
  const auto outcome = detail::walkDocument(buffer);
  ParseReport report;
  report.status = outcome.kind == detail::WalkKind::Ok
      ? ParseReport::Status::Ok
      : ParseReport::Status::FormatError;
  report.offset = outcome.offset;
  report.message = outcome.message;
  return report;
}

/// Independent checker: returns the offset of the first node whose declared
/// payload length crosses the remaining bytes of its parent container, in
/// the same depth-first document order the parsers use, or kNoViolation.
/// Implemented separately from the parser walk (iterative, explicit stack).
inline std::size_t firstBoundaryViolation(
    const std::vector<std::uint8_t> &buffer) {
  if (buffer.size() > kMaxInputBytes) {
    return kNoViolation;
  }

  struct Frame {
    std::size_t pos;
    std::size_t end;
    int depth;
  };

  std::vector<Frame> stack;
  stack.push_back({0, buffer.size(), 1});
  while (!stack.empty()) {
    Frame &frame = stack.back();
    if (frame.pos >= frame.end) {
      stack.pop_back();
      continue;
    }
    if (frame.end - frame.pos < kHeaderSize) {
      return kNoViolation; // ordinary format error stops the walk
    }
    if (frame.depth > kMaxDepth) {
      return kNoViolation; // ordinary format error stops the walk
    }

    const std::size_t pos = frame.pos;
    const std::uint8_t type = buffer[pos];
    const std::uint32_t length =
        static_cast<std::uint32_t>(buffer[pos + 1]) |
        (static_cast<std::uint32_t>(buffer[pos + 2]) << 8);
    const std::size_t remaining = frame.end - pos - kHeaderSize;
    if (length > remaining) {
      return pos;
    }

    frame.pos += kHeaderSize + length;
    if (isContainer(type)) {
      stack.push_back({pos + kHeaderSize, pos + kHeaderSize + length,
                       frame.depth + 1});
    }
  }
  return kNoViolation;
}

} // namespace tlv
