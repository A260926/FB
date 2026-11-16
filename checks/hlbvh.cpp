// checks/hlbvh.cpp — TASK_CHECK cases for the production LBVH path.
//
// Groups: keys (Morton coding, node numbering, structural checker),
// parallel (worker-count invariance, real work sharing, rebuilds),
// refit (frame-to-frame refits) and render (nearest-hit, finite-shadow and
// path-tracing queries through Scene::isect, verified against independent
// exhaustive intersection of the original faces).
#include "task_support/scene-under-test.h"
#include "task_support/check.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace{

void addTri(Scene& sc,I oi,V a,V b,V c){
 I n=I(sc.G.ps.size()); sc.G.ps.insert(sc.G.ps.end(),{a,b,c});
 auto& fs=sc.os[oi].fs; fs.insert(fs.end(),{Ind{n},Ind{n+1},Ind{n+2}});
}
void addTriN(Scene& sc,I oi,V a,V b,V c,V na,V nb,V nc,V ta,V tb,V tc){
 I np=I(sc.G.ps.size()),nn=I(sc.G.ns.size()),nt=I(sc.G.ts.size());
 sc.G.ps.insert(sc.G.ps.end(),{a,b,c}); sc.G.ns.insert(sc.G.ns.end(),{na,nb,nc});
 sc.G.ts.insert(sc.G.ts.end(),{ta,tb,tc});
 auto& fs=sc.os[oi].fs;
 fs.insert(fs.end(),{Ind{np,nt,nn},Ind{np+1,nt+1,nn+1},Ind{np+2,nt+2,nn+2}});
}
void addGrid(Scene& sc,I oi,I nx,I nz,F x0,F z0,F dx,F dz,F y){
 for(I i=0;i<nx;i++)for(I j=0;j<nz;j++){
  V a(x0+i*dx,y,z0+j*dz),b(x0+(i+1)*dx,y,z0+j*dz);
  V c(x0+i*dx,y,z0+(j+1)*dz),d(x0+(i+1)*dx,y,z0+(j+1)*dz);
  addTri(sc,oi,a,b,c); addTri(sc,oi,b,d,c);
 }
}
void addGridN(Scene& sc,I oi,I nx,I nz,F x0,F z0,F dx,F dz,F y){
 V n0=norm(V(.1,.9,.05)),n1=norm(V(-.2,.8,.1)),n2=norm(V(.05,.95,-.1)),n3=norm(V(0,1,.2));
 for(I i=0;i<nx;i++)for(I j=0;j<nz;j++){
  V a(x0+i*dx,y,z0+j*dz),b(x0+(i+1)*dx,y,z0+j*dz);
  V c(x0+i*dx,y,z0+(j+1)*dz),d(x0+(i+1)*dx,y,z0+(j+1)*dz);
  V ta(i*dx,j*dz,0),tb((i+1)*dx,j*dz,0),tc(i*dx,(j+1)*dz,0),td((i+1)*dx,(j+1)*dz,0);
  addTriN(sc,oi,a,b,c,n0,n1,n2,ta,tb,tc);
  addTriN(sc,oi,b,d,c,n1,n3,n2,tb,td,tc);
 }
}
void oneMat(Scene& sc,V kd){sc.ms.push_back({T::D,kd}); sc.os.push_back({T::D,&sc.ms[0]});}

F adiff(V a,V b){return max({abs(a.x-b.x),abs(a.y-b.y),abs(a.z-b.z)});}

// Independent exhaustive intersection of the original faces (no BVH).
struct BF{bool hit=0; F t=0,u=0,v=0; I oi=-1,fi=-1;};
BF brute(Scene& sc,const R& r,F tl,F th){
 BF b; b.t=th;
 for(size_t oi=0;oi<sc.os.size();oi++){auto& fs=sc.os[oi].fs;
  for(size_t fi=0;fi+2<fs.size();fi+=3){
   Scene::Tri tr(sc.G.ps[fs[fi].p],sc.G.ps[fs[fi+1].p],sc.G.ps[fs[fi+2].p],I(oi),I(fi));
   if(auto h=tr.isect(r,tl,b.t)){b.hit=1; b.t=h->t; b.u=h->u; b.v=h->v; b.oi=I(oi); b.fi=I(fi);}}}
 return b;
}

