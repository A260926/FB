#ifndef OFF_AXIS_PERSPECTIVE_CAMERA_H
#define OFF_AXIS_PERSPECTIVE_CAMERA_H

#include "PERSPECTIVECAMERA.H"

// A perspective camera with the same orientation and intrinsics (fov, aspect,
// viewport, near/far) as a reference PerspectiveCamera, but whose center of
// projection is translated to a thin-lens pupil and whose optical axis stays
// parallel to the reference camera (an off-axis / shifted-lens frustum).
//
// The pupil offset (a, b) is expressed in the reference camera's local frame
// (right = +X, up = +Y). Keeping the orientation identical while shifting the
// projection makes the focal plane Z = F land on exactly the same raster
// coordinates as the reference pinhole camera while nearer/farther geometry
// acquires the lens parallax.
//
// In this camera's own view space (origin at the pupil) the imaging equation is
//   ndc.x = A0 * (Xc/Zc + a/F)
//   ndc.y = A1 * (Yc/Zc + b/F)
// which, substituting the pupil view coordinates (Xc = X - a, Yc = Y - b),
// reproduces the thin-lens form in the reference camera frame
//   X/Z + a*(1/F - 1/Z),  Y/Z + b*(1/F - 1/Z).
class OffAxisPerspectiveCamera final : public PerspectiveCamera {
private:
  double m_pupil_a;
  double m_pupil_b;
  double m_focal_F;
  double m_focal_scale_x; // A0
  double m_focal_scale_y; // A1

public:
  OffAxisPerspectiveCamera(const PerspectiveCamera& reference,
                           const Point3D& pupil_world,
                           double pupil_a,
                           double pupil_b,
                           double focal_F,
                           Renderer* renderer);

  // Exact analytic inverse of the off-axis projection. Given the NDC point and
  // the view-space depth Zc, returns the point in this pupil camera's view
  // space. It matches projectTransform()/rasterize()/unrasterize() exactly and
  // does not reuse the centered-camera matrix inverse.
  const Point3D projectTransformInv(const Point2D& point_projected, const double depth) const override;

  // View direction measured from the pupil position, so shading uses the same
  // per-pupil camera that produced the coverage and depth.
  const Vector3D viewDirection(const Point3D& point) const override;

  inline double pupil_a() const { return m_pupil_a; }
  inline double pupil_b() const { return m_pupil_b; }
  inline double focal_distance() const { return m_focal_F; }
  inline double focal_scale_x() const { return m_focal_scale_x; }
  inline double focal_scale_y() const { return m_focal_scale_y; }
};

#endif // OFF_AXIS_PERSPECTIVE_CAMERA_H
