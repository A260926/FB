#ifndef __SPIRV_VM_WORKGROUP_H__
#define __SPIRV_VM_WORKGROUP_H__

#include <spvm/state.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/*
 * Workgroup executor.
 *
 * A spvm_workgroup runs a fixed-size group of spvm_state invocations of one
 * compute entry point, sharing its Workgroup storage class variables and
 * rendezvousing at OpControlBarrier. Private, Function, Input/Output storage
 * and the call stack stay per invocation; Workgroup scalars, vectors,
 * fixed-length arrays and structs (including every nested member/alias) are
 * one shared, zero-initialized copy for the whole group.
 *
 * The executor only drives the per-state stepping rules, it never runs a
 * state to completion in order to fake synchronization: invocations are
 * advanced one instruction at a time and block at the instruction following
 * a barrier until every invocation has reached the same barrier rendezvous.
 */

/* Maximum supported nested function-call depth at a barrier site. */
#define SPVM_WORKGROUP_MAX_CALL_DEPTH 64

typedef struct spvm_workgroup* spvm_workgroup_t;

typedef enum spvm_wg_status {
	/* the invocation executed exactly one instruction and is still active */
	SPVM_WG_STATUS_RUNNING = 0,

	/* the invocation is blocked at a barrier, no instruction was executed */
	SPVM_WG_STATUS_WAITING,

	/* the invocation has executed OpReturn and its state has no code left */
	SPVM_WG_STATUS_FINISHED,

	/* a barrier used an unsupported execution/memory scope or semantics */
	SPVM_WG_STATUS_ERROR_BARRIER_SCOPE,

	/* invocations reached different barrier sites in the same dynamic round */
	SPVM_WG_STATUS_ERROR_BARRIER_MISMATCH,

	/* an invocation finished while another one was still waiting at a barrier */
	SPVM_WG_STATUS_ERROR_PEER_FINISHED,

	/* an invalid argument was passed to the workgroup API */
	SPVM_WG_STATUS_ERROR_INVALID_ARGUMENT,

	/* the module uses a feature the workgroup executor does not support */
	SPVM_WG_STATUS_ERROR_UNSUPPORTED
} spvm_wg_status;

/*
 * Creates a workgroup for the single compute entry point of prog.
 * The three-dimensional local size is read from the module's numeric
 * OpExecutionMode LocalSize and invocations are laid out x-fastest:
 *     index = z * size_x * size_y + y * size_x + x
 * group_x/y/z is the WorkgroupId the invocations report through builtins.
 *
 * Only modules with one GLCompute entry point, a fixed numeric LocalSize and
 * no derivative groups, images, runtime arrays, atomics or recursion are
 * supported. Returns NULL when the program is not a supported module.
 */
spvm_workgroup_t spvm_workgroup_create(spvm_program_t prog,
	spvm_word group_x, spvm_word group_y, spvm_word group_z);

/* Number of invocations in the group (size_x * size_y * size_z). */
spvm_word spvm_workgroup_invocation_count(spvm_workgroup_t wg);

/* The state of one invocation; stays valid for the workgroup's lifetime. */
spvm_state_t spvm_workgroup_get_state(spvm_workgroup_t wg, spvm_word invocation);

/*
 * Advances one invocation by a single instruction.
 *
 * A waiting invocation executes nothing and keeps reporting
 * SPVM_WG_STATUS_WAITING (its instruction_count does not grow). A finished
 * invocation keeps reporting SPVM_WG_STATUS_FINISHED. When the last
 * invocation reaches a barrier, the whole current round is released and the
 * waiting invocations resume, one instruction at a time, on their next step.
 */
spvm_wg_status spvm_workgroup_step(spvm_workgroup_t wg, spvm_word invocation);

/*
 * Drives the whole group until every invocation has finished, reusing the
 * exact spvm_workgroup_step() rules. Returns SPVM_WG_STATUS_FINISHED on
 * success or the first error status encountered.
 */
spvm_wg_status spvm_workgroup_run(spvm_workgroup_t wg);

/* Dynamic barrier round the group is currently in (0 before the first barrier). */
spvm_word spvm_workgroup_current_round(spvm_workgroup_t wg);

/* Human-readable diagnostic for the last error, or NULL when there is none. */
const char* spvm_workgroup_error(spvm_workgroup_t wg);

/*
 * Frees only this workgroup's storage. Shared member aliases are released
 * once by the owning invocation; other workgroups built from the same
 * program keep their own independent storage and remain valid.
 */
void spvm_workgroup_delete(spvm_workgroup_t wg);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // __SPIRV_VM_WORKGROUP_H__
