// Task support added; production sources unmodified.
#include "our_gl.h"
#include "model.h"
#include <cstdlib>
#include <stdexcept>
#include <string>
extern mat<4,4> ModelView, Viewport, Perspective;
extern std::vector<double> zbuffer;
void ck(bool x,const char* why){if(!x)throw std::runtime_error(why);}
struct Solid:IShader{TGAColor c;bool discard;mutable unsigned calls=0;
 Solid(TGAColor c,bool d=false):c(c),discard(d){}
 std::pair<bool,TGAColor> fragment(const vec3) const override{++calls;return {discard,c};}};
int main(int argc,char**argv){try{
 std::string mode=argc>1?argv[1]:"";
 if(mode=="--prepare"){
  std::ofstream o("triangle.obj");o<<"v -0.8 -0.7 0\nv 0.8 -0.7 0\nv 0 0.8 0\nvt 0 0\nvt 1 0\nvt 0.5 1\nvn 0 0 1\nf 1/1/1 2/2/1 3/3/1\n";o.close();
  ck(TGAImage(2,2,TGAImage::RGB,{20,80,160,255}).write_tga_file("triangle_diffuse.tga"),"diffuse write");
  ck(TGAImage(2,2,TGAImage::RGB,{255,128,128,255}).write_tga_file("triangle_nm_tangent.tga"),"normal write");
  ck(TGAImage(2,2,TGAImage::RGB,{0,0,0,255}).write_tga_file("triangle_spec.tga"),"spec write");
  Model m("triangle.obj");ck(m.nfaces()==1&&m.nverts()==3,"OBJ model count");ck(m.diffuse().width()==2,"model texture");
  puts("LEGACY_MODEL_INPUT_OK vertices=3 faces=1 textures=3");return 0;
 }
 if(mode=="--image"){
  TGAImage im;ck(im.read_tga_file("framebuffer.tga"),"legacy output read");ck(im.width()==800&&im.height()==800,"legacy output dimensions");
  uint64_t hash=14695981039346656037ull;unsigned changed=0;
  for(int y=0;y<800;++y)for(int x=0;x<800;++x){auto c=im.get(x,y);if(c[0]!=177||c[1]!=195||c[2]!=209)++changed;for(int k=0;k<3;++k){hash^=c[k];hash*=1099511628211ull;}}
  ck(changed==134936 && hash==12385197142296575813ull,"legacy image differs from fixed normal path");
  std::cout<<"LEGACY_IMAGE_OK pixels="<<changed<<" fnv="<<hash<<"\n";
  return 0;
 }
 init_viewport(0,0,8,8);init_zbuffer(8,8);TGAImage im(8,8,TGAImage::RGB,{});
 Triangle t={vec4{-.75,-.75,.1,1},vec4{.75,-.75,.1,1},vec4{0,.75,.1,1}};
 Solid red({0,0,200,255});rasterize(t,red,im);ck(red.calls>0,"no raster calls");ck(im.get(4,3)[2]==200,"front draw");
 for(auto&v:t)v.z=.5;Solid blue({200,0,0,255});rasterize(t,blue,im);ck(im.get(4,3)[0]==200,"closer draw");
 for(auto&v:t)v.z=.8;Solid drop({0,200,0,255},true);rasterize(t,drop,im);ck(drop.calls>0&&im.get(4,3)[0]==200&&zbuffer[4+3*8]==.5,"discard commits");
 std::swap(t[0],t[1]);Solid back({0,200,0,255});rasterize(t,back,im);ck(back.calls==0,"backface cull");
 ck(im.write_tga_file("legacy-small.tga"),"small image write");puts("LEGACY_RASTER_OK depth=0.5 discard=preserved backface=culled");return 0;
 }catch(const std::exception&e){std::cerr<<"BASELINE_FAIL "<<e.what()<<"\n";return 1;}}
