#ifndef __SPIRV_VM_SPECIALIZE_H__
#define __SPIRV_VM_SPECIALIZE_H__

#include <spvm/types.h>
#include <spvm/program.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/* One specialization override.
 *
 * spec_id is the literal number carried by an OpDecorate SpecId, NOT a
 * SPIR-V result ID. bits is the raw 32-bit pattern to install:
 *   - int/uint scalars: the 32-bit integer value (reinterpreted per type)
 *   - float32 scalars: the IEEE-754 bit pattern (e.g. 0x80000000 for -0.0)
 *   - bool scalars: only 0 or 1 are accepted
 * Entries targeting ids that no decorated definition binds to, duplicate
 * entries, and bool entries whose bits are not 0/1 are rejected.
 */
typedef struct spvm_spec_entry {
	spvm_word spec_id;
	uint32_t bits;
} spvm_spec_entry;

/* Error codes returned through spvm_spec_error. SPVM_SPEC_OK (0) is success.
 * Every failure returns NULL from spvm_program_create_specialized and never
 * leaves a partially constructed program behind. result_id identifies the
 * definition involved in the failure; it is 0 when the failure has no single
 * associated result ID (bad module header, an unknown spec id, ...).
 */
enum spvm_spec_error_code {
	SPVM_SPEC_OK = 0,

	/* The module could not be parsed at all (bad magic, truncated header). */
	SPVM_SPEC_INVALID_MODULE = 1,

	/* The same spec_id appears more than once in the entry table. */
	SPVM_SPEC_DUPLICATE_ENTRY = 2,

	/* No definition in the module carries an OpDecorate SpecId with that
	 * spec_id. result_id is 0. */
	SPVM_SPEC_UNKNOWN_ID = 3,

	/* Two different definitions carry an OpDecorate SpecId with the same
	 * spec_id. result_id is the second definition's result ID. */
	SPVM_SPEC_AMBIGUOUS_ID = 4,

	/* An override value is illegal for its target type (bool bits not 0/1),
	 * or a scalar specialization constant is not a 32-bit int/uint/float or
	 * bool. result_id is the offending definition. */
	SPVM_SPEC_INVALID_VALUE = 5,

	/* A specialization value, its operands or an array/struct member uses a
	 * type outside the supported universe: matrices, 64-bit (or other
	 * non-32-bit) scalars, images/samplers, ... result_id is the offending
	 * definition. */
	SPVM_SPEC_UNSUPPORTED_TYPE = 6,

	/* An OpSpecConstantOp wraps an opcode the specializer does not evaluate
	 * (this includes every float arithmetic opcode). result_id is the
	 * OpSpecConstantOp result ID. */
	SPVM_SPEC_UNSUPPORTED_OP = 7,

	/* A dependency does not resolve to a constant definition, references an
	 * unknown result ID, or a shuffle uses an undefined (0xFFFFFFFF)
	 * component selector. result_id is the definition holding the bad
	 * reference. */
	SPVM_SPEC_BAD_REFERENCE = 8,

	/* The specialization dependency graph contains a cycle. result_id is the
	 * result ID at which the cycle was detected. */
	SPVM_SPEC_CYCLE = 9,

	/* A shape rule is violated: an array length is not positive, a composite
	 * constituent/extract/insert index is out of bounds, operands of an op
	 * have mismatched shapes, or a variable initializer does not match the
	 * variable's type. result_id is the offending type, op or variable. */
	SPVM_SPEC_BAD_SHAPE = 10,

	/* An integer UDiv/SDiv has a zero divisor. result_id is the
	 * OpSpecConstantOp result ID. */
	SPVM_SPEC_DIVISION_BY_ZERO = 11,

	/* A signed division computes INT32_MIN / -1, which has no representable
	 * result. result_id is the OpSpecConstantOp result ID. */
	SPVM_SPEC_INT_OVERFLOW = 12,

	/* Host memory allocation failed. result_id is 0. */
	SPVM_SPEC_ALLOCATION = 13
};

typedef struct spvm_spec_error {
	int code;
	spvm_word result_id;
} spvm_spec_error;

/* Create a program from a SPIR-V module with specialization constants
 * resolved.
 *
 * The whole specialization subsystem is evaluated up front: leaf
 * OpSpecConstant/OpSpecConstantTrue/OpSpecConstantFalse values (overridden
 * from entries where present, default values otherwise), derived
 * OpSpecConstantComposite/OpSpecConstantOp values, and every array/struct
 * shape and Private initializer that depends on them. The returned program
 * runs on the ordinary state/executor path and behaves exactly like a module
 * in which the same values had been written as plain constants.
 *
 * The program takes ownership of everything it needs: on success the caller
 * may release words and entries immediately. Programs created from the same
 * source and different entry tables are fully independent and may be used
 * interleaved within one context.
 *
 * error may be NULL. On failure the return value is NULL and *error (when
 * non-NULL) describes the first problem encountered. An empty entry table is
 * valid and still forces evaluation of every default specialization value.
 */
spvm_program_t spvm_program_create_specialized(spvm_context_t ctx, spvm_source words, size_t word_count,
	const spvm_spec_entry *entries, size_t entry_count, spvm_spec_error *error);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // __SPIRV_VM_SPECIALIZE_H__
