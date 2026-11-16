#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
#include <math.h>
#include <assert.h>
struct P{double x,y,z;};
static P sub(P a,P b){P r={a.x-b.x,a.y-b.y,a.z-b.z};return r;}
static double dot(P a,P b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static double len(P a){return sqrt(dot(a,a));}
static double ptTri(P p,P a,P b,P c){
  P ab=sub(b,a),ac=sub(c,a),ap=sub(p,a);double d1=dot(ab,ap),d2=dot(ac,ap);
  if(d1<=0&&d2<=0)return len(ap);
  P bp=sub(p,b);double d3=dot(ab,bp),d4=dot(ac,bp);if(d3>=0&&d4<=d3)return len(bp);
  double vc=d1*d4-d3*d2;if(vc<=0&&d1>=0&&d3<=0){double v=d1/(d1-d3);P q={a.x+ab.x*v,a.y+ab.y*v,a.z+ab.z*v};return len(sub(p,q));}
  P cp=sub(p,c);double d5=dot(ab,cp),d6=dot(ac,cp);if(d6>=0&&d5<=d6)return len(cp);
  double vb=d5*d2-d1*d6;if(vb<=0&&d2>=0&&d6<=0){double w=d2/(d2-d6);P q={a.x+ac.x*w,a.y+ac.y*w,a.z+ac.z*w};return len(sub(p,q));}
  double va=d3*d6-d5*d4;if(va<=0&&(d4-d3)>=0&&(d5-d6)>=0){double w=(d4-d3)/((d4-d3)+(d5-d6));P bc=sub(c,b);P q={b.x+bc.x*w,b.y+bc.y*w,b.z+bc.z*w};return len(sub(p,q));}
  double den=va+vb+vc;if(den==0)return 1e30;double v=vb/den,w=vc/den;P q={a.x+ab.x*v+ac.x*w,a.y+ab.y*v+ac.y*w,a.z+ab.z*v+ac.z*w};return len(sub(p,q));
}
static double directed(const std::vector<float>&p,const std::vector<unsigned>&ai,const std::vector<float>&q,const std::vector<unsigned>&bi,int S){
  double worst=0;
  for(size_t f=0;f<ai.size();f+=3){
    P p0={p[ai[f]*3],p[ai[f]*3+1],p[ai[f]*3+2]},p1={p[ai[f+1]*3],p[ai[f+1]*3+1],p[ai[f+1]*3+2]},p2={p[ai[f+2]*3],p[ai[f+2]*3+1],p[ai[f+2]*3+2]};
    for(int i=0;i<=S;++i)for(int j=0;i+j<=S;++j){
      double u=double(i)/S,v=double(j)/S,w=1-u-v;
      P pp={p0.x*w+p1.x*u+p2.x*v,p0.y*w+p1.y*u+p2.y*v,p0.z*w+p1.z*u+p2.z*v};
      double best=1e30;
      for(size_t g=0;g<bi.size();g+=3){P q0={q[bi[g]*3],q[bi[g]*3+1],q[bi[g]*3+2]},q1={q[bi[g+1]*3],q[bi[g+1]*3+1],q[bi[g+1]*3+2]},q2={q[bi[g+2]*3],q[bi[g+2]*3+1],q[bi[g+2]*3+2]};double d=ptTri(pp,q0,q1,q2);best=best<d?best:d;}
      worst=worst<best?best:worst;
    }
  }
  return worst;
}
int main(int argc,char**argv){
  unsigned n=argc>1?atoi(argv[1]):10; float amp=argc>2?atof(argv[2]):0.3f; float mse=argc>3?atof(argv[3]):0.02f; unsigned depth=argc>4?atoi(argv[4]):16; size_t target=argc>5?atoi(argv[5]):0;
  std::vector<float> pos,attr; std::vector<unsigned> idx;
  for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i)/n,y=float(j)/n;float z=amp*sinf(x*3.14159f)*sinf(y*3.14159f);pos.push_back(x);pos.push_back(y);pos.push_back(z);attr.push_back(x);attr.push_back(y);}
  for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,d=c+1;unsigned f[]={a,b,d,a,d,c};idx.insert(idx.end(),f,f+6);}
  std::vector<unsigned> out(idx.size());float w[2]={0.1f,0.1f};float err=-1;meshopt_SurfaceErrorStats st={};
  size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,target,0.5f,0,&err,mse,depth,&st);
  double d0=directed(pos,idx,pos,std::vector<unsigned>(out.begin(),out.begin()+c),12);
  double d1=directed(pos,std::vector<unsigned>(out.begin(),out.begin()+c),pos,idx,12);
  printf("n=%u amp=%g mse=%g depth=%u target=%zu in=%zu out=%zu cert=%g refA=%.6f refB=%.6f acc=%zu rejD=%zu rejB=%zu\n",n,amp,mse,depth,target,idx.size(),c,st.certified_error,d0,d1,st.accepted_collapses,st.rejected_distance,st.rejected_budget);
  if(d0>st.certified_error+1e-4||d1>st.certified_error+1e-4){printf("VIOLATION d0=%g d1=%g cert=%g\n",d0,d1,st.certified_error);return 1;}
  return 0;
}
