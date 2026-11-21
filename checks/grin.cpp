// GRIN checks: trajectory, intersection and transport groups.
// Registered through task_support/check.h (no main here).
#include "minpt_scene.h"
#include "minpt_grin.h"
#include "task_support/check.h"
#include <cmath>
#include <array>
#include <vector>

static F am(V v){return max({fabs(v.x),fabs(v.y),fabs(v.z)});}
static bool approx(F a,F b,F tol){return fabs(a-b)<=tol;}
static bool approxV(V a,V b,F tol){return am(a-b)<=tol;}
static F artanh_(F x){return 0.5*(log1p(x)-log1p(-x));}
template<class F_> static bool throwsIA(F_ f){
 try{f();}catch(const invalid_argument&){return 1;}catch(...){}return 0;}

// ---------------------------------------------------------------------------
// Independent reference integrator for the ray ODE (trajectory group).
// Classical fixed-step RK4 on dx/ds=u, du/ds=(grad n-u*dot(u,grad n))/n with
// the piecewise-exponential n(y) written out inline below. Convergence basis:
// the global error of RK4 is O(h^4); layer boundaries (where d(grad n)/dy is
// discontinuous) are located by recursive step halving so the fourth-order
// rate is preserved. Every use halves h once and requires the change to stay
// below 1e-9 (the observed gap contracts by ~16x as expected) before the
// analytic trajectory is allowed to agree within 1e-7.
// ---------------------------------------------------------------------------
static F refN(const GrinConfig&c,F y){
 return y<c.y0?c.n0:y>c.y1?c.n0*exp(c.k*(c.y1-c.y0)):c.n0*exp(c.k*(y-c.y0));}
static V refDU(const GrinConfig&c,const V&p,const V&u){
 F n=refN(c,p.y), g=(p.y>=c.y0&&p.y<=c.y1)?c.k*n:0;
 return (V(0,g,0)-u*(u.y*g))/n;}
static void refRK4(const GrinConfig&c,V&p,V&u,F h){
 V k1p=u, k1u=refDU(c,p,u);
 V k2p=u+k1u*(h/2), k2u=refDU(c,p+k1p*(h/2),u+k1u*(h/2));
 V k3p=u+k2u*(h/2), k3u=refDU(c,p+k2p*(h/2),u+k2u*(h/2));
 V k4p=u+k3u*h,   k4u=refDU(c,p+k3p*h,u+k3u*h);
 p=p+(k1p+k2p*2+k3p*2+k4p)*(h/6); u=u+(k1u+k2u*2+k3u*2+k4u)*(h/6);}
static void refStepEv(const GrinConfig&c,V&p,V&u,F h,I depth){
 V p0=p,u0=u,pp=p,uu=u; refRK4(c,pp,uu,h);
 bool cross=0;
 if(depth<40) for(F yB:{c.y0,c.y1}) if((p0.y-yB)*(pp.y-yB)<0) cross=1;
 if(cross){p=p0;u=u0; refStepEv(c,p,u,h/2,depth+1); refStepEv(c,p,u,h/2,depth+1);}
 else{p=pp;u=uu;}}
static void refIntegrate(const GrinConfig&c,V p,V u,F sMax,F h,V&po,V&uo){
 F s=0; while(s<sMax-1e-15){F hh=min(h,sMax-s); refStepEv(c,p,u,hh,0); s+=hh;}
 po=p; uo=u;}
// Checks analytic path against the reference at arc smax (see basis above).
static bool rk4Agree(const GrinConfig&c,V o,V d,const GrinPath&P,F smax){
 V u=norm(d),pa,ua,pb,ub;
 refIntegrate(c,o,u,smax,smax/4096,pa,ua);
 refIntegrate(c,o,u,smax,smax/8192,pb,ub);
 if(am(pa-pb)>1e-9||am(ua-ub)>1e-9) return 0;   // reference not converged
 return am(P.point(smax)-pb)<1e-7&&am(P.tangent(smax)-ub)<1e-7;}

// ---------------------------------------------------------------------------
// Independent straight-line triangle reference (intersection group): a full
// scene scan with its own Möller–Trumbore routine, no BVH, no GRIN code.
// ---------------------------------------------------------------------------
struct RefHit{F t,u,v; I oi,fi;};
static op<RefHit> refIsect(const Scene& sc,V o,V d,F tmin,F tmax){
 op<RefHit> best;
 for(I oi=0;oi<I(sc.os.size());oi++){auto& ob=sc.os[oi];
  for(size_t fi=0;fi<ob.fs.size();fi+=3){
   V a=sc.G.ps[ob.fs[fi].p], b=sc.G.ps[ob.fs[fi+1].p], c=sc.G.ps[ob.fs[fi+2].p];
   V e1=b-a,e2=c-a,cr=cross(e1,e2);
   if(dot(cr,cr)<1e-26) continue;
   V pv=cross(d,e2),tv=o-a,q=cross(tv,e1);
   F det=dot(e1,pv); if(fabs(det)<1e-12) continue;
   F u=dot(tv,pv)/det, v=dot(d,q)/det, t=dot(e2,q)/det;
   if(u<-1e-9||v<-1e-9||u+v>1+1e-9||t<tmin||t>tmax) continue;
   if(!best||t<best->t-1e-12||
     (t<best->t+1e-12&&make_tuple(oi,I(fi))<make_tuple(best->oi,best->fi)))
    best=RefHit{t,u,v,oi,I(fi)};}}
 return best;}

// ---------------------------------------------------------------------------
// Scene construction helpers.
// ---------------------------------------------------------------------------
static I addMat(Scene& sc,I t,V ke=V(0),V kd=V(0),F ni=1.5){
 sc.ms.push_back(Mat{t,kd,V(0),ke,nullptr,ni,0,0,0,0}); return I(sc.ms.size())-1;}
static I addObj(Scene& sc,I t,I m){
 sc.os.push_back(Obj{t,&sc.ms[m]}); return I(sc.os.size())-1;}
