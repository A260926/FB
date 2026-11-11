include!("core_modules.rs");
use glam::{Vec3,vec3};
use collision::{Sphere,Ray,Hitable};
use rand::SeedableRng;
use rand_xoshiro::Xoshiro256Plus;
fn uv_eq(a: (f32, f32), b: (f32, f32)) -> bool {
    (a.0 - b.0).abs() <= 2e-6 && (a.1 - b.1).abs() <= 2e-6
}
fn main(){
 let file=std::env::args().nth(1).unwrap();
 let pixels:[u8;24]=[255,0,0,0,255,0,0,0,255,255,255,0,255,0,0,0,255,0,0,0,255,255,255,0];
 image::save_buffer(&file,&pixels,4,2,image::ColorType::Rgb8).unwrap();
 let img=texture::RgbImage::open(&file);let tex=texture::rgb_image(&img);let mat=material::diffuse_light(&tex);
 let a=mat.get_sphere_uv(Vec3::Z);let b=mat.get_sphere_uv(vec3(0.,0.,-1.));assert!(uv_eq(a,(0.25,0.5)));assert!(uv_eq(b,(0.75,0.5)));
 println!("CURRENT_UV plus_z={:?} minus_z={:?}",a,b);
 let sphere=Sphere::new(Vec3::ZERO,1.);let h=Hitable::Sphere(&sphere,&mat);
 let ray=Ray::new(vec3(0.,0.,3.),-Vec3::Z,0.5);let mut rng=Xoshiro256Plus::seed_from_u64(5);
 let (hit,_) = h.ray_hit(&ray,0.001,100.,&mut rng).unwrap();assert!(uv_eq((hit.u,hit.v),(0.25,0.5)));
 let soa=collision::SpheresSoA::new(&[h]);let (sh,_) = soa.hit_scalar(&ray,0.001,100.).unwrap();assert!(uv_eq((sh.u,sh.v),(hit.u,hit.v)));
 println!("SOA_CURRENT_UV {:?}",(sh.u,sh.v));
 let nested=texture::checker(&tex,&tex);let nested_mat=material::diffuse_light(&nested);
 assert!(uv_eq(nested_mat.get_sphere_uv(Vec3::Z),(0.25,0.5)));
 println!("NESTED_CURRENT_UV {:?}",nested_mat.get_sphere_uv(Vec3::Z));
 let world=scene::World::Hitable(h);let scene=scene::Scene::new(world,Some(Vec3::ZERO));
 let cam=camera::Camera::new(vec3(0.,0.,3.),Vec3::ZERO,Vec3::Y,10.,2.,0.,3.,0.,1.);
 let p=params::Params{width:4,height:2,samples:4,max_depth:0,random_seed:false,use_bvh:false,soa:params::SoaMode::Off};
 let mut out=vec![(0.,0.,0.);8];let rays=scene.update(&p,&cam,0,&mut out);
 let sum:f32=out.iter().map(|c|c.0+c.1+c.2).sum();assert!(sum.is_finite()&&sum>0.&&rays==32);
 println!("SPHERE_RENDER rays={} rgb_sum={:.6}",rays,sum);println!("SPHERE_UV_BASELINE_OK");
}
