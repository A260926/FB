#ifndef MINPT_GRIN_H
#define MINPT_GRIN_H
#include "minpt_scene.h"
// Graded-index (GRIN) layer support for minpt. The medium index varies only
// with world y: n=n0 for y<y0, n=n0*exp(k*(y-y0)) for y0<=y<=y1, and
// n=n0*exp(k*(y1-y0)) for y>y1. The profile is continuous, so rays bend
// smoothly (no boundary reflection, absorption, or scattering) following
//   dx/ds = u,  du/ds = (grad(n) - u*dot(u,grad(n)))/n,  u(0)=normalize(R.d)
// where s is the true arc length from the ray origin.
struct GrinConfig{double y0,y1,n0,k,maxArc;};
struct GrinHit{H hit; double arc; V tangent; int object,face;};
// Piecewise analytic trajectory of a ray through the GRIN profile: straight
// segments in the uniform regions and exponential-layer segments with
// n(s)=q*cosh(k*(s-s_turn)) (horizontal optical momentum n*(u.x,u.z)
// conserved). At most one curved segment can occur per ray.
struct GrinPath{
 struct Seg{I kind; double s0,s1; V p0,u0; double nA,uh,k;};
 GrinConfig cfg{}; R ray{}; V u0; vector<Seg> segs;
 V point(double s)const;    // position at arc length s (clamped to [0,maxArc])
 V tangent(double s)const;  // unit propagation direction at arc length s
 double index(double s)const;// medium index at arc length s
};
// Medium index of the layered profile at world height y.
double grinIndex(const GrinConfig& cfg,double y);
// Validates cfg/ray (throws std::invalid_argument) and builds the trajectory.
GrinPath makeGrinPath(const GrinConfig& cfg,const R& r);
// Nearest surface intersection along the curved trajectory with
// arc in [arcMin, cfg.maxArc] (both ends inclusive). Ties are broken by the
// original object index, then by triangle order inside the object.
op<GrinHit> intersectGrinPath(Scene& sc,const GrinPath& path,F arcMin=Eps);
op<GrinHit> intersectGrin(Scene& sc,const R& r,const GrinConfig& cfg,F arcMin=Eps);
// Transport loop of the original renderer reorganized for curved segments:
// original camera/BSDF sampling, no explicit light sampling or MIS, emitters
// accumulate at full weight, per-segment radiance factor (n_start/n_end)^2.
V traceGrin(Scene& sc,Rng& rn,V pixelUV,const GrinConfig& cfg,I maxBounces);
// Read-only query workload counters of the last intersectGrin* calls.
struct GrinWork{unsigned long long boundsTests,triangleTests;};
GrinWork grinWork();
void grinWorkReset();
#endif