// Bounded wait around a parallel build/refit so a deadlock fails the case.
bool runBounded(std::function<void()> fn,double secs){
 std::atomic<bool> done{0};
 std::thread t([&]{fn(); done.store(1);});
 auto t0=std::chrono::steady_clock::now();
 while(!done.load()){
  if(std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count()>secs){
   t.detach(); return false;}
  std::this_thread::sleep_for(std::chrono::milliseconds(1));}
 t.join(); return true;
}

// ---------------- keys ----------------
int k_morton(){
 TASK_CHECK(lmorton(0,0,0)==0u);
 TASK_CHECK(lmorton(1,0,0)==4u);
 TASK_CHECK(lmorton(0,1,0)==2u);
 TASK_CHECK(lmorton(0,0,1)==1u);
 TASK_CHECK(lmorton(1023,1023,1023)==0x3fffffffu);
 Scene sc; oneMat(sc,V(.5));
 for(I k=0;k<4;k++){addTri(sc,0,V(3*k,0,0),V(3*k+1,0,0),V(3*k,1,0));}
 sc.buildLBVH(1);
 auto& d=sc.lbvhDiag();
 TASK_CHECK(d.workers==1&&d.mc.size()==4&&d.ti.size()==4);
 const uint32_t q[4]={0,341,682,1023};   // min(1023,floor(1024*k/3))
 for(I k=0;k<4;k++){TASK_CHECK(d.mc[k]==(lspread(q[k])<<2));}  // y,z zero-span -> 0
 for(I k=0;k<4;k++){TASK_CHECK(d.ti[k]==k);}
 TASK_CHECK(sc.lns.size()==7);
 TASK_CHECK(sc.checkLBVH());
 return 0;
}
int k_same_code(){
 Scene sc; oneMat(sc,V(.5));
 V a(0,0,0),b(1,0,0),c(0,1,0);
 addTri(sc,0,a,b,c); addTri(sc,0,a,b,c); addTri(sc,0,a,b,c);   // identical keys
 addTri(sc,0,V(4,0,0),V(5,0,0),V(4,1,0));
 sc.buildLBVH(2);
 auto& d=sc.lbvhDiag();
 TASK_CHECK(d.mc[0]==d.mc[1]&&d.mc[1]==d.mc[2]);
 TASK_CHECK(d.mc[3]!=d.mc[0]);
 // identical codes ordered by original triangle index
 TASK_CHECK(d.ti[0]==0&&d.ti[1]==1&&d.ti[2]==2&&d.ti[3]==3);
 TASK_CHECK(sc.lns.size()==7);
 TASK_CHECK(sc.checkLBVH());
 auto h=sc.isect({V(.2,.2,1),V(0,0,-1)});
 TASK_CHECK(h&&h->o==&sc.os[0]);
 TASK_CHECK(abs(h->sp.p.z)<1e-12&&abs(h->sp.p.x-.2)<1e-12&&abs(h->sp.p.y-.2)<1e-12);
 return 0;
}
int k_zero_span(){
 Scene sc; oneMat(sc,V(.5));
 addGrid(sc,0,4,4, 0,0, 1,1, 2.5);   // plane y=2.5: y axis has zero span
 sc.buildLBVH(2);
 auto& d=sc.lbvhDiag();
 TASK_CHECK(d.mc.size()==32);
 for(auto m:d.mc){TASK_CHECK((m&(lspread(1023)<<1))==0);}   // y bits all zero
 TASK_CHECK(sc.checkLBVH());
 auto h=sc.isect({V(1.5,0,1.5),V(0,1,0)});
 TASK_CHECK(h&&abs(h->sp.p.y-2.5)<1e-12);
 return 0;
}
int k_elongated(){
 Scene sc; oneMat(sc,V(.5));
 addGrid(sc,0,1,512, 0,0, .01,1, 0);   // 1024 extremely elongated faces
 sc.buildLBVH(4);
 TASK_CHECK(sc.lns.size()==2*1024-1);
 TASK_CHECK(sc.checkLBVH());
 for(I k=0;k<8;k++){V o(.005,.5,k*64+.3); R r{o,V(0,-1,0)};
  auto h=sc.isect(r); auto b=brute(sc,r,Eps,Inf);
  TASK_CHECK(h.has_value()==b.hit);
  if(h&&b.hit){TASK_CHECK(adiff(h->sp.p,o+b.t*r.d)<1e-9); TASK_CHECK(h->o==&sc.os[b.oi]);}}
 return 0;
}
int k_numbering(){
 Scene sc; oneMat(sc,V(.5));
 for(I k=0;k<8;k++){addTri(sc,0,V(k*7%13,(k*3)%5,k),V(k*7%13+1,(k*3)%5,k),V(k*7%13,(k*3)%5+1,k+.5));}
 sc.buildLBVH(1);
 I nt=8,ni=nt-1;
 TASK_CHECK(I(sc.lns.size())==2*nt-1);
 for(I i=0;i<ni;i++){auto& n=sc.lns[i];
  TASK_CHECK(!n.leaf);
  auto& a=sc.lns[n.c1]; auto& b=sc.lns[n.c2];
  I ls=a.e-a.s,rs=b.e-b.s;
  TASK_CHECK(a.s==n.s&&b.e==n.e&&a.e==b.s);          // children partition [s,e)
  if(ls==1){TASK_CHECK(n.c1==ni+n.s);}else{TASK_CHECK(n.c1==i+1);}   // preorder ids
  if(rs==1){TASK_CHECK(n.c2==ni+a.e);}else{TASK_CHECK(n.c2==i+ls);}
  TASK_CHECK(a.par==i&&b.par==i);
 }
 for(I s=0;s<nt;s++){auto& n=sc.lns[ni+s];           // leaf id == n-1 + slot
  TASK_CHECK(n.leaf&&n.s==s&&n.e==s+1&&n.c1==-1&&n.c2==-1);}
 TASK_CHECK(sc.checkLBVH());
 return 0;
}
int k_checker_detects(){
 Scene sc; oneMat(sc,V(.5));
 for(I k=0;k<8;k++){addTri(sc,0,V(2.*k,0,0),V(2.*k+1,0,0),V(2.*k,1,0));}
 sc.buildLBVH(2);
 TASK_CHECK(sc.checkLBVH());
 auto saved=sc.lns; I ni=I(sc.ti.size())-1;
 sc.lns[ni].s=sc.lns[ni+1].s;                        // duplicate leaf slot
 TASK_CHECK(!sc.checkLBVH());
 sc.lns=saved; sc.lns[1].b.ma=V(1e9);                // child box escapes parent
 TASK_CHECK(!sc.checkLBVH());
 sc.lns=saved; sc.lns[2].par=5;                      // wrong parent link
 TASK_CHECK(!sc.checkLBVH());
 sc.lns=saved; sc.lns[1].e+=1;                       // broken child interval
 TASK_CHECK(!sc.checkLBVH());
 sc.lns=saved; sc.lns[0].c1=sc.lns[0].c2;            // subtree unreachable
 TASK_CHECK(!sc.checkLBVH());
 sc.lns=saved;
 TASK_CHECK(sc.checkLBVH());
 return 0;
}

