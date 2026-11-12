#include "frame.hpp"
#include <stdexcept>
namespace render {
Frame draw(const std::vector<Triangle>& scene,const Camera& current,const Camera& previous,
 int width,int height,V2 jitter,double exposure,uint64_t sequence){
 if(!std::isfinite(exposure)||exposure<=0)throw std::invalid_argument("invalid exposure");
 Frame frame(width,height);frame.jitter=jitter;frame.exposure=exposure;frame.sequence=sequence;
 std::fill(frame.hdr.color.begin(),frame.hdr.color.end(),V3{.02,.03,.05}*exposure);
 for(const Triangle& t:scene){
  if(t.object==0)throw std::invalid_argument("object zero reserved for background");
  V3 vertices[]={t.a,t.b,t.c};Projection screen[3];
  bool valid=true;
  for(int k=0;k<3;k++){
   screen[k]=current.project(vertices[k]+t.offset,width,height,jitter);
   valid=valid&&screen[k].valid;
  }
  // This seed accepts triangles wholly in front of the near plane.
  if(!valid)continue;
  double area=edge(screen[0].pixel,screen[1].pixel,screen[2].pixel);
  if(std::abs(area)<1e-12)continue;
  V3 normal=normalized(cross(t.b-t.a,t.c-t.a));
  double lambert=.15+.85*std::abs(dot(normal,normalized(V3{.3,.5,-1})));
  double minx=width,maxx=0,miny=height,maxy=0;
  for(auto s:screen){minx=std::min(minx,s.pixel.x);maxx=std::max(maxx,s.pixel.x);
   miny=std::min(miny,s.pixel.y);maxy=std::max(maxy,s.pixel.y);}
  int x0=std::max(0,int(std::floor(minx))),x1=std::min(width-1,int(std::ceil(maxx)));
  int y0=std::max(0,int(std::floor(miny))),y1=std::min(height-1,int(std::ceil(maxy)));
  for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++){
   V2 p{double(x)+.5,double(y)+.5};
   double w[]={edge(screen[1].pixel,screen[2].pixel,p)/area,
               edge(screen[2].pixel,screen[0].pixel,p)/area,
               edge(screen[0].pixel,screen[1].pixel,p)/area};
   if(w[0]<-1e-12||w[1]<-1e-12||w[2]<-1e-12)continue;
   double invz=0;for(int k=0;k<3;k++)invz+=w[k]/screen[k].depth;
   double z=1/invz;size_t i=size_t(y)*width+x;
   if(z>=frame.depth[i])continue;
   V3 local{};for(int k=0;k<3;k++)local=local+vertices[k]*(w[k]/screen[k].depth/invz);
   Projection old=previous.project(local+t.previous_offset,width,height);
   Projection now=current.project(local+t.offset,width,height);
   int stripe=int(std::floor(local.x*12)+std::floor(local.y*12));
   double texture=(stripe%2==0)?1.:.25;
   frame.hdr.color[i]=t.albedo*(lambert*texture*exposure);
   frame.depth[i]=z;frame.previous_depth[i]=old.valid?old.depth:0;
   frame.motion[i]=old.valid?old.pixel-now.pixel:V2{double(width)*2,0};
   frame.object[i]=t.object;
  }
 }
 return frame;
}
}
