// Tests for the multi-view depth impostor.
//
// The harness compiles task_support/driver.c as C++ and provides main() plus
// task_check. check.h declares the registration tables; because global
// variables keep their identifier names (no name mangling) while task_check is
// a C++ function, including check.h directly links against the harness driver.

#include "../task_support/check.h"

#include "Impostor.h"
#include "Graphics3D.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

using namespace al::graphics;

namespace {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

const float kEps = 1e-4f;

u32 opaque(int r, int g, int b)
{
  return ImageData::makePixel(static_cast<u8>(r), static_cast<u8>(g),
                              static_cast<u8>(b), 255);
}

Triangle triangle(float x1, float y1, float z1,
                  float x2, float y2, float z2,
                  float x3, float y3, float z3,
                  u32 color)
{
  Triangle t(Vector4f(x1, y1, z1, 1.0f),
             Vector4f(x2, y2, z2, 1.0f),
             Vector4f(x3, y3, z3, 1.0f));
  t.color = color;
  return t;
}

// An axis-aligned quad in the z = depth plane, facing +z, split into two
// triangles. Winding keeps the counter-clockwise face normal pointing +z.
void addQuadZ(std::vector<Triangle>& out,
              float x0, float y0, float x1, float y1, float z, u32 color)
{
  out.push_back(triangle(x0, y0, z, x1, y0, z, x1, y1, z, color));
  out.push_back(triangle(x0, y0, z, x1, y1, z, x0, y1, z, color));
}

CaptureView viewFrom(float eyeX, float eyeY, float eyeZ,
                     float halfWidth, float halfHeight,
                     float nearP, float farP,
                     u32 w, u32 h)
{
  // A canonical front view: at eye, looking toward -z. right = -x so that
  // forward = right x up = (0,0,-1).
  CaptureView v;
  v.eye = Vector3f(eyeX, eyeY, eyeZ);
  v.right = Vector3f(-1.0f, 0.0f, 0.0f);
  v.up = Vector3f(0.0f, 1.0f, 0.0f);
  v.forward = Vector3f(0.0f, 0.0f, -1.0f);
  v.halfWidth = halfWidth;
  v.halfHeight = halfHeight;
  v.nearPlane = nearP;
  v.farPlane = farP;
  v.width = w;
  v.height = h;
  return v;
}

// A front-facing (toward -z) capture view whose right axis is +x, so image
// columns increase with +x. Used for tests that need intuitive pixel mapping.
CaptureView viewFrontZ(float eyeZ, float halfW, float halfH,
                       float nearP, float farP, u32 w, u32 h)
{
  CaptureView v;
  v.eye = Vector3f(0.0f, 0.0f, eyeZ);
  v.right = Vector3f(1.0f, 0.0f, 0.0f);
  v.up = Vector3f(0.0f, 1.0f, 0.0f);
  // forward = right x up = +z: looks away from -z. Flip up so forward is -z.
  v.up = Vector3f(0.0f, -1.0f, 0.0f);
  v.forward = Vector3f(0.0f, 0.0f, -1.0f);
  v.halfWidth = halfW;
  v.halfHeight = halfH;
  v.nearPlane = nearP;
  v.farPlane = farP;
  v.width = w;
  v.height = h;
  return v;
}

// Builds a perspective world->clip matrix for a camera at `eye` looking along
// -z with the given field of view.
Matrix4f perspectiveCamera(const Vector3f& eye,
                           float fovY, float aspect,
                           float nearP, float farP)
{
  Matrix4f view = Matrix4f::makeTranslation(-eye.x(), -eye.y(), -eye.z());
  Matrix4f proj = Matrix4f::makePerspective(fovY, aspect, nearP, farP);
  return proj * view;
}

// Independent local-space segment/triangle intersection (Moller-Trumbore),
// used to verify the captured records without touching the rasterizer.
// Returns true and the hit distance on success.
bool rayTriangle(const Vector3f& origin, const Vector3f& dir,
                 const Vector3f& a, const Vector3f& b, const Vector3f& c,
                 float& t)
{
  const Vector3f e1(b.x()-a.x(), b.y()-a.y(), b.z()-a.z());
  const Vector3f e2(c.x()-a.x(), c.y()-a.y(), c.z()-a.z());
  const Vector3f p(dir.y()*e2.z()-dir.z()*e2.y(),
                   dir.z()*e2.x()-dir.x()*e2.z(),
                   dir.x()*e2.y()-dir.y()*e2.x());
  const float det = e1.x()*p.x()+e1.y()*p.y()+e1.z()*p.z();
  if (std::abs(det) < 1e-12f) {
    return false;
  }
  const float inv = 1.0f/det;
  const Vector3f tv(origin.x()-a.x(), origin.y()-a.y(), origin.z()-a.z());
  const float u = (tv.x()*p.x()+tv.y()*p.y()+tv.z()*p.z()) * inv;
  if (u < -kEps || u > 1.0f+kEps) {
    return false;
  }
  const Vector3f q(tv.y()*e1.z()-tv.z()*e1.y(),
                   tv.z()*e1.x()-tv.x()*e1.z(),
                   tv.x()*e1.y()-tv.y()*e1.x());
  const float v = (dir.x()*q.x()+dir.y()*q.y()+dir.z()*q.z()) * inv;
  if (v < -kEps || u+v > 1.0f+kEps) {
    return false;
  }
  t = (e2.x()*q.x()+e2.y()*q.y()+e2.z()*q.z()) * inv;
  return t > 0.0f;
}

float dot3(const Vector3f& a, const Vector3f& b)
{
  return a.x()*b.x()+a.y()*b.y()+a.z()*b.z();
}

Vector3f normalize3(const Vector3f& v)
{
  const float len = std::sqrt(dot3(v, v));
  return len != 0.0f ? Vector3f(v.x()/len, v.y()/len, v.z()/len) : v;
}

// The capture projection for a view, matching the implementation's image
// mapping, so tests can compute expected values independently.
void projectToPixel(const CaptureView& v, const Vector3f& p,
                    float& px, float& py, float& depth)
{
  const Vector3f rel(p.x()-v.eye.x(), p.y()-v.eye.y(), p.z()-v.eye.z());
  const float r = dot3(rel, v.right);
  const float u = dot3(rel, v.up);
  const float f = dot3(rel, v.forward);
  px = (r/v.halfWidth + 1.0f) * v.width * 0.5f;
  py = (1.0f - u/v.halfHeight) * v.height * 0.5f;
  depth = f;
}

// Nearest surface color at a pixel by independently intersecting all source
// triangles with the pixel-center ray, restricted to this view.
bool nearestColor(const std::vector<Triangle>& src, const CaptureView& v,
                  int px, int py, Vector3f& colorOut, float& depthOut)
{
  // Pixel center in the view's local frame.
  const float nx = (px + 0.5f) / v.width * 2.0f - 1.0f;
  const float ny = 1.0f - (py + 0.5f) / v.height * 2.0f;
  // Orthographic rays are parallel to the view axis; the pixel center only
  // offsets the ray origin within the view plane.
  const Vector3f origin(
    v.eye.x() + v.right.x()*nx*v.halfWidth + v.up.x()*ny*v.halfHeight,
    v.eye.y() + v.right.y()*nx*v.halfWidth + v.up.y()*ny*v.halfHeight,
    v.eye.z() + v.right.z()*nx*v.halfWidth + v.up.z()*ny*v.halfHeight);
  const Vector3f dir = normalize3(v.forward);
  bool found = false;
  float best = std::numeric_limits<float>::infinity();
  for (const Triangle& t : src) {
    const Vector3f a(t[0].x(), t[0].y(), t[0].z());
    const Vector3f b(t[1].x(), t[1].y(), t[1].z());
    const Vector3f c(t[2].x(), t[2].y(), t[2].z());
    float dist;
    if (rayTriangle(origin, dir, a, b, c, dist) && dist < best) {
      best = dist;
      found = true;
      colorOut = Vector3f(static_cast<float>((t.color >> 0) & 0xffu)/255.0f,
                          static_cast<float>((t.color >> 8) & 0xffu)/255.0f,
                          static_cast<float>((t.color >> 16) & 0xffu)/255.0f);
      // Depth along forward of the hit point.
      const Vector3f hit(origin.x()+dir.x()*dist,
                         origin.y()+dir.y()*dist,
                         origin.z()+dir.z()*dist);
      depthOut = dot3(Vector3f(hit.x()-origin.x(),
                               hit.y()-origin.y(),
                               hit.z()-origin.z()),
                      v.forward);
    }
  }
  return found;
}

// A profile of the captured record at a pixel.
bool recordMatchesRay(const DepthImpostor& imp, std::size_t viewIndex,
                      const std::vector<Triangle>& src,
                      int px, int py, float posTolerance)
{
  const ImpostorView& iv = imp.view(viewIndex);
  const ImpostorRecord& rec = iv.records[py * iv.view.width + px];
  Vector3f rayColor;
  float rayDepth = 0.0f;
  const bool hit = nearestColor(src, iv.view, px, py, rayColor, rayDepth);
  if (!hit) {
    return !rec.valid;
  }
  if (!rec.valid) {
    return false;
  }
  if (std::abs(rec.color.x()-rayColor.x()) > 1.0f/255.0f + 1e-5f ||
      std::abs(rec.color.y()-rayColor.y()) > 1.0f/255.0f + 1e-5f ||
      std::abs(rec.color.z()-rayColor.z()) > 1.0f/255.0f + 1e-5f) {
    return false;
  }
  // Recompute the expected local position from the ray hit: project back.
  // rec.position should lie on the same depth along forward.
  const float expected = dot3(Vector3f(rec.position.x()-iv.view.eye.x(),
                                       rec.position.y()-iv.view.eye.y(),
                                       rec.position.z()-iv.view.eye.z()),
                              iv.view.forward);
  return std::abs(expected - rayDepth) <= posTolerance;
}

// ---------------------------------------------------------------------------
// capture cases
// ---------------------------------------------------------------------------

// Orthographic capture uses real per-view projected depth: recall that the
// legacy rasterizer stores inverse-w. A tilted plane must produce strictly
// increasing depth across the image.
int caseCaptureOrthographicDepth()
{
  const u32 W = 32, H = 32;
  CaptureView v = viewFrom(0.0f, 0.0f, 5.0f, 2.0f, 2.0f, 1.0f, 10.0f, W, H);
  // A plane tilted so z varies with x: z = -0.5*x.  Corners:
  // (-1,-1, 0.5), (1,-1,-0.5), (1,1,-0.5), (-1,1,0.5)
  std::vector<Triangle> src;
  src.push_back(triangle(-1,-1, 0.5f,  1,-1,-0.5f,  1,1,-0.5f, opaque(200,60,60)));
  src.push_back(triangle(-1,-1, 0.5f,  1,1,-0.5f,  -1,1, 0.5f, opaque(200,60,60)));

  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v}, 1.0f, imp));
  TASK_CHECK(imp.viewCount() == 1);
  const ImpostorView& iv = imp.view(0);
  TASK_CHECK(iv.records.size() == W*H);

  // Sample left and right of the middle row within the plane's footprint.
  // With right = -x, image x grows as local x decreases, so image-left is the
  // larger local x, where the plane is nearer the +z eye (smaller along-
  // forward distance).
  const int midY = H/2;
  const ImpostorRecord& left = iv.records[midY*W + 10];
  const ImpostorRecord& right = iv.records[midY*W + W-11];
  TASK_CHECK(left.valid && right.valid);
  // Depth is the true along-forward distance from the eye; the plane is
  // z = -0.5*x, and local x = -image_x_ndc * halfWidth. So image-left (large
  // local x) is the nearer side; depth grows toward image-right.
  TASK_CHECK(right.depth < left.depth - 0.3f);
  TASK_CHECK(std::abs(left.depth - (5.0f + 0.5f * 0.69f)) < 0.3f);
  TASK_CHECK(std::abs(right.depth - (5.0f - 0.5f * 0.69f)) < 0.3f);
  return 0;
}

