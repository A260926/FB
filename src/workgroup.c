#include <spvm/workgroup.h>
#include <spvm/spirv.h>
#include <string.h>
#include <stdio.h>

/*
 * The whole executor is built on top of the existing per-state rules:
 * spvm_state_step_opcode() still executes every instruction. The workgroup
 * only (1) points every invocation's Workgroup variables at one shared member
 * tree and (2) intercepts the state's control_barrier callback to rendezvous.
 */

typedef struct spvm_wg_site {
	spvm_word instruction; /* word offset of the barrier instruction */
	spvm_word frame_count;
	spvm_word frames[SPVM_WORKGROUP_MAX_CALL_DEPTH]; /* frame code offsets */
} spvm_wg_site;

typedef struct spvm_wg_invocation {
	spvm_state_t state;
	spvm_byte waiting;
	spvm_byte finished;
	spvm_byte errored;
	spvm_wg_site site;
} spvm_wg_invocation;

struct spvm_workgroup {
	spvm_program_t program;
	spvm_word entry_id;

	spvm_word size_x, size_y, size_z;
	spvm_word count;
	spvm_word group_x, group_y, group_z;

	spvm_wg_invocation* invocations;

	/* barrier rendezvous state */
	spvm_word round;   /* fully completed barrier rounds */
	spvm_word arrived; /* arrivals in the current round */
	spvm_byte have_site;
	spvm_wg_site round_site;

	spvm_wg_status status;
	char error[512];
};

static void spvm_wg_set_error(spvm_workgroup_t wg, spvm_wg_status status, const char* msg)
{
	wg->status = status;
	snprintf(wg->error, sizeof(wg->error), "%s", msg);
}

static spvm_word spvm_wg_find_invocation(spvm_workgroup_t wg, spvm_state_t state)
{
	for (spvm_word i = 0; i < wg->count; i++)
		if (wg->invocations[i].state == state)
			return i;
	return (spvm_word)-1;
}

static spvm_byte spvm_wg_sites_equal(const spvm_wg_site* a, const spvm_wg_site* b)
{
	if (a->instruction != b->instruction || a->frame_count != b->frame_count)
		return 0;
	for (spvm_word i = 0; i < a->frame_count; i++)
		if (a->frames[i] != b->frames[i])
			return 0;
	return 1;
}

static void spvm_wg_capture_site(spvm_workgroup_t wg, spvm_wg_invocation* inv, spvm_word instruction)
{
	spvm_state_t state = inv->state;

	inv->site.instruction = instruction;

	/*
	 * At function_stack_current == d, function_stack[d] is the current
	 * function's own body location while function_stack[0..d-1] are the
	 * return addresses of the outer calls. Only those outer return positions
	 * identify a call site: a top-level barrier has an empty sequence, and a
	 * helper barrier reached from two different call sites differs here even
	 * though its static instruction position is identical.
	 */
	inv->site.frame_count = state->function_stack_current;
	if (inv->site.frame_count > SPVM_WORKGROUP_MAX_CALL_DEPTH)
		inv->site.frame_count = SPVM_WORKGROUP_MAX_CALL_DEPTH;

	for (spvm_word i = 0; i < inv->site.frame_count; i++)
		inv->site.frames[i] = (spvm_word)(state->function_stack[i] - wg->program->code);
}

/* OpControlBarrier callback installed on every owned state. */
static void spvm_wg_control_barrier(spvm_state_t state, spvm_word execution, spvm_word memory, spvm_word semantics)
{
	spvm_workgroup_t wg = (spvm_workgroup_t)state->user_data;
	spvm_word invocation = spvm_wg_find_invocation(wg, state);
	spvm_wg_invocation* inv = &wg->invocations[invocation];

	/* the operands are constant result ids; read their actual values */
	spvm_result_t ex_res = &state->results[execution];
	spvm_result_t mem_res = &state->results[memory];
	spvm_result_t sem_res = &state->results[semantics];

	if (ex_res->type != spvm_result_type_constant ||
		mem_res->type != spvm_result_type_constant ||
		sem_res->type != spvm_result_type_constant ||
		ex_res->members == NULL || mem_res->members == NULL || sem_res->members == NULL) {
		inv->errored = 1;
		spvm_wg_set_error(wg, SPVM_WG_STATUS_ERROR_BARRIER_SCOPE,
			"OpControlBarrier operands must be constant result ids");
		return;
	}

	spvm_word ex_val = ex_res->members[0].value.u;
	spvm_word mem_val = mem_res->members[0].value.u;
	spvm_word sem_val = sem_res->members[0].value.u;

	/* only Workgroup/Workgroup with AcquireRelease|WorkgroupMemory is supported */
	if (ex_val != SpvScopeWorkgroup || mem_val != SpvScopeWorkgroup ||
		sem_val != (SpvMemorySemanticsAcquireReleaseMask | SpvMemorySemanticsWorkgroupMemoryMask)) {
		inv->errored = 1;
		snprintf(wg->error, sizeof(wg->error),
			"invocation %u: unsupported OpControlBarrier (execution=%u memory=%u semantics=0x%x); "
			"only Workgroup/Workgroup AcquireRelease|WorkgroupMemory is supported",
			invocation, ex_val, mem_val, sem_val);
		wg->status = SPVM_WG_STATUS_ERROR_BARRIER_SCOPE;
		return;
	}

	inv->waiting = 1;
}

