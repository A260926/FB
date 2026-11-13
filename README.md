# SPIRV-VM
**SPIRV-VM** is a virtual machine for executing SPIR-V shaders. It is written in C, has no dependencies & is licensed under MIT license.
Both **HLSL & GLSL shaders** can be compiled to SPIR-V using tools such as [glslangValidator](https://github.com/KhronosGroup/glslang) and [shaderc](https://github.com/google/shaderc) which means
that you can use this library to debug shaders.

## Example
First, create a SPIRV-VM context:
```c
spvm_context_t ctx = spvm_context_initialize();
```

Load your SPIR-V binary file:
```c
size_t spv_length = 0;
spvm_source spv = load_source("shader.spv", &spv_length);
```

Now you can create a SPIR-V program and a state. The program holds general information about 
the SPIR-V file (like generator version, used capabilities, etc...) while the `spvm_state` holds information
about state of program while executing it. The idea behind states is to make it possible, for example,
to create four states for one program and run the same shader simultaneously in 4 different threads (this is not tested though):
```c
spvm_program_t prog = spvm_program_create(ctx, spv, spv_length);
spvm_state_t state = spvm_state_create(prog);
```

You have to add extensions manually (if your program uses it):
```c
spvm_ext_opcode_func* glsl_ext_data = spvm_build_glsl450_ext();
spvm_result_t glsl_std_450 = spvm_state_get_result(state, "GLSL.std.450");
if (glsl_std_450)
	glsl_std_450->extension = glsl_ext_data;
```


Before debugging your shader, you have to initialize global and uniform variables.

To set a single uniform:
```c
float someUniformData[2] = { 0.5f, 0.6f };
spvm_result_t someUniform = spvm_state_get_result(state, "someUniform");
spvm_member_set_value_f(someUniform->members, someUniform->member_count, someUniformData); // vec2
```

But if you are using newer GLSL version, you probably have to deal with interface blocks:
```c
// first get the block
spvm_result_t uBlock = spvm_state_get_result(state, "uBlock");

// then set its members
float timeData = 0.5f;
spvm_member_t uBlock_time = spvm_state_get_object_member(state, uBlock, "time"); // uBlock.time
spvm_member_set_value_f(uBlock_time->members, uBlock_time->member_count, &timeData);
```

To bind textures:
```c
spvm_image noise2D_data;
spvm_image_create(&noise2D_data, image_data, image_width, image_height, 1);
spvm_result_t noise2D = spvm_state_get_result(state, "noise2D");
noise2D->members[0].image_data = &noise2D_data;
```

You can run your shader in two different ways. You can step line by line through the code.
This can be done with the `spvm_state_step_into` function (which executes only one line).
Or you can execute whole shader with a single function call:
```c
spvm_word fnMain = spvm_state_get_result_location(state, "main");
spvm_state_prepare(state, fnMain);
spvm_state_call_function(state);
```

You can then retrieve results:
```c
spvm_result_t outColor = spvm_state_get_result(state, "outColor");
for (int i = 0; i < outColor->member_count; i++)
	printf("%.2f ", outColor->members[i].value.f);
printf("\n");
```

Use `spvm_state_get_local_result()` function if you are stepping through code line by line and want to get local variable's value.
Example:
```c
spvm_result_t a = spvm_state_get_local_result(state, fnMain, "a");
printf("a = %.2f\n", a->members[0].value.f);
```

Don't forget to free the memory:
```c
spvm_state_delete(state);
spvm_program_delete(prog);
free(glsl_ext_data);
free(spv);

spvm_context_deinitialize(ctx);
```

## Workgroup execution for compute shaders

`spvm_state` runs one invocation.  For offline debugging of shaders that use
shared memory and `barrier()`, `spvm/workgroup.h` adds an executor that runs
a whole compute workgroup in lock-step:

```c
#include <spvm/workgroup.h>

spvm_word workgroup_id[3] = { 0, 0, 0 };
spvm_workgroup_error error;
spvm_workgroup_t wg = spvm_workgroup_create(prog, workgroup_id, &error);
if (!wg)
	fprintf(stderr, "%s\n", error.message);

/* Option A: run everything. */
if (spvm_workgroup_run(wg, &error) != SPVM_WORKGROUP_RESULT_FINISHED)
	fprintf(stderr, "%s\n", error.message);

/* Option B: drive individual invocations yourself, one instruction at a
 * time, in any order.  Waiting and finished invocations are no-ops. */
unsigned count = spvm_workgroup_get_invocation_count(wg);
unsigned finished = 0;
while (finished < count) {
	for (unsigned i = 0; i < count; i++) {
		spvm_state_t state = spvm_workgroup_get_state(wg, i);
		int was_live = state->code_current != NULL;
		if (spvm_workgroup_step(wg, i, &error) == SPVM_WORKGROUP_STEP_ERROR) {
			fprintf(stderr, "%s\n", error.message);
			finished = count; /* bail out */
			break;
		}
		if (was_live && state->code_current == NULL)
			finished++;
	}
}

/* Read per-invocation outputs; the names are the shader's global names. */
spvm_state_t first = spvm_workgroup_get_state(wg, 0);
spvm_result_t out = spvm_state_get_result_with_value(first, "out_value");
printf("%u\n", out->members[0].value.u);

spvm_workgroup_delete(wg);
```

The three-dimensional local size is read from the module's numeric
`OpExecutionMode LocalSize`.  Invocations are created in x-fastest order and
their `LocalInvocationId`, `LocalInvocationIndex`, `WorkgroupId` and
`GlobalInvocationId` builtins are filled in from the caller supplied
workgroup coordinates.

Each invocation has its own `Private`, `Function`, `Input` and `Output`
storage and its own call stack.  `Workgroup` storage is allocated once for
the group, zero initialized, and shared: the invocation states point at the
same `spvm_member` tree, so a write made through `OpAccessChain`,
`OpCopyMemory` or a `Workgroup` pointer parameter of a called helper is
visible to every other invocation.  This is alias-correct: writing a nested
struct or array member through an access chain updates the one shared
storage, and no top-level variable copying is involved.  `Workgroup` memory
is freed exactly once when the workgroup is destroyed, so member aliases are
never double freed.

### Supported barriers

```spir-v
%scope = OpConstant %uint 2                  ; Workgroup
%sem   = OpConstant %uint 264                ; AcquireRelease|WorkgroupMemory
         OpControlBarrier %scope %scope %sem
```

An invocation that reaches a supported `OpControlBarrier` stops *before the
instruction following the barrier*.  Its `spvm_state_get_state()` position is
the next instruction; `spvm_workgroup_step()` on a waiting invocation does
not execute anything and reports `SPVM_WORKGROUP_STEP_WAITING` (the
instruction count does not change).  The last arrival releases the whole
group and returns `SPVM_WORKGROUP_STEP_RELEASED`; all released invocations
then continue after the barrier.

A rendezvous succeeds only when every invocation has arrived in the same
dynamic round, at the same static barrier instruction, through the same
sequence of call-stack return addresses.  Rounds are counted per
invocation, so a barrier executed twice in a loop rendezvouses twice, and
the same helper barrier called from two different call sites is not merged
into one meeting.  Mismatches, and a finished invocation while another is
waiting, produce a diagnostic with the invocation number, instruction
position (word offset) and round:

```c
typedef struct spvm_workgroup_error {
	spvm_workgroup_error_code code;
	spvm_word invocation;
	spvm_word instruction; /* word offset */
	spvm_word round;
	spvm_word barrier;     /* word offset of the barrier */
	char message[256];
} spvm_workgroup_error;
```

Hosts may step invocations forwards, backwards or interleaved; a valid race
free program produces the same per-invocation results in every order.
`spvm_workgroup_run()` uses the same stepping rules as
`spvm_workgroup_step()`, interleaving invocations one instruction at a time,
so it does not cheat by running each invocation to completion independently.

Multiple workgroups can be created from one `spvm_program` and advanced
interleaved.  Shared storage and builtins live in the workgroup, not in the
program's global callbacks or `user_data`, so one group can be deleted
without invalidating another.

### Limits

* exactly one `GLCompute` entry point with a numeric `LocalSize`;
* no runtime arrays, images, samplers, derivative instructions, atomics or
  recursive calls (rejected at creation with a clear error);
* barriers must use Workgroup execution and memory scope with
  `AcquireRelease|WorkgroupMemory` semantics, and the scope/semantics
  operands must be constant ids;
* programs are assumed to reach a barrier or return within a finite amount of
  work; arbitrary infinite loops are not detected.

The existing single-state API and the `control_barrier` /
`allocate_workgroup_memory` / `write_workgroup_memory` callbacks keep working
unchanged.

Run the workgroup checks (needs only a C99 compiler and libm) with:
```
sh checks/check-workgroup.sh
```

## How to link
If you are using CMake add these lines to your `CMakeLists.txt` file:
```
add_subdirectory(./path/to/your/SPIRV-VM)
target_include_directories(example PRIVATE ./path/to/your/SPIRV-VM/inc)
```

If you just want to build this project, run these two commands:
```
cmake .
make
```

## Contributors
- nyorain ([Twitter](https://twitter.com/nyorain), [GitHub](https://github.com/nyorain))
- dfranx ([Twitter](https://twitter.com/dfranx_), [GitHub](https://github.com/dfranx))

Contact me on this e-mail address: **dfranx at shadered dot org**

## LICENSE
SPIRV-VM is licensed under MIT license. See [LICENSE](./LICENSE) for more details.