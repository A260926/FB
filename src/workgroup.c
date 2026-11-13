#include <spvm/workgroup.h>
#include <spvm/spirv.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPVM_WORKGROUP_MAX_INVOCATIONS (1u << 16)

typedef enum spvm_invocation_status {
	spvm_invocation_runnable,
	spvm_invocation_waiting,
	spvm_invocation_finished
} spvm_invocation_status;

typedef struct spvm_workgroup_invocation {
	spvm_state_t state;
	spvm_invocation_status status;

	/* Number of barriers this invocation has executed so far, i.e. the
	 * dynamic round of the next barrier it will reach. */
	spvm_word barrier_round;

	/* Rendezvous key captured when the invocation started waiting. */
	spvm_word wait_round;
	spvm_word wait_instruction;
	spvm_word wait_depth;
	spvm_word* wait_stack;
	spvm_word wait_stack_capacity;
} spvm_workgroup_invocation;

struct spvm_workgroup {
	spvm_program_t program;

	unsigned invocation_count;
	spvm_workgroup_invocation* invocations;

	spvm_word local_size[3];
	spvm_word workgroup_id[3];
	spvm_word entry_function;

	/* Workgroup storage class memory, allocated once for the whole group. */
	unsigned shared_count;
	spvm_word* shared_ids;
	spvm_member_t* shared_members;
	spvm_word* shared_member_counts;

	spvm_byte failed;
	spvm_workgroup_error last_error;
};

/* ------------------------------------------------------------------ */
/* diagnostics                                                         */
/* ------------------------------------------------------------------ */

static void wg_set_error(spvm_workgroup_t wg, spvm_workgroup_error* out,
	spvm_workgroup_error_code code, spvm_word invocation,
	spvm_word instruction, spvm_word round, spvm_word barrier,
	const char* format, ...)
{
	spvm_workgroup_error err;
	memset(&err, 0, sizeof(err));
	err.code = code;
	err.invocation = invocation;
	err.instruction = instruction;
	err.round = round;
	err.barrier = barrier;

	va_list args;
	va_start(args, format);
	vsnprintf(err.message, sizeof(err.message), format, args);
	va_end(args);

	if (wg) {
		wg->last_error = err;
		wg->failed = 1;
	}
	if (out)
		*out = err;
}

/* ------------------------------------------------------------------ */
/* program inspection                                                  */
/* ------------------------------------------------------------------ */

typedef struct wg_code_iterator {
	spvm_source code;
	size_t remaining;
} wg_code_iterator;

static int wg_code_next(wg_code_iterator* it, SpvOp* opcode,
	spvm_word* word_count, spvm_source* start)
{
	if (it->remaining < 1)
		return 0;

	spvm_word header = *it->code;
	spvm_word count = (header >> SpvWordCountShift) & 0xffff;
	if (count < 1 || (size_t)count > it->remaining)
		return 0;

	*opcode = (SpvOp)(header & SpvOpCodeMask);
	*word_count = count;
	*start = it->code;

	it->code += count;
	it->remaining -= count;
	return 1;
}

static int wg_is_atomic_opcode(SpvOp opcode)
{
	return (opcode >= SpvOpAtomicLoad && opcode <= SpvOpAtomicXor) ||
		opcode == SpvOpAtomicFlagTestAndSet ||
		opcode == SpvOpAtomicFlagClear;
}

static int wg_is_derivative_opcode(SpvOp opcode)
{
	return opcode == SpvOpDPdx || opcode == SpvOpDPdy ||
		opcode == SpvOpDPdxFine || opcode == SpvOpDPdyFine ||
		opcode == SpvOpDPdxCoarse || opcode == SpvOpDPdyCoarse ||
		opcode == SpvOpFwidth || opcode == SpvOpFwidthFine ||
		opcode == SpvOpFwidthCoarse;
}

typedef struct wg_call_edge {
	spvm_word caller;
	spvm_word callee;
} wg_call_edge;