static spvm_byte spvm_wg_opcode_unsupported(SpvOp op)
{
	/* images and samplers, including sparse/query/footprint variants */
	if (op == SpvOpTypeImage || op == SpvOpTypeSampler || op == SpvOpTypeSampledImage ||
		op == SpvOpSampledImage || op == SpvOpImageTexelPointer ||
		(op >= SpvOpImageSampleImplicitLod && op <= SpvOpImageQuerySamples) ||
		(op >= SpvOpImageSparseSampleImplicitLod && op <= SpvOpImageSparseTexelsResident) ||
		op == SpvOpImageSparseRead || op == SpvOpImageSampleFootprintNV)
		return 1;

	/* runtime arrays have no fixed size to share */
	if (op == SpvOpTypeRuntimeArray)
		return 1;

	/* atomics */
	if ((op >= SpvOpAtomicLoad && op <= SpvOpAtomicXor) ||
		op == SpvOpAtomicFlagTestAndSet || op == SpvOpAtomicFlagClear)
		return 1;

	/* the local size has to be a numeric OpExecutionMode, not an id mode */
	if (op == SpvOpExecutionModeId)
		return 1;

	return 0;
}

/* DFS over the static function-call graph; any back edge is recursion. */
static spvm_byte spvm_wg_has_recursion(spvm_program_t prog)
{
	spvm_word bound = prog->bound;
	spvm_byte* is_function = (spvm_byte*)calloc(bound, sizeof(spvm_byte));

	/* first pass: collect function ids from a raw code walk */
	spvm_source p = prog->code;
	spvm_source end = prog->code + prog->code_length;
	while (p < end) {
		spvm_word header = *p++;
		spvm_word word_count = (header >> SpvWordCountShift) - 1;
		SpvOp op = header & SpvOpCodeMask;
		spvm_source next = p + word_count;

		if (op == SpvOpFunction && word_count >= 2)
			is_function[p[1]] = 1; /* result id is the second word */

		p = next;
	}

	/* adjacency list, bounded by the number of result ids */
	spvm_word* adj_count = (spvm_word*)calloc(bound, sizeof(spvm_word));
	spvm_word* adj_cap = (spvm_word*)calloc(bound, sizeof(spvm_word));
	spvm_word** adj = (spvm_word**)calloc(bound, sizeof(spvm_word*));
	spvm_word fn = 0;

	p = prog->code;
	while (p < end) {
		spvm_word header = *p++;
		spvm_word word_count = (header >> SpvWordCountShift) - 1;
		SpvOp op = header & SpvOpCodeMask;
		spvm_source next = p + word_count;

		if (op == SpvOpFunction && word_count >= 2)
			fn = p[1];
		else if (op == SpvOpFunctionEnd)
			fn = 0;
		else if (op == SpvOpFunctionCall && fn != 0 && word_count >= 3) {
			spvm_word callee = p[2]; /* result type, result id, function id ... */
			if (adj_count[fn] == adj_cap[fn]) {
				adj_cap[fn] = adj_cap[fn] ? adj_cap[fn] * 2 : 4;
				adj[fn] = (spvm_word*)realloc(adj[fn], adj_cap[fn] * sizeof(spvm_word));
			}
			adj[fn][adj_count[fn]++] = callee;
		}

		p = next;
	}

	/* iterative DFS with an explicit stack; color: 0 white, 1 gray, 2 black */
	spvm_byte* color = (spvm_byte*)calloc(bound, sizeof(spvm_byte));
	spvm_word* stack = (spvm_word*)malloc(bound * sizeof(spvm_word));
	spvm_byte recursive = 0;

	for (spvm_word root = 0; root < bound && !recursive; root++) {
		if (!is_function[root] || color[root] != 0)
			continue;

		spvm_word top = 0;
		stack[top++] = root;
		while (top > 0 && !recursive) {
			spvm_word node = stack[top - 1];
			if (color[node] == 0) {
				color[node] = 1;
				for (spvm_word i = 0; i < adj_count[node]; i++) {
					spvm_word child = adj[node][i];
					if (color[child] == 1) {
						recursive = 1;
						break;
					}
					if (color[child] == 0)
						stack[top++] = child;
				}
			} else {
				color[node] = 2;
				top--;
			}
		}
	}

	for (spvm_word i = 0; i < bound; i++)
		free(adj[i]);
	free(adj);
	free(adj_count);
	free(adj_cap);
	free(stack);
	free(color);
	free(is_function);
	return recursive;
}