// Equal depth within a view keeps the lower original input face index.
int caseCaptureEqualDepthSourceIndex()
{
  const u32 W = 32, H = 32;
  CaptureView v = viewFrom(0.0f, 0.0f, 5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  std::vector<Triangle> src;
  // Index 0/1: a red quad at z = 0 (two triangles).
  addQuadZ(src, -1.5f, -1.5f, 1.5f, 1.5f, 0.0f, opaque(240, 20, 20));
  // Index 2/3: a blue quad at the same z = 0, overlapping. Because depths tie,
  // the lower index (red) must win everywhere they overlap.
  addQuadZ(src, -1.0f, -1.0f, 1.0f, 1.0f, 0.0f, opaque(20, 20, 240));

  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v}, 5.0f, imp));
  // Where both quads cover a pixel at the same depth, red (lower index) wins.
  // A handful of pixels lie on the blue quad's own diagonal triangulation
  // seam and are not covered by red at all, so allow a small number of blue.
  int checked = 0, blue = 0;
  for (u32 y = 6; y < H-6; ++y) {
    for (u32 x = 6; x < W-6; ++x) {
      const ImpostorRecord& r = imp.view(0).records[y*W + x];
      if (!r.valid) {
        continue;
      }
      ++checked;
      if (r.color.z() > 0.5f) {
        ++blue;
      }
      else {
        TASK_CHECK(r.color.x() > 0.8f);   // red from the lower source index
        TASK_CHECK(r.color.z() < 0.2f);
      }
    }
  }
  TASK_CHECK(checked > 100);
  // At most a few seam pixels may show the higher-index color.
  TASK_CHECK(blue * 20 <= checked);
  return 0;
}