// ---------------- parallel ----------------
int p_consistency(){
 Scene sc; oneMat(sc,V(.5));
 uint32_t h=12345;
 auto rnd=[&](){h=h*1664525u+1013904223u; return (h>>8)/16777216.0;};
 for(I k=0;k<3000;k++){V a(rnd()*10,rnd()*10,rnd()*10);
  addTri(sc,0,a,a+V(rnd(),rnd()*.5,rnd()),a+V(rnd()*.5,rnd(),rnd()*.2));}
 TASK_CHECK(runBounded([&]{sc.buildLBVH(1);},60));
 auto d1=sc.lbvhDiag(); TASK_CHECK(sc.checkLBVH());
 TASK_CHECK(runBounded([&]{sc.buildLBVH(2);},60));
 auto d2=sc.lbvhDiag(); TASK_CHECK(sc.checkLBVH());
 TASK_CHECK(runBounded([&]{sc.buildLBVH(4);},60));
 auto d4=sc.lbvhDiag(); TASK_CHECK(sc.checkLBVH());
 TASK_CHECK(d1.ti==d2.ti&&d1.ti==d4.ti);
 TASK_CHECK(d1.mc==d2.mc&&d1.mc==d4.mc);
 TASK_CHECK(d1.ns.size()==d2.ns.size()&&d1.ns.size()==d4.ns.size());
 for(size_t i=0;i<d1.ns.size();i++){auto&a=d1.ns[i];auto&b=d2.ns[i];auto&c=d4.ns[i];
  TASK_CHECK(a.leaf==b.leaf&&a.leaf==c.leaf&&a.c1==b.c1&&a.c1==c.c1&&a.c2==b.c2&&a.c2==c.c2);
  TASK_CHECK(a.s==b.s&&a.s==c.s&&a.e==b.e&&a.e==c.e&&a.par==b.par&&a.par==c.par);
  TASK_CHECK(a.b.mi.x==b.b.mi.x&&a.b.mi.x==c.b.mi.x&&a.b.mi.y==b.b.mi.y&&a.b.mi.y==c.b.mi.y&&a.b.mi.z==b.b.mi.z&&a.b.mi.z==c.b.mi.z);
  TASK_CHECK(a.b.ma.x==b.b.ma.x&&a.b.ma.x==c.b.ma.x&&a.b.ma.y==b.b.ma.y&&a.b.ma.y==c.b.ma.y&&a.b.ma.z==b.b.ma.z&&a.b.ma.z==c.b.ma.z);}
 TASK_CHECK(sc.ti==d4.ti);
 TASK_CHECK(d4.ns.size()==sc.lns.size());
 for(size_t i=0;i<sc.lns.size();i++){
  TASK_CHECK(d4.ns[i].c1==sc.lns[i].c1&&d4.ns[i].c2==sc.lns[i].c2&&d4.ns[i].s==sc.lns[i].s&&d4.ns[i].e==sc.lns[i].e&&d4.ns[i].par==sc.lns[i].par&&d4.ns[i].leaf==sc.lns[i].leaf);}
 return 0;
}
int p_threads(){
 Scene sc; oneMat(sc,V(.5));
 addGrid(sc,0,64,64, 0,0, 1,1, 0);   // 8192 non-degenerate faces
 TASK_CHECK(runBounded([&]{sc.buildLBVH(4);},60));
 TASK_CHECK(sc.trs.size()==8192);
 auto& d=sc.lbvhDiag();
 TASK_CHECK(d.workers==4&&d.th.size()==4);
 unsigned tt=0,tb=0,wt=0,wb=0;
 for(auto& x:d.th){tt+=x.topo; tb+=x.bnd; wt+=x.topo>0; wb+=x.bnd>0;}
 TASK_CHECK(tt==8191&&tb==8191);
 TASK_CHECK(wt>=3);   // more than two threads did internal-node tasks
 TASK_CHECK(wb>=3);   // more than two threads did bounds reductions
 TASK_CHECK(sc.checkLBVH());
 for(I k=0;k<4;k++){V o(5.5+k,1,3.25+k); R r{o,V(0,-1,0)};
  auto hh=sc.isect(r); auto b=brute(sc,r,Eps,Inf);
  TASK_CHECK(hh.has_value()==b.hit);
  if(hh&&b.hit){TASK_CHECK(adiff(hh->sp.p,o+b.t*r.d)<1e-9);}}
 return 0;
}
int p_rebuild(){
 Scene sc; oneMat(sc,V(.5));
 TASK_CHECK(runBounded([&]{sc.buildLBVH(2);},30));   // empty tree
 TASK_CHECK(sc.useL&&sc.lns.empty()&&sc.trs.empty()&&sc.ti.empty());
 TASK_CHECK(sc.checkLBVH());
 TASK_CHECK(!sc.isect({V(0,0,0),V(0,0,1)}));
 addGrid(sc,0,4,4, 0,0, 1,1, 3);                     // refill
 TASK_CHECK(runBounded([&]{sc.buildLBVH(2);},30));
 TASK_CHECK(sc.trs.size()==32&&sc.lns.size()==63&&sc.ti.size()==32);
 auto h=sc.isect({V(1.5,0,1.5),V(0,1,0)});
 TASK_CHECK(h&&abs(h->sp.p.y-3)<1e-12);
 TASK_CHECK(sc.checkLBVH());
 auto d1=sc.lbvhDiag();
 TASK_CHECK(runBounded([&]{sc.buildLBVH(2);},30));   // second rebuild
 auto d2=sc.lbvhDiag();
 TASK_CHECK(sc.trs.size()==32&&sc.lns.size()==63);   // caches cleared, not accumulated
 TASK_CHECK(d1.ti==d2.ti&&d1.ns.size()==d2.ns.size());
 unsigned t1=0,t2=0; for(auto&x:d1.th){t1+=x.topo;} for(auto&x:d2.th){t2+=x.topo;}
 TASK_CHECK(t1==31&&t2==31);                         // counters reset per build
 TASK_CHECK(sc.checkLBVH());
 return 0;
}

