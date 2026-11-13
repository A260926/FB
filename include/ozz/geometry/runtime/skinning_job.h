//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) Guillaume Blanc                                              //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

#ifndef OZZ_OZZ_GEOMETRY_RUNTIME_SKINNING_JOB_H_
#define OZZ_OZZ_GEOMETRY_RUNTIME_SKINNING_JOB_H_

#include "ozz/base/platform.h"
#include "ozz/base/span.h"
#include "ozz/geometry/runtime/export.h"

namespace ozz {
namespace math {
struct Float4x4;
}
namespace geometry {

// Provides per-vertex matrix palette skinning job implementation.
// Skinning is the process of creating the association of skeleton joints with
// some vertices of a mesh. Portions of the mesh's skin can normally be
// associated with multiple joints, each one having a weight. The sum of the
// weights for a vertex is equal to 1. To calculate the final position of the
// vertex, each joint transformation is applied to the vertex position, scaled
// by its corresponding weight. This algorithm is called matrix palette skinning
// because of the set of joint transformations (stored as transform matrices)
// form a palette for the skin vertex to choose from.
//
// This job iterates and transforms vertices (points and vectors) provided as
// input using the matrix palette skinning algorithm. The implementation
// supports any number of joints influences per vertex, and can transform one
// point (vertex position) and two vectors (vertex normal and tangent) per loop
// (aka vertex). It assumes bi-normals aren't needed as they can be rebuilt
// from the normal and tangent with a lower cost than skinning (a single cross
// product).
// Input and output buffers must be provided with a stride value (aka the
// number of bytes from a vertex to the next). This allows the job to support
// vertices packed as array-of-structs (array of vertices with positions,
// normals...) or struct-of-arrays (buffers of points, buffer of normals...).
// The skinning job optimizes every code path at maximum. The loop is indeed not
// the same depending on the number of joints influencing a vertex (or if there
// are normals to transform). To maximize performances, application should
// partition its vertices based on their number of joints influences, and call
// a different job for every vertices set.
// Joint matrices are accessed using the per-vertex joints indices provided as
// input. These matrices must be pre-multiplied with the inverse of the skeleton
// bind-pose matrices. This allows to transform vertices to joints local space.
// In case of non-uniform-scale matrices, the job proposes to transform vectors
// using an optional set of matrices, whose are usually inverse transpose of
// joints matrices (see http://www.glprogramming.com/red/appendixf.html). This
// code path is less efficient than the one without this matrices set, and
// should only be used when input matrices have non uniform scaling or shearing.
// The job does not owned the buffers (in/output) and will thus not delete them
// during job's destruction.
//
// Two skinning algorithms are available, selected by the |mode| parameter:
// - kLinear (default): the historical matrix palette (linear blend) skinning
//   implementation. Existing callers get the exact same behavior as before.
// - kDualQuaternion: dual quaternion skinning, with a per-vertex fallback to
//   the linear blend implementation when one of the vertex influences cannot
//   be represented by a rigid transformation (non-uniform scale, uniform scale,
//   shear or reflection). See |mode| for more details.
struct OZZ_GEOMETRY_DLL SkinningJob {
  // Skinning algorithm used by Run().
  enum SkinningMode {
    // Matrix palette linear blend skinning. This is the default, and the mode
    // historical callers use (they don't have to set it).
    kLinear,

    // Dual quaternion skinning. The per-vertex decision is made as follows:
    // - If all the matrices actually referenced by strictly positive weights
    //   are rigid (finite matrix, bottom row (0,0,0,1), unit length and
    //   orthogonal linear columns, determinant +1, all within 1e-4), then the
    //   vertex is transformed with a dual quaternion.
    // - Otherwise the whole vertex (position, normal and tangent) is
    //   transformed with the legacy linear blend formula. A non-rigid
    //   influence with a zero weight does not trigger this fallback, though its
    //   index still has to be valid.
    // - If the blended real part length is lower than 1e-6, the vertex also
    //   falls back to linear blend skinning.
    // In addition to the span capacity checks performed by the linear mode,
    // this mode validates all the data it reads before writing any output:
    // indices must be in the palette, explicit and restored weights must be
    // finite and in [0,1], and the sum of the explicit weights must not exceed
    // 1. Run() returns false without modifying any output if one of these
    // conditions isn't met.
    // The dual quaternion path doesn't allocate memory.
    kDualQuaternion,
  };

  // Default constructor, initializes default values.
  SkinningJob();