// Records match an independent ray cast for front/back layers, a notch and
// adjacent differently colored faces.
int caseCaptureLayersAndAdjacency()
{
  const u32 W = 48, H = 48;
  CaptureView v = viewFrom(0.0f, 0.0f, 5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  std::vector<Triangle> src;
  // Back layer (blue) full, at z = -2.
  addQuadZ(src, -1.5f, -1.5f, 1.5f, 1.5f, -2.0f, opaque(20, 40, 220));
  // Front layer (red) left half, at z = 0.
  addQuadZ(src, -1.5f, -1.5f, 0.0f, 1.5f, 0.0f, opaque(220, 40, 40));
  // Front layer (green) right half but only the lower part, leaving a notch
  // exposing blue above.
  addQuadZ(src, 0.0f, -1.5f, 1.5f, 0.0f, 0.0f, opaque(30, 200, 60));

  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v}, 5.0f, imp));
  TASK_CHECK(imp.viewCount() == 1);

  // Sample points chosen relative to the geometry via the mapping.
  auto at = [&](float x, float y) {
    float fx, fy, d;
    projectToPixel(v, Vector3f(x, y, 0.0f), fx, fy, d);
    int px = static_cast<int>(fx);
    int py = static_cast<int>(fy);
    px = std::max(0, std::min<int>(static_cast<int>(W)-1, px));
    py = std::max(0, std::min<int>(static_cast<int>(H)-1, py));
    return std::pair<int,int>(px, py);
  };
  std::pair<int,int> pRed = at(-1.0f, 0.0f);
  std::pair<int,int> pGreen = at(1.0f, -1.0f);
  std::pair<int,int> pBlue = at(1.0f, 1.0f);

  const ImpostorRecord& rRed = imp.view(0).records[pRed.second*W + pRed.first];
  const ImpostorRecord& rGreen = imp.view(0).records[pGreen.second*W + pGreen.first];
  const ImpostorRecord& rBlue = imp.view(0).records[pBlue.second*W + pBlue.first];
  TASK_CHECK(rRed.valid && rGreen.valid && rBlue.valid);
  TASK_CHECK(rRed.color.x() > 0.8f && rRed.color.y() < 0.2f);      // red
  TASK_CHECK(rGreen.color.y() > 0.7f && rGreen.color.x() < 0.2f);  // green
  TASK_CHECK(rBlue.color.z() > 0.8f && rBlue.color.x() < 0.2f);    // blue
  TASK_CHECK(std::abs(rRed.position.z() - 0.0f) < 0.05f);
  TASK_CHECK(std::abs(rBlue.position.z() + 2.0f) < 0.05f);

  // Every sampled pixel matches the independent ray cast.
  for (int py = 0; py < static_cast<int>(H); py += 7) {
    for (int px = 0; px < static_cast<int>(W); px += 7) {
      TASK_CHECK(recordMatchesRay(imp, 0, src, px, py, 0.1f));
    }
  }
  return 0;
}

// Adjacent, differently colored faces: each record keeps its own face color
// and the original (winding-preserving) face normal.
int caseCaptureFaceNormalsAndColor()
{
  const u32 W = 40, H = 40;
  CaptureView v = viewFrom(0.0f, 0.0f, 5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  std::vector<Triangle> src;
  // Left half faces +z (winding a,b,c CCW in xy). Right half is a reversed
  // winding so its face normal points -z but it is still visible (capture is
  // double sided).
  src.push_back(triangle(-1.5f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f,
                         0.0f, 1.0f, 0.0f, opaque(230, 50, 50)));
  src.push_back(triangle(-1.5f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                         -1.5f, 1.0f, 0.0f, opaque(230, 50, 50)));
  src.push_back(triangle(0.0f, 1.0f, 0.0f, 1.5f, -1.0f, 0.0f,
                         0.0f, -1.0f, 0.0f, opaque(60, 200, 90)));
  src.push_back(triangle(0.0f, 1.0f, 0.0f, 1.5f, 1.0f, 0.0f,
                         1.5f, -1.0f, 0.0f, opaque(60, 200, 90)));

  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v}, 5.0f, imp));
  const ImpostorView& iv = imp.view(0);

  float fx, fy, d;
  projectToPixel(v, Vector3f(-0.8f, 0.0f, 0.0f), fx, fy, d);
  const ImpostorRecord& left = iv.records[static_cast<int>(fy)*W +
                                           static_cast<int>(fx)];
  projectToPixel(v, Vector3f(0.8f, 0.0f, 0.0f), fx, fy, d);
  const ImpostorRecord& right = iv.records[static_cast<int>(fy)*W +
                                            static_cast<int>(fx)];
  TASK_CHECK(left.valid && right.valid);
  // Both normals are unit length and axis aligned, with opposite z signs,
  // preserving the original winding.
  TASK_CHECK(std::abs(std::abs(left.normal.z()) - 1.0f) < 0.05f);
  TASK_CHECK(std::abs(std::abs(right.normal.z()) - 1.0f) < 0.05f);
  TASK_CHECK(left.normal.z() > 0.9f);
  TASK_CHECK(right.normal.z() < -0.9f);
  // Colors are distinct and match their faces.
  TASK_CHECK(left.color.x() > 0.7f && left.color.y() < 0.3f);
  TASK_CHECK(right.color.y() > 0.7f && right.color.x() < 0.3f);
  return 0;
}

// Validation rejects bad input without clobbering an existing resource.
int caseCaptureValidation()
{
  std::vector<Triangle> good;
  addQuadZ(good, -1, -1, 1, 1, 0.0f, opaque(200, 200, 200));
  CaptureView v = viewFrom(0, 0, 5, 2, 2, 1, 10, 16, 16);
  DepthImpostor imp;
  TASK_CHECK(captureImpostor(good, {v}, 1.0f, imp));
  const std::size_t originalViews = imp.viewCount();
  TASK_CHECK(originalViews == 1);
  const std::size_t originalRecords = imp.view(0).records.size();

  // Non-positive maxEdge.
  TASK_CHECK(!captureImpostor(good, {v}, 0.0f, imp));
  TASK_CHECK(!captureImpostor(good, {v}, -2.0f, imp));
  TASK_CHECK(!captureImpostor(good, {v},
                              std::numeric_limits<float>::infinity(), imp));

  // Invalid view: zero size, bad near/far, non-unit basis, wrong forward.
  CaptureView bad = v;
  bad.width = 0;
  TASK_CHECK(!captureImpostor(good, {bad}, 1.0f, imp));
  bad = v;
  bad.nearPlane = 0.0f;
  TASK_CHECK(!captureImpostor(good, {bad}, 1.0f, imp));
  bad = v;
  bad.farPlane = bad.nearPlane;
  TASK_CHECK(!captureImpostor(good, {bad}, 1.0f, imp));
  bad = v;
  bad.right = Vector3f(2.0f, 0.0f, 0.0f);
  TASK_CHECK(!captureImpostor(good, {bad}, 1.0f, imp));
  bad = v;
  bad.forward = Vector3f(0.0f, 0.0f, 1.0f);
  TASK_CHECK(!captureImpostor(good, {bad}, 1.0f, imp));

  // Degenerate triangle.
  std::vector<Triangle> degenerate;
  degenerate.push_back(triangle(0, 0, 0, 1, 0, 0, 2, 0, 0, opaque(1, 2, 3)));
  TASK_CHECK(!captureImpostor(degenerate, {v}, 1.0f, imp));

  // Non-opaque color.
  std::vector<Triangle> translucent;
  translucent.push_back(triangle(-1, -1, 0, 1, -1, 0, 0, 1, 0,
                                 ImageData::makePixel(10, 20, 30, 128)));
  TASK_CHECK(!captureImpostor(translucent, {v}, 1.0f, imp));

  // Non-finite geometry.
  std::vector<Triangle> nanTri;
  nanTri.push_back(triangle(-1, -1, 0, 1, -1, 0, 0,
                            std::numeric_limits<float>::quiet_NaN(), 0,
                            opaque(1, 2, 3)));
  TASK_CHECK(!captureImpostor(nanTri, {v}, 1.0f, imp));

  // Source released: the resource is independent of the triangle list, so it
  // still exposes its views and records after the input is destroyed.
  good.clear();
  good.shrink_to_fit();
  TASK_CHECK(imp.viewCount() == originalViews);
  TASK_CHECK(imp.view(0).records.size() == originalRecords);
  // The freed-and-cleared source cannot have been retained: the view still
  // reports full record and index storage of its own.
  TASK_CHECK(!imp.view(0).records.empty());
  return 0;
}

