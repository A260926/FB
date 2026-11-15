#ifndef PATCH_TRANSPORT_H
#define PATCH_TRANSPORT_H

#include "util/Utilities.h"
#include "object/Object.h"
#include "object/PatchMesh.h"

#include <array>
#include <memory>
#include <optional>
#include <vector>

// One brightness observation for an emission fit: the exitance of global
// face `face` should be close to `target` (per RGB channel, finite; targets
// may be negative and are used as-is) with the given positive `weight`.
// Repeated observations of the same face accumulate as independent terms.
struct FitObservation {
    int face = -1;
    Radiance target = Radiance(0, 0, 0);
    double weight = 0.0;
};

// Owning result of a successful PatchTransport::fit. Holds shared_ptr
// references to the source meshes plus value copies of the per-face
// emission (E) and exitance (B), so it stays readable and renderable after
// the transport object is destroyed.
class EmissionFit {
  public:
    int face_count() const { return static_cast<int>(exitance_.size()); }

    // Per-face fitted values in global face numbering. E is zero on
    // non-candidate faces; B solves B = E + rho * F * B over all faces.
    Radiance emission(int face) const { return emission_.at(face); }
    Radiance exitance(int face) const { return exitance_.at(face); }

    // Total objective: sum_o weight_o * |B_face(o) - target_o|^2
    //                  + lambda * sum_k |x_k|^2   (summed over RGB).
    double objective() const { return objective_; }

    // max over RGB channels of ||x - max(0, x - grad)||_inf at the solution.
    double projected_gradient_norm() const { return projected_gradient_norm_; }
    double projected_gradient_norm(int channel) const { return channel_norms_.at(channel); }

    int mesh_count() const { return static_cast<int>(meshes_.size()); }
    const PatchMesh* mesh(int k) const { return meshes_.at(k).get(); }

    // Look up fitted values for a hit on (mesh, source_face). Returns false
    // when the mesh did not participate in this fit.
    bool lookup(const PatchMesh* mesh, int source_face,
                Radiance& emission, Radiance& exitance) const;

  private:
    friend class PatchTransport;

    std::vector<shared_ptr<PatchMesh>> meshes_; // owning mesh references
    std::vector<int> face_offsets_;             // global id of each mesh's face 0
    std::vector<Radiance> emission_;            // E per global face
    std::vector<Radiance> exitance_;            // B per global face
    double objective_ = 0.0;
    double projected_gradient_norm_ = 0.0;
    std::array<double, 3> channel_norms_{{0.0, 0.0, 0.0}};
};

// Discrete patch-to-patch light transport over the faces of a set of
// PatchMesh objects, embedded in the real scene (an already-built Object
// world that contains these meshes exactly once, plus any occluders).
//
// Faces are numbered consecutively in mesh input order, then by source face
// number; at most 128 faces. Each face contributes three equal-weight area
// sample points at the cyclic permutations of barycentric (2/3, 1/6, 1/6).
// For every ordered pair of distinct faces the nine connecting segments are
// tested with the world's own intersect() on the open interval
// (eps, distance - eps); every primitive occludes double-sided and back
// faces do not transmit. A sample-point pair closer than 100*eps
// contributes zero. With Ai, ni the true triangle area and oriented unit
// normal,
//
//   H_ij = Ai*Aj/(9*pi) * sum_9 max(0, ni.d) * max(0, -nj.d) * V / dist^2
//
// stored symmetrically (H_ii = 0). The whole H is then divided by
// max(1, s) with s = max_i sum_j H_ij/Ai (a single global scale, never a
// per-row normalization, so area reciprocity is preserved) and
// F_ij = H_ij/Ai.
//
// The transport keeps an owning snapshot of geometry and reflectance
// (rho = kd * diffusecolor per channel); it does not reference the world
// after construction. Rebuild it when geometry or occlusion changes.
class PatchTransport {
  public:
    static constexpr int kMaxFaces = 128;

    // Throws std::invalid_argument on non-finite/non-positive eps, empty or
    // duplicate mesh list, more than kMaxFaces faces, or faces whose
    // material is textured/reflective/refractive or whose rho = kd *
    // diffusecolor falls outside [0, 0.95] in any channel.
    PatchTransport(std::vector<shared_ptr<PatchMesh>> meshes,
                   const Object& world, double eps);

    int face_count() const { return face_count_; }
    int mesh_count() const { return static_cast<int>(meshes_.size()); }

    // Read-only queries in global face numbering.
    double area(int face) const;
    double form_factor(int i, int j) const;   // F_ij (row i receives from j)
    Radiance reflectance(int face) const;     // rho per channel
    const PatchMesh* face_mesh(int face) const;
    int face_source_index(int face) const;

    // Solve for non-negative RGB emission x on 1..16 distinct candidate
    // faces that minimizes
    //   sum_o weight_o * |B_face(o) - target_o|^2 + lambda * sum_k |x_k|^2
    // subject to B = E + rho * F * B (all reflection orders, per channel).
    // Returns std::nullopt on invalid input or when the positive iteration
    // budget is exhausted before all three channels satisfy
    //   ||x - max(0, x - grad)||_inf <= 1e-8 * max(1, ||grad_at_zero||_inf).
    // The call is const and keeps no state between fits; previously
    // returned EmissionFit results are never modified.
    std::optional<EmissionFit> fit(const std::vector<int>& candidates,
                                   const std::vector<FitObservation>& observations,
                                   double lambda, int max_iterations) const;

  private:
    std::vector<shared_ptr<PatchMesh>> meshes_;
    std::vector<int> face_offsets_;  // global id of each mesh's face 0
    int face_count_ = 0;
    double eps_ = 0.0;

    // Owning snapshot taken at construction time.
    std::vector<std::array<Point3, 3>> verts_;  // per-face vertex positions
    std::vector<double> areas_;
    std::vector<Vector3> normals_;              // oriented unit normals
    std::vector<Radiance> rho_;                 // kd * diffusecolor per face

    std::vector<double> F_;                     // n x n row-major form factors
    std::array<std::vector<double>, 3> resp_;   // per-channel (I - D_c F)^-1
};

#endif