typedef struct spvm_wg_module_info {
	spvm_word entry_count;
	spvm_word entry_id;
	SpvExecutionModel exec_model;
	spvm_word size_x, size_y, size_z;
} spvm_wg_module_info;

/*
 * Reads entry points and LocalSize straight from the immutable code words.
 * This deliberately avoids spvm_program's entry_points array, whose setup
 * pass appends a fresh entry every time a state is created and which would
 * therefore report one entry per invocation rather than one per module.
 */
static spvm_byte spvm_wg_scan_supported(spvm_program_t prog,
	spvm_wg_module_info* info, char* error, size_t error_size)
{
	memset(info, 0, sizeof(*info));
	info->size_x = info->size_y = info->size_z = 0;

	spvm_source p = prog->code;
	spvm_source end = prog->code + prog->code_length;
	while (p < end) {
		spvm_word header = *p++;
		spvm_word word_count = (header >> SpvWordCountShift) - 1;
		SpvOp op = header & SpvOpCodeMask;
		spvm_source args = p; /* operands of this instruction */

		if (spvm_wg_opcode_unsupported(op)) {
			snprintf(error, error_size, "workgroup executor: unsupported opcode %u in module", (spvm_word)op);
			return 0;
		}

		if (op == SpvOpEntryPoint && word_count >= 2) {
			info->entry_count++;
			info->exec_model = (SpvExecutionModel)args[0];
			info->entry_id = args[1];
		} else if (op == SpvOpExecutionMode && word_count >= 2 &&
			args[1] == SpvExecutionModeLocalSize && word_count >= 5) {
			info->size_x = args[2];
			info->size_y = args[3];
			info->size_z = args[4];
		}

		p += word_count;
	}

	if (spvm_wg_has_recursion(prog)) {
		snprintf(error, error_size, "workgroup executor does not support recursive function calls");
		return 0;
	}

	if (info->entry_count != 1) {
		snprintf(error, error_size, "workgroup executor requires exactly one entry point (found %u)",
			info->entry_count);
		return 0;
	}
	if (info->exec_model != SpvExecutionModelGLCompute) {
		snprintf(error, error_size, "workgroup executor only supports GLCompute entry points");
		return 0;
	}
	if (info->size_x == 0 || info->size_y == 0 || info->size_z == 0) {
		snprintf(error, error_size, "workgroup executor requires a non-zero numeric LocalSize");
		return 0;
	}
	return 1;
}

static void spvm_wg_set_builtin(spvm_state_t state, SpvBuiltIn builtin, const spvm_word* values, spvm_word count)
{
	spvm_word member_count = 0;
	spvm_member_t members = spvm_state_get_builtin(state, builtin, &member_count);
	if (members == NULL)
		return;

	for (spvm_word i = 0; i < member_count && i < count; i++)
		members[i].value.s = values[i];
}