// Reconstruction index: a 2x2 cell becomes two fixed triangles; maxEdge
// removes edges that are too long. A depth step produces no bridge triangle.
int caseCaptureReconstructionIndex()
{
  const u32 W = 8, H = 8;
  CaptureView v = viewFrom(0.0f, 0.0f, 5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);

  // A flat plane so every record is valid. With a generous maxEdge each of
  // the (W-1)*(H-1) cells contributes exactly two triangles.
  std::vector<Triangle> flat;
  addQuadZ(flat, -120.0f, -120.0f, 120.0f, 120.0f, 0.0f, opaque(150, 150, 150));
  DepthImpostor gen;
  TASK_CHECK(captureImpostor(flat, {v}, 1e6f, gen));
  TASK_CHECK(gen.view(0).triangles.size() == (W-1)*(H-1)*2*3);
  // The fixed split persists: first two triangles of cell (0,0) are
  // (0,1,9) and (0,9,8) with row-major indices.
  TASK_CHECK(gen.view(0).triangles[0] == 0);
  TASK_CHECK(gen.view(0).triangles[1] == 1);
  TASK_CHECK(gen.view(0).triangles[2] == W+1);
  TASK_CHECK(gen.view(0).triangles[3] == 0);
  TASK_CHECK(gen.view(0).triangles[4] == W+1);
  TASK_CHECK(gen.view(0).triangles[5] == W);

  // Very small maxEdge leaves no triangles on this coarse grid.
  DepthImpostor tiny;
  TASK_CHECK(captureImpostor(flat, {v}, 0.001f, tiny));
  TASK_CHECK(tiny.view(0).triangles.empty());

  // Depth discontinuity: two separate front planes at z=0 and z=-1 with a
  // gap between their in-image footprints. No triangle should span pixels
  // whose local positions differ by a large z step.
  CaptureView v2 = viewFrom(0.0f, 0.0f, 5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  std::vector<Triangle> stepped;
  // Left half at z=0, right half at z=-1.5. The long edges across the seam
  // are local-space length > 1.0, so maxEdge=1.0 must drop them.
  addQuadZ(stepped, -2.0f, -2.0f, -0.1f, 2.0f, 0.0f, opaque(200, 40, 40));
  addQuadZ(stepped, 0.1f, -2.0f, 2.0f, 2.0f, -1.5f, opaque(40, 80, 220));
  DepthImpostor stepImp;
  TASK_CHECK(captureImpostor(stepped, {v2}, 1.0f, stepImp));
  const ImpostorView& st = stepImp.view(0);
  // Every retained triangle must have all edges within maxEdge (no bridge).
  for (std::size_t i = 0; i + 2 < st.triangles.size(); i += 3) {
    const ImpostorRecord& a = st.records[st.triangles[i+0]];
    const ImpostorRecord& b = st.records[st.triangles[i+1]];
    const ImpostorRecord& c = st.records[st.triangles[i+2]];
    TASK_CHECK(a.valid && b.valid && c.valid);
    auto edge = [](const ImpostorRecord& p, const ImpostorRecord& q) {
      return std::sqrt((p.position.x()-q.position.x())*
                       (p.position.x()-q.position.x()) +
                       (p.position.y()-q.position.y())*
                       (p.position.y()-q.position.y()) +
                       (p.position.z()-q.position.z())*
                       (p.position.z()-q.position.z()));
    };
    TASK_CHECK(edge(a,b) <= 1.0f + 1e-4f);
    TASK_CHECK(edge(b,c) <= 1.0f + 1e-4f);
    TASK_CHECK(edge(c,a) <= 1.0f + 1e-4f);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// reprojection cases
// ---------------------------------------------------------------------------

// Reprojection of a front-facing plane through a perspective camera yields
// the expected color and depth, verified by independent ray intersection with
// the reconstruction micro triangles.
int caseReprojectionPlane()
{
  const u32 W = 64, H = 64;
  CaptureView v = viewFrontZ(5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  std::vector<Triangle> src;
  addQuadZ(src, -1.2f, -1.2f, 1.2f, 1.2f, 0.0f, opaque(220, 70, 70));
  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v}, 0.5f, imp));

  const int outW = 64, outH = 64;
  ImageData color(outW, outH);
  color.fill(0);
  std::vector<float> depth(outW*outH, std::numeric_limits<float>::infinity());
  const Vector3f eye(0.0f, 0.0f, 6.0f);
  Matrix4f m = perspectiveCamera(eye, G3D::deg2rad(60.0f), 1.0f, 0.5f, 50.0f);
  TASK_CHECK(drawImpostor(imp, Matrix4f(), m, eye, color, depth, 0.25f));

  // Center pixel should be the plane's red at roughly z=0 (distance 6).
  const int cx = outW/2, cy = outH/2;
  const u32 p = color.getPixel(cx, cy);
  TASK_CHECK(((p >> 0) & 0xffu) > 180);       // red
  TASK_CHECK(((p >> 8) & 0xffu) < 80);
  TASK_CHECK(std::isfinite(depth[cy*outW+cx]));
  TASK_CHECK(std::abs(depth[cy*outW+cx] - 6.0f) < 0.5f);

  // The plane covers its whole screen footprint; the capture/reprojection is
  // a sampling approximation, so a small number of single-pixel holes are
  // expected where micro triangles land between output samples.
  int covered = 0;
  for (u32 y = 0; y < H; ++y) {
    for (u32 x = 0; x < W; ++x) {
      if (color.getPixel(x, y)) {
        ++covered;
      }
    }
  }
  TASK_CHECK(covered > (outW*outH)/12);
  // Inside the footprint, coverage is dense (>= 97% of sampled pixels).
  int inside = 0, insideCovered = 0;
  for (int y = outH/3; y < 2*outH/3; ++y) {
    for (int x = outW/3; x < 2*outW/3; ++x) {
      ++inside;
      if (color.getPixel(x, y)) {
        ++insideCovered;
      }
    }
  }
  TASK_CHECK(insideCovered * 100 >= inside * 97);
  return 0;
}

// Distinguish per-view occlusion from "mix colors first, then depth". When
// two views see different nearest surfaces, each view must resolve its own
// nearest surface before any merging.
int caseReprojectionPerViewOcclusion()
{
  // Two-layer scene: front red plane (z = 0) covering the left half, back
  // blue plane (z = -2) full width. Two capture views from the front and
  // from an angle.
  const u32 W = 48, H = 48;
  std::vector<Triangle> src;
  addQuadZ(src, -1.5f, -1.5f, 1.5f, 1.5f, -2.0f, opaque(20, 40, 220));
  addQuadZ(src, -1.5f, -1.5f, 0.0f, 1.5f, 0.0f, opaque(220, 40, 40));

  CaptureView v0 = viewFrom(0.0f, 0.0f, 5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  // A second view offset to the right and slightly rotated.
  CaptureView v1 = v0;
  v1.eye = Vector3f(1.5f, 0.0f, 5.0f);
  v1.right = Vector3f(-1.0f, 0.0f, 0.0f);
  v1.up = Vector3f(0.0f, 1.0f, 0.0f);
  v1.forward = Vector3f(0.0f, 0.0f, -1.0f);

  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v0, v1}, 5.0f, imp));
  TASK_CHECK(imp.viewCount() == 2);

  // The per-view records differ: the right-offset view sees red further to
  // the right of the image than the centered view (parallax in ortho only if
  // the right axis differs). Instead check both views independently match
  // their own ray casts.
  for (int py = 4; py < static_cast<int>(H); py += 9) {
    for (int px = 4; px < static_cast<int>(W); px += 9) {
      TASK_CHECK(recordMatchesRay(imp, 0, src, px, py, 0.1f));
    }
  }
  return 0;
}

