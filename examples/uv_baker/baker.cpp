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

#include "baker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../nanort.h"
#include "../uv_raster/uv_surface.h"

namespace uv_baker {

namespace {

const double kOnEdgeEpsilon = 1.0e-7;     // UV distance considered "on an edge"
const double kEdgeMatchEpsilon = 1.0e-7;  // shared-feature endpoint tolerance

inline bool IsFinite(float v) { return std::isfinite(v); }

inline bool IsFinite3(const float *v) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

bool Fail(Result *result, std::string *error, const char *message) {
  if (error) *error = message;
  if (result) result->Clear();
  return false;
}

struct Vec3d {
  double x, y, z;
};

inline Vec3d Sub(const Vec3d &a, const Vec3d &b) {
  return Vec3d{a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3d Cross(const Vec3d &a, const Vec3d &b) {
  return Vec3d{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
               a.x * b.y - a.y * b.x};
}
inline double Length(const Vec3d &a) {
  return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

// Signed distance from (pu, pv) to the directed UV edge a -> b, scaled so the
// triangle interior is positive after multiplying by `orientation`.
inline double EdgeDistance(const Vec3d &a, const Vec3d &b, double pu, double pv,
                           double orientation) {
  const Vec3d e = Sub(b, a);
  const double len = std::sqrt(e.x * e.x + e.y * e.y);
  if (!(len > 0.0)) return 0.0;
  const double cr = e.x * (pv - a.y) - e.y * (pu - a.x);
  return orientation * cr / len;
}

struct FaceGeometry {
  // World vertices in original corner order (for validation and area).
  Vec3d p[3];
  Vec3d normal;
  int chart_id = 0;

  // UV triangle in original corner order.
  Vec3d uv[3];
  double orientation = 1.0;  // +1 when the UV triangle is CCW
  bool active = false;       // false for zero-UV-area faces

  // Unit tangent frame derived from the same double-precision geometric
  // normal that offsets the ray origin, so rays and frames agree exactly.
  Vec3d tangent{1.0, 0.0, 0.0};
  Vec3d bitangent{0.0, 1.0, 0.0};
};

// Double-precision tangent frame with the documented axis choice.
void ComputeTangentFrameD(const Vec3d &normal, Vec3d *tangent,
                          Vec3d *bitangent) {
  int axis = 0;
  double best = std::fabs(normal.x);
  const double ay = std::fabs(normal.y);
  if (ay < best) {
    axis = 1;
    best = ay;
  }
  const double az = std::fabs(normal.z);
  if (az < best) {
    axis = 2;
  }

  Vec3d a{0.0, 0.0, 0.0};
  if (axis == 0) a.x = 1.0;
  if (axis == 1) a.y = 1.0;
  if (axis == 2) a.z = 1.0;

  Vec3d t = Cross(a, normal);
  double tlen = Length(t);
  if (!(tlen > 0.0)) {
    t = Vec3d{1.0, 0.0, 0.0};
    tlen = 1.0;
  }
  t.x /= tlen;
  t.y /= tlen;
  t.z /= tlen;
  *tangent = t;
  // (T, B, N) is right-handed: cross(T, B) == N.
  *bitangent = Cross(normal, t);
}

inline bool Barycentric(const FaceGeometry &face, double pu, double pv,
                        double out[3]) {
  const Vec3d &a = face.uv[0];
  const Vec3d &b = face.uv[1];
  const Vec3d &c = face.uv[2];
  const double denom =
      (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
  if (!(denom != 0.0)) return false;
  const double w0 =
      ((b.y - c.y) * (pu - c.x) + (c.x - b.x) * (pv - c.y)) / denom;
  const double w1 =
      ((c.y - a.y) * (pu - c.x) + (a.x - c.x) * (pv - c.y)) / denom;
  out[0] = w0;
  out[1] = w1;
  out[2] = 1.0 - w0 - w1;
  return true;
}

// After normalizing a triangle to CCW (y-up), an edge is a "top" edge when it
// is horizontal and directed along -U, and a "left" edge when it is not
// horizontal and directed along -V.  Such edges own a texel center that lies
// exactly on them.  Mirrored UV winding is normalized by `orientation` first,
// so both windings behave identically.
inline bool IsTopOrLeftEdge(const FaceGeometry &face, int edge) {
  const Vec3d &a = face.uv[edge];
  const Vec3d &b = face.uv[(edge + 1) % 3];
  const double du = face.orientation * (b.x - a.x);
  const double dv = face.orientation * (b.y - a.y);
  const double len = std::sqrt(du * du + dv * dv);
  if (!(len > 0.0)) return false;
  const bool horizontal = std::fabs(dv) <= kOnEdgeEpsilon * len;
  if (horizontal) return du < 0.0;  // top
  return dv < 0.0;  // left
}

inline bool SamePoint(const Vec3d &a, const Vec3d &b) {
  return std::fabs(a.x - b.x) <= kEdgeMatchEpsilon &&
         std::fabs(a.y - b.y) <= kEdgeMatchEpsilon;
}

inline bool VertexContainsCenter(const Vec3d &v, const double center[2]) {
  return std::fabs(v.x - center[0]) <= kEdgeMatchEpsilon &&
         std::fabs(v.y - center[1]) <= kEdgeMatchEpsilon;
}

struct FaceBoundary {
  bool vertex[3] = {false, false, false};
  bool edge[3] = {false, false, false};
};

// True when two faces share at least one identical boundary feature (a
// coincident UV edge segment or a coincident UV vertex) at this center.
bool SharesBoundaryFeature(const FaceGeometry &a, const FaceBoundary &ba,
                           const FaceGeometry &b, const FaceBoundary &bb,
                           const double center[2]) {
  for (int va = 0; va < 3; ++va) {
    if (!ba.vertex[va]) continue;
    for (int vb = 0; vb < 3; ++vb) {
      if (!bb.vertex[vb]) continue;
      if (SamePoint(a.uv[va], b.uv[vb])) return true;
    }
  }
  for (int ea = 0; ea < 3; ++ea) {
    if (!ba.edge[ea]) continue;
    const Vec3d &a0 = a.uv[ea];
    const Vec3d &a1 = a.uv[(ea + 1) % 3];
    for (int eb = 0; eb < 3; ++eb) {
      if (!bb.edge[eb]) continue;
      const Vec3d &b0 = b.uv[eb];
      const Vec3d &b1 = b.uv[(eb + 1) % 3];
      if ((SamePoint(a0, b0) && SamePoint(a1, b1)) ||
          (SamePoint(a0, b1) && SamePoint(a1, b0))) {
        return true;
      }
    }
  }
  (void)center;
  return false;
}

struct ClaimInfo {
  std::uint32_t face = 0;
  bool strict = true;
  bool owned = true;  // true until a right/bottom boundary edge is found
  FaceBoundary boundary;
  double bary[3] = {0.0, 0.0, 0.0};
};

// Resolve one texel center against the closed-coverage claims.
//   1 : covered, attribution written
//   0 : not covered
//  -1 : invalid overlap, `error` written
int ResolveCoverage(const std::vector<FaceGeometry> &faces,
                    const std::vector<std::uint32_t> &claims,
                    const double center[2], std::uint32_t *out_face,
                    double out_bary[3], std::string *error) {
  if (claims.empty()) return 0;

  std::vector<ClaimInfo> info;
  info.reserve(claims.size());
  for (std::uint32_t face_id : claims) {
    const FaceGeometry &face = faces[face_id];
    ClaimInfo ci;
    ci.face = face_id;
    for (int e = 0; e < 3; ++e) {
      const Vec3d &a = face.uv[e];
      const Vec3d &b = face.uv[(e + 1) % 3];
      const double d =
          EdgeDistance(a, b, center[0], center[1], face.orientation);
      if (d <= kOnEdgeEpsilon) ci.strict = false;
      if (std::fabs(d) <= kOnEdgeEpsilon) {
        ci.boundary.edge[e] = true;
        // A boundary point is owned only when every edge passing through it
        // is a top/left edge.  A triangle whose right or bottom edge also
        // touches the point must not claim it (that point belongs to the
        // neighbouring triangle by the half-open rule).
        if (!IsTopOrLeftEdge(face, e)) ci.owned = false;
      }
    }
    for (int v = 0; v < 3; ++v) {
      ci.boundary.vertex[v] = VertexContainsCenter(face.uv[v], center);
    }
    if (!Barycentric(face, center[0], center[1], ci.bary)) {
      if (error) *error = "degenerate UV triangle";
      return -1;
    }
    info.push_back(ci);
  }

  if (info.size() == 1) {
    const ClaimInfo &ci = info[0];
    if (ci.strict || ci.owned) {
      *out_face = ci.face;
      std::memcpy(out_bary, ci.bary, sizeof(ci.bary));
      return 1;
    }
    return 0;
  }

  // Multiple faces claim this center.  Interior overlap is always invalid.
  for (const ClaimInfo &ci : info) {
    if (ci.strict) {
      if (error) *error = "overlapping UV faces cover the same texel center";
      return -1;
    }
  }

  // Boundary contact is only valid when every pair of claimants shares a
  // boundary feature (edge segment or vertex).  A T-junction or a boundary
  // crossing a foreign interior is an overlap.
  for (std::size_t i = 0; i < info.size(); ++i) {
    for (std::size_t j = i + 1; j < info.size(); ++j) {
      if (!SharesBoundaryFeature(faces[info[i].face], info[i].boundary,
                                 faces[info[j].face], info[j].boundary,
                                 center)) {
        if (error) *error = "overlapping UV faces cover the same texel center";
        return -1;
      }
    }
  }

  // Apply the top-left ownership rule.  A point on a shared edge is owned by
  // exactly one of the two adjacent faces (the edge runs in opposite
  // directions in each, and only one direction is "top or left"), and at a
  // shared vertex exactly one incident face has all of its touching edges
  // top/left.  More than one owner therefore means the UV layout is
  // ambiguous or overlapping; never pick one arbitrarily.
  const ClaimInfo *owner = nullptr;
  for (const ClaimInfo &ci : info) {
    if (!ci.owned) continue;
    if (owner) {
      if (error) *error = "ambiguous shared-boundary UV ownership";
      return -1;
    }
    owner = &ci;
  }
  if (!owner) return 0;  // shared bottom/right boundary is not covered

  *out_face = owner->face;
  std::memcpy(out_bary, owner->bary, sizeof(owner->bary));
  return 1;
}

inline Vec3d RotateLocal(const FaceGeometry &face, const float local[3]) {
  const Vec3d &t = face.tangent;
  const Vec3d &b = face.bitangent;
  const Vec3d &n = face.normal;
  Vec3d w{t.x * local[0] + b.x * local[1] + n.x * local[2],
          t.y * local[0] + b.y * local[1] + n.y * local[2],
          t.z * local[0] + b.z * local[1] + n.z * local[2]};
  const double len = Length(w);
  if (len > 0.0) {
    w.x /= len;
    w.y /= len;
    w.z /= len;
  }
  return w;
}

// Clear every texel that is not part of the real coverage.  Used both by the
// dilation pass and when a bake resets its result.
void ClearNonReal(Result *result, const std::vector<std::uint8_t> &real) {
  const std::size_t n = result->texel_count();
  for (std::size_t i = 0; i < n; ++i) {
    if (real[i]) continue;
    result->dilated[i] = 0;
    result->has_value[i] = 0;
    result->face_ids[i] = -1;
    result->chart_ids[i] = -1;
    result->barycentric_u[i] = 0.0f;
    result->barycentric_v[i] = 0.0f;
    result->visibility[i] = 0.0f;
    result->bent_normals[3 * i + 0] = 0.0f;
    result->bent_normals[3 * i + 1] = 0.0f;
    result->bent_normals[3 * i + 2] = 0.0f;
  }
}

}  // namespace

void Result::Clear() {
  width = 0;
  height = 0;
  real_coverage.clear();
  dilated.clear();
  has_value.clear();
  face_ids.clear();
  chart_ids.clear();
  barycentric_u.clear();
  barycentric_v.clear();
  visibility.clear();
  bent_normals.clear();
  applied_dilation_radius = 0;
}

void Result::Reset(int new_width, int new_height) {
  Clear();
  width = new_width;
  height = new_height;
  const std::size_t n = texel_count();
  real_coverage.assign(n, 0);
  dilated.assign(n, 0);
  has_value.assign(n, 0);
  face_ids.assign(n, -1);
  chart_ids.assign(n, -1);
  barycentric_u.assign(n, 0.0f);
  barycentric_v.assign(n, 0.0f);
  visibility.assign(n, 0.0f);
  bent_normals.assign(3 * n, 0.0f);
}

void ComputeTangentFrame(const float normal[3], float tangent[3],
                         float bitangent[3]) {
  // Axis with the smallest |dot(N, axis)|; strict comparisons resolve ties in
  // X, then Y, then Z order.
  int axis = 0;
  double best = std::fabs(static_cast<double>(normal[0]));
  const double ay = std::fabs(static_cast<double>(normal[1]));
  if (ay < best) {
    axis = 1;
    best = ay;
  }
  const double az = std::fabs(static_cast<double>(normal[2]));
  if (az < best) {
    axis = 2;
  }

  double a[3] = {0.0, 0.0, 0.0};
  a[axis] = 1.0;

  double t[3] = {a[1] * normal[2] - a[2] * normal[1],
                 a[2] * normal[0] - a[0] * normal[2],
                 a[0] * normal[1] - a[1] * normal[0]};
  double tlen = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
  if (!(tlen > 0.0)) {
    t[0] = 1.0;
    t[1] = 0.0;
    t[2] = 0.0;
    tlen = 1.0;
  }
  t[0] /= tlen;
  t[1] /= tlen;
  t[2] /= tlen;

  // (T, B, N) is right-handed: cross(T, B) == N.
  const double b[3] = {normal[1] * t[2] - normal[2] * t[1],
                       normal[2] * t[0] - normal[0] * t[2],
                       normal[0] * t[1] - normal[1] * t[0]};

  tangent[0] = static_cast<float>(t[0]);
  tangent[1] = static_cast<float>(t[1]);
  tangent[2] = static_cast<float>(t[2]);
  bitangent[0] = static_cast<float>(b[0]);
  bitangent[1] = static_cast<float>(b[1]);
  bitangent[2] = static_cast<float>(b[2]);
}

bool LocalDirectionToWorld(const float normal[3], const float local_direction[3],
                           float world_direction[3]) {
  float tangent[3];
  float bitangent[3];
  ComputeTangentFrame(normal, tangent, bitangent);

  const double w[3] = {
      tangent[0] * static_cast<double>(local_direction[0]) +
          bitangent[0] * static_cast<double>(local_direction[1]) +
          normal[0] * static_cast<double>(local_direction[2]),
      tangent[1] * static_cast<double>(local_direction[0]) +
          bitangent[1] * static_cast<double>(local_direction[1]) +
          normal[1] * static_cast<double>(local_direction[2]),
      tangent[2] * static_cast<double>(local_direction[0]) +
          bitangent[2] * static_cast<double>(local_direction[1]) +
          normal[2] * static_cast<double>(local_direction[2])};
  const double len = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
  if (!(len > 0.0)) {
    world_direction[0] = world_direction[1] = world_direction[2] = 0.0f;
    return false;
  }
  world_direction[0] = static_cast<float>(w[0] / len);
  world_direction[1] = static_cast<float>(w[1] / len);
  world_direction[2] = static_cast<float>(w[2] / len);
  return true;
}

bool Dilate(Result *result, int radius, std::string *error) {
  if (!result) {
    if (error) *error = "null result";
    return false;
  }
  if (radius < 0) {
    if (error) *error = "dilation radius must be >= 0";
    return false;
  }

  const int w = result->width;
  const int h = result->height;
  if (w < 0 || h < 0) {
    if (error) *error = "negative result dimensions";
    return false;
  }
  const std::size_t n = result->texel_count();
  if (result->real_coverage.size() != n || result->dilated.size() != n ||
      result->has_value.size() != n || result->face_ids.size() != n ||
      result->chart_ids.size() != n || result->barycentric_u.size() != n ||
      result->barycentric_v.size() != n || result->visibility.size() != n ||
      result->bent_normals.size() != 3 * n) {
    if (error) *error = "malformed dilation input";
    return false;
  }

  result->applied_dilation_radius = radius;

  // Snapshot real coverage so texels filled by this pass can never act as
  // sources, and so a repeated call rebuilds from the original coverage.
  const std::vector<std::uint8_t> real = result->real_coverage;
  ClearNonReal(result, real);

  if (radius == 0 || n == 0) return true;

  const std::int64_t r = radius;
  const std::int64_t r2 = r * r;

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t dst = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
      if (real[dst]) continue;

      bool found = false;
      std::int64_t best_dist2 = 0;
      std::int32_t best_chart = 0;
      int best_y = 0;
      int best_x = 0;
      std::size_t best_index = 0;

      const int y0 = std::max(0, y - radius);
      const int y1 = std::min(h - 1, y + radius);
      const int x0 = std::max(0, x - radius);
      const int x1 = std::min(w - 1, x + radius);

      for (int sy = y0; sy <= y1; ++sy) {
        const std::int64_t dy = static_cast<std::int64_t>(sy) - y;
        for (int sx = x0; sx <= x1; ++sx) {
          const std::size_t src = static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(sx);
          if (!real[src]) continue;
          const std::int64_t dx = static_cast<std::int64_t>(sx) - x;
          const std::int64_t dist2 = dx * dx + dy * dy;
          if (dist2 > r2) continue;

          const std::int32_t chart = result->chart_ids[src];
          bool better = false;
          if (!found) {
            better = true;
          } else if (dist2 != best_dist2) {
            better = dist2 < best_dist2;
          } else if (chart != best_chart) {
            better = chart < best_chart;
          } else if (sy != best_y) {
            better = sy < best_y;
          } else {
            better = sx < best_x;
          }

          if (better) {
            found = true;
            best_dist2 = dist2;
            best_chart = chart;
            best_y = sy;
            best_x = sx;
            best_index = src;
          }
        }
      }

      if (!found) continue;

      result->dilated[dst] = 1;
      result->has_value[dst] = 1;
      result->face_ids[dst] = result->face_ids[best_index];
      result->chart_ids[dst] = result->chart_ids[best_index];
      result->barycentric_u[dst] = result->barycentric_u[best_index];
      result->barycentric_v[dst] = result->barycentric_v[best_index];
      result->visibility[dst] = result->visibility[best_index];
      result->bent_normals[3 * dst + 0] =
          result->bent_normals[3 * best_index + 0];
      result->bent_normals[3 * dst + 1] =
          result->bent_normals[3 * best_index + 1];
      result->bent_normals[3 * dst + 2] =
          result->bent_normals[3 * best_index + 2];
    }
  }

  return true;
}

bool Bake(const Input &input, const Options &options, Result *result,
          std::string *error) {
  if (!result) {
    if (error) *error = "null result";
    return false;
  }

  if (options.width <= 0 || options.height <= 0) {
    return Fail(result, error, "width and height must be positive");
  }
  if (!std::isfinite(options.radius) || !(options.radius > 0.0f)) {
    return Fail(result, error, "radius must be finite and > 0");
  }
  if (!std::isfinite(options.bias) || options.bias < 0.0f) {
    return Fail(result, error, "bias must be finite and >= 0");
  }
  if (options.dilation_radius < 0) {
    return Fail(result, error, "dilation radius must be >= 0");
  }

  // Directions: non-empty, finite, unit length, local +Z hemisphere.
  if (!input.directions || input.direction_count == 0 ||
      input.direction_count % 3 != 0) {
    return Fail(result, error, "directions must be a non-empty xyz array");
  }
  const std::size_t num_directions = input.direction_count / 3;
  for (std::size_t i = 0; i < num_directions; ++i) {
    const float *d = input.directions + 3 * i;
    if (!IsFinite3(d)) {
      return Fail(result, error, "direction contains a non-finite value");
    }
    const double len =
        std::sqrt(static_cast<double>(d[0]) * d[0] +
                  static_cast<double>(d[1]) * d[1] +
                  static_cast<double>(d[2]) * d[2]);
    if (std::fabs(len - 1.0) > static_cast<double>(kDirectionUnitTolerance)) {
      return Fail(result, error, "direction is not a unit vector");
    }
    if (d[2] < 0.0f) {
      return Fail(result, error,
                  "direction is outside the local +Z hemisphere");
    }
  }

  // Positions.
  if (input.position_count % 3 != 0) {
    return Fail(result, error, "position_count must be a multiple of 3");
  }
  if (input.position_count > 0 && !input.positions) {
    return Fail(result, error, "positions is null");
  }
  const std::size_t num_vertices = input.position_count / 3;
  for (std::size_t i = 0; i < num_vertices; ++i) {
    if (!IsFinite3(input.positions + 3 * i)) {
      return Fail(result, error, "position contains a non-finite value");
    }
  }

  // Triangles.
  if (input.triangle_index_count % 3 != 0) {
    return Fail(result, error, "triangle_index_count must be a multiple of 3");
  }
  if (input.triangle_index_count > 0 && !input.triangle_indices) {
    return Fail(result, error, "triangle_indices is null");
  }
  const std::size_t num_faces = input.triangle_index_count / 3;
  if (num_faces >
      static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
    return Fail(result, error, "too many triangles");
  }
  for (std::size_t i = 0; i < input.triangle_index_count; ++i) {
    if (static_cast<std::size_t>(input.triangle_indices[i]) >= num_vertices) {
      return Fail(result, error, "triangle index is out of range");
    }
  }

  // UVs and charts.
  if (num_faces > 0) {
    if (!input.face_uvs || input.face_uv_count != 6 * num_faces) {
      return Fail(result, error, "face_uvs must contain 6 floats per face");
    }
    if (!input.face_chart_ids || input.face_chart_id_count != num_faces) {
      return Fail(result, error,
                  "face_chart_ids must contain one id per face");
    }
  } else if (input.face_uv_count != 0 || input.face_chart_id_count != 0) {
    return Fail(result, error,
                "unexpected UV or chart data for empty geometry");
  }
  for (std::size_t i = 0; i < input.face_uv_count; ++i) {
    if (!IsFinite(input.face_uvs[i])) {
      return Fail(result, error, "UV contains a non-finite value");
    }
  }

  // Per-face geometry, UV plane, chart and tangent frame.
  std::vector<FaceGeometry> faces(num_faces);
  for (std::size_t f = 0; f < num_faces; ++f) {
    FaceGeometry &face = faces[f];
    for (int c = 0; c < 3; ++c) {
      const unsigned int vi =
          input.triangle_indices[3 * f + static_cast<std::size_t>(c)];
      const float *p = input.positions + 3 * static_cast<std::size_t>(vi);
      face.p[c] = Vec3d{p[0], p[1], p[2]};
    }

    const Vec3d e1 = Sub(face.p[1], face.p[0]);
    const Vec3d e2 = Sub(face.p[2], face.p[0]);
    Vec3d n = Cross(e1, e2);
    const double nlen = Length(n);
    if (!(nlen > 0.0) || !std::isfinite(nlen)) {
      return Fail(result, error, "face has zero world area");
    }
    face.normal = Vec3d{n.x / nlen, n.y / nlen, n.z / nlen};
    ComputeTangentFrameD(face.normal, &face.tangent, &face.bitangent);

    face.chart_id = input.face_chart_ids ? input.face_chart_ids[f] : 0;

    for (int c = 0; c < 3; ++c) {
      const float *uv =
          input.face_uvs + 6 * f + 2 * static_cast<std::size_t>(c);
      face.uv[c] = Vec3d{uv[0], uv[1], 0.0};
    }
    const Vec3d uve1 = Sub(face.uv[1], face.uv[0]);
    const Vec3d uve2 = Sub(face.uv[2], face.uv[0]);
    const double uv_area2 = uve1.x * uve2.y - uve1.y * uve2.x;
    face.active = (uv_area2 != 0.0) && std::isfinite(uv_area2);
    face.orientation = uv_area2 >= 0.0 ? 1.0 : -1.0;
  }

  result->Reset(options.width, options.height);

  // World triangle BVH shared by all occlusion queries.
  std::unique_ptr<nanort::BVHAccel<float> > accel;
  if (num_faces > 0) {
    nanort::BVHBuildOptions<float> build_options;
    build_options.cache_bbox = false;
    nanort::TriangleMesh<float> triangle_mesh(
        input.positions, input.triangle_indices, sizeof(float) * 3);
    nanort::TriangleSAHPred<float> triangle_pred(
        input.positions, input.triangle_indices, sizeof(float) * 3);
    accel.reset(new nanort::BVHAccel<float>());
    if (!accel->Build(static_cast<unsigned int>(num_faces), triangle_mesh,
                      triangle_pred, build_options)) {
      return Fail(result, error, "failed to build the triangle BVH");
    }
  }

  // ---------------------------------------------------------------------
  // Coverage: rasterize each active UV triangle and collect claims.
  // ---------------------------------------------------------------------
  const int width = options.width;
  const int height = options.height;
  const std::size_t num_pixels =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

  std::vector<std::int32_t> first_claim(num_pixels, -1);
  std::vector<std::uint8_t> claim_count(num_pixels, 0);
  std::unordered_map<std::size_t, std::vector<std::uint32_t> > extra_claims;

  for (std::size_t f = 0; f < num_faces; ++f) {
    const FaceGeometry &face = faces[f];
    if (!face.active) continue;

    const double umin =
        std::min(face.uv[0].x, std::min(face.uv[1].x, face.uv[2].x));
    const double umax =
        std::max(face.uv[0].x, std::max(face.uv[1].x, face.uv[2].x));
    const double vmin =
        std::min(face.uv[0].y, std::min(face.uv[1].y, face.uv[2].y));
    const double vmax =
        std::max(face.uv[0].y, std::max(face.uv[1].y, face.uv[2].y));

    int x0 = static_cast<int>(std::floor(umin * width - 0.5));
    int x1 = static_cast<int>(std::ceil(umax * width - 0.5));
    int y0 = static_cast<int>(std::floor(vmin * height - 0.5));
    int y1 = static_cast<int>(std::ceil(vmax * height - 0.5));
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(width - 1, x1);
    y1 = std::min(height - 1, y1);
    if (x0 > x1 || y0 > y1) continue;

    for (int y = y0; y <= y1; ++y) {
      const double pv = (y + 0.5) / static_cast<double>(height);
      for (int x = x0; x <= x1; ++x) {
        const double pu = (x + 0.5) / static_cast<double>(width);

        bool closed = true;
        for (int e = 0; e < 3; ++e) {
          const Vec3d &a = face.uv[e];
          const Vec3d &b = face.uv[(e + 1) % 3];
          const double d = EdgeDistance(a, b, pu, pv, face.orientation);
          if (d < -kOnEdgeEpsilon) {
            closed = false;
            break;
          }
        }
        if (!closed) continue;

        const std::size_t p = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
        if (claim_count[p] == 0) {
          first_claim[p] = static_cast<std::int32_t>(f);
          claim_count[p] = 1;
        } else {
          std::vector<std::uint32_t> &v = extra_claims[p];
          if (claim_count[p] == 1) {
            v.push_back(static_cast<std::uint32_t>(first_claim[p]));
            claim_count[p] = 2;
          }
          v.push_back(static_cast<std::uint32_t>(f));
        }
      }
    }
  }

  std::vector<std::uint32_t> claims;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t p = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
      claims.clear();
      if (claim_count[p] == 1) {
        claims.push_back(static_cast<std::uint32_t>(first_claim[p]));
      } else if (claim_count[p] >= 2) {
        const std::vector<std::uint32_t> &v = extra_claims[p];
        claims.assign(v.begin(), v.end());
      }
      if (claims.empty()) continue;

      const double center[2] = {
          (x + 0.5) / static_cast<double>(width),
          (y + 0.5) / static_cast<double>(height)};
      std::uint32_t covered_face = 0;
      double bary[3] = {0.0, 0.0, 0.0};
      const int resolved = ResolveCoverage(faces, claims, center, &covered_face,
                                           bary, error);
      if (resolved < 0) {
        return Fail(result, error, "UV overlap");
      }
      if (resolved == 0) continue;

      result->real_coverage[p] = 1;
      result->has_value[p] = 1;
      result->face_ids[p] = static_cast<std::int32_t>(covered_face);
      result->chart_ids[p] = faces[covered_face].chart_id;
      result->barycentric_u[p] = static_cast<float>(bary[1]);
      result->barycentric_v[p] = static_cast<float>(bary[2]);
    }
  }

