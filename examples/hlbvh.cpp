// examples/hlbvh.cpp — windowless render using the production LBVH path.
//
// Builds a small scene with the production Scene, constructs the LBVH with
// Scene::buildLBVH, renders a frame, deforms the geometry between frames
// (G.ps only, object/face topology fixed), updates the tree with
// Scene::refitLBVH and renders again. The final frame is written as a
// binary PPM (P6).
//
// Build:  c++ -std=c++17 -O1 -pthread -Itask_support -I. \
//           examples/hlbvh.cpp src/hlbvh.cpp -o hlbvh_example
// Run:    ./hlbvh_example out.ppm
#include "task_support/scene-under-test.h"

static void addTri(Scene& sc,I oi,V a,V b,V c){
 I n=I(sc.G.ps.size()); sc.G.ps.insert(sc.G.ps.end(),{a,b,c});
 auto& fs=sc.os[oi].fs; fs.insert(fs.end(),{Ind{n},Ind{n+1},Ind{n+2}});
}

I main(I ac,C**v){
 if(ac<2){fprintf(stderr,"usage: %s out.ppm\n",v[0]); return 1;}
 Scene sc;
 sc.ms.push_back({T::D,V(.75,.72,.68)});                 // 0: ground
 sc.ms.push_back({T::D,V(.85,.35,.25)});                 // 1: tents
 Mat lm; lm.t=T::L; lm.Ke=V(9,9,9); sc.ms.push_back(lm); // 2: area light
 sc.os.push_back({T::D,&sc.ms[0]});
 sc.os.push_back({T::D,&sc.ms[1]});
 sc.os.push_back({T::L,&sc.ms[2]});
 for(I i=0;i<8;i++)for(I j=0;j<8;j++){F x=-4+i,z=-4+j;
  addTri(sc,0,V(x,0,z),V(x+1,0,z),V(x,0,z+1));
  addTri(sc,0,V(x+1,0,z),V(x+1,0,z+1),V(x,0,z+1));}
 const size_t tv0=sc.G.ps.size();                        // first tent vertex
 for(I k=0;k<5;k++){F cx=-3+k*1.5,cz=-1+(k%2)*2;
  V a(cx,0,cz),b(cx+1,0,cz),c(cx+.5,0,cz+1),d(cx+.5,1.2,cz+.5);
  addTri(sc,1,a,b,d); addTri(sc,1,b,c,d); addTri(sc,1,c,a,d);}
 const size_t tv1=sc.G.ps.size();                        // one past last tent vertex
 addTri(sc,2,V(-1,3.2,-1),V(1,3.2,-1),V(1,3.2,1));       // light, normal -y
 addTri(sc,2,V(-1,3.2,-1),V(1,3.2,1),V(-1,3.2,1));
 sc.Ls.push_back(2);
 sc.os[2].initAreaL(sc.G);
 sc.buildLBVH(4);
 if(!sc.checkLBVH()){fprintf(stderr,"structure check failed\n"); return 1;}
 {unsigned tp=0,bd=0; for(auto& t:sc.lbvhDiag().th){tp+=t.topo; bd+=t.bnd;}
  printf("buildLBVH: tris=%zu nodes=%zu workers=%u topo=%u bnd=%u\n",
   sc.trs.size(),sc.lns.size(),sc.lbvhDiag().workers,tp,bd);}
 const I w=160,h=120,ns=8,mb=8;
 sc.E.initE("",V(0,2.2,7),V(0,.8,0),V(0,1,0),38,0,0,0,F(w)/h);
 vector<V> D(w*h,V());
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
 auto render=[&](unsigned nTh){vector<thread> ths;
  for(unsigned t=0;t<nTh;t++){ths.emplace_back([&,t]{Rng rn(42+I(t));
   for(I i=I(t);i<w*h;i+=I(nTh)){for(I j=0;j<ns;j++){
    D[i]=D[i]+L(rn,V((i%w+rn.u())/w,(i/w+rn.u())/h,0))/ns;}}});}
  for(auto& th:ths){th.join();}};
 render(4);
 printf("frame 1 rendered\n");
 // Between frames: move the tents (topology fixed), then refit the tree.
 for(size_t i=tv0;i<tv1;i++){sc.G.ps[i].x+=.4;}
 sc.refitLBVH(4);
 if(!sc.checkLBVH()){fprintf(stderr,"structure check after refit failed\n"); return 1;}
 fill(D.begin(),D.end(),V());
 render(4);
 printf("frame 2 rendered (after refitLBVH)\n");
 FILE* f=fopen(v[1],"wb"); if(!f){perror(v[1]); return 1;}
 fprintf(f,"P6\n%d %d\n255\n",w,h);
 for(I i=0;i<w*h;i++){V c=D[i];
  for(I k=0;k<3;k++){F g=pow(min(max(c[k],F(0)),F(1)),F(1)/F(2.2));
   fputc(min(255,I(g*255.99)),f);}}
 fclose(f);
 printf("wrote %s\n",v[1]);
 return 0;
}
