#pragma once

#include <gsl/util>

namespace eely {
// Describes different ways of compression appliable to a clip.
enum class clip_compression_scheme {
  // Clip data is not compressed, all data is played as is.
  // Optimized for forward playback.
  none,

  // Clip data is compressed into fixed number of bits.
  // Optimized for forward playback.
  fixed,

  // Clip is compressed using ACL library.
  acl,

  // Clip data is compressed into the same fixed-bit stream as `fixed`,
  // but is additionally equipped with a sparse set of decoding checkpoints,
  // so that arbitrary seeks (including backwards ones) do not require
  // replaying the whole stream from the beginning.
  fixed_seek
};

namespace internal {
static constexpr gsl::index bits_clip_compression_scheme = 2;
}
}  // namespace eely