static int wg_call_graph_has_cycle(spvm_word* functions, unsigned function_count,
	const wg_call_edge* edges, unsigned edge_count, unsigned bound, spvm_workgroup_error* error)
{
	if (function_count == 0)
		return 0;

	unsigned char* color = (unsigned char*)calloc(bound ? bound : 1, 1);
	if (!color) {
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_OUT_OF_MEMORY,
			-1, -1, -1, -1, "out of memory while validating the call graph");
		return 1;
	}

	/* Iterative DFS: each frame stores the function and the edge cursor. */
	typedef struct { spvm_word function; unsigned edge; } frame_t;
	frame_t* stack = (frame_t*)malloc((function_count + 1) * sizeof(frame_t));
	if (!stack) {
		free(color);
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_OUT_OF_MEMORY,
			-1, -1, -1, -1, "out of memory while validating the call graph");
		return 1;
	}

	int has_cycle = 0;
	for (unsigned f = 0; f < function_count && !has_cycle; f++) {
		spvm_word root = functions[f];
		if (root < 0 || (unsigned)root >= bound || color[root] != 0)
			continue;

		unsigned depth = 0;
		stack[depth].function = root;
		stack[depth].edge = 0;
		color[root] = 1;
		depth++;

		while (depth > 0) {
			frame_t* frame = &stack[depth - 1];
			int descended = 0;

			while (frame->edge < edge_count) {
				const wg_call_edge* edge = &edges[frame->edge++];
				if (edge->caller != frame->function)
					continue;

				spvm_word child = edge->callee;
				if (child < 0 || (unsigned)child >= bound)
					continue;

				if (color[child] == 1) {
					has_cycle = 1;
					break;
				}
				if (color[child] == 0) {
					color[child] = 1;
					stack[depth].function = child;
					stack[depth].edge = 0;
					depth++;
					descended = 1;
					break;
				}
			}

			if (has_cycle)
				break;
			if (!descended) {
				color[frame->function] = 2;
				depth--;
			}
		}
	}

	free(stack);
	free(color);

	if (has_cycle)
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_UNSUPPORTED_FEATURE,
			-1, -1, -1, -1, "recursive function calls are not supported");
	return has_cycle;
}

/*
 * Check the subset of SPIR-V this executor supports.  Returns 0 on success
 * and sets *error on failure.
 *
 * Note: spvm_state_create() appends an entry point to the program every
 * time a state is created, so program->entry_points cannot be used to count
 * the module's entry points.  Everything below is scanned from the code,
 * which is immutable.
 */
