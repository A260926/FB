use crate::{
    collision::{Ray, RayHit, sphere_uv},
    math::{random_in_unit_sphere, random_unit_vector, reflect, refract, schlick},
    texture::Texture,
};
use glam::{Vec3, vec3};
use rand::RngExt;
use rand_xoshiro::Xoshiro256Plus;

// #[derive(Clone, Copy, Debug, Serialize, Deserialize)]
#[derive(Clone, Copy, Debug)]
pub enum Material<'a> {
    Lambertian { albedo: &'a Texture<'a> },
    Metal { albedo: Vec3, fuzz: f32 },
    Dielectric { ref_idx: f32 },
    DiffuseLight { emit: &'a Texture<'a> },
    Isotropic { albedo: &'a Texture<'a> },
}

pub fn lambertian<'a>(albedo: &'a Texture<'a>) -> Material<'a> {
    Material::Lambertian { albedo }
}

pub fn metal<'a>(albedo: Vec3, fuzz: f32) -> Material<'a> {
    Material::Metal { albedo, fuzz }
}

pub fn dielectric<'a>(ref_idx: f32) -> Material<'a> {
    Material::Dielectric { ref_idx }
}

pub fn diffuse_light<'a>(emit: &'a Texture<'a>) -> Material<'a> {
    Material::DiffuseLight { emit }
}

pub fn isotropic<'a>(albedo: &'a Texture<'a>) -> Material<'a> {
    Material::Isotropic { albedo }
}


impl<'a> Material<'a> {
    fn scatter_lambertian(
        albedo: &Texture,
        ray_in: &Ray,
        ray_hit: &RayHit,
        rng: &mut Xoshiro256Plus,
    ) -> Option<(Vec3, Ray)> {
        let target = ray_hit.point + ray_hit.normal + random_unit_vector(rng);
        Some((
            albedo.value(ray_hit.u, ray_hit.v, ray_hit.point),
            Ray::new(
                ray_hit.point,
                (target - ray_hit.point).normalize(),
                ray_in.time,
            ),
        ))
    }

    fn scatter_metal(
        albedo: &Vec3,
        fuzz: f32,
        ray_in: &Ray,
        ray_hit: &RayHit,
        rng: &mut Xoshiro256Plus,
    ) -> Option<(Vec3, Ray)> {
        let reflected = reflect(ray_in.direction, ray_hit.normal);
        if reflected.dot(ray_hit.normal) > 0.0 {
            Some((
                *albedo,
                Ray::new(
                    ray_hit.point,
                    (reflected + fuzz * random_in_unit_sphere(rng)).normalize(),
                    ray_in.time,
                ),
            ))
        } else {
            None
        }
    }

    fn scatter_dielectric(
        ref_idx: f32,
        ray_in: &Ray,
        ray_hit: &RayHit,
        rng: &mut Xoshiro256Plus,
    ) -> Option<(Vec3, Ray)> {
        let attenuation = vec3(1.0, 1.0, 1.0);
        let rdotn = ray_in.direction.dot(ray_hit.normal);
        let (outward_normal, ni_over_nt, cosine) = if rdotn > 0.0 {
            let cosine = rdotn / ray_in.direction.length();
            let cosine = (1.0 - ref_idx * ref_idx * (1.0 - cosine * cosine)).sqrt();
            (-ray_hit.normal, ref_idx, cosine)
        } else {
            let cosine = -rdotn / ray_in.direction.length();
            (ray_hit.normal, 1.0 / ref_idx, cosine)
        };
        if let Some(refracted) = refract(ray_in.direction, outward_normal, ni_over_nt) {
            let reflect_prob = schlick(cosine, ref_idx);
            if rng.random::<f32>() > reflect_prob {
                return Some((
                    attenuation,
                    Ray::new(ray_hit.point, refracted.normalize(), ray_in.time),
                ));
            }
        }
        Some((
            attenuation,
            Ray::new(
                ray_hit.point,
                reflect(ray_in.direction, ray_hit.normal).normalize(),
                ray_in.time,
            ),
        ))
    }

    fn scatter_isotropic(
        albedo: &Texture,
        ray_in: &Ray,
        ray_hit: &RayHit,
        rng: &mut Xoshiro256Plus,
    ) -> Option<(Vec3, Ray)> {
        Some((
            albedo.value(ray_hit.u, ray_hit.v, ray_hit.point),
            Ray::new(ray_hit.point, random_in_unit_sphere(rng), ray_in.time),
        ))
    }

    pub fn scatter(
        &self,
        ray: &Ray,
        ray_hit: &RayHit,
        rng: &mut Xoshiro256Plus,
    ) -> Option<(Vec3, Ray)> {
        match self {
            Material::Lambertian { albedo } => {
                Material::scatter_lambertian(albedo, ray, ray_hit, rng)
            }
            Material::Metal { albedo, fuzz } => {
                Material::scatter_metal(albedo, *fuzz, ray, ray_hit, rng)
            }
            Material::Dielectric { ref_idx } => {
                Material::scatter_dielectric(*ref_idx, ray, ray_hit, rng)
            }
            Material::Isotropic { albedo } => {
                Material::scatter_isotropic(albedo, ray, ray_hit, rng)
            }
            Material::DiffuseLight { emit: _ } => None,
        }
    }

    pub fn emitted(&self, u: f32, v: f32, point: Vec3) -> Vec3 {
        if let Material::DiffuseLight { emit } = self {
            emit.value(u, v, point)
        } else {
            Vec3::ZERO
        }
    }

    pub fn get_sphere_uv(&self, normal: Vec3) -> (f32, f32) {
        // Kept as a public compatibility helper. Geometry no longer calls this method to
        // decide whether a sphere has UVs; `Sphere::ray_hit` and `MovingSphere::ray_hit`
        // always attach the geometric parameterization themselves.
        sphere_uv(normal)
    }
}
