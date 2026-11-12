use rand::{SeedableRng, rngs::StdRng};
use rpt::{glm, Material};
fn density_h(h: glm::DVec3, m: f64) -> f64 {
 let c = h.z.abs();
 if c < 1e-12 { return 0.0; }
 (-(1.0-c*c)/(m*m*c*c)).exp()/(std::f64::consts::PI*m*m*c.powi(3))
}
fn main() {
 let mat=Material::clear(1.5,0.3); let n=glm::vec3(0.,0.,1.); let wo=n;
 let mut rng=StdRng::seed_from_u64(19);
 for _ in 0..100 {
  if let Some((wi,pdf))=mat.sample_f(&n,&wo,&mut rng) {
   if wi.z >= -0.2 {continue;}
   let hr=(wi+wo).normalize(); let ht=(wi*1.5+wo).normalize();
   let f=0.04+0.96*0.2;
   let reflection=f*density_h(hr,0.3)/(4.*hr.dot(&wo).abs());
   let transmission=(1.-f)*density_h(ht,0.3)*2.25*ht.dot(&wi).abs()
    /(1.5*ht.dot(&wi)+ht.dot(&wo)).powi(2);
   let reference=reflection+transmission;
   assert!(pdf.is_finite() && reference.is_finite() && pdf>0.);
   let bsdf=mat.bsdf(&n,&wo,&wi); assert!(bsdf.x.is_finite() && bsdf.x>0.);
   println!("SAMPLING_BASELINE_OK pdf={pdf:.12} reference={reference:.12}");
   if std::env::args().any(|s|s=="--repro") { assert!((pdf-reference).abs()<1e-8,"transmission solid-angle PDF mismatch"); }
   return;
  }
 }
 panic!("no finite transmission sample");
}
