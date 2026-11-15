#pragma once

#include "eely/job/job_base.h"
#include "eely/job/job_queue.h"
#include "eely/skeleton/skeleton_pose.h"
#include "eely/skeleton/skeleton_pose_pool.h"

#include <gsl/util>

#include <memory>

namespace eely::internal {
// Job that adds an additive pose to another.
class job_add final : public job_base {
public:
  // Set index of a job that produces a first pose (the one that produces main pose).
  void set_first_job_index(gsl::index index);

  // Set index of a job that produces a second pose (the one that produces additive pose).
  void set_second_job_index(gsl::index index);

private:
  skeleton_pose_pool::ptr execute_impl(job_queue& queue) override;

  std::optional<gsl::index> _first;
  std::optional<gsl::index> _second;
};

// Implementation

inline void job_add::set_first_job_index(const gsl::index index)
{
  _first = index;
}

inline void job_add::set_second_job_index(const gsl::index index)
{
  _second = index;
}

inline skeleton_pose_pool::ptr job_add::execute_impl(job_queue& queue)
{
  job_base& first_job{queue.get_job(_first.value())};
  job_base& second_job{queue.get_job(_second.value())};

  if (queue.is_shared_mode()) {
    // Same ownership rules as in `job_blend`:
    // the first input is only reused when this is its single consuming edge.

    const bool first_exclusive{queue.is_input_exclusive(_first.value())};

    skeleton_pose_pool::ptr result;
    const skeleton_pose_pool::ptr& second_pose{second_job.get_result_pose()};

    if (first_exclusive) {
      result = first_job.transfer_result_pose();
      skeleton_pose_add(*result, *second_pose, *result);
    }
    else {
      const skeleton_pose_pool::ptr& first_pose{first_job.get_result_pose()};
      result = queue.get_pose_pool().borrow();
      skeleton_pose_add(*first_pose, *second_pose, *result);
    }

    queue.consume_job_result(_first.value());
    queue.consume_job_result(_second.value());

    return result;
  }

  // We can reuse one pose from the pool here,
  // let it be a pose from the first job.
  // Second one can be released after the addition, no longer needed.

  skeleton_pose_pool::ptr p0{first_job.transfer_result_pose()};
  const skeleton_pose_pool::ptr& p1{second_job.get_result_pose()};

  skeleton_pose_add(*p0, *p1, *p0);

  second_job.release_result_pose();

  return p0;
}

}  // namespace eely::internal