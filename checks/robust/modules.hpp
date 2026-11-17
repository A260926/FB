// Factory functions that build the small, valid SPIR-V 1.3 GLCompute modules exercised by the robust
// buffer access checks. Exact boundary values (negative one, UINT_MAX, length, ...) are baked in as
// constants and then routed through a trivial OpIAdd/OpBitcast so the index is a genuine dynamic SSA
// value: its runtime value is preserved exactly, while the backend cannot treat it as a literal.
#ifndef ROBUST_MODULES_HPP
#define ROBUST_MODULES_HPP

#include "spirv_builder.hpp"
#include <cstdint>

namespace robust
{

enum class ScalarKind
{
	Int,
	UInt,
	Float
};

// Common declaration bookkeeping shared by the simple block-based modules.
struct ScalarIds
{
	Word tVoid = 0, tFn = 0, tInt = 0, tScalar = 0;
	Word tRArr = 0, tStruct = 0, tPtrStruct = 0, tPtrScalar = 0;
	Word vBuf = 0, cZero = 0, entry = 0;
};

// Emits the capability/entry/type preamble for a runtime scalar block and returns the allocated ids.
// member_decl is appended by the caller-specific builder.
inline ScalarIds scalar_preamble(SpirvBuilder &b, ScalarKind kind, const char *member)
{
	ScalarIds s;
	s.tVoid = b.next_id();
	s.tFn = b.next_id();
	s.tInt = b.next_id();
	s.tScalar = b.next_id();
	s.tRArr = b.next_id();
	s.tStruct = b.next_id();
	s.tPtrStruct = b.next_id();
	s.tPtrScalar = b.next_id();
	s.vBuf = b.next_id();
	s.cZero = b.next_id();
	s.entry = b.next_id();

	b.op(spv::OpCapability, {spv::CapabilityShader});
	b.op(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
	b.op(spv::OpEntryPoint, {spv::ExecutionModelGLCompute, s.entry, 0x006e696du});
	b.op(spv::OpExecutionMode, {s.entry, spv::ExecutionModeLocalSize, 1, 1, 1});
	b.name(s.vBuf, "buf");
	b.member_name(s.tStruct, 0, member);
	b.decorate(s.tRArr, spv::DecorationArrayStride, {4});
	b.decorate(s.tStruct, spv::DecorationBlock, {});
	b.member_decorate(s.tStruct, 0, spv::DecorationOffset, {0});
	b.decorate(s.vBuf, spv::DecorationDescriptorSet, {0});
	b.decorate(s.vBuf, spv::DecorationBinding, {0});

	b.op(spv::OpTypeVoid, {s.tVoid});
	b.op(spv::OpTypeFunction, {s.tFn, s.tVoid});
	b.op(spv::OpTypeInt, {s.tInt, 32, 0});
	if (kind == ScalarKind::Float)
		b.op(spv::OpTypeFloat, {s.tScalar, 32});
	else
		b.op(spv::OpTypeInt, {s.tScalar, 32, kind == ScalarKind::UInt ? 1u : 0u});
	b.op(spv::OpTypeRuntimeArray, {s.tRArr, s.tScalar});
	b.op(spv::OpTypeStruct, {s.tStruct, s.tRArr});
	b.op(spv::OpTypePointer, {s.tPtrStruct, spv::StorageClassStorageBuffer, s.tStruct});
	b.op(spv::OpTypePointer, {s.tPtrScalar, spv::StorageClassStorageBuffer, s.tScalar});
	b.op(spv::OpVariable, {s.tPtrStruct, s.vBuf, spv::StorageClassStorageBuffer});
	b.op(spv::OpConstant, {s.tInt, s.cZero, 0});
	return s;
}

// Runtime array of a scalar type. main performs  out[0] = in[idx]  with separate load/store chains
// that share the same dynamic index. The index constant is hidden behind OpIAdd(0).
struct ScalarRuntimeModule
{
	std::string member_name = "values";
	ScalarKind kind = ScalarKind::Int;

	std::vector<Word> build(uint32_t index_bits)
	{
		SpirvBuilder b;
		ScalarIds s = scalar_preamble(b, kind, member_name.c_str());
		Word cIndex = b.next_id();
		Word dynIndex = b.next_id();
		Word lbl = b.next_id();
		Word chainL = b.next_id(), val = b.next_id(), chainS = b.next_id();

		b.op(spv::OpConstant, {s.tInt, cIndex, index_bits});

		b.op(spv::OpFunction, {s.tVoid, s.entry, 0, s.tFn});
		b.op(spv::OpLabel, {lbl});
		// Hide the exact boundary constant behind an SSA op so it remains a dynamic index at runtime.
		b.op(spv::OpIAdd, {s.tInt, dynIndex, cIndex, s.cZero});
		b.op(spv::OpAccessChain, {s.tPtrScalar, chainL, s.vBuf, s.cZero, dynIndex});
		b.op(spv::OpLoad, {s.tScalar, val, chainL});
		b.op(spv::OpAccessChain, {s.tPtrScalar, chainS, s.vBuf, s.cZero, dynIndex});
		b.op(spv::OpStore, {chainS, val});
		b.op(spv::OpReturn, {});
		b.op(spv::OpFunctionEnd, {});
		return b.finish();
	}
};

// Runtime array of vec4. Loads the whole vector at a dynamic outer index, then stores component 0 of
// that vector back through a scalar chain at dynamic outer AND dynamic component indices, exercising
// two independent dynamic bounds levels.
struct Vec4RuntimeModule
{
	std::vector<Word> build(uint32_t outer_bits, uint32_t comp_bits)
	{
		SpirvBuilder b;
		Word tVoid=b.next_id(), tFn=b.next_id(), tInt=b.next_id(), tFloat=b.next_id(), tVec4=b.next_id();
		Word tRArr=b.next_id(), tStruct=b.next_id(), tPtrStruct=b.next_id(), tPtrVec=b.next_id(), tPtrFloat=b.next_id();
		Word vBuf=b.next_id(), cZero=b.next_id(), cOuter=b.next_id(), cComp=b.next_id();
		Word dynOuter=b.next_id(), dynComp=b.next_id();
		Word entry=b.next_id(), lbl=b.next_id(), chainL=b.next_id(), vec=b.next_id();
		Word cZeroComp=b.next_id(), compVal=b.next_id(), chainS=b.next_id();

		b.op(spv::OpCapability, {spv::CapabilityShader});
		b.op(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
		b.op(spv::OpEntryPoint, {spv::ExecutionModelGLCompute, entry, 0x006e696du});
		b.op(spv::OpExecutionMode, {entry, spv::ExecutionModeLocalSize, 1, 1, 1});
		b.name(vBuf, "buf");
		b.member_name(tStruct, 0, "vals");
		b.decorate(tRArr, spv::DecorationArrayStride, {16});
		b.decorate(tStruct, spv::DecorationBlock, {});
		b.member_decorate(tStruct, 0, spv::DecorationOffset, {0});
		b.decorate(vBuf, spv::DecorationBinding, {0});

		b.op(spv::OpTypeVoid, {tVoid});
		b.op(spv::OpTypeFunction, {tFn, tVoid});
		b.op(spv::OpTypeInt, {tInt, 32, 0});
		b.op(spv::OpTypeFloat, {tFloat, 32});
		b.op(spv::OpTypeVector, {tVec4, tFloat, 4});
		b.op(spv::OpTypeRuntimeArray, {tRArr, tVec4});
		b.op(spv::OpTypeStruct, {tStruct, tRArr});
		b.op(spv::OpTypePointer, {tPtrStruct, spv::StorageClassStorageBuffer, tStruct});
		b.op(spv::OpTypePointer, {tPtrVec, spv::StorageClassStorageBuffer, tVec4});
		b.op(spv::OpTypePointer, {tPtrFloat, spv::StorageClassStorageBuffer, tFloat});
		b.op(spv::OpVariable, {tPtrStruct, vBuf, spv::StorageClassStorageBuffer});
		b.op(spv::OpConstant, {tInt, cZero, 0});
		b.op(spv::OpConstant, {tInt, cOuter, outer_bits});
		b.op(spv::OpConstant, {tInt, cComp, comp_bits});
		b.op(spv::OpConstant, {tInt, cZeroComp, 0});

		b.op(spv::OpFunction, {tVoid, entry, 0, tFn});
		b.op(spv::OpLabel, {lbl});
		b.op(spv::OpIAdd, {tInt, dynOuter, cOuter, cZero});
		b.op(spv::OpIAdd, {tInt, dynComp, cComp, cZero});
		b.op(spv::OpAccessChain, {tPtrVec, chainL, vBuf, cZero, dynOuter});
		b.op(spv::OpLoad, {tVec4, vec, chainL});
		b.op(spv::OpCompositeExtract, {tFloat, compVal, vec, 0});
		b.op(spv::OpAccessChain, {tPtrFloat, chainS, vBuf, cZero, dynOuter, dynComp});
		b.op(spv::OpStore, {chainS, compVal});
		b.op(spv::OpReturn, {});
		b.op(spv::OpFunctionEnd, {});
		return b.finish();
	}
};

// Runtime array of structs { int inner[3] }. Two loads isolate outer (runtime) and inner (fixed)
// out-of-bounds indices; their sum is stored to a valid location so neither load is dropped.
struct NestedStructModule
{
	std::vector<Word> build(uint32_t outer_bits, uint32_t inner_bits)
	{
		SpirvBuilder b;
		Word tVoid=b.next_id(), tFn=b.next_id(), tInt=b.next_id(), cThree=b.next_id();
		Word tFixed=b.next_id(), tStructElem=b.next_id(), tRArr=b.next_id(), tStruct=b.next_id();
		Word tPtrStruct=b.next_id(), tPtrInt=b.next_id();
		Word vBuf=b.next_id(), cZero=b.next_id(), cOuter=b.next_id(), cInner=b.next_id();
		Word dynOuter=b.next_id(), dynInner=b.next_id();
		Word entry=b.next_id(), lbl=b.next_id(), chainOuter=b.next_id(), chainInner=b.next_id();
		Word vOuter=b.next_id(), vInner=b.next_id(), sum=b.next_id(), chainS=b.next_id();

		b.op(spv::OpCapability, {spv::CapabilityShader});
		b.op(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
		b.op(spv::OpEntryPoint, {spv::ExecutionModelGLCompute, entry, 0x006e696du});
		b.op(spv::OpExecutionMode, {entry, spv::ExecutionModeLocalSize, 1, 1, 1});
		b.name(vBuf, "buf");
		b.member_name(tStruct, 0, "items");
		b.member_name(tStructElem, 0, "inner");
		b.decorate(tFixed, spv::DecorationArrayStride, {4});
		b.decorate(tRArr, spv::DecorationArrayStride, {12});
		b.decorate(tStruct, spv::DecorationBlock, {});
		b.member_decorate(tStruct, 0, spv::DecorationOffset, {0});
		b.member_decorate(tStructElem, 0, spv::DecorationOffset, {0});
		b.decorate(vBuf, spv::DecorationBinding, {0});

		b.op(spv::OpTypeVoid, {tVoid});
		b.op(spv::OpTypeFunction, {tFn, tVoid});
		b.op(spv::OpTypeInt, {tInt, 32, 0});
		b.op(spv::OpConstant, {tInt, cThree, 3});
		b.op(spv::OpTypeArray, {tFixed, tInt, cThree});
		b.op(spv::OpTypeStruct, {tStructElem, tFixed});
		b.op(spv::OpTypeRuntimeArray, {tRArr, tStructElem});
		b.op(spv::OpTypeStruct, {tStruct, tRArr});
		b.op(spv::OpTypePointer, {tPtrStruct, spv::StorageClassStorageBuffer, tStruct});
		b.op(spv::OpTypePointer, {tPtrInt, spv::StorageClassStorageBuffer, tInt});
		b.op(spv::OpVariable, {tPtrStruct, vBuf, spv::StorageClassStorageBuffer});
		b.op(spv::OpConstant, {tInt, cZero, 0});
		b.op(spv::OpConstant, {tInt, cOuter, outer_bits});
		b.op(spv::OpConstant, {tInt, cInner, inner_bits});

		b.op(spv::OpFunction, {tVoid, entry, 0, tFn});
		b.op(spv::OpLabel, {lbl});
		b.op(spv::OpIAdd, {tInt, dynOuter, cOuter, cZero});
		b.op(spv::OpIAdd, {tInt, dynInner, cInner, cZero});
		// items[outer].inner[0]
		b.op(spv::OpAccessChain, {tPtrInt, chainOuter, vBuf, cZero, dynOuter, cZero, cZero});
		b.op(spv::OpLoad, {tInt, vOuter, chainOuter});
		// items[0].inner[inner]
		b.op(spv::OpAccessChain, {tPtrInt, chainInner, vBuf, cZero, cZero, cZero, dynInner});
		b.op(spv::OpLoad, {tInt, vInner, chainInner});
		b.op(spv::OpIAdd, {tInt, sum, vOuter, vInner});
		b.op(spv::OpAccessChain, {tPtrInt, chainS, vBuf, cZero, cZero, cZero, cZero});
		b.op(spv::OpStore, {chainS, sum});
		b.op(spv::OpReturn, {});
		b.op(spv::OpFunctionEnd, {});
		return b.finish();
	}
};

// Non-recursive helper that writes a different SSBO location (the observable side effect) and returns
// the stored value. main stores the helper's result through a possibly out-of-bounds pointer. Even when
// the final store is dropped, the helper's own write event must still have happened exactly once.
struct HelperSideEffectModule
{
	std::vector<Word> build(uint32_t target_index_bits)
	{
		SpirvBuilder b;
		Word tVoid=b.next_id(), tFnVoid=b.next_id(), tInt=b.next_id(), tFloat=b.next_id(), tFnFloat=b.next_id();
		Word tRArr=b.next_id(), tStruct=b.next_id(), tPtrStruct=b.next_id(), tPtrFloat=b.next_id();
		Word vBuf=b.next_id(), cZero=b.next_id(), cOne=b.next_id(), cSeven=b.next_id();
		Word helper=b.next_id(), param=b.next_id(), lblH=b.next_id(), chW=b.next_id(), chR=b.next_id(), rv=b.next_id();
		Word entry=b.next_id(), lblE=b.next_id(), cTarget=b.next_id(), dynTarget=b.next_id();
		Word chT=b.next_id(), rhs=b.next_id();

		b.op(spv::OpCapability, {spv::CapabilityShader});
		b.op(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
		b.op(spv::OpEntryPoint, {spv::ExecutionModelGLCompute, entry, 0x006e696du});
		b.op(spv::OpExecutionMode, {entry, spv::ExecutionModeLocalSize, 1, 1, 1});
		b.name(vBuf, "buf");
		b.member_name(tStruct, 0, "values");
		b.decorate(tRArr, spv::DecorationArrayStride, {4});
		b.decorate(tStruct, spv::DecorationBlock, {});
		b.member_decorate(tStruct, 0, spv::DecorationOffset, {0});
		b.decorate(vBuf, spv::DecorationBinding, {0});

		b.op(spv::OpTypeVoid, {tVoid});
		b.op(spv::OpTypeFunction, {tFnVoid, tVoid});
		b.op(spv::OpTypeInt, {tInt, 32, 0});
		b.op(spv::OpTypeFloat, {tFloat, 32});
		b.op(spv::OpTypeFunction, {tFnFloat, tFloat, tInt});
		b.op(spv::OpTypeRuntimeArray, {tRArr, tFloat});
		b.op(spv::OpTypeStruct, {tStruct, tRArr});
		b.op(spv::OpTypePointer, {tPtrStruct, spv::StorageClassStorageBuffer, tStruct});
		b.op(spv::OpTypePointer, {tPtrFloat, spv::StorageClassStorageBuffer, tFloat});
		b.op(spv::OpVariable, {tPtrStruct, vBuf, spv::StorageClassStorageBuffer});
		b.op(spv::OpConstant, {tInt, cZero, 0});
		b.op(spv::OpConstant, {tInt, cOne, 1});
		b.op(spv::OpConstant, {tFloat, cSeven, 0x40e00000u}); // 7.0f

		// float helper(int) { values[1] = 7.0; return values[1]; }
		b.op(spv::OpFunction, {tFloat, helper, 0, tFnFloat});
		b.op(spv::OpFunctionParameter, {tInt, param});
		b.op(spv::OpLabel, {lblH});
		b.op(spv::OpAccessChain, {tPtrFloat, chW, vBuf, cZero, cOne});
		b.op(spv::OpStore, {chW, cSeven});
		b.op(spv::OpAccessChain, {tPtrFloat, chR, vBuf, cZero, cOne});
		b.op(spv::OpLoad, {tFloat, rv, chR});
		b.op(spv::OpReturnValue, {rv});
		b.op(spv::OpFunctionEnd, {});

		// main: values[target] = helper(0)
		b.op(spv::OpConstant, {tInt, cTarget, target_index_bits});
		b.op(spv::OpFunction, {tVoid, entry, 0, tFnVoid});
		b.op(spv::OpLabel, {lblE});
		b.op(spv::OpIAdd, {tInt, dynTarget, cTarget, cZero});
		b.op(spv::OpFunctionCall, {tFloat, rhs, helper, cZero});
		b.op(spv::OpAccessChain, {tPtrFloat, chT, vBuf, cZero, dynTarget});
		b.op(spv::OpStore, {chT, rhs});
		b.op(spv::OpReturn, {});
		b.op(spv::OpFunctionEnd, {});
		return b.finish();
	}
};

// A guard placed inside a loop and a user-selected if branch. The counter is an SSA function-local
// variable; the generated GLSL must keep the bounds guard inside the control flow rather than hoisting
// it out. Pseudo-GLSL: for (int i = 0; i < trip; i++) if (i != skip) values[i] = float(i);
struct LoopBranchModule
{
	std::vector<Word> build(uint32_t trip_bits)
	{
		SpirvBuilder b;
		Word tVoid=b.next_id(), tFn=b.next_id(), tInt=b.next_id(), tFloat=b.next_id();
		Word tRArr=b.next_id(), tStruct=b.next_id(), tPtrStruct=b.next_id(), tPtrFloat=b.next_id();
		Word tPtrIntFunc=b.next_id();
		Word vBuf=b.next_id(), cZero=b.next_id(), cOne=b.next_id(), cTrip=b.next_id(), cSkip=b.next_id();
		Word entry=b.next_id();
		Word tBool=b.next_id();
		// blocks
		Word lblEntry=b.next_id(), lblCond=b.next_id(), lblBody=b.next_id(), lblMerge=b.next_id();
		Word lblThen=b.next_id(), lblIfMerge=b.next_id();
		// locals / ssa
		Word varI=b.next_id(), varLimit=b.next_id();
		Word iVal=b.next_id(), limitVal=b.next_id(), cmpId=b.next_id();
		Word neq=b.next_id(), fVal=b.next_id(), chS=b.next_id(), iNext=b.next_id();

		b.op(spv::OpCapability, {spv::CapabilityShader});
		b.op(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
		b.op(spv::OpEntryPoint, {spv::ExecutionModelGLCompute, entry, 0x006e696du});
		b.op(spv::OpExecutionMode, {entry, spv::ExecutionModeLocalSize, 1, 1, 1});
		b.name(vBuf, "buf");
		b.member_name(tStruct, 0, "values");
		b.decorate(tRArr, spv::DecorationArrayStride, {4});
		b.decorate(tStruct, spv::DecorationBlock, {});
		b.member_decorate(tStruct, 0, spv::DecorationOffset, {0});
		b.decorate(vBuf, spv::DecorationBinding, {0});

		b.op(spv::OpTypeVoid, {tVoid});
		b.op(spv::OpTypeFunction, {tFn, tVoid});
		b.op(spv::OpTypeInt, {tInt, 32, 0});
		b.op(spv::OpTypeBool, {tBool});
		b.op(spv::OpTypeFloat, {tFloat, 32});
		b.op(spv::OpTypeRuntimeArray, {tRArr, tFloat});
		b.op(spv::OpTypeStruct, {tStruct, tRArr});
		b.op(spv::OpTypePointer, {tPtrStruct, spv::StorageClassStorageBuffer, tStruct});
		b.op(spv::OpTypePointer, {tPtrFloat, spv::StorageClassStorageBuffer, tFloat});
		b.op(spv::OpTypePointer, {tPtrIntFunc, spv::StorageClassFunction, tInt});
		b.op(spv::OpVariable, {tPtrStruct, vBuf, spv::StorageClassStorageBuffer});
		b.op(spv::OpConstant, {tInt, cZero, 0});
		b.op(spv::OpConstant, {tInt, cOne, 1});
		b.op(spv::OpConstant, {tInt, cTrip, trip_bits});
		b.op(spv::OpConstant, {tInt, cSkip, 3});

		b.op(spv::OpFunction, {tVoid, entry, 0, tFn});
		b.op(spv::OpLabel, {lblEntry});
		// Function-scope variables must be the first instructions in the entry block.
		b.op(spv::OpVariable, {tPtrIntFunc, varI, spv::StorageClassFunction});
		b.op(spv::OpVariable, {tPtrIntFunc, varLimit, spv::StorageClassFunction});
		b.op(spv::OpStore, {varI, cZero});
		b.op(spv::OpStore, {varLimit, cTrip});
		b.op(spv::OpBranch, {lblCond});

		// Loop header/condition block with a loop merge.
		b.op(spv::OpLabel, {lblCond});
		b.op(spv::OpLoopMerge, {lblMerge, lblCond, 0});
		b.op(spv::OpLoad, {tInt, iVal, varI});
		b.op(spv::OpLoad, {tInt, limitVal, varLimit});
		b.op(spv::OpSLessThan, {tBool, cmpId, iVal, limitVal});
		b.op(spv::OpBranchConditional, {cmpId, lblBody, lblMerge});

		// Body: an explicit user branch (i != skip) that wraps the guarded store. The robust guard must
		// remain nested inside this selection and the loop, never hoisted out.
		b.op(spv::OpLabel, {lblBody});
		b.op(spv::OpINotEqual, {tBool, neq, iVal, cSkip});
		b.op(spv::OpSelectionMerge, {lblIfMerge, 0});
		b.op(spv::OpBranchConditional, {neq, lblThen, lblIfMerge});

		b.op(spv::OpLabel, {lblThen});
		b.op(spv::OpConvertSToF, {tFloat, fVal, iVal});
		b.op(spv::OpAccessChain, {tPtrFloat, chS, vBuf, cZero, iVal});
		b.op(spv::OpStore, {chS, fVal});
		b.op(spv::OpBranch, {lblIfMerge});

		b.op(spv::OpLabel, {lblIfMerge});
		b.op(spv::OpIAdd, {tInt, iNext, iVal, cOne});
		b.op(spv::OpStore, {varI, iNext});
		b.op(spv::OpBranch, {lblCond});

		b.op(spv::OpLabel, {lblMerge});
		b.op(spv::OpReturn, {});
		b.op(spv::OpFunctionEnd, {});
		return b.finish();
	}
};

// Whole-struct SSBO load (a non scalar/vector value type): rejected in robust mode, accepted off.
struct WholeStructModule
{
	std::vector<Word> build()
	{
		SpirvBuilder b;
		Word tVoid=b.next_id(), tFn=b.next_id(), tInt=b.next_id(), c3=b.next_id();
		Word tFixed=b.next_id(), tElem=b.next_id(), tRArr=b.next_id(), tStruct=b.next_id();
		Word tPtrStruct=b.next_id(), tPtrElem=b.next_id();
		Word vBuf=b.next_id(), cZero=b.next_id(), entry=b.next_id(), lbl=b.next_id();
		Word ch=b.next_id(), val=b.next_id(), copy=b.next_id();

		b.op(spv::OpCapability, {spv::CapabilityShader});
		b.op(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
		b.op(spv::OpEntryPoint, {spv::ExecutionModelGLCompute, entry, 0x006e696du});
		b.op(spv::OpExecutionMode, {entry, spv::ExecutionModeLocalSize, 1, 1, 1});
		b.decorate(tFixed, spv::DecorationArrayStride, {4});
		b.decorate(tRArr, spv::DecorationArrayStride, {12});
		b.decorate(tStruct, spv::DecorationBlock, {});
		b.member_decorate(tStruct, 0, spv::DecorationOffset, {0});
		b.member_decorate(tElem, 0, spv::DecorationOffset, {0});
		b.decorate(vBuf, spv::DecorationBinding, {0});

		b.op(spv::OpTypeVoid, {tVoid});
		b.op(spv::OpTypeFunction, {tFn, tVoid});
		b.op(spv::OpTypeInt, {tInt, 32, 0});
		b.op(spv::OpConstant, {tInt, c3, 3});
		b.op(spv::OpConstant, {tInt, cZero, 0});
		b.op(spv::OpTypeArray, {tFixed, tInt, c3});
		b.op(spv::OpTypeStruct, {tElem, tFixed});
		b.op(spv::OpTypeRuntimeArray, {tRArr, tElem});
		b.op(spv::OpTypeStruct, {tStruct, tRArr});
		b.op(spv::OpTypePointer, {tPtrStruct, spv::StorageClassStorageBuffer, tStruct});
		b.op(spv::OpTypePointer, {tPtrElem, spv::StorageClassStorageBuffer, tElem});
		b.op(spv::OpVariable, {tPtrStruct, vBuf, spv::StorageClassStorageBuffer});
		b.op(spv::OpFunction, {tVoid, entry, 0, tFn});
		b.op(spv::OpLabel, {lbl});
		b.op(spv::OpAccessChain, {tPtrElem, ch, vBuf, cZero, cZero});
		b.op(spv::OpLoad, {tElem, val, ch});
		b.op(spv::OpCopyObject, {tElem, copy, val});
		(void)copy;
		b.op(spv::OpReturn, {});
		b.op(spv::OpFunctionEnd, {});
		return b.finish();
	}
};

// OpAtomicStore targeting an SSBO: must be rejected up front in robust mode, accepted when disabled.
struct AtomicStoreModule
{
	std::vector<Word> build()
	{
		SpirvBuilder b;
		Word tVoid=b.next_id(), tFn=b.next_id(), tInt=b.next_id(), tUInt=b.next_id();
		Word tRArr=b.next_id(), tStruct=b.next_id(), tPtrStruct=b.next_id(), tPtrUInt=b.next_id();
		Word vBuf=b.next_id(), cZero=b.next_id(), cValue=b.next_id(), entry=b.next_id(), lbl=b.next_id();
		Word ch=b.next_id();

		b.op(spv::OpCapability, {spv::CapabilityShader});
		b.op(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
		b.op(spv::OpEntryPoint, {spv::ExecutionModelGLCompute, entry, 0x006e696du});
		b.op(spv::OpExecutionMode, {entry, spv::ExecutionModeLocalSize, 1, 1, 1});
		b.decorate(tRArr, spv::DecorationArrayStride, {4});
		b.decorate(tStruct, spv::DecorationBlock, {});
		b.member_decorate(tStruct, 0, spv::DecorationOffset, {0});
		b.op(spv::OpTypeVoid, {tVoid});
		b.op(spv::OpTypeFunction, {tFn, tVoid});
		b.op(spv::OpTypeInt, {tInt, 32, 0});
		b.op(spv::OpTypeInt, {tUInt, 32, 1});
		b.op(spv::OpTypeRuntimeArray, {tRArr, tUInt});
		b.op(spv::OpTypeStruct, {tStruct, tRArr});
		b.op(spv::OpTypePointer, {tPtrStruct, spv::StorageClassStorageBuffer, tStruct});
		b.op(spv::OpTypePointer, {tPtrUInt, spv::StorageClassStorageBuffer, tUInt});
		b.op(spv::OpVariable, {tPtrStruct, vBuf, spv::StorageClassStorageBuffer});
		b.op(spv::OpConstant, {tInt, cZero, 0});
		b.op(spv::OpConstant, {tUInt, cValue, 42});
		b.op(spv::OpFunction, {tVoid, entry, 0, tFn});
		b.op(spv::OpLabel, {lbl});
		b.op(spv::OpAccessChain, {tPtrUInt, ch, vBuf, cZero, cZero});
		b.op(spv::OpAtomicStore, {ch, 2 /*Workgroup*/, 0, cValue});
		b.op(spv::OpReturn, {});
		b.op(spv::OpFunctionEnd, {});
		return b.finish();
	}
};

} // namespace robust

#endif
