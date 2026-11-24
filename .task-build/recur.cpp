#include "fixture.hpp"
#include <iostream>
// Build a module with a self-recursive function indexing a sampler array.
using namespace spirv_cross;
int main(){
  Module b;
  auto V=b.id(),U=b.id(),F=b.id(),V2=b.id(),V4=b.id(),I=b.id(),S=b.id(),IS=b.id();
  auto C0=b.id(),C1=b.id(),CM=b.id(),F0=b.id(),UV=b.id(),SA=b.id(),PSA=b.id();
  auto PS=b.id(),PU=b.id(),images=b.id(),samplers=b.id(),main=b.id(),rec=b.id(),FT=b.id(),FR=b.id();
  auto out=b.id(),PO=b.id();
  b.op(b.pre,spv::OpCapability,{spv::CapabilityShader});
  b.op(b.pre,spv::OpCapability,{spv::CapabilitySampledImageArrayDynamicIndexing});
  b.op(b.pre,spv::OpMemoryModel,{spv::AddressingModelLogical,spv::MemoryModelGLSL450});
  b.op(b.pre,spv::OpEntryPoint,{spv::ExecutionModelFragment,main},"main",{out});
  b.op(b.pre,spv::OpExecutionMode,{main,spv::ExecutionModeOriginUpperLeft});
  b.op(b.types,spv::OpTypeVoid,{V});
  b.op(b.types,spv::OpTypeInt,{U,32,0});
  b.op(b.types,spv::OpTypeFloat,{F,32});
  b.op(b.types,spv::OpTypeVector,{V2,F,2});
  b.op(b.types,spv::OpTypeVector,{V4,F,4});
  b.op(b.types,spv::OpTypeImage,{I,F,spv::Dim2D,0,0,0,1,spv::ImageFormatUnknown});
  b.op(b.types,spv::OpTypeSampler,{S});
  b.op(b.types,spv::OpTypeSampledImage,{IS,I});
  b.op(b.types,spv::OpConstant,{U,C0,0});b.op(b.types,spv::OpConstant,{U,C1,1});b.op(b.types,spv::OpConstant,{U,CM,3});
  b.op(b.types,spv::OpConstant,{F,F0,0});
  b.op(b.types,spv::OpConstantComposite,{V2,UV,F0,F0});
  b.op(b.types,spv::OpTypeArray,{SA,S,CM});
  b.op(b.types,spv::OpTypePointer,{PS,spv::StorageClassUniformConstant,S});
  b.op(b.types,spv::OpTypePointer,{PSA,spv::StorageClassUniformConstant,SA});
  b.op(b.types,spv::OpTypePointer,{PU,spv::StorageClassFunction,U});
  b.op(b.types,spv::OpTypePointer,{PO,spv::StorageClassOutput,V4});
  b.op(b.types,spv::OpVariable,{PSA,samplers,spv::StorageClassUniformConstant});
  // scalar image global
  auto PI=b.id();
  b.op(b.types,spv::OpTypePointer,{PI,spv::StorageClassUniformConstant,I});
  b.op(b.types,spv::OpVariable,{PI,images,spv::StorageClassUniformConstant});
  b.op(b.types,spv::OpVariable,{PO,out,spv::StorageClassOutput});
  b.op(b.types,spv::OpTypeFunction,{FT,V});
  b.op(b.types,spv::OpTypeFunction,{FR,V4,PSA,U,I});
  // recursive function rec(samplerArr ptr, idx, img)
  auto ws=b.id(),sx=b.id(),li=b.id(),rcond=b.id(),lbl0=b.id(),lbl1=b.id(),merge=b.id();
  b.op(b.funcs,spv::OpFunction,{V4,rec,0,FR});
  b.op(b.funcs,spv::OpFunctionParameter,{PSA,ws});
  b.op(b.funcs,spv::OpFunctionParameter,{U,sx});
  b.op(b.funcs,spv::OpFunctionParameter,{I,li});
  b.op(b.funcs,spv::OpLabel,{lbl0});
  auto condvar=b.id();
  b.op(b.funcs,spv::OpVariable,{PU,condvar,spv::StorageClassFunction});
  b.op(b.funcs,spv::OpStore,{condvar,sx});
  auto cv=b.id(),zero=b.id();
  b.op(b.funcs,spv::OpAccessChain,{PU,zero,condvar,C0});
  auto cvload=b.id();
  b.op(b.funcs,spv::OpLoad,{U,cvload,zero});
  b.op(b.funcs,spv::OpIEqual,{rcond,cvload,C0});
  b.op(b.funcs,spv::OpBranchConditional,{rcond,lbl1,merge});
  // base case: sample
  b.op(b.funcs,spv::OpLabel,{lbl1});
  auto sp=b.id(),sl=b.id(),cimg=b.id(),val=b.id();
  b.op(b.funcs,spv::OpAccessChain,{PS,sp,ws,cvload});
  b.op(b.funcs,spv::OpLoad,{S,sl,sp});
  b.op(b.funcs,spv::OpSampledImage,{IS,cimg,li,sl});
  b.op(b.funcs,spv::OpImageSampleExplicitLod,{V4,val,cimg,UV,spv::ImageOperandsLodMask,F0});
  b.op(b.funcs,spv::OpReturnValue,{val});
  // recursive case
  b.op(b.funcs,spv::OpLabel,{merge});
  auto sub=b.id(),rv=b.id();
  b.op(b.funcs,spv::OpISub,{U,sub,cvload,C1});
  b.op(b.funcs,spv::OpFunctionCall,{V4,rv,rec,ws,sub,li});
  b.op(b.funcs,spv::OpReturnValue,{rv});
  b.op(b.funcs,spv::OpFunctionEnd,{});
  // main
  auto ml=b.id();
  b.op(b.funcs,spv::OpFunction,{V,main,0,FT});
  b.op(b.funcs,spv::OpLabel,{ml});
  auto iload=b.id(),res=b.id();
  b.op(b.funcs,spv::OpLoad,{I,iload,images});
  b.op(b.funcs,spv::OpFunctionCall,{V4,res,rec,samplers,C1,iload});
  b.op(b.funcs,spv::OpStore,{out,res});
  b.op(b.funcs,spv::OpReturn,{});
  b.op(b.funcs,spv::OpFunctionEnd,{});
  try{
    CompilerGLSL c(b.words); c.set_common_options(options());
    c.build_combined_image_samplers();
    std::cout<<c.compile()<<"\nRECURSION NO THROW (bad)\n";
  }catch(const CompilerError& e){ std::cout<<"RECURSION THREW OK: "<<e.what()<<"\n"; }
  return 0;
}
