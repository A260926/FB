#ifndef CONSTANTS_MODULE_H
#define CONSTANTS_MODULE_H
#include <spvm/spirv.h>
#include <spvm/types.h>
#include <stddef.h>
/* A normal-constant SPIR-V 1.0 fragment module, NOT a specialization engine.
 * %12 = uint 3; %16 = uvec2(7,11); %18 = uint[3](7,11,7)
 * %19 = struct { uint[3], uvec2, float }; %20 = { %18, %16, -0.0 }
 * Private %22 is initialized from %20. helper(uint*) returns *arg.
 * main: out = helper(&private.field0[1]) + private.field1[0] = 11+7.
 * IDs and source order are deliberately explicit for inspection.
 */
static size_t constants_module(spvm_word *w) {
    size_t n=0;
#define W(x) (w[n++]=(spvm_word)(x))
#define OP(op,...) do { const spvm_word a[]={__VA_ARGS__}; size_t j; \
 W(((1+sizeof(a)/sizeof(a[0]))<<SpvWordCountShift)|(op)); \
 for(j=0;j<sizeof(a)/sizeof(a[0]);++j) W(a[j]); } while(0)
#define OP0(op) W((1<<SpvWordCountShift)|(op))
    W(SpvMagicNumber); W(0x00010000); W(0); W(60); W(0);
    OP(SpvOpCapability,SpvCapabilityShader);
    OP(SpvOpMemoryModel,SpvAddressingModelLogical,SpvMemoryModelGLSL450);
    OP(SpvOpEntryPoint,SpvExecutionModelFragment,40,0x6e69616d,0,25);
    OP(SpvOpExecutionMode,40,SpvExecutionModeOriginUpperLeft);
    OP(SpvOpDecorate,25,SpvDecorationLocation,0);
    OP(SpvOpTypeVoid,1);
    OP(SpvOpTypeInt,2,32,0); OP(SpvOpTypeInt,3,32,1);
    OP(SpvOpTypeFloat,4,32); OP(SpvOpTypeBool,5);
    OP(SpvOpTypeVector,6,2,2);
    OP(SpvOpConstant,2,10,0); OP(SpvOpConstant,2,11,1);
    OP(SpvOpConstant,2,12,3); OP(SpvOpConstant,2,13,7);
    OP(SpvOpConstant,2,14,11); OP(SpvOpConstant,4,15,0x80000000u);
    OP(SpvOpConstantComposite,6,16,13,14);
    OP(SpvOpTypeArray,17,2,12);
    OP(SpvOpConstantComposite,17,18,13,14,13);
    OP(SpvOpTypeStruct,19,17,6,4);
    OP(SpvOpConstantComposite,19,20,18,16,15);
    OP(SpvOpTypePointer,21,SpvStorageClassPrivate,19);
    OP(SpvOpVariable,21,22,SpvStorageClassPrivate,20);
    OP(SpvOpTypePointer,23,SpvStorageClassPrivate,2);
    OP(SpvOpTypePointer,24,SpvStorageClassOutput,2);
    OP(SpvOpVariable,24,25,SpvStorageClassOutput);
    OP(SpvOpTypeFunction,26,1); OP(SpvOpTypeFunction,27,2,23);
    OP(SpvOpFunction,2,30,SpvFunctionControlMaskNone,27);
    OP(SpvOpFunctionParameter,23,31); OP(SpvOpLabel,32);
    OP(SpvOpLoad,2,33,31); OP(SpvOpReturnValue,33); OP0(SpvOpFunctionEnd);
    OP(SpvOpFunction,1,40,SpvFunctionControlMaskNone,26);
    OP(SpvOpLabel,41);
    OP(SpvOpAccessChain,23,42,22,10,11);
    OP(SpvOpFunctionCall,2,43,30,42);
    OP(SpvOpAccessChain,23,44,22,11,10);
    OP(SpvOpLoad,2,45,44); OP(SpvOpIAdd,2,46,43,45);
    OP(SpvOpStore,25,46); OP0(SpvOpReturn); OP0(SpvOpFunctionEnd);
#undef OP0
#undef OP
#undef W
    return n;
}
#endif
