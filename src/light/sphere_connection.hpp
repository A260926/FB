#ifndef SPHERE_CONNECTION_HPP_
#define SPHERE_CONNECTION_HPP_

#include "objects.hpp"
#include <vector>

// One physically valid air -> sphere -> air light path between an
// exterior receiver point P and an exterior point light L.
// `entry` and `exit` are the two sphere surface points (world coords),
// `transmission` is the fraction of light carried along this path.
struct SphereConnection {
  Vec3f entry{};
  Vec3f exit{};
  double transmission{0.0};
};

// Find every isolated simple P -> entry -> exit -> L connection through
// `sphere` that obeys Snell's law at both interfaces (no internal
// reflection). All computations are carried out in double precision.
// Returns an empty vector when no such connection exists.
// The result is sorted by the signed emission angle at P (looking
// towards the sphere center) and duplicate solutions are removed.
std::vector<SphereConnection>
ConnectSphere(const Sphere &sphere, const Vec3f &P, const Vec3f &L);

#endif // SPHERE_CONNECTION_HPP_
