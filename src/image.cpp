#include "frame.hpp"
#include <stdexcept>
#include <limits>
#include <fstream>
namespace render {
Image::Image(int w,int h,V3 clear):width(w),height(h){
 if(w<=0||h<=0||w>16384||h>16384)throw std::invalid_argument("invalid image size");
 color.assign(size_t(w)*h,clear);
}
V3& Image::at(int x,int y){
 if(x<0||y<0||x>=width||y>=height)throw std::out_of_range("image index");
 return color[size_t(y)*width+x];
}
V3 Image::at(int x,int y)const{
 if(x<0||y<0||x>=width||y>=height)throw std::out_of_range("image index");
 return color[size_t(y)*width+x];
}
V3 Image::bilinear(V2 pixel)const{
 double x=std::clamp(pixel.x-.5,0.,double(width-1));
 double y=std::clamp(pixel.y-.5,0.,double(height-1));
 int x0=int(std::floor(x)),y0=int(std::floor(y));
 int x1=std::min(x0+1,width-1),y1=std::min(y0+1,height-1);
 double fx=x-x0,fy=y-y0;
 return at(x0,y0)*((1-fx)*(1-fy))+at(x1,y0)*(fx*(1-fy))+
        at(x0,y1)*((1-fx)*fy)+at(x1,y1)*(fx*fy);
}
void Image::ppm(const std::string& path)const{
 std::ofstream file(path,std::ios::binary);
 if(!file)throw std::runtime_error("cannot open image");
 file<<"P6\n"<<width<<" "<<height<<"\n255\n";
 for(V3 c:color){
  if(!finite(c))throw std::runtime_error("nonfinite image color");
  unsigned char rgb[3];double channels[]={c.x,c.y,c.z};
  for(int k=0;k<3;k++){
   double linear=std::max(0.,channels[k]);
   double mapped=linear/(1+linear);
   double srgb=mapped<=.0031308?12.92*mapped:1.055*std::pow(mapped,1/2.4)-.055;
   rgb[k]=static_cast<unsigned char>(std::lround(std::clamp(srgb,0.,1.)*255));
  }
  file.write(reinterpret_cast<const char*>(rgb),3);
 }
 if(!file)throw std::runtime_error("cannot write image");
}
Frame::Frame(int w,int h):hdr(w,h){
 const size_t n=size_t(w)*h;
 depth.assign(n,std::numeric_limits<double>::infinity());
 previous_depth=depth;motion.assign(n,{});object.assign(n,0);
}
}