  // ---------------------------------------------------------------------
  // Occlusion: every covered texel re-derives its own surface sample and rays.
  // ---------------------------------------------------------------------
  nanort::TriangleIntersector<float> intersector(
      input.positions, input.triangle_indices, sizeof(float) * 3);

  // NanoRT reports a hit only when its distance is strictly below max_t, so
  // setting max_t to the radius yields exactly the half-open [0, radius)
  // occlusion interval (a hit exactly at the radius does not occlude).
  const float ray_max_t = options.radius;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t p = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
      if (!result->real_coverage[p]) continue;

      const std::uint32_t face_id =
          static_cast<std::uint32_t>(result->face_ids[p]);
      const FaceGeometry &face = faces[face_id];

      // Reuse the exact surface reconstruction path from the legacy example.
      uv_surface::SurfaceSample sample;
      if (!uv_surface::ReconstructSample(
              input.positions, input.position_count, input.triangle_indices,
              input.triangle_index_count, nullptr, 0, face_id,
              result->barycentric_u[p], result->barycentric_v[p], &sample)) {
        return Fail(result, error, "failed to reconstruct the surface sample");
      }

      const double bx = static_cast<double>(sample.position[0]) +
                        static_cast<double>(options.bias) * face.normal.x;
      const double by = static_cast<double>(sample.position[1]) +
                        static_cast<double>(options.bias) * face.normal.y;
      const double bz = static_cast<double>(sample.position[2]) +
                        static_cast<double>(options.bias) * face.normal.z;