  // Validates job parameters.
  // Returns true for a valid job, false otherwise:
  // - if any range is invalid. See each range description.
  // - if normals are provided but positions aren't.
  // - if tangents are provided but normals aren't.
  // - if no output is provided while an input is. For example, if input normals
  // are provided, then output normals must also.
  // - in kDualQuaternion mode, if the per-vertex input data (indices, weights)
  // is invalid, as described by |mode|. kLinear mode doesn't perform these
  // data checks.
  bool Validate() const;

  // Runs job's skinning task.
  // The job is validated before any operation is performed, see Validate() for
  // more details.
  // Returns false if *this job is not valid.
  bool Run() const;

  // Number of vertices to transform. All input and output arrays must store at
  // least this number of vertices.
  int vertex_count;

  // Skinning algorithm used by Run(). Defaults to kLinear, preserving the
  // historical behavior for callers that don't set it.
  SkinningMode mode;

  // Maximum number of joints influencing each vertex. Must be greater than 0.
  // The number of influences drives how joint_indices and joint_weights are
  // sampled:
  // - influences_count joint indices are red from joint_indices for each
  // vertex.
  // - influences_count - 1 joint weights are red from joint_weightrs for each
  // vertex. The weight of the last joint is restored (weights are normalized).
  int influences_count;

  // Array of matrices for each joint. Joint are indexed through indices array.
  span<const math::Float4x4> joint_matrices;

  // Optional array of inverse transposed matrices for each joint. If provided,
  // this array is used to transform vectors (normals and tangents), otherwise
  // joint_matrices array is used.
  // As explained here (http://www.glprogramming.com/red/appendixf.html) in the,
  // red book, transforming normals requires a special attention when the
  // transformation matrix has scaling or shearing. In this case the right
  // transformation is done by the inverse transpose of the transformation that
  // transforms points. Any rotation matrix is good though.
  // These matrices are optional as they might by costly to compute, and also
  // fall into a more costly code path in the skinning algorithm.
  span<const math::Float4x4> joint_inverse_transpose_matrices;

  // Array of joints indices. This array is used to indexes matrices in joints
  // array.
  // Each vertex has influences_max number of indices, meaning that the size of
  // this array must be at least influences_max * vertex_count.
  span<const uint16_t> joint_indices;
  size_t joint_indices_stride;

  // Array of joints weights. This array is used to associate a weight to every
  // joint that influences a vertex. The number of weights required per vertex
  // is "influences_max - 1". The weight for the last joint (for each vertex) is
  // restored at runtime thanks to the fact that the sum of the weights for each
  // vertex is 1.
  // Each vertex has (influences_max - 1) number of weights, meaning that the
  // size of this array must be at least (influences_max - 1)* vertex_count.
  span<const float> joint_weights;
  size_t joint_weights_stride;

  // Input vertex positions array (3 float values per vertex) and stride (number
  // of bytes between each position).
  // Array length must be at least vertex_count * in_positions_stride.
  span<const float> in_positions;
  size_t in_positions_stride;

  // Input vertex normals (3 float values per vertex) array and stride (number
  // of bytes between each normal).
  // Array length must be at least vertex_count * in_normals_stride.
  span<const float> in_normals;
  size_t in_normals_stride;

  // Input vertex tangents (3 float values per vertex) array and stride (number
  // of bytes between each tangent).
  // Array length must be at least vertex_count * in_tangents_stride.
  span<const float> in_tangents;
  size_t in_tangents_stride;

  // Output vertex positions (3 float values per vertex) array and stride
  // (number of bytes between each position).
  // Array length must be at least vertex_count * out_positions_stride.
  span<float> out_positions;
  size_t out_positions_stride;

  // Output vertex normals (3 float values per vertex) array and stride (number
  // of bytes between each normal).
  // Note that output normals are not normalized by the skinning job. This task
  // should be handled by the application, who knows if transform matrices have
  // uniform scale, and if normals are re-normalized later in the rendering
  // pipeline (shader vertex transformation stage).
  // Array length must be at least vertex_count * out_normals_stride.
  span<float> out_normals;
  size_t out_normals_stride;

  // Output vertex positions (3 float values per vertex) array and stride
  // (number of bytes between each tangent).
  // Like normals, Note that output tangents are not normalized by the skinning
  // job.
  // Array length must be at least vertex_count * out_tangents_stride.
  span<float> out_tangents;
  size_t out_tangents_stride;
};
}  // namespace geometry
}  // namespace ozz
#endif  // OZZ_OZZ_GEOMETRY_RUNTIME_SKINNING_JOB_H_
