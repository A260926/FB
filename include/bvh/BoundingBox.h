#ifndef BOUNDINGBOX_H
#define BOUNDINGBOX_H

#include"util/Utilities.h"
#include <cassert>

class BoundingBox {
  public:
    Interval x, y, z;

    BoundingBox() {} // The default AABB is empty, since intervals are empty by default.

    BoundingBox(const Interval& x, const Interval& y, const Interval& z)
      : x(x), y(y), z(z) {
        pad_to_minimums();
      }

    BoundingBox(const Point3& a, const Point3& b) {
        // Treat the two points a and b as extrema for the bounding box, so we don't require a
        // particular minimum/maximum coordinate order.

        x = construct_interval(a[0], b[0]);
        y = construct_interval(a[1], b[1]);
        z = construct_interval(a[2], b[2]);

        pad_to_minimums();
    }

    BoundingBox(const BoundingBox& box0, const BoundingBox& box1) {
        x = Interval(box0.x, box1.x);
        y = Interval(box0.y, box1.y);
        z = Interval(box0.z, box1.z);
    }

    const Interval& axis_interval(int n) const {
        assert(n >= 0 && n < 3);
        if (n == 1) return y;
        if (n == 2) return z;
        return x;
    }

    bool intersect(const Ray& r, Interval ray_t) const {
        const Point3& ray_orig = r.origin();
        const Vector3& ray_inv_dir = r.inv_direction();

        if (!axis_intersect(ray_orig[0], ray_inv_dir[0], x, ray_t)) return false;
        if (!axis_intersect(ray_orig[1], ray_inv_dir[1], y, ray_t)) return false;
        if (!axis_intersect(ray_orig[2], ray_inv_dir[2], z, ray_t)) return false;

        return true;
    }

    double surface_area() const {
        // Calculate the surface area of the AABB
        double dx = x.size();
        double dy = y.size();
        double dz = z.size();

        double face1 = dx * dy;
        double face2 = dy * dz;
        double face3 = dz * dx;

        return 2.0 * (face1 + face2 + face3);
    }

    private:
        Interval construct_interval(double a, double b) const {
            // Construct an interval from two values
            return (a <= b) ? Interval(a, b) : Interval(b, a);
        }

        void pad_to_minimums() {
            // Adjust the AABB so that no side is narrower than some delta, padding if necessary.

            double delta = 0.0001;
            if (x.size() < delta) x = x.expand(delta);
            if (y.size() < delta) y = y.expand(delta);
            if (z.size() < delta) z = z.expand(delta);
        }

        bool axis_intersect(double origin, double adinv, const Interval& axis, Interval& ray_t) const {
            auto t0 = (axis.min - origin) * adinv;
            auto t1 = (axis.max - origin) * adinv;

            if (t0 < t1) {
                if (t0 > ray_t.min) ray_t.min = t0;
                if (t1 < ray_t.max) ray_t.max = t1;
            } else {
                if (t1 > ray_t.min) ray_t.min = t1;
                if (t0 < ray_t.max) ray_t.max = t0;
            }

            return ray_t.max > ray_t.min;
        }
};

#endif