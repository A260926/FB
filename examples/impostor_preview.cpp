// Depth impostor preview example.
//
// Builds a small multi-layer scene in code, captures it into a DepthImpostor,
// destroys the source triangles, and renders two views to PPM (P6) files using
// only the captured resource.
//
// Usage: impostor_preview <output-directory>

#include "Impostor.h"
#include "Graphics3D.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace al::graphics;

namespace {

const int kWidth = 320;
const int kHeight = 240;

u32 opaqueRgb(int r, int g, int b)
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

// A simple layered model: a red/green checker front panel with a gap, a
// tilted cyan wedge behind it, and a distant blue backstop. It has front and
// back layers, a slanted face, an occlusion notch and adjacent differently
// colored faces.
std::vector<Triangle> buildModel()
{
  std::vector<Triangle> tris;
  const float zFront = 0.0f;
  const int cells = 4;
  const float step = 2.0f / cells;
  for (int cy = 0; cy < cells; ++cy) {
    for (int cx = 0; cx < cells; ++cx) {
      // Leave a notch in the upper-right of the front panel.
      if (cx == cells - 1 && cy == 0) {
        continue;
      }
      const float x0 = -1.0f + cx * step;
      const float x1 = x0 + step;
      const float y0 = -1.0f + cy * step;
      const float y1 = y0 + step;
      const bool red = ((cx + cy) & 1) == 0;
      const u32 color = red ? opaqueRgb(230, 60, 60) : opaqueRgb(60, 200, 90);
      tris.push_back(triangle(x0, y0, zFront, x1, y0, zFront, x1, y1, zFront,
                              color));
      tris.push_back(triangle(x0, y0, zFront, x1, y1, zFront, x0, y1, zFront,
                              color));
    }
  }
  // Tilted cyan wedge behind the panel.
  tris.push_back(triangle(-1.0f, -0.8f, -1.6f,
                          0.9f, -0.8f, -2.2f,
                          0.9f, 0.8f, -2.2f,
                          opaqueRgb(70, 200, 220)));
  tris.push_back(triangle(-1.0f, -0.8f, -1.6f,
                          0.9f, 0.8f, -2.2f,
                          -1.0f, 0.8f, -1.2f,
                          opaqueRgb(70, 200, 220)));
  // Distant blue backstop.
  tris.push_back(triangle(-1.3f, -1.1f, -3.0f,
                          1.3f, -1.1f, -3.0f,
                          1.3f, 1.1f, -3.0f,
                          opaqueRgb(60, 90, 230)));
  tris.push_back(triangle(-1.3f, -1.1f, -3.0f,
                          1.3f, 1.1f, -3.0f,
                          -1.3f, 1.1f, -3.0f,
                          opaqueRgb(60, 90, 230)));
  return tris;
}

CaptureView makeView(const Vector3f& eye,
                     const Vector3f& right,
                     const Vector3f& upIn,
                     float halfWidth, float halfHeight,
                     float nearPlane, float farPlane,
                     u32 width, u32 height)
{
  CaptureView v;
  v.eye = eye;
  v.right = G3D::normalize(right);
  v.up = G3D::normalize(upIn);
  // forward = right x up, exactly, per the capture contract.
  v.forward = G3D::normalize(G3D::crossProduct(v.right, v.up));
  v.halfWidth = halfWidth;
  v.halfHeight = halfHeight;
  v.nearPlane = nearPlane;
  v.farPlane = farPlane;
  v.width = width;
  v.height = height;
  return v;
}

// Builds a perspective view-projection matrix for a camera at eye looking
// along `forward`. Matches Matrix4::makePerspective, which expects camera
// space with -z pointing forward, so the view matrix maps `forward` to -z.
Matrix4f makePerspectiveView(const Vector3f& eye,
                             const Vector3f& right,
                             const Vector3f& up,
                             const Vector3f& forward,
                             float fovY, float aspect,
                             float nearPlane, float farPlane)
{
  Matrix4f view;
  view.fill(0.0f);
  view.set(0, 0, right.x());
  view.set(1, 0, right.y());
  view.set(2, 0, right.z());
  view.set(0, 1, up.x());
  view.set(1, 1, up.y());
  view.set(2, 1, up.z());
  view.set(0, 2, -forward.x());
  view.set(1, 2, -forward.y());
  view.set(2, 2, -forward.z());
  view.set(3, 0, -G3D::dot(right, eye));
  view.set(3, 1, -G3D::dot(up, eye));
  view.set(3, 2, G3D::dot(forward, eye));
  view.set(3, 3, 1.0f);
  Matrix4f proj = Matrix4f::makePerspective(fovY, aspect, nearPlane, farPlane);
  return proj * view;
}

// Derives an orthonormal camera basis (right, up, forward) from an eye point
// and a look-at target, with forward == right x up.
void cameraBasis(const Vector3f& eye, const Vector3f& target,
                 Vector3f& right, Vector3f& up, Vector3f& forward)
{
  forward = G3D::normalize(Vector3f(target.x()-eye.x(),
                                    target.y()-eye.y(),
                                    target.z()-eye.z()));
  const Vector3f worldUp(0.0f, 1.0f, 0.0f);
  right = G3D::normalize(G3D::crossProduct(worldUp, forward));
  up = G3D::crossProduct(forward, right);
}

bool writePpm(const std::string& path, const ImageData& image)
{
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    return false;
  }
  std::fprintf(f, "P6\n%u %u\n255\n", image.width(), image.height());
  for (u32 y = 0; y < image.height(); ++y) {
    for (u32 x = 0; x < image.width(); ++x) {
      const u32 p = image.getPixel(x, y);
      const u8 rgb[3] = {
        static_cast<u8>((p >> 0) & 0xffu),
        static_cast<u8>((p >> 8) & 0xffu),
        static_cast<u8>((p >> 16) & 0xffu)
      };
      std::fwrite(rgb, 1, 3, f);
    }
  }
  std::fclose(f);
  return true;
}