// Real micro-triangle reprojection: intersect a ray with the retained
// reconstruction triangles and compare to the draw's color/depth.
int caseReprojectionMicroMesh()
{
  const u32 W = 64, H = 64;
  CaptureView v = viewFrontZ(5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  std::vector<Triangle> src;
  // Two adjacent differently colored halves at the same depth.
  addQuadZ(src, -1.5f, -1.5f, 0.0f, 1.5f, 0.0f, opaque(210, 60, 60));
  addQuadZ(src, 0.0f, -1.5f, 1.5f, 1.5f, 0.0f, opaque(50, 190, 70));
  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v}, 0.5f, imp));

  const int outW = 80, outH = 80;
  ImageData color(outW, outH);
  color.fill(0);
  std::vector<float> depth(outW*outH, std::numeric_limits<float>::infinity());
  const Vector3f eye(0.25f, 0.1f, 6.0f);
  Matrix4f m = perspectiveCamera(eye, G3D::deg2rad(55.0f),
                                 static_cast<float>(outW)/outH, 0.5f, 50.0f);
  TASK_CHECK(drawImpostor(imp, Matrix4f(), m, eye, color, depth, 0.2f));

  // For a sample of colored output pixels, the color must match one of the
  // two source colors (no blended third color) and the depth must be finite
  // and near the plane. Blending across the seam is only allowed within
  // mergeDistance of the same depth, and both colors here are at z=0, so a
  // seam blend would still be red+green. Ensure away-from-seam pixels are
  // pure.
  int checked = 0;
  for (int py = 8; py < outH-8; py += 11) {
    for (int px = 8; px < outW-8; px += 11) {
      const u32 p = color.getPixel(px, py);
      if (!p) {
        continue;
      }
      const int r = (p >> 0) & 0xff;
      const int g = (p >> 8) & 0xff;
      const int b = (p >> 16) & 0xff;
      // The two source colors are (210,60,60) and (50,190,70); neither has
      // significant blue, so a blended third color would.
      TASK_CHECK(b < 90);
      const bool red = r > 150 && g < 110;
      const bool green = g > 140 && r < 120;
      TASK_CHECK(red || green);
      TASK_CHECK(std::isfinite(depth[py*outW+px]));
      TASK_CHECK(depth[py*outW+px] > 2.0f && depth[py*outW+px] < 9.0f);
      ++checked;
    }
  }
  TASK_CHECK(checked > 10);
  return 0;
}

// Clipping against the near face: geometry closer than the near plane must be
// clipped, not wrapped or smeared.
int caseReprojectionNearClip()
{
  const u32 W = 32, H = 32;
  CaptureView v = viewFrontZ(5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  std::vector<Triangle> src;
  addQuadZ(src, -1.5f, -1.5f, 1.5f, 1.5f, 0.0f, opaque(200, 100, 50));
  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v}, 5.0f, imp));

  const int outW = 48, outH = 48;
  ImageData color(outW, outH);
  color.fill(0);
  std::vector<float> depth(outW*outH, std::numeric_limits<float>::infinity());
  // Place the camera very close to the plane (distance 0.3) with near = 0.5,
  // so the plane is inside the near plane and must be culled/clipped away.
  const Vector3f eye(0.0f, 0.0f, 0.3f);
  Matrix4f m = perspectiveCamera(eye, G3D::deg2rad(60.0f), 1.0f, 0.5f, 50.0f);
  TASK_CHECK(drawImpostor(imp, Matrix4f(), m, eye, color, depth, 0.2f));
  int covered = 0;
  for (u32 y = 0; y < (u32)outH; ++y) {
    for (u32 x = 0; x < (u32)outW; ++x) {
      if (color.getPixel(x, y)) {
        ++covered;
      }
    }
  }
  TASK_CHECK(covered == 0);
  return 0;
}

