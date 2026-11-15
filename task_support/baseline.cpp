#include <scimesh/renderer.h>
#include <scimesh/scene.h>
#include <scimesh/mesh.h>
#include <scimesh/camera.h>
#include <scimesh/render_options.h>
#include <iostream>
#include <cstdint>
int main(){
 using namespace scimesh;
 Mesh m; m.vertices={{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}};
 m.triangles={{0,1,2},{0,2,3}};m.uvs={{0,1},{1,1},{1,0},{0,0}};
 m.colors.assign(4,Color(1,1,1,1));m.normals.assign(4,Vec3(0,0,1));m.texture=Image(8,8);
 for(int y=0;y<8;++y)for(int x=0;x<8;++x)m.texture.set_pixel(x,y,32+x*25,32+y*25,160,255);
 Scene s;s.add(m);Camera cam;cam.eye=Vec3(0,0,4);cam.center=Vec3(0,0,0);cam.up=Vec3(0,1,0);
 RenderOptions o;o.width=48;o.height=48;o.threads=1;o.background_color=Color(0,0,0,1);o.backface_culling=false;
 Image im=Renderer().render_scene(s,cam,o);
 unsigned count=0;uint64_t hash=1469598103934665603ULL;
 for(int y=0;y<48;++y)for(int x=0;x<48;++x){uint8_t r,g,b,a;im.get_pixel(x,y,r,g,b,a);if(r||g||b)++count;for(unsigned v:{unsigned(r),unsigned(g),unsigned(b),unsigned(a)}){hash^=v;hash*=1099511628211ULL;}}
 uint8_t r,g,b,a;im.get_pixel(24,24,r,g,b,a);
 if(count<100 || !r || !g || !b || a!=255 || im.width!=48 || im.height!=48)return 1;
 std::cout<<"DENSE_RENDER size="<<im.width<<"x"<<im.height<<" colored="<<count<<" hash="<<hash<<" center="<<unsigned(r)<<","<<unsigned(g)<<","<<unsigned(b)<<","<<unsigned(a)<<"\n";
 std::cout<<"BASELINE_COMPLETE dense_texture_production\n";
}