spvm_workgroup_t spvm_workgroup_create(spvm_program_t prog,
	spvm_word group_x, spvm_word group_y, spvm_word group_z)
{
	if (prog == NULL)
		return NULL;

	char error[512];
	spvm_wg_module_info info;
	if (!spvm_wg_scan_supported(prog, &info, error, sizeof(error)))
		return NULL;

	spvm_workgroup_t wg = (spvm_workgroup_t)calloc(1, sizeof(struct spvm_workgroup));
	wg->program = prog;
	wg->entry_id = info.entry_id;
	wg->size_x = info.size_x;
	wg->size_y = info.size_y;
	wg->size_z = info.size_z;
	wg->group_x = group_x;
	wg->group_y = group_y;
	wg->group_z = group_z;
	wg->count = wg->size_x * wg->size_y * wg->size_z;
	wg->status = SPVM_WG_STATUS_RUNNING;

	wg->invocations = (spvm_wg_invocation*)calloc(wg->count, sizeof(spvm_wg_invocation));

	/* every invocation gets its own state: Private/Function/Interface/call stack */
	for (spvm_word i = 0; i < wg->count; i++) {
		spvm_state_t state = spvm_state_create(prog);
		if (state == NULL || state->derivative_group_x != NULL) {
			if (state) spvm_state_delete(state);
			for (spvm_word j = 0; j < i; j++)
				spvm_state_delete(wg->invocations[j].state);
			free(wg->invocations);
			free(wg);
			return NULL;
		}
		state->user_data = wg;
		state->control_barrier = spvm_wg_control_barrier;
		wg->invocations[i].state = state;
	}

	/*
	 * Share Workgroup storage. All states allocated their own member tree in
	 * the setup pass; release the duplicates and point them at invocation 0's
	 * tree. Access chains and helper pointer parameters are derived from the
	 * variable root at runtime, so every nested alias stays shared as well.
	 */
	spvm_state_t root = wg->invocations[0].state;
	for (size_t id = 0; id < prog->bound; id++) {
		spvm_result_t root_res = &root->results[id];
		if (root_res->type != spvm_result_type_variable ||
			root_res->storage_class != SpvStorageClassWorkgroup ||
			root_res->owner != NULL)
			continue;

		for (spvm_word i = 1; i < wg->count; i++) {
			spvm_result_t peer = &wg->invocations[i].state->results[id];
			if (peer->members != NULL && peer->member_count > 0)
				spvm_member_free(peer->members, peer->member_count);
			peer->members = root_res->members;
			peer->member_count = root_res->member_count;
		}
	}

	/* per-invocation builtins, x-fastest invocation order */
	for (spvm_word z = 0; z < wg->size_z; z++) {
		for (spvm_word y = 0; y < wg->size_y; y++) {
			for (spvm_word x = 0; x < wg->size_x; x++) {
				spvm_word index = (z * wg->size_y + y) * wg->size_x + x;
				spvm_state_t state = wg->invocations[index].state;

				spvm_word local_id[3] = { x, y, z };
				spvm_word local_index[1] = { index };
				spvm_word group_id[3] = { group_x, group_y, group_z };
				spvm_word global_id[3] = {
					group_x * wg->size_x + x,
					group_y * wg->size_y + y,
					group_z * wg->size_z + z
				};

				spvm_wg_set_builtin(state, SpvBuiltInLocalInvocationId, local_id, 3);
				spvm_wg_set_builtin(state, SpvBuiltInLocalInvocationIndex, local_index, 1);
				spvm_wg_set_builtin(state, SpvBuiltInWorkgroupId, group_id, 3);
				spvm_wg_set_builtin(state, SpvBuiltInGlobalInvocationId, global_id, 3);

				spvm_state_prepare(state, wg->entry_id);
			}
		}
	}

	return wg;
}

spvm_word spvm_workgroup_invocation_count(spvm_workgroup_t wg)
{
	return wg ? wg->count : 0;
}

spvm_state_t spvm_workgroup_get_state(spvm_workgroup_t wg, spvm_word invocation)
{
	if (wg == NULL || invocation >= wg->count)
		return NULL;
	return wg->invocations[invocation].state;
}