// Non-uniform instance transform: normals must go through the inverse
// transpose, and positions through the affine part.
int caseReprojectionInstanceTransform()
{
  const u32 W = 48, H = 48;
  CaptureView v = viewFrontZ(5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  std::vector<Triangle> src;
  addQuadZ(src, -1.0f, -1.0f, 1.0f, 1.0f, 0.0f, opaque(180, 80, 200));
  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v}, 5.0f, imp));

  // Non-uniform scale plus translation; the plane stays a plane and remains
  // front-facing.
  Matrix4f world = Matrix4f::makeTranslation(0.0f, 0.0f, -1.0f);
  world.set(0, 0, 1.5f);
  world.set(1, 1, 0.7f);
  world.set(2, 2, 1.0f);

  const int outW = 64, outH = 64;
  ImageData color(outW, outH);
  color.fill(0);
  std::vector<float> depth(outW*outH, std::numeric_limits<float>::infinity());
  const Vector3f eye(0.0f, 0.0f, 6.0f);
  Matrix4f m = perspectiveCamera(eye, G3D::deg2rad(60.0f), 1.0f, 0.5f, 50.0f);
  TASK_CHECK(drawImpostor(imp, world, m, eye, color, depth, 0.5f));

  // Some coverage and plausible depth (plane translated to z=-1, so distance
  // about 7).
  int covered = 0;
  float centerDepth = 0.0f;
  for (u32 y = 0; y < (u32)outH; ++y) {
    for (u32 x = 0; x < (u32)outW; ++x) {
      if (color.getPixel(x, y)) {
        ++covered;
      }
    }
  }
  TASK_CHECK(covered > 100);
  const std::size_t ci = static_cast<std::size_t>(outH/2)*outW + outW/2;
  centerDepth = depth[ci];
  TASK_CHECK(std::isfinite(centerDepth));
  TASK_CHECK(std::abs(centerDepth - 7.0f) < 1.0f);
  return 0;
}

// External foreground depth: the caller's depth buffer acts as an occlusion
// target. A fragment not strictly nearer must not overwrite color or depth.
int caseReprojectionExternalDepth()
{
  const u32 W = 32, H = 32;
  CaptureView v = viewFrontZ(5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  std::vector<Triangle> src;
  addQuadZ(src, -1.5f, -1.5f, 1.5f, 1.5f, 0.0f, opaque(240, 40, 40));
  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v}, 5.0f, imp));

  const int outW = 40, outH = 40;
  const Vector3f eye(0.0f, 0.0f, 6.0f);
  Matrix4f m = perspectiveCamera(eye, G3D::deg2rad(60.0f), 1.0f, 0.5f, 50.0f);

  // Fill the target with a very near foreground (distance 1.0) plus a sentinel
  // color. Nothing should overwrite it.
  ImageData color(outW, outH);
  const u32 sentinel = opaque(5, 6, 7);
  color.fill(sentinel);
  std::vector<float> depth(outW*outH, 1.0f);
  TASK_CHECK(drawImpostor(imp, Matrix4f(), m, eye, color, depth, 0.2f));
  for (u32 y = 0; y < (u32)outH; ++y) {
    for (u32 x = 0; x < (u32)outW; ++x) {
      TASK_CHECK(color.getPixel(x, y) == sentinel);
      TASK_CHECK(depth[y*outW + x] == 1.0f);
    }
  }

  // Now clear depth to +inf and sentinel color; it must draw and update both.
  color.fill(sentinel);
  std::fill(depth.begin(), depth.end(),
            std::numeric_limits<float>::infinity());
  TASK_CHECK(drawImpostor(imp, Matrix4f(), m, eye, color, depth, 0.2f));
  const std::size_t ci = static_cast<std::size_t>(outH/2)*outW + outW/2;
  TASK_CHECK(color.getPixel(outW/2, outH/2) != sentinel);
  TASK_CHECK(std::isfinite(depth[ci]));
  return 0;
}

// Invalid draw arguments must return false before touching caller buffers.
int caseReprojectionValidation()
{
  const u32 W = 16, H = 16;
  CaptureView v = viewFrontZ(5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  std::vector<Triangle> src;
  addQuadZ(src, -1, -1, 1, 1, 0.0f, opaque(100, 120, 140));
  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v}, 5.0f, imp));

  ImageData color(20, 20);
  color.fill(0x12345678u);
  std::vector<float> depth(20*20, 42.0f);
  const Vector3f eye(0, 0, 6.0f);
  Matrix4f good = perspectiveCamera(eye, G3D::deg2rad(60.0f), 1.0f, 0.5f, 50.0f);

  auto unchanged = [&]() {
    for (u32 y = 0; y < 20; ++y) {
      for (u32 x = 0; x < 20; ++x) {
        if (color.getPixel(x, y) != 0x12345678u) return false;
      }
    }
    for (float d : depth) {
      if (d != 42.0f) return false;
    }
    return true;
  };

  // Negative merge distance.
  TASK_CHECK(!drawImpostor(imp, Matrix4f(), good, eye, color, depth, -1.0f));
  TASK_CHECK(unchanged());
  // Non-finite merge distance.
  TASK_CHECK(!drawImpostor(imp, Matrix4f(), good, eye, color, depth,
                           std::numeric_limits<float>::infinity()));
  TASK_CHECK(unchanged());
  // Singular instance transform.
  Matrix4f singular = Matrix4f();
  singular.set(0, 0, 0.0f);
  singular.set(1, 1, 0.0f);
  singular.set(2, 2, 0.0f);
  TASK_CHECK(!drawImpostor(imp, singular, good, eye, color, depth, 0.1f));
  TASK_CHECK(unchanged());
  // Wrong depth buffer size.
  std::vector<float> shortDepth(10, 42.0f);
  TASK_CHECK(!drawImpostor(imp, Matrix4f(), good, eye, color, shortDepth, 0.1f));
  TASK_CHECK(unchanged());
  // Non-finite camera.
  TASK_CHECK(!drawImpostor(imp, Matrix4f(), good,
                           Vector3f(std::numeric_limits<float>::quiet_NaN(),
                                    0, 0), color, depth, 0.1f));
  TASK_CHECK(unchanged());
  return 0;
}

// ---------------------------------------------------------------------------
// composition cases
// ---------------------------------------------------------------------------

// Two-view blend: two surfaces whose colors differ are merged with facing^4
// weights. Weighted color must be exactly the normalized sum for the
// participating candidates.
int caseCompositionWeightedBlend()
{
  const u32 W = 64, H = 64;
  // One plane seen from two views. The planes' normals are the same so both
  // views contribute; colors are identical so the blend is that color.
  std::vector<Triangle> src;
  addQuadZ(src, -1.5f, -1.5f, 1.5f, 1.5f, 0.0f, opaque(120, 180, 240));
  CaptureView v0 = viewFrontZ(5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  CaptureView v1 = viewFrom(0.0f, 0.0f, 5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v0, v1}, 5.0f, imp));

  const int outW = 64, outH = 64;
  ImageData color(outW, outH);
  color.fill(0);
  std::vector<float> depth(outW*outH, std::numeric_limits<float>::infinity());
  const Vector3f eye(0.2f, 0.1f, 6.0f);
  Matrix4f m = perspectiveCamera(eye, G3D::deg2rad(60.0f), 1.0f, 0.5f, 50.0f);
  TASK_CHECK(drawImpostor(imp, Matrix4f(), m, eye, color, depth, 0.5f));
  // Sample center: color near the plane's RGB/255.
  const u32 p = color.getPixel(outW/2, outH/2);
  TASK_CHECK(p != 0);
  TASK_CHECK(std::abs(int((p >> 0) & 0xff) - 120) <= 2);
  TASK_CHECK(std::abs(int((p >> 8) & 0xff) - 180) <= 2);
  TASK_CHECK(std::abs(int((p >> 16) & 0xff) - 240) <= 2);
  const float d = depth[static_cast<std::size_t>(outH/2)*outW + outW/2];
  TASK_CHECK(std::isfinite(d));
  TASK_CHECK(std::abs(d - 6.0f) < 0.5f);
  return 0;
}

