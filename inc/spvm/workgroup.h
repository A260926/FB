#ifndef __SPIRV_VM_WORKGROUP_H__
#define __SPIRV_VM_WORKGROUP_H__

#include <spvm/state.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/*
 * Offline workgroup execution for compute shaders.
 *
 * A spvm_workgroup owns one spvm_state per local invocation.  Private,
 * Function, Input and Output storage is per invocation.  Storage declared in
 * the Workgroup storage class is allocated once by the workgroup and shared
 * by every invocation, so aliases created through OpAccessChain,
 * OpCopyMemory or pointer parameters of called functions all observe the
 * same bytes.  The shared storage is zero initialized on creation.
 *
 * Only modules with exactly one GLCompute entry point and a numeric
 * OpExecutionMode LocalSize are accepted.  Runtime arrays, images, samplers
 * and atomic instructions are rejected.  Recursive calls are not supported.
 *
 * OpControlBarrier is a rendezvous point.  An invocation that reaches one
 * stops *before the instruction following the barrier* (the barrier itself
 * has already executed).  All invocations rendezvous only if they arrive in
 * the same dynamic round, at the same static barrier instruction and with
 * the same sequence of call-stack return addresses.  Otherwise the step/run
 * call fails with a diagnostic that identifies the invocation, the code
 * position and the round.
 *
 * The only supported barrier is one whose execution scope and memory scope
 * are both Workgroup and whose memory semantics are exactly
 * AcquireRelease|WorkgroupMemory.
 */

typedef struct spvm_workgroup spvm_workgroup;
typedef spvm_workgroup* spvm_workgroup_t;

typedef enum spvm_workgroup_error_code {
	SPVM_WORKGROUP_ERROR_NONE = 0,

	/* creation failed */
	SPVM_WORKGROUP_ERROR_INVALID_ARGUMENT,
	SPVM_WORKGROUP_ERROR_INVALID_PROGRAM,
	SPVM_WORKGROUP_ERROR_OUT_OF_MEMORY,
	SPVM_WORKGROUP_ERROR_UNSUPPORTED_FEATURE,

	/* execution failed */
	SPVM_WORKGROUP_ERROR_UNSUPPORTED_BARRIER,
	SPVM_WORKGROUP_ERROR_BARRIER_SITE_MISMATCH,
	SPVM_WORKGROUP_ERROR_BARRIER_STACK_MISMATCH,
	SPVM_WORKGROUP_ERROR_BARRIER_ROUND_MISMATCH,
	SPVM_WORKGROUP_ERROR_FINISHED_WHILE_WAITING,
	SPVM_WORKGROUP_ERROR_DEADLOCK
} spvm_workgroup_error_code;

typedef struct spvm_workgroup_error {
	spvm_workgroup_error_code code;

	/* Invocation the error is attributed to, -1 when not applicable. */
	spvm_word invocation;
	/* Word offset of the relevant instruction in the program, -1 if none. */
	spvm_word instruction;
	/* Dynamic barrier round (zero based), -1 if not applicable. */
	spvm_word round;
	/* Word offset of the barrier that could not be matched, -1 if none. */
	spvm_word barrier;

	char message[256];
} spvm_workgroup_error;

typedef enum spvm_workgroup_step_result {
	SPVM_WORKGROUP_STEP_ERROR = -1,
	/* One instruction was executed and the invocation can continue. */
	SPVM_WORKGROUP_STEP_OK = 0,
	/* The invocation executed a barrier and is waiting for the group. */
	SPVM_WORKGROUP_STEP_WAITING = 1,
	/* The invocation executed the last arrival of a barrier; all
	 * invocations of the round were released. */
	SPVM_WORKGROUP_STEP_RELEASED = 2,
	/* The invocation returned and is finished. */
	SPVM_WORKGROUP_STEP_FINISHED = 3
} spvm_workgroup_step_result;

typedef enum spvm_workgroup_result {
	SPVM_WORKGROUP_RESULT_ERROR = -1,
	/* Every invocation finished. */
	SPVM_WORKGROUP_RESULT_FINISHED = 0
} spvm_workgroup_result;

/*
 * Create a workgroup for the given workgroup coordinates.  workgroup_id must
 * point to three values (x, y, z).  Local invocations are created in
 * x-fastest order: LocalInvocationIndex = x + LocalSizeX * (y + LocalSizeY * z).
 * On failure NULL is returned; when error is not NULL it is filled with a
 * diagnostic.
 */
spvm_workgroup_t spvm_workgroup_create(spvm_program_t program,
	const spvm_word workgroup_id[3], spvm_workgroup_error* error);

/* Release the group and all invocation states.  Other workgroups created
 * from the same program are not affected. */
void spvm_workgroup_delete(spvm_workgroup_t workgroup);

unsigned spvm_workgroup_get_invocation_count(spvm_workgroup_t workgroup);

/* Returns NULL for an out of range invocation. */
spvm_state_t spvm_workgroup_get_state(spvm_workgroup_t workgroup,
	unsigned invocation);

/*
 * Execute at most one instruction of the given invocation.
 *
 * Stepping a waiting invocation does not execute anything and returns
 * SPVM_WORKGROUP_STEP_WAITING; its instruction_count does not change.
 * Stepping a finished invocation returns SPVM_WORKGROUP_STEP_FINISHED.
 *
 * The last invocation to arrive at a barrier releases the whole round; that
 * call returns SPVM_WORKGROUP_STEP_RELEASED.  Released invocations resume at
 * the instruction after the barrier on their next step.
 */
spvm_workgroup_step_result spvm_workgroup_step(spvm_workgroup_t workgroup,
	unsigned invocation, spvm_workgroup_error* error);

/*
 * Run the whole group using the same rendezvous rules as
 * spvm_workgroup_step, interleaving invocations one instruction at a time.
 * Returns SPVM_WORKGROUP_RESULT_FINISHED when every invocation returned, or
 * SPVM_WORKGROUP_RESULT_ERROR on a synchronization error.
 */
spvm_workgroup_result spvm_workgroup_run(spvm_workgroup_t workgroup,
	spvm_workgroup_error* error);

/* The last error recorded by this workgroup, or NULL.  Once an error has
 * been recorded the group is poisoned: step and run keep returning the same
 * failure until the group is deleted. */
const spvm_workgroup_error* spvm_workgroup_get_error(spvm_workgroup_t workgroup);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // __SPIRV_VM_WORKGROUP_H__
