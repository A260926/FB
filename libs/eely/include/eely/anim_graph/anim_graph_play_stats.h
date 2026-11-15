#pragma once

#include <cstddef>

namespace eely {
// Statistics collected during a single `anim_graph_player::play` call.
// Values are only committed after a play finishes successfully:
// if a play throws, stats from the last successful play are preserved.
struct anim_graph_play_stats final {
  // Number of distinct pose nodes (clip, blend, sum) that were actually
  // evaluated during the play. A pose node shared by several branches
  // is counted once.
  std::size_t evaluated_pose_nodes{0};

  // Number of jobs that were executed by the job queue.
  // Every unique job is executed at most once.
  std::size_t executed_jobs{0};

  // Number of times a pose node result was reused without recomputation
  // (always zero in non-shared mode).
  std::size_t cache_hits{0};

  // Maximum number of pose pool leases (live poses) held simultaneously
  // while executing the job queue.
  std::size_t peak_live_poses{0};
};
}  // namespace eely