static void addTri(Scene& sc,I oi,array<V,3> p){
 I a=I(sc.G.ps.size()); sc.G.ps.insert(sc.G.ps.end(),{p[0],p[1],p[2]});
 auto& fs=sc.os[oi].fs;
 fs.insert(fs.end(),{Ind{a,-1,-1},Ind{a+1,-1,-1},Ind{a+2,-1,-1}});}
static void addTriN(Scene& sc,I oi,array<V,3> p,array<V,3> n,array<V,3> t){
 I a=I(sc.G.ps.size()), b=I(sc.G.ts.size()), c=I(sc.G.ns.size());
 for(I i=0;i<3;i++){sc.G.ps.push_back(p[i]); sc.G.ns.push_back(n[i]);
  sc.G.ts.push_back(t[i]);}
 auto& fs=sc.os[oi].fs;
 fs.insert(fs.end(),{Ind{a,b,c},Ind{a+1,b+1,c+1},Ind{a+2,b+2,c+2}});}
static array<V,3> triAt(V C,V N,F r){auto[u,v]=odr(N);
 return{C+u*r,C+v*r,C-u*r-v*r};}
static void pinhole(Scene& sc,V pos,V lookat){
 sc.E.initE("",pos,lookat,V(0,1,0),60,1,0,1,1);}
static F bisectY(const GrinPath&P,F y,F lo,F hi){
 bool up=P.point(lo).y<y;
 for(I i=0;i<200;i++){F m=(lo+hi)/2; ((P.point(m).y<y)==up?lo:hi)=m;}
 return (lo+hi)/2;}

// ===========================================================================
// trajectory group
// ===========================================================================
static int t_constant(){
 GrinConfig c{0.3,1.7,2.5,0.0,10.0};
 R r{V(1,-2,0.5),V(0.3,1,-0.2)};            // non-unit direction
 auto P=makeGrinPath(c,r); V u=norm(r.d);
 for(F s:{0.0,0.1,1.0,5.0,9.999,10.0}){
  TASK_CHECK(approxV(P.point(s),r.o+u*s,1e-12));
  TASK_CHECK(approxV(P.tangent(s),u,1e-12));
  TASK_CHECK(approx(P.index(s),2.5,1e-12));}
 GrinConfig c2{-1.0,2.0,0.5,0.0,3.0};       // uniform, different n0
 auto P2=makeGrinPath(c2,r);
 TASK_CHECK(approxV(P2.point(2.5),r.o+u*2.5,1e-12));
 TASK_CHECK(approx(P2.index(2.5),0.5,1e-12));
 return 0;}

static int t_boundaries(){
 GrinConfig c{0.0,1.0,1.0,1.0,12.0};
 V o(0.2,-2,0.1), d(0.1,0.9,-0.2), u=norm(d);
 auto P=makeGrinPath(c,{o,d});
 F s0=(c.y0-o.y)/u.y;                        // entry arc at y0
 TASK_CHECK(approx(P.point(s0).y,c.y0,1e-9));
 TASK_CHECK(approx(P.index(s0),c.n0,1e-9));
 F s1=bisectY(P,c.y1,s0,c.maxArc);           // exit arc at y1
 TASK_CHECK(approx(P.point(s1).y,c.y1,1e-9));
 TASK_CHECK(approx(P.index(s1),exp(1.0),1e-9));
 TASK_CHECK(P.tangent(s1).y>0);
 TASK_CHECK(P.point(c.maxArc).y>c.y1);       // leaves upward, never returns
 for(F s:{0.5,2.0,2.5,3.0,4.5,8.0,11.9}) TASK_CHECK(rk4Agree(c,o,d,P,s));
 F q0=P.index(0)*u.x, q2=P.index(0)*u.z;     // horizontal momentum conserved
 for(F s=0.05;s<12;s+=0.61){
  TASK_CHECK(approx(P.index(s)*P.tangent(s).x,q0,1e-9));
  TASK_CHECK(approx(P.index(s)*P.tangent(s).z,q2,1e-9));
  TASK_CHECK(approx(dot(P.tangent(s),P.tangent(s)),1.0,1e-12));}
 return 0;}

static int t_in_layer_start(){
 GrinConfig c{0.0,1.0,2.0,-0.7,12.0};        // negative k, n in [~0.99,2]
 V o(0.3,0.5,-0.2), d(0.4,-0.35,0.6), u=norm(d);
 auto P=makeGrinPath(c,{o,d});
 TASK_CHECK(approx(P.index(0),2.0*exp(-0.35),1e-12));
 for(F s:{0.3,1.0,2.0,4.0,7.0,11.5}) TASK_CHECK(rk4Agree(c,o,d,P,s));
 F q0=P.index(0)*u.x, q2=P.index(0)*u.z;
 for(F s=0.05;s<12;s+=0.83){
  TASK_CHECK(approx(P.index(s)*P.tangent(s).x,q0,1e-9));
  TASK_CHECK(approx(P.index(s)*P.tangent(s).z,q2,1e-9));}
 return 0;}

static int t_horizontal_vertical(){
 GrinConfig c{0.0,1.0,1.0,1.0,6.0};
 auto P1=makeGrinPath(c,{V(0,0.5,0),V(1,0,0)});   // horizontal, in layer
 TASK_CHECK(P1.tangent(0.5).y>0);                 // bends toward higher n
 TASK_CHECK(approx(P1.point(0.5).y,0.5+log(cosh(0.5)),1e-12));
 TASK_CHECK(approx(P1.point(1.0).x,atan(sinh(1.0)),1e-12));
 TASK_CHECK(rk4Agree(c,V(0,0.5,0),V(1,0,0),P1,1.5));
 auto P2=makeGrinPath(c,{V(0,0.5,0),V(0,1,0)});   // vertical: no bending
 TASK_CHECK(approxV(P2.point(2.0),V(0,2.5,0),1e-12));
 TASK_CHECK(approxV(P2.tangent(2.0),V(0,1,0),1e-12));
 auto P3=makeGrinPath(c,{V(0,-1,0),V(1,0,0)});    // horizontal below layer
 TASK_CHECK(approxV(P3.point(3.0),V(3,-1,0),1e-12));
 auto P4=makeGrinPath(c,{V(0,0.5,0),V(0,-1,0)});  // vertical down
 TASK_CHECK(approxV(P4.point(1.5),V(0,-1.0,0),1e-12));
 TASK_CHECK(approxV(P4.tangent(1.5),V(0,-1,0),1e-12));
 return 0;}

