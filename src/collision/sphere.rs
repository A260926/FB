use crate::collision::{AABB, Ray, RayHit};
use glam::{Vec3, Vec3A};

/// Longitude/latitude parameterization of a unit direction in the sphere's local space.
///
/// `n` is the unit direction from the sphere centre to the hit point. `u` is measured around
/// the Y axis with `atan2(n.z, n.x)` and wraps into `[0, 1)`, while `v` runs from the north
/// pole (`n.y == 1`) through the equator to the south pole (`n.y == -1`).
///
/// This lives with the geometry rather than with the material: a sphere always has a
/// parameterization, whether the material's texture happens to be an `Image`, a `Checker`,
/// a constant or anything else.
#[inline]
pub fn sphere_uv(n: Vec3) -> (f32, f32) {
    const FRAC_1_2PI: f64 = 1.0 / (2.0 * core::f64::consts::PI);

    // The hit normal is reconstructed from the hit point, so normalize before using its
    // components as a direction. Compute in f64 and round once at the end so axis-aligned
    // directions land on the exact longitudes/latitudes (for example +Z keeps u == 0.25
    // instead of landing a hair on the wrong side of a texel boundary).
    let x = n.x as f64;
    let y = n.y as f64;
    let z = n.z as f64;
    let len = (x * x + y * y + z * z).sqrt();
    let (x, y, z) = if len > 0.0 {
        (x / len, y / len, z / len)
    } else {
        (0.0, 0.0, 0.0)
    };

    let phi = z.atan2(x);
    let mut u = 1.0 - (phi + core::f64::consts::PI) * FRAC_1_2PI;
    // Wrap into [0, 1). `floor` maps the -X seam (phi == ±PI, including a signed zero in
    // `n.z`) to the same longitude instead of leaving it as exactly 1 or -0.
    u -= u.floor();
    if u >= 1.0 {
        u = 0.0;
    }
    let v = 0.5 + y.clamp(-1.0, 1.0).asin() * core::f64::consts::FRAC_1_PI;
    (u as f32, v as f32)
}

// #[derive(Clone, Copy, Debug, Serialize, Deserialize)]
#[derive(Clone, Copy, Debug)]
pub struct Sphere {
    centre: Vec3,
    radius: f32,
}

impl Sphere {
    #[inline]
    pub fn new(centre: Vec3, radius: f32) -> Sphere {
        Sphere { centre, radius }
    }

    #[inline]
    pub fn centre(&self) -> Vec3 {
        self.centre
    }

    #[inline]
    pub fn radius(&self) -> f32 {
        self.radius
    }

    #[inline]
    pub fn ray_hit(&self, ray: &Ray, t_min: f32, t_max: f32) -> Option<RayHit> {
        let centre = Vec3A::from(self.centre);
        let ray_origin = Vec3A::from(ray.origin);
        let ray_direction = Vec3A::from(ray.direction);
        let oc = ray_origin - centre;
        let a = ray_direction.dot(ray_direction);
        let b = oc.dot(ray_direction);
        let c = oc.dot(oc) - self.radius * self.radius;
        let discriminant = b * b - a * c;
        if discriminant > 0.0 {
            let discriminant_sqrt = discriminant.sqrt();
            let t = (-b - discriminant_sqrt) / a;
            if t < t_max && t > t_min {
                let point = ray.point_at_parameter(t);
                let normal = (point - self.centre) / self.radius;
                let (u, v) = sphere_uv(normal);
                return Some(RayHit {
                    point,
                    normal,
                    t,
                    u,
                    v,
                });
            }
            let t = (-b + discriminant_sqrt) / a;
            if t < t_max && t > t_min {
                let point = ray.point_at_parameter(t);
                let normal = (point - self.centre) / self.radius;
                let (u, v) = sphere_uv(normal);
                return Some(RayHit {
                    point,
                    normal,
                    t,
                    u,
                    v,
                });
            }
        }
        None
    }

    #[inline]
    pub fn bounding_box(&self) -> AABB {
        let radius = Vec3::splat(self.radius);
        AABB {
            min: self.centre - radius,
            max: self.centre + radius,
        }
    }
}
