#include "eely/job/job_queue.h"

#include "eely/base/assert.h"
#include "eely/job/job_base.h"
#include "eely/skeleton/skeleton.h"
#include "eely/skeleton/skeleton_pose.h"
#include "eely/skeleton/skeleton_pose_pool.h"

#include <gsl/util>

#include <algorithm>
#include <vector>

namespace eely::internal {
job_queue::job_queue(const skeleton& skeleton) : _pose_pool{skeleton} {}

gsl::index job_queue::add_job(job_base& job)
{
  EXPECTS(std::find(_jobs.begin(), _jobs.end(), &job) == _jobs.end());
  _jobs.push_back(&job);
  return std::ssize(_jobs) - 1;
}

skeleton_pose_pool& job_queue::get_pose_pool()
{
  return _pose_pool;
}

gsl::index job_queue::acquire_saved_pose_slot()
{
  _saved_poses.emplace_back();
  return std::ssize(_saved_poses) - 1;
}

void job_queue::save_pose(const job_base& job, const gsl::index pose_slot)
{
  if (_saved_poses[pose_slot] == nullptr) {
    _saved_poses[pose_slot] = _pose_pool.borrow();
  }

  *_saved_poses[pose_slot] = *job.get_result_pose();
}

const skeleton_pose_pool::ptr& job_queue::restore_pose(const gsl::index pose_slot)
{
  return _saved_poses[pose_slot];
}

job_base& job_queue::get_job(gsl::index job_index)
{
  job_base* result{_jobs.at(job_index)};
  EXPECTS(result != nullptr);
  return *result;
}

bool job_queue::is_shared_mode() const
{
  return _shared_mode;
}

void job_queue::begin_play(const bool shared_mode)
{
  _shared_mode = shared_mode;
  _remaining_consumers.clear();
  _executed_jobs = 0;
  _pose_pool.reset_peak_live_poses();
}

void job_queue::abort_play()
{
  // Traversal never executes jobs, so normally no result poses exist.
  // Release them anyway in case execution itself fails midway: every result
  // pose must go back to the pool before the next play starts.
  for (job_base* job : _jobs) {
    job->release_result_pose();
  }

  _jobs.clear();
  _remaining_consumers.clear();
}

void job_queue::register_consumer(const gsl::index job_index)
{
  EXPECTS(_shared_mode);
  ++_remaining_consumers[job_index];
}

bool job_queue::is_input_exclusive(const gsl::index job_index) const
{
  EXPECTS(_shared_mode);
  const auto iter{_remaining_consumers.find(job_index)};
  EXPECTS(iter != _remaining_consumers.end());
  return iter->second == 1;
}

void job_queue::consume_job_result(const gsl::index job_index)
{
  EXPECTS(_shared_mode);

  const auto iter{_remaining_consumers.find(job_index)};
  EXPECTS(iter != _remaining_consumers.end());
  EXPECTS(iter->second > 0);

  --iter->second;
  if (iter->second == 0) {
    // Every consuming edge has finished: the pose is no longer needed.
    // Note that an exclusive consumer might have transferred ownership
    // of the result already, in which case this is a no-op.
    _jobs.at(static_cast<std::size_t>(job_index))->release_result_pose();
    _remaining_consumers.erase(iter);
  }
}

std::size_t job_queue::get_executed_jobs_count() const
{
  return _executed_jobs;
}

std::size_t job_queue::get_peak_live_poses() const
{
  return std::max(_peak_live_poses, _pose_pool.get_peak_live_poses());
}

void job_queue::execute(skeleton_pose& out_pose)
{
  EXPECTS(!_jobs.empty());

  job_base* final_job{nullptr};

  for (job_base* job : _jobs) {
    job->execute(*this);
    ++_executed_jobs;

    if (job->get_result_pose() != nullptr) {
      // Final job is the one that produces final pose.
      // It's not necessarily last in a queue,
      // there can be other jobs after it that do some utility stuff.
      final_job = job;
    }
  }

  out_pose = *final_job->get_result_pose();

  for (job_base* job : _jobs) {
    job->release_result_pose();
  }

  _peak_live_poses = _pose_pool.get_peak_live_poses();

  _jobs.clear();
  _remaining_consumers.clear();
}
}  // namespace eely::internal