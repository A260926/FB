#include "cgltf.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
int main(){
 const float positions[12]={0,0,0,91, 1,0,0,92, 0,1,0,93};
 const unsigned char si=1; const unsigned char pad[3]={0,0,0}; const float delta[3]={0,0,2};
 std::ofstream b(".task-build/probe.bin",std::ios::binary); b.write((const char*)positions,sizeof positions); b.write((const char*)&si,1); b.write((const char*)pad,3);b.write((const char*)delta,sizeof delta); b.close();
 const char* json=R"({"asset":{"version":"2.0"},"buffers":[{"uri":"probe.bin","byteLength":64}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48,"byteStride":16},{"buffer":0,"byteOffset":48,"byteLength":1},{"buffer":0,"byteOffset":52,"byteLength":12}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"componentType":5126,"count":3,"type":"VEC3","sparse":{"count":1,"indices":{"bufferView":1,"componentType":5121},"values":{"bufferView":2}}}],"materials":[{"name":"occluder"}],"meshes":[{"weights":[0.5],"primitives":[{"attributes":{"POSITION":0},"targets":[{"POSITION":1}],"material":0}]}],"nodes":[{"translation":[10,0,0],"children":[1]},{"mesh":0,"weights":[0]}],"scenes":[{"nodes":[0]}],"scene":0})";
 std::ofstream f(".task-build/probe.gltf");f<<json;f.close();
 cgltf_options opts={};cgltf_data* d=nullptr;
 if(cgltf_parse_file(&opts,".task-build/probe.gltf",&d)!=cgltf_result_success)return 1;
 if(cgltf_load_buffers(&opts,d,".task-build/probe.gltf")!=cgltf_result_success || cgltf_validate(d)!=cgltf_result_success){cgltf_free(d);return 2;}
 float p[9]={},m[9]={},w[16]={};
 bool ok=cgltf_accessor_unpack_floats(&d->accessors[0],p,9)==9 && cgltf_accessor_unpack_floats(&d->accessors[1],m,9)==9;
 cgltf_node_transform_world(&d->nodes[1],w);
 ok=ok && p[3]==1 && p[7]==1 && m[5]==2 && m[2]==0 && w[12]==10 && d->meshes[0].primitives[0].material==&d->materials[0] && d->nodes[1].weights[0]==0 && d->meshes[0].weights[0]==0.5f;
 if(ok)std::printf("CGLTF_PRODUCTION_OK stride_x=%.0f sparse_z=%.0f world_x=%.0f material=occluder explicit_weight=0\n",p[3],m[5],w[12]);
 cgltf_free(d);return ok?0:3;
}
