use rpt::*;
fn main(){
 let n=glm::vec3(0.,0.,1.);let white=glm::vec3(1.,1.,1.);
 let mat=Material::clear(1.5,0.3);let wi=-n;
 let f=mat.bsdf(&n,&n,&wi);assert!(f.x.is_finite()&&f.x>0.);
 println!("TRANSMISSION bsdf={:.9} signed_direct={:.9}",f.x,f.x*wi.dot(&n));
 let p=plane(n,0.);let ray=Ray{origin:glm::vec3(0.,0.,-1.),dir:n};let mut h=HitRecord::new();
 assert!(p.intersect(&ray,1e-8,&mut h));assert!(h.normal.z<0.);
 println!("PLANE_BACK_NORMAL z={:.1}",h.normal.z);
 let mut scene=Scene::new();scene.add(Object::new(p).material(mat));
 scene.add(Light::Directional(white,n));
 let cam=Camera::look_at(glm::vec3(0.,0.,1.),glm::vec3(0.,0.,0.),glm::vec3(0.,1.,0.),1e-6);
 let img=Renderer::new(&scene,cam).width(1).height(1).num_samples(1).render();
 let px=img.get_pixel(0,0).0;assert_eq!(px,[0,0,0]);println!("CURRENT_TRANSMITTED_PIXEL {:?}",px);
 let mut normal_scene=Scene::new();normal_scene.add(Object::new(plane(n,0.)).material(Material::diffuse(white)));
 normal_scene.add(Light::Directional(white,-n));
 let front=Renderer::new(&normal_scene,cam).width(1).height(1).render();assert!(front.get_pixel(0,0).0[0]>0);
 println!("FRONT_DIFFUSE_PIXEL {:?}",front.get_pixel(0,0).0);
 println!("RPT_TRANSMISSION_BASELINE_OK");
}
