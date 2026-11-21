#ifndef ANDREW_LIM_GRAPHICS_IMPOSTOR_H
#define ANDREW_LIM_GRAPHICS_IMPOSTOR_H

/*
Depth impostor: a multi-view depth proxy that can be drawn without the
original mesh.

Capture records, for each orthographic view, the color, local position, unit
face normal and validity of the nearest surface at every grid point. Those
records are turned into small perspective-correct micro triangles. Drawing
rebuilds the surface from the records only - the source triangles, texture
pointers and callbacks are never retained.

All math is C++17 + the standard library + the surrounding sw3dcpp sources.
*/

#include "ImageData.h"
#include "Matrix.h"
#include "Triangle.h"
#include "Vector2f.h"
#include "Vector3f.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace al { namespace graphics {

// One orthographic capture view. right/up/forward must all be unit length and
// orthogonal with forward == right x up. Distance along forward increases as
// the point moves away from the eye (the view looks down +forward). Pixels
// are laid out with +y pointing down the image.
struct CaptureView
{
  Vector3f eye;
  Vector3f right;
  Vector3f up;
  Vector3f forward;
  float halfWidth;
  float halfHeight;
  float nearPlane;
  float farPlane;
  std::uint32_t width;
  std::uint32_t height;
};

// Per-grid-point record of the winning fragment for one view. Grid points are
// indexed by row-major (y*width + x); the sampled location is the pixel
// center (x+0.5, y+0.5).
struct ImpostorRecord
{
  bool valid;
  Vector3f color;      // rgb / 255, stored as float
  Vector3f position;   // local-space position of the surface
  Vector3f normal;     // local-space unit face normal
  float depth;         // orthographic depth along forward from the eye

  ImpostorRecord()
  : valid(false), color(), position(), normal(), depth(0)
  {}
};

// One capture view's records plus the reconstruction micro-triangle indices.
struct ImpostorView
{
  CaptureView view;
  std::vector<ImpostorRecord> records;
  // Each entry holds the 3 record indices of one retained micro triangle.
  std::vector<std::uint32_t> triangles;

  ImpostorView() : view(), records(), triangles() {}
};

// The captured resource. Holds only its own data; no source triangle,
// texture pointer or callback survives capture.
class DepthImpostor
{
public:
  DepthImpostor();

  std::size_t viewCount() const { return views_.size(); }
  const ImpostorView& view(std::size_t i) const { return views_[i]; }
  const std::vector<ImpostorView>& views() const { return views_; }

  // Swaps in a freshly captured resource.
  void swap(DepthImpostor& other);

private:
  friend bool captureImpostor(const std::vector<Triangle>&,
                              const std::vector<CaptureView>&,
                              float,
                              DepthImpostor&);
  friend bool drawImpostor(const DepthImpostor&,
                           const Matrix4f&,
                           const Matrix4f&,
                           const Vector3f&,
                           ImageData&,
                           std::vector<float>&,
                           float);

  std::vector<ImpostorView> views_;
};

// Captures the opaque source triangles into a depth impostor.
//
// inputTriangles must be in local space with an opaque color. maxEdge is the
// maximum local-space length of a reconstruction micro-triangle edge (finite
// and positive). On failure the existing out resource is left untouched and
// false is returned.
bool captureImpostor(const std::vector<Triangle>& inputTriangles,
                     const std::vector<CaptureView>& views,
                     float maxEdge,
                     DepthImpostor& out);

// Draws the impostor with a perspective camera.
//
// localToWorld is a reversible affine transform, worldToClip a perspective
// projection (z/w smaller is nearer, visible when -w<=x,y,z<=w and w>0).
// cameraWorld is the world-space camera position. colorOut must be the same
// size as depthOut. depthOut is a projected-depth target treated as an
// existing occluder set: a fragment only overwrites the color and depth where
// it is strictly nearer than the current depth. Initialise depthOut to
// positive infinity for an empty target. mergeDistance is a non-negative
// finite merge tolerance.
//
// On invalid arguments false is returned before any caller buffer is touched.
bool drawImpostor(const DepthImpostor& impostor,
                  const Matrix4f& localToWorld,
                  const Matrix4f& worldToClip,
                  const Vector3f& cameraWorld,
                  ImageData& colorOut,
                  std::vector<float>& depthOut,
                  float mergeDistance);

} // namespace graphics
} // namespace al

#endif
