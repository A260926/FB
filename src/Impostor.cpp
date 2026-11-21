#include "Impostor.h"

#include "ClipSpace.h"
#include "Graphics3D.h"
#include "Grid.h"
#include "TriangleDrawer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace al { namespace graphics {
namespace {

typedef Grid<double> ImpostorZBuffer;
// ---------------------------------------------------------------------------
// Small local-space helpers
// ---------------------------------------------------------------------------

bool finite(float v) { return std::isfinite(v); }

bool finiteVector(const Vector3f& v)
{
  return finite(v.x()) && finite(v.y()) && finite(v.z());
}

float dot3(const Vector3f& a, const Vector3f& b)
{
  return a.x()*b.x() + a.y()*b.y() + a.z()*b.z();
}

Vector3f cross3(const Vector3f& a, const Vector3f& b)
{
  return Vector3f(a.y()*b.z() - a.z()*b.y(),
                  a.z()*b.x() - a.x()*b.z(),
                  a.x()*b.y() - a.y()*b.x());
}

float length3(const Vector3f& v) { return std::sqrt(dot3(v, v)); }

Vector3f sub3(const Vector3f& a, const Vector3f& b)
{
  return Vector3f(a.x()-b.x(), a.y()-b.y(), a.z()-b.z());
}

float maxComponentAbs(const Vector3f& v)
{
  return std::max(std::abs(v.x()), std::max(std::abs(v.y()), std::abs(v.z())));
}

// A valid unit vector: finite and length within a small tolerance of 1.
bool unitVector(const Vector3f& v)
{
  return finiteVector(v) && std::abs(length3(v) - 1.0f) <= 1e-3f;
}

// Unpacks a 0xAABBGGRR pixel into linear rgb floats (RGB/255). ImageData
// stores channel 0 in the low byte.
Vector3f unpackColor(std::uint32_t pixel)
{
  return Vector3f(static_cast<float>(pixel & 0xffu) / 255.0f,
                  static_cast<float>((pixel >> 8) & 0xffu) / 255.0f,
                  static_cast<float>((pixel >> 16) & 0xffu) / 255.0f);
}

// ---------------------------------------------------------------------------
// Capture validation
// ---------------------------------------------------------------------------

bool validView(const CaptureView& view)
{
  if (!finiteVector(view.eye) ||
      !unitVector(view.right) ||
      !unitVector(view.up) ||
      !unitVector(view.forward)) {
    return false;
  }
  if (!finite(view.halfWidth) || !(view.halfWidth > 0) ||
      !finite(view.halfHeight) || !(view.halfHeight > 0)) {
    return false;
  }
  if (!finite(view.nearPlane) || !finite(view.farPlane) ||
      !(view.nearPlane > 0) || !(view.nearPlane < view.farPlane)) {
    return false;
  }
  if (view.width == 0 || view.height == 0) {
    return false;
  }
  // forward must equal right x up.
  const Vector3f expected = cross3(view.right, view.up);
  return maxComponentAbs(sub3(expected, view.forward)) <= 1e-3f;
}

// True for a non-degenerate (in local space) triangle.
bool validSourceTriangle(const Triangle& t)
{
  for (int i = 0; i < 3; ++i) {
    const Vector4f& p = t.vertex(i).pos;
    if (!finite(p.x()) || !finite(p.y()) || !finite(p.z()) ||
        !finite(p.w()) || !(p.w() > 0)) {
      return false;
    }
  }
  const Vector3f a(t[0].x(), t[0].y(), t[0].z());
  const Vector3f b(t[1].x(), t[1].y(), t[1].z());
  const Vector3f c(t[2].x(), t[2].y(), t[2].z());
  const Vector3f n = cross3(sub3(b, a), sub3(c, a));
  const Vector3f lo(std::min(a.x(), std::min(b.x(), c.x())),
                    std::min(a.y(), std::min(b.y(), c.y())),
                    std::min(a.z(), std::min(b.z(), c.z())));
  const Vector3f hi(std::max(a.x(), std::max(b.x(), c.x())),
                    std::max(a.y(), std::max(b.y(), c.y())),
                    std::max(a.z(), std::max(b.z(), c.z())));
  const float extent = std::max(1.0f, maxComponentAbs(sub3(hi, lo)));
  // Degenerate when the normal length vanishes relative to the triangle size.
  return length3(n) > 1e-6f * extent * extent;
}

// Builds this view's reconstruction micro triangles.
//
// Every 2x2 cell is split into (00,10,11) and (00,11,01). A triangle is kept
// only when all three corner records are valid and all three local-space edge
// lengths are <= maxEdge. Holes are left unfilled; no bridge crosses a depth
// discontinuity because a discontinuity shows up as a long edge.
void buildViewTriangles(ImpostorView& viewData, float maxEdge)
{
  const CaptureView& view = viewData.view;
  const std::uint32_t width = view.width;
  const std::uint32_t height = view.height;
  std::vector<ImpostorRecord>& records = viewData.records;
  std::vector<std::uint32_t>& tris = viewData.triangles;

  auto edgeOk = [&](std::uint32_t ia, std::uint32_t ib) -> bool {
    return length3(sub3(records[ia].position, records[ib].position)) <= maxEdge;
  };

  auto tryCell = [&](std::uint32_t i0, std::uint32_t i1, std::uint32_t i2) {
    if (!records[i0].valid || !records[i1].valid || !records[i2].valid) {
      return;
    }
    if (!edgeOk(i0, i1) || !edgeOk(i1, i2) || !edgeOk(i2, i0)) {
      return;
    }
    tris.push_back(i0);
    tris.push_back(i1);
    tris.push_back(i2);
  };

  if (width < 2 || height < 2) {
    return;
  }
  for (std::uint32_t y = 0; y + 1 < height; ++y) {
    for (std::uint32_t x = 0; x + 1 < width; ++x) {
      const std::uint32_t i00 = y * width + x;
      const std::uint32_t i10 = y * width + (x + 1);
      const std::uint32_t i01 = (y + 1) * width + x;
      const std::uint32_t i11 = (y + 1) * width + (x + 1);
      tryCell(i00, i10, i11);
      tryCell(i00, i11, i01);
    }
  }
}

bool captureValidated(const std::vector<Triangle>& inputTriangles,
                      const std::vector<CaptureView>& views,
                      float maxEdge,
                      std::vector<ImpostorView>& built)
{
  // ---- Validate everything before touching out. ----
  if (!finite(maxEdge) || !(maxEdge > 0)) {
    return false;
  }
  if (views.empty() || inputTriangles.empty()) {
    return false;
  }
  for (const CaptureView& v : views) {
    if (!validView(v)) {
      return false;
    }
  }
  for (const Triangle& t : inputTriangles) {
    if (!validSourceTriangle(t)) {
      return false;
    }
    if (((t.color >> 24) & 0xffu) != 0xffu) {
      return false;
    }
  }

  std::vector<ImpostorView> result;
  result.reserve(views.size());

  for (const CaptureView& view : views) {
    ImpostorView captured;
    captured.view = view;

    const std::uint32_t width = view.width;
    const std::uint32_t height = view.height;
    const std::size_t pixelCount =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    // Per-view targets: the single final depth commit updates color,
    // position, normal and valid together.
    ImageData color(width, height);
    color.fill(0);
    ImpostorZBuffer depth(width, height);
    depth.fill(std::numeric_limits<double>::infinity());
    std::vector<FragmentRecord> records(pixelCount);

    // Local -> this orthographic view's clip space. w is the depth along
    // forward, so the rasterizer's projected depth (ndc z) is the true
    // orthographic depth rather than the constant w of a raw ortho matrix.
    const float nearZ = view.nearPlane;
    const float farZ = view.farPlane;
    const float zScale = 2.0f / (farZ - nearZ);
    const float zBias = -(farZ + nearZ) / (farZ - nearZ);
    const float sx = 1.0f / view.halfWidth;
    const float sy = 1.0f / view.halfHeight;
    const float eyeRight = -dot3(view.eye, view.right);
    const float eyeUp = -dot3(view.eye, view.up);
    const float eyeForward = -dot3(view.eye, view.forward);

    // Build by rows (Matrix4 storage is column-major). This is a true
    // orthographic projection: w stays 1 (so x/y are not foreshortened) while
    // z encodes the real distance along forward. That makes the rasterizer's
    // projected depth vary with depth instead of being the constant w that a
    // raw ortho matrix would otherwise give.
    //   x = sx * (right . (p-eye))
    //   y = sy * (up    . (p-eye))
    //   z = zScale * (forward . (p-eye)) + zBias
    //   w = 1
    Matrix4f toClip;
    toClip.fill(0.0f);
    auto setRow = [&](int row, const Vector3f& axis, float translation) {
      toClip.set(0, row, axis.x());
      toClip.set(1, row, axis.y());
      toClip.set(2, row, axis.z());
      toClip.set(3, row, translation);
    };
    setRow(0, Vector3f(view.right.x()*sx, view.right.y()*sx,
                       view.right.z()*sx), eyeRight*sx);
    setRow(1, Vector3f(view.up.x()*sy, view.up.y()*sy,
                       view.up.z()*sy), eyeUp*sy);
    setRow(2, Vector3f(view.forward.x()*zScale, view.forward.y()*zScale,
                       view.forward.z()*zScale), eyeForward*zScale + zBias);
    setRow(3, Vector3f(0.0f, 0.0f, 0.0f), 1.0f);

    TriangleDrawer drawer(&depth);
    drawer.algo = TriangleDrawer::BARYCENTRIC;
    drawer.drawMode = TriangleDrawer::DRAW_SOLID;
    drawer.lightsStyle = TriangleDrawer::LIGHTS_STYLE_NONE;
    drawer.useProjectedDepth = true;
    drawer.recordFragments = true;
    drawer.doubleSided = true;
    drawer.fragmentRecords = &records;

    const std::size_t numSource = inputTriangles.size();
    for (std::size_t sourceIndex = 0; sourceIndex < numSource; ++sourceIndex) {
      const Triangle& source = inputTriangles[sourceIndex];
      const Vector3f a(source[0].x(), source[0].y(), source[0].z());
      const Vector3f b(source[1].x(), source[1].y(), source[1].z());
      const Vector3f c(source[2].x(), source[2].y(), source[2].z());
      // Preserve the original face winding: normal = (b-a) x (c-a).
      const Vector3f faceNormal =
          G3D::normalize(cross3(sub3(b, a), sub3(c, a)));

      // Clip-space vertices plus the original homogeneous w per vertex, used
      // to pre-divide the perspective-correct attributes.
      Triangle clip(source);
      clip.color = source.color;
      for (int i = 0; i < 3; ++i) {
        const Vector4f cp = toClip * source[i].pos;
        clip[i].pos = cp;
        clip[i].rgb = Vector3f(source[i].x(), source[i].y(), source[i].z());
        clip[i].normal = faceNormal;
      }
      // Nothing to draw if the whole triangle is clipped away by the near
      // plane (all vertices have non-positive w).
      const Vector4f& c0 = clip[0].pos;
      const Vector4f& c1 = clip[1].pos;
      const Vector4f& c2 = clip[2].pos;
      if (!(c0.w() > 0) && !(c1.w() > 0) && !(c2.w() > 0)) {
        continue;
      }

      const std::vector<Triangle> pieces =
          ClipSpace::clipByAllPlanes(std::vector<Triangle>{clip});

      for (const Triangle& piece : pieces) {
        Triangle out(piece);
        out.color = source.color;
        for (int i = 0; i < 3; ++i) {
          const Vector4f& cp = piece[i].pos;
          const float w = cp.w();
          const float inv = w != 0.0f ? 1.0f / w : 0.0f;
          const float ndcX = cp.x() * inv;
          const float ndcY = cp.y() * inv;
          const float ndcZ = cp.z() * inv;
          // pos is NDC x/y/z plus the original homogeneous w, matching the
          // rasterizer's DRAW_PERSPECTIVE attribute convention. The copy from
          // piece keeps the raw attribute in rgb / normal; the rasterizer
          // divides by w internally.
          out[i].pos = Vector4f(ndcX, ndcY, ndcZ, w);
        }
        for (int i = 0; i < 3; ++i) {
          G3D::ndcToWindow(out[i].pos, static_cast<float>(width),
                           static_cast<float>(height));
        }
        drawer.triangle(color, out);
      }
    }

    // Reduce the winning fragments into records.
    captured.records.resize(pixelCount);
    for (std::size_t i = 0; i < pixelCount; ++i) {
      const FragmentRecord& rec = records[i];
      if (!rec.valid) {
        continue;
      }
      ImpostorRecord& dst = captured.records[i];
      dst.valid = true;
      dst.color = unpackColor(rec.color);
      dst.position = rec.attribute;
      dst.normal = G3D::normalize(rec.normal);
      dst.depth = dot3(sub3(dst.position, view.eye), view.forward);
    }

    buildViewTriangles(captured, maxEdge);
    result.push_back(std::move(captured));
  }

  // ---- Commit atomically. ----
  built.swap(result);
  return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

bool finiteMatrix(const Matrix4f& m)
{
  for (int i = 0; i < 16; ++i) {
    if (!finite(m.get(i))) {
      return false;
    }
  }
  return true;
}

// True if the upper-left 3x3 block is non-singular.
bool reversibleAffine(const Matrix4f& m)
{
  const double a = m.get(0,0), b = m.get(1,0), c = m.get(2,0);
  const double d = m.get(0,1), e = m.get(1,1), f = m.get(2,1);
  const double g = m.get(0,2), h = m.get(1,2), i = m.get(2,2);
  const double det = a*(e*i - f*h) - b*(d*i - f*g) + c*(d*h - e*g);
  return det > 1e-12 || det < -1e-12;
}

// A clipped world-space vertex: clip position plus float attributes.
struct WorldVertex
{
  Vector4f clip;
  Vector3f color;       // float RGB (unquantized)
  Vector3f position;    // local position attribute
  Vector3f normal;      // local unit normal attribute
};

std::vector<WorldVertex> clipWorldTriangle(const WorldVertex in[3])
{
  std::vector<WorldVertex> poly(in, in + 3);
  for (int ixyz = 0; ixyz < 3; ++ixyz) {
    for (int sign = -1; sign <= 1; sign += 2) {
      if (poly.empty()) {
        return poly;
      }
      std::vector<WorldVertex> out;
      out.reserve(poly.size() + 1);
      const std::size_t n = poly.size();
      for (std::size_t i = 0; i < n; ++i) {
        const WorldVertex& cur = poly[i];
        const WorldVertex& nxt = poly[(i + 1) % n];
        const double dc = static_cast<double>(cur.clip.get(ixyz)) -
                          static_cast<double>(cur.clip.w()) * sign;
        const double dn = static_cast<double>(nxt.clip.get(ixyz)) -
                          static_cast<double>(nxt.clip.w()) * sign;
        const bool curIn = sign < 0 ? dc >= 0.0 : dc <= 0.0;
        const bool nxtIn = sign < 0 ? dn >= 0.0 : dn <= 0.0;
        if (curIn) {
          out.push_back(cur);
        }
        if (curIn != nxtIn) {
          const double denom = dc - dn;
          const float t = denom != 0.0 ? static_cast<float>(dc / denom) : 0.0f;
          WorldVertex mid;
          mid.clip = cur.clip.lerp(nxt.clip, t);
          mid.color = cur.color.lerp(nxt.color, t);
          mid.position = cur.position.lerp(nxt.position, t);
          mid.normal = cur.normal.lerp(nxt.normal, t);
          out.push_back(mid);
        }
      }
      poly.swap(out);
    }
  }
  return poly;
}

bool drawValidated(const DepthImpostor& impostor,
                   const Matrix4f& localToWorld,
                   const Matrix4f& worldToClip,
                   const Vector3f& cameraWorld,
                   ImageData& colorOut,
                   std::vector<float>& depthOut,
                   float mergeDistance)
{
  if (!finiteMatrix(localToWorld) || !finiteMatrix(worldToClip) ||
      !finiteVector(cameraWorld)) {
    return false;
  }
  if (!reversibleAffine(localToWorld)) {
    return false;
  }
  if (!finite(mergeDistance) || mergeDistance < 0) {
    return false;
  }
  if (colorOut.width() == 0 || colorOut.height() == 0) {
    return false;
  }
  const std::uint32_t width = colorOut.width();
  const std::uint32_t height = colorOut.height();
  const std::size_t pixelCount =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (depthOut.size() != pixelCount) {
    return false;
  }

  float normalMatrix[9];
  if (!localToWorld.inverseTranspose3x3(normalMatrix)) {
    return false;
  }

  // All validation done. From here the caller buffers may change.
  // The caller's depth target is treated as an existing occluder set: it is
  // not reset, and a fragment only writes when strictly nearer than the
  // current value there. Callers that want an empty target initialise it to
  // +inf themselves.

  const int numViews = static_cast<int>(impostor.viewCount());

  // Per-pixel merged candidate arrival. Each (view, pixel) candidate is
  // reduced into candidateSets[pixel]; a view contributes at most one
  // candidate per pixel.
  struct PixelCandidates
  {
    Vector3f color;
    Vector3f worldPosition;
    Vector3f worldNormal;
    float cameraDistance;
    float weight;
    bool valid;
    PixelCandidates()
    : color(), worldPosition(), worldNormal(), cameraDistance(0), weight(0),
      valid(false) {}
  };
  std::vector<std::vector<PixelCandidates>> candidateSets(pixelCount);

  for (int viewIndex = 0; viewIndex < numViews; ++viewIndex) {
    const ImpostorView& view = impostor.view(viewIndex);

    // Independent per-view scratch state.
    ImageData viewColor(width, height);
    viewColor.fill(0);
    ImpostorZBuffer viewDepth(width, height);
    viewDepth.fill(std::numeric_limits<double>::infinity());
    std::vector<FragmentRecord> records(pixelCount);

    TriangleDrawer drawer(&viewDepth);
    drawer.algo = TriangleDrawer::BARYCENTRIC;
    drawer.drawMode = TriangleDrawer::DRAW_SOLID;
    drawer.lightsStyle = TriangleDrawer::LIGHTS_STYLE_NONE;
    drawer.useProjectedDepth = true;
    drawer.recordFragments = true;
    drawer.doubleSided = true;
    drawer.fragmentRecords = &records;

    // Per-emitted-triangle vertex data. rgb carries perspective-correct
    // float color; this table lets us rebuild the perspective-correct local
    // position from the recorded barycentrics (no intermediate byte image).
    struct EmittedVertex
    {
      Vector3f position;
      float invW;
    };
    std::vector<EmittedVertex> emitted;
    int emittedCount = 0;

    const std::size_t triCount = view.triangles.size() / 3;
    for (std::size_t t = 0; t < triCount; ++t) {
      const std::uint32_t ia = view.triangles[t * 3 + 0];
      const std::uint32_t ib = view.triangles[t * 3 + 1];
      const std::uint32_t ic = view.triangles[t * 3 + 2];
      if (ia >= view.records.size() || ib >= view.records.size() ||
          ic >= view.records.size()) {
        continue;
      }
      const ImpostorRecord& ra = view.records[ia];
      const ImpostorRecord& rb = view.records[ib];
      const ImpostorRecord& rc = view.records[ic];
      if (!ra.valid || !rb.valid || !rc.valid) {
        continue;
      }

      const ImpostorRecord* recs[3] = {&ra, &rb, &rc};
      WorldVertex world[3];
      float maxW = 0.0f;
      for (int i = 0; i < 3; ++i) {
        const Vector4f local(recs[i]->position.x(), recs[i]->position.y(),
                             recs[i]->position.z(), 1.0f);
        const Vector4f worldPos = localToWorld * local;
        world[i].clip = worldToClip * worldPos;
        world[i].color = recs[i]->color;
        world[i].position = recs[i]->position;
        world[i].normal = recs[i]->normal;
        maxW = std::max(maxW, world[i].clip.w());
      }
      if (maxW <= 0.0f) {
        continue;
      }

      const std::vector<WorldVertex> clipped = clipWorldTriangle(world);
      if (clipped.size() < 3) {
        continue;
      }

      for (std::size_t k = 1; k + 1 < clipped.size(); ++k) {
        const WorldVertex* vs[3] = {&clipped[0], &clipped[k], &clipped[k + 1]};
        Triangle tri;
        tri.color = 0xffffffffu;
        tri.textureID = emittedCount;
        bool skip = false;
        EmittedVertex ev[3];
        for (int i = 0; i < 3; ++i) {
          const float w = vs[i]->clip.w();
          if (!finite(w) || !(w > 0)) {
            skip = true;
            break;
          }
          const float inv = 1.0f / w;
          tri[i].pos = Vector4f(vs[i]->clip.x() * inv,
                                vs[i]->clip.y() * inv,
                                vs[i]->clip.z() * inv,
                                w);
          // Raw attributes; the rasterizer divides by w internally.
          tri[i].rgb = vs[i]->color;
          tri[i].normal = vs[i]->normal;
          ev[i].position = vs[i]->position;
          ev[i].invW = inv;
        }
        if (skip) {
          continue;
        }
        for (int i = 0; i < 3; ++i) {
          G3D::ndcToWindow(tri[i].pos, static_cast<float>(width),
                           static_cast<float>(height));
        }
        emitted.push_back(ev[0]);
        emitted.push_back(ev[1]);
        emitted.push_back(ev[2]);
        ++emittedCount;
        drawer.triangle(viewColor, tri);
      }
    }

    // Reduce this view's winning fragments into merged candidates.
    for (std::size_t i = 0; i < pixelCount; ++i) {
      const FragmentRecord& rec = records[i];
      if (!rec.valid) {
        continue;
      }
      // Perspective-correct local position from the recorded barycentrics.
      const int id = rec.triangleId;
      if (id < 0 || static_cast<std::size_t>(id) * 3 + 2 >= emitted.size()) {
        continue;
      }
      const EmittedVertex& e0 = emitted[static_cast<std::size_t>(id) * 3 + 0];
      const EmittedVertex& e1 = emitted[static_cast<std::size_t>(id) * 3 + 1];
      const EmittedVertex& e2 = emitted[static_cast<std::size_t>(id) * 3 + 2];
      const float denom = rec.bary[0]*e0.invW + rec.bary[1]*e1.invW +
                          rec.bary[2]*e2.invW;
      if (!(std::abs(denom) > 1e-12f)) {
        continue;
      }
      const float invDenom = 1.0f / denom;
      const Vector3f localPosition(
        (e0.position.x()*e0.invW*rec.bary[0] +
         e1.position.x()*e1.invW*rec.bary[1] +
         e2.position.x()*e2.invW*rec.bary[2]) * invDenom,
        (e0.position.y()*e0.invW*rec.bary[0] +
         e1.position.y()*e1.invW*rec.bary[1] +
         e2.position.y()*e2.invW*rec.bary[2]) * invDenom,
        (e0.position.z()*e0.invW*rec.bary[0] +
         e1.position.z()*e1.invW*rec.bary[1] +
         e2.position.z()*e2.invW*rec.bary[2]) * invDenom);

      // World-space normal via inverse-transpose of localToWorld.
      const float lx = rec.normal.x(), ly = rec.normal.y(), lz = rec.normal.z();
      Vector3f worldN(
        normalMatrix[0]*lx + normalMatrix[3]*ly + normalMatrix[6]*lz,
        normalMatrix[1]*lx + normalMatrix[4]*ly + normalMatrix[7]*lz,
        normalMatrix[2]*lx + normalMatrix[5]*ly + normalMatrix[8]*lz);
      worldN = G3D::normalize(worldN);
      const Vector4f worldPos = localToWorld *
          Vector4f(localPosition.x(), localPosition.y(), localPosition.z(),
                   1.0f);
      const Vector3f wp(worldPos.x(), worldPos.y(), worldPos.z());
      const Vector3f toCamera = sub3(cameraWorld, wp);
      const float dist = length3(toCamera);
      const Vector3f viewDir = dist > 0.0f
                                   ? Vector3f(toCamera.x()/dist,
                                              toCamera.y()/dist,
                                              toCamera.z()/dist)
                                   : Vector3f();
      const float facing = std::max(dot3(worldN, viewDir), 0.0f);
      const float facing2 = facing * facing;
      const float weight = facing2 * facing2;
      if (!(weight > 0.0f)) {
        continue;
      }
      PixelCandidates cand;
      cand.valid = true;
      cand.color = rec.attribute;
      cand.worldPosition = wp;
      cand.worldNormal = worldN;
      cand.cameraDistance = dist;
      cand.weight = weight;
      candidateSets[i].push_back(cand);
    }
  }

  // Merge and depth-test per pixel.
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * width + x;
      const std::vector<PixelCandidates>& cands = candidateSets[i];
      if (cands.empty()) {
        continue;
      }

      // Sort into a canonical order so the blend sums do not depend on the
      // order in which source views were supplied. This keeps the result
      // invariant (to floating-point rounding) when views are reordered.
      std::vector<PixelCandidates> ordered = cands;
      std::sort(ordered.begin(), ordered.end(),
                [](const PixelCandidates& a, const PixelCandidates& b) {
                  if (a.worldPosition.x() != b.worldPosition.x())
                    return a.worldPosition.x() < b.worldPosition.x();
                  if (a.worldPosition.y() != b.worldPosition.y())
                    return a.worldPosition.y() < b.worldPosition.y();
                  if (a.worldPosition.z() != b.worldPosition.z())
                    return a.worldPosition.z() < b.worldPosition.z();
                  if (a.color.x() != b.color.x())
                    return a.color.x() < b.color.x();
                  if (a.color.y() != b.color.y())
                    return a.color.y() < b.color.y();
                  return a.color.z() < b.color.z();
                });

      // Drop zero-weight candidates first. The nearest distance and every
      // following blend decision use only the surviving (positive-weight)
      // candidates.
      const PixelCandidates* nearest = nullptr;
      for (const PixelCandidates& c : ordered) {
        if (!(c.weight > 0.0f)) {
          continue;
        }
        if (!nearest || c.cameraDistance < nearest->cameraDistance) {
          nearest = &c;
        }
      }
      if (!nearest) {
        continue;
      }
      const float limit = nearest->cameraDistance + mergeDistance;

      // Nearest positive-weight candidate within the merge limit provides
      // the final depth.
      const PixelCandidates* depthSource = nullptr;
      float colorSum[3] = {0.0f, 0.0f, 0.0f};
      float weightSum = 0.0f;
      for (const PixelCandidates& c : ordered) {
        if (!(c.weight > 0.0f)) {
          continue;
        }
        if (c.cameraDistance > limit) {
          continue;
        }
        colorSum[0] += c.color.x() * c.weight;
        colorSum[1] += c.color.y() * c.weight;
        colorSum[2] += c.color.z() * c.weight;
        weightSum += c.weight;
        if (!depthSource || c.cameraDistance < depthSource->cameraDistance) {
          depthSource = &c;
        }
      }
      if (!depthSource || !(weightSum > 0.0f)) {
        continue;
      }

      const float invWeight = 1.0f / weightSum;
      const Vector3f blended(colorSum[0] * invWeight,
                             colorSum[1] * invWeight,
                             colorSum[2] * invWeight);

      // Final depth is the nearest positive-weight candidate's world
      // distance. Reject if not strictly nearer than the existing depth.
      const float finalDepth = depthSource->cameraDistance;
      if (!(finalDepth < depthOut[i])) {
        continue;
      }
      // Compose the color in the ImageData byte layout (channel 0 = lowest
      // byte) directly from floats. This is the only quantization step.
      const std::uint32_t pixel = ImageData::makePixel(
        static_cast<u8>(blended.x() * 255.0f + 0.5f),
        static_cast<u8>(blended.y() * 255.0f + 0.5f),
        static_cast<u8>(blended.z() * 255.0f + 0.5f),
        255);
      colorOut.setPixel(x, y, pixel);
      depthOut[i] = finalDepth;
    }
  }

  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

DepthImpostor::DepthImpostor() : views_() {}

void DepthImpostor::swap(DepthImpostor& other)
{
  views_.swap(other.views_);
}

bool captureImpostor(const std::vector<Triangle>& inputTriangles,
                     const std::vector<CaptureView>& views,
                     float maxEdge,
                     DepthImpostor& out)
{
  std::vector<ImpostorView> built;
  if (!captureValidated(inputTriangles, views, maxEdge, built)) {
    return false;
  }
  out.views_.swap(built);
  return true;
}

bool drawImpostor(const DepthImpostor& impostor,
                  const Matrix4f& localToWorld,
                  const Matrix4f& worldToClip,
                  const Vector3f& cameraWorld,
                  ImageData& colorOut,
                  std::vector<float>& depthOut,
                  float mergeDistance)
{
  return drawValidated(impostor, localToWorld, worldToClip, cameraWorld,
                       colorOut, depthOut, mergeDistance);
}

} // namespace graphics
} // namespace al
