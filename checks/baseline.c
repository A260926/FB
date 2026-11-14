#define CGLTF_IMPLEMENTATION
#include "../cgltf.h"
#include <assert.h>
#include <math.h>
int main(void) {
  cgltf_options options = {0}; cgltf_data* data = 0;
  assert(cgltf_parse_file(&options,"checks/motion.gltf",&data)==cgltf_result_success);
  assert(cgltf_load_buffers(&options,data,"checks/motion.gltf")==cgltf_result_success);
  assert(cgltf_validate(data)==cgltf_result_success);
  assert(data->animations_count==1 && data->nodes_count==3);
  const cgltf_animation* a=&data->animations[0]; float values[6], m[16];
  assert(a->channels_count==2 && a->samplers_count==2);
  assert(cgltf_accessor_unpack_floats(a->samplers[0].output,values,6)==6);
  assert(values[0]==0 && values[3]==2 && values[4]==0);
  cgltf_node_transform_world(&data->nodes[2],m);
  assert(fabsf(m[12]-4)<1e-6f && fabsf(m[13]-1)<1e-6f);
  puts("CGLTF_BASELINE_OK channels=2 hierarchy=3");
  cgltf_free(data);return 0;
}
