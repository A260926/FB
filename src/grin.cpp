#include "minpt_scene.h"
#include "minpt_grin.h"
#include <stdexcept>
#include <cmath>

// ---------------------------------------------------------------------------
// Medium profile
// ---------------------------------------------------------------------------
double grinIndex(const GrinConfig& c,double y){
 if(y<c.y0) return c.n0;
 if(y>c.y1) return c.n0*exp(c.k*(c.y1-c.y0));
 return c.n0*exp(c.k*(y-c.y0));}

static void validateGrin(const GrinConfig& c,const R& r){
 if(!isfinite(c.y0)||!isfinite(c.y1)||!isfinite(c.n0)||!isfinite(c.k)||
   !isfinite(c.maxArc)) throw invalid_argument("grin: config not finite");
 if(!(c.y1>c.y0)) throw invalid_argument("grin: require y1>y0");
 if(!(c.maxArc>0)) throw invalid_argument("grin: require maxArc>0");
 if(fabs(c.k)>2) throw invalid_argument("grin: require |k|<=2");
 double n1=c.n0*exp(c.k*(c.y1-c.y0));
 if(min(c.n0,n1)<0.25||max(c.n0,n1)>4)
  throw invalid_argument("grin: layer index outside [0.25,4]");
 if(!isfinite(r.o.x)||!isfinite(r.o.y)||!isfinite(r.o.z)||
   !isfinite(r.d.x)||!isfinite(r.d.y)||!isfinite(r.d.z))
  throw invalid_argument("grin: ray not finite");
 if(dot(r.d,r.d)==0) throw invalid_argument("grin: zero direction");}

// ---------------------------------------------------------------------------
// Analytic trajectory
//
// Inside the exponential layer the horizontal optical momentum
// p=(n*u.x,n*u.z) is conserved, so with uh=|(u.x,u.z)|, q=nA*uh and
// phi0=artanh(u.y) the trajectory at local arc length ds is
//   n(ds)   = nA*G(ds),   G(ds)=cosh(k*ds)+u.y*sinh(k*ds)=uh*cosh(phi0+k*ds)
//   y(ds)   = y0' + ln G(ds)/k
//   u.y(ds) = tanh(phi0+k*ds),  (u.x,u.z)(ds) = (u.x,u.z)/G(ds)
//   (x,z)   = (x0,z0) + (u.x,u.z)/uh * [gd(phi0+k*ds)-gd(phi0)]/k
// with gd the Gudermannian. All forms below are written so that the k->0
// (uniform) limit is evaluated without catastrophic cancellation.
// ---------------------------------------------------------------------------
static F artanh(F x){return 0.5*(log1p(x)-log1p(-x));}
static F arcosh(F x){return x<=1?0:log(x+sqrt(x-1)*sqrt(x+1));}

static V segPoint(const GrinPath::Seg& sg,F ds){
 if(sg.kind==0) return sg.p0+sg.u0*ds;
 F x=sg.k*ds, uy0=sg.u0.y, uh=sg.uh;
 F gm1=0.5*((1+uy0)*expm1(x)+(1-uy0)*expm1(-x));   // G-1, accurate for small x
 F y=sg.p0.y+log1p(gm1)/sg.k;
 F e0=sqrt((1+uy0)/(1-uy0));                        // e^phi0
 F dg=2*atan2(e0*expm1(x),1+e0*e0*exp(x));          // gd(phi0+x)-gd(phi0)
 F h=dg/sg.k;
 return V(sg.p0.x+sg.u0.x/uh*h,y,sg.p0.z+sg.u0.z/uh*h);}

static V segTangent(const GrinPath::Seg& sg,F ds){
 if(sg.kind==0) return sg.u0;
 F x=sg.k*ds, uy0=sg.u0.y;
 F g=0.5*((1+uy0)*exp(x)+(1-uy0)*exp(-x));           // G, no cancellation
 F t=tanh(x), uy=(uy0+t)/(1+uy0*t);                  // tanh(phi0+x)
 return V(sg.u0.x/g,uy,sg.u0.z/g);}

