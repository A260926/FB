#include "spirv_cross.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>
int main() {
 std::vector<uint32_t> w={0x07230203,0x00010000,0,32,0};
 auto op=[&](spv::Op o,std::initializer_list<uint32_t> a){w.push_back((uint32_t(a.size()+1)<<16)|o);w.insert(w.end(),a);};
 op(spv::OpCapability,{spv::CapabilityShader}); op(spv::OpMemoryModel,{0,1});
 op(spv::OpEntryPoint,{spv::ExecutionModelFragment,20,0x6e69616d,0}); op(spv::OpExecutionMode,{20,spv::ExecutionModeOriginUpperLeft});
 op(spv::OpDecorate,{6,spv::DecorationArrayStride,16}); op(spv::OpDecorate,{7,spv::DecorationBlock});
 op(spv::OpMemberDecorate,{7,0,spv::DecorationOffset,0}); op(spv::OpMemberDecorate,{7,1,spv::DecorationOffset,64});
 op(spv::OpDecorate,{10,spv::DecorationDescriptorSet,0}); op(spv::OpDecorate,{10,spv::DecorationBinding,0});
 op(spv::OpTypeVoid,{1}); op(spv::OpTypeFunction,{2,1}); op(spv::OpTypeFloat,{3,32});op(spv::OpTypeInt,{4,32,0});
 op(spv::OpConstant,{4,5,4});op(spv::OpTypeArray,{6,3,5});op(spv::OpTypeStruct,{7,6,3});
 op(spv::OpTypePointer,{8,spv::StorageClassUniform,7});op(spv::OpTypePointer,{9,spv::StorageClassUniform,3});
 op(spv::OpVariable,{8,10,spv::StorageClassUniform});op(spv::OpConstant,{4,11,0});op(spv::OpConstant,{4,12,2});
 op(spv::OpFunction,{1,20,0,2});op(spv::OpLabel,{21});op(spv::OpAccessChain,{9,22,10,11,12});op(spv::OpLoad,{3,23,22});op(spv::OpReturn,{});op(spv::OpFunctionEnd,{});
 spirv_cross::Compiler c(w);auto r=c.get_active_buffer_ranges(10);
 assert(r.size()==1 && r[0].index==0 && r[0].offset==0 && r[0].range==64);
 assert(c.get_declared_struct_size(c.get_type(7))==68);
 std::printf("TOP_MEMBER offset=%zu range=%zu struct=68\n",r[0].offset,r[0].range);std::puts("BASELINE_OK");
}