static int t_small_k(){
 GrinConfig c{0.0,2.0,1.5,1e-6,8.0};
 V o(0.1,0.7,0.2), d(0.3,0.5,-0.8), u=norm(d);
 auto P=makeGrinPath(c,{o,d});
 GrinConfig c0{0.0,2.0,1.5,0.0,8.0};
 auto P0=makeGrinPath(c0,{o,d});
 for(F s:{1.0,4.0,8.0}){
  TASK_CHECK(am(P.point(s)-P0.point(s))<1e-6*s*s); // |delta| <= k*s^2
  TASK_CHECK(am(P.tangent(s)-P0.tangent(s))<1e-6*s);}
 for(F s:{2.0,5.0,8.0}) TASK_CHECK(rk4Agree(c,o,d,P,s));
 return 0;}

static int t_turning(){
 GrinConfig c{0.0,1.0986122886681098,1.0,1.0,4.0}; // n_hi = 3
 V o(0,1.0,0), d=norm(V(0,-0.2,-1));
 F uh=hypot(d.x,d.z), q=exp(1.0)*uh;      // nA*uh at y=1 (nA=e)
 F yt=log(q), st=-artanh_(d.y);           // turning height and arc (k=1)
 auto P=makeGrinPath(c,{o,d});
 TASK_CHECK(q>1.0&&q<3.0);                // turning point inside the layer
 TASK_CHECK(approx(P.point(st).y,yt,1e-9));
 TASK_CHECK(approx(P.tangent(st).y,0.0,1e-9));
 TASK_CHECK(P.tangent(st-0.05).y<0);
 TASK_CHECK(P.tangent(st+0.05).y>0);      // descends, then turns upward
 F ym=1e9; for(F s=0.0;s<2.0;s+=0.001) ym=min(ym,P.point(s).y);
 TASK_CHECK(approx(ym,yt,1e-4));
 F sx=bisectY(P,c.y1,st,c.maxArc);        // exits at the high-index boundary
 TASK_CHECK(approx(P.index(sx),3.0,1e-9));
 TASK_CHECK(P.tangent(sx).y>0);
 TASK_CHECK(P.point(c.maxArc).y>c.y1);    // not treated as out of bounds
 TASK_CHECK(rk4Agree(c,o,d,P,0.9));
 TASK_CHECK(rk4Agree(c,o,d,P,2.5));
 return 0;}

static int t_reciprocity(){
 GrinConfig c{0.0,1.0,1.3,-1.1,8.0};
 V o(0.4,0.3,-0.1), d=norm(V(0.5,0.4,0.3));
 auto Pf=makeGrinPath(c,{o,d});
 F sm=3.7;
 V p1=Pf.point(sm), u1=Pf.tangent(sm);
 auto Pr=makeGrinPath(c,{p1,-u1});        // reverse ray retraces the path
 for(F s:{0.0,0.3,1.1,2.9,3.7}){
  TASK_CHECK(approxV(Pr.point(s),Pf.point(sm-s),1e-9));
  TASK_CHECK(approxV(Pr.tangent(s),-Pf.tangent(sm-s),1e-9));}
 return 0;}

static int t_validation(){
 R ok{V(0),V(0,1,0)}; GrinConfig okc{0,1,1,1,4};
 TASK_CHECK(throwsIA([&]{makeGrinPath({0,0,1,1,4},ok);}));      // y1<=y0
 TASK_CHECK(throwsIA([&]{makeGrinPath({0,1,1,2.5,4},ok);}));    // |k|>2
 TASK_CHECK(throwsIA([&]{makeGrinPath({0,1,0.2,1,4},ok);}));    // n<0.25
 TASK_CHECK(throwsIA([&]{makeGrinPath({0,1,1.0,2.0,4},ok);}));  // n1=e^2>4
 TASK_CHECK(throwsIA([&]{makeGrinPath({0,1,5.0,0,4},ok);}));    // n0>4
 TASK_CHECK(throwsIA([&]{makeGrinPath({0,1,1,1,0},ok);}));      // maxArc=0
 TASK_CHECK(throwsIA([&]{makeGrinPath({0,1,1,1,-1},ok);}));     // maxArc<0
 TASK_CHECK(throwsIA([&]{makeGrinPath({0,1,1,NAN,4},ok);}));
 TASK_CHECK(throwsIA([&]{makeGrinPath({0,1,1,1,INFINITY},ok);}));
 TASK_CHECK(throwsIA([&]{makeGrinPath({NAN,1,1,1,4},ok);}));
 TASK_CHECK(throwsIA([&]{makeGrinPath(okc,{V(0),V(0,0,0)});})); // zero dir
 TASK_CHECK(throwsIA([&]{makeGrinPath(okc,{V(NAN),V(0,1,0)});}));
 TASK_CHECK(throwsIA([&]{makeGrinPath(okc,{V(0),V(INFINITY,0,0)});}));
 makeGrinPath({0,1,0.25,2.0,4},ok);                 // |k|=2, n=0.25 ok
 makeGrinPath({0,0.6931471805599453,4.0,-2.0,4},ok);// n1=1.0 ok
 Scene sc; sc.ms.reserve(1);
 I m=addMat(sc,T::D,V(0),V(0.5)); I o=addObj(sc,T::NonS,m);
 addTri(sc,o,triAt(V(0,0,-2),V(0,0,1),1.0)); sc.build();
 TASK_CHECK(throwsIA([&]{intersectGrin(sc,ok,okc,-0.5);}));  // arcMin<0
 TASK_CHECK(throwsIA([&]{intersectGrin(sc,ok,okc,4.0);}));   // arcMin==maxArc
 TASK_CHECK(throwsIA([&]{intersectGrin(sc,ok,okc,5.0);}));   // arcMin>maxArc
 TASK_CHECK(!throwsIA([&]{intersectGrin(sc,ok,okc,0.0);}));
 TASK_CHECK(!throwsIA([&]{intersectGrin(sc,ok,okc,Eps);}));
 return 0;}

