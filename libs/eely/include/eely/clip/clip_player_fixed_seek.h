#pragma once

#include "eely/clip/clip_cursor.h"
#include "eely/clip/clip_impl_fixed_seek.h"
#include "eely/clip/clip_player_base.h"
#include "eely/skeleton/skeleton_pose.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace eely::internal {
// Counters of real work performed by `clip_player_fixed_seek`.
struct fixed_seek_stats final {
  // Number of compressed records actually decoded since construction
  // (across all `play` calls).
  size_t decoded_records{0};

  // Number of checkpoint time comparisons performed since construction.
  size_t checkpoint_comparisons{0};

  // Number of times a checkpoint was restored since construction.
  size_t restored_checkpoints{0};
};

// Read-only description of the seek index cooked into a clip,
// useful for verifying the documented size bounds.
struct fixed_seek_layout final {
  // Number of cooked records (`P`): each header together with its
  // time/TRS payload counts as one.
  size_t encoded_records{0};

  // Number of animated component cursors (`C`),
  // i.e. rest-pose channels removed, at most three per joint.
  size_t component_count{0};

  // Number of serialized checkpoints (`N`).
  size_t checkpoint_count{0};
};

// Player for clips compressed with `clip_compression_scheme::fixed_seek`.
//
// The cooked clip owns and shares the read-only quantized stream and the
// checkpoint index; each player keeps its own cursor and seek statistics.
// Two players over the same clip never share mutable state, so interleaved
// jumps of one player do not affect the other.
//
// After construction, `play` does not allocate heap memory.
class clip_player_fixed_seek final : public clip_player_base {
public:
  clip_player_fixed_seek(const clip_metadata_fixed_seek& metadata,
                         std::span<const uint16_t> data,
                         std::span<const fixed_seek_checkpoint> checkpoints,
                         uint32_t encoded_records);

  [[nodiscard]] float get_duration_s() override;

  void play(float time_s, skeleton_pose& out_pose) override;

  // Return counters of real decoding work since construction.
  [[nodiscard]] fixed_seek_stats get_last_seek_stats() const;

  // Return read-only description of the underlying seek index.
  [[nodiscard]] fixed_seek_layout get_seek_layout() const;

private:
  const clip_metadata_fixed_seek& _metadata;
  const std::span<const uint16_t> _data;
  const std::span<const fixed_seek_checkpoint> _checkpoints;
  const uint32_t _encoded_records;

  cursor _cursor;

  fixed_seek_stats _stats;
};
}  // namespace eely::internal