bool renderView(const DepthImpostor& impostor,
                const Matrix4f& localToWorld,
                const Matrix4f& worldToClip,
                const Vector3f& cameraWorld,
                const std::string& path)
{
  ImageData color(kWidth, kHeight);
  color.fill(0);
  // An empty depth target starts at positive infinity; draw only writes where
  // a fragment is strictly nearer.
  std::vector<float> depth(static_cast<std::size_t>(kWidth) * kHeight,
                           std::numeric_limits<float>::infinity());
  if (!drawImpostor(impostor, localToWorld, worldToClip, cameraWorld,
                    color, depth, 0.75f)) {
    return false;
  }
  return writePpm(path, color);
}

} // namespace

int main(int argc, char** argv)
{
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
    return 1;
  }
  const std::string outDir = argv[1];

  // Capture views: two orthographic views from opposite sides.
  std::vector<CaptureView> views;
  // Eye at +z looking toward -z; forward = right x up = (0,0,-1) with
  // right = (-1,0,0), up = (0,1,0).
  views.push_back(makeView(Vector3f(0.0f, 0.0f, 4.0f),
                           Vector3f(-1.0f, 0.0f, 0.0f),
                           Vector3f(0.0f, 1.0f, 0.0f),
                           2.2f, 1.65f, 1.0f, 8.0f, 192, 144));
  // Oblique second view from the front-right so it still sees the panel.
  {
    const Vector3f eye(2.6f, 0.7f, 3.2f);
    const Vector3f fwd = G3D::normalize(Vector3f(-2.6f, -0.7f, -3.2f));
    const Vector3f right = G3D::normalize(
        G3D::crossProduct(Vector3f(0.0f, 1.0f, 0.0f), fwd));
    const Vector3f up = G3D::crossProduct(fwd, right);
    views.push_back(makeView(eye, right, up, 2.2f, 1.65f, 1.0f, 8.0f,
                             192, 144));
  }

  DepthImpostor impostor;
  std::vector<Triangle> model = buildModel();
  const bool captured = captureImpostor(model, views, 0.35f, impostor);
  // Destroy the source model before drawing: nothing in the impostor may
  // reference it.
  model.clear();
  model.shrink_to_fit();
  if (!captured) {
    std::fprintf(stderr, "capture failed\n");
    return 2;
  }

  const Matrix4f identity = Matrix4f();
  const float aspect = static_cast<float>(kWidth) / kHeight;

  // View 0: front, slightly to the side.
  const Vector3f eye0(1.4f, 0.5f, 4.2f);
  const Vector3f target0(0.0f, 0.0f, -0.5f);
  Vector3f right0, up0, forward0;
  cameraBasis(eye0, target0, right0, up0, forward0);
  Matrix4f cam0 = makePerspectiveView(eye0, right0, up0, forward0,
                                      G3D::deg2rad(52.0f), aspect,
                                      1.0f, 12.0f);
  if (!renderView(impostor, identity, cam0, eye0, outDir + "/view0.ppm")) {
    std::fprintf(stderr, "view0 failed\n");
    return 3;
  }

  // View 1: from the other side, looking back.
  const Vector3f eye1(-1.2f, -0.4f, 5.0f);
  const Vector3f target1(0.0f, 0.0f, 0.0f);
  Vector3f right1, up1, forward1;
  cameraBasis(eye1, target1, right1, up1, forward1);
  Matrix4f cam1 = makePerspectiveView(eye1, right1, up1, forward1,
                                      G3D::deg2rad(58.0f), aspect,
                                      1.0f, 12.0f);
  if (!renderView(impostor, identity, cam1, eye1, outDir + "/view1.ppm")) {
    std::fprintf(stderr, "view1 failed\n");
    return 4;
  }

  return 0;
}