// ===========================================================================
// intersection group
// ===========================================================================
static int i_straight_reference(){
 Scene sc; sc.ms.reserve(4);
 I m0=addMat(sc,T::D,V(0),V(0.7)); I m1=addMat(sc,T::D,V(0),V(0.3));
 I o0=addObj(sc,T::NonS,m0), o1=addObj(sc,T::NonS,m1);
 addTriN(sc,o0,{V(-2,-1,-1),V(2,-1,-0.5),V(0,2,-1.5)},
  {norm(V(0,0,1)),norm(V(0.1,0,1)),norm(V(0,0.1,1))},
  {V(0.1,0.2,0),V(0.9,0.2,0),V(0.5,0.8,0)});
 addTriN(sc,o0,{V(-3,0.5,1),V(-1,2,0.5),V(-2,-1,2)},
  {norm(V(1,0,0)),norm(V(1,0.1,0)),norm(V(1,0,0.1))},
  {V(0.2,0.1,0),V(0.7,0.6,0),V(0.1,0.9,0)});
 addTriN(sc,o1,{V(1,-2,0.5),V(3,0,1),V(1,2,0.5)},
  {norm(V(0,0,1)),norm(V(0,0.2,1)),norm(V(0.2,0,1))},
  {V(0.3,0.3,0),V(0.6,0.6,0),V(0.3,0.6,0)});
 addTriN(sc,o1,{V(-0.5,-3,-2),V(1.5,-2.5,-1),V(0.5,-0.5,-3)},
  {norm(V(0,1,0)),norm(V(0.1,1,0)),norm(V(0,1,0.1))},
  {V(0.4,0.4,0),V(0.8,0.4,0),V(0.4,0.8,0)});
 sc.build();
 GrinConfig c{-1.0,2.0,1.7,0.0,20.0};        // uniform index: straight
 mt19937 g(7); uniform_real_distribution<F> U(-3,3);
 I hits=0;
 for(I i=0;i<400;i++){
  V o(U(g),U(g),U(g)), d(U(g),U(g),U(g)); d=norm(d);
  auto gh=intersectGrin(sc,{o,d},c);
  auto rh=refIsect(sc,o,d,Eps,20.0);
  TASK_CHECK(gh.has_value()==rh.has_value());
  if(!gh) continue; hits++;
  TASK_CHECK(approx(gh->arc,rh->t,1e-9));
  TASK_CHECK(approxV(gh->hit.sp.p,o+d*rh->t,1e-9));
  TASK_CHECK(gh->object==rh->oi&&gh->face==rh->fi);
  auto& fs=sc.os[rh->oi].fs;
  V te=intp(sc.G.ts[fs[rh->fi].t],sc.G.ts[fs[rh->fi+1].t],
   sc.G.ts[fs[rh->fi+2].t],rh->u,rh->v);
  TASK_CHECK(approxV(gh->hit.sp.t,te,1e-9));
  V ne=norm(intp(sc.G.ns[fs[rh->fi].n],sc.G.ns[fs[rh->fi+1].n],
   sc.G.ns[fs[rh->fi+2].n],rh->u,rh->v));
  TASK_CHECK(approxV(gh->hit.sp.n,ne,1e-9));}
 TASK_CHECK(hits>20);                         // exercise both hit and miss
 return 0;}

static int i_curved_chord(){
 GrinConfig c{0.0,1.0,1.0,1.3,4.0};
 V o(0,0.5,-2), d=norm(V(0,-0.3,1));
 auto P=makeGrinPath(c,{o,d});
 F ss=0.62; V C=P.point(ss), tg=P.tangent(ss);
 Scene sc; sc.ms.reserve(1);
 I m=addMat(sc,T::D,V(0),V(0.5)); I ob=addObj(sc,T::NonS,m);
 addTri(sc,ob,triAt(C,-tg,0.08)); sc.build();
 V pe=P.point(c.maxArc), chord=norm(pe-o);
 F dc=dot(C-o,chord);                        // chord clearance at C
 TASK_CHECK(sqrt(max(.0,dot(C-o,C-o)-dc*dc))>0.15);
 TASK_CHECK(!sc.isect({o,chord},Eps,sqrt(dot(pe-o,pe-o))*(1-1e-9)));
 GrinConfig c0{0.0,1.0,1.0,0.0,4.0};         // straight query also misses
 TASK_CHECK(!intersectGrin(sc,{o,d},c0));
 auto gh=intersectGrin(sc,{o,d},c);          // curved query hits
 TASK_CHECK(gh.has_value());
 TASK_CHECK(approx(gh->arc,ss,1e-6));
 TASK_CHECK(approxV(gh->hit.sp.p,C,1e-6));
 TASK_CHECK(approxV(gh->tangent,tg,1e-6));
 TASK_CHECK(gh->object==ob&&gh->face==0);
 return 0;}

