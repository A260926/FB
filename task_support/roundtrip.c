#include "cgltf.h"
#include "cgltf_write.h"
#include <stdio.h>
#include <math.h>
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"roundtrip:%d:%s\n",__LINE__,#x);return 2;}}while(0)
int main(void){
 cgltf_options o={0};cgltf_data *a=0,*b=0;
 REQUIRE(cgltf_parse_file(&o,"checks/motion.gltf",&a)==cgltf_result_success);
 REQUIRE(cgltf_load_buffers(&o,a,"checks/motion.gltf")==cgltf_result_success);
 REQUIRE(cgltf_validate(a)==cgltf_result_success);
 REQUIRE(cgltf_write_file(&o,".task-build/motion-roundtrip.gltf",a)==cgltf_result_success);
 REQUIRE(cgltf_parse_file(&o,".task-build/motion-roundtrip.gltf",&b)==cgltf_result_success);
 REQUIRE(cgltf_load_buffers(&o,b,".task-build/motion-roundtrip.gltf")==cgltf_result_success);
 REQUIRE(cgltf_validate(b)==cgltf_result_success);
 REQUIRE(b->animations_count==1 && b->animations[0].channels_count==2);
 cgltf_animation_channel *c=&b->animations[0].channels[0];
 REQUIRE(c->target_node==b->nodes+1 && c->sampler->input==b->accessors);
 float v[4];REQUIRE(cgltf_accessor_read_float(c->sampler->input,1,v,1));REQUIRE(v[0]==1);
 REQUIRE(cgltf_accessor_read_float(c->sampler->output,1,v,3));REQUIRE(v[0]==2);
 cgltf_free(a);REQUIRE(cgltf_accessor_read_float(c->sampler->output,1,v,3));REQUIRE(v[0]==2);
 cgltf_free(b);puts("CGLTF_ANIMATION_ROUNDTRIP_OK");return 0;
}
