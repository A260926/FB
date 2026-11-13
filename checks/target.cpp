// Target tests for Compiler::get_buffer_byte_accesses().
//
// All SPIR-V modules are assembled directly as legal 32-bit word streams, so no compiler SDK or
// external shader tooling is required. Every module uses the logical addressing model and ordinary
// Uniform / StorageBuffer logical pointers.
#include "suite.h"
#include "spirv_cross.hpp"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace spv;
namespace sc = spirv_cross;

namespace
{
// Minimal logical-module builder. IDs are allocated explicitly by the caller to keep decorations
// and type relationships easy to follow.
struct ModuleBuilder
{
	std::vector<uint32_t> words;
	explicit ModuleBuilder(uint32_t id_bound)
	{
		words = { 0x07230203u, 0x00010000u, 0u, id_bound, 0u };
	}

	void op(Op opcode, std::initializer_list<uint32_t> args)
	{
		words.push_back((uint32_t(args.size() + 1) << 16) | uint32_t(opcode));
		words.insert(words.end(), args.begin(), args.end());
	}

	void op(Op opcode, const std::vector<uint32_t> &args)
	{
		words.push_back((uint32_t(args.size() + 1) << 16) | uint32_t(opcode));
		words.insert(words.end(), args.begin(), args.end());
	}

	void entry_point(ExecutionModel model, uint32_t function_id, const char *name)
	{
		std::vector<uint32_t> args = { uint32_t(model), function_id };
		push_string(args, name);
		op(OpEntryPoint, args);
	}

	static void push_string(std::vector<uint32_t> &out, const char *text)
	{
		size_t length = std::strlen(text);
		size_t word_count = length / 4 + 1;
		for (size_t i = 0; i < word_count; i++)
		{
			uint32_t word = 0;
			for (size_t byte = 0; byte < 4; byte++)
			{
				size_t pos = i * 4 + byte;
				char c = pos < length ? text[pos] : '\0';
				word |= uint32_t(uint8_t(c)) << (8 * byte);
			}
			out.push_back(word);
		}
	}

