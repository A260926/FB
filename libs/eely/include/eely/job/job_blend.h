#pragma once

#include "eely/job/job_base.h"
#include "eely/job/job_queue.h"
#include "eely/skeleton/skeleton_pose.h"
#include "eely/skeleton/skeleton_pose_pool.h"

#include <gsl/util>

#include <memory>

namespace eely::internal {
// Job that blends two poses together with specified weight.
class job_blend final : public job_base {
public:
  // Set index of a job that produces a first pose (the one to blend from).
  void set_first_job_index(gsl::index index);

  // Set index of a job that produces a second pose (the one to blend to).
  void set_second_job_index(gsl::index index);

  // Set blending weight.
  void set_weight(float weight);

private:
  skeleton_pose_pool::ptr execute_impl(job_queue& queue) override;

  std::optional<gsl::index> _first;
  std::optional<gsl::index> _second;
  float _weight{0.0F};
};

// Implementation

inline void job_blend::set_first_job_index(const gsl::index index)
{
  _first = index;
}

inline void job_blend::set_second_job_index(const gsl::index index)
{
  _second = index;
}

inline void job_blend::set_weight(const float weight)
{
  _weight = weight;
}

inline skeleton_pose_pool::ptr job_blend::execute_impl(job_queue& queue)
{
  job_base& first_job{queue.get_job(_first.value())};
  job_base& second_job{queue.get_job(_second.value())};

  if (queue.is_shared_mode()) {
    // An input pose can have several consumers in a shared-pose graph.
    // Its memory is only reused when this blend is the single consuming edge,
    // otherwise an independent pose is acquired from the pool.

    const bool first_exclusive{queue.is_input_exclusive(_first.value())};

    skeleton_pose_pool::ptr result;
    const skeleton_pose_pool::ptr& second_pose{second_job.get_result_pose()};

    if (first_exclusive) {
      result = first_job.transfer_result_pose();
      skeleton_pose_blend(*result, *second_pose, _weight, *result);
    }
    else {
      const skeleton_pose_pool::ptr& first_pose{first_job.get_result_pose()};
      result = queue.get_pose_pool().borrow();
      skeleton_pose_blend(*first_pose, *second_pose, _weight, *result);
    }

    // Inputs stay valid until every consuming edge has finished.
    queue.consume_job_result(_first.value());
    queue.consume_job_result(_second.value());

    return result;
  }

  // We can reuse one pose from the pool here,
  // let it be a pose from the first job.
  // Second one can be released after the blending, no longer needed.

  skeleton_pose_pool::ptr p0{first_job.transfer_result_pose()};
  const skeleton_pose_pool::ptr& p1{second_job.get_result_pose()};

  skeleton_pose_blend(*p0, *p1, _weight, *p0);

  second_job.release_result_pose();

  return p0;
}
}  // namespace eely::internal