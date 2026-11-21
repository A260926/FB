#include "minpt_scene.h"
#include "minpt_grin.h"
I main(I ac,C**v){Scene sc; puts(v[1]); sc.load(v[1],v[2],atof(v[7])); sc.build();
 I ns=atoi(v[5]),mb=atoi(v[6]),w=atoi(v[8]),h=atoi(v[9]),sz=w*h*3;
 vector<V> D(w*h,V()); sc.E.initE(v[3],V(atof(v[10]),atof(v[11]),atof(v[12])),
  V(atof(v[13]),atof(v[14]),atof(v[15])),V(0,1,0),F(atof(v[16])),
  F(atof(v[17])),F(atof(v[18])),F(atof(v[19])),F(w)/h);
 bool gr=ac>20; GrinConfig cfg{};
 if(gr){if(ac!=26||strcmp(v[20],"--grin")){fprintf(stderr,
   "usage: minpt <19 args> [--grin y0 y1 n0 k maxArc]\n"); return 1;}
  cfg=GrinConfig{atof(v[21]),atof(v[22]),atof(v[23]),atof(v[24]),atof(v[25])};
  try{makeGrinPath(cfg,{V(0),V(0,1,0)});}catch(const invalid_argument& e){
   fprintf(stderr,"invalid --grin arguments: %s\n",e.what()); return 1;}}
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
 #pragma omp parallel for schedule(dynamic,1)
 for(I i=0;i<w*h;i++){thread_local Rng rn(42+omp_get_thread_num());
  for(I j=0;j<ns;j++){V uv((i%w+rn.u())/w,(i/w+rn.u())/h,0);
   D[i]=D[i]+(gr?traceGrin(sc,rn,uv,cfg,mb):L(rn,V(uv)))/ns;}}
 FILE* f=fopen(v[4],"wb"); fprintf(f, "PF\n%d %d\n-1\n",w,h);
 vector<float> d(sz); for(I y=0;y<h;y++) for(I x=0;x<w;x++) for(I i=0;i<3;i++){
  d[3*(y*w+x)+i]=float(D[(h-1-y)*w+(w-1-x)][i]);}
 fwrite(d.data(),4,d.size(),f); fclose(f); return 0;}