static int wg_validate_program(spvm_program_t program, spvm_word local_size[3],
	spvm_word* entry_function, spvm_workgroup_error* error)
{
	if (!program) {
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_INVALID_ARGUMENT,
			-1, -1, -1, -1, "program is NULL");
		return 0;
	}

	wg_code_iterator scan = { program->code, (size_t)program->code_length };
	SpvOp scan_op;
	spvm_word scan_wc;
	spvm_source scan_start;
	unsigned entry_count = 0;
	spvm_word entry_id = 0;
	SpvExecutionModel entry_model = SpvExecutionModelMax;

	while (wg_code_next(&scan, &scan_op, &scan_wc, &scan_start)) {
		if (scan_op != SpvOpEntryPoint || scan_wc < 3)
			continue;
		entry_count++;
		entry_id = scan_start[2];
		entry_model = (SpvExecutionModel)scan_start[1];
	}

	if (entry_count == 0) {
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_INVALID_PROGRAM,
			-1, -1, -1, -1, "the module has no entry point");
		return 0;
	}
	if (entry_count != 1) {
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_INVALID_PROGRAM,
			-1, -1, -1, -1, "expected exactly one entry point, found %u",
			entry_count);
		return 0;
	}
	if (entry_model != SpvExecutionModelGLCompute) {
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_INVALID_PROGRAM,
			-1, -1, -1, -1, "the entry point is not a GLCompute entry point");
		return 0;
	}

	spvm_word found_local_size = 0;
	spvm_word size[3] = { 1, 1, 1 };
	int local_size_id_mode = 0;

	spvm_word* functions = (spvm_word*)calloc(program->bound ? program->bound : 1, sizeof(spvm_word));
	unsigned char* is_function = (unsigned char*)calloc(program->bound ? program->bound : 1, 1);
	unsigned function_count = 0;
	wg_call_edge* edges = NULL;
	unsigned edge_count = 0;
	unsigned edge_capacity = 0;

	if (!functions || !is_function) {
		free(functions);
		free(is_function);
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_OUT_OF_MEMORY,
			-1, -1, -1, -1, "out of memory while validating the program");
		return 0;
	}

	wg_code_iterator it = { program->code, (size_t)program->code_length };
	SpvOp opcode;
	spvm_word word_count;
	spvm_source start;
	spvm_word current_function = -1;

	while (wg_code_next(&it, &opcode, &word_count, &start)) {
		spvm_word instruction = (spvm_word)(start - program->code);

		switch (opcode) {
		case SpvOpTypeRuntimeArray:
		case SpvOpTypeImage:
		case SpvOpTypeSampler:
		case SpvOpTypeSampledImage:
			wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_UNSUPPORTED_FEATURE,
				-1, instruction, -1, -1,
				"opcode %d (runtime arrays, images, samplers) is not supported",
				(int)opcode);
			goto invalid;
		case SpvOpExecutionMode:
			if (word_count >= 3) {
				spvm_word target = start[1];
				SpvExecutionMode mode = (SpvExecutionMode)start[2];
				if (target == entry_id && mode == SpvExecutionModeLocalSize) {
					if (word_count < 6) {
						wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_INVALID_PROGRAM,
							-1, instruction, -1, -1, "malformed LocalSize execution mode");
						goto invalid;
					}
					size[0] = start[3];
					size[1] = start[4];
					size[2] = start[5];
					found_local_size = 1;
				} else if (target == entry_id && mode == SpvExecutionModeLocalSizeId) {
					local_size_id_mode = 1;
				}
			}
			break;
		case SpvOpFunction:
			if (word_count >= 3) {
				spvm_word id = start[2];
				current_function = id;
				if (id < 0 || (unsigned)id >= program->bound) {
					wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_INVALID_PROGRAM,
						-1, instruction, -1, -1,
						"OpFunction id %d is out of bounds", id);
					goto invalid;
				}
				if (!is_function[id]) {
					is_function[id] = 1;
					functions[function_count++] = id;
				}
			}
			break;
		case SpvOpFunctionEnd:
			current_function = -1;
			break;
		case SpvOpFunctionCall:
			if (word_count >= 4 && current_function >= 0) {
				if (edge_count == edge_capacity) {
					unsigned new_capacity = edge_capacity ? edge_capacity * 2 : 16;
					wg_call_edge* grown = (wg_call_edge*)realloc(edges, new_capacity * sizeof(wg_call_edge));
					if (!grown) {
						wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_OUT_OF_MEMORY,
							-1, -1, -1, -1, "out of memory while validating the call graph");
						goto invalid;
					}
					edges = grown;
					edge_capacity = new_capacity;
				}
				edges[edge_count].caller = current_function;
				edges[edge_count].callee = start[3];
				edge_count++;
			}
			break;
		default:
			if (wg_is_atomic_opcode(opcode)) {
				wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_UNSUPPORTED_FEATURE,
					-1, instruction, -1, -1, "atomic instructions are not supported");
				goto invalid;
			}
			if (wg_is_derivative_opcode(opcode)) {
				wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_UNSUPPORTED_FEATURE,
					-1, instruction, -1, -1,
					"derivative instructions are not supported in a compute workgroup");
				goto invalid;
			}
			break;
		}
	}

	if (local_size_id_mode && !found_local_size) {
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_UNSUPPORTED_FEATURE,
			-1, -1, -1, -1, "LocalSizeId is not supported, use a numeric LocalSize");
		goto invalid;
	}
	if (!found_local_size) {
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_INVALID_PROGRAM,
			-1, -1, -1, -1, "the compute entry point has no numeric LocalSize");
		goto invalid;
	}
	if (size[0] <= 0 || size[1] <= 0 || size[2] <= 0) {
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_INVALID_PROGRAM,
			-1, -1, -1, -1, "LocalSize dimensions must be positive");
		goto invalid;
	}
	if ((unsigned long long)size[0] * (unsigned long long)size[1] *
		(unsigned long long)size[2] > SPVM_WORKGROUP_MAX_INVOCATIONS) {
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_UNSUPPORTED_FEATURE,
			-1, -1, -1, -1, "LocalSize %dx%dx%d exceeds the invocation limit",
			size[0], size[1], size[2]);
		goto invalid;
	}
	if (wg_call_graph_has_cycle(functions, function_count, edges, edge_count,
		program->bound, error))
		goto invalid;

	/* The entry point must name a function defined in the module. */
	{
		int entry_is_function = 0;
		for (unsigned i = 0; i < function_count; i++)
			if (functions[i] == entry_id)
				entry_is_function = 1;
		if (!entry_is_function) {
			wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_INVALID_PROGRAM,
				-1, -1, -1, -1,
				"the entry point function is not defined in the module");
			goto invalid;
		}
	}

	local_size[0] = size[0];
	local_size[1] = size[1];
	local_size[2] = size[2];
	*entry_function = entry_id;

	free(edges);
	free(functions);
	free(is_function);
	return 1;

invalid:
	free(edges);
	free(functions);
	free(is_function);
	return 0;
}

/* ------------------------------------------------------------------ */
/* workgroup storage                                                   */
/* ------------------------------------------------------------------ */

static unsigned wg_count_workgroup_variables(spvm_program_t program)
{
	unsigned count = 0;
	wg_code_iterator it = { program->code, (size_t)program->code_length };
	SpvOp opcode;
	spvm_word word_count;
	spvm_source start;

	while (wg_code_next(&it, &opcode, &word_count, &start)) {
		if (opcode == SpvOpVariable && word_count >= 4 &&
			start[3] == SpvStorageClassWorkgroup)
			count++;
	}
	return count;
}
/*
 * Replace every state's private copy of a Workgroup variable with one
 * allocation owned by the workgroup.  Sharing the actual spvm_member array
 * (rather than copying values on OpStore) means AccessChain and pointer
 * aliases into nested members all observe the same storage.
 */