      nanort::BVHTraceOptions trace_options;
      trace_options.skip_prim_id = face_id;

      std::size_t unoccluded = 0;
      Vec3d sum{0.0, 0.0, 0.0};

      for (std::size_t d = 0; d < num_directions; ++d) {
        const float *local = input.directions + 3 * d;
        const Vec3d world = RotateLocal(face, local);

        nanort::Ray<float> ray;
        ray.org[0] = static_cast<float>(bx);
        ray.org[1] = static_cast<float>(by);
        ray.org[2] = static_cast<float>(bz);
        ray.dir[0] = static_cast<float>(world.x);
        ray.dir[1] = static_cast<float>(world.y);
        ray.dir[2] = static_cast<float>(world.z);
        ray.min_t = 0.0f;
        ray.max_t = ray_max_t;

        nanort::TriangleIntersection<float> isect;
        const bool hit =
            accel && accel->Traverse(ray, intersector, &isect, trace_options);
        if (!hit) {
          ++unoccluded;
          sum.x += world.x;
          sum.y += world.y;
          sum.z += world.z;
        }
      }

      result->visibility[p] = static_cast<float>(
          static_cast<double>(unoccluded) / static_cast<double>(num_directions));

      const double sum_len = Length(sum);
      if (unoccluded > 0 && sum_len > 0.0) {
        result->bent_normals[3 * p + 0] =
            static_cast<float>(sum.x / sum_len);
        result->bent_normals[3 * p + 1] =
            static_cast<float>(sum.y / sum_len);
        result->bent_normals[3 * p + 2] =
            static_cast<float>(sum.z / sum_len);
      }
    }
  }

  if (options.dilation_radius > 0) {
    if (!Dilate(result, options.dilation_radius, error)) {
      return Fail(result, error, "dilation failed");
    }
  }

  return true;
}

}  // namespace uv_baker