// ---------------------------------------------------------------------------
// Trajectory construction: at most straight -> curved -> straight.
// ---------------------------------------------------------------------------
static bool inLayer(const GrinConfig& c,F y,F uy){
 bool a=y>c.y0||(y==c.y0&&(uy>0||(uy==0&&c.k>0)));
 bool b=y<c.y1||(y==c.y1&&(uy<0||(uy==0&&c.k<0)));
 return a&&b;}

GrinPath makeGrinPath(const GrinConfig& c,const R& r){
 validateGrin(c,r);
 GrinPath P; P.cfg=c; P.ray=r; P.u0=norm(r.d);
 F n1=c.n0*exp(c.k*(c.y1-c.y0)), nlo=min(c.n0,n1), nhi=max(c.n0,n1);
 V p=r.o, u=P.u0; F s=0;
 for(I it=0; it<8 && s<c.maxArc; it++){
  F uh=hypot(u.x,u.z);
  if(c.k==0||uh<1e-13||!inLayer(c,p.y,u.y)){
   // uniform region: straight segment, optionally to a layer boundary
   F ds=c.maxArc-s, yB=0; bool cross=0;
   if(c.k!=0&&uh>=1e-13){
    if(p.y<c.y0&&u.y>0){cross=1; yB=c.y0;}
    else if(p.y>c.y1&&u.y<0){cross=1; yB=c.y1;}}
   F dsB=cross?(yB-p.y)/u.y:ds;
   if(!(dsB>0)||dsB>=ds){P.segs.push_back({0,s,s+ds,p,u,grinIndex(c,p.y),uh,c.k}); break;}
   P.segs.push_back({0,s,s+dsB,p,u,grinIndex(c,p.y),uh,c.k});
   s+=dsB; p=p+u*dsB; p.y=yB; continue;}
  // inside the exponential layer: curved segment to the next boundary
  F nA=c.n0*exp(c.k*(p.y-c.y0));
  F uy0=u.y, phi0=artanh(uy0), sk=c.k>0?1:-1, ak=fabs(c.k);
  F thi=arcosh((nhi/nA)/uh);
  F shi=(thi-sk*phi0)/ak;                 // arc to the high-index boundary
  F slo=Inf, q=nA*uh;
  if(q<nlo&&phi0*c.k<0){                  // low-index boundary reachable?
   F tlo=arcosh((nlo/nA)/uh);
   slo=(fabs(phi0)-tlo)/ak;}
  F send=min(min(shi,slo),c.maxArc-s);
  if(!(send>0)){P.segs.push_back({0,s,c.maxArc,p,u,nA,uh,c.k}); break;}
  P.segs.push_back({1,s,s+send,p,u,nA,uh,c.k});
  s+=send; if(s>=c.maxArc) break;
  p=segPoint(P.segs.back(),send); u=segTangent(P.segs.back(),send);
  p.y=(send==slo)?(c.k>0?c.y0:c.y1):(c.k>0?c.y1:c.y0);}
 return P;}

static const GrinPath::Seg& segAt(const GrinPath& P,F s){
 for(auto& sg:P.segs) if(s<=sg.s1) return sg;
 return P.segs.back();}
V GrinPath::point(double s)const{
 s=clamp(s,0.0,cfg.maxArc); auto& sg=segAt(*this,s); return segPoint(sg,s-sg.s0);}
V GrinPath::tangent(double s)const{
 s=clamp(s,0.0,cfg.maxArc); auto& sg=segAt(*this,s); return segTangent(sg,s-sg.s0);}
double GrinPath::index(double s)const{return grinIndex(cfg,point(s).y);}

// ---------------------------------------------------------------------------
// Query workload counters (real traversals of the original BVH).
// ---------------------------------------------------------------------------
static atomic<unsigned long long> g_bt{0}, g_tt{0};
GrinWork grinWork(){return{g_bt.load(memory_order_relaxed),
 g_tt.load(memory_order_relaxed)};}
void grinWorkReset(){g_bt.store(0,memory_order_relaxed);
 g_tt.store(0,memory_order_relaxed);}

// Conservative AABB of one arc segment from its true coordinate extrema.
static B segBox(const GrinPath::Seg& sg){
 V a=segPoint(sg,0), b=segPoint(sg,sg.s1-sg.s0);
 B bx=merge(merge(B{},a),b);
 if(sg.kind==1){F st=-artanh(sg.u0.y)/sg.k;          // turning point arc
  if(st>0&&st<sg.s1-sg.s0) bx=merge(bx,segPoint(sg,st));}
 F m=0; for(I i=0;i<3;i++) m=max(m,max(fabs(bx.mi[i]),fabs(bx.ma[i])));
 F e=1e-9*(1+m);
 bx.mi=bx.mi-V(e); bx.ma=bx.ma+V(e); return bx;}