static int wg_setup_shared_memory(spvm_workgroup_t wg, spvm_workgroup_error* error)
{
	unsigned count = wg_count_workgroup_variables(wg->program);
	wg->shared_count = count;
	if (count == 0)
		return 1;

	wg->shared_ids = (spvm_word*)calloc(count, sizeof(spvm_word));
	wg->shared_members = (spvm_member_t*)calloc(count, sizeof(spvm_member_t));
	wg->shared_member_counts = (spvm_word*)calloc(count, sizeof(spvm_word));
	if (!wg->shared_ids || !wg->shared_members || !wg->shared_member_counts) {
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_OUT_OF_MEMORY,
			-1, -1, -1, -1, "out of memory while allocating workgroup memory");
		return 0;
	}

	spvm_state_t primary = wg->invocations[0].state;
	unsigned index = 0;

	wg_code_iterator it = { wg->program->code, (size_t)wg->program->code_length };
	SpvOp opcode;
	spvm_word word_count;
	spvm_source start;

	while (wg_code_next(&it, &opcode, &word_count, &start)) {
		if (opcode != SpvOpVariable || word_count < 4 ||
			start[3] != SpvStorageClassWorkgroup)
			continue;

		spvm_word type_id = start[1];
		spvm_word id = start[2];

		spvm_result shared;
		memset(&shared, 0, sizeof(shared));
		spvm_result_allocate_typed_value(&shared, primary->results, type_id);

		wg->shared_ids[index] = id;
		wg->shared_members[index] = shared.members;
		wg->shared_member_counts[index] = shared.member_count;
		index++;

		for (unsigned i = 0; i < wg->invocation_count; i++) {
			spvm_state_t state = wg->invocations[i].state;
			if (id < 0 || (unsigned)id >= (unsigned)state->owner->bound)
				continue;

			spvm_result_t result = &state->results[id];
			if (result->members)
				spvm_member_free(result->members, result->member_count);

			result->members = shared.members;
			result->member_count = shared.member_count;
		}
	}

	return 1;
}

/* ------------------------------------------------------------------ */
/* builtins                                                            */
/* ------------------------------------------------------------------ */

static void wg_set_builtin(spvm_state_t state, SpvBuiltIn builtin,
	const spvm_word values[3], spvm_word value_count)
{
	spvm_word member_count = 0;
	spvm_member_t members = spvm_state_get_builtin(state, builtin, &member_count);
	if (!members)
		return;

	spvm_word count = member_count < value_count ? member_count : value_count;
	for (spvm_word i = 0; i < count; i++)
		members[i].value.u = (unsigned)values[i];
}

static void wg_set_builtins(spvm_workgroup_t wg)
{
	for (unsigned i = 0; i < wg->invocation_count; i++) {
		spvm_state_t state = wg->invocations[i].state;

		spvm_word x = (spvm_word)(i % (unsigned)wg->local_size[0]);
		spvm_word y = (spvm_word)((i / (unsigned)wg->local_size[0]) %
			(unsigned)wg->local_size[1]);
		spvm_word z = (spvm_word)(i / ((unsigned)wg->local_size[0] *
			(unsigned)wg->local_size[1]));

		spvm_word local_id[3] = { x, y, z };
		spvm_word local_index[1] = { (spvm_word)i };
		spvm_word group_id[3] = { wg->workgroup_id[0], wg->workgroup_id[1], wg->workgroup_id[2] };
		spvm_word global_id[3] = {
			wg->workgroup_id[0] * wg->local_size[0] + x,
			wg->workgroup_id[1] * wg->local_size[1] + y,
			wg->workgroup_id[2] * wg->local_size[2] + z
		};

		wg_set_builtin(state, SpvBuiltInLocalInvocationId, local_id, 3);
		wg_set_builtin(state, SpvBuiltInLocalInvocationIndex, local_index, 1);
		wg_set_builtin(state, SpvBuiltInWorkgroupId, group_id, 3);
		wg_set_builtin(state, SpvBuiltInGlobalInvocationId, global_id, 3);
	}
}

/* ------------------------------------------------------------------ */
/* creation / destruction                                              */
/* ------------------------------------------------------------------ */