	std::vector<uint32_t> build() const
	{
		return words;
	}
};

// Verifies one expected access against the result and accumulates real assertions. The caller
// supplies the expected list so every offset/size/flag pair is checked independently rather than
// through a single opaque vector comparison.
#define CHECK_RANGE(VEC, INDEX, EXPECTED_OFFSET, EXPECTED_SIZE, EXPECTED_READ, EXPECTED_WRITE)   \
	do                                                                                             \
	{                                                                                              \
		UNIT_CHECK((VEC).size() > (INDEX));                                                        \
		UNIT_CHECK((VEC)[INDEX].offset == size_t(EXPECTED_OFFSET));                                \
		UNIT_CHECK((VEC)[INDEX].size == size_t(EXPECTED_SIZE));                                    \
		UNIT_CHECK((VEC)[INDEX].read == bool(EXPECTED_READ));                                      \
		UNIT_CHECK((VEC)[INDEX].write == bool(EXPECTED_WRITE));                                    \
	} while (0)

// Asserts the structural contract: non-zero size, ascending offsets and no overlap (adjacent
// ranges are allowed, but only merge when flags match), followed by the exact element count.
#define CHECK_RANGE_INVARIANTS_AND_COUNT(VEC, EXPECTED_COUNT)                                     \
	do                                                                                             \
	{                                                                                              \
		UNIT_CHECK((VEC).size() == size_t(EXPECTED_COUNT));                                        \
		size_t previous_end = 0;                                                                   \
		for (size_t i = 0; i < (VEC).size(); i++)                                                  \
		{                                                                                          \
			UNIT_CHECK((VEC)[i].size != 0);                                                        \
			UNIT_CHECK((VEC)[i].offset >= previous_end);                                           \
			if (i > 0 && (VEC)[i].offset == previous_end)                                          \
				UNIT_CHECK((VEC)[i - 1].read != (VEC)[i].read ||                                   \
				           (VEC)[i - 1].write != (VEC)[i].write);                                  \
			previous_end = (VEC)[i].offset + (VEC)[i].size;                                        \
		}                                                                                          \
	} while (0)

template <typename Fn>
bool throws_compiler_error(Fn &&fn)
{
	try
	{
		fn();
		return false;
	}
	catch (const sc::CompilerError &)
	{
		return true;
	}
}

// Common module prologue: shader capability, GLSL memory model and fragment execution mode.
void emit_prologue(ModuleBuilder &m)
{
	m.op(OpCapability, { CapabilityShader });
	m.op(OpMemoryModel, { AddressingModelLogical, MemoryModelGLSL450 });
	m.op(OpExecutionMode, { 20, ExecutionModeOriginUpperLeft });
}

uint32_t float_bits(float value)
{
	uint32_t bits;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

// ---------------------------------------------------------------------------------------------
// layout group:
//   * discrete columns of a row-major non-square matrix,
//   * a full column-major matrix load without inter-column padding,
//   * a nested array with stride padding,
//   * runtime indexing of a fixed-length padded array,
//   * overlapping read/write splitting,
//   * unused pointers and unreachable blocks contributing nothing.
// ---------------------------------------------------------------------------------------------
int run_layout_group()
{
	ModuleBuilder m(100);
	emit_prologue(m);
	m.entry_point(ExecutionModelFragment, 20, "main");

	// Scalar / vector / matrix types.
	m.op(OpTypeVoid, { 1 });
	m.op(OpTypeFunction, { 2, 1 });
	m.op(OpTypeFloat, { 3, 32 });
	m.op(OpTypeInt, { 4, 32, 0 });
	m.op(OpTypeBool, { 5 });
	m.op(OpTypeVector, { 6, 3, 3 });   // float3
	m.op(OpTypeMatrix, { 7, 6, 2 });   // mat2x3: two columns of float3
	m.op(OpTypeMatrix, { 13, 6, 2 });  // second mat2x3, column-major

	// Array length constants must precede the OpTypeArray instructions that use them.
	m.op(OpConstant, { 4, 30, 2 }); // Inner[2] length
	m.op(OpConstant, { 4, 31, 4 }); // float[4] length

	m.op(OpTypeStruct, { 8, 3, 6 });   // Inner { float a; float3 b; }
	m.op(OpTypeArray, { 9, 8, 30 });   // Inner[2]
	m.op(OpTypeArray, { 12, 3, 31 });  // float[4]
	m.op(OpTypeStruct, { 10, 3, 9, 7, 6, 12, 13 });

	// Pointer types (Uniform storage class).
	m.op(OpTypePointer, { 14, StorageClassUniform, 10 }); // ptr to block
	m.op(OpTypePointer, { 15, StorageClassUniform, 3 });  // ptr to float
	m.op(OpTypePointer, { 16, StorageClassUniform, 6 });  // ptr to float3
	m.op(OpTypePointer, { 17, StorageClassUniform, 13 }); // ptr to matrix

	// Decorations.
	m.op(OpDecorate, { 10, DecorationBlock });
	m.op(OpDecorate, { 9, DecorationArrayStride, 32 });
	m.op(OpDecorate, { 12, DecorationArrayStride, 16 });
	m.op(OpMemberDecorate, { 8, 0, DecorationOffset, 0 });
	m.op(OpMemberDecorate, { 8, 1, DecorationOffset, 16 });

	m.op(OpMemberDecorate, { 10, 0, DecorationOffset, 0 });
	m.op(OpMemberDecorate, { 10, 1, DecorationOffset, 16 });
	m.op(OpMemberDecorate, { 10, 2, DecorationOffset, 80 });
	m.op(OpMemberDecorate, { 10, 2, DecorationMatrixStride, 16 });
	m.op(OpMemberDecorate, { 10, 2, DecorationRowMajor });
	m.op(OpMemberDecorate, { 10, 3, DecorationOffset, 128 });
	m.op(OpMemberDecorate, { 10, 4, DecorationOffset, 144 });
	m.op(OpMemberDecorate, { 10, 5, DecorationOffset, 208 });
	m.op(OpMemberDecorate, { 10, 5, DecorationMatrixStride, 16 });
	m.op(OpMemberDecorate, { 10, 5, DecorationColMajor });
	m.op(OpDecorate, { 40, DecorationDescriptorSet, 0 });
	m.op(OpDecorate, { 40, DecorationBinding, 0 });

	// Index / value constants.
	m.op(OpConstant, { 4, 32, 0 });
	m.op(OpConstant, { 4, 33, 1 });
	m.op(OpConstant, { 4, 34, 2 });
	m.op(OpConstant, { 4, 36, 4 });
	m.op(OpConstant, { 4, 37, 5 });
	m.op(OpConstant, { 4, 38, 3 });
	m.op(OpConstant, { 3, 35, float_bits(1.0f) });

	// Block variable.
	m.op(OpVariable, { 14, 40, StorageClassUniform });

	// Entry function.
	m.op(OpFunction, { 1, 20, 0, 2 });
	m.op(OpLabel, { 21 });

	// Runtime (non-constant) index and a float3 store value, deliberately undefined.
	m.op(OpUndef, { 4, 41 });
	m.op(OpUndef, { 6, 61 });

	// 1. Load scalar member 0 -> read [0, 4).
	m.op(OpAccessChain, { 15, 50, 40, 32 });
	m.op(OpLoad, { 3, 51, 50 });

	// 2. Load nested array element Inner[1].b -> offset 16 + 32 + 16 = 64, float3 -> [64, 76).
	m.op(OpAccessChain, { 16, 52, 40, 33, 33, 33 });
	m.op(OpLoad, { 6, 53, 52 });

	// 3. Load one logical column of the row-major mat2x3 (member 2), column 0.
	//    Row-major columns are stridden scalars: [80,84), [96,100), [112,116).
	m.op(OpAccessChain, { 16, 54, 40, 34, 32 });
	m.op(OpLoad, { 6, 55, 54 });

	// 4. Runtime-indexed load into padded float[4] (member 4, stride 16). Union of all element
	//    scalar bytes, excluding padding: 144, 160, 176, 192.
	m.op(OpAccessChain, { 15, 56, 40, 36, 41 });
	m.op(OpLoad, { 3, 57, 56 });

	// 5. Full load of the column-major mat2x3 (member 5): contiguous columns only.
	//    [208,220) and [224,236), excluding the stride padding.
	m.op(OpAccessChain, { 17, 58, 40, 37 });
	m.op(OpLoad, { 13, 59, 58 });

	// 6. Overlapping read/write on member 3 (float3 at 128): full store plus a scalar load of .y.
	m.op(OpAccessChain, { 16, 60, 40, 38 });
	m.op(OpStore, { 60, 61 });
	m.op(OpAccessChain, { 15, 62, 40, 38, 33 });
	m.op(OpLoad, { 3, 63, 62 });

	// 7. Pointer to row-major column 1 (bytes 84/100/116) constructed but never dereferenced;
	//    those bytes must not appear in the result.
	m.op(OpInBoundsAccessChain, { 16, 64, 40, 34, 33 });
	(void)64;

	// Reachable entry block terminates.
	m.op(OpReturn, {});

	// Orphan, unreachable block. Its store targets row-major column 1, but must be ignored, so the
	// same bytes stay absent from the access set.
	m.op(OpLabel, { 70 });
	m.op(OpAccessChain, { 16, 71, 40, 34, 33 });
	m.op(OpStore, { 71, 61 });
	m.op(OpReturn, {});

	m.op(OpFunctionEnd, {});

	sc::Compiler compiler(m.build());
	auto accesses = compiler.get_buffer_byte_accesses(40);

	CHECK_RANGE_INVARIANTS_AND_COUNT(accesses, 14);
	CHECK_RANGE(accesses, 0, 0, 4, true, false);    // member 0 scalar load
	CHECK_RANGE(accesses, 1, 64, 12, true, false);  // nested Inner[1].b
	// Row-major mat2x3, logical column 0: three discrete, stridden scalar segments.
	CHECK_RANGE(accesses, 2, 80, 4, true, false);
	CHECK_RANGE(accesses, 3, 96, 4, true, false);
	CHECK_RANGE(accesses, 4, 112, 4, true, false);
	// Overlapping full-vector write and scalar read split into write / read-write / write.
	CHECK_RANGE(accesses, 5, 128, 4, false, true);
	CHECK_RANGE(accesses, 6, 132, 4, true, true);
	CHECK_RANGE(accesses, 7, 136, 4, false, true);
	// Runtime-indexed padded float[4]: real scalar bytes only, never the inter-element padding.
	CHECK_RANGE(accesses, 8, 144, 4, true, false);
	CHECK_RANGE(accesses, 9, 160, 4, true, false);
	CHECK_RANGE(accesses, 10, 176, 4, true, false);
	CHECK_RANGE(accesses, 11, 192, 4, true, false);
	// Full column-major matrix load: contiguous columns only, no matrix-stride padding.
	CHECK_RANGE(accesses, 12, 208, 12, true, false);
	CHECK_RANGE(accesses, 13, 224, 12, true, false);

	// --- OpCopyMemory: the destination accumulates a write and the source a read. --------------
	{
		ModuleBuilder cm(90);
		emit_prologue(cm);
		cm.entry_point(ExecutionModelFragment, 20, "main");

		cm.op(OpTypeVoid, { 1 });
		cm.op(OpTypeFunction, { 2, 1 });
		cm.op(OpTypeFloat, { 3, 32 });
		cm.op(OpTypeInt, { 4, 32, 0 });
		cm.op(OpTypeStruct, { 5, 3, 3 });
		cm.op(OpTypePointer, { 6, StorageClassStorageBuffer, 5 });
		cm.op(OpTypePointer, { 7, StorageClassStorageBuffer, 3 });
		cm.op(OpDecorate, { 5, DecorationBufferBlock });
		cm.op(OpMemberDecorate, { 5, 0, DecorationOffset, 0 });
		cm.op(OpMemberDecorate, { 5, 1, DecorationOffset, 16 });
		cm.op(OpConstant, { 4, 30, 0 });
		cm.op(OpConstant, { 4, 31, 1 });
		cm.op(OpVariable, { 6, 40, StorageClassStorageBuffer });

		cm.op(OpFunction, { 1, 20, 0, 2 });
		cm.op(OpLabel, { 21 });
		cm.op(OpAccessChain, { 7, 60, 40, 30 }); // member 0
		cm.op(OpAccessChain, { 7, 61, 40, 31 }); // member 1
		cm.op(OpCopyMemory, { 61, 60 });         // destination first, source second
		cm.op(OpReturn, {});
		cm.op(OpFunctionEnd, {});

		sc::Compiler compiler(cm.build());
		auto cm_accesses = compiler.get_buffer_byte_accesses(40);
		CHECK_RANGE_INVARIANTS_AND_COUNT(cm_accesses, 2);
		CHECK_RANGE(cm_accesses, 0, 0, 4, true, false);   // source read
		CHECK_RANGE(cm_accesses, 1, 16, 4, false, true);  // destination written
	}

	// --- Adjacent same-flag segments are merged into one contiguous range. ----------------------
	{
		ModuleBuilder adj(80);
		emit_prologue(adj);
		adj.entry_point(ExecutionModelFragment, 20, "main");

		adj.op(OpTypeVoid, { 1 });
		adj.op(OpTypeFunction, { 2, 1 });
		adj.op(OpTypeFloat, { 3, 32 });
		adj.op(OpTypeInt, { 4, 32, 0 });
		adj.op(OpTypeStruct, { 5, 3, 3 });
		adj.op(OpTypePointer, { 6, StorageClassUniform, 5 });
		adj.op(OpTypePointer, { 7, StorageClassUniform, 3 });
		adj.op(OpDecorate, { 5, DecorationBlock });
		adj.op(OpMemberDecorate, { 5, 0, DecorationOffset, 0 });
		adj.op(OpMemberDecorate, { 5, 1, DecorationOffset, 4 }); // tightly adjacent
		adj.op(OpConstant, { 4, 30, 0 });
		adj.op(OpConstant, { 4, 31, 1 });
		adj.op(OpVariable, { 6, 40, StorageClassUniform });

		adj.op(OpFunction, { 1, 20, 0, 2 });
		adj.op(OpLabel, { 21 });
		adj.op(OpAccessChain, { 7, 60, 40, 30 });
		adj.op(OpLoad, { 3, 62, 60 });
		adj.op(OpAccessChain, { 7, 61, 40, 31 });
		adj.op(OpLoad, { 3, 63, 61 });
		adj.op(OpReturn, {});
		adj.op(OpFunctionEnd, {});

		sc::Compiler compiler(adj.build());
		auto adj_accesses = compiler.get_buffer_byte_accesses(40);
		// The two adjacent reads collapse into a single [0, 8) range.
		CHECK_RANGE_INVARIANTS_AND_COUNT(adj_accesses, 1);
		CHECK_RANGE(adj_accesses, 0, 0, 8, true, false);
	}

	return unit_checks;
}

// ---------------------------------------------------------------------------------------------
// aliases group:
//   * one helper invoked with two different member arguments,
//   * pointer confluence through OpSelect and OpPhi,
//   * OpCopyObject forwarding.
// ---------------------------------------------------------------------------------------------
int run_aliases_group()
{
	// --- Module A: one helper invoked twice with two distinct member arguments. ---------------
	{
		ModuleBuilder a(120);
		emit_prologue(a);
		a.entry_point(ExecutionModelFragment, 20, "main");

		a.op(OpTypeVoid, { 1 });
		a.op(OpTypeFunction, { 2, 1 });
		a.op(OpTypeFloat, { 3, 32 });
		a.op(OpTypeInt, { 4, 32, 0 });
		a.op(OpTypeStruct, { 6, 3, 3 });
		a.op(OpTypePointer, { 7, StorageClassUniform, 6 });
		a.op(OpTypePointer, { 8, StorageClassUniform, 3 });
		a.op(OpTypeFunction, { 9, 1, 8 });

		a.op(OpDecorate, { 6, DecorationBlock });
		a.op(OpMemberDecorate, { 6, 0, DecorationOffset, 0 });
		a.op(OpMemberDecorate, { 6, 1, DecorationOffset, 16 });
		a.op(OpDecorate, { 40, DecorationBinding, 0 });
		a.op(OpConstant, { 4, 30, 0 });
		a.op(OpConstant, { 4, 31, 1 });
		a.op(OpVariable, { 7, 40, StorageClassUniform });

		// Helper loads through its pointer parameter.
		a.op(OpFunction, { 1, 50, 0, 9 });
		a.op(OpFunctionParameter, { 8, 51 });
		a.op(OpLabel, { 52 });
		a.op(OpLoad, { 3, 53, 51 });
		a.op(OpReturn, {});
		a.op(OpFunctionEnd, {});

		a.op(OpFunction, { 1, 20, 0, 2 });
		a.op(OpLabel, { 21 });
		a.op(OpAccessChain, { 8, 60, 40, 30 });
		a.op(OpFunctionCall, { 1, 61, 50, 60 });
		a.op(OpAccessChain, { 8, 62, 40, 31 });
		a.op(OpFunctionCall, { 1, 63, 50, 62 });
		a.op(OpReturn, {});
		a.op(OpFunctionEnd, {});

		sc::Compiler compiler(a.build());
		auto accesses = compiler.get_buffer_byte_accesses(40);
		CHECK_RANGE_INVARIANTS_AND_COUNT(accesses, 2);
		CHECK_RANGE(accesses, 0, 0, 4, true, false);
		CHECK_RANGE(accesses, 1, 16, 4, true, false);
	}

	// --- Module B: pointer confluence through OpSelect, OpPhi and OpCopyObject. ----------------
	{
		ModuleBuilder m(200);
		emit_prologue(m);
		m.entry_point(ExecutionModelFragment, 20, "main");

		m.op(OpTypeVoid, { 1 });
		m.op(OpTypeFunction, { 2, 1 });
		m.op(OpTypeFloat, { 3, 32 });
		m.op(OpTypeInt, { 4, 32, 0 });
		m.op(OpTypeBool, { 5 });
		m.op(OpTypeStruct, { 6, 3, 3 });
		m.op(OpTypePointer, { 7, StorageClassUniform, 6 });
		m.op(OpTypePointer, { 8, StorageClassUniform, 3 });

		m.op(OpDecorate, { 6, DecorationBlock });
		m.op(OpMemberDecorate, { 6, 0, DecorationOffset, 0 });
		m.op(OpMemberDecorate, { 6, 1, DecorationOffset, 16 });
		m.op(OpDecorate, { 40, DecorationBinding, 0 });

		m.op(OpConstant, { 4, 30, 0 });
		m.op(OpConstant, { 4, 31, 1 });
		m.op(OpConstant, { 3, 32, float_bits(2.0f) });
		m.op(OpUndef, { 5, 33 });

		m.op(OpVariable, { 7, 40, StorageClassUniform });

		m.op(OpFunction, { 1, 20, 0, 2 });
		m.op(OpLabel, { 21 });

		m.op(OpAccessChain, { 8, 60, 40, 30 });
		m.op(OpAccessChain, { 8, 62, 40, 31 });

		// OpSelect pointer confluence, then a store through the merged pointer.
		m.op(OpSelect, { 8, 64, 33, 60, 62 });
		m.op(OpStore, { 64, 32 });

		// OpCopyObject forwarding, then read.
		m.op(OpCopyObject, { 8, 65, 60 });
		m.op(OpLoad, { 3, 66, 65 });

		// OpPhi confluence over a real conditional.
		m.op(OpSelectionMerge, { 74, 0 });
		m.op(OpBranchConditional, { 33, 71, 72 });

		m.op(OpLabel, { 71 });
		m.op(OpAccessChain, { 8, 75, 40, 30 });
		m.op(OpBranch, { 74 });

		m.op(OpLabel, { 72 });
		m.op(OpAccessChain, { 8, 76, 40, 31 });
		m.op(OpBranch, { 74 });

		m.op(OpLabel, { 74 });
		m.op(OpPhi, { 8, 77, 75, 71, 76, 72 });
		m.op(OpLoad, { 3, 78, 77 });
		m.op(OpBranch, { 79 });

		m.op(OpLabel, { 79 });
		m.op(OpReturn, {});
		m.op(OpFunctionEnd, {});

		sc::Compiler compiler(m.build());
		auto accesses = compiler.get_buffer_byte_accesses(40);
		// Both members are written (select) and read (phi / copy object).
		CHECK_RANGE_INVARIANTS_AND_COUNT(accesses, 2);
		CHECK_RANGE(accesses, 0, 0, 4, true, true);
		CHECK_RANGE(accesses, 1, 16, 4, true, true);
	}

	// --- Module C: loop-carried pointer Phi with a cyclic back edge. ---------------------------
	// The pointer Phi's back-edge operand is an OpCopyObject of the Phi itself, so resolution must
	// reach a fixed point instead of recursing forever, and loop iterations must not multiply the
	// accessed byte set.
	{
		ModuleBuilder lp(220);
		emit_prologue(lp);
		lp.entry_point(ExecutionModelFragment, 20, "main");

		lp.op(OpTypeVoid, { 1 });
		lp.op(OpTypeFunction, { 2, 1 });
		lp.op(OpTypeFloat, { 3, 32 });
		lp.op(OpTypeInt, { 4, 32, 0 });
		lp.op(OpTypeBool, { 5 });
		lp.op(OpTypeStruct, { 6, 3, 3 });
		lp.op(OpTypePointer, { 7, StorageClassUniform, 6 });
		lp.op(OpTypePointer, { 8, StorageClassUniform, 3 });

		lp.op(OpDecorate, { 6, DecorationBlock });
		lp.op(OpMemberDecorate, { 6, 0, DecorationOffset, 0 });
		lp.op(OpMemberDecorate, { 6, 1, DecorationOffset, 16 });
		lp.op(OpConstant, { 4, 30, 0 });
		lp.op(OpUndef, { 5, 33 });
		lp.op(OpVariable, { 7, 40, StorageClassUniform });

		lp.op(OpFunction, { 1, 20, 0, 2 });

		// Entry block: seed pointer to member 0, then enter the loop header.
		lp.op(OpLabel, { 21 });
		lp.op(OpAccessChain, { 8, 60, 40, 30 });
		lp.op(OpBranch, { 22 });

		// Loop header: pointer Phi merges the seed and the cyclic back-edge pointer.
		lp.op(OpLabel, { 22 });
		lp.op(OpPhi, { 8, 70, 60, 21, 73, 23 });
		lp.op(OpLoopMerge, { 24, 23, 0 });
		lp.op(OpLoad, { 3, 71, 70 });
		lp.op(OpBranch, { 23 });

		// Continue block: feed a copy of the Phi pointer back around the loop.
		lp.op(OpLabel, { 23 });
		lp.op(OpCopyObject, { 8, 73, 70 });
		lp.op(OpBranchConditional, { 33, 22, 24 });

		// Merge block.
		lp.op(OpLabel, { 24 });
		lp.op(OpReturn, {});
		lp.op(OpFunctionEnd, {});

		sc::Compiler compiler(lp.build());
		auto accesses = compiler.get_buffer_byte_accesses(40);
		// Member 0 is read exactly once regardless of how many loop iterations are imagined.
		CHECK_RANGE_INVARIANTS_AND_COUNT(accesses, 1);
		CHECK_RANGE(accesses, 0, 0, 4, true, false);
	}

	return unit_checks;
}

// ---------------------------------------------------------------------------------------------
// compat group:
//   * legacy get_active_buffer_ranges() keeps returning whole top-level members,
//   * the new interface reports the precise scalar bytes for the same module,
//   * repeated queries and entry-point switching are independent and stable,
//   * unsupported features on the queried resource throw, while the same features on an
//     unrelated resource leave the query intact.
// ---------------------------------------------------------------------------------------------
std::vector<uint32_t> make_legacy_module()
{
	ModuleBuilder m(64);
	m.op(OpCapability, { CapabilityShader });
	m.op(OpMemoryModel, { AddressingModelLogical, MemoryModelGLSL450 });
	m.entry_point(ExecutionModelFragment, 20, "main");
	m.op(OpExecutionMode, { 20, ExecutionModeOriginUpperLeft });

	m.op(OpTypeVoid, { 1 });
	m.op(OpTypeFunction, { 2, 1 });
	m.op(OpTypeFloat, { 3, 32 });
	m.op(OpTypeInt, { 4, 32, 0 });
	m.op(OpConstant, { 4, 5, 4 });
	m.op(OpTypeArray, { 6, 3, 5 });
	m.op(OpTypeStruct, { 7, 6, 3 });
	m.op(OpTypePointer, { 8, StorageClassUniform, 7 });
	m.op(OpTypePointer, { 9, StorageClassUniform, 3 });
	m.op(OpVariable, { 8, 10, StorageClassUniform });
	m.op(OpConstant, { 4, 11, 0 });
	m.op(OpConstant, { 4, 12, 2 });

	m.op(OpDecorate, { 6, DecorationArrayStride, 16 });
	m.op(OpDecorate, { 7, DecorationBlock });
	m.op(OpMemberDecorate, { 7, 0, DecorationOffset, 0 });
	m.op(OpMemberDecorate, { 7, 1, DecorationOffset, 64 });
	m.op(OpDecorate, { 10, DecorationDescriptorSet, 0 });
	m.op(OpDecorate, { 10, DecorationBinding, 0 });

	m.op(OpFunction, { 1, 20, 0, 2 });
	m.op(OpLabel, { 21 });
	m.op(OpAccessChain, { 9, 22, 10, 11, 12 });
	m.op(OpLoad, { 3, 23, 22 });
	m.op(OpReturn, {});
	m.op(OpFunctionEnd, {});
	return m.build();
}

int run_compat_group()
{
	// Legacy interface is unchanged while the new interface is byte-accurate.
	sc::Compiler legacy(make_legacy_module());
	auto old_ranges = legacy.get_active_buffer_ranges(10);
	UNIT_CHECK(old_ranges.size() == 1);
	UNIT_CHECK(old_ranges[0].index == 0 && old_ranges[0].offset == 0 && old_ranges[0].range == 64);
	UNIT_CHECK(legacy.get_declared_struct_size(legacy.get_type(7)) == 68);

	auto precise = legacy.get_buffer_byte_accesses(10);
	CHECK_RANGE_INVARIANTS_AND_COUNT(precise, 1);
	CHECK_RANGE(precise, 0, 32, 4, true, false);

	// Multi-entry module: entry A reads member 0, entry B writes member 1.
	ModuleBuilder me(128);
	me.op(OpCapability, { CapabilityShader });
	me.op(OpMemoryModel, { AddressingModelLogical, MemoryModelGLSL450 });
	me.entry_point(ExecutionModelVertex, 80, "a");
	me.entry_point(ExecutionModelFragment, 81, "b");

	me.op(OpTypeVoid, { 1 });
	me.op(OpTypeFunction, { 2, 1 });
	me.op(OpTypeFloat, { 3, 32 });
	me.op(OpTypeInt, { 4, 32, 0 });
	me.op(OpTypeStruct, { 5, 3, 3 });
	me.op(OpTypePointer, { 6, StorageClassStorageBuffer, 5 });
	me.op(OpTypePointer, { 7, StorageClassStorageBuffer, 3 });
	me.op(OpDecorate, { 5, DecorationBufferBlock });
	me.op(OpMemberDecorate, { 5, 0, DecorationOffset, 0 });
	me.op(OpMemberDecorate, { 5, 1, DecorationOffset, 16 });
	me.op(OpConstant, { 4, 10, 0 });
	me.op(OpConstant, { 4, 11, 1 });
	me.op(OpConstant, { 3, 12, float_bits(1.0f) });
	me.op(OpVariable, { 6, 20, StorageClassStorageBuffer });

	// Entry A: read member 0.
	me.op(OpFunction, { 1, 80, 0, 2 });
	me.op(OpLabel, { 82 });
	me.op(OpAccessChain, { 7, 83, 20, 10 });
	me.op(OpLoad, { 3, 84, 83 });
	me.op(OpReturn, {});
	me.op(OpFunctionEnd, {});

	// Entry B: write member 1.
	me.op(OpFunction, { 1, 81, 0, 2 });
	me.op(OpLabel, { 85 });
	me.op(OpAccessChain, { 7, 86, 20, 11 });
	me.op(OpStore, { 86, 12 });
	me.op(OpReturn, {});
	me.op(OpFunctionEnd, {});

	sc::Compiler entries(me.build());
	auto a_first = entries.get_buffer_byte_accesses(20);
	CHECK_RANGE_INVARIANTS_AND_COUNT(a_first, 1);
	CHECK_RANGE(a_first, 0, 0, 4, true, false);

	entries.set_entry_point("b", ExecutionModelFragment);
	auto b_accesses = entries.get_buffer_byte_accesses(20);
	CHECK_RANGE_INVARIANTS_AND_COUNT(b_accesses, 1);
	CHECK_RANGE(b_accesses, 0, 16, 4, false, true);

	entries.set_entry_point("a", ExecutionModelVertex);
	auto a_second = entries.get_buffer_byte_accesses(20);
	CHECK_RANGE_INVARIANTS_AND_COUNT(a_second, 1);
	CHECK_RANGE(a_second, 0, 0, 4, true, false);
	auto a_third = entries.get_buffer_byte_accesses(20);
	CHECK_RANGE_INVARIANTS_AND_COUNT(a_third, 1);
	CHECK_RANGE(a_third, 0, 0, 4, true, false);

	// Legacy reflection still behaves on the multi-entry module.
	auto legacy_entries = entries.get_active_buffer_ranges(20);
	UNIT_CHECK(legacy_entries.size() == 1 && legacy_entries[0].index == 0);

	// Consecutive queries of distinct resources on one compiler stay independent and repeatable.
	ModuleBuilder two(150);
	two.op(OpCapability, { CapabilityShader });
	two.op(OpMemoryModel, { AddressingModelLogical, MemoryModelGLSL450 });
	two.entry_point(ExecutionModelFragment, 20, "main");
	two.op(OpExecutionMode, { 20, ExecutionModeOriginUpperLeft });
	two.op(OpTypeVoid, { 1 });
	two.op(OpTypeFunction, { 2, 1 });
	two.op(OpTypeFloat, { 3, 32 });
	two.op(OpTypeInt, { 4, 32, 0 });
	two.op(OpTypeStruct, { 5, 3, 3 });
	two.op(OpTypePointer, { 6, StorageClassStorageBuffer, 5 });
	two.op(OpTypePointer, { 7, StorageClassStorageBuffer, 3 });
	two.op(OpDecorate, { 5, DecorationBufferBlock });
	two.op(OpMemberDecorate, { 5, 0, DecorationOffset, 0 });
	two.op(OpMemberDecorate, { 5, 1, DecorationOffset, 16 });
	two.op(OpConstant, { 4, 10, 0 });
	two.op(OpConstant, { 4, 11, 1 });
	two.op(OpConstant, { 3, 12, float_bits(1.0f) });
	two.op(OpVariable, { 6, 40, StorageClassStorageBuffer });
	two.op(OpVariable, { 6, 41, StorageClassStorageBuffer });

	two.op(OpFunction, { 1, 20, 0, 2 });
	two.op(OpLabel, { 21 });
	two.op(OpAccessChain, { 7, 90, 40, 10 }); // read resource 40, member 0
	two.op(OpLoad, { 3, 91, 90 });
	two.op(OpAccessChain, { 7, 92, 41, 11 }); // write resource 41, member 1
	two.op(OpStore, { 92, 12 });
	two.op(OpReturn, {});
	two.op(OpFunctionEnd, {});

	sc::Compiler both(two.build());
	auto res40_first = both.get_buffer_byte_accesses(40);
	CHECK_RANGE_INVARIANTS_AND_COUNT(res40_first, 1);
	CHECK_RANGE(res40_first, 0, 0, 4, true, false);
	auto res41 = both.get_buffer_byte_accesses(41);
	CHECK_RANGE_INVARIANTS_AND_COUNT(res41, 1);
	CHECK_RANGE(res41, 0, 16, 4, false, true);
	auto res40_second = both.get_buffer_byte_accesses(40);
	CHECK_RANGE_INVARIANTS_AND_COUNT(res40_second, 1);
	CHECK_RANGE(res40_second, 0, 0, 4, true, false);

	// Unsupported features touching the queried resource throw through the normal error channel.
	// Module with two resources: variable 40 is queried, variable 41 carries the unsupported ops.
	ModuleBuilder bad(160);
	bad.op(OpCapability, { CapabilityShader });
	bad.op(OpMemoryModel, { AddressingModelLogical, MemoryModelGLSL450 });
	bad.entry_point(ExecutionModelFragment, 20, "main");
	bad.op(OpExecutionMode, { 20, ExecutionModeOriginUpperLeft });

	bad.op(OpTypeVoid, { 1 });
	bad.op(OpTypeFunction, { 2, 1 });
	bad.op(OpTypeFloat, { 3, 32 });
	bad.op(OpTypeInt, { 4, 32, 0 });
	bad.op(OpTypeStruct, { 5, 3 });
	bad.op(OpTypePointer, { 6, StorageClassStorageBuffer, 5 });
	bad.op(OpTypePointer, { 7, StorageClassStorageBuffer, 3 });
	bad.op(OpDecorate, { 5, DecorationBufferBlock });
	bad.op(OpMemberDecorate, { 5, 0, DecorationOffset, 0 });
	bad.op(OpConstant, { 4, 10, 0 });
	bad.op(OpConstant, { 4, 11, 1 }); // Device scope
	bad.op(OpConstant, { 4, 12, 0 }); // memory semantics
	bad.op(OpConstant, { 3, 13, float_bits(3.0f) });
	bad.op(OpVariable, { 6, 40, StorageClassStorageBuffer });
	bad.op(OpVariable, { 6, 41, StorageClassStorageBuffer });

	bad.op(OpFunction, { 1, 20, 0, 2 });
	bad.op(OpLabel, { 21 });
	// Ordinary read of the queried resource.
	bad.op(OpAccessChain, { 7, 90, 40, 10 });
	bad.op(OpLoad, { 3, 91, 90 });
	// Atomic on the unrelated resource: must not disturb the query for variable 40.
	bad.op(OpAccessChain, { 7, 92, 41, 10 });
	bad.op(OpAtomicStore, { 92, 11, 12, 13 });
	bad.op(OpReturn, {});
	bad.op(OpFunctionEnd, {});

	sc::Compiler unsupported(bad.build());
	auto clean = unsupported.get_buffer_byte_accesses(40);
	CHECK_RANGE_INVARIANTS_AND_COUNT(clean, 1);
	CHECK_RANGE(clean, 0, 0, 4, true, false);
	UNIT_CHECK(throws_compiler_error([&] { (void)unsupported.get_buffer_byte_accesses(41); }));

	// Pointer arithmetic (OpPtrAccessChain) and runtime arrays throw when they reach the resource.
	ModuleBuilder ptr_arith(180);
	ptr_arith.op(OpCapability, { CapabilityShader });
	ptr_arith.op(OpMemoryModel, { AddressingModelLogical, MemoryModelGLSL450 });
	ptr_arith.entry_point(ExecutionModelFragment, 20, "main");
	ptr_arith.op(OpExecutionMode, { 20, ExecutionModeOriginUpperLeft });
	ptr_arith.op(OpTypeVoid, { 1 });
	ptr_arith.op(OpTypeFunction, { 2, 1 });
	ptr_arith.op(OpTypeFloat, { 3, 32 });
	ptr_arith.op(OpTypeInt, { 4, 32, 0 });
	ptr_arith.op(OpTypeStruct, { 5, 3 });
	ptr_arith.op(OpTypePointer, { 6, StorageClassStorageBuffer, 5 });
	ptr_arith.op(OpTypePointer, { 7, StorageClassStorageBuffer, 3 });
	ptr_arith.op(OpDecorate, { 5, DecorationBufferBlock });
	ptr_arith.op(OpMemberDecorate, { 5, 0, DecorationOffset, 0 });
	ptr_arith.op(OpConstant, { 4, 10, 0 });
	ptr_arith.op(OpConstant, { 3, 13, float_bits(3.0f) });
	ptr_arith.op(OpVariable, { 6, 40, StorageClassStorageBuffer });
	ptr_arith.op(OpFunction, { 1, 20, 0, 2 });
	ptr_arith.op(OpLabel, { 21 });
	ptr_arith.op(OpPtrAccessChain, { 7, 90, 40, 10, 10 });
	ptr_arith.op(OpStore, { 90, 13 });
	ptr_arith.op(OpReturn, {});
	ptr_arith.op(OpFunctionEnd, {});

	sc::Compiler pointer_module(ptr_arith.build());
	UNIT_CHECK(throws_compiler_error([&] { (void)pointer_module.get_buffer_byte_accesses(40); }));

	ModuleBuilder runtime(190);
	runtime.op(OpCapability, { CapabilityShader });
	runtime.op(OpMemoryModel, { AddressingModelLogical, MemoryModelGLSL450 });
	runtime.entry_point(ExecutionModelFragment, 20, "main");
	runtime.op(OpExecutionMode, { 20, ExecutionModeOriginUpperLeft });
	runtime.op(OpTypeVoid, { 1 });
	runtime.op(OpTypeFunction, { 2, 1 });
	runtime.op(OpTypeFloat, { 3, 32 });
	runtime.op(OpTypeInt, { 4, 32, 0 });
	runtime.op(OpTypeRuntimeArray, { 8, 3 });
	runtime.op(OpTypeStruct, { 5, 8 });
	runtime.op(OpTypePointer, { 6, StorageClassStorageBuffer, 5 });
	runtime.op(OpTypePointer, { 7, StorageClassStorageBuffer, 3 });
	runtime.op(OpDecorate, { 5, DecorationBufferBlock });
	runtime.op(OpMemberDecorate, { 5, 0, DecorationOffset, 0 });
	runtime.op(OpDecorate, { 8, DecorationArrayStride, 4 });
	runtime.op(OpConstant, { 4, 10, 0 });
	runtime.op(OpVariable, { 6, 40, StorageClassStorageBuffer });
	runtime.op(OpFunction, { 1, 20, 0, 2 });
	runtime.op(OpLabel, { 21 });
	runtime.op(OpAccessChain, { 7, 90, 40, 10, 10 });
	runtime.op(OpLoad, { 3, 91, 90 });
	runtime.op(OpReturn, {});
	runtime.op(OpFunctionEnd, {});

	sc::Compiler runtime_module(runtime.build());
	UNIT_CHECK(throws_compiler_error([&] { (void)runtime_module.get_buffer_byte_accesses(40); }));

	// A reachable self-recursive function that touches the resource must fail loudly rather than
	// return a partial result.
	ModuleBuilder rec(200);
	rec.op(OpCapability, { CapabilityShader });
	rec.op(OpMemoryModel, { AddressingModelLogical, MemoryModelGLSL450 });
	rec.entry_point(ExecutionModelFragment, 20, "main");
	rec.op(OpExecutionMode, { 20, ExecutionModeOriginUpperLeft });
	rec.op(OpTypeVoid, { 1 });
	rec.op(OpTypeFunction, { 2, 1 });
	rec.op(OpTypeFloat, { 3, 32 });
	rec.op(OpTypeInt, { 4, 32, 0 });
	rec.op(OpTypeStruct, { 5, 3 });
	rec.op(OpTypePointer, { 6, StorageClassStorageBuffer, 5 });
	rec.op(OpTypePointer, { 7, StorageClassStorageBuffer, 3 });
	rec.op(OpDecorate, { 5, DecorationBufferBlock });
	rec.op(OpMemberDecorate, { 5, 0, DecorationOffset, 0 });
	rec.op(OpConstant, { 4, 10, 0 });
	rec.op(OpVariable, { 6, 40, StorageClassStorageBuffer });

	// Self-recursive helper: reads the resource then invokes itself.
	rec.op(OpFunction, { 1, 50, 0, 2 });
	rec.op(OpLabel, { 52 });
	rec.op(OpAccessChain, { 7, 53, 40, 10 });
	rec.op(OpLoad, { 3, 54, 53 });
	rec.op(OpFunctionCall, { 1, 55, 50 });
	rec.op(OpReturn, {});
	rec.op(OpFunctionEnd, {});

	rec.op(OpFunction, { 1, 20, 0, 2 });
	rec.op(OpLabel, { 21 });
	rec.op(OpFunctionCall, { 1, 56, 50 });
	rec.op(OpReturn, {});
	rec.op(OpFunctionEnd, {});

	sc::Compiler recursive_module(rec.build());
	UNIT_CHECK(throws_compiler_error([&] { (void)recursive_module.get_buffer_byte_accesses(40); }));

	return unit_checks;
}
} // namespace

int unit_run_group(const char *name)
{
	if (std::strcmp(name, "layout") == 0)
		return run_layout_group();
	if (std::strcmp(name, "aliases") == 0)
		return run_aliases_group();
	if (std::strcmp(name, "compat") == 0)
		return run_compat_group();
	return -1;
}
