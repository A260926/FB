#include "TriangleDrawer.h"
#include "ClipSpace.h"
#include <cstdio>
#include <limits>
#include <cmath>
using namespace al::graphics;
static unsigned draw(TriangleDrawer& d,ImageData& out,const Triangle& clip){
  auto pieces=ClipSpace::clipByAllPlanes(std::vector<Triangle>{clip});
  for(Triangle t:pieces){t.color=clip.color;t.perspectiveDivide();
    for(int i=0;i<3;++i)G3D::ndcToWindow(t[i].pos,out.width(),out.height());d.triangle(out,t);}
  return static_cast<unsigned>(pieces.size());
}
int main(){
 const u32 blue=ImageData::makePixel(0,0,255,255),red=ImageData::makePixel(255,0,0,255),blank=0;
 unsigned total=0;
 for(int a=0;a<4;++a){
  ImageData out(32,24),tex(2,2);out.fill(blank);tex.fill(red);tex.setPixel(0,0,0);
  ZBuffer z(32,24);z.fill(std::numeric_limits<double>::infinity());
  TriangleDrawer d(&z);d.algo=static_cast<TriangleDrawer::Algo>(a);d.lightsStyle=TriangleDrawer::LIGHTS_STYLE_NONE;
  Triangle far(Vertex4f(Vector4f(-2.8f,-2.8f,.2f,2.8f)),Vertex4f(Vector4f(2.8f,-2.8f,.2f,2.8f)),Vertex4f(Vector4f(0,2.8f,.2f,2.8f)));
  far.color=blue;d.drawMode=TriangleDrawer::DRAW_SOLID;draw(d,out,far);
  Triangle near(Vertex4f(Vector4f(-1.4f,-.8f,-.2f,1),Vector2f(0,0)),Vertex4f(Vector4f(.9f,-.8f,-.2f,1),Vector2f(1,0)),Vertex4f(Vector4f(0,1.5f,-.2f,2),Vector2f(.5f,1)));
  d.drawMode=TriangleDrawer::DRAW_PERSPECTIVE;d.textureImageData=&tex;
  unsigned pieces=draw(d,out,near),colored=0,redCount=0,blueCount=0;double minz=10,maxz=0;
  for(unsigned y=0;y<out.height();++y)for(unsigned x=0;x<out.width();++x){
    u32 p=out.getPixel(x,y);if(p){++colored;redCount+=p==red;blueCount+=p==blue;double v=z.unsafeGet(x,y);if(!std::isfinite(v)||v<=0)return 1;minz=std::min(minz,v);maxz=std::max(maxz,v);}
  }
  if(pieces<2||redCount<20||blueCount<20||minz>=2||maxz<2.7)return 2;
  total+=colored;
  std::printf("BASELINE algo=%d clipped_pieces=%u colored=%u near=%u far=%u depth_min=%.6f depth_max=%.6f\n",a,pieces,colored,redCount,blueCount,minz,maxz);
 }
 std::printf("BASELINE_COMPLETE algorithms=4 pixels=%u\n",total);return 0;
}