static void wg_free_contents(spvm_workgroup_t wg)
{
	if (wg->invocations) {
		for (unsigned i = 0; i < wg->invocation_count; i++) {
			spvm_workgroup_invocation* inv = &wg->invocations[i];
			if (inv->state) {
				/* Workgroup variables alias the shared allocation which
				 * this workgroup frees once below. */
				if (wg->shared_ids) {
					for (unsigned j = 0; j < wg->shared_count; j++) {
						spvm_word id = wg->shared_ids[j];
						if (id >= 0 && (unsigned)id < (unsigned)inv->state->owner->bound) {
							inv->state->results[id].members = NULL;
							inv->state->results[id].member_count = 0;
						}
					}
				}
				spvm_state_delete(inv->state);
			}
			free(inv->wait_stack);
		}
		free(wg->invocations);
	}

	for (unsigned i = 0; i < wg->shared_count; i++)
		if (wg->shared_members && wg->shared_members[i])
			spvm_member_free(wg->shared_members[i], wg->shared_member_counts[i]);

	free(wg->shared_ids);
	free(wg->shared_members);
	free(wg->shared_member_counts);
}

spvm_workgroup_t spvm_workgroup_create(spvm_program_t program,
	const spvm_word workgroup_id[3], spvm_workgroup_error* error)
{
	if (!workgroup_id) {
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_INVALID_ARGUMENT,
			-1, -1, -1, -1, "workgroup_id is NULL");
		return NULL;
	}
	if (!program) {
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_INVALID_ARGUMENT,
			-1, -1, -1, -1, "program is NULL");
		return NULL;
	}

	/*
	 * Validate before creating any state: the first state walks the module
	 * into its result slots, so unsupported modules are rejected before that
	 * work happens.  Everything the validator needs (code, bound, entry
	 * points, execution modes) is available straight from
	 * spvm_program_create().
	 */
	spvm_word local_size[3];
	spvm_word entry_function = 0;
	if (!wg_validate_program(program, local_size, &entry_function, error))
		return NULL;

	spvm_state_t primary = spvm_state_create(program);
	if (!primary) {
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_OUT_OF_MEMORY,
			-1, -1, -1, -1, "out of memory while parsing the program");
		return NULL;
	}

	spvm_workgroup_t wg = (spvm_workgroup_t)calloc(1, sizeof(spvm_workgroup));
	if (!wg) {
		spvm_state_delete(primary);
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_OUT_OF_MEMORY,
			-1, -1, -1, -1, "out of memory while creating the workgroup");
		return NULL;
	}

	wg->program = program;
	wg->local_size[0] = local_size[0];
	wg->local_size[1] = local_size[1];
	wg->local_size[2] = local_size[2];
	wg->workgroup_id[0] = workgroup_id[0];
	wg->workgroup_id[1] = workgroup_id[1];
	wg->workgroup_id[2] = workgroup_id[2];
	wg->entry_function = entry_function;
	wg->invocation_count = (unsigned)local_size[0] * (unsigned)local_size[1] *
		(unsigned)local_size[2];

	wg->invocations = (spvm_workgroup_invocation*)calloc(wg->invocation_count,
		sizeof(spvm_workgroup_invocation));
	if (!wg->invocations) {
		spvm_state_delete(primary);
		wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_OUT_OF_MEMORY,
			-1, -1, -1, -1, "out of memory while creating invocations");
		free(wg);
		return NULL;
	}

	wg->invocations[0].state = primary;
	wg->invocations[0].status = spvm_invocation_runnable;

	for (unsigned i = 1; i < wg->invocation_count; i++) {
		spvm_state_t state = spvm_state_create(program);
		if (!state) {
			wg_set_error(NULL, error, SPVM_WORKGROUP_ERROR_OUT_OF_MEMORY,
				(spvm_word)i, -1, -1, -1, "failed to create invocation state");
			wg_free_contents(wg);
			free(wg);
			return NULL;
		}
		wg->invocations[i].state = state;
		wg->invocations[i].status = spvm_invocation_runnable;
	}

	if (!wg_setup_shared_memory(wg, error)) {
		wg_free_contents(wg);
		free(wg);
		return NULL;
	}

	wg_set_builtins(wg);

	for (unsigned i = 0; i < wg->invocation_count; i++)
		spvm_state_prepare(wg->invocations[i].state, entry_function);

	wg->last_error.code = SPVM_WORKGROUP_ERROR_NONE;
	if (error)
		memset(error, 0, sizeof(*error));

	return wg;
}

void spvm_workgroup_delete(spvm_workgroup_t workgroup)
{
	if (!workgroup)
		return;

	wg_free_contents(workgroup);
	free(workgroup);
}

unsigned spvm_workgroup_get_invocation_count(spvm_workgroup_t workgroup)
{
	return workgroup ? workgroup->invocation_count : 0;
}