// ---------------- refit ----------------
int r_deform(){
 Scene sc; oneMat(sc,V(.6,.2,.2));
 addGrid(sc,0,8,8, 0,0, 1,1, 0);
 sc.buildLBVH(2);
 TASK_CHECK(sc.checkLBVH());
 B root0=sc.lns[0].b; auto ti0=sc.ti;
 auto topo=[&](){vector<tuple<I,I,I,I,bool>> v; for(auto&n:sc.lns){v.emplace_back(n.c1,n.c2,n.s,n.e,n.leaf);} return v;};
 auto t0=topo();
 TASK_CHECK(!sc.isect({V(1.5,5,1.5),V(0,-1,0)},Eps,4));   // grid at t=5, beyond th
 TASK_CHECK(sc.isect({V(1.5,5,1.5),V(0,-1,0)},Eps,6));    // within th
 for(I st=1;st<=5;st++){                                  // continuous deformation
  for(auto& p:sc.G.ps){p.y+=2;}
  TASK_CHECK(runBounded([&]{sc.refitLBVH(2);},30));
  TASK_CHECK(sc.checkLBVH());
  TASK_CHECK(sc.ti==ti0);                                 // leaf order preserved
  TASK_CHECK(topo()==t0);                                 // connectivity/slots preserved
  TASK_CHECK(sc.lns[0].b.mi.y>=2*st-1e-12);               // root tracks geometry
 }
 TASK_CHECK(root0.ma.y<1&&sc.lns[0].b.mi.y>=10-1e-12);    // moved out of the old root box
 auto h=sc.isect({V(1.5,20,1.5),V(0,-1,0)});
 TASK_CHECK(h&&abs(h->sp.p.y-10)<1e-9&&h->o==&sc.os[0]);
 TASK_CHECK(!sc.isect({V(1.5,5,1.5),V(0,-1,0)}));         // nothing left at the old spot
 for(I k=0;k<16;k++){V o((k%4)+.31,20,(k/4)+.77); R r{o,V(0,-1,0)};
  auto hh=sc.isect(r); auto b=brute(sc,r,Eps,Inf);
  TASK_CHECK(hh.has_value()==b.hit);
  if(hh&&b.hit){TASK_CHECK(adiff(hh->sp.p,o+b.t*r.d)<1e-9); TASK_CHECK(hh->o==&sc.os[b.oi]);}}
 return 0;
}
int r_workers(){
 Scene sc; oneMat(sc,V(.5));
 addGrid(sc,0,16,16, 0,0, 1,1, 0);
 sc.buildLBVH(2);
 for(auto& p:sc.G.ps){p.x+=.25*p.z; p.y+=.1;}
 TASK_CHECK(runBounded([&]{sc.refitLBVH(1);},30));
 auto b1=sc.lns;
 TASK_CHECK(runBounded([&]{sc.refitLBVH(4);},30));   // same geometry, other worker count
 auto b4=sc.lns;
 TASK_CHECK(b1.size()==b4.size());
 for(size_t i=0;i<b1.size();i++){
  TASK_CHECK(b1[i].b.mi.x==b4[i].b.mi.x&&b1[i].b.mi.y==b4[i].b.mi.y&&b1[i].b.mi.z==b4[i].b.mi.z);
  TASK_CHECK(b1[i].b.ma.x==b4[i].b.ma.x&&b1[i].b.ma.y==b4[i].b.ma.y&&b1[i].b.ma.z==b4[i].b.ma.z);}
 auto& d=sc.lbvhDiag();                              // diagnostics still describe the build
 unsigned tp=0; for(auto&t:d.th){tp+=t.topo;}
 TASK_CHECK(d.workers==2&&tp==I(sc.trs.size())-1);
 TASK_CHECK(sc.checkLBVH());
 return 0;
}
int r_leaf_bounds(){
 Scene sc; oneMat(sc,V(.5));
 addGrid(sc,0,4,4, 0,0, 1,1, 0);
 sc.buildLBVH(1);
 for(I k=9;k<12;k++){sc.G.ps[k].z+=5;}               // move vertices of triangle 3
 sc.refitLBVH(1);
 I nt=I(sc.ti.size());
 for(I s=0;s<nt;s++){const LN& n=sc.lns[nt-1+s]; B tb=sc.trs[sc.ti[s]].b;
  TASK_CHECK(n.b.mi.x==tb.mi.x&&n.b.mi.y==tb.mi.y&&n.b.mi.z==tb.mi.z);
  TASK_CHECK(n.b.ma.x==tb.ma.x&&n.b.ma.y==tb.ma.y&&n.b.ma.z==tb.ma.z);}
 for(I i=nt-2;i>=0;i--){const LN& n=sc.lns[i]; B m=merge(sc.lns[n.c1].b,sc.lns[n.c2].b);
  TASK_CHECK(n.b.mi.x==m.mi.x&&n.b.mi.y==m.mi.y&&n.b.mi.z==m.mi.z);
  TASK_CHECK(n.b.ma.x==m.ma.x&&n.b.ma.y==m.ma.y&&n.b.ma.z==m.ma.z);}
 bool seen=0;
 for(I s=0;s<nt;s++){if(sc.ti[s]==3){seen=1; TASK_CHECK(sc.lns[nt-1+s].b.mi.z>=5-1e-12);}}
 TASK_CHECK(seen);
 TASK_CHECK(sc.checkLBVH());
 return 0;
}

