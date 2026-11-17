#ifndef SPHERE_CONNECTION_HPP_
#define SPHERE_CONNECTION_HPP_

#include "objects.hpp"
#include "vec.hpp"
#include <vector>

// One valid air -> sphere -> air light path between a receiver point P
// and a point light L, refracting twice on the same sphere (no internal
// reflection). All solving is carried out in double precision.
struct SphereConnection {
  // first (air -> sphere) and second (sphere -> air) surface points,
  // stored in world coordinates
  Vec3f entry;
  Vec3f exit;
  // material transparency squared times the exact unpolarized Fresnel
  // power transmittance of both interfaces
  double transmission{0.0};
};

// Find every isolated simple P -> entry -> exit -> L connection through
// `sphere` whose three open segments have strictly positive length and
// whose air-side incidence cosines are at least 0.1 at both interfaces.
// P and L are guaranteed to lie outside the sphere. The returned
// connections are sorted by increasing signed emission angle at P
// (positive towards the projection of (sphere center -> L) on the plane
// perpendicular to the P -> center axis) and de-duplicated. Returns an
// empty vector when no such connection exists.
std::vector<SphereConnection> ConnectSphere(const Sphere& sphere,
                                            const Vec3f& P,
                                            const Vec3f& L);

#endif // SPHERE_CONNECTION_HPP_