spvm_state_t spvm_workgroup_get_state(spvm_workgroup_t workgroup,
	unsigned invocation)
{
	if (!workgroup || invocation >= workgroup->invocation_count)
		return NULL;
	return workgroup->invocations[invocation].state;
}

const spvm_workgroup_error* spvm_workgroup_get_error(spvm_workgroup_t workgroup)
{
	if (!workgroup || workgroup->last_error.code == SPVM_WORKGROUP_ERROR_NONE)
		return NULL;
	return &workgroup->last_error;
}

/* ------------------------------------------------------------------ */
/* synchronization                                                     */
/* ------------------------------------------------------------------ */

static int wg_capture_call_stack(spvm_workgroup_invocation* inv,
	spvm_state_t state, spvm_source code)
{
	spvm_word depth = state->function_stack_current;
	if (depth < 0)
		depth = 0;

	if (depth > inv->wait_stack_capacity) {
		spvm_word capacity = depth + 8;
		spvm_word* grown = (spvm_word*)realloc(inv->wait_stack,
			capacity * sizeof(spvm_word));
		if (!grown)
			return 0;
		inv->wait_stack = grown;
		inv->wait_stack_capacity = capacity;
	}

	inv->wait_depth = depth;
	for (spvm_word i = 0; i < depth; i++)
		inv->wait_stack[i] = (spvm_word)(state->function_stack[i] - code);
	return 1;
}

