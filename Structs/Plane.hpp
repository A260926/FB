#ifndef QHPLANE_HPP_
#define QHPLANE_HPP_

#include "Vector3.hpp"

namespace quickhull {

	template<typename T>
	class Plane {
	public:
		Vector3<T> m_N;

		// Signed distance (if normal is of length 1) to the plane from origin.
		// Kept for compatibility. Side tests must not be evaluated as m_N.dot(Q)+m_D
		// directly: far away from the origin two large world-space terms would cancel
		// and the local residual would be lost. Use isPointOnPositiveSide() /
		// mathutils::getSignedDistanceToPlane(), which evaluate m_N.dot(Q - m_point).
		T m_D;

		// A point of the plane. All signed distances are evaluated relative to it,
		// which makes the result invariant under translations of the point cloud.
		Vector3<T> m_point;

		// Normal length squared
		T m_sqrNLength;

		bool isPointOnPositiveSide(const Vector3<T>& Q) const {
			T d = m_N.dotProduct(Q - m_point);
			if (d>=0) return true;
			return false;
		}

		Plane() = default;

		// Construct a plane using normal N and any point P on the plane
		Plane(const Vector3<T>& N, const Vector3<T>& P) :
			m_N(N), m_D(-N.dotProduct(P)), m_point(P),
			m_sqrNLength(m_N.x*m_N.x+m_N.y*m_N.y+m_N.z*m_N.z)
		{

		}
	};

}


#endif /* PLANE_HPP_ */
