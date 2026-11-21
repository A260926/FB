// GRIN preview example: renders the production fixture scene twice through
// the production traceGrin - once with k=0 (straight) and once with an
// exponential index layer (bent) - and writes straight.ppm / bent.ppm.
#include "minpt_scene.h"
#include "minpt_grin.h"
I main(I ac,C**v){
 string out=ac>1?v[1]:".";
 string scenePath=ac>2?v[2]:"task_support/fixtures/scene.obj";
 Scene sc; sc.load(scenePath,"",0); sc.build();
 I w=64,h=64,ns=16,mb=4;
 sc.E.initE("",V(0,0,0),V(0,0,-1),V(0,1,0),60,1,0,1,F(w)/h);
 GrinConfig straight{0.25,1.25,1.0,0.0,8.0};   // uniform: straight rays
 GrinConfig bent{0.25,1.25,1.0,1.0,8.0};       // exponential layer: bent rays
 auto render=[&](const GrinConfig&c,vector<V>&D){
  D.assign(w*h,V());
  for(I i=0;i<w*h;i++){Rng rn(7+I(i));
   for(I j=0;j<ns;j++)
    D[i]=D[i]+traceGrin(sc,rn,V((i%w+rn.u())/w,(i/w+rn.u())/h,0),c,mb)/ns;}};
 auto ppm=[&](const string& p,const vector<V>&D){
  FILE* f=fopen(p.c_str(),"wb");
  fprintf(f,"P6\n%d %d\n255\n",w,h);
  for(I y=0;y<h;y++)for(I x=0;x<w;x++){V c=D[(h-1-y)*w+x];
   for(I i=0;i<3;i++)fputc(int(pow(clamp(c[i],0.0,1.0),1/2.2)*255.99),f);}
  fclose(f);};
 vector<V> A,B; render(straight,A); render(bent,B);
 ppm(out+"/straight.ppm",A); ppm(out+"/bent.ppm",B);
 printf("wrote %s/straight.ppm and %s/bent.ppm\n",out.c_str(),out.c_str());
 return 0;}