static spvm_workgroup_step_result wg_handle_barrier(spvm_workgroup_t wg,
	unsigned index, spvm_workgroup_error* error)
{
	spvm_workgroup_invocation* inv = &wg->invocations[index];
	spvm_state_t state = inv->state;
	spvm_source start = state->code_current;
	spvm_word total = (*start >> SpvWordCountShift) & 0xffff;
	spvm_word instruction = (spvm_word)(start - wg->program->code);

	if (total < 4) {
		wg_set_error(wg, error, SPVM_WORKGROUP_ERROR_INVALID_PROGRAM,
			(spvm_word)index, instruction, inv->barrier_round, instruction,
			"invocation %u: malformed OpControlBarrier", index);
		return SPVM_WORKGROUP_STEP_ERROR;
	}

	spvm_word execution_id = start[1];
	spvm_word memory_id = start[2];
	spvm_word semantics_id = start[3];

	/* The operands are constant ids; fetch their values. */
	if (execution_id < 0 || memory_id < 0 || semantics_id < 0 ||
		(unsigned)execution_id >= (unsigned)state->owner->bound ||
		(unsigned)memory_id >= (unsigned)state->owner->bound ||
		(unsigned)semantics_id >= (unsigned)state->owner->bound) {
		wg_set_error(wg, error, SPVM_WORKGROUP_ERROR_UNSUPPORTED_BARRIER,
			(spvm_word)index, instruction, inv->barrier_round, instruction,
			"invocation %u: barrier at word %d round %d has out of range operands",
			index, instruction, inv->barrier_round);
		return SPVM_WORKGROUP_STEP_ERROR;
	}

	if (state->results[execution_id].type != spvm_result_type_constant ||
		state->results[memory_id].type != spvm_result_type_constant ||
		state->results[semantics_id].type != spvm_result_type_constant ||
		!state->results[execution_id].members ||
		!state->results[memory_id].members ||
		!state->results[semantics_id].members) {
		wg_set_error(wg, error, SPVM_WORKGROUP_ERROR_UNSUPPORTED_BARRIER,
			(spvm_word)index, instruction, inv->barrier_round, instruction,
			"invocation %u: barrier at word %d round %d does not use constant "
			"execution/memory/semantics ids", index, instruction, inv->barrier_round);
		return SPVM_WORKGROUP_STEP_ERROR;
	}

	spvm_word execution = state->results[execution_id].members[0].value.s;
	spvm_word memory = state->results[memory_id].members[0].value.s;
	spvm_word semantics = (spvm_word)state->results[semantics_id].members[0].value.u64;

	const spvm_word required_semantics =
		SpvMemorySemanticsAcquireReleaseMask | SpvMemorySemanticsWorkgroupMemoryMask;

	if (execution != SpvScopeWorkgroup || memory != SpvScopeWorkgroup ||
		semantics != required_semantics) {
		wg_set_error(wg, error, SPVM_WORKGROUP_ERROR_UNSUPPORTED_BARRIER,
			(spvm_word)index, instruction, inv->barrier_round, instruction,
			"invocation %u: unsupported barrier at word %d round %d "
			"(execution=%d memory=%d semantics=0x%x)",
			index, instruction, inv->barrier_round, execution, memory,
			(unsigned)semantics);
		return SPVM_WORKGROUP_STEP_ERROR;
	}

	/* The barrier itself has executed; stop before the next instruction. */
	state->code_current = start + total;
	state->instruction_count++;
	state->did_jump = 0;

	spvm_word round = inv->barrier_round;
	inv->barrier_round = round + 1;
	inv->status = spvm_invocation_waiting;
	inv->wait_round = round;
	inv->wait_instruction = instruction;
	if (!wg_capture_call_stack(inv, state, wg->program->code)) {
		wg_set_error(wg, error, SPVM_WORKGROUP_ERROR_OUT_OF_MEMORY,
			(spvm_word)index, instruction, round, instruction,
			"invocation %u: out of memory capturing the call stack at "
			"barrier word %d", index, instruction);
		return SPVM_WORKGROUP_STEP_ERROR;
	}

	for (unsigned i = 0; i < wg->invocation_count; i++) {
		if (i == index)
			continue;
		if (wg->invocations[i].status == spvm_invocation_finished) {
			wg_set_error(wg, error, SPVM_WORKGROUP_ERROR_FINISHED_WHILE_WAITING,
				(spvm_word)i, instruction, round, instruction,
				"invocation %u: invocation %u finished but the group is "
				"waiting at barrier word %d round %d",
				index, i, instruction, round);
			return SPVM_WORKGROUP_STEP_ERROR;
		}
	}

	unsigned waiting = 0;
	for (unsigned i = 0; i < wg->invocation_count; i++)
		if (wg->invocations[i].status == spvm_invocation_waiting)
			waiting++;

	if (waiting < wg->invocation_count)
		return SPVM_WORKGROUP_STEP_WAITING;

	/* Everybody is here.  Match dynamic round, static instruction and the
	 * sequence of call stack return addresses. */
	spvm_workgroup_invocation* reference = NULL;
	for (unsigned i = 0; i < wg->invocation_count; i++) {
		if (wg->invocations[i].status == spvm_invocation_waiting) {
			reference = &wg->invocations[i];
			break;
		}
	}

	for (unsigned i = 0; i < wg->invocation_count; i++) {
		spvm_workgroup_invocation* other = &wg->invocations[i];
		if (other == reference)
			continue;

		if (other->wait_round != reference->wait_round) {
			wg_set_error(wg, error, SPVM_WORKGROUP_ERROR_BARRIER_ROUND_MISMATCH,
				(spvm_word)i, other->wait_instruction, other->wait_round,
				reference->wait_instruction,
				"invocation %u: barrier at word %d is in round %d, "
				"expected round %d (barrier word %d)",
				i, other->wait_instruction, other->wait_round,
				reference->wait_round, reference->wait_instruction);
			return SPVM_WORKGROUP_STEP_ERROR;
		}
		if (other->wait_instruction != reference->wait_instruction) {
			wg_set_error(wg, error, SPVM_WORKGROUP_ERROR_BARRIER_SITE_MISMATCH,
				(spvm_word)i, other->wait_instruction, other->wait_round,
				reference->wait_instruction,
				"invocation %u: reached barrier word %d round %d, "
				"but another invocation reached barrier word %d",
				i, other->wait_instruction, other->wait_round,
				reference->wait_instruction);
			return SPVM_WORKGROUP_STEP_ERROR;
		}
		if (other->wait_depth != reference->wait_depth) {
			wg_set_error(wg, error, SPVM_WORKGROUP_ERROR_BARRIER_STACK_MISMATCH,
				(spvm_word)i, other->wait_instruction, other->wait_round,
				reference->wait_instruction,
				"invocation %u: barrier at word %d round %d has call depth %d, "
				"expected %d", i, other->wait_instruction, other->wait_round,
				other->wait_depth, reference->wait_depth);
			return SPVM_WORKGROUP_STEP_ERROR;
		}
		for (spvm_word d = 0; d < other->wait_depth; d++) {
			if (other->wait_stack[d] != reference->wait_stack[d]) {
				wg_set_error(wg, error, SPVM_WORKGROUP_ERROR_BARRIER_STACK_MISMATCH,
					(spvm_word)i, other->wait_instruction, other->wait_round,
					reference->wait_instruction,
					"invocation %u: barrier at word %d round %d was reached "
					"through a different call site", i,
					other->wait_instruction, other->wait_round);
				return SPVM_WORKGROUP_STEP_ERROR;
			}
		}
	}

	for (unsigned i = 0; i < wg->invocation_count; i++)
		if (wg->invocations[i].status == spvm_invocation_waiting)
			wg->invocations[i].status = spvm_invocation_runnable;

	return SPVM_WORKGROUP_STEP_RELEASED;
}

