#include "OffAxisPerspectiveCamera.h"

OffAxisPerspectiveCamera::OffAxisPerspectiveCamera(const PerspectiveCamera& reference,
                                                   const Point3D& pupil_world,
                                                   double pupil_a,
                                                   double pupil_b,
                                                   double focal_F,
                                                   Renderer* renderer)
  // Reuse the full perspective setup (look-at, viewport and projection matrices)
  // with an identical orientation: the target is the pupil translated along the
  // reference viewing direction, so right/up/direction are unchanged.
  : PerspectiveCamera(pupil_world,
                      pupil_world + reference.get_direction(),
                      reference.get_height(),
                      reference.get_width(),
                      renderer,
                      reference.get_near_plane(),
                      reference.get_far_plane(),
                      reference.m_fov),
    m_pupil_a(pupil_a),
    m_pupil_b(pupil_b),
    m_focal_F(focal_F) {

  // The base class stores the focal scales at these entries after transpose.
  m_focal_scale_x = m_project[0][0];
  m_focal_scale_y = m_project[1][1];

  // Off-axis shift: add the principal-point offset that keeps the focal plane
  // fixed. In logical (column-vector) form these are L[0][2] += A0*a/F and
  // L[1][2] += A1*b/F; because the code transposes the matrix, they live at
  // stored m_project[0][2] / m_project[1][2].
  m_project[0][2] += m_focal_scale_x * (m_pupil_a / m_focal_F);
  m_project[1][2] += m_focal_scale_y * (m_pupil_b / m_focal_F);

  m_project_inv = glm::inverse(m_project);
}

const Point3D OffAxisPerspectiveCamera::projectTransformInv(const Point2D& point_projected, const double depth) const {
  // Exact inverse of
  //   ndc.x = A0 * (Xc/Zc + a/F),  ndc.y = A1 * (Yc/Zc + b/F)
  // so that projectTransform/unrasterize round-trip in the pupil view space.
  const double Zc = depth;
  const double Xc = Zc * (point_projected.x / m_focal_scale_x - m_pupil_a / m_focal_F);
  const double Yc = Zc * (point_projected.y / m_focal_scale_y - m_pupil_b / m_focal_F);
  return Point3D(Xc, Yc, Zc);
}

const Vector3D OffAxisPerspectiveCamera::viewDirection(const Point3D& point) const {
  // Material observation direction measured from the pupil, never from the
  // center pinhole position.
  Vector3D view = point - m_position;
  view.normalize();
  return view;
}