static int i_two_roots(){
 GrinConfig c{0.0,1.0986122886681098,1.0,1.0,2.0};
 V o(0,0.99,0), d=norm(V(0,-0.2,-1));
 auto P=makeGrinPath(c,{o,d});
 // the path dips from y=0.99 to the turning point y~0.9704 and rises back,
 // crossing the plane y=0.98 twice (down, then up) at different z.
 F s1=bisectY(P,0.98,0.0,0.15), s2=bisectY(P,0.98,0.2,c.maxArc);
 V p1=P.point(s1), p2=P.point(s2);
 TASK_CHECK(approx(p1.y,0.98,1e-9)&&approx(p2.y,0.98,1e-9));
 TASK_CHECK(s2-s1>0.2&&fabs(p1.z-p2.z)>0.2);
 Scene sc; sc.ms.reserve(1);
 I m=addMat(sc,T::D,V(0),V(0.5)); I ob=addObj(sc,T::NonS,m);
 addTri(sc,ob,triAt(p2,V(0,1,0),0.05));      // covers only the 2nd root
 sc.build();
 TASK_CHECK(am(p1-p2)>0.2);
 auto gh=intersectGrin(sc,{o,d},c);          // must skip the out-of-domain
 TASK_CHECK(gh.has_value());                  // first root, report the 2nd
 TASK_CHECK(approx(gh->arc,s2,1e-6));
 TASK_CHECK(approxV(gh->hit.sp.p,p2,1e-6));
 TASK_CHECK(gh->object==ob&&gh->face==0);
 Scene sc2; sc2.ms.reserve(1);
 I m2=addMat(sc2,T::D,V(0),V(0.5)); I ob2=addObj(sc2,T::NonS,m2);
 addTri(sc2,ob2,triAt(V(0,0.98,-0.2),V(0,1,0),1.0)); // covers both roots
 sc2.build();
 auto g1=intersectGrin(sc2,{o,d},c);         // nearest root wins
 TASK_CHECK(g1.has_value()&&approx(g1->arc,s1,1e-6));
 auto g2=intersectGrin(sc2,{o,d},c,0.2);     // arcMin past the 1st root:
 TASK_CHECK(g2.has_value());                  // the 2nd root of the same
 TASK_CHECK(approx(g2->arc,s2,1e-6));         // face must not be missed
 TASK_CHECK(approxV(g2->hit.sp.p,p2,1e-6));
 return 0;}

static int i_non_unit(){
 Scene sc; sc.ms.reserve(1);
 I m=addMat(sc,T::D,V(0),V(0.5)); I ob=addObj(sc,T::NonS,m);
 addTri(sc,ob,{V(-1,-1,-3),V(1,-1,-3),V(0,1,-3)}); sc.build();
 GrinConfig c0{0,1,1,0.0,10.0};
 auto gh=intersectGrin(sc,{V(0.2,0.1,0),V(0,0,-7.5)},c0);
 TASK_CHECK(gh.has_value());
 TASK_CHECK(approx(gh->arc,3.0,1e-9));        // true arc length, not t of d
 TASK_CHECK(approxV(gh->hit.sp.p,V(0.2,0.1,-3),1e-9));
 GrinConfig c{0.0,1.0,1.0,1.3,6.0};
 V o(0.1,0.4,-1), u=norm(V(0.2,-0.3,-0.9));
 auto g1=intersectGrin(sc,{o,u},c);
 auto g2=intersectGrin(sc,{o,u*2.5},c);      // scaled direction: same ray
 TASK_CHECK(g1.has_value()==g2.has_value());
 if(g1&&g2){TASK_CHECK(approx(g1->arc,g2->arc,1e-12));
  TASK_CHECK(approxV(g1->hit.sp.p,g2->hit.sp.p,1e-12));
  TASK_CHECK(approxV(g1->tangent,g2->tangent,1e-12));}
 return 0;}

static int i_arc_limits(){
 Scene sc; sc.ms.reserve(1);
 I m=addMat(sc,T::D,V(0),V(0.5)); I ob=addObj(sc,T::NonS,m);
 I v0=I(sc.G.ps.size());
 sc.G.ps.insert(sc.G.ps.end(),{V(-1,-1,0),V(1,-1,0),V(0,1,0)});
 auto& fs=sc.os[ob].fs;
 fs.insert(fs.end(),{Ind{v0,-1,-1},Ind{v0+1,-1,-1},Ind{v0+2,-1,-1}});
 sc.build();
 GrinConfig c{0,1,1,0.0,2.5};
 V o(0.1,0.2,0), d(0,0,-1);
 auto place=[&](F z){sc.G.ps[v0].z=z; sc.G.ps[v0+1].z=z; sc.G.ps[v0+2].z=z;
  sc.trs.clear(); sc.ns.clear(); sc.ti.clear(); sc.build();};
 place(-2.5);                                 // exactly at maxArc: included
 TASK_CHECK(intersectGrin(sc,{o,d},c).has_value());
 place(-2.500001);                            // beyond maxArc: excluded
 TASK_CHECK(!intersectGrin(sc,{o,d},c).has_value());
 place(-1.25);                                // exactly at arcMin: included
 TASK_CHECK(intersectGrin(sc,{o,d},c,1.25).has_value());
 place(-1.24);                                // before arcMin: excluded
 TASK_CHECK(!intersectGrin(sc,{o,d},c,1.25).has_value());
 place(-1.0);                                 // ordinary mid-range hit
 auto gh=intersectGrin(sc,{o,d},c,1.25);
 TASK_CHECK(!gh);                             // 1.0 < arcMin
 gh=intersectGrin(sc,{o,d},c);
 TASK_CHECK(gh&&approx(gh->arc,1.0,1e-9));
 return 0;}

static int i_tangent(){
 GrinConfig c{0.0,1.0986122886681098,1.0,1.0,2.0};
 V o(0,0.99,0), d=norm(V(0,-0.2,-1));
 F uh=hypot(d.x,d.z), uy=d.y;
 F st=artanh_(-uy);                           // turning arc (k=1)
 F yt=0.99+log(uh);                           // turning height
 F e0=sqrt((1+uy)/(1-uy));
 F zt=(d.z/uh)*2*(atan(1.0)-atan(e0));        // turning z
 V C(0,yt,zt);
 auto P=makeGrinPath(c,{o,d});
 TASK_CHECK(approxV(P.point(st),C,1e-9));
 Scene sc; sc.ms.reserve(2);
 I m=addMat(sc,T::D,V(0),V(0.5)); I ob=addObj(sc,T::NonS,m);
 addTri(sc,ob,triAt(C,V(0,1,0),0.05));        // tangent to the path at C
 sc.build();
 auto gh=intersectGrin(sc,{o,d},c);           // grazing contact counts
 TASK_CHECK(gh.has_value());
 TASK_CHECK(approx(gh->arc,st,1e-6));
 TASK_CHECK(approxV(gh->hit.sp.p,C,1e-6));
 TASK_CHECK(gh->object==ob&&gh->face==0);
 Scene sc2; sc2.ms.reserve(1);                // coplanar ray: face ignored
 I m2=addMat(sc2,T::D,V(0),V(0.5)); I ob2=addObj(sc2,T::NonS,m2);
 addTri(sc2,ob2,{V(-1,0,-1),V(1,0,-1),V(0,0,1)}); sc2.build();
 GrinConfig c0{0,1,1,0.0,5.0};
 TASK_CHECK(!intersectGrin(sc2,{V(0.1,0,0.2),V(1,0,0)},c0));
 Scene sc3; sc3.ms.reserve(1);                // zero-area face ignored
 I m3=addMat(sc3,T::D,V(0),V(0.5)); I ob3=addObj(sc3,T::NonS,m3);
 addTri(sc3,ob3,{V(0,0,0),V(1,0,0),V(2,0,0)}); sc3.build();
 TASK_CHECK(!intersectGrin(sc3,{V(1,-1,0),V(0,1,0)},c0));
 return 0;}

