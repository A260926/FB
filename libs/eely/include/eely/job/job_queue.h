#pragma once

#include "eely/skeleton/skeleton.h"
#include "eely/skeleton/skeleton_pose.h"
#include "eely/skeleton/skeleton_pose_pool.h"

#include <gsl/util>

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace eely::internal {
class job_base;

// Job queue list jobs to be executed to produce final pose for specified skeleton.
// Animation graphs and blendtrees produce these jobs when traversed.
class job_queue final {
public:
  // Construct job queue for specified skeleton.
  explicit job_queue(const skeleton& skeleton);

  // Add job to be executed and return its index.
  gsl::index add_job(job_base& job);

  // Execute the queue and write results into `out_pose`.
  void execute(skeleton_pose& out_pose);

  // Get job by its index.
  [[nodiscard]] job_base& get_job(gsl::index job_index);

  // Get pose pool to be used during execution.
  [[nodiscard]] skeleton_pose_pool& get_pose_pool();

  // Acquire slot to save pose to for further use.
  [[nodiscard]] gsl::index acquire_saved_pose_slot();

  // Save job's pose in a specified slot.
  void save_pose(const job_base& job, gsl::index pose_slot);

  // Restore pose from a speicifed slot.
  const skeleton_pose_pool::ptr& restore_pose(gsl::index pose_slot);

  // Prepare per-play bookkeeping. Called before graph traversal.
  // In shared mode a job's result can have several consumers, and input
  // poses are only reused or released once every consuming edge finished.
  void begin_play(bool shared_mode);

  // Return `true` if shared-pose mode is enabled.
  [[nodiscard]] bool is_shared_mode() const;

  // Discard every job planned for the current play without executing it.
  // Used when graph traversal fails before the queue is executed:
  // no result poses exist yet, so only bookkeeping needs to be cleared.
  void abort_play();

  // Register one consuming edge for a job that produces a pose.
  // Only used in shared-pose mode.
  void register_consumer(gsl::index job_index);

  // Return `true` if the job's result has a single consuming edge,
  // i.e. its input memory can be reused in place.
  [[nodiscard]] bool is_input_exclusive(gsl::index job_index) const;

  // Mark one consuming edge of a job as finished.
  // The job's result pose is released back to the pool
  // only after its last consumer is done with it.
  // In non-shared mode this releases the result immediately.
  void consume_job_result(gsl::index job_index);

  // Number of jobs executed during the last `execute` call.
  [[nodiscard]] std::size_t get_executed_jobs_count() const;

  // Peak number of simultaneously borrowed pool poses
  // observed during the last `execute` call.
  [[nodiscard]] std::size_t get_peak_live_poses() const;

private:
  std::vector<job_base*> _jobs;
  skeleton_pose_pool _pose_pool;
  std::vector<skeleton_pose_pool::ptr> _saved_poses;

  bool _shared_mode{false};

  // Number of consuming edges not yet processed for each job.
  // Only populated in shared-pose mode.
  std::unordered_map<gsl::index, std::size_t> _remaining_consumers;

  std::size_t _executed_jobs{0};
  std::size_t _peak_live_poses{0};
};
}  // namespace eely::internal