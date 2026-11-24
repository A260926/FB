
#include <iostream>
#include <cmath>
#include <vector>
using uint=unsigned;
struct vec2 {float x,y;explicit vec2(float f):x(f),y(f){} vec2(float a,float b):x(a),y(b){}};
struct vec4 {float x,y,z,w;vec4():x(0),y(0),z(0),w(0){} vec4(float a,float b,float c,float d):x(a),y(b),z(c),w(d){}};
struct sampler2D {unsigned image,sampler;};
struct Event {unsigned image,sampler;float x,y,lod;};
std::vector<Event> events;
vec4 textureLod(sampler2D s,vec2 uv,float lod) {
 events.push_back({s.image,s.sampler,uv.x,uv.y,lod});
 return vec4(float(19*s.image+3*s.sampler+1),float(s.image+2),float(s.sampler+5),uv.x+uv.y+lod);
}

struct Control
{
    uint image_index;
    uint sampler_index;
} control;

sampler2D paired[6];

vec4 out_color;

vec4 leaf(sampler2D SPIRV_Cross_Combined_34_35)
{
    return textureLod(SPIRV_Cross_Combined_34_35, vec2(0.0), 0.0);
}

vec4 wrapper(uint _41, uint _42, sampler2D SPIRV_Cross_Combined_39_40[6])
{
    return leaf(SPIRV_Cross_Combined_39_40[(_41 * 2 + _42)]);
}

void shader_main()
{
    out_color = wrapper(control.image_index, control.sampler_index, paired);
}


int main(){
sampler2D units[4096]; for(auto& u:units)u={9999,9999};
units[7]={0,0};
units[8]={0,1};
units[9]={1,0};
units[10]={1,1};
units[11]={2,0};
units[12]={2,1};
paired[0]=units[7];
paired[1]=units[8];
paired[2]=units[9];
paired[3]=units[10];
paired[4]=units[11];
paired[5]=units[12];
unsigned checked=0; for(unsigned i=0;i<3;++i)for(unsigned j=0;j<2;++j){
control.image_index=i;control.sampler_index=j;events.clear();shader_main();
if(events.size()!=1 || events[0].image!=i || events[0].sampler!=j || events[0].x!=0 || events[0].y!=0 || events[0].lod!=0) return 11;
if(out_color.x!=float(19*i+3*j+1)||out_color.y!=float(i+2)||out_color.z!=float(j+5)||out_color.w!=0) return 12; ++checked; }
std::cout<<"CPU_PAIR_MODEL_OK pairs="<<checked<<"\n";}
