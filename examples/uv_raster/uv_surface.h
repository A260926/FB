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

#ifndef UV_RASTER_UV_SURFACE_H_
#define UV_RASTER_UV_SURFACE_H_

//
// Reusable UV rasterization / world surface reconstruction helpers shared by
// examples/uv_raster and examples/uv_baker.
//
// The mesh layout used here is the one prepared by examples/uv_raster:
//
//   positions                 : xyz per world vertex
//   triangle_indices          : three world vertex indices per triangle
//   face_uvs                  : (u, v) for each of the three triangle corners,
//                               in triangle order (face-varying UVs)
//   face_linear_normals        : xyz for each of the three triangle corners,
//                               in triangle order (face-varying normals)
//
// The UV triangle is in the first quadrant, v points up.  There is no
// dependency on a window, image IO or a model loader.
//

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace uv_surface {

// Barycentric interpolation, matching the convention used by NanoRT:
//   result = (1 - u - v) * v0 + u * v1 + v * v2
template <typename T>
inline T Lerp(T v0, T v1, T v2, float u, float v) {
  return (1.0f - u - v) * v0 + u * v1 + v * v2;
}

inline void InterpolatePosition(float out[3], const float *p0, const float *p1,
                                const float *p2, float u, float v) {
  const double w = 1.0 - static_cast<double>(u) - static_cast<double>(v);
  const double du = static_cast<double>(u);
  const double dv = static_cast<double>(v);
  out[0] = static_cast<float>(w * static_cast<double>(p0[0]) +
                              du * static_cast<double>(p1[0]) +
                              dv * static_cast<double>(p2[0]));
  out[1] = static_cast<float>(w * static_cast<double>(p0[1]) +
                              du * static_cast<double>(p1[1]) +
                              dv * static_cast<double>(p2[1]));
  out[2] = static_cast<float>(w * static_cast<double>(p0[2]) +
                              du * static_cast<double>(p1[2]) +
                              dv * static_cast<double>(p2[2]));
}

// Standard right-handed geometric normal:
//   normalize(cross(p1 - p0, p2 - p0))
// The orientation follows the world triangle's listed vertex order.  Returns
// false for a degenerate (zero-area) triangle.
inline bool ComputeGeometricNormal(float out[3], const float *p0,
                                   const float *p1, const float *p2) {
  const double e1[3] = {static_cast<double>(p1[0]) - static_cast<double>(p0[0]),
                        static_cast<double>(p1[1]) - static_cast<double>(p0[1]),
                        static_cast<double>(p1[2]) - static_cast<double>(p0[2])};
  const double e2[3] = {static_cast<double>(p2[0]) - static_cast<double>(p0[0]),
                        static_cast<double>(p2[1]) - static_cast<double>(p0[1]),
                        static_cast<double>(p2[2]) - static_cast<double>(p0[2])};
  const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                       e1[2] * e2[0] - e1[0] * e2[2],
                       e1[0] * e2[1] - e1[1] * e2[0]};
  const double len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
  if (!(len2 > 0.0) || !std::isfinite(len2)) {
    out[0] = out[1] = out[2] = 0.0f;
    return false;
  }
  const double inv_len = 1.0 / std::sqrt(len2);
  out[0] = static_cast<float>(n[0] * inv_len);
  out[1] = static_cast<float>(n[1] * inv_len);
  out[2] = static_cast<float>(n[2] * inv_len);
  return true;
}

// Orientation preserved by the original uv_raster fallback normal:
//   normalize(cross(p2 - p0, p1 - p0))
// Kept as a separate entry point so the legacy example's generated normals do
// not change while the shared module is used.
inline bool ComputeLegacyTriangleNormal(float out[3], const float *p0,
                                        const float *p1, const float *p2) {
  const double e1[3] = {static_cast<double>(p2[0]) - static_cast<double>(p0[0]),
                        static_cast<double>(p2[1]) - static_cast<double>(p0[1]),
                        static_cast<double>(p2[2]) - static_cast<double>(p0[2])};
  const double e2[3] = {static_cast<double>(p1[0]) - static_cast<double>(p0[0]),
                        static_cast<double>(p1[1]) - static_cast<double>(p0[1]),
                        static_cast<double>(p1[2]) - static_cast<double>(p0[2])};
  const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                       e1[2] * e2[0] - e1[0] * e2[2],
                       e1[0] * e2[1] - e1[1] * e2[0]};
  const double len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
  if (!(len2 > 0.0) || !std::isfinite(len2)) {
    out[0] = out[1] = out[2] = 0.0f;
    return false;
  }
  const double inv_len = 1.0 / std::sqrt(len2);
  out[0] = static_cast<float>(n[0] * inv_len);
  out[1] = static_cast<float>(n[1] * inv_len);
  out[2] = static_cast<float>(n[2] * inv_len);
  return true;
}