static int i_identity_uv(){
 GrinConfig c{0.0,1.0,1.0,1.3,4.0};
 V o(0,0.5,-2), d=norm(V(0,-0.3,1));
 auto P=makeGrinPath(c,{o,d});
 F ss=0.9; V C=P.point(ss), tg=P.tangent(ss);
 Scene sc; sc.ms.reserve(2);
 I m0=addMat(sc,T::D,V(0),V(0.1)); I m1=addMat(sc,T::D,V(0),V(0.9));
 I o0=addObj(sc,T::NonS,m0), o1=addObj(sc,T::NonS,m1);
 addTri(sc,o0,triAt(V(50,0,0),V(1,0,0),1.0));        // decoys
 addTri(sc,o1,triAt(V(-50,0,0),V(1,0,0),1.0));
 array<V,3> n{norm(V(1,0,0.2)),norm(V(0.2,1,0)),norm(V(0,0.3,1))};
 array<V,3> t{V(0.3,0.4,0),V(0.6,0.8,0),V(0.9,0.1,0)};
 addTriN(sc,o1,triAt(C,-tg,0.1),n,t);                // target: o1, fi=3
 sc.build();
 auto gh=intersectGrin(sc,{o,d},c);
 TASK_CHECK(gh.has_value());
 TASK_CHECK(gh->object==o1&&gh->face==3);
 TASK_CHECK(gh->hit.o==&sc.os[o1]);
 TASK_CHECK(gh->hit.o->M==&sc.ms[m1]);               // original material
 TASK_CHECK(approx(gh->arc,ss,1e-6));
 TASK_CHECK(approxV(gh->hit.sp.p,C,1e-6));
 TASK_CHECK(approxV(gh->tangent,tg,1e-6));
 TASK_CHECK(approxV(gh->hit.sp.t,(t[0]+t[1]+t[2])/3.0,1e-9));
 TASK_CHECK(approxV(gh->hit.sp.n,norm(n[0]+n[1]+n[2]),1e-9));
 return 0;}

static int i_pruning(){
 Scene sc; sc.ms.reserve(2);
 I m0=addMat(sc,T::D,V(0),V(0.5)); I m1=addMat(sc,T::D,V(0),V(0.6));
 I o0=addObj(sc,T::NonS,m0), o1=addObj(sc,T::NonS,m1);
 addTri(sc,o0,{V(-1,-1,-2),V(1,-1,-2),V(0,1,-2)});   // near target
 mt19937 g(3); uniform_real_distribution<F> U(100,200), R_(0.5,1.5);
 for(I i=0;i<300;i++){V c(U(g),U(g),U(g));           // many far faces
  addTri(sc,o1,triAt(c,norm(V(R_(g),R_(g),R_(g))),R_(g)));}
 sc.build();
 GrinConfig c{0,1,1,0.0,10.0};
 grinWorkReset();
 auto gh=intersectGrin(sc,{V(0.1,0.2,0),V(0,0,-1)},c);
 TASK_CHECK(gh.has_value());
 TASK_CHECK(approx(gh->arc,2.0,1e-9));
 TASK_CHECK(gh->object==o0&&gh->face==0);
 auto w=grinWork();
 TASK_CHECK(w.triangleTests<=4);                     // far faces pruned
 TASK_CHECK(w.boundsTests<=128);
 TASK_CHECK(w.triangleTests<301);
 grinWorkReset();
 auto g2=intersectGrin(sc,{V(0,0,0),V(0,0,1)},c);    // miss: no leaf work
 TASK_CHECK(!g2);
 TASK_CHECK(grinWork().triangleTests==0);
 TASK_CHECK(grinWork().boundsTests<=128);
 return 0;}

static int i_tie_break(){
 GrinConfig c{0,1,1,0.0,5.0};
 array<V,3> tA{V(0,0,0),V(1,0,0),V(0,1,0)}, tB{V(1,0,0),V(1,1,0),V(0,1,0)};
 Scene sc; sc.ms.reserve(1);                          // two objects share an edge
 I m=addMat(sc,T::D,V(0),V(0.5));
 I o0=addObj(sc,T::NonS,m), o1=addObj(sc,T::NonS,m);
 addTri(sc,o0,tA); addTri(sc,o1,tB); sc.build();
 auto gh=intersectGrin(sc,{V(0.5,0.5,1),V(0,0,-1)},c);
 TASK_CHECK(gh.has_value());
 TASK_CHECK(approx(gh->arc,1.0,1e-12));
 TASK_CHECK(gh->object==o0&&gh->face==0);             // lower object index
 Scene sc2; sc2.ms.reserve(1);                        // same object, two faces
 I m2=addMat(sc2,T::D,V(0),V(0.5)); I ob=addObj(sc2,T::NonS,m2);
 addTri(sc2,ob,tB); addTri(sc2,ob,tA);                // tA second: fi=3
 sc2.build();
 auto g2=intersectGrin(sc2,{V(0.5,0.5,1),V(0,0,-1)},c);
 TASK_CHECK(g2.has_value());
 TASK_CHECK(g2->object==ob&&g2->face==0);             // lower face index
 return 0;}