static bool overlap(const B& a,const B& b){
 return a.mi.x<=b.ma.x&&a.ma.x>=b.mi.x&&a.mi.y<=b.ma.y&&a.ma.y>=b.mi.y&&
  a.mi.z<=b.ma.z&&a.ma.z>=b.mi.z;}

// Illinois/regula-falsi safeguarded root solve on a bracket with f(a)f(b)<0.
template<class Fun> static F rootIso(Fun f,F a,F b,F fa,F fb,F xtol){
 for(I it=0;it<200;it++){
  F c=(a*fb-b*fa)/(fb-fa), fc=f(c);
  if(fc==0||fabs(b-a)<=xtol) return c;
  if(fb*fc<0){a=b; fa=fb;} else fa*=0.5;
  b=c; fb=fc;}
 return (a+b)*0.5;}

// Roots of dot(x(ds)-p1,N)=0 on a curved segment, ds in [lo,hi]. f has at
// most one critical point (f'=0) in the segment, hence at most two roots.
static void curvedRoots(const GrinPath::Seg& sg,V p1,V N,F lo,F hi,
 vector<F>& out){
 F uy0=sg.u0.y;
 auto f=[&](F ds){return dot(segPoint(sg,ds)-p1,N);};
 F A0=N.x*sg.u0.x+N.z*sg.u0.z;
 if(N.y==0&&A0==0) return; // f constant: coplanar (ignored) or no crossing
 F ftol=1e-9*(1+fabs(dot(p1,N)));
 F xtol=5e-13*(1+hi);
 vector<tuple<F,F,F,F>> br; // [a,b,fa,fb] monotonic brackets
 F dsc=0; bool crit=0;
 if(N.y!=0){
  F a=N.y*(1+uy0), b=2*A0, c=-N.y*(1-uy0);
  F D=sqrt(b*b-4*a*c), sgn=N.y>0?1:-1, w;
  if(A0*N.y>=0) w=2*N.y*(1-uy0)/(b+sgn*D);
  else w=(-b+sgn*D)/(2*a);
  dsc=log(w)/sg.k; crit=dsc>lo&&dsc<hi;}
 F flo=f(lo), fhi=f(hi);
 if(crit){F fc=f(dsc);
  if(fabs(fc)<=ftol){out.push_back(dsc); return;}  // tangent contact
  br.emplace_back(lo,dsc,flo,fc); br.emplace_back(dsc,hi,fc,fhi);}
 else br.emplace_back(lo,hi,flo,fhi);
 for(auto&[a,b,fa,fb]:br){
  if(fabs(fa)<=ftol) out.push_back(a);
  else if(fabs(fb)<=ftol) out.push_back(b);
  else if(fa*fb<0) out.push_back(rootIso(f,a,b,fa,fb,xtol));}
 sort(out.begin(),out.end());
 out.erase(unique(out.begin(),out.end(),[&](F a,F b){
  return fabs(a-b)<=1e-9*(1+hi);}),out.end());}

// ---------------------------------------------------------------------------
// Intersection of the curved trajectory with the original BVH.
// ---------------------------------------------------------------------------
struct GrinCand{F arc,u,v; I ti;};