spvm_workgroup_step_result spvm_workgroup_step(spvm_workgroup_t workgroup,
	unsigned invocation, spvm_workgroup_error* error)
{
	if (!workgroup) {
		if (error)
			memset(error, 0, sizeof(*error));
		return SPVM_WORKGROUP_STEP_ERROR;
	}
	if (invocation >= workgroup->invocation_count) {
		wg_set_error(workgroup, error, SPVM_WORKGROUP_ERROR_INVALID_ARGUMENT,
			(spvm_word)invocation, -1, -1, -1,
			"invocation %u is out of range", invocation);
		return SPVM_WORKGROUP_STEP_ERROR;
	}
	if (workgroup->failed) {
		if (error)
			*error = workgroup->last_error;
		return SPVM_WORKGROUP_STEP_ERROR;
	}

	spvm_workgroup_invocation* inv = &workgroup->invocations[invocation];

	if (inv->status == spvm_invocation_finished)
		return SPVM_WORKGROUP_STEP_FINISHED;
	if (inv->status == spvm_invocation_waiting)
		return SPVM_WORKGROUP_STEP_WAITING;

	spvm_state_t state = inv->state;

	/* OpControlBarrier is executed by the workgroup itself so that the
	 * invocation can be parked between the barrier and the next
	 * instruction. */
	if (state->code_current &&
		(spvm_word)(*state->code_current & SpvOpCodeMask) == SpvOpControlBarrier)
		return wg_handle_barrier(workgroup, invocation, error);

	spvm_word instruction = state->code_current ?
		(spvm_word)(state->code_current - workgroup->program->code) : -1;

	spvm_state_step_opcode(state);

	if (state->code_current == NULL) {
		inv->status = spvm_invocation_finished;

		/* Report the position of a barrier the group is stuck at, so the
		 * diagnostic carries the invocation, instruction and round. */
		for (unsigned i = 0; i < workgroup->invocation_count; i++) {
			spvm_workgroup_invocation* other = &workgroup->invocations[i];
			if (other->status != spvm_invocation_waiting)
				continue;

			wg_set_error(workgroup, error, SPVM_WORKGROUP_ERROR_FINISHED_WHILE_WAITING,
				(spvm_word)invocation, other->wait_instruction, other->wait_round,
				other->wait_instruction,
				"invocation %u: finished at word %d while invocation %u is "
				"waiting at barrier word %d round %d",
				invocation, instruction, i, other->wait_instruction,
				other->wait_round);
			return SPVM_WORKGROUP_STEP_ERROR;
		}
		return SPVM_WORKGROUP_STEP_FINISHED;
	}

	return SPVM_WORKGROUP_STEP_OK;
}

spvm_workgroup_result spvm_workgroup_run(spvm_workgroup_t workgroup,
	spvm_workgroup_error* error)
{
	if (!workgroup) {
		if (error)
			memset(error, 0, sizeof(*error));
		return SPVM_WORKGROUP_RESULT_ERROR;
	}
	if (workgroup->failed) {
		if (error)
			*error = workgroup->last_error;
		return SPVM_WORKGROUP_RESULT_ERROR;
	}

	for (;;) {
		int runnable = 0;
		int finished = 0;

		for (unsigned i = 0; i < workgroup->invocation_count; i++) {
			if (workgroup->invocations[i].status == spvm_invocation_runnable)
				runnable = 1;
			else if (workgroup->invocations[i].status == spvm_invocation_finished)
				finished++;
		}

		if (!runnable) {
			if (finished == (int)workgroup->invocation_count)
				return SPVM_WORKGROUP_RESULT_FINISHED;

			/* Every remaining invocation is parked, but they do not all
			 * share one rendezvous. */
			spvm_word wait_instruction = -1;
			spvm_word wait_round = -1;
			for (unsigned i = 0; i < workgroup->invocation_count; i++) {
				if (workgroup->invocations[i].status == spvm_invocation_waiting) {
					wait_instruction = workgroup->invocations[i].wait_instruction;
					wait_round = workgroup->invocations[i].wait_round;
					break;
				}
			}

			wg_set_error(workgroup, error, SPVM_WORKGROUP_ERROR_DEADLOCK,
				-1, wait_instruction, wait_round, -1,
				"the workgroup cannot make progress");
			return SPVM_WORKGROUP_RESULT_ERROR;
		}

		/* One instruction per invocation per pass, in invocation order. */
		for (unsigned i = 0; i < workgroup->invocation_count; i++) {
			spvm_workgroup_step_result result =
				spvm_workgroup_step(workgroup, i, error);
			if (result == SPVM_WORKGROUP_STEP_ERROR)
				return SPVM_WORKGROUP_RESULT_ERROR;
		}
	}
}