// ===========================================================================
// transport group
// ===========================================================================
static int tr_specular(){
 Scene sc; sc.ms.reserve(2);
 I mMir=addMat(sc,T::PRefl);
 I mEm=addMat(sc,T::AreaL,V(0.64,0.25,0.09));
 I oMir=addObj(sc,T::PRefl,mMir), oEm=addObj(sc,T::AreaL,mEm);
 GrinConfig c{0.0,1.0,1.0,1.0,8.0};
 V camPos(0,2,0), camDir=norm(V(0.1,-1,0.05));
 pinhole(sc,camPos,camPos+camDir);
 auto P=makeGrinPath(c,{camPos,camDir});
 F sm=bisectY(P,-0.5,0,c.maxArc);
 V Pm=P.point(sm), Um=P.tangent(sm);
 V Nm=norm(V(1,-0.2,0.1));
 V wi=-Um, u2=refl(wi,Nm);                   // reflection off arrival tangent
 TASK_CHECK(u2.y<-0.05);                     // stays below the layer
 V Pe=Pm+u2;
 TASK_CHECK(Pe.y<-0.6);
 addTri(sc,oMir,triAt(Pm,Nm,0.2));
 addTri(sc,oEm,triAt(Pe,-u2,0.06));
 sc.build();
 Rng rn(1);
 V L=traceGrin(sc,rn,V(0.5,0.5,0),c,3);
 // factors telescope: (n_cam/n_mirror)^2 * (n_mirror/n_emit)^2 = (e/1)^2
 TASK_CHECK(approxV(L,V(0.64,0.25,0.09)*exp(2.0),1e-9));
 return 0;}

static int tr_multiplier(){
 Scene sc; sc.ms.reserve(1);                  // (a) one segment into layer
 I mEm=addMat(sc,T::AreaL,V(0.64,0.25,0.09)); I oEm=addObj(sc,T::AreaL,mEm);
 GrinConfig c{0.0,1.0,1.0,1.0,8.0};
 V camPos(0,-1,0), camDir=norm(V(0.05,1,-0.03));
 pinhole(sc,camPos,camPos+camDir);
 auto P=makeGrinPath(c,{camPos,camDir});
 F sE=bisectY(P,0.5,0,c.maxArc);
 V Pe=P.point(sE), te=P.tangent(sE);
 addTri(sc,oEm,triAt(Pe,-te,0.1)); sc.build();
 Rng rn(2);
 V L=traceGrin(sc,rn,V(0.5,0.5,0),c,1);
 TASK_CHECK(approxV(L,V(0.64,0.25,0.09)*exp(-1.0),1e-9));
 Scene sc2; sc2.ms.reserve(1);                // (b) k=0: uniform, factor 1
 I mE2=addMat(sc2,T::AreaL,V(0.64,0.25,0.09)); I oE2=addObj(sc2,T::AreaL,mE2);
 GrinConfig c0{0.0,1.0,2.0,0.0,8.0};
 pinhole(sc2,V(0,0,0),V(0.1,0.2,-1));
 auto P0=makeGrinPath(c0,{V(0,0,0),norm(V(0.1,0.2,-1))});
 V Pe0=P0.point(2.0), t0=P0.tangent(2.0);
 addTri(sc2,oE2,triAt(Pe0,-t0,0.1)); sc2.build();
 Rng rn2(3);
 V L0=traceGrin(sc2,rn2,V(0.5,0.5,0),c0,1);
 TASK_CHECK(approxV(L0,V(0.64,0.25,0.09),1e-9));
 Scene sc3; sc3.ms.reserve(3);                // (c) three segments, two mirrors
 I mM=addMat(sc3,T::PRefl); I mE3=addMat(sc3,T::AreaL,V(0.64,0.25,0.09));
 I oM1=addObj(sc3,T::PRefl,mM), oM2=addObj(sc3,T::PRefl,mM);
 I oE3=addObj(sc3,T::AreaL,mE3);
 V camPos3(0,2,0), camDir3=norm(V(0.1,-1,0.05)); // above the layer (n=e)
 pinhole(sc3,camPos3,camPos3+camDir3);
 auto P3th=makeGrinPath(c,{camPos3,camDir3});
 F s1=bisectY(P3th,-0.5,0,c.maxArc);
 V P1=P3th.point(s1), U1=P3th.tangent(s1);
 V N1=norm(V(1,-0.2,0.1)), u2=refl(-U1,N1);
 TASK_CHECK(u2.y<-0.05);                      // stays below the layer
 V P2=P1+u2*0.8;
 V N2=norm(V(0.3,0.2,0.9)), u3=refl(-u2,N2);
 TASK_CHECK(u3.y<-0.05);                      // stays below the layer
 V P3=P2+u3*0.9;
 TASK_CHECK(P3.y<-1.0);
 addTri(sc3,oM1,triAt(P1,N1,0.2));
 addTri(sc3,oM2,triAt(P2,N2,0.2));
 addTri(sc3,oE3,triAt(P3,-u3,0.06));
 sc3.build();
 Rng rn3(4);
 V L3=traceGrin(sc3,rn3,V(0.5,0.5,0),c,4);
 // (n_cam/n_1)^2*(n_1/n_2)^2*(n_2/n_3)^2 = (e/1)^2*1*1 = e^2
 TASK_CHECK(approxV(L3,V(0.64,0.25,0.09)*exp(2.0),1e-9));
 return 0;}