op<GrinHit> intersectGrinPath(Scene& sc,const GrinPath& path,F arcMin){
 const GrinConfig& c=path.cfg;
 if(!isfinite(arcMin)||arcMin<0||!(arcMin<c.maxArc))
  throw invalid_argument("grin: require 0<=arcMin<maxArc");
 if(sc.trs.empty()||sc.ns.empty()) return{};
 op<GrinCand> best; F arcTol=1e-9*max(1.0,c.maxArc);
 auto accept=[&](const GrinCand& cd){
  if(cd.arc<arcMin-arcTol||cd.arc>c.maxArc+arcTol) return;
  if(!best||cd.arc<best->arc-arcTol||
    (cd.arc<=best->arc+arcTol&&
     tie(sc.trs[cd.ti].oi,sc.trs[cd.ti].fi)<tie(sc.trs[best->ti].oi,sc.trs[best->ti].fi)))
   best=cd;};
 for(auto& sg:path.segs){
  F lo=max(sg.s0,arcMin), hi=min(sg.s1,c.maxArc);
  if(lo>hi) continue;
  B box=segBox(sg); I st[99]{}, si=0;
  while(si>=0){auto& nd=sc.ns[st[si--]];
   g_bt.fetch_add(1,memory_order_relaxed);
   if(!overlap(nd.b,box)) continue;
   if(!nd.leaf){st[++si]=nd.c1; st[++si]=nd.c2; continue;}
   for(I i=nd.s;i<nd.e;i++){I ti=sc.ti[i]; auto& tr=sc.trs[ti];
    g_tt.fetch_add(1,memory_order_relaxed);
    V cr=cross(tr.e1,tr.e2); F a2=dot(cr,cr);
    if(a2<1e-26) continue;                        // zero-area face ignored
    if(sg.kind==0){                               // straight arc: exact ray test
     if(auto h=tr.isect({sg.p0,sg.u0},lo-sg.s0,hi-sg.s0))
      accept({sg.s0+h->t,h->u,h->v,ti});
     continue;}
    F a2i=1/a2; V N=cr*sqrt(a2i);
    vector<F> roots; curvedRoots(sg,tr.p1,N,lo-sg.s0,hi-sg.s0,roots);
    for(F ds:roots){V P=segPoint(sg,ds);
     F u=dot(cross(P-tr.p1,tr.e2),cr)*a2i;
     F v=dot(cross(tr.e1,P-tr.p1),cr)*a2i;
     if(u>=-1e-7&&v>=-1e-7&&u+v<=1+1e-7) accept({sg.s0+ds,u,v,ti});}}}}
 if(!best) return{};
 auto& tr=sc.trs[best->ti]; Obj& o=sc.os[tr.oi];
 F u=best->u, v=best->v;
 Ind& i0=o.fs[tr.fi],&j0=o.fs[tr.fi+1],&k0=o.fs[tr.fi+2];
 V n=i0.n<0?tr.n:norm(intp(sc.G.ns[i0.n],sc.G.ns[j0.n],sc.G.ns[k0.n],u,v));
 V t=i0.t<0?0:intp(sc.G.ts[i0.t],sc.G.ts[j0.t],sc.G.ts[k0.t],u,v);
 V P=path.point(best->arc), tg=path.tangent(best->arc);
 return GrinHit{{{P,n,t},&o},best->arc,tg,tr.oi,tr.fi};}

op<GrinHit> intersectGrin(Scene& sc,const R& r,const GrinConfig& c,F arcMin){
 return intersectGrinPath(sc,makeGrinPath(c,r),arcMin);}

// ---------------------------------------------------------------------------
// Transport: original loop, curved propagation, no NEE/MIS, L/n^2 scaling.
// ---------------------------------------------------------------------------
V traceGrin(Scene& sc,Rng& rn,V uv,const GrinConfig& c,I mb){
 V L,T(1),wi=uv; I ty=T::E; Obj* o=&sc.E; Surf sp;
 for(I b=0;b<mb;b++){
  auto s=o->samp(rn,ty,sp,wi); if(!s){break;} auto[t,r,w,pcs]=*s; T=T/pcs; T=T*w;
  GrinPath path=makeGrinPath(c,r);
  F nS=grinIndex(c,r.o.y);
  auto hi=intersectGrinPath(sc,path,Eps);
  if(!hi){V pe=path.point(c.maxArc), ue=path.tangent(c.maxArc);
   T=T*sq(nS/grinIndex(c,pe.y));
   if(Obj* el=sc.envL()){L=L+T*el->ev(T::EnvL,Surf{},V(0),-ue);} break;}
  auto[spH,oH]=hi->hit;
  T=T*sq(nS/grinIndex(c,spH.p.y));
  if(oH->t&T::L){L=L+T*oH->ev(oH->t&T::L,spH,V(0),-hi->tangent);}
  if(b>3){F q=max(.2,1-T.m()); T=T/(1-q); if(rn.u()<q) break;}
  wi=-hi->tangent; o=oH; sp=spH; ty=o->t&~T::L;}
 return L;}