// ---------------- render ----------------
int r_brute_force(){
 Scene sc;
 sc.ms.push_back({T::D,V(.7,.3,.2)});
 sc.ms.push_back({T::D,V(.2,.4,.8)});
 sc.os.push_back({T::D,&sc.ms[0]});
 sc.os.push_back({T::D,&sc.ms[1]});
 addGridN(sc,0,6,6, 0,0, 1,1, 3);
 addGridN(sc,1,4,4, .5,.5, 1,1, 5);
 sc.buildLBVH(4);
 TASK_CHECK(sc.checkLBVH());
 I tested=0;
 for(I oi=0;oi<2;oi++){auto& fs=sc.os[oi].fs;
  for(size_t fi=0;fi<fs.size();fi+=9){
   V a=sc.G.ps[fs[fi].p],b=sc.G.ps[fs[fi+1].p],c=sc.G.ps[fs[fi+2].p];
   V ce=(a+b+c)/3; V o=ce+V(.13,100,-.29); V d=norm(ce-o); R r{o,d};
   auto h=sc.isect(r); auto bf=brute(sc,r,Eps,Inf);
   TASK_CHECK(h.has_value()==bf.hit);
   if(!h||!bf.hit){continue;}
   TASK_CHECK(adiff(h->sp.p,o+bf.t*d)<1e-9);                       // world position
   TASK_CHECK(abs(sqrt(dot(h->sp.p-o,h->sp.p-o))-bf.t)<1e-9);      // distance
   TASK_CHECK(h->o==&sc.os[bf.oi]);                                // object identity
   TASK_CHECK(h->o->M==sc.os[bf.oi].M&&h->o->M==&sc.ms[bf.oi]);    // material identity
   auto& ofs=sc.os[bf.oi].fs;
   Ind i0=ofs[bf.fi],i1=ofs[bf.fi+1],i2=ofs[bf.fi+2];
   TASK_CHECK(i0.n>=0&&i0.t>=0);
   V en=norm(intp(sc.G.ns[i0.n],sc.G.ns[i1.n],sc.G.ns[i2.n],bf.u,bf.v));
   TASK_CHECK(adiff(h->sp.n,en)<1e-9);                             // interpolated normal
   V et=intp(sc.G.ts[i0.t],sc.G.ts[i1.t],sc.G.ts[i2.t],bf.u,bf.v);
   TASK_CHECK(adiff(h->sp.t,et)<1e-9);                             // interpolated uv
   tested++;
  }}
 TASK_CHECK(tested>10);
 for(I k=0;k<4;k++){R r{V(50+k,50,0),V(0,0,1)};
  TASK_CHECK(!sc.isect(r)); TASK_CHECK(!brute(sc,r,Eps,Inf).hit);}
 for(I k=0;k<8;k++){                                              // finite shadow rays
  V p(.5+k*.7,1,.5+(k%3)); V q=p+V(0,5,.2); V d=q-p; F L=sqrt(dot(d,d)); d=d/L;
  R r{p,d}; F th=L*.9;
  auto h=sc.isect(r,Eps,th); auto bf=brute(sc,r,Eps,th);
  TASK_CHECK(h.has_value()==bf.hit);
  if(h&&bf.hit){TASK_CHECK(adiff(h->sp.p,p+bf.t*d)<1e-9);}
 }
 return 0;
}
int r_path_trace(){
 Scene sc;
 sc.ms.push_back({T::D,V(.7,.7,.7)});
 Mat lm; lm.t=T::L; lm.Ke=V(6,6,6); sc.ms.push_back(lm);
 sc.os.push_back({T::D,&sc.ms[0]});
 sc.os.push_back({T::L,&sc.ms[1]});
 addGrid(sc,0,4,4, -2,-2, 1,1, 0);
 addTri(sc,1,V(-1,2.5,-1),V(1,2.5,-1),V(1,2.5,1));
 addTri(sc,1,V(-1,2.5,-1),V(1,2.5,1),V(-1,2.5,1));
 sc.Ls.push_back(1);
 sc.os[1].initAreaL(sc.G);
 const I w=8,h=8,ns=4,mb=6;
 sc.E.initE("",V(0,1.2,4),V(0,.4,0),V(0,1,0),40,0,0,0,F(w)/h);
 auto render=[&](vector<V>& D){
  D.assign(w*h,V());
  auto L=[&](Rng& rn,V&& uv){V L,T(1),wi=uv; I ty=T::E; Obj* o=&sc.E; Surf sp;
   for(I b=0;b<mb;b++){bool nee=!sc.Ls.empty()&&b>0&&ty&T::NonS;
    auto s=o->samp(rn,ty,sp,wi); if(!s){break;} auto[t,r,w,pcs]=*s; T=T/pcs;
    if(nee){auto[oL,pLs]=sc.sampL(rn); auto sL=oL.sampL(rn,sc.G,sp);
     if(sL){auto[wo,d,fL,pL]=*sL; if(!sc.isect({sp.p,wo},Eps,d*(1-Eps))){
      L=L+T*o->ev(t,sp,wi,wo)*fL/(o->pdf(t,sp,wi,wo)+pL*pLs);}}}
    T=T*w; auto hi=sc.isect(r); if(!hi){hi=H{{},sc.envL()}; if(!hi->o){break;}}
    auto[spH,oH]=*hi; if(oH->t&T::L){ L=L+T*oH->ev(oH->t&T::L,spH,{},-r.d)/
     (b==0||t&T::S?1:oH->pdfL(sp,spH,-r.d)*sc.pdfL()/o->pdf(t,sp,wi,r.d)+1);}
    if(b>3){F q=max(.2,1-T.m()); T=T/(1-q); if(rn.u()<q) break;}
    wi=-r.d; o=oH; sp=spH; ty=o->t&~T::L;} return L;};
  Rng rn(42);
  for(I i=0;i<w*h;i++){for(I j=0;j<ns;j++){
   D[i]=D[i]+L(rn,V((i%w+rn.u())/w,(i/w+rn.u())/h,0))/ns;}}
 };
 sc.buildLBVH(1); vector<V> A; render(A);
 TASK_CHECK(sc.checkLBVH());
 sc.buildLBVH(4); vector<V> B; render(B);
 TASK_CHECK(sc.checkLBVH());
 F mx=0;
 for(I i=0;i<w*h;i++){
  TASK_CHECK(A[i].x==B[i].x&&A[i].y==B[i].y&&A[i].z==B[i].z);
  TASK_CHECK(std::isfinite(A[i].x)&&std::isfinite(A[i].y)&&std::isfinite(A[i].z));
  TASK_CHECK(A[i].x>=0&&A[i].y>=0&&A[i].z>=0);
  mx=max(mx,A[i].m());}
 TASK_CHECK(mx>0);
 return 0;
}
int r_cli(){
 char prog[]="minpt";
 {char* v[]={prog}; TASK_CHECK(minpt_original_main(1,v)!=0);}
 char lb[]="--lbvh",bad[]="--sah",w0[]="0",w65[]="65",wx[]="x",w4[]="4",ex[]="ex";
 char* v[24]; for(int i=0;i<24;i++){v[i]=prog;}
 v[20]=lb; v[21]=w0;  TASK_CHECK(minpt_original_main(22,v)!=0);
 v[21]=w65;           TASK_CHECK(minpt_original_main(22,v)!=0);
 v[21]=wx;            TASK_CHECK(minpt_original_main(22,v)!=0);
 v[20]=bad; v[21]=w4; TASK_CHECK(minpt_original_main(22,v)!=0);
 v[20]=lb;            TASK_CHECK(minpt_original_main(21,v)!=0);
 v[21]=w4; v[22]=ex;  TASK_CHECK(minpt_original_main(23,v)!=0);
 return 0;
}

} // namespace

const task_case task_cases[]={
 {"keys","morton",k_morton},
 {"keys","same_code",k_same_code},
 {"keys","zero_span",k_zero_span},
 {"keys","elongated",k_elongated},
 {"keys","numbering",k_numbering},
 {"keys","checker_detects",k_checker_detects},
 {"parallel","consistency",p_consistency},
 {"parallel","threads",p_threads},
 {"parallel","rebuild",p_rebuild},
 {"refit","deform",r_deform},
 {"refit","workers",r_workers},
 {"refit","leaf_bounds",r_leaf_bounds},
 {"render","brute_force",r_brute_force},
 {"render","path_trace",r_path_trace},
 {"render","cli",r_cli},
};
const size_t task_case_count=sizeof(task_cases)/sizeof(task_cases[0]);