spvm_wg_status spvm_workgroup_step(spvm_workgroup_t wg, spvm_word invocation)
{
	if (wg == NULL || invocation >= wg->count)
		return SPVM_WG_STATUS_ERROR_INVALID_ARGUMENT;

	spvm_wg_invocation* inv = &wg->invocations[invocation];

	if (inv->errored)
		return wg->status;
	if (inv->finished)
		return SPVM_WG_STATUS_FINISHED;
	if (inv->waiting)
		return SPVM_WG_STATUS_WAITING; /* no instruction is executed */

	spvm_state_t state = inv->state;
	spvm_word instruction = (spvm_word)(state->code_current - wg->program->code);
	spvm_state_step_opcode(state);

	if (inv->errored)
		return wg->status;

	if (inv->waiting) {
		/* a peer that already finished can never reach this round */
		spvm_word finished = 0;
		for (spvm_word i = 0; i < wg->count; i++)
			if (wg->invocations[i].finished)
				finished++;

		if (finished > 0) {
			inv->errored = 1;
			wg->status = SPVM_WG_STATUS_ERROR_PEER_FINISHED;
			snprintf(wg->error, sizeof(wg->error),
				"invocation %u reached a barrier at instruction %u in round %u "
				"but %u invocation(s) already returned",
				invocation, instruction, wg->round, finished);
			return wg->status;
		}

		spvm_wg_capture_site(wg, inv, instruction);

		if (!wg->have_site) {
			wg->have_site = 1;
			wg->round_site = inv->site;
		} else if (!spvm_wg_sites_equal(&wg->round_site, &inv->site)) {
			inv->errored = 1;
			wg->status = SPVM_WG_STATUS_ERROR_BARRIER_MISMATCH;
			snprintf(wg->error, sizeof(wg->error),
				"invocation %u reached barrier at instruction %u (call depth %u) in round %u "
				"but the round started at instruction %u (call depth %u)",
				invocation, inv->site.instruction, inv->site.frame_count, wg->round,
				wg->round_site.instruction, wg->round_site.frame_count);
			return wg->status;
		}

		wg->arrived++;
		if (wg->arrived == wg->count) {
			/* last arrival releases the whole round */
			wg->round++;
			wg->arrived = 0;
			wg->have_site = 0;
			for (spvm_word i = 0; i < wg->count; i++)
				wg->invocations[i].waiting = 0;
		}

		return SPVM_WG_STATUS_WAITING;
	}

	if (state->code_current == NULL) {
		inv->finished = 1;
		if (wg->arrived > 0) {
			inv->errored = 1;
			wg->status = SPVM_WG_STATUS_ERROR_PEER_FINISHED;
			snprintf(wg->error, sizeof(wg->error),
				"invocation %u returned in round %u while %u invocation(s) were still "
				"waiting at the barrier at instruction %u",
				invocation, wg->round, wg->arrived, wg->round_site.instruction);
			return wg->status;
		}
		return SPVM_WG_STATUS_FINISHED;
	}

	return SPVM_WG_STATUS_RUNNING;
}

spvm_wg_status spvm_workgroup_run(spvm_workgroup_t wg)
{
	if (wg == NULL)
		return SPVM_WG_STATUS_ERROR_INVALID_ARGUMENT;

	spvm_byte all_finished = 0;
	while (!all_finished) {
		all_finished = 1;
		for (spvm_word i = 0; i < wg->count; i++) {
			spvm_wg_invocation* inv = &wg->invocations[i];
			if (inv->finished || inv->errored)
				continue;

			all_finished = 0;
			spvm_wg_status status = spvm_workgroup_step(wg, i);
			if (status == SPVM_WG_STATUS_ERROR_BARRIER_SCOPE ||
				status == SPVM_WG_STATUS_ERROR_BARRIER_MISMATCH ||
				status == SPVM_WG_STATUS_ERROR_PEER_FINISHED ||
				status == SPVM_WG_STATUS_ERROR_UNSUPPORTED)
				return status;
		}
	}

	return SPVM_WG_STATUS_FINISHED;
}

spvm_word spvm_workgroup_current_round(spvm_workgroup_t wg)
{
	return wg ? wg->round : 0;
}

const char* spvm_workgroup_error(spvm_workgroup_t wg)
{
	if (wg == NULL || wg->status == SPVM_WG_STATUS_RUNNING)
		return NULL;
	return wg->error;
}

void spvm_workgroup_delete(spvm_workgroup_t wg)
{
	if (wg == NULL)
		return;

	/*
	 * Detach the shared Workgroup member trees from every non-owning state so
	 * spvm_state_delete() frees each tree exactly once (through invocation 0).
	 * Access-chain/parameter aliases are not owned by their results and are
	 * never freed by spvm_result_delete().
	 */
	spvm_state_t root = wg->invocations[0].state;
	for (size_t id = 0; id < wg->program->bound; id++) {
		spvm_result_t root_res = &root->results[id];
		if (root_res->type != spvm_result_type_variable ||
			root_res->storage_class != SpvStorageClassWorkgroup ||
			root_res->owner != NULL)
			continue;

		for (spvm_word i = 1; i < wg->count; i++) {
			spvm_result_t peer = &wg->invocations[i].state->results[id];
			peer->members = NULL;
			peer->member_count = 0;
		}
	}

	for (spvm_word i = 0; i < wg->count; i++) {
		spvm_state_t state = wg->invocations[i].state;
		state->control_barrier = NULL;
		state->user_data = NULL;
		spvm_state_delete(state);
	}

	free(wg->invocations);
	free(wg);
}