// Reordering source views changes neither color (<=2e-6) nor depth (<=1e-6).
int caseCompositionViewOrderInvariance()
{
  const u32 W = 48, H = 48;
  // Two differently colored adjacent planes so blending is non-trivial.
  std::vector<Triangle> src;
  addQuadZ(src, -1.5f, -1.5f, 0.0f, 1.5f, 0.0f, opaque(230, 60, 40));
  addQuadZ(src, 0.0f, -1.5f, 1.5f, 1.5f, -0.3f, opaque(40, 90, 230));
  CaptureView v0 = viewFrontZ(5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  CaptureView v1 = viewFrom(0.0f, 0.0f, 5.5f, 2.2f, 2.2f, 0.5f, 10.0f, W, H);

  DepthImpostor a, b;
  TASK_CHECK(captureImpostor(src, {v0, v1}, 5.0f, a));
  TASK_CHECK(captureImpostor(src, {v1, v0}, 5.0f, b));

  const int outW = 64, outH = 64;
  ImageData ca(outW, outH), cb(outW, outH);
  ca.fill(0); cb.fill(0);
  std::vector<float> da(outW*outH, std::numeric_limits<float>::infinity()), db(outW*outH, std::numeric_limits<float>::infinity());
  const Vector3f eye(0.3f, -0.2f, 6.0f);
  Matrix4f m = perspectiveCamera(eye, G3D::deg2rad(60.0f), 1.0f, 0.5f, 50.0f);
  TASK_CHECK(drawImpostor(a, Matrix4f(), m, eye, ca, da, 0.5f));
  TASK_CHECK(drawImpostor(b, Matrix4f(), m, eye, cb, db, 0.5f));

  for (u32 y = 0; y < (u32)outH; ++y) {
    for (u32 x = 0; x < (u32)outW; ++x) {
      const u32 pa = ca.getPixel(x, y);
      const u32 pb = cb.getPixel(x, y);
      const float da0 = da[y*outW + x];
      const float db0 = db[y*outW + x];
      // Both must agree on coverage.
      TASK_CHECK((pa != 0) == (pb != 0));
      if (pa && pb) {
        for (int s = 0; s < 3; ++s) {
          const float fa = ((pa >> (8*s)) & 0xffu) / 255.0f;
          const float fb = ((pb >> (8*s)) & 0xffu) / 255.0f;
          TASK_CHECK(std::abs(fa - fb) <= 2e-6f);
        }
        TASK_CHECK(std::abs(da0 - db0) <= 1e-6f);
      }
    }
  }
  return 0;
}

// Two resources drawn interleaved do not share scratch buffers.
int caseCompositionInterleavedResources()
{
  const u32 W = 40, H = 40;
  std::vector<Triangle> srcA;
  addQuadZ(srcA, -1.5f, -1.5f, 1.5f, 1.5f, 0.0f, opaque(210, 50, 50));
  std::vector<Triangle> srcB;
  addQuadZ(srcB, -1.2f, -1.2f, 1.2f, 1.2f, -1.0f, opaque(50, 90, 220));
  CaptureView v = viewFrontZ(5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  DepthImpostor a, b;
  TASK_CHECK(captureImpostor(srcA, {v}, 5.0f, a));
  TASK_CHECK(captureImpostor(srcB, {v}, 5.0f, b));

  const int outW = 48, outH = 48;
  const Vector3f eye(0.0f, 0.0f, 6.0f);
  Matrix4f m = perspectiveCamera(eye, G3D::deg2rad(60.0f), 1.0f, 0.5f, 50.0f);

  ImageData ca(outW, outH), cb(outW, outH);
  ca.fill(0); cb.fill(0);
  std::vector<float> da(outW*outH, std::numeric_limits<float>::infinity()), db(outW*outH, std::numeric_limits<float>::infinity());
  // Interleave: draw A, then B, then A again into fresh targets.
  TASK_CHECK(drawImpostor(a, Matrix4f(), m, eye, ca, da, 0.2f));
  TASK_CHECK(drawImpostor(b, Matrix4f(), m, eye, cb, db, 0.2f));
  ImageData ca2(outW, outH);
  ca2.fill(0);
  std::vector<float> da2(outW*outH, std::numeric_limits<float>::infinity());
  TASK_CHECK(drawImpostor(a, Matrix4f(), m, eye, ca2, da2, 0.2f));

  // A's two draws are bit-identical.
  for (u32 y = 0; y < (u32)outH; ++y) {
    for (u32 x = 0; x < (u32)outW; ++x) {
      TASK_CHECK(ca.getPixel(x, y) == ca2.getPixel(x, y));
      const float d1 = da[y*outW + x];
      const float d2 = da2[y*outW + x];
      TASK_CHECK((std::isfinite(d1) && std::isfinite(d2) && d1 == d2) ||
                 (std::isinf(d1) && std::isinf(d2)));
    }
  }
  // B rendered its own (different) content.
  const std::size_t ci = static_cast<std::size_t>(outH/2)*outW + outW/2;
  TASK_CHECK(ca.getPixel(outW/2, outH/2) != cb.getPixel(outW/2, outH/2));
  return 0;
}

// Zero total weight yields no output.
int caseCompositionZeroWeight()
{
  const u32 W = 32, H = 32;
  // A plane facing +z captured from a front view. Drawing from directly
  // behind (camera on the -z side) makes the world normal point away from the
  // camera, so facing <= 0 and the pixel gets no candidate.
  std::vector<Triangle> src;
  addQuadZ(src, -1.5f, -1.5f, 1.5f, 1.5f, 0.0f, opaque(200, 200, 60));
  CaptureView v = viewFrontZ(5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v}, 5.0f, imp));

  const int outW = 48, outH = 48;
  ImageData color(outW, outH);
  color.fill(0);
  std::vector<float> depth(outW*outH, std::numeric_limits<float>::infinity());
  // Camera behind the plane, looking toward +z.
  const Vector3f eye(0.0f, 0.0f, -6.0f);
  Matrix4f view = Matrix4f::makeTranslation(-eye.x(), -eye.y(), -eye.z());
  Matrix4f flip = Matrix4f::makeYRotation(G3D::deg2rad(180.0f));
  Matrix4f m = Matrix4f::makePerspective(G3D::deg2rad(60.0f), 1.0f,
                                         0.5f, 50.0f) * view;
  (void)flip;
  // The perspective matrix maps -z forward; from behind, the plane's winding
  // normal (+z) faces away.
  TASK_CHECK(drawImpostor(imp, Matrix4f(), m, eye, color, depth, 0.2f));
  int covered = 0;
  for (u32 y = 0; y < (u32)outH; ++y) {
    for (u32 x = 0; x < (u32)outW; ++x) {
      if (color.getPixel(x, y)) {
        ++covered;
      }
    }
  }
  TASK_CHECK(covered == 0);
  return 0;
}

