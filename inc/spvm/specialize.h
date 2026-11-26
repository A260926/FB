#ifndef __SPIRV_VM_SPECIALIZE_H__
#define __SPIRV_VM_SPECIALIZE_H__

#include <spvm/types.h>
#include <spvm/program.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/* A single specialization override.
 * spec_id is the number carried by an OpDecorate SpecId, NOT a SPIR-V
 * result <id>. bits is the raw 32-bit pattern of the override:
 *  - 32-bit signed/unsigned integers: the integer value
 *  - float32: the IEEE-754 bit pattern (memcpy, not a numeric cast)
 *  - bool: only 0 or 1 are accepted
 */
typedef struct spvm_spec_entry {
	spvm_word spec_id;
	uint32_t bits;
} spvm_spec_entry;

enum spvm_spec_error_code {
	SPVM_SPEC_OK = 0,                 /* success */
	SPVM_SPEC_ERR_MALFORMED = 1,      /* header is missing or the instruction stream is truncated */
	SPVM_SPEC_ERR_DUPLICATE_ENTRY = 2,/* the same spec_id appears more than once in entries */
	SPVM_SPEC_ERR_UNKNOWN_SPEC_ID = 3,/* no definition carries a SpecId decoration with that number */
	SPVM_SPEC_ERR_AMBIGUOUS_SPEC_ID = 4, /* one spec_id is bound to more than one definition */
	SPVM_SPEC_ERR_INVALID_BOOL = 5,   /* bool specialization override is neither 0 nor 1 */
	SPVM_SPEC_ERR_UNSUPPORTED_TYPE = 6, /* matrices, 64-bit types, pointers/images/... used by a spec value */
	SPVM_SPEC_ERR_UNSUPPORTED_OP = 7, /* OpSpecConstantOp wraps an opcode outside the supported set */
	SPVM_SPEC_ERR_UNKNOWN_REF = 8,    /* a dependency points at an id that is not an evaluated constant */
	SPVM_SPEC_ERR_CYCLE = 9,          /* the specialization dependency graph contains a cycle */
	SPVM_SPEC_ERR_SHAPE_MISMATCH = 10,/* operands/results do not share the required type shape */
	SPVM_SPEC_ERR_INDEX_OUT_OF_RANGE = 11, /* composite extract/insert literal or shuffle selector is out of range */
	SPVM_SPEC_ERR_DIVIDE_BY_ZERO = 12,/* UDiv/SDiv by a zero divisor */
	SPVM_SPEC_ERR_SIGNED_OVERFLOW = 13,  /* SDiv INT_MIN / -1 */
	SPVM_SPEC_ERR_BAD_ARRAY_LENGTH = 14, /* specialization-derived array length is not a positive integer */
	SPVM_SPEC_ERR_ALLOCATION = 15     /* out of memory while building the specialized program */
};

typedef struct spvm_spec_error {
	int code;        /* one of spvm_spec_error_code; SPVM_SPEC_OK on success */
	spvm_word result_id; /* result <id> of the definition involved in the failure, 0 when none applies */
} spvm_spec_error;

/* Build an independent, fully specialized program from the same SPIR-V module.
 *
 * Specialization constants (and the OpSpecConstantComposite / OpSpecConstantOp
 * chain built on top of them, ordinary constants included) are evaluated up
 * front and baked into an owned copy of the source, so every array length,
 * aggregate member and Private initializer already has its final shape when a
 * state is created. The returned program runs on the existing executor.
 *
 * On success the program owns every buffer it needs: words and entries may be
 * released by the caller immediately. On failure NULL is returned and, when
 * error is non-NULL, *error describes the first failure. Passing a NULL error
 * is allowed. */
spvm_program_t spvm_program_create_specialized(spvm_context_t ctx, spvm_source words, size_t word_count,
	const spvm_spec_entry* entries, size_t entry_count, spvm_spec_error* error);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // __SPIRV_VM_SPECIALIZE_H__
