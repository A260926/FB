#include "cgltf.h"
#include "cgltf_write.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <sstream>
#include <cstdint>
static std::string base64(const std::vector<unsigned char>& bytes) {
    const char* a="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for(size_t i=0;i<bytes.size();i+=3) {
        unsigned x=unsigned(bytes[i])<<16;
        if(i+1<bytes.size())x|=unsigned(bytes[i+1])<<8;
        if(i+2<bytes.size())x|=bytes[i+2];
        s+=a[(x>>18)&63];s+=a[(x>>12)&63];s+=i+1<bytes.size()?a[(x>>6)&63]:'=';s+=i+2<bytes.size()?a[x&63]:'=';
    }return s;
}
template<class T>static void append(std::vector<unsigned char>& b,const T& x) {
    auto p=reinterpret_cast<const unsigned char*>(&x);b.insert(b.end(),p,p+sizeof(x));
}
static bool observe(cgltf_data* d,double& z) {
    if(cgltf_validate(d)!=cgltf_result_success||d->meshes_count!=1||d->animations_count!=1)return false;
    const auto& p=d->meshes[0].primitives[0];
    if(p.attributes_count!=1||p.targets_count!=1||d->nodes_count!=2)return false;
    float base[9],delta[9],w[2],times[2],m[16];
    if(cgltf_accessor_unpack_floats(p.attributes[0].data,base,9)!=9||
       cgltf_accessor_unpack_floats(p.targets[0].attributes[0].data,delta,9)!=9)return false;
    const auto& s=d->animations[0].samplers[0];
    if(cgltf_accessor_unpack_floats(s.input,times,2)!=2||cgltf_accessor_unpack_floats(s.output,w,2)!=2)return false;
    if(times[0]!=0||times[1]!=2||w[0]!=0||w[1]!=.75F||delta[5]!=2||delta[0]!=0||base[3]!=1)return false;
    cgltf_node_transform_world(d->nodes+1,m);
    if(m[12]!=3||m[13]!=4||m[14]!=0)return false;
    z=base[5]+w[1]*delta[5]+m[14];
    return std::fabs(z-1.5)<1.e-7;
}
int main(int argc,char** argv) {
    if(argc!=2)return 1;
    const float pos[3][4]={{0,0,0,77},{1,0,0,77},{0,1,0,77}};
    std::vector<unsigned char>b;
    for(const auto& p:pos)append(b,p);
    uint32_t sparse_index=1;append(b,sparse_index);
    float delta[3]={0,0,2};append(b,delta);
    float times[2]={0,2},weights[2]={0,.75F};append(b,times);append(b,weights);
    if(b.size()!=80)return 2;
    std::ostringstream out;
    out<<R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":80,"uri":"data:application/octet-stream;base64,)"<<base64(b)<<R"("}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48,"byteStride":16},{"buffer":0,"byteOffset":48,"byteLength":4},{"buffer":0,"byteOffset":52,"byteLength":12},{"buffer":0,"byteOffset":64,"byteLength":8},{"buffer":0,"byteOffset":72,"byteLength":8}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"componentType":5126,"count":3,"type":"VEC3","sparse":{"count":1,"indices":{"bufferView":1,"componentType":5125},"values":{"bufferView":2}}},{"bufferView":3,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[2]},{"bufferView":4,"componentType":5126,"count":2,"type":"SCALAR"}],"meshes":[{"weights":[0],"primitives":[{"attributes":{"POSITION":0},"targets":[{"POSITION":1}]}]}],"nodes":[{"translation":[3,4,0],"children":[1]},{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0,"animations":[{"samplers":[{"input":2,"output":3,"interpolation":"LINEAR"}],"channels":[{"sampler":0,"target":{"node":1,"path":"weights"}}]}]})";
    std::string json=out.str();cgltf_options options={};cgltf_data*d=nullptr;
    if(cgltf_parse(&options,json.data(),json.size(),&d)!=cgltf_result_success)return 3;
    if(cgltf_load_buffers(&options,d,nullptr)!=cgltf_result_success)return 4;
    double z=0;if(!observe(d,z))return 5;
    if(cgltf_write_file(&options,argv[1],d)!=cgltf_result_success)return 6;
    cgltf_free(d);d=nullptr;
    if(cgltf_parse_file(&options,argv[1],&d)!=cgltf_result_success)return 7;
    if(cgltf_load_buffers(&options,d,argv[1])!=cgltf_result_success)return 8;
    double z2=0;if(!observe(d,z2)||z!=z2)return 9;
    cgltf_free(d);
    std::printf("CGLTF_BASELINE_OK stride=16 sparse=1 morph_z=%.6f world_offset=3,4,0 roundtrip=1\n",z2);
    return 0;
}