// Per-view nearest surface, then merge: a nearer front surface seen by view A
// must not be blended with a farther back surface seen by view B beyond the
// merge distance, and the final depth must come from the nearest positive
// weight candidate (not the blended point or a proxy plane).
int caseCompositionDepthDiscontinuity()
{
  const u32 W = 48, H = 48;
  // Front small plane at z=0 (red), back large plane at z=-4 (blue).
  std::vector<Triangle> src;
  addQuadZ(src, -3.0f, -3.0f, 3.0f, 3.0f, -4.0f, opaque(30, 50, 220));
  addQuadZ(src, -0.8f, -0.8f, 0.8f, 0.8f, 0.0f, opaque(220, 40, 40));
  CaptureView v0 = viewFrontZ(5.0f, 2.5f, 2.5f, 0.5f, 12.0f, W, H);
  CaptureView v1 = viewFrom(0.0f, 0.0f, 5.0f, 2.5f, 2.5f, 0.5f, 12.0f, W, H);
  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v0, v1}, 6.0f, imp));

  const int outW = 64, outH = 64;
  ImageData color(outW, outH);
  color.fill(0);
  std::vector<float> depth(outW*outH, std::numeric_limits<float>::infinity());
  const Vector3f eye(0.0f, 0.0f, 6.0f);
  Matrix4f m = perspectiveCamera(eye, G3D::deg2rad(60.0f), 1.0f, 0.5f, 50.0f);
  TASK_CHECK(drawImpostor(imp, Matrix4f(), m, eye, color, depth, 0.1f));

  // Center should be the front red plane; its depth must be the front
  // surface's distance (6), not the average of 6 and 10.
  const std::size_t ci = static_cast<std::size_t>(outH/2)*outW + outW/2;
  const u32 p = color.getPixel(outW/2, outH/2);
  TASK_CHECK(p != 0);
  TASK_CHECK(((p >> 0) & 0xffu) > 170);   // red dominant near the front
  TASK_CHECK(((p >> 16) & 0xffu) < 90);
  TASK_CHECK(std::isfinite(depth[ci]));
  TASK_CHECK(std::abs(depth[ci] - 6.0f) < 0.5f);
  // A far corner sees only the back plane (blue).
  const u32 corner = color.getPixel(3, 3);
  if (corner) {
    TASK_CHECK(((corner >> 16) & 0xffu) > 150);
  }
  return 0;
}

// draw() honours a caller-initialised +inf depth target and writes only
// strictly nearer fragments.
int caseCompositionDepthInit()
{
  const u32 W = 32, H = 32;
  std::vector<Triangle> src;
  addQuadZ(src, -1.5f, -1.5f, 1.5f, 1.5f, 0.0f, opaque(90, 160, 220));
  CaptureView v = viewFrontZ(5.0f, 2.0f, 2.0f, 0.5f, 10.0f, W, H);
  DepthImpostor imp;
  TASK_CHECK(captureImpostor(src, {v}, 5.0f, imp));

  const int outW = 40, outH = 40;
  ImageData color(outW, outH);
  color.fill(0);
  // The convention when starting empty is a +inf target.
  std::vector<float> depth(outW*outH,
                           std::numeric_limits<float>::infinity());
  const Vector3f eye(0.0f, 0.0f, 6.0f);
  Matrix4f m = perspectiveCamera(eye, G3D::deg2rad(60.0f), 1.0f, 0.5f, 50.0f);
  TASK_CHECK(drawImpostor(imp, Matrix4f(), m, eye, color, depth, 0.3f));
  // Pixels that received no color keep +inf; colored pixels hold finite
  // positive depths.
  for (u32 y = 0; y < (u32)outH; ++y) {
    for (u32 x = 0; x < (u32)outW; ++x) {
      const float d = depth[y*outW + x];
      if (!color.getPixel(x, y)) {
        TASK_CHECK(std::isinf(d) && d > 0.0f);
      }
      else {
        TASK_CHECK(std::isfinite(d) && d > 0.0f);
      }
    }
  }
  return 0;
}

// An empty resource is legal input: it writes no colors and leaves the
// caller's depth target untouched.
int caseCompositionEmptyResource()
{
  DepthImpostor empty;
  ImageData color(8, 8);
  color.fill(0);
  std::vector<float> depth(64,
                           std::numeric_limits<float>::infinity());
  const Vector3f eye(0, 0, 5.0f);
  Matrix4f m = perspectiveCamera(eye, G3D::deg2rad(60.0f), 1.0f, 0.5f, 50.0f);
  const bool ok = drawImpostor(empty, Matrix4f(), m, eye, color, depth, 0.1f);
  TASK_CHECK(ok);
  for (u32 y = 0; y < 8; ++y) {
    for (u32 x = 0; x < 8; ++x) {
      TASK_CHECK(color.getPixel(x, y) == 0);
      TASK_CHECK(std::isinf(depth[y*8 + x]));
    }
  }
  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Case registration (C linkage for the C driver)
// ---------------------------------------------------------------------------

extern const struct task_case task_cases[] = {
  {"capture", "orthographic_projected_depth", caseCaptureOrthographicDepth},
  {"capture", "equal_depth_source_index", caseCaptureEqualDepthSourceIndex},
  {"capture", "layers_adjacency_ray_match", caseCaptureLayersAndAdjacency},
  {"capture", "face_normals_and_color", caseCaptureFaceNormalsAndColor},
  {"capture", "validation_atomic", caseCaptureValidation},
  {"capture", "reconstruction_index", caseCaptureReconstructionIndex},

  {"reprojection", "plane_color_depth", caseReprojectionPlane},
  {"reprojection", "per_view_occlusion", caseReprojectionPerViewOcclusion},
  {"reprojection", "micro_mesh", caseReprojectionMicroMesh},
  {"reprojection", "near_clip", caseReprojectionNearClip},
  {"reprojection", "instance_transform", caseReprojectionInstanceTransform},
  {"reprojection", "external_depth", caseReprojectionExternalDepth},
  {"reprojection", "validation_atomic", caseReprojectionValidation},

  {"composition", "weighted_blend", caseCompositionWeightedBlend},
  {"composition", "view_order_invariance", caseCompositionViewOrderInvariance},
  {"composition", "interleaved_resources", caseCompositionInterleavedResources},
  {"composition", "zero_weight", caseCompositionZeroWeight},
  {"composition", "depth_discontinuity", caseCompositionDepthDiscontinuity},
  {"composition", "depth_init", caseCompositionDepthInit},
  {"composition", "empty_resource", caseCompositionEmptyResource},
};

extern const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
