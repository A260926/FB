#pragma once
#include <cmath>
#include <algorithm>
namespace render {
struct V2 { double x=0,y=0; };
struct V3 { double x=0,y=0,z=0; };
inline V2 operator+(V2 a,V2 b){return {a.x+b.x,a.y+b.y};}
inline V2 operator-(V2 a,V2 b){return {a.x-b.x,a.y-b.y};}
inline V2 operator*(V2 a,double b){return {a.x*b,a.y*b};}
inline V3 operator+(V3 a,V3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline V3 operator-(V3 a,V3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline V3 operator*(V3 a,double b){return {a.x*b,a.y*b,a.z*b};}
inline V3 operator/(V3 a,double b){return a*(1/b);}
inline double dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline V3 cross(V3 a,V3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline double length(V3 a){return std::sqrt(dot(a,a));}
inline V3 normalized(V3 a){double n=length(a);return n>1e-14?a/n:V3{};}
inline bool finite(V3 a){return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z);}
inline double edge(V2 a,V2 b,V2 p){return (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x);}
}