// A reconstructed surface point from a UV hit.
struct SurfaceSample {
  float position[3];         // world position, barycentric interpolation
  float geometric_normal[3]; // standard world geometric normal
  float shading_normal[3];   // interpolated face-varying normal when present
  bool has_shading_normal;
};

// Reconstruct a world surface sample from a UV triangle hit.
//
// `face` is the triangle index (the same value NanoRT reports as prim_id for
// the UV triangle BVH).  `(u, v)` are the barycentric coordinates reported by
// NanoRT.  `face_linear_normals` may be null; when present it must contain
// 9 floats per triangle.
//
// Returns false on an out-of-range face/index or a degenerate world triangle.
inline bool ReconstructSample(const float *positions, std::size_t num_floats,
                              const unsigned int *triangle_indices,
                              std::size_t num_indices, const float *face_linear_normals,
                              std::size_t num_face_normal_triangles,
                              unsigned int face, float u, float v,
                              SurfaceSample *sample) {
  if (!sample) return false;
  sample->has_shading_normal = false;

  if (num_floats % 3 != 0 || num_indices % 3 != 0) return false;
  const std::size_t num_vertices = num_floats / 3;
  const std::size_t num_faces = num_indices / 3;
  if (face >= num_faces) return false;

  const unsigned int i0 = triangle_indices[3 * face + 0];
  const unsigned int i1 = triangle_indices[3 * face + 1];
  const unsigned int i2 = triangle_indices[3 * face + 2];
  if (i0 >= num_vertices || i1 >= num_vertices || i2 >= num_vertices) {
    return false;
  }

  const float *p0 = positions + 3 * static_cast<std::size_t>(i0);
  const float *p1 = positions + 3 * static_cast<std::size_t>(i1);
  const float *p2 = positions + 3 * static_cast<std::size_t>(i2);

  InterpolatePosition(sample->position, p0, p1, p2, u, v);
  if (!ComputeGeometricNormal(sample->geometric_normal, p0, p1, p2)) {
    return false;
  }

  if (face_linear_normals && face < num_face_normal_triangles) {
    const float *n0 = face_linear_normals + 9 * static_cast<std::size_t>(face);
    InterpolatePosition(sample->shading_normal, n0 + 0, n0 + 3, n0 + 6, u, v);
    sample->has_shading_normal = true;
  } else {
    sample->shading_normal[0] = 0.0f;
    sample->shading_normal[1] = 0.0f;
    sample->shading_normal[2] = 0.0f;
  }
  return true;
}

// Build the 2D UV mesh used by the legacy rasterizer (z = 0, one UV corner per
// triangle vertex, indices are the identity permutation).  `uv_triangle_corners`
// receives 3 * (number of UV triangles).
inline bool BuildUVTriangleMesh(const float *facevarying_uvs,
                                std::size_t num_uv_floats,
                                std::vector<float> *uv_positions,
                                std::vector<unsigned int> *uv_indices,
                                std::size_t *uv_triangle_corners) {
  if (!uv_positions || !uv_indices || !uv_triangle_corners) return false;
  if (num_uv_floats % 2 != 0) return false;

  const std::size_t num_corners = num_uv_floats / 2;

  uv_positions->assign(3 * num_corners, 0.0f);
  for (std::size_t i = 0; i < num_corners; ++i) {
    (*uv_positions)[3 * i + 0] = facevarying_uvs[2 * i + 0];
    (*uv_positions)[3 * i + 1] = facevarying_uvs[2 * i + 1];
    (*uv_positions)[3 * i + 2] = 0.0f;
  }

  uv_indices->resize(num_corners);
  for (std::size_t i = 0; i < num_corners; ++i) {
    (*uv_indices)[i] = static_cast<unsigned int>(i);
  }
  *uv_triangle_corners = num_corners;
  return true;
}

}  // namespace uv_surface

#endif  // UV_RASTER_UV_SURFACE_H_
