#ifndef PAIR_FIXTURE_HPP
#define PAIR_FIXTURE_HPP
#include "spirv_glsl.hpp"
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

// Small logical Shader modules, built without an assembler or an SDK.
namespace pair_fixture {
struct Module {
    uint32_t next = 1;
    std::vector<uint32_t> pre, debug, ann, types, funcs;
    uint32_t id() { return next++; }
    void op(std::vector<uint32_t>& v, spv::Op code, std::initializer_list<uint32_t> a) {
        v.push_back((uint32_t(a.size()+1)<<16)|uint32_t(code)); v.insert(v.end(),a);
    }
    void text(std::vector<uint32_t>& v, spv::Op code, std::vector<uint32_t> a, const char* s,
              std::initializer_list<uint32_t> tail = {}) {
        size_t n = (std::strlen(s)+4)/4, off=a.size(); a.resize(off+n,0);
        std::memcpy(a.data()+off,s,std::strlen(s)); a.insert(a.end(),tail);
        v.push_back((uint32_t(a.size()+1)<<16)|uint32_t(code));v.insert(v.end(),a.begin(),a.end());
    }
    void name(uint32_t x,const char* n) { text(debug,spv::OpName,{x},n); }
    std::vector<uint32_t> words() {
        std::vector<uint32_t> r={0x07230203,0x00010000,0,next,0};
        for(auto p:{&pre,&debug,&ann,&types,&funcs}) r.insert(r.end(),p->begin(),p->end());return r;
    }
};
struct Fixture {
    std::vector<uint32_t> words;
    uint32_t images, samplers;
    unsigned n, m;
};
inline Fixture make(unsigned n,unsigned m,bool image_array,bool sampler_array,bool helpers=true) {
    Module b;
    auto V=b.id(), U=b.id(), F=b.id(), V2=b.id(), V4=b.id(), I=b.id(), S=b.id(), IS=b.id();
    auto C0=b.id(), CN=b.id(), CM=b.id(), F0=b.id(), UV=b.id(), IA=b.id(), SA=b.id();
    auto PI=b.id(), PS=b.id(), PIA=b.id(), PSA=b.id(), CTRL=b.id(), PC=b.id(), PU=b.id();
    auto PO=b.id(), images=b.id(), samplers=b.id(), control=b.id(), output=b.id();
    auto FT=b.id(), FL=b.id(), FW=b.id(), leaf=b.id(), wrapper=b.id(), main=b.id();
    b.op(b.pre,spv::OpCapability,{spv::CapabilityShader});
    b.op(b.pre,spv::OpCapability,{spv::CapabilitySampledImageArrayDynamicIndexing});
    b.op(b.pre,spv::OpMemoryModel,{spv::AddressingModelLogical,spv::MemoryModelGLSL450});
    b.text(b.pre,spv::OpEntryPoint,{spv::ExecutionModelFragment,main},"main",{output});
    b.op(b.pre,spv::OpExecutionMode,{main,spv::ExecutionModeOriginUpperLeft});
    b.name(images,"images"); b.name(samplers,"samplers"); b.name(control,"control");
    b.name(CTRL,"Control"); b.name(output,"out_color"); b.name(leaf,"leaf"); b.name(wrapper,"wrapper");
    b.text(b.debug,spv::OpMemberName,{CTRL,0},"image_index");
    b.text(b.debug,spv::OpMemberName,{CTRL,1},"sampler_index");
    for(auto r:{images,samplers,control}) {
        b.op(b.ann,spv::OpDecorate,{r,spv::DecorationDescriptorSet,0});
        b.op(b.ann,spv::OpDecorate,{r,spv::DecorationBinding,r==images?0u:r==samplers?1u:2u});
    }
    b.op(b.ann,spv::OpDecorate,{CTRL,spv::DecorationBlock});
    b.op(b.ann,spv::OpMemberDecorate,{CTRL,0,spv::DecorationOffset,0});
    b.op(b.ann,spv::OpMemberDecorate,{CTRL,1,spv::DecorationOffset,4});
    b.op(b.ann,spv::OpDecorate,{output,spv::DecorationLocation,0});
    b.op(b.types,spv::OpTypeVoid,{V}); b.op(b.types,spv::OpTypeInt,{U,32,0});
    b.op(b.types,spv::OpTypeFloat,{F,32});b.op(b.types,spv::OpTypeVector,{V2,F,2});
    b.op(b.types,spv::OpTypeVector,{V4,F,4});
    b.op(b.types,spv::OpTypeImage,{I,F,spv::Dim2D,0,0,0,1,spv::ImageFormatUnknown});
    b.op(b.types,spv::OpTypeSampler,{S});b.op(b.types,spv::OpTypeSampledImage,{IS,I});
    b.op(b.types,spv::OpConstant,{U,C0,0});b.op(b.types,spv::OpConstant,{U,CN,n});
    b.op(b.types,spv::OpConstant,{U,CM,m});b.op(b.types,spv::OpConstant,{F,F0,0});
    b.op(b.types,spv::OpConstantComposite,{V2,UV,F0,F0});
    b.op(b.types,spv::OpTypeArray,{IA,I,CN});b.op(b.types,spv::OpTypeArray,{SA,S,CM});
    b.op(b.types,spv::OpTypePointer,{PI,spv::StorageClassUniformConstant,I});
    b.op(b.types,spv::OpTypePointer,{PS,spv::StorageClassUniformConstant,S});
    b.op(b.types,spv::OpTypePointer,{PIA,spv::StorageClassUniformConstant,IA});
    b.op(b.types,spv::OpTypePointer,{PSA,spv::StorageClassUniformConstant,SA});
    b.op(b.types,spv::OpTypeStruct,{CTRL,U,U});
    b.op(b.types,spv::OpTypePointer,{PC,spv::StorageClassUniform,CTRL});
    b.op(b.types,spv::OpTypePointer,{PU,spv::StorageClassUniform,U});
    b.op(b.types,spv::OpTypePointer,{PO,spv::StorageClassOutput,V4});
    b.op(b.types,spv::OpVariable,{image_array?PIA:PI,images,spv::StorageClassUniformConstant});
    b.op(b.types,spv::OpVariable,{sampler_array?PSA:PS,samplers,spv::StorageClassUniformConstant});
    b.op(b.types,spv::OpVariable,{PC,control,spv::StorageClassUniform});
    b.op(b.types,spv::OpVariable,{PO,output,spv::StorageClassOutput});
    b.op(b.types,spv::OpTypeFunction,{FT,V});b.op(b.types,spv::OpTypeFunction,{FL,V4,I,S});
    b.op(b.types,spv::OpTypeFunction,{FW,V4,image_array?PIA:PI,sampler_array?PSA:PS,U,U});
    // leaf receives already selected opaque handles; wrapper receives the resource axes.
    auto li=b.id(),ls=b.id();
    b.op(b.funcs,spv::OpFunction,{V4,leaf,0,FL});
    b.op(b.funcs,spv::OpFunctionParameter,{I,li});b.op(b.funcs,spv::OpFunctionParameter,{S,ls});
    b.op(b.funcs,spv::OpLabel,{b.id()}); auto combined=b.id(),value=b.id();
    b.op(b.funcs,spv::OpSampledImage,{IS,combined,li,ls});
    b.op(b.funcs,spv::OpImageSampleExplicitLod,{V4,value,combined,UV,spv::ImageOperandsLodMask,F0});
    b.op(b.funcs,spv::OpReturnValue,{value}); b.op(b.funcs,spv::OpFunctionEnd,{});
    auto wi=b.id(),ws=b.id(),ix=b.id(),sx=b.id();
    b.op(b.funcs,spv::OpFunction,{V4,wrapper,0,FW});
    b.op(b.funcs,spv::OpFunctionParameter,{image_array?PIA:PI,wi});
    b.op(b.funcs,spv::OpFunctionParameter,{sampler_array?PSA:PS,ws});
    b.op(b.funcs,spv::OpFunctionParameter,{U,ix});b.op(b.funcs,spv::OpFunctionParameter,{U,sx});
    b.op(b.funcs,spv::OpLabel,{b.id()});
    auto sample=[&](uint32_t ir,uint32_t sr,uint32_t i,uint32_t s){
        uint32_t ip=ir,sp=sr;
        if(image_array){ip=b.id();b.op(b.funcs,spv::OpAccessChain,{PI,ip,ir,i});}
        if(sampler_array){sp=b.id();b.op(b.funcs,spv::OpAccessChain,{PS,sp,sr,s});}
        auto il=b.id(),sl=b.id(),r=b.id();
        b.op(b.funcs,spv::OpLoad,{I,il,ip});b.op(b.funcs,spv::OpLoad,{S,sl,sp});
        if(helpers) b.op(b.funcs,spv::OpFunctionCall,{V4,r,leaf,il,sl});
        else {auto c=b.id();b.op(b.funcs,spv::OpSampledImage,{IS,c,il,sl});
            b.op(b.funcs,spv::OpImageSampleExplicitLod,{V4,r,c,UV,spv::ImageOperandsLodMask,F0});}
        return r;
    };
    auto wv=sample(wi,ws,ix,sx);
    b.op(b.funcs,spv::OpReturnValue,{wv});b.op(b.funcs,spv::OpFunctionEnd,{});
    b.op(b.funcs,spv::OpFunction,{V,main,0,FT});b.op(b.funcs,spv::OpLabel,{b.id()});
    auto c1=b.id(); b.op(b.types,spv::OpConstant,{U,c1,1});
    auto pi=b.id(),ps=b.id(),ii=b.id(),ss=b.id(),result=b.id();
    b.op(b.funcs,spv::OpAccessChain,{PU,pi,control,C0});b.op(b.funcs,spv::OpAccessChain,{PU,ps,control,c1});
    b.op(b.funcs,spv::OpLoad,{U,ii,pi});b.op(b.funcs,spv::OpLoad,{U,ss,ps});
    if(helpers) b.op(b.funcs,spv::OpFunctionCall,{V4,result,wrapper,images,samplers,ii,ss});
    else result=sample(images,samplers,ii,ss);
    b.op(b.funcs,spv::OpStore,{output,result});b.op(b.funcs,spv::OpReturn,{});
    b.op(b.funcs,spv::OpFunctionEnd,{});
    return {b.words(),images,samplers,image_array?n:1,sampler_array?m:1};
}
inline spirv_cross::CompilerGLSL::Options options() {
    spirv_cross::CompilerGLSL::Options o; o.version=430;o.es=false;o.vulkan_semantics=false;return o;
}
}
#endif
