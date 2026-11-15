#pragma once

#include "eely/skeleton/skeleton.h"
#include "eely/skeleton/skeleton_pose.h"

#include <gsl/util>

#include <memory>
#include <vector>

namespace eely::internal {
// Pool for skeleton poses constructed for specific skeleton,
// used when executing job queue.
class skeleton_pose_pool final {
public:
  // Deleter for `unique_ptr` that returns pose back to the pool it was taken from.
  // Should never outlive the pool.
  struct deleter final {
    explicit deleter() = default;
    explicit deleter(skeleton_pose_pool& pool);

    void operator()(skeleton_pose* ptr);

  private:
    skeleton_pose_pool* _pool{nullptr};
  };

  using ptr = std::unique_ptr<skeleton_pose, deleter>;

  explicit skeleton_pose_pool(const skeleton& skeleton);

  skeleton_pose_pool(const skeleton_pose_pool&) = delete;
  skeleton_pose_pool(skeleton_pose_pool&&) = delete;

  ~skeleton_pose_pool();

  skeleton_pose_pool& operator=(const skeleton_pose_pool&) = delete;
  skeleton_pose_pool& operator=(skeleton_pose_pool&&) = delete;

  ptr borrow();

  // Return number of poses currently borrowed from the pool.
  [[nodiscard]] std::size_t get_live_poses_count() const;

  // Return maximum number of simultaneously borrowed poses
  // since the last call to `reset_peak_live_poses`.
  [[nodiscard]] std::size_t get_peak_live_poses() const;

  // Reset peak number of simultaneously borrowed poses.
  void reset_peak_live_poses();

private:
  // For returning back poses into the pool once unique ptr is destroyed
  friend struct deleter;

  const skeleton& _skeleton;
  std::vector<std::unique_ptr<skeleton_pose>> _poses;

  // Number of borrowed poses that are not yet returned to the pool.
  // Tracked unconditionally so that live/peak usage can be reported
  // regardless of the build type.
  std::size_t _live_poses{0};
  std::size_t _peak_live_poses{0};

#if defined(EELY_DEBUG)
  // Number of borrowed poses that are not yet returned to the pool.
  // To check that all poses are returned before the pool is destroyed.
  gsl::index _borrows{0};
#endif
};
}  // namespace eely::internal