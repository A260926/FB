/*
The MIT License (MIT)

Copyright (c) 2020 Light Transport Entertainment, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef UV_BAKER_BAKER_H_
#define UV_BAKER_BAKER_H_

//
// UV-space environment visibility / bent-normal baker.
//
// The baker maps world triangles into UV space, finds the world surface point
// under each texel center, then traces occlusion rays against a NanoRT
// triangle BVH built from the world geometry.  Nothing here depends on a
// window, an image loader or a model file.
//
// Conventions
// -----------
// * UV is in [0, 1] with v pointing up.  The result arrays are stored bottom
//   to top: index = y * width + x, y = 0 is the bottom row.
// * A texel is covered when its center ((x + 0.5) / width,
//   (y + 0.5) / height) lies inside a UV triangle.  Boundary ownership uses
//   the top-left half-open rule in a y-up frame.  A triangle is first
//   normalized to counter-clockwise orientation (positive signed UV area);
//   an edge that contains the center is owned only when, after that
//   normalization, it is a "top" edge (horizontal, interior below, i.e.
//   directed along -U) or a "left" edge (interior to the right, i.e.
//   directed along -V).  Mirrored / opposite-winding UV triangles therefore
//   behave exactly like their unmirrored counterparts.
// * If two different faces cover the same texel center and that center is
//   strictly inside either face (interior overlap), or the faces meet there
//   without sharing the same coincident UV edge/vertex, the bake fails with
//   an explicit overlap error instead of choosing one face arbitrarily.
//   At a shared edge or shared vertex the rule yields exactly one owner
//   (a point where it would yield several is reported as ambiguous).
//   Traversal order and input order never change the attribution.
// * Faces with zero UV area are ignored for rasterization.  Faces with zero
//   world area are rejected.
// * World geometric normals are unit vectors
//     N = normalize(cross(p1 - p0, p2 - p0))
//   following the triangle's listed world vertex order.  They do not depend
//   on UV winding.
// * The tangent frame picks the coordinate axis A in {+X, +Y, +Z} with the
//   smallest |dot(N, A)|; ties are resolved in X, then Y, then Z order.
//   Then
//     T = normalize(cross(A, N))
//     B = cross(N, T)
//   so that (T, B, N) is right-handed (T x B = N).  A local direction
//   (x, y, z), z >= 0, becomes normalize(x * T + y * B + z * N).
// * Each texel starts its own ray at P + bias * N, where P is the
//   barycentrically reconstructed world position.  The ray segment is
//   [0, radius): a hit counts as occlusion only when its distance t
//   satisfies 0 <= t < radius.  Only the source face is skipped; other
//   faces that happen to share a vertex or edge with it still occlude.
// * visibility = (# unoccluded directions) / (# directions).
//   bent normal = normalize(sum of unoccluded world directions), or the zero
//   vector when every direction is occluded.
//

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace uv_baker {

// The unit-length validation tolerance for caller supplied directions.
static const float kDirectionUnitTolerance = 1.0e-3f;

// Surface input.  All pointers are borrowed for the duration of Bake() and are
// never modified.
struct Input {
  // World vertex positions, xyz per vertex.
  const float *positions = nullptr;
  std::size_t position_count = 0;  // number of floats (3 per vertex)

  // Three world vertex indices per triangle.  Triangle i is "face i".
  const unsigned int *triangle_indices = nullptr;
  std::size_t triangle_index_count = 0;  // number of indices (3 per face)

  // Face-varying UVs: six floats per face (u0, v0, u1, v1, u2, v2), in the
  // same corner order as triangle_indices.
  const float *face_uvs = nullptr;
  std::size_t face_uv_count = 0;  // number of floats (6 per face)

  // One chart number per face.
  const int *face_chart_ids = nullptr;
  std::size_t face_chart_id_count = 0;  // number of faces

  // Non-empty array of unit directions in the local +Z hemisphere
  // (z >= 0).  All texels use the same directions.
  const float *directions = nullptr;
  std::size_t direction_count = 0;  // number of directions (3 floats each)
};

struct Options {
  int width = 0;
  int height = 0;

  // Occlusion ray segment length.  Must be finite and > 0.
  float radius = 0.0f;

  // Ray origin offset along the world geometric normal.  Finite and >= 0.
  float bias = 0.0f;

  // Optional integer-radius texel dilation.  0 disables it.  Must be >= 0.
  int dilation_radius = 0;
};

// Bake output.  Every vector is sized width * height and stored bottom to
// top.  On failure the result is cleared, so a previous bake can never leave
// stale values behind.
struct Result {
  int width = 0;
  int height = 0;

  // 1 for texels whose center truly covered a UV triangle.
  std::vector<std::uint8_t> real_coverage;
  // 1 for texels filled only by the optional dilation pass.
  std::vector<std::uint8_t> dilated;
  // 1 when real_coverage[i] or dilated[i] is 1.
  std::vector<std::uint8_t> has_value;

  // Original face (triangle index) and chart assignment.  -1 when no value.
  std::vector<std::int32_t> face_ids;
  std::vector<std::int32_t> chart_ids;

  // Barycentric coordinates matching NanoRT's convention:
  //   P = (1 - u - v) * p0 + u * p1 + v * p2
  // Dilation copies these from its source texel, as required for the full
  // value/assignment copy, even though they no longer locate the dilated
  // texel on the surface.
  std::vector<float> barycentric_u;
  std::vector<float> barycentric_v;

  // Fraction of unoccluded directions in [0, 1].
  std::vector<float> visibility;

  // World-space bent normal, xyz per texel.  Zero when fully occluded or when
  // the texel has no value.
  std::vector<float> bent_normals;

  // The dilation radius applied by the last Bake()/Dilate() call.
  int applied_dilation_radius = 0;

  std::size_t texel_count() const {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  }

  void Clear();
  void Reset(int new_width, int new_height);
};

// Compute the tangent frame described at the top of this header.  `normal`
// must be a unit vector.  Outputs are unit vectors and satisfy T x B = N.
void ComputeTangentFrame(const float normal[3], float tangent[3],
                         float bitangent[3]);

// Convert one local +Z-hemisphere direction into a unit world direction using
// the tangent frame above.  `local_direction` need not be normalized; the
// result always is (unless the local direction is zero, in which case the
// output is zero and the function returns false).
bool LocalDirectionToWorld(const float normal[3], const float local_direction[3],
                           float world_direction[3]);

// Apply the optional integer-radius fill.  Only texels with
// real_coverage == 0 are candidates.  A source must be an original covered
// texel (a texel filled by this pass is never a source) whose center is at
// Euclidean distance <= radius; the nearest source wins, with ties broken by
// chart id, then source y, then source x (all ascending).  Every data value
// and attribution is copied.  Calling it repeatedly rebuilds the fill from
// the real coverage, so a larger earlier radius cannot leak stale values.
//
// Returns false (and writes `error` when non-null) for a malformed result or
// a negative radius.
bool Dilate(Result *result, int radius, std::string *error = nullptr);

// Bake environment visibility and bent normals.
//
// Returns false and writes `error` (when non-null) for invalid input:
// non-positive dimensions, non-finite / non-positive radius, non-finite /
// negative bias, invalid directions, out-of-range indices, non-finite
// coordinates, inconsistent array lengths, zero world-area faces, ambiguous
// UV overlap, or a negative dilation radius.  Empty real coverage is valid.
bool Bake(const Input &input, const Options &options, Result *result,
          std::string *error = nullptr);

}  // namespace uv_baker

#endif  // UV_BAKER_BAKER_H_