static int tr_envdir(){
 Scene sc; sc.ms.reserve(1);
 Obj env; env.t=T::EnvL;
 env.EL.map.w=1; env.EL.map.h=2;
 env.EL.map.cs={1,0,0, 0,1,0};                // up: red, down: green
 env.EL.rot=0;
 vector<F> ls(2);
 for(I i=0;i<2;i++){V v(env.EL.map.cs[3*i],env.EL.map.cs[3*i+1],
  env.EL.map.cs[3*i+2]); ls[i]=v.m()*sin(Pi*(i/1+.5)/2);}
 env.EL.dist.init(ls,1,2);
 sc.os.push_back(env); sc.useEL=1;
 I mD=addMat(sc,T::D,V(0),V(0.5)); I oD=addObj(sc,T::NonS,mD);
 addTri(sc,oD,triAt(V(100,100,100),V(0,0,1),0.5));
 sc.build();
 GrinConfig c{0.0,1.0,1.0,1.0,8.0};
 V camPos(0,0.5,0), camDir=norm(V(0.6,-0.2,0.8));
 pinhole(sc,camPos,camPos+camDir);
 auto P=makeGrinPath(c,{camPos,camDir});
 TASK_CHECK(P.tangent(0).y<0);                // departs downward
 TASK_CHECK(P.tangent(c.maxArc).y>0);         // turns and leaves upward
 Rng rn(5);
 V L=traceGrin(sc,rn,V(0.5,0.5,0),c,2);
 // env is queried with the far tangent (upward -> red texel), and the
 // segment multiplier (n_start/n_end)^2 = (e^0.5/e)^2 = e^-1 applies.
 TASK_CHECK(approxV(L,V(exp(-1.0),0,0),1e-9));
 return 0;}

static int tr_fresnel(){
 Scene sc; sc.ms.reserve(2);                  // deterministic TIR bounce
 I mF=addMat(sc,T::Fres,V(0),V(0),1.5);
 I mEm=addMat(sc,T::AreaL,V(0.64,0.25,0.09));
 I oF=addObj(sc,T::Fres,mF), oEm=addObj(sc,T::AreaL,mEm);
 GrinConfig c{0,1,1,0.0,8.0};
 V Nf=norm(V(0.9,0,-0.2));                    // steep: TIR, Fr=1, no RNG branch
 pinhole(sc,V(0,0,0),V(0,0,-1));
 V wi(0,0,1);
 TASK_CHECK(dot(wi,Nf)<0);
 auto wt=refr(wi,-Nf,1.5);
 TASK_CHECK(!wt.has_value());                 // total internal reflection
 V u2=refl(wi,Nf);
 V Pe=V(0,0,-2)+u2*2.0;
 addTri(sc,oF,triAt(V(0,0,-2),Nf,0.5));
 addTri(sc,oEm,triAt(Pe,-u2,0.1));
 sc.build();
 Rng rn(6);
 V L=traceGrin(sc,rn,V(0.5,0.5,0),c,3);
 TASK_CHECK(approxV(L,V(0.64,0.25,0.09),1e-9));
 // Fresnel transmission: material Ni refracts, successor keeps bending
 Scene sc2; sc2.ms.reserve(3);
 I mF2=addMat(sc2,T::Fres,V(0),V(0),1.5);
 I mE2=addMat(sc2,T::AreaL,V(0.64,0.25,0.09));
 I mD2=addMat(sc2,T::AreaL,V(0.1,0.2,0.3));
 I oF2=addObj(sc2,T::Fres,mF2), oE2=addObj(sc2,T::AreaL,mE2);
 I oD2=addObj(sc2,T::AreaL,mD2);
 GrinConfig c2{0.0,1.0,1.0,1.3,8.0};
 V camPos2(0,-2,0), camDir2=norm(V(1.2,1,0));
 pinhole(sc2,camPos2,camPos2+camDir2);
 V hit2(1.2,-1,0);                            // camera ray meets y=-1 at x=1.2
 V wi2=-camDir2, n2(0,-1,0);
 auto wt2=refr(wi2,n2,1/1.5);
 TASK_CHECK(wt2.has_value());
 auto Pt=makeGrinPath(c2,{hit2,*wt2});        // transmitted ray bends
 V Pe2=Pt.point(2.5), te2=Pt.tangent(2.5);
 V decoy=hit2+*wt2*2.5;                       // straight-line decoy emitter
 TASK_CHECK(am(Pe2-decoy)>0.3);
 addTri(sc2,oF2,triAt(hit2,n2,0.3));
 addTri(sc2,oE2,triAt(Pe2,-te2,0.1));
 addTri(sc2,oD2,triAt(decoy,-*wt2,0.06));
 sc2.build();
 F cosI=dot(wi2,n2), r0=(1-1.5)/(1+1.5);
 F Fr=r0*r0+(1-r0*r0)*pow(1-cosI,5);
 F nEnd=grinIndex(c2,Pe2.y);
 V expect=V(0.64,0.25,0.09)*sq(1/1.5)*sq(1/nEnd);
 Rng rn2(7); I n=512, got=0;
 for(I i=0;i<n;i++){
  V L=traceGrin(sc2,rn2,V(0.5,0.5,0),c2,2);
  if(L.m()>0){got++; TASK_CHECK(approxV(L,expect,1e-9));}}
 F p=1-Fr, sg=sqrt(n*p*(1-p));
 TASK_CHECK(got>n*p-5*sg&&got<n*p+5*sg);
 return 0;}

// ===========================================================================
const task_case task_cases[]={
 {"trajectory","constant_index",t_constant},
 {"trajectory","boundaries",t_boundaries},
 {"trajectory","in_layer_start",t_in_layer_start},
 {"trajectory","horizontal_vertical",t_horizontal_vertical},
 {"trajectory","small_k",t_small_k},
 {"trajectory","turning",t_turning},
 {"trajectory","reciprocity",t_reciprocity},
 {"trajectory","validation",t_validation},
 {"intersection","straight_reference",i_straight_reference},
 {"intersection","curved_chord",i_curved_chord},
 {"intersection","two_roots",i_two_roots},
 {"intersection","non_unit_direction",i_non_unit},
 {"intersection","arc_limits",i_arc_limits},
 {"intersection","tangent_contact",i_tangent},
 {"intersection","identity_uv",i_identity_uv},
 {"intersection","pruning",i_pruning},
 {"intersection","tie_break",i_tie_break},
 {"transport","specular_continuation",tr_specular},
 {"transport","index_multiplier",tr_multiplier},
 {"transport","environment_direction",tr_envdir},
 {"transport","fresnel",tr_fresnel},
};
const size_t task_case_count=sizeof(task_cases)/sizeof(task_cases[0]);
