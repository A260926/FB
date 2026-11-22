// skin_decompose_solver.h
//
// Offline decomposition of morph-driven vertex trajectories into a rigid
// skinned approximation.
//
// Given, for S sampled frames, the rest positions p_v and the observed
// positions x_sv of V vertices, this module jointly solves for:
//
//   * weights w_vk shared by every frame: w_vk >= 0, sum_k w_vk = 1, with at
//     most P positive entries per vertex;
//   * per-frame, per-bone proper rigid transforms (R_sk, t_sk) where R_sk is a
//     rotation matrix (orthogonal with determinant +1, never a reflection or a
//     scale),
//
// minimizing
//
//   E = sum_s sum_v || x_sv - sum_k w_vk (R_sk p_v + t_sk) ||^2.
//
// The solver is a deterministic block-coordinate (alternating) descent:
//   1. one-hot weights from the supplied bone labels, then a per-frame /
//      per-bone proper rigid least-squares fit for the initial transforms;
//   2. every iteration first re-solves every vertex's sparse simplex weights
//      against the fixed transforms (over all frames at once), then re-fits
//      each rigid transform in (frame, bone) order against the fixed weights,
//      subtracting the current contribution of every other bone.
// Bones with no supporting vertices keep their previous transform.
//
// The problem is non-convex; only convergence of this alternating scheme is
// promised, not a global optimum. All arithmetic uses double.
//
// The module depends on the C++17 standard library only.
#ifndef SKIN_DECOMPOSE_SOLVER_H
#define SKIN_DECOMPOSE_SOLVER_H

#include <cstddef>
#include <string>
#include <vector>

namespace skin_decompose {

// A 3x3 proper rotation, stored row-major, together with a translation.
struct RigidTransform {
    double rotation[9]; // row-major 3x3, orthogonal with determinant +1
    double translation[3];
};

// Per-iteration record of the complete objective.
struct EnergyReport {
    std::size_t iteration;       // 0 is the initialization
    double energy;               // full objective E at this point
    std::size_t non_finite;      // count of non-finite residuals (0 on success)
};

// Inputs and tuning parameters for the joint solve.
struct SkinProblem {
    std::size_t frame_count = 0;                 // S, >= 1
    std::size_t vertex_count = 0;                // V, >= 1
    std::size_t bone_count = 0;                  // K, 1..8
    std::size_t max_influences = 0;              // P, 1..min(4, K)
    std::size_t iterations = 0;                  // any non-negative value
    double max_error = 0.0;                      // finite, non-negative
    // rest_positions: V * 3 doubles, row-major over vertices.
    std::vector<double> rest_positions;
    // observed_positions: S * V * 3 doubles, indexed [s * V + v][c].
    std::vector<double> observed_positions;
    // labels: V entries in [0, K); every label must occur at least once.
    std::vector<int> labels;
};

// Full, self-contained result of a successful solve.
struct SkinSolution {
    bool ok = false;
    std::string error;                          // empty when ok == true
    // V * K weights; weights[v * K + k] >= 0 and each vertex row sums to 1.
    std::vector<double> weights;
    // S * K transforms, indexed [s * K + k].
    std::vector<RigidTransform> transforms;
    std::vector<EnergyReport> reports;          // initialization + every round
    double final_energy = 0.0;
    std::size_t frames = 0;
    std::size_t vertices = 0;
    std::size_t bones = 0;
    std::size_t influences = 0;
};

// Validate the problem description. On success returns an empty string;
// otherwise returns a human-readable, sentence-free reason.
std::string validate_problem(const SkinProblem& problem);

// Run the full alternating solve. A failure (invalid input, a non-finite
// intermediate result, or a numerical breakdown) leaves any previously held
// result untouched: the returned object is independent of the inputs and the
// caller's existing state is not modified through this call.
SkinSolution solve(const SkinProblem& problem);

// Recompute the complete objective for an externally held solution. Used by
// tests and by the export verification path. Returns a quiet NaN if any
// residual is non-finite.
double evaluate_energy(const SkinProblem& problem, const SkinSolution& solution);

// Number of strictly positive (above a fixed numerical floor) weights of a
// vertex; exposed for tests.
std::size_t positive_influence_count(const SkinSolution& solution, std::size_t vertex);

} // namespace skin_decompose

#endif // SKIN_DECOMPOSE_SOLVER_H